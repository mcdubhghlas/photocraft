/* Incremental Brush/Pencil previews. Session documents and history stay untouched. */
#ifndef PC_ENGINE_LIVE_STROKE_H
#define PC_ENGINE_LIVE_STROKE_H
#include "engine.h"
typedef struct pc_engine_live_stroke pc_engine_live_stroke;
pc_status pc_engine_live_stroke_begin(pc_engine_session *session, const char *command, yyjson_val *params,
                                      pc_engine_live_stroke **out, pc_error *err);
/* Transfer a finished preview to its matching next Brush/Pencil commit. Replaces any old offer. */
void pc_engine_session_offer_live_stroke(pc_engine_session *session, pc_engine_live_stroke **live);
void pc_engine_session_clear_live_stroke(pc_engine_session *session);
uint64_t pc_engine_session_live_stroke_reuses(const pc_engine_session *session);
void pc_engine_live_stroke_free(pc_engine_live_stroke *live);
pc_status pc_engine_live_stroke_push(pc_engine_live_stroke *live, const pc_paint_point *points, size_t count,
                                     pc_rect *damage, pc_error *err);
pc_doc_arc *pc_engine_live_stroke_document(const pc_engine_live_stroke *live); /* borrowed */
uint64_t pc_engine_live_stroke_seed(const pc_engine_live_stroke *live);
pc_rect pc_engine_live_stroke_bounds(const pc_engine_live_stroke *live);
bool pc_engine_live_stroke_wants_time(const pc_engine_live_stroke *live);
bool pc_engine_live_stroke_smoothing(const pc_engine_live_stroke *live, double brush[2], double pointer[2]);
#endif
