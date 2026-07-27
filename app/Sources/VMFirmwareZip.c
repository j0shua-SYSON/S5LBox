/*
 * S5LBox — reading an IPSW, which is a ZIP.
 *
 * Three decisions in here are not obvious and all three were forced by the real
 * 7E18 archive rather than by the specification:
 *
 * 1. Sizes come from the CENTRAL DIRECTORY, never from the local header. Every
 *    one of the 49 members in that IPSW sets flag bit 3, which means Apple's
 *    writer streamed them and left the local header's CRC and both sizes as
 *    zero. A reader that trusted the local header would extract nothing and
 *    report success while doing it.
 *
 * 2. The local header is still read, but only for its name and extra lengths.
 *    Those two fields may legitimately differ from the central directory's, so
 *    the payload offset cannot be computed from the central copy.
 *
 * 3. The CRC-32 is recomputed over everything produced. It is the only
 *    end-to-end evidence that what we hand to the IMG3 parser is what Apple
 *    shipped, and the pass it costs is one we are already making.
 *
 * As everywhere on this path: 64-bit arithmetic, every offset checked against
 * the real file size before the read, and an impossible field refused rather
 * than clamped. The archive came off the internet.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMFirmwareFormats.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* Little-endian scalar reads                                                */
/* ------------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

#define SIG_LOCAL      0x04034b50u   /* PK\3\4 */
#define SIG_CENTRAL    0x02014b50u   /* PK\1\2 */
#define SIG_EOCD       0x06054b50u   /* PK\5\6 */
#define SIG_EOCD64     0x06064b50u   /* PK\6\6 */
#define SIG_EOCD64_LOC 0x07064b50u   /* PK\6\7 */

#define EOCD_SIZE          22u
#define EOCD64_LOC_SIZE    20u
#define EOCD64_MIN_SIZE    56u
#define CENTRAL_MIN_SIZE   46u
#define LOCAL_MIN_SIZE     30u

/* A ZIP comment is a 16-bit length, so the record starts at most this far back. */
#define EOCD_MAX_SCAN (EOCD_SIZE + 0xffffu)

/* The sentinel a 32-bit field carries when the real value lives in a ZIP64
 * extra field. Treating it as a literal size is how a 4 GB archive turns into a
 * 4 GB read of nothing. */
#define ZIP64_SENTINEL32 0xffffffffu
#define ZIP64_SENTINEL16 0xffffu

/* ------------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3, reflected)                                            */
/* ------------------------------------------------------------------------ */
/* Const, so this is shareable across threads; the app may import while the
 * emulator thread is running. */
static const uint32_t k_crc32_table[256] = {
    0x00000000u,0x77073096u,0xee0e612cu,0x990951bau,0x076dc419u,0x706af48fu,
    0xe963a535u,0x9e6495a3u,0x0edb8832u,0x79dcb8a4u,0xe0d5e91eu,0x97d2d988u,
    0x09b64c2bu,0x7eb17cbdu,0xe7b82d07u,0x90bf1d91u,0x1db71064u,0x6ab020f2u,
    0xf3b97148u,0x84be41deu,0x1adad47du,0x6ddde4ebu,0xf4d4b551u,0x83d385c7u,
    0x136c9856u,0x646ba8c0u,0xfd62f97au,0x8a65c9ecu,0x14015c4fu,0x63066cd9u,
    0xfa0f3d63u,0x8d080df5u,0x3b6e20c8u,0x4c69105eu,0xd56041e4u,0xa2677172u,
    0x3c03e4d1u,0x4b04d447u,0xd20d85fdu,0xa50ab56bu,0x35b5a8fau,0x42b2986cu,
    0xdbbbc9d6u,0xacbcf940u,0x32d86ce3u,0x45df5c75u,0xdcd60dcfu,0xabd13d59u,
    0x26d930acu,0x51de003au,0xc8d75180u,0xbfd06116u,0x21b4f4b5u,0x56b3c423u,
    0xcfba9599u,0xb8bda50fu,0x2802b89eu,0x5f058808u,0xc60cd9b2u,0xb10be924u,
    0x2f6f7c87u,0x58684c11u,0xc1611dabu,0xb6662d3du,0x76dc4190u,0x01db7106u,
    0x98d220bcu,0xefd5102au,0x71b18589u,0x06b6b51fu,0x9fbfe4a5u,0xe8b8d433u,
    0x7807c9a2u,0x0f00f934u,0x9609a88eu,0xe10e9818u,0x7f6a0dbbu,0x086d3d2du,
    0x91646c97u,0xe6635c01u,0x6b6b51f4u,0x1c6c6162u,0x856530d8u,0xf262004eu,
    0x6c0695edu,0x1b01a57bu,0x8208f4c1u,0xf50fc457u,0x65b0d9c6u,0x12b7e950u,
    0x8bbeb8eau,0xfcb9887cu,0x62dd1ddfu,0x15da2d49u,0x8cd37cf3u,0xfbd44c65u,
    0x4db26158u,0x3ab551ceu,0xa3bc0074u,0xd4bb30e2u,0x4adfa541u,0x3dd895d7u,
    0xa4d1c46du,0xd3d6f4fbu,0x4369e96au,0x346ed9fcu,0xad678846u,0xda60b8d0u,
    0x44042d73u,0x33031de5u,0xaa0a4c5fu,0xdd0d7cc9u,0x5005713cu,0x270241aau,
    0xbe0b1010u,0xc90c2086u,0x5768b525u,0x206f85b3u,0xb966d409u,0xce61e49fu,
    0x5edef90eu,0x29d9c998u,0xb0d09822u,0xc7d7a8b4u,0x59b33d17u,0x2eb40d81u,
    0xb7bd5c3bu,0xc0ba6cadu,0xedb88320u,0x9abfb3b6u,0x03b6e20cu,0x74b1d29au,
    0xead54739u,0x9dd277afu,0x04db2615u,0x73dc1683u,0xe3630b12u,0x94643b84u,
    0x0d6d6a3eu,0x7a6a5aa8u,0xe40ecf0bu,0x9309ff9du,0x0a00ae27u,0x7d079eb1u,
    0xf00f9344u,0x8708a3d2u,0x1e01f268u,0x6906c2feu,0xf762575du,0x806567cbu,
    0x196c3671u,0x6e6b06e7u,0xfed41b76u,0x89d32be0u,0x10da7a5au,0x67dd4accu,
    0xf9b9df6fu,0x8ebeeff9u,0x17b7be43u,0x60b08ed5u,0xd6d6a3e8u,0xa1d1937eu,
    0x38d8c2c4u,0x4fdff252u,0xd1bb67f1u,0xa6bc5767u,0x3fb506ddu,0x48b2364bu,
    0xd80d2bdau,0xaf0a1b4cu,0x36034af6u,0x41047a60u,0xdf60efc3u,0xa867df55u,
    0x316e8eefu,0x4669be79u,0xcb61b38cu,0xbc66831au,0x256fd2a0u,0x5268e236u,
    0xcc0c7795u,0xbb0b4703u,0x220216b9u,0x5505262fu,0xc5ba3bbeu,0xb2bd0b28u,
    0x2bb45a92u,0x5cb36a04u,0xc2d7ffa7u,0xb5d0cf31u,0x2cd99e8bu,0x5bdeae1du,
    0x9b64c2b0u,0xec63f226u,0x756aa39cu,0x026d930au,0x9c0906a9u,0xeb0e363fu,
    0x72076785u,0x05005713u,0x95bf4a82u,0xe2b87a14u,0x7bb12baeu,0x0cb61b38u,
    0x92d28e9bu,0xe5d5be0du,0x7cdcefb7u,0x0bdbdf21u,0x86d3d2d4u,0xf1d4e242u,
    0x68ddb3f8u,0x1fda836eu,0x81be16cdu,0xf6b9265bu,0x6fb077e1u,0x18b74777u,
    0x88085ae6u,0xff0f6a70u,0x66063bcau,0x11010b5cu,0x8f659effu,0xf862ae69u,
    0x616bffd3u,0x166ccf45u,0xa00ae278u,0xd70dd2eeu,0x4e048354u,0x3903b3c2u,
    0xa7672661u,0xd06016f7u,0x4969474du,0x3e6e77dbu,0xaed16a4au,0xd9d65adcu,
    0x40df0b66u,0x37d83bf0u,0xa9bcae53u,0xdebb9ec5u,0x47b2cf7fu,0x30b5ffe9u,
    0xbdbdf21cu,0xcabac28au,0x53b39330u,0x24b4a3a6u,0xbad03605u,0xcdd70693u,
    0x54de5729u,0x23d967bfu,0xb3667a2eu,0xc4614ab8u,0x5d681b02u,0x2a6f2b94u,
    0xb40bbe37u,0xc30c8ea1u,0x5a05df1bu,0x2d02ef8du
};

uint32_t vmfw_crc32(uint32_t crc, const uint8_t *data, size_t len) {
    if (!data) return crc;
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = k_crc32_table[(crc ^ data[i]) & 0xffu] ^ (crc >> 8);
    return ~crc;
}

const char *vmfw_zip_strerror(vmfw_zip_status_t st) {
    switch (st) {
        case VMFW_ZIP_OK:                    return "ok";
        case VMFW_ZIP_ERR_INVALID_ARGUMENT:  return "invalid zip reader argument";
        case VMFW_ZIP_ERR_READ:              return "the archive could not be read";
        case VMFW_ZIP_ERR_NOT_ZIP:           return "not a zip archive: no end-of-central-directory record";
        case VMFW_ZIP_ERR_MULTI_DISK:        return "split archives are not supported";
        case VMFW_ZIP_ERR_ZIP64:             return "malformed or unaddressable zip64 record";
        case VMFW_ZIP_ERR_BAD_DIRECTORY:     return "a central directory entry is impossible";
        case VMFW_ZIP_ERR_TOO_MANY:          return "the archive declares more members than its directory can hold";
        case VMFW_ZIP_ERR_NAME_TOO_LONG:     return "a member name is longer than this reader accepts";
        case VMFW_ZIP_ERR_ENCRYPTED:         return "the archive uses zip encryption";
        case VMFW_ZIP_ERR_BAD_METHOD:        return "a member uses a compression method other than store or deflate";
        case VMFW_ZIP_ERR_BAD_LOCAL_HEADER:  return "a member's local header is missing or malformed";
        case VMFW_ZIP_ERR_MEMBER_TRUNCATED:  return "a member runs past the end of the archive";
        case VMFW_ZIP_ERR_INFLATE:           return "a member's deflate stream is corrupt";
        case VMFW_ZIP_ERR_CRC_MISMATCH:      return "a member's contents do not match its checksum";
        case VMFW_ZIP_ERR_SINK:              return "the destination refused the extracted bytes";
        default:                             return "unknown zip error";
    }
}

/* Read exactly `len` bytes or fail. The pread contract allows a short read, and
 * a caller that treated one as success would parse whatever was left in the
 * buffer from the previous read. */
static bool read_exact(const vmfw_zip_t *zip, uint64_t off, uint8_t *buf,
                       size_t len) {
    if (len == 0) return true;
    if (off > zip->size || (uint64_t)len > zip->size - off) return false;
    return zip->pread(zip->ctx, off, buf, len) == len;
}

/* ------------------------------------------------------------------------ */
/* Opening                                                                   */
/* ------------------------------------------------------------------------ */

/*
 * The ZIP64 end-of-central-directory locator sits immediately before the
 * classic EOCD. Its presence is what distinguishes "this archive is genuinely
 * 64-bit" from "this archive happens to contain the bytes PK\6\7", so it is
 * located by position rather than by searching for it.
 */
static vmfw_zip_status_t read_zip64(vmfw_zip_t *zip, uint64_t eocd_off) {
    if (eocd_off < EOCD64_LOC_SIZE) return VMFW_ZIP_OK;   /* no room: not zip64 */

    uint8_t loc[EOCD64_LOC_SIZE];
    if (!read_exact(zip, eocd_off - EOCD64_LOC_SIZE, loc, sizeof loc))
        return VMFW_ZIP_ERR_READ;
    if (rd32(loc) != SIG_EOCD64_LOC) return VMFW_ZIP_OK;  /* classic archive */

    if (rd32(loc + 4) != 0) return VMFW_ZIP_ERR_MULTI_DISK;
    if (rd32(loc + 16) > 1)  return VMFW_ZIP_ERR_MULTI_DISK;

    uint64_t rec_off = rd64(loc + 8);
    if (rec_off > zip->size || zip->size - rec_off < EOCD64_MIN_SIZE)
        return VMFW_ZIP_ERR_ZIP64;

    uint8_t rec[EOCD64_MIN_SIZE];
    if (!read_exact(zip, rec_off, rec, sizeof rec)) return VMFW_ZIP_ERR_READ;
    if (rd32(rec) != SIG_EOCD64) return VMFW_ZIP_ERR_ZIP64;

    if (rd32(rec + 16) != 0 || rd32(rec + 20) != 0)
        return VMFW_ZIP_ERR_MULTI_DISK;

    uint64_t total = rd64(rec + 32);
    uint64_t cd_size = rd64(rec + 40);
    uint64_t cd_off  = rd64(rec + 48);

    /* We iterate rather than store, so the only bound that matters is that the
     * count is representable and consistent with the directory's own size. */
    if (total > 0xffffffffu) return VMFW_ZIP_ERR_TOO_MANY;

    zip->entry_count = (uint32_t)total;
    zip->cd_size     = cd_size;
    zip->cd_offset   = cd_off;
    return VMFW_ZIP_OK;
}

vmfw_zip_status_t vmfw_zip_open(vmfw_zip_t *zip, vmfw_pread_fn pread, void *ctx,
                                uint64_t size) {
    if (!zip || !pread) return VMFW_ZIP_ERR_INVALID_ARGUMENT;
    memset(zip, 0, sizeof *zip);
    if (size < EOCD_SIZE) return VMFW_ZIP_ERR_NOT_ZIP;

    zip->pread = pread;
    zip->ctx   = ctx;
    zip->size  = size;

    /*
     * Scan backwards for the EOCD signature. Backwards matters: a member whose
     * own contents happen to contain PK\5\6 is not rare in a 240 MB archive,
     * and the real record is the LAST one. The window is bounded by the largest
     * comment the format can express.
     */
    uint64_t scan_len = size < EOCD_MAX_SCAN ? size : EOCD_MAX_SCAN;
    uint64_t scan_start = size - scan_len;

    /*
     * 64 KB on the stack is more than a phone's secondary thread should spend,
     * so the tail is walked in overlapping windows. The windows run from the
     * END backwards and stop at the first hit, because a 240 MB archive can
     * easily contain the bytes PK\5\6 inside a member and only the last one is
     * the record.
     */
    enum { WINDOW = 4096u, OVERLAP = 3u };   /* a signature spans 4 bytes */
    uint8_t win[WINDOW];
    bool found = false;
    uint64_t eocd_off = 0;

    uint64_t win_end = size;
    for (;;) {
        uint64_t chunk = win_end - scan_start;
        if (chunk > WINDOW) chunk = WINDOW;
        if (chunk < 4) break;                /* no room for a signature */
        uint64_t start = win_end - chunk;

        if (!read_exact(zip, start, win, (size_t)chunk))
            return VMFW_ZIP_ERR_READ;

        /* i is bounded so rd32 stays inside the window that was actually read;
         * the separate `abs` test rejects a signature too close to the end of
         * the FILE for a whole record to follow it. Both are needed: the first
         * bounds the buffer, the second bounds the parse. */
        for (size_t i = (size_t)chunk - 3; i-- > 0; ) {
            uint64_t abs = start + i;
            if (abs + EOCD_SIZE > size) continue;
            if (rd32(win + i) == SIG_EOCD) { eocd_off = abs; found = true; break; }
        }
        if (found || start == scan_start) break;

        /* Overlap by three so a signature straddling the boundary is still
         * seen whole in the next window. */
        win_end = start + OVERLAP;
    }

    if (!found) return VMFW_ZIP_ERR_NOT_ZIP;

    uint8_t eocd[EOCD_SIZE];
    if (!read_exact(zip, eocd_off, eocd, sizeof eocd)) return VMFW_ZIP_ERR_READ;

    if (rd16(eocd + 4) != 0 || rd16(eocd + 6) != 0)
        return VMFW_ZIP_ERR_MULTI_DISK;

    uint16_t here  = rd16(eocd + 8);
    uint16_t total = rd16(eocd + 10);
    if (here != total) return VMFW_ZIP_ERR_MULTI_DISK;

    zip->entry_count = total;
    zip->cd_size     = rd32(eocd + 12);
    zip->cd_offset   = rd32(eocd + 16);

    /* Any sentinel means the true value is in the ZIP64 record. */
    bool wants64 = (total == ZIP64_SENTINEL16)
                || (rd32(eocd + 12) == ZIP64_SENTINEL32)
                || (rd32(eocd + 16) == ZIP64_SENTINEL32);

    vmfw_zip_status_t st = read_zip64(zip, eocd_off);
    if (st != VMFW_ZIP_OK) return st;

    /* If the classic record said "look in the zip64 record" and there was none,
     * nothing we have is usable. Continuing with 0xffffffff as an offset is how
     * a reader ends up seeking to the end of the file and reporting an empty
     * archive. */
    if (wants64 && (zip->cd_offset == ZIP64_SENTINEL32
                 || zip->cd_size == ZIP64_SENTINEL32
                 || zip->entry_count == ZIP64_SENTINEL16))
        return VMFW_ZIP_ERR_ZIP64;

    if (zip->cd_offset > size || zip->cd_size > size - zip->cd_offset)
        return VMFW_ZIP_ERR_BAD_DIRECTORY;

    /* Each entry occupies at least a fixed header, so a directory that claims
     * more entries than it has room for is malformed and would otherwise send
     * the walk off the end of the region. */
    if ((uint64_t)zip->entry_count * CENTRAL_MIN_SIZE > zip->cd_size)
        return VMFW_ZIP_ERR_TOO_MANY;

    return VMFW_ZIP_OK;
}

/* ------------------------------------------------------------------------ */
/* Walking the central directory                                             */
/* ------------------------------------------------------------------------ */

/*
 * Pull the 64-bit values out of a ZIP64 extended-information extra field.
 * The field is positional, not tagged: it carries only those of
 * (uncompressed, compressed, local offset, disk) whose 32-bit slot held the
 * sentinel, in that order. Reading them unconditionally would take the
 * compressed size from whatever followed.
 */
static vmfw_zip_status_t apply_zip64_extra(const vmfw_zip_t *zip,
                                           uint64_t extra_off, size_t extra_len,
                                           vmfw_zip_entry_t *e,
                                           bool need_usize, bool need_csize,
                                           bool need_offset) {
    /* The extra area is up to 64 KB by its own 16-bit length, which is far too
     * much stack for a phone. Only the tag headers are walked, and only the one
     * field we want is ever read -- at most 28 bytes. */
    uint8_t hdr[4];
    uint8_t body[24];
    size_t off = 0;

    while (off + 4 <= extra_len) {
        if (!read_exact(zip, extra_off + off, hdr, sizeof hdr))
            return VMFW_ZIP_ERR_READ;
        uint16_t tag = rd16(hdr);
        uint16_t len = rd16(hdr + 2);
        if ((uint64_t)off + 4 + len > (uint64_t)extra_len)
            return VMFW_ZIP_ERR_BAD_DIRECTORY;

        if (tag == 0x0001u) {
            /* Positional, not tagged: the field carries only those of
             * (uncompressed, compressed, local offset) whose 32-bit slot held
             * the sentinel, in that order. Reading them unconditionally would
             * take the compressed size from whatever followed. */
            unsigned want = (need_usize ? 8u : 0u) + (need_csize ? 8u : 0u)
                          + (need_offset ? 8u : 0u);
            if (len < want || want > sizeof body) return VMFW_ZIP_ERR_ZIP64;
            if (want && !read_exact(zip, extra_off + off + 4, body, want))
                return VMFW_ZIP_ERR_READ;

            const uint8_t *p = body;
            if (need_usize)  { e->uncompressed_size   = rd64(p); p += 8; }
            if (need_csize)  { e->compressed_size     = rd64(p); p += 8; }
            if (need_offset) { e->local_header_offset = rd64(p); }
            return VMFW_ZIP_OK;
        }
        off += (size_t)4 + len;
    }
    /* A sentinel with no zip64 field to explain it is not a small archive; it
     * is a malformed one. */
    return VMFW_ZIP_ERR_ZIP64;
}

vmfw_zip_status_t vmfw_zip_iterate(const vmfw_zip_t *zip,
                                   vmfw_zip_visit_fn visit, void *visit_ctx) {
    if (!zip || !zip->pread || !visit) return VMFW_ZIP_ERR_INVALID_ARGUMENT;

    uint64_t off = zip->cd_offset;
    const uint64_t end = zip->cd_offset + zip->cd_size;

    for (uint32_t i = 0; i < zip->entry_count; i++) {
        uint8_t hdr[CENTRAL_MIN_SIZE];
        if (off + CENTRAL_MIN_SIZE > end) return VMFW_ZIP_ERR_BAD_DIRECTORY;
        if (!read_exact(zip, off, hdr, sizeof hdr)) return VMFW_ZIP_ERR_READ;
        if (rd32(hdr) != SIG_CENTRAL) return VMFW_ZIP_ERR_BAD_DIRECTORY;

        uint16_t flags    = rd16(hdr + 8);
        uint16_t method   = rd16(hdr + 10);
        uint32_t crc      = rd32(hdr + 16);
        uint32_t csize32  = rd32(hdr + 20);
        uint32_t usize32  = rd32(hdr + 24);
        uint16_t name_len = rd16(hdr + 28);
        uint16_t xtra_len = rd16(hdr + 30);
        uint16_t cmnt_len = rd16(hdr + 32);
        uint32_t loff32   = rd32(hdr + 42);

        uint64_t entry_len = (uint64_t)CENTRAL_MIN_SIZE + name_len + xtra_len
                           + cmnt_len;
        if (off + entry_len > end) return VMFW_ZIP_ERR_BAD_DIRECTORY;

        /* Bit 0 is traditional PKWARE encryption, bit 6 is strong encryption.
         * No IPSW uses either; a file that does cannot be extracted and saying
         * so is more useful than producing scrambled bytes. */
        if ((flags & 0x0001u) || (flags & 0x0040u))
            return VMFW_ZIP_ERR_ENCRYPTED;

        if (name_len >= VMFW_ZIP_MAX_NAME) return VMFW_ZIP_ERR_NAME_TOO_LONG;

        vmfw_zip_entry_t e;
        memset(&e, 0, sizeof e);
        e.crc32               = crc;
        e.method              = method;
        e.compressed_size     = csize32;
        e.uncompressed_size   = usize32;
        e.local_header_offset = loff32;

        if (name_len) {
            if (!read_exact(zip, off + CENTRAL_MIN_SIZE, (uint8_t *)e.name,
                            name_len))
                return VMFW_ZIP_ERR_READ;
        }
        e.name[name_len] = '\0';
        /* An embedded NUL would make the C name shorter than the stored one, so
         * two different members could compare equal by name. */
        if (strlen(e.name) != (size_t)name_len)
            return VMFW_ZIP_ERR_BAD_DIRECTORY;

        e.is_directory = (name_len > 0 && e.name[name_len - 1] == '/');

        bool need_usize  = (usize32 == ZIP64_SENTINEL32);
        bool need_csize  = (csize32 == ZIP64_SENTINEL32);
        bool need_offset = (loff32  == ZIP64_SENTINEL32);

        if (need_usize || need_csize || need_offset) {
            vmfw_zip_status_t st =
                apply_zip64_extra(zip, off + CENTRAL_MIN_SIZE + name_len,
                                  xtra_len, &e,
                                  need_usize, need_csize, need_offset);
            if (st != VMFW_ZIP_OK) return st;
        }

        if (e.local_header_offset > zip->size) return VMFW_ZIP_ERR_BAD_DIRECTORY;

        if (!visit(visit_ctx, &e, i)) return VMFW_ZIP_OK;   /* caller is done */

        off += entry_len;
    }

    return VMFW_ZIP_OK;
}

typedef struct {
    const char *want;
    vmfw_zip_entry_t *out;
    bool found;
} find_ctx_t;

static bool find_visit(void *ctx, const vmfw_zip_entry_t *e, uint32_t index) {
    find_ctx_t *f = (find_ctx_t *)ctx;
    (void)index;
    if (strcmp(e->name, f->want) != 0) return true;
    *f->out = *e;
    f->found = true;
    return false;
}

vmfw_zip_status_t vmfw_zip_find(const vmfw_zip_t *zip, const char *name,
                                vmfw_zip_entry_t *out) {
    if (!zip || !name || !out) return VMFW_ZIP_ERR_INVALID_ARGUMENT;
    find_ctx_t f = { name, out, false };
    vmfw_zip_status_t st = vmfw_zip_iterate(zip, find_visit, &f);
    if (st != VMFW_ZIP_OK) return st;
    return f.found ? VMFW_ZIP_OK : VMFW_ZIP_ERR_BAD_DIRECTORY;
}

/* ------------------------------------------------------------------------ */
/* Extraction                                                                */
/* ------------------------------------------------------------------------ */

/* Pulls the member's compressed bytes out of the archive for the inflater. */
typedef struct {
    const vmfw_zip_t *zip;
    uint64_t off;
    uint64_t remaining;
    bool failed;
} member_source_t;

static size_t member_read(void *ctx, uint8_t *buf, size_t cap) {
    member_source_t *m = (member_source_t *)ctx;
    if (m->remaining == 0) return 0;
    size_t want = cap;
    if ((uint64_t)want > m->remaining) want = (size_t)m->remaining;
    size_t got = m->zip->pread(m->zip->ctx, m->off, buf, want);
    if (got != want) { m->failed = true; return VMFW_SOURCE_ERROR; }
    m->off += got;
    m->remaining -= got;
    return got;
}

/* Tees the produced bytes through CRC-32 on the way to the caller's sink, so
 * verification costs no extra pass over 208 MB. */
typedef struct {
    vmfw_sink_fn inner;
    void *inner_ctx;
    uint32_t crc;
    uint64_t total;
    bool inner_refused;
} crc_sink_t;

static bool crc_sink_write(void *ctx, const uint8_t *data, size_t len) {
    crc_sink_t *c = (crc_sink_t *)ctx;
    c->crc = vmfw_crc32(c->crc, data, len);
    c->total += len;
    if (c->inner && !c->inner(c->inner_ctx, data, len)) {
        c->inner_refused = true;
        return false;
    }
    return true;
}

vmfw_zip_status_t vmfw_zip_extract(const vmfw_zip_t *zip,
                                   const vmfw_zip_entry_t *entry,
                                   vmfw_sink_fn sink, void *sink_ctx) {
    if (!zip || !zip->pread || !entry) return VMFW_ZIP_ERR_INVALID_ARGUMENT;
    if (entry->method != VMFW_ZIP_METHOD_STORE &&
        entry->method != VMFW_ZIP_METHOD_DEFLATE)
        return VMFW_ZIP_ERR_BAD_METHOD;

    /*
     * The payload offset cannot be computed from the central directory: the
     * local header carries its OWN name and extra lengths, and they are allowed
     * to differ. Read it.
     */
    uint8_t lh[LOCAL_MIN_SIZE];
    if (entry->local_header_offset > zip->size ||
        zip->size - entry->local_header_offset < LOCAL_MIN_SIZE)
        return VMFW_ZIP_ERR_BAD_LOCAL_HEADER;
    if (!read_exact(zip, entry->local_header_offset, lh, sizeof lh))
        return VMFW_ZIP_ERR_READ;
    if (rd32(lh) != SIG_LOCAL) return VMFW_ZIP_ERR_BAD_LOCAL_HEADER;

    uint64_t data_off = entry->local_header_offset + LOCAL_MIN_SIZE
                      + rd16(lh + 26) + rd16(lh + 28);

    if (data_off > zip->size ||
        entry->compressed_size > zip->size - data_off)
        return VMFW_ZIP_ERR_MEMBER_TRUNCATED;

    member_source_t src = { zip, data_off, entry->compressed_size, false };
    crc_sink_t crc = { sink, sink_ctx, 0u, 0u, false };

    if (entry->method == VMFW_ZIP_METHOD_STORE) {
        if (entry->compressed_size != entry->uncompressed_size)
            return VMFW_ZIP_ERR_BAD_DIRECTORY;
        uint8_t buf[8192u];
        while (src.remaining) {
            size_t got = member_read(&src, buf, sizeof buf);
            if (got == VMFW_SOURCE_ERROR) return VMFW_ZIP_ERR_READ;
            if (got == 0) return VMFW_ZIP_ERR_MEMBER_TRUNCATED;
            if (!crc_sink_write(&crc, buf, got)) return VMFW_ZIP_ERR_SINK;
        }
    } else {
        uint64_t produced = 0;
        vmfw_inflate_status_t ist =
            vmfw_inflate(member_read, &src, crc_sink_write, &crc,
                         entry->uncompressed_size, &produced);
        if (ist != VMFW_INFLATE_OK) {
            if (crc.inner_refused)  return VMFW_ZIP_ERR_SINK;
            if (src.failed)         return VMFW_ZIP_ERR_READ;
            return VMFW_ZIP_ERR_INFLATE;
        }
    }

    /*
     * DELIBERATELY REDUNDANT, and mutation testing says so: deleting this line
     * does not fail the suite. It cannot, because both paths above already
     * guarantee the count -- the inflater is given uncompressed_size as a hard
     * bound and refuses a stream that ends short or long, and the stored path
     * checks the two sizes agree and then copies exactly that many bytes.
     *
     * It stays because it is the one statement of the property that does not
     * depend on either of those being right, and it costs a comparison per
     * member. Do not remove it as dead code; it is the backstop, not the check.
     */
    if (crc.total != entry->uncompressed_size)
        return VMFW_ZIP_ERR_MEMBER_TRUNCATED;

    /*
     * The central directory's CRC is the archive's own statement about what
     * this member is. Checking it here means every later stage -- IMG3, LZSS,
     * the disk image -- starts from bytes that have already been shown to be
     * Apple's, so a failure downstream is a real disagreement and not a bad
     * download.
     */
    if (crc.crc != entry->crc32) return VMFW_ZIP_ERR_CRC_MISMATCH;

    return VMFW_ZIP_OK;
}
