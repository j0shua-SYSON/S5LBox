/* Exact-XNU32 memory-disk geometry seam. Copyright (c) 2026 j0shua-SYSON. */
#ifndef S5LBOX_MD_GEOMETRY_BRIDGE_H
#define S5LBOX_MD_GEOMETRY_BRIDGE_H

#include "md_bridge.h"

#define MD_GEOMETRY_MAX_SIZE UINT64_C(0x200000000) /* 8 GiB */
#define MD_GEOMETRY_BOOTSTRAP_SIZE UINT32_C(0x80000000)

/* All addresses are supplied by the exact-build gate, never discovered at
 * runtime. These sites replace arithmetic only: native registration, buffer
 * mapping, error/completion paths and ioctl stores remain guest code. */
typedef struct {
    uint32_t register_pc;     /* e5: lsrs r2,r2,#12 before mdevadd */
    uint32_t bounds_pc;       /* e6: after saving 64-bit block offset */
    uint32_t count64_pc;      /* e7: before overflowing page-to-byte shift */
    uint32_t count32_pc;      /* e8: same calculation for legacy ioctl */
    uint32_t device_va;       /* mdev[0], XNU32 layout */
    uint32_t map_pc;          /* in-range request -> native buf_map */
    uint32_t trim_pc;         /* r1-r4 -> native buf_setcount, then map */
    uint32_t eof_pc;          /* native buf_biodone, keep residual */
    uint32_t invalid_pc;      /* native EINVAL and buf_biodone */
    uint32_t count64_done_pc; /* native stores of r0 and zero high word */
    uint32_t count32_done_pc; /* native store of r0 */
} md_geometry_sites_t;

typedef struct {
    md_geometry_sites_t sites;
    const md_bridge_t *strategy;
} md_geometry_bridge_t;

bool md_geometry_bridge_valid(const md_geometry_bridge_t *bridge);

/* Recognized malformed state fails closed without CPU or guest-memory writes.
 * Success changes only the registers consumed by the audited continuation;
 * its incoming flags and LR are dead there. No disk I/O or guest writes. */
arm_svc_result_t md_geometry_bridge_svc(void *context, arm_cpu_t *cpu, uint32_t pc,
                                        uint32_t encoding);

#endif
