/* tools/rom_derive_selftest.c — Phase-5AJ host harness for tools/nnk_rom.inc.
 *
 * Compiles the RUNTIME fragment (not a copy of it) against the prelude's card
 * globals and prints what it derives from a card image.  This is the
 * cross-check the class-45/46 tests and PHASE5AJ.md quote:
 *
 *   cc -O2 -std=c11 -I tools tools/rom_derive_selftest.c -o out/rom_derive
 *   out/rom_derive "Ni No Kuni.nds" /tmp/p5aj
 *   md5sum /tmp/p5aj/*.bin            # vs work/ghidra_src/arm9.bin etc.
 *
 * The derived files are the proof that no off-line artifact is needed:
 *   guest.bin    == work/phase3a/arm9_stored_padded.bin
 *   arm9dec.bin  == work/ghidra_src/arm9.bin   (recompiler input)
 *   arena.bin    == work/phase3a/ovl_p4f4.bin  except the loader-written
 *                   sinit pointers (data, not code — see PHASE5AJ.md)
 */
#include <stdint.h>
#include <stddef.h>

/* The prelude's card model, which nnk_rom.inc fills by mmap. */
static uint8_t *rom = NULL;
static size_t rom_len = 0;

#include "nnk_rom.inc"

int main(int argc, char **argv) {
    const char *outdir = argc > 2 ? argv[2] : NULL;
    uint8_t *guest = NULL, *a9 = NULL, *win = NULL;
    size_t glen = 0, alen = 0, wlen = 0;
    char path[1024];

    if (argc < 2) {
        fprintf(stderr, "usage: %s <card.nds> [outdir]\n", argv[0]);
        return 2;
    }
    if (nnk_rom_attach(argv[1]) != 0) {
        fprintf(stderr, "not a NITRO card image: %s\n", argv[1]);
        return 2;
    }
    printf("hdr arm9 off=0x%X size=0x%X ram=0x%08X entry=0x%08X\n",
           nnk_rom_hdr.arm9_off, nnk_rom_hdr.arm9_size,
           nnk_rom_hdr.arm9_ram, nnk_rom_hdr.arm9_entry);
    printf("hdr arm7 off=0x%X size=0x%X fnt=0x%X+0x%X fat=0x%X+0x%X ovt=0x%X (%u entries)\n",
           nnk_rom_hdr.arm7_off, nnk_rom_hdr.arm7_size,
           nnk_rom_hdr.fnt_off, nnk_rom_hdr.fnt_size,
           nnk_rom_hdr.fat_off, nnk_rom_hdr.fat_size,
           nnk_rom_hdr.ovt_off, nnk_rom_hdr.ovt_size / 32u);

    glen = (size_t)nnk_rom_hdr.arm9_size;          /* no: calloc the dispatch size */
    glen = 0;
    guest = (uint8_t *)calloc(1, 0x6CB78);          /* the dispatch image size */
    if (!guest || nnk_rom_guest_image(guest, 0x6CB78) != 0) {
        fprintf(stderr, "guest image derivation failed\n");
        return 1;
    }
    glen = 0x6CB78;
    printf("guest     len=%zu fnv=%016llx\n", glen,
           (unsigned long long)nnk_rom_fnv64(guest, glen));

    if (nnk_rom_arm9_dec(&a9, &alen) != 0) {
        fprintf(stderr, "arm9 decompression failed\n");
        return 1;
    }
    printf("arm9dec   len=%zu fnv=%016llx\n", alen,
           (unsigned long long)nnk_rom_fnv64(a9, alen));

    if (nnk_rom_arena_window(NNK_SMC_GEN0_OVL, NNK_SMC_GEN0_BASE,
                             NNK_SMC_GEN0_LEN, &win, &wlen) != 0) {
        fprintf(stderr, "arena window derivation failed\n");
        return 1;
    }
    printf("arena     len=%zu fnv=%016llx (overlay %d at 0x%08X+)\n", wlen,
           (unsigned long long)nnk_rom_fnv64(win, wlen), NNK_SMC_GEN0_OVL,
           NNK_SMC_GEN0_BASE);

    if (outdir) {
        struct { const char *name; const uint8_t *p; size_t n; } f[3];
        int i;
        f[0].name = "guest.bin";   f[0].p = guest; f[0].n = glen;
        f[1].name = "arm9dec.bin"; f[1].p = a9;    f[1].n = alen;
        f[2].name = "arena.bin";   f[2].p = win;   f[2].n = wlen;
        for (i = 0; i < 3; i++) {
            FILE *fp;
            snprintf(path, sizeof path, "%s/%s", outdir, f[i].name);
            fp = fopen(path, "wb");
            if (!fp) { fprintf(stderr, "cannot write %s\n", path); return 1; }
            fwrite(f[i].p, 1, f[i].n, fp);
            fclose(fp);
            printf("wrote %s (%zu bytes)\n", path, f[i].n);
        }
    }
    free(guest); free(a9); free(win);
    return 0;
}
