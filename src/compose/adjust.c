/* Evaluation of adjustment layers on a composite buffer (port of crates/compose/src/adjust.rs).
 *
 * Formulas are documented approximations of Photoshop behaviour, matched against
 * Photoshop-rendered composites. Expression order follows the reference exactly (float results
 * are compared bit for bit with it). Tone curves are in adjust_tone.c. */
#include <float.h>
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

/* HSL-to-RGB with the low (`p`) and high (`q`) explicit: the 32-bit Hue/Saturation path builds a
 * colour from a chroma around its lightness directly, without the `2 - max - min` divisor that is
 * zero at lightness 1 and negative above it. */
static void hue_rgb(float h, float p, float q, float out[3])
{
    out[0] = hsl_channel(p, q, h + 1.0f / 3.0f);
    out[1] = hsl_channel(p, q, h);
    out[2] = hsl_channel(p, q, h - 1.0f / 3.0f);
}

/* The factor 32-bit Hue/Saturation scales chroma by for saturation `s` (-1..1), as Photoshop
 * computes it (b526378de): lowering cuts `s` to 1/256 steps, raising cuts it to 254 steps out of
 * 255, takes 255 / (255 - steps) to 1/256 and stops at 32767/256, so +25 % gives 340/256 and
 * +100 % 127.996 (finite at L = 1). */
float pc_compose_hdr_saturation_factor(float s)
{
    uint32_t steps;
    uint32_t denom;
    uint32_t v;
    /* The 1e-4 keeps a master-plus-range sum on its step: -0.46 + 0.96 is 0.49999997 in f32. */
    s = pc_clampf(s, -1.0f, 1.0f);
    if (s <= 0.0f) {
        return 1.0f - floorf(-s * 256.0f + 1e-4f) / 256.0f;
    }
    steps = (uint32_t)pc_f32_to_i32(floorf(s * 254.0f + 1e-4f));
    denom = 255u - (steps < 255u ? steps : 255u);
    if (denom < 1u) {
        denom = 1u;
    }
    v = 65280u / denom;
    if (v > 32767u) {
        v = 32767u;
    }
    return (float)v / 256.0f;
}

/* Hue/Saturation's saturation `s` (-1..1) as taken in a document of the given depth: a 32-bit
 * Colorize builds its colour with the saturation times `1 - |lightness|`; 8 and 16 bits keep the
 * full saturation. */
float pc_compose_colorize_saturation(float s, float lightness, bool colorize, bool hdr)
{
    if (colorize && hdr) {
        return pc_clampf(s, -1.0f, 1.0f) * (1.0f - pc_rs_minf_k(fabsf(lightness), 1.0f));
    }
    return s;
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
    pc_compose_hue_saturation_in(c, hue, s, l, colorize, false, out);
}

/* [`pc_compose_hue_saturation`] in a document of the given depth. At 32 bits (`hdr`) Photoshop
 * doesn't clip the raised saturation, so saturated colours leave 0..1, and it scales each colour's
 * chroma around its lightness by [`pc_compose_hdr_saturation_factor`], so colours at or above
 * lightness 1 keep their hue too. */
void pc_compose_hue_saturation_in(const float c[3], float hue, float s, float l, bool colorize, bool hdr,
                                  float out[3])
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
    if (hdr) {
        /* The chroma, not the HSL saturation, whose `2 - max - min` divisor is 0 at lightness 1 and
         * negative above it: (2, 0, 0) with Hue +40 becomes (2, 1.333, 0) in Photoshop. */
        float mx = pc_rs_maxf(pc_rs_maxf(lc[0], lc[1]), lc[2]);
        float mn = pc_rs_minf(pc_rs_minf(lc[0], lc[1]), lc[2]);
        float d = (mx - mn) * pc_compose_hdr_saturation_factor(s);
        hue_rgb(hh, ll - d / 2.0f, ll + d / 2.0f, out);
        return;
    }
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

/* Modern Brightness curve constants for one slider value. Raised, it is a line of slope s from
 * the origin up to the knee at output 126/255, then a monotone roll-off to (1, 1): a cubic Hermite
 * whose end slope is e, or a power roll-off when the Hermite would overshoot. Lowered is its
 * inverse. */
#define BRIGHTNESS_KNEE (126.0f / 255.0f)

typedef struct brightness_curve {
    bool raise;
    float s;
    float e;
    float x0;
    float alpha;
    bool hermite;
} brightness_curve;

static brightness_curve brightness_new(float brightness)
{
    brightness_curve c;
    float b = fabsf(brightness);
    float secant;
    float beta;
    c.s = powf(1.375f, b / 50.0f);
    c.e = 1.0f / (1.0f + 0.084f * b);
    c.x0 = pc_rs_minf_k(BRIGHTNESS_KNEE / c.s, 1.0f);
    secant = (1.0f - BRIGHTNESS_KNEE) / pc_rs_maxf_k(1.0f - c.x0, 1e-6f);
    c.alpha = c.s / secant;
    beta = c.e / secant;
    c.raise = brightness > 0.0f;
    c.hermite = c.alpha * c.alpha + beta * beta <= 9.0f;
    return c;
}

/* The curve raised: the line, then a monotone roll-off to (1, 1). */
static float brightness_raised(const brightness_curve *c, float x)
{
    float y0 = BRIGHTNESS_KNEE;
    float h;
    float t;
    float t2;
    float t3;
    float y;
    if (x <= c->x0) {
        return pc_rs_minf(y0, c->s * x);
    }
    h = pc_rs_maxf_k(1.0f - c->x0, 1e-6f);
    t = pc_clampf((x - c->x0) / h, 0.0f, 1.0f);
    if (c->hermite) {
        t2 = t * t;
        t3 = t * t * t;
        y = (2.0f * t3 - 3.0f * t2 + 1.0f) * y0 + (t3 - 2.0f * t2 + t) * h * c->s +
            (-2.0f * t3 + 3.0f * t2) + (t3 - t2) * h * c->e;
    } else {
        y = 1.0f - (1.0f - y0) * powf(1.0f - t, c->alpha);
    }
    return pc_clampf(y, 0.0f, 1.0f);
}

static float brightness_eval(const brightness_curve *c, float v)
{
    float lo;
    float hi;
    int i;
    v = pc_clampf(v, 0.0f, 1.0f);
    if (c->raise) {
        return brightness_raised(c, v);
    }
    if (v <= 0.0f || v >= 1.0f) {
        return v;
    }
    /* The inverse of the raised curve (monotone on [0, 1]), by bisection. */
    lo = 0.0f;
    hi = 1.0f;
    for (i = 0; i < 32; i++) {
        float mid = 0.5f * (lo + hi);
        if (brightness_raised(c, mid) < v) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5f * (lo + hi);
}

float pc_compose_modern_brightness(float v, float brightness)
{
    brightness_curve c;
    if (brightness == 0.0f || !isfinite(brightness)) {
        return v;
    }
    c = brightness_new(brightness);
    return brightness_eval(&c, v);
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

/* Modern Brightness/Contrast as one channel's lookup table (LUT samples over 0..1): brightness
 * (raised directly, lowered inverted through a finer sampling in one monotone pass) then contrast,
 * as Photoshop applies them. Fills out[LUT]. */
pc_status pc_cmp_modern_lut(float brightness, float contrast, float *out, pc_error *err)
{
    size_t k;
    if (brightness == 0.0f || !isfinite(brightness)) {
        for (k = 0; k < LUT; k++) {
            out[k] = (float)k / (float)(LUT - 1);
        }
    } else if (brightness > 0.0f) {
        brightness_curve c = brightness_new(brightness);
        for (k = 0; k < LUT; k++) {
            out[k] = brightness_raised(&c, (float)k / (float)(LUT - 1));
        }
    } else {
        brightness_curve c = brightness_new(-brightness);
        size_t fine = 4 * LUT;
        size_t j = 0;
        float *ys = pc_malloc_array(fine, sizeof *ys);
        if (ys == NULL) {
            return nomem(err);
        }
        for (k = 0; k < fine; k++) {
            ys[k] = brightness_raised(&c, (float)k / (float)(fine - 1));
        }
        for (k = 0; k < LUT; k++) {
            float y = (float)k / (float)(LUT - 1);
            float y0;
            float y1;
            float f;
            while (j + 2 < fine && ys[j + 1] < y) {
                j++;
            }
            y0 = ys[j];
            y1 = ys[j + 1];
            f = y1 > y0 ? pc_clampf((y - y0) / (y1 - y0), 0.0f, 1.0f) : 0.0f;
            out[k] = pc_clampf((float)j / (float)(fine - 1) + f / (float)(fine - 1), 0.0f, 1.0f);
        }
        free(ys);
    }
    for (k = 0; k < LUT; k++) {
        out[k] = pc_clampf(pc_compose_modern_contrast(out[k], contrast), 0.0f, 1.0f);
    }
    return PC_OK;
}

float pc_compose_posterize(float v, uint32_t levels)
{
    /* n equal input bins over 0..=255, output levels floor(k * 255 / (n - 1)). */
    float n = (float)(levels < 2 ? 2 : (levels > 255 ? 255 : levels));
    float x = roundf(pc_clampf(v, 0.0f, 1.0f) * 255.0f);
    float bin = pc_rs_minf(floorf(x * n / 256.0f), n - 1.0f);
    return floorf(bin * 255.0f / (n - 1.0f)) / 255.0f;
}

float pc_compose_posterize_in(float v, uint32_t levels, bool u16)
{
    double n;
    double x;
    double bin;
    double step;
    if (!u16) {
        return pc_compose_posterize(v, levels);
    }
    /* 16 bits: bin the 16-bit sample and land on 8-bit levels through a 16-bit step, so 127.5
     * comes out as 128 but 42.5 as 42 (psd-tools posterize_16bits). */
    n = (double)(levels < 2 ? 2 : (levels > 255 ? 255 : levels));
    x = round((double)pc_clampf(v, 0.0f, 1.0f) * 32768.0);
    /* .min(n - 1.0): Rust's min returns the non-NaN operand, so a NaN sample lands on the top
     * bin rather than propagating (matches the U8 path's pc_rs_minf). */
    bin = pc_rs_min(floor(x * n / 32768.0), n - 1.0);
    step = floor(bin * 32768.0 / (n - 1.0));
    return (float)(floor((step * 255.0 + 16384.0) / 32768.0) / 255.0);
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

/* threshold_white depth cases: 0 = None/U8, 1 = U16, 2 = F32. */
typedef struct threshold_ctx {
    float t; /* round(level * 255) */
    int depth;
} threshold_ctx;

/* threshold_white (#5263): Photoshop splits on the luminosity 0.30 R + 0.59 G + 0.11 B of the
 * document's WHOLE sample values, not the Rec. 601 grey, depth by depth. */
static void f_threshold(void *ctx, const float c[3], float out[3])
{
    const threshold_ctx *x = ctx;
    float t = x->t;
    bool white;
    if (x->depth == 2) {
        white = (0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]) * 255.0f >= t;
    } else {
        float q = x->depth == 1 ? 32768.0f : 255.0f;
        float r = roundf(pc_clampf(c[0], 0.0f, 1.0f) * q);
        float g = roundf(pc_clampf(c[1], 0.0f, 1.0f) * q);
        float b = roundf(pc_clampf(c[2], 0.0f, 1.0f) * q);
        float sum = 30.0f * r + 59.0f * g + 11.0f * b;
        white = x->depth == 1 ? (sum >= 100.0f * roundf(t * 32768.0f / 255.0f)) : (sum > 100.0f * t - 50.0f);
    }
    out[0] = out[1] = out[2] = white ? 1.0f : 0.0f;
}

typedef struct posterize_ctx {
    uint32_t n;
    bool u16;
} posterize_ctx;

static void f_posterize(void *ctx, const float c[3], float out[3])
{
    const posterize_ctx *x = ctx;
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = pc_compose_posterize_in(c[i], x->n, x->u16);
    }
}

typedef struct bc_ctx {
    float b;
    float c; /* modern */
    pc_cmp_legacy_bc legacy;
    const float *modern; /* LUT entries for the modern curve */
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

/* Legacy Brightness/Contrast in a 32-bit document: contrast pivots at 127/255 (not 128/255) and
 * the result is NOT clamped, so negative and over-white values survive (photoshop corpus rgb32 /
 * gray32 brightness-contrast-legacy.psd). k/pre/post are pivot-independent, so the shared struct
 * serves both. */
#define PC_COMPOSE_LEGACY_PIVOT_HDR (127.0f / 255.0f)
static void f_legacy_bc_hdr(void *ctx, const float c[3], float out[3])
{
    const bc_ctx *x = ctx;
    const pc_cmp_legacy_bc *l = &x->legacy;
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = (c[i] + l->pre - PC_COMPOSE_LEGACY_PIVOT_HDR) * l->k + PC_COMPOSE_LEGACY_PIVOT_HDR + l->post;
    }
}

static void f_modern_bc(void *ctx, const float c[3], float out[3])
{
    const bc_ctx *x = ctx;
    int i;
    for (i = 0; i < 3; i++) {
        out[i] = pc_cmp_lut(x->modern, LUT, c[i]);
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
    bool hdr;
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
        float sat = pc_compose_colorize_saturation(pc_clampf(x->saturation / 100.0f + ds, -1.0f, 1.0f),
                                                   x->lightness / 100.0f, x->colorize, x->hdr);
        pc_compose_hue_saturation_in(c, x->hue + dh, sat, pc_clampf(x->lightness / 100.0f, -1.0f, 1.0f),
                                     x->colorize, x->hdr, o);
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
    bool hdr; /* 32-bit: coefficients cut to 1/1024, nothing clips */
} mixer_ctx;

static float mixer_row(const float row[4], const float c[3], bool hdr)
{
    float v = row[0] * c[0] + row[1] * c[1] + row[2] * c[2] + row[3];
    return hdr ? v : pc_clampf(v, 0.0f, 1.0f);
}

static void f_mixer(void *ctx, const float c[3], float out[3])
{
    const mixer_ctx *x = ctx;
    if (x->mono) {
        out[0] = out[1] = out[2] = mixer_row(x->m[0], c, x->hdr);
        return;
    }
    out[0] = mixer_row(x->m[0], c, x->hdr);
    out[1] = mixer_row(x->m[1], c, x->hdr);
    out[2] = mixer_row(x->m[2], c, x->hdr);
}

typedef struct pf_ctx {
    float m[3][3];
    bool set_lum;
    bool linear; /* 32-bit: samples are already linear and may leave 0..1 unclipped */
    pc_compose_transfer t;
} pf_ctx;

static void f_photo_filter(void *ctx, const float c[3], float out[3])
{
    const pf_ctx *x = ctx;
    float d[3];
    float f[3];
    int i;
    if (x->linear) {
        /* 32-bit samples are linear already, and the result isn't clipped (photoshop corpus rgb32
         * photo-filter.psd: white filters to 1.19 red). */
        pc_color_mat3_mul(x->m, c, f);
        memcpy(out, f, sizeof f);
        return;
    }
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
    bool hdr;          /* 32-bit: tint through the Color blend mode, nothing clips */
} bw_ctx;

static void f_black_white(void *ctx, const float c[3], float out[3])
{
    const bw_ctx *x = ctx;
    float g = pc_compose_black_white_gray(c, x->weights);
    int i;
    if (x->tint != NULL) {
        /* Photoshop tints like the Color blend mode: the tint's hue and saturation at the grey's
         * luminosity (SetLum + ClipColor, Photoshop 27.11). In 32-bit documents nothing clips at 1
         * (photoshop corpus rgb32 black-white-tint.psd). */
        float t[3];
        if (x->hdr) {
            float bg[3] = {g, g, g};
            pc_blend_rgb_within(PC_BLEND_COLOR, bg, x->tint, FLT_MAX, out);
            return;
        }
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
            /* Driven by the W3C luminosity (0.3, 0.59, 0.11), not the Rec. 601 grey (#5018). */
            float t = pc_blend_lum(p);
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
        threshold_ctx x;
        x.t = roundf(adj->u.threshold.level * 255.0f);
        x.depth = !has_depth || depth == PC_SAMPLE_TYPE_U8 ? 0 : (depth == PC_SAMPLE_TYPE_U16 ? 1 : 2);
        map_rgb(buf, f_threshold, &x);
        return PC_OK;
    }
    case PC_ADJ_POSTERIZE: {
        posterize_ctx x;
        x.n = adj->u.posterize.levels;
        x.u16 = has_depth && depth == PC_SAMPLE_TYPE_U16;
        map_rgb(buf, f_posterize, &x);
        return PC_OK;
    }
    case PC_ADJ_BRIGHTNESS_CONTRAST: {
        bc_ctx x;
        x.b = adj->u.brightness_contrast.brightness;
        x.c = adj->u.brightness_contrast.contrast;
        x.legacy = pc_cmp_legacy_bc_new(x.b, x.c);
        x.modern = NULL;
        if (adj->u.brightness_contrast.legacy) {
            map_rgb(buf, f32 ? f_legacy_bc_hdr : f_legacy_bc, &x);
        } else {
            /* Modern (CS3+): smooth curves pinning black and white, brightness then contrast,
             * through one lookup table. */
            float *table = pc_malloc_array(LUT, sizeof *table);
            pc_status st;
            if (table == NULL) {
                return nomem(err);
            }
            st = pc_cmp_modern_lut(x.b, x.c, table, err);
            if (st != PC_OK) {
                free(table);
                return st;
            }
            x.modern = table;
            map_rgb(buf, f_modern_bc, &x);
            free(table);
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
        x.hdr = f32;
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
        float cut[3][4];
        x.mono = adj->u.channel_mixer.monochrome;
        x.hdr = f32;
        if (f32) {
            /* Each source percentage is cut to 1/1024 (toward zero), the constant counts 255/512
             * of its value, and nothing clips (photoshop corpus rgb32 channel-mixer.psd). */
            int r;
            int ci;
            for (r = 0; r < 3; r++) {
                for (ci = 0; ci < 3; ci++) {
                    cut[r][ci] = truncf(adj->u.channel_mixer.matrix[r][ci] * 1024.0f) / 1024.0f;
                }
                cut[r][3] = adj->u.channel_mixer.matrix[r][3] * 255.0f / 512.0f;
            }
            x.m = (const float (*)[4])cut;
        } else {
            x.m = (const float (*)[4])adj->u.channel_mixer.matrix;
        }
        map_rgb(buf, f_mixer, &x);
        return PC_OK;
    }
    case PC_ADJ_PHOTO_FILTER: {
        pf_ctx x;
        bool pl = adj->u.photo_filter.preserve_luminosity;
        pc_compose_photo_filter_matrix(adj->u.photo_filter.color, adj->u.photo_filter.density, pl && f32,
                                       x.m);
        x.set_lum = pl && !f32;
        x.linear = f32;
        x.t = transfer;
        map_rgb(buf, f_photo_filter, &x);
        return PC_OK;
    }
    case PC_ADJ_BLACK_WHITE: {
        bw_ctx x;
        x.weights = adj->u.black_white.weights;
        x.tint = adj->u.black_white.has_tint ? adj->u.black_white.tint : NULL;
        x.hdr = f32;
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
