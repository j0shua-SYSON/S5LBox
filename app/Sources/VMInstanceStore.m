//
//  iOS3-VM — VMInstanceStore. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMInstanceStore.h"

#import "VMOptions.h"

NSString *const VMInstanceStoreDidChangeNotification =
    @"VMInstanceStoreDidChangeNotification";

static NSString *const kStoreFile = @"machines.txt";
static NSString *const kErrorDomain = @"VMInstanceStore";

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

- (NSString *)containerDirectory {
    NSArray<NSString *> *dirs =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,
                                            NSUserDomainMask, YES);
    NSString *base = dirs.firstObject ?: NSTemporaryDirectory();
    NSString *dir = [base stringByAppendingPathComponent:@"Machines"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                             withIntermediateDirectories:YES
                                              attributes:nil
                                                   error:NULL];
    return dir;
}

- (NSString *)storePath {
    return [[self containerDirectory] stringByAppendingPathComponent:kStoreFile];
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
    NSString *why = @(vm_instance_status_text(status));
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
    /* Defaults come from the option table, so a new machine matches what the
     * harness would do with no flags. */
    bool defaults[VM_INSTANCE_OPTION_MAX];
    memset(defaults, 0, sizeof defaults);
    unsigned n = vm_option_count();
    if (n > VM_INSTANCE_OPTION_MAX) n = VM_INSTANCE_OPTION_MAX;
    for (unsigned i = 0; i < n; i++) {
        const vm_option_t *o = vm_option_at(i);
        defaults[i] = o ? o->def : false;
    }

    vm_instance_status_t s =
        vm_instance_add(&_list, identifier.UTF8String, name.UTF8String,
                        defaults, VM_INSTANCE_OPTION_MAX,
                        (uint64_t)[NSDate date].timeIntervalSince1970, NULL);
    if (![self report:s to:error]) return nil;
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

    vm_instance_status_t s =
        vm_instance_duplicate(&_list, (unsigned)index, identifier.UTF8String,
                              base.UTF8String,
                              (uint64_t)[NSDate date].timeIntervalSince1970,
                              NULL);
    if (![self report:s to:error]) return nil;
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
