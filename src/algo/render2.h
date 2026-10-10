/* Procedural renderers of Filter > Render: Flame, Picture Frame and Tree (port of
 * crates/algo/src/render2.rs).
 *
 * Each renderer turns its options into a list of anti-aliased vector primitives (tapered
 * capsules and shaded ellipses, blended normally, additively or by lighten), and
 * pc_algo_render_composite rasterizes the list over a surface tile by tile (in parallel on the
 * global pool), in straight sRGB converted to and from the surface's own colour model and depth,
 * mixed by a selection. The shapes are our own designs (recursive branching trees, turbulent
 * flame strands, ornaments walked along the frame edge).
 *
 * Primitive lists are owned (pc_algo_prims_fini). Renderers refuse absurd sizes that would
 * produce more than PC_ALGO_MAX_PRIMS primitives with PC_ERR_LIMIT (the reference would run out of
 * memory). Thread safety: pure functions; composite writes only `surface`. */
#ifndef PC_ALGO_RENDER2_H
#define PC_ALGO_RENDER2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "algo/params.h"
#include "base/status.h"
#include "geom/geom.h"
#include "raster/surface.h"

#define PC_ALGO_MAX_PRIMS ((size_t)1 << 24)

typedef enum pc_algo_prim_shape { PC_ALGO_PRIM_CAPSULE, PC_ALGO_PRIM_ELLIPSE } pc_algo_prim_shape;

/* How a primitive combines with what is under it. */
typedef enum pc_algo_prim_blend {
    PC_ALGO_BLEND_NORMAL, /* source over */
    PC_ALGO_BLEND_ADD,    /* additive light (glows) */
    PC_ALGO_BLEND_LIGHTEN /* per-channel maximum of premultiplied values */
} pc_algo_prim_blend;

/* One drawable primitive (document pixels). Capsule: a segment a -> b whose radius goes ra -> rb,
 * colour c0 -> c1 along it. Ellipse: centre c, radii, rotation `angle` (radians), colour c0
 * (centre) -> c1 (rim). Colours are straight sRGB RGBA 0..1; soft is the edge softness in px. */
typedef struct pc_algo_prim {
    pc_algo_prim_shape shape;
    float a[2], b[2], ra, rb;
    float c[2], rx, ry, angle;
    float c0[4], c1[4];
    float soft;
    pc_algo_prim_blend blend;
} pc_algo_prim;

typedef struct pc_algo_prims {
    pc_algo_prim *v;
    size_t n, cap;
} pc_algo_prims;

void pc_algo_prims_fini(pc_algo_prims *p); /* NULL-safe */
pc_algo_prim pc_algo_prim_capsule(const float a[2], const float b[2], float ra, float rb, const float c0[4],
                                  const float c1[4]);
pc_algo_prim pc_algo_prim_ellipse(const float c[2], float rx, float ry, float angle, const float c0[4],
                                  const float c1[4]);
pc_algo_prim pc_algo_prim_disc(const float c[2], float r, const float col[4]);
/* Pixel bounds a primitive can touch (empty when its geometry is not finite). */
pc_rect pc_algo_prim_bounds(const pc_algo_prim *p);

/* Draws prims over surface inside clip, mixed by channel 0 of selection (may be NULL). *used
 * receives the bounds of what was drawn. */
pc_status pc_algo_render_composite(pc_surface *surface, const pc_algo_prim *prims, size_t n, pc_rect clip,
                                   const pc_surface *selection, pc_rect *used, pc_error *err);

/* ---- randomness (SplitMix64) ---- */

typedef struct pc_algo_rng {
    uint64_t s;
} pc_algo_rng;

pc_algo_rng pc_algo_rng_new(uint32_t seed, uint64_t salt);
uint64_t pc_algo_rng_next(pc_algo_rng *r);
float pc_algo_rng_f(pc_algo_rng *r);                       /* [0, 1) */
float pc_algo_rng_range(pc_algo_rng *r, float a, float b); /* [a, b) */
float pc_algo_rng_sym(pc_algo_rng *r);                     /* [-1, 1) */

/* ---- Flame ---- */

typedef enum pc_algo_flame_type {
    PC_ALGO_FLAME_ONE_ALONG_PATH,
    PC_ALGO_FLAME_MULTIPLE_ALONG_PATH,
    PC_ALGO_FLAME_MULTIPLE_PATH_DIRECTIONS,
    PC_ALGO_FLAME_MULTIPLE_VARIOUS_LENGTH,
    PC_ALGO_FLAME_CANDLE_LIGHT,
    PC_ALGO_FLAME_MULTIPLE_ONE_DIRECTION
} pc_algo_flame_type;
typedef enum pc_algo_flame_style {
    PC_ALGO_FLAME_STYLE_NORMAL,
    PC_ALGO_FLAME_STYLE_VIOLENT,
    PC_ALGO_FLAME_STYLE_FLAT
} pc_algo_flame_style;
typedef enum pc_algo_flame_shape {
    PC_ALGO_FLAME_SHAPE_PARALLEL,
    PC_ALGO_FLAME_SHAPE_TO_CENTER,
    PC_ALGO_FLAME_SHAPE_SPREAD,
    PC_ALGO_FLAME_SHAPE_OVAL,
    PC_ALGO_FLAME_SHAPE_POINTED
} pc_algo_flame_shape;

extern const pc_algo_enum_desc PC_ALGO_ENUM_FLAME_TYPE, PC_ALGO_ENUM_FLAME_STYLE, PC_ALGO_ENUM_FLAME_SHAPE;

/* Flame options (Photoshop's Flame dialog, Basic + Advanced). */
typedef struct pc_algo_flame_spec {
    pc_algo_flame_type flame_type;
    float length; /* px 1..1000 */
    bool randomize_length;
    float width;    /* px 1..1000 */
    float angle;    /* degrees, 0 = up, clockwise (multipleFlamesOneDirection) */
    float interval; /* px 1..1000 */
    bool adjust_interval_for_loops;
    bool has_color; /* false = natural flame colours */
    float color[4];
    float turbulent, jag, opacity, flame_lines, flame_bottom_alignment;
    pc_algo_flame_style flame_style;
    pc_algo_flame_shape flame_shape;
    bool randomize_shapes;
    uint32_t seed;
    uint32_t quality; /* 0 (draft) .. 4 (fine) */
} pc_algo_flame_spec;

/* A flattened polyline (document pixels). */
typedef struct pc_algo_polyline {
    float (*v)[2];
    size_t n;
} pc_algo_polyline;

pc_algo_flame_spec pc_algo_flame_spec_default(void);
/* Builds flame primitives along the paths into *out (initialised on success). */
pc_status pc_algo_flame(const pc_algo_flame_spec *spec, const pc_algo_polyline *paths, size_t npaths,
                        pc_algo_prims *out, pc_error *err);
/* The path Flame uses when the document has none: a gentle arc across the lower canvas (33
 * points written to out). */
void pc_algo_default_flame_path(pc_rect canvas, float out[33][2]);

/* ---- Tree ---- */

#define PC_ALGO_TREE_TYPES 34
/* Names of the base tree types (baseTreeType = index + 1). */
extern const char *const pc_algo_tree_type_names[PC_ALGO_TREE_TYPES];

typedef struct pc_algo_tree_spec {
    uint32_t base_tree_type;  /* 1..34 */
    uint32_t light_direction; /* 1 (left) .. 3 (above) .. 5 (right) */
    float leaves_amount, leaves_size, branches_height, branches_thickness;
    bool default_leaves;
    float leaves_color[4];
    bool custom_branch_color;
    float branches_color[4];
    bool flat_shading;
    uint32_t seed;
    float x, y, size; /* base position and height as fractions of the canvas */
} pc_algo_tree_spec;

pc_algo_tree_spec pc_algo_tree_spec_default(void);
pc_status pc_algo_tree(const pc_algo_tree_spec *spec, pc_rect canvas, pc_algo_prims *out, pc_error *err);

/* ---- Picture Frame ---- */

#define PC_ALGO_FRAME_STYLES 16
extern const char *const pc_algo_frame_styles[PC_ALGO_FRAME_STYLES];

typedef struct pc_algo_frame_spec {
    uint32_t frame; /* 1-based design */
    float margin, size, arrangement;
    uint32_t lines;
    float thickness, fade;
    float vine_color[4], flower_color[4], leaf_color[4];
    uint32_t seed;
} pc_algo_frame_spec;

pc_algo_frame_spec pc_algo_frame_spec_default(void);
pc_status pc_algo_picture_frame(const pc_algo_frame_spec *spec, pc_rect canvas, pc_algo_prims *out,
                                pc_error *err);

/* ---- spec JSON (serde with container defaults: missing fields keep the default) ---- */

pc_status pc_algo_flame_spec_from_json(yyjson_val *v, pc_algo_flame_spec *out, pc_error *err);
pc_status pc_algo_tree_spec_from_json(yyjson_val *v, pc_algo_tree_spec *out, pc_error *err);
pc_status pc_algo_frame_spec_from_json(yyjson_val *v, pc_algo_frame_spec *out, pc_error *err);
yyjson_mut_val *pc_algo_flame_spec_to_json(yyjson_mut_doc *d, const pc_algo_flame_spec *s);
yyjson_mut_val *pc_algo_tree_spec_to_json(yyjson_mut_doc *d, const pc_algo_tree_spec *s);
yyjson_mut_val *pc_algo_frame_spec_to_json(yyjson_mut_doc *d, const pc_algo_frame_spec *s);

/* Internal (render2_shapes.c / render2.c): appends a primitive, PC_ERR_LIMIT past
 * PC_ALGO_MAX_PRIMS. */
pc_status pc_algo_prims_push(pc_algo_prims *p, pc_algo_prim prim, pc_error *err);
/* Internal: f32::max-style colour shading [c * k clamped, 1]. */
void pc_algo_render2_shade(const float c[3], float k, float out[4]);

#endif
