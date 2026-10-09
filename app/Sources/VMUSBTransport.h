// Host service streams over the emulated USB cable. MIT licensed.
#import <Foundation/Foundation.h>
#include "usb_mux.h"

@interface VMUSBTransport : NSObject
/* Worker-thread only. Returns an owned nonblocking local socket, or -1.
 * No physical USB, network listener, host filesystem or guest disk access. */
- (int)openPort:(uint16_t)port timeout:(NSTimeInterval)timeout error:(NSError **)error;
/* Owner emulator thread lifecycle; reset wakes every pending client. */
- (void)beginSession;
- (void)endSession:(NSString *)reason;
- (void)pollWithMux:(usb_mux_t *)mux;
@end
