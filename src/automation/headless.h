/* Headless backend (port of headless.rs): a photocraft_engine session plus file I/O. Synchronous
 * and UI-free; the MCP server and the CLI both drive it. Also hosts the JSON-lines dispatch
 * (handle/batch, port of rpc.rs's Headless methods). */
#ifndef PC_AUTOMATION_HEADLESS_H
#define PC_AUTOMATION_HEADLESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "automation/budgets.h"
#include "automation/workspace.h"
#include "base/json.h"
#include "base/status.h"
#include "engine/engine.h"
#include "format/store.h"
#include "io/core.h"

typedef enum pc_auto_fs_mode {
    PC_AUTO_FS_DENIED,        /* no filesystem authority */
    PC_AUTO_FS_TRUSTED_LOCAL, /* an explicit local CLI invocation (ambient host paths) */
    PC_AUTO_FS_WORKSPACE      /* capability-scoped file access */
} pc_auto_fs_mode;

typedef struct pc_auto_writer_entry {
    uint64_t doc_id;
    pc_pcraft_writer *writer;
} pc_auto_writer_entry;

typedef struct pc_headless {
    pc_engine_session *session;
    pc_auto_writer_entry *writers;
    size_t nwriters, cap_writers;
    pc_auto_fs_mode fs;
    pc_auto_workspace workspace; /* valid when fs == PC_AUTO_FS_WORKSPACE */
} pc_headless;

/* Create an automation session with no filesystem authority. */
pc_status pc_headless_new(pc_headless **out, pc_error *err);
/* A session for an explicit local CLI invocation (ambient host paths; nested steps not re-checked). */
pc_status pc_headless_trusted_local(pc_headless **out, pc_error *err);
/* A session with capability-scoped file access; takes ownership of *ws (moved, zeroed). */
pc_status pc_headless_with_workspace(pc_auto_workspace *ws, pc_headless **out, pc_error *err);
void pc_headless_free(pc_headless *h); /* NULL-safe */

/* Apply background jobs that finished since the last request. Cheap when no job runs. */
void pc_headless_sync_jobs(pc_headless *h);

/* ---- requests (each writes *out, an owned pc_json_value, only on success) ---- */

pc_status pc_headless_open(pc_headless *h, const char *path, pc_json_value *out, pc_error *err);
/* index < 0 means the active document. format/opts may be NULL. */
pc_status pc_headless_save(pc_headless *h, int64_t index, const char *path, const char *format,
                           const pc_io_export_opts *opts, pc_json_value *out, pc_error *err);
pc_status pc_headless_inspect(pc_headless *h, int64_t index, pc_json_value *out, pc_error *err);
pc_status pc_headless_render_png(pc_headless *h, int64_t index, uint32_t max_side, pc_buf *png, pc_error *err);
pc_status pc_headless_session_list(pc_headless *h, pc_json_value *out, pc_error *err);
pc_status pc_headless_select(pc_headless *h, size_t index, pc_json_value *out, pc_error *err);
pc_status pc_headless_close(pc_headless *h, int64_t index, pc_json_value *out, pc_error *err);
pc_status pc_headless_command_list(pc_headless *h, pc_json_value *out, pc_error *err);
/* Run a command (params may be NULL). wait=false lets a job-capable command run in the background. */
pc_status pc_headless_command_start(pc_headless *h, const char *id, yyjson_val *params, bool wait,
                                    pc_json_value *out, pc_error *err);
/* Write rendered bytes through the configured write authority. */
pc_status pc_headless_write_render(pc_headless *h, const char *path, const void *bytes, size_t len,
                                   pc_error *err);

/* Dispatch one control-protocol request (method + params); unknown methods / bad params are
 * errors, never crashes. *out gets the owned result on PC_OK. */
pc_status pc_headless_handle(pc_headless *h, const char *method, yyjson_val *params, pc_json_value *out,
                             pc_error *err);
/* Run `steps` in order with a reply budget (port of batch_with_budget). */
pc_status pc_headless_batch(pc_headless *h, yyjson_val *params, pc_auto_batch_budget budget,
                            pc_json_value *out, pc_error *err);

/* The JSON-lines methods served by handle(), NULL-terminated (port of METHODS). */
extern const char *const pc_headless_methods[];

#endif /* PC_AUTOMATION_HEADLESS_H */
