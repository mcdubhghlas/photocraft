/* Engine internals shared across the src/engine .c files (not a public API).
 *
 * The Session struct lives here so session.c, command.c and jobs.c can touch its fields directly,
 * as the Rust `impl Session` blocks (spread across lib.rs, commands.rs, jobs.rs) all do. Later
 * engine chunks add fields with small surgical edits (see the EXTENSION REGION markers).
 */
#ifndef PC_ENGINE_INTERNAL_H
#define PC_ENGINE_INTERNAL_H

#include <stdatomic.h>
#include <pthread.h>

#include "algo/filter.h" /* E9: pc_algo_filter (apply primitive + gallery params) */
#include "engine.h"
#include "color_cmds.h" /* E8: pc_color_state (embedded in the Session) */
#include "presets.h"    /* E2b: pc_preset_state (embedded in the Session) */

/* ---- engine errors: Display-text helpers (match the reference's EngineError strings) ------ */

pc_status pc_engine_err_unknown_command(pc_error *err, const char *id);
pc_status pc_engine_err_disabled(pc_error *err, const char *id, const char *why);
pc_status pc_engine_err_bad_params(pc_error *err, const char *cmd, const char *fmt, ...);
pc_status pc_engine_err_no_document(pc_error *err);
pc_status pc_engine_err_no_layer(pc_error *err, pc_layer_id id);
pc_status pc_engine_err_other(pc_error *err, const char *fmt, ...);
pc_status pc_engine_err_cancelled(pc_error *err);

/* ---- journal ------------------------------------------------------------------------------ */

typedef struct pc_engine_journal_entry {
    char *id;              /* owned */
    pc_json_value params;  /* owned */
} pc_engine_journal_entry;

/* ---- background jobs (jobs.c) ------------------------------------------------------------- */

/* What a job's work callback sees: progress plus a cancellation flag. Reference-counted; cloned
 * ctx share the same state (Rust JobCtx). */
typedef struct pc_engine_job_shared {
    atomic_int refs;
    atomic_bool cancel;
    atomic_uint progress; /* f32 bits of the 0..1 fraction (monotonic via max) */
    pthread_mutex_t msg_lock;
    char message[256];
} pc_engine_job_shared;

struct pc_engine_job_ctx { /* the typedef name is declared in engine.h */
    pc_engine_job_shared *shared; /* never NULL once initialised */
};

struct pc_engine_running; /* opaque per-job record (jobs.c) */

typedef struct pc_engine_job_info_row pc_engine_job_info_row; /* for recent[] (jobs.c) */

typedef struct pc_engine_jobs {
    uint64_t next;
    bool spawn;                        /* set while start() runs a command: run() spawns */
    struct pc_engine_running *pending; /* a job registered while spawn was set */
    struct pc_engine_running **running;
    size_t nrunning, cap_running;
    /* Ended jobs not yet collected by poll_jobs (owned JSON events). */
    struct pc_engine_job_event *events;
    size_t nevents, cap_events;
    /* The last few ended jobs (JobInfo rows), for jobs.list. */
    pc_engine_job_info_row *recent;
    size_t nrecent, cap_recent;
    /* Workers of cancelled jobs that may still be unwinding. */
    pthread_t *draining;
    bool *draining_joined;
    size_t ndraining, cap_draining;
    bool has_inline;               /* the ctx inline job work runs under */
    pc_engine_job_ctx inline_ctx;
} pc_engine_jobs;

void pc_engine_jobs_init(pc_engine_jobs *j);
void pc_engine_jobs_fini(pc_engine_jobs *j); /* cancels running jobs, joins drainers */

/* Run a job-capable command's heavy part (jobs::run). work runs on a worker when the command was
 * started in the background (inline otherwise); apply then runs on the calling thread. With
 * lock_document the active document is locked while the job runs. Writes *out only when it ran
 * inline. A command module calls this from its run(). */
pc_status pc_engine_run_job(pc_engine_session *s, const char *label, bool lock_document,
                            pc_engine_job_work_fn work, void *ud, void (*free_ud)(void *),
                            void (*free_payload)(void *), pc_engine_job_apply_fn apply,
                            pc_json_value *out, pc_error *err);

/* ---- preferences (prefs.c) ---------------------------------------------------------------- */

/* The session's preferences, kept as one yyjson document (the camelCase Preferences tree) plus a
 * revision counter that moves on every change (frontends persist when it moves). Port of
 * prefs::PrefsStore. */
typedef struct pc_engine_prefs {
    yyjson_mut_doc *values; /* the Preferences object; NULL only on an init OOM */
    uint64_t rev;
} pc_engine_prefs;

void pc_engine_prefs_init(pc_engine_prefs *p); /* build defaults (leaves values NULL on OOM) */
void pc_engine_prefs_fini(pc_engine_prefs *p); /* NULL-safe */

/* ---- the session -------------------------------------------------------------------------- */

struct pc_engine_session {
    pc_engine_doc_state *docs;
    size_t ndocs, cap_docs;
    bool has_active;
    size_t active;
    pc_engine_tool_state tools;
    pc_engine_journal_entry *journal;
    size_t njournal, cap_journal;
    char *coalesce_request; /* coalescing key of the command being executed (owned), or NULL */
    /* Untrusted sessions gate every command. NULL runs everything. */
    pc_status (*authorize)(const char *id, yyjson_val *params, pc_error *err);
    pc_engine_jobs jobs;
    /* A canvas-size map (crop/Canvas Size/Image Size/rotation) the next edit applies to Color
     * Sampler points, and how many were dropped since last read. The sampler points themselves
     * (DocState.color_samplers) arrive with E12; until then the map is tracked but moves nothing. */
    bool has_sampler_map;
    pc_affine sampler_map;
    size_t samplers_removed;
    /* A single colour channel the running command may change (channel_cmds, E7); SIZE_MAX = none. */
    size_t color_restrict;
    /* --- EXTENSION REGION: clipboard (E4), color (E8), smart_links (E6), prefs (E2), patterns
     * (E10), analysis (E8), presets (E2), actions (E12), preset_store (E2), subject/object caches
     * (E7), live_commit (E10), ... are added here by the owning chunk with a surgical edit. */
    /* E2: Edit > Preferences, keyboard-shortcut, menu and toolbar customisation (prefs.c). */
    pc_engine_prefs prefs;
    /* E2: the persistent brush-preset store (desktop only; NULL keeps presets session-only, as in
     * headless and test sessions). The preset DATA model and the preset-panel commands depend on
     * documents, vectors, patterns and swatches (E3-E11) and on brush presets and actions
     * (E10/E12); E2 owns only the module seam (preset_store.c) and the sync_preset_store hook. */
    struct pc_engine_preset_store *preset_store;
    /* E2b: the preset-panel data model (Window > Gradients/Patterns/Styles/Shapes/Tool
     * Presets/Clone Source, and the Swatches panel). Distinct from preset_store (the persistent
     * brush-preset I/O backend). Built-in defaults set in session_new, released in session_free;
     * persisted with the preferences document (pc_engine_presets_to_json / load_presets_json). */
    pc_preset_state presets;
    /* E4: the session clipboard (Cut / Copy / Paste). NULL = empty. Owned; released by
     * pc_engine_clipboard_free (edit_cmds.c) in session_free. The whole-layer clipboard (a copied
     * smart/type/shape/group layer that Paste recreates) is deferred with its io-linked dependency
     * (REWRITE_PLAN.md §7/§8), so the C Clip carries only pixels (no `layers` member yet). */
    struct pc_engine_clip *clipboard;
    /* E8: Edit > Color Settings, per-document proofing, and the colour-conversion primitives
     * (color_cmds.c). Initialised in session_new, released in session_free. */
    pc_color_state color;
    /* E7: session-wide Quick Mask Options (double-click the Quick Mask button): the overlay
     * colour, opacity and whether the colour marks masked or selected areas (channel_cmds). */
    pc_color quick_mask_color;
    float quick_mask_opacity;
    pc_color_indicates quick_mask_indicates;
    /* E11: Paths panel shape-style clipboard (path.style.copyFill / copyStroke -> pasteFill /
     * pasteStroke). has_* false = empty (zero-initialised in session_new). Released by
     * pc_engine_vector_clipboard_fini (vector_cmds.c) in session_free. */
    bool has_path_fill_clip;
    pc_fill path_fill_clip;
    bool has_path_stroke_clip;
    pc_shape_stroke path_stroke_clip;
    /* E11 (type): Type > Save/Load Default Type Styles (Session::type_defaults). The character
     * and paragraph style new type layers start from; has_type_defaults false = Photoshop's
     * default (zero-initialised in session_new). Released by pc_engine_type_defaults_fini
     * (type_cmds.c) in session_free. */
    bool has_type_defaults;
    pc_char_style type_defaults_char;
    pc_paragraph_style type_defaults_para;
    /* E8 (analysis): Image > Analysis session-wide state -- the Measurement Log, the row/label
     * counters and the Select Data Points choices (Session::analysis). NULL = the defaults (lazily
     * created on first use). Owned; released by pc_engine_analysis_fini (analysis_cmds.c) in
     * session_free. The type is private to analysis_cmds.c. */
    struct pc_engine_analysis *analysis;
    /* E10: the app-level pattern library (Window > Patterns; pattern_cmds.c). Built-in procedural
     * patterns plus defined/imported ones (Session::patterns in the reference); distinct from each
     * document's own patterns. Built in session_new, released in session_free through
     * pc_engine_pattern_library_fini. */
    pc_pattern *patterns;
    size_t npatterns;
    size_t cap_patterns;
    /* E12: File-menu session state shared by slice_cmds (View > Lock Slices), automate_cmds and
     * web_cmds (automate_cmds::FileMenuState). Not saved, not history. The dynamic members
     * (image_assets, event_log, last_print/last_web JSON) are owned and released in session_free. */
    bool slices_locked;        /* View > Lock Slices */
    pc_doc_id *image_assets;   /* documents with File > Generate > Image Assets on */
    size_t nimage_assets;
    bool firing;               /* set while event scripts run (no recursive events) */
    pc_json_value last_print;  /* File > Print settings used last (null = none) */
    pc_json_value last_web;    /* Save for Web settings used last (null = none) */
    pc_json_value *event_log;  /* recent script-event results, newest last, capped */
    size_t nevent_log;
    /* E12: Window > Actions recorded-action state (actions_cmds.c). Opaque (the full type is in
     * actions_cmds.h); lazily allocated, released by pc_engine_actions_session_fini. */
    struct pc_action_state *actions;
};

/* E12: release a session's Actions state (actions_cmds.c). NULL-safe. */
void pc_engine_actions_session_fini(pc_engine_session *s);

/* E11: release the shape-style clipboard of a session (vector_cmds.c). NULL-safe. */
void pc_engine_vector_clipboard_fini(pc_engine_session *s);

/* E11 (type): release a session's saved default type styles (type_cmds.c). NULL-safe. */
void pc_engine_type_defaults_fini(pc_engine_session *s);

/* Internal session helpers (session.c). */
void pc_engine_fix_selection(pc_engine_doc_state *st);
/* Append one journal entry (id + owned copy of params; params may be NULL). */
pc_status pc_engine_journal_push(pc_engine_session *s, const char *id, yyjson_val *params, pc_error *err);
pc_status pc_engine_doc_state_init(pc_engine_doc_state *st, pc_document *doc, const char *path,
                                   size_t max_states, size_t max_bytes, pc_error *err);
void pc_engine_doc_state_fini(pc_engine_doc_state *st);
/* E7: free a DocState's Channels-panel view state (channel_cmds.c). NULL-safe. Declared here so
 * session.c can release it in doc_state_fini without including channel_cmds.h. */
void pc_engine_channel_view_free(struct pc_engine_channel_view *v);
/* E10: free a DocState's painting symmetry (symmetry_cmds.c). NULL-safe. Declared here so
 * session.c can release it in doc_state_fini without including symmetry_cmds.h. */
void pc_engine_symmetry_free(struct pc_engine_symmetry *sym);
/* E10: deep-clone a DocState's painting symmetry, or NULL -> NULL (symmetry_cmds.c). */
struct pc_engine_symmetry *pc_engine_symmetry_clone(const struct pc_engine_symmetry *sym);
/* E4: free a DocState's floating selection (float_cmds.c). NULL-safe. Declared here so session.c
 * can release it in doc_state_fini without including float_cmds internals. */
void pc_engine_floating_free(struct pc_engine_floating *f);
/* E4: free a session's clipboard (edit_cmds.c). NULL-safe. Declared here so session.c can release
 * it in session_free without including edit_cmds internals. */
void pc_engine_clipboard_free(struct pc_engine_clip *c);
/* Bytes the session clipboard holds (edit_cmds.c), for Edit > Purge > Clipboard. 0 if NULL. */
size_t pc_engine_clipboard_bytes(const struct pc_engine_clip *c);
/* E10: release the painting-tool fields of a ToolState (brush, presets, gesture, current preset);
 * leaves the scalars. NULL-safe (session.c tool_state_fini). */
void pc_engine_tool_state_fini(pc_engine_tool_state *t);
/* E10: initialise the painting-tool fields of a ToolState to Photoshop's defaults (built-in
 * presets, 10% smoothing brush). Returns false on OOM, leaving an empty-but-valid tool state. */
bool pc_engine_tool_state_painting_default(pc_engine_tool_state *t);

/* E10: build the session's pattern library from the built-ins and fill the Patterns-panel default
 * folders (pattern_cmds.c). Declared here so session.c can call it without including
 * pattern_cmds.h. Leaves a valid (possibly empty) library on OOM; never crashes. */
void pc_engine_pattern_library_init(pc_engine_session *s);
/* E10: release the session's pattern library (pattern_cmds.c). NULL-safe. */
void pc_engine_pattern_library_fini(pc_engine_session *s);

/* dispatch (jobs.c): run command id with params; background=true may start a job.
 * *out_is_job / *out_job_id report a started background job; otherwise *out holds the result. */
pc_status pc_engine_dispatch(pc_engine_session *s, const char *id, yyjson_val *params, bool background,
                             bool *out_is_job, uint64_t *out_job_id, pc_json_value *out, pc_error *err);

/* jobs.c helpers used by session.c / command.c. */
bool pc_engine_active_job(const pc_engine_session *s); /* a job locks the active document */
bool pc_engine_job_on(const pc_engine_session *s, pc_doc_id doc);
void pc_engine_cancel_jobs_on(pc_engine_session *s, pc_doc_id doc);
/* Why id can't run now because of a job (false = no conflict); *reason gets the message. */
bool pc_engine_job_conflict(const pc_engine_session *s, const char *id, bool journal, pc_error *reason);

/* owned JSON result helpers (command.c), shared with command modules. */
pc_status pc_engine_result_doc(pc_json_value *out, yyjson_mut_doc **doc, pc_error *err);
pc_status pc_engine_result_null(pc_json_value *out, pc_error *err);

/* The job-table commands (jobs.list, jobs.cancel), appended by the registry (jobs.c). */
const pc_engine_command *pc_engine_jobs_specs(size_t *n);

/* The preference commands (prefs.get/set/reset, edit.preferences.*, keyboard shortcuts, menus,
 * toolbar), appended by the registry (prefs.c). */
const pc_engine_command *pc_engine_prefs_specs(size_t *n);

/* File > New plus the New Document validation helpers (document_preset_cmds.c, E3). */
const pc_engine_command *pc_engine_document_preset_specs(size_t *n);

/* ---- File menu + file I/O primitives (file_cmds.c, E3) ------------------------------------ *
 * File-menu commands beyond New/Close and the document-level guide items of the View menu, plus
 * the byte-level primitives that route documents through src/io + src/format. The primitives are
 * exported for E6 (editContents / artboard export), E9 (lens / vp / Photomerge load) and E2b
 * (preset import). Port of crates/engine/src/file_cmds.rs. */
const pc_engine_command *pc_engine_file_specs(size_t *n);

/* Save options (file_cmds::SaveOpts). dpx_bits is a pc_dpx_bits stored as int so this header does
 * not depend on codecs; `save == NULL` everywhere means the defaults. */
typedef struct pc_engine_save_opts {
    bool has_quality;
    double quality; /* Photoshop's 0..12 JPEG scale */
    bool tiff_layers;
    bool image_previews;
    bool psd_maximize_compatibility;
    int dpx_bits; /* pc_dpx_bits (PC_DPX_BITS_AUTO default) */
} pc_engine_save_opts;

pc_engine_save_opts pc_engine_save_opts_default(void);
/* `quality`/`tiffLayers`/`dpxBits` from `p` (a bad `dpxBits` is an error of `cmd`); image previews
 * and PSD merged image from the session File Handling preferences. */
pc_status pc_engine_save_opts_from_params(const pc_engine_session *s, yyjson_val *p, const char *cmd,
                                          pc_engine_save_opts *out, pc_error *err);

/* pc_io_warnings forward typedef so callers can receive export notes without including io here;
 * a caller that passes a non-NULL warnings pointer includes io/core.h. */
typedef struct pc_io_warnings pc_io_warnings;

/* Read a whole file at `path` into *out (bounded, src/format read). The path prefixes the error. */
pc_status pc_engine_read_file(const char *path, pc_buf *out, pc_error *err);
/* Crash-safe (symlink-hardened temp + rename) write of `len` bytes to `path` (src/base atomic). */
pc_status pc_engine_write_file(const char *path, const void *data, size_t len, pc_error *err);
/* Import a file's bytes into *out (document only; warnings discarded; an Affinity preview-only
 * file is refused). Port of file_cmds::import = load_sources. */
pc_status pc_engine_import(const char *name, const uint8_t *bytes, size_t len, pc_document *out,
                           pc_error *err);
/* Encode `doc` for `path`'s extension into *out (owned bytes); *warnings (may be NULL) gets the
 * export notes. `save` may be NULL for the defaults. Port of file_cmds::encode. */
pc_status pc_engine_encode(const pc_document *doc, const char *path, const pc_engine_save_opts *save,
                           pc_buf *out, pc_io_warnings *warnings, pc_error *err);
/* Encode and atomically write `doc` to `path`. Port of file_cmds::save_doc. */
pc_status pc_engine_save_doc(const pc_document *doc, const char *path, const pc_engine_save_opts *save,
                             pc_io_warnings *warnings, pc_error *err);
/* The flattened image of `doc` as a surface in `fmt` at the origin (file_cmds::flattened). On OOM
 * returns a valid empty surface and fills err. */
pc_status pc_engine_flattened(const pc_document *doc, pc_pixel_format fmt, pc_surface *out,
                              pc_error *err);

/* File > Export > Paths to Illustrator (print_cmds.c, E3). File > Print / Print One Copy / Printers
 * / Package are deferred (PDF writer + system spooler + relinkToFile not ported). */
const pc_engine_command *pc_engine_print_specs(size_t *n);

/* ---- preset panels (E2b, presets*.c + the <kind>_preset_cmds.c modules) ------------------- */
/* Record a preset change: bump the panel revision and mark the preferences document dirty (Rust
 * Session::presets_changed). */
void pc_engine_presets_changed(pc_engine_session *s);
/* The preset pattern groups (char* ids) and gradient groups, for E10 (pattern_cmds /
 * gradient_fill_cmds) and layer-style patternOverlay to read and sync. Borrowed. */
pc_preset_groups *pc_engine_presets_patterns(pc_engine_session *s);
pc_preset_groups *pc_engine_presets_gradients(pc_engine_session *s);
/* Persistence (prefs "presets" key): serialise into *out (owned); restore a saved blob (a
 * malformed value is ignored). */
pc_status pc_engine_presets_to_json(const pc_engine_session *s, pc_json_value *out, pc_error *err);
void pc_engine_load_presets_json(pc_engine_session *s, yyjson_val *v);
/* Trim a hand-edited swatch library to the limits (swatch_cmds.c; used by load_presets_json). */
void pc_engine_swatches_cap_library(pc_preset_groups *g);
/* Read a preset file from `data` (base64) or `path`; *out gets the bytes (owned, pc_buf_fini),
 * *stem the file's base name without extension (or "" for `data`). `what` names the file kind in
 * the error. Capped at 512 MiB. Port of preset_import_cmds::file_bytes. */
pc_status pc_engine_preset_file_bytes(yyjson_val *p, const char *cmd, const char *what, pc_buf *out,
                                      char *stem, size_t stem_cap, pc_error *err);

/* The preset-panel command modules (register_modules() in command.c). */
const pc_engine_command *pc_engine_clone_source_specs(size_t *n);    /* cloneSource.* */
const pc_engine_command *pc_engine_tool_preset_specs(size_t *n);     /* tool.presets.* */
const pc_engine_command *pc_engine_swatch_specs(size_t *n);          /* swatches.* */
const pc_engine_command *pc_engine_gradient_preset_specs(size_t *n); /* gradient.presets.* */
const pc_engine_command *pc_engine_style_preset_specs(size_t *n);    /* style.presets.* */
const pc_engine_command *pc_engine_shape_preset_specs(size_t *n);    /* shape.presets.* */

/* ---- Clone Stamp / Healing Brush stroke mapping (presets_clone.c, E10-retouch) ------------ *
 * Port of crates/engine/src/presets/clone_source.rs Mapping/transform_matrix/resolve/mapping/
 * resample: where a clone stroke samples (destination d reads source + m.(d - anchor), m
 * row-major 2x2). `struct pc_paint_region` is forward-declared (paint/retouch.h). */
struct pc_paint_region;
typedef struct pc_clone_mapping {
    double source[2];
    double anchor[2];
    double m[4]; /* row-major 2x2 taking destination offsets to source offsets */
} pc_clone_mapping;
/* Matrix for a slot's W/H percent scale, rotation (deg CCW on screen) and flips. */
void pc_engine_clone_transform_matrix(const double scale[2], double rotation_deg, bool flip_h, bool flip_v,
                                      double out[4]);
bool pc_engine_clone_is_translation(const pc_clone_mapping *map);
void pc_engine_clone_offset(const pc_clone_mapping *map, int32_t *dx, int32_t *dy);
void pc_engine_clone_map(const pc_clone_mapping *map, double x, double y, double *ox, double *oy);
/* Source pixels (with a 2-px apron) needed to resample `dst`; clamped to i32 (#716). */
pc_rect pc_engine_clone_source_rect(const pc_clone_mapping *map, pc_rect dst);
/* Resolve a stroke's mapping from its params and the active slot, without remembering anything
 * (`first` = the stroke's first point). *has_anchor / anchor[2] carry the anchor an aligned stroke
 * would store. PC_ERR_INVALID (nothing written) when no source is known. */
pc_status pc_engine_clone_resolve(const pc_engine_session *s, yyjson_val *p, double fx, double fy,
                                  const char *cmd, pc_clone_mapping *out, bool *has_anchor, double anchor[2],
                                  pc_error *err);
/* pc_engine_clone_resolve, then remember the anchor in the active slot (mutates the session). */
pc_status pc_engine_clone_mapping(pc_engine_session *s, yyjson_val *p, double fx, double fy,
                                  const char *cmd, pc_clone_mapping *out, pc_error *err);
/* Bilinear resample of `src` (covering pc_engine_clone_source_rect) onto `dst`, premultiplying by
 * the alpha channel `alpha` when has_alpha. *out is a fresh region (pc_paint_region_fini). */
pc_status pc_engine_clone_resample(const struct pc_paint_region *src, pc_rect dst,
                                   const pc_clone_mapping *map, bool has_alpha, size_t alpha,
                                   struct pc_paint_region *out, pc_error *err);

/* File > Import > WIA Support (wia_cmds.c, E3). */
const pc_engine_command *pc_engine_wia_specs(size_t *n);

/* Image > Mode > Multichannel (multichannel_cmds.c, E3). */
const pc_engine_command *pc_engine_multichannel_specs(size_t *n);
/* Image > Mode > RGB/CMYK/Lab/Grayscale from a Multichannel document, called by color_cmds
 * (E8) convert_mode. Writes *out on success. */
pc_status pc_engine_multichannel_convert_from(pc_engine_session *s, pc_color_mode mode, yyjson_val *p,
                                              pc_json_value *out, pc_error *err);
/* Names of the ink (spot) channels of a Multichannel image; *out is an owned array of *n owned
 * strings (free each then the array), used by inspect (E7). */
pc_status pc_engine_multichannel_ink_names(const pc_document *doc, char ***out, size_t *n, pc_error *err);

/* Image menu: Image Size, Canvas Size, Crop, Trim, Mode (colour model + bit depth), Duplicate,
 * Reveal All and right-angle Image Rotation / flips (image_cmds.c, E3). */
const pc_engine_command *pc_engine_image_specs(size_t *n);
/* Image > Mode palette/ink modes (Indexed Color, Color Table, Bitmap, Duotone) and Image Rotation
 * › Arbitrary (mode_cmds.c, E3). */
const pc_engine_command *pc_engine_mode_specs(size_t *n);
/* Apply `cb` to every pixel surface of a layer tree: raster content or the text/shape/smart cache,
 * the fill cache, and (when `masks`) each layer mask and smart-filter mask; groups recurse. The
 * `is_mask` argument is true for masks. Port of image_cmds::for_each_surface; exported for E4
 * transform_cmds. */
typedef void (*pc_engine_surface_cb)(pc_surface *surf, bool is_mask, void *ctx);
void pc_engine_image_for_each_surface(pc_layer *layers, size_t n, bool masks, pc_engine_surface_cb cb,
                                      void *ctx);

/* Image > Trap (trap_cmds.c, E9). */
const pc_engine_command *pc_engine_trap_specs(size_t *n);

/* Layers panel: effects-list disclosure (fx_view_cmds.c, E9). */
const pc_engine_command *pc_engine_fx_view_specs(size_t *n);

/* Filter > Render (Flame, Picture Frame, Tree) (render_cmds.c, E9). */
const pc_engine_command *pc_engine_render_specs(size_t *n);

/* Filter menu: blur/sharpen/noise/pixelate/stylize/distort/other + Last Filter (filters_cmds.c,
 * E9). Also defines the pc_engine_hook_filter_validate dispatch hook. */
const pc_engine_command *pc_engine_filters_specs(size_t *n);
/* Run filter command `id` with JSON `params` on a copy of `surf`, the pixel function behind every
 * filter.* command (port of filters::apply_filter_to_surface). `bounds` frames distortions;
 * `selection` (NULL = none) limits the result; neighbourhood filters repeat the edge pixels of
 * `canvas` unioned with the surface's content instead of fading in transparency, and the output is
 * clipped to it. On success *applied is true and *out is the filtered surface (pc_surface_fini);
 * *applied is false (and *out untouched) for ids that are not pixel filters (Rust None) -- the
 * caller leaves the pixels alone. Never crashes on hostile params. The KEY primitive for smart
 * filters (E6), smart adjustments (E8) and the Filter Gallery (E9); the command-id -> algo-filter
 * mapping is params_for (filters_cmds.c). (filters_cmds.c, E9.) */
pc_status pc_engine_apply_filter_to_surface(const char *id, yyjson_val *params, const pc_surface *surf,
                                            pc_rect bounds, const pc_surface *selection, pc_rect canvas,
                                            pc_surface *out, bool *applied, pc_error *err);

/* Edit > Auto-Align Layers / Auto-Blend Layers (align_cmds.c, E9), on the panorama primitives in
 * photo_cmds.c. */
const pc_engine_command *pc_engine_align_specs(size_t *n);

/* Filter > Filter Gallery and the per-gallery-filter commands (gallery_cmds.c, E9). */
const pc_engine_command *pc_engine_gallery_specs(size_t *n);
/* Build the algo filter for a gallery id (filter.filterGallery / filter.gallery.*); *found false
 * for other ids (chained from filters_cmds.c params_for). Never crashes. */
pc_status pc_engine_gallery_params_for(const char *id, yyjson_val *p, pc_algo_filter *out, bool *found,
                                       pc_error *err);
/* Validate a gallery invocation's choice params (filters_validate.inc dispatches gallery ids here). */
pc_status pc_engine_gallery_validate(const char *id, yyjson_val *params, pc_error *err);
/* The shared filter runner and the filter-layer `enabled` predicate (filters_cmds.c), used by
 * gallery_cmds.c. */
pc_status pc_engine_run_filter(pc_engine_session *s, const char *id, yyjson_val *p, pc_json_value *out,
                               pc_error *err);
pc_status pc_engine_filterable(const pc_engine_session *s, pc_error *reason);

/* E10 painting modules (brush_cmds.c, symmetry_cmds.c, brush_key_cmds.c). */
const pc_engine_command *pc_engine_brush_specs(size_t *n);
const pc_engine_command *pc_engine_symmetry_specs(size_t *n);
const pc_engine_command *pc_engine_brush_key_specs(size_t *n);
const pc_engine_command *pc_engine_redeye_specs(size_t *n);
const pc_engine_command *pc_engine_pick_specs(size_t *n);
/* E10 fills/erasers (pattern_cmds.c, paint_cmds.c, gradient_fill_cmds.c, eraser_cmds.c,
 * retouch_cmds.c). */
const pc_engine_command *pc_engine_pattern_specs(size_t *n);
const pc_engine_command *pc_engine_paint_specs(size_t *n);
const pc_engine_command *pc_engine_gradient_fill_specs(size_t *n);
const pc_engine_command *pc_engine_eraser_specs(size_t *n);
const pc_engine_command *pc_engine_retouch_specs(size_t *n);
/* E7 (selection_cmds.c): straight RGBA8 of the active layer (all_layers false) or the visible
 * composite over the canvas; *area gets the canvas bounds, *out an owned array (free()) of *len
 * pixels. Used by the Paint Bucket (paint_cmds) and Magic Eraser (eraser_cmds). */
pc_status pc_engine_sample_rgba8(pc_engine_session *s, bool all_layers, pc_rect *area,
                                 uint8_t (**out)[4], size_t *len, pc_error *err);

/* E11 vector module (vector_specs.c): shape.*, path.*, layer.vectorMask.*. */
const pc_engine_command *pc_engine_vector_specs(size_t *n);

/* E8 adjustment commands (adjust_cmds.c): the generated per-kind Image > Adjustments and
 * Layer > New Adjustment Layer commands, the destructive-only Shadows/Highlights, Replace Color,
 * Match Color and HDR Toning, layer.setAdjustment, Desaturate and the Color Lookup look list. */
const pc_engine_command *pc_engine_adjust_specs(size_t *n);
/* E8 analysis commands (analysis_cmds.c): Image > Analysis measurement scale, data points, Record
 * Measurements, the Ruler and Count tools, Place Scale Marker and the Measurement Log. */
const pc_engine_command *pc_engine_analysis_specs(size_t *n);
/* E8 (analysis_cmds.c): release a session's analysis state (Measurement Log, data points). NULL-safe. */
void pc_engine_analysis_fini(pc_engine_session *s);

/* E11 type modules: type.* (type_cmds.c), the Type menu (type_extra_cmds.c), caret queries
 * (type_caret_cmds.c), Check Spelling + Insert Glyph (type_spell_cmds.c), Character/Paragraph
 * Styles (type_styles_cmds.c), and point-level path edits (path_edit_cmds.inc). */
const pc_engine_command *pc_engine_type_specs(size_t *n);
const pc_engine_command *pc_engine_type_extra_specs(size_t *n);
const pc_engine_command *pc_engine_type_caret_specs(size_t *n);
const pc_engine_command *pc_engine_type_spell_specs(size_t *n);
const pc_engine_command *pc_engine_type_styles_specs(size_t *n);
const pc_engine_command *pc_engine_path_edit_specs(size_t *n);

/* ---- E12 modules (actions, automate, comps, slices, variables, video, notes, frame, timeline,
 * plug-ins, web). Each exposes its command specs; register_modules() in command.c adds one line. */
const pc_engine_command *pc_engine_notes_specs(size_t *n);
const pc_engine_command *pc_engine_frame_specs(size_t *n);
const pc_engine_command *pc_engine_timeline_specs(size_t *n);
const pc_engine_command *pc_engine_comps_specs(size_t *n);
const pc_engine_command *pc_engine_slice_specs(size_t *n);
/* slice_cmds.c: re-fit layer-based slices to their layers after a command (no history step).
 * Called from the automate after-command hook (slice_cmds::refresh). NULL-safe on no document. */
void pc_engine_slices_refresh(pc_engine_session *s);
const pc_engine_command *pc_engine_variables_specs(size_t *n);
const pc_engine_command *pc_engine_video_specs(size_t *n);
const pc_engine_command *pc_engine_web_specs(size_t *n);
const pc_engine_command *pc_engine_plugin_specs(size_t *n);
const pc_engine_command *pc_engine_automate_specs(size_t *n);
const pc_engine_command *pc_engine_actions_specs(size_t *n);
/* E-tail (ET2): deferred-but-now-unblocked command fills.
 * tool_colors_cmds.c: tools.setColors/swapColors/defaultColors (foreground/background colours).
 * auto_adjust_cmds.c: image.autoTone/autoContrast/autoColor (one-click Levels).
 * layer_menu_cmds.c: layer.flattenImage, layer.layerMask.apply/hideSelection/fromTransparency,
 *                    layer.smartObjects.stackMode.* (image-stack statistics).
 * edit_menu_cmds.c: edit.purge.* (free undo/clipboard/history memory).
 * color_to_alpha_cmds.c: filter.other.colorToAlpha. */
const pc_engine_command *pc_engine_tool_colors_specs(size_t *n);
const pc_engine_command *pc_engine_auto_adjust_specs(size_t *n);
const pc_engine_command *pc_engine_layer_menu_specs(size_t *n);
const pc_engine_command *pc_engine_edit_menu_specs(size_t *n);
const pc_engine_command *pc_engine_color_to_alpha_specs(size_t *n);
/* video_cmds.c: write every video layer's displayed content back into its playhead frame (store),
 * and set every video layer's displayed content to its playhead frame (sync). Used by
 * timeline_cmds navigation. */
void pc_engine_video_store(pc_document *doc);
void pc_engine_video_sync(pc_document *doc);
/* File > Import > Notes... from file bytes (notes_cmds.c); exported for the UI file picker. */
pc_status pc_engine_import_notes_from(pc_engine_session *s, const char *name, const uint8_t *bytes,
                                      size_t len, pc_json_value *out, pc_error *err);

/* ---- colour management (color_cmds.c, E8) ------------------------------------------------- *
 * ColorState lifecycle, the command specs, and the conversion primitives E3's Image > Mode and
 * E4 depend on. */
const pc_engine_command *pc_engine_color_specs(size_t *n);
/* Image > Mode > RGB/Grayscale/CMYK/Lab through the CMS (Rust color_cmds::convert_mode). E3's
 * image.mode.* commands call this. Writes *out on success. */
pc_status pc_engine_color_convert_mode(pc_engine_session *s, pc_color_mode mode, yyjson_val *p,
                                       pc_json_value *out, pc_error *err);
/* Convert every colour surface and colour value of `doc` from its profile to `dst`, switching the
 * mode and tagging the document (Rust convert_document). The document is left unchanged on error. */
pc_status pc_engine_convert_document(pc_document *doc, const pc_profile *dst, pc_intent intent, bool bpc,
                                     pc_error *err);
/* The document's profile (embedded if it parses and matches the mode, else the working profile).
 * *out is an owned clone (pc_profile_fini). */
pc_status pc_engine_document_profile(const pc_document *doc, pc_profile *out, pc_error *err);
/* The compositor's RGB output profile for `doc` (RGB: document profile; Gray: its gray-as-RGB
 * view; CMYK/Lab: sRGB). *out owned. */
pc_status pc_engine_composite_profile(const pc_document *doc, pc_profile *out, pc_error *err);
/* Built-in working (default) profile of a mode (borrowed, process-static). */
const pc_profile *pc_engine_working_profile(pc_color_mode mode);
/* Resolve a profile spec ("working*" aliases from Color Settings, a built-in id/description,
 * "document", or an .icc path). *out owned. */
pc_status pc_engine_resolve_profile(const pc_color_state *cs, const char *spec, const pc_document *doc,
                                    bool has_mode, pc_color_mode mode, pc_profile *out, pc_error *err);
/* Prefs routing for colorSettings (prefs.c seam, E2a): serialise the whole object into *out; apply
 * one camelCase key into `next`; load a whole object leniently into the state. */
pc_status pc_engine_color_settings_to_mut(const pc_color_state *cs, yyjson_mut_doc *d, yyjson_mut_val **out);
pc_status pc_engine_color_settings_apply(pc_color_settings *next, const char *key, yyjson_val *v,
                                         pc_error *err);
pc_status pc_engine_color_settings_load(pc_color_state *cs, yyjson_val *obj, pc_error *err);
/* prefs.get: the whole object (subpath "") or one camelCase key. */
pc_status pc_engine_color_settings_get(const pc_color_state *cs, const char *subpath, pc_json_value *out,
                                       pc_error *err);
/* prefs.set: apply a colorSettings subpath ("" = whole-object value) into `next`. */
pc_status pc_engine_color_settings_set_path(pc_color_settings *next, const char *subpath,
                                            yyjson_val *value, pc_error *err);
/* prefs.reset: reset one colorSettings key ("" = the whole object) to its default in `next`. */
pc_status pc_engine_color_settings_reset_key(pc_color_settings *next, const char *key, pc_error *err);

/* param helpers (command.c), shared with command modules. */
bool pc_engine_param_int(yyjson_val *p, const char *key, int64_t *out);       /* accepts floats; rounds */
pc_status pc_engine_param_int_i32(const char *cmd, yyjson_val *p, const char *key, bool *present,
                                  int32_t *out, pc_error *err);
pc_status pc_engine_param_u32_id(const char *cmd, const char *key, yyjson_val *v, uint32_t *out, pc_error *err);
void pc_engine_param_color(yyjson_val *p, const char *key, const float def[4], float out[4]);
bool pc_engine_parse_hex(const char *s, size_t len, float out[4]);
yyjson_mut_val *pc_engine_inject_kind(const char *id, yyjson_mut_doc *doc, yyjson_val *params);

/* pixel helpers (pixels.c). Remap a surface through a pixel-coordinate mapping (flips/rotations);
 * the returned surface is owned by the caller (pc_surface_fini). On OOM returns a valid empty
 * surface and fills err. */
pc_surface pc_engine_remap_surface(const pc_surface *s,
                                   void (*map)(int32_t, int32_t, int32_t *, int32_t *, void *), void *ctx,
                                   pc_error *err);
/* E5 (pixels.c): composite the visible layers of `solo` into a tiled surface (effects included);
 * `background` is three channels or NULL. On OOM returns a valid empty surface and fills err. */
pc_status pc_engine_composite_layers(const pc_document *solo, pc_pixel_format fmt,
                                     const float *background, pc_surface *out, pc_error *err);
/* E5 (pixels.c): Merge Down -- `upper` onto `lower` into one raster layer keeping the lower
 * layer's id, blend and locks at full opacity. *out untouched on error. */
pc_status pc_engine_merge_down(const pc_document *doc, const pc_layer *lower, const pc_layer *upper,
                               pc_layer *out, pc_error *err);
/* Move a layer's pixels, linked mask, text and smart-object placement by whole pixels (vectors
 * move separately through pc_engine_translate_vectors). Groups recurse, artboards carry their
 * rect. Port of commands::translate_layer; exported here for E4 edit/copy and E11. Never fails
 * (best-effort on OOM, leaving the layer as it was). (layer_base_cmds.c, this chunk.) */
void pc_engine_translate_layer(const pc_document *doc, pc_layer *l, int32_t dx, int32_t dy);
/* Fill `area` of `s` with a straight-RGBA `color` through the selection's coverage (NULL = all
 * covered); lock_transparency keeps each pixel's alpha (E4, pixels.c try_fill_surface). */
pc_status pc_engine_try_fill_surface(pc_surface *s, pc_rect area, const float color[4],
                                     const pc_surface *selection, bool lock_transparency, pc_error *err);
/* Clear (make transparent) `area` of `s` through the selection's coverage (E4, try_clear_surface). */
pc_status pc_engine_try_clear_surface(pc_surface *s, pc_rect area, const pc_surface *selection, pc_error *err);
/* E8 (pixels.c): apply an adjustment destructively to a surface, weighted by an optional selection
 * (NULL = fully covered); `mode` is the document's colour model (the tone transfer). Any colour
 * model and bit depth, routed through compose adjust (pixels::adjust_surface). Best-effort on OOM
 * (leaves the surface unchanged or partly adjusted); never crashes. */
void pc_engine_adjust_surface(pc_surface *s, const pc_adjustment *adj, const pc_surface *selection,
                              pc_color_mode mode);
/* E8 (pixels.c): apply an adjustment to a layer mask through the selection (grayscale). Without a
 * selection the mask's default value is adjusted too, so Invert turns reveal-all into hide-all
 * (pixels::adjust_mask). */
void pc_engine_adjust_mask(pc_surface *s, const pc_adjustment *adj, const pc_surface *selection);
/* E8 (pixels.c): `edited` mixed over `orig` (straight RGBA, `count` pixels over `r`, row-major) by
 * the selection's coverage, encoded in `fmt` into *out (an owned float array the caller free()s,
 * *out_len values). pixels::mix_through_selection. */
pc_status pc_engine_mix_through_selection(pc_pixel_format fmt, pc_rect r, const float (*orig)[4],
                                          const float (*edited)[4], size_t count,
                                          const pc_surface *selection, float **out, size_t *out_len,
                                          pc_error *err);

/* enabled predicates (command.c), shared with command modules. */
pc_status pc_engine_has_doc(const pc_engine_session *s, pc_error *reason);
pc_status pc_engine_has_layer(const pc_engine_session *s, pc_error *reason);
const pc_layer *pc_engine_active_layer_of(const pc_engine_session *s, pc_error *reason);

/* ---- canvas geometry (canvas_geom.c) ------------------------------------------------------ *
 * Image > Image Rotation (right angles, flips) moves everything in the document, not only pixels.
 * E3's image commands call pc_engine_turn_canvas; the vector/type/smart content is moved through
 * the transform hooks above (noop until E6/E11). */
typedef enum pc_engine_turn {
    PC_ENGINE_TURN_FLIP_H,
    PC_ENGINE_TURN_FLIP_V,
    PC_ENGINE_TURN_ROTATE_180,
    PC_ENGINE_TURN_CW90,
    PC_ENGINE_TURN_CCW90
} pc_engine_turn;

pc_affine pc_engine_turn_affine(pc_engine_turn t, double w, double h);
bool pc_engine_turn_swaps(pc_engine_turn t);
/* A gradient angle (degrees, CCW on screen) carried through a's linear part. */
float pc_engine_map_angle(const pc_affine *a, float deg);
/* Bounding box of the four mapped corners of r (rounded, clamped to half the i32 range). */
pc_rect pc_engine_map_rect(const pc_affine *a, pc_rect r);
/* Move all non-pixel geometry of the document through a (layers + document marks). */
void pc_engine_transform_geometry(pc_document *doc, const pc_affine *a);
/* Move one layer's non-pixel geometry (canvas_geom::transform_layer_geometry): with `content`,
 * its content path/transform too; without, only what Free Transform leaves alone (unlinked masks,
 * fill angles, artboards). Used by Image Rotation › Arbitrary (mode_cmds, ET3). */
void pc_engine_transform_layer_geometry(pc_layer *l, const pc_affine *a, bool content, float dpi);
/* image_cmds::translate_doc: move the whole document (every surface, channel, selection, geometry)
 * by (dx, dy), refused (unchanged) when a pixel would leave the 32-bit range. Used by artboards. */
pc_status pc_engine_translate_doc(pc_document *doc, const char *cmd, int32_t dx, int32_t dy,
                                  pc_error *err);
/* commands::layer_copy: deep-copy layer `id` with fresh ids and a "copy" name (*out initialised on
 * success). Used by Duplicate Artboard (artboard_cmds, ET3). */
pc_status pc_engine_layer_copy(const pc_document *doc, pc_layer_id id, pc_layer *out, pc_error *err);
/* file_cmds file-name helpers for the artboard exports (ET3). */
void pc_engine_file_stem(const char *path, char *out, size_t cap);
char *pc_engine_file_join(const char *dir, const char *name);
pc_status pc_engine_file_sanitize(pc_buf *out, const char *name, pc_error *err);
/* canvas_geom::refresh: re-render shape caches (always) and, when `all`, type and smart caches too,
 * from a snapshot of the just-moved document. Best-effort; never crashes. */
void pc_engine_canvas_refresh(pc_document *doc, bool all);
/* Canvas size after rotating `size` by `deg` degrees (grows to fit; mode_cmds::rotated_size). */
pc_size pc_engine_rotated_size(pc_size size, double deg);

/* File > Automate computational-photography commands (automate_photo_cmds.c, ET3). */
const pc_engine_command *pc_engine_photo_automate_specs(size_t *n);

/* Run several commands as one history step labelled `label` (analysis_cmds::compound): on failure
 * the document goes back as it was and the journal forgets the commands f ran. f drives the session
 * through the execute / edit entry points. Used by the rotated Crop (image_cmds) and Straighten
 * Layer (analysis_cmds). Never crashes. */
typedef pc_status (*pc_engine_compound_fn)(pc_engine_session *s, void *ctx, pc_error *err);
pc_status pc_engine_compound(pc_engine_session *s, const char *label, pc_engine_compound_fn f,
                             void *ctx, pc_error *err);
/* Document-level marks: paths, work path, notes, count/ruler marks, slices and guides. */
void pc_engine_transform_doc_marks(pc_document *doc, const pc_affine *a);
/* Turn or flip the whole canvas: pixels and caches remapped exactly, geometry through the map. */
pc_status pc_engine_turn_canvas(pc_document *doc, pc_engine_turn turn, pc_error *err);

/* =====================================================================================
 * HOOK SEAM: behaviour owned by later engine chunks, declared here with E1-safe defaults in
 * hooks.c. When a later chunk implements the real behaviour it MOVES that function's definition
 * out of hooks.c into its own module (deleting the default) -- a surgical edit, no duplicate
 * symbols. Every default is chosen so the engine never crashes and E1's own commands behave
 * correctly (REWRITE_PLAN.md rule 9).
 * ===================================================================================== */

/* Fill in the Channels-panel target on `params` (a mutable value in `doc`) for a pixel command
 * (channel_cmds, E7). Returns the (possibly new) value. Default: return params unchanged. */
yyjson_mut_val *pc_engine_hook_inject_target(const pc_engine_session *s, const char *id,
                                             yyjson_mut_doc *doc, yyjson_mut_val *params);
/* Validate params against the spec's documented shape (param_check, later). Default: PC_OK. */
pc_status pc_engine_hook_param_check(const pc_engine_command *spec, yyjson_val *params, pc_error *err);
/* Filter menu: last-filter redirect and param validation (filters, E9). *redirect=false default. */
pc_status pc_engine_hook_filter_validate(const char *id, yyjson_val *params, pc_error *err);
/* Smart-filter subtypes pc_engine_apply_filter_to_surface dispatches before the core filter table:
 * Liquify / Puppet / Perspective Warp (E4 distort_cmds), Lens Correction / Camera Raw (E6
 * lens_cmds), image.adjustments.* recorded as a smart filter (E8 adjust_cmds), and WASM plug-in
 * filters (E12 plugin_cmds). On a match set *applied=true and initialise *out (the filtered
 * surface); default leaves *applied=false. Never crashes. */
pc_status pc_engine_hook_filter_subtype(const char *id, yyjson_val *params, const pc_surface *surf,
                                        pc_rect bounds, const pc_surface *selection, pc_rect canvas,
                                        pc_surface *out, bool *applied, pc_error *err);
/* file.new param validation (document_preset_cmds, E3). Default: PC_OK. */
pc_status pc_engine_hook_validate_new_params(pc_engine_session *s, yyjson_val *params, pc_error *err);
/* Drop a floating selection before any other command (float_cmds, E4). *handled=false default. */
pc_status pc_engine_hook_float_before_command(pc_engine_session *s, const char *id, bool *handled,
                                              pc_json_value *out, pc_error *err);
/* Colour-channel restriction for id (channel_cmds, E7). Returns SIZE_MAX = none (default). */
size_t pc_engine_hook_color_restriction(const pc_engine_session *s, const char *id, yyjson_val *params);
/* In edit(): restrict a just-made change to one colour channel (channel_cmds, E7). Default: noop. */
void pc_engine_hook_restrict_to_color(const pc_document *before, pc_document *after, pc_layer_id id,
                                      size_t channel);
/* In edit()/undo()/redo(): keep channel-view state valid after a document change (E7). Default: noop. */
void pc_engine_hook_fix_view(pc_engine_doc_state *st);
/* Pixels an edit can have changed, as undo would report them (layer_multi_cmds::step_damage, E5).
 * *present=false default (= assume everything changed). */
void pc_engine_hook_step_damage(const pc_document *before, const pc_document *after, bool *present,
                                pc_rect *out);
/* A command stands for the commands it ran itself and records as one step (actions_cmds, E12).
 * Default: false. */
bool pc_engine_hook_replayable(const char *id);
/* Post-command bookkeeping hooks (default: noop). */
void pc_engine_hook_mask_view_fix(pc_engine_doc_state *st);                 /* E5 */
void pc_engine_hook_edit_menu_after_command(pc_engine_session *s, const char *id);  /* E4 */
void pc_engine_hook_automate_after_command(pc_engine_session *s, const char *id);   /* E12 */
void pc_engine_hook_sync_preset_store(pc_engine_session *s);                /* E2 */
void pc_engine_hook_smart_on_close(pc_engine_session *s, size_t index);     /* E6 */
/* Coalesce a gesture's journal entries into one (brush_cmds, E10). Default: false (don't). */
bool pc_engine_hook_coalesce_journal(pc_engine_session *s, const char *id, yyjson_val *params);
/* hidden_target: why a paint/move command must not run (its target layer is hidden). Returns the
 * message or NULL. This one is implemented in E1 (hidden_target.c), not a default. */
const char *pc_engine_hidden_target_refusal(const pc_engine_session *s, const char *id, yyjson_val *params);

/* inspect hooks (channels_json E7, mask-view E5, adjustment serde E8): fill the named key of the
 * mutable object `obj` in `doc`. Defaults add nothing (E1 reports the structural fields only). */
void pc_engine_hook_inspect_channels(yyjson_mut_doc *doc, yyjson_mut_val *obj, const pc_engine_doc_state *d);
void pc_engine_hook_inspect_mask_view(yyjson_mut_doc *doc, yyjson_mut_val *obj, const pc_engine_doc_state *d);
void pc_engine_hook_inspect_adjustment(yyjson_mut_doc *doc, yyjson_mut_val *obj, const pc_layer *l);

/* snap: the Free-Transform bounds of a layer for snapping (transform_cmds::transform_bounds, E4).
 * Default: the layer's surface content bounds (correct for raster layers). */
pc_rect pc_engine_hook_transform_bounds(const pc_document *doc, const pc_layer *l);

/* canvas_geom: move the non-pixel geometry of vector/text/smart content (E4/E6/E11). Defaults noop. */
void pc_engine_hook_transform_shape(pc_layer *l, const pc_affine *a);                 /* E11 vector */
void pc_engine_hook_transform_text(pc_layer *l, const pc_affine *a, float dpi);       /* E11 type */
void pc_engine_hook_transform_smart(pc_layer *l, const pc_affine *a);                 /* E6 smart */
void pc_engine_hook_refresh_shape(const pc_document *snapshot, pc_layer *l);          /* E11 */
void pc_engine_hook_refresh_text(const pc_document *snapshot, pc_layer *l);           /* E11 */
void pc_engine_hook_refresh_smart(const pc_document *snapshot, pc_layer *l);          /* E6 */

#endif /* PC_ENGINE_INTERNAL_H */
