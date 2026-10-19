#ifndef PC_ENGINE_PREVIEW_H
#define PC_ENGINE_PREVIEW_H
#include "engine/engine.h"
/* Independent session for preview work. Documents retain COW tiles and profiles; targets,
 * selections, preferences and working spaces match the source. No source history, jobs,
 * journal, preset-store writes or file paths are carried over. Clone on the session's owner
 * thread; the returned session may then be handed to one worker. Restricted sources yield
 * a clone that rejects execution: authorizers can depend on their owner thread. A synchronous
 * engine adapter may restore the source authorizer before dispatching its constituent commands. */
pc_status pc_engine_preview_clone(const pc_engine_session *source, pc_engine_session **out, pc_error *err);
/* Owner-thread document preview with the source policy retained for every dispatched command.
 * Restricted policy callbacks must not be called from worker threads through this adapter. */
pc_status pc_engine_preview_sync(const pc_engine_session *source, const char *command, yyjson_val *params,
                                 pc_doc_arc **out, pc_error *err);
/* Apply a document-only command on a worker; commit its COW result with normal job locking,
 * history, journal and hooks. File operations and multi-document mutations are excluded.
 * Restricted sessions execute inline on their owner thread, retaining all authorization checks. */
bool pc_engine_preview_job_command(const char *command);
pc_status pc_engine_preview_start_job(pc_engine_session *session, const char *command, yyjson_val *params,
                                      bool *is_job, uint64_t *id, pc_json_value *out, pc_error *err);
#endif
