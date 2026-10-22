/* Edit > Perspective Warp UI model (clean-room port of crates/ui-egui/src/perspective_ui.rs).
 *
 * The interactive session that drives the engine's `edit.perspectiveWarp` /
 * `layer.smartObjects.perspectiveWarp` commands: planes (quads) drawn in Layout mode whose corners
 * then move in Warp mode. Pure data and bounded maths over the `pc_algo_persp_*` primitives; no GL,
 * SDL or Nuklear. Coordinates are document pixels. The shell converts screen <-> document, supplies
 * the hit tolerance (8 / max(zoom, 0.01) document px), owns the preview textures, draws the overlay
 * from `planes`/`grid`, and dispatches the command built by pc_ui_persp_commit_params.
 *
 * Never crashes: hostile coordinates, counts and indices are bounded and validated; a drag that
 * outlived the plane list it indexed is dropped. */
#ifndef PC_UI_PERSP_WARP_H
#define PC_UI_PERSP_WARP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "algo/perspective.h"
#include "base/status.h"
#include "ui/tool_gesture.h"

/* perspective preview grid: GRID + 1 points per axis (perspective_ui.rs GRID = 40). */
enum { PC_UI_PERSP_GRID = 40, PC_UI_PERSP_GRID_N = 41, PC_UI_PERSP_NGRID = 41 * 41 };
/* Bound on planes accepted from the control channel / drawn (hostile input guard). */
enum { PC_UI_PERSP_MAX_PLANES = 1024 };

typedef enum pc_ui_persp_mode { PC_UI_PERSP_LAYOUT = 0, PC_UI_PERSP_WARP } pc_ui_persp_mode;

typedef enum pc_ui_persp_drag_kind {
    PC_UI_PERSP_DRAG_NONE = 0,
    PC_UI_PERSP_DRAG_NEWPLANE,
    PC_UI_PERSP_DRAG_CORNER
} pc_ui_persp_drag_kind;

/* One (plane, corner) reference; a drag may move a whole linked group of them. */
typedef struct pc_ui_persp_ref {
    size_t plane, corner;
} pc_ui_persp_ref;

typedef struct pc_ui_persp_session {
    bool active;
    uint64_t key, layer;
    char command[64]; /* "edit.perspectiveWarp" or "layer.smartObjects.perspectiveWarp" */
    double bounds[4]; /* source box {x0, y0, x1, y1}, document px */
    pc_algo_persp_plane *planes;
    size_t nplanes, cap_planes;
    pc_ui_persp_mode mode;

    pc_ui_persp_drag_kind drag;
    double new_start[2], new_cur[2]; /* NewPlane rubber-band */
    pc_ui_persp_ref *group;          /* Corner drag: the linked corners moved together */
    size_t ngroup, cap_group;
    double corner_offset[2]; /* [corner - pointer] at press */

    double grid[PC_UI_PERSP_NGRID][2]; /* destination preview mesh, row-major (j outer, i inner) */
} pc_ui_persp_session;

/* Begins a session over `bounds` with no planes, Layout mode (perspective_ui begin). `command` is
 * copied (truncated to fit). Returns PC_ERR_INVALID for non-finite bounds. */
pc_status pc_ui_persp_begin(pc_ui_persp_session *s, uint64_t key, uint64_t layer, const char *command,
                            const double bounds[4], pc_error *err);
void pc_ui_persp_fini(pc_ui_persp_session *s); /* NULL-safe; frees planes/group, leaves inactive */

/* Set Layout/Warp. Switching to Warp unifies linked corners then rebuilds the grid. */
pc_status pc_ui_persp_set_mode(pc_ui_persp_session *s, pc_ui_persp_mode mode, pc_error *err);
/* Rebuilds the 41x41 preview grid (identity in Layout, the plane map in Warp). */
pc_status pc_ui_persp_update_grid(pc_ui_persp_session *s, pc_error *err);

/* One pointer event in document coordinates; `tol` is the hit radius in document px. Add/drag
 * planes and corners. Never indexes out of range. */
pc_status pc_ui_persp_pointer(pc_ui_persp_session *s, pc_ui_pointer_kind kind, double x, double y,
                              double tol, pc_error *err);

/* Replaces the planes from a JSON array (control channel ui.planes); clears any drag (#952). */
pc_status pc_ui_persp_set_planes_json(pc_ui_persp_session *s, yyjson_val *arr, pc_error *err);
/* Applies a straighten mode to the planes, then rebuilds the grid. */
pc_status pc_ui_persp_straighten(pc_ui_persp_session *s, pc_algo_persp_straighten mode, pc_error *err);
/* Clears all planes (options-bar "Remove Planes"). */
void pc_ui_persp_clear(pc_ui_persp_session *s);

/* True when there is nothing to commit (no planes, or every plane maps onto itself). */
bool pc_ui_persp_is_noop(const pc_ui_persp_session *s);

/* describe(): {"layer", "mode", "planes"}. Appends to `doc`, writes *out on success. */
pc_status pc_ui_persp_describe(const pc_ui_persp_session *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                               pc_error *err);
/* The command params {"planes", "layer"} to dispatch on commit. Sets *run false (and *out NULL)
 * when the session is a no-op. */
pc_status pc_ui_persp_commit_params(const pc_ui_persp_session *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                    bool *run, pc_error *err);

#endif
