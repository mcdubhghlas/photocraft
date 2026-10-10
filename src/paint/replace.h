/* Color Replacement (port of crates/paint/src/replace.rs): recolour pixels similar to a sampled
 * colour using a non-separable blend (Hue, Saturation, Color or Luminosity) with the foreground
 * colour. The Sampling and Limits enums are shared with the Background Eraser (bg_erase.h).
 *
 * Thread safety: the target must not be in use elsewhere. */
#ifndef PC_PAINT_REPLACE_H
#define PC_PAINT_REPLACE_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "geom/geom.h"
#include "paint/brush.h"
#include "raster/surface.h"

/* serde names: "hue", "saturation", "color", "luminosity". */
typedef enum pc_paint_replace_mode {
    PC_PAINT_REPLACE_HUE,
    PC_PAINT_REPLACE_SATURATION,
    PC_PAINT_REPLACE_COLOR, /* default */
    PC_PAINT_REPLACE_LUMINOSITY
} pc_paint_replace_mode;

/* serde names: "continuous", "once", "backgroundSwatch". */
typedef enum pc_paint_sampling {
    PC_PAINT_SAMPLING_CONTINUOUS,       /* resample under the brush centre at every dab (default) */
    PC_PAINT_SAMPLING_ONCE,             /* sample once, where the stroke starts */
    PC_PAINT_SAMPLING_BACKGROUND_SWATCH /* only colours near the background swatch */
} pc_paint_sampling;

/* serde names: "discontiguous", "contiguous", "findEdges". */
typedef enum pc_paint_limits {
    PC_PAINT_LIMITS_DISCONTIGUOUS, /* matching pixels anywhere under the brush */
    PC_PAINT_LIMITS_CONTIGUOUS,    /* matching pixels connected to the brush centre (default) */
    PC_PAINT_LIMITS_FIND_EDGES     /* contiguous, with no soft tolerance falloff */
} pc_paint_limits;

typedef struct pc_paint_replace_settings {
    pc_paint_replace_mode mode;
    pc_paint_sampling sampling;
    pc_paint_limits limits;
    float tolerance; /* 0..1 (max per-channel RGB distance) */
    bool anti_alias;
    float color[4];      /* replacement (foreground) colour, straight RGBA */
    float background[4]; /* background swatch for PC_PAINT_SAMPLING_BACKGROUND_SWATCH */
} pc_paint_replace_settings;

void pc_paint_replace_settings_default(pc_paint_replace_settings *s);
bool pc_paint_replace_settings_eq(const pc_paint_replace_settings *a, const pc_paint_replace_settings *b);

/* serde names of the enums (NULL out of range), and the reverse (false for an unknown name). */
const char *pc_paint_replace_mode_name(pc_paint_replace_mode m);
const char *pc_paint_sampling_name(pc_paint_sampling s);
const char *pc_paint_limits_name(pc_paint_limits l);
bool pc_paint_replace_mode_parse(const char *s, size_t len, pc_paint_replace_mode *out);
bool pc_paint_sampling_parse(const char *s, size_t len, pc_paint_sampling *out);
bool pc_paint_limits_parse(const char *s, size_t len, pc_paint_limits *out);

/* Replace colours along a stroke. *damage = the stroke bounds. On failure target is as for
 * pc_paint_apply_dab_stroke. */
pc_status pc_paint_apply_color_replacement(pc_surface *target, const pc_paint_stroke *stroke,
                                           const pc_paint_replace_settings *rs, const pc_surface *selection,
                                           bool lock_transparency, pc_rect *damage, pc_error *err);

/* Shared with the Background Eraser: zero the w * h weights not 4-connected to (sx, sy) through
 * non-zero weights (none kept when the start is outside or zero). PC_ERR_NOMEM. */
pc_status pc_paint_keep_connected(float *weight, size_t w, size_t h, size_t sx, size_t sy, pc_error *err);

#endif
