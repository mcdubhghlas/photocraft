/* Descriptor-driven serde for the document types: the generic struct / enum / sequence code and
 * the primitive types (see json_util.h). Messages are serde's and serde_json's, verified against
 * serde_json 1.0.149 (`from_value::<T>(Value)`). */
#include "doc/json_util.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/utf8.h"

/* ---- messages ---- */

/* serde_json parses "-0" as the float -0.0, yyjson as the signed integer 0. Our own writers and
 * normalised values never produce a signed zero, so one can only come from such input. */
static bool neg_zero_int(yyjson_val *v)
{
    return yyjson_is_sint(v) && yyjson_get_sint(v) == 0;
}

void pc_junexpected(yyjson_val *v, char *out, size_t cap)
{
    char num[PC_JSON_NUM_MAX];
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (v == NULL || yyjson_is_null(v)) {
        (void)snprintf(out, cap, "null");
    } else if (yyjson_is_bool(v)) {
        (void)snprintf(out, cap, "boolean `%s`", yyjson_get_bool(v) ? "true" : "false");
    } else if (neg_zero_int(v)) {
        (void)snprintf(out, cap, "floating point `-0.0`");
    } else if (yyjson_is_uint(v)) {
        (void)snprintf(out, cap, "integer `%llu`", (unsigned long long)yyjson_get_uint(v));
    } else if (yyjson_is_sint(v)) {
        (void)snprintf(out, cap, "integer `%lld`", (long long)yyjson_get_sint(v));
    } else if (yyjson_is_real(v)) {
        (void)pc_json_format_f64(yyjson_get_real(v), num);
        (void)snprintf(out, cap, "floating point `%s`", num);
    } else if (yyjson_is_str(v)) {
        char q[PC_ERROR_MSG_MAX];
        pc_utf8_debug_quote((const uint8_t *)yyjson_get_str(v), yyjson_get_len(v), q, sizeof q);
        (void)snprintf(out, cap, "string %s", q);
    } else if (yyjson_is_arr(v)) {
        (void)snprintf(out, cap, "sequence");
    } else if (yyjson_is_obj(v)) {
        (void)snprintf(out, cap, "map");
    }
}

pc_status pc_jfail_type(yyjson_val *v, const char *exp, pc_error *err)
{
    char u[PC_ERROR_MSG_MAX];
    pc_junexpected(v, u, sizeof u);
    return pc_fail(err, PC_ERR_INVALID, "invalid type: %s, expected %s", u, exp);
}

pc_status pc_jfail_missing(const char *field, pc_error *err)
{
    return pc_fail(err, PC_ERR_INVALID, "missing field `%s`", field);
}

pc_status pc_jfail_nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "out of memory reading JSON");
}

/* An over-long sequence: serde_json's Value deserializer and serde's Content deserializer word
 * it differently. */
static pc_status fail_too_long(size_t total, size_t used, unsigned mode, pc_error *err)
{
    if ((mode & JR_CONTENT) != 0) {
        return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected %zu element%s in sequence", total,
                       used, used == 1 ? "" : "s");
    }
    return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected fewer elements in array", total);
}

/* ---- generic entry points ---- */

void pc_jfini(const jtype *t, void *v)
{
    if (t->fini != NULL) {
        t->fini(t, v);
    }
    memset(v, 0, t->size);
}

pc_status pc_jread_into(const jtype *t, yyjson_val *v, void *out, pc_error *err)
{
    void *tmp;
    pc_status st;
    if (out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no output value");
    }
    tmp = calloc(1, t->size);
    if (tmp == NULL) {
        return pc_jfail_nomem(err);
    }
    st = t->read(t, v, tmp, 0, err);
    if (st == PC_OK) {
        memcpy(out, tmp, t->size);
    }
    free(tmp);
    return st;
}

yyjson_mut_val *pc_jwrite(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    if (d == NULL || v == NULL) {
        return NULL;
    }
    return t->write(t, d, v, flags);
}

/* ---- primitive writers ---- */

yyjson_mut_val *pc_jw_f32(yyjson_mut_doc *d, float v, unsigned flags)
{
    if (!isfinite(v) && (flags & PC_DOC_JSON_NONFINITE) == 0) {
        return yyjson_mut_null(d); /* serde_json writes non-finite floats as null */
    }
    /* serde_json::to_value widens an f32 to f64 (Number::from_f64(f64::from(v))). */
    return (flags & PC_DOC_JSON_VALUE) != 0 ? yyjson_mut_real(d, (double)v) : yyjson_mut_float(d, v);
}

yyjson_mut_val *pc_jw_f64(yyjson_mut_doc *d, double v, unsigned flags)
{
    return isfinite(v) || (flags & PC_DOC_JSON_NONFINITE) != 0 ? yyjson_mut_real(d, v) : yyjson_mut_null(d);
}

yyjson_mut_val *pc_jw_str(yyjson_mut_doc *d, const pc_str *s)
{
    return yyjson_mut_strncpy(d, pc_str_c(s), s->ptr != NULL ? s->len : 0);
}

/* ---- numbers ---- */

static pc_status read_f32(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    float f;
    (void)t;
    (void)mode;
    /* serde's f32 visitor: `v as f32` from u64, i64 or f64 (each a single rounding). */
    if (neg_zero_int(v)) {
        f = -0.0f;
    } else if (yyjson_is_uint(v)) {
        f = (float)yyjson_get_uint(v);
    } else if (yyjson_is_sint(v)) {
        f = (float)yyjson_get_sint(v);
    } else if (yyjson_is_real(v)) {
        f = pc_f64_to_f32(yyjson_get_real(v));
    } else {
        return pc_jfail_type(v, "f32", err);
    }
    memcpy(out, &f, sizeof f);
    return PC_OK;
}

static yyjson_mut_val *write_f32(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    float f;
    (void)t;
    memcpy(&f, v, sizeof f);
    return pc_jw_f32(d, f, flags);
}

static pc_status read_f64(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    double f;
    (void)t;
    (void)mode;
    if (neg_zero_int(v)) {
        f = -0.0;
    } else if (yyjson_is_uint(v)) {
        f = (double)yyjson_get_uint(v);
    } else if (yyjson_is_sint(v)) {
        f = (double)yyjson_get_sint(v);
    } else if (yyjson_is_real(v)) {
        f = yyjson_get_real(v);
    } else {
        return pc_jfail_type(v, "f64", err);
    }
    memcpy(out, &f, sizeof f);
    return PC_OK;
}

static yyjson_mut_val *write_f64(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    double f;
    (void)t;
    memcpy(&f, v, sizeof f);
    return pc_jw_f64(d, f, flags);
}

/* An unsigned integer of at most `max` (serde's primitive visitors: floats are the wrong type,
 * out-of-range integers the wrong value). */
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
    if (yyjson_is_sint(v) && !neg_zero_int(v)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%lld`, expected %s",
                       (long long)yyjson_get_sint(v), name);
    }
    return pc_jfail_type(v, name, err);
}

static pc_status read_u8(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    uint64_t u = 0;
    pc_status st = read_uint(v, UINT8_MAX, "u8", &u, err);
    (void)t;
    (void)mode;
    if (st == PC_OK) {
        uint8_t x = (uint8_t)u;
        memcpy(out, &x, sizeof x);
    }
    return st;
}

static pc_status read_u16(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    uint64_t u = 0;
    pc_status st = read_uint(v, UINT16_MAX, "u16", &u, err);
    (void)t;
    (void)mode;
    if (st == PC_OK) {
        uint16_t x = (uint16_t)u;
        memcpy(out, &x, sizeof x);
    }
    return st;
}

static pc_status read_u32(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    uint64_t u = 0;
    pc_status st = read_uint(v, UINT32_MAX, "u32", &u, err);
    (void)t;
    (void)mode;
    if (st == PC_OK) {
        uint32_t x = (uint32_t)u;
        memcpy(out, &x, sizeof x);
    }
    return st;
}

static pc_status read_u64(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    uint64_t u = 0;
    pc_status st = read_uint(v, UINT64_MAX, "u64", &u, err);
    (void)t;
    (void)mode;
    if (st == PC_OK) {
        memcpy(out, &u, sizeof u);
    }
    return st;
}

static pc_status read_usize(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    uint64_t u = 0;
    pc_status st = read_uint(v, (uint64_t)SIZE_MAX, "usize", &u, err);
    (void)t;
    (void)mode;
    if (st == PC_OK) {
        size_t x = (size_t)u;
        memcpy(out, &x, sizeof x);
    }
    return st;
}

static pc_status read_i32(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    int32_t x;
    (void)t;
    (void)mode;
    if (yyjson_is_uint(v)) {
        uint64_t u = yyjson_get_uint(v);
        if (u > (uint64_t)INT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%llu`, expected i32",
                           (unsigned long long)u);
        }
        x = (int32_t)u;
    } else if (yyjson_is_sint(v) && !neg_zero_int(v)) {
        int64_t i = yyjson_get_sint(v);
        if (i < INT32_MIN || i > INT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%lld`, expected i32", (long long)i);
        }
        x = (int32_t)i;
    } else {
        return pc_jfail_type(v, "i32", err);
    }
    memcpy(out, &x, sizeof x);
    return PC_OK;
}

static yyjson_mut_val *write_u8(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    uint8_t x;
    (void)t;
    (void)flags;
    memcpy(&x, v, sizeof x);
    return yyjson_mut_uint(d, x);
}

static yyjson_mut_val *write_u16(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    uint16_t x;
    (void)t;
    (void)flags;
    memcpy(&x, v, sizeof x);
    return yyjson_mut_uint(d, x);
}

static yyjson_mut_val *write_u32(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    uint32_t x;
    (void)t;
    (void)flags;
    memcpy(&x, v, sizeof x);
    return yyjson_mut_uint(d, x);
}

static yyjson_mut_val *write_u64(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    uint64_t x;
    (void)t;
    (void)flags;
    memcpy(&x, v, sizeof x);
    return yyjson_mut_uint(d, x);
}

static yyjson_mut_val *write_usize(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    size_t x;
    (void)t;
    (void)flags;
    memcpy(&x, v, sizeof x);
    return yyjson_mut_uint(d, (uint64_t)x);
}

static yyjson_mut_val *write_i32(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    int32_t x;
    (void)t;
    (void)flags;
    memcpy(&x, v, sizeof x);
    /* serde_json::Value keeps non-negative integers unsigned. */
    return x >= 0 ? yyjson_mut_uint(d, (uint64_t)x) : yyjson_mut_sint(d, x);
}

/* ---- bool and strings ---- */

static pc_status read_bool(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    bool b;
    (void)t;
    (void)mode;
    if (!yyjson_is_bool(v)) {
        return pc_jfail_type(v, "a boolean", err);
    }
    b = yyjson_get_bool(v);
    memcpy(out, &b, sizeof b);
    return PC_OK;
}

static yyjson_mut_val *write_bool(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    bool b;
    (void)t;
    (void)flags;
    memcpy(&b, v, sizeof b);
    return yyjson_mut_bool(d, b);
}

static pc_status read_str(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    pc_str *s = out;
    (void)t;
    (void)mode;
    if (!yyjson_is_str(v)) {
        return pc_jfail_type(v, "a string", err);
    }
    if (pc_str_set(s, yyjson_get_str(v), yyjson_get_len(v), err) != PC_OK) {
        return pc_jfail_nomem(err);
    }
    return PC_OK;
}

static yyjson_mut_val *write_str(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    (void)t;
    (void)flags;
    return pc_jw_str(d, v);
}

static void fini_str(const jtype *t, void *v)
{
    (void)t;
    pc_str_fini(v);
}

/* ---- serde_json::Value fields ---- */

static pc_status read_value(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    pc_json_value *jv = out;
    (void)t;
    (void)mode;
    if (v == NULL || yyjson_is_null(v)) {
        jv->doc = NULL; /* Value::Null (and Option<Value>'s None) */
        return PC_OK;
    }
    if (pc_json_value_from_val(v, jv, err) != PC_OK) {
        return pc_jfail_nomem(err);
    }
    return PC_OK;
}

static yyjson_mut_val *write_value(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    (void)t;
    (void)flags;
    return pc_json_normalized_copy(d, pc_json_value_root(v));
}

static void fini_value(const jtype *t, void *v)
{
    (void)t;
    pc_json_value_fini(v);
}

static pc_status read_map(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    pc_json_value *jv = out;
    (void)t;
    (void)mode;
    if (!yyjson_is_obj(v)) {
        return pc_jfail_type(v, "a map", err);
    }
    if (yyjson_obj_size(v) == 0) {
        jv->doc = NULL; /* the empty set */
        return PC_OK;
    }
    if (pc_json_value_from_val(v, jv, err) != PC_OK) {
        return pc_jfail_nomem(err);
    }
    return PC_OK;
}

static yyjson_mut_val *write_map(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    yyjson_mut_val *root = pc_json_value_root(v);
    (void)t;
    (void)flags;
    if (root == NULL || !yyjson_mut_is_obj(root)) {
        return yyjson_mut_obj(d);
    }
    return pc_json_normalized_copy(d, root);
}

/* ---- byte and float vectors kept in blobs ---- */

static pc_status read_byte_blob(yyjson_val *v, bool empty_is_null, pc_blob **out, pc_error *err)
{
    size_t n;
    size_t i = 0;
    pc_blob *b = NULL;
    uint8_t *p = NULL;
    yyjson_arr_iter it;
    yyjson_val *e;
    if (!yyjson_is_arr(v)) {
        return pc_jfail_type(v, "a sequence", err);
    }
    n = yyjson_arr_size(v);
    if (n == 0 && empty_is_null) {
        *out = NULL;
        return PC_OK;
    }
    if (pc_blob_alloc(n, &b, err) != PC_OK) {
        return pc_jfail_nomem(err);
    }
    p = pc_blob_data_mut(b);
    (void)yyjson_arr_iter_init(v, &it);
    while ((e = yyjson_arr_iter_next(&it)) != NULL && i < n) {
        uint64_t u = 0;
        if (read_uint(e, UINT8_MAX, "u8", &u, err) != PC_OK) {
            pc_blob_release(b);
            return PC_ERR_INVALID;
        }
        p[i++] = (uint8_t)u;
    }
    *out = b;
    return PC_OK;
}

static yyjson_mut_val *write_byte_blob(yyjson_mut_doc *d, const pc_blob *b)
{
    const uint8_t *p = pc_blob_data(b);
    size_t n = pc_blob_len(b);
    size_t i;
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (arr == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (!yyjson_mut_arr_add_uint(d, arr, p[i])) {
            return NULL;
        }
    }
    return arr;
}

static pc_status read_bytes(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    pc_blob *b = NULL;
    pc_status st = read_byte_blob(v, true, &b, err);
    (void)t;
    (void)mode;
    if (st == PC_OK) {
        memcpy(out, &b, sizeof b);
    }
    return st;
}

static pc_status read_bytes_opt(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    pc_blob *b = NULL;
    pc_status st;
    (void)t;
    (void)mode;
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK; /* None */
    }
    st = read_byte_blob(v, false, &b, err);
    if (st == PC_OK) {
        memcpy(out, &b, sizeof b);
    }
    return st;
}

static yyjson_mut_val *write_bytes(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    pc_blob *b;
    (void)t;
    (void)flags;
    memcpy(&b, v, sizeof b);
    return write_byte_blob(d, b);
}

static yyjson_mut_val *write_bytes_opt(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    pc_blob *b;
    (void)t;
    (void)flags;
    memcpy(&b, v, sizeof b);
    return b == NULL ? yyjson_mut_null(d) : write_byte_blob(d, b);
}

static void fini_blob(const jtype *t, void *v)
{
    pc_blob *b;
    (void)t;
    memcpy(&b, v, sizeof b);
    pc_blob_release(b);
}

static pc_status read_f32s_opt(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    size_t n;
    size_t i = 0;
    pc_blob *b = NULL;
    size_t bytes;
    yyjson_arr_iter it;
    yyjson_val *e;
    (void)t;
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    if (!yyjson_is_arr(v)) {
        return pc_jfail_type(v, "a sequence", err);
    }
    n = yyjson_arr_size(v);
    if (!pc_size_mul(n, sizeof(float), &bytes) || pc_blob_alloc(bytes, &b, err) != PC_OK) {
        return pc_jfail_nomem(err);
    }
    (void)yyjson_arr_iter_init(v, &it);
    while ((e = yyjson_arr_iter_next(&it)) != NULL && i < n) {
        float f = 0.0f;
        if (read_f32(&pc_jt_f32, e, &f, mode, err) != PC_OK) {
            pc_blob_release(b);
            return PC_ERR_INVALID;
        }
        memcpy(pc_blob_data_mut(b) + i * sizeof f, &f, sizeof f);
        i++;
    }
    memcpy(out, &b, sizeof b);
    return PC_OK;
}

static yyjson_mut_val *write_f32s_opt(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    pc_blob *b;
    const uint8_t *p;
    size_t n;
    size_t i;
    yyjson_mut_val *arr;
    (void)t;
    memcpy(&b, v, sizeof b);
    if (b == NULL) {
        return yyjson_mut_null(d);
    }
    p = pc_blob_data(b);
    n = pc_blob_len(b) / sizeof(float);
    arr = yyjson_mut_arr(d);
    if (arr == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        float f;
        memcpy(&f, p + i * sizeof f, sizeof f);
        if (!yyjson_mut_arr_append(arr, pc_jw_f32(d, f, flags))) {
            return NULL;
        }
    }
    return arr;
}

const jtype pc_jt_f32 = {sizeof(float), read_f32, write_f32, NULL, NULL, 0, NULL};
const jtype pc_jt_f64 = {sizeof(double), read_f64, write_f64, NULL, NULL, 0, NULL};
const jtype pc_jt_bool = {sizeof(bool), read_bool, write_bool, NULL, NULL, 0, NULL};
const jtype pc_jt_u8 = {sizeof(uint8_t), read_u8, write_u8, NULL, NULL, 0, NULL};
const jtype pc_jt_u16 = {sizeof(uint16_t), read_u16, write_u16, NULL, NULL, 0, NULL};
const jtype pc_jt_u32 = {sizeof(uint32_t), read_u32, write_u32, NULL, NULL, 0, NULL};
const jtype pc_jt_u64 = {sizeof(uint64_t), read_u64, write_u64, NULL, NULL, 0, NULL};
const jtype pc_jt_usize = {sizeof(size_t), read_usize, write_usize, NULL, NULL, 0, NULL};
const jtype pc_jt_i32 = {sizeof(int32_t), read_i32, write_i32, NULL, NULL, 0, NULL};
const jtype pc_jt_str = {sizeof(pc_str), read_str, write_str, fini_str, NULL, 0, NULL};
const jtype pc_jt_value = {sizeof(pc_json_value), read_value, write_value, fini_value, NULL, 0, NULL};
const jtype pc_jt_value_opt = {sizeof(pc_json_value), read_value, write_value, fini_value, NULL, 0, NULL};
const jtype pc_jt_map = {sizeof(pc_json_value), read_map, write_map, fini_value, NULL, 0, NULL};
const jtype pc_jt_bytes = {sizeof(pc_blob *), read_bytes, write_bytes, fini_blob, NULL, 0, NULL};
const jtype pc_jt_bytes_opt = {sizeof(pc_blob *), read_bytes_opt, write_bytes_opt, fini_blob, NULL, 0, NULL};
const jtype pc_jt_f32s_opt = {sizeof(pc_blob *), read_f32s_opt, write_f32s_opt, fini_blob, NULL, 0, NULL};

static const jtuple_el f32x2_el[] = {{&pc_jt_f32, 0}, {&pc_jt_f32, sizeof(float)}};
static const jtuple f32x2_tu = {f32x2_el, 2};
const jtype pc_jt_f32x2t = {2 * sizeof(float), pc_jt_tuple_read, pc_jt_tuple_write, pc_jt_tuple_fini, NULL, 0,
                            &f32x2_tu};

/* ---- sequences ---- */

/* Reads up to n elements of an array into consecutive slots; `what` is the expected text
 * ("an array of length 4", "a tuple of size 2"). Element i goes to slot(i). */
typedef struct seq_target {
    const jtype *const *types; /* per element (tuples) or NULL */
    const jtype *elem;         /* all elements (arrays) */
    const size_t *offs;        /* per element offsets (tuples) or NULL */
} seq_target;

static const jtype *seq_type(const seq_target *s, size_t i)
{
    return s->types != NULL ? s->types[i] : s->elem;
}

static size_t seq_off(const seq_target *s, size_t i)
{
    return s->offs != NULL ? s->offs[i] : i * s->elem->size;
}

static void seq_fini(const seq_target *s, char *out, size_t upto)
{
    size_t i;
    for (i = 0; i < upto; i++) {
        pc_jfini(seq_type(s, i), out + seq_off(s, i));
    }
}

static pc_status seq_read(const seq_target *s, size_t n, const char *what, yyjson_val *v, char *out,
                          unsigned mode, pc_error *err)
{
    yyjson_arr_iter it;
    size_t len;
    size_t i;
    if (!yyjson_is_arr(v)) {
        return pc_jfail_type(v, what, err);
    }
    len = yyjson_arr_size(v);
    (void)yyjson_arr_iter_init(v, &it);
    for (i = 0; i < n; i++) {
        yyjson_val *e = yyjson_arr_iter_next(&it);
        pc_status st;
        if (e == NULL) {
            seq_fini(s, out, i);
            return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected %s", i, what);
        }
        st = seq_type(s, i)->read(seq_type(s, i), e, out + seq_off(s, i), mode, err);
        if (st != PC_OK) {
            seq_fini(s, out, i);
            return st;
        }
    }
    if (len > n) {
        seq_fini(s, out, n);
        return fail_too_long(len, n, mode, err);
    }
    return PC_OK;
}

pc_status pc_jt_array_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    char what[64];
    seq_target s = {NULL, t->elem, NULL};
    (void)snprintf(what, sizeof what, "an array of length %zu", t->n);
    return seq_read(&s, t->n, what, v, out, mode, err);
}

yyjson_mut_val *pc_jt_array_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    const char *p = v;
    size_t i;
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (arr == NULL) {
        return NULL;
    }
    for (i = 0; i < t->n; i++) {
        if (!yyjson_mut_arr_append(arr, t->elem->write(t->elem, d, p + i * t->elem->size, flags))) {
            return NULL;
        }
    }
    return arr;
}

void pc_jt_array_fini(const jtype *t, void *v)
{
    seq_target s = {NULL, t->elem, NULL};
    seq_fini(&s, v, t->n);
}

enum { MAX_TUPLE = 4 };

static bool tuple_target(const jtuple *tu, const jtype *types[MAX_TUPLE], size_t offs[MAX_TUPLE],
                         seq_target *s)
{
    size_t i;
    if (tu->n > MAX_TUPLE) {
        return false;
    }
    for (i = 0; i < tu->n; i++) {
        types[i] = tu->el[i].type;
        offs[i] = tu->el[i].off;
    }
    s->types = types;
    s->elem = NULL;
    s->offs = offs;
    return true;
}

pc_status pc_jt_tuple_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    const jtuple *tu = t->aux;
    const jtype *types[MAX_TUPLE];
    size_t offs[MAX_TUPLE];
    seq_target s;
    char what[64];
    if (!tuple_target(tu, types, offs, &s)) {
        return pc_fail(err, PC_ERR_INVALID, "tuple too long");
    }
    (void)snprintf(what, sizeof what, "a tuple of size %zu", tu->n);
    return seq_read(&s, tu->n, what, v, out, mode, err);
}

yyjson_mut_val *pc_jt_tuple_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    const jtuple *tu = t->aux;
    const char *p = v;
    size_t i;
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (arr == NULL) {
        return NULL;
    }
    for (i = 0; i < tu->n; i++) {
        const jtype *et = tu->el[i].type;
        if (!yyjson_mut_arr_append(arr, et->write(et, d, p + tu->el[i].off, flags))) {
            return NULL;
        }
    }
    return arr;
}

void pc_jt_tuple_fini(const jtype *t, void *v)
{
    const jtuple *tu = t->aux;
    size_t i;
    for (i = 0; i < tu->n; i++) {
        pc_jfini(tu->el[i].type, (char *)v + tu->el[i].off);
    }
}

/* ---- Vec<T> ---- */

pc_status pc_jvec_read(const jtype *elem, yyjson_val *v, void **ptr, size_t *len, unsigned mode,
                       pc_error *err)
{
    yyjson_arr_iter it;
    yyjson_val *e;
    size_t n;
    size_t i = 0;
    char *arr;
    if (!yyjson_is_arr(v)) {
        return pc_jfail_type(v, "a sequence", err);
    }
    n = yyjson_arr_size(v);
    if (n == 0) {
        *ptr = NULL;
        *len = 0;
        return PC_OK;
    }
    arr = pc_calloc_array(n, elem->size);
    if (arr == NULL) {
        return pc_jfail_nomem(err);
    }
    (void)yyjson_arr_iter_init(v, &it);
    while (i < n && (e = yyjson_arr_iter_next(&it)) != NULL) {
        pc_status st = elem->read(elem, e, arr + i * elem->size, mode, err);
        if (st != PC_OK) {
            size_t k;
            for (k = 0; k < i; k++) {
                pc_jfini(elem, arr + k * elem->size);
            }
            free(arr);
            return st;
        }
        i++;
    }
    *ptr = arr;
    *len = i;
    return PC_OK;
}

yyjson_mut_val *pc_jvec_write(const jtype *elem, yyjson_mut_doc *d, const void *ptr, size_t len,
                              unsigned flags)
{
    const char *p = ptr;
    size_t i;
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    if (arr == NULL) {
        return NULL;
    }
    for (i = 0; i < len && p != NULL; i++) {
        if (!yyjson_mut_arr_append(arr, elem->write(elem, d, p + i * elem->size, flags))) {
            return NULL;
        }
    }
    return arr;
}

void pc_jvec_fini(const jtype *elem, void **ptr, size_t *len)
{
    char *p = *ptr;
    size_t i;
    if (p != NULL) {
        for (i = 0; i < *len; i++) {
            pc_jfini(elem, p + i * elem->size);
        }
    }
    free(p);
    *ptr = NULL;
    *len = 0;
}

/* Pointer members of any object type share void *'s representation (POSIX), so they are moved
 * through memcpy rather than type-punned lvalues. */
static void get_vec(const void *base, size_t ptr_off, size_t len_off, void **p, size_t *n)
{
    memcpy(p, (const char *)base + ptr_off, sizeof *p);
    memcpy(n, (const char *)base + len_off, sizeof *n);
}

static void set_vec(void *base, size_t ptr_off, size_t len_off, void *p, size_t n)
{
    memcpy((char *)base + ptr_off, &p, sizeof p);
    memcpy((char *)base + len_off, &n, sizeof n);
}

pc_status pc_jt_vecval_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    const jvecpair *vp = t->aux;
    void *p = NULL;
    size_t n = 0;
    pc_status st = pc_jvec_read(t->elem, v, &p, &n, mode, err);
    if (st == PC_OK) {
        set_vec(out, vp->ptr_off, vp->len_off, p, n);
    }
    return st;
}

yyjson_mut_val *pc_jt_vecval_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    const jvecpair *vp = t->aux;
    void *p;
    size_t n;
    get_vec(v, vp->ptr_off, vp->len_off, &p, &n);
    return pc_jvec_write(t->elem, d, p, n, flags);
}

void pc_jt_vecval_fini(const jtype *t, void *v)
{
    const jvecpair *vp = t->aux;
    void *p;
    size_t n;
    get_vec(v, vp->ptr_off, vp->len_off, &p, &n);
    pc_jvec_fini(t->elem, &p, &n);
    set_vec(v, vp->ptr_off, vp->len_off, NULL, 0);
}

/* ---- structs ---- */

static pc_status field_read(const jfield *f, yyjson_val *v, void *base, unsigned mode, pc_error *err)
{
    char *b = base;
    pc_status st;
    switch ((jf_kind)f->kind) {
    case JF_OPT:
        if (yyjson_is_null(v)) {
            return PC_OK; /* None: the flag stays false */
        }
        st = f->type->read(f->type, v, b + f->off, mode, err);
        if (st == PC_OK) {
            bool t = true;
            memcpy(b + f->off2, &t, sizeof t);
        }
        return st;
    case JF_VEC: {
        void *p = NULL;
        size_t n = 0;
        st = pc_jvec_read(f->type, v, &p, &n, mode, err);
        if (st == PC_OK) {
            set_vec(base, f->off, f->off2, p, n);
        }
        return st;
    }
    case JF_VAL:
    default:
        return f->type->read(f->type, v, b + f->off, mode, err);
    }
}

static void field_fini(const jfield *f, void *base)
{
    char *b = base;
    switch ((jf_kind)f->kind) {
    case JF_OPT: {
        bool no = false;
        pc_jfini(f->type, b + f->off);
        memcpy(b + f->off2, &no, sizeof no);
        break;
    }
    case JF_VEC: {
        void *p;
        size_t n;
        get_vec(base, f->off, f->off2, &p, &n);
        pc_jvec_fini(f->type, &p, &n);
        set_vec(base, f->off, f->off2, NULL, 0);
        break;
    }
    case JF_VAL:
    default:
        pc_jfini(f->type, b + f->off);
        break;
    }
}

/* Moves field f's bytes from src to dst (and zeroes them in src). */
static void field_move(const jfield *f, void *dst, void *src)
{
    char *d = dst;
    char *s = src;
    size_t sz1 = f->kind == JF_VEC ? sizeof(void *) : f->type->size;
    memcpy(d + f->off, s + f->off, sz1);
    memset(s + f->off, 0, sz1);
    if (f->kind == JF_VEC) {
        memcpy(d + f->off2, s + f->off2, sizeof(size_t));
        memset(s + f->off2, 0, sizeof(size_t));
    } else if (f->kind == JF_OPT) {
        memcpy(d + f->off2, s + f->off2, sizeof(bool));
        memset(s + f->off2, 0, sizeof(bool));
    }
}

void pc_jstruct_fini(const jstruct *s, void *base)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        field_fini(&s->f[i], base);
    }
}

/* The default of a field that is absent. *cont is set when it comes from the container's
 * Default (applied afterwards). False when the field is required. */
static bool field_default(const jfield *f, void *base, bool *cont)
{
    char *b = base;
    switch ((jf_def)f->def) {
    case JD_ZERO:
        return true;
    case JD_TRUE: {
        bool t = true;
        memcpy(b + f->off, &t, sizeof t);
        return true;
    }
    case JD_FN:
        if (f->deffn != NULL) {
            f->deffn(b + f->off);
        }
        return true;
    case JD_CONT:
        *cont = true;
        return true;
    case JD_REQ:
    default:
        return false;
    }
}

static pc_status apply_container_default(const jstruct *s, void *base, const bool *missing, pc_error *err)
{
    void *tmp;
    size_t i;
    if (s->cdef == NULL) {
        return PC_OK;
    }
    tmp = calloc(1, s->size);
    if (tmp == NULL) {
        return pc_jfail_nomem(err);
    }
    if (s->cdef(tmp, err) != PC_OK) {
        free(tmp);
        return pc_jfail_nomem(err);
    }
    for (i = 0; i < s->n; i++) {
        if (missing[i] && s->f[i].def == JD_CONT) {
            field_move(&s->f[i], base, tmp);
        }
    }
    pc_jstruct_fini(s, tmp);
    free(tmp);
    return PC_OK;
}

static pc_status struct_from_map(const jstruct *s, yyjson_val *v, void *base, unsigned mode, pc_error *err)
{
    size_t order[JS_MAX_FIELDS];
    bool missing[JS_MAX_FIELDS];
    size_t i;
    bool cont = false;
    /* serde_json::Value objects are sorted maps: fields are visited in key order. */
    for (i = 0; i < s->n; i++) {
        size_t j = i;
        while (j > 0 && strcmp(s->f[order[j - 1]].name, s->f[i].name) > 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    for (i = 0; i < s->n; i++) {
        const jfield *f = &s->f[order[i]];
        yyjson_val *fv = pc_json_get(v, f->name);
        pc_status st;
        missing[order[i]] = fv == NULL;
        if (fv == NULL) {
            continue;
        }
        st = field_read(f, fv, base, mode, err);
        if (st != PC_OK) {
            pc_jstruct_fini(s, base);
            return st;
        }
    }
    for (i = 0; i < s->n; i++) {
        const jfield *f = &s->f[i];
        if (missing[i] && !field_default(f, base, &cont) && f->kind != JF_OPT) {
            pc_jstruct_fini(s, base);
            return pc_jfail_missing(f->name, err);
        }
    }
    if (cont && apply_container_default(s, base, missing, err) != PC_OK) {
        pc_jstruct_fini(s, base);
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

static pc_status struct_from_seq(const jstruct *s, yyjson_val *v, void *base, unsigned mode, size_t start,
                                 pc_error *err)
{
    bool missing[JS_MAX_FIELDS];
    yyjson_arr_iter it;
    size_t len = yyjson_arr_size(v);
    size_t avail = len > start ? len - start : 0;
    size_t i;
    bool cont = false;
    (void)yyjson_arr_iter_init(v, &it);
    for (i = 0; i < start; i++) {
        (void)yyjson_arr_iter_next(&it);
    }
    for (i = 0; i < s->n; i++) {
        const jfield *f = &s->f[i];
        missing[i] = i >= avail;
        if (!missing[i]) {
            pc_status st = field_read(f, yyjson_arr_iter_next(&it), base, mode, err);
            if (st != PC_OK) {
                pc_jstruct_fini(s, base);
                return st;
            }
        } else if (!field_default(f, base, &cont)) {
            pc_jstruct_fini(s, base);
            return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected %s with %zu element%s", i,
                           s->expecting, s->n, s->n == 1 ? "" : "s");
        }
    }
    if (avail > s->n) {
        pc_jstruct_fini(s, base);
        return fail_too_long((mode & JR_CONTENT) != 0 ? avail : len, s->n, mode, err);
    }
    if (cont && apply_container_default(s, base, missing, err) != PC_OK) {
        pc_jstruct_fini(s, base);
        return PC_ERR_NOMEM;
    }
    return PC_OK;
}

pc_status pc_jstruct_read(const jstruct *s, yyjson_val *v, void *base, unsigned mode, size_t start,
                          pc_error *err)
{
    if (s->n > JS_MAX_FIELDS) {
        return pc_fail(err, PC_ERR_INVALID, "too many fields");
    }
    if (yyjson_is_obj(v)) {
        return struct_from_map(s, v, base, mode, err);
    }
    if (yyjson_is_arr(v)) {
        return struct_from_seq(s, v, base, mode, start, err);
    }
    return pc_jfail_type(v, s->expecting, err);
}

bool pc_jstruct_write_fields(const jstruct *s, yyjson_mut_doc *d, yyjson_mut_val *obj, const void *base,
                             unsigned flags)
{
    const char *b = base;
    size_t i;
    for (i = 0; i < s->n; i++) {
        const jfield *f = &s->f[i];
        yyjson_mut_val *fv;
        switch ((jf_kind)f->kind) {
        case JF_OPT: {
            bool has;
            memcpy(&has, b + f->off2, sizeof has);
            if (!has && f->skip_none) {
                continue;
            }
            fv = has ? f->type->write(f->type, d, b + f->off, flags) : yyjson_mut_null(d);
            break;
        }
        case JF_VEC: {
            void *p;
            size_t n;
            get_vec(base, f->off, f->off2, &p, &n);
            fv = pc_jvec_write(f->type, d, p, n, flags);
            break;
        }
        case JF_VAL:
        default:
            fv = f->type->write(f->type, d, b + f->off, flags);
            break;
        }
        /* Field names are static strings, so the document may reference them. */
        if (fv == NULL || !yyjson_mut_obj_add(obj, yyjson_mut_str(d, f->name), fv)) {
            return false;
        }
    }
    return true;
}

yyjson_mut_val *pc_jstruct_write(const jstruct *s, yyjson_mut_doc *d, const void *base, unsigned flags)
{
    yyjson_mut_val *obj = yyjson_mut_obj(d);
    if (obj == NULL || !pc_jstruct_write_fields(s, d, obj, base, flags)) {
        return NULL;
    }
    return obj;
}

pc_status pc_jt_struct_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    return pc_jstruct_read(t->aux, v, out, mode, 0, err);
}

yyjson_mut_val *pc_jt_struct_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    return pc_jstruct_write(t->aux, d, v, flags);
}

void pc_jt_struct_fini(const jtype *t, void *v)
{
    pc_jstruct_fini(t->aux, v);
}

/* ---- externally tagged enums ---- */

unsigned pc_jenum_get(const jenum *e, const void *v)
{
    const char *p = (const char *)v + e->kind_off;
    switch (e->kind_size) {
    case 1: {
        uint8_t x;
        memcpy(&x, p, 1);
        return x;
    }
    case 2: {
        uint16_t x;
        memcpy(&x, p, 2);
        return x;
    }
    case 4: {
        uint32_t x;
        memcpy(&x, p, 4);
        return x;
    }
    default:
        return UINT32_MAX;
    }
}

void pc_jenum_set(const jenum *e, void *v, unsigned idx)
{
    char *p = (char *)v + e->kind_off;
    switch (e->kind_size) {
    case 1: {
        uint8_t x = (uint8_t)idx;
        memcpy(p, &x, 1);
        break;
    }
    case 2: {
        uint16_t x = (uint16_t)idx;
        memcpy(p, &x, 2);
        break;
    }
    case 4: {
        uint32_t x = (uint32_t)idx;
        memcpy(p, &x, 4);
        break;
    }
    default:
        break;
    }
}

pc_status pc_jenum_tag(yyjson_val *v, const char **name, size_t *nlen, yyjson_val **payload, pc_error *err)
{
    if (yyjson_is_str(v)) {
        *name = yyjson_get_str(v);
        *nlen = yyjson_get_len(v);
        *payload = NULL;
        return PC_OK;
    }
    if (yyjson_is_obj(v)) {
        /* A serde_json::Value map has unique keys (the last duplicate wins), so "one key" means
         * one distinct key here. */
        yyjson_obj_iter it;
        yyjson_val *k;
        yyjson_val *first = NULL;
        yyjson_val *last = NULL;
        (void)yyjson_obj_iter_init(v, &it);
        while ((k = yyjson_obj_iter_next(&it)) != NULL) {
            if (first == NULL) {
                first = k;
            } else if (yyjson_get_len(k) != yyjson_get_len(first) ||
                       memcmp(yyjson_get_str(k), yyjson_get_str(first), yyjson_get_len(k)) != 0) {
                first = NULL;
                break;
            }
            last = k;
        }
        if (first == NULL || last == NULL) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: map, expected map with a single key");
        }
        *name = yyjson_get_str(last);
        *nlen = yyjson_get_len(last);
        *payload = yyjson_obj_iter_get_val(last);
        return PC_OK;
    }
    return pc_jfail_type(v, "string or map", err);
}

pc_status pc_jenum_find(const jenum *e, const char *name, size_t nlen, unsigned *idx, pc_error *err)
{
    char list[PC_ERROR_MSG_MAX];
    size_t used = 0;
    size_t i;
    for (i = 0; i < e->n; i++) {
        if (strlen(e->v[i].name) == nlen && memcmp(e->v[i].name, name, nlen) == 0) {
            *idx = (unsigned)i;
            return PC_OK;
        }
    }
    /* serde's OneOf: "`A`", "`A` or `B`", "one of `A`, `B`, `C`". */
    list[0] = '\0';
    if (e->n == 2) {
        (void)snprintf(list, sizeof list, "`%s` or `%s`", e->v[0].name, e->v[1].name);
    } else {
        if (e->n > 2) {
            used = (size_t)snprintf(list, sizeof list, "one of ");
        }
        for (i = 0; i < e->n && used < sizeof list; i++) {
            int w = snprintf(list + used, sizeof list - used, "%s`%s`", i == 0 ? "" : ", ", e->v[i].name);
            if (w < 0) {
                break;
            }
            used += (size_t)w;
        }
    }
    return pc_fail(err, PC_ERR_INVALID, "unknown variant `%.*s`, expected %s", (int)(nlen > 200 ? 200 : nlen),
                   name, list);
}

pc_status pc_jt_enum_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err)
{
    const jenum *e = t->aux;
    const jvariant *var;
    const char *name = NULL;
    size_t nlen = 0;
    yyjson_val *payload = NULL;
    unsigned idx = 0;
    pc_status st = pc_jenum_tag(v, &name, &nlen, &payload, err);
    if (st != PC_OK || (st = pc_jenum_find(e, name, nlen, &idx, err)) != PC_OK) {
        return st;
    }
    var = &e->v[idx];
    switch (var->kind) {
    case JV_NEWTYPE:
        if (payload == NULL) {
            return pc_fail(err, PC_ERR_INVALID, "invalid type: unit variant, expected newtype variant");
        }
        st = var->type->read(var->type, payload, (char *)out + var->off, mode, err);
        break;
    case JV_STRUCT:
        if (payload == NULL) {
            return pc_fail(err, PC_ERR_INVALID, "invalid type: unit variant, expected struct variant");
        }
        /* serde_json's Value accepts only a map for a struct variant; serde's Content (inside a
         * tagged enum) also a sequence. */
        if (yyjson_is_obj(payload) || (yyjson_is_arr(payload) && (mode & JR_CONTENT) != 0)) {
            st = pc_jstruct_read(var->st, payload, out, mode, 0, err);
        } else {
            st = pc_jfail_type(payload, "struct variant", err);
        }
        break;
    case JV_UNIT:
    default:
        if (payload != NULL && !yyjson_is_null(payload)) {
            return pc_jfail_type(payload, "unit", err);
        }
        break;
    }
    if (st == PC_OK) {
        pc_jenum_set(e, out, idx);
    }
    return st;
}

yyjson_mut_val *pc_jt_enum_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags)
{
    const jenum *e = t->aux;
    unsigned idx = pc_jenum_get(e, v);
    const jvariant *var;
    yyjson_mut_val *inner;
    yyjson_mut_val *obj;
    if (idx >= e->n) {
        return yyjson_mut_null(d); /* not a valid value; never crash on it */
    }
    var = &e->v[idx];
    switch (var->kind) {
    case JV_NEWTYPE:
        inner = var->type->write(var->type, d, (const char *)v + var->off, flags);
        break;
    case JV_STRUCT:
        inner = pc_jstruct_write(var->st, d, v, flags);
        break;
    case JV_UNIT:
    default:
        return yyjson_mut_str(d, var->name);
    }
    obj = yyjson_mut_obj(d);
    if (inner == NULL || obj == NULL || !yyjson_mut_obj_add(obj, yyjson_mut_str(d, var->name), inner)) {
        return NULL;
    }
    return obj;
}

void pc_jt_enum_fini(const jtype *t, void *v)
{
    const jenum *e = t->aux;
    unsigned idx = pc_jenum_get(e, v);
    const jvariant *var;
    if (idx >= e->n) {
        return;
    }
    var = &e->v[idx];
    if (var->kind == JV_NEWTYPE) {
        pc_jfini(var->type, (char *)v + var->off);
    } else if (var->kind == JV_STRUCT) {
        pc_jstruct_fini(var->st, v);
    }
}
