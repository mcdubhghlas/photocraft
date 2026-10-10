/* Internal: the Filter Gallery's pixel work (port of crates/algo/src/artistic_fx.rs), shared by
 * artistic.c, artistic_fx.c and artistic_fx2.c. Not part of the public API.
 *
 * Every effect runs on an sRGB working copy of the tile's input window (straight colour; the
 * source alpha is kept) and may read up to its reach around each output pixel, so a stack reads
 * the sum of the reaches. Noise, strokes and textures are functions of document coordinates, so
 * results do not depend on the tiling. */
#ifndef PC_ALGO_ARTISTIC_FX_H
#define PC_ALGO_ARTISTIC_FX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/artistic.h"
#include "algo/filter.h"
#include "algo/image.h"
#include "base/status.h"

/* An sRGB colour of the working window. */
typedef struct gpx {
    float c[3];
} gpx;

/* The working window: sRGB colour of every pixel of r. */
typedef struct gwin {
    pc_rect r;
    size_t w, h;
    gpx *c;
} gwin;

int32_t pc_algo_gallery_effect_reach(const pc_algo_gallery_effect *e);
/* Runs a stack of effects over out (src covers out grown by the reach). */
pc_status pc_algo_gallery_run(const pc_algo_gallery_effects *effects, const pc_algo_image *src, pc_rect out,
                              const pc_algo_ctx *ctx, float *dst, pc_error *err);

/* ---- helpers (artistic_fx.c) ---- */

void pc_gfx_xy(const gwin *w, size_t i, int32_t *x, int32_t *y);
gpx pc_gfx_sample(const gwin *w, float x, float y);
gpx pc_gfx_at(const gwin *w, int32_t x, int32_t y);
float pc_gfx_smoothstep(float e0, float e1, float x);
float pc_gfx_lum(gpx p);
gpx pc_gfx_mix(gpx a, gpx b, float t);
gpx pc_gfx_grey(float v);
gpx pc_gfx_rgb(const float c[4]);
float pc_gfx_clamp01(float v);
float pc_gfx_posterize(float v, float levels);
gpx pc_gfx_saturate(gpx p, float k);
int32_t pc_gfx_gs(float sigma);
pc_status pc_gfx_blur1(float *p, size_t w, size_t h, float sigma, pc_error *err);
pc_status pc_gfx_blur3(gpx *c, size_t w, size_t h, float sigma, pc_error *err);
/* Sobel gradients (/8, edges clamped); gx and gy hold w * h. */
void pc_gfx_sobel(const float *p, size_t w, size_t h, float *gx, float *gy);
/* Gradient magnitude, newly allocated (free()); NULL on out-of-memory. */
float *pc_gfx_grad_mag(const float *p, size_t w, size_t h);
/* Luminance plane of the window, newly allocated; NULL on out-of-memory. */
float *pc_gfx_win_lum(const gwin *w);
pc_status pc_gfx_line_blur(float *buf, size_t n, size_t w, size_t h, int32_t dx, int32_t dy, size_t r,
                           pc_error *err);
pc_status pc_gfx_line_blur3(gpx *c, size_t w, size_t h, int32_t dx, int32_t dy, size_t r, pc_error *err);
void pc_gfx_direction(const char *name, int32_t *dx, int32_t *dy);
pc_status pc_gfx_kuwahara(gwin *w, size_t r, pc_error *err);
pc_status pc_gfx_unsharp(gwin *w, float sigma, float amount, pc_error *err);
float pc_gfx_vnoise(float x, float y, uint32_t seed);
float pc_gfx_fbm(float x, float y, uint32_t seed);
float pc_gfx_strokes(int32_t x, int32_t y, int32_t dx, int32_t dy, float len, float wid, uint32_t seed);
gpx pc_gfx_paint_stroke(const gwin *src, int32_t x, int32_t y, int32_t dx, int32_t dy, float len, float wid,
                        uint32_t seed, float *t);
void pc_gfx_light_dir(const char *name, float *lx, float *ly);
float pc_gfx_shade(float gx, float gy, float lx, float ly, float k);
float pc_gfx_texture(const char *kind, float x, float y, float s);
/* Texturizer core; mode 0: weight 1, 1: Rough Pastels' weight (thick chalk in bright areas). */
void pc_gfx_texturize(gwin *w, const char *kind, float scaling, float relief, const char *light, bool invert,
                      int mode);
void pc_gfx_voronoi(float x, float y, float cell, uint32_t seed, float *d1, float *d2, float *px, float *py);
gpx pc_gfx_highlights(gpx p, float l, float area, float intensity);
void pc_gfx_normalize3(const float v[3], float out[3]);
/* A copy of the window's colours, newly allocated; NULL on out-of-memory. */
gpx *pc_gfx_clone(const gwin *w);

/* Sizes shared by reaches and effects. */
float pc_gfx_basrelief_sigma(const pc_algo_gallery_effect *e);
float pc_gfx_patch_size(const pc_algo_gallery_effect *e);
float pc_gfx_glass_cell(const pc_algo_gallery_effect *e);

/* The effects of the Artistic and Brush Strokes folders (artistic_fx.c) and of the others
 * (artistic_fx2.c); *handled is false when f belongs to the other file. */
pc_status pc_gfx_apply1(const pc_algo_gallery_effect *e, gwin *w, const pc_algo_ctx *ctx, bool *handled,
                        pc_error *err);
pc_status pc_gfx_apply2(const pc_algo_gallery_effect *e, gwin *w, const pc_algo_ctx *ctx, pc_error *err);

#endif
