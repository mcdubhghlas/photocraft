#include "blend.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "base/num.h"
#include "base/rmath.h"

const pc_blend_mode PC_BLEND_LAYER_MODES[PC_BLEND_LAYER_MODE_COUNT] = {
    PC_BLEND_NORMAL,       PC_BLEND_DISSOLVE,     PC_BLEND_DARKEN,       PC_BLEND_MULTIPLY,
    PC_BLEND_COLOR_BURN,   PC_BLEND_LINEAR_BURN,  PC_BLEND_DARKER_COLOR, PC_BLEND_LIGHTEN,
    PC_BLEND_SCREEN,       PC_BLEND_COLOR_DODGE,  PC_BLEND_LINEAR_DODGE, PC_BLEND_LIGHTER_COLOR,
    PC_BLEND_OVERLAY,      PC_BLEND_SOFT_LIGHT,   PC_BLEND_HARD_LIGHT,   PC_BLEND_VIVID_LIGHT,
    PC_BLEND_LINEAR_LIGHT, PC_BLEND_PIN_LIGHT,    PC_BLEND_HARD_MIX,     PC_BLEND_DIFFERENCE,
    PC_BLEND_EXCLUSION,    PC_BLEND_SUBTRACT,     PC_BLEND_DIVIDE,       PC_BLEND_HUE,
    PC_BLEND_SATURATION,   PC_BLEND_COLOR,        PC_BLEND_LUMINOSITY};

typedef struct mode_info {
    const char *label;
    const char *name;
    char key[5];
} mode_info;

static const mode_info INFO[PC_BLEND_COUNT] = {
    {"Pass Through", "PassThrough", "pass"},
    {"Normal", "Normal", "norm"},
    {"Dissolve", "Dissolve", "diss"},
    {"Darken", "Darken", "dark"},
    {"Multiply", "Multiply", "mul "},
    {"Color Burn", "ColorBurn", "idiv"},
    {"Linear Burn", "LinearBurn", "lbrn"},
    {"Darker Color", "DarkerColor", "dkCl"},
    {"Lighten", "Lighten", "lite"},
    {"Screen", "Screen", "scrn"},
    {"Color Dodge", "ColorDodge", "div "},
    {"Linear Dodge (Add)", "LinearDodge", "lddg"},
    {"Lighter Color", "LighterColor", "lgCl"},
    {"Overlay", "Overlay", "over"},
    {"Soft Light", "SoftLight", "sLit"},
    {"Hard Light", "HardLight", "hLit"},
    {"Vivid Light", "VividLight", "vLit"},
    {"Linear Light", "LinearLight", "lLit"},
    {"Pin Light", "PinLight", "pLit"},
    {"Hard Mix", "HardMix", "hMix"},
    {"Difference", "Difference", "diff"},
    {"Exclusion", "Exclusion", "smud"},
    {"Subtract", "Subtract", "fsub"},
    {"Divide", "Divide", "fdiv"},
    {"Hue", "Hue", "hue "},
    {"Saturation", "Saturation", "sat "},
    {"Color", "Color", "colr"},
    {"Luminosity", "Luminosity", "lum "},
};

static const mode_info *info(pc_blend_mode m)
{
    return &INFO[(unsigned)m < PC_BLEND_COUNT ? (unsigned)m : (unsigned)PC_BLEND_NORMAL];
}

const char *pc_blend_label(pc_blend_mode m)
{
    return info(m)->label;
}

const char *pc_blend_name(pc_blend_mode m)
{
    return info(m)->name;
}

bool pc_blend_from_name(const char *s, size_t len, pc_blend_mode *out)
{
    unsigned i;
    for (i = 0; s != NULL && i < PC_BLEND_COUNT; i++) {
        if (strlen(INFO[i].name) == len && memcmp(INFO[i].name, s, len) == 0) {
            *out = (pc_blend_mode)i;
            return true;
        }
    }
    return false;
}

const char *pc_blend_psd_key(pc_blend_mode m)
{
    return info(m)->key;
}

bool pc_blend_from_psd_key(const char key[4], pc_blend_mode *out)
{
    unsigned i;
    for (i = 0; key != NULL && i < PC_BLEND_COUNT; i++) {
        if (memcmp(INFO[i].key, key, 4) == 0) {
            *out = (pc_blend_mode)i;
            return true;
        }
    }
    return false;
}

bool pc_blend_is_separable(pc_blend_mode m)
{
    switch (m) {
    case PC_BLEND_HUE:
    case PC_BLEND_SATURATION:
    case PC_BLEND_COLOR:
    case PC_BLEND_LUMINOSITY:
    case PC_BLEND_DARKER_COLOR:
    case PC_BLEND_LIGHTER_COLOR:
        return false;
    default:
        return true;
    }
}

static float vivid_light_hard_mix(float cb, float cs)
{
    const float edge = 1e-4f;
    if (cs <= 0.5f) {
        if (cb >= 1.0f - edge) {
            return 1.0f;
        }
        if (cs <= 0.0f) {
            return 0.0f;
        }
        return 1.0f - pc_rs_minf_k((1.0f - cb) / (2.0f * cs), 1.0f);
    }
    if (cb <= edge) {
        return 0.0f;
    }
    if (cs >= 1.0f) {
        return 1.0f;
    }
    return pc_rs_minf_k(cb / (2.0f * (1.0f - cs)), 1.0f);
}

static float color_burn(float cb, float cs)
{
    if (cb >= 1.0f) {
        return 1.0f;
    }
    if (cs <= 0.0f) {
        return 0.0f;
    }
    return 1.0f - pc_rs_minf_k((1.0f - cb) / cs, 1.0f);
}

static float color_dodge(float cb, float cs)
{
    if (cb <= 0.0f) {
        return 0.0f;
    }
    if (cs >= 1.0f) {
        return 1.0f;
    }
    return pc_rs_minf_k(cb / (1.0f - cs), 1.0f);
}

static float hard_light(float cb, float cs)
{
    float s;
    if (cs <= 0.5f) {
        return cb * 2.0f * cs;
    }
    s = 2.0f * cs - 1.0f;
    return cb + s - cb * s;
}

/* Photoshop's Soft Light (differs from W3C/PDF in the upper half). */
static float soft_light_ps(float cb, float cs)
{
    if (cs <= 0.5f) {
        return 2.0f * cb * cs + cb * cb * (1.0f - 2.0f * cs);
    }
    return 2.0f * cb * (1.0f - cs) + sqrtf(pc_rs_maxf_k(cb, 0.0f)) * (2.0f * cs - 1.0f);
}

/* B(cb, cs) with Linear Dodge and Divide clipped at `hi` (1 at integer depths). */
static float channel(pc_blend_mode m, float cb, float cs, float hi)
{
    switch (m) {
    case PC_BLEND_NORMAL:
    case PC_BLEND_DISSOLVE:
    case PC_BLEND_PASS_THROUGH:
        return cs;
    case PC_BLEND_DARKEN:
        return pc_rs_minf(cb, cs);
    case PC_BLEND_MULTIPLY:
        return cb * cs;
    case PC_BLEND_COLOR_BURN:
        return color_burn(cb, cs);
    case PC_BLEND_LINEAR_BURN:
        return pc_rs_maxf_k(cb + cs - 1.0f, 0.0f);
    case PC_BLEND_LIGHTEN:
        return pc_rs_maxf(cb, cs);
    case PC_BLEND_SCREEN:
        return cb + cs - cb * cs;
    case PC_BLEND_COLOR_DODGE:
        return color_dodge(cb, cs);
    case PC_BLEND_LINEAR_DODGE:
        return pc_rs_minf_k(cb + cs, hi);
    case PC_BLEND_OVERLAY:
        return hard_light(cs, cb);
    case PC_BLEND_SOFT_LIGHT:
        return soft_light_ps(cb, cs);
    case PC_BLEND_HARD_LIGHT:
        return hard_light(cb, cs);
    case PC_BLEND_VIVID_LIGHT:
        if (cs <= 0.0f) {
            return 0.0f;
        }
        if (cs >= 1.0f) {
            return 1.0f;
        }
        return cs <= 0.5f ? color_burn(cb, 2.0f * cs) : color_dodge(cb, 2.0f * (cs - 0.5f));
    case PC_BLEND_LINEAR_LIGHT:
        return pc_clampf(cb + 2.0f * cs - 1.0f, 0.0f, 1.0f);
    case PC_BLEND_PIN_LIGHT:
        return cs <= 0.5f ? pc_rs_minf(cb, 2.0f * cs) : pc_rs_maxf(cb, 2.0f * cs - 1.0f);
    case PC_BLEND_HARD_MIX:
        return vivid_light_hard_mix(cb, cs) >= 0.5f - 1e-6f ? 1.0f : 0.0f;
    case PC_BLEND_DIFFERENCE:
        return fabsf(cb - cs);
    case PC_BLEND_EXCLUSION:
        return cb + cs - 2.0f * cb * cs;
    case PC_BLEND_SUBTRACT:
        return pc_rs_maxf_k(cb - cs, 0.0f);
    case PC_BLEND_DIVIDE:
        if (cs <= 0.0f) {
            return cb <= 0.0f ? 0.0f : 1.0f;
        }
        return pc_rs_minf_k(cb / cs, hi);
    case PC_BLEND_DARKER_COLOR:
    case PC_BLEND_LIGHTER_COLOR:
    case PC_BLEND_HUE:
    case PC_BLEND_SATURATION:
    case PC_BLEND_COLOR:
    case PC_BLEND_LUMINOSITY:
        return cs; /* non-separable: handled by pc_blend_rgb */
    }
    return cs;
}

float pc_blend_channel(pc_blend_mode m, float cb, float cs)
{
    return channel(m, cb, cs, 1.0f);
}

float pc_blend_channel_hdr(pc_blend_mode m, float cb, float cs)
{
    return channel(m, cb, cs, FLT_MAX);
}

/* ---- non-separable helpers (W3C Compositing Level 1) ---- */

float pc_blend_lum(const float c[3])
{
    return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2];
}

static void clip_color(const float c[3], float out[3])
{
    float l = pc_blend_lum(c);
    float n = pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
    float x = pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]);
    float o[3];
    int i;
    o[0] = c[0];
    o[1] = c[1];
    o[2] = c[2];
    if (n < 0.0f) {
        float d = l - n;
        for (i = 0; i < 3; i++) {
            o[i] = fabsf(d) < 1e-12f ? l : l + (o[i] - l) * l / d;
        }
    }
    if (x > 1.0f) {
        float d = x - l;
        for (i = 0; i < 3; i++) {
            o[i] = fabsf(d) < 1e-12f ? l : l + (o[i] - l) * (1.0f - l) / d;
        }
    }
    memcpy(out, o, sizeof o);
}

void pc_blend_set_lum(const float c[3], float l, float out[3])
{
    float d = l - pc_blend_lum(c);
    float t[3];
    t[0] = c[0] + d;
    t[1] = c[1] + d;
    t[2] = c[2] + d;
    clip_color(t, out);
}

static float sat(const float c[3])
{
    return pc_rs_maxf(pc_rs_maxf(c[0], c[1]), c[2]) - pc_rs_minf(pc_rs_minf(c[0], c[1]), c[2]);
}

static void set_sat(const float c[3], float s, float out[3])
{
    /* Rust's stable sort_by on three elements is an insertion sort; NaN compares Equal. */
    size_t idx[3] = {0, 1, 2};
    size_t i;
    float o[3] = {0.0f, 0.0f, 0.0f};
    size_t imin;
    size_t imid;
    size_t imax;
    for (i = 1; i < 3; i++) {
        size_t t = idx[i];
        size_t j = i;
        while (j > 0 && c[t] < c[idx[j - 1]]) {
            idx[j] = idx[j - 1];
            j--;
        }
        idx[j] = t;
    }
    imin = idx[0];
    imid = idx[1];
    imax = idx[2];
    if (c[imax] > c[imin]) {
        o[imid] = (c[imid] - c[imin]) * s / (c[imax] - c[imin]);
        o[imax] = s;
    }
    o[imin] = 0.0f;
    memcpy(out, o, sizeof o);
}

void pc_blend_rgb(pc_blend_mode m, const float cb[3], const float cs[3], float out[3])
{
    float t[3];
    switch (m) {
    case PC_BLEND_HUE:
        set_sat(cs, sat(cb), t);
        pc_blend_set_lum(t, pc_blend_lum(cb), out);
        return;
    case PC_BLEND_SATURATION:
        set_sat(cb, sat(cs), t);
        pc_blend_set_lum(t, pc_blend_lum(cb), out);
        return;
    case PC_BLEND_COLOR:
        pc_blend_set_lum(cs, pc_blend_lum(cb), out);
        return;
    case PC_BLEND_LUMINOSITY:
        pc_blend_set_lum(cb, pc_blend_lum(cs), out);
        return;
    case PC_BLEND_DARKER_COLOR:
        memcpy(t, pc_blend_lum(cs) < pc_blend_lum(cb) ? cs : cb, sizeof t);
        memcpy(out, t, sizeof t);
        return;
    case PC_BLEND_LIGHTER_COLOR:
        memcpy(t, pc_blend_lum(cs) > pc_blend_lum(cb) ? cs : cb, sizeof t);
        memcpy(out, t, sizeof t);
        return;
    default:
        t[0] = pc_blend_channel(m, cb[0], cs[0]);
        t[1] = pc_blend_channel(m, cb[1], cs[1]);
        t[2] = pc_blend_channel(m, cb[2], cs[2]);
        memcpy(out, t, sizeof t);
        return;
    }
}

void pc_blend_rgb_hdr(pc_blend_mode m, const float cb[3], const float cs[3], float out[3])
{
    float t[3];
    if (!pc_blend_is_separable(m)) {
        pc_blend_rgb(m, cb, cs, out);
        return;
    }
    t[0] = pc_blend_channel_hdr(m, cb[0], cs[0]);
    t[1] = pc_blend_channel_hdr(m, cb[1], cs[1]);
    t[2] = pc_blend_channel_hdr(m, cb[2], cs[2]);
    memcpy(out, t, sizeof t);
}

void pc_blend_composite(pc_blend_mode m, const float backdrop[4], const float source[4], float opacity,
                        float out[4])
{
    float ab = backdrop[3];
    float as = source[3] * opacity;
    float cb[3];
    float cs[3];
    float b[3];
    float ao;
    float o[4];
    int i;
    if (as <= 0.0f) {
        memmove(out, backdrop, 4 * sizeof *out);
        return;
    }
    memcpy(cb, backdrop, sizeof cb);
    memcpy(cs, source, sizeof cs);
    pc_blend_rgb(m, cb, cs, b);
    ao = as + ab * (1.0f - as);
    if (ao <= 0.0f) {
        memset(out, 0, 4 * sizeof *out);
        return;
    }
    for (i = 0; i < 3; i++) {
        float premul = (1.0f - as) * ab * cb[i] + (1.0f - ab) * as * cs[i] + as * ab * b[i];
        o[i] = premul / ao;
    }
    o[3] = ao;
    memcpy(out, o, sizeof o);
}
