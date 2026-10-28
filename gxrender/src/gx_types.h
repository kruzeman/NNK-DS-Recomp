/* gx_types.h — shared fixed-point conventions for the DS GX render path.
 *
 * All render-pipeline math is integer-only so output is byte-deterministic.
 *
 *  - Matrix / clip-space values: s32, 1.19.12 fixed point (0x1000 == 1.0).
 *  - Screen coordinates: s32, 1/16 pixel (GX_SCREEN_ONE == 16).
 *  - Texture coordinates: s32, 12.4 fixed point (16 == 1.0 texel), as on DS.
 *  - Colors: 5-bit per channel in state, 6-bit per channel inside the
 *    rasterizer's packed pixel format (matches melonDS GPU3D_Soft layout:
 *    r | g<<8 | b<<16 | a<<24, each component 0..31/0..63).
 */
#ifndef GX_TYPES_H
#define GX_TYPES_H

#include <stdint.h>
#include <stddef.h>

#define GX_FX_ONE 0x1000          /* 1.0 in 1.19.12 */
#define GX_SCREEN_ONE 16          /* 1.0 pixel in screen fixed point */
#define GX_SCREEN_W 256
#define GX_SCREEN_H 192

/* Reciprocal-w scale used for perspective-correct attribute interpolation
 * (see gx_raster.c).  2^30 keeps full precision without int64 overflow. */
#define GX_INVW_SHIFT 30

static inline int32_t gx_min32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t gx_max32(int32_t a, int32_t b) { return a > b ? a : b; }
static inline int32_t gx_clamp32(int64_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return (int32_t)v;
}

/* 5-bit (0..31) -> 6-bit (0..63) DS component expansion:
 * value<<1, +1 when nonzero (melonDS GPU3D_Soft.cpp idiom). */
static inline uint8_t gx_c5_to_c6(uint8_t v)
{
    uint8_t r = (uint8_t)((v << 1) & 0x3E);
    if (r) r++;
    return r;
}

/* 6-bit -> 8-bit expansion used when writing PPM output. */
static inline uint8_t gx_c6_to_c8(uint8_t v)
{
    return (uint8_t)((v << 2) | (v >> 4));
}

#endif /* GX_TYPES_H */
