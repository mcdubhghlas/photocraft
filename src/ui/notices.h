/* Non-blocking notices: a small stack of cards in the lower-right corner for import/export
 * warnings, failed operations and slow-refresh explanations. Port of the data model + auto-hide
 * timer logic of crates/ui-egui/src/notices.rs; the egui card rendering is rebuilt in the shell,
 * and the Wayland-drop guidance (needs the Services struct) is deferred (§7).
 *
 * Pure logic + i18n; unit-testable headlessly. */
#ifndef PC_UI_NOTICES_H
#define PC_UI_NOTICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"

/* At most this many notices are kept; older temporary ones drop off. */
#define PC_UI_MAX_NOTICES 3
/* Lines shown per notice before "...and N more". */
#define PC_UI_NOTICE_MAX_LINES 8
/* The GPU-canvas reason string that triggers the memory-budget hint (mirrors gpu_canvas's
 * OVER_BUDGET; U4's gpu canvas uses the same text). */
#define PC_UI_GPU_OVER_BUDGET "layers exceed the GPU memory budget; full refresh on the CPU"

typedef struct pc_ui_notice_arg {
    char *key;
    char *val;
} pc_ui_notice_arg;

typedef struct pc_ui_notice {
    uint64_t id;
    char *title;         /* English source (translated and arg-filled when drawn) */
    char **lines;        /* owned */
    size_t n_lines;
    bool error;          /* warning colour */
    char *dismiss_pref;  /* preference key to set when dismissed, or NULL */
    pc_ui_notice_arg *args;
    size_t n_args;
    bool has_shown_at;   /* the auto-hide timer start, stamped on first draw */
    double shown_at;
} pc_ui_notice;

typedef struct pc_ui_notices {
    pc_ui_notice *items;
    size_t n;
    uint64_t *slow_noticed; /* document ids already explained */
    size_t n_slow;
    uint64_t next_id;       /* own id counter (the shell wires this to UiState::alloc_id) */
} pc_ui_notices;

void pc_ui_notices_init(pc_ui_notices *ns);
void pc_ui_notices_fini(pc_ui_notices *ns);

/* Post a notice (newest last); returns its id. `lines` is copied. Caps the stack. */
uint64_t pc_ui_notices_post(pc_ui_notices *ns, const char *title, const char *const *lines,
                            size_t n_lines, bool error, const char *dismiss_pref);

/* Remove the notice `id`; returns its dismiss_pref (borrowed into a caller buffer `pref_out`,
 * empty if none) so the shell can persist it. Returns true if a notice was removed. */
bool pc_ui_notices_dismiss(pc_ui_notices *ns, uint64_t id, char *pref_out, size_t pref_cap);

/* Auto-hide: drop notices whose delay elapsed, pausing timers while hovered. Returns the seconds
 * until the next notice expires (writing *has_next true), or *has_next false when none pending. */
double pc_ui_notices_expire(pc_ui_notices *ns, double now, bool hovered, bool just_left,
                            bool auto_hide, double duration, bool *has_next);

/* Report import/export `warnings` for `what` (e.g. "Opened a.psd"): writes the status-bar line
 * into `status_out` and posts a notice listing them. Does nothing when there are none. */
void pc_ui_notices_io_warnings(pc_ui_notices *ns, const char *what, const char *const *warnings,
                               size_t n, pc_buf *status_out, bool *status_error);

/* A full refresh of `doc` fell back to the CPU and took `ms`. Once per document, post an
 * explanation. `reason` is the GPU's reason (NULL = none). Returns true if a notice was posted. */
bool pc_ui_notices_slow_cpu_refresh(pc_ui_notices *ns, uint64_t doc, double ms, const char *reason);

/* `s` (a title or line) translated into the current language with the notice's args filled in. */
void pc_ui_notice_text(const pc_ui_notice *n, const char *s, pc_buf *out);

#endif /* PC_UI_NOTICES_H */
