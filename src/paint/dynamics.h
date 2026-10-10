/* From input points to dabs (port of crates/paint/src/dynamics.rs): smoothing, brush pose, path
 * walking (spacing, speed spacing, airbrush build-up) and the per-dab dynamics (shape,
 * scattering, transfer, colour, texture depth).
 *
 * Everything is incremental (feed points in any chunking, get the same dabs) and deterministic:
 * jitter comes from rng.h keyed by the stroke seed and the step/dab index.
 *
 * Limits (never crash): a walker refuses to emit more than PC_PAINT_MAX_STEPS steps in one
 * stroke (PC_ERR_LIMIT): a hostile point such as x = 1e300 or an infinite segment would
 * otherwise loop for ever (the reference never returns there). Output vectors grow with
 * PC_ERR_NOMEM on failure; on any failure the generator's state is unspecified but valid
 * (finalise it).
 *
 * Thread safety: every object is single-owner; distinct objects may be used on different
 * threads. Functions taking `brush` only read it. */
#ifndef PC_PAINT_DYNAMICS_H
#define PC_PAINT_DYNAMICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "paint/brush.h"

/* Maximum pulled-string length at 100 % smoothing, in screen pixels. */
#define PC_PAINT_MAX_STRING_PX 100.0
/* With Spacing unchecked, one dab per this many milliseconds of stroke time. */
#define PC_PAINT_SPEED_SPACING_MS 8.0
/* Steps one walker may emit per stroke (C guard; see above). */
#define PC_PAINT_MAX_STEPS ((uint64_t)1 << 20)

/* ---- smoothing ---- */

typedef struct pc_paint_smoother {
    pc_paint_smoothing cfg;
    double zoom;
    bool has_pos, has_last;
    pc_paint_point pos, last_input;
} pc_paint_smoother;

/* `zoom` is the view zoom (only used with adjust_for_zoom). */
void pc_paint_smoother_init(pc_paint_smoother *s, const pc_paint_smoothing *cfg, float zoom);
/* Feed one input point; appends the brush positions to paint through. */
pc_status pc_paint_smoother_push(pc_paint_smoother *s, pc_paint_point p, pc_paint_point_vec *out,
                                 pc_error *err);
/* End of stroke: with Catch-Up On Stroke End the brush finishes at the last pointer position. */
pc_status pc_paint_smoother_finish(pc_paint_smoother *s, pc_paint_point_vec *out, pc_error *err);
/* Smooth a whole point list (appends to *out). */
pc_status pc_paint_smooth_points(const pc_paint_smoothing *cfg, float zoom, const pc_paint_point *pts,
                                 size_t n, pc_paint_point_vec *out, pc_error *err);

/* ---- path walking ---- */

/* One spacing (or airbrush time) step along the path. */
typedef struct pc_paint_step {
    pc_paint_point point;
    float direction; /* visual (CCW, y-up) direction of travel, radians */
    float initial_direction;
    uint64_t step; /* sequential step number */
} pc_paint_step;

typedef double (*pc_paint_step_len_fn)(void *ctx, const pc_paint_point *p);
typedef pc_status (*pc_paint_step_fn)(void *ctx, const pc_paint_step *s, pc_error *err);

/* Walks a polyline and emits steps every step_len(point) pixels, plus airbrush steps in time. */
typedef struct pc_paint_walker {
    bool has_last;
    pc_paint_point last;
    double next_at; /* path distance from `last` to the next step */
    bool has_pending_first;
    pc_paint_point pending_first;
    bool has_initial_dir;
    float initial_dir;
    float dir;
    uint64_t step;
    bool has_interval; /* airbrush */
    double interval;
    double time_acc;
    bool has_speed; /* speed spacing (Spacing unchecked) */
    double speed;
    double speed_acc;
} pc_paint_walker;

void pc_paint_walker_init(pc_paint_walker *w, bool has_interval, double build_up_interval_ms);
/* Space steps by stroke time instead of distance (segments without timestamps by distance). */
void pc_paint_walker_speed_spacing(pc_paint_walker *w, double interval_ms);
pc_status pc_paint_walker_push(pc_paint_walker *w, pc_paint_point p, pc_paint_step_len_fn len, void *len_ctx,
                               pc_paint_step_fn emit, void *emit_ctx, pc_error *err);
pc_status pc_paint_walker_finish(pc_paint_walker *w, pc_paint_step_fn emit, void *emit_ctx, pc_error *err);

/* ---- dynamics ---- */

/* Scalar control value in 0..1. */
float pc_paint_control_value(pc_paint_control c, uint32_t fade_steps, const pc_paint_step *s);
/* Angle contributed by a control, degrees (counter-clockwise). */
float pc_paint_control_angle(pc_paint_control c, uint32_t fade_steps, const pc_paint_step *s);
/* Colour Dynamics for one dab (or once per stroke with index 0). */
void pc_paint_dynamic_color(const pc_paint_brush *b, const pc_paint_step *s, uint64_t seed, uint64_t index,
                            float out[4]);
/* Base (pre-jitter) diameter at a step: size x size control (pressure etc.). Drives spacing. */
float pc_paint_base_diameter(const pc_paint_brush *b, const pc_paint_step *s);
/* Brush Pose applied to an input point. */
pc_paint_point pc_paint_apply_pose(const pc_paint_brush *b, pc_paint_point p);

/* Builds the primary dabs of a step. The brush is passed to each call (the reference keeps a
 * copy); pass the same brush every time. */
typedef struct pc_paint_dab_builder {
    uint64_t seed;
    bool has_stroke_color; /* per-stroke colour (per-tip off), computed at the first step */
    float stroke_color[4];
    uint64_t next_index;
} pc_paint_dab_builder;

void pc_paint_dab_builder_init(pc_paint_dab_builder *b, const pc_paint_brush *brush);
double pc_paint_dab_builder_step_len(const pc_paint_brush *brush, const pc_paint_point *p);
pc_status pc_paint_dab_builder_build(pc_paint_dab_builder *b, const pc_paint_brush *brush,
                                     const pc_paint_step *s, pc_paint_dab_vec *out, pc_error *err);

/* Builds Dual Brush dabs (the second tip: size, spacing, scatter, count, flip). */
typedef struct pc_paint_dual_builder {
    uint64_t next_index;
} pc_paint_dual_builder;

double pc_paint_dual_builder_step_len(const pc_paint_brush *brush, const pc_paint_point *p);
pc_status pc_paint_dual_builder_build(pc_paint_dual_builder *b, const pc_paint_brush *brush,
                                      const pc_paint_step *s, pc_paint_dab_vec *out, pc_error *err);

/* Incremental dab generation for a whole brush (primary and optional dual dabs). Holds its own
 * bounded copy of the brush. Copy with pc_paint_dab_generator_copy (never fails). */
typedef struct pc_paint_dab_generator {
    pc_paint_brush brush; /* bounded_for_render copy */
    pc_paint_smoother smoother;
    pc_paint_walker walker;
    bool dual_on;
    pc_paint_walker dual_walker;
    pc_paint_dab_builder builder;
    pc_paint_dual_builder dual;
    pc_paint_point_vec scratch;
} pc_paint_dab_generator;

void pc_paint_dab_generator_init(pc_paint_dab_generator *g, const pc_paint_brush *brush, float zoom);
void pc_paint_dab_generator_fini(pc_paint_dab_generator *g); /* NULL-safe */
void pc_paint_dab_generator_copy(pc_paint_dab_generator *dst, const pc_paint_dab_generator *src);
/* Feed input points; appends the new primary and dual dabs. */
pc_status pc_paint_dab_generator_push(pc_paint_dab_generator *g, const pc_paint_point *pts, size_t n,
                                      pc_paint_dab_vec *dabs, pc_paint_dab_vec *dual, pc_error *err);
/* End of stroke. */
pc_status pc_paint_dab_generator_finish(pc_paint_dab_generator *g, pc_paint_dab_vec *dabs,
                                        pc_paint_dab_vec *dual, pc_error *err);

/* All primary dabs of a stroke (lib.rs `dabs`): appended to *out. */
pc_status pc_paint_dabs(const pc_paint_brush *brush, const pc_paint_point *pts, size_t n,
                        pc_paint_dab_vec *out, pc_error *err);

#endif
