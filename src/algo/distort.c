/* Distortions by inverse mapping + bilinear resampling, centred on the reference bounds: Twirl,
 * Pinch, Spherize, Wave, Ripple, Polar Coordinates (port of crates/algo/src/distort.rs) and
 * Displace, Shear, ZigZag (distort2.rs). */
#include <stdlib.h>
#include <string.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"
#include "base/mem.h"

/* A map from output pixel centre to source position. */
typedef void (*map_fn)(const void *m, float x, float y, float *sx, float *sy);

static void remap(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, pc_algo_edge edge, map_fn f,
                  const void *m, float *dst)
{
    size_t n = src->ch, k = 0;
    int32_t x, y;
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            float sx, sy;
            f(m, (float)x + 0.5f, (float)y + 0.5f, &sx, &sy);
            pc_algo_image_sample(src, sx, sy, edge, ctx->bounds, ctx->alpha, dst + k, n);
            k += n;
        }
    }
}

/* Centre of b and the radius of its inscribed circle. */
typedef struct centre {
    float cx, cy, rr;
} centre;

static centre centre_of(pc_rect b)
{
    float w = (float)pc_rect_width(b), h = (float)pc_rect_height(b);
    centre c = {(float)b.x0 + w / 2.0f, (float)b.y0 + h / 2.0f, pc_rs_minf(w, h) / 2.0f};
    return c;
}

/* ---- Twirl ---- */

typedef struct twirl_m {
    centre c;
    float a;
} twirl_m;

static void twirl_map(const void *vm, float x, float y, float *sx, float *sy)
{
    const twirl_m *m = vm;
    float dx = x - m->c.cx, dy = y - m->c.cy, r = sqrtf(dx * dx + dy * dy), t, s, co;
    if (r >= m->c.rr || m->c.rr <= 0.0f) {
        *sx = x, *sy = y;
        return;
    }
    t = m->a * (1.0f - r / m->c.rr);
    s = sinf(t), co = cosf(t);
    *sx = m->c.cx + dx * co - dy * s;
    *sy = m->c.cy + dx * s + dy * co;
}

static pc_status twirl_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    twirl_m m = {centre_of(ctx->bounds), pc_fx_rad(((const pc_algo_twirl_params *)p)->angle)};
    (void)prep, (void)err;
    remap(src, out, ctx, PC_ALGO_EDGE_REPEAT, twirl_map, &m, dst);
    return PC_OK;
}

/* ---- Pinch ---- */

typedef struct pinch_m {
    centre c;
    float a;
} pinch_m;

static void pinch_map(const void *vm, float x, float y, float *sx, float *sy)
{
    const pinch_m *m = vm;
    float dx = x - m->c.cx, dy = y - m->c.cy, r = sqrtf(dx * dx + dy * dy), rs;
    if (r >= m->c.rr || r <= 0.0f) {
        *sx = x, *sy = y;
        return;
    }
    rs = m->c.rr * powf(r / m->c.rr, 1.0f - m->a * 0.5f);
    *sx = m->c.cx + dx * rs / r;
    *sy = m->c.cy + dy * rs / r;
}

static pc_status pinch_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    pinch_m m = {centre_of(ctx->bounds),
                 pc_clampf(((const pc_algo_pinch_params *)p)->amount / 100.0f, -1.0f, 1.0f)};
    (void)prep, (void)err;
    remap(src, out, ctx, PC_ALGO_EDGE_REPEAT, pinch_map, &m, dst);
    return PC_OK;
}

/* ---- Spherize ---- */

/* rn in [0, 1): a positive amount magnifies the centre (samples closer in). */
static float sphere_1d(float rn, float a)
{
    float target = a >= 0.0f ? asinf(rn) * 2.0f / PC_FX_PI : sinf(rn * PC_FX_FRAC_PI_2);
    return rn + fabsf(a) * (target - rn);
}

typedef struct sphere_m {
    centre c;
    float hx, hy, a;
    pc_algo_spherize_mode mode;
} sphere_m;

static void sphere_map(const void *vm, float x, float y, float *sx, float *sy)
{
    const sphere_m *m = vm;
    float dx = x - m->c.cx, dy = y - m->c.cy;
    *sx = x, *sy = y;
    switch (m->mode) {
    case PC_ALGO_SPHERIZE_HORIZONTAL_ONLY:
        if (fabsf(dx) < m->hx) {
            *sx = m->c.cx + pc_fx_signum(dx) * m->hx * sphere_1d(fabsf(dx) / m->hx, m->a);
        }
        break;
    case PC_ALGO_SPHERIZE_VERTICAL_ONLY:
        if (fabsf(dy) < m->hy) {
            *sy = m->c.cy + pc_fx_signum(dy) * m->hy * sphere_1d(fabsf(dy) / m->hy, m->a);
        }
        break;
    default: {
        float r = sqrtf(dx * dx + dy * dy), rs;
        if (r >= m->c.rr || r <= 0.0f) {
            return;
        }
        rs = m->c.rr * sphere_1d(r / m->c.rr, m->a);
        *sx = m->c.cx + dx * rs / r;
        *sy = m->c.cy + dy * rs / r;
        break;
    }
    }
}

static pc_status spherize_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                            const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_spherize_params *s = p;
    pc_rect b = ctx->bounds;
    sphere_m m = {centre_of(b), (float)pc_rect_width(b) / 2.0f, (float)pc_rect_height(b) / 2.0f,
                  pc_clampf(s->amount / 100.0f, -1.0f, 1.0f), s->mode};
    (void)prep, (void)err;
    remap(src, out, ctx, PC_ALGO_EDGE_REPEAT, sphere_map, &m, dst);
    return PC_OK;
}

/* ---- Wave ---- */

#define MAX_GENERATORS 64

typedef struct wave_gen {
    float len, amp, p1, p2;
} wave_gen;

typedef struct wave_m {
    wave_gen g[MAX_GENERATORS];
    uint32_t n;
    pc_algo_wave_type type;
    float bx, by;
} wave_m;

static float wave_shape(pc_algo_wave_type t, float phase)
{
    float p = pc_algo_rem_euclidf(phase, PC_FX_TAU) / PC_FX_TAU;
    switch (t) {
    case PC_ALGO_WAVE_TRIANGLE:
        return 1.0f - 4.0f * fabsf(p - 0.5f);
    case PC_ALGO_WAVE_SQUARE:
        return p < 0.5f ? 1.0f : -1.0f;
    default:
        return sinf(phase);
    }
}

static void wave_map(const void *vm, float x, float y, float *sx, float *sy)
{
    const wave_m *m = vm;
    float lx = x - m->bx, ly = y - m->by, dx = 0.0f, dy = 0.0f;
    uint32_t i;
    for (i = 0; i < m->n; i++) {
        const wave_gen *g = &m->g[i];
        dx += g->amp * wave_shape(m->type, PC_FX_TAU * ly / g->len + g->p1);
        dy += g->amp * wave_shape(m->type, PC_FX_TAU * lx / g->len + g->p2);
    }
    *sx = x + dx / (float)m->n;
    *sy = y + dy / (float)m->n;
}

static float wave_lerp(float a, float b, float t)
{
    return a + pc_rs_maxf_k(b - a, 0.0f) * t;
}

static pc_status wave_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                        const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_wave_params *w = p;
    wave_m m;
    uint32_t i;
    (void)prep, (void)err;
    m.n = w->generators < 1 ? 1 : (w->generators > MAX_GENERATORS ? MAX_GENERATORS : w->generators);
    m.type = w->wave_type;
    m.bx = (float)ctx->bounds.x0;
    m.by = (float)ctx->bounds.y0;
    for (i = 0; i < m.n; i++) {
        int32_t ii = (int32_t)i;
        m.g[i].len = pc_rs_maxf_k(
            wave_lerp(w->wavelength_min, w->wavelength_max, pc_algo_hash01(ii, 1, 7, w->seed)), 1.0f);
        m.g[i].amp = wave_lerp(w->amplitude_min, w->amplitude_max, pc_algo_hash01(ii, 2, 7, w->seed));
        m.g[i].p1 = pc_algo_hash01(ii, 3, 7, w->seed) * PC_FX_TAU;
        m.g[i].p2 = pc_algo_hash01(ii, 4, 7, w->seed) * PC_FX_TAU;
    }
    remap(src, out, ctx, pc_fx_edge_of(w->undefined), wave_map, &m, dst);
    return PC_OK;
}

/* ---- Ripple ---- */

typedef struct ripple_m {
    float amp, len, bx, by;
} ripple_m;

static void ripple_map(const void *vm, float x, float y, float *sx, float *sy)
{
    const ripple_m *m = vm;
    float lx = x - m->bx, ly = y - m->by;
    *sx = x + m->amp * sinf(PC_FX_TAU * ly / m->len);
    *sy = y + m->amp * sinf(PC_FX_TAU * lx / m->len);
}

static pc_status ripple_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                          const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_ripple_params *r = p;
    ripple_m m;
    (void)prep, (void)err;
    m.len = r->size == PC_ALGO_RIPPLE_SMALL ? 8.0f : (r->size == PC_ALGO_RIPPLE_LARGE ? 32.0f : 16.0f);
    m.amp = r->amount / 100.0f * m.len / 8.0f;
    m.bx = (float)ctx->bounds.x0;
    m.by = (float)ctx->bounds.y0;
    remap(src, out, ctx, PC_ALGO_EDGE_REPEAT, ripple_map, &m, dst);
    return PC_OK;
}

/* ---- Polar Coordinates ---- */

typedef struct polar_m {
    float w, h, cx, cy, rmax, bx, by;
    pc_algo_polar_mode mode;
} polar_m;

static void polar_map(const void *vm, float x, float y, float *sx, float *sy)
{
    const polar_m *m = vm;
    if (m->mode == PC_ALGO_POLAR_POLAR_TO_RECT) {
        float theta = (x - m->bx) / m->w * PC_FX_TAU, r = (y - m->by) / m->h * m->rmax;
        *sx = m->cx + r * sinf(theta);
        *sy = m->cy - r * cosf(theta);
    } else {
        /* Output is polar: angle from 12 o'clock clockwise -> source x, radius -> source y. */
        float dx = x - m->cx, dy = y - m->cy;
        float theta = pc_algo_rem_euclidf(atan2f(dx, -dy), PC_FX_TAU), r = sqrtf(dx * dx + dy * dy);
        *sx = m->bx + theta / PC_FX_TAU * m->w;
        *sy = m->by + r / m->rmax * m->h;
    }
}

static pc_status polar_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    pc_rect b = ctx->bounds;
    uint32_t bw = pc_rect_width(b), bh = pc_rect_height(b);
    polar_m m;
    (void)prep, (void)err;
    m.w = (float)(bw > 1 ? bw : 1);
    m.h = (float)(bh > 1 ? bh : 1);
    m.bx = (float)b.x0;
    m.by = (float)b.y0;
    m.cx = m.bx + m.w / 2.0f;
    m.cy = m.by + m.h / 2.0f;
    m.rmax = pc_rs_minf(m.w, m.h) / 2.0f;
    m.mode = ((const pc_algo_polar_params *)p)->mode;
    remap(src, out, ctx, PC_ALGO_EDGE_TRANSPARENT, polar_map, &m, dst);
    return PC_OK;
}

/* ---- Displace (distort2.rs) ---- */

/* Max displacement at 100 % scale and full map contrast, px. */
#define DISPLACE_PX 128.0f

/* Bilinear read of channel c of a map at map-local coordinates (clamped). */
static float map_sample(const pc_algo_image *map, float u, float v, size_t c)
{
    pc_rect r = map->rect;
    int32_t w = (int32_t)pc_rect_width(r), h = (int32_t)pc_rect_height(r);
    float fx = u - 0.5f, fy = v - 0.5f, x0f = floorf(fx), y0f = floorf(fy), ax = fx - x0f, ay = fy - y0f;
    int32_t x0 = pc_f32_to_i32(x0f), y0 = pc_f32_to_i32(y0f), x1 = pc_i32_wrapping_add(x0, 1),
            y1 = pc_i32_wrapping_add(y0, 1);
    float g00, g10, g01, g11, top, bot;
#define G(xx, yy)                                                                                            \
    pc_algo_image_get(map, pc_i32_wrapping_add(r.x0, pc_clamp_i32((xx), 0, w - 1)),                          \
                      pc_i32_wrapping_add(r.y0, pc_clamp_i32((yy), 0, h - 1)), c)
    g00 = G(x0, y0), g10 = G(x1, y0), g01 = G(x0, y1), g11 = G(x1, y1);
#undef G
    top = g00 + (g10 - g00) * ax;
    bot = g01 + (g11 - g01) * ax;
    return top + (bot - top) * ay;
}

typedef struct displace_m {
    const pc_algo_image *map;
    float mw, mh, bw, bh, bx, by, sx, sy;
    size_t vc;
    bool stretch;
} displace_m;

static void displace_map(const void *vm, float x, float y, float *sx, float *sy)
{
    const displace_m *m = vm;
    float lx = x - m->bx, ly = y - m->by, u, v;
    if (m->stretch) {
        u = lx / m->bw * m->mw;
        v = ly / m->bh * m->mh;
    } else {
        u = pc_algo_rem_euclidf(lx, m->mw);
        v = pc_algo_rem_euclidf(ly, m->mh);
    }
    *sx = x + (map_sample(m->map, u, v, 0) - 0.5f) * 2.0f * m->sx;
    *sy = y + (map_sample(m->map, u, v, m->vc) - 0.5f) * 2.0f * m->sy;
}

static pc_status displace_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                            const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_displace_params *d = p;
    pc_rect b = ctx->bounds;
    uint32_t bw = pc_rect_width(b), bh = pc_rect_height(b);
    displace_m m;
    (void)prep, (void)err;
    if (d->map == NULL || pc_rect_is_empty(d->map->rect) || d->map->ch == 0 || d->map->data == NULL) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    m.map = d->map;
    m.mw = (float)pc_rect_width(d->map->rect);
    m.mh = (float)pc_rect_height(d->map->rect);
    m.bw = (float)(bw > 1 ? bw : 1);
    m.bh = (float)(bh > 1 ? bh : 1);
    m.bx = (float)b.x0;
    m.by = (float)b.y0;
    /* Colour channels of the map (alpha excluded when present: 2 = gray+alpha, 4 = RGBA...). */
    m.vc = d->map->ch >= 3 ? 1 : 0;
    m.sx = d->horizontal / 100.0f * DISPLACE_PX;
    m.sy = d->vertical / 100.0f * DISPLACE_PX;
    m.stretch = d->stretch;
    remap(src, out, ctx, pc_fx_edge_of(d->undefined), displace_map, &m, dst);
    return PC_OK;
}

/* ---- Shear ---- */

float pc_algo_distort2_curve_at(const float (*pts)[2], size_t n, float t)
{
    size_t last, i;
    float p0[2], p1[2], p2[2], p3[2], span, s, m1, m2, s2, s3;
    if (n == 0) {
        return 0.0f;
    }
    if (n == 1) {
        return pts[0][1];
    }
    if (t <= pts[0][0]) {
        return pts[0][1];
    }
    last = n - 1;
    if (t >= pts[last][0]) {
        return pts[last][1];
    }
    for (i = 0; i + 1 < n; i++) {
        if (t >= pts[i][0] && t <= pts[i + 1][0]) {
            break;
        }
    }
    if (i + 1 >= n) {
        i = 0;
    }
    memcpy(p1, pts[i], sizeof p1);
    memcpy(p2, pts[i + 1], sizeof p2);
    memcpy(p0, i > 0 ? pts[i - 1] : pts[i], sizeof p0);
    memcpy(p3, i + 2 <= last ? pts[i + 2] : pts[i + 1], sizeof p3);
    span = pc_rs_maxf_k(p2[0] - p1[0], 1e-6f);
    s = (t - p1[0]) / span;
    /* Tangents scaled to the span (non-uniform Catmull-Rom). */
    m1 = i > 0 ? (p2[1] - p0[1]) / pc_rs_maxf_k(p2[0] - p0[0], 1e-6f) * span : p2[1] - p1[1];
    m2 = i + 2 <= last ? (p3[1] - p1[1]) / pc_rs_maxf_k(p3[0] - p1[0], 1e-6f) * span : p2[1] - p1[1];
    s2 = s * s;
    s3 = s * s * s;
    return (2.0f * s3 - 3.0f * s2 + 1.0f) * p1[1] + (s3 - 2.0f * s2 + s) * m1 +
           (-2.0f * s3 + 3.0f * s2) * p2[1] + (s3 - s2) * m2;
}

/* f32::total_cmp as a signed key. */
static int32_t total_key(float v)
{
    uint32_t u;
    int32_t i;
    memcpy(&u, &v, sizeof u);
    i = pc_i32_from_bits(u);
    return i ^ (int32_t)((uint32_t)(i >> 31) >> 1);
}

typedef struct shear_pt {
    float v[2];
    size_t idx;
} shear_pt;

/* sort_by(total_cmp) is stable: ties keep their input order. */
static int cmp_shear(const void *a, const void *b)
{
    const shear_pt *x = a, *y = b;
    int32_t kx = total_key(x->v[0]), ky = total_key(y->v[0]);
    if (kx != ky) {
        return kx < ky ? -1 : 1;
    }
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

typedef struct shear_m {
    const float *rows;
    size_t nrows;
    int32_t oy0;
} shear_m;

static void shear_map(const void *vm, float x, float y, float *sx, float *sy)
{
    const shear_m *m = vm;
    int32_t i = pc_i32_wrapping_sub(pc_f32_to_i32(y - 0.5f), m->oy0);
    i = pc_clamp_i32(i, 0, (int32_t)m->nrows - 1);
    *sx = x - m->rows[i];
    *sy = y;
}

static pc_status shear_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_shear_params *s = p;
    pc_rect b = ctx->bounds;
    uint32_t bwu = pc_rect_width(b), bhu = pc_rect_height(b);
    float bw = (float)(bwu > 1 ? bwu : 1), bh = (float)(bhu > 1 ? bhu : 1);
    size_t np = s->points.n, nd = 0, i, nrows = pc_rect_height(out);
    shear_pt *sp = NULL;
    float (*pts)[2] = NULL, *rows = NULL;
    shear_m m;
    (void)prep;
    if (nrows == 0 || pc_rect_width(out) == 0) {
        return PC_OK;
    }
    if (np > 0) {
        sp = pc_malloc_array(np, sizeof *sp);
        pts = pc_malloc_array(np, sizeof *pts);
    }
    rows = pc_malloc_array(nrows, sizeof *rows);
    if ((np > 0 && (sp == NULL || pts == NULL)) || rows == NULL) {
        free(sp), free(pts), free(rows);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Shear");
    }
    for (i = 0; i < np; i++) {
        sp[i].v[0] = pc_clampf(s->points.v[i][0], 0.0f, 1.0f);
        sp[i].v[1] = pc_clampf(s->points.v[i][1], -1.0f, 1.0f);
        sp[i].idx = i;
    }
    if (np > 1) {
        qsort(sp, np, sizeof *sp, cmp_shear);
    }
    /* dedup_by keeps the first of each run of near-equal t. */
    for (i = 0; i < np; i++) {
        if (nd > 0 && fabsf(sp[i].v[0] - pts[nd - 1][0]) < 1e-6f) {
            continue;
        }
        pts[nd][0] = sp[i].v[0];
        pts[nd][1] = sp[i].v[1];
        nd++;
    }
    /* Per-row offsets are shared by every pixel of the row. */
    for (i = 0; i < nrows; i++) {
        float t = ((float)((int64_t)out.y0 + (int64_t)i) + 0.5f - (float)b.y0) / bh;
        rows[i] = pc_algo_distort2_curve_at((const float (*)[2])pts, nd, t) * bw / 2.0f;
    }
    m.rows = rows;
    m.nrows = nrows;
    m.oy0 = out.y0;
    remap(src, out, ctx, pc_fx_edge_of(s->undefined), shear_map, &m, dst);
    free(sp), free(pts), free(rows);
    return PC_OK;
}

/* ---- ZigZag ---- */

typedef struct zigzag_m {
    centre c;
    float a, k;
    pc_algo_zigzag_style style;
} zigzag_m;

static void zigzag_map(const void *vm, float x, float y, float *sx, float *sy)
{
    const zigzag_m *m = vm;
    float dx = x - m->c.cx, dy = y - m->c.cy, r = sqrtf(dx * dx + dy * dy), rn, wave, radial, angular, th, rs;
    float a = m->a, k = m->k, rr = m->c.rr;
    if (r >= rr || r <= 0.0f || a == 0.0f) {
        *sx = x, *sy = y;
        return;
    }
    rn = r / rr;
    wave = sinf(PC_FX_TAU * k * rn) * (1.0f - rn);
    switch (m->style) {
    case PC_ALGO_ZIGZAG_OUT_FROM_CENTER:
        radial = a * wave * rr / (4.0f * k);
        angular = 0.0f;
        break;
    case PC_ALGO_ZIGZAG_AROUND_CENTER:
        radial = 0.0f;
        angular = a * wave * PC_FX_PI / (2.0f * k);
        break;
    default:
        radial = a * wave * rr / (8.0f * k);
        angular = a * wave * PC_FX_PI / (4.0f * k);
        break;
    }
    th = atan2f(dy, dx) + angular;
    rs = pc_rs_maxf_k(r + radial, 0.0f);
    *sx = m->c.cx + rs * cosf(th);
    *sy = m->c.cy + rs * sinf(th);
}

static pc_status zigzag_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                          const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_zigzag_params *z = p;
    zigzag_m m = {centre_of(ctx->bounds), pc_clampf(z->amount / 100.0f, -1.0f, 1.0f),
                  pc_rs_maxf_k(pc_clampf(z->ridges, 0.0f, 20.0f), 0.5f), z->style};
    (void)prep, (void)err;
    remap(src, out, ctx, PC_ALGO_EDGE_REPEAT, zigzag_map, &m, dst);
    return PC_OK;
}

/* ---- defs ---- */

static const pc_algo_jfield TWIRL_F[] = {PC_ALGO_JF("angle", pc_algo_twirl_params, angle, PC_ALGO_JF_F32)};
static const pc_algo_jfield PINCH_F[] = {PC_ALGO_JF("amount", pc_algo_pinch_params, amount, PC_ALGO_JF_F32)};
static const pc_algo_jfield SPHERIZE_F[] = {
    PC_ALGO_JF("amount", pc_algo_spherize_params, amount, PC_ALGO_JF_F32),
    PC_ALGO_JFE("mode", pc_algo_spherize_params, mode, PC_ALGO_ENUM_SPHERIZE_MODE, false),
};
static const pc_algo_jfield WAVE_F[] = {
    PC_ALGO_JF("generators", pc_algo_wave_params, generators, PC_ALGO_JF_U32),
    PC_ALGO_JF("wavelengthMin", pc_algo_wave_params, wavelength_min, PC_ALGO_JF_F32),
    PC_ALGO_JF("wavelengthMax", pc_algo_wave_params, wavelength_max, PC_ALGO_JF_F32),
    PC_ALGO_JF("amplitudeMin", pc_algo_wave_params, amplitude_min, PC_ALGO_JF_F32),
    PC_ALGO_JF("amplitudeMax", pc_algo_wave_params, amplitude_max, PC_ALGO_JF_F32),
    PC_ALGO_JFE("waveType", pc_algo_wave_params, wave_type, PC_ALGO_ENUM_WAVE_TYPE, false),
    PC_ALGO_JFE("undefined", pc_algo_wave_params, undefined, PC_ALGO_ENUM_UNDEFINED_AREAS, false),
    PC_ALGO_JF("seed", pc_algo_wave_params, seed, PC_ALGO_JF_U32),
};
static const pc_algo_jfield RIPPLE_F[] = {
    PC_ALGO_JF("amount", pc_algo_ripple_params, amount, PC_ALGO_JF_F32),
    PC_ALGO_JFE("size", pc_algo_ripple_params, size, PC_ALGO_ENUM_RIPPLE_SIZE, false),
};
static const pc_algo_jfield POLAR_F[] = {
    PC_ALGO_JFE("mode", pc_algo_polar_params, mode, PC_ALGO_ENUM_POLAR_MODE, false),
};
static const pc_algo_jfield DISPLACE_F[] = {
    PC_ALGO_JF("horizontal", pc_algo_displace_params, horizontal, PC_ALGO_JF_F32),
    PC_ALGO_JF("vertical", pc_algo_displace_params, vertical, PC_ALGO_JF_F32),
    PC_ALGO_JF("stretch", pc_algo_displace_params, stretch, PC_ALGO_JF_BOOL),
    PC_ALGO_JFE("undefined", pc_algo_displace_params, undefined, PC_ALGO_ENUM_UNDEFINED_AREAS, false),
    PC_ALGO_JFSKIP("map", pc_algo_displace_params, map),
};
static const pc_algo_jfield SHEAR_F[] = {
    PC_ALGO_JF("points", pc_algo_shear_params, points, PC_ALGO_JF_POINTS),
    PC_ALGO_JFE("undefined", pc_algo_shear_params, undefined, PC_ALGO_ENUM_UNDEFINED_AREAS, false),
};
static const pc_algo_jfield ZIGZAG_F[] = {
    PC_ALGO_JF("amount", pc_algo_zigzag_params, amount, PC_ALGO_JF_F32),
    PC_ALGO_JF("ridges", pc_algo_zigzag_params, ridges, PC_ALGO_JF_F32),
    PC_ALGO_JFE("style", pc_algo_zigzag_params, style, PC_ALGO_ENUM_ZIGZAG_STYLE, false),
};

PC_FX_DEF(pc_algo_def_twirl, "twirl", pc_algo_twirl_params, TWIRL_F, NULL, pc_fx_halo_bounds, twirl_k);
PC_FX_DEF(pc_algo_def_pinch, "pinch", pc_algo_pinch_params, PINCH_F, NULL, pc_fx_halo_bounds, pinch_k);
PC_FX_DEF(pc_algo_def_spherize, "spherize", pc_algo_spherize_params, SPHERIZE_F, NULL, pc_fx_halo_bounds,
          spherize_k);
PC_FX_DEF(pc_algo_def_wave, "wave", pc_algo_wave_params, WAVE_F, NULL, pc_fx_halo_bounds, wave_k);
PC_FX_DEF(pc_algo_def_ripple, "ripple", pc_algo_ripple_params, RIPPLE_F, NULL, pc_fx_halo_bounds, ripple_k);
PC_FX_DEF(pc_algo_def_polar, "polarCoordinates", pc_algo_polar_params, POLAR_F, NULL, pc_fx_halo_bounds,
          polar_k);
PC_FX_DEF(pc_algo_def_displace, "displace", pc_algo_displace_params, DISPLACE_F, NULL, pc_fx_halo_bounds,
          displace_k);
PC_FX_DEF(pc_algo_def_shear, "shear", pc_algo_shear_params, SHEAR_F, NULL, pc_fx_halo_bounds, shear_k);
PC_FX_DEF(pc_algo_def_zigzag, "zigZag", pc_algo_zigzag_params, ZIGZAG_F, NULL, pc_fx_halo_bounds, zigzag_k);
