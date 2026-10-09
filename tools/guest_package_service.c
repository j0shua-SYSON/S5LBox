/* S5LBox guest-only package executor. MIT licensed.
 * Runs INSIDE iPhone OS 3, never on the host. Loopback + per-machine 256-bit
 * capability; no shell, arbitrary command, or client-chosen filesystem path.
 * Wire: SPM1 + 32 capability bytes + S/I/R. Replies: BE32 length, kind, bytes.
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
#include "sha256.h"

#ifdef S5LBOX_PACKAGE_TEST
#define STATE "fixture"
#define STATUS STATE "/status"
#define DPKG STATE "/dpkg"
#define DPKG_LOCK STATE "/lock"
#define OWNER getuid()
#else
#define STATE "/private/var/lib/s5lbox-package-manager-v1"
#define STATUS "/private/var/lib/dpkg/status"
#define DPKG "/usr/bin/dpkg"
#define DPKG_LOCK "/private/var/lib/dpkg/lock"
#define OWNER 0
#endif
#define MAX_STATUS (4u*1024u*1024u)
#define MAX_DEB (64u*1024u*1024u)
static int peer = -1, connected = 1, logfd = -1;
static unsigned char buffer[65536];
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
static int execute(char *const argv[]) {
    int pipes[2]; if (pipe(pipes)) return -1;
    pid_t child = fork();
    if (!child) {
        close(pipes[0]); dup2(pipes[1],1); dup2(pipes[1],2); close(pipes[1]);
        int nullfd = open("/dev/null",O_RDONLY); if (nullfd >= 0) { dup2(nullfd,0); close(nullfd); }
        close(peer); if (logfd >= 0) close(logfd);
        setenv("PATH","/usr/bin:/bin:/usr/sbin:/sbin",1);
        setenv("DEBIAN_FRONTEND","noninteractive",1);
        execv(argv[0],argv); _exit(127);
    }
    close(pipes[1]);
    if (child < 0) { close(pipes[0]); return -1; }
    for (;;) {
        // Heartbeats prevent an otherwise silent maintainer script from being
        // mistaken for an idle broken USB stream. Disconnect never kills dpkg.
        struct pollfd p = {pipes[0],POLLIN,0}; int ready = poll(&p,1,1000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (!ready) { frame('L',"",0); continue; }
        ssize_t n = read(pipes[0],buffer,sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (logfd >= 0) io(logfd,buffer,(size_t)n,1);
        frame('L',buffer,(size_t)n);
    }
    close(pipes[0]); int status = 0;
    while (waitpid(child,&status,0) < 0) { if (errno != EINTR) return -1; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
static int transaction(char op) {
    unsigned char expected[32], current[32]; size_t status_size;
    if (!io(peer,expected,32,0)) return 0;
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
    frame('L',"Running guest dpkg...\n",21);
    ok = 1;
    if (op == 'I') {
        for (unsigned i = 0; i < count && ok; i++) {
            char *args[] = {DPKG,"--force-confdef","--force-confold","--install",paths[i],NULL};
            ok = execute(args) == 0;
        }
    } else {
        char *args[] = {DPKG,"--remove",name,NULL}; ok = execute(args) == 0;
    }
    fsync(logfd); close(logfd); logfd = -1;
    if (ok) send_status('D');
    else failure("Guest dpkg failed. Some packages may have changed; refresh Installed. Details are in the transaction log.");
done:
    for (unsigned i = 0; i < count; i++) unlink(paths[i]);
    rmdir(directory); return ok;
}
static void connection(void) {
    unsigned char request[37], token[32];
    int fd = open(STATE "/capability",O_RDONLY|O_NOFOLLOW); struct stat s;
    if (fd < 0) return;
    int valid = !fstat(fd,&s) && S_ISREG(s.st_mode) && s.st_uid == OWNER && !(s.st_mode&0077) && s.st_size == 32 && io(fd,token,32,0);
    close(fd);
    if (!valid || !io(peer,request,sizeof request,0)) return;
    unsigned mismatch = 0; for (unsigned i=0;i<32;i++) mismatch |= token[i]^request[4+i];
    if (memcmp(request,"SPM1",4) || mismatch) return;
    if (request[36] == 'S') send_status('S');
    else if (request[36] == 'I' || request[36] == 'R') transaction(request[36]);
}
int main(void) {
    umask(0077); signal(SIGPIPE,SIG_IGN);
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
