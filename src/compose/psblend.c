/* Photoshop blend variants and the compositing formula (port of crates/compose/src/psblend.rs). */
#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "base/num.h"
#include "base/rmath.h"
#include "color/convert.h"
#include "compose/psblend.h"

static _Thread_local bool lab_mix_flag;
static _Thread_local bool hdr_flag;

bool pc_psblend_set_lab_mix(bool on)
{
    bool prev = lab_mix_flag;
    lab_mix_flag = on;
    return prev;
}

bool pc_psblend_lab_mix(void)
{
    return lab_mix_flag;
}

bool pc_psblend_set_hdr(bool on)
{
    bool prev = hdr_flag;
    hdr_flag = on;
    return prev;
}

bool pc_psblend_hdr(void)
{
    return hdr_flag;
}

static float vivid_light_ps(float cb, float cs)
{
    if (cs <= 0.5f) {
        if (cs <= 0.0f) {
            return 0.0f;
        }
        return 1.0f - pc_rs_minf_k((1.0f - cb) / (2.0f * cs), 1.0f);
    }
    if (cs >= 1.0f) {
        return 1.0f;
    }
    return pc_rs_minf_k(cb / (2.0f * (1.0f - cs)), 1.0f);
}

static float color_burn(float cb, float cs)
{
    if (cb >= 1.0f - PC_PSBLEND_EDGE) {
        return 1.0f;
    }
    if (cs <= 0.0f) {
        return 0.0f;
    }
    return 1.0f - pc_rs_minf_k((1.0f - cb) / cs, 1.0f);
}

static float color_dodge(float cb, float cs)
{
    if (cb <= PC_PSBLEND_EDGE) {
        return 0.0f;
    }
    if (cs >= 1.0f) {
        return 1.0f;
    }
    return pc_rs_minf_k(cb / (1.0f - cs), 1.0f);
}

static float vivid_light_generic(float cb, float cs)
{
    if (cs <= 0.5f) {
        if (cb >= 1.0f - PC_PSBLEND_EDGE) {
            return 1.0f;
        }
        if (cs <= 0.0f) {
            return 0.0f;
        }
        return 1.0f - pc_rs_minf_k((1.0f - cb) / (2.0f * cs), 1.0f);
    }
    if (cb <= PC_PSBLEND_EDGE) {
        return 0.0f;
    }
    if (cs >= 1.0f) {
        return 1.0f;
    }
    return pc_rs_minf_k(cb / (2.0f * (1.0f - cs)), 1.0f);
}

static float hard_mix_ps(float cb, float cs)
{
    return vivid_light_generic(cb, cs) >= 0.5f - 1e-6f ? 1.0f : 0.0f;
}

void pc_psblend_rgb(pc_blend_mode mode, const float cb[3], const float cs[3], float out[3])
{
    float b[3] = {cb[0], cb[1], cb[2]};
    float s[3] = {cs[0], cs[1], cs[2]};
    int i;
    switch (mode) {
    case PC_BLEND_VIVID_LIGHT:
        for (i = 0; i < 3; i++) {
            out[i] = vivid_light_ps(b[i], s[i]);
        }
        return;
    case PC_BLEND_HARD_MIX:
        for (i = 0; i < 3; i++) {
            out[i] = hard_mix_ps(b[i], s[i]);
        }
        return;
    case PC_BLEND_COLOR_BURN:
        for (i = 0; i < 3; i++) {
            out[i] = color_burn(b[i], s[i]);
        }
        return;
    case PC_BLEND_COLOR_DODGE:
        for (i = 0; i < 3; i++) {
            out[i] = color_dodge(b[i], s[i]);
        }
        return;
    default:
        break;
    }
    if (hdr_flag) {
        pc_blend_rgb_hdr(mode, b, s, out);
    } else {
        pc_blend_rgb(mode, b, s, out);
    }
}

void pc_psblend_composite(pc_blend_mode mode, const float backdrop[4], const float source[4], float opacity,
                          float out[4])
{
    float ab = backdrop[3];
    float as = source[3] * opacity;
    float ao;
    float cb[3];
    float cs[3];
    float b[3];
    float r[4];
    int i;
    if (as <= 0.0f) {
        memmove(out, backdrop, 4 * sizeof(float));
        return;
    }
    ao = as + ab * (1.0f - as);
    cb[0] = backdrop[0];
    cb[1] = backdrop[1];
    cb[2] = backdrop[2];
    cs[0] = source[0];
    cs[1] = source[1];
    cs[2] = source[2];
    if (mode == PC_BLEND_NORMAL && lab_mix_flag && ab > 0.0f && ao > 0.0f) {
        float lb[3];
        float ls[3];
        float m[3];
        float kb = ab * (1.0f - as) / ao;
        float k = as / ao;
        pc_srgb_to_lab(cb, lb);
        pc_srgb_to_lab(cs, ls);
        for (i = 0; i < 3; i++) {
            m[i] = lb[i] * kb + ls[i] * k;
        }
        pc_lab_to_srgb(m, r);
        out[0] = r[0];
        out[1] = r[1];
        out[2] = r[2];
        out[3] = ao;
        return;
    }
    if (mode == PC_BLEND_NORMAL) {
        /* Fast path: B(Cb, Cs) = Cs. */
        float kb;
        float inv;
        if (ao <= 0.0f) {
            memset(out, 0, 4 * sizeof(float));
            return;
        }
        kb = ab * (1.0f - as);
        inv = 1.0f / ao;
        out[0] = (kb * cb[0] + as * cs[0]) * inv;
        out[1] = (kb * cb[1] + as * cs[1]) * inv;
        out[2] = (kb * cb[2] + as * cs[2]) * inv;
        out[3] = ao;
        return;
    }
    pc_psblend_rgb(mode, cb, cs, b);
    if (ao <= 0.0f) {
        memset(out, 0, 4 * sizeof(float));
        return;
    }
    for (i = 0; i < 3; i++) {
        r[i] = ((1.0f - as) * ab * cb[i] + (1.0f - ab) * as * cs[i] + as * ab * b[i]) / ao;
    }
    out[0] = r[0];
    out[1] = r[1];
    out[2] = r[2];
    out[3] = ao;
}

bool pc_psblend_fill_is_special(pc_blend_mode mode)
{
    switch (mode) {
    case PC_BLEND_COLOR_BURN:
    case PC_BLEND_LINEAR_BURN:
    case PC_BLEND_COLOR_DODGE:
    case PC_BLEND_LINEAR_DODGE:
    case PC_BLEND_VIVID_LIGHT:
    case PC_BLEND_LINEAR_LIGHT:
    case PC_BLEND_HARD_MIX:
    case PC_BLEND_DIFFERENCE:
        return true;
    default:
        return false;
    }
}

void pc_psblend_rgb_fill(pc_blend_mode mode, const float cb[3], const float cs[3], float fill, float out[3])
{
    float f = pc_clampf(fill, 0.0f, 1.0f);
    float neutral;
    float t[3];
    int i;
    if (f >= 1.0f || !pc_psblend_fill_is_special(mode)) {
        pc_psblend_rgb(mode, cb, cs, out);
        return;
    }
    switch (mode) {
    case PC_BLEND_HARD_MIX:
        /* (cb - f (1 - cs)) / (1 - f), clamped: the hard threshold at f = 1, the backdrop at 0. */
        for (i = 0; i < 3; i++) {
            t[i] = pc_clampf((cb[i] - f * (1.0f - cs[i])) / (1.0f - f), 0.0f, 1.0f);
        }
        memcpy(out, t, sizeof t);
        return;
    case PC_BLEND_COLOR_BURN:
    case PC_BLEND_LINEAR_BURN:
        neutral = 1.0f;
        break;
    case PC_BLEND_VIVID_LIGHT:
    case PC_BLEND_LINEAR_LIGHT:
        neutral = 0.5f;
        break;
    default:
        neutral = 0.0f;
        break;
    }
    for (i = 0; i < 3; i++) {
        t[i] = neutral + (cs[i] - neutral) * f;
    }
    pc_psblend_rgb(mode, cb, t, out);
}

void pc_psblend_composite_fill(pc_blend_mode mode, const float backdrop[4], const float source[4],
                               float opacity, float fill, float gamma, float out[4])
{
    float ab;
    float over;
    float alone;
    float ao;
    float cb[3];
    float cs[3];
    float b[3];
    float wb;
    float wbs;
    float ws;
    float r[3];
    int i;
    if (fill >= 1.0f || !pc_psblend_fill_is_special(mode)) {
        pc_psblend_composite_gamma(mode, backdrop, source, opacity * pc_clampf(fill, 0.0f, 1.0f), gamma, out);
        return;
    }
    ab = backdrop[3];
    over = source[3] * opacity;
    alone = over * pc_clampf(fill, 0.0f, 1.0f);
    if (over <= 0.0f) {
        memmove(out, backdrop, 4 * sizeof(float));
        return;
    }
    ao = ab + (1.0f - ab) * alone;
    if (ao <= 0.0f) {
        memset(out, 0, 4 * sizeof(float));
        return;
    }
    for (i = 0; i < 3; i++) {
        cb[i] = backdrop[i];
        cs[i] = source[i];
    }
    pc_psblend_rgb_fill(mode, cb, cs, fill, b);
    wb = ab * (1.0f - over) / ao;
    wbs = ab * over / ao;
    ws = (1.0f - ab) * alone / ao;
    for (i = 0; i < 3; i++) {
        if (gamma == 1.0f) {
            r[i] = wb * cb[i] + wbs * b[i] + ws * cs[i];
        } else {
            r[i] = pc_psblend_text_decode(wb * pc_psblend_text_encode(cb[i], gamma) +
                                              wbs * pc_psblend_text_encode(b[i], gamma) +
                                              ws * pc_psblend_text_encode(cs[i], gamma),
                                          gamma);
        }
    }
    out[0] = r[0];
    out[1] = r[1];
    out[2] = r[2];
    out[3] = ao;
}

/* 1.45f32 */
static atomic_uint_least32_t text_gamma_bits = 0x3fb9999au;

void pc_psblend_set_text_gamma(float gamma)
{
    float g = isfinite(gamma) ? (gamma < 1.0f ? 1.0f : (gamma > 2.2f ? 2.2f : gamma)) : PC_PSBLEND_TEXT_GAMMA;
    uint32_t bits;
    memcpy(&bits, &g, sizeof bits);
    atomic_store_explicit(&text_gamma_bits, bits, memory_order_relaxed);
}

float pc_psblend_text_gamma(void)
{
    uint32_t bits = (uint32_t)atomic_load_explicit(&text_gamma_bits, memory_order_relaxed);
    float g;
    memcpy(&g, &bits, sizeof g);
    return g;
}

float pc_psblend_text_encode(float v, float gamma)
{
    return powf(pc_srgb_to_linear(pc_rs_maxf_k(v, 0.0f)), 1.0f / gamma);
}

float pc_psblend_text_decode(float v, float gamma)
{
    return pc_linear_to_srgb(powf(pc_rs_maxf_k(v, 0.0f), gamma));
}

void pc_psblend_composite_gamma(pc_blend_mode mode, const float backdrop[4], const float source[4],
                                float opacity, float gamma, float out[4])
{
    float ab;
    float as;
    float ao;
    float cb[3];
    float cs[3];
    float b[3];
    float wb;
    float ws;
    float wbs;
    float r[3];
    int i;
    if (gamma == 1.0f) {
        pc_psblend_composite(mode, backdrop, source, opacity, out);
        return;
    }
    ab = backdrop[3];
    as = source[3] * opacity;
    if (as <= 0.0f) {
        memmove(out, backdrop, 4 * sizeof(float));
        return;
    }
    ao = as + ab * (1.0f - as);
    if (ao <= 0.0f) {
        memset(out, 0, 4 * sizeof(float));
        return;
    }
    for (i = 0; i < 3; i++) {
        cb[i] = backdrop[i];
        cs[i] = source[i];
    }
    pc_psblend_rgb(mode, cb, cs, b);
    wb = (1.0f - as) * ab / ao;
    ws = (1.0f - ab) * as / ao;
    wbs = as * ab / ao;
    for (i = 0; i < 3; i++) {
        r[i] = pc_psblend_text_decode(wb * pc_psblend_text_encode(cb[i], gamma) +
                                          ws * pc_psblend_text_encode(cs[i], gamma) +
                                          wbs * pc_psblend_text_encode(b[i], gamma),
                                      gamma);
    }
    out[0] = r[0];
    out[1] = r[1];
    out[2] = r[2];
    out[3] = ao;
}
