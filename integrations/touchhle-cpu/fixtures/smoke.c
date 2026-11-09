/* Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * A real ARMv6 Mach-O test app for the interpreter/framework boundary. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <stdint.h>
#include <dlfcn.h>
#include <math.h>
#include <stdarg.h>
extern int __snprintf_chk(char *,size_t,int,size_t,const char *,...);
extern int __vsnprintf_chk(char *,size_t,int,size_t,const char *,va_list);
extern int __sprintf_chk(char *,int,size_t,const char *,...);
extern int __vsprintf_chk(char *,int,size_t,const char *,va_list);
extern double drand48(void), erand48(unsigned short *);
extern long lrand48(void), mrand48(void), nrand48(unsigned short *), jrand48(unsigned short *);
extern void srand48(long), lcong48(unsigned short *);
extern unsigned short *seed48(unsigned short *);
/* statvfs.h was not supplied by the early iPhone SDK. Darwin's ARM ABI
 * contains eleven unsigned longs (32 bits each). */
struct smoke_statvfs { uint32_t bsize,frsize,blocks,bfree,bavail,files,ffree,favail,fsid,flag,namemax; };
extern int statvfs(const char *,struct smoke_statvfs *);
extern int fstatvfs(int,struct smoke_statvfs *);

static int checked_vformat(char *buffer,size_t size,int bounded,const char *format,...) {
    va_list args; va_start(args,format);
    int result=bounded?__vsnprintf_chk(buffer,size,0,size,format,args):__vsprintf_chk(buffer,0,size,format,args);
    va_end(args); return result;
}

extern int pthread_cond_timedwait_relative_np(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);

extern unsigned thumb_sum(unsigned, unsigned);
static unsigned failures;
#define CHECK(value) do { if (!(value)) { failures++; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #value); } } while (0)

static int compare(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

static void rand48_smoke(void) {
    unsigned short zero[3]={0}, user[3]={1,2,3};
    unsigned short *old=seed48(zero);
    CHECK(old && old[0]==0x330e && old[1]==0xabcd && old[2]==0x1234);
    CHECK(seed48(old)==old); /* The input may alias the static result buffer. */
    srand48(0); CHECK(drand48()==0.17082803610628972);
    srand48(0); CHECK(lrand48()==366850414L);
    srand48(0); CHECK(mrand48()==733700828L);
    srand48(-1); CHECK(seed48(user)==old);
    CHECK(old[0]==0x330e && old[1]==0xffff && old[2]==0xffff);
    CHECK(nrand48(zero)==0 && zero[0]==11 && zero[1]==0 && zero[2]==0);
    unsigned short negative[3]={0xffff,0xffff,0xffff};
    CHECK(jrand48(negative)==-384749L);
    CHECK(negative[0]==0x199e && negative[1]==0x2113 && negative[2]==0xfffa);
    unsigned short custom[7]={1,0,0,2,0,0,3};
    lcong48(custom); CHECK(drand48()==5.0/281474976710656.0);
    unsigned short other[3]={4,0,0};
    CHECK(erand48(other)==11.0/281474976710656.0 && other[0]==11);
    CHECK(seed48(user)==old && old[0]==5 && old[1]==0 && old[2]==0);
    other[0]=1; other[1]=0; other[2]=0;
    (void)erand48(other); /* seed48 also resets the custom coefficients. */
    CHECK(other[0]==0xe678 && other[1]==0xdeec && other[2]==5);
    lcong48(custom); srand48(0); CHECK(lrand48()==366850414L);
    srand(19); int first=rand(); srand(19); srand48(91); CHECK(rand()==first);
}
static void *thread_main(void *arg) {
    return (void *)(unsigned long)thumb_sum((unsigned long)arg, 1);
}

static void *thread_exit_app(void *arg) {
    exit((int)(unsigned long)arg);
    return NULL;
}
static void *thread_park(void *arg) {
    (void)arg;
    for (;;) sleep(10);
    return NULL;
}

static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t exit_key, repeating_key;
static unsigned destructor_calls, repeating_calls, cleanup_order;
static pthread_t primary_thread;

static void exit_cleanup(void *value) {
    cleanup_order = cleanup_order * 10 + (unsigned long)value;
    usleep(1000); /* Cleanup may yield while its stack is still alive. */
}
static void exit_destructor(void *value) {
    CHECK(pthread_getspecific(exit_key) == NULL);
    CHECK(cleanup_order == 0 || cleanup_order == 21);
    destructor_calls++;
    if ((unsigned long)value > 1)
        CHECK(pthread_setspecific(exit_key, (void *)((unsigned long)value - 1)) == 0);
}
static void repeating_destructor(void *value) {
    CHECK(pthread_getspecific(repeating_key) == NULL);
    repeating_calls++;
    CHECK(pthread_setspecific(repeating_key, value) == 0);
}
static void *exit_worker(void *explicit_exit) {
    CHECK(pthread_setspecific(exit_key, (void *)3) == 0);
    CHECK(pthread_setspecific(repeating_key, (void *)1) == 0);
    if (explicit_exit) {
        pthread_cleanup_push(exit_cleanup, (void *)1);
        pthread_cleanup_push(exit_cleanup, (void *)2);
        pthread_exit((void *)42);
        pthread_cleanup_pop(0);
        pthread_cleanup_pop(0);
        CHECK(0); /* pthread_exit must not return. */
    }
    return (void *)42;
}
static void thread_lifecycle_smoke(void) {
    CHECK(pthread_key_create(&exit_key, exit_destructor) == 0);
    CHECK(pthread_key_create(&repeating_key, repeating_destructor) == 0);
    for (unsigned explicit_exit = 0; explicit_exit < 2; explicit_exit++) {
        destructor_calls = repeating_calls = cleanup_order = 0;
        pthread_t worker;
        int created = pthread_create(&worker, NULL, exit_worker, (void *)explicit_exit);
        CHECK(created == 0);
        if (!created) {
            void *result = NULL;
            CHECK(pthread_join(worker, &result) == 0);
            CHECK(result == (void *)42);
            CHECK(destructor_calls == 3 && repeating_calls == 4);
            CHECK(cleanup_order == (explicit_exit ? 21u : 0u));
            CHECK(pthread_getspecific(exit_key) == NULL);
        }
    }
    CHECK(pthread_key_delete(exit_key) == 0);
    CHECK(pthread_key_delete(repeating_key) == 0);
}
static void *join_primary(void *unused) {
    (void)unused;
    void *result = NULL;
    CHECK(pthread_join(primary_thread, &result) == 0);
    CHECK(result == (void *)73);
    printf("S5LBOX-HLE-PRIMARY-EXIT %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    if (failures) exit(1);
    return NULL; /* Last worker returning must exit the process successfully. */
}

static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static int worker_finished;

static void *hold_wait_mutex(void *arg) {
    CHECK(pthread_mutex_lock(&wait_mutex) == 0);
    if (arg) CHECK(pthread_cond_signal(&wait_cond) == 0);
    usleep(80000); /* Keep the mutex past the waiter's 20 ms deadline. */
    worker_finished = 1;
    CHECK(pthread_mutex_unlock(&wait_mutex) == 0);
    return NULL;
}

static void condition_smoke(void) {
    struct timespec zero = {0, 0}, bad = {0, 1000000000}, negative = {-1, 0};
    CHECK(pthread_mutex_lock(&wait_mutex) == 0);
    CHECK(pthread_cond_timedwait_relative_np(&wait_cond, &wait_mutex, &bad) == EINVAL);
    CHECK(pthread_cond_timedwait(&wait_cond, &wait_mutex, &negative) == EINVAL);
    CHECK(pthread_mutex_trylock(&wait_mutex) == EBUSY);
    CHECK(pthread_cond_timedwait_relative_np(&wait_cond, &wait_mutex, &zero) == ETIMEDOUT);
    CHECK(pthread_cond_timedwait(&wait_cond, &wait_mutex, &zero) == ETIMEDOUT);
    /* A sole timed waiter must wake instead of being diagnosed as deadlocked. */
    struct timespec short_wait = {0, 10000000};
    CHECK(pthread_cond_timedwait_relative_np(&wait_cond, &wait_mutex, &short_wait) == ETIMEDOUT);
    CHECK(pthread_mutex_trylock(&wait_mutex) == EBUSY);
    CHECK(pthread_mutex_unlock(&wait_mutex) == 0);

    for (unsigned absolute = 0; absolute < 2; absolute++) {
        for (unsigned signalled = 0; signalled < 2; signalled++) {
            pthread_t worker;
            CHECK(pthread_mutex_lock(&wait_mutex) == 0);
            worker_finished = 0;
            int created = pthread_create(&worker, NULL, hold_wait_mutex, (void *)signalled);
            CHECK(created == 0);
            if (!created) {
                struct timespec timeout = {0, 20000000};
                if (absolute) {
                    struct timeval now;
                    CHECK(gettimeofday(&now, NULL) == 0);
                    timeout.tv_sec = now.tv_sec;
                    timeout.tv_nsec += now.tv_usec * 1000;
                    if (timeout.tv_nsec >= 1000000000) {
                        timeout.tv_sec++;
                        timeout.tv_nsec -= 1000000000;
                    }
                }
                int result = absolute ? pthread_cond_timedwait(&wait_cond, &wait_mutex, &timeout)
                    : pthread_cond_timedwait_relative_np(&wait_cond, &wait_mutex, &timeout);
                CHECK(result == (signalled ? 0 : ETIMEDOUT));
                CHECK(worker_finished == 1);
                CHECK(pthread_mutex_trylock(&wait_mutex) == EBUSY);
            }
            CHECK(pthread_mutex_unlock(&wait_mutex) == 0);
            if (!created) CHECK(pthread_join(worker, NULL) == 0);
        }
    }
    CHECK(pthread_cond_destroy(&wait_cond) == 0);
    CHECK(pthread_mutex_destroy(&wait_mutex) == 0);
}

unsigned s5lbox_dynamic_value = 73;
unsigned s5lbox_dynamic_sum(unsigned a, unsigned b) { return a + b; }
static __attribute__((used,noinline)) unsigned s5lbox_dynamic_private(unsigned value) { return value + 1; }

static void *dynamic_worker(void *arg) {
    (void)arg;
    CHECK(dlerror() == NULL);
    CHECK(dlsym(RTLD_DEFAULT, "s5lbox_missing_worker") == NULL);
    const char *error = dlerror();
    CHECK(error && strstr(error, "s5lbox_missing_worker"));
    CHECK(dlerror() == NULL);
    return NULL;
}

static void dynamic_smoke(void) {
    CHECK(dlerror() == NULL);
    void *all = dlopen(NULL, RTLD_LAZY);
    CHECK(all != NULL);
    unsigned (*arm)(unsigned,unsigned) = (unsigned (*)(unsigned,unsigned))dlsym(all,"s5lbox_dynamic_sum");
    unsigned (*thumb)(unsigned,unsigned) = (unsigned (*)(unsigned,unsigned))dlsym(all,"thumb_sum");
    CHECK(arm && arm(19,23) == 42);
    CHECK(thumb && thumb(20,22) == 42);
    unsigned *value = (unsigned *)dlsym(all,"s5lbox_dynamic_value");
    CHECK(value == &s5lbox_dynamic_value && *value == 73);
    CHECK(dlsym(all,"s5lbox_dynamic_private") == NULL);
    const char *error = dlerror();
    CHECK(error && strstr(error,"s5lbox_dynamic_private"));
    CHECK(dlerror() == NULL);
    CHECK(dlsym(all,"s5lbox_missing_main") == NULL);
    pthread_t worker;
    int created = pthread_create(&worker,NULL,dynamic_worker,NULL);
    CHECK(created == 0);
    if (!created) CHECK(pthread_join(worker,NULL) == 0);
    error = dlerror();
    CHECK(error && strstr(error,"s5lbox_missing_main"));
    CHECK(dlerror() == NULL);
    CHECK(dlopen("/missing-fixture.dylib",RTLD_LAZY) == NULL);
    error = dlerror();
    CHECK(error && strstr(error,"missing-fixture"));
    char *library = strdup("/usr/lib/libSystem.B.dylib");
    void *system = dlopen(library,RTLD_LAZY);
    free(library); /* The opaque handle must not alias the caller's path. */
    void *system2 = dlopen("/usr/lib/libSystem.B.dylib",RTLD_LAZY);
    CHECK(system && system == system2);
    size_t (*length)(const char *) = (size_t (*)(const char *))dlsym(system,"strlen");
    CHECK(length && length("fixture") == 7);
    CHECK(dlclose(system) == 0);
    CHECK(dlsym(system2,"strlen") != NULL);
    CHECK(dlclose(system2) == 0);
    CHECK(dlsym(system2,"strlen") == NULL);
    CHECK(dlerror() != NULL && dlerror() == NULL);
    CHECK(dlclose(NULL) == -1);
    CHECK(dlerror() != NULL && dlerror() == NULL);
    CHECK(dlclose(all) == 0);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--format-overflow-smoke") == 0) {
        char small[4];
        int (*volatile checked)(char *,int,size_t,const char *,...) = __sprintf_chk;
        checked(small,0,sizeof small,"%s","too long");
        return 99; /* Must fail before writing beyond the destination. */
    }
    if (argc > 1 && strcmp(argv[1], "--thread-lifecycle-smoke") == 0) {
        thread_lifecycle_smoke();
        printf("S5LBOX-HLE-THREAD-LIFECYCLE %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "--primary-exit-smoke") == 0) {
        pthread_t worker;
        primary_thread = pthread_self();
        if (pthread_create(&worker, NULL, join_primary, NULL)) return 99;
        pthread_exit((void *)73);
        return 98;
    }
    if (argc > 1 && strcmp(argv[1], "--dynamic-smoke") == 0) {
        dynamic_smoke();
        printf("S5LBOX-HLE-DYNAMIC %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "--condition-smoke") == 0) {
        condition_smoke();
        printf("S5LBOX-HLE-CONDITION %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    if (argc > 1 && strcmp(argv[1], "--spin") == 0) {
        for (;;) __asm__ volatile("nop");
    }
    if (argc > 1 && strcmp(argv[1], "--sleep") == 0) {
        for (;;) sleep(60);
    }
    if (argc > 1 && strcmp(argv[1], "--exit") == 0) exit(23);
    if (argc > 1 && strcmp(argv[1], "--thread-exit") == 0) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, thread_exit_app, (void *)29)) return 99;
        pthread_join(thread, NULL);
        return 98; /* exit() in the worker must stop the entire guest app. */
    }
    if (argc > 1 && strcmp(argv[1], "--parked-exit") == 0) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, thread_park, NULL)) return 99;
        usleep(1000);
        exit(31); /* Host cleanup must unwind the sleeping worker. */
    }
    if (argc > 1 && strcmp(argv[1], "--fault") == 0) {
        __asm__ volatile(".long 0xe7f000f0"); /* ARM undefined instruction. */
        return 97;
    }
    CHECK(thumb_sum(19, 23) == 42);
    rand48_smoke();
    int (*volatile bounded_format)(char *,size_t,int,size_t,const char *,...) = __snprintf_chk;
    int (*volatile checked_format)(char *,int,size_t,const char *,...) = __sprintf_chk;
    char formatted[16]; memset(formatted,'!',sizeof formatted);
    CHECK(bounded_format(formatted,5,0,sizeof formatted,"%s-%d","abcdef",42)==9);
    CHECK(!strcmp(formatted,"abcd") && formatted[5]=='!');
    CHECK(bounded_format(NULL,0,0,0,"%s-%d","test",7)==6);
    CHECK(checked_format(formatted,0,sizeof formatted,"%.1f/%d",1.5,7)==5);
    CHECK(!strcmp(formatted,"1.5/7"));
    CHECK(checked_vformat(formatted,sizeof formatted,1,"%d %.1f",42,2.5)==6);
    CHECK(!strcmp(formatted,"42 2.5"));
    CHECK(checked_vformat(formatted,sizeof formatted,0,"%s","abc")==3);
    // Call through pointers to exercise the guest ABI rather than compiler
    // builtins folding the constants (notably double-valued nan()).
    int (*volatile classify_double)(double) = __fpclassify;
    int (*volatile classify_float)(float) = __fpclassifyf;
    double (*volatile make_nan)(const char *) = nan;
    double (*volatile stable_hypot)(double,double) = hypot;
    CHECK(classify_double(0.0) == FP_ZERO && classify_double(-0.0) == FP_ZERO);
    CHECK(classify_double(1.25) == FP_NORMAL);
    CHECK(classify_double(0x1p-1074) == FP_SUBNORMAL);
    CHECK(classify_double(INFINITY) == FP_INFINITE);
    CHECK(classify_double(make_nan("123")) == FP_NAN);
    CHECK(classify_float(0x1p-149f) == FP_SUBNORMAL);
    CHECK(fabs(stable_hypot(3e200,4e200) / 1e200 - 5.0) < 1e-12);
    CHECK(fabs(stable_hypot(3e-200,4e-200) / 1e-200 - 5.0) < 1e-12);
    volatile float a = 1.5f, b = 2.0f;
    CHECK(a * b + 0.25f == 3.25f);
    int values[] = {8, 2, 9, 1, 4};
    qsort(values, 5, sizeof values[0], compare);
    CHECK(values[0] == 1 && values[4] == 9);

    pthread_t thread;
    int created = pthread_create(&thread, NULL, thread_main, (void *)41);
    CHECK(created == 0);
    if (!created) {
        void *result = NULL;
        CHECK(pthread_join(thread, &result) == 0);
        CHECK((unsigned long)result == 42);
    }

    size_t querySize=0;
    CHECK(sysctlbyname("hw.pagesize",NULL,&querySize,NULL,0)==0 && querySize==8);
    int pageSize=0; querySize=sizeof pageSize;
    CHECK(sysctlbyname("hw.pagesize",&pageSize,&querySize,NULL,0)==0);
    CHECK(pageSize==4096 && querySize==4);
    uint64_t wideValue=0; querySize=sizeof wideValue;
    CHECK(sysctlbyname("hw.memsize",&wideValue,&querySize,NULL,0)==0);
    CHECK(wideValue==121634816 && querySize==8);
    int mib[]={CTL_HW,HW_PAGESIZE}; pageSize=0; querySize=sizeof pageSize;
    CHECK(sysctl(mib,2,&pageSize,&querySize,NULL,0)==0 && pageSize==4096 && querySize==4);
    mib[1]=HW_CPU_FREQ; querySize=sizeof pageSize;
    CHECK(sysctl(mib,2,&pageSize,&querySize,NULL,0)==0 && pageSize==412000000 && querySize==4);
    querySize=sizeof wideValue;
    CHECK(sysctlbyname("hw.cpufrequency",&wideValue,&querySize,NULL,0)==0 && querySize==8);
    CHECK(wideValue==412000000);
    querySize=sizeof pageSize;
    CHECK(sysctlbyname("kern.osrevision",&pageSize,&querySize,NULL,0)==0 && querySize==4);
    CHECK(pageSize==199506);
    char queryBuffer[16]; memset(queryBuffer,0x7a,sizeof queryBuffer); querySize=2;
    errno=0; CHECK(sysctlbyname("kern.ostype",queryBuffer,&querySize,NULL,0)==-1 && errno==ENOMEM);
    CHECK(querySize==0 && queryBuffer[0]==0x7a && queryBuffer[2]==0x7a);
    querySize=sizeof queryBuffer;
    CHECK(sysctlbyname("kern.ostype",queryBuffer,&querySize,NULL,0)==0);
    CHECK(!strcmp(queryBuffer,"Darwin") && querySize==7);
    errno=0; CHECK(sysctlbyname("unavailable.fixture",NULL,&querySize,NULL,0)==-1 && errno==ENOENT);
    mib[1]=99999; errno=0;
    CHECK(sysctl(mib,2,NULL,&querySize,NULL,0)==-1 && errno==ENOENT);
    errno=0; CHECK(sysctl(mib,0,NULL,&querySize,NULL,0)==-1 && errno==EINVAL);
    errno=0; CHECK(sysctl(NULL,2,NULL,&querySize,NULL,0)==-1 && errno==EFAULT);
    errno=0; CHECK(sysctlbyname(NULL,NULL,&querySize,NULL,0)==-1 && errno==EFAULT);
    errno=0; CHECK(sysctlbyname("hw.pagesize",&pageSize,NULL,NULL,0)==-1 && errno==EFAULT);
    errno=0; CHECK(sysctlbyname("hw.pagesize",NULL,NULL,&pageSize,sizeof pageSize)==-1 && errno==EPERM);
    CHECK(sysctlbyname("hw.pagesize",NULL,NULL,NULL,0)==0);

    const char *home = getenv("HOME");
    CHECK(home != NULL);
    if (home) {
        struct statfs volume;
        struct smoke_statvfs portable;
        CHECK(sizeof(volume)==2168 && sizeof(portable)==44);
        CHECK(statfs(home,&volume)==0 && volume.f_bsize>0);
        CHECK(statfs("/",&volume)==0);
        CHECK(statfs(".",&volume)==0);
        CHECK(statvfs(home,&portable)==0);
        CHECK(portable.frsize==volume.f_bsize && portable.namemax==255);
        CHECK(portable.flag==((volume.f_flags&1)|((volume.f_flags&8)?2:0)));
        errno=0; CHECK(statfs("/missing-statfs-file",&volume)==-1 && errno==ENOENT);
        errno=0; CHECK(statfs("",&volume)==-1 && errno==ENOENT);
        errno=0; CHECK(statfs(NULL,&volume)==-1 && errno==EFAULT);
        errno=0; CHECK(statfs((const char *)0x60000000,&volume)==-1 && errno==EFAULT);
        errno=0; CHECK(statfs(home,NULL)==-1 && errno==EFAULT);
        errno=0; CHECK(statvfs(home,NULL)==-1 && errno==EFAULT);
        errno=0; CHECK(fstatfs(-1,&volume)==-1 && errno==EBADF);
        errno=0; CHECK(fstatvfs(-1,&portable)==-1 && errno==EBADF);
        char longpath[1025]; memset(longpath,'x',1024); longpath[1024]=0;
        errno=0; CHECK(statfs(longpath,&volume)==-1 && errno==ENAMETOOLONG);
        char path[1024], contents[32] = {0};
        int length = snprintf(path, sizeof path, "%s/Documents/interpreter-smoke.txt", home);
        CHECK(length > 0 && (unsigned)length < sizeof path);
        if (length > 0 && (unsigned)length < sizeof path) {
            FILE *file = fopen(path, "w+");
            CHECK(file != NULL);
            if (file) {
                CHECK(statfs(path,&volume)==0);
                CHECK(fstatfs(fileno(file),&volume)==0);
                CHECK(fstatvfs(fileno(file),&portable)==0);
                char child[1100]; snprintf(child,sizeof child,"%s/child",path);
                errno=0; CHECK(statfs(child,&volume)==-1 && errno==ENOTDIR);
                CHECK(fputs("interpreter smoke", file) >= 0);
                CHECK(fflush(file) == 0);
                CHECK(fseek(file, 0, SEEK_SET) == 0);
                CHECK(fgets(contents, sizeof contents, file) != NULL);
                CHECK(strcmp(contents, "interpreter smoke") == 0);
                /* Full elements must be counted even when supplied by ungetc. */
                CHECK(ungetc('b', file) == 'b');
                CHECK(ungetc('a', file) == 'a');
                CHECK(fread(contents, 2, 1, file) == 1);
                CHECK(contents[0] == 'a' && contents[1] == 'b');
                /* Only actual file bytes need a destination, even when an app
                   supplies a larger maximum count than its resource buffer. */
                void *resource = malloc(512 * 1024);
                CHECK(resource != NULL);
                if (resource) {
                    CHECK(fseek(file, 0, SEEK_SET) == 0);
                    CHECK(fread(resource, 1, 1024 * 1024, file) == 17);
                    CHECK(memcmp(resource, "interpreter smoke", 17) == 0);
                    CHECK(feof(file) != 0);
                    CHECK(ftell(file) == 17);
                    free(resource);
                }
                errno=0; CHECK(read(-1, contents, 1)==-1 && errno==EBADF);
                errno=0;
                CHECK(pread(fileno(file), (void *)0x60000000, 1, 0)==-1 && errno==EFAULT);
                CHECK(ftell(file)==17);
                CHECK(pread(fileno(file), contents, 4, 0)==4);
                CHECK(memcmp(contents,"inte",4)==0 && ftell(file)==17);
                errno=0; CHECK(write(-1, contents, 1)==-1 && errno==EBADF);
                errno=0;
                CHECK(pwrite(fileno(file), (void *)0x60000000, 1, 0)==-1 && errno==EFAULT);
                CHECK(ftell(file)==17);
                CHECK(fclose(file) == 0);
                CHECK(remove(path) == 0);
            }
        }
    }
    printf("S5LBOX-HLE-SMOKE %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
