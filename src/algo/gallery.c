/* Blur Gallery: Tilt-Shift, Iris, Field (spatially varying Gaussian blur), Spin (rotational blur
 * inside ellipses) and Path (motion along paths) (port of crates/algo/src/gallery.rs).
 *
 * Spatially varying blur interpolates between a stack of Gaussian levels (sigma 0 and a geometric
 * series up to the maximum), each computed with a radius-independent box cascade, so cost grows
 * with the number of levels, not the blur size. */
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fx.h"
#include "algo/fx_defs.h"
#include "base/mem.h"

/* Gaussian sigma for a gallery blur amount in px. */
static float sigma_of(float blur)
{
    return pc_clampf(blur, 0.0f, 500.0f) * 0.6f;
}

float pc_algo_blur_gallery_reach(float blur)
{
    return 3.0f * sigma_of(blur) + 2.0f;
}

#define LEVELS 6

/* Per-pixel sigma at a document position. */
typedef float (*sigma_fn)(const void *c, float x, float y);

/* Hat-function weight of level k for sigma s. */
static float level_weight(const float *lv, size_t nlv, size_t k, float s)
{
    float c = lv[k];
    if (k > 0 && s >= lv[k - 1] && s <= c) {
        return (s - lv[k - 1]) / pc_rs_maxf_k(c - lv[k - 1], 1e-6f);
    }
    if (k + 1 < nlv && s >= c && s <= lv[k + 1]) {
        return (lv[k + 1] - s) / pc_rs_maxf_k(lv[k + 1] - c, 1e-6f);
    }
    if (k + 1 == nlv && s >= c) {
        return 1.0f;
    }
    return 0.0f;
}

/* Blurs src over out with a per-pixel sigma (<= smax). */
static pc_status variable_blur(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float smax,
                               sigma_fn sigma, const void *sc, float *dst, pc_error *err)
{
    size_t n = src->ch, ww = pc_rect_width(src->rect), wh = pc_rect_height(src->rect), npx = pc_fx_npx(out);
    size_t nlv = LEVELS + 1, k, i, c, plen = 0;
    pc_rect win = src->rect;
    float lv[LEVELS + 1], *sig = NULL, *p = NULL, *buf = NULL;
    pc_status st = PC_OK;
    if (smax < 0.3f) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    if (isnan(smax)) {
        /* The reference panics (f32::clamp with a NaN bound). */
        return pc_fail(err, PC_ERR_INVALID, "Blur Gallery: the blur amount must be a number");
    }
    if (npx == 0 || n == 0) {
        return PC_OK;
    }
    if (!pc_rect_contains_rect(win, out)) {
        return pc_fail(err, PC_ERR_INVALID, "Blur Gallery: the source must cover the output and its halo");
    }
    /* Level sigmas: 0, then smax / 2^(LEVELS-1) ... smax. */
    lv[0] = 0.0f;
    for (k = 0; k < LEVELS; k++) {
        lv[k + 1] = smax / (float)(1u << (LEVELS - 1 - k));
    }
    sig = pc_malloc_array(npx, sizeof *sig);
    if (sig == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the Blur Gallery");
    }
    for (i = 0; i < npx; i++) {
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        sig[i] = pc_clampf(sigma(sc, (float)x + 0.5f, (float)y + 0.5f), 0.0f, smax);
    }
    st = pc_algo_premul_window(src, win, ctx->alpha, &p, &plen, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    for (k = 0; k < nlv; k++) {
        const float *level = p;
        bool any = false;
        for (i = 0; i < npx && !any; i++) {
            any = level_weight(lv, nlv, k, sig[i]) > 0.0f;
        }
        if (!any) {
            continue;
        }
        if (k > 0) {
            if (buf == NULL) {
                buf = pc_malloc_array(plen > 0 ? plen : 1, sizeof *buf);
                if (buf == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory for the Blur Gallery");
                    goto cleanup;
                }
            }
            memcpy(buf, p, plen * sizeof *buf);
            st = pc_algo_gauss_blur_n(buf, ww, wh, n, lv[k], err);
            if (st != PC_OK) {
                goto cleanup;
            }
            level = buf;
        }
        for (i = 0; i < npx; i++) {
            float wk = level_weight(lv, nlv, k, sig[i]);
            int32_t x, y;
            size_t o;
            if (wk <= 0.0f) {
                continue;
            }
            pc_algo_xy(out, i, &x, &y);
            o = ((size_t)(y - win.y0) * ww + (size_t)(x - win.x0)) * n;
            for (c = 0; c < n; c++) {
                dst[i * n + c] += level[o + c] * wk;
            }
        }
    }
    for (i = 0; i < npx; i++) {
        pc_algo_unpremul_px(dst + i * n, n, ctx->alpha);
    }
cleanup:
    free(sig);
    free(p);
    free(buf);
    return st;
}

static float short_side(pc_rect b)
{
    uint32_t w = pc_rect_width(b), h = pc_rect_height(b);
    return pc_rs_maxf_k((float)(w < h ? w : h), 1.0f);
}

/* ---- Tilt-Shift ---- */

typedef struct tilt_c {
    float cx, cy, nx, ny, ss, smax, f0, f1;
} tilt_c;

static float tilt_sigma(const void *vc, float x, float y)
{
    const tilt_c *t = vc;
    float d = fabsf((x - t->cx) * t->nx + (y - t->cy) * t->ny) / t->ss;
    return t->smax * pc_algo_smoothstep(t->f0, t->f1, d);
}

/* A sharp band through the centre at `angle`, blurring with distance beyond `focus` over
 * `transition` (both fractions of the shorter side). */
static pc_status tilt_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                        const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_tilt_shift_params *tp = p;
    pc_rect b = ctx->bounds;
    float a = pc_fx_rad(tp->angle);
    tilt_c t;
    (void)prep;
    t.ss = short_side(b);
    t.cx = (float)b.x0 + tp->center_x * (float)pc_rect_width(b);
    t.cy = (float)b.y0 + tp->center_y * (float)pc_rect_height(b);
    /* Band normal (the band runs along `angle`). */
    t.nx = sinf(a);
    t.ny = cosf(a);
    t.smax = sigma_of(tp->blur);
    t.f0 = pc_rs_maxf_k(tp->focus, 0.0f);
    t.f1 = pc_rs_maxf_k(tp->focus, 0.0f) + pc_rs_maxf_k(tp->transition, 1e-3f);
    return variable_blur(src, out, ctx, t.smax, tilt_sigma, &t, dst, err);
}

/* ---- Iris / Spin geometry ---- */

typedef struct pin_d {
    float e, u, v;
} pin_d;

/* Normalized (super)elliptical distance of a point from an iris/spin pin. */
static pin_d pin_distance(float x, float y, pc_rect b, float px, float py, float rx, float ry, float angle,
                          float m)
{
    float ss = short_side(b);
    float cx = (float)b.x0 + px * (float)pc_rect_width(b), cy = (float)b.y0 + py * (float)pc_rect_height(b);
    float a = pc_fx_rad(isfinite(angle) ? angle : 0.0f), s = sinf(a), c = cosf(a);
    float dx = x - cx, dy = y - cy, u = dx * c + dy * s, v = -dx * s + dy * c, e;
    pin_d r;
    u = u / (pc_rs_maxf_k(rx, 1e-3f) * ss);
    v = v / (pc_rs_maxf_k(ry, 1e-3f) * ss);
    if (fabsf(m - 2.0f) < 1e-3f) {
        e = sqrtf(u * u + v * v);
    } else {
        e = powf(powf(fabsf(u), m) + powf(fabsf(v), m), 1.0f / m);
    }
    if (!isfinite(e)) {
        r.e = INFINITY, r.u = 0.0f, r.v = 0.0f;
    } else {
        r.e = e, r.u = u, r.v = v;
    }
    return r;
}

/* ---- Iris ---- */

typedef struct iris_c {
    const pc_algo_iris_pins *pins;
    pc_rect b;
} iris_c;

static float iris_sigma(const void *vc, float x, float y)
{
    const iris_c *ic = vc;
    float best = FLT_MAX;
    size_t i;
    for (i = 0; i < ic->pins->n; i++) {
        const pc_algo_iris_pin *p = &ic->pins->v[i];
        float m = 2.0f + pc_clampf(p->roundness, 0.0f, 100.0f) / 100.0f * 6.0f;
        pin_d d = pin_distance(x, y, ic->b, p->x, p->y, p->radius_x, p->radius_y, p->angle, m);
        best = pc_rs_minf(best, sigma_of(p->blur) *
                                    pc_algo_smoothstep(pc_clampf(p->feather, 0.0f, 0.999f), 1.0f, d.e));
    }
    return best;
}

/* Everything outside each pin's ellipse is blurred; the sharp core is `feather` of the ellipse.
 * Sharpness from any pin wins. */
static pc_status iris_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                        const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_iris_params *ip = p;
    iris_c ic = {&ip->pins, ctx->bounds};
    float smax = 0.0f;
    size_t i;
    (void)prep;
    for (i = 0; i < ip->pins.n; i++) {
        smax = pc_rs_maxf(smax, sigma_of(ip->pins.v[i].blur));
    }
    if (ip->pins.n == 0) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    return variable_blur(src, out, ctx, smax, iris_sigma, &ic, dst, err);
}

/* ---- Field ---- */

typedef struct field_c {
    const float (*pts)[3];
    size_t n;
} field_c;

static float field_sigma(const void *vc, float x, float y)
{
    const field_c *fc = vc;
    float ws = 0.0f, s = 0.0f;
    size_t i;
    for (i = 0; i < fc->n; i++) {
        float w = 1.0f / (pc_fx_sq(x - fc->pts[i][0]) + pc_fx_sq(y - fc->pts[i][1]) + 1.0f);
        ws += w;
        s += w * fc->pts[i][2];
    }
    return s / ws;
}

/* Blur amounts at pins, interpolated by inverse squared distance. */
static pc_status field_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                         const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_field_params *fp = p;
    pc_rect b = ctx->bounds;
    float smax = 0.0f, (*pts)[3];
    field_c fc;
    size_t i;
    pc_status st;
    (void)prep;
    if (fp->pins.n == 0) {
        pc_algo_image_crop_into(src, out, dst);
        return PC_OK;
    }
    for (i = 0; i < fp->pins.n; i++) {
        smax = pc_rs_maxf(smax, sigma_of(fp->pins.v[i].blur));
    }
    pts = pc_malloc_array(fp->pins.n, sizeof *pts);
    if (pts == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Field Blur");
    }
    for (i = 0; i < fp->pins.n; i++) {
        const pc_algo_field_pin *pin = &fp->pins.v[i];
        pts[i][0] = (float)b.x0 + pin->x * (float)pc_rect_width(b);
        pts[i][1] = (float)b.y0 + pin->y * (float)pc_rect_height(b);
        pts[i][2] = sigma_of(pin->blur);
    }
    fc.pts = (const float (*)[3])pts;
    fc.n = fp->pins.n;
    st = variable_blur(src, out, ctx, smax, field_sigma, &fc, dst, err);
    free(pts);
    return st;
}

/* ---- Spin ---- */

/* Content inside each ellipse is blurred along arcs about its centre (`blur_angle` degrees of
 * rotation), feathered at the rim. */
static pc_status spin_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                        const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_spin_params *sp = p;
    size_t n = src->ch, i, c, pi;
    pc_rect b = ctx->bounds, clip = pc_rect_intersect(b, src->rect);
    float ss = short_side(b);
    (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    if (pc_rect_is_empty(clip) || n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, fx, fy;
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        fx = (float)x + 0.5f;
        fy = (float)y + 0.5f;
        for (pi = 0; pi < sp->pins.n; pi++) {
            const pc_algo_spin_pin *pin = &sp->pins.v[pi];
            pin_d d = pin_distance(fx, fy, b, pin->x, pin->y, pin->radius_x, pin->radius_y, pin->angle, 2.0f);
            float theta, rpx, cx, cy, a, sa, ca, acc[PC_ALGO_MAXC] = {0}, k;
            size_t m, j;
            if (!isfinite(d.e) || d.e >= 1.0f) {
                continue;
            }
            theta = pc_fx_rad(pc_clampf(pin->blur_angle, 0.0f, 360.0f));
            rpx = pc_rs_maxf_k(d.e * pc_rs_maxf(pin->radius_x, pin->radius_y) * ss, 0.0f);
            /* ~2 px between samples along the arc is smooth enough for a blur and halves the cost. */
            if (isfinite(rpx) && isfinite(theta)) {
                m = pc_f32_to_size(ceilf(theta * rpx / 2.0f));
                m = m < 2 ? 2 : (m > 64 ? 64 : m);
            } else {
                m = 2;
            }
            cx = (float)b.x0 + pin->x * (float)pc_rect_width(b);
            cy = (float)b.y0 + pin->y * (float)pc_rect_height(b);
            a = pc_fx_rad(isfinite(pin->angle) ? pin->angle : 0.0f);
            sa = sinf(a);
            ca = cosf(a);
            for (j = 0; j < m; j++) {
                float phi = theta * ((float)j / (float)(m - 1) - 0.5f), s2 = sinf(phi), c2 = cosf(phi);
                /* Rotate in the ellipse's normalized space, then map back. */
                float ru = d.u * c2 - d.v * s2, rv = d.u * s2 + d.v * c2;
                float lu = ru * pin->radius_x * ss, lv = rv * pin->radius_y * ss;
                pc_algo_add_sample_premul(src, cx + lu * ca - lv * sa, cy + lu * sa + lv * ca, clip,
                                          ctx->alpha, acc);
            }
            for (c = 0; c < n; c++) {
                acc[c] /= (float)m;
            }
            pc_algo_unpremul_px(acc, n, ctx->alpha);
            k = 1.0f - pc_algo_smoothstep(0.85f, 1.0f, d.e);
            for (c = 0; c < n; c++) {
                px[c] += (acc[c] - px[c]) * k;
            }
            break;
        }
    }
    return PC_OK;
}

/* ---- Path ---- */

typedef struct seg {
    float ax, ay, bx, by, t0, t1;
    size_t pi;
} seg;

/* Motion blur whose direction follows the nearest paths (inverse-distance weighted tangents) and
 * whose length is the path speed, optionally tapering toward each path's end. */
static pc_status path_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                        const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const pc_algo_path_params *pp = p;
    size_t n = src->ch, i, c, pi, nseg = 0, cap = 0, si;
    pc_rect b = ctx->bounds, clip;
    float bw = (float)pc_rect_width(b), bh = (float)pc_rect_height(b);
    seg *segs = NULL;
    (void)prep;
    pc_algo_image_crop_into(src, out, dst);
    for (pi = 0; pi < pp->paths.n; pi++) {
        if (pp->paths.v[pi].points.n > 1) {
            cap += pp->paths.v[pi].points.n - 1;
        }
    }
    clip = pc_rect_intersect(b, src->rect);
    if (cap == 0 || pc_rect_is_empty(clip) || n == 0 || n > PC_ALGO_MAXC) {
        return PC_OK;
    }
    segs = pc_malloc_array(cap, sizeof *segs);
    if (segs == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Path Blur");
    }
    /* Segments in document space, with their position along the path. */
    for (pi = 0; pi < pp->paths.n; pi++) {
        const pc_algo_points *pts = &pp->paths.v[pi].points;
        float total = -0.0f, run = 0.0f;
        size_t k;
        if (pts->n < 2) {
            continue;
        }
#define PX(k) ((float)b.x0 + pts->v[k][0] * bw)
#define PY(k) ((float)b.y0 + pts->v[k][1] * bh)
        for (k = 0; k + 1 < pts->n; k++) {
            total += hypotf(PX(k + 1) - PX(k), PY(k + 1) - PY(k));
        }
        total = pc_rs_maxf_k(total, 1e-3f);
        for (k = 0; k + 1 < pts->n; k++) {
            float l = hypotf(PX(k + 1) - PX(k), PY(k + 1) - PY(k));
            seg s = {PX(k), PY(k), PX(k + 1), PY(k + 1), run / total, (run + l) / total, pi};
            segs[nseg++] = s;
            run += l;
        }
#undef PX
#undef PY
    }
    for (i = 0; i < pc_fx_npx(out); i++) {
        float *px = dst + i * n, fx, fy, dx = 0.0f, dy = 0.0f, len = 0.0f, ws = 0.0f, m, ux, uy;
        float acc[PC_ALGO_MAXC] = {0};
        size_t steps, j;
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        fx = (float)x + 0.5f;
        fy = (float)y + 0.5f;
        for (si = 0; si < nseg; si++) {
            const seg *s = &segs[si];
            const pc_algo_blur_path *bp = &pp->paths.v[s->pi];
            float sx = s->bx - s->ax, sy = s->by - s->ay, l2 = pc_rs_maxf_k(sx * sx + sy * sy, 1e-6f);
            float t = pc_clampf(((fx - s->ax) * sx + (fy - s->ay) * sy) / l2, 0.0f, 1.0f);
            float d2 = pc_fx_sq(fx - s->ax - sx * t) + pc_fx_sq(fy - s->ay - sy * t);
            float w = 1.0f / (d2 + 1.0f), l = sqrtf(l2), along = s->t0 + (s->t1 - s->t0) * t;
            dx += w * sx / l;
            dy += w * sy / l;
            len += w * fabsf(bp->speed) * (1.0f - pc_clampf(bp->taper, 0.0f, 100.0f) / 100.0f * along);
            ws += w;
        }
        m = sqrtf(dx * dx + dy * dy);
        len = len / ws;
        if (m < 1e-6f || len < 0.5f) {
            continue;
        }
        ux = dx / m;
        uy = dy / m;
        /* Samples ~1.5 px apart (bilinear fills the gaps), capped for very long blurs. */
        steps = pc_f32_to_size(ceilf(len / 1.5f)) + 1;
        steps = steps < 2 ? 2 : (steps > 64 ? 64 : steps);
        for (j = 0; j < steps; j++) {
            float o = len * ((float)j / (float)(steps - 1) - 0.5f);
            pc_algo_add_sample_premul(src, fx + ux * o, fy + uy * o, clip, ctx->alpha, acc);
        }
        for (c = 0; c < n; c++) {
            acc[c] /= (float)steps;
        }
        pc_algo_unpremul_px(acc, n, ctx->alpha);
        memcpy(px, acc, n * sizeof(float));
    }
    free(segs);
    return PC_OK;
}

/* ---- halos and defs ---- */

static pc_algo_halo tilt_halo(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_reach(pc_algo_blur_gallery_reach(((const pc_algo_tilt_shift_params *)p)->blur));
}

static pc_algo_halo iris_halo(const void *p, const pc_rect *b)
{
    const pc_algo_iris_params *ip = p;
    float m = 0.0f;
    size_t i;
    (void)b;
    for (i = 0; i < ip->pins.n; i++) {
        m = pc_rs_maxf(m, ip->pins.v[i].blur);
    }
    return pc_algo_halo_reach(pc_algo_blur_gallery_reach(m));
}

static pc_algo_halo field_halo(const void *p, const pc_rect *b)
{
    const pc_algo_field_params *fp = p;
    float m = 0.0f;
    size_t i;
    (void)b;
    for (i = 0; i < fp->pins.n; i++) {
        m = pc_rs_maxf(m, fp->pins.v[i].blur);
    }
    return pc_algo_halo_reach(pc_algo_blur_gallery_reach(m));
}

static pc_algo_halo path_halo(const void *p, const pc_rect *b)
{
    const pc_algo_path_params *pp = p;
    float m = 0.0f;
    size_t i;
    (void)b;
    for (i = 0; i < pp->paths.n; i++) {
        m = pc_rs_maxf(m, fabsf(pp->paths.v[i].speed));
    }
    return pc_algo_halo_reach(m / 2.0f + 1.0f);
}

static const pc_algo_jfield TILT_F[] = {
    PC_ALGO_JF("blur", pc_algo_tilt_shift_params, blur, PC_ALGO_JF_F32),
    PC_ALGO_JF("centerX", pc_algo_tilt_shift_params, center_x, PC_ALGO_JF_F32),
    PC_ALGO_JF("centerY", pc_algo_tilt_shift_params, center_y, PC_ALGO_JF_F32),
    PC_ALGO_JF("angle", pc_algo_tilt_shift_params, angle, PC_ALGO_JF_F32),
    PC_ALGO_JF("focus", pc_algo_tilt_shift_params, focus, PC_ALGO_JF_F32),
    PC_ALGO_JF("transition", pc_algo_tilt_shift_params, transition, PC_ALGO_JF_F32),
};
static const pc_algo_jfield IRIS_F[] = {PC_ALGO_JF("pins", pc_algo_iris_params, pins, PC_ALGO_JF_IRIS_PINS)};
static const pc_algo_jfield FIELD_F[] = {
    PC_ALGO_JF("pins", pc_algo_field_params, pins, PC_ALGO_JF_FIELD_PINS)};
static const pc_algo_jfield SPIN_F[] = {PC_ALGO_JF("pins", pc_algo_spin_params, pins, PC_ALGO_JF_SPIN_PINS)};
static const pc_algo_jfield PATH_F[] = {
    PC_ALGO_JF("paths", pc_algo_path_params, paths, PC_ALGO_JF_BLUR_PATHS)};

PC_FX_DEF(pc_algo_def_tilt_shift, "tiltShift", pc_algo_tilt_shift_params, TILT_F, NULL, tilt_halo, tilt_k);
PC_FX_DEF(pc_algo_def_iris_blur, "irisBlur", pc_algo_iris_params, IRIS_F, NULL, iris_halo, iris_k);
PC_FX_DEF(pc_algo_def_field_blur, "fieldBlur", pc_algo_field_params, FIELD_F, NULL, field_halo, field_k);
PC_FX_DEF(pc_algo_def_spin_blur, "spinBlur", pc_algo_spin_params, SPIN_F, NULL, pc_fx_halo_bounds, spin_k);
PC_FX_DEF(pc_algo_def_path_blur, "pathBlur", pc_algo_path_params, PATH_F, NULL, path_halo, path_k);
