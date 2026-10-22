/* Shell wiring for the four warp editors (U8): Perspective Warp, Puppet Warp, Liquify and Adaptive
 * Wide Angle. The portable editor models (src/ui/persp_warp.*, puppet_warp.*, liquify_dialog.*,
 * wide_angle.*) hold all the maths, undo/redo and command JSON; this layer begins them from the
 * engine commands, routes SDL pointer/key input to the active model, draws the on-canvas overlay
 * with Nuklear, builds the options bar and dispatches the committed command as one undo step.
 *
 * Perspective and Puppet warp edit on the canvas (transform-style); Liquify and Adaptive Wide Angle
 * are modal: while any editor is active the normal tools are suspended and Enter commits / Esc
 * cancels. Never crashes: pointer input is validated by the models, overlay coordinates are clipped
 * to the 16-bit Nuklear range and every count is bounded. */
#ifndef PC_UI_SHELL_WARP_UI_H
#define PC_UI_SHELL_WARP_UI_H

#include "shell_internal.h"
#include "ui/canvas.h"
#include "ui/liquify_dialog.h"
#include "ui/persp_warp.h"
#include "ui/puppet_warp.h"
#include "ui/wide_angle.h"

/* Which editor, if any, owns the canvas. */
typedef enum pc_ui_warp_kind {
    PC_UI_WARP_NONE = 0,
    PC_UI_WARP_PERSP,
    PC_UI_WARP_PUPPET,
    PC_UI_WARP_LIQUIFY,
    PC_UI_WARP_WIDE
} pc_ui_warp_kind;

typedef struct pc_ui_warp_runtime {
    pc_ui_warp_kind kind;
    pc_ui_persp_session persp;
    pc_ui_puppet_session puppet;
    pc_ui_liquify_dialog liquify;
    pc_ui_wide_dialog wide;
    bool dragging;         /* a pointer drag is in progress */
    bool has_cursor;       /* cursor[] holds a document-space pointer (brush preview) */
    double cursor[2];
    uint64_t document;     /* active document id when the editor began (cancel on change) */
} pc_ui_warp_runtime;

/* The warp runtime lives in the shell app (defined in app.c). */
pc_ui_warp_runtime *pc_ui_shell_warp_runtime(pc_ui_shell_app *app);
void pc_ui_shell_warp_fini(pc_ui_warp_runtime *r); /* NULL-safe */

/* True while any warp editor owns the canvas. */
bool pc_ui_shell_warp_active(pc_ui_shell_app *app);

/* Command front end: begins an editor (edit.perspectiveWarp / edit.puppetWarp / filter.liquify /
 * filter.adaptiveWideAngle and their smart-object variants) on a menu click, and handles the
 * ui.warp.* control ids plus edit.undo/redo while active. Returns PC_ERR_NOT_FOUND when `id` is not
 * handled here (so the caller tries the engine command directly, e.g. a scripted call with params). */
pc_status pc_ui_shell_warp_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                   pc_json_value *out, pc_error *err);

/* Routes a document-space pointer event to the active editor. Returns PC_ERR_NOT_FOUND when no
 * editor is active. */
pc_status pc_ui_shell_warp_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                   unsigned modifiers, pc_json_value *out, pc_error *err);

/* Routes a canvas SDL mouse event to the active editor (translating to document coordinates and
 * dispatching through the shell pointer path). Returns true when the event is consumed. */
bool pc_ui_shell_warp_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                             float factor);

/* Enter commits, Esc cancels the active editor. Returns true when the key is consumed. */
bool pc_ui_shell_warp_key(pc_ui_shell_app *app, const SDL_Event *event);

/* Draws the active editor's options bar; returns true when an editor is active (so the generic
 * options bar is skipped). */
bool pc_ui_shell_warp_options(pc_ui_shell_app *app);

/* Draws the active editor's on-canvas overlay (planes/grid, mesh/pins, brush cursor, arcs). */
void pc_ui_shell_warp_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf);

#endif /* PC_UI_SHELL_WARP_UI_H */
