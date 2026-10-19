/* The ui.* control-channel handler (reimplementation of the ui-chapter half of
 * crates/ui-egui/src/control.rs). The automation bridge (src/automation) forwards ui.* methods to a
 * running app over TCP; the desktop app's control server (Phase 9 P1) parses the JSON-lines envelope
 * and calls this handler. It drives the PhotocraftApp + UiState and matches the method names and
 * result shapes of docs/control-protocol.md and the src/automation rpc shapes.
 *
 * SCOPE (U2/U3): the methods a headless app can serve without the later UI chunks' state —
 * ui.inspect, ui.menu.list, ui.menu.invoke / engine.execute, ui.set (the fields UiState models),
 * ui.pointer (validated), ui.resize, ui.screenshot (GL readback, else a CPU image). The large
 * dialog/tool surface (ui.dialog.*, ui.gpu.*, app.open/save, camera-raw) is added as its state
 * lands in U5..U10; unknown or not-yet-ported methods return a clear PC_ERR_* error, never crash.
 *
 * Convention (matches src/automation headless): returns pc_status; writes *out (an owned
 * pc_json_value the caller frees) only on success; on failure fills `err` and leaves *out untouched.
 * The ok/error JSON envelope is applied by the caller (control server / test). Never crashes for any
 * method name or params. */
#ifndef PC_UI_SHELL_CONTROL_H
#define PC_UI_SHELL_CONTROL_H

#include <yyjson.h>

#include "app.h"
#include "base/json.h"
#include "base/status.h"

/* Handle one control request. `params` may be NULL (treated as an empty object). */
pc_status pc_ui_control_handle(pc_ui_shell_app *a, const char *method, yyjson_val *params, pc_json_value *out,
                               pc_error *err);

/* The UI state as ui.inspect reports it (the scoped subset). Exposed for the control handler and
 * tests. Writes *out only on success. */
pc_status pc_ui_control_inspect(pc_ui_shell_app *a, pc_json_value *out, pc_error *err);

#endif /* PC_UI_SHELL_CONTROL_H */
