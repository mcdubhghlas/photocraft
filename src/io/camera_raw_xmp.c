/* Camera Raw settings files (.xmp): the crs: namespace on rdf:Description (port of
 * crates/io/src/camera_raw_xmp.rs). Parsing reuses the SVG module's XML DOM (io/svg_internal.h:
 * sx_parse + sx_* accessors). Never panics on malformed input (REWRITE_PLAN.md rule 9). */
#include "io/camera_raw_xmp.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"
#include "io/svg_internal.h"

#define RDF_NS "http://www.w3.org/1999/02/22-rdf-syntax-ns#"

/* ---- field tables ------------------------------------------------------------------------- */

typedef struct scalar_def {
    const char *name;
    size_t off; /* byte offset of the float field in pc_algo_camera_raw */
    float lo, hi;
} scalar_def;

#define F(member) offsetof(pc_algo_camera_raw, member)

static const scalar_def SCALARS[] = {
    {"IncrementalTemperature", F(temperature), -100.0f, 100.0f},
    {"IncrementalTint", F(tint), -100.0f, 100.0f},
    {"Exposure2012", F(exposure), -5.0f, 5.0f},
    {"Contrast2012", F(contrast), -100.0f, 100.0f},
    {"Highlights2012", F(highlights), -100.0f, 100.0f},
    {"Shadows2012", F(shadows), -100.0f, 100.0f},
    {"Whites2012", F(whites), -100.0f, 100.0f},
    {"Blacks2012", F(blacks), -100.0f, 100.0f},
    {"Texture", F(texture), -100.0f, 100.0f},
    {"Clarity2012", F(clarity), -100.0f, 100.0f},
    {"Dehaze", F(dehaze), -100.0f, 100.0f},
    {"Vibrance", F(vibrance), -100.0f, 100.0f},
    {"Saturation", F(saturation), -100.0f, 100.0f},
    {"ParametricShadows", F(curve_shadows), -100.0f, 100.0f},
    {"ParametricDarks", F(curve_darks), -100.0f, 100.0f},
    {"ParametricLights", F(curve_lights), -100.0f, 100.0f},
    {"ParametricHighlights", F(curve_highlights), -100.0f, 100.0f},
    {"ParametricShadowSplit", F(curve_splits[0]), 0.0f, 100.0f},
    {"ParametricMidtoneSplit", F(curve_splits[1]), 0.0f, 100.0f},
    {"ParametricHighlightSplit", F(curve_splits[2]), 0.0f, 100.0f},
    {"HueAdjustmentRed", F(hsl_hue[0]), -100.0f, 100.0f},
    {"HueAdjustmentOrange", F(hsl_hue[1]), -100.0f, 100.0f},
    {"HueAdjustmentYellow", F(hsl_hue[2]), -100.0f, 100.0f},
    {"HueAdjustmentGreen", F(hsl_hue[3]), -100.0f, 100.0f},
    {"HueAdjustmentAqua", F(hsl_hue[4]), -100.0f, 100.0f},
    {"HueAdjustmentBlue", F(hsl_hue[5]), -100.0f, 100.0f},
    {"HueAdjustmentPurple", F(hsl_hue[6]), -100.0f, 100.0f},
    {"HueAdjustmentMagenta", F(hsl_hue[7]), -100.0f, 100.0f},
    {"SaturationAdjustmentRed", F(hsl_sat[0]), -100.0f, 100.0f},
    {"SaturationAdjustmentOrange", F(hsl_sat[1]), -100.0f, 100.0f},
    {"SaturationAdjustmentYellow", F(hsl_sat[2]), -100.0f, 100.0f},
    {"SaturationAdjustmentGreen", F(hsl_sat[3]), -100.0f, 100.0f},
    {"SaturationAdjustmentAqua", F(hsl_sat[4]), -100.0f, 100.0f},
    {"SaturationAdjustmentBlue", F(hsl_sat[5]), -100.0f, 100.0f},
    {"SaturationAdjustmentPurple", F(hsl_sat[6]), -100.0f, 100.0f},
    {"SaturationAdjustmentMagenta", F(hsl_sat[7]), -100.0f, 100.0f},
    {"LuminanceAdjustmentRed", F(hsl_lum[0]), -100.0f, 100.0f},
    {"LuminanceAdjustmentOrange", F(hsl_lum[1]), -100.0f, 100.0f},
    {"LuminanceAdjustmentYellow", F(hsl_lum[2]), -100.0f, 100.0f},
    {"LuminanceAdjustmentGreen", F(hsl_lum[3]), -100.0f, 100.0f},
    {"LuminanceAdjustmentAqua", F(hsl_lum[4]), -100.0f, 100.0f},
    {"LuminanceAdjustmentBlue", F(hsl_lum[5]), -100.0f, 100.0f},
    {"LuminanceAdjustmentPurple", F(hsl_lum[6]), -100.0f, 100.0f},
    {"LuminanceAdjustmentMagenta", F(hsl_lum[7]), -100.0f, 100.0f},
    {"SplitToningShadowHue", F(grade_shadows.hue), 0.0f, 360.0f},
    {"SplitToningShadowSaturation", F(grade_shadows.sat), 0.0f, 100.0f},
    {"ColorGradeShadowLum", F(grade_shadows.lum), -100.0f, 100.0f},
    {"ColorGradeMidtoneHue", F(grade_midtones.hue), 0.0f, 360.0f},
    {"ColorGradeMidtoneSat", F(grade_midtones.sat), 0.0f, 100.0f},
    {"ColorGradeMidtoneLum", F(grade_midtones.lum), -100.0f, 100.0f},
    {"SplitToningHighlightHue", F(grade_highlights.hue), 0.0f, 360.0f},
    {"SplitToningHighlightSaturation", F(grade_highlights.sat), 0.0f, 100.0f},
    {"ColorGradeHighlightLum", F(grade_highlights.lum), -100.0f, 100.0f},
    {"ColorGradeGlobalHue", F(grade_global.hue), 0.0f, 360.0f},
    {"ColorGradeGlobalSat", F(grade_global.sat), 0.0f, 100.0f},
    {"ColorGradeGlobalLum", F(grade_global.lum), -100.0f, 100.0f},
    {"ColorGradeBlending", F(grade_blending), 0.0f, 100.0f},
    {"SplitToningBalance", F(grade_balance), -100.0f, 100.0f},
    {"Sharpness", F(sharpen_amount), 0.0f, 150.0f},
    {"SharpenRadius", F(sharpen_radius), 0.5f, 3.0f},
    {"SharpenDetail", F(sharpen_detail), 0.0f, 100.0f},
    {"SharpenEdgeMasking", F(sharpen_masking), 0.0f, 100.0f},
    {"LuminanceSmoothing", F(noise_luminance), 0.0f, 100.0f},
    {"LuminanceNoiseReductionDetail", F(noise_luminance_detail), 0.0f, 100.0f},
    {"ColorNoiseReduction", F(noise_color), 0.0f, 100.0f},
    {"ColorNoiseReductionDetail", F(noise_color_detail), 0.0f, 100.0f},
    {"GrainAmount", F(grain_amount), 0.0f, 100.0f},
    {"GrainSize", F(grain_size), 0.0f, 100.0f},
    {"GrainFrequency", F(grain_roughness), 0.0f, 100.0f},
    {"PostCropVignetteAmount", F(vignette_amount), -100.0f, 100.0f},
    {"PostCropVignetteMidpoint", F(vignette_midpoint), 0.0f, 100.0f},
    {"PostCropVignetteRoundness", F(vignette_roundness), -100.0f, 100.0f},
    {"PostCropVignetteFeather", F(vignette_feather), 0.0f, 100.0f},
    {"PostCropVignetteHighlightContrast", F(vignette_highlights), 0.0f, 100.0f},
};
#define NSCALARS (sizeof SCALARS / sizeof SCALARS[0])

typedef struct curve_def {
    const char *name;
    size_t off; /* byte offset of the pc_algo_points field */
} curve_def;

static const curve_def CURVES[] = {
    {"ToneCurvePV2012", F(point_curve)},
    {"ToneCurvePV2012Red", F(point_curve_red)},
    {"ToneCurvePV2012Green", F(point_curve_green)},
    {"ToneCurvePV2012Blue", F(point_curve_blue)},
};
#define NCURVES (sizeof CURVES / sizeof CURVES[0])

static const char *const STYLE_NUMS[3] = {"1", "2", "3"};
static const char *const STYLE_NAMES[3] = {"highlightPriority", "colorPriority", "paintOverlay"};

/* Bookkeeping fields Camera Raw writes that carry no setting. */
static const char *const METADATA[] = {
    "Version",       "ProcessVersion",       "HasSettings",       "HasCrop",
    "WhiteBalance",  "ToneCurveName2012",    "CameraProfile",     "PresetType",
    "UUID",          "SupportsAmount",       "SupportsColor",     "SupportsMonochrome",
    "SupportsHighDynamicRange",              "SupportsNormalDynamicRange",
    "SupportsSceneReferred",                 "SupportsOutputReferred",
    "CameraModelRestriction",                "Copyright",         "ContactInfo",
    "Name",          "ShortName",            "SortName",          "Group",
    "Description",
};
#define NMETADATA (sizeof METADATA / sizeof METADATA[0])

/* ---- small text helpers ------------------------------------------------------------------ */

static bool is_ascii_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Trimmed span of s[0..len): [*start, *start + out_len). */
static size_t trim_span(const char *s, size_t len, size_t *start)
{
    size_t a = 0, b = len;
    while (a < b && is_ascii_ws(s[a])) {
        a++;
    }
    while (b > a && is_ascii_ws(s[b - 1])) {
        b--;
    }
    *start = a;
    return b - a;
}

/* Rust `s.trim().strip_prefix('+').parse::<f32>().ok().filter(is_finite)`. */
static bool parse_number(const char *s, float *out)
{
    size_t start;
    size_t n = trim_span(s, strlen(s), &start);
    const char *p = s + start;
    char tmp[64];
    char *end;
    double v;
    if (n > 0 && p[0] == '+') {
        p++;
        n--;
    }
    if (n == 0 || n >= sizeof tmp) {
        return false;
    }
    memcpy(tmp, p, n);
    tmp[n] = '\0';
    end = NULL;
    v = strtod(tmp, &end);
    if (end != tmp + n) {
        return false; /* trailing junk: Rust's parse requires the whole string */
    }
    if (!isfinite(v)) {
        return false;
    }
    *out = (float)v;
    return true;
}

static bool eq_ignore_case(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
    }
    return *a == *b;
}

static bool str_in(const char *s, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* An unsupported setting at its neutral value changes nothing, so it isn't reported. */
static bool neutral_value(const char *name, const char *value)
{
    size_t start;
    size_t n = trim_span(value, strlen(value), &start);
    char v[64];
    float num;
    bool have_num;
    if (n < sizeof v) {
        memcpy(v, value + start, n);
        v[n] = '\0';
    } else {
        v[0] = '\0';
    }
    have_num = parse_number(v, &num);
    if (eq_ignore_case(v, "false") || (have_num && num == 0.0f)) {
        return true;
    }
    if (strcmp(name, "GrainSeed") == 0) {
        return true;
    }
    if (strcmp(name, "VignetteMidpoint") == 0) {
        return have_num && num == 50.0f;
    }
    if (strcmp(name, "PerspectiveScale") == 0) {
        return have_num && num == 100.0f;
    }
    return strncmp(name, "Defringe", 8) == 0 && strstr(name, "Hue") != NULL;
}

/* ---- report ------------------------------------------------------------------------------ */

void pc_io_cr_xmp_report_fini(pc_io_cr_xmp_report *r)
{
    if (r == NULL) {
        return;
    }
    for (size_t i = 0; i < r->nignored; i++) {
        free(r->ignored[i]);
    }
    free(r->ignored);
    r->ignored = NULL;
    r->nignored = 0;
    r->applied = 0;
}

static pc_status report_ignore(pc_io_cr_xmp_report *r, const char *name, pc_error *err)
{
    char **grown = realloc(r->ignored, (r->nignored + 1) * sizeof *grown);
    char *copy;
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "camera raw settings");
    }
    r->ignored = grown;
    copy = pc_strndup(name, strlen(name));
    if (copy == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "camera raw settings");
    }
    r->ignored[r->nignored++] = copy;
    return PC_OK;
}

/* ---- apply ------------------------------------------------------------------------------- */

static bool set_vignette_style(pc_algo_camera_raw *p, const char *style)
{
    char *copy = pc_strndup(style, strlen(style));
    if (copy == NULL) {
        return false;
    }
    free(p->vignette_style);
    p->vignette_style = copy;
    return true;
}

static pc_status apply_setting(pc_algo_camera_raw *p, pc_io_cr_xmp_report *rep, const char *name,
                               const char *value, pc_error *err)
{
    for (size_t i = 0; i < NSCALARS; i++) {
        if (strcmp(SCALARS[i].name, name) == 0) {
            float v;
            if (parse_number(value, &v)) {
                float *field = (float *)((char *)p + SCALARS[i].off);
                *field = pc_clampf(v, SCALARS[i].lo, SCALARS[i].hi);
                rep->applied++;
                return PC_OK;
            }
            return report_ignore(rep, name, err);
        }
    }
    if (strcmp(name, "PostCropVignetteStyle") == 0) {
        size_t start;
        size_t n = trim_span(value, strlen(value), &start);
        for (size_t i = 0; i < 3; i++) {
            if (strlen(STYLE_NUMS[i]) == n && strncmp(value + start, STYLE_NUMS[i], n) == 0) {
                if (!set_vignette_style(p, STYLE_NAMES[i])) {
                    return pc_fail(err, PC_ERR_NOMEM, "camera raw settings");
                }
                rep->applied++;
                return PC_OK;
            }
        }
        return report_ignore(rep, name, err);
    }
    if (!str_in(name, METADATA, NMETADATA) && !neutral_value(name, value)) {
        return report_ignore(rep, name, err);
    }
    return PC_OK;
}

/* Set a pc_algo_points field from points (npts == 0 clears it). */
static bool set_points(pc_algo_points *c, const float (*pts)[2], size_t npts)
{
    float (*v)[2] = NULL;
    if (npts > 0) {
        v = malloc(npts * sizeof *v);
        if (v == NULL) {
            return false;
        }
        memcpy(v, pts, npts * sizeof *v);
    }
    free(c->v);
    c->v = v;
    c->n = npts;
    return true;
}

/* Parse a `<crs:ToneCurve...>` element: its rdf:Seq's "x, y" items as validated curve points.
 * Returns true (with *npts, possibly 0 for the linear curve) when usable; false otherwise. */
#define CURVE_SCRATCH 257
static bool parse_curve(const sx_node *node, float (*pts)[2], size_t *npts)
{
    const sx_node *seq = NULL;
    size_t n = 0;
    for (const sx_node *c = sx_first_element(node); c != NULL; c = sx_next_element(c)) {
        if (c->ns != NULL && strcmp(c->ns, RDF_NS) == 0 && c->name != NULL &&
            strcmp(c->name, "Seq") == 0) {
            seq = c;
            break;
        }
    }
    if (seq == NULL) {
        return false;
    }
    for (const sx_node *li = sx_first_element(seq); li != NULL; li = sx_next_element(li)) {
        if (li->name == NULL || strcmp(li->name, "li") != 0) {
            continue;
        }
        const char *text = sx_text(li);
        const char *comma;
        char xbuf[64];
        size_t xl;
        float x, y;
        if (text == NULL) {
            text = "";
        }
        comma = strchr(text, ',');
        if (comma == NULL) {
            return false;
        }
        xl = (size_t)(comma - text);
        if (xl >= sizeof xbuf) {
            return false;
        }
        memcpy(xbuf, text, xl);
        xbuf[xl] = '\0';
        if (!parse_number(xbuf, &x) || !parse_number(comma + 1, &y)) {
            return false;
        }
        if (n >= CURVE_SCRATCH) {
            return false; /* far more than validate_curve accepts */
        }
        pts[n][0] = x;
        pts[n][1] = y;
        n++;
    }
    if (pc_algo_cr_validate_curve((const float (*)[2])pts, n, NULL) != PC_OK) {
        return false;
    }
    /* Camera Raw's linear curve is the identity's two end points; PhotoCraft's is empty. */
    if (n == 2 && pts[0][0] == 0.0f && pts[0][1] == 0.0f && pts[1][0] == 255.0f && pts[1][1] == 255.0f) {
        n = 0;
    }
    *npts = n;
    return true;
}

/* Process one rdf:Description. *found is set true when a crs: attribute or child is seen. */
static pc_status process_description(const sx_node *desc, pc_algo_camera_raw *p,
                                     pc_io_cr_xmp_report *rep, bool *found, pc_error *err)
{
    pc_status st;
    for (size_t i = 0; i < desc->nattrs; i++) {
        const sx_attr *a = &desc->attrs[i];
        if (a->ns != NULL && strcmp(a->ns, PC_IO_CR_XMP_NS) == 0) {
            *found = true;
            st = apply_setting(p, rep, a->name, a->value != NULL ? a->value : "", err);
            if (st != PC_OK) {
                return st;
            }
        }
    }
    for (const sx_node *c = sx_first_element(desc); c != NULL; c = sx_next_element(c)) {
        if (c->ns == NULL || strcmp(c->ns, PC_IO_CR_XMP_NS) != 0 || c->name == NULL) {
            continue;
        }
        *found = true;
        const char *name = c->name;
        bool is_curve = false;
        for (size_t i = 0; i < NCURVES; i++) {
            if (strcmp(CURVES[i].name, name) == 0) {
                float scratch[CURVE_SCRATCH][2];
                size_t npts = 0;
                is_curve = true;
                if (parse_curve(c, scratch, &npts)) {
                    pc_algo_points *field = (pc_algo_points *)((char *)p + CURVES[i].off);
                    if (!set_points(field, (const float (*)[2])scratch, npts)) {
                        return pc_fail(err, PC_ERR_NOMEM, "camera raw settings");
                    }
                    rep->applied++;
                } else {
                    st = report_ignore(rep, name, err);
                    if (st != PC_OK) {
                        return st;
                    }
                }
                break;
            }
        }
        if (is_curve) {
            continue;
        }
        if (sx_first_element(c) != NULL) {
            /* Structured settings (masks, looks, lens profiles...) have no counterpart. */
            if (!str_in(name, METADATA, NMETADATA)) {
                st = report_ignore(rep, name, err);
                if (st != PC_OK) {
                    return st;
                }
            }
        } else {
            const char *text = sx_text(c);
            st = apply_setting(p, rep, name, text != NULL ? text : "", err);
            if (st != PC_OK) {
                return st;
            }
        }
    }
    return PC_OK;
}

static pc_status walk(const sx_node *node, pc_algo_camera_raw *p, pc_io_cr_xmp_report *rep,
                      bool *found, pc_error *err)
{
    pc_status st;
    if (node == NULL) {
        return PC_OK;
    }
    if (node->ns != NULL && strcmp(node->ns, RDF_NS) == 0 && node->name != NULL &&
        strcmp(node->name, "Description") == 0) {
        st = process_description(node, p, rep, found, err);
        if (st != PC_OK) {
            return st;
        }
    }
    for (const sx_node *c = sx_first_element(node); c != NULL; c = sx_next_element(c)) {
        st = walk(c, p, rep, found, err);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

static bool valid_utf8(const uint8_t *b, size_t len)
{
    size_t i = 0;
    uint32_t cp;
    while (i < len) {
        if (!pc_utf8_next(b, len, &i, &cp)) {
            return false;
        }
    }
    return true;
}

pc_status pc_io_camera_raw_xmp_load(pc_algo_camera_raw *params, const uint8_t *bytes, size_t len,
                                    pc_io_cr_xmp_report *report, pc_error *err)
{
    pc_io_cr_xmp_report rep = {0, NULL, 0};
    pc_algo_camera_raw next;
    sv_arena arena;
    sx_doc doc;
    char msg[256];
    const char *text;
    size_t text_len;
    bool found = false;
    pc_status st;

    if (len > PC_IO_CR_XMP_MAX_BYTES) {
        return pc_fail(err, PC_ERR_LIMIT, "the settings file is too large");
    }
    if (!valid_utf8(bytes, len)) {
        return pc_fail(err, PC_ERR_FORMAT, "not a Camera Raw settings file (not UTF-8 text)");
    }
    text = (const char *)bytes;
    text_len = len;
    /* Skip a UTF-8 byte order mark. */
    if (text_len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) {
        text += 3;
        text_len -= 3;
    }

    st = pc_algo_camera_raw_copy(&next, params, err);
    if (st != PC_OK) {
        return st;
    }

    sv_arena_init(&arena);
    msg[0] = '\0';
    st = sx_parse(&arena, text, text_len, &doc, msg, sizeof msg);
    if (st != PC_OK) {
        sv_arena_fini(&arena);
        pc_algo_camera_raw_fini(&next);
        return pc_fail(err, PC_ERR_FORMAT, "not a Camera Raw settings file (%s)",
                       msg[0] != '\0' ? msg : "malformed XML");
    }

    st = walk(sx_first_element(doc.root), &next, &rep, &found, err);
    sv_arena_fini(&arena);
    if (st != PC_OK) {
        pc_io_cr_xmp_report_fini(&rep);
        pc_algo_camera_raw_fini(&next);
        return st;
    }
    if (!found) {
        pc_io_cr_xmp_report_fini(&rep);
        pc_algo_camera_raw_fini(&next);
        return pc_fail(err, PC_ERR_FORMAT, "no Camera Raw settings in the file");
    }

    pc_algo_camera_raw_fini(params);
    *params = next;
    if (report != NULL) {
        *report = rep;
    } else {
        pc_io_cr_xmp_report_fini(&rep);
    }
    return PC_OK;
}

/* ---- save -------------------------------------------------------------------------------- */

/* Rust `signed(v, decimals)`: "%.*f", with a leading '+' for positive values. */
static void signed_dec(float v, int decimals, char *out, size_t cap)
{
    int n;
    if (!isfinite(v)) {
        (void)snprintf(out, cap, "%s", isnan(v) ? "NaN" : v < 0 ? "-inf" : "inf");
        return;
    }
    n = snprintf(out, cap, "%.*f", decimals, (double)v);
    if (n > 0 && v > 0.0f && out[0] != '+' && (size_t)n + 1 < cap) {
        memmove(out + 1, out, (size_t)n + 1);
        out[0] = '+';
    }
}

pc_status pc_io_camera_raw_xmp_save(const pc_algo_camera_raw *params, pc_buf *out, pc_error *err)
{
    pc_buf buf = PC_BUF_INIT;
    const char *wb = (params->temperature != 0.0f || params->tint != 0.0f) ? "Custom" : "As Shot";
    const char *style_num = "1";
    pc_status st;

    for (size_t i = 0; i < 3; i++) {
        if (strcmp(pc_algo_camera_raw_style(params), STYLE_NAMES[i]) == 0) {
            style_num = STYLE_NUMS[i];
            break;
        }
    }

    st = pc_buf_append_str(
        &buf,
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"PhotoCraft\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\"\n"
        "    xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n"
        "   crs:Version=\"18.4\"\n   crs:ProcessVersion=\"15.4\"\n",
        err);
    if (st == PC_OK) {
        st = pc_buf_appendf(&buf, err, "   crs:WhiteBalance=\"%s\"\n", wb);
    }
    for (size_t i = 0; i < NSCALARS && st == PC_OK; i++) {
        float v = *(const float *)((const char *)params + SCALARS[i].off);
        char text[64];
        if (strcmp(SCALARS[i].name, "Exposure2012") == 0) {
            signed_dec(v, 2, text, sizeof text);
        } else if (strcmp(SCALARS[i].name, "SharpenRadius") == 0) {
            signed_dec(v, 1, text, sizeof text);
        } else if (SCALARS[i].lo < 0.0f) {
            signed_dec(roundf(v), 0, text, sizeof text);
        } else {
            (void)snprintf(text, sizeof text, "%.0f", (double)roundf(v));
        }
        st = pc_buf_appendf(&buf, err, "   crs:%s=\"%s\"\n", SCALARS[i].name, text);
    }
    if (st == PC_OK) {
        st = pc_buf_appendf(&buf, err, "   crs:PostCropVignetteStyle=\"%s\"\n", style_num);
    }
    if (st == PC_OK) {
        st = pc_buf_append_str(&buf, "   crs:ToneCurveName2012=\"Custom\"\n   crs:HasSettings=\"True\">\n", err);
    }
    for (size_t i = 0; i < NCURVES && st == PC_OK; i++) {
        const pc_algo_points *c = (const pc_algo_points *)((const char *)params + CURVES[i].off);
        static const float linear[2][2] = {{0.0f, 0.0f}, {255.0f, 255.0f}};
        const float (*pts)[2] = c->n > 0 ? (const float (*)[2])c->v : linear;
        size_t npts = c->n > 0 ? c->n : 2;
        st = pc_buf_appendf(&buf, err, "   <crs:%s>\n    <rdf:Seq>\n", CURVES[i].name);
        for (size_t k = 0; k < npts && st == PC_OK; k++) {
            st = pc_buf_appendf(&buf, err, "     <rdf:li>%.0f, %.0f</rdf:li>\n", (double)roundf(pts[k][0]),
                                (double)roundf(pts[k][1]));
        }
        if (st == PC_OK) {
            st = pc_buf_appendf(&buf, err, "    </rdf:Seq>\n   </crs:%s>\n", CURVES[i].name);
        }
    }
    if (st == PC_OK) {
        st = pc_buf_append_str(&buf, "  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n", err);
    }
    if (st != PC_OK) {
        pc_buf_fini(&buf);
        return st;
    }
    pc_buf_fini(out);
    *out = buf;
    return PC_OK;
}
