// Worker-thread, bounded service I/O. MIT licensed.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
@class VMUSBTransport;
@interface VMUSBService : NSObject
- (instancetype)initWithTransport:(VMUSBTransport *)transport port:(uint16_t)port error:(NSError **)error;
- (instancetype)initWithTransport:(VMUSBTransport *)transport port:(uint16_t)port
                         canceled:(BOOL (^)(void))canceled error:(NSError **)error;
@property (nonatomic, copy) BOOL (^canceled)(void);
- (BOOL)writeData:(NSData *)data error:(NSError **)error;
- (NSData *)readCount:(NSUInteger)count error:(NSError **)error;
- (BOOL)sendPlist:(NSDictionary *)plist error:(NSError **)error;
- (NSDictionary *)receivePlist:(NSError **)error;
- (NSDictionary *)request:(NSDictionary *)plist error:(NSError **)error;
/* Legacy TLS is confined to this local emulated-device stream. The peer key
 * must equal the guest key used to construct the pairing record. */
- (BOOL)enableTLSWithIdentity:(SecIdentityRef)identity
                 expectedKey:(SecKeyRef)key error:(NSError **)error;
- (void)close;
@end
