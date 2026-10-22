/* Filter > Adaptive Wide Angle dialog model (clean-room port of wide_angle_ui.rs).
 *
 * The modal dialog session driving `filter.adaptiveWideAngle`: lens model + focal/crop/scale and a
 * list of straightening constraints drawn over a proxy. Pure data over the `pc_algo_wide_*`
 * primitives; no GL/SDL/Nuklear. Constraint end points are in DOCUMENT pixels. The shell converts
 * screen <-> document, owns the proxy/preview textures and arc drawing, and dispatches the command
 * built by pc_ui_wide_commit_params. Dialog-local undo/redo of the parameters is kept here
 * (independent of document history). */
#ifndef PC_UI_WIDE_ANGLE_H
#define PC_UI_WIDE_ANGLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "algo/wideangle.h"
#include "base/status.h"
#include "geom/geom.h"
#include "ui/tool_gesture.h"

typedef struct pc_ui_wide_dialog {
    bool active;
    uint64_t key, layer;
    pc_rect frame; /* document-pixel canvas bounds */
    pc_algo_wide_angle params;
    bool preview; /* show corrected (true) vs source */
    double residual;

    bool has_drag;
    double drag_a[2], drag_b[2];

    pc_algo_wide_angle *undo;
    size_t nundo, cap_undo;
    pc_algo_wide_angle *redo;
    size_t nredo, cap_redo;
} pc_ui_wide_dialog;

pc_status pc_ui_wide_begin(pc_ui_wide_dialog *s, uint64_t key, uint64_t layer, pc_rect frame, pc_error *err);
void pc_ui_wide_fini(pc_ui_wide_dialog *s); /* NULL-safe */

/* Pointer in document coords; `zoom` is document-px-to-screen-px so the >6px add / <10px delete
 * thresholds are in screen pixels, `shift` constrains the new line to horizontal/vertical. */
pc_status pc_ui_wide_pointer(pc_ui_wide_dialog *s, pc_ui_pointer_kind kind, double x, double y, double zoom,
                             bool shift, pc_error *err);
/* Nearest constraint to (x,y) within 10 screen px (via its arc); false when none. */
bool pc_ui_wide_nearest(const pc_ui_wide_dialog *s, double x, double y, double zoom, size_t *out);

/* Control-channel helpers. */
pc_status pc_ui_wide_add(pc_ui_wide_dialog *s, const double a[2], const double b[2],
                         pc_algo_wide_orientation orientation, pc_error *err);
pc_status pc_ui_wide_remove(pc_ui_wide_dialog *s, size_t index, pc_error *err);
pc_status pc_ui_wide_apply_json(pc_ui_wide_dialog *s, yyjson_val *set, pc_error *err); /* ui.set merge */
void pc_ui_wide_set_preview(pc_ui_wide_dialog *s, bool on);

bool pc_ui_wide_undo(pc_ui_wide_dialog *s);
bool pc_ui_wide_redo(pc_ui_wide_dialog *s);

pc_status pc_ui_wide_describe(const pc_ui_wide_dialog *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                             pc_error *err);
pc_status pc_ui_wide_commit_params(const pc_ui_wide_dialog *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                   pc_error *err);

#endif
