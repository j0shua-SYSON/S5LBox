/*
 * S5LBox — the snapshots belonging to one machine. See VMSnapshotStore.h.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#import "VMSnapshotStore.h"

static NSString *const kSnapshotDirName = @"Snapshots";
static NSString *const kSnapshotExt     = @"s5lsnap";
/* The companion file. JSON rather than a plist because it is three fields and
 * gets read by eye when something goes wrong. */
static NSString *const kMetaExt         = @"json";

@interface VMSnapshot ()
@property (nonatomic, copy) NSString *displayName;
@property (nonatomic, copy) NSString *path;
@property (nonatomic, strong) NSDate *created;
@property (nonatomic, assign) unsigned long long bytes;
@property (nonatomic, assign) BOOL automatic;
@end

@implementation VMSnapshot
@end

@implementation VMSnapshotStore {
    NSString *_dir;
}

+ (nullable instancetype)storeForWorkImagePath:(NSString *)workImagePath {
    if (workImagePath.length == 0) return nil;
    VMSnapshotStore *s = [[VMSnapshotStore alloc] init];
    if (!s) return nil;
    s->_dir = [[workImagePath stringByDeletingLastPathComponent]
               stringByAppendingPathComponent:kSnapshotDirName];
    return s;
}

- (BOOL)ensureDirectory {
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    if ([fm fileExistsAtPath:_dir isDirectory:&isDir]) return isDir;
    return [fm createDirectoryAtPath:_dir
         withIntermediateDirectories:YES
                          attributes:nil
                               error:NULL];
}

- (NSString *)metaPathFor:(NSString *)snapshotPath {
    return [[snapshotPath stringByDeletingPathExtension]
            stringByAppendingPathExtension:kMetaExt];
}

/*
 * A filename that is safe on this volume and still recognisable. The display
 * name is kept in the metadata, so this only has to be legal and unique — it is
 * never parsed back into a name.
 */
- (NSString *)slugFor:(NSString *)displayName {
    NSMutableString *out = [NSMutableString string];
    NSCharacterSet *ok = [NSCharacterSet alphanumericCharacterSet];
    for (NSUInteger i = 0; i < displayName.length && out.length < 40; i++) {
        unichar c = [displayName characterAtIndex:i];
        if ([ok characterIsMember:c])      [out appendFormat:@"%C", c];
        else if (out.length && ![out hasSuffix:@"-"]) [out appendString:@"-"];
    }
    while ([out hasSuffix:@"-"])
        [out deleteCharactersInRange:NSMakeRange(out.length - 1, 1)];
    return out.length ? out : @"snapshot";
}

- (nullable NSString *)pathForNewSnapshotNamed:(NSString *)displayName
                                     automatic:(BOOL)automatic {
    if (![self ensureDirectory]) return nil;
    NSString *name = displayName.length ? displayName : @"Snapshot";
    NSString *slug = [self slugFor:name];
    NSFileManager *fm = [NSFileManager defaultManager];

    /*
     * Uniqueness by trying, not by asking. Checking for a free name and then
     * using it is a race with the automatic timer, which runs on another
     * thread; the metadata is written here and now, so the winner of a tie is
     * whoever wrote first and the loser simply takes the next number.
     */
    for (unsigned n = 0; n < 10000u; n++) {
        NSString *base = n ? [NSString stringWithFormat:@"%@-%u", slug, n] : slug;
        NSString *path = [[_dir stringByAppendingPathComponent:base]
                          stringByAppendingPathExtension:kSnapshotExt];
        NSString *meta = [self metaPathFor:path];
        if ([fm fileExistsAtPath:path] || [fm fileExistsAtPath:meta]) continue;

        NSDictionary *d = @{ @"name"      : name,
                             @"created"   : @([[NSDate date] timeIntervalSince1970]),
                             @"automatic" : @(automatic) };
        NSData *json = [NSJSONSerialization dataWithJSONObject:d
                                                       options:0
                                                         error:NULL];
        /* withIntermediateDirectories is irrelevant here; what matters is that
         * the metadata exists BEFORE the machine state, so a save that is
         * interrupted leaves a listed-but-empty snapshot rather than an
         * unlisted file nobody can name or delete from the UI. */
        if (!json || ![json writeToFile:meta atomically:YES]) return nil;
        return path;
    }
    return nil;
}

- (NSArray<VMSnapshot *> *)snapshots {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSArray<NSString *> *names = [fm contentsOfDirectoryAtPath:_dir error:NULL];
    NSMutableArray<VMSnapshot *> *out = [NSMutableArray array];

    for (NSString *entry in names) {
        if (![entry.pathExtension isEqualToString:kSnapshotExt]) continue;
        NSString *path = [_dir stringByAppendingPathComponent:entry];
        NSString *meta = [self metaPathFor:path];

        VMSnapshot *s = [[VMSnapshot alloc] init];
        s.path = path;

        NSData *json = [NSData dataWithContentsOfFile:meta];
        NSDictionary *d = json ? [NSJSONSerialization JSONObjectWithData:json
                                                                 options:0
                                                                   error:NULL]
                               : nil;
        if ([d isKindOfClass:[NSDictionary class]]) {
            id name = d[@"name"];
            s.displayName = [name isKindOfClass:[NSString class]]
                          ? name : entry.stringByDeletingPathExtension;
            id when = d[@"created"];
            s.created = [when isKindOfClass:[NSNumber class]]
                      ? [NSDate dateWithTimeIntervalSince1970:
                            [when doubleValue]]
                      : nil;
            s.automatic = [d[@"automatic"] boolValue];
        } else {
            /* A snapshot whose metadata is missing or corrupt is still a real
             * machine state and must remain restorable and deletable. It is
             * named after its file and treated as user-made, because the one
             * thing that must not happen is the pruner deleting something it
             * cannot identify. */
            s.displayName = entry.stringByDeletingPathExtension;
            s.automatic = NO;
        }

        NSDictionary *attrs = [fm attributesOfItemAtPath:path error:NULL];
        s.bytes = attrs.fileSize;
        if (!s.created) s.created = attrs.fileModificationDate ?: [NSDate date];
        [out addObject:s];
    }

    [out sortUsingComparator:^NSComparisonResult(VMSnapshot *a, VMSnapshot *b) {
        return [b.created compare:a.created];      /* newest first */
    }];
    return out;
}

- (BOOL)renameSnapshot:(VMSnapshot *)snapshot to:(NSString *)displayName {
    if (!snapshot || displayName.length == 0) return NO;
    NSString *meta = [self metaPathFor:snapshot.path];
    NSData *json = [NSData dataWithContentsOfFile:meta];
    NSMutableDictionary *d =
        [([NSJSONSerialization JSONObjectWithData:(json ?: [NSData data])
                                          options:0
                                            error:NULL] ?: @{}) mutableCopy];
    d[@"name"] = displayName;
    if (!d[@"created"])
        d[@"created"] = @([snapshot.created timeIntervalSince1970]);
    if (!d[@"automatic"]) d[@"automatic"] = @(snapshot.automatic);

    NSData *out = [NSJSONSerialization dataWithJSONObject:d options:0 error:NULL];
    if (!out || ![out writeToFile:meta atomically:YES]) return NO;
    snapshot.displayName = displayName;
    return YES;
}

- (BOOL)deleteSnapshot:(VMSnapshot *)snapshot {
    if (!snapshot) return NO;
    NSFileManager *fm = [NSFileManager defaultManager];
    /* Metadata last: if the machine state goes and the metadata survives, the
     * next listing shows a snapshot that cannot be restored. Better to lose the
     * name than to keep a ghost. */
    BOOL ok = [fm removeItemAtPath:snapshot.path error:NULL];
    (void)[fm removeItemAtPath:[self metaPathFor:snapshot.path] error:NULL];
    return ok;
}

- (NSUInteger)pruneAutomaticSnapshotsKeeping:(NSUInteger)keep {
    NSArray<VMSnapshot *> *all = [self snapshots];      /* newest first */
    NSMutableArray<VMSnapshot *> *autos = [NSMutableArray array];
    for (VMSnapshot *s in all) if (s.automatic) [autos addObject:s];
    if (autos.count <= keep) return 0;

    NSUInteger gone = 0;
    for (NSUInteger i = keep; i < autos.count; i++)
        if ([self deleteSnapshot:autos[i]]) gone++;
    return gone;
}

@end
