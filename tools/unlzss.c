/*
 * S5LBox — unlzss: expand an Apple "complzss" kernelcache to a raw Mach-O.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "lzss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <complzss.bin> <out.macho>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    long n = ftell(f);
    if (n < (long)LZSS_HEADER_SIZE || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "input is too short or unreadable\n");
        fclose(f);
        return 1;
    }
    uint8_t *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "read failed\n"); free(buf); fclose(f); return 1;
    }
    fclose(f);

    lzss_header_t h;
    if (!lzss_parse_header(buf, (size_t)n, &h) || !h.uncompressed_size) {
        fprintf(stderr, "not a nonempty complzss blob\n"); free(buf); return 2;
    }
    printf("uncompressed : %u bytes\ncompressed   : %u bytes\nadler32      : 0x%08x\n",
           h.uncompressed_size, h.compressed_size, h.adler32);

    uint8_t *out = malloc(h.uncompressed_size);
    if (!out) { fprintf(stderr, "oom\n"); free(buf); return 1; }
    size_t got = lzss_decompress(out, h.uncompressed_size,
                                 buf + LZSS_HEADER_SIZE, h.compressed_size);
    free(buf);
    printf("produced     : %zu bytes %s\n", got,
           got == h.uncompressed_size ? "(matches header)" : "(SHORT!)");

    uint32_t a = lzss_adler32(out, got);
    printf("adler32 check: 0x%08x %s\n", a, a == h.adler32 ? "MATCHES" : "MISMATCH");

    /* Incomplete IMG3 CBC decryption caused the historical short output.
     * Do not manufacture an addressable Mach-O by padding damaged input. */
    if (got != h.uncompressed_size || a != h.adler32) {
        fprintf(stderr, "invalid expansion; destination was not opened\n");
        free(out);
        return 2;
    }

    printf("first 16 bytes: ");
    for (int i = 0; i < 16 && (size_t)i < got; i++) printf("%02x ", out[i]);
    printf("\n");

    FILE *o = fopen(argv[2], "wb");
    if (!o) { perror("open output"); free(out); return 1; }
    bool ok = fwrite(out, 1, got, o) == got;
    if (fclose(o) != 0) ok = false;
    free(out);
    if (!ok) { fprintf(stderr, "write failed\n"); return 1; }
    printf("wrote        : %s\n", argv[2]);
    return 0;
}
