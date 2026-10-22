/* Edit > Puppet Warp UI model (clean-room port of crates/ui-egui/src/puppet_ui.rs).
 *
 * The on-canvas session driving `edit.puppetWarp` / `layer.smartObjects.puppetWarp`: pins on an
 * ARAP triangle mesh over the layer's opaque region. Pure data over the `pc_algo_puppet_*`
 * primitives; no GL/SDL/Nuklear. Coordinates are document pixels. The shell converts screen <->
 * document, supplies the hit tolerance, owns the preview textures, draws the deformed mesh + pins
 * from the solver/deformed arrays, and dispatches the command built by pc_ui_puppet_commit_params.
 *
 * Session-local pin undo/redo is kept here (independent of document history). Never crashes:
 * density/expansion that would empty the mesh are refused and the previous state restored (#951);
 * hostile coordinates and indices are bounded. */
#ifndef PC_UI_PUPPET_WARP_H
#define PC_UI_PUPPET_WARP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "algo/puppet.h"
#include "base/status.h"
#include "raster/surface.h"
#include "ui/tool_gesture.h"

/* A snapshot of the pin list for the session-local undo/redo stacks. */
typedef struct pc_ui_puppet_snapshot {
    pc_algo_puppet_pin *pins;
    size_t n;
} pc_ui_puppet_snapshot;

typedef struct pc_ui_puppet_session {
    bool active;
    uint64_t key, layer;
    char command[64];
    double bounds[4];
    pc_surface surface;        /* owned clone of the layer pixels */
    pc_algo_puppet_warp warp;  /* pins + mode + density + expansion */
    bool show_mesh;
    bool has_selected;
    size_t selected;
    bool has_drag;
    size_t drag;
    bool has_drag_from;
    pc_ui_puppet_snapshot drag_from;

    pc_algo_puppet_solver *solver; /* owns the current mesh */
    double (*deformed)[2];         /* nverts live vertex positions */
    size_t ndeformed;
    size_t *order; /* ntris triangle draw order (back to front) */
    size_t norder;

    pc_ui_puppet_snapshot *undo;
    size_t nundo, cap_undo;
    pc_ui_puppet_snapshot *redo;
    size_t nredo, cap_redo;
} pc_ui_puppet_session;

/* Begins a session over the layer `surface` (cloned) and its content `bounds`. Default warp
 * (Normal/Normal/expansion 2), show_mesh true. Fails PC_ERR_INVALID if the mesh is empty (no
 * opaque pixels). */
pc_status pc_ui_puppet_begin(pc_ui_puppet_session *s, uint64_t key, uint64_t layer, const char *command,
                             const double bounds[4], const pc_surface *surface, pc_error *err);
void pc_ui_puppet_fini(pc_ui_puppet_session *s); /* NULL-safe */

/* One pointer event (document coords). `tol` is the hit radius in document px, `alt` the remove
 * modifier. Adds/drags/removes pins and keeps the solve up to date. */
pc_status pc_ui_puppet_pointer(pc_ui_puppet_session *s, pc_ui_pointer_kind kind, double x, double y,
                               double tol, bool alt, pc_error *err);

pc_status pc_ui_puppet_set_mode(pc_ui_puppet_session *s, pc_algo_puppet_mode mode, pc_error *err);
/* Changes density/expansion; refuses and restores the previous setting if the mesh empties (#951). */
pc_status pc_ui_puppet_set_density(pc_ui_puppet_session *s, pc_algo_puppet_density density, pc_error *err);
pc_status pc_ui_puppet_set_expansion(pc_ui_puppet_session *s, double expansion, pc_error *err);
void pc_ui_puppet_set_show_mesh(pc_ui_puppet_session *s, bool on);
/* Options bar edits of the selected pin (no-op without a selection). */
pc_status pc_ui_puppet_nudge_depth(pc_ui_puppet_session *s, int delta, pc_error *err);
pc_status pc_ui_puppet_set_rotation(pc_ui_puppet_session *s, bool fixed, double deg, pc_error *err);
pc_status pc_ui_puppet_remove_all(pc_ui_puppet_session *s, pc_error *err);

bool pc_ui_puppet_undo(pc_ui_puppet_session *s); /* returns whether anything changed */
bool pc_ui_puppet_redo(pc_ui_puppet_session *s);

bool pc_ui_puppet_is_noop(const pc_ui_puppet_session *s);

pc_status pc_ui_puppet_describe(const pc_ui_puppet_session *s, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                pc_error *err);
pc_status pc_ui_puppet_commit_params(const pc_ui_puppet_session *s, yyjson_mut_doc *doc,
                                     yyjson_mut_val **out, bool *run, pc_error *err);

#endif
