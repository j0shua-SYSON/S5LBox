// Live guest file channel for the bundled interpreter. MIT licensed.
#import <Foundation/Foundation.h>
#include "../../integrations/touchhle-cpu/s5lbox_hle.h"
@class VMUSBService;

@protocol VMTouchHLEFileChannel <NSObject>
- (s5lbox_hle_live_config)configuration;
- (void)close;
@property(nonatomic,readonly) NSError *error;
@end
@interface VMTouchHLEFiles : NSObject <VMTouchHLEFileChannel>
// service must already have completed the authenticated file-service handshake.
// Own this object and its configuration strings until run_live returns.
- (instancetype)initWithService:(VMUSBService *)service
                 homeDirectory:(NSString *)home bundleDirectory:(NSString *)bundle;
- (s5lbox_hle_live_config)configuration;
// Call on the runtime worker after it returns, including failure/cancellation.
- (void)close;
@property(nonatomic,readonly) NSError *error;
@end
