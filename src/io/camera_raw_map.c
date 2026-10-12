/* Camera Raw Filter descriptors (port of crates/io/src/camera_raw_map.rs). The reference goes
 * through serde_json::Value for the params; here the typed settings (pc_algo_camera_raw) are
 * used directly: from_value(to_value(..)) of the reference is the f64 -> f32 cast, and comparing
 * the normalised values is comparing the f32 fields. */
#include "io/camera_raw_map.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "algo/camera_raw.h"
#include "base/num.h"

#define CR_FILTER_ID 2783
#define TEMPLATE_KEY "__cameraRawPsd"
#define MAX_TEMPLATE_BYTES ((size_t)1024 * 1024)

typedef pc_algo_camera_raw CR;

/* Exposure and sharpening radius are doubles; other observed controls are integer levels. */
typedef struct scalar {
    const char *param; /* "wheel.field" for grading wheels */
    const char *key;
    size_t off;
} scalar;

#define S(p, k, f) {p, k, offsetof(CR, f)}
static const scalar SCALARS[] = {
    S("temperature", "Temp", temperature),
    S("tint", "Tint", tint),
    S("exposure", "Ex12", exposure),
    S("contrast", "Cr12", contrast),
    S("highlights", "Hi12", highlights),
    S("shadows", "Sh12", shadows),
    S("whites", "Wh12", whites),
    S("blacks", "Bk12", blacks),
    S("texture", "CrTx", texture),
    S("clarity", "Cl12", clarity),
    S("dehaze", "Dhze", dehaze),
    S("vibrance", "Vibr", vibrance),
    S("saturation", "Strt", saturation),
    S("curveShadows", "PC_S", curve_shadows),
    S("curveDarks", "PC_D", curve_darks),
    S("curveLights", "PC_L", curve_lights),
    S("curveHighlights", "PC_H", curve_highlights),
    S("sharpenAmount", "Shrp", sharpen_amount),
    S("sharpenRadius", "ShpR", sharpen_radius),
    S("sharpenDetail", "ShpD", sharpen_detail),
    S("sharpenMasking", "ShpM", sharpen_masking),
    S("noiseLuminance", "LNR ", noise_luminance),
    S("noiseLuminanceDetail", "LNRD", noise_luminance_detail),
    S("noiseColor", "CNR ", noise_color),
    S("noiseColorDetail", "CNRD", noise_color_detail),
    S("gradeShadows.hue", "STSH", grade_shadows.hue),
    S("gradeShadows.sat", "STSS", grade_shadows.sat),
    S("gradeShadows.lum", "CgSL", grade_shadows.lum),
    S("gradeMidtones.hue", "CgMH", grade_midtones.hue),
    S("gradeMidtones.sat", "CgMS", grade_midtones.sat),
    S("gradeMidtones.lum", "CgML", grade_midtones.lum),
    S("gradeHighlights.hue", "STHH", grade_highlights.hue),
    S("gradeHighlights.sat", "STHS", grade_highlights.sat),
    S("gradeHighlights.lum", "CgHL", grade_highlights.lum),
    S("gradeGlobal.hue", "CgGH", grade_global.hue),
    S("gradeGlobal.sat", "CgGS", grade_global.sat),
    S("gradeGlobal.lum", "CgGL", grade_global.lum),
    S("gradeBlending", "CgBl", grade_blending),
    S("gradeBalance", "STB ", grade_balance),
    S("grainAmount", "GRNA", grain_amount),
    S("grainSize", "GRNS", grain_size),
    S("grainRoughness", "GRNF", grain_roughness),
    S("vignetteAmount", "PCVA", vignette_amount),
    S("vignetteMidpoint", "PCVM", vignette_midpoint),
    S("vignetteFeather", "PCVF", vignette_feather),
    S("vignetteRoundness", "PCVR", vignette_roundness),
};
#undef S
#define NSCALARS (sizeof SCALARS / sizeof SCALARS[0])

typedef struct array {
    const char *param;
    size_t off;
    size_t n;
    const char *keys[8];
} array;

static const array ARRAYS[] = {
    {"curveSplits", offsetof(CR, curve_splits), 3, {"PC_1", "PC_2", "PC_3"}},
    {"hslHue", offsetof(CR, hsl_hue), 8, {"HA_R", "HA_O", "HA_Y", "HA_G", "HA_A", "HA_B", "HA_P", "HA_M"}},
    {"hslSat", offsetof(CR, hsl_sat), 8, {"SA_R", "SA_O", "SA_Y", "SA_G", "SA_A", "SA_B", "SA_P", "SA_M"}},
    {"hslLum", offsetof(CR, hsl_lum), 8, {"LA_R", "LA_O", "LA_Y", "LA_G", "LA_A", "LA_B", "LA_P", "LA_M"}},
};
#define NARRAYS (sizeof ARRAYS / sizeof ARRAYS[0])

typedef struct curve {
    const char *param;
    const char *key;
    size_t off;
} curve;

static const curve CURVES[] = {
    {"pointCurve", "Crv ", offsetof(CR, point_curve)},
    {"pointCurveRed", "CrvR", offsetof(CR, point_curve_red)},
    {"pointCurveGreen", "CrvG", offsetof(CR, point_curve_green)},
    {"pointCurveBlue", "CrvB", offsetof(CR, point_curve_blue)},
};
#define NCURVES (sizeof CURVES / sizeof CURVES[0])

/* These controls have no PhotoCraft counterpart yet. Their observed neutral values can be
 * retained in the template; active values must keep the entire filter opaque. */
typedef struct neutral {
    const char *key;
    int32_t value;
} neutral;

static const neutral NEUTRAL_ONLY[] = {{"LNRC", 0},   {"CNRS", 50}, {"MDis", 0}, {"VigA", 0}, {"VigM", 50},
                                       {"crfs", 100}, {"TMMs", 0},  {"PGTM", 0}, {"RGBt", 0}};
#define NNEUTRAL (sizeof NEUTRAL_ONLY / sizeof NEUTRAL_ONLY[0])

static float *fld(CR *p, size_t off)
{
    return (float *)(void *)((char *)p + off);
}

static const float *cfld(const CR *p, size_t off)
{
    return (const float *)(const void *)((const char *)p + off);
}

static pc_algo_points *curve_of(CR *p, size_t off)
{
    return (pc_algo_points *)(void *)((char *)p + off);
}

static const pc_algo_points *ccurve_of(const CR *p, size_t off)
{
    return (const pc_algo_points *)(const void *)((const char *)p + off);
}

static bool key_eq(const pc_psd_id *id, const char *key)
{
    return pc_psd_id_is(id, key);
}

static bool known_key(const pc_psd_id *id)
{
    size_t i;
    size_t j;
    if (key_eq(id, "CrVe") || key_eq(id, "PrVN") || key_eq(id, "PrVe") || key_eq(id, "WBal")) {
        return true;
    }
    for (i = 0; i < NSCALARS; i++) {
        if (key_eq(id, SCALARS[i].key)) {
            return true;
        }
    }
    for (i = 0; i < NARRAYS; i++) {
        for (j = 0; j < ARRAYS[i].n; j++) {
            if (key_eq(id, ARRAYS[i].keys[j])) {
                return true;
            }
        }
    }
    for (i = 0; i < NCURVES; i++) {
        if (key_eq(id, CURVES[i].key)) {
            return true;
        }
    }
    for (i = 0; i < NNEUTRAL; i++) {
        if (key_eq(id, NEUTRAL_ONLY[i].key)) {
            return true;
        }
    }
    return false;
}

static bool is_double_key(const char *key)
{
    return strcmp(key, "Ex12") == 0 || strcmp(key, "ShpR") == 0;
}

static bool is_hue_key(const char *key)
{
    return strcmp(key, "STSH") == 0 || strcmp(key, "STHH") == 0 || strcmp(key, "CgMH") == 0 ||
           strcmp(key, "CgGH") == 0;
}

static void number_range(const char *key, double *lo, double *hi)
{
    static const char *const PCT[] = {"PC_1", "PC_2", "PC_3", "ShpD", "ShpM", "LNR ", "LNRD",
                                      "CNR ", "CNRD", "STSS", "STHS", "CgMS", "CgGS", "CgBl",
                                      "GRNA", "GRNS", "GRNF", "PCVM", "PCVF"};
    size_t i;
    if (strcmp(key, "Ex12") == 0) {
        *lo = -5.0;
        *hi = 5.0;
        return;
    }
    if (strcmp(key, "ShpR") == 0) {
        *lo = 0.5;
        *hi = 3.0;
        return;
    }
    if (strcmp(key, "Shrp") == 0) {
        *lo = 0.0;
        *hi = 150.0;
        return;
    }
    if (is_hue_key(key)) {
        *lo = 0.0;
        *hi = 360.0;
        return;
    }
    for (i = 0; i < sizeof PCT / sizeof PCT[0]; i++) {
        if (strcmp(key, PCT[i]) == 0) {
            *lo = 0.0;
            *hi = 100.0;
            return;
        }
    }
    *lo = -100.0;
    *hi = 100.0;
}

static bool in_range(const char *key, double v)
{
    double lo;
    double hi;
    number_range(key, &lo, &hi);
    return v >= lo && v <= hi;
}

static bool int_of(const pc_psd_desc *d, const char *key, int32_t *out)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    if (v == NULL || v->kind != PC_PSD_V_INTEGER) {
        return false;
    }
    *out = v->i;
    return true;
}

static const pc_psd_desc *desc_of(const pc_psd_desc *d, const char *key)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    if (v == NULL || (v->kind != PC_PSD_V_DESCRIPTOR && v->kind != PC_PSD_V_GLOBAL_OBJECT)) {
        return NULL;
    }
    return v->desc;
}

/* read_number: a finite in-range slider level (a double for Ex12 / ShpR, an integer otherwise). */
static bool read_number(const pc_psd_desc *d, const char *key, double *out)
{
    const pc_psd_value *v = pc_psd_desc_get(d, key);
    double value;
    if (v == NULL) {
        return false;
    }
    if (is_double_key(key)) {
        if (v->kind != PC_PSD_V_DOUBLE) {
            return false;
        }
        value = v->d;
    } else if (v->kind == PC_PSD_V_INTEGER) {
        value = (double)v->i;
    } else {
        return false;
    }
    if (!isfinite(value) || !in_range(key, value)) {
        return false;
    }
    *out = value;
    return true;
}

/* read_curve: an integer list of 2..=16 points; *ok false for the reference's None. */
static pc_status read_curve(const pc_psd_value *value, bool *ok, pc_algo_points *out, pc_error *err)
{
    float (*pts)[2];
    size_t n;
    size_t i;
    *ok = false;
    if (value->kind != PC_PSD_V_LIST) {
        return PC_OK;
    }
    if (value->nlist < 4 || value->nlist % 2 != 0 || value->nlist > PC_ALGO_CR_MAX_CURVE_POINTS * 2) {
        return PC_OK;
    }
    n = value->nlist / 2;
    pts = malloc(n * sizeof *pts);
    if (pts == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading a Camera Raw curve");
    }
    for (i = 0; i < n; i++) {
        const pc_psd_value *x = &value->list[2 * i];
        const pc_psd_value *y = &value->list[2 * i + 1];
        if (x->kind != PC_PSD_V_INTEGER || y->kind != PC_PSD_V_INTEGER) {
            free(pts);
            return PC_OK;
        }
        pts[i][0] = (float)x->i;
        pts[i][1] = (float)y->i;
    }
    if (pc_algo_cr_validate_curve((const float (*)[2])pts, n, NULL) != PC_OK) {
        free(pts);
        return PC_OK;
    }
    free(out->v);
    if (n == 2 && pts[0][0] == 0.0f && pts[0][1] == 0.0f && pts[1][0] == 255.0f && pts[1][1] == 255.0f) {
        free(pts);
        out->v = NULL;
        out->n = 0;
    } else {
        out->v = pts;
        out->n = n;
    }
    *ok = true;
    return PC_OK;
}

/* validate_splits on settings: three finite, strictly increasing percentages. */
static const char *splits_error(const float s[3])
{
    double previous = -1.0;
    size_t i;
    for (i = 0; i < 3; i++) {
        double v = (double)s[i];
        if (!(isfinite(v) && v >= 0.0 && v <= 100.0)) {
            return "curve splits must be finite percentages";
        }
        if (v <= previous) {
            return "curve splits must be strictly increasing";
        }
        previous = v;
    }
    return NULL;
}

static bool is_wbal_custom(const pc_psd_value *v)
{
    return v->kind == PC_PSD_V_ENUMERATED && pc_psd_id_is(&v->type_id, "WBal") &&
           pc_psd_id_is(&v->value, "Cst ");
}

/* descriptor_params: the settings of a supported Fltr descriptor; *ok false for None. */
static pc_status descriptor_params(const pc_psd_desc *d, bool *ok, CR *out, pc_error *err)
{
    int32_t prvn = 0;
    int32_t prve = 0;
    const pc_psd_value *crve;
    const pc_psd_value *wbal;
    CR p;
    size_t i;
    size_t j;
    pc_status st;
    *ok = false;
    if (!pc_psd_id_is(&d->class_id, PC_IO_CAMERA_RAW_CLASS)) {
        return PC_OK;
    }
    if (!int_of(d, "PrVN", &prvn) || !int_of(d, "PrVe", &prve) ||
        !((prvn == 5 && prve == 184549376) || (prvn == 6 && prve == 251920384))) {
        return PC_OK;
    }
    crve = pc_psd_desc_get(d, "CrVe");
    if (crve == NULL || crve->kind != PC_PSD_V_TEXT) {
        return PC_OK;
    }
    /* A partial decode would omit active processing (masks, profiles, HDR or other settings)
     * when the layer is re-rendered. Keep such a filter opaque instead. */
    for (i = 0; i < d->n; i++) {
        if (!known_key(&d->items[i].key)) {
            return PC_OK;
        }
    }
    wbal = pc_psd_desc_get(d, "WBal");
    if (wbal != NULL && !is_wbal_custom(wbal)) {
        return PC_OK;
    }
    for (i = 0; i < NNEUTRAL; i++) {
        int32_t v;
        if (pc_psd_desc_get(d, NEUTRAL_ONLY[i].key) != NULL &&
            (!int_of(d, NEUTRAL_ONLY[i].key, &v) || v != NEUTRAL_ONLY[i].value)) {
            return PC_OK;
        }
    }
    pc_algo_camera_raw_default(&p);
    for (i = 0; i < NSCALARS; i++) {
        double v;
        if (pc_psd_desc_get(d, SCALARS[i].key) == NULL) {
            continue;
        }
        if (!read_number(d, SCALARS[i].key, &v)) {
            return PC_OK;
        }
        *fld(&p, SCALARS[i].off) = (float)v;
    }
    for (i = 0; i < NCURVES; i++) {
        const pc_psd_value *v = pc_psd_desc_get(d, CURVES[i].key);
        bool cok;
        if (v == NULL) {
            continue;
        }
        st = read_curve(v, &cok, curve_of(&p, CURVES[i].off), err);
        if (st != PC_OK || !cok) {
            pc_algo_camera_raw_fini(&p);
            return st;
        }
    }
    for (i = 0; i < NARRAYS; i++) {
        for (j = 0; j < ARRAYS[i].n; j++) {
            double v;
            if (pc_psd_desc_get(d, ARRAYS[i].keys[j]) == NULL) {
                continue;
            }
            if (!read_number(d, ARRAYS[i].keys[j], &v)) {
                pc_algo_camera_raw_fini(&p);
                return PC_OK;
            }
            fld(&p, ARRAYS[i].off)[j] = (float)v;
        }
    }
    /* The reference checks the f64 values before the f32 cast; the levels are integers. */
    if (splits_error(p.curve_splits) != NULL) {
        pc_algo_camera_raw_fini(&p);
        return PC_OK;
    }
    *out = p;
    *ok = true;
    return PC_OK;
}

/* ---- descriptor helpers ---- */

/* Replaces item `key` in place, or appends it; takes *v (zeroed), also on failure. */
static pc_status set(pc_psd_desc *d, const char *key, pc_psd_value *v, pc_error *err)
{
    size_t i;
    for (i = 0; i < d->n; i++) {
        if (pc_psd_id_is(&d->items[i].key, key)) {
            pc_psd_value_fini(&d->items[i].value);
            d->items[i].value = *v;
            memset(v, 0, sizeof *v);
            return PC_OK;
        }
    }
    return pc_psd_desc_add(d, key, v, err);
}

static pc_status set_object(pc_psd_desc *d, const char *key, pc_psd_desc *obj, pc_error *err)
{
    pc_psd_value v;
    pc_status st = pc_psd_value_object(obj, &v, err);
    if (st != PC_OK) {
        return st;
    }
    return set(d, key, &v, err);
}

/* blend_options of a neutral filter: 100 % opacity, blend_long_value(Normal) = BlnM "normal". */
static pc_status neutral_blend_options(pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc d;
    pc_psd_value v;
    pc_status st = pc_psd_desc_init(&d, "blendOptions", err);
    if (st != PC_OK) {
        return st;
    }
    memset(&v, 0, sizeof v);
    v.kind = PC_PSD_V_UNIT_FLOAT;
    memcpy(v.unit, "#Prc", 4);
    v.d = 100.0;
    st = pc_psd_desc_add(&d, "Opct", &v, err);
    if (st == PC_OK) {
        st = pc_psd_value_enum("BlnM", "normal", &v, err);
    }
    if (st == PC_OK) {
        st = pc_psd_desc_add(&d, "Md  ", &v, err);
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    *out = d;
    return PC_OK;
}

static pc_status rgbc(double v, pc_psd_value *out, pc_error *err)
{
    static const char *const K[3] = {"Rd  ", "Grn ", "Bl  "};
    pc_psd_desc d;
    size_t i;
    pc_status st = pc_psd_desc_init(&d, "RGBC", err);
    for (i = 0; st == PC_OK && i < 3; i++) {
        pc_psd_value x = pc_psd_value_double(v);
        st = pc_psd_desc_add(&d, K[i], &x, err);
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&d);
        return st;
    }
    return pc_psd_value_object(&d, out, err);
}

static char *to_hex(const uint8_t *b, size_t n)
{
    static const char H[] = "0123456789abcdef";
    char *s;
    size_t i;
    if (n > (SIZE_MAX - 1) / 2) {
        return NULL;
    }
    s = malloc(n * 2 + 1);
    if (s == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        s[2 * i] = H[b[i] >> 4];
        s[2 * i + 1] = H[b[i] & 15];
    }
    s[2 * n] = '\0';
    return s;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* from_hex; *ok false for invalid hex. */
static pc_status from_hex(const char *s, size_t len, bool *ok, uint8_t **out, size_t *n, pc_error *err)
{
    uint8_t *b;
    size_t i;
    *ok = false;
    if (len % 2 != 0) {
        return PC_OK;
    }
    b = malloc(len / 2 + 1);
    if (b == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory decoding a Camera Raw template");
    }
    for (i = 0; i < len / 2; i++) {
        int hi = hex_digit(s[2 * i]);
        int lo = hex_digit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            free(b);
            return PC_OK;
        }
        b[i] = (uint8_t)(hi << 4 | lo);
    }
    *ok = true;
    *out = b;
    *n = len / 2;
    return PC_OK;
}

/* ---- import ---- */

pc_status pc_io_camera_raw_import_params(const pc_psd_desc *item, bool *found, pc_json_value *out,
                                         pc_error *err)
{
    const pc_psd_desc *fltr;
    pc_psd_desc template;
    pc_psd_desc blend;
    pc_psd_value v;
    pc_psd_bytes bytes = {NULL, 0};
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root;
    pc_json_value res = PC_JSON_VALUE_INIT;
    char *hex = NULL;
    int32_t id = 0;
    bool ok;
    CR p;
    pc_status st;
    *found = false;
    if (!int_of(item, "filterID", &id) || id != CR_FILTER_ID) {
        return PC_OK;
    }
    fltr = desc_of(item, "Fltr");
    if (fltr == NULL) {
        return PC_OK;
    }
    st = descriptor_params(fltr, &ok, &p, err);
    if (st != PC_OK || !ok) {
        return st;
    }
    memset(&template, 0, sizeof template);
    st = pc_psd_desc_clone(item, &template, err);
    if (st == PC_OK) {
        st = neutral_blend_options(&blend, err);
        if (st == PC_OK) {
            st = set_object(&template, "blendOptions", &blend, err);
        }
    }
    if (st == PC_OK) {
        v = pc_psd_value_bool(true);
        st = set(&template, "enab", &v, err);
    }
    if (st == PC_OK) {
        st = pc_psd_desc_to_bytes(&template, &bytes, err);
    }
    if (st != PC_OK || bytes.len > MAX_TEMPLATE_BYTES) {
        goto cleanup;
    }
    hex = to_hex(bytes.data, bytes.len);
    doc = yyjson_mut_doc_new(NULL);
    root = doc != NULL ? pc_algo_camera_raw_to_json(doc, &p) : NULL;
    if (hex == NULL || root == NULL || !yyjson_mut_obj_add_strcpy(doc, root, TEMPLATE_KEY, hex)) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory encoding Camera Raw settings");
        goto cleanup;
    }
    st = pc_json_value_from_mut(root, &res, err);
    if (st == PC_OK) {
        *out = res;
        *found = true;
    }
cleanup:
    yyjson_mut_doc_free(doc);
    free(hex);
    free(bytes.data);
    pc_psd_desc_fini(&template);
    pc_algo_camera_raw_fini(&p);
    return st;
}

/* ---- export ---- */

#define UNSUP(err, ...) pc_fail((err), PC_ERR_UNSUPPORTED, __VA_ARGS__)

/* Keys of the serialised defaults (and their values) for the unknown-setting checks. */
static yyjson_mut_val *defaults_json(yyjson_mut_doc *doc)
{
    CR d;
    yyjson_mut_val *v;
    pc_algo_camera_raw_default(&d);
    v = pc_algo_camera_raw_to_json(doc, &d);
    pc_algo_camera_raw_fini(&d);
    return v;
}

static bool supported_param(const char *key)
{
    size_t i;
    for (i = 0; i < NSCALARS; i++) {
        const char *p = SCALARS[i].param;
        const char *dot = strchr(p, '.');
        size_t n = dot != NULL ? (size_t)(dot - p) : strlen(p);
        if (strlen(key) == n && strncmp(key, p, n) == 0) {
            return true;
        }
    }
    for (i = 0; i < NARRAYS; i++) {
        if (strcmp(key, ARRAYS[i].param) == 0) {
            return true;
        }
    }
    for (i = 0; i < NCURVES; i++) {
        if (strcmp(key, CURVES[i].param) == 0) {
            return true;
        }
    }
    return false;
}

/* write_number: one slider level into d (rounded to the PSD's integer levels). */
static pc_status write_number(pc_psd_desc *d, const char *key, float value, pc_error *err)
{
    double number = (double)value;
    pc_psd_value v;
    if (!isfinite(number)) {
        return UNSUP(err, "Camera Raw %s must be finite", key);
    }
    if (!in_range(key, number)) {
        return UNSUP(err, "Camera Raw %s is outside the supported range", key);
    }
    /* PSD stores these as integer slider levels; PhotoCraft's sliders are continuous. Rounding
     * (at most half a level) keeps the filter instead of leaving it out of the PSD. Hue 360 is
     * the same angle as 0. */
    if (is_double_key(key)) {
        v = pc_psd_value_double(number);
    } else if (is_hue_key(key)) {
        v = pc_psd_value_int(pc_f64_to_i32(round(number)) % 360);
    } else {
        v = pc_psd_value_int(pc_f64_to_i32(round(number)));
    }
    return set(d, key, &v, err);
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

static pc_status write_curve(pc_psd_desc *d, const curve *c, const pc_algo_points *pts, pc_error *err)
{
    float levels[PC_ALGO_CR_MAX_CURVE_POINTS][2];
    size_t n = pts->n;
    size_t i;
    pc_psd_value list;
    pc_error e;
    pc_status st;
    if (n > PC_ALGO_CR_MAX_CURVE_POINTS) {
        return UNSUP(err, "Camera Raw `%s` has too many points", c->param);
    }
    for (i = 0; i < n; i++) {
        size_t k;
        for (k = 0; k < 2; k++) {
            double number = (double)pts->v[i][k];
            if (!(isfinite(number) && number >= 0.0 && number <= 255.0)) {
                return UNSUP(err, "Camera Raw curve coordinates must be levels 0\xe2\x80\x93"
                                  "255");
            }
            levels[i][k] = (float)round(number);
        }
    }
    if (n == 0) {
        levels[0][0] = 0.0f;
        levels[0][1] = 0.0f;
        levels[1][0] = 255.0f;
        levels[1][1] = 255.0f;
        n = 2;
    }
    if (pc_algo_cr_validate_curve((const float (*)[2])levels, n, &e) != PC_OK) {
        return UNSUP(err, "Camera Raw `%s`: %s", c->param, e.msg);
    }
    memset(&list, 0, sizeof list);
    list.kind = PC_PSD_V_LIST;
    for (i = 0; i < n; i++) {
        size_t k;
        for (k = 0; k < 2; k++) {
            pc_psd_value x = pc_psd_value_int(pc_f32_to_i32(levels[i][k]));
            st = pc_psd_value_list_push(&list, &x, err);
            if (st != PC_OK) {
                pc_psd_value_fini(&list);
                return st;
            }
        }
    }
    return set(d, c->key, &list, err);
}

/* The template item from TEMPLATE_KEY, or a new Camera Raw filter (18.4, process 6). */
static pc_status template_item(yyjson_mut_val *raw, pc_psd_desc *out, pc_error *err)
{
    pc_psd_desc item;
    pc_psd_desc fltr;
    pc_psd_value v;
    pc_status st;
    if (raw != NULL) {
        const char *s = yyjson_mut_get_str(raw);
        size_t len = yyjson_mut_get_len(raw);
        uint8_t *bytes = NULL;
        size_t n = 0;
        bool ok;
        const pc_psd_desc *f;
        int32_t id = 0;
        pc_error e;
        if (!yyjson_mut_is_str(raw) || s == NULL) {
            return UNSUP(err, "Camera Raw PSD template must be hex text");
        }
        if (len > MAX_TEMPLATE_BYTES * 2) {
            return UNSUP(err, "Camera Raw PSD template is too large");
        }
        st = from_hex(s, len, &ok, &bytes, &n, err);
        if (st != PC_OK) {
            return st;
        }
        if (!ok) {
            return UNSUP(err, "Camera Raw PSD template is not valid hex");
        }
        st = pc_psd_desc_parse(bytes, n, &item, &e);
        free(bytes);
        if (st == PC_ERR_NOMEM) {
            return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
        }
        if (st != PC_OK) {
            return UNSUP(err, "bad Camera Raw PSD template: %s", e.msg);
        }
        f = desc_of(&item, "Fltr");
        ok = false;
        if (pc_psd_id_is(&item.class_id, "filterFX") && int_of(&item, "filterID", &id) &&
            id == CR_FILTER_ID && f != NULL) {
            CR p;
            st = descriptor_params(f, &ok, &p, err);
            if (st != PC_OK) {
                pc_psd_desc_fini(&item);
                return st;
            }
            if (ok) {
                pc_algo_camera_raw_fini(&p);
            }
        }
        if (!ok) {
            pc_psd_desc_fini(&item);
            return UNSUP(err, "Camera Raw PSD template is not a supported Camera Raw filter");
        }
        *out = item;
        return PC_OK;
    }
    st = pc_psd_desc_init(&item, "filterFX", err);
    if (st != PC_OK) {
        return st;
    }
    memset(&fltr, 0, sizeof fltr);
    st = pc_psd_value_text("Camera Raw Filter", true, &v, err);
    if (st == PC_OK) {
        st = pc_psd_desc_add(&item, "Nm  ", &v, err);
    }
    if (st == PC_OK) {
        v = pc_psd_value_bool(true);
        st = pc_psd_desc_add(&item, "hasoptions", &v, err);
    }
    if (st == PC_OK) {
        st = rgbc(0.0, &v, err);
        if (st == PC_OK) {
            st = pc_psd_desc_add(&item, "FrgC", &v, err);
        }
    }
    if (st == PC_OK) {
        st = rgbc(255.0, &v, err);
        if (st == PC_OK) {
            st = pc_psd_desc_add(&item, "BckC", &v, err);
        }
    }
    if (st == PC_OK) {
        st = pc_psd_desc_init(&fltr, PC_IO_CAMERA_RAW_CLASS, err);
    }
    if (st == PC_OK) {
        st = pc_psd_value_text("18.4", true, &v, err);
        if (st == PC_OK) {
            st = pc_psd_desc_add(&fltr, "CrVe", &v, err);
        }
    }
    if (st == PC_OK) {
        v = pc_psd_value_int(6);
        st = pc_psd_desc_add(&fltr, "PrVN", &v, err);
    }
    if (st == PC_OK) {
        v = pc_psd_value_int(251920384);
        st = pc_psd_desc_add(&fltr, "PrVe", &v, err);
    }
    if (st == PC_OK) {
        st = pc_psd_value_object(&fltr, &v, err);
        if (st == PC_OK) {
            st = pc_psd_desc_add(&item, "Fltr", &v, err);
        }
    }
    if (st == PC_OK) {
        v = pc_psd_value_int(CR_FILTER_ID);
        st = pc_psd_desc_add(&item, "filterID", &v, err);
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&fltr);
        pc_psd_desc_fini(&item);
        return st;
    }
    *out = item;
    return PC_OK;
}

/* The checks before deserialising: object, known keys, wheel fields, curve sizes. */
/* obj[key] (first match, as yyjson_mut_obj_getn) through the iterator only: the clang analyzer
 * can't follow yyjson's inline lookup and reports an impossible NULL child inside yyjson.h. */
static yyjson_mut_val *obj_find(yyjson_mut_val *obj, const char *key, size_t klen)
{
    yyjson_mut_obj_iter it;
    yyjson_mut_val *k;
    if (!yyjson_mut_is_obj(obj) || !yyjson_mut_obj_iter_init(obj, &it)) {
        return NULL;
    }
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        const char *s = yyjson_mut_get_str(k);
        if (s != NULL && yyjson_mut_get_len(k) == klen && memcmp(s, key, klen) == 0) {
            return yyjson_mut_obj_iter_get_val(k);
        }
    }
    return NULL;
}

static pc_status precheck(yyjson_mut_val *obj, yyjson_mut_val *defaults, pc_error *err)
{
    static const char *const WHEELS[4] = {"gradeShadows", "gradeMidtones", "gradeHighlights", "gradeGlobal"};
    yyjson_mut_obj_iter it;
    yyjson_mut_val *k;
    size_t i;
    if (!yyjson_mut_is_obj(obj)) {
        return UNSUP(err, "Camera Raw parameters must be an object");
    }
    yyjson_mut_obj_iter_init(obj, &it);
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        const char *key = yyjson_mut_get_str(k);
        size_t klen = yyjson_mut_get_len(k);
        if (key == NULL) {
            continue;
        }
        if (strcmp(key, "layer") != 0 && strncmp(key, "__", 2) != 0 &&
            obj_find(defaults, key, klen) == NULL) {
            return UNSUP(err, "Camera Raw setting `%.*s` has no verified PSD mapping",
                         (int)(klen > 200 ? 200 : klen), key);
        }
    }
    for (i = 0; i < 4; i++) {
        yyjson_mut_val *w = obj_find(obj, WHEELS[i], strlen(WHEELS[i]));
        if (yyjson_mut_is_obj(w)) {
            yyjson_mut_obj_iter_init(w, &it);
            while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
                const char *f = yyjson_mut_get_str(k);
                if (f == NULL || (strcmp(f, "hue") != 0 && strcmp(f, "sat") != 0 && strcmp(f, "lum") != 0)) {
                    return UNSUP(err, "Camera Raw `%s` contains an unknown wheel setting", WHEELS[i]);
                }
            }
        }
    }
    /* Bound variable-sized curves before deserialisation allocates their point arrays. */
    for (i = 0; i < NCURVES; i++) {
        yyjson_mut_val *c = obj_find(obj, CURVES[i].param, strlen(CURVES[i].param));
        if (yyjson_mut_is_arr(c) && yyjson_mut_arr_size(c) > PC_ALGO_CR_MAX_CURVE_POINTS) {
            return UNSUP(err, "Camera Raw `%s` has too many points", CURVES[i].param);
        }
    }
    return PC_OK;
}

/* Settings from the params (serde from_value, then validate). */
static pc_status typed_params(yyjson_mut_val *obj, CR *out, pc_error *err)
{
    yyjson_mut_doc *m = yyjson_mut_doc_new(NULL);
    yyjson_doc *imut = NULL;
    pc_error e;
    pc_status st;
    CR p;
    if (m != NULL) {
        yyjson_mut_val *copy = yyjson_mut_val_mut_copy(m, obj);
        if (copy != NULL) {
            yyjson_mut_doc_set_root(m, copy);
            imut = yyjson_mut_doc_imut_copy(m, NULL);
        }
    }
    yyjson_mut_doc_free(m);
    if (imut == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading Camera Raw parameters");
    }
    st = pc_algo_camera_raw_from_json(yyjson_doc_get_root(imut), &p, &e);
    yyjson_doc_free(imut);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
    }
    if (st != PC_OK) {
        return UNSUP(err, "bad Camera Raw parameters: %s", e.msg);
    }
    st = pc_algo_camera_raw_validate(&p, &e);
    if (st != PC_OK) {
        pc_algo_camera_raw_fini(&p);
        return UNSUP(err, "%s", e.msg);
    }
    *out = p;
    return PC_OK;
}

/* Unsupported settings of the normalised params must equal their defaults. */
static pc_status check_unmapped(const CR *p, yyjson_mut_val *defaults, pc_error *err)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *norm = doc != NULL ? pc_algo_camera_raw_to_json(doc, p) : NULL;
    yyjson_mut_doc *nd = NULL;
    yyjson_mut_val *sorted = NULL;
    yyjson_mut_obj_iter it;
    yyjson_mut_val *k;
    pc_status st = PC_OK;
    if (norm != NULL) {
        nd = yyjson_mut_doc_new(NULL);
        sorted = nd != NULL ? pc_json_normalized_copy(nd, norm) : NULL;
    }
    if (sorted == NULL) {
        yyjson_mut_doc_free(doc);
        yyjson_mut_doc_free(nd);
        return pc_fail(err, PC_ERR_NOMEM, "cannot encode Camera Raw parameters");
    }
    yyjson_mut_obj_iter_init(sorted, &it);
    while ((k = yyjson_mut_obj_iter_next(&it)) != NULL) {
        const char *key = yyjson_mut_get_str(k);
        yyjson_mut_val *v = yyjson_mut_obj_iter_get_val(k);
        yyjson_mut_val *dv = key != NULL ? yyjson_mut_obj_get(defaults, key) : NULL;
        if (key != NULL && !supported_param(key) && (dv == NULL || !pc_json_mut_equal(dv, v))) {
            st = UNSUP(err, "Camera Raw setting `%s` has no verified PSD mapping", key);
            break;
        }
    }
    yyjson_mut_doc_free(doc);
    yyjson_mut_doc_free(nd);
    return st;
}

static pc_status patch(pc_psd_desc *d, const CR *p, const CR *original, pc_error *err)
{
    size_t i;
    size_t j;
    pc_status st;
    for (i = 0; i < NSCALARS; i++) {
        float value = *cfld(p, SCALARS[i].off);
        if (*cfld(original, SCALARS[i].off) == value) {
            continue;
        }
        st = write_number(d, SCALARS[i].key, value, err);
        if (st == PC_OK && (strcmp(SCALARS[i].key, "Temp") == 0 || strcmp(SCALARS[i].key, "Tint") == 0)) {
            pc_psd_value v;
            st = pc_psd_value_enum("WBal", "Cst ", &v, err);
            if (st == PC_OK) {
                st = set(d, "WBal", &v, err);
            }
        }
        if (st != PC_OK) {
            return st;
        }
    }
    for (i = 0; i < NCURVES; i++) {
        const pc_algo_points *c = ccurve_of(p, CURVES[i].off);
        if (points_eq(ccurve_of(original, CURVES[i].off), c)) {
            continue;
        }
        st = write_curve(d, &CURVES[i], c, err);
        if (st != PC_OK) {
            return st;
        }
    }
    for (i = 0; i < NARRAYS; i++) {
        for (j = 0; j < ARRAYS[i].n; j++) {
            float value = cfld(p, ARRAYS[i].off)[j];
            if (cfld(original, ARRAYS[i].off)[j] == value) {
                continue;
            }
            st = write_number(d, ARRAYS[i].keys[j], value, err);
            if (st != PC_OK) {
                return st;
            }
        }
    }
    return PC_OK;
}

pc_status pc_io_camera_raw_export_item(const pc_json_value *params, pc_psd_desc *out, pc_error *err)
{
    yyjson_mut_val *obj = pc_json_value_root(params);
    yyjson_mut_doc *ddoc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *defaults = ddoc != NULL ? defaults_json(ddoc) : NULL;
    pc_psd_desc item;
    pc_psd_desc d;
    const pc_psd_desc *f;
    const char *why;
    bool have_item = false;
    bool have_p = false;
    bool have_orig = false;
    bool ok = false;
    CR p;
    CR original;
    pc_status st;
    memset(&d, 0, sizeof d);
    if (defaults == NULL) {
        yyjson_mut_doc_free(ddoc);
        return pc_fail(err, PC_ERR_NOMEM, "cannot encode Camera Raw defaults");
    }
    st = precheck(obj, defaults, err);
    if (st == PC_OK) {
        st = template_item(yyjson_mut_obj_get(obj, TEMPLATE_KEY), &item, err);
        have_item = st == PC_OK;
    }
    if (st == PC_OK) {
        st = typed_params(obj, &p, err);
        have_p = st == PC_OK;
    }
    if (st == PC_OK) {
        st = check_unmapped(&p, defaults, err);
    }
    if (st == PC_OK && (why = splits_error(p.curve_splits)) != NULL) {
        st = UNSUP(err, "%s", why);
    }
    if (st == PC_OK) {
        f = desc_of(&item, "Fltr");
        if (f == NULL) {
            st = UNSUP(err, "Camera Raw PSD template has no filter settings");
        } else {
            st = pc_psd_desc_clone(f, &d, err);
        }
    }
    if (st == PC_OK) {
        st = descriptor_params(&d, &ok, &original, err);
        have_orig = st == PC_OK && ok;
        if (st == PC_OK && !ok) {
            st = UNSUP(err, "unsupported Camera Raw processing version or settings");
        }
    }
    if (st == PC_OK) {
        st = patch(&d, &p, &original, err);
    }
    if (st == PC_OK) {
        st = set_object(&item, "Fltr", &d, err);
    }
    yyjson_mut_doc_free(ddoc);
    pc_psd_desc_fini(&d);
    if (have_p) {
        pc_algo_camera_raw_fini(&p);
    }
    if (have_orig) {
        pc_algo_camera_raw_fini(&original);
    }
    if (st != PC_OK) {
        if (have_item) {
            pc_psd_desc_fini(&item);
        }
        return st;
    }
    *out = item;
    return PC_OK;
}
