/* Gradient Fill layers (port of crates/compose/src/gradient_fill.rs): their colour ramp (colour
 * stops with midpoints, opacity stops, dither) and the on-canvas geometry of the Gradient tool's
 * live gradients, where a start and an end point stand for the fill's angle, scale and centre
 * offset.
 *
 * The mapping matches the Gradient tool's destructive drag: a Linear gradient runs from the start
 * (t = 0) to the end point (t = 1); Radial, Angle, Reflected and Diamond gradients are centred on
 * the start point and reach t = 1 at the end point's distance. Pure functions; thread-safe (a
 * prepared ramp is only read). */
#ifndef PC_COMPOSE_GRADIENT_FILL_H
#define PC_COMPOSE_GRADIENT_FILL_H

#include <stdbool.h>
#include <stddef.h>

#include "doc/doc.h"

struct pc_compose_buffer;

typedef struct pc_compose_ramp_stop {
    float pos;
    float c[5]; /* sRGB + alpha (nch 4), or native CMYK + alpha (nch 5) */
} pc_compose_ramp_stop;

/* A gradient fill's colour ramp, prepared for one render (colours converted once, through the
 * calling thread's active CMYK space). Owns its arrays; the midpoints are borrowed from the fill,
 * which must outlive the ramp. */
typedef struct pc_compose_ramp {
    pc_compose_ramp_stop *color; /* sorted by position (stable) */
    size_t ncolor;
    const float *mids;
    size_t nmids;
    pc_opacity_stop *opacity; /* sorted by position (stable) */
    size_t nopacity;
    size_t nch; /* channels per colour, alpha last: 4 (RGBA) or 5 (CMYKA) */
} pc_compose_ramp;

/* Ramp::new: *found = false (out untouched) for fills that aren't gradients. */
pc_status pc_compose_ramp_init(const pc_fill *f, bool *found, pc_compose_ramp *out, pc_error *err);
void pc_compose_ramp_fini(pc_compose_ramp *r); /* NULL-safe; leaves r empty */
/* The ramp in native CMYK ink values (Ramp::with_colors with each stop's colour in CMYK): nch 5. */
pc_status pc_compose_ramp_init_cmyk(const pc_fill *f, bool *found, pc_compose_ramp *out, pc_error *err);
/* Colour and alpha at t (0..=1); out has r->nch values (4 for pc_compose_ramp_init). */
void pc_compose_ramp_sample(const pc_compose_ramp *r, float t, float *out);
/* Where a segment of relative position u lands once its midpoint is m (0.5 = unchanged). */
float pc_compose_midpoint_remap(float u, float m);
/* Adds one level of the shared dither noise to a colour (as the Gradient tool does). */
void pc_compose_gradient_dither(float c[4], int32_t x, int32_t y);
/* The same on n channels, then rounded to quantum steps when has_quantum (the document depth's
 * levels): left in float, a dark channel lifted a fraction of a level is still that fraction to
 * the next blend, and Color Dodge turns it into visible speckle (#2755). */
void pc_compose_gradient_dither_to(float *c, size_t n, int32_t x, int32_t y, bool has_quantum, float quantum);

/* gradient_fill::render: a gradient fill over rect laid out in frame into a fresh buffer over
 * rect (transparent for other fills). Rows render on pc_pool_global(). */
pc_status pc_compose_gradient_fill_render(const pc_fill *f, pc_rect rect, pc_rect frame,
                                          struct pc_compose_buffer *out, pc_error *err);
/* gradient_fill::render_quantized: dithered pixels rounded to quantum steps (has_quantum false:
 * pc_compose_gradient_fill_render). */
pc_status pc_compose_gradient_fill_render_quantized(const pc_fill *f, pc_rect rect, pc_rect frame,
                                                    bool has_quantum, float quantum,
                                                    struct pc_compose_buffer *out, pc_error *err);
/* gradient_fill::render_cmyk_fill: a solid or gradient fill's native CMYK pixels (with alpha) at
 * depth over rect, laid out in frame, as a pruned surface (the same samples for rendering and PSD
 * caches, so the fill is not separated through RGB). *found false (out untouched) for patterns.
 * Colours convert through the calling thread's active CMYK space. */
pc_status pc_compose_render_cmyk_fill(const pc_fill *f, pc_rect rect, pc_rect frame, pc_sample_type depth,
                                      bool *found, pc_surface *out, pc_error *err);

/* The start and end points (document pixels) of a gradient laid out in frame. */
void pc_compose_gradient_handles(pc_gradient_style style, float angle, float scale, const float offset[2],
                                 pc_rect frame, float from[2], float to[2]);
/* The handles of a gradient fill in frame; false (untouched) for other fills. */
bool pc_compose_gradient_fill_handles(const pc_fill *f, pc_rect frame, float from[2], float to[2]);
/* Angle (degrees), scale and centre offset that put a gradient's start and end points at from
 * and to in frame (inverse of pc_compose_gradient_handles). A zero-length drag keeps the
 * direction of fallback_angle. */
void pc_compose_gradient_from_handles(pc_gradient_style style, const float from[2], const float to[2],
                                      pc_rect frame, float fallback_angle, float *angle, float *scale,
                                      float offset[2]);

#endif
