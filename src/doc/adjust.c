/* Adjustment-layer parameters (port of crates/doc/src/adjust.rs). */
#include "doc/adjust.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* ---- curves ---- */

void pc_curve_fini(pc_curve *c)
{
    if (c == NULL) {
        return;
    }
    free(c->pts);
    c->pts = NULL;
    c->len = 0;
}

pc_status pc_curve_clone(const pc_curve *c, pc_curve *out, pc_error *err)
{
    pc_curve t = {NULL, 0};
    if (c->len > 0) {
        t.pts = pc_malloc_array(c->len, sizeof *t.pts);
        if (t.pts == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(t.pts, c->pts, c->len * sizeof *t.pts);
        t.len = c->len;
    }
    *out = t;
    return PC_OK;
}

bool pc_curve_eq(const pc_curve *a, const pc_curve *b)
{
    size_t i;
    if (a->len != b->len) {
        return false;
    }
    for (i = 0; i < a->len; i++) {
        if (a->pts[i].input != b->pts[i].input || a->pts[i].output != b->pts[i].output) {
            return false;
        }
    }
    return true;
}

pc_status pc_curve_identity(pc_curve *out, pc_error *err)
{
    pc_curve_point *p = pc_malloc_array(2, sizeof *p);
    if (p == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    p[0] = (pc_curve_point){0.0f, 0.0f};
    p[1] = (pc_curve_point){1.0f, 1.0f};
    out->pts = p;
    out->len = 2;
    return PC_OK;
}

bool pc_curve_is_identity(const pc_curve_point *pts, size_t n)
{
    if (n == 2) {
        return pts[0].input == 0.0f && pts[0].output == 0.0f && pts[1].input == 1.0f && pts[1].output == 1.0f;
    }
    return n < 2;
}

pc_status pc_curve_map(const float *outputs, size_t n, pc_curve *out, pc_error *err)
{
    const float last = (float)(PC_CURVE_MAP_LEN - 1);
    pc_curve_point *p = pc_malloc_array(PC_CURVE_MAP_LEN, sizeof *p);
    float prev = 0.0f;
    if (p == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (size_t k = 0; k < PC_CURVE_MAP_LEN; k++) {
        float o = prev;
        if (outputs != NULL && k < n && isfinite(outputs[k])) {
            o = pc_clampf(outputs[k], 0.0f, 1.0f);
        }
        prev = o;
        p[k] = (pc_curve_point){(float)k / last, o};
    }
    out->pts = p;
    out->len = PC_CURVE_MAP_LEN;
    return PC_OK;
}

/* f32::total_cmp as a signed integer key. */
static int32_t total_key(float f)
{
    uint32_t u;
    int32_t k;
    memcpy(&u, &f, sizeof u);
    u ^= ((u >> 31) != 0 ? 0xffffffffu : 0u) >> 1;
    memcpy(&k, &u, sizeof k);
    return k;
}

typedef struct map_pt {
    float in, out;
    size_t idx;
} map_pt;

static int map_pt_cmp(const void *a, const void *b)
{
    const map_pt *x = a;
    const map_pt *y = b;
    int32_t kx = total_key(x->in);
    int32_t ky = total_key(y->in);
    if (kx != ky) {
        return kx < ky ? -1 : 1;
    }
    return x->idx < y->idx ? -1 : x->idx > y->idx ? 1 : 0; /* sort_by is stable */
}

pc_status pc_curve_map_outputs(const pc_curve_point *pts, size_t n, float out[PC_CURVE_MAP_LEN],
                               pc_error *err)
{
    const float last = (float)(PC_CURVE_MAP_LEN - 1);
    map_pt *v = pc_malloc_array(n > 0 ? n : 1, sizeof *v);
    size_t m = 0;
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < n; i++) {
        if (isfinite(pts[i].input) && isfinite(pts[i].output)) {
            v[m] = (map_pt){pts[i].input, pts[i].output, m};
            m++;
        }
    }
    qsort(v, m, sizeof *v, map_pt_cmp);
    for (size_t k = 0; k < PC_CURVE_MAP_LEN; k++) {
        float x = (float)k / last;
        float y = x;
        if (m >= 2) {
            const map_pt *a = &v[0];
            const map_pt *b = &v[m - 1];
            if (x <= a->in) {
                y = a->out;
            } else if (x >= b->in) {
                y = b->out;
            } else {
                size_t lo = 0;
                size_t hi = m; /* partition_point(|p| p.in < x) */
                while (lo < hi) {
                    size_t mid = lo + (hi - lo) / 2;
                    if (v[mid].in < x) {
                        lo = mid + 1;
                    } else {
                        hi = mid;
                    }
                }
                if (lo >= 1 && lo < m && v[lo].in > v[lo - 1].in) {
                    const map_pt *p0 = &v[lo - 1];
                    const map_pt *p1 = &v[lo];
                    y = p0->out + (p1->out - p0->out) * (x - p0->in) / (p1->in - p0->in);
                } else if (lo < m) {
                    y = v[lo].out;
                } else {
                    y = b->out;
                }
            }
        }
        out[k] = pc_clampf(y, 0.0f, 1.0f);
    }
    free(v);
    return PC_OK;
}

bool pc_lut3d_len(uint32_t size, size_t *out)
{
    size_t s = (size_t)size;
    size_t cube;
    size_t len;
    if (size < 2) {
        return false;
    }
    if (!pc_size_mul3(s, s, s, &cube) || !pc_size_mul(cube, 3, &len)) {
        return false;
    }
    *out = len;
    return true;
}

pc_levels_channel pc_levels_channel_default(void)
{
    return (pc_levels_channel){0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
}

static bool levels_channel_eq(const pc_levels_channel *a, const pc_levels_channel *b)
{
    return a->in_black == b->in_black && a->in_white == b->in_white && a->gamma == b->gamma &&
           a->out_black == b->out_black && a->out_white == b->out_white;
}

/* ---- hue ranges ---- */

/* f32::rem_euclid: `r = a % b; if r < 0 { r + |b| } else { r }` (Rust's % is fmodf). */
static float rem_euclidf(float a, float b)
{
    float r = fmodf(a, b);
    return r < 0.0f ? r + fabsf(b) : r;
}

pc_hue_range pc_hue_range_neutral(size_t i)
{
    float c = 60.0f * (float)i;
    pc_hue_range r;
    r.hue = 0.0f;
    r.saturation = 0.0f;
    r.lightness = 0.0f;
    r.bounds[0] = c - 45.0f;
    r.bounds[1] = c - 15.0f;
    r.bounds[2] = c + 15.0f;
    r.bounds[3] = c + 45.0f;
    return r;
}

void pc_hue_range_defaults(pc_hue_range out[6])
{
    size_t i;
    for (i = 0; i < 6; i++) {
        out[i] = pc_hue_range_neutral(i);
    }
}

void pc_hue_range_canonical_bounds(const float b[4], float out[4])
{
    float o[4];
    float centre;
    float shift;
    size_t k;
    for (k = 0; k < 4; k++) {
        if (!isfinite(b[k])) {
            out[0] = -45.0f;
            out[1] = -15.0f;
            out[2] = 15.0f;
            out[3] = 45.0f;
            return;
        }
    }
    o[0] = o[1] = o[2] = o[3] = b[0];
    for (k = 1; k < 4; k++) {
        o[k] = pc_rs_minf(o[k - 1] + rem_euclidf(b[k] - o[k - 1], 360.0f), o[0] + 359.0f);
    }
    centre = (o[1] + o[2]) / 2.0f;
    shift = rem_euclidf(centre + 30.0f, 360.0f) - 30.0f - centre;
    for (k = 0; k < 4; k++) {
        out[k] = o[k] + shift;
    }
}

bool pc_hue_range_is_neutral(const pc_hue_range *r)
{
    return r->hue == 0.0f && r->saturation == 0.0f && r->lightness == 0.0f;
}

/* Degrees past the range's start, so wrap-around ranges (reds) work; 0 for non-finite x. */
static float hue_rel(float a, float x)
{
    return isfinite(x) ? rem_euclidf(x - a, 360.0f) : 0.0f;
}

float pc_hue_range_weight(const pc_hue_range *r, float deg)
{
    float a = r->bounds[0];
    float b = hue_rel(a, r->bounds[1]);
    float c = pc_rs_maxf(hue_rel(a, r->bounds[2]), b);
    float d = pc_rs_maxf(hue_rel(a, r->bounds[3]), c);
    float h = hue_rel(a, deg);
    if (h <= b) {
        return b <= 0.0f ? 1.0f : h / b;
    }
    if (h <= c) {
        return 1.0f;
    }
    if (h <= d) {
        return d <= c ? 1.0f : (d - h) / (d - c);
    }
    return 0.0f;
}

static bool hue_range_eq(const pc_hue_range *a, const pc_hue_range *b)
{
    return a->hue == b->hue && a->saturation == b->saturation && a->lightness == b->lightness &&
           a->bounds[0] == b->bounds[0] && a->bounds[1] == b->bounds[1] && a->bounds[2] == b->bounds[2] &&
           a->bounds[3] == b->bounds[3];
}

/* ---- adjustments ---- */

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

void pc_adjustment_fini(pc_adjustment *a)
{
    size_t k;
    if (a == NULL) {
        return;
    }
    switch (a->kind) {
    case PC_ADJ_CURVES:
        pc_curve_fini(&a->u.curves.master);
        for (k = 0; k < 3; k++) {
            pc_curve_fini(&a->u.curves.per_channel[k]);
        }
        pc_curve_fini(&a->u.curves.black);
        break;
    case PC_ADJ_COLOR_LOOKUP:
        pc_str_fini(&a->u.color_lookup.name);
        pc_blob_release(a->u.color_lookup.lut);
        break;
    case PC_ADJ_GRADIENT_MAP:
        free(a->u.gradient_map.stops);
        break;
    case PC_ADJ_UNSUPPORTED:
        pc_str_fini(&a->u.unsupported.psd_key);
        pc_blob_release(a->u.unsupported.raw);
        break;
    default:
        break;
    }
    memset(a, 0, sizeof *a);
}

pc_status pc_adjustment_clone(const pc_adjustment *a, pc_adjustment *out, pc_error *err)
{
    pc_adjustment t = *a; /* plain fields; owned members are replaced below */
    pc_status st = PC_OK;
    size_t k;
    switch (a->kind) {
    case PC_ADJ_CURVES: {
        pc_curve *dst[5];
        const pc_curve *src[5];
        size_t done = 0;
        dst[0] = &t.u.curves.master;
        src[0] = &a->u.curves.master;
        for (k = 0; k < 3; k++) {
            dst[k + 1] = &t.u.curves.per_channel[k];
            src[k + 1] = &a->u.curves.per_channel[k];
        }
        dst[4] = &t.u.curves.black;
        src[4] = &a->u.curves.black;
        for (k = 0; k < 5; k++) {
            dst[k]->pts = NULL;
            dst[k]->len = 0;
        }
        for (done = 0; done < 5 && st == PC_OK; done++) {
            st = pc_curve_clone(src[done], dst[done], err);
        }
        if (st != PC_OK) {
            for (k = 0; k < 5; k++) {
                pc_curve_fini(dst[k]);
            }
            return st;
        }
        break;
    }
    case PC_ADJ_COLOR_LOOKUP:
        st = pc_str_clone(&a->u.color_lookup.name, &t.u.color_lookup.name, err);
        if (st != PC_OK) {
            return st;
        }
        t.u.color_lookup.lut = pc_blob_retain(a->u.color_lookup.lut);
        break;
    case PC_ADJ_GRADIENT_MAP:
        t.u.gradient_map.stops = NULL;
        if (a->u.gradient_map.nstops > 0) {
            t.u.gradient_map.stops = pc_malloc_array(a->u.gradient_map.nstops, sizeof *t.u.gradient_map.stops);
            if (t.u.gradient_map.stops == NULL) {
                return pc_fail(err, PC_ERR_NOMEM, "out of memory");
            }
            memcpy(t.u.gradient_map.stops, a->u.gradient_map.stops,
                   a->u.gradient_map.nstops * sizeof *t.u.gradient_map.stops);
        }
        break;
    case PC_ADJ_UNSUPPORTED:
        st = pc_str_clone(&a->u.unsupported.psd_key, &t.u.unsupported.psd_key, err);
        if (st != PC_OK) {
            return st;
        }
        t.u.unsupported.raw = pc_blob_retain(a->u.unsupported.raw);
        break;
    default:
        break;
    }
    *out = t;
    return PC_OK;
}

/* Vec<u8> equality where NULL is the empty vector. */
static bool bytes_blob_eq(const pc_blob *a, const pc_blob *b)
{
    size_t n = pc_blob_len(a);
    if (n != pc_blob_len(b)) {
        return false;
    }
    return n == 0 || memcmp(pc_blob_data(a), pc_blob_data(b), n) == 0;
}

/* Option<Arc<Vec<f32>>> equality: element-wise ==, like Vec<f32>'s PartialEq. */
static bool lut_eq(const pc_adjustment *a, const pc_adjustment *b)
{
    size_t na;
    size_t nb;
    const float *fa;
    const float *fb;
    if ((a->u.color_lookup.lut == NULL) != (b->u.color_lookup.lut == NULL)) {
        return false;
    }
    if (a->u.color_lookup.lut == NULL) {
        return true;
    }
    fa = pc_adjustment_lut(a, &na);
    fb = pc_adjustment_lut(b, &nb);
    return na == nb && (na == 0 || floats_eq(fa, fb, na));
}

bool pc_adjustment_eq(const pc_adjustment *a, const pc_adjustment *b)
{
    size_t k;
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_ADJ_BRIGHTNESS_CONTRAST:
        return a->u.brightness_contrast.brightness == b->u.brightness_contrast.brightness &&
               a->u.brightness_contrast.contrast == b->u.brightness_contrast.contrast &&
               a->u.brightness_contrast.legacy == b->u.brightness_contrast.legacy;
    case PC_ADJ_LEVELS:
        if (!levels_channel_eq(&a->u.levels.master, &b->u.levels.master)) {
            return false;
        }
        for (k = 0; k < 3; k++) {
            if (!levels_channel_eq(&a->u.levels.per_channel[k], &b->u.levels.per_channel[k])) {
                return false;
            }
        }
        return a->u.levels.space == b->u.levels.space && levels_channel_eq(&a->u.levels.black, &b->u.levels.black);
    case PC_ADJ_CURVES:
        if (!pc_curve_eq(&a->u.curves.master, &b->u.curves.master)) {
            return false;
        }
        for (k = 0; k < 3; k++) {
            if (!pc_curve_eq(&a->u.curves.per_channel[k], &b->u.curves.per_channel[k])) {
                return false;
            }
        }
        return a->u.curves.space == b->u.curves.space &&
               pc_curve_eq(&a->u.curves.black, &b->u.curves.black) &&
               a->u.curves.pencil == b->u.curves.pencil;
    case PC_ADJ_EXPOSURE:
        return a->u.exposure.exposure == b->u.exposure.exposure && a->u.exposure.offset == b->u.exposure.offset &&
               a->u.exposure.gamma == b->u.exposure.gamma;
    case PC_ADJ_VIBRANCE:
        return a->u.vibrance.vibrance == b->u.vibrance.vibrance &&
               a->u.vibrance.saturation == b->u.vibrance.saturation;
    case PC_ADJ_HUE_SATURATION:
        if (a->u.hue_saturation.hue != b->u.hue_saturation.hue ||
            a->u.hue_saturation.saturation != b->u.hue_saturation.saturation ||
            a->u.hue_saturation.lightness != b->u.hue_saturation.lightness ||
            a->u.hue_saturation.colorize != b->u.hue_saturation.colorize) {
            return false;
        }
        for (k = 0; k < 6; k++) {
            if (!hue_range_eq(&a->u.hue_saturation.ranges[k], &b->u.hue_saturation.ranges[k])) {
                return false;
            }
        }
        return true;
    case PC_ADJ_COLOR_BALANCE:
        return floats_eq(a->u.color_balance.shadows, b->u.color_balance.shadows, 3) &&
               floats_eq(a->u.color_balance.midtones, b->u.color_balance.midtones, 3) &&
               floats_eq(a->u.color_balance.highlights, b->u.color_balance.highlights, 3) &&
               a->u.color_balance.preserve_luminosity == b->u.color_balance.preserve_luminosity;
    case PC_ADJ_BLACK_WHITE:
        if (!floats_eq(a->u.black_white.weights, b->u.black_white.weights, 6) ||
            a->u.black_white.has_tint != b->u.black_white.has_tint) {
            return false;
        }
        return !a->u.black_white.has_tint || floats_eq(a->u.black_white.tint, b->u.black_white.tint, 3);
    case PC_ADJ_PHOTO_FILTER:
        return floats_eq(a->u.photo_filter.color, b->u.photo_filter.color, 3) &&
               a->u.photo_filter.density == b->u.photo_filter.density &&
               a->u.photo_filter.preserve_luminosity == b->u.photo_filter.preserve_luminosity;
    case PC_ADJ_CHANNEL_MIXER:
        for (k = 0; k < 3; k++) {
            if (!floats_eq(a->u.channel_mixer.matrix[k], b->u.channel_mixer.matrix[k], 4)) {
                return false;
            }
        }
        return a->u.channel_mixer.monochrome == b->u.channel_mixer.monochrome;
    case PC_ADJ_COLOR_LOOKUP:
        return pc_str_eq(&a->u.color_lookup.name, &b->u.color_lookup.name) && lut_eq(a, b) &&
               a->u.color_lookup.size == b->u.color_lookup.size &&
               a->u.color_lookup.tetrahedral == b->u.color_lookup.tetrahedral &&
               a->u.color_lookup.dither == b->u.color_lookup.dither;
    case PC_ADJ_INVERT:
        return true;
    case PC_ADJ_POSTERIZE:
        return a->u.posterize.levels == b->u.posterize.levels;
    case PC_ADJ_THRESHOLD:
        return a->u.threshold.level == b->u.threshold.level;
    case PC_ADJ_GRADIENT_MAP:
        if (a->u.gradient_map.nstops != b->u.gradient_map.nstops) {
            return false;
        }
        for (k = 0; k < a->u.gradient_map.nstops; k++) {
            const pc_gradient_map_stop *sa = &a->u.gradient_map.stops[k];
            const pc_gradient_map_stop *sb = &b->u.gradient_map.stops[k];
            if (sa->pos != sb->pos || !floats_eq(sa->rgb, sb->rgb, 3)) {
                return false;
            }
        }
        return a->u.gradient_map.reverse == b->u.gradient_map.reverse &&
               a->u.gradient_map.dither == b->u.gradient_map.dither;
    case PC_ADJ_SELECTIVE_COLOR:
        if (a->u.selective_color.relative != b->u.selective_color.relative) {
            return false;
        }
        for (k = 0; k < 9; k++) {
            if (!floats_eq(a->u.selective_color.adjustments[k], b->u.selective_color.adjustments[k], 4)) {
                return false;
            }
        }
        return true;
    case PC_ADJ_UNSUPPORTED:
        return pc_str_eq(&a->u.unsupported.psd_key, &b->u.unsupported.psd_key) &&
               bytes_blob_eq(a->u.unsupported.raw, b->u.unsupported.raw);
    }
    return false;
}

const char *pc_adjustment_label(const pc_adjustment *a)
{
    switch (a->kind) {
    case PC_ADJ_BRIGHTNESS_CONTRAST:
        return "Brightness/Contrast";
    case PC_ADJ_LEVELS:
        return "Levels";
    case PC_ADJ_CURVES:
        return "Curves";
    case PC_ADJ_EXPOSURE:
        return "Exposure";
    case PC_ADJ_VIBRANCE:
        return "Vibrance";
    case PC_ADJ_HUE_SATURATION:
        return "Hue/Saturation";
    case PC_ADJ_COLOR_BALANCE:
        return "Color Balance";
    case PC_ADJ_BLACK_WHITE:
        return "Black & White";
    case PC_ADJ_PHOTO_FILTER:
        return "Photo Filter";
    case PC_ADJ_CHANNEL_MIXER:
        return "Channel Mixer";
    case PC_ADJ_COLOR_LOOKUP:
        return "Color Lookup";
    case PC_ADJ_INVERT:
        return "Invert";
    case PC_ADJ_POSTERIZE:
        return "Posterize";
    case PC_ADJ_THRESHOLD:
        return "Threshold";
    case PC_ADJ_GRADIENT_MAP:
        return "Gradient Map";
    case PC_ADJ_SELECTIVE_COLOR:
        return "Selective Color";
    case PC_ADJ_UNSUPPORTED:
        return "Adjustment";
    }
    return "Adjustment";
}

const float *pc_adjustment_lut(const pc_adjustment *a, size_t *n)
{
    if (a->kind != PC_ADJ_COLOR_LOOKUP || a->u.color_lookup.lut == NULL) {
        *n = 0;
        return NULL;
    }
    *n = pc_blob_len(a->u.color_lookup.lut) / sizeof(float);
    /* Blob data is aligned for any scalar type (blob.h). */
    return (const float *)(const void *)pc_blob_data(a->u.color_lookup.lut);
}

void pc_adjustment_default_hue_saturation(pc_adjustment *out)
{
    memset(out, 0, sizeof *out);
    out->kind = PC_ADJ_HUE_SATURATION;
    out->u.hue_saturation.hue = 0.0f;
    out->u.hue_saturation.saturation = 0.0f;
    out->u.hue_saturation.lightness = 0.0f;
    out->u.hue_saturation.colorize = false;
    pc_hue_range_defaults(out->u.hue_saturation.ranges);
}

pc_status pc_adjustment_identity_curves(pc_adjustment *out, pc_error *err)
{
    pc_adjustment t;
    pc_status st = PC_OK;
    size_t k;
    memset(&t, 0, sizeof t);
    t.kind = PC_ADJ_CURVES;
    t.u.curves.space = PC_TONE_RGB;
    st = pc_curve_identity(&t.u.curves.master, err);
    for (k = 0; k < 3 && st == PC_OK; k++) {
        st = pc_curve_identity(&t.u.curves.per_channel[k], err);
    }
    if (st != PC_OK) {
        pc_adjustment_fini(&t);
        return st;
    }
    *out = t;
    return PC_OK;
}

void pc_adjustment_identity_levels(pc_adjustment *out)
{
    size_t k;
    memset(out, 0, sizeof *out);
    out->kind = PC_ADJ_LEVELS;
    out->u.levels.master = pc_levels_channel_default();
    for (k = 0; k < 3; k++) {
        out->u.levels.per_channel[k] = pc_levels_channel_default();
    }
    out->u.levels.space = PC_TONE_RGB;
    out->u.levels.black = pc_levels_channel_default();
}
