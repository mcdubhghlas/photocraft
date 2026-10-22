/* Image > Analysis tools and panels view-state and headless model (port of the portable half of
 * crates/ui-egui/src/analysis_ui.rs): the Ruler, Count and Note tools' pointer routing and canvas
 * overlay geometry, the Note tool options, the Measurement Log / Notes panel visibility and row
 * selection, and the Set Measurement Scale / Select Data Points / Place Scale Marker dialog fields.
 *
 * Every mutation goes through the engine's analysis commands (`image.analysis.*`, `count.*`,
 * `notes.*`); this struct only holds view state and maps pointer input, so it is headless-testable.
 * The Color Sampler tool is handled by src/ui/sampling_tool.c (and drawn by sampling_ui.c); it is
 * not duplicated here. Nothing here depends on the GUI toolkit. */
#ifndef PC_UI_ANALYSIS_H
#define PC_UI_ANALYSIS_H

#include "ui/state.h"
#include "ui/shortcuts.h"
#include "ui/tool_gesture.h"
#include "engine/engine.h"
#include "doc/analysis.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What a pointer drag is doing (runtime only, never serialised). */
typedef enum pc_ui_analysis_drag {
    PC_UI_ANALYSIS_DRAG_NONE = 0,
    PC_UI_ANALYSIS_DRAG_RULER_NEW,   /* a fresh line being dragged out */
    PC_UI_ANALYSIS_DRAG_RULER_START, /* moving handle 0 (start) */
    PC_UI_ANALYSIS_DRAG_RULER_END,   /* moving handle 1 (end) */
    PC_UI_ANALYSIS_DRAG_RULER_PROT,  /* moving handle 2 (protractor arm end) */
    PC_UI_ANALYSIS_DRAG_COUNT,       /* moving a count marker */
    PC_UI_ANALYSIS_DRAG_NOTE         /* moving a note icon */
} pc_ui_analysis_drag;

typedef enum pc_ui_analysis_dialog {
    PC_UI_ANALYSIS_DIALOG_NONE = 0,
    PC_UI_ANALYSIS_DIALOG_SCALE,
    PC_UI_ANALYSIS_DIALOG_DATA_POINTS,
    PC_UI_ANALYSIS_DIALOG_SCALE_MARKER
} pc_ui_analysis_dialog;

/* One Select-Data-Points checkbox row (bounded, no allocation). */
#define PC_UI_ANALYSIS_DP_MAX 128
typedef struct pc_ui_analysis_dp_row {
    char source[12]; /* "selection" | "ruler" | "count" */
    char key[32];
    char name[64];
    bool selected;
} pc_ui_analysis_dp_row;

/* The analysis tools' and panels' view state plus the in-progress drag. */
typedef struct pc_ui_analysis {
    /* Window > Measurement Log / Notes panels. */
    bool measurement_log;
    bool notes_panel;
    /* Note tool options: author and colour of new notes. */
    char note_author[128];
    float note_color[3];
    /* Ruler options bar: show values in the measurement scale's units. */
    bool use_measurement_scale;
    /* Notes panel: the note shown. */
    bool has_note_selected;
    size_t note_selected;
    /* Measurement Log: selected row ids. */
    uint64_t *log_selected;
    size_t nlog_selected, cap_log_selected;

    /* In-progress drag (not serialised). */
    pc_ui_analysis_drag drag;
    size_t drag_group, drag_index;
    double drag_from[2]; /* count: press point; note: grab offset from the icon origin */

    /* Open dialog and its fields. */
    pc_ui_analysis_dialog dialog;
    /* Set Measurement Scale. */
    bool scale_default;
    double scale_pixel, scale_logical;
    char scale_units[64];
    /* Place Scale Marker. */
    double marker_length, marker_font;
    bool marker_display_text;
    int marker_text_position; /* 0 bottom, 1 top */
    int marker_color;         /* 0 black, 1 white */
    char marker_units[64];
    /* Select Data Points. */
    pc_ui_analysis_dp_row dp_rows[PC_UI_ANALYSIS_DP_MAX];
    size_t dp_nrows;
} pc_ui_analysis;

void pc_ui_analysis_init(pc_ui_analysis *a);
void pc_ui_analysis_fini(pc_ui_analysis *a);

/* True for the tools this module routes (Ruler, Count, Note). */
bool pc_ui_analysis_tool(pc_ui_tool tool);

/* ---- pure geometry / math (headless-testable) ---- */

double pc_ui_analysis_dist(const double a[2], const double b[2]);
/* Shift constrains `to` to 45-degree steps around `from`. */
void pc_ui_analysis_constrain45(const double from[2], const double to[2], bool shift, double out[2]);
/* Hit tolerance in document pixels for the given view zoom. */
double pc_ui_analysis_tolerance(double zoom);
/* A "nice" scale-marker bar length near a fifth of the image width. */
double pc_ui_analysis_suggested_length(uint32_t width, const pc_measurement_scale *scale);

/* Ruler readout for the options bar (x,y,w,h,angle,l1,l2). angle has no scale factor; the lengths
 * are multiplied by the scale factor when `use_scale`. l2 is NAN without a protractor arm. Returns
 * false (leaving `out` untouched) when `r` is NULL. */
typedef struct pc_ui_ruler_readout {
    double x, y, w, h, angle, l1, l2;
    bool has_protractor;
} pc_ui_ruler_readout;
bool pc_ui_analysis_ruler_readout(const pc_ruler *r, const pc_measurement_scale *scale, bool use_scale,
                                  pc_ui_ruler_readout *out);

/* The nearest visible count marker within `tol`; writes (group,index) and returns true. */
bool pc_ui_analysis_nearest_count(const pc_measurement *m, const double at[2], double tol, size_t *group,
                                  size_t *index);
/* The topmost note whose icon box contains `at` (icon is 16/zoom by 20/zoom document px); returns the
 * index or -1. */
long pc_ui_analysis_nearest_note(const pc_note *notes, size_t nnotes, const double at[2], double zoom);

/* ---- Measurement Log row selection (operate on the runtime) ---- */

bool pc_ui_analysis_log_contains(const pc_ui_analysis *a, uint64_t id);
void pc_ui_analysis_log_clear(pc_ui_analysis *a);
/* Add or remove `id`. Returns PC_OK, or PC_ERR_NOMEM if the list cannot grow. */
pc_status pc_ui_analysis_log_toggle(pc_ui_analysis *a, uint64_t id);
/* Replace the selection with `ids` (Select All). */
pc_status pc_ui_analysis_log_set(pc_ui_analysis *a, const uint64_t *ids, size_t n);

/* ---- pointer routing (Ruler, Count, Note) ---- */

/* Routes a pointer event for the Ruler, Count and Note tools, running the matching engine commands.
 * Returns PC_ERR_NOT_FOUND when the active tool is not one of these (so the caller tries the next
 * handler); otherwise PC_OK (the event is consumed, possibly as a no-op) or the engine's error. */
pc_status pc_ui_analysis_pointer(pc_ui_analysis *a, pc_engine_session *session, const pc_ui_state *ui,
                                 pc_ui_pointer_kind kind, pc_ui_tool_point point, unsigned modifiers,
                                 double zoom, pc_json_value *out, pc_error *err);

#endif
