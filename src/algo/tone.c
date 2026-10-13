/* Shadows/Highlights defaults (the adjustment is in tone_sh.c), Replace Color, Match Color and HDR Toning
 * (port of crates/algo/src/tone.rs). */
#include "algo/tone.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "color/convert.h"

static float luma3(const float c[3])
{
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

static pc_status check_dims(size_t n, size_t w, size_t h, pc_error *err)
{
    size_t wh;
    if (!pc_size_mul(w, h, &wh) || wh != n) {
        return pc_fail(err, PC_ERR_INVALID, "%zu pixels given for a %zux%zu image", n, w, h);
    }
    return PC_OK;
}

pc_algo_shadows_highlights_params pc_algo_shadows_highlights_default(void)
{
    pc_algo_shadows_highlights_params p;
    p.shadow_amount = 35.0f;
    p.shadow_tone = 50.0f;
    p.shadow_radius = 30.0f;
    p.highlight_amount = 0.0f;
    p.highlight_tone = 50.0f;
    p.highlight_radius = 30.0f;
    p.color = 20.0f;
    p.midtone = 0.0f;
    p.black_clip = 0.01f;
    p.white_clip = 0.01f;
    return p;
}

static size_t row_grain(size_t w)
{
    size_t g = w > 0 ? 8192 / w : 1;
    return g > 0 ? g : 1;
}

void pc_algo_replace_color(float (*px)[4], size_t n, const float color[3], float fuzziness, float hue,
                           float saturation, float lightness, float *mask)
{
    size_t i;
    for (i = 0; i < n; i++) {
        float k, c[3], hsl[3], s, l, o[3], t[3];
        int j;
        float one;
        pc_algo_color_range((const float (*)[4]) & px[i], 1, color, fuzziness, &one);
        if (mask != NULL) {
            mask[i] = one;
        }
        k = one;
        if (k <= 0.0f) {
            continue;
        }
        c[0] = px[i][0], c[1] = px[i][1], c[2] = px[i][2];
        pc_algo_rgb_to_hsl(c, hsl);
        s = saturation >= 0.0f ? hsl[1] + (1.0f - hsl[1]) * saturation / 100.0f
                               : hsl[1] * (1.0f + saturation / 100.0f);
        l = lightness >= 0.0f ? hsl[2] + (1.0f - hsl[2]) * lightness / 100.0f
                              : hsl[2] * (1.0f + lightness / 100.0f);
        t[0] = pc_algo_rem_euclidf(hsl[0] + hue / 360.0f, 1.0f);
        t[1] = pc_clampf(s, 0.0f, 1.0f);
        t[2] = pc_clampf(l, 0.0f, 1.0f);
        pc_algo_hsl_to_rgb(t, o);
        for (j = 0; j < 3; j++) {
            px[i][j] = pc_clampf(c[j] + (o[j] - c[j]) * k, 0.0f, 1.0f);
        }
    }
}

bool pc_algo_lab_stats_compute(const float (*px)[4], size_t n, const float *mask, size_t mask_len,
                               pc_algo_lab_stats *out)
{
    double sum[3] = {0.0, 0.0, 0.0}, sq[3] = {0.0, 0.0, 0.0}, wsum = 0.0;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        float m = mask == NULL ? 1.0f : (i < mask_len ? mask[i] : 0.0f);
        double w = (double)(px[i][3] * m);
        float rgb[3], lab[3];
        if (w <= 0.0) {
            continue;
        }
        rgb[0] = px[i][0], rgb[1] = px[i][1], rgb[2] = px[i][2];
        pc_srgb_to_lab(rgb, lab);
        for (k = 0; k < 3; k++) {
            sum[k] += w * (double)lab[k];
            sq[k] += w * (double)lab[k] * (double)lab[k];
        }
        wsum += w;
    }
    if (wsum <= 0.0) {
        return false;
    }
    for (k = 0; k < 3; k++) {
        double m = sum[k] / wsum;
        out->mean[k] = (float)(sum[k] / wsum);
        out->std[k] = (float)sqrt(pc_rs_max_k(sq[k] / wsum - m * m, 0.0));
    }
    return true;
}

pc_algo_match_color pc_algo_match_color_default(void)
{
    pc_algo_match_color o = {100.0f, 100.0f, 0.0f, false};
    return o;
}

void pc_algo_match_color_apply(float (*px)[4], size_t n, const pc_algo_lab_stats *target,
                               const pc_algo_lab_stats *source, const pc_algo_match_color *o)
{
    const pc_algo_lab_stats *src = source != NULL ? source : target;
    float lum = o->luminance / 100.0f, inten = o->intensity / 100.0f;
    float fade = pc_clampf(o->fade / 100.0f, 0.0f, 1.0f);
    float scale[3], dst_mean[3];
    size_t i;
    int k;
    for (k = 0; k < 3; k++) {
        scale[k] = target->std[k] > 1e-4f ? src->std[k] / target->std[k] : 1.0f;
        dst_mean[k] = src->mean[k];
    }
    if (o->neutralize) {
        dst_mean[1] = 0.0f;
        dst_mean[2] = 0.0f;
    }
    for (i = 0; i < n; i++) {
        float c[3], lab[3], nl[3], rgb[3];
        if (!(px[i][3] > 0.0f)) {
            continue;
        }
        c[0] = px[i][0], c[1] = px[i][1], c[2] = px[i][2];
        pc_srgb_to_lab(c, lab);
        nl[0] = pc_clampf(((lab[0] - target->mean[0]) * scale[0] + dst_mean[0]) * lum, 0.0f, 100.0f);
        nl[1] = ((lab[1] - target->mean[1]) * scale[1] + dst_mean[1]) * inten;
        nl[2] = ((lab[2] - target->mean[2]) * scale[2] + dst_mean[2]) * inten;
        pc_lab_to_srgb(nl, rgb);
        for (k = 0; k < 3; k++) {
            px[i][k] = pc_clampf(rgb[k] + (c[k] - rgb[k]) * fade, 0.0f, 1.0f);
        }
    }
}

pc_algo_hdr_toning_params pc_algo_hdr_toning_default(void)
{
    pc_algo_hdr_toning_params p;
    memset(&p, 0, sizeof p);
    p.radius = 30.0f;
    p.strength = 0.5f;
    p.gamma = 1.0f;
    p.exposure = 0.0f;
    p.detail = 30.0f;
    p.saturation = 20.0f;
    return p;
}

/* f32::total_cmp as a signed integer key. */
static int32_t total_key(float v)
{
    uint32_t u;
    int32_t b;
    memcpy(&u, &v, sizeof u);
    b = pc_u32_as_i32(u);
    return b ^ (int32_t)((uint32_t)(b >> 31) >> 1);
}

typedef struct curve_pt {
    float x, y;
    size_t idx;
} curve_pt;

static int cmp_pt(const void *a, const void *b)
{
    const curve_pt *p = a, *q = b;
    int32_t ka = total_key(p->x), kb = total_key(q->x);
    if (ka != kb) {
        return ka < kb ? -1 : 1;
    }
    return p->idx < q->idx ? -1 : (p->idx > q->idx ? 1 : 0); /* sort_by is stable */
}

static float curve_eval(const curve_pt *pts, size_t n, float x)
{
    size_t i;
    if (n < 2) {
        return x;
    }
    if (x <= pts[0].x) {
        return pts[0].y;
    }
    for (i = 0; i + 1 < n; i++) {
        if (x <= pts[i + 1].x) {
            float span = pc_rs_maxf_k(pts[i + 1].x - pts[i].x, 1e-6f);
            return pts[i].y + (pts[i + 1].y - pts[i].y) * (x - pts[i].x) / span;
        }
    }
    return pts[n - 1].y;
}

typedef struct hdr_ctx {
    float (*px)[4];
    float *logl;
    const float *base;
    const curve_pt *curve;
    size_t ncurve, w;
    float mean, compress, detail, exposure, gamma;
    const pc_algo_hdr_toning_params *p;
} hdr_ctx;

#define HDR_EPS 1e-4f

static pc_status hdr_log(void *vctx, size_t chunk, size_t begin, size_t end)
{
    hdr_ctx *k = vctx;
    size_t i;
    (void)chunk;
    for (i = begin; i < end; i++) {
        k->logl[i] = logf(pc_rs_maxf_k(luma3(k->px[i]), 0.0f) + HDR_EPS);
    }
    return PC_OK;
}

static pc_status hdr_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const hdr_ctx *k = vctx;
    const pc_algo_hdr_toning_params *p = k->p;
    size_t i;
    (void)chunk;
    for (i = begin * k->w; i < end * k->w; i++) {
        float *q = k->px[i];
        float c[3], out[3], l0, new_log, l1, ratio, ll, mx, mn, sat_now, s, a, b;
        int j;
        if (q[3] <= 0.0f) {
            continue;
        }
        c[0] = q[0], c[1] = q[1], c[2] = q[2];
        l0 = pc_rs_maxf_k(luma3(c), 0.0f) + HDR_EPS;
        new_log = k->mean + (k->base[i] - k->mean) * k->compress + (k->logl[i] - k->base[i]) * k->detail;
        /* "lower gamma = more contrast": gamma divides the exponent (issue #710). */
        l1 = powf(pc_rs_maxf_k(expf(new_log) * k->exposure, 0.0f), 1.0f / k->gamma);
        a = 1.0f - l1;
        b = l1;
        l1 += p->shadow / 100.0f * (a * a * a) * 0.5f - p->highlight / 100.0f * (b * b * b) * 0.5f;
        l1 = curve_eval(k->curve, k->ncurve, pc_clampf(l1, 0.0f, 1.0f));
        ratio = l1 / l0;
        for (j = 0; j < 3; j++) {
            out[j] = c[j] * ratio;
        }
        /* Saturation then vibrance (less saturated colours move more), around the new luminance. */
        ll = luma3(out);
        mx = pc_rs_maxf(pc_rs_maxf(out[0], out[1]), out[2]);
        mn = pc_rs_minf(pc_rs_minf(out[0], out[1]), out[2]);
        sat_now = mx > 0.0f ? (mx - mn) / mx : 0.0f;
        s = 1.0f + p->saturation / 100.0f + p->vibrance / 100.0f * (1.0f - sat_now);
        for (j = 0; j < 3; j++) {
            q[j] = pc_clampf(ll + (out[j] - ll) * pc_rs_maxf_k(s, 0.0f), 0.0f, 1.0f);
        }
    }
    return PC_OK;
}

pc_status pc_algo_hdr_toning(float (*px)[4], size_t n, size_t w, size_t h, const pc_algo_hdr_toning_params *p,
                             pc_error *err)
{
    float *logl = NULL, *base = NULL;
    curve_pt *pts = NULL;
    float sum = -0.0f; /* Iterator::sum starts at -0.0 */
    size_t cnt = 0, i;
    hdr_ctx k;
    pc_status st;
    if (n == 0) {
        return PC_OK;
    }
    st = check_dims(n, w, h, err);
    if (st != PC_OK) {
        return st;
    }
    logl = pc_malloc_array(n, sizeof(float));
    base = pc_malloc_array(n, sizeof(float));
    if (p->curve_len > 0) {
        pts = pc_malloc_array(p->curve_len, sizeof *pts);
    }
    if (logl == NULL || base == NULL || (p->curve_len > 0 && pts == NULL)) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for HDR toning");
        goto done;
    }
    memset(&k, 0, sizeof k);
    k.px = px;
    k.logl = logl;
    st = pc_algo_par_for(n, 16384, hdr_log, &k);
    if (st != PC_OK) {
        st = pc_fail(err, st, "HDR toning failed");
        goto done;
    }
    memcpy(base, logl, n * sizeof(float));
    st = pc_algo_gauss_blur_n(base, w, h, 1, pc_rs_maxf_k(p->radius, 1.0f) / 2.0f, err);
    if (st != PC_OK) {
        goto done;
    }
    for (i = 0; i < n; i++) {
        if (px[i][3] > 0.0f) {
            sum += base[i];
            cnt++;
        }
    }
    for (i = 0; i < p->curve_len; i++) {
        pts[i].x = p->curve[i][0];
        pts[i].y = p->curve[i][1];
        pts[i].idx = i;
    }
    if (p->curve_len > 1) {
        qsort(pts, p->curve_len, sizeof *pts, cmp_pt);
    }
    k.base = base;
    k.curve = pts;
    k.ncurve = p->curve_len;
    k.w = w;
    k.mean = sum / (float)(cnt > 0 ? cnt : 1);
    k.compress = 1.0f / (1.0f + pc_rs_maxf_k(p->strength, 0.0f));
    k.detail = 1.0f + p->detail / 100.0f;
    k.exposure = exp2f(p->exposure); /* LLVM folds 2f32.powf(x) into exp2f */
    k.gamma = pc_clampf(p->gamma, 0.1f, 4.0f);
    k.p = p;
    st = pc_algo_par_for(h, row_grain(w), hdr_rows, &k);
    if (st != PC_OK) {
        st = pc_fail(err, st, "HDR toning failed");
    }
done:
    free(logl);
    free(base);
    free(pts);
    return st;
}
