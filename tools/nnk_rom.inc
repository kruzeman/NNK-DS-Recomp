/* ==========================================================================
 * nnk_rom.inc — Phase-5AJ: the card image is the ONLY input.
 *
 * WHY: every phase before this one shipped a binary that needed files derived
 * off-line from the ROM: argv[1] was `arm9_stored_padded.bin`, and the SMC
 * generation-0 snapshot needed `NNK_SMC_IMAGE="0x02000000:<arm9.bin>,
 * 0x020C0000:<ovl_p4f4.bin>"`.  Measured in a directory holding only the ROM
 * and the runner:
 *
 *     $ ./nnk_runner arm9_stored_padded.bin
 *     cannot open guest image arm9_stored_padded.bin
 *
 * The operator requirement is that NOTHING comes from outside the ROM.  This
 * fragment derives all three images AT START-UP, in memory, from the card
 * image the user passes as argv[1]:
 *
 *   1. the GUEST IMAGE the dispatch loop boots (445,304 B at 0x02000000) —
 *      the ARM9 slice the ROM header names, zero-padded.  MEASURED, and the
 *      reason no decompressor is needed on this path: the shipped
 *      arm9_stored_padded.bin is byte-identical to
 *      ROM[0x4000 .. 0x4000+0x49328] for all 299,816 bytes and zero for the
 *      remaining 145,488 (md5 c9cf9596def17a08a9aeee259c877890).  The guest
 *      decompresses its own ARM9 in place at boot (the in-place decompressor
 *      at 0x0200098C, checkpoint 0x020009FC), which is exactly why the
 *      dispatch snapshot has to track the change.
 *   2. the DECOMPRESSED ARM9 (445,304 B) — the recompiler's input image and
 *      therefore the SMC generation-0 segment at 0x02000000.  Nintendo "code
 *      compression" (reversed LZ77 + 0xFF footer); our decoder is
 *      nnk_code_decompress() below, and the output is byte-identical to
 *      work/ghidra_src/arm9.bin (md5 2a19d5a14be083b40f07c8f80f5deb94).
 *   3. the ARENA WINDOW at 0x020C0000 (0xC0000 B) the dispatch image was also
 *      compiled from — the SMC generation-0 segment for overlay code.  It is
 *      a zero window with one decompressed overlay placed at its OVT ram base:
 *      overlay 17, the module resident in slot C at the first dispatch
 *      (measured: work/phase3a/ovl_p4f4.bin is 468,720/468,736 bytes identical
 *      to the decompressed ov17 and ZERO everywhere else; the 16 differing
 *      bytes are the loader-written static-initializer pointers inside the
 *      OVT sinit range 0x0213A26C..0x0213A290, i.e. data, not code).
 *
 * ITCM/DTCM: nothing to derive.  The runtime models the TCMs as zero-filled
 * memory (itcm[0x8000], dtcm[0x4000]) whose contents the guest fills itself
 * with its own DMA from main RAM; no phase ever loaded a TCM image from disk,
 * and the dsd extract's arm9/itcm.bin is a debug artifact only.
 *
 * Debug overrides (reviewer tools, never requirements):
 *   NNK_SMC_IMAGE="addr:path[,...]"  generation-0 segments from files
 *   NNK_SMC_GEN0_OVL=<id>            overlay placed in the arena window (17)
 *   NNK_ROM_PATH=<path>              card image for the card model/movie HLE
 *                                    when argv[1] is a raw guest image
 * ========================================================================== */
#ifndef NNK_ROM_INC
#define NNK_ROM_INC

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* --------------------------------------------------------- little endian -- */
static uint32_t nnk_rom_u32(const uint8_t *p, size_t at) {
    return (uint32_t)p[at] | ((uint32_t)p[at + 1] << 8) |
           ((uint32_t)p[at + 2] << 16) | ((uint32_t)p[at + 3] << 24);
}

/* FNV-1a 64 — the provenance number the startup log prints, so a ROM-derived
 * image can be compared against the reference bytes without a hash library. */
static uint64_t nnk_rom_fnv64(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= (uint64_t)p[i]; h *= 0x100000001B3ull; }
    return h;
}

/* ============================ DS code compression =========================
 * Nintendo's "code compression" (GBATEK; DSDecmp LZOverlay; ndspy
 * codeCompression).  An LZ-0x10-like stream read BACK TO FRONT: the last
 * header_len bytes of the stored slice are the footer
 *
 *     [pad 0xFF][extraSize u32][compLen:u24 + headerLen:u8]
 *
 * and the decompressed TAIL is produced by walking the compressed bytes from
 * the end towards the start, writing the output the same way, so both ends of
 * the output are addressed as data[len-1-x].  Bytes before the compressed
 * region are a literal passthrough prefix (ARM9: 0x3E00 bytes, the part the
 * guest's own decompressor stub occupies).
 *
 * Flags are consumed in a byte at data[-1-rb] with masks 0x80..0x01; a set bit
 * means a (length,displacement) pair, both bytes at data[-1-rb] and
 * data[-2-rb], length = (b1>>4)+3, disp = (((b1&0xF)<<8)|b2)+3.  ndspy's
 * clamp `if disp > currentOutSize then disp = 2` is reproduced: it is what the
 * real format does at the tail (minimum representable disp is 3).
 * Returns 0 on success; the caller owns *out.
 * ------------------------------------------------------------------------- */
#define NNK_CODE_MAX_OUT 0x02000000u   /* sanity cap: 32 MiB per image */

static int nnk_code_decompress(const uint8_t *src, size_t n,
                               uint8_t **outp, size_t *outlenp) {
    size_t full = n, amt, pass, m, cur, rb, cdata_len;
    uint32_t w, header_len, comp_len, extra, flags = 0;
    const uint8_t *cdata;
    uint8_t *out, *D;
    size_t i;
    int mask = 1, found = 0;

    if (!src || n < 0x10) return -1;
    /* Appended data after the footer (none in this ROM; ndspy probes it). */
    amt = 0;
    for (amt = 0; amt < 0x20; amt += 4) {
        if (n < amt + 8) break;
        w = nnk_rom_u32(src, n - amt - 8);
        if ((w >> 24) < 8) continue;
        if ((w & 0xFFFFFFu) > n) continue;
        found = 1;
        break;
    }
    if (!found) return -1;
    n = full - amt;
    if (n < 0x10) return -1;
    if (nnk_rom_u32(src, n - 4) == 0) {          /* extraSize 0: not compressed */
        out = (uint8_t *)malloc(full);
        if (!out) return -1;
        memcpy(out, src, full);
        *outp = out; *outlenp = full;
        return 0;
    }
    w = nnk_rom_u32(src, n - 8);
    header_len = w >> 24;
    comp_len   = w & 0xFFFFFFu;
    extra      = nnk_rom_u32(src, n - 4);
    if (header_len < 8 || (size_t)header_len > n) return -1;
    if ((size_t)comp_len > n) comp_len = (uint32_t)n;
    if ((size_t)comp_len < (size_t)header_len) return -1;
    pass = n - comp_len;                          /* literal prefix */
    cdata = src + pass;
    cdata_len = (size_t)comp_len - header_len;    /* footer excluded */
    m = n + (size_t)extra - pass;                 /* decompressed tail size */
    if (m == 0 || m > NNK_CODE_MAX_OUT) return -1;
    if (pass + m > NNK_CODE_MAX_OUT) return -1;
    out = (uint8_t *)calloc(1, pass + m + amt);
    if (!out) return -1;
    memcpy(out, src, pass);
    D = out + pass;
    cur = 0; rb = 0;
    while (cur < m) {
        if (mask == 1) {
            if (rb >= cdata_len) goto fail;
            flags = cdata[cdata_len - 1 - rb];
            rb++;
            mask = 0x80;
        } else mask >>= 1;
        if (flags & (uint32_t)mask) {
            uint32_t b1, b2, len, disp;
            size_t buf;
            if (rb + 1 >= cdata_len) goto fail;
            b1 = cdata[cdata_len - 1 - rb]; rb++;
            b2 = cdata[cdata_len - 1 - rb]; rb++;
            len = (b1 >> 4) + 3u;
            disp = (((b1 & 0xFu) << 8) | b2) + 3u;
            if ((size_t)disp > cur) {
                if (cur < 2) goto fail;
                disp = 2;                        /* ndspy's documented clamp */
            }
            buf = cur - disp;
            for (i = 0; i < len && cur < m; i++) {
                uint8_t nxt = D[m - 1 - buf];
                buf++;
                D[m - 1 - cur] = nxt;
                cur++;
            }
        } else {
            if (rb >= cdata_len) goto fail;
            D[m - 1 - cur] = cdata[cdata_len - 1 - rb];
            rb++;
            cur++;
        }
    }
    if (amt) memcpy(out + pass + m, src + n, amt);
    *outp = out;
    *outlenp = pass + m + amt;
    return 0;
fail:
    free(out);
    return -1;
}

/* ================================ card image ============================== */
/* Fields the derivations need, read straight out of the NITRO header. */
typedef struct {
    uint32_t arm9_off, arm9_entry, arm9_ram, arm9_size;
    uint32_t arm7_off, arm7_entry, arm7_ram, arm7_size;
    uint32_t fnt_off, fnt_size, fat_off, fat_size;
    uint32_t ovt_off, ovt_size;
    uint32_t header_size;
} NnkRomHdr;

static NnkRomHdr nnk_rom_hdr;
static char nnk_rom_path[512];
static int  nnk_rom_is_card;      /* 1 = `rom` is a card image from argv */

/* A NITRO header is recognised by its own invariants, not by the file name:
 * the header size word is 0x4000 (NDS) or 0x1000 (DSi), the ARM9/ARM7 slices
 * and the FNT/FAT/overlay tables all lie inside the file, and the ARM9 loads
 * below the 4 MiB IO window.  A raw guest image (445,304 B of ARM9 bytes)
 * cannot satisfy this: its "header size" word is compressed code. */
static int nnk_rom_parse_hdr(const uint8_t *d, size_t n, NnkRomHdr *h) {
    if (!d || n < 0x200) return -1;
    h->header_size = nnk_rom_u32(d, 0x84);
    if (h->header_size != 0x4000u && h->header_size != 0x1000u) return -1;
    h->arm9_off  = nnk_rom_u32(d, 0x20);
    h->arm9_entry = nnk_rom_u32(d, 0x24);
    h->arm9_ram  = nnk_rom_u32(d, 0x28);
    h->arm9_size = nnk_rom_u32(d, 0x2C);
    h->arm7_off  = nnk_rom_u32(d, 0x30);
    h->arm7_entry = nnk_rom_u32(d, 0x34);
    h->arm7_ram  = nnk_rom_u32(d, 0x38);
    h->arm7_size = nnk_rom_u32(d, 0x3C);
    h->fnt_off   = nnk_rom_u32(d, 0x40);
    h->fnt_size  = nnk_rom_u32(d, 0x44);
    h->fat_off   = nnk_rom_u32(d, 0x48);
    h->fat_size  = nnk_rom_u32(d, 0x4C);
    h->ovt_off   = nnk_rom_u32(d, 0x50);
    h->ovt_size  = nnk_rom_u32(d, 0x54);
    if (h->arm9_size == 0 || h->arm9_off < (uint32_t)h->header_size) return -1;
    if ((size_t)h->arm9_off + h->arm9_size > n) return -1;
    if (h->arm9_ram >= 0x04000000u) return -1;
    if ((size_t)h->fnt_off + h->fnt_size > n) return -1;
    if ((size_t)h->fat_off + h->fat_size > n) return -1;
    if ((size_t)h->ovt_off + h->ovt_size > n) return -1;
    if (h->ovt_size == 0 || (h->ovt_size % 32u) != 0) return -1;
    return 0;
}

/* Attach `path` as the card image (mmap: a 512 MiB ROM costs no RSS).  On
 * success `rom`/`rom_len` (the prelude's card model) point at the mapping, so
 * the card reads, the FNT/FAT walk and the movie HLE all use the same bytes. */
static int nnk_rom_attach(const char *path) {
    int fd;
    struct stat st;
    void *m;
    NnkRomHdr h;
    if (!path || !path[0]) return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || st.st_size < 0x200) { close(fd); return -1; }
    m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return -1;
    if (nnk_rom_parse_hdr((const uint8_t *)m, (size_t)st.st_size, &h) != 0) {
        munmap(m, (size_t)st.st_size);
        return -1;
    }
    rom = (uint8_t *)m;
    rom_len = (size_t)st.st_size;
    nnk_rom_hdr = h;
    nnk_rom_is_card = 1;
    snprintf(nnk_rom_path, sizeof nnk_rom_path, "%s", path);
    return 0;
}

static int nnk_rom_ready(void) { return nnk_rom_is_card && rom != NULL; }

/* ---- derivation 1: the guest image the dispatch loop boots --------------
 * The ROM slice the header names, zero-padded to the dispatch window.  The
 * padding is not cosmetic: it is the region the guest's in-place decompressor
 * writes (the stored slice is 299,816 B, the decompressed image 445,304 B). */
static int nnk_rom_guest_image(uint8_t *dst, size_t dstlen) {
    if (!nnk_rom_ready()) return -1;
    if ((size_t)nnk_rom_hdr.arm9_size > dstlen) return -2;
    memset(dst, 0, dstlen);
    memcpy(dst, rom + nnk_rom_hdr.arm9_off, nnk_rom_hdr.arm9_size);
    return 0;
}

/* ---- derivation 2: the decompressed ARM9 (recompiler input) -------------- */
static int nnk_rom_arm9_dec(uint8_t **out, size_t *outlen) {
    if (!nnk_rom_ready()) return -1;
    return nnk_code_decompress(rom + nnk_rom_hdr.arm9_off,
                               nnk_rom_hdr.arm9_size, out, outlen);
}

/* ---- derivation 3: one decompressed overlay ------------------------------ */
/* Overlay ids index the ROM's own 21-entry table (id -> FAT file id).  The
 * stored slice is code-compressed like the ARM9; the result lands exactly on
 * the OVT ramSize (verified for all 21 in DSD_OVERLAYS.md). */
static int nnk_rom_ovl_dec(int id, uint32_t *ram, uint32_t *ram_size,
                           uint8_t **out, size_t *outlen) {
    uint32_t k, n = nnk_rom_hdr.ovt_size / 32u;
    if (!nnk_rom_ready()) return -1;
    for (k = 0; k < n; k++) {
        const uint8_t *e = rom + nnk_rom_hdr.ovt_off + k * 32u;
        uint32_t oid = nnk_rom_u32(e, 0x00);
        uint32_t o_ram = nnk_rom_u32(e, 0x04);
        uint32_t o_size = nnk_rom_u32(e, 0x08);
        uint32_t fid = nnk_rom_u32(e, 0x18);
        uint32_t a, b;
        if (oid != (uint32_t)id) continue;
        if ((size_t)nnk_rom_hdr.fat_off + 8u * fid + 8u > rom_len) return -2;
        a = nnk_rom_u32(rom, nnk_rom_hdr.fat_off + 8u * fid);
        b = nnk_rom_u32(rom, nnk_rom_hdr.fat_off + 8u * fid + 4u);
        if (b <= a || (size_t)b > rom_len) return -3;
        if (nnk_code_decompress(rom + a, (size_t)(b - a), out, outlen) != 0) return -4;
        if (ram) *ram = o_ram;
        if (ram_size) *ram_size = o_size;
        return 0;
    }
    return -5;
}

/* ---- the SMC arena window ----------------------------------------------
 * A zero window of `len` bytes at `base`, with one decompressed overlay placed
 * at its OVT ram base.  Same bytes the frozen dispatch image was compiled
 * from (work/phase3a/ovl_p4f4.bin), except the loader-written sinit pointers,
 * which are data. */
#define NNK_SMC_GEN0_BASE 0x020C0000u
#define NNK_SMC_GEN0_LEN  0x000C0000u
#define NNK_SMC_GEN0_OVL  17

static int nnk_rom_arena_window(int ovl_id, uint32_t base, uint32_t len,
                                uint8_t **out, size_t *outlen) {
    uint8_t *img = NULL, *win;
    uint32_t ram = 0, rsize = 0;
    size_t ilen = 0;
    if (nnk_rom_ovl_dec(ovl_id, &ram, &rsize, &img, &ilen) != 0) return -1;
    if ((size_t)ilen > (size_t)len || ram < base || (size_t)(ram - base) + ilen > len) {
        free(img);
        return -2;
    }
    win = (uint8_t *)calloc(1, len);
    if (!win) { free(img); return -3; }
    memcpy(win + (ram - base), img, ilen);
    free(img);
    *out = win;
    *outlen = len;
    return 0;
}

/* ---- argv[1] -> gmem ----------------------------------------------------
 * A card image is derived; anything else is loaded as a raw guest image, the
 * Phase-3A..5AI behaviour, kept so an existing 445,304-byte image still runs
 * for A/B diagnosis (it is a debug path, not the shipped contract). */
static size_t nnk_rom_guest_len;
static int nnk_rom_image_load(const char *path, uint8_t *dst, size_t dstlen) {
    if (nnk_rom_attach(path) == 0) {
        if (nnk_rom_guest_image(dst, dstlen) != 0) {
            fprintf(stderr, "rom: cannot derive the guest image from %s "
                            "(arm9 %u B at 0x%X does not fit %zu)\n",
                    path, nnk_rom_hdr.arm9_size, nnk_rom_hdr.arm9_off, dstlen);
            return -1;
        }
        nnk_rom_guest_len = dstlen;
        fprintf(stderr, "rom: card image %s (%zu B) arm9 %u B @0x%X -> 0x%08X "
                        "entry 0x%08X | fnt %u B fat %u B ovt %u entries\n",
                path, rom_len, nnk_rom_hdr.arm9_size, nnk_rom_hdr.arm9_off,
                nnk_rom_hdr.arm9_ram, nnk_rom_hdr.arm9_entry,
                nnk_rom_hdr.fnt_size, nnk_rom_hdr.fat_size,
                nnk_rom_hdr.ovt_size / 32u);
        fprintf(stderr, "rom: derived guest image (stored arm9 zero-padded) "
                        "%zu B fnv=%016llx\n", nnk_rom_guest_len,
                (unsigned long long)nnk_rom_fnv64(dst, nnk_rom_guest_len));
        return 0;
    }
    {   /* legacy raw guest image */
        FILE *fp = fopen(path, "rb");
        size_t got;
        if (!fp) {
            fprintf(stderr, "cannot open guest image %s: %s\n", path, strerror(errno));
            fprintf(stderr, "  expected a NITRO card image (.nds): every guest image "
                            "is derived from it at start-up (Phase-5AJ)\n");
            return -1;
        }
        got = fread(dst, 1, dstlen, fp);
        fclose(fp);
        if (got != dstlen) {
            fprintf(stderr, "short read: got %zu, want %zu\n", got, dstlen);
            return -1;
        }
        nnk_rom_guest_len = dstlen;
        fprintf(stderr, "image: raw guest image %s (%zu B, no card header); "
                        "the card model needs NNK_ROM_PATH and the SMC snapshot "
                        "needs NNK_SMC_IMAGE\n", path, got);
        return 0;
    }
}

#endif /* NNK_ROM_INC */
