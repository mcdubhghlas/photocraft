/* Helpers shared by tests/paint/test_*.c (the brush_tests.rs fixtures). Header-only. */
#ifndef PC_TESTS_PAINT_SUPPORT_H
#define PC_TESTS_PAINT_SUPPORT_H

#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "paint/paint.h"
#include "raster/pixel.h"
#include "raster/surface.h"

/* brush_tests.rs `brush()`: pressure size off, 25 % spacing. */
static inline pc_paint_brush tb_brush(void)
{
    pc_paint_brush b;
    pc_paint_brush_default(&b);
    b.pressure_size = false;
    b.spacing = 0.25f;
    return b;
}

static inline void tb_line(double x0, double x1, double y, pc_paint_point out[2])
{
    out[0] = pc_paint_point_make(x0, y, 1.0f);
    out[1] = pc_paint_point_make(x1, y, 1.0f);
}

/* All primary dabs of a stroke (lib.rs `dabs`). Caller frees with pc_paint_dab_vec_fini. */
static inline pc_paint_dab_vec tb_dabs(const pc_paint_brush *b, const pc_paint_point *pts, size_t n)
{
    pc_paint_dab_vec v = {0};
    CHECK(pc_paint_dabs(b, pts, n, &v, NULL) == PC_OK);
    return v;
}

/* Primary and dual dabs (the generator, as the renderer drives it). */
static inline void tb_all_dabs(const pc_paint_brush *b, const pc_paint_point *pts, size_t n,
                               pc_paint_dab_vec *d, pc_paint_dab_vec *u)
{
    pc_paint_dab_generator g;
    memset(d, 0, sizeof *d);
    memset(u, 0, sizeof *u);
    pc_paint_dab_generator_init(&g, b, 1.0f);
    CHECK(pc_paint_dab_generator_push(&g, pts, n, d, u, NULL) == PC_OK);
    CHECK(pc_paint_dab_generator_finish(&g, d, u, NULL) == PC_OK);
    pc_paint_dab_generator_fini(&g);
}

/* A rasterised dab: rect + values. */
typedef struct tb_raster {
    pc_rect r;
    pc_paint_fbuf v;
} tb_raster;

static inline tb_raster tb_rast(const pc_paint_brush *b, const pc_paint_dab *d, bool dual)
{
    tb_raster out;
    pc_paint_context ctx;
    memset(&out, 0, sizeof out);
    CHECK(pc_paint_context_init(&ctx, b, NULL) == PC_OK);
    out.r = pc_paint_context_dab_rect(&ctx, d, dual);
    CHECK(pc_paint_context_rasterize(&ctx, d, dual, out.r, &out.v, NULL) == PC_OK);
    pc_paint_context_fini(&ctx);
    return out;
}

static inline float tb_at(const tb_raster *r, int32_t x, int32_t y)
{
    if (!pc_rect_contains(r->r, x, y)) {
        return 0.0f;
    }
    return r->v.v[(size_t)(y - r->r.y0) * pc_rect_width(r->r) + (size_t)(x - r->r.x0)];
}

/* brush_tests.rs `paint`: a stroke rendered onto a fresh RGBA32F surface. */
static inline void tb_paint(pc_surface *s, const pc_paint_brush *b, const pc_paint_point *pts, size_t n)
{
    pc_rect d;
    pc_surface_init(s, PC_PIXEL_FORMAT_RGBA32F);
    CHECK(pc_paint_render_stroke(s, b, pts, n, NULL, false, 1.0f, &d, NULL) == PC_OK);
}

static inline float tb_alpha(const pc_surface *s, int32_t x, int32_t y)
{
    float c[4];
    pc_surface_rgba(s, x, y, c);
    return c[3];
}

static inline float tb_alpha_sum(const pc_surface *s, pc_rect r)
{
    float t = 0.0f;
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            t += tb_alpha(s, x, y);
        }
    }
    return t;
}

static inline float tb_deg(float r)
{
    return r * 57.2957795130823208767981548141051703f;
}

/* Gray-tile builders for tests: the left half (x < split) painted, or a callback. */
static inline float tb_left_fn(void *ctx, uint32_t x, uint32_t y)
{
    (void)y;
    return x < *(const uint32_t *)ctx ? 1.0f : 0.0f;
}

static inline pc_paint_gray_tile tb_left_tile(uint32_t w, uint32_t h, uint32_t split)
{
    pc_paint_gray_tile t = PC_PAINT_GRAY_TILE_INIT;
    CHECK(pc_paint_gray_tile_from_fn(w, h, tb_left_fn, &split, &t, NULL) == PC_OK);
    return t;
}

#endif
