/* tools/nnk_input.inc — Phase-4W: SDL keyboard -> DS KEYINPUT/EXTKEYIN.
 *
 * Spliced into nnk_main.c by tools/split_dispatch.py (same pattern as
 * nnk_dma.inc / nnk_vramcnt.inc), so it sees every runtime static (steps,
 * gxd, gxd_frames, ...).  It is the one owner of the SDL -> DS key mapping:
 * an SDL event filter consumes KEYDOWN/KEYUP before gx_display's own pump
 * sees them (pump_events would otherwise apply its built-in keymap), and the
 * resulting state is written through the merged API's key_set path
 * (gxd_set_keymask), which is exactly what the guest reads back from
 * KEYINPUT (0x04000130) via io_read16 -> gxd_io_read.
 *
 * Key map (documented in INPUT.md, scancode names resolved at startup):
 *
 *   arrows = D-pad   X = A   Z = B   S = DS-X   A = DS-Y
 *   Q = L   W = R   Return = Start   RShift/Backspace = Select
 *
 * Semantics (melonDS ground truth, src/NDS.cpp):
 *   - KEYINPUT bits 0-9 are ACTIVE-LOW (0 = pressed); idle word 0x03FF.
 *   - NDS::SetKeyMask maps mask bits 10-11 onto KeyInput bits 16-17, i.e.
 *     EXTKEYIN bits 0-1 = DS X / DS Y (GBATEK EXTKEYIN bit0/1, active-low).
 *   - KeyInput reset is 0x007F03FF -> EXTKEYIN (KeyInput>>16)&0xFF idles at
 *     0x7F; lid closed would set bit 23 (EXTKEYIN bit7).  The runtime keeps
 *     the lid open and serves EXTKEYIN from nnk_extkey.
 *   - key-repeat events are ignored (a held key stays down via its KEYDOWN,
 *     repeats add nothing), focus loss / minimize releases everything (no
 *     stuck keys).
 *
 * Input injection for the end-to-end proof (see INPUT.md):
 *   NNK_INPUT_INJECT="<step>:<scancode-name>:<down|up>[;...]"
 *       pushes real SDL key events (SDL_PushEvent) at the given runtime step
 *       (windowed) or applies the same state change headless.
 *   NNK_INPUT_TRACE=<path>   append-only event/transition trace.
 *
 * Non-NNK_GX builds get no-op stubs, so the spliced io_read16 hook and the
 * per-dispatch tick compile and cost nothing.
 */
#ifndef NNK_INPUT_INC
#define NNK_INPUT_INC

#ifdef NNK_GX

#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <stdarg.h>

/* ---- state ---- */
static FILE *nnk_in_log;                 /* NNK_INPUT_TRACE, NULL = quiet */
static uint32_t nnk_mask = 0xFFF;        /* active-low SetKeyMask shape:
                                          * bits 0-9 keys, 10 = DS X, 11 = DS Y */
static uint32_t nnk_extkey = 0x7F;       /* EXTKEYIN low byte (X/Y pressed
                                          * clear bits 0/1) */
static int nnk_inited;
static uint64_t nnk_ki_reads;            /* guest KEYINPUT reads seen */
static uint32_t nnk_ki_last = 0xFFFFFFFFu;

static void nnk_ilog(const char *fmt, ...)
{
    va_list ap;
    if (!nnk_in_log) return;
    va_start(ap, fmt);
    fprintf(nnk_in_log, "step=%llu ", (unsigned long long)steps);
    vfprintf(nnk_in_log, fmt, ap);
    va_end(ap);
    fputc('\n', nnk_in_log);
    fflush(nnk_in_log);
}

/* ---- key map: scancode name -> DS key ---- */
static const struct { const char *name; int keybit; int is_xy; } nnk_keymap[] = {
    { "Up",        GXD_KEY_UP,     0 },
    { "Down",      GXD_KEY_DOWN,   0 },
    { "Left",      GXD_KEY_LEFT,   0 },
    { "Right",     GXD_KEY_RIGHT,  0 },
    { "X",         GXD_KEY_A,      0 },
    { "Z",         GXD_KEY_B,      0 },
    { "S",         10,             1 },   /* DS X -> EXTKEYIN bit0 / mask bit10 */
    { "A",         11,             1 },   /* DS Y -> EXTKEYIN bit1 / mask bit11 */
    { "Q",         GXD_KEY_L,      0 },
    { "W",         GXD_KEY_R,      0 },
    { "Return",    GXD_KEY_START,  0 },
    { "Right Shift", GXD_KEY_SELECT, 0 },
    { "Backspace", GXD_KEY_SELECT, 0 },
};
#define NNK_NKEYS ((int)(sizeof nnk_keymap / sizeof nnk_keymap[0]))
static SDL_Scancode nnk_sc[NNK_NKEYS];

static const char *nnk_keyname(SDL_Scancode sc)
{
    int i;
    for (i = 0; i < NNK_NKEYS; i++)
        if (nnk_sc[i] == sc) return nnk_keymap[i].name;
    return NULL;
}

static void nnk_apply_key(int idx, int down)
{
    if (down) {
        nnk_mask &= ~(1u << nnk_keymap[idx].keybit);
        if (nnk_keymap[idx].is_xy) nnk_extkey &= ~(1u << (nnk_keymap[idx].keybit - 10));
    } else {
        nnk_mask |= (1u << nnk_keymap[idx].keybit);
        if (nnk_keymap[idx].is_xy) nnk_extkey |= (1u << (nnk_keymap[idx].keybit - 10));
    }
    nnk_mask &= 0xFFF;
    gxd_set_keymask(gxd, nnk_mask);
    nnk_ilog("key %-13s %-4s -> DS %-6s mask=0x%03X EXTKEYIN=0x%02X",
             nnk_keymap[idx].name, down ? "down" : "up",
             nnk_keymap[idx].keybit == 10 ? "X" :
             nnk_keymap[idx].keybit == 11 ? "Y" :
             nnk_keymap[idx].keybit == GXD_KEY_A ? "A" :
             nnk_keymap[idx].keybit == GXD_KEY_B ? "B" :
             nnk_keymap[idx].keybit == GXD_KEY_L ? "L" :
             nnk_keymap[idx].keybit == GXD_KEY_R ? "R" :
             nnk_keymap[idx].keybit == GXD_KEY_START ? "Start" :
             nnk_keymap[idx].keybit == GXD_KEY_SELECT ? "Select" : "D-pad",
             nnk_mask, nnk_extkey);
}

static void nnk_release_all(const char *why)
{
    nnk_mask = 0xFFF;
    nnk_extkey = 0x7F;
    gxd_set_keymask(gxd, nnk_mask);
    nnk_ilog("release-all (%s): mask=0xFFF EXTKEYIN=0x7F", why);
}

/* One SDL key event -> DS state.  Shared by the filter (real events) and the
 * headless injection path. */
static void nnk_input_key(SDL_Scancode sc, int down)
{
    int i;
    for (i = 0; i < NNK_NKEYS; i++)
        if (nnk_sc[i] == sc) { nnk_apply_key(i, down); return; }
}

/* ---- SDL event filter: the single owner of keyboard state ---- */
static int nnk_input_filter(void *ud, SDL_Event *ev)
{
    (void)ud;
    if (ev->type == SDL_KEYDOWN || ev->type == SDL_KEYUP) {
        if (ev->key.repeat) return 0;        /* repeat policy: ignore */
        nnk_input_key(ev->key.keysym.scancode, ev->type == SDL_KEYDOWN);
        return 0;                            /* consumed: gxd's built-in keymap
                                              * must never see key events */
    }
    if (ev->type == SDL_WINDOWEVENT &&
        (ev->window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
         ev->window.event == SDL_WINDOWEVENT_MINIMIZED))
        nnk_release_all("focus lost");
    return 1;                                /* quit/mouse/window pass through */
}

/* ---- injection schedule (NNK_INPUT_INJECT) ---- */
static struct { uint64_t step; SDL_Scancode sc; int down; } nnk_inj[128];
static int nnk_inj_n, nnk_inj_next;

static void nnk_input_init(void)
{
    const char *s;
    int i;
    nnk_inited = 1;
    s = getenv("NNK_INPUT_TRACE");
    if (s && s[0]) {
        nnk_in_log = fopen(s, "w");
        if (!nnk_in_log) fprintf(stderr, "nnk_input: cannot open trace %s\n", s);
    }
    for (i = 0; i < NNK_NKEYS; i++) {
        nnk_sc[i] = SDL_GetScancodeFromName(nnk_keymap[i].name);
        if (nnk_sc[i] == SDL_SCANCODE_UNKNOWN)
            fprintf(stderr, "nnk_input: unknown scancode name %s\n", nnk_keymap[i].name);
    }
    if (gxd) gxd_set_keymask(gxd, nnk_mask);
    if (getenv("NNK_GX") && strcmp(getenv("NNK_GX"), "window") == 0) {
        SDL_SetEventFilter(nnk_input_filter, NULL);
        nnk_ilog("install: SDL event filter active (windowed); key events are "
                 "consumed here and written through gxd_set_keymask");
    } else {
        nnk_ilog("install: headless (no SDL filter); injection applies state "
                 "directly through gxd_set_keymask");
    }
    s = getenv("NNK_INPUT_INJECT");
    if (s && s[0]) {
        const char *p = s;
        while (*p && nnk_inj_n < (int)(sizeof nnk_inj / sizeof nnk_inj[0])) {
            char tok[256], name[64], act[8];
            unsigned long long step;
            const char *semi = strchr(p, ';');
            size_t len = semi ? (size_t)(semi - p) : strlen(p);
            SDL_Scancode sc;
            int down;
            if (len >= sizeof tok) len = sizeof tok - 1;
            memcpy(tok, p, len);
            tok[len] = 0;
            p = semi ? semi + 1 : p + strlen(p);
            if (sscanf(tok, "%llu:%63[^:]:%7s", &step, name, act) != 3) {
                fprintf(stderr, "nnk_input: bad inject entry '%s'\n", tok);
                continue;
            }
            sc = SDL_GetScancodeFromName(name);
            down = (strcmp(act, "down") == 0);
            if (sc == SDL_SCANCODE_UNKNOWN) {
                fprintf(stderr, "nnk_input: bad inject scancode '%s'\n", name);
                continue;
            }
            nnk_inj[nnk_inj_n].step = step;
            nnk_inj[nnk_inj_n].sc = sc;
            nnk_inj[nnk_inj_n].down = down;
            nnk_inj_n++;
        }
        nnk_ilog("schedule: %d injection event(s) armed", nnk_inj_n);
    }
    nnk_ilog("map: arrows=D-pad X=A Z=B S=DS-X A=DS-Y Q=L W=R "
             "Return=Start RShift/Backspace=Select");
}

/* ---- frame-timed mask file (NNK_INPUT_FILE) ------------------------------
 * The DQ8 analogue (`DQ8_INPUT_FILE`, DQ8 src/native_hid.c:native_hid_pump):
 * a small text file the host writes; its first line is the desired active-low
 * key mask, re-read once per DISPLAY FRAME and published through the same
 * gxd_set_keymask path a real key event takes.
 *   Step-timed NNK_INPUT_INJECT cannot express "press when the prompt
 * appears": the operator's real taps are frame-timed (he presses when he SEES
 * the screen), which is exactly why a fixed-step injection at 380 M/390 M did
 * not reproduce his session while periodic presses did.  This file is the
 * reproducible form of a human session, and it is what a live operator can be
 * driven by as well.
 * Format: line 1 = strtoul(base 0) mask, low 12 bits =
 *   bit 0-9  KEYINPUT (active low; 0x3FF = nothing pressed)
 *   bit 10   DS X (EXTKEYIN bit0)   bit 11 = DS Y (EXTKEYIN bit1)
 * So B = 0x3FD, A = 0x3FE, Start = 0x3F7, idle = 0x3FF.
 * A missing/unreadable/short file HOLDS the last mask (the host may rewrite it
 * only on transitions); re-reading one short line per frame is cheap. */
static unsigned nnk_file_frame = 0xFFFFFFFFu;
static void nnk_input_file_tick(void)
{
    const char *p = getenv("NNK_INPUT_FILE");
    char buf[64];
    FILE *f;
    unsigned long v;
    if (!p || !p[0]) return;
    if (gxd_frames == nnk_file_frame) return;       /* one read per frame */
    nnk_file_frame = gxd_frames;
    f = fopen(p, "r");
    if (!f) return;
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return; }
    fclose(f);
    v = (unsigned long)strtoul(buf, NULL, 0) & 0xFFFu;
    if ((uint32_t)v == nnk_mask) return;            /* no edge: no state change */
    nnk_mask = (uint32_t)v;
    nnk_extkey = 0x7Fu;
    if (!(nnk_mask & (1u << 10))) nnk_extkey &= ~1u;
    if (!(nnk_mask & (1u << 11))) nnk_extkey &= ~2u;
    if (gxd) gxd_set_keymask(gxd, nnk_mask);
    nnk_ilog("file mask=0x%03X EXTKEYIN=0x%02X frame=%u",
             nnk_mask, nnk_extkey, gxd_frames);
}

static void nnk_input_tick(void)
{
    if (!nnk_inited) nnk_input_init();
    nnk_input_file_tick();
    while (nnk_inj_next < nnk_inj_n && steps >= nnk_inj[nnk_inj_next].step) {
        SDL_Event ev;
        int down = nnk_inj[nnk_inj_next].down;
        SDL_Scancode sc = nnk_inj[nnk_inj_next].sc;
        nnk_inj_next++;
        if (getenv("NNK_GX") && strcmp(getenv("NNK_GX"), "window") == 0) {
            memset(&ev, 0, sizeof ev);
            ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
            ev.key.repeat = 0;
            ev.key.keysym.scancode = sc;
            nnk_ilog("inject SDL_%s scancode=%s frame=%u", down ? "KEYDOWN" : "KEYUP",
                     nnk_keyname(sc) ? nnk_keyname(sc) : "?", gxd_frames);
            SDL_PushEvent(&ev);          /* runs nnk_input_filter on push */
        } else {
            nnk_ilog("inject (headless, no SDL) scancode=%s %s frame=%u",
                     nnk_keyname(sc) ? nnk_keyname(sc) : "?",
                     down ? "down" : "up", gxd_frames);
            nnk_input_key(sc, down);
        }
    }
}

/* io_read16 hook (spliced at the top of io_read16 by split_dispatch.py):
 * returns 1 when the address is handled here.  0x04000130 (KEYINPUT) is only
 * OBSERVED — the existing path serves the value from gxd_io_read, so a trace
 * line of "guest KEYINPUT read" is a true guest read of the merged API's
 * register.  0x04000136 (EXTKEYIN) is served here (melonDS NDS.cpp:3892:
 * read16(0x04000136) = KeyInput >> 16). */
static int nnk_input_ioread16(uint32_t a, uint32_t *out)
{
    if (!nnk_inited) nnk_input_init();
    if (a == 0x04000130u) {
        uint32_t v = gxd ? gxd_io_read(gxd, 0x04000130u, 2) : 0x03FFu;
        nnk_ki_reads++;
        if (v != nnk_ki_last || nnk_ki_reads <= 10 || (nnk_ki_reads % 10000) == 0) {
            nnk_ilog("guest KEYINPUT read #%llu val=0x%04X frame=%u",
                     (unsigned long long)nnk_ki_reads, v, gxd_frames);
            nnk_ki_last = v;
        }
        return 0;                        /* fall through to the normal path */
    }
    if (a == 0x04000136u) {
        nnk_ilog("guest EXTKEYIN read val=0x%02X frame=%u", nnk_extkey, gxd_frames);
        *out = nnk_extkey;
        return 1;
    }
    return 0;
}

#else /* !NNK_GX */

static int nnk_input_ioread16(uint32_t a, uint32_t *out)
{ (void)a; (void)out; return 0; }
static void nnk_input_tick(void) { }

#endif /* NNK_GX */

/* Phase-4X: first-dispatch probe counter for the 0x029xxxxx band.  Declared
 * here (included before run()) so the run-loop probe injected by
 * tools/split_dispatch.py has a home outside the prelude injects. */
static unsigned p4x_band_log;

#endif /* NNK_INPUT_INC */
