/*
 * S5LBox — core/src/boot/bringup.c against the real firmware.
 *
 * WHAT THIS SUITE IS FOR. Every address bring-up computes is one the guest
 * cannot survive being wrong about, and none of them is checkable by reading
 * the code: they fall out of the kernel's own segment table, the tree's own
 * length, and four alignment rules. So this test does not assert that the
 * arithmetic is self-consistent. It asserts that it produces THE NUMBERS
 * run89-base printed, transcribed here from docs/BOOTLOG.md:
 *
 *   virt base 0xc0000000, phys base 0x08000000, RAM 128 MB
 *   entry     : vm 0xc0069040 -> pa 0x08069040
 *   devicetree: -> pa 0x087d1000  (40544 bytes)
 *   boot_args : pa 0x087db000  topOfKernelData vm 0xc0988000
 *   cmdline   : "debug=0x8 serial=1 nand-enable-adm=0 rd=md0"
 *   /vram reg -> {0x0885c000,0x0012c000}
 *   /chosen/memory-map MemoryMapReserved-4 -> RAMDisk {0xe0000000,0x1bd33000}
 *
 * TWO INDEPENDENT CHECKS, deliberately not written by this file:
 *
 *   tools/dt_inplace.h  reads the patched tree back. It is the shipping
 *                       device-tree implementation the desktop harness uses
 *                       and core/tests/test_devicetree.c pins, so a patch that
 *                       both bringup.c and this test got wrong the same way
 *                       still fails.
 *   ios3_kernel_patch_apply()  re-parses the kernel file, re-derives every
 *                       segment's physical placement, and compares guest RAM
 *                       against it (LOADED_SEGMENT_MISMATCH). It is the gate
 *                       bring-up calls anyway, so a mis-load is caught by a
 *                       component that had no part in doing the loading.
 *
 * PUBLIC CI CANNOT HOLD APPLE'S FIRMWARE. The refusal cases below need none
 * and always run; the firmware-backed cases print a SKIP verdict and pass when
 * firmware/ is absent. That is a deliberate asymmetry: the cases that protect
 * users from a silently-wrong boot are the fail-closed ones, and those are the
 * ones that run everywhere.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "bringup.h"
#include "ios3_bringup_gate.h"

/* The shipping in-place device-tree readers, used here ONLY to read back. */
#include "dt_inplace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks = 0;
static unsigned failures = 0;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define CHECK_U32(got, want, what)                                            \
    do {                                                                      \
        uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);                 \
        CHECK(g_ == w_, "%s: got 0x%08x want 0x%08x", (what), g_, w_);        \
    } while (0)

/* ------------------------------------------------------------------ files - */

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n ? (size_t)n : 1u);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    *out_len = (size_t)n;
    return buf;
}

static char *firmware_path(const char *name) {
    static char path[1024];
    int n = snprintf(path, sizeof path, "%s/%s", S5LBOX_FIRMWARE_DIR, name);
    if (n < 0 || (size_t)n >= sizeof path) return NULL;
    return path;
}

/* -------------------------------------------------------------- root media - */
/*
 * A synthetic root medium: correctly sized, backed by nothing.
 *
 * Bring-up reads no byte of the root filesystem -- it publishes the medium's
 * SIZE in /chosen/memory-map and hands the descriptor to the bridges, which do
 * not touch it until the guest asks. So a zero-filled block of exactly the
 * right length reproduces run89-base's RAMDisk entry without a 445 MB file,
 * and without opening firmware/ for writing, which must never happen.
 */
static vm_block_io_status_t sparse_read(void *ctx, uint64_t offset, void *dst,
                                        size_t requested, size_t *actual) {
    (void)ctx; (void)offset;
    memset(dst, 0, requested);
    *actual = requested;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t sparse_write(void *ctx, uint64_t offset,
                                         const void *src, size_t requested,
                                         size_t *actual) {
    (void)ctx; (void)offset; (void)src; (void)requested;
    *actual = 0;
    return VM_BLOCK_IO_ERROR;   /* never reached during bring-up */
}

static vm_block_io_status_t sparse_flush(void *ctx) {
    (void)ctx;
    return VM_BLOCK_IO_OK;
}

static vm_block_t sparse_media(uint64_t size) {
    vm_block_t block;
    memset(&block, 0, sizeof block);
    block.size = size;
    block.read_at = sparse_read;
    block.write_at = sparse_write;
    block.flush = sparse_flush;
    return block;
}

/* The size run89-base's work image had: firmware/rootfs.img (433,274,880)
 * grown by the harness's default 32 MB and page-rounded. */
#define RUN89_WORK_IMAGE_SIZE  UINT64_C(0x1bd33000)

/* ------------------------------------------------------- expected geometry - */

#define WANT_ENTRY_VA        UINT32_C(0xc0069040)
#define WANT_ENTRY_PA        UINT32_C(0x08069040)
#define WANT_DT_PA           UINT32_C(0x087d1000)
#define WANT_DT_SIZE         UINT32_C(40544)
#define WANT_BOOT_ARGS_PA    UINT32_C(0x087db000)
#define WANT_FB_PA           UINT32_C(0x0885c000)
/*
 * DERIVED from S5L_BRINGUP_VRAM_SURFACES, not written out, because these three
 * numbers move together whenever the pool is resized and a hand-copied set
 * fails as six separate mysteries. The relationship is what is worth pinning:
 * the pool is exactly N framebuffers, and topOfKernelData sits directly above
 * it, 16 KiB aligned. The pool's SIZE is a deliberate choice justified in
 * bringup.h; that it is a whole number of surfaces and that nothing overlaps
 * it are properties this test owns.
 */
#define WANT_VRAM_BYTES      ((uint32_t)S5L_BRINGUP_VRAM_BYTES)
#define WANT_TOKD_PA         ((WANT_FB_PA + WANT_VRAM_BYTES + 0x3fffu) & ~0x3fffu)
#define WANT_TOKD_VA         (WANT_TOKD_PA - UINT32_C(0x08000000) + \
                              UINT32_C(0xc0000000))
#define WANT_BOUNCE_PA       UINT32_C(0x087dc000)
#define WANT_CMDLINE  "debug=0x8 serial=1 nand-enable-adm=0 rd=md0"

/* -------------------------------------------------------------------------- */

static bool build_machine(s5l8900_t *m) {
    return s5l8900_init(m, S5L_BRINGUP_PHYS_BASE, S5L_BRINGUP_RAM_SIZE);
}

/*
 * The full run89-base bring-up, and every number it produced.
 */
static void test_run89_layout(const uint8_t *kernel, size_t kernel_len,
                              const uint8_t *tree, size_t tree_len) {
    s5l8900_t machine;
    s5l_bringup_request_t request;
    s5l_bringup_result_t result;
    ios3_bringup_gate_report_t gate;
    s5l_bringup_md_t *md;
    vm_block_t media = sparse_media(RUN89_WORK_IMAGE_SIZE);

    printf("run89-base layout, with a root filesystem\n");

    md = (s5l_bringup_md_t *)calloc(1, sizeof *md);
    CHECK(md != NULL, "could not allocate bridge storage");
    if (!md) return;
    memset(&gate, 0, sizeof gate);

    CHECK(build_machine(&machine), "s5l8900_init failed");

    memset(&request, 0, sizeof request);
    request.kernel = kernel;
    request.kernel_size = kernel_len;
    request.devicetree = tree;
    request.devicetree_size = tree_len;
    request.root_media = &media;
    ios3_bringup_gate_configure(&request, &gate);

    s5l_bringup_status_t status = s5l_bringup(&machine, &request, md, &result);
    CHECK(status == S5L_BRINGUP_OK, "bring-up refused: %s at %s (%s)",
          s5l_bringup_status_name(status),
          s5l_bringup_stage_name(result.stage), result.detail);
    if (status != S5L_BRINGUP_OK) {
        free(md);
        s5l8900_free(&machine);
        return;
    }

    /* The mapping itself. */
    CHECK_U32(result.virt_base, 0xc0000000u, "virt base");
    CHECK_U32(result.phys_base, 0x08000000u, "phys base");
    CHECK_U32(result.ram_size, 128u * 1024u * 1024u, "RAM size");

    /* Entry, and where every segment landed. */
    CHECK_U32(result.entry_va, WANT_ENTRY_VA, "entry vm");
    CHECK_U32(result.entry_pa, WANT_ENTRY_PA, "entry pa");
    CHECK(result.segments_loaded >= 2u,
          "expected at least __TEXT and __DATA to be loaded, got %u",
          result.segments_loaded);
    /* The kernel's physical span has to end just under the tree, or the tree
     * is sitting on top of the kernel's BSS. */
    CHECK(result.kernel_end_pa <= WANT_DT_PA &&
          result.kernel_end_pa > WANT_DT_PA - 0x1000u,
          "kernel ends at 0x%08x; the device tree is placed at 0x%08x",
          result.kernel_end_pa, WANT_DT_PA);

    /* Boot-owned memory, in the order it is laid down. */
    CHECK_U32(result.devicetree_pa, WANT_DT_PA, "device tree pa");
    CHECK_U32(result.devicetree_size, WANT_DT_SIZE, "device tree size");
    CHECK_U32(result.devicetree_va, 0xc07d1000u, "device tree vm");
    CHECK_U32(result.boot_args_pa, WANT_BOOT_ARGS_PA, "boot_args pa");
    CHECK_U32(result.raw_bounce_pa, WANT_BOUNCE_PA, "raw bounce reserve pa");
    CHECK_U32(result.framebuffer_pa, WANT_FB_PA, "framebuffer pa");
    CHECK_U32(result.vram_bytes, WANT_VRAM_BYTES, "/vram pool bytes");
    CHECK_U32(result.top_of_kernel_data_pa, WANT_TOKD_PA, "topOfKernelData pa");
    CHECK_U32(result.top_of_kernel_data_va, WANT_TOKD_VA, "topOfKernelData vm");

    /* The 16 KiB rule that cost a prefetch abort to find. */
    CHECK((result.top_of_kernel_data_pa & 0x3fffu) == 0u,
          "topOfKernelData 0x%08x is not 16 KiB aligned",
          result.top_of_kernel_data_pa);
    /* And the bootstrap headroom above it. */
    CHECK(result.free_pool_bytes >= 0x11000u,
          "free page pool is only 0x%x bytes", result.free_pool_bytes);

    /*
     * A WHOLE number of framebuffers, and at least the two that made run59
     * composite at all. The exact count is bringup.h's call -- it went to four
     * on 2026-07-30 when r190 showed a two-surface pool starving the first UI
     * animation of a layer -- but a pool that is not a multiple of the surface
     * size means IOSurface's last allocation runs off the end, and that is this
     * test's business rather than bringup.h's.
     */
    CHECK_U32(result.vram_bytes,
              (uint32_t)S5L_BRINGUP_VRAM_SURFACES * 320u * 480u * 4u,
              "/vram must hold a whole number of surfaces");
    CHECK(S5L_BRINGUP_VRAM_SURFACES >= 2u,
          "/vram holds %u surface(s); one is what made run76 composite nothing",
          (unsigned)S5L_BRINGUP_VRAM_SURFACES);

    CHECK(strcmp(result.cmdline, WANT_CMDLINE) == 0,
          "cmdline: got \"%s\" want \"%s\"", result.cmdline, WANT_CMDLINE);

    CHECK(result.md_bridge_installed, "the md bridge was not installed");
    CHECK(gate.ran && gate.status == IOS3_KERNEL_PATCH_STATUS_OK,
          "the kernel gate did not accept the real kernel: %s",
          ios3_kernel_patch_status_string(gate.status));

    /* --- the CPU is actually pointed at the kernel ------------------------ */
    CHECK_U32(machine.cpu.r[15], WANT_ENTRY_PA, "PC at entry");
    CHECK_U32(machine.cpu.r[0], WANT_BOOT_ARGS_PA, "r0 = boot_args");

    /* --- boot_args, read back out of guest DRAM -------------------------- */
    {
        const uint8_t *ba = machine.ram + (WANT_BOOT_ARGS_PA - 0x08000000u);
        uint32_t (*ld)(const uint8_t *) = dtn_ld32;   /* same LE decode */
        CHECK(ba[0] == 1 && ba[1] == 0, "boot_args Revision");
        CHECK(ba[2] == 6 && ba[3] == 0,
              "boot_args Version must be 6 (MEASURED), got %u", ba[2]);
        CHECK_U32(ld(ba + 0x04), 0xc0000000u, "boot_args virtBase");
        CHECK_U32(ld(ba + 0x08), 0x08000000u, "boot_args physBase");
        CHECK_U32(ld(ba + 0x0c), 0x08000000u, "boot_args memSize");
        CHECK_U32(ld(ba + 0x10), WANT_TOKD_PA,
                  "boot_args topOfKernelData (PHYSICAL)");
        CHECK_U32(ld(ba + 0x14), WANT_FB_PA, "boot_args v_baseAddr");
        CHECK_U32(ld(ba + 0x18), 0u,
                  "boot_args v_display must be 0 or the kernel draws nothing");
        CHECK_U32(ld(ba + 0x1c), 320u * 4u, "boot_args v_rowBytes");
        CHECK_U32(ld(ba + 0x20), 320u, "boot_args v_width");
        CHECK_U32(ld(ba + 0x24), 480u, "boot_args v_height");
        CHECK_U32(ld(ba + 0x28), 32u, "boot_args v_depth");
        CHECK_U32(ld(ba + 0x30), 0xc07d1000u,
                  "boot_args deviceTreeP (VIRTUAL)");
        CHECK_U32(ld(ba + 0x34), WANT_DT_SIZE, "boot_args deviceTreeLength");
        CHECK(memcmp(ba + 0x38, WANT_CMDLINE, sizeof WANT_CMDLINE) == 0,
              "boot_args CommandLine does not match");
    }

    /* --- the patched device tree, read back through dt_inplace.h ---------- */
    {
        uint8_t *dt = machine.ram + (WANT_DT_PA - 0x08000000u);
        size_t   n  = tree_len;
        uint32_t vl;
        const uint8_t *v;
        size_t node;

        struct { const char *path, *prop; uint32_t want; } expect[] = {
            { "",          "clock-frequency",      103000000u },
            { "cpus/cpu0", "timebase-frequency",     6000000u },
            { "cpus/cpu0", "clock-frequency",      412000000u },
            { "cpus/cpu0", "bus-frequency",        103000000u },
            { "cpus/cpu0", "memory-frequency",     103000000u },
            { "cpus/cpu0", "peripheral-frequency",  51500000u },
            { "cpus/cpu0", "fixed-frequency",       24000000u },
            { "arm-io/spi0/lcd0", "lcd-panel-id",  0x00a5c22bu },
        };
        for (size_t i = 0; i < sizeof expect / sizeof expect[0]; i++) {
            node = dtn_path(dt, n, expect[i].path);
            CHECK(node != DT_NONE, "/%s not found", expect[i].path);
            if (node == DT_NONE) continue;
            vl = 0;
            v = dtn_prop(dt, n, node, expect[i].prop, &vl);
            CHECK(v != NULL && vl == 4u, "/%s:%s missing or not 4 bytes",
                  expect[i].path, expect[i].prop);
            if (!v || vl != 4u) continue;
            CHECK(dtn_ld32(v) == expect[i].want,
                  "/%s:%s got 0x%08x want 0x%08x", expect[i].path,
                  expect[i].prop, dtn_ld32(v), expect[i].want);
        }

        /* /memory reg = the DRAM bank; /vram reg = the whole IOSurface pool. */
        struct { const char *path; uint32_t base, size; } regs[] = {
            { "memory", 0x08000000u, 0x08000000u },
            { "vram",   WANT_FB_PA,  WANT_VRAM_BYTES },
        };
        for (size_t i = 0; i < sizeof regs / sizeof regs[0]; i++) {
            node = dtn_path(dt, n, regs[i].path);
            CHECK(node != DT_NONE, "/%s not found", regs[i].path);
            if (node == DT_NONE) continue;
            vl = 0;
            v = dtn_prop(dt, n, node, "reg", &vl);
            CHECK(v != NULL && vl == 8u, "/%s:reg missing or not 8 bytes",
                  regs[i].path);
            if (!v || vl != 8u) continue;
            CHECK(dtn_ld32(v) == regs[i].base && dtn_ld32(v + 4) == regs[i].size,
                  "/%s:reg got {0x%08x,0x%08x} want {0x%08x,0x%08x}",
                  regs[i].path, dtn_ld32(v), dtn_ld32(v + 4),
                  regs[i].base, regs[i].size);
        }

        /* The RAMDisk entry: a claimed MemoryMapReserved-* placeholder naming
         * the synthetic token base and the medium's exact size. */
        node = dtn_path(dt, n, "chosen/memory-map");
        CHECK(node != DT_NONE, "/chosen/memory-map not found");
        if (node != DT_NONE) {
            vl = 0;
            v = dtn_prop(dt, n, node, "RAMDisk", &vl);
            CHECK(v != NULL && vl == 8u, "RAMDisk entry was not published");
            if (v && vl == 8u) {
                CHECK(dtn_ld32(v) == 0xe0000000u &&
                      dtn_ld32(v + 4) == (uint32_t)RUN89_WORK_IMAGE_SIZE,
                      "RAMDisk got {0x%08x,0x%08x} want {0xe0000000,0x%08x}",
                      dtn_ld32(v), dtn_ld32(v + 4),
                      (uint32_t)RUN89_WORK_IMAGE_SIZE);
            }
            /* Claiming a placeholder must not have consumed the live
             * DeviceTree entry, whose {0, 0x9e60} -- 0x9e60 being exactly the
             * 40544-byte length of devicetree.bin -- is what corroborates the
             * whole {address, size} reading of the format. */
            vl = 0;
            v = dtn_prop(dt, n, node, "DeviceTree", &vl);
            CHECK(v != NULL && vl == 8u && dtn_ld32(v + 4) == 0x9e60u,
                  "the live DeviceTree entry was disturbed");

            /*
             * WHICH placeholder was claimed. run89-base's log names
             * MemoryMapReserved-4, so the four before it are already spoken
             * for and this pins the selection rule -- first free slot in
             * property order -- rather than merely "some slot".
             */
            CHECK(dtn_prop(dt, n, node, "MemoryMapReserved-4", &vl) == NULL,
                  "MemoryMapReserved-4 should have been renamed to RAMDisk");
            CHECK(dtn_prop(dt, n, node, "MemoryMapReserved-5", &vl) != NULL,
                  "MemoryMapReserved-5 was consumed as well; only one slot "
                  "should have been claimed");
            CHECK(dtn_prop(dt, n, node, "MemoryMapReserved-3", &vl) != NULL,
                  "MemoryMapReserved-3 disappeared");
        }
    }

    /* --- the display controller was seeded -------------------------------- */
    {
        uint32_t fb = 0, w = 0, h = 0, stride = 0, format = 0, order = 0;
        bool on = s5l_clcd_window(&machine.clcd, 0, &fb, &w, &h, &stride,
                                  &format, &order);
        CHECK(on, "CLCD window 0 was not enabled");
        if (on) {
            CHECK_U32(fb, WANT_FB_PA, "CLCD scanout base");
            CHECK_U32(w, 320u, "CLCD width");
            CHECK_U32(h, 480u, "CLCD height");
            CHECK_U32(stride, 320u * 4u, "CLCD stride");
        }
    }

    /* --- the caller's device tree buffer was NOT modified ----------------- */
    CHECK(dtn_ld32(tree + 0) != 0u || dtn_ld32(tree + 4) != 0u,
          "the source tree looks empty; the read-back above proves nothing");
    {
        size_t node = dtn_path((uint8_t *)(uintptr_t)tree, tree_len,
                               "cpus/cpu0");
        uint32_t vl = 0;
        const uint8_t *v = node == DT_NONE ? NULL
            : dtn_prop((uint8_t *)(uintptr_t)tree, tree_len, node,
                       "clock-frequency", &vl);
        CHECK(v != NULL && vl == 4u && dtn_ld32(v) == 0u,
              "bring-up modified the caller's device-tree buffer; it must "
              "patch only the copy in guest DRAM");
    }

    free(md);
    s5l8900_free(&machine);
}

/*
 * The same firmware without a root filesystem. Everything below the bounce
 * reservation is unchanged, which is what proves the layout is driven by the
 * inputs rather than by a table of constants; topOfKernelData moves down
 * because the bounce slots are gone.
 */
static void test_no_root(const uint8_t *kernel, size_t kernel_len,
                         const uint8_t *tree, size_t tree_len) {
    s5l8900_t machine;
    s5l_bringup_request_t request;
    s5l_bringup_result_t result;

    printf("bring-up with no root filesystem\n");
    CHECK(build_machine(&machine), "s5l8900_init failed");

    memset(&request, 0, sizeof request);
    request.kernel = kernel;
    request.kernel_size = kernel_len;
    request.devicetree = tree;
    request.devicetree_size = tree_len;

    s5l_bringup_status_t status =
        s5l_bringup(&machine, &request, NULL, &result);
    CHECK(status == S5L_BRINGUP_OK, "refused: %s (%s)",
          s5l_bringup_status_name(status), result.detail);
    if (status == S5L_BRINGUP_OK) {
        CHECK_U32(result.entry_pa, WANT_ENTRY_PA, "entry pa");
        CHECK_U32(result.devicetree_pa, WANT_DT_PA, "device tree pa");
        CHECK_U32(result.boot_args_pa, WANT_BOOT_ARGS_PA, "boot_args pa");
        CHECK(!result.md_bridge_installed,
              "no root medium was supplied, yet a bridge was installed");
        CHECK_U32(result.raw_bounce_pa, 0u, "no bounce reserve without a root");
        /* No rd= token when there is no root device to name. */
        CHECK(strcmp(result.cmdline, "debug=0x8 serial=1 nand-enable-adm=0") == 0,
              "cmdline: got \"%s\"", result.cmdline);
        CHECK(result.framebuffer_enabled, "the framebuffer should be on");
    }
    s5l8900_free(&machine);

    /*
     * "rd=" is a substring of "board=", so a caller-supplied command line that
     * merely CONTAINS those three characters must still get its rd=md0. The
     * opposite mistake -- appending a second root device -- would have the
     * guest mount something nobody chose.
     */
    {
        static const struct { const char *given, *want; } lines[] = {
            { "board=1",            "board=1 rd=md0" },
            { "rd=md0 debug=0x8",   "rd=md0 debug=0x8" },
            { "debug=0x8 rd=md0",   "debug=0x8 rd=md0" },
            { "",                   "rd=md0" },
        };
        for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++) {
            s5l_bringup_md_t *md = (s5l_bringup_md_t *)calloc(1, sizeof *md);
            vm_block_t media = sparse_media(RUN89_WORK_IMAGE_SIZE);
            s5l_bringup_request_t r;
            s5l_bringup_result_t out;
            if (!md) { CHECK(false, "allocation"); break; }
            CHECK(build_machine(&machine), "s5l8900_init failed");
            memset(&r, 0, sizeof r);
            r.kernel = kernel;      r.kernel_size = kernel_len;
            r.devicetree = tree;    r.devicetree_size = tree_len;
            r.root_media = &media;  r.cmdline = lines[i].given;
            ios3_bringup_gate_configure(&r, NULL);
            if (s5l_bringup(&machine, &r, md, &out) == S5L_BRINGUP_OK)
                CHECK(strcmp(out.cmdline, lines[i].want) == 0,
                      "cmdline \"%s\" became \"%s\", want \"%s\"",
                      lines[i].given, out.cmdline, lines[i].want);
            else
                CHECK(false, "bring-up refused cmdline \"%s\": %s",
                      lines[i].given, out.detail);
            s5l8900_free(&machine);
            free(md);
        }
    }
}

/* -------------------------------------------------------- refusal cases --- */
/*
 * These need no firmware and are the ones that matter most: each is a way the
 * app could otherwise report a boot that did not happen.
 */
static void test_refusals(const uint8_t *kernel, size_t kernel_len,
                          const uint8_t *tree, size_t tree_len) {
    s5l8900_t machine;
    s5l_bringup_request_t request;
    s5l_bringup_result_t result;
    s5l_bringup_status_t status;
    bool have_firmware = kernel != NULL && tree != NULL;

    printf("refusals\n");

    /* A missing kernel. */
    CHECK(build_machine(&machine), "s5l8900_init failed");
    memset(&request, 0, sizeof request);
    request.devicetree = tree;
    request.devicetree_size = tree_len;
    status = s5l_bringup(&machine, &request, NULL, &result);
    CHECK(status == S5L_BRINGUP_KERNEL_MISSING,
          "a missing kernel gave %s", s5l_bringup_status_name(status));
    CHECK(result.detail[0] != '\0', "a refusal must carry a reason");
    s5l8900_free(&machine);

    /* A missing device tree. */
    CHECK(build_machine(&machine), "s5l8900_init failed");
    memset(&request, 0, sizeof request);
    request.kernel = kernel ? kernel : (const uint8_t *)"x";
    request.kernel_size = kernel ? kernel_len : 1u;
    status = s5l_bringup(&machine, &request, NULL, &result);
    CHECK(status == S5L_BRINGUP_DEVICETREE_MISSING,
          "a missing device tree gave %s", s5l_bringup_status_name(status));
    s5l8900_free(&machine);

    /* Bytes that are not a Mach-O at all. */
    {
        static const uint8_t junk[512] = { 0 };
        CHECK(build_machine(&machine), "s5l8900_init failed");
        memset(&request, 0, sizeof request);
        request.kernel = junk;
        request.kernel_size = sizeof junk;
        request.devicetree = tree ? tree : junk;
        request.devicetree_size = tree ? tree_len : sizeof junk;
        status = s5l_bringup(&machine, &request, NULL, &result);
        CHECK(status == S5L_BRINGUP_KERNEL_MALFORMED,
              "512 zero bytes as a kernel gave %s",
              s5l_bringup_status_name(status));
        CHECK(result.stage == S5L_BRINGUP_STAGE_KERNEL_PARSE,
              "wrong stage reported: %s",
              s5l_bringup_stage_name(result.stage));
        s5l8900_free(&machine);
    }

    /* A root filesystem with no gate: the SVC sites would never be installed,
     * so the guest would read whatever is at 0xe0000000, which is nothing. */
    if (have_firmware) {
        vm_block_t media = sparse_media(RUN89_WORK_IMAGE_SIZE);
        s5l_bringup_md_t *md = (s5l_bringup_md_t *)calloc(1, sizeof *md);
        CHECK(md != NULL, "allocation");
        if (md) {
            CHECK(build_machine(&machine), "s5l8900_init failed");
            memset(&request, 0, sizeof request);
            request.kernel = kernel;
            request.kernel_size = kernel_len;
            request.devicetree = tree;
            request.devicetree_size = tree_len;
            request.root_media = &media;
            status = s5l_bringup(&machine, &request, md, &result);
            CHECK(status == S5L_BRINGUP_ROOT_GATE_MISSING,
                  "a root filesystem without a kernel gate gave %s",
                  s5l_bringup_status_name(status));
            s5l8900_free(&machine);

            /* A root filesystem with a gate but no storage for the bridges. */
            CHECK(build_machine(&machine), "s5l8900_init failed");
            ios3_bringup_gate_configure(&request, NULL);
            status = s5l_bringup(&machine, &request, NULL, &result);
            CHECK(status == S5L_BRINGUP_ROOT_STORAGE_MISSING,
                  "a root filesystem without bridge storage gave %s",
                  s5l_bringup_status_name(status));
            s5l8900_free(&machine);

            /* A medium whose size is not a page multiple. mdevadd takes page
             * NUMBERS, so this would be a silently truncated disk. */
            CHECK(build_machine(&machine), "s5l8900_init failed");
            media = sparse_media(RUN89_WORK_IMAGE_SIZE + 1u);
            status = s5l_bringup(&machine, &request, md, &result);
            CHECK(status == S5L_BRINGUP_ROOT_MEDIA_INVALID,
                  "an unaligned medium gave %s",
                  s5l_bringup_status_name(status));
            s5l8900_free(&machine);

            /* A medium too large for the token window. */
            CHECK(build_machine(&machine), "s5l8900_init failed");
            media = sparse_media(S5L_BRINGUP_MD_MAX_SIZE + 0x1000u);
            status = s5l_bringup(&machine, &request, md, &result);
            CHECK(status == S5L_BRINGUP_ROOT_MEDIA_INVALID,
                  "an oversized medium gave %s",
                  s5l_bringup_status_name(status));
            s5l8900_free(&machine);
            free(md);
        }
    }

    /* A truncated kernel: the load commands still parse for a while, so this
     * has to be caught by a bounds check rather than by the magic number. */
    if (have_firmware) {
        CHECK(build_machine(&machine), "s5l8900_init failed");
        memset(&request, 0, sizeof request);
        request.kernel = kernel;
        request.kernel_size = kernel_len / 2u;   /* half an image */
        request.devicetree = tree;
        request.devicetree_size = tree_len;
        status = s5l_bringup(&machine, &request, NULL, &result);
        CHECK(status != S5L_BRINGUP_OK,
              "a kernel truncated to half its length was ACCEPTED");
        CHECK(result.detail[0] != '\0', "a refusal must carry a reason");
        printf("  (truncated kernel refused: %s -- %s)\n",
               s5l_bringup_status_name(status), result.detail);
        s5l8900_free(&machine);
    }

    /* A truncated device tree. The header parses; the walk runs off the end. */
    if (have_firmware) {
        CHECK(build_machine(&machine), "s5l8900_init failed");
        memset(&request, 0, sizeof request);
        request.kernel = kernel;
        request.kernel_size = kernel_len;
        request.devicetree = tree;
        request.devicetree_size = tree_len / 2u;
        status = s5l_bringup(&machine, &request, NULL, &result);
        CHECK(status == S5L_BRINGUP_DEVICETREE_MALFORMED ||
              status == S5L_BRINGUP_DEVICETREE_PATCH_FAILED,
              "a half device tree gave %s", s5l_bringup_status_name(status));
        printf("  (truncated device tree refused: %s -- %s)\n",
               s5l_bringup_status_name(status), result.detail);
        s5l8900_free(&machine);
    }

    /* The device tree offered as the kernel, and vice versa -- the "user put
     * the wrong file in" case, which the app must not boot. */
    if (have_firmware) {
        CHECK(build_machine(&machine), "s5l8900_init failed");
        memset(&request, 0, sizeof request);
        request.kernel = tree;
        request.kernel_size = tree_len;
        request.devicetree = kernel;
        request.devicetree_size = kernel_len;
        status = s5l_bringup(&machine, &request, NULL, &result);
        CHECK(status == S5L_BRINGUP_KERNEL_MALFORMED,
              "the device tree passed as a kernel gave %s",
              s5l_bringup_status_name(status));
        s5l8900_free(&machine);
    }

    /* A machine built with the wrong DRAM geometry. */
    {
        s5l8900_t small;
        if (s5l8900_init(&small, S5L_BRINGUP_PHYS_BASE, 64u * 1024u * 1024u)) {
            memset(&request, 0, sizeof request);
            request.kernel = kernel ? kernel : (const uint8_t *)"x";
            request.kernel_size = kernel ? kernel_len : 1u;
            request.devicetree = tree ? tree : (const uint8_t *)"x";
            request.devicetree_size = tree ? tree_len : 1u;
            status = s5l_bringup(&small, &request, NULL, &result);
            CHECK(status == S5L_BRINGUP_INVALID_ARGUMENT,
                  "a 64 MB machine gave %s", s5l_bringup_status_name(status));
            s5l8900_free(&small);
        }
    }

    /* Every named status and stage must have a name. A new enumerator with no
     * string is a reason that reaches the user as "unknown". */
    for (int i = 0; i <= (int)S5L_BRINGUP_ROOT_STORAGE_MISSING; i++)
        CHECK(strcmp(s5l_bringup_status_name((s5l_bringup_status_t)i),
                     "unknown") != 0,
              "status %d has no name", i);
    for (int i = 0; i <= (int)S5L_BRINGUP_STAGE_ENTRY; i++)
        CHECK(strcmp(s5l_bringup_stage_name((s5l_bringup_stage_t)i),
                     "unknown") != 0,
              "stage %d has no name", i);
}

/*
 * How far the brought-up machine actually gets.
 *
 * The DEFAULT bound is small and deliberately so: at the interpreter's ~1.4 M
 * instructions/second on this host, a boot deep enough to say anything costs
 * twenty seconds, and the whole suite is eighteen. What the default proves is
 * narrow and still worth proving -- that the entry point is real code and the
 * first stretch of it executes without the interpreter refusing -- and it is
 * reported as a number rather than as a verdict.
 *
 * WHAT THE LONGER RUN MEASURED, on this host, at the commit that added this
 * file. S5LBOX_BRINGUP_STEPS=300000000, against firmware/kernel.macho and
 * firmware/devicetree.bin, with a zero-filled synthetic root medium:
 *
 *   executed 300,000,000 instructions from 0x08069040; stopped at pc
 *   0xc0779fac with status ARM_OK -- i.e. it ran out of budget, it did not
 *   fault -- and 4,094 bytes of guest console, beginning:
 *
 *     iBoot version:
 *     Seatbelt MACF policy initialized
 *     BSD root: md0, major 2, minor 0
 *     AppleS5L8900XIO::start: chip-revision: EVT0
 *     AppleBaseband::start(0xc07b0a00): baseband
 *     AppleARMPL192VIC::start: _v...
 *
 * That third line is the one worth reading twice: IOFindBSDRoot resolved the
 * RAMDisk entry this file's bring-up wrote into /chosen/memory-map, mdevadd
 * accepted it, and the kernel named md0 as its root device. The memory-disk
 * path is therefore working end to end, not merely configured.
 *
 * It is NOT a claim that the system booted. The medium here is zero-filled, so
 * there is no HFS volume behind md0 to mount; how far a real work image gets
 * is a different measurement and this file does not make it.
 */
static void test_executes(const uint8_t *kernel, size_t kernel_len,
                          const uint8_t *tree, size_t tree_len) {
    s5l8900_t machine;
    s5l_bringup_request_t request;
    s5l_bringup_result_t result;
    ios3_bringup_gate_report_t gate;
    s5l_bringup_md_t *md;
    vm_block_t media = sparse_media(RUN89_WORK_IMAGE_SIZE);

    printf("execution\n");
    md = (s5l_bringup_md_t *)calloc(1, sizeof *md);
    CHECK(md != NULL, "allocation");
    if (!md) return;
    memset(&gate, 0, sizeof gate);
    CHECK(build_machine(&machine), "s5l8900_init failed");

    memset(&request, 0, sizeof request);
    request.kernel = kernel;
    request.kernel_size = kernel_len;
    request.devicetree = tree;
    request.devicetree_size = tree_len;
    request.root_media = &media;
    ios3_bringup_gate_configure(&request, &gate);

    if (s5l_bringup(&machine, &request, md, &result) != S5L_BRINGUP_OK) {
        free(md);
        s5l8900_free(&machine);
        return;
    }

    /* Small by default so the suite stays fast; S5LBOX_BRINGUP_STEPS pushes it
     * further when someone wants the measurement rather than the smoke test. */
    uint64_t budget = 200000;
    const char *env = getenv("S5LBOX_BRINGUP_STEPS");
    if (env && *env) {
        char *end = NULL;
        unsigned long long requested = strtoull(env, &end, 0);
        if (end && *end == '\0' && requested > 0ull) budget = requested;
    }
    /*
     * s5l8900_run(), NOT a bare arm_step() loop. It steps the CPU and then
     * calls s5l8900_tick(), which is what advances the timers and delivers
     * interrupts -- and an XNU that never receives a timer interrupt makes
     * some progress and then waits forever for time to pass. An earlier
     * version of this measurement stepped the CPU alone, ran 300 million
     * instructions, and reported zero bytes of guest console, which is what
     * that mistake looks like from the outside.
     */
    uint64_t retired = 0;
    arm_status_t st = ARM_OK;
    while (retired < budget && st == ARM_OK) {
        uint64_t remaining = budget - retired;
        unsigned chunk = remaining > 1000000u ? 1000000u : (unsigned)remaining;
        retired += s5l8900_run(&machine, chunk, &st);
    }
    printf("  executed %llu instruction(s) from 0x%08x; stopped at pc 0x%08x "
           "with status %d\n",
           (unsigned long long)retired, WANT_ENTRY_PA, machine.cpu.r[15],
           (int)st);

    /*
     * What the guest actually SAID, which is the only evidence here that means
     * anything about a boot. An instruction count says the interpreter did not
     * refuse; a kprintf banner says XNU reached its own console.
     *
     * uart0 is the kprintf console, and its capture is a first-N cap rather
     * than a ring, so this is the HEAD of the log -- the earliest output, which
     * is exactly the part that says how the boot started.
     */
    printf("  guest console: %zu byte(s) on uart0\n", machine.uart0.tx_len);
    if (machine.uart0.tx_len) {
        size_t show = machine.uart0.tx_len < 200u ? machine.uart0.tx_len : 200u;
        printf("  first %zu: \"", show);
        for (size_t i = 0; i < show; i++) {
            unsigned char c = (unsigned char)machine.uart0.tx[i];
            if (c == '\n') printf("\\n");
            else if (c >= 0x20 && c < 0x7f) putchar((int)c);
            else printf("\\x%02x", c);
        }
        printf("\"\n");
    }

    /* The narrow claim: the first instruction at the entry point is decodable
     * and the interpreter did not refuse it immediately. Anything beyond that
     * is reported above, not asserted here. */
    CHECK(retired > 0u,
          "the kernel's entry point did not execute a single instruction "
          "(status %d)", (int)st);

    free(md);
    s5l8900_free(&machine);
}

/*
 * UN-MATCHING, verified by diffing the published tree against the same
 * bring-up without it.
 *
 * Counting struck nodes would pass just as happily if the code overwrote the
 * wrong byte, moved a node, or rebuilt the blob a little shorter. What must be
 * true is much narrower: the tree is the same size, and EXACTLY one byte per
 * named node is different, and each of those became 'x'. Nothing is deleted,
 * no phandle moves, and everything that indexes the tree by offset still
 * finds what it found before.
 */
static void test_unmatch(const uint8_t *kernel, size_t kernel_len,
                         const uint8_t *tree, size_t tree_len) {
    static const char *const PATHS[] = { "arm-io/mbx", "arm-io/sha1" };
    const unsigned want = (unsigned)(sizeof PATHS / sizeof PATHS[0]);
    s5l8900_t machine;
    s5l_bringup_request_t request;
    s5l_bringup_result_t result;
    uint8_t *baseline = NULL;
    uint32_t published = 0u;

    printf("un-matching device-tree nodes\n");

    CHECK(build_machine(&machine), "s5l8900_init failed");
    memset(&request, 0, sizeof request);
    request.kernel = kernel;
    request.kernel_size = kernel_len;
    request.devicetree = tree;
    request.devicetree_size = tree_len;
    CHECK(s5l_bringup(&machine, &request, NULL, &result) == S5L_BRINGUP_OK,
          "the baseline bring-up was refused: %s", result.detail);
    CHECK_U32(result.devicetree_unmatched, 0u, "struck with nothing asked");
    published = result.devicetree_size;
    baseline = (uint8_t *)malloc(published ? published : 1u);
    CHECK(baseline != NULL, "out of memory");
    if (baseline)
        memcpy(baseline, machine.ram + (result.devicetree_pa - 0x08000000u),
               published);
    s5l8900_free(&machine);

    CHECK(build_machine(&machine), "s5l8900_init failed");
    memset(&request, 0, sizeof request);
    request.kernel = kernel;
    request.kernel_size = kernel_len;
    request.devicetree = tree;
    request.devicetree_size = tree_len;
    request.unmatch = PATHS;
    request.unmatch_count = want;
    CHECK(s5l_bringup(&machine, &request, NULL, &result) == S5L_BRINGUP_OK,
          "un-matching was refused: %s", result.detail);
    CHECK_U32(result.devicetree_unmatched, want, "nodes struck");
    CHECK_U32(result.devicetree_size, published, "the tree changed size");

    if (baseline && result.devicetree_size == published) {
        const uint8_t *now = machine.ram + (result.devicetree_pa - 0x08000000u);
        unsigned differs = 0u;
        for (uint32_t i = 0; i < published; i++) {
            if (baseline[i] == now[i]) continue;
            differs++;
            CHECK(now[i] == (uint8_t)'x',
                  "byte %u became 0x%02x, not 'x'", i, now[i]);
            CHECK(baseline[i] != (uint8_t)'x',
                  "byte %u was already struck", i);
        }
        CHECK(differs == want, "%u bytes differ, expected %u", differs, want);
    }
    s5l8900_free(&machine);
    free(baseline);

    /*
     * Fails closed on a node that is not there. Skipping it would hand back a
     * machine with the device still present after the caller asked for it to
     * be gone -- and the caller would then debug the wrong machine, which is
     * exactly how /arm-io/mbx cost a boot in the first place.
     */
    {
        static const char *const MISSING[] = { "arm-io/no-such-node" };
        CHECK(build_machine(&machine), "s5l8900_init failed");
        memset(&request, 0, sizeof request);
        request.kernel = kernel;
        request.kernel_size = kernel_len;
        request.devicetree = tree;
        request.devicetree_size = tree_len;
        request.unmatch = MISSING;
        request.unmatch_count = 1u;
        s5l_bringup_status_t st =
            s5l_bringup(&machine, &request, NULL, &result);
        CHECK(st == S5L_BRINGUP_DEVICETREE_PATCH_FAILED,
              "an absent node was accepted: %s",
              s5l_bringup_status_name(st));
        s5l8900_free(&machine);
    }
}

int main(void) {
    size_t kernel_len = 0, tree_len = 0;
    uint8_t *kernel = NULL, *tree = NULL;
    const char *p;

    printf("== bringup ==\n");

    p = firmware_path("kernel.macho");
    if (p) kernel = slurp(p, &kernel_len);
    p = firmware_path("devicetree.bin");
    if (p) tree = slurp(p, &tree_len);

    if (kernel && tree) {
        printf("firmware: kernel %zu bytes, device tree %zu bytes\n",
               kernel_len, tree_len);
        test_run89_layout(kernel, kernel_len, tree, tree_len);
        test_no_root(kernel, kernel_len, tree, tree_len);
        test_unmatch(kernel, kernel_len, tree, tree_len);
        test_executes(kernel, kernel_len, tree, tree_len);
    } else {
        printf("SKIP: no firmware in %s -- the firmware-backed cases need "
               "Apple's kernelcache and device tree, which cannot be "
               "committed. The refusal cases below still run.\n",
               S5LBOX_FIRMWARE_DIR);
    }

    test_refusals(kernel, kernel_len, tree, tree_len);

    free(kernel);
    free(tree);

    printf("== bringup: %u checks, %u failure(s) ==\n", checks, failures);
    return failures ? 1 : 0;
}
