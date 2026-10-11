/* Private to the layer-effect files of src/compose (C3, port of effects.rs): the single-channel
 * map type and the helpers shared between fx_map.c (distance fields, blurs, shadow / glow /
 * satin maps), fx_bevel.c (bevel maps), fx_paint.c (paint, gradients, pattern tiles),
 * fx_build.c (FxMaps) and fx_composite.c (composite_with_effects).
 *
 * Every allocating helper returns pc_status (PC_ERR_NOMEM through pc_fail) and writes its
 * out-params only on success; maps are finalised with pc_fxm_free (NULL-safe on a zeroed map). */
#ifndef PC_COMPOSE_FX_INTERNAL_H
#define PC_COMPOSE_FX_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "compose/effects_util.h"
#include "compose/internal.h"

#define PC_FX_INSIDE_EPS PC_COMPOSE_FX_INSIDE_EPS
/* Start value of a chamfer pixel that is not a seed. */
#define PC_FX_CHAMFER_INF 1e20f
/* Far outside any shape (distance fill for cropped distance fields). */
#define PC_FX_FAR 1.0e9f

/* A single-channel map over a rectangle (effects.rs Map): w * h values, row-major. */
typedef struct pc_fxm {
    size_t w;
    size_t h;
    float *v; /* owned; may be NULL only when w * h == 0 */
} pc_fxm;

static inline size_t pc_fxm_len(const pc_fxm *m)
{
    return m->w * m->h;
}

/* ---- fx_map.c ---- */

pc_status pc_fxm_new(size_t w, size_t h, float fill, pc_fxm *out, pc_error *err);
pc_status pc_fxm_clone(const pc_fxm *m, pc_fxm *out, pc_error *err);
/* Takes over v (w * h values, may be NULL when that is 0). */
pc_fxm pc_fxm_wrap(size_t w, size_t h, float *v);
void pc_fxm_free(pc_fxm *m);
/* Zero outside the map. */
float pc_fxm_get(const pc_fxm *m, int64_t x, int64_t y);
/* Two box passes of width w along each axis, in place (rows in parallel on large maps). */
pc_status pc_fxm_tent(pc_fxm *m, float w, pc_error *err);
/* The table lookup through lut (n entries) in place; lut NULL leaves the map. */
void pc_fxm_apply_lut(pc_fxm *m, const float *lut, size_t n);

typedef enum pc_fx_metric { PC_FX_EUCLIDEAN, PC_FX_CHAMFER } pc_fx_metric;
/* Distance from pixel centres outside the shape to its edge (inside pixels -0.5). */
pc_status pc_fx_dist_outside(const pc_fxm *s, pc_fx_metric m, float **out, pc_error *err);
/* Distance from pixel centres inside the shape to its edge (outside pixels -0.5). */
pc_status pc_fx_dist_inside(const pc_fxm *s, pc_fx_metric m, float **out, pc_error *err);
/* Grows the shape by r pixels (chamfer metric, anti-aliased), keeping its soft edge. */
pc_status pc_fx_dilate(const pc_fxm *s, float r, pc_fxm *out, pc_error *err);
/* Alpha relative to the 3 x 3 neighbourhood maximum. */
pc_status pc_fx_local_coverage(const pc_fxm *s, pc_fxm *out, pc_error *err);

/* Shift of an effect lit from angle at distance (falls away from the light), whole pixels. */
void pc_fx_offset(float angle, float distance, float *dx, float *dy);

pc_status pc_fx_shadow_map(const pc_fxm *shape, const pc_shadow *s, const pc_global_light *light, bool inner,
                           int32_t ox, int32_t oy, pc_fxm *out, pc_error *err);
pc_status pc_fx_glow_map(const pc_fxm *shape, const pc_glow *g, bool inner, int32_t ox, int32_t oy,
                         pc_fxm *out, pc_error *err);
pc_status pc_fx_satin_map(const pc_fxm *shape, const pc_satin *s, pc_fxm *out, pc_error *err);

/* ---- fx_paint.c ---- */

/* PreparedPatterns::get by id / name (pc_str): *out = NULL when missing or empty. */
pc_status pc_fx_tile_get(const pc_compose_patterns *patterns, const pc_str *id, const pc_str *name,
                         pc_compose_pattern_tile **out, pc_error *err);

/* Composite colour x coverage m (over dst's rect) in blend at opacity. */
void pc_fx_paint_color(pc_compose_buffer *dst, const pc_fxm *m, const float c[3], pc_blend_mode blend,
                       float opacity);
pc_status pc_fx_paint_pattern(pc_compose_buffer *dst, const pc_fxm *m, const pc_compose_patterns *patterns,
                              const pc_str *name, const pc_str *id, const pc_compose_placement *place,
                              pc_rect big, pc_blend_mode blend, float opacity, pc_error *err);
pc_status pc_fx_paint_gradient(pc_compose_buffer *dst, const pc_fxm *m, const pc_gradient *g,
                               pc_rect shape_bounds, pc_rect big, pc_blend_mode blend, float opacity,
                               pc_error *err);
pc_status pc_fx_paint_fx(pc_compose_buffer *dst, const pc_fxm *m, const pc_fx_paint *p, pc_rect shape_bounds,
                         const double anchor[2], pc_rect big, pc_blend_mode blend, float opacity,
                         const pc_compose_patterns *patterns, pc_error *err);
pc_status pc_fx_paint_glow(pc_compose_buffer *dst, const pc_fxm *m, const pc_glow *g, pc_rect shape_bounds,
                           const double anchor[2], pc_rect big, const pc_compose_patterns *patterns,
                           pc_error *err);

/* ---- fx_bevel.c ---- */

/* Bevel highlight and shadow maps (maps[0], maps[1]) and where they paint. */
pc_status pc_fx_bevel_maps(const pc_fxm *shape, const pc_bevel *b, const pc_global_light *light,
                           const pc_compose_texture_ctx *tex, const pc_compose_patterns *patterns,
                           pc_fxm maps[2], pc_compose_bevel_paint *paint, pc_error *err);

/* ---- fx_build.c ---- */

/* The maps one enabled effect paints through (none, one, or a bevel's highlight + shadow). */
typedef struct pc_fx_item {
    pc_fxm m[2];
    size_t n;
    pc_compose_bevel_paint bevel_paint;
} pc_fx_item;

typedef struct pc_fx_frame {
    uint32_t bits; /* outside width (f32 bits) */
    pc_rect rect;
} pc_fx_frame;

/* effects::FxMaps. */
struct pc_compose_fx_maps {
    pc_rect rect;
    pc_fxm shape;
    pc_fx_item *per; /* per enabled effect, in items order */
    size_t nitems;
    float *din; /* chamfer distances when the layer has strokes, else NULL */
    float *dout;
    float *vdout;        /* shape layers without an outline: distance outside the estimated outline */
    bool outline;        /* the shape joins a filled shape's outline (pc_compose_effect_outline) */
    pc_fx_frame *frames; /* frames of gradient strokes by outside width */
    size_t nframes;
};

/* FxMaps::crop_vec / crop: v (over maps->rect; NULL = nothing) over `to`, `fill` elsewhere. */
pc_status pc_fx_crop_vec(const pc_compose_fx_maps *maps, const float *v, pc_rect to, float fill, float **out,
                         pc_error *err);
pc_status pc_fx_crop(const pc_compose_fx_maps *maps, const pc_fxm *m, pc_rect to, float fill, pc_fxm *out,
                     pc_error *err);

#endif
