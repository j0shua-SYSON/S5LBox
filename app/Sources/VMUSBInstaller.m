// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMUSBInstaller.h"
#import "VMUSBService.h"
#import "VMUSBPairing.h"
#import "VMUSBAFC.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static NSError *InstallError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.GuestInstaller" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
}
@implementation VMUSBInstaller {
    VMUSBTransport *_transport;
    NSString *_identifier;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier {
    self = [super init]; if (self) { _transport = transport; _identifier = [identifier copy]; } return self;
}
- (void)report:(NSString *)message fraction:(double)fraction {
    if (self.progress) self.progress(message, fraction);
}
- (NSDictionary *)lockdown:(VMUSBService *)service request:(NSString *)request
                       values:(NSDictionary *)values error:(NSError **)error {
    NSMutableDictionary *query = [@{@"Label":@"S5LBox", @"Request":request} mutableCopy];
    if (values) [query addEntriesFromDictionary:values];
    NSDictionary *reply = [service request:query error:error];
    if (!reply) return nil;
    if (![reply[@"Request"] isEqual:request] || reply[@"Error"] ||
        (reply[@"Result"] && ![reply[@"Result"] isEqual:@"Success"])) {
        if (error) *error = InstallError([NSString stringWithFormat:@"Guest %@ failed: %@.", request,
                                        reply[@"Error"] ?: reply[@"Result"] ?: @"invalid response"]);
        return nil;
    }
    return reply;
}
- (VMUSBService *)openService:(NSString *)name lockdown:(VMUSBService *)lockdown
                     pairing:(VMUSBPairing *)pairing error:(NSError **)error {
    NSDictionary *reply = [self lockdown:lockdown request:@"StartService" values:@{@"Service":name} error:error];
    if (!reply) return nil;
    NSNumber *port = reply[@"Port"];
    if (![port isKindOfClass:NSNumber.class] || port.integerValue < 1 || port.integerValue > 65535) {
        if (error) *error = InstallError(@"The guest returned an invalid service port."); return nil;
    }
    VMUSBService *service = [[VMUSBService alloc] initWithTransport:_transport port:port.unsignedShortValue error:error];
    __weak VMUSBInstaller *weakSelf = self;
    service.canceled = ^BOOL{ return weakSelf.canceled; };
    if ([reply[@"EnableServiceSSL"] boolValue] &&
        ![service enableTLSWithIdentity:pairing.identity expectedKey:pairing.deviceKey error:error]) return nil;
    return service;
}
- (BOOL)installURL:(NSURL *)url error:(NSError **)error {
    if (NSThread.isMainThread) { if (error) *error = InstallError(@"Installation requires a worker thread."); return NO; }
    int fd = url.isFileURL ? open(url.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC) : -1;
    struct stat initial;
    if (fd < 0 || fstat(fd, &initial) || !S_ISREG(initial.st_mode) || initial.st_size < 1 || initial.st_size > 512ll * 1024 * 1024) {
        if (fd >= 0) close(fd);
        if (error) *error = InstallError(@"Choose a regular IPA file no larger than 512 MB."); return NO;
    }
    BOOL complete = NO, submitted = NO, terminal = NO, staged = NO;
    VMUSBService *lockdown = nil, *files = nil, *installer = nil;
    VMUSBAFC *afc = nil;
    NSString *path = [NSString stringWithFormat:@"PublicStaging/S5LBox-%@.ipa", NSUUID.UUID.UUIDString];
    NSError *failure = nil;
    __weak VMUSBInstaller *weakSelf = self;
    do {
        if (self.canceled) { failure = InstallError(@"Installation canceled."); break; }
        [self report:@"Connecting to the guest’s virtual USB…" fraction:-1];
        lockdown = [[VMUSBService alloc] initWithTransport:_transport port:62078 error:&failure];
        if (!lockdown) break;
        lockdown.canceled = ^BOOL{ return weakSelf.canceled; };
        NSDictionary *type = [self lockdown:lockdown request:@"QueryType" values:nil error:&failure];
        if (!type) break;
        if (![type[@"Type"] isEqual:@"com.apple.mobile.lockdown"]) {
            failure = InstallError(@"The guest did not identify its pairing service."); break;
        }
        NSDictionary *key = [self lockdown:lockdown request:@"GetValue" values:@{@"Key":@"DevicePublicKey"} error:&failure];
        if (!key) break;
        if (![key[@"Value"] isKindOfClass:NSData.class]) { failure = InstallError(@"The guest did not return a pairing key."); break; }
        [self report:@"Pairing with the guest…" fraction:-1];
        VMUSBPairing *pairing = [[VMUSBPairing alloc] initWithInstanceID:_identifier devicePublicKey:key[@"Value"] error:&failure];
        if (!pairing) break;
        NSDictionary *pairValues = @{@"PairRecord":pairing.record, @"ProtocolVersion":@"2"};
        // Legacy guests require ValidatePair. Reuse an already accepted identity
        // without rewriting the guest pair record on every installation.
        NSDictionary *validated = [self lockdown:lockdown request:@"ValidatePair" values:pairValues error:NULL];
        if (!validated && (![self lockdown:lockdown request:@"Pair" values:pairValues error:&failure] ||
                           ![self lockdown:lockdown request:@"ValidatePair" values:pairValues error:&failure])) break;
        NSDictionary *session = [self lockdown:lockdown request:@"StartSession"
            values:@{@"HostID":pairing.record[@"HostID"], @"SystemBUID":pairing.record[@"SystemBUID"]} error:&failure];
        if (!session) break;
        if (![session[@"SessionID"] isKindOfClass:NSString.class]) { failure = InstallError(@"The guest returned an invalid session."); break; }
        if ([session[@"EnableSessionSSL"] boolValue] &&
            ![lockdown enableTLSWithIdentity:pairing.identity expectedKey:pairing.deviceKey error:&failure]) break;
        [self report:@"Opening the guest’s installation services…" fraction:-1];
        files = [self openService:@"com.apple.afc" lockdown:lockdown pairing:pairing error:&failure];
        if (!files) break;
        installer = [self openService:@"com.apple.mobile.installation_proxy" lockdown:lockdown pairing:pairing error:&failure];
        if (!installer) break;
        // Disconnecting this session leaves the independently opened services
        // alive and releases a mux slot. No host-side guest filesystem access.
        [lockdown close]; lockdown = nil;
        afc = [[VMUSBAFC alloc] initWithService:files];
        if (![afc makeStagingDirectory:&failure]) break;
        staged = YES; // open may reach the guest even if its reply is lost
        uint64_t handle = [afc openPath:path error:&failure];
        if (!handle) break;
        uint8_t buffer[65536]; off_t sent = 0;
        while (sent < initial.st_size) {
            ssize_t n = read(fd, buffer, (size_t)MIN((off_t)sizeof buffer, initial.st_size - sent));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { failure = InstallError(@"The IPA changed or could not be read during upload."); break; }
            if (![afc writeHandle:handle data:[NSData dataWithBytes:buffer length:(NSUInteger)n] error:&failure]) break;
            sent += n;
            [self report:@"Sending IPA to the guest…" fraction:0.75 * (double)sent / (double)initial.st_size];
        }
        if (failure || ![afc closeHandle:handle error:&failure]) break;
        struct stat final;
        if (fstat(fd, &final) || final.st_size != initial.st_size ||
            final.st_mtimespec.tv_sec != initial.st_mtimespec.tv_sec || final.st_mtimespec.tv_nsec != initial.st_mtimespec.tv_nsec) {
            failure = InstallError(@"The IPA changed during upload. Try again with an unchanged file."); break;
        }
        [self report:@"The guest is installing the app…" fraction:0.75];
        submitted = YES; // even a failed write can have reached the installer
        if (![installer sendPlist:@{@"Command":@"Install", @"PackagePath":path,
                                   @"ClientOptions":@{@"ApplicationType":@"User"}} error:&failure]) break;
        NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 600;
        while (NSProcessInfo.processInfo.systemUptime < deadline) {
            NSDictionary *reply = [installer receivePlist:&failure];
            if (!reply) break;
            if (reply[@"Error"]) {
                terminal = YES;
                failure = InstallError([NSString stringWithFormat:@"The guest rejected this IPA: %@. %@",
                    reply[@"Error"], reply[@"ErrorDescription"] ?: @"Its signing and iPhone OS compatibility must be accepted by the guest."]);
                break;
            }
            if ([reply[@"Status"] isEqual:@"Complete"]) { complete = terminal = YES; break; }
            NSNumber *percent = reply[@"PercentComplete"];
            double fraction = [percent isKindOfClass:NSNumber.class] ? 0.75 + 0.25 * MAX(0, MIN(100, percent.doubleValue)) / 100.0 : -1;
            NSString *status = [reply[@"Status"] isKindOfClass:NSString.class] ? reply[@"Status"] : @"Installing";
            [self report:[NSString stringWithFormat:@"Guest: %@…", status] fraction:fraction];
        }
        if (!complete && !failure) failure = InstallError(@"The guest installer did not finish within ten minutes.");
    } while (0);
    close(fd);
    // Only this attempt's unique file, and never while installation may still
    // be running. An interrupted request is explicitly an unknown outcome.
    if (staged && (!submitted || terminal)) {
        files.canceled = nil;
        [afc removePath:path error:NULL];
    }
    [lockdown close]; [files close]; [installer close];
    if (!complete && submitted && !terminal)
        failure = InstallError([NSString stringWithFormat:@"%@ The guest may still finish installing. Check its Home screen before retrying.",
                                failure.localizedDescription ?: @"The connection ended."]);
    if (!complete && error) *error = failure ?: InstallError(@"Installation did not finish.");
    if (complete) [self report:@"Installed by the guest. Check its Home screen to open the app." fraction:1];
    return complete;
}
@end
