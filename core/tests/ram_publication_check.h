/* Test-only oracle: every changed RAM byte must be announced before writing. */
#ifndef S5LBOX_TEST_RAM_PUBLICATION_CHECK_H
#define S5LBOX_TEST_RAM_PUBLICATION_CHECK_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const uint8_t *ram;
    uint8_t *before, *notified;
    uint32_t base, size;
    bool active, valid;
} ram_publication_check_t;

static void ram_publication_begin(ram_publication_check_t *check,
        const uint8_t *ram, uint32_t base, uint32_t size,
        uint8_t *before, uint8_t *notified) {
    *check = (ram_publication_check_t){ram, before, notified, base, size, true, true};
    memcpy(before, ram, size);
    memset(notified, 0, size);
}

static void ram_publication_notice(void *context, uint32_t pa, uint32_t length) {
    ram_publication_check_t *check = context;
    /* Some gate-only tests invoke the handler without beginning an oracle. */
    if (!check->active) return;
    if (!length || pa < check->base ||
        (uint64_t)pa - check->base + length > check->size) {
        check->valid = false;
        return;
    }
    uint32_t offset = pa - check->base;
    for (uint32_t i = offset; i < offset + length; i++) {
        if (!check->notified[i] && check->ram[i] != check->before[i])
            check->valid = false; /* A first notification after the write. */
        check->notified[i] = 1u;
    }
}

static bool ram_publication_end(ram_publication_check_t *check) {
    for (uint32_t i = 0u; i < check->size; i++)
        if (!check->notified[i] && check->ram[i] != check->before[i])
            check->valid = false;
    check->active = false;
    return check->valid;
}
#endif
