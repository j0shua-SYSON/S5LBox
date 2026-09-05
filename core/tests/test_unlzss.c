/* Exercise the extraction command's actual exit status and output boundary. */
#define main unlzss_main
#include "../../tools/unlzss.c"
#undef main

static unsigned failures;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; \
} } while (0)

static void write_be32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}

static bool put_file(const char *path, const uint8_t *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool ok = fwrite(data, 1, size, file) == size;
    return fclose(file) == 0 && ok;
}

static bool matches_file(const char *path, const uint8_t *data, size_t size) {
    uint8_t got[16];
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    size_t n = fread(got, 1, sizeof got, file);
    fclose(file);
    return n == size && memcmp(got, data, size) == 0;
}

int main(void) {
    char input[] = "test-unlzss-input.bin";
    char output[] = "test-unlzss-output.bin";
    char program[] = "unlzss";
    char *args[] = { program, input, output };
    uint8_t blob[LZSS_HEADER_SIZE + 4] = {0};
    memcpy(blob, "complzss", 8);
    write_be32(blob + 8, 0x024d0127u); /* Adler-32 of abc */
    write_be32(blob + 12, 3);
    write_be32(blob + 16, 4);
    memcpy(blob + LZSS_HEADER_SIZE, "\x07" "abc", 4);

    CHECK(put_file(input, blob, sizeof blob));
    CHECK(unlzss_main(3, args) == 0);
    CHECK(matches_file(output, (const uint8_t *)"abc", 3));

    /* Failure must precede opening the destination, including an existing
     * output that might be the user's last valid extraction. */
    const uint8_t saved[] = "keep";
    for (unsigned bad = 0; bad < 3; bad++) {
        write_be32(blob + 8, bad == 1 ? 0x024d0126u : 0x024d0127u);
        write_be32(blob + 12, bad == 0 ? 4 : 3);
        CHECK(put_file(input, blob, bad == 2 ? 0 : sizeof blob));
        CHECK(put_file(output, saved, sizeof saved));
        CHECK(unlzss_main(3, args) != 0);
        CHECK(matches_file(output, saved, sizeof saved));
    }

    write_be32(blob + 8, 0x024d0127u);
    CHECK(put_file(input, blob, sizeof blob));
    char directory[] = ".";
    args[2] = directory;
    CHECK(unlzss_main(3, args) != 0);
    CHECK(remove(input) == 0);
    CHECK(remove(output) == 0);
    return failures ? 1 : 0;
}
