#import <Foundation/Foundation.h>
@interface VMPackageRepository : NSObject <NSURLSessionDataDelegate>
@property (atomic) BOOL canceled;
@property (nonatomic, copy) void (^progress)(NSString *message);
+ (NSURL *)directory;
+ (NSString *)sha256:(NSData *)data;
// Each source has a base URL and index-directory URL (both HTTPS).
- (NSArray<NSDictionary *> *)refresh:(NSArray<NSDictionary *> *)sources error:(NSError **)error;
- (NSURL *)download:(NSDictionary *)package error:(NSError **)error;
@end
