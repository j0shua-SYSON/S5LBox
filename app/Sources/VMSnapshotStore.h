/*
 * S5LBox — the snapshots belonging to one machine.
 *
 * A machine may have as many as the disk will hold. Each is a file in a
 * Snapshots directory beside that machine's work image, so a machine and every
 * checkpoint of it are copied, backed up and deleted as one thing — and so the
 * Files app shows them where a user already looks for the machine.
 *
 * WHY THE NAME IS NOT THE FILENAME. A snapshot's display name is whatever the
 * user typed, including spaces, slashes, emoji and names that differ only by
 * case on a case-insensitive volume. The file it lives in is a sanitised form
 * of that, and the display name is stored INSIDE the file's companion metadata
 * rather than inferred back out of the filename. Renaming therefore never has
 * to move 130 MB of machine state, and two snapshots may legitimately share a
 * display name without one silently overwriting the other.
 *
 * Nothing here touches the machine. Saving and restoring are VMEngine's, because
 * only the emulator thread may read or write a running machine; this decides
 * WHERE and WHAT THEY ARE CALLED.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface VMSnapshot : NSObject
/* What the user sees and may change. */
@property (nonatomic, copy, readonly) NSString *displayName;
/* Where the machine state is. Absolute. */
@property (nonatomic, copy, readonly) NSString *path;
/* When it was taken, and how big it is, for a list the user can reason about. */
@property (nonatomic, strong, readonly) NSDate *created;
@property (nonatomic, assign, readonly) unsigned long long bytes;
/* Automatic snapshots are pruned; ones the user asked for never are. */
@property (nonatomic, assign, readonly) BOOL automatic;
@end

@interface VMSnapshotStore : NSObject

/* The store for one machine, derived from that machine's work-image path. Nil
 * if the machine has no work image yet — there is nothing to snapshot. */
+ (nullable instancetype)storeForWorkImagePath:(NSString *)workImagePath;

/* Newest first. Reads the directory every call: another process, the Files app
 * or the user may have deleted or added one since, and a cached list would
 * offer to restore something that is no longer there. */
- (NSArray<VMSnapshot *> *)snapshots;

/*
 * A path for a NEW snapshot with this display name. Always unique, even when
 * the display name is not: the caller hands this to VMEngine, which writes it
 * on the emulator thread some milliseconds later, and two saves requested in
 * the same second must not land on the same file.
 *
 * `automatic` marks it as prunable. Pass NO for anything the user asked for.
 */
- (nullable NSString *)pathForNewSnapshotNamed:(NSString *)displayName
                                     automatic:(BOOL)automatic;

/* Rename in place. Moves no machine state — only the companion metadata. */
- (BOOL)renameSnapshot:(VMSnapshot *)snapshot to:(NSString *)displayName;

- (BOOL)deleteSnapshot:(VMSnapshot *)snapshot;

/*
 * Delete the oldest AUTOMATIC snapshots until at most `keep` remain, and return
 * how many went.
 *
 * Only automatic ones, and that is the whole point of the flag. A retention
 * rule that could delete a checkpoint the user made by hand and named is not a
 * retention rule, it is data loss on a timer.
 */
- (NSUInteger)pruneAutomaticSnapshotsKeeping:(NSUInteger)keep;

@end

NS_ASSUME_NONNULL_END
