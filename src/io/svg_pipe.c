/* tiny-skia 0.11's raster pipeline as resvg 0.45 drives it, evaluated per pixel: solid colours,
 * linear / radial / two-point-conical gradients and pattern pixmaps (nearest, bilinear, bicubic;
 * pad, repeat, reflect) as shaders; the low-precision (u16, div255) pipeline when every stage has
 * one and the high-precision (f32) one otherwise, picked like RasterPipelineBuilder::compile; the
 * blend modes with tiny-skia's premultiplied formulas; coverage applied by pre-scaling or by
 * lerp as the blitter does; masks (Mask::from_pixmap, apply_mask) and draw_pixmap.
 *
 * The reference evaluates eight or sixteen pixels per SIMD lane on x86-64 (SSE2); the scalar
 * helpers below reproduce those instructions' semantics where they differ from C's: max/min
 * return the second operand for NaN, truncation and rounding to int give INT32_MIN out of range,
 * f32 rounding is to nearest-even. _mm_rcp_ps (an approximate reciprocal in colour-dodge/-burn)
 * is computed exactly instead. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "io/svg_internal.h"

/* ---------------------------------------------------------------- SSE2 scalar semantics */

static float mx(float a, float b) /* _mm_max_ps(a, b) */
{
    return a > b ? a : b;
}

static float mn(float a, float b) /* _mm_min_ps(a, b) */
{
    return a < b ? a : b;
}

static int32_t trunc_int(float v) /* _mm_cvttps_epi32 */
{
    if (!(v > -2147483904.0f && v < 2147483648.0f)) {
        return INT32_MIN;
    }
    return (int32_t)v;
}

static int32_t round_int(float v) /* _mm_cvtps_epi32 (nearest even) */
{
    float r;
    if (!(v > -2147483904.0f && v < 2147483648.0f)) {
        return INT32_MIN;
    }
    r = nearbyintf(v);
    if (!(r >= -2147483648.0f && r < 2147483648.0f)) {
        return INT32_MIN;
    }
    return (int32_t)r;
}

static float floor8(float v) /* f32x8::floor on SSE2: truncation round trip */
{
    float rt = (float)trunc_int(v);
    return rt > v ? rt - 1.0f : rt;
}

static float round16(float v) /* f32x16::round (lowp; nearest even) */
{
    return fabsf(v) < 8388608.0f ? nearbyintf(v) : v;
}

static float floor16(float v) /* f32x16::floor (lowp) */
{
    float rt = round16(v);
    return rt > v ? rt - 1.0f : rt;
}

static float normalize(float v)
{
    return mn(mx(v, 0.0f), 1.0f);
}

static uint16_t u16_sat(float v) /* `as u16` */
{
    if (!(v > 0.0f)) {
        return 0;
    }
    return v >= 65535.0f ? 65535 : (uint16_t)v;
}

static uint8_t unnorm(float v) /* highp store */
{
    return (uint8_t)round_int(mn(mx(v, 0.0f), 1.0f) * 255.0f);
}

static uint16_t div255(uint32_t v)
{
    return (uint16_t)(((v + 255u) >> 8) & 0xffffu);
}

#define F255 (1.0f / 255.0f)

/* ---------------------------------------------------------------- transforms */

/* The device -> shader space transform: transform.invert().post_concat(post). */
static bool inverse(sv_ts t, sv_ts *out)
{
    if (ts_is_identity(t)) {
        *out = t;
        return true;
    }
    return ts_invert(t, out);
}

/* tiny-skia's transform stage: x*sx + (y*kx + tx), x*ky + (y*sy + ty). */
static void map(const sv_ts *t, float x, float y, float *ox, float *oy)
{
    *ox = x * t->sx + (y * t->kx + t->tx);
    *oy = x * t->ky + (y * t->sy + t->ty);
}

/* ---------------------------------------------------------------- shaders */

typedef struct gcolor {
    float r, g, b, a;
} gcolor;

void sv_shader_fini(sv_shader *s)
{
    if (s != NULL) {
        free(s->t_values);
        free(s->factors);
        free(s->biases);
        free(s->stops);
        memset(s, 0, sizeof *s);
    }
}

void sv_shader_solid(sv_shader *s, uint8_t r, uint8_t g, uint8_t b, float opacity)
{
    memset(s, 0, sizeof *s);
    s->kind = SH_SOLID;
    s->color[0] = (float)r / 255.0f;
    s->color[1] = (float)g / 255.0f;
    s->color[2] = (float)b / 255.0f;
    s->color[3] = (float)sv_opacity_u8(opacity) / 255.0f;
}

/* Color::premultiply then push_uniform_color. */
static void solid_premul(const sv_shader *s, float pf[4], uint16_t pu[4])
{
    float a = s->color[3];
    int i;
    for (i = 0; i < 3; i++) {
        float v = a == 1.0f ? s->color[i] : s->color[i] * a;
        pf[i] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
    pf[3] = a;
    for (i = 0; i < 4; i++) {
        pu[i] = u16_sat(pf[i] * 255.0f + 0.5f);
    }
}

static bool nearly_zero(float v, float tol)
{
    return fabsf(v) <= tol;
}

#define DEGENERATE (1.0f / 32768.0f)

/* Gradient::new: dummy end stops, monotonic positions (already clamped to 0..1 by
 * GradientStop::new), uniform spacing. */
static bool gradient_new(sv_shader *s)
{
    size_t n = s->nstops;
    sv_gstop *st = s->stops;
    bool dummy_first;
    bool dummy_last;
    size_t start;
    float prev = 0.0f;
    float step;
    size_t i;
    size_t cap;
    sv_gstop *v;
    dummy_first = st[0].t != 0.0f;
    dummy_last = st[n - 1].t != 1.0f;
    cap = n + 2;
    v = pc_malloc_array(cap, sizeof *v);
    if (v == NULL) {
        return false;
    }
    i = 0;
    if (dummy_first) {
        v[i] = st[0];
        v[i].t = 0.0f;
        i++;
    }
    memcpy(v + i, st, n * sizeof *v);
    i += n;
    if (dummy_last) {
        v[i] = st[n - 1];
        v[i].t = 1.0f;
        i++;
    }
    free(s->stops);
    s->stops = v;
    s->nstops = n = i;
    st = v;
    start = dummy_first ? 0 : 1;
    s->uniform = true;
    step = st[start].t - prev;
    for (i = start; i < n; i++) {
        float curr;
        if (i + 1 == n) {
            curr = 1.0f;
        } else {
            /* bound(prev, 1.0) */
            curr = st[i].t;
            curr = mx(prev, mn(curr, 1.0f));
        }
        s->uniform = s->uniform && fabsf(step - (curr - prev)) <= 1.0f / 4096.0f;
        st[i].t = curr < 0.0f ? 0.0f : curr > 1.0f ? 1.0f : curr;
        prev = curr;
    }
    return true;
}

/* push_stages' gradient context from the processed stops. */
static bool gradient_build(sv_shader *s)
{
    size_t n = s->nstops;
    const sv_gstop *st = s->stops;
    size_t i;
    free(s->t_values);
    free(s->factors);
    free(s->biases);
    s->t_values = NULL;
    s->factors = NULL;
    s->biases = NULL;
    s->two_stop = false;
    s->len = 0;
    s->opaque = true;
    for (i = 0; i < n; i++) {
        if (st[i].c[3] != 1.0f) {
            s->opaque = false;
        }
    }
    if (n == 2) {
        s->two_stop = true;
        for (i = 0; i < 4; i++) {
            s->factor[i] = st[1].c[i] - st[0].c[i];
            s->bias[i] = st[0].c[i];
        }
        return true;
    }
    {
        size_t first = 0;
        size_t last = n - 1;
        float tl;
        gcolor cl;
        size_t m = 0;
        if (n > 2) {
            first = memcmp(st[0].c, st[1].c, sizeof st[0].c) != 0 ? 0 : 1;
            last = memcmp(st[n - 2].c, st[n - 1].c, sizeof st[0].c) != 0 ? n - 1 : n - 2;
        }
        s->t_values = pc_malloc_array(n + 2, sizeof *s->t_values);
        s->factors = pc_malloc_array(n + 2, 4 * sizeof(float));
        s->biases = pc_malloc_array(n + 2, 4 * sizeof(float));
        if (s->t_values == NULL || s->factors == NULL || s->biases == NULL) {
            return false;
        }
        tl = st[first].t;
        cl.r = st[first].c[0];
        cl.g = st[first].c[1];
        cl.b = st[first].c[2];
        cl.a = st[first].c[3];
        /* push_const_color: factor 0, bias c */
        memset(s->factors[m], 0, sizeof s->factors[m]);
        s->biases[m][0] = cl.r;
        s->biases[m][1] = cl.g;
        s->biases[m][2] = cl.b;
        s->biases[m][3] = cl.a;
        s->t_values[m++] = 0.0f;
        for (i = first; i < last; i++) {
            float tr = st[i + 1].t;
            gcolor cr;
            cr.r = st[i + 1].c[0];
            cr.g = st[i + 1].c[1];
            cr.b = st[i + 1].c[2];
            cr.a = st[i + 1].c[3];
            if (tl < tr) {
                float f[4];
                f[0] = (cr.r - cl.r) / (tr - tl);
                f[1] = (cr.g - cl.g) / (tr - tl);
                f[2] = (cr.b - cl.b) / (tr - tl);
                f[3] = (cr.a - cl.a) / (tr - tl);
                memcpy(s->factors[m], f, sizeof f);
                s->biases[m][0] = cl.r - f[0] * tl;
                s->biases[m][1] = cl.g - f[1] * tl;
                s->biases[m][2] = cl.b - f[2] * tl;
                s->biases[m][3] = cl.a - f[3] * tl;
                s->t_values[m++] = tl < 0.0f ? 0.0f : tl > 1.0f ? 1.0f : tl;
            }
            tl = tr;
            cl = cr;
        }
        memset(s->factors[m], 0, sizeof s->factors[m]);
        s->biases[m][0] = cl.r;
        s->biases[m][1] = cl.g;
        s->biases[m][2] = cl.b;
        s->biases[m][3] = cl.a;
        s->t_values[m++] = tl < 0.0f ? 0.0f : tl > 1.0f ? 1.0f : tl;
        s->len = m;
    }
    return true;
}

/* points_to_unit_ts for a linear gradient. */
static sv_ts points_to_unit(float sx, float sy, float ex, float ey)
{
    float vx = ex - sx;
    float vy = ey - sy;
    float mag = sqrtf(vx * vx + vy * vy);
    float inv = mag != 0.0f ? 1.0f / mag : 0.0f;
    float sn;
    float cs;
    float cos_inv;
    sv_ts ts;
    vx *= inv;
    vy *= inv;
    sn = -vy;
    cs = vx;
    cos_inv = 1.0f - cs;
    ts = ts_row(cs, sn, -sn, cs, sn * sy + cos_inv * sx, -sn * sx + cos_inv * sy);
    ts = ts_post_concat(ts, ts_translate(-sx, -sy));
    ts = ts_post_concat(ts, ts_scale(inv, inv));
    return ts;
}

static sv_ts from_poly2(float x0, float y0, float x1, float y1)
{
    return ts_row(y1 - y0, x0 - x1, x1 - x0, y1 - y0, x0, y0);
}

/* The average colour of reflected / repeated degenerate gradients. */
static void average_color(const sv_gstop *p, size_t n, float out[4])
{
    float b[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    size_t i;
    int k;
    for (i = 0; i + 1 < n; i++) {
        float w = p[i + 1].t - p[i].t;
        for (k = 0; k < 4; k++) {
            b[k] += 0.5f * w * (p[i + 1].c[k] + p[i].c[k]);
        }
    }
    if (p[0].t > 0.0f) {
        for (k = 0; k < 4; k++) {
            b[k] += p[0].t * p[0].c[k];
        }
    }
    if (p[n - 1].t < 1.0f) {
        for (k = 0; k < 4; k++) {
            b[k] += (1.0f - p[n - 1].t) * p[n - 1].c[k];
        }
    }
    for (k = 0; k < 4; k++) {
        out[k] = b[k] < 0.0f ? 0.0f : b[k] > 1.0f ? 1.0f : b[k];
    }
}

static void make_solid_f(sv_shader *s, const float c[4])
{
    sv_shader_fini(s);
    s->kind = SH_SOLID;
    memcpy(s->color, c, sizeof s->color);
}

int sv_shader_gradient(sv_shader *s, const sv_grad *g, float opacity, sv_ts path_ts)
{
    size_t i;
    sv_ts inv;
    memset(s, 0, sizeof *s);
    if (g->nstops == 0) {
        return 0;
    }
    s->stops = pc_malloc_array(g->nstops, sizeof *s->stops);
    if (s->stops == NULL) {
        return -1;
    }
    s->nstops = g->nstops;
    for (i = 0; i < g->nstops; i++) {
        float t = g->stops[i].offset;
        float a = g->stops[i].opacity * opacity;
        s->stops[i].t = t > 0.0f ? (t < 1.0f ? t : 1.0f) : 0.0f;
        s->stops[i].c[0] = (float)g->stops[i].r / 255.0f;
        s->stops[i].c[1] = (float)g->stops[i].g / 255.0f;
        s->stops[i].c[2] = (float)g->stops[i].b / 255.0f;
        s->stops[i].c[3] = (float)sv_opacity_u8(a) / 255.0f;
    }
    if (g->nstops == 1) {
        float c[4];
        memcpy(c, s->stops[0].c, sizeof c);
        make_solid_f(s, c);
        return 1;
    }
    s->spread = g->spread;
    if (!g->radial) {
        float dx = g->x2 - g->x1;
        float dy = g->y2 - g->y1;
        float length = sqrtf(dx * dx + dy * dy);
        if (!isfinite(length)) {
            sv_shader_fini(s);
            return 0;
        }
        if (nearly_zero(length, DEGENERATE)) {
            float c[4];
            if (g->spread == SPREAD_PAD) {
                memcpy(c, s->stops[s->nstops - 1].c, sizeof c);
            } else {
                average_color(s->stops, s->nstops, c);
            }
            make_solid_f(s, c);
            return 1;
        }
        if (!inverse(g->transform, &inv)) {
            sv_shader_fini(s);
            return 0;
        }
        s->kind = SH_LINEAR;
        s->unit = points_to_unit(g->x1, g->y1, g->x2, g->y2);
    } else {
        float r = g->r;
        float dx = g->cx - g->fx;
        float dy = g->cy - g->fy;
        float length;
        if (r < 0.0f || nearly_zero(r, 1.0f / 4096.0f)) {
            sv_shader_fini(s);
            return 0;
        }
        if (!inverse(g->transform, &inv)) {
            sv_shader_fini(s);
            return 0;
        }
        length = sqrtf(dx * dx + dy * dy);
        if (!isfinite(length)) {
            sv_shader_fini(s);
            return 0;
        }
        s->kind = SH_RADIAL;
        if (nearly_zero(length, DEGENERATE)) {
            float iv = 1.0f / r;
            s->unit = ts_post_concat(ts_translate(-g->fx, -g->fy), ts_scale(iv, iv));
            s->focal = 0;
        } else {
            sv_ts a = from_poly2(g->fx, g->fy, g->cx, g->cy);
            sv_ts ai;
            float d;
            float r1;
            if (!inverse(a, &ai)) {
                sv_shader_fini(s);
                return 0;
            }
            s->unit = ts_pre_concat(from_poly2(0.0f, 0.0f, 1.0f, 0.0f), ai);
            d = sqrtf((g->fx - g->cx) * (g->fx - g->cx) + (g->fy - g->cy) * (g->fy - g->cy));
            r1 = r / d;
            s->r1 = r1;
            if (nearly_zero(1.0f - r1, 1.0f / 4096.0f)) {
                s->focal = 1; /* focal on circle */
                s->unit = ts_post_concat(s->unit, ts_scale(0.5f, 0.5f));
            } else {
                s->focal = r1 > 1.0f ? 2 : 3; /* well behaved : greater */
                s->unit = ts_post_concat(
                    s->unit, ts_scale(r1 / (r1 * r1 - 1.0f), 1.0f / sqrtf(fabsf(r1 * r1 - 1.0f))));
            }
        }
    }
    s->transform = ts_post_concat(g->transform, path_ts);
    if (!gradient_new(s) || !gradient_build(s)) {
        sv_shader_fini(s);
        return -1;
    }
    if (!inverse(s->transform, &inv)) {
        sv_shader_fini(s);
        return 0;
    }
    s->inv = ts_post_concat(inv, s->unit);
    /* lowp unless a two-point-conical stage is needed */
    s->lowp = !(s->kind == SH_RADIAL && s->focal != 0);
    return 1;
}

int sv_shader_pattern(sv_shader *s, const sv_pixmap *pm, bool repeat, sv_quality q, float opacity, sv_ts ts)
{
    memset(s, 0, sizeof *s);
    s->kind = SH_PATTERN;
    s->pm = pm;
    s->repeat = repeat;
    s->quality = q;
    s->popacity = opacity;
    s->transform = ts;
    if (!inverse(ts, &s->inv)) {
        return 0;
    }
    if (ts_is_identity(s->inv) ||
        (s->inv.sx == 1.0f && s->inv.sy == 1.0f && s->inv.kx == 0.0f && s->inv.ky == 0.0f)) {
        s->quality = SQ_NEAREST;
    }
    return 1;
}

bool sv_shader_apply_opacity(sv_shader *s, float o)
{
    size_t i;
    o = mx(0.0f, mn(o, 1.0f));
    switch (s->kind) {
    case SH_SOLID:
        s->color[3] = mx(0.0f, mn(s->color[3] * o, 1.0f));
        return true;
    case SH_PATTERN:
        s->popacity = s->popacity * o;
        return true;
    default:
        /* Gradient::apply_opacity: every stop's alpha; the context is built afterwards */
        for (i = 0; i < s->nstops; i++) {
            s->stops[i].c[3] = mx(0.0f, mn(s->stops[i].c[3] * o, 1.0f));
        }
        return gradient_build(s);
    }
}

bool sv_shader_is_opaque(const sv_shader *s)
{
    switch (s->kind) {
    case SH_SOLID:
        return s->color[3] == 1.0f;
    case SH_LINEAR:
        return s->opaque;
    default:
        return false;
    }
}

/* ---- evaluation ---- */

static void grad_color_f(const sv_shader *s, float t, float out[4])
{
    int k;
    if (s->two_stop) {
        for (k = 0; k < 4; k++) {
            out[k] = t * s->factor[k] + s->bias[k];
        }
        return;
    }
    {
        size_t idx = 0;
        size_t i;
        for (i = 1; i < s->len; i++) {
            idx += t >= s->t_values[i] ? 1u : 0u;
        }
        for (k = 0; k < 4; k++) {
            out[k] = t * s->factors[idx][k] + s->biases[idx][k];
        }
    }
}

/* The gradient's t for device pixel centre (x, y); false when masked out (degenerate conical). */
static bool grad_t(const sv_shader *s, float x, float y, bool lowp, float *t)
{
    float u;
    float v;
    float r;
    map(&s->inv, x, y, &u, &v);
    r = u;
    if (s->kind == SH_RADIAL) {
        if (s->focal == 0) {
            r = sqrtf(u * u + v * v);
        } else if (s->focal == 1) {
            r = u + v * v / u;
        } else if (s->focal == 2) {
            r = sqrtf(u * u + v * v) - u * (1.0f / s->r1);
        } else {
            r = sqrtf(u * u - v * v) - u * (1.0f / s->r1);
        }
        if (s->focal != 0 && s->focal != 2) {
            if (r <= 0.0f || r != r) {
                *t = 0.0f;
                return false;
            }
        }
    }
    switch (s->spread) {
    case SPREAD_REFLECT:
        if (lowp) {
            r = normalize(fabsf((r - 1.0f) - 2.0f * floor16((r - 1.0f) * 0.5f) - 1.0f));
        } else {
            r = normalize(fabsf((r - 1.0f) - 2.0f * floor8((r - 1.0f) * 0.5f) - 1.0f));
        }
        break;
    case SPREAD_REPEAT:
        r = normalize(r - (lowp ? floor16(r) : floor8(r)));
        break;
    case SPREAD_PAD:
    default:
        if (s->uniform) {
            r = normalize(r);
        }
        break;
    }
    *t = r;
    return true;
}

/* Pattern sampling (highp gather / bilinear / bicubic). */
static void gather(const sv_pixmap *pm, float x, float y, float out[4])
{
    float w;
    float h;
    uint32_t bw;
    uint32_t bh;
    int32_t ix;
    int32_t iy;
    const uint8_t *p;
    memcpy(&bw, &(float){(float)pm->w}, 4);
    memcpy(&bh, &(float){(float)pm->h}, 4);
    bw -= 1;
    bh -= 1;
    memcpy(&w, &bw, 4);
    memcpy(&h, &bh, 4);
    x = mn(mx(x, 0.0f), w);
    y = mn(mx(y, 0.0f), h);
    ix = trunc_int(x);
    iy = trunc_int(y);
    if (ix < 0 || iy < 0 || (uint32_t)ix >= pm->w || (uint32_t)iy >= pm->h) {
        memset(out, 0, 4 * sizeof(float));
        return;
    }
    p = pm->px + ((size_t)iy * pm->w + (size_t)ix) * 4;
    out[0] = (float)p[0] * F255;
    out[1] = (float)p[1] * F255;
    out[2] = (float)p[2] * F255;
    out[3] = (float)p[3] * F255;
}

static float ex_repeat(float v, float limit, float inv)
{
    return v - floor8(v * inv) * limit;
}

static float tile(const sv_shader *s, float v, float limit)
{
    float inv = 1.0f / limit;
    return s->repeat ? ex_repeat(v, limit, inv) : v;
}

static float bicubic_near(float t)
{
    return t * (t * ((-21.0f / 18.0f) * t + 27.0f / 18.0f) + 9.0f / 18.0f) + 1.0f / 18.0f;
}

static float bicubic_far(float t)
{
    return (t * t) * ((7.0f / 18.0f) * t + (-6.0f / 18.0f));
}

static float fract8(float v)
{
    return v - floor8(v);
}

static void pattern_color(const sv_shader *s, float x, float y, float out[4])
{
    float u;
    float v;
    const sv_pixmap *pm = s->pm;
    map(&s->inv, x, y, &u, &v);
    if (s->quality == SQ_NEAREST) {
        if (s->repeat) {
            u = ex_repeat(u, (float)pm->w, 1.0f / (float)pm->w);
            v = ex_repeat(v, (float)pm->h, 1.0f / (float)pm->h);
        }
        gather(pm, u, v, out);
    } else {
        int n = s->quality == SQ_BILINEAR ? 2 : 4;
        float fx = fract8(u + 0.5f);
        float fy = fract8(v + 0.5f);
        float wx[4];
        float wy[4];
        float start = n == 2 ? -0.5f : -1.5f;
        float yy;
        int i;
        int j;
        int k;
        if (n == 2) {
            wx[0] = 1.0f - fx;
            wx[1] = fx;
            wy[0] = 1.0f - fy;
            wy[1] = fy;
        } else {
            wx[0] = bicubic_far(1.0f - fx);
            wx[1] = bicubic_near(1.0f - fx);
            wx[2] = bicubic_near(fx);
            wx[3] = bicubic_far(fx);
            wy[0] = bicubic_far(1.0f - fy);
            wy[1] = bicubic_near(1.0f - fy);
            wy[2] = bicubic_near(fy);
            wy[3] = bicubic_far(fy);
        }
        memset(out, 0, 4 * sizeof(float));
        yy = v + start;
        for (j = 0; j < n; j++) {
            float xx = u + start;
            for (i = 0; i < n; i++) {
                float c[4];
                float w = wx[i] * wy[j];
                gather(pm, tile(s, xx, (float)pm->w), tile(s, yy, (float)pm->h), c);
                for (k = 0; k < 4; k++) {
                    out[k] = w * c[k] + out[k];
                }
                xx += 1.0f;
            }
            yy += 1.0f;
        }
        if (n == 4) {
            for (k = 0; k < 4; k++) {
                out[k] = mn(mx(out[k], 0.0f), 1.0f);
            }
        }
    }
    if (s->popacity != 1.0f) {
        int k;
        for (k = 0; k < 4; k++) {
            out[k] *= s->popacity;
        }
    }
}

/* highp shader colour (premultiplied f32) at device pixel (x, y). */
static void shade_f(const sv_shader *s, int32_t x, int32_t y, float out[4])
{
    float fx = (float)x + 0.5f;
    float fy = (float)y + 0.5f;
    switch (s->kind) {
    case SH_SOLID: {
        uint16_t pu[4];
        solid_premul(s, out, pu);
        return;
    }
    case SH_PATTERN:
        pattern_color(s, fx, fy, out);
        return;
    default: {
        float t;
        bool keep = grad_t(s, fx, fy, false, &t);
        grad_color_f(s, t, out);
        if (!s->opaque) {
            out[0] *= out[3];
            out[1] *= out[3];
            out[2] *= out[3];
        }
        if (!keep) {
            memset(out, 0, 4 * sizeof(float));
        }
        return;
    }
    }
}

/* lowp shader colour (premultiplied 0..255) at device pixel (x, y). */
static void shade_u(const sv_shader *s, int32_t x, int32_t y, uint16_t out[4])
{
    float fx = (float)x + 0.5f;
    float fy = (float)y + 0.5f;
    if (s->kind == SH_SOLID) {
        float pf[4];
        solid_premul(s, pf, out);
        return;
    }
    {
        float t;
        float c[4];
        int k;
        (void)grad_t(s, fx, fy, true, &t);
        grad_color_f(s, t, c);
        for (k = 0; k < 3; k++) {
            out[k] = u16_sat(normalize(c[k]) * 255.0f + 0.5f);
        }
        out[3] = u16_sat(c[3] * 255.0f + 0.5f);
        if (!s->opaque) {
            for (k = 0; k < 3; k++) {
                out[k] = div255((uint32_t)out[k] * out[3]);
            }
        }
    }
}

/* ---------------------------------------------------------------- blending */

static bool pre_scale(sv_tsblend m)
{
    return m == TSB_DESTINATION || m == TSB_DESTINATION_OVER || m == TSB_PLUS || m == TSB_DESTINATION_OUT ||
           m == TSB_SOURCE_ATOP || m == TSB_SOURCE_OVER || m == TSB_XOR;
}

static bool lowp_blend(sv_tsblend m)
{
    switch (m) {
    case TSB_COLOR_BURN:
    case TSB_COLOR_DODGE:
    case TSB_SOFT_LIGHT:
    case TSB_HUE:
    case TSB_SATURATION:
    case TSB_COLOR:
    case TSB_LUMINOSITY:
        return false;
    default:
        return true;
    }
}

static uint16_t inv_u(uint16_t v)
{
    return (uint16_t)(255u - v);
}

/* lowp blend of premultiplied s onto d (both 0..255, u16 lanes with wrapping arithmetic). */
static void blend_u(sv_tsblend m, uint16_t s[4], const uint16_t d[4])
{
    int i;
    uint16_t sa = s[3];
    uint16_t da = d[3];
    uint16_t o[4];
    switch (m) {
    case TSB_CLEAR:
        memset(o, 0, sizeof o);
        break;
    case TSB_SOURCE:
        return;
    case TSB_DESTINATION:
        memcpy(o, d, sizeof o);
        break;
    case TSB_SOURCE_OVER:
        for (i = 0; i < 4; i++) {
            o[i] = (uint16_t)(s[i] + div255((uint32_t)d[i] * inv_u(sa)));
        }
        break;
    case TSB_DESTINATION_OVER:
        for (i = 0; i < 4; i++) {
            o[i] = (uint16_t)(d[i] + div255((uint32_t)s[i] * inv_u(da)));
        }
        break;
    case TSB_SOURCE_IN:
        for (i = 0; i < 4; i++) {
            o[i] = div255((uint32_t)s[i] * da);
        }
        break;
    case TSB_DESTINATION_IN:
        for (i = 0; i < 4; i++) {
            o[i] = div255((uint32_t)d[i] * sa);
        }
        break;
    case TSB_SOURCE_OUT:
        for (i = 0; i < 4; i++) {
            o[i] = div255((uint32_t)s[i] * inv_u(da));
        }
        break;
    case TSB_DESTINATION_OUT:
        for (i = 0; i < 4; i++) {
            o[i] = div255((uint32_t)d[i] * inv_u(sa));
        }
        break;
    case TSB_SOURCE_ATOP:
        for (i = 0; i < 4; i++) {
            o[i] = div255(((uint32_t)s[i] * da + (uint32_t)d[i] * inv_u(sa)) & 0xffffu);
        }
        break;
    case TSB_DESTINATION_ATOP:
        for (i = 0; i < 4; i++) {
            o[i] = div255(((uint32_t)d[i] * sa + (uint32_t)s[i] * inv_u(da)) & 0xffffu);
        }
        break;
    case TSB_XOR:
        for (i = 0; i < 4; i++) {
            o[i] = div255(((uint32_t)s[i] * inv_u(da) + (uint32_t)d[i] * inv_u(sa)) & 0xffffu);
        }
        break;
    case TSB_PLUS:
        for (i = 0; i < 4; i++) {
            uint16_t v = (uint16_t)(s[i] + d[i]);
            o[i] = v < 255 ? v : 255;
        }
        break;
    case TSB_MODULATE:
        for (i = 0; i < 4; i++) {
            o[i] = div255((uint32_t)s[i] * d[i]);
        }
        break;
    case TSB_SCREEN:
        for (i = 0; i < 4; i++) {
            o[i] = (uint16_t)(s[i] + d[i] - div255((uint32_t)s[i] * d[i]));
        }
        break;
    case TSB_MULTIPLY:
        for (i = 0; i < 4; i++) {
            o[i] = div255(((uint32_t)s[i] * inv_u(da) + (uint32_t)d[i] * inv_u(sa) + (uint32_t)s[i] * d[i]) &
                          0xffffu);
        }
        break;
    default: {
        /* blend_fn2: darken, lighten, exclusion, difference, hard light, overlay */
        for (i = 0; i < 3; i++) {
            uint32_t sd = ((uint32_t)s[i] * da) & 0xffffu;
            uint32_t ds = ((uint32_t)d[i] * sa) & 0xffffu;
            uint16_t v;
            switch (m) {
            case TSB_DARKEN:
                v = (uint16_t)(s[i] + d[i] - div255(sd > ds ? sd : ds));
                break;
            case TSB_LIGHTEN:
                v = (uint16_t)(s[i] + d[i] - div255(sd < ds ? sd : ds));
                break;
            case TSB_EXCLUSION:
                v = (uint16_t)(s[i] + d[i] - 2u * div255((uint32_t)s[i] * d[i]));
                break;
            case TSB_DIFFERENCE:
                v = (uint16_t)(s[i] + d[i] - 2u * div255(sd < ds ? sd : ds));
                break;
            case TSB_HARD_LIGHT:
            case TSB_OVERLAY: {
                bool c = m == TSB_HARD_LIGHT ? (uint16_t)(s[i] + s[i]) <= sa : (uint16_t)(d[i] + d[i]) <= da;
                uint16_t x =
                    c ? (uint16_t)(2u * s[i] * d[i])
                      : (uint16_t)((uint32_t)sa * da - 2u * (uint16_t)(sa - s[i]) * (uint16_t)(da - d[i]));
                v = div255((uint16_t)((uint32_t)s[i] * inv_u(da) + (uint32_t)d[i] * inv_u(sa) + x));
                break;
            }
            default:
                v = s[i];
                break;
            }
            o[i] = v;
        }
        o[3] = (uint16_t)(sa + div255((uint32_t)da * inv_u(sa)));
        break;
    }
    }
    memcpy(s, o, sizeof o);
}

static float two(float v)
{
    return v + v;
}

static float sat3(float r, float g, float b)
{
    return mx(r, mx(g, b)) - mn(r, mn(g, b));
}

static float lum3(float r, float g, float b)
{
    return r * 0.30f + g * 0.59f + b * 0.11f;
}

static void set_sat(float *r, float *g, float *b, float s)
{
    float lo = mn(*r, mn(*g, *b));
    float hi = mx(*r, mx(*g, *b));
    float sat = hi - lo;
    *r = sat == 0.0f ? 0.0f : (*r - lo) * s / sat;
    *g = sat == 0.0f ? 0.0f : (*g - lo) * s / sat;
    *b = sat == 0.0f ? 0.0f : (*b - lo) * s / sat;
}

static void set_lum(float *r, float *g, float *b, float l)
{
    float diff = l - lum3(*r, *g, *b);
    *r += diff;
    *g += diff;
    *b += diff;
}

static float clip1(float c, float lo, float hi, float l, float a)
{
    c = hi >= 0.0f ? c : l + (c - l) * l / (l - lo);
    c = hi > a ? l + (c - l) * (a - l) / (hi - l) : c;
    return mx(c, 0.0f);
}

static void clip_color(float *r, float *g, float *b, float a)
{
    float lo = mn(*r, mn(*g, *b));
    float hi = mx(*r, mx(*g, *b));
    float l = lum3(*r, *g, *b);
    *r = clip1(*r, lo, hi, l, a);
    *g = clip1(*g, lo, hi, l, a);
    *b = clip1(*b, lo, hi, l, a);
}

/* highp blend of premultiplied s onto d (f32). */
static void blend_f(sv_tsblend m, float s[4], const float d[4])
{
    int i;
    float sa = s[3];
    float da = d[3];
    float o[4];
    switch (m) {
    case TSB_CLEAR:
        memset(o, 0, sizeof o);
        break;
    case TSB_SOURCE:
        return;
    case TSB_DESTINATION:
        memcpy(o, d, sizeof o);
        break;
    case TSB_SOURCE_OVER:
        for (i = 0; i < 4; i++) {
            o[i] = d[i] * (1.0f - sa) + s[i];
        }
        break;
    case TSB_DESTINATION_OVER:
        for (i = 0; i < 4; i++) {
            o[i] = s[i] * (1.0f - da) + d[i];
        }
        break;
    case TSB_SOURCE_IN:
        for (i = 0; i < 4; i++) {
            o[i] = s[i] * da;
        }
        break;
    case TSB_DESTINATION_IN:
        for (i = 0; i < 4; i++) {
            o[i] = d[i] * sa;
        }
        break;
    case TSB_SOURCE_OUT:
        for (i = 0; i < 4; i++) {
            o[i] = s[i] * (1.0f - da);
        }
        break;
    case TSB_DESTINATION_OUT:
        for (i = 0; i < 4; i++) {
            o[i] = d[i] * (1.0f - sa);
        }
        break;
    case TSB_SOURCE_ATOP:
        for (i = 0; i < 4; i++) {
            o[i] = s[i] * da + d[i] * (1.0f - sa);
        }
        break;
    case TSB_DESTINATION_ATOP:
        for (i = 0; i < 4; i++) {
            o[i] = d[i] * sa + s[i] * (1.0f - da);
        }
        break;
    case TSB_XOR:
        for (i = 0; i < 4; i++) {
            o[i] = s[i] * (1.0f - da) + d[i] * (1.0f - sa);
        }
        break;
    case TSB_PLUS:
        for (i = 0; i < 4; i++) {
            o[i] = mn(s[i] + d[i], 1.0f);
        }
        break;
    case TSB_MODULATE:
        for (i = 0; i < 4; i++) {
            o[i] = s[i] * d[i];
        }
        break;
    case TSB_SCREEN:
        for (i = 0; i < 4; i++) {
            o[i] = s[i] + d[i] - s[i] * d[i];
        }
        break;
    case TSB_MULTIPLY:
        for (i = 0; i < 4; i++) {
            o[i] = s[i] * (1.0f - da) + d[i] * (1.0f - sa) + s[i] * d[i];
        }
        break;
    case TSB_HUE:
    case TSB_SATURATION:
    case TSB_COLOR:
    case TSB_LUMINOSITY: {
        float r = s[0];
        float g = s[1];
        float b = s[2];
        float a = sa;
        float dr = d[0];
        float dg = d[1];
        float db = d[2];
        float rr;
        float gg;
        float bb;
        if (m == TSB_HUE) {
            rr = r * a;
            gg = g * a;
            bb = b * a;
            set_sat(&rr, &gg, &bb, sat3(dr, dg, db) * a);
            set_lum(&rr, &gg, &bb, lum3(dr, dg, db) * a);
        } else if (m == TSB_SATURATION) {
            rr = dr * a;
            gg = dg * a;
            bb = db * a;
            set_sat(&rr, &gg, &bb, sat3(r, g, b) * da);
            set_lum(&rr, &gg, &bb, lum3(dr, dg, db) * a);
        } else if (m == TSB_COLOR) {
            rr = r * da;
            gg = g * da;
            bb = b * da;
            set_lum(&rr, &gg, &bb, lum3(dr, dg, db) * a);
        } else {
            rr = dr * a;
            gg = dg * a;
            bb = db * a;
            set_lum(&rr, &gg, &bb, lum3(r, g, b) * da);
        }
        clip_color(&rr, &gg, &bb, a * da);
        o[0] = r * (1.0f - da) + dr * (1.0f - a) + rr;
        o[1] = g * (1.0f - da) + dg * (1.0f - a) + gg;
        o[2] = b * (1.0f - da) + db * (1.0f - a) + bb;
        o[3] = a + da - a * da;
        break;
    }
    default:
        for (i = 0; i < 3; i++) {
            float sc = s[i];
            float dc = d[i];
            float v;
            switch (m) {
            case TSB_DARKEN:
                v = sc + dc - mx(sc * da, dc * sa);
                break;
            case TSB_LIGHTEN:
                v = sc + dc - mn(sc * da, dc * sa);
                break;
            case TSB_DIFFERENCE:
                v = sc + dc - two(mn(sc * da, dc * sa));
                break;
            case TSB_EXCLUSION:
                v = sc + dc - two(sc * dc);
                break;
            case TSB_COLOR_BURN:
                v = dc == da     ? dc + sc * (1.0f - da)
                    : sc == 0.0f ? dc * (1.0f - sa)
                                 : sa * (da - mn(da, (da - dc) * sa * (1.0f / sc))) + sc * (1.0f - da) +
                                       dc * (1.0f - sa);
                break;
            case TSB_COLOR_DODGE:
                v = dc == 0.0f ? sc * (1.0f - da)
                    : sc == sa
                        ? sc + dc * (1.0f - sa)
                        : sa * mn(da, (dc * sa) * (1.0f / (sa - sc))) + sc * (1.0f - da) + dc * (1.0f - sa);
                break;
            case TSB_HARD_LIGHT:
                v = sc * (1.0f - da) + dc * (1.0f - sa) +
                    (two(sc) <= sa ? two(sc * dc) : sa * da - two((da - dc) * (sa - sc)));
                break;
            case TSB_OVERLAY:
                v = sc * (1.0f - da) + dc * (1.0f - sa) +
                    (two(dc) <= da ? two(sc * dc) : sa * da - two((da - dc) * (sa - sc)));
                break;
            case TSB_SOFT_LIGHT: {
                float mm = da > 0.0f ? dc / da : 0.0f;
                float s2 = two(sc);
                float m4 = two(two(mm));
                float dark_src = dc * (sa + (s2 - sa) * (1.0f - mm));
                float dark_dst = (m4 * m4 + m4) * (mm - 1.0f) + 7.0f * mm;
                float lite_dst = sqrtf(mm) - mm;
                float lite_src = dc * sa + da * (s2 - sa) * (two(two(dc)) <= da ? dark_dst : lite_dst);
                v = sc * (1.0f - da) + dc * (1.0f - sa) + (s2 <= sa ? dark_src : lite_src);
                break;
            }
            default:
                v = sc;
                break;
            }
            o[i] = v;
        }
        o[3] = d[3] * (1.0f - sa) + sa;
        break;
    }
    memcpy(s, o, sizeof o);
}

/* ---------------------------------------------------------------- blitting */

/* One pixel of the blitter: shader colour at (x, y), coverage c (1..255), blend mode m. */
static void blit_px(uint8_t *d8, int32_t x, int32_t y, uint8_t c, const sv_shader *s, sv_tsblend m, bool lowp,
                    bool source)
{
    sv_tsblend mode = source ? TSB_SOURCE : m;
    if (lowp) {
        uint16_t sc[4];
        uint16_t d[4];
        int i;
        shade_u(s, x, y, sc);
        for (i = 0; i < 4; i++) {
            d[i] = d8[i];
        }
        if (pre_scale(mode)) {
            if (c != 255) {
                for (i = 0; i < 4; i++) {
                    sc[i] = div255((uint32_t)sc[i] * c);
                }
            }
            blend_u(mode, sc, d);
        } else {
            blend_u(mode, sc, d);
            if (c != 255) {
                for (i = 0; i < 4; i++) {
                    sc[i] = div255(((uint32_t)d[i] * (255u - c) + (uint32_t)sc[i] * c) & 0xffffu);
                }
            }
        }
        for (i = 0; i < 4; i++) {
            d8[i] = (uint8_t)sc[i];
        }
    } else {
        float sc[4];
        float d[4];
        float cf = (float)c * F255;
        int i;
        shade_f(s, x, y, sc);
        for (i = 0; i < 4; i++) {
            d[i] = (float)d8[i] * F255;
        }
        if (pre_scale(mode)) {
            if (c != 255) {
                for (i = 0; i < 4; i++) {
                    sc[i] *= cf;
                }
            }
            blend_f(mode, sc, d);
        } else {
            blend_f(mode, sc, d);
            if (c != 255) {
                for (i = 0; i < 4; i++) {
                    sc[i] = (sc[i] - d[i]) * cf + d[i];
                }
            }
        }
        for (i = 0; i < 4; i++) {
            d8[i] = unnorm(sc[i]);
        }
    }
}

static bool use_lowp(const sv_shader *s, sv_tsblend m)
{
    if (s->kind == SH_PATTERN) {
        return false;
    }
    if (s->kind != SH_SOLID && !s->lowp) {
        return false;
    }
    return lowp_blend(m);
}

void sv_blit_rect(sv_pixmap *pm, int64_t x0, int64_t y0, int64_t x1, int64_t y1, const sv_shader *s,
                  sv_tsblend m)
{
    bool lowp = use_lowp(s, m);
    bool source = m == TSB_SOURCE_OVER && sv_shader_is_opaque(s);
    int64_t x;
    int64_t y;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > (int64_t)pm->w ? (int64_t)pm->w : x1;
    y1 = y1 > (int64_t)pm->h ? (int64_t)pm->h : y1;
    if (m == TSB_DESTINATION) {
        return;
    }
    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            blit_px(pm->px + ((size_t)y * pm->w + (size_t)x) * 4, (int32_t)x, (int32_t)y, 255, s, m, lowp,
                    source);
        }
    }
}

void sv_draw_pixmap(sv_pixmap *dst, int32_t x, int32_t y, const sv_pixmap *src, float opacity, sv_tsblend m)
{
    sv_shader s;
    if (src->w == 0 || src->h == 0) {
        return;
    }
    (void)sv_shader_pattern(&s, src, false, SQ_NEAREST, opacity, ts_translate((float)x, (float)y));
    sv_blit_rect(dst, (int64_t)x, (int64_t)y, (int64_t)x + src->w, (int64_t)y + src->h, &s, m);
}

/* ---------------------------------------------------------------- masks */

void sv_mask_from_pixmap(const sv_pixmap *pm, bool luminance, uint8_t *out)
{
    size_t i;
    size_t n = (size_t)pm->w * pm->h;
    for (i = 0; i < n; i++) {
        const uint8_t *p = pm->px + i * 4;
        if (!luminance) {
            out[i] = p[3];
        } else {
            float r = (float)p[0] / 255.0f;
            float g = (float)p[1] / 255.0f;
            float b = (float)p[2] / 255.0f;
            float a = (float)p[3] / 255.0f;
            float luma;
            float v;
            if (p[3] != 0) {
                r /= a;
                g /= a;
                b /= a;
            }
            luma = r * 0.2126f + g * 0.7152f + b * 0.0722f;
            v = (luma * a) * 255.0f;
            v = v > 0.0f ? (v < 255.0f ? v : 255.0f) : 0.0f;
            out[i] = (uint8_t)ceilf(v);
        }
    }
}

void sv_pixmap_apply_mask(sv_pixmap *pm, const uint8_t *mask)
{
    size_t i;
    size_t n = (size_t)pm->w * pm->h;
    for (i = 0; i < n; i++) {
        uint8_t *d = pm->px + i * 4;
        int k;
        for (k = 0; k < 4; k++) {
            d[k] = (uint8_t)div255((uint32_t)d[k] * mask[i]);
        }
    }
}

uint8_t sv_opacity_u8(float o)
{
    float v = o * 255.0f + 0.5f;
    return v > 0.0f ? (v < 255.0f ? (uint8_t)v : 255) : 0;
}

/* ---------------------------------------------------------------- spans (I5b raster) */

/* sv_spans sink for RasterPipelineBlitter: every span blends pixel by pixel through blit_px
 * (alpha 255 = full coverage, as blit_h / blit_rect; otherwise the coverage stage of
 * blit_anti_h / blit_mask); pixels outside the pixmap are dropped. */
static void pipe_span(void *ctx, uint32_t x, uint32_t y, uint32_t w, uint8_t a)
{
    const sv_pipe_spans *p = ctx;
    uint64_t end = (uint64_t)x + w;
    uint32_t px;
    if (a == 0 || y >= p->pm->h || x >= p->pm->w) {
        return;
    }
    if (end > p->pm->w) {
        end = p->pm->w;
    }
    for (px = x; px < end; px++) {
        blit_px(p->pm->px + ((size_t)y * p->pm->w + px) * 4, (int32_t)px, (int32_t)y, a, p->s, p->m, p->lowp,
                p->source);
    }
}

bool sv_pipe_spans_init(sv_pipe_spans *ps, sv_pixmap *pm, const sv_shader *s, sv_tsblend m, sv_spans *out)
{
    if (m == TSB_DESTINATION || (m == TSB_DESTINATION_IN && s->kind == SH_SOLID && sv_shader_is_opaque(s))) {
        return false;
    }
    ps->pm = pm;
    ps->s = s;
    ps->m = m;
    ps->lowp = use_lowp(s, m);
    ps->source = m == TSB_SOURCE_OVER && sv_shader_is_opaque(s);
    out->span = pipe_span;
    out->ctx = ps;
    return true;
}
