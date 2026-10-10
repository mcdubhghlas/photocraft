/* Procedural renderers: primitives, compositing, randomness, Flame and the spec JSON (port of
 * crates/algo/src/render2.rs; Tree and Picture Frame are in render2_shapes.c). */
#include "algo/render2.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fx_defs.h"
#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "raster/pixel.h"

/* Size of the tiles composite works in. */
#define CTILE 128
/* Most composite tiles (the reference would run out of memory binning a huge clip). */
#define MAX_CTILES ((size_t)1 << 22)

/* ---- primitives ---- */

void pc_algo_prims_fini(pc_algo_prims *p)
{
    if (p == NULL) {
        return;
    }
    free(p->v);
    p->v = NULL;
    p->n = p->cap = 0;
}

pc_status pc_algo_prims_push(pc_algo_prims *p, pc_algo_prim prim, pc_error *err)
{
    if (p->n >= PC_ALGO_MAX_PRIMS) {
        return pc_fail(err, PC_ERR_LIMIT, "more than %zu render primitives", PC_ALGO_MAX_PRIMS);
    }
    if (p->n == p->cap) {
        pc_algo_prim *nv = pc_grow_array(p->v, &p->cap, p->n + 1, sizeof *nv);
        if (nv == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for render primitives");
        }
        p->v = nv;
    }
    p->v[p->n++] = prim;
    return PC_OK;
}

void pc_algo_render2_shade(const float c[3], float k, float out[4])
{
    out[0] = pc_clampf(c[0] * k, 0.0f, 1.0f);
    out[1] = pc_clampf(c[1] * k, 0.0f, 1.0f);
    out[2] = pc_clampf(c[2] * k, 0.0f, 1.0f);
    out[3] = 1.0f;
}

pc_algo_prim pc_algo_prim_capsule(const float a[2], const float b[2], float ra, float rb, const float c0[4],
                                  const float c1[4])
{
    pc_algo_prim p;
    memset(&p, 0, sizeof p);
    p.shape = PC_ALGO_PRIM_CAPSULE;
    p.a[0] = a[0], p.a[1] = a[1], p.b[0] = b[0], p.b[1] = b[1];
    p.ra = ra, p.rb = rb;
    memcpy(p.c0, c0, sizeof p.c0);
    memcpy(p.c1, c1, sizeof p.c1);
    p.soft = 1.0f;
    p.blend = PC_ALGO_BLEND_NORMAL;
    return p;
}

pc_algo_prim pc_algo_prim_ellipse(const float c[2], float rx, float ry, float angle, const float c0[4],
                                  const float c1[4])
{
    pc_algo_prim p;
    memset(&p, 0, sizeof p);
    p.shape = PC_ALGO_PRIM_ELLIPSE;
    p.c[0] = c[0], p.c[1] = c[1];
    p.rx = rx, p.ry = ry, p.angle = angle;
    memcpy(p.c0, c0, sizeof p.c0);
    memcpy(p.c1, c1, sizeof p.c1);
    p.soft = 1.0f;
    p.blend = PC_ALGO_BLEND_NORMAL;
    return p;
}

pc_algo_prim pc_algo_prim_disc(const float c[2], float r, const float col[4])
{
    return pc_algo_prim_ellipse(c, r, r, 0.0f, col, col);
}

static int32_t clamp_coord(float v)
{
    return pc_f32_to_i32(pc_clampf(v, -1.0e8f, 1.0e8f));
}

pc_rect pc_algo_prim_bounds(const pc_algo_prim *p)
{
    float s = pc_rs_maxf_k(p->soft, 1.0f) + 1.0f, x0, y0, x1, y1;
    if (p->shape == PC_ALGO_PRIM_CAPSULE) {
        float r = pc_rs_maxf(p->ra, p->rb) + s;
        x0 = pc_rs_minf(p->a[0], p->b[0]) - r;
        y0 = pc_rs_minf(p->a[1], p->b[1]) - r;
        x1 = pc_rs_maxf(p->a[0], p->b[0]) + r;
        y1 = pc_rs_maxf(p->a[1], p->b[1]) + r;
    } else {
        float sn = sinf(p->angle), cs = cosf(p->angle);
        float ex = sqrtf(pc_fx_sq(p->rx * cs) + pc_fx_sq(p->ry * sn)) + s;
        float ey = sqrtf(pc_fx_sq(p->rx * sn) + pc_fx_sq(p->ry * cs)) + s;
        x0 = p->c[0] - ex, y0 = p->c[1] - ey, x1 = p->c[0] + ex, y1 = p->c[1] + ey;
    }
    if (!(isfinite(x0) && isfinite(y0) && isfinite(x1) && isfinite(y1))) {
        return PC_RECT_EMPTY;
    }
    return pc_rect_make(clamp_coord(floorf(x0)), clamp_coord(floorf(y0)), clamp_coord(ceilf(x1)),
                        clamp_coord(ceilf(y1)));
}

/* Coverage (0..1) and colour position (0..1) at a pixel centre. */
static float prim_eval(const pc_algo_prim *p, float x, float y, float *ct)
{
    float soft = pc_rs_maxf_k(p->soft, 1.0f);
    if (p->shape == PC_ALGO_PRIM_CAPSULE) {
        float dx = p->b[0] - p->a[0], dy = p->b[1] - p->a[1], l2 = dx * dx + dy * dy, t, px, py, d, r, thin;
        t = l2 > 1e-9f ? pc_clampf(((x - p->a[0]) * dx + (y - p->a[1]) * dy) / l2, 0.0f, 1.0f) : 0.0f;
        px = p->a[0] + dx * t - x;
        py = p->a[1] + dy * t - y;
        d = sqrtf(px * px + py * py);
        r = p->ra + (p->rb - p->ra) * t;
        /* Hairlines thinner than a pixel fade instead of vanishing. */
        thin = pc_clampf(2.0f * r, 0.0f, 1.0f);
        *ct = t;
        return pc_clampf((pc_rs_maxf_k(r, 0.5f) - d) / soft + 0.5f, 0.0f, 1.0f) * thin;
    } else {
        float s = sinf(p->angle), co = cosf(p->angle), dx = x - p->c[0], dy = y - p->c[1];
        float u = dx * co + dy * s, v = -dx * s + dy * co;
        float rx = pc_rs_maxf_k(p->rx, 0.05f), ry = pc_rs_maxf_k(p->ry, 0.05f);
        float e = sqrtf(pc_fx_sq(u / rx) + pc_fx_sq(v / ry)), m = pc_rs_minf(rx, ry);
        float edge = (1.0f - e) * m, thin = pc_clampf(2.0f * m, 0.0f, 1.0f);
        *ct = pc_rs_minf_k(e, 1.0f);
        return pc_clampf(edge / soft + 0.5f, 0.0f, 1.0f) * thin;
    }
}

/* Rasterizes the listed prims (in order) into a premultiplied RGBA buffer covering t. */
static void raster_tile(const pc_algo_prim *prims, const uint32_t *idx, size_t nidx, pc_rect t,
                        float (*buf)[4])
{
    size_t w = pc_rect_width(t), k;
    for (k = 0; k < nidx; k++) {
        const pc_algo_prim *p = &prims[idx[k]];
        pc_rect r = pc_rect_intersect(pc_algo_prim_bounds(p), t);
        bool same =
            p->c0[0] == p->c1[0] && p->c0[1] == p->c1[1] && p->c0[2] == p->c1[2] && p->c0[3] == p->c1[3];
        int32_t x, y;
        if (pc_rect_is_empty(r)) {
            continue;
        }
        for (y = r.y0; y < r.y1; y++) {
            size_t row = (size_t)(y - t.y0) * w;
            for (x = r.x0; x < r.x1; x++) {
                float ct, cov = prim_eval(p, (float)x + 0.5f, (float)y + 0.5f, &ct), c[4], a, *d;
                int j;
                if (cov <= 0.0f) {
                    continue;
                }
                if (same) {
                    memcpy(c, p->c0, sizeof c);
                } else {
                    for (j = 0; j < 4; j++) {
                        c[j] = p->c0[j] + (p->c1[j] - p->c0[j]) * ct;
                    }
                }
                a = pc_clampf(c[3] * cov, 0.0f, 1.0f);
                if (a <= 0.0f) {
                    continue;
                }
                d = buf[row + (size_t)(x - t.x0)];
                switch (p->blend) {
                case PC_ALGO_BLEND_ADD:
                    d[0] += c[0] * a;
                    d[1] += c[1] * a;
                    d[2] += c[2] * a;
                    d[3] += a * (1.0f - d[3]);
                    break;
                case PC_ALGO_BLEND_LIGHTEN:
                    d[0] = pc_rs_maxf(d[0], c[0] * a);
                    d[1] = pc_rs_maxf(d[1], c[1] * a);
                    d[2] = pc_rs_maxf(d[2], c[2] * a);
                    d[3] = pc_rs_maxf(d[3], a);
                    break;
                default: {
                    float kk = 1.0f - a;
                    d[0] = c[0] * a + d[0] * kk;
                    d[1] = c[1] * a + d[1] * kk;
                    d[2] = c[2] * a + d[2] * kk;
                    d[3] = a + d[3] * kk;
                    break;
                }
                }
            }
        }
    }
}

typedef struct comp_job {
    pc_rect t;
    uint32_t *idx;
    size_t n, cap;
    float *data;
    pc_status st;
    pc_error err;
} comp_job;

typedef struct comp_ctx {
    const pc_surface *src;
    const pc_surface *sel;
    const pc_algo_prim *prims;
    comp_job *jobs;
} comp_ctx;

static pc_status comp_one(comp_ctx *cc, comp_job *j)
{
    pc_pixel_format fmt = pc_surface_format(cc->src);
    size_t n = pc_pixel_format_channels(fmt), npx = (size_t)pc_rect_width(j->t) * pc_rect_height(j->t),
           len = 0, slen = 0, sn = 1, i;
    float (*paint)[4] = pc_calloc_array(npx, sizeof *paint), *sel = NULL;
    pc_status st;
    if (paint == NULL) {
        return pc_fail(&j->err, PC_ERR_NOMEM, "out of memory compositing");
    }
    raster_tile(cc->prims, j->idx, j->n, j->t, paint);
    st = pc_surface_read_region(cc->src, j->t, &j->data, &len, &j->err);
    if (st == PC_OK && cc->sel != NULL) {
        sn = pc_surface_channels(cc->sel);
        st = pc_surface_read_region(cc->sel, j->t, &sel, &slen, &j->err);
    }
    if (st != PC_OK) {
        free(paint);
        free(sel);
        return st;
    }
    for (i = 0; i < npx; i++) {
        float *px = j->data + i * n, *s = paint[i],
              k = sel != NULL ? pc_clampf(sel[i * sn], 0.0f, 1.0f) : 1.0f;
        float sa = pc_clampf(s[3], 0.0f, 1.0f) * k, inv, sc[3], d[4], da, oa, o[4],
              tmp[PC_RASTER_MAX_CHANNELS];
        size_t m, c;
        if (sa <= 0.0f) {
            continue;
        }
        inv = 1.0f / pc_rs_maxf_k(s[3], 1e-6f);
        for (c = 0; c < 3; c++) {
            sc[c] = pc_clampf(s[c] * inv, 0.0f, 1.0f);
        }
        pc_to_rgba(fmt, px, n, d);
        da = fmt.alpha ? d[3] : 1.0f;
        oa = sa + da * (1.0f - sa);
        for (c = 0; c < 3; c++) {
            o[c] = oa > 1e-7f ? (sc[c] * sa + d[c] * da * (1.0f - sa)) / oa : 0.0f;
        }
        o[3] = oa;
        m = pc_from_rgba(fmt, o, tmp);
        memcpy(px, tmp, (m < n ? m : n) * sizeof(float));
    }
    free(paint);
    free(sel);
    return PC_OK;
}

static pc_status comp_chunk(void *vc, size_t chunk, size_t begin, size_t end)
{
    comp_ctx *cc = vc;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        cc->jobs[k].st = comp_one(cc, &cc->jobs[k]);
    }
    return PC_OK;
}

pc_status pc_algo_render_composite(pc_surface *surface, const pc_algo_prim *prims, size_t n, pc_rect clip,
                                   const pc_surface *selection, pc_rect *used, pc_error *err)
{
    pc_rect u = PC_RECT_EMPTY, *bounds = NULL;
    int32_t tx0, ty0, tx1, ty1;
    size_t nx, ny, nb, i, njobs = 0;
    comp_job *jobs = NULL;
    size_t *map = NULL;
    comp_ctx cc;
    pc_status st = PC_OK;
    if (n > 0) {
        bounds = pc_malloc_array(n, sizeof *bounds);
        if (bounds == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory compositing");
        }
    }
    for (i = 0; i < n; i++) {
        bounds[i] = pc_rect_intersect(pc_algo_prim_bounds(&prims[i]), clip);
        if (!pc_rect_is_empty(bounds[i])) {
            u = pc_rect_is_empty(u) ? bounds[i] : pc_rect_union(u, bounds[i]);
        }
    }
    if (pc_rect_is_empty(u)) {
        free(bounds);
        *used = u;
        return PC_OK;
    }
    /* Bin primitives into tiles (order preserved within a tile). */
    tx0 = pc_fx_div_euclid(u.x0, CTILE);
    ty0 = pc_fx_div_euclid(u.y0, CTILE);
    tx1 = pc_fx_div_euclid(u.x1 - 1, CTILE);
    ty1 = pc_fx_div_euclid(u.y1 - 1, CTILE);
    nx = (size_t)(tx1 - tx0 + 1);
    ny = (size_t)(ty1 - ty0 + 1);
    if (!pc_size_mul(nx, ny, &nb) || nb > MAX_CTILES) {
        free(bounds);
        return pc_fail(err, PC_ERR_LIMIT, "composite area of %zux%zu tiles is too large", nx, ny);
    }
    jobs = pc_calloc_array(nb, sizeof *jobs);
    map = pc_malloc_array(nb, sizeof *map);
    if (jobs == NULL || map == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory compositing");
        goto done;
    }
    for (i = 0; i < n; i++) {
        pc_rect b = bounds[i];
        int32_t ty, tx;
        if (pc_rect_is_empty(b)) {
            continue;
        }
        for (ty = pc_fx_div_euclid(b.y0, CTILE); ty <= pc_fx_div_euclid(b.y1 - 1, CTILE); ty++) {
            for (tx = pc_fx_div_euclid(b.x0, CTILE); tx <= pc_fx_div_euclid(b.x1 - 1, CTILE); tx++) {
                comp_job *j = &jobs[(size_t)(ty - ty0) * nx + (size_t)(tx - tx0)];
                if (j->n == j->cap) {
                    uint32_t *ni = pc_grow_array(j->idx, &j->cap, j->n + 1, sizeof *ni);
                    if (ni == NULL) {
                        st = pc_fail(err, PC_ERR_NOMEM, "out of memory compositing");
                        goto done;
                    }
                    j->idx = ni;
                }
                j->idx[j->n++] = (uint32_t)i;
            }
        }
    }
    for (i = 0; i < nb; i++) {
        int32_t tx = tx0 + (int32_t)(i % nx), ty = ty0 + (int32_t)(i / nx);
        if (jobs[i].n == 0) {
            continue;
        }
        jobs[i].t =
            pc_rect_intersect(pc_rect_make(tx * CTILE, ty * CTILE, (tx + 1) * CTILE, (ty + 1) * CTILE), clip);
        if (!pc_rect_is_empty(jobs[i].t)) {
            map[njobs++] = i;
        }
    }
    /* Compact the jobs so the parallel loop sees them in order. */
    for (i = 0; i < njobs; i++) {
        if (map[i] != i) {
            comp_job tmp = jobs[i];
            jobs[i] = jobs[map[i]];
            jobs[map[i]] = tmp;
        }
    }
    cc.src = surface;
    cc.sel = selection;
    cc.prims = prims;
    cc.jobs = jobs;
    (void)pc_algo_par_for(njobs, 1, comp_chunk, &cc);
    for (i = 0; i < njobs; i++) {
        if (jobs[i].st != PC_OK) {
            if (err != NULL) {
                *err = jobs[i].err;
            }
            st = jobs[i].st;
            goto done;
        }
    }
    for (i = 0; i < njobs && st == PC_OK; i++) {
        size_t len =
            (size_t)pc_rect_width(jobs[i].t) * pc_rect_height(jobs[i].t) * pc_surface_channels(surface);
        st = pc_surface_write_region(surface, jobs[i].t, jobs[i].data, len, err);
    }
done:
    if (jobs != NULL) {
        for (i = 0; i < nb; i++) {
            free(jobs[i].idx);
            free(jobs[i].data);
        }
    }
    free(jobs);
    free(map);
    free(bounds);
    if (st == PC_OK) {
        *used = u;
    }
    return st;
}

/* ---- randomness ---- */

pc_algo_rng pc_algo_rng_new(uint32_t seed, uint64_t salt)
{
    pc_algo_rng r = {((uint64_t)seed << 32) ^ (salt * 0x9E3779B97F4A7C15ull) ^ 0x2545F4914F6CDD1Dull};
    return r;
}

uint64_t pc_algo_rng_next(pc_algo_rng *r)
{
    uint64_t z;
    r->s += 0x9E3779B97F4A7C15ull;
    z = r->s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float pc_algo_rng_f(pc_algo_rng *r)
{
    return (float)(pc_algo_rng_next(r) >> 40) / (float)(1ull << 24);
}

float pc_algo_rng_range(pc_algo_rng *r, float a, float b)
{
    return a + (b - a) * pc_algo_rng_f(r);
}

float pc_algo_rng_sym(pc_algo_rng *r)
{
    return pc_algo_rng_f(r) * 2.0f - 1.0f;
}

static float noise_h(int64_t i, uint64_t seed)
{
    pc_algo_rng r = {((uint64_t)i * 0xD6E8FEB86659FD93ull) ^ seed};
    return pc_algo_rng_sym(&r);
}

/* Smooth 1-D value noise in [-1, 1]. */
static float noise1(float x, uint64_t seed)
{
    float i = floorf(x), f = x - i, t = f * f * (3.0f - 2.0f * f);
    int64_t ii = pc_f32_to_i64(i);
    float h0 = noise_h(ii, seed), h1 = noise_h((int64_t)((uint64_t)ii + 1u), seed);
    return h0 + (h1 - h0) * t;
}

/* Two octaves of noise1. */
static float fbm1(float x, uint64_t seed)
{
    return (noise1(x, seed) + 0.5f * noise1(x * 2.13f + 7.1f, seed ^ 0xA5A5u)) / 1.5f;
}

/* ---- polylines parameterized by arc length ---- */

typedef struct spine {
    const float (*pts)[2];
    size_t n;
    float *cum;
} spine;

static pc_status spine_init(spine *s, const float (*pts)[2], size_t n, pc_error *err)
{
    size_t i;
    s->pts = pts;
    s->n = n;
    s->cum = pc_malloc_array(n > 0 ? n : 1, sizeof *s->cum);
    if (s->cum == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a flame path");
    }
    s->cum[0] = 0.0f;
    for (i = 1; i < n; i++) {
        s->cum[i] =
            s->cum[i - 1] + sqrtf(pc_fx_sq(pts[i][0] - pts[i - 1][0]) + pc_fx_sq(pts[i][1] - pts[i - 1][1]));
    }
    return PC_OK;
}

static float spine_len(const spine *s)
{
    return s->n > 0 ? s->cum[s->n - 1] : 0.0f;
}

/* slice::binary_search_by with `v.partial_cmp(&s).unwrap_or(Less)` (Rust 1.95's branchless loop),
 * returning the reference's segment choice. */
static size_t spine_segment(const spine *s, float t)
{
    size_t size = s->n, base = 0, k;
    int cmp;
    while (size > 1) {
        size_t half = size / 2, mid = base + half;
        float v = s->cum[mid];
        /* Greater only when v > t (NaN compares as Less). */
        base = v > t ? base : mid;
        size -= half;
    }
    cmp = s->cum[base] > t ? 1 : (s->cum[base] == t ? 0 : -1);
    if (cmp == 0) {
        k = base;
    } else {
        k = base + (cmp < 0 ? 1 : 0);
        k = k > 0 ? k - 1 : 0;
    }
    return k < s->n - 2 ? k : s->n - 2;
}

/* Point and unit tangent at arc length t. */
static void spine_at(const spine *s, float t, float p[2], float d[2])
{
    size_t k;
    float seg, u, dx, dy, l;
    if (s->n == 0) {
        p[0] = 0.0f, p[1] = 0.0f, d[0] = 1.0f, d[1] = 0.0f;
        return;
    }
    if (s->n == 1) {
        p[0] = s->pts[0][0], p[1] = s->pts[0][1], d[0] = 0.0f, d[1] = -1.0f;
        return;
    }
    t = pc_clampf(t, 0.0f, spine_len(s));
    k = spine_segment(s, t);
    seg = pc_rs_maxf_k(s->cum[k + 1] - s->cum[k], 1e-6f);
    u = pc_clampf((t - s->cum[k]) / seg, 0.0f, 1.0f);
    dx = s->pts[k + 1][0] - s->pts[k][0];
    dy = s->pts[k + 1][1] - s->pts[k][1];
    l = pc_rs_maxf_k(sqrtf(dx * dx + dy * dy), 1e-6f);
    p[0] = s->pts[k][0] + dx * u;
    p[1] = s->pts[k][1] + dy * u;
    d[0] = dx / l;
    d[1] = dy / l;
}

/* ---- Flame ---- */

static const char *const FLAME_TYPES[] = {"oneFlameAlongPath",
                                          "multipleFlamesAlongPath",
                                          "multipleFlamesPathDirections",
                                          "multipleFlamesVariousLength",
                                          "candleLight",
                                          "multipleFlamesOneDirection"};
static const char *const FLAME_STYLES[] = {"normal", "violent", "flat"};
static const char *const FLAME_SHAPES[] = {"parallel", "toCenter", "spread", "oval", "pointed"};
const pc_algo_enum_desc PC_ALGO_ENUM_FLAME_TYPE = {"FlameType", FLAME_TYPES, 6, 0};
const pc_algo_enum_desc PC_ALGO_ENUM_FLAME_STYLE = {"FlameStyle", FLAME_STYLES, 3, 0};
const pc_algo_enum_desc PC_ALGO_ENUM_FLAME_SHAPE = {"FlameShape", FLAME_SHAPES, 5, 0};

pc_algo_flame_spec pc_algo_flame_spec_default(void)
{
    pc_algo_flame_spec s;
    memset(&s, 0, sizeof s);
    s.flame_type = PC_ALGO_FLAME_ONE_ALONG_PATH;
    s.length = 150.0f;
    s.width = 40.0f;
    s.interval = 60.0f;
    s.adjust_interval_for_loops = true;
    s.turbulent = 25.0f;
    s.jag = 25.0f;
    s.opacity = 75.0f;
    s.flame_lines = 20.0f;
    s.flame_bottom_alignment = 20.0f;
    s.quality = 2;
    return s;
}

/* Natural flame colour at position t along the flame (0 = base, 1 = tip). */
static void flame_colour(float t, const pc_algo_flame_spec *spec, float out[4])
{
    float a = powf(1.0f - t, 0.7f) * 0.9f + 0.1f * (1.0f - t), k = pc_clampf(t, 0.0f, 1.0f);
    int i;
    if (spec->has_color) {
        const float *c = spec->color;
        /* Hot near-white core -> the colour -> a darker rim. */
        for (i = 0; i < 3; i++) {
            float core = c[i] * 0.4f + 0.6f, rim = c[i] * 0.6f;
            out[i] =
                k < 0.35f ? core + (c[i] - core) * (k / 0.35f) : c[i] + (rim - c[i]) * ((k - 0.35f) / 0.65f);
        }
        out[3] = a * c[3];
    } else {
        static const float stops[5][4] = {{0.0f, 1.0f, 0.98f, 0.85f},
                                          {0.2f, 1.0f, 0.86f, 0.35f},
                                          {0.45f, 1.0f, 0.55f, 0.08f},
                                          {0.75f, 0.9f, 0.25f, 0.02f},
                                          {1.0f, 0.6f, 0.08f, 0.0f}};
        int s;
        out[0] = stops[4][1], out[1] = stops[4][2], out[2] = stops[4][3];
        for (s = 0; s < 4; s++) {
            if (k <= stops[s + 1][0]) {
                float u = (k - stops[s][0]) / (stops[s + 1][0] - stops[s][0]);
                for (i = 0; i < 3; i++) {
                    out[i] = stops[s][i + 1] + (stops[s + 1][i + 1] - stops[s][i + 1]) * u;
                }
                break;
            }
        }
        out[3] = a;
    }
}

static float flame_sway(float turb, float half, uint64_t seed, float u)
{
    return turb * half * 1.6f * fbm1(u * 1.7f, seed ^ 0x5A5Au) * powf(u, 1.4f);
}

/* One flame: its spine (base -> tip), width and random stream. */
static pc_status draw_flame(pc_algo_prims *out, const spine *sp, float width, const pc_algo_flame_spec *spec,
                            pc_algo_rng *rng, pc_error *err)
{
    static const size_t SEGS[5] = {6, 10, 16, 24, 36};
    float len = spine_len(sp), turb, jag, opacity, half, line_r;
    size_t lines, segs, li, k;
    pc_algo_flame_shape shape;
    uint64_t seed;
    pc_status st;
    if (len < 1.0f) {
        return PC_OK;
    }
    lines = pc_f32_to_size(roundf(pc_clampf(spec->flame_lines, 1.0f, 100.0f) * 0.6f + 3.0f));
    segs = SEGS[spec->quality < 4 ? spec->quality : 4];
    k = pc_f32_to_size(len / 12.0f);
    segs = segs > k ? segs : k;
    segs = segs < 160 ? segs : 160;
    turb = pc_clampf(spec->turbulent, 0.0f, 100.0f) / 100.0f;
    jag = pc_clampf(spec->jag, 0.0f, 100.0f) / 100.0f;
    if (spec->flame_style == PC_ALGO_FLAME_STYLE_VIOLENT) {
        turb = pc_rs_minf_k(turb * 1.8f + 0.2f, 2.0f);
        jag = pc_rs_minf_k(jag * 1.8f + 0.15f, 2.0f);
    } else if (spec->flame_style == PC_ALGO_FLAME_STYLE_FLAT) {
        turb *= 0.35f;
        jag *= 0.35f;
    }
    shape = spec->randomize_shapes ? (pc_algo_flame_shape)(pc_algo_rng_next(rng) % 5) : spec->flame_shape;
    opacity = pc_clampf(spec->opacity, 0.0f, 100.0f) / 100.0f;
    half = pc_rs_maxf_k(width, 1.0f) / 2.0f;
    seed = pc_algo_rng_next(rng);
    /* A soft additive body glow along the spine. */
    for (k = 0; k < 6; k++) {
        float u = (float)k / 6.0f * 0.75f + 0.05f, p[2], d[2], o, c[4], r, cen[2], c0[4], c1[4];
        pc_algo_prim prim;
        spine_at(sp, u * len, p, d);
        o = flame_sway(turb, half, seed, u);
        flame_colour(u, spec, c);
        r = half * (1.25f - u) + 2.0f;
        cen[0] = p[0] - d[1] * o;
        cen[1] = p[1] + d[0] * o;
        c0[0] = c[0], c0[1] = c[1], c0[2] = c[2], c0[3] = 0.16f * opacity;
        c1[0] = c[0], c1[1] = c[1] * 0.6f, c1[2] = c[2] * 0.3f, c1[3] = 0.0f;
        prim = pc_algo_prim_ellipse(cen, r, r * 1.2f, atan2f(d[1], d[0]), c0, c1);
        prim.soft = pc_rs_maxf_k(r, 2.0f);
        prim.blend = PC_ALGO_BLEND_ADD;
        st = pc_algo_prims_push(out, prim, err);
        if (st != PC_OK) {
            return st;
        }
    }
    line_r = pc_clampf(half / (float)lines * 2.2f, 0.8f, 10.0f);
    for (li = 0; li < lines; li++) {
        float lane = lines == 1 ? 0.0f : (float)li / (float)(lines - 1) * 2.0f - 1.0f;
        uint64_t lseed = seed ^ ((uint64_t)li * 0x517CC1B727220Aull);
        float start =
            (pc_clampf(spec->flame_bottom_alignment, 0.0f, 100.0f) / 100.0f) * pc_algo_rng_f(rng) * 0.4f;
        /* Outer lines are shorter, giving the flame its tongue shape. */
        float reach = (1.0f - 0.55f * powf(fabsf(lane), 1.5f)) * pc_algo_rng_range(rng, 0.7f, 1.0f);
        float end = pc_clampf(start + reach * (1.0f - start), start + 0.05f, 1.0f);
        float lr = line_r * pc_algo_rng_range(rng, 0.6f, 1.3f), q[2] = {0, 0}, rq = 0.0f, uq = 0.0f;
        bool prev = false;
        for (k = 0; k <= segs; k++) {
            float u = start + (end - start) * (float)k / (float)segs, s = u * len, p[2], d[2], nx, ny, spread,
                  flick, off, pt[2], r, fade_t;
            spine_at(sp, s, p, d);
            nx = -d[1];
            ny = d[0];
            switch (shape) {
            case PC_ALGO_FLAME_SHAPE_TO_CENTER:
                spread = 1.0f - u;
                break;
            case PC_ALGO_FLAME_SHAPE_SPREAD:
                spread = 1.0f + u;
                break;
            case PC_ALGO_FLAME_SHAPE_OVAL:
                spread = sinf(PC_FX_PI * (0.15f + 0.85f * u)) * 1.3f;
                break;
            case PC_ALGO_FLAME_SHAPE_POINTED:
                spread = pc_fx_sq(1.0f - u);
                break;
            default:
                spread = 1.0f - 0.6f * u;
                break;
            }
            flick = turb * half * 0.5f * fbm1(s / (len * 0.2f + 6.0f) + (float)li * 0.37f, lseed) * u +
                    jag * half * 0.25f * noise1(s / 5.0f, lseed ^ 0x77u) * u;
            off = lane * half * spread + flame_sway(turb, half, seed, u) + flick;
            pt[0] = p[0] + nx * off;
            pt[1] = p[1] + ny * off;
            r = lr * (1.0f - u * 0.75f);
            /* Lines fade out towards their own tip. */
            fade_t = pc_clampf((u - start) / pc_rs_maxf_k(end - start, 1e-3f), 0.0f, 1.0f);
            if (prev) {
                /* Fade in over the first tenth (a soft base) and out towards the line's tip. */
                float ca[4], cbb[4];
                pc_algo_prim prim;
                flame_colour(uq, spec, ca);
                flame_colour(u, spec, cbb);
                ca[3] = ca[3] * opacity * (1.0f - fade_t * (fade_t * fade_t)) *
                        sqrtf(pc_clampf((uq - start) / 0.08f, 0.0f, 1.0f));
                cbb[3] = cbb[3] * opacity * (1.0f - fade_t * (fade_t * fade_t)) *
                         sqrtf(pc_clampf((u - start) / 0.08f, 0.0f, 1.0f));
                prim = pc_algo_prim_capsule(q, pt, rq, r, ca, cbb);
                prim.soft = 1.0f + lr * 0.6f;
                prim.blend = PC_ALGO_BLEND_LIGHTEN;
                st = pc_algo_prims_push(out, prim, err);
                if (st != PC_OK) {
                    return st;
                }
            }
            prev = true;
            q[0] = pt[0], q[1] = pt[1], rq = r, uq = u;
        }
    }
    return PC_OK;
}

static pc_status flame_on(pc_algo_prims *out, const float (*pts)[2], size_t n, float width,
                          const pc_algo_flame_spec *spec, pc_algo_rng *rng, pc_error *err)
{
    spine sp;
    pc_status st = spine_init(&sp, pts, n, err);
    if (st == PC_OK) {
        st = draw_flame(out, &sp, width, spec, rng, err);
        free(sp.cum);
    }
    return st;
}

pc_status pc_algo_flame(const pc_algo_flame_spec *spec, const pc_algo_polyline *paths, size_t npaths,
                        pc_algo_prims *out, pc_error *err)
{
    pc_algo_prims res = {NULL, 0, 0};
    pc_algo_rng rng = pc_algo_rng_new(spec->seed, 0xF1A3);
    float length = pc_clampf(spec->length, 1.0f, 1000.0f), width = pc_clampf(spec->width, 1.0f, 1000.0f);
    float interval = pc_clampf(spec->interval, 1.0f, 1000.0f), opac = pc_clampf(spec->opacity, 0.0f, 100.0f);
    size_t pi;
    pc_status st = PC_OK;
    for (pi = 0; pi < npaths && st == PC_OK; pi++) {
        const pc_algo_polyline *path = &paths[pi];
        spine sp;
        float total;
        if (path->n == 0) {
            continue;
        }
        st = spine_init(&sp, (const float (*)[2])path->v, path->n, err);
        if (st != PC_OK) {
            break;
        }
        total = spine_len(&sp);
        if (spec->flame_type == PC_ALGO_FLAME_ONE_ALONG_PATH && path->n >= 2 && total >= 1.0f) {
            st = draw_flame(&res, &sp, width, spec, &rng, err);
        } else if (spec->flame_type == PC_ALGO_FLAME_CANDLE_LIGHT ||
                   spec->flame_type == PC_ALGO_FLAME_ONE_ALONG_PATH) {
            /* A candle: a short teardrop rising straight up from the start. */
            float base[2] = {path->v[0][0], path->v[0][1]}, h = pc_rs_minf_k(length, 400.0f);
            const float cpts[2][2] = {{base[0], base[1]}, {base[0], base[1] - h}};
            pc_algo_flame_spec c = *spec;
            float core[4], cen[2] = {base[0], base[1] - h * 0.28f}, c0[4], c1[4];
            pc_algo_prim prim;
            c.flame_shape = PC_ALGO_FLAME_SHAPE_OVAL;
            c.turbulent = spec->turbulent * 0.3f;
            c.jag = spec->jag * 0.2f;
            st = flame_on(&res, cpts, 2, pc_rs_maxf_k(width * 0.6f, 4.0f), &c, &rng, err);
            if (st == PC_OK) {
                flame_colour(0.05f, spec, core);
                c0[0] = c1[0] = core[0], c0[1] = c1[1] = core[1], c0[2] = c1[2] = core[2];
                c0[3] = 0.9f * opac / 100.0f;
                c1[3] = 0.0f;
                prim = pc_algo_prim_ellipse(cen, pc_rs_maxf_k(width * 0.12f, 1.5f), h * 0.22f, 0.0f, c0, c1);
                prim.soft = 2.0f;
                prim.blend = PC_ALGO_BLEND_ADD;
                st = pc_algo_prims_push(&res, prim, err);
            }
        } else {
            bool closed = path->n > 2 && fabsf(path->v[0][0] - path->v[path->n - 1][0]) < 1.0f &&
                          fabsf(path->v[0][1] - path->v[path->n - 1][1]) < 1.0f;
            bool loop = spec->adjust_interval_for_loops && closed;
            size_t count, k;
            float step;
            if (loop) {
                float c = roundf(total / interval);
                count = isfinite(c) && c > 1.0f ? pc_f32_to_size(c) : 1;
                count = count < 2048 ? count : 2048;
            } else {
                float c = floorf(total / interval);
                count = isfinite(c) && c >= 0.0f ? pc_f32_to_size(c) : 0;
                count = isfinite(c) && c >= 0.0f ? (count >= 2047 ? 2048 : count + 1) : 1;
            }
            step = loop ? total / (float)count : interval;
            for (k = 0; k < count && st == PC_OK; k++) {
                float s = (float)k * step, p[2], d[2], dir[2], var, l;
                float fpts[2][2];
                if (res.n >= 65536) {
                    break;
                }
                spine_at(&sp, s, p, d);
                if (spec->flame_type == PC_ALGO_FLAME_MULTIPLE_PATH_DIRECTIONS) {
                    dir[0] = d[0], dir[1] = d[1];
                } else if (spec->flame_type == PC_ALGO_FLAME_MULTIPLE_ONE_DIRECTION) {
                    float a = pc_fx_rad(spec->angle);
                    dir[0] = sinf(a), dir[1] = -cosf(a);
                } else {
                    /* Left of the path direction (up for a left-to-right path). */
                    dir[0] = d[1], dir[1] = -d[0];
                }
                var = spec->flame_type == PC_ALGO_FLAME_MULTIPLE_VARIOUS_LENGTH ? 0.7f : 0.0f;
                if (spec->randomize_length || var > 0.0f) {
                    float vm = pc_rs_maxf_k(var, 0.4f);
                    l = length * pc_algo_rng_range(&rng, 1.0f - vm, 1.0f + vm * 0.5f);
                } else {
                    l = length;
                }
                fpts[0][0] = p[0], fpts[0][1] = p[1];
                fpts[1][0] = p[0] + dir[0] * l, fpts[1][1] = p[1] + dir[1] * l;
                st = flame_on(&res, (const float (*)[2])fpts, 2, width, spec, &rng, err);
            }
        }
        free(sp.cum);
    }
    if (st != PC_OK) {
        pc_algo_prims_fini(&res);
        return st;
    }
    *out = res;
    return PC_OK;
}

void pc_algo_default_flame_path(pc_rect canvas, float out[33][2])
{
    float w = (float)pc_rect_width(canvas), h = (float)pc_rect_height(canvas);
    int k;
    for (k = 0; k <= 32; k++) {
        float t = (float)k / 32.0f;
        out[k][0] = (float)canvas.x0 + w * (0.15f + 0.7f * t);
        out[k][1] = (float)canvas.y0 + h * (0.8f - 0.08f * sinf(PC_FX_PI * t));
    }
}

/* ---- spec JSON ---- */

/* Option<[f32; 4]>: null or an array of four numbers. */
static pc_status read_color(yyjson_val *v, void *slot, pc_error *err)
{
    pc_algo_flame_spec *s = (pc_algo_flame_spec *)((char *)slot - offsetof(pc_algo_flame_spec, has_color));
    size_t i = 0, total;
    yyjson_arr_iter it;
    yyjson_val *e;
    float tmp[4];
    if (yyjson_is_null(v)) {
        s->has_color = false;
        return PC_OK;
    }
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "an array of length 4", err);
    }
    total = yyjson_arr_size(v);
    yyjson_arr_iter_init(v, &it);
    while (i < 4 && (e = yyjson_arr_iter_next(&it)) != NULL) {
        pc_status st = pc_algo_jread_f32(e, &tmp[i], err);
        if (st != PC_OK) {
            return st;
        }
        i++;
    }
    if (i < 4) {
        return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected an array of length 4", i);
    }
    if (total > 4) {
        return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected 4 elements in sequence", total);
    }
    memcpy(s->color, tmp, sizeof tmp);
    s->has_color = true;
    return PC_OK;
}

static yyjson_mut_val *write_f32(yyjson_mut_doc *d, float v)
{
    return isfinite(v) ? yyjson_mut_float(d, v) : yyjson_mut_null(d);
}

static yyjson_mut_val *write_color(yyjson_mut_doc *d, const void *slot)
{
    const pc_algo_flame_spec *s =
        (const pc_algo_flame_spec *)(const void *)((const char *)slot -
                                                   offsetof(pc_algo_flame_spec, has_color));
    yyjson_mut_val *a;
    int i;
    if (!s->has_color) {
        return yyjson_mut_null(d);
    }
    a = yyjson_mut_arr(d);
    for (i = 0; a != NULL && i < 4; i++) {
        yyjson_mut_val *v = write_f32(d, s->color[i]);
        if (v == NULL || !yyjson_mut_arr_append(a, v)) {
            return NULL;
        }
    }
    return a;
}

#define F(nm, T, m, k) PC_ALGO_JFD(nm, T, m, k)
static const pc_algo_jfield FLAME_F[] = {
    PC_ALGO_JFE("flameType", pc_algo_flame_spec, flame_type, PC_ALGO_ENUM_FLAME_TYPE, true),
    F("length", pc_algo_flame_spec, length, PC_ALGO_JF_F32),
    F("randomizeLength", pc_algo_flame_spec, randomize_length, PC_ALGO_JF_BOOL),
    F("width", pc_algo_flame_spec, width, PC_ALGO_JF_F32),
    F("angle", pc_algo_flame_spec, angle, PC_ALGO_JF_F32),
    F("interval", pc_algo_flame_spec, interval, PC_ALGO_JF_F32),
    F("adjustIntervalForLoops", pc_algo_flame_spec, adjust_interval_for_loops, PC_ALGO_JF_BOOL),
    {"color", PC_ALGO_JF_CUSTOM, offsetof(pc_algo_flame_spec, has_color), true, NULL, 0, read_color,
     write_color, NULL, NULL},
    F("turbulent", pc_algo_flame_spec, turbulent, PC_ALGO_JF_F32),
    F("jag", pc_algo_flame_spec, jag, PC_ALGO_JF_F32),
    F("opacity", pc_algo_flame_spec, opacity, PC_ALGO_JF_F32),
    F("flameLines", pc_algo_flame_spec, flame_lines, PC_ALGO_JF_F32),
    F("flameBottomAlignment", pc_algo_flame_spec, flame_bottom_alignment, PC_ALGO_JF_F32),
    PC_ALGO_JFE("flameStyle", pc_algo_flame_spec, flame_style, PC_ALGO_ENUM_FLAME_STYLE, true),
    PC_ALGO_JFE("flameShape", pc_algo_flame_spec, flame_shape, PC_ALGO_ENUM_FLAME_SHAPE, true),
    F("randomizeShapes", pc_algo_flame_spec, randomize_shapes, PC_ALGO_JF_BOOL),
    F("seed", pc_algo_flame_spec, seed, PC_ALGO_JF_U32),
    F("quality", pc_algo_flame_spec, quality, PC_ALGO_JF_U32),
};
static const pc_algo_jfield TREE_F[] = {
    F("baseTreeType", pc_algo_tree_spec, base_tree_type, PC_ALGO_JF_U32),
    F("lightDirection", pc_algo_tree_spec, light_direction, PC_ALGO_JF_U32),
    F("leavesAmount", pc_algo_tree_spec, leaves_amount, PC_ALGO_JF_F32),
    F("leavesSize", pc_algo_tree_spec, leaves_size, PC_ALGO_JF_F32),
    F("branchesHeight", pc_algo_tree_spec, branches_height, PC_ALGO_JF_F32),
    F("branchesThickness", pc_algo_tree_spec, branches_thickness, PC_ALGO_JF_F32),
    F("defaultLeaves", pc_algo_tree_spec, default_leaves, PC_ALGO_JF_BOOL),
    PC_ALGO_JFA("leavesColor", pc_algo_tree_spec, leaves_color, 4, true),
    F("customBranchColor", pc_algo_tree_spec, custom_branch_color, PC_ALGO_JF_BOOL),
    PC_ALGO_JFA("branchesColor", pc_algo_tree_spec, branches_color, 4, true),
    F("flatShading", pc_algo_tree_spec, flat_shading, PC_ALGO_JF_BOOL),
    F("seed", pc_algo_tree_spec, seed, PC_ALGO_JF_U32),
    F("x", pc_algo_tree_spec, x, PC_ALGO_JF_F32),
    F("y", pc_algo_tree_spec, y, PC_ALGO_JF_F32),
    F("size", pc_algo_tree_spec, size, PC_ALGO_JF_F32),
};
static const pc_algo_jfield FRAME_F[] = {
    F("frame", pc_algo_frame_spec, frame, PC_ALGO_JF_U32),
    F("margin", pc_algo_frame_spec, margin, PC_ALGO_JF_F32),
    F("size", pc_algo_frame_spec, size, PC_ALGO_JF_F32),
    F("arrangement", pc_algo_frame_spec, arrangement, PC_ALGO_JF_F32),
    F("lines", pc_algo_frame_spec, lines, PC_ALGO_JF_U32),
    F("thickness", pc_algo_frame_spec, thickness, PC_ALGO_JF_F32),
    F("fade", pc_algo_frame_spec, fade, PC_ALGO_JF_F32),
    PC_ALGO_JFA("vineColor", pc_algo_frame_spec, vine_color, 4, true),
    PC_ALGO_JFA("flowerColor", pc_algo_frame_spec, flower_color, 4, true),
    PC_ALGO_JFA("leafColor", pc_algo_frame_spec, leaf_color, 4, true),
    F("seed", pc_algo_frame_spec, seed, PC_ALGO_JF_U32),
};
#undef F

#define NF(a) (sizeof(a) / sizeof((a)[0]))

pc_status pc_algo_flame_spec_from_json(yyjson_val *v, pc_algo_flame_spec *out, pc_error *err)
{
    pc_algo_flame_spec s = pc_algo_flame_spec_default();
    pc_status st = pc_algo_jfields_read(FLAME_F, NF(FLAME_F), v, &s, "struct FlameSpec", NULL, err);
    if (st == PC_OK) {
        *out = s;
    }
    return st;
}

pc_status pc_algo_tree_spec_from_json(yyjson_val *v, pc_algo_tree_spec *out, pc_error *err)
{
    pc_algo_tree_spec s = pc_algo_tree_spec_default();
    pc_status st = pc_algo_jfields_read(TREE_F, NF(TREE_F), v, &s, "struct TreeSpec", NULL, err);
    if (st == PC_OK) {
        *out = s;
    }
    return st;
}

pc_status pc_algo_frame_spec_from_json(yyjson_val *v, pc_algo_frame_spec *out, pc_error *err)
{
    pc_algo_frame_spec s = pc_algo_frame_spec_default();
    pc_status st = pc_algo_jfields_read(FRAME_F, NF(FRAME_F), v, &s, "struct FrameSpec", NULL, err);
    if (st == PC_OK) {
        *out = s;
    }
    return st;
}

static yyjson_mut_val *write_obj(yyjson_mut_doc *d, const pc_algo_jfield *f, size_t n, const void *base)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (o == NULL || !pc_algo_jfields_write(f, n, d, o, base)) {
        return NULL;
    }
    return o;
}

yyjson_mut_val *pc_algo_flame_spec_to_json(yyjson_mut_doc *d, const pc_algo_flame_spec *s)
{
    return write_obj(d, FLAME_F, NF(FLAME_F), s);
}

yyjson_mut_val *pc_algo_tree_spec_to_json(yyjson_mut_doc *d, const pc_algo_tree_spec *s)
{
    return write_obj(d, TREE_F, NF(TREE_F), s);
}

yyjson_mut_val *pc_algo_frame_spec_to_json(yyjson_mut_doc *d, const pc_algo_frame_spec *s)
{
    return write_obj(d, FRAME_F, NF(FRAME_F), s);
}
