/* Displace smart filters <-> filterFX items (port of crates/io/src/displace_map.rs). The
 * reference goes through serde_json::Value for the params; here yyjson builds the same object.
 * See displace_map.h. */
#include "io/displace_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "io/smart_internal.h"

/* tdta header: height, width (u32 LE) and mode (u16 LE). */
#define DSP_HEADER 10u
/* The only map layout seen in Photoshop files: two 8-bit planes. */
#define DSP_MODE_TWO_PLANES 3u
/* 8K x 8K: far beyond the observed document-sized maps, and bounds the f32 map we build. */
#define DSP_MAX_MAP_PIXELS ((size_t)1 << 26)
/* The template is the item plus the map; a little room for the rest of the descriptor. */
#define DSP_MAX_TEMPLATE_BYTES (2 * DSP_MAX_MAP_PIXELS + ((size_t)1 << 20))
/* code(b"Dspl"): the big-endian i32 of the class. */
#define DSP_FILTER_ID ((int32_t)0x4473706c)
#define TEMPLATE_KEY "__displacePsd"
#define MAP_KEY "DspD"

#define UNSUP(err, ...) pc_fail((err), PC_ERR_UNSUPPORTED, __VA_ARGS__)

/* A parsed embedded map: size and the two 8-bit planes (borrowed from the blob). */
typedef struct dsp_map {
    size_t width;
    size_t height;
    const uint8_t *horizontal;
    const uint8_t *vertical;
} dsp_map;

/* (PSD enum bytes, our string) tables for the fit (DspM) and undefined areas (UndA). */
typedef struct dsp_choice {
    const char *code; /* 4 bytes */
    const char *name;
} dsp_choice;
static const dsp_choice FITS[2] = {{"StrF", "stretch"}, {"Tile", "tile"}};
static const dsp_choice UNDEFINED[2] = {{"RptE", "repeat"}, {"WrpA", "wrap"}};

/* forward declarations (definitions follow their first use) */
static bool int_of_item(const pc_psd_desc *d, int32_t *out);
static double dsp_scale(yyjson_mut_val *obj, const char *k, double d, pc_status *st, pc_error *err);
static const char *dsp_text(yyjson_mut_val *obj, const char *k, const char *d, pc_status *st, pc_error *err);

/* Parses an embedded displacement map; false for any other mode, a zero or oversized map, or a
 * blob shorter than the header plus both planes. */
static bool parse_map(const uint8_t *data, size_t len, dsp_map *out)
{
    uint32_t h32;
    uint32_t w32;
    uint16_t mode;
    size_t width;
    size_t height;
    size_t n;
    size_t two_n;
    if (len < DSP_HEADER) {
        return false;
    }
    h32 = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
    w32 = (uint32_t)data[4] | (uint32_t)data[5] << 8 | (uint32_t)data[6] << 16 | (uint32_t)data[7] << 24;
    mode = (uint16_t)((uint16_t)data[8] | (uint16_t)data[9] << 8);
    height = (size_t)h32;
    width = (size_t)w32;
    if (!pc_size_mul(width, height, &n)) {
        return false;
    }
    if (mode != DSP_MODE_TWO_PLANES || n == 0 || n > DSP_MAX_MAP_PIXELS) {
        return false;
    }
    if (!pc_size_mul(n, 2, &two_n) || len < DSP_HEADER + two_n) {
        return false;
    }
    out->width = width;
    out->height = height;
    out->horizontal = data + DSP_HEADER;
    out->vertical = data + DSP_HEADER + n;
    return true;
}

/* The embedded map of a filterFX item's Fltr descriptor, or false. */
static bool fltr_map(const pc_psd_desc *item, dsp_map *out)
{
    const pc_psd_desc *f = pc_io3_get_desc(item, "Fltr");
    const pc_psd_value *v = f != NULL ? pc_psd_desc_get(f, MAP_KEY) : NULL;
    if (v == NULL || v->kind != PC_PSD_V_RAW_DATA) {
        return false;
    }
    return parse_map(v->raw.data, v->raw.len, out);
}

/* lookup(table, enum bytes) -> our string, or NULL. */
static const char *lookup(const dsp_choice *table, const uint8_t *v, size_t n)
{
    size_t i;
    if (v == NULL || n != 4) {
        return NULL;
    }
    for (i = 0; i < 2; i++) {
        if (memcmp(table[i].code, v, 4) == 0) {
            return table[i].name;
        }
    }
    return NULL;
}

/* A finite scale in [-999, 999] under key, or false. */
static bool scale_of(const pc_psd_desc *f, const char *key, double *out)
{
    double v;
    if (!pc_io3_num(pc_psd_desc_get(f, key), &v) || !isfinite(v) || v < -999.0 || v > 999.0) {
        return false;
    }
    *out = v;
    return true;
}

/* The editable settings of a Dspl descriptor; false when one is missing or unknown. */
static bool settings(const pc_psd_desc *f, double *horizontal, double *vertical, const char **fit,
                     const char **undefined)
{
    size_t dn = 0;
    size_t un = 0;
    const uint8_t *dsp_m = pc_io3_enum_of(f, "DspM", &dn);
    const uint8_t *und_a = pc_io3_enum_of(f, "UndA", &un);
    if (!scale_of(f, "HrzS", horizontal) || !scale_of(f, "VrtS", vertical)) {
        return false;
    }
    *fit = lookup(FITS, dsp_m, dn);
    *undefined = lookup(UNDEFINED, und_a, un);
    return *fit != NULL && *undefined != NULL;
}

/* blend_options of a neutral filter: 100 % opacity, BlnM "normal". */
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

/* filterID as an integer, or false. */
static bool int_of_item(const pc_psd_desc *d, int32_t *out)
{
    const pc_psd_value *v = pc_psd_desc_get(d, "filterID");
    if (v == NULL || v->kind != PC_PSD_V_INTEGER) {
        return false;
    }
    *out = v->i;
    return true;
}

/* ---- import ---- */

pc_status pc_io_displace_import_params(const pc_psd_desc *item, bool *found, pc_json_value *out,
                                       pc_error *err)
{
    const pc_psd_desc *f = pc_io3_get_desc(item, "Fltr");
    double horizontal;
    double vertical;
    const char *fit;
    const char *undefined;
    dsp_map map;
    pc_psd_desc template;
    pc_psd_desc blend;
    pc_psd_value v;
    pc_psd_bytes bytes = {NULL, 0};
    yyjson_mut_doc *doc = NULL;
    yyjson_mut_val *root;
    pc_json_value res = PC_JSON_VALUE_INIT;
    char *hex = NULL;
    pc_status st;
    *found = false;
    if (f == NULL || !pc_psd_id_is(&f->class_id, PC_IO_DISPLACE_CLASS)) {
        return PC_OK;
    }
    if (!settings(f, &horizontal, &vertical, &fit, &undefined) || !fltr_map(item, &map)) {
        return PC_OK;
    }
    /* Neutral blending in the template: the model's own fields replace it on export. */
    memset(&template, 0, sizeof template);
    st = pc_psd_desc_clone(item, &template, err);
    if (st == PC_OK) {
        st = neutral_blend_options(&blend, err);
        if (st == PC_OK) {
            st = pc_psd_value_object(&blend, &v, err);
            if (st == PC_OK) {
                st = pc_io3_set(&template, "blendOptions", &v, err);
            }
        }
    }
    if (st == PC_OK) {
        v = pc_psd_value_bool(true);
        st = pc_io3_set(&template, "enab", &v, err);
    }
    if (st == PC_OK) {
        st = pc_psd_desc_to_bytes(&template, &bytes, err);
    }
    if (st == PC_OK) {
        st = pc_io3_to_hex(bytes.data, bytes.len, &hex, err);
    }
    if (st != PC_OK) {
        goto cleanup;
    }
    doc = yyjson_mut_doc_new(NULL);
    root = doc != NULL ? yyjson_mut_obj(doc) : NULL;
    if (root == NULL || !yyjson_mut_obj_add_real(doc, root, "horizontal", horizontal) ||
        !yyjson_mut_obj_add_real(doc, root, "vertical", vertical) ||
        !yyjson_mut_obj_add_strcpy(doc, root, "fit", fit) ||
        !yyjson_mut_obj_add_strcpy(doc, root, "undefinedAreas", undefined) ||
        !yyjson_mut_obj_add_strcpy(doc, root, TEMPLATE_KEY, hex)) {
        st = pc_io_nomem(err, "encoding Displace settings");
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
    pc_psd_bytes_fini(&bytes);
    pc_psd_desc_fini(&template);
    return st;
}

/* ---- export ---- */

/* The template item of params, checked to be a Displace filter with a readable map. */
static pc_status template_item(yyjson_mut_val *obj, pc_psd_desc *out, pc_error *err)
{
    yyjson_mut_val *raw = yyjson_mut_obj_get(obj, TEMPLATE_KEY);
    const char *s;
    size_t len;
    uint8_t *bytes = NULL;
    size_t n = 0;
    bool ok = false;
    dsp_map map;
    int32_t id = 0;
    pc_psd_desc item;
    pc_error pe;
    pc_status st;
    if (raw == NULL) {
        return UNSUP(err, "a Displace map from a file or document is not written to PSD");
    }
    if (!yyjson_mut_is_str(raw)) {
        return UNSUP(err, "the Displace PSD data must be hex text");
    }
    s = yyjson_mut_get_str(raw);
    len = yyjson_mut_get_len(raw);
    if (len > DSP_MAX_TEMPLATE_BYTES * 2) {
        return UNSUP(err, "the Displace PSD data is too large");
    }
    st = pc_io3_from_hex(s, len, &ok, &bytes, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (!ok) {
        return UNSUP(err, "the Displace PSD data is not valid hex");
    }
    st = pc_psd_desc_parse(bytes, n, &item, &pe);
    free(bytes);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", pe.msg);
    }
    if (st != PC_OK) {
        return UNSUP(err, "the Displace PSD data is unreadable (%s)", pe.msg);
    }
    if (!pc_psd_id_is(&item.class_id, "filterFX") || !(int_of_item(&item, &id) && id == DSP_FILTER_ID) ||
        !fltr_map(&item, &map)) {
        pc_psd_desc_fini(&item);
        return UNSUP(err, "the Displace PSD data is not a Displace filter with an embedded map");
    }
    *out = item;
    return PC_OK;
}

/* The enum value of `name` in `table`, or false; *code receives the 4 PSD bytes. */
static bool choice_code(const dsp_choice *table, const char *name, const char **code)
{
    size_t i;
    if (name == NULL) {
        return false;
    }
    for (i = 0; i < 2; i++) {
        if (strcmp(table[i].name, name) == 0) {
            *code = table[i].code;
            return true;
        }
    }
    return false;
}

/* Keep the template's enum type; only the value changes (set_enum of the reference). */
static pc_status set_enum(pc_psd_desc *f, const char *key, const char *value_code, pc_error *err)
{
    const pc_psd_value *cur = pc_psd_desc_get(f, key);
    pc_psd_value v;
    pc_status st;
    memset(&v, 0, sizeof v);
    if (cur != NULL && cur->kind == PC_PSD_V_ENUMERATED) {
        st = pc_psd_value_clone(cur, &v, err); /* keeps the existing type_id */
        if (st == PC_OK) {
            pc_psd_id_fini(&v.value);
            st = pc_psd_id_make(value_code, &v.value, err);
        }
    } else {
        st = pc_psd_value_enum(key, value_code, &v, err);
    }
    if (st != PC_OK) {
        pc_psd_value_fini(&v);
        return st;
    }
    return pc_io3_set(f, key, &v, err);
}

pc_status pc_io_displace_export_item(const pc_json_value *params, pc_psd_desc *out, pc_error *err)
{
    static const char *const ALLOWED[4] = {"horizontal", "vertical", "fit", "undefinedAreas"};
    yyjson_mut_val *obj = pc_json_value_root(params);
    yyjson_mut_obj_iter it;
    yyjson_mut_val *key;
    pc_psd_desc item;
    const pc_psd_desc *src_f;
    pc_psd_desc f;
    pc_psd_value fv;
    double hv;
    double vv;
    const char *fit;
    const char *undefined;
    const char *fit_code = NULL;
    const char *und_code = NULL;
    pc_psd_value iv;
    pc_status st;
    if (obj == NULL || !yyjson_mut_is_obj(obj)) {
        return UNSUP(err, "Displace parameters must be an object");
    }
    /* Any key that is not meta and not one of the four editable settings has no PSD equivalent. */
    if (yyjson_mut_obj_iter_init(obj, &it)) {
        while ((key = yyjson_mut_obj_iter_next(&it)) != NULL) {
            const char *k = yyjson_mut_get_str(key);
            size_t kn = yyjson_mut_get_len(key);
            size_t i;
            bool allowed = false;
            if (k == NULL) {
                continue;
            }
            if ((kn >= 2 && k[0] == '_' && k[1] == '_') || (kn == 5 && memcmp(k, "layer", 5) == 0)) {
                continue; /* is_meta */
            }
            for (i = 0; i < 4; i++) {
                if (strcmp(k, ALLOWED[i]) == 0) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                return UNSUP(err, "Displace setting `%s` has no PSD equivalent next to its embedded map",
                             k);
            }
        }
    }
    st = template_item(obj, &item, err);
    if (st != PC_OK) {
        return st;
    }
    src_f = pc_io3_get_desc(&item, "Fltr");
    if (src_f == NULL) {
        pc_psd_desc_fini(&item);
        return UNSUP(err, "the Displace PSD data has no filter");
    }
    hv = dsp_scale(obj, "horizontal", 10.0, &st, err);
    if (st == PC_OK) {
        vv = dsp_scale(obj, "vertical", 10.0, &st, err);
    } else {
        vv = 0.0;
    }
    if (st == PC_OK) {
        fit = dsp_text(obj, "fit", "stretch", &st, err);
        if (st == PC_OK && !choice_code(FITS, fit, &fit_code)) {
            st = UNSUP(err, "Displace `fit` is not one of the PSD values");
        }
    }
    if (st == PC_OK) {
        undefined = dsp_text(obj, "undefinedAreas", "repeat", &st, err);
        if (st == PC_OK && !choice_code(UNDEFINED, undefined, &und_code)) {
            st = UNSUP(err, "Displace `undefinedAreas` is not one of the PSD values");
        }
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&item);
        return st;
    }
    st = pc_psd_desc_clone(src_f, &f, err);
    if (st == PC_OK) {
        iv = pc_psd_value_int((int32_t)hv);
        st = pc_io3_set(&f, "HrzS", &iv, err);
    }
    if (st == PC_OK) {
        iv = pc_psd_value_int((int32_t)vv);
        st = pc_io3_set(&f, "VrtS", &iv, err);
    }
    if (st == PC_OK) {
        st = set_enum(&f, "DspM", fit_code, err);
    }
    if (st == PC_OK) {
        st = set_enum(&f, "UndA", und_code, err);
    }
    if (st == PC_OK) {
        st = pc_psd_value_object(&f, &fv, err);
        if (st == PC_OK) {
            st = pc_io3_set(&item, "Fltr", &fv, err);
        }
    } else {
        pc_psd_desc_fini(&f);
    }
    if (st != PC_OK) {
        pc_psd_desc_fini(&item);
        return st;
    }
    *out = item;
    return PC_OK;
}

/* params[k] or default `d`, finite, rounded, clamped to [-999, 999]. *st carries the error. */
static double dsp_scale(yyjson_mut_val *obj, const char *k, double d, pc_status *st, pc_error *err)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, k);
    double x = d;
    *st = PC_OK;
    if (v != NULL) {
        if (!yyjson_mut_is_num(v)) {
            *st = UNSUP(err, "Displace `%s` must be a number", k);
            return 0.0;
        }
        x = yyjson_mut_get_num(v);
    }
    if (!isfinite(x)) {
        *st = UNSUP(err, "Displace `%s` must be a number", k);
        return 0.0;
    }
    x = round(x);
    return x < -999.0 ? -999.0 : x > 999.0 ? 999.0 : x;
}

/* params[k] as text, or default `d`. *st carries the error (not-text). */
static const char *dsp_text(yyjson_mut_val *obj, const char *k, const char *d, pc_status *st, pc_error *err)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(obj, k);
    *st = PC_OK;
    if (v == NULL) {
        return d;
    }
    if (!yyjson_mut_is_str(v)) {
        *st = UNSUP(err, "Displace `%s` must be text", k);
        return NULL;
    }
    return yyjson_mut_get_str(v);
}

/* ---- embedded map ---- */

pc_status pc_io_displace_embedded_map(const pc_json_value *params, bool *found, pc_algo_image *out,
                                      pc_error *err)
{
    yyjson_mut_val *obj = pc_json_value_root(params);
    pc_psd_desc item;
    dsp_map map;
    pc_rect rect;
    float *data;
    size_t px;
    size_t ch;
    size_t count;
    pc_error pe;
    pc_status st;
    *found = false;
    if (obj == NULL || !yyjson_mut_is_obj(obj)) {
        return PC_OK;
    }
    st = template_item(obj, &item, &pe);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", pe.msg);
    }
    if (st != PC_OK) {
        return PC_OK; /* unreadable template: None */
    }
    if (!fltr_map(&item, &map)) {
        pc_psd_desc_fini(&item);
        return PC_OK;
    }
    if (map.width > (size_t)INT32_MAX || map.height > (size_t)INT32_MAX) {
        pc_psd_desc_fini(&item);
        return PC_OK;
    }
    px = map.width * map.height; /* bounded by DSP_MAX_MAP_PIXELS in parse_map */
    ch = memcmp(map.horizontal, map.vertical, px) == 0 ? 1 : 3;
    rect = pc_rect_make(0, 0, (int32_t)map.width, (int32_t)map.height);
    count = px * ch;
    data = pc_calloc_array(count > 0 ? count : 1, sizeof *data);
    if (data == NULL) {
        pc_psd_desc_fini(&item);
        return pc_io_nomem(err, "building a Displace map");
    }
    if (ch == 1) {
        size_t i;
        for (i = 0; i < px; i++) {
            data[i] = (float)map.horizontal[i] / 255.0f;
        }
    } else {
        size_t i;
        for (i = 0; i < px; i++) {
            data[i * 3] = (float)map.horizontal[i] / 255.0f;
            data[i * 3 + 1] = (float)map.vertical[i] / 255.0f;
            data[i * 3 + 2] = 0.5f;
        }
    }
    st = pc_algo_image_wrap(out, rect, ch, data, count, err);
    pc_psd_desc_fini(&item);
    if (st != PC_OK) {
        free(data);
        return st;
    }
    *found = true;
    return PC_OK;
}
