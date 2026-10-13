/* Camera Raw settings: defaults, copies, serde JSON, curve validation, the point-curve LUT and
 * the Color Mixer band weights (port of the settings half of crates/algo/src/camera_raw.rs; the
 * develop pipeline is in camera_raw_develop.c). */
#include "algo/camera_raw.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/l9util.h"
#include "algo/photo_util.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

const float PC_ALGO_CR_HSL_BANDS[8] = {0.0f, 30.0f, 60.0f, 120.0f, 180.0f, 240.0f, 270.0f, 300.0f};

static const char DEFAULT_STYLE[] = "highlightPriority";

void pc_algo_camera_raw_default(pc_algo_camera_raw *p)
{
    memset(p, 0, sizeof *p);
    p->curve_splits[0] = 25.0f;
    p->curve_splits[1] = 50.0f;
    p->curve_splits[2] = 75.0f;
    p->grade_blending = 50.0f;
    p->sharpen_radius = 1.0f;
    p->sharpen_detail = 25.0f;
    p->noise_luminance_detail = 50.0f;
    p->noise_color_detail = 50.0f;
    p->grain_size = 25.0f;
    p->grain_roughness = 50.0f;
    p->vignette_midpoint = 50.0f;
    p->vignette_feather = 50.0f;
    p->vignette_style = NULL;
    p->pixel_scale = 1.0f;
}

static void points_fini(pc_algo_points *c)
{
    free(c->v);
    c->v = NULL;
    c->n = 0;
}

void pc_algo_camera_raw_fini(pc_algo_camera_raw *p)
{
    if (p == NULL) {
        return;
    }
    points_fini(&p->point_curve);
    points_fini(&p->point_curve_red);
    points_fini(&p->point_curve_green);
    points_fini(&p->point_curve_blue);
    free(p->vignette_style);
    pc_algo_camera_raw_default(p);
}

const char *pc_algo_camera_raw_style(const pc_algo_camera_raw *p)
{
    return p->vignette_style != NULL ? p->vignette_style : DEFAULT_STYLE;
}

static pc_status points_dup(pc_algo_points *dst, const pc_algo_points *src, pc_error *err)
{
    dst->v = NULL;
    dst->n = 0;
    if (src->n == 0) {
        return PC_OK;
    }
    dst->v = pc_malloc_array(src->n, sizeof *src->v);
    if (dst->v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory copying a curve");
    }
    memcpy(dst->v, src->v, src->n * sizeof *src->v);
    dst->n = src->n;
    return PC_OK;
}

pc_status pc_algo_camera_raw_copy(pc_algo_camera_raw *dst, const pc_algo_camera_raw *src, pc_error *err)
{
    pc_algo_camera_raw t = *src;
    pc_status st;
    t.point_curve = t.point_curve_red = t.point_curve_green = t.point_curve_blue = (pc_algo_points){NULL, 0};
    t.vignette_style = NULL;
    if ((st = points_dup(&t.point_curve, &src->point_curve, err)) != PC_OK ||
        (st = points_dup(&t.point_curve_red, &src->point_curve_red, err)) != PC_OK ||
        (st = points_dup(&t.point_curve_green, &src->point_curve_green, err)) != PC_OK ||
        (st = points_dup(&t.point_curve_blue, &src->point_curve_blue, err)) != PC_OK) {
        pc_algo_camera_raw_fini(&t);
        return st;
    }
    if (src->vignette_style != NULL) {
        t.vignette_style = pc_strndup(src->vignette_style, strlen(src->vignette_style));
        if (t.vignette_style == NULL) {
            pc_algo_camera_raw_fini(&t);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying settings");
        }
    }
    *dst = t;
    return PC_OK;
}

static bool points_eq(const pc_algo_points *a, const pc_algo_points *b)
{
    size_t i;
    if (a->n != b->n) {
        return false;
    }
    for (i = 0; i < a->n; i++) {
        if (a->v[i][0] != b->v[i][0] || a->v[i][1] != b->v[i][1]) {
            return false;
        }
    }
    return true;
}

static bool wheel_eq(pc_algo_cr_wheel a, pc_algo_cr_wheel b)
{
    return a.hue == b.hue && a.sat == b.sat && a.lum == b.lum;
}

static bool floats_eq(const float *a, const float *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool pc_algo_camera_raw_eq(const pc_algo_camera_raw *a, const pc_algo_camera_raw *b)
{
    const float sa[] = {a->temperature,
                        a->tint,
                        a->exposure,
                        a->contrast,
                        a->highlights,
                        a->shadows,
                        a->whites,
                        a->blacks,
                        a->texture,
                        a->clarity,
                        a->dehaze,
                        a->vibrance,
                        a->saturation,
                        a->curve_highlights,
                        a->curve_lights,
                        a->curve_darks,
                        a->curve_shadows,
                        a->grade_blending,
                        a->grade_balance,
                        a->sharpen_amount,
                        a->sharpen_radius,
                        a->sharpen_detail,
                        a->sharpen_masking,
                        a->noise_luminance,
                        a->noise_luminance_detail,
                        a->noise_color,
                        a->noise_color_detail,
                        a->grain_amount,
                        a->grain_size,
                        a->grain_roughness,
                        a->vignette_amount,
                        a->vignette_midpoint,
                        a->vignette_roundness,
                        a->vignette_feather,
                        a->vignette_highlights,
                        a->pixel_scale,
                        a->encoding_gamma};
    const float sb[] = {b->temperature,
                        b->tint,
                        b->exposure,
                        b->contrast,
                        b->highlights,
                        b->shadows,
                        b->whites,
                        b->blacks,
                        b->texture,
                        b->clarity,
                        b->dehaze,
                        b->vibrance,
                        b->saturation,
                        b->curve_highlights,
                        b->curve_lights,
                        b->curve_darks,
                        b->curve_shadows,
                        b->grade_blending,
                        b->grade_balance,
                        b->sharpen_amount,
                        b->sharpen_radius,
                        b->sharpen_detail,
                        b->sharpen_masking,
                        b->noise_luminance,
                        b->noise_luminance_detail,
                        b->noise_color,
                        b->noise_color_detail,
                        b->grain_amount,
                        b->grain_size,
                        b->grain_roughness,
                        b->vignette_amount,
                        b->vignette_midpoint,
                        b->vignette_roundness,
                        b->vignette_feather,
                        b->vignette_highlights,
                        b->pixel_scale,
                        b->encoding_gamma};
    return floats_eq(sa, sb, sizeof sa / sizeof sa[0]) && floats_eq(a->curve_splits, b->curve_splits, 3) &&
           points_eq(&a->point_curve, &b->point_curve) &&
           points_eq(&a->point_curve_red, &b->point_curve_red) &&
           points_eq(&a->point_curve_green, &b->point_curve_green) &&
           points_eq(&a->point_curve_blue, &b->point_curve_blue) && floats_eq(a->hsl_hue, b->hsl_hue, 8) &&
           floats_eq(a->hsl_sat, b->hsl_sat, 8) && floats_eq(a->hsl_lum, b->hsl_lum, 8) &&
           wheel_eq(a->grade_shadows, b->grade_shadows) && wheel_eq(a->grade_midtones, b->grade_midtones) &&
           wheel_eq(a->grade_highlights, b->grade_highlights) && wheel_eq(a->grade_global, b->grade_global) &&
           strcmp(pc_algo_camera_raw_style(a), pc_algo_camera_raw_style(b)) == 0 && a->seed == b->seed;
}

/* ---- JSON ---- */

static const pc_algo_jfield WHEEL_F[] = {
    PC_ALGO_JFD("hue", pc_algo_cr_wheel, hue, PC_ALGO_JF_F32),
    PC_ALGO_JFD("sat", pc_algo_cr_wheel, sat, PC_ALGO_JF_F32),
    PC_ALGO_JFD("lum", pc_algo_cr_wheel, lum, PC_ALGO_JF_F32),
};

static pc_status wheel_read(yyjson_val *v, void *slot, pc_error *err)
{
    pc_algo_cr_wheel w = {0.0f, 0.0f, 0.0f};
    pc_status st = pc_algo_jfields_read(WHEEL_F, 3, v, &w, "struct Wheel", NULL, err);
    if (st == PC_OK) {
        *(pc_algo_cr_wheel *)slot = w;
    }
    return st;
}

static yyjson_mut_val *wheel_write(yyjson_mut_doc *d, const void *slot)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (o == NULL || !pc_algo_jfields_write(WHEEL_F, 3, d, o, slot)) {
        return NULL;
    }
    return o;
}

static pc_status style_read(yyjson_val *v, void *slot, pc_error *err)
{
    char *s;
    if (!yyjson_is_str(v)) {
        return pc_algo_jfail_type(v, "a string", err);
    }
    s = pc_strndup(yyjson_get_str(v), yyjson_get_len(v));
    if (s == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading settings");
    }
    free(*(char **)slot);
    *(char **)slot = s;
    return PC_OK;
}

static yyjson_mut_val *style_write(yyjson_mut_doc *d, const void *slot)
{
    const char *s = *(char *const *)slot;
    return yyjson_mut_strcpy(d, s != NULL ? s : DEFAULT_STYLE);
}

static void style_fini(void *slot)
{
    free(*(char **)slot);
    *(char **)slot = NULL;
}

static pc_status style_copy(void *dst, const void *src, pc_error *err)
{
    const char *s = *(char *const *)src;
    *(char **)dst = NULL;
    if (s != NULL) {
        *(char **)dst = pc_strndup(s, strlen(s));
        if (*(char **)dst == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying settings");
        }
    }
    return PC_OK;
}

#define CR pc_algo_camera_raw
#define F(nm, m) PC_ALGO_JFD(nm, CR, m, PC_ALGO_JF_F32)
#define WHEEL(nm, m)                                                                                         \
    {(nm), PC_ALGO_JF_CUSTOM, offsetof(CR, m), true, NULL, 0, wheel_read, wheel_write, NULL, NULL}

static const pc_algo_jfield CR_F[] = {
    F("temperature", temperature),
    F("tint", tint),
    F("exposure", exposure),
    F("contrast", contrast),
    F("highlights", highlights),
    F("shadows", shadows),
    F("whites", whites),
    F("blacks", blacks),
    F("texture", texture),
    F("clarity", clarity),
    F("dehaze", dehaze),
    F("vibrance", vibrance),
    F("saturation", saturation),
    F("curveHighlights", curve_highlights),
    F("curveLights", curve_lights),
    F("curveDarks", curve_darks),
    F("curveShadows", curve_shadows),
    PC_ALGO_JFA("curveSplits", CR, curve_splits, 3, true),
    PC_ALGO_JFD("pointCurve", CR, point_curve, PC_ALGO_JF_POINTS),
    PC_ALGO_JFD("pointCurveRed", CR, point_curve_red, PC_ALGO_JF_POINTS),
    PC_ALGO_JFD("pointCurveGreen", CR, point_curve_green, PC_ALGO_JF_POINTS),
    PC_ALGO_JFD("pointCurveBlue", CR, point_curve_blue, PC_ALGO_JF_POINTS),
    PC_ALGO_JFA("hslHue", CR, hsl_hue, 8, true),
    PC_ALGO_JFA("hslSat", CR, hsl_sat, 8, true),
    PC_ALGO_JFA("hslLum", CR, hsl_lum, 8, true),
    WHEEL("gradeShadows", grade_shadows),
    WHEEL("gradeMidtones", grade_midtones),
    WHEEL("gradeHighlights", grade_highlights),
    WHEEL("gradeGlobal", grade_global),
    F("gradeBlending", grade_blending),
    F("gradeBalance", grade_balance),
    F("sharpenAmount", sharpen_amount),
    F("sharpenRadius", sharpen_radius),
    F("sharpenDetail", sharpen_detail),
    F("sharpenMasking", sharpen_masking),
    F("noiseLuminance", noise_luminance),
    F("noiseLuminanceDetail", noise_luminance_detail),
    F("noiseColor", noise_color),
    F("noiseColorDetail", noise_color_detail),
    F("grainAmount", grain_amount),
    F("grainSize", grain_size),
    F("grainRoughness", grain_roughness),
    F("vignetteAmount", vignette_amount),
    F("vignetteMidpoint", vignette_midpoint),
    F("vignetteRoundness", vignette_roundness),
    F("vignetteFeather", vignette_feather),
    F("vignetteHighlights", vignette_highlights),
    {"vignetteStyle", PC_ALGO_JF_CUSTOM, offsetof(CR, vignette_style), true, NULL, 0, style_read, style_write,
     style_fini, style_copy},
    PC_ALGO_JFD("seed", CR, seed, PC_ALGO_JF_U32),
    F("pixelScale", pixel_scale),
    F("encodingGamma", encoding_gamma),
};
#undef F
#undef WHEEL
#undef CR

pc_status pc_algo_camera_raw_from_json(yyjson_val *v, pc_algo_camera_raw *out, pc_error *err)
{
    pc_algo_camera_raw p;
    pc_status st;
    pc_algo_camera_raw_default(&p);
    st = pc_algo_jfields_read(CR_F, sizeof CR_F / sizeof CR_F[0], v, &p, "struct CameraRaw", NULL, err);
    if (st != PC_OK) {
        pc_algo_camera_raw_fini(&p);
        return st;
    }
    *out = p;
    return PC_OK;
}

yyjson_mut_val *pc_algo_camera_raw_to_json(yyjson_mut_doc *d, const pc_algo_camera_raw *p)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (o == NULL || !pc_algo_jfields_write(CR_F, sizeof CR_F / sizeof CR_F[0], d, o, p)) {
        return NULL;
    }
    return o;
}

/* ---- validation ---- */

pc_status pc_algo_cr_validate_curve(const float (*pts)[2], size_t n, pc_error *err)
{
    size_t i;
    if (n == 1) {
        return pc_fail(err, PC_ERR_INVALID, "a nonempty curve needs at least two points");
    }
    if (n > PC_ALGO_CR_MAX_CURVE_POINTS) {
        return pc_fail(err, PC_ERR_INVALID, "at most %d curve points", PC_ALGO_CR_MAX_CURVE_POINTS);
    }
    for (i = 0; i < n; i++) {
        int k;
        for (k = 0; k < 2; k++) {
            float v = pts[i][k];
            if (!(isfinite(v) && v >= 0.0f && v <= 255.0f)) {
                return pc_fail(err, PC_ERR_INVALID, "curve coordinates must be finite and in 0..=255");
            }
        }
    }
    for (i = 0; i + 1 < n; i++) {
        if (pts[i + 1][0] - pts[i][0] < 1.0f) {
            return pc_fail(err, PC_ERR_INVALID, "curve inputs must increase by at least one level");
        }
    }
    return PC_OK;
}

static pc_status named(const char *name, const pc_algo_points *c, pc_error *err)
{
    pc_error e;
    pc_status st = pc_algo_cr_validate_curve((const float (*)[2])c->v, c->n, &e);
    if (st != PC_OK) {
        return pc_fail(err, st, "%s: %s", name, e.msg);
    }
    return PC_OK;
}

pc_status pc_algo_camera_raw_validate(const pc_algo_camera_raw *p, pc_error *err)
{
    pc_status st;
    if ((st = named("pointCurve", &p->point_curve, err)) != PC_OK ||
        (st = named("pointCurveRed", &p->point_curve_red, err)) != PC_OK ||
        (st = named("pointCurveGreen", &p->point_curve_green, err)) != PC_OK ||
        (st = named("pointCurveBlue", &p->point_curve_blue, err)) != PC_OK) {
        return st;
    }
    return PC_OK;
}

pc_status pc_algo_camera_raw_validate_changes(const pc_algo_camera_raw *p, const pc_algo_camera_raw *stored,
                                              pc_error *err)
{
    const char *names[4] = {"pointCurve", "pointCurveRed", "pointCurveGreen", "pointCurveBlue"};
    const pc_algo_points *cur[4] = {&p->point_curve, &p->point_curve_red, &p->point_curve_green,
                                    &p->point_curve_blue};
    const pc_algo_points *kept[4] = {&stored->point_curve, &stored->point_curve_red,
                                     &stored->point_curve_green, &stored->point_curve_blue};
    int i;
    for (i = 0; i < 4; i++) {
        if (!points_eq(cur[i], kept[i])) {
            pc_status st = named(names[i], cur[i], err);
            if (st != PC_OK) {
                return st;
            }
        }
    }
    return PC_OK;
}

/* ---- curves ---- */

typedef struct cpt {
    float x, y;
    size_t i; /* original index: the reference's sort is stable */
} cpt;

static int cmp_cpt(const void *a, const void *b)
{
    const cpt *p = a, *q = b;
    int c = pc_algo_l9u_total_cmpf(p->x, q->x);
    return c != 0 ? c : (p->i < q->i ? -1 : (p->i > q->i ? 1 : 0));
}

pc_status pc_algo_cr_curve_lut(const float (*pts)[2], size_t npts, float *out, size_t n, pc_error *err)
{
    cpt *p = NULL;
    float *d = NULL, *t = NULL;
    size_t m = 0, i, k;
    if (npts > 0) {
        p = pc_malloc_array(npts, sizeof *p);
        if (p == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < npts; i++) {
            p[i].x = pts[i][0] / 255.0f;
            p[i].y = pts[i][1] / 255.0f;
            p[i].i = i;
        }
        qsort(p, npts, sizeof *p, cmp_cpt);
        /* dedup_by: drop a point within 1e-6 of the last kept one. */
        for (i = 0; i < npts; i++) {
            if (m > 0 && fabsf(p[i].x - p[m - 1].x) < 1e-6f) {
                continue;
            }
            p[m++] = p[i];
        }
    }
    if (m < 2) {
        for (i = 0; i < n; i++) {
            out[i] = (float)i / (float)(n - 1);
        }
        free(p);
        return PC_OK;
    }
    d = pc_malloc_array(m, sizeof *d);
    t = pc_calloc_array(m, sizeof *t);
    if (d == NULL || t == NULL) {
        free(p);
        free(d);
        free(t);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (k = 0; k + 1 < m; k++) {
        d[k] = (p[k + 1].y - p[k].y) / pc_rs_maxf_k(p[k + 1].x - p[k].x, 1e-6f);
    }
    t[0] = d[0];
    t[m - 1] = d[m - 2];
    for (k = 1; k + 1 < m; k++) {
        t[k] = d[k - 1] * d[k] <= 0.0f ? 0.0f : (d[k - 1] + d[k]) / 2.0f;
    }
    for (k = 0; k + 1 < m; k++) {
        float a, b, s;
        if (d[k] == 0.0f) {
            t[k] = 0.0f;
            t[k + 1] = 0.0f;
            continue;
        }
        a = t[k] / d[k];
        b = t[k + 1] / d[k];
        s = a * a + b * b;
        if (s > 9.0f) {
            float tau = 3.0f / sqrtf(s);
            t[k] = tau * a * d[k];
            t[k + 1] = tau * b * d[k];
        }
    }
    for (i = 0; i < n; i++) {
        float x = (float)i / (float)(n - 1), hh, s, h00, h10, h01, h11;
        if (x <= p[0].x) {
            out[i] = p[0].y;
            continue;
        }
        if (x >= p[m - 1].x) {
            out[i] = p[m - 1].y;
            continue;
        }
        k = m - 2;
        for (size_t j = 0; j + 1 < m; j++) {
            if (x <= p[j + 1].x) {
                k = j;
                break;
            }
        }
        hh = p[k + 1].x - p[k].x;
        s = (x - p[k].x) / hh;
        h00 = 2.0f * s * s * s - 3.0f * s * s + 1.0f;
        h10 = s * s * s - 2.0f * s * s + s;
        h01 = -2.0f * s * s * s + 3.0f * s * s;
        h11 = s * s * s - s * s;
        out[i] =
            pc_clampf(h00 * p[k].y + h10 * hh * t[k] + h01 * p[k + 1].y + h11 * hh * t[k + 1], 0.0f, 1.0f);
    }
    free(p);
    free(d);
    free(t);
    return PC_OK;
}

void pc_algo_cr_band_weights(float h, float out[8])
{
    int k;
    for (k = 0; k < 8; k++) {
        float c = PC_ALGO_CR_HSL_BANDS[k], prev = PC_ALGO_CR_HSL_BANDS[(k + 7) % 8];
        float next = PC_ALGO_CR_HSL_BANDS[(k + 1) % 8];
        float diff = fmodf(h - c + 540.0f, 360.0f) - 180.0f;
        float span =
            diff >= 0.0f ? pc_algo_rem_euclidf(next - c, 360.0f) : pc_algo_rem_euclidf(c - prev, 360.0f);
        out[k] = pc_rs_maxf_k(1.0f - fabsf(diff) / pc_rs_maxf_k(span, 1e-3f), 0.0f);
    }
}

void pc_algo_cr_white_balance_gains(float temperature, float tint, float out[3])
{
    float t = temperature / 100.0f, tn = tint / 100.0f;
    out[0] = 1.0f + 0.35f * t;
    out[1] = 1.0f - 0.25f * tn;
    out[2] = 1.0f - 0.35f * t;
}
