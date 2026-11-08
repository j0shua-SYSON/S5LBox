/* Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * Guest-kernel-owned file descriptors for the interpreter's live app sandbox.
 * No HFS image access, host-side mirrors, shell commands, or retry/replay of
 * mutations. See integrations/touchhle-cpu/files-protocol.md. */
#define _DEFAULT_SOURCE
#include "guest_hle_files.h"
#if defined(__APPLE__) && !defined(S5LBOX_PACKAGE_TEST)
#include "guest_hle_launch_guard.h"
#endif
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <grp.h>
#include <dirent.h>
#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define LIMIT 65536u
#define HANDLES S5LBOX_HLE_FILE_HANDLE_LIMIT
typedef struct { uint32_t id; int fd; DIR *dir; } handle;
typedef struct { const unsigned char *p; size_t n; } cursor;
static uint32_t number(const unsigned char *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static void put32(unsigned char *p, uint32_t n) {
    p[0]=(unsigned char)(n>>24); p[1]=(unsigned char)(n>>16);
    p[2]=(unsigned char)(n>>8); p[3]=(unsigned char)n;
}
static void put64(unsigned char *p, uint64_t n) { put32(p,(uint32_t)(n>>32)); put32(p+4,(uint32_t)n); }
static int take32(cursor *c, uint32_t *n) {
    if (c->n<4) return 0;
    *n=number(c->p); c->p+=4; c->n-=4; return 1;
}
static int take64(cursor *c, uint64_t *n) {
    uint32_t hi,lo;
    if (!take32(c,&hi) || !take32(c,&lo)) return 0;
    *n=(uint64_t)hi<<32 | lo; return 1;
}
static int path(cursor *c, char out[1024]) {
    uint32_t n;
    if (!take32(c,&n) || !n || n>=1024 || n>c->n || memchr(c->p,0,n)) return 0;
    memcpy(out,c->p,n); out[n]=0; c->p+=n; c->n-=n;
    return out[0]=='/';
}
static int transfer(int fd, void *data, size_t n, int writing, int idle) {
    unsigned char *p=data;
    while (n) {
        struct pollfd item={fd,writing?POLLOUT:POLLIN,0};
        int ready=poll(&item,1,idle?-1:5000);
        if (ready<0 && errno==EINTR) continue;
        if (ready<=0) return 0;
        ssize_t done=writing?write(fd,p,n):read(fd,p,n);
        if (done<0 && errno==EINTR) continue;
        if (done<=0) return 0;
        p+=done; n-=(size_t)done; idle=0;
    }
    return 1;
}
static int reply(int peer, int error, unsigned char *bytes, size_t n) {
    unsigned char header[8]; put32(header,(uint32_t)n+4); put32(header+4,(uint32_t)error);
    return transfer(peer,header,8,1,0) && transfer(peer,bytes,n,1,0);
}
static handle *find(handle *items, uint32_t id) {
    if (!id) return NULL;
    for (unsigned i=0;i<HANDLES;i++) if (items[i].id==id) return items+i;
    return NULL;
}
static void close_handle(handle *item) {
    if (item->dir) closedir(item->dir);
    else if (item->fd>=0) close(item->fd);
    item->id=0; item->fd=-1; item->dir=NULL;
}
static int request(handle *items, uint32_t *next, unsigned op, cursor c,
                   unsigned char *out, size_t *size) {
    char a[1024],b[1024]; uint32_t id,flags,count;
    uint64_t value,length; handle *item; struct stat st;
    *size=0;
    if (op==1) { /* stat follows symlinks, but only inside the chroot. */
        if (!path(&c,a) || c.n) return EINVAL;
        if (stat(a,&st)) return errno;
        put64(out,(uint64_t)st.st_size); put64(out+8,(uint64_t)st.st_mtime);
        put32(out+16,(uint32_t)st.st_mode); put32(out+20,(uint32_t)st.st_uid);
        put32(out+24,(uint32_t)st.st_gid); put64(out+28,(uint64_t)st.st_ino);
        *size=36; return 0;
    }
    if (op==2 || op==14) { /* open regular file or directory */
        if (!take32(&c,&flags) || !path(&c,a) || c.n || (flags&~63u)) return EINVAL;
        if (op==14 && flags) return EINVAL;
        item=NULL;
        for (unsigned i=0;i<HANDLES;i++) if (!items[i].id) { item=items+i; break; }
        if (!item || !*next) return EMFILE;
        int fd=-1; DIR *dir=NULL;
        if (op==14) { dir=opendir(a); if (!dir) return errno; }
        else {
            /* Stable wire flags, never platform-specific O_* values. */
            if (!(flags&3) || ((flags&60) && !(flags&2)) || ((flags&32) && !(flags&8))) return EINVAL;
            int oflags=(flags&3)==3?O_RDWR:(flags&2)?O_WRONLY:O_RDONLY;
            if (flags&4) oflags|=O_APPEND;
            if (flags&8) oflags|=O_CREAT;
            if (flags&16) oflags|=O_TRUNC;
            if (flags&32) oflags|=O_EXCL;
            /* O_NONBLOCK avoids hanging on a FIFO planted in app data. */
            fd=open(a,oflags|O_NONBLOCK,0600); if (fd<0) return errno;
            int error=0;
            if (fstat(fd,&st)) error=errno;
            else if (!S_ISREG(st.st_mode)) error=S_ISDIR(st.st_mode)?EISDIR:EACCES;
            if (error) { close(fd); return error; }
        }
        item->id=(*next)++; item->fd=fd; item->dir=dir;
        put32(out,item->id); *size=4; return 0;
    }
    if (op==9 || op==10 || op==11 || op==15) {
        if (!path(&c,a) || (op==11 && !path(&c,b)) || c.n) return EINVAL;
        int result=op==9?mkdir(a,0700):op==10?unlink(a):op==11?rename(a,b):rmdir(a);
        return result?errno:0;
    }
    if (!take32(&c,&id)) return EINVAL;
    item=find(items,id); if (!item) return EBADF;
    if (op==3) {
        if (c.n) return EINVAL;
        int result=item->dir?closedir(item->dir):close(item->fd);
        int error=result?errno:0;
        item->id=0; item->fd=-1; item->dir=NULL; return error;
    }
    if (op==12) {
        if (c.n) return EINVAL;
        if (!item->dir) return ENOTDIR;
        size_t used=4; count=0;
        while (count<128) {
            errno=0; struct dirent *entry=readdir(item->dir);
            if (!entry) { if (errno) return errno; break; }
            if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
            size_t n=strlen(entry->d_name);
            if (n>255 || used+4+n>LIMIT) return EOVERFLOW;
            put32(out+used,(uint32_t)n); memcpy(out+used+4,entry->d_name,n);
            used+=4+n; count++;
        }
        put32(out,count); *size=used; return 0;
    }
    if (item->dir) return EISDIR;
    if (op==4) {
        if (!take32(&c,&count) || count>LIMIT || c.n) return EINVAL;
        ssize_t done; do { done=read(item->fd,out,count); } while (done<0 && errno==EINTR);
        if (done<0) return errno;
        *size=(size_t)done; return 0;
    }
    if (op==5) {
        ssize_t done; do { done=write(item->fd,c.p,c.n); } while (done<0 && errno==EINTR);
        if (done<0) return errno;
        put32(out,(uint32_t)done); *size=4; return 0;
    }
    if (op==6) {
        if (!take32(&c,&flags) || flags>2 || !take64(&c,&value) || c.n) return EINVAL;
        off_t offset=(off_t)(int64_t)value;
        if ((int64_t)offset!=(int64_t)value) return EOVERFLOW;
        off_t result=lseek(item->fd,offset,flags==0?SEEK_SET:flags==1?SEEK_CUR:SEEK_END);
        if (result<0) return errno;
        put64(out,(uint64_t)result); *size=8; return 0;
    }
    if (op==7) { if (c.n) return EINVAL; return fsync(item->fd)?errno:0; }
    if (op==8) {
        if (!take64(&c,&value) || c.n || value>INT64_MAX) return EINVAL;
        off_t offset=(off_t)value; if ((uint64_t)offset!=value) return EOVERFLOW;
        return ftruncate(item->fd,offset)?errno:0;
    }
    if (op==13 || op==16) { /* nonblocking POSIX byte-range lock / query */
        if (!take32(&c,&flags) || flags<1 || flags>3 || !take64(&c,&value) ||
            !take64(&c,&length) || c.n || value>INT64_MAX || length>INT64_MAX ||
            length>INT64_MAX-value) return EINVAL;
        struct flock lock; memset(&lock,0,sizeof lock);
        lock.l_type=flags==1?F_RDLCK:flags==2?F_WRLCK:F_UNLCK;
        lock.l_whence=SEEK_SET; lock.l_start=(off_t)value; lock.l_len=(off_t)length;
        if ((uint64_t)lock.l_start!=value || (uint64_t)lock.l_len!=length) return EOVERFLOW;
        if (fcntl(item->fd,op==13?F_SETLK:F_GETLK,&lock)) return errno;
        if (op==16) {
            put32(out,lock.l_type==F_UNLCK?3:lock.l_type==F_RDLCK?1:2);
            put64(out+4,(uint64_t)lock.l_start); put64(out+12,(uint64_t)lock.l_len);
            put32(out+20,(uint32_t)lock.l_pid); *size=24;
        }
        return 0;
    }
    if (op==17) {
        if (!take32(&c,&flags) || c.n || flags<1 || flags>3) return EINVAL;
        return flock(item->fd,(flags==1?LOCK_SH:flags==2?LOCK_EX:LOCK_UN)|LOCK_NB)?errno:0;
    }
    return ENOSYS;
}
int s5lbox_hle_files(int peer, const char *state, const char *applications,
                     uid_t mobile_uid, gid_t mobile_gid, int guard_launches) {
    unsigned char uuid[37]; char root[PATH_MAX],lease[PATH_MAX]; struct stat st;
    int leasefd=-1,rootfd=-1,guardfd=-1,error=0;
    /* Length is fixed: no path, command or arbitrary root from the client. */
    if (!transfer(peer,uuid,36,0,0)) return 1;
    uuid[36]=0;
    for (unsigned i=0;i<36;i++) {
        int dash=i==8||i==13||i==18||i==23;
        if (dash?uuid[i]!='-':!((uuid[i]>='0'&&uuid[i]<='9')||
            (uuid[i]>='a'&&uuid[i]<='f')||(uuid[i]>='A'&&uuid[i]<='F'))) error=EINVAL;
    }
    if (!mobile_uid || !mobile_gid) error=EINVAL;
    if (snprintf(root,sizeof root,"%s/%s",applications,uuid)>=(int)sizeof root ||
        snprintf(lease,sizeof lease,"%s/hle.lease",state)>=(int)sizeof lease) error=ENAMETOOLONG;
    if (error) goto rejected;
    leasefd=open(lease,O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    if (leasefd<0) { error=errno; goto rejected; }
    if (fstat(leasefd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=0 || (st.st_mode&0077)) { error=EACCES; goto rejected; }
    if (flock(leasefd,LOCK_EX|LOCK_NB)) { error=EBUSY; goto rejected; }
    if (guard_launches) {
#if defined(__APPLE__) && !defined(S5LBOX_PACKAGE_TEST)
        guardfd=s5lbox_hle_launch_acquire((const char *)uuid);
        if (guardfd<0) { error=errno; goto rejected; }
#else
        error=ENOTSUP; goto rejected;
#endif
    }
    rootfd=open(root,O_RDONLY|O_NOFOLLOW);
    if (rootfd<0) { error=errno; goto rejected; }
    if (fstat(rootfd,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=mobile_uid) { error=EACCES; goto rejected; }
    if (fchdir(rootfd) || chroot(".") || chdir("/")) { error=errno; goto rejected; }
    close(rootfd); rootfd=-1;
    /* Leave room for the control socket, locks and inherited standard fds.
     * This changes only the forked service child's soft limit, never the
     * system limit or its parent's limit. A lower hard limit still applies. */
    struct rlimit files;
    if (!getrlimit(RLIMIT_NOFILE,&files) && files.rlim_cur<HANDLES+32u) {
        files.rlim_cur=files.rlim_max<HANDLES+32u?files.rlim_max:HANDLES+32u;
        (void)setrlimit(RLIMIT_NOFILE,&files);
    }
    if (setgroups(0,NULL) || setgid(mobile_gid) || setuid(mobile_uid) || getuid()!=mobile_uid || geteuid()!=mobile_uid) {
        error=EACCES; goto rejected;
    }
    /* Only the socket and a non-directory lease fd survive the jail entry.
     * A caller must close other inherited fds before calling this function. */
    if (!reply(peer,0,NULL,0)) { if (guardfd>=0) close(guardfd); close(leasefd); return 1; }
    unsigned char *input=malloc(LIMIT),*output=malloc(LIMIT);
    handle items[HANDLES]; memset(items,0,sizeof items); uint32_t next=1;
    if (!input || !output) { free(input); free(output); if (guardfd>=0) close(guardfd); close(leasefd); return 1; }
    for (;;) {
        unsigned char header[4];
        if (!transfer(peer,header,4,0,1)) break;
        uint32_t n=number(header);
        if (!n || n>LIMIT || !transfer(peer,input,n,0,0)) break;
        cursor c={input+1,n-1}; size_t size;
        error=request(items,&next,input[0],c,output,&size);
        if (!reply(peer,error,output,error?0:size)) break;
    }
    for (unsigned i=0;i<HANDLES;i++) if (items[i].id) close_handle(items+i);
    free(input); free(output); if (guardfd>=0) close(guardfd); close(leasefd); return 0;
rejected:
    reply(peer,error,NULL,0);
    if (rootfd>=0) close(rootfd);
    if (guardfd>=0) close(guardfd);
    if (leasefd>=0) close(leasefd);
    return 1;
}
