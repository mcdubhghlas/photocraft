#ifndef PC_ENGINE_FLOATING_H
#define PC_ENGINE_FLOATING_H
#include "engine/engine.h"
/* The Move tool floats only one visible, movable raster layer with a selection. */
bool pc_engine_floating_can_move(const pc_engine_session *session);
/* Read the current floating-piece offset. A stale piece is treated as absent. */
bool pc_engine_floating_offset(const pc_engine_session *session, int32_t offset[2]);
/* Independent COW document for displaying a pending selected-pixel move. Neither history nor
 * the session's floating piece changes. Offsets are relative to its existing floating offset. */
pc_status pc_engine_floating_preview(const pc_engine_session *session, int32_t dx, int32_t dy, bool copy,
                                     bool mask, pc_doc_arc **out, pc_error *err);
/* Borrowed cached document for the current floating piece; NULL when none exists. */
pc_status pc_engine_floating_document(pc_engine_session *session, pc_doc_arc **out, pc_error *err);
#endif
