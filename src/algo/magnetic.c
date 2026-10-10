/* Magnetic Lasso edge tracing (port of crates/algo/src/magnetic.rs). */
#include "algo/magnetic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Side of the cached pixel blocks (px). */
#define BLOCK 128
/* Blocks kept before the cache starts over (128^2 RGBA8 is 64 KiB, so at most 64 MiB). */
#define MAX_BLOCKS 1024
/* Longest stretch of the pointer's path searched at once (px). */
#define PIECE 256.0

/* Link cost weights: Mortensen & Barrett's 0.43 / 0.43 / 0.14, a cost per pixel of length, and
 * the pull towards the pointer's path. */
#define W_CENTRE 0.43f
#define W_STRENGTH 0.43f
#define W_DIRECTION 0.14f
#define W_LENGTH 0.02f
#define W_GUIDE 0.4f
/* The direction term where either pixel has no edge direction (the term's mean). */
#define NEUTRAL_DIRECTION (2.0f / 3.0f)
#define PI_F 3.14159265358979323846f
#define SQRT_2_F 1.41421356237309504880f

/* The 8 neighbours, in the order the search records them. */
static const int32_t DIRS[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};

typedef struct block {
    int32_t bx, by;
    uint8_t (*px)[4];
    size_t n;
} block;

struct pc_algo_magnetic_tracer {
    pc_rect bounds;
    block *blocks;
    size_t nblocks, cap;
};

/* A growable list of points. */
typedef struct pts {
    double (*p)[2];
    size_t n, cap;
} pts;

static bool pts_push(pts *v, const double q[2])
{
    if (v->n == v->cap) {
        void *g = pc_grow_array(v->p, &v->cap, v->n + 1, sizeof *v->p);
        if (g == NULL) {
            return false;
        }
        v->p = g;
    }
    v->p[v->n][0] = q[0];
    v->p[v->n][1] = q[1];
    v->n++;
    return true;
}

pc_algo_magnetic_settings pc_algo_magnetic_settings_default(void)
{
    pc_algo_magnetic_settings s = {10.0, 0.1f};
    return s;
}

pc_algo_magnetic_settings pc_algo_magnetic_settings_new(double width, float contrast)
{
    pc_algo_magnetic_settings d = pc_algo_magnetic_settings_default(), s;
    s.width =
        isfinite(width) ? pc_clampd(width, PC_ALGO_MAGNETIC_WIDTH_MIN, PC_ALGO_MAGNETIC_WIDTH_MAX) : d.width;
    s.contrast = isfinite(contrast) ? pc_clampf(contrast, 0.01f, 1.0f) : d.contrast;
    return s;
}

pc_status pc_algo_magnetic_tracer_new(pc_rect bounds, pc_algo_magnetic_tracer **out, pc_error *err)
{
    pc_algo_magnetic_tracer *t = calloc(1, sizeof *t);
    if (t == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a tracer");
    }
    t->bounds = bounds;
    *out = t;
    return PC_OK;
}

static void clear_blocks(pc_algo_magnetic_tracer *t)
{
    size_t i;
    for (i = 0; i < t->nblocks; i++) {
        free(t->blocks[i].px);
    }
    t->nblocks = 0;
}

void pc_algo_magnetic_tracer_free(pc_algo_magnetic_tracer *t)
{
    if (t == NULL) {
        return;
    }
    clear_blocks(t);
    free(t->blocks);
    free(t);
}

pc_rect pc_algo_magnetic_tracer_bounds(const pc_algo_magnetic_tracer *t)
{
    return t->bounds;
}

size_t pc_algo_magnetic_cached_pixels(const pc_algo_magnetic_tracer *t)
{
    size_t i, s = 0;
    for (i = 0; i < t->nblocks; i++) {
        s += t->blocks[i].n;
    }
    return s;
}

static int32_t sat_mul(int32_t a, int32_t b)
{
    int64_t r = (int64_t)a * b;
    return r > INT32_MAX ? INT32_MAX : (r < INT32_MIN ? INT32_MIN : (int32_t)r);
}

static int32_t div_euclid(int32_t a, int32_t b)
{
    int32_t q = a / b;
    if (a % b < 0) {
        q -= 1;
    }
    return q;
}

static double dist(const double a[2], const double b[2])
{
    return hypot(a[0] - b[0], a[1] - b[1]);
}

static double dist2_segment(const double p[2], const double a[2], const double b[2])
{
    double vx = b[0] - a[0], vy = b[1] - a[1], l2 = vx * vx + vy * vy, t, dx, dy;
    t = l2 > 0.0 ? pc_clampd(((p[0] - a[0]) * vx + (p[1] - a[1]) * vy) / l2, 0.0, 1.0) : 0.0;
    dx = p[0] - (a[0] + t * vx);
    dy = p[1] - (a[1] + t * vy);
    return dx * dx + dy * dy;
}

/* p kept inside the image; false if p is not finite or the image is empty. */
static bool clamp_point(const pc_algo_magnetic_tracer *t, const double p[2], double out[2])
{
    pc_rect b = t->bounds;
    if (pc_rect_is_empty(b) || !isfinite(p[0]) || !isfinite(p[1])) {
        return false;
    }
    out[0] = pc_clampd(p[0], (double)b.x0, (double)b.x1);
    out[1] = pc_clampd(p[1], (double)b.y0, (double)b.y1);
    return true;
}

static int32_t clamp_coord(double v, int32_t lo, int32_t hi)
{
    int32_t top = pc_i32_wrapping_sub(hi, 1);
    if (top < lo) {
        top = lo;
    }
    if (!isfinite(v)) {
        return lo;
    }
    return pc_clamp_i32(pc_f64_to_i32(floor(v)), lo, top);
}

/* The pixel under a point (clamped to the image). */
static void pixel_of(const pc_algo_magnetic_tracer *t, const double p[2], int32_t *x, int32_t *y)
{
    *x = clamp_coord(p[0], t->bounds.x0, t->bounds.x1);
    *y = clamp_coord(p[1], t->bounds.y0, t->bounds.y1);
}

/* Straight RGBA8 pixels of r (inside the image) through the block cache; out is w * h. */
static pc_status read_pixels(pc_algo_magnetic_tracer *t, pc_algo_magnetic_fetch fetch, void *ctx, pc_rect r,
                             uint8_t (*out)[4], pc_error *err)
{
    size_t w = pc_rect_width(r);
    int32_t bx0, by0, bx1, by1, bx, by;
    if (pc_rect_is_empty(r)) {
        return PC_OK;
    }
    if (t->nblocks > MAX_BLOCKS) {
        clear_blocks(t);
    }
    bx0 = div_euclid(r.x0, BLOCK);
    by0 = div_euclid(r.y0, BLOCK);
    bx1 = div_euclid(r.x1 - 1, BLOCK);
    by1 = div_euclid(r.y1 - 1, BLOCK);
    for (by = by0; by <= by1; by++) {
        for (bx = bx0; bx <= bx1; bx++) {
            pc_rect br = pc_rect_intersect(pc_rect_make(sat_mul(bx, BLOCK), sat_mul(by, BLOCK),
                                                        sat_mul(pc_i32_sat_add(bx, 1), BLOCK),
                                                        sat_mul(pc_i32_sat_add(by, 1), BLOCK)),
                                           t->bounds);
            block *b = NULL;
            size_t i, n, len, bw;
            pc_rect ov;
            int32_t y;
            if (pc_rect_is_empty(br)) {
                continue;
            }
            bw = pc_rect_width(br);
            n = bw * pc_rect_height(br);
            for (i = 0; i < t->nblocks; i++) {
                if (t->blocks[i].bx == bx && t->blocks[i].by == by) {
                    b = &t->blocks[i];
                    break;
                }
            }
            if (b == NULL) {
                uint8_t (*px)[4] = pc_calloc_array(n, 4);
                if (px == NULL) {
                    return pc_fail(err, PC_ERR_NOMEM, "out of memory reading pixels");
                }
                if (!fetch(ctx, br, px, n)) {
                    memset(px, 0, n * 4);
                }
                if (t->nblocks == t->cap) {
                    void *g = pc_grow_array(t->blocks, &t->cap, t->nblocks + 1, sizeof *t->blocks);
                    if (g == NULL) {
                        free(px);
                        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading pixels");
                    }
                    t->blocks = g;
                }
                b = &t->blocks[t->nblocks++];
                b->bx = bx;
                b->by = by;
                b->px = px;
                b->n = n;
            }
            ov = pc_rect_intersect(br, r);
            len = pc_rect_width(ov);
            for (y = ov.y0; y < ov.y1; y++) {
                size_t src = (size_t)(y - br.y0) * bw + (size_t)(ov.x0 - br.x0);
                size_t dst = (size_t)(y - r.y0) * w + (size_t)(ov.x0 - r.x0);
                memcpy(out[dst], b->px[src], len * 4);
            }
        }
    }
    return PC_OK;
}

/* Edge features of the pixels in an area. */
typedef struct field {
    pc_rect area;
    size_t w, n;
    float *strength;    /* steepest channel's Sobel gradient / 4 (capped at 1) */
    float (*grad)[2];   /* unit gradient of that channel (0 on flat pixels) */
    bool *ridge;        /* strongest across its edge (non-maximum suppression) */
    float (*offset)[2]; /* pixel centre -> the edge's sub-pixel centre */
} field;

static void field_fini(field *f)
{
    free(f->strength);
    free(f->grad);
    free(f->ridge);
    free(f->offset);
    memset(f, 0, sizeof *f);
}

static float signum_i(float v)
{
    if (isnan(v)) {
        return 0.0f;
    }
    return signbit(v) ? -1.0f : 1.0f;
}

/* Features over area from the straight RGBA8 pixels px of ext (area plus up to two pixels). */
static pc_status field_new(pc_rect area, pc_rect ext, const uint8_t (*px)[4], field *f, pc_error *err)
{
    int32_t ew = (int32_t)pc_rect_width(ext), eh = (int32_t)pc_rect_height(ext), x, y;
    size_t en = (size_t)ew * (size_t)eh, i, gw, gn;
    pc_rect g1 = pc_rect_intersect(pc_rect_inflate(area, 1), ext);
    float (*ch)[4] = pc_malloc_array(en ? en : 1, sizeof *ch);
    float *st;
    float (*gd)[2];
    memset(f, 0, sizeof *f);
    gw = pc_rect_width(g1);
    gn = gw * pc_rect_height(g1);
    st = pc_calloc_array(gn ? gn : 1, sizeof(float));
    gd = pc_calloc_array(gn ? gn : 1, sizeof *gd);
    f->area = area;
    f->w = pc_rect_width(area);
    f->n = f->w * pc_rect_height(area);
    f->strength = pc_calloc_array(f->n ? f->n : 1, sizeof(float));
    f->grad = pc_calloc_array(f->n ? f->n : 1, sizeof *f->grad);
    f->ridge = pc_calloc_array(f->n ? f->n : 1, sizeof(bool));
    f->offset = pc_calloc_array(f->n ? f->n : 1, sizeof *f->offset);
    if (ch == NULL || st == NULL || gd == NULL || f->strength == NULL || f->grad == NULL ||
        f->ridge == NULL || f->offset == NULL) {
        free(ch);
        free(st);
        free(gd);
        field_fini(f);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for edge features");
    }
    /* Premultiplied channels: an opaque shape on transparency has an edge, transparent pixels of
     * different colours do not. */
    for (i = 0; i < en; i++) {
        float a = (float)px[i][3] / 255.0f;
        ch[i][0] = (float)px[i][0] / 255.0f * a;
        ch[i][1] = (float)px[i][1] / 255.0f * a;
        ch[i][2] = (float)px[i][2] / 255.0f * a;
        ch[i][3] = a;
    }
#define AT(xx, yy)                                                                                           \
    (ch[(size_t)pc_clamp_i32((yy) - ext.y0, 0, eh - 1 > 0 ? eh - 1 : 0) * (size_t)(ew > 0 ? ew : 0) +        \
        (size_t)pc_clamp_i32((xx) - ext.x0, 0, ew - 1 > 0 ? ew - 1 : 0)])
    /* Gradients over the area plus one ring (non-maximum suppression looks across it). */
    for (y = g1.y0; y < g1.y1; y++) {
        for (x = g1.x0; x < g1.x1; x++) {
            const float *nw = AT(x - 1, y - 1), *nn = AT(x, y - 1), *ne = AT(x + 1, y - 1);
            const float *ww = AT(x - 1, y), *ee = AT(x + 1, y);
            const float *sw = AT(x - 1, y + 1), *ss = AT(x, y + 1), *se = AT(x + 1, y + 1);
            float best = 0.0f, bg[2] = {0.0f, 0.0f}, mag;
            int c;
            size_t k = (size_t)(y - g1.y0) * gw + (size_t)(x - g1.x0);
            for (c = 0; c < 4; c++) {
                float gx = (ne[c] + 2.0f * ee[c] + se[c]) - (nw[c] + 2.0f * ww[c] + sw[c]);
                float gy = (sw[c] + 2.0f * ss[c] + se[c]) - (nw[c] + 2.0f * nn[c] + ne[c]);
                float m2 = gx * gx + gy * gy;
                if (m2 > best) {
                    best = m2;
                    bg[0] = gx;
                    bg[1] = gy;
                }
            }
            mag = sqrtf(best);
            st[k] = pc_rs_minf_k(mag / 4.0f, 1.0f);
            if (mag > 1e-6f) {
                gd[k][0] = bg[0] / mag;
                gd[k][1] = bg[1] / mag;
            }
        }
    }
#undef AT
    free(ch);
#define S_AT(xx, yy)                                                                                         \
    (pc_rect_contains(g1, (xx), (yy)) ? st[(size_t)((yy) - g1.y0) * gw + (size_t)((xx) - g1.x0)] : 0.0f)
    for (y = area.y0; y < area.y1; y++) {
        for (x = area.x0; x < area.x1; x++) {
            float s0 = S_AT(x, y), g[2] = {0.0f, 0.0f}, sm, sp, gx, gy;
            int32_t dx, dy;
            size_t k = (size_t)(y - area.y0) * f->w + (size_t)(x - area.x0);
            if (pc_rect_contains(g1, x, y)) {
                size_t j = (size_t)(y - g1.y0) * gw + (size_t)(x - g1.x0);
                g[0] = gd[j][0];
                g[1] = gd[j][1];
            }
            f->strength[k] = s0;
            f->grad[k][0] = g[0];
            f->grad[k][1] = g[1];
            if (g[0] == 0.0f && g[1] == 0.0f) {
                continue;
            }
            /* The neighbours across the edge: the gradient rounded to one of 8 directions. */
            gx = g[0];
            gy = g[1];
            dx = fabsf(gx) >= 0.4142f * fabsf(gy) ? (int32_t)signum_i(gx) : 0;
            dy = fabsf(gy) >= 0.4142f * fabsf(gx) ? (int32_t)signum_i(gy) : 0;
            sm = S_AT(x - dx, y - dy);
            sp = S_AT(x + dx, y + dy);
            if (s0 > 0.0f && s0 >= sm && s0 >= sp) {
                /* The peak of the parabola through the three strengths. */
                float denom = sm - 2.0f * s0 + sp;
                float t = denom < -1e-6f ? pc_clampf(0.5f * (sm - sp) / denom, -0.5f, 0.5f) : 0.0f;
                f->ridge[k] = true;
                f->offset[k][0] = t * (float)dx;
                f->offset[k][1] = t * (float)dy;
            }
        }
    }
#undef S_AT
    free(st);
    free(gd);
    return PC_OK;
}

static bool field_index(const field *f, int32_t x, int32_t y, size_t *i)
{
    if (!pc_rect_contains(f->area, x, y)) {
        return false;
    }
    *i = (size_t)(y - f->area.y0) * f->w + (size_t)(x - f->area.x0);
    return true;
}

/* Where the border runs through pixel i at (x, y). */
static void field_point(const field *f, size_t i, int32_t x, int32_t y, float contrast, double out[2])
{
    bool on_edge = i < f->n && f->ridge[i] && f->strength[i] >= contrast;
    float o0 = on_edge ? f->offset[i][0] : 0.0f, o1 = on_edge ? f->offset[i][1] : 0.0f;
    out[0] = (double)x + 0.5 + (double)o0;
    out[1] = (double)y + 0.5 + (double)o1;
}

static void field_point_at(const field *f, size_t i, float contrast, double out[2])
{
    size_t w = f->w > 0 ? f->w : 1;
    field_point(f, i, (int32_t)(i % w) + f->area.x0, (int32_t)(i / w) + f->area.y0, contrast, out);
}

/* Edge features over area (inside the image). */
static pc_status make_field(pc_algo_magnetic_tracer *t, pc_algo_magnetic_fetch fetch, void *ctx, pc_rect area,
                            field *f, pc_error *err)
{
    /* Sobel reads one ring beyond the gradient, non-maximum suppression one beyond the area. */
    pc_rect ext = pc_rect_intersect(pc_rect_inflate(area, 2), t->bounds);
    size_t n = (size_t)pc_rect_width(ext) * pc_rect_height(ext);
    uint8_t (*px)[4] = pc_calloc_array(n ? n : 1, 4);
    pc_status st;
    if (px == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading pixels");
    }
    st = read_pixels(t, fetch, ctx, ext, px, err);
    if (st == PC_OK) {
        st = field_new(area, ext, (const uint8_t (*)[4])px, f, err);
    }
    free(px);
    return st;
}

pc_status pc_algo_magnetic_snap(pc_algo_magnetic_tracer *t, pc_algo_magnetic_fetch fetch, void *ctx,
                                const double p_in[2], pc_algo_magnetic_settings s_in, double out[2],
                                bool *some, pc_error *err)
{
    pc_algo_magnetic_settings s = pc_algo_magnetic_settings_new(s_in.width, s_in.contrast);
    double p[2], r = s.width, best_q[2] = {0.0, 0.0};
    int32_t px, py, ri, x, y;
    float best = 0.0f;
    bool have = false;
    pc_rect area;
    field f;
    pc_status st;
    if (!clamp_point(t, p_in, p)) {
        *some = false;
        return PC_OK;
    }
    pixel_of(t, p, &px, &py);
    ri = pc_i32_wrapping_add(pc_f64_to_i32(ceil(r)), 1);
    area = pc_rect_intersect(pc_rect_make(pc_i32_sat_sub(px, ri), pc_i32_sat_sub(py, ri),
                                          pc_i32_sat_add(px, pc_i32_wrapping_add(ri, 1)),
                                          pc_i32_sat_add(py, pc_i32_wrapping_add(ri, 1))),
                             t->bounds);
    st = make_field(t, fetch, ctx, area, &f, err);
    if (st != PC_OK) {
        return st;
    }
    for (y = area.y0; y < area.y1; y++) {
        for (x = area.x0; x < area.x1; x++) {
            size_t i;
            float strength, score;
            double c[2], d;
            if (!field_index(&f, x, y, &i)) {
                continue;
            }
            strength = f.strength[i];
            if (strength < s.contrast || !f.ridge[i]) {
                continue;
            }
            c[0] = (double)x + 0.5;
            c[1] = (double)y + 0.5;
            d = dist(c, p);
            if (d > r) {
                continue;
            }
            /* The strongest edge, a little in favour of the nearer one. */
            score = strength * (1.0f - 0.25f * pc_f64_to_f32(d / r));
            if (!have || score > best) {
                have = true;
                best = score;
                field_point(&f, i, x, y, s.contrast, best_q);
            }
        }
    }
    field_fini(&f);
    if (have) {
        out[0] = best_q[0];
        out[1] = best_q[1];
    } else {
        out[0] = p[0];
        out[1] = p[1];
    }
    *some = true;
    return PC_OK;
}

/* Squared distance from each pixel centre of area to the polyline guide, infinite beyond rad. */
static pc_status corridor(pc_rect area, const double (*guide)[2], size_t ng, double rad, float **out,
                          pc_error *err)
{
    size_t w = pc_rect_width(area), n = w * pc_rect_height(area), i, k, nseg;
    double r2 = rad * rad;
    float *near = pc_malloc_array(n ? n : 1, sizeof(float));
    if (near == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the corridor");
    }
    for (i = 0; i < n; i++) {
        near[i] = INFINITY;
    }
    nseg = ng == 1 ? 1 : (ng >= 2 ? ng - 1 : 0);
    for (k = 0; k < nseg; k++) {
        const double *a = guide[k], *b = ng == 1 ? guide[0] : guide[k + 1];
        pc_rect r = pc_rect_intersect(
            pc_rect_make(pc_f64_to_i32(floor(pc_rs_min(a[0], b[0]) - rad)),
                         pc_f64_to_i32(floor(pc_rs_min(a[1], b[1]) - rad)),
                         pc_i32_wrapping_add(pc_f64_to_i32(ceil(pc_rs_max(a[0], b[0]) + rad)), 1),
                         pc_i32_wrapping_add(pc_f64_to_i32(ceil(pc_rs_max(a[1], b[1]) + rad)), 1)),
            area);
        int32_t x, y;
        for (y = r.y0; y < r.y1; y++) {
            for (x = r.x0; x < r.x1; x++) {
                double c[2] = {(double)x + 0.5, (double)y + 0.5};
                double d2 = dist2_segment(c, a, b);
                if (d2 <= r2) {
                    size_t j = (size_t)(y - area.y0) * w + (size_t)(x - area.x0);
                    near[j] = pc_rs_minf(near[j], (float)d2);
                }
            }
        }
    }
    *out = near;
    return PC_OK;
}

/* The gradient-direction term of a link (Mortensen & Barrett's f_D). */
static float direction_cost(const float tp[2], const float tq[2], int32_t dx, int32_t dy, float len)
{
    float l[2], dp, dq;
    if ((tp[0] == 0.0f && tp[1] == 0.0f) || (tq[0] == 0.0f && tq[1] == 0.0f)) {
        return NEUTRAL_DIRECTION;
    }
    l[0] = (float)dx / len;
    l[1] = (float)dy / len;
    if (tp[0] * l[0] + tp[1] * l[1] < 0.0f) {
        l[0] = -l[0];
        l[1] = -l[1];
    }
    dp = pc_clampf(tp[0] * l[0] + tp[1] * l[1], -1.0f, 1.0f);
    dq = pc_clampf(l[0] * tq[0] + l[1] * tq[1], -1.0f, 1.0f);
    return 2.0f / (3.0f * PI_F) * (acosf(dp) + acosf(dq));
}

/* A binary heap ordered exactly like Rust's BinaryHeap<Reverse<(u32, usize)>> (a min-heap on
 * (bits, index)), sift for sift, so ties pop in the same order. */
typedef struct hitem {
    uint32_t bits;
    size_t i;
} hitem;

typedef struct heap {
    hitem *d;
    size_t n, cap;
} heap;

/* Rust's `a <= b` for Reverse((bits, i)). */
static bool h_le(hitem a, hitem b)
{
    return b.bits < a.bits || (b.bits == a.bits && b.i <= a.i);
}

static size_t h_sift_up(heap *h, size_t start, size_t pos)
{
    hitem e = h->d[pos];
    while (pos > start) {
        size_t parent = (pos - 1) / 2;
        if (h_le(e, h->d[parent])) {
            break;
        }
        h->d[pos] = h->d[parent];
        pos = parent;
    }
    h->d[pos] = e;
    return pos;
}

static bool h_push(heap *h, hitem x)
{
    if (h->n == h->cap) {
        void *g = pc_grow_array(h->d, &h->cap, h->n + 1, sizeof *h->d);
        if (g == NULL) {
            return false;
        }
        h->d = g;
    }
    h->d[h->n++] = x;
    (void)h_sift_up(h, 0, h->n - 1);
    return true;
}

static hitem h_pop(heap *h)
{
    hitem item = h->d[--h->n];
    if (h->n > 0) {
        size_t end = h->n, pos = 0, child = 1;
        hitem t = h->d[0];
        h->d[0] = item;
        item = t;
        {
            hitem e = h->d[0];
            while (child <= (end >= 2 ? end - 2 : 0) && end >= 2) {
                child += h_le(h->d[child], h->d[child + 1]) ? 1 : 0;
                h->d[pos] = h->d[child];
                pos = child;
                child = 2 * pos + 1;
            }
            if (child == end - 1) {
                h->d[pos] = h->d[child];
                pos = child;
            }
            h->d[pos] = e;
            (void)h_sift_up(h, 0, pos);
        }
    }
    return item;
}

static uint32_t f2bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static float bits2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* The cheapest 8-connected path from start to goal through the corridor `near`, as field indices
 * from start to goal; *found false when there is none. */
static pc_status search(const field *f, const float *near, float rad2, size_t start, size_t goal,
                        float contrast, size_t **cells_out, size_t *ncells, bool *found, pc_error *err)
{
    size_t w = f->w, h = w > 0 ? f->n / w : 0, n = w * h, i, *cells = NULL, nc = 0, cur;
    float *node = pc_malloc_array(n ? n : 1, sizeof(float));
    float (*tangent)[2] = pc_calloc_array(n ? n : 1, sizeof *tangent);
    float *distv = pc_malloc_array(n ? n : 1, sizeof(float));
    uint8_t *from = pc_malloc_array(n ? n : 1, 1);
    heap hp = {NULL, 0, 0};
    pc_status st = PC_OK;
    *found = false;
    if (node == NULL || tangent == NULL || distv == NULL || from == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for the edge search");
        goto cleanup;
    }
    /* Cost of entering each pixel, per pixel of length, and its edge tangent. */
    for (i = 0; i < n; i++) {
        float d2 = near[i], s, good, pull, centre;
        bool on_edge;
        node[i] = INFINITY;
        distv[i] = INFINITY;
        from[i] = UINT8_MAX;
        if (!isfinite(d2)) {
            continue;
        }
        s = f->strength[i];
        on_edge = s >= contrast && f->ridge[i];
        /* No pull below the contrast, full pull from twice the contrast, a little more for
         * stronger edges still. */
        good = pc_clampf((s - contrast) / pc_rs_maxf_k(contrast, 0.02f), 0.0f, 1.0f) *
               (0.6f + 0.4f * pc_rs_minf_k(s, 1.0f));
        /* Grows linearly away from the pointer's path, so the border hugs it without edges. */
        pull = pc_rs_minf_k(sqrtf(d2 / pc_rs_maxf_k(rad2, 1.0f)), 1.0f);
        centre = on_edge ? 0.0f : 1.0f;
        node[i] = W_CENTRE * centre + W_STRENGTH * (1.0f - good) + W_LENGTH + W_GUIDE * pull;
        if (s >= 0.5f * contrast) {
            tangent[i][0] = f->grad[i][1];
            tangent[i][1] = -f->grad[i][0];
        }
    }
    if (w == 0 || start >= n || goal >= n) {
        goto cleanup;
    }
    distv[start] = 0.0f;
    {
        hitem s0 = {f2bits(0.0f), start};
        if (!h_push(&hp, s0)) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for the edge search");
            goto cleanup;
        }
    }
    while (hp.n > 0) {
        hitem it = h_pop(&hp);
        float d = bits2f(it.bits);
        int32_t x, y;
        int k;
        const float *tp;
        i = it.i;
        if (d > distv[i]) {
            continue;
        }
        if (i == goal) {
            break;
        }
        x = (int32_t)(i % w);
        y = (int32_t)(i / w);
        tp = tangent[i];
        for (k = 0; k < 8; k++) {
            int32_t dx = DIRS[k][0], dy = DIRS[k][1], nx = x + dx, ny = y + dy;
            size_t j;
            float cost, len, nd;
            if (nx < 0 || ny < 0 || nx >= (int32_t)w || ny >= (int32_t)h) {
                continue;
            }
            j = (size_t)ny * w + (size_t)nx;
            cost = node[j];
            if (!isfinite(cost)) {
                continue;
            }
            len = dx != 0 && dy != 0 ? SQRT_2_F : 1.0f;
            nd = d + cost * len + W_DIRECTION * direction_cost(tp, tangent[j], dx, dy, len);
            if (nd < distv[j]) {
                hitem e;
                distv[j] = nd;
                from[j] = (uint8_t)k;
                e.bits = f2bits(nd);
                e.i = j;
                if (!h_push(&hp, e)) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory for the edge search");
                    goto cleanup;
                }
            }
        }
    }
    if (!isfinite(distv[goal])) {
        goto cleanup;
    }
    cells = pc_malloc_array(n + 2, sizeof(size_t));
    if (cells == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for the edge search");
        goto cleanup;
    }
    cells[nc++] = goal;
    cur = goal;
    while (cur != start && nc <= n) {
        int32_t x, y;
        uint8_t k = from[cur];
        if (k >= 8) {
            goto cleanup;
        }
        x = (int32_t)(cur % w) - DIRS[k][0];
        y = (int32_t)(cur / w) - DIRS[k][1];
        if (x < 0 || y < 0) {
            goto cleanup;
        }
        cur = (size_t)y * w + (size_t)x;
        cells[nc++] = cur;
    }
    /* reverse */
    for (i = 0; i < nc / 2; i++) {
        size_t tmp = cells[i];
        cells[i] = cells[nc - 1 - i];
        cells[nc - 1 - i] = tmp;
    }
    *cells_out = cells;
    *ncells = nc;
    cells = NULL;
    *found = true;
cleanup:
    free(cells);
    free(node);
    free(tangent);
    free(distv);
    free(from);
    free(hp.d);
    return st;
}

/* One piece: the cheapest path from a to b in the corridor around guide, a and b inclusive,
 * appended to `out` without its first point. */
static pc_status trace_piece(pc_algo_magnetic_tracer *t, pc_algo_magnetic_fetch fetch, void *ctx,
                             const double a[2], const double b[2], const double (*guide)[2], size_t ng,
                             pc_algo_magnetic_settings s, pts *out, pc_error *err)
{
    double rad = s.width + 1.5;
    int32_t ax, ay, bx, by;
    pc_rect bbox, area;
    field f;
    float *near = NULL;
    size_t start, goal, *cells = NULL, nc = 0, k;
    bool found = false;
    pc_status st;
    pixel_of(t, a, &ax, &ay);
    pixel_of(t, b, &bx, &by);
    bbox =
        pc_rect_make(ax < bx ? ax : bx, ay < by ? ay : by, (ax > bx ? ax : bx) + 1, (ay > by ? ay : by) + 1);
    for (k = 0; k < ng; k++) {
        int32_t x, y;
        pixel_of(t, guide[k], &x, &y);
        bbox = pc_rect_union(bbox, pc_rect_make(x, y, x + 1, y + 1));
    }
    area =
        pc_rect_intersect(pc_rect_inflate(bbox, pc_i32_wrapping_add(pc_f64_to_i32(ceil(rad)), 1)), t->bounds);
    st = make_field(t, fetch, ctx, area, &f, err);
    if (st != PC_OK) {
        return st;
    }
    st = corridor(area, guide, ng, rad, &near, err);
    if (st != PC_OK) {
        field_fini(&f);
        return st;
    }
    if (field_index(&f, ax, ay, &start) && field_index(&f, bx, by, &goal)) {
        near[start] = 0.0f;
        near[goal] = 0.0f;
        st = search(&f, near, pc_f64_to_f32(rad * rad), start, goal, s.contrast, &cells, &nc, &found, err);
    }
    if (st == PC_OK) {
        if (!found || nc < 2) {
            if (!pts_push(out, b)) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory tracing an edge");
            }
        } else {
            for (k = 1; k < nc; k++) {
                double q[2];
                if (k == nc - 1) {
                    q[0] = b[0];
                    q[1] = b[1];
                } else {
                    field_point_at(&f, cells[k], s.contrast, q);
                }
                if (!pts_push(out, q)) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory tracing an edge");
                    break;
                }
            }
        }
    }
    free(cells);
    free(near);
    field_fini(&f);
    return st;
}

pc_status pc_algo_magnetic_simplify(const double (*p)[2], size_t n, double tol, double (**out)[2],
                                    size_t *nout, pc_error *err)
{
    bool *keep;
    size_t (*stack)[2], top = 0, i, k = 0;
    double tol2 = tol * tol;
    double (*o)[2] = pc_malloc_array(n ? n : 1, sizeof *o);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory simplifying a path");
    }
    if (n < 3) {
        if (n > 0) {
            memcpy(o, p, n * sizeof *o);
        }
        *out = o;
        *nout = n;
        return PC_OK;
    }
    keep = pc_calloc_array(n, sizeof(bool));
    /* Each split pushes two ranges and keeps one point, so at most 2n ranges are pending. */
    stack = pc_malloc_array(2 * n + 2, sizeof *stack);
    if (keep == NULL || stack == NULL) {
        free(keep);
        free(stack);
        free(o);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory simplifying a path");
    }
    keep[0] = true;
    keep[n - 1] = true;
    stack[top][0] = 0;
    stack[top][1] = n - 1;
    top++;
    while (top > 0) {
        size_t a = stack[top - 1][0], b = stack[top - 1][1], far_i = a;
        double far_d = 0.0;
        top--;
        if (b <= a + 1) {
            continue;
        }
        for (i = a + 1; i < b; i++) {
            double d2 = dist2_segment(p[i], p[a], p[b]);
            if (d2 > far_d) {
                far_d = d2;
                far_i = i;
            }
        }
        if (far_d > tol2) {
            keep[far_i] = true;
            stack[top][0] = a;
            stack[top][1] = far_i;
            top++;
            stack[top][0] = far_i;
            stack[top][1] = b;
            top++;
        }
    }
    for (i = 0; i < n; i++) {
        if (keep[i]) {
            o[k][0] = p[i][0];
            o[k][1] = p[i][1];
            k++;
        }
    }
    free(keep);
    free(stack);
    *out = o;
    *nout = k;
    return PC_OK;
}

double pc_algo_magnetic_length(const double (*p)[2], size_t n)
{
    double s = -0.0;
    size_t i;
    for (i = 0; i + 1 < n; i++) {
        s += dist(p[i], p[i + 1]);
    }
    return s;
}

pc_status pc_algo_magnetic_split(const double (*g)[2], size_t n, double piece, double (**pts_out)[2],
                                 size_t **starts_out, size_t *npieces, pc_error *err)
{
    pts all = {NULL, 0, 0};
    size_t *starts = NULL, ns = 0, scap = 0, i;
    double acc = 0.0;
    if (n == 0) {
        *pts_out = NULL;
        *starts_out = NULL;
        *npieces = 0;
        return PC_OK;
    }
#define START_PIECE()                                                                                        \
    do {                                                                                                     \
        if (ns == scap) {                                                                                    \
            void *gg = pc_grow_array(starts, &scap, ns + 2, sizeof *starts);                                 \
            if (gg == NULL) {                                                                                \
                goto nomem;                                                                                  \
            }                                                                                                \
            starts = gg;                                                                                     \
        }                                                                                                    \
        starts[ns++] = all.n;                                                                                \
    } while (0)
    START_PIECE();
    if (!pts_push(&all, g[0])) {
        goto nomem;
    }
    for (i = 0; i + 1 < n; i++) {
        double a[2] = {g[i][0], g[i][1]};
        const double *b = g[i + 1];
        double seg = dist(a, b);
        while (acc + seg > piece && seg > 0.0) {
            double tt = pc_clampd((piece - acc) / seg, 0.0, 1.0);
            double m[2];
            m[0] = a[0] + (b[0] - a[0]) * tt;
            m[1] = a[1] + (b[1] - a[1]) * tt;
            if (!pts_push(&all, m)) {
                goto nomem;
            }
            START_PIECE();
            if (!pts_push(&all, m)) {
                goto nomem;
            }
            a[0] = m[0];
            a[1] = m[1];
            seg = dist(a, b);
            acc = 0.0;
        }
        if (!pts_push(&all, b)) {
            goto nomem;
        }
        acc += seg;
    }
    START_PIECE();
#undef START_PIECE
    *pts_out = all.p;
    *starts_out = starts;
    *npieces = ns - 1;
    starts[ns - 1] = all.n;
    return PC_OK;
nomem:
    free(all.p);
    free(starts);
    return pc_fail(err, PC_ERR_NOMEM, "out of memory splitting a path");
}

pc_status pc_algo_magnetic_trace(pc_algo_magnetic_tracer *t, pc_algo_magnetic_fetch fetch, void *ctx,
                                 const double from_in[2], const double to_in[2], const double (*guide)[2],
                                 size_t nguide, pc_algo_magnetic_settings s_in, double (**out)[2],
                                 size_t *nout, pc_error *err)
{
    pc_algo_magnetic_settings s = pc_algo_magnetic_settings_new(s_in.width, s_in.contrast);
    double from[2], to[2], cur[2], (*g)[2] = NULL, (*pp)[2] = NULL;
    pts raw = {NULL, 0, 0}, res = {NULL, 0, 0};
    size_t ng = 0, *starts = NULL, npieces = 0, k;
    pc_status st;
    if (!clamp_point(t, from_in, from) || !clamp_point(t, to_in, to)) {
        *out = NULL;
        *nout = 0;
        return PC_OK;
    }
    if (!pts_push(&raw, from)) {
        goto nomem;
    }
    for (k = 0; k < nguide; k++) {
        double q[2];
        if (clamp_point(t, guide[k], q) && !pts_push(&raw, q)) {
            goto nomem;
        }
    }
    if (!pts_push(&raw, to)) {
        goto nomem;
    }
    /* A coarse guide is enough for the corridor, and much cheaper to draw. */
    st = pc_algo_magnetic_simplify((const double (*)[2])raw.p, raw.n, 0.5, &g, &ng, err);
    free(raw.p);
    raw.p = NULL;
    if (st != PC_OK) {
        return st;
    }
    if (pc_algo_magnetic_length((const double (*)[2])g, ng) > PC_ALGO_MAGNETIC_MAX_GUIDE_LENGTH) {
        *out = g;
        *nout = ng;
        return PC_OK;
    }
    st = pc_algo_magnetic_split((const double (*)[2])g, ng, PIECE, &pp, &starts, &npieces, err);
    free(g);
    if (st != PC_OK) {
        return st;
    }
    if (!pts_push(&res, from)) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory tracing an edge");
        goto done;
    }
    cur[0] = from[0];
    cur[1] = from[1];
    for (k = 0; k < npieces; k++) {
        const double (*piece)[2] = (const double (*)[2])(pp + starts[k]);
        size_t pn = starts[k + 1] - starts[k];
        double target[2] = {to[0], to[1]};
        if (k + 1 != npieces && pn > 0) {
            bool some = false;
            double q[2];
            st = pc_algo_magnetic_snap(t, fetch, ctx, piece[pn - 1], s, q, &some, err);
            if (st != PC_OK) {
                goto done;
            }
            if (some) {
                target[0] = q[0];
                target[1] = q[1];
            }
        }
        st = trace_piece(t, fetch, ctx, cur, target, piece, pn, s, &res, err);
        if (st != PC_OK) {
            goto done;
        }
        cur[0] = target[0];
        cur[1] = target[1];
    }
    /* Within half a pixel: drops the staircase of pixel steps, keeps the curve of an edge. */
    st = pc_algo_magnetic_simplify((const double (*)[2])res.p, res.n, 0.5, out, nout, err);
done:
    free(pp);
    free(starts);
    free(res.p);
    return st;
nomem:
    free(raw.p);
    return pc_fail(err, PC_ERR_NOMEM, "out of memory tracing an edge");
}
