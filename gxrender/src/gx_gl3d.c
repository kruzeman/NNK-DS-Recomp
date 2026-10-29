/* gx_gl3d.c — OpenGL 4.3-core render engine for the DS GX pipeline.
 *
 * GL rasterizes the polygon triangles; the fragment shader ports the
 * software rasterizer's per-pixel body (gx_raster.c) 1:1 against image
 * load/store buffers:
 *   - manual depth test (LessThan / LessThan_FrontFacing tie rule /
 *     Equal_Z|Equal_W band, POLYGON_ATTR bit 14)
 *   - AA second layer: depth-fail fallthrough, opaque push-down,
 *     translucent twin-plot (GPU3D_Soft.cpp:1142-1201)
 *   - shading: gx_shade_pixel (modulate / decal / toon / highlight,
 *     1-bit alpha, wireframe alpha 31, texture alpha modulation)
 *   - translucent plot: plot_translucent + alpha_blend verbatim
 *     (translucent polygon-ID skip rule, fog flag inheritance, depth
 *     write only with POLYGON_ATTR bit 11)
 *   - post pass: edge marking (incl. break-AA-coverage write), fog with
 *     second-layer fog, AA final blend (gx_post_pass ports)
 *
 * Integer semantics where the reference is integer (colors packed r6|g6<<8|
 * b6<<16|a5<<24, depth in melonDS FinalZ scale).  Divergences from the
 * software path are limited to (a) float perspective-correct interpolation
 * vs the DS 8/9-bit perspective factor, (b) GL pixel-center fill vs the
 * melonDS span fill (sliver edge pixels), both classified by
 * tools/gl_vs_soft.py.
 *
 * Rasterization convention: GxPolygon screen coords (sx, sy) are integer
 * pixel positions mapped to pixel CORNERS (window x = sx, y = 192 - sy), so
 * GL samples at pixel centers (sx+0.5, sy+0.5) in DS coords and vertical
 * edges at integer x fill exactly like the software span fill.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GL_GLEXT_PROTOTYPES 1
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "gx_gl3d.h"
#include "gx_tex.h"

#define FB_W GX_SCREEN_W
#define FB_H GX_SCREEN_H

#define TEX_CACHE_MAX 64

/* ------------------------------------------------------------------ */
/* shaders                                                             */

static const char *poly_vs_src =
    "#version 430 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in float a_depth;\n"
    "layout(location=2) in vec3 a_col;\n"
    "layout(location=3) in vec2 a_st;\n"
    "layout(location=4) in float a_wn;\n"
    "out float v_invw;\n"
    "noperspective out vec3 v_colw;\n"
    "noperspective out vec2 v_stw;\n"
    "noperspective out float v_z;\n"
    "noperspective out float v_zw;\n"
    "void main() {\n"
    "    float nx = (a_pos.x / 256.0) * 2.0 - 1.0;\n"
    "    float ny = 1.0 - (a_pos.y / 192.0) * 2.0;\n"
    "    gl_Position = vec4(nx, ny, 0.0, 1.0);\n"
    "    float iw = 1.0 / a_wn;\n"
    "    v_invw = iw;\n"
    "    v_colw = a_col * iw;\n"
    "    v_stw = a_st * iw;\n"
    "    v_z = a_depth;\n"
    "    v_zw = a_depth * iw;\n"
    "}\n";

static const char *poly_fs_src =
    "#version 430 core\n"
    "layout(r32ui) uniform uimage2D u_col0;\n"
    "layout(r32ui) uniform uimage2D u_z0;\n"
    "layout(r32ui) uniform uimage2D u_attr0;\n"
    "layout(r32ui) uniform uimage2D u_col1;\n"
    "layout(r32ui) uniform uimage2D u_z1;\n"
    "layout(r32ui) uniform uimage2D u_attr1;\n"
    "uniform usampler2D u_tex;\n"
    "uniform isampler2D u_span;\n"
    "uniform uint u_attr;\n"
    "uniform int u_ytop;\n"
    "uniform int u_ybot;\n"
    "uniform bool u_wbuffer;\n"
    "uniform bool u_facing;\n"
    "uniform bool u_textured;\n"
    "uniform bool u_highlight;\n"
    "uniform bool u_alpha_blend;\n"
    "uniform bool u_aa;\n"
    "uniform int u_alpha_ref;\n"
    "uniform uint u_texparam;\n"
    "uniform ivec2 u_texsize;\n"
    "uniform uint u_toon[32];\n"
    "in float v_invw;\n"
    "noperspective in vec3 v_colw;\n"
    "noperspective in vec2 v_stw;\n"
    "noperspective in float v_z;\n"
    "noperspective in float v_zw;\n"
    "\n"
    "bool depth_pass(int z, uint dstattr, int dstz) {\n"
    "    if ((u_attr & (1u << 14)) != 0u) {\n"
    "        int lim = u_wbuffer ? 0xFF : 0x200;\n"
    "        uint diff = uint(dstz - z + lim);\n"
    "        return diff <= uint(lim * 2);\n"
    "    }\n"
    "    if (u_facing && (dstattr & 0x00400010u) == 0x00000010u)\n"
    "        return z <= dstz;\n"
    "    return z < dstz;\n"
    "}\n"
    "\n"
    "uint alpha_blend(uint src, uint dst, uint alpha) {\n"
    "    uint dstalpha = dst >> 24;\n"
    "    if (dstalpha == 0u) return src;\n"
    "    uint srcR = src & 0x3Fu, srcG = (src >> 8) & 0x3Fu, srcB = (src >> 16) & 0x3Fu;\n"
    "    if (u_alpha_blend) {\n"
    "        uint dstR = dst & 0x3Fu, dstG = (dst >> 8) & 0x3Fu, dstB = (dst >> 16) & 0x3Fu;\n"
    "        alpha++;\n"
    "        srcR = (srcR * alpha + dstR * (32u - alpha)) >> 5;\n"
    "        srcG = (srcG * alpha + dstG * (32u - alpha)) >> 5;\n"
    "        srcB = (srcB * alpha + dstB * (32u - alpha)) >> 5;\n"
    "        alpha--;\n"
    "    }\n"
    "    if (alpha > dstalpha) dstalpha = alpha;\n"
    "    return srcR | (srcG << 8) | (srcB << 16) | (dstalpha << 24);\n"
    "}\n"
    "\n"
    "void plot_translucent(uimage2D colImg, uimage2D zImg, uimage2D attrImg,\n"
    "                      ivec2 p, uint color, int z, uint polyattr) {\n"
    "    uint dstattr = imageLoad(attrImg, p).r;\n"
    "    uint attr = (polyattr & 0xE0F0u) | ((polyattr >> 8) & 0xFF0000u) |\n"
    "                (1u << 22) | (dstattr & 0xFF001F0Fu);\n"
    "    if ((dstattr & 0x007F0000u) == (attr & 0x007F0000u))\n"
    "        return;\n"
    "    if ((dstattr & (1u << 15)) == 0u)\n"
    "        attr &= ~(1u << 15);\n"
    "    color = alpha_blend(color, imageLoad(colImg, p).r, color >> 24);\n"
    "    if (z != -1)\n"
    "        imageStore(zImg, p, uvec4(uint(z)));\n"
    "    imageStore(colImg, p, uvec4(color));\n"
    "    imageStore(attrImg, p, uvec4(attr));\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    int px = int(gl_FragCoord.x);\n"
    "    int py = 191 - int(gl_FragCoord.y);\n"
    "    ivec2 p = ivec2(px, py);\n"
    "\n"
    "    /* sample-point correction: the software rasterizer interpolates\n"
    "     * attributes at integer pixel coords (pixel centers at integers,\n"
    "     * melonDS convention); GL rasterizes at pixel centers (x+0.5,\n"
    "     * y+0.5).  Undo the half-pixel offset on the linearly-interpolated\n"
    "     * varyings (exact for linear interpolation), then combine.\n"
    "     * Note dFdy is in GL's bottom-up y; DS rows run top-down, so the\n"
    "     * y term is +0.5*dFdy. */\n"
    "    vec3 colw = v_colw + 0.5 * (dFdy(v_colw) - dFdx(v_colw));\n"
    "    float invw = v_invw + 0.5 * (dFdy(v_invw) - dFdx(v_invw));\n"
    "    vec2 stw = v_stw + 0.5 * (dFdy(v_stw) - dFdx(v_stw));\n"
    "    float zl = v_z + 0.5 * (dFdy(v_z) - dFdx(v_z));\n"
    "    float zw = v_zw + 0.5 * (dFdy(v_zw) - dFdx(v_zw));\n"
    "\n"
    "    float z_f = u_wbuffer ? (zw / invw) : zl;\n"
    "    int z = int(floor(z_f + 0.5));\n"
    "\n"
    "    /* fill classification from the software rasterizer's recorded span\n"
    "     * parameters (gx_raster_span_params): part 1 = left edge run\n"
    "     * (flag 0x1), part 2 = interior (flag 0), part 3 = right edge run\n"
    "     * (flag 0x2), each with the exact AA coverage model (constant or\n"
    "     * accumulated edge coverage; interior = 31 on yedge rows). */\n"
    "    ivec4 s0 = texelFetch(u_span, ivec2(0, py), 0);\n"
    "    ivec4 s1 = texelFetch(u_span, ivec2(1, py), 0);\n"
    "    ivec4 s2 = texelFetch(u_span, ivec2(2, py), 0);\n"
    "    int a_lo = s0.x, a_hi = s0.y, b_lo = s0.z, b_hi = s0.w;\n"
    "    int c_lo = s1.x, c_hi = s1.y, l_cov0 = s1.z, l_incr = s1.w;\n"
    "    int r_cov0 = s2.x, r_incr = s2.y, l_mode = s2.z, r_mode = s2.w;\n"
    "    uint yedge = (py == u_ytop) ? 0x4u : ((py == u_ybot - 1) ? 0x8u : 0u);\n"
    "    uint edge = 0u;\n"
    "    uint cov5 = 0u;\n"
    "    if (px >= a_lo && px < a_hi) {\n"
    "        edge = 0x1u | yedge;\n"
    "        if (l_mode == 0) cov5 = uint(l_cov0) & 0x1Fu;\n"
    "        else {\n"
    "            int cov = (l_cov0 + (px - a_lo) * l_incr) >> 5;\n"
    "            cov5 = uint(clamp(cov, 0, 31));\n"
    "        }\n"
    "    } else if (px >= b_lo && px < b_hi) {\n"
    "        edge = yedge;\n"
    "        cov5 = (yedge != 0u) ? 31u : 0u;\n"
    "    } else if (px >= c_lo && px < c_hi) {\n"
    "        edge = 0x2u | yedge;\n"
    "        if (r_mode == 0) cov5 = uint(r_cov0) & 0x1Fu;\n"
    "        else {\n"
    "            int cov = 0x1F - ((r_cov0 + (px - c_lo) * r_incr) >> 5);\n"
    "            cov5 = uint(clamp(cov, 0, 31));\n"
    "        }\n"
    "    } else {\n"
    "        discard; /* outside the software fill (GL raster slivers) */\n"
    "    }\n"
    "\n"
    "    uint polyattr = (u_attr & 0x3F008000u) | (u_facing ? 0u : 0x10u);\n"
    "\n"
    "    uint dstattr = imageLoad(u_attr0, p).r;\n"
    "    int dstz = int(imageLoad(u_z0, p).r);\n"
    "    bool layer2 = false;\n"
    "    if (!depth_pass(z, dstattr, dstz)) {\n"
    "        if ((dstattr & 0xFu) == 0u) discard;\n"
    "        layer2 = true;\n"
    "        dstattr = imageLoad(u_attr1, p).r;\n"
    "        dstz = int(imageLoad(u_z1, p).r);\n"
    "        if (!depth_pass(z, dstattr, dstz)) discard;\n"
    "    }\n"
    "\n"
    "    vec3 vin = colw / invw;\n"
    "    uint vr = uint(max(vin.x, 0.0) + 0.001) >> 3;\n"
    "    uint vg = uint(max(vin.y, 0.0) + 0.001) >> 3;\n"
    "    uint vb = uint(max(vin.z, 0.0) + 0.001) >> 3;\n"
    "    uint blendmode = (u_attr >> 4) & 3u;\n"
    "    uint polyalpha = (u_attr >> 16) & 0x1Fu;\n"
    "    bool wireframe = polyalpha == 0u;\n"
    "\n"
    "    if (blendmode == 2u) {\n"
    "        if (u_highlight) { vg = vr; vb = vr; }\n"
    "        else {\n"
    "            uint tc = u_toon[vr >> 1];\n"
    "            uint r = (tc << 1) & 0x3Eu; if (r != 0u) r++;\n"
    "            uint gg = (tc >> 4) & 0x3Eu; if (gg != 0u) gg++;\n"
    "            uint b = (tc >> 9) & 0x3Eu; if (b != 0u) b++;\n"
    "            vr = r; vg = gg; vb = b;\n"
    "        }\n"
    "    }\n"
    "\n"
    "    uint cr, cg, cb, ca;\n"
    "    if (u_textured) {\n"
    "        vec2 st = stw / invw;\n"
    "        int s = int(floor(st.x / 16.0 + 0.001));\n"
    "        int t = int(floor(st.y / 16.0 + 0.001));\n"
    "        int w = u_texsize.x, h = u_texsize.y;\n"
    "        if ((u_texparam & (1u << 16)) != 0u) {\n"
    "            if ((u_texparam & (1u << 18)) != 0u) {\n"
    "                if ((s & w) != 0) s = (w - 1) - (s & (w - 1));\n"
    "                else s = s & (w - 1);\n"
    "            } else s &= w - 1;\n"
    "        } else {\n"
    "            if (s < 0) s = 0; else if (s >= w) s = w - 1;\n"
    "        }\n"
    "        if ((u_texparam & (1u << 17)) != 0u) {\n"
    "            if ((u_texparam & (1u << 19)) != 0u) {\n"
    "                if ((t & h) != 0) t = (h - 1) - (t & (h - 1));\n"
    "                else t = t & (h - 1);\n"
    "            } else t &= h - 1;\n"
    "        } else {\n"
    "            if (t < 0) t = 0; else if (t >= h) t = h - 1;\n"
    "        }\n"
    "        uvec4 tx = texelFetch(u_tex, ivec2(s, t), 0);\n"
    "        uint tr = tx.r, tg = tx.g, tb = tx.b, talpha = tx.a;\n"
    "        if ((blendmode & 1u) != 0u) {\n"
    "            if (talpha == 0u) { cr = vr; cg = vg; cb = vb; }\n"
    "            else if (talpha == 31u) { cr = tr; cg = tg; cb = tb; }\n"
    "            else {\n"
    "                cr = (tr * talpha + vr * (31u - talpha)) >> 5;\n"
    "                cg = (tg * talpha + vg * (31u - talpha)) >> 5;\n"
    "                cb = (tb * talpha + vb * (31u - talpha)) >> 5;\n"
    "            }\n"
    "            ca = polyalpha;\n"
    "        } else {\n"
    "            cr = ((tr + 1u) * (vr + 1u) - 1u) >> 6;\n"
    "            cg = ((tg + 1u) * (vg + 1u) - 1u) >> 6;\n"
    "            cb = ((tb + 1u) * (vb + 1u) - 1u) >> 6;\n"
    "            ca = ((talpha + 1u) * (polyalpha + 1u) - 1u) >> 5;\n"
    "        }\n"
    "    } else {\n"
    "        cr = vr; cg = vg; cb = vb; ca = polyalpha;\n"
    "    }\n"
    "\n"
    "    if (blendmode == 2u && u_highlight) {\n"
    "        uint tc = u_toon[vr >> 1];\n"
    "        uint hvr = (tc << 1) & 0x3Eu; if (hvr != 0u) hvr++;\n"
    "        uint hvg = (tc >> 4) & 0x3Eu; if (hvg != 0u) hvg++;\n"
    "        uint hvb = (tc >> 9) & 0x3Eu; if (hvb != 0u) hvb++;\n"
    "        cr = min(cr + hvr, 63u);\n"
    "        cg = min(cg + hvg, 63u);\n"
    "        cb = min(cb + hvb, 63u);\n"
    "    }\n"
    "    if (wireframe) ca = 31u;\n"
    "\n"
    "    if (int(ca) <= u_alpha_ref) discard;\n"
    "    uint color = cr | (cg << 8) | (cb << 16) | (ca << 24);\n"
    "\n"
    "    if (ca == 31u) {\n"
    "        uint attr = polyattr | edge;\n"
    "        if (u_aa && edge != 0u) {\n"
    "            attr |= cov5 << 8;\n"
    "            if (!layer2) {\n"
    "                imageStore(u_col1, p, imageLoad(u_col0, p));\n"
    "                imageStore(u_z1, p, imageLoad(u_z0, p));\n"
    "                imageStore(u_attr1, p, imageLoad(u_attr0, p));\n"
    "            }\n"
    "        }\n"
    "        if (layer2) {\n"
    "            imageStore(u_z1, p, uvec4(uint(z)));\n"
    "            imageStore(u_col1, p, uvec4(color));\n"
    "            imageStore(u_attr1, p, uvec4(attr));\n"
    "        } else {\n"
    "            imageStore(u_z0, p, uvec4(uint(z)));\n"
    "            imageStore(u_col0, p, uvec4(color));\n"
    "            imageStore(u_attr0, p, uvec4(attr));\n"
    "        }\n"
    "    } else {\n"
    "        int zw = ((u_attr & (1u << 11)) != 0u) ? z : -1;\n"
    "        if (layer2) {\n"
    "            plot_translucent(u_col1, u_z1, u_attr1, p, color, zw, polyattr);\n"
    "        } else {\n"
    "            plot_translucent(u_col0, u_z0, u_attr0, p, color, zw, polyattr);\n"
    "            if ((dstattr & 0xFu) != 0u)\n"
    "                plot_translucent(u_col1, u_z1, u_attr1, p, color, zw, polyattr);\n"
    "        }\n"
    "    }\n"
    "}\n";

static const char *fsq_vs_src =
    "#version 430 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 pos = vec2((gl_VertexID == 1) ? 3.0 : -1.0,\n"
    "                    (gl_VertexID == 2) ? 3.0 : -1.0);\n"
    "    v_uv = pos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

/* edge marking (gx_post_pass edge loop) */
static const char *edge_fs_src =
    "#version 430 core\n"
    "layout(r32ui) uniform uimage2D u_col0;\n"
    "layout(r32ui) uniform uimage2D u_z0;\n"
    "layout(r32ui) uniform uimage2D u_attr0;\n"
    "uniform uint u_edge[8];\n"
    "uniform uint u_clear_attr;\n"
    "uniform uint u_clear_depth;\n"
    "in vec2 v_uv;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(int(gl_FragCoord.x), 191 - int(gl_FragCoord.y));\n"
    "    uint attr = imageLoad(u_attr0, p).r;\n"
    "    if ((attr & 0xFu) == 0u) return;\n"
    "    uint polyid = attr >> 24;\n"
    "    uint z = imageLoad(u_z0, p).r;\n"
    "    bool mark = false;\n"
    "    const int dx[4] = int[4](-1, 1, 0, 0);\n"
    "    const int dy[4] = int[4](0, 0, -1, 1);\n"
    "    for (int i = 0; i < 4 && !mark; i++) {\n"
    "        ivec2 np = p + ivec2(dx[i], dy[i]);\n"
    "        uint npolyid, nz;\n"
    "        if (np.x < 0 || np.x >= 256 || np.y < 0 || np.y >= 192) {\n"
    "            npolyid = u_clear_attr >> 24;\n"
    "            nz = u_clear_depth;\n"
    "        } else {\n"
    "            npolyid = imageLoad(u_attr0, np).r >> 24;\n"
    "            nz = imageLoad(u_z0, np).r;\n"
    "        }\n"
    "        if (polyid != npolyid && z < nz) mark = true;\n"
    "    }\n"
    "    if (!mark) return;\n"
    "    uint ec = u_edge[polyid >> 3];\n"
    "    uint er = (ec << 1) & 0x3Eu; if (er != 0u) er++;\n"
    "    uint eg = (ec >> 4) & 0x3Eu; if (eg != 0u) eg++;\n"
    "    uint eb = (ec >> 9) & 0x3Eu; if (eb != 0u) eb++;\n"
    "    uint col = imageLoad(u_col0, p).r;\n"
    "    col = er | (eg << 8) | (eb << 16) | (col & 0xFF000000u);\n"
    "    imageStore(u_col0, p, uvec4(col));\n"
    "    imageStore(u_attr0, p, uvec4((attr & 0xFFFFE0FFu) | 0x00001000u));\n"
    "}\n";

/* fog (gx_post_pass fog loop, top pixel + second layer) */
static const char *fog_fs_src =
    "#version 430 core\n"
    "layout(r32ui) uniform uimage2D u_col0;\n"
    "layout(r32ui) uniform uimage2D u_z0;\n"
    "layout(r32ui) uniform uimage2D u_attr0;\n"
    "layout(r32ui) uniform uimage2D u_col1;\n"
    "layout(r32ui) uniform uimage2D u_z1;\n"
    "layout(r32ui) uniform uimage2D u_attr1;\n"
    "uniform uint u_fog_density[34];\n"
    "uniform uint u_fog_offset;\n"
    "uniform int u_fog_shift;\n"
    "uniform uint u_fogR, u_fogG, u_fogB, u_fogA;\n"
    "uniform bool u_fogcolor;\n"
    "in vec2 v_uv;\n"
    "\n"
    "uint fog_density(uint z) {\n"
    "    uint id, frac;\n"
    "    if (z < u_fog_offset) { id = 0u; frac = 0u; }\n"
    "    else {\n"
    "        z -= u_fog_offset;\n"
    "        z = (z >> 2) << uint(u_fog_shift);\n"
    "        id = z >> 17;\n"
    "        if (id >= 32u) { id = 32u; frac = 0u; }\n"
    "        else frac = z & 0x1FFFFu;\n"
    "    }\n"
    "    uint d = (u_fog_density[id] * (0x20000u - frac) +\n"
    "              u_fog_density[id + 1u] * frac) >> 17;\n"
    "    if (d >= 127u) d = 128u;\n"
    "    return d;\n"
    "}\n"
    "\n"
    "uint fog_blend(uint col, uint density) {\n"
    "    uint r = col & 0x3Fu, g = (col >> 8) & 0x3Fu, b = (col >> 16) & 0x3Fu;\n"
    "    uint a = (col >> 24) & 0x1Fu;\n"
    "    if (u_fogcolor) {\n"
    "        r = (u_fogR * density + r * (128u - density)) >> 7;\n"
    "        g = (u_fogG * density + g * (128u - density)) >> 7;\n"
    "        b = (u_fogB * density + b * (128u - density)) >> 7;\n"
    "    }\n"
    "    a = (u_fogA * density + a * (128u - density)) >> 7;\n"
    "    return r | (g << 8) | (b << 16) | (a << 24);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    ivec2 p = ivec2(int(gl_FragCoord.x), 191 - int(gl_FragCoord.y));\n"
    "    uint attr = imageLoad(u_attr0, p).r;\n"
    "    if ((attr & (1u << 15)) != 0u) {\n"
    "        uint d = fog_density(imageLoad(u_z0, p).r);\n"
    "        imageStore(u_col0, p, uvec4(fog_blend(imageLoad(u_col0, p).r, d)));\n"
    "    }\n"
    "    if ((attr & 0xFu) != 0u && (imageLoad(u_attr1, p).r & (1u << 15)) != 0u) {\n"
    "        uint d = fog_density(imageLoad(u_z1, p).r);\n"
    "        imageStore(u_col1, p, uvec4(fog_blend(imageLoad(u_col1, p).r, d)));\n"
    "    }\n"
    "}\n";

/* antialiasing final blend (gx_post_pass AA loop) */
static const char *aa_fs_src =
    "#version 430 core\n"
    "layout(r32ui) uniform uimage2D u_col0;\n"
    "layout(r32ui) uniform uimage2D u_attr0;\n"
    "layout(r32ui) uniform uimage2D u_col1;\n"
    "in vec2 v_uv;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(int(gl_FragCoord.x), 191 - int(gl_FragCoord.y));\n"
    "    uint attr = imageLoad(u_attr0, p).r;\n"
    "    if ((attr & 0xFu) == 0u) return;\n"
    "    uint coverage = (attr >> 8) & 0x1Fu;\n"
    "    if (coverage == 0x1Fu) return;\n"
    "    uint top = imageLoad(u_col0, p).r;\n"
    "    uint bot = imageLoad(u_col1, p).r;\n"
    "    if (coverage == 0u) { imageStore(u_col0, p, uvec4(bot)); return; }\n"
    "    uint topR = top & 0x3Fu, topG = (top >> 8) & 0x3Fu, topB = (top >> 16) & 0x3Fu;\n"
    "    uint topA = (top >> 24) & 0x1Fu;\n"
    "    uint botR = bot & 0x3Fu, botG = (bot >> 8) & 0x3Fu, botB = (bot >> 16) & 0x3Fu;\n"
    "    uint botA = (bot >> 24) & 0x1Fu;\n"
    "    coverage++;\n"
    "    if (botA > 0u) {\n"
    "        topR = (topR * coverage + botR * (32u - coverage)) >> 5;\n"
    "        topG = (topG * coverage + botG * (32u - coverage)) >> 5;\n"
    "        topB = (topB * coverage + botB * (32u - coverage)) >> 5;\n"
    "    }\n"
    "    topA = (topA * coverage + botA * (32u - coverage)) >> 5;\n"
    "    imageStore(u_col0, p, uvec4(topR | (topG << 8) | (topB << 16) | (topA << 24)));\n"
    "}\n";

/* present: image -> window RGBA */
static const char *present_fs_src =
    "#version 430 core\n"
    "layout(r32ui) uniform readonly uimage2D u_col0;\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(int(v_uv.x * 256.0), int((1.0 - v_uv.y) * 192.0));\n"
    "    uint c = imageLoad(u_col0, p).r;\n"
    "    o_color = vec4(float(c & 63u) / 63.0, float((c >> 8) & 63u) / 63.0,\n"
    "                   float((c >> 16) & 63u) / 63.0, 1.0);\n"
    "}\n";

/* ------------------------------------------------------------------ */
/* engine state                                                        */

typedef struct {
    uint32_t texparam, texpal;
    GLuint tex;
    int w, h;
    int in_use;
} TexCacheEntry;

struct GxGl3d {
    SDL_Window *window;
    SDL_GLContext ctx;
    int win_w, win_h;

    GLuint prog_poly, prog_edge, prog_fog, prog_aa, prog_present;
    GLuint fbo, rbo;                 /* 256x192 dummy raster target */
    GLuint img_col[2], img_z[2], img_attr[2];
    GLuint span_tex;
    GLuint vao, vbo;

    TexCacheEntry cache[TEX_CACHE_MAX];
    int cache_next;

    GxRasterEnv env;
    GxPostEnv post;
    int have_env;

    GxGl3dStats stats;
};

/* vertex interleaving: pos(2) depth(1) col(3) st(2) wn(1) = 9 floats */
#define VTX_FLOATS 9

static void die_shader(const char *what, GLuint obj, int is_program)
{
    char log[4096];
    GLint len = 0;
    if (is_program) glGetProgramInfoLog(obj, sizeof(log), &len, log);
    else glGetShaderInfoLog(obj, sizeof(log), &len, log);
    fprintf(stderr, "gx_gl3d: %s failed:\n%.*s\n", what, (int)len, log);
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        die_shader(type == GL_VERTEX_SHADER ? "vertex shader compile"
                                            : "fragment shader compile", s, 0);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    GLuint p;
    GLint ok = 0;
    if (!vs || !fs)
        return 0;
    p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        die_shader("program link", p, 1);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

static GLuint make_image(uint32_t *zero_init)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, FB_W, FB_H, 0,
                 GL_RED_INTEGER, GL_UNSIGNED_INT, zero_init);
    return t;
}

/* image uniforms default to image unit 0; bind each buffer to its own unit */
static void set_image_units(GLuint prog, const char *const *names,
                            const int *units, int n)
{
    int i;
    glUseProgram(prog);
    for (i = 0; i < n; i++)
        glUniform1i(glGetUniformLocation(prog, names[i]), units[i]);
}

static void bind_all_image_units(GxGl3d *g)
{
    static const char *const n6[6] = { "u_col0", "u_z0", "u_attr0",
                                       "u_col1", "u_z1", "u_attr1" };
    static const int u6[6] = { 0, 1, 2, 3, 4, 5 };
    static const char *const n3[3] = { "u_col0", "u_z0", "u_attr0" };
    static const int u3[3] = { 0, 1, 2 };
    static const char *const naa[3] = { "u_col0", "u_attr0", "u_col1" };
    static const int uaa[3] = { 0, 1, 2 };
    static const char *const n1[1] = { "u_col0" };
    static const int u1[1] = { 0 };
    set_image_units(g->prog_poly, n6, u6, 6);
    set_image_units(g->prog_edge, n3, u3, 3);
    set_image_units(g->prog_fog, n6, u6, 6);
    set_image_units(g->prog_aa, naa, uaa, 3);
    set_image_units(g->prog_present, n1, u1, 1);
}

GxGl3d *gx_gl3d_open(int present_scale)
{
    GxGl3d *g;
    uint32_t *zero;
    int i;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "gx_gl3d: SDL_Init: %s\n", SDL_GetError());
        return NULL;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, present_scale > 0 ? 1 : 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    g = (GxGl3d *)calloc(1, sizeof(GxGl3d));
    if (!g) return NULL;
    if (present_scale < 1) present_scale = 1;
    g->win_w = FB_W * present_scale;
    g->win_h = FB_H * present_scale;

    g->window = SDL_CreateWindow("gxrender-gl3d",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 g->win_w, g->win_h,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!g->window) {
        fprintf(stderr, "gx_gl3d: SDL_CreateWindow: %s\n", SDL_GetError());
        free(g);
        return NULL;
    }
    g->ctx = SDL_GL_CreateContext(g->window);
    if (!g->ctx) {
        fprintf(stderr, "gx_gl3d: SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(g->window);
        free(g);
        return NULL;
    }
    SDL_GL_MakeCurrent(g->window, g->ctx);

    g->prog_poly = link_program(poly_vs_src, poly_fs_src);
    g->prog_edge = link_program(fsq_vs_src, edge_fs_src);
    g->prog_fog = link_program(fsq_vs_src, fog_fs_src);
    g->prog_aa = link_program(fsq_vs_src, aa_fs_src);
    g->prog_present = link_program(fsq_vs_src, present_fs_src);
    if (!g->prog_poly || !g->prog_edge || !g->prog_fog || !g->prog_aa ||
        !g->prog_present) {
        gx_gl3d_close(g);
        return NULL;
    }
    bind_all_image_units(g);

    zero = (uint32_t *)calloc((size_t)FB_W * FB_H, 4);
    if (!zero) { gx_gl3d_close(g); return NULL; }
    for (i = 0; i < 2; i++) {
        g->img_col[i] = make_image(zero);
        g->img_z[i] = make_image(zero);
        g->img_attr[i] = make_image(zero);
    }
    free(zero);

    glGenTextures(1, &g->span_tex);
    glBindTexture(GL_TEXTURE_2D, g->span_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32I, 3, FB_H, 0,
                 GL_RGBA_INTEGER, GL_INT, NULL);

    glGenFramebuffers(1, &g->fbo);
    glGenRenderbuffers(1, &g->rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, g->rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, FB_W, FB_H);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, g->rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gx_gl3d: FBO incomplete\n");
        gx_gl3d_close(g);
        return NULL;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, FB_W, FB_H);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    glGenVertexArrays(1, &g->vao);
    glGenBuffers(1, &g->vbo);
    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER, 16 * VTX_FLOATS * sizeof(float), NULL,
                 GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, VTX_FLOATS * sizeof(float),
                          (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, VTX_FLOATS * sizeof(float),
                          (void *)(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, VTX_FLOATS * sizeof(float),
                          (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, VTX_FLOATS * sizeof(float),
                          (void *)(6 * sizeof(float)));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, VTX_FLOATS * sizeof(float),
                          (void *)(8 * sizeof(float)));
    glBindVertexArray(0);
    return g;
}

void gx_gl3d_close(GxGl3d *g)
{
    int i;
    if (!g) return;
    for (i = 0; i < TEX_CACHE_MAX; i++)
        if (g->cache[i].in_use)
            glDeleteTextures(1, &g->cache[i].tex);
    if (g->ctx) {
        if (g->img_col[0]) glDeleteTextures(2, g->img_col);
        if (g->img_z[0]) glDeleteTextures(2, g->img_z);
        if (g->img_attr[0]) glDeleteTextures(2, g->img_attr);
        if (g->span_tex) glDeleteTextures(1, &g->span_tex);
        if (g->vbo) glDeleteBuffers(1, &g->vbo);
        if (g->vao) glDeleteVertexArrays(1, &g->vao);
        if (g->rbo) glDeleteRenderbuffers(1, &g->rbo);
        if (g->fbo) glDeleteFramebuffers(1, &g->fbo);
        if (g->prog_poly) glDeleteProgram(g->prog_poly);
        if (g->prog_edge) glDeleteProgram(g->prog_edge);
        if (g->prog_fog) glDeleteProgram(g->prog_fog);
        if (g->prog_aa) glDeleteProgram(g->prog_aa);
        if (g->prog_present) glDeleteProgram(g->prog_present);
        SDL_GL_DeleteContext(g->ctx);
    }
    if (g->window) SDL_DestroyWindow(g->window);
    SDL_Quit();
    free(g);
}

const char *gx_gl3d_vendor(GxGl3d *g)
{
    (void)g;
    return (const char *)glGetString(GL_VENDOR);
}
const char *gx_gl3d_renderer(GxGl3d *g)
{
    (void)g;
    return (const char *)glGetString(GL_RENDERER);
}
const char *gx_gl3d_version(GxGl3d *g)
{
    (void)g;
    return (const char *)glGetString(GL_VERSION);
}

void gx_gl3d_get_stats(GxGl3d *g, GxGl3dStats *out)
{
    *out = g->stats;
}

/* ------------------------------------------------------------------ */
/* frame setup                                                         */

void gx_gl3d_load_clear(GxGl3d *g, const uint32_t *color, const uint32_t *depth,
                        const uint32_t *attr)
{
    glBindTexture(GL_TEXTURE_2D, g->img_col[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FB_W, FB_H, GL_RED_INTEGER,
                    GL_UNSIGNED_INT, color);
    glBindTexture(GL_TEXTURE_2D, g->img_z[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FB_W, FB_H, GL_RED_INTEGER,
                    GL_UNSIGNED_INT, depth);
    glBindTexture(GL_TEXTURE_2D, g->img_attr[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, FB_W, FB_H, GL_RED_INTEGER,
                    GL_UNSIGNED_INT, attr);
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
}

void gx_gl3d_set_env(GxGl3d *g, const GxRasterEnv *env, const GxPostEnv *post)
{
    int i;
    g->env = *env;   /* struct copy; env->toon contents copied below */
    g->post = *post;
    g->have_env = 1;
    /* per-frame texture cache invalidation: VRAM/palette snapshots change
     * per unit, so nothing carries over */
    for (i = 0; i < TEX_CACHE_MAX; i++)
        g->cache[i].in_use = 0;
    g->cache_next = 0;
}

/* ------------------------------------------------------------------ */
/* texture decode cache (CPU decode via gx_tex.c, exact integer path)  */

static TexCacheEntry *tex_for(GxGl3d *g, const GxPolygon *poly)
{
    uint32_t texparam = poly->texparam, texpal = poly->texpal;
    int width = 8 << ((texparam >> 20) & 0x7);
    int height = 8 << ((texparam >> 23) & 0x7);
    TexCacheEntry *e = NULL;
    uint8_t *data;
    int i, x, y;

    for (i = 0; i < TEX_CACHE_MAX; i++) {
        if (g->cache[i].in_use && g->cache[i].texparam == texparam &&
            g->cache[i].texpal == texpal)
            return &g->cache[i];
    }
    e = &g->cache[g->cache_next];
    g->cache_next = (g->cache_next + 1) % TEX_CACHE_MAX;
    if (e->in_use)
        glDeleteTextures(1, &e->tex);
    e->in_use = 0;

    data = (uint8_t *)malloc((size_t)width * height * 4);
    if (!data)
        return NULL;
    {
        /* decode the base image with wrap forced on (same trick as
         * gx_tex_decode_rgba); per-texel values are packed r6/g6/b6/a5
         * exactly like the software lookup produces */
        uint32_t tp = texparam | (1u << 16) | (1u << 17);
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                uint16_t color;
                uint8_t alpha, r, g2, b;
                int vis = gx_tex_lookup(g->env.tex, tp, texpal,
                                        (int32_t)((x << 4) | 8),
                                        (int32_t)((y << 4) | 8),
                                        &color, &alpha);
                if (!vis) { r = g2 = b = 0; alpha = 0; }
                else {
                    r = gx_c5_to_c6((uint8_t)(color & 0x1F));
                    g2 = gx_c5_to_c6((uint8_t)((color >> 5) & 0x1F));
                    b = gx_c5_to_c6((uint8_t)((color >> 10) & 0x1F));
                    if (alpha == 0) r = g2 = b = 0;
                }
                data[((size_t)y * width + x) * 4 + 0] = r;
                data[((size_t)y * width + x) * 4 + 1] = g2;
                data[((size_t)y * width + x) * 4 + 2] = b;
                data[((size_t)y * width + x) * 4 + 3] = alpha;
            }
        }
    }
    glGenTextures(1, &e->tex);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, width, height, 0,
                 GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, data);
    free(data);
    e->texparam = texparam;
    e->texpal = texpal;
    e->w = width;
    e->h = height;
    e->in_use = 1;
    g->stats.textures_decoded++;
    return e;
}

/* ------------------------------------------------------------------ */
/* polygon draw                                                        */

/* pack recorded span params into the 3x192 RGBA32I texture layout:
 * texel (0,y) = (a_lo, a_hi, b_lo, b_hi), (1,y) = (c_lo, c_hi, l_cov0,
 * l_incr), (2,y) = (r_cov0, r_incr, l_mode, r_mode) */
static void pack_span_rows(const GxSpanRow *rows, int *texels /* FB_H*12 */)
{
    int y;
    for (y = 0; y < FB_H; y++) {
        const GxSpanRow *r = &rows[y];
        int *t = &texels[y * 12];
        t[0] = r->a_lo; t[1] = r->a_hi; t[2] = r->b_lo; t[3] = r->b_hi;
        t[4] = r->c_lo; t[5] = r->c_hi; t[6] = r->l_cov0; t[7] = r->l_incr;
        t[8] = r->r_cov0; t[9] = r->r_incr; t[10] = r->l_mode; t[11] = r->r_mode;
    }
}

void gx_gl3d_draw_polygon(GxGl3d *g, const GxPolygon *poly)
{
    float verts[16 * VTX_FLOATS];
    GxSpanRow spanrows[FB_H];
    int spantex[FB_H * 12];
    int ytop = 1 << 30, ybot = -(1 << 30);
    int i, nv, flat;
    GLuint prog = g->prog_poly;

    for (i = 0; i < poly->nv; i++) {
        if (poly->v[i].sy < ytop) ytop = poly->v[i].sy;
        if (poly->v[i].sy > ybot) ybot = poly->v[i].sy;
    }
    ybot = ybot + 1;
    if (ytop < 0) ytop = 0;
    if (ybot > FB_H) ybot = FB_H;
    if (ybot < 0) ybot = 0;
    if (ytop > FB_H) ytop = FB_H;

    /* flat polygon (all vertices on one row): GL cannot rasterize a
     * zero-height fan; rasterize a 1px-tall quad spanning [xmin, xmax]
     * with the min-x/max-x vertices' attributes (software renders these
     * as a single scanline with a horizontal vertex walk). */
    flat = (ybot - ytop == 1);
    if (flat) {
        int imin = 0, imax = 0;
        float xmin = 1e9f, xmax = -1e9f;
        for (i = 0; i < poly->nv; i++) {
            float x = (float)poly->v[i].sx;
            if (x < xmin) { xmin = x; imin = i; }
            if (x > xmax) { xmax = x; imax = i; }
        }
        g->stats.flat_polys++;
        memset(verts, 0, sizeof(verts));
        for (i = 0; i < 4; i++) {
            const GxRasVertex *v = (i == 0 || i == 3) ? &poly->v[imin]
                                                      : &poly->v[imax];
            float x = (i == 0 || i == 3) ? xmin : xmax;
            float y = (i < 2) ? (float)poly->v[0].sy : (float)poly->v[0].sy + 1.0f;
            float *o = &verts[i * VTX_FLOATS];
            o[0] = x; o[1] = y;
            o[2] = (float)v->depth;
            o[3] = (float)v->cr; o[4] = (float)v->cg; o[5] = (float)v->cb;
            o[6] = (float)v->s; o[7] = (float)v->t;
            o[8] = (float)v->wnorm;
        }
        nv = 4;
    } else {
        /* degenerate = the whole ring is collinear (zero shoelace area);
         * the software rasterizer would still fill 1px spans for these */
        int64_t area2 = 0;
        for (i = 0; i < poly->nv; i++) {
            int j = (i + 1) % poly->nv;
            area2 += (int64_t)poly->v[i].sx * poly->v[j].sy -
                     (int64_t)poly->v[j].sx * poly->v[i].sy;
        }
        if (area2 == 0) {
            g->stats.degenerate_polys++;
            return;
        }
        for (i = 0; i < poly->nv; i++) {
            const GxRasVertex *v = &poly->v[i];
            float *o = &verts[i * VTX_FLOATS];
            o[0] = (float)v->sx; o[1] = (float)v->sy;
            o[2] = (float)v->depth;
            o[3] = (float)v->cr; o[4] = (float)v->cg; o[5] = (float)v->cb;
            o[6] = (float)v->s; o[7] = (float)v->t;
            o[8] = (float)v->wnorm;
        }
        nv = poly->nv;
    }

    /* recorded fill parameters: exactly what the software rasterizer would
     * fill this row with (spans + edge flags + AA coverage model) */
    memset(spanrows, 0, sizeof(spanrows));
    gx_raster_span_params(poly, &g->env, spanrows, FB_H);
    pack_span_rows(spanrows, spantex);

    glUseProgram(prog);
    glBindImageTexture(0, g->img_col[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(1, g->img_z[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(2, g->img_attr[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(3, g->img_col[1], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(4, g->img_z[1], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glBindImageTexture(5, g->img_attr[1], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);

    glUniform1ui(glGetUniformLocation(prog, "u_attr"), poly->attr);
    glUniform1i(glGetUniformLocation(prog, "u_ytop"), ytop);
    glUniform1i(glGetUniformLocation(prog, "u_ybot"), ybot);
    glUniform1i(glGetUniformLocation(prog, "u_wbuffer"), poly->wbuffer != 0);
    glUniform1i(glGetUniformLocation(prog, "u_facing"), poly->facing_view != 0);
    glUniform1i(glGetUniformLocation(prog, "u_highlight"), g->env.highlight != 0);
    glUniform1i(glGetUniformLocation(prog, "u_alpha_blend"), g->env.alpha_blend != 0);
    glUniform1i(glGetUniformLocation(prog, "u_aa"), g->env.aa_enable != 0);
    glUniform1i(glGetUniformLocation(prog, "u_alpha_ref"), g->env.alpha_ref);
    glUniform1ui(glGetUniformLocation(prog, "u_texparam"), poly->texparam);

    {
        uint32_t toon32[32];
        for (i = 0; i < 32; i++)
            toon32[i] = g->env.toon ? g->env.toon[i] : 0;
        glUniform1uiv(glGetUniformLocation(prog, "u_toon"), 32, toon32);
    }

    {
        int textured = g->env.tex_enable && g->env.tex &&
                       ((poly->texparam >> 26) & 0x7) != 0;
        glUniform1i(glGetUniformLocation(prog, "u_textured"), textured);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
        if (textured) {
            TexCacheEntry *e = tex_for(g, poly);
            if (e) {
                glBindTexture(GL_TEXTURE_2D, e->tex);
                glUniform2i(glGetUniformLocation(prog, "u_texsize"), e->w, e->h);
            } else {
                glBindTexture(GL_TEXTURE_2D, 0);
                glUniform2i(glGetUniformLocation(prog, "u_texsize"), 1, 1);
            }
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
            glUniform2i(glGetUniformLocation(prog, "u_texsize"), 1, 1);
        }
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g->span_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 3, FB_H, GL_RGBA_INTEGER, GL_INT,
                    spantex);
    glUniform1i(glGetUniformLocation(prog, "u_span"), 1);

    glBindVertexArray(g->vao);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, nv * VTX_FLOATS * sizeof(float), verts);
    glDrawArrays(GL_TRIANGLE_FAN, 0, nv);
    glBindVertexArray(0);

    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    g->stats.polys_drawn++;
}

/* ------------------------------------------------------------------ */
/* post pass                                                           */

static void draw_fullscreen(GxGl3d *g)
{
    glBindVertexArray(g->vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void gx_gl3d_post_pass(GxGl3d *g)
{
    GLuint prog;

    if (g->post.edge_enable && g->post.edge) {
        uint32_t edge32[8];
        int i;
        prog = g->prog_edge;
        glUseProgram(prog);
        glBindImageTexture(0, g->img_col[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(1, g->img_z[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(2, g->img_attr[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        for (i = 0; i < 8; i++)
            edge32[i] = g->post.edge[i];
        glUniform1uiv(glGetUniformLocation(prog, "u_edge"), 8, edge32);
        glUniform1ui(glGetUniformLocation(prog, "u_clear_attr"), g->post.clear_attr);
        glUniform1ui(glGetUniformLocation(prog, "u_clear_depth"), g->post.clear_depth);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        draw_fullscreen(g);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    if (g->post.fog_enable) {
        uint32_t d32[34];
        uint32_t fogR = (g->post.fog_color << 1) & 0x3E; if (fogR) fogR++;
        uint32_t fogG = (g->post.fog_color >> 4) & 0x3E; if (fogG) fogG++;
        uint32_t fogB = (g->post.fog_color >> 9) & 0x3E; if (fogB) fogB++;
        uint32_t fogA = (g->post.fog_color >> 16) & 0x1F;
        int i;
        prog = g->prog_fog;
        glUseProgram(prog);
        glBindImageTexture(0, g->img_col[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(1, g->img_z[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(2, g->img_attr[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(3, g->img_col[1], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(4, g->img_z[1], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(5, g->img_attr[1], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        for (i = 0; i < 34; i++)
            d32[i] = g->post.fog_density[i];
        glUniform1uiv(glGetUniformLocation(prog, "u_fog_density"), 34, d32);
        glUniform1ui(glGetUniformLocation(prog, "u_fog_offset"), g->post.fog_offset);
        glUniform1i(glGetUniformLocation(prog, "u_fog_shift"), g->post.fog_shift);
        glUniform1ui(glGetUniformLocation(prog, "u_fogR"), fogR);
        glUniform1ui(glGetUniformLocation(prog, "u_fogG"), fogG);
        glUniform1ui(glGetUniformLocation(prog, "u_fogB"), fogB);
        glUniform1ui(glGetUniformLocation(prog, "u_fogA"), fogA);
        glUniform1i(glGetUniformLocation(prog, "u_fogcolor"), !g->post.fog_alpha_only);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        draw_fullscreen(g);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }

    if (g->post.aa_enable) {
        prog = g->prog_aa;
        glUseProgram(prog);
        glBindImageTexture(0, g->img_col[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(1, g->img_attr[0], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glBindImageTexture(2, g->img_col[1], 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
        draw_fullscreen(g);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    }
}

void gx_gl3d_read_color(GxGl3d *g, uint32_t *out)
{
    glMemoryBarrier(GL_ALL_BARRIER_BITS);
    glFinish();
    glBindTexture(GL_TEXTURE_2D, g->img_col[0]);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, out);
}

void gx_gl3d_present(GxGl3d *g)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g->win_w, g->win_h);
    glUseProgram(g->prog_present);
    glBindImageTexture(0, g->img_col[0], 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    draw_fullscreen(g);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    SDL_GL_SwapWindow(g->window);
    glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
    glViewport(0, 0, FB_W, FB_H);
}
