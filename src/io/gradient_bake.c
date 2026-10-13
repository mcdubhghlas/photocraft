/* Photoshop gradient appearance -> dense linear stops (port of crates/io/src/gradient_bake.rs).
 *
 * Photoshop interpolates gradient colours with a Smoothness setting (0-100 %) and, since the 2023
 * releases, an interpolation method (Perceptual / Linear / Classic). The compositors interpolate
 * stops linearly in sRGB, so on import Photoshop's curve is sampled into dense stops. The model
 * was fitted to Photoshop's own composites (clean-room, from pixels): Smoothness blends linear
 * interpolation with a Catmull-Rom spline through the stop colours (end tangents at half the
 * chord slope); Perceptual interpolates in Oklab, Linear in linear light, Classic in sRGB. Stop
 * midpoints remap each segment piecewise-linearly. All arithmetic is f32 like the reference. */
#include "io/gradient_bake.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "io/core.h"

#define SAMPLES 96

pc_io_bake_method pc_io_bake_method_from_code(const uint8_t *code, size_t len)
{
    if (code != NULL && len == 4 && memcmp(code, "Perc", 4) == 0) {
        return PC_IO_BAKE_PERCEPTUAL;
    }
    if (code != NULL && len == 4 && memcmp(code, "Lnr ", 4) == 0) {
        return PC_IO_BAKE_LINEAR;
    }
    if (code != NULL && len == 4 && memcmp(code, "Smoo", 4) == 0) {
        return PC_IO_BAKE_SMOOTH;
    }
    return PC_IO_BAKE_CLASSIC;
}

static float lin(float c)
{
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float enc(float c)
{
    c = pc_clampf(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? 12.92f * c : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

static void to_oklab(const float rgb[3], float out[3])
{
    float r = lin(rgb[0]);
    float g = lin(rgb[1]);
    float b = lin(rgb[2]);
    float l = pc_cbrtf(0.41222146f * r + 0.53633255f * g + 0.051445995f * b);
    float m = pc_cbrtf(0.2119035f * r + 0.6806995f * g + 0.10739696f * b);
    float s = pc_cbrtf(0.08830246f * r + 0.28171885f * g + 0.6299787f * b);
    out[0] = 0.21045426f * l + 0.7936178f * m - 0.004072047f * s;
    out[1] = 1.9779985f * l - 2.4285922f * m + 0.4505937f * s;
    out[2] = 0.025904037f * l + 0.78277177f * m - 0.80867577f * s;
}

static float cube(float x)
{
    return x * x * x;
}

static void from_oklab(const float p[3], float out[3])
{
    float l = cube(p[0] + 0.39633778f * p[1] + 0.21580376f * p[2]);
    float m = cube(p[0] - 0.105561346f * p[1] - 0.06385417f * p[2]);
    float s = cube(p[0] - 0.08948418f * p[1] - 1.2914855f * p[2]);
    out[0] = enc(4.0767417f * l - 3.3077116f * m + 0.23096994f * s);
    out[1] = enc(-1.268438f * l + 2.6097574f * m - 0.34131938f * s);
    out[2] = enc(-0.0041960863f * l - 0.7034186f * m + 1.7076147f * s);
}

static void into_space(pc_io_bake_method m, const float rgb[3], float out[3])
{
    switch (m) {
    case PC_IO_BAKE_CLASSIC:
        memcpy(out, rgb, 3 * sizeof *out);
        break;
    case PC_IO_BAKE_LINEAR:
        for (size_t i = 0; i < 3; i++) {
            out[i] = lin(rgb[i]);
        }
        break;
    default: /* PC_IO_BAKE_PERCEPTUAL */
        to_oklab(rgb, out);
        break;
    }
}

static void out_of_space(pc_io_bake_method m, const float p[3], float out[3])
{
    switch (m) {
    case PC_IO_BAKE_CLASSIC:
        for (size_t i = 0; i < 3; i++) {
            out[i] = pc_clampf(p[i], 0.0f, 1.0f);
        }
        break;
    case PC_IO_BAKE_LINEAR:
        for (size_t i = 0; i < 3; i++) {
            out[i] = enc(p[i]);
        }
        break;
    default: /* PC_IO_BAKE_PERCEPTUAL */
        from_oklab(p, out);
        break;
    }
}

typedef struct pt {
    float t;
    float c[3];
} pt;

/* Catmull-Rom tangent per unit t at stop k; the ends use half the chord slope. */
static void tangent(const pt *s, size_t n, size_t k, float out[3])
{
    size_t a = k == 0 ? 0 : (k == n - 1 ? n - 2 : k - 1);
    size_t b = k == 0 ? 1 : (k == n - 1 ? n - 1 : k + 1);
    float d = pc_rs_maxf_k(s[b].t - s[a].t, 1e-6f);
    float half = k == 0 || k == n - 1 ? 0.5f : 1.0f;
    for (size_t c = 0; c < 3; c++) {
        out[c] = (s[b].c[c] - s[a].c[c]) / d * half;
    }
}

/* Smooth's progress across a segment at u = 0, 1/16, ..., 1 (Photoshop 27.11, measured as the
 * position along the Oklab line between the two stop colours). */
static const float SMOOTH_EASE[17] = {0.0f,    0.0438f, 0.099f,  0.1644f, 0.2303f, 0.2988f,
                                      0.3667f, 0.433f,  0.4974f, 0.5642f, 0.6302f, 0.6985f,
                                      0.7656f, 0.8333f, 0.8945f, 0.9517f, 1.0f};

static float smooth_ease(float u)
{
    float x = pc_clampf(u, 0.0f, 1.0f) * 16.0f;
    size_t i = (size_t)floorf(x);
    float a;
    float b;
    if (i > 15) {
        i = 15;
    }
    a = SMOOTH_EASE[i];
    b = SMOOTH_EASE[i + 1];
    return a + (b - a) * (x - (float)i);
}

/* Colour at t for sorted stops (positions in 0..1) in the method's space. */
static void eval(const pt *s, size_t n, const float *mids, size_t nmid, float smooth, float t,
                 pc_io_bake_method method, float out[3])
{
    size_t i = 0;
    float t0;
    float t1;
    float h;
    float u;
    float mid;
    float m0[3];
    float m1[3];
    float u2;
    float u3;
    float h00;
    float h10;
    float h01;
    float h11;
    if (t <= s[0].t) {
        memcpy(out, s[0].c, sizeof s[0].c);
        return;
    }
    if (t >= s[n - 1].t) {
        memcpy(out, s[n - 1].c, sizeof s[0].c);
        return;
    }
    for (size_t k = 0; k + 1 < n; k++) {
        if (t >= s[k].t && t <= s[k + 1].t) {
            i = k;
            break;
        }
    }
    t0 = s[i].t;
    t1 = s[i + 1].t;
    h = pc_rs_maxf_k(t1 - t0, 1e-6f);
    u = (t - t0) / h;
    /* Midpoint: where the segment reaches 50 % (default 0.5 = identity). */
    mid = pc_clampf(i < nmid ? mids[i] : 0.5f, 0.05f, 0.95f);
    u = u <= mid ? 0.5f * u / mid : 0.5f + 0.5f * (u - mid) / (1.0f - mid);
    if (method == PC_IO_BAKE_SMOOTH) {
        float e = smooth_ease(u);
        for (size_t c = 0; c < 3; c++) {
            float p0 = s[i].c[c];
            float p1 = s[i + 1].c[c];
            out[c] = p0 + (p1 - p0) * e;
        }
        return;
    }
    tangent(s, n, i, m0);
    tangent(s, n, i + 1, m1);
    u2 = u * u;
    u3 = u * u * u;
    h00 = 2.0f * u3 - 3.0f * u2 + 1.0f;
    h10 = u3 - 2.0f * u2 + u;
    h01 = -2.0f * u3 + 3.0f * u2;
    h11 = u3 - u2;
    for (size_t c = 0; c < 3; c++) {
        float p0 = s[i].c[c];
        float p1 = s[i + 1].c[c];
        float linear = p0 + (p1 - p0) * u;
        float spline = h00 * p0 + h10 * h * m0[c] + h01 * p1 + h11 * h * m1[c];
        float value = linear + (spline - linear) * smooth;
        /* Catmull-Rom tangents can overshoot neighbouring stops, turning even a flat segment
         * into a dip. Smoothness should bend the ramp without inventing colours outside it. */
        float lo = pc_rs_minf(p0, p1);
        float hi = pc_rs_maxf(p0, p1);
        out[c] = (isfinite(lo) && isfinite(hi)) ? pc_clampf(value, lo, hi) : value;
    }
}

/* f32::total_cmp as a signed key. */
static int32_t total_key(float v)
{
    uint32_t u;
    int32_t k;
    memcpy(&u, &v, sizeof u);
    k = pc_u32_as_i32(u);
    return k ^ (int32_t)(((uint32_t)(k >> 31)) >> 1);
}

static int total_cmp(float a, float b)
{
    int32_t x = total_key(a);
    int32_t y = total_key(b);
    return (x > y) - (x < y);
}

/* Stable insertion sort by position (sort_by(total_cmp)); gradients have few stops. */
static void sort_stops(pc_io_color_stop *v, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        pc_io_color_stop x = v[i];
        size_t j = i;
        while (j > 0 && total_cmp(v[j - 1].at, x.at) > 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

/* Stop colours carry no alpha in Photoshop (opacity has its own stops); keep the nearest. */
static float nearest_alpha(const pc_io_color_stop *s, size_t n, float t)
{
    size_t best = 0;
    for (size_t i = 1; i < n; i++) {
        if (total_cmp(fabsf(s[i].at - t), fabsf(s[best].at - t)) < 0) {
            best = i;
        }
    }
    return n > 0 ? s[best].color.alpha : 1.0f;
}

static pc_status copy_out(const pc_io_color_stop *stops, size_t n, pc_io_color_stop **out, size_t *nout,
                          pc_error *err)
{
    pc_io_color_stop *v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    if (v == NULL) {
        return pc_io_nomem(err, "baking a gradient");
    }
    if (n > 0) {
        memcpy(v, stops, n * sizeof *v);
    }
    *out = v;
    *nout = n;
    return PC_OK;
}

pc_status pc_io_gradient_bake(const pc_io_color_stop *stops, size_t n, const float *midpoints, size_t nmid,
                              float smoothness, pc_io_bake_method method, pc_io_color_stop **out,
                              size_t *nout, pc_error *err)
{
    bool plain_mids = true;
    bool lab = method == PC_IO_BAKE_CLASSIC;
    pc_io_color_stop *s;
    pt *pts;
    pc_io_color_stop *v;
    size_t k = 0;
    float smooth;
    for (size_t i = 0; i < nmid; i++) {
        if (!(fabsf(midpoints[i] - 0.5f) < 1e-3f)) {
            plain_mids = false;
        }
    }
    /* Classic interpolates in the stops' own model: Lab stops (Lab documents) blend in L*a*b*,
     * which the compositors' sRGB interpolation can't reproduce (psd-tools 4x4_16bit_lab). */
    for (size_t i = 0; i < n && lab; i++) {
        lab = stops[i].color.mode == PC_COLOR_MODE_LAB;
    }
    if (n < 2 || (smoothness <= 0.0f && method == PC_IO_BAKE_CLASSIC && plain_mids && !lab)) {
        return copy_out(stops, n, out, nout, err);
    }
    s = pc_malloc_array(n, sizeof *s);
    pts = pc_malloc_array(n, sizeof *pts);
    v = pc_malloc_array(SAMPLES + 1, sizeof *v);
    if (s == NULL || pts == NULL || v == NULL) {
        free(s);
        free(pts);
        free(v);
        return pc_io_nomem(err, "baking a gradient");
    }
    memcpy(s, stops, n * sizeof *s);
    sort_stops(s, n);
    for (size_t i = 0; i < n; i++) {
        pts[i].t = s[i].at;
        if (lab) {
            memcpy(pts[i].c, s[i].color.c, sizeof pts[i].c);
        } else {
            float rgb[3];
            pc_color_to_rgb(&s[i].color, rgb);
            into_space(method, rgb, pts[i].c);
        }
    }
    smooth = pc_clampf(smoothness, 0.0f, 1.0f);
    for (int32_t i = 0; i <= SAMPLES; i++) {
        float t = (float)i / (float)SAMPLES;
        float c[3];
        pc_io_color_stop stop;
        eval(pts, n, midpoints, nmid, smooth, t, method, c);
        stop.at = t;
        if (lab) {
            stop.color.mode = PC_COLOR_MODE_LAB;
            for (size_t j = 0; j < 3; j++) {
                stop.color.c[j] = pc_clampf(c[j], 0.0f, 1.0f);
            }
            stop.color.c[3] = 0.0f;
            stop.color.alpha = nearest_alpha(s, n, t);
        } else {
            float rgb[3];
            out_of_space(method, c, rgb);
            stop.color = pc_color_rgba(rgb[0], rgb[1], rgb[2], nearest_alpha(s, n, t));
        }
        /* dedup_by: drop a sample within 1e-6 of the one kept before it. */
        if (k > 0 && fabsf(stop.at - v[k - 1].at) < 1e-6f) {
            continue;
        }
        v[k++] = stop;
    }
    free(s);
    free(pts);
    *out = v;
    *nout = k;
    return PC_OK;
}
