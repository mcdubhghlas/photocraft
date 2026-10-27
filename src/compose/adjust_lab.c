/* Adjustments evaluated on a Lab document's L, a and b channels, Color Balance's per-channel
 * curves and a Hue/Saturation range's Lightness (port of the parts of crates/compose/src/adjust.rs
 * added after C2: apply_in, lab_channel_adjustment, apply_lab, color_balance_channel,
 * range_lightness). Expression order follows the reference exactly. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "color/convert.h"
#include "compose/adjust_internal.h"
#include "compose/internal.h"

/* ---- Hue/Saturation ranges ---- */

void pc_compose_range_lightness(const float c[3], float l, float out[3])
{
    /* Towards the pixel's brightest channel when raised, its darkest when lowered, not towards
     * white or black: Blues +50 turns (0, 29, 115) into (57, 71, 115) (Photoshop 27.11). */
    float k = pc_clampf(l, -1.0f, 1.0f);
    float target =
        k > 0.0f ? pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]) : pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
    float r[3];
    int i;
    for (i = 0; i < 3; i++) {
        r[i] = c[i] + (target - c[i]) * fabsf(k);
    }
    memcpy(out, r, sizeof r);
}

/* ---- Color Balance ---- */

/* One channel's Color Balance curve. Reverse-engineered from Photoshop: the three tones fold into
 * one Levels per channel (input black point, input white point, midtone gamma), within 0.8 of 255
 * everywhere. Folding matters: a lowered shadows and a raised highlights slider cancel each other's
 * gamma, which applying the tones one after another doesn't (#3423).
 * - Preserve Luminosity off: a slider only moves its own channel. Lowered shadows set the black
 *   point to |shadows| levels, raised highlights set the white point to 255 - highlights levels,
 *   and the gamma is 2^(m / 100) with m = midtones + (shadows + highlights) / 2.
 * - Preserve Luminosity on: each channel's black point is (max - own) shadows levels, its white
 *   point 255 - (own - min) highlights levels, and m = own - (max + min) / 2 midtones. */
typedef struct balance_curve {
    float b; /* input black point */
    float w; /* input white point */
    float e; /* the power curve's exponent, 1 / gamma */
    bool has_knee;
    float u0; /* where a gamma > 1 curve's Hermite join ends */
    float s0; /* its starting slope 2^gamma */
    float p;  /* the power curve's value at u0 */
    float d;  /* the power curve's slope at u0 */
} balance_curve;

static float balance_clamp_slider(float s)
{
    return isfinite(s) ? pc_clampf(s, -100.0f, 100.0f) : 0.0f;
}

static balance_curve balance_curve_new(const float tones[3][3], bool preserve_luminosity, int ch)
{
    balance_curve c;
    float shadows[3];
    float midtones[3];
    float highlights[3];
    float black;
    float white;
    float m;
    float gamma;
    int i;
    for (i = 0; i < 3; i++) {
        shadows[i] = balance_clamp_slider(tones[0][i]);
        midtones[i] = balance_clamp_slider(tones[1][i]);
        highlights[i] = balance_clamp_slider(tones[2][i]);
    }
#define OWN(t) ((ch) >= 0 && (ch) < 3 ? (t)[(ch)] : 0.0f)
#define MX(t) pc_rs_maxf(pc_rs_maxf((t)[0], (t)[1]), (t)[2])
#define MN(t) pc_rs_minf(pc_rs_minf((t)[0], (t)[1]), (t)[2])
    if (preserve_luminosity) {
        black = MX(shadows) - OWN(shadows);
        white = OWN(highlights) - MN(highlights);
        m = OWN(midtones) - (MX(midtones) + MN(midtones)) / 2.0f;
    } else {
        black = pc_rs_maxf_k(-OWN(shadows), 0.0f);
        white = pc_rs_maxf_k(OWN(highlights), 0.0f);
        m = OWN(midtones) + (OWN(shadows) + OWN(highlights)) / 2.0f;
    }
#undef OWN
#undef MX
#undef MN
    /* The midtone gamma of the channel's Levels: u^(1/gamma), except that a gamma > 1 (which would
     * start with infinite slope) starts at slope 2^gamma and joins the power curve in a cubic
     * Hermite at twice the input where that line meets it. */
    gamma = exp2f(m / 100.0f);
    c.e = 1.0f / pc_rs_maxf_k(gamma, 0.01f);
    c.has_knee = false;
    c.u0 = c.s0 = c.p = c.d = 0.0f;
    if (gamma > 1.0f) {
        float s0 = exp2f(gamma);
        float u0 = pc_rs_minf_k(2.0f * powf(s0, 1.0f / (c.e - 1.0f)), 1.0f);
        if (!isnan(u0) && u0 > 0.0f) {
            c.has_knee = true;
            c.u0 = u0;
            c.s0 = s0;
            c.p = powf(u0, c.e);
            c.d = c.e * powf(u0, c.e - 1.0f);
        }
    }
    c.b = black / 255.0f;
    c.w = 1.0f - white / 255.0f;
    return c;
}

static float balance_curve_eval(const balance_curve *c, float v)
{
    float u;
    v = isfinite(v) ? pc_clampf(v, 0.0f, 1.0f) : 0.0f;
    if (v >= c->w) {
        return 1.0f;
    }
    if (v <= c->b) {
        return 0.0f;
    }
    u = (v - c->b) / (c->w - c->b);
    if (c->has_knee && u < c->u0) {
        float t = u / c->u0;
        float t2 = t * t;
        float t3 = t * t * t;
        return pc_clampf((t3 - 2.0f * t2 + t) * c->u0 * c->s0 + (3.0f * t2 - 2.0f * t3) * c->p +
                             (t3 - t2) * c->u0 * c->d,
                         0.0f, 1.0f);
    }
    return powf(u, c->e);
}

float pc_compose_color_balance_channel(const float tones[3][3], bool preserve_luminosity, int ch, float v)
{
    balance_curve c = balance_curve_new(tones, preserve_luminosity, ch);
    return balance_curve_eval(&c, v);
}

/* ---- Lab documents ---- */

bool pc_compose_lab_channel_adjustment(const pc_adjustment *adj)
{
    switch (adj->kind) {
    case PC_ADJ_INVERT:
    case PC_ADJ_POSTERIZE:
    case PC_ADJ_THRESHOLD:
    case PC_ADJ_BRIGHTNESS_CONTRAST:
    case PC_ADJ_GRADIENT_MAP:
        return true;
    default:
        return false;
    }
}

/* Whether stored_channels evaluates adj: Invert, Posterize and Threshold. */
static bool stored_channel_adjustment(const pc_adjustment *adj)
{
    switch (adj->kind) {
    case PC_ADJ_INVERT:
    case PC_ADJ_POSTERIZE:
    case PC_ADJ_THRESHOLD:
        return true;
    default:
        return false;
    }
}

/* A CMYK document applies Hue/Saturation (not colorize) and Color Balance to its C, M and Y inks
 * as a screen-colour adjustment of (1 - ink). */
bool pc_cmp_cmy_adjustment(const pc_adjustment *adj)
{
    return (adj->kind == PC_ADJ_HUE_SATURATION && !adj->u.hue_saturation.colorize) ||
           adj->kind == PC_ADJ_COLOR_BALANCE;
}

/* Whether a CMYK document evaluates adj on its inks rather than on the screen colour: Invert and
 * Posterize (each ink, black included, so paper inverts to 400% ink), Threshold (paper or 100%
 * black ink alone), Brightness/Contrast (each ink's stored value 1 - ink through the tone curve)
 * and the CMY adjustments (Hue/Saturation not colorize, Color Balance). */
bool pc_compose_cmyk_channel_adjustment(const pc_adjustment *adj)
{
    return stored_channel_adjustment(adj) || pc_cmp_cmy_adjustment(adj) ||
           adj->kind == PC_ADJ_BRIGHTNESS_CONTRAST;
}

/* Invert, Posterize or Threshold on one pixel's own channels in a Lab or CMYK document of depth
 * (has_depth false: unknown, treated as 8-bit): ch holds L, a, b normalised as lab_norm gives
 * them, or the C, M, Y, K inks (0: none); gray is the pixel's luma, which CMYK Threshold compares.
 * Lab Invert turns 8-bit bytes v into 255 - v (a becomes -1 - a) and mirrors a and b around
 * neutral at 16 bits (photoshop corpus lab16 invert.psd: neutral stays neutral). */
void pc_cmp_stored_channels(const pc_adjustment *adj, pc_color_mode mode, bool has_depth,
                            pc_sample_type depth, float *ch, size_t nch, float gray)
{
    bool lab = mode == PC_COLOR_MODE_LAB;
    size_t i;
    switch (adj->kind) {
    case PC_ADJ_INVERT: {
        float mirror = (lab && has_depth && depth != PC_SAMPLE_TYPE_U8) ? 256.0f / 255.0f : 1.0f;
        for (i = 0; i < nch; i++) {
            ch[i] = (i > 0 ? mirror : 1.0f) - pc_clampf(ch[i], 0.0f, 1.0f);
        }
        break;
    }
    case PC_ADJ_POSTERIZE: {
        /* Depth-aware (16-bit documents bin the 16-bit sample). CMYK posterizes the STORED value
         * (1 - ink), which its uneven levels make differ from posterizing the ink. */
        bool u16 = has_depth && depth == PC_SAMPLE_TYPE_U16;
        bool cmyk = mode == PC_COLOR_MODE_CMYK;
        for (i = 0; i < nch; i++) {
            ch[i] = cmyk ? 1.0f - pc_compose_posterize_in(1.0f - ch[i], adj->u.posterize.levels, u16)
                         : pc_compose_posterize_in(ch[i], adj->u.posterize.levels, u16);
        }
        break;
    }
    case PC_ADJ_THRESHOLD: {
        /* On the 8-bit grid, like the RGB threshold. */
        float t = roundf(adj->u.threshold.level * 255.0f);
        float out[4];
        if (lab) {
            bool white = nch >= 1 && roundf(pc_clampf(ch[0], 0.0f, 1.0f) * 255.0f) >= t;
            out[0] = white ? 1.0f : 0.0f;
            out[1] = 128.0f / 255.0f;
            out[2] = 128.0f / 255.0f;
            out[3] = 0.0f;
        } else {
            out[0] = 0.0f;
            out[1] = 0.0f;
            out[2] = 0.0f;
            out[3] = roundf(gray * 255.0f) < t ? 1.0f : 0.0f;
        }
        for (i = 0; i < nch && i < 4; i++) {
            ch[i] = out[i];
        }
        break;
    }
    default:
        break;
    }
}

/* Encoded sRGB -> Lab normalised the way Photoshop's channels hold it: L/100, (a+128)/255,
 * (b+128)/255. */
static void lab_norm(const float c[3], float out[3])
{
    float l[3];
    pc_srgb_to_lab(c, l);
    out[0] = l[0] / 100.0f;
    out[1] = (l[1] + 128.0f) / 255.0f;
    out[2] = (l[2] + 128.0f) / 255.0f;
}

static void lab_denorm(const float n[3], float out[3])
{
    const float lab[3] = {n[0] * 100.0f, n[1] * 255.0f - 128.0f, n[2] * 255.0f - 128.0f};
    pc_lab_to_srgb(lab, out);
}

static void apply_lab(const pc_adjustment *adj, pc_compose_buffer *buf, bool has_depth, pc_sample_type depth)
{
    uint32_t bw = pc_rect_width(buf->rect);
    size_t w = bw > 1 ? bw : 1;
    pc_cmp_legacy_bc legacy = {0.0f, 0.0f, 0.0f};
    float threshold = 0.0f;
    float modern_table[PC_COMPOSE_LUT_SIZE];
    bool use_modern = false;
    size_t i;
    int c;
    if (adj->kind == PC_ADJ_THRESHOLD) {
        threshold = roundf(adj->u.threshold.level * 255.0f);
    } else if (adj->kind == PC_ADJ_BRIGHTNESS_CONTRAST) {
        legacy =
            pc_cmp_legacy_bc_new(adj->u.brightness_contrast.brightness, adj->u.brightness_contrast.contrast);
        /* Modern (CS3+) BC is a lookup table (brightness then contrast), sampled like the RGB
         * path; the legacy curve is linear and needs no table. */
        if (!adj->u.brightness_contrast.legacy &&
            pc_cmp_modern_lut(adj->u.brightness_contrast.brightness, adj->u.brightness_contrast.contrast,
                              modern_table, NULL) == PC_OK) {
            use_modern = true;
        }
    }
    for (i = 0; i < buf->len; i++) {
        float *p = buf->px[i];
        float n[3];
        float o[3];
        if (p[3] <= 0.0f) {
            continue;
        }
        lab_norm(p, n);
        switch (adj->kind) {
        case PC_ADJ_INVERT:
        case PC_ADJ_POSTERIZE:
            /* L -> 100 - L and a, b mirrored about neutral; each channel posterized (see
             * stored_channels). */
            pc_cmp_stored_channels(adj, PC_COLOR_MODE_LAB, has_depth, depth, n, 3, 0.0f);
            lab_denorm(n, o);
            break;
        case PC_ADJ_THRESHOLD:
            /* Split on L alone (on the 8-bit grid); the result is neutral black or white. */
            o[0] = o[1] = o[2] = roundf(pc_clampf(n[0], 0.0f, 1.0f) * 255.0f) >= threshold ? 1.0f : 0.0f;
            break;
        case PC_ADJ_BRIGHTNESS_CONTRAST: {
            /* The tone curve on L only, added as the difference of two round trips (like
             * tone_in_space) so a and b, and colours a round trip moves, stay put. */
            float l = use_modern ? pc_cmp_lut(modern_table, PC_COMPOSE_LUT_SIZE, n[0])
                                 : pc_cmp_legacy_bc_apply(&legacy, n[0]);
            float before[3];
            float after[3];
            float m[3];
            if (l == n[0]) {
                continue;
            }
            m[0] = l;
            m[1] = n[1];
            m[2] = n[2];
            lab_denorm(n, before);
            lab_denorm(m, after);
            for (c = 0; c < 3; c++) {
                o[c] = pc_clampf(p[c] + after[c] - before[c], 0.0f, 1.0f);
            }
            break;
        }
        case PC_ADJ_GRADIENT_MAP: {
            /* Driven by L: equal lightness maps to the same stop colour whatever the chroma. */
            float l = pc_clampf(n[0], 0.0f, 1.0f);
            pc_cmp_gradient_map_color(adj->u.gradient_map.stops, adj->u.gradient_map.nstops,
                                      adj->u.gradient_map.reverse ? 1.0f - l : l, o);
            if (adj->u.gradient_map.dither) {
                float d = pc_compose_bayer4(pc_i32_wrapping_add(buf->rect.x0, (int32_t)(uint32_t)(i % w)),
                                            pc_i32_wrapping_add(buf->rect.y0, (int32_t)(uint32_t)(i / w))) /
                          255.0f;
                for (c = 0; c < 3; c++) {
                    o[c] = pc_clampf(o[c] + d, 0.0f, 1.0f);
                }
            }
            break;
        }
        default:
            return;
        }
        p[0] = o[0];
        p[1] = o[1];
        p[2] = o[2];
    }
}

/* ink v through the ink tone table (built over the stored value 1 - ink): ink_through. */
float pc_cmp_ink_through(const float *table, float ink)
{
    return 1.0f - pc_cmp_lut(table, PC_COMPOSE_LUT_SIZE, 1.0f - ink);
}

/* The luma Photoshop's CMYK Threshold compares: Rec. 601 weights on the inks' plain complement,
 * (1 - c)(1 - k) and so on, whatever the document's profile (adjust.rs ink_luma). */
float pc_cmp_ink_luma(const float ink[4])
{
    float paper = 1.0f - pc_clampf(ink[3], 0.0f, 1.0f);
    float rgb[3];
    int i;
    for (i = 0; i < 3; i++) {
        rgb[i] = (1.0f - pc_clampf(ink[i], 0.0f, 1.0f)) * paper;
    }
    return pc_rgb_to_gray(rgb);
}

/* Brightness/Contrast as the ink tone table (ink_brightness_contrast_lut): the modern LUT, or the
 * legacy curve pivoting at 1 - 128/255 (the stored value's pivot). Fills table[PC_COMPOSE_LUT_SIZE]. */
pc_status pc_cmp_ink_bc_lut(float brightness, float contrast, bool legacy_flag, float *table, pc_error *err)
{
    size_t k;
    pc_cmp_legacy_bc l;
    float pivot;
    if (!legacy_flag) {
        return pc_cmp_modern_lut(brightness, contrast, table, err);
    }
    l = pc_cmp_legacy_bc_new(brightness, contrast);
    pivot = 1.0f - PC_COMPOSE_LEGACY_PIVOT;
    for (k = 0; k < PC_COMPOSE_LUT_SIZE; k++) {
        float v = (float)k / (float)(PC_COMPOSE_LUT_SIZE - 1);
        table[k] = pc_clampf((v + l.pre - pivot) * l.k + pivot + l.post, 0.0f, 1.0f);
    }
    return PC_OK;
}

/* Hue/Saturation (not colorize) and Color Balance on a CMYK document's C, M and Y inks: the inks
 * as screen-colour (1 - ink) through apply_depth, then stored back (apply_cmy). K is untouched. */
pc_status pc_cmp_apply_cmy(const pc_adjustment *adj, float (*inks)[4], size_t n, pc_sample_type depth,
                           pc_error *err)
{
    const pc_compose_transfer srgb = {PC_COMPOSE_TRANSFER_SRGB, 0.0f};
    pc_compose_buffer tmp;
    pc_status st;
    size_t i;
    int c;
    if (n == 0) {
        return PC_OK;
    }
    st = pc_compose_buffer_transparent(pc_rect_make(0, 0, (int32_t)(n > (size_t)INT32_MAX ? INT32_MAX : n), 1),
                                       &tmp, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n && i < tmp.len; i++) {
        tmp.px[i][0] = 1.0f - inks[i][0];
        tmp.px[i][1] = 1.0f - inks[i][1];
        tmp.px[i][2] = 1.0f - inks[i][2];
        tmp.px[i][3] = 1.0f;
    }
    st = pc_compose_adjust_apply_depth(adj, &tmp, srgb, false, depth, err);
    for (i = 0; st == PC_OK && i < n && i < tmp.len; i++) {
        for (c = 0; c < 3; c++) {
            inks[i][c] = pc_clampf(1.0f - tmp.px[i][c], 0.0f, 1.0f);
        }
    }
    pc_compose_buffer_fini(&tmp);
    return st;
}

pc_status pc_compose_adjust_apply_in(const pc_adjustment *adj, pc_compose_buffer *buf,
                                     pc_compose_transfer transfer, bool has_depth, pc_sample_type depth,
                                     pc_color_mode mode, pc_error *err)
{
    if (mode == PC_COLOR_MODE_LAB && pc_compose_lab_channel_adjustment(adj)) {
        apply_lab(adj, buf, has_depth, depth);
        return PC_OK;
    }
    /* A CMYK document evaluates some adjustments on its inks (the screen colour separated through
     * the document's CMYK profile, so neutrals carry the black that profile generates). */
    if (mode == PC_COLOR_MODE_CMYK && pc_compose_cmyk_channel_adjustment(adj)) {
        size_t i;
        if (adj->kind == PC_ADJ_BRIGHTNESS_CONTRAST) {
            /* Each ink's stored value (1 - ink) through the Brightness/Contrast tone curve. */
            float *table = pc_malloc_array(PC_COMPOSE_LUT_SIZE, sizeof *table);
            pc_status st;
            if (table == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
            }
            st = pc_cmp_ink_bc_lut(adj->u.brightness_contrast.brightness, adj->u.brightness_contrast.contrast,
                            adj->u.brightness_contrast.legacy, table, err);
            for (i = 0; st == PC_OK && i < buf->len; i++) {
                float *p = buf->px[i];
                float ink[4];
                float rgb[3];
                int c;
                if (p[3] <= 0.0f) {
                    continue;
                }
                pc_rgb_to_cmyk(p, ink);
                for (c = 0; c < 4; c++) {
                    ink[c] = pc_cmp_ink_through(table, ink[c]);
                }
                pc_cmyk_to_rgb(ink, rgb);
                p[0] = rgb[0];
                p[1] = rgb[1];
                p[2] = rgb[2];
            }
            free(table);
            return st;
        }
        if (pc_cmp_cmy_adjustment(adj)) {
            /* The C, M and Y inks through the adjustment as screen colour; K untouched. */
            float (*inks)[4] = pc_malloc_array(buf->len > 0 ? buf->len : 1, sizeof *inks);
            pc_status st;
            if (inks == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
            }
            for (i = 0; i < buf->len; i++) {
                pc_rgb_to_cmyk(buf->px[i], inks[i]);
            }
            st = pc_cmp_apply_cmy(adj, inks, buf->len, depth, err);
            for (i = 0; st == PC_OK && i < buf->len; i++) {
                float *p = buf->px[i];
                float rgb[3];
                if (p[3] <= 0.0f) {
                    continue;
                }
                pc_cmyk_to_rgb(inks[i], rgb);
                p[0] = rgb[0];
                p[1] = rgb[1];
                p[2] = rgb[2];
            }
            free(inks);
            return st;
        }
        /* Invert, Posterize and Threshold on the stored inks. */
        for (i = 0; i < buf->len; i++) {
            float *p = buf->px[i];
            float ink[4];
            float rgb[3];
            if (p[3] <= 0.0f) {
                continue;
            }
            pc_rgb_to_cmyk(p, ink);
            pc_cmp_stored_channels(adj, PC_COLOR_MODE_CMYK, has_depth, depth, ink, 4, pc_cmp_ink_luma(ink));
            pc_cmyk_to_rgb(ink, rgb);
            p[0] = rgb[0];
            p[1] = rgb[1];
            p[2] = rgb[2];
        }
        return PC_OK;
    }
    return pc_compose_adjust_apply_depth(adj, buf, transfer, has_depth, depth, err);
}
