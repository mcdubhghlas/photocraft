/* Background Eraser (bg_erase.h). */
#include "paint/bg_erase.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "paint/pmath.h"
#include "paint/retouch.h"
#include "raster/pixel.h"

#define MAXC PC_RASTER_MAX_CHANNELS

void pc_paint_bg_erase_settings_default(pc_paint_bg_erase_settings *s)
{
    static const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f}, white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    memset(s, 0, sizeof *s);
    s->sampling = PC_PAINT_SAMPLING_CONTINUOUS;
    s->limits = PC_PAINT_LIMITS_CONTIGUOUS;
    s->tolerance = 0.5f;
    s->protect_foreground = false;
    memcpy(s->foreground, black, sizeof s->foreground);
    memcpy(s->background, white, sizeof s->background);
}

bool pc_paint_bg_erase_settings_eq(const pc_paint_bg_erase_settings *a, const pc_paint_bg_erase_settings *b)
{
    if (a->sampling != b->sampling || a->limits != b->limits || !(a->tolerance == b->tolerance) ||
        a->protect_foreground != b->protect_foreground) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        if (!(a->foreground[i] == b->foreground[i]) || !(a->background[i] == b->background[i])) {
            return false;
        }
    }
    return true;
}

typedef struct bg_ctx {
    const pc_paint_bg_erase_settings *bs;
    size_t a, nc, n;
    float tol;
    bool soft;
    float swatch[MAXC], fg[MAXC];
    /* Sampling Once: has_once = sampled already; once_ok = it found a colour. */
    bool has_once, once_ok;
    float once[MAXC];
} bg_ctx;

/* Largest per-channel difference over the colour channels. */
static float dist(const float *a, const float *b, size_t n)
{
    float m = 0.0f;
    for (size_t i = 0; i < n; i++) {
        m = pc_rs_maxf(m, fabsf(a[i] - b[i]));
    }
    return m;
}

static float match_w(const bg_ctx *c, float d)
{
    if (d > c->tol) {
        return 0.0f;
    }
    if (c->soft && c->tol > 0.0f) {
        /* Soft falloff over the last quarter of the tolerance. */
        return pc_clampf((c->tol - d) / (c->tol * 0.25f), 0.0f, 1.0f);
    }
    return 1.0f;
}

/* The hotspot's colour: false where it is already transparent or off the working area. */
static bool hotspot(const bg_ctx *c, const pc_paint_region *work, int32_t cx, int32_t cy, float out[MAXC])
{
    const float *p = pc_paint_region_cpx(work, cx, cy);
    if (p == NULL || !(p[c->a] > 0.0f)) {
        return false;
    }
    memcpy(out, p, c->nc * sizeof *out);
    return true;
}

static pc_status bg_dab(void *vctx, pc_paint_region *work, const pc_paint_footprint *fp, pc_error *err)
{
    bg_ctx *c = vctx;
    pc_rect r = pc_rect_intersect(fp->rect, work->rect);
    int32_t cx, cy, sx, sy;
    float sampled[MAXC];
    bool ok;
    size_t w, h, a = c->a, nc = c->nc;
    float *weight;
    pc_status st = PC_OK;
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    cx = pc_f64_to_i32(floor(fp->dab.center.x));
    cy = pc_f64_to_i32(floor(fp->dab.center.y));
    switch (c->bs->sampling) {
    case PC_PAINT_SAMPLING_ONCE:
        if (!c->has_once) {
            c->once_ok = hotspot(c, work, cx, cy, c->once);
            c->has_once = true;
        }
        ok = c->once_ok;
        memcpy(sampled, c->once, sizeof sampled);
        break;
    case PC_PAINT_SAMPLING_BACKGROUND_SWATCH:
        ok = true;
        memcpy(sampled, c->swatch, sizeof sampled);
        break;
    default:
        ok = hotspot(c, work, cx, cy, sampled);
        break;
    }
    if (!ok) {
        return PC_OK;
    }
    w = pc_rect_width(r);
    h = pc_rect_height(r);
    /* r lies inside the footprint, which is capped at PC_PAINT_MAX_DAB_PIXELS. */
    weight = pc_calloc_array(w * h, sizeof *weight);
    if (weight == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the background eraser");
    }
    /* Per-pixel match weight. Erasing only lowers alpha, so the working copy still holds the
     * pre-stroke colours. */
    for (int32_t y = r.y0; y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            const float *p;
            if (pc_paint_footprint_at(fp, x, y) <= 0.0f || (p = pc_paint_region_cpx(work, x, y)) == NULL) {
                continue;
            }
            if (p[a] <= 0.0f) {
                continue;
            }
            if (c->bs->protect_foreground && dist(p, c->fg, nc) <= c->tol) {
                continue;
            }
            weight[(size_t)((int64_t)y - r.y0) * w + (size_t)((int64_t)x - r.x0)] =
                match_w(c, dist(p, sampled, nc));
        }
    }
    if (c->bs->limits != PC_PAINT_LIMITS_DISCONTIGUOUS) {
        sx = cx < r.x0 ? r.x0 : (cx > r.x1 - 1 ? r.x1 - 1 : cx);
        sy = cy < r.y0 ? r.y0 : (cy > r.y1 - 1 ? r.y1 - 1 : cy);
        st = pc_paint_keep_connected(weight, w, h, (size_t)((int64_t)sx - r.x0), (size_t)((int64_t)sy - r.y0),
                                     err);
    }
    for (int32_t y = r.y0; st == PC_OK && y < r.y1; y++) {
        for (int32_t x = r.x0; x < r.x1; x++) {
            float k = pc_paint_footprint_at(fp, x, y) *
                      weight[(size_t)((int64_t)y - r.y0) * w + (size_t)((int64_t)x - r.x0)];
            float *p;
            if (k <= 0.0f || (p = pc_paint_region_px(work, x, y)) == NULL) {
                continue;
            }
            p[a] *= 1.0f - pc_paint_minf_k(k, 1.0f);
            if (p[a] <= 0.0f) {
                memset(p, 0, c->n * sizeof *p);
            }
        }
    }
    free(weight);
    return st;
}

/* A straight RGB colour as an opaque native pixel's colour channels. */
static void native(pc_pixel_format fmt, const float c[4], float out[MAXC])
{
    float rgba[4] = {c[0], c[1], c[2], 1.0f};
    (void)pc_from_rgba(fmt, rgba, out);
}

pc_status pc_paint_apply_background_eraser(pc_surface *target, const pc_paint_stroke *stroke,
                                           const pc_paint_bg_erase_settings *bs, const pc_surface *selection,
                                           pc_rect *damage, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(target);
    pc_paint_stroke st = *stroke; /* shallow: only read, never finalised */
    bg_ctx c;
    memset(&c, 0, sizeof c);
    if (!pc_paint_alpha_index(fmt, &c.a)) {
        *damage = PC_RECT_EMPTY;
        return PC_OK;
    }
    c.bs = bs;
    c.nc = c.a;
    c.n = pc_pixel_format_channels(fmt);
    if (c.n > MAXC) {
        return pc_fail(err, PC_ERR_INVALID, "unsupported pixel format for the Background Eraser");
    }
    c.tol = isfinite(bs->tolerance) ? pc_clampf(bs->tolerance, 0.0f, 1.0f) : 0.0f;
    c.soft = bs->limits != PC_PAINT_LIMITS_FIND_EDGES;
    native(fmt, bs->background, c.swatch);
    native(fmt, bs->foreground, c.fg);
    st.brush.opacity = 1.0f;
    st.brush.erase = false;
    return pc_paint_apply_dab_stroke(target, &st, selection, false, 0, bg_dab, &c, damage, err);
}
