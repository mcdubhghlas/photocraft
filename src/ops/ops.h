/* Undo/redo history (port of crates/ops).
 *
 * Pixel tiles are reference-counted copy-on-write (raster/surface.h), so a full document clone
 * costs O(layers + tiles) pointer copies, not pixel copies. The history therefore stores a
 * whole-document snapshot per step: simple, obviously correct, and what Photoshop's History
 * panel shows (one state per step).
 *
 * Snapshots are pc_doc_arc: an immutable, reference-counted document (Rust Arc<Document>).
 * Memory is bounded by max_states plus an optional pixel byte budget (max_bytes); only tiles not
 * shared with the current document count against it.
 *
 * Ownership: functions taking a `pc_doc_arc *` borrow it and retain their own reference when
 * they keep it; returned `pc_doc_arc *` out-params carry a new reference the caller releases.
 * Strings and documents returned as const pointers are borrowed until the history is next
 * mutated or freed. Labels are arbitrary bytes (pc_str), copied in.
 *
 * Failure: functions that allocate return pc_status (PC_ERR_NOMEM) and leave the history
 * exactly as it was. The tile counter behind the byte budget is a cache: if updating it runs
 * out of memory it is dropped and rebuilt by the next pc_history_trim, never reported.
 *
 * Thread safety: a pc_history is not synchronised (one owner at a time; const calls may run
 * concurrently). pc_doc_arc reference counts are atomic: snapshots may be retained, read and
 * released from any thread, but the document inside must not be mutated while shared. */
#ifndef PC_OPS_OPS_H
#define PC_OPS_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "base/str.h"
#include "doc/doc.h"

/* ---- shared documents ---- */

typedef struct pc_doc_arc pc_doc_arc;

/* A shared document holding *doc (moved: taken over bitwise and zeroed on success only). */
pc_status pc_doc_arc_new(pc_document *doc, pc_doc_arc **out, pc_error *err);
/* A shared deep copy of doc (tiles and blobs shared copy-on-write). */
pc_status pc_doc_arc_from_clone(const pc_document *doc, pc_doc_arc **out, pc_error *err);
pc_doc_arc *pc_doc_arc_retain(pc_doc_arc *a); /* NULL-safe; returns a */
void pc_doc_arc_release(pc_doc_arc *a);       /* NULL-safe; frees the document with the last reference */
const pc_document *pc_doc_arc_get(const pc_doc_arc *a);
/* Current number of references (Arc::strong_count); a hint only while other threads hold it. */
size_t pc_doc_arc_refs(const pc_doc_arc *a);
/* Arc::make_mut: *out is the document of *a for writing. When *a is shared it is first replaced
 * by a private clone (the old reference released); on failure *a is unchanged. */
pc_status pc_doc_arc_make_mut(pc_doc_arc **a, pc_document **out, pc_error *err);

/* ---- layer targets ---- */

/* The layers a state targeted: the active ("key") layer and every selected layer. Undo and redo
 * bring them back with the state's document, as the reference app does. Selecting layers is not
 * a step of its own, so the session updates the current state's target with
 * pc_history_set_current_layers just before recording the next step (#1356). A zeroed value
 * is the default (no active layer, nothing selected). */
typedef struct pc_layer_target {
    bool has_active;
    pc_layer_id active;
    pc_layer_id *selected; /* NULL when nselected is 0 */
    size_t nselected;
} pc_layer_target;

void pc_layer_target_fini(pc_layer_target *t); /* NULL-safe; leaves it zeroed */
pc_status pc_layer_target_clone(const pc_layer_target *t, pc_layer_target *out, pc_error *err);
bool pc_layer_target_eq(const pc_layer_target *a, const pc_layer_target *b);

/* ---- history ---- */

typedef struct pc_history pc_history;

#define PC_HISTORY_DEFAULT_MAX_STATES 50 /* History::default() */

/* An empty history labelled "Open", keeping at most max(max_states, 1) past states and no byte
 * budget. Caller frees with pc_history_free. */
pc_status pc_history_new(size_t max_states, pc_history **out, pc_error *err);
void pc_history_free(pc_history *h); /* NULL-safe; releases every snapshot */
/* Independent copy (snapshots shared). */
pc_status pc_history_clone(const pc_history *h, pc_history **out, pc_error *err);

/* The reference's public fields. Changing them takes effect at the next record/trim. A
 * max_states of 0 (only reachable here) keeps no past state at all, as in the reference. */
size_t pc_history_max_states(const pc_history *h);
void pc_history_set_max_states(pc_history *h, size_t n);
/* Pixel memory budget in bytes for the current document plus the tiles only the history holds
 * (0 = unlimited). pc_history_trim drops the oldest states beyond it. */
size_t pc_history_max_bytes(const pc_history *h);
void pc_history_set_max_bytes(pc_history *h, size_t n);

/* Record that `before` was replaced by a new current document via step `label` (len bytes),
 * which left `layers` targeted (NULL = the default target). Retains `before`; clears redo;
 * drops the oldest states beyond max_states. */
pc_status pc_history_record(pc_history *h, const char *label, size_t len, pc_doc_arc *before,
                            const pc_layer_target *layers, pc_error *err);

bool pc_history_can_undo(const pc_history *h);
bool pc_history_can_redo(const pc_history *h);
/* Label of the step undo would revert (the current one), or NULL when nothing to undo. */
const pc_str *pc_history_undo_label(const pc_history *h);
/* Label of the next redo, or NULL. */
const pc_str *pc_history_redo_label(const pc_history *h);
/* The redo states, the next redo first (the History panel's greyed rows): count, and the label
 * of redo i (NULL past the end). */
size_t pc_history_redo_count(const pc_history *h);
const pc_str *pc_history_redo_label_at(const pc_history *h, size_t i);

/* Set the layers the current document targets from its creation on: when it is first opened,
 * or when its step coalesces or changes the selection after recording (copied). */
pc_status pc_history_set_current_layers(pc_history *h, const pc_layer_target *layers, pc_error *err);

/* Undo: `current` (retained) becomes redoable; *out_doc is the document to make current (a new
 * reference) and *out_layers (optional) the layers it targeted when it was created, which the
 * caller finalises. With nothing to undo it returns PC_OK with *out_doc = NULL and *out_layers
 * zeroed, and keeps no reference to current. */
pc_status pc_history_undo(pc_history *h, pc_doc_arc *current, pc_doc_arc **out_doc,
                          pc_layer_target *out_layers, pc_error *err);
/* Redo: the mirror of pc_history_undo. Like the reference, it doesn't apply max_states. */
pc_status pc_history_redo(pc_history *h, pc_doc_arc *current, pc_doc_arc **out_doc,
                          pc_layer_target *out_layers, pc_error *err);

/* Entries for a History panel: past labels oldest -> newest, then the current label.
 * Count = past_len + 1; entry i is borrowed, NULL past the end. */
size_t pc_history_entry_count(const pc_history *h);
const pc_str *pc_history_entry(const pc_history *h, size_t i);
/* Document of past entry i (0 = oldest), borrowed (retain to keep it). The last entry is the
 * current document, which the history does not hold, so it and any later index give NULL. */
pc_doc_arc *pc_history_state(const pc_history *h, size_t i);
/* Number of past states (entries before the current one). */
size_t pc_history_past_len(const pc_history *h);

/* Rename the current step (e.g. after folding several steps into one with purge_last). */
pc_status pc_history_set_current_label(pc_history *h, const char *label, size_t len, pc_error *err);
/* Forget the most recent undo state and every redo state (Edit > Purge > Undo): the last step
 * can no longer be undone and its pixels are released. False when there was no undo state
 * (the redo states are cleared anyway). */
bool pc_history_purge_last(pc_history *h);
/* Forget the redo states, e.g. after undoing half of a compound step that failed. */
void pc_history_clear_redo(pc_history *h);
/* Forget every past and redo state (labels and the byte budget stay). */
void pc_history_clear(pc_history *h);

/* Approximate unique pixel bytes held by the history (tiles not shared with current). */
pc_status pc_history_unique_bytes(const pc_history *h, const pc_document *current, size_t *out,
                                  pc_error *err);
/* Pixel bytes of current plus the tiles only the history holds (what trim bounds by
 * max_bytes), saturating at SIZE_MAX. */
pc_status pc_history_pixel_bytes(const pc_history *h, const pc_document *current, size_t *out, pc_error *err);
/* Keep pixel memory within max_bytes: the current document's tiles plus the tiles only the
 * history holds (newest states first). The oldest undo states that don't fit are dropped; the
 * most recent one is always kept so the last step can be undone. *dropped (optional) receives
 * how many states were dropped. On failure nothing was dropped. */
pc_status pc_history_trim(pc_history *h, const pc_document *current, size_t *dropped, pc_error *err);

#endif
