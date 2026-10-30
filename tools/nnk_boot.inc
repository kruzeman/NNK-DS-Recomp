/* ==========================================================================
 * Phase-4K: console / firmware boot state that the NDS BIOS leaves in main
 * RAM before the ARM9 entry point (the runtime starts at the entry, so this
 * is initial state here — same precedent as bios_bootparam in the prelude).
 *
 * WHY (measured): the guest reads the 6-byte WIFI MAC of the console from
 * 0x02FFFCF4 (the mirrored firmware-header copy; the code is
 *   0x02008EB0: mov r1,r0; ldr r0,[pc,#8] (=0x02FFFCF4); ldr ip,[pc,#8];
 *               mov r2,#6; bx ip        -> memcpy(r1, 0x02FFFCF4, 6)
 * and compares it against the MAC recorded in its own data (table
 * 0x0215C17C, compared inverted: `ldrb r0,[r5,r6]; ldrb r1,[r2,r6];
 * eor r0,r0,#0xFF; cmp r1,r0; bne`).
 *
 * melonDS probe (MELONDS_REGPROBE_PCS=0x0215B79C,
 * MELONDS_REGPROBE_MEM=0x02FFFCF4:8) measures 0x02FFFCF4 =
 *   00 09 BF 11 22 33 41 10
 * at every decryptor entry and at the compare loop, and the stack buffer
 * (r5) holds 00 09 BF 11 22 33.  00:09:BF is Nintendo's OUI and
 * 11:22:33 is melonDS's documented default console MAC
 * (src/SPI_Firmware.h DEFAULT_MAC = {0x00,0x09,0xBF,0x11,0x22,0x33}); its
 * boot log prints "MAC: 00:09:BF:11:22:33".  The MAC is a CONSOLE property —
 * it comes from the firmware header, not from the ROM — so the native
 * runtime has to supply it, exactly like the RTC / firmware user-settings
 * block already supplied above.
 *
 * Without it the guest's 6-byte compare stopped at byte 1 (native buffer all
 * zero) instead of byte 3 (the game's stored MAC is 00:09:BF:00:00:31, so
 * only the OUI matches melonDS's default), and the guest took the
 * "different console" path — the Phase-4K divergence after the decrypt loop.
 *
 * NNK_MAC overrides the value ("aabbccddeeff", case-insensitive hex); the
 * default is melonDS's DEFAULT_MAC so native/melonDS traces stay comparable.
 * ========================================================================== */
#ifndef NNK_BOOT_INC
#define NNK_BOOT_INC

static void nnk_boot_firmware(void) {
    static const uint8_t def_mac[6] = { 0x00, 0x09, 0xBF, 0x11, 0x22, 0x33 };
    uint8_t mac[6];
    char txt[16];
    const char *s = getenv("NNK_MAC");
    unsigned i, ok = 0;

    memcpy(mac, def_mac, sizeof mac);
    if (s && strlen(s) == 12u) {
        ok = 1;
        for (i = 0; i < 6u; i++) {
            unsigned v = 0, k;
            for (k = 0; k < 2u; k++) {
                char ch = s[2u * i + k];
                unsigned d;
                if (ch >= '0' && ch <= '9') d = (unsigned)(ch - '0');
                else if (ch >= 'a' && ch <= 'f') d = (unsigned)(ch - 'a' + 10);
                else if (ch >= 'A' && ch <= 'F') d = (unsigned)(ch - 'A' + 10);
                else { ok = 0; break; }
                v = (v << 4) | d;
            }
            if (!ok) break;
            mac[i] = (uint8_t)v;
        }
    }
    if (s && s[0] && !ok)
        fprintf(stderr, "bios: NNK_MAC=%s is not 12 hex digits; using the "
                        "default console MAC\n", s);

    for (i = 0; i < 6u; i++)
        *(uint8_t *)main_ram_ptr(0x02FFFCF4u + i) = mac[i];
    /* 0x02FFFCFA: enabled wifi channels (melonDS writes header.EnabledChannels
     * there; measured 0x1041 in the reference boot). */
    *(uint8_t *)main_ram_ptr(0x02FFFCFAu) = 0x41u;
    *(uint8_t *)main_ram_ptr(0x02FFFCFBu) = 0x10u;

    snprintf(txt, sizeof txt, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    fprintf(stderr, "bios: console MAC %s at 0x02FFFCF4%s\n", txt,
            (s && s[0]) ? " (NNK_MAC)" : " (default = melonDS DEFAULT_MAC)");
}

#endif /* NNK_BOOT_INC */
