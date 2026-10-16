/* UI state as plain data (port of crates/ui-egui/src/state.rs), so the control channel and MCP can
 * read and drive the interface: tools, zoom, panels, dialogs and windows. Serde field names and
 * enum representations are kept byte-for-byte with the reference.
 *
 * SCOPE (U1): the self-contained foundation — the Tool registry and the plain view-state structs
 * (Panels, View, DocWindow, Dialog, Extras, DockTabs, ColorPanel, Camera Raw scope/preview,
 * PointCurve) and a UiState aggregate carrying the fields U1 owns. Fields whose types belong to
 * later UI chunks (ToolOptions, dock/docking layout, per-panel state, in-progress gestures) are
 * added by those chunks at the marked extension points; their serde names are already reserved in
 * state.rs. No GUI dependency. */
#ifndef PC_UI_STATE_H
#define PC_UI_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/status.h"
#include "ui/theme.h"

/* ---- Tool (state.rs Tool) ---- */
typedef enum pc_ui_tool {
    PC_TOOL_MOVE = 0,
    PC_TOOL_ARTBOARD,
    PC_TOOL_RECT_MARQUEE,
    PC_TOOL_ELLIPSE_MARQUEE,
    PC_TOOL_SINGLE_ROW_MARQUEE,
    PC_TOOL_SINGLE_COLUMN_MARQUEE,
    PC_TOOL_LASSO,
    PC_TOOL_POLYGON_LASSO,
    PC_TOOL_MAGNETIC_LASSO,
    PC_TOOL_MAGIC_WAND,
    PC_TOOL_CROP,
    PC_TOOL_PERSPECTIVE_CROP,
    PC_TOOL_EYEDROPPER,
    PC_TOOL_COLOR_SAMPLER,
    PC_TOOL_RULER,
    PC_TOOL_NOTE,
    PC_TOOL_COUNT_TOOL,
    PC_TOOL_BRUSH,
    PC_TOOL_PENCIL,
    PC_TOOL_MIXER_BRUSH,
    PC_TOOL_COLOR_REPLACEMENT,
    PC_TOOL_ERASER,
    PC_TOOL_BACKGROUND_ERASER,
    PC_TOOL_MAGIC_ERASER,
    PC_TOOL_GRADIENT,
    PC_TOOL_PAINT_BUCKET,
    PC_TOOL_TYPE,
    PC_TOOL_VERTICAL_TYPE,
    PC_TOOL_HORIZONTAL_TYPE_MASK,
    PC_TOOL_VERTICAL_TYPE_MASK,
    PC_TOOL_HAND,
    PC_TOOL_ROTATE_VIEW,
    PC_TOOL_ZOOM,
    PC_TOOL_REMOVE,
    PC_TOOL_SPOT_HEALING,
    PC_TOOL_HEALING,
    PC_TOOL_PATCH,
    PC_TOOL_CONTENT_AWARE_MOVE,
    PC_TOOL_RED_EYE,
    PC_TOOL_CLONE_STAMP,
    PC_TOOL_PATTERN_STAMP,
    PC_TOOL_HISTORY_BRUSH,
    PC_TOOL_BLUR,
    PC_TOOL_SHARPEN,
    PC_TOOL_SMUDGE,
    PC_TOOL_DODGE,
    PC_TOOL_BURN,
    PC_TOOL_SPONGE,
    PC_TOOL_QUICK_SELECTION,
    PC_TOOL_OBJECT_SELECTION,
    PC_TOOL_PEN,
    PC_TOOL_FREEFORM_PEN,
    PC_TOOL_CURVATURE_PEN,
    PC_TOOL_ADD_ANCHOR_POINT,
    PC_TOOL_DELETE_ANCHOR_POINT,
    PC_TOOL_CONVERT_POINT,
    PC_TOOL_PATH_SELECTION,
    PC_TOOL_DIRECT_SELECTION,
    PC_TOOL_RECTANGLE,
    PC_TOOL_ELLIPSE_SHAPE,
    PC_TOOL_TRIANGLE,
    PC_TOOL_POLYGON,
    PC_TOOL_LINE,
    PC_TOOL_CUSTOM_SHAPE,
    PC_TOOL_SLICE,
    PC_TOOL_SLICE_SELECT,
    PC_TOOL_FRAME,
    PC_TOOL_N /* count: 67 */
} pc_ui_tool;

extern const pc_ui_tool PC_UI_TOOL_ALL[PC_TOOL_N];

const char *pc_ui_tool_label(pc_ui_tool t);        /* "Move Tool" */
const char *pc_ui_tool_serde(pc_ui_tool t);        /* serde variant name "Move", "RectMarquee" */
bool pc_ui_tool_from_serde(const char *s, pc_ui_tool *out);
char pc_ui_tool_key(pc_ui_tool t);                 /* Photoshop single-key shortcut, '\0' for none */
const char *pc_ui_tool_glyph(pc_ui_tool t);        /* toolbar glyph */
bool pc_ui_tool_is_type(pc_ui_tool t);
bool pc_ui_tool_is_type_mask(pc_ui_tool t);
bool pc_ui_tool_is_vertical_type(pc_ui_tool t);
bool pc_ui_tool_is_brushlike(pc_ui_tool t);
/* Fuzzy name match (state.rs Tool::from_name): case/sep-insensitive debug name, label or
 * "marquee"-stripped. Returns true and writes *out on a match. */
bool pc_ui_tool_from_name(const char *s, pc_ui_tool *out);

/* ---- enums ---- */
typedef enum pc_ui_dialog_kind {
    PC_DIALOG_NEW_DOCUMENT = 0,
    PC_DIALOG_ABOUT,
    PC_DIALOG_COMMAND,
    PC_DIALOG_ERROR,
    PC_DIALOG_LAYER_STYLE
} pc_ui_dialog_kind;
const char *pc_ui_dialog_kind_serde(pc_ui_dialog_kind k); /* "NewDocument", ... */
bool pc_ui_dialog_kind_from_serde(const char *s, pc_ui_dialog_kind *out);

typedef enum pc_ui_transform_mode {
    PC_TRANSFORM_FREE = 0,
    PC_TRANSFORM_ROTATE,
    PC_TRANSFORM_SKEW,
    PC_TRANSFORM_DISTORT,
    PC_TRANSFORM_PERSPECTIVE
} pc_ui_transform_mode;
const char *pc_ui_transform_mode_serde(pc_ui_transform_mode m); /* camelCase "free",... */
/* The mode an edit.transform.* / edit.freeTransform id starts. */
pc_ui_transform_mode pc_ui_transform_mode_for_command(const char *id);

/* ---- plain view-state structs ---- */
typedef struct pc_ui_panels {
    bool layers, history, properties, color, navigator, toolbar, options_bar, status_bar;
    bool brush_settings, character, toolbar_double, dock;
    bool has_hidden_by_tab;
    bool hidden_by_tab[3];
    bool rail, menu_bar, gradient_editor;
} pc_ui_panels;
void pc_ui_panels_default(pc_ui_panels *p);

typedef struct pc_ui_view {
    float zoom;
    float center[2];
    bool fit_pending;
    bool fill_pending;
    uint32_t doc_size[2];
    float rotation;
} pc_ui_view;
void pc_ui_view_default(pc_ui_view *v);

typedef struct pc_ui_doc_window {
    uint64_t id;
    size_t document;
    pc_ui_view view;
    bool open;
    bool has_position;
    float position[2];
} pc_ui_doc_window;

typedef struct pc_ui_extras {
    bool rulers, grid, guides, snap, lock_guides;
} pc_ui_extras;
void pc_ui_extras_default(pc_ui_extras *e);

typedef struct pc_ui_dock_tabs {
    size_t properties, color, layers, navigator, history, character;
} pc_ui_dock_tabs;

typedef struct pc_ui_color_panel {
    bool background;
} pc_ui_color_panel;

typedef struct pc_ui_camera_raw_scope {
    bool shadows, highlights, lab, sampler_tool;
    float (*samplers)[2];
    size_t n_samplers;
    bool vectorscope, selected_region, red_right, hide_skin_line, floating;
    bool has_floating_rect;
    float floating_rect[4];
} pc_ui_camera_raw_scope;

typedef struct pc_ui_camera_raw_preview {
    bool has_zoom;
    float zoom;
    float center[2];
    bool hand;
} pc_ui_camera_raw_preview;
void pc_ui_camera_raw_preview_default(pc_ui_camera_raw_preview *p);

/* A dialog with editable fields (kind + an owned JSON object of field values). */
typedef struct pc_ui_dialog {
    uint64_t id;
    pc_ui_dialog_kind kind;
    yyjson_mut_doc *fields; /* a JSON object; owned */
} pc_ui_dialog;

/* ---- UiState aggregate (U1 subset) ---- */
typedef struct pc_ui_state {
    pc_ui_tool tool;
    pc_ui_tool *group_tools; /* last tool used per shortcut group */
    size_t n_group_tools;
    char **recent_files;
    size_t n_recent_files;
    bool mask_target, vector_mask_target, transform_unlinked;
    bool has_properties_layer;
    uint64_t properties_layer;
    bool has_layer_comp_selected;
    uint32_t layer_comp_selected;
    char *selected_path; /* NULL = none */
    char **layer_filter;
    size_t n_layer_filter;

    pc_ui_panels panels;
    pc_ui_extras extras;
    pc_ui_dock_tabs dock_tabs;
    pc_ui_color_panel color_panel;
    pc_ui_camera_raw_scope camera_raw_scope;
    pc_ui_camera_raw_preview camera_raw_preview;

    pc_ui_view *views; /* one per open document */
    size_t n_views;
    pc_ui_doc_window *windows;
    size_t n_windows;
    pc_ui_dialog *dialogs;
    size_t n_dialogs;

    pc_ui_theme_kind theme;
    char *workspace;
    bool palette_open;
    size_t brush_section, brush_tab;
    uint8_t selection_mode;

    char *status;       /* last status message */
    bool status_error;
    char *gpu_fallback_notice; /* NULL = none */

    double crop_angle;
    uint64_t next_id;

    /* EXTENSION REGION (later chunks): tool_options (U6/U7), dock/docking layout (U3),
     * per-panel state (U5), in-progress gestures (transform/text_edit/pen/magnetic/polygon),
     * notices/kys/analysis/timeline/slices/frame/artboard/shell. Their serde names are reserved
     * in state.rs; add fields and (de)serialisation here when their types land. */
} pc_ui_state;

/* Lifecycle. */
void pc_ui_state_init(pc_ui_state *s);  /* UiState::default() */
void pc_ui_state_fini(pc_ui_state *s);

/* Methods (state.rs impl UiState). */
uint64_t pc_ui_state_alloc_id(pc_ui_state *s);
void pc_ui_state_remember_group_tool(pc_ui_state *s);
bool pc_ui_state_group_tool(const pc_ui_state *s, char key, pc_ui_tool *out);
/* Open a dialog with the given JSON object of fields (copied); returns its id. */
uint64_t pc_ui_state_open_dialog(pc_ui_state *s, pc_ui_dialog_kind kind, yyjson_val *fields);
pc_ui_dialog *pc_ui_state_dialog_mut(pc_ui_state *s, uint64_t id);
bool pc_ui_state_close_dialog(pc_ui_state *s, uint64_t id);
/* The default New Document field values (serde_json object). Appends to `doc`, returns the root. */
yyjson_mut_val *pc_ui_state_new_document_fields(yyjson_mut_doc *doc);

/* Serialisation (the U1 subset; serde field names byte-exact). */
pc_status pc_ui_state_to_json(const pc_ui_state *s, yyjson_mut_doc *doc, yyjson_mut_val **out, pc_error *err);
pc_status pc_ui_state_from_json(pc_ui_state *s, yyjson_val *obj, pc_error *err);

#endif
