/* Adjustments evaluated on a Lab document's L, a and b channels, Color Balance's per-channel
 * curves and a Hue/Saturation range's Lightness (port of the parts of crates/compose/src/adjust.rs
 * added after C2: apply_in, lab_channel_adjustment, apply_lab, color_balance_channel,
 * range_lightness). Expression order follows the reference exactly. */
#include <math.h>
#include <string.h>

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

/* How far one slider at +-100 moves the black or white point (Photoshop 27.11: shadows -100 maps
 * 99.6 of 255 to black, highlights +100 maps 155.5 to white). */
#define BALANCE_LEVELS 0.39f

static float balance_black(float v, float b)
{
    return pc_rs_maxf_k((v - b) / (1.0f - b), 0.0f);
}

static float balance_white(float v, float w)
{
    return pc_rs_minf_k(v / w, 1.0f);
}

float pc_compose_color_balance_channel(const float tones[3][3], bool preserve_luminosity, int ch, float v)
{
    /* Reverse-engineered from Photoshop 27.11: every slider is a levels-and-gamma curve per
     * channel. Preserve Luminosity off: a slider moves only its own channel (midtones gamma
     * 2^-a; shadows gamma 2^(-a/2) after a black point at 0.39 |a| when lowered; highlights gamma
     * 2^(-a/2) after a white point at 1 - 0.39 a when raised). On: midtones is gamma 2^(-a/2) on
     * the slider's channel and 2^(a/2) on the others; shadows and highlights go by the
     * differences between a tone's three sliders (plain black / white points). Tones apply
     * highlights, shadows, then midtones. */
    static const int order[3] = {2, 0, 1};
    int t;
    v = pc_clampf(v, 0.0f, 1.0f);
    for (t = 0; t < 3; t++) {
        int tone = order[t];
        const float *sliders = tones[tone];
        float a[3];
        int src;
        for (src = 0; src < 3; src++) {
            a[src] = pc_clampf(sliders[src] / 100.0f, -1.0f, 1.0f);
        }
        if (preserve_luminosity && tone != 1) {
            float own = ch >= 0 && ch < 3 ? a[ch] : 0.0f;
            float lo = FLT_MAX;
            float hi = -FLT_MAX;
            for (src = 0; src < 3; src++) {
                lo = pc_rs_minf(lo, a[src]);
                hi = pc_rs_maxf(hi, a[src]);
            }
            v = tone == 0 ? balance_black(v, BALANCE_LEVELS * (hi - own))
                          : balance_white(v, 1.0f - BALANCE_LEVELS * (own - lo));
            continue;
        }
        for (src = 0; src < 3; src++) {
            float k = pc_clampf(sliders[src] / 100.0f, -1.0f, 1.0f);
            bool own = src == ch;
            if (k == 0.0f || (!preserve_luminosity && !own)) {
                continue;
            }
            if (tone == 1 && !preserve_luminosity) {
                v = powf(v, exp2f(-k));
            } else if (tone == 1) {
                v = powf(v, exp2f((own ? -k : k) / 2.0f));
            } else if (tone == 0 && !preserve_luminosity && k < 0.0f) {
                v = powf(balance_black(v, BALANCE_LEVELS * -k), exp2f(-k / 2.0f));
            } else if (tone == 0 && !preserve_luminosity) {
                v = powf(v, exp2f(-k / 2.0f));
            } else if (tone == 2 && !preserve_luminosity && k > 0.0f) {
                v = powf(balance_white(v, 1.0f - BALANCE_LEVELS * k), exp2f(-k / 2.0f));
            } else if (tone == 2 && !preserve_luminosity) {
                v = powf(v, exp2f(-k / 2.0f));
            }
        }
    }
    return v;
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

static void apply_lab(const pc_adjustment *adj, pc_compose_buffer *buf)
{
    uint32_t bw = pc_rect_width(buf->rect);
    size_t w = bw > 1 ? bw : 1;
    pc_cmp_legacy_bc legacy = {0.0f, 0.0f, 0.0f};
    float threshold = 0.0f;
    size_t i;
    int c;
    if (adj->kind == PC_ADJ_THRESHOLD) {
        threshold = roundf(adj->u.threshold.level * 255.0f);
    } else if (adj->kind == PC_ADJ_BRIGHTNESS_CONTRAST) {
        legacy =
            pc_cmp_legacy_bc_new(adj->u.brightness_contrast.brightness, adj->u.brightness_contrast.contrast);
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
            /* n -> 1 - n on every channel: L -> 100 - L and a, b mirrored about neutral. */
            for (c = 0; c < 3; c++) {
                n[c] = 1.0f - pc_clampf(n[c], 0.0f, 1.0f);
            }
            lab_denorm(n, o);
            break;
        case PC_ADJ_POSTERIZE:
            for (c = 0; c < 3; c++) {
                n[c] = pc_compose_posterize(n[c], adj->u.posterize.levels);
            }
            lab_denorm(n, o);
            break;
        case PC_ADJ_THRESHOLD:
            /* Split on L alone (on the 8-bit grid); the result is neutral black or white. */
            o[0] = o[1] = o[2] = roundf(pc_clampf(n[0], 0.0f, 1.0f) * 255.0f) >= threshold ? 1.0f : 0.0f;
            break;
        case PC_ADJ_BRIGHTNESS_CONTRAST: {
            /* The tone curve on L only, added as the difference of two round trips (like
             * tone_in_space) so a and b, and colours a round trip moves, stay put. */
            float l =
                adj->u.brightness_contrast.legacy
                    ? pc_cmp_legacy_bc_apply(&legacy, n[0])
                    : pc_clampf(pc_compose_modern_contrast(
                                    pc_compose_modern_brightness(n[0], adj->u.brightness_contrast.brightness),
                                    adj->u.brightness_contrast.contrast),
                                0.0f, 1.0f);
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

pc_status pc_compose_adjust_apply_in(const pc_adjustment *adj, pc_compose_buffer *buf,
                                     pc_compose_transfer transfer, bool has_depth, pc_sample_type depth,
                                     pc_color_mode mode, pc_error *err)
{
    if (mode == PC_COLOR_MODE_LAB && pc_compose_lab_channel_adjustment(adj)) {
        apply_lab(adj, buf);
        return PC_OK;
    }
    return pc_compose_adjust_apply_depth(adj, buf, transfer, has_depth, depth, err);
}
