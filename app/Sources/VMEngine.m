//
//  S5LBox — the emulator run loop. See VMEngine.h.
//
//  THREADING
//
//  One thread owns the machine for its whole lifetime: it allocates it, steps
//  it, and frees it. Nothing else calls into core/ at all. Every few tens of
//  milliseconds that thread takes a mutex and publishes a snapshot — a copy of
//  the guest's framebuffer, whatever the guest printed to the UART, and a
//  couple of counters. The UI takes the same mutex and copies the snapshot out.
//  The lock is therefore held only for two memcpys of a 600 KB buffer, never
//  across interpretation, so the main thread cannot be blocked behind guest
//  execution no matter how slow the guest is.
//
//  MEMORY
//
//  Guest DRAM is 128 MB, matching the hardware. On a 2 GB device that sounds
//  alarming and is not, for one specific reason: core's s5l8900_init() gets it
//  from calloc(), and a request that size goes straight to mmap'd anonymous
//  memory that the kernel fills with zeroes lazily, one page at a time, on
//  first touch. Untouched guest RAM is address space, not footprint. This demo
//  guest touches its one code page and the 600 KB framebuffer, so the resident
//  cost is well under a megabyte of the 128.
//
//  That is a claim about the allocator, not a measurement, so the app measures
//  it: physFootprintBytes reads phys_footprint from TASK_VM_INFO, which is the
//  exact counter jetsam compares against the per-process limit, and the app
//  prints the before/after delta on screen. Do not trust the paragraph above
//  over the number on the phone.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMEngine.h"
#import "VMTouchQueue.h"
#import "VMButtonQueue.h"
#import <dispatch/dispatch.h>
#import "VMFirmwareBoot.h"
#import "VMInstancePaths.h"
#import "VMInstanceStore.h"
#import "VMSettings.h"

#import <mach/mach.h>
#import <pthread.h>
#import <dlfcn.h>
#import <time.h>
#import <unistd.h>
#import <stdio.h>
#import <stdlib.h>
#import <string.h>

// How many instructions to interpret between checks of the stop/pause flags.
// At a few million instructions a second this is roughly 15-30 ms, which keeps
// pause latency short without paying the flag check too often.
static const unsigned kVMChunkInstructions = 100000;

// Publish a snapshot at most this often. The UI redraws at 30 Hz; going faster
// would only copy the same pixels twice.
static const double kVMPublishInterval = 1.0 / 30.0;

/* TCP payload goodput, not UART framing or host socket bytes. A one-second
 * window rejects the short buffering spikes that made earlier network claims
 * look much better than sustained guest downloads actually were. */
static const double kVMNetworkRateInterval = 1.0;

// Cap on UART bytes held for the UI. The view controller drains this every
// frame and keeps the real scrollback; this bound only matters if the UI
// stops draining (backgrounded, say) while the guest keeps printing.
static const NSUInteger kVMConsoleLimit = 16000;

/*
 * Automatic resume must never serialize a UIKit press whose matching release
 * lives only in a host queue. Give the guest far more than the known physical
 * floors (0.5 s and 8 M retirements for Power) to consume the cancellation,
 * but fail the save instead of displaying "Saving" forever if its input driver
 * is genuinely wedged. Either independent bound may stop the attempt; the
 * retirement bound is the fallback if the monotonic host clock is unavailable.
 */
static const uint64_t kVMCheckpointInputTimeoutNS = UINT64_C(10000000000);
static const uint64_t kVMCheckpointInputTimeoutInstructions = UINT64_C(128000000);

/*
 * THE ONE PLACE THE APP'S TWO BUTTON ENUMS MEET.
 *
 * VMButton (VMEngine.h) is Objective-C and needs Foundation; VM_BUTTON_*
 * (VMButtonQueue.h) is plain C so a host CI runner can test the mapping table
 * without an Apple toolchain. They are two spellings of one order and they must
 * agree by value, or -setButton:pressed:'s cast quietly sends the wrong key.
 * Neither header can check that on its own. This file can, and does, here —
 * on the macOS build, which is the only build where both exist.
 */
_Static_assert((NSUInteger)VMButtonHome         == VM_BUTTON_HOME,          "VMButton order");
_Static_assert((NSUInteger)VMButtonPower        == VM_BUTTON_POWER,         "VMButton order");
_Static_assert((NSUInteger)VMButtonVolumeUp     == VM_BUTTON_VOLUME_UP,     "VMButton order");
_Static_assert((NSUInteger)VMButtonVolumeDown   == VM_BUTTON_VOLUME_DOWN,   "VMButton order");
_Static_assert((NSUInteger)VMButtonRingerSilent == VM_BUTTON_RINGER_SILENT, "VMButton order");
_Static_assert((NSUInteger)VMButtonCount        == VM_BUTTON_COUNT,         "VMButton count");

typedef NS_ENUM(uint8_t, VMEngineState) {
    VMEngineStateIdle = 0,
    VMEngineStateStarting,
    VMEngineStateRunning,
    VMEngineStateCheckpointing,
    VMEngineStateStopping,
};

static double vm_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint64_t vm_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0 ||
        (uint64_t)ts.tv_sec > UINT64_MAX / UINT64_C(1000000000))
        return 0u;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) +
           (uint64_t)ts.tv_nsec;
}

// Declared up front so every call below is checked against a prototype.
@interface VMEngine ()
- (void)threadMain:(id)unused;
- (void)publishRetired:(uint64_t)retired
                  rate:(double)instantRate
                status:(arm_status_t)status;
- (void)appendConsole:(NSString *)text;
- (void)exportGuestPCProfile_emulatorThread;
- (void)exportNativePCProfile_emulatorThread;
- (void)noteDiscardedInput;
- (void)noteDroppedTouch;
- (void)noteDroppedButton;
- (void)drainOneTouch_emulatorThread;
- (void)drainOneButton_emulatorThread;
- (void)beginCheckpointInputQuiesce_emulatorThread;
- (BOOL)checkpointInputIsQuiescent_emulatorThread;
- (void)publishBlankSnapshotLocked;
- (BOOL)installGuestPayload;
- (void)provisionRootFilesystem:(id)unused;
- (BOOL)resolveFilesInto:(vm_instance_paths_t *)paths note:(NSString **)note;
- (NSUInteger)copyOptionValuesInto:(bool *)values capacity:(NSUInteger)capacity;
@end

/*
 * A sampled signature of the frame, not a complete hash.
 *
 * Hashing all 460,800 bytes at up to 60 Hz would be ~27 MB/s of pure
 * measurement overhead on the very phone whose speed is in question, which
 * would make the counter change the number it reports. The 397-byte stride is
 * coprime with the 1,280-byte row pitch, so successive samples walk across
 * rows rather than re-reading one column of every row.
 *
 * Sampling can only ever MISS a change, never invent one, so this undercounts
 * frames that differ in fewer than ~1,160 sampled words -- a cursor-sized
 * change on an otherwise still screen. It is accurate for the animations the
 * 30fps target is about, and the direction of its error is known.
 */
static uint64_t vm_engine_fb_signature(const uint8_t *fb, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i + 4u <= n; i += 397u) {
        uint32_t w = (uint32_t)fb[i] | ((uint32_t)fb[i + 1] << 8) |
                     ((uint32_t)fb[i + 2] << 16) | ((uint32_t)fb[i + 3] << 24);
        h = (h ^ (uint64_t)w) * 1099511628211ull;
    }
    return h;
}

static double vm_engine_now_seconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

@implementation VMEngine {
    s5l8900_t        _machine;
    /* The provisioning copy's byte counters; see -rootFilesystemProgress. */
    uint64_t         _prepareDone;
    uint64_t         _prepareTotal;
    BOOL             _machineReady;

    /*
     * WHICH GUEST IS RUNNING, and why it is not the other one.
     *
     * The app has two payloads now: Apple's kernel, and the synthetic guest in
     * VMGuest.c. Which one it got is the first thing a user needs to know and
     * the easiest thing for a UI to be wrong about, so it is recorded here as
     * a string the engine sets from what actually happened rather than as a
     * flag the UI infers. -modeDescription is the only way to read it, and
     * there is no path that sets it to a firmware string without
     * vm_firmware_boot_start() having returned true.
     *
     * _bringUpNote carries the reason the firmware path was NOT taken, when
     * there was firmware to take it with. Empty means "nothing to explain".
     */
    vm_firmware_boot_t *_firmwareBoot;   // owns the work image + bridge storage
    NSString        *_mode;
    NSString        *_bringUpNote;
    BOOL             _preparingRootFS;
    /*
     * WHICH MACHINE THIS ENGINE IS. Set once at construction and never
     * changed: it names the directory this machine's writable root filesystem
     * lives in, and a machine that changed identity mid-run would be a machine
     * that changed disks mid-run. nil means nobody said, which is a
     * configuration this class refuses to guess about -- see -resolveFilesInto:.
     */
    NSString        *_instanceID;

    NSThread        *_thread;
    pthread_mutex_t  _lock;
    BOOL             _lockReady;

    // Everything below is guarded by _lock.
    uint8_t         *_snapshot;      // VM_FB_BYTES, last published frame
    BOOL             _snapshotFresh;
    BOOL             _snapshotARGB;  // byte order of the snapshot's pixels
    BOOL             _snapshotBlank;
    /* Frame rate, measured rather than assumed.
     *
     * The goal for this project is "30fps+", and until now the app could not
     * report it: the status line showed M insn/s, which is not a frame rate
     * and cannot be converted into one without knowing the per-frame cost.
     * Every fps figure quoted so far has been extrapolated on a desktop.
     *
     * What is counted is a CHANGED published frame, not a publish and not a
     * guest composite. That is deliberately the user-visible quantity: this
     * guest composites into a continuously scanned surface and never writes
     * CLCD_UPDATE, so there is no register edge to count, and publishing an
     * identical frame is not a frame anyone can see.
     */
    uint64_t         _fbSignature;
    BOOL             _fbSignatureValid;
    uint64_t         _fpsWindowFrames;
    double           _fpsWindowStart;
    double           _fps;
    /* The geometry the display controller was scanning out when the snapshot
     * was taken. Published with the pixels rather than assumed by the reader:
     * the buffer is a fixed VM_FB_BYTES, but what is IN it is whatever window
     * the guest enabled. */
    uint32_t         _snapshotWidth;
    uint32_t         _snapshotHeight;
    uint32_t         _snapshotStride;
    NSMutableString *_pending;
    uint64_t         _retired;
    double           _rate;          // instructions per second, smoothed
    /* Live PPP/NAT payload counters, sampled on the emulator thread and
     * published under this lock. Rates are bytes per second. */
    BOOL             _networkStatusValid;
    vm_network_status_t _networkStatus;
    uint64_t         _networkDownBaseline;
    uint64_t         _networkUpBaseline;
    double           _networkWindowStart;
    double           _networkDownRate;
    double           _networkUpRate;
    NSString        *_status;
    VMEngineState    _state;
    BOOL             _stopRequested;
    VMEngineStopCompletion _stopCompletion;
    BOOL             _checkpointRequested;
    VMEngineCheckpointCompletion _checkpointCompletion;
    BOOL             _paused;
    NSString        *_pauseReason;
    uint64_t         _instructionCap; // 0: run until stopped or halted
    BOOL             _buttons[VMButtonCount];
    BOOL             _discardedInputLogged;
    BOOL             _droppedTouchLogged;
    /*
     * Touch reports waiting to be handed to the Z2.
     *
     * The UI thread must not touch the machine — it runs at whatever rate
     * UIKit delivers events, and s5l8900_run() is executing on the emulator
     * thread the whole time. So a report is enqueued here under _lock and
     * applied between chunks by threadMain, which is the only place the
     * machine is ever poked.
     *
     * The container and its drop policy live in VMTouchQueue.c, in plain C,
     * because "which report gets thrown away when the queue is full" is the
     * part of this that can be wrong without anything reporting an error —
     * and the part a host CI runner can test.
     */
    vm_touch_queue_t _touch;
    vm_touch_delivery_state_t _touchDelivery;
    uint64_t         _touchDelivered;
    /*
     * Button transitions waiting to be handed to the board, for exactly the
     * same reason and with a stricter rule: nothing here may ever be coalesced
     * away, because a press and its release are two edges and dropping either
     * leaves the guest holding a key nobody is pressing. See VMButtonQueue.h.
     */
    vm_button_queue_t _buttonQueue;
    uint64_t          _buttonDelivered;
    uint64_t          _buttonRefused;
    vm_button_power_hold_t _powerHold;
    vm_button_momentary_holds_t _momentaryHolds;
    BOOL              _droppedButtonLogged;
}

+ (uint64_t)physFootprintBytes {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    kern_return_t kr = task_info(mach_task_self(), TASK_VM_INFO,
                                 (task_info_t)&info, &count);
    if (kr != KERN_SUCCESS) return 0;
    return (uint64_t)info.phys_footprint;
}

- (instancetype)init {
    return [self initWithInstanceID:nil];
}

- (instancetype)initWithInstanceID:(NSString *)identifier {
    self = [super init];
    if (!self) return nil;
    if (pthread_mutex_init(&_lock, NULL) != 0) return nil;
    _lockReady = YES;
    _pending = [NSMutableString string];
    if (!_pending) return nil;
    _status  = @"idle";
    _mode    = @"";
    _bringUpNote = @"";
    _instanceID = [identifier copy];
    return self;
}

- (void)dealloc {
    // Safe without any handshake: NSThread holds a strong reference to its
    // target for as long as the thread is alive, so -dealloc cannot possibly
    // run while -threadMain is still using the snapshot buffer or the lock.
    free(_snapshot);
    if (_lockReady) pthread_mutex_destroy(&_lock);
}

#pragma mark - Choosing a guest

/*
 * THE ONE DECISION: Apple's kernel, or the built-in test guest.
 *
 * Called on the starting thread with the machine already allocated and nothing
 * else touching it. All the real work is in app/Sources/VMFirmwareBoot.c, which
 * is plain C and is tested by core/tests -- this method is only the policy of
 * WHEN to try, and what to say when it does not work.
 *
 * THE RULES IT ENFORCES, in order of how badly getting them wrong would hurt:
 *
 *  1. The firmware path is taken only when vm_firmware_boot_start() returns
 *     true. There is no other assignment of a firmware string to _mode.
 *  2. A firmware failure is NEVER silent. _bringUpNote gets the reason,
 *     -statusLine carries it, and the console gets the full detail. The demo
 *     guest then runs, and _mode says it is the demo guest.
 *  3. A failed bring-up may have written megabytes of kernel into DRAM, so the
 *     machine is torn down and rebuilt before the demo guest is installed
 *     rather than layered on top of a half-loaded kernel.
 *
 * WHERE IT LOOKS. Two directories, not one, and the split is what makes two
 * machines two machines: the three imported artefacts are shared and read-only
 * (Documents/firmware, where VMFirmwareImporter writes), and the writable work
 * image belongs to THIS machine alone (Machines/<id>). VMInstancePaths.c owns
 * that derivation, including what happens to the single shared work image an
 * older build left behind. An engine with no instance id refuses the firmware
 * path outright rather than guessing at a directory -- see -resolveFilesInto:.
 */
- (BOOL)installGuestPayload {
    vm_instance_paths_t paths;
    NSString *note = nil;
    if (![self resolveFilesInto:&paths note:&note]) {
        if (!vm_guest_install(&_machine)) return NO;
        pthread_mutex_lock(&_lock);
        _mode = @"built-in test guest";
        _bringUpNote = note ?: @"";
        pthread_mutex_unlock(&_lock);
        return YES;
    }

    vm_firmware_boot_paths_t boot_paths;
    vm_instance_paths_to_boot(&paths, &boot_paths);

    /*
     * ADOPTION, before the probe rather than after it: a machine that can take
     * over the pre-instance work image IS ready, and probing first would send
     * it down the 450 MB provisioning path with a perfectly good disk sitting
     * unused. The move is a rename, so it costs nothing and cannot run long
     * enough to matter on this thread.
     */
    NSString *adoptionProblem = nil;
    if (vm_instance_work_plan(&paths) == VM_INSTANCE_WORK_ADOPT) {
        char detail[VM_FW_BOOT_DETAIL_CAPACITY];
        BOOL adopted = vm_instance_work_adopt(&paths, detail, sizeof detail);
        NSString *said = [NSString stringWithUTF8String:detail];
        [self appendConsole:[NSString stringWithFormat:@"[vm] %@\n",
            said ?: (adopted ? @"adopted the shared root filesystem"
                             : @"could not adopt the shared root filesystem")]];
        /*
         * A FAILED ADOPTION IS NEVER SILENT, and it is kept separately from
         * `note` because the next thing that happens is the branch below
         * overwriting `note` with "preparing the root filesystem" -- which is
         * true, and which would replace "we could not move the 450 MB disk you
         * already had" with a sentence that sounds like everything is fine.
         */
        if (!adopted) adoptionProblem =
            said.length ? said
                        : @"The root filesystem this machine could have "
                          @"adopted could not be moved.";
    }

    /* The switches, resolved by the same C the tests run. */
    bool values[VM_BOOT_OPTION_MAX];
    NSUInteger count = [self copyOptionValuesInto:values
                                         capacity:VM_BOOT_OPTION_MAX];

    vm_firmware_boot_state_t state;
    vm_firmware_boot_probe(&boot_paths, &state);

    if (state.readiness == VM_FW_BOOT_READY) {
        vm_firmware_boot_destroy(&_firmwareBoot);
        _firmwareBoot = vm_firmware_boot_create();
        if (_firmwareBoot) {
            vm_firmware_boot_report_t report;
            if (vm_firmware_boot_start(_firmwareBoot, &_machine, &boot_paths,
                                       values, (unsigned)count, &report)) {
                [self appendConsole:[NSString stringWithFormat:
                    @"[vm] Apple firmware: kernel at pa 0x%08x, device tree at "
                    @"0x%08x (%u bytes), boot_args at 0x%08x\n"
                    @"[vm] topOfKernelData 0x%08x, /vram pool 0x%08x+0x%x, "
                    @"free page pool %.1f MB\n"
                    @"[vm] boot arguments: \"%s\"\n",
                    report.bringup.entry_pa, report.bringup.devicetree_pa,
                    report.bringup.devicetree_size, report.bringup.boot_args_pa,
                    report.bringup.top_of_kernel_data_pa,
                    report.bringup.framebuffer_pa, report.bringup.vram_bytes,
                    report.bringup.free_pool_bytes / 1048576.0,
                    report.bringup.cmdline]];
                /* -stringWithUTF8String: rather than the @() boxing syntax:
                 * these are fixed char arrays, and the explicit form has no
                 * decay question in it. It returns nil for bytes that are not
                 * valid UTF-8, so neither result is used unguarded. */
                NSString *summary =
                    [NSString stringWithUTF8String:report.summary];
                /*
                 * A SUCCESSFUL BOOT STILL OWES THE USER THIS. Twelve of the
                 * fourteen switches do not reach the request, and six of them
                 * disagree with what the machine did on an installation
                 * nobody has touched. Saying nothing here would be the exact
                 * failure this class was built to end -- the settings screen
                 * showing one configuration and the machine running another.
                 */
                NSString *switches =
                    [NSString stringWithUTF8String:report.options.summary];
                if (switches.length)
                    [self appendConsole:[NSString stringWithFormat:
                        @"[vm] settings: %@\n", switches]];
                /*
                 * WHICH NUBS THIS BOOT HID, by name and from the list bring-up
                 * was actually handed -- not from the settings table, which is
                 * what the user set rather than what the guest got.
                 *
                 * This exists because on 2026-07-29 a phone log could not
                 * answer "is this the build with the touchscreen fix?". The
                 * summary above lists only switches whose outcome DISAGREES
                 * with the request, so a nub working exactly as asked appeared
                 * nowhere at all, and an old binary and a new one printed the
                 * same lines. A boot that hides a device should say which.
                 */
                if (report.options.unmatch_count) {
                    NSMutableString *nubs = [NSMutableString string];
                    for (unsigned i = 0; i < report.options.unmatch_count; i++) {
                        const char *path = report.options.unmatch[i];
                        if (!path) continue;
                        if (nubs.length) [nubs appendString:@", "];
                        [nubs appendString:
                            [NSString stringWithUTF8String:path] ?: @"?"];
                    }
                    [self appendConsole:[NSString stringWithFormat:
                        @"[vm] device tree: %u nub(s) hidden from the guest: "
                        @"%@\n", report.options.unmatch_count, nubs]];
                }
                /* Actionable, not merely alarming: the settings screen is
                 * where each of these now says what happens instead. */
                NSString *said = switches.length
                    ? [switches stringByAppendingString:
                        @" Each one says what the machine does instead, under "
                        @"its own switch in Settings."]
                    : nil;
                pthread_mutex_lock(&_lock);
                _mode = summary ?: @"Apple firmware";
                _bringUpNote = said ?: (note ?: @"");
                pthread_mutex_unlock(&_lock);
                return YES;
            }
            note = [NSString stringWithUTF8String:report.detail]
                 ?: @"Apple's kernel could not be started.";
            [self appendConsole:[NSString stringWithFormat:
                @"[vm] FIRMWARE BOOT FAILED: %s\n"
                @"[vm] falling back to the built-in test guest\n",
                report.detail]];
            vm_firmware_boot_destroy(&_firmwareBoot);
            /* A partially-loaded kernel is not a blank machine. */
            s5l8900_free(&_machine);
            if (!s5l8900_init(&_machine, VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE))
                return NO;
        } else {
            note = @"Not enough memory to start Apple's kernel.";
        }
    } else if (state.readiness == VM_FW_BOOT_NEEDS_WORK_IMAGE) {
        /*
         * The three imported files are here but the writable root filesystem
         * has not been made. That copy is ~450 MB and takes long enough to be
         * killed by the watchdog if it ran here, so it runs on its own thread
         * and this machine gets the demo guest. Reopening the machine after it
         * finishes boots the real kernel.
         */
        /* Says what the bar means and what happens next, and NOTHING MORE.
         * A first draft of this string promised "iPhone OS starts by itself
         * when the copy finishes"; it does not -- the completion path below
         * says "Reopen it to boot iPhone OS" -- and a message that tells a
         * user to wait for something that never happens is worse than the
         * vague one it replaced. */
        note = @"Preparing this machine's writable root filesystem — first "
               @"boot only. The bar on the screen is the real copy progress "
               @"and keeps going if you leave. Reopen this machine when it "
               @"reaches the end.";
        BOOL alreadyRunning;
        pthread_mutex_lock(&_lock);
        alreadyRunning = _preparingRootFS;
        _preparingRootFS = YES;
        pthread_mutex_unlock(&_lock);
        if (!alreadyRunning) {
            /* Two strings rather than the C struct: NSThread wants an object,
             * and the worker rebuilds the struct itself so nothing on the
             * emulator side has to outlive this stack frame.
             * -stringWithUTF8String: rather than @(): these are fixed char
             * arrays and the explicit form has no decay question in it, and it
             * returns nil for bytes that are not valid UTF-8 -- which a
             * dictionary literal would turn into a crash, so it is checked. */
            NSString *firmwareDir =
                [NSString stringWithUTF8String:boot_paths.firmware];
            NSString *workDir = [NSString stringWithUTF8String:boot_paths.work];
            NSDictionary *where = (firmwareDir && workDir)
                ? @{ @"firmware": firmwareDir, @"work": workDir } : nil;
            NSThread *worker = where ? [[NSThread alloc]
                initWithTarget:self
                      selector:@selector(provisionRootFilesystem:)
                        object:where] : nil;
            if (worker) {
                worker.name = @"S5LBox rootfs provisioning";
                worker.qualityOfService = NSQualityOfServiceUtility;
                [worker start];
            } else {
                pthread_mutex_lock(&_lock);
                _preparingRootFS = NO;
                pthread_mutex_unlock(&_lock);
                note = @"Could not start preparing the root filesystem.";
            }
        }
    } else {
        /* No firmware at all is the ordinary case, not a failure: the demo
         * guest is what this app does for anyone who has imported nothing. */
        note = @"";
    }

    if (!vm_guest_install(&_machine)) return NO;

    /* The adoption failure leads, because it is about a file the user already
     * had and everything else here is about one the app is making. */
    if (adoptionProblem.length)
        note = note.length
            ? [adoptionProblem stringByAppendingFormat:@" %@", note]
            : adoptionProblem;

    pthread_mutex_lock(&_lock);
    _mode = @"built-in test guest";
    _bringUpNote = note ?: @"";
    pthread_mutex_unlock(&_lock);
    return YES;
}

/*
 * The one slow step, on its own thread. rootfs_work_create() refuses to
 * replace an existing destination, so a second start while this is running
 * cannot corrupt the image being written -- it is the flag, not the file, that
 * keeps two of these from being launched.
 */
- (void)provisionRootFilesystem:(id)where {
    @autoreleasepool {
        NSDictionary *dirs = (NSDictionary *)where;
        char detail[VM_FW_BOOT_DETAIL_CAPACITY];
        vm_firmware_boot_paths_t paths;
        detail[0] = '\0';

        NSString *firmware = dirs[@"firmware"];
        NSString *work = dirs[@"work"];
        char firmwarePath[VM_FW_BOOT_PATH_CAPACITY];
        char workPath[VM_FW_BOOT_PATH_CAPACITY];
        BOOL havePaths =
            firmware.length && work.length &&
            [firmware getFileSystemRepresentation:firmwarePath
                                        maxLength:sizeof firmwarePath] &&
            [work getFileSystemRepresentation:workPath
                                    maxLength:sizeof workPath] &&
            vm_firmware_boot_paths_split(&paths, firmwarePath, workPath);

        /*
         * The switches are read HERE, not carried from the start, because this
         * is the moment their value is written into the image: the QuartzCore
         * software-renderer rewrite is a property of the file from now on, and
         * a value captured a second earlier would be no more accurate and much
         * easier to get wrong.
         */
        bool values[VM_BOOT_OPTION_MAX];
        NSUInteger count = [self copyOptionValuesInto:values
                                             capacity:VM_BOOT_OPTION_MAX];

        BOOL ok = havePaths
            ? vm_firmware_boot_provision(&paths, values, (unsigned)count,
                                         vm_engine_prepare_progress,
                                         (__bridge void *)self,
                                         detail, sizeof detail)
            : NO;
        if (!havePaths)
            (void)snprintf(detail, sizeof detail,
                           "This app has nowhere to prepare this machine's "
                           "root filesystem.");

        /*
         * A REFUSAL IS NOT ALWAYS A FAILURE. rootfs_work_create() will not
         * replace an existing destination, which is the behaviour that keeps a
         * second run from discarding the guest's writes -- but it also means
         * that a machine which was provisioned by another engine (a Reset
         * relaunches one, and its _preparingRootFS flag is a per-engine ivar)
         * gets told "could not prepare the root filesystem" about a filesystem
         * it now has. Ask the disk rather than the return value: if the work
         * image is there, this machine is ready, whoever made it.
         */
        if (!ok && havePaths) {
            vm_firmware_boot_state_t after;
            vm_firmware_boot_probe(&paths, &after);
            if (after.work_present) {
                ok = YES;
                (void)snprintf(detail, sizeof detail,
                               "The root filesystem was already prepared.");
            }
        }
        NSString *said = [NSString stringWithUTF8String:detail];
        pthread_mutex_lock(&_lock);
        _preparingRootFS = NO;
        _bringUpNote = ok
            ? [NSString stringWithFormat:
                @"This machine's root filesystem is ready. Reopen it to boot "
                @"iPhone OS. %@", said ?: @""]
            : (said ?: @"The root filesystem could not be prepared.");
        pthread_mutex_unlock(&_lock);
        /* Two literal format strings rather than one chosen by a ternary: a
         * non-literal format is unchecked by the compiler, and this one is
         * built from a fixed char array whose contents come from four
         * different refusal paths. */
        [self appendConsole:ok
            ? [NSString stringWithFormat:
                @"[vm] root filesystem prepared; reopen to boot iPhone OS "
                @"(%s)\n", detail]
            : [NSString stringWithFormat:
                @"[vm] could not prepare the root filesystem: %s\n", detail]];
    }
}

/*
 * WHICH FILES THIS MACHINE USES, or why it cannot have any.
 *
 * Everything that can be wrong here -- an identifier that is not sixteen hex
 * digits, a path too long to hold, the pre-instance work image -- is decided
 * in VMInstancePaths.c, which a host runner tests. This method is the two
 * lookups Objective-C is needed for and nothing else.
 *
 * Returns NO, with a sentence in `note`, when the firmware path is not
 * available. That is not always a failure: no instance id is the ordinary case
 * for a machine opened by something other than the machine list, and no
 * firmware imported is the ordinary case full stop.
 */
- (BOOL)resolveFilesInto:(vm_instance_paths_t *)paths note:(NSString **)note {
    if (note) *note = @"";
    if (!paths) return NO;

    NSString *firmware = [[VMSettings sharedSettings] firmwareDirectory];
    if (!firmware.length) {
        if (note) *note = @"This app has no documents directory to look for "
                          @"firmware in.";
        return NO;
    }

    if (!_instanceID.length) {
        /*
         * REFUSED RATHER THAN GUESSED. The alternative -- falling back to a
         * shared work image in the firmware directory -- is exactly the
         * behaviour that made every machine one machine, and it would come
         * back silently the first time something constructed an engine
         * without an identifier. The demo guest runs and says why.
         */
        if (note) *note = @"This machine has no identity, so it has no root "
                          @"filesystem of its own. Open it from the machine "
                          @"list to boot iPhone OS.";
        return NO;
    }

    /*
     * Called for its SIDE EFFECT as much as its value: it CREATES this
     * machine's directory, and C cannot portably mkdir, so both the adoption
     * rename and the provisioner depend on this having happened first.
     *
     * The C then rebuilds the same path from the container and the identifier.
     * That is one derivation stated twice, and it is deliberate: the C one is
     * the one that can be tested, and it is also the one that refuses an
     * identifier that is not sixteen hex digits. They agree because both are
     * <container>/<id> and the identifier contains no separator.
     */
    NSString *mine =
        [[VMInstanceStore sharedStore] directoryForInstanceWithID:_instanceID];
    NSString *machines = [[VMInstanceStore sharedStore] machinesDirectory];
    if (!mine.length) {
        if (note) *note = @"This machine has nowhere on disk to keep its root "
                          @"filesystem.";
        return NO;
    }

    char firmwarePath[VM_FW_BOOT_PATH_CAPACITY];
    char machinesPath[VM_FW_BOOT_PATH_CAPACITY];
    if (![firmware getFileSystemRepresentation:firmwarePath
                                     maxLength:sizeof firmwarePath] ||
        !machines.length ||
        ![machines getFileSystemRepresentation:machinesPath
                                     maxLength:sizeof machinesPath]) {
        if (note) *note = @"This machine's files are somewhere this app cannot "
                          @"name.";
        return NO;
    }

    vm_instance_paths_status_t s =
        vm_instance_paths_derive(firmwarePath, machinesPath,
                                 _instanceID.UTF8String, paths);
    if (s != VM_INSTANCE_PATHS_OK) {
        if (note)
            *note = [NSString stringWithFormat:
                @"This machine's files cannot be located: %s.",
                vm_instance_paths_status_text(s)];
        return NO;
    }
    return YES;
}

/*
 * The settings screen's values, in option-table order.
 *
 * READ FROM VMSettings, which is the store the settings screen writes.
 * VMInstanceStore also carries a per-instance option array, but nothing in the
 * app writes it yet, so reading it here would take the switches back to doing
 * nothing -- this time invisibly. When the settings screen becomes
 * per-machine, this is the one method that changes.
 */
- (NSUInteger)copyOptionValuesInto:(bool *)values capacity:(NSUInteger)capacity {
    if (!values || capacity == 0) return 0;
    NSUInteger count = vm_option_count();
    if (count > capacity) count = capacity;
    VMSettings *settings = [VMSettings sharedSettings];
    for (NSUInteger i = 0; i < count; i++)
        values[i] = [settings valueForOptionIndex:i] ? true : false;
    return count;
}

#pragma mark - Lifecycle

- (BOOL)start {
    /* start/stop are normally called by the main thread, but keeping the state
     * transition under the same lock as the worker flags prevents a retry from
     * racing the final free after a guest halt. */
    pthread_mutex_lock(&_lock);
    if (_state == VMEngineStateRunning) {
        pthread_mutex_unlock(&_lock);
        return YES;
    }
    if (_state != VMEngineStateIdle) {
        pthread_mutex_unlock(&_lock);
        return NO;
    }
    _state = VMEngineStateStarting;
    _stopRequested = NO;
    _stopCompletion = nil;
    _checkpointRequested = NO;
    _checkpointCompletion = nil;
    _paused = NO;
    _pauseReason = nil;
    _snapshotFresh = NO;
    _snapshotBlank = NO;
    _retired = 0;
    _rate = 0.0;
    _networkStatusValid = NO;
    memset(&_networkStatus, 0, sizeof _networkStatus);
    _networkDownBaseline = 0u;
    _networkUpBaseline = 0u;
    _networkWindowStart = 0.0;
    _networkDownRate = 0.0;
    _networkUpRate = 0.0;
    /* A restart must not inherit the previous machine's pending finger, nor
     * its counters — the status line reads them as claims about THIS run. */
    vm_touch_queue_reset(&_touch);
    vm_touch_delivery_reset(&_touchDelivery);
    _touchDelivered = 0;
    _droppedTouchLogged = NO;
    /* And the buttons, for the same reason: a switch held when the last
     * machine went away is not held on this one, whose board resets with
     * every switch released. */
    vm_button_queue_reset(&_buttonQueue);
    memset(_buttons, 0, sizeof _buttons);
    _buttonDelivered = 0;
    _buttonRefused = 0;
    memset(&_powerHold, 0, sizeof _powerHold);
    memset(&_momentaryHolds, 0, sizeof _momentaryHolds);
    _droppedButtonLogged = NO;
    _status = @"starting";
    BOOL needSnapshot = (_snapshot == NULL);
    [self publishBlankSnapshotLocked];
    pthread_mutex_unlock(&_lock);

    if (needSnapshot) {
        uint8_t *snapshot = calloc(1, VM_FB_BYTES);
        pthread_mutex_lock(&_lock);
        _snapshot = snapshot;
        [self publishBlankSnapshotLocked];
        if (!snapshot) {
            _state = VMEngineStateIdle;
            _stopRequested = NO;
            _paused = NO;
            _status = @"out of memory";
        }
        pthread_mutex_unlock(&_lock);
        if (!snapshot) return NO;
    }

    pthread_mutex_lock(&_lock);
    BOOL cancelledEarly = (_state == VMEngineStateStopping || _stopRequested);
    VMEngineStopCompletion earlyStopCompletion = nil;
    if (cancelledEarly) {
        earlyStopCompletion = _stopCompletion;
        _stopCompletion = nil;
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _paused = NO;
        _pauseReason = nil;
        _status = @"stopped";
    }
    pthread_mutex_unlock(&_lock);
    if (cancelledEarly) {
        if (earlyStopCompletion)
            dispatch_async(dispatch_get_main_queue(), earlyStopCompletion);
        return NO;
    }

    // Measured either side of the allocation so the guest DRAM's real cost is
    // a number on the screen rather than an assertion in a comment.
    uint64_t before = [VMEngine physFootprintBytes];

    if (!s5l8900_init(&_machine, VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE)) {
        pthread_mutex_lock(&_lock);
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _status = @"allocation failed";
        pthread_mutex_unlock(&_lock);
        [self appendConsole:@"[vm] could not allocate 128 MB of guest DRAM\n"];
        return NO;
    }
    if (![self installGuestPayload]) {
        s5l8900_free(&_machine);
        vm_firmware_boot_destroy(&_firmwareBoot);
        pthread_mutex_lock(&_lock);
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _status = @"guest setup failed";
        pthread_mutex_unlock(&_lock);
        [self appendConsole:@"[vm] could not install the guest payload\n"];
        return NO;
    }

    uint64_t after = [VMEngine physFootprintBytes];
    [self appendConsole:[NSString stringWithFormat:
        @"[vm] guest DRAM: %u MB at 0x%08x, framebuffer at 0x%08x\n"
        @"[vm] footprint before/after allocating it: %.1f / %.1f MB\n",
        VM_GUEST_RAM_SIZE / (1024u * 1024u), VM_GUEST_RAM_BASE,
        vm_guest_fb_pa(VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE),
        before / 1048576.0, after / 1048576.0]];

    NSThread *thread = [[NSThread alloc] initWithTarget:self
                                               selector:@selector(threadMain:)
                                                 object:nil];
    if (!thread) {
        s5l8900_free(&_machine);
        vm_firmware_boot_destroy(&_firmwareBoot);
        pthread_mutex_lock(&_lock);
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _status = @"thread allocation failed";
        pthread_mutex_unlock(&_lock);
        [self appendConsole:@"[vm] could not allocate the emulator thread\n"];
        return NO;
    }
    thread.name = @"S5LBox emulator";
    thread.qualityOfService = NSQualityOfServiceUserInitiated;
    thread.stackSize = 512 * 1024;

    pthread_mutex_lock(&_lock);
    BOOL cancelled = (_state == VMEngineStateStopping || _stopRequested);
    if (!cancelled) {
        _machineReady = YES;
        _thread = thread;
        _state = VMEngineStateRunning;
    }
    pthread_mutex_unlock(&_lock);

    if (cancelled) {
        s5l8900_free(&_machine);
        vm_firmware_boot_destroy(&_firmwareBoot);
        pthread_mutex_lock(&_lock);
        VMEngineStopCompletion cancelledStopCompletion = _stopCompletion;
        _stopCompletion = nil;
        _state = VMEngineStateIdle;
        _stopRequested = NO;
        _paused = NO;
        _pauseReason = nil;
        _status = @"stopped";
        pthread_mutex_unlock(&_lock);
        if (cancelledStopCompletion)
            dispatch_async(dispatch_get_main_queue(), cancelledStopCompletion);
        return NO;
    }

    [thread start];
    return YES;
}

- (void)stop {
    [self stopWithCompletion:nil];
}

- (void)stopWithCompletion:(VMEngineStopCompletion)completion {
    BOOL completeNow = NO;
    pthread_mutex_lock(&_lock);
    if (_state == VMEngineStateIdle) {
        completeNow = YES;
    } else {
        if (completion) {
            VMEngineStopCompletion next = [completion copy];
            VMEngineStopCompletion prior = _stopCompletion;
            if (prior) {
                _stopCompletion = ^{
                    prior();
                    next();
                };
            } else {
                _stopCompletion = next;
            }
        }
    }
    if (_state == VMEngineStateStarting || _state == VMEngineStateRunning ||
        _state == VMEngineStateCheckpointing) {
        _stopRequested = YES;
        _paused = NO;
        _pauseReason = nil;
        _state = VMEngineStateStopping;
    }
    pthread_mutex_unlock(&_lock);

    if (completeNow && completion) {
        VMEngineStopCompletion done = [completion copy];
        dispatch_async(dispatch_get_main_queue(), done);
    }
}

- (void)saveCheckpointAndStopWithCompletion:
        (VMEngineCheckpointCompletion)completion {
    NSString *refusal = nil;
    pthread_mutex_lock(&_lock);
    if (_state == VMEngineStateCheckpointing || _checkpointRequested) {
        refusal = @"A machine checkpoint is already being saved.";
    } else if (_state != VMEngineStateRunning || !_machineReady) {
        refusal = @"The machine stopped before it could be saved.";
    } else if (!_firmwareBoot) {
        refusal = @"Only a running iPhone OS machine can be resumed automatically.";
    } else {
        _checkpointRequested = YES;
        _checkpointCompletion = [completion copy];
        _state = VMEngineStateCheckpointing;
        _status = @"saving checkpoint";
        _rate = 0.0;
        _networkDownRate = 0.0;
        _networkUpRate = 0.0;
        _networkWindowStart = 0.0;
    }
    pthread_mutex_unlock(&_lock);

    if (refusal.length && completion) {
        VMEngineCheckpointCompletion rejected = [completion copy];
        dispatch_async(dispatch_get_main_queue(), ^{
            rejected(NO, refusal);
        });
    }
}

- (void)setPaused:(BOOL)paused {
    [self setPaused:paused reason:(paused ? @"requested" : nil)];
}

- (void)setPaused:(BOOL)paused reason:(NSString *)reason {
    NSString *event = nil;
    pthread_mutex_lock(&_lock);
    /* A suspended machine is retiring nothing, so say nothing rather than
     * leaving the smoothed rate frozen at whatever it was a moment before the
     * pause -- a number that keeps claiming work is being done.
     *
     * Only when it is actually running: -setPaused: is also reached on the way
     * to the background, and a machine that has already halted must keep
     * saying "halted" rather than being relabelled as merely paused. */
    if (_state == VMEngineStateRunning && paused != _paused) {
        _rate = 0.0;
        /* Service is intentionally stopped while the guest is paused. A rate
         * from the last active one-second window would otherwise remain on
         * screen and claim that bytes are still crossing the guest boundary. */
        _networkDownRate = 0.0;
        _networkUpRate = 0.0;
        _networkWindowStart = 0.0;
        if (paused) {
            _pauseReason = reason.length ? [reason copy] : @"requested";
            _status = [NSString stringWithFormat:@"paused (%@)", _pauseReason];
            event = [NSString stringWithFormat:@"[vm] paused: %@\n", _pauseReason];
        } else {
            NSString *oldReason = _pauseReason ?: @"requested";
            _pauseReason = nil;
            _status = @"running";
            event = [NSString stringWithFormat:@"[vm] resumed: %@ pause cleared\n",
                                               oldReason];
        }
    }
    _paused = paused;
    pthread_mutex_unlock(&_lock);
    if (event.length) [self appendConsole:event];
}

- (BOOL)isPaused {
    pthread_mutex_lock(&_lock);
    BOOL paused = _paused;
    pthread_mutex_unlock(&_lock);
    return paused;
}

- (BOOL)isRunning {
    pthread_mutex_lock(&_lock);
    BOOL running = (_state == VMEngineStateRunning ||
                    _state == VMEngineStateCheckpointing);
    pthread_mutex_unlock(&_lock);
    return running;
}

- (void)setInstructionCap:(uint64_t)cap {
    pthread_mutex_lock(&_lock);
    _instructionCap = cap;
    pthread_mutex_unlock(&_lock);
}

- (uint64_t)instructionCap {
    pthread_mutex_lock(&_lock);
    uint64_t cap = _instructionCap;
    pthread_mutex_unlock(&_lock);
    return cap;
}

#pragma mark - Guest input (see the header for what reaches the guest)

+ (NSString *)buttonUnavailableReason {
    /*
     * NIL, and this is the only thing that changed about it: the path exists.
     * core/src/soc/buttons.c models all five of the board's switches on the
     * GPIO pins and interrupt lines /device-tree/buttons names, and
     * -setButton:pressed: queues a transition that the emulator thread hands
     * to that model.
     *
     * This is a statement about the EMULATOR, not about the guest. Whether any
     * particular guest has a driver listening is a live question with a live
     * answer, and -buttonUnavailableReason is where it is asked; a control bar
     * that wants to know whether pressing Home will do something must ask that
     * one. This class method exists so a control can be built at all.
     */
    return nil;
}

- (NSString *)buttonUnavailableReason {
    pthread_mutex_lock(&_lock);
    BOOL ready      = _machineReady && _state == VMEngineStateRunning;
    uint64_t made   = _buttonDelivered;
    uint64_t lost   = _buttonQueue.dropped;
    uint64_t held   = _buttonQueue.count;
    uint64_t said_no = _buttonRefused;
    pthread_mutex_unlock(&_lock);

    /* Once the board has taken a transition, the app's half of this is proven
     * and nothing later can un-prove it. A subsequent refusal is a fact about
     * the guest at that moment, not about whether this path works. */
    if (made > 0) return nil;
    if (!ready)   return @"the machine is not running";
    if (said_no > 0)
        return @"the guest has not armed the button interrupt lines — no "
               @"driver has claimed /device-tree/buttons";
    if (lost > 0) return @"the queue filled before the board took anything";
    if (held > 0) return @"queued, not yet handed to the board";
    return @"nothing sent yet";
}

- (void)buttonCountersQueued:(uint64_t *)queued
                   delivered:(uint64_t *)delivered
                     refused:(uint64_t *)refused
                     dropped:(uint64_t *)dropped {
    pthread_mutex_lock(&_lock);
    if (queued)    *queued    = _buttonQueue.queued;
    if (delivered) *delivered = _buttonDelivered;
    if (refused)   *refused   = _buttonRefused;
    if (dropped)   *dropped   = _buttonQueue.dropped;
    pthread_mutex_unlock(&_lock);
}

- (NSString *)touchUnavailableReason {
    pthread_mutex_lock(&_lock);
    BOOL ready     = _machineReady && _state == VMEngineStateRunning;
    uint64_t made  = _touchDelivered;
    uint64_t lost  = _touch.dropped;
    uint64_t held  = _touch.count;
    pthread_mutex_unlock(&_lock);

    /* Once the device has taken a report, the app's half of this is proven and
     * nothing later can un-prove it. A subsequent refusal is a fact about the
     * guest at that moment, not about whether this path works. */
    if (made > 0) return nil;
    if (!ready)   return @"the machine is not running";
    if (lost > 0)
        return @"the guest's touch controller refused the report — no driver "
               @"has announced itself";
    if (held > 0) return @"queued, not yet handed to the controller";
    return @"nothing sent yet";
}

- (void)touchCountersQueued:(uint64_t *)queued
                  delivered:(uint64_t *)delivered
                  coalesced:(uint64_t *)coalesced
                    dropped:(uint64_t *)dropped {
    pthread_mutex_lock(&_lock);
    if (queued)    *queued    = _touch.queued;
    if (delivered) *delivered = _touchDelivered;
    if (coalesced) *coalesced = _touch.coalesced;
    if (dropped)   *dropped   = _touch.dropped;
    pthread_mutex_unlock(&_lock);
}

+ (NSString *)nameForButton:(VMButton)button {
    switch (button) {
        case VMButtonHome:         return @"Home";
        case VMButtonPower:        return @"Power";
        case VMButtonVolumeUp:     return @"Vol +";
        case VMButtonVolumeDown:   return @"Vol -";
        case VMButtonRingerSilent: return @"Silent";
        case VMButtonCount:        break;
    }
    return @"?";
}

- (BOOL)setButton:(VMButton)button pressed:(BOOL)pressed {
    unsigned which = 0;
    bool guestPressed = false;
    /* The app's enum order and the core's are different and permanently so —
     * see VMButtonQueue.h. The translation is a table in plain C that a host CI
     * runner checks against the core's enum, rather than a cast that happens to
     * work; the cast below is safe only because of the _Static_asserts at the
     * top of this file, which are the one place the two app enums meet. */
    if (!vm_button_to_guest((unsigned)button, pressed ? true : false,
                            &which, &guestPressed))
        return NO;

    BOOL queued = NO, notRunning = NO;
    pthread_mutex_lock(&_lock);
    /* Recorded under the same lock as everything else the UI can read, so the
     * control bar can draw a held key without asking the emulator thread. */
    _buttons[button] = pressed;
    if (!_machineReady || _state != VMEngineStateRunning)
        notRunning = YES;
    else
        queued = vm_button_queue_push(&_buttonQueue, which, guestPressed);
    pthread_mutex_unlock(&_lock);

    if (notRunning) {
        [self noteDiscardedInput];
        return NO;
    }
    if (!queued) {
        [self noteDroppedButton];
        return NO;
    }
    return YES;     /* QUEUED. Not "the guest saw it" — see the header. */
}

- (BOOL)isButtonPressed:(VMButton)button {
    if (button >= VMButtonCount) return NO;
    pthread_mutex_lock(&_lock);
    BOOL held = _buttons[button];
    pthread_mutex_unlock(&_lock);
    return held;
}

- (BOOL)sendTouchAtGuestX:(int)x y:(int)y phase:(vm_touch_phase_t)phase {
    s5l_mt_contact_t c;
    if (!vm_touch_contact_from_ui(phase, x, y, &c))
        return NO;              // off the panel, or a phase we do not produce

    BOOL queued = NO, notRunning = NO;
    pthread_mutex_lock(&_lock);
    if (!_machineReady || _state != VMEngineStateRunning)
        notRunning = YES;
    else
        queued = vm_touch_queue_push(&_touch, &c);
    pthread_mutex_unlock(&_lock);

    if (notRunning) {
        [self noteDiscardedInput];
        return NO;
    }
    if (!queued) {
        [self noteDroppedTouch];
        return NO;
    }
    return YES;
}

/*
 * Drain one queued report into the device. Runs ONLY on the emulator thread,
 * between chunks, which is what makes it safe to reach into _machine at all.
 *
 * One per chunk is not a throttle, it is the device's own shape: the Z2 holds
 * a single report and refuses the next until the guest has clocked this one
 * out. Draining more would just be a run of refusals.
 *
 * A refusal is not necessarily an error. s5l_mtz2_set_contacts() says no when
 * a report is still pending — ordinary backpressure, and the report stays
 * queued for the next chunk — and also when the part is held in reset or the
 * driver has not yet been told it is alive, in which case the report can never
 * be read and holding it would wedge the queue behind it. s5l_mtz2_irq() tells
 * the two apart: it is true exactly while a queued report is still unread.
 */
- (void)drainOneTouch_emulatorThread {
    s5l_mt_contact_t c;
    /* A full-queue producer may displace even the front MOVED. Keep the
     * queue locked until this exact report has been accepted and removed,
     * otherwise pop could discard the next edge instead. The device setter
     * only builds one bounded in-memory frame; tick and UI work stay outside
     * the critical section. */
    pthread_mutex_lock(&_lock);
    BOOL have = vm_touch_queue_peek(&_touch, &c);
    if (!have) {
        pthread_mutex_unlock(&_lock);
        return;
    }

    if (s5l_mtz2_set_contacts(&_machine.mtz2, &c, 1u)) {
        vm_touch_queue_pop(&_touch);
        vm_touch_delivery_note_accepted(&_touchDelivery, &c);
        _touchDelivered++;
        pthread_mutex_unlock(&_lock);
        /* The attention line moved behind the bus. `level_dirty` in soc.h says
         * why a machine that is not told re-derives the cascade up to 68
         * instructions later instead of at this chunk boundary. */
        s5l8900_tick(&_machine, 0);
        return;
    }

    if (s5l_mtz2_irq(&_machine.mtz2)) {
        pthread_mutex_unlock(&_lock);
        return;                 // backpressure: the guest has not read yet
    }

    /* The device is in no state to report and will not become one by waiting
     * on this report in particular. Drop it, count it, and let the next one
     * try — a queue that never empties would turn a transient into a
     * permanent loss of input. */
    vm_touch_queue_pop(&_touch);
    _touch.dropped++;
    pthread_mutex_unlock(&_lock);
    [self noteDroppedTouch];
}

/*
 * Hand ONE queued transition to the board. Runs ONLY on the emulator thread,
 * between chunks, which is what makes it safe to reach into _machine at all.
 *
 * One per chunk is not a throttle, it is the board's own shape: it refuses a
 * second transition on a line whose previous one the guest has not serviced,
 * so draining more would just be a run of refusals.
 *
 * A refusal is not necessarily an error and it is never a reason to throw the
 * transition away. s5l8900_set_button() says no while the guest has not armed
 * that line's interrupt — which, for a real boot, is true until
 * AppleM68Buttons starts a couple of hundred million instructions in — and
 * while the previous transition on that line is still pending. Both become
 * false with time, so the transition stays queued and is retried, exactly as
 * bootkernel's --button retries on every instruction. Only a FULL queue loses
 * input, and VMButtonQueue.c counts that.
 */
- (void)drainOneButton_emulatorThread {
    vm_button_event_t e;
    pthread_mutex_lock(&_lock);
    BOOL have = vm_button_queue_peek(&_buttonQueue, &e);
    vm_button_power_hold_t powerHold = _powerHold;
    vm_button_momentary_holds_t momentaryHolds = _momentaryHolds;
    pthread_mutex_unlock(&_lock);
    if (!have) return;

    /* A UIKit tap can queue down and up in the same emulator chunk.  Every
     * momentary key is sampled by a later guest debounce callback, so a
     * zero-duration electrical pulse can disappear before AppleM68Buttons ever
     * sees it.  Power additionally has a post-wake boundary.  Both policies
     * anchor at BOARD acceptance, not at the UI event, because a press may wait
     * in this queue while the guest arms its line. */
    uint64_t nowNS = vm_now_ns();
    uint64_t nowCycles = _machine.cpu.cycles;
    bool displayRunning = s5l_clcd_running(&_machine.clcd);
    if (!vm_button_momentary_release_ready(&e, &momentaryHolds, nowNS))
        return;
    if (!vm_button_power_release_ready(&e, &powerHold, nowNS, nowCycles,
                                       displayRunning))
        return;

    if (s5l8900_set_button(&_machine, e.which, e.pressed)) {
        if (nowNS == 0u) nowNS = vm_now_ns();
        nowCycles = _machine.cpu.cycles;
        pthread_mutex_lock(&_lock);
        vm_button_queue_pop(&_buttonQueue);
        _buttonDelivered++;
        vm_button_momentary_note_accepted(&e, &_momentaryHolds, nowNS);
        if (e.which == S5L_BUTTON_HOLD) {
            if (e.pressed) {
                _powerHold.active = true;
                _powerHold.display_running_at_press = displayRunning;
                _powerHold.delivered_ns = nowNS;
                _powerHold.delivered_cycles = nowCycles;
            } else {
                memset(&_powerHold, 0, sizeof _powerHold);
            }
        }
        pthread_mutex_unlock(&_lock);
        return;
    }
    pthread_mutex_lock(&_lock);
    _buttonRefused++;
    pthread_mutex_unlock(&_lock);
}

/*
 * A checkpoint is a lifecycle cancellation boundary. Once
 * saveCheckpointAndStopWithCompletion: changes the state to Checkpointing, the
 * UI cannot enqueue anything new. Reports which have not reached hardware can
 * therefore be cancelled; reports the devices already accepted must receive a
 * real release and enough guest execution to consume it before serialization.
 */
- (void)beginCheckpointInputQuiesce_emulatorThread {
    pthread_mutex_lock(&_lock);
    unsigned touches = vm_touch_queue_cancel_pending(&_touch);
    unsigned buttons = vm_button_queue_cancel_pending(&_buttonQueue);
    memset(_buttons, 0, sizeof _buttons);
    pthread_mutex_unlock(&_lock);

    if (touches || buttons) {
        [self appendConsole:[NSString stringWithFormat:
            @"[input] checkpoint cancelled %u queued touch report(s) and %u "
             @"button transition(s) before releasing accepted input\n",
            touches, buttons]];
    }
}

- (BOOL)checkpointInputIsQuiescent_emulatorThread {
    BOOL touchWaiting = NO;
    pthread_mutex_lock(&_lock);
    unsigned touchQueued = _touch.count;
    vm_touch_delivery_state_t touchDelivery = _touchDelivery;
    pthread_mutex_unlock(&_lock);

    if (touchQueued) {
        [self drainOneTouch_emulatorThread];
        touchWaiting = YES;
    } else if (s5l_mtz2_irq(&_machine.mtz2)) {
        /* A report already accepted by the controller still has to be clocked
         * out before a cancellation can follow it. */
        touchWaiting = YES;
    } else if (touchDelivery.active) {
        s5l_mt_contact_t release;
        if (vm_touch_delivery_make_break(&touchDelivery, &release)) {
            pthread_mutex_lock(&_lock);
            BOOL queued = vm_touch_queue_push(&_touch, &release);
            pthread_mutex_unlock(&_lock);
            if (queued) [self drainOneTouch_emulatorThread];
        }
        touchWaiting = YES;
    } else if (s5l_gpioic_pending(&_machine.gpioic,
                                  S5L_GPIOIC_LINE_MULTITOUCH)) {
        /* The controller has been read; let the guest acknowledge the cascade
         * as well so the restored boundary contains no deferred touch IRQ. */
        touchWaiting = YES;
    }

    BOOL buttonWaiting = NO;
    pthread_mutex_lock(&_lock);
    unsigned buttonQueued = _buttonQueue.count;
    pthread_mutex_unlock(&_lock);

    if (buttonQueued) {
        [self drainOneButton_emulatorThread];
        buttonWaiting = YES;
    } else {
        /* Ordinary keys first. Power has a longer, display-aware release floor
         * and must not prevent independent lines from returning to rest. */
        static const unsigned releaseOrder[S5L_BUTTON_COUNT] = {
            S5L_BUTTON_MENU, S5L_BUTTON_VOLUP, S5L_BUTTON_VOLDOWN,
            S5L_BUTTON_RINGERAB, S5L_BUTTON_HOLD,
        };
        for (unsigned i = 0; i < S5L_BUTTON_COUNT; i++) {
            unsigned which = releaseOrder[i];
            if (!s5l_buttons_held(&_machine.buttons, which)) continue;
            pthread_mutex_lock(&_lock);
            BOOL queued = vm_button_queue_push(&_buttonQueue, which, false);
            pthread_mutex_unlock(&_lock);
            if (queued) [self drainOneButton_emulatorThread];
            buttonWaiting = YES;
            break;
        }

        /* The release itself is serialized, but waiting for its interrupt to
         * clear makes this a genuinely quiet guest boundary rather than a
         * deferred host gesture that happens to be representable on disk. */
        if (!buttonWaiting) {
            for (unsigned i = 0; i < S5L_BUTTON_COUNT; i++) {
                if (s5l_gpioic_pending(&_machine.gpioic,
                                       s5l_button_line(i))) {
                    buttonWaiting = YES;
                    break;
                }
            }
        }
    }

    if (touchWaiting || buttonWaiting) return NO;

    pthread_mutex_lock(&_lock);
    memset(_buttons, 0, sizeof _buttons);
    memset(&_powerHold, 0, sizeof _powerHold);
    memset(&_momentaryHolds, 0, sizeof _momentaryHolds);
    pthread_mutex_unlock(&_lock);
    return YES;
}

/* Same discipline as noteDroppedTouch. */
- (void)noteDroppedButton {
    pthread_mutex_lock(&_lock);
    BOOL first = !_droppedButtonLogged;
    _droppedButtonLogged = YES;
    uint64_t dropped = _buttonQueue.dropped;
    pthread_mutex_unlock(&_lock);
    if (!first) return;

    [self appendConsole:[NSString stringWithFormat:
        @"[input] a button transition was not delivered (%llu so far). The "
        @"queue was full, and a press or a release may never be coalesced "
        @"away. Printed once.\n", (unsigned long long)dropped]];
}

/* Same discipline as noteDiscardedInput: a drag can drop many reports, and a
 * console that scrolls the guest's own output away to repeat itself is worse
 * than one that says it once. */
- (void)noteDroppedTouch {
    pthread_mutex_lock(&_lock);
    BOOL first = !_droppedTouchLogged;
    _droppedTouchLogged = YES;
    uint64_t dropped = _touch.dropped;
    pthread_mutex_unlock(&_lock);
    if (!first) return;

    [self appendConsole:[NSString stringWithFormat:
        @"[input] a touch report was not delivered (%llu so far). The guest's "
        @"touch controller refused it, or the queue had no safe space for "
        @"the report and its release. Printed once.\n",
        (unsigned long long)dropped]];
}

/* Say it once. A drag produces a report per frame, and a console that scrolls
 * the guest's own output away to repeat the same refusal is worse than one
 * that states it plainly and then stops. */
- (void)noteDiscardedInput {
    pthread_mutex_lock(&_lock);
    BOOL first = !_discardedInputLogged;
    _discardedInputLogged = YES;
    pthread_mutex_unlock(&_lock);
    if (!first) return;

    /* This used to quote +buttonUnavailableReason, which now returns nil and
     * would have formatted as "(null)". It was never the right sentence
     * anyway: both callers reach here for one reason and it is this one. */
    [self appendConsole:
        @"[input] discarded: the machine is not running. The input is shown on "
        @"screen and thrown away; the guest is never told. Printed once.\n"];
}

#pragma mark - Spin sampler

/*
 * A WEDGED GUEST AND A BUSY ONE LOOK IDENTICAL FROM OUTSIDE. Instructions
 * retire, the rate looks healthy, and nothing is printed. The difference is
 * WHERE the instructions go, and on a phone there is no debugger to ask.
 *
 * So the emulator thread asks the machine itself, between chunks, at a point
 * where nothing is executing and no lock is held: one PC per chunk, binned
 * into 64-byte regions.
 *
 * Sampling once per 100,000 instructions sounds far too sparse to find a
 * loop, and for a profile it would be. For THIS question it is exactly right:
 * a spin loop executes its handful of instructions billions of times, so
 * every single sample lands inside it. Sparseness is not a weakness here, it
 * is what makes the signal unambiguous -- a region that takes almost a whole
 * window is not "hot", it is the only thing running.
 *
 * Diagnostics only, in the sense this project means it: it never stops the
 * guest, never writes guest state, never changes what executes, and cannot
 * manufacture a result. It reports where the machine already was.
 *
 * Addresses are printed raw rather than symbolised. Carrying a symbol table
 * for an 8 MB kernel on the device costs memory on the one machine that has
 * least of it, and resolving four numbers against ksyms afterwards costs
 * nothing.
 */
#define kVMSpinRegionShift 6u      /* 64-byte regions                        */
#define kVMSpinSlots       64u     /* wider than any spin; full == not a spin */
#define kVMSpinWindow      512u    /* ~2.5 s of guest time at 20 M insn/s    */
#define kVMSpinShareNum    7u      /* report at >= 7/8 of a window in one    */
#define kVMSpinShareDen    8u      /* region                                 */
#define kVMSpinReportCap   6u      /* never turn the console into a log      */
#define kVMSpinNarrowRegions 4u    /* <= 256 bytes of code IS a loop         */

typedef struct {
    uint32_t region[kVMSpinSlots];
    uint32_t count[kVMSpinSlots];
    unsigned used;
    unsigned samples;
    uint32_t reported[kVMSpinReportCap];
    unsigned reports;
    unsigned quiet;          /* windows closed without a concentrated region */
    bool     spreadReported;
} vm_spin_t;

/* Windows of ordinary work before saying so. A "stuck" report that stays
 * silent when the guest is merely grinding leaves the user unable to tell the
 * two apart, which is the whole question they are asking. */
#define kVMSpinSpreadWindows 8u

static void vm_spin_sample(vm_spin_t *s, uint32_t pc) {
    uint32_t region = pc >> kVMSpinRegionShift;
    s->samples++;
    for (unsigned i = 0; i < s->used; i++) {
        if (s->region[i] == region) { s->count[i]++; return; }
    }
    /* Table full means the guest is spread across more than 64 regions, which
     * is the shape of ordinary work, not a spin. The sample still counts so
     * the window closes on time and the share test fails honestly. */
    if (s->used < kVMSpinSlots) {
        s->region[s->used] = region;
        s->count[s->used] = 1u;
        s->used++;
    }
}

static void vm_spin_reset(vm_spin_t *s) {
    s->used = 0u;
    s->samples = 0u;
}

/* Index of the nth-hottest region, or -1 once they run out. Selection rather
 * than a sort: n is 3 and the table is 64. */
static int vm_spin_rank(const vm_spin_t *s, unsigned n) {
    int best = -1;
    uint32_t ceiling = 0xffffffffu;
    for (unsigned rank = 0; rank <= n; rank++) {
        best = -1;
        for (unsigned i = 0; i < s->used; i++) {
            if (s->count[i] > ceiling) continue;
            if (best < 0 || s->count[i] > s->count[best]) best = (int)i;
        }
        if (best < 0) return -1;
        ceiling = s->count[best] - 1u;   /* strictly below, next round */
    }
    return best;
}

static bool vm_spin_already_reported(const vm_spin_t *s, uint32_t region) {
    for (unsigned i = 0; i < s->reports && i < kVMSpinReportCap; i++)
        if (s->reported[i] == region) return true;
    return false;
}

#pragma mark - Emulator thread

static bool vm_guest_pc_profile_row(void *opaque, uint64_t bin,
                                     uint64_t pc, uint64_t samples) {
    NSMutableString *text = (__bridge NSMutableString *)opaque;
    [text appendFormat:@"%08llx,%08llx,%llu\n",
        (unsigned long long)bin, (unsigned long long)pc,
        (unsigned long long)samples];
    return true;
}

/* Export only at a pause/stop boundary, never while timing guest work. Unlike
 * the compact AX top-16, this preserves every captured bin for attribution.
 * A unique no-overwrite file and completion footer make partial files explicit. */
- (void)exportGuestPCProfile_emulatorThread {
    s5l_static_a64_compact_pc_profile_t profile;
    s5l8900_static_a64_compact_raw_pc_profile(&_machine, &profile);
    if (!profile.enabled) return;
    @autoreleasepool {
        vm_instance_paths_t paths;
        if (![self resolveFilesInto:&paths note:NULL]) return;
        NSMutableString *text = [NSMutableString stringWithString:
            @"s5lbox-guest-pc-v1\nbin,representative_pc,samples\n"];
        uint64_t captured = 0u, dropped = 0u;
        if (!text || !s5l8900_static_a64_compact_raw_guest_pc_profile_visit(
                &_machine, vm_guest_pc_profile_row, (__bridge void *)text,
                &captured, &dropped)) {
            [self appendConsole:@"[vm] guest PC export could not capture all bins\n"];
            return;
        }
        [text appendFormat:@"complete,%llu,%llu\n",
            (unsigned long long)captured, (unsigned long long)dropped];
        NSString *directory = [NSString stringWithUTF8String:paths.machine];
        NSString *name = [NSString stringWithFormat:@"engine.guest-pc-%@.csv",
            [NSUUID UUID].UUIDString];
        NSString *path = [directory stringByAppendingPathComponent:name];
        NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
        NSError *error = nil;
        if (path && data && [data writeToFile:path
                options:NSDataWritingWithoutOverwriting error:&error])
            [self appendConsole:[NSString stringWithFormat:
                @"[vm] complete guest PC histogram saved: %@\n", name]];
        else
            [self appendConsole:[NSString stringWithFormat:
                @"[vm] guest PC histogram was not saved: %@\n",
                error.localizedDescription ?: @"invalid export path or data"]];
    }
}

static NSString *vm_profile_csv_string(const char *value) {
    NSString *text = value ? [NSString stringWithUTF8String:value] : @"";
    return [NSString stringWithFormat:@"\"%@\"",
        [(text ?: @"") stringByReplacingOccurrencesOfString:@"\""
                                                withString:@"\"\""]];
}

static bool vm_native_pc_profile_row(void *opaque, uint64_t bin,
                                     uint64_t pc, uint64_t samples) {
    NSMutableString *text = (__bridge NSMutableString *)opaque;
    Dl_info info = {0};
    (void)dladdr((const void *)(uintptr_t)pc, &info);
    [text appendFormat:@"%016llx,%016llx,%llu,%016llx,%016llx,%@,%@\n",
        (unsigned long long)bin, (unsigned long long)pc,
        (unsigned long long)samples,
        (unsigned long long)(uintptr_t)info.dli_fbase,
        (unsigned long long)(uintptr_t)info.dli_saddr,
        vm_profile_csv_string(info.dli_fname),
        vm_profile_csv_string(info.dli_sname)];
    return true;
}

/* Native addresses are host diagnostic state, never serialized into a guest
 * checkpoint. Symbol lookup and file I/O occur only at pause/stop boundaries.
 * Keep all bins: the AX top-eight omits most outside-runner observations. */
- (void)exportNativePCProfile_emulatorThread {
    s5l_static_a64_compact_pc_profile_t profile;
    s5l8900_static_a64_compact_raw_pc_profile(&_machine, &profile);
    if (!profile.enabled) return;
    @autoreleasepool {
        vm_instance_paths_t paths;
        if (![self resolveFilesInto:&paths note:NULL]) return;
        NSMutableString *text = [NSMutableString stringWithFormat:
            @"s5lbox-native-pc-v1\nreference_pc,%016llx\n"
             "bin,representative_pc,samples,image_base,symbol_address,image,symbol\n",
            (unsigned long long)profile.reference_pc];
        uint64_t captured = 0u, dropped = 0u;
        if (!text || !s5l8900_static_a64_compact_raw_host_pc_profile_visit(
                &_machine, vm_native_pc_profile_row, (__bridge void *)text,
                &captured, &dropped) || captured > profile.samples ||
                dropped != profile.samples - captured) {
            [self appendConsole:@"[vm] native PC export did not match the paused sample total\n"];
            return;
        }
        [text appendFormat:@"complete,%llu,%llu\n",
            (unsigned long long)captured, (unsigned long long)dropped];
        NSString *directory = [NSString stringWithUTF8String:paths.machine];
        NSString *name = [NSString stringWithFormat:@"engine.native-pc-%@.csv",
            [NSUUID UUID].UUIDString];
        NSString *path = [directory stringByAppendingPathComponent:name];
        NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
        NSError *error = nil;
        if (path && data && [data writeToFile:path
                options:NSDataWritingWithoutOverwriting error:&error])
            [self appendConsole:[NSString stringWithFormat:
                @"[vm] complete native PC histogram saved: %@\n", name]];
        else
            [self appendConsole:[NSString stringWithFormat:
                @"[vm] native PC histogram was not saved: %@\n",
                error.localizedDescription ?: @"invalid export path or data"]];
    }
}

- (void)threadMain:(id)unused {
    (void)unused;
    double lastPublish = vm_now();
    uint64_t retired = 0, retiredAtLastPublish = 0;
    arm_status_t status = ARM_OK;
    BOOL stoppedByRequest = NO;
    BOOL reachedCap = NO;
    BOOL checkpointSaved = NO;
    NSString *checkpointFailure = nil;
    BOOL checkpointInputPrepared = NO;
    BOOL profilePauseExported = NO;
    uint64_t checkpointInputStartNS = 0u;
    uint64_t checkpointInputStartRetired = 0u;

    /* Emulator-thread only, so it is a local and not an ivar: nothing else may
     * read it and no lock can be forgotten. */
    vm_spin_t spin;
    memset(&spin, 0, sizeof spin);

    while (YES) {
        @autoreleasepool {
            pthread_mutex_lock(&_lock);
            BOOL stop = _stopRequested;
            BOOL checkpoint = _checkpointRequested;
            BOOL paused = _paused;
            pthread_mutex_unlock(&_lock);

            if (stop) {
                stoppedByRequest = YES;
                break;
            }
            if (checkpoint) {
                if (!checkpointInputPrepared) {
                    [self beginCheckpointInputQuiesce_emulatorThread];
                    checkpointInputStartNS = vm_now_ns();
                    checkpointInputStartRetired = retired;
                    checkpointInputPrepared = YES;
                }

                BOOL inputReady =
                    [self checkpointInputIsQuiescent_emulatorThread];
                uint64_t nowNS = vm_now_ns();
                BOOL hostTimedOut =
                    !inputReady && checkpointInputStartNS != 0u &&
                    nowNS != 0u && nowNS >= checkpointInputStartNS &&
                    nowNS - checkpointInputStartNS >=
                        kVMCheckpointInputTimeoutNS;
                BOOL guestTimedOut =
                    !inputReady && retired >= checkpointInputStartRetired &&
                    retired - checkpointInputStartRetired >=
                        kVMCheckpointInputTimeoutInstructions;

                char why[VM_FW_BOOT_DETAIL_CAPACITY] = {0};
                vm_firmware_checkpoint_status_t saved = VM_FW_CHECKPOINT_BUSY;
                if (inputReady) {
                    /* Drain host-visible output before serializing. Otherwise
                     * UART bytes already shown on this run would be present
                     * again after restore and appear twice. */
                    [self publishRetired:retired rate:0.0 status:ARM_OK];
                    saved = vm_firmware_boot_save_resume(
                        _firmwareBoot, &_machine, why, sizeof why);
                } else if (hostTimedOut || guestTimedOut) {
                    (void)snprintf(
                        why, sizeof why,
                        "The guest did not consume transient input releases "
                        "within the checkpoint safety window. Nothing was "
                        "saved; the machine is still running.");
                    saved = VM_FW_CHECKPOINT_ERROR;
                }
                if (saved == VM_FW_CHECKPOINT_OK) {
                    [self appendConsole:@"[vm] automatic resume checkpoint saved\n"];
                    checkpointSaved = YES;
                    stoppedByRequest = YES;
                    break;
                }
                if (saved == VM_FW_CHECKPOINT_ERROR) {
                    checkpointFailure = why[0]
                        ? [NSString stringWithUTF8String:why]
                        : @"The machine checkpoint could not be saved.";
                    if (!checkpointFailure.length)
                        checkpointFailure = @"The machine checkpoint could not be saved.";

                    pthread_mutex_lock(&_lock);
                    VMEngineCheckpointCompletion failed = _checkpointCompletion;
                    _checkpointCompletion = nil;
                    _checkpointRequested = NO;
                    _state = VMEngineStateRunning;
                    _status = _paused ? @"paused" : @"running (save failed)";
                    pthread_mutex_unlock(&_lock);
                    checkpointInputPrepared = NO;

                    if (failed) {
                        NSString *message = checkpointFailure;
                        dispatch_async(dispatch_get_main_queue(), ^{
                            failed(NO, message);
                        });
                    }
                    checkpointFailure = nil;
                    continue;
                }
                /* BUSY: execute one bounded chunk so input cancellation or an
                 * in-flight disk operation can reach a safe boundary, then
                 * retry before any further UI work. This deliberately advances
                 * even if the machine was user-paused: neither boundary is safe
                 * to freeze halfway through. */
            }
            if (paused && !checkpoint) {
                if (!profilePauseExported) {
                    [self exportGuestPCProfile_emulatorThread];
                    [self exportNativePCProfile_emulatorThread];
                    profilePauseExported = YES;
                }
                usleep(50 * 1000);
                lastPublish = vm_now();
                retiredAtLastPublish = retired;
                continue;
            }
            profilePauseExported = NO;

            /* The only place ordinary UI input reaches the machine, and it is
             * on this thread, between chunks, with nothing executing. During a
             * checkpoint the quiesce state machine above owns input instead. */
            if (!checkpoint) {
                [self drainOneTouch_emulatorThread];
                [self drainOneButton_emulatorThread];
            }

            retired += s5l8900_run(&_machine, kVMChunkInstructions, &status);

            /* Taken here precisely because the chunk has ENDED: the machine is
             * between instructions, this thread owns it, and no lock is held. */
            vm_spin_sample(&spin, _machine.cpu.r[15]);
            if (spin.samples >= kVMSpinWindow) {
                int hot = vm_spin_rank(&spin, 0);
                uint32_t region = hot >= 0 ? spin.region[hot] : 0u;
                /*
                 * Two ways to be a spin, and the second was learned the hard
                 * way. A single region taking almost the whole window is the
                 * obvious one. But a loop that straddles a 64-byte boundary
                 * splits across two regions, and an unrolled one across three
                 * or four -- and testing only the hottest region called a
                 * genuinely wedged device "not spinning" while it sat in
                 * AppleMBX+0xb440 with 309 of 512 samples in one half and the
                 * rest in the other. A handful of distinct regions across a
                 * whole window IS the signal, however it splits between them.
                 */
                bool concentrated =
                    hot >= 0 &&
                    (uint64_t)spin.count[hot] * kVMSpinShareDen >=
                        (uint64_t)spin.samples * kVMSpinShareNum;
                bool narrow = spin.used > 0u && spin.used <= kVMSpinNarrowRegions;
                if (hot >= 0 && (concentrated || narrow) &&
                    spin.reports < kVMSpinReportCap &&
                    !vm_spin_already_reported(&spin, region)) {
                    spin.reported[spin.reports++] = region;

                    NSMutableString *next = [NSMutableString string];
                    for (unsigned n = 1; n <= 2; n++) {
                        int i = vm_spin_rank(&spin, n);
                        if (i < 0) break;
                        [next appendFormat:@"%@0x%08x (%u)",
                            next.length ? @", " : @"",
                            (unsigned)(spin.region[i] << kVMSpinRegionShift),
                            (unsigned)spin.count[i]];
                    }
                    uint32_t base = region << kVMSpinRegionShift;
                    [self appendConsole:[NSString stringWithFormat:
                        @"[stall] at %.1f M insn the guest is looping in %u "
                        @"region%@: 0x%08x-0x%08x took %u of %u samples, cpsr "
                        @"0x%08x.%@%@ Nothing was stopped or altered to find "
                        @"this. Printed once per region.\n",
                        retired / 1.0e6, spin.used,
                        spin.used == 1u ? @"" : @"s", base,
                        base + (1u << kVMSpinRegionShift) - 1u,
                        (unsigned)spin.count[hot], (unsigned)spin.samples,
                        _machine.cpu.cpsr,
                        next.length ? @" Next: " : @"",
                        next.length ? [next stringByAppendingString:@"."] : @""]];
                } else if (hot >= 0) {
                    /* Not concentrated. Say that too, once: "it is working, it
                     * is just slow" and "it is wedged" are the two answers, and
                     * an instrument that can only report one of them is not
                     * telling the user what they asked. */
                    spin.quiet++;
                    if (spin.quiet >= kVMSpinSpreadWindows && !spin.spreadReported) {
                        spin.spreadReported = true;
                        [self appendConsole:[NSString stringWithFormat:
                            @"[stall] at %.1f M insn the guest is NOT spinning: "
                            @"work is spread over %u regions, the busiest "
                            @"(0x%08x) taking only %u of %u samples. It is "
                            @"executing real code, just slowly. Printed once.\n",
                            retired / 1.0e6, spin.used,
                            (unsigned)(spin.region[hot] << kVMSpinRegionShift),
                            (unsigned)spin.count[hot], (unsigned)spin.samples]];
                    }
                }
                vm_spin_reset(&spin);
            }

            /* A stop can arrive while the bounded chunk is executing. Let the
             * explicit lifecycle request win before publishing a simultaneous
             * guest halt as though the controller were still active. */
            pthread_mutex_lock(&_lock);
            stop = _stopRequested;
            checkpoint = _checkpointRequested;
            uint64_t cap = _instructionCap;
            if (status != ARM_OK && !stop &&
                (_state == VMEngineStateRunning ||
                 _state == VMEngineStateCheckpointing))
                _state = VMEngineStateStopping;
            if (cap > 0 && retired >= cap && !stop &&
                _state == VMEngineStateRunning)
                _state = VMEngineStateStopping;
            pthread_mutex_unlock(&_lock);
            if (stop) {
                stoppedByRequest = YES;
                break;
            }
            if (status != ARM_OK && checkpoint) {
                checkpointFailure =
                    @"The guest stopped before its checkpoint could be completed.";
            }

            /* A diagnostic limit, not a guest fault. Publish the frame and the
             * counters that were reached and stop there, leaving the last
             * picture up: the whole point of asking for a limit is to look at
             * what the machine had drawn by then.
             *
             * `status == ARM_OK` is load-bearing, not defensive. A chunk can
             * both fault and cross the cap, and the terminal block below
             * overwrites _status with "instruction cap reached" — so without
             * this test an undefined instruction or a halt would be reported
             * as a clean diagnostic stop. A failure reported as a success is
             * the one outcome this project does not permit. The fault wins;
             * the cap is still crossed and will be reported next time. */
            if (status == ARM_OK && !checkpoint && cap > 0 && retired >= cap) {
                reachedCap = YES;
                [self publishRetired:retired rate:0.0 status:status];
                break;
            }

            double now = vm_now();
            double elapsed = now - lastPublish;
            if (elapsed >= kVMPublishInterval || status != ARM_OK) {
                double instantRate = elapsed > 0
                    ? (double)(retired - retiredAtLastPublish) / elapsed : 0.0;
                [self publishRetired:retired rate:instantRate status:status];
                lastPublish = now;
                retiredAtLastPublish = retired;
            }

            // A non-OK status means the guest hit an encoding this core does
            // not implement, or halted. Stopping is right: spinning on the same
            // faulting instruction would burn the battery and tell us nothing.
            if (status != ARM_OK) {
                break;
            }
        }
    }

    /* An explicit stop can arrive between regular publications. Drain the
     * remaining UART bytes and publish the final counters before destroying
     * the sole copy of the machine. */
    if (stoppedByRequest && !checkpointSaved)
        [self publishRetired:retired rate:0.0 status:ARM_OK];

    if (reachedCap)
        [self appendConsole:[NSString stringWithFormat:
            @"[vm] stopped at the instruction cap: %llu retired\n",
            (unsigned long long)retired]];
    else if (status != ARM_OK && !stoppedByRequest)
        [self appendConsole:[NSString stringWithFormat:
            @"[vm] terminal CPU status %d at pc=0x%08x cpsr=0x%08x "
             "after %llu retired\n",
            (int)status, _machine.cpu.r[15], _machine.cpu.cpsr,
            (unsigned long long)retired]];

    if (_machineReady) {
        [self exportGuestPCProfile_emulatorThread];
        [self exportNativePCProfile_emulatorThread];
        s5l8900_free(&_machine);
    }
    /* AFTER the machine, never before: the memory-disk bridges hold a borrowed
     * descriptor onto the work image and a pointer into guest DRAM, and both
     * have to stop existing before the file they write through is closed. */
    vm_firmware_boot_destroy(&_firmwareBoot);

    /* Publish the terminal state only after the machine is gone. In
     * particular, clear _thread here so a later start really creates a fresh
     * machine instead of returning a false success for a dead worker. */
    pthread_mutex_lock(&_lock);
    VMEngineCheckpointCompletion checkpointCompletion = _checkpointCompletion;
    VMEngineStopCompletion stopCompletion = _stopCompletion;
    _checkpointCompletion = nil;
    _stopCompletion = nil;
    _checkpointRequested = NO;
    _machineReady = NO;
    _thread = nil;
    _networkStatusValid = NO;
    memset(&_networkStatus, 0, sizeof _networkStatus);
    _networkDownBaseline = 0u;
    _networkUpBaseline = 0u;
    _networkWindowStart = 0.0;
    _networkDownRate = 0.0;
    _networkUpRate = 0.0;
    if (stoppedByRequest) {
        [self publishBlankSnapshotLocked];
        _status = @"stopped";
        _rate = 0.0;
    } else if (reachedCap) {
        _status = @"instruction cap reached";
        _rate = 0.0;
    }
    _state = VMEngineStateIdle;
    _stopRequested = NO;
    _paused = NO;
    _pauseReason = nil;
    pthread_mutex_unlock(&_lock);

    if (checkpointCompletion) {
        BOOL saved = checkpointSaved;
        NSString *message = saved
            ? @"The machine was saved and will resume here next time."
            : (checkpointFailure ?:
               @"The machine stopped before its checkpoint could be completed.");
        dispatch_async(dispatch_get_main_queue(), ^{
            checkpointCompletion(saved, message);
        });
    }
    if (stopCompletion)
        dispatch_async(dispatch_get_main_queue(), stopCompletion);
}

// Called on the emulator thread only.
- (void)publishRetired:(uint64_t)retired
                  rate:(double)instantRate
                status:(arm_status_t)status {
    vm_network_status_t networkStatus;
    BOOL haveNetwork = vm_firmware_boot_network_status(
        _firmwareBoot, &networkStatus) ? YES : NO;
    double nowSec = vm_engine_now_seconds();

    // Ask the display controller where the framebuffer is and how it is laid
    // out, rather than assuming. vm_guest_display() validates that the reported
    // buffer is inside DRAM and no larger than VM_FB_BYTES before returning it.
    uint32_t fbStride = 0, fbW = 0, fbH = 0;
    vm_pixel_order_t order = VM_ORDER_BGRA;
    const uint8_t *fb = vm_guest_display(&_machine, &fbW, &fbH, &fbStride, &order);
    size_t fbBytes = 0;
    if (!fb || fbW == 0 || fbH == 0 || fbW > SIZE_MAX / VM_FB_BPP ||
        fbStride < (size_t)fbW * VM_FB_BPP || fbStride > SIZE_MAX / fbH) {
        fb = NULL;
    } else {
        fbBytes = (size_t)fbStride * fbH;
        if (fbBytes == 0 || fbBytes > VM_FB_BYTES) fb = NULL;
    }

    // Drain the UART before taking the lock's contents out of the machine:
    // core's tx buffer is a fixed 8 KB that stops accepting bytes when full, so
    // whoever is watching has to empty it or the guest goes quiet.
    NSString *fresh = nil;
    if (_machine.uart0.tx_len > 0) {
        fresh = [[NSString alloc] initWithBytes:_machine.uart0.tx
                                         length:_machine.uart0.tx_len
                                       encoding:NSISOLatin1StringEncoding];
        _machine.uart0.tx_len = 0;
    }

    NSString *statusText;
    switch (status) {
        case ARM_OK:        statusText = @"running";        break;
        case ARM_UNDEFINED:
            statusText = [NSString stringWithFormat:@"undefined insn at 0x%08x",
                                                    _machine.cpu.r[15]];
            break;
        case ARM_HALT:
            statusText = [NSString stringWithFormat:@"halted at 0x%08x",
                                                    _machine.cpu.r[15]];
            break;
        default:            statusText = @"?";              break;
    }

    pthread_mutex_lock(&_lock);
    if (fb && _snapshot) {
        /* A future guest mode may use less than the fixed publication buffer.
         * Clear the unused tail so a geometry change cannot expose pixels from
         * the previous frame. */
        if (fbBytes < VM_FB_BYTES)
            memset(_snapshot + fbBytes, 0, VM_FB_BYTES - fbBytes);
        memcpy(_snapshot, fb, fbBytes);
        /* Counted here, where a frame actually becomes visible, and only when
         * its contents differ from the one before it. */
        uint64_t sig = vm_engine_fb_signature(fb, fbBytes);
        if (!_fbSignatureValid || sig != _fbSignature) {
            _fbSignature = sig;
            _fbSignatureValid = YES;
            _fpsWindowFrames++;
        }
        if (_fpsWindowStart <= 0.0) {
            _fpsWindowStart = nowSec;
        } else if (nowSec - _fpsWindowStart >= 0.5) {
            /* A half-second window: long enough that one slow frame does not
             * dominate, short enough to follow a stutter the user can feel. */
            _fps = (double)_fpsWindowFrames / (nowSec - _fpsWindowStart);
            _fpsWindowFrames = 0;
            _fpsWindowStart = nowSec;
        }
        _snapshotARGB = (order == VM_ORDER_ARGB);
        /* Published with the pixels. The reader must not assume 320x480: this
         * geometry came out of whichever CLCD window the guest enabled, and it
         * is the only description of what the bytes above mean. */
        _snapshotWidth  = fbW;
        _snapshotHeight = fbH;
        _snapshotStride = fbStride;
        _snapshotFresh = YES;
        _snapshotBlank = NO;
    } else if (_snapshot && !_snapshotBlank) {
        /* A stopped or invalid controller is a black panel, not permission to
         * leave the last good frame on screen forever. Publish that transition
         * once; do not allocate a new black CGImage at 30 Hz while it remains
         * stopped. */
        [self publishBlankSnapshotLocked];
    }
    if (fresh.length) [_pending appendString:fresh];
    if (_pending.length > kVMConsoleLimit) {
        [_pending deleteCharactersInRange:
            NSMakeRange(0, _pending.length - kVMConsoleLimit)];
    }
    _retired = retired;
    if (haveNetwork) {
        _networkStatus = networkStatus;
        uint64_t down = networkStatus.tcp_bytes_to_guest;
        uint64_t up = networkStatus.tcp_bytes_to_host;
        BOOL counterReset = down < _networkDownBaseline ||
                            up < _networkUpBaseline;
        if (!_networkStatusValid || counterReset || nowSec <= 0.0 ||
            _networkWindowStart <= 0.0 || nowSec < _networkWindowStart) {
            _networkStatusValid = YES;
            _networkDownBaseline = down;
            _networkUpBaseline = up;
            _networkWindowStart = nowSec;
            _networkDownRate = 0.0;
            _networkUpRate = 0.0;
        } else {
            double elapsed = nowSec - _networkWindowStart;
            if (elapsed >= kVMNetworkRateInterval) {
                _networkDownRate =
                    (double)(down - _networkDownBaseline) / elapsed;
                _networkUpRate =
                    (double)(up - _networkUpBaseline) / elapsed;
                _networkDownBaseline = down;
                _networkUpBaseline = up;
                _networkWindowStart = nowSec;
            }
        }
    } else {
        _networkStatusValid = NO;
        memset(&_networkStatus, 0, sizeof _networkStatus);
        _networkDownBaseline = 0u;
        _networkUpBaseline = 0u;
        _networkWindowStart = 0.0;
        _networkDownRate = 0.0;
        _networkUpRate = 0.0;
    }
    // Smooth the rate: a per-chunk figure jitters too much to read.
    if (_paused && _state == VMEngineStateRunning) {
        /* A pause can land while s5l8900_run() is inside its bounded chunk.
         * That chunk publishes once after the request; it must not overwrite
         * the pause cause with a misleading "running" status. */
        _rate = 0.0;
        _status = [NSString stringWithFormat:@"paused (%@)",
                   _pauseReason.length ? _pauseReason : @"requested"];
    } else {
        _rate = (_rate > 0.0) ? (_rate * 0.8 + instantRate * 0.2) : instantRate;
        _status = statusText;
    }
    pthread_mutex_unlock(&_lock);
}

- (void)appendConsole:(NSString *)text {
    if (!text.length) return;
    pthread_mutex_lock(&_lock);
    [_pending appendString:text];
    pthread_mutex_unlock(&_lock);
}

/* A black panel of the panel's nominal size. MUST be called with _lock already
 * held: it is the tail of four different critical sections, and taking the
 * lock here would deadlock every one of them. */
- (void)publishBlankSnapshotLocked {
    if (!_snapshot) return;
    memset(_snapshot, 0, VM_FB_BYTES);
    _snapshotARGB = NO;
    _snapshotFresh = YES;
    _snapshotBlank = YES;
    _snapshotWidth  = VM_FB_WIDTH;
    _snapshotHeight = VM_FB_HEIGHT;
    _snapshotStride = VM_FB_WIDTH * VM_FB_BPP;
}

#pragma mark - Snapshot readers (main thread)

- (BOOL)copyFrameInto:(void *)dst
             capacity:(size_t)capacity
                width:(uint32_t *)outWidth
               height:(uint32_t *)outHeight
               stride:(uint32_t *)outStride
                 argb:(BOOL *)outARGB {
    if (!dst || capacity < VM_FB_BYTES) return NO;
    BOOL copied = NO;
    pthread_mutex_lock(&_lock);
    if (_snapshotFresh && _snapshot) {
        memcpy(dst, _snapshot, VM_FB_BYTES);
        if (outARGB)   *outARGB   = _snapshotARGB;
        if (outWidth)  *outWidth  = _snapshotWidth;
        if (outHeight) *outHeight = _snapshotHeight;
        if (outStride) *outStride = _snapshotStride;
        _snapshotFresh = NO;
        copied = YES;
    }
    pthread_mutex_unlock(&_lock);
    return copied;
}

- (NSString *)takePendingConsoleText {
    NSString *out = nil;
    pthread_mutex_lock(&_lock);
    if (_pending.length) {
        out = [_pending copy];
        [_pending setString:@""];
    }
    pthread_mutex_unlock(&_lock);
    return out;
}

- (NSString *)statusLine {
    pthread_mutex_lock(&_lock);
    uint64_t retired = _retired;
    double rate = _rate;
    NSString *status = _status;
    NSString *mode = _mode;
    double fps = _fps;
    BOOL haveFps = _fbSignatureValid;
    BOOL haveNetwork = _networkStatusValid;
    vm_network_status_t networkStatus = _networkStatus;
    double networkDownRate = _networkDownRate;
    double networkUpRate = _networkUpRate;
    pthread_mutex_unlock(&_lock);

    /* "--" until a frame has actually been published. Printing "0 fps" before
     * anything has rendered would report a stall that is not happening. */
    NSString *fpsText = haveFps ? [NSString stringWithFormat:@"%.0f fps", fps]
                                : @"-- fps";
    double footprintMB = [VMEngine physFootprintBytes] / 1048576.0;
    NSString *networkText = @"";
    if (haveNetwork) {
        NSString *phase = networkStatus.ipcp_open ? @"IP"
                          : networkStatus.lcp_open ? @"LCP"
                          : networkStatus.peer_opened ? @"NEG"
                          : @"idle";
        /* The rates alone can say only "slow". These cumulative boundary
         * counters say WHERE a transfer stopped: guest UART/PPP, IP/TCP, or
         * the ordinary host socket. Keeping the last errno next to the error
         * count prevents a denied connect from looking like an idle link. */
        networkText = [NSString stringWithFormat:
            @"  ·  net %@ D %.2f U %.2f MB/s  ·  ppp g2h %llu h2g %llu B bad %llu ovf %llu  ·  ip g2h %llu h2g %llu  ·  tcp %llu rst in/out %llu/%llu rtx %llu ooo %llu drop/q %llu/%u  ·  live %u st %u gp %u dp %u wnd %u mss %u cwnd %u ss %u fly %u tx %u rx %u rtx %u wait %u fl %x  ·  abort syn/con/send/recv/rtx %llu/%llu/%llu/%llu/%llu last %u st %u wnd %u fly %u buf %u rtx %u  ·  peer-rst st %u wnd %u fly %u buf %u rtx %u  ·  dns %llu/%llu  ·  host out %llu in %llu B err %llu:%d  ·  urx push/read/drop/under %llu/%llu/%llu/%llu  ·  ureg u/f/q/p/t %08x/%08x/%u/%08x/%u  ·  dma0 r/w/b/i/c/bad %llu/%llu/%llu/%llu/%llu/%llu  ·  dma1 r/w/b/i/c/bad %llu/%llu/%llu/%llu/%llu/%llu  ·  dtx %@  ·  drx %@",
            phase, networkDownRate / 1.0e6, networkUpRate / 1.0e6,
            (unsigned long long)networkStatus.guest_tx_bytes,
            (unsigned long long)networkStatus.guest_rx_bytes,
            (unsigned long long)networkStatus.ppp_fcs_errors,
            (unsigned long long)networkStatus.ppp_tx_overflows,
            (unsigned long long)networkStatus.ip_in,
            (unsigned long long)networkStatus.ip_out,
            (unsigned long long)networkStatus.tcp_established,
            (unsigned long long)networkStatus.tcp_resets_in,
            (unsigned long long)networkStatus.tcp_resets_out,
            (unsigned long long)networkStatus.tcp_retransmits,
            (unsigned long long)networkStatus.tcp_out_of_order,
            (unsigned long long)networkStatus.tcp_output_dropped,
            networkStatus.tcp_output_pending,
            networkStatus.tcp_live_flows,
            networkStatus.tcp_live_state,
            (unsigned)networkStatus.tcp_live_guest_port,
            (unsigned)networkStatus.tcp_live_dst_port,
            networkStatus.tcp_live_window,
            (unsigned)networkStatus.tcp_live_mss,
            networkStatus.tcp_live_congestion_window,
            networkStatus.tcp_live_slow_start_threshold,
            networkStatus.tcp_live_inflight,
            networkStatus.tcp_live_tx_buffered,
            networkStatus.tcp_live_rx_buffered,
            networkStatus.tcp_live_retries,
            networkStatus.tcp_live_rto_remaining_ms,
            networkStatus.tcp_live_flags,
            (unsigned long long)networkStatus.tcp_aborts_unexpected_syn,
            (unsigned long long)networkStatus.tcp_aborts_connect,
            (unsigned long long)networkStatus.tcp_aborts_host_send,
            (unsigned long long)networkStatus.tcp_aborts_host_recv,
            (unsigned long long)networkStatus.tcp_aborts_retransmit,
            networkStatus.tcp_last_abort_reason,
            networkStatus.tcp_last_abort_state,
            networkStatus.tcp_last_abort_window,
            networkStatus.tcp_last_abort_inflight,
            networkStatus.tcp_last_abort_buffered,
            networkStatus.tcp_last_abort_retries,
            networkStatus.tcp_last_peer_reset_state,
            networkStatus.tcp_last_peer_reset_window,
            networkStatus.tcp_last_peer_reset_inflight,
            networkStatus.tcp_last_peer_reset_buffered,
            networkStatus.tcp_last_peer_reset_retries,
            (unsigned long long)networkStatus.dns_queries,
            (unsigned long long)networkStatus.dns_answered,
            (unsigned long long)networkStatus.host_bytes_out,
            (unsigned long long)networkStatus.host_bytes_in,
            (unsigned long long)networkStatus.host_errors,
            networkStatus.host_last_error,
            (unsigned long long)networkStatus.uart4_rx_pushed,
            (unsigned long long)networkStatus.uart4_rx_reads,
            (unsigned long long)networkStatus.uart4_rx_dropped,
            (unsigned long long)networkStatus.uart4_rx_underruns,
            networkStatus.uart4_ucon,
            networkStatus.uart4_ufcon,
            (unsigned)networkStatus.uart4_rx_count,
            networkStatus.uart4_utrstat_pending,
            (unsigned)networkStatus.uart4_rx_timeout_state,
            (unsigned long long)networkStatus.dmac_reads[0],
            (unsigned long long)networkStatus.dmac_writes[0],
            (unsigned long long)networkStatus.dmac_bytes[0],
            (unsigned long long)networkStatus.dmac_items[0],
            (unsigned long long)networkStatus.dmac_completions[0],
            (unsigned long long)(networkStatus.dmac_refused_flow[0] +
                networkStatus.dmac_refused_width[0] +
                networkStatus.dmac_refused_chain[0]),
            (unsigned long long)networkStatus.dmac_reads[1],
            (unsigned long long)networkStatus.dmac_writes[1],
            (unsigned long long)networkStatus.dmac_bytes[1],
            (unsigned long long)networkStatus.dmac_items[1],
            (unsigned long long)networkStatus.dmac_completions[1],
            (unsigned long long)(networkStatus.dmac_refused_flow[1] +
                networkStatus.dmac_refused_width[1] +
                networkStatus.dmac_refused_chain[1]),
            networkStatus.dma_guest_tx.found
                ? [NSString stringWithFormat:@"%u:%u cfg%08x ctl%08x run%llu b%llu",
                    networkStatus.dma_guest_tx.controller,
                    networkStatus.dma_guest_tx.channel,
                    networkStatus.dma_guest_tx.cfg,
                    networkStatus.dma_guest_tx.ctrl,
                    (unsigned long long)networkStatus.dma_guest_tx.runs,
                    (unsigned long long)networkStatus.dma_guest_tx.bytes]
                : @"none",
            networkStatus.dma_guest_rx.found
                ? [NSString stringWithFormat:@"%u:%u cfg%08x ctl%08x run%llu b%llu",
                    networkStatus.dma_guest_rx.controller,
                    networkStatus.dma_guest_rx.channel,
                    networkStatus.dma_guest_rx.cfg,
                    networkStatus.dma_guest_rx.ctrl,
                    (unsigned long long)networkStatus.dma_guest_rx.runs,
                    (unsigned long long)networkStatus.dma_guest_rx.bytes]
                : @"none"];
    }
    if (haveNetwork && networkStatus.packet_offload) {
        networkText = [networkText stringByAppendingFormat:
            @"  | bulk tx/rx %llu/%llu pkt %llu/%llu B stale/fail %llu/%llu batch/extra %llu/%llu merged %llu",
            (unsigned long long)networkStatus.bulk_tx_packets,
            (unsigned long long)networkStatus.bulk_rx_packets,
            (unsigned long long)networkStatus.bulk_tx_bytes,
            (unsigned long long)networkStatus.bulk_rx_bytes,
            (unsigned long long)networkStatus.bulk_stale_tokens,
            (unsigned long long)networkStatus.bulk_failures,
            (unsigned long long)networkStatus.bulk_batches,
            (unsigned long long)networkStatus.bulk_batched_packets,
            (unsigned long long)networkStatus.bulk_merged_packets];
    }
    /* The mode leads, because "3.2 M insn/s" means something different
     * depending on what is retiring them, and a user who cannot see which
     * guest is running has no way to tell. */
    return [NSString stringWithFormat:
            @"%@%@  ·  %.1f M insn  ·  %.2f M insn/s  ·  %@  ·  %.0f MB%@",
            mode.length ? [mode stringByAppendingString:@"  ·  "] : @"",
            status, retired / 1.0e6, rate / 1.0e6, fpsText, footprintMB,
            networkText];
}

- (NSString *)statusDescription {
    pthread_mutex_lock(&_lock);
    NSString *status = [_status copy];
    pthread_mutex_unlock(&_lock);
    return status.length ? status : @"unknown";
}

- (NSString *)modeDescription {
    pthread_mutex_lock(&_lock);
    NSString *mode = _mode;
    pthread_mutex_unlock(&_lock);
    return mode.length ? mode : nil;
}

- (BOOL)isRunningFirmware {
    /* Deliberately derived from the same string the UI shows rather than from
     * a second flag: two sources of truth is how a UI ends up claiming one
     * thing in the status bar and another in an alert. */
    pthread_mutex_lock(&_lock);
    BOOL firmware = _firmwareBoot != NULL && _mode.length > 0 &&
                    ![_mode isEqualToString:@"built-in test guest"];
    pthread_mutex_unlock(&_lock);
    return firmware;
}

- (NSString *)bringUpNote {
    pthread_mutex_lock(&_lock);
    NSString *note = _bringUpNote;
    pthread_mutex_unlock(&_lock);
    return note.length ? note : nil;
}

- (BOOL)isPreparingRootFilesystem {
    pthread_mutex_lock(&_lock);
    BOOL preparing = _preparingRootFS;
    pthread_mutex_unlock(&_lock);
    return preparing;
}

- (double)rootFilesystemProgress {
    pthread_mutex_lock(&_lock);
    double f = _prepareTotal ? (double)_prepareDone / (double)_prepareTotal
                             : -1.0;
    pthread_mutex_unlock(&_lock);
    return f;
}

/*
 * Called from rootfs_work.c's copy loop, on the PROVISIONING thread -- never
 * the UI one. It therefore does the least possible: takes the same lock every
 * other engine field uses and stores two integers. The view polls; nothing is
 * dispatched from here, because a callback that hopped to the main queue a few
 * hundred times would be a UI storm reporting on a disk copy.
 */
static void vm_engine_prepare_progress(void *ctx, uint64_t done,
                                       uint64_t total) {
    VMEngine *engine = (__bridge VMEngine *)ctx;
    if (!engine) return;
    /* Direct ivar access rather than a message send: this is the same
     * translation unit, and a selector declared below the callback would need
     * a forward declaration for no benefit. Two stores under the lock every
     * engine field already uses. */
    pthread_mutex_lock(&engine->_lock);
    engine->_prepareDone  = done;
    engine->_prepareTotal = total;
    pthread_mutex_unlock(&engine->_lock);
}

/*
 * WHAT A MACHINE WOULD DO, asked without naming a machine.
 *
 * It therefore cannot answer the half that is per-machine. The work image
 * belongs to one machine and lives in that machine's directory, so this probes
 * the SHARED artefacts with an empty work directory -- which reports
 * NEEDS_WORK_IMAGE -- and words that as what it is: every machine prepares its
 * own on first open. Claiming READY here would be claiming a fact about a
 * machine the caller has not identified.
 */
+ (NSString *)firmwareReadinessSummary {
    NSString *directory = [[VMSettings sharedSettings] firmwareDirectory];
    char buffer[VM_FW_BOOT_PATH_CAPACITY];
    BOOL havePath = directory.length &&
        [directory getFileSystemRepresentation:buffer maxLength:sizeof buffer];
    vm_firmware_boot_paths_t paths;
    if (!havePath || !vm_firmware_boot_paths_split(&paths, buffer, ""))
        memset(&paths, 0, sizeof paths);

    vm_firmware_boot_state_t state;
    vm_firmware_boot_probe(&paths, &state);
    switch (state.readiness) {
    case VM_FW_BOOT_READY:
    case VM_FW_BOOT_NEEDS_WORK_IMAGE:
        return @"Firmware imported. Each machine prepares its own writable "
               @"root filesystem the first time you open it, then boots "
               @"iPhone OS 3.1.3.";
    case VM_FW_BOOT_INCOMPLETE:
    default:
        break;
    }
    return @"No firmware imported — runs the built-in test guest, which "
           @"exercises the processor, the serial port and the screen.";
}

@end
