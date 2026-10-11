/* Internal to src/doc/json*.c: a small descriptor-driven serde for the document types.
 *
 * Every serialisable type gets a `jtype` (size, reader, writer, finaliser). Structs and enums are
 * described by field / variant tables and handled by generic code, so serde's behaviour is
 * implemented once: map entries are deserialised in sorted key order (serde_json::Value is a
 * BTreeMap, so the first failing field in key order is the one reported), missing fields are
 * checked afterwards in declaration order, `#[serde(default)]` (field and container), structs
 * from arrays (serde's visit_seq), externally tagged enums, and serde's error texts.
 *
 * Reader contract: `out` points at zeroed memory of `size` bytes. On success it holds the value;
 * on failure the reader has released whatever it built and left `out` zeroed. Readers never
 * recurse deeper than the JSON input (itself at most 127 levels deep) or the type nesting.
 *
 * Not part of the public API; doc/json.h is. */
#ifndef PC_DOC_JSON_UTIL_H
#define PC_DOC_JSON_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "doc/json.h"

/* Reader modes. */
enum {
    /* Deserialising through serde's private Content buffer (inside internally or adjacently
     * tagged enums): an over-long sequence reports "expected N elements in sequence". */
    JR_CONTENT = 1u
};

typedef struct jtype jtype;
typedef pc_status (*jt_read_fn)(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err);
typedef yyjson_mut_val *(*jt_write_fn)(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags);
typedef void (*jt_fini_fn)(const jtype *t, void *v);

struct jtype {
    size_t size;
    jt_read_fn read;
    jt_write_fn write;
    jt_fini_fn fini;   /* NULL for plain data */
    const jtype *elem; /* arrays, tuples' first element, vectors */
    size_t n;          /* fixed array length */
    const void *aux;   /* jstruct / jenum / jtuple */
};

/* ---- structs ---- */

typedef enum jf_kind {
    JF_VAL, /* the value at off */
    JF_OPT, /* Option<T>: bool flag at off2, value at off */
    JF_VEC  /* Vec<T>: T * at off, size_t count at off2 (NULL when empty) */
} jf_kind;

typedef enum jf_def {
    JD_REQ,  /* required (Option fields: missing is None) */
    JD_ZERO, /* #[serde(default)] whose default is all-zero (0, false, "", empty, None) */
    JD_TRUE, /* #[serde(default = "yes")] on a bool */
    JD_FN,   /* field default written by deffn */
    JD_CONT  /* the container's Default (jstruct.cdef) */
} jf_def;

typedef struct jfield {
    const char *name;
    const jtype *type;
    size_t off;
    size_t off2;
    unsigned char kind;      /* jf_kind */
    unsigned char def;       /* jf_def */
    unsigned char skip_none; /* skip_serializing_if = "Option::is_none" (JF_OPT) */
    void (*deffn)(void *slot);
} jfield;

typedef struct jstruct {
    const char *expecting; /* "struct Shadow", "struct variant Adjustment::Levels" */
    const jfield *f;
    size_t n;
    /* Container default (JD_CONT fields): writes the whole C struct (size bytes). */
    pc_status (*cdef)(void *out, pc_error *err);
    size_t size;
} jstruct;

#define JS_MAX_FIELDS 32

#define JFV(nm, T, m, jt) {(nm), &(jt), offsetof(T, m), 0, JF_VAL, JD_REQ, 0, NULL}
#define JFVD(nm, T, m, jt, d) {(nm), &(jt), offsetof(T, m), 0, JF_VAL, (d), 0, NULL}
#define JFFN(nm, T, m, jt, fn) {(nm), &(jt), offsetof(T, m), 0, JF_VAL, JD_FN, 0, (fn)}
#define JFO(nm, T, m, has, jt, d) {(nm), &(jt), offsetof(T, m), offsetof(T, has), JF_OPT, (d), 0, NULL}
#define JFOSKIP(nm, T, m, has, jt, d) {(nm), &(jt), offsetof(T, m), offsetof(T, has), JF_OPT, (d), 1, NULL}
#define JFVEC(nm, T, m, cnt, jt) {(nm), &(jt), offsetof(T, m), offsetof(T, cnt), JF_VEC, JD_REQ, 0, NULL}
#define JFVECD(nm, T, m, cnt, jt, d) {(nm), &(jt), offsetof(T, m), offsetof(T, cnt), JF_VEC, (d), 0, NULL}
#define JCOUNT(a) (sizeof(a) / sizeof((a)[0]))

/* Reads struct s from v (object, or array from element `start` on) into base. */
pc_status pc_jstruct_read(const jstruct *s, yyjson_val *v, void *base, unsigned mode, size_t start,
                          pc_error *err);
/* Adds s's fields to obj (declaration order). False on out-of-memory. */
bool pc_jstruct_write_fields(const jstruct *s, yyjson_mut_doc *d, yyjson_mut_val *obj, const void *base,
                             unsigned flags);
yyjson_mut_val *pc_jstruct_write(const jstruct *s, yyjson_mut_doc *d, const void *base, unsigned flags);
/* Finalises s's fields and zeroes them. */
void pc_jstruct_fini(const jstruct *s, void *base);

/* jtype callbacks for a struct type (aux = jstruct). */
pc_status pc_jt_struct_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err);
yyjson_mut_val *pc_jt_struct_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags);
void pc_jt_struct_fini(const jtype *t, void *v);

#define JT_STRUCT(T, st) {sizeof(T), pc_jt_struct_read, pc_jt_struct_write, pc_jt_struct_fini, NULL, 0, &(st)}

/* ---- externally tagged enums (C enums and kind + union types) ---- */

typedef enum jv_kind { JV_UNIT, JV_NEWTYPE, JV_STRUCT } jv_kind;

typedef struct jvariant {
    const char *name;
    jv_kind kind;
    const jtype *type; /* JV_NEWTYPE payload */
    size_t off;        /* JV_NEWTYPE payload offset */
    const jstruct *st; /* JV_STRUCT fields (offsets relative to the whole value) */
} jvariant;

typedef struct jenum {
    const jvariant *v;
    size_t n;
    size_t kind_off;  /* discriminant (variant index) offset */
    size_t kind_size; /* 1 (bool), 2 or 4 (C enum) */
} jenum;

#define JVU(nm) {(nm), JV_UNIT, NULL, 0, NULL}
#define JVN(nm, jt, T, m) {(nm), JV_NEWTYPE, &(jt), offsetof(T, m), NULL}
#define JVS(nm, st) {(nm), JV_STRUCT, NULL, 0, &(st)}

pc_status pc_jt_enum_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err);
yyjson_mut_val *pc_jt_enum_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags);
void pc_jt_enum_fini(const jtype *t, void *v);

/* A C enum with unit variants only (the value is the variant index). */
#define JT_UNIT_ENUM(T, en) {sizeof(T), pc_jt_enum_read, pc_jt_enum_write, NULL, NULL, 0, &(en)}
#define JENUM_UNITS(T, vars) {(vars), JCOUNT(vars), 0, sizeof(T)}
/* A tagged union: discriminant member `kind`. */
#define JT_TAGGED(T, en) {sizeof(T), pc_jt_enum_read, pc_jt_enum_write, pc_jt_enum_fini, NULL, 0, &(en)}
#define JENUM_TAGGED(T, vars, kindm) {(vars), JCOUNT(vars), offsetof(T, kindm), sizeof(((T *)0)->kindm)}

unsigned pc_jenum_get(const jenum *e, const void *v);
void pc_jenum_set(const jenum *e, void *v, unsigned idx);
/* Resolves an externally tagged value: *name / *nlen = the variant name, *payload = the map's
 * value (NULL for a plain string). */
pc_status pc_jenum_tag(yyjson_val *v, const char **name, size_t *nlen, yyjson_val **payload, pc_error *err);
/* Index of name among e's variants, else serde's "unknown variant" error. */
pc_status pc_jenum_find(const jenum *e, const char *name, size_t nlen, unsigned *idx, pc_error *err);

/* ---- internally tagged enums (#[serde(tag = "...")]): unit and struct variants ---- */

typedef struct jinternal {
    const char *tag;       /* "kind", "type" */
    const char *expecting; /* "internally tagged enum LiveShape" */
    const jenum *e;
} jinternal;

pc_status pc_jt_internal_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err);
/* The variant an internally tagged enum's tag names: a variant name, or (JR_CONTENT only) an
 * unsigned variant index, with serde's errors. */
pc_status pc_jinternal_tag(const jenum *e, yyjson_val *tag, unsigned mode, unsigned *idx, pc_error *err);
yyjson_mut_val *pc_jt_internal_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags);
void pc_jt_internal_fini(const jtype *t, void *v);
#define JT_INTERNAL(T, in)                                                                                   \
    {sizeof(T), pc_jt_internal_read, pc_jt_internal_write, pc_jt_internal_fini, NULL, 0, &(in)}

/* ---- tuples ---- */

typedef struct jtuple_el {
    const jtype *type;
    size_t off;
} jtuple_el;

typedef struct jtuple {
    const jtuple_el *el;
    size_t n;
} jtuple;

pc_status pc_jt_tuple_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err);
yyjson_mut_val *pc_jt_tuple_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags);
void pc_jt_tuple_fini(const jtype *t, void *v);
#define JT_TUPLE(T, tu) {sizeof(T), pc_jt_tuple_read, pc_jt_tuple_write, pc_jt_tuple_fini, NULL, 0, &(tu)}

/* ---- fixed arrays [T; N] and Vec<T> stored as a {T *, size_t} pair ---- */

pc_status pc_jt_array_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err);
yyjson_mut_val *pc_jt_array_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags);
void pc_jt_array_fini(const jtype *t, void *v);
/* CT = the C element type, ej = its jtype. */
#define JT_ARRAY(CT, ej, N)                                                                                  \
    {sizeof(CT) * (N), pc_jt_array_read, pc_jt_array_write, pc_jt_array_fini, &(ej), (N), NULL}

/* A value type holding a Vec<T> as {T *ptr; size_t len} at offsets given by aux (jvecpair). */
typedef struct jvecpair {
    size_t ptr_off;
    size_t len_off;
} jvecpair;
pc_status pc_jt_vecval_read(const jtype *t, yyjson_val *v, void *out, unsigned mode, pc_error *err);
yyjson_mut_val *pc_jt_vecval_write(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags);
void pc_jt_vecval_fini(const jtype *t, void *v);

/* Vec<T> stored as pointer + count anywhere. */
pc_status pc_jvec_read(const jtype *elem, yyjson_val *v, void **ptr, size_t *len, unsigned mode,
                       pc_error *err);
yyjson_mut_val *pc_jvec_write(const jtype *elem, yyjson_mut_doc *d, const void *ptr, size_t len,
                              unsigned flags);
void pc_jvec_fini(const jtype *elem, void **ptr, size_t *len);

/* ---- primitives ---- */

extern const jtype pc_jt_f32, pc_jt_f64, pc_jt_bool, pc_jt_u8, pc_jt_u16, pc_jt_u32, pc_jt_u64, pc_jt_usize,
    pc_jt_i32, pc_jt_str;
extern const jtype pc_jt_value;     /* serde_json::Value (pc_json_value; null = NULL doc) */
extern const jtype pc_jt_value_opt; /* Option<Value>: null = None (NULL doc) */
extern const jtype pc_jt_map;       /* Map<String, Value> (pc_json_value; NULL doc = {}) */
extern const jtype pc_jt_bytes;     /* Vec<u8> as pc_blob * (empty = NULL) */
extern const jtype pc_jt_bytes_opt; /* Option<Vec<u8>> as pc_blob * (None = NULL) */
extern const jtype pc_jt_f32s_opt;  /* Option<Arc<Vec<f32>>> as a pc_blob * of floats */
extern const jtype pc_jt_f32x2t;    /* (f32, f32) as float[2] */

/* Shared document types (defined in the json_*.c files). */
extern const jtype pc_jt_size, pc_jt_rect, pc_jt_point, pc_jt_affine, pc_jt_sample_type, pc_jt_color_mode,
    pc_jt_pixel_format, pc_jt_color, pc_jt_blend_mode, pc_jt_fill, pc_jt_curve_point, pc_jt_curve,
    pc_jt_gradient_style, pc_jt_gradient, pc_jt_contour, pc_jt_fx_common, pc_jt_color_stop, pc_jt_path,
    pc_jt_char_style, pc_jt_paragraph_style;

/* ---- helpers for hand-written readers ---- */

/* serde's description of an unexpected value: "string \"a\"", "integer `5`", "null", ... */
void pc_junexpected(yyjson_val *v, char *out, size_t cap);
/* "invalid type: <v>, expected <exp>" */
pc_status pc_jfail_type(yyjson_val *v, const char *exp, pc_error *err);
pc_status pc_jfail_missing(const char *field, pc_error *err);
pc_status pc_jfail_nomem(pc_error *err);
/* Reads t into a zeroed temporary and copies it to out on success (the public from_json). */
pc_status pc_jread_into(const jtype *t, yyjson_val *v, void *out, pc_error *err);
/* Writes NULL-safe: a NULL d or v gives NULL. */
yyjson_mut_val *pc_jwrite(const jtype *t, yyjson_mut_doc *d, const void *v, unsigned flags);
void pc_jfini(const jtype *t, void *v);

yyjson_mut_val *pc_jw_f32(yyjson_mut_doc *d, float v, unsigned flags);
yyjson_mut_val *pc_jw_f64(yyjson_mut_doc *d, double v, unsigned flags);
yyjson_mut_val *pc_jw_str(yyjson_mut_doc *d, const pc_str *s);

/* Public wrappers for a type T with descriptor J. */
#define PC_JSON_API(T, J)                                                                                    \
    yyjson_mut_val *T##_to_json(yyjson_mut_doc *d, const T *v, unsigned flags)                               \
    {                                                                                                        \
        return pc_jwrite(&(J), d, v, flags);                                                                 \
    }                                                                                                        \
    pc_status T##_from_json(yyjson_val *v, T *out, pc_error *err)                                            \
    {                                                                                                        \
        return pc_jread_into(&(J), v, out, err);                                                             \
    }                                                                                                        \
    const jtype *T##_jtype(void)                                                                             \
    {                                                                                                        \
        return &(J);                                                                                         \
    }

#endif
