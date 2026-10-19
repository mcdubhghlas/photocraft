/* E5 shared helpers exported by the layer command modules (layer_multi_cmds.c and friends).
 *
 * The Layer-menu, multi-layer, navigation, label, group-view and mask-view command modules live
 * in src/engine/<area>_cmds.c; this header exposes the cross-module helpers they share (and that
 * later chunks reuse), mirroring the `pub(crate) fn` surface of crates/engine/src/layer_multi_cmds.rs
 * plus the `commands.rs` layer-targeting helpers the reference keeps there. None of this is the
 * public engine API (engine.h); it is internal to the engine crate. */
#ifndef PC_ENGINE_LAYER_CMDS_H
#define PC_ENGINE_LAYER_CMDS_H

#include "engine_internal.h"

pc_status pc_engine_deletion_neighbours(const pc_document *doc, pc_layer_id target, pc_layer_id **out,
                                        size_t *count, pc_error *err);

/* ---- layer-targeting helpers (port of commands::explicit_layer / layer_param) -------------- *
 *
 * The `"layer"` param names a layer by id or by name (the topmost layer of that name, as
 * Photoshop's "select layer by name" action step finds it). These are the reference's
 * commands.rs helpers; they live with the layer modules until the full commands.rs is ported. */

/* The layer `p["layer"]` names: *present=false (and *out untouched) when the param is absent or
 * null. A bad value or a missing named layer is an error. */
pc_status pc_engine_explicit_layer(const pc_engine_session *s, yyjson_val *p, bool *present, pc_layer_id *out,
                                   pc_error *err);
/* explicit_layer, else the active layer (error when there is none). */
pc_status pc_engine_layer_param(const pc_engine_session *s, yyjson_val *p, pc_layer_id *out, pc_error *err);

/* ---- multi-layer selection (layer_multi_cmds.c) ------------------------------------------- */

/* Selected layers of the active document, bottom-to-top (DocState::selected_layers). *out is an
 * owned array (free()) of *n ids; empty (*out NULL, *n 0) without a document or active layer. */
pc_status pc_engine_selected(const pc_engine_session *s, pc_layer_id **out, size_t *n, pc_error *err);

/* Replace the layer selection without a history step (set_selection). `ids` (n ids) become the
 * selection; `active` (has_active false = derive from the top of ids) the active layer; `anchor`
 * (has_anchor false = the active layer) the Shift-click anchor. PC_ERR_NOT_FOUND if any id is
 * stale. Bumps the revision, keeps a clean document clean, marks an empty damage rect. */
pc_status pc_engine_set_selection(pc_engine_session *s, const pc_layer_id *ids, size_t n, bool has_active,
                                  pc_layer_id active, bool has_anchor, pc_layer_id anchor, pc_error *err);

/* Preserve structural-command layer selection in the current history target. */
void pc_engine_reselect(pc_engine_session *s, const pc_layer_id *ids, size_t n, bool has_active,
                        pc_layer_id active);
const pc_engine_command *pc_engine_layer_move_to_specs(size_t *n);

/* Ids in `ids` (n) with no ancestor also in `ids`, bottom-to-top (top_level). *out is an owned
 * array (free()) of *nout ids. */
pc_status pc_engine_top_level(const pc_document *doc, const pc_layer_id *ids, size_t n, pc_layer_id **out,
                              size_t *nout, pc_error *err);

/* Content bounds used by Align/Distribute: the layer's pixels (type/shape use rendered pixels) or
 * the union of a group's children; false (fill/adjustment/empty) = none. */
bool pc_engine_layer_bounds(const pc_layer *l, pc_rect *out);

/* Whether `a` and `b` composite to the same pixels (same_pixels): they differ only in fields the
 * compositor never reads. Conservative (false) past a nesting depth no real document reaches. */
bool pc_engine_same_pixels(const pc_document *a, const pc_document *b);

/* ---- module registries (register_modules() in command.c) ---------------------------------- */

const pc_engine_command *pc_engine_layer_multi_specs(size_t *n);
const pc_engine_command *pc_engine_group_view_specs(size_t *n);
const pc_engine_command *pc_engine_layer_nav_specs(size_t *n);
const pc_engine_command *pc_engine_layer_label_specs(size_t *n);
const pc_engine_command *pc_engine_mask_view_specs(size_t *n);

/* ---- navigation (layer_nav_cmds.c) -------------------------------------------------------- */

/* The Layers panel's rows, top to bottom: each group before its children, collapsed groups
 * without them (panel_rows). *out is an owned array (free()) of *n ids. */
pc_status pc_engine_panel_rows(const pc_document *doc, pc_layer_id **out, size_t *n, pc_error *err);

#endif /* PC_ENGINE_LAYER_CMDS_H */
