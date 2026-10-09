// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMUSBAFC.h"
#import "VMUSBService.h"
#include <string.h>

static void Put64(uint8_t *p, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(value >> (8 * i));
}
static uint64_t Get64(const uint8_t *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; i++) value |= (uint64_t)p[i] << (8 * i);
    return value;
}
static NSData *Word(uint64_t value) {
    uint8_t p[8]; Put64(p, value); return [NSData dataWithBytes:p length:8];
}
static NSError *AFCError(NSInteger code, NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.AFC" code:code userInfo:@{NSLocalizedDescriptionKey:message}];
}
static NSData *Path(NSString *path) {
    // This client can only modify the one private staging namespace.
    if (![path hasPrefix:@"PublicStaging/S5LBox-"] || ![path hasSuffix:@".ipa"] ||
        path.length > 120 || [path rangeOfString:@"/" options:0 range:NSMakeRange(14, path.length - 14)].location != NSNotFound)
        return nil;
    NSData *bytes = [path dataUsingEncoding:NSUTF8StringEncoding];
    if (!bytes || memchr(bytes.bytes, 0, bytes.length)) return nil;
    NSMutableData *result = [bytes mutableCopy]; uint8_t zero = 0;
    [result appendBytes:&zero length:1]; return result;
}
@implementation VMUSBAFC {
    VMUSBService *_service;
    uint64_t _sequence;
}
- (instancetype)initWithService:(VMUSBService *)service {
    self = [super init]; if (self) _service = service; return self;
}
- (NSData *)operation:(uint64_t)operation arguments:(NSData *)arguments payload:(NSData *)payload
                reply:(uint64_t)expected allowExists:(BOOL)exists error:(NSError **)error {
    if (!arguments || payload.length > 65536) {
        if (error) *error = AFCError(-1, @"Invalid AFC request."); return nil;
    }
    uint8_t header[40] = {'C','F','A','6','L','P','A','A'};
    Put64(header + 8, 40 + arguments.length + payload.length);
    Put64(header + 16, 40 + arguments.length);
    Put64(header + 24, ++_sequence); Put64(header + 32, operation);
    if (![_service writeData:[NSData dataWithBytes:header length:40] error:error] ||
        ![_service writeData:arguments error:error] ||
        (payload.length && ![_service writeData:payload error:error])) return nil;
    NSData *response = [_service readCount:40 error:error];
    if (response.length != 40) return nil;
    const uint8_t *p = response.bytes;
    uint64_t length = Get64(p + 8), head = Get64(p + 16), code = Get64(p + 32);
    if (memcmp(p, "CFA6LPAA", 8) || Get64(p + 24) != _sequence ||
        length < 40 || length > 4u * 1024u * 1024u || head < 40 || head > length) {
        if (error) *error = AFCError(-1, @"The guest returned an invalid AFC packet."); return nil;
    }
    NSData *body = [_service readCount:(NSUInteger)(length - 40) error:error];
    if (!body) return nil;
    if (code == 1 && body.length == 8) {
        uint64_t status = Get64(body.bytes);
        if (expected == 1 && (!status || (exists && status == 16))) return body;
        if (error) *error = AFCError((NSInteger)status,
            [NSString stringWithFormat:@"Guest file transfer failed (AFC %llu).", (unsigned long long)status]);
        return nil;
    }
    if (code != expected || body.length != 8) {
        if (error) *error = AFCError(-1, @"The guest returned an unexpected AFC response."); return nil;
    }
    return body;
}
- (BOOL)makeStagingDirectory:(NSError **)error {
    return [self operation:9 arguments:[NSData dataWithBytes:"PublicStaging" length:14]
                   payload:nil reply:1 allowExists:YES error:error] != nil;
}
- (uint64_t)openPath:(NSString *)path error:(NSError **)error {
    NSData *name = Path(path);
    if (!name) { if (error) *error = AFCError(-1, @"Invalid staging path."); return 0; }
    NSMutableData *arguments = [Word(3) mutableCopy]; // write-only, create/truncate
    [arguments appendData:name];
    NSData *result = [self operation:13 arguments:arguments payload:nil reply:14 allowExists:NO error:error];
    uint64_t handle = result ? Get64(result.bytes) : 0;
    if (result && !handle && error) *error = AFCError(-1, @"The guest returned an invalid file handle.");
    return handle;
}
- (BOOL)writeHandle:(uint64_t)handle data:(NSData *)data error:(NSError **)error {
    return [self operation:16 arguments:handle ? Word(handle) : nil payload:data reply:1 allowExists:NO error:error] != nil;
}
- (BOOL)closeHandle:(uint64_t)handle error:(NSError **)error {
    return [self operation:20 arguments:handle ? Word(handle) : nil payload:nil reply:1 allowExists:NO error:error] != nil;
}
- (BOOL)removePath:(NSString *)path error:(NSError **)error {
    return [self operation:8 arguments:Path(path) payload:nil reply:1 allowExists:NO error:error] != nil;
}
@end
