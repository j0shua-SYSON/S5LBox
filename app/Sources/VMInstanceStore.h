//
//  S5LBox — the machine list, on disk.
//
//  VMInstances.c owns the list and everything that can be wrong about it; this
//  is the thin Objective-C layer that gives it a file and a place to live. It
//  holds no rules of its own: every validation, every refusal and the entire
//  persisted format come from the C, which is where they can be tested without
//  a device.
//
//  WRITES ARE ATOMIC, and that is the whole reason this class exists rather
//  than a couple of calls in a view controller. The list is written to a
//  temporary file and then renamed over the real one, so a process killed
//  mid-save leaves the previous list intact rather than a truncated file. The
//  parser refuses a truncated file outright — it will not return half a list —
//  which turns "killed while saving" into "lost the last change" instead of
//  "lost every machine".
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>

#import "VMInstances.h"

NS_ASSUME_NONNULL_BEGIN

/* Posted after any change that altered the stored list. */
extern NSString *const VMInstanceStoreDidChangeNotification;

@interface VMInstanceStore : NSObject

/* The one store the app uses. Loads on first access; a missing file is an
 * empty list and not an error, because that is what a first launch looks
 * like. */
+ (instancetype)sharedStore;

/* How many machines, and the row at an index (nil when out of range). */
- (NSUInteger)count;
- (nullable NSDictionary<NSString *, id> *)instanceAtIndex:(NSUInteger)index;

/*
 * Create a machine. The identifier is generated here, not by the caller, so
 * that nothing outside this class has to know it must be 16 lower-case hex
 * digits. Returns the new identifier, or nil with `error` describing which
 * refusal the C layer returned.
 */
- (nullable NSString *)createInstanceNamed:(NSString *)name
                                     error:(NSError **)error;

/* Rename, duplicate and delete. Each returns NO with `error` set on refusal,
 * and none of them changes anything when they refuse. */
- (BOOL)renameInstanceAtIndex:(NSUInteger)index
                           to:(NSString *)name
                        error:(NSError **)error;
- (nullable NSString *)duplicateInstanceAtIndex:(NSUInteger)index
                                          error:(NSError **)error;
- (BOOL)deleteInstanceAtIndex:(NSUInteger)index error:(NSError **)error;

/* Record that a machine was opened, and add to its lifetime instruction
 * count. Both are best-effort: a failure to persist them is not worth
 * refusing to run a machine over. */
- (void)noteOpenedInstanceWithID:(NSString *)identifier;
- (void)addRetired:(uint64_t)retired toInstanceWithID:(NSString *)identifier;

/*
 * Per-instance option values, in option-table order. `values` is read and
 * written by the settings screen; out-of-range indices are ignored rather
 * than growing the array.
 */
- (BOOL)optionValueAtIndex:(NSUInteger)optionIndex
      forInstanceWithID:(NSString *)identifier;
- (void)setOptionValue:(BOOL)value
               atIndex:(NSUInteger)optionIndex
     forInstanceWithID:(NSString *)identifier;

/*
 * Where this machine's mutable files belong — its work image and, later, its
 * snapshots. One directory per identifier, created on demand.
 *
 * This is now load-bearing rather than a placeholder: VMInstancePaths.c derives
 * <this>/rootfs-work.img and the engine boots that file, so two machines have
 * two root filesystems. -deleteInstanceAtIndex:error: removes the whole
 * directory, which is how a machine's ~465 MB disk is reclaimed.
 *
 * The IMPORTED artefacts are deliberately NOT here. They are read-only for
 * their whole life, so one shared copy in Documents/firmware is the right
 * number and a per-machine copy would multiply the largest thing on the device
 * for nothing.
 */
- (NSString *)directoryForInstanceWithID:(NSString *)identifier;

/*
 * The directory those per-machine directories sit in. Exposed because the path
 * derivation lives in C (VMInstancePaths.c) and takes the container plus an
 * identifier rather than a finished path — deriving the container by stripping
 * a component off the one above would work today and break silently the day
 * the layout gains a level.
 */
- (NSString *)machinesDirectory;

@end

NS_ASSUME_NONNULL_END
