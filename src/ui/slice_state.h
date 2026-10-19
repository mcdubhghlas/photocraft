#ifndef PC_UI_SLICE_STATE_H
#define PC_UI_SLICE_STATE_H
#include "doc/slices.h"
typedef struct pc_ui_slice_state {
    bool has_selected, hide_auto;
    size_t selected;
    bool dragging, moving;
    int tool;
    uint64_t document, revision;
    uint32_t slice;
    double start[2], current[2];
    pc_rect rect;
    pc_resolved_slice *resolved;
    size_t count;
    size_t *z;
    uint64_t cache_document, cache_revision;
    bool cached;
} pc_ui_slice_state;
void pc_ui_slice_fini(pc_ui_slice_state *state);
#endif
