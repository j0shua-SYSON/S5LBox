#import "VMPackageBridge.h"
#import "VMPackageCatalog.h"
#import "VMUSBService.h"
#import <Security/Security.h>
#import <CommonCrypto/CommonDigest.h>
#include "VMGuestInstall.h"
#include "VMGuestFilesService.h"
#include "VMSnapshotStore.h"
#include "VMResumeCheckpoint.h"
#include "rootfs_work.h"
#include "sha256.h"
#include <arpa/inet.h>
#include <errno.h>

typedef struct {
    __unsafe_unretained VMPackageSetupProgress progress;
    NSTimeInterval last;
} PackageCopyProgress;
static void PackageCopy(void *context, uint64_t done, uint64_t total) {
    PackageCopyProgress *state = context;
    NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    if (!state->progress || (now - state->last < 0.1 && done != total)) return;
    state->last = now;
    state->progress(@"Preparing package files", total ? (double)done / total : -1);
}

static NSURL *CapabilityURL(NSString *identifier, NSError **error) {
    if (identifier.length != 16 || [identifier rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location != NSNotFound) {
        if (error) *error = VMPackageError(@"Invalid machine identity."); return nil;
    }
    NSURL *base = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *folder = [base URLByAppendingPathComponent:@"PackageCapabilities" isDirectory:YES];
    return [folder URLByAppendingPathComponent:identifier];
}
static NSData *Capability(NSString *identifier, BOOL create, NSError **error) {
    NSURL *path = CapabilityURL(identifier,error); if (!path) return nil;
    NSURL *folder = path.URLByDeletingLastPathComponent;
    NSData *data = [NSData dataWithContentsOfURL:path options:0 error:NULL];
    if (data.length == 32) return data;
    if (!create || [NSFileManager.defaultManager fileExistsAtPath:path.path]) {
        if (error) *error = VMPackageError(@"This machine needs package setup. Choose Set up Packages in the package manager."); return nil;
    }
    unsigned char bytes[32];
    if (SecRandomCopyBytes(kSecRandomDefault,sizeof bytes,bytes) != errSecSuccess) {
        if (error) *error = VMPackageError(@"Could not create a private guest capability."); return nil;
    }
    data = [NSData dataWithBytes:bytes length:sizeof bytes];
    if (![NSFileManager.defaultManager createDirectoryAtURL:folder withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:error] ||
        ![data writeToURL:path options:NSDataWritingAtomic error:error] ||
        ![NSFileManager.defaultManager setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:path.path error:error]) return nil;
    return data;
}
static BOOL PackageSetupPending(NSString *identifier, BOOL pending, NSError **error) {
    NSURL *token = CapabilityURL(identifier,error); if (!token) return NO;
    NSURL *marker = [token URLByAppendingPathExtension:@"pending"];
    if (!pending) {
        if (![NSFileManager.defaultManager fileExistsAtPath:marker.path]) return YES;
        return [NSFileManager.defaultManager removeItemAtURL:marker error:error];
    }
    return [NSFileManager.defaultManager createDirectoryAtURL:token.URLByDeletingLastPathComponent
        withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:error] &&
        [[NSData data] writeToURL:marker options:NSDataWritingAtomic error:error];
}
static NSString *FirstConnectionKey(NSString *identifier) {
    if (!CapabilityURL(identifier,NULL)) return nil;
    return [@"PackageManagerAwaitingFirstConnection-" stringByAppendingString:identifier];
}
static void AwaitFirstConnection(NSString *identifier) {
    NSString *key=FirstConnectionKey(identifier);
    if (key) [NSUserDefaults.standardUserDefaults setBool:YES forKey:key];
}
@implementation VMPackageBridge {
    VMUSBTransport *_transport;
    NSString *_identifier;
}
+ (BOOL)hasCapabilityForInstance:(NSString *)identifier {
    NSURL *token = CapabilityURL(identifier,NULL);
    return token && ![NSFileManager.defaultManager fileExistsAtPath:
        [token URLByAppendingPathExtension:@"pending"].path] && Capability(identifier, NO, NULL).length == 32;
}
+ (BOOL)awaitingFirstConnectionForInstance:(NSString *)identifier {
    NSString *key=FirstConnectionKey(identifier);
    return key && [NSUserDefaults.standardUserDefaults boolForKey:key];
}
+ (void)noteSuccessfulConnectionForInstance:(NSString *)identifier {
    NSString *key=FirstConnectionKey(identifier);
    if (key) [NSUserDefaults.standardUserDefaults removeObjectForKey:key];
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier {
    self = [super init]; if (self) { _transport = transport; _identifier = [identifier copy]; } return self;
}
- (VMUSBService *)connect:(char)operation error:(NSError **)error {
    if (NSThread.isMainThread) { if (error) *error = VMPackageError(@"Package work requires a worker thread."); return nil; }
    NSData *capability = Capability(_identifier,NO,error); if (!capability) return nil;
    __weak VMPackageBridge *weakSelf = self;
    NSTimeInterval statusDeadline=(operation=='S'||operation=='T') ? NSProcessInfo.processInfo.systemUptime+10 : 0;
    VMUSBService *service = [[VMUSBService alloc] initWithTransport:_transport port:operation=='T'?64322:64321
        canceled:^BOOL{ return weakSelf.canceled; } deadline:statusDeadline allowGuestWake:operation!='T' error:error];
    if (!service) return nil;
    NSMutableData *hello = [NSMutableData dataWithBytes:"SPM2" length:4];
    [hello appendData:capability]; [hello appendBytes:&operation length:1];
    if (![service writeData:hello error:error]) { [service close]; return nil; }
    return service;
}
- (NSData *)exchangeAppRoute:(NSData *)request error:(NSError **)error {
    if (request.length<27 || request.length>32923) {
        if (error) *error=VMPackageError(@"Invalid app routing request."); return nil;
    }
    VMUSBService *service=[self connect:'T' error:error]; if (!service) return nil;
    uint32_t length=htonl((uint32_t)request.length);
    NSMutableData *packet=[NSMutableData dataWithBytes:&length length:4]; [packet appendData:request];
    NSData *reply=[service writeData:packet error:error]?[self receive:service terminal:'T' error:error]:nil;
    [service close];
    if (reply && (reply.length<18 || reply.length>273 || memcmp(reply.bytes,"HLR1",4))) {
        if (error) *error=VMPackageError(@"Invalid guest app routing response."); return nil;
    }
    return reply;
}
- (NSData *)receive:(VMUSBService *)service terminal:(char)terminal error:(NSError **)error {
    NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 1800;
    while (NSProcessInfo.processInfo.systemUptime < deadline) {
        NSData *header = [service readCount:4 error:error]; if (!header) return nil;
        uint32_t n; memcpy(&n,header.bytes,4); n = ntohl(n);
        if (n < 1 || n > 4u*1024u*1024u+1) { if (error) *error = VMPackageError(@"Invalid guest package response."); return nil; }
        NSData *frame = [service readCount:n error:error]; if (!frame) return nil;
        char kind = ((const char *)frame.bytes)[0]; NSData *payload = [frame subdataWithRange:NSMakeRange(1,n-1)];
        if (kind == terminal) return payload;
        if (kind=='F' && payload.length==2) {
            const unsigned char *value=payload.bytes;
            if (value[0]<=4 && value[1]<=1) { _finishAction=value[0]; _finishReady=value[1]!=0; continue; }
        }
        NSString *message = [[NSString alloc] initWithData:payload encoding:NSUTF8StringEncoding] ?: @"Guest output could not be decoded.";
        if (kind == 'E') {
            _setupPending=[message isEqualToString:@"Jailbreak setup is still finishing. Wait for it to complete, then refresh Packages and retry."];
            if (error) *error = VMPackageError(message); return nil;
        }
        if (kind != 'L') { if (error) *error = VMPackageError(@"Unexpected guest package response."); return nil; }
        if (message.length && self.progress) self.progress(message);
    }
    if (error) *error = VMPackageError(@"The guest operation exceeded thirty minutes. Its outcome is unknown; check Installed before retrying.");
    return nil;
}
- (VMUSBService *)openLiveFilesForContainer:(NSString *)uuid error:(NSError **)error {
    _setupPending=NO;
    NSUUID *identity=[[NSUUID alloc] initWithUUIDString:uuid];
    if (uuid.length!=36 || !identity || [identity.UUIDString caseInsensitiveCompare:uuid]!=NSOrderedSame) {
        if (error) *error=VMPackageError(@"Invalid guest application container."); return nil;
    }
    VMUSBService *service=[self connect:'L' error:error];
    if (!service) return nil;
    [service setOperationDeadline:NSProcessInfo.processInfo.systemUptime+30];
    NSData *name=[uuid dataUsingEncoding:NSUTF8StringEncoding];
    if (![service writeData:name error:error]) { [service close]; return nil; }
    // The package supervisor can refuse the request before handing the socket
    // to the file broker. Its framed E response is not a broker errno packet.
    NSData *header=[service readCount:4 error:error];
    uint32_t length=0,status=0;
    if (header.length==4) memcpy(&length,header.bytes,4);
    length=ntohl(length);
    if (header.length!=4 || length<4 || length>4096) {
        if (error && !*error) *error=VMPackageError(@"Invalid guest file handshake.");
        [service close]; return nil;
    }
    NSData *reply=[service readCount:length error:error];
    if (reply.length!=length) { [service close]; return nil; }
    if (length!=4) {
        NSString *message=nil;
        if (((const uint8_t *)reply.bytes)[0]=='E')
            message=[[NSString alloc] initWithBytes:(const uint8_t *)reply.bytes+1 length:length-1 encoding:NSUTF8StringEncoding];
        _setupPending=[message isEqualToString:@"Jailbreak setup is still finishing. Wait for it to complete, then refresh Packages and retry."];
        if (error) *error=VMPackageError(message.length?message:@"Invalid guest file handshake.");
        [service close]; return nil;
    }
    memcpy(&status,reply.bytes,4);
    if (status) {
        if (error && !*error) *error=VMPackageError(ntohl(status)==EBUSY ?
            @"Return to the guest's Home screen and close any other app session, then retry." :
            @"The guest launch guard is unavailable. Set up touchHLE for this machine and restart it.");
        [service close]; return nil;
    }
    [service setOperationDeadline:0];
    return service;
}
- (NSData *)status:(NSError **)error {
    _finishAction=0; _finishReady=NO; _setupPending=NO;
    VMUSBService *service = [self connect:'S' error:error];
    NSData *data = service ? [self receive:service terminal:'S' error:error] : nil; [service close];
    if (!data && !self.canceled && !_setupPending && error) *error=VMPackageError([NSString stringWithFormat:
        @"Cannot connect to iPhone OS. %@",(*error).localizedDescription ?: @"Let it finish starting, then try again."]);
    return data;
}
- (BOOL)number:(uint32_t)n service:(VMUSBService *)service error:(NSError **)error {
    n = htonl(n); return [service writeData:[NSData dataWithBytes:&n length:4] error:error];
}
- (NSData *)change:(NSArray<NSURL *> *)archives remove:(NSString *)name status:(NSData *)status error:(NSError **)error {
    if (!status.length || status.length > 4u*1024u*1024u || (!name && (!archives.count || archives.count > 128)) || (name && !VMPackageIdentifierValid(name))) {
        if (error) *error = VMPackageError(@"Invalid package transaction."); return nil;
    }
    VMUSBService *service = [self connect:name ? 'R' : 'I' error:error]; if (!service) return nil;
    BOOL submitted = NO; NSData *result = nil;
    @try {
        unsigned char hash[32]; CC_SHA256(status.bytes,(CC_LONG)status.length,hash);
        if (![service writeData:[NSData dataWithBytes:hash length:32] error:error]) return nil;
        unsigned char hint=self.respringAfterChanges ? 2 : 0;
        if (![service writeData:[NSData dataWithBytes:&hint length:1] error:error]) return nil;
        if (name) {
            NSData *bytes = [name dataUsingEncoding:NSASCIIStringEncoding]; submitted = YES;
            if (![self number:(uint32_t)bytes.length service:service error:error] || ![service writeData:bytes error:error]) return nil;
        } else {
            if (![self number:(uint32_t)archives.count service:service error:error]) return nil;
            NSUInteger total = 0;
            for (NSURL *url in archives) {
                NSData *bytes = [NSData dataWithContentsOfURL:url options:NSDataReadingMappedIfSafe error:error];
                if (!bytes.length || bytes.length > 64u*1024u*1024u || total + bytes.length > 256u*1024u*1024u) {
                    if (error) *error = VMPackageError(@"Package transaction exceeds its transfer limit."); return nil;
                }
                total += bytes.length; CC_SHA256(bytes.bytes,(CC_LONG)bytes.length,hash);
                if (![self number:(uint32_t)bytes.length service:service error:error] ||
                    ![service writeData:[NSData dataWithBytes:hash length:32] error:error]) return nil;
                for (NSUInteger at=0;at<bytes.length;at+=65536) {
                    NSUInteger length = MIN((NSUInteger)65536,bytes.length-at);
                    if (url == archives.lastObject && at+length == bytes.length) submitted = YES;
                    if (![service writeData:[bytes subdataWithRange:NSMakeRange(at,length)] error:error]) return nil;
                }
                if (self.progress) self.progress(@"Transferred a verified package to the guest.\n");
            }
        }
        result = [self receive:service terminal:'D' error:error];
    } @finally {
        [service close];
        if (!result && submitted && error) *error = VMPackageError([NSString stringWithFormat:
            @"%@ The guest may have changed packages or may still be working. Refresh Installed before retrying.",(*error).localizedDescription ?: @"Connection ended."]);
    }
    return result;
}
- (NSData *)install:(NSArray<NSURL *> *)archives status:(NSData *)status error:(NSError **)error {
    return [self change:archives remove:nil status:status error:error];
}
- (NSData *)remove:(NSString *)package status:(NSData *)status error:(NSError **)error {
    return [self change:nil remove:package status:status error:error];
}
- (BOOL)performFinish:(NSData *)status error:(NSError **)error {
    unsigned char action=(unsigned char)self.finishAction;
    if (!action || action>4 || !self.finishReady || !status.length || status.length>4u*1024u*1024u) {
        if (error) *error=VMPackageError(@"No verified finish request. Refresh Installed first."); return NO;
    }
    VMUSBService *service=[self connect:'F' error:error]; if (!service) return NO;
    unsigned char hash[32]; CC_SHA256(status.bytes,(CC_LONG)status.length,hash);
    NSMutableData *request=[NSMutableData dataWithBytes:&action length:1]; [request appendBytes:hash length:32];
    BOOL ok=NO;
    if ([service writeData:request error:error]) {
        NSData *accepted=[self receive:service terminal:'A' error:error];
        ok=accepted.length==1 && ((const unsigned char *)accepted.bytes)[0]==action;
        // A reboot intentionally drops USB. Other actions must report that the
        // fixed guest command actually returned successfully, not just an ACK.
        if (ok && action!=4) {
            NSData *complete=[self receive:service terminal:'C' error:error];
            ok=complete.length==1 && ((const unsigned char *)complete.bytes)[0]==action;
        }
    }
    [service close];
    if (ok) { _finishAction=0; _finishReady=NO; }
    else if (error && !*error) *error=VMPackageError(@"Guest restart outcome is unknown. Refresh before retrying.");
    return ok;
}
+ (BOOL)prepareStoppedMachine:(NSString *)directory instanceID:(NSString *)identifier error:(NSError **)error {
    return [self prepareStoppedMachine:directory instanceID:identifier progress:nil error:error];
}
+ (BOOL)prepareStoppedMachine:(NSString *)directory instanceID:(NSString *)identifier
                    progress:(VMPackageSetupProgress)progress error:(NSError **)error {
    if (NSThread.isMainThread) { if (error) *error = VMPackageError(@"Guest preparation requires a worker thread."); return NO; }
    if (progress) progress(@"Checking machine", -1);
    // Check eligibility before creating credentials. A rejected stock machine
    // must not later look like one whose package service is merely starting.
    char detail[1200] = {0}; const char *work = directory.fileSystemRepresentation;
    uint8_t manifest[32];
    if (vm_guest_install_probe(work,manifest,detail,sizeof detail) != VM_GUEST_INSTALL_PROBE_VALID) {
        if (error) *error = VMPackageError(@"Jailbreak this machine before setting up Packages."); return NO;
    }
    NSURL *binaryURL = [NSBundle.mainBundle URLForResource:@"s5lbox-package-service" withExtension:nil];
    if (!binaryURL) { if (error) *error=VMPackageError(@"This build is missing its guest package helper."); return NO; }
    NSData *binary = [NSData dataWithContentsOfURL:binaryURL options:0 error:error]; if (!binary.length) return NO;
    NSString *root = @"/private/var/lib/s5lbox-package-manager-v1";
    // Immutable executable generations avoid resizing an existing HFS fork.
    // Upgrade only the exact previous launch job in the staged transaction.
    NSString *program = [root stringByAppendingPathComponent:@"service-hle-v9"];
    NSDictionary *job = @{@"Label":@"com.j0shua.s5lbox.packages", @"ProgramArguments":@[program],
        @"RunAtLoad":@YES, @"KeepAlive":@YES, @"ThrottleInterval":@10};
    NSData *plist = [NSPropertyListSerialization dataWithPropertyList:job format:NSPropertyListXMLFormat_v1_0 options:0 error:error];
    if (!plist) return NO;
    NSMutableArray<NSData *> *legacyPlists=[NSMutableArray array];
    for (NSString *legacy in @[@"service",@"service-finish-v2",@"service-finish-v3",@"service-finish-v4",@"service-finish-v5",@"service-hle-v6",@"service-hle-v7",@"service-hle-v8"]) {
        NSMutableDictionary *legacyJob=[job mutableCopy]; legacyJob[@"ProgramArguments"]=@[[root stringByAppendingPathComponent:legacy]];
        NSData *bytes=[NSPropertyListSerialization dataWithPropertyList:legacyJob format:NSPropertyListXMLFormat_v1_0 options:0 error:error];
        if (!bytes) return NO; [legacyPlists addObject:bytes];
    }
    rootfs_work_file_rewrite_t rewrite={0}; BOOL needsRewrite=NO;
    NSString *live = [directory stringByAppendingPathComponent:@"rootfs-work.img"];
    // Strict read-only identity/count audit, never allocation/backlink repair.
    rootfs_work_result_t check;
    if (rootfs_work_probe_catalog_counters(live.fileSystemRepresentation,true,&check) != ROOTFS_WORK_OK) {
        if (error) *error = VMPackageError([NSString stringWithUTF8String:check.detail]); return NO;
    }
    BOOL needsCounters = check.catalog_directory_counts_repairable || check.catalog_next_id_stale;
    // Keep the credential stable across retries/crashes, but never advertise
    // a half-finished setup as ready. Clear this only after verified publication
    // (or exact verification of a previously completed installation).
    if (!PackageSetupPending(identifier,YES,error)) return NO;
    NSData *token = Capability(identifier,YES,error); if (!token) return NO;
    NSData *filesPlist = [NSData dataWithBytes:VM_GUEST_FILES_PLIST length:sizeof VM_GUEST_FILES_PLIST - 1u];
    NSArray *files = @[@[program,binary,@0755],@[[root stringByAppendingPathComponent:@"capability"],token,@0600],
        @[@VM_GUEST_FILES_PLIST_PATH,filesPlist,@0644],
        @[@"/System/Library/LaunchDaemons/com.j0shua.s5lbox.packages-v1.plist",plist,@0644]];
    rootfs_work_entry_t entries[5] = {0}; size_t count = 1;
    entries[0].kind=ROOTFS_WORK_ENTRY_DIRECTORY; entries[0].path=root.UTF8String;
    entries[0].permissions=0700; entries[0].existing_policy=ROOTFS_WORK_EXISTING_REUSE_DIRECTORY;
    for (NSArray *file in files) {
        NSString *path=file[0]; NSData *data=file[1]; uint16_t mode=[file[2] unsignedShortValue];
        rootfs_work_file_repair_t probe={0}; probe.path=path.UTF8String; probe.expected_size=data.length;
        ios3_sha256(data.bytes,data.length,probe.expected_sha256); probe.expected_permissions=probe.desired_permissions=mode;
        rootfs_work_file_repair_state_t state;
        if (rootfs_work_probe_file_repair_ex(live.fileSystemRepresentation,&probe,true,&state,&check) != ROOTFS_WORK_OK ||
            (state != ROOTFS_WORK_FILE_REPAIR_MISSING && state != ROOTFS_WORK_FILE_REPAIR_SATISFIED)) {
            rootfs_work_file_rewrite_state_t rewriteState;
            if (file==files.lastObject) for (NSData *legacyPlist in legacyPlists) {
                rewrite.path=path.UTF8String; rewrite.expected_content=legacyPlist.bytes; rewrite.expected_content_size=legacyPlist.length;
                rewrite.desired_content=plist.bytes; rewrite.desired_content_size=plist.length; rewrite.permissions=0644;
                if (rootfs_work_probe_file_rewrite_ex(live.fileSystemRepresentation,&rewrite,true,&rewriteState,&check)==ROOTFS_WORK_OK &&
                    rewriteState==ROOTFS_WORK_FILE_REWRITE_NEEDED) { needsRewrite=YES; break; }
            }
            if (needsRewrite) continue;
            if (error) *error = VMPackageError(@"Guest package helper differs from a recognized version. It was not overwritten."); return NO;
        }
        if (state == ROOTFS_WORK_FILE_REPAIR_MISSING) {
            entries[count].kind=ROOTFS_WORK_ENTRY_FILE; entries[count].path=path.UTF8String;
            entries[count].content=data.bytes; entries[count].content_size=data.length; entries[count].permissions=mode; count++;
        }
    }
    if (count == 1 && !needsRewrite && !needsCounters) {
        if (!PackageSetupPending(identifier,NO,error)) return NO;
        AwaitFirstConnection(identifier);
        if (progress) progress(@"Package files verified", 1);
        return YES;
    }
    char snapshots[VM_GUEST_INSTALL_PATH_CAPACITY]; vm_snapshot_info_t items[VM_SNAPSHOT_MAX]; size_t n=0;
    if (vm_snapshot_dir(work,snapshots,sizeof snapshots) != VM_SNAPSHOT_OK ||
        vm_snapshot_list(snapshots,items,VM_SNAPSHOT_MAX,&n,detail,sizeof detail) != VM_SNAPSHOT_OK || n) {
        if (error) *error = VMPackageError(@"Historical snapshots prevent adding the package helper. Resolve them before setup."); return NO;
    }
    vm_guest_install_result_t transaction; char stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    if (vm_guest_app_prepare_stage(work,&transaction,detail,sizeof detail) != VM_GUEST_INSTALL_OK ||
        !vm_guest_app_stage_image_path(stage,sizeof stage,work)) {
        if (error) *error = VMPackageError([NSString stringWithUTF8String:detail]); return NO;
    }
    rootfs_work_options_t options={0}; options.preserve_fstab=true; options.allow_unclean_source=true;
    options.reconcile_next_catalog_id=true;
    options.reconcile_catalog_counts=true;
    options.protect_referenced_allocations=true;
    options.entries=entries; options.entry_count=count;
    __attribute__((objc_precise_lifetime)) VMPackageSetupProgress copyProgress = [progress copy];
    PackageCopyProgress progressState = {copyProgress, 0};
    options.progress=PackageCopy; options.progress_ctx=&progressState;
    if (progress) progress(@"Preparing package files", 0);
    if (needsRewrite) { options.file_rewrites=&rewrite; options.file_rewrite_count=1; }
    if (rootfs_work_create(live.fileSystemRepresentation,stage,&options,&check) != ROOTFS_WORK_OK || !check.published) {
        vm_guest_app_discard_stage(work,NULL,0);
        if (error) *error = VMPackageError([NSString stringWithUTF8String:check.detail]); return NO;
    }
    unsigned char identity[32]; ios3_sha256(binary.bytes,binary.length,identity);
    if (progress) progress(@"Finishing setup", -1);
    if (vm_guest_app_publish(work,identity,&transaction,detail,sizeof detail) != VM_GUEST_INSTALL_OK || !transaction.committed) {
        if (error) *error = VMPackageError([NSString stringWithUTF8String:detail]); return NO;
    }
    if (!PackageSetupPending(identifier,NO,error)) return NO;
    AwaitFirstConnection(identifier);
    if (progress) progress(@"Package files verified", 1);
    return YES;
}
@end
