/* The MCP server (port of server.rs + the server submodules). Own JSON-RPC over stdio with poll(), no async
 * runtime (REWRITE_PLAN.md D14). Tool names use snake_case. Drives an in-process headless engine
 * session, or forwards to a running desktop app over the control protocol (bridge mode). */
#ifndef PC_AUTOMATION_SERVER_H
#define PC_AUTOMATION_SERVER_H

#include "automation/bridge.h"
#include "automation/headless.h"
#include "automation/workspace.h"
#include "base/status.h"

typedef struct pc_auto_mcp pc_auto_mcp;

/* Headless backend with no filesystem authority. */
pc_status pc_auto_mcp_headless(pc_auto_mcp **out, pc_error *err);
/* Headless backend with capability-scoped access; takes ownership of *ws (moved). */
pc_status pc_auto_mcp_headless_with_workspace(pc_auto_workspace *ws, pc_auto_mcp **out, pc_error *err);
/* Bridge backend to a running app. */
pc_status pc_auto_mcp_bridge(const char *addr, const char *token, pc_auto_mcp **out, pc_error *err);
void pc_auto_mcp_free(pc_auto_mcp *m); /* NULL-safe */

/* Serve MCP over the given fds (stdio: 0/1) until the client disconnects. */
pc_status pc_auto_mcp_serve(pc_auto_mcp *m, int in_fd, int out_fd, pc_error *err);

#endif /* PC_AUTOMATION_SERVER_H */
