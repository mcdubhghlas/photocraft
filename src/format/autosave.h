/* Autosave and crash recovery (port of crates/format/src/autosave.rs, architecture §9): the
 * incremental writer saves a document snapshot into a recovery directory on a background
 * thread, so the UI never blocks.
 *
 * Layout: <recovery_dir>/<key>.pcraft/ (directory bundles, so repeated autosaves write only
 * changed tiles) plus <key>.json, the pc_recovery_info sidecar. Keys are sanitised to ASCII
 * letters, digits, '-' and '_' (every other character becomes '_'; empty is "untitled"), so a
 * key can never name a path outside the directory.
 *
 * pc_recovery_store is the app-facing lifecycle: an entry is only deleted once the document is
 * saved or closed, never just because it was recovered.
 *
 * Errors carry the reference's texts (FormatError's Display: "I/O: ...", ...). Thread safety: a
 * pc_autosaver and a pc_recovery_store may be used from one thread at a time (their workers run
 * on their own threads); everything else is thread-safe. */
#ifndef PC_FORMAT_AUTOSAVE_H
#define PC_FORMAT_AUTOSAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "base/str.h"
#include "doc/doc.h"
#include "format/store.h"

/* The sidecar describing an autosave (RecoveryInfo). */
typedef struct pc_recovery_info {
    pc_str key;
    pc_str document_name;
    bool has_original_path; /* where the user last saved the document, if anywhere */
    pc_str original_path;
    uint64_t saved_at; /* seconds since the Unix epoch */
    uint64_t revision;
} pc_recovery_info;

void pc_recovery_info_fini(pc_recovery_info *i); /* NULL-safe */
/* serde_json::to_vec_pretty of the info (*out replaced). */
pc_status pc_recovery_info_to_json(const pc_recovery_info *i, pc_buf *out, pc_error *err);
/* serde_json::from_slice: unknown fields ignored, original_path may be missing or null. */
pc_status pc_recovery_info_from_json(const uint8_t *json, size_t len, pc_recovery_info *out, pc_error *err);

/* The sanitised form of a key (*out replaced): ASCII alphanumerics, '-' and '_' kept, every
 * other character (a whole UTF-8 sequence, or one invalid byte) becomes '_'; empty ->
 * "untitled". */
pc_status pc_autosave_sanitize_key(const char *key, size_t len, pc_str *out, pc_error *err);

/* One recoverable document found by pc_list_recovery. */
typedef struct pc_recovery_entry {
    pc_recovery_info info;
    char *bundle; /* <dir>/<key>.pcraft (owned) */
} pc_recovery_entry;

void pc_recovery_entries_free(pc_recovery_entry *e, size_t n); /* NULL-safe */

/* Recoverable documents in dir, newest first (ties by key). A sidecar counts only when it
 * parses, its key is already sanitised and equals the file's stem, and <key>.pcraft/manifest.json
 * is a file. A missing or unreadable directory gives none. *out is NULL when *n is 0. Fails
 * only when out of memory. */
pc_status pc_list_recovery(const char *dir, pc_recovery_entry **out, size_t *n, pc_error *err);
/* Load a recovered document (*out initialised on success). */
pc_status pc_recover(const pc_recovery_entry *e, pc_document *out, pc_error *err);
/* Delete a recovery entry (its bundle and sidecar). */
pc_status pc_discard_recovery(const char *dir, const pc_recovery_entry *e, pc_error *err);

/* ---- one document's background autosaver ----
 *
 * Requests are coalesced: if saves arrive faster than they complete, only the newest snapshot is
 * written. A thread that could not start is reported through pc_autosaver_last_result (and
 * every request), never silently ignored. */
typedef struct pc_autosaver pc_autosaver;

/* key identifies the document across autosaves (it is sanitised). Fails only when out of memory. */
pc_status pc_autosaver_new(const char *recovery_dir, const char *key, size_t key_len, pc_autosaver **out,
                           pc_error *err);
/* Stops the worker after its pending save, then frees. NULL-safe. */
void pc_autosaver_free(pc_autosaver *a);
/* <dir>/<key>.pcraft (borrowed). */
const char *pc_autosaver_bundle_path(const pc_autosaver *a);

/* Queue a snapshot of doc (a copy-on-write clone; doc stays the caller's) and return at once.
 * original_path may be NULL; opts may be NULL (its images are copied). Success means queued,
 * not written: pc_autosaver_take_completed reports each write. Fails with "autosave worker is
 * stopped" after a flush, or the thread's start failure ("autosave worker is unavailable" when
 * none was recorded). */
pc_status pc_autosaver_request(pc_autosaver *a, const pc_document *doc, uint64_t revision,
                               const char *original_path, const pc_format_save_opts *opts, pc_error *err);

/* One completed write: its revision and outcome (msg is the error when !ok). */
typedef struct pc_autosave_outcome {
    uint64_t revision;
    bool ok;
    char msg[PC_ERROR_MSG_MAX];
} pc_autosave_outcome;

/* Drains the completed writes without waiting (*out is malloc'd, or NULL when *n is 0). */
pc_status pc_autosaver_take_completed(pc_autosaver *a, pc_autosave_outcome **out, size_t *n, pc_error *err);

/* The most recent completed save: false when none ran yet; else *ok and either *stats or the
 * error in *msg (each may be NULL). */
bool pc_autosaver_last_result(pc_autosaver *a, bool *ok, pc_format_save_stats *stats, pc_error *msg);
/* Finish pending saves, stop the thread, then report as pc_autosaver_last_result. Later
 * requests fail. */
bool pc_autosaver_flush(pc_autosaver *a, bool *ok, pc_format_save_stats *stats, pc_error *msg);
/* The document was saved normally or closed: stop the worker and drop its recovery data. */
pc_status pc_autosaver_discard(pc_autosaver *a, pc_error *err);

/* ---- the autosaves of one app session (RecoveryStore) ----
 *
 * New documents save under "doc-<session>-<id>", with a session unique to this store: ids
 * restart every launch, so a new document never overwrites an entry an earlier launch left. A
 * recovered document adopts the entry it was loaded from; its autosaves replace that entry in
 * place and saving or closing it removes the entry. */
typedef struct pc_recovery_store pc_recovery_store;

pc_status pc_recovery_store_new(const char *recovery_dir, pc_recovery_store **out, pc_error *err);
/* Stops every worker (after their pending saves), then frees. NULL-safe. */
void pc_recovery_store_free(pc_recovery_store *s);

typedef struct pc_recovered {
    pc_recovery_entry entry;
    pc_document doc;
} pc_recovered;

/* Loads every recoverable document, newest first; entries that fail to load are skipped and
 * stay on disk. Nothing is deleted. */
pc_status pc_recovery_store_recover(const pc_recovery_store *s, pc_recovered **out, size_t *n, pc_error *err);
void pc_recovered_free(pc_recovered *r, size_t n); /* NULL-safe */
/* Document doc_id was opened from the entry `key`: its autosaves now replace that entry. */
pc_status pc_recovery_store_adopt(pc_recovery_store *s, uint64_t doc_id, const char *key, pc_error *err);
/* Queue an autosave of doc (cloned), keyed by doc->id. */
pc_status pc_recovery_store_autosave(pc_recovery_store *s, const pc_document *doc, uint64_t revision,
                                     const char *original_path, pc_error *err);

typedef struct pc_recovery_outcome {
    uint64_t doc_id;
    pc_autosave_outcome outcome;
} pc_recovery_outcome;

/* Completed writes of every document, without blocking (*out malloc'd or NULL). */
pc_status pc_recovery_store_take_completed(pc_recovery_store *s, pc_recovery_outcome **out, size_t *n,
                                           pc_error *err);
/* Document doc_id was saved or closed: remove its own autosaves and the entry it adopted. The
 * first failure is reported (its own entry's first). */
pc_status pc_recovery_store_discard(pc_recovery_store *s, uint64_t doc_id, pc_error *err);

#endif
