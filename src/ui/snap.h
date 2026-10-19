/* Captured snap targets and smart-guide feedback. No document mutation. */
#ifndef PC_UI_SNAP_H
#define PC_UI_SNAP_H
#include "engine/snap.h"
#include "engine/engine.h"
#include "ui/state.h"
typedef struct pc_ui_snap {
    pc_snap_targets targets, smart;
    pc_snap_line *lines;
    size_t nlines;
    bool active, moving, smart_enabled, preserve_press;
    uint64_t document, revision;
    double start[2], rect[4];
} pc_ui_snap;
void pc_ui_snap_fini(pc_ui_snap *snap);
pc_status pc_ui_snap_begin(pc_ui_snap *snap, pc_engine_session *session, const pc_ui_state *ui,
                           const double start[2], const double *moving_rect, const pc_layer_id *exclude,
                           size_t nexclude, bool smart, bool exclude_selection, bool exclude_guides,
                           pc_error *err);
pc_status pc_ui_snap_point(pc_ui_snap *snap, const double point[2], double tolerance, bool disabled,
                           double out[2], pc_error *err);
#endif
