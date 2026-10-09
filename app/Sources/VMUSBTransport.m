// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMUSBTransport.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

@interface VMUSBConnection : NSObject
@property uint16_t port;
@property int bridgeFD;
@property int channel;
@property BOOL ready;
@property BOOL canceled;
@property (nonatomic, strong) NSError *error;
@end
@implementation VMUSBConnection
@end

static NSError *VMUSBError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.VirtualUSB" code:1
                          userInfo:@{NSLocalizedDescriptionKey:message}];
}
@implementation VMUSBTransport {
    NSCondition *_condition;
    NSMutableArray<VMUSBConnection *> *_connections;
    BOOL _accepting;
    NSString *_unavailable;
}
- (instancetype)init {
    self = [super init];
    if (self) {
        _condition = [[NSCondition alloc] init];
        _connections = [NSMutableArray array];
        _unavailable = @"Start the machine with virtual USB enabled first.";
    }
    return self;
}
- (void)dealloc { [self endSession:@"Virtual USB transport closed."]; }
- (void)beginSession {
    [self endSession:@"The guest started a new USB session."];
    [_condition lock]; _accepting = YES; _unavailable = nil; [_condition unlock];
}
- (void)endSession:(NSString *)reason {
    [_condition lock];
    _accepting = NO; _unavailable = [reason copy];
    for (VMUSBConnection *c in _connections) {
        c.error = VMUSBError(reason);
        if (c.bridgeFD >= 0) { close(c.bridgeFD); c.bridgeFD = -1; }
    }
    [_connections removeAllObjects];
    [_condition broadcast]; [_condition unlock];
}
- (int)openPort:(uint16_t)port timeout:(NSTimeInterval)timeout error:(NSError **)error {
    if (NSThread.isMainThread || !port || !isfinite(timeout) || timeout <= 0) {
        if (error) *error = VMUSBError(@"USB service connections require a worker thread and a finite timeout.");
        return -1;
    }
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        return -1;
    }
    int one = 1;
    BOOL configured = YES;
    for (unsigned i = 0; i < 2; i++) {
        if (fcntl(fds[i], F_SETFL, O_NONBLOCK) != 0 ||
            fcntl(fds[i], F_SETFD, FD_CLOEXEC) != 0 ||
            setsockopt(fds[i], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0)
            configured = NO;
    }
    if (!configured) {
        int saved = errno; close(fds[0]); close(fds[1]);
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:saved userInfo:nil];
        return -1;
    }
    VMUSBConnection *c = [[VMUSBConnection alloc] init];
    c.port = port; c.bridgeFD = fds[0]; c.channel = -1;
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:MIN(timeout, 300.0)];
    [_condition lock];
    if (!_accepting || _connections.count >= USB_MUX_CHANNELS) {
        c.error = VMUSBError(_unavailable ?: @"All virtual USB service channels are busy.");
        close(c.bridgeFD); c.bridgeFD = -1;
    } else {
        [_connections addObject:c];
        while (!c.ready && !c.error) {
            if (![_condition waitUntilDate:deadline] && !c.ready && !c.error) {
                c.canceled = YES;
                c.error = VMUSBError(@"The guest USB service did not respond in time.");
            }
        }
    }
    NSError *failure = c.error;
    [_condition unlock];
    if (failure) { close(fds[1]); if (error) *error = failure; return -1; }
    return fds[1];
}
- (void)pollWithMux:(usb_mux_t *)mux {
    if (!mux) return;
    if (mux->error) { [self endSession:[NSString stringWithUTF8String:mux->error]]; return; }
    if (!usb_mux_ready(mux)) return;
    [_condition lock];
    /* Nonblocking I/O only, bounded to one queue-sized transfer per direction
     * per channel. Neither a slow client nor TLS work runs on this thread. */
    for (VMUSBConnection *c in [_connections copy]) {
        if (c.canceled) {
            if (c.channel >= 0) usb_mux_close(mux, (unsigned)c.channel);
            if (c.bridgeFD >= 0) close(c.bridgeFD);
            c.bridgeFD = -1; [_connections removeObject:c]; continue;
        }
        if (c.channel < 0) c.channel = usb_mux_connect(mux, c.port);
        if (c.channel < 0) continue;
        usb_mux_channel_t *stream = &mux->channel[c.channel];
        /* A final reply and RST can arrive in the same USB batch. Drain the
         * accepted bytes before closing the local socket (notably Complete). */
        if (c.ready && stream->receive_used) {
            uint8_t buffer[USB_MUX_TX_SIZE];
            ssize_t n = send(c.bridgeFD, stream->receive, MIN((size_t)stream->receive_used, sizeof buffer), 0);
            if (n > 0) (void)usb_mux_read(mux, (unsigned)c.channel, buffer, (size_t)n);
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) c.canceled = YES;
            if (!c.canceled && stream->receive_used) continue;
        }
        if (stream->state == USB_MUX_ERROR || stream->state == USB_MUX_CLOSED) {
            c.error = VMUSBError(stream->error ? [NSString stringWithUTF8String:stream->error]
                                               : @"The guest closed the service connection.");
            usb_mux_close(mux, (unsigned)c.channel);
            close(c.bridgeFD); c.bridgeFD = -1;
            [_connections removeObject:c]; [_condition broadcast]; continue;
        }
        if (stream->state != USB_MUX_OPEN) continue;
        if (!c.ready) { c.ready = YES; [_condition broadcast]; }
        uint8_t buffer[USB_MUX_TX_SIZE];
        size_t space = sizeof stream->send - stream->send_used;
        if (space) {
            ssize_t n = recv(c.bridgeFD, buffer, space, 0);
            if (n > 0) (void)usb_mux_write(mux, (unsigned)c.channel, buffer, (size_t)n);
            else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                c.canceled = YES;
        }
        if (!c.canceled && stream->receive_used) {
            size_t size = MIN((size_t)stream->receive_used, sizeof buffer);
            ssize_t n = send(c.bridgeFD, stream->receive, size, 0);
            if (n > 0) (void)usb_mux_read(mux, (unsigned)c.channel, buffer, (size_t)n);
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                c.canceled = YES;
        }
    }
    [_condition unlock];
}
@end
