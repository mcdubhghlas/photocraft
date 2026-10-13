/* Color Range, Modify and lasso polygons (port of crates/algo/src/selection.rs, part 2). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/selection.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* ---- Sampled Colors ---- */

void pc_algo_selection_color_range_samples(const float (*px)[4], size_t n, size_t w,
                                           const pc_algo_range_sample *samples, size_t nsamples,
                                           float fuzziness, bool has_radius, float radius, float *mask)
{
    float f = pc_rs_maxf_k(fuzziness, 0.0f) + 1.0f;
    size_t ww = w > 0 ? w : 1, i, k;
    int c;
    for (i = 0; i < n; i++) {
        float best = 0.0f, x, y;
        if (px[i][3] <= 0.0f) {
            mask[i] = 0.0f;
            continue;
        }
        x = (float)(i % ww);
        y = (float)(i / ww);
        for (k = 0; k < nsamples; k++) {
            const pc_algo_range_sample *s = &samples[k];
            float d = 0.0f, kv;
            for (c = 0; c < 3; c++) {
                d = pc_rs_maxf(d, fabsf(px[i][c] - s->color[c]) * 255.0f);
            }
            kv = pc_clampf(1.0f - d / f, 0.0f, 1.0f);
            if (has_radius && s->has_at) {
                float dx = x - s->at_x, dy = y - s->at_y;
                float dist = sqrtf(dx * dx + dy * dy);
                kv *= pc_clampf(1.0f - dist / pc_rs_maxf_k(radius, 1.0f), 0.0f, 1.0f);
            }
            best = pc_rs_maxf(best, kv);
        }
        mask[i] = best;
    }
}

/* ---- Lab ranges ---- */

pc_algo_lab_range pc_algo_lab_range_point(const float lab[3])
{
    pc_algo_lab_range r;
    memcpy(r.lo, lab, sizeof r.lo);
    memcpy(r.hi, lab, sizeof r.hi);
    return r;
}

bool pc_algo_lab_range_spanning(const float (*colors)[3], size_t n, pc_algo_lab_range *out)
{
    pc_algo_lab_range r;
    size_t i;
    if (n == 0) {
        return false;
    }
    r = pc_algo_lab_range_point(colors[0]);
    for (i = 0; i < n; i++) {
        r = pc_algo_lab_range_grow(r, colors[i]);
    }
    *out = r;
    return true;
}

pc_algo_lab_range pc_algo_lab_range_grow(pc_algo_lab_range r, const float lab[3])
{
    int k;
    for (k = 0; k < 3; k++) {
        r.lo[k] = pc_rs_minf(r.lo[k], lab[k]);
        r.hi[k] = pc_rs_maxf(r.hi[k], lab[k]);
    }
    return r;
}

pc_algo_lab_range pc_algo_lab_range_subtract(pc_algo_lab_range self, const float lab[3], float fuzziness)
{
    float fz = isfinite(fuzziness) ? pc_rs_maxf_k(fuzziness, 0.0f) : 0.0f;
    pc_algo_lab_range r = self;
    int k;
    for (k = 0; k < 3; k++) {
        float m = lab[k];
        /* Measured: the lightness step has no 256/255 factor, the a/b steps do. */
        float w = k == 0 ? fz / 2.55f : fz * 256.0f / 255.0f / 3.0f;
        float lo = r.lo[k], hi = r.hi[k], q, d, nn;
        if (!isfinite(m)) {
            continue;
        }
        if (m > lo && m < hi) {
            if (m - lo < hi - m) {
                r.lo[k] = m;
            } else {
                r.hi[k] = m;
            }
            continue;
        }
        q = w / 5.0f;
        if (q <= 0.0f) {
            continue;
        }
        d = m <= lo ? lo - m : m - hi;
        nn = floorf(d / q) + 1.0f;
        if (nn > 5.0f) {
            continue;
        }
        if (m <= lo) {
            r.lo[k] = pc_rs_minf(m + nn * q, hi);
        } else {
            r.hi[k] = pc_rs_maxf(m - nn * q, lo);
        }
    }
    return r;
}

static float outside(float v, float lo, float hi)
{
    return pc_rs_maxf_k(pc_rs_maxf(lo - v, v - hi), 0.0f);
}

static float ab_dist(float v, float lo, float hi, bool grid8)
{
    if (!grid8) {
        return outside(v, lo, hi);
    }
    if (v > hi) {
        return v - hi + 0.5f;
    }
    if (v < lo) {
        return pc_rs_maxf_k(lo - v - 0.5f, 0.0f);
    }
    return 0.0f;
}

static float coverage(const float lab[3], const pc_algo_lab_range *range, float fuzziness, bool grid8)
{
    float dl, da, db, t, v;
    if (isnan(fuzziness) || fuzziness <= 0.0f) {
        float d = pc_rs_maxf(pc_rs_maxf(outside(lab[0], range->lo[0], range->hi[0]) * 2.55f,
                                        outside(lab[1], range->lo[1], range->hi[1])),
                             outside(lab[2], range->lo[2], range->hi[2]));
        return d < 0.5f ? 1.0f : 0.0f;
    }
    dl = outside(lab[0], range->lo[0], range->hi[0]) * 2.55f;
    da = 3.0f * ab_dist(lab[1], range->lo[1], range->hi[1], grid8);
    db = 3.0f * ab_dist(lab[2], range->lo[2], range->hi[2], grid8);
    t = pc_rs_maxf(pc_rs_maxf(dl, da), db) / (fuzziness * 256.0f / 255.0f);
    if (t <= 0.5f) {
        v = 1.0f - 2.0f * t * t;
    } else if (t < 1.0f) {
        v = 2.0f * (1.0f - t) * (1.0f - t);
    } else {
        v = 0.0f;
    }
    return pc_clampf(floorf(v * 256.0f), 0.0f, 255.0f) / 255.0f;
}

float pc_algo_lab_range_coverage(const float lab[3], const pc_algo_lab_range *range, float fuzziness)
{
    return coverage(lab, range, fuzziness, false);
}

float pc_algo_lab_range_coverage_8bit(const float lab[3], const pc_algo_lab_range *range, float fuzziness)
{
    return coverage(lab, range, fuzziness, true);
}

void pc_algo_quantize_lab8(const float lab[3], float out[3])
{
    out[0] = roundf(lab[0] * 2.55f) / 2.55f;
    out[1] = roundf(lab[1]);
    out[2] = roundf(lab[2]);
}

typedef struct lab_ctx {
    const float (*lab)[4];
    size_t n, w;
    const pc_algo_lab_range *range;
    float fuzziness;
    bool grid8, localized;
    float radius;
    const float (*points)[2];
    size_t npoints;
    float *mask;
} lab_ctx;

static pc_status lab_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    lab_ctx *c = ctx;
    size_t y, x, j;
    (void)chunk;
    for (y = begin; y < end; y++) {
        for (x = 0; x < c->w; x++) {
            size_t i = y * c->w + x;
            const float *p;
            float k;
            if (i >= c->n) {
                break;
            }
            p = c->lab[i];
            if (isnan(p[3]) || p[3] <= 0.0f) {
                continue;
            }
            k = coverage(p, c->range, c->fuzziness, c->grid8);
            if (c->localized && k > 0.0f) {
                float px = (float)x, py = (float)y, near = 0.0f;
                for (j = 0; j < c->npoints; j++) {
                    float dx = px - c->points[j][0], dy = py - c->points[j][1];
                    float v = pc_clampf(1.0f - sqrtf(dx * dx + dy * dy) / pc_rs_maxf_k(c->radius, 1.0f), 0.0f,
                                        1.0f);
                    near = pc_rs_maxf(near, v);
                }
                k *= near;
            }
            c->mask[i] = k;
        }
    }
    return PC_OK;
}

pc_status pc_algo_selection_color_range_lab(const float (*lab)[4], size_t n, size_t w,
                                            const pc_algo_lab_range *range, float fuzziness, bool grid8,
                                            bool localized, float radius, const float (*points)[2],
                                            size_t npoints, float *mask, pc_error *err)
{
    lab_ctx c;
    (void)err;
    if (n == 0) {
        return PC_OK;
    }
    memset(mask, 0, n * sizeof(float));
    c.lab = lab;
    c.n = n;
    c.w = w > 0 ? w : 1;
    c.range = range;
    c.fuzziness = fuzziness;
    c.grid8 = grid8;
    /* Without positions, Localized Color Clusters have nothing to centre on. */
    c.localized = localized && npoints > 0 && points != NULL;
    c.radius = radius;
    c.points = points;
    c.npoints = npoints;
    c.mask = mask;
    return pc_algo_par_for((n + c.w - 1) / c.w, 1, lab_rows, &c);
}

/* Hue (degrees, 0..360) and chroma of an RGB colour. */
static void hue_chroma(const float p[3], float *hue, float *chroma)
{
    float r = p[0], g = p[1], b = p[2];
    float mx = pc_rs_maxf(pc_rs_maxf(r, g), b), mn = pc_rs_minf(pc_rs_minf(r, g), b), d = mx - mn, h;
    if (d <= 0.0f) {
        *hue = 0.0f;
        *chroma = 0.0f;
        return;
    }
    if (mx == r) {
        h = pc_algo_rem_euclidf((g - b) / d, 6.0f);
    } else if (mx == g) {
        h = (b - r) / d + 2.0f;
    } else {
        h = (r - g) / d + 4.0f;
    }
    *hue = h * 60.0f;
    *chroma = d;
}

void pc_algo_selection_hue_range(const float (*px)[4], size_t n, float center, float *mask)
{
    size_t i;
    for (i = 0; i < n; i++) {
        float c[3], h, chroma, dh;
        int k;
        if (px[i][3] <= 0.0f) {
            mask[i] = 0.0f;
            continue;
        }
        for (k = 0; k < 3; k++) {
            c[k] = pc_clampf(px[i][k], 0.0f, 1.0f);
        }
        hue_chroma(c, &h, &chroma);
        dh = pc_algo_rem_euclidf(h - center, 360.0f);
        dh = pc_rs_minf(dh, 360.0f - dh);
        mask[i] = pc_clampf(pc_clampf(1.0f - dh / 60.0f, 0.0f, 1.0f) * chroma, 0.0f, 1.0f);
    }
}

void pc_algo_selection_tone_range(const float (*px)[4], size_t n, float lo, float hi, float falloff,
                                  float *mask)
{
    float ramp = pc_rs_maxf_k(falloff, 1e-3f);
    size_t i;
    for (i = 0; i < n; i++) {
        float l, out;
        if (px[i][3] <= 0.0f) {
            mask[i] = 0.0f;
            continue;
        }
        l = pc_clampf(0.299f * px[i][0] + 0.587f * px[i][1] + 0.114f * px[i][2], 0.0f, 1.0f) * 255.0f;
        out = l < lo ? lo - l : (l > hi ? l - hi : 0.0f);
        mask[i] = pc_clampf(1.0f - out / ramp, 0.0f, 1.0f);
    }
}

/* ---- expand / contract / border (the distance transform is in selection_distance.c) ---- */

static pc_status mask_count(size_t w, size_t h, size_t *n, pc_error *err)
{
    if (!pc_size_mul(w, h, n) || *n > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "selection mask too large");
    }
    return PC_OK;
}

pc_status pc_algo_selection_expand(const float *m, size_t w, size_t h, float r, float **out, pc_error *err)
{
    size_t n, i;
    bool *inside;
    float *d;
    pc_status st = mask_count(w, h, &n, err);
    if (st != PC_OK) {
        return st;
    }
    inside = pc_malloc_array(n ? n : 1, sizeof(bool));
    if (inside == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory expanding a selection");
    }
    for (i = 0; i < n; i++) {
        inside[i] = m[i] >= 0.5f;
    }
    st = pc_algo_selection_edt(inside, w, h, &d, err);
    free(inside);
    if (st != PC_OK) {
        return st;
    }
    /* A pixel whose centre is d away from the nearest selected centre is covered when d <= r
     * (fractional radii give partial coverage). */
    for (i = 0; i < n; i++) {
        d[i] = pc_rs_maxf(m[i], pc_clampf(r + 1.0f - d[i], 0.0f, 1.0f));
    }
    *out = d;
    return PC_OK;
}

pc_status pc_algo_selection_contract(const float *m, size_t w, size_t h, float r, float **out, pc_error *err)
{
    size_t n, i;
    float *inv, *e;
    pc_status st = mask_count(w, h, &n, err);
    if (st != PC_OK) {
        return st;
    }
    inv = pc_malloc_array(n ? n : 1, sizeof(float));
    if (inv == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory contracting a selection");
    }
    for (i = 0; i < n; i++) {
        inv[i] = 1.0f - m[i];
    }
    st = pc_algo_selection_expand(inv, w, h, r, &e, err);
    free(inv);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n; i++) {
        e[i] = 1.0f - e[i];
    }
    *out = e;
    return PC_OK;
}

/* Photoshop's soft band 2r wide centred on the edge: a pixel D away (centre to centre) from the
 * nearest pixel on the other side is selected 1 - (D - 1) / r. */
pc_status pc_algo_selection_border(const float *m, size_t w, size_t h, float r, float **out, pc_error *err)
{
    float *to_in = NULL, *to_out = NULL, *res;
    bool *inside, *outside;
    size_t n, i;
    pc_status st = mask_count(w, h, &n, err);
    if (st != PC_OK) {
        return st;
    }
    inside = pc_malloc_array(n ? n : 1, sizeof(bool));
    outside = pc_malloc_array(n ? n : 1, sizeof(bool));
    res = pc_calloc_array(n ? n : 1, sizeof(float));
    if (inside == NULL || outside == NULL || res == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a selection border");
        goto done;
    }
    for (i = 0; i < n; i++) {
        inside[i] = m[i] >= 0.5f;
        outside[i] = !inside[i];
    }
    if ((st = pc_algo_selection_edt(inside, w, h, &to_in, err)) != PC_OK ||
        (st = pc_algo_selection_edt(outside, w, h, &to_out, err)) != PC_OK) {
        goto done;
    }
    if (!(r <= 0.0f) && n > 0) {
        for (i = 0; i < n; i++) {
            float d = inside[i] ? to_out[i] : to_in[i];
            res[i] = pc_clampf(1.0f - (d - 1.0f) / r, 0.0f, 1.0f);
        }
    }
    *out = res;
    res = NULL;
done:
    free(inside);
    free(outside);
    free(to_in);
    free(to_out);
    free(res);
    return st;
}

/* ---- polygon ---- */

/* f32::total_cmp as a qsort comparator. */
static int total_cmp(const void *a, const void *b)
{
    int32_t x, y;
    memcpy(&x, a, 4);
    memcpy(&y, b, 4);
    x ^= (int32_t)(((uint32_t)(x >> 31)) >> 1);
    y ^= (int32_t)(((uint32_t)(y >> 31)) >> 1);
    return x < y ? -1 : (x > y ? 1 : 0);
}

pc_status pc_algo_selection_polygon(const float (*points)[2], size_t npoints, pc_rect area, bool anti_alias,
                                    float **out, size_t *len, pc_error *err)
{
    size_t w = pc_rect_width(area), h = pc_rect_height(area), n, py, i;
    int sub = anti_alias ? 4 : 1, s;
    float *m, *xs;
    pc_status st = mask_count(w, h, &n, err);
    if (st != PC_OK) {
        return st;
    }
    m = pc_calloc_array(n ? n : 1, sizeof(float));
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a polygon");
    }
    if (npoints < 3) {
        *out = m;
        *len = n;
        return PC_OK;
    }
    xs = pc_malloc_array(npoints, sizeof(float));
    if (xs == NULL) {
        free(m);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a polygon");
    }
    for (py = 0; py < h; py++) {
        for (s = 0; s < sub; s++) {
            float y = (float)area.y0 + (float)py + ((float)s + 0.5f) / (float)sub;
            size_t nx = 0, sp;
            for (i = 0; i < npoints; i++) {
                const float *a = points[i], *b = points[(i + 1) % npoints];
                if ((a[1] <= y && b[1] > y) || (b[1] <= y && a[1] > y)) {
                    xs[nx++] = a[0] + (y - a[1]) / (b[1] - a[1]) * (b[0] - a[0]);
                }
            }
            qsort(xs, nx, sizeof(float), total_cmp);
            for (sp = 0; sp + 1 < nx; sp += 2) {
                float x0 = pc_rs_maxf_k(xs[sp] - (float)area.x0, 0.0f);
                float x1 = pc_rs_minf(xs[sp + 1] - (float)area.x0, (float)w);
                size_t c0, c1, cx;
                if (x1 <= x0) {
                    continue;
                }
                c0 = pc_f32_to_size(floorf(x0));
                c1 = pc_f32_to_size(ceilf(x1));
                if (c1 > w) {
                    c1 = w;
                }
                for (cx = c0; cx < c1; cx++) {
                    float cov;
                    if (anti_alias) {
                        cov = pc_rs_minf((float)(cx + 1), x1) - pc_rs_maxf((float)cx, x0);
                    } else {
                        float c = (float)cx + 0.5f;
                        cov = c >= x0 && c < x1 ? 1.0f : 0.0f;
                    }
                    m[py * w + cx] += pc_rs_maxf_k(cov, 0.0f) / (float)sub;
                }
            }
        }
    }
    free(xs);
    for (i = 0; i < n; i++) {
        m[i] = pc_rs_minf_k(m[i], 1.0f);
    }
    *out = m;
    *len = n;
    return PC_OK;
}
