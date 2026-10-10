// Atomic publication of a complete, already-authenticated firmware set.
#import <Foundation/Foundation.h>
NS_ASSUME_NONNULL_BEGIN
@interface VMFirmwareStore : NSObject
/* Legacy loose files remain supported. A small atomic pointer selects complete
 * new imports, so cancellation or process death cannot publish half a set. */
+ (NSString *)activeDirectoryInRoot:(NSString *)root;
+ (BOOL)hasPreparedFilesInDirectory:(NSString *)directory;
+ (nullable NSString *)createStagingDirectoryInRoot:(NSString *)root error:(NSError **)error;
+ (BOOL)publishDirectory:(NSString *)directory inRoot:(NSString *)root error:(NSError **)error;
@end
NS_ASSUME_NONNULL_END
