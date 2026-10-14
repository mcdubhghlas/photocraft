/* Evaluation of adjustment layers on a composite buffer (port of crates/compose/src/adjust.rs).
 *
 * Formulas are documented approximations of Photoshop behaviour, matched against
 * Photoshop-rendered composites. Expression order follows the reference exactly (float results
 * are compared bit for bit with it). Tone curves are in adjust_tone.c. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "color/blend.h"
#include "compose/adjust_internal.h"
#include "compose/internal.h"

#define LUT PC_COMPOSE_LUT_SIZE

const float PC_COMPOSE_VIBRANCE_GREY[2] = {0.288f, 0.712f};
const float PC_COMPOSE_VIBRANCE_BOOST[7] = {0.5001f, 0.0887f, 0.4374f, 0.7573f, -0.8113f, 0.5154f, 2.8026f};

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

/* ---- transfer ---- */

pc_compose_transfer pc_compose_transfer_for_exposure(pc_compose_transfer t)
{
    /* RGB documents use a pure 2.2 power, not the sRGB curve (psd-tools
     * adjustment_nested_composition_4: an offset of 0.1738 lifts 76 to 134, not 136). */
    if (t.kind == PC_COMPOSE_TRANSFER_SRGB) {
        return (pc_compose_transfer){PC_COMPOSE_TRANSFER_GAMMA, 2.2f};
    }
    return t;
}

float pc_compose_transfer_decode(pc_compose_transfer t, float v)
{
    if (t.kind == PC_COMPOSE_TRANSFER_SRGB) {
        return pc_srgb_to_linear(pc_rs_maxf_k(v, 0.0f));
    }
    if (t.gamma == 1.0f) {
        return pc_rs_maxf_k(v, 0.0f);
    }
    return powf(pc_rs_maxf_k(v, 0.0f), t.gamma);
}

float pc_compose_transfer_encode(pc_compose_transfer t, float v)
{
    if (t.kind == PC_COMPOSE_TRANSFER_SRGB) {
        return pc_linear_to_srgb(pc_rs_maxf_k(v, 0.0f));
    }
    if (t.gamma == 1.0f) {
        return pc_rs_maxf_k(v, 0.0f);
    }
    return powf(pc_rs_maxf_k(v, 0.0f), 1.0f / t.gamma);
}

/* f32::rem_euclid. */
static float rem_euclid(float a, float b)
{
    float r = fmodf(a, b);
    return r < 0.0f ? r + fabsf(b) : r;
}

/* ---- small public helpers ---- */

void pc_compose_rgb_to_hsl(const float c[3], float *h, float *s, float *l)
{
    float mx = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]);
    float mn = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
    float ll = (mx + mn) / 2.0f;
    float d;
    float hh;
    if (fabsf(mx - mn) < 1e-7f) {
        *h = 0.0f;
        *s = 0.0f;
        *l = ll;
        return;
    }
    d = mx - mn;
    *s = ll > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    if (mx == c[0]) {
        hh = rem_euclid((c[1] - c[2]) / d, 6.0f);
    } else if (mx == c[1]) {
        hh = (c[2] - c[0]) / d + 2.0f;
    } else {
        hh = (c[0] - c[1]) / d + 4.0f;
    }
    *h = hh / 6.0f;
    *l = ll;
}

static float hsl_channel(float p, float q, float t)
{
    t = rem_euclid(t, 1.0f);
    if (t < 1.0f / 6.0f) {
        return p + (q - p) * 6.0f * t;
    }
    if (t < 0.5f) {
        return q;
    }
    if (t < 2.0f / 3.0f) {
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    }
    return p;
}

void pc_compose_hsl_to_rgb(float h, float s, float l, float out[3])
{
    float q;
    float p;
    if (s <= 0.0f) {
        out[0] = out[1] = out[2] = l;
        return;
    }
    q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    p = 2.0f * l - q;
    out[0] = hsl_channel(p, q, h + 1.0f / 3.0f);
    out[1] = hsl_channel(p, q, h);
    out[2] = hsl_channel(p, q, h - 1.0f / 3.0f);
}

void pc_compose_hue_range_tables(const pc_hue_range ranges[6], float *out)
{
    size_t k;
    size_t r;
    for (k = 0; k < LUT; k++) {
        float deg = (float)k / (float)(LUT - 1) * 360.0f;
        float acc[3] = {0.0f, 0.0f, 0.0f};
        for (r = 0; r < 6; r++) {
            float w;
            if (pc_hue_range_is_neutral(&ranges[r])) {
                continue;
            }
            w = pc_hue_range_weight(&ranges[r], deg);
            acc[0] = acc[0] + w * ranges[r].hue;
            acc[1] = acc[1] + w * ranges[r].saturation / 100.0f;
            acc[2] = acc[2] + w * ranges[r].lightness / 100.0f;
        }
        out[k] = acc[0];
        out[LUT + k] = acc[1];
        out[2 * LUT + k] = acc[2];
    }
}

static float lighten(float v, float l)
{
    return l > 0.0f ? v + (1.0f - v) * l : v * (1.0f + l);
}

void pc_compose_hue_saturation(const float c[3], float hue, float s, float l, bool colorize, float out[3])
{
    float hh;
    float ss;
    float ll;
    float lc[3];
    int i;
    if (colorize) {
        /* Colorize builds the colour at the lightened (or darkened) lightness, so black with
         * Lightness +30 becomes a 30 % lightness colour, not grey (Photoshop 27.11); at 0 %
         * saturation only the lightness is left (#2326). */
        pc_compose_rgb_to_hsl(c, &hh, &ss, &ll);
        pc_compose_hsl_to_rgb(rem_euclid(hue, 360.0f) / 360.0f, pc_rs_minf_k(fabsf(s), 1.0f), lighten(ll, l),
                              out);
        return;
    }
    /* Lightness first, then the hue rotates and the saturation of the result scales: raising it
     * divides by 1 - s, lowering multiplies by 1 + s (Photoshop 27.11). */
    for (i = 0; i < 3; i++) {
        lc[i] = lighten(c[i], l);
    }
    pc_compose_rgb_to_hsl(lc, &hh, &ss, &ll);
    hh = rem_euclid(hh + hue / 360.0f, 1.0f);
    ss = pc_clampf(s > 0.0f ? ss / pc_rs_maxf_k(1.0f - s, 1.0f / 512.0f) : ss * (1.0f + s), 0.0f, 1.0f);
    pc_compose_hsl_to_rgb(hh, ss, ll, out);
}

float pc_compose_black_white_gray(const float c[3], const float weights[6])
{
    /* Each colour is a grey part (its smallest channel), a secondary-colour part (middle -
     * smallest) and a primary part (largest - middle); the sliders weight the colour parts. */
    float r = c[0];
    float g = c[1];
    float b = c[2];
    float mx = pc_rs_maxf(pc_rs_maxf(r, g), b);
    float mn = pc_rs_minf(pc_rs_minf(r, g), b);
    float mid = r + g + b - mx - mn;
    size_t primary = (r >= g && r >= b) ? 0 : (g >= b ? 2 : 4);
    size_t secondary = (b <= r && b <= g) ? 1 : (r <= g ? 3 : 5);
    return pc_clampf(mn + (mid - mn) * weights[secondary] / 100.0f + (mx - mid) * weights[primary] / 100.0f,
                     0.0f, 1.0f);
}

float pc_compose_bayer4(int32_t x, int32_t y)
{
    static const float m[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (m[(y & 3) * 4 + (x & 3)] + 0.5f) / 16.0f - 0.5f;
}

/* `w.iter().map(|(a, v)| a * v[k]).sum()` over four weighted corners (Rust's sum starts at -0.0). */
static void mix4(const float a[4], const float *v[4], float out[3])
{
    int k;
    int i;
    for (k = 0; k < 3; k++) {
        float s = -0.0f;
        for (i = 0; i < 4; i++) {
            s += a[i] * v[i][k];
        }
        out[k] = s;
    }
}

void pc_compose_lut3d_sample(const float *lut, size_t lut_len, size_t n, const float c[3], bool tetrahedral,
                             float out[3])
{
    size_t need;
    size_t i0[3];
    float f[3];
    float m;
    int k;
#define AT(dr, dg, db) (lut + (((i0[2] + (db)) * n + i0[1] + (dg)) * n + i0[0] + (dr)) * 3)
    if (n < 2 || !pc_lut3d_len((uint32_t)(n <= UINT32_MAX ? n : 0), &need) || lut_len < need) {
        memcpy(out, c, 3 * sizeof(float));
        return;
    }
    m = (float)(n - 1);
    for (k = 0; k < 3; k++) {
        float pos = pc_clampf(c[k], 0.0f, 1.0f) * m;
        size_t i = pc_f32_to_size(floorf(pos));
        i0[k] = i < n - 2 ? i : n - 2;
        f[k] = pos - (float)i0[k];
    }
    if (tetrahedral) {
        float fr = f[0];
        float fg = f[1];
        float fb = f[2];
        const float *c000 = AT(0, 0, 0);
        const float *c111 = AT(1, 1, 1);
        float w[4];
        const float *v[4];
        if (fr > fg) {
            if (fg > fb) {
                w[0] = 1.0f - fr, w[1] = fr - fg, w[2] = fg - fb, w[3] = fb;
                v[0] = c000, v[1] = AT(1, 0, 0), v[2] = AT(1, 1, 0), v[3] = c111;
            } else if (fr > fb) {
                w[0] = 1.0f - fr, w[1] = fr - fb, w[2] = fb - fg, w[3] = fg;
                v[0] = c000, v[1] = AT(1, 0, 0), v[2] = AT(1, 0, 1), v[3] = c111;
            } else {
                w[0] = 1.0f - fb, w[1] = fb - fr, w[2] = fr - fg, w[3] = fg;
                v[0] = c000, v[1] = AT(0, 0, 1), v[2] = AT(1, 0, 1), v[3] = c111;
            }
        } else if (fb > fg) {
            w[0] = 1.0f - fb, w[1] = fb - fg, w[2] = fg - fr, w[3] = fr;
            v[0] = c000, v[1] = AT(0, 0, 1), v[2] = AT(0, 1, 1), v[3] = c111;
        } else if (fb > fr) {
            w[0] = 1.0f - fg, w[1] = fg - fb, w[2] = fb - fr, w[3] = fr;
            v[0] = c000, v[1] = AT(0, 1, 0), v[2] = AT(0, 1, 1), v[3] = c111;
        } else {
            w[0] = 1.0f - fg, w[1] = fg - fr, w[2] = fr - fb, w[3] = fb;
            v[0] = c000, v[1] = AT(0, 1, 0), v[2] = AT(1, 1, 0), v[3] = c111;
        }
        mix4(w, v, out);
    } else {
        const float *p[8] = {AT(0, 0, 0), AT(1, 0, 0), AT(0, 1, 0), AT(1, 1, 0),
                             AT(0, 0, 1), AT(1, 0, 1), AT(0, 1, 1), AT(1, 1, 1)};
        for (k = 0; k < 3; k++) {
            float c00 = p[0][k] + (p[1][k] - p[0][k]) * f[0];
            float c10 = p[2][k] + (p[3][k] - p[2][k]) * f[0];
            float c01 = p[4][k] + (p[5][k] - p[4][k]) * f[0];
            float c11 = p[6][k] + (p[7][k] - p[6][k]) * f[0];
            float a = c00 + (c10 - c00) * f[1];
            float b = c01 + (c11 - c01) * f[1];
            out[k] = a + (b - a) * f[2];
        }
    }
#undef AT
}

void pc_compose_selective_color_weights(const float c[3], float out[9])
{
    float mx = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]);
    float mn = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
    float mid = c[0] + c[1] + c[2] - mx - mn;
#define TOP(i) (c[i] >= mx ? mx - mid : 0.0f)
#define BOTTOM(i) (c[i] <= mn ? mid - mn : 0.0f)
    out[0] = TOP(0);
    out[1] = BOTTOM(2);
    out[2] = TOP(1);
    out[3] = BOTTOM(0);
    out[4] = TOP(2);
    out[5] = BOTTOM(1);
#undef TOP
#undef BOTTOM
    out[6] = pc_rs_maxf_k((mn - 0.5f) * 2.0f, 0.0f);
    out[7] = pc_clampf(1.0f - fabsf(mx - 0.5f) - fabsf(mn - 0.5f), 0.0f, 1.0f);
    out[8] = pc_rs_maxf_k((0.5f - mx) * 2.0f, 0.0f);
}

void pc_compose_selective_color(const float c[3], bool relative, const float adj[9][4], float out[3])
{
    /* Each range shifts the cyan, magenta and yellow ink (1 - R, G, B) by its percentage, plus
     * its black percentage on every ink, weighted by how much the colour belongs to the range.
     * Relative mode scales the shift by the ink already present. */
    float w[9];
    float delta[3] = {0.0f, 0.0f, 0.0f};
    size_t r;
    int i;
    pc_compose_selective_color_weights(c, w);
    for (r = 0; r < 9; r++) {
        float a[4];
        if (w[r] <= 0.0f) {
            continue;
        }
        for (i = 0; i < 4; i++) {
            a[i] = adj[r][i] / 100.0f;
        }
        for (i = 0; i < 3; i++) {
            /* Absolute mode scales the shift by the room the ink has: adding ink by
             * min(1, 2 (1 - ink)), removing it by min(1, 2 ink) (Photoshop 27.11). */
            float ink = 1.0f - c[i];
            float d = a[i] + a[3];
            if (relative) {
                d = d * ink;
            } else if (d > 0.0f) {
                d = d * pc_rs_minf_k(2.0f * (1.0f - ink), 1.0f);
            } else {
                d = d * pc_rs_minf_k(2.0f * ink, 1.0f);
            }
            delta[i] += d * w[r];
        }
    }
    for (i = 0; i < 3; i++) {
        out[i] = pc_clampf(c[i] - delta[i], 0.0f, 1.0f);
    }
}

void pc_compose_photo_filter_matrix(const float color[3], float density, bool normalize_y, float out[3][3])
{
    /* Photoshop multiplies the pixel's D50 XYZ, relative to white, by the filter colour's (mixed
     * in by density): a fixed linear map, folded into one matrix. normalize_y keeps luminance Y
     * (32-bit Preserve Luminosity). Fitted on the photoshop corpus photo-filter.psd. */
    float d = isfinite(density) ? pc_clampf(density, 0.0f, 1.0f) : 0.0f;
    float lin[3];
    float xyz[3];
    float s[3];
    int i;
    int r;
    int c;
    int k;
    for (i = 0; i < 3; i++) {
        lin[i] = pc_srgb_to_linear(pc_clampf(color[i], 0.0f, 1.0f));
    }
    pc_color_mat3_mul(PC_SRGB_TO_XYZ_D50, lin, xyz);
    for (i = 0; i < 3; i++) {
        s[i] = 1.0f - d + d * xyz[i] / PC_COLOR_D50[i];
    }
    if (normalize_y && s[1] > 1e-6f) {
        float y = s[1];
        for (i = 0; i < 3; i++) {
            s[i] = s[i] / y;
        }
    }
    /* XYZ_D50_TO_SRGB . diag(s) . SRGB_TO_XYZ_D50 */
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            float sum = -0.0f;
            for (k = 0; k < 3; k++) {
                sum += PC_XYZ_D50_TO_SRGB[r][k] * s[k] * PC_SRGB_TO_XYZ_D50[k][c];
            }
            out[r][c] = sum;
        }
    }
}

/* How much positive Vibrance treats a hue (degrees) as skin. */
static float vibrance_skin(float h)
{
    if (h <= 30.0f) {
        return 1.0f;
    }
    if (h < 45.0f) {
        return (45.0f - h) / 15.0f;
    }
    if (h < 300.0f) {
        return 0.0f;
    }
    return (h - 300.0f) / 60.0f;
}

static float hsv_hue(const float c[3], float mx, float mn)
{
    float d = mx - mn;
    float h;
    if (mx == c[0]) {
        h = rem_euclid((c[1] - c[2]) / d, 6.0f);
    } else if (mx == c[1]) {
        h = (c[2] - c[0]) / d + 2.0f;
    } else {
        h = (c[0] - c[1]) / d + 4.0f;
    }
    return h * 60.0f;
}

void pc_compose_vibrance_lin(const float c[3], float v, float out[3])
{
    /* With t = 1 - min/max (HSV saturation) and V = max (measured against Photoshop 25.4; see
     * the reference for the derivation): negative Vibrance desaturates and darkens by a shaped
     * amount, positive boosts saturation, damped on reds and skin. */
    float mx = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]);
    float mn = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
    float t;
    float shape;
    float t2;
    float v2;
    float m2;
    float k;
    int i;
    if (mx <= 0.0f || mx - mn <= 1e-9f) {
        memcpy(out, c, 3 * sizeof(float));
        return;
    }
    t = 1.0f - mn / mx;
    shape = t * (1.0f - t) * (2.0f - t + t * t) * mx * pc_rs_maxf_k(1.0f - mx, 0.0f);
    if (v < 0.0f) {
        float a = -v;
        t2 = t * (1.0f - a / 4.0f) * (1.0f - a + a * t * (1.0f + t) / 2.0f);
        v2 = mx - a * shape;
    } else {
        const float *b = PC_COMPOSE_VIBRANCE_BOOST;
        float w = vibrance_skin(hsv_hue(c, mx, mn));
        float a = v * (1.0f - w) + 0.64f * powf(v, 1.6f) * w;
        float boost = (b[0] * a + b[1] * a * a) * powf(t, b[2]) * powf(1.0f - t, b[3]) *
                      expf((b[4] + b[5] * a + b[6] * (1.0f - t)) * pc_rs_maxf_k(1.0f - mx, 0.0f));
        t2 = pc_rs_minf_k(t * (1.0f + boost), 1.0f);
        v2 = mx + v / 4.0f * shape;
    }
    m2 = v2 * (1.0f - t2);
    k = (v2 - m2) / (mx - mn);
    for (i = 0; i < 3; i++) {
        out[i] = m2 + (c[i] - mn) * k;
    }
}

/* Transfer::decode / encode as rustc compiles them inlined into vibrance_px: for Gamma(1) the
 * vectorised v.max(0.0) gives +0.0 for a -0.0 red or blue (second operand wins) but keeps green's
 * -0.0 (first wins). Only the sign of zero differs, but hash sweeps see it (measured; §7). */
static float vib_decode(pc_compose_transfer t, float v, int ch)
{
    if (ch == 1 && t.kind == PC_COMPOSE_TRANSFER_GAMMA && t.gamma == 1.0f) {
        return pc_rs_maxf(v, 0.0f);
    }
    return pc_compose_transfer_decode(t, v);
}

static float vib_encode(pc_compose_transfer t, float v, int ch)
{
    if (ch == 1 && t.kind == PC_COMPOSE_TRANSFER_GAMMA && t.gamma == 1.0f) {
        return pc_rs_maxf(v, 0.0f);
    }
    return pc_compose_transfer_encode(t, v);
}

void pc_compose_vibrance_px(const float c[3], float v, float s, pc_compose_transfer transfer, float hi,
                            float out[3])
{
    /* Both sliders work in linear light through the document's tone curve, Vibrance first.
     * Saturation moves each colour away from (or towards) the grey 0.288 R + 0.712 G. */
    float l[3];
    int i;
    for (i = 0; i < 3; i++) {
        l[i] = vib_decode(transfer, c[i], i);
    }
    if (v != 0.0f) {
        float t[3];
        pc_compose_vibrance_lin(l, v, t);
        memcpy(l, t, sizeof l);
    }
    if (s != 0.0f) {
        float g = PC_COMPOSE_VIBRANCE_GREY[0] * l[0] + PC_COMPOSE_VIBRANCE_GREY[1] * l[1];
        for (i = 0; i < 3; i++) {
            l[i] = pc_clampf(g + (1.0f + s) * (l[i] - g), 0.0f, hi);
        }
    }
    for (i = 0; i < 3; i++) {
        out[i] = vib_encode(transfer, pc_clampf(l[i], 0.0f, hi), i);
    }
}

float pc_compose_modern_brightness(float v, float brightness)
{
    /* A line of slope 1.375^(b/50) from the origin, rolled off to (1, 1) by a v^P white-anchor
     * term; pins black and white. Fit to Photoshop ground truth. */
    float s;
    float p;
    float x;
    if (brightness == 0.0f) {
        return v;
    }
    s = powf(1.375f, brightness / 50.0f);
    p = brightness >= 0.0f ? pc_rs_maxf_k(4.5f - 0.013f * brightness, 2.0f) : 5.0f - 0.072f * brightness;
    x = pc_clampf(v, 0.0f, 1.0f);
    return pc_clampf(s * x + (1.0f - s) * powf(x, p), 0.0f, 1.0f);
}

/* Cubic Hermite on [0, 0.5]: (0, 0) with slope end, (0.5, 0.5) with slope mid; t in [0, 1]. */
static float contrast_half(float end, float mid, float t)
{
    return end * 0.5f * (t * t * t - 2.0f * t * t + t) + (-2.0f * t * t * t + 3.0f * t * t) * 0.5f +
           mid * 0.5f * (t * t * t - t * t);
}

float pc_compose_modern_contrast(float v, float contrast)
{
    /* A symmetric S-curve pivoting at 0.5: endpoint slope 1 - c/128, centre slope 1 + c/128. */
    float k;
    float x;
    if (contrast == 0.0f) {
        return v;
    }
    k = contrast / 128.0f;
    x = pc_clampf(v, 0.0f, 1.0f);
    if (x <= 0.5f) {
        return contrast_half(1.0f - k, 1.0f + k, x / 0.5f);
    }
    return 1.0f - contrast_half(1.0f - k, 1.0f + k, (1.0f - x) / 0.5f);
}

float pc_compose_posterize(float v, uint32_t levels)
{
    /* n equal input bins over 0..=255, output levels floor(k * 255 / (n - 1)). */
    float n = (float)(levels < 2 ? 2 : (levels > 255 ? 255 : levels));
    float x = roundf(pc_clampf(v, 0.0f, 1.0f) * 255.0f);
    float bin = pc_rs_minf(floorf(x * n / 256.0f), n - 1.0f);
    return floorf(bin * 255.0f / (n - 1.0f)) / 255.0f;
}

/* ---- applying ---- */

typedef void (*rgb_fn)(void *ctx, const float in[3], float out[3]);

static void map_rgb(pc_compose_buffer *buf, rgb_fn f, void *ctx)
{
    size_t i;
    for (i = 0; i < buf->len; i++) {
        float *p = buf->px[i];
        float r[3];
        if (p[3] <= 0.0f) {
            continue;
        }
        f(ctx, p, r);
        p[0] = r[0];
        p[1] = r[1];
        p[2] = r[2];
    }
}

static void f_invert(void *ctx, const float c[3], float out[3])
{
    (void)ctx;
    out[0] = 1.0f - c[0];
    out[1] = 1.0f - c[1];
    out[2] = 1.0f - c[2];
}

static void f_threshold(void *ctx, const float c[3], float out[3])
{
    /* Compared on the 8-bit grid like Photoshop (avoids float ties). */
    float t = *(const float *)ctx;
    float v = roundf(pc_rgb_to_gray(c) * 255.0f) >= t ? 1.0f : 0.0f;
    out[0] = out[1] = out[2] = v;
}

static void f_posterize(void *ctx, const float c[3], float out[3])
{
    uint32_t n = *(const uint32_t *)ctx;
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = pc_compose_posterize(c[i], n);
    }
}

typedef struct bc_ctx {
    float b;
    float c; /* modern */
    pc_cmp_legacy_bc legacy;
} bc_ctx;

pc_cmp_legacy_bc pc_cmp_legacy_bc_new(float brightness, float contrast)
{
    /* Contrast scales around 128/255, brightness added before it when contrast is raised and after
     * it when lowered (Photoshop 27.11: +40 / +30 maps 128 to 185; psd-tools -23 / -26 needs
     * contrast first). */
    pc_cmp_legacy_bc l;
    float b = brightness / 255.0f;
    float c = pc_clampf(contrast, -100.0f, 99.0f);
    l.k = c >= 0.0f ? 1.0f / (1.0f - c / 100.0f) : 1.0f + c / 100.0f;
    l.pre = c >= 0.0f ? b : 0.0f;
    l.post = c >= 0.0f ? 0.0f : b;
    return l;
}

float pc_cmp_legacy_bc_apply(const pc_cmp_legacy_bc *l, float v)
{
    return pc_clampf((v + l->pre - PC_COMPOSE_LEGACY_PIVOT) * l->k + PC_COMPOSE_LEGACY_PIVOT + l->post, 0.0f,
                     1.0f);
}

static void f_legacy_bc(void *ctx, const float c[3], float out[3])
{
    const bc_ctx *x = ctx;
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = pc_cmp_legacy_bc_apply(&x->legacy, c[i]);
    }
}

static void f_modern_bc(void *ctx, const float c[3], float out[3])
{
    const bc_ctx *x = ctx;
    int i;
    for (i = 0; i < 3; i++) {
        out[i] =
            pc_clampf(pc_compose_modern_contrast(pc_compose_modern_brightness(c[i], x->b), x->c), 0.0f, 1.0f);
    }
}

typedef struct exposure_ctx {
    float m;
    float offset;
    float g;
    pc_compose_transfer t;
    bool hdr;
} exposure_ctx;

/* f32::signum: NaN stays NaN, the sign of zero counts. */
static float signum(float x)
{
    return isnan(x) ? x : copysignf(1.0f, x);
}

static void f_exposure(void *ctx, const float c[3], float out[3])
{
    /* In linear light: (lin 2^exposure + offset)^(1/gamma), then back through the tone curve. */
    const exposure_ctx *x = ctx;
    int i;
    for (i = 0; i < 3; i++) {
        float lin;
        if (x->hdr) {
            /* Float documents store linear samples; Photoshop's 32-bit Exposure also keeps
             * negative results, with a signed gamma. */
            lin = c[i] * x->m + x->offset;
            out[i] = x->g == 1.0f ? lin : signum(lin) * powf(fabsf(lin), 1.0f / x->g);
            continue;
        }
        lin = pc_rs_maxf_k(pc_compose_transfer_decode(x->t, c[i]) * x->m + x->offset, 0.0f);
        if (x->g != 1.0f) {
            lin = powf(lin, 1.0f / x->g);
        }
        out[i] = pc_clampf(pc_compose_transfer_encode(x->t, lin), 0.0f, 1.0f);
    }
}

typedef struct levels_f32_ctx {
    const pc_levels_channel *master;
    const pc_levels_channel *per_channel;
    bool master_identity;
    bool channel_identity[3];
} levels_f32_ctx;

static void f_levels_f32(void *ctx, const float c[3], float out[3])
{
    const levels_f32_ctx *x = ctx;
    int i;
    for (i = 0; i < 3; i++) {
        float v = x->channel_identity[i] ? c[i] : pc_compose_levels_float(&x->per_channel[i], c[i]);
        out[i] = x->master_identity ? v : pc_compose_levels_float(x->master, v);
    }
}

static bool levels_is_default(const pc_levels_channel *ch)
{
    return ch->in_black == 0.0f && ch->in_white == 1.0f && ch->gamma == 1.0f && ch->out_black == 0.0f &&
           ch->out_white == 1.0f;
}

typedef struct tone_ctx {
    const float *luts;
    pc_tone_space space;
    bool identity[3];
} tone_ctx;

static void f_tone(void *ctx, const float c[3], float out[3])
{
    const tone_ctx *x = ctx;
    if (x->space == PC_TONE_RGB) {
        int i;
        for (i = 0; i < 3; i++) {
            out[i] = x->identity[i] ? c[i] : pc_cmp_lut(x->luts + (size_t)i * LUT, LUT, c[i]);
        }
        return;
    }
    pc_cmp_tone_in_space(x->space, x->luts, c, out);
}

typedef struct hs_ctx {
    const float *table; /* NULL: no range edits */
    float hue;
    float saturation;
    float lightness;
    bool colorize;
} hs_ctx;

static void f_hue_sat(void *ctx, const float c[3], float out[3])
{
    const hs_ctx *x = ctx;
    float dh = 0.0f;
    float ds = 0.0f;
    float dl = 0.0f;
    if (x->table != NULL) {
        /* Range edits follow the pixel's original hue, in full however dull the colour (Photoshop's
         * merged images, #4089). Greys stay grey: no hue to shift, no saturation to scale, and range
         * Lightness moves towards a channel they share. */
        float h0;
        float s0;
        float l0;
        pc_compose_rgb_to_hsl(c, &h0, &s0, &l0);
        dh = pc_cmp_lut(x->table, LUT, h0);
        ds = pc_cmp_lut(x->table + LUT, LUT, h0);
        dl = pc_cmp_lut(x->table + 2 * LUT, LUT, h0);
    }
    {
        float o[3];
        pc_compose_hue_saturation(c, x->hue + dh, pc_clampf(x->saturation / 100.0f + ds, -1.0f, 1.0f),
                                  pc_clampf(x->lightness / 100.0f, -1.0f, 1.0f), x->colorize, o);
        pc_compose_range_lightness(o, dl, out);
    }
}

typedef struct vib_ctx {
    float v;
    float s;
    pc_compose_transfer t;
    float hi;
} vib_ctx;

static void f_vibrance(void *ctx, const float c[3], float out[3])
{
    const vib_ctx *x = ctx;
    pc_compose_vibrance_px(c, x->v, x->s, x->t, x->hi, out);
}

typedef struct mixer_ctx {
    const float (*m)[4];
    bool mono;
} mixer_ctx;

static float mixer_row(const float row[4], const float c[3])
{
    return pc_clampf(row[0] * c[0] + row[1] * c[1] + row[2] * c[2] + row[3], 0.0f, 1.0f);
}

static void f_mixer(void *ctx, const float c[3], float out[3])
{
    const mixer_ctx *x = ctx;
    if (x->mono) {
        out[0] = out[1] = out[2] = mixer_row(x->m[0], c);
        return;
    }
    out[0] = mixer_row(x->m[0], c);
    out[1] = mixer_row(x->m[1], c);
    out[2] = mixer_row(x->m[2], c);
}

typedef struct pf_ctx {
    float m[3][3];
    bool set_lum;
    pc_compose_transfer t;
} pf_ctx;

static void f_photo_filter(void *ctx, const float c[3], float out[3])
{
    const pf_ctx *x = ctx;
    float d[3];
    float f[3];
    int i;
    for (i = 0; i < 3; i++) {
        d[i] = pc_compose_transfer_decode(x->t, c[i]);
    }
    pc_color_mat3_mul(x->m, d, f);
    for (i = 0; i < 3; i++) {
        f[i] = pc_compose_transfer_encode(x->t, f[i]);
    }
    if (x->set_lum) {
        float l[3];
        pc_blend_set_lum(f, pc_blend_lum(c), l);
        memcpy(f, l, sizeof f);
    }
    for (i = 0; i < 3; i++) {
        out[i] = pc_clampf(f[i], 0.0f, 1.0f);
    }
}

typedef struct bw_ctx {
    const float *weights;
    const float *tint; /* NULL: none */
} bw_ctx;

static void f_black_white(void *ctx, const float c[3], float out[3])
{
    const bw_ctx *x = ctx;
    float g = pc_compose_black_white_gray(c, x->weights);
    int i;
    if (x->tint != NULL) {
        /* Photoshop tints like the Color blend mode: the tint's hue and saturation at the grey's
         * luminosity (SetLum + ClipColor, Photoshop 27.11). */
        float t[3];
        pc_blend_set_lum(x->tint, g, t);
        for (i = 0; i < 3; i++) {
            out[i] = pc_clampf(t[i], 0.0f, 1.0f);
        }
        return;
    }
    out[0] = out[1] = out[2] = g;
}

static void f_color_balance(void *ctx, const float c[3], float out[3])
{
    const pc_adjustment *a = ctx;
    float tones[3][3];
    int i;
    for (i = 0; i < 3; i++) {
        tones[0][i] = a->u.color_balance.shadows[i];
        tones[1][i] = a->u.color_balance.midtones[i];
        tones[2][i] = a->u.color_balance.highlights[i];
    }
    for (i = 0; i < 3; i++) {
        out[i] = pc_compose_color_balance_channel((const float (*)[3])tones,
                                                  a->u.color_balance.preserve_luminosity, i, c[i]);
    }
}

static void f_selective(void *ctx, const float c[3], float out[3])
{
    const pc_adjustment *a = ctx;
    pc_compose_selective_color(c, a->u.selective_color.relative, a->u.selective_color.adjustments, out);
}

void pc_cmp_gradient_map_color(const pc_gradient_map_stop *s, size_t n, float t, float out[3])
{
    size_t i;
    int c;
    if (n == 0) {
        out[0] = out[1] = out[2] = t;
        return;
    }
    if (n == 1 || t <= s[0].pos) {
        memcpy(out, s[0].rgb, 3 * sizeof(float));
        return;
    }
    for (i = 0; i + 1 < n; i++) {
        if (t <= s[i + 1].pos) {
            float k = s[i + 1].pos > s[i].pos ? (t - s[i].pos) / (s[i + 1].pos - s[i].pos) : 0.0f;
            for (c = 0; c < 3; c++) {
                out[c] = s[i].rgb[c] + (s[i + 1].rgb[c] - s[i].rgb[c]) * k;
            }
            return;
        }
    }
    memcpy(out, s[n - 1].rgb, 3 * sizeof(float));
}

/* Gradient Map and Color Lookup: per pixel with document coordinates for the dither. */
static void map_dithered(pc_compose_buffer *buf, const pc_adjustment *adj, const float *lut, size_t lut_len)
{
    uint32_t bw = pc_rect_width(buf->rect);
    size_t w = bw > 1 ? bw : 1;
    bool dither = adj->kind == PC_ADJ_GRADIENT_MAP ? adj->u.gradient_map.dither : adj->u.color_lookup.dither;
    size_t i;
    int c;
    for (i = 0; i < buf->len; i++) {
        float *p = buf->px[i];
        float o[3];
        if (p[3] <= 0.0f) {
            continue;
        }
        if (adj->kind == PC_ADJ_GRADIENT_MAP) {
            float t = pc_rgb_to_gray(p);
            if (adj->u.gradient_map.reverse) {
                t = 1.0f - t;
            }
            pc_cmp_gradient_map_color(adj->u.gradient_map.stops, adj->u.gradient_map.nstops, t, o);
        } else {
            pc_compose_lut3d_sample(lut, lut_len, adj->u.color_lookup.size, p,
                                    adj->u.color_lookup.tetrahedral, o);
        }
        if (dither) {
            float d = pc_compose_bayer4(pc_i32_wrapping_add(buf->rect.x0, (int32_t)(uint32_t)(i % w)),
                                        pc_i32_wrapping_add(buf->rect.y0, (int32_t)(uint32_t)(i / w))) /
                      255.0f;
            for (c = 0; c < 3; c++) {
                o[c] = pc_clampf(o[c] + d, 0.0f, 1.0f);
            }
        }
        memcpy(p, o, sizeof o);
    }
}

pc_status pc_compose_adjust_apply_depth(const pc_adjustment *adj, pc_compose_buffer *buf,
                                        pc_compose_transfer transfer, bool has_depth, pc_sample_type depth,
                                        pc_error *err)
{
    bool f32 = has_depth && depth == PC_SAMPLE_TYPE_F32;
    switch (adj->kind) {
    case PC_ADJ_INVERT:
        map_rgb(buf, f_invert, NULL);
        return PC_OK;
    case PC_ADJ_THRESHOLD: {
        float t = roundf(adj->u.threshold.level * 255.0f);
        map_rgb(buf, f_threshold, &t);
        return PC_OK;
    }
    case PC_ADJ_POSTERIZE: {
        uint32_t n = adj->u.posterize.levels;
        map_rgb(buf, f_posterize, &n);
        return PC_OK;
    }
    case PC_ADJ_BRIGHTNESS_CONTRAST: {
        bc_ctx x;
        x.b = adj->u.brightness_contrast.brightness;
        x.c = adj->u.brightness_contrast.contrast;
        x.legacy = pc_cmp_legacy_bc_new(x.b, x.c);
        if (adj->u.brightness_contrast.legacy) {
            map_rgb(buf, f_legacy_bc, &x);
        } else {
            /* Modern (CS3+): smooth curves pinning black and white, brightness then contrast. */
            map_rgb(buf, f_modern_bc, &x);
        }
        return PC_OK;
    }
    case PC_ADJ_EXPOSURE: {
        exposure_ctx x;
        /* 2^x: LLVM lowers the reference's 2f32.powf(x) to exp2f. */
        x.m = exp2f(adj->u.exposure.exposure);
        x.offset = adj->u.exposure.offset;
        x.g = pc_rs_maxf_k(adj->u.exposure.gamma, 0.01f);
        x.t = pc_compose_transfer_for_exposure(transfer);
        x.hdr = f32;
        map_rgb(buf, f_exposure, &x);
        return PC_OK;
    }
    case PC_ADJ_LEVELS:
    case PC_ADJ_CURVES: {
        tone_ctx x;
        if (adj->kind == PC_ADJ_LEVELS && adj->u.levels.space == PC_TONE_RGB && f32) {
            /* A 0..1 lookup table cannot represent signed, unbounded float samples. */
            levels_f32_ctx lx;
            int c;
            bool any = false;
            lx.master = &adj->u.levels.master;
            lx.per_channel = adj->u.levels.per_channel;
            lx.master_identity = levels_is_default(lx.master);
            for (c = 0; c < 3; c++) {
                lx.channel_identity[c] = levels_is_default(&lx.per_channel[c]);
                any = any || !lx.channel_identity[c];
            }
            if (!lx.master_identity || any) {
                map_rgb(buf, f_levels_f32, &lx);
            }
            return PC_OK;
        }
        float *luts = pc_malloc_array(4 * LUT, sizeof *luts);
        pc_status st;
        if (luts == NULL) {
            return nomem(err);
        }
        st = pc_compose_tone_luts_depth(adj, has_depth, depth, luts, err);
        if (st == PC_OK) {
            x.luts = luts;
            x.space = adj->kind == PC_ADJ_LEVELS ? adj->u.levels.space : adj->u.curves.space;
            pc_compose_float_tone_identity(adj, has_depth, depth, x.identity);
            map_rgb(buf, f_tone, &x);
        }
        free(luts);
        return st;
    }
    case PC_ADJ_HUE_SATURATION: {
        hs_ctx x;
        float *table = NULL;
        size_t i;
        bool ranges = false;
        for (i = 0; i < 6; i++) {
            ranges = ranges || !pc_hue_range_is_neutral(&adj->u.hue_saturation.ranges[i]);
        }
        if (!adj->u.hue_saturation.colorize && ranges) {
            table = pc_malloc_array(3 * LUT, sizeof *table);
            if (table == NULL) {
                return nomem(err);
            }
            pc_compose_hue_range_tables(adj->u.hue_saturation.ranges, table);
        }
        x.table = table;
        x.hue = adj->u.hue_saturation.hue;
        x.saturation = adj->u.hue_saturation.saturation;
        x.lightness = adj->u.hue_saturation.lightness;
        x.colorize = adj->u.hue_saturation.colorize;
        map_rgb(buf, f_hue_sat, &x);
        free(table);
        return PC_OK;
    }
    case PC_ADJ_VIBRANCE: {
        vib_ctx x;
        x.v = adj->u.vibrance.vibrance / 100.0f;
        x.s = adj->u.vibrance.saturation / 100.0f;
        x.t = transfer;
        x.hi = f32 ? (float)INFINITY : 1.0f;
        map_rgb(buf, f_vibrance, &x);
        return PC_OK;
    }
    case PC_ADJ_CHANNEL_MIXER: {
        mixer_ctx x;
        x.m = (const float (*)[4])adj->u.channel_mixer.matrix;
        x.mono = adj->u.channel_mixer.monochrome;
        map_rgb(buf, f_mixer, &x);
        return PC_OK;
    }
    case PC_ADJ_PHOTO_FILTER: {
        pf_ctx x;
        bool pl = adj->u.photo_filter.preserve_luminosity;
        pc_compose_photo_filter_matrix(adj->u.photo_filter.color, adj->u.photo_filter.density, pl && f32,
                                       x.m);
        x.set_lum = pl && !f32;
        x.t = transfer;
        map_rgb(buf, f_photo_filter, &x);
        return PC_OK;
    }
    case PC_ADJ_BLACK_WHITE: {
        bw_ctx x;
        x.weights = adj->u.black_white.weights;
        x.tint = adj->u.black_white.has_tint ? adj->u.black_white.tint : NULL;
        map_rgb(buf, f_black_white, &x);
        return PC_OK;
    }
    case PC_ADJ_GRADIENT_MAP:
        map_dithered(buf, adj, NULL, 0);
        return PC_OK;
    case PC_ADJ_COLOR_BALANCE:
        map_rgb(buf, f_color_balance, (void *)(uintptr_t)adj);
        return PC_OK;
    case PC_ADJ_SELECTIVE_COLOR:
        map_rgb(buf, f_selective, (void *)(uintptr_t)adj);
        return PC_OK;
    case PC_ADJ_COLOR_LOOKUP: {
        size_t n = 0;
        size_t need;
        const float *lut = pc_adjustment_lut(adj, &n);
        /* A stored size is untrusted: only a table that covers size^3 entries is used. */
        if (lut != NULL && pc_lut3d_len(adj->u.color_lookup.size, &need) && n >= need) {
            map_dithered(buf, adj, lut, n);
        }
        return PC_OK;
    }
    default:
        /* Not evaluated: identity (still round-trips through PSD). */
        return PC_OK;
    }
}

pc_status pc_compose_adjust_apply(const pc_adjustment *adj, pc_compose_buffer *buf, pc_error *err)
{
    return pc_compose_adjust_apply_depth(adj, buf, (pc_compose_transfer){PC_COMPOSE_TRANSFER_SRGB, 0.0f},
                                         false, PC_SAMPLE_TYPE_F32, err);
}

pc_status pc_compose_adjust_apply_with(const pc_adjustment *adj, pc_compose_buffer *buf,
                                       pc_compose_transfer transfer, pc_error *err)
{
    return pc_compose_adjust_apply_depth(adj, buf, transfer, false, PC_SAMPLE_TYPE_F32, err);
}
