/* Bit-exact parity with the Rust reference: tests/golden/doc-vectors.txt was written by
 * `rustref doc-vectors` (tools/rustref/src/doc_vectors.rs). One case per line, tab-separated:
 *
 *   ser  TYPE JSON          parse, T_from_json, T_to_json, compact write: must give JSON back
 *   serp TYPE JSON PRETTY   the same written with PC_JSON_PRETTY (newlines shown as U+00B6)
 *   serv TYPE JSON VALUE    to_json with PC_DOC_JSON_VALUE, written with sorted keys
 *   de   TYPE INPUT RESULT  from_value semantics: re-serialised result or "ERR <message>"
 *   fn   NAME ARGS... RES   logic vectors; floats as hex bits (f32 %08x, f64 %016x)
 *
 * Values with NaN/inf serialise as null, which from_json rightly rejects, so they are not in
 * the ser lines: the "nonfinite" fn lines build them here and check the serialisation only.
 * Error messages longer than pc_error's 255 bytes compare on their first 255 bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/json.h"
#include "doc/doc.h"
#include "doc/json.h"
#include "harness.h"

#define GOLDEN "tests/golden/doc-vectors.txt"
#define H0 0xcbf29ce484222325ull

/* ------------------------------------------------------------------------------------------ */
/* Type table: from_json / to_json / fini through void pointers. */

typedef struct tinfo {
    size_t size;
    pc_status (*from)(yyjson_val *v, void *out, pc_error *err);
    yyjson_mut_val *(*to)(yyjson_mut_doc *d, const void *v, unsigned flags);
    void (*fini)(void *p);
} tinfo;

#define NOFINI(p) ((void)(p))

#define DEFT(T, FINI)                                                                              \
    static pc_status T##_f(yyjson_val *v, void *o, pc_error *e) { return T##_from_json(v, (T *)o, e); } \
    static yyjson_mut_val *T##_t(yyjson_mut_doc *d, const void *v, unsigned fl)                    \
    {                                                                                              \
        return T##_to_json(d, (const T *)v, fl);                                                   \
    }                                                                                              \
    static void T##_x(void *p) { FINI((T *)p); }                                                   \
    static const tinfo TI_##T = {sizeof(T), T##_f, T##_t, T##_x};

static void shadow_fini(pc_shadow *s) { pc_contour_fini(&s->contour); }
static void glow_fini(pc_glow *g)
{
    pc_fx_paint_fini(&g->paint);
    pc_contour_fini(&g->contour);
}
static void stroke_fx_fini(pc_stroke_fx *s) { pc_fx_paint_fini(&s->paint); }
static void satin_fini(pc_satin *s) { pc_contour_fini(&s->contour); }
static void bevel_texture_fini(pc_bevel_texture *t)
{
    pc_str_fini(&t->name);
    pc_str_fini(&t->id);
}
static void bevel_contour_fini(pc_bevel_contour *c) { pc_contour_fini(&c->contour); }
static void bevel_fini(pc_bevel *b)
{
    pc_contour_fini(&b->gloss_contour);
    pc_contour_fini(&b->contour.contour);
    bevel_texture_fini(&b->texture);
}
static void font_feature_fini(pc_font_feature *f) { pc_str_fini(&f->tag); }
static void font_variation_fini(pc_font_variation *f) { pc_str_fini(&f->axis); }
static void text_run_fini(pc_text_run *r) { pc_char_style_fini(&r->style); }

DEFT(pc_size, NOFINI)
DEFT(pc_rect, NOFINI)
DEFT(pc_point, NOFINI)
DEFT(pc_affine, NOFINI)
DEFT(pc_sample_type, NOFINI)
DEFT(pc_color_mode, NOFINI)
DEFT(pc_pixel_format, NOFINI)
DEFT(pc_color, NOFINI)
DEFT(pc_blend_mode, NOFINI)
DEFT(pc_locks, NOFINI)
DEFT(pc_label_color, NOFINI)
DEFT(pc_fill, pc_fill_fini)
DEFT(pc_smart_filter, pc_smart_filter_fini)
DEFT(pc_guides, pc_guides_fini)
DEFT(pc_guide_color, NOFINI)
DEFT(pc_file_encoding, NOFINI)
DEFT(pc_color_indicates, NOFINI)
DEFT(pc_curve_point, NOFINI)
DEFT(pc_levels_channel, NOFINI)
DEFT(pc_tone_space, NOFINI)
DEFT(pc_hue_range, NOFINI)
DEFT(pc_adjustment, pc_adjustment_fini)
DEFT(pc_knockout, NOFINI)
DEFT(pc_advanced_blending, NOFINI)
DEFT(pc_blend_range, NOFINI)
DEFT(pc_blend_if, pc_blend_if_fini)
DEFT(pc_gradient_style, NOFINI)
DEFT(pc_gradient, pc_gradient_fini)
DEFT(pc_contour, pc_contour_fini)
DEFT(pc_fx_paint, pc_fx_paint_fini)
DEFT(pc_fx_common, NOFINI)
DEFT(pc_shadow, shadow_fini)
DEFT(pc_glow_technique, NOFINI)
DEFT(pc_glow_source, NOFINI)
DEFT(pc_glow, glow_fini)
DEFT(pc_stroke_position, NOFINI)
DEFT(pc_stroke_fx, stroke_fx_fini)
DEFT(pc_satin, satin_fini)
DEFT(pc_bevel_style, NOFINI)
DEFT(pc_bevel_technique, NOFINI)
DEFT(pc_bevel, bevel_fini)
DEFT(pc_bevel_contour, bevel_contour_fini)
DEFT(pc_bevel_texture, bevel_texture_fini)
DEFT(pc_effect, pc_effect_fini)
DEFT(pc_global_light, NOFINI)
DEFT(pc_color_table, pc_color_table_fini)
DEFT(pc_duotone_ink, pc_duotone_ink_fini)
DEFT(pc_duotone, pc_duotone_fini)
DEFT(pc_stack_mode, NOFINI)
DEFT(pc_knot, NOFINI)
DEFT(pc_path_op, NOFINI)
DEFT(pc_fill_rule, NOFINI)
DEFT(pc_subpath, pc_subpath_fini)
DEFT(pc_path, pc_path_fini)
DEFT(pc_clipping_path, pc_clipping_path_fini)
DEFT(pc_vector_mask, pc_vector_mask_fini)
DEFT(pc_stroke_align, NOFINI)
DEFT(pc_line_cap, NOFINI)
DEFT(pc_line_join, NOFINI)
DEFT(pc_shape_stroke, pc_shape_stroke_fini)
DEFT(pc_live_shape, NOFINI)
DEFT(pc_kerning, NOFINI)
DEFT(pc_caps, NOFINI)
DEFT(pc_font_feature, font_feature_fini)
DEFT(pc_font_variation, font_variation_fini)
DEFT(pc_char_style, pc_char_style_fini)
DEFT(pc_text_run, text_run_fini)
DEFT(pc_text_align, NOFINI)
DEFT(pc_text_direction, NOFINI)
DEFT(pc_paragraph_style, NOFINI)
DEFT(pc_paragraph_run, NOFINI)
DEFT(pc_text_shape, pc_text_shape_fini)
DEFT(pc_orientation, NOFINI)
DEFT(pc_antialias, NOFINI)
DEFT(pc_text_warp, pc_text_warp_fini)
DEFT(pc_character_style_def, pc_character_style_def_fini)
DEFT(pc_paragraph_style_def, pc_paragraph_style_def_fini)
DEFT(pc_text_styles, pc_text_styles_fini)
DEFT(pc_artboard_background, NOFINI)
DEFT(pc_artboard, pc_artboard_fini)
DEFT(pc_slice_origin, NOFINI)
DEFT(pc_slice_kind, NOFINI)
DEFT(pc_slice, pc_slice_fini)
DEFT(pc_slices, pc_slices_fini)
DEFT(pc_variables, pc_variables_fini)
DEFT(pc_variable_def, pc_variable_def_fini)
DEFT(pc_var_kind, NOFINI)
DEFT(pc_pixel_method, NOFINI)
DEFT(pc_pixel_align, NOFINI)
DEFT(pc_data_set, pc_data_set_fini)
DEFT(pc_data_value, pc_data_value_fini)
DEFT(pc_video_source, pc_video_source_fini)
DEFT(pc_timeline, NOFINI)
DEFT(pc_measurement_scale, pc_measurement_scale_fini)
DEFT(pc_count_group, pc_count_group_fini)
DEFT(pc_ruler, NOFINI)
DEFT(pc_measurement, pc_measurement_fini)
DEFT(pc_note, pc_note_fini)

static const struct {
    const char *name;
    const tinfo *t;
} TYPES[] = {
    {"Size", &TI_pc_size},
    {"Rect", &TI_pc_rect},
    {"Point", &TI_pc_point},
    {"Affine", &TI_pc_affine},
    {"SampleType", &TI_pc_sample_type},
    {"ColorMode", &TI_pc_color_mode},
    {"PixelFormat", &TI_pc_pixel_format},
    {"Color", &TI_pc_color},
    {"BlendMode", &TI_pc_blend_mode},
    {"Locks", &TI_pc_locks},
    {"LabelColor", &TI_pc_label_color},
    {"Fill", &TI_pc_fill},
    {"SmartFilter", &TI_pc_smart_filter},
    {"Guides", &TI_pc_guides},
    {"GuideColor", &TI_pc_guide_color},
    {"FileEncoding", &TI_pc_file_encoding},
    {"ColorIndicates", &TI_pc_color_indicates},
    {"CurvePoint", &TI_pc_curve_point},
    {"LevelsChannel", &TI_pc_levels_channel},
    {"ToneSpace", &TI_pc_tone_space},
    {"HueRange", &TI_pc_hue_range},
    {"Adjustment", &TI_pc_adjustment},
    {"Knockout", &TI_pc_knockout},
    {"AdvancedBlending", &TI_pc_advanced_blending},
    {"BlendRange", &TI_pc_blend_range},
    {"BlendIf", &TI_pc_blend_if},
    {"GradientStyle", &TI_pc_gradient_style},
    {"Gradient", &TI_pc_gradient},
    {"Contour", &TI_pc_contour},
    {"FxPaint", &TI_pc_fx_paint},
    {"FxCommon", &TI_pc_fx_common},
    {"Shadow", &TI_pc_shadow},
    {"GlowTechnique", &TI_pc_glow_technique},
    {"GlowSource", &TI_pc_glow_source},
    {"Glow", &TI_pc_glow},
    {"StrokePosition", &TI_pc_stroke_position},
    {"StrokeFx", &TI_pc_stroke_fx},
    {"Satin", &TI_pc_satin},
    {"BevelStyle", &TI_pc_bevel_style},
    {"BevelTechnique", &TI_pc_bevel_technique},
    {"Bevel", &TI_pc_bevel},
    {"BevelContour", &TI_pc_bevel_contour},
    {"BevelTexture", &TI_pc_bevel_texture},
    {"Effect", &TI_pc_effect},
    {"GlobalLight", &TI_pc_global_light},
    {"ColorTable", &TI_pc_color_table},
    {"DuotoneInk", &TI_pc_duotone_ink},
    {"Duotone", &TI_pc_duotone},
    {"StackMode", &TI_pc_stack_mode},
    {"Knot", &TI_pc_knot},
    {"PathOp", &TI_pc_path_op},
    {"FillRule", &TI_pc_fill_rule},
    {"Subpath", &TI_pc_subpath},
    {"Path", &TI_pc_path},
    {"ClippingPath", &TI_pc_clipping_path},
    {"VectorMask", &TI_pc_vector_mask},
    {"StrokeAlign", &TI_pc_stroke_align},
    {"LineCap", &TI_pc_line_cap},
    {"LineJoin", &TI_pc_line_join},
    {"ShapeStroke", &TI_pc_shape_stroke},
    {"LiveShape", &TI_pc_live_shape},
    {"Kerning", &TI_pc_kerning},
    {"Caps", &TI_pc_caps},
    {"FontFeature", &TI_pc_font_feature},
    {"FontVariation", &TI_pc_font_variation},
    {"CharStyle", &TI_pc_char_style},
    {"TextRun", &TI_pc_text_run},
    {"TextAlign", &TI_pc_text_align},
    {"TextDirection", &TI_pc_text_direction},
    {"ParagraphStyle", &TI_pc_paragraph_style},
    {"ParagraphRun", &TI_pc_paragraph_run},
    {"TextShape", &TI_pc_text_shape},
    {"Orientation", &TI_pc_orientation},
    {"AntiAlias", &TI_pc_antialias},
    {"TextWarp", &TI_pc_text_warp},
    {"CharacterStyleDef", &TI_pc_character_style_def},
    {"ParagraphStyleDef", &TI_pc_paragraph_style_def},
    {"TextStyles", &TI_pc_text_styles},
    {"ArtboardBackground", &TI_pc_artboard_background},
    {"Artboard", &TI_pc_artboard},
    {"SliceOrigin", &TI_pc_slice_origin},
    {"SliceKind", &TI_pc_slice_kind},
    {"Slice", &TI_pc_slice},
    {"Slices", &TI_pc_slices},
    {"Variables", &TI_pc_variables},
    {"VariableDef", &TI_pc_variable_def},
    {"VarKind", &TI_pc_var_kind},
    {"PixelMethod", &TI_pc_pixel_method},
    {"PixelAlign", &TI_pc_pixel_align},
    {"DataSet", &TI_pc_data_set},
    {"DataValue", &TI_pc_data_value},
    {"VideoSource", &TI_pc_video_source},
    {"Timeline", &TI_pc_timeline},
    {"MeasurementScale", &TI_pc_measurement_scale},
    {"CountGroup", &TI_pc_count_group},
    {"Ruler", &TI_pc_ruler},
    {"Measurement", &TI_pc_measurement},
    {"Note", &TI_pc_note},
};

static const tinfo *type_named(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof TYPES / sizeof TYPES[0]; i++) {
        if (strcmp(TYPES[i].name, name) == 0) {
            return TYPES[i].t;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* Per-line state: the output being built, scratch JSON documents. */

static pc_buf out;
static yyjson_mut_doc *md;
static yyjson_doc *pdocs[64];
static size_t npdocs;
static char **F; /* fields of the current line */
static size_t NF;
static size_t passes;
static size_t fails;

static void line_reset(void)
{
    size_t i;
    pc_buf_truncate(&out, 0);
    for (i = 0; i < npdocs; i++) {
        yyjson_doc_free(pdocs[i]);
    }
    npdocs = 0;
    yyjson_mut_doc_free(md);
    md = yyjson_mut_doc_new(NULL);
}

static void S(const char *s) { (void)pc_buf_append_str(&out, s, NULL); }
static void SN(const char *s, size_t n) { (void)pc_buf_append(&out, s, n, NULL); }
static void T(void) { S("\t"); }
static void TS(const char *s)
{
    T();
    S(s);
}
static void TB(bool b) { TS(b ? "true" : "false"); }
static void TU(uint64_t v) { (void)pc_buf_appendf(&out, NULL, "\t%llu", (unsigned long long)v); }
static void TI(int64_t v) { (void)pc_buf_appendf(&out, NULL, "\t%lld", (long long)v); }

static uint32_t bits32(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    return u;
}

static uint64_t bits64(double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    return u;
}

static void X32(float v) { (void)pc_buf_appendf(&out, NULL, "%08lx", (unsigned long)bits32(v)); }
static void TX32(float v)
{
    T();
    X32(v);
}
static void TX32S(const float *v, size_t n)
{
    size_t i;
    T();
    for (i = 0; i < n; i++) {
        if (i > 0) {
            S(",");
        }
        X32(v[i]);
    }
}
static void TX64(double v) { (void)pc_buf_appendf(&out, NULL, "\t%016llx", (unsigned long long)bits64(v)); }

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}

static void fnv_f32(uint64_t *h, float v)
{
    uint32_t u = bits32(v);
    uint8_t b[4] = {(uint8_t)u, (uint8_t)(u >> 8), (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
    fnv(h, b, 4);
}

static void THASH(uint64_t h) { (void)pc_buf_appendf(&out, NULL, "\t%016llx", (unsigned long long)h); }

static float P32(const char *s)
{
    uint32_t u = (uint32_t)strtoul(s, NULL, 16);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static double P64(const char *s)
{
    uint64_t u = (uint64_t)strtoull(s, NULL, 16);
    double f;
    memcpy(&f, &u, 8);
    return f;
}

static size_t P32S(const char *s, float *out_v, size_t max)
{
    size_t n = 0;
    while (*s != '\0' && n < max) {
        out_v[n++] = P32(s);
        while (*s != '\0' && *s != ',') {
            s++;
        }
        if (*s == ',') {
            s++;
        }
    }
    return n;
}

static uint64_t U(const char *s) { return (uint64_t)strtoull(s, NULL, 10); }
static int64_t I(const char *s) { return (int64_t)strtoll(s, NULL, 10); }

/* Writes a mutable value (NULL = out-of-memory marker). */
static void W(yyjson_mut_val *v, unsigned flags)
{
    pc_error e;
    if (v == NULL) {
        S("<to_json NULL>");
        return;
    }
    if (pc_json_write(v, flags, &out, &e) != PC_OK) {
        S("<write: ");
        S(e.msg);
        S(">");
    }
}

static void TJ(const tinfo *t, const void *v)
{
    T();
    W(t->to(md, v, 0), 0);
}

static void TERR(const pc_error *e)
{
    TS("CERR ");
    S(e->msg);
}

/* JSON string of s (serde escaping, via the writer). */
static void TJSTR(const char *s, size_t n)
{
    T();
    W(yyjson_mut_strncpy(md, s, n), 0);
}

static yyjson_val *parse(const char *s)
{
    yyjson_doc *d = NULL;
    pc_error e;
    if (pc_json_parse(s, strlen(s), &d, &e) != PC_OK) {
        return NULL;
    }
    if (npdocs < sizeof pdocs / sizeof pdocs[0]) {
        pdocs[npdocs++] = d;
    } else {
        yyjson_doc_free(d);
        return NULL;
    }
    return yyjson_doc_get_root(d);
}

/* Loads a value of type t from JSON text into zeroed memory at p. */
static bool load(const tinfo *t, const char *s, void *p)
{
    pc_error e;
    yyjson_val *v = parse(s);
    memset(p, 0, t->size);
    if (v == NULL) {
        S("<parse failed>");
        return false;
    }
    if (t->from(v, p, &e) != PC_OK) {
        S("<load failed: ");
        S(e.msg);
        S(">");
        return false;
    }
    return true;
}

/* Loads a JSON array of t into a malloc'd array. */
static bool load_arr(const tinfo *t, const char *s, void **arr, size_t *n)
{
    yyjson_val *v = parse(s);
    yyjson_val *it;
    size_t i, max;
    pc_error e;
    *arr = NULL;
    *n = 0;
    if (v == NULL || !yyjson_is_arr(v)) {
        S("<parse failed>");
        return false;
    }
    max = yyjson_arr_size(v);
    *arr = calloc(max > 0 ? max : 1, t->size);
    if (*arr == NULL) {
        return false;
    }
    yyjson_arr_foreach(v, i, max, it)
    {
        if (t->from(it, (char *)*arr + i * t->size, &e) != PC_OK) {
            S("<load failed: ");
            S(e.msg);
            S(">");
            return false;
        }
        (*n)++;
    }
    return true;
}

static void free_arr(const tinfo *t, void *arr, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        t->fini((char *)arr + i * t->size);
    }
    free(arr);
}

static yyjson_mut_val *arr_of(const tinfo *t, const void *base, size_t n)
{
    yyjson_mut_val *a = yyjson_mut_arr(md);
    size_t i;
    for (i = 0; a != NULL && i < n; i++) {
        yyjson_mut_val *x = t->to(md, (const char *)base + i * t->size, 0);
        if (x == NULL || !yyjson_mut_arr_append(a, x)) {
            return NULL;
        }
    }
    return a;
}

static void TARR(const tinfo *t, const void *base, size_t n)
{
    T();
    W(arr_of(t, base, n), 0);
}

static void TATTRS(const pc_style_attrs *a)
{
    yyjson_mut_val *r = pc_json_value_root(a);
    T();
    if (r == NULL) {
        S("{}");
    } else {
        W(r, 0);
    }
}

/* "-" or a number -> optional id pointer. */
static const uint32_t *OPTID(const char *s, uint32_t *slot)
{
    if (strcmp(s, "-") == 0) {
        return NULL;
    }
    *slot = (uint32_t)U(s);
    return slot;
}

static bool B(const char *s) { return strcmp(s, "true") == 0; }

/* Undo the generator's "\\0", "\\r", "\\n" escapes (into a malloc'd buffer, length in *n). */
static char *unesc(const char *s, size_t *n)
{
    size_t len = strlen(s), i, k = 0;
    char *o = malloc(len + 1);
    if (o == NULL) {
        *n = 0;
        return NULL;
    }
    for (i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len && (s[i + 1] == '0' || s[i + 1] == 'r' || s[i + 1] == 'n')) {
            o[k++] = s[i + 1] == '0' ? '\0' : s[i + 1] == 'r' ? '\r' : '\n';
            i++;
        } else {
            o[k++] = s[i];
        }
    }
    o[k] = '\0';
    *n = k;
    return o;
}

static void TESC0(const char *s, size_t n)
{
    size_t i;
    T();
    for (i = 0; i < n; i++) {
        if (s[i] == '\0') {
            S("\\0");
        } else {
            SN(s + i, 1);
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* ser / serp / serv / de */

static void do_ser(const char *kind)
{
    const tinfo *t = type_named(F[1]);
    void *v;
    bool pretty = strcmp(kind, "serp") == 0;
    bool value = strcmp(kind, "serv") == 0;
    size_t i, start;
    S(kind);
    TS(F[1]);
    T();
    if (pretty || value) {
        S(F[2]);
        T();
    }
    if (t == NULL) {
        S("<unknown type>");
        return;
    }
    v = calloc(1, t->size);
    if (v == NULL) {
        return;
    }
    start = out.len;
    if (load(t, F[2], v)) {
        if (value) {
            W(t->to(md, v, PC_DOC_JSON_VALUE), PC_JSON_SORT_KEYS);
        } else {
            W(t->to(md, v, 0), pretty ? PC_JSON_PRETTY : 0);
        }
        if (pretty) {
            /* Newlines -> U+00B6. */
            pc_buf tmp = PC_BUF_INIT;
            for (i = start; i < out.len; i++) {
                if (out.data[i] == '\n') {
                    (void)pc_buf_append(&tmp, "\xc2\xb6", 2, NULL);
                } else {
                    (void)pc_buf_append_byte(&tmp, out.data[i], NULL);
                }
            }
            pc_buf_truncate(&out, start);
            (void)pc_buf_append(&out, tmp.data, tmp.len, NULL);
            pc_buf_fini(&tmp);
        }
    }
    t->fini(v);
    free(v);
}

static void do_de(void)
{
    const tinfo *t = type_named(F[1]);
    yyjson_val *in;
    void *v;
    pc_error e;
    S("de");
    TS(F[1]);
    TS(F[2]);
    if (t == NULL) {
        TS("<unknown type>");
        return;
    }
    in = parse(F[2]);
    if (in == NULL) {
        TS("<input parse failed>");
        return;
    }
    v = calloc(1, t->size);
    if (v == NULL) {
        return;
    }
    if (t->from(in, v, &e) == PC_OK) {
        TJ(t, v);
    } else {
        TS("ERR ");
        S(e.msg);
        if (e.code != PC_ERR_INVALID) {
            S(" <code not INVALID>");
        }
    }
    t->fini(v);
    free(v);
}

/* ------------------------------------------------------------------------------------------ */
/* fn handlers. Each gets the line's fields (F[2..]) and appends "\t<result>" fields to out, which
 * already holds "fn\tNAME\t<args>". */

static void sweep_degrees(float *v, size_t *n)
{
    int i;
    size_t k = 0;
    for (i = -1440; i <= 1440; i++) {
        v[k++] = (float)i * 0.5f;
    }
    v[k++] = NAN;
    v[k++] = INFINITY;
    v[k++] = -INFINITY;
    v[k++] = -0.0f;
    v[k++] = 1e-7f;
    v[k++] = 359.99997f;
    v[k++] = 1e20f;
    v[k++] = -1e20f;
    v[k++] = P32("00000001");
    *n = k;
}

static pc_hue_range hue_with_bounds(const char *s)
{
    pc_hue_range r = {1.0f, 0.0f, 0.0f, {0, 0, 0, 0}};
    (void)P32S(s, r.bounds, 4);
    return r;
}

static void fn_hue_neutral(void)
{
    pc_hue_range r = pc_hue_range_neutral((size_t)U(F[2]));
    TJ(&TI_pc_hue_range, &r);
}

static void fn_hue_defaults(void)
{
    pc_hue_range r[6];
    pc_hue_range_defaults(r);
    TARR(&TI_pc_hue_range, r, 6);
}

static void fn_hue_weight_sweep(void)
{
    static float v[2900];
    size_t n, i;
    uint64_t h = H0;
    pc_hue_range r = hue_with_bounds(F[2]);
    sweep_degrees(v, &n);
    for (i = 0; i < n; i++) {
        fnv_f32(&h, pc_hue_range_weight(&r, v[i]));
    }
    THASH(h);
}

static void fn_hue_weight(void)
{
    pc_hue_range r = hue_with_bounds(F[2]);
    TX32(pc_hue_range_weight(&r, P32(F[3])));
}

static void fn_canonical_bounds(void)
{
    float b[4] = {0, 0, 0, 0}, o[4];
    (void)P32S(F[2], b, 4);
    pc_hue_range_canonical_bounds(b, o);
    TX32S(o, 4);
}

static void fn_hue_is_neutral(void)
{
    pc_hue_range r;
    if (load(&TI_pc_hue_range, F[2], &r)) {
        TB(pc_hue_range_is_neutral(&r));
    }
}

static void fn_blend_weight_sweep(void)
{
    pc_blend_range r, back;
    uint8_t b[4];
    uint64_t h = H0;
    int i;
    float extra[5] = {NAN, INFINITY, -INFINITY, 1e-7f, 254.99998f};
    if (!load(&TI_pc_blend_range, F[2], &r)) {
        return;
    }
    for (i = -40; i <= 1060; i++) {
        fnv_f32(&h, pc_blend_range_weight(r, (float)i * 0.25f));
    }
    for (i = 0; i < 5; i++) {
        fnv_f32(&h, pc_blend_range_weight(r, extra[i]));
    }
    THASH(h);
    TB(pc_blend_range_is_full(r));
    pc_blend_range_to_bytes(r, b);
    (void)pc_buf_appendf(&out, NULL, "\t[%u,%u,%u,%u]", b[0], b[1], b[2], b[3]);
    back = pc_blend_range_from_bytes(b);
    TJ(&TI_pc_blend_range, &back);
}

static void fn_blend_weight(void)
{
    pc_blend_range r;
    if (load(&TI_pc_blend_range, F[2], &r)) {
        TX32(pc_blend_range_weight(r, P32(F[3])));
    }
}

static void fn_blend_if_set(void)
{
    pc_blend_if b = {NULL, 0};
    const char *s = F[2];
    pc_error e;
    size_t i;
    yyjson_mut_val *gets = yyjson_mut_arr(md);
    while (*s != '\0') {
        char *end;
        size_t idx = (size_t)strtoull(s, &end, 10);
        unsigned v[8];
        pc_blend_range pair[2];
        int k;
        s = end;
        if (*s == ':') {
            s++;
        }
        for (k = 0; k < 8; k++) {
            v[k] = (unsigned)strtoul(s, &end, 10);
            s = end;
            if (*s == ',') {
                s++;
            }
        }
        if (*s == ';') {
            s++;
        }
        pair[0] = (pc_blend_range){{(uint8_t)v[0], (uint8_t)v[1]}, {(uint8_t)v[2], (uint8_t)v[3]}};
        pair[1] = (pc_blend_range){{(uint8_t)v[4], (uint8_t)v[5]}, {(uint8_t)v[6], (uint8_t)v[7]}};
        if (pc_blend_if_set(&b, idx, pair, &e) != PC_OK) {
            TERR(&e);
        }
    }
    TJ(&TI_pc_blend_if, &b);
    TB(pc_blend_if_is_default(&b));
    for (i = 0; i < 6; i++) {
        pc_blend_range g[2];
        pc_blend_if_get(&b, i, g);
        (void)yyjson_mut_arr_append(gets, arr_of(&TI_pc_blend_range, g, 2));
    }
    T();
    W(gets, 0);
    pc_blend_if_fini(&b);
}

static void fn_blend_if_is_default(void)
{
    pc_blend_if b;
    if (load(&TI_pc_blend_if, F[2], &b)) {
        TB(pc_blend_if_is_default(&b));
    }
    pc_blend_if_fini(&b);
}

static void fn_knockout(void)
{
    pc_knockout k;
    if (load(&TI_pc_knockout, F[2], &k)) {
        TU(pc_knockout_to_psd(k));
        TS(pc_knockout_name(k));
    }
}

static void fn_knockout_from_psd(void)
{
    pc_knockout k = pc_knockout_from_psd((uint8_t)U(F[2]));
    TJ(&TI_pc_knockout, &k);
}

static void fn_knockout_from_name(void)
{
    pc_knockout k;
    if (pc_knockout_from_name(F[2], strlen(F[2]), &k)) {
        TJ(&TI_pc_knockout, &k);
    } else {
        TS("None");
    }
}

static void fn_advanced_default(void)
{
    pc_advanced_blending a = pc_advanced_blending_default();
    TJ(&TI_pc_advanced_blending, &a);
    TB(pc_advanced_blending_is_default(&a));
}

static void fn_advanced_is_default(void)
{
    pc_advanced_blending a;
    if (load(&TI_pc_advanced_blending, F[2], &a)) {
        TB(pc_advanced_blending_is_default(&a));
    }
}

static void fn_lut3d_len(void)
{
    size_t n;
    if (pc_lut3d_len((uint32_t)U(F[2]), &n)) {
        TU(n);
    } else {
        TS("None");
    }
}

static void fn_is_identity_curve(void)
{
    void *a;
    size_t n;
    if (load_arr(&TI_pc_curve_point, F[2], &a, &n)) {
        TB(pc_curve_is_identity(a, n));
    }
    free_arr(&TI_pc_curve_point, a, n);
}

static void fn_adj_label(void)
{
    pc_adjustment a;
    if (load(&TI_pc_adjustment, F[2], &a)) {
        TS(pc_adjustment_label(&a));
    }
    pc_adjustment_fini(&a);
}

static void fn_adj_default(void)
{
    pc_adjustment a;
    pc_error e;
    memset(&a, 0, sizeof a);
    if (strcmp(F[2], "hue_saturation") == 0) {
        pc_adjustment_default_hue_saturation(&a);
    } else if (strcmp(F[2], "identity_curves") == 0) {
        if (pc_adjustment_identity_curves(&a, &e) != PC_OK) {
            TERR(&e);
            return;
        }
    } else {
        pc_adjustment_identity_levels(&a);
    }
    TJ(&TI_pc_adjustment, &a);
    pc_adjustment_fini(&a);
}

static pc_color colors1(void) { return pc_color_rgba(0.1f, 1e-7f, 1e20f, 0.5f); }

static pc_knot knots_at(int i)
{
    if (i == 0) {
        return pc_knot_corner(0.0, 0.0);
    }
    if (i == 1) {
        return pc_knot_smooth((pc_point){10.5, -3.25}, (pc_point){0.1, 1e-7}, (pc_point){1e20, -0.0});
    }
    return (pc_knot){{1.7976931348623157e308, 5e-324}, {1.0, 2.0}, {3.0, 4.0}, false};
}

static void fn_default(void)
{
    const char *n = F[2];
    pc_error e;
    if (strcmp(n, "LevelsChannel") == 0) {
        pc_levels_channel v = pc_levels_channel_default();
        TJ(&TI_pc_levels_channel, &v);
    } else if (strcmp(n, "drop_shadow") == 0) {
        pc_effect v;
        memset(&v, 0, sizeof v);
        pc_effect_default_drop_shadow(&v);
        TJ(&TI_pc_effect, &v);
        pc_effect_fini(&v);
    } else if (strcmp(n, "Gradient") == 0) {
        pc_gradient v;
        memset(&v, 0, sizeof v);
        if (pc_gradient_default(&v, &e) == PC_OK) {
            TJ(&TI_pc_gradient, &v);
            pc_gradient_fini(&v);
        } else {
            TERR(&e);
        }
    } else if (strcmp(n, "BevelContour") == 0) {
        pc_bevel_contour v = pc_bevel_contour_default();
        TJ(&TI_pc_bevel_contour, &v);
        bevel_contour_fini(&v);
    } else if (strcmp(n, "GlobalLight") == 0) {
        pc_global_light v = pc_global_light_default();
        TJ(&TI_pc_global_light, &v);
    } else if (strcmp(n, "FxCommon") == 0) {
        pc_fx_common v = pc_fx_common_new(PC_BLEND_OVERLAY, 0.25f);
        TJ(&TI_pc_fx_common, &v);
    } else if (strcmp(n, "ShapeStroke") == 0) {
        pc_shape_stroke v;
        memset(&v, 0, sizeof v);
        pc_shape_stroke_default(&v);
        TJ(&TI_pc_shape_stroke, &v);
        pc_shape_stroke_fini(&v);
    } else if (strcmp(n, "CharStyle") == 0) {
        pc_char_style v;
        pc_char_style_default(&v);
        TJ(&TI_pc_char_style, &v);
        pc_char_style_fini(&v);
    } else if (strcmp(n, "ParagraphStyle") == 0) {
        pc_paragraph_style v = pc_paragraph_style_default();
        TJ(&TI_pc_paragraph_style, &v);
    } else if (strcmp(n, "TextStyles") == 0) {
        pc_text_styles v;
        memset(&v, 0, sizeof v);
        if (pc_text_styles_default(&v, &e) == PC_OK) {
            TJ(&TI_pc_text_styles, &v);
            pc_text_styles_fini(&v);
        } else {
            TERR(&e);
        }
    } else if (strcmp(n, "MeasurementScale") == 0) {
        pc_measurement_scale v;
        memset(&v, 0, sizeof v);
        if (pc_measurement_scale_default(&v, &e) == PC_OK) {
            TJ(&TI_pc_measurement_scale, &v);
            pc_measurement_scale_fini(&v);
        } else {
            TERR(&e);
        }
    } else if (strcmp(n, "CountGroup") == 0) {
        pc_count_group v;
        memset(&v, 0, sizeof v);
        if (pc_count_group_default(&v, &e) == PC_OK) {
            TJ(&TI_pc_count_group, &v);
            pc_count_group_fini(&v);
        } else {
            TERR(&e);
        }
    } else if (strcmp(n, "Note") == 0) {
        pc_note v;
        pc_note_default(&v);
        TJ(&TI_pc_note, &v);
        pc_note_fini(&v);
    } else if (strcmp(n, "Measurement") == 0) {
        pc_measurement v;
        memset(&v, 0, sizeof v);
        if (pc_measurement_default(&v, &e) == PC_OK) {
            TJ(&TI_pc_measurement, &v);
            pc_measurement_fini(&v);
        } else {
            TERR(&e);
        }
    } else if (strcmp(n, "AdvancedBlending") == 0) {
        pc_advanced_blending v = pc_advanced_blending_default();
        TJ(&TI_pc_advanced_blending, &v);
    } else if (strcmp(n, "Slice") == 0) {
        pc_slice v;
        pc_slice_default(&v);
        TJ(&TI_pc_slice, &v);
        pc_slice_fini(&v);
    } else if (strcmp(n, "DuotoneInk") == 0) {
        pc_duotone_ink v;
        const float c[3] = {0.1f, 0.2f, 0.3f};
        memset(&v, 0, sizeof v);
        if (pc_duotone_ink_new("Ink 1", c, &v, &e) == PC_OK) {
            TJ(&TI_pc_duotone_ink, &v);
            pc_duotone_ink_fini(&v);
        } else {
            TERR(&e);
        }
    } else if (strcmp(n, "VectorMask") == 0) {
        pc_vector_mask v;
        pc_path p;
        memset(&p, 0, sizeof p);
        pc_vector_mask_init(&v, &p);
        TJ(&TI_pc_vector_mask, &v);
        pc_vector_mask_fini(&v);
    } else if (strcmp(n, "Artboard") == 0) {
        pc_artboard v;
        pc_artboard_init(&v, (pc_rect){1, 2, 3, 4});
        TJ(&TI_pc_artboard, &v);
        pc_artboard_fini(&v);
    } else if (strcmp(n, "FillGradient") == 0) {
        pc_color_stop st[2] = {{0.0f, PC_COLOR_BLACK}, {0.5f, colors1()}};
        pc_fill v;
        memset(&v, 0, sizeof v);
        if (pc_fill_gradient(st, 2, 45.0f, 0.5f, PC_GRADIENT_ANGLE, true, &v, &e) == PC_OK) {
            TJ(&TI_pc_fill, &v);
            pc_fill_fini(&v);
        } else {
            TERR(&e);
        }
    } else if (strcmp(n, "Knots") == 0) {
        pc_knot k[3] = {knots_at(0), knots_at(1), knots_at(2)};
        TARR(&TI_pc_knot, k, 3);
    } else {
        TS("<unknown default>");
    }
}

static void fn_ct_nearest(void)
{
    pc_color_table t;
    float c[3] = {0, 0, 0};
    if (load(&TI_pc_color_table, F[2], &t)) {
        (void)P32S(F[3], c, 3);
        TU(pc_color_table_nearest(&t, c));
    }
    pc_color_table_fini(&t);
}

static void fn_ct_rgb(void)
{
    pc_color_table t;
    float c[3];
    if (load(&TI_pc_color_table, F[2], &t)) {
        pc_color_table_rgb(&t, (size_t)U(F[3]), c);
        TX32S(c, 3);
    }
    pc_color_table_fini(&t);
}

static size_t ink_xs(float *v)
{
    size_t k = 0;
    int i;
    for (i = -10; i <= 110; i++) {
        v[k++] = (float)i / 100.0f;
    }
    v[k++] = NAN;
    v[k++] = INFINITY;
    v[k++] = -0.0f;
    v[k++] = 0.2000001f;
    v[k++] = 1e-7f;
    return k;
}

static void fn_ink_density_sweep(void)
{
    pc_duotone_ink ink;
    float xs[130];
    size_t n = ink_xs(xs), i;
    uint64_t h = H0;
    if (load(&TI_pc_duotone_ink, F[2], &ink)) {
        for (i = 0; i < n; i++) {
            fnv_f32(&h, pc_duotone_ink_density(&ink, xs[i]));
        }
        THASH(h);
    }
    pc_duotone_ink_fini(&ink);
}

static void fn_ink_density(void)
{
    pc_duotone_ink ink;
    if (load(&TI_pc_duotone_ink, F[2], &ink)) {
        TX32(pc_duotone_ink_density(&ink, P32(F[3])));
    }
    pc_duotone_ink_fini(&ink);
}

static void fn_duotone_render_sweep(void)
{
    pc_duotone d;
    float xs[130], c[3];
    size_t n = ink_xs(xs), i;
    uint64_t h = H0;
    if (load(&TI_pc_duotone, F[2], &d)) {
        for (i = 0; i < n; i++) {
            pc_duotone_render(&d, xs[i], c);
            fnv_f32(&h, c[0]);
            fnv_f32(&h, c[1]);
            fnv_f32(&h, c[2]);
        }
        THASH(h);
    }
    pc_duotone_fini(&d);
}

static void fn_duotone_render(void)
{
    pc_duotone d;
    float c[3];
    if (load(&TI_pc_duotone, F[2], &d)) {
        pc_duotone_render(&d, P32(F[3]), c);
        TX32S(c, 3);
    }
    pc_duotone_fini(&d);
}

static void fn_duotone_display(void)
{
    pc_duotone d;
    pc_adjustment a;
    pc_error e;
    memset(&a, 0, sizeof a);
    if (load(&TI_pc_duotone, F[2], &d)) {
        if (pc_duotone_display_adjustment(&d, &a, &e) == PC_OK) {
            TJ(&TI_pc_adjustment, &a);
        } else {
            TERR(&e);
        }
    }
    pc_adjustment_fini(&a);
    pc_duotone_fini(&d);
}

static void fn_stack_mode(void)
{
    pc_stack_mode m = (pc_stack_mode)U(F[2]), back;
    const char *id = pc_stack_mode_id(m);
    TS(id);
    TS(pc_stack_mode_label(m));
    TJ(&TI_pc_stack_mode, &m);
    if (pc_stack_mode_from_id(id, strlen(id), &back)) {
        TJ(&TI_pc_stack_mode, &back);
    } else {
        TS("None");
    }
}

static void fn_stack_mode_from_id(void)
{
    pc_stack_mode m;
    if (pc_stack_mode_from_id(F[2], strlen(F[2]), &m)) {
        TJ(&TI_pc_stack_mode, &m);
    } else {
        TS("None");
    }
}

static void fn_label_color(void)
{
    pc_label_color c = PC_LABEL_COLOR_ALL[U(F[2]) % PC_LABEL_COLOR_COUNT], back;
    const char *id = pc_label_color_id(c);
    TJ(&TI_pc_label_color, &c);
    TS(id);
    TS(pc_label_color_label(c));
    if (pc_label_color_from_id(id, strlen(id), &back)) {
        TJ(&TI_pc_label_color, &back);
    } else {
        TS("None");
    }
}

static void fn_label_from_id(void)
{
    pc_label_color c;
    if (pc_label_color_from_id(F[2], strlen(F[2]), &c)) {
        TJ(&TI_pc_label_color, &c);
    } else {
        TS("None");
    }
}

static void fn_slice_codes(void)
{
    uint32_t c = (uint32_t)U(F[2]);
    pc_slice_origin o = pc_slice_origin_from_code(c);
    pc_slice_kind k = pc_slice_kind_from_code(c);
    TJ(&TI_pc_slice_origin, &o);
    TU(pc_slice_origin_code(o));
    TS(pc_slice_origin_id(o));
    TJ(&TI_pc_slice_kind, &k);
    TU(pc_slice_kind_code(k));
    TS(pc_slice_kind_id(k));
}

static void fn_slice_kind_from_id(void)
{
    pc_slice_kind k;
    if (pc_slice_kind_from_id(F[2], strlen(F[2]), &k)) {
        TJ(&TI_pc_slice_kind, &k);
    } else {
        TS("None");
    }
}

static void fn_effect_label(void)
{
    pc_effect fx;
    if (load(&TI_pc_effect, F[2], &fx)) {
        TS(pc_effect_label(&fx));
        TB(pc_effect_enabled(&fx));
    }
    pc_effect_fini(&fx);
}

static void fn_text_align_justified(void)
{
    pc_text_align a;
    if (load(&TI_pc_text_align, F[2], &a)) {
        TB(pc_text_align_is_justified(a));
    }
}

static void fn_content_kind(void)
{
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    c.kind = (pc_layer_kind)U(F[2]);
    TS(pc_layer_content_kind_name(&c));
    TS(pc_layer_content_article(&c));
}

static void fn_artboard_bg(void)
{
    pc_artboard_background b;
    float c[4];
    if (load(&TI_pc_artboard_background, F[2], &b)) {
        if (pc_artboard_background_rgba(&b, c)) {
            TX32S(c, 4);
        } else {
            TS("None");
        }
        TI(pc_artboard_background_psd_type(&b));
        TS(pc_artboard_background_name(&b));
    }
}

static void fn_comp_info(void)
{
    pc_layer_comp c;
    memset(&c, 0, sizeof c);
    pc_layer_comp_set_captured_info(&c, (int32_t)I(F[2]));
    TI(pc_layer_comp_captured_info(&c));
    TB(c.apply_visibility);
    TB(c.apply_position);
    TB(c.apply_appearance);
}

static void fn_overlay_coverage(void)
{
    pc_alpha_channel a;
    memset(&a, 0, sizeof a);
    a.has_spot = B(F[2]);
    a.indicates = B(F[3]) ? PC_INDICATES_SELECTED_AREAS : PC_INDICATES_MASKED_AREAS;
    a.opacity = 0.5f;
    TX32(pc_alpha_channel_overlay_coverage(&a, P32(F[4])));
}

static void fn_locks_union(void)
{
    pc_locks a, b, c;
    if (load(&TI_pc_locks, F[2], &a) && load(&TI_pc_locks, F[3], &b)) {
        c = pc_locks_union(a, b);
        TJ(&TI_pc_locks, &c);
    }
}

static void fn_mask_value(void)
{
    pc_layer_mask m;
    pc_error e;
    static const int32_t pts[5][2] = {{0, 0}, {2, 1}, {3, 1}, {-1, 0}, {1000, 1000}};
    float vals[20];
    size_t i;
    if (B(F[2])) {
        pc_layer_mask_reveal_all(&m);
    } else {
        pc_layer_mask_hide_all(&m);
    }
    m.enabled = B(F[3]);
    m.density = P32(F[4]);
    if (strcmp(F[5], "-") != 0) {
        float v = P32(F[5]);
        if (pc_surface_fill_rect(&m.surface, (pc_rect){0, 0, 3, 2}, &v, 1, &e) != PC_OK) {
            TERR(&e);
        }
    }
    T();
    for (i = 0; i < 5; i++) {
        if (i > 0) {
            S(",");
        }
        X32(pc_layer_mask_value(&m, pts[i][0], pts[i][1]));
    }
    if (pc_layer_mask_values_into(&m, (pc_rect){-1, -1, 4, 3}, vals, 20, &e) == PC_OK) {
        TX32S(vals, 20);
    } else {
        TERR(&e);
    }
    pc_layer_mask_fini(&m);
}

static const pc_affine AFF = {{2.0, 0.5, -1.0, 3.0, 10.0, -20.0}};

static void fn_path(void)
{
    pc_path p, t;
    pc_index_range *r = NULL;
    size_t n = 0, i;
    double b[4];
    pc_error e;
    memset(&t, 0, sizeof t);
    if (!load(&TI_pc_path, F[2], &p)) {
        pc_path_fini(&p);
        return;
    }
    if (pc_path_components(&p, &r, &n, &e) != PC_OK) {
        TERR(&e);
    } else {
        T();
        for (i = 0; i < n; i++) {
            (void)pc_buf_appendf(&out, NULL, "%s%zu..%zu", i > 0 ? "," : "", r[i].start, r[i].end);
        }
    }
    free(r);
    T();
    for (i = 0; i < p.nsubpaths; i++) {
        pc_path_op op = pc_path_effective_op(&p, i);
        if (i > 0) {
            S(",");
        }
        W(pc_path_op_to_json(md, &op, 0), 0);
    }
    if (pc_path_control_bounds(&p, b)) {
        (void)pc_buf_appendf(&out, NULL, "\t%016llx,%016llx,%016llx,%016llx",
                             (unsigned long long)bits64(b[0]), (unsigned long long)bits64(b[1]),
                             (unsigned long long)bits64(b[2]), (unsigned long long)bits64(b[3]));
    } else {
        TS("None");
    }
    TB(pc_path_is_empty(&p));
    if (pc_path_transform(&p, AFF, &t, &e) == PC_OK) {
        TJ(&TI_pc_path, &t);
    } else {
        TERR(&e);
    }
    pc_path_fini(&t);
    pc_path_fini(&p);
}

static void fn_subpath_segments(void)
{
    pc_subpath s;
    size_t n, i;
    yyjson_mut_val *a = yyjson_mut_arr(md);
    if (load(&TI_pc_subpath, F[2], &s)) {
        n = pc_subpath_segment_count(&s);
        TU(n);
        for (i = 0; i < n; i++) {
            pc_point seg[4];
            pc_subpath_segment(&s, i, seg);
            (void)yyjson_mut_arr_append(a, arr_of(&TI_pc_point, seg, 4));
        }
        T();
        W(a, 0);
    }
    pc_subpath_fini(&s);
}

static void fn_subpath_poly(void)
{
    yyjson_val *v = parse(F[2]), *it;
    double(*pts)[2];
    size_t i, max, n = 0;
    pc_subpath a, b;
    pc_error e;
    if (v == NULL) {
        return;
    }
    max = yyjson_arr_size(v);
    pts = calloc(max > 0 ? max : 1, sizeof *pts);
    if (pts == NULL) {
        return;
    }
    yyjson_arr_foreach(v, i, max, it)
    {
        pts[n][0] = yyjson_get_num(yyjson_arr_get(it, 0));
        pts[n][1] = yyjson_get_num(yyjson_arr_get(it, 1));
        n++;
    }
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    if (pc_subpath_polygon((const double(*)[2])pts, n, &a, &e) == PC_OK) {
        TJ(&TI_pc_subpath, &a);
    } else {
        TERR(&e);
    }
    if (pc_subpath_polyline((const double(*)[2])pts, n, &b, &e) == PC_OK) {
        TJ(&TI_pc_subpath, &b);
    } else {
        TERR(&e);
    }
    pc_subpath_fini(&a);
    pc_subpath_fini(&b);
    free(pts);
}

static void fn_knot_transform(void)
{
    pc_knot k, t;
    if (load(&TI_pc_knot, F[2], &k)) {
        t = pc_knot_transform(k, AFF);
        TJ(&TI_pc_knot, &t);
    }
}

/* Patterns made by the pattern_new lines, for pattern_find. */
static pc_pattern pats[16];
static size_t npats;

static void fn_pattern_new(void)
{
    size_t nlen, dl;
    char *name = unesc(F[2], &nlen);
    pc_pixel_format f;
    pc_rect r;
    float px[8];
    size_t npx = P32S(F[7], px, 8);
    pc_surface s;
    pc_pattern p;
    pc_error e;
    const char *dn;
    pc_rect pr;
    if (name == NULL || !load(&TI_pc_pixel_format, F[3], &f) || !load(&TI_pc_rect, F[6], &r)) {
        free(name);
        return;
    }
    pc_surface_init(&s, f);
    if (pc_surface_fill_rect(&s, r, px, npx, &e) != PC_OK) {
        TERR(&e);
    }
    if (pc_pattern_new(name, nlen, &s, (uint32_t)U(F[4]), (uint32_t)U(F[5]), &p, &e) != PC_OK) {
        TERR(&e);
        pc_surface_fini(&s);
        free(name);
        return;
    }
    TS(pc_str_c(&p.id));
    dn = pc_pattern_display_name(p.name.ptr != NULL ? p.name.ptr : "", p.name.len, &dl);
    TESC0(dn, dl);
    pr = pc_pattern_rect(&p);
    TJ(&TI_pc_rect, &pr);
    TB(pc_pattern_is_empty(&p));
    if (npats < 16) {
        pats[npats++] = p;
    } else {
        pc_pattern_fini(&p);
    }
    free(name);
}

static void fn_pattern_display_name(void)
{
    size_t n, dl;
    char *s = unesc(F[2], &n);
    const char *d;
    if (s == NULL) {
        return;
    }
    d = pc_pattern_display_name(s, n, &dl);
    TESC0(d, dl);
    free(s);
}

static void fn_pattern_find(void)
{
    size_t nlen;
    char *name = unesc(F[3], &nlen);
    const char *id = F[2];
    size_t idlen = strlen(id);
    const pc_pattern *p;
    if (name == NULL) {
        return;
    }
    if (id[0] == '#') {
        size_t i = (size_t)U(id + 1);
        id = i < npats ? pc_str_c(&pats[i].id) : "";
        idlen = i < npats ? pats[i].id.len : 0;
    }
    p = pc_pattern_find(pats, npats, id, idlen, name, nlen);
    if (p == NULL) {
        TS("None");
    } else {
        TU((uint64_t)(p - pats));
    }
    free(name);
}

static void fn_scale(void)
{
    pc_measurement_scale s;
    pc_str d = PC_STR_INIT;
    pc_error e;
    memset(&s, 0, sizeof s);
    s.pixel_length = P64(F[2]);
    s.logical_length = P64(F[3]);
    if (pc_str_set_c(&s.units, F[4], &e) != PC_OK) {
        return;
    }
    if (pc_measurement_scale_describe(&s, &d, &e) == PC_OK) {
        TS(pc_str_c(&d));
    } else {
        TERR(&e);
    }
    TX64(pc_measurement_scale_factor(&s));
    TB(pc_measurement_scale_is_default(&s));
    pc_str_fini(&d);
    pc_measurement_scale_fini(&s);
}

static void fn_measurement(void)
{
    pc_measurement m;
    if (load(&TI_pc_measurement, F[2], &m)) {
        TU(pc_measurement_count_total(&m));
        TB(pc_measurement_is_empty(&m));
    }
    pc_measurement_fini(&m);
}

static void fn_timeline_new(void)
{
    pc_timeline t = pc_timeline_new((size_t)U(F[2]), P32(F[3]));
    TJ(&TI_pc_timeline, &t);
}

static void fn_timeline_clamp(void)
{
    pc_timeline t;
    const char *s = F[2];
    float fps = P32(s);
    uint64_t v[4];
    int k;
    s = strchr(s, ',');
    for (k = 0; k < 4 && s != NULL; k++) {
        v[k] = U(s + 1);
        s = strchr(s + 1, ',');
    }
    t = (pc_timeline){fps, (size_t)v[0], (size_t)v[1], (size_t)v[2], (size_t)v[3]};
    TX32(pc_timeline_time(&t));
    pc_timeline_clamp(&t);
    TJ(&TI_pc_timeline, &t);
    TX32(pc_timeline_time(&t));
}

static void fn_timeline_step(void)
{
    pc_timeline t = {30.0f, (size_t)U(F[2]), (size_t)U(F[3]), 0, 1};
    const char *s = F[4];
    bool first = true;
    T();
    while (s != NULL && *s != '\0') {
        pc_timeline_step(&t, I(s));
        (void)pc_buf_appendf(&out, NULL, "%s%zu", first ? "" : ",", t.current);
        first = false;
        s = strchr(s, ',');
        if (s != NULL) {
            s++;
        }
    }
}

static void fn_timeline_time(void)
{
    pc_timeline t = {P32(F[2]), 100, (size_t)U(F[3]), 0, 1};
    TX32(pc_timeline_time(&t));
}

static bool text_layer_from(pc_text_layer *t, const char *runs, const char *paras)
{
    void *r, *p;
    size_t nr, np;
    pc_text_layer_default(t);
    t->size_pt = 9.0f;
    t->color = pc_color_rgb(0.1f, 0.2f, 0.3f);
    if (pc_str_set_c(&t->font_family, "Fam", NULL) != PC_OK) {
        return false;
    }
    if (!load_arr(&TI_pc_text_run, runs, &r, &nr)) {
        free_arr(&TI_pc_text_run, r, nr);
        return false;
    }
    t->runs = r;
    t->nruns = nr;
    if (paras != NULL) {
        if (!load_arr(&TI_pc_paragraph_run, paras, &p, &np)) {
            free_arr(&TI_pc_paragraph_run, p, np);
            return false;
        }
        t->paragraphs = p;
        t->nparagraphs = np;
    }
    return true;
}

static void fn_text_runs(void)
{
    pc_text_layer t;
    size_t n, nc = 0, np = 0;
    char *text = unesc(F[2], &n);
    pc_text_run *cr = NULL;
    pc_paragraph_run *pr = NULL;
    pc_error e;
    if (text == NULL) {
        return;
    }
    if (text_layer_from(&t, F[3], F[4]) && pc_str_set(&t.text, text, n, &e) == PC_OK) {
        if (pc_text_layer_char_runs(&t, &cr, &nc, &e) == PC_OK) {
            TARR(&TI_pc_text_run, cr, nc);
        } else {
            TERR(&e);
        }
        if (pc_text_layer_paragraph_runs(&t, &pr, &np, &e) == PC_OK) {
            TARR(&TI_pc_paragraph_run, pr, np);
        } else {
            TERR(&e);
        }
    }
    pc_text_runs_free(cr, nc);
    free(pr);
    pc_text_layer_fini(&t);
    free(text);
}

static void fn_text_sync_summary(void)
{
    pc_text_layer t;
    pc_error e;
    if (text_layer_from(&t, F[2], NULL)) {
        if (pc_text_layer_sync_summary(&t, &e) == PC_OK) {
            TS(pc_str_c(&t.font_family));
            TX32(t.size_pt);
            TJ(&TI_pc_color, &t.color);
        } else {
            TERR(&e);
        }
    }
    pc_text_layer_fini(&t);
}

/* Text styles: S0..S3 from ts_styles, runs and paragraphs from ts_run / ts_para. */
static pc_text_styles STY[4];
static pc_char_style RUNS[4];
static pc_paragraph_style PARAS[4];

static void fn_ts_styles(void)
{
    size_t i = (size_t)U(F[2]) % 4;
    uint32_t id;
    pc_text_styles_fini(&STY[i]);
    if (!load(&TI_pc_text_styles, F[3], &STY[i])) {
        return;
    }
    TJ(&TI_pc_text_styles, &STY[i]);
    TB(pc_text_styles_is_empty(&STY[i]));
    if (pc_text_styles_next_char_id(&STY[i], &id)) {
        TU(id);
    } else {
        TS("-");
    }
    if (pc_text_styles_next_para_id(&STY[i], &id)) {
        TU(id);
    } else {
        TS("-");
    }
}

static void fn_ts_run(void)
{
    size_t i = (size_t)U(F[2]) % 4;
    pc_char_style_fini(&RUNS[i]);
    if (load(&TI_pc_char_style, F[3], &RUNS[i])) {
        TJ(&TI_pc_char_style, &RUNS[i]);
    }
}

static void fn_ts_para(void)
{
    size_t i = (size_t)U(F[2]) % 4;
    if (load(&TI_pc_paragraph_style, F[3], &PARAS[i])) {
        TJ(&TI_pc_paragraph_style, &PARAS[i]);
    }
}

static void fn_ts_resolve_para(void)
{
    uint32_t slot;
    pc_paragraph_style p;
    pc_error e;
    if (pc_text_styles_resolve_para(&STY[U(F[2]) % 4], OPTID(F[3], &slot), &p, &e) == PC_OK) {
        TJ(&TI_pc_paragraph_style, &p);
    } else {
        TERR(&e);
    }
}

static void fn_ts_resolve_char(void)
{
    uint32_t s1, s2;
    pc_char_style c;
    pc_error e;
    memset(&c, 0, sizeof c);
    if (pc_text_styles_resolve_char(&STY[U(F[2]) % 4], OPTID(F[3], &s1), OPTID(F[4], &s2), F[5], &c, &e) ==
        PC_OK) {
        TJ(&TI_pc_char_style, &c);
    } else {
        TERR(&e);
    }
    pc_char_style_fini(&c);
}

static void fn_ts_unique_name(void)
{
    pc_str s = PC_STR_INIT;
    pc_error e;
    if (pc_text_styles_unique_name(&STY[U(F[2]) % 4], B(F[3]), F[4], &s, &e) == PC_OK) {
        TS(pc_str_c(&s));
    } else {
        TERR(&e);
    }
    pc_str_fini(&s);
}

static void fn_ts_lookup(void)
{
    const pc_text_styles *s = &STY[U(F[2]) % 4];
    uint32_t id = (uint32_t)U(F[3]);
    const pc_character_style_def *c = pc_text_styles_char_style(s, id);
    const pc_paragraph_style_def *p = pc_text_styles_para_style(s, id);
    if (c != NULL) {
        TJSTR(pc_str_c(&c->name), c->name.len);
    } else {
        TS("None");
    }
    if (p != NULL) {
        TJSTR(pc_str_c(&p->name), p->name.len);
    } else {
        TS("None");
    }
}

static void fn_ts_unique_name_named(void)
{
    pc_text_styles s;
    pc_str a = PC_STR_INIT, b = PC_STR_INIT;
    pc_error e;
    if (load(&TI_pc_text_styles, F[2], &s)) {
        if (pc_text_styles_unique_name(&s, false, "Character Style", &a, &e) == PC_OK &&
            pc_text_styles_unique_name(&s, true, "Paragraph Style", &b, &e) == PC_OK) {
            TS(pc_str_c(&a));
            TS(pc_str_c(&b));
        } else {
            TERR(&e);
        }
    }
    pc_str_fini(&a);
    pc_str_fini(&b);
    pc_text_styles_fini(&s);
}

static void fn_ts_char_overrides(void)
{
    uint32_t slot;
    pc_style_attrs a = PC_JSON_VALUE_INIT;
    pc_error e;
    if (pc_text_styles_char_overrides(&STY[U(F[2]) % 4], &RUNS[U(F[3]) % 4], OPTID(F[4], &slot), F[5], &a,
                                      &e) == PC_OK) {
        TATTRS(&a);
    } else {
        TERR(&e);
    }
    pc_json_value_fini(&a);
}

static void fn_ts_para_overrides(void)
{
    pc_style_attrs a = PC_JSON_VALUE_INIT;
    pc_error e;
    if (pc_text_styles_para_overrides(&STY[U(F[2]) % 4], &PARAS[U(F[3]) % 4], &a, &e) == PC_OK) {
        TATTRS(&a);
    } else {
        TERR(&e);
    }
    pc_json_value_fini(&a);
}

static void fn_ts_restyle_char(void)
{
    uint32_t s1, s2, s3;
    pc_char_style c;
    pc_error e;
    memset(&c, 0, sizeof c);
    if (pc_text_styles_restyle_char(&STY[U(F[2]) % 4], &STY[U(F[3]) % 4], &RUNS[U(F[4]) % 4],
                                    OPTID(F[5], &s1), OPTID(F[6], &s2), OPTID(F[7], &s3), F[8], &c,
                                    &e) == PC_OK) {
        TJ(&TI_pc_char_style, &c);
    } else {
        TERR(&e);
    }
    pc_char_style_fini(&c);
}

static void fn_ts_restyle_para(void)
{
    uint32_t slot;
    pc_paragraph_style p;
    pc_error e;
    if (pc_text_styles_restyle_para(&STY[U(F[2]) % 4], &STY[U(F[3]) % 4], &PARAS[U(F[4]) % 4],
                                    OPTID(F[5], &slot), &p, &e) == PC_OK) {
        TJ(&TI_pc_paragraph_style, &p);
    } else {
        TERR(&e);
    }
}

static bool load_attrs(const char *s, pc_style_attrs *a)
{
    yyjson_val *v = parse(s);
    pc_error e;
    a->doc = NULL;
    return v != NULL && pc_json_value_from_val(v, a, &e) == PC_OK;
}

static void T_RESULT(pc_status st, const pc_error *e)
{
    if (st == PC_OK) {
        TS("OK");
    } else {
        TS("ERR ");
        S(e->msg);
    }
}

static void fn_ts_validate(void)
{
    pc_style_attrs a;
    pc_error e;
    pc_status st;
    if (!load_attrs(F[2], &a)) {
        TS("<attrs>");
        return;
    }
    st = pc_char_style_validate_attrs(&a, &e);
    T_RESULT(st, &e);
    st = pc_paragraph_style_validate_attrs(&a, &e);
    T_RESULT(st, &e);
    pc_json_value_fini(&a);
}

static void fn_ts_apply(void)
{
    pc_style_attrs a;
    pc_char_style c;
    pc_paragraph_style p;
    pc_error e;
    if (!load_attrs(F[2], &a)) {
        TS("<attrs>");
        return;
    }
    memset(&c, 0, sizeof c);
    if (pc_char_style_apply_attrs(&RUNS[1], &a, &c, &e) == PC_OK) {
        TJ(&TI_pc_char_style, &c);
        pc_char_style_fini(&c);
    } else {
        TS("ERR ");
        S(e.msg);
    }
    if (pc_paragraph_style_apply_attrs(&PARAS[1], &a, &p, &e) == PC_OK) {
        TJ(&TI_pc_paragraph_style, &p);
    } else {
        TS("ERR ");
        S(e.msg);
    }
    pc_json_value_fini(&a);
}

static void fn_ts_all_attrs(void)
{
    pc_style_attrs a = PC_JSON_VALUE_INIT, b = PC_JSON_VALUE_INIT;
    pc_error e;
    if (pc_char_style_all_attrs(&RUNS[1], &a, &e) == PC_OK) {
        TATTRS(&a);
    } else {
        TERR(&e);
    }
    if (pc_paragraph_style_all_attrs(&PARAS[1], &b, &e) == PC_OK) {
        TATTRS(&b);
    } else {
        TERR(&e);
    }
    pc_json_value_fini(&a);
    pc_json_value_fini(&b);
}

static void fn_ts_diff_attrs(void)
{
    pc_style_attrs a = PC_JSON_VALUE_INIT, b = PC_JSON_VALUE_INIT, c = PC_JSON_VALUE_INIT;
    pc_char_style d, s3;
    pc_paragraph_style pd = pc_paragraph_style_default();
    pc_error e;
    pc_char_style_default(&d);
    pc_char_style_default(&s3);
    s3.has_style_sheet = true;
    s3.style_sheet = 3;
    if (pc_char_style_diff_attrs(&RUNS[1], &d, &a, &e) == PC_OK) {
        TATTRS(&a);
    } else {
        TERR(&e);
    }
    if (pc_paragraph_style_diff_attrs(&PARAS[1], &pd, &b, &e) == PC_OK) {
        TATTRS(&b);
    } else {
        TERR(&e);
    }
    if (pc_char_style_diff_attrs(&s3, &d, &c, &e) == PC_OK) {
        TATTRS(&c);
    } else {
        TERR(&e);
    }
    pc_json_value_fini(&a);
    pc_json_value_fini(&b);
    pc_json_value_fini(&c);
    pc_char_style_fini(&d);
    pc_char_style_fini(&s3);
}

/* Document trees: a string is a raster layer; {"n":name,"c":[children]?,"l":[5 bools]?,"k":link?}. */
static bool build_layer(yyjson_val *v, pc_layer *out_l, int depth)
{
    pc_error e;
    yyjson_val *c, *lk, *k;
    const char *name;
    memset(out_l, 0, sizeof *out_l);
    if (depth > 32) {
        return false;
    }
    if (yyjson_is_str(v)) {
        return pc_layer_raster(out_l, yyjson_get_str(v), PC_PIXEL_FORMAT_RGBA8, &e) == PC_OK;
    }
    name = yyjson_get_str(yyjson_obj_get(v, "n"));
    c = yyjson_obj_get(v, "c");
    if (name == NULL) {
        return false;
    }
    if (c != NULL) {
        size_t n = yyjson_arr_size(c), i, max;
        pc_layer *ch = n > 0 ? calloc(n, sizeof *ch) : NULL;
        yyjson_val *it;
        if (n > 0 && ch == NULL) {
            return false;
        }
        yyjson_arr_foreach(c, i, max, it)
        {
            if (!build_layer(it, &ch[i], depth + 1)) {
                return false;
            }
        }
        if (pc_layer_group(out_l, name, ch, n, &e) != PC_OK) {
            return false;
        }
    } else if (pc_layer_raster(out_l, name, PC_PIXEL_FORMAT_RGBA8, &e) != PC_OK) {
        return false;
    }
    lk = yyjson_obj_get(v, "l");
    if (lk != NULL) {
        out_l->locks.transparency = yyjson_get_bool(yyjson_arr_get(lk, 0));
        out_l->locks.pixels = yyjson_get_bool(yyjson_arr_get(lk, 1));
        out_l->locks.position = yyjson_get_bool(yyjson_arr_get(lk, 2));
        out_l->locks.artboard = yyjson_get_bool(yyjson_arr_get(lk, 3));
        out_l->locks.all = yyjson_get_bool(yyjson_arr_get(lk, 4));
    }
    k = yyjson_obj_get(v, "k");
    if (k != NULL) {
        out_l->has_link_group = true;
        out_l->link_group = yyjson_get_uint(k);
    }
    return true;
}

static bool build_doc(pc_document *d, const char *name, uint32_t w, uint32_t h, const char *tree)
{
    pc_error e;
    yyjson_val *t = parse(tree), *it;
    size_t n, i, max;
    memset(d, 0, sizeof *d);
    if (t == NULL ||
        pc_document_init(d, name, (pc_size){w, h}, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &e) != PC_OK) {
        S("<doc>");
        return false;
    }
    n = yyjson_arr_size(t);
    if (n == 0) {
        return true;
    }
    d->layers = calloc(n, sizeof *d->layers);
    if (d->layers == NULL) {
        return false;
    }
    d->nlayers = n;
    yyjson_arr_foreach(t, i, max, it)
    {
        if (!build_layer(it, &d->layers[i], 0)) {
            S("<layer>");
            return false;
        }
    }
    return true;
}

static void fn_doc_depth(void)
{
    pc_document d;
    size_t depth, count;
    pc_error e;
    if (build_doc(&d, "t", 10, 10, F[2])) {
        if (pc_document_max_group_depth(&d, &depth, &e) == PC_OK &&
            pc_document_layer_count(&d, &count, &e) == PC_OK) {
            TU(depth);
            TU(count);
        } else {
            TERR(&e);
        }
    }
    pc_document_fini(&d);
}

static void fn_doc_link(void)
{
    pc_document d;
    bool found;
    uint64_t g;
    pc_error e;
    if (build_doc(&d, "t", 10, 10, F[2])) {
        if (pc_document_unused_link_group(&d, &found, &g, &e) != PC_OK) {
            TERR(&e);
        } else if (found) {
            TU(g);
        } else {
            TS("None");
        }
    }
    pc_document_fini(&d);
}

static void fn_doc_next_name(void)
{
    pc_document d;
    pc_str s = PC_STR_INIT;
    pc_error e;
    if (build_doc(&d, "t", 10, 10, F[2])) {
        if (pc_document_next_layer_name(&d, F[3], &s, &e) == PC_OK) {
            TS(pc_str_c(&s));
        } else {
            TERR(&e);
        }
    }
    pc_str_fini(&s);
    pc_document_fini(&d);
}

static void fn_doc_copy_name(void)
{
    pc_document d;
    pc_str s = PC_STR_INIT;
    pc_error e;
    if (build_doc(&d, "t", 10, 10, F[2])) {
        if (pc_document_copy_name(&d, F[3], strlen(F[3]), &s, &e) == PC_OK) {
            TS(pc_str_c(&s));
        } else {
            TERR(&e);
        }
    }
    pc_str_fini(&s);
    pc_document_fini(&d);
}

static void fn_doc_locks(void)
{
    pc_document d;
    size_t path[16], n = 0;
    const char *s = F[3];
    pc_locks l;
    while (*s != '\0' && n < 16) {
        char *end;
        path[n++] = (size_t)strtoull(s, &end, 10);
        s = end;
        if (*s == '.') {
            s++;
        }
    }
    if (build_doc(&d, "t", 10, 10, F[2])) {
        l = pc_document_locks_at(&d, n > 0 ? path : NULL, n);
        TJ(&TI_pc_locks, &l);
    }
    pc_document_fini(&d);
}

static void fn_auto_slices(void)
{
    pc_rect c, *taken = NULL, *res = NULL;
    void *tk = NULL;
    size_t nt = 0, nr = 0;
    pc_error e;
    if (load(&TI_pc_rect, F[2], &c) && load_arr(&TI_pc_rect, F[3], &tk, &nt)) {
        taken = tk;
        if (pc_auto_slices(c, taken, nt, &res, &nr, &e) == PC_OK) {
            TARR(&TI_pc_rect, res, nr);
        } else {
            TERR(&e);
        }
    }
    free(res);
    free(tk);
}

static void fn_slices_resolve(void)
{
    pc_document d;
    pc_resolved_slice *r = NULL;
    size_t n = 0, i;
    uint32_t w = (uint32_t)U(F[3]), h = (uint32_t)U(strchr(F[3], 'x') + 1), id;
    pc_error e;
    if (build_doc(&d, F[2], w, h, "[]")) {
        pc_slices_fini(&d.slices);
        if (load(&TI_pc_slices, F[4], &d.slices)) {
            if (pc_slices_resolve(&d, &r, &n, &e) == PC_OK) {
                T();
                for (i = 0; i < n; i++) {
                    (void)pc_buf_appendf(&out, NULL, "%s%zu|", i > 0 ? ";" : "", r[i].number);
                    if (r[i].has_id) {
                        (void)pc_buf_appendf(&out, NULL, "%lu", (unsigned long)r[i].id);
                    } else {
                        S("-");
                    }
                    S("|");
                    S(pc_slice_origin_id(r[i].origin));
                    S("|");
                    W(pc_rect_to_json(md, &r[i].rect, 0), 0);
                    S("|");
                    SN(pc_str_c(&r[i].name), r[i].name.len);
                    S("|");
                    S(pc_slice_kind_id(r[i].kind));
                }
            } else {
                TERR(&e);
            }
            if (pc_slices_next_id(&d.slices, &id)) {
                TU(id);
            } else {
                TS("-");
            }
        }
    }
    pc_resolved_slices_free(r, n);
    pc_document_fini(&d);
}

static void fn_slices_base_name(void)
{
    pc_document d;
    pc_str s = PC_STR_INIT;
    pc_error e;
    if (build_doc(&d, F[2], 1, 1, "[]")) {
        if (pc_slices_base_name(&d, &s, &e) == PC_OK) {
            TS(pc_str_c(&s));
        } else {
            TERR(&e);
        }
    }
    pc_str_fini(&s);
    pc_document_fini(&d);
}

static void fn_slices_next_id(void)
{
    pc_slices s;
    uint32_t id;
    const pc_slice *g;
    if (load(&TI_pc_slices, F[2], &s)) {
        if (pc_slices_next_id(&s, &id)) {
            TU(id);
        } else {
            TS("-");
        }
        g = pc_slices_get(&s, 9);
        if (g != NULL) {
            TJ(&TI_pc_slice, g);
        } else {
            TS("None");
        }
        TB(pc_slices_is_empty(&s));
    }
    pc_slices_fini(&s);
}

static pc_variables VARS;

static void fn_vars_full(void)
{
    pc_variables_fini(&VARS);
    if (load(&TI_pc_variables, F[2], &VARS)) {
        TJ(&TI_pc_variables, &VARS);
    }
}

static void fn_vars_lookup(void)
{
    const char *n = F[2];
    size_t len = strlen(n), idx;
    const pc_variable_def *d = pc_variables_def(&VARS, n, len);
    const pc_data_set *s = pc_variables_data_set(&VARS, n, len);
    const pc_data_value *v = VARS.ndata_sets > 0 ? pc_data_set_get(&VARS.data_sets[0], n, len) : NULL;
    if (d != NULL) {
        TJ(&TI_pc_variable_def, d);
    } else {
        TS("None");
    }
    if (s != NULL) {
        TJ(&TI_pc_data_set, s);
    } else {
        TS("None");
    }
    if (pc_variables_data_set_index(&VARS, n, len, &idx)) {
        TU(idx);
    } else {
        TS("None");
    }
    if (v != NULL) {
        TJ(&TI_pc_data_value, v);
    } else {
        TS("None");
    }
}

static void fn_vars_is_empty(void)
{
    pc_variables a;
    memset(&a, 0, sizeof a);
    TB(pc_variables_is_empty(&a));
    TB(pc_variables_is_empty(&VARS));
    a.has_active = true;
    a.active = 1;
    TB(pc_variables_is_empty(&a));
}

static void fn_nonfinite(void)
{
    const char *n = F[2];
    pc_error e;
    if (strcmp(n, "CurvePoint") == 0) {
        pc_curve_point v = {NAN, INFINITY};
        TJ(&TI_pc_curve_point, &v);
    } else if (strcmp(n, "Point") == 0) {
        pc_point v = {NAN, -INFINITY};
        TJ(&TI_pc_point, &v);
    } else if (strcmp(n, "Color") == 0) {
        pc_color v = pc_color_rgba(NAN, INFINITY, -INFINITY, 0.5f);
        TJ(&TI_pc_color, &v);
    } else if (strcmp(n, "MeasurementScale") == 0) {
        pc_measurement_scale v;
        memset(&v, 0, sizeof v);
        v.pixel_length = NAN;
        v.logical_length = INFINITY;
        if (pc_str_set_c(&v.units, "x", &e) == PC_OK) {
            TJ(&TI_pc_measurement_scale, &v);
        }
        pc_measurement_scale_fini(&v);
    } else if (strcmp(n, "Timeline") == 0) {
        pc_timeline v = {NAN, 1, 0, 0, 1};
        TJ(&TI_pc_timeline, &v);
    } else if (strcmp(n, "GlobalLight") == 0) {
        pc_global_light v = {INFINITY, NAN};
        TJ(&TI_pc_global_light, &v);
    } else if (strcmp(n, "Guides") == 0) {
        double h[2] = {NAN, 1.0}, vv[1] = {-INFINITY};
        pc_guides v = {h, 2, vv, 1, NULL, 0};
        TJ(&TI_pc_guides, &v);
    } else if (strcmp(n, "CharStyleValue") == 0) {
        pc_char_style v;
        pc_char_style_default(&v);
        v.size_pt = NAN;
        v.has_leading = true;
        v.leading_pt = INFINITY;
        T();
        W(pc_char_style_to_json(md, &v, PC_DOC_JSON_VALUE), PC_JSON_SORT_KEYS);
        pc_char_style_fini(&v);
    } else {
        TS("<unknown>");
    }
}

#include "parity_resync.h"

typedef struct fn_entry {
    const char *name;
    size_t nargs;
    void (*fn)(void);
} fn_entry;

static const fn_entry FNS[] = {
    {"curve_map", 1, fn_curve_map},
    {"curve_map_outputs", 1, fn_curve_map_outputs},
    {"stroke_validate", 5, fn_stroke_validate},
    {"pattern_paintable", 2, fn_pattern_paintable},
    {"pattern_limits", 0, fn_pattern_limits},
    {"effect_in_mode", 2, fn_effect_in_mode},
    {"effect_set_enabled", 2, fn_effect_set_enabled},
    {"fill_in_mode", 2, fn_fill_in_mode},
    {"contour_is_corner", 2, fn_contour_is_corner},
    {"hue_neutral", 1, fn_hue_neutral},
    {"hue_defaults", 0, fn_hue_defaults},
    {"hue_weight_sweep", 1, fn_hue_weight_sweep},
    {"hue_weight", 2, fn_hue_weight},
    {"canonical_bounds", 1, fn_canonical_bounds},
    {"hue_is_neutral", 1, fn_hue_is_neutral},
    {"blend_weight_sweep", 1, fn_blend_weight_sweep},
    {"blend_weight", 2, fn_blend_weight},
    {"blend_if_set", 1, fn_blend_if_set},
    {"blend_if_is_default", 1, fn_blend_if_is_default},
    {"knockout", 1, fn_knockout},
    {"knockout_from_psd", 1, fn_knockout_from_psd},
    {"knockout_from_name", 1, fn_knockout_from_name},
    {"advanced_default", 0, fn_advanced_default},
    {"advanced_is_default", 1, fn_advanced_is_default},
    {"lut3d_len", 1, fn_lut3d_len},
    {"is_identity_curve", 1, fn_is_identity_curve},
    {"adj_label", 1, fn_adj_label},
    {"adj_default", 1, fn_adj_default},
    {"default", 1, fn_default},
    {"ct_nearest", 2, fn_ct_nearest},
    {"ct_rgb", 2, fn_ct_rgb},
    {"ink_density_sweep", 1, fn_ink_density_sweep},
    {"ink_density", 2, fn_ink_density},
    {"duotone_render_sweep", 1, fn_duotone_render_sweep},
    {"duotone_render", 2, fn_duotone_render},
    {"duotone_display", 1, fn_duotone_display},
    {"stack_mode", 1, fn_stack_mode},
    {"stack_mode_from_id", 1, fn_stack_mode_from_id},
    {"label_color", 1, fn_label_color},
    {"label_from_id", 1, fn_label_from_id},
    {"slice_codes", 1, fn_slice_codes},
    {"slice_kind_from_id", 1, fn_slice_kind_from_id},
    {"effect_label", 1, fn_effect_label},
    {"text_align_justified", 1, fn_text_align_justified},
    {"content_kind", 1, fn_content_kind},
    {"artboard_bg", 1, fn_artboard_bg},
    {"comp_info", 1, fn_comp_info},
    {"overlay_coverage", 3, fn_overlay_coverage},
    {"locks_union", 2, fn_locks_union},
    {"mask_value", 4, fn_mask_value},
    {"path", 1, fn_path},
    {"subpath_segments", 1, fn_subpath_segments},
    {"subpath_poly", 1, fn_subpath_poly},
    {"knot_transform", 1, fn_knot_transform},
    {"pattern_new", 6, fn_pattern_new},
    {"pattern_display_name", 1, fn_pattern_display_name},
    {"pattern_find", 2, fn_pattern_find},
    {"scale", 3, fn_scale},
    {"measurement", 1, fn_measurement},
    {"timeline_new", 2, fn_timeline_new},
    {"timeline_clamp", 1, fn_timeline_clamp},
    {"timeline_step", 3, fn_timeline_step},
    {"timeline_time", 2, fn_timeline_time},
    {"text_runs", 3, fn_text_runs},
    {"text_sync_summary", 1, fn_text_sync_summary},
    {"ts_styles", 1, fn_ts_styles},
    {"ts_run", 1, fn_ts_run},
    {"ts_para", 1, fn_ts_para},
    {"ts_resolve_para", 2, fn_ts_resolve_para},
    {"ts_resolve_char", 4, fn_ts_resolve_char},
    {"ts_unique_name", 3, fn_ts_unique_name},
    {"ts_lookup", 2, fn_ts_lookup},
    {"ts_unique_name_named", 1, fn_ts_unique_name_named},
    {"ts_char_overrides", 4, fn_ts_char_overrides},
    {"ts_para_overrides", 2, fn_ts_para_overrides},
    {"ts_restyle_char", 7, fn_ts_restyle_char},
    {"ts_restyle_para", 4, fn_ts_restyle_para},
    {"ts_validate", 1, fn_ts_validate},
    {"ts_apply", 1, fn_ts_apply},
    {"ts_all_attrs", 0, fn_ts_all_attrs},
    {"ts_diff_attrs", 0, fn_ts_diff_attrs},
    {"doc_depth", 1, fn_doc_depth},
    {"doc_link", 1, fn_doc_link},
    {"doc_next_name", 2, fn_doc_next_name},
    {"doc_copy_name", 2, fn_doc_copy_name},
    {"doc_locks", 2, fn_doc_locks},
    {"auto_slices", 2, fn_auto_slices},
    {"slices_resolve", 3, fn_slices_resolve},
    {"slices_base_name", 1, fn_slices_base_name},
    {"slices_next_id", 1, fn_slices_next_id},
    {"vars_full", 0, fn_vars_full},
    {"vars_lookup", 1, fn_vars_lookup},
    {"vars_is_empty", 0, fn_vars_is_empty},
    {"nonfinite", 1, fn_nonfinite},
};

static void do_fn(void)
{
    size_t i, k;
    for (i = 0; i < sizeof FNS / sizeof FNS[0]; i++) {
        if (strcmp(FNS[i].name, F[1]) == 0) {
            break;
        }
    }
    S("fn");
    TS(F[1]);
    if (i == sizeof FNS / sizeof FNS[0]) {
        TS("<unknown fn>");
        return;
    }
    for (k = 0; k < FNS[i].nargs && 2 + k < NF; k++) {
        TS(F[2 + k]);
    }
    if (NF < 3 + FNS[i].nargs) {
        TS("<too few fields>");
        return;
    }
    FNS[i].fn();
}

/* ------------------------------------------------------------------------------------------ */

/* Equal, or both errors where got hit pc_error's cap and want starts with it. */
static bool same(const char *want, const char *got)
{
    const char *we, *ge;
    size_t pre;
    if (strcmp(want, got) == 0) {
        return true;
    }
    we = strstr(want, "\tERR ");
    ge = strstr(got, "\tERR ");
    if (we == NULL || ge == NULL || we - want != ge - got) {
        return false;
    }
    pre = (size_t)(ge - got) + 5;
    return strncmp(want, got, pre) == 0 && strlen(got + pre) == PC_ERROR_MSG_MAX - 1 &&
           strncmp(want + pre, got + pre, PC_ERROR_MSG_MAX - 1) == 0;
}

static char *read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t cap = 0, len = 0;
    if (f == NULL) {
        return NULL;
    }
    for (;;) {
        size_t got;
        if (len + 65536 + 1 > cap) {
            char *nb;
            cap = (len + 65536 + 1) * 2;
            nb = realloc(buf, cap);
            if (nb == NULL) {
                free(buf);
                (void)fclose(f);
                return NULL;
            }
            buf = nb;
        }
        got = fread(buf + len, 1, 65536, f);
        len += got;
        if (got == 0) {
            break;
        }
    }
    (void)fclose(f);
    buf[len] = '\0';
    *n = len;
    return buf;
}

static void test_doc_vectors_match_the_reference(void)
{
    size_t n, lineno = 0;
    char *all = read_all(GOLDEN, &n);
    char *p, *fields[24];
    size_t shown = 0, kinds[5] = {0, 0, 0, 0, 0}, kfail[5] = {0, 0, 0, 0, 0};
    static const char *const KN[5] = {"ser", "serp", "serv", "de", "fn"};
    if (all == NULL) {
        PC_T_FAIL("cannot read %s", GOLDEN);
        return;
    }
    p = all;
    while (*p != '\0') {
        char *eol = strchr(p, '\n');
        char *copy, *q;
        size_t kind;
        if (eol != NULL) {
            *eol = '\0';
        }
        lineno++;
        copy = malloc(strlen(p) + 1);
        if (copy == NULL) {
            break;
        }
        memcpy(copy, p, strlen(p) + 1);
        NF = 0;
        q = copy;
        while (NF < 24) {
            char *tab = strchr(q, '\t');
            fields[NF++] = q;
            if (tab == NULL) {
                break;
            }
            *tab = '\0';
            q = tab + 1;
        }
        F = fields;
        line_reset();
        for (kind = 0; kind < 5 && strcmp(fields[0], KN[kind]) != 0; kind++) {
        }
        if (kind < 3 && NF >= 3) {
            do_ser(fields[0]);
        } else if (kind == 3 && NF >= 4) {
            do_de();
        } else if (kind == 4 && NF >= 3) {
            do_fn();
        } else {
            S("<bad golden line>");
        }
        pc_t_checks++;
        if (kind < 5) {
            kinds[kind]++;
        }
        if (same(p, pc_buf_cstr(&out))) {
            passes++;
        } else {
            fails++;
            if (kind < 5) {
                kfail[kind]++;
            }
            if (shown < 400) {
                shown++;
                PC_T_FAIL("line %zu", lineno);
                (void)fprintf(stderr, "  want %s\n  got  %s\n", p, pc_buf_cstr(&out));
            } else {
                pc_t_failed_checks++;
            }
        }
        free(copy);
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    for (n = 0; n < 5; n++) {
        (void)fprintf(stderr, "  %-4s %zu lines, %zu mismatches\n", KN[n], kinds[n], kfail[n]);
    }
    (void)fprintf(stderr, "doc parity: %zu of %zu golden lines match\n", passes, passes + fails);
    CHECK(lineno > 2000);
    free(all);
}

int main(int argc, char **argv)
{
    size_t i;
    RUN(test_doc_vectors_match_the_reference);
    line_reset();
    yyjson_mut_doc_free(md);
    pc_buf_fini(&out);
    for (i = 0; i < npats; i++) {
        pc_pattern_fini(&pats[i]);
    }
    for (i = 0; i < 4; i++) {
        pc_text_styles_fini(&STY[i]);
        pc_char_style_fini(&RUNS[i]);
    }
    pc_variables_fini(&VARS);
    return pc_t_finish();
}
