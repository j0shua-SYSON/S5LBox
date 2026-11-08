/* Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * A real ARMv6 Mach-O test app for the interpreter/framework boundary. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <dlfcn.h>

extern int pthread_cond_timedwait_relative_np(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);

extern unsigned thumb_sum(unsigned, unsigned);
static unsigned failures;
#define CHECK(value) do { if (!(value)) { failures++; \
    printf("FAIL line %u: %s\n", (unsigned)__LINE__, #value); } } while (0)

static int compare(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
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

    const char *home = getenv("HOME");
    CHECK(home != NULL);
    if (home) {
        char path[1024], contents[32] = {0};
        int length = snprintf(path, sizeof path, "%s/Documents/interpreter-smoke.txt", home);
        CHECK(length > 0 && (unsigned)length < sizeof path);
        if (length > 0 && (unsigned)length < sizeof path) {
            FILE *file = fopen(path, "w+");
            CHECK(file != NULL);
            if (file) {
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
                CHECK(fclose(file) == 0);
                CHECK(remove(path) == 0);
            }
        }
    }
    printf("S5LBOX-HLE-SMOKE %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
