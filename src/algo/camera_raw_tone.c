/* Camera Raw Shadows and Highlights as a local Laplacian filter (port of
 * crates/algo/src/camera_raw/local_tone.rs).
 *
 * Adobe has said Camera Raw's process version 2012 rests on local Laplacian filters (S. Paris,
 * S. W. Hasinoff, J. Kautz, SIGGRAPH 2011). This follows that paper in its fast form (M. Aubry et
 * al., ACM TOG 2014: the remapping sampled at a few reference values, each pyramid coefficient
 * interpolated between them), on the log2 luminance. Shadows raises pixels darker than their
 * surroundings, Highlights pulls pixels brighter than their surroundings toward them; Shadows then
 * keeps the image's 99th percentile, Highlights its 1st. The strengths were fitted to Photoshop
 * 25.4's Camera Raw Filter on public-domain photos (black-box measurements).
 *
 * Every sum keeps the reference's order (`.sum()` from -0.0), so results are bit-identical. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/camera_raw_int.h"
#include "algo/fxutil.h"
#include "algo/photo_util.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Spacing of the fast filter's reference values in stops. */
#define REF_STEP 0.125f
/* The filter runs on a copy at most this many pixels across (guided upsampling brings the change
 * back to full size, so a preview and the full image agree). */
#define WORK_SIDE 1600
/* Knots of the strength curves over the surroundings' place in the image range (0..1). */
#define KNOTS 5

/* Shadows: how far a pixel darker than its surroundings rises, in stops per unit slider, as a
 * function of its own place in the image range times a factor of its surroundings' place; fully
 * once it is `sigma` stops below them. Lowering is `down` times as strong. */
static const float SH_OWN[KNOTS] = {-0.017f, 0.466f, 0.886f, 1.221f, -0.081f};
static const float SH_AROUND[KNOTS] = {4.566f, 4.067f, 1.088f, 0.149f, 0.878f};
#define SH_SIGMA 0.571f
#define SH_DOWN 0.808f
/* Highlights: strengths per unit slider for lowering (pull) and raising (push), and the detail
 * threshold in stops. */
static const float HL_PULL[KNOTS] = {0.179f, 0.141f, 0.196f, 0.629f, 2.07f};
static const float HL_PUSH[KNOTS] = {0.156f, 0.08f, 0.175f, 0.049f, 1.261f};
#define HL_SIGMA 0.05f

static const float TAPS[5] = {1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};

/* One channel, w x h. */
typedef struct plane {
    size_t w, h;
    float *v;
} plane;

static void plane_free(plane *p)
{
    free(p->v);
    p->v = NULL;
}

static bool plane_new(plane *p, size_t w, size_t h, bool zero)
{
    p->w = w;
    p->h = h;
    p->v = zero ? pc_calloc_array(w * h > 0 ? w * h : 1, sizeof *p->v)
                : pc_malloc_array(w * h > 0 ? w * h : 1, sizeof *p->v);
    return p->v != NULL;
}

static bool plane_copy(plane *dst, const plane *src)
{
    if (!plane_new(dst, src->w, src->h, false)) {
        return false;
    }
    memcpy(dst->v, src->v, src->w * src->h * sizeof *src->v);
    return true;
}

/* Mirrors i into 0..n (the edge sample is not repeated). */
static size_t mirror(int64_t i, size_t n)
{
    int64_t nn = (int64_t)n, p, m;
    if (nn == 1) {
        return 0;
    }
    p = 2 * (nn - 1);
    m = i % p;
    if (m < 0) {
        m += p;
    }
    return (size_t)(m >= nn ? p - m : m);
}

typedef struct blur_ctx {
    const plane *p;
    size_t step, w2;
    float *tmp, *out;
} blur_ctx;

static pc_status blur_cols(void *vc, size_t chunk, size_t y0, size_t y1)
{
    const blur_ctx *b = vc;
    const plane *p = b->p;
    size_t y2, x;
    (void)chunk;
    for (y2 = y0; y2 < y1; y2++) {
        int64_t y = (int64_t)(y2 * b->step);
        float *row = b->tmp + y2 * p->w;
        for (x = 0; x < p->w; x++) {
            float s = -0.0f;
            int k;
            for (k = 0; k < 5; k++) {
                s += TAPS[k] * p->v[mirror(y + k - 2, p->h) * p->w + x];
            }
            row[x] = s;
        }
    }
    return PC_OK;
}

static pc_status blur_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    const blur_ctx *b = vc;
    size_t w = b->p->w, y2, x2;
    (void)chunk;
    for (y2 = y0; y2 < y1; y2++) {
        const float *src = b->tmp + y2 * w;
        float *row = b->out + y2 * b->w2;
        for (x2 = 0; x2 < b->w2; x2++) {
            int64_t x = (int64_t)(x2 * b->step);
            float s = -0.0f;
            int k;
            for (k = 0; k < 5; k++) {
                s += TAPS[k] * src[mirror(x + k - 2, w)];
            }
            row[x2] = s;
        }
    }
    return PC_OK;
}

/* The binomial 5-tap blur, sampled every `step` pixels from 0 (1 = full resolution): columns first
 * at the kept rows, then rows at the kept columns. */
static bool blur_step(const plane *p, size_t step, plane *out)
{
    size_t w2 = (p->w + step - 1) / step, h2 = (p->h + step - 1) / step;
    blur_ctx b;
    b.tmp = pc_malloc_array(p->w * h2 > 0 ? p->w * h2 : 1, sizeof *b.tmp);
    if (b.tmp == NULL || !plane_new(out, w2, h2, false)) {
        free(b.tmp);
        return false;
    }
    b.p = p;
    b.step = step;
    b.w2 = w2;
    b.out = out->v;
    (void)pc_algo_par_for(h2, 4, blur_cols, &b);
    (void)pc_algo_par_for(h2, 4, blur_rows, &b);
    free(b.tmp);
    return true;
}

/* p brought up to w x h: zeros between samples, then the blur times four. */
static bool up(const plane *p, size_t w, size_t h, plane *out)
{
    plane z;
    size_t x, y, ye = p->h < (h + 1) / 2 ? p->h : (h + 1) / 2, xe = p->w < (w + 1) / 2 ? p->w : (w + 1) / 2;
    bool ok;
    if (!plane_new(&z, w, h, true)) {
        return false;
    }
    for (y = 0; y < ye; y++) {
        for (x = 0; x < xe; x++) {
            z.v[2 * y * w + 2 * x] = p->v[y * p->w + x] * 4.0f;
        }
    }
    ok = blur_step(&z, 1, out);
    plane_free(&z);
    return ok;
}

/* Levels down to about 2 pixels on the short side. */
static size_t depth(size_t w, size_t h)
{
    size_t s = w < h ? w : h, n = 1;
    while (s > 2) {
        s = (s + 1) / 2;
        n++;
    }
    return n;
}

static void pyramid_free(plane *g, size_t n)
{
    size_t i;
    if (g == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        plane_free(&g[i]);
    }
    free(g);
}

/* The Gaussian pyramid of p (taken over) with n levels; NULL on NOMEM. */
static plane *gaussian(plane p, size_t n)
{
    plane *g = pc_calloc_array(n, sizeof *g);
    size_t i;
    if (g == NULL) {
        plane_free(&p);
        return NULL;
    }
    g[0] = p;
    for (i = 1; i < n; i++) {
        if (!blur_step(&g[i - 1], 2, &g[i])) {
            pyramid_free(g, n);
            return NULL;
        }
    }
    return g;
}

static int cmp_total_f32(const void *pa, const void *pb)
{
    float a = *(const float *)pa, b = *(const float *)pb;
    int32_t ia, ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    ia ^= (int32_t)((uint32_t)(ia >> 31) >> 1);
    ib ^= (int32_t)((uint32_t)(ib >> 31) >> 1);
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

/* The q quantile (0..1) of v; NaN when empty or on NOMEM (callers then keep the image). */
static float quantile(const float *v, size_t n, float q)
{
    float *s, r;
    if (n == 0) {
        return NAN;
    }
    s = pc_malloc_array(n, sizeof *s);
    if (s == NULL) {
        return NAN;
    }
    memcpy(s, v, n * sizeof *s);
    qsort(s, n, sizeof *s, cmp_total_f32);
    r = s[pc_f32_to_size(roundf((float)(n - 1) * q))];
    free(s);
    return r;
}

/* The remapping of a value x against a reference g. */
typedef struct remap_fn {
    bool shadows;
    float lo, span, k; /* k: Shadows' signed strength, Highlights' amount */
    const float *strengths;
} remap_fn;

static float knot(float t, const float *k)
{
    float x = pc_clampf(t, 0.0f, 1.0f) * (float)(KNOTS - 1);
    size_t i = pc_f32_to_size(x);
    i = i < KNOTS - 2 ? i : KNOTS - 2;
    return k[i] + (k[i + 1] - k[i]) * (x - (float)i);
}

/* d past the detail threshold: about 0 below sigma, d - sigma well above it. */
static float excess(float d, float sigma)
{
    return d - sigma * tanhf(d / sigma);
}

static float remap(const remap_fn *f, float x, float g)
{
    if (f->shadows) {
        float below = pc_clampf((g - x) / SH_SIGMA, 0.0f, 1.0f), w;
        if (below == 0.0f) {
            return x;
        }
        w = below * below * (3.0f - 2.0f * below);
        return x + f->k * knot((x - f->lo) / f->span, SH_OWN) * knot((g - f->lo) / f->span, SH_AROUND) * w;
    }
    return x + knot((g - f->lo) / f->span, f->strengths) * f->k * excess(pc_rs_maxf_k(x - g, 0.0f), HL_SIGMA);
}

typedef struct remap_ctx {
    const remap_fn *f;
    const float *src;
    float *dst, r;
    size_t w;
} remap_ctx;

static pc_status remap_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    const remap_ctx *c = vc;
    size_t i;
    (void)chunk;
    for (i = y0 * c->w; i < y1 * c->w; i++) {
        c->dst[i] = remap(c->f, c->src[i], c->r);
    }
    return PC_OK;
}

typedef struct lap_ctx {
    float *lap;
    const float *gl, *fine, *coarse;
    size_t lw;
    float lo, hi, r, step;
} lap_ctx;

static pc_status lap_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    const lap_ctx *c = vc;
    size_t i;
    (void)chunk;
    for (i = y0 * c->lw; i < y1 * c->lw; i++) {
        float wgt = pc_rs_maxf_k(1.0f - fabsf(pc_clampf(c->gl[i], c->lo, c->hi) - c->r) / c->step, 0.0f);
        if (wgt > 0.0f) {
            c->lap[i] += wgt * (c->fine[i] - c->coarse[i]);
        }
    }
    return PC_OK;
}

/* The local Laplacian filter of u with the remapping f; the coarsest level is kept. */
static bool llf(const plane *u, const remap_fn *f, plane *out)
{
    size_t n = depth(u->w, u->h), npx = u->w * u->h, i, l, refs, k;
    plane copy, *g, *gr = NULL;
    float **lap = NULL, hi = -INFINITY, lo, step;
    bool ok = false;
    if (!plane_copy(&copy, u)) {
        return false;
    }
    g = gaussian(copy, n);
    if (g == NULL) {
        return false;
    }
    /* References span the image's tones; the deepest 0.1 % share the lowest reference. */
    for (i = 0; i < npx; i++) {
        hi = pc_rs_maxf(hi, u->v[i]);
    }
    lo = pc_rs_minf(quantile(u->v, npx, 0.001f), hi);
    if (hi - lo <= 1e-6f || isnan(hi - lo)) {
        pyramid_free(g, n);
        return plane_copy(out, u);
    }
    refs = pc_f32_to_size(ceilf((hi - lo) / REF_STEP)) + 1;
    refs = refs < 2 ? 2 : (refs > 96 ? 96 : refs);
    step = (hi - lo) / (float)(refs - 1);
    lap = pc_calloc_array(n, sizeof *lap);
    if (lap == NULL) {
        goto done;
    }
    for (l = 0; l + 1 < n; l++) {
        lap[l] = pc_calloc_array(g[l].w * g[l].h > 0 ? g[l].w * g[l].h : 1, sizeof **lap);
        if (lap[l] == NULL) {
            goto done;
        }
    }
    for (k = 0; k < refs; k++) {
        float r = lo + step * (float)k;
        plane m;
        remap_ctx rc;
        if (!plane_new(&m, u->w, u->h, false)) {
            goto done;
        }
        rc.f = f;
        rc.src = u->v;
        rc.dst = m.v;
        rc.r = r;
        rc.w = u->w;
        (void)pc_algo_par_for(u->h, 4, remap_rows, &rc);
        gr = gaussian(m, n);
        if (gr == NULL) {
            goto done;
        }
        for (l = 0; l + 1 < n; l++) {
            plane coarse;
            lap_ctx lc;
            if (!up(&gr[l + 1], gr[l].w, gr[l].h, &coarse)) {
                goto done;
            }
            lc.lap = lap[l];
            lc.gl = g[l].v;
            lc.fine = gr[l].v;
            lc.coarse = coarse.v;
            lc.lw = g[l].w;
            lc.lo = lo;
            lc.hi = hi;
            lc.r = r;
            lc.step = step;
            (void)pc_algo_par_for(g[l].h, 4, lap_rows, &lc);
            plane_free(&coarse);
        }
        pyramid_free(gr, n);
        gr = NULL;
    }
    {
        plane o;
        if (!plane_copy(&o, &g[n - 1])) {
            goto done;
        }
        for (l = n - 1; l-- > 0;) {
            plane next;
            bool up_ok = up(&o, g[l].w, g[l].h, &next);
            plane_free(&o);
            if (!up_ok) {
                goto done;
            }
            for (i = 0; i < next.w * next.h; i++) {
                next.v[i] += lap[l][i];
            }
            o = next;
        }
        *out = o;
    }
    ok = true;
done:
    pyramid_free(gr, n);
    if (lap != NULL) {
        for (l = 0; l < n; l++) {
            free(lap[l]);
        }
        free(lap);
    }
    pyramid_free(g, n);
    return ok;
}

/* 1st and 99th percentiles of the log luminance on a copy at most 256 pixels across. */
static bool lum_range(const plane *u, float *lo, float *hi)
{
    size_t m = u->w > u->h ? u->w : u->h, f = (m + 255) / 256, sw, sh, x, y, i, n = 0;
    double *sum;
    uint32_t *cnt;
    float *cells;
    f = f > 1 ? f : 1;
    sw = (u->w + f - 1) / f;
    sh = (u->h + f - 1) / f;
    sum = pc_calloc_array(sw * sh, sizeof *sum);
    cnt = pc_calloc_array(sw * sh, sizeof *cnt);
    cells = pc_malloc_array(sw * sh, sizeof *cells);
    if (sum == NULL || cnt == NULL || cells == NULL) {
        free(sum), free(cnt), free(cells);
        return false;
    }
    for (y = 0; y < u->h; y++) {
        for (x = 0; x < u->w; x++) {
            size_t c = (y / f) * sw + x / f;
            sum[c] += (double)u->v[y * u->w + x];
            cnt[c]++;
        }
    }
    for (i = 0; i < sw * sh; i++) {
        if (cnt[i] > 0) {
            cells[n++] = pc_f64_to_f32(sum[i] / (double)cnt[i]);
        }
    }
    *lo = quantile(cells, n, 0.01f);
    *hi = quantile(cells, n, 0.99f);
    free(sum), free(cnt), free(cells);
    return true;
}

/* One pass: the filter, then the shift that keeps quantile q of the input in place (Shadows keeps
 * the brightest content, Highlights the darkest). */
static bool tone_pass(const plane *u, bool shadows, float amount, plane *out)
{
    remap_fn f;
    float lo, hi, q, shift;
    size_t i;
    if (!lum_range(u, &lo, &hi)) {
        return false;
    }
    f.shadows = shadows;
    f.lo = lo;
    f.span = pc_rs_maxf_k(hi - lo, 1.0f);
    if (shadows) {
        f.k = amount > 0.0f ? amount : amount * SH_DOWN;
        f.strengths = NULL;
        q = 0.99f;
    } else {
        f.k = amount;
        f.strengths = amount < 0.0f ? HL_PULL : HL_PUSH;
        q = 0.01f;
    }
    if (!llf(u, &f, out)) {
        return false;
    }
    shift = quantile(u->v, u->w * u->h, q) - quantile(out->v, out->w * out->h, q);
    for (i = 0; i < out->w * out->h; i++) {
        out->v[i] = out->v[i] + shift;
    }
    return true;
}

/* u averaged over f x f blocks. */
static bool shrink(const plane *u, size_t f, plane *out)
{
    size_t sw = (u->w + f - 1) / f, sh = (u->h + f - 1) / f, x, y, xx, yy;
    if (!plane_new(out, sw, sh, false)) {
        return false;
    }
    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            size_t x0 = x * f, y0 = y * f, x1 = x0 + f < u->w ? x0 + f : u->w,
                   y1 = y0 + f < u->h ? y0 + f : u->h;
            double s = 0.0;
            for (yy = y0; yy < y1; yy++) {
                for (xx = x0; xx < x1; xx++) {
                    s += (double)u->v[yy * u->w + xx];
                }
            }
            out->v[y * sw + x] = pc_f64_to_f32(s / (double)((x1 - x0) * (y1 - y0)));
        }
    }
    return true;
}

/* The change d computed on `small` brought to the size of `full`, following its edges: a local
 * linear model of the change against the log luminance (He & Sun's fast guided filter), fitted on
 * the small copy and evaluated on the full one. *out has full->w * full->h values. */
static pc_status guided_upsample(const plane *small, const float *d, const plane *full, float **out,
                                 pc_error *err)
{
    size_t sw = small->w, sh = small->h, n = sw * sh, i, x, y;
    const size_t r = 2;
    const float eps = 0.01f;
    float *ud, *uu, *a, *b, *o, *mu = NULL, *md = NULL, *mud = NULL, *muu = NULL, *ma = NULL, *mb = NULL, fx,
                                fy;
    pc_status st;
    ud = pc_malloc_array(n, sizeof *ud);
    uu = pc_malloc_array(n, sizeof *uu);
    a = pc_malloc_array(n, sizeof *a);
    b = pc_malloc_array(n, sizeof *b);
    o = pc_malloc_array(full->w * full->h, sizeof *o);
    if (ud == NULL || uu == NULL || a == NULL || b == NULL || o == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Camera Raw");
        goto done;
    }
    for (i = 0; i < n; i++) {
        ud[i] = small->v[i] * d[i];
        uu[i] = small->v[i] * small->v[i];
    }
    if ((st = pc_algo_cr_box_mean(small->v, sw, sh, r, &mu, err)) != PC_OK ||
        (st = pc_algo_cr_box_mean(d, sw, sh, r, &md, err)) != PC_OK ||
        (st = pc_algo_cr_box_mean(ud, sw, sh, r, &mud, err)) != PC_OK ||
        (st = pc_algo_cr_box_mean(uu, sw, sh, r, &muu, err)) != PC_OK) {
        goto done;
    }
    for (i = 0; i < n; i++) {
        a[i] = (mud[i] - mu[i] * md[i]) / (muu[i] - mu[i] * mu[i] + eps);
        b[i] = md[i] - a[i] * mu[i];
    }
    if ((st = pc_algo_cr_box_mean(a, sw, sh, r, &ma, err)) != PC_OK ||
        (st = pc_algo_cr_box_mean(b, sw, sh, r, &mb, err)) != PC_OK) {
        goto done;
    }
    fx = (float)sw / (float)full->w;
    fy = (float)sh / (float)full->h;
    for (y = 0; y < full->h; y++) {
        float sy = ((float)y + 0.5f) * fy - 0.5f;
        for (x = 0; x < full->w; x++) {
            float sx = ((float)x + 0.5f) * fx - 0.5f;
            float av = pc_algo_photo_bilinear(ma, n, sw, sh, 1, 0, sx, sy);
            float bv = pc_algo_photo_bilinear(mb, n, sw, sh, 1, 0, sx, sy);
            o[y * full->w + x] = av * full->v[y * full->w + x] + bv;
        }
    }
    *out = o;
    o = NULL;
done:
    free(ud), free(uu), free(mu), free(md), free(mud), free(muu), free(a), free(b), free(ma), free(mb),
        free(o);
    return st;
}

typedef struct gain_ctx {
    float (*px)[4];
    const float *change;
    size_t w;
    pc_algo_cr_encoding enc;
} gain_ctx;

static pc_status gain_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    const gain_ctx *c = vc;
    size_t i;
    (void)chunk;
    for (i = y0 * c->w; i < y1 * c->w; i++) {
        float gain = exp2f(c->change[i]);
        int k;
        for (k = 0; k < 3; k++) {
            c->px[i][k] = pc_algo_cr_encode(c->enc, pc_algo_cr_decode(c->enc, c->px[i][k]) * gain);
        }
    }
    return PC_OK;
}

pc_status pc_algo_cr_local_tone(float (*px)[4], size_t w, size_t h, float shadows, float highlights,
                                pc_algo_cr_encoding enc, pc_error *err)
{
    const float FLOOR = 1.0f / 65536.0f;
    plane u = {0, 0, NULL}, small = {0, 0, NULL}, o = {0, 0, NULL}, next;
    size_t i, f, m = w > h ? w : h;
    float *change = NULL;
    gain_ctx gc;
    pc_status st = PC_OK;
    if ((shadows == 0.0f && highlights == 0.0f) || w < 2 || h < 2) {
        return PC_OK;
    }
    if (!plane_new(&u, w, h, false)) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Camera Raw");
    }
    for (i = 0; i < w * h; i++) {
        float d0 = pc_algo_cr_decode(enc, px[i][0]), d1 = pc_algo_cr_decode(enc, px[i][1]),
              d2 = pc_algo_cr_decode(enc, px[i][2]);
        u.v[i] = log2f(pc_rs_maxf_k(0.2126f * d0 + 0.7152f * d1 + 0.0722f * d2, 0.0f) + FLOOR);
    }
    /* The filter on a copy at most WORK_SIDE across (box-averaged in log). */
    f = (m + WORK_SIDE - 1) / WORK_SIDE;
    f = f > 1 ? f : 1;
    if (!(f == 1 ? plane_copy(&small, &u) : shrink(&u, f, &small)) || !plane_copy(&o, &small)) {
        goto nomem;
    }
    if (highlights != 0.0f) {
        if (!tone_pass(&o, false, pc_clampf(highlights, -100.0f, 100.0f) / 100.0f, &next)) {
            goto nomem;
        }
        plane_free(&o);
        o = next;
    }
    if (shadows != 0.0f) {
        if (!tone_pass(&o, true, pc_clampf(shadows, -100.0f, 100.0f) / 100.0f, &next)) {
            goto nomem;
        }
        plane_free(&o);
        o = next;
    }
    /* The change in log luminance, at full size. */
    if (f == 1) {
        change = pc_malloc_array(w * h, sizeof *change);
        if (change == NULL) {
            goto nomem;
        }
        for (i = 0; i < w * h; i++) {
            change[i] = o.v[i] - u.v[i];
        }
    } else {
        float *d = pc_malloc_array(small.w * small.h, sizeof *d);
        if (d == NULL) {
            goto nomem;
        }
        for (i = 0; i < small.w * small.h; i++) {
            d[i] = o.v[i] - small.v[i];
        }
        st = guided_upsample(&small, d, &u, &change, err);
        free(d);
        if (st != PC_OK) {
            goto done;
        }
    }
    gc.px = px;
    gc.change = change;
    gc.w = w;
    gc.enc = enc;
    (void)pc_algo_par_for(h, 4, gain_rows, &gc);
    goto done;
nomem:
    st = pc_fail(err, PC_ERR_NOMEM, "out of memory for Camera Raw Shadows/Highlights");
done:
    plane_free(&u);
    plane_free(&small);
    plane_free(&o);
    free(change);
    return st;
}
