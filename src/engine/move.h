/* The Move tool's checked whole-layer translation and COW preview. */
#ifndef PC_ENGINE_MOVE_H
#define PC_ENGINE_MOVE_H
#include "engine.h"
/* Also reports the preview's active layer, which can be a newly duplicated id. */
pc_status pc_engine_move_preview_layer(const pc_engine_session *session, yyjson_val *params, pc_doc_arc **out,
                                       pc_layer_id *active, pc_error *err);
pc_status pc_engine_move_preview(const pc_engine_session *session, yyjson_val *params, pc_doc_arc **out,
                                 pc_error *err);
#endif
