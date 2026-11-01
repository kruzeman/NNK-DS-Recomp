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
 *   NNK_INPUT_FILE=<path>    frame-timed input (DQ8 `DQ8_INPUT_FILE`
 *       analogue).  Either the legacy single value (whole file = one mask,
 *       constant) or a SCRIPT: one `<frame> <mask>` entry per line (frame =
 *       decimal DISPLAY-frame index, mask hex, `#` comments / blank lines,
 *       `end` stops parsing; EOF holds the last mask).  An entry's mask is
 *       applied when the display-frame counter reaches its frame and HELD
 *       until the next entry.  Parsed once per file (mtime/size) change.
 *   NNK_INPUT_RECORD=<path>  write the EFFECTIVE guest mask as a replayable
 *       script: one `<frame> 0x%03X` line per mask transition, flushed
 *       immediately, with a 3-line header (title, binary, bit legend).
 *       Replay the produced file verbatim through NNK_INPUT_FILE.
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
#include <sys/stat.h>
#include <stdlib.h>

/* ---- state ---- */
static FILE *nnk_in_log;                 /* NNK_INPUT_TRACE, NULL = quiet */
static uint32_t nnk_mask = 0xFFF;        /* active-low SetKeyMask shape:
                                          * bits 0-9 keys, 10 = DS X, 11 = DS Y */
static uint32_t nnk_extkey = 0x7F;       /* EXTKEYIN low byte (X/Y pressed
                                          * clear bits 0/1) */
static int nnk_inited;
static uint64_t nnk_ki_reads;            /* guest KEYINPUT reads seen */
static uint32_t nnk_ki_last = 0xFFFFFFFFu;

static void nnk_input_atexit(void);       /* defined with the recorder below */

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
    atexit(nnk_input_atexit);
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
 * a small text file the host writes; its content is the desired active-low
 * key mask, re-read once per DISPLAY FRAME and published through the same
 * gxd_set_keymask path a real key event takes.
 *   Step-timed NNK_INPUT_INJECT cannot express "press when the prompt
 * appears": the operator's real taps are frame-timed (he presses when he SEES
 * the screen), which is exactly why a fixed-step injection at 380 M/390 M did
 * not reproduce his session while periodic presses did.  This file is the
 * reproducible form of a human session, and it is what a live operator can be
 * driven by as well.
 * Two accepted forms (operator's spec):
 *   1) legacy: the whole file is ONE number -> constant mask (DQ8-compatible);
 *   2) SCRIPT: one `<frame> <mask>` entry per line, frame = decimal display
 *      frame, mask = hex (strtoul base 0), `#` comments and blank lines,
 *      `end` stops parsing, EOF/end HOLDS the last mask.  Entries are applied
 *      in file order as the display-frame counter passes them.
 * Mask bits, KEYINPUT active-low (idle 0x3FF):
 *   bit 0-9 = A B Select Start Right Left Up Down R L (cleared bit = pressed)
 *   bit 10 = DS X, bit 11 = DS Y (EXTKEYIN bits 0/1)
 * So B = 0x3FD, A = 0x3FE, Start = 0x3F7, idle = 0x3FF.
 * The file is re-parsed only when its mtime/size changes; a missing file HOLDS
 * the last mask (the host may write it only on transitions). */
#define NNK_SCRIPT_MAX 512
static struct { unsigned frame; uint32_t mask; } nnk_script[NNK_SCRIPT_MAX];
static int nnk_script_n;          /* entries parsed */
static int nnk_script_next;       /* next entry to apply */
static int nnk_script_mode;       /* 1 = "<frame> <mask>" script, 0 = constant */
static int nnk_file_parsed;
static long nnk_file_mtime;
static long long nnk_file_size;
static unsigned nnk_file_frame = 0xFFFFFFFFu;
static unsigned nnk_script_applied;

/* Publish an effective mask exactly like a real key edge does. */
static void nnk_mask_publish(uint32_t v, const char *why)
{
    v &= 0xFFFu;
    /* Two accepted mask spaces: the runtime's 12-bit form (bits 10/11 = DS X/Y,
     * active-low like the keys) and the operator's KEYINPUT-only script form
     * (< 0x400: idle 0x3FF, B 0x3FD, A 0x3FE).  A bare KEYINPUT mask must not
     * press DS X/Y, so the X/Y bits default to "released". */
    if ((v & 0xC00u) == 0) v |= 0xC00u;
    if (v == nnk_mask) return;
    nnk_mask = v;
    nnk_extkey = 0x7Fu;
    if (!(nnk_mask & (1u << 10))) nnk_extkey &= ~1u;
    if (!(nnk_mask & (1u << 11))) nnk_extkey &= ~2u;
    if (gxd) gxd_set_keymask(gxd, nnk_mask);
    nnk_ilog("%s mask=0x%03X EXTKEYIN=0x%02X frame=%u",
             why, nnk_mask, nnk_extkey, gxd_frames);
}

static void nnk_script_parse(FILE *f, const char *path)
{
    char line[256];
    unsigned last_frame = 0;
    int n = 0, lineno = 0, have_single = 0;
    nnk_script_mode = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *t1, *t2, *e1, *e2;
        unsigned long fr, mk;
        lineno++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '\r' || *p == '\n' || *p == '#') continue;
        if (!strncmp(p, "end", 3) &&
            (p[3] == 0 || p[3] == '\r' || p[3] == '\n' || p[3] == ' ' || p[3] == '\t'))
            break;
        t1 = strtok(p, " \t\r\n");
        if (!t1) continue;
        t2 = strtok(NULL, " \t\r\n");
        fr = strtoul(t1, &e1, 0);
        if (e1 == t1) {
            fprintf(stderr, "nnk_input: %s:%d: bad frame '%s' "
                    "(want '<frame> <mask>' or a single mask)\n", path, lineno, t1);
            continue;
        }
        if (!t2) {
            /* legacy form: the whole file / this lone line is one mask */
            if (!have_single && n == 0) {
                have_single = 1;
                nnk_script[n].frame = 0;
                nnk_script[n].mask = (uint32_t)fr & 0xFFFu;
                n++;
            }
            continue;
        }
        mk = strtoul(t2, &e2, 0);
        if (e2 == t2) {
            fprintf(stderr, "nnk_input: %s:%d: bad mask '%s'\n", path, lineno, t2);
            continue;
        }
        if (n >= NNK_SCRIPT_MAX) {
            fprintf(stderr, "nnk_input: %s:%d: script full (max %d entries)\n",
                    path, lineno, NNK_SCRIPT_MAX);
            break;
        }
        if (nnk_script_mode && (unsigned)fr < last_frame)
            fprintf(stderr, "nnk_input: %s:%d: frame %lu goes backwards "
                    "(previous %u); applied in file order anyway\n",
                    path, lineno, fr, last_frame);
        nnk_script[n].frame = (unsigned)fr;
        nnk_script[n].mask = (uint32_t)mk & 0xFFFu;
        last_frame = (unsigned)fr;
        n++;
        nnk_script_mode = 1;
    }
    nnk_script_n = n;
}

/* (Re)load the file when its identity/mtime/size changed.  A reload during a
 * run does not replay entries already in the past (the guest already saw
 * them); it only arms the future ones. */
static void nnk_input_file_load(const char *path)
{
    struct stat st;
    FILE *f;
    if (stat(path, &st) != 0) return;
    if (nnk_file_parsed && st.st_mtime == nnk_file_mtime &&
        (long long)st.st_size == nnk_file_size) return;
    f = fopen(path, "r");
    if (!f) return;
    nnk_file_mtime = st.st_mtime;
    nnk_file_size = (long long)st.st_size;
    nnk_script_parse(f, path);
    fclose(f);
    nnk_script_next = 0;
    if (nnk_file_parsed) {
        while (nnk_script_next < nnk_script_n &&
               nnk_script[nnk_script_next].frame <= gxd_frames)
            nnk_script_next++;
    }
    nnk_file_parsed = 1;
    nnk_ilog("file: loaded %d entr%s (%s) at frame %u",
             nnk_script_n, nnk_script_n == 1 ? "y" : "ies",
             nnk_script_mode ? "script" : "constant mask", gxd_frames);
}

static void nnk_input_file_tick(void)
{
    const char *p = getenv("NNK_INPUT_FILE");
    if (!p || !p[0]) return;
    if (gxd_frames == nnk_file_frame) return;       /* one read per frame */
    nnk_file_frame = gxd_frames;
    nnk_input_file_load(p);
    while (nnk_script_next < nnk_script_n &&
           gxd_frames >= nnk_script[nnk_script_next].frame) {
        unsigned fr = nnk_script[nnk_script_next].frame;
        uint32_t m = nnk_script[nnk_script_next].mask;
        uint32_t before = nnk_mask;
        uint32_t pressed = before & ~m & 0xFFFu;
        uint32_t released = ~before & m & 0xFFFu;
        nnk_script_next++;
        nnk_script_applied++;
        nnk_ilog("script applied frame=%u mask=0x%03X entry=%d/%d "
                 "pressed=0x%03X released=0x%03X",
                 fr, m, nnk_script_next, nnk_script_n, pressed, released);
        nnk_mask_publish(m, "file");
    }
    /* a constant mask (legacy single value) is applied once, at load time */
    if (!nnk_script_mode && nnk_script_n == 1 && nnk_script_applied == 0) {
        nnk_script_applied++;
        nnk_mask_publish(nnk_script[0].mask, "file");
    }
}

/* ---- input recorder (NNK_INPUT_RECORD) ----------------------------------
 * At every display frame, if the EFFECTIVE guest mask (after SDL keys +
 * NNK_INPUT_FILE script + NNK_INPUT_INJECT, i.e. exactly what was pushed to
 * gxd_set_keymask) differs from the last recorded one, append
 * `<frame> 0x%03X` and flush, so a crash still leaves a replayable script.
 * The produced file is accepted verbatim by NNK_INPUT_FILE. */
static FILE *nnk_rec;
static int nnk_rec_open;
static uint32_t nnk_rec_last = 0xFFFFFFFFu;
static unsigned nnk_rec_frame = 0xFFFFFFFFu;
static unsigned nnk_rec_entries;
static unsigned nnk_rec_first_frame;

static void nnk_input_record_tick(void)
{
    const char *p = getenv("NNK_INPUT_RECORD");
    if (!p || !p[0]) return;
    if (gxd_frames == nnk_rec_frame) return;        /* one line per frame max */
    nnk_rec_frame = gxd_frames;
    if (!nnk_rec_open) {
        nnk_rec_open = 1;
        nnk_rec = fopen(p, "w");
        if (!nnk_rec) {
            fprintf(stderr, "nnk_input: cannot open record %s\n", p);
            return;
        }
        fprintf(nnk_rec, "# Ni no Kuni DS input recording (NNK_INPUT_RECORD) "
                "- replay with NNK_INPUT_FILE\n");
        fprintf(nnk_rec, "# binary md5: n/a (recorded from a live run); "
                "frame = DISPLAY frame\n");
        fprintf(nnk_rec, "# KEYINPUT active-low, idle 0x3FF; A=bit0 B=bit1 "
                "Select=bit2 Start=bit3 Right=bit4 Left=bit5 Up=bit6 "
                "Down=bit7 R=bit8 L=bit9; X=bit10 Y=bit11\n");
        fflush(nnk_rec);
    }
    if (nnk_mask == nnk_rec_last) return;
    nnk_rec_last = nnk_mask;
    if (!nnk_rec_entries) nnk_rec_first_frame = gxd_frames;
    nnk_rec_entries++;
    {
        /* Write the operator's KEYINPUT space when X/Y are released (idle
         * 0x3FF), the 12-bit form only when a DS X/Y bit is actually held. */
        unsigned v = ((nnk_mask & 0xC00u) == 0xC00u) ? (nnk_mask & 0x3FFu)
                                                     : (nnk_mask & 0xFFFu);
        fprintf(nnk_rec, "%u 0x%03X\n", gxd_frames, v);
        fflush(nnk_rec);
        nnk_ilog("record frame=%u mask=0x%03X (entry %u)",
                 gxd_frames, v, nnk_rec_entries);
    }
}

static void nnk_input_atexit(void)
{
    if (getenv("NNK_INPUT_FILE") || nnk_script_applied || nnk_rec_entries) {
        fprintf(stderr, "nnk_input: summary script entries=%d applied=%u "
                "last_frame=%u recorded=%u entries",
                nnk_script_n, nnk_script_applied, gxd_frames, nnk_rec_entries);
        if (nnk_rec_entries)
            fprintf(stderr, " first_recorded_frame=%u", nnk_rec_first_frame);
        fputc('\n', stderr);
        fflush(stderr);
        nnk_ilog("summary: script=%d applied=%u last_frame=%u recorded=%u",
                 nnk_script_n, nnk_script_applied, gxd_frames, nnk_rec_entries);
    }
    if (nnk_rec) { fclose(nnk_rec); nnk_rec = NULL; }
    if (nnk_in_log) { fflush(nnk_in_log); }
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
    nnk_input_record_tick();
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
