/* tools/nnks_container.inc — Phase-5BH: the NNKS container FORMAT layer.
 *
 * This fragment is the part of the savestate code that is pure data: the
 * header, the per-region records, the per-region FIELD DIRECTORY, the
 * checksums, the identity check and the two field-matching policies (exact,
 * and best-effort/tolerant).  It has NO dependency on the emulated machine —
 * no register file, no guest memory, no display handle — so the same C that
 * ships in the runtime is compiled by tools/nnks_selftest.c and exercised by
 * the class test (phase_5bh_*).  The machine glue (region lengths, the gx blob,
 * the movie stream table, file I/O, logging) stays in tools/nnk_state.inc.
 *
 * WHY THE FORMAT CHANGED (Phase-5BH, measured on master e395fb4)
 *   The v1 container recorded a region as {name[16], offset, length, crc}.  A
 *   region's byte length is IMPLICIT in the writer: `io` is the concatenation
 *   of every generated field row.  Any phase that adds one runtime static grows
 *   `io` (68660 B then, 85192 B now), and a v1 loader could only compare the
 *   length and refuse the whole file:
 *       state: REFUSED ...: region 'io' is 68660 bytes, this build writes 85192
 *       state: boot from ... failed — replaying the boot
 *   i.e. every earlier state died on a rebuild, and the run silently became a
 *   cold boot.  v2 makes the file self-describing:
 *     - the header carries a `flags` word (v1's 4 bytes of padding) marking the
 *       field directories;
 *     - a region record carries {id, version, offset, length, crc, nfields}
 *       and the region's name, so a stored region is matched to the running
 *       build BY ID (never by table position);
 *     - after the region table each field-table region carries its own
 *       directory of {name, off, len}, so a stored field is matched to the
 *       running build BY NAME — an inserted or removed static cannot shift the
 *       bytes of every later field into the wrong variable;
 *     - a length/version mismatch is no longer fatal: matching fields are
 *       restored, the rest are reported as defaulted, and only a real
 *       incompatibility (magic, container version, header/payload CRC, ROM
 *       identity) refuses the file.
 *   v1 files are still READ (region records are matched by name, fields by
 *   prefix) — the operator's existing reproducers are exactly such files.
 *
 * NNKS_VERSION 2 is written; 1 and 2 are read.
 */
#ifndef NNK_CONTAINER_INC
#define NNK_CONTAINER_INC

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NNKS_VERSION      2u
#define NNKS_VERSION_MIN  1u
#define NNKS_MAXREGIONS   16
#define NNKS_FIELDNAME    24     /* NUL-terminated field name inside a directory */
#define NNKS_MAXREPORT    24     /* field names quoted per region in the log */

/* ---- checksums ---------------------------------------------------------- */

static uint32_t nnks_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t nnks_fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* ---- one generated field table row -------------------------------------- */

typedef struct {
    const char *name;
    uint32_t len;
    uint8_t *p;
} NnkStateField;

/* ---- container records -------------------------------------------------- */

typedef struct {
    char     magic[4];       /* "NNKS" */
    uint32_t version;        /* 2 now; 1 files are still read */
    uint32_t header_size;    /* sizeof(hdr) + nreg*region_rec + total_dir*field_rec */
    uint32_t region_count;
    uint32_t build_fp;       /* FNV-1a over the region names + every field name/len */
    uint32_t flags;          /* NNKS_FLAG_*; in v1 this was 4 bytes of padding */
    uint64_t rom_size;       /* identity: card image size in bytes */
    uint32_t rom_hdr_crc;    /* identity: CRC32 of ROM[0x000..0x1FF] */
    uint32_t arm9_crc;       /* identity: CRC32 of the ROM's ARM9 blob */
    uint64_t record_step;    /* guest step counter at the save point */
    uint64_t record_frames;  /* display frame counter at the save point */
    uint64_t payload_size;
    uint32_t payload_crc;
    uint32_t header_crc;     /* over every byte of the header before this field */
} NnkStateHdr;

/* v1 region record (read-only path) */
typedef struct {
    char     name[16];
    uint64_t offset;
    uint64_t length;
    uint32_t crc;
    uint32_t rsv;
} NnkStateRegionV1;

/* v2 region record: ID + layout version + length + field-directory count */
typedef struct {
    uint32_t id;
    uint32_t version;        /* layout version of the region's payload */
    uint64_t offset;
    uint64_t length;
    uint32_t crc;
    uint32_t nfields;        /* field-directory entries that follow, 0 for blob kinds */
    char     name[16];
} NnkStateRegionV2;

typedef struct {
    char     name[NNKS_FIELDNAME];
    uint32_t off;            /* offset inside the region payload */
    uint32_t len;
} NnkFieldRec;

#define NNKS_FLAG_FIELDDIR 1u

#define NNKS_OFF_HDRCRC (sizeof(NnkStateHdr) - sizeof(uint32_t))

/* The header must stay byte-compatible with v1 (72 bytes, flags occupies the
 * old padding word): a v1 file's header parse is the same code. */
_Static_assert(sizeof(NnkStateHdr) == 72, "NNKS header must stay 72 bytes");
_Static_assert(sizeof(NnkStateRegionV1) == 40, "v1 region record is 40 bytes");
_Static_assert(sizeof(NnkStateRegionV2) == 48, "v2 region record is 48 bytes");
_Static_assert(sizeof(NnkFieldRec) == 32, "field record is 32 bytes");
_Static_assert(offsetof(NnkStateHdr, rom_size) == 24, "identity block moved");

/* ---- identity ----------------------------------------------------------- */

#define NNKS_ID_MAGIC    1u
#define NNKS_ID_VERSION  2u
#define NNKS_ID_HDRCRC   4u
#define NNKS_ID_ROMSIZE  8u
#define NNKS_ID_ROMHDR   16u
#define NNKS_ID_ARM9     32u

/* Everything that makes a file genuinely unusable.  A field layout change is
 * NOT in this list — that is what the tolerant restore exists for. */
static uint32_t nnks_identity(const NnkStateHdr *h, uint64_t rom_size,
                              uint32_t rom_hdr_crc, uint32_t arm9_crc)
{
    uint32_t f = 0;
    if (memcmp(h->magic, "NNKS", 4) != 0) f |= NNKS_ID_MAGIC;
    if (h->version < NNKS_VERSION_MIN || h->version > NNKS_VERSION) f |= NNKS_ID_VERSION;
    if (h->header_crc != nnks_crc32((const uint8_t *)h, NNKS_OFF_HDRCRC)) f |= NNKS_ID_HDRCRC;
    if (h->rom_size != rom_size) f |= NNKS_ID_ROMSIZE;
    if (h->rom_hdr_crc != rom_hdr_crc) f |= NNKS_ID_ROMHDR;
    if (h->arm9_crc != arm9_crc) f |= NNKS_ID_ARM9;
    return f;
}

/* ---- region layout version / field directory ---------------------------- */

/* FNV-1a over the region's field names and lengths: the region's layout
 * version.  Two builds with the same io fields get the same number; adding,
 * removing, re-ordering or resizing a field changes it. */
static uint32_t nnks_field_fp(const NnkStateField *f, uint32_t n)
{
    uint32_t h = 2166136261u;
    uint32_t i;
    for (i = 0; i < n; i++) {
        h ^= nnks_fnv(f[i].name, strlen(f[i].name));
        h *= 16777619u;
        h ^= f[i].len;
        h *= 16777619u;
    }
    return h;
}

static uint32_t nnks_field_total(const NnkStateField *f, uint32_t n, int *bad_name)
{
    uint32_t total = 0, i;
    if (bad_name) *bad_name = 0;
    for (i = 0; i < n; i++) {
        if (strlen(f[i].name) >= NNKS_FIELDNAME && bad_name) *bad_name = 1;
        total += f[i].len;
    }
    return total;
}

/* Build the directory of a field table.  Returns the entry count written. */
static uint32_t nnks_mkdir(const NnkStateField *f, uint32_t n, NnkFieldRec *dst, uint32_t cap)
{
    uint32_t i, off = 0, w = 0;
    for (i = 0; i < n && w < cap; i++) {
        memset(&dst[w], 0, sizeof dst[w]);
        snprintf(dst[w].name, sizeof dst[w].name, "%s", f[i].name);
        dst[w].off = off;
        dst[w].len = f[i].len;
        off += f[i].len;
        w++;
    }
    return w;
}

/* The STRICT contract: does a stored region describe EXACTLY this build's
 * region?  (The default path does not need this — it matches per field.) */
static int nnks_region_exact(uint32_t stored_id, uint32_t stored_version,
                             uint64_t stored_len,
                             uint32_t id, uint32_t version, size_t len)
{
    return stored_id == id && stored_version == version && stored_len == (uint64_t)len;
}

/* ---- tolerant field restore --------------------------------------------- */

typedef struct {
    uint32_t ncur;            /* fields this build has */
    uint32_t ok;              /* restored byte-for-byte */
    uint32_t partial;         /* restored up to min(stored, current) bytes */
    uint32_t missing;         /* defaulted: not in the stored file / past its end */
    uint32_t extra;           /* stored fields this build no longer has */
    uint64_t bytes_defaulted; /* bytes left at the current value */
    uint32_t nnames;          /* distinct names quoted (capped) */
    uint32_t nnames_total;    /* every defaulted/partial field name */
    char     names[NNKS_MAXREPORT][NNKS_FIELDNAME];
} NnkFieldPlan;

static void nnks_plan_note(NnkFieldPlan *pl, const char *name)
{
    pl->nnames_total++;
    if (pl->nnames >= NNKS_MAXREPORT) return;
    snprintf(pl->names[pl->nnames], NNKS_FIELDNAME, "%s", name);
    pl->nnames++;
}

static int nnks_plan_all_ok(const NnkFieldPlan *pl)
{
    return pl->partial == 0 && pl->missing == 0 && pl->bytes_defaulted == 0;
}

/* The v2 policy: match by NAME through the stored directory. */
static void nnks_restore_fields(const NnkStateField *cur, uint32_t ncur,
                                const NnkFieldRec *dir, uint32_t ndir,
                                const uint8_t *src, size_t srclen,
                                NnkFieldPlan *pl)
{
    uint32_t i, j;
    memset(pl, 0, sizeof *pl);
    pl->ncur = ncur;
    for (i = 0; i < ncur; i++) {
        int found = 0;
        for (j = 0; j < ndir; j++) {
            if (strncmp(dir[j].name, cur[i].name, NNKS_FIELDNAME) != 0) continue;
            found = 1;
            if ((size_t)dir[j].off >= srclen) {
                pl->missing++;
                pl->bytes_defaulted += cur[i].len;
                nnks_plan_note(pl, cur[i].name);
            } else {
                size_t avail = srclen - (size_t)dir[j].off;
                size_t n = cur[i].len;
                if (n > (size_t)dir[j].len) n = (size_t)dir[j].len;
                if (n > avail) n = avail;
                memcpy(cur[i].p, src + dir[j].off, n);
                if (n == cur[i].len && n == (size_t)dir[j].len) {
                    pl->ok++;
                } else {
                    pl->partial++;
                    pl->bytes_defaulted += cur[i].len - n;
                    nnks_plan_note(pl, cur[i].name);
                }
            }
            break;
        }
        if (!found) {
            pl->missing++;
            pl->bytes_defaulted += cur[i].len;
            nnks_plan_note(pl, cur[i].name);
        }
    }
    for (j = 0; j < ndir; j++) {
        int known = 0;
        for (i = 0; i < ncur; i++)
            if (strncmp(dir[j].name, cur[i].name, NNKS_FIELDNAME) == 0) { known = 1; break; }
        if (!known) pl->extra++;
    }
}

/* The v1 policy (a v1 file has no field directory): a region is the
 * concatenation of its fields, so copy as far as the stored bytes reach and
 * report every field that is not covered.  This is the documented
 * "apply the fields that exist up to min(len)" rule. */
static void nnks_restore_fields_prefix(const NnkStateField *cur, uint32_t ncur,
                                       const uint8_t *src, size_t srclen,
                                       NnkFieldPlan *pl)
{
    size_t off = 0;
    uint32_t i;
    memset(pl, 0, sizeof *pl);
    pl->ncur = ncur;
    for (i = 0; i < ncur; i++) {
        if (off + cur[i].len <= srclen) {
            memcpy(cur[i].p, src + off, cur[i].len);
            pl->ok++;
            off += cur[i].len;
        } else if (off < srclen) {
            size_t n = srclen - off;
            memcpy(cur[i].p, src + off, n);
            pl->partial++;
            pl->bytes_defaulted += cur[i].len - n;
            nnks_plan_note(pl, cur[i].name);
            off = srclen;
        } else {
            pl->missing++;
            pl->bytes_defaulted += cur[i].len;
            nnks_plan_note(pl, cur[i].name);
        }
    }
    if (srclen > off) pl->extra++;
}

/* The canonical state hash ignores the container's framing and hashes the
 * payload bytes in table order; nothing here depends on the machine either. */
static uint32_t nnks_payload_fnv(const uint8_t *p, size_t n) { return nnks_fnv(p, n); }

#endif /* NNK_CONTAINER_INC */
