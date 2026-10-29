/* gx_2d.c — DS 2D engine + screen compositor (see gx_2d.h).
 *
 * Faithful port of the melonDS software 2D renderer's scanline model:
 *   GPU2D_Soft.cpp  DrawScanline_BGOBJ, DrawBG_Text/Affine/Extended/Large,
 *                   DrawBG_3D, DrawSprites, DrawSprite_Normal/Rotscale,
 *                   InterleaveSprites, ApplySpriteMosaicX, ColorComposite
 *   GPU_ColorOp.h   ColorBlend4/5, ColorBrightnessUp/Down
 *   GPU2D.cpp       CalculateWindowMask, UpdateWindows,
 *                   UpdateRegistersPreDraw/PostDraw (latch/mosaic/ref
 *                   timing), register write semantics
 *   GPU_Soft.cpp    DrawScanlineA/B (display modes), ApplyMasterBrightness,
 *                   ExpandColor, screens-disabled handling
 * The BGOBJLine pixel format is melonDS's: r|g<<8|b<<16|flag<<24 with 6-bit
 * rgb and flag bits (0x80 semitransparent sprite, 0x40 3D layer, 0x10/0x01
 * OBJ/BG0 slot, low nibble = sprite alpha for bitmap sprites).  All math is
 * integer; output bytes match melonDS framebuffers exactly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_2d.h"

/* ---- OBJLine flags (melonDS GPU2D_Soft.h enum) ---- */
#define OBJ_STANDARD_PAL   (1u << 12)
#define OBJ_DIRECT_COLOR   (1u << 15)
#define OBJ_BGPRIO_MASK    (0x3u << 16)
#define OBJ_IS_OPAQUE      (1u << 18)
#define OBJ_OPA_PRIO_MASK  (OBJ_BGPRIO_MASK | OBJ_IS_OPAQUE)
#define OBJ_IS_SPRITE      (1u << 19)
#define OBJ_MOSAIC_FLAG    (1u << 20)

static inline uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static inline uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ---- color ops (melonDS GPU_ColorOp.h, verbatim) ---- */

static uint32_t color_blend4(uint32_t val1, uint32_t val2, uint32_t eva,
                             uint32_t evb)
{
    uint32_t r = (((val1 & 0x00003F) * eva) + ((val2 & 0x00003F) * evb) + 0x000008) >> 4;
    uint32_t g = ((((val1 & 0x003F00) * eva) + ((val2 & 0x003F00) * evb) + 0x000800) >> 4) & 0x007F00;
    uint32_t b = ((((val1 & 0x3F0000) * eva) + ((val2 & 0x3F0000) * evb) + 0x080000) >> 4) & 0x7F0000;

    if (r > 0x00003F) r = 0x00003F;
    if (g > 0x003F00) g = 0x003F00;
    if (b > 0x3F0000) b = 0x3F0000;

    return r | g | b | 0xFF000000;
}

static uint32_t color_blend5(uint32_t val1, uint32_t val2)
{
    uint32_t eva = ((val1 >> 24) & 0x1F) + 1;
    uint32_t evb = 32 - eva;

    if (eva == 32) return val1;

    uint32_t r = (((val1 & 0x00003F) * eva) + ((val2 & 0x00003F) * evb) + 0x000010) >> 5;
    uint32_t g = ((((val1 & 0x003F00) * eva) + ((val2 & 0x003F00) * evb) + 0x001000) >> 5) & 0x007F00;
    uint32_t b = ((((val1 & 0x3F0000) * eva) + ((val2 & 0x3F0000) * evb) + 0x100000) >> 5) & 0x7F0000;

    if (r > 0x00003F) r = 0x00003F;
    if (g > 0x003F00) g = 0x003F00;
    if (b > 0x3F0000) b = 0x3F0000;

    return r | g | b | 0xFF000000;
}

static uint32_t color_brightness_up(uint32_t val, uint32_t factor, uint32_t bias)
{
    uint32_t rb = val & 0x3F003F;
    uint32_t g = val & 0x003F00;

    rb += (((((0x3F003F - rb) * factor) + (bias * 0x010001)) >> 4) & 0x3F003F);
    g += (((((0x003F00 - g) * factor) + (bias * 0x000100)) >> 4) & 0x003F00);

    return rb | g | 0xFF000000;
}

static uint32_t color_brightness_down(uint32_t val, uint32_t factor, uint32_t bias)
{
    uint32_t rb = val & 0x3F003F;
    uint32_t g = val & 0x003F00;

    rb -= ((((rb * factor) + (bias * 0x010001)) >> 4) & 0x3F003F);
    g -= ((((g * factor) + (bias * 0x000100)) >> 4) & 0x003F00);

    return rb | g | 0xFF000000;
}

/* ---- per-engine frame context ---- */

typedef struct {
    const Gx2DState *st;
    const Gx2DEngine *e;
    int num;                     /* 0 = A, 1 = B */
    int vcount;                  /* scanline being processed */

    /* latched per-scanline regs (melonDS UpdateRegistersPreDraw) */
    uint8_t layer_enable, obj_enable, forced_blank;

    /* rotscale ref counters (BGXRef/BGYRef running value) */
    int32_t bgxref[2], bgyref[2];
    int32_t bgxrefint[2], bgyrefint[2];

    /* mosaic counters */
    uint8_t bgmosaic_y, bgmosaic_ymax, bgmosaic_latch;
    uint32_t bgmosaic_line;
    uint8_t objmosaic_y, objmosaic_latch;
    uint32_t objmosaic_line;

    /* window latches */
    uint8_t win0active, win1active;

    /* per-line buffers (melonDS BGOBJLine/OBJLine/WindowMask/OBJWindow) */
    uint32_t bgobjline[512];
    uint8_t windowmask[256];
    uint32_t objline[256];
    uint8_t objwindow[256];
    uint32_t numsprites;

    uint8_t bgxmosaictable[16][256];

    /* output scanline (6-bit rgb, pre master brightness) */
    uint32_t outline[256];
} Gx2DEng;

static void eng_init(Gx2DEng *en, const Gx2DState *st, int num)
{
    memset(en, 0, sizeof(*en));
    en->st = st;
    en->e = &st->eng[num];
    en->num = num;
    for (int m = 0; m < 16; m++)
        for (int x = 0; x < 256; x++)
            en->bgxmosaictable[m][x] = (uint8_t)(x % (m + 1));
}

/* ---- register latch / mosaic / ref timing (melonDS GPU2D.cpp) ---- */

static void eng_pre_draw(Gx2DEng *en, int reset)
{
    const Gx2DEngine *e = en->e;
    /* static per-frame state: latch history collapses to the current value
     * (melonDS DispCntLatch[3] 2-scanline delay) */
    en->layer_enable = (uint8_t)((e->dispcnt >> 8) & 0x1F);
    en->obj_enable = (uint8_t)((e->dispcnt >> 12) & 0x1);
    en->forced_blank = (uint8_t)((e->dispcnt >> 7) & 0x1);

    if (en->bgmosaic_latch)
        en->bgmosaic_line = reset ? 262 : (uint32_t)en->vcount;

    for (int i = 0; i < 2; i++)
    {
        if (!(e->bgcnt[2 + i] & (1 << 6)) || en->bgmosaic_latch)
        {
            en->bgxrefint[i] = en->bgxref[i];
            en->bgyrefint[i] = en->bgyref[i];
        }
    }

    if (e->dispcnt & (1u << 12))
    {
        if (reset || (en->objmosaic_y == e->objmosaic[1]))
        {
            en->objmosaic_y = 0;
            en->objmosaic_latch = 1;
        }
        else
        {
            en->objmosaic_y = (uint8_t)((en->objmosaic_y + 1) & 0xF);
            en->objmosaic_latch = 0;
        }
    }

    if (en->objmosaic_latch)
        en->objmosaic_line = reset ? 0 : (uint32_t)(en->vcount + 1);
}

static void eng_post_draw(Gx2DEng *en, int reset)
{
    const Gx2DEngine *e = en->e;
    if (reset)
    {
        en->bgmosaic_ymax = e->bgmosaic[1];
        en->bgmosaic_y = 0;
        en->bgmosaic_latch = 1;
    }
    else
    {
        if (en->bgmosaic_y == en->bgmosaic_ymax)
        {
            en->bgmosaic_ymax = e->bgmosaic[1];
            en->bgmosaic_y = 0;
            en->bgmosaic_latch = 1;
        }
        else
        {
            en->bgmosaic_y = (uint8_t)((en->bgmosaic_y + 1) & 0xF);
            en->bgmosaic_latch = 0;
        }
    }

    for (int i = 0; i < 2; i++)
    {
        if (!(en->layer_enable & (4u << i)))
            continue;
        if (reset)
        {
            en->bgxref[i] = e->bgxrefreload[i];
            en->bgyref[i] = e->bgyrefreload[i];
        }
        else
        {
            en->bgxref[i] += e->bgrotb[i];
            en->bgyref[i] += e->bgrotd[i];
        }
    }
}

static void eng_update_windows(Gx2DEng *en, int line)
{
    const Gx2DEngine *e = en->e;
    line &= 0xFF;
    if (line == e->win0coords[3])      en->win0active &= (uint8_t)~0x1;
    else if (line == e->win0coords[2]) en->win0active |= 0x1;
    if (line == e->win1coords[3])      en->win1active &= (uint8_t)~0x1;
    else if (line == e->win1coords[2]) en->win1active |= 0x1;
}

static void eng_calculate_window_mask(Gx2DEng *en)
{
    const Gx2DEngine *e = en->e;
    for (int i = 0; i < 256; i++)
        en->windowmask[i] = e->wincnt[2]; /* window outside */

    if (e->dispcnt & (1u << 15))
    {
        /* OBJ window */
        for (int i = 0; i < 256; i++)
            if (en->objwindow[i])
                en->windowmask[i] = e->wincnt[3];
    }

    if (e->dispcnt & (1u << 14))
    {
        /* window 1 */
        uint8_t x1 = e->win1coords[0], x2 = e->win1coords[1];
        for (int i = 0; i < 256; i++)
        {
            if (i == x2)      en->win1active &= (uint8_t)~0x2;
            else if (i == x1) en->win1active |= 0x2;
            if (en->win1active == 0x3) en->windowmask[i] = e->wincnt[1];
        }
    }

    if (e->dispcnt & (1u << 13))
    {
        /* window 0 */
        uint8_t x1 = e->win0coords[0], x2 = e->win0coords[1];
        for (int i = 0; i < 256; i++)
        {
            if (i == x2)      en->win0active &= (uint8_t)~0x2;
            else if (i == x1) en->win0active |= 0x2;
            if (en->win0active == 0x3) en->windowmask[i] = e->wincnt[0];
        }
    }
}

/* ---- pixel plot / backdrop / 3D layer ---- */

static void draw_pixel(uint32_t *dst, uint16_t color, uint32_t flag)
{
    /* melonDS: bit15 of palette colors is the bottom green bit */
    uint8_t r = (uint8_t)((color & 0x001F) << 1);
    uint8_t g = (uint8_t)(((color & 0x03E0) >> 4) | ((color & 0x8000) >> 15));
    uint8_t b = (uint8_t)((color & 0x7C00) >> 9);

    *(dst + 256) = *dst;
    *dst = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | flag;
}

static void draw_bg_3d(Gx2DEng *en, const uint32_t *fb3dline)
{
    for (int i = 0; i < 256; i++)
    {
        uint32_t c = fb3dline[i];
        if ((c >> 24) == 0) continue;
        if (!(en->windowmask[i] & 0x01)) continue;
        en->bgobjline[i + 256] = en->bgobjline[i];
        en->bgobjline[i] = c | 0x40000000;
    }
}

/* ---- BG renderers (melonDS GPU2D_Soft.cpp DrawBG_*) ---- */

static void draw_bg_text(Gx2DEng *en, int line, int bgnum, int mosaic)
{
    const Gx2DEngine *e = en->e;
    const Gx2DState *st = en->st;
    uint16_t bgcnt = e->bgcnt[bgnum];
    uint32_t tilesetaddr, tilemapaddr;
    const uint16_t *pal;
    int extpal, extpalslot = 0;
    uint16_t xoff = e->bgxpos[bgnum];
    uint16_t yoff = e->bgypos[bgnum];

    if (bgcnt & (1 << 6))
        yoff = (uint16_t)(yoff + en->bgmosaic_line);
    else
        yoff = (uint16_t)(yoff + line);

    uint32_t widexmask = (bgcnt & (1 << 14)) ? 0x100 : 0;

    extpal = (e->dispcnt & (1u << 30)) != 0;
    if (extpal) extpalslot = ((bgnum < 2) && (bgcnt & 0x2000)) ? (2 + bgnum) : bgnum;

    const uint8_t *bgvram = st->bgvram[en->num];
    uint32_t bgvrammask = en->num ? 0x1FFFF : 0x7FFFF;
    if (en->num)
    {
        tilesetaddr = (uint32_t)((bgcnt & 0x003C) << 12);
        tilemapaddr = (uint32_t)((bgcnt & 0x1F00) << 3);
        pal = (const uint16_t *)(st->palette + 0x400);
    }
    else
    {
        tilesetaddr = (uint32_t)(((e->dispcnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12));
        tilemapaddr = (uint32_t)(((e->dispcnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3));
        pal = (const uint16_t *)st->palette;
    }

    /* adjust Y position in tilemap */
    if (bgcnt & (1 << 15))
    {
        tilemapaddr += (uint32_t)((yoff & 0x1F8) << 3);
        if (bgcnt & (1 << 14))
            tilemapaddr += (uint32_t)((yoff & 0x100) << 3);
    }
    else
        tilemapaddr += (uint32_t)((yoff & 0xF8) << 3);

    uint16_t curtile = 0;
    const uint16_t *curpal = pal;
    uint32_t pixelsaddr = 0;
    uint8_t color;
    uint32_t lastxpos = 0;
    const uint8_t *xmt = en->bgxmosaictable[e->bgmosaic[0]];

    if (bgcnt & (1 << 7))
    {
        /* 256-color */
        if ((xoff & 0x7) || mosaic)
        {
            curtile = rd16(&bgvram[(tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3)) & bgvrammask]);
            if (extpal) curpal = (const uint16_t *)(st->bgextpal[en->num] +
                (uint32_t)extpalslot * 8192 + (uint32_t)(curtile >> 12) * 512);
            else        curpal = pal;
            pixelsaddr = tilesetaddr + ((uint32_t)(curtile & 0x03FF) << 6)
                + (uint32_t)(((curtile & (1 << 11)) ? (7 - (yoff & 0x7)) : (yoff & 0x7)) << 3);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            uint32_t xpos;
            if (mosaic) xpos = xoff - xmt[i];
            else        xpos = xoff;
            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                curtile = rd16(&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask]);
                if (extpal) curpal = (const uint16_t *)(st->bgextpal[en->num] +
                    (uint32_t)extpalslot * 8192 + (uint32_t)(curtile >> 12) * 512);
                else        curpal = pal;
                pixelsaddr = tilesetaddr + ((uint32_t)(curtile & 0x03FF) << 6)
                    + (uint32_t)(((curtile & (1 << 11)) ? (7 - (yoff & 0x7)) : (yoff & 0x7)) << 3);
                if (mosaic) lastxpos = xpos;
            }

            if (en->windowmask[i] & (1 << bgnum))
            {
                uint32_t tilexoff = (curtile & (1 << 10)) ? (7 - (xpos & 0x7)) : (xpos & 0x7);
                color = bgvram[(pixelsaddr + tilexoff) & bgvrammask];
                if (color)
                    draw_pixel(&en->bgobjline[i], curpal[color], 0x01000000u << bgnum);
            }
            xoff++;
        }
    }
    else
    {
        /* 16-color */
        if ((xoff & 0x7) || mosaic)
        {
            curtile = rd16(&bgvram[(tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3)) & bgvrammask]);
            curpal = pal + ((curtile & 0xF000) >> 8);
            pixelsaddr = tilesetaddr + ((uint32_t)(curtile & 0x03FF) << 5)
                + (uint32_t)(((curtile & (1 << 11)) ? (7 - (yoff & 0x7)) : (yoff & 0x7)) << 2);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            uint32_t xpos;
            if (mosaic) xpos = xoff - xmt[i];
            else        xpos = xoff;
            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                curtile = rd16(&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask]);
                curpal = pal + ((curtile & 0xF000) >> 8);
                pixelsaddr = tilesetaddr + ((uint32_t)(curtile & 0x03FF) << 5)
                    + (uint32_t)(((curtile & (1 << 11)) ? (7 - (yoff & 0x7)) : (yoff & 0x7)) << 2);
                if (mosaic) lastxpos = xpos;
            }

            if (en->windowmask[i] & (1 << bgnum))
            {
                uint32_t tilexoff = (curtile & (1 << 10)) ? (7 - (xpos & 0x7)) : (xpos & 0x7);
                if (tilexoff & 0x1)
                    color = (uint8_t)(bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] >> 4);
                else
                    color = (uint8_t)(bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] & 0x0F);
                if (color)
                    draw_pixel(&en->bgobjline[i], curpal[color], 0x01000000u << bgnum);
            }
            xoff++;
        }
    }
}

static void draw_bg_affine(Gx2DEng *en, int line, int bgnum, int mosaic)
{
    (void)line;
    const Gx2DEngine *e = en->e;
    const Gx2DState *st = en->st;
    uint16_t bgcnt = e->bgcnt[bgnum];
    uint32_t tilesetaddr, tilemapaddr;
    const uint16_t *pal;
    uint32_t coordmask, yshift;

    switch ((bgcnt >> 14) & 0x3)
    {
        case 0: coordmask = 0x07800; yshift = 7; break;
        case 1: coordmask = 0x0F800; yshift = 8; break;
        case 2: coordmask = 0x1F800; yshift = 9; break;
        default: coordmask = 0x3F800; yshift = 10; break;
    }

    uint32_t overflowmask;
    if (bgcnt & (1 << 13)) overflowmask = 0;
    else                   overflowmask = ~(coordmask | 0x7FF);

    int16_t rotA = e->bgrota[bgnum - 2];
    int16_t rotC = e->bgrotc[bgnum - 2];
    int32_t rotX = en->bgxrefint[bgnum - 2];
    int32_t rotY = en->bgyrefint[bgnum - 2];

    const uint8_t *bgvram = st->bgvram[en->num];
    uint32_t bgvrammask = en->num ? 0x1FFFF : 0x7FFFF;
    if (en->num)
    {
        tilesetaddr = (uint32_t)((bgcnt & 0x003C) << 12);
        tilemapaddr = (uint32_t)((bgcnt & 0x1F00) << 3);
        pal = (const uint16_t *)(st->palette + 0x400);
    }
    else
    {
        tilesetaddr = (uint32_t)(((e->dispcnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12));
        tilemapaddr = (uint32_t)(((e->dispcnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3));
        pal = (const uint16_t *)st->palette;
    }

    yshift -= 3;

    for (int i = 0; i < 256; i++)
    {
        if (en->windowmask[i] & (1 << bgnum))
        {
            int32_t finalX, finalY;
            if (mosaic)
            {
                int im = en->bgxmosaictable[e->bgmosaic[0]][i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if (!((finalX | finalY) & (int32_t)overflowmask))
            {
                uint16_t curtile = bgvram[(tilemapaddr + ((((uint32_t)finalY & coordmask) >> 11) << yshift) + (((uint32_t)finalX & coordmask) >> 11)) & bgvrammask];
                uint32_t tilexoff = ((uint32_t)finalX >> 8) & 0x7;
                uint32_t tileyoff = ((uint32_t)finalY >> 8) & 0x7;
                uint8_t color = bgvram[(tilesetaddr + ((uint32_t)curtile << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];
                if (color)
                    draw_pixel(&en->bgobjline[i], pal[color], 0x01000000u << bgnum);
            }
        }
        rotX += rotA;
        rotY += rotC;
    }
}

static void draw_bg_extended(Gx2DEng *en, int line, int bgnum, int mosaic)
{
    (void)line;
    const Gx2DEngine *e = en->e;
    const Gx2DState *st = en->st;
    uint16_t bgcnt = e->bgcnt[bgnum];
    uint32_t tilesetaddr = 0, tilemapaddr = 0;
    const uint16_t *pal = NULL;
    int extpal = (e->dispcnt & (1u << 30)) != 0;

    const uint8_t *bgvram = st->bgvram[en->num];
    uint32_t bgvrammask = en->num ? 0x1FFFF : 0x7FFFF;

    int16_t rotA = e->bgrota[bgnum - 2];
    int16_t rotC = e->bgrotc[bgnum - 2];
    int32_t rotX = en->bgxrefint[bgnum - 2];
    int32_t rotY = en->bgyrefint[bgnum - 2];

    if (bgcnt & (1 << 7))
    {
        /* bitmap modes */
        uint32_t xmask, ymask, yshift;
        switch ((bgcnt >> 14) & 0x3)
        {
            case 0: xmask = 0x07FFF; ymask = 0x07FFF; yshift = 7; break;
            case 1: xmask = 0x0FFFF; ymask = 0x0FFFF; yshift = 8; break;
            case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
            default: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
        }

        uint32_t ofxmask, ofymask;
        if (bgcnt & (1 << 13))
        {
            ofxmask = 0;
            ofymask = 0;
        }
        else
        {
            ofxmask = ~xmask;
            ofymask = ~ymask;
        }

        tilemapaddr = (uint32_t)((bgcnt & 0x1F00) << 6);

        if (bgcnt & (1 << 2))
        {
            /* direct color bitmap */
            for (int i = 0; i < 256; i++)
            {
                if (en->windowmask[i] & (1 << bgnum))
                {
                    int32_t finalX, finalY;
                    if (mosaic)
                    {
                        int im = en->bgxmosaictable[e->bgmosaic[0]][i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!((uint32_t)finalX & ofxmask) && !((uint32_t)finalY & ofymask))
                    {
                        uint16_t color = rd16(&bgvram[(tilemapaddr + ((((((uint32_t)finalY & ymask) >> 8) << yshift) + (((uint32_t)finalX & xmask) >> 8)) << 1)) & bgvrammask]);
                        if (color & 0x8000)
                            draw_pixel(&en->bgobjline[i], color & 0x7FFF, 0x01000000u << bgnum);
                    }
                }
                rotX += rotA;
                rotY += rotC;
            }
        }
        else
        {
            /* 256-color bitmap */
            if (en->num) pal = (const uint16_t *)(st->palette + 0x400);
            else           pal = (const uint16_t *)st->palette;

            for (int i = 0; i < 256; i++)
            {
                if (en->windowmask[i] & (1 << bgnum))
                {
                    int32_t finalX, finalY;
                    if (mosaic)
                    {
                        int im = en->bgxmosaictable[e->bgmosaic[0]][i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!((uint32_t)finalX & ofxmask) && !((uint32_t)finalY & ofymask))
                    {
                        uint8_t color = bgvram[(tilemapaddr + ((((uint32_t)finalY & ymask) >> 8) << yshift) + (((uint32_t)finalX & xmask) >> 8)) & bgvrammask];
                        if (color)
                            draw_pixel(&en->bgobjline[i], pal[color], 0x01000000u << bgnum);
                    }
                }
                rotX += rotA;
                rotY += rotC;
            }
        }
    }
    else
    {
        /* mixed affine/text mode */
        uint32_t coordmask, yshift;
        switch ((bgcnt >> 14) & 0x3)
        {
            case 0: coordmask = 0x07800; yshift = 7; break;
            case 1: coordmask = 0x0F800; yshift = 8; break;
            case 2: coordmask = 0x1F800; yshift = 9; break;
            default: coordmask = 0x3F800; yshift = 10; break;
        }

        uint32_t overflowmask;
        if (bgcnt & (1 << 13)) overflowmask = 0;
        else                   overflowmask = ~(coordmask | 0x7FF);

        if (en->num)
        {
            tilesetaddr = (uint32_t)((bgcnt & 0x003C) << 12);
            tilemapaddr = (uint32_t)((bgcnt & 0x1F00) << 3);
            pal = (const uint16_t *)(st->palette + 0x400);
        }
        else
        {
            tilesetaddr = (uint32_t)(((e->dispcnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12));
            tilemapaddr = (uint32_t)(((e->dispcnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3));
            pal = (const uint16_t *)st->palette;
        }

        yshift -= 3;

        for (int i = 0; i < 256; i++)
        {
            if (en->windowmask[i] & (1 << bgnum))
            {
                int32_t finalX, finalY;
                if (mosaic)
                {
                    int im = en->bgxmosaictable[e->bgmosaic[0]][i];
                    finalX = rotX - (im * rotA);
                    finalY = rotY - (im * rotC);
                }
                else
                {
                    finalX = rotX;
                    finalY = rotY;
                }

                if (!((finalX | finalY) & (int32_t)overflowmask))
                {
                    uint16_t curtile = rd16(&bgvram[(tilemapaddr + ((((((uint32_t)finalY & coordmask) >> 11) << yshift) + (((uint32_t)finalX & coordmask) >> 11)) << 1)) & bgvrammask]);
                    const uint16_t *curpal;
                    if (extpal) curpal = (const uint16_t *)(st->bgextpal[en->num] +
                        (uint32_t)bgnum * 8192 + (uint32_t)(curtile >> 12) * 512);
                    else        curpal = pal;
                    uint32_t tilexoff = ((uint32_t)finalX >> 8) & 0x7;
                    uint32_t tileyoff = ((uint32_t)finalY >> 8) & 0x7;
                    if (curtile & (1 << 10)) tilexoff = 7 - tilexoff;
                    if (curtile & (1 << 11)) tileyoff = 7 - tileyoff;
                    uint8_t color = bgvram[(tilesetaddr + ((uint32_t)(curtile & 0x03FF) << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];
                    if (color)
                        draw_pixel(&en->bgobjline[i], curpal[color], 0x01000000u << bgnum);
                }
            }
            rotX += rotA;
            rotY += rotC;
        }
    }
}

static void draw_bg_large(Gx2DEng *en, int mosaic)
{
    const Gx2DEngine *e = en->e;
    const Gx2DState *st = en->st;
    uint16_t bgcnt = e->bgcnt[2];
    const uint16_t *pal;
    /* large BG sizes: 0: 512x1024, 1: 1024x512, 2: 512x256, 3: 512x512 */
    uint32_t xmask, ymask, yshift;
    switch ((bgcnt >> 14) & 0x3)
    {
        case 0: xmask = 0x1FFFF; ymask = 0x3FFFF; yshift = 9; break;
        case 1: xmask = 0x3FFFF; ymask = 0x1FFFF; yshift = 10; break;
        case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
        default: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
    }

    uint32_t ofxmask, ofymask;
    if (bgcnt & (1 << 13))
    {
        ofxmask = 0;
        ofymask = 0;
    }
    else
    {
        ofxmask = ~xmask;
        ofymask = ~ymask;
    }

    int16_t rotA = e->bgrota[0];
    int16_t rotC = e->bgrotc[0];
    int32_t rotX = en->bgxrefint[0];
    int32_t rotY = en->bgyrefint[0];

    const uint8_t *bgvram = st->bgvram[en->num];
    uint32_t bgvrammask = en->num ? 0x1FFFF : 0x7FFFF;

    if (en->num) pal = (const uint16_t *)(st->palette + 0x400);
    else           pal = (const uint16_t *)st->palette;

    for (int i = 0; i < 256; i++)
    {
        if (en->windowmask[i] & (1 << 2))
        {
            int32_t finalX, finalY;
            if (mosaic)
            {
                int im = en->bgxmosaictable[e->bgmosaic[0]][i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if (!((uint32_t)finalX & ofxmask) && !((uint32_t)finalY & ofymask))
            {
                uint8_t color = bgvram[(((((uint32_t)finalY & ymask) >> 8) << yshift) + (((uint32_t)finalX & xmask) >> 8)) & bgvrammask];
                if (color)
                    draw_pixel(&en->bgobjline[i], pal[color], 0x01000000u << 2);
            }
        }
        rotX += rotA;
        rotY += rotC;
    }
}

/* ---- sprites (melonDS GPU2D_Soft.cpp DrawSprites/DrawSprite_*) ---- */

static void draw_sprite_pixel(Gx2DEng *en, int window, int color, uint32_t pixelattr,
                              int32_t xpos)
{
    if (window)
    {
        if (color != -1)
            en->objwindow[xpos] = 1;
    }
    else
    {
        uint32_t oldpixel = en->objline[xpos];
        int oldisopaque = (oldpixel & OBJ_IS_OPAQUE) != 0;
        int newisopaque = (color != -1);
        int priocheck = (pixelattr & OBJ_BGPRIO_MASK) < (oldpixel & OBJ_BGPRIO_MASK);

        if (newisopaque && (!oldisopaque || priocheck))
        {
            en->objline[xpos] = (uint32_t)color | pixelattr;
        }
        else if (!newisopaque && !oldisopaque)
        {
            en->objline[xpos] &= ~(OBJ_MOSAIC_FLAG | OBJ_BGPRIO_MASK);
            en->objline[xpos] |= (pixelattr & (OBJ_IS_SPRITE | OBJ_MOSAIC_FLAG | OBJ_BGPRIO_MASK));
        }
    }
}

static void draw_sprite_rotscale(Gx2DEng *en, int window, int num, int boundwidth,
                                 int boundheight, int width, int height,
                                 int32_t xpos, int32_t ypos)
{
    const Gx2DState *st = en->st;
    const Gx2DEngine *e = en->e;
    const uint8_t *oam = st->oam + (en->num ? 0x400 : 0);
    const uint8_t *attrib = &oam[num * 8];
    uint32_t a0 = rd16(attrib), a1 = rd16(attrib + 2), a2 = rd16(attrib + 4);
    const uint8_t *rotparams = &oam[(((a1 >> 9) & 0x1F) * 32) + 6];

    uint32_t pixelattr = ((a2 & 0x0C00) << 6) | OBJ_IS_SPRITE | OBJ_IS_OPAQUE;
    uint32_t tilenum = a2 & 0x03FF;
    uint32_t spritemode = window ? 0 : ((a0 >> 10) & 0x3);

    uint32_t ytilefactor = 0;
    const uint8_t *objvram = st->objvram[en->num];
    uint32_t objvrammask = en->num ? 0x1FFFF : 0x3FFFF;

    int32_t centerX = boundwidth >> 1;
    int32_t centerY = boundheight >> 1;

    if ((a0 & (1 << 12)) && !window)
    {
        /* apply Y mosaic */
        pixelattr |= OBJ_MOSAIC_FLAG;
    }

    uint32_t xoff;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos + boundwidth) > 256)
            boundwidth = 256 - xpos;
    }
    else
    {
        xoff = (uint32_t)-xpos;
        xpos = 0;
    }

    int16_t rotA = (int16_t)rd16(rotparams + 0);
    int16_t rotB = (int16_t)rd16(rotparams + 8);
    int16_t rotC = (int16_t)rd16(rotparams + 16);
    int16_t rotD = (int16_t)rd16(rotparams + 24);

    int32_t rotX = ((int32_t)xoff - centerX) * rotA + ((int32_t)ypos - centerY) * rotB + (width << 7);
    int32_t rotY = ((int32_t)xoff - centerX) * rotC + ((int32_t)ypos - centerY) * rotD + (height << 7);

    width <<= 8;
    height <<= 8;

    int color = -1;

    if (spritemode == 3)
    {
        uint32_t alpha = a2 >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000u | (alpha << 24));

        uint32_t pixelsaddr;
        if (e->dispcnt & 0x40)
        {
            if (e->dispcnt & 0x20)
            {
                /* 'reserved': draws nothing */
                return;
            }
            else
            {
                pixelsaddr = tilenum << (7 + ((e->dispcnt >> 22) & 0x1));
                ytilefactor = (uint32_t)((width >> 8) * 2);
            }
        }
        else
        {
            if (e->dispcnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                ytilefactor = (256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                ytilefactor = (128 * 2);
            }
        }

        for (; xoff < (uint32_t)boundwidth;)
        {
            if ((uint32_t)rotX < (uint32_t)width && (uint32_t)rotY < (uint32_t)height)
            {
                uint16_t c = rd16(&objvram[(pixelsaddr + ((uint32_t)(rotY >> 8) * ytilefactor) + ((uint32_t)(rotX >> 8) << 1)) & objvrammask]);
                draw_sprite_pixel(en, window, (c & 0x8000) ? (int)c : -1, pixelattr, xpos);
                (void)color;
            }
            rotX += rotA;
            rotY += rotC;
            xoff++;
            xpos++;
        }
    }
    else
    {
        uint32_t pixelsaddr = tilenum;
        if (e->dispcnt & (1 << 4))
        {
            pixelsaddr <<= ((e->dispcnt >> 20) & 0x3);
            ytilefactor = (uint32_t)((width >> 11) << ((a0 & 0x2000) ? 1 : 0));
        }
        else
        {
            ytilefactor = 0x20;
        }

        if (spritemode == 1) pixelattr |= 0x80000000u;
        else                 pixelattr |= 0x10000000u;

        ytilefactor <<= 5;
        pixelsaddr <<= 5;

        if (a0 & (1 << 13))
        {
            /* 256-color */
            if (!window)
            {
                if (!(e->dispcnt & (1u << 31)))
                    pixelattr |= OBJ_STANDARD_PAL;
                else
                    pixelattr |= ((a2 & 0xF000) >> 4);
            }

            for (; xoff < (uint32_t)boundwidth;)
            {
                if ((uint32_t)rotX < (uint32_t)width && (uint32_t)rotY < (uint32_t)height)
                {
                    int c = objvram[(pixelsaddr + ((uint32_t)(rotY >> 11) * ytilefactor) + ((uint32_t)(rotY & 0x700) >> 5) + ((uint32_t)(rotX >> 11) * 64) + ((uint32_t)(rotX & 0x700) >> 8)) & objvrammask];
                    draw_sprite_pixel(en, window, c ? c : -1, pixelattr, xpos);
                }
                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
        else
        {
            /* 16-color */
            if (!window)
            {
                pixelattr |= OBJ_STANDARD_PAL;
                pixelattr |= ((a2 & 0xF000) >> 8);
            }

            for (; xoff < (uint32_t)boundwidth;)
            {
                if ((uint32_t)rotX < (uint32_t)width && (uint32_t)rotY < (uint32_t)height)
                {
                    int c = objvram[(pixelsaddr + ((uint32_t)(rotY >> 11) * ytilefactor) + ((uint32_t)(rotY & 0x700) >> 6) + ((uint32_t)(rotX >> 11) * 32) + ((uint32_t)(rotX & 0x700) >> 9)) & objvrammask];
                    if (rotX & 0x100)
                        c >>= 4;
                    else
                        c &= 0x0F;
                    draw_sprite_pixel(en, window, c ? c : -1, pixelattr, xpos);
                }
                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
    }
}

static void draw_sprite_normal(Gx2DEng *en, int window, int num, int width, int height,
                               int32_t xpos, int32_t ypos)
{
    const Gx2DState *st = en->st;
    const Gx2DEngine *e = en->e;
    const uint8_t *oam = st->oam + (en->num ? 0x400 : 0);
    const uint8_t *attrib = &oam[num * 8];
    uint32_t a0 = rd16(attrib), a1 = rd16(attrib + 2), a2 = rd16(attrib + 4);

    uint32_t pixelattr = ((a2 & 0x0C00) << 6) | OBJ_IS_SPRITE | OBJ_IS_OPAQUE;
    uint32_t tilenum = a2 & 0x03FF;
    uint32_t spritemode = window ? 0 : ((a0 >> 10) & 0x3);

    uint32_t wmask = (uint32_t)(width - 8); /* really ((width - 1) & ~0x7) */

    if ((a0 & (1 << 12)) && !window)
    {
        /* apply Y mosaic */
        pixelattr |= OBJ_MOSAIC_FLAG;
    }

    const uint8_t *objvram = st->objvram[en->num];
    uint32_t objvrammask = en->num ? 0x1FFFF : 0x3FFFF;

    /* yflip */
    if (a1 & (1 << 13))
        ypos = height - 1 - ypos;

    uint32_t xoff;
    uint32_t xend = (uint32_t)width;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos + (int32_t)xend) > 256)
            xend = (uint32_t)(256 - xpos);
    }
    else
    {
        xoff = (uint32_t)-xpos;
        xpos = 0;
    }

    if (spritemode == 3)
    {
        /* bitmap sprite */
        uint32_t alpha = a2 >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000u | (alpha << 24));

        uint32_t pixelsaddr = tilenum;
        if (e->dispcnt & 0x40)
        {
            if (e->dispcnt & 0x20)
            {
                /* 'reserved': draws nothing */
                return;
            }
            else
            {
                pixelsaddr <<= (7 + ((e->dispcnt >> 22) & 0x1));
                pixelsaddr += (uint32_t)(ypos * width * 2);
            }
        }
        else
        {
            if (e->dispcnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                pixelsaddr += (uint32_t)(ypos * 256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                pixelsaddr += (uint32_t)(ypos * 128 * 2);
            }
        }

        int32_t pixelstride;
        if (a1 & (1 << 12)) /* xflip */
        {
            pixelsaddr += (uint32_t)((width - 1) << 1);
            pixelsaddr -= (xoff << 1);
            pixelstride = -2;
        }
        else
        {
            pixelsaddr += (xoff << 1);
            pixelstride = 2;
        }

        for (; xoff < xend;)
        {
            uint16_t c = rd16(&objvram[pixelsaddr & objvrammask]);
            pixelsaddr = (uint32_t)(pixelsaddr + pixelstride);
            draw_sprite_pixel(en, window, (c & 0x8000) ? (int)c : -1, pixelattr, xpos);
            xoff++;
            xpos++;
        }
    }
    else
    {
        uint32_t pixelsaddr = tilenum;
        if (e->dispcnt & (1 << 4))
        {
            pixelsaddr <<= ((e->dispcnt >> 20) & 0x3);
            pixelsaddr += (uint32_t)((ypos >> 3) * (width >> 3)) << ((a0 & 0x2000) ? 1 : 0);
        }
        else
        {
            pixelsaddr += (uint32_t)((ypos >> 3) * 0x20);
        }

        if (spritemode == 1) pixelattr |= 0x80000000u;
        else                 pixelattr |= 0x10000000u;

        if (a0 & (1 << 13))
        {
            /* 256-color */
            pixelsaddr <<= 5;
            pixelsaddr += (uint32_t)((ypos & 0x7) << 3);
            int32_t pixelstride;
            if (!window)
            {
                if (!(e->dispcnt & (1u << 31)))
                    pixelattr |= OBJ_STANDARD_PAL;
                else
                    pixelattr |= ((a2 & 0xF000) >> 4);
            }

            if (a1 & (1 << 12)) /* xflip */
            {
                pixelsaddr += (((uint32_t)(width - 1) & wmask) << 3);
                pixelsaddr += (uint32_t)((width - 1) & 0x7);
                pixelsaddr -= ((xoff & wmask) << 3);
                pixelsaddr -= (xoff & 0x7);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 3);
                pixelsaddr += (xoff & 0x7);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                int c = objvram[pixelsaddr & objvrammask];
                pixelsaddr = (uint32_t)(pixelsaddr + pixelstride);
                draw_sprite_pixel(en, window, c ? c : -1, pixelattr, xpos);
                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr = (uint32_t)(pixelsaddr + 56 * pixelstride);
            }
        }
        else
        {
            /* 16-color */
            pixelsaddr <<= 5;
            pixelsaddr += (uint32_t)((ypos & 0x7) << 2);
            int32_t pixelstride;
            if (!window)
            {
                pixelattr |= OBJ_STANDARD_PAL;
                pixelattr |= ((a2 & 0xF000) >> 8);
            }

            if (a1 & (1 << 12)) /* xflip */
            {
                pixelsaddr += (((uint32_t)(width - 1) & wmask) << 2);
                pixelsaddr += (uint32_t)(((width - 1) & 0x7) >> 1);
                pixelsaddr -= ((xoff & wmask) << 2);
                pixelsaddr -= ((xoff & 0x7) >> 1);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 2);
                pixelsaddr += ((xoff & 0x7) >> 1);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                int c;
                if (a1 & (1 << 12))
                {
                    if (xoff & 0x1) { c = objvram[pixelsaddr & objvrammask] & 0x0F; pixelsaddr = (uint32_t)(pixelsaddr + pixelstride); }
                    else              c = objvram[pixelsaddr & objvrammask] >> 4;
                }
                else
                {
                    if (xoff & 0x1) { c = objvram[pixelsaddr & objvrammask] >> 4; pixelsaddr = (uint32_t)(pixelsaddr + pixelstride); }
                    else              c = objvram[pixelsaddr & objvrammask] & 0x0F;
                }

                draw_sprite_pixel(en, window, c ? c : -1, pixelattr, xpos);
                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr = (uint32_t)(pixelsaddr + ((a1 & 0x1000) ? -28 : 28));
            }
        }
    }
}

static void draw_sprites(Gx2DEng *en, int line)
{
    const Gx2DState *st = en->st;

    /* the OBJ buffers don't get updated at all if the 2D engine is disabled */
    if (!st->eng_enabled[en->num])
        return;

    en->numsprites = 0;
    memset(en->objline, 0, sizeof(en->objline));
    memset(en->objwindow, 0, sizeof(en->objwindow));

    if (!en->obj_enable)
        return;

    const uint8_t *oam = st->oam + (en->num ? 0x400 : 0);

    static const int32_t spritewidth[16] =
    {
        8, 16, 8, 8,
        16, 32, 8, 8,
        32, 32, 16, 8,
        64, 64, 32, 8
    };
    static const int32_t spriteheight[16] =
    {
        8, 8, 16, 8,
        16, 8, 32, 8,
        32, 16, 32, 8,
        64, 32, 64, 8
    };

    for (int sprnum = 0; sprnum < 128; sprnum++)
    {
        const uint8_t *attrib = &oam[sprnum * 8];
        uint32_t a0 = rd16(attrib), a1 = rd16(attrib + 2);

        uint16_t sprtype = (uint16_t)((a0 >> 8) & 0x3);
        if (sprtype == 2) /* disabled */
            continue;

        int iswin = (((a0 >> 10) & 0x3) == 2);

        uint32_t sizeparam = (a0 >> 14) | ((a1 & 0xC000) >> 12);
        int32_t width = spritewidth[sizeparam];
        int32_t height = spriteheight[sizeparam];
        int32_t boundwidth = width;
        int32_t boundheight = height;

        if (sprtype == 3) /* double-size rotscale sprite */
        {
            boundwidth <<= 1;
            boundheight <<= 1;
        }

        int32_t ypos = (int32_t)(a0 & 0xFF);
        if ((uint32_t)((line - ypos) & 0xFF) >= (uint32_t)boundheight)
            continue;

        int32_t xpos = (int32_t)((uint32_t)a1 << 23) >> 23;
        if (xpos <= -boundwidth)
            continue;

        if ((a0 & (1 << 12)) && (!iswin))
        {
            /* adjust Y position for sprite mosaic (see melonDS comment) */
            ypos = (int32_t)((en->objmosaic_line - (uint32_t)ypos) & 0xFF);
            if (ypos >= boundheight) ypos = 0;
        }
        else
            ypos = (int32_t)((line - ypos) & 0xFF);

        if (sprtype & 1)
            draw_sprite_rotscale(en, iswin, sprnum, boundwidth, boundheight, width,
                                 height, xpos, ypos);
        else
            draw_sprite_normal(en, iswin, sprnum, width, height, xpos, ypos);

        en->numsprites++;
    }
}

static void apply_sprite_mosaic_x(Gx2DEng *en)
{
    const Gx2DEngine *e = en->e;
    uint8_t mosw = e->objmosaic[0];
    if (mosw == 0) return;

    uint8_t mosx = 0;
    uint32_t latchcolor = 0;
    for (int i = 0; i < 256; i++)
    {
        uint32_t curcolor = en->objline[i];
        int latch = 0;

        if (mosx == 0)
            latch = 1;
        else if (!(curcolor & OBJ_MOSAIC_FLAG))
            latch = 1;
        else if (!(latchcolor & OBJ_MOSAIC_FLAG))
            latch = 1;
        else if ((curcolor & OBJ_BGPRIO_MASK) < (latchcolor & OBJ_BGPRIO_MASK))
            latch = 1;

        if (latch)
            latchcolor = curcolor;

        en->objline[i] = latchcolor;

        if (mosx == mosw)
            mosx = 0;
        else
            mosx++;
    }
}

static void interleave_sprites(Gx2DEng *en, int prio)
{
    const Gx2DState *st = en->st;
    uint32_t attrmask = ((uint32_t)prio << 16) | OBJ_IS_OPAQUE;
    const uint16_t *pal = (const uint16_t *)(st->palette + (en->num ? 0x600 : 0x200));
    const uint16_t *extpal = (const uint16_t *)st->objextpal[en->num];

    for (int i = 0; i < 256; i++)
    {
        if ((en->objline[i] & OBJ_OPA_PRIO_MASK) != attrmask)
            continue;
        if (!(en->windowmask[i] & 0x10))
            continue;

        uint16_t color;
        uint32_t pixel = en->objline[i];
        if (pixel & OBJ_DIRECT_COLOR)
            color = (uint16_t)(pixel & 0x7FFF);
        else if (pixel & OBJ_STANDARD_PAL)
            color = pal[pixel & 0xFF];
        else
            color = extpal[pixel & 0xFFF];

        draw_pixel(&en->bgobjline[i], color, pixel & 0xFF000000);
    }
}

/* ---- per-line composite (melonDS DrawScanline_BGOBJ + ColorComposite) ---- */

static uint32_t color_composite(Gx2DEng *en, int i, uint32_t val1, uint32_t val2)
{
    const Gx2DEngine *e = en->e;
    uint32_t coloreffect = 0;
    uint32_t eva = 0, evb = 0;

    uint32_t flag1 = val1 >> 24;
    uint32_t flag2 = val2 >> 24;
    uint32_t blendCnt = e->blendcnt;
    uint32_t target2;
    if      (flag2 & 0x80) target2 = 0x1000;
    else if (flag2 & 0x40) target2 = 0x0100;
    else                   target2 = flag2 << 8;

    if ((flag1 & 0x80) && (blendCnt & target2))
    {
        /* sprite blending */
        coloreffect = 1;
        if (flag1 & 0x40)
        {
            eva = flag1 & 0x1F;
            evb = 16 - eva;
        }
        else
        {
            eva = e->eva;
            evb = e->evb;
        }
    }
    else if ((flag1 & 0x40) && (blendCnt & target2))
    {
        /* 3D layer blending */
        coloreffect = 4;
    }
    else
    {
        if      (flag1 & 0x80) flag1 = 0x10;
        else if (flag1 & 0x40) flag1 = 0x01;

        if ((blendCnt & flag1) && (en->windowmask[i] & 0x20))
        {
            coloreffect = (blendCnt >> 6) & 0x3;
            if (coloreffect == 1)
            {
                if (blendCnt & target2)
                {
                    eva = e->eva;
                    evb = e->evb;
                }
                else
                    coloreffect = 0;
            }
        }
    }

    switch (coloreffect)
    {
        case 0: return val1;
        case 1: return color_blend4(val1, val2, eva, evb);
        case 2: return color_brightness_up(val1, e->evy, 0x8);
        case 3: return color_brightness_down(val1, e->evy, 0x7);
        case 4: return color_blend5(val1, val2);
    }

    return val1;
}

static void draw_scanline_bgobj(Gx2DEng *en, int line, const uint32_t *fb3dline)
{
    const Gx2DEngine *e = en->e;
    const Gx2DState *st = en->st;

    uint64_t backdrop;
    if (en->num)
        backdrop = rd16(st->palette + 0x400);
    else
        backdrop = rd16(st->palette);

    {
        uint8_t r = (uint8_t)((backdrop & 0x001F) << 1);
        uint8_t g = (uint8_t)(((backdrop & 0x03E0) >> 4) | ((backdrop & 0x8000) >> 15));
        uint8_t b = (uint8_t)((backdrop & 0x7C00) >> 9);

        backdrop = (uint64_t)r | ((uint64_t)g << 8) | ((uint64_t)b << 16) | 0x20000000ull;
        backdrop |= (backdrop << 32);

        for (int i = 0; i < 256; i += 2)
            memcpy(&en->bgobjline[i], &backdrop, 8);
        for (int i = 256; i < 512; i += 2)
        {
            uint64_t z = 0;
            memcpy(&en->bgobjline[i], &z, 8);
        }
    }

    if (e->dispcnt & 0xE000)
        eng_calculate_window_mask(en);
    else
        memset(en->windowmask, 0xFF, 256);

    apply_sprite_mosaic_x(en);

    uint32_t dispCnt = e->dispcnt;
    const uint16_t *bgcnt = e->bgcnt;
    int mode = (int)(dispCnt & 0x7);
    int mos = (e->bgmosaic[0] > 0);

    /* layer draw order: melonDS DrawScanlineBGMode<bgmode>/Mode6/Mode7 —
     * iterate BG priorities high->low; within a priority BG3..BG0 then OBJ */
    for (int i = 3; i >= 0; i--)
    {
        if (mode == 6)
        {
            /* mode 6: BG2 large bitmap + BG0 3D only */
            if ((bgcnt[2] & 0x3) == i && (en->layer_enable & (1 << 2)))
                draw_bg_large(en, (bgcnt[2] & (1 << 6)) && mos);
            if ((bgcnt[0] & 0x3) == i && (en->layer_enable & (1 << 0)))
            {
                if (!en->num && (dispCnt & 0x8) && fb3dline)
                    draw_bg_3d(en, fb3dline);
            }
        }
        else if (mode == 7)
        {
            /* mode 7: BG0/BG1 text (BG0 -> 3D) only */
            if ((bgcnt[1] & 0x3) == i && (en->layer_enable & (1 << 1)))
                draw_bg_text(en, line, 1, (bgcnt[1] & (1 << 6)) && mos);
            if ((bgcnt[0] & 0x3) == i && (en->layer_enable & (1 << 0)))
            {
                if (!en->num && (dispCnt & 0x8))
                {
                    if (fb3dline)
                        draw_bg_3d(en, fb3dline);
                }
                else
                    draw_bg_text(en, line, 0, (bgcnt[0] & (1 << 6)) && mos);
            }
        }
        else
        {
            /* modes 0-5 */
            if ((bgcnt[3] & 0x3) == i && (en->layer_enable & (1 << 3)))
            {
                if (mode >= 3)
                    draw_bg_extended(en, line, 3, (bgcnt[3] & (1 << 6)) && mos);
                else if (mode >= 1)
                    draw_bg_affine(en, line, 3, (bgcnt[3] & (1 << 6)) && mos);
                else
                    draw_bg_text(en, line, 3, (bgcnt[3] & (1 << 6)) && mos);
            }
            if ((bgcnt[2] & 0x3) == i && (en->layer_enable & (1 << 2)))
            {
                if (mode == 5)
                    draw_bg_extended(en, line, 2, (bgcnt[2] & (1 << 6)) && mos);
                else if (mode == 4 || mode == 2)
                    draw_bg_affine(en, line, 2, (bgcnt[2] & (1 << 6)) && mos);
                else
                    draw_bg_text(en, line, 2, (bgcnt[2] & (1 << 6)) && mos);
            }
            if ((bgcnt[1] & 0x3) == i && (en->layer_enable & (1 << 1)))
                draw_bg_text(en, line, 1, (bgcnt[1] & (1 << 6)) && mos);
            if ((bgcnt[0] & 0x3) == i && (en->layer_enable & (1 << 0)))
            {
                if (!en->num && (dispCnt & 0x8))
                {
                    if (fb3dline)
                        draw_bg_3d(en, fb3dline);
                }
                else
                    draw_bg_text(en, line, 0, (bgcnt[0] & (1 << 6)) && mos);
            }
        }
        if ((en->layer_enable & (1 << 4)) && en->numsprites)
        {
            interleave_sprites(en, i);
        }
    }

    /* color special effects */
    for (int i = 0; i < 256; i++)
    {
        uint32_t val1 = en->bgobjline[i];
        uint32_t val2 = en->bgobjline[256 + i];
        en->outline[i] = color_composite(en, i, val1, val2);
    }
}

/* BG mode dispatch, mirroring melonDS DrawScanlineBGMode<0..5>/Mode6/Mode7.
 * melonDS: BG3 type by mode (>=3 extended / >=1 affine / else text),
 * BG2 type (5: extended, 4|2: affine, 6: large, else text),
 * BG1/BG0 always text (BG0 -> 3D on engine A with DISP3CNT bit3).
 * Mode 6/7 are dispatched separately there; handled inline above via the
 * mode checks (mode 6 -> BG2 large, mode 7 -> BG0/BG1 text, BG2/BG3 unused
 * because in mode 7 their bgcnt slots are not drawn). */
static void draw_scanline(Gx2DEng *en, int line, const uint32_t *fb3dline)
{
    const Gx2DState *st = en->st;

    if (!st->eng_enabled[en->num])
    {
        uint32_t fillcolor = (en->num == 0) ? 0xFF000000u : 0xFF3F3F3Fu;
        for (int i = 0; i < 256; i++)
            en->outline[i] = fillcolor;
        return;
    }

    if (en->forced_blank)
    {
        for (int i = 0; i < 256; i++)
            en->outline[i] = 0xFF3F3F3Fu;
        return;
    }

    draw_scanline_bgobj(en, line, fb3dline);
}

/* ---- frame driver: replicate the melonDS scanline loop ---- */

static void eng_run_frame(Gx2DEng *en, const uint32_t *fb3d, uint32_t *dst6)
{
    /* frame start: VCount 262 hblank (PreDraw(reset), DrawSprites(0),
     * PostDraw(reset)) */
    en->vcount = 262;
    en->bgmosaic_ymax = en->e->bgmosaic[1];
    en->bgmosaic_y = 0;
    en->bgmosaic_latch = 1;
    en->objmosaic_y = 0;
    en->objmosaic_latch = 1;
    en->objmosaic_line = 0;
    for (int i = 0; i < 2; i++)
    {
        en->bgxref[i] = en->e->bgxref[i];
        en->bgyref[i] = en->e->bgyref[i];
    }
    en->win0active = en->e->win0active;
    en->win1active = en->e->win1active;
    eng_pre_draw(en, 1);
    draw_sprites(en, 0);
    eng_post_draw(en, 1);

    for (int L = 0; L < 192; L++)
    {
        en->vcount = L;
        /* UpdateWindows(L) happens at scanline start; L=0's effect is part
         * of the captured win0/1active latch */
        if (L > 0)
            eng_update_windows(en, L);
        eng_pre_draw(en, 0);
        draw_scanline(en, L, fb3d ? fb3d + L * 256 : NULL);
        if (L < 191)
            draw_sprites(en, L + 1);
        eng_post_draw(en, 0);
        memcpy(dst6 + L * 256, en->outline, 256 * 4);
    }
}

/* ---- master brightness + 6->8 bit expansion (melonDS GPU_Soft.cpp) ---- */

static void apply_master_brightness(uint16_t regval, uint32_t *dst)
{
    uint16_t mode = (uint16_t)(regval >> 14);
    if (mode == 1)
    {
        uint32_t factor = regval & 0x1F;
        if (factor > 16) factor = 16;
        for (int i = 0; i < 256; i++)
            dst[i] = color_brightness_up(dst[i], factor, 0x0);
    }
    else if (mode == 2)
    {
        uint32_t factor = regval & 0x1F;
        if (factor > 16) factor = 16;
        for (int i = 0; i < 256; i++)
            dst[i] = color_brightness_down(dst[i], factor, 0xF);
    }
}

static void expand_color_line(uint32_t *dst)
{
    for (int i = 0; i < 256; i += 2)
    {
        uint64_t c;
        memcpy(&c, &dst[i], 8);
        uint64_t r = (c << 18) & 0xFC000000FC0000ull;
        uint64_t g = (c << 2) & 0xFC000000FC00ull;
        uint64_t b = (c >> 14) & 0xFC000000FCull;
        c = r | g | b;
        c |= ((c & 0x00C0C0C000C0C0C0ull) >> 6) | 0xFF000000FF000000ull;
        memcpy(&dst[i], &c, 8);
    }
}

/* ---- public API ---- */

void gx2d_state_load(Gx2DState *st, const uint8_t *image)
{
    memset(st, 0, sizeof(*st));
    for (int n = 0; n < 2; n++)
    {
        const uint8_t *b = image + (n ? 0x100 : 0x000);
        Gx2DEngine *e = &st->eng[n];
        e->dispcnt = rd32(b + 0x00);
        for (int i = 0; i < 4; i++) e->bgcnt[i] = rd16(b + 0x04 + i * 2);
        for (int i = 0; i < 4; i++) e->bgxpos[i] = rd16(b + 0x0C + i * 2);
        for (int i = 0; i < 4; i++) e->bgypos[i] = rd16(b + 0x14 + i * 2);
        for (int i = 0; i < 2; i++) e->bgxref[i] = (int32_t)rd32(b + 0x1C + i * 4);
        for (int i = 0; i < 2; i++) e->bgyref[i] = (int32_t)rd32(b + 0x24 + i * 4);
        for (int i = 0; i < 2; i++) e->bgxrefreload[i] = (int32_t)rd32(b + 0x2C + i * 4);
        for (int i = 0; i < 2; i++) e->bgyrefreload[i] = (int32_t)rd32(b + 0x34 + i * 4);
        for (int i = 0; i < 2; i++) e->bgrota[i] = (int16_t)rd16(b + 0x3C + i * 2);
        for (int i = 0; i < 2; i++) e->bgrotb[i] = (int16_t)rd16(b + 0x40 + i * 2);
        for (int i = 0; i < 2; i++) e->bgrotc[i] = (int16_t)rd16(b + 0x44 + i * 2);
        for (int i = 0; i < 2; i++) e->bgrotd[i] = (int16_t)rd16(b + 0x48 + i * 2);
        memcpy(e->win0coords, b + 0x4C, 4);
        memcpy(e->win1coords, b + 0x50, 4);
        memcpy(e->wincnt, b + 0x54, 4);
        e->bgmosaic[0] = b[0x58]; e->bgmosaic[1] = b[0x59];
        e->objmosaic[0] = b[0x5A]; e->objmosaic[1] = b[0x5B];
        e->blendcnt = rd16(b + 0x5C);
        e->blendalpha = rd16(b + 0x5E);
        e->eva = b[0x60]; e->evb = b[0x61]; e->evy = b[0x62];
        e->win0active = b[0x63]; e->win1active = b[0x64];
    }

    const uint8_t *g = image + 0x200;
    st->masterbright[0] = rd16(g + 0x00);
    st->masterbright[1] = rd16(g + 0x02);
    st->screens_enabled = g[0x04];
    st->screen_swap = g[0x05];
    st->eng_enabled[0] = g[0x06];
    st->eng_enabled[1] = g[0x07];
    memcpy(st->vramcnt, g + 0x08, 9);
    st->vramstat = g[0x11];
    st->capture_enable = g[0x12];
    st->capturecnt = rd32(g + 0x14);
    st->dispfifo = (const uint16_t *)(g + 0x18);

    st->palette = image + 0x400;
    st->oam = image + 0xC00;
    st->bgvram[0] = image + 0x01400;
    st->bgvram[1] = image + 0x81400;
    st->objvram[0] = image + 0xA1400;
    st->objvram[1] = image + 0xE1400;
    st->bgextpal[0] = image + 0x101400;
    st->bgextpal[1] = image + 0x109400;
    st->objextpal[0] = image + 0x111400;
    st->objextpal[1] = image + 0x113400;
    st->vram_raw = image + 0x115400;
}

void gx2d_render_screen(const Gx2DState *st, int engine, const uint32_t *fb3d,
                        uint32_t *dst)
{
    static Gx2DEng en_a, en_b;
    Gx2DEng *en = engine ? &en_b : &en_a;
    uint32_t *line6 = (uint32_t *)malloc(256 * 192 * 4);
    if (!line6) return;

    eng_init(en, st, engine);
    eng_run_frame(en, fb3d, line6);

    for (int L = 0; L < 192; L++)
    {
        uint32_t *out = dst + L * 256;
        const Gx2DEngine *e = &st->eng[engine];
        uint32_t dispcnt = e->dispcnt;
        int disp_mode = (int)((dispcnt >> 16) & (engine ? 0x1 : 0x3));
        int skip_brightness = 0;
        switch (disp_mode)
        {
            case 0: /* screen off (melonDS skips master brightness) */
                for (int i = 0; i < 256; i++)
                    out[i] = engine ? 0xFF3F3F3Fu : 0x3F3F3Fu;
                skip_brightness = 1;
                break;
            case 1: /* regular display */
                memcpy(out, line6 + L * 256, 256 * 4);
                break;
            case 2: /* VRAM display (engine A only) */
            {
                uint32_t vrambank = (dispcnt >> 18) & 0x3;
                int mapped = (st->vramcnt[vrambank] & 0x80) &&
                             ((st->vramcnt[vrambank] & 0x3) == 0);
                if (mapped)
                {
                    const uint16_t *vram = (const uint16_t *)(st->vram_raw +
                                            vrambank * 0x20000);
                    vram = &vram[L * 256];
                    for (int i = 0; i < 256; i++)
                    {
                        uint16_t color = vram[i];
                        uint8_t r = (uint8_t)((color & 0x001F) << 1);
                        uint8_t g = (uint8_t)((color & 0x03E0) >> 4);
                        uint8_t b = (uint8_t)((color & 0x7C00) >> 9);
                        out[i] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
                    }
                }
                else
                {
                    for (int i = 0; i < 256; i++)
                        out[i] = 0;
                }
                break;
            }
            case 3: /* FIFO display (engine A only) */
            {
                const uint16_t *fifo = st->dispfifo;
                for (int i = 0; i < 256; i++)
                {
                    uint16_t color = fifo[i];
                    uint8_t r = (uint8_t)((color & 0x001F) << 1);
                    uint8_t g = (uint8_t)((color & 0x03E0) >> 4);
                    uint8_t b = (uint8_t)((color & 0x7C00) >> 9);
                    out[i] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
                }
                break;
            }
        }
        if (!skip_brightness)
            apply_master_brightness(st->masterbright[engine], out);
        expand_color_line(out);
    }
    free(line6);
}

void gx2d_render_frame(const Gx2DState *st, const uint32_t *fb3d,
                       uint32_t *out0, uint32_t *out1)
{
    if (!st->screens_enabled)
    {
        for (int i = 0; i < 256 * 192; i++)
        {
            out0[i] = 0xFF000000u;
            out1[i] = 0xFF000000u;
        }
        return;
    }

    uint32_t *bufa = (uint32_t *)malloc(256 * 192 * 4);
    uint32_t *bufb = (uint32_t *)malloc(256 * 192 * 4);
    if (!bufa || !bufb) { free(bufa); free(bufb); return; }

    gx2d_render_screen(st, 0, fb3d, bufa);
    gx2d_render_screen(st, 1, NULL, bufb);

    /* melonDS SoftRenderer::DrawScanline: ScreenSwap ? A->0,B->1 : A->1,B->0 */
    if (st->screen_swap)
    {
        memcpy(out0, bufa, 256 * 192 * 4);
        memcpy(out1, bufb, 256 * 192 * 4);
    }
    else
    {
        memcpy(out0, bufb, 256 * 192 * 4);
        memcpy(out1, bufa, 256 * 192 * 4);
    }
    free(bufa);
    free(bufb);
}

int gx2d_write_ppm(const char *path, const uint32_t *buf)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n256 192\n255\n");
    for (int i = 0; i < 256 * 192; i++)
    {
        /* BGRA 8-bit (melonDS ExpandColor) -> RGB */
        uint8_t rgb[3] = {
            (uint8_t)(buf[i] & 0xFF),
            (uint8_t)((buf[i] >> 8) & 0xFF),
            (uint8_t)((buf[i] >> 16) & 0xFF)
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return 0;
}
