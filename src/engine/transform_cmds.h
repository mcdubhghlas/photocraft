/* Free Transform / Transform commands (port of crates/engine/src/transform_cmds.rs, E4 ·
 * deferred transform-engine follow-up, E-tail ET1).
 *
 * This chunk ports the scriptable edit.transform command (projective transforms of a layer, the
 * selection, a lone mask / alpha channel / Quick Mask) and the frame-based presets
 * (edit.transform.{rotate180,rotate90Cw,rotate90Ccw,flipHorizontal,flipVertical}, which live in
 * extra_cmds.rs but were deferred there until edit.transform existed). The specs are registered
 * through pc_engine_transform_specs() (register_modules() in command.c).
 *
 * It also exports the two cross-module primitives the reference's transform_cmds exposes
 * (transform_layer and warp_gray), which the deferred Image Rotation · Arbitrary (mode_cmds, E3),
 * the Perspective/Straighten crop angle (image_cmds, E3), Straighten Layer (analysis_cmds) and the
 * job-based retouch patch / Content-Aware Move (retouch_cmds, E10) reach for. The straight-alpha
 * composite primitive and the Free-Transform snapping bounds stay where the earlier E4 slice put
 * them (pc_engine_transform_composite_over / pc_engine_hook_transform_bounds, engine_internal.h). */
#ifndef PC_ENGINE_TRANSFORM_CMDS_H
#define PC_ENGINE_TRANSFORM_CMDS_H

#include "algo/params.h"    /* pc_algo_interp */
#include "algo/transform.h" /* pc_algo_homography */
#include "doc/doc.h"
#include "engine.h"
#include "geom/geom.h" /* pc_affine */

/* The command specs for register_modules() (command.c). */
const pc_engine_command *pc_engine_transform_specs(size_t *n);

/* ---- cross-module primitives (Rust transform_cmds::{transform_layer, warp_gray}) ---------- */

/* The largest result (px) a single Free Transform warp may build: 2^30, the bound Puppet Warp
 * uses too (PC_ALGO_PUPPET_MAX_WARP_PIXELS). A scale past it is refused, not clipped. */
#define PC_ENGINE_MAX_TRANSFORM_PIXELS ((uint64_t)1 << 30)

/* transform_cmds::transform_layer. Projectively transform layer `l` by `h` (source -> document):
 * its pixels (only the pixels inside `doc_sel`, NULL = all), a linked raster mask and vector mask,
 * and - for the kinds that keep their geometry - the type transform, the shape path and the smart
 * placement. `affine` (NULL = projective) is the affine form of `h` when it is affine: type needs
 * it (Distort / Perspective on type is refused); shapes and smart objects keep a live shape /
 * exact affine with it and map the path / full projective otherwise. `group` holds the locks `l`
 * inherits from the groups around it (pc_engine_transform_group_locks). Refused with `l` untouched
 * when a result would exceed PC_ENGINE_MAX_TRANSFORM_PIXELS or a layer is locked in place; the
 * caller re-renders type/shape/smart caches afterwards (the transform hooks' refresh variants). */
pc_status pc_engine_transform_layer(const pc_surface *doc_sel, pc_locks group, pc_layer *l,
                                    const pc_algo_homography *h, const pc_affine *affine,
                                    pc_algo_interp interp, pc_error *err);

/* transform_cmds::composite_over: normal-blend `top` (straight alpha) over `dst` (straight alpha,
 * the same format), source-over, in the surfaces' own channels. Used by Image Rotation › Arbitrary
 * to drop the rotated Background pixels onto the background-colour fill (mode_cmds, ET3). */
pc_status pc_engine_transform_composite_over_native(pc_surface *dst, const pc_surface *top,
                                                    pc_error *err);

/* transform_cmds::split_selected: split a layer surface by a selection into (selected pixels,
 * everything else), both converted to `surf`'s format with alpha. *lifted and *rest are
 * initialised on success (caller frees with pc_surface_fini). Used by the distort warps
 * (edit.puppetWarp / edit.perspectiveWarp, distort_cmds) to lift the selected pixels, deform them
 * and composite them back over the rest. Never crashes. */
pc_status pc_engine_transform_split_selected(const pc_surface *surf, const pc_surface *sel,
                                             pc_surface *lifted, pc_surface *rest, pc_error *err);

/* transform_cmds::warp_gray. Warp a single-channel surface whose pixels outside the content read
 * as its default (a layer mask, an alpha channel, a selection) by `h`: the vacated area reads as
 * the default again. *out is initialised on success (pc_surface_fini); never crashes. */
pc_status pc_engine_transform_warp_gray(const pc_surface *s, const pc_algo_homography *h,
                                        pc_algo_interp interp, pc_surface *out, pc_error *err);

/* Refuse a warp of the content inside `src` by `h` whose result would exceed the pixel budget,
 * before anything is built (transform_cmds::check_budget). PC_OK when within budget or the map is
 * degenerate; the caller then handles the degenerate map. Used by select.transformSelection. */
pc_status pc_engine_transform_check_budget(pc_rect src, const pc_algo_homography *h, const char *what,
                                           pc_error *err);

/* The locks `id` inherits from the groups around it (transform_cmds::group_locks): the union of
 * every enclosing group's locks. Default locks when `id` is absent. */
pc_locks pc_engine_transform_group_locks(const pc_document *doc, pc_layer_id id);

/* Would a transform of layer `id` be refused because it, a group around it, or (for a group) a
 * layer inside it is locked in place? (transform_cmds::locked_for_transform, for the UI's
 * Free-Transform enablement). */
bool pc_engine_transform_locked(const pc_document *doc, pc_layer_id id);

#endif /* PC_ENGINE_TRANSFORM_CMDS_H */
