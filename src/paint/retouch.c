/* Retouch stroke machinery (retouch.h): regions, footprints, coverage compositing, sequential
 * dab strokes, the premultiplied mix-back and the Smudge tool. */
#include "paint/retouch.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "paint/dynamics.h"
#include "paint/pmath.h"
#include "raster/pixel.h"

/* ---- regions ---- */

static pc_status region_alloc(pc_rect rect, size_t ch, bool zero, pc_paint_region *out, pc_error *err)
{
    size_t n;
    float *d = NULL;
    if (pc_rect_is_empty(rect)) {
        rect = PC_RECT_EMPTY;
    }
    if (!pc_size_mul3(pc_rect_width(rect), pc_rect_height(rect), ch, &n) ||
        n > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "retouch region too large");
    }
    if (n > 0) {
        d = zero ? pc_calloc_array(n, sizeof *d) : pc_malloc_array(n, sizeof *d);
        if (d == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a retouch region");
        }
    }
    out->rect = rect;
    out->ch = ch;
    out->data = d;
    out->len = n;
    return PC_OK;
}

pc_status pc_paint_region_new(pc_rect rect, size_t ch, pc_paint_region *out, pc_error *err)
{
    return region_alloc(rect, ch, true, out, err);
}

pc_status pc_paint_region_read(const pc_surface *s, pc_rect rect, pc_paint_region *out, pc_error *err)
{
    pc_paint_region r;
    pc_status st = region_alloc(rect, pc_surface_channels(s), false, &r, err);
    if (st != PC_OK) {
        return st;
    }
    if (r.len > 0) {
        st = pc_surface_read_region_into(s, r.rect, r.data, r.len, err);
        if (st != PC_OK) {
            pc_paint_region_fini(&r);
            return st;
        }
    }
    *out = r;
    return PC_OK;
}

pc_status pc_paint_region_clone(const pc_paint_region *r, pc_paint_region *out, pc_error *err)
{
    pc_paint_region c;
    pc_status st = region_alloc(r->rect, r->ch, false, &c, err);
    if (st != PC_OK) {
        return st;
    }
    if (c.len != r->len) {
        pc_paint_region_fini(&c);
        return pc_fail(err, PC_ERR_INVALID, "retouch region length does not match its rect");
    }
    if (c.len > 0) {
        memcpy(c.data, r->data, c.len * sizeof *c.data);
    }
    *out = c;
    return PC_OK;
}

void pc_paint_region_fini(pc_paint_region *r)
{
    if (r == NULL) {
        return;
    }
    free(r->data);
    memset(r, 0, sizeof *r);
}

size_t pc_paint_region_width(const pc_paint_region *r)
{
    return pc_rect_width(r->rect);
}

size_t pc_paint_region_height(const pc_paint_region *r)
{
    return pc_rect_height(r->rect);
}

/* Float offset of pixel (x, y), or SIZE_MAX outside the region (or its data). */
static size_t region_off(const pc_paint_region *r, int32_t x, int32_t y)
{
    size_t i;
    if (r->data == NULL || !pc_rect_contains(r->rect, x, y) || r->ch == 0) {
        return SIZE_MAX;
    }
    i = ((size_t)((int64_t)y - r->rect.y0) * pc_rect_width(r->rect) + (size_t)((int64_t)x - r->rect.x0)) *
        r->ch;
    return i + r->ch <= r->len ? i : SIZE_MAX;
}

float *pc_paint_region_px(pc_paint_region *r, int32_t x, int32_t y)
{
    size_t i = region_off(r, x, y);
    return i == SIZE_MAX ? NULL : r->data + i;
}

const float *pc_paint_region_cpx(const pc_paint_region *r, int32_t x, int32_t y)
{
    size_t i = region_off(r, x, y);
    return i == SIZE_MAX ? NULL : r->data + i;
}

pc_status pc_paint_region_crop(const pc_paint_region *src, pc_rect r, pc_paint_region *out, pc_error *err)
{
    pc_paint_region o;
    size_t w, n;
    pc_status st;
    r = pc_rect_intersect(r, src->rect);
    st = region_alloc(r, src->ch, false, &o, err);
    if (st != PC_OK) {
        return st;
    }
    w = pc_rect_width(r);
    n = src->ch;
    for (int32_t y = r.y0; y < r.y1 && o.len > 0; y++) {
        size_t s = region_off(src, r.x0, y);
        size_t d = (size_t)((int64_t)y - r.y0) * w * n;
        if (s == SIZE_MAX || src->data == NULL || o.data == NULL || s + w * n > src->len) {
            pc_paint_region_fini(&o);
            return pc_fail(err, PC_ERR_INVALID, "retouch region length does not match its rect");
        }
        memcpy(o.data + d, src->data + s, w * n * sizeof *o.data);
    }
    *out = o;
    return PC_OK;
}

pc_status pc_paint_region_write(const pc_paint_region *r, pc_surface *s, pc_error *err)
{
    if (r->len == 0) {
        return PC_OK;
    }
    return pc_surface_write_region(s, r->rect, r->data, r->len, err);
}

/* ---- footprints and coverage ---- */

pc_rect pc_paint_dab_rect(const pc_paint_dab *d)
{
    return pc_paint_rect_around(d, d->radius);
}

pc_status pc_paint_footprint_round(const pc_paint_dab *d, size_t index, float hardness,
                                   pc_paint_footprint *out, pc_error *err)
{
    pc_rect rect = pc_paint_dab_rect(d);
    float cx = (float)d->center.x, cy = (float)d->center.y;
    size_t n = 0, k = 0;
    float *cov = NULL;
    if (!pc_rect_is_empty(rect)) {
        n = (size_t)pc_rect_width(rect) * pc_rect_height(rect);
    }
    if (n > PC_PAINT_MAX_DAB_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "brush dab too large (%zu pixels)", n);
    }
    if (n > 0) {
        cov = pc_malloc_array(n, sizeof *cov);
        if (cov == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a brush footprint");
        }
    }
    for (int32_t y = rect.y0; y < rect.y1 && n > 0; y++) {
        for (int32_t x = rect.x0; x < rect.x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            cov[k++] = pc_paint_dab_coverage(sqrtf(dx * dx + dy * dy), d->radius, hardness) * d->alpha;
        }
    }
    out->dab = *d;
    out->index = index;
    out->rect = rect;
    out->cov = cov;
    out->len = n;
    return PC_OK;
}

pc_rect pc_paint_stroke_bounds(const pc_paint_dab *ds, size_t n)
{
    pc_rect r = PC_RECT_EMPTY;
    for (size_t i = 0; i < n; i++) {
        r = pc_rect_union(r, pc_paint_dab_rect(&ds[i]));
    }
    return r;
}

pc_status pc_paint_stroke_coverage(const pc_paint_stroke *stroke, pc_rect *rect, float **cov, size_t *n,
                                   pc_error *err)
{
    pc_paint_renderer *r = NULL;
    pc_status st = pc_paint_renderer_new(&stroke->brush, NULL, 1.0f, &r, err);
    if (st == PC_OK) {
        st = pc_paint_renderer_push(r, stroke->points, stroke->count, err);
    }
    if (st == PC_OK) {
        st = pc_paint_renderer_finish(r, err);
    }
    if (st == PC_OK) {
        st = pc_paint_renderer_dense_coverage(r, rect, cov, n, err);
    }
    pc_paint_renderer_free(r);
    return st;
}

/* ---- compositing ---- */

pc_status pc_paint_apply_coverage(pc_surface *target, pc_rect rect, const float *cov, size_t ncov,
                                  float opacity, const pc_surface *selection, bool lock_transparency,
                                  const pc_paint_region *paint, pc_blend_mode blend, pc_rect *damage,
                                  pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(target);
    size_t n = pc_pixel_format_channels(fmt), a = 0, w, total;
    bool has_a = pc_paint_alpha_index(fmt, &a);
    pc_paint_region region;
    pc_status st;
    if (pc_rect_is_empty(rect)) {
        *damage = PC_RECT_EMPTY;
        return PC_OK;
    }
    if (paint->ch < n) {
        return pc_fail(err, PC_ERR_INVALID, "paint image has %zu channels, the target %zu", paint->ch, n);
    }
    st = pc_paint_region_read(target, rect, &region, err);
    if (st != PC_OK) {
        return st;
    }
    w = pc_rect_width(rect);
    total = region.len / n;
    for (size_t i = 0; i < ncov && i < total; i++) {
        int32_t x = (int32_t)((int64_t)rect.x0 + (int64_t)(i % w));
        int32_t y = (int32_t)((int64_t)rect.y0 + (int64_t)(i / w));
        float sel = selection != NULL ? pc_surface_sample_channel(selection, x, y, 0) : 1.0f;
        float k = pc_paint_minf_k(cov[i] * opacity * sel, 1.0f);
        float *px = region.data + i * n;
        const float *src;
        if (k <= 0.0f) {
            continue;
        }
        if (lock_transparency && has_a && px[a] <= 0.0f) {
            continue;
        }
        src = pc_paint_region_cpx(paint, x, y);
        if (src == NULL) {
            continue;
        }
        if (blend == PC_BLEND_NORMAL) {
            pc_paint_over_native(fmt, px, src, k, lock_transparency);
        } else {
            float d[4], s[4], o[4], enc[PC_RASTER_MAX_CHANNELS];
            pc_to_rgba(fmt, px, n, d);
            pc_to_rgba(fmt, src, n, s);
            pc_blend_composite(blend, d, s, k, o);
            if (lock_transparency) {
                o[3] = d[3];
            }
            (void)pc_from_rgba(fmt, o, enc);
            memcpy(px, enc, n * sizeof *px);
        }
    }
    st = pc_paint_region_write(&region, target, err);
    pc_paint_region_fini(&region);
    if (st == PC_OK) {
        *damage = rect;
    }
    return st;
}

void pc_paint_mix_premultiplied(float *dst, size_t ndst, const float *src, size_t nsrc, size_t a, float k)
{
    size_t m = ndst < nsrc ? ndst : nsrc;
    float da, sa, oa;
    if (a >= ndst || a >= nsrc) {
        return;
    }
    da = dst[a];
    sa = src[a];
    oa = da + (sa - da) * k;
    for (size_t c = 0; c < m; c++) {
        if (c == a) {
            continue;
        }
        dst[c] = oa > 1e-6f ? (dst[c] * da + (src[c] * sa - dst[c] * da) * k) / oa
                            : dst[c] + (src[c] - dst[c]) * k;
    }
    dst[a] = oa;
}

pc_status pc_paint_mix_back(pc_surface *target, const pc_paint_region *orig, const pc_paint_region *work,
                            pc_rect rect, float opacity, const pc_surface *selection, bool lock_transparency,
                            pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(target);
    size_t a = 0, n = pc_pixel_format_channels(fmt);
    bool has_a = pc_paint_alpha_index(fmt, &a);
    pc_paint_region out;
    pc_status st;
    if (pc_rect_is_empty(rect)) {
        return PC_OK;
    }
    if (orig->ch != n || work->ch != n || !pc_rect_contains_rect(orig->rect, rect) ||
        !pc_rect_contains_rect(work->rect, rect)) {
        return pc_fail(err, PC_ERR_INVALID,
                       "mix back: regions must cover the rect with the target's channels");
    }
    st = pc_paint_region_crop(orig, rect, &out, err);
    if (st != PC_OK) {
        return st;
    }
    opacity = pc_clampf(opacity, 0.0f, 1.0f);
    for (int32_t y = rect.y0; y < rect.y1; y++) {
        for (int32_t x = rect.x0; x < rect.x1; x++) {
            float k = opacity * (selection != NULL ? pc_surface_sample_channel(selection, x, y, 0) : 1.0f);
            const float *wv;
            float *o;
            if (k <= 0.0f) {
                continue;
            }
            wv = pc_paint_region_cpx(work, x, y);
            o = pc_paint_region_px(&out, x, y);
            if (wv == NULL || o == NULL) {
                continue; /* unreachable: both cover rect */
            }
            if (has_a && !lock_transparency) {
                /* Alpha changes: mix premultiplied, so colour under transparent pixels doesn't
                 * bleed in. */
                pc_paint_mix_premultiplied(o, n, wv, n, a, k);
            } else {
                for (size_t c = 0; c < n; c++) {
                    if (lock_transparency && has_a && c == a) {
                        continue;
                    }
                    o[c] += (wv[c] - o[c]) * k;
                }
            }
        }
    }
    st = pc_paint_region_write(&out, target, err);
    pc_paint_region_fini(&out);
    return st;
}

pc_status pc_paint_apply_dab_stroke(pc_surface *target, const pc_paint_stroke *stroke,
                                    const pc_surface *selection, bool lock_transparency, int32_t halo,
                                    pc_paint_dab_effect effect, void *ctx, pc_rect *damage, pc_error *err)
{
    pc_paint_dab_vec ds = {0};
    pc_paint_context bc;
    pc_paint_region orig = {0}, work = {0};
    pc_rect bounds = PC_RECT_EMPTY;
    pc_status st = pc_paint_dabs(&stroke->brush, stroke->points, stroke->count, &ds, err);
    if (st != PC_OK) {
        pc_paint_dab_vec_fini(&ds);
        return st;
    }
    if (ds.len == 0) {
        pc_paint_dab_vec_fini(&ds);
        *damage = PC_RECT_EMPTY;
        return PC_OK;
    }
    st = pc_paint_context_init(&bc, &stroke->brush, err);
    if (st != PC_OK) {
        pc_paint_dab_vec_fini(&ds);
        return st;
    }
    for (size_t i = 0; i < ds.len; i++) {
        bounds = pc_rect_union(bounds, pc_paint_context_dab_rect(&bc, &ds.v[i], false));
    }
    st = pc_paint_region_read(target, pc_rect_inflate(bounds, halo), &orig, err);
    if (st == PC_OK) {
        st = pc_paint_region_clone(&orig, &work, err);
    }
    for (size_t i = 0; st == PC_OK && i < ds.len; i++) {
        pc_paint_footprint fp;
        st = pc_paint_context_footprint(&bc, &ds.v[i], i, &fp, err);
        if (st == PC_OK) {
            st = effect != NULL ? effect(ctx, &work, &fp, err) : PC_OK;
            pc_paint_footprint_fini(&fp);
        }
    }
    if (st == PC_OK) {
        st = pc_paint_mix_back(target, &orig, &work, bounds, stroke->brush.opacity, selection,
                               lock_transparency, err);
    }
    if (st == PC_OK) {
        *damage = bounds;
    }
    pc_paint_region_fini(&orig);
    pc_paint_region_fini(&work);
    pc_paint_context_fini(&bc);
    pc_paint_dab_vec_fini(&ds);
    return st;
}

/* ---- smudge ---- */

pc_status pc_paint_smudge_init(pc_paint_smudge *s, float strength, const float *finger, size_t finger_len,
                               float max_brush_size, pc_error *err)
{
    float *f = NULL;
    if (finger != NULL) {
        f = pc_malloc_array(finger_len, sizeof *f);
        if (f == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a smudge colour");
        }
        if (finger_len > 0) {
            memcpy(f, finger, finger_len * sizeof *f);
        }
    }
    memset(s, 0, sizeof *s);
    s->strength = pc_clampf(strength, 0.0f, 1.0f);
    s->finger = f;
    s->finger_len = finger_len;
    s->radius = pc_i32_sat_add(pc_f32_to_i32(ceilf(max_brush_size / 2.0f)), 2);
    return PC_OK;
}

void pc_paint_smudge_with_alpha(pc_paint_smudge *s, bool has_alpha, size_t a)
{
    s->has_alpha = has_alpha;
    s->alpha = a;
}

void pc_paint_smudge_fini(pc_paint_smudge *s)
{
    if (s == NULL) {
        return;
    }
    free(s->finger);
    free(s->carry);
    s->finger = NULL;
    s->carry = NULL;
    s->finger_len = 0;
    s->carry_len = 0;
}

/* Premultiply a straight pixel of n channels in place (no-op without alpha). */
static void premultiply(float *px, size_t n, bool has_alpha, size_t a)
{
    if (!has_alpha || a >= n) {
        return;
    }
    for (size_t c = 0; c < n; c++) {
        if (c != a) {
            px[c] *= px[a];
        }
    }
}

static int32_t clamp_i64(int64_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : (int32_t)v);
}

/* The first dab's carry: the finger colour, or the pixels around the dab centre (edge-clamped
 * into the working area). */
static void smudge_fill_carry(const pc_paint_smudge *s, const pc_paint_region *work, int64_t cx, int64_t cy,
                              size_t side, bool has_alpha, size_t a, float *carry)
{
    size_t n = work->ch;
    pc_rect wr = work->rect;
    float v[PC_RASTER_MAX_CHANNELS];
    if (s->finger != NULL) {
        for (size_t c = 0; c < n; c++) {
            v[c] = c < s->finger_len ? s->finger[c] : 1.0f;
        }
        premultiply(v, n, has_alpha, a);
    }
    for (size_t qy = 0; qy < side; qy++) {
        for (size_t qx = 0; qx < side; qx++) {
            float *dst = carry + (qy * side + qx) * n;
            if (s->finger != NULL) {
                memcpy(dst, v, n * sizeof *dst);
            } else {
                int32_t x = clamp_i64(cx + (int64_t)qx - s->radius, wr.x0, wr.x1 - 1);
                int32_t y = clamp_i64(cy + (int64_t)qy - s->radius, wr.y0, wr.y1 - 1);
                const float *p = pc_paint_region_cpx(work, x, y);
                if (p != NULL) {
                    memcpy(dst, p, n * sizeof *dst);
                    premultiply(dst, n, has_alpha, a);
                }
            }
        }
    }
}

pc_status pc_paint_smudge_dab(pc_paint_smudge *s, pc_paint_region *work, const pc_paint_footprint *fp,
                              pc_error *err)
{
    size_t n = work->ch, side, need;
    int64_t r = s->radius, cx = pc_f64_to_i32(floor(fp->dab.center.x)),
            cy = pc_f64_to_i32(floor(fp->dab.center.y));
    pc_rect wr = work->rect;
    bool has_alpha = s->has_alpha && s->alpha < n;
    size_t a = s->alpha;
    float keep;
    if (r < 0 || n == 0 || n > PC_RASTER_MAX_CHANNELS) {
        return pc_fail(err, PC_ERR_LIMIT, "smudge radius or channel count out of range");
    }
    side = (size_t)(2 * r + 1);
    if (!pc_size_mul3(side, side, n, &need) || need > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "smudge brush too large");
    }
    if (pc_rect_is_empty(wr)) {
        return PC_OK; /* nothing to read or write (the reference can't clamp into an empty area) */
    }
    if (s->carry == NULL) {
        s->carry = pc_malloc_array(need, sizeof *s->carry);
        if (s->carry == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for the smudge colour");
        }
        s->carry_len = need;
        smudge_fill_carry(s, work, cx, cy, side, has_alpha, a, s->carry);
    } else if (s->carry_len != need) {
        return pc_fail(err, PC_ERR_INVALID, "smudge working area changed channel count");
    }
    /* Lay down the carried colour. */
    for (int32_t y = fp->rect.y0 > wr.y0 ? fp->rect.y0 : wr.y0;
         y < (fp->rect.y1 < wr.y1 ? fp->rect.y1 : wr.y1); y++) {
        for (int32_t x = fp->rect.x0 > wr.x0 ? fp->rect.x0 : wr.x0;
             x < (fp->rect.x1 < wr.x1 ? fp->rect.x1 : wr.x1); x++) {
            float k = pc_paint_footprint_at(fp, x, y) * s->strength;
            int64_t qx = (int64_t)x - cx + r, qy = (int64_t)y - cy + r;
            const float *cq;
            float *px;
            if (k <= 0.0f || qx < 0 || qy < 0 || qx >= (int64_t)side || qy >= (int64_t)side) {
                continue;
            }
            cq = s->carry + ((size_t)qy * side + (size_t)qx) * n;
            px = pc_paint_region_px(work, x, y);
            if (px == NULL) {
                continue;
            }
            if (has_alpha) {
                /* Back to straight for the mix (premultiplied lerp towards the carried colour). */
                float ca = cq[a], buf[PC_RASTER_MAX_CHANNELS];
                memcpy(buf, cq, n * sizeof *buf);
                if (ca > 1e-6f) {
                    for (size_t c = 0; c < n; c++) {
                        if (c != a) {
                            buf[c] /= ca;
                        }
                    }
                }
                pc_paint_mix_premultiplied(px, n, buf, n, a, k);
            } else {
                for (size_t c = 0; c < n; c++) {
                    px[c] += (cq[c] - px[c]) * k;
                }
            }
        }
    }
    /* Pick up: the carried colour keeps `strength` of itself. Only cells over the working area
     * change, so the loops visit just those. */
    keep = s->strength;
    {
        int64_t ox = cx - r, oy = cy - r;
        int64_t qy0 = (int64_t)wr.y0 - oy, qy1 = (int64_t)wr.y1 - oy;
        int64_t qx0 = (int64_t)wr.x0 - ox, qx1 = (int64_t)wr.x1 - ox;
        qy0 = qy0 < 0 ? 0 : qy0;
        qx0 = qx0 < 0 ? 0 : qx0;
        qy1 = qy1 > (int64_t)side ? (int64_t)side : qy1;
        qx1 = qx1 > (int64_t)side ? (int64_t)side : qx1;
        for (int64_t qy = qy0; qy < qy1; qy++) {
            for (int64_t qx = qx0; qx < qx1; qx++) {
                const float *px = pc_paint_region_cpx(work, (int32_t)(ox + qx), (int32_t)(oy + qy));
                float *cq = s->carry + ((size_t)qy * side + (size_t)qx) * n;
                float pa;
                if (px == NULL) {
                    continue;
                }
                pa = has_alpha ? px[a] : 1.0f;
                for (size_t c = 0; c < n; c++) {
                    float v = has_alpha && c != a ? px[c] * pa : px[c];
                    cq[c] = cq[c] * keep + v * (1.0f - keep);
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_paint_smudge_effect(void *ctx, pc_paint_region *work, const pc_paint_footprint *fp,
                                 pc_error *err)
{
    return pc_paint_smudge_dab(ctx, work, fp, err);
}
