// Bounded AFC operations over a guest-owned service. MIT licensed.
#import <Foundation/Foundation.h>
@class VMUSBService;
@interface VMUSBAFC : NSObject
- (instancetype)initWithService:(VMUSBService *)service;
- (BOOL)makeStagingDirectory:(NSError **)error;
- (uint64_t)openPath:(NSString *)path error:(NSError **)error;
- (BOOL)writeHandle:(uint64_t)handle data:(NSData *)data error:(NSError **)error;
- (BOOL)closeHandle:(uint64_t)handle error:(NSError **)error;
- (BOOL)removePath:(NSString *)path error:(NSError **)error;
@end
