// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMUSBService.h"
#import "VMUSBTransport.h"
#import <Security/SecureTransport.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>

@interface VMUSBService ()
@property int socketFD;
@property BOOL wantsRead;
@end
static NSError *ServiceError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.GuestService" code:1
                          userInfo:@{NSLocalizedDescriptionKey:message}];
}
static OSStatus ServiceRead(SSLConnectionRef ref, void *data, size_t *size) {
    VMUSBService *s = (__bridge VMUSBService *)ref; s.wantsRead = YES;
    size_t requested = *size;
    ssize_t n = recv(s.socketFD, data, requested, 0);
    *size = n > 0 ? (size_t)n : 0;
    if (!n) return errSSLClosedGraceful;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return errSSLClosedAbort;
    return n == (ssize_t)requested ? noErr : errSSLWouldBlock;
}
static OSStatus ServiceWrite(SSLConnectionRef ref, const void *data, size_t *size) {
    VMUSBService *s = (__bridge VMUSBService *)ref; s.wantsRead = NO;
    size_t requested = *size;
    ssize_t n = send(s.socketFD, data, requested, 0);
    *size = n > 0 ? (size_t)n : 0;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return errSSLClosedAbort;
    return n == (ssize_t)requested ? noErr : errSSLWouldBlock;
}
@implementation VMUSBService {
    SSLContextRef _tls;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport port:(uint16_t)port error:(NSError **)error {
    return [self initWithTransport:transport port:port canceled:nil error:error];
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport port:(uint16_t)port
                         canceled:(BOOL (^)(void))canceled error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    _socketFD = -1;
    _canceled = [canceled copy];
    _socketFD = [transport openPort:port timeout:120 canceled:_canceled error:error];
    if (_socketFD < 0) return nil;
    return self;
}
- (void)dealloc { [self close]; }
- (void)close {
    if (_tls) { CFRelease(_tls); _tls = NULL; }
    if (_socketFD >= 0) { close(_socketFD); _socketFD = -1; }
}
- (BOOL)waitUntil:(NSTimeInterval)deadline error:(NSError **)error {
    for (;;) {
        if (self.canceled && self.canceled()) {
            if (error) *error = ServiceError(@"Installation canceled."); return NO;
        }
        NSTimeInterval left = deadline - NSProcessInfo.processInfo.systemUptime;
        if (left <= 0 || _socketFD < 0) {
            if (error) *error = ServiceError(@"The guest service timed out or disconnected."); return NO;
        }
        struct pollfd p = {_socketFD, _wantsRead ? POLLIN : POLLOUT, 0};
        int result = poll(&p, 1, (int)MIN(left * 1000.0, 250.0) + 1);
        if (result > 0) {
            if (p.revents & (POLLERR | POLLNVAL)) {
                if (error) *error = ServiceError(@"The guest service connection failed."); return NO;
            }
            return YES; /* HUP is read once to drain buffered bytes / receive EOF */
        }
        if (result < 0 && errno != EINTR) {
            if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
            return NO;
        }
    }
}
- (BOOL)transfer:(void *)bytes count:(NSUInteger)count read:(BOOL)read error:(NSError **)error {
    NSUInteger at = 0;
    NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 120.0;
    while (at < count) {
        if (self.canceled && self.canceled()) {
            if (error) *error = ServiceError(@"Installation canceled."); return NO;
        }
        size_t n = count - at;
        OSStatus status;
        if (_tls) {
            n = 0;
            status = read ? SSLRead(_tls, (uint8_t *)bytes + at, count - at, &n)
                          : SSLWrite(_tls, (const uint8_t *)bytes + at, count - at, &n);
        } else {
            status = read ? ServiceRead((__bridge SSLConnectionRef)self, (uint8_t *)bytes + at, &n)
                          : ServiceWrite((__bridge SSLConnectionRef)self, (uint8_t *)bytes + at, &n);
        }
        at += n;
        if (status != noErr && status != errSSLWouldBlock) {
            if (error) *error = ServiceError([NSString stringWithFormat:@"Guest service I/O failed (%d).", (int)status]);
            return NO;
        }
        if (at < count && ![self waitUntil:deadline error:error]) return NO;
    }
    return YES;
}
- (BOOL)writeData:(NSData *)data error:(NSError **)error {
    return [self transfer:(void *)data.bytes count:data.length read:NO error:error];
}
- (NSData *)readCount:(NSUInteger)count error:(NSError **)error {
    if (count > 4u * 1024u * 1024u) {
        if (error) *error = ServiceError(@"The guest service response exceeds the size limit."); return nil;
    }
    NSMutableData *data = [NSMutableData dataWithLength:count];
    return [self transfer:data.mutableBytes count:count read:YES error:error] ? data : nil;
}
- (BOOL)sendPlist:(NSDictionary *)plist error:(NSError **)error {
    NSData *body = [NSPropertyListSerialization dataWithPropertyList:plist
                         format:NSPropertyListXMLFormat_v1_0 options:0 error:error];
    if (!body || body.length > 4u * 1024u * 1024u) return NO;
    uint32_t n = (uint32_t)body.length;
    uint8_t prefix[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    return [self writeData:[NSData dataWithBytes:prefix length:4] error:error] &&
           [self writeData:body error:error];
}
- (NSDictionary *)receivePlist:(NSError **)error {
    NSData *prefix = [self readCount:4 error:error];
    if (!prefix) return nil;
    const uint8_t *p = prefix.bytes;
    uint32_t n = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    NSData *body = [self readCount:n error:error];
    if (!body) return nil;
    id result = [NSPropertyListSerialization propertyListWithData:body options:NSPropertyListImmutable
                                                        format:nil error:error];
    if (![result isKindOfClass:NSDictionary.class]) {
        if (error) *error = ServiceError(@"The guest returned an invalid property-list response.");
        return nil;
    }
    return result;
}
- (NSDictionary *)request:(NSDictionary *)plist error:(NSError **)error {
    return [self sendPlist:plist error:error] ? [self receivePlist:error] : nil;
}
- (BOOL)enableTLSWithIdentity:(SecIdentityRef)identity expectedKey:(SecKeyRef)key error:(NSError **)error {
    if (_tls || !identity || !key) {
        if (error) *error = ServiceError(@"A valid guest pairing identity is required."); return NO;
    }
    _tls = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    NSArray *certificates = @[(__bridge id)identity];
    OSStatus status = _tls ? SSLSetIOFuncs(_tls, ServiceRead, ServiceWrite) : errSecAllocate;
    if (!status) status = SSLSetConnection(_tls, (__bridge SSLConnectionRef)self);
    if (!status) status = SSLSetProtocolVersionMin(_tls, kTLSProtocol1);
    if (!status) status = SSLSetProtocolVersionMax(_tls, kTLSProtocol1);
    /* Legacy lockdown expects its four-byte plist length in one TLS record.
     * BEAST 1/n-1 splitting is unnecessary on the private in-process cable
     * and can make old service readers close an otherwise valid TLS session. */
    if (!status) status = SSLSetSessionOption(_tls, kSSLSessionOptionSendOneByteRecord, false);
    if (!status) status = SSLSetCertificate(_tls, (__bridge CFArrayRef)certificates);
    if (!status) status = SSLSetSessionOption(_tls, kSSLSessionOptionBreakOnServerAuth, true);
    NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 120.0;
    BOOL pinned = NO;
    while (!status || status == errSSLWouldBlock || status == errSSLServerAuthCompleted) {
        status = SSLHandshake(_tls);
        if (status == errSSLServerAuthCompleted) {
            SecTrustRef trust = NULL;
            OSStatus copied = SSLCopyPeerTrust(_tls, &trust);
            SecKeyRef peer = !copied && trust ? SecTrustCopyKey(trust) : NULL;
            CFDataRef actual = peer ? SecKeyCopyExternalRepresentation(peer, NULL) : NULL;
            CFDataRef expected = SecKeyCopyExternalRepresentation(key, NULL);
            pinned = actual && expected && CFEqual(actual, expected);
            if (actual) CFRelease(actual);
            if (expected) CFRelease(expected);
            if (peer) CFRelease(peer);
            if (trust) CFRelease(trust);
            if (!pinned) { status = errSSLPeerBadCert; break; }
        } else if (!status) break;
        else if (status == errSSLWouldBlock && ![self waitUntil:deadline error:error]) return NO;
        else if (status != errSSLWouldBlock) break;
    }
    if (!status && pinned) return YES;
    if (error) *error = ServiceError([NSString stringWithFormat:@"Guest TLS pairing failed (%d).", (int)status]);
    return NO;
}
@end
