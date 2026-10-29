/* gx_gl2d.c — DS 2D engine + compositor on the GPU (OpenGL 4.3 core).
 *
 * Per-pixel GLSL port of the validated software model (src/gx_2d.c):
 * identical integer arithmetic and identical candidate-stack semantics.
 * See gx_gl2d.h for the architecture.  Per-scanline latch state (ref
 * counters, mosaic rows, window y/x latches) is computed here with the same
 * PreDraw/PostDraw/UpdateWindows model as eng_run_frame() in gx_2d.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "gx_gl2d.h"

#define FB_W 256
#define FB_H 192

/* ------------------------------------------------------------------ */
/* shader preamble: data textures + helpers                            */

static const char *glsl_pre =
    "#version 430 core\n"
    "uniform usampler2D u_bgvram;\n"
    "uniform usampler2D u_objvram;\n"
    "uniform usampler2D u_palette;\n"
    "uniform usampler2D u_extpal_bg;\n"
    "uniform usampler2D u_extpal_obj;\n"
    "uniform usampler2D u_oam;\n"
    "uniform usampler2D u_fb3d;\n"
    "uniform isampler2D u_latch;\n"
    "uniform uint u_dispcnt;\n"
    "uniform uint u_bgcnt[4];\n"
    "uniform int u_bgxpos[4];\n"
    "uniform int u_bgypos[4];\n"
    "uniform int u_bgrota[2];\n"
    "uniform int u_bgrotc[2];\n"
    "uniform int u_win0[4];\n"
    "uniform int u_win1[4];\n"
    "uniform uint u_wincnt[4];\n"
    "uniform int u_bgmosx;   /* X size-1 */\n"
    "uniform int u_bgmosy;\n"
    "uniform int u_objmosx;\n"
    "uniform uint u_blendcnt;\n"
    "uniform uint u_eva, u_evb, u_evy;\n"
    "uniform int u_enginenum;\n"
    "\n"
    "uint bfetch(usampler2D t, int a) {\n"
    "    return texelFetch(t, ivec2(a & 1023, a >> 10), 0).r;\n"
    "}\n"
    "uint r16(usampler2D t, int a) {\n"
    "    return bfetch(t, a) | (bfetch(t, a + 1) << 8);\n"
    "}\n"
    "uint pal16(int off) { return r16(u_palette, off); }\n"
    "\n"
    "/* draw_pixel color expansion (melonDS bottom-green-bit quirk) */\n"
    "uint expand5(uint color) {\n"
    "    uint r = (color & 0x001Fu) << 1;\n"
    "    uint g = ((color & 0x03E0u) >> 4) | ((color & 0x8000u) >> 15);\n"
    "    uint b = (color & 0x7C00u) >> 9;\n"
    "    return r | (g << 8) | (b << 16);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* pass 1: sprite layer (draw_sprites + draw_sprite_pixel)             */

static const char *sprite_fs_src =
    "#version 430 core\n"
    "/* SPRITE_PRE */\n"
    "layout(r32ui) uniform writeonly uimage2D u_objimg;\n"
    "\n"
    "const int spritewidth[16] = int[16](8,16,8,8, 16,32,8,8, 32,32,16,8, 64,64,32,8);\n"
    "const int spriteheight[16] = int[16](8,8,16,8, 16,8,32,8, 32,16,32,8, 64,32,64,8);\n"
    "\n"
    "/* OBJLine flags (gx_2d.c) */\n"
    "const uint OBJ_STANDARD_PAL = 1u << 12;\n"
    "const uint OBJ_DIRECT_COLOR = 1u << 15;\n"
    "const uint OBJ_BGPRIO_MASK  = 0x3u << 16;\n"
    "const uint OBJ_IS_OPAQUE    = 1u << 18;\n"
    "const uint OBJ_IS_SPRITE    = 1u << 19;\n"
    "const uint OBJ_MOSAIC_FLAG  = 1u << 20;\n"
    "const uint OBJ_WINDOW_BIT   = 1u << 21; /* output: objwindow accumulator */\n"
    "\n"
    "void draw_sprite_pixel(inout uint objline, inout uint objwin, int window,\n"
    "                       int color, uint pixelattr) {\n"
    "    if (window != 0) {\n"
    "        if (color != -1) objwin = 1u;\n"
    "    } else {\n"
    "        uint oldpixel = objline;\n"
    "        bool oldisopaque = (oldpixel & OBJ_IS_OPAQUE) != 0u;\n"
    "        bool newisopaque = (color != -1);\n"
    "        bool priocheck = (pixelattr & OBJ_BGPRIO_MASK) < (oldpixel & OBJ_BGPRIO_MASK);\n"
    "        if (newisopaque && (!oldisopaque || priocheck)) {\n"
    "            objline = uint(color) | pixelattr;\n"
    "        } else if (!newisopaque && !oldisopaque) {\n"
    "            objline &= ~(OBJ_MOSAIC_FLAG | OBJ_BGPRIO_MASK);\n"
    "            objline |= (pixelattr & (OBJ_IS_SPRITE | OBJ_MOSAIC_FLAG | OBJ_BGPRIO_MASK));\n"
    "        }\n"
    "    }\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    int x = int(gl_FragCoord.x);\n"
    "    int line = 191 - int(gl_FragCoord.y);\n"
    "    uint objline = 0u;\n"
    "    uint objwin = 0u;\n"
    "    ivec4 lat = texelFetch(u_latch, ivec2(1, line), 0);\n"
    "    int objmos_line = lat.y;\n"
    "\n"
    "    if (((u_dispcnt >> 12) & 1u) != 0u) {\n"
    "    for (int sprnum = 0; sprnum < 128; sprnum++) {\n"
    "        int attrib = sprnum * 8;\n"
    "        uint a0 = r16(u_oam, attrib), a1 = r16(u_oam, attrib + 2), a2 = r16(u_oam, attrib + 4);\n"
    "        int sprtype = int((a0 >> 8) & 0x3u);\n"
    "        if (sprtype == 2) continue;\n"
    "        int iswin = (((a0 >> 10) & 0x3u) == 2u) ? 1 : 0;\n"
    "        uint sizeparam = (a0 >> 14) | ((a1 & 0xC000u) >> 12);\n"
    "        int width = spritewidth[sizeparam];\n"
    "        int height = spriteheight[sizeparam];\n"
    "        int boundwidth = width, boundheight = height;\n"
    "        if (sprtype == 3) { boundwidth <<= 1; boundheight <<= 1; }\n"
    "        int ypos0 = int(a0 & 0xFFu);\n"
    "        if (uint((line - ypos0) & 0xFF) >= uint(boundheight)) continue;\n"
    "        int xpos0 = int(a1 << 23) >> 23;\n"
    "        if (xpos0 <= -boundwidth) continue;\n"
    "        int ypos;\n"
    "        if (((a0 & (1u << 12)) != 0u) && iswin == 0) {\n"
    "            ypos = (objmos_line - ypos0) & 0xFF;\n"
    "            if (ypos >= boundheight) ypos = 0;\n"
    "        } else {\n"
    "            ypos = (line - ypos0) & 0xFF;\n"
    "        }\n"
    "\n"
    "        uint pixelattr = ((a2 & 0x0C00u) << 6) | OBJ_IS_SPRITE | OBJ_IS_OPAQUE;\n"
    "        uint tilenum = a2 & 0x03FFu;\n"
    "        int spritemode = iswin != 0 ? 0 : int((a0 >> 10) & 0x3u);\n"
    "        if (((a0 & (1u << 12)) != 0u) && iswin == 0)\n"
    "            pixelattr |= OBJ_MOSAIC_FLAG;\n"
    "\n"
    "        int xoff = x - xpos0;\n"
    "        int color = -1;\n"
    "\n"
    "        if ((sprtype & 1) != 0) {\n"
    "            /* ---- rotscale ---- */\n"
    "            int rotnum = int((a1 >> 9) & 0x1Fu);\n"
    "            int rp = rotnum * 32 + 6;\n"
    "            int rotA = int(r16(u_oam, rp)) ; rotA = (rotA << 16) >> 16;\n"
    "            int rotB = int(r16(u_oam, rp + 8)); rotB = (rotB << 16) >> 16;\n"
    "            int rotC = int(r16(u_oam, rp + 16)); rotC = (rotC << 16) >> 16;\n"
    "            int rotD = int(r16(u_oam, rp + 24)); rotD = (rotD << 16) >> 16;\n"
    "            if (xoff < 0 || xoff >= boundwidth) continue;\n"
    "            int centerX = boundwidth >> 1, centerY = boundheight >> 1;\n"
    "            int rotX = (xoff - centerX) * rotA + (ypos - centerY) * rotB + (width << 7);\n"
    "            int rotY = (xoff - centerX) * rotC + (ypos - centerY) * rotD + (height << 7);\n"
    "            int w8 = width << 8, h8 = height << 8;\n"
    "            if (uint(rotX) >= uint(w8) || uint(rotY) >= uint(h8))\n"
    "                continue; /* software draws nothing out of bounds */\n"
    "            if (spritemode == 3) {\n"
    "                uint alpha = a2 >> 12;\n"
    "                if (alpha == 0u) continue;\n"
    "                alpha++;\n"
    "                pixelattr |= (0xC0000000u | (alpha << 24));\n"
    "                int pixelsaddr; uint ytilefactor;\n"
    "                if ((u_dispcnt & 0x40u) != 0u) {\n"
    "                    if ((u_dispcnt & 0x20u) != 0u) { continue; }\n"
    "                    pixelsaddr = int(tilenum << (7 + ((u_dispcnt >> 22) & 0x1u)));\n"
    "                    ytilefactor = uint((w8 >> 8) * 2);\n"
    "                } else if ((u_dispcnt & 0x20u) != 0u) {\n"
    "                    pixelsaddr = int(((tilenum & 0x01Fu) << 4) + ((tilenum & 0x3E0u) << 7));\n"
    "                    ytilefactor = 256u * 2u;\n"
    "                } else {\n"
    "                    pixelsaddr = int(((tilenum & 0x00Fu) << 4) + ((tilenum & 0x3F0u) << 7));\n"
    "                    ytilefactor = 128u * 2u;\n"
    "                }\n"
    "                uint c = r16(u_objvram, (pixelsaddr + int(uint(rotY >> 8) * ytilefactor) + ((rotX >> 8) << 1)) & 0x3FFFF);\n"
    "                color = ((c & 0x8000u) != 0u) ? int(c) : -1;\n"
    "                draw_sprite_pixel(objline, objwin, iswin, color, pixelattr);\n"
    "                continue;\n"
    "            }\n"
    "            {\n"
    "                int pixelsaddr = int(tilenum); uint ytilefactor;\n"
    "                if ((u_dispcnt & (1u << 4)) != 0u) {\n"
    "                    pixelsaddr <<= int((u_dispcnt >> 20) & 0x3u);\n"
    "                    ytilefactor = uint((w8 >> 11) << (((a0 & 0x2000u) != 0u) ? 1 : 0));\n"
    "                } else {\n"
    "                    ytilefactor = 0x20u;\n"
    "                }\n"
    "                if (spritemode == 1) pixelattr |= 0x80000000u;\n"
    "                else                 pixelattr |= 0x10000000u;\n"
    "                ytilefactor <<= 5;\n"
    "                pixelsaddr <<= 5;\n"
    "                if ((a0 & (1u << 13)) != 0u) {\n"
    "                    if (iswin == 0) {\n"
    "                        if ((u_dispcnt & (1u << 31)) == 0u) pixelattr |= OBJ_STANDARD_PAL;\n"
    "                        else pixelattr |= ((a2 & 0xF000u) >> 4);\n"
    "                    }\n"
    "                    int c = int(bfetch(u_objvram, (pixelsaddr + int(uint(rotY >> 11) * ytilefactor) + ((rotY & 0x700) >> 5) + ((rotX >> 11) * 64) + ((rotX & 0x700) >> 8)) & 0x3FFFF));\n"
    "                    color = c != 0 ? c : -1;\n"
    "                } else {\n"
    "                    if (iswin == 0) {\n"
    "                        pixelattr |= OBJ_STANDARD_PAL;\n"
    "                        pixelattr |= ((a2 & 0xF000u) >> 8);\n"
    "                    }\n"
    "                    int c = int(bfetch(u_objvram, (pixelsaddr + int(uint(rotY >> 11) * ytilefactor) + ((rotY & 0x700) >> 6) + ((rotX >> 11) * 32) + ((rotX & 0x700) >> 9)) & 0x3FFFF));\n"
    "                    if ((rotX & 0x100) != 0) c >>= 4; else c &= 0x0F;\n"
    "                    color = c != 0 ? c : -1;\n"
    "                }\n"
    "                draw_sprite_pixel(objline, objwin, iswin, color, pixelattr);\n"
    "                continue;\n"
    "            }\n"
    "        }\n"
    "\n"
    "        /* ---- normal sprite ---- */\n"
    "        int wmask = width - 8;\n"
    "        if ((a1 & (1u << 13)) != 0u) ypos = height - 1 - ypos;\n"
    "        if (xoff < 0 || xoff >= width)\n"
    "            continue; /* outside the sprite: software draws nothing */\n"
    "        if (spritemode == 3) {\n"
    "            uint alpha = a2 >> 12;\n"
    "            if (alpha == 0u) continue;\n"
    "            alpha++;\n"
    "            pixelattr |= (0xC0000000u | (alpha << 24));\n"
    "            int pixelsaddr = int(tilenum);\n"
    "            if ((u_dispcnt & 0x40u) != 0u) {\n"
    "                if ((u_dispcnt & 0x20u) != 0u) { continue; }\n"
    "                pixelsaddr <<= (7 + int((u_dispcnt >> 22) & 0x1u));\n"
    "                pixelsaddr += ypos * width * 2;\n"
    "            } else if ((u_dispcnt & 0x20u) != 0u) {\n"
    "                pixelsaddr = int(((tilenum & 0x01Fu) << 4) + ((tilenum & 0x3E0u) << 7));\n"
    "                pixelsaddr += ypos * 256 * 2;\n"
    "            } else {\n"
    "                pixelsaddr = int(((tilenum & 0x00Fu) << 4) + ((tilenum & 0x3F0u) << 7));\n"
    "                pixelsaddr += ypos * 128 * 2;\n"
    "            }\n"
    "            if ((a1 & (1u << 12)) != 0u) {\n"
    "                pixelsaddr += (width - 1) << 1;\n"
    "                pixelsaddr -= xoff << 1;\n"
    "            } else {\n"
    "                pixelsaddr += xoff << 1;\n"
    "            }\n"
    "            uint c = r16(u_objvram, pixelsaddr & 0x3FFFF);\n"
    "            color = ((c & 0x8000u) != 0u) ? int(c) : -1;\n"
    "            draw_sprite_pixel(objline, objwin, iswin, color, pixelattr);\n"
    "            continue;\n"
    "        }\n"
    "        {\n"
    "            int pixelsaddr = int(tilenum);\n"
    "            if ((u_dispcnt & (1u << 4)) != 0u) {\n"
    "                pixelsaddr <<= int((u_dispcnt >> 20) & 0x3u);\n"
    "                pixelsaddr += ((ypos >> 3) * (width >> 3)) << (((a0 & 0x2000u) != 0u) ? 1 : 0);\n"
    "            } else {\n"
    "                pixelsaddr += (ypos >> 3) * 0x20;\n"
    "            }\n"
    "            if (spritemode == 1) pixelattr |= 0x80000000u;\n"
    "            else                 pixelattr |= 0x10000000u;\n"
    "            if ((a0 & (1u << 13)) != 0u) {\n"
    "                /* 256-color */\n"
    "                pixelsaddr <<= 5;\n"
    "                pixelsaddr += (ypos & 0x7) << 3;\n"
    "                if (iswin == 0) {\n"
    "                    if ((u_dispcnt & (1u << 31)) == 0u) pixelattr |= OBJ_STANDARD_PAL;\n"
    "                    else pixelattr |= ((a2 & 0xF000u) >> 4);\n"
    "                }\n"
    "                if ((a1 & (1u << 12)) != 0u) {\n"
    "                    pixelsaddr += (((width - 1) & wmask) << 3);\n"
    "                    pixelsaddr += (width - 1) & 0x7;\n"
    "                    pixelsaddr -= (xoff & wmask) << 3;\n"
    "                    pixelsaddr -= xoff & 0x7;\n"
    "                } else {\n"
    "                    pixelsaddr += (xoff & wmask) << 3;\n"
    "                    pixelsaddr += xoff & 0x7;\n"
    "                }\n"
    "                int c = int(bfetch(u_objvram, pixelsaddr & 0x3FFFF));\n"
    "                color = c != 0 ? c : -1;\n"
    "            } else {\n"
    "                /* 16-color */\n"
    "                pixelsaddr <<= 5;\n"
    "                pixelsaddr += (ypos & 0x7) << 2;\n"
    "                if (iswin == 0) {\n"
    "                    pixelattr |= OBJ_STANDARD_PAL;\n"
    "                    pixelattr |= ((a2 & 0xF000u) >> 8);\n"
    "                }\n"
    "                if ((a1 & (1u << 12)) != 0u) {\n"
    "                    pixelsaddr += (((width - 1) & wmask) << 2);\n"
    "                    pixelsaddr += ((width - 1) & 0x7) >> 1;\n"
    "                    pixelsaddr -= (xoff & wmask) << 2;\n"
    "                    pixelsaddr -= (xoff & 0x7) >> 1;\n"
    "                } else {\n"
    "                    pixelsaddr += (xoff & wmask) << 2;\n"
    "                    pixelsaddr += (xoff & 0x7) >> 1;\n"
    "                }\n"
    "                int c;\n"
    "                if ((a1 & (1u << 12)) != 0u) {\n"
    "                    if ((xoff & 0x1) != 0) c = int(bfetch(u_objvram, pixelsaddr & 0x3FFFF)) & 0x0F;\n"
    "                    else                 c = int(bfetch(u_objvram, pixelsaddr & 0x3FFFF)) >> 4;\n"
    "                } else {\n"
    "                    if ((xoff & 0x1) != 0) c = int(bfetch(u_objvram, pixelsaddr & 0x3FFFF)) >> 4;\n"
    "                    else                 c = int(bfetch(u_objvram, pixelsaddr & 0x3FFFF)) & 0x0F;\n"
    "                }\n"
    "                color = c != 0 ? c : -1;\n"
    "            }\n"
    "            draw_sprite_pixel(objline, objwin, iswin, color, pixelattr);\n"
    "        }\n"
    "    }\n"
    "    }\n"
    "    imageStore(u_objimg, ivec2(x, line), uvec4(objline | (objwin << 21)));\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* pass 2: compositor (draw_scanline_bgobj + color_composite)          */

static const char *comp_fs_src =
    "#version 430 core\n"
    "/* COMP_PRE */\n"
    "layout(r32ui) uniform writeonly uimage2D u_out;   /* 6-bit outline */\n"
    "uniform usampler2D u_objimg_s;\n"
    "\n"
    "const uint OBJ_BGPRIO_MASK = 0x3u << 16;\n"
    "const uint OBJ_IS_OPAQUE   = 1u << 18;\n"
    "const uint OBJ_MOSAIC_FLAG = 1u << 20;\n"
    "const uint OBJ_WINDOW_BIT  = 1u << 21;\n"
    "\n"
    "uint draw_pixel(uint color, uint flag) {\n"
    "    return expand5(color) | flag;\n"
    "}\n"
    "\n"
    "/* window x-latch closed form (eng_calculate_window_mask sweep) */\n"
    "bool x_active(int x, int x1, int x2, int init) {\n"
    "    int last = -1, st = init;\n"
    "    if (x1 <= x) { last = x1; st = 1; }\n"
    "    if (x2 <= x && x2 >= last) { st = 0; }\n"
    "    return st != 0;\n"
    "}\n"
    "\n"
    "uint color_blend4(uint val1, uint val2, uint eva, uint evb) {\n"
    "    uint r = (((val1 & 0x00003Fu) * eva) + ((val2 & 0x00003Fu) * evb) + 0x000008u) >> 4;\n"
    "    uint g = ((((val1 & 0x003F00u) * eva) + ((val2 & 0x003F00u) * evb) + 0x000800u) >> 4) & 0x007F00u;\n"
    "    uint b = ((((val1 & 0x3F0000u) * eva) + ((val2 & 0x3F0000u) * evb) + 0x080000u) >> 4) & 0x7F0000u;\n"
    "    if (r > 0x00003Fu) r = 0x00003Fu;\n"
    "    if (g > 0x003F00u) g = 0x003F00u;\n"
    "    if (b > 0x3F0000u) b = 0x3F0000u;\n"
    "    return r | g | b | 0xFF000000u;\n"
    "}\n"
    "\n"
    "uint color_blend5(uint val1, uint val2) {\n"
    "    uint eva = ((val1 >> 24) & 0x1Fu) + 1u;\n"
    "    uint evb = 32u - eva;\n"
    "    if (eva == 32u) return val1;\n"
    "    uint r = (((val1 & 0x00003Fu) * eva) + ((val2 & 0x00003Fu) * evb) + 0x000010u) >> 5;\n"
    "    uint g = ((((val1 & 0x003F00u) * eva) + ((val2 & 0x003F00u) * evb) + 0x001000u) >> 5) & 0x007F00u;\n"
    "    uint b = ((((val1 & 0x3F0000u) * eva) + ((val2 & 0x3F0000u) * evb) + 0x100000u) >> 5) & 0x7F0000u;\n"
    "    if (r > 0x00003Fu) r = 0x00003Fu;\n"
    "    if (g > 0x003F00u) g = 0x003F00u;\n"
    "    if (b > 0x3F0000u) b = 0x3F0000u;\n"
    "    return r | g | b | 0xFF000000u;\n"
    "}\n"
    "\n"
    "uint color_brightness_up(uint val, uint factor, uint bias) {\n"
    "    uint rb = val & 0x3F003Fu;\n"
    "    uint g = val & 0x003F00u;\n"
    "    rb += (((((0x3F003Fu - rb) * factor) + (bias * 0x010001u)) >> 4) & 0x3F003Fu);\n"
    "    g  += (((((0x003F00u - g) * factor) + (bias * 0x000100u)) >> 4) & 0x003F00u);\n"
    "    return rb | g | 0xFF000000u;\n"
    "}\n"
    "\n"
    "uint color_brightness_down(uint val, uint factor, uint bias) {\n"
    "    uint rb = val & 0x3F003Fu;\n"
    "    uint g = val & 0x003F00u;\n"
    "    rb -= ((((rb * factor) + (bias * 0x010001u)) >> 4) & 0x3F003Fu);\n"
    "    g  -= ((((g  * factor) + (bias * 0x000100u)) >> 4) & 0x003F00u);\n"
    "    return rb | g | 0xFF000000u;\n"
    "}\n"
    "\n"
    "uint color_composite(int x, uint val1, uint val2, uint wmask) {\n"
    "    uint coloreffect = 0u;\n"
    "    uint eva = 0u, evb = 0u;\n"
    "    uint flag1 = val1 >> 24;\n"
    "    uint flag2 = val2 >> 24;\n"
    "    uint blendCnt = u_blendcnt;\n"
    "    uint target2;\n"
    "    if      ((flag2 & 0x80u) != 0u) target2 = 0x1000u;\n"
    "    else if ((flag2 & 0x40u) != 0u) target2 = 0x0100u;\n"
    "    else                   target2 = flag2 << 8;\n"
    "\n"
    "    if ((flag1 & 0x80u) != 0u && (blendCnt & target2) != 0u) {\n"
    "        coloreffect = 1u;\n"
    "        if ((flag1 & 0x40u) != 0u) {\n"
    "            eva = flag1 & 0x1Fu;\n"
    "            evb = 16u - eva;\n"
    "        } else {\n"
    "            eva = u_eva;\n"
    "            evb = u_evb;\n"
    "        }\n"
    "    } else if ((flag1 & 0x40u) != 0u && (blendCnt & target2) != 0u) {\n"
    "        coloreffect = 4u;\n"
    "    } else {\n"
    "        if      ((flag1 & 0x80u) != 0u) flag1 = 0x10u;\n"
    "        else if ((flag1 & 0x40u) != 0u) flag1 = 0x01u;\n"
    "        if ((blendCnt & flag1) != 0u && (wmask & 0x20u) != 0u) {\n"
    "            coloreffect = (blendCnt >> 6) & 0x3u;\n"
    "            if (coloreffect == 1u) {\n"
    "                if ((blendCnt & target2) != 0u) {\n"
    "                    eva = u_eva;\n"
    "                    evb = u_evb;\n"
    "                } else\n"
    "                    coloreffect = 0u;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "\n"
    "    switch (coloreffect) {\n"
    "        case 0u: return val1;\n"
    "        case 1u: return color_blend4(val1, val2, eva, evb);\n"
    "        case 2u: return color_brightness_up(val1, u_evy, 0x8u);\n"
    "        case 3u: return color_brightness_down(val1, u_evy, 0x7u);\n"
    "        case 4u: return color_blend5(val1, val2);\n"
    "    }\n"
    "    return val1;\n"
    "}\n"
    "\n"
    "/* ---- BG pixel evaluators (draw_bg_*) ---- */\n"
    "\n"
    "int bg_base_x(int bgnum) { return u_bgxpos[bgnum]; }\n"
    "\n"
    "/* returns 0 when transparent, else (palette color16 | flag<<24) */\n"
    "uint bg_text_pixel(int bgnum, int x, int line, int bgmos_line, int xmt) {\n"
    "    uint bgcnt = u_bgcnt[bgnum];\n"
    "    int xoff = u_bgxpos[bgnum];\n"
    "    int yoff = u_bgypos[bgnum];\n"
    "    if ((bgcnt & (1u << 6)) != 0u) yoff += bgmos_line;\n"
    "    else yoff += line;\n"
    "    int widexmask = ((bgcnt & (1u << 14)) != 0u) ? 0x100 : 0;\n"
    "    bool extpal = (u_dispcnt & (1u << 30)) != 0u;\n"
    "    int extpalslot = ((bgnum < 2) && ((bgcnt & 0x2000u) != 0u)) ? (2 + bgnum) : bgnum;\n"
    "    int tilesetaddr, tilemapaddr, paloff;\n"
    "    if (u_enginenum != 0) {\n"
    "        tilesetaddr = int((bgcnt & 0x003Cu) << 12);\n"
    "        tilemapaddr = int((bgcnt & 0x1F00u) << 3);\n"
    "        paloff = 0x400;\n"
    "    } else {\n"
    "        tilesetaddr = int(((u_dispcnt & 0x07000000u) >> 8) + ((bgcnt & 0x003Cu) << 12));\n"
    "        tilemapaddr = int(((u_dispcnt & 0x38000000u) >> 11) + ((bgcnt & 0x1F00u) << 3));\n"
    "        paloff = 0;\n"
    "    }\n"
    "    if ((bgcnt & (1u << 15)) != 0u) {\n"
    "        tilemapaddr += (yoff & 0x1F8) << 3;\n"
    "        if ((bgcnt & (1u << 14)) != 0u)\n"
    "            tilemapaddr += (yoff & 0x100) << 3;\n"
    "    } else {\n"
    "        tilemapaddr += (yoff & 0xF8) << 3;\n"
    "    }\n"
    "    int xpos = xoff + x - xmt; /* software: xoff runs bgxpos..bgxpos+255 */\n"
    "    int mask = (u_enginenum != 0) ? 0x1FFFF : 0x7FFFF;\n"
    "    uint curtile = r16(u_bgvram, (tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & mask);\n"
    "    if ((bgcnt & (1u << 7)) != 0u) {\n"
    "        /* 256-color */\n"
    "        int palbase = paloff;\n"
    "        if (extpal) palbase = extpalslot * 8192 + int(curtile >> 12) * 512 + 0x100000;\n"
    "        int pixelsaddr = tilesetaddr + (int(curtile & 0x03FFu) << 6)\n"
    "            + (((curtile & (1u << 11)) != 0u ? (7 - (yoff & 0x7)) : (yoff & 0x7)) << 3);\n"
    "        int tilexoff = ((curtile & (1u << 10)) != 0u) ? (7 - (xpos & 0x7)) : (xpos & 0x7);\n"
    "        int color = int(bfetch(u_bgvram, (pixelsaddr + tilexoff) & mask));\n"
    "        if (color == 0) return 0u;\n"
    "        uint c = extpal ? r16(u_extpal_bg, (palbase + color * 2) & 0x7FFF)\n"
    "                        : pal16(palbase + color * 2);\n"
    "        return c | (0x01000000u << bgnum);\n"
    "    } else {\n"
    "        /* 16-color */\n"
    "        int palbase = paloff + int(((curtile & 0xF000u) >> 8)) * 2;\n"
    "        int pixelsaddr = tilesetaddr + (int(curtile & 0x03FFu) << 5)\n"
    "            + (((curtile & (1u << 11)) != 0u ? (7 - (yoff & 0x7)) : (yoff & 0x7)) << 2);\n"
    "        int tilexoff = ((curtile & (1u << 10)) != 0u) ? (7 - (xpos & 0x7)) : (xpos & 0x7);\n"
    "        int color;\n"
    "        if ((tilexoff & 0x1) != 0)\n"
    "            color = int(bfetch(u_bgvram, (pixelsaddr + (tilexoff >> 1)) & mask)) >> 4;\n"
    "        else\n"
    "            color = int(bfetch(u_bgvram, (pixelsaddr + (tilexoff >> 1)) & mask)) & 0x0F;\n"
    "        if (color == 0) return 0u;\n"
    "        return pal16(palbase + color * 2) | (0x01000000u << bgnum);\n"
    "    }\n"
    "}\n"
    "\n"
    "uint bg_affine_pixel(int bgnum, int x, int xmt, int rotX0, int rotY0) {\n"
    "    uint bgcnt = u_bgcnt[bgnum];\n"
    "    int coordmask, yshift;\n"
    "    switch ((bgcnt >> 14) & 0x3u) {\n"
    "        case 0u: coordmask = 0x07800; yshift = 7; break;\n"
    "        case 1u: coordmask = 0x0F800; yshift = 8; break;\n"
    "        case 2u: coordmask = 0x1F800; yshift = 9; break;\n"
    "        default: coordmask = 0x3F800; yshift = 10; break;\n"
    "    }\n"
    "    int overflowmask = ((bgcnt & (1u << 13)) != 0u) ? 0 : ~(coordmask | 0x7FF);\n"
    "    int rotA = u_bgrota[bgnum - 2];\n"
    "    int rotC = u_bgrotc[bgnum - 2];\n"
    "    int finalX = rotX0 - xmt * rotA;\n"
    "    int finalY = rotY0 - xmt * rotC;\n"
    "    int tilesetaddr, tilemapaddr, paloff;\n"
    "    if (u_enginenum != 0) {\n"
    "        tilesetaddr = int((bgcnt & 0x003Cu) << 12);\n"
    "        tilemapaddr = int((bgcnt & 0x1F00u) << 3);\n"
    "        paloff = 0x400;\n"
    "    } else {\n"
    "        tilesetaddr = int(((u_dispcnt & 0x07000000u) >> 8) + ((bgcnt & 0x003Cu) << 12));\n"
    "        tilemapaddr = int(((u_dispcnt & 0x38000000u) >> 11) + ((bgcnt & 0x1F00u) << 3));\n"
    "        paloff = 0;\n"
    "    }\n"
    "    yshift -= 3;\n"
    "    if (((finalX | finalY) & overflowmask) != 0) return 0u;\n"
    "    int mask = (u_enginenum != 0) ? 0x1FFFF : 0x7FFFF;\n"
    "    int yidx = int((uint(finalY) & uint(coordmask)) >> 11);\n"
    "    int xidx = int((uint(finalX) & uint(coordmask)) >> 11);\n"
    "    uint curtile = bfetch(u_bgvram, (tilemapaddr + (yidx << yshift) + xidx) & mask);\n"
    "    int tilexoff = (finalX >> 8) & 0x7;\n"
    "    int tileyoff = (finalY >> 8) & 0x7;\n"
    "    int color = int(bfetch(u_bgvram, (tilesetaddr + (int(curtile) << 6) + (tileyoff << 3) + tilexoff) & mask));\n"
    "    if (color == 0) return 0u;\n"
    "    return pal16(paloff + color * 2) | (0x01000000u << bgnum);\n"
    "}\n"
    "\n"
    "uint bg_extended_pixel(int bgnum, int x, int xmt, int rotX0, int rotY0) {\n"
    "    uint bgcnt = u_bgcnt[bgnum];\n"
    "    bool extpal = (u_dispcnt & (1u << 30)) != 0u;\n"
    "    int mask = (u_enginenum != 0) ? 0x1FFFF : 0x7FFFF;\n"
    "    int rotA = u_bgrota[bgnum - 2];\n"
    "    int rotC = u_bgrotc[bgnum - 2];\n"
    "    int finalX = rotX0 - xmt * rotA;\n"
    "    int finalY = rotY0 - xmt * rotC;\n"
    "    int tilesetaddr = 0, tilemapaddr = 0, paloff;\n"
    "    if (u_enginenum != 0) {\n"
    "        tilesetaddr = int((bgcnt & 0x003Cu) << 12);\n"
    "        tilemapaddr = int((bgcnt & 0x1F00u) << 3);\n"
    "        paloff = 0x400;\n"
    "    } else {\n"
    "        tilesetaddr = int(((u_dispcnt & 0x07000000u) >> 8) + ((bgcnt & 0x003Cu) << 12));\n"
    "        tilemapaddr = int(((u_dispcnt & 0x38000000u) >> 11) + ((bgcnt & 0x1F00u) << 3));\n"
    "        paloff = 0;\n"
    "    }\n"
    "    if ((bgcnt & (1u << 7)) != 0u) {\n"
    "        /* bitmap modes */\n"
    "        int xmask, ymask, yshift;\n"
    "        switch ((bgcnt >> 14) & 0x3u) {\n"
    "            case 0u: xmask = 0x07FFF; ymask = 0x07FFF; yshift = 7; break;\n"
    "            case 1u: xmask = 0x0FFFF; ymask = 0x0FFFF; yshift = 8; break;\n"
    "            case 2u: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;\n"
    "            default: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;\n"
    "        }\n"
    "        int ofxmask = ((bgcnt & (1u << 13)) != 0u) ? 0 : ~xmask;\n"
    "        int ofymask = ((bgcnt & (1u << 13)) != 0u) ? 0 : ~ymask;\n"
    "        tilemapaddr = int((bgcnt & 0x1F00u) << 6);\n"
    "        if (((uint(finalX) & uint(ofxmask)) != 0u) || ((uint(finalY) & uint(ofymask)) != 0u))\n"
    "            return 0u;\n"
    "        if ((bgcnt & (1u << 2)) != 0u) {\n"
    "            int yidx = int((uint(finalY) & uint(ymask)) >> 8);\n"
    "            int xidx = int((uint(finalX) & uint(xmask)) >> 8);\n"
    "            int bmpaddr = tilemapaddr + ((yidx << yshift) + xidx) * 2;\n"
    "            uint color = r16(u_bgvram, bmpaddr & mask);\n"
    "            if ((color & 0x8000u) == 0u) return 0u;\n"
    "            return (color & 0x7FFFu) | (0x01000000u << bgnum);\n"
    "        } else {\n"
    "            int yidx = int((uint(finalY) & uint(ymask)) >> 8);\n"
    "            int xidx = int((uint(finalX) & uint(xmask)) >> 8);\n"
    "            int bmpaddr = tilemapaddr + (yidx << yshift) + xidx;\n"
    "            int color = int(bfetch(u_bgvram, bmpaddr & mask));\n"
    "            if (color == 0) return 0u;\n"
    "            return pal16(paloff + color * 2) | (0x01000000u << bgnum);\n"
    "        }\n"
    "    }\n"
    "    /* mixed affine/text */\n"
    "    {\n"
    "        int coordmask, yshift;\n"
    "        switch ((bgcnt >> 14) & 0x3u) {\n"
    "            case 0u: coordmask = 0x07800; yshift = 7; break;\n"
    "            case 1u: coordmask = 0x0F800; yshift = 8; break;\n"
    "            case 2u: coordmask = 0x1F800; yshift = 9; break;\n"
    "            default: coordmask = 0x3F800; yshift = 10; break;\n"
    "        }\n"
    "        int overflowmask = ((bgcnt & (1u << 13)) != 0u) ? 0 : ~(coordmask | 0x7FF);\n"
    "        yshift -= 3;\n"
    "        if (((finalX | finalY) & overflowmask) != 0) return 0u;\n"
    "        int yidx = int((uint(finalY) & uint(coordmask)) >> 11);\n"
    "        int xidx = int((uint(finalX) & uint(coordmask)) >> 11);\n"
    "        uint curtile = r16(u_bgvram, (tilemapaddr + (((yidx << yshift) + xidx) << 1)) & mask);\n"
    "        int palbase = paloff;\n"
    "        if (extpal) palbase = bgnum * 8192 + int(curtile >> 12) * 512 + 0x100000;\n"
    "        int tilexoff = (finalX >> 8) & 0x7;\n"
    "        int tileyoff = (finalY >> 8) & 0x7;\n"
    "        if ((curtile & (1u << 10)) != 0u) tilexoff = 7 - tilexoff;\n"
    "        if ((curtile & (1u << 11)) != 0u) tileyoff = 7 - tileyoff;\n"
    "        int color = int(bfetch(u_bgvram, (tilesetaddr + (int(curtile & 0x03FFu) << 6) + (tileyoff << 3) + tilexoff) & mask));\n"
    "        if (color == 0) return 0u;\n"
    "        uint c = extpal ? r16(u_extpal_bg, (palbase + color * 2) & 0x7FFF)\n"
    "                        : pal16(palbase + color * 2);\n"
    "        return c | (0x01000000u << bgnum);\n"
    "    }\n"
    "}\n"
    "\n"
    "uint bg_large_pixel(int x, int xmt, int rotX0, int rotY0) {\n"
    "    uint bgcnt = u_bgcnt[2];\n"
    "    int xmask, ymask, yshift;\n"
    "    switch ((bgcnt >> 14) & 0x3u) {\n"
    "        case 0u: xmask = 0x1FFFF; ymask = 0x3FFFF; yshift = 9; break;\n"
    "        case 1u: xmask = 0x3FFFF; ymask = 0x1FFFF; yshift = 10; break;\n"
    "        case 2u: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;\n"
    "        default: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;\n"
    "    }\n"
    "    int ofxmask = ((bgcnt & (1u << 13)) != 0u) ? 0 : ~xmask;\n"
    "    int ofymask = ((bgcnt & (1u << 13)) != 0u) ? 0 : ~ymask;\n"
    "    int rotA = u_bgrota[0];\n"
    "    int rotC = u_bgrotc[0];\n"
    "    int finalX = rotX0 - xmt * rotA;\n"
    "    int finalY = rotY0 - xmt * rotC;\n"
    "    int mask = (u_enginenum != 0) ? 0x1FFFF : 0x7FFFF;\n"
    "    int paloff = (u_enginenum != 0) ? 0x400 : 0;\n"
    "    if (((uint(finalX) & uint(ofxmask)) != 0u) || ((uint(finalY) & uint(ofymask)) != 0u))\n"
    "        return 0u;\n"
    "    int yidx = int((uint(finalY) & uint(ymask)) >> 8);\n"
    "    int xidx = int((uint(finalX) & uint(xmask)) >> 8);\n"
    "    int color = int(bfetch(u_bgvram, (((yidx << yshift) + xidx) & mask)));\n"
    "    if (color == 0) return 0u;\n"
    "    return pal16(paloff + color * 2) | (0x01000000u << 2);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    int x = int(gl_FragCoord.x);\n"
    "    int line = 191 - int(gl_FragCoord.y);\n"
    "    ivec4 lat0 = texelFetch(u_latch, ivec2(0, line), 0);\n"
    "    ivec4 lat1 = texelFetch(u_latch, ivec2(1, line), 0);\n"
    "    int bgmos_line = lat1.x;\n"
    "    int win0init = lat1.z, win1init = lat1.w;\n"
    "\n"
    "    /* window mask (eng_calculate_window_mask; no windows -> all layers) */\n"
    "    uint wmask = ((u_dispcnt & 0xE000u) != 0u) ? u_wincnt[2] : 0xFFu;\n"
    "    uint objpix = texelFetch(u_objimg_s, ivec2(x, line), 0).r;\n"
    "    if ((u_dispcnt & (1u << 15)) != 0u) {\n"
    "        if ((objpix & OBJ_WINDOW_BIT) != 0u) wmask = u_wincnt[3];\n"
    "    }\n"
    "    if ((u_dispcnt & (1u << 14)) != 0u) {\n"
    "        if (((win1init & 1) != 0) && x_active(x, u_win1[0], u_win1[1], (win1init >> 1) & 1))\n"
    "            wmask = u_wincnt[1];\n"
    "    }\n"
    "    if ((u_dispcnt & (1u << 13)) != 0u) {\n"
    "        if (((win0init & 1) != 0) && x_active(x, u_win0[0], u_win0[1], (win0init >> 1) & 1))\n"
    "            wmask = u_wincnt[0];\n"
    "    }\n"
    "\n"
    "    /* backdrop */\n"
    "    uint bd = pal16((u_enginenum != 0) ? 0x400 : 0);\n"
    "    uint top = expand5(bd) | 0x20000000u;\n"
    "    uint second = 0u;\n"
    "\n"
    "    uint dispCnt = u_dispcnt;\n"
    "    int mode = int(dispCnt & 0x7u);\n"
    "    bool mos = (u_bgmosx > 0);\n"
    "\n"
    "    for (int prio = 3; prio >= 0; prio--) {\n"
    "        for (int bgnum = 3; bgnum >= 0; bgnum--) {\n"
    "            bool want = false;\n"
    "            if (mode == 6) {\n"
    "                if (bgnum == 2) want = ((u_bgcnt[2] & 0x3u) == uint(prio)) && (((u_dispcnt >> 8) & 4u) != 0u);\n"
    "                else if (bgnum == 0) want = ((u_bgcnt[0] & 0x3u) == uint(prio)) && (((u_dispcnt >> 8) & 1u) != 0u)\n"
    "                    && u_enginenum == 0 && (dispCnt & 0x8u) != 0u; /* mode 6 BG0 = 3D only */\n"
    "            } else if (mode == 7) {\n"
    "                if (bgnum == 1) want = ((u_bgcnt[1] & 0x3u) == uint(prio)) && (((u_dispcnt >> 8) & 2u) != 0u);\n"
    "                else if (bgnum == 0) want = ((u_bgcnt[0] & 0x3u) == uint(prio)) && (((u_dispcnt >> 8) & 1u) != 0u);\n"
    "            } else {\n"
    "                if (bgnum >= 0 && ((u_dispcnt >> 8) & (1u << bgnum)) != 0u && (u_bgcnt[bgnum] & 0x3u) == uint(prio))\n"
    "                    want = true;\n"
    "            }\n"
    "            if (!want) continue;\n"
    "            bool mosaic = ((u_bgcnt[bgnum] & (1u << 6)) != 0u) && mos;\n"
    "            int xmt = mosaic ? (x % (u_bgmosx + 1)) : 0;\n"
    "            uint c = 0u;\n"
    "            bool is3d = false;\n"
    "            if (mode == 6) {\n"
    "                if (bgnum == 2) c = bg_large_pixel(x, xmt, lat0.x + x * u_bgrota[0], lat0.y + x * u_bgrotc[0]);\n"
    "                else is3d = true;\n"
    "            } else if (mode == 7) {\n"
    "                if (bgnum == 1) c = bg_text_pixel(1, x, line, bgmos_line, xmt);\n"
    "                else {\n"
    "                    if (u_enginenum == 0 && (dispCnt & 0x8u) != 0u) is3d = true;\n"
    "                    else c = bg_text_pixel(0, x, line, bgmos_line, xmt);\n"
    "                }\n"
    "            } else {\n"
    "                if (bgnum == 3) {\n"
    "                    if (mode >= 3) c = bg_extended_pixel(3, x, xmt, lat0.z + x * u_bgrota[1], lat0.w + x * u_bgrotc[1]);\n"
    "                    else if (mode >= 1) c = bg_affine_pixel(3, x, xmt, lat0.z + x * u_bgrota[1], lat0.w + x * u_bgrotc[1]);\n"
    "                    else c = bg_text_pixel(3, x, line, bgmos_line, xmt);\n"
    "                } else if (bgnum == 2) {\n"
    "                    if (mode == 5) c = bg_extended_pixel(2, x, xmt, lat0.x + x * u_bgrota[0], lat0.y + x * u_bgrotc[0]);\n"
    "                    else if (mode == 4 || mode == 2) c = bg_affine_pixel(2, x, xmt, lat0.x + x * u_bgrota[0], lat0.y + x * u_bgrotc[0]);\n"
    "                    else c = bg_text_pixel(2, x, line, bgmos_line, xmt);\n"
    "                } else if (bgnum == 1) {\n"
    "                    c = bg_text_pixel(1, x, line, bgmos_line, xmt);\n"
    "                } else {\n"
    "                    if (u_enginenum == 0 && (dispCnt & 0x8u) != 0u) is3d = true;\n"
    "                    else c = bg_text_pixel(0, x, line, bgmos_line, xmt);\n"
    "                }\n"
    "            }\n"
    "            if (is3d) {\n"
    "                uint c3 = texelFetch(u_fb3d, ivec2(x, line), 0).r;\n"
    "                if ((c3 >> 24) != 0u && (wmask & 0x01u) != 0u) {\n"
    "                    second = top;\n"
    "                    top = c3 | 0x40000000u;\n"
    "                }\n"
    "                continue;\n"
    "            }\n"
    "            if (c != 0u && (wmask & (1u << bgnum)) != 0u) {\n"
    "                second = top;\n"
    "                top = expand5(c & 0xFFFFu) | (c & 0xFF000000u); /* draw_pixel */\n"
    "            }\n"
    "        }\n"
    "        /* interleave_sprites(prio): sprite winner of this priority\n"
    "         * (apply_sprite_mosaic_x latch first, as draw_scanline_bgobj) */\n"
    "        if (((u_dispcnt >> 8) & 0x10u) != 0u) {\n"
    "            uint cur = objpix;\n"
    "            if (u_objmosx > 0) {\n"
    "                int mosw = u_objmosx;\n"
    "                int blk = x - (x % (mosw + 1));\n"
    "                uint latchcolor = 0u;\n"
    "                int mosx = 0;\n"
    "                for (int j = blk; j <= x; j++) {\n"
    "                    uint cc = texelFetch(u_objimg_s, ivec2(j, line), 0).r;\n"
    "                    int latch = 0;\n"
    "                    if (mosx == 0) latch = 1;\n"
    "                    else if ((cc & OBJ_MOSAIC_FLAG) == 0u) latch = 1;\n"
    "                    else if ((latchcolor & OBJ_MOSAIC_FLAG) == 0u) latch = 1;\n"
    "                    else if ((cc & OBJ_BGPRIO_MASK) < (latchcolor & OBJ_BGPRIO_MASK)) latch = 1;\n"
    "                    if (latch != 0) latchcolor = cc;\n"
    "                    if (mosx == mosw) mosx = 0; else mosx++;\n"
    "                }\n"
    "                cur = latchcolor;\n"
    "            }\n"
    "            if ((cur & (OBJ_BGPRIO_MASK | OBJ_IS_OPAQUE)) ==\n"
    "                ((uint(prio) << 16) | OBJ_IS_OPAQUE) && (wmask & 0x10u) != 0u) {\n"
    "                uint color;\n"
    "                if ((cur & 0x8000u) != 0u) color = cur & 0x7FFFu;\n"
    "                else if ((cur & 0x1000u) != 0u) color = pal16(((u_enginenum != 0) ? 0x600 : 0x200) + int(cur & 0xFFu) * 2);\n"
    "                else color = r16(u_extpal_obj, int(cur & 0xFFFu) * 2);\n"
    "                second = top;\n"
    "                top = expand5(color) | (cur & 0xFF000000u);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "\n"
    "    uint outv = color_composite(x, top, second, wmask);\n"
    "    imageStore(u_out, ivec2(x, line), uvec4(outv));\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* C driver                                                            */

struct GxGl2d {
    SDL_Window *window;
    SDL_GLContext ctx;
    GLuint prog_sprite, prog_comp;
    GLuint tex_bgvram, tex_objvram, tex_palette, tex_extbg, tex_extobj;
    GLuint tex_oam, tex_fb3d, tex_latch, tex_objimg, tex_out;
    GLuint fbo, rbo, vao;
    GxGl2dStats stats;
};

/* per-scanline latch model (mirror of eng_pre_draw/eng_post_draw/
 * eng_update_windows/eng_run_frame in gx_2d.c) */
typedef struct {
    int vcount;
    int32_t bgxref[2], bgyref[2], bgxrefint[2], bgyrefint[2];
    uint8_t bgmosaic_y, bgmosaic_ymax, bgmosaic_latch;
    uint32_t bgmosaic_line;
    uint8_t objmosaic_y, objmosaic_latch;
    uint32_t objmosaic_line;
    uint8_t win0active, win1active;
} LatchState;

static void latch_pre_draw(const Gx2DEngine *e, LatchState *ls, int reset)
{
    if (ls->bgmosaic_latch)
        ls->bgmosaic_line = reset ? 262u : (uint32_t)ls->vcount;
    for (int i = 0; i < 2; i++) {
        if (!(e->bgcnt[2 + i] & (1 << 6)) || ls->bgmosaic_latch) {
            ls->bgxrefint[i] = ls->bgxref[i];
            ls->bgyrefint[i] = ls->bgyref[i];
        }
    }
    if (e->dispcnt & (1u << 12)) {
        if (reset || (ls->objmosaic_y == e->objmosaic[1])) {
            ls->objmosaic_y = 0;
            ls->objmosaic_latch = 1;
        } else {
            ls->objmosaic_y = (uint8_t)((ls->objmosaic_y + 1) & 0xF);
            ls->objmosaic_latch = 0;
        }
    }
    if (ls->objmosaic_latch)
        ls->objmosaic_line = reset ? 0u : (uint32_t)(ls->vcount + 1);
}

static void latch_post_draw(const Gx2DEngine *e, LatchState *ls, int reset)
{
    if (reset) {
        ls->bgmosaic_ymax = e->bgmosaic[1];
        ls->bgmosaic_y = 0;
        ls->bgmosaic_latch = 1;
    } else {
        if (ls->bgmosaic_y == ls->bgmosaic_ymax) {
            ls->bgmosaic_ymax = e->bgmosaic[1];
            ls->bgmosaic_y = 0;
            ls->bgmosaic_latch = 1;
        } else {
            ls->bgmosaic_y = (uint8_t)((ls->bgmosaic_y + 1) & 0xF);
            ls->bgmosaic_latch = 0;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (!(((e->dispcnt >> 8) & 0x1F) & (4u << i)))
            continue; /* layer_enable gate (eng_post_draw) */
        if (reset) {
            ls->bgxref[i] = e->bgxrefreload[i];
            ls->bgyref[i] = e->bgyrefreload[i];
        } else {
            ls->bgxref[i] += e->bgrotb[i];
            ls->bgyref[i] += e->bgrotd[i];
        }
    }
}

static void latch_update_windows(const Gx2DEngine *e, LatchState *ls, int line)
{
    line &= 0xFF;
    if (line == e->win0coords[3])      ls->win0active &= (uint8_t)~0x1;
    else if (line == e->win0coords[2]) ls->win0active |= 0x1;
    if (line == e->win1coords[3])      ls->win1active &= (uint8_t)~0x1;
    else if (line == e->win1coords[2]) ls->win1active |= 0x1;
}

static int x_active_c(int x, int x1, int x2, int init)
{
    int last = -1, st = init;
    if (x1 <= x) { last = x1; st = 1; }
    if (x2 <= x && x2 >= last) { st = 0; }
    return st;
}

/* ---- GL plumbing ---- */

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    char log[8192];
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderInfoLog(s, sizeof(log), &len, log);
        fprintf(stderr, "gx_gl2d: %s compile failed:\n%.*s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", (int)len, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *fs_src)
{
    static const char *vs_src =
        "#version 430 core\n"
        "void main() {\n"
        "    vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
        "                    (gl_VertexID == 2) ? 3.0 : -1.0);\n"
        "    gl_Position = vec4(pos, 0.0, 1.0);\n"
        "}\n";
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint p;
    GLint ok = 0;
    if (!vs || !fs) return 0;
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[8192];
        GLint len = 0;
        glGetProgramInfoLog(p, sizeof(log), &len, log);
        fprintf(stderr, "gx_gl2d: link failed:\n%.*s\n", (int)len, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint make_tex_u8(int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, w, h, 0, GL_RED_INTEGER,
                 GL_UNSIGNED_BYTE, NULL);
    return t;
}

static GLuint make_tex_u32(int w, int h)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, w, h, 0, GL_RED_INTEGER,
                 GL_UNSIGNED_INT, NULL);
    return t;
}

static void upload_u8(GLuint t, int w, int h, const void *data)
{
    glBindTexture(GL_TEXTURE_2D, t);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED_INTEGER,
                    GL_UNSIGNED_BYTE, data);
}

/* build the fragment source: preamble + body */
static char *build_src(const char *body)
{
    const char *marker = strstr(body, "\n");
    size_t prelen = strlen(glsl_pre);
    size_t bodylen = strlen(body);
    char *s = (char *)malloc(prelen + bodylen + 2);
    if (!s) return NULL;
    memcpy(s, glsl_pre, prelen);
    /* body starts with "#version 430 core\n" -- drop it (preamble has it) */
    memcpy(s + prelen, marker ? marker + 1 : body, bodylen - (marker ? (size_t)(marker + 1 - body) : 0));
    s[prelen + bodylen - (marker ? (size_t)(marker + 1 - body) : 0)] = 0;
    return s;
}

GxGl2d *gx_gl2d_open(void)
{
    GxGl2d *g = (GxGl2d *)calloc(1, sizeof(GxGl2d));
    char *src;
    if (!g) return NULL;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "gx_gl2d: SDL_Init: %s\n", SDL_GetError());
        free(g);
        return NULL;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 0);
    g->window = SDL_CreateWindow("gx-gl2d", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, 256, 192,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!g->window) { fprintf(stderr, "gx_gl2d: window: %s\n", SDL_GetError()); free(g); return NULL; }
    g->ctx = SDL_GL_CreateContext(g->window);
    if (!g->ctx) { fprintf(stderr, "gx_gl2d: context: %s\n", SDL_GetError()); SDL_DestroyWindow(g->window); free(g); return NULL; }
    SDL_GL_MakeCurrent(g->window, g->ctx);

    src = build_src(sprite_fs_src);
    g->prog_sprite = src ? link_program(src) : 0;
    free(src);
    src = build_src(comp_fs_src);
    g->prog_comp = src ? link_program(src) : 0;
    free(src);
    if (!g->prog_sprite || !g->prog_comp) {
        gx_gl2d_close(g);
        return NULL;
    }
    if (getenv("GX_GL2D_DEBUG")) {
        fprintf(stderr, "dbg locs: u_bgcnt[0]=%d u_bgcnt[1]=%d u_bgcnt=%d "
                "u_bgxpos[1]=%d u_wincnt[2]=%d u_dispcnt=%d\n",
                glGetUniformLocation(g->prog_comp, "u_bgcnt[0]"),
                glGetUniformLocation(g->prog_comp, "u_bgcnt[1]"),
                glGetUniformLocation(g->prog_comp, "u_bgcnt"),
                glGetUniformLocation(g->prog_comp, "u_bgxpos[1]"),
                glGetUniformLocation(g->prog_comp, "u_wincnt[2]"),
                glGetUniformLocation(g->prog_comp, "u_dispcnt"));
    }

    g->tex_bgvram = make_tex_u8(1024, 512);
    g->tex_objvram = make_tex_u8(1024, 256);
    g->tex_palette = make_tex_u8(1024, 2);
    g->tex_extbg = make_tex_u8(1024, 32);
    g->tex_extobj = make_tex_u8(1024, 8);
    g->tex_oam = make_tex_u8(1024, 1);
    g->tex_fb3d = make_tex_u32(256, 192);
    g->tex_objimg = make_tex_u32(256, 192);
    g->tex_out = make_tex_u32(256, 192);
    glGenTextures(1, &g->tex_latch);
    glBindTexture(GL_TEXTURE_2D, g->tex_latch);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, 2, 192, 0, GL_RGBA_INTEGER,
                 GL_INT, NULL);

    glGenFramebuffers(1, &g->fbo);
    glGenRenderbuffers(1, &g->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, g->rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 256, 192);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, g->rbo);
    glGenVertexArrays(1, &g->vao);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, 256, 192);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    return g;
}

void gx_gl2d_close(GxGl2d *g)
{
    if (!g) return;
    if (g->ctx) {
        GLuint texs[] = { g->tex_bgvram, g->tex_objvram, g->tex_palette,
                          g->tex_extbg, g->tex_extobj, g->tex_oam, g->tex_fb3d,
                          g->tex_latch, g->tex_objimg, g->tex_out };
        glDeleteTextures(10, texs);
        if (g->rbo) glDeleteRenderbuffers(1, &g->rbo);
        if (g->fbo) glDeleteFramebuffers(1, &g->fbo);
        if (g->vao) glDeleteVertexArrays(1, &g->vao);
        if (g->prog_sprite) glDeleteProgram(g->prog_sprite);
        if (g->prog_comp) glDeleteProgram(g->prog_comp);
        SDL_GL_DeleteContext(g->ctx);
    }
    if (g->window) SDL_DestroyWindow(g->window);
    SDL_Quit();
    free(g);
}

const char *gx_gl2d_renderer(GxGl2d *g)
{
    (void)g;
    return (const char *)glGetString(GL_RENDERER);
}
const char *gx_gl2d_version(GxGl2d *g)
{
    (void)g;
    return (const char *)glGetString(GL_VERSION);
}
void gx_gl2d_get_stats(GxGl2d *g, GxGl2dStats *out)
{
    *out = g->stats;
}

/* ---- display tail (gx2d_render_screen post loop, verbatim semantics) ---- */

static uint32_t tail_blend4(uint32_t v1, uint32_t v2, uint32_t eva, uint32_t evb)
{
    uint32_t r = (((v1 & 0x3F) * eva) + ((v2 & 0x3F) * evb) + 0x08) >> 4;
    uint32_t g = ((((v1 & 0x3F00) * eva) + ((v2 & 0x3F00) * evb) + 0x0800) >> 4) & 0x7F00;
    uint32_t b = ((((v1 & 0x3F0000) * eva) + ((v2 & 0x3F0000) * evb) + 0x080000) >> 4) & 0x7F0000;
    if (r > 0x3F) r = 0x3F;
    if (g > 0x3F00) g = 0x3F00;
    if (b > 0x3F0000) b = 0x3F0000;
    return r | g | b | 0xFF000000;
}

static void tail_brightness(uint16_t regval, uint32_t *dst)
{
    uint16_t mode = (uint16_t)(regval >> 14);
    uint32_t factor = regval & 0x1F;
    if (factor > 16) factor = 16;
    if (mode == 1) {
        for (int i = 0; i < 256; i++) {
            uint32_t val = dst[i];
            uint32_t rb = val & 0x3F003F, g = val & 0x3F00;
            rb += (((((0x3F003F - rb) * factor) + 0) >> 4) & 0x3F003F);
            g += (((((0x3F00 - g) * factor) + 0) >> 4) & 0x3F00);
            dst[i] = rb | g | 0xFF000000;
        }
    } else if (mode == 2) {
        for (int i = 0; i < 256; i++) {
            uint32_t val = dst[i];
            uint32_t rb = val & 0x3F003F, g = val & 0x3F00;
            rb -= ((((rb * factor) + (0xF * 0x010001)) >> 4) & 0x3F003F);
            g -= ((((g * factor) + (0xF * 0x000100)) >> 4) & 0x3F00);
            dst[i] = rb | g | 0xFF000000;
        }
    }
    (void)tail_blend4;
}

static void tail_expand(uint32_t *dst)
{
    for (int i = 0; i < 256; i++) {
        uint32_t c = dst[i];
        uint32_t r = (c << 18) & 0xFC0000;
        uint32_t g = (c << 2) & 0xFC00;
        uint32_t b = (c >> 14) & 0xFC;
        c = r | g | b;
        c |= ((c & 0x00C0C0C0) >> 6) | 0xFF000000;
        dst[i] = c;
    }
}

/* ---- main entry ---- */

int gx_gl2d_render_screen(GxGl2d *g, const Gx2DState *st, int engine,
                          const uint32_t *fb3d, uint32_t *dst)
{
    const Gx2DEngine *e = &st->eng[engine];
    int32_t latchdata[2 * 192 * 4];
    uint32_t *outline = (uint32_t *)malloc(256 * 192 * 4);
    LatchState ls;
    if (!outline) return -1;

    /* texture uploads (state snapshot for this unit) */
    upload_u8(g->tex_bgvram, 1024, engine ? 128 : 512, st->bgvram[engine]);
    upload_u8(g->tex_objvram, 1024, engine ? 128 : 256, st->objvram[engine]);
    upload_u8(g->tex_palette, 1024, 2, st->palette);
    upload_u8(g->tex_extbg, 1024, 32, st->bgextpal[engine]);
    upload_u8(g->tex_extobj, 1024, 8, st->objextpal[engine]);
    upload_u8(g->tex_oam, 1024, 1, st->oam + (engine ? 0x400 : 0));
    if (fb3d) {
        glBindTexture(GL_TEXTURE_2D, g->tex_fb3d);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RED_INTEGER,
                        GL_UNSIGNED_INT, fb3d);
    } else {
        static const uint32_t zeros[256];
        glBindTexture(GL_TEXTURE_2D, g->tex_fb3d);
        for (int y = 0; y < 192; y++)
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, y, 256, 1, GL_RED_INTEGER,
                            GL_UNSIGNED_INT, zeros);
    }

    /* per-scanline latch model */
    memset(&ls, 0, sizeof(ls));
    ls.vcount = 262;
    ls.bgmosaic_ymax = e->bgmosaic[1];
    ls.bgmosaic_y = 0;
    ls.bgmosaic_latch = 1;
    ls.objmosaic_y = 0;
    ls.objmosaic_latch = 1;
    ls.objmosaic_line = 0;
    for (int i = 0; i < 2; i++) {
        ls.bgxref[i] = e->bgxref[i];
        ls.bgyref[i] = e->bgyref[i];
    }
    ls.win0active = e->win0active;
    ls.win1active = e->win1active;
    latch_pre_draw(e, &ls, 1);
    int objmos_sprites0 = (int)ls.objmosaic_line;
    latch_post_draw(e, &ls, 1);

    int objmos_for_sprites[192];
    objmos_for_sprites[0] = objmos_sprites0;

    for (int L = 0; L < 192; L++) {
        ls.vcount = L;
        if (L > 0)
            latch_update_windows(e, &ls, L);
        latch_pre_draw(e, &ls, 0);

        int32_t *t0 = &latchdata[(L * 2 + 0) * 4];
        int32_t *t1 = &latchdata[(L * 2 + 1) * 4];
        t0[0] = ls.bgxrefint[0]; t0[1] = ls.bgyrefint[0];
        t0[2] = ls.bgxrefint[1]; t0[3] = ls.bgyrefint[1];
        t1[0] = (int32_t)ls.bgmosaic_line;
        t1[1] = objmos_for_sprites[L];
        t1[2] = ls.win0active;
        t1[3] = ls.win1active;

        if (L < 191) {
            /* draw_sprites(L+1) runs before post_draw(L) and uses the
             * objmosaic_line value latched by pre_draw(L) */
            objmos_for_sprites[L + 1] = (int)ls.objmosaic_line;
        }

        /* x-latch carry into the next line (eng_calculate_window_mask) */
        if (e->dispcnt & 0xE000) {
            if (e->dispcnt & (1u << 14))
                ls.win1active = (uint8_t)((ls.win1active & ~2) |
                    (x_active_c(255, e->win1coords[0], e->win1coords[1],
                                (ls.win1active >> 1) & 1) ? 2 : 0));
            if (e->dispcnt & (1u << 13))
                ls.win0active = (uint8_t)((ls.win0active & ~2) |
                    (x_active_c(255, e->win0coords[0], e->win0coords[1],
                                (ls.win0active >> 1) & 1) ? 2 : 0));
        }
        latch_post_draw(e, &ls, 0);
    }

    glBindTexture(GL_TEXTURE_2D, g->tex_latch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 192, GL_RGBA_INTEGER, GL_INT,
                    latchdata);

    /* pass 1: sprite layer */
    glUseProgram(g->prog_sprite);
    glBindImageTexture(0, g->tex_objimg, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
    glUniform1i(glGetUniformLocation(g->prog_sprite, "u_oam"), 0);
    glUniform1i(glGetUniformLocation(g->prog_sprite, "u_objvram"), 1);
    glUniform1i(glGetUniformLocation(g->prog_sprite, "u_extpal_obj"), 2);
    glUniform1i(glGetUniformLocation(g->prog_sprite, "u_bgvram"), 3);
    glUniform1i(glGetUniformLocation(g->prog_sprite, "u_palette"), 4);
    glUniform1i(glGetUniformLocation(g->prog_sprite, "u_extpal_bg"), 5);
    glUniform1i(glGetUniformLocation(g->prog_sprite, "u_fb3d"), 6);
    glUniform1i(glGetUniformLocation(g->prog_sprite, "u_latch"), 7);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g->tex_oam);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, g->tex_objvram);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, g->tex_extobj);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, g->tex_bgvram);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, g->tex_palette);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, g->tex_extbg);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, g->tex_fb3d);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D, g->tex_latch);
    glUniform1ui(glGetUniformLocation(g->prog_sprite, "u_dispcnt"), e->dispcnt);
    glUniform1i(glGetUniformLocation(g->prog_sprite, "u_enginenum"), engine);
    glBindVertexArray(g->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    g->stats.sprite_passes++;

    /* pass 2: compositor */
    glUseProgram(g->prog_comp);
    glBindImageTexture(0, g->tex_out, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_objimg_s"), 8);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_bgvram"), 3);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_objvram"), 1);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_palette"), 4);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_extpal_bg"), 5);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_extpal_obj"), 2);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_oam"), 0);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_fb3d"), 6);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_latch"), 7);
    glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D, g->tex_objimg);
    glUniform1ui(glGetUniformLocation(g->prog_comp, "u_dispcnt"), e->dispcnt);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_enginenum"), engine);
    glUniform1ui(glGetUniformLocation(g->prog_comp, "u_blendcnt"), e->blendcnt);
    glUniform1ui(glGetUniformLocation(g->prog_comp, "u_eva"), e->eva);
    glUniform1ui(glGetUniformLocation(g->prog_comp, "u_evb"), e->evb);
    glUniform1ui(glGetUniformLocation(g->prog_comp, "u_evy"), e->evy);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_bgmosx"), e->bgmosaic[0]);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_bgmosy"), e->bgmosaic[1]);
    glUniform1i(glGetUniformLocation(g->prog_comp, "u_objmosx"), e->objmosaic[0]);
    {
        char name[32];
        int i;
        for (i = 0; i < 4; i++) {
            snprintf(name, sizeof(name), "u_bgcnt[%d]", i);
            glUniform1ui(glGetUniformLocation(g->prog_comp, name), e->bgcnt[i]);
            snprintf(name, sizeof(name), "u_bgxpos[%d]", i);
            glUniform1i(glGetUniformLocation(g->prog_comp, name), e->bgxpos[i]);
            snprintf(name, sizeof(name), "u_bgypos[%d]", i);
            glUniform1i(glGetUniformLocation(g->prog_comp, name), e->bgypos[i]);
            snprintf(name, sizeof(name), "u_win0[%d]", i);
            glUniform1i(glGetUniformLocation(g->prog_comp, name), e->win0coords[i]);
            snprintf(name, sizeof(name), "u_win1[%d]", i);
            glUniform1i(glGetUniformLocation(g->prog_comp, name), e->win1coords[i]);
            snprintf(name, sizeof(name), "u_wincnt[%d]", i);
            glUniform1ui(glGetUniformLocation(g->prog_comp, name), e->wincnt[i]);
        }
        for (i = 0; i < 2; i++) {
            snprintf(name, sizeof(name), "u_bgrota[%d]", i);
            glUniform1i(glGetUniformLocation(g->prog_comp, name), e->bgrota[i]);
            snprintf(name, sizeof(name), "u_bgrotc[%d]", i);
            glUniform1i(glGetUniformLocation(g->prog_comp, name), e->bgrotc[i]);
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glFinish();

    /* read back the 6-bit outline */
    glBindTexture(GL_TEXTURE_2D, g->tex_out);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, outline);

    /* display tail per line */
    for (int L = 0; L < 192; L++) {
        uint32_t *out = dst + L * 256;
        uint32_t *line = outline + L * 256;
        uint32_t dispcnt = e->dispcnt;
        int disp_mode = (int)((dispcnt >> 16) & (engine ? 0x1 : 0x3));
        int skip_brightness = 0;
        if (!st->eng_enabled[engine]) {
            uint32_t fillcolor = engine ? 0xFF3F3F3Fu : 0x3F3F3Fu;
            for (int i = 0; i < 256; i++) line[i] = fillcolor;
        } else if (e->dispcnt & (1u << 7)) {
            for (int i = 0; i < 256; i++) line[i] = 0xFF3F3F3Fu;
        }
        switch (disp_mode) {
        case 0:
            for (int i = 0; i < 256; i++)
                out[i] = engine ? 0xFF3F3F3Fu : 0x3F3F3Fu;
            skip_brightness = 1;
            break;
        case 1:
            memcpy(out, line, 256 * 4);
            break;
        case 2: {
            uint32_t vrambank = (dispcnt >> 18) & 0x3;
            int mapped = (st->vramcnt[vrambank] & 0x80) &&
                         ((st->vramcnt[vrambank] & 0x3) == 0);
            if (mapped) {
                const uint16_t *vram = (const uint16_t *)(st->vram_raw +
                                        vrambank * 0x20000);
                vram = &vram[L * 256];
                for (int i = 0; i < 256; i++) {
                    uint16_t color = vram[i];
                    uint8_t r = (uint8_t)((color & 0x001F) << 1);
                    uint8_t gg = (uint8_t)((color & 0x03E0) >> 4);
                    uint8_t b = (uint8_t)((color & 0x7C00) >> 9);
                    out[i] = (uint32_t)r | ((uint32_t)gg << 8) | ((uint32_t)b << 16);
                }
            } else {
                for (int i = 0; i < 256; i++) out[i] = 0;
            }
            break;
        }
        case 3: {
            const uint16_t *fifo = st->dispfifo;
            for (int i = 0; i < 256; i++) {
                uint16_t color = fifo[i];
                uint8_t r = (uint8_t)((color & 0x001F) << 1);
                uint8_t gg = (uint8_t)((color & 0x03E0) >> 4);
                uint8_t b = (uint8_t)((color & 0x7C00) >> 9);
                out[i] = (uint32_t)r | ((uint32_t)gg << 8) | ((uint32_t)b << 16);
            }
            break;
        }
        }
        if (!skip_brightness)
            tail_brightness(st->masterbright[engine], out);
        tail_expand(out);
    }
    free(outline);
    g->stats.frames++;
    return 0;
}
