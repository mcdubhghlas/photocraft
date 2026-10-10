/* Gradient tool and Paint Bucket (port of crates/algo/src/paint.rs); see algo_paint.h. */
#include "algo/algo_paint.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "color/color.h"
#include "raster/pixel.h"

static const char *const SHAPE_NAMES[5] = {"linear", "radial", "angle", "reflected", "diamond"};

const char *pc_algo_gradient_shape_name(pc_algo_gradient_shape s)
{
    return (unsigned)s < 5u ? SHAPE_NAMES[s] : NULL;
}

bool pc_algo_gradient_shape_from_name(const char *s, size_t len, pc_algo_gradient_shape *out)
{
    size_t i;
    for (i = 0; s != NULL && i < 5; i++) {
        if (strlen(SHAPE_NAMES[i]) == len && memcmp(SHAPE_NAMES[i], s, len) == 0) {
            *out = (pc_algo_gradient_shape)i;
            return true;
        }
    }
    return false;
}

float pc_algo_paint_gradient_t(pc_algo_gradient_shape shape, const float from[2], const float to[2], float x,
                               float y)
{
    float dx = to[0] - from[0], dy = to[1] - from[1];
    float len = pc_rs_maxf_k(sqrtf(dx * dx + dy * dy), 1e-6f);
    float ux = dx / len, uy = dy / len, px = x - from[0], py = y - from[1];
    float along = px * ux + py * uy, across = -px * uy + py * ux, t;
    switch (shape) {
    case PC_ALGO_GRADIENT_RADIAL:
        t = sqrtf(px * px + py * py) / len;
        break;
    case PC_ALGO_GRADIENT_REFLECTED:
        t = fabsf(along) / len;
        break;
    case PC_ALGO_GRADIENT_DIAMOND:
        t = (fabsf(along) + fabsf(across)) / len;
        break;
    case PC_ALGO_GRADIENT_ANGLE:
        t = pc_algo_rem_euclidf(atan2f(across, along) / 6.28318530717958647692f, 1.0f);
        break;
    case PC_ALGO_GRADIENT_LINEAR:
    default:
        t = along / len;
        break;
    }
    return pc_clampf(t, 0.0f, 1.0f);
}

void pc_algo_paint_sample_stops(const pc_algo_gradient_stop *stops, size_t n, float t, float out[4])
{
    size_t i, c;
    if (n == 0 || stops == NULL) {
        memset(out, 0, 4 * sizeof(float));
        return;
    }
    if (n == 1 || t <= stops[0].pos) {
        memcpy(out, stops[0].color, 4 * sizeof(float));
        return;
    }
    for (i = 0; i + 1 < n; i++) {
        const pc_algo_gradient_stop *a = &stops[i], *b = &stops[i + 1];
        if (t <= b->pos) {
            float k = b->pos > a->pos ? (t - a->pos) / (b->pos - a->pos) : 0.0f;
            for (c = 0; c < 4; c++) {
                out[c] = a->color[c] + (b->color[c] - a->color[c]) * k;
            }
            return;
        }
    }
    memcpy(out, stops[n - 1].color, 4 * sizeof(float));
}

/* ---- tiled compositing ------------------------------------------------------------------------- */

typedef enum cover_kind { COVER_GRADIENT, COVER_REGION } cover_kind;

typedef struct comp_tile {
    pc_rect r;
    float *region; /* NULL: no pixel covered */
    size_t len;
    pc_error err;
} comp_tile;

typedef struct comp_job {
    const pc_surface *src;
    pc_pixel_format fmt;
    size_t n;
    pc_blend_mode mode;
    bool lock_transparency;
    float opacity;
    const pc_surface *selection;
    cover_kind kind;
    /* gradient */
    pc_algo_gradient_shape shape;
    float from[2], to[2];
    const pc_algo_gradient_stop *stops;
    size_t nstops;
    bool reverse, dither;
    /* region */
    const pc_algo_region *region;
    pc_algo_paint_src_fn src_fn;
    void *src_ctx;
    comp_tile *tiles;
} comp_job;

static float sel_at(const comp_job *j, int32_t x, int32_t y)
{
    return j->selection != NULL ? pc_surface_sample_channel(j->selection, x, y, 0) : 1.0f;
}

static float cover(const comp_job *j, int32_t x, int32_t y)
{
    if (j->kind == COVER_GRADIENT) {
        return j->opacity * sel_at(j, x, y);
    }
    return pc_algo_region_at(j->region, x, y) * j->opacity * sel_at(j, x, y);
}

static void color_at(const comp_job *j, int32_t x, int32_t y, float out[4])
{
    if (j->kind == COVER_GRADIENT) {
        float t = pc_algo_paint_gradient_t(j->shape, j->from, j->to, (float)x + 0.5f, (float)y + 0.5f);
        pc_algo_paint_sample_stops(j->stops, j->nstops, j->reverse ? 1.0f - t : t, out);
        if (j->dither) {
            /* One quantisation step of monochromatic noise breaks 8-bit banding without speckle. */
            float nz = (pc_dither_noise(x, y) - 0.5f) / 255.0f;
            int c;
            for (c = 0; c < 3; c++) {
                out[c] = pc_clampf(out[c] + nz, 0.0f, 1.0f);
            }
        }
        return;
    }
    j->src_fn(j->src_ctx, x, y, out);
}

static pc_status comp_tile_run(const comp_job *j, comp_tile *t)
{
    pc_rect r = t->r;
    size_t w = pc_rect_width(r), n = j->n;
    int32_t x, y;
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            float k = cover(j, x, y), d[4], c[4], o[4], enc[PC_RASTER_MAX_CHANNELS];
            size_t i, cnt;
            if (k <= 0.0f) {
                continue;
            }
            if (t->region == NULL) {
                pc_status st = pc_surface_read_region(j->src, r, &t->region, &t->len, &t->err);
                if (st != PC_OK) {
                    t->region = NULL;
                    return st;
                }
            }
            i = ((size_t)(y - r.y0) * w + (size_t)(x - r.x0)) * n;
            if (i + n > t->len) {
                continue;
            }
            pc_to_rgba(j->fmt, &t->region[i], n, d);
            color_at(j, x, y, c);
            pc_blend_composite(j->mode, d, c, k, o);
            if (j->lock_transparency) {
                o[3] = d[3];
            }
            memset(enc, 0, sizeof enc);
            cnt = pc_from_rgba(j->fmt, o, enc);
            memcpy(&t->region[i], enc, (cnt < n ? cnt : n) * sizeof(float));
        }
    }
    return PC_OK;
}

static pc_status comp_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const comp_job *j = ctx;
    size_t i;
    pc_status first = PC_OK;
    (void)chunk;
    for (i = begin; i < end; i++) {
        pc_status st = comp_tile_run(j, &j->tiles[i]);
        first = first == PC_OK ? st : first;
    }
    return first;
}

static pc_status composite_area(pc_surface *s, pc_rect area, comp_job *j, pc_error *err)
{
    pc_tile first, last;
    size_t ntiles = 0, i;
    int64_t tx, ty, nx, ny;
    pc_status st = PC_OK;
    if (!pc_rect_tile_range(area, &first, &last)) {
        return PC_OK;
    }
    nx = (int64_t)last.tx - first.tx + 1;
    ny = (int64_t)last.ty - first.ty + 1;
    j->src = s;
    j->fmt = pc_surface_format(s);
    j->n = pc_pixel_format_channels(j->fmt);
    j->tiles = pc_calloc_array((size_t)ny, (size_t)nx * sizeof *j->tiles);
    if (j->tiles == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (ty = first.ty; ty <= last.ty; ty++) {
        for (tx = first.tx; tx <= last.tx; tx++) {
            pc_rect r = pc_rect_intersect(pc_tile_rect((pc_tile){(int32_t)tx, (int32_t)ty}), area);
            if (!pc_rect_is_empty(r)) {
                j->tiles[ntiles++].r = r;
            }
        }
    }
    st = pc_algo_par_for(ntiles, 1, comp_run, j);
    if (st != PC_OK) {
        for (i = 0; i < ntiles; i++) {
            if (j->tiles[i].err.code != PC_OK) {
                if (err != NULL) {
                    *err = j->tiles[i].err;
                }
                break;
            }
        }
    }
    for (i = 0; i < ntiles && st == PC_OK; i++) {
        if (j->tiles[i].region != NULL) {
            st = pc_surface_write_region(s, j->tiles[i].r, j->tiles[i].region, j->tiles[i].len, err);
        }
    }
    for (i = 0; i < ntiles; i++) {
        free(j->tiles[i].region);
    }
    free(j->tiles);
    j->tiles = NULL;
    return st;
}

pc_status pc_algo_paint_gradient(pc_surface *s, pc_rect area, const float from[2], const float to[2],
                                 pc_algo_gradient_shape shape, const pc_algo_gradient_stop *stops,
                                 size_t nstops, bool reverse, float opacity, pc_blend_mode mode, bool dither,
                                 const pc_surface *selection, bool lock_transparency, pc_error *err)
{
    comp_job j;
    if (s == NULL || from == NULL || to == NULL || (nstops > 0 && stops == NULL)) {
        return pc_fail(err, PC_ERR_INVALID, "missing gradient argument");
    }
    memset(&j, 0, sizeof j);
    j.mode = mode, j.lock_transparency = lock_transparency, j.opacity = opacity, j.selection = selection;
    j.kind = COVER_GRADIENT, j.shape = shape, j.stops = stops, j.nstops = nstops;
    j.from[0] = from[0], j.from[1] = from[1], j.to[0] = to[0], j.to[1] = to[1];
    j.reverse = reverse, j.dither = dither;
    return composite_area(s, area, &j, err);
}

pc_status pc_algo_paint_bucket_fill_region_src(pc_surface *s, const pc_algo_region *region, float opacity,
                                               const pc_surface *selection, bool lock_transparency,
                                               pc_algo_paint_src_fn src, void *ctx, pc_error *err)
{
    comp_job j;
    if (s == NULL || region == NULL || src == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing bucket argument");
    }
    memset(&j, 0, sizeof j);
    j.mode = PC_BLEND_NORMAL, j.lock_transparency = lock_transparency, j.opacity = opacity;
    j.selection = selection, j.kind = COVER_REGION, j.region = region, j.src_fn = src, j.src_ctx = ctx;
    /* Composite only over the filled region's box. */
    return composite_area(s, region->bbox, &j, err);
}

pc_status pc_algo_paint_bucket_fill_src(pc_surface *s, pc_rect area, int32_t seed_x, int32_t seed_y,
                                        float tolerance, bool contiguous, bool anti_alias, float opacity,
                                        const pc_surface *selection, bool lock_transparency,
                                        pc_algo_paint_src_fn src, void *ctx, bool *filled, pc_error *err)
{
    uint8_t (*img)[4] = NULL;
    size_t len = 0;
    pc_algo_region region;
    pc_status st;
    if (s == NULL || src == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing bucket argument");
    }
    if (!pc_rect_contains(area, seed_x, seed_y)) {
        *filled = false;
        return PC_OK;
    }
    if ((st = pc_algo_selection_rgba8_image(s, area, &img, &len, err)) != PC_OK) {
        return st;
    }
    memset(&region, 0, sizeof region);
    st = pc_algo_selection_wand_region((const uint8_t (*)[4])img, len, area, seed_x, seed_y, tolerance,
                                       contiguous, anti_alias, &region, err);
    free(img);
    if (st != PC_OK) {
        return st;
    }
    if (pc_algo_region_is_none(&region)) {
        *filled = false;
        return PC_OK;
    }
    st = pc_algo_paint_bucket_fill_region_src(s, &region, opacity, selection, lock_transparency, src, ctx,
                                              err);
    pc_algo_region_fini(&region);
    if (st == PC_OK) {
        *filled = true;
    }
    return st;
}

static void solid(void *ctx, int32_t x, int32_t y, float out[4])
{
    (void)x, (void)y;
    memcpy(out, ctx, 4 * sizeof(float));
}

pc_status pc_algo_paint_bucket_fill(pc_surface *s, pc_rect area, int32_t seed_x, int32_t seed_y,
                                    float tolerance, bool contiguous, bool anti_alias, const float color[4],
                                    float opacity, const pc_surface *selection, bool lock_transparency,
                                    bool *filled, pc_error *err)
{
    float c[4];
    if (color == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing colour");
    }
    memcpy(c, color, sizeof c);
    return pc_algo_paint_bucket_fill_src(s, area, seed_x, seed_y, tolerance, contiguous, anti_alias, opacity,
                                         selection, lock_transparency, solid, c, filled, err);
}
