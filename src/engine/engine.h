/* The PhotoCraft engine (port of crates/engine, layer L5).
 *
 * Every user-visible action is a *command* with a stable id (Photoshop-style, such as
 * "edit.undo" or "layer.newAdjustmentLayer.invert") and JSON parameters. Every frontend goes
 * through the same entry point, pc_engine_session_execute: the UI, the CLI, the remote-control
 * channel and the MCP server. This is what makes the UI swappable and the app fully scriptable.
 *
 * ===========================================================================================
 * COMMAND-REGISTRATION API (read this before adding commands in a later engine chunk, E2..E12)
 * ===========================================================================================
 *
 * A command is a `pc_engine_command` value with static-lifetime strings and two function
 * pointers (`enabled` and `run`). Group related commands in a new file src/engine/<area>_cmds.c
 * that exposes
 *
 *     const pc_engine_command *pc_engine_<area>_specs(size_t *n);
 *
 * returning a pointer to a module-static array of specs and its length. Then register that module
 * by adding one line to `register_modules()` in src/engine/command.c (a small, surgical edit to a
 * shared file, exactly as the Rust `v.extend(crate::<area>::specs())` list works -- see
 * AGENTS.md section 6). The registry is built once (thread-safe, lazily) and looked up by exact
 * id; the first spec registered for an id wins, as in the reference's linear scan. Ids must match
 * crates/engine byte-for-byte: command_lookup and the UI menu catalogue depend on them.
 *
 * `run(session, params, out, err)` returns a pc_status; it writes *out (a pc_json_value the caller
 * frees) only on success. It MUST NOT crash for ANY params or document state (AGENTS.md "Never
 * crash", REWRITE_PLAN.md rule 9): validate params, bounds-check before indexing/dividing, cap
 * input-sized allocations. Return an error, never abort/assert.
 *
 * `enabled(session, reason)` returns PC_OK when the command can run now; otherwise it returns a
 * non-OK status and fills *reason with the human/agent-readable message (drives menu greying and
 * the "disabledReason" field). Use pc_engine_always for "always enabled".
 *
 * Document edits go through pc_engine_session_edit so they become one undo step; long work can run
 * on a worker thread through the jobs API (jobs.h concepts, declared below).
 */
#ifndef PC_ENGINE_ENGINE_H
#define PC_ENGINE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/json.h"
#include "base/status.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "ops/ops.h"
#include "paint/brush.h"   /* E10: the session brush (pc_paint_brush) */
#include "paint/mixer.h"   /* E10: Mixer Brush paint (pc_paint_mixer_state) */
#include "paint/presets.h" /* E10: brush presets (pc_paint_preset) */
#include "raster/surface.h"

/* ---- engine error model (port of EngineError) --------------------------------------------
 *
 * The engine reuses the project-wide pc_status / pc_error (REWRITE_PLAN.md section 4). The Rust
 * EngineError variants map to status codes, and their Display text (which tests and the control
 * protocol expose) is reproduced verbatim in the error message:
 *
 *   UnknownCommand(id)   -> PC_ERR_NOT_FOUND,  "unknown command `<id>`"
 *   Disabled(id, why)    -> PC_ERR_INVALID,    "command `<id>` is not available right now: <why>"
 *   BadParams{cmd,msg}   -> PC_ERR_INVALID,    "invalid parameters for `<cmd>`: <msg>"
 *   NoDocument           -> PC_ERR_INVALID,    "no active document"
 *   NoLayer(id)          -> PC_ERR_NOT_FOUND,  "no such layer LayerId(<n>)"
 *   Other(s)             -> PC_ERR_INVALID,    "<s>"
 *   Cancelled            -> PC_ERR_CANCELLED,  "cancelled"
 */

/* ---- commands ----------------------------------------------------------------------------- */

typedef struct pc_engine_session pc_engine_session;

/* Why a command can't run: PC_OK = it can; otherwise *reason (may be NULL) gets the message. */
typedef pc_status (*pc_engine_enabled_fn)(const pc_engine_session *s, pc_error *reason);

/* Run a command. Writes *out (an owned pc_json_value, PC_JSON null when the command has no
 * result) only on success; on failure fills err and leaves *out untouched. Never crashes. */
typedef pc_status (*pc_engine_run_fn)(pc_engine_session *s, yyjson_val *params, pc_json_value *out,
                                      pc_error *err);

/* Metadata plus implementation for one command. All string fields are static-lifetime. */
typedef struct pc_engine_command {
    const char *id;
    const char *label;
    const char *const *menu; /* menu path, e.g. {"Layer","New"}; NULL or menu_len 0 = not in menus */
    size_t menu_len;
    const char *shortcut; /* egui-style "Cmd+Shift+N", mapped per platform by the UI; NULL = none */
    const char *params;   /* human/agent-readable parameter description (JSON-ish) */
    pc_engine_enabled_fn enabled;
    pc_engine_run_fn run;
    bool journal; /* record in the session journal (false for queries) */
} pc_engine_command;

/* Lenient engine blend-name parser (case/space/underscore/parenthesis insensitive). */
bool pc_engine_blend_from_str(const char *name, size_t length, pc_blend_mode *out);

/* An "always enabled" predicate (crate::commands::always). */
pc_status pc_engine_always(const pc_engine_session *s, pc_error *reason);

/* The whole registry (command_specs): a borrowed array of *n specs, built once. */
const pc_engine_command *pc_engine_commands(size_t *n);
/* The spec for id, or NULL (crate::commands::find). First registration of an id wins. */
const pc_engine_command *pc_engine_command_find(const char *id);

/* One command.list / engine.commands row for c, given why it can't run now (reason NULL = it
 * can). Builds {id,label,menu,shortcut,params,enabled,disabledReason,needsDocument} into *out. */
pc_status pc_engine_command_list_entry(const pc_engine_session *s, const pc_engine_command *c,
                                       const char *reason, pc_json_value *out, pc_error *err);

/* ---- per-document editing state (port of DocState) ---------------------------------------- */

typedef struct pc_engine_doc_state {
    pc_doc_arc *doc;       /* shared, reference-counted document (Rust Arc<Document>) */
    pc_history *history;   /* undo/redo */
    bool has_active_layer; /* the primary ("key") layer single-layer commands act on */
    pc_layer_id active_layer;
    pc_layer_id *selected_layers; /* every selected Layers-panel layer, selection order */
    size_t nselected;
    bool has_anchor; /* Shift-click range anchor */
    pc_layer_id layer_anchor;
    char *path;              /* saved path, or NULL (owned) */
    bool source_read_only;   /* an Affinity source must not become the Save path */
    uint64_t revision;       /* bumped on every change; UIs re-render when it moves */
    uint64_t saved_revision; /* revision at the last save; is_dirty when they differ */
    bool has_last_damage;    /* area changed by the latest revision (absent = assume everything) */
    pc_rect last_damage;
    char *coalesce; /* coalescing key of the latest history step, or NULL (owned) */
    /* --- EXTENSION REGION ---------------------------------------------------------------
     * Later engine chunks add per-document view/tool state here with a small surgical edit
     * (channel_view E7, isolated_layers E5, symmetry E10, fx_collapsed/show_only E5, floating
     * E4, color_samplers, snapshots E12 ...). Each new field is cloned in doc_state_clone and
     * released in doc_state_fini (session.c). Mirrors the Rust DocState, whose fields all live
     * in lib.rs while their types come from the owning module. */
    /* E5: Select > Isolate Layers (select_extra_cmds, E7, populates it; inspect reports it). An
     * owned array (free()) of *nisolated ids, or NULL when nothing is isolated. */
    pc_layer_id *isolated_layers;
    size_t nisolated;
    /* E9: Layers panel, the effects-list disclosure triangle (fx_view_cmds,
     * layer.setEffectsExpanded). An owned array (free()) of *nfx_collapsed ids whose effects list
     * is collapsed; absence = expanded. View state, not an undo step. */
    pc_layer_id *fx_collapsed;
    size_t nfx_collapsed;
    /* E5: View > layer mask (mask_view_cmds): ⌥-click shows the active layer's mask as gray, or
     * ⇧⌥-click / `\` as a red rubylith. View state, not an undo step. In the reference this lives
     * on channel_cmds::ChannelView::layer_mask (E7); kept as its own field here so mask view does
     * not depend on the unported channel chunk (REWRITE_PLAN.md §7). mask_view_cmds::current()
     * re-validates liveness on every read, so a stale value is never reported. */
    bool has_layer_mask_view;
    pc_layer_id layer_mask_view_layer;
    int layer_mask_view_mode; /* 0 = gray, 1 = overlay (pc_engine_mask_view_mode) */
    /* E7: Channels panel view state -- the targeted channel and the eye toggles
     * (channel_cmds::ChannelView). View state, not history. NULL = Photoshop's default (composite
     * targeted, every colour channel visible, no alpha eyes). Owned; released by doc_state_fini
     * through pc_engine_channel_view_free. The type is private to channel_cmds.c; reach it through
     * the channel_cmds accessors. The layer-mask view lives in the fields above (E5), so the C
     * ChannelView carries no layer_mask member. */
    struct pc_engine_channel_view *channel_view;
    /* E10: per-document painting symmetry (symmetry_cmds::PaintingSymmetry): a straight preset or
     * a sampled Bézier path, never stored in the document's pixels or paths. NULL = no symmetry.
     * Owned; released by doc_state_fini through pc_engine_symmetry_free. The type is private to
     * symmetry_cmds.c; reach it through the symmetry_cmds accessors. */
    struct pc_engine_symmetry *symmetry;
    /* E4: Select > Float (float_cmds::Floating): the selected pixels of a layer cut out and moved
     * as a piece, while the document stays unchanged until the piece is dropped. NULL = nothing
     * floating. View state, not an undo step (the drop is the history step). Owned; released by
     * doc_state_fini through pc_engine_floating_free. The type is private to float_cmds.c. */
    struct pc_engine_floating *floating;
    /* EF (sampler_cmds.c): the Color Sampler points (sampler_cmds::color_samplers), each a document
     * pixel position [x, y]. Per-document view state: not saved to .pcraft/PSD, no history step.
     * A canvas-size change (crop, Canvas Size, rotation) moves them with their pixels and drops the
     * ones outside the new canvas (session.c edit() applies the Session sampler map). An owned array
     * (free()) of ncolor_samplers points, or NULL. */
    double (*color_samplers)[2];
    size_t ncolor_samplers, cap_color_samplers;
    struct pc_engine_snapshot *snapshots; /* History panel, session only; COW documents */
    size_t nsnapshots, cap_snapshots;
    uint64_t next_snapshot;
} pc_engine_doc_state;

/* How a shown layer mask is drawn (mask_view_cmds::MaskViewMode). */
typedef enum pc_engine_mask_view_mode {
    PC_ENGINE_MASK_VIEW_GRAY = 0,   /* the mask alone, as a grayscale image (⌥-click) */
    PC_ENGINE_MASK_VIEW_OVERLAY = 1 /* composite with the mask as a red overlay (⇧⌥-click, `\`) */
} pc_engine_mask_view_mode;

/* The selected layers in bottom-to-top document order, always including the active layer; robust
 * against stale ids. *out is a newly allocated array (free()) of *n ids (DocState::selected_layers). */
pc_status pc_engine_doc_state_selected_layers(const pc_engine_doc_state *d, pc_layer_id **out, size_t *n,
                                              pc_error *err);
bool pc_engine_doc_state_is_dirty(const pc_engine_doc_state *d);

/* ---- app-wide tool state (port of ToolState) ---------------------------------------------- */

/* How many CMYK picks ToolState remembers (ToolState::CMYK_PICKS). */
#define PC_ENGINE_CMYK_PICKS 8

/* One remembered CMYK pick: the sRGB colour that stands for the inks, and the inks (each 0..1). */
typedef struct pc_engine_cmyk_pick {
    float rgb[3];
    float inks[4];
} pc_engine_cmyk_pick;

typedef struct pc_engine_tool_state {
    float foreground[4];
    float background[4];
    /* --- EXTENSION REGION (E10 painting): the session brush, brush presets, Mixer Brush paint,
     * the coalescing brush gesture, the current preset name, the mask/pixels target split and the
     * CMYK picks. Mirrors the Rust ToolState, whose types come from photocraft_paint. */
    pc_paint_brush brush;     /* the session brush (BrushSettings) */
    pc_paint_preset *presets; /* built-ins plus user presets (owned) */
    size_t npresets;
    uint64_t presets_rev;       /* bumped by brush_presets_changed; the preset store syncs */
    pc_paint_mixer_state mixer; /* Mixer Brush paint carried between strokes */
    /* The coalescing key of the running tools.setBrush gesture and the brush before it, so the
     * gesture journals as one call (brush_cmds coalesce_journal). has_brush_gesture gates it. */
    bool has_brush_gesture;
    char *brush_gesture_key; /* owned */
    pc_paint_brush brush_gesture_start;
    char *current_preset;     /* name the current brush was picked from, or NULL (owned) */
    bool mask_targeted;       /* a layer mask is the edit target (ToolState::target_mask) */
    float other_colors[2][4]; /* the fg/bg pair of the target that isn't current */
    pc_engine_cmyk_pick cmyk_picks[PC_ENGINE_CMYK_PICKS]; /* newest last */
    size_t ncmyk_picks;
} pc_engine_tool_state;

/* Photoshop's colour pair for a layer mask that has none yet (ToolState::MASK_COLORS): white
 * foreground, black background. */
extern const float PC_ENGINE_MASK_COLORS[2][4];

/* Remember that sRGB `rgb` was picked as the inks `cmyk` (ToolState::remember_cmyk). */
void pc_engine_tool_state_remember_cmyk(pc_engine_tool_state *t, const float rgb[3], const float cmyk[4]);
/* The inks `rgb` was picked as, if it was picked as CMYK (ToolState::picked_cmyk); false else. */
bool pc_engine_tool_state_picked_cmyk(const pc_engine_tool_state *t, const float rgb[3], float out[4]);
/* Switch the edit target between layer pixels and a layer mask (ToolState::target_mask). Returns
 * whether the target changed; swaps the foreground/background pair with other_colors. */
bool pc_engine_tool_state_target_mask(pc_engine_tool_state *t, bool mask);
/* Shell target synchronization; preserves the separate pixel/mask color pairs. */
bool pc_engine_session_target_mask(pc_engine_session *session, bool mask);

/* ---- the session (port of Session) -------------------------------------------------------- */

/* Create/destroy a session. */
pc_status pc_engine_session_new(pc_engine_session **out, pc_error *err);
void pc_engine_session_free(pc_engine_session *s); /* NULL-safe; cancels running jobs */

/* Documents and the active one. */
const pc_engine_doc_state *pc_engine_session_documents(const pc_engine_session *s, size_t *n);
bool pc_engine_session_active_index(const pc_engine_session *s, size_t *out);
const pc_engine_doc_state *pc_engine_session_active(const pc_engine_session *s);
pc_engine_doc_state *pc_engine_session_active_mut(pc_engine_session *s);
bool pc_engine_session_set_active(pc_engine_session *s, size_t index);
const pc_engine_tool_state *pc_engine_session_tools(const pc_engine_session *s);

/* Add a document (from New/import) and make it active; *out_index receives its index. Takes over
 * *doc (moved; zeroed on success). path may be NULL. */
pc_status pc_engine_session_add_document(pc_engine_session *s, pc_document *doc, const char *path,
                                         size_t *out_index, pc_error *err);
/* Close the document at index; false when out of range. */
bool pc_engine_session_close(pc_engine_session *s, size_t index);
/* Move the document from tab `from` to `to` (clamped); *out_to its new index; false when out of
 * range. Keeps the active document active. */
bool pc_engine_session_move_document(pc_engine_session *s, size_t from, size_t to, size_t *out_to);

/* Pixel dimensions of the current engine clipboard; false for an empty clipboard.
 * This is a query and does not paste, copy pixels or record a journal entry. */
bool pc_engine_session_clipboard_size(const pc_engine_session *session, pc_size *out);

/* The session journal (executed commands) length (Session::journal.len()). */
size_t pc_engine_session_journal_len(const pc_engine_session *s);
/* Journal entry i: *id (borrowed) and *params (borrowed yyjson_mut_val of the stored value). */
bool pc_engine_session_journal_at(const pc_engine_session *s, size_t i, const char **id,
                                  const yyjson_mut_val **params);

/* Shell-owned View commands participate in action recording/playback. The engine retains no
 * ownership of `ctx`; clear the callback before freeing it. Only the four supported navigation
 * commands can be recorded this way. Playback without a shell still returns an unknown-command
 * failure, preserving headless semantics. Calls run synchronously on the session owner thread. */
typedef pc_status (*pc_engine_shell_dispatch)(void *ctx, const char *id, yyjson_val *params,
                                              pc_json_value *out, pc_error *err);
void pc_engine_session_set_shell_dispatch(pc_engine_session *s, pc_engine_shell_dispatch dispatch, void *ctx);
pc_status pc_engine_session_record_view(pc_engine_session *s, const char *id, yyjson_val *params,
                                        pc_error *err);

/* Run a command by id with JSON params (object or NULL). Writes *out (owned) only on success;
 * runs synchronously (crate Session::execute). params may be NULL for "no params". */
pc_status pc_engine_session_execute(pc_engine_session *s, const char *id, yyjson_val *params,
                                    pc_json_value *out, pc_error *err);
/* Convenience: execute with params given as JSON text (parsed with the engine's limits). */
pc_status pc_engine_session_execute_json(pc_engine_session *s, const char *id, const char *params_json,
                                         pc_json_value *out, pc_error *err);

/* Is the command currently runnable? (drives menu enablement). */
bool pc_engine_session_is_enabled(const pc_engine_session *s, const char *id);
/* Why the command can't run now (false = it can). *reason (optional) gets the message. */
bool pc_engine_session_disabled_reason(const pc_engine_session *s, const char *id, pc_error *reason);

/* Undoable edit of the active document (Session::edit). f edits a private copy; on PC_OK it is
 * committed as one history step named label, revision bumped. On error nothing changes. *has_active
 * / *active carry the active layer in and out. ctx is passed through. */
typedef pc_status (*pc_engine_edit_fn)(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                                       pc_error *err);
pc_status pc_engine_session_edit(pc_engine_session *s, const char *label, pc_engine_edit_fn f, void *ctx,
                                 pc_error *err);

/* Replace the active document's active layer without a history step (Session::select_layer). */
pc_status pc_engine_session_select_layer(pc_engine_session *s, pc_layer_id id, pc_error *err);

bool pc_engine_session_undo(pc_engine_session *s);
bool pc_engine_session_redo(pc_engine_session *s);

/* Move item i of a pc_layer_id vector to position to (clamped). *out_to its new index; false when
 * i is out of range (crate::move_item, used by the UI for tab/layer reordering). */
bool pc_engine_move_layer_id(pc_layer_id *v, size_t n, size_t i, size_t to, size_t *out_to);

/* ---- background jobs (port of jobs.rs) ---------------------------------------------------- *
 *
 * A long command runs its heavy part on a worker thread, reports progress and can be cancelled.
 * pc_engine_session_execute runs everything inline (tests, CLI, scripts see no difference);
 * pc_engine_session_start runs a job-capable command in the background. A command module opts in
 * from its run() by calling pc_engine_run_job / pc_engine_edit_job (declared in engine_internal.h).
 */

typedef struct pc_engine_job_ctx pc_engine_job_ctx; /* what a job's work sees (opaque) */

/* Report progress in 0..1 with a short status (empty keeps the previous one); never goes
 * backwards. Has the job been cancelled? Ask it to stop. PC_ERR_CANCELLED once cancelled. */
void pc_engine_job_ctx_progress(const pc_engine_job_ctx *ctx, float fraction, const char *message);
bool pc_engine_job_ctx_cancelled(const pc_engine_job_ctx *ctx);
void pc_engine_job_ctx_cancel(const pc_engine_job_ctx *ctx);
pc_status pc_engine_job_ctx_check(const pc_engine_job_ctx *ctx, pc_error *err);
float pc_engine_job_ctx_fraction(const pc_engine_job_ctx *ctx);

typedef enum pc_engine_job_outcome {
    PC_ENGINE_JOB_DONE,
    PC_ENGINE_JOB_FAILED,
    PC_ENGINE_JOB_CANCELLED
} pc_engine_job_outcome;

/* A job that has ended, as reported by poll_jobs. Owned fields; free with pc_engine_job_events_free. */
typedef struct pc_engine_job_event {
    uint64_t id;
    char *command;
    char *label;
    bool has_document;
    pc_doc_id document;
    pc_engine_job_outcome outcome;
    pc_json_value result; /* when DONE */
    char *error;          /* when FAILED */
} pc_engine_job_event;

void pc_engine_job_events_free(pc_engine_job_event *ev, size_t n);

/* A job's work callback (runs on the worker; must read only what it captured in ud, never the
 * session) and its apply callback (runs on the calling thread with the work's payload, like a
 * command). work writes *out_payload (a void* the apply/free_payload own); apply writes *out. */
typedef pc_status (*pc_engine_job_work_fn)(const pc_engine_job_ctx *ctx, void *ud, void **out_payload,
                                           pc_error *err);
typedef pc_status (*pc_engine_job_apply_fn)(pc_engine_session *s, void *payload, pc_json_value *out,
                                            pc_error *err);

/* Run a command by id, in the background when it supports it (Session::start). On a started job
 * *out_is_job is true and *out_job_id its id; otherwise *out holds the inline result. */
pc_status pc_engine_session_start(pc_engine_session *s, const char *id, yyjson_val *params, bool *out_is_job,
                                  uint64_t *out_job_id, pc_json_value *out, pc_error *err);

/* Start a job that isn't an engine command (an open, a shell op, a test). ud is passed to work;
 * free_ud frees it (may be NULL); free_payload frees a payload work produced but apply never
 * consumed (cancellation/failure; may be NULL). Runs inline when nothing is in the background. */
pc_status pc_engine_session_start_job(pc_engine_session *s, const char *command, yyjson_val *params,
                                      const char *label, bool lock_document, pc_engine_job_work_fn work,
                                      void *ud, void (*free_ud)(void *), void (*free_payload)(void *),
                                      pc_engine_job_apply_fn apply, bool *out_is_job, uint64_t *out_job_id,
                                      pc_json_value *out, pc_error *err);

/* Apply finished background jobs (on this thread) and return every job that ended since the last
 * call as an owned array of *n events (free with pc_engine_job_events_free). */
pc_status pc_engine_session_poll_jobs(pc_engine_session *s, pc_engine_job_event **out, size_t *n,
                                      pc_error *err);
/* Block until job id ends, apply it, and return its result (PC_ERR_CANCELLED if it was cancelled). */
pc_status pc_engine_session_wait_job(pc_engine_session *s, uint64_t id, pc_json_value *out, pc_error *err);
bool pc_engine_session_cancel_job(pc_engine_session *s, uint64_t id);
void pc_engine_session_cancel_all_jobs(pc_engine_session *s);
void pc_engine_session_join_cancelled_jobs(pc_engine_session *s);
bool pc_engine_session_has_jobs(const pc_engine_session *s);
/* Run inline (not background) job work under ctx so another thread can cancel it (live previews).
 * ctx NULL restores the default (a context nothing can cancel). */
void pc_engine_session_set_inline_job_ctx(pc_engine_session *s, const pc_engine_job_ctx *ctx);
/* A fresh standalone JobCtx (for an inline cancellation context); free with pc_engine_job_ctx_free. */
pc_engine_job_ctx *pc_engine_job_ctx_new(void);
void pc_engine_job_ctx_free(pc_engine_job_ctx *ctx);

/* ---- build provenance (build_info) -------------------------------------------------------- */

extern const char *const pc_engine_version; /* the workspace version, e.g. "0.8.2" */
const char *pc_engine_git_sha(void);        /* full commit, or NULL */
const char *pc_engine_build_date(void);     /* "YYYY-MM-DD", or NULL */
const char *pc_engine_short_sha(void);      /* commit shortened to 9 chars, or NULL (static buffer) */
/* "0.8.2 (3f2a9c1d0, 2026-10-01)" or "0.8.2 (dev build)" into out[cap]; returns the length it
 * wanted (may exceed cap-1, truncated). */
size_t pc_engine_long_version(char *out, size_t cap);

/* ---- JSON views (inspect) ----------------------------------------------------------------- */

pc_status pc_engine_inspect_session(const pc_engine_session *s, pc_json_value *out, pc_error *err);
pc_status pc_engine_inspect_history(const pc_engine_doc_state *state, pc_json_value *out, pc_error *err);
pc_status pc_engine_inspect_document(const pc_engine_doc_state *d, pc_json_value *out, pc_error *err);
pc_status pc_engine_inspect_layer(const pc_layer *l, pc_json_value *out, pc_error *err);

#endif /* PC_ENGINE_ENGINE_H */
