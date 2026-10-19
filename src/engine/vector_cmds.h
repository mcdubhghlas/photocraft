/* Vector commands (port of crates/engine/src/vector_cmds.rs, E11): shape layers (shape.*),
 * document paths (path.*), selection <-> path, and layer vector masks.
 *
 * This header exposes only the cross-module primitives other engine chunks reference (the Rust
 * pub / pub(crate) items): path transforms and the shape cache refresh (E4 Free Transform, E9
 * Distort/Perspective/Warp, canvas_geom's content-transform hooks). The command specs are
 * registered through pc_engine_vector_specs() and the rest of the module is private to the .c.
 */
#ifndef PC_ENGINE_VECTOR_CMDS_H
#define PC_ENGINE_VECTOR_CMDS_H

#include "algo/transform.h" /* pc_algo_homography */
#include "doc/doc.h"
#include "engine.h"
#include "geom/warp.h" /* pc_warp */

/* The command specs for register_modules() (command.c). */
const pc_engine_command *pc_engine_vector_specs(size_t *n);
/* E11: point-level path edits (path.moveAnchors / moveHandle / bendSegment / convertPoint /
 * addAnchor / deleteAnchor), defined in path_edit_cmds.inc (included by vector_specs.c). */
const pc_engine_command *pc_engine_path_edit_specs(size_t *n);

/* ---- cross-module primitives (Rust vector_cmds::{...}) ------------------------------------- */

/* vector_cmds::transform_shape. Applies an affine to a shape's path, keeping the live shape when
 * the transform is a translation plus axis-aligned scale (otherwise the shape stops being
 * parametric). Flips / 90-degree turns keep rectangles, ellipses and lines live. Clears psd_raw. */
void pc_engine_transform_shape(pc_shape_layer *sh, const pc_affine *a);

/* vector_cmds::refresh_shape. Re-renders the shape layer's cache with src/vector for `doc`'s
 * pixel format and bounds. Best effort: on OOM the cache is left unchanged. */
void pc_engine_refresh_shape(const pc_document *doc, pc_shape_layer *sh);

/* vector_cmds::translate_vectors. Moves a layer's vector content by (dx, dy): shape paths
 * (re-rendered) and linked vector masks, recursing into groups. Move tool / layer.move. */
void pc_engine_translate_vectors(const pc_document *doc, pc_layer *l, double dx, double dy);

/* vector_cmds::map_path. `path` through Distort / Perspective (`h`) or Warp (`warp`); NULL for
 * neither (a plain clone). Refused (PC_ERR_INVALID) when the map sends a point to infinity.
 * *out is written on success only (pc_path_fini). */
pc_status pc_engine_map_path(const pc_path *path, const pc_algo_homography *h, const pc_warp *warp,
                             pc_path *out, pc_error *err);

/* vector_cmds::map_shape. Distort / Perspective / Warp of a shape layer: the path is mapped
 * (map_path) and the shape stops being a live shape. Unchanged on error. */
pc_status pc_engine_map_shape(pc_shape_layer *sh, const pc_algo_homography *h, const pc_warp *warp,
                              pc_error *err);

/* Parse geometry-only or full engine stroke parameters; null/false disables a stroke. */
pc_status pc_engine_parse_shape_stroke(yyjson_val *value, const pc_shape_stroke *base,
                                       const float foreground[4], bool *present, pc_shape_stroke *out,
                                       pc_error *err);

/* Exact shape.create geometry/fill/stroke preparation, without a document edit or pixel cache. */
pc_status pc_engine_shape_from_params(const float foreground[4], yyjson_val *params, pc_shape_layer *out,
                                      pc_error *err);

/* vector_cmds::{parse_path, path_json}. Parse a `{subpaths,fillRule,inverted}` path value into
 * *out (false with emsg set on error), and serialise a path to that same JSON (for the Shapes
 * panel / shape.presets.place, which build shape.create params). */
bool pc_engine_parse_path(yyjson_val *v, pc_path *out, char *emsg, size_t ecap);
yyjson_mut_val *pc_engine_path_to_json(yyjson_mut_doc *d, const pc_path *p);

#endif /* PC_ENGINE_VECTOR_CMDS_H */
