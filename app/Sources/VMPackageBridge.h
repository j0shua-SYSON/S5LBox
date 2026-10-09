#import <Foundation/Foundation.h>
@class VMUSBTransport;
@interface VMPackageBridge : NSObject
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier;
@property (atomic) BOOL canceled;
@property (nonatomic, copy) void (^progress)(NSString *text);
// Worker only. No raw guest disk access in any live operation.
- (NSData *)status:(NSError **)error;
- (NSData *)install:(NSArray<NSURL *> *)archives status:(NSData *)status error:(NSError **)error;
- (NSData *)remove:(NSString *)package status:(NSData *)status error:(NSError **)error;
// Caller must own a stopped machine, have explicit force-off consent, and
// finish the existing jailbreak builder/recovery before invoking this method.
+ (BOOL)prepareStoppedMachine:(NSString *)directory instanceID:(NSString *)identifier error:(NSError **)error;
@end
