/* Edit > Fill and the compositing helpers the fill, stroke, float and clear commands share
 * (port of crates/engine/src/fill_cmds.rs, E4).
 *
 * Exposes the small blend helpers the sibling E4 modules reuse: composite_over (paste pixels over
 * a surface), fill_color (an opaque colour through a coverage mask) and blend_color_mask (Edit >
 * Stroke's rasterised band), plus the Content-Aware sampling-window helpers. Internal to the
 * engine crate (not the public engine.h API). */
#ifndef PC_ENGINE_FILL_CMDS_H
#define PC_ENGINE_FILL_CMDS_H

#include "color/blend.h"
#include "engine_internal.h"
#include "raster/surface.h"

/* `src` over `surf` across `area` (Normal, 100%), each pixel weighted by `limit` when given (NULL
 * = none): a paste into a layer mask or channel, which keeps the pasted pixels' luminosity. */
pc_status pc_engine_composite_over(pc_surface *surf, const pc_surface *src, pc_rect area,
                                   const pc_surface *limit, pc_error *err);
/* `area` of `surf` filled with an opaque straight-RGBA `color` (Normal, 100%), weighted by `sel`
 * when given (NULL = fully covered). */
pc_status pc_engine_fill_color(pc_surface *surf, pc_rect area, const float color[4],
                               const pc_surface *sel, pc_error *err);
/* Edit > Stroke's rasterised band: `color` (optionally the CMYK inks it was picked as in `picked`,
 * else NULL) through the grayscale `coverage` mask, in `mode` at `opacity`, optionally preserving
 * transparency. */
pc_status pc_engine_blend_color_mask(pc_surface *surf, pc_rect area, const float color[4],
                                     const float *picked, const pc_surface *coverage, pc_blend_mode mode,
                                     float opacity, bool preserve_transparency, pc_error *err);

/* Photoshop's Content-Aware sampling window around the selection bounds `hole`, inside `canvas`. */
pc_rect pc_engine_fill_sampling_window(pc_rect hole, pc_rect canvas);
/* The size of a Content-Aware sampling window, or PC_ERR_LIMIT for a window too large to read. */
pc_status pc_engine_fill_window_size(pc_rect window, size_t *w, size_t *h, pc_error *err);

/* Edit > Fill (the registered command run). */
pc_status pc_engine_fill(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err);
/* edit.fill's enabled predicate: a pixel layer or a targeted channel (has_pixel_or_channel). */
pc_status pc_engine_has_pixel_or_channel(const pc_engine_session *s, pc_error *reason);
/* The registry module: edit.fill (register_modules() in command.c). */
const pc_engine_command *pc_engine_fill_specs(size_t *n);
/* The other E4 command-module registries (register_modules() in command.c). */
const pc_engine_command *pc_engine_fill_key_specs(size_t *n); /* fill_key_cmds.c: the 5 fill keys */
const pc_engine_command *pc_engine_select_rect_specs(size_t *n); /* command.c-local: select.rect */
const pc_engine_command *pc_engine_float_specs(size_t *n);       /* float_cmds.c: select.float/drop */
const pc_engine_command *pc_engine_stamp_specs(size_t *n);       /* stamp_cmds.c: stamp visible/down */
/* The Alt variant of a Layer-menu merge command (stamp_cmds::alt_variant), used by dispatch (E12);
 * NULL when there is none. */
const char *pc_engine_stamp_alt_variant(const char *id);

/* transform_cmds.c composite_over: `top` (straight alpha) over `dst`, straight RGBA, over top's
 * content bounds. Used by the floating-selection drop (E4). */
void pc_engine_transform_composite_over(pc_surface *dst, const pc_surface *top, pc_error *err);

#endif /* PC_ENGINE_FILL_CMDS_H */
