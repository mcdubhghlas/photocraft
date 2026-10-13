/* Camera Raw's Shadows, Highlights, Clarity, Texture and Dehaze on raw files, applied while
 * developing (in linear light, before the tone curve), and its adaptive black point (port of
 * crates/raw/src/tone.rs; see it for how each was measured against Photoshop 25.4).
 *
 * Shadows / Highlights: log2 of the gain is slider / 100 * C(B - anchor), B an edge-preserving base
 * of the log2 luminance (K. He's guided filter, radius 32, eps 1 stop^2, on a copy 1232 px on its
 * long side), the anchor the log2 power mean (p = 1.5) for Shadows and the mean log2 luminance for
 * Highlights. Clarity: bands of log2 luminance between guided bases (radii 2, 8, 32, 128), each
 * scaled by a gain following its base's tone. Texture: bands between Gaussians on a half-size copy,
 * each through its measured response. The change is carried to full size by guided upsampling.
 * Dehaze: He, Sun and Tang's dark channel prior (2009). The black point: a factor times the 0.1th
 * percentile of the linear luminance, subtracted before the gain.
 *
 * Float operations follow the reference one for one (sum orders, f32 vs f64, Rust's min/max) so the
 * results are bit-identical. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"
#include "raw/internal.h"
#include "raw/tone_tables.h"

/* Long side of the work copy, the base's guided-filter radius and eps there. */
#define WORK_SIDE 1232
#define BASE_RADIUS 32.0f
#define BASE_EPS 1.0f
#define UPSAMPLE_EPS 1.0f
/* The Shadows / Highlights curves' first knot (stops from the anchor) and knot spacing. */
#define TONE_LO (-12.0f)
#define TONE_STEP 0.25f
static const float CLARITY_RADII[4] = {2.0f, 8.0f, 32.0f, 128.0f};
#define CLARITY_EPS 1.0f
#define TEXTURE_SIDE 2464
static const float TEXTURE_SIGMAS[6] = {0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f};
#define TEXTURE_UPSAMPLE_EPS 1e-4f
#define HAZE_RADIUS 8.0f
#define HAZE_SMOOTH 64.0f
#define HAZE_EPS 1e-3f
#define HAZE_T0 0.1f
#define HAZE_UP 0.8f
#define HAZE_DOWN 0.22f
/* Linear ProPhoto -> linear Display P3 (both D50): the dark channel was measured in Photoshop's
 * Display P3 output. */
static const float PRO_TO_P3[3][3] = {{1.632638f, -0.379816f, -0.252821f},
                                      {-0.153846f, 1.166859f, -0.013013f},
                                      {0.010511f, -0.062886f, 1.052375f}};
/* 2^-20, the floor of the 0.1th percentile. */
#define P01_FLOOR 9.5367431640625e-07f

bool pc_raw_tone_is_neutral(const pc_raw_tone *t)
{
    return t->shadows == 0.0f && t->highlights == 0.0f && t->clarity == 0.0f && t->texture == 0.0f &&
           t->dehaze == 0.0f;
}

static bool work_neutral(const pc_raw_tone *t)
{
    return t->shadows == 0.0f && t->highlights == 0.0f && t->clarity == 0.0f;
}

static bool gain_neutral(const pc_raw_tone *t)
{
    return work_neutral(t) && t->texture == 0.0f;
}

static size_t div_ceil(size_t a, size_t b)
{
    return a / b + (a % b != 0);
}

/* f32 round-then-saturate to usize, at least 1 (a filter radius). */
static size_t radius(float r)
{
    size_t v = pc_f32_to_size(roundf(r));
    return v < 1 ? 1 : v;
}

/* ---- reduce: box means over k x k cells ---- */

typedef struct reduce_ctx {
    size_t w, h, k, sw, nch;
    rx_px_fn f;
    const void *fctx;
    float *out;
} reduce_ctx;

static pc_status reduce_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    reduce_ctx *c = ctx;
    size_t sy;
    (void)chunk;
    for (sy = begin; sy < end; sy++) {
        size_t y0 = sy * c->k;
        size_t y1 = (sy + 1) * c->k < c->h ? (sy + 1) * c->k : c->h;
        size_t sx;
        for (sx = 0; sx < c->sw; sx++) {
            size_t x0 = sx * c->k;
            size_t x1 = (sx + 1) * c->k < c->w ? (sx + 1) * c->k : c->w;
            double s[3] = {0.0, 0.0, 0.0};
            double n = (double)((x1 - x0) * (y1 - y0));
            size_t x;
            size_t y;
            size_t ch;
            for (y = y0; y < y1; y++) {
                for (x = x0; x < x1; x++) {
                    float v[3] = {0.0f, 0.0f, 0.0f};
                    c->f(c->fctx, y * c->w + x, v);
                    for (ch = 0; ch < c->nch; ch++) {
                        s[ch] += (double)pc_rs_maxf_k(v[ch], 0.0f);
                    }
                }
            }
            for (ch = 0; ch < c->nch; ch++) {
                c->out[(sy * c->sw + sx) * c->nch + ch] = (float)(s[ch] / n);
            }
        }
    }
    return PC_OK;
}

/* Box means of f over k x k cells, k the smallest factor that brings the long side to side or less.
 * NULL on out of memory. */
static float *reduce(size_t w, size_t h, size_t side, size_t nch, rx_px_fn f, const void *fctx, size_t *sw,
                     size_t *sh)
{
    reduce_ctx c;
    size_t k = div_ceil(w > h ? w : h, side);
    k = k < 1 ? 1 : k;
    c.w = w;
    c.h = h;
    c.k = k;
    c.sw = div_ceil(w, k);
    c.nch = nch;
    c.f = f;
    c.fctx = fctx;
    *sw = c.sw;
    *sh = div_ceil(h, k);
    c.out = pc_malloc_array(c.sw * *sh * nch, sizeof *c.out);
    if (c.out != NULL) {
        (void)pc_pool_for(pc_pool_global(), *sh, 1, reduce_rows, &c);
    }
    return c.out;
}

/* ---- order statistics ---- */

/* f32::total_cmp as a "less" on the values behind two indices. */
static bool total_less_f(float a, float b)
{
    int32_t ia;
    int32_t ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    ia ^= (int32_t)((uint32_t)(ia >> 31) >> 1);
    ib ^= (int32_t)((uint32_t)(ib >> 31) >> 1);
    return ia < ib;
}

static bool less_by_value(void *ctx, size_t a, size_t b)
{
    const float *v = ctx;
    return total_less_f(v[a], v[b]);
}

/* The q quantile of v (n >= 1); false on out of memory. */
static bool quantile(const float *v, size_t n, float q, float *out)
{
    size_t *idx = pc_malloc_array(n, sizeof *idx);
    size_t k = pc_f32_to_size(roundf((float)(n - 1) * q));
    size_t i;
    if (idx == NULL) {
        return false;
    }
    for (i = 0; i < n; i++) {
        idx[i] = i;
    }
    k = k < n ? k : n - 1;
    rx_select_nth(idx, n, k, less_by_value, (void *)(uintptr_t)v);
    *out = v[idx[k]];
    free(idx);
    return true;
}

/* ---- box mean, guided filter, Gaussian, minimum filter ---- */

/* Index i mirrored into 0..n (half-sample symmetric). */
static size_t mirror(ptrdiff_t i, size_t n)
{
    ptrdiff_t m = (ptrdiff_t)n;
    ptrdiff_t p = ((i % (2 * m)) + 2 * m) % (2 * m);
    return (size_t)(p < m ? p : 2 * m - 1 - p);
}

typedef struct sep_ctx {
    const float *src;
    float *dst;
    size_t w, h, r;
    const float *kernel; /* Gaussian taps (2r + 1), or NULL for the box mean / min filter */
    int kind;            /* 0 box, 1 gauss, 2 min */
} sep_ctx;

/* Rows: dst row y from src row y. */
static pc_status sep_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    sep_ctx *c = ctx;
    ptrdiff_t ri = (ptrdiff_t)c->r;
    size_t y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        const float *src = c->src + y * c->w;
        float *dst = c->dst + y * c->w;
        size_t x;
        if (c->kind == 0) {
            double k = (double)(2 * c->r + 1);
            double acc = -0.0;
            ptrdiff_t i;
            for (i = -ri; i <= ri; i++) {
                acc += (double)src[mirror(i, c->w)];
            }
            for (x = 0; x < c->w; x++) {
                dst[x] = (float)(acc / k);
                acc += (double)src[mirror((ptrdiff_t)x + ri + 1, c->w)] -
                       (double)src[mirror((ptrdiff_t)x - ri, c->w)];
            }
        } else if (c->kind == 1) {
            for (x = 0; x < c->w; x++) {
                float s = -0.0f;
                size_t j;
                for (j = 0; j <= 2 * c->r; j++) {
                    s += c->kernel[j] * src[mirror((ptrdiff_t)x + (ptrdiff_t)j - ri, c->w)];
                }
                dst[x] = s;
            }
        } else {
            for (x = 0; x < c->w; x++) {
                size_t lo = x > c->r ? x - c->r : 0;
                size_t hi = x + c->r < c->w - 1 ? x + c->r : c->w - 1;
                float m = 3.40282347e38f;
                size_t i;
                for (i = lo; i <= hi; i++) {
                    m = pc_rs_minf(m, src[i]);
                }
                dst[x] = m;
            }
        }
    }
    return PC_OK;
}

/* Columns: dst column x from src column x. */
static pc_status sep_cols(void *ctx, size_t chunk, size_t begin, size_t end)
{
    sep_ctx *c = ctx;
    ptrdiff_t ri = (ptrdiff_t)c->r;
    size_t x;
    (void)chunk;
    for (x = begin; x < end; x++) {
        size_t y;
#define AT(yy) c->src[mirror((yy), c->h) * c->w + x]
        if (c->kind == 0) {
            double k = (double)(2 * c->r + 1);
            double acc = -0.0;
            ptrdiff_t i;
            for (i = -ri; i <= ri; i++) {
                acc += (double)AT(i);
            }
            for (y = 0; y < c->h; y++) {
                c->dst[y * c->w + x] = (float)(acc / k);
                acc += (double)AT((ptrdiff_t)y + ri + 1) - (double)AT((ptrdiff_t)y - ri);
            }
        } else if (c->kind == 1) {
            for (y = 0; y < c->h; y++) {
                float s = -0.0f;
                size_t j;
                for (j = 0; j <= 2 * c->r; j++) {
                    s += c->kernel[j] * AT((ptrdiff_t)y + (ptrdiff_t)j - ri);
                }
                c->dst[y * c->w + x] = s;
            }
        } else {
            for (y = 0; y < c->h; y++) {
                size_t lo = y > c->r ? y - c->r : 0;
                size_t hi = y + c->r < c->h - 1 ? y + c->r : c->h - 1;
                float m = 3.40282347e38f;
                size_t i;
                for (i = lo; i <= hi; i++) {
                    m = pc_rs_minf(m, c->src[i * c->w + x]);
                }
                c->dst[y * c->w + x] = m;
            }
        }
#undef AT
    }
    return PC_OK;
}

/* A separable filter (rows, then columns) into a new buffer; NULL on out of memory. */
static float *separable(const float *v, size_t w, size_t h, size_t r, const float *kernel, int kind)
{
    sep_ctx c;
    float *tmp = pc_malloc_array(w * h, sizeof *tmp);
    float *out = pc_malloc_array(w * h, sizeof *out);
    if (tmp == NULL || out == NULL) {
        free(tmp);
        free(out);
        return NULL;
    }
    c.w = w;
    c.h = h;
    c.r = r;
    c.kernel = kernel;
    c.kind = kind;
    c.src = v;
    c.dst = tmp;
    (void)pc_pool_for(pc_pool_global(), h, 1, sep_rows, &c);
    c.src = tmp;
    c.dst = out;
    (void)pc_pool_for(pc_pool_global(), w, 1, sep_cols, &c);
    free(tmp);
    return out;
}

static float *box_mean(const float *v, size_t w, size_t h, size_t r)
{
    return separable(v, w, h, r, NULL, 0);
}

/* Gaussian blur, sigma, radius ceil(3 sigma), mirrored edges. */
static float *gauss(const float *v, size_t w, size_t h, float sigma)
{
    float *k;
    float *out;
    ptrdiff_t r;
    ptrdiff_t x;
    float sum = -0.0f;
    if (sigma <= 0.05f) {
        out = pc_malloc_array(w * h, sizeof *out);
        if (out != NULL) {
            memcpy(out, v, w * h * sizeof *out);
        }
        return out;
    }
    r = (ptrdiff_t)pc_f32_to_i64(ceilf(3.0f * sigma));
    k = pc_malloc_array((size_t)(2 * r + 1), sizeof *k);
    if (k == NULL) {
        return NULL;
    }
    for (x = -r; x <= r; x++) {
        k[x + r] = expf((float)(-(x * x)) / (2.0f * sigma * sigma));
    }
    for (x = 0; x <= 2 * r; x++) {
        sum += k[x];
    }
    for (x = 0; x <= 2 * r; x++) {
        k[x] /= sum;
    }
    out = separable(v, w, h, (size_t)r, k, 1);
    free(k);
    return out;
}

/* Local linear fit p ~ a * i + b over boxes of radius r (regularized by eps), with a and b
 * box-averaged (He, Sun, Tang, Guided Image Filtering, 2010). */
static bool linear_fit(const float *i, const float *p, size_t w, size_t h, size_t r, float eps, float **a_out,
                       float **b_out)
{
    size_t n = w * h;
    float *ip = pc_malloc_array(n, sizeof *ip);
    float *ii = pc_malloc_array(n, sizeof *ii);
    float *mi = NULL;
    float *mp = NULL;
    float *mip = NULL;
    float *mii = NULL;
    float *a = NULL;
    float *b = NULL;
    bool ok = false;
    size_t k;
    *a_out = NULL;
    *b_out = NULL;
    if (ip == NULL || ii == NULL) {
        goto done;
    }
    for (k = 0; k < n; k++) {
        ip[k] = i[k] * p[k];
        ii[k] = i[k] * i[k];
    }
    if ((mi = box_mean(i, w, h, r)) == NULL || (mp = box_mean(p, w, h, r)) == NULL ||
        (mip = box_mean(ip, w, h, r)) == NULL || (mii = box_mean(ii, w, h, r)) == NULL) {
        goto done;
    }
    /* Reuse ip / ii as a / b. */
    for (k = 0; k < n; k++) {
        ip[k] = (mip[k] - mi[k] * mp[k]) / (pc_rs_maxf_k(mii[k] - mi[k] * mi[k], 0.0f) + eps);
        ii[k] = mp[k] - ip[k] * mi[k];
    }
    if ((a = box_mean(ip, w, h, r)) == NULL || (b = box_mean(ii, w, h, r)) == NULL) {
        free(a);
        a = NULL;
        goto done;
    }
    *a_out = a;
    *b_out = b;
    ok = true;
done:
    free(ip);
    free(ii);
    free(mi);
    free(mp);
    free(mip);
    free(mii);
    return ok;
}

/* Guided filter of p with guide i; NULL on out of memory. */
static float *guided(const float *i, const float *p, size_t w, size_t h, size_t r, float eps)
{
    float *a;
    float *b;
    size_t k;
    if (!linear_fit(i, p, w, h, r, eps, &a, &b)) {
        return NULL;
    }
    for (k = 0; k < w * h; k++) {
        a[k] = a[k] * i[k] + b[k];
    }
    free(b);
    return a;
}

/* ---- curves ---- */

static float curve73(const float c[73], float d)
{
    float t = pc_clampf((d - TONE_LO) / TONE_STEP, 0.0f, 72.0f);
    size_t i = pc_f32_to_size(t);
    float f;
    i = i < 71 ? i : 71;
    f = t - (float)i;
    return c[i] + (c[i + 1] - c[i]) * f;
}

static float shadows(float s, float d)
{
    s = pc_clampf(s, -100.0f, 100.0f) / 100.0f;
    return s > 0.0f ? s * curve73(SHADOWS_UP, d) : s < 0.0f ? -s * curve73(SHADOWS_DOWN, d) : 0.0f;
}

static float highlights(float h, float d)
{
    h = pc_clampf(h, -100.0f, 100.0f) / 100.0f;
    return h > 0.0f ? h * curve73(HIGHLIGHTS_UP, d) : h < 0.0f ? -h * curve73(HIGHLIGHTS_DOWN, d) : 0.0f;
}

/* A table on evenly spaced knots lo, lo + step, ..., read linearly, flat beyond its ends. */
static float even(const float *c, size_t n, float lo, float step, float x)
{
    float t = pc_clampf((x - lo) / step, 0.0f, (float)(n - 1));
    size_t i = pc_f32_to_size(t);
    i = i < n - 2 ? i : n - 2;
    return c[i] + (c[i + 1] - c[i]) * (t - (float)i);
}

/* Texture's band response on TEXTURE_KNOTS, read linearly, flat beyond. */
static float knots(const float c[11], float d)
{
    const float *k = TEXTURE_KNOTS;
    size_t i = 0;
    float f;
    d = pc_clampf(d, k[0], k[10]);
    while (i < 11 && k[i] <= d) {
        i++;
    }
    i = i < 1 ? 1 : i > 10 ? 10 : i;
    f = (d - k[i - 1]) / (k[i] - k[i - 1]);
    return c[i - 1] + (c[i] - c[i - 1]) * f;
}

/* ---- the luminance change ---- */

static float bilinear(const float *v, size_t sw, size_t sh, size_t w, size_t h, size_t x, size_t y)
{
    float sx = pc_clampf(((float)x + 0.5f) * (float)sw / (float)w - 0.5f, 0.0f, (float)(sw - 1));
    float sy = pc_clampf(((float)y + 0.5f) * (float)sh / (float)h - 0.5f, 0.0f, (float)(sh - 1));
    size_t x0 = pc_f32_to_size(sx);
    size_t y0 = pc_f32_to_size(sy);
    size_t x1 = x0 + 1 < sw - 1 ? x0 + 1 : sw - 1;
    size_t y1 = y0 + 1 < sh - 1 ? y0 + 1 : sh - 1;
    float fx = sx - (float)x0;
    float fy = sy - (float)y0;
    float top = v[y0 * sw + x0] * (1.0f - fx) + v[y0 * sw + x1] * fx;
    float bottom = v[y1 * sw + x0] * (1.0f - fx) + v[y1 * sw + x1] * fx;
    return top * (1.0f - fy) + bottom * fy;
}

static float up_change(const rx_upsampled *u, size_t w, size_t h, size_t x, size_t y, float lum)
{
    float l = lum > 0.0f ? pc_rs_maxf(log2f(lum), u->floor) : u->floor;
    return bilinear(u->a, u->sw, u->sh, w, h, x, y) * l + bilinear(u->b, u->sw, u->sh, w, h, x, y);
}

static void up_fini(rx_upsampled *u)
{
    free(u->a);
    free(u->b);
    u->a = NULL;
    u->b = NULL;
}

/* Clarity s added to change: bands of l between guided bases, each scaled by a gain following its
 * base's tone (relative to geo), and a tone term of the coarsest base. */
static bool clarity(float *change, const float *l, size_t sw, size_t sh, float geo, float scale, float s)
{
    float k = pc_clampf(s, -100.0f, 100.0f) / 100.0f;
    const clarity_table *t = k >= 0.0f ? &CLARITY_UP : &CLARITY_DOWN;
    size_t n = sw * sh;
    float *prev = pc_malloc_array(n, sizeof *prev);
    size_t band;
    size_t i;
    k = k >= 0.0f ? k : -k;
    if (prev == NULL) {
        return false;
    }
    memcpy(prev, l, n * sizeof *prev);
    for (band = 0; band < 4; band++) {
        float *base = guided(l, l, sw, sh, radius(CLARITY_RADII[band] * scale), CLARITY_EPS);
        if (base == NULL) {
            free(prev);
            return false;
        }
        for (i = 0; i < n; i++) {
            change[i] += k * (prev[i] - base[i]) *
                         even(t->bands[band], 7, CLARITY_TONE_LO, CLARITY_TONE_STEP, base[i] - geo);
        }
        free(prev);
        prev = base;
    }
    for (i = 0; i < n; i++) {
        change[i] += k * (even(t->base, 13, CLARITY_BASE_LO, CLARITY_BASE_STEP, prev[i] - geo) + t->offset);
    }
    free(prev);
    return true;
}

/* log2 of each reduced value, floored at the 0.1th percentile; NULL on out of memory. */
static float *log_floor(const float *y, size_t n, float *p01)
{
    float *l;
    size_t i;
    float q;
    if (!quantile(y, n, 0.001f, &q)) {
        return NULL;
    }
    *p01 = pc_rs_maxf_k(q, P01_FLOOR);
    l = pc_malloc_array(n, sizeof *l);
    if (l != NULL) {
        for (i = 0; i < n; i++) {
            l[i] = log2f(pc_rs_maxf(y[i], *p01));
        }
    }
    return l;
}

/* Texture s on a half-size copy: each Gaussian band of log2 luminance through its measured
 * response, carried to full size. */
static bool texture(size_t w, size_t h, rx_px_fn lum, const void *ctx, float s, rx_upsampled *out)
{
    size_t sw;
    size_t sh;
    float *y = reduce(w, h, TEXTURE_SIDE, 1, lum, ctx, &sw, &sh);
    float *l = NULL;
    float *change = NULL;
    float *prev = NULL;
    float p01 = 0.0f;
    float k = pc_clampf(s, -100.0f, 100.0f) / 100.0f;
    const float (*t)[11] = k >= 0.0f ? TEXTURE_UP : TEXTURE_DOWN;
    float scale;
    size_t n;
    size_t band;
    size_t i;
    bool ok = false;
    k = k >= 0.0f ? k : -k;
    if (y == NULL) {
        return false;
    }
    n = sw * sh;
    scale = (float)(sw > sh ? sw : sh) / (float)TEXTURE_SIDE;
    if ((l = log_floor(y, n, &p01)) == NULL || (change = pc_calloc_array(n, sizeof *change)) == NULL ||
        (prev = pc_malloc_array(n, sizeof *prev)) == NULL) {
        goto done;
    }
    memcpy(prev, l, n * sizeof *prev);
    for (band = 0; band < 6; band++) {
        float *base = gauss(l, sw, sh, TEXTURE_SIGMAS[band] * scale);
        float zero = knots(t[band], 0.0f);
        if (base == NULL) {
            goto done;
        }
        for (i = 0; i < n; i++) {
            change[i] += k * (knots(t[band], prev[i] - base[i]) - zero);
        }
        free(prev);
        prev = base;
    }
    if (!linear_fit(l, change, sw, sh, 2, TEXTURE_UPSAMPLE_EPS, &out->a, &out->b)) {
        goto done;
    }
    out->sw = sw;
    out->sh = sh;
    out->floor = log2f(p01);
    ok = true;
done:
    free(y);
    free(l);
    free(change);
    free(prev);
    return ok;
}

void rx_tonemap_fini(rx_tonemap *m)
{
    up_fini(&m->work);
    up_fini(&m->texture);
    m->has_work = false;
    m->has_texture = false;
}

pc_status rx_tonemap_new(size_t w, size_t h, rx_px_fn lum, const void *ctx, const pc_raw_tone *tone,
                         float black_point, rx_tonemap *out, bool *some, pc_error *err)
{
    rx_tonemap m;
    size_t sw;
    size_t sh;
    size_t n;
    size_t i;
    float *y = NULL;
    float *l = NULL;
    float *change = NULL;
    float p01 = 0.0f;
    float q;
    memset(&m, 0, sizeof m);
    *some = false;
    if (w == 0 || h == 0 || (gain_neutral(tone) && black_point <= 0.0f)) {
        return PC_OK;
    }
    m.w = w;
    m.h = h;
    if ((y = reduce(w, h, WORK_SIDE, 1, lum, ctx, &sw, &sh)) == NULL) {
        goto nomem;
    }
    n = sw * sh;
    if (!quantile(y, n, 0.001f, &q)) {
        goto nomem;
    }
    p01 = pc_rs_maxf_k(q, P01_FLOOR);
    m.black = black_point > 0.0f ? black_point * p01 : 0.0f;
    if (!work_neutral(tone)) {
        double sum = -0.0;
        float geo;
        float scale = (float)(sw > sh ? sw : sh) / (float)WORK_SIDE;
        if ((l = pc_malloc_array(n, sizeof *l)) == NULL ||
            (change = pc_calloc_array(n, sizeof *change)) == NULL) {
            goto nomem;
        }
        for (i = 0; i < n; i++) {
            l[i] = log2f(pc_rs_maxf(y[i], p01));
        }
        for (i = 0; i < n; i++) {
            sum += (double)l[i];
        }
        geo = (float)(sum / (double)n);
        if (tone->shadows != 0.0f || tone->highlights != 0.0f) {
            double ps = -0.0;
            float pm;
            float *base;
            for (i = 0; i < n; i++) {
                ps += pow((double)exp2f(l[i]), 1.5);
            }
            pm = (float)(log2(ps / (double)n) / 1.5);
            if ((base = guided(l, l, sw, sh, radius(BASE_RADIUS * scale), BASE_EPS)) == NULL) {
                goto nomem;
            }
            for (i = 0; i < n; i++) {
                change[i] +=
                    shadows(tone->shadows, base[i] - pm) + highlights(tone->highlights, base[i] - geo);
            }
            free(base);
        }
        if (tone->clarity != 0.0f && !clarity(change, l, sw, sh, geo, scale, tone->clarity)) {
            goto nomem;
        }
        /* change ~ a * l + b locally, evaluated at full size; the large eps lets the change follow
         * strong edges only. */
        if (!linear_fit(l, change, sw, sh, 2, UPSAMPLE_EPS, &m.work.a, &m.work.b)) {
            goto nomem;
        }
        m.work.sw = sw;
        m.work.sh = sh;
        m.work.floor = log2f(p01);
        m.has_work = true;
    }
    if (tone->texture != 0.0f) {
        if (!texture(w, h, lum, ctx, tone->texture, &m.texture)) {
            goto nomem;
        }
        m.has_texture = true;
    }
    free(y);
    free(l);
    free(change);
    *out = m;
    *some = true;
    return PC_OK;
nomem:
    free(y);
    free(l);
    free(change);
    rx_tonemap_fini(&m);
    return rx_nomem(err);
}

float rx_tonemap_gain(const rx_tonemap *m, size_t x, size_t y, float lum)
{
    float c = -0.0f;
    if (!m->has_work && !m->has_texture) {
        return 1.0f;
    }
    if (m->has_work) {
        c += up_change(&m->work, m->w, m->h, x, y, lum);
    }
    if (m->has_texture) {
        c += up_change(&m->texture, m->w, m->h, x, y, lum);
    }
    return exp2f(pc_clampf(c, -6.0f, 6.0f));
}

float rx_tonemap_unblack(const rx_tonemap *m, float v)
{
    return m->black > 0.0f ? pc_rs_maxf_k((v - m->black) / (1.0f - m->black), 0.0f) : v;
}

pc_status rx_black_level(size_t w, size_t h, rx_px_fn lum, const void *ctx, float factor, float *out,
                         pc_error *err)
{
    size_t sw;
    size_t sh;
    float *y;
    float q;
    if (w == 0 || h == 0 || factor <= 0.0f) {
        *out = 0.0f;
        return PC_OK;
    }
    if ((y = reduce(w, h, WORK_SIDE, 1, lum, ctx, &sw, &sh)) == NULL || !quantile(y, sw * sh, 0.001f, &q)) {
        free(y);
        return rx_nomem(err);
    }
    free(y);
    *out = factor * pc_rs_maxf_k(q, P01_FLOOR);
    return PC_OK;
}

/* ---- Dehaze ---- */

static void p3(const float v[3], float out[3])
{
    int r;
    for (r = 0; r < 3; r++) {
        out[r] = pc_rs_maxf_k(PRO_TO_P3[r][0] * v[0] + PRO_TO_P3[r][1] * v[1] + PRO_TO_P3[r][2] * v[2], 0.0f);
    }
}

typedef struct dc_less {
    const float *dc;
} dc_less;

/* The brightest dark channel first: |a, b| dc[b].total_cmp(&dc[a]). */
static bool brighter(void *ctx, size_t a, size_t b)
{
    const dc_less *d = ctx;
    return total_less_f(d->dc[b], d->dc[a]);
}

void rx_haze_fini(rx_haze *z)
{
    free(z->t);
    z->t = NULL;
}

pc_status rx_haze_new(size_t w, size_t h, rx_px_fn px, const void *ctx, float amount, rx_haze *out,
                      bool *some, pc_error *err)
{
    rx_haze z;
    size_t sw;
    size_t sh;
    size_t n;
    size_t i;
    size_t top;
    float *small = NULL;
    float *small_p3 = NULL;
    float *dc = NULL;
    size_t *idx = NULL;
    float *dark = NULL;
    float *guide = NULL;
    float *smooth = NULL;
    float air[3] = {0.0f, 0.0f, 0.0f};
    float air_p3[3];
    float yrow[3];
    float air_y;
    float scale;
    float wgt;
    pc_raw_mat3 m;
    dc_less dl;
    int c;
    memset(&z, 0, sizeof z);
    *some = false;
    amount = pc_clampf(amount, -100.0f, 100.0f) / 100.0f;
    if (w == 0 || h == 0 || amount == 0.0f) {
        return PC_OK;
    }
    if ((small = reduce(w, h, WORK_SIDE, 3, px, ctx, &sw, &sh)) == NULL) {
        goto nomem;
    }
    n = sw * sh;
    if ((small_p3 = pc_malloc_array(n * 3, sizeof *small_p3)) == NULL ||
        (dc = pc_malloc_array(n, sizeof *dc)) == NULL || (idx = pc_malloc_array(n, sizeof *idx)) == NULL) {
        goto nomem;
    }
    for (i = 0; i < n; i++) {
        p3(small + i * 3, small_p3 + i * 3);
        dc[i] = pc_rs_minf(pc_rs_minf(small_p3[i * 3], small_p3[i * 3 + 1]), small_p3[i * 3 + 2]);
        idx[i] = i;
    }
    /* Airlight: mean colour of the 0.1 % of pixels with the brightest dark channel. */
    top = n / 1000 > 1 ? n / 1000 : 1;
    dl.dc = dc;
    rx_select_nth(idx, n, top - 1, brighter, &dl);
    for (i = 0; i < top; i++) {
        for (c = 0; c < 3; c++) {
            air[c] += small[idx[i] * 3 + (size_t)c] / (float)top;
        }
    }
    for (c = 0; c < 3; c++) {
        air[c] = pc_rs_maxf_k(air[c], 1e-6f);
    }
    z.w = w;
    z.h = h;
    z.sw = sw;
    z.sh = sh;
    memcpy(z.air, air, sizeof air);
    if (amount < 0.0f) {
        z.veil = HAZE_DOWN * -amount;
        goto ok;
    }
    p3(air, air_p3);
    for (c = 0; c < 3; c++) {
        air_p3[c] = pc_rs_maxf_k(air_p3[c], 1e-6f);
    }
    scale = (float)(sw > sh ? sw : sh) / (float)WORK_SIDE;
    for (i = 0; i < n; i++) {
        float v = 3.40282347e38f;
        for (c = 0; c < 3; c++) {
            v = pc_rs_minf(v, small_p3[i * 3 + (size_t)c] / air_p3[c]);
        }
        dc[i] = v;
    }
    m = rx_rgb_to_xyz(RX_ROMM, RX_D50_XY);
    for (c = 0; c < 3; c++) {
        yrow[c] = (float)m.m[1][c];
    }
    air_y = pc_rs_maxf_k(yrow[0] * air[0] + yrow[1] * air[1] + yrow[2] * air[2], 1e-6f);
    if ((dark = separable(dc, sw, sh, radius(HAZE_RADIUS * scale), NULL, 2)) == NULL ||
        (guide = pc_malloc_array(n, sizeof *guide)) == NULL) {
        goto nomem;
    }
    for (i = 0; i < n; i++) {
        const float *s3 = small + i * 3;
        guide[i] = (yrow[0] * s3[0] + yrow[1] * s3[1] + yrow[2] * s3[2]) / air_y;
    }
    if ((smooth = guided(guide, dark, sw, sh, radius(HAZE_SMOOTH * scale), HAZE_EPS)) == NULL) {
        goto nomem;
    }
    wgt = HAZE_UP * amount;
    for (i = 0; i < n; i++) {
        smooth[i] = pc_rs_maxf_k(1.0f - wgt * smooth[i], HAZE_T0);
    }
    z.t = smooth;
    smooth = NULL;
ok:
    free(small);
    free(small_p3);
    free(dc);
    free(idx);
    free(dark);
    free(guide);
    *out = z;
    *some = true;
    return PC_OK;
nomem:
    free(small);
    free(small_p3);
    free(dc);
    free(idx);
    free(dark);
    free(guide);
    free(smooth);
    return rx_nomem(err);
}

void rx_haze_apply(const rx_haze *z, size_t x, size_t y, const float v[3], float out[3])
{
    int c;
    float t;
    if (z->t == NULL) {
        for (c = 0; c < 3; c++) {
            out[c] = v[c] * (1.0f - z->veil) + z->air[c] * z->veil;
        }
        return;
    }
    t = pc_rs_maxf_k(bilinear(z->t, z->sw, z->sh, z->w, z->h, x, y), HAZE_T0);
    for (c = 0; c < 3; c++) {
        out[c] = pc_rs_maxf_k((v[c] - z->air[c]) / t + z->air[c], 0.0f);
    }
}

/* ---- retone ---- */

typedef struct retone_ctx {
    const float (*lin)[3];
    float yrow[3];
} retone_ctx;

static void px_lin(const void *ctx, size_t i, float out[3])
{
    const retone_ctx *c = ctx;
    memcpy(out, c->lin[i], 3 * sizeof *out);
}

static void px_lin_lum(const void *ctx, size_t i, float out[3])
{
    const retone_ctx *c = ctx;
    out[0] = c->yrow[0] * c->lin[i][0] + c->yrow[1] * c->lin[i][1] + c->yrow[2] * c->lin[i][2];
}

static float forward(const float (*curve)[2], size_t n, float v)
{
    return powf(pc_clampf(rx_tone(curve, n, pc_clampf(v, 0.0f, 1.0f)), 0.0f, 1.0f), 1.0f / 1.8f);
}

#define RETONE_N 4096

pc_status pc_raw_retone(float (*px)[4], size_t w, size_t h, const float (*curve)[2], size_t ncurve,
                        const pc_raw_tone *tone, pc_error *err)
{
    float *table;
    float (*lin)[3];
    retone_ctx c;
    rx_haze haze;
    rx_tonemap map;
    bool has_haze = false;
    bool has_map = false;
    pc_raw_mat3 m;
    size_t n;
    size_t i;
    pc_status st;
    int k;
    if (pc_raw_tone_is_neutral(tone) || w == 0 || h == 0 || !pc_size_mul(w, h, &n)) {
        return PC_OK;
    }
    table = pc_malloc_array(RETONE_N + 1, sizeof *table);
    lin = pc_malloc_array(n, sizeof *lin);
    if (table == NULL || lin == NULL) {
        free(table);
        free(lin);
        return rx_nomem(err);
    }
    for (i = 0; i <= RETONE_N; i++) {
        float s = (float)i / (float)RETONE_N;
        table[i] = forward(curve, ncurve, s * s);
    }
    /* The inverse from the table over s = sqrt(v) (the encoding is steep near 0). */
    for (i = 0; i < n; i++) {
        for (k = 0; k < 3; k++) {
            float e = px[i][k];
            size_t lo = 0;
            size_t hi = RETONE_N + 1;
            size_t j;
            float a;
            float b;
            float f;
            float s;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (table[mid] < e) {
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }
            j = lo < 1 ? 1 : lo > RETONE_N ? RETONE_N : lo;
            a = table[j - 1];
            b = table[j];
            f = b > a ? pc_clampf((e - a) / (b - a), 0.0f, 1.0f) : 0.0f;
            s = ((float)(j - 1) + f) / (float)RETONE_N;
            lin[i][k] = s * s;
        }
    }
    free(table);
    c.lin = (const float (*)[3])lin;
    if ((st = rx_haze_new(w, h, px_lin, &c, tone->dehaze, &haze, &has_haze, err)) != PC_OK) {
        free(lin);
        return st;
    }
    if (has_haze) {
        for (i = 0; i < n; i++) {
            float o[3];
            rx_haze_apply(&haze, i % w, i / w, lin[i], o);
            memcpy(lin[i], o, sizeof o);
        }
        rx_haze_fini(&haze);
    }
    m = rx_rgb_to_xyz(RX_ROMM, RX_D50_XY);
    for (k = 0; k < 3; k++) {
        c.yrow[k] = (float)m.m[1][k];
    }
    if ((st = rx_tonemap_new(w, h, px_lin_lum, &c, tone, 0.0f, &map, &has_map, err)) != PC_OK) {
        free(lin);
        return st;
    }
    for (i = 0; i < n; i++) {
        float lum = c.yrow[0] * lin[i][0] + c.yrow[1] * lin[i][1] + c.yrow[2] * lin[i][2];
        float g = has_map ? rx_tonemap_gain(&map, i % w, i / w, lum) : 1.0f;
        for (k = 0; k < 3; k++) {
            px[i][k] = forward(curve, ncurve, lin[i][k] * g);
        }
    }
    if (has_map) {
        rx_tonemap_fini(&map);
    }
    free(lin);
    return PC_OK;
}
