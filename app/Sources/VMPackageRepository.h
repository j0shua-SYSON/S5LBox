#import <Foundation/Foundation.h>
@interface VMPackageRepository : NSObject <NSURLSessionDataDelegate>
@property (atomic) BOOL canceled;
@property (nonatomic, copy) void (^progress)(NSString *message);
+ (NSURL *)directory;
+ (NSString *)sha256:(NSData *)data;
+ (NSString *)checksumField:(NSDictionary *)package;
+ (BOOL)verify:(NSData *)data package:(NSDictionary *)package;
+ (NSDictionary *)bigBossSource;
+ (NSArray<NSDictionary *> *)sourcesForDefaults:(NSUserDefaults *)defaults;
// HTTPS by default; BigBoss explicitly uses its published HTTP endpoint.
- (NSArray<NSDictionary *> *)refresh:(NSArray<NSDictionary *> *)sources error:(NSError **)error;
- (NSURL *)download:(NSDictionary *)package error:(NSError **)error;
@end
