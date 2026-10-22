/* The desktop app's localhost JSON-lines control server (one request per line, one reply per line,
 * loopback only). Behavioural port of apps/photocraft/src/control_server.rs.
 *
 * Unlike the headless automation server (src/automation/rpc.c, which drives its own pc_headless on a
 * worker thread), this server drives the LIVE GUI app, whose state must only be touched on the main
 * thread. So the accept/serve work runs on background threads, but each request is handed to the
 * main thread through a mailbox and executed by pc_app_control_server_pump (called once per frame),
 * which invokes the caller's dispatch callback (the shell's pc_ui_control_handle). Framing, token
 * authentication, connection limits and the oversized/too-slow reply handling reuse the shared
 * primitives in src/automation (security.c, budgets.c). */
#ifndef PC_APP_CONTROL_SERVER_H
#define PC_APP_CONTROL_SERVER_H

#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/json.h"
#include "base/status.h"

/* How long a background connection waits for the main thread to answer before telling the client the
 * UI did not reply in time (the command may still be applying). */
#define PC_APP_CONTROL_REPLY_DEADLINE_SECS 60

/* Dispatch one control request ON THE MAIN THREAD. Writes *out (an owned pc_json_value the caller
 * frees) only on success; on failure fills `err` and leaves *out untouched. Must never crash. This
 * is exactly the contract of pc_ui_control_handle. */
typedef pc_status (*pc_app_control_dispatch)(void *ctx, const char *method, yyjson_val *params,
                                             pc_json_value *out, pc_error *err);

typedef struct pc_app_control_server pc_app_control_server;

/* Start a control server on 127.0.0.1:<port> (port 0 picks a free one). `token` must be a 64-hex
 * bearer token. On success *out owns the server and *bound_port (may be NULL) gets the real port. */
pc_status pc_app_control_server_start(uint16_t port, const char *token,
                                      pc_app_control_dispatch dispatch, void *dispatch_ctx,
                                      pc_app_control_server **out, uint16_t *bound_port, pc_error *err);

/* Drain and dispatch any queued requests on the CALLING (main) thread. Returns how many it handled.
 * Safe to call when there are none. */
size_t pc_app_control_server_pump(pc_app_control_server *s);

/* Register a wake callback, invoked from a background thread when a request is queued, so a blocked
 * main loop can wake and pump. `wake` may be NULL to clear it. */
void pc_app_control_server_on_wake(pc_app_control_server *s, void (*wake)(void *ud), void *ud);

/* The port the server is listening on. */
uint16_t pc_app_control_server_port(const pc_app_control_server *s);

/* Stop the server and free it (NULL-safe). Joins its threads. */
void pc_app_control_server_stop(pc_app_control_server *s);

#endif /* PC_APP_CONTROL_SERVER_H */
