/* serde-compatible JSON for the paint types (json.h): a small table-driven reader/writer.
 *
 * Messages are serde's and serde_json's for from_value::<T>(Value) (serde_json 1.0.149): unknown
 * keys are ignored, keys are visited in sorted order (a Value object is a BTreeMap), missing
 * fields are reported in declaration order after the visit, structs may be given as arrays
 * (visit_seq), enums are externally tagged. */
#include "paint/json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/str.h"
#include "base/utf8.h"
#include "color/blend.h"
#include "paint/bg_erase.h"
#include "paint/brush.h"
#include "paint/presets.h"
#include "paint/replace.h"
#include "paint/tile.h"

_Static_assert(sizeof(pc_paint_control) == sizeof(int), "C enums are stored as int");
_Static_assert(sizeof(pc_paint_mask_mode) == sizeof(int), "C enums are stored as int");
_Static_assert(sizeof(pc_paint_pattern_style) == sizeof(int), "C enums are stored as int");
_Static_assert(sizeof(pc_blend_mode) == sizeof(int), "C enums are stored as int");
_Static_assert(sizeof(pc_paint_replace_mode) == sizeof(int), "C enums are stored as int");
_Static_assert(sizeof(pc_paint_sampling) == sizeof(int), "C enums are stored as int");
_Static_assert(sizeof(pc_paint_limits) == sizeof(int), "C enums are stored as int");

/* ---- descriptors ---- */

enum kind {
    K_F32,
    K_F64,
    K_U32,
    K_U64,
    K_BOOL,
    K_STR,
    K_RGBA,
    K_ENUM,
    K_TIP,
    K_PATTERN,
    K_TILE,
    K_STRUCT,
    K_POINTS,
    K_STRS /* pc_paint_str_list */
};
enum def { D_REQ, D_FIELD, D_CONT };

typedef struct uenum {
    const char *(*name)(unsigned i);
    unsigned n;
} uenum;

typedef struct field {
    const char *name;
    const char *alias;
    unsigned char kind;
    unsigned char def;
    unsigned char skip_empty; /* skip_serializing_if: String/Vec::is_empty, or Not::not for a bool */
    size_t off;
    const void *aux; /* uenum (K_ENUM) or sdesc (K_STRUCT) */
} field;

typedef struct sdesc {
    const char *expecting; /* "struct BrushSettings" */
    const char *name;      /* "BrushSettings" (for "struct X with N elements") */
    const field *f;
    size_t n;
    size_t size;
    void (*defaults)(void *); /* container Default (D_CONT fields), or NULL: zeroed */
} sdesc;

/* ---- messages ---- */

/* serde_json's Unexpected for a value. */
static void unexpected(yyjson_val *v, char *out, size_t cap)
{
    char num[PC_JSON_NUM_MAX];
    out[0] = '\0';
    if (v == NULL || yyjson_is_null(v)) {
        (void)snprintf(out, cap, "null");
    } else if (yyjson_is_bool(v)) {
        (void)snprintf(out, cap, "boolean `%s`", yyjson_get_bool(v) ? "true" : "false");
    } else if (yyjson_is_sint(v) && yyjson_get_sint(v) == 0) {
        (void)snprintf(out, cap, "floating point `-0.0`");
    } else if (yyjson_is_uint(v)) {
        (void)snprintf(out, cap, "integer `%llu`", (unsigned long long)yyjson_get_uint(v));
    } else if (yyjson_is_sint(v)) {
        (void)snprintf(out, cap, "integer `%lld`", (long long)yyjson_get_sint(v));
    } else if (yyjson_is_real(v)) {
        (void)pc_json_format_f64(yyjson_get_real(v), num);
        (void)snprintf(out, cap, "floating point `%s`", num);
    } else if (yyjson_is_str(v)) {
        char q[PC_ERROR_MSG_MAX - 8];
        pc_utf8_debug_quote((const uint8_t *)yyjson_get_str(v), yyjson_get_len(v), q, sizeof q);
        (void)snprintf(out, cap, "string %s", q);
    } else if (yyjson_is_arr(v)) {
        (void)snprintf(out, cap, "sequence");
    } else {
        (void)snprintf(out, cap, "map");
    }
}

static pc_status fail_type(yyjson_val *v, const char *exp, pc_error *err)
{
    char u[PC_ERROR_MSG_MAX];
    unexpected(v, u, sizeof u);
    return pc_fail(err, PC_ERR_INVALID, "invalid type: %s, expected %s", u, exp);
}

static pc_status fail_nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "out of memory reading JSON");
}

/* A yyjson "-0" is the integer 0 with a sign; serde_json reads it as the float -0.0. */
static bool neg_zero(yyjson_val *v)
{
    return yyjson_is_sint(v) && yyjson_get_sint(v) == 0;
}

/* ---- primitives ---- */

static pc_status read_f64v(yyjson_val *v, const char *exp, double *out, pc_error *err)
{
    if (neg_zero(v)) {
        *out = -0.0;
    } else if (yyjson_is_uint(v)) {
        *out = (double)yyjson_get_uint(v);
    } else if (yyjson_is_sint(v)) {
        *out = (double)yyjson_get_sint(v);
    } else if (yyjson_is_real(v)) {
        *out = yyjson_get_real(v);
    } else {
        return fail_type(v, exp, err);
    }
    return PC_OK;
}

static pc_status read_f32v(yyjson_val *v, float *out, pc_error *err)
{
    /* serde's f32 visitor: `v as f32` from u64, i64 or f64 (each a single rounding). */
    if (neg_zero(v)) {
        *out = -0.0f;
    } else if (yyjson_is_uint(v)) {
        *out = (float)yyjson_get_uint(v);
    } else if (yyjson_is_sint(v)) {
        *out = (float)yyjson_get_sint(v);
    } else if (yyjson_is_real(v)) {
        *out = pc_f64_to_f32(yyjson_get_real(v));
    } else {
        return fail_type(v, "f32", err);
    }
    return PC_OK;
}

static pc_status read_uint(yyjson_val *v, uint64_t max, const char *name, uint64_t *out, pc_error *err)
{
    if (yyjson_is_uint(v) || (yyjson_is_sint(v) && yyjson_get_sint(v) > 0)) {
        uint64_t u = yyjson_is_uint(v) ? yyjson_get_uint(v) : (uint64_t)yyjson_get_sint(v);
        if (u > max) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%llu`, expected %s",
                           (unsigned long long)u, name);
        }
        *out = u;
        return PC_OK;
    }
    if (yyjson_is_sint(v) && !neg_zero(v)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%lld`, expected %s",
                       (long long)yyjson_get_sint(v), name);
    }
    return fail_type(v, name, err);
}

static yyjson_mut_val *w_f32(yyjson_mut_doc *d, float v, unsigned flags)
{
    if (!isfinite(v)) {
        return yyjson_mut_null(d);
    }
    return (flags & PC_PAINT_JSON_VALUE) != 0 ? yyjson_mut_real(d, (double)v) : yyjson_mut_float(d, v);
}

static yyjson_mut_val *w_f64(yyjson_mut_doc *d, double v)
{
    return isfinite(v) ? yyjson_mut_real(d, v) : yyjson_mut_null(d);
}

/* ---- unit enums ---- */

static const char *const CONTROL_NAMES[] = {"off",         "fade",     "penPressure",      "penTilt",
                                            "stylusWheel", "rotation", "initialDirection", "direction"};
static const char *const MASK_NAMES[] = {"multiply",  "subtract",   "darken",  "overlay",      "colorDodge",
                                         "colorBurn", "linearBurn", "hardMix", "linearHeight", "height"};
static const char *const STYLE_NAMES[] = {"noise", "canvas", "paper", "dots"};
static const char *const TIP_NAMES[] = {"round", "sampled", "stored"};
static const char *const PATTERN_NAMES[] = {"procedural", "tile", "stored"};

static const char *control_name(unsigned i)
{
    return CONTROL_NAMES[i];
}
static const char *mask_name(unsigned i)
{
    return MASK_NAMES[i];
}
static const char *style_name(unsigned i)
{
    return STYLE_NAMES[i];
}
static const char *tip_name(unsigned i)
{
    return TIP_NAMES[i];
}
static const char *pattern_name(unsigned i)
{
    return PATTERN_NAMES[i];
}
static const char *blend_name(unsigned i)
{
    return pc_blend_name((pc_blend_mode)i);
}

static const uenum E_CONTROL = {control_name, 8};
static const uenum E_MASK = {mask_name, 10};
static const uenum E_STYLE = {style_name, 4};
static const uenum E_TIP = {tip_name, 3};
static const uenum E_PATTERN = {pattern_name, 3};
static const uenum E_BLEND = {blend_name, PC_BLEND_COUNT};
/* replace.h's names (chunk L3: Color Replacement and Background Eraser options). */
static const char *replace_mode_name(unsigned i)
{
    return pc_paint_replace_mode_name((pc_paint_replace_mode)i);
}
static const char *sampling_name(unsigned i)
{
    return pc_paint_sampling_name((pc_paint_sampling)i);
}
static const char *limits_name(unsigned i)
{
    return pc_paint_limits_name((pc_paint_limits)i);
}
static const uenum E_REPLACE_MODE = {replace_mode_name, 4};
static const uenum E_SAMPLING = {sampling_name, 3};
static const uenum E_LIMITS = {limits_name, 3};

/* serde's "unknown variant" message with its OneOf list. */
static pc_status fail_variant(const uenum *e, const char *name, size_t len, pc_error *err)
{
    char list[PC_ERROR_MSG_MAX * 2];
    size_t at = 0;
    list[0] = '\0';
    if (e->n == 1) {
        (void)snprintf(list, sizeof list, "`%s`", e->name(0));
    } else if (e->n == 2) {
        (void)snprintf(list, sizeof list, "`%s` or `%s`", e->name(0), e->name(1));
    } else {
        at = (size_t)snprintf(list, sizeof list, "one of ");
        for (unsigned i = 0; i < e->n && at < sizeof list; i++) {
            int k = snprintf(list + at, sizeof list - at, "%s`%s`", i > 0 ? ", " : "", e->name(i));
            if (k < 0) {
                break;
            }
            at += (size_t)k;
        }
    }
    return pc_fail(err, PC_ERR_INVALID, "unknown variant `%.*s`, expected %s", (int)(len > 200 ? 200 : len),
                   name, list);
}

/* Number of distinct keys of an object (a Value collapses duplicates). */
static size_t distinct_keys(yyjson_val *obj, yyjson_val **first_key, yyjson_val **first_val)
{
    size_t n = 0;
    yyjson_obj_iter it;
    yyjson_val *k;
    (void)yyjson_obj_iter_init(obj, &it);
    while ((k = yyjson_obj_iter_next(&it)) != NULL) {
        yyjson_obj_iter it2;
        yyjson_val *k2;
        bool dup = false;
        (void)yyjson_obj_iter_init(obj, &it2);
        while ((k2 = yyjson_obj_iter_next(&it2)) != k) {
            if (yyjson_get_len(k2) == yyjson_get_len(k) &&
                memcmp(yyjson_get_str(k2), yyjson_get_str(k), yyjson_get_len(k)) == 0) {
                dup = true;
                break;
            }
        }
        if (!dup && ++n == 1) {
            *first_key = k;
        }
        if (n > 1) {
            break;
        }
    }
    if (n == 1) {
        *first_val = pc_json_get(obj, yyjson_get_str(*first_key));
    }
    return n;
}

/* Resolves an externally tagged enum value: *idx = the variant, *payload = the map's value
 * (NULL for a plain string). */
static pc_status enum_tag(const uenum *e, yyjson_val *v, unsigned *idx, yyjson_val **payload, pc_error *err)
{
    const char *name;
    size_t len;
    if (yyjson_is_str(v)) {
        name = yyjson_get_str(v);
        len = yyjson_get_len(v);
        *payload = NULL;
    } else if (yyjson_is_obj(v)) {
        yyjson_val *k = NULL, *val = NULL;
        if (distinct_keys(v, &k, &val) != 1) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: map, expected map with a single key");
        }
        name = yyjson_get_str(k);
        len = yyjson_get_len(k);
        *payload = val;
    } else {
        return fail_type(v, "string or map", err);
    }
    for (unsigned i = 0; i < e->n; i++) {
        const char *s = e->name(i);
        if (strlen(s) == len && memcmp(s, name, len) == 0) {
            *idx = i;
            return PC_OK;
        }
    }
    return fail_variant(e, name, len, err);
}

static pc_status read_unit_enum(const uenum *e, yyjson_val *v, int *out, pc_error *err)
{
    unsigned idx = 0;
    yyjson_val *payload = NULL;
    pc_status st = enum_tag(e, v, &idx, &payload, err);
    if (st != PC_OK) {
        return st;
    }
    if (payload != NULL && !yyjson_is_null(payload)) {
        return fail_type(payload, "unit", err);
    }
    *out = (int)idx;
    return PC_OK;
}

/* ---- gray tiles ---- */

static pc_status tile_alloc_fail(pc_status st, pc_error *err)
{
    return st == PC_ERR_NOMEM ? fail_nomem(err) : pc_fail(err, st, "tile data too large");
}

/* DataIn, untagged: a string (base64, decoded after the struct is read) or an array of floats.
 * *str or *floats is set; the float samples are converted right away (q16). */
static pc_status read_tile_data(yyjson_val *v, yyjson_val **str, uint16_t **floats, size_t *count,
                                pc_error *err)
{
    size_t len;
    uint16_t *samples;
    if (yyjson_is_str(v)) {
        *str = v;
        return PC_OK;
    }
    if (!yyjson_is_arr(v)) {
        return pc_fail(err, PC_ERR_INVALID, "data did not match any variant of untagged enum DataIn");
    }
    len = yyjson_arr_size(v);
    samples = pc_malloc_array(len > 0 ? len : 1, sizeof *samples);
    if (samples == NULL) {
        return fail_nomem(err);
    }
    for (size_t k = 0; k < len; k++) {
        float f;
        if (read_f32v(yyjson_arr_get(v, k), &f, NULL) != PC_OK) {
            free(samples);
            return pc_fail(err, PC_ERR_INVALID, "data did not match any variant of untagged enum DataIn");
        }
        samples[k] = pc_paint_q16(f);
    }
    *floats = samples;
    *count = len;
    return PC_OK;
}

/* GrayTileIn { width: u32, height: u32, data: DataIn }, then the size check. */
static pc_status read_tile(yyjson_val *v, pc_paint_gray_tile *out, pc_error *err)
{
    static const char *const names[3] = {"width", "height", "data"};
    yyjson_val *fv[3] = {NULL, NULL, NULL}, *str = NULL;
    uint64_t wh[2] = {0, 0};
    uint16_t *samples = NULL;
    size_t count = 0, len = 0;
    pc_status st = PC_OK;
    bool obj = yyjson_is_obj(v);
    if (obj) {
        for (unsigned i = 0; i < 3; i++) {
            fv[i] = pc_json_get(v, names[i]);
        }
    } else if (yyjson_is_arr(v)) {
        len = yyjson_arr_size(v);
    } else {
        return fail_type(v, "struct GrayTileIn", err);
    }
    /* Visit order: an object's keys sorted (data, height, width), an array's elements in order. */
    for (unsigned j = 0; j < 3 && st == PC_OK; j++) {
        unsigned i = obj ? 2 - j : j;
        if (!obj) {
            if (i >= len) {
                st = pc_fail(err, PC_ERR_INVALID,
                             "invalid length %u, expected struct GrayTileIn with 3 elements", i);
                break;
            }
            fv[i] = yyjson_arr_get(v, i);
        }
        if (fv[i] == NULL) {
            continue;
        }
        if (i < 2) {
            st = read_uint(fv[i], UINT32_MAX, "u32", &wh[i], err);
        } else {
            st = read_tile_data(fv[i], &str, &samples, &count, err);
        }
    }
    if (st == PC_OK && !obj && len > 3) {
        st = pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected fewer elements in array", len);
    }
    for (unsigned i = 0; i < 3 && st == PC_OK; i++) {
        if (fv[i] == NULL) {
            st = pc_fail(err, PC_ERR_INVALID, "missing field `%s`", names[i]);
        }
    }
    if (st == PC_OK && str != NULL) {
        pc_buf bytes = PC_BUF_INIT;
        st = pc_paint_b64_decode(yyjson_get_str(str), yyjson_get_len(str), &bytes, NULL);
        if (st == PC_ERR_FORMAT) {
            st = pc_fail(err, PC_ERR_INVALID, "invalid base64 in tile data");
        } else if (st != PC_OK) {
            st = fail_nomem(err);
        } else {
            count = bytes.len / 2;
            samples = pc_malloc_array(count > 0 ? count : 1, sizeof *samples);
            if (samples == NULL) {
                st = fail_nomem(err);
            }
            for (size_t k = 0; samples != NULL && k < count; k++) {
                samples[k] = (uint16_t)(bytes.data[2 * k] | bytes.data[2 * k + 1] << 8);
            }
        }
        pc_buf_fini(&bytes);
    }
    if (st == PC_OK && (wh[0] * wh[1] == 0 || count != wh[0] * wh[1])) {
        st = pc_fail(err, PC_ERR_INVALID, "tile data has %zu samples, expected %llu\xc3\x97%llu", count,
                     (unsigned long long)wh[0], (unsigned long long)wh[1]);
    }
    if (st == PC_OK) {
        st = pc_paint_gray_tile_from_u16((uint32_t)wh[0], (uint32_t)wh[1], samples, count, out, err);
        if (st != PC_OK) {
            st = tile_alloc_fail(st, err);
        }
    }
    free(samples);
    return st;
}

static yyjson_mut_val *write_tile(yyjson_mut_doc *d, const pc_paint_gray_tile *t)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    size_t n = pc_paint_gray_tile_len(t);
    const uint16_t *s = pc_paint_gray_tile_samples(t);
    pc_buf bytes = PC_BUF_INIT, enc = PC_BUF_INIT;
    yyjson_mut_val *data = NULL;
    bool ok = o != NULL;
    for (size_t i = 0; ok && i < n; i++) {
        uint8_t le[2] = {(uint8_t)(s[i] & 0xFF), (uint8_t)(s[i] >> 8)};
        ok = pc_buf_append(&bytes, le, 2, NULL) == PC_OK;
    }
    ok = ok && pc_paint_b64_encode(bytes.data, bytes.len, &enc, NULL) == PC_OK;
    if (ok) {
        data = yyjson_mut_strncpy(d, pc_buf_cstr(&enc), enc.len);
    }
    ok = ok && data != NULL && yyjson_mut_obj_add_uint(d, o, "width", t->width) &&
         yyjson_mut_obj_add_uint(d, o, "height", t->height) && yyjson_mut_obj_add_val(d, o, "data", data);
    pc_buf_fini(&bytes);
    pc_buf_fini(&enc);
    return ok ? o : NULL;
}

/* ---- tagged enums: tip and pattern ---- */

static pc_status read_struct(const sdesc *s, yyjson_val *v, void *out, pc_error *err);
static const sdesc POINT_S;
static yyjson_mut_val *write_struct(const sdesc *s, yyjson_mut_doc *d, const void *base, unsigned flags);
static void fini_struct(const sdesc *s, void *base);

/* StoredTile { key, width, height, preview } (`full` is never serialised). */
typedef struct stored_c {
    pc_str key;
    uint32_t width;
    uint32_t height;
    pc_paint_gray_tile preview;
} stored_c;

static const field STORED_F[] = {
    {"key", NULL, K_STR, D_REQ, 0, offsetof(stored_c, key), NULL},
    {"width", NULL, K_U32, D_REQ, 0, offsetof(stored_c, width), NULL},
    {"height", NULL, K_U32, D_REQ, 0, offsetof(stored_c, height), NULL},
    {"preview", NULL, K_TILE, D_REQ, 0, offsetof(stored_c, preview), NULL},
};
static const sdesc STORED_S = {"struct StoredTile", "StoredTile", STORED_F, 4, sizeof(stored_c), NULL};

/* The payload of a `stored` variant into *out (unloaded). */
static pc_status read_stored(yyjson_val *payload, pc_paint_stored_tile *out, pc_error *err)
{
    stored_c c;
    pc_status st;
    if (payload == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "invalid type: unit variant, expected newtype variant");
    }
    st = read_struct(&STORED_S, payload, &c, err);
    if (st != PC_OK) {
        return st;
    }
    st =
        pc_paint_stored_tile_init(pc_str_c(&c.key), c.key.len, c.width, c.height, &c.preview, NULL, out, err);
    if (st != PC_OK) {
        st = fail_nomem(err);
    }
    fini_struct(&STORED_S, &c);
    return st;
}

/* {"tag": tile} for a loaded stored tile (its full bitmap), else {"stored": {...}}. */
static yyjson_mut_val *write_stored(yyjson_mut_doc *d, const pc_paint_stored_tile *t, const char *tag,
                                    unsigned flags)
{
    yyjson_mut_val *o = yyjson_mut_obj(d), *inner;
    if (o == NULL) {
        return NULL;
    }
    if (t->loaded) {
        inner = write_tile(d, &t->full);
        return inner != NULL && yyjson_mut_obj_add_val(d, o, tag, inner) ? o : NULL;
    } else {
        size_t len;
        const char *key = pc_paint_stored_tile_key(t, &len);
        stored_c c = {PC_STR_INIT, t->width, t->height, t->preview};
        c.key.ptr = (char *)(uintptr_t)key; /* borrowed for writing only */
        c.key.len = len;
        inner = write_struct(&STORED_S, d, &c, flags);
        return inner != NULL && yyjson_mut_obj_add_val(d, o, "stored", inner) ? o : NULL;
    }
}

static pc_status read_tip(yyjson_val *v, pc_paint_tip *out, pc_error *err)
{
    unsigned idx = 0;
    yyjson_val *payload = NULL;
    pc_paint_gray_tile t;
    pc_status st = enum_tag(&E_TIP, v, &idx, &payload, err);
    if (st != PC_OK) {
        return st;
    }
    if (idx == PC_PAINT_TIP_ROUND) {
        if (payload != NULL && !yyjson_is_null(payload)) {
            return fail_type(payload, "unit", err);
        }
        pc_paint_tip_default(out);
        return PC_OK;
    }
    if (idx == PC_PAINT_TIP_STORED) {
        pc_paint_stored_tile r;
        st = read_stored(payload, &r, err);
        if (st == PC_OK) {
            pc_paint_tip_default(out);
            out->kind = PC_PAINT_TIP_STORED;
            out->stored = r;
        }
        return st;
    }
    if (payload == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "invalid type: unit variant, expected newtype variant");
    }
    st = read_tile(payload, &t, err);
    if (st != PC_OK) {
        return st;
    }
    pc_paint_tip_default(out);
    out->kind = PC_PAINT_TIP_SAMPLED;
    out->tile = t;
    return PC_OK;
}

static yyjson_mut_val *write_tip(yyjson_mut_doc *d, const pc_paint_tip *t, unsigned flags)
{
    yyjson_mut_val *o, *tile;
    if (t->kind == PC_PAINT_TIP_ROUND) {
        return yyjson_mut_str(d, "round");
    }
    if (t->kind == PC_PAINT_TIP_STORED) {
        return write_stored(d, &t->stored, "sampled", flags);
    }
    o = yyjson_mut_obj(d);
    tile = write_tile(d, &t->tile);
    return o != NULL && tile != NULL && yyjson_mut_obj_add_val(d, o, "sampled", tile) ? o : NULL;
}

typedef struct procedural_c {
    pc_paint_pattern_style style;
    uint32_t size;
    uint32_t seed;
} procedural_c;

static const field PROCEDURAL_F[] = {
    {"style", NULL, K_ENUM, D_REQ, 0, offsetof(procedural_c, style), &E_STYLE},
    {"size", NULL, K_U32, D_REQ, 0, offsetof(procedural_c, size), NULL},
    {"seed", NULL, K_U32, D_REQ, 0, offsetof(procedural_c, seed), NULL},
};
static const sdesc PROCEDURAL_S = {
    "struct variant Pattern::Procedural", "Pattern::Procedural", PROCEDURAL_F, 3, sizeof(procedural_c), NULL};

static pc_status read_pattern(yyjson_val *v, pc_paint_pattern *out, pc_error *err)
{
    unsigned idx = 0;
    yyjson_val *payload = NULL;
    pc_status st = enum_tag(&E_PATTERN, v, &idx, &payload, err);
    if (st != PC_OK) {
        return st;
    }
    if (idx == PC_PAINT_PATTERN_PROCEDURAL) {
        procedural_c p;
        if (payload == NULL) {
            return pc_fail(err, PC_ERR_INVALID, "invalid type: unit variant, expected struct variant");
        }
        if (!yyjson_is_obj(payload)) {
            return fail_type(payload, "struct variant", err);
        }
        st = read_struct(&PROCEDURAL_S, payload, &p, err);
        if (st != PC_OK) {
            return st;
        }
        memset(out, 0, sizeof *out);
        out->kind = PC_PAINT_PATTERN_PROCEDURAL;
        out->style = p.style;
        out->size = p.size;
        out->seed = p.seed;
        return PC_OK;
    }
    if (idx == PC_PAINT_PATTERN_STORED) {
        pc_paint_stored_tile r;
        st = read_stored(payload, &r, err);
        if (st == PC_OK) {
            memset(out, 0, sizeof *out);
            out->kind = PC_PAINT_PATTERN_STORED;
            out->stored = r;
        }
        return st;
    }
    if (payload == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "invalid type: unit variant, expected newtype variant");
    }
    memset(out, 0, sizeof *out);
    st = read_tile(payload, &out->tile, err);
    if (st != PC_OK) {
        return st;
    }
    out->kind = PC_PAINT_PATTERN_TILE;
    return PC_OK;
}

static yyjson_mut_val *write_pattern(yyjson_mut_doc *d, const pc_paint_pattern *p, unsigned flags)
{
    yyjson_mut_val *o, *inner;
    if (p->kind == PC_PAINT_PATTERN_STORED) {
        return write_stored(d, &p->stored, "tile", flags);
    }
    o = yyjson_mut_obj(d);
    if (o == NULL) {
        return NULL;
    }
    if (p->kind == PC_PAINT_PATTERN_TILE) {
        inner = write_tile(d, &p->tile);
        return inner != NULL && yyjson_mut_obj_add_val(d, o, "tile", inner) ? o : NULL;
    } else {
        procedural_c c = {p->style, p->size, p->seed};
        inner = write_struct(&PROCEDURAL_S, d, &c, flags);
        return inner != NULL && yyjson_mut_obj_add_val(d, o, "procedural", inner) ? o : NULL;
    }
}

/* ---- fields ---- */

static void fini_field(const field *f, void *slot)
{
    switch (f->kind) {
    case K_STR:
        pc_str_fini(slot);
        break;
    case K_TIP:
        pc_paint_tip_fini(slot);
        break;
    case K_PATTERN:
        pc_paint_pattern_fini(slot);
        break;
    case K_TILE:
        pc_paint_gray_tile_fini(slot);
        break;
    case K_STRUCT:
        fini_struct(f->aux, slot);
        break;
    case K_STRS:
        pc_paint_str_list_fini(slot);
        break;
    case K_POINTS: {
        pc_paint_point **pp = slot;
        free(*pp);
        *pp = NULL;
        break;
    }
    default:
        break;
    }
}

/* Point arrays are stored as {pc_paint_point *points; size_t count} (pc_paint_stroke). */
static pc_status read_points(yyjson_val *v, void *slot, pc_error *err)
{
    size_t n;
    pc_paint_point *pts = NULL;
    if (!yyjson_is_arr(v)) {
        return fail_type(v, "a sequence", err);
    }
    n = yyjson_arr_size(v);
    if (n > 0) {
        pts = pc_malloc_array(n, sizeof *pts);
        if (pts == NULL) {
            return fail_nomem(err);
        }
    }
    for (size_t i = 0; i < n; i++) {
        pc_status st = read_struct(&POINT_S, yyjson_arr_get(v, i), &pts[i], err);
        if (st != PC_OK) {
            free(pts);
            return st;
        }
    }
    {
        pc_paint_point **pp = slot;
        size_t *cnt =
            (size_t *)((char *)slot + offsetof(pc_paint_stroke, count) - offsetof(pc_paint_stroke, points));
        free(*pp);
        *pp = pts;
        *cnt = n;
    }
    return PC_OK;
}

/* Vec<String>: a sequence of strings. */
static pc_status read_strs(yyjson_val *v, pc_paint_str_list *slot, pc_error *err)
{
    pc_paint_str_list l = {NULL, 0};
    size_t n;
    if (!yyjson_is_arr(v)) {
        return fail_type(v, "a sequence", err);
    }
    n = yyjson_arr_size(v);
    if (n > 0) {
        l.v = pc_calloc_array(n, sizeof *l.v);
        if (l.v == NULL) {
            return fail_nomem(err);
        }
    }
    for (; l.len < n; l.len++) {
        yyjson_val *e = yyjson_arr_get(v, l.len);
        pc_status st;
        if (!yyjson_is_str(e)) {
            st = fail_type(e, "a string", err);
        } else {
            st = pc_str_set(&l.v[l.len], yyjson_get_str(e), yyjson_get_len(e), NULL) == PC_OK
                     ? PC_OK
                     : fail_nomem(err);
        }
        if (st != PC_OK) {
            pc_paint_str_list_fini(&l);
            return st;
        }
    }
    pc_paint_str_list_fini(slot);
    *slot = l;
    return PC_OK;
}

/* Reads one field value into its slot: on success the slot's old content is released and
 * replaced; on failure the slot is unchanged. */
static pc_status read_field(const field *f, yyjson_val *v, void *slot, pc_error *err)
{
    pc_status st = PC_OK;
    switch (f->kind) {
    case K_F32: {
        float x = 0.0f;
        st = read_f32v(v, &x, err);
        if (st == PC_OK) {
            memcpy(slot, &x, sizeof x);
        }
        return st;
    }
    case K_F64: {
        double x = 0.0;
        st = read_f64v(v, "f64", &x, err);
        if (st == PC_OK) {
            memcpy(slot, &x, sizeof x);
        }
        return st;
    }
    case K_U32: {
        uint64_t u = 0;
        st = read_uint(v, UINT32_MAX, "u32", &u, err);
        if (st == PC_OK) {
            uint32_t x = (uint32_t)u;
            memcpy(slot, &x, sizeof x);
        }
        return st;
    }
    case K_U64: {
        uint64_t u = 0;
        st = read_uint(v, UINT64_MAX, "u64", &u, err);
        if (st == PC_OK) {
            memcpy(slot, &u, sizeof u);
        }
        return st;
    }
    case K_BOOL: {
        bool b;
        if (!yyjson_is_bool(v)) {
            return fail_type(v, "a boolean", err);
        }
        b = yyjson_get_bool(v);
        memcpy(slot, &b, sizeof b);
        return PC_OK;
    }
    case K_STR:
        if (!yyjson_is_str(v)) {
            return fail_type(v, "a string", err);
        }
        return pc_str_set(slot, yyjson_get_str(v), yyjson_get_len(v), err) == PC_OK ? PC_OK : fail_nomem(err);
    case K_RGBA: {
        float c[4];
        size_t len;
        if (!yyjson_is_arr(v)) {
            return fail_type(v, "an array of length 4", err);
        }
        len = yyjson_arr_size(v);
        for (size_t i = 0; i < 4; i++) {
            if (i >= len) {
                return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected an array of length 4", i);
            }
            st = read_f32v(yyjson_arr_get(v, i), &c[i], err);
            if (st != PC_OK) {
                return st;
            }
        }
        if (len > 4) {
            return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected fewer elements in array", len);
        }
        memcpy(slot, c, sizeof c);
        return PC_OK;
    }
    case K_ENUM: {
        int x = 0;
        st = read_unit_enum(f->aux, v, &x, err);
        if (st == PC_OK) {
            memcpy(slot, &x, sizeof x);
        }
        return st;
    }
    case K_TIP: {
        pc_paint_tip t;
        st = read_tip(v, &t, err);
        if (st == PC_OK) {
            pc_paint_tip_fini(slot);
            memcpy(slot, &t, sizeof t);
        }
        return st;
    }
    case K_PATTERN: {
        pc_paint_pattern p;
        st = read_pattern(v, &p, err);
        if (st == PC_OK) {
            pc_paint_pattern_fini(slot);
            memcpy(slot, &p, sizeof p);
        }
        return st;
    }
    case K_TILE: {
        pc_paint_gray_tile t;
        st = read_tile(v, &t, err);
        if (st == PC_OK) {
            pc_paint_gray_tile_fini(slot);
            memcpy(slot, &t, sizeof t);
        }
        return st;
    }
    case K_STRUCT: {
        const sdesc *s = f->aux;
        void *tmp = malloc(s->size);
        if (tmp == NULL) {
            return fail_nomem(err);
        }
        st = read_struct(s, v, tmp, err);
        if (st == PC_OK) {
            fini_struct(s, slot);
            memcpy(slot, tmp, s->size);
        }
        free(tmp);
        return st;
    }
    case K_POINTS:
        return read_points(v, slot, err);
    case K_STRS:
        return read_strs(v, slot, err);
    default:
        return pc_fail(err, PC_ERR_INVALID, "unknown field kind");
    }
}

static yyjson_mut_val *write_field(const field *f, yyjson_mut_doc *d, const void *slot, unsigned flags)
{
    switch (f->kind) {
    case K_F32: {
        float x;
        memcpy(&x, slot, sizeof x);
        return w_f32(d, x, flags);
    }
    case K_F64: {
        double x;
        memcpy(&x, slot, sizeof x);
        return w_f64(d, x);
    }
    case K_U32: {
        uint32_t x;
        memcpy(&x, slot, sizeof x);
        return yyjson_mut_uint(d, x);
    }
    case K_U64: {
        uint64_t x;
        memcpy(&x, slot, sizeof x);
        return yyjson_mut_uint(d, x);
    }
    case K_BOOL: {
        bool b;
        memcpy(&b, slot, sizeof b);
        return yyjson_mut_bool(d, b);
    }
    case K_STR: {
        const pc_str *s = slot;
        return yyjson_mut_strncpy(d, pc_str_c(s), s->len);
    }
    case K_RGBA: {
        const float *c = slot;
        yyjson_mut_val *a = yyjson_mut_arr(d);
        for (int i = 0; a != NULL && i < 4; i++) {
            yyjson_mut_val *x = w_f32(d, c[i], flags);
            if (x == NULL || !yyjson_mut_arr_append(a, x)) {
                return NULL;
            }
        }
        return a;
    }
    case K_ENUM: {
        int x;
        const uenum *e = f->aux;
        memcpy(&x, slot, sizeof x);
        return x >= 0 && (unsigned)x < e->n ? yyjson_mut_str(d, e->name((unsigned)x)) : NULL;
    }
    case K_TIP:
        return write_tip(d, slot, flags);
    case K_PATTERN:
        return write_pattern(d, slot, flags);
    case K_TILE:
        return write_tile(d, slot);
    case K_STRUCT:
        return write_struct(f->aux, d, slot, flags);
    case K_POINTS: {
        const pc_paint_point *const *pp = slot;
        const size_t *cnt = (const size_t *)((const char *)slot + offsetof(pc_paint_stroke, count) -
                                             offsetof(pc_paint_stroke, points));
        yyjson_mut_val *a = yyjson_mut_arr(d);
        for (size_t i = 0; a != NULL && i < *cnt; i++) {
            yyjson_mut_val *x = write_struct(&POINT_S, d, &(*pp)[i], flags);
            if (x == NULL || !yyjson_mut_arr_append(a, x)) {
                return NULL;
            }
        }
        return a;
    }
    case K_STRS: {
        const pc_paint_str_list *l = slot;
        yyjson_mut_val *a = yyjson_mut_arr(d);
        for (size_t i = 0; a != NULL && i < l->len; i++) {
            yyjson_mut_val *x = yyjson_mut_strncpy(d, pc_str_c(&l->v[i]), l->v[i].len);
            if (x == NULL || !yyjson_mut_arr_append(a, x)) {
                return NULL;
            }
        }
        return a;
    }
    default:
        return NULL;
    }
}

/* skip_serializing_if: an empty string or list, or a false bool. */
static bool skipped(const field *f, const void *slot)
{
    if (!f->skip_empty) {
        return false;
    }
    switch (f->kind) {
    case K_BOOL:
        return !*(const bool *)slot;
    case K_STRS:
        return ((const pc_paint_str_list *)slot)->len == 0;
    default:
        return ((const pc_str *)slot)->len == 0;
    }
}

/* ---- structs ---- */

static void struct_init(const sdesc *s, void *base)
{
    memset(base, 0, s->size);
    if (s->defaults != NULL) {
        s->defaults(base);
    }
}

static void fini_struct(const sdesc *s, void *base)
{
    for (size_t i = 0; i < s->n; i++) {
        fini_field(&s->f[i], (char *)base + s->f[i].off);
    }
}

#define MAX_KEYS 64

typedef struct keyref {
    const char *key;
    size_t field;
} keyref;

static int cmp_keyref(const void *a, const void *b)
{
    return strcmp(((const keyref *)a)->key, ((const keyref *)b)->key);
}

static pc_status read_struct(const sdesc *s, yyjson_val *v, void *out, pc_error *err)
{
    bool seen[MAX_KEYS] = {false};
    pc_status st = PC_OK;
    struct_init(s, out);
    if (yyjson_is_obj(v)) {
        keyref keys[MAX_KEYS];
        size_t nk = 0;
        for (size_t i = 0; i < s->n && nk + 2 <= MAX_KEYS; i++) {
            keys[nk++] = (keyref){s->f[i].name, i};
            if (s->f[i].alias != NULL) {
                keys[nk++] = (keyref){s->f[i].alias, i};
            }
        }
        qsort(keys, nk, sizeof keys[0], cmp_keyref);
        for (size_t k = 0; k < nk && st == PC_OK; k++) {
            const field *f = &s->f[keys[k].field];
            yyjson_val *fv = pc_json_get(v, keys[k].key);
            if (fv == NULL) {
                continue;
            }
            if (seen[keys[k].field]) {
                st = pc_fail(err, PC_ERR_INVALID, "duplicate field `%s`", f->name);
                break;
            }
            seen[keys[k].field] = true;
            st = read_field(f, fv, (char *)out + f->off, err);
        }
        for (size_t i = 0; i < s->n && st == PC_OK; i++) {
            if (!seen[i] && s->f[i].def == D_REQ) {
                st = pc_fail(err, PC_ERR_INVALID, "missing field `%s`", s->f[i].name);
            }
        }
    } else if (yyjson_is_arr(v)) {
        size_t len = yyjson_arr_size(v);
        for (size_t i = 0; i < s->n && st == PC_OK; i++) {
            if (i < len) {
                st = read_field(&s->f[i], yyjson_arr_get(v, i), (char *)out + s->f[i].off, err);
            } else if (s->f[i].def == D_REQ) {
                st = pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected struct %s with %zu element%s",
                             i, s->name, s->n, s->n == 1 ? "" : "s");
            }
        }
        if (st == PC_OK && len > s->n) {
            st = pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected fewer elements in array", len);
        }
    } else {
        st = fail_type(v, s->expecting, err);
    }
    if (st != PC_OK) {
        fini_struct(s, out);
    }
    return st;
}

static yyjson_mut_val *write_struct(const sdesc *s, yyjson_mut_doc *d, const void *base, unsigned flags)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    for (size_t i = 0; o != NULL && i < s->n; i++) {
        const field *f = &s->f[i];
        const void *slot = (const char *)base + f->off;
        yyjson_mut_val *x;
        if (skipped(f, slot)) {
            continue;
        }
        x = write_field(f, d, slot, flags);
        if (x == NULL || !yyjson_mut_obj_add_val(d, o, f->name, x)) {
            return NULL;
        }
    }
    return o;
}

/* ---- the type tables ---- */

#define F(nm, T, m, k, aux) {(nm), NULL, (k), D_CONT, 0, offsetof(T, m), (aux)}

static void def_point(void *p)
{
    *(pc_paint_point *)p = pc_paint_point_default();
}
static void def_dynamic(void *p)
{
    *(pc_paint_dynamic *)p = pc_paint_dynamic_default();
}
static void def_shape(void *p)
{
    pc_paint_shape_dynamics_default(p);
}
static void def_scatter(void *p)
{
    pc_paint_scattering_default(p);
}
static void def_texture(void *p)
{
    pc_paint_texture_default(p);
}
static void def_dual(void *p)
{
    pc_paint_dual_brush_default(p);
}
static void def_color(void *p)
{
    pc_paint_color_dynamics_default(p);
}
static void def_transfer(void *p)
{
    pc_paint_transfer_default(p);
}
static void def_locks(void *p)
{
    pc_paint_section_locks_default(p);
}
static void def_pose(void *p)
{
    pc_paint_pose_default(p);
}
static void def_smoothing(void *p)
{
    pc_paint_smoothing_default(p);
}
static void def_mixer(void *p)
{
    pc_paint_mixer_default(p);
}
static void def_replace(void *p)
{
    pc_paint_replace_settings_default(p);
}
static void def_bg_erase(void *p)
{
    pc_paint_bg_erase_settings_default(p);
}
static void def_brush(void *p)
{
    pc_paint_brush_default(p);
}

typedef pc_paint_point PT;
static const field POINT_F[] = {
    F("x", PT, x, K_F64, NULL),
    F("y", PT, y, K_F64, NULL),
    F("pressure", PT, pressure, K_F32, NULL),
    F("tiltX", PT, tilt_x, K_F32, NULL),
    F("tiltY", PT, tilt_y, K_F32, NULL),
    F("rotation", PT, rotation, K_F32, NULL),
    F("wheel", PT, wheel, K_F32, NULL),
    F("time", PT, time, K_F64, NULL),
};
static const sdesc POINT_S = {"struct StrokePoint", "StrokePoint", POINT_F, 8, sizeof(PT), def_point};

typedef pc_paint_dynamic DY;
static const field DYN_F[] = {
    F("jitter", DY, jitter, K_F32, NULL),
    F("control", DY, control, K_ENUM, &E_CONTROL),
    F("fadeSteps", DY, fade_steps, K_U32, NULL),
    F("minimum", DY, minimum, K_F32, NULL),
};
static const sdesc DYN_S = {"struct Dynamic", "Dynamic", DYN_F, 4, sizeof(DY), def_dynamic};

typedef pc_paint_shape_dynamics SD;
static const field SHAPE_F[] = {
    F("enabled", SD, enabled, K_BOOL, NULL),
    F("size", SD, size, K_STRUCT, &DYN_S),
    F("tiltScale", SD, tilt_scale, K_F32, NULL),
    F("angle", SD, angle, K_STRUCT, &DYN_S),
    F("roundness", SD, roundness, K_STRUCT, &DYN_S),
    F("flipXJitter", SD, flip_x_jitter, K_BOOL, NULL),
    F("flipYJitter", SD, flip_y_jitter, K_BOOL, NULL),
    F("brushProjection", SD, brush_projection, K_BOOL, NULL),
};
static const sdesc SHAPE_S = {"struct ShapeDynamics", "ShapeDynamics", SHAPE_F, 8, sizeof(SD), def_shape};

typedef pc_paint_scattering SC;
static const field SCATTER_F[] = {
    F("enabled", SC, enabled, K_BOOL, NULL),
    F("scatter", SC, scatter, K_STRUCT, &DYN_S),
    F("bothAxes", SC, both_axes, K_BOOL, NULL),
    F("count", SC, count, K_U32, NULL),
    F("countJitter", SC, count_jitter, K_STRUCT, &DYN_S),
};
static const sdesc SCATTER_S = {"struct Scattering", "Scattering", SCATTER_F, 5, sizeof(SC), def_scatter};

typedef pc_paint_texture TX;
static const field TEXTURE_F[] = {
    F("enabled", TX, enabled, K_BOOL, NULL),      F("pattern", TX, pattern, K_PATTERN, NULL),
    F("invert", TX, invert, K_BOOL, NULL),        F("scale", TX, scale, K_F32, NULL),
    F("brightness", TX, brightness, K_F32, NULL), F("contrast", TX, contrast, K_F32, NULL),
    F("eachTip", TX, each_tip, K_BOOL, NULL),     F("mode", TX, mode, K_ENUM, &E_MASK),
    F("depth", TX, depth, K_F32, NULL),           F("depthJitter", TX, depth_jitter, K_STRUCT, &DYN_S),
};
static const sdesc TEXTURE_S = {"struct Texture", "Texture", TEXTURE_F, 10, sizeof(TX), def_texture};

typedef pc_paint_dual_brush DB;
static const field DUAL_F[] = {
    F("enabled", DB, enabled, K_BOOL, NULL),  F("mode", DB, mode, K_ENUM, &E_MASK),
    F("tip", DB, tip, K_TIP, NULL),           F("size", DB, size, K_F32, NULL),
    F("hardness", DB, hardness, K_F32, NULL), F("roundness", DB, roundness, K_F32, NULL),
    F("angle", DB, angle, K_F32, NULL),       F("spacing", DB, spacing, K_F32, NULL),
    F("scatter", DB, scatter, K_F32, NULL),   F("bothAxes", DB, both_axes, K_BOOL, NULL),
    F("count", DB, count, K_U32, NULL),       F("flip", DB, flip, K_BOOL, NULL),
};
static const sdesc DUAL_S = {"struct DualBrush", "DualBrush", DUAL_F, 12, sizeof(DB), def_dual};

typedef pc_paint_color_dynamics CD;
static const field COLOR_F[] = {
    F("enabled", CD, enabled, K_BOOL, NULL),
    F("perTip", CD, per_tip, K_BOOL, NULL),
    F("fgBg", CD, fg_bg, K_STRUCT, &DYN_S),
    F("hueJitter", CD, hue_jitter, K_F32, NULL),
    F("saturationJitter", CD, saturation_jitter, K_F32, NULL),
    F("brightnessJitter", CD, brightness_jitter, K_F32, NULL),
    F("purity", CD, purity, K_F32, NULL),
};
static const sdesc COLOR_S = {"struct ColorDynamics", "ColorDynamics", COLOR_F, 7, sizeof(CD), def_color};

typedef pc_paint_transfer TR;
static const field TRANSFER_F[] = {
    F("enabled", TR, enabled, K_BOOL, NULL), F("opacity", TR, opacity, K_STRUCT, &DYN_S),
    F("flow", TR, flow, K_STRUCT, &DYN_S),   F("wetness", TR, wetness, K_STRUCT, &DYN_S),
    F("mix", TR, mix, K_STRUCT, &DYN_S),
};
static const sdesc TRANSFER_S = {"struct Transfer", "Transfer", TRANSFER_F, 5, sizeof(TR), def_transfer};

typedef pc_paint_section_locks SL;
static const field LOCKS_F[] = {
    F("shapeDynamics", SL, shape_dynamics, K_BOOL, NULL),
    F("scattering", SL, scattering, K_BOOL, NULL),
    F("texture", SL, texture, K_BOOL, NULL),
    F("dualBrush", SL, dual_brush, K_BOOL, NULL),
    F("colorDynamics", SL, color_dynamics, K_BOOL, NULL),
    F("transfer", SL, transfer, K_BOOL, NULL),
    F("pose", SL, pose, K_BOOL, NULL),
    F("noise", SL, noise, K_BOOL, NULL),
    F("wetEdges", SL, wet_edges, K_BOOL, NULL),
    F("buildUp", SL, build_up, K_BOOL, NULL),
    F("smoothing", SL, smoothing, K_BOOL, NULL),
    F("protectTexture", SL, protect_texture, K_BOOL, NULL),
};
static const sdesc LOCKS_S = {"struct SectionLocks", "SectionLocks", LOCKS_F, 12, sizeof(SL), def_locks};

typedef pc_paint_pose PO;
static const field POSE_F[] = {
    F("enabled", PO, enabled, K_BOOL, NULL),
    F("tiltX", PO, tilt_x, K_F32, NULL),
    F("tiltY", PO, tilt_y, K_F32, NULL),
    F("rotation", PO, rotation, K_F32, NULL),
    F("pressure", PO, pressure, K_F32, NULL),
    F("overrideTilt", PO, override_tilt, K_BOOL, NULL),
    F("overrideRotation", PO, override_rotation, K_BOOL, NULL),
    F("overridePressure", PO, override_pressure, K_BOOL, NULL),
};
static const sdesc POSE_S = {"struct Pose", "Pose", POSE_F, 8, sizeof(PO), def_pose};

typedef pc_paint_smoothing SM;
static const field SMOOTH_F[] = {
    F("amount", SM, amount, K_F32, NULL),
    F("pulledString", SM, pulled_string, K_BOOL, NULL),
    F("catchUp", SM, catch_up, K_BOOL, NULL),
    F("catchUpOnEnd", SM, catch_up_on_end, K_BOOL, NULL),
    F("adjustForZoom", SM, adjust_for_zoom, K_BOOL, NULL),
};
static const sdesc SMOOTH_S = {"struct Smoothing", "Smoothing", SMOOTH_F, 5, sizeof(SM), def_smoothing};

typedef pc_paint_mixer_settings MX;
static const field MIXER_F[] = {
    F("wet", MX, wet, K_F32, NULL),
    F("load", MX, load, K_F32, NULL),
    F("mix", MX, mix, K_F32, NULL),
    F("flow", MX, flow, K_F32, NULL),
    F("sampleAllLayers", MX, sample_all_layers, K_BOOL, NULL),
};
static const sdesc MIXER_S = {"struct MixerSettings", "MixerSettings", MIXER_F, 5, sizeof(MX), def_mixer};

typedef pc_paint_replace_settings RS;
static const field REPLACE_F[] = {
    F("mode", RS, mode, K_ENUM, &E_REPLACE_MODE),
    F("sampling", RS, sampling, K_ENUM, &E_SAMPLING),
    F("limits", RS, limits, K_ENUM, &E_LIMITS),
    F("tolerance", RS, tolerance, K_F32, NULL),
    F("antiAlias", RS, anti_alias, K_BOOL, NULL),
    F("color", RS, color, K_RGBA, NULL),
    F("background", RS, background, K_RGBA, NULL),
};
static const sdesc REPLACE_S = {"struct ReplaceSettings", "ReplaceSettings", REPLACE_F, 7, sizeof(RS),
                                def_replace};

typedef pc_paint_bg_erase_settings BG;
static const field BG_ERASE_F[] = {
    F("sampling", BG, sampling, K_ENUM, &E_SAMPLING),
    F("limits", BG, limits, K_ENUM, &E_LIMITS),
    F("tolerance", BG, tolerance, K_F32, NULL),
    F("protectForeground", BG, protect_foreground, K_BOOL, NULL),
    F("foreground", BG, foreground, K_RGBA, NULL),
    F("background", BG, background, K_RGBA, NULL),
};
static const sdesc BG_ERASE_S = {"struct BgEraseSettings", "BgEraseSettings", BG_ERASE_F, 6, sizeof(BG),
                                 def_bg_erase};

typedef pc_paint_brush BR;
static const field BRUSH_F[] = {
    F("size", BR, size, K_F32, NULL),
    F("hardness", BR, hardness, K_F32, NULL),
    F("spacing", BR, spacing, K_F32, NULL),
    F("spacingEnabled", BR, spacing_enabled, K_BOOL, NULL),
    F("opacity", BR, opacity, K_F32, NULL),
    F("flow", BR, flow, K_F32, NULL),
    {"pressureSize", "pressure_size", K_BOOL, D_CONT, 0, offsetof(BR, pressure_size), NULL},
    {"pressureOpacity", "pressure_opacity", K_BOOL, D_CONT, 0, offsetof(BR, pressure_opacity), NULL},
    F("color", BR, color, K_RGBA, NULL),
    F("background", BR, background, K_RGBA, NULL),
    F("erase", BR, erase, K_BOOL, NULL),
    F("mode", BR, mode, K_ENUM, &E_BLEND),
    F("tip", BR, tip, K_TIP, NULL),
    F("angle", BR, angle, K_F32, NULL),
    F("roundness", BR, roundness, K_F32, NULL),
    F("flipX", BR, flip_x, K_BOOL, NULL),
    F("flipY", BR, flip_y, K_BOOL, NULL),
    F("aliased", BR, aliased, K_BOOL, NULL),
    {"square", NULL, K_BOOL, D_CONT, 1, offsetof(BR, square), NULL},
    F("shapeDynamics", BR, shape_dynamics, K_STRUCT, &SHAPE_S),
    F("scattering", BR, scattering, K_STRUCT, &SCATTER_S),
    F("texture", BR, texture, K_STRUCT, &TEXTURE_S),
    F("dualBrush", BR, dual_brush, K_STRUCT, &DUAL_S),
    F("colorDynamics", BR, color_dynamics, K_STRUCT, &COLOR_S),
    F("transfer", BR, transfer, K_STRUCT, &TRANSFER_S),
    F("pose", BR, pose, K_STRUCT, &POSE_S),
    F("noise", BR, noise, K_BOOL, NULL),
    F("wetEdges", BR, wet_edges, K_BOOL, NULL),
    F("buildUp", BR, build_up, K_BOOL, NULL),
    F("buildUpRate", BR, build_up_rate, K_F32, NULL),
    F("smoothing", BR, smoothing, K_STRUCT, &SMOOTH_S),
    F("protectTexture", BR, protect_texture, K_BOOL, NULL),
    F("locks", BR, locks, K_STRUCT, &LOCKS_S),
    F("mixer", BR, mixer, K_STRUCT, &MIXER_S),
    F("seed", BR, seed, K_U64, NULL),
};
static const sdesc BRUSH_S = {"struct BrushSettings", "BrushSettings", BRUSH_F, 35, sizeof(BR), def_brush};

typedef pc_paint_stroke ST;
static const field STROKE_F[] = {
    {"brush", NULL, K_STRUCT, D_REQ, 0, offsetof(ST, brush), &BRUSH_S},
    {"points", NULL, K_POINTS, D_REQ, 0, offsetof(ST, points), NULL},
};
static const sdesc STROKE_S = {"struct Stroke", "Stroke", STROKE_F, 2, sizeof(ST), NULL};

typedef pc_paint_preset PR;
static const field PRESET_F[] = {
    {"name", NULL, K_STR, D_REQ, 0, offsetof(PR, name), NULL},
    {"brush", NULL, K_STRUCT, D_REQ, 0, offsetof(PR, brush), &BRUSH_S},
    {"builtin", NULL, K_BOOL, D_FIELD, 0, offsetof(PR, builtin), NULL},
    {"group", NULL, K_STR, D_FIELD, 1, offsetof(PR, group), NULL},
    {"folder", NULL, K_STRS, D_FIELD, 1, offsetof(PR, folder), NULL},
};
static const sdesc PRESET_S = {"struct BrushPreset", "BrushPreset", PRESET_F, 5, sizeof(PR), NULL};

/* ---- public entry points ---- */

/* How each public type is handled: a struct table, or a single pseudo-field. */
typedef struct tinfo {
    size_t size;
    const sdesc *s;
    field single; /* when s == NULL */
} tinfo;

static bool type_info(pc_paint_json_type t, tinfo *out)
{
    static const sdesc *const structs[PC_PAINT_JSON_TYPE_COUNT] = {
        [PC_PAINT_JSON_POINT] = &POINT_S,        [PC_PAINT_JSON_STROKE] = &STROKE_S,
        [PC_PAINT_JSON_BRUSH] = &BRUSH_S,        [PC_PAINT_JSON_PRESET] = &PRESET_S,
        [PC_PAINT_JSON_DYNAMIC] = &DYN_S,        [PC_PAINT_JSON_SHAPE_DYNAMICS] = &SHAPE_S,
        [PC_PAINT_JSON_SCATTERING] = &SCATTER_S, [PC_PAINT_JSON_TEXTURE] = &TEXTURE_S,
        [PC_PAINT_JSON_DUAL_BRUSH] = &DUAL_S,    [PC_PAINT_JSON_COLOR_DYNAMICS] = &COLOR_S,
        [PC_PAINT_JSON_TRANSFER] = &TRANSFER_S,  [PC_PAINT_JSON_SECTION_LOCKS] = &LOCKS_S,
        [PC_PAINT_JSON_POSE] = &POSE_S,          [PC_PAINT_JSON_SMOOTHING] = &SMOOTH_S,
        [PC_PAINT_JSON_MIXER] = &MIXER_S,        [PC_PAINT_JSON_REPLACE_SETTINGS] = &REPLACE_S,
        [PC_PAINT_JSON_BG_ERASE_SETTINGS] = &BG_ERASE_S,
    };
    memset(out, 0, sizeof *out);
    if ((unsigned)t >= PC_PAINT_JSON_TYPE_COUNT) {
        return false;
    }
    if (structs[t] != NULL) {
        out->s = structs[t];
        out->size = structs[t]->size;
        return true;
    }
    switch (t) {
    case PC_PAINT_JSON_GRAY_TILE:
        out->single = (field){"", NULL, K_TILE, D_REQ, 0, 0, NULL};
        out->size = sizeof(pc_paint_gray_tile);
        break;
    case PC_PAINT_JSON_CONTROL:
        out->single = (field){"", NULL, K_ENUM, D_REQ, 0, 0, &E_CONTROL};
        out->size = sizeof(pc_paint_control);
        break;
    case PC_PAINT_JSON_MASK_MODE:
        out->single = (field){"", NULL, K_ENUM, D_REQ, 0, 0, &E_MASK};
        out->size = sizeof(pc_paint_mask_mode);
        break;
    case PC_PAINT_JSON_PATTERN_STYLE:
        out->single = (field){"", NULL, K_ENUM, D_REQ, 0, 0, &E_STYLE};
        out->size = sizeof(pc_paint_pattern_style);
        break;
    case PC_PAINT_JSON_REPLACE_MODE:
        out->single = (field){"", NULL, K_ENUM, D_REQ, 0, 0, &E_REPLACE_MODE};
        out->size = sizeof(pc_paint_replace_mode);
        break;
    case PC_PAINT_JSON_SAMPLING:
        out->single = (field){"", NULL, K_ENUM, D_REQ, 0, 0, &E_SAMPLING};
        out->size = sizeof(pc_paint_sampling);
        break;
    case PC_PAINT_JSON_LIMITS:
        out->single = (field){"", NULL, K_ENUM, D_REQ, 0, 0, &E_LIMITS};
        out->size = sizeof(pc_paint_limits);
        break;
    case PC_PAINT_JSON_TIP:
        out->single = (field){"", NULL, K_TIP, D_REQ, 0, 0, NULL};
        out->size = sizeof(pc_paint_tip);
        break;
    default: /* PC_PAINT_JSON_PATTERN */
        out->single = (field){"", NULL, K_PATTERN, D_REQ, 0, 0, NULL};
        out->size = sizeof(pc_paint_pattern);
        break;
    }
    return true;
}

size_t pc_paint_json_size(pc_paint_json_type t)
{
    tinfo ti;
    return type_info(t, &ti) ? ti.size : 0;
}

pc_status pc_paint_json_read(pc_paint_json_type t, yyjson_val *v, void *out, pc_error *err)
{
    tinfo ti;
    void *tmp;
    pc_status st;
    if (!type_info(t, &ti) || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "unknown paint JSON type");
    }
    tmp = calloc(1, ti.size);
    if (tmp == NULL) {
        return fail_nomem(err);
    }
    st = ti.s != NULL ? read_struct(ti.s, v, tmp, err) : read_field(&ti.single, v, tmp, err);
    if (st == PC_OK) {
        memcpy(out, tmp, ti.size);
    }
    free(tmp);
    return st;
}

yyjson_mut_val *pc_paint_json_write(pc_paint_json_type t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    tinfo ti;
    if (d == NULL || v == NULL || !type_info(t, &ti)) {
        return NULL;
    }
    return ti.s != NULL ? write_struct(ti.s, d, v, flags) : write_field(&ti.single, d, v, flags);
}

void pc_paint_json_fini(pc_paint_json_type t, void *v)
{
    tinfo ti;
    if (v == NULL || !type_info(t, &ti)) {
        return;
    }
    if (ti.s != NULL) {
        fini_struct(ti.s, v);
    } else {
        fini_field(&ti.single, v);
    }
}

pc_status pc_paint_json_to_string(pc_paint_json_type t, const void *v, unsigned flags, pc_buf *out,
                                  pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root;
    pc_status st;
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory writing JSON");
    }
    root = pc_paint_json_write(t, d, v, flags);
    if (root == NULL) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory writing JSON");
    }
    st = pc_json_write(root, (flags & PC_PAINT_JSON_VALUE) != 0 ? PC_JSON_SORT_KEYS : 0, out, err);
    yyjson_mut_doc_free(d);
    return st;
}

pc_status pc_paint_json_from_string(pc_paint_json_type t, const char *text, size_t len, void *out,
                                    pc_error *err)
{
    yyjson_doc *doc = NULL;
    pc_status st = pc_json_parse(text, len, &doc, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_paint_json_read(t, yyjson_doc_get_root(doc), out, err);
    yyjson_doc_free(doc);
    return st;
}
