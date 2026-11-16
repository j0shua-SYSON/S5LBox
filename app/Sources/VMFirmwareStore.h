// Atomic publication of a complete firmware set and its import policy.
#import <Foundation/Foundation.h>
NS_ASSUME_NONNULL_BEGIN
@interface VMFirmwareStore : NSObject
/* Legacy loose files remain supported. A small atomic pointer selects complete
 * new imports, so cancellation or process death cannot publish half a set. */
+ (NSString *)activeDirectoryInRoot:(NSString *)root;
+ (BOOL)hasPreparedFilesInDirectory:(NSString *)directory;
+ (nullable NSString *)createStagingDirectoryInRoot:(NSString *)root error:(NSError **)error;
+ (BOOL)publishDirectory:(NSString *)directory inRoot:(NSString *)root error:(NSError **)error;
+ (BOOL)writeMetadata:(NSDictionary<NSString *, id> *)metadata inDirectory:(NSString *)directory error:(NSError **)error;
/* Imported generations are retained as a library. Identifiers are relative
 * names, never host paths. Existing machines keep a durable source record. */
+ (NSArray<NSDictionary<NSString *, id> *> *)firmwaresInRoot:(NSString *)root;
+ (NSString *)displayNameInDirectory:(NSString *)directory;
+ (BOOL)selectFirmware:(NSString *)identifier inRoot:(NSString *)root error:(NSError **)error;
+ (BOOL)removeFirmware:(NSString *)identifier inRoot:(NSString *)root error:(NSError **)error;
+ (nullable NSString *)firmwareForMachine:(NSString *)machine inRoot:(NSString *)root error:(NSError **)error;
+ (BOOL)pinFirmware:(nullable NSString *)identifier forMachine:(NSString *)machine
             inRoot:(NSString *)root error:(NSError **)error;
@end
NS_ASSUME_NONNULL_END
