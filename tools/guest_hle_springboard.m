/* Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * Guest-only iPhone OS 3.1.3 launch guard. It does not hook the host iOS.
 * The selectors and encodings below were checked against the user's 7E18
 * SpringBoard; no Apple binary or implementation is included here.
 * One main run loop serializes launch hooks and the quiescence response. */
#include "guest_hle_launch_guard.h"
#include "guest_hle_cfmessageport.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <mach/mach_time.h>

typedef signed char BOOL;
typedef struct objc_method *Method;
typedef id (*IMP)(id,SEL,...);
extern Class objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern Method class_getInstanceMethod(Class,SEL);
extern Method class_getClassMethod(Class,SEL);
extern const char *method_getTypeEncoding(Method);
extern IMP method_setImplementation(Method,IMP);
/* The Darwin runtime takes an erased implementation address, not a call
 * through IMP's generic prototype. Match the void-pointer conversion used
 * for saved implementations below; signature() validates every method ABI. */
#define HLE_IMP(function) ((IMP)(void *)(function))
__attribute__((objc_root_class)) @interface NSObject { Class isa; } @end
@interface NSString : NSObject
- (const char *)UTF8String;
@end
@interface NSArray : NSObject
- (unsigned)count;
- (id)objectAtIndex:(unsigned)index;
@end
@interface SBApplication : NSObject
- (NSString *)path;
- (NSString *)launchdJobLabel;
- (int)pid;
- (NSString *)bundleIdentifier;
@end
@interface SBApplicationIcon : NSObject
- (SBApplication *)application;
@end
@interface SBUIController : NSObject
+ (id)sharedInstance;
- (void)activateApplicationAnimated:(SBApplication *)app;
@end
@interface SpringBoard : NSObject
+ (id)sharedApplication;
- (void)resetIdleTimer;
- (BOOL)isLocked;
@end
@interface SBApplicationController : NSObject
+ (id)sharedInstance;
- (NSArray *)allApplications;
@end
@interface SBLaunchdUtilities : NSObject
+ (NSArray *)allJobLabels;
@end

static BOOL (*original_launch)(id,SEL);
static void (*original_exit)(id,SEL),(*original_failed)(id,SEL),(*original_dealloc)(id,SEL);
static id pending[128];
static BOOL overflow;
static s5lbox_hle_routes routes;
static void (*original_icon_launch)(id,SEL);
static void (*original_idle)(id,SEL),(*original_auto_lock)(id,SEL);
static BOOL has_lock_state;
static double uptime(void) {
    mach_timebase_info_data_t scale; mach_timebase_info(&scale);
    return (double)mach_absolute_time()*scale.numer/scale.denom/1e9;
}
static const char *container_path(id application) {
    const char *path=[[(SBApplication *)application path] UTF8String];
    if (!path) return NULL;
    if (!strncmp(path,"/private/",9)) path+=8;
    const char prefix[]="/var/mobile/Applications/";
    return !strncmp(path,prefix,sizeof prefix-1)?path+sizeof prefix-1:NULL;
}
static void forget(id app) { for (unsigned i=0;i<128;i++) if (pending[i]==app) pending[i]=NULL; }
static void remember(id app) {
    for (unsigned i=0;i<128;i++) if (pending[i]==app) return;
    for (unsigned i=0;i<128;i++) if (!pending[i]) { pending[i]=app; return; }
    overflow=1; /* Refuse HLE, not native apps, until SpringBoard restarts. */
}
static BOOL launch(id app,SEL cmd) {
    if (!container_path(app)) return original_launch(app,cmd);
    int fd=open(S5LBOX_HLE_GUARD_PATH,O_RDONLY|O_NOFOLLOW);
    if (fd>=0) {
        struct stat st;
        if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid || st.st_nlink!=1 ||
            flock(fd,LOCK_SH|LOCK_NB)) { close(fd); return 0; }
    } else if (errno!=ENOENT) return 0;
    /* Retain pending identity until exitedCommon/launchFailed, not merely until
     * launch returns: launchd may not have assigned a PID at that point. */
    remember(app);
    BOOL result=original_launch(app,cmd);
    if (!result) forget(app);
    if (fd>=0) close(fd);
    return result;
}
static void exited(id app,SEL cmd) { forget(app); original_exit(app,cmd); }
static void failed(id app,SEL cmd) { forget(app); original_failed(app,cmd); }
static void deallocated(id app,SEL cmd) { forget(app); original_dealloc(app,cmd); }

static BOOL live_file_lease(void) {
    int fd=open(S5LBOX_HLE_GUARD_PATH,O_RDONLY|O_NOFOLLOW);
    if (fd<0) return 0;
    struct stat st;
    int valid=!fstat(fd,&st) && S_ISREG(st.st_mode) && !st.st_uid && st.st_nlink==1 && !(st.st_mode&0022);
    int held=valid?flock(fd,LOCK_SH|LOCK_NB):0,error=errno; close(fd);
    return valid && held && (error==EWOULDBLOCK || error==EAGAIN);
}
static void idle(id app,SEL cmd) {
    if (live_file_lease()) [(SpringBoard *)app resetIdleTimer];
    else original_idle(app,cmd);
}
static void auto_lock(id app,SEL cmd) {
    if (live_file_lease()) [(SpringBoard *)app resetIdleTimer];
    else original_auto_lock(app,cmd);
}
static void icon_launch(id icon,SEL cmd) {
    SBApplication *app=[(SBApplicationIcon *)icon application];
    if (container_path(app) && s5lbox_hle_route_icon(&routes,[[app bundleIdentifier] UTF8String],uptime())) return;
    original_icon_launch(icon,cmd);
}
static int native_launch(void *context,const char *identifier) {
    (void)context;
    if (has_lock_state && [(SpringBoard *)[(id)objc_getClass("SpringBoard") sharedApplication] isLocked]) return EACCES;
    int fd=open(S5LBOX_HLE_GUARD_PATH,O_RDONLY|O_NOFOLLOW);
    if (fd>=0) {
        struct stat st;
        int busy=fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid || st.st_nlink!=1 ||
            flock(fd,LOCK_SH|LOCK_NB);
        close(fd); if (busy) return EBUSY;
    } else if (errno!=ENOENT) return EACCES;
    NSArray *apps=[[(id)objc_getClass("SBApplicationController") sharedInstance] allApplications];
    for (unsigned i=0;i<[apps count];i++) {
        SBApplication *app=[apps objectAtIndex:i];
        const char *name=[[app bundleIdentifier] UTF8String];
        if (container_path(app) && name && !strcmp(name,identifier)) {
            /* Bypass only the icon-routing hook, never the native launch guard. */
            [(SBUIController *)[(id)objc_getClass("SBUIController") sharedInstance] activateApplicationAnimated:app];
            return 0;
        }
    }
    return ENOENT;
}
static CFDataRef route_query(CFDataRef data) {
    static unsigned char nonce[32],cached[HLE_ROUTE_MAX_REPLY];
    static size_t cached_length;
    if (!original_icon_launch || !data || CFDataGetLength(data)<59 ||
        CFDataGetLength(data)>32+HLE_ROUTE_MAX_REQUEST) return NULL;
    size_t count=(size_t)CFDataGetLength(data);
    int fd=open(S5LBOX_HLE_ROUTE_PATH,O_RDONLY|O_NOFOLLOW); if (fd<0) return NULL;
    struct stat st; struct flock lock; memset(&lock,0,sizeof lock);
    lock.l_type=F_RDLCK; lock.l_whence=SEEK_SET;
    unsigned char *trusted=malloc(count);
    int valid=trusted && !fstat(fd,&st) && S_ISREG(st.st_mode) && !st.st_uid &&
        st.st_nlink==1 && !(st.st_mode&0022) && st.st_size==(off_t)count &&
        !fcntl(fd,F_GETLK,&lock) && lock.l_type==F_WRLCK &&
        read(fd,trusted,count)==(ssize_t)count && !memcmp(trusted,CFDataGetBytePtr(data),count);
    close(fd);
    if (!valid) { free(trusted); return NULL; }
    /* Replaying an authorized packet while the root broker owns its lock
     * must not repeat native activation or consume another launch ticket. */
    if (!cached_length || memcmp(nonce,trusted,32)) {
        memcpy(nonce,trusted,32);
        cached_length=s5lbox_hle_route_request(&routes,trusted+32,count-32,uptime(),cached,native_launch,NULL);
    }
    free(trusted); return CFDataCreate(NULL,cached,(CFIndex)cached_length);
}
static CFDataRef query(CFMessagePortRef port,SInt32 message,CFDataRef data,void *info) {
    (void)port; (void)info;
    if (message==2) return route_query(data);
    int error=EINVAL;
    if (message!=1 || !data || CFDataGetLength(data)!=36) goto done;
    const char *uuid=(const char *)CFDataGetBytePtr(data);
    for (unsigned i=0;i<36;i++) {
        int dash=i==8||i==13||i==18||i==23;
        if (dash?uuid[i]!='-':!((uuid[i]>='0'&&uuid[i]<='9')||
            (uuid[i]>='a'&&uuid[i]<='f')||(uuid[i]>='A'&&uuid[i]<='F'))) goto done;
    }
    /* A query without the broker's exclusive kernel lock is not a lease. */
    int fd=open(S5LBOX_HLE_GUARD_PATH,O_RDONLY|O_NOFOLLOW);
    if (fd<0) { error=ENOTCONN; goto done; }
    int held=flock(fd,LOCK_SH|LOCK_NB); int lock_error=errno; close(fd);
    if (!held || (lock_error!=EWOULDBLOCK && lock_error!=EAGAIN)) { error=ENOTCONN; goto done; }
    if (overflow) { error=EBUSY; goto done; }
    for (unsigned i=0;i<128;i++) if (pending[i]) { error=EBUSY; goto done; }
    SBApplicationController *controller=[(id)objc_getClass("SBApplicationController") sharedInstance];
    NSArray *apps=[controller allApplications];
    NSArray *jobs=[(id)objc_getClass("SBLaunchdUtilities") allJobLabels];
    if (!apps || !jobs) { error=ENOTCONN; goto done; }
    error=ENOENT;
    for (unsigned i=0;i<[apps count];i++) {
        SBApplication *app=[apps objectAtIndex:i]; const char *path=container_path(app);
        if (!path) continue;
        if ([app pid]>0) { error=EBUSY; goto done; }
        /* Also reject a retained launchd job after SpringBoard restarted,
         * including a job whose PID has not appeared yet. */
        const char *label=[[app launchdJobLabel] UTF8String];
        if (label && *label) for (unsigned j=0;j<[jobs count];j++) {
            const char *job=[[jobs objectAtIndex:j] UTF8String];
            if (job && !strcmp(label,job)) { error=EBUSY; goto done; }
        }
        if (strlen(path)>36 && !memcmp(path,uuid,36) && path[36]=='/') error=0;
    }
done:;
    unsigned char bytes[8]={'H','L','E','1'}; uint32_t wire=htonl((uint32_t)error);
    memcpy(bytes+4,&wire,4); return CFDataCreate(NULL,bytes,sizeof bytes);
}
static int signature(Method method,const char *expected) {
    if (!method) return 0;
    const char *actual=method_getTypeEncoding(method);
    /* Frame offsets vary with host word size, not the callable types. */
    while (*actual) {
        if (*actual>='0' && *actual<='9') { actual++; continue; }
        if (*actual++!=*expected++) return 0;
    }
    return !*expected;
}
#ifndef S5LBOX_HLE_GUARD_TEST
__attribute__((constructor))
#endif
void s5lbox_hle_guard_install(void) {
    Class app=objc_getClass("SBApplication");
    const char *names[]={"launch","exitedCommon","launchFailed","dealloc"};
    const char *types[]={"c@:","v@:","v@:","v@:"};
    Method methods[4];
    if (!app || !objc_getClass("SBApplicationController") || !objc_getClass("SBLaunchdUtilities")) return;
    for (unsigned i=0;i<4;i++) {
        methods[i]=class_getInstanceMethod(app,sel_registerName(names[i]));
        if (!signature(methods[i],types[i])) return;
    }
    for (unsigned i=0;i<3;i++) {
        const char *selector=i==0?"path":i==1?"launchdJobLabel":"pid";
        if (!signature(class_getInstanceMethod(app,sel_registerName(selector)),i==2?"i@:":"@@:")) return;
    }
    Class controller=objc_getClass("SBApplicationController"),utilities=objc_getClass("SBLaunchdUtilities");
    if (!signature(class_getClassMethod(controller,sel_registerName("sharedInstance")),"@@:") ||
        !signature(class_getInstanceMethod(controller,sel_registerName("allApplications")),"@@:") ||
        !signature(class_getClassMethod(utilities,sel_registerName("allJobLabels")),"@@:")) return;
    original_launch=(void *)method_setImplementation(methods[0],HLE_IMP(launch));
    original_exit=(void *)method_setImplementation(methods[1],HLE_IMP(exited));
    original_failed=(void *)method_setImplementation(methods[2],HLE_IMP(failed));
    original_dealloc=(void *)method_setImplementation(methods[3],HLE_IMP(deallocated));
    Class icon=objc_getClass("SBApplicationIcon"),ui=objc_getClass("SBUIController");
    Method icon_method=icon?class_getInstanceMethod(icon,sel_registerName("launch")):NULL;
    if (signature(icon_method,"v@:") &&
        signature(class_getInstanceMethod(icon,sel_registerName("application")),"@@:") &&
        signature(class_getInstanceMethod(app,sel_registerName("bundleIdentifier")),"@@:") &&
        signature(class_getClassMethod(ui,sel_registerName("sharedInstance")),"@@:") &&
        signature(class_getInstanceMethod(ui,sel_registerName("activateApplicationAnimated:")),"v@:@"))
        original_icon_launch=(void *)method_setImplementation(icon_method,HLE_IMP(icon_launch));
    Class springboard=objc_getClass("SpringBoard");
    if (springboard) {
        Method idle_method=class_getInstanceMethod(springboard,sel_registerName("didIdle"));
        Method lock_method=class_getInstanceMethod(springboard,sel_registerName("autoLock"));
        if (signature(idle_method,"v@:") && signature(lock_method,"v@:") &&
            signature(class_getInstanceMethod(springboard,sel_registerName("resetIdleTimer")),"v@:")) {
            original_idle=(void *)method_setImplementation(idle_method,HLE_IMP(idle));
            original_auto_lock=(void *)method_setImplementation(lock_method,HLE_IMP(auto_lock));
        }
        has_lock_state=signature(class_getClassMethod(springboard,sel_registerName("sharedApplication")),"@@:") &&
            signature(class_getInstanceMethod(springboard,sel_registerName("isLocked")),"c@:");
    }
    CFMessagePortRef port=CFMessagePortCreateLocal(NULL,CFSTR(S5LBOX_HLE_GUARD_PORT),query,NULL,NULL);
    if (!port) return; /* Native still works; HLE refuses without a handshake. */
    CFRunLoopSourceRef source=CFMessagePortCreateRunLoopSource(NULL,port,0);
    if (source) { CFRunLoopAddSource(CFRunLoopGetMain(),source,kCFRunLoopCommonModes); CFRelease(source); }
    CFRelease(port);
}
