// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMUSBLockdown.h"
#import "VMUSBService.h"
#import "VMUSBPairing.h"
#include <math.h>

static NSError *LockdownError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.GuestServices" code:1
                          userInfo:@{NSLocalizedDescriptionKey:message}];
}
static BOOL VMUSBValidCalendarValue(id value) {
    return [value isKindOfClass:NSNumber.class] &&
        CFGetTypeID((__bridge CFTypeRef)value) != CFBooleanGetTypeID() &&
        isfinite([value doubleValue]) && [value doubleValue] >= 0;
}
static NSTimeZone *VMUSBValidatedTimeZone(id identifier) {
    if (![identifier isKindOfClass:NSString.class] || ![identifier length] || [identifier length] > 128) return nil;
    // Legacy lockdownd resolves this name below /usr/share/zoneinfo. Reject
    // path traversal and anything other than timezone identifier characters.
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:
        @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789/_+-"];
    if ([identifier rangeOfCharacterFromSet:allowed.invertedSet].location != NSNotFound) return nil;
    for (NSString *component in [identifier componentsSeparatedByString:@"/"])
        if (!component.length) return nil;
    return [NSTimeZone timeZoneWithName:identifier];
}
@implementation VMUSBLockdown {
    VMUSBService *_service;
    VMUSBTransport *_transport;
    VMUSBPairing *_pairing;
    BOOL (^_canceled)(void);
    NSTimeInterval _deadline;
}
- (instancetype)initWithService:(VMUSBService *)service {
    self = [super init]; if (self) _service = service; return self;
}
- (void)dealloc { [self close]; }
- (void)close { [_service close]; _service = nil; }
- (NSDictionary *)request:(NSString *)request values:(NSDictionary *)values error:(NSError **)error {
    NSMutableDictionary *query = [@{@"Label":@"S5LBox", @"Request":request} mutableCopy];
    if (values) [query addEntriesFromDictionary:values];
    NSDictionary *reply = [_service request:query error:error];
    if (!reply) return nil;
    if (![reply[@"Request"] isEqual:request] || reply[@"Error"] ||
        (reply[@"Result"] && ![reply[@"Result"] isEqual:@"Success"])) {
        if (error) *error = LockdownError([NSString stringWithFormat:@"Guest %@ failed: %@.", request,
            reply[@"Error"] ?: reply[@"Result"] ?: @"invalid response"]);
        return nil;
    }
    return reply;
}
+ (instancetype)openWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier
                         canceled:(BOOL (^)(void))canceled deadline:(NSTimeInterval)deadline
                            error:(NSError **)error {
    VMUSBService *service = [[VMUSBService alloc] initWithTransport:transport port:62078
        canceled:canceled deadline:deadline error:error];
    if (!service) return nil;
    VMUSBLockdown *client = [[self alloc] initWithService:service];
    client->_transport = transport; client->_canceled = [canceled copy]; client->_deadline = deadline;
    NSDictionary *type = [client request:@"QueryType" values:nil error:error];
    if (!type) return nil;
    if (![type[@"Type"] isEqual:@"com.apple.mobile.lockdown"]) {
        if (error) *error = LockdownError(@"The guest did not identify its pairing service."); return nil;
    }
    NSDictionary *key = [client request:@"GetValue" values:@{@"Key":@"DevicePublicKey"} error:error];
    if (!key) return nil;
    if (![key[@"Value"] isKindOfClass:NSData.class]) {
        if (error) *error = LockdownError(@"The guest did not return a pairing key."); return nil;
    }
    client->_pairing = [[VMUSBPairing alloc] initWithInstanceID:identifier devicePublicKey:key[@"Value"] error:error];
    if (!client->_pairing) return nil;
    NSDictionary *record = client->_pairing.record;
    NSDictionary *pairValues = @{@"PairRecord":record, @"ProtocolVersion":@"2"};
    // A host identity does not prove this guest checkpoint has accepted it.
    if (![client request:@"Pair" values:pairValues error:error] ||
        ![client request:@"ValidatePair" values:pairValues error:error]) return nil;
    NSDictionary *session = [client request:@"StartSession"
        values:@{@"HostID":record[@"HostID"], @"SystemBUID":record[@"SystemBUID"]} error:error];
    if (!session) return nil;
    if (![session[@"SessionID"] isKindOfClass:NSString.class] || ![session[@"SessionID"] length]) {
        if (error) *error = LockdownError(@"The guest returned an invalid session."); return nil;
    }
    if ([session[@"EnableSessionSSL"] boolValue] &&
        ![service enableTLSWithIdentity:client->_pairing.identity expectedKey:client->_pairing.deviceKey error:error]) return nil;
    return client;
}
- (VMUSBService *)openService:(NSString *)name error:(NSError **)error {
    if (!_pairing) { if (error) *error = LockdownError(@"The guest session is not paired."); return nil; }
    NSDictionary *reply = [self request:@"StartService" values:@{@"Service":name} error:error];
    if (!reply) return nil;
    NSNumber *port = reply[@"Port"];
    if (![port isKindOfClass:NSNumber.class] || port.integerValue < 1 || port.integerValue > 65535) {
        if (error) *error = LockdownError(@"The guest returned an invalid service port."); return nil;
    }
    VMUSBService *service = [[VMUSBService alloc] initWithTransport:_transport port:port.unsignedShortValue
        canceled:_canceled deadline:_deadline error:error];
    if ([reply[@"EnableServiceSSL"] boolValue] &&
        ![service enableTLSWithIdentity:_pairing.identity expectedKey:_pairing.deviceKey error:error]) return nil;
    return service;
}
- (BOOL)synchronizeClockWithNow:(NSTimeInterval (^)(void))now error:(NSError **)error {
    NSDictionary *reply = [self request:@"GetValue" values:@{@"Key":@"TimeIntervalSince1970"} error:error];
    if (!reply) return NO;
    NSNumber *old = reply[@"Value"];
    NSTimeInterval target = now ? now() : NAN;
    if (!VMUSBValidCalendarValue(old) || !isfinite(target) || target < 946684800.0 || target >= 4102444800.0) {
        if (error) *error = LockdownError(@"The guest or host returned an invalid calendar time."); return NO;
    }
    // Legacy lockdown expects the original plist number type (integer/real).
    BOOL real = CFNumberIsFloatType((__bridge CFNumberRef)old);
    NSNumber *value = real ? @(target) : @((uint64_t)target);
    if (![self request:@"SetValue" values:@{@"Key":@"TimeIntervalSince1970", @"Value":value} error:error]) return NO;
    reply = [self request:@"GetValue" values:@{@"Key":@"TimeIntervalSince1970"} error:error];
    if (!reply) return NO;
    NSNumber *actual = reply[@"Value"];
    NSTimeInterval current = now();
    if (!VMUSBValidCalendarValue(actual) || !isfinite(current) || current < target ||
        actual.doubleValue < target - 2 || actual.doubleValue > current + 2 ||
        fabs(actual.doubleValue - current) > 10) {
        if (error) *error = LockdownError(@"The guest calendar did not confirm the current time."); return NO;
    }
    return YES;
}
- (BOOL)synchronizeTimeZone:(NSString *)identifier now:(NSTimeInterval (^)(void))now error:(NSError **)error {
    NSTimeZone *zone = VMUSBValidatedTimeZone(identifier);
    if (!zone || !now) {
        if (error) *error = LockdownError(@"The host returned an invalid timezone."); return NO;
    }
    NSDictionary *reply = [self request:@"GetValue" values:@{@"Key":@"TimeZone"} error:error];
    if (!reply) return NO;
    NSString *old = reply[@"Value"];
    if (![old isKindOfClass:NSString.class] || !old.length) {
        if (error) *error = LockdownError(@"The guest returned an invalid timezone."); return NO;
    }
    if (![old isEqualToString:identifier]) {
        if (![self request:@"SetValue" values:@{@"Key":@"TimeZone", @"Value":identifier} error:error]) return NO;
        reply = [self request:@"GetValue" values:@{@"Key":@"TimeZone"} error:error];
        if (!reply) return NO;
        if (![reply[@"Value"] isEqual:identifier]) {
            if (error) *error = LockdownError(@"The guest did not confirm the host timezone."); return NO;
        }
    }
    reply = [self request:@"GetValue" values:@{@"Key":@"TimeZoneOffsetFromUTC"} error:error];
    if (!reply) return NO;
    id offset = reply[@"Value"];
    NSTimeInterval current = now();
    if (![offset isKindOfClass:NSNumber.class] ||
        CFGetTypeID((__bridge CFTypeRef)offset) == CFBooleanGetTypeID() ||
        !isfinite([offset doubleValue]) || !isfinite(current) ||
        current < 946684800.0 || current >= 4102444800.0) {
        if (error) *error = LockdownError(@"The guest or host returned an invalid timezone offset."); return NO;
    }
    NSInteger expected = [zone secondsFromGMTForDate:[NSDate dateWithTimeIntervalSince1970:current]];
    if ([offset doubleValue] != expected) {
        if (error) *error = LockdownError(@"The guest timezone rules do not match the host's current UTC offset."); return NO;
    }
    return YES;
}
@end
