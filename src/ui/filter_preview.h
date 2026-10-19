#ifndef PC_UI_FILTER_PREVIEW_H
#define PC_UI_FILTER_PREVIEW_H
#include "engine/engine.h"
#include "engine/tone_preview.h"
typedef struct pc_ui_filter_preview {
    struct pc_ui_filter_task *task;
    pc_doc_arc *result;
    uint8_t wanted[32], ready[32];
    pc_engine_tone_histogram histogram;
    uint64_t histogram_scope[8];
    bool has_histogram;
    uint64_t generation;
    bool has_wanted, has_ready;
    pc_status status;
    pc_error error;
} pc_ui_filter_preview;
void pc_ui_filter_preview_fini(pc_ui_filter_preview *preview);
/* Nonblocking. At most one worker owns an independent session. NULL fields disables preview.
 * result is borrowed until the next update/fini, generation changes on every completed result. */
pc_status pc_ui_filter_preview_update(pc_ui_filter_preview *preview, const pc_engine_session *source,
                                      const yyjson_mut_val *fields, pc_error *err);
bool pc_ui_filter_preview_busy(const pc_ui_filter_preview *preview);
#endif
