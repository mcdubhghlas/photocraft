/* Dodge / Burn / Sponge / Blur / Sharpen kernels (port of crates/algo/src/retouch.rs). */
#include "algo/retouch.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* The window pass costs (h + 2r) x (2r + 1) per column: past this the reference runs for hours. */
#define MAX_BLUR_RADIUS ((int64_t)1 << 12)

static const char *const RANGE_NAMES[3] = {"shadows", "midtones", "highlights"};

const char *pc_algo_tone_range_name(pc_algo_tone_range r)
{
    return (unsigned)r < 3u ? RANGE_NAMES[r] : NULL;
}

bool pc_algo_tone_range_from_name(const char *s, size_t len, pc_algo_tone_range *out)
{
    size_t i;
    for (i = 0; s != NULL && i < 3; i++) {
        if (strlen(RANGE_NAMES[i]) == len && memcmp(RANGE_NAMES[i], s, len) == 0) {
            *out = (pc_algo_tone_range)i;
            return true;
        }
    }
    return false;
}

float pc_algo_retouch_luma(const float c[3])
{
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

float pc_algo_retouch_tone_curve(float v, float e, pc_algo_tone_range range, bool burn)
{
    float v1, w;
    v = pc_rs_maxf_k(v, 0.0f);
    e = pc_clampf(e, 0.0f, 1.0f);
    v1 = pc_rs_minf_k(v, 1.0f);
    switch (range) {
    case PC_ALGO_TONE_MIDTONES:
        /* Gamma bend, weighted to peak at mid-grey: 0 and 1 are fixed points. */
        w = pc_clampf(4.0f * v * (1.0f - v), 0.0f, 1.0f);
        return v + w * (powf(v1, burn ? 1.0f + e : 1.0f / (1.0f + e)) - v);
    case PC_ALGO_TONE_SHADOWS: {
        /* v + e * w * (target - v) with weight (1 - v)^2: black lifts most when dodging. */
        float q = 1.0f - v1;
        return burn ? v - e * (q * q) * v : v + e * (q * q) * (1.0f - v1);
    }
    case PC_ALGO_TONE_HIGHLIGHTS:
        return burn ? v - e * (v1 * v1) * v : v + e * (v1 * v1) * (1.0f - v1);
    }
    return v;
}

/* Bring an RGB triple back into 0..1 keeping its luma (mix toward grey) instead of clipping each
 * channel (which would shift hue). */
static void compress_gamut(const float c[3], float l, float out[3])
{
    float lo = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]), hi = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]);
    float t = 1.0f, r0 = c[0], r1 = c[1], r2 = c[2];
    l = pc_clampf(l, 0.0f, 1.0f);
    if (hi > 1.0f && hi - l > 1e-9f) {
        t = pc_rs_minf(t, (1.0f - l) / (hi - l));
    }
    if (lo < 0.0f && l - lo > 1e-9f) {
        t = pc_rs_minf(t, l / (l - lo));
    }
    out[0] = l + (r0 - l) * t;
    out[1] = l + (r1 - l) * t;
    out[2] = l + (r2 - l) * t;
}

void pc_algo_retouch_dodge_burn(const float c[3], float e, pc_algo_tone_range range, bool burn,
                                bool protect_tones, float out[3])
{
    float t[3];
    if (e <= 0.0f) {
        memmove(out, c, 3 * sizeof(float));
        return;
    }
    if (protect_tones) {
        float l = pc_algo_retouch_luma(c), nl = pc_algo_retouch_tone_curve(l, e, range, burn);
        if (l > 1e-6f) {
            t[0] = c[0] * nl / l, t[1] = c[1] * nl / l, t[2] = c[2] * nl / l;
        } else {
            t[0] = t[1] = t[2] = nl;
        }
        compress_gamut(t, nl, out);
    } else {
        t[0] = pc_algo_retouch_tone_curve(c[0], e, range, burn);
        t[1] = pc_algo_retouch_tone_curve(c[1], e, range, burn);
        t[2] = pc_algo_retouch_tone_curve(c[2], e, range, burn);
        memcpy(out, t, sizeof t);
    }
}

float pc_algo_retouch_saturation(const float c[3])
{
    float lo = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]), hi = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]);
    return hi <= 1e-6f ? 0.0f : pc_clampf((hi - lo) / hi, 0.0f, 1.0f);
}

void pc_algo_retouch_sponge(const float c[3], float amount, bool saturate, bool vibrance, float out[3])
{
    float a = pc_clampf(amount, 0.0f, 1.0f), s, l, k, t[3];
    if (a <= 0.0f) {
        memmove(out, c, 3 * sizeof(float));
        return;
    }
    s = pc_algo_retouch_saturation(c);
    if (vibrance) {
        a *= saturate ? 1.0f - s : 0.25f + 0.75f * s;
    }
    l = pc_algo_retouch_luma(c);
    k = saturate ? 1.0f + a : 1.0f - a;
    t[0] = l + (c[0] - l) * k, t[1] = l + (c[1] - l) * k, t[2] = l + (c[2] - l) * k;
    compress_gamut(t, l, out);
}

static pc_status check_window(const float *data, size_t len, size_t w, size_t h, size_t ch, size_t alpha,
                              const size_t win[4], pc_error *err)
{
    size_t n;
    if (win == NULL || win[0] > win[2] || win[1] > win[3]) {
        return pc_fail(err, PC_ERR_INVALID, "invalid window");
    }
    if (win[0] == win[2] || win[1] == win[3]) {
        return PC_OK; /* empty window: nothing is read */
    }
    if (w == 0 || h == 0 || win[2] > w || win[3] > h || w > INT32_MAX / 2 || h > INT32_MAX / 2) {
        return pc_fail(err, PC_ERR_INVALID, "window outside the %zu x %zu buffer", w, h);
    }
    if (!pc_size_mul3(w, h, ch, &n) || n != len || data == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "buffer holds %zu samples, expected %zu x %zu x %zu", len, w, h,
                       ch);
    }
    if (alpha != SIZE_MAX && alpha >= ch) {
        return pc_fail(err, PC_ERR_INVALID, "alpha channel %zu of %zu", alpha, ch);
    }
    return PC_OK;
}

pc_status pc_algo_retouch_local_blur(const float *data, size_t len, size_t w, size_t h, size_t ch,
                                     size_t alpha, const size_t win[4], float sigma, float **out,
                                     pc_error *err)
{
    size_t ow, oh, th, ksz, i, ox, oy, ty, c;
    int64_t r, q;
    float *k = NULL, *tmp = NULL, *o = NULL, *px = NULL, ks = -0.0f;
    pc_status st = check_window(data, len, w, h, ch, alpha, win, err);
    if (st != PC_OK) {
        return st;
    }
    ow = win[2] - win[0], oh = win[3] - win[1];
    r = (int64_t)pc_f32_to_i32(pc_rs_maxf_k(ceilf(sigma * 3.0f), 1.0f));
    if (r > MAX_BLUR_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "blur radius %lld too large", (long long)r);
    }
    ksz = (size_t)(2 * r + 1);
    th = oh + 2 * (size_t)r;
    k = pc_malloc_array(ksz, sizeof *k);
    tmp = pc_calloc_array(ow * th, ch * sizeof(float));
    o = pc_calloc_array(ow * oh, ch * sizeof(float));
    px = pc_malloc_array(ch, sizeof(float));
    if (k == NULL || tmp == NULL || o == NULL || px == NULL || (ow > 0 && th > SIZE_MAX / ow)) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    for (q = -r; q <= r; q++) {
        /* (i * i) is i32 arithmetic in the reference (wraps past 46340). */
        int32_t qi = (int32_t)q;
        float num = -(float)pc_i32_wrapping_mul(qi, qi);
        k[q + r] = expf(num / pc_rs_maxf_k(2.0f * sigma * sigma, 1e-6f));
    }
    for (i = 0; i < ksz; i++) {
        ks += k[i];
    }
    for (i = 0; i < ksz; i++) {
        k[i] /= ks;
    }
    if (ow == 0 || oh == 0) {
        goto cleanup;
    }
    /* Horizontal pass over the window rows extended vertically by r. */
    for (ty = 0; ty < th; ty++) {
        int64_t y = (int64_t)win[1] + (int64_t)ty - r;
        size_t yy = (size_t)pc_clamp_i64(y, 0, (int64_t)h - 1);
        for (ox = 0; ox < ow; ox++) {
            size_t ob = (ty * ow + ox) * ch;
            for (i = 0; i < ksz; i++) {
                int64_t x = (int64_t)win[0] + (int64_t)ox + (int64_t)i - r;
                size_t xx = (size_t)pc_clamp_i64(x, 0, (int64_t)w - 1), si = (yy * w + xx) * ch;
                memcpy(px, &data[si], ch * sizeof(float));
                if (alpha != SIZE_MAX) {
                    float av = px[alpha];
                    for (c = 0; c < ch; c++) {
                        if (c != alpha) {
                            px[c] *= av;
                        }
                    }
                }
                for (c = 0; c < ch; c++) {
                    tmp[ob + c] += px[c] * k[i];
                }
            }
        }
    }
    for (oy = 0; oy < oh; oy++) {
        for (ox = 0; ox < ow; ox++) {
            size_t ob = (oy * ow + ox) * ch;
            for (i = 0; i < ksz; i++) {
                size_t s = ((oy + i) * ow + ox) * ch;
                for (c = 0; c < ch; c++) {
                    o[ob + c] += tmp[s + c] * k[i];
                }
            }
            if (alpha != SIZE_MAX) {
                float av = o[ob + alpha];
                for (c = 0; c < ch; c++) {
                    if (c != alpha) {
                        o[ob + c] =
                            av > 1e-6f ? o[ob + c] / av : data[((win[1] + oy) * w + win[0] + ox) * ch + c];
                    }
                }
            }
        }
    }
cleanup:
    free(k);
    free(tmp);
    free(px);
    if (st != PC_OK) {
        free(o);
        return st;
    }
    *out = o;
    return PC_OK;
}

pc_status pc_algo_retouch_local_sharpen(const float *data, size_t len, size_t w, size_t h, size_t ch,
                                        size_t alpha, const size_t win[4], float amount, bool protect_detail,
                                        float **out, pc_error *err)
{
    static const int DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, DY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    size_t ow, x, y, c;
    float *blur = NULL, *o;
    pc_status st = pc_algo_retouch_local_blur(data, len, w, h, ch, alpha, win, 1.0f, &blur, err);
    if (st != PC_OK) {
        return st;
    }
    ow = win[2] - win[0];
    o = pc_calloc_array((win[3] - win[1]) * ow, ch * sizeof(float));
    if (o == NULL) {
        free(blur);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (y = win[1]; y < win[3]; y++) {
        for (x = win[0]; x < win[2]; x++) {
            size_t ob = ((y - win[1]) * ow + (x - win[0])) * ch, ib = (y * w + x) * ch;
            for (c = 0; c < ch; c++) {
                float v = data[ib + c], s;
                if (c == alpha) {
                    o[ob + c] = v;
                    continue;
                }
                s = v + amount * (v - blur[ob + c]);
                if (protect_detail) {
                    float lo = v, hi = v;
                    int d;
                    for (d = 0; d < 8; d++) {
                        size_t nx = (size_t)pc_clamp_i64((int64_t)x + DX[d], 0, (int64_t)w - 1);
                        size_t ny = (size_t)pc_clamp_i64((int64_t)y + DY[d], 0, (int64_t)h - 1);
                        float nv = data[(ny * w + nx) * ch + c];
                        lo = pc_rs_minf(lo, nv);
                        hi = pc_rs_maxf(hi, nv);
                    }
                    s = pc_clampf(s, lo, hi);
                }
                o[ob + c] = s;
            }
        }
    }
    free(blur);
    *out = o;
    return PC_OK;
}
