/* Agent-facing automation (REWRITE_PLAN.md Phase 7, A1/A2; port of crates/automation, layer L6).
 *
 * The automation layer drives a headless photocraft_engine Session (no UI) through the control
 * protocol's JSON envelope and the MCP tool protocol. It carries NO UI-toolkit dependency.
 *
 * Error model (port of AutomationError). The crate's error enum maps to pc_status + pc_error text
 * (REWRITE_PLAN.md §4); the Display text that tests and the control protocol expose is reproduced
 * verbatim. The helpers below fill *err and evaluate to the status, like base's pc_fail:
 *
 *   BadRequest(s) -> PC_ERR_INVALID  "<s>"           pc_auto_bad
 *   Io(s)         -> PC_ERR_IO        "I/O: <s>"      pc_auto_io
 *   Bridge(s)     -> PC_ERR_INVALID   "bridge: <s>"   pc_auto_bridge
 *   App(s)        -> PC_ERR_INVALID   "app: <s>"      pc_auto_app
 *   Other(s)      -> PC_ERR_INVALID   "<s>"           pc_auto_other
 *   Engine/Import/Format(e) -> the status + message from the lower layer, passed through unchanged.
 */
#ifndef PC_AUTOMATION_AUTOMATION_H
#define PC_AUTOMATION_AUTOMATION_H

#include "base/status.h"

/* BadRequest: a caller mistake or hostile request. */
#define pc_auto_bad(err, ...) (pc_fail_set((err), PC_ERR_INVALID, __VA_ARGS__), (pc_status)PC_ERR_INVALID)
/* Other: an internal failure reported as plain text. */
#define pc_auto_other(err, ...) (pc_fail_set((err), PC_ERR_INVALID, __VA_ARGS__), (pc_status)PC_ERR_INVALID)

/* I/O: <msg> (the Rust AutomationError::Io Display). */
void pc_auto_io_set(pc_error *err, const char *fmt, ...);
#define pc_auto_io(err, ...) (pc_auto_io_set((err), __VA_ARGS__), (pc_status)PC_ERR_IO)
/* bridge: <msg> */
void pc_auto_bridge_set(pc_error *err, const char *fmt, ...);
#define pc_auto_bridge(err, ...) (pc_auto_bridge_set((err), __VA_ARGS__), (pc_status)PC_ERR_INVALID)
/* app: <msg> */
void pc_auto_app_set(pc_error *err, const char *fmt, ...);
#define pc_auto_app(err, ...) (pc_auto_app_set((err), __VA_ARGS__), (pc_status)PC_ERR_INVALID)

#endif /* PC_AUTOMATION_AUTOMATION_H */
