// S5LBox -- a validated, privately staged guest application. MIT licensed.
#import <Foundation/Foundation.h>

@interface VMIPAPackage : NSObject
@property (nonatomic, readonly, copy) NSString *displayName;
@property (nonatomic, readonly, copy) NSString *bundleIdentifier;
@property (nonatomic, readonly, copy) NSString *bundleDirectoryName;
@property (nonatomic, readonly, copy) NSDictionary *infoDictionary;
/* Synchronous visitor over already validated, privately staged bundle data.
 * Relative paths and symlink targets retain their original archive semantics.
 * A data view is borrowed for the callback's duration, avoiding a second IPA
 * extraction or a whole-app memory copy for offline installation. */
- (BOOL)visitBundleFiles:(BOOL (^)(NSString *path,NSData *data,uint32_t mode))visitor;
- (instancetype)initWithURL:(NSURL *)url error:(NSError **)error;
/* Caller must keep every guest stopped until this returns. The independent
 * checkpoint witness is checked again here, not trusted from UI state. */
- (BOOL)installInDirectory:(NSString *)directory error:(NSError **)error;
@end
