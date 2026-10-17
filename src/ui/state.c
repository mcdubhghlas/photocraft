/* UI state as plain data (port of crates/ui-egui/src/state.rs; U1 subset). */
#include "ui/state.h"

#include <stdlib.h>
#include <string.h>

#include "base/json.h"

/* ---- Tool tables (enum order == state.rs Tool::ALL) ---- */
const pc_ui_tool PC_UI_TOOL_ALL[PC_TOOL_N] = {
    PC_TOOL_MOVE, PC_TOOL_ARTBOARD, PC_TOOL_RECT_MARQUEE, PC_TOOL_ELLIPSE_MARQUEE, PC_TOOL_SINGLE_ROW_MARQUEE,
    PC_TOOL_SINGLE_COLUMN_MARQUEE, PC_TOOL_LASSO, PC_TOOL_POLYGON_LASSO, PC_TOOL_MAGNETIC_LASSO, PC_TOOL_MAGIC_WAND,
    PC_TOOL_CROP, PC_TOOL_PERSPECTIVE_CROP, PC_TOOL_EYEDROPPER, PC_TOOL_COLOR_SAMPLER, PC_TOOL_RULER, PC_TOOL_NOTE,
    PC_TOOL_COUNT_TOOL, PC_TOOL_BRUSH, PC_TOOL_PENCIL, PC_TOOL_MIXER_BRUSH, PC_TOOL_COLOR_REPLACEMENT, PC_TOOL_ERASER,
    PC_TOOL_BACKGROUND_ERASER, PC_TOOL_MAGIC_ERASER, PC_TOOL_GRADIENT, PC_TOOL_PAINT_BUCKET, PC_TOOL_TYPE,
    PC_TOOL_VERTICAL_TYPE, PC_TOOL_HORIZONTAL_TYPE_MASK, PC_TOOL_VERTICAL_TYPE_MASK, PC_TOOL_HAND, PC_TOOL_ROTATE_VIEW,
    PC_TOOL_ZOOM, PC_TOOL_REMOVE, PC_TOOL_SPOT_HEALING, PC_TOOL_HEALING, PC_TOOL_PATCH, PC_TOOL_CONTENT_AWARE_MOVE,
    PC_TOOL_RED_EYE, PC_TOOL_CLONE_STAMP, PC_TOOL_PATTERN_STAMP, PC_TOOL_HISTORY_BRUSH, PC_TOOL_BLUR, PC_TOOL_SHARPEN,
    PC_TOOL_SMUDGE, PC_TOOL_DODGE, PC_TOOL_BURN, PC_TOOL_SPONGE, PC_TOOL_QUICK_SELECTION, PC_TOOL_OBJECT_SELECTION,
    PC_TOOL_PEN, PC_TOOL_FREEFORM_PEN, PC_TOOL_CURVATURE_PEN, PC_TOOL_ADD_ANCHOR_POINT, PC_TOOL_DELETE_ANCHOR_POINT,
    PC_TOOL_CONVERT_POINT, PC_TOOL_PATH_SELECTION, PC_TOOL_DIRECT_SELECTION, PC_TOOL_RECTANGLE, PC_TOOL_ELLIPSE_SHAPE,
    PC_TOOL_TRIANGLE, PC_TOOL_POLYGON, PC_TOOL_LINE, PC_TOOL_CUSTOM_SHAPE, PC_TOOL_SLICE, PC_TOOL_SLICE_SELECT,
    PC_TOOL_FRAME,
};

static const char *const TOOL_SERDE[PC_TOOL_N] = {
    "Move", "Artboard", "RectMarquee", "EllipseMarquee", "SingleRowMarquee", "SingleColumnMarquee", "Lasso",
    "PolygonLasso", "MagneticLasso", "MagicWand", "Crop", "PerspectiveCrop", "Eyedropper", "ColorSampler", "Ruler",
    "Note", "Count", "Brush", "Pencil", "MixerBrush", "ColorReplacement", "Eraser", "BackgroundEraser", "MagicEraser",
    "Gradient", "PaintBucket", "Type", "VerticalType", "HorizontalTypeMask", "VerticalTypeMask", "Hand", "RotateView",
    "Zoom", "Remove", "SpotHealing", "Healing", "Patch", "ContentAwareMove", "RedEye", "CloneStamp", "PatternStamp",
    "HistoryBrush", "Blur", "Sharpen", "Smudge", "Dodge", "Burn", "Sponge", "QuickSelection", "ObjectSelection", "Pen",
    "FreeformPen", "CurvaturePen", "AddAnchorPoint", "DeleteAnchorPoint", "ConvertPoint", "PathSelection",
    "DirectSelection", "Rectangle", "EllipseShape", "Triangle", "Polygon", "Line", "CustomShape", "Slice", "SliceSelect",
    "Frame",
};

static const char *const TOOL_LABEL[PC_TOOL_N] = {
    "Move Tool", "Artboard Tool", "Rectangular Marquee Tool", "Elliptical Marquee Tool", "Single Row Marquee Tool",
    "Single Column Marquee Tool", "Lasso Tool", "Polygonal Lasso Tool", "Magnetic Lasso Tool", "Magic Wand Tool",
    "Crop Tool", "Perspective Crop Tool", "Eyedropper Tool", "Color Sampler Tool", "Ruler Tool", "Note Tool",
    "Count Tool", "Brush Tool", "Pencil Tool", "Mixer Brush Tool", "Color Replacement Tool", "Eraser Tool",
    "Background Eraser Tool", "Magic Eraser Tool", "Gradient Tool", "Paint Bucket Tool", "Horizontal Type Tool",
    "Vertical Type Tool", "Horizontal Type Mask Tool", "Vertical Type Mask Tool", "Hand Tool", "Rotate View Tool",
    "Zoom Tool", "Remove Tool", "Spot Healing Brush Tool", "Healing Brush Tool", "Patch Tool",
    "Content-Aware Move Tool", "Red Eye Tool", "Clone Stamp Tool", "Pattern Stamp Tool", "History Brush Tool",
    "Blur Tool", "Sharpen Tool", "Smudge Tool", "Dodge Tool", "Burn Tool", "Sponge Tool", "Quick Selection Tool",
    "Object Selection Tool", "Pen Tool", "Freeform Pen Tool", "Curvature Pen Tool", "Add Anchor Point Tool",
    "Delete Anchor Point Tool", "Convert Point Tool", "Path Selection Tool", "Direct Selection Tool", "Rectangle Tool",
    "Ellipse Tool", "Triangle Tool", "Polygon Tool", "Line Tool", "Custom Shape Tool", "Slice Tool",
    "Slice Select Tool", "Frame Tool",
};

const char *pc_ui_tool_label(pc_ui_tool t) { return (unsigned)t < PC_TOOL_N ? TOOL_LABEL[t] : TOOL_LABEL[0]; }
const char *pc_ui_tool_serde(pc_ui_tool t) { return (unsigned)t < PC_TOOL_N ? TOOL_SERDE[t] : TOOL_SERDE[0]; }

bool pc_ui_tool_from_serde(const char *s, pc_ui_tool *out)
{
    if (!s) { return false; }
    for (int i = 0; i < PC_TOOL_N; i++) {
        if (strcmp(s, TOOL_SERDE[i]) == 0) {
            *out = (pc_ui_tool)i;
            return true;
        }
    }
    return false;
}

char pc_ui_tool_key(pc_ui_tool t)
{
    switch (t) {
    case PC_TOOL_MOVE: case PC_TOOL_ARTBOARD: return 'V';
    case PC_TOOL_RECT_MARQUEE: case PC_TOOL_ELLIPSE_MARQUEE: return 'M';
    case PC_TOOL_BRUSH: case PC_TOOL_PENCIL: case PC_TOOL_MIXER_BRUSH: case PC_TOOL_COLOR_REPLACEMENT: return 'B';
    case PC_TOOL_ERASER: case PC_TOOL_BACKGROUND_ERASER: case PC_TOOL_MAGIC_ERASER: return 'E';
    case PC_TOOL_EYEDROPPER: case PC_TOOL_COLOR_SAMPLER: case PC_TOOL_RULER: case PC_TOOL_NOTE:
    case PC_TOOL_COUNT_TOOL: return 'I';
    case PC_TOOL_LASSO: case PC_TOOL_POLYGON_LASSO: case PC_TOOL_MAGNETIC_LASSO: return 'L';
    case PC_TOOL_MAGIC_WAND: return 'W';
    case PC_TOOL_CROP: case PC_TOOL_PERSPECTIVE_CROP: case PC_TOOL_SLICE: case PC_TOOL_SLICE_SELECT: return 'C';
    case PC_TOOL_FRAME: return 'K';
    case PC_TOOL_GRADIENT: case PC_TOOL_PAINT_BUCKET: return 'G';
    case PC_TOOL_TYPE: case PC_TOOL_VERTICAL_TYPE: case PC_TOOL_HORIZONTAL_TYPE_MASK:
    case PC_TOOL_VERTICAL_TYPE_MASK: return 'T';
    case PC_TOOL_HAND: return 'H';
    case PC_TOOL_ROTATE_VIEW: return 'R';
    case PC_TOOL_ZOOM: return 'Z';
    case PC_TOOL_REMOVE: case PC_TOOL_SPOT_HEALING: case PC_TOOL_HEALING: case PC_TOOL_PATCH:
    case PC_TOOL_CONTENT_AWARE_MOVE: case PC_TOOL_RED_EYE: return 'J';
    case PC_TOOL_CLONE_STAMP: case PC_TOOL_PATTERN_STAMP: return 'S';
    case PC_TOOL_HISTORY_BRUSH: return 'Y';
    case PC_TOOL_DODGE: case PC_TOOL_BURN: case PC_TOOL_SPONGE: return 'O';
    case PC_TOOL_QUICK_SELECTION: case PC_TOOL_OBJECT_SELECTION: return 'W';
    case PC_TOOL_PEN: case PC_TOOL_FREEFORM_PEN: case PC_TOOL_CURVATURE_PEN: case PC_TOOL_ADD_ANCHOR_POINT:
    case PC_TOOL_DELETE_ANCHOR_POINT: case PC_TOOL_CONVERT_POINT: return 'P';
    case PC_TOOL_PATH_SELECTION: case PC_TOOL_DIRECT_SELECTION: return 'A';
    case PC_TOOL_RECTANGLE: case PC_TOOL_ELLIPSE_SHAPE: case PC_TOOL_TRIANGLE: case PC_TOOL_POLYGON:
    case PC_TOOL_LINE: case PC_TOOL_CUSTOM_SHAPE: return 'U';
    default: return '\0'; /* Blur/Sharpen/Smudge, SingleRow/SingleColumn */
    }
}

const char *pc_ui_tool_glyph(pc_ui_tool t)
{
    switch (t) {
    case PC_TOOL_MOVE: return "\xe2\x9c\xa5";
    case PC_TOOL_RECT_MARQUEE: return "\xe2\xac\x9a";
    case PC_TOOL_ELLIPSE_MARQUEE: return "\xe2\x97\x8c";
    case PC_TOOL_BRUSH: case PC_TOOL_MIXER_BRUSH: return "\xf0\x9f\x96\x8c";
    case PC_TOOL_ERASER: return "\xe2\x8c\xab";
    case PC_TOOL_EYEDROPPER: return "\xf0\x9f\x92\xa7";
    case PC_TOOL_COLOR_SAMPLER: return "\xe2\x97\x8e";
    case PC_TOOL_LASSO: case PC_TOOL_POLYGON_LASSO: case PC_TOOL_MAGNETIC_LASSO: return "L";
    case PC_TOOL_MAGIC_WAND: return "W";
    case PC_TOOL_CROP: return "C";
    case PC_TOOL_GRADIENT: case PC_TOOL_PAINT_BUCKET: return "G";
    case PC_TOOL_TYPE: case PC_TOOL_VERTICAL_TYPE: case PC_TOOL_HORIZONTAL_TYPE_MASK:
    case PC_TOOL_VERTICAL_TYPE_MASK: return "T";
    case PC_TOOL_HAND: return "\xe2\x9c\x8b";
    case PC_TOOL_ROTATE_VIEW: return "\xf0\x9f\xa7\xad";
    case PC_TOOL_ZOOM: return "\xf0\x9f\x94\x8d";
    default: return "\xe2\x80\xa2";
    }
}

bool pc_ui_tool_is_type(pc_ui_tool t)
{
    return t == PC_TOOL_TYPE || t == PC_TOOL_VERTICAL_TYPE || t == PC_TOOL_HORIZONTAL_TYPE_MASK ||
           t == PC_TOOL_VERTICAL_TYPE_MASK;
}
bool pc_ui_tool_is_type_mask(pc_ui_tool t)
{
    return t == PC_TOOL_HORIZONTAL_TYPE_MASK || t == PC_TOOL_VERTICAL_TYPE_MASK;
}
bool pc_ui_tool_is_vertical_type(pc_ui_tool t)
{
    return t == PC_TOOL_VERTICAL_TYPE || t == PC_TOOL_VERTICAL_TYPE_MASK;
}
bool pc_ui_tool_is_brushlike(pc_ui_tool t)
{
    switch (t) {
    case PC_TOOL_BRUSH: case PC_TOOL_PENCIL: case PC_TOOL_MIXER_BRUSH: case PC_TOOL_COLOR_REPLACEMENT:
    case PC_TOOL_ERASER: case PC_TOOL_BACKGROUND_ERASER: case PC_TOOL_REMOVE: case PC_TOOL_SPOT_HEALING:
    case PC_TOOL_HEALING: case PC_TOOL_CLONE_STAMP: case PC_TOOL_PATTERN_STAMP: case PC_TOOL_HISTORY_BRUSH:
    case PC_TOOL_BLUR: case PC_TOOL_SHARPEN: case PC_TOOL_SMUDGE: case PC_TOOL_DODGE: case PC_TOOL_BURN:
    case PC_TOOL_SPONGE: return true;
    default: return false;
    }
}

/* Lowercase-copy, optionally stripping space/_/-. */
static void norm(const char *s, char *out, size_t cap, bool strip_sep)
{
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (strip_sep && (c == ' ' || c == '_' || c == '-')) { continue; }
        if (c >= 'A' && c <= 'Z') { c = (unsigned char)(c - 'A' + 'a'); }
        out[j++] = (char)c;
    }
    out[j] = '\0';
}

/* Remove every occurrence of `sub` from `s` in place. */
static void strip_sub(char *s, const char *sub)
{
    size_t sl = strlen(sub);
    if (sl == 0) { return; }
    char *w = s;
    for (char *r = s; *r;) {
        if (strncmp(r, sub, sl) == 0) {
            r += sl;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

bool pc_ui_tool_from_name(const char *s, pc_ui_tool *out)
{
    if (!s) { return false; }
    char n[64];
    norm(s, n, sizeof n, true); /* strip space/_/- */
    for (int i = 0; i < PC_TOOL_N; i++) {
        char a[64];
        norm(TOOL_SERDE[i], a, sizeof a, false); /* debug/variant name lowercased */
        if (strcmp(n, a) == 0) { *out = (pc_ui_tool)i; return true; }
        char am[64];
        memcpy(am, a, strlen(a) + 1);
        strip_sub(am, "marquee");
        if (strcmp(n, am) == 0) { *out = (pc_ui_tool)i; return true; }
        /* label lowercased with spaces removed only (keeps '-', as the reference does). */
        char lbl[64];
        size_t j = 0;
        for (const char *p = TOOL_LABEL[i]; *p && j + 1 < sizeof lbl; p++) {
            if (*p == ' ') { continue; }
            char c = *p;
            if (c >= 'A' && c <= 'Z') { c = (char)(c - 'A' + 'a'); }
            lbl[j++] = c;
        }
        lbl[j] = '\0';
        if (strcmp(n, lbl) == 0) { *out = (pc_ui_tool)i; return true; }
    }
    return false;
}

/* ---- enums ---- */
static const char *const DIALOG_KIND[] = {"NewDocument", "About", "Command", "Error", "LayerStyle"};
const char *pc_ui_dialog_kind_serde(pc_ui_dialog_kind k)
{
    return (unsigned)k <= PC_DIALOG_LAYER_STYLE ? DIALOG_KIND[k] : DIALOG_KIND[0];
}
bool pc_ui_dialog_kind_from_serde(const char *s, pc_ui_dialog_kind *out)
{
    if (!s) { return false; }
    for (int i = 0; i <= PC_DIALOG_LAYER_STYLE; i++) {
        if (strcmp(s, DIALOG_KIND[i]) == 0) { *out = (pc_ui_dialog_kind)i; return true; }
    }
    return false;
}

static const char *const TRANSFORM_MODE[] = {"free", "rotate", "skew", "distort", "perspective"};
const char *pc_ui_transform_mode_serde(pc_ui_transform_mode m)
{
    return (unsigned)m <= PC_TRANSFORM_PERSPECTIVE ? TRANSFORM_MODE[m] : TRANSFORM_MODE[0];
}
pc_ui_transform_mode pc_ui_transform_mode_for_command(const char *id)
{
    if (!id) { return PC_TRANSFORM_FREE; }
    if (strcmp(id, "edit.transform.rotate") == 0) { return PC_TRANSFORM_ROTATE; }
    if (strcmp(id, "edit.transform.skew") == 0) { return PC_TRANSFORM_SKEW; }
    if (strcmp(id, "edit.transform.distort") == 0) { return PC_TRANSFORM_DISTORT; }
    if (strcmp(id, "edit.transform.perspective") == 0) { return PC_TRANSFORM_PERSPECTIVE; }
    return PC_TRANSFORM_FREE;
}

/* ---- struct defaults ---- */
void pc_ui_panels_default(pc_ui_panels *p)
{
    *p = (pc_ui_panels){0};
    p->layers = true;
    p->properties = true;
    p->color = true;
    p->toolbar = true;
    p->options_bar = true;
    p->status_bar = true;
    p->dock = true;
    p->rail = true;
    p->menu_bar = true;
}
void pc_ui_view_default(pc_ui_view *v)
{
    *v = (pc_ui_view){0};
    v->zoom = 1.0f;
    v->fit_pending = true;
}
void pc_ui_extras_default(pc_ui_extras *e)
{
    *e = (pc_ui_extras){0};
    e->guides = true;
    e->snap = true;
}
void pc_ui_camera_raw_preview_default(pc_ui_camera_raw_preview *p)
{
    *p = (pc_ui_camera_raw_preview){0};
    p->has_zoom = false;
    p->center[0] = 0.5f;
    p->center[1] = 0.5f;
}

/* ---- JSON helpers ---- */
static yyjson_mut_val *vec2f(yyjson_mut_doc *d, const float v[2])
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, a, (double)v[0]);
    yyjson_mut_arr_add_real(d, a, (double)v[1]);
    return a;
}
static bool read_vec2f(yyjson_val *a, float out[2])
{
    if (!a || !yyjson_is_arr(a) || yyjson_arr_size(a) != 2) { return false; }
    double x, y;
    if (!pc_json_as_f64(yyjson_arr_get(a, 0), &x) || !pc_json_as_f64(yyjson_arr_get(a, 1), &y)) { return false; }
    out[0] = (float)x;
    out[1] = (float)y;
    return true;
}

static yyjson_mut_val *panels_json(yyjson_mut_doc *d, const pc_ui_panels *p)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, o, "layers", p->layers);
    yyjson_mut_obj_add_bool(d, o, "history", p->history);
    yyjson_mut_obj_add_bool(d, o, "properties", p->properties);
    yyjson_mut_obj_add_bool(d, o, "color", p->color);
    yyjson_mut_obj_add_bool(d, o, "navigator", p->navigator);
    yyjson_mut_obj_add_bool(d, o, "toolbar", p->toolbar);
    yyjson_mut_obj_add_bool(d, o, "options_bar", p->options_bar);
    yyjson_mut_obj_add_bool(d, o, "status_bar", p->status_bar);
    yyjson_mut_obj_add_bool(d, o, "brush_settings", p->brush_settings);
    yyjson_mut_obj_add_bool(d, o, "character", p->character);
    yyjson_mut_obj_add_bool(d, o, "toolbar_double", p->toolbar_double);
    yyjson_mut_obj_add_bool(d, o, "dock", p->dock);
    if (p->has_hidden_by_tab) {
        yyjson_mut_val *a = yyjson_mut_arr(d);
        for (int i = 0; i < 3; i++) { yyjson_mut_arr_add_bool(d, a, p->hidden_by_tab[i]); }
        yyjson_mut_obj_add_val(d, o, "hidden_by_tab", a);
    }
    yyjson_mut_obj_add_bool(d, o, "rail", p->rail);
    yyjson_mut_obj_add_bool(d, o, "menu_bar", p->menu_bar);
    yyjson_mut_obj_add_bool(d, o, "gradient_editor", p->gradient_editor);
    return o;
}
static void panels_read(yyjson_val *o, pc_ui_panels *p)
{
    pc_ui_panels_default(p);
    if (!o) { return; }
    bool b;
#define RB(field) if (pc_json_as_bool(pc_json_get(o, #field), &b)) { p->field = b; }
    RB(layers) RB(history) RB(properties) RB(color) RB(navigator) RB(toolbar) RB(options_bar)
    RB(status_bar) RB(brush_settings) RB(character) RB(toolbar_double) RB(dock) RB(rail) RB(menu_bar)
    RB(gradient_editor)
#undef RB
    yyjson_val *hbt = pc_json_get(o, "hidden_by_tab");
    if (hbt && yyjson_is_arr(hbt) && yyjson_arr_size(hbt) == 3) {
        p->has_hidden_by_tab = true;
        for (int i = 0; i < 3; i++) {
            bool v = false;
            (void)pc_json_as_bool(yyjson_arr_get(hbt, (size_t)i), &v);
            p->hidden_by_tab[i] = v;
        }
    }
}

static yyjson_mut_val *view_json(yyjson_mut_doc *d, const pc_ui_view *v)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_real(d, o, "zoom", (double)v->zoom);
    yyjson_mut_obj_add_val(d, o, "center", vec2f(d, v->center));
    yyjson_mut_obj_add_bool(d, o, "fit_pending", v->fit_pending);
    yyjson_mut_obj_add_bool(d, o, "fill_pending", v->fill_pending);
    yyjson_mut_val *ds = yyjson_mut_arr(d);
    yyjson_mut_arr_add_uint(d, ds, v->doc_size[0]);
    yyjson_mut_arr_add_uint(d, ds, v->doc_size[1]);
    yyjson_mut_obj_add_val(d, o, "doc_size", ds);
    yyjson_mut_obj_add_real(d, o, "rotation", (double)v->rotation);
    return o;
}
static void view_read(yyjson_val *o, pc_ui_view *v)
{
    pc_ui_view_default(v);
    if (!o) { return; }
    double z, r;
    if (pc_json_as_f64(pc_json_get(o, "zoom"), &z)) { v->zoom = (float)z; }
    read_vec2f(pc_json_get(o, "center"), v->center);
    bool b;
    if (pc_json_as_bool(pc_json_get(o, "fit_pending"), &b)) { v->fit_pending = b; }
    if (pc_json_as_bool(pc_json_get(o, "fill_pending"), &b)) { v->fill_pending = b; }
    yyjson_val *ds = pc_json_get(o, "doc_size");
    if (ds && yyjson_is_arr(ds) && yyjson_arr_size(ds) == 2) {
        uint64_t w, h;
        if (pc_json_as_u64(yyjson_arr_get(ds, 0), &w)) { v->doc_size[0] = (uint32_t)w; }
        if (pc_json_as_u64(yyjson_arr_get(ds, 1), &h)) { v->doc_size[1] = (uint32_t)h; }
    }
    if (pc_json_as_f64(pc_json_get(o, "rotation"), &r)) { v->rotation = (float)r; }
}

/* ---- lifecycle ---- */
void pc_ui_state_init(pc_ui_state *s)
{
    memset(s, 0, sizeof *s);
    s->tool = PC_TOOL_BRUSH;
    pc_ui_panels_default(&s->panels);
    pc_ui_extras_default(&s->extras);
    pc_ui_camera_raw_preview_default(&s->camera_raw_preview);
    s->theme = PC_UI_THEME_DEFAULT;
    s->workspace = strdup("Essentials");
    s->next_id = 1;
    s->status = strdup("");
}

static void free_str_arr(char **a, size_t n)
{
    for (size_t i = 0; i < n; i++) { free(a[i]); }
    free(a);
}

void pc_ui_state_fini(pc_ui_state *s)
{
    if (!s) { return; }
    free(s->group_tools);
    free_str_arr(s->recent_files, s->n_recent_files);
    free_str_arr(s->layer_filter, s->n_layer_filter);
    free(s->selected_path);
    free(s->camera_raw_scope.samplers);
    free(s->views);
    free(s->windows);
    for (size_t i = 0; i < s->n_dialogs; i++) {
        if (s->dialogs[i].fields) { yyjson_mut_doc_free(s->dialogs[i].fields); }
    }
    free(s->dialogs);
    free(s->workspace);
    free(s->status);
    free(s->gpu_fallback_notice);
    pc_ui_dock_layout_fini(&s->dock);
    memset(s, 0, sizeof *s);
}

/* ---- methods ---- */
uint64_t pc_ui_state_alloc_id(pc_ui_state *s)
{
    uint64_t id = s->next_id;
    s->next_id++;
    return id;
}

void pc_ui_state_remember_group_tool(pc_ui_state *s)
{
    pc_ui_tool tool = s->tool;
    for (size_t i = 0; i < s->n_group_tools; i++) {
        if (s->group_tools[i] == tool) { return; } /* already recorded */
    }
    /* drop any other tool in the same shortcut group, then append. */
    char key = pc_ui_tool_key(tool);
    size_t w = 0;
    for (size_t i = 0; i < s->n_group_tools; i++) {
        if (pc_ui_tool_key(s->group_tools[i]) != key) { s->group_tools[w++] = s->group_tools[i]; }
    }
    s->n_group_tools = w;
    pc_ui_tool *grown = realloc(s->group_tools, (s->n_group_tools + 1) * sizeof(pc_ui_tool));
    if (!grown) { return; }
    s->group_tools = grown;
    s->group_tools[s->n_group_tools++] = tool;
}

bool pc_ui_state_group_tool(const pc_ui_state *s, char key, pc_ui_tool *out)
{
    for (size_t i = 0; i < s->n_group_tools; i++) {
        if (pc_ui_tool_key(s->group_tools[i]) == key) { *out = s->group_tools[i]; return true; }
    }
    return false;
}

uint64_t pc_ui_state_open_dialog(pc_ui_state *s, pc_ui_dialog_kind kind, yyjson_val *fields)
{
    uint64_t id = pc_ui_state_alloc_id(s);
    pc_ui_dialog *grown = realloc(s->dialogs, (s->n_dialogs + 1) * sizeof(pc_ui_dialog));
    if (!grown) { return id; }
    s->dialogs = grown;
    pc_ui_dialog *d = &s->dialogs[s->n_dialogs];
    d->id = id;
    d->kind = kind;
    d->fields = yyjson_mut_doc_new(NULL);
    if (d->fields) {
        yyjson_mut_val *root =
            fields ? yyjson_val_mut_copy(d->fields, fields) : yyjson_mut_obj(d->fields);
        yyjson_mut_doc_set_root(d->fields, root ? root : yyjson_mut_obj(d->fields));
    }
    s->n_dialogs++;
    return id;
}

pc_ui_dialog *pc_ui_state_dialog_mut(pc_ui_state *s, uint64_t id)
{
    for (size_t i = 0; i < s->n_dialogs; i++) {
        if (s->dialogs[i].id == id) { return &s->dialogs[i]; }
    }
    return NULL;
}

bool pc_ui_state_close_dialog(pc_ui_state *s, uint64_t id)
{
    for (size_t i = 0; i < s->n_dialogs; i++) {
        if (s->dialogs[i].id == id) {
            if (s->dialogs[i].fields) { yyjson_mut_doc_free(s->dialogs[i].fields); }
            memmove(&s->dialogs[i], &s->dialogs[i + 1], (s->n_dialogs - i - 1) * sizeof(pc_ui_dialog));
            s->n_dialogs--;
            return true;
        }
    }
    return false;
}

yyjson_mut_val *pc_ui_state_new_document_fields(yyjson_mut_doc *doc)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_str(doc, o, "name", "Untitled-1");
    yyjson_mut_obj_add_uint(doc, o, "width", 1920);
    yyjson_mut_obj_add_uint(doc, o, "height", 1080);
    yyjson_mut_obj_add_str(doc, o, "mode", "rgb");
    yyjson_mut_obj_add_uint(doc, o, "depth", 8);
    yyjson_mut_obj_add_str(doc, o, "background", "white");
    return o;
}

/* ---- UiState serialisation (U1 subset) ---- */
static yyjson_mut_val *str_arr(yyjson_mut_doc *d, char *const *a, size_t n)
{
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < n; i++) { yyjson_mut_arr_add_strcpy(d, arr, a[i]); }
    return arr;
}

static yyjson_mut_val *cr_scope_json(yyjson_mut_doc *d, const pc_ui_camera_raw_scope *c)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, o, "shadows", c->shadows);
    yyjson_mut_obj_add_bool(d, o, "highlights", c->highlights);
    yyjson_mut_obj_add_bool(d, o, "lab", c->lab);
    yyjson_mut_obj_add_bool(d, o, "samplerTool", c->sampler_tool);
    yyjson_mut_val *s = yyjson_mut_arr(d);
    for (size_t i = 0; i < c->n_samplers; i++) { yyjson_mut_arr_append(s, vec2f(d, c->samplers[i])); }
    yyjson_mut_obj_add_val(d, o, "samplers", s);
    yyjson_mut_obj_add_bool(d, o, "vectorscope", c->vectorscope);
    yyjson_mut_obj_add_bool(d, o, "selectedRegion", c->selected_region);
    yyjson_mut_obj_add_bool(d, o, "redRight", c->red_right);
    yyjson_mut_obj_add_bool(d, o, "hideSkinLine", c->hide_skin_line);
    yyjson_mut_obj_add_bool(d, o, "floating", c->floating);
    if (c->has_floating_rect) {
        yyjson_mut_val *r = yyjson_mut_arr(d);
        for (int i = 0; i < 4; i++) { yyjson_mut_arr_add_real(d, r, (double)c->floating_rect[i]); }
        yyjson_mut_obj_add_val(d, o, "floatingRect", r);
    } else {
        yyjson_mut_obj_add_null(d, o, "floatingRect");
    }
    return o;
}

static yyjson_mut_val *cr_preview_json(yyjson_mut_doc *d, const pc_ui_camera_raw_preview *p)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    if (p->has_zoom) { yyjson_mut_obj_add_real(d, o, "zoom", (double)p->zoom); } else { yyjson_mut_obj_add_null(d, o, "zoom"); }
    yyjson_mut_obj_add_val(d, o, "center", vec2f(d, p->center));
    yyjson_mut_obj_add_bool(d, o, "hand", p->hand);
    return o;
}

pc_status pc_ui_state_to_json(const pc_ui_state *s, yyjson_mut_doc *doc, yyjson_mut_val **out, pc_error *err)
{
    yyjson_mut_val *o = yyjson_mut_obj(doc);
    if (!o) { return pc_fail(err, PC_ERR_NOMEM, "out of memory"); }
    yyjson_mut_obj_add_str(doc, o, "tool", pc_ui_tool_serde(s->tool));
    yyjson_mut_val *gt = yyjson_mut_arr(doc);
    for (size_t i = 0; i < s->n_group_tools; i++) { yyjson_mut_arr_add_str(doc, gt, pc_ui_tool_serde(s->group_tools[i])); }
    yyjson_mut_obj_add_val(doc, o, "group_tools", gt);
    yyjson_mut_obj_add_val(doc, o, "recent_files", str_arr(doc, s->recent_files, s->n_recent_files));
    yyjson_mut_obj_add_bool(doc, o, "mask_target", s->mask_target);
    yyjson_mut_obj_add_bool(doc, o, "vector_mask_target", s->vector_mask_target);
    yyjson_mut_obj_add_bool(doc, o, "transform_unlinked", s->transform_unlinked);
    if (s->has_properties_layer) { yyjson_mut_obj_add_uint(doc, o, "properties_layer", s->properties_layer); } else { yyjson_mut_obj_add_null(doc, o, "properties_layer"); }
    if (s->has_layer_comp_selected) { yyjson_mut_obj_add_uint(doc, o, "layer_comp_selected", s->layer_comp_selected); } else { yyjson_mut_obj_add_null(doc, o, "layer_comp_selected"); }
    if (s->selected_path) { yyjson_mut_obj_add_strcpy(doc, o, "selected_path", s->selected_path); } else { yyjson_mut_obj_add_null(doc, o, "selected_path"); }
    yyjson_mut_obj_add_val(doc, o, "layer_filter", str_arr(doc, s->layer_filter, s->n_layer_filter));
    yyjson_mut_obj_add_val(doc, o, "panels", panels_json(doc, &s->panels));
    /* extras */
    yyjson_mut_val *ex = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, ex, "rulers", s->extras.rulers);
    yyjson_mut_obj_add_bool(doc, ex, "grid", s->extras.grid);
    yyjson_mut_obj_add_bool(doc, ex, "guides", s->extras.guides);
    yyjson_mut_obj_add_bool(doc, ex, "snap", s->extras.snap);
    yyjson_mut_obj_add_bool(doc, ex, "lock_guides", s->extras.lock_guides);
    yyjson_mut_obj_add_val(doc, o, "extras", ex);
    /* dock_tabs */
    yyjson_mut_val *dt = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_uint(doc, dt, "properties", s->dock_tabs.properties);
    yyjson_mut_obj_add_uint(doc, dt, "color", s->dock_tabs.color);
    yyjson_mut_obj_add_uint(doc, dt, "layers", s->dock_tabs.layers);
    yyjson_mut_obj_add_uint(doc, dt, "navigator", s->dock_tabs.navigator);
    yyjson_mut_obj_add_uint(doc, dt, "history", s->dock_tabs.history);
    yyjson_mut_obj_add_uint(doc, dt, "character", s->dock_tabs.character);
    yyjson_mut_obj_add_val(doc, o, "dock_tabs", dt);
    /* color_panel */
    yyjson_mut_val *cp = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, cp, "background", s->color_panel.background);
    yyjson_mut_obj_add_val(doc, o, "color_panel", cp);
    /* views / windows / dialogs */
    yyjson_mut_val *vs = yyjson_mut_arr(doc);
    for (size_t i = 0; i < s->n_views; i++) { yyjson_mut_arr_append(vs, view_json(doc, &s->views[i])); }
    yyjson_mut_obj_add_val(doc, o, "views", vs);
    yyjson_mut_val *ws = yyjson_mut_arr(doc);
    for (size_t i = 0; i < s->n_windows; i++) {
        yyjson_mut_val *w = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, w, "id", s->windows[i].id);
        yyjson_mut_obj_add_uint(doc, w, "document", s->windows[i].document);
        yyjson_mut_obj_add_val(doc, w, "view", view_json(doc, &s->windows[i].view));
        yyjson_mut_obj_add_bool(doc, w, "open", s->windows[i].open);
        if (s->windows[i].has_position) { yyjson_mut_obj_add_val(doc, w, "position", vec2f(doc, s->windows[i].position)); } else { yyjson_mut_obj_add_null(doc, w, "position"); }
        yyjson_mut_arr_append(ws, w);
    }
    yyjson_mut_obj_add_val(doc, o, "windows", ws);
    yyjson_mut_val *ds = yyjson_mut_arr(doc);
    for (size_t i = 0; i < s->n_dialogs; i++) {
        yyjson_mut_val *dlg = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_uint(doc, dlg, "id", s->dialogs[i].id);
        yyjson_mut_obj_add_str(doc, dlg, "kind", pc_ui_dialog_kind_serde(s->dialogs[i].kind));
        yyjson_mut_val *f = NULL;
        if (s->dialogs[i].fields) {
            yyjson_mut_val *root = yyjson_mut_doc_get_root(s->dialogs[i].fields);
            f = root ? yyjson_mut_val_mut_copy(doc, root) : NULL;
        }
        yyjson_mut_obj_add_val(doc, dlg, "fields", f ? f : yyjson_mut_obj(doc));
        yyjson_mut_arr_append(ds, dlg);
    }
    yyjson_mut_obj_add_val(doc, o, "dialogs", ds);
    yyjson_mut_obj_add_str(doc, o, "theme", pc_ui_theme_kind_name(s->theme));
    yyjson_mut_obj_add_strcpy(doc, o, "workspace", s->workspace ? s->workspace : "");
    yyjson_mut_obj_add_bool(doc, o, "palette_open", s->palette_open);
    yyjson_mut_obj_add_uint(doc, o, "brush_section", s->brush_section);
    yyjson_mut_obj_add_uint(doc, o, "brush_tab", s->brush_tab);
    yyjson_mut_obj_add_uint(doc, o, "selection_mode", s->selection_mode);
    yyjson_mut_obj_add_strcpy(doc, o, "status", s->status ? s->status : "");
    yyjson_mut_obj_add_bool(doc, o, "status_error", s->status_error);
    if (s->gpu_fallback_notice) { yyjson_mut_obj_add_strcpy(doc, o, "gpu_fallback_notice", s->gpu_fallback_notice); } else { yyjson_mut_obj_add_null(doc, o, "gpu_fallback_notice"); }
    yyjson_mut_obj_add_real(doc, o, "crop_angle", s->crop_angle);
    yyjson_mut_obj_add_uint(doc, o, "next_id", s->next_id);
    yyjson_mut_obj_add_val(doc, o, "camera_raw_scope", cr_scope_json(doc, &s->camera_raw_scope));
    yyjson_mut_obj_add_val(doc, o, "camera_raw_preview", cr_preview_json(doc, &s->camera_raw_preview));
    yyjson_mut_val *dock = NULL;
    if (pc_ui_dock_layout_to_json(&s->dock, doc, &dock, err) != PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_obj_add_val(doc, o, "dock", dock);
    *out = o;
    return PC_OK;
}

static char **read_str_arr(yyjson_val *a, size_t *n_out)
{
    *n_out = 0;
    if (!a || !yyjson_is_arr(a)) { return NULL; }
    size_t n = yyjson_arr_size(a);
    if (n == 0) { return NULL; }
    char **out = calloc(n, sizeof(char *));
    if (!out) { return NULL; }
    size_t w = 0;
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach(a, idx, max, v) {
        const char *str = pc_json_as_str(v, NULL);
        if (str) { out[w] = strdup(str); w++; }
    }
    *n_out = w;
    return out;
}

pc_status pc_ui_state_from_json(pc_ui_state *s, yyjson_val *o, pc_error *err)
{
    (void)err;
    pc_ui_state_fini(s);
    pc_ui_state_init(s);
    if (!o || !yyjson_is_obj(o)) { return PC_OK; }
    pc_ui_tool t;
    if (pc_ui_tool_from_serde(pc_json_as_str(pc_json_get(o, "tool"), NULL), &t)) { s->tool = t; }
    /* group_tools */
    yyjson_val *gt = pc_json_get(o, "group_tools");
    if (gt && yyjson_is_arr(gt)) {
        size_t n = yyjson_arr_size(gt);
        s->group_tools = n ? calloc(n, sizeof(pc_ui_tool)) : NULL;
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(gt, idx, max, v) {
            pc_ui_tool gtool;
            if (s->group_tools && pc_ui_tool_from_serde(pc_json_as_str(v, NULL), &gtool)) {
                s->group_tools[s->n_group_tools++] = gtool;
            }
        }
    }
    free_str_arr(s->recent_files, s->n_recent_files);
    s->recent_files = read_str_arr(pc_json_get(o, "recent_files"), &s->n_recent_files);
    free_str_arr(s->layer_filter, s->n_layer_filter);
    s->layer_filter = read_str_arr(pc_json_get(o, "layer_filter"), &s->n_layer_filter);
    bool b;
    if (pc_json_as_bool(pc_json_get(o, "mask_target"), &b)) { s->mask_target = b; }
    if (pc_json_as_bool(pc_json_get(o, "vector_mask_target"), &b)) { s->vector_mask_target = b; }
    if (pc_json_as_bool(pc_json_get(o, "transform_unlinked"), &b)) { s->transform_unlinked = b; }
    uint64_t u;
    if (pc_json_as_u64(pc_json_get(o, "properties_layer"), &u)) { s->has_properties_layer = true; s->properties_layer = u; }
    if (pc_json_as_u64(pc_json_get(o, "layer_comp_selected"), &u)) { s->has_layer_comp_selected = true; s->layer_comp_selected = (uint32_t)u; }
    const char *sp = pc_json_as_str(pc_json_get(o, "selected_path"), NULL);
    free(s->selected_path);
    s->selected_path = sp ? strdup(sp) : NULL;
    panels_read(pc_json_get(o, "panels"), &s->panels);
    yyjson_val *ex = pc_json_get(o, "extras");
    if (ex) {
        pc_ui_extras_default(&s->extras);
        if (pc_json_as_bool(pc_json_get(ex, "rulers"), &b)) { s->extras.rulers = b; }
        if (pc_json_as_bool(pc_json_get(ex, "grid"), &b)) { s->extras.grid = b; }
        if (pc_json_as_bool(pc_json_get(ex, "guides"), &b)) { s->extras.guides = b; }
        if (pc_json_as_bool(pc_json_get(ex, "snap"), &b)) { s->extras.snap = b; }
        if (pc_json_as_bool(pc_json_get(ex, "lock_guides"), &b)) { s->extras.lock_guides = b; }
    }
    yyjson_val *dt = pc_json_get(o, "dock_tabs");
    if (dt) {
#define RU(field) if (pc_json_as_u64(pc_json_get(dt, #field), &u)) { s->dock_tabs.field = (size_t)u; }
        RU(properties) RU(color) RU(layers) RU(navigator) RU(history) RU(character)
#undef RU
    }
    yyjson_val *cp = pc_json_get(o, "color_panel");
    if (cp && pc_json_as_bool(pc_json_get(cp, "background"), &b)) { s->color_panel.background = b; }
    pc_ui_dock_layout_from_json(&s->dock, pc_json_get(o, "dock"));
    /* views */
    yyjson_val *vs = pc_json_get(o, "views");
    if (vs && yyjson_is_arr(vs)) {
        size_t n = yyjson_arr_size(vs);
        s->views = n ? calloc(n, sizeof(pc_ui_view)) : NULL;
        size_t idx, max;
        yyjson_val *v;
        yyjson_arr_foreach(vs, idx, max, v) {
            if (s->views) { view_read(v, &s->views[s->n_views++]); }
        }
    }
    /* windows */
    yyjson_val *ws = pc_json_get(o, "windows");
    if (ws && yyjson_is_arr(ws)) {
        size_t n = yyjson_arr_size(ws);
        s->windows = n ? calloc(n, sizeof(pc_ui_doc_window)) : NULL;
        size_t idx, max;
        yyjson_val *w;
        yyjson_arr_foreach(ws, idx, max, w) {
            if (!s->windows) { break; }
            pc_ui_doc_window *dw = &s->windows[s->n_windows++];
            if (pc_json_as_u64(pc_json_get(w, "id"), &u)) { dw->id = u; }
            if (pc_json_as_u64(pc_json_get(w, "document"), &u)) { dw->document = (size_t)u; }
            view_read(pc_json_get(w, "view"), &dw->view);
            if (pc_json_as_bool(pc_json_get(w, "open"), &b)) { dw->open = b; }
            if (read_vec2f(pc_json_get(w, "position"), dw->position)) { dw->has_position = true; }
        }
    }
    /* dialogs */
    yyjson_val *dls = pc_json_get(o, "dialogs");
    if (dls && yyjson_is_arr(dls)) {
        size_t n = yyjson_arr_size(dls);
        s->dialogs = n ? calloc(n, sizeof(pc_ui_dialog)) : NULL;
        size_t idx, max;
        yyjson_val *dl;
        yyjson_arr_foreach(dls, idx, max, dl) {
            if (!s->dialogs) { break; }
            pc_ui_dialog *dd = &s->dialogs[s->n_dialogs++];
            if (pc_json_as_u64(pc_json_get(dl, "id"), &u)) { dd->id = u; }
            pc_ui_dialog_kind k;
            if (pc_ui_dialog_kind_from_serde(pc_json_as_str(pc_json_get(dl, "kind"), NULL), &k)) { dd->kind = k; }
            dd->fields = yyjson_mut_doc_new(NULL);
            if (dd->fields) {
                yyjson_val *f = pc_json_get(dl, "fields");
                yyjson_mut_val *root = f ? yyjson_val_mut_copy(dd->fields, f) : yyjson_mut_obj(dd->fields);
                yyjson_mut_doc_set_root(dd->fields, root ? root : yyjson_mut_obj(dd->fields));
            }
        }
    }
    pc_ui_theme_kind tk;
    if (pc_ui_theme_kind_from_serde(pc_json_as_str(pc_json_get(o, "theme"), NULL), &tk)) { s->theme = tk; }
    const char *wsname = pc_json_as_str(pc_json_get(o, "workspace"), NULL);
    if (wsname) { free(s->workspace); s->workspace = strdup(wsname); }
    if (pc_json_as_bool(pc_json_get(o, "palette_open"), &b)) { s->palette_open = b; }
    if (pc_json_as_u64(pc_json_get(o, "brush_section"), &u)) { s->brush_section = (size_t)u; }
    if (pc_json_as_u64(pc_json_get(o, "brush_tab"), &u)) { s->brush_tab = (size_t)u; }
    if (pc_json_as_u64(pc_json_get(o, "selection_mode"), &u)) { s->selection_mode = (uint8_t)u; }
    const char *st = pc_json_as_str(pc_json_get(o, "status"), NULL);
    if (st) { free(s->status); s->status = strdup(st); }
    if (pc_json_as_bool(pc_json_get(o, "status_error"), &b)) { s->status_error = b; }
    const char *gf = pc_json_as_str(pc_json_get(o, "gpu_fallback_notice"), NULL);
    free(s->gpu_fallback_notice);
    s->gpu_fallback_notice = gf ? strdup(gf) : NULL;
    double ca;
    if (pc_json_as_f64(pc_json_get(o, "crop_angle"), &ca)) { s->crop_angle = ca; }
    if (pc_json_as_u64(pc_json_get(o, "next_id"), &u)) { s->next_id = u; }
    /* camera raw scope / preview */
    yyjson_val *crs = pc_json_get(o, "camera_raw_scope");
    if (crs) {
        pc_ui_camera_raw_scope *c = &s->camera_raw_scope;
        if (pc_json_as_bool(pc_json_get(crs, "shadows"), &b)) { c->shadows = b; }
        if (pc_json_as_bool(pc_json_get(crs, "highlights"), &b)) { c->highlights = b; }
        if (pc_json_as_bool(pc_json_get(crs, "lab"), &b)) { c->lab = b; }
        if (pc_json_as_bool(pc_json_get(crs, "samplerTool"), &b)) { c->sampler_tool = b; }
        if (pc_json_as_bool(pc_json_get(crs, "vectorscope"), &b)) { c->vectorscope = b; }
        if (pc_json_as_bool(pc_json_get(crs, "selectedRegion"), &b)) { c->selected_region = b; }
        if (pc_json_as_bool(pc_json_get(crs, "redRight"), &b)) { c->red_right = b; }
        if (pc_json_as_bool(pc_json_get(crs, "hideSkinLine"), &b)) { c->hide_skin_line = b; }
        if (pc_json_as_bool(pc_json_get(crs, "floating"), &b)) { c->floating = b; }
        yyjson_val *samp = pc_json_get(crs, "samplers");
        if (samp && yyjson_is_arr(samp) && yyjson_arr_size(samp) > 0) {
            size_t n = yyjson_arr_size(samp);
            c->samplers = calloc(n, sizeof(*c->samplers));
            if (c->samplers) {
                size_t idx, max;
                yyjson_val *pt;
                yyjson_arr_foreach(samp, idx, max, pt) {
                    if (read_vec2f(pt, c->samplers[c->n_samplers])) { c->n_samplers++; }
                }
            }
        }
        if (read_vec2f(pc_json_get(crs, "floatingRect"), c->floating_rect)) { c->has_floating_rect = true; }
        else {
            /* floatingRect is a [f32;4]; read 4 if present. */
            yyjson_val *fr = pc_json_get(crs, "floatingRect");
            if (fr && yyjson_is_arr(fr) && yyjson_arr_size(fr) == 4) {
                for (int i = 0; i < 4; i++) {
                    double dv;
                    if (pc_json_as_f64(yyjson_arr_get(fr, (size_t)i), &dv)) { c->floating_rect[i] = (float)dv; }
                }
                c->has_floating_rect = true;
            }
        }
    }
    yyjson_val *crp = pc_json_get(o, "camera_raw_preview");
    if (crp) {
        double z;
        if (pc_json_as_f64(pc_json_get(crp, "zoom"), &z)) { s->camera_raw_preview.has_zoom = true; s->camera_raw_preview.zoom = (float)z; }
        else { s->camera_raw_preview.has_zoom = false; }
        read_vec2f(pc_json_get(crp, "center"), s->camera_raw_preview.center);
        if (pc_json_as_bool(pc_json_get(crp, "hand"), &b)) { s->camera_raw_preview.hand = b; }
    }
    return PC_OK;
}
