#ifndef PC_ENGINE_STYLE_EDITOR_H
#define PC_ENGINE_STYLE_EDITOR_H
#include "engine/engine.h"
const char *pc_engine_style_contour_name(size_t index);
/* Canonical point coordinates in 0..255, corner flags and the rendered 256-sample curve. */
pc_status pc_engine_style_contour_view(yyjson_val *param, yyjson_mut_doc *doc, yyjson_mut_val **levels,
                                       yyjson_mut_val **corners, float samples[256], pc_error *err);
const pc_engine_command *pc_engine_style_editor_specs(size_t *count);
/* Display values are separate from deltas: unchanged imported effects retain their full
 * snapshots and native colour values. select_kind NULL opens Blending Options. */
pc_status pc_engine_style_editor_initial(const pc_engine_session *session, const char *select_kind,
                                         yyjson_mut_doc *doc, yyjson_mut_val **out, pc_error *err);
pc_status pc_engine_style_editor_entry(const char *kind, const pc_effect *effect, uint64_t id,
                                       yyjson_mut_doc *doc, yyjson_mut_val **out, pc_error *err);
pc_status pc_engine_style_editor_new_entry(const pc_engine_session *session, const char *kind, uint64_t id,
                                           yyjson_mut_doc *doc, yyjson_mut_val **out, pc_error *err);
#endif
