/* gx_gl.c — OpenGL backend skeleton (GL 1.1-style fixed pipeline on a
 * compatibility context, via SDL2).  Screen-space GxPolygon input; vertex
 * colors (5-bit) x decoded DS texture; depth via GL depth test with an
 * approximated normalization.  See gx_gl.h / GX_COVERAGE.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>

#include "gx_gl.h"
#include "gx_types.h"

#define GL_TEX_CACHE 8

typedef struct {
    uint32_t key;      /* texparam ^ (texpal << 1) */
    int in_use;
    GLuint tex;
    int width, height;
} GlTexCacheEntry;

struct GxGl {
    SDL_Window *window;
    SDL_GLContext ctx;
    int w, h;
    int vp_w, vp_h;    /* GL viewport backing the 256x192 target */
    int external;      /* window/context owned by the caller */
    GlTexCacheEntry cache[GL_TEX_CACHE];
    int cache_next;
};

static GxGl *gl_alloc(void)
{
    GxGl *g = (GxGl *)calloc(1, sizeof(GxGl));
    if (g) {
        g->w = GX_SCREEN_W;
        g->h = GX_SCREEN_H;
        g->vp_w = GX_SCREEN_W;
        g->vp_h = GX_SCREEN_H;
    }
    return g;
}

static void gl_setup_target(GxGl *g)
{
    glViewport(0, 0, g->vp_w, g->vp_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* screen coords: x right, y down (row 0 = top), depth z in [-1, 1] */
    glOrtho(0, GX_SCREEN_W, GX_SCREEN_H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_TEXTURE_2D);
}

GxGl *gx_gl_open(int scale)
{
    GxGl *g;
    if (scale < 1) scale = 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "gx_gl: SDL_Init: %s\n", SDL_GetError());
        return NULL;
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    g = gl_alloc();
    if (!g) return NULL;
    g->vp_w = GX_SCREEN_W * scale;
    g->vp_h = GX_SCREEN_H * scale;

    g->window = SDL_CreateWindow("gxrender",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 GX_SCREEN_W * scale, GX_SCREEN_H * scale,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g->window) {
        fprintf(stderr, "gx_gl: SDL_CreateWindow: %s\n", SDL_GetError());
        free(g);
        return NULL;
    }
    g->ctx = SDL_GL_CreateContext(g->window);
    if (!g->ctx) {
        fprintf(stderr, "gx_gl: SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(g->window);
        free(g);
        return NULL;
    }
    SDL_GL_MakeCurrent(g->window, g->ctx);
    gl_setup_target(g);
    return g;
}

GxGl *gx_gl_open_external(SDL_Window *window, SDL_GLContext ctx)
{
    GxGl *g;
    if (!window || !ctx) return NULL;
    g = gl_alloc();
    if (!g) return NULL;
    g->window = window;
    g->ctx = ctx;
    g->external = 1;
    SDL_GL_MakeCurrent(g->window, g->ctx);
    gl_setup_target(g);
    return g;
}

void gx_gl_close(GxGl *g)
{
    int i;
    if (!g) return;
    for (i = 0; i < GL_TEX_CACHE; i++)
        if (g->cache[i].in_use)
            glDeleteTextures(1, &g->cache[i].tex);
    if (!g->external) {
        if (g->ctx) SDL_GL_DeleteContext(g->ctx);
        if (g->window) SDL_DestroyWindow(g->window);
        SDL_Quit();
    }
    free(g);
}

const char *gx_gl_vendor(GxGl *g)
{
    (void)g;
    return (const char *)glGetString(GL_VENDOR);
}
const char *gx_gl_renderer(GxGl *g)
{
    (void)g;
    return (const char *)glGetString(GL_RENDERER);
}
const char *gx_gl_version(GxGl *g)
{
    (void)g;
    return (const char *)glGetString(GL_VERSION);
}

void gx_gl_begin_frame(GxGl *g, uint32_t clear_color6)
{
    gx_gl_begin_frame_a(g, clear_color6, 1.0f);
}

void gx_gl_begin_frame_a(GxGl *g, uint32_t clear_color6, float clear_alpha)
{
    float r = gx_c6_to_c8((uint8_t)(clear_color6 & 0x3F)) / 255.0f;
    float gg = gx_c6_to_c8((uint8_t)((clear_color6 >> 8) & 0x3F)) / 255.0f;
    float b = gx_c6_to_c8((uint8_t)((clear_color6 >> 16) & 0x3F)) / 255.0f;
    (void)g;
    gl_setup_target(g);
    glClearColor(r, gg, b, clear_alpha);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static GLuint gl_texture_for(GxGl *g, const GxPolygon *poly, const GxTexMem *tex)
{
    uint32_t key = poly->texparam ^ (poly->texpal << 1);
    GlTexCacheEntry *e = NULL;
    uint8_t *rgba;
    int i, width, height;

    for (i = 0; i < GL_TEX_CACHE; i++) {
        if (g->cache[i].in_use && g->cache[i].key == key) {
            glBindTexture(GL_TEXTURE_2D, g->cache[i].tex);
            return g->cache[i].tex;
        }
    }

    width = 8 << ((poly->texparam >> 20) & 0x7);
    height = 8 << ((poly->texparam >> 23) & 0x7);
    rgba = (uint8_t *)malloc((size_t)width * height * 4);
    if (!rgba)
        return 0;
    gx_tex_decode_rgba(tex, poly->texparam, poly->texpal, rgba);

    e = &g->cache[g->cache_next];
    g->cache_next = (g->cache_next + 1) % GL_TEX_CACHE;
    if (e->in_use)
        glDeleteTextures(1, &e->tex);
    glGenTextures(1, &e->tex);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba);
    e->key = key;
    e->in_use = 1;
    e->width = width;
    e->height = height;
    free(rgba);
    return e->tex;
}

void gx_gl_draw_polygon(GxGl *g, const GxPolygon *poly, const GxTexMem *tex)
{
    int i, textured;
    int w = 0, h = 0;
    float dscale;
    static const int tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    int ntri = (poly->nv == 4) ? 2 : 1;

    textured = tex && ((poly->texparam >> 26) & 0x7) != 0;
    if (textured) {
        gl_texture_for(g, poly, tex);
        w = 8 << ((poly->texparam >> 20) & 0x7);
        h = 8 << ((poly->texparam >> 23) & 0x7);
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* depth normalization (approximate): GxRasVertex.depth is the melonDS
     * FinalZ scale -- 24-bit Z (0..0xFFFFFF, smaller = nearer) in Z mode,
     * 16-bit-normalized W (smaller = nearer) in W mode.  Map to GL z in
     * [-1,1] with -1 = nearest. */
    dscale = ((poly->attr >> 14) & 1) ? 65535.0f : 16777215.0f;

    glBegin(GL_TRIANGLES);
    for (i = 0; i < ntri; i++) {
        int k;
        for (k = 0; k < 3; k++) {
            const GxRasVertex *v = &poly->v[tri[i][k]];
            /* FinalColor: 9-bit scale ((c<<4)+0xF, 0 stays 0), 0..511 */
            float r = v->cr / 511.0f, gg = v->cg / 511.0f, b = v->cb / 511.0f;
            float pa = ((poly->attr >> 16) & 0x1F) / 31.0f;
            float s = v->s / 16.0f, t = v->t / 16.0f;
            float z = 2.0f * ((float)v->depth / dscale) - 1.0f;
            if (z < -1.0f) z = -1.0f;
            if (z > 1.0f) z = 1.0f;
            if (textured) {
                s /= (float)w;
                t /= (float)h;
            }
            glTexCoord2f(s, t);
            glColor4f(r, gg, b, pa);
            glVertex3f((float)v->sx, (float)v->sy, z);
        }
    }
    glEnd();
}

void gx_gl_end_frame(GxGl *g)
{
    SDL_GL_SwapWindow(g->window);
    glFinish();
}

int gx_gl_read_rgba(GxGl *g, uint8_t *rgba)
{
    (void)g;
    glReadPixels(0, 0, GX_SCREEN_W, GX_SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    return 0;
}

int gx_gl_read_ppm(GxGl *g, const char *path)
{
    uint8_t *rgba, *row;
    FILE *f;
    int y;

    rgba = (uint8_t *)malloc((size_t)GX_SCREEN_W * GX_SCREEN_H * 4);
    if (!rgba)
        return -1;
    gx_gl_read_rgba(g, rgba);

    f = fopen(path, "wb");
    if (!f) {
        free(rgba);
        return -1;
    }
    fprintf(f, "P6\n%d %d\n255\n", GX_SCREEN_W, GX_SCREEN_H);
    row = (uint8_t *)malloc((size_t)GX_SCREEN_W * 3);
    for (y = GX_SCREEN_H - 1; y >= 0; y--) { /* GL reads bottom-up */
        int x;
        for (x = 0; x < GX_SCREEN_W; x++) {
            row[x * 3 + 0] = rgba[((size_t)y * GX_SCREEN_W + x) * 4 + 0];
            row[x * 3 + 1] = rgba[((size_t)y * GX_SCREEN_W + x) * 4 + 1];
            row[x * 3 + 2] = rgba[((size_t)y * GX_SCREEN_W + x) * 4 + 2];
        }
        fwrite(row, 1, GX_SCREEN_W * 3, f);
    }
    free(row);
    free(rgba);
    fclose(f);
    return 0;
}
