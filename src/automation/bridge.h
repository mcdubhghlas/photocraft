/* Client for the desktop app's JSON-lines control protocol (port of bridge.rs): one JSON request
 * per line, replies matched by `id`. Blocking + poll() (REWRITE_PLAN.md D14), one cached loopback
 * connection reconnected after a failure or a long idle. The app itself arrives in Phase 8; this
 * client lets `photocraft-cli mcp --bridge` drive it. */
#ifndef PC_AUTOMATION_BRIDGE_H
#define PC_AUTOMATION_BRIDGE_H

#include <stdbool.h>

#include <yyjson.h>

#include "base/json.h"
#include "base/status.h"

typedef struct pc_auto_bridge pc_auto_bridge;

/* `addr` such as "127.0.0.1:7878" (loopback only). */
pc_status pc_auto_bridge_new(const char *addr, const char *token, pc_auto_bridge **out, pc_error *err);
void pc_auto_bridge_free(pc_auto_bridge *b); /* NULL-safe */
const char *pc_auto_bridge_addr(const pc_auto_bridge *b);

/* Call a control method; *out (owned) gets its `result`, or the app's error fills *err. `read`
 * marks a method the caller knows only reads state (safe to retry once after a transport failure).
 * `params` may be NULL. */
pc_status pc_auto_bridge_call(pc_auto_bridge *b, const char *method, yyjson_val *params, bool read,
                              pc_json_value *out, pc_error *err);

#endif /* PC_AUTOMATION_BRIDGE_H */
