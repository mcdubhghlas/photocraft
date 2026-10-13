/* Camera Raw develop pipeline (port of camera_raw.rs `develop` and its stages). Per-row work runs
 * on the global pool with disjoint outputs; reductions run in index order, so results are
 * identical for any thread count. Arithmetic mirrors the reference operation for operation (f32
 * where it uses f32, f64 running sums in the box means). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/camera_raw.h"
#include "algo/camera_raw_int.h"
#include "algo/fxutil.h"
#include "algo/l9util.h"
#include "algo/photo_util.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define TAU_F 6.28318530717958647692f
#define ROWS 4 /* rows per pool chunk */

static float luma3(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

/* camera_raw.rs's own smoothstep (no guard for e1 <= e0). */
static float smooth(float e0, float e1, float x)
{
    float t = pc_clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static pc_status dup_f(const float *v, size_t n, float **out, pc_error *err)
{
    pc_status st = pc_algo_l9u_alloc_f(n, false, out, err);
    if (st == PC_OK && n > 0) {
        memcpy(*out, v, n * sizeof *v);
    }
    return st;
}

/* ---- box mean, Gaussian, guided filter ---- */

typedef struct box_ctx {
    const float *v;
    float *tmp, *out;
    size_t w, h, r;
    double n;
    double *acc; /* one per column */
} box_ctx;

static size_t clampi(int64_t k, size_t len)
{
    return k < 0 ? 0 : ((uint64_t)k >= len ? len - 1 : (size_t)k);
}

static pc_status box_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    box_ctx *c = vc;
    size_t y, x;
    int64_t r = (int64_t)c->r, k;
    (void)chunk;
    for (y = y0; y < y1; y++) {
        const float *src = c->v + y * c->w;
        float *row = c->tmp + y * c->w;
        double acc = -0.0;
        for (k = -r; k <= r; k++) {
            acc += (double)src[clampi(k, c->w)];
        }
        for (x = 0; x < c->w; x++) {
            row[x] = pc_f64_to_f32(acc / c->n);
            acc += (double)src[clampi((int64_t)x + r + 1, c->w)] - (double)src[clampi((int64_t)x - r, c->w)];
        }
    }
    return PC_OK;
}

#define BAND 256

static pc_status box_cols(void *vc, size_t chunk, size_t b0, size_t b1)
{
    box_ctx *c = vc;
    size_t b, y, a;
    int64_t r = (int64_t)c->r, k;
    (void)chunk;
    for (b = b0; b < b1; b++) {
        size_t x0 = b * BAND, x1 = (b + 1) * BAND < c->w ? (b + 1) * BAND : c->w;
        double *acc = c->acc + x0;
        for (a = x0; a < x1; a++) {
            acc[a - x0] = 0.0;
        }
        for (k = -r; k <= r; k++) {
            const float *row = c->tmp + clampi(k, c->h) * c->w;
            for (a = x0; a < x1; a++) {
                acc[a - x0] += (double)row[a];
            }
        }
        for (y = 0; y < c->h; y++) {
            const float *add = c->tmp + clampi((int64_t)y + r + 1, c->h) * c->w;
            const float *sub = c->tmp + clampi((int64_t)y - r, c->h) * c->w;
            float *o = c->out + y * c->w;
            for (a = x0; a < x1; a++) {
                o[a] = pc_f64_to_f32(acc[a - x0] / c->n);
                acc[a - x0] += (double)(add[a] - sub[a]);
            }
        }
    }
    return PC_OK;
}

/* Box mean of a single-channel buffer (radius r, edge clamped). *out is newly allocated. */
pc_status pc_algo_cr_box_mean(const float *v, size_t w, size_t h, size_t r, float **out, pc_error *err)
{
    box_ctx c;
    pc_status st;
    size_t n = w * h;
    float *tmp = NULL, *o = NULL;
    double *acc = NULL;
    if (r == 0 || w == 0 || h == 0) {
        return dup_f(v, n, out, err);
    }
    if (r > PC_ALGO_MAX_BOX_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "blur radius %zu is too large", r);
    }
    if ((st = pc_algo_l9u_alloc_f(n, false, &tmp, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &o, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_d(w, false, &acc, err)) != PC_OK) {
        goto fail;
    }
    c.v = v;
    c.tmp = tmp;
    c.out = o;
    c.w = w;
    c.h = h;
    c.r = r;
    c.n = (double)(2 * r + 1);
    c.acc = acc;
    if ((st = pc_algo_par_for(h, ROWS, box_rows, &c)) != PC_OK ||
        (st = pc_algo_par_for((w + BAND - 1) / BAND, 1, box_cols, &c)) != PC_OK) {
        (void)pc_fail(err, st, "box mean failed");
        goto fail;
    }
    free(tmp);
    free(acc);
    *out = o;
    return PC_OK;
fail:
    free(tmp);
    free(o);
    free(acc);
    return st;
}

/* Radii of the three box passes (none below sigma 0.3); PC_ERR_LIMIT past the box cap. */
static pc_status gauss_radii(float sigma, size_t radii[3], bool *skip, pc_error *err)
{
    int i;
    *skip = sigma < 0.3f;
    if (*skip) {
        return PC_OK;
    }
    pc_algo_gauss_box_radii(sigma, radii);
    for (i = 0; i < 3; i++) {
        if (radii[i] > PC_ALGO_MAX_BOX_RADIUS) {
            return pc_fail(err, PC_ERR_LIMIT, "blur sigma %g is too large", (double)sigma);
        }
    }
    return PC_OK;
}

/* Approximate Gaussian (three box passes). *out newly allocated. */
static pc_status gauss(const float *v, size_t w, size_t h, float sigma, float **out, pc_error *err)
{
    size_t radii[3];
    bool skip;
    float *cur = NULL;
    int i;
    pc_status st = gauss_radii(sigma, radii, &skip, err);
    if (st != PC_OK) {
        return st;
    }
    if ((st = dup_f(v, w * h, &cur, err)) != PC_OK || skip) {
        if (st == PC_OK) {
            *out = cur;
        }
        return st;
    }
    for (i = 0; i < 3; i++) {
        float *next = NULL;
        st = pc_algo_cr_box_mean(cur, w, h, radii[i], &next, err);
        free(cur);
        if (st != PC_OK) {
            return st;
        }
        cur = next;
    }
    *out = cur;
    return PC_OK;
}

/* a = cov(i, p) / (var(i) + eps), b = mean(p) - a mean(i), each box-meaned; n samples. */
static pc_status guided_coeffs(const float *i, const float *p, size_t w, size_t h, size_t r, float eps,
                               float **ma, float **mb, pc_error *err)
{
    size_t n = w * h, k;
    float *mi = NULL, *mp = NULL, *ip = NULL, *ii = NULL, *mip = NULL, *mii = NULL, *a = NULL, *b = NULL;
    pc_status st;
    if ((st = pc_algo_cr_box_mean(i, w, h, r, &mi, err)) != PC_OK ||
        (st = pc_algo_cr_box_mean(p, w, h, r, &mp, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &ip, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &ii, err)) != PC_OK) {
        goto done;
    }
    for (k = 0; k < n; k++) {
        ip[k] = i[k] * p[k];
        ii[k] = i[k] * i[k];
    }
    if ((st = pc_algo_cr_box_mean(ip, w, h, r, &mip, err)) != PC_OK ||
        (st = pc_algo_cr_box_mean(ii, w, h, r, &mii, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &a, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &b, err)) != PC_OK) {
        goto done;
    }
    for (k = 0; k < n; k++) {
        a[k] = (mip[k] - mi[k] * mp[k]) / (mii[k] - mi[k] * mi[k] + eps);
        b[k] = mp[k] - a[k] * mi[k];
    }
    if ((st = pc_algo_cr_box_mean(a, w, h, r, ma, err)) != PC_OK) {
        goto done;
    }
    if ((st = pc_algo_cr_box_mean(b, w, h, r, mb, err)) != PC_OK) {
        free(*ma);
        *ma = NULL;
    }
done:
    free(mi), free(mp), free(ip), free(ii), free(mip), free(mii), free(a), free(b);
    return st;
}

/* Guided filter of p guided by i (He et al. 2010). */
static pc_status guided(const float *i, const float *p, size_t w, size_t h, size_t r, float eps, float **out,
                        pc_error *err)
{
    float *ma = NULL, *mb = NULL, *o = NULL;
    size_t k, n = w * h;
    pc_status st = guided_coeffs(i, p, w, h, r, eps, &ma, &mb, err);
    if (st == PC_OK && (st = pc_algo_l9u_alloc_f(n, false, &o, err)) == PC_OK) {
        for (k = 0; k < n; k++) {
            o[k] = ma[k] * i[k] + mb[k];
        }
        *out = o;
    }
    free(ma);
    free(mb);
    return st;
}

typedef struct up_ctx {
    const float *ma, *mb, *i;
    float *out;
    size_t w, sw, sh;
    float sf;
} up_ctx;

static pc_status guided_up_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    up_ctx *c = vc;
    size_t y, x;
    (void)chunk;
    for (y = y0; y < y1; y++) {
        float fy = ((float)y + 0.5f) / c->sf - 0.5f;
        for (x = 0; x < c->w; x++) {
            float fx = ((float)x + 0.5f) / c->sf - 0.5f;
            float av = pc_algo_photo_bilinear(c->ma, c->sw * c->sh, c->sw, c->sh, 1, 0, fx, fy);
            float bv = pc_algo_photo_bilinear(c->mb, c->sw * c->sh, c->sw, c->sh, 1, 0, fx, fy);
            c->out[y * c->w + x] = av * c->i[y * c->w + x] + bv;
        }
    }
    return PC_OK;
}

/* Box-average downsample by s (partial cells averaged over what they hold). */
static pc_status down(const float *v, size_t w, size_t h, size_t s, size_t sw, size_t sh, float **out,
                      pc_error *err)
{
    float *o = NULL, *c = NULL;
    size_t y, x, k, n = sw * sh;
    pc_status st;
    if ((st = pc_algo_l9u_alloc_f(n, true, &o, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, true, &c, err)) != PC_OK) {
        free(o);
        return st;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            k = (y / s) * sw + x / s;
            o[k] += v[y * w + x];
            c[k] += 1.0f;
        }
    }
    for (k = 0; k < n; k++) {
        o[k] = o[k] / pc_rs_maxf_k(c[k], 1.0f);
    }
    free(c);
    *out = o;
    return PC_OK;
}

/* Fast guided filter (He, Sun 2015): coefficients on an s-times subsampled copy, upsampled. */
static pc_status guided_fast(const float *i, const float *p, size_t w, size_t h, size_t r, float eps,
                             float **out, pc_error *err)
{
    size_t s = r / 8, sw, sh, rs;
    float *si = NULL, *sp = NULL, *ma = NULL, *mb = NULL, *o = NULL;
    up_ctx c;
    pc_status st;
    s = s < 1 ? 1 : (s > 8 ? 8 : s);
    if (s == 1) {
        return guided(i, p, w, h, r, eps, out, err);
    }
    sw = (w + s - 1) / s;
    sh = (h + s - 1) / s;
    rs = r / s < 1 ? 1 : r / s;
    if ((st = down(i, w, h, s, sw, sh, &si, err)) != PC_OK ||
        (st = down(p, w, h, s, sw, sh, &sp, err)) != PC_OK ||
        (st = guided_coeffs(si, sp, sw, sh, rs, eps, &ma, &mb, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(w * h, false, &o, err)) != PC_OK) {
        goto done;
    }
    c.ma = ma;
    c.mb = mb;
    c.i = i;
    c.out = o;
    c.w = w;
    c.sw = sw;
    c.sh = sh;
    c.sf = (float)s;
    if ((st = pc_algo_par_for(h, ROWS, guided_up_rows, &c)) != PC_OK) {
        (void)pc_fail(err, st, "guided filter failed");
        free(o);
        goto done;
    }
    *out = o;
done:
    free(si), free(sp), free(ma), free(mb);
    return st;
}

/* ---- curves, HSV ---- */

static float lut_eval(const float *lut, size_t n, float v)
{
    float x, t;
    size_t i;
    if (!(v >= 0.0f && v <= 1.0f)) {
        /* Extend linearly outside (float documents). */
        return v < 0.0f ? v + lut[0] : lut[n - 1] + (v - 1.0f);
    }
    x = v * (float)(n - 1);
    i = pc_f32_to_size(x);
    if (i > n - 2) {
        i = n - 2;
    }
    t = x - (float)i;
    return lut[i] + (lut[i + 1] - lut[i]) * t;
}

/* Parametric tone curve as a LUT of n entries. */
static void parametric_lut(const pc_algo_camera_raw *p, float *lut, size_t n)
{
    float s[3], centres[4], amounts[4], widths[4];
    size_t i;
    int k;
    for (k = 0; k < 3; k++) {
        s[k] = pc_clampf(p->curve_splits[k], 5.0f, 95.0f) / 100.0f;
    }
    centres[0] = s[0] / 2.0f;
    centres[1] = (s[0] + s[1]) / 2.0f;
    centres[2] = (s[1] + s[2]) / 2.0f;
    centres[3] = (s[2] + 1.0f) / 2.0f;
    amounts[0] = p->curve_shadows;
    amounts[1] = p->curve_darks;
    amounts[2] = p->curve_lights;
    amounts[3] = p->curve_highlights;
    widths[0] = pc_rs_maxf_k(s[0], 0.1f);
    widths[1] = pc_rs_maxf_k(s[1] - s[0], 0.1f);
    widths[2] = pc_rs_maxf_k(s[2] - s[1], 0.1f);
    widths[3] = pc_rs_maxf_k(1.0f - s[2], 0.1f);
    for (i = 0; i < n; i++) {
        float x = (float)i / (float)(n - 1), y = x;
        for (k = 0; k < 4; k++) {
            float d = (x - centres[k]) / (widths[k] * 1.2f);
            float bump = expf(-d * d * 2.0f) * x * (1.0f - x) * 4.0f;
            y += amounts[k] / 100.0f * 0.25f * bump;
        }
        lut[i] = pc_clampf(y, 0.0f, 1.0f);
    }
    for (i = 1; i < n; i++) {
        lut[i] = pc_rs_maxf(lut[i], lut[i - 1]);
    }
}

static void rgb_to_hsv(const float c[3], float out[3])
{
    float mx = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]), mn = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
    float d = mx - mn, h;
    if (d <= 1e-9f) {
        h = 0.0f;
    } else if (mx == c[0]) {
        h = 60.0f * pc_algo_rem_euclidf((c[1] - c[2]) / d, 6.0f);
    } else if (mx == c[1]) {
        h = 60.0f * ((c[2] - c[0]) / d + 2.0f);
    } else {
        h = 60.0f * ((c[0] - c[1]) / d + 4.0f);
    }
    out[0] = h;
    out[1] = mx > 1e-9f ? d / mx : 0.0f;
    out[2] = mx;
}

static void hsv_to_rgb(float h, float s, float v, float out[3])
{
    float c, x, m, r, g, b;
    h = pc_algo_rem_euclidf(h, 360.0f) / 60.0f;
    c = v * s;
    x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f));
    switch (pc_f32_to_u32(h)) {
    case 0:
        r = c, g = x, b = 0.0f;
        break;
    case 1:
        r = x, g = c, b = 0.0f;
        break;
    case 2:
        r = 0.0f, g = c, b = x;
        break;
    case 3:
        r = 0.0f, g = x, b = c;
        break;
    case 4:
        r = x, g = 0.0f, b = c;
        break;
    default:
        r = c, g = 0.0f, b = x;
        break;
    }
    m = v - c;
    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
}

/* ---- neutrality ---- */

static bool is_linear(const pc_algo_points *c)
{
    size_t i;
    if (c->n < 2) {
        return true;
    }
    for (i = 0; i < c->n; i++) {
        if (!(fabsf(c->v[i][0] - c->v[i][1]) < 1e-3f)) {
            return false;
        }
    }
    return true;
}

static bool wb_neutral(const pc_algo_camera_raw *p)
{
    return p->temperature == 0.0f && p->tint == 0.0f && p->exposure == 0.0f;
}
/* Contrast, Highlights, Shadows, Texture and Clarity (the luminance stage). */
static bool local_tone_neutral(const pc_algo_camera_raw *p)
{
    return p->contrast == 0.0f && p->highlights == 0.0f && p->shadows == 0.0f && p->texture == 0.0f &&
           p->clarity == 0.0f;
}
static bool tone_neutral(const pc_algo_camera_raw *p)
{
    return local_tone_neutral(p) && p->whites == 0.0f && p->blacks == 0.0f;
}
static bool hsl_neutral(const pc_algo_camera_raw *p)
{
    int k;
    for (k = 0; k < 8; k++) {
        if (p->hsl_hue[k] != 0.0f || p->hsl_sat[k] != 0.0f || p->hsl_lum[k] != 0.0f) {
            return false;
        }
    }
    return true;
}
static bool wheel_neutral(pc_algo_cr_wheel w)
{
    return w.sat == 0.0f && w.lum == 0.0f;
}
static bool grade_neutral(const pc_algo_camera_raw *p)
{
    return wheel_neutral(p->grade_shadows) && wheel_neutral(p->grade_midtones) &&
           wheel_neutral(p->grade_highlights) && wheel_neutral(p->grade_global);
}
static bool param_curve_neutral(const pc_algo_camera_raw *p)
{
    return p->curve_highlights == 0.0f && p->curve_lights == 0.0f && p->curve_darks == 0.0f &&
           p->curve_shadows == 0.0f;
}
static bool curves_neutral(const pc_algo_camera_raw *p)
{
    return param_curve_neutral(p) && is_linear(&p->point_curve) && is_linear(&p->point_curve_red) &&
           is_linear(&p->point_curve_green) && is_linear(&p->point_curve_blue);
}

bool pc_algo_camera_raw_is_identity(const pc_algo_camera_raw *p)
{
    return wb_neutral(p) && tone_neutral(p) && p->dehaze == 0.0f && p->vibrance == 0.0f &&
           p->saturation == 0.0f && curves_neutral(p) && hsl_neutral(p) && grade_neutral(p) &&
           p->sharpen_amount == 0.0f && p->noise_luminance == 0.0f && p->noise_color == 0.0f &&
           p->grain_amount == 0.0f && p->vignette_amount == 0.0f;
}

/* ---- per-pixel stages ---- */

#define LUT_N 1024

typedef struct dev_ctx {
    float (*px)[4];
    size_t w, h;
    const pc_algo_camera_raw *p;
    /* white balance */
    float g[3];
    /* tone */
    const float *l, *lc, *clar, *tex;
    float k_con;
    /* colour */
    const float *par, *master, *cur[3];
    bool hsl, grade;
    float tints[4][3], blend, bal;
    /* grain */
    float cell, rough, amt, pscale;
    uint64_t seed;
} dev_ctx;

static pc_status wb_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    dev_ctx *c = vc;
    size_t i, k;
    (void)chunk;
    for (i = y0 * c->w; i < y1 * c->w; i++) {
        for (k = 0; k < 3; k++) {
            float lin = pc_algo_photo_srgb_to_linear(c->px[i][k]) * c->g[k];
            c->px[i][k] = pc_algo_photo_linear_to_srgb(pc_rs_maxf_k(lin, 0.0f));
        }
    }
    return PC_OK;
}

static pc_status tone_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    dev_ctx *c = vc;
    const pc_algo_camera_raw *p = c->p;
    size_t i, k;
    (void)chunk;
    for (i = y0 * c->w; i < y1 * c->w; i++) {
        float *q = c->px[i];
        float l0 = c->l[i], lv = c->lc[i], ratio;
        if (c->clar != NULL) {
            float t = 2.0f * pc_clampf(lv, 0.0f, 1.0f) - 1.0f;
            float mw = 1.0f - t * t;
            lv += p->clarity / 100.0f * 0.8f * (c->lc[i] - c->clar[i]) * mw;
        }
        if (c->tex != NULL) {
            lv += p->texture / 100.0f * 0.9f * pc_clampf(c->lc[i] - c->tex[i], -0.1f, 0.1f);
        }
        if (c->k_con != 0.0f) {
            float xx = pc_clampf(lv, 0.0f, 1.0f);
            lv += -c->k_con * sinf(TAU_F * xx) / TAU_F;
        }
        lv = pc_rs_maxf_k(lv, 0.0f) + (l0 - c->lc[i]);
        ratio = l0 > 1e-5f ? lv / l0 : 1.0f;
        for (k = 0; k < 3; k++) {
            q[k] = l0 > 1e-5f ? q[k] * ratio : q[k] + (lv - l0);
        }
    }
    return PC_OK;
}

static pc_status color_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    dev_ctx *c = vc;
    const pc_algo_camera_raw *p = c->p;
    size_t i;
    int k, ch;
    (void)chunk;
    for (i = y0 * c->w; i < y1 * c->w; i++) {
        float *q = c->px[i];
        float col[3] = {q[0], q[1], q[2]};
        if (p->vibrance != 0.0f || p->saturation != 0.0f) {
            float l = luma3(col[0], col[1], col[2]);
            float mx = pc_rs_maxf(pc_rs_maxf(col[0], col[1]), col[2]);
            float mn = pc_rs_minf(pc_rs_minf(col[0], col[1]), col[2]);
            float sat = mx > 1e-6f ? (mx - mn) / mx : 0.0f, hsv[3], skin, kk;
            rgb_to_hsv(col, hsv);
            skin = pc_clampf(1.0f - fabsf(hsv[0] - 25.0f) / 25.0f, 0.0f, 1.0f) * 0.5f;
            kk = 1.0f + p->saturation / 100.0f + p->vibrance / 100.0f * (1.0f - sat) * (1.0f - skin);
            for (ch = 0; ch < 3; ch++) {
                col[ch] = l + (col[ch] - l) * pc_rs_maxf_k(kk, 0.0f);
            }
        }
        if (c->par != NULL) {
            for (ch = 0; ch < 3; ch++) {
                col[ch] = lut_eval(c->par, LUT_N, col[ch]);
            }
        }
        if (c->master != NULL) {
            for (ch = 0; ch < 3; ch++) {
                col[ch] = lut_eval(c->master, LUT_N, col[ch]);
            }
        }
        for (ch = 0; ch < 3; ch++) {
            if (c->cur[ch] != NULL) {
                col[ch] = lut_eval(c->cur[ch], LUT_N, col[ch]);
            }
        }
        if (c->hsl) {
            float pos[3], hsv[3];
            for (ch = 0; ch < 3; ch++) {
                pos[ch] = pc_rs_maxf_k(col[ch], 0.0f);
            }
            rgb_to_hsv(pos, hsv);
            if (hsv[1] > 1e-4f) {
                float wts[8], dh = 0.0f, ds = 0.0f, dl = 0.0f, nh, ns, out[3], l0, l1, kk;
                pc_algo_cr_band_weights(hsv[0], wts);
                for (k = 0; k < 8; k++) {
                    dh += wts[k] * p->hsl_hue[k];
                    ds += wts[k] * p->hsl_sat[k];
                    dl += wts[k] * p->hsl_lum[k];
                }
                nh = hsv[0] + dh / 100.0f * 30.0f;
                ns = pc_clampf(hsv[1] * (1.0f + ds / 100.0f), 0.0f, 1.0f);
                hsv_to_rgb(nh, ns, hsv[2], out);
                /* Keep the luminance, then apply the luminance shift weighted by saturation. */
                l0 = luma3(col[0], col[1], col[2]);
                l1 = luma3(out[0], out[1], out[2]);
                if (l1 > 1e-6f) {
                    for (ch = 0; ch < 3; ch++) {
                        out[ch] = out[ch] * l0 / l1;
                    }
                }
                kk = exp2f(dl / 100.0f * 1.0f * hsv[1]);
                for (ch = 0; ch < 3; ch++) {
                    col[ch] = out[ch] * kk;
                }
            }
        }
        if (c->grade) {
            float l = pc_clampf(luma3(col[0], col[1], col[2]), 0.0f, 1.0f);
            float sh = 1.0f - smooth(0.0f, 0.5f + c->bal + c->blend, l);
            float hi = smooth(0.5f + c->bal - c->blend, 1.0f, l);
            float mid = pc_rs_maxf_k(1.0f - sh - hi, 0.0f);
            float masks[4] = {sh, mid, hi, 1.0f};
            float lums[4] = {p->grade_shadows.lum, p->grade_midtones.lum, p->grade_highlights.lum,
                             p->grade_global.lum};
            for (k = 0; k < 4; k++) {
                for (ch = 0; ch < 3; ch++) {
                    col[ch] += masks[k] * c->tints[k][ch];
                }
                if (lums[k] != 0.0f) {
                    float f = exp2f(masks[k] * lums[k] / 100.0f * 0.6f);
                    for (ch = 0; ch < 3; ch++) {
                        col[ch] = col[ch] * f;
                    }
                }
            }
        }
        q[0] = col[0];
        q[1] = col[1];
        q[2] = col[2];
    }
    return PC_OK;
}

/* i64 + 1, wrapping as the reference's release build does. */
static int64_t wrap_inc(int64_t v)
{
    return v == INT64_MAX ? INT64_MIN : v + 1;
}

static float value_noise(float x, float y, uint64_t seed)
{
    float xi = floorf(x), yi = floorf(y), tx = x - xi, ty = y - yi;
    float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
    /* `xi as i64 + dx` wraps in the reference's release build. */
    int64_t ix = pc_f32_to_i64(xi), iy = pc_f32_to_i64(yi), ix1 = wrap_inc(ix), iy1 = wrap_inc(iy);
    float h00 = pc_algo_photo_hash01(ix, iy, seed), h10 = pc_algo_photo_hash01(ix1, iy, seed);
    float h01 = pc_algo_photo_hash01(ix, iy1, seed), h11 = pc_algo_photo_hash01(ix1, iy1, seed);
    float top = h00 + (h10 - h00) * sx;
    float bot = h01 + (h11 - h01) * sx;
    return top + (bot - top) * sy;
}

static pc_status grain_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    dev_ctx *c = vc;
    size_t y, x;
    int k;
    (void)chunk;
    for (y = y0; y < y1; y++) {
        for (x = 0; x < c->w; x++) {
            float *q = c->px[y * c->w + x];
            /* Sample in full-resolution coordinates so previews match the result. */
            float fx = (float)x * c->pscale / c->cell, fy = (float)y * c->pscale / c->cell;
            float n = value_noise(fx, fy, c->seed) * (1.0f - c->rough) +
                      (value_noise(fx * 2.3f, fy * 2.3f, c->seed + 7) - 0.0f) * c->rough;
            float l = pc_clampf(luma3(q[0], q[1], q[2]), 0.0f, 1.0f);
            float g = (n - 0.5f) * 2.0f * c->amt * (0.4f + 2.4f * l * (1.0f - l));
            for (k = 0; k < 3; k++) {
                q[k] += g;
            }
        }
    }
    return PC_OK;
}

/* ---- dehaze ---- */

/* Separable min filter of radius r over a sw x sh field. */
static pc_status min_filter(const float *v, size_t sw, size_t sh, size_t r, float **out, pc_error *err)
{
    float *a = NULL, *b = NULL;
    size_t x, y, k;
    pc_status st;
    if ((st = pc_algo_l9u_alloc_f(sw * sh, false, &a, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(sw * sh, false, &b, err)) != PC_OK) {
        free(a);
        return st;
    }
    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            size_t x0 = x > r ? x - r : 0, x1 = x + r < sw - 1 ? x + r : sw - 1;
            float m = 3.40282347e38f;
            for (k = x0; k <= x1; k++) {
                m = pc_rs_minf(m, v[y * sw + k]);
            }
            a[y * sw + x] = m;
        }
    }
    for (y = 0; y < sh; y++) {
        for (x = 0; x < sw; x++) {
            size_t y0 = y > r ? y - r : 0, y1 = y + r < sh - 1 ? y + r : sh - 1;
            float m = 3.40282347e38f;
            for (k = y0; k <= y1; k++) {
                m = pc_rs_minf(m, a[k * sw + x]);
            }
            b[y * sw + x] = m;
        }
    }
    free(a);
    *out = b;
    return PC_OK;
}

typedef struct dark_ent {
    float v;
    size_t i;
} dark_ent;

/* Descending by total_cmp, stable. */
static int cmp_dark(const void *pa, const void *pb)
{
    const dark_ent *a = pa, *b = pb;
    int c = pc_algo_l9u_total_cmpf(b->v, a->v);
    return c != 0 ? c : (a->i < b->i ? -1 : (a->i > b->i ? 1 : 0));
}

typedef struct haze_ctx {
    float (*px)[4];
    size_t w, sw, sh;
    const float *t;
    float air[3], ks, amount;
} haze_ctx;

static pc_status haze_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    haze_ctx *c = vc;
    size_t y, x;
    int k;
    (void)chunk;
    for (y = y0; y < y1; y++) {
        for (x = 0; x < c->w; x++) {
            float *q = c->px[y * c->w + x];
            float tv = pc_clampf(pc_algo_photo_bilinear(c->t, c->sw * c->sh, c->sw, c->sh, 1, 0,
                                                        ((float)x + 0.5f) / c->ks - 0.5f,
                                                        ((float)y + 0.5f) / c->ks - 0.5f),
                                 0.1f, 1.0f);
            for (k = 0; k < 3; k++) {
                float i = q[k], j = (i - c->air[k]) / tv + c->air[k];
                q[k] = c->amount >= 0.0f ? i + (j - i) * c->amount
                                         : i + (c->air[k] - i) * (-c->amount) * 0.5f * (1.0f - tv * 0.5f);
            }
        }
    }
    return PC_OK;
}

static pc_status dehaze(float (*px)[4], size_t w, size_t h, float amount, pc_error *err)
{
    size_t k = ((w > h ? w : h) + 767) / 768, sw, sh, n, i, x, y, r, top;
    float (*small)[3] = NULL, *cnt = NULL, *tmp = NULL, *dark = NULL, *ndark = NULL, *guide = NULL, *t = NULL;
    float air[3] = {0.0f, 0.0f, 0.0f};
    dark_ent *idx = NULL;
    haze_ctx hc;
    pc_status st;
    int c;
    k = k < 1 ? 1 : k;
    sw = (w + k - 1) / k;
    sh = (h + k - 1) / k;
    n = sw * sh;
    small = pc_calloc_array(n, sizeof *small);
    if (small == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if ((st = pc_algo_l9u_alloc_f(n, true, &cnt, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &tmp, err)) != PC_OK) {
        goto done;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            i = (y / k) * sw + x / k;
            for (c = 0; c < 3; c++) {
                small[i][c] += pc_clampf(px[y * w + x][c], 0.0f, 1.0f);
            }
            cnt[i] += 1.0f;
        }
    }
    for (i = 0; i < n; i++) {
        for (c = 0; c < 3; c++) {
            small[i][c] = small[i][c] / pc_rs_maxf_k(cnt[i], 1.0f);
        }
    }
    r = (sw > sh ? sw : sh) / 60;
    r = r < 2 ? 2 : r;
    for (i = 0; i < n; i++) {
        tmp[i] = pc_rs_minf(pc_rs_minf(small[i][0], small[i][1]), small[i][2]);
    }
    if ((st = min_filter(tmp, sw, sh, r, &dark, err)) != PC_OK) {
        goto done;
    }
    /* Atmospheric light: mean colour of the brightest 0.1 % of the dark channel. */
    idx = pc_malloc_array(n, sizeof *idx);
    if (idx == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto done;
    }
    for (i = 0; i < n; i++) {
        idx[i].v = dark[i];
        idx[i].i = i;
    }
    qsort(idx, n, sizeof *idx, cmp_dark);
    top = n / 1000 < 1 ? 1 : n / 1000;
    for (i = 0; i < top; i++) {
        for (c = 0; c < 3; c++) {
            air[c] += small[idx[i].i][c] / (float)top;
        }
    }
    for (c = 0; c < 3; c++) {
        air[c] = pc_rs_maxf_k(air[c], 0.2f);
    }
    for (i = 0; i < n; i++) {
        tmp[i] = pc_rs_minf(pc_rs_minf(small[i][0] / air[0], small[i][1] / air[1]), small[i][2] / air[2]);
    }
    if ((st = min_filter(tmp, sw, sh, r, &ndark, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &guide, err)) != PC_OK) {
        goto done;
    }
    for (i = 0; i < n; i++) {
        tmp[i] = 1.0f - 0.95f * ndark[i];
        guide[i] = luma3(small[i][0], small[i][1], small[i][2]);
    }
    if ((st = guided(guide, tmp, sw, sh, r * 2, 1e-3f, &t, err)) != PC_OK) {
        goto done;
    }
    hc.px = px;
    hc.w = w;
    hc.sw = sw;
    hc.sh = sh;
    hc.t = t;
    memcpy(hc.air, air, sizeof air);
    hc.ks = (float)k;
    hc.amount = amount;
    if ((st = pc_algo_par_for(h, ROWS, haze_rows, &hc)) != PC_OK) {
        (void)pc_fail(err, st, "dehaze failed");
    }
done:
    free(small), free(cnt), free(tmp), free(dark), free(ndark), free(guide), free(t), free(idx);
    return st;
}

/* ---- detail ---- */

static pc_status detail(float (*px)[4], size_t w, size_t h, const pc_algo_camera_raw *p, float ps,
                        pc_error *err)
{
    size_t n = w * h, i;
    float *y = NULL, *cb = NULL, *cr = NULL, *a = NULL, *b = NULL, *mask = NULL;
    pc_status st;
    if ((st = pc_algo_l9u_alloc_f(n, false, &y, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &cb, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &cr, err)) != PC_OK) {
        goto done;
    }
    /* Y + chroma (BT.709 luma, B-Y, R-Y). */
    for (i = 0; i < n; i++) {
        y[i] = luma3(px[i][0], px[i][1], px[i][2]);
        cb[i] = px[i][2] - y[i];
        cr[i] = px[i][0] - y[i];
    }
    if (p->noise_luminance > 0.0f) {
        float nl = p->noise_luminance / 100.0f, t = nl * 0.06f, keep;
        size_t r = pc_f32_to_size(pc_rs_maxf_k(roundf((1.0f + 3.0f * nl) * ps), 1.0f));
        if ((st = guided(y, y, w, h, r, t * t + 1e-6f, &a, err)) != PC_OK) {
            goto done;
        }
        keep = p->noise_luminance_detail / 100.0f * 0.5f;
        for (i = 0; i < n; i++) {
            y[i] = a[i] + (y[i] - a[i]) * keep;
        }
        free(a);
        a = NULL;
    }
    if (p->noise_color > 0.0f) {
        float sigma = pc_rs_maxf_k(p->noise_color / 100.0f * 6.0f * ps, 0.3f), keep;
        if ((st = gauss(cb, w, h, sigma, &a, err)) != PC_OK ||
            (st = gauss(cr, w, h, sigma, &b, err)) != PC_OK) {
            goto done;
        }
        keep = p->noise_color_detail / 100.0f * 0.3f;
        for (i = 0; i < n; i++) {
            cb[i] = a[i] + (cb[i] - a[i]) * keep;
            cr[i] = b[i] + (cr[i] - b[i]) * keep;
        }
        free(a);
        free(b);
        a = b = NULL;
    }
    if (p->sharpen_amount > 0.0f) {
        float sigma = pc_rs_maxf_k(pc_clampf(p->sharpen_radius, 0.5f, 3.0f) * ps, 0.3f), amt, det, limit;
        if ((st = gauss(y, w, h, sigma, &a, err)) != PC_OK) {
            goto done;
        }
        /* Masking: an edge mask from the gradient of a smoothed luminance. */
        if (p->sharpen_masking > 0.0f) {
            float t = p->sharpen_masking / 100.0f * 0.08f;
            size_t x, yy;
            if ((st = gauss(y, w, h, 1.0f * pc_rs_maxf_k(ps, 0.5f), &b, err)) != PC_OK ||
                (st = pc_algo_l9u_alloc_f(n, false, &mask, err)) != PC_OK) {
                goto done;
            }
            for (i = 0; i < n; i++) {
                float gx, gy;
                x = i % w;
                yy = i / w;
                gx = b[yy * w + (x + 1 < w - 1 ? x + 1 : w - 1)] - b[yy * w + (x > 0 ? x - 1 : 0)];
                gy = b[(yy + 1 < h - 1 ? yy + 1 : h - 1) * w + x] - b[(yy > 0 ? yy - 1 : 0) * w + x];
                mask[i] = smooth(t * 0.5f, t + 1e-4f, hypotf(gx, gy));
            }
        }
        amt = p->sharpen_amount / 100.0f * 1.2f;
        det = p->sharpen_detail / 100.0f;
        for (i = 0; i < n; i++) {
            float hp = y[i] - a[i];
            /* Low Detail damps large (halo-producing) differences. */
            limit = 0.03f + det * 0.3f;
            hp = pc_clampf(hp, -limit, limit) * (0.5f + 0.5f * det);
            y[i] += amt * hp * (mask != NULL ? mask[i] : 1.0f);
        }
    }
    for (i = 0; i < n; i++) {
        float l = y[i], bb = cb[i] + l, rr = cr[i] + l;
        float g = (l - 0.2126f * rr - 0.0722f * bb) / 0.7152f;
        px[i][0] = rr;
        px[i][1] = g;
        px[i][2] = bb;
    }
done:
    free(y), free(cb), free(cr), free(a), free(b), free(mask);
    return st;
}

/* ---- vignette ---- */

typedef struct vig_ctx {
    float (*px)[4];
    size_t w;
    float amt, hl, cx, cy, ax, ay, pw, edge0, edge1;
    int style; /* 0 highlight priority, 1 colour priority, 2 paint overlay */
} vig_ctx;

static pc_status vig_rows(void *vc, size_t chunk, size_t y0, size_t y1)
{
    vig_ctx *c = vc;
    size_t y, x;
    int k;
    (void)chunk;
    for (y = y0; y < y1; y++) {
        for (x = 0; x < c->w; x++) {
            float *q = c->px[y * c->w + x];
            float dx = fabsf(((float)x + 0.5f - c->cx) / c->ax),
                  dy = fabsf(((float)y + 0.5f - c->cy) / c->ay);
            float r = powf(powf(dx, c->pw) + powf(dy, c->pw), 1.0f / c->pw);
            float m = smooth(c->edge0, c->edge1, r), l;
            if (m <= 0.0f) {
                continue;
            }
            l = pc_clampf(luma3(q[0], q[1], q[2]), 0.0f, 1.0f);
            if (c->style == 2) {
                float target = c->amt < 0.0f ? 0.0f : 1.0f, kk = m * fabsf(c->amt);
                for (k = 0; k < 3; k++) {
                    q[k] += (target - q[k]) * kk;
                }
            } else {
                float kk = exp2f(c->amt * 2.0f * m);
                if (c->amt < 0.0f && c->hl > 0.0f) {
                    /* Highlights: let bright areas punch through a darkening vignette. */
                    float protect = smooth(0.6f, 1.0f, l) * c->hl;
                    kk += (1.0f - kk) * protect;
                }
                if (c->style == 1) {
                    float nl = l * kk;
                    for (k = 0; k < 3; k++) {
                        q[k] += nl - l;
                    }
                } else {
                    for (k = 0; k < 3; k++) {
                        q[k] *= kk;
                    }
                }
            }
        }
    }
    return PC_OK;
}

static pc_status vignette(float (*px)[4], size_t w, size_t h, const pc_algo_camera_raw *p, pc_error *err)
{
    vig_ctx c;
    const char *style = pc_algo_camera_raw_style(p);
    float mid = pc_clampf(p->vignette_midpoint, 0.0f, 100.0f) / 100.0f;
    float feather = pc_clampf(p->vignette_feather, 0.0f, 100.0f) / 100.0f;
    float round = pc_clampf(p->vignette_roundness, -100.0f, 100.0f) / 100.0f, inner;
    pc_status st;
    c.px = px;
    c.w = w;
    c.amt = p->vignette_amount / 100.0f;
    c.hl = pc_clampf(p->vignette_highlights, 0.0f, 100.0f) / 100.0f;
    c.style = strcmp(style, "paintOverlay") == 0 ? 2 : (strcmp(style, "colorPriority") == 0 ? 1 : 0);
    c.cx = (float)w / 2.0f;
    c.cy = (float)h / 2.0f;
    /* Roundness: 0 follows the frame's aspect, +1 a circle, -1 a rounded rectangle. */
    if (round >= 0.0f) {
        float lo = pc_rs_minf(c.cx, c.cy), hi = pc_rs_maxf(c.cx, c.cy);
        float r = lo + (hi - lo) * (1.0f - round);
        c.ax = c.cx >= c.cy ? r : c.cx;
        c.ay = c.cy > c.cx ? r : c.cy;
    } else {
        c.ax = c.cx;
        c.ay = c.cy;
    }
    c.pw = 2.0f + pc_rs_maxf_k(-round, 0.0f) * 6.0f;
    inner = 1.0f - mid * 0.8f;
    c.edge0 = inner - feather * 0.5f * inner;
    c.edge1 = inner + feather * 0.5f * (1.4f - inner) + 0.02f;
    if ((st = pc_algo_par_for(h, ROWS, vig_rows, &c)) != PC_OK) {
        return pc_fail(err, st, "vignette failed");
    }
    return PC_OK;
}

/* ---- pipeline ---- */

/* Checks what the reference would panic on or run for hours with, before touching pixels. */
static pc_status precheck(const pc_algo_camera_raw *p, float ps, pc_error *err)
{
    size_t radii[3];
    bool skip;
    pc_status st;
    if (p->noise_luminance > 0.0f) {
        float nl = p->noise_luminance / 100.0f;
        size_t r = pc_f32_to_size(pc_rs_maxf_k(roundf((1.0f + 3.0f * nl) * ps), 1.0f));
        if (r > PC_ALGO_MAX_BOX_RADIUS) {
            return pc_fail(err, PC_ERR_LIMIT, "luminance noise reduction radius %zu is too large", r);
        }
    }
    if (p->noise_color > 0.0f && (st = gauss_radii(pc_rs_maxf_k(p->noise_color / 100.0f * 6.0f * ps, 0.3f),
                                                   radii, &skip, err)) != PC_OK) {
        return st;
    }
    if (p->sharpen_amount > 0.0f) {
        float limit = 0.03f + p->sharpen_detail / 100.0f * 0.3f;
        if (!(-limit <= limit)) {
            return pc_fail(err, PC_ERR_INVALID, "sharpenDetail %g is out of range",
                           (double)p->sharpen_detail);
        }
    }
    return PC_OK;
}

static pc_status run_rows(size_t h, pc_pool_fn fn, dev_ctx *c, pc_error *err)
{
    pc_status st = pc_algo_par_for(h, ROWS, fn, c);
    return st == PC_OK ? PC_OK : pc_fail(err, st, "develop failed");
}

static pc_status tone_stage(dev_ctx *c, float ps, pc_error *err)
{
    const pc_algo_camera_raw *p = c->p;
    size_t n = c->w * c->h, i, w = c->w, h = c->h;
    float long_side = (float)(w > h ? w : h);
    float *l = NULL, *lc = NULL, *clar = NULL, *tex = NULL;
    pc_status st;
    if ((st = pc_algo_l9u_alloc_f(n, false, &l, err)) != PC_OK ||
        (st = pc_algo_l9u_alloc_f(n, false, &lc, err)) != PC_OK) {
        goto done;
    }
    for (i = 0; i < n; i++) {
        l[i] = pc_rs_maxf_k(luma3(c->px[i][0], c->px[i][1], c->px[i][2]), 0.0f);
        lc[i] = pc_rs_minf_k(l[i], 1.0f);
    }
    if (p->clarity != 0.0f) {
        size_t r = pc_f32_to_size(long_side * 0.02f);
        if ((st = guided_fast(lc, lc, w, h, r < 3 ? 3 : r, 0.005f, &clar, err)) != PC_OK) {
            goto done;
        }
    }
    if (p->texture != 0.0f &&
        (st = gauss(lc, w, h, pc_rs_maxf(long_side * 0.002f, 1.5f * ps), &tex, err)) != PC_OK) {
        goto done;
    }
    c->l = l;
    c->lc = lc;
    c->clar = clar;
    c->tex = tex;
    c->k_con = p->contrast / 100.0f * 0.6f;
    st = run_rows(h, tone_rows, c, err);
done:
    free(l), free(lc), free(clar), free(tex);
    return st;
}

static void wheel_tint(pc_algo_cr_wheel wh, float out[3])
{
    float c[3], l;
    int k;
    hsv_to_rgb(wh.hue, 1.0f, 1.0f, c);
    l = luma3(c[0], c[1], c[2]);
    for (k = 0; k < 3; k++) {
        out[k] = (c[k] - l) * wh.sat / 100.0f * 0.3f;
    }
}

static pc_status color_stage(dev_ctx *c, pc_error *err)
{
    const pc_algo_camera_raw *p = c->p;
    float *luts[5] = {NULL, NULL, NULL, NULL, NULL};
    const pc_algo_points *curves[4] = {&p->point_curve, &p->point_curve_red, &p->point_curve_green,
                                       &p->point_curve_blue};
    pc_status st = PC_OK;
    int k;
    if (!param_curve_neutral(p)) {
        if ((st = pc_algo_l9u_alloc_f(LUT_N, false, &luts[0], err)) != PC_OK) {
            goto done;
        }
        parametric_lut(p, luts[0], LUT_N);
    }
    for (k = 0; k < 4; k++) {
        if (!is_linear(curves[k])) {
            if ((st = pc_algo_l9u_alloc_f(LUT_N, false, &luts[k + 1], err)) != PC_OK ||
                (st = pc_algo_cr_curve_lut((const float (*)[2])curves[k]->v, curves[k]->n, luts[k + 1], LUT_N,
                                           err)) != PC_OK) {
                goto done;
            }
        }
    }
    c->par = luts[0];
    c->master = luts[1];
    c->cur[0] = luts[2];
    c->cur[1] = luts[3];
    c->cur[2] = luts[4];
    c->hsl = !hsl_neutral(p);
    c->grade = !grade_neutral(p);
    wheel_tint(p->grade_shadows, c->tints[0]);
    wheel_tint(p->grade_midtones, c->tints[1]);
    wheel_tint(p->grade_highlights, c->tints[2]);
    wheel_tint(p->grade_global, c->tints[3]);
    c->blend = 0.15f + pc_clampf(p->grade_blending, 0.0f, 100.0f) / 100.0f * 0.35f;
    c->bal = pc_clampf(p->grade_balance, -100.0f, 100.0f) / 100.0f * 0.25f;
    st = run_rows(c->h, color_rows, c, err);
done:
    for (k = 0; k < 5; k++) {
        free(luts[k]);
    }
    return st;
}

pc_status pc_algo_camera_raw_develop(float (*px)[4], size_t n, size_t w, size_t h,
                                     const pc_algo_camera_raw *p, bool float_doc, pc_error *err)
{
    size_t wh;
    float ps;
    dev_ctx c;
    pc_status st;
    if (!pc_size_mul(w, h, &wh) || wh != n) {
        return pc_fail(err, PC_ERR_INVALID, "pixel count %zu does not match %zu x %zu", n, w, h);
    }
    if (n == 0 || pc_algo_camera_raw_is_identity(p)) {
        return PC_OK;
    }
    ps = pc_clampf(p->pixel_scale, 0.01f, 1.0f);
    if ((st = precheck(p, ps, err)) != PC_OK) {
        return st;
    }
    memset(&c, 0, sizeof c);
    c.px = px;
    c.w = w;
    c.h = h;
    c.p = p;
    /* 1. White balance + exposure in linear light. */
    if (!wb_neutral(p)) {
        float g[3], norm, e = exp2f(p->exposure);
        int k;
        pc_algo_cr_white_balance_gains(p->temperature, p->tint, g);
        norm = luma3(g[0], g[1], g[2]);
        for (k = 0; k < 3; k++) {
            c.g[k] = g[k] / norm * e;
        }
        if ((st = run_rows(h, wb_rows, &c, err)) != PC_OK) {
            return st;
        }
    }
    /* 2. Dehaze. */
    if (p->dehaze != 0.0f && (st = dehaze(px, w, h, p->dehaze / 100.0f, err)) != PC_OK) {
        return st;
    }
    /* 3. Tone. Highlights and Shadows: a local Laplacian filter (camera_raw_tone.c). */
    if ((p->highlights != 0.0f || p->shadows != 0.0f) &&
        (st = pc_algo_cr_local_tone(px, w, h, p->shadows, p->highlights,
                                    pc_algo_cr_encoding_of(p->encoding_gamma), err)) != PC_OK) {
        return st;
    }
    if (!local_tone_neutral(p) && (p->contrast != 0.0f || p->texture != 0.0f || p->clarity != 0.0f) &&
        (st = tone_stage(&c, ps, err)) != PC_OK) {
        return st;
    }
    /* 3b. Whites and Blacks: global curves adapted to the image's black and white points. */
    if ((p->whites != 0.0f || p->blacks != 0.0f) &&
        (st = pc_algo_cr_whites_blacks(px, w, h, p->whites, p->blacks,
                                       pc_algo_cr_encoding_of(p->encoding_gamma), err)) != PC_OK) {
        return st;
    }
    /* 4-7. Per-pixel colour stages. */
    if ((p->vibrance != 0.0f || p->saturation != 0.0f || !curves_neutral(p) || !hsl_neutral(p) ||
         !grade_neutral(p)) &&
        (st = color_stage(&c, err)) != PC_OK) {
        return st;
    }
    /* 8. Detail. */
    if ((p->noise_luminance > 0.0f || p->noise_color > 0.0f || p->sharpen_amount > 0.0f) &&
        (st = detail(px, w, h, p, ps, err)) != PC_OK) {
        return st;
    }
    /* 9. Effects. */
    if (p->vignette_amount != 0.0f && (st = vignette(px, w, h, p, err)) != PC_OK) {
        return st;
    }
    if (p->grain_amount > 0.0f) {
        c.cell = 1.0f + p->grain_size / 25.0f;
        c.rough = p->grain_roughness / 100.0f;
        c.amt = p->grain_amount / 100.0f * 0.12f;
        c.seed = (uint64_t)p->seed;
        c.pscale = 1.0f / ps;
        if ((st = run_rows(h, grain_rows, &c, err)) != PC_OK) {
            return st;
        }
    }
    if (!float_doc) {
        size_t i;
        int k;
        for (i = 0; i < n; i++) {
            for (k = 0; k < 3; k++) {
                px[i][k] = pc_clampf(px[i][k], 0.0f, 1.0f);
            }
        }
    }
    return PC_OK;
}
