#ifndef PC_UI_DIRECT_STATE_H
#define PC_UI_DIRECT_STATE_H
#include "base/json.h"
#include "vector/vector.h"
/* Selection persists; document/revision captures and preview paths are runtime-only. */
typedef struct pc_ui_direct {
    pc_str name;
    uint64_t layer;
    pc_vec_knot_ref *anchors;
    size_t count;
    bool has_segment;
    pc_vec_knot_ref segment;
    bool active, marquee, add, independent, has_preview;
    unsigned edit;
    pc_vec_hit hit;
    pc_point handle_start;
    double start[2], end[2];
    uint64_t document;
    uint64_t revision, generation;
    pc_path base, preview;
} pc_ui_direct;
void pc_ui_direct_cancel(pc_ui_direct *state);
void pc_ui_direct_fini(pc_ui_direct *state);
pc_status pc_ui_direct_to_json(const pc_ui_direct *state, yyjson_mut_doc *doc, yyjson_mut_val **out,
                               pc_error *err);
pc_status pc_ui_direct_from_json(pc_ui_direct *state, yyjson_val *value, pc_error *err);
#endif
