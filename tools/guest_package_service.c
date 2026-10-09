/* S5LBox guest-only package executor. MIT licensed.
 * Runs INSIDE iPhone OS 3, never on the host. Loopback + per-machine 256-bit
 * capability; no shell, arbitrary command, or client-chosen filesystem path.
 * Wire: SPM2 + 32 capability bytes + S/I/R/F. SPM1 remains read-compatible.
 * I/R requests include the SHA256 of the exact status read during planning.
 * I: BE32 count, then (BE32 length, SHA256, deb bytes) for each archive.
 * R: BE32 name length, package identifier. Uploads finish before any dpkg call.
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <poll.h>
#ifndef S5LBOX_PACKAGE_TEST
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#include "sha256.h"

#ifdef S5LBOX_PACKAGE_TEST
#define STATE "fixture"
#define STATUS STATE "/status"
#define DPKG STATE "/dpkg"
#define DPKG_LOCK STATE "/lock"
#define OWNER getuid()
#define SPRINGBOARD STATE "/SpringBoard.plist"
#define NOTIFY_CONFIG STATE "/notify.conf"
#else
#define STATE "/private/var/lib/s5lbox-package-manager-v1"
#define STATUS "/private/var/lib/dpkg/status"
#define DPKG "/usr/bin/dpkg"
#define DPKG_LOCK "/private/var/lib/dpkg/lock"
#define OWNER 0
#define SPRINGBOARD "/System/Library/LaunchDaemons/com.apple.SpringBoard.plist"
#define NOTIFY_CONFIG "/etc/notify.conf"
#endif
#define MAX_STATUS (4u*1024u*1024u)
#define MAX_DEB (64u*1024u*1024u)
static int peer = -1, connected = 1, logfd = -1;
static unsigned char buffer[65536];
// Cydia's ordered finish vocabulary: return, reopen, restart, reload, reboot.
// Persistent, boot-scoped state prevents a lost USB response from losing a request.
static unsigned char pending, finish_ready;
static uint64_t boot_id;
static int modern, finish_error;
static int io(int fd, void *buf, size_t length, int writing) {
    unsigned char *p = buf;
    while (length) {
        struct pollfd f = {fd, writing ? POLLOUT : POLLIN, 0};
        int ready = poll(&f,1,30000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return 0;
        ssize_t n = writing ? write(fd,p,length) : read(fd,p,length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n; length -= (size_t)n;
    }
    return 1;
}
static int frame(char kind, const void *bytes, size_t length) {
    uint32_t n = htonl((uint32_t)length + 1);
    if (!connected) return 0;
    connected = io(peer,&n,4,1) && io(peer,&kind,1,1) && io(peer,(void *)bytes,length,1);
    return connected;
}
static int failure(const char *why) { frame('E',why,strlen(why)); return 0; }
static int save_finish(void) {
    unsigned char state[10] = {pending,finish_ready};
    memcpy(state+2,&boot_id,8);
    char path[] = STATE "/finish-XXXXXX";
    int fd = mkstemp(path); if (fd < 0) return 0;
    int ok = io(fd,state,sizeof state,1) && !fsync(fd);
    close(fd);
    if (ok) ok = !rename(path,STATE "/pending-finish");
    if (!ok) unlink(path);
    return ok;
}
static int load_finish(void) {
    unsigned char state[10]; uint64_t saved_boot; struct stat st;
    pending=finish_ready=0;
    int fd = open(STATE "/pending-finish",O_RDONLY|O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT;
    int ok = !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==OWNER &&
        !(st.st_mode&0077) && st.st_size==sizeof state && io(fd,state,sizeof state,0);
    close(fd); if (!ok || state[0]>4 || state[1]>1) return 0;
    memcpy(&saved_boot,state+2,8);
    if (saved_boot==boot_id) { pending=state[0]; finish_ready=state[1]; }
    return 1;
}
static void request_finish(unsigned char action) {
    if (action <= pending) return;
    pending=action;
    if (!save_finish()) finish_error=1;
}
static int send_finish(void) {
    unsigned char result[2] = {pending,finish_ready};
    return !modern || frame('F',result,sizeof result);
}
typedef struct finish_parser { char line[64]; unsigned length; int overflow; } finish_parser;
static void finish_bytes(finish_parser *p, const unsigned char *data, size_t size) {
    static const char *names[] = {"return","reopen","restart","reload","reboot"};
    for (size_t i=0;i<size;i++) {
        if (data[i]=='\n') {
            if (!p->overflow) {
                p->line[p->length]=0;
                for (unsigned j=0;j<5;j++) if (p->length==7+strlen(names[j]) &&
                    !memcmp(p->line,"finish:",7) && !strcmp(p->line+7,names[j])) request_finish((unsigned char)j);
            }
            p->length=0; p->overflow=0;
        } else if (p->length < sizeof p->line-1) p->line[p->length++]=(char)data[i];
        else p->overflow=1;
    }
}
static int fingerprint(const char *path, unsigned char hash[33]) {
    memset(hash,0,33); int fd=open(path,O_RDONLY); struct stat st;
    if (fd<0) return errno==ENOENT;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size>MAX_STATUS) { close(fd); return 0; }
    ios3_sha256_context_t context; ios3_sha256_init(&context);
    ssize_t n; size_t total=0;
    while ((n=read(fd,buffer,sizeof buffer))>0) {
        total+=(size_t)n; if (total>MAX_STATUS) { close(fd); return 0; }
        ios3_sha256_update(&context,buffer,(size_t)n);
    }
    close(fd); if (n<0) return 0;
    hash[0]=1; ios3_sha256_final(&context,hash+1); return 1;
}
static unsigned char *status_data(size_t *size) {
    int fd = open(STATUS,O_RDONLY|O_NOFOLLOW); struct stat s;
    if (fd < 0) return NULL;
    if (fstat(fd,&s) || !S_ISREG(s.st_mode) || s.st_size < 1 || s.st_size > MAX_STATUS) { close(fd); return NULL; }
    unsigned char *data = malloc((size_t)s.st_size);
    if (!data || !io(fd,data,(size_t)s.st_size,0)) { free(data); close(fd); return NULL; }
    close(fd); *size = (size_t)s.st_size; return data;
}
static int send_status(char kind) {
    size_t size; unsigned char *data = status_data(&size);
    if (!data) return failure("The guest dpkg database is unavailable. Finish jailbreak setup first.");
    int ok = frame(kind,data,size); free(data); return ok;
}
static int read_number(uint32_t *n) { if (!io(peer,n,4,0)) return 0; *n = ntohl(*n); return 1; }
static int valid_name(const char *name) {
    size_t n = strlen(name); if (n < 2 || n > 200 || name[0] == '-') return 0;
    for (size_t i = 0; i < n; i++) if (!((name[i]>='a'&&name[i]<='z') ||
        (name[i]>='0'&&name[i]<='9') || name[i]=='+' || name[i]=='-' || name[i]=='.')) return 0;
    return 1;
}
static int execute(char *const argv[], int collect_finish) {
    int pipes[2], finishes[2]; if (pipe(pipes)) return -1;
    if (pipe(finishes)) { close(pipes[0]); close(pipes[1]); return -1; }
    pid_t child = fork();
    if (!child) {
        close(finishes[0]);
        close(pipes[0]); dup2(pipes[1],1); dup2(pipes[1],2); close(pipes[1]);
        int nullfd = open("/dev/null",O_RDONLY); if (nullfd >= 0) { dup2(nullfd,0); close(nullfd); }
        close(peer); if (logfd >= 0) close(logfd);
        setenv("PATH","/usr/bin:/bin:/usr/sbin:/sbin",1);
        setenv("DEBIAN_FRONTEND","noninteractive",1);
        // Maintainer scripts inherit this dedicated descriptor, not stdout.
        if (collect_finish) { char value[32]; snprintf(value,sizeof value,"%d 1",finishes[1]); setenv("CYDIA",value,1); }
        else { close(finishes[1]); unsetenv("CYDIA"); }
        execv(argv[0],argv); _exit(127);
    }
    close(pipes[1]); close(finishes[1]);
    if (child < 0) { close(pipes[0]); close(finishes[0]); return -1; }
    struct pollfd streams[2] = {{pipes[0],POLLIN,0},{finishes[0],POLLIN,0}};
    finish_parser parser = {{0},0,0}; int status=0, exited=0;
    while (streams[0].fd>=0 || streams[1].fd>=0) {
        // Heartbeats prevent an otherwise silent maintainer script from being
        // mistaken for an idle broken USB stream. Disconnect never kills dpkg.
        int ready = poll(streams,2,1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (!ready) {
            // A background process inherited a script fd: don't wait forever
            // once dpkg itself has exited. Drain buffered requests first.
            if (exited) break;
            frame('L',"",0);
        }
        for (unsigned i=0;i<2;i++) if (streams[i].fd>=0 && streams[i].revents) {
            ssize_t n=read(streams[i].fd,buffer,sizeof buffer);
            if (n<0 && errno==EINTR) continue;
            if (n<=0) { close(streams[i].fd); streams[i].fd=-1; continue; }
            if (i==1) finish_bytes(&parser,buffer,(size_t)n);
            else { if (logfd>=0) io(logfd,buffer,(size_t)n,1); frame('L',buffer,(size_t)n); }
        }
        if (!exited) { pid_t result=waitpid(child,&status,WNOHANG); if (result==child) exited=1; }
    }
    for (unsigned i=0;i<2;i++) if (streams[i].fd>=0) close(streams[i].fd);
    if (!exited) while (waitpid(child,&status,0) < 0) { if (errno != EINTR) return -1; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
static int transaction(char op) {
    unsigned char expected[32], current[32]; size_t status_size;
    if (!io(peer,expected,32,0)) return 0;
    unsigned char hint=0;
    if (modern && (!io(peer,&hint,1,0) || (hint!=0 && hint!=2))) return failure("Invalid finish hint.");
    unsigned char *status = status_data(&status_size);
    if (!status) return failure("No guest package database.");
    ios3_sha256(status,status_size,current); free(status);
    if (memcmp(current,expected,32)) return failure("Installed packages changed. Refresh and review the new plan.");
    // dpkg owns its own lock during each invocation; this preflight avoids
    // uploads while another package operation is already running.
    int lockfd = open(DPKG_LOCK,O_RDWR|O_CREAT|O_NOFOLLOW,0644);
    struct flock lock; memset(&lock,0,sizeof lock); lock.l_type = F_WRLCK; lock.l_whence = SEEK_SET;
    if (lockfd < 0 || fcntl(lockfd,F_SETLK,&lock)) { if (lockfd>=0) close(lockfd); return failure("dpkg is busy. Close Cydia and retry when its operation finishes."); }
    close(lockfd);
    char directory[] = STATE "/transaction-XXXXXX";
    if (!mkdtemp(directory)) return failure("Cannot create guest package staging directory.");
    char paths[128][256]; unsigned count = 0; uint32_t total = 0; int ok = 0;
    char name[201] = {0};
    if (op == 'I') {
        uint32_t requested;
        if (!read_number(&requested) || !requested || requested > 128) goto done;
        for (unsigned i = 0; i < requested; i++) {
            uint32_t size; unsigned char checksum[32], actual[32];
            if (!read_number(&size) || !size || size > MAX_DEB || total > 256u*1024u*1024u-size || !io(peer,checksum,32,0)) goto done;
            total += size; snprintf(paths[count],sizeof paths[count],"%s/%03u.deb",directory,count);
            int fd = open(paths[count],O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
            if (fd < 0) goto done;
            count++; ios3_sha256_context_t hash; ios3_sha256_init(&hash); int transferred = 1;
            while (size) {
                size_t n = size < sizeof buffer ? size : sizeof buffer;
                if (!io(peer,buffer,n,0) || !io(fd,buffer,n,1)) { transferred = 0; break; }
                ios3_sha256_update(&hash,buffer,n); size -= (uint32_t)n;
            }
            if (fsync(fd)) transferred = 0;
            close(fd); ios3_sha256_final(&hash,actual);
            if (!transferred || memcmp(checksum,actual,32)) { failure("Guest upload checksum failed. Nothing was installed."); goto done; }
        }
    } else {
        uint32_t length;
        if (!read_number(&length) || length < 2 || length > 200 || !io(peer,name,length,0) || strlen(name) != length || !valid_name(name)) goto done;
    }
    // Recheck the status after upload, before permitting any mutation.
    status = status_data(&status_size);
    if (!status) goto done;
    ios3_sha256(status,status_size,current); free(status);
    if (memcmp(current,expected,32)) { failure("Installed packages changed during transfer. Review again."); goto done; }
    logfd = open(STATE "/last-operation.log",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if (logfd < 0) { failure("Cannot create guest transaction log."); goto done; }
    unsigned char before[2][33], after[33];
    if (!fingerprint(SPRINGBOARD,before[0]) || !fingerprint(NOTIFY_CONFIG,before[1])) {
        close(logfd); logfd=-1; failure("Cannot check guest restart configuration."); goto done;
    }
    finish_ready=0; finish_error=0;
    if (!save_finish()) { close(logfd); logfd=-1; failure("Cannot preserve guest finish requests."); goto done; }
    request_finish(hint);
    frame('L',"Running guest dpkg...\n",21);
    ok = 1;
    if (op == 'I') {
        for (unsigned i = 0; i < count && ok; i++) {
            char *args[] = {DPKG,"--force-confdef","--force-confold","--install",paths[i],NULL};
            ok = execute(args,1) == 0;
        }
    } else {
        char *args[] = {DPKG,"--remove",name,NULL}; ok = execute(args,1) == 0;
    }
    if (!fingerprint(SPRINGBOARD,after)) finish_error=1;
    else if (memcmp(before[0],after,33)) request_finish(3);
    if (!fingerprint(NOTIFY_CONFIG,after)) finish_error=1;
    else if (memcmp(before[1],after,33)) request_finish(4);
    finish_ready=(unsigned char)(ok && !finish_error);
    if (!save_finish()) finish_error=1;
    fsync(logfd); close(logfd); logfd = -1;
    send_finish();
    if (ok && !finish_error) send_status('D');
    else if (finish_error) failure("Packages may have changed, but restart tracking failed. Refresh Installed and restart the guest manually.");
    else failure("Guest dpkg failed. Some packages may have changed; refresh Installed. Details are in the transaction log.");
done:
    for (unsigned i = 0; i < count; i++) unlink(paths[i]);
    rmdir(directory); return ok;
}
static void apply_finish(void) {
    unsigned char action, expected[32], actual[32]; size_t size;
    if (!io(peer,&action,1,0) || !io(peer,expected,32,0)) return;
    unsigned char *data=status_data(&size);
    if (!data) { failure("No guest package database."); return; }
    ios3_sha256(data,size,actual); free(data);
    if (action!=pending || !action || !finish_ready || memcmp(expected,actual,32)) {
        failure("Finish request or installed packages changed. Refresh Installed first."); return;
    }
    int lockfd=open(DPKG_LOCK,O_RDWR|O_NOFOLLOW);
    struct flock lock; memset(&lock,0,sizeof lock); lock.l_type=F_WRLCK; lock.l_whence=SEEK_SET;
    if (lockfd<0 || fcntl(lockfd,F_SETLK,&lock)) { if (lockfd>=0) close(lockfd); failure("dpkg is busy. Finish that operation first."); return; }
    // Keep this lock through the restart, preventing a concurrent dpkg launch.
#ifdef S5LBOX_PACKAGE_TEST
    char *args[]={STATE "/finish",action==4 ? "reboot" : action==3 ? "reload" : "restart",NULL};
#else
    char *args[]={action==4 ? "/sbin/reboot" : "/bin/launchctl",
        action==4 ? NULL : action==3 ? "unload" : "stop",
        action==3 ? SPRINGBOARD : "com.apple.SpringBoard",NULL};
#endif
    if (action>1 && access(args[0],X_OK)) { close(lockfd); failure("Guest restart command is unavailable."); return; }
    // Acceptance is not a claim that SpringBoard/boot has completed. Reboot can
    // close USB and terminate this service; boot-scoped state handles that.
    if (frame('A',&action,1)) {
        int ok=action==1 || execute(args,0)==0;
#ifndef S5LBOX_PACKAGE_TEST
        if (action==3) {
            char *load[]={"/bin/launchctl","load",SPRINGBOARD,NULL};
            // Always attempt load, even if unload reported an error.
            int loaded=execute(load,0)==0; ok=ok && loaded;
        }
#endif
        if (ok) { pending=finish_ready=0; if (!save_finish()) ok=0; }
        if (ok) frame('C',&action,1);
        else failure("Guest restart command failed. The request is still pending; refresh and retry.");
    }
    close(lockfd);
}
static void connection(void) {
    unsigned char request[37], token[32];
    int fd = open(STATE "/capability",O_RDONLY|O_NOFOLLOW); struct stat s;
    if (fd < 0) return;
    int valid = !fstat(fd,&s) && S_ISREG(s.st_mode) && s.st_uid == OWNER && !(s.st_mode&0077) && s.st_size == 32 && io(fd,token,32,0);
    close(fd);
    if (!valid || !io(peer,request,sizeof request,0)) return;
    unsigned mismatch = 0; for (unsigned i=0;i<32;i++) mismatch |= token[i]^request[4+i];
    modern=!memcmp(request,"SPM2",4);
    if ((!modern && memcmp(request,"SPM1",4)) || mismatch) return;
    if (!load_finish()) { failure("Guest finish state is invalid; repair package setup before continuing."); return; }
    if (request[36] == 'S') { send_finish(); send_status('S'); }
    else if (request[36] == 'I' || request[36] == 'R') transaction(request[36]);
    else if (modern && request[36]=='F') apply_finish();
}
int main(void) {
    umask(0077); signal(SIGPIPE,SIG_IGN);
#ifdef S5LBOX_PACKAGE_TEST
    const char *test_boot=getenv("S5LBOX_TEST_BOOT"); boot_id=test_boot ? strtoull(test_boot,NULL,10) : 1;
#else
    int mib[2]={CTL_KERN,KERN_BOOTTIME}; struct timeval boot; size_t size=sizeof boot;
    if (sysctl(mib,2,&boot,&size,NULL,0) || size!=sizeof boot) return 1;
    boot_id=(uint64_t)boot.tv_sec*1000000u+(uint64_t)boot.tv_usec;
#endif
    int listener = socket(AF_INET,SOCK_STREAM,0); if (listener < 0) return 1;
    fcntl(listener,F_SETFD,FD_CLOEXEC); int yes=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
    struct sockaddr_in address; memset(&address,0,sizeof address);
    address.sin_family=AF_INET; address.sin_port=htons(64321); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (bind(listener,(struct sockaddr *)&address,sizeof address) || listen(listener,2)) return 1;
    for (;;) {
        peer = accept(listener,NULL,NULL); if (peer < 0) { if (errno==EINTR) continue; return 1; }
        fcntl(peer,F_SETFD,FD_CLOEXEC); connected=1; connection(); close(peer); peer=-1;
    }
}
