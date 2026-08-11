//
//  S5LBox — VMInstanceStore. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMInstanceStore.h"

#import "VMFirmwareBoot.h"
#import "VMOptions.h"
#import "VMSettings.h"

NSString *const VMInstanceStoreDidChangeNotification =
    @"VMInstanceStoreDidChangeNotification";

static NSString *const kStoreFile = @"machines.txt";
static NSString *const kGraphicsRecordFile = @".graphics-v1";
static NSString *const kErrorDomain = @"VMInstanceStore";
static const NSInteger kGraphicsRecordError = 1000;

/* realpath(), PATH_MAX. Declarations must precede @implementation --
 * Objective-C takes only method definitions inside one. */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static NSString *VMGraphicsRecordText(BOOL mbxEnabled,
                                      BOOL softwareRendererEnabled) {
    return [NSString stringWithFormat:
        @"s5lbox-machine-graphics 1\nmbx %u\nca-software-render %u\n",
        mbxEnabled ? 1u : 0u, softwareRendererEnabled ? 1u : 0u];
}

@interface VMInstanceStore ()
- (BOOL)recordedGraphicsForInstanceWithID:(NSString *)identifier
                               mbxEnabled:(BOOL *)mbxEnabled
                  softwareRendererEnabled:(BOOL *)softwareRendererEnabled;
- (BOOL)writeRecordedGraphicsForInstanceWithID:(NSString *)identifier
                                    mbxEnabled:(BOOL)mbxEnabled
                       softwareRendererEnabled:(BOOL)softwareRendererEnabled
                                         error:(NSError **)error;
@end


@implementation VMInstanceStore {
    vm_instance_list_t _list;
}

+ (instancetype)sharedStore {
    static VMInstanceStore *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [[VMInstanceStore alloc] init]; });
    return shared;
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    vm_instance_list_reset(&_list);
    [self load];
    return self;
}

#pragma mark - Paths

/*
 * WHERE MACHINES LIVE, and it moved.
 *
 * They were under Application Support, which the Files app does not show. That
 * was defensible while nothing was user-visible, and stopped being defensible
 * the moment UIFileSharingEnabled went in: the argument for that switch was
 * that a work image you cannot copy off the device is a VM you cannot back up,
 * and Application Support is exactly where you cannot copy it from. Worse, the
 * launch code then created an EMPTY Documents/Machines beside it, so a user
 * looking for a machine found a folder that was correct in name and would
 * never contain anything.
 *
 * They are in Documents/Machines now: visible, copyable, deletable, and where
 * this app has already told people to look.
 */
- (NSString *)containerDirectory {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray<NSString *> *docs =
        NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                            NSUserDomainMask, YES);
    NSString *base = docs.firstObject ?: NSTemporaryDirectory();
    NSString *dir = [base stringByAppendingPathComponent:@"Machines"];
    [fm createDirectoryAtPath:dir
  withIntermediateDirectories:YES
                   attributes:nil
                        error:NULL];

    /*
     * Anything already under the old root is moved across once. Item by item
     * rather than by renaming the directory, because the new one already
     * exists -- the launch path creates it so Files has somewhere to drop an
     * IPSW -- and a rename onto an existing directory fails.
     *
     * Nothing is overwritten: a name that is already in the new location wins,
     * because it is the one this build has been using. Anything left behind
     * stays where it is rather than being deleted, so a failed move loses
     * nothing.
     */
    static dispatch_once_t migrateOnce;
    dispatch_once(&migrateOnce, ^{
        NSArray<NSString *> *support =
            NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                                NSUserDomainMask, YES);
        NSString *oldDir = [support.firstObject
            stringByAppendingPathComponent:@"Machines"];
        if (oldDir.length == 0 || ![fm fileExistsAtPath:oldDir]) return;

        for (NSString *name in [fm contentsOfDirectoryAtPath:oldDir error:NULL]) {
            NSString *from = [oldDir stringByAppendingPathComponent:name];
            NSString *to   = [dir stringByAppendingPathComponent:name];
            if ([fm fileExistsAtPath:to]) continue;
            (void)[fm moveItemAtPath:from toPath:to error:NULL];
        }
        /* Only if it emptied. A leftover means a move failed and the data is
         * still there to be recovered by hand. */
        if ([fm contentsOfDirectoryAtPath:oldDir error:NULL].count == 0)
            (void)[fm removeItemAtPath:oldDir error:NULL];
    });

    /*
     * PHYSICALLY RESOLVED, for the same reason VMSettings' documents directory
     * is, and it must happen AFTER the directory exists because realpath()
     * resolves a path that is really there.
     *
     * This is the DESTINATION half. On iOS this begins /var/mobile/..., and
     * /var is a symlink to /private/var; rootfs_work.c walks every component
     * with AT_SYMLINK_NOFOLLOW and refuses any that is a link. Fixing only the
     * source path would have moved the same refusal from "unsafe-path at
     * source-path" to "unsafe-path at destination-path" and looked like a
     * different bug.
     */
    char buf[PATH_MAX];
    const char *in = [dir fileSystemRepresentation];
    if (in && realpath(in, buf)) {
        NSString *real = [[NSFileManager defaultManager]
            stringWithFileSystemRepresentation:buf length:strlen(buf)];
        if (real.length) return real;
    }
    return dir;
}

- (NSString *)storePath {
    return [[self containerDirectory] stringByAppendingPathComponent:kStoreFile];
}

- (NSString *)machinesDirectory {
    return [self containerDirectory];
}

- (NSString *)directoryForInstanceWithID:(NSString *)identifier {
    NSString *dir =
        [[self containerDirectory] stringByAppendingPathComponent:identifier];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                             withIntermediateDirectories:YES
                                              attributes:nil
                                                   error:NULL];
    return dir;
}

- (NSString *)graphicsRecordPathForInstanceWithID:(NSString *)identifier {
    NSString *dir =
        [[self containerDirectory] stringByAppendingPathComponent:identifier];
    return [dir stringByAppendingPathComponent:kGraphicsRecordFile];
}

- (BOOL)writeRecordedGraphicsForInstanceWithID:(NSString *)identifier
                                    mbxEnabled:(BOOL)mbxEnabled
                       softwareRendererEnabled:(BOOL)softwareRendererEnabled
                                         error:(NSError **)error {
    NSString *dir = [self directoryForInstanceWithID:identifier];
    NSString *path = [dir stringByAppendingPathComponent:kGraphicsRecordFile];
    NSError *writeError = nil;
    BOOL ok = [VMGraphicsRecordText(mbxEnabled, softwareRendererEnabled)
        writeToFile:path
         atomically:YES
           encoding:NSUTF8StringEncoding
              error:&writeError];
    if (!ok && error) {
        NSString *why = writeError.localizedDescription
            ?: @"the graphics record could not be written";
        *error = [NSError errorWithDomain:kErrorDomain
                                     code:kGraphicsRecordError
                                 userInfo:@{ NSLocalizedDescriptionKey: why }];
    }
    return ok;
}

- (BOOL)recordedGraphicsForInstanceWithID:(NSString *)identifier
                               mbxEnabled:(BOOL *)mbxEnabled
                  softwareRendererEnabled:(BOOL *)softwareRendererEnabled {
    if ([self indexOfID:identifier] < 0) return NO;

    NSString *text = [NSString stringWithContentsOfFile:
        [self graphicsRecordPathForInstanceWithID:identifier]
                                             encoding:NSUTF8StringEncoding
                                                error:NULL];
    if (!text) return NO;

    /* Accept only one of the four canonical records. A damaged or hand-edited
     * marker is not permission to guess at an image-time renderer. */
    for (unsigned mbx = 0; mbx <= 1u; mbx++) {
        for (unsigned software = 0; software <= 1u; software++) {
            if (![text isEqualToString:
                    VMGraphicsRecordText(mbx != 0u, software != 0u)])
                continue;
            if (mbxEnabled) *mbxEnabled = mbx != 0u;
            if (softwareRendererEnabled)
                *softwareRendererEnabled = software != 0u;
            return YES;
        }
    }
    NSLog(@"[instances] %@ has an unreadable graphics record; using legacy "
           "app-wide settings", identifier);
    return NO;
}

- (BOOL)graphicsForOpeningInstanceWithID:(NSString *)identifier
                              mbxEnabled:(BOOL *)mbxEnabled
                 softwareRendererEnabled:(BOOL *)softwareRendererEnabled
                                   error:(NSError **)error {
    if ([self indexOfID:identifier] < 0) {
        if (error) *error = [self errorFor:VM_INSTANCE_ERR_RANGE];
        return NO;
    }

    NSString *recordPath =
        [self graphicsRecordPathForInstanceWithID:identifier];
    BOOL recordExists =
        [[NSFileManager defaultManager] fileExistsAtPath:recordPath];
    BOOL recordedMBX = NO, recordedSoftware = NO;
    if ([self recordedGraphicsForInstanceWithID:identifier
                                     mbxEnabled:&recordedMBX
                        softwareRendererEnabled:&recordedSoftware]) {
        if (mbxEnabled) *mbxEnabled = recordedMBX;
        if (softwareRendererEnabled)
            *softwareRendererEnabled = recordedSoftware;
        return YES;
    }

    if (recordExists) {
        if (error) {
            *error = [NSError errorWithDomain:kErrorDomain
                                         code:kGraphicsRecordError
                                     userInfo:@{
                NSLocalizedDescriptionKey:
                    @"This machine's graphics record is unreadable. It was "
                     "not started because guessing could mismatch its image."
            }];
        }
        return NO;
    }

    NSString *workName =
        [NSString stringWithUTF8String:VM_FW_BOOT_WORK_FILE];
    NSString *workPath = [[[self containerDirectory]
        stringByAppendingPathComponent:identifier]
        stringByAppendingPathComponent:workName];
    if ([[NSFileManager defaultManager] fileExistsAtPath:workPath]) {
        /* Deliberate legacy path. Its old option bits cannot tell us which
         * renderer prepared this existing file. Preserve global behaviour. */
        return NO;
    }

    int mbx = vm_option_index("mbx");
    int ca = vm_option_index("ca-software-render");
    if (mbx < 0 || ca < 0 ||
        (unsigned)mbx >= VM_INSTANCE_OPTION_MAX ||
        (unsigned)ca >= VM_INSTANCE_OPTION_MAX) {
        if (error) {
            *error = [NSError errorWithDomain:kErrorDomain
                                         code:kGraphicsRecordError
                                     userInfo:@{
                NSLocalizedDescriptionKey:
                    @"The graphics option table is incomplete."
            }];
        }
        return NO;
    }

    VMSettings *settings = [VMSettings sharedSettings];
    recordedMBX =
        [settings valueForNewMachineOptionIndex:(NSUInteger)mbx];
    recordedSoftware =
        [settings valueForNewMachineOptionIndex:(NSUInteger)ca];
    NSError *writeError = nil;
    if (![self writeRecordedGraphicsForInstanceWithID:identifier
                                           mbxEnabled:recordedMBX
                              softwareRendererEnabled:recordedSoftware
                                                error:&writeError]) {
        if (error) *error = writeError;
        return NO;
    }

    int row = [self indexOfID:identifier];
    if (row >= 0) {
        _list.slot[row].options[mbx] = recordedMBX ? true : false;
        _list.slot[row].options[ca] = recordedSoftware ? true : false;
        [self changed];
    }
    if (mbxEnabled) *mbxEnabled = recordedMBX;
    if (softwareRendererEnabled)
        *softwareRendererEnabled = recordedSoftware;
    return YES;
}

#pragma mark - Persistence

- (void)load {
    NSString *text = [NSString stringWithContentsOfFile:[self storePath]
                                               encoding:NSUTF8StringEncoding
                                                  error:NULL];
    if (!text) {
        /* No file is a first launch, not a failure. */
        vm_instance_list_reset(&_list);
        return;
    }
    vm_instance_status_t s =
        vm_instance_deserialize(&_list, text.UTF8String);
    if (s != VM_INSTANCE_OK) {
        /*
         * The C layer has already emptied the list rather than returning a
         * partial one. Move the unreadable file aside instead of overwriting
         * it: whatever went wrong, the user's machine list is the last thing
         * that should be silently destroyed on the way to a working app.
         */
        NSString *aside = [[self storePath] stringByAppendingPathExtension:@"unreadable"];
        [[NSFileManager defaultManager] removeItemAtPath:aside error:NULL];
        [[NSFileManager defaultManager] moveItemAtPath:[self storePath]
                                                toPath:aside
                                                 error:NULL];
        NSLog(@"[instances] %s; the previous file was kept as %@",
              vm_instance_status_text(s), aside.lastPathComponent);
        vm_instance_list_reset(&_list);
    }
}

- (BOOL)save {
    size_t need = vm_instance_serialize(&_list, NULL, 0);
    char *buf = malloc(need + 1u);
    if (!buf) return NO;
    vm_instance_serialize(&_list, buf, need + 1u);
    NSString *text = [[NSString alloc] initWithBytesNoCopy:buf
                                                    length:need
                                                  encoding:NSUTF8StringEncoding
                                              freeWhenDone:YES];
    if (!text) { free(buf); return NO; }

    /* atomically:YES is the whole point — it writes a temporary and renames,
     * so a kill mid-save leaves the previous list rather than a truncated one
     * the parser would (correctly) refuse in full. */
    NSError *err = nil;
    BOOL ok = [text writeToFile:[self storePath]
                     atomically:YES
                       encoding:NSUTF8StringEncoding
                          error:&err];
    if (!ok) NSLog(@"[instances] could not save: %@", err.localizedDescription);
    return ok;
}

- (void)changed {
    [self save];
    [[NSNotificationCenter defaultCenter]
        postNotificationName:VMInstanceStoreDidChangeNotification object:self];
}

#pragma mark - Errors

- (NSError *)errorFor:(vm_instance_status_t)status {
    /* Guarded rather than @()-boxed: the boxing syntax goes through
     * +stringWithUTF8String:, which returns nil for anything that is not valid
     * UTF-8, and a nil value in the dictionary literal below raises. Every
     * string this can return today is ASCII, which is exactly the argument
     * that stops being true the day somebody adds a status. */
    NSString *why = [NSString stringWithUTF8String:vm_instance_status_text(status)]
                  ?: @"the machine list refused, without saying why";
    return [NSError errorWithDomain:kErrorDomain
                               code:(NSInteger)status
                           userInfo:@{ NSLocalizedDescriptionKey: why }];
}

- (BOOL)report:(vm_instance_status_t)status to:(NSError **)error {
    if (status == VM_INSTANCE_OK) return YES;
    if (error) *error = [self errorFor:status];
    return NO;
}

#pragma mark - Reading

- (NSUInteger)count { return _list.count; }

- (NSDictionary<NSString *, id> *)instanceAtIndex:(NSUInteger)index {
    if (index >= _list.count) return nil;
    const vm_instance_t *row = vm_instance_at(&_list, (unsigned)index);
    if (!row) return nil;
    return @{
        @"id":       @(row->id),
        @"name":     @(row->name),
        @"created":  @(row->created_unix),
        @"opened":   @(row->last_opened_unix),
        @"retired":  @(row->retired_total),
    };
}

- (int)indexOfID:(NSString *)identifier {
    if (!identifier) return -1;
    return vm_instance_index_of_id(&_list, identifier.UTF8String);
}

#pragma mark - Writing

/* 16 lower-case hex digits, from the system CSPRNG. Generated here so nothing
 * outside this file has to know the shape the C layer requires. */
- (NSString *)freshIdentifier {
    for (unsigned attempt = 0; attempt < 8u; attempt++) {
        uint32_t hi = arc4random(), lo = arc4random();
        NSString *candidate = [NSString stringWithFormat:@"%08x%08x", hi, lo];
        if (vm_instance_index_of_id(&_list, candidate.UTF8String) < 0)
            return candidate;
    }
    return nil;    /* eight collisions on 64 bits is not luck, it is a bug */
}

- (NSString *)createInstanceNamed:(NSString *)name error:(NSError **)error {
    NSString *identifier = [self freshIdentifier];
    if (!identifier) {
        if (error) *error = [self errorFor:VM_INSTANCE_ERR_ID_TAKEN];
        return nil;
    }
    /* Snapshot what Settings ACTUALLY says for a new machine. The old path
     * copied compile-time table defaults here while VMEngine read NSUserDefaults
     * at launch, making machines.txt look authoritative when it was not. */
    bool defaults[VM_INSTANCE_OPTION_MAX];
    memset(defaults, 0, sizeof defaults);
    unsigned n = vm_option_count();
    if (n > VM_INSTANCE_OPTION_MAX) n = VM_INSTANCE_OPTION_MAX;
    VMSettings *settings = [VMSettings sharedSettings];
    for (unsigned i = 0; i < n; i++) {
        defaults[i] = [settings valueForNewMachineOptionIndex:i]
            ? true : false;
    }

    unsigned added = 0;
    vm_instance_status_t s =
        vm_instance_add(&_list, identifier.UTF8String, name.UTF8String,
                        defaults, VM_INSTANCE_OPTION_MAX,
                        (uint64_t)[NSDate date].timeIntervalSince1970, &added);
    if (![self report:s to:error]) return nil;

    int mbx = vm_option_index("mbx");
    int ca = vm_option_index("ca-software-render");
    if (mbx < 0 || ca < 0 ||
        (unsigned)mbx >= VM_INSTANCE_OPTION_MAX ||
        (unsigned)ca >= VM_INSTANCE_OPTION_MAX) {
        (void)vm_instance_remove(&_list, added);
        if (error) {
            *error = [NSError errorWithDomain:kErrorDomain
                                         code:kGraphicsRecordError
                                     userInfo:@{
                NSLocalizedDescriptionKey:
                    @"The graphics option table is incomplete."
            }];
        }
        return nil;
    }

    /* Do not write the ownership marker yet. The first open, not the list-row
     * creation, is when the image-time setting becomes immutable. This also
     * lets the automatically created first row follow a choice made before it
     * is opened. */
    [self changed];
    return identifier;
}

- (BOOL)renameInstanceAtIndex:(NSUInteger)index
                           to:(NSString *)name
                        error:(NSError **)error {
    vm_instance_status_t s =
        vm_instance_rename(&_list, (unsigned)index, name.UTF8String);
    if (![self report:s to:error]) return NO;
    [self changed];
    return YES;
}

- (NSString *)duplicateInstanceAtIndex:(NSUInteger)index error:(NSError **)error {
    NSDictionary *source = [self instanceAtIndex:index];
    if (!source) {
        if (error) *error = [self errorFor:VM_INSTANCE_ERR_RANGE];
        return nil;
    }
    NSString *identifier = [self freshIdentifier];
    if (!identifier) {
        if (error) *error = [self errorFor:VM_INSTANCE_ERR_ID_TAKEN];
        return nil;
    }

    /* Bound the copy's name to the same byte limit the C layer enforces,
     * truncating on a character boundary so a multi-byte name is never cut
     * mid-sequence into invalid UTF-8. */
    NSString *base = [NSString stringWithFormat:@"%@ copy", source[@"name"]];
    while ([base lengthOfBytesUsingEncoding:NSUTF8StringEncoding]
               > VM_INSTANCE_NAME_MAX && base.length > 1u)
        base = [base substringToIndex:base.length - 1u];

    /* A duplicate gets a fresh work image, so it must also get an exact
     * image-time graphics record. Copy a trustworthy source record. For a
     * legacy source whose historical bits are not evidence, use the current
     * new-machine setting rather than blessing those bits retroactively. */
    BOOL mbxEnabled = NO, softwareRendererEnabled = NO;
    if (![self recordedGraphicsForInstanceWithID:source[@"id"]
                                      mbxEnabled:&mbxEnabled
                         softwareRendererEnabled:&softwareRendererEnabled]) {
        VMSettings *settings = [VMSettings sharedSettings];
        int mbx = vm_option_index("mbx");
        int ca = vm_option_index("ca-software-render");
        if (mbx < 0 || ca < 0) {
            if (error) {
                *error = [NSError errorWithDomain:kErrorDomain
                                             code:kGraphicsRecordError
                                         userInfo:@{
                    NSLocalizedDescriptionKey:
                        @"The graphics option table is incomplete."
                }];
            }
            return nil;
        }
        mbxEnabled =
            [settings valueForNewMachineOptionIndex:(NSUInteger)mbx];
        softwareRendererEnabled =
            [settings valueForNewMachineOptionIndex:(NSUInteger)ca];
    }

    unsigned added = 0;
    vm_instance_status_t s =
        vm_instance_duplicate(&_list, (unsigned)index, identifier.UTF8String,
                              base.UTF8String,
                              (uint64_t)[NSDate date].timeIntervalSince1970,
                              &added);
    if (![self report:s to:error]) return nil;

    int mbx = vm_option_index("mbx");
    int ca = vm_option_index("ca-software-render");
    if (mbx < 0 || ca < 0 ||
        (unsigned)mbx >= VM_INSTANCE_OPTION_MAX ||
        (unsigned)ca >= VM_INSTANCE_OPTION_MAX) {
        (void)vm_instance_remove(&_list, added);
        if (error) {
            *error = [NSError errorWithDomain:kErrorDomain
                                         code:kGraphicsRecordError
                                     userInfo:@{
                NSLocalizedDescriptionKey:
                    @"The graphics option table is incomplete."
            }];
        }
        return nil;
    }
    _list.slot[added].options[mbx] = mbxEnabled ? true : false;
    _list.slot[added].options[ca] =
        softwareRendererEnabled ? true : false;

    NSError *graphicsError = nil;
    if (![self writeRecordedGraphicsForInstanceWithID:identifier
                                           mbxEnabled:mbxEnabled
                              softwareRendererEnabled:softwareRendererEnabled
                                                error:&graphicsError]) {
        (void)vm_instance_remove(&_list, added);
        NSString *dir = [[self containerDirectory]
            stringByAppendingPathComponent:identifier];
        (void)[[NSFileManager defaultManager] removeItemAtPath:dir error:NULL];
        if (error) *error = graphicsError;
        return nil;
    }
    [self changed];
    return identifier;
}

- (BOOL)deleteInstanceAtIndex:(NSUInteger)index error:(NSError **)error {
    NSDictionary *row = [self instanceAtIndex:index];
    vm_instance_status_t s = vm_instance_remove(&_list, (unsigned)index);
    if (![self report:s to:error]) return NO;

    /* The record is gone; take its files with it. Deliberately after the
     * record is removed, so a failure to delete files leaves an orphan
     * directory rather than a machine the list still shows but cannot run. */
    if (row[@"id"]) {
        NSString *dir = [[self containerDirectory]
                            stringByAppendingPathComponent:row[@"id"]];
        [[NSFileManager defaultManager] removeItemAtPath:dir error:NULL];
    }
    [self changed];
    return YES;
}

- (void)noteOpenedInstanceWithID:(NSString *)identifier {
    int i = [self indexOfID:identifier];
    if (i < 0) return;
    _list.slot[i].last_opened_unix =
        (uint64_t)[NSDate date].timeIntervalSince1970;
    [self changed];
}

- (void)addRetired:(uint64_t)retired toInstanceWithID:(NSString *)identifier {
    if (retired == 0u) return;
    int i = [self indexOfID:identifier];
    if (i < 0) return;
    /* Saturate rather than wrap: a lifetime counter that goes backwards is
     * worse than one that stops being precise at 1.8e19 instructions. */
    uint64_t *total = &_list.slot[i].retired_total;
    *total = (UINT64_MAX - *total < retired) ? UINT64_MAX : *total + retired;
    [self changed];
}

- (BOOL)optionValueAtIndex:(NSUInteger)optionIndex
         forInstanceWithID:(NSString *)identifier {
    int i = [self indexOfID:identifier];
    if (i < 0 || optionIndex >= VM_INSTANCE_OPTION_MAX) return NO;
    return _list.slot[i].options[optionIndex] ? YES : NO;
}

- (void)setOptionValue:(BOOL)value
               atIndex:(NSUInteger)optionIndex
     forInstanceWithID:(NSString *)identifier {
    int i = [self indexOfID:identifier];
    if (i < 0 || optionIndex >= VM_INSTANCE_OPTION_MAX) return;
    _list.slot[i].options[optionIndex] = value ? true : false;
    [self changed];
}

@end
