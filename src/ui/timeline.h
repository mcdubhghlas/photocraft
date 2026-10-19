#ifndef PC_UI_TIMELINE_H
#define PC_UI_TIMELINE_H
#include "base/json.h"
#include "doc/doc.h"
typedef struct pc_ui_timeline {
    bool open, playing;
    double last_time;   /* monotonic milliseconds, transient */
    pc_doc_id document; /* transient; changing tabs stops playback */
} pc_ui_timeline;
/* Partial configuration is transactional; unknown fields and invalid types are errors. */
pc_status pc_ui_timeline_configure(pc_ui_timeline *state, yyjson_val *params, pc_error *err);
yyjson_mut_val *pc_ui_timeline_json(const pc_ui_timeline *state, yyjson_mut_doc *doc);
bool pc_ui_timeline_step(pc_ui_timeline *state, pc_doc_id document, const pc_timeline *timeline, double now,
                         size_t *frame);
#endif
