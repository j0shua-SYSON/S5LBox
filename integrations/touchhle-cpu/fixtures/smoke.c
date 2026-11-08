/* Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * A real ARMv6 Mach-O test app for the interpreter/framework boundary. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

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

int main(int argc, char **argv) {
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
