/* tools/nnks_selftest.c — Phase-5BH host harness for the NNKS container.
 *
 * It compiles the SHIPPED fragment tools/nnks_container.inc (the header, the
 * region records, the field directory, the identity check and the two field
 * matching policies) — not a copy of it — and exercises the four load classes
 * the savestate format has to distinguish:
 *
 *   (a) EXACT        : the same layout restores byte-for-byte;
 *   (b) PARTIAL      : a stored file from an older layout restores every field
 *                      it still knows BY NAME, defaults the new ones and
 *                      reports them — an INSERTED field must not shift its
 *                      neighbours (the prefix policy does, which is why the
 *                      directory exists; both are measured here);
 *   (c) REFUSED      : bad magic and a different ROM identity are identified
 *                      as genuine incompatibilities (and a field-layout change
 *                      is NOT one);
 *   (d) NOT A COLD BOOT: after (b) the machine is neither the saved one nor a
 *                      fresh one — the restored fields hold the saved values
 *                      while only the unknown fields keep their defaults.
 *
 * Build/run:  cc -O2 -std=c11 -I tools tools/nnks_selftest.c -o nnks_selftest
 *             ./nnks_selftest        (exit 0 = every check passed)
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nnks_container.inc"

static int failures;
static int checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) { failures++; printf("FAIL: %s\n", what); }
    else     printf("ok  : %s\n", what);
}

#define F(x, n) { n, (uint32_t)sizeof(x), (uint8_t *)&(x) }

/* Layout A (the file that was written):  a, b, c */
typedef struct { uint32_t a, b, c; } LayA;
/* Layout B (the build that loads it):  a, NEW, b, c  — NEW was INSERTED in the
 * middle, exactly the shape a later phase produces when a static lands in the
 * middle of an existing fragment. */
typedef struct { uint32_t a, newf, b, c; } LayB;

int main(void)
{
    LayA A;
    LayB B;
    NnkStateField fa[] = { F(A.a, "a"), F(A.b, "b"), F(A.c, "c") };
    NnkStateField fb[] = { F(B.a, "a"), F(B.newf, "newf"), F(B.b, "b"), F(B.c, "c") };
    NnkFieldRec dirA[8];
    uint8_t payload[64];
    NnkFieldPlan pl;
    NnkStateHdr h;
    uint32_t ndir, ident;
    size_t off = 0, i;
    int bad = 0;

    /* ---- the header is byte-compatible with v1 (72 bytes) ---- */
    check(sizeof(NnkStateHdr) == 72, "header is 72 bytes");
    check(sizeof(NnkStateRegionV1) == 40, "v1 region record is 40 bytes");
    check(sizeof(NnkStateRegionV2) == 48, "v2 region record is 48 bytes");
    check(sizeof(NnkFieldRec) == 32, "field record is 32 bytes");
    check(nnks_field_total(fa, 3, &bad) == 12 && !bad, "layout A is 12 bytes");

    /* ---- (a) EXACT ---- */
    A.a = 0x11111111u; A.b = 0x22222222u; A.c = 0x33333333u;
    for (i = 0; i < 3; i++) { memcpy(payload + off, fa[i].p, fa[i].len); off += fa[i].len; }
    ndir = nnks_mkdir(fa, 3, dirA, 8);
    check(ndir == 3 && dirA[0].off == 0 && dirA[1].off == 4 && dirA[2].off == 8,
          "the directory carries name/off/len per field");
    check(nnks_region_exact(1, nnks_field_fp(fa, 3), 12, 1, nnks_field_fp(fa, 3), 12),
          "STRICT: an identical region is exact");
    check(!nnks_region_exact(1, nnks_field_fp(fa, 3), 12, 1, nnks_field_fp(fb, 4), 16),
          "STRICT: a resized region is NOT exact");

    B.a = B.newf = B.b = B.c = 0;
    nnks_restore_fields(fb, 4, dirA, ndir, payload, 12, &pl);
    check(pl.ok == 3 && pl.missing == 1 && pl.partial == 0,
          "PARTIAL: three fields restored by name, one defaulted");
    check(pl.nnames_total == 1 && strcmp(pl.names[0], "newf") == 0,
          "PARTIAL: the defaulted field is named");
    check(!nnks_plan_all_ok(&pl), "PARTIAL: the plan is not 'all ok'");

    /* ---- (b) an INSERTED field must not shift its neighbours ---- */
    check(B.a == 0x11111111u && B.b == 0x22222222u && B.c == 0x33333333u,
          "PARTIAL: a, b and c hold the saved values across an insertion");
    check(B.newf == 0, "PARTIAL: the new field keeps the build's default");
    /* the v1 policy on the same bytes misaligns — it is only safe when the
     * layout only ever grew at the END, which is why v2 stores the directory */
    memset(&B, 0, sizeof B);
    B.newf = 0xDEADBEEFu;           /* a non-zero default makes the shift visible */
    nnks_restore_fields_prefix(fb, 4, payload, 12, &pl);
    check(B.newf == 0x22222222u && B.b == 0x33333333u,
          "PREFIX (v1 policy): an insertion shifts every later field (measured)");

    /* ---- (c) identity: the only genuine incompatibilities ---- */
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "NNKS", 4);
    h.version = NNKS_VERSION;
    h.rom_size = 0x4000000ull;
    h.rom_hdr_crc = 0xAABBCCDDu;
    h.arm9_crc = 0x12345678u;
    h.header_size = 200;
    h.region_count = 11;
    h.header_crc = nnks_crc32((const uint8_t *)&h, NNKS_OFF_HDRCRC);
    ident = nnks_identity(&h, 0x4000000ull, 0xAABBCCDDu, 0x12345678u);
    check(ident == 0, "identity: a matching header is accepted");
    /* a different FIELD LAYOUT is not an identity failure (that is the point) */
    {
        NnkStateHdr h2 = h;
        h2.build_fp = 0x0BADF00Du;
        h2.header_crc = nnks_crc32((const uint8_t *)&h2, NNKS_OFF_HDRCRC);
        check(nnks_identity(&h2, 0x4000000ull, 0xAABBCCDDu, 0x12345678u) == 0,
              "identity: a build/layout change is NOT a refusal");
    }
    {
        NnkStateHdr h2 = h;
        h2.magic[0] = 'X';
        h2.header_crc = nnks_crc32((const uint8_t *)&h2, NNKS_OFF_HDRCRC);
        check(nnks_identity(&h2, 0x4000000ull, 0xAABBCCDDu, 0x12345678u) & NNKS_ID_MAGIC,
              "identity: bad magic is refused");
    }
    {
        NnkStateHdr h2 = h;
        h2.version = 9;
        h2.header_crc = nnks_crc32((const uint8_t *)&h2, NNKS_OFF_HDRCRC);
        check(nnks_identity(&h2, 0x4000000ull, 0xAABBCCDDu, 0x12345678u) & NNKS_ID_VERSION,
              "identity: an unknown container version is refused");
    }
    check(nnks_identity(&h, 0x8000000ull, 0xAABBCCDDu, 0x12345678u) & NNKS_ID_ROMSIZE,
          "identity: a different ROM size is refused");
    check(nnks_identity(&h, 0x4000000ull, 0xDEADBEEFu, 0x12345678u) & NNKS_ID_ROMHDR,
          "identity: a different ROM header is refused");
    check(nnks_identity(&h, 0x4000000ull, 0xAABBCCDDu, 0xFFFFFFFFu) & NNKS_ID_ARM9,
          "identity: a different ARM9 blob is refused");
    {
        NnkStateHdr h2 = h;
        h2.rom_hdr_crc ^= 1u;      /* header CRC now stale */
        check(nnks_identity(&h2, 0x4000000ull, 0xAABBCCDDu, 0x12345678u) & NNKS_ID_HDRCRC,
              "identity: a corrupt header is refused");
    }

    /* ---- (d) a partial restore is never a cold boot, never the saved one ---- */
    memset(&B, 0, sizeof B);
    nnks_restore_fields(fb, 4, dirA, ndir, payload, 12, &pl);
    {
        LayB cold;
        memset(&cold, 0, sizeof cold);
        check(memcmp(&B, &cold, sizeof B) != 0,
              "partial restore != cold boot (the machine really moved)");
        check(B.a == A.a && B.b == A.b && B.c == A.c,
              "partial restore carries the saved CPU/memory-style fields");
        B.newf = 0xDEADBEEFu;
        check(memcmp(&B, &cold, sizeof B) != 0, "still not the cold machine");
    }
    /* an empty/absent region cannot be mistaken for a restored one */
    memset(&B, 0, sizeof B);
    nnks_restore_fields(fb, 4, NULL, 0, payload, 0, &pl);
    check(pl.ok == 0 && pl.missing == 4 && pl.bytes_defaulted == 16,
          "no stored fields: every field is reported defaulted");

    printf("\n%u checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
