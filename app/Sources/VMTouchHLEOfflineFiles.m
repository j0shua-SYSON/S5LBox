// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#if S5LBOX_TOUCHHLE
#import "VMTouchHLEOfflineFiles.h"
#import "VMStorageLease.h"
#import "VMInstanceStore.h"
#import "VMGuestInstall.h"
#import "VMResumeCheckpoint.h"
#import "VMGuest.h"
#import "VMIPAArchive.h"
#import "VMIPAPackage.h"
#import "VMSettings.h"
#import "VMOptions.h"
#import "VMInstancePaths.h"
#import "VMBootOptions.h"
#include "../../tools/hfs_offline.h"
#include "../../tools/guest_hle_files.h"
#include "../../tools/sha256.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static NSError *OfflineError(int code,NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.Offline" code:code
        userInfo:@{NSLocalizedDescriptionKey:message ?: @"The app's files could not be accessed."}];
}
static uint32_t Read32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint64_t Read64(const uint8_t *p) { return ((uint64_t)Read32(p)<<32)|Read32(p+4); }
static void Put32(NSMutableData *data,uint32_t n) {
    uint8_t p[4]={(uint8_t)(n>>24),(uint8_t)(n>>16),(uint8_t)(n>>8),(uint8_t)n}; [data appendBytes:p length:4];
}
static void Put64(NSMutableData *data,uint64_t n) { Put32(data,(uint32_t)(n>>32)); Put32(data,(uint32_t)n); }
static NSString *Name(const hfs_offline_info *info) { return [[NSString alloc] initWithCharacters:info->name length:info->name_length]; }
static NSDictionary *DiskIdentity(NSString *path) {
    int fd=open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    struct stat st; uint8_t header[512],digest[32];
    BOOL ok=fd>=0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_nlink==1 &&
        pread(fd,header,sizeof header,1024)==sizeof header;
    if (fd>=0) close(fd);
    if (!ok) return nil;
    if (!ios3_sha256(header,sizeof header,digest)) return nil;
    return @{@"inode":@((uint64_t)st.st_ino),@"device":@((uint64_t)st.st_dev),@"size":@((uint64_t)st.st_size),
        @"seconds":@((int64_t)st.st_mtimespec.tv_sec),@"nanos":@(st.st_mtimespec.tv_nsec),
        @"header":[NSData dataWithBytes:digest length:32]};
}
static BOOL SaveWitness(NSString *directory,NSArray *identities,NSError **error) {
    NSData *data=[NSPropertyListSerialization dataWithPropertyList:@{@"version":@1,@"images":identities}
        format:NSPropertyListBinaryFormat_v1_0 options:0 error:error];
    if (!data) return NO;
    NSString *path=[directory stringByAppendingPathComponent:@"offline-storage.plist"];
    if (![data writeToFile:path options:NSDataWritingAtomic error:error]) return NO;
    int fd=open(path.fileSystemRepresentation,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    BOOL ok=fd>=0 && !fsync(fd); if (fd>=0) close(fd);
    fd=open(directory.fileSystemRepresentation,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    ok=fd>=0 && !fsync(fd) && ok; if (fd>=0) close(fd);
    if (!ok && error) *error=OfflineError(EIO,@"The disk handoff could not be saved.");
    return ok;
}
@interface HLEOfflineHandle : NSObject
@property uint32_t inode,flags,flockKind;
@property uint64_t offset;
@property size_t cursor;
@property BOOL directory;
@end
@implementation HLEOfflineHandle @end

@interface VMTouchHLEOfflineFiles ()
- (int32_t)exchange:(const uint8_t *)request length:(uint32_t)length
    response:(uint8_t *)response capacity:(uint32_t)capacity count:(uint32_t *)count;
@end
static int32_t OfflineExchange(void *context,const uint8_t *request,uint32_t length,uint8_t *response,uint32_t capacity,uint32_t *count) {
    @autoreleasepool {
        VMTouchHLEOfflineFiles *files=(__bridge VMTouchHLEOfflineFiles *)context;
        @try { return [files exchange:request length:length response:response capacity:capacity count:count]; }
        @catch (__unused NSException *exception) { return -1; }
    }
}
@implementation VMTouchHLEOfflineFiles {
    hfs_offline *_volume;
    VMStorageLease *_lease;
    NSString *_directory,*_live,*_home,*_bundle;
    NSDictionary *_identity;
    uint32_t _root,_nextHandle;
    NSMutableDictionary<NSNumber *,HLEOfflineHandle *> *_handles;
    NSMutableDictionary<NSString *,NSArray<NSString *> *> *_icons;
    NSError *_error;
    BOOL _batchActive;
}
- (instancetype)initWithInstanceID:(NSString *)identifier error:(NSError **)error {
    self=[super init]; if (!self) return nil;
    _directory=[[VMInstanceStore sharedStore] directoryForInstanceWithID:identifier];
    _live=[_directory stringByAppendingPathComponent:@"rootfs-work.img"];
    _lease=[VMStorageLease acquireDirectory:_directory error:error]; if (!_lease) return nil;
    char detail[320]={0};
    if (vm_guest_maintenance_recover(_directory.fileSystemRepresentation,NULL,NULL,NULL,detail,sizeof detail)!=VM_GUEST_INSTALL_OK) {
        if (error) *error=OfflineError(EIO,[NSString stringWithUTF8String:detail]); return nil;
    }
    NSDictionary *identity=DiskIdentity(_live);
    BOOL created=NO;
    if (!identity && ![NSFileManager.defaultManager fileExistsAtPath:_live]) {
        VMInstanceStore *store=VMInstanceStore.sharedStore; VMSettings *settings=VMSettings.sharedSettings;
        vm_instance_paths_t paths; vm_firmware_boot_paths_t bootPaths;
        BOOL mbx=NO,software=NO;
        BOOL graphics=[store graphicsForOpeningInstanceWithID:identifier mbxEnabled:&mbx softwareRendererEnabled:&software error:error];
        if (error && *error) return nil;
        if (vm_instance_paths_derive(settings.firmwareDirectory.fileSystemRepresentation,store.machinesDirectory.fileSystemRepresentation,
            identifier.UTF8String,&paths)!=VM_INSTANCE_PATHS_OK || vm_instance_work_plan(&paths)!=VM_INSTANCE_WORK_PROVISION) {
            if (error) *error=OfflineError(EINVAL,@"This machine's existing storage must be opened and shut down before using Apps."); return nil;
        }
        vm_instance_paths_to_boot(&paths,&bootPaths);
        bool values[VM_BOOT_OPTION_MAX]={0}; unsigned count=vm_option_count();
        if (count>VM_BOOT_OPTION_MAX) { if (error) *error=OfflineError(EINVAL,@"The machine settings could not be loaded."); return nil; }
        for (unsigned i=0;i<count;i++) values[i]=[settings valueForNewMachineOptionIndex:i];
        int mbxIndex=vm_option_index("mbx"),softwareIndex=vm_option_index("ca-software-render");
        if (graphics && mbxIndex>=0 && softwareIndex>=0) { values[mbxIndex]=mbx; values[softwareIndex]=software; }
        if (!vm_firmware_boot_provision(&bootPaths,values,count,NULL,NULL,detail,sizeof detail)) {
            if (error) *error=OfflineError(EIO,[NSString stringWithUTF8String:detail]); return nil;
        }
        identity=DiskIdentity(_live); created=identity!=nil;
    }
    if (!identity) { if (error) *error=OfflineError(ENOENT,@"Set up this machine before opening its apps."); return nil; }
    _identity=identity;
    NSDictionary *witness=[NSDictionary dictionaryWithContentsOfFile:[_directory stringByAppendingPathComponent:@"offline-storage.plist"]];
    NSArray *images=[witness[@"images"] isKindOfClass:NSArray.class]?witness[@"images"]:@[];
    BOOL handedOff=[witness[@"version"] isEqual:@1] && [images containsObject:identity];
    if (!handedOff && !created) {
        vm_resume_checkpoint_state_t state=vm_resume_checkpoint_probe_state(_directory.fileSystemRepresentation,
            [identity[@"size"] unsignedLongLongValue],VM_GUEST_RAM_BASE,VM_GUEST_RAM_SIZE,detail,sizeof detail);
        if (state!=VM_RESUME_CHECKPOINT_POWERED_OFF) {
            if (error) *error=OfflineError(EBUSY,@"Shut down iPhone OS once to make these apps available offline. A saved machine must finish shutting down before its disk can be shared.");
            return nil;
        }
    }
    int e=hfs_offline_open(_live.fileSystemRepresentation,&_volume);
    if (e) { if (error) *error=OfflineError(e,@"This guest disk cannot be opened offline. Its original files have not been changed."); return nil; }
    if (!handedOff && !SaveWitness(_directory,@[identity],error)) return nil;
    _root=2; _nextHandle=1; _handles=[NSMutableDictionary dictionary]; _icons=[NSMutableDictionary dictionary]; return self;
}
- (NSError *)error { return _error; }
- (int)lookup:(NSString *)name parent:(uint32_t)parent result:(hfs_offline_info *)result {
    // HFSX stores decomposed UTF-16 names. Existing catalog bytes remain intact.
    name=name.decomposedStringWithCanonicalMapping;
    if (!name.length || name.length>255) return ENAMETOOLONG;
    unichar units[255]; [name getCharacters:units range:NSMakeRange(0,name.length)];
    return hfs_offline_lookup(_volume,parent,units,name.length,result);
}
- (int)resolve:(NSString *)path root:(uint32_t)root followFinal:(BOOL)follow result:(hfs_offline_info *)result {
    if (![path hasPrefix:@"/"] || [path lengthOfBytesUsingEncoding:NSUTF8StringEncoding]>=1024 ||
        [path rangeOfString:[NSString stringWithFormat:@"%C",(unichar)0]].location!=NSNotFound) return EINVAL;
    NSMutableArray<NSString *> *todo=[[path componentsSeparatedByString:@"/"] mutableCopy];
    NSMutableArray<NSNumber *> *stack=[NSMutableArray arrayWithObject:@(root)];
    NSUInteger links=0;
    while (todo.count) {
        NSString *part=todo.firstObject; [todo removeObjectAtIndex:0];
        if (!part.length || [part isEqual:@"."]) continue;
        if ([part isEqual:@".."]) { if (stack.count>1) [stack removeLastObject]; continue; }
        int e=[self lookup:part parent:stack.lastObject.unsignedIntValue result:result]; if (e) return e;
        if ((result->mode&0170000)==0120000 && (follow || todo.count)) {
            if (++links>40 || result->size>=1024) return ELOOP;
            NSMutableData *bytes=[NSMutableData dataWithLength:(NSUInteger)result->size]; size_t count=0;
            e=hfs_offline_read(_volume,result->id,0,bytes.mutableBytes,bytes.length,&count); if (e) return e;
            NSString *target=[[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding];
            if (!target.length || count!=bytes.length) return EIO;
            if ([target hasPrefix:@"/"]) { [stack removeAllObjects]; [stack addObject:@(root)]; }
            NSIndexSet *indexes=[NSIndexSet indexSetWithIndexesInRange:NSMakeRange(0,[target componentsSeparatedByString:@"/"].count)];
            [todo insertObjects:[target componentsSeparatedByString:@"/"] atIndexes:indexes];
        } else {
            if (todo.count && (result->mode&0170000)!=0040000) return ENOTDIR;
            [stack addObject:@(result->id)];
        }
    }
    return hfs_offline_stat(_volume,stack.lastObject.unsignedIntValue,result);
}
- (NSArray<VMGuestApplication *> *)installedApplications:(NSError **)error {
    hfs_offline_info applications; int e=[self resolve:@"/private/var/mobile/Applications" root:2 followFinal:YES result:&applications];
    if (e==ENOENT) return @[];
    if (e) { if (error) *error=OfflineError(e,@"Installed apps could not be read from the guest disk."); return nil; }
    NSMutableArray *apps=[NSMutableArray array]; size_t cursor=0; hfs_offline_info container;
    while (!hfs_offline_next(_volume,applications.id,&cursor,&container)) {
        NSString *uuid=Name(&container);
        if ((container.mode&0170000)!=0040000 || uuid.length!=36 || ![[NSUUID alloc] initWithUUIDString:uuid]) continue;
        size_t childCursor=0; hfs_offline_info child;
        while (!hfs_offline_next(_volume,container.id,&childCursor,&child)) {
            NSString *name=Name(&child); if ((child.mode&0170000)!=0040000 || ![name hasSuffix:@".app"]) continue;
            hfs_offline_info plist;
            if ([self lookup:@"Info.plist" parent:child.id result:&plist] || (plist.mode&0170000)!=0100000 || plist.size>4*1024*1024) continue;
            NSMutableData *data=[NSMutableData dataWithLength:(NSUInteger)plist.size]; size_t count=0;
            if (hfs_offline_read(_volume,plist.id,0,data.mutableBytes,data.length,&count) || count!=data.length) continue;
            id decoded=[NSPropertyListSerialization propertyListWithData:data options:0 format:NULL error:NULL];
            if (![decoded isKindOfClass:NSDictionary.class]) continue;
            NSMutableDictionary *record=[decoded mutableCopy];
            record[@"Path"]=[NSString stringWithFormat:@"/var/mobile/Applications/%@/%@",uuid,name];
            VMGuestApplication *app=[VMGuestApplication fromInstallationRecord:record];
            if (app) {
                [apps addObject:app]; NSMutableArray *icons=[NSMutableArray array];
                if ([record[@"CFBundleIconFiles"] isKindOfClass:NSArray.class]) [icons addObjectsFromArray:record[@"CFBundleIconFiles"]];
                if ([record[@"CFBundleIconFile"] isKindOfClass:NSString.class]) [icons addObject:record[@"CFBundleIconFile"]];
                [icons addObjectsFromArray:@[@"Icon.png",@"icon.png"]]; _icons[app.identifier]=icons;
            }
        }
    }
    [apps sortUsingComparator:^NSComparisonResult(VMGuestApplication *a,VMGuestApplication *b) { return [a.name localizedStandardCompare:b.name]; }];
    return apps;
}
- (NSData *)iconForApplication:(VMGuestApplication *)app {
    NSString *bundle=[[@"/private" stringByAppendingString:app.homeDirectory] stringByAppendingPathComponent:app.bundleDirectory];
    for (id candidate in _icons[app.identifier]) {
        if (![candidate isKindOfClass:NSString.class] || ![candidate length] || [candidate length]>255 ||
            ![[candidate lastPathComponent] isEqual:candidate]) continue;
        NSString *name=[candidate pathExtension].length?candidate:[candidate stringByAppendingString:@".png"];
        hfs_offline_info info; if ([self resolve:[bundle stringByAppendingPathComponent:name] root:2 followFinal:YES result:&info] ||
            (info.mode&0170000)!=0100000 || !info.size || info.size>1024*1024) continue;
        NSMutableData *data=[NSMutableData dataWithLength:(NSUInteger)info.size]; size_t count=0;
        if (hfs_offline_read(_volume,info.id,0,data.mutableBytes,data.length,&count) || count!=data.length) continue;
        uint32_t width=0,height=0;
        if (vm_ipa_icon_png_dimensions(data.bytes,data.length,&width,&height) && width<=512 && height<=512) return data;
    }
    return nil;
}
- (BOOL)selectApplication:(VMGuestApplication *)app error:(NSError **)error {
    hfs_offline_info container;
    NSString *path=[@"/private" stringByAppendingString:app.homeDirectory];
    int e=[self resolve:path root:2 followFinal:YES result:&container];
    if (e || (container.mode&0170000)!=0040000) { if (error) *error=OfflineError(e?:ENOTDIR,@"This app is no longer installed."); return NO; }
    _root=container.id; _home=app.homeDirectory; _bundle=app.bundleDirectory; return YES;
}
- (s5lbox_hle_live_config)configuration {
    s5lbox_hle_live_config config={0}; config.abi_version=1; config.context=(__bridge void *)self;
    config.exchange=OfflineExchange; config.home_directory=_home.UTF8String; config.bundle_directory=_bundle.UTF8String; return config;
}
- (BOOL)flush {
    if (_error) return NO;
    if (!_volume || !hfs_offline_dirty(_volume)) return YES;
    char detail[320]={0},stage[VM_GUEST_INSTALL_PATH_CAPACITY]; vm_guest_install_result_t transaction={0};
    const char *work=_directory.fileSystemRepresentation;
    NSDictionary *original=DiskIdentity(_live);
    if (![original isEqual:_identity]) {
        _error=OfflineError(ESTALE,@"The guest disk changed outside this session. Close the app and reopen the machine."); return NO;
    }
    if (!original || vm_guest_recovery_prepare_stage(work,&transaction,detail,sizeof detail)!=VM_GUEST_INSTALL_OK ||
        !vm_guest_recovery_stage_image_path(stage,sizeof stage,work) ||
        vm_guest_recovery_clone_live_to_stage(work,detail,sizeof detail)!=VM_GUEST_INSTALL_OK) {
        _error=OfflineError(EIO,@"There is not enough available storage to save this app safely."); return NO;
    }
    int e=hfs_offline_commit(_volume,stage);
    if (e) {
        vm_guest_recovery_discard_stage(work,detail,sizeof detail);
        _error=OfflineError(e,e==ENOSPC?@"The guest disk is full. The previous saved data is intact.":@"The app's changes could not be saved. The previous saved data is intact."); return NO;
    }
    NSDictionary *candidate=DiskIdentity([NSString stringWithUTF8String:stage]);
    // Both identities survive a crash on either side of atomic publication.
    // A guest run revokes this witness before opening any writable disk.
    if (!candidate || ![DiskIdentity(_live) isEqual:original]) {
        _error=OfflineError(ESTALE,@"The guest disk changed while saving. The replacement was not published."); return NO;
    }
    NSError *witnessError=nil;
    if (!SaveWitness(_directory,@[original,candidate],&witnessError)) { _error=witnessError; return NO; }
    if (vm_guest_recovery_publish(work,&transaction,detail,sizeof detail)!=VM_GUEST_INSTALL_OK || !transaction.committed) {
        _error=OfflineError(EIO,[NSString stringWithUTF8String:detail]); return NO;
    }
    _identity=candidate;
    return YES;
}
- (void)close {
    if (!_batchActive) [self flush];
    hfs_offline_close(_volume); _volume=NULL; [_handles removeAllObjects];
    if (_batchActive) { char detail[320]; vm_guest_recovery_discard_stage(_directory.fileSystemRepresentation,detail,sizeof detail); _batchActive=NO; }
    [_lease close]; _lease=nil;
}
- (void)dealloc { [self close]; }

- (int)ensureDirectory:(NSString *)path result:(hfs_offline_info *)result {
    uint32_t parent=2;
    for (NSString *component in [path componentsSeparatedByString:@"/"]) {
        if (!component.length) continue;
        if ([component isEqual:@"."] || [component isEqual:@".."] || component.length>255) return EINVAL;
        int e=[self lookup:component parent:parent result:result];
        if (e==ENOENT) {
            NSString *name=component.decomposedStringWithCanonicalMapping;
            if (name.length>255) return ENAMETOOLONG;
            unichar units[255]; [name getCharacters:units range:NSMakeRange(0,name.length)]; uint32_t id=0;
            e=hfs_offline_create(_volume,parent,units,name.length,0040755,&id);
            if (!e) e=hfs_offline_stat(_volume,id,result);
        }
        if (e) return e;
        if ((result->mode&0170000)!=0040000) return ENOTDIR;
        parent=result->id;
    }
    return hfs_offline_stat(_volume,parent,result);
}
- (int)writeFile:(NSString *)path data:(NSData *)data mode:(uint32_t)mode {
    hfs_offline_info parent,entry;
    int e=[self ensureDirectory:path.stringByDeletingLastPathComponent result:&parent]; if (e) return e;
    NSString *name=path.lastPathComponent.decomposedStringWithCanonicalMapping;
    if (!name.length || name.length>255) return ENAMETOOLONG;
    e=[self lookup:name parent:parent.id result:&entry];
    if (e==ENOENT) {
        unichar units[255]; [name getCharacters:units range:NSMakeRange(0,name.length)]; uint32_t id=0;
        e=hfs_offline_create(_volume,parent.id,units,name.length,mode,&id);
        if (!e) e=hfs_offline_stat(_volume,id,&entry);
    }
    if (e) return e;
    if ((entry.mode&0170000)!=(mode&0170000)) return EINVAL;
    e=hfs_offline_truncate(_volume,entry.id,0); if (e) return e;
    return hfs_offline_write(_volume,entry.id,0,data.bytes,data.length);
}
- (NSData *)readFile:(NSString *)path error:(int *)error {
    hfs_offline_info info;
    *error=[self resolve:path root:2 followFinal:YES result:&info]; if (*error) return nil;
    if ((info.mode&0170000)!=0100000 || info.size>8*1024*1024) { *error=EFBIG; return nil; }
    NSMutableData *data=[NSMutableData dataWithLength:(NSUInteger)info.size]; size_t count=0;
    *error=hfs_offline_read(_volume,info.id,0,data.mutableBytes,data.length,&count);
    if (!*error && count!=data.length) *error=EIO;
    return *error?nil:data;
}
- (int)removeTree:(uint32_t)inode depth:(NSUInteger)depth {
    if (depth>64) return ELOOP;
    hfs_offline_info info; int e=hfs_offline_stat(_volume,inode,&info); if (e) return e;
    if ((info.mode&0170000)==0040000) {
        size_t cursor=0;
        while (!hfs_offline_next(_volume,inode,&cursor,&info)) {
            e=[self removeTree:info.id depth:depth+1]; if (e) return e;
        }
    }
    return hfs_offline_remove_closed(_volume,inode);
}
- (NSMutableDictionary *)initialInstallationRegistry:(int *)error {
    hfs_offline_info applications;
    *error=[self resolve:@"/Applications" root:2 followFinal:YES result:&applications]; if (*error) return nil;
    NSMutableDictionary *system=[NSMutableDictionary dictionary]; size_t cursor=0; hfs_offline_info entry;
    while (!hfs_offline_next(_volume,applications.id,&cursor,&entry)) {
        NSString *name=Name(&entry);
        if ((entry.mode&0170000)!=0040000 || ![name hasSuffix:@".app"]) continue;
        NSString *path=[@"/Applications" stringByAppendingPathComponent:name];
        int readError=0; NSData *data=[self readFile:[path stringByAppendingPathComponent:@"Info.plist"] error:&readError];
        if (!data) { *error=readError; return nil; }
        id decoded=[NSPropertyListSerialization propertyListWithData:data options:NSPropertyListMutableContainers format:NULL error:NULL];
        if (![decoded isKindOfClass:NSMutableDictionary.class] || ![decoded[@"CFBundleIdentifier"] isKindOfClass:NSString.class]) { *error=EINVAL; return nil; }
        decoded[@"Path"]=path; decoded[@"ApplicationType"]=@"System"; system[decoded[@"CFBundleIdentifier"]]=decoded;
    }
    int versionError=0; NSData *version=[self readFile:@"/System/Library/CoreServices/SystemVersion.plist" error:&versionError];
    id metadata=version?[NSPropertyListSerialization propertyListWithData:version options:0 format:NULL error:NULL]:nil;
    if (![metadata isKindOfClass:NSDictionary.class]) { *error=versionError?:EINVAL; return nil; }
    return [@{@"System":system,@"User":[NSMutableDictionary dictionary],@"Metadata":metadata} mutableCopy];
}
- (BOOL)installPackage:(VMIPAPackage *)package error:(NSError **)error {
    if (!_volume || _home || _batchActive || _error || !package) {
        if (error) *error=OfflineError(EBUSY,@"Close the running app before installing."); return NO;
    }
    VMGuestApplication *existing=nil;
    for (VMGuestApplication *app in [self installedApplications:error]) if ([app.identifier isEqual:package.bundleIdentifier]) existing=app;
    if (error && *error) return NO;
    NSString *home=existing?[@"/private" stringByAppendingString:existing.homeDirectory]:
        [@"/private/var/mobile/Applications" stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSString *bundle=[home stringByAppendingPathComponent:package.bundleDirectoryName];
    const char *work=_directory.fileSystemRepresentation; char detail[320]={0},stage[VM_GUEST_INSTALL_PATH_CAPACITY];
    vm_guest_install_result_t transaction={0}; int failure=0;
    if (![DiskIdentity(_live) isEqual:_identity] ||
        vm_guest_recovery_prepare_stage(work,&transaction,detail,sizeof detail)!=VM_GUEST_INSTALL_OK ||
        !vm_guest_recovery_stage_image_path(stage,sizeof stage,work) ||
        vm_guest_recovery_clone_live_to_stage(work,detail,sizeof detail)!=VM_GUEST_INSTALL_OK) {
        if (error) *error=OfflineError(EIO,@"The disk could not be prepared for installation."); return NO;
    }
    _batchActive=YES;
    hfs_offline_close(_volume); _volume=NULL;
    failure=hfs_offline_open_private(stage,&_volume);
    if (failure) goto failed;
    if (existing) {
        hfs_offline_info old;
        failure=[self resolve:[home stringByAppendingPathComponent:existing.bundleDirectory] root:2 followFinal:NO result:&old];
        if (!failure) failure=[self removeTree:old.id depth:0];
        if (!failure) failure=hfs_offline_checkpoint_private(_volume);
        if (failure) goto failed;
    }
    {
        hfs_offline_info directory;
        for (NSString *path in @[bundle,[home stringByAppendingPathComponent:@"Documents"],
            [home stringByAppendingPathComponent:@"Library/Preferences"],[home stringByAppendingPathComponent:@"tmp"]]) {
            failure=[self ensureDirectory:path result:&directory]; if (failure) goto failed;
        }
        __block int writeError=0; __block NSUInteger pending=0;
        BOOL copied=[package visitBundleFiles:^BOOL(NSString *relative,NSData *data,uint32_t mode) {
            NSString *path=[bundle stringByAppendingPathComponent:relative];
            if (data.length>64u*1024u*1024u) { writeError=EFBIG; return NO; }
            if (pending+data.length>32u*1024u*1024u) {
                writeError=hfs_offline_checkpoint_private(self->_volume); pending=0; if (writeError) return NO;
            }
            if ((mode&0170000)==0040000) { hfs_offline_info directoryInfo; writeError=[self ensureDirectory:path result:&directoryInfo]; }
            else { writeError=[self writeFile:path data:data mode:mode]; pending+=data.length; }
            return !writeError;
        }];
        if (!copied) { failure=writeError?:EIO; goto failed; }
        failure=hfs_offline_checkpoint_private(_volume); if (failure) goto failed;
    }
    {
        NSString *registryPath=@"/private/var/mobile/Library/Caches/com.apple.mobile.installation.plist";
        NSData *data=[self readFile:registryPath error:&failure];
        id decoded=nil;
        if (failure==ENOENT) { failure=0; decoded=[self initialInstallationRegistry:&failure]; }
        else if (!failure) decoded=[NSPropertyListSerialization propertyListWithData:data options:NSPropertyListMutableContainers format:NULL error:NULL];
        if (failure) goto failed;
        if (![decoded isKindOfClass:NSMutableDictionary.class] || ![decoded[@"User"] isKindOfClass:NSMutableDictionary.class]) { failure=EINVAL; goto failed; }
        NSMutableDictionary *record=[package.infoDictionary mutableCopy];
        record[@"Path"]=bundle; record[@"Container"]=home; record[@"ApplicationType"]=@"User";
        record[@"EnvironmentVariables"]=@{@"HOME":home,@"CFFIXED_USER_HOME":home,@"TMPDIR":[home stringByAppendingPathComponent:@"tmp"]};
        decoded[@"User"][package.bundleIdentifier]=record;
        data=[NSPropertyListSerialization dataWithPropertyList:decoded format:NSPropertyListBinaryFormat_v1_0 options:0 error:NULL];
        if (!data) { failure=EINVAL; goto failed; }
        failure=[self writeFile:registryPath data:data mode:0100644];
        if (!failure) failure=hfs_offline_checkpoint_private(_volume);
        if (failure) goto failed;
    }
    {
        NSDictionary *candidate=DiskIdentity([NSString stringWithUTF8String:stage]); NSError *saveError=nil;
        if (!candidate || ![DiskIdentity(_live) isEqual:_identity] || !SaveWitness(_directory,@[_identity,candidate],&saveError)) {
            failure=EIO; goto failed;
        }
        if (vm_guest_recovery_publish(work,&transaction,detail,sizeof detail)!=VM_GUEST_INSTALL_OK || !transaction.committed) {
            failure=EIO; goto failed;
        }
        _identity=candidate; _batchActive=NO; return YES;
    }
failed:
    _error=OfflineError(failure,failure==EFBIG?@"An app file exceeds the offline installer's current 64 MB limit.":
        failure==ENOSPC?@"There is not enough free space to install this app safely.":@"Installation could not finish. The previous installation and saved data have been kept.");
    if (error) *error=_error;
    [self close]; return NO;
}

- (NSString *)pathAt:(const uint8_t **)p end:(const uint8_t *)end {
    if (end-*p<4) return nil;
    uint32_t n=Read32(*p); *p+=4;
    if (!n || n>=1024 || n>(uint32_t)(end-*p) || memchr(*p,0,n)) return nil;
    NSString *path=[[NSString alloc] initWithBytes:*p length:n encoding:NSUTF8StringEncoding]; *p+=n;
    return [path hasPrefix:@"/"]?path:nil;
}
- (int)parent:(NSString *)path info:(hfs_offline_info *)info name:(NSString **)name {
    *name=path.lastPathComponent.decomposedStringWithCanonicalMapping;
    if (!(*name).length || [*name isEqual:@"."] || [*name isEqual:@".."] || [*name isEqual:@"/"] || (*name).length>255) return EINVAL;
    return [self resolve:path.stringByDeletingLastPathComponent root:_root followFinal:YES result:info];
}
- (int32_t)exchange:(const uint8_t *)request length:(uint32_t)length response:(uint8_t *)response capacity:(uint32_t)capacity count:(uint32_t *)count {
    if (count) *count=0;
    if (!_volume || _error || !_home || !request || !response || !count || length<5 || length>65540 ||
        capacity<8 || Read32(request)!=length-4) return -1;
    const uint8_t *p=request+5,*end=request+length; uint8_t op=request[4]; int e=0;
    NSMutableData *payload=[NSMutableData data];
    hfs_offline_info info={0}; NSString *path=nil;
    HLEOfflineHandle *handle=nil; uint32_t number=0;
    if (op==1 || op==9 || op==10 || op==11 || op==15 || op==18) {
        path=[self pathAt:&p end:end]; if (!path) e=EINVAL;
    } else if (op>=3 && op<=17 && op!=9 && op!=10 && op!=11 && op!=14 && op!=15) {
        if (end-p<4) e=EINVAL;
        else { number=Read32(p); p+=4; handle=_handles[@(number)]; if (!handle) e=EBADF; }
    }
    if (!e) switch (op) {
        case 1:
            e=[self resolve:path root:_root followFinal:YES result:&info];
            if (!e) { Put64(payload,info.size); Put64(payload,(uint64_t)info.modified); Put32(payload,info.mode); Put32(payload,info.uid); Put32(payload,info.gid); Put64(payload,info.id); }
            break;
        case 2: case 14: {
            if (end-p<4) { e=EINVAL; break; }
            uint32_t flags=Read32(p); p+=4; path=[self pathAt:&p end:end];
            if (!path || (flags&~63u) || (op==14?flags!=0:!(flags&3)) || ((flags&52) && !(flags&2))) { e=EINVAL; break; }
            if (_handles.count>=S5LBOX_HLE_FILE_HANDLE_LIMIT || _nextHandle==UINT32_MAX) { e=EMFILE; break; }
            e=[self resolve:path root:_root followFinal:YES result:&info];
            if (!e && (flags&40)==40) { e=EEXIST; break; }
            if (e==ENOENT && (flags&8)) {
                NSString *name=nil; e=[self parent:path info:&info name:&name];
                if (!e) { unichar units[255]; [name getCharacters:units range:NSMakeRange(0,name.length)];
                    uint32_t id=0; e=hfs_offline_create(_volume,info.id,units,name.length,0100644,&id);
                    if (!e) e=hfs_offline_stat(_volume,id,&info); }
            }
            if (e) break;
            BOOL directory=(info.mode&0170000)==0040000;
            if ((op==14)!=directory) { e=directory?EISDIR:ENOTDIR; break; }
            if (!directory && (info.mode&0170000)!=0100000) { e=EACCES; break; }
            if (flags&16) { e=hfs_offline_truncate(_volume,info.id,0); if (e) break; }
            handle=[HLEOfflineHandle new]; handle.inode=info.id; handle.flags=flags; handle.directory=directory;
            number=_nextHandle++; _handles[@(number)]=handle; Put32(payload,number); break;
        }
        case 3: [_handles removeObjectForKey:@(number)]; if (![self flush]) e=EIO; break;
        case 4: {
            if (end-p!=4 || handle.directory || !(handle.flags&1)) { e=handle.directory?EISDIR:EBADF; break; }
            uint32_t requested=Read32(p); p+=4;
            if (requested>65536) { e=EINVAL; break; }
            payload.length=requested; size_t got=0;
            e=hfs_offline_read(_volume,handle.inode,handle.offset,payload.mutableBytes,requested,&got);
            payload.length=got; if (!e) handle.offset+=got; break;
        }
        case 5: {
            if (handle.directory || !(handle.flags&2)) { e=EBADF; break; }
            if (handle.flags&4) { e=hfs_offline_stat(_volume,handle.inode,&info); if (e) break; handle.offset=info.size; }
            size_t n=(size_t)(end-p); e=hfs_offline_write(_volume,handle.inode,handle.offset,p,n); p=end;
            if (!e) { handle.offset+=n; Put32(payload,(uint32_t)n); } break;
        }
        case 6: {
            if (end-p!=12 || handle.directory) { e=EINVAL; break; }
            uint32_t whence=Read32(p); int64_t delta=(int64_t)Read64(p+4); p=end;
            e=hfs_offline_stat(_volume,handle.inode,&info); if (e) break;
            uint64_t base=whence==0?0:whence==1?handle.offset:info.size;
            if (whence>2 || base>INT64_MAX || (delta<0?(uint64_t)(-(delta+1))+1>base:(uint64_t)delta>INT64_MAX-base)) { e=EINVAL; break; }
            handle.offset=(uint64_t)((int64_t)base+delta); Put64(payload,handle.offset); break;
        }
        case 7: if (![self flush]) e=EIO; break;
        case 8:
            if (end-p!=8 || !(handle.flags&2)) e=EBADF;
            else { e=hfs_offline_truncate(_volume,handle.inode,Read64(p)); p=end; } break;
        case 9: {
            NSString *name=nil; e=[self parent:path info:&info name:&name];
            if (!e) { unichar units[255]; [name getCharacters:units range:NSMakeRange(0,name.length)]; uint32_t id=0;
                e=hfs_offline_create(_volume,info.id,units,name.length,0040755,&id); } break;
        }
        case 18: {
            if (end-p!=4) { e=EINVAL; break; }
            uint32_t mode=Read32(p); p=end;
            if (mode&~0777u) { e=EINVAL; break; }
            e=[self resolve:path root:_root followFinal:YES result:&info];
            if (!e && info.uid!=501) e=EPERM;
            if (!e) e=hfs_offline_chmod(_volume,info.id,mode);
            if (!e && ![self flush]) e=EIO; break;
        }
        case 10: case 15:
            e=[self resolve:path root:_root followFinal:NO result:&info];
            if (!e && info.id==_root) e=EACCES;
            if (!e && ((info.mode&0170000)==0040000)!=(op==15)) e=op==15?ENOTDIR:EISDIR;
            if (!e) e=hfs_offline_remove(_volume,info.id);
            if (!e && ![self flush]) e=EIO; break;
        case 11: {
            NSString *destination=[self pathAt:&p end:end],*name=nil;
            if (!destination) { e=EINVAL; break; }
            e=[self resolve:path root:_root followFinal:NO result:&info]; if (e) break;
            if (info.id==_root) { e=EACCES; break; }
            uint32_t id=info.id; e=[self parent:destination info:&info name:&name];
            if (!e) { unichar units[255]; [name getCharacters:units range:NSMakeRange(0,name.length)];
                e=hfs_offline_rename(_volume,id,info.id,units,name.length); }
            if (!e && ![self flush]) e=EIO; break;
        }
        case 12: {
            if (!handle.directory) { e=ENOTDIR; break; }
            uint32_t n=0; Put32(payload,0); size_t cursor=handle.cursor;
            while (n<128 && !hfs_offline_next(_volume,handle.inode,&cursor,&info)) {
                NSData *name=[Name(&info) dataUsingEncoding:NSUTF8StringEncoding];
                if (!name || payload.length+4+name.length>65536) break;
                Put32(payload,(uint32_t)name.length); [payload appendData:name]; handle.cursor=cursor; n++;
            }
            uint8_t *bytes=payload.mutableBytes; bytes[0]=(uint8_t)(n>>24); bytes[1]=(uint8_t)(n>>16); bytes[2]=(uint8_t)(n>>8); bytes[3]=(uint8_t)n; break;
        }
        case 13: case 16: {
            if (end-p!=20) { e=EINVAL; break; }
            uint32_t kind=Read32(p); uint64_t start=Read64(p+4),len=Read64(p+12); p=end;
            if (kind<1 || kind>3 || start>INT64_MAX || len>INT64_MAX-start) { e=EINVAL; break; }
            // fcntl locks are process-owned. This volume has exactly one
            // interpreted process and its lease excludes every other writer.
            if (op==16) { Put32(payload,3); Put64(payload,start); Put64(payload,len); Put32(payload,0); } break;
        }
        case 17: {
            if (end-p!=4) { e=EINVAL; break; } uint32_t kind=Read32(p); p=end;
            if (kind<1 || kind>3) { e=EINVAL; break; }
            if (kind!=3) for (HLEOfflineHandle *other in _handles.allValues)
                if (other!=handle && other.inode==handle.inode && other.flockKind && other.flockKind!=3 && (kind==2 || other.flockKind==2)) { e=EWOULDBLOCK; break; }
            if (!e) handle.flockKind=kind; break;
        }
        default: e=ENOSYS; break;
    }
    if (p!=end && !e) e=EINVAL;
    if (e) payload.length=0;
    if (payload.length+8>capacity) return -1;
    NSMutableData *reply=[NSMutableData data]; Put32(reply,(uint32_t)payload.length+4); Put32(reply,(uint32_t)e); [reply appendData:payload];
    memcpy(response,reply.bytes,reply.length); *count=(uint32_t)reply.length; return 0;
}
@end
#endif
