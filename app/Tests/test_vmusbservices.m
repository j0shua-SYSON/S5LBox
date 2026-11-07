// Synthetic native protocol/identity checks. Not a firmware installation test.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>
#import "VMUSBService.h"
#import "VMUSBTransport.h"
#import "VMUSBPairing.h"
#import "VMUSBAFC.h"
#include "VMGuestFilesService.h"
#import "VMGuestFiles.h"
#import "VMUSBLockdown.h"
#import "VMGuestApplications.h"
#import <dispatch/dispatch.h>
#include <math.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #x); } } while (0)
// Service tests substitute only the FD factory. The service framing code is real.
static int nextFD = -1;
static BOOL lastAllowGuestWake;
@implementation VMUSBTransport
- (int)openPort:(uint16_t)port timeout:(NSTimeInterval)timeout
      canceled:(BOOL (^)(void))canceled allowGuestWake:(BOOL)allowGuestWake error:(NSError **)error {
    lastAllowGuestWake = allowGuestWake;
    return [self openPort:port timeout:timeout canceled:canceled error:error];
}
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
- (void)reconnectSession:(NSString *)reason { (void)reason; }
- (BOOL)hasServiceActivity { return NO; }
- (void)pollWithMux:(usb_mux_t *)mux { (void)mux; }
@end
@interface ClockService : VMUSBService
@property (nonatomic, strong) NSMutableArray *replies;
@property (nonatomic, strong) NSMutableArray *queries;
@end
@implementation ClockService
- (instancetype)init {
    self = [super init];
    if (self) { _replies = [NSMutableArray array]; _queries = [NSMutableArray array]; }
    return self;
}
- (void)close {} // no descriptor in this synthetic guest
- (NSDictionary *)request:(NSDictionary *)query error:(NSError **)error {
    if (self.canceled && self.canceled()) {
        if (error) *error = [NSError errorWithDomain:@"fixture" code:1
            userInfo:@{NSLocalizedDescriptionKey:@"Operation canceled."}];
        return nil;
    }
    [_queries addObject:query];
    if (!_replies.count) return nil;
    NSDictionary *reply = _replies.firstObject; [_replies removeObjectAtIndex:0]; return reply;
}
@end
static void testClock(void) {
    const NSTimeInterval epoch = 1792198800.75;
    for (NSNumber *old in @[@1000000000ULL, @1000000000.5]) {
        ClockService *s = [ClockService new];
        [s.replies addObjectsFromArray:@[@{@"Request":@"GetValue", @"Value":old},
            @{@"Request":@"SetValue", @"Result":@"Success"},
            @{@"Request":@"GetValue", @"Value":@(floor(epoch))}]];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        __block unsigned samples = 0;
        CHECK([client synchronizeClockWithNow:^NSTimeInterval{
            // The wall clock is sampled after the query, never at job creation.
            CHECK(s.queries.count >= 1); samples++; return epoch;
        } error:NULL]);
        CHECK(samples == 2 && s.queries.count == 3);
        NSDictionary *set = s.queries[1];
        CHECK([set[@"Request"] isEqual:@"SetValue"]);
        CHECK(CFNumberIsFloatType((__bridge CFNumberRef)set[@"Value"]) ==
              CFNumberIsFloatType((__bridge CFNumberRef)old));
        CHECK(fabs([set[@"Value"] doubleValue] - epoch) < 1);
        for (NSDictionary *query in s.queries) {
            CHECK([query[@"Key"] isEqual:@"TimeIntervalSince1970"] && !query[@"Domain"]);
        }
    }
    for (id value in @[@YES, @"1000000000", NSNull.null, @(-1), @(NAN), @(INFINITY)]) {
        ClockService *s = [ClockService new];
        [s.replies addObject:@{@"Request":@"GetValue", @"Value":value}];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        NSError *error = nil;
        CHECK(![client synchronizeClockWithNow:^NSTimeInterval{ return epoch; } error:&error]);
        CHECK(error && s.queries.count == 1); // Invalid data never reaches SetValue.
    }
    for (NSNumber *host in @[@(NAN), @(INFINITY), @946684799.0, @4102444800.0]) {
        ClockService *s = [ClockService new];
        [s.replies addObject:@{@"Request":@"GetValue", @"Value":@1000000000}];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        CHECK(![client synchronizeClockWithNow:^NSTimeInterval{ return host.doubleValue; } error:NULL]);
        CHECK(s.queries.count == 1);
    }
    for (NSDictionary *bad in @[@{@"Request":@"GetValue", @"Error":@"MissingValue"},
            @{@"Request":@"SetValue", @"Value":@1000000000},
            @{@"Request":@"GetValue", @"Result":@"Failure", @"Value":@1000000000}]) {
        ClockService *s = [ClockService new]; [s.replies addObject:bad];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        CHECK(![client synchronizeClockWithNow:^NSTimeInterval{ return epoch; } error:NULL]);
        CHECK(s.queries.count == 1);
    }
    for (id actual in @[@1000000000, @(epoch+60), @(epoch-60), @YES, NSNull.null]) {
        ClockService *s = [ClockService new];
        [s.replies addObjectsFromArray:@[@{@"Request":@"GetValue", @"Value":@1000000000},
            @{@"Request":@"SetValue"}, @{@"Request":@"GetValue", @"Value":actual}]];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        CHECK(![client synchronizeClockWithNow:^NSTimeInterval{ return epoch; } error:NULL]);
        CHECK(s.queries.count == 3); // A SetValue acknowledgement alone is not success.
    }
    ClockService *s = [ClockService new];
    [s.replies addObjectsFromArray:@[@{@"Request":@"GetValue", @"Value":@1000000000},
        @{@"Request":@"SetValue", @"Error":@"SetProhibited"}]];
    VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
    CHECK(![client synchronizeClockWithNow:^NSTimeInterval{ return epoch; } error:NULL]);
    CHECK(s.queries.count == 2);
    [s.queries removeAllObjects]; s.canceled = ^BOOL{ return YES; };
    CHECK(![client synchronizeClockWithNow:^NSTimeInterval{ return epoch; } error:NULL]);
    CHECK(s.queries.count == 0);
    CHECK(![VMUSBLockdown openWithTransport:[VMUSBTransport new] instanceID:@"abc"
        canceled:^BOOL{ return YES; } deadline:0 error:NULL]);
}

static void testTimeZone(void) {
    const NSTimeInterval october = 1792198800.0;
    // Real named zones, including DST, negative and non-whole-hour offsets.
    for (NSArray *fixture in @[@[@"Europe/Paris", @(october), @7200],
            @[@"America/Los_Angeles", @1768435200.0, @(-28800)],
            @[@"America/Los_Angeles", @1784073600.0, @(-25200)],
            @[@"Asia/Kathmandu", @(october), @20700],
            @[@"Etc/UTC", @(october), @0]]) {
        NSString *zone = fixture[0]; NSNumber *epoch = fixture[1], *offset = fixture[2];
        for (NSNumber *alreadySet in @[@NO, @YES]) {
            ClockService *s = [ClockService new];
            [s.replies addObject:@{@"Request":@"GetValue", @"Value":alreadySet.boolValue ? zone : @"US/Pacific"}];
            if (!alreadySet.boolValue) [s.replies addObjectsFromArray:@[
                @{@"Request":@"SetValue", @"Result":@"Success"}, @{@"Request":@"GetValue", @"Value":zone}]];
            [s.replies addObject:@{@"Request":@"GetValue", @"Value":offset}];
            VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
            CHECK([client synchronizeTimeZone:zone now:^NSTimeInterval{ return epoch.doubleValue; } error:NULL]);
            CHECK(s.queries.count == (alreadySet.boolValue ? 2 : 4));
            for (NSDictionary *query in s.queries) {
                CHECK(!query[@"Domain"]);
                CHECK([query[@"Key"] isEqual:@"TimeZone"] || [query[@"Key"] isEqual:@"TimeZoneOffsetFromUTC"]);
                if ([query[@"Request"] isEqual:@"SetValue"])
                    CHECK([query[@"Key"] isEqual:@"TimeZone"] && [query[@"Value"] isEqual:zone]);
            }
        }
    }
    for (id invalid in @[@"", @"../Europe/Paris", @"/Europe/Paris", @"Europe//Paris", @"Europe/Paris/",
            @"Europe/Paris\n", @"Unknown/NotAZone", @42, NSNull.null]) {
        ClockService *s = [ClockService new];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        NSError *error = nil;
        CHECK(![client synchronizeTimeZone:invalid now:^NSTimeInterval{ return october; } error:&error]);
        CHECK(error && !s.queries.count);
    }
    for (id invalid in @[@YES, @42, @"", NSNull.null]) {
        ClockService *s = [ClockService new];
        [s.replies addObject:@{@"Request":@"GetValue", @"Value":invalid}];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        CHECK(![client synchronizeTimeZone:@"Europe/Paris" now:^NSTimeInterval{ return october; } error:NULL]);
        CHECK(s.queries.count == 1);
    }
    for (NSDictionary *bad in @[@{@"Request":@"GetValue", @"Error":@"MissingValue"},
            @{@"Request":@"SetValue", @"Value":@"Europe/Paris"}]) {
        ClockService *s = [ClockService new]; [s.replies addObject:bad];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        CHECK(![client synchronizeTimeZone:@"Europe/Paris" now:^NSTimeInterval{ return october; } error:NULL]);
        CHECK(s.queries.count == 1);
    }
    for (id actual in @[@"US/Pacific", @YES, NSNull.null]) {
        ClockService *s = [ClockService new];
        [s.replies addObjectsFromArray:@[@{@"Request":@"GetValue", @"Value":@"US/Pacific"},
            @{@"Request":@"SetValue"}, @{@"Request":@"GetValue", @"Value":actual}]];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        CHECK(![client synchronizeTimeZone:@"Europe/Paris" now:^NSTimeInterval{ return october; } error:NULL]);
        CHECK(s.queries.count == 3);
    }
    for (id offset in @[@YES, @"32400", NSNull.null, @(NAN), @(INFINITY), @0, @(-25200), @86400]) {
        ClockService *s = [ClockService new];
        [s.replies addObjectsFromArray:@[@{@"Request":@"GetValue", @"Value":@"Europe/Paris"},
            @{@"Request":@"GetValue", @"Value":offset}]];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        CHECK(![client synchronizeTimeZone:@"Europe/Paris" now:^NSTimeInterval{ return october; } error:NULL]);
        CHECK(s.queries.count == 2); // Matching name alone cannot confirm current rules.
    }
    for (NSNumber *host in @[@(NAN), @(INFINITY), @946684799.0, @4102444800.0]) {
        ClockService *s = [ClockService new];
        [s.replies addObjectsFromArray:@[@{@"Request":@"GetValue", @"Value":@"Europe/Paris"},
            @{@"Request":@"GetValue", @"Value":@7200}]];
        VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
        CHECK(![client synchronizeTimeZone:@"Europe/Paris" now:^NSTimeInterval{ return host.doubleValue; } error:NULL]);
    }
    ClockService *s = [ClockService new];
    [s.replies addObjectsFromArray:@[@{@"Request":@"GetValue", @"Value":@"US/Pacific"},
        @{@"Request":@"SetValue", @"Error":@"SetProhibited"}]];
    VMUSBLockdown *client = [[VMUSBLockdown alloc] initWithService:s];
    CHECK(![client synchronizeTimeZone:@"Europe/Paris" now:^NSTimeInterval{ return october; } error:NULL]);
    CHECK(s.queries.count == 2);
    CHECK(![client synchronizeTimeZone:nil now:nil error:NULL]);
    for (NSUInteger cancelAfter = 0; cancelAfter < 4; cancelAfter++) {
        ClockService *peer = [ClockService new];
        [peer.replies addObjectsFromArray:@[@{@"Request":@"GetValue", @"Value":@"US/Pacific"},
            @{@"Request":@"SetValue"}, @{@"Request":@"GetValue", @"Value":@"Europe/Paris"},
            @{@"Request":@"GetValue", @"Value":@7200}]];
        __weak ClockService *weakPeer = peer;
        peer.canceled = ^BOOL{ return weakPeer.queries.count >= cancelAfter; };
        VMUSBLockdown *c = [[VMUSBLockdown alloc] initWithService:peer];
        NSError *error = nil;
        CHECK(![c synchronizeTimeZone:@"Europe/Paris" now:^NSTimeInterval{ return october; } error:&error]);
        CHECK(error && peer.queries.count == cancelAfter);
    }
}

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
static void dataReply(FixtureService *s, uint64_t sequence, const void *bytes, NSUInteger size) {
    uint8_t header[40] = {'C','F','A','6','L','P','A','A'};
    put64(header + 8, 40 + size); put64(header + 16, 40); put64(header + 24, sequence); put64(header + 32, 2);
    [s.input appendBytes:header length:40]; [s.input appendBytes:bytes length:size];
}
static void testBrowserAFC(void) {
    NSData *jobData = [NSData dataWithBytes:VM_GUEST_FILES_PLIST length:sizeof VM_GUEST_FILES_PLIST - 1];
    NSDictionary *job = [NSPropertyListSerialization propertyListWithData:jobData options:0 format:NULL error:NULL];
    CHECK(([job[@"ProgramArguments"] isEqual:@[@"/usr/libexec/afcd", @"--launchd", @"-d", @"/"]]));
    CHECK([job[@"Sockets"][@"Listeners"][@"SockNodeName"] isEqual:@"127.0.0.1"]);
    CHECK([job[@"Sockets"][@"Listeners"][@"SockServiceName"] integerValue] == VM_GUEST_FILES_PORT);
    FixtureService *service = [FixtureService new]; NSError *error = nil;
    VMUSBAFC *installer = [[VMUSBAFC alloc] initWithService:service];
    CHECK(![installer contentsOfDirectory:@"/" error:&error]);
    CHECK(![installer openBrowserPath:@"/file" writing:YES error:&error]);
    CHECK(!service.output.length);
    VMUSBAFC *afc = [[VMUSBAFC alloc] initForFileBrowserWithService:service];
    const char names[] = ".\0..\0Documents\0hello.txt\0";
    dataReply(service, 1, names, sizeof names - 1);
    CHECK(([[afc contentsOfDirectory:@"/" error:&error] isEqual:@[@"Documents", @"hello.txt"]]));
    CHECK(get64((const uint8_t *)service.output.bytes + 32) == 3);
    // Keep numeric text separate from the NUL escape (octal escapes consume digits).
    const char validInfo[] = "st_ifmt\0S_IFREG\0st_size\0" "123\0";
    dataReply(service, 2, validInfo, sizeof validInfo - 1);
    CHECK([[afc attributesOfPath:@"/hello.txt" error:&error][@"st_size"] isEqual:@"123"]);
    reply(service, 3, 14, 9); CHECK([afc openBrowserPath:@"/hello.txt" writing:NO error:&error] == 9);
    dataReply(service, 4, "abc", 3); CHECK([[afc readHandle:9 count:3 error:&error] isEqual:[NSData dataWithBytes:"abc" length:3]]);
    reply(service, 5, 1, 0); CHECK([afc closeHandle:9 error:&error]);
    reply(service, 6, 1, 0); CHECK([afc createDirectory:@"/new" error:&error]);
    reply(service, 7, 1, 0); CHECK([afc renameBrowserPath:@"/hello.txt" toPath:@"/new/name.txt" error:&error]);
    reply(service, 8, 1, 0); CHECK([afc removeBrowserPath:@"/new/name.txt" error:&error]);
    NSUInteger before = service.output.length;
    CHECK(![afc removeBrowserPath:@"/" error:&error]);
    CHECK(![afc createDirectory:@"/../new" error:&error]);
    CHECK(![afc openBrowserPath:@"/a//b" writing:YES error:&error]);
    CHECK(![afc renameBrowserPath:@"/good" toPath:@"/" error:&error]);
    CHECK(![afc readHandle:9 count:65537 error:&error]);
    CHECK(service.output.length == before);
    CHECK(![VMUSBAFC validFilename:@".."] && ![VMUSBAFC validFilename:@"a/b"] && ![VMUSBAFC validFilename:@"x\ny"]);
    CHECK([VMUSBAFC validFilename:@"A normal file.txt"]);
    dataReply(service, 9, "not-terminated", 14); CHECK(![afc contentsOfDirectory:@"/" error:&error]);
    const char duplicate[] = "file\0file\0";
    dataReply(service, 10, duplicate, sizeof duplicate - 1); CHECK(![afc contentsOfDirectory:@"/" error:&error]);
    const char traversal[] = "../outside\0";
    dataReply(service, 11, traversal, sizeof traversal - 1); CHECK(![afc contentsOfDirectory:@"/" error:&error]);
    dataReply(service, 12, "abc", 3); CHECK(![afc readHandle:9 count:2 error:&error]);
    const char odd[] = "st_ifmt\0";
    dataReply(service, 13, odd, sizeof odd - 1); CHECK(![afc attributesOfPath:@"/file" error:&error]);
}
@interface FixtureFiles : VMGuestFiles
@property (nonatomic, strong) FixtureService *fixture;
@end
@implementation FixtureFiles
- (VMUSBService *)connect:(NSError **)error { (void)error; return self.fixture; }
@end
static void testGuestFiles(NSURL *base) {
    FixtureFiles *files = [[FixtureFiles alloc] initWithTransport:nil instanceID:@"fixture" root:YES];
    files.fixture = [FixtureService new]; NSError *error = nil;
    const char info[] = "st_ifmt\0S_IFREG\0st_size\0" "3\0";
    const char names[] = "hello.txt\0";
    dataReply(files.fixture, 1, names, sizeof names - 1);
    dataReply(files.fixture, 2, info, sizeof info - 1);
    NSArray<VMGuestFile *> *listing = [files list:@"/" error:&error];
    CHECK(listing.count == 1 && listing.firstObject.size == 3 && [listing.firstObject.path isEqual:@"/hello.txt"]);
    NSURL *folder = [base URLByAppendingPathComponent:[@"files-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    CHECK([NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:nil error:&error]);
    files.fixture = [FixtureService new];
    dataReply(files.fixture, 1, info, sizeof info - 1);
    reply(files.fixture, 2, 14, 12); dataReply(files.fixture, 3, "abc", 3); reply(files.fixture, 4, 1, 0);
    NSURL *download = [files download:listing.firstObject intoDirectory:folder error:&error];
    CHECK([[NSData dataWithContentsOfURL:download] isEqual:[NSData dataWithBytes:"abc" length:3]]);
    files.fixture = [FixtureService new];
    dataReply(files.fixture, 1, info, sizeof info - 1);
    CHECK(![files upload:download directory:@"/" error:&error]); // never truncate an existing destination
    CHECK([error.localizedDescription containsString:@"already exists"]);
    files.fixture = [FixtureService new]; error = nil;
    reply(files.fixture, 1, 1, 8); reply(files.fixture, 2, 1, 8); // final + unique temporary absent
    reply(files.fixture, 3, 14, 13); reply(files.fixture, 4, 1, 0); reply(files.fixture, 5, 1, 0);
    reply(files.fixture, 6, 1, 8); reply(files.fixture, 7, 1, 0); // recheck and publish
    CHECK([files upload:download directory:@"/" error:&error]);
    CHECK(files.fixture.input.length == 0);
    files.fixture = [FixtureService new];
    dataReply(files.fixture, 1, info, sizeof info - 1);
    CHECK(![files rename:listing.firstObject name:@"taken.txt" error:&error]);
    files.fixture = [FixtureService new]; reply(files.fixture, 1, 1, 0);
    CHECK([files remove:listing.firstObject error:&error]);
    CHECK(get64((const uint8_t *)files.fixture.output.bytes + 32) == 8);
    CHECK([NSFileManager.defaultManager removeItemAtURL:folder error:&error]);
}
static void testService(void) {
    int fds[2]; CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    nextFD = fds[0];
    VMUSBService *service = [[VMUSBService alloc] initWithTransport:[VMUSBTransport new] port:62078 error:NULL];
    CHECK(service != nil && lastAllowGuestWake);
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
    CHECK([error.localizedDescription isEqual:@"Operation canceled."]);
    service.canceled = nil;
    CHECK(![service readCount:4194305 error:&error]);
    const uint8_t large[4] = {0xff,0xff,0xff,0xff};
    CHECK(write(fds[1], large, 4) == 4);
    CHECK(![service receivePlist:&error]);
    close(fds[1]); CHECK(![service readCount:1 error:&error]);
    [service close]; [service close];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    nextFD = fds[0];
    service = [[VMUSBService alloc] initWithTransport:[VMUSBTransport new] port:64322
        canceled:nil deadline:NSProcessInfo.processInfo.systemUptime+1 allowGuestWake:NO error:NULL];
    CHECK(service != nil && !lastAllowGuestWake);
    [service close]; close(fds[1]);

    // An expired connection deadline never opens a transport or masquerades
    // as user cancellation. The same bound applies after the socket connects.
    NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    service = [[VMUSBService alloc] initWithTransport:[VMUSBTransport new] port:64321
        canceled:nil deadline:now-1 error:&error];
    CHECK(!service && [error.localizedDescription containsString:@"did not respond in time"]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    nextFD=fds[0]; now=NSProcessInfo.processInfo.systemUptime;
    service = [[VMUSBService alloc] initWithTransport:[VMUSBTransport new] port:64321
        canceled:nil deadline:now+0.05 error:&error];
    CHECK(service != nil);
    CHECK(![service readCount:1 error:&error]);
    CHECK(NSProcessInfo.processInfo.systemUptime-now < 2);
    CHECK(![error.localizedDescription containsString:@"canceled"]);
    CHECK(![service writeData:[NSData dataWithBytes:"x" length:1] error:&error]);
    CHECK([error.localizedDescription containsString:@"did not respond in time"]);
    service.canceled=^BOOL{ return YES; };
    CHECK(![service readCount:1 error:&error] && [error.localizedDescription isEqual:@"Operation canceled."]);
    [service close]; close(fds[1]);
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
    // Two first-time users must not generate different host records for one
    // guest. Each worker uses the actual atomic persistence implementation.
    NSString *concurrentID = NSUUID.UUID.UUIDString;
    NSMutableArray *records = [NSMutableArray array];
    dispatch_apply(4, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(size_t i) {
        (void)i;
        VMUSBPairing *other = [[VMUSBPairing alloc] initWithInstanceID:concurrentID
            devicePublicKey:der directory:folder error:NULL];
        @synchronized(records) { [records addObject:other.record ?: @{}]; }
    });
    CHECK(records.count == 4 && [records[0] count] > 0);
    for (NSDictionary *record in records) CHECK([record isEqual:records[0]]);
    CFRelease(publicKey); CFRelease(key);
}
@interface AppsService : ClockService
@property(nonatomic) BOOL closed;
@end
@implementation AppsService
- (BOOL)sendPlist:(NSDictionary *)query error:(NSError **)error { (void)error; [self.queries addObject:query]; return YES; }
- (NSDictionary *)receivePlist:(NSError **)error { (void)error;
    if (!self.replies.count) return nil; NSDictionary *reply=self.replies.firstObject;
    [self.replies removeObjectAtIndex:0]; return reply;
}
- (void)close { self.closed=YES; }
@end
@interface AppsFixture : VMGuestApplications
@property(nonatomic,strong) AppsService *service;
@end
@implementation AppsFixture
- (VMUSBService *)connect:(NSError **)error { (void)error; return self.service; }
@end
static void testGuestApps(void) {
    NSString *home=@"/var/mobile/Applications/01234567-89AB-CDEF-0123-456789ABCDEF";
    NSDictionary *record=@{@"CFBundleIdentifier":@"org.s5lbox.fixture",@"CFBundleDisplayName":@"Fixture",
        @"CFBundleVersion":@"1",@"Path":[home stringByAppendingString:@"/Actual Name.app"]};
    VMGuestApplication *app=[VMGuestApplication fromInstallationRecord:record];
    CHECK([app.name isEqual:@"Fixture"] && [app.homeDirectory isEqual:home]);
    CHECK([app.bundleDirectory isEqual:@"Actual Name.app"] && [app.version isEqual:@"1"]);
    for (NSString *path in @[@"/Applications/Cydia.app",[home stringByAppendingString:@"/../Outside.app"],
        [home stringByAppendingString:@"/One.app/Other.app"],@"/var/mobile/Applications/invalid/One.app",
        [home stringByAppendingString:@"/One.app/"],@"/var/mobile/Applications//One.app"]) {
        NSMutableDictionary *bad=[record mutableCopy]; bad[@"Path"]=path;
        CHECK([VMGuestApplication fromInstallationRecord:bad]==nil);
    }
    NSMutableDictionary *alias=[record mutableCopy]; alias[@"Path"]=[@"/private" stringByAppendingString:record[@"Path"]];
    CHECK([[VMGuestApplication fromInstallationRecord:alias].homeDirectory isEqual:home]);
    AppsFixture *fixture=[AppsFixture new]; fixture.service=[AppsService new];
    NSMutableArray *stages=[NSMutableArray array];
    fixture.progress=^(VMGuestApplicationsStage stage) { [stages addObject:@(stage)]; };
    [fixture.service.replies addObjectsFromArray:@[@{@"CurrentList":@[record],@"CurrentAmount":@1},@{@"Status":@"Complete"}]];
    NSArray *apps=[fixture installedApplications:NULL];
    CHECK(apps.count==1 && fixture.service.closed);
    CHECK(([stages isEqual:@[@(VMGuestApplicationsConnecting),@(VMGuestApplicationsReading)]]));
    CHECK([fixture.service.queries[0][@"Command"] isEqual:@"Browse"]);
    // A connection failure must not claim that the installed-app list is loading.
    [stages removeAllObjects]; fixture.service=nil;
    CHECK([fixture installedApplications:NULL]==nil);
    CHECK(([stages isEqual:@[@(VMGuestApplicationsConnecting)]]));
    for (NSDictionary *bad in @[@{@"CurrentList":@[record],@"CurrentAmount":@2},
        @{@"CurrentList":@[record,record],@"Status":@"Complete"},@{@"CurrentList":@"invalid"},@{@"Error":@"Failed"}]) {
        fixture.service=[AppsService new]; [fixture.service.replies addObject:bad]; NSError *error=nil;
        CHECK([fixture installedApplications:&error]==nil && error!=nil && fixture.service.closed);
    }
    fixture.service=[AppsService new]; fixture.canceled=YES; NSError *error=nil;
    CHECK([fixture installedApplications:&error]==nil && error!=nil && fixture.service.closed);
}
int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        testAFC(); testBrowserAFC(); testService(); testClock(); testTimeZone(); testGuestApps();
        testPairing([NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES]);
        testGuestFiles([NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]] isDirectory:YES]);
        printf("Native USB services: %u checks, %u failures\n", checks, failures);
        return failures ? 1 : 0;
    }
}
