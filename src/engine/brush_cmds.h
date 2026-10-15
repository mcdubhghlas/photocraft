/* Brush engine commands and the painting primitives the sibling tool chunks share (port of
 * crates/engine/src/brush_cmds.rs, E10).
 *
 * This header exports the helpers the Eraser (eraser_cmds), the brush-size keys (brush_key_cmds),
 * the retouch tools (retouch_cmds) and the Paint Bucket / Gradient (paint_cmds) reach for: point
 * parsing, brush resolution and the session brush (tools.setBrush), plus the mutable target
 * surface a pixel command writes to and the small enabled/blend/mask predicates that live in the
 * Rust crate::commands module. */
#ifndef PC_ENGINE_BRUSH_CMDS_H
#define PC_ENGINE_BRUSH_CMDS_H

#include "color/blend.h"
#include "color/convert.h"
#include "engine_internal.h"
#include "paint/brush.h"

/* Largest stroke coordinate accepted (MAX_COORD): a few times the largest document side. */
#define PC_ENGINE_BRUSH_MAX_COORD 1000000.0

/* ---- shared painting predicates (Rust crate::commands) ------------------------------------ */

/* A layer that can be painted: a pixel layer, any layer with a mask, or a targeted channel /
 * Quick Mask (has_paintable). */
pc_status pc_engine_has_paintable(const pc_engine_session *s, pc_error *reason);
/* The "target" param asks for the active layer's mask ("target":"mask"). */
bool pc_engine_is_mask_target(yyjson_val *p);
/* Lenient blend-mode name (normal|multiply|…, case/space/underscore/paren-insensitive), including
 * "passThrough"; false (nothing written) for an unknown name (crate::commands::blend_from_str). */
bool pc_engine_blend_from_str(const char *s, size_t len, pc_blend_mode *out);

/* The surface a paint command writes to and whether the layer's transparency is locked
 * (channel_cmds::target_surface). has_layer/layer give the active layer for the Pixels/Mask
 * targets. Borrowed from doc; valid until doc is mutated. On a locked layer, PC_ERR_INVALID. */
pc_status pc_engine_target_surface(pc_document *doc, bool has_layer, pc_layer_id layer, yyjson_val *p,
                                   pc_surface **out, bool *lock, pc_error *err);
/* The surface a paint command writes to: the layer's pixels, or its mask with "target":"mask"
 * (crate::commands::paint_surface). Refuses a locked layer. Borrowed from doc. */
pc_status pc_engine_paint_surface(pc_document *doc, pc_layer_id id, yyjson_val *p, pc_surface **out,
                                  pc_error *err);

/* ---- brush resolution and the session brush ---------------------------------------------- */

/* Parse `points` (arrays [x,y,pressure?,tiltX?,tiltY?,rotation?,timeMs?,wheel?] or objects). *out
 * is an owned array (free()) of *n points. PC_ERR_INVALID (nothing written) on missing/empty/out
 * of range. */
pc_status pc_engine_parse_points(yyjson_val *p, const char *cmd, pc_paint_point **out, size_t *n,
                                 pc_error *err);
/* A stroke's coordinates must be finite and within +-MAX_COORD (check_coords, #976). */
pc_status pc_engine_check_coords(const pc_paint_point *pts, size_t n, const char *cmd, pc_error *err);

/* Deep-merge the JSON brush `patch` onto `base` (merge_brush). *out is overwritten, not finalised
 * (pc_paint_brush_fini it). */
pc_status pc_engine_merge_brush(const pc_paint_brush *base, yyjson_val *patch, const char *cmd,
                               pc_paint_brush *out, pc_error *err);
/* Reject brush sizes and Scatter amounts out of range (validate_brush). */
pc_status pc_engine_validate_brush(const pc_paint_brush *b, const char *cmd, pc_error *err);
/* Resolve the brush for a stroke command: session brush <- preset <- brush patch <- scalar params;
 * colours from the session, seed from `seed` or the points (resolve_brush). *out finalised by the
 * caller. */
pc_status pc_engine_resolve_brush(const pc_engine_session *s, yyjson_val *p, const char *cmd,
                                 pc_paint_brush *out, pc_error *err);
/* tools.setBrush: deep-merge, validate and store the session brush (set_brush). Writes *out. */
pc_status pc_engine_set_brush(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err);
/* The session brush as serde JSON (brush_json); NULL on OOM. */
yyjson_mut_val *pc_engine_brush_json(yyjson_mut_doc *d, const pc_paint_brush *b);

/* The document's own CMYK space (a new reference, release with pc_cmyk_space_release), or NULL for
 * the default conversions (doc_cmyk_space). */
pc_cmyk_space *pc_engine_doc_cmyk_space(const pc_engine_session *s);

/* Bump presets_rev so the preset store syncs (Session::brush_presets_changed). */
void pc_engine_brush_presets_changed(pc_engine_session *s);

#endif /* PC_ENGINE_BRUSH_CMDS_H */
