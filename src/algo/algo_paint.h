/* Paint helpers: the Gradient tool and the Paint Bucket (port of crates/algo/src/paint.rs; not
 * the brush engine, which is src/paint/).
 *
 * Both composite a colour over an area of a surface tile by tile (in parallel on the global pool;
 * tiles with zero coverage are left untouched, so their copy-on-write storage survives), through
 * the surface's own pixel format. Colours are straight sRGB RGBA. */
#ifndef PC_ALGO_ALGO_PAINT_H
#define PC_ALGO_ALGO_PAINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/selection.h"
#include "base/status.h"
#include "color/blend.h"
#include "geom/geom.h"
#include "raster/surface.h"

/* Gradient tool shape (serde camelCase: "linear", "radial", "angle", "reflected", "diamond";
 * default linear). */
typedef enum pc_algo_gradient_shape {
    PC_ALGO_GRADIENT_LINEAR,
    PC_ALGO_GRADIENT_RADIAL,
    PC_ALGO_GRADIENT_ANGLE,
    PC_ALGO_GRADIENT_REFLECTED,
    PC_ALGO_GRADIENT_DIAMOND
} pc_algo_gradient_shape;

const char *pc_algo_gradient_shape_name(pc_algo_gradient_shape s); /* NULL for an unknown value */
bool pc_algo_gradient_shape_from_name(const char *s, size_t len, pc_algo_gradient_shape *out);

/* One RGBA gradient stop at position `pos` (stops are sorted by position). */
typedef struct pc_algo_gradient_stop {
    float pos;
    float color[4];
} pc_algo_gradient_stop;

/* Gradient tool parameter t (0..1) at (x, y) for a drag from `from` to `to`. */
float pc_algo_paint_gradient_t(pc_algo_gradient_shape shape, const float from[2], const float to[2], float x,
                               float y);
/* Samples the stops at t (no stops: transparent black). */
void pc_algo_paint_sample_stops(const pc_algo_gradient_stop *stops, size_t n, float t, float out[4]);

/* Gradient tool over `area` (the selection bounds or canvas); selection may be NULL. */
pc_status pc_algo_paint_gradient(pc_surface *s, pc_rect area, const float from[2], const float to[2],
                                 pc_algo_gradient_shape shape, const pc_algo_gradient_stop *stops,
                                 size_t nstops, bool reverse, float opacity, pc_blend_mode mode, bool dither,
                                 const pc_surface *selection, bool lock_transparency, pc_error *err);

/* A fill colour per canvas pixel (e.g. a pattern); called from several pool workers at once, so
 * it must be thread-safe. */
typedef void (*pc_algo_paint_src_fn)(void *ctx, int32_t x, int32_t y, float out[4]);

/* Paint bucket: fills the region similar to the seed pixel (on the surface's own pixels) within
 * `area`, respecting the selection. *filled is false when the seed lies outside `area`. */
pc_status pc_algo_paint_bucket_fill(pc_surface *s, pc_rect area, int32_t seed_x, int32_t seed_y,
                                    float tolerance, bool contiguous, bool anti_alias, const float color[4],
                                    float opacity, const pc_surface *selection, bool lock_transparency,
                                    bool *filled, pc_error *err);
/* As pc_algo_paint_bucket_fill with the colour from src(ctx, x, y). */
pc_status pc_algo_paint_bucket_fill_src(pc_surface *s, pc_rect area, int32_t seed_x, int32_t seed_y,
                                        float tolerance, bool contiguous, bool anti_alias, float opacity,
                                        const pc_surface *selection, bool lock_transparency,
                                        pc_algo_paint_src_fn src, void *ctx, bool *filled, pc_error *err);
/* Paints a bucket region sampled before the edit (so its boundary can come from the visible
 * composite while only `s` receives the colour), clipped by the selection coverage. */
pc_status pc_algo_paint_bucket_fill_region_src(pc_surface *s, const pc_algo_region *region, float opacity,
                                               const pc_surface *selection, bool lock_transparency,
                                               pc_algo_paint_src_fn src, void *ctx, pc_error *err);

#endif
