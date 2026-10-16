/* Capability-based filesystem policy for untrusted automation paths (port of workspace.rs).
 *
 * Root paths are trusted launch configuration, held as directory file descriptors; request paths
 * are untrusted forward-slash relative paths resolved beneath the held handle (REWRITE_PLAN.md
 * D16: openat / O_NOFOLLOW / O_DIRECTORY), or absolute paths whose text lies beneath the root.
 * Parent traversal, alternate separators, prefixes and empty components are rejected before I/O. */
#ifndef PC_AUTOMATION_WORKSPACE_H
#define PC_AUTOMATION_WORKSPACE_H

#include <stddef.h>

#include <yyjson.h>

#include "base/buf.h"
#include "base/status.h"

typedef struct pc_auto_root {
    int fd;      /* dirfd of the root, or -1 when absent */
    char *path;  /* absolute path of the root (owned), or NULL */
} pc_auto_root;

typedef struct pc_auto_workspace {
    pc_auto_root read;
    pc_auto_root write;
} pc_auto_workspace;

/* Open the configured roots as capabilities. Either authority may be NULL (fails closed). */
pc_status pc_auto_workspace_init(pc_auto_workspace *ws, const char *read_root, const char *write_root,
                                 pc_error *err);
void pc_auto_workspace_fini(pc_auto_workspace *ws); /* NULL-safe */

/* Read one regular file below the read root into *out (appended). */
pc_status pc_auto_workspace_read(const pc_auto_workspace *ws, const char *path, pc_buf *out, pc_error *err);
/* Create or replace one file below the write root, crash-safely (temp + fsync + rename). */
pc_status pc_auto_workspace_write(const pc_auto_workspace *ws, const char *path, const void *bytes,
                                  size_t len, pc_error *err);

/* ---- command authorization (the Session::authorize hook) ---- */

/* Refuse automation commands that use ambient filesystem paths; use capability-scoped document
 * methods instead. PC_OK when the command is allowed. */
pc_status pc_auto_authorize_engine_command(const char *id, yyjson_val *params, pc_error *err);
/* As above plus the desktop-session restrictions (image.mode.* resolve ambient profiles). */
pc_status pc_auto_authorize_desktop_engine_command(const char *id, yyjson_val *params, pc_error *err);

#endif /* PC_AUTOMATION_WORKSPACE_H */
