/* Test-only filter defs for tests/algo: the parameter tables of a few reference FilterParams
 * variants (their real defs arrive with chunks L5/L6) and exact ports of the simplest kernels
 * (Invert, Solarize, Find Edges, Offset from stylize.rs / other.rs), so the L4 driver and JSON
 * machinery can be checked against the reference without the later chunks. Header-only. */
#ifndef PC_TESTS_ALGO_TESTDEFS_H
#define PC_TESTS_ALGO_TESTDEFS_H

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "base/rmath.h"

/* ---- parameter structs ---- */

typedef struct td_radius {
    float radius;
} td_radius;
typedef struct td_minmax {
    float radius;
    pc_algo_preserve preserve;
} td_minmax;
typedef struct td_radial {
    float amount;
    pc_algo_radial_method method;
    pc_algo_radial_quality quality;
    float center_x, center_y;
} td_radial;
typedef struct td_noise {
    float amount;
    pc_algo_distribution distribution;
    bool monochromatic;
    uint32_t seed;
} td_noise;
typedef struct td_offset {
    int32_t horizontal, vertical;
    pc_algo_undefined_areas undefined;
} td_offset;
typedef struct td_halftone {
    float max_radius;
    float angles[4];
} td_halftone;
typedef struct td_lighting {
    pc_algo_lights lights;
    float gloss, metallic, exposure, ambience;
    pc_algo_texture_channel texture;
    float height;
    bool white_is_high;
} td_lighting;
typedef struct td_shear {
    pc_algo_points points;
    pc_algo_undefined_areas undefined;
} td_shear;
typedef struct td_iris {
    pc_algo_iris_pins pins;
} td_iris;
typedef struct td_field {
    pc_algo_field_pins pins;
} td_field;
typedef struct td_spin {
    pc_algo_spin_pins pins;
} td_spin;
typedef struct td_path {
    pc_algo_blur_paths paths;
} td_path;
typedef struct td_custom {
    pc_algo_floats kernel;
    float scale, offset;
} td_custom;
typedef struct td_displace {
    float horizontal, vertical;
    bool stretch;
    pc_algo_undefined_areas undefined;
    const pc_algo_image *map;
} td_displace;
typedef struct td_motion {
    float angle, distance;
} td_motion;
typedef struct td_mosaic {
    float cell_size;
} td_mosaic;
typedef struct td_emboss {
    float angle, height, amount;
} td_emboss;
typedef struct td_twirl {
    float angle;
} td_twirl;

#define TD_N(a) (sizeof(a) / sizeof((a)[0]))

static const pc_algo_jfield TD_RADIUS_F[] = {PC_ALGO_JF("radius", td_radius, radius, PC_ALGO_JF_F32)};
static const pc_algo_jfield TD_MINMAX_F[] = {
    PC_ALGO_JF("radius", td_minmax, radius, PC_ALGO_JF_F32),
    PC_ALGO_JFE("preserve", td_minmax, preserve, PC_ALGO_ENUM_PRESERVE, true),
};
static const pc_algo_jfield TD_RADIAL_F[] = {
    PC_ALGO_JF("amount", td_radial, amount, PC_ALGO_JF_F32),
    PC_ALGO_JFE("method", td_radial, method, PC_ALGO_ENUM_RADIAL_METHOD, false),
    PC_ALGO_JFE("quality", td_radial, quality, PC_ALGO_ENUM_RADIAL_QUALITY, true),
    PC_ALGO_JF("centerX", td_radial, center_x, PC_ALGO_JF_F32),
    PC_ALGO_JF("centerY", td_radial, center_y, PC_ALGO_JF_F32),
};
static const pc_algo_jfield TD_NOISE_F[] = {
    PC_ALGO_JF("amount", td_noise, amount, PC_ALGO_JF_F32),
    PC_ALGO_JFE("distribution", td_noise, distribution, PC_ALGO_ENUM_DISTRIBUTION, false),
    PC_ALGO_JF("monochromatic", td_noise, monochromatic, PC_ALGO_JF_BOOL),
    PC_ALGO_JF("seed", td_noise, seed, PC_ALGO_JF_U32),
};
static const pc_algo_jfield TD_OFFSET_F[] = {
    PC_ALGO_JF("horizontal", td_offset, horizontal, PC_ALGO_JF_I32),
    PC_ALGO_JF("vertical", td_offset, vertical, PC_ALGO_JF_I32),
    PC_ALGO_JFE("undefined", td_offset, undefined, PC_ALGO_ENUM_UNDEFINED_AREAS, false),
};
static const pc_algo_jfield TD_HALFTONE_F[] = {
    PC_ALGO_JF("maxRadius", td_halftone, max_radius, PC_ALGO_JF_F32),
    PC_ALGO_JFA("angles", td_halftone, angles, 4, false),
};
static const pc_algo_jfield TD_LIGHTING_F[] = {
    PC_ALGO_JF("lights", td_lighting, lights, PC_ALGO_JF_LIGHTS),
    PC_ALGO_JF("gloss", td_lighting, gloss, PC_ALGO_JF_F32),
    PC_ALGO_JF("metallic", td_lighting, metallic, PC_ALGO_JF_F32),
    PC_ALGO_JF("exposure", td_lighting, exposure, PC_ALGO_JF_F32),
    PC_ALGO_JF("ambience", td_lighting, ambience, PC_ALGO_JF_F32),
    PC_ALGO_JFE("texture", td_lighting, texture, PC_ALGO_ENUM_TEXTURE_CHANNEL, false),
    PC_ALGO_JF("height", td_lighting, height, PC_ALGO_JF_F32),
    PC_ALGO_JF("whiteIsHigh", td_lighting, white_is_high, PC_ALGO_JF_BOOL),
};
static const pc_algo_jfield TD_SHEAR_F[] = {
    PC_ALGO_JF("points", td_shear, points, PC_ALGO_JF_POINTS),
    PC_ALGO_JFE("undefined", td_shear, undefined, PC_ALGO_ENUM_UNDEFINED_AREAS, false),
};
static const pc_algo_jfield TD_IRIS_F[] = {PC_ALGO_JF("pins", td_iris, pins, PC_ALGO_JF_IRIS_PINS)};
static const pc_algo_jfield TD_FIELD_F[] = {PC_ALGO_JF("pins", td_field, pins, PC_ALGO_JF_FIELD_PINS)};
static const pc_algo_jfield TD_SPIN_F[] = {PC_ALGO_JF("pins", td_spin, pins, PC_ALGO_JF_SPIN_PINS)};
static const pc_algo_jfield TD_PATH_F[] = {PC_ALGO_JF("paths", td_path, paths, PC_ALGO_JF_BLUR_PATHS)};
static const pc_algo_jfield TD_CUSTOM_F[] = {
    PC_ALGO_JF("kernel", td_custom, kernel, PC_ALGO_JF_F32_VEC),
    PC_ALGO_JF("scale", td_custom, scale, PC_ALGO_JF_F32),
    PC_ALGO_JF("offset", td_custom, offset, PC_ALGO_JF_F32),
};
static const pc_algo_jfield TD_DISPLACE_F[] = {
    PC_ALGO_JF("horizontal", td_displace, horizontal, PC_ALGO_JF_F32),
    PC_ALGO_JF("vertical", td_displace, vertical, PC_ALGO_JF_F32),
    PC_ALGO_JF("stretch", td_displace, stretch, PC_ALGO_JF_BOOL),
    PC_ALGO_JFE("undefined", td_displace, undefined, PC_ALGO_ENUM_UNDEFINED_AREAS, false),
    PC_ALGO_JFSKIP("map", td_displace, map),
};
static const pc_algo_jfield TD_MOTION_F[] = {
    PC_ALGO_JF("angle", td_motion, angle, PC_ALGO_JF_F32),
    PC_ALGO_JF("distance", td_motion, distance, PC_ALGO_JF_F32),
};
static const pc_algo_jfield TD_MOSAIC_F[] = {PC_ALGO_JF("cellSize", td_mosaic, cell_size, PC_ALGO_JF_F32)};
static const pc_algo_jfield TD_EMBOSS_F[] = {
    PC_ALGO_JF("angle", td_emboss, angle, PC_ALGO_JF_F32),
    PC_ALGO_JF("height", td_emboss, height, PC_ALGO_JF_F32),
    PC_ALGO_JF("amount", td_emboss, amount, PC_ALGO_JF_F32),
};
static const pc_algo_jfield TD_TWIRL_F[] = {PC_ALGO_JF("angle", td_twirl, angle, PC_ALGO_JF_F32)};

static void td_radial_init(void *p)
{
    ((td_radial *)p)->quality = PC_ALGO_RADIAL_GOOD;
}

/* ---- halos (lib.rs) ---- */

static int32_t td_i(float v)
{
    return pc_f32_to_i32(v);
}

static pc_algo_halo td_halo_gauss(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_radius(
        pc_i32_wrapping_add(td_i(ceilf(pc_rs_maxf_k(((const td_radius *)p)->radius, 0.0f) * 3.0f)), 1));
}
static pc_algo_halo td_halo_box(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_radius(
        pc_i32_wrapping_add(td_i(ceilf(pc_rs_maxf_k(((const td_radius *)p)->radius, 0.0f))), 1));
}
static pc_algo_halo td_halo_motion(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_radius(
        pc_i32_wrapping_add(td_i(ceilf(fabsf(((const td_motion *)p)->distance) / 2.0f)), 2));
}
static pc_algo_halo td_halo_mosaic(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_radius(
        pc_i32_wrapping_add(td_i(ceilf(pc_rs_maxf_k(((const td_mosaic *)p)->cell_size, 1.0f))), 1));
}
static pc_algo_halo td_halo_emboss(const void *p, const pc_rect *b)
{
    (void)b;
    return pc_algo_halo_radius(pc_i32_wrapping_add(td_i(ceilf(fabsf(((const td_emboss *)p)->height))), 2));
}
static pc_algo_halo td_halo_one(const void *p, const pc_rect *b)
{
    (void)p;
    (void)b;
    return pc_algo_halo_radius(1);
}
static pc_algo_halo td_halo_bounds(const void *p, const pc_rect *b)
{
    (void)p;
    (void)b;
    return pc_algo_halo_bounds();
}

/* ---- kernels (stylize.rs per_pixel / find_edges, other.rs offset) ---- */

static pc_status td_invert_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                             const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, c,
           npx = (size_t)pc_rect_width(out) * pc_rect_height(out);
    (void)p, (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < npx; i++) {
        for (c = 0; c < cc; c++) {
            dst[i * n + c] = 1.0f - dst[i * n + c];
        }
    }
    return PC_OK;
}

static pc_status td_solarize_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                               const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, c,
           npx = (size_t)pc_rect_width(out) * pc_rect_height(out);
    bool sub = pc_algo_subtractive(ctx);
    (void)p, (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < npx; i++) {
        for (c = 0; c < cc; c++) {
            float v = dst[i * n + c];
            dst[i * n + c] = sub ? (v < 0.5f ? 1.0f - v : v) : (v > 0.5f ? 1.0f - v : v);
        }
    }
    return PC_OK;
}

static pc_status td_find_edges_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                                 const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    size_t n = src->ch, cc = pc_algo_ncol(ctx, n), i, c,
           npx = (size_t)pc_rect_width(out) * pc_rect_height(out);
    bool sub = pc_algo_subtractive(ctx);
    (void)p, (void)prep, (void)err;
    pc_algo_image_crop_into(src, out, dst);
    for (i = 0; i < npx; i++) {
        int32_t x, y;
        pc_algo_xy(out, i, &x, &y);
        for (c = 0; c < cc; c++) {
#define G(dx, dy) pc_algo_image_get(src, x + (dx), y + (dy), c)
            float gx = (G(1, -1) + 2.0f * G(1, 0) + G(1, 1)) - (G(-1, -1) + 2.0f * G(-1, 0) + G(-1, 1));
            float gy = (G(-1, 1) + 2.0f * G(0, 1) + G(1, 1)) - (G(-1, -1) + 2.0f * G(0, -1) + G(1, -1));
#undef G
            float e = pc_clampf(sqrtf(gx * gx + gy * gy) / 4.0f, 0.0f, 1.0f);
            dst[i * n + c] = sub ? e : 1.0f - e;
        }
    }
    return PC_OK;
}

static pc_status td_offset_k(const void *p, const void *prep, const pc_algo_image *src, pc_rect out,
                             const pc_algo_ctx *ctx, float *dst, pc_error *err)
{
    const td_offset *o = p;
    pc_algo_edge e = o->undefined == PC_ALGO_UNDEF_WRAP     ? PC_ALGO_EDGE_WRAP
                     : o->undefined == PC_ALGO_UNDEF_REPEAT ? PC_ALGO_EDGE_REPEAT
                                                            : PC_ALGO_EDGE_TRANSPARENT;
    size_t k = 0, c;
    int32_t x, y;
    (void)prep, (void)err;
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            for (c = 0; c < src->ch; c++) {
                dst[k++] = pc_algo_image_get_edge(src, pc_i32_wrapping_sub(x, o->horizontal),
                                                  pc_i32_wrapping_sub(y, o->vertical), c, e, ctx->bounds);
            }
        }
    }
    return PC_OK;
}

#define TD_DEF(name, tg, T, F, init, halo, kern)                                                             \
    static const pc_algo_filter_def name = {(tg), sizeof(T), (F),  TD_N(F), (init), NULL, (halo),            \
                                            NULL, NULL,      NULL, (kern),  NULL,   0}
#define TD_UNIT(name, tg, halo, kern)                                                                        \
    static const pc_algo_filter_def name = {(tg), 0,    NULL, 0,      NULL, NULL, (halo),                    \
                                            NULL, NULL, NULL, (kern), NULL, 0}

TD_DEF(TD_GAUSS, "gaussianBlur", td_radius, TD_RADIUS_F, NULL, td_halo_gauss, NULL);
TD_DEF(TD_BOX, "boxBlur", td_radius, TD_RADIUS_F, NULL, td_halo_box, NULL);
TD_DEF(TD_MIN, "minimum", td_minmax, TD_MINMAX_F, NULL, td_halo_box, NULL);
TD_DEF(TD_MAX, "maximum", td_minmax, TD_MINMAX_F, NULL, td_halo_box, NULL);
TD_DEF(TD_RADIAL, "radialBlur", td_radial, TD_RADIAL_F, td_radial_init, td_halo_bounds, NULL);
TD_DEF(TD_NOISE, "addNoise", td_noise, TD_NOISE_F, NULL, NULL, NULL);
TD_DEF(TD_OFFSET, "offset", td_offset, TD_OFFSET_F, NULL, td_halo_bounds, td_offset_k);
TD_DEF(TD_HALFTONE, "colorHalftone", td_halftone, TD_HALFTONE_F, NULL, NULL, NULL);
TD_DEF(TD_LIGHTING, "lightingEffects", td_lighting, TD_LIGHTING_F, NULL, NULL, NULL);
TD_DEF(TD_SHEAR, "shear", td_shear, TD_SHEAR_F, NULL, td_halo_bounds, NULL);
TD_DEF(TD_IRIS, "irisBlur", td_iris, TD_IRIS_F, NULL, NULL, NULL);
TD_DEF(TD_FIELD, "fieldBlur", td_field, TD_FIELD_F, NULL, NULL, NULL);
TD_DEF(TD_SPIN, "spinBlur", td_spin, TD_SPIN_F, NULL, td_halo_bounds, NULL);
TD_DEF(TD_PATH, "pathBlur", td_path, TD_PATH_F, NULL, NULL, NULL);
TD_DEF(TD_CUSTOM, "custom", td_custom, TD_CUSTOM_F, NULL, NULL, NULL);
TD_DEF(TD_DISPLACE, "displace", td_displace, TD_DISPLACE_F, NULL, td_halo_bounds, NULL);
TD_DEF(TD_MOTION, "motionBlur", td_motion, TD_MOTION_F, NULL, td_halo_motion, NULL);
TD_DEF(TD_MOSAIC, "mosaic", td_mosaic, TD_MOSAIC_F, NULL, td_halo_mosaic, NULL);
TD_DEF(TD_EMBOSS, "emboss", td_emboss, TD_EMBOSS_F, NULL, td_halo_emboss, NULL);
TD_DEF(TD_TWIRL, "twirl", td_twirl, TD_TWIRL_F, NULL, td_halo_bounds, NULL);
TD_UNIT(TD_FIND_EDGES, "findEdges", td_halo_one, td_find_edges_k);
TD_UNIT(TD_INVERT, "invert", NULL, td_invert_k);
TD_UNIT(TD_SOLARIZE, "solarize", NULL, td_solarize_k);

static const pc_algo_filter_def *const TD_ALL[] = {
    &TD_GAUSS,    &TD_BOX,    &TD_MIN,    &TD_MAX,   &TD_RADIAL,     &TD_NOISE,  &TD_OFFSET,   &TD_HALFTONE,
    &TD_LIGHTING, &TD_SHEAR,  &TD_IRIS,   &TD_FIELD, &TD_SPIN,       &TD_PATH,   &TD_CUSTOM,   &TD_DISPLACE,
    &TD_MOTION,   &TD_MOSAIC, &TD_EMBOSS, &TD_TWIRL, &TD_FIND_EDGES, &TD_INVERT, &TD_SOLARIZE, NULL,
};

/* A filter of def with params (NULL for unit variants); aborts the test on failure via CHECK. */
static pc_algo_filter td_make(const pc_algo_filter_def *def, const void *params)
{
    pc_algo_filter f = {NULL, NULL};
    if (pc_algo_filter_make(def, params, &f, NULL) != PC_OK) {
        f.def = NULL;
    }
    return f;
}

#endif
