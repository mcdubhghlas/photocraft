/* Synthetic images shared by the L10 tests and test_analysis_parity.c: exact ports of the test
 * helpers in features.rs (texture, warp), panorama.rs (scene, sample, view) and scancrop.rs
 * (scan), so the C side reproduces the reference's inputs bit for bit. Header-only. */
#ifndef PC_TESTS_ALGO_ANALYSIS_GEN_H
#define PC_TESTS_ALGO_ANALYSIS_GEN_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/panorama.h"
#include "algo/photo_util.h"
#include "algo/scancrop.h"
#include "base/num.h"
#include "base/rmath.h"

/* features.rs tests::texture: blobs of varying brightness (caller frees). */
static inline float *gen_texture(size_t w, size_t h)
{
    pc_algo_photo_rng rng = {42};
    float *img = malloc(w * h * sizeof *img);
    size_t i, x, y;
    int k;
    for (i = 0; i < w * h; i++) {
        img[i] = 0.3f;
    }
    for (k = 0; k < 140; k++) {
        double cx = pc_algo_photo_rng_unit(&rng) * (double)w, cy = pc_algo_photo_rng_unit(&rng) * (double)h;
        double r = 3.0 + pc_algo_photo_rng_unit(&rng) * 9.0;
        float v = (float)pc_algo_photo_rng_unit(&rng);
        bool sq = pc_algo_photo_rng_unit(&rng) > 0.5;
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                double dx = (double)x - cx, dy = (double)y - cy;
                bool inside = sq ? fabs(dx) < r && fabs(dy) < r * 0.6 : hypot(dx, dy) < r;
                if (inside) {
                    img[y * w + x] = v;
                }
            }
        }
    }
    return img;
}

/* features.rs tests::warp: destination -> source lookup through inv, bilinear, background 0.3. */
static inline float *gen_warp(size_t w, size_t hgt, const float *img, const pc_algo_homography *inv)
{
    float *out = malloc(w * hgt * sizeof *out);
    size_t x, y;
    for (x = 0; x < w * hgt; x++) {
        out[x] = 0.3f;
    }
    for (y = 0; y < hgt; y++) {
        for (x = 0; x < w; x++) {
            double sx, sy;
            size_t x0, y0;
            float tx, ty, top, bot;
            pc_algo_homography_apply(inv, (double)x, (double)y, &sx, &sy);
            if (sx < 0.0 || sy < 0.0 || sx >= (double)(w - 1) || sy >= (double)(hgt - 1)) {
                continue;
            }
            x0 = pc_f64_to_size(floor(sx)), y0 = pc_f64_to_size(floor(sy));
            tx = (float)(sx - (double)x0), ty = (float)(sy - (double)y0);
#define P(xx, yy) img[(yy) * w + (xx)]
            top = P(x0, y0) + (P(x0 + 1, y0) - P(x0, y0)) * tx;
            bot = P(x0, y0 + 1) + (P(x0 + 1, y0 + 1) - P(x0, y0 + 1)) * tx;
#undef P
            out[y * w + x] = top + (bot - top) * ty;
        }
    }
    return out;
}

/* panorama.rs tests::scene: blobs over a gradient. */
static inline float *gen_scene(size_t w, size_t h, uint64_t seed)
{
    pc_algo_photo_rng rng = {seed};
    float *img = malloc(w * h * sizeof *img);
    size_t i, x, y, n = w * h / 900 > 40 ? w * h / 900 : 40;
    for (i = 0; i < w * h; i++) {
        img[i] = 0.25f + 0.2f * ((float)(i % w) / (float)w);
    }
    for (i = 0; i < n; i++) {
        double cx = (double)(pc_algo_photo_rng_next(&rng) % w),
               cy = (double)(pc_algo_photo_rng_next(&rng) % h);
        double r = 3.0 + (double)(pc_algo_photo_rng_next(&rng) % 9);
        float v = (float)(pc_algo_photo_rng_next(&rng) % 1000) / 1000.0f;
        bool sq = pc_algo_photo_rng_next(&rng) % 2 == 0;
        size_t x0 = pc_f64_to_size(pc_rs_max_k(cx - r, 0.0)), x1 = pc_f64_to_size(cx + r) + 1;
        size_t y0 = pc_f64_to_size(pc_rs_max_k(cy - r, 0.0)), y1 = pc_f64_to_size(cy + r) + 1;
        x1 = x1 < w ? x1 : w, y1 = y1 < h ? y1 : h;
        for (y = y0; y < y1; y++) {
            for (x = x0; x < x1; x++) {
                double dx = (double)x - cx, dy = (double)y - cy;
                if (sq ? fabs(dx) < r && fabs(dy) < r * 0.6 : hypot(dx, dy) < r) {
                    img[y * w + x] = v;
                }
            }
        }
    }
    return img;
}

typedef void (*gen_map_fn)(const void *ctx, double x, double y, double *sx, double *sy);

/* panorama.rs tests::view: tw x th crop of a scene through map (view px -> scene px). */
static inline float *gen_view(const float *scene, size_t sw, size_t sh, size_t tw, size_t th, gen_map_fn map,
                              const void *ctx)
{
    float *out = malloc(tw * th * sizeof *out);
    size_t i;
    for (i = 0; i < tw * th; i++) {
        double x, y;
        map(ctx, (double)(i % tw), (double)(i / tw), &x, &y);
        out[i] = (x < 0.0 || y < 0.0 || x > (double)(sw - 1) || y > (double)(sh - 1))
                     ? 0.0f
                     : pc_algo_photo_bilinear(scene, sw * sh, sw, sh, 1, 0, (float)x, (float)y);
    }
    return out;
}

static inline void gen_map_offset(const void *ctx, double x, double y, double *sx, double *sy)
{
    const double *o = ctx;
    *sx = x + o[0];
    *sy = y + o[1];
}

static inline void gen_map_homography(const void *ctx, double x, double y, double *sx, double *sy)
{
    pc_algo_homography_apply(ctx, x, y, sx, sy);
}

/* Rotating camera on a cylinder-mapped scene: ctx = {f, tw, th, sw, sh, pan degrees}. */
static inline void gen_map_pan(const void *ctx, double x, double y, double *sx, double *sy)
{
    const double *c = ctx;
    double f = c[0], dx = x - c[1] / 2.0, dy = y - c[2] / 2.0;
    double th_ = atan2(dx, f) + c[5] * (3.141592653589793 / 180.0);
    double hh = dy / hypot(dx, f);
    *sx = c[3] / 2.0 + f * th_;
    *sy = c[4] / 2.0 + f * hh;
}

/* scancrop.rs tests::scan: white background with rotated textured rectangles. */
static inline float (*gen_scan(size_t w, size_t h, const pc_algo_found_photo *photos, size_t n))[4]
{
    float (*px)[4] = malloc(w * h * sizeof *px);
    size_t i, x, y, k;
    for (i = 0; i < w * h; i++) {
        px[i][0] = 0.97f, px[i][1] = 0.97f, px[i][2] = 0.96f, px[i][3] = 1.0f;
    }
    for (k = 0; k < n; k++) {
        const pc_algo_found_photo *p = &photos[k];
        double a = p->angle * (3.141592653589793 / 180.0), s = sin(a), c = cos(a);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                double dx = (double)x + 0.5 - p->center[0], dy = (double)y + 0.5 - p->center[1];
                double u = c * dx + s * dy, v = -s * dx + c * dy;
                if (fabs(u) < p->width / 2.0 && fabs(v) < p->height / 2.0) {
                    float t = (float)(sin(u * 0.05) * cos(v * 0.07));
                    px[y * w + x][0] = 0.3f + 0.2f * t;
                    px[y * w + x][1] = 0.2f + 0.1f * (float)k;
                    px[y * w + x][2] = 0.5f - 0.2f * t;
                    px[y * w + x][3] = 1.0f;
                }
            }
        }
    }
    return px;
}

#endif
