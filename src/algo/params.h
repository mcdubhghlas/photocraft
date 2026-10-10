/* Filter option enums, parameter objects and their serde-compatible JSON (port of the enums in
 * crates/algo/src/lib.rs and of crates/algo/src/params_ext.rs).
 *
 * Every Rust option enum is a C enum plus a pc_algo_enum_desc naming its serde variants
 * (camelCase) and its #[default]. Parameter structs (Light, the Blur Gallery pins, Path Blur
 * paths) have *_default() initialisers matching the Rust Default impls.
 *
 * JSON: a small descriptor-driven serde (pc_algo_jfield tables) that reads and writes parameter
 * structs the way serde does for the reference's FilterParams (an internally tagged enum, so
 * fields arrive through serde's Content buffer):
 *   - fields are visited in byte order of their keys with the last duplicate winning (the
 *     reference deserialises from a serde_json::Value, a BTreeMap), unknown keys are ignored,
 *     and missing required fields are reported after the walk in declaration order;
 *   - f32 accepts any JSON number (`as f32`), u32/i32 integers in range ("invalid value:
 *     integer `-1`, expected u32"), bool only booleans ("expected a boolean"), enums a variant
 *     name ("unknown variant `x`, expected one of `a`, `b`") or a one-key map whose value is null;
 *   - fixed float arrays ([f32; N]) take exactly N numbers;
 *   - vectors (Vec<[f32; 2]>, Vec<f32>, Vec<Light>, pins, paths) take arrays; their struct
 *     elements are maps with #[serde(default)] semantics (missing fields keep the default);
 *   - writing emits fields in declaration order; f32 as f32 text, non-finite as null.
 * Error texts are serde's, verified against serde_json 1.0 (`rustref algo-core-vectors`).
 * Structs given as JSON arrays (serde's visit_seq) are refused ("invalid type: sequence, ...").
 *
 * Later algo chunks describe their FilterParams variants with these tables (see filter.h). */
#ifndef PC_ALGO_PARAMS_H
#define PC_ALGO_PARAMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/status.h"

/* ---- enum descriptors ---- */

typedef struct pc_algo_enum_desc {
    const char *rust_name;    /* "RadialMethod" */
    const char *const *names; /* serde names in declaration order */
    size_t n;
    int def; /* #[default] variant */
} pc_algo_enum_desc;

/* Name of variant v ("" when out of range). */
const char *pc_algo_enum_name(const pc_algo_enum_desc *d, int v);
/* Variant named s[0..len); false when unknown. */
bool pc_algo_enum_from_name(const pc_algo_enum_desc *d, const char *s, size_t len, int *out);

/* lib.rs */
typedef enum pc_algo_radial_method { PC_ALGO_RADIAL_SPIN, PC_ALGO_RADIAL_ZOOM } pc_algo_radial_method;
typedef enum pc_algo_radial_quality {
    PC_ALGO_RADIAL_DRAFT,
    PC_ALGO_RADIAL_GOOD,
    PC_ALGO_RADIAL_BEST
} pc_algo_radial_quality;
typedef enum pc_algo_distribution { PC_ALGO_DIST_UNIFORM, PC_ALGO_DIST_GAUSSIAN } pc_algo_distribution;
typedef enum pc_algo_undefined_areas {
    PC_ALGO_UNDEF_WRAP,
    PC_ALGO_UNDEF_REPEAT,
    PC_ALGO_UNDEF_TRANSPARENT
} pc_algo_undefined_areas;
typedef enum pc_algo_preserve { PC_ALGO_PRESERVE_SQUARENESS, PC_ALGO_PRESERVE_ROUNDNESS } pc_algo_preserve;
typedef enum pc_algo_spherize_mode {
    PC_ALGO_SPHERIZE_NORMAL,
    PC_ALGO_SPHERIZE_HORIZONTAL_ONLY,
    PC_ALGO_SPHERIZE_VERTICAL_ONLY
} pc_algo_spherize_mode;
typedef enum pc_algo_wave_type {
    PC_ALGO_WAVE_SINE,
    PC_ALGO_WAVE_TRIANGLE,
    PC_ALGO_WAVE_SQUARE
} pc_algo_wave_type;
typedef enum pc_algo_ripple_size {
    PC_ALGO_RIPPLE_SMALL,
    PC_ALGO_RIPPLE_MEDIUM,
    PC_ALGO_RIPPLE_LARGE
} pc_algo_ripple_size;
typedef enum pc_algo_polar_mode {
    PC_ALGO_POLAR_RECT_TO_POLAR,
    PC_ALGO_POLAR_POLAR_TO_RECT
} pc_algo_polar_mode;

/* params_ext.rs */
typedef enum pc_algo_mezzotint_type {
    PC_ALGO_MEZZO_FINE_DOTS,
    PC_ALGO_MEZZO_MEDIUM_DOTS,
    PC_ALGO_MEZZO_GRAINY_DOTS,
    PC_ALGO_MEZZO_COARSE_DOTS,
    PC_ALGO_MEZZO_SHORT_LINES,
    PC_ALGO_MEZZO_MEDIUM_LINES,
    PC_ALGO_MEZZO_LONG_LINES,
    PC_ALGO_MEZZO_SHORT_STROKES,
    PC_ALGO_MEZZO_MEDIUM_STROKES,
    PC_ALGO_MEZZO_LONG_STROKES
} pc_algo_mezzotint_type;
typedef enum pc_algo_diffuse_mode {
    PC_ALGO_DIFFUSE_NORMAL,
    PC_ALGO_DIFFUSE_DARKEN_ONLY,
    PC_ALGO_DIFFUSE_LIGHTEN_ONLY,
    PC_ALGO_DIFFUSE_ANISOTROPIC
} pc_algo_diffuse_mode;
typedef enum pc_algo_extrude_type { PC_ALGO_EXTRUDE_BLOCKS, PC_ALGO_EXTRUDE_PYRAMIDS } pc_algo_extrude_type;
typedef enum pc_algo_tile_fill {
    PC_ALGO_TILEFILL_BACKGROUND,
    PC_ALGO_TILEFILL_FOREGROUND,
    PC_ALGO_TILEFILL_INVERSE,
    PC_ALGO_TILEFILL_UNALTERED
} pc_algo_tile_fill;
typedef enum pc_algo_wind_method {
    PC_ALGO_WIND_WIND,
    PC_ALGO_WIND_BLAST,
    PC_ALGO_WIND_STAGGER
} pc_algo_wind_method;
typedef enum pc_algo_zigzag_style {
    PC_ALGO_ZIGZAG_AROUND_CENTER,
    PC_ALGO_ZIGZAG_OUT_FROM_CENTER,
    PC_ALGO_ZIGZAG_POND_RIPPLES
} pc_algo_zigzag_style;
typedef enum pc_algo_lens_type {
    PC_ALGO_LENS_ZOOM,
    PC_ALGO_LENS_PRIME35,
    PC_ALGO_LENS_PRIME105,
    PC_ALGO_LENS_MOVIE_PRIME
} pc_algo_lens_type;
typedef enum pc_algo_light_kind {
    PC_ALGO_LIGHT_SPOT,
    PC_ALGO_LIGHT_POINT,
    PC_ALGO_LIGHT_INFINITE
} pc_algo_light_kind;
typedef enum pc_algo_texture_channel {
    PC_ALGO_TEX_NONE,
    PC_ALGO_TEX_RED,
    PC_ALGO_TEX_GREEN,
    PC_ALGO_TEX_BLUE,
    PC_ALGO_TEX_ALPHA,
    PC_ALGO_TEX_LUMINANCE
} pc_algo_texture_channel;
typedef enum pc_algo_blur_quality {
    PC_ALGO_BLURQ_LOW,
    PC_ALGO_BLURQ_MEDIUM,
    PC_ALGO_BLURQ_HIGH
} pc_algo_blur_quality;
typedef enum pc_algo_smart_blur_mode {
    PC_ALGO_SMART_NORMAL,
    PC_ALGO_SMART_EDGE_ONLY,
    PC_ALGO_SMART_OVERLAY_EDGE
} pc_algo_smart_blur_mode;
typedef enum pc_algo_depth_source {
    PC_ALGO_DEPTH_NONE,
    PC_ALGO_DEPTH_TRANSPARENCY,
    PC_ALGO_DEPTH_LAYER_MASK
} pc_algo_depth_source;
typedef enum pc_algo_blur_shape {
    PC_ALGO_SHAPE_CIRCLE,
    PC_ALGO_SHAPE_RING,
    PC_ALGO_SHAPE_SQUARE,
    PC_ALGO_SHAPE_DIAMOND,
    PC_ALGO_SHAPE_TRIANGLE,
    PC_ALGO_SHAPE_HEXAGON,
    PC_ALGO_SHAPE_STAR,
    PC_ALGO_SHAPE_HEART,
    PC_ALGO_SHAPE_CROSS
} pc_algo_blur_shape;
typedef enum pc_algo_hsb_model { PC_ALGO_HSB_RGB, PC_ALGO_HSB_HSB, PC_ALGO_HSB_HSL } pc_algo_hsb_model;

/* resample.rs / transform.rs (declared here so every enum shares one descriptor table) */
typedef enum pc_algo_resample {
    PC_ALGO_RESAMPLE_NEAREST,
    PC_ALGO_RESAMPLE_BILINEAR,
    PC_ALGO_RESAMPLE_BICUBIC,
    PC_ALGO_RESAMPLE_LANCZOS,
    PC_ALGO_RESAMPLE_PRESERVE_DETAILS
} pc_algo_resample;
typedef enum pc_algo_interp {
    PC_ALGO_INTERP_NEAREST,
    PC_ALGO_INTERP_BILINEAR,
    PC_ALGO_INTERP_BICUBIC
} pc_algo_interp;

/* The JSON field code stores enums through int-sized slots. */
_Static_assert(sizeof(pc_algo_radial_method) == sizeof(int), "enums must be int-sized");
_Static_assert(sizeof(pc_algo_blur_shape) == sizeof(int), "enums must be int-sized");

extern const pc_algo_enum_desc PC_ALGO_ENUM_RADIAL_METHOD, PC_ALGO_ENUM_RADIAL_QUALITY,
    PC_ALGO_ENUM_DISTRIBUTION, PC_ALGO_ENUM_UNDEFINED_AREAS, PC_ALGO_ENUM_PRESERVE,
    PC_ALGO_ENUM_SPHERIZE_MODE, PC_ALGO_ENUM_WAVE_TYPE, PC_ALGO_ENUM_RIPPLE_SIZE, PC_ALGO_ENUM_POLAR_MODE,
    PC_ALGO_ENUM_MEZZOTINT_TYPE, PC_ALGO_ENUM_DIFFUSE_MODE, PC_ALGO_ENUM_EXTRUDE_TYPE, PC_ALGO_ENUM_TILE_FILL,
    PC_ALGO_ENUM_WIND_METHOD, PC_ALGO_ENUM_ZIGZAG_STYLE, PC_ALGO_ENUM_LENS_TYPE, PC_ALGO_ENUM_LIGHT_KIND,
    PC_ALGO_ENUM_TEXTURE_CHANNEL, PC_ALGO_ENUM_BLUR_QUALITY, PC_ALGO_ENUM_SMART_BLUR_MODE,
    PC_ALGO_ENUM_DEPTH_SOURCE, PC_ALGO_ENUM_BLUR_SHAPE, PC_ALGO_ENUM_HSB_MODEL, PC_ALGO_ENUM_RESAMPLE,
    PC_ALGO_ENUM_INTERP;

/* ---- parameter objects (params_ext.rs) ---- */

/* One Lighting Effects light (positions are fractions of the reference bounds). */
typedef struct pc_algo_light {
    pc_algo_light_kind kind;
    float x, y, z;
    float target_x, target_y;
    float angle, elevation;
    float color[3];
    float intensity, cone, hotspot, radius;
} pc_algo_light;

typedef struct pc_algo_iris_pin {
    float x, y, radius_x, radius_y, angle, roundness, feather, blur;
} pc_algo_iris_pin;

typedef struct pc_algo_field_pin {
    float x, y, blur;
} pc_algo_field_pin;

typedef struct pc_algo_spin_pin {
    float x, y, radius_x, radius_y, angle, blur_angle;
} pc_algo_spin_pin;

/* Owned vectors (v NULL when n == 0; free with the matching _fini or pc_algo_jfields_fini). */
typedef struct pc_algo_points {
    float (*v)[2];
    size_t n;
} pc_algo_points;

typedef struct pc_algo_floats {
    float *v;
    size_t n;
} pc_algo_floats;

typedef struct pc_algo_blur_path {
    pc_algo_points points;
    float speed, taper;
} pc_algo_blur_path;

typedef struct pc_algo_lights {
    pc_algo_light *v;
    size_t n;
} pc_algo_lights;
typedef struct pc_algo_iris_pins {
    pc_algo_iris_pin *v;
    size_t n;
} pc_algo_iris_pins;
typedef struct pc_algo_field_pins {
    pc_algo_field_pin *v;
    size_t n;
} pc_algo_field_pins;
typedef struct pc_algo_spin_pins {
    pc_algo_spin_pin *v;
    size_t n;
} pc_algo_spin_pins;
typedef struct pc_algo_blur_paths {
    pc_algo_blur_path *v;
    size_t n;
} pc_algo_blur_paths;

pc_algo_light pc_algo_light_default(void);
pc_algo_iris_pin pc_algo_iris_pin_default(void);
pc_algo_field_pin pc_algo_field_pin_default(void);
pc_algo_spin_pin pc_algo_spin_pin_default(void);
/* points [[0.2, 0.5], [0.8, 0.5]], speed 50, taper 0. */
pc_status pc_algo_blur_path_default(pc_algo_blur_path *out, pc_error *err);
void pc_algo_blur_path_fini(pc_algo_blur_path *p);

/* ---- descriptor-driven JSON ---- */

typedef enum pc_algo_jkind {
    PC_ALGO_JF_F32,
    PC_ALGO_JF_U32,
    PC_ALGO_JF_I32,
    PC_ALGO_JF_BOOL,
    PC_ALGO_JF_ENUM,       /* int-sized enum; `en` names it */
    PC_ALGO_JF_F32_ARRAY,  /* float[n] */
    PC_ALGO_JF_POINTS,     /* pc_algo_points (Vec<[f32; 2]>) */
    PC_ALGO_JF_F32_VEC,    /* pc_algo_floats (Vec<f32>) */
    PC_ALGO_JF_LIGHTS,     /* pc_algo_lights */
    PC_ALGO_JF_IRIS_PINS,  /* pc_algo_iris_pins */
    PC_ALGO_JF_FIELD_PINS, /* pc_algo_field_pins */
    PC_ALGO_JF_SPIN_PINS,  /* pc_algo_spin_pins */
    PC_ALGO_JF_BLUR_PATHS, /* pc_algo_blur_paths */
    PC_ALGO_JF_CUSTOM,     /* read / write / fini / copy callbacks below */
    PC_ALGO_JF_SKIP        /* #[serde(skip)]: never read or written (caller-supplied data) */
} pc_algo_jkind;

typedef struct pc_algo_jfield {
    const char *name; /* JSON key (camelCase) */
    pc_algo_jkind kind;
    size_t off;    /* offsetof the member in the struct */
    bool optional; /* #[serde(default)] (field or container): missing keeps the prefilled value */
    const pc_algo_enum_desc *en; /* PC_ALGO_JF_ENUM */
    size_t n;                    /* PC_ALGO_JF_F32_ARRAY length */
    /* PC_ALGO_JF_CUSTOM: read v into slot (zeroed or default on entry; on failure leave nothing
     * owned), write slot (NULL on out-of-memory), release, deep-copy (dst holds a shallow copy of
     * src on entry; required when fini is set, else copying fails with PC_ERR_UNSUPPORTED). */
    pc_status (*read)(yyjson_val *v, void *slot, pc_error *err);
    yyjson_mut_val *(*write)(yyjson_mut_doc *d, const void *slot);
    void (*fini)(void *slot);
    pc_status (*copy)(void *dst, const void *src, pc_error *err);
} pc_algo_jfield;

#define PC_ALGO_JF(nm, T, m, k) {(nm), (k), offsetof(T, m), false, NULL, 0, NULL, NULL, NULL, NULL}
#define PC_ALGO_JFD(nm, T, m, k) {(nm), (k), offsetof(T, m), true, NULL, 0, NULL, NULL, NULL, NULL}
#define PC_ALGO_JFE(nm, T, m, e, opt)                                                                        \
    {(nm), PC_ALGO_JF_ENUM, offsetof(T, m), (opt), &(e), 0, NULL, NULL, NULL, NULL}
#define PC_ALGO_JFA(nm, T, m, cnt, opt)                                                                      \
    {(nm), PC_ALGO_JF_F32_ARRAY, offsetof(T, m), (opt), NULL, (cnt), NULL, NULL, NULL, NULL}
#define PC_ALGO_JFSKIP(nm, T, m)                                                                             \
    {(nm), PC_ALGO_JF_SKIP, offsetof(T, m), true, NULL, 0, NULL, NULL, NULL, NULL}

/* Reads the fields of `obj` into base (prefilled with defaults / zero). `expecting` names the type
 * for "invalid type" errors ("struct variant FilterParams::GaussianBlur", "struct Light").
 * `ignore` is a key to skip silently (the enum tag), or NULL. On failure every owned field is
 * released and zeroed. */
pc_status pc_algo_jfields_read(const pc_algo_jfield *f, size_t n, yyjson_val *obj, void *base,
                               const char *expecting, const char *ignore, pc_error *err);
/* Adds the fields to obj in declaration order. False on out-of-memory. */
bool pc_algo_jfields_write(const pc_algo_jfield *f, size_t n, yyjson_mut_doc *d, yyjson_mut_val *obj,
                           const void *base);
/* Releases owned fields (vectors, custom) and zeroes them. */
void pc_algo_jfields_fini(const pc_algo_jfield *f, size_t n, void *base);
/* dst = deep copy of src (size bytes): plain members copied, owned fields duplicated. On failure dst
 * owns nothing. */
pc_status pc_algo_jfields_copy(const pc_algo_jfield *f, size_t n, void *dst, const void *src, size_t size,
                               pc_error *err);

/* serde's "invalid type" wording of a JSON value ("integer `1`", "string \"x\"", "null", ...). */
void pc_algo_junexpected(yyjson_val *v, char *out, size_t cap);
/* "invalid type: <v>, expected <exp>". */
pc_status pc_algo_jfail_type(yyjson_val *v, const char *exp, pc_error *err);
/* serde's "unknown variant `x`, expected ..." for d (or a raw name list). */
pc_status pc_algo_jfail_variant(const char *s, size_t len, const char *const *names, size_t n, pc_error *err);
/* JSON number -> f32 like serde (`as f32`); false (with serde's message) otherwise. */
pc_status pc_algo_jread_f32(yyjson_val *v, float *out, pc_error *err);

/* Single-struct JSON for the params_ext types (maps with #[serde(default)]). */
pc_status pc_algo_light_from_json(yyjson_val *v, pc_algo_light *out, pc_error *err);
yyjson_mut_val *pc_algo_light_to_json(yyjson_mut_doc *d, const pc_algo_light *l);

#endif
