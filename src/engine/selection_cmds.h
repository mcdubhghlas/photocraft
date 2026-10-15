/* Select menu and selection tools (port of crates/engine/src/selection_cmds.rs, E7): Magic Wand,
 * Modify (expand/contract/border/smooth/feather), Grow, Similar, Lasso. */
#ifndef PC_ENGINE_SELECTION_CMDS_H
#define PC_ENGINE_SELECTION_CMDS_H

#include "engine_internal.h"

const pc_engine_command *pc_engine_selection_specs(size_t *n);

/* Select the polygon `pts` (x,y pairs) with the lasso tools' mode/antiAlias/feather from `p`, as
 * one history step `label` (selection_cmds::polygon_selection). Shared with the Magnetic Lasso
 * (magnetic_cmds, E7). Writes *out on success (a {"selected":bool} object). */
pc_status pc_engine_polygon_selection(pc_engine_session *s, const char *cmd, const char *label,
                                      const float (*pts)[2], size_t npts, yyjson_val *p,
                                      pc_json_value *out, pc_error *err);

/* Other E7 selection registries (magnetic lasso, Sky/Isolate/Transform Selection, smart select). */
const pc_engine_command *pc_engine_magnetic_specs(size_t *n);
const pc_engine_command *pc_engine_select_extra_specs(size_t *n);
const pc_engine_command *pc_engine_smartselect_specs(size_t *n);

/* Whether a Layers-panel row for `id` is shown under Select > Isolate Layers (select_extra_cmds);
 * ancestors of an isolated layer stay visible. isolation_shows: used by inspect. */
bool pc_engine_isolation_shows(const pc_document *doc, const pc_layer_id *isolated, size_t nisolated,
                               pc_layer_id id);

#endif /* PC_ENGINE_SELECTION_CMDS_H */
