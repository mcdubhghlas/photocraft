/* Filter > Liquify dialog model (clean-room port of liquify_ui.rs).
 *
 * The modal dialog session driving `filter.liquify`: a displacement field (pc_algo_liquify_field)
 * edited by brush strokes, with a tool strip, brush settings, a freeze mask and session-local
 * undo/redo. Pure data over the `pc_algo_liquify_*` primitives; no GL/SDL/Nuklear. Coordinates are
 * document pixels. The shell owns the proxy/mask/backdrop textures, the preview rendering and the
 * zoom/pan, converts screen <-> document, and dispatches the command built by
 * pc_ui_liquify_commit_params. The brush/view settings persist under prefs key "filter.liquify". */
#ifndef PC_UI_LIQUIFY_DIALOG_H
#define PC_UI_LIQUIFY_DIALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "algo/liquify.h"
#include "base/status.h"
#include "geom/geom.h"
#include "ui/tool_gesture.h"

enum { PC_UI_LIQUIFY_REDO_LIMIT = 100 };

/* Persisted brush / view settings (prefs key "filter.liquify"). */
typedef struct pc_ui_liquify_opts {
    pc_algo_liquify_tool tool;
    double size, density, pressure, rate;
    bool show_mesh;
    int mesh_size; /* 0 small, 1 medium, 2 large */
    bool show_mask, show_backdrop;
    double backdrop_opacity;
    double reconstruct_amount; /* not persisted */
} pc_ui_liquify_opts;

void pc_ui_liquify_opts_default(pc_ui_liquify_opts *o);
/* LiquifyOpts::apply: reads camelCase keys, clamps, skips non-finite; a bad "tool" is applied last
 * and returns PC_ERR_INVALID (other fields still applied). */
pc_status pc_ui_liquify_opts_apply(pc_ui_liquify_opts *o, yyjson_val *v, pc_error *err);
yyjson_mut_val *pc_ui_liquify_opts_to_json(yyjson_mut_doc *d, const pc_ui_liquify_opts *o);

typedef struct pc_ui_liquify_dialog {
    bool active;
    uint64_t layer;
    pc_rect canvas;
    double cell;
    pc_algo_liquify_field field;
    pc_algo_liquify_stroke *strokes;
    size_t nstrokes, cap_strokes;
    pc_algo_liquify_stroke *redo;
    size_t nredo, cap_redo;
    bool has_cur;
    pc_algo_liquify_stroke cur;
    double last_point[3];
    bool lasso_active, lasso_subtract;
    double (*lasso)[2];
    size_t nlasso, cap_lasso;
    pc_ui_liquify_opts opts;
} pc_ui_liquify_dialog;

/* Begins over the layer `canvas`. Builds an identity field (auto cell), sets the initial brush
 * size from the canvas, then applies `prefs` (a JSON object, may be NULL; bad fields skipped). */
pc_status pc_ui_liquify_begin(pc_ui_liquify_dialog *s, uint64_t layer, pc_rect canvas, yyjson_val *prefs,
                              pc_error *err);
void pc_ui_liquify_fini(pc_ui_liquify_dialog *s); /* NULL-safe */

/* One pointer event (document coords + pressure 0..1, alt the reverse-tool modifier). */
pc_status pc_ui_liquify_pointer(pc_ui_liquify_dialog *s, pc_ui_pointer_kind kind, double x, double y,
                                double pressure, bool alt, pc_error *err);
/* A whole-field operation (ReconstructAll/FreezeAll/ThawAll/InvertFreeze); amount for reconstruct. */
pc_status pc_ui_liquify_global(pc_ui_liquify_dialog *s, pc_algo_liquify_tool tool, bool has_amount,
                              double amount, pc_error *err);
pc_status pc_ui_liquify_restore_all(pc_ui_liquify_dialog *s, pc_error *err);
pc_status pc_ui_liquify_undo(pc_ui_liquify_dialog *s, pc_error *err);
pc_status pc_ui_liquify_redo(pc_ui_liquify_dialog *s, pc_error *err);
/* `[` (dir < 0) / `]` (dir > 0): change size by at least 1, clamped 1..15000. */
void pc_ui_liquify_size_step(pc_ui_liquify_dialog *s, int dir);

/* The Option-reversed tool (twirl cw<->ccw, pucker<->bloat, else unchanged). */
pc_algo_liquify_tool pc_ui_liquify_alt_tool(pc_algo_liquify_tool t);
/* Alt+right-drag or Ctrl+Alt+left-drag resizes the brush instead of painting. */
bool pc_ui_liquify_resize_gesture(bool alt, bool ctrl, bool secondary);
/* resized_brush: size = clamp(round(start_size+dx),1,15000); density = clamp(start_density +
 * dy*100/200, 0, 100) rounded to 0.1. */
void pc_ui_liquify_resized_brush(double start_size, double start_density, double dx, double dy,
                                 double *size, double *density);

/* {"strokes", "meshSize":cell, "layer"} on commit; *run false (and *out NULL) when no strokes. */
pc_status pc_ui_liquify_commit_params(pc_ui_liquify_dialog *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                      bool *run, pc_error *err);
pc_status pc_ui_liquify_describe(pc_ui_liquify_dialog *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                pc_error *err);

#endif
