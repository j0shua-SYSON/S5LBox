//
//  S5LBox -- VMFirmwareImporter. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMFirmwareImporter.h"

#import "VMSettings.h"

#import <errno.h>
#import <fcntl.h>
#import <stdatomic.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>
#import <sys/stat.h>
#import <unistd.h>

/* Darwin's PATH_MAX is 1024 and lives behind <sys/syslimits.h>. Spelled out
 * here rather than included, so this file depends on one fewer header and
 * every buffer below is bounded by a constant you can see. */
#define VMFW_PATH_CAP 1024u

// Declared up front so every call below is checked against a prototype.
@interface VMFirmwareImporter ()
- (void)deliverStage:(vm_fw_stage_t)stage
            artefact:(vm_fw_artefact_t)which
                done:(uint64_t)done
               total:(uint64_t)total;
- (void)performImportOfURL:(NSURL *)url scoped:(BOOL)scoped;
- (vm_fw_status_t)runImportOfURL:(NSURL *)url intoReport:(vm_fw_report_t *)report;
- (void)finishWithStatus:(vm_fw_status_t)status
                  report:(const vm_fw_report_t *)report;
@end

/* ------------------------------------------------------------------------ */
/* Run state shared with the C core                                          */
/* ------------------------------------------------------------------------ */
/*
 * Heap-allocated rather than an ivar, and read through C11 atomics, because the
 * core is handed a raw pointer to it and polls it from the import queue while
 * the main queue writes it. An ivar address would be a pointer into an object
 * whose lifetime ARC manages; this is a pointer to memory this class frees in
 * exactly one place.
 */
typedef struct {
    atomic_bool cancel;
    atomic_bool running;
} vmfw_run_state_t;

static bool vmfw_cancel_cb(void *ctx) {
    vmfw_run_state_t *state = (vmfw_run_state_t *)ctx;
    return state ? atomic_load(&state->cancel) : false;
}

/* ------------------------------------------------------------------------ */
/* Reading the IPSW                                                          */
/* ------------------------------------------------------------------------ */
/*
 * pread(2) rather than a FILE*: it carries its own offset, so nothing here
 * depends on a shared file position, and the archive is 400-odd MB that must
 * never be held in memory.
 */
typedef struct { int fd; } vmfw_ipsw_t;

static size_t vmfw_ipsw_pread(void *ctx, uint64_t offset,
                              uint8_t *buf, size_t len) {
    vmfw_ipsw_t *src = (vmfw_ipsw_t *)ctx;
    if (!src || src->fd < 0 || !buf || len == 0) return 0;

    /* off_t is signed 64-bit here. An offset or an end past its range is a
     * damaged central directory, not something to truncate quietly. */
    if (offset > (uint64_t)INT64_MAX) return 0;
    if ((uint64_t)len > (uint64_t)INT64_MAX - offset) return 0;

    size_t done = 0;
    while (done < len) {
        ssize_t got = pread(src->fd, buf + done, len - done,
                            (off_t)(offset + (uint64_t)done));
        if (got < 0) {
            if (errno == EINTR) continue;
            break;                 /* a short count is a read error to the core */
        }
        if (got == 0) break;       /* end of file */
        done += (size_t)got;
    }
    return done;
}

/* ------------------------------------------------------------------------ */
/* Where the results go                                                      */
/* ------------------------------------------------------------------------ */
/*
 * Two directories, chosen by the name the core asks for:
 *
 *   "kernel.macho", "devicetree.bin", "rootfs.img"  -> the firmware directory
 *   anything ending in ".part"                      -> NSTemporaryDirectory()
 *
 * The ".part" file is the one large intermediate -- the root filesystem member
 * as it sits in the archive, before decryption -- and it is deleted on close,
 * because the core always closes it with keep=false. It goes to the temporary
 * directory both because it is not a result and because iOS will reclaim it if
 * the app is killed with a run in flight, which is the only way it can be left
 * behind.
 */
typedef struct {
    char out_dir[VMFW_PATH_CAP];
    char scratch_dir[VMFW_PATH_CAP];

    /*
     * A NULL from `open` means "produce nothing" to the core, which reports it
     * as VM_FW_STATE_FOUND -- correct for a caller that only wanted the archive
     * identified, and misleading for one that ran out of disk. So a real
     * failure is recorded here and the shell appends it to the report, rather
     * than letting "no destination was opened" stand as the whole story.
     */
    bool open_failed;
    char open_error[192];
} vmfw_file_ctx_t;

typedef struct {
    FILE *fp;
    char  path[VMFW_PATH_CAP];
    off_t write_offset;
    bool  last_was_write;
} vmfw_out_file_t;

static bool vmfw_name_is_safe(const char *name) {
    if (!name || !name[0]) return false;
    if (name[0] == '.') return false;              /* no dotfiles, no ".." */
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') return false; /* no traversal, no subdirs */
    return true;
}

static bool vmfw_name_is_scratch(const char *name) {
    const size_t n = strlen(name);
    return n > 5 && strcmp(name + n - 5, ".part") == 0;
}

static void vmfw_note_open_failure(vmfw_file_ctx_t *ctx, const char *name,
                                   const char *why) {
    if (!ctx || ctx->open_failed) return;          /* keep the first one */
    ctx->open_failed = true;
    snprintf(ctx->open_error, sizeof ctx->open_error, "%s: %s",
             name ? name : "(unnamed)", why ? why : "unknown reason");
}

static void *vmfw_files_open(void *ctx, const char *name) {
    vmfw_file_ctx_t *fc = (vmfw_file_ctx_t *)ctx;
    if (!fc) return NULL;
    if (!vmfw_name_is_safe(name)) {
        vmfw_note_open_failure(fc, name, "not a plain file name");
        return NULL;
    }

    const char *base = vmfw_name_is_scratch(name) ? fc->scratch_dir
                                                  : fc->out_dir;
    if (!base[0]) {
        vmfw_note_open_failure(fc, name, "no directory to write into");
        return NULL;
    }

    vmfw_out_file_t *file = (vmfw_out_file_t *)calloc(1, sizeof *file);
    if (!file) {
        vmfw_note_open_failure(fc, name, "out of memory");
        return NULL;
    }

    const int wrote = snprintf(file->path, sizeof file->path, "%s/%s",
                               base, name);
    if (wrote < 0 || (size_t)wrote >= sizeof file->path) {
        free(file);
        vmfw_note_open_failure(fc, name, "the path would be too long");
        return NULL;
    }

    /* "w+b": truncate anything already there, and stay readable. The core
     * reads the ".part" file back through `pread` after writing it, which a
     * write-only stream could not serve. */
    file->fp = fopen(file->path, "w+b");
    if (!file->fp) {
        vmfw_note_open_failure(fc, name, strerror(errno));
        free(file);
        return NULL;
    }
    file->write_offset = 0;
    file->last_was_write = true;   /* a fresh w+b stream is at 0, for writing */
    return file;
}

static bool vmfw_files_write(void *ctx, void *handle,
                             const uint8_t *data, size_t len) {
    (void)ctx;
    vmfw_out_file_t *file = (vmfw_out_file_t *)handle;
    if (!file || !file->fp) return false;
    if (len == 0) return true;
    if (!data) return false;

    /*
     * C forbids a read followed directly by a write on the same stream without
     * an intervening file-positioning call. The core does read the scratch file
     * back, so the position is restored whenever the last operation was not a
     * write -- and a run of sequential writes seeks not at all, which is what
     * keeps 433 MB from turning into one lseek per 8 KB.
     */
    if (!file->last_was_write) {
        if (fseeko(file->fp, file->write_offset, SEEK_SET) != 0) return false;
        file->last_was_write = true;
    }
    if (fwrite(data, 1, len, file->fp) != len) {
        /* The stream is now somewhere between write_offset and the end of what
         * fwrite managed. The core aborts on a false return, but if it ever
         * stopped doing so, the next write must re-seek rather than trust a
         * position nobody knows. */
        file->last_was_write = false;
        return false;
    }
    file->write_offset += (off_t)len;
    return true;
}

static size_t vmfw_files_pread(void *ctx, void *handle, uint64_t offset,
                               uint8_t *buf, size_t len) {
    (void)ctx;
    vmfw_out_file_t *file = (vmfw_out_file_t *)handle;
    if (!file || !file->fp || !buf || len == 0) return 0;
    if (offset > (uint64_t)INT64_MAX) return 0;

    /* Cleared before the seek, not after: a seek that fails leaves the position
     * unspecified, and the next write must not assume it is still where the
     * last one left it. */
    file->last_was_write = false;
    if (fseeko(file->fp, (off_t)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, len, file->fp);
}

static void vmfw_files_close(void *ctx, void *handle, bool keep) {
    (void)ctx;
    vmfw_out_file_t *file = (vmfw_out_file_t *)handle;
    if (!file) return;

    if (file->fp) {
        fclose(file->fp);
        file->fp = NULL;
    }
    /*
     * keep=false is the core saying this file is an intermediate, or that
     * production of a real artefact failed partway. Either way it must not
     * survive: a half-written rootfs.img in the firmware directory is worse
     * than none, because the emulator's own size-and-hash gate would reject it
     * only after the user had trusted it.
     */
    if (!keep && file->path[0]) (void)remove(file->path);
    free(file);
}

/* ------------------------------------------------------------------------ */
/* Progress, back across the bridge                                          */
/* ------------------------------------------------------------------------ */
/*
 * The context is a bridged, unretained pointer to the importer. That is only
 * sound because a run in flight holds a strong reference to it (see
 * -importIPSWAtURL:), so the object cannot go away while the core is calling.
 */
static void vmfw_progress_cb(void *ctx, vm_fw_artefact_t which,
                             vm_fw_stage_t stage,
                             uint64_t done, uint64_t total) {
    VMFirmwareImporter *importer = (__bridge VMFirmwareImporter *)ctx;
    [importer deliverStage:stage artefact:which done:done total:total];
}

/* ------------------------------------------------------------------------ */
/* Failing before the core is reached                                        */
/* ------------------------------------------------------------------------ */
/*
 * Shaped exactly like the core's own early failures -- a status, one sentence,
 * and three artefacts still at NOT_STARTED -- so the screen has one kind of
 * result to render rather than two.
 */
static vm_fw_status_t vmfw_fail(vm_fw_report_t *report, vm_fw_status_t status,
                                const char *detail) {
    if (!report) return status;
    memset(report, 0, sizeof *report);
    report->status = status;
    for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++)
        report->artefacts[i].state = VM_FW_STATE_NOT_STARTED;
    snprintf(report->detail, sizeof report->detail, "%s",
             detail ? detail : vm_fw_strerror(status));
    return status;
}

static void vmfw_strip_trailing_slash(char *path) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') path[--n] = '\0';
}

@implementation VMFirmwareImporter {
    dispatch_queue_t  _queue;
    vmfw_run_state_t *_state;

    /* Guards _keys against a setter on the main queue racing the copy the
     * import queue takes at the start of a run. */
    NSLock      *_keysLock;
    vm_fw_keys_t _keys;
}

#pragma mark - Lifecycle

+ (instancetype)sharedImporter {
    static VMFirmwareImporter *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [[VMFirmwareImporter alloc] init]; });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;

    _state = (vmfw_run_state_t *)calloc(1, sizeof *_state);
    if (!_state) return nil;
    atomic_init(&_state->cancel, false);
    atomic_init(&_state->running, false);

    _keysLock = [[NSLock alloc] init];
    vm_fw_keys_clear(&_keys);

    /* Utility, not user-initiated: the user is watching a bar, but a 433 MB
     * root filesystem is minutes of work, and that is the class Apple names
     * for progress-visible work of that length. */
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0);
    _queue = dispatch_queue_create("com.j0shua.S5LBox.firmware-import", attr);
    return self;
}

- (void)dealloc {
    /*
     * Reaching here means no run is in flight: a run holds a strong reference
     * to this object for its whole duration, so the C core cannot still be
     * holding _state or reading _keys.
     *
     * vm_fw_keys_clear lives in another translation unit, so the overwrite
     * cannot be optimised away as a store to memory about to be freed.
     */
    vm_fw_keys_clear(&_keys);
    free(_state);
    _state = NULL;
}

#pragma mark - Running

- (BOOL)isRunning {
    if (!_state) return NO;
    return atomic_load(&_state->running) ? YES : NO;
}

- (void)cancelImport {
    if (_state) atomic_store(&_state->cancel, true);
}

- (void)importIPSWAtURL:(NSURL *)url {
    if (!url || !_state) return;

    /* Test-and-set rather than a read then a write, so two taps landing in the
     * same turn of the run loop cannot both get through. */
    bool idle = false;
    if (!atomic_compare_exchange_strong(&_state->running, &idle, true)) return;
    atomic_store(&_state->cancel, false);

    /*
     * In open mode the picker hands back a security-scoped URL. Access is taken
     * here, in the same turn of the run loop as the picker's callback, and
     * released exactly once at the end of -performImportOfURL:scoped:.
     *
     * NO is not necessarily a refusal: a URL that is not security-scoped at all
     * returns NO too. So it is remembered, the open is attempted anyway, and if
     * access really was denied then open(2) fails and the report says which
     * file and why. What must not happen is a stop without a start, which is
     * why the answer is carried rather than assumed.
     */
    const BOOL scoped = [url startAccessingSecurityScopedResource];

    /*
     * Deliberately strong, and the reason the two raw pointers handed to the C
     * core are safe: the core gets (__bridge void *)self as its progress
     * context and _state as its cancel context, and neither may outlive this
     * object -- so the run keeps the object alive instead of tolerating its
     * death. The block is owned by the queue and released when it returns, so
     * this is a cycle that ends by itself.
     */
    VMFirmwareImporter *keptAlive = self;
    dispatch_async(_queue, ^{
        [keptAlive performImportOfURL:url scoped:scoped];
    });
}

/* The one place the security scope is released, and it has no early returns
 * above that line -- which is the whole reason the work is one method deeper. */
- (void)performImportOfURL:(NSURL *)url scoped:(BOOL)scoped {
    vm_fw_report_t report;
    memset(&report, 0, sizeof report);

    const vm_fw_status_t status = [self runImportOfURL:url intoReport:&report];

    if (scoped) [url stopAccessingSecurityScopedResource];

    /* The report is a plain C struct: captured by value, so the block owns the
     * only copy the main queue ever sees, and &copy inside the block is a
     * pointer to storage that lives exactly as long as the call. */
    const vm_fw_report_t copy = report;
    VMFirmwareImporter *keptAlive = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [keptAlive finishWithStatus:status report:&copy];
    });
}

- (vm_fw_status_t)runImportOfURL:(NSURL *)url intoReport:(vm_fw_report_t *)report {
    if (!url.isFileURL)
        return vmfw_fail(report, VM_FW_ERR_ARCHIVE_UNREADABLE,
                         "That is not a file S5LBox can open.");

    /* Where the three results go. Created if absent: on a fresh install nobody
     * has put anything in Documents/firmware, so it does not exist yet. */
    NSString *outDir = [[VMSettings sharedSettings] firmwareDirectory];
    if (outDir.length == 0)
        return vmfw_fail(report, VM_FW_ERR_OUTPUT_REFUSED,
                         "This app has no documents directory to write into.");

    NSError *error = nil;
    if (![[NSFileManager defaultManager] createDirectoryAtPath:outDir
                                   withIntermediateDirectories:YES
                                                    attributes:nil
                                                         error:&error]) {
        char detail[VM_FW_DETAIL_LEN];
        snprintf(detail, sizeof detail,
                 "The firmware directory could not be created: %s",
                 error.localizedDescription.UTF8String ?: "unknown reason");
        return vmfw_fail(report, VM_FW_ERR_OUTPUT_REFUSED, detail);
    }

    vmfw_file_ctx_t files_ctx;
    memset(&files_ctx, 0, sizeof files_ctx);
    if (![outDir getFileSystemRepresentation:files_ctx.out_dir
                                   maxLength:sizeof files_ctx.out_dir])
        return vmfw_fail(report, VM_FW_ERR_OUTPUT_REFUSED,
                         "The firmware directory's path is too long to use.");

    NSString *scratchDir = NSTemporaryDirectory();
    if (scratchDir.length == 0 ||
        ![scratchDir getFileSystemRepresentation:files_ctx.scratch_dir
                                       maxLength:sizeof files_ctx.scratch_dir])
        return vmfw_fail(report, VM_FW_ERR_SCRATCH_REFUSED,
                         "There is no temporary directory to unpack through.");

    /* NSTemporaryDirectory() ends in a slash; the paths below join with one. */
    vmfw_strip_trailing_slash(files_ctx.out_dir);
    vmfw_strip_trailing_slash(files_ctx.scratch_dir);

    char ipswPath[VMFW_PATH_CAP];
    NSString *path = url.path;
    if (path.length == 0 ||
        ![path getFileSystemRepresentation:ipswPath maxLength:sizeof ipswPath])
        return vmfw_fail(report, VM_FW_ERR_ARCHIVE_UNREADABLE,
                         "That file's path could not be resolved.");

    vmfw_ipsw_t source;
    source.fd = open(ipswPath, O_RDONLY);
    if (source.fd < 0) {
        char detail[VM_FW_DETAIL_LEN];
        snprintf(detail, sizeof detail,
                 "The file you chose could not be opened (%s). If you picked it "
                 "a while ago, choose it again.", strerror(errno));
        return vmfw_fail(report, VM_FW_ERR_ARCHIVE_UNREADABLE, detail);
    }

    struct stat info;
    if (fstat(source.fd, &info) != 0 || info.st_size <= 0) {
        close(source.fd);
        return vmfw_fail(report, VM_FW_ERR_ARCHIVE_UNREADABLE,
                         "That file is empty, or its size could not be read.");
    }

    /* One copy of the keys, taken under the lock, used for this run only, and
     * overwritten before this method returns. */
    vm_fw_keys_t keys;
    [_keysLock lock];
    keys = _keys;
    [_keysLock unlock];

    vm_fw_files_t files;
    files.open  = vmfw_files_open;
    files.write = vmfw_files_write;
    files.pread = vmfw_files_pread;
    files.close = vmfw_files_close;
    files.ctx   = &files_ctx;

    vm_fw_import_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.pread        = vmfw_ipsw_pread;
    cfg.pread_ctx    = &source;
    cfg.size         = (uint64_t)info.st_size;
    cfg.files        = &files;
    cfg.keys         = &keys;
    cfg.progress     = vmfw_progress_cb;
    cfg.progress_ctx = (__bridge void *)self;
    cfg.cancel       = vmfw_cancel_cb;
    cfg.cancel_ctx   = _state;

    const vm_fw_status_t status = vm_fw_import_run(&cfg, report);

    vm_fw_keys_clear(&keys);
    close(source.fd);

    /* A destination that could not be created reads, in the core's report, as
     * "no destination was opened for it" -- which is what a caller asking for
     * identification only would see. Say which it really was. */
    if (report && files_ctx.open_failed) {
        char merged[VM_FW_DETAIL_LEN];
        snprintf(merged, sizeof merged, "%s%sA file could not be created: %s",
                 report->detail, report->detail[0] ? "  " : "",
                 files_ctx.open_error);
        memcpy(report->detail, merged, sizeof merged);
    }

    return status;
}

#pragma mark - Back on the main queue

- (void)deliverStage:(vm_fw_stage_t)stage
            artefact:(vm_fw_artefact_t)which
                done:(uint64_t)done
               total:(uint64_t)total {
    const double fraction = (total > 0) ? ((double)done / (double)total) : -1.0;

    VMFirmwareImporter *keptAlive = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        id<VMFirmwareImporterDelegate> delegate = keptAlive.delegate;
        if ([delegate respondsToSelector:
                @selector(importer:didReachStage:forArtefact:fraction:)])
            [delegate importer:keptAlive
                 didReachStage:stage
                   forArtefact:which
                      fraction:fraction];
    });
}

- (void)finishWithStatus:(vm_fw_status_t)status
                  report:(const vm_fw_report_t *)report {
    /* Cleared here rather than on the import queue, so -isRunning stays YES
     * until the delegate has actually been told the run is over. */
    if (_state) atomic_store(&_state->running, false);

    id<VMFirmwareImporterDelegate> delegate = self.delegate;
    if (report &&
        [delegate respondsToSelector:@selector(importer:didFinishWithStatus:report:)])
        [delegate importer:self didFinishWithStatus:status report:report];
}

#pragma mark - Keys the user supplies

- (vm_fw_status_t)setKeyHex:(NSString *)keyHex
                      ivHex:(NSString *)ivHex
                forArtefact:(vm_fw_artefact_t)which {
    /* The root filesystem's key is one combined string and has its own setter;
     * the C parser refuses it here too, but saying so at this end means the
     * caller never sees "invalid import argument" for a routing mistake. */
    if (which != VM_FW_KERNEL && which != VM_FW_DEVICE_TREE)
        return VM_FW_ERR_INVALID_ARGUMENT;

    const char *key = keyHex.UTF8String;
    const char *iv  = ivHex.UTF8String;
    if (!key || !iv) return VM_FW_ERR_INVALID_ARGUMENT;

    [_keysLock lock];
    const vm_fw_status_t status = vm_fw_keys_set_img3(&_keys, which, key, iv);
    [_keysLock unlock];
    return status;
}

- (vm_fw_status_t)setRootFilesystemKeyHex:(NSString *)keyHex {
    const char *key = keyHex.UTF8String;
    if (!key) return VM_FW_ERR_INVALID_ARGUMENT;

    [_keysLock lock];
    const vm_fw_status_t status = vm_fw_keys_set_root(&_keys, key);
    [_keysLock unlock];
    return status;
}

- (BOOL)haveKeyForArtefact:(vm_fw_artefact_t)which {
    BOOL have = NO;
    [_keysLock lock];
    switch (which) {
        case VM_FW_KERNEL:          have = _keys.kernel.present ? YES : NO; break;
        case VM_FW_DEVICE_TREE:     have = _keys.device_tree.present ? YES : NO; break;
        case VM_FW_ROOT_FILESYSTEM: have = _keys.root_present ? YES : NO; break;
        default:                    have = NO; break;
    }
    [_keysLock unlock];
    return have;
}

- (void)forgetKeys {
    [_keysLock lock];
    vm_fw_keys_clear(&_keys);
    [_keysLock unlock];
}

#pragma mark - Reports

+ (NSString *)renderReport:(const vm_fw_report_t *)report {
    if (!report) return nil;

    /* Two passes: the first with no buffer, to be told the exact length. */
    size_t needed = vm_fw_report_render(report, NULL, 0);
    if (needed == 0) return nil;
    if (needed > (1u << 20)) needed = (1u << 20);   /* a report is ~2 KB */

    /* NSMutableData rather than malloc, so the buffer is owned by ARC and
     * cannot be leaked by an early return added later. */
    NSMutableData *buffer = [NSMutableData dataWithLength:needed + 1];
    char *out = (char *)buffer.mutableBytes;
    if (!out) return nil;

    (void)vm_fw_report_render(report, out, needed + 1);

    /* Member names come out of a zip directory somebody else wrote, so the
     * rendered text is not guaranteed to be UTF-8. Losing the whole report to
     * one bad byte in one file name would be the wrong trade. */
    NSString *text = [NSString stringWithUTF8String:out];
    if (!text)
        text = [[NSString alloc] initWithBytes:out
                                        length:strlen(out)
                                      encoding:NSISOLatin1StringEncoding];
    return text;
}

@end
