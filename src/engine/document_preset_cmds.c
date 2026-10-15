/* File > New and the New Document validation helpers (port of crates/engine/src/document_preset_cmds.rs
 * plus the `file.new` command from crates/engine/src/commands.rs, deferred to E3 by E1).
 *
 * The named New Document configurations and the Recent list (document.presets.* / document.recent.*)
 * live on the session preset store (crate::presets::PresetState), which is ported in chunk E2b. Only
 * the creation path -- the validation helpers, `file.new`, and the validate_new_params hook moved
 * here from hooks.c -- belongs to E3; the preset/recent commands are registered by E2b. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine_internal.h"
#include "presets.h"

/* Limits (document_preset_cmds::MAX_DIMENSION / MAX_RESOLUTION). */
#define PC_DP_MAX_DIMENSION 300000u
#define PC_DP_MAX_RESOLUTION 30000.0

/* The colour models `file.new` accepts (document_preset_cmds::color_mode). */
static bool dp_color_mode(const char *s, size_t len, pc_color_mode *out)
{
    if (len == 3 && memcmp(s, "rgb", 3) == 0) {
        *out = PC_COLOR_MODE_RGB;
        return true;
    }
    if ((len == 4 && memcmp(s, "gray", 4) == 0) || (len == 9 && memcmp(s, "grayscale", 9) == 0)) {
        *out = PC_COLOR_MODE_GRAYSCALE;
        return true;
    }
    if (len == 4 && memcmp(s, "cmyk", 4) == 0) {
        *out = PC_COLOR_MODE_CMYK;
        return true;
    }
    if (len == 3 && memcmp(s, "lab", 3) == 0) {
        *out = PC_COLOR_MODE_LAB;
        return true;
    }
    return false;
}

/* Bit depth -> sample type (document_preset_cmds::sample_type). */
static bool dp_sample_type(uint64_t depth, pc_sample_type *out)
{
    switch (depth) {
    case 8:
        *out = PC_SAMPLE_TYPE_U8;
        return true;
    case 16:
        *out = PC_SAMPLE_TYPE_U16;
        return true;
    case 32:
        *out = PC_SAMPLE_TYPE_F32;
        return true;
    default:
        return false;
    }
}

/* commands::parse_hex: strip an optional leading '#', accept exactly 6 or 8 hex digits (straight
 * RGBA 0..1). Note this is the `file.new` colour grammar, stricter than pc_engine_parse_hex (which
 * also accepts the 3/4-digit short forms and requires the '#'). */
static bool dp_parse_hex(const char *s, size_t len, float out[4])
{
    if (len > 0 && s[0] == '#') {
        s++;
        len--;
    }
    unsigned v[4] = {0, 0, 0, 255};
    size_t comps;
    if (len == 6) {
        comps = 3;
    } else if (len == 8) {
        comps = 4;
    } else {
        return false;
    }
    for (size_t i = 0; i < comps; i++) {
        int hi = s[i * 2], lo = s[i * 2 + 1];
        int dh, dl;
        if (hi >= '0' && hi <= '9') {
            dh = hi - '0';
        } else if (hi >= 'a' && hi <= 'f') {
            dh = hi - 'a' + 10;
        } else if (hi >= 'A' && hi <= 'F') {
            dh = hi - 'A' + 10;
        } else {
            return false;
        }
        if (lo >= '0' && lo <= '9') {
            dl = lo - '0';
        } else if (lo >= 'a' && lo <= 'f') {
            dl = lo - 'a' + 10;
        } else if (lo >= 'A' && lo <= 'F') {
            dl = lo - 'A' + 10;
        } else {
            return false;
        }
        v[i] = (unsigned)(dh * 16 + dl);
    }
    for (int i = 0; i < 4; i++) {
        out[i] = (float)v[i] / 255.0f;
    }
    return true;
}

/* A saved Background Color must not follow later changes to the toolbox colour
 * (document_preset_cmds::background_color). On success writes out[4] = [r,g,b,1]. */
static pc_status dp_background_color(yyjson_val *p, const float fallback[4], const char *cmd,
                                     float out[4], pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "backgroundColor");
    float c[3] = {0.0f, 0.0f, 0.0f};
    if (v == NULL) {
        c[0] = fallback[0];
        c[1] = fallback[1];
        c[2] = fallback[2];
    } else {
        if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 3) {
            return pc_engine_err_bad_params(err, cmd, "`backgroundColor` must be [r,g,b] in 0..1");
        }
        yyjson_arr_iter it = yyjson_arr_iter_with(v);
        yyjson_val *e;
        size_t i = 0;
        while ((e = yyjson_arr_iter_next(&it)) != NULL && i < 3) {
            double f;
            if (!pc_json_as_f64(e, &f)) {
                return pc_engine_err_bad_params(err, cmd, "`backgroundColor` must be [r,g,b] in 0..1");
            }
            c[i++] = (float)f;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (!isfinite(c[i]) || c[i] < 0.0f || c[i] > 1.0f) {
            return pc_engine_err_bad_params(err, cmd, "`backgroundColor` must be [r,g,b] in 0..1");
        }
    }
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
    out[3] = 1.0f;
    return PC_OK;
}

/* Whether `s`/`len` is one of the named background choices, or a valid `file.new` hex colour whose
 * digits are all hex (document_preset_cmds::validate_new_params `background` arm). */
static bool dp_background_valid(const char *s, size_t len)
{
    if ((len == 5 && memcmp(s, "white", 5) == 0) || (len == 5 && memcmp(s, "black", 5) == 0) ||
        (len == 11 && memcmp(s, "transparent", 11) == 0) ||
        (len == 15 && memcmp(s, "backgroundColor", 15) == 0)) {
        return true;
    }
    /* strip_prefix('#'): the remaining bytes must all be hex, and parse_hex must accept it. */
    const char *h = s;
    size_t hl = len;
    if (hl > 0 && h[0] == '#') {
        h++;
        hl--;
    }
    for (size_t i = 0; i < hl; i++) {
        int c = h[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    float rgba[4];
    return dp_parse_hex(s, len, rgba);
}

/* document_preset_cmds::validate_new_params: reject creation before dispatch commits a floating
 * selection or adds a document. Missing fields keep file.new's documented defaults; a null params
 * object is accepted. */
static pc_status validate_new_params(const pc_engine_session *s, yyjson_val *p, pc_error *err)
{
    const char *cmd = "file.new";
    if (p == NULL || yyjson_is_null(p)) {
        return PC_OK;
    }
    if (!yyjson_is_obj(p)) {
        return pc_engine_err_bad_params(err, cmd, "params must be a JSON object");
    }
    const float *bg = pc_engine_session_tools(s)->background;
    yyjson_obj_iter it = yyjson_obj_iter_with(p);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&it)) != NULL) {
        size_t klen = 0;
        const char *k = yyjson_get_str(key);
        klen = yyjson_get_len(key);
        yyjson_val *value = yyjson_obj_iter_get_val(key);
        bool valid;
        const char *expected;
        if ((klen == 5 && memcmp(k, "width", 5) == 0) || (klen == 6 && memcmp(k, "height", 6) == 0) ||
            (klen == 10 && memcmp(k, "resolution", 10) == 0)) {
            bool is_res = klen == 10;
            double max = is_res ? PC_DP_MAX_RESOLUTION : (double)PC_DP_MAX_DIMENSION;
            double n;
            valid = pc_json_as_f64(value, &n) && isfinite(n) && n >= 1.0 && n <= max;
            if (!valid) {
                return pc_engine_err_bad_params(err, cmd, "`%s` must be a number within 1..=%.0f", k, max);
            }
            continue;
        } else if (klen == 4 && memcmp(k, "mode", 4) == 0) {
            size_t vlen = 0;
            const char *vs = pc_json_as_str(value, &vlen);
            pc_color_mode m;
            valid = vs != NULL && dp_color_mode(vs, vlen, &m);
            expected = "rgb, gray, grayscale, cmyk or lab";
        } else if (klen == 5 && memcmp(k, "depth", 5) == 0) {
            uint64_t d;
            pc_sample_type st;
            valid = pc_json_as_u64(value, &d) && dp_sample_type(d, &st);
            expected = "8, 16 or 32";
        } else if (klen == 10 && memcmp(k, "background", 10) == 0) {
            size_t vlen = 0;
            const char *vs = pc_json_as_str(value, &vlen);
            valid = vs != NULL && dp_background_valid(vs, vlen);
            expected = "white, black, transparent, backgroundColor or a valid hex colour";
        } else if (klen == 15 && memcmp(k, "backgroundColor", 15) == 0) {
            float out[4];
            pc_status rc = dp_background_color(p, bg, cmd, out, err);
            if (rc != PC_OK) {
                return rc;
            }
            continue;
        } else if (klen == 4 && memcmp(k, "name", 4) == 0) {
            valid = yyjson_is_str(value);
            expected = "a string";
        } else {
            return pc_engine_err_bad_params(err, cmd, "unknown parameter `%.*s`", (int)klen, k);
        }
        if (!valid) {
            return pc_engine_err_bad_params(err, cmd, "`%.*s` must be %s", (int)klen, k, expected);
        }
    }
    /* An explicit backgroundColor choice must carry a valid colour even when none was listed above. */
    size_t blen = 0;
    yyjson_val *bgv = pc_json_get(p, "background");
    const char *bs = bgv != NULL ? pc_json_as_str(bgv, &blen) : NULL;
    if (bs != NULL && blen == 15 && memcmp(bs, "backgroundColor", 15) == 0) {
        float out[4];
        pc_status rc = dp_background_color(p, bg, cmd, out, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    return PC_OK;
}

/* file.new validation hook (moved here from hooks.c; its default returned PC_OK). dispatch calls
 * this before committing any floating selection so a bad New is rejected cleanly. */
pc_status pc_engine_hook_validate_new_params(pc_engine_session *s, yyjson_val *params, pc_error *err)
{
    return validate_new_params(s, params, err);
}

/* A pixel-count parameter: an integer, or a float from a UI field rounded and clamped to the
 * dimension range; absent or non-numeric keeps the default (commands.rs file.new `px`). */
static uint32_t dp_px(yyjson_val *p, const char *key, uint32_t def)
{
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return def;
    }
    uint64_t u;
    if (pc_json_as_u64(v, &u)) {
        if (u < 1) {
            u = 1;
        }
        if (u > PC_DP_MAX_DIMENSION) {
            u = PC_DP_MAX_DIMENSION;
        }
        return (uint32_t)u;
    }
    double f;
    if (pc_json_as_f64(v, &f) && isfinite(f)) {
        double r = round(f);
        if (r < 1.0) {
            r = 1.0;
        }
        if (r > (double)PC_DP_MAX_DIMENSION) {
            r = (double)PC_DP_MAX_DIMENSION;
        }
        return (uint32_t)r;
    }
    return def;
}

static pc_status run_file_new(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_status rc = validate_new_params(s, p, err);
    if (rc != PC_OK) {
        return rc;
    }
    uint32_t w = dp_px(p, "width", 1920);
    uint32_t h = dp_px(p, "height", 1080);

    pc_color_mode mode = PC_COLOR_MODE_RGB;
    yyjson_val *mv = pc_json_get(p, "mode");
    size_t mlen = 0;
    const char *ms = mv != NULL ? pc_json_as_str(mv, &mlen) : NULL;
    if (ms != NULL) {
        (void)dp_color_mode(ms, mlen, &mode);
    }

    pc_sample_type depth = PC_SAMPLE_TYPE_U8;
    uint64_t dv;
    if (pc_json_as_u64(pc_json_get(p, "depth"), &dv)) {
        (void)dp_sample_type(dv, &depth);
    }

    yyjson_val *nv = pc_json_get(p, "name");
    size_t nlen = 0;
    const char *name = nv != NULL ? pc_json_as_str(nv, &nlen) : NULL;
    if (name == NULL) {
        name = "Untitled";
    }

    double res = 72.0;
    double rv;
    if (pc_json_as_f64(pc_json_get(p, "resolution"), &rv) && isfinite(rv)) {
        res = rv;
    }
    if (res < 1.0) {
        res = 1.0;
    }
    if (res > PC_DP_MAX_RESOLUTION) {
        res = PC_DP_MAX_RESOLUTION;
    }

    yyjson_val *bgv = pc_json_get(p, "background");
    size_t blen = 0;
    const char *bs = bgv != NULL ? pc_json_as_str(bgv, &blen) : NULL;

    pc_document doc;
    pc_size size = pc_size_make(w, h);
    bool built = false;
    if (bs != NULL && blen == 15 && memcmp(bs, "backgroundColor", 15) == 0) {
        float bgc[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        rc = dp_background_color(p, pc_engine_session_tools(s)->background, "file.new", bgc, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = pc_document_with_background(&doc, name, size, mode, depth,
                                         pc_color_rgba(bgc[0], bgc[1], bgc[2], 1.0f), err);
        built = rc == PC_OK;
    } else if (bs != NULL && blen == 11 && memcmp(bs, "transparent", 11) == 0) {
        rc = pc_document_init(&doc, name, size, mode, depth, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_layer layer;
        rc = pc_layer_raster(&layer, "Layer 1", pc_document_pixel_format(&doc), err);
        if (rc != PC_OK) {
            pc_document_fini(&doc);
            return rc;
        }
        pc_layer_id id;
        rc = pc_document_insert_above(&doc, NULL, &layer, &id, err);
        if (rc != PC_OK) {
            pc_layer_fini(&layer);
            pc_document_fini(&doc);
            return rc;
        }
        built = true;
    } else if (bs != NULL && blen == 5 && memcmp(bs, "black", 5) == 0) {
        rc = pc_document_with_background(&doc, name, size, mode, depth, pc_color_rgba(0.0f, 0.0f, 0.0f, 1.0f),
                                         err);
        built = rc == PC_OK;
    } else {
        /* "white" and every other value go through the hex grammar; an unparsable one is white. */
        float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (bs != NULL) {
            (void)dp_parse_hex(bs, blen, c);
        }
        rc = pc_document_with_background(&doc, name, size, mode, depth, pc_color_rgba(c[0], c[1], c[2], c[3]),
                                         err);
        built = rc == PC_OK;
    }
    if (!built) {
        return rc;
    }
    doc.resolution_dpi = (float)res;

    size_t index = 0;
    rc = pc_engine_session_add_document(s, &doc, NULL, &index, err);
    if (rc != PC_OK) {
        pc_document_fini(&doc);
        return rc;
    }

    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_uint(jd, root, "document", index);
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

/* ==========================================================================================
 * Named New Document configurations (document.presets.*) and the Recent list (document.recent.*).
 * The data lives on the session PresetState (documents / recent_documents, added for E3); the
 * logic and struct layout live here. Port of the rest of document_preset_cmds.rs.
 * ========================================================================================== */

#define PC_DP_MAX_PRESETS 256
#define PC_DP_MAX_RECENT 20

/* DocumentSettings (document_preset_cmds::DocumentSettings). */
typedef struct pc_document_settings {
    uint32_t width, height;
    float resolution;
    char *mode;            /* "rgb"|"gray"|"cmyk"|"lab" (owned) */
    uint8_t depth;
    char *background;      /* owned */
    bool has_bg_color;
    float bg_color[3];
    char *unit;            /* owned */
    char *resolution_unit; /* owned */
} pc_document_settings;

typedef struct pc_document_preset {
    char *name; /* owned */
    pc_document_settings settings;
} pc_document_preset;

static void ds_fini(pc_document_settings *s)
{
    if (s == NULL) {
        return;
    }
    free(s->mode);
    free(s->background);
    free(s->unit);
    free(s->resolution_unit);
    memset(s, 0, sizeof *s);
}

static char *dp_dup(const char *s)
{
    return pc_strndup(s, strlen(s));
}

/* Code-point count of a string (brush_preset_cmds::MAX_NAME is a char count). */
static size_t dp_char_count(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

/* Trim ASCII whitespace; *out points into s, *out_len its length. */
static void dp_trim(const char *s, size_t len, const char **out, size_t *out_len)
{
    size_t a = 0, b = len;
    while (a < b) {
        unsigned char c = (unsigned char)s[a];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            a++;
        } else {
            break;
        }
    }
    while (b > a) {
        unsigned char c = (unsigned char)s[b - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            b--;
        } else {
            break;
        }
    }
    *out = s + a;
    *out_len = b - a;
}

/* A trimmed, length-checked preset name into *out (owned), or an error of `cmd`. */
static pc_status dp_checked_name(const char *name, size_t len, const char *cmd, char **out,
                                 pc_error *err)
{
    const char *t;
    size_t tl;
    dp_trim(name, len, &t, &tl);
    size_t chars = dp_char_count(t, tl);
    if (tl == 0 || chars > PC_PRESET_MAX_NAME) {
        return pc_engine_err_bad_params(
            err, cmd, "Preset names must contain 1\xe2\x80\x93" "255 characters after trimming");
    }
    char *dup = pc_strndup(t, tl);
    if (dup == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = dup;
    return PC_OK;
}

static bool dp_eq_ascii_ci(const char *a, const char *b)
{
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca += 'a' - 'A';
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 'a' - 'A';
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

/* Validate a settings value; returns an English message or NULL (document_preset_cmds::validate). */
static const char *ds_validate(const pc_document_settings *s)
{
    if (s->width < 1 || s->width > PC_DP_MAX_DIMENSION || s->height < 1 ||
        s->height > PC_DP_MAX_DIMENSION) {
        return "Width and height must be within 1..=300000 pixels";
    }
    if (!isfinite(s->resolution) || s->resolution < 1.0f || s->resolution > (float)PC_DP_MAX_RESOLUTION) {
        return "Resolution must be within 1..=30000 ppi";
    }
    pc_color_mode m;
    pc_sample_type st;
    if (!dp_color_mode(s->mode, strlen(s->mode), &m) || !dp_sample_type(s->depth, &st)) {
        return "Use RGB, Grayscale, CMYK or Lab at 8, 16 or 32 bits";
    }
    if (!dp_background_valid(s->background, strlen(s->background))) {
        return "Unknown background choice or invalid colour";
    }
    if (strcmp(s->background, "backgroundColor") == 0 && !s->has_bg_color) {
        return "A saved Background Color needs its colour value";
    }
    if (s->has_bg_color) {
        for (int i = 0; i < 3; i++) {
            if (!isfinite(s->bg_color[i]) || s->bg_color[i] < 0.0f || s->bg_color[i] > 1.0f) {
                return "Background colour components must be within 0..1";
            }
        }
    }
    bool unit_ok = strcmp(s->unit, "px") == 0 || strcmp(s->unit, "in") == 0 ||
                   strcmp(s->unit, "cm") == 0 || strcmp(s->unit, "mm") == 0 ||
                   strcmp(s->unit, "pt") == 0 || strcmp(s->unit, "pica") == 0;
    bool ru_ok = strcmp(s->resolution_unit, "in") == 0 || strcmp(s->resolution_unit, "cm") == 0;
    if (!unit_ok || !ru_ok) {
        return "Unknown dimension or resolution display unit";
    }
    return NULL;
}

/* A string field of `p` into a freshly duplicated string, or `def` duplicated when absent/empty. */
static char *dp_str_field(yyjson_val *p, const char *key, const char *def)
{
    yyjson_val *v = pc_json_get(p, key);
    size_t len = 0;
    const char *s = v != NULL ? pc_json_as_str(v, &len) : NULL;
    if (s != NULL) {
        return pc_strndup(s, len);
    }
    return dp_dup(def);
}

/* Build settings from a params object (document_preset_cmds::DocumentSettings::from_params). */
static pc_status ds_from_params(const pc_engine_session *sess, yyjson_val *p, const char *cmd,
                                pc_document_settings *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    if (p == NULL || !yyjson_is_obj(p)) {
        return pc_engine_err_bad_params(err, cmd, "`settings` must be an object");
    }
    /* width / height: accept floats, round, 1..=MAX. */
    for (int k = 0; k < 2; k++) {
        const char *key = k == 0 ? "width" : "height";
        double n;
        if (!pc_json_as_f64(pc_json_get(p, key), &n) || !isfinite(n) || n < 1.0 ||
            n > (double)PC_DP_MAX_DIMENSION) {
            ds_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`%s` must be within 1..=300000 pixels", key);
        }
        uint32_t rv = (uint32_t)(n + 0.5);
        if (k == 0) {
            out->width = rv;
        } else {
            out->height = rv;
        }
    }
    double res = 72.0;
    double rv;
    if (pc_json_as_f64(pc_json_get(p, "resolution"), &rv)) {
        res = rv;
    }
    out->resolution = (float)res;
    uint64_t depth = 8;
    (void)pc_json_as_u64(pc_json_get(p, "depth"), &depth);
    out->depth = depth > 255 ? 0 : (uint8_t)depth;
    out->mode = dp_str_field(p, "mode", "rgb");
    out->background = dp_str_field(p, "background", "white");
    out->unit = dp_str_field(p, "unit", "px");
    out->resolution_unit = dp_str_field(p, "resolutionUnit", "in");
    if (out->mode == NULL || out->background == NULL || out->unit == NULL ||
        out->resolution_unit == NULL) {
        ds_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (strcmp(out->mode, "grayscale") == 0) {
        free(out->mode);
        out->mode = dp_dup("gray");
        if (out->mode == NULL) {
            ds_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    if (strcmp(out->background, "backgroundColor") == 0) {
        float c[4] = {0};
        pc_status rc = dp_background_color(p, pc_engine_session_tools(sess)->background, cmd, c, err);
        if (rc != PC_OK) {
            ds_fini(out);
            return rc;
        }
        out->has_bg_color = true;
        out->bg_color[0] = c[0];
        out->bg_color[1] = c[1];
        out->bg_color[2] = c[2];
    }
    const char *msg = ds_validate(out);
    if (msg != NULL) {
        ds_fini(out);
        return pc_engine_err_bad_params(err, cmd, "%s", msg);
    }
    return PC_OK;
}

/* The file.new creation params (document_preset_cmds::command_params), excluding name + display
 * units. Returns a mutable object owned by `d`. */
static yyjson_mut_val *ds_command_params(const pc_document_settings *s, yyjson_mut_doc *d)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "width", s->width);
    yyjson_mut_obj_add_uint(d, o, "height", s->height);
    yyjson_mut_obj_add_real(d, o, "resolution", (double)s->resolution);
    yyjson_mut_obj_add_strcpy(d, o, "mode", s->mode);
    yyjson_mut_obj_add_uint(d, o, "depth", s->depth);
    yyjson_mut_obj_add_strcpy(d, o, "background", s->background);
    if (s->has_bg_color) {
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        for (int i = 0; i < 3; i++) {
            yyjson_mut_arr_add_real(d, arr, (double)s->bg_color[i]);
        }
        yyjson_mut_obj_add_val(d, o, "backgroundColor", arr);
    }
    return o;
}

/* The full serde form (all fields, camelCase), a mutable object owned by `d`. */
static yyjson_mut_val *ds_to_json(const pc_document_settings *s, yyjson_mut_doc *d)
{
    yyjson_mut_val *o = ds_command_params(s, d);
    yyjson_mut_obj_add_strcpy(d, o, "unit", s->unit);
    yyjson_mut_obj_add_strcpy(d, o, "resolutionUnit", s->resolution_unit);
    return o;
}

static yyjson_mut_val *preset_to_json(const pc_document_preset *pr, yyjson_mut_doc *d)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, o, "name", pr->name);
    yyjson_mut_obj_add_val(d, o, "settings", ds_to_json(&pr->settings, d));
    return o;
}

/* Deserialize settings from a stored value (no session; grayscale normalised; not validated). */
static bool ds_from_json(yyjson_val *v, pc_document_settings *out)
{
    memset(out, 0, sizeof *out);
    if (v == NULL || !yyjson_is_obj(v)) {
        return false;
    }
    double w, h;
    if (!pc_json_as_f64(pc_json_get(v, "width"), &w) || !pc_json_as_f64(pc_json_get(v, "height"), &h)) {
        return false;
    }
    if (!isfinite(w) || !isfinite(h) || w < 0.0 || h < 0.0 || w > 4.0e9 || h > 4.0e9) {
        return false;
    }
    out->width = (uint32_t)w;
    out->height = (uint32_t)h;
    double res = 72.0;
    (void)pc_json_as_f64(pc_json_get(v, "resolution"), &res);
    out->resolution = (float)res;
    uint64_t depth = 8;
    (void)pc_json_as_u64(pc_json_get(v, "depth"), &depth);
    out->depth = depth > 255 ? 0 : (uint8_t)depth;
    out->mode = dp_str_field(v, "mode", "rgb");
    out->background = dp_str_field(v, "background", "white");
    out->unit = dp_str_field(v, "unit", "px");
    out->resolution_unit = dp_str_field(v, "resolutionUnit", "in");
    if (out->mode == NULL || out->background == NULL || out->unit == NULL ||
        out->resolution_unit == NULL) {
        ds_fini(out);
        return false;
    }
    if (strcmp(out->mode, "grayscale") == 0) {
        free(out->mode);
        out->mode = dp_dup("gray");
        if (out->mode == NULL) {
            ds_fini(out);
            return false;
        }
    }
    yyjson_val *bc = pc_json_get(v, "backgroundColor");
    if (bc != NULL && yyjson_is_arr(bc) && yyjson_arr_size(bc) == 3) {
        bool ok = true;
        for (size_t i = 0; i < 3; i++) {
            double c;
            if (!pc_json_as_f64(yyjson_arr_get(bc, i), &c)) {
                ok = false;
                break;
            }
            out->bg_color[i] = (float)c;
        }
        out->has_bg_color = ok;
        if (!ok) {
            ds_fini(out);
            return false;
        }
    }
    return true;
}

/* Whether two settings yield the same file.new command params (recent dedup). */
static bool dp_streq(const char *a, const char *b)
{
    return strcmp(a != NULL ? a : "", b != NULL ? b : "") == 0;
}

static bool ds_params_equal(const pc_document_settings *a, const pc_document_settings *b)
{
    if (a->width != b->width || a->height != b->height || a->depth != b->depth ||
        a->resolution != b->resolution || !dp_streq(a->mode, b->mode) ||
        !dp_streq(a->background, b->background) || a->has_bg_color != b->has_bg_color) {
        return false;
    }
    if (a->has_bg_color) {
        for (int i = 0; i < 3; i++) {
            if (a->bg_color[i] != b->bg_color[i]) {
                return false;
            }
        }
    }
    return true;
}

/* ---- persistence (called from presets_persist.c) ------------------------------------------ */

void pc_engine_document_presets_fini(pc_preset_state *st)
{
    if (st == NULL) {
        return;
    }
    for (size_t i = 0; i < st->ndocuments; i++) {
        free(st->documents[i].name);
        ds_fini(&st->documents[i].settings);
    }
    free(st->documents);
    st->documents = NULL;
    st->ndocuments = 0;
    for (size_t i = 0; i < st->nrecent; i++) {
        ds_fini(&st->recent_documents[i]);
    }
    free(st->recent_documents);
    st->recent_documents = NULL;
    st->nrecent = 0;
}

void pc_engine_document_presets_to_json(const pc_preset_state *st, yyjson_mut_doc *d,
                                        yyjson_mut_val *root)
{
    yyjson_mut_val *docs = yyjson_mut_arr(d);
    for (size_t i = 0; i < st->ndocuments; i++) {
        yyjson_mut_arr_append(docs, preset_to_json(&st->documents[i], d));
    }
    yyjson_mut_obj_add_val(d, root, "documents", docs);
    yyjson_mut_val *rec = yyjson_mut_arr(d);
    for (size_t i = 0; i < st->nrecent; i++) {
        yyjson_mut_arr_append(rec, ds_to_json(&st->recent_documents[i], d));
    }
    yyjson_mut_obj_add_val(d, root, "recent_documents", rec);
}

void pc_engine_document_presets_load(pc_preset_state *st, yyjson_val *v)
{
    if (v == NULL || !yyjson_is_obj(v)) {
        return;
    }
    yyjson_val *docs = pc_json_get(v, "documents");
    if (docs != NULL && yyjson_is_arr(docs)) {
        pc_document_preset *nv = NULL;
        size_t nn = 0, cap = 0;
        size_t idx, mx;
        yyjson_val *el;
        yyjson_arr_foreach(docs, idx, mx, el) {
            if (nn >= PC_DP_MAX_PRESETS) {
                break;
            }
            size_t nl = 0;
            const char *nm = pc_json_as_str(pc_json_get(el, "name"), &nl);
            if (nm == NULL) {
                continue;
            }
            const char *t;
            size_t tl;
            dp_trim(nm, nl, &t, &tl);
            if (tl == 0 || dp_char_count(t, tl) > PC_PRESET_MAX_NAME) {
                continue;
            }
            pc_document_settings s;
            if (!ds_from_json(pc_json_get(el, "settings"), &s)) {
                continue;
            }
            if (ds_validate(&s) != NULL) {
                ds_fini(&s);
                continue;
            }
            char *name = pc_strndup(t, tl);
            if (name == NULL) {
                ds_fini(&s);
                break;
            }
            bool dup = false;
            for (size_t i = 0; i < nn; i++) {
                if (dp_eq_ascii_ci(nv[i].name, name)) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                free(name);
                ds_fini(&s);
                continue;
            }
            pc_document_preset *g2 = pc_grow_array(nv, &cap, nn + 1, sizeof *g2);
            if (g2 == NULL) {
                free(name);
                ds_fini(&s);
                break;
            }
            nv = g2;
            nv[nn].name = name;
            nv[nn].settings = s;
            nn++;
        }
        for (size_t i = 0; i < st->ndocuments; i++) {
            free(st->documents[i].name);
            ds_fini(&st->documents[i].settings);
        }
        free(st->documents);
        st->documents = nv;
        st->ndocuments = nn;
    }
    yyjson_val *rec = pc_json_get(v, "recent_documents");
    if (rec != NULL && yyjson_is_arr(rec)) {
        pc_document_settings *nv = NULL;
        size_t nn = 0, cap = 0;
        size_t idx, mx;
        yyjson_val *el;
        yyjson_arr_foreach(rec, idx, mx, el) {
            if (nn >= PC_DP_MAX_RECENT) {
                break;
            }
            pc_document_settings s;
            if (!ds_from_json(el, &s)) {
                continue;
            }
            if (ds_validate(&s) != NULL) {
                ds_fini(&s);
                continue;
            }
            bool dup = false;
            for (size_t i = 0; i < nn; i++) {
                if (ds_params_equal(&nv[i], &s)) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                ds_fini(&s);
                continue;
            }
            pc_document_settings *g2 = pc_grow_array(nv, &cap, nn + 1, sizeof *g2);
            if (g2 == NULL) {
                ds_fini(&s);
                break;
            }
            nv = g2;
            nv[nn++] = s;
        }
        for (size_t i = 0; i < st->nrecent; i++) {
            ds_fini(&st->recent_documents[i]);
        }
        free(st->recent_documents);
        st->recent_documents = nv;
        st->nrecent = nn;
    }
}

/* ---- commands ----------------------------------------------------------------------------- */

static pc_status dp_result(pc_json_value *out, yyjson_mut_doc **d, yyjson_mut_val **root, pc_error *err)
{
    if (pc_engine_result_doc(out, d, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    *root = yyjson_mut_obj(*d);
    yyjson_mut_doc_set_root(*d, *root);
    return PC_OK;
}

static pc_status dp_req_name(yyjson_val *p, const char *cmd, const char **out, size_t *len,
                             pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "name");
    size_t l = 0;
    const char *s = v != NULL ? pc_json_as_str(v, &l) : NULL;
    if (s == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing \"name\"");
    }
    *out = s;
    *len = l;
    return PC_OK;
}

static pc_status run_presets_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    yyjson_mut_doc *d = NULL;
    yyjson_mut_val *root = NULL;
    pc_status rc = dp_result(out, &d, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->presets.ndocuments; i++) {
        yyjson_mut_arr_append(arr, preset_to_json(&s->presets.documents[i], d));
    }
    yyjson_mut_obj_add_val(d, root, "presets", arr);
    return PC_OK;
}

static pc_status run_presets_get(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *name = NULL;
    size_t nl = 0;
    pc_status rc = dp_req_name(p, "document.presets.get", &name, &nl, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *tn;
    size_t tl;
    dp_trim(name, nl, &tn, &tl);
    char *key = pc_strndup(tn, tl);
    if (key == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    const pc_document_preset *pr = NULL;
    for (size_t i = 0; i < s->presets.ndocuments; i++) {
        if (dp_eq_ascii_ci(s->presets.documents[i].name, key)) {
            pr = &s->presets.documents[i];
            break;
        }
    }
    free(key);
    if (pr == NULL) {
        return pc_engine_err_bad_params(err, "document.presets.get", "No such document preset");
    }
    yyjson_mut_doc *d = NULL;
    yyjson_mut_val *root = NULL;
    rc = dp_result(out, &d, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_obj_add_val(d, root, "preset", preset_to_json(pr, d));
    yyjson_mut_obj_add_val(d, root, "params", ds_command_params(&pr->settings, d));
    return PC_OK;
}

static pc_status run_presets_save(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "document.presets.save";
    const char *name = NULL;
    size_t nl = 0;
    pc_status rc = dp_req_name(p, CMD, &name, &nl, err);
    if (rc != PC_OK) {
        return rc;
    }
    char *checked = NULL;
    rc = dp_checked_name(name, nl, CMD, &checked, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < s->presets.ndocuments; i++) {
        if (dp_eq_ascii_ci(s->presets.documents[i].name, checked)) {
            rc = pc_engine_err_bad_params(
                err, CMD, "A preset named \"%s\" already exists; choose another name", checked);
            free(checked);
            return rc;
        }
    }
    if (s->presets.ndocuments >= PC_DP_MAX_PRESETS) {
        free(checked);
        return pc_engine_err_bad_params(err, CMD, "At most %d document presets can be saved",
                                        PC_DP_MAX_PRESETS);
    }
    pc_document_settings settings;
    rc = ds_from_params(s, pc_json_get(p, "settings"), CMD, &settings, err);
    if (rc != PC_OK) {
        free(checked);
        return rc;
    }
    size_t cap = s->presets.ndocuments;
    pc_document_preset *g2 =
        pc_grow_array(s->presets.documents, &cap, s->presets.ndocuments + 1, sizeof *g2);
    if (g2 == NULL) {
        free(checked);
        ds_fini(&settings);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s->presets.documents = g2;
    pc_document_preset *pr = &s->presets.documents[s->presets.ndocuments];
    pr->name = checked;
    pr->settings = settings;
    s->presets.ndocuments++;
    yyjson_mut_doc *d = NULL;
    yyjson_mut_val *root = NULL;
    rc = dp_result(out, &d, &root, err);
    if (rc != PC_OK) {
        /* The preset is saved; a result-OOM is still an error but state stays consistent. */
        return rc;
    }
    yyjson_mut_obj_add_val(d, root, "preset", preset_to_json(pr, d));
    pc_engine_presets_changed(s);
    return PC_OK;
}

static pc_status run_presets_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                    pc_error *err)
{
    const char *CMD = "document.presets.delete";
    const char *name = NULL;
    size_t nl = 0;
    pc_status rc = dp_req_name(p, CMD, &name, &nl, err);
    if (rc != PC_OK) {
        return rc;
    }
    const char *tn;
    size_t tl;
    dp_trim(name, nl, &tn, &tl);
    char *key = pc_strndup(tn, tl);
    if (key == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    size_t idx = s->presets.ndocuments;
    for (size_t i = 0; i < s->presets.ndocuments; i++) {
        if (dp_eq_ascii_ci(s->presets.documents[i].name, key)) {
            idx = i;
            break;
        }
    }
    if (idx == s->presets.ndocuments) {
        free(key);
        return pc_engine_err_bad_params(err, CMD, "No such document preset");
    }
    free(s->presets.documents[idx].name);
    ds_fini(&s->presets.documents[idx].settings);
    for (size_t i = idx + 1; i < s->presets.ndocuments; i++) {
        s->presets.documents[i - 1] = s->presets.documents[i];
    }
    s->presets.ndocuments--;
    yyjson_mut_doc *d = NULL;
    yyjson_mut_val *root = NULL;
    rc = dp_result(out, &d, &root, err);
    if (rc != PC_OK) {
        free(key);
        return rc;
    }
    yyjson_mut_obj_add_strcpy(d, root, "deleted", key);
    free(key);
    pc_engine_presets_changed(s);
    return PC_OK;
}

static pc_status run_recent_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    yyjson_mut_doc *d = NULL;
    yyjson_mut_val *root = NULL;
    pc_status rc = dp_result(out, &d, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->presets.nrecent; i++) {
        yyjson_mut_arr_append(arr, ds_to_json(&s->presets.recent_documents[i], d));
    }
    yyjson_mut_obj_add_val(d, root, "recent", arr);
    return PC_OK;
}

static pc_status run_recent_add(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "document.recent.add";
    pc_document_settings settings;
    pc_status rc = ds_from_params(s, pc_json_get(p, "settings"), CMD, &settings, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Remove an equal older entry. */
    size_t w = 0;
    for (size_t i = 0; i < s->presets.nrecent; i++) {
        if (ds_params_equal(&s->presets.recent_documents[i], &settings)) {
            ds_fini(&s->presets.recent_documents[i]);
        } else {
            s->presets.recent_documents[w++] = s->presets.recent_documents[i];
        }
    }
    s->presets.nrecent = w;
    /* Insert at the front. */
    size_t cap = s->presets.nrecent;
    pc_document_settings *g2 =
        pc_grow_array(s->presets.recent_documents, &cap, s->presets.nrecent + 1, sizeof *g2);
    if (g2 == NULL) {
        ds_fini(&settings);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s->presets.recent_documents = g2;
    for (size_t i = s->presets.nrecent; i > 0; i--) {
        s->presets.recent_documents[i] = s->presets.recent_documents[i - 1];
    }
    s->presets.recent_documents[0] = settings;
    s->presets.nrecent++;
    /* Truncate to the cap. */
    while (s->presets.nrecent > PC_DP_MAX_RECENT) {
        ds_fini(&s->presets.recent_documents[s->presets.nrecent - 1]);
        s->presets.nrecent--;
    }
    pc_engine_presets_changed(s);
    yyjson_mut_doc *d = NULL;
    yyjson_mut_val *root = NULL;
    rc = dp_result(out, &d, &root, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->presets.nrecent; i++) {
        yyjson_mut_arr_append(arr, ds_to_json(&s->presets.recent_documents[i], d));
    }
    yyjson_mut_obj_add_val(d, root, "recent", arr);
    return PC_OK;
}

static const char *const MENU_FILE[] = {"File"};

const pc_engine_command *pc_engine_document_preset_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"file.new", "New\xe2\x80\xa6", MENU_FILE, 1, "Cmd+N",
         "{\"width\":1..300000=1920,\"height\":1..300000=1080 (pixel counts are rounded),"
         "\"mode\":\"rgb|gray|grayscale|cmyk|lab\"=\"rgb\",\"depth\":8|16|32=8,"
         "\"background\":\"white|black|backgroundColor|transparent|#rrggbb\"=\"white\","
         "\"backgroundColor\":[r,g,b]? (0..1; defaults to toolbox colour),"
         "\"resolution\":1..30000=72 (ppi),\"name\":str}",
         pc_engine_always, run_file_new, true},
        {"document.presets.list", "List Document Presets", NULL, 0, NULL, "{}", pc_engine_always,
         run_presets_list, false},
        {"document.presets.get", "Get Document Preset", NULL, 0, NULL,
         "{\"name\":str} -> {preset,params} (params for file.new; no document is created)",
         pc_engine_always, run_presets_get, false},
        {"document.presets.save", "Save Document Preset", NULL, 0, NULL,
         "{\"name\":str (trimmed, ASCII case-insensitive unique, 1..255 characters),"
         "\"settings\":{\"width\":px,\"height\":px,\"resolution\":ppi=72,"
         "\"mode\":\"rgb|gray|cmyk|lab\"=\"rgb\",\"depth\":8|16|32=8,"
         "\"background\":\"white|black|transparent|backgroundColor|#rrggbb\"=\"white\","
         "\"backgroundColor\":[r,g,b]?,\"unit\":\"px|in|cm|mm|pt|pica\"=\"px\","
         "\"resolutionUnit\":\"in|cm\"=\"in\"}}",
         pc_engine_always, run_presets_save, true},
        {"document.presets.delete", "Delete Document Preset", NULL, 0, NULL, "{\"name\":str}",
         pc_engine_always, run_presets_delete, true},
        {"document.recent.list", "List Recent Document Settings", NULL, 0, NULL,
         "{} -> {recent:[settings]} (newest first; each settings' command params minus "
         "unit/resolutionUnit go to file.new)",
         pc_engine_always, run_recent_list, false},
        {"document.recent.add", "Add Recent Document Settings", NULL, 0, NULL,
         "{\"settings\":{same fields as document.presets.save}} (newest first, max 20, duplicates "
         "collapsed)",
         pc_engine_always, run_recent_add, false},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
