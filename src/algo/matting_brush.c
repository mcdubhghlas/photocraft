/* Select and Mask's Refine Edge Brush (port of crates/algo/src/matting/brush.rs). See matting.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/matting.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

typedef struct pt {
    float x, y;
} pt;

static float size_of(const pc_algo_edge_stroke *s)
{
    return pc_clampf(s->size, PC_ALGO_EDGE_BRUSH_SIZE_MIN, PC_ALGO_EDGE_BRUSH_SIZE_MAX);
}

/* Coverage (0-1) of a round brush of radius r swept from a to b, at pixel centre p. */
static float capsule(pt p, pt a, pt b, float r)
{
    float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy, t = 0.0f, cx, cy, ex, ey;
    if (len2 > 0.0f) {
        t = pc_clampf(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2, 0.0f, 1.0f);
    }
    cx = a.x + t * dx;
    cy = a.y + t * dy;
    ex = p.x - cx;
    ey = p.y - cy;
    return pc_clampf(r + 0.5f - sqrtf(ex * ex + ey * ey), 0.0f, 1.0f);
}

static int32_t clamp_coord(float v)
{
    return pc_f32_to_i32(pc_clampf(v, -1073741824.0f, 1073741824.0f));
}

/* The pixels a disc of radius r at p can touch (possibly empty). */
static pc_rect dab_bounds(pt p, float r)
{
    return pc_rect_make(clamp_coord(floorf(p.x - r - 1.0f)), clamp_coord(floorf(p.y - r - 1.0f)),
                        clamp_coord(ceilf(p.x + r + 1.0f)), clamp_coord(ceilf(p.y + r + 1.0f)));
}

static pt point(const pc_algo_edge_stroke *s, size_t i)
{
    pt p = {s->points[i][0], s->points[i][1]};
    return p;
}

double pc_algo_edge_brush_swept_area(const pc_algo_edge_stroke *strokes, size_t n)
{
    double total = -0.0;
    size_t k, i;
    for (k = 0; k < n; k++) {
        const pc_algo_edge_stroke *s = &strokes[k];
        double size = (double)size_of(s), len = -0.0;
        for (i = 1; i < s->npoints; i++) {
            len +=
                (double)hypotf(s->points[i][0] - s->points[i - 1][0], s->points[i][1] - s->points[i - 1][1]);
        }
        total += isfinite(len) ? len * size + size * size : INFINITY;
    }
    return total;
}

static bool usable(const pc_algo_edge_stroke *s)
{
    size_t i;
    if (!isfinite(s->size) || s->npoints == 0 || s->points == NULL) {
        return false;
    }
    for (i = 0; i < s->npoints; i++) {
        if (!isfinite(s->points[i][0]) || !isfinite(s->points[i][1])) {
            return false;
        }
    }
    return true;
}

static pc_rect stroke_bounds(const pc_algo_edge_stroke *s, pc_rect canvas)
{
    float r = size_of(s) / 2.0f;
    pc_rect acc = PC_RECT_EMPTY;
    size_t i;
    for (i = 0; i < s->npoints; i++) {
        acc = pc_rect_union(acc, dab_bounds(point(s, i), r));
    }
    return pc_rect_intersect(acc, canvas);
}

static bool pt_eq(pt a, pt b)
{
    return a.x == b.x && a.y == b.y;
}

/* Paints one segment's capsule into cov (the most any segment gives) within sb. */
static void paint_segment(float *cov, pc_rect sb, pt a, pt b, float r)
{
    pc_rect seg = pc_rect_intersect(pc_rect_union(dab_bounds(a, r), dab_bounds(b, r)), sb);
    size_t sw = pc_rect_width(sb);
    int32_t x, y;
    for (y = seg.y0; y < seg.y1; y++) {
        for (x = seg.x0; x < seg.x1; x++) {
            pt p = {(float)x + 0.5f, (float)y + 0.5f};
            float c = capsule(p, a, b, r);
            float *slot = &cov[(size_t)(y - sb.y0) * sw + (size_t)(x - sb.x0)];
            if (c > *slot) {
                *slot = c;
            }
        }
    }
}

/* The stroke's path as short segments: points closer than a quarter of the radius to the last
 * kept one are dropped, and long segments are split at most r long. kept: scratch of npoints + 1. */
static void paint_stroke(float *cov, pc_rect sb, const pc_algo_edge_stroke *s, float r, pt *kept)
{
    float min_step = pc_rs_maxf_k(r / 4.0f, 0.5f), max_step = pc_rs_maxf_k(r, 2.0f);
    pt first = point(s, 0), end = point(s, s->npoints - 1);
    size_t nk = 1, i, k;
    kept[0] = first;
    for (i = 1; i < s->npoints; i++) {
        pt p = point(s, i), last = kept[nk - 1];
        if (hypotf(p.x - last.x, p.y - last.y) >= min_step) {
            kept[nk++] = p;
        }
    }
    if (!pt_eq(kept[nk - 1], end)) {
        kept[nk++] = end;
    }
    if (nk == 1) {
        paint_segment(cov, sb, first, first, r);
        return;
    }
    for (i = 0; i + 1 < nk; i++) {
        pt a = kept[i], b = kept[i + 1];
        size_t pieces = pc_f32_to_size(pc_clampf(ceilf(hypotf(b.x - a.x, b.y - a.y) / max_step), 1.0f, 1e6f));
        for (k = 0; k < pieces; k++) {
            float t0 = (float)k / (float)pieces, t1 = (float)(k + 1) / (float)pieces;
            pt prev = {a.x + (b.x - a.x) * t0, a.y + (b.y - a.y) * t0};
            pt next = {a.x + (b.x - a.x) * t1, a.y + (b.y - a.y) * t1};
            paint_segment(cov, sb, prev, next, r);
        }
    }
}

pc_status pc_algo_edge_brush_new(const pc_algo_edge_stroke *strokes, size_t n, pc_rect canvas,
                                 pc_algo_edge_brush *out, bool *some, pc_error *err)
{
    pc_rect bbox = PC_RECT_EMPTY;
    size_t bw, bh, len, k, maxpts = 0;
    int8_t *v = NULL;
    float *cov = NULL, max_add = 0.0f;
    pt *kept = NULL;
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN, x, y;
    bool any = false;
    pc_status st = PC_OK;
    *some = false;
    if (n > 0 && strokes == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no strokes");
    }
    if (pc_algo_edge_brush_swept_area(strokes, n) > PC_ALGO_EDGE_BRUSH_MAX_SWEPT_AREA) {
        return PC_OK;
    }
    for (k = 0; k < n; k++) {
        if (usable(&strokes[k])) {
            bbox = pc_rect_union(bbox, stroke_bounds(&strokes[k], canvas));
            maxpts = strokes[k].npoints > maxpts ? strokes[k].npoints : maxpts;
        }
    }
    if (pc_rect_is_empty(bbox)) {
        return PC_OK;
    }
    bw = pc_rect_width(bbox);
    bh = pc_rect_height(bbox);
    if (!pc_size_mul(bw, bh, &len) || len > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "edge brush area too large");
    }
    v = pc_calloc_array(len, 1);
    cov = pc_malloc_array(len, sizeof(float));
    kept = pc_malloc_array(maxpts + 1, sizeof(pt));
    if (v == NULL || cov == NULL || kept == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for an edge brush");
        goto done;
    }
    for (k = 0; k < n; k++) {
        const pc_algo_edge_stroke *s = &strokes[k];
        pc_rect sb;
        size_t sw;
        float r, target;
        if (!usable(s)) {
            continue;
        }
        sb = stroke_bounds(s, canvas);
        if (pc_rect_is_empty(sb)) {
            continue;
        }
        r = size_of(s) / 2.0f;
        if (s->add) {
            max_add = pc_rs_maxf(max_add, 2.0f * r);
        }
        sw = pc_rect_width(sb);
        memset(cov, 0, sw * pc_rect_height(sb) * sizeof(float));
        paint_stroke(cov, sb, s, r, kept);
        target = s->add ? 1.0f : -1.0f;
        for (y = sb.y0; y < sb.y1; y++) {
            for (x = sb.x0; x < sb.x1; x++) {
                float c = cov[(size_t)(y - sb.y0) * sw + (size_t)(x - sb.x0)], cur;
                int8_t *slot;
                if (c <= 0.0f) {
                    continue;
                }
                slot = &v[(size_t)(y - bbox.y0) * bw + (size_t)(x - bbox.x0)];
                cur = (float)*slot / 127.0f;
                *slot = (int8_t)pc_f32_to_i32(
                    pc_clampf(roundf((cur + (target - cur) * c) * 127.0f), -127.0f, 127.0f));
            }
        }
    }
    for (k = 0; k < len; k++) {
        any |= v[k] != 0;
    }
    if (!any) {
        goto done;
    }
    for (y = 0; (size_t)y < bh; y++) {
        const int8_t *line = v + (size_t)y * bw;
        size_t f = bw, l = 0, i;
        for (i = 0; i < bw; i++) {
            if (line[i] > 0) {
                if (f == bw) {
                    f = i;
                }
                l = i;
            }
        }
        if (f == bw) {
            continue;
        }
        x0 = x0 < bbox.x0 + (int32_t)f ? x0 : bbox.x0 + (int32_t)f;
        x1 = x1 > bbox.x0 + (int32_t)l + 1 ? x1 : bbox.x0 + (int32_t)l + 1;
        y0 = y0 < bbox.y0 + y ? y0 : bbox.y0 + y;
        y1 = y1 > bbox.y0 + y + 1 ? y1 : bbox.y0 + y + 1;
    }
    out->bbox = bbox;
    out->v = v;
    out->len = len;
    out->added = x0 < x1 ? pc_rect_make(x0, y0, x1, y1) : PC_RECT_EMPTY;
    out->window = pc_f32_to_size(pc_clampf(ceilf(max_add), 4.0f, 64.0f));
    *some = true;
    v = NULL;
done:
    free(v);
    free(cov);
    free(kept);
    return st;
}

void pc_algo_edge_brush_fini(pc_algo_edge_brush *b)
{
    if (b == NULL) {
        return;
    }
    free(b->v);
    b->v = NULL;
    b->len = 0;
    b->bbox = PC_RECT_EMPTY;
    b->added = PC_RECT_EMPTY;
}

float pc_algo_edge_brush_at(const pc_algo_edge_brush *b, int32_t x, int32_t y)
{
    size_t i;
    if (b == NULL || b->v == NULL || !pc_rect_contains(b->bbox, x, y)) {
        return 0.0f;
    }
    i = (size_t)(y - b->bbox.y0) * pc_rect_width(b->bbox) + (size_t)(x - b->bbox.x0);
    return i < b->len ? (float)b->v[i] / 127.0f : 0.0f;
}

void pc_algo_edge_brush_read(const pc_algo_edge_brush *b, pc_rect r, float *out, size_t n)
{
    size_t i = 0;
    int32_t x, y;
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1 && i < n; x++) {
            out[i++] = pc_algo_edge_brush_at(b, x, y);
        }
    }
}

/* ---- the colour estimate ---- */

typedef struct means {
    float *d;    /* weight sums */
    float *s[3]; /* weighted colour sums */
} means;

static void means_fini(means *m)
{
    int c;
    free(m->d);
    for (c = 0; c < 3; c++) {
        free(m->s[c]);
    }
}

/* Normalised convolution of the known colours over radius r. */
static pc_status means_of(const pc_algo_rgb_image *img, const float *wt, size_t w, size_t h, size_t r,
                          float *tmp, means *out, pc_error *err)
{
    size_t n = w * h, i;
    int c;
    pc_status st;
    memset(out, 0, sizeof *out);
    for (c = 0; c < 3; c++) {
        for (i = 0; i < n; i++) {
            tmp[i] = img->px[i][c] * wt[i];
        }
        if ((st = pc_algo_matting_box_mean(tmp, w, h, r, &out->s[c], err)) != PC_OK) {
            means_fini(out);
            return st;
        }
    }
    if ((st = pc_algo_matting_box_mean(wt, w, h, r, &out->d, err)) != PC_OK) {
        means_fini(out);
    }
    return st;
}

static bool pick(const means *m, size_t i, float out[3])
{
    float d = m->d[i];
    int c;
    if (!(d >= 1e-4f)) {
        return false;
    }
    for (c = 0; c < 3; c++) {
        out[c] = m->s[c][i] / d;
    }
    return true;
}

pc_status pc_algo_matting_brush_estimate(const pc_algo_rgb_image *img, const float *m, const float *brush,
                                         size_t w, size_t h, size_t window, float **out_p, pc_error *err)
{
    size_t n, i, far_r;
    float *wf = NULL, *wb = NULL, *tmp = NULL, *out = NULL;
    means mm[4];
    int k, made = 0;
    pc_status st = PC_OK;
    if (!pc_size_mul(w, h, &n) || n > PC_MAX_ALLOC / sizeof(double)) {
        return pc_fail(err, PC_ERR_LIMIT, "mask too large");
    }
    if (img->w * img->h != n) {
        return pc_fail(err, PC_ERR_INVALID, "guide image %zux%zu for a %zux%zu mask", img->w, img->h, w, h);
    }
    memset(mm, 0, sizeof mm);
    wf = pc_malloc_array(n ? n : 1, sizeof(float));
    wb = pc_malloc_array(n ? n : 1, sizeof(float));
    tmp = pc_malloc_array(n ? n : 1, sizeof(float));
    out = pc_malloc_array(n ? n : 1, sizeof(float));
    if (wf == NULL || wb == NULL || tmp == NULL || out == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory estimating an edge");
        goto done;
    }
    /* Known: outside the painted area and (nearly) fully in or out of m. */
    for (i = 0; i < n; i++) {
        bool free_px = brush[i] <= 0.0f;
        wf[i] = free_px && m[i] >= 0.98f ? 1.0f : 0.0f;
        wb[i] = free_px && m[i] <= 0.02f ? 1.0f : 0.0f;
    }
    far_r = window > SIZE_MAX / 4 ? SIZE_MAX : window * 4;
    for (k = 0; k < 4; k++) {
        if ((st = means_of(img, k % 2 == 0 ? wf : wb, w, h, k < 2 ? window : far_r, tmp, &mm[k], err)) !=
            PC_OK) {
            goto done;
        }
        made++;
    }
    for (i = 0; i < n; i++) {
        float fm[3], bm[3], num = 0.0f, den = 0.0f;
        int c;
        out[i] = m[i];
        if (!(brush[i] > 0.0f)) {
            continue;
        }
        if (!(pick(&mm[0], i, fm) || pick(&mm[2], i, fm)) || !(pick(&mm[1], i, bm) || pick(&mm[3], i, bm))) {
            continue;
        }
        for (c = 0; c < 3; c++) {
            num += (img->px[i][c] - bm[c]) * (fm[c] - bm[c]);
            den += (fm[c] - bm[c]) * (fm[c] - bm[c]);
        }
        if (den > 1e-4f) {
            out[i] = pc_clampf(num / den, 0.0f, 1.0f);
        }
    }
    *out_p = out;
    out = NULL;
done:
    for (k = 0; k < made; k++) {
        means_fini(&mm[k]);
    }
    free(wf);
    free(wb);
    free(tmp);
    free(out);
    return st;
}
