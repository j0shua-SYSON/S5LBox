// Synthetic native protocol/identity checks. Not a firmware installation test.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>
#import "VMUSBService.h"
#import "VMUSBTransport.h"
#import "VMUSBPairing.h"
#import "VMUSBAFC.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
// Service tests substitute only the FD factory. The service framing code is real.
static int nextFD = -1;
@implementation VMUSBTransport
- (int)openPort:(uint16_t)port timeout:(NSTimeInterval)timeout
      canceled:(BOOL (^)(void))canceled error:(NSError **)error {
    (void)canceled;
    return [self openPort:port timeout:timeout error:error];
}
- (int)openPort:(uint16_t)port timeout:(NSTimeInterval)timeout error:(NSError **)error {
    (void)port; (void)timeout; (void)error;
    int result = nextFD; nextFD = -1; return result;
}
- (void)beginSession {}
- (void)endSession:(NSString *)reason { (void)reason; }
- (void)pollWithMux:(usb_mux_t *)mux { (void)mux; }
@end

@interface FixtureService : VMUSBService
@property (nonatomic, strong) NSMutableData *input;
@property (nonatomic, strong) NSMutableData *output;
@end
@implementation FixtureService
- (instancetype)init {
    self = [super init];
    if (self) { _input = [NSMutableData data]; _output = [NSMutableData data]; }
    return self;
}
- (void)close {} // no descriptor was opened by this synthetic peer
- (BOOL)writeData:(NSData *)data error:(NSError **)error {
    (void)error; [_output appendData:data]; return YES;
}
- (NSData *)readCount:(NSUInteger)count error:(NSError **)error {
    (void)error;
    if (count > _input.length) return nil;
    NSData *part = [_input subdataWithRange:NSMakeRange(0, count)];
    [_input replaceBytesInRange:NSMakeRange(0, count) withBytes:NULL length:0]; return part;
}
@end
static void put64(uint8_t *p, uint64_t n) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(n >> (8 * i)); }
static uint64_t get64(const uint8_t *p) { uint64_t n = 0; for (unsigned i = 0; i < 8; i++) n |= (uint64_t)p[i] << (8 * i); return n; }
static void reply(FixtureService *s, uint64_t sequence, uint64_t op, uint64_t value) {
    uint8_t packet[48] = {'C','F','A','6','L','P','A','A'};
    put64(packet + 8, 48); put64(packet + 16, 48); put64(packet + 24, sequence);
    put64(packet + 32, op); put64(packet + 40, value);
    [s.input appendBytes:packet length:48];
}
static void testAFC(void) {
    FixtureService *service = [FixtureService new];
    VMUSBAFC *afc = [[VMUSBAFC alloc] initWithService:service];
    NSError *error = nil;
    reply(service, 1, 1, 16); CHECK([afc makeStagingDirectory:&error]);
    const uint8_t *sent = service.output.bytes;
    CHECK(service.output.length == 54 && get64(sent + 8) == 54 && get64(sent + 32) == 9);
    CHECK(!memcmp(sent + 40, "PublicStaging", 14));
    reply(service, 2, 14, 17); CHECK([afc openPath:@"PublicStaging/S5LBox-test.ipa" error:&error] == 17);
    NSUInteger offset = service.output.length;
    reply(service, 3, 1, 0); CHECK([afc writeHandle:17 data:[NSData dataWithBytes:"ipa" length:3] error:&error]);
    sent = (const uint8_t *)service.output.bytes + offset;
    CHECK(get64(sent + 8) == 51 && get64(sent + 16) == 48 && get64(sent + 32) == 16 && get64(sent + 40) == 17);
    CHECK(!memcmp(sent + 48, "ipa", 3));
    reply(service, 4, 1, 0); CHECK([afc closeHandle:17 error:&error]);
    reply(service, 5, 1, 0); CHECK([afc removePath:@"PublicStaging/S5LBox-test.ipa" error:&error]);
    offset = service.output.length;
    CHECK(![afc removePath:@"PublicStaging/another-user.ipa" error:&error]);
    CHECK(![afc openPath:@"PublicStaging/S5LBox-../foo.ipa" error:&error]);
    CHECK(![afc writeHandle:0 data:NSData.data error:&error]);
    CHECK(![afc writeHandle:17 data:[NSMutableData dataWithLength:65537] error:&error]);
    CHECK(service.output.length == offset);
    reply(service, 6, 1, 18); CHECK(![afc makeStagingDirectory:&error] && error.code == 18);
    reply(service, 100, 1, 0); CHECK(![afc makeStagingDirectory:&error]);
    service = [FixtureService new]; afc = [[VMUSBAFC alloc] initWithService:service];
    reply(service, 1, 1, 0); put64((uint8_t *)service.input.mutableBytes + 8, UINT64_MAX);
    CHECK(![afc makeStagingDirectory:&error] && service.input.length == 8);
    service = [FixtureService new]; afc = [[VMUSBAFC alloc] initWithService:service];
    reply(service, 1, 1, 0); ((uint8_t *)service.input.mutableBytes)[0] = 'X';
    CHECK(![afc makeStagingDirectory:&error]);
}
static void testService(void) {
    int fds[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    nextFD = fds[0];
    VMUSBService *service = [[VMUSBService alloc] initWithTransport:[VMUSBTransport new] port:62078 error:NULL];
    CHECK(service != nil);
    NSDictionary *value = @{@"Status":@"Complete"};
    CHECK([service sendPlist:value error:NULL]);
    uint8_t buffer[2048]; ssize_t n = read(fds[1], buffer, sizeof buffer);
    CHECK(n > 4);
    uint32_t size = (uint32_t)buffer[0] << 24 | (uint32_t)buffer[1] << 16 | (uint32_t)buffer[2] << 8 | buffer[3];
    CHECK(size == (uint32_t)n - 4);
    CHECK(write(fds[1], buffer, (size_t)n) == n);
    CHECK([[service receivePlist:NULL] isEqual:value]);
    service.canceled = ^BOOL{ return YES; };
    NSError *error = nil;
    CHECK(![service readCount:1 error:&error] && error != nil);
    service.canceled = nil;
    CHECK(![service readCount:4194305 error:&error]);
    const uint8_t large[4] = {0xff,0xff,0xff,0xff};
    CHECK(write(fds[1], large, 4) == 4);
    CHECK(![service receivePlist:&error]);
    close(fds[1]); CHECK(![service readCount:1 error:&error]);
    [service close]; [service close];
}
static void testPairing(NSURL *folder) {
    NSDictionary *attributes = @{(__bridge id)kSecAttrKeyType:(__bridge id)kSecAttrKeyTypeRSA, (__bridge id)kSecAttrKeySizeInBits:@2048};
    SecKeyRef key = SecKeyCreateRandomKey((__bridge CFDictionaryRef)attributes, NULL);
    CHECK(key != NULL); if (!key) return;
    SecKeyRef publicKey = SecKeyCopyPublicKey(key);
    NSData *der = CFBridgingRelease(SecKeyCopyExternalRepresentation(publicKey, NULL));
    NSError *error = nil;
    NSString *identifier = NSUUID.UUID.UUIDString;
    VMUSBPairing *pair = [[VMUSBPairing alloc] initWithInstanceID:identifier devicePublicKey:der directory:folder error:&error];
    CHECK(pair && pair.identity && pair.deviceKey);
    CHECK(pair.record[@"HostID"] && !pair.record[@"HostPrivateKey"] && !pair.record[@"RootPrivateKey"]);
    SecCertificateRef certificate = NULL;
    CHECK(pair && SecIdentityCopyCertificate(pair.identity, &certificate) == errSecSuccess);
    SecKeyRef certificateKey = certificate ? SecCertificateCopyKey(certificate) : NULL;
    CHECK(certificateKey != NULL);
    if (certificateKey) CFRelease(certificateKey);
    if (certificate) CFRelease(certificate);
    VMUSBPairing *again = [[VMUSBPairing alloc] initWithInstanceID:identifier devicePublicKey:der directory:folder error:&error];
    CHECK([again.record isEqual:pair.record]);
    CHECK(![[VMUSBPairing alloc] initWithInstanceID:@"../bad" devicePublicKey:der directory:folder error:&error]);
    CHECK(![[VMUSBPairing alloc] initWithInstanceID:identifier devicePublicKey:NSData.data directory:folder error:&error]);
    NSData *stored = [NSData dataWithContentsOfURL:[folder URLByAppendingPathComponent:[identifier stringByAppendingPathExtension:@"plist"]]];
    NSDictionary *saved = [NSPropertyListSerialization propertyListWithData:stored options:0 format:nil error:NULL];
    CHECK([saved[@"HostPrivateKey"] isKindOfClass:NSData.class]);
    CHECK(!saved[@"RootPrivateKey"]);
    CFRelease(publicKey); CFRelease(key);
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        testAFC(); testService();
        testPairing([NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES]);
        printf("Native USB services: %u checks, %u failures\n", checks, failures);
        return failures ? 1 : 0;
    }
}
