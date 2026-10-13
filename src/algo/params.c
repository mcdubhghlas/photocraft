/* Option enums, parameter objects and their serde-compatible JSON (see params.h). */
#include "algo/params.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "base/utf8.h"

/* ---- enum descriptors ---- */

#define NAMES(...) ((const char *const[]){__VA_ARGS__})
#define COUNT(...) (sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *))
#define ENUM_DESC(id, rust, def, ...)                                                                        \
    const pc_algo_enum_desc id = {(rust), NAMES(__VA_ARGS__), COUNT(__VA_ARGS__), (def)}

ENUM_DESC(PC_ALGO_ENUM_RADIAL_METHOD, "RadialMethod", 0, "spin", "zoom");
ENUM_DESC(PC_ALGO_ENUM_RADIAL_QUALITY, "RadialQuality", 1, "draft", "good", "best");
ENUM_DESC(PC_ALGO_ENUM_DISTRIBUTION, "Distribution", 0, "uniform", "gaussian");
ENUM_DESC(PC_ALGO_ENUM_UNDEFINED_AREAS, "UndefinedAreas", 0, "wrap", "repeat", "transparent");
ENUM_DESC(PC_ALGO_ENUM_PRESERVE, "Preserve", 0, "squareness", "roundness");
ENUM_DESC(PC_ALGO_ENUM_SPHERIZE_MODE, "SpherizeMode", 0, "normal", "horizontalOnly", "verticalOnly");
ENUM_DESC(PC_ALGO_ENUM_WAVE_TYPE, "WaveType", 0, "sine", "triangle", "square");
ENUM_DESC(PC_ALGO_ENUM_RIPPLE_SIZE, "RippleSize", 1, "small", "medium", "large");
ENUM_DESC(PC_ALGO_ENUM_POLAR_MODE, "PolarMode", 0, "rectangularToPolar", "polarToRectangular");
ENUM_DESC(PC_ALGO_ENUM_MEZZOTINT_TYPE, "MezzotintType", 0, "fineDots", "mediumDots", "grainyDots",
          "coarseDots", "shortLines", "mediumLines", "longLines", "shortStrokes", "mediumStrokes",
          "longStrokes");
ENUM_DESC(PC_ALGO_ENUM_DIFFUSE_MODE, "DiffuseMode", 0, "normal", "darkenOnly", "lightenOnly", "anisotropic");
ENUM_DESC(PC_ALGO_ENUM_EXTRUDE_TYPE, "ExtrudeType", 0, "blocks", "pyramids");
ENUM_DESC(PC_ALGO_ENUM_TILE_FILL, "TileFill", 0, "background", "foreground", "inverse", "unaltered");
ENUM_DESC(PC_ALGO_ENUM_WIND_METHOD, "WindMethod", 0, "wind", "blast", "stagger");
ENUM_DESC(PC_ALGO_ENUM_ZIGZAG_STYLE, "ZigZagStyle", 2, "aroundCenter", "outFromCenter", "pondRipples");
ENUM_DESC(PC_ALGO_ENUM_LENS_TYPE, "LensType", 0, "zoom", "prime35", "prime105", "moviePrime");
ENUM_DESC(PC_ALGO_ENUM_LIGHT_KIND, "LightKind", 0, "spot", "point", "infinite");
ENUM_DESC(PC_ALGO_ENUM_TEXTURE_CHANNEL, "TextureChannel", 0, "none", "red", "green", "blue", "alpha",
          "luminance");
ENUM_DESC(PC_ALGO_ENUM_BLUR_QUALITY, "BlurQuality", 1, "low", "medium", "high");
ENUM_DESC(PC_ALGO_ENUM_SMART_BLUR_MODE, "SmartBlurMode", 0, "normal", "edgeOnly", "overlayEdge");
ENUM_DESC(PC_ALGO_ENUM_DEPTH_SOURCE, "DepthSource", 0, "none", "transparency", "layerMask");
ENUM_DESC(PC_ALGO_ENUM_BLUR_SHAPE, "BlurShape", 0, "circle", "ring", "square", "diamond", "triangle",
          "hexagon", "star", "heart", "cross");
ENUM_DESC(PC_ALGO_ENUM_HSB_MODEL, "HsbModel", 0, "rgb", "hsb", "hsl");
ENUM_DESC(PC_ALGO_ENUM_RESAMPLE, "Resample", 2, "nearest", "bilinear", "bicubic", "bicubicSmoother",
          "bicubicSharper", "bicubicAutomatic", "lanczos", "preserveDetails");
ENUM_DESC(PC_ALGO_ENUM_INTERP, "Interp", 2, "nearest", "bilinear", "bicubic");

const char *pc_algo_enum_name(const pc_algo_enum_desc *d, int v)
{
    return v >= 0 && (size_t)v < d->n ? d->names[v] : "";
}

bool pc_algo_enum_from_name(const pc_algo_enum_desc *d, const char *s, size_t len, int *out)
{
    size_t i;
    for (i = 0; i < d->n; i++) {
        if (strlen(d->names[i]) == len && memcmp(d->names[i], s, len) == 0) {
            *out = (int)i;
            return true;
        }
    }
    return false;
}

/* ---- defaults ---- */

pc_algo_light pc_algo_light_default(void)
{
    pc_algo_light l;
    l.kind = PC_ALGO_LIGHT_SPOT;
    l.x = 0.25f;
    l.y = 0.2f;
    l.z = 0.6f;
    l.target_x = 0.5f;
    l.target_y = 0.55f;
    l.angle = 135.0f;
    l.elevation = 45.0f;
    l.color[0] = l.color[1] = l.color[2] = 1.0f;
    l.intensity = 75.0f;
    l.cone = 45.0f;
    l.hotspot = 50.0f;
    l.radius = 0.8f;
    return l;
}

pc_algo_iris_pin pc_algo_iris_pin_default(void)
{
    pc_algo_iris_pin p = {0.5f, 0.5f, 0.35f, 0.25f, 0.0f, 0.0f, 0.5f, 15.0f};
    return p;
}

pc_algo_field_pin pc_algo_field_pin_default(void)
{
    pc_algo_field_pin p = {0.5f, 0.5f, 15.0f};
    return p;
}

pc_algo_spin_pin pc_algo_spin_pin_default(void)
{
    pc_algo_spin_pin p = {0.5f, 0.5f, 0.3f, 0.3f, 0.0f, 15.0f};
    return p;
}

pc_status pc_algo_blur_path_default(pc_algo_blur_path *out, pc_error *err)
{
    float (*v)[2] = pc_malloc_array(2, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    v[0][0] = 0.2f;
    v[0][1] = 0.5f;
    v[1][0] = 0.8f;
    v[1][1] = 0.5f;
    out->points.v = v;
    out->points.n = 2;
    out->speed = 50.0f;
    out->taper = 0.0f;
    return PC_OK;
}

void pc_algo_blur_path_fini(pc_algo_blur_path *p)
{
    if (p != NULL) {
        free(p->points.v);
        p->points.v = NULL;
        p->points.n = 0;
    }
}

/* ---- messages ---- */

/* serde_json reads "-0" as the float -0.0; yyjson as the signed integer 0. */
static bool neg_zero_int(yyjson_val *v)
{
    return yyjson_is_sint(v) && yyjson_get_sint(v) == 0;
}

void pc_algo_junexpected(yyjson_val *v, char *out, size_t cap)
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

pc_status pc_algo_jfail_type(yyjson_val *v, const char *exp, pc_error *err)
{
    char u[PC_ERROR_MSG_MAX];
    pc_algo_junexpected(v, u, sizeof u);
    return pc_fail(err, PC_ERR_INVALID, "invalid type: %s, expected %s", u, exp);
}

pc_status pc_algo_jfail_variant(const char *s, size_t len, const char *const *names, size_t n, pc_error *err)
{
    char list[PC_ERROR_MSG_MAX * 4];
    size_t used = 0, i;
    int w;
    list[0] = '\0';
    if (n == 0) {
        w = snprintf(list, sizeof list, "there are no variants");
        used = w > 0 ? (size_t)w : 0;
    } else if (n == 1) {
        w = snprintf(list, sizeof list, "expected `%s`", names[0]);
        used = w > 0 ? (size_t)w : 0;
    } else if (n == 2) {
        w = snprintf(list, sizeof list, "expected `%s` or `%s`", names[0], names[1]);
        used = w > 0 ? (size_t)w : 0;
    } else {
        w = snprintf(list, sizeof list, "expected one of ");
        used = w > 0 ? (size_t)w : 0;
        for (i = 0; i < n && used < sizeof list; i++) {
            w = snprintf(list + used, sizeof list - used, "%s`%s`", i > 0 ? ", " : "", names[i]);
            if (w < 0) {
                break;
            }
            used += (size_t)w;
        }
    }
    (void)used;
    if (len > 200) {
        len = 200; /* the message is capped anyway; keep room for the list */
    }
    return pc_fail(err, PC_ERR_INVALID, "unknown variant `%.*s`, %s", (int)len, s, list);
}

pc_status pc_algo_jread_f32(yyjson_val *v, float *out, pc_error *err)
{
    if (neg_zero_int(v)) {
        *out = -0.0f;
    } else if (yyjson_is_uint(v)) {
        *out = (float)yyjson_get_uint(v);
    } else if (yyjson_is_sint(v)) {
        *out = (float)yyjson_get_sint(v);
    } else if (yyjson_is_real(v)) {
        *out = (float)yyjson_get_real(v);
    } else {
        return pc_algo_jfail_type(v, "f32", err);
    }
    return PC_OK;
}

static pc_status read_u32(yyjson_val *v, uint32_t *out, pc_error *err)
{
    if (neg_zero_int(v) || yyjson_is_real(v) || !yyjson_is_int(v)) {
        return pc_algo_jfail_type(v, "u32", err);
    }
    if (yyjson_is_uint(v)) {
        uint64_t u = yyjson_get_uint(v);
        if (u > UINT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%llu`, expected u32",
                           (unsigned long long)u);
        }
        *out = (uint32_t)u;
        return PC_OK;
    }
    {
        int64_t i = yyjson_get_sint(v);
        if (i < 0 || i > (int64_t)UINT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%lld`, expected u32", (long long)i);
        }
        *out = (uint32_t)i;
    }
    return PC_OK;
}

static pc_status read_i32(yyjson_val *v, int32_t *out, pc_error *err)
{
    if (neg_zero_int(v) || yyjson_is_real(v) || !yyjson_is_int(v)) {
        return pc_algo_jfail_type(v, "i32", err);
    }
    if (yyjson_is_uint(v)) {
        uint64_t u = yyjson_get_uint(v);
        if (u > INT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%llu`, expected i32",
                           (unsigned long long)u);
        }
        *out = (int32_t)u;
        return PC_OK;
    }
    {
        int64_t i = yyjson_get_sint(v);
        if (i < INT32_MIN || i > INT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: integer `%lld`, expected i32", (long long)i);
        }
        *out = (int32_t)i;
    }
    return PC_OK;
}

static pc_status read_enum(yyjson_val *v, const pc_algo_enum_desc *e, int *out, pc_error *err)
{
    yyjson_val *name = v, *payload = NULL;
    const char *s;
    size_t len;
    if (yyjson_is_obj(v)) {
        yyjson_obj_iter it;
        if (yyjson_obj_size(v) != 1) {
            return pc_fail(err, PC_ERR_INVALID, "invalid value: map, expected map with a single key");
        }
        yyjson_obj_iter_init(v, &it);
        name = yyjson_obj_iter_next(&it);
        payload = yyjson_obj_iter_get_val(name);
    } else if (!yyjson_is_str(v)) {
        return pc_algo_jfail_type(v, "string or map", err);
    }
    s = yyjson_get_str(name);
    len = yyjson_get_len(name);
    if (!pc_algo_enum_from_name(e, s, len, out)) {
        return pc_algo_jfail_variant(s, len, e->names, e->n, err);
    }
    if (payload != NULL && !yyjson_is_null(payload)) {
        return pc_algo_jfail_type(payload, "unit", err);
    }
    return PC_OK;
}

/* [f32; n] through serde's Content: exactly n numbers. */
static pc_status read_f32_array(yyjson_val *v, float *out, size_t n, pc_error *err)
{
    char exp[64];
    size_t i = 0, total;
    yyjson_arr_iter it;
    yyjson_val *e;
    pc_status st;
    (void)snprintf(exp, sizeof exp, "an array of length %zu", n);
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, exp, err);
    }
    total = yyjson_arr_size(v);
    yyjson_arr_iter_init(v, &it);
    while (i < n && (e = yyjson_arr_iter_next(&it)) != NULL) {
        st = pc_algo_jread_f32(e, &out[i], err);
        if (st != PC_OK) {
            return st;
        }
        i++;
    }
    if (i < n) {
        return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected %s", i, exp);
    }
    if (total > n) {
        return pc_fail(err, PC_ERR_INVALID, "invalid length %zu, expected %zu element%s in sequence", total,
                       n, n == 1 ? "" : "s");
    }
    return PC_OK;
}

/* ---- struct tables of the params_ext types ---- */

static const pc_algo_jfield LIGHT_FIELDS[] = {
    PC_ALGO_JFE("kind", pc_algo_light, kind, PC_ALGO_ENUM_LIGHT_KIND, true),
    PC_ALGO_JFD("x", pc_algo_light, x, PC_ALGO_JF_F32),
    PC_ALGO_JFD("y", pc_algo_light, y, PC_ALGO_JF_F32),
    PC_ALGO_JFD("z", pc_algo_light, z, PC_ALGO_JF_F32),
    PC_ALGO_JFD("targetX", pc_algo_light, target_x, PC_ALGO_JF_F32),
    PC_ALGO_JFD("targetY", pc_algo_light, target_y, PC_ALGO_JF_F32),
    PC_ALGO_JFD("angle", pc_algo_light, angle, PC_ALGO_JF_F32),
    PC_ALGO_JFD("elevation", pc_algo_light, elevation, PC_ALGO_JF_F32),
    PC_ALGO_JFA("color", pc_algo_light, color, 3, true),
    PC_ALGO_JFD("intensity", pc_algo_light, intensity, PC_ALGO_JF_F32),
    PC_ALGO_JFD("cone", pc_algo_light, cone, PC_ALGO_JF_F32),
    PC_ALGO_JFD("hotspot", pc_algo_light, hotspot, PC_ALGO_JF_F32),
    PC_ALGO_JFD("radius", pc_algo_light, radius, PC_ALGO_JF_F32),
};

static const pc_algo_jfield IRIS_FIELDS[] = {
    PC_ALGO_JFD("x", pc_algo_iris_pin, x, PC_ALGO_JF_F32),
    PC_ALGO_JFD("y", pc_algo_iris_pin, y, PC_ALGO_JF_F32),
    PC_ALGO_JFD("radiusX", pc_algo_iris_pin, radius_x, PC_ALGO_JF_F32),
    PC_ALGO_JFD("radiusY", pc_algo_iris_pin, radius_y, PC_ALGO_JF_F32),
    PC_ALGO_JFD("angle", pc_algo_iris_pin, angle, PC_ALGO_JF_F32),
    PC_ALGO_JFD("roundness", pc_algo_iris_pin, roundness, PC_ALGO_JF_F32),
    PC_ALGO_JFD("feather", pc_algo_iris_pin, feather, PC_ALGO_JF_F32),
    PC_ALGO_JFD("blur", pc_algo_iris_pin, blur, PC_ALGO_JF_F32),
};

static const pc_algo_jfield FIELD_FIELDS[] = {
    PC_ALGO_JFD("x", pc_algo_field_pin, x, PC_ALGO_JF_F32),
    PC_ALGO_JFD("y", pc_algo_field_pin, y, PC_ALGO_JF_F32),
    PC_ALGO_JFD("blur", pc_algo_field_pin, blur, PC_ALGO_JF_F32),
};

static const pc_algo_jfield SPIN_FIELDS[] = {
    PC_ALGO_JFD("x", pc_algo_spin_pin, x, PC_ALGO_JF_F32),
    PC_ALGO_JFD("y", pc_algo_spin_pin, y, PC_ALGO_JF_F32),
    PC_ALGO_JFD("radiusX", pc_algo_spin_pin, radius_x, PC_ALGO_JF_F32),
    PC_ALGO_JFD("radiusY", pc_algo_spin_pin, radius_y, PC_ALGO_JF_F32),
    PC_ALGO_JFD("angle", pc_algo_spin_pin, angle, PC_ALGO_JF_F32),
    PC_ALGO_JFD("blurAngle", pc_algo_spin_pin, blur_angle, PC_ALGO_JF_F32),
};

static const pc_algo_jfield PATH_FIELDS[] = {
    PC_ALGO_JFD("points", pc_algo_blur_path, points, PC_ALGO_JF_POINTS),
    PC_ALGO_JFD("speed", pc_algo_blur_path, speed, PC_ALGO_JF_F32),
    PC_ALGO_JFD("taper", pc_algo_blur_path, taper, PC_ALGO_JF_F32),
};

#define NF(a) (sizeof(a) / sizeof((a)[0]))

/* A vector of structs, generic over the element table. */
typedef struct vecinfo {
    const pc_algo_jfield *f;
    size_t nf;
    size_t elem;
    const char *expecting;
    pc_status (*init)(void *e, pc_error *err); /* the element's Default */
} vecinfo;

static pc_status light_init(void *e, pc_error *err)
{
    (void)err;
    *(pc_algo_light *)e = pc_algo_light_default();
    return PC_OK;
}
static pc_status iris_init(void *e, pc_error *err)
{
    (void)err;
    *(pc_algo_iris_pin *)e = pc_algo_iris_pin_default();
    return PC_OK;
}
static pc_status field_init(void *e, pc_error *err)
{
    (void)err;
    *(pc_algo_field_pin *)e = pc_algo_field_pin_default();
    return PC_OK;
}
static pc_status spin_init(void *e, pc_error *err)
{
    (void)err;
    *(pc_algo_spin_pin *)e = pc_algo_spin_pin_default();
    return PC_OK;
}
static pc_status path_init(void *e, pc_error *err)
{
    return pc_algo_blur_path_default((pc_algo_blur_path *)e, err);
}

static const vecinfo VEC_LIGHTS = {LIGHT_FIELDS, NF(LIGHT_FIELDS), sizeof(pc_algo_light), "struct Light",
                                   light_init};
static const vecinfo VEC_IRIS = {IRIS_FIELDS, NF(IRIS_FIELDS), sizeof(pc_algo_iris_pin), "struct IrisPin",
                                 iris_init};
static const vecinfo VEC_FIELD = {FIELD_FIELDS, NF(FIELD_FIELDS), sizeof(pc_algo_field_pin),
                                  "struct FieldPin", field_init};
static const vecinfo VEC_SPIN = {SPIN_FIELDS, NF(SPIN_FIELDS), sizeof(pc_algo_spin_pin), "struct SpinPin",
                                 spin_init};
static const vecinfo VEC_PATHS = {PATH_FIELDS, NF(PATH_FIELDS), sizeof(pc_algo_blur_path), "struct BlurPath",
                                  path_init};

static const vecinfo *vec_of(pc_algo_jkind k)
{
    switch (k) {
    case PC_ALGO_JF_LIGHTS:
        return &VEC_LIGHTS;
    case PC_ALGO_JF_IRIS_PINS:
        return &VEC_IRIS;
    case PC_ALGO_JF_FIELD_PINS:
        return &VEC_FIELD;
    case PC_ALGO_JF_SPIN_PINS:
        return &VEC_SPIN;
    case PC_ALGO_JF_BLUR_PATHS:
        return &VEC_PATHS;
    default:
        return NULL;
    }
}

/* Every vector member has the layout {T *v; size_t n;}. */
typedef struct anyvec {
    void *v;
    size_t n;
} anyvec;

static void vec_fini(const vecinfo *vi, anyvec *a)
{
    size_t i;
    if (a->v != NULL) {
        for (i = 0; i < a->n; i++) {
            pc_algo_jfields_fini(vi->f, vi->nf, (char *)a->v + i * vi->elem);
        }
    }
    free(a->v);
    a->v = NULL;
    a->n = 0;
}

/* One struct element: a map (serde's visit_map) or a sequence (visit_seq: fields in declaration
 * order; missing trailing ones keep the container default). */
static pc_status read_struct_elem(const vecinfo *vi, yyjson_val *v, void *e, pc_error *err)
{
    pc_status st = vi->init(e, err);
    if (st != PC_OK) {
        return st;
    }
    if (yyjson_is_obj(v)) {
        st = pc_algo_jfields_read(vi->f, vi->nf, v, e, vi->expecting, NULL, err);
        return st;
    }
    pc_algo_jfields_fini(vi->f, vi->nf, e);
    return pc_algo_jfail_type(v, vi->expecting, err);
}

static pc_status read_struct_vec(const vecinfo *vi, yyjson_val *v, anyvec *out, pc_error *err)
{
    size_t n, i = 0;
    char *buf;
    yyjson_arr_iter it;
    yyjson_val *e;
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "a sequence", err);
    }
    n = yyjson_arr_size(v);
    buf = n > 0 ? pc_calloc_array(n, vi->elem) : NULL;
    if (n > 0 && buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading a %zu-element list", n);
    }
    yyjson_arr_iter_init(v, &it);
    while (buf != NULL && i < n && (e = yyjson_arr_iter_next(&it)) != NULL) {
        pc_status st = read_struct_elem(vi, e, buf + i * vi->elem, err);
        if (st != PC_OK) {
            anyvec tmp = {buf, i};
            vec_fini(vi, &tmp);
            return st;
        }
        i++;
    }
    out->v = buf;
    out->n = n;
    return PC_OK;
}

static pc_status read_points(yyjson_val *v, pc_algo_points *out, pc_error *err)
{
    size_t n, i = 0;
    float (*buf)[2];
    yyjson_arr_iter it;
    yyjson_val *e;
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "a sequence", err);
    }
    n = yyjson_arr_size(v);
    buf = n > 0 ? pc_calloc_array(n, sizeof *buf) : NULL;
    if (n > 0 && buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading %zu points", n);
    }
    yyjson_arr_iter_init(v, &it);
    while (buf != NULL && i < n && (e = yyjson_arr_iter_next(&it)) != NULL) {
        pc_status st = read_f32_array(e, buf[i], 2, err);
        if (st != PC_OK) {
            free(buf);
            return st;
        }
        i++;
    }
    out->v = buf;
    out->n = n;
    return PC_OK;
}

static pc_status read_floats(yyjson_val *v, pc_algo_floats *out, pc_error *err)
{
    size_t n, i = 0;
    float *buf;
    yyjson_arr_iter it;
    yyjson_val *e;
    if (!yyjson_is_arr(v)) {
        return pc_algo_jfail_type(v, "a sequence", err);
    }
    n = yyjson_arr_size(v);
    buf = n > 0 ? pc_malloc_array(n, sizeof *buf) : NULL;
    if (n > 0 && buf == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory reading %zu numbers", n);
    }
    yyjson_arr_iter_init(v, &it);
    while (buf != NULL && i < n && (e = yyjson_arr_iter_next(&it)) != NULL) {
        pc_status st = pc_algo_jread_f32(e, &buf[i], err);
        if (st != PC_OK) {
            free(buf);
            return st;
        }
        i++;
    }
    out->v = buf;
    out->n = n;
    return PC_OK;
}

static void field_fini(const pc_algo_jfield *f, void *slot)
{
    const vecinfo *vi = vec_of(f->kind);
    if (vi != NULL) {
        vec_fini(vi, (anyvec *)slot);
    } else if (f->kind == PC_ALGO_JF_POINTS) {
        pc_algo_points *p = slot;
        free(p->v);
        p->v = NULL;
        p->n = 0;
    } else if (f->kind == PC_ALGO_JF_F32_VEC) {
        pc_algo_floats *p = slot;
        free(p->v);
        p->v = NULL;
        p->n = 0;
    } else if (f->kind == PC_ALGO_JF_CUSTOM && f->fini != NULL) {
        f->fini(slot);
    }
}

static bool owned(const pc_algo_jfield *f)
{
    return vec_of(f->kind) != NULL || f->kind == PC_ALGO_JF_POINTS || f->kind == PC_ALGO_JF_F32_VEC ||
           (f->kind == PC_ALGO_JF_CUSTOM && f->fini != NULL);
}

static pc_status read_field(const pc_algo_jfield *f, yyjson_val *v, void *slot, pc_error *err)
{
    switch (f->kind) {
    case PC_ALGO_JF_F32:
        return pc_algo_jread_f32(v, slot, err);
    case PC_ALGO_JF_U32:
        return read_u32(v, slot, err);
    case PC_ALGO_JF_I32:
        return read_i32(v, slot, err);
    case PC_ALGO_JF_BOOL:
        if (!yyjson_is_bool(v)) {
            return pc_algo_jfail_type(v, "a boolean", err);
        }
        *(bool *)slot = yyjson_get_bool(v);
        return PC_OK;
    case PC_ALGO_JF_ENUM: {
        int e = 0;
        pc_status st = read_enum(v, f->en, &e, err);
        if (st == PC_OK) {
            memcpy(slot, &e, sizeof e);
        }
        return st;
    }
    case PC_ALGO_JF_F32_ARRAY: {
        float tmp[16];
        pc_status st;
        if (f->n > 16) {
            return pc_fail(err, PC_ERR_INVALID, "array field `%s` is too long", f->name);
        }
        st = read_f32_array(v, tmp, f->n, err);
        if (st == PC_OK) {
            memcpy(slot, tmp, f->n * sizeof(float));
        }
        return st;
    }
    case PC_ALGO_JF_POINTS: {
        pc_algo_points p = {NULL, 0};
        pc_status st = read_points(v, &p, err);
        if (st == PC_OK) {
            field_fini(f, slot);
            *(pc_algo_points *)slot = p;
        }
        return st;
    }
    case PC_ALGO_JF_F32_VEC: {
        pc_algo_floats p = {NULL, 0};
        pc_status st = read_floats(v, &p, err);
        if (st == PC_OK) {
            field_fini(f, slot);
            *(pc_algo_floats *)slot = p;
        }
        return st;
    }
    case PC_ALGO_JF_LIGHTS:
    case PC_ALGO_JF_IRIS_PINS:
    case PC_ALGO_JF_FIELD_PINS:
    case PC_ALGO_JF_SPIN_PINS:
    case PC_ALGO_JF_BLUR_PATHS: {
        anyvec a = {NULL, 0};
        pc_status st = read_struct_vec(vec_of(f->kind), v, &a, err);
        if (st == PC_OK) {
            field_fini(f, slot);
            *(anyvec *)slot = a;
        }
        return st;
    }
    case PC_ALGO_JF_CUSTOM:
        if (f->read == NULL) {
            return pc_fail(err, PC_ERR_INVALID, "field `%s` has no reader", f->name);
        }
        return f->read(v, slot, err);
    case PC_ALGO_JF_SKIP:
        return PC_OK;
    }
    return pc_fail(err, PC_ERR_INVALID, "bad field kind");
}

/* One object entry, for visiting keys the way serde_json::Value holds them. */
typedef struct jentry {
    const char *k;
    size_t klen;
    size_t idx;
    yyjson_val *v;
} jentry;

static int cmp_entry(const void *a, const void *b)
{
    const jentry *x = a, *y = b;
    size_t n = x->klen < y->klen ? x->klen : y->klen;
    int c = n > 0 ? memcmp(x->k, y->k, n) : 0;
    if (c != 0) {
        return c;
    }
    if (x->klen != y->klen) {
        return x->klen < y->klen ? -1 : 1;
    }
    return x->idx < y->idx ? -1 : (x->idx > y->idx ? 1 : 0);
}

pc_status pc_algo_jfields_read(const pc_algo_jfield *f, size_t n, yyjson_val *obj, void *base,
                               const char *expecting, const char *ignore, pc_error *err)
{
    unsigned char seen[64];
    yyjson_obj_iter it;
    yyjson_val *key;
    jentry *ents = NULL;
    size_t i, ne = 0, total;
    pc_status st = PC_OK;
    if (n > sizeof seen) {
        return pc_fail(err, PC_ERR_INVALID, "too many fields in %s", expecting);
    }
    if (!yyjson_is_obj(obj)) {
        st = pc_algo_jfail_type(obj, expecting, err);
        pc_algo_jfields_fini(f, n, base);
        return st;
    }
    /* serde_json::Value is a BTreeMap: keys are visited in byte order and a repeated key keeps its
     * last value (the reference deserialises FilterParams from a Value). */
    total = yyjson_obj_size(obj);
    if (total > 0) {
        ents = pc_malloc_array(total, sizeof *ents);
        if (ents == NULL) {
            pc_algo_jfields_fini(f, n, base);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory reading %s", expecting);
        }
    }
    yyjson_obj_iter_init(obj, &it);
    while ((key = yyjson_obj_iter_next(&it)) != NULL && ne < total) {
        ents[ne].k = yyjson_get_str(key);
        ents[ne].klen = yyjson_get_len(key);
        ents[ne].idx = ne;
        ents[ne].v = yyjson_obj_iter_get_val(key);
        ne++;
    }
    if (ne > 1) {
        qsort(ents, ne, sizeof *ents, cmp_entry);
    }
    memset(seen, 0, sizeof seen);
    for (size_t e = 0; e < ne; e++) {
        const jentry *en = &ents[e];
        if (e + 1 < ne && ents[e + 1].klen == en->klen && memcmp(ents[e + 1].k, en->k, en->klen) == 0) {
            continue; /* a later duplicate wins */
        }
        if (ignore != NULL && strlen(ignore) == en->klen && memcmp(ignore, en->k, en->klen) == 0) {
            continue;
        }
        for (i = 0; i < n; i++) {
            if (f[i].kind != PC_ALGO_JF_SKIP && strlen(f[i].name) == en->klen &&
                memcmp(f[i].name, en->k, en->klen) == 0) {
                break;
            }
        }
        if (i == n) {
            continue; /* serde ignores unknown fields */
        }
        seen[i] = 1;
        st = read_field(&f[i], en->v, (char *)base + f[i].off, err);
        if (st != PC_OK) {
            goto fail;
        }
    }
    for (i = 0; i < n; i++) {
        if (!seen[i] && !f[i].optional && f[i].kind != PC_ALGO_JF_SKIP) {
            st = pc_fail(err, PC_ERR_INVALID, "missing field `%s`", f[i].name);
            goto fail;
        }
    }
    free(ents);
    return PC_OK;
fail:
    free(ents);
    pc_algo_jfields_fini(f, n, base);
    return st;
}

static yyjson_mut_val *w_f32(yyjson_mut_doc *d, float v)
{
    return isfinite(v) ? yyjson_mut_float(d, v) : yyjson_mut_null(d);
}

static yyjson_mut_val *w_f32s(yyjson_mut_doc *d, const float *v, size_t n)
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    size_t i;
    if (a == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        yyjson_mut_val *e = w_f32(d, v[i]);
        if (e == NULL || !yyjson_mut_arr_append(a, e)) {
            return NULL;
        }
    }
    return a;
}

static yyjson_mut_val *write_field(const pc_algo_jfield *f, yyjson_mut_doc *d, const void *slot)
{
    switch (f->kind) {
    case PC_ALGO_JF_F32:
        return w_f32(d, *(const float *)slot);
    case PC_ALGO_JF_U32:
        return yyjson_mut_uint(d, *(const uint32_t *)slot);
    case PC_ALGO_JF_I32:
        return yyjson_mut_sint(d, *(const int32_t *)slot);
    case PC_ALGO_JF_BOOL:
        return yyjson_mut_bool(d, *(const bool *)slot);
    case PC_ALGO_JF_ENUM: {
        int e;
        memcpy(&e, slot, sizeof e);
        return yyjson_mut_str(d, pc_algo_enum_name(f->en, e));
    }
    case PC_ALGO_JF_F32_ARRAY:
        return w_f32s(d, slot, f->n);
    case PC_ALGO_JF_POINTS: {
        const pc_algo_points *p = slot;
        yyjson_mut_val *a = yyjson_mut_arr(d);
        size_t i;
        if (a == NULL) {
            return NULL;
        }
        for (i = 0; i < p->n; i++) {
            yyjson_mut_val *e = w_f32s(d, p->v[i], 2);
            if (e == NULL || !yyjson_mut_arr_append(a, e)) {
                return NULL;
            }
        }
        return a;
    }
    case PC_ALGO_JF_F32_VEC: {
        const pc_algo_floats *p = slot;
        return w_f32s(d, p->v, p->n);
    }
    case PC_ALGO_JF_LIGHTS:
    case PC_ALGO_JF_IRIS_PINS:
    case PC_ALGO_JF_FIELD_PINS:
    case PC_ALGO_JF_SPIN_PINS:
    case PC_ALGO_JF_BLUR_PATHS: {
        const vecinfo *vi = vec_of(f->kind);
        const anyvec *a = slot;
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        size_t i;
        if (arr == NULL) {
            return NULL;
        }
        for (i = 0; i < a->n; i++) {
            yyjson_mut_val *o = yyjson_mut_obj(d);
            if (o == NULL || !pc_algo_jfields_write(vi->f, vi->nf, d, o, (const char *)a->v + i * vi->elem) ||
                !yyjson_mut_arr_append(arr, o)) {
                return NULL;
            }
        }
        return arr;
    }
    case PC_ALGO_JF_CUSTOM:
        return f->write != NULL ? f->write(d, slot) : NULL;
    case PC_ALGO_JF_SKIP:
        return NULL;
    }
    return NULL;
}

bool pc_algo_jfields_write(const pc_algo_jfield *f, size_t n, yyjson_mut_doc *d, yyjson_mut_val *obj,
                           const void *base)
{
    size_t i;
    for (i = 0; i < n; i++) {
        yyjson_mut_val *v;
        if (f[i].kind == PC_ALGO_JF_SKIP) {
            continue;
        }
        v = write_field(&f[i], d, (const char *)base + f[i].off);
        if (v == NULL || !yyjson_mut_obj_add(obj, yyjson_mut_str(d, f[i].name), v)) {
            return false;
        }
    }
    return true;
}

void pc_algo_jfields_fini(const pc_algo_jfield *f, size_t n, void *base)
{
    size_t i;
    if (base == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        if (owned(&f[i])) {
            field_fini(&f[i], (char *)base + f[i].off);
        }
    }
}

static void *memdup_n(const void *src, size_t n, size_t elem)
{
    void *d;
    if (n == 0 || src == NULL) {
        return NULL;
    }
    d = pc_malloc_array(n, elem);
    if (d != NULL) {
        memcpy(d, src, n * elem);
    }
    return d;
}

static pc_status copy_field(const pc_algo_jfield *f, void *dst, const void *src, pc_error *err)
{
    const vecinfo *vi = vec_of(f->kind);
    if (vi != NULL) {
        const anyvec *s = src;
        anyvec *o = dst;
        size_t i;
        o->v = NULL;
        o->n = 0;
        if (s->n == 0 || s->v == NULL) {
            return PC_OK;
        }
        o->v = pc_calloc_array(s->n, vi->elem);
        if (o->v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory copying parameters");
        }
        for (i = 0; i < s->n; i++) {
            pc_status st = pc_algo_jfields_copy(vi->f, vi->nf, (char *)o->v + i * vi->elem,
                                                (const char *)s->v + i * vi->elem, vi->elem, err);
            if (st != PC_OK) {
                o->n = i;
                vec_fini(vi, o);
                return st;
            }
        }
        o->n = s->n;
        return PC_OK;
    }
    if (f->kind == PC_ALGO_JF_POINTS) {
        const pc_algo_points *s = src;
        pc_algo_points *o = dst;
        o->v = memdup_n(s->v, s->n, sizeof *s->v);
        o->n = o->v != NULL ? s->n : 0;
        return o->n == s->n || s->v == NULL ? PC_OK
                                            : pc_fail(err, PC_ERR_NOMEM, "out of memory copying parameters");
    }
    if (f->kind == PC_ALGO_JF_F32_VEC) {
        const pc_algo_floats *s = src;
        pc_algo_floats *o = dst;
        o->v = memdup_n(s->v, s->n, sizeof *s->v);
        o->n = o->v != NULL ? s->n : 0;
        return o->n == s->n || s->v == NULL ? PC_OK
                                            : pc_fail(err, PC_ERR_NOMEM, "out of memory copying parameters");
    }
    if (f->kind == PC_ALGO_JF_CUSTOM && f->copy != NULL) {
        return f->copy(dst, src, err);
    }
    if (f->kind == PC_ALGO_JF_CUSTOM && f->fini != NULL) {
        /* Owned data without a copier would be shared and freed twice. */
        return pc_fail(err, PC_ERR_UNSUPPORTED, "field `%s` cannot be copied", f->name);
    }
    return PC_OK;
}

static size_t field_size(const pc_algo_jfield *f)
{
    if (vec_of(f->kind) != NULL) {
        return sizeof(anyvec);
    }
    if (f->kind == PC_ALGO_JF_POINTS) {
        return sizeof(pc_algo_points);
    }
    return sizeof(pc_algo_floats);
}

pc_status pc_algo_jfields_copy(const pc_algo_jfield *f, size_t n, void *dst, const void *src, size_t size,
                               pc_error *err)
{
    size_t i;
    memcpy(dst, src, size);
    /* Owned members must not alias src while the copies are made (a failure frees dst's). */
    for (i = 0; i < n; i++) {
        if (owned(&f[i]) && !(f[i].kind == PC_ALGO_JF_CUSTOM)) {
            memset((char *)dst + f[i].off, 0, field_size(&f[i]));
        }
    }
    for (i = 0; i < n; i++) {
        if (owned(&f[i]) || f[i].kind == PC_ALGO_JF_CUSTOM) {
            pc_status st = copy_field(&f[i], (char *)dst + f[i].off, (const char *)src + f[i].off, err);
            if (st != PC_OK) {
                size_t j;
                for (j = 0; j < i; j++) {
                    if (owned(&f[j])) {
                        field_fini(&f[j], (char *)dst + f[j].off);
                    }
                }
                return st;
            }
        }
    }
    return PC_OK;
}

pc_status pc_algo_light_from_json(yyjson_val *v, pc_algo_light *out, pc_error *err)
{
    pc_algo_light l;
    pc_status st = read_struct_elem(&VEC_LIGHTS, v, &l, err);
    if (st == PC_OK) {
        *out = l;
    }
    return st;
}

yyjson_mut_val *pc_algo_light_to_json(yyjson_mut_doc *d, const pc_algo_light *l)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (o == NULL || !pc_algo_jfields_write(LIGHT_FIELDS, NF(LIGHT_FIELDS), d, o, l)) {
        return NULL;
    }
    return o;
}
