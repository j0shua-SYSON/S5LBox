// S5LBox -- a validated, privately staged guest application. MIT licensed.
#import <Foundation/Foundation.h>

@interface VMIPAPackage : NSObject
@property (nonatomic, readonly, copy) NSString *displayName;
@property (nonatomic, readonly, copy) NSString *bundleIdentifier;
- (instancetype)initWithURL:(NSURL *)url error:(NSError **)error;
/* Caller must keep every guest stopped until this returns. The independent
 * checkpoint witness is checked again here, not trusted from UI state. */
- (BOOL)installInDirectory:(NSString *)directory error:(NSError **)error;
@end
