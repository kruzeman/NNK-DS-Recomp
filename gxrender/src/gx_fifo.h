/* gx_fifo.h — DS GX command FIFO decoder + geometry vertex pipeline.
 *
 * The FIFO accepts raw 32-bit GXFIFO words exactly as the hardware does:
 * each word packs up to 4 command bytes (bits 0-7, 8-15, 16-23, 24-31);
 * parameter words follow as full 32-bit values.  The state machine mirrors
 * melonDS GPU3D::WriteToGXFIFO; the command semantics mirror
 * GPU3D::ExecuteCommand / SubmitVertex / SubmitPolygon / CalculateLighting.
 *
 * Pipeline: FIFO decode -> matrix stack -> clip transform (proj*pos) ->
 * viewport map -> cull -> GxPolygon -> software rasterizer and/or draw_fn
 * (GL backend skeleton).
 */
#ifndef GX_FIFO_H
#define GX_FIFO_H

#include <stdint.h>
#include "gx_types.h"
#include "gx_matrix.h"
#include "gx_raster.h"

/* Called for each polygon that survives culling. */
typedef void (*GxDrawFn)(void *user, const GxPolygon *poly);

typedef struct {
    int32_t clip[4];  /* clip-space position (1.19.12) */
    int16_t s, t;     /* texcoords, s16 (12.4) */
    uint32_t col20[3]; /* vertex color, 12.20 scale ((c5<<12)+0xFFF),
                        * melonDS GPU3D.cpp:1572-1574 */
} GxClipVertex;

typedef struct Gx {
    /* GX FIFO decode state */
    uint32_t cur_command;
    int num_commands, param_count, total_params;
    uint32_t exec_params[32];
    int exec_param_count;

    /* matrix engine */
    int matrix_mode;
    int32_t proj[16], pos[16], vec[16], texm[16];
    int32_t proj_stack[16], pos_stack[32][16], vec_stack[32][16], tex_stack[16];
    uint32_t proj_sp, pos_sp, tex_sp;
    int32_t clip_mat[16];
    int clip_dirty;

    /* attribute state */
    uint8_t vtx_color[3];
    int16_t normal[3];
    int16_t raw_s, raw_t, cur_s, cur_t; /* s16 like melonDS TexCoords */
    int16_t cur_vtx[3];
    uint32_t polygon_attr;   /* live POLYGON_ATTR register */
    uint32_t cur_polygon_attr; /* latched at BEGIN_VTXS */
    uint32_t tex_param, tex_palette;
    uint32_t zero_dot_w;     /* 0x04000610 ZeroDotWLimit (default 0xFFFFFF) */
    int32_t vp_x0, vp_y0, vp_w, vp_h; /* viewport in px (y0 = top row) */

    /* lighting */
    uint8_t mat_diff[3], mat_amb[3], mat_spec[3], mat_emi[3];
    int use_shininess;
    uint8_t shininess[128];
    int32_t light_dir[4][3];
    uint8_t light_color[4][3];
    int32_t spec_recip[4];

    /* polygon assembly */
    int polygon_mode, vtx_in_poly, vtx_count, num_consecutive;
    GxClipVertex temp_vtx[4];

    /* output */
    GxSurface surface;
    GxRasterEnv env;
    GxTexMem texmem;
    uint16_t toon[32];
    uint32_t disp_cnt;
    int wbuffer; /* W-buffering: SWAP_BUFFERS param bit 1, latched at the
                  * command and applied to polygons submitted after it
                  * (melonDS GPU3D.cpp:2284, 1514) */
    GxDrawFn draw_fn;
    void *draw_user;
    int skip_software; /* 1: only draw_fn renders (GL backend path) */

    /* stats */
    uint32_t n_verts, n_polys, n_polys_drawn, n_unknown_cmds;
    uint32_t n_culled_front, n_culled_back, n_drop_clip, n_drop_degenerate;
} Gx;

void gx_init(Gx *gx, uint32_t *color, uint32_t *depth, int w, int h);
void gx_set_texture_mem(Gx *gx, const uint8_t *vram, uint32_t vram_size,
                        const uint16_t *pal, uint32_t pal_count);
/* DISP3DCNT: bit0 texture enable, bit1 highlight, bit3 alpha blend */
void gx_set_disp_cnt(Gx *gx, uint32_t disp_cnt);

void gx_fifo_write(Gx *gx, uint32_t word);
void gx_fifo_write_block(Gx *gx, const uint32_t *words, size_t n);

/* Execute one command entry (command byte + one parameter) directly, as the
 * hardware direct command ports (4000440h-40005CCh) do; the parameter
 * accumulation for multi-param commands is shared with gx_fifo_write. */
void gx_cmd_entry(Gx *gx, uint8_t cmd, uint32_t param);

#endif /* GX_FIFO_H */
