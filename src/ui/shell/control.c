#include "layer_picker_ui.h"
#include "sampling_ui.h"
#include "ui/move_mods.h"
#include "artboard_ui.h"
#include "lasso_ui.h"
#include "slice_ui.h"
#include "transform_ui.h"
#include "context_ui.h"
#include "crop_ui.h"
#include "jobs_ui.h"
/* ui.* control-channel handler: see control.h. */
#include "control.h"
#include "ui/control_options.h"
#include "dialogs.h"
#include "shell_internal.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/base64.h"
#include "base/utf8.h"
#include "ui/shortcut_dispatch.h"
#include "ui/enable_rules.h"
#include "ui/pen_tool.h"
#include "ui/direct_tool.h"
#include "base/buf.h"
#include "base/file.h"
#include "codecs/codecs.h"
#include "codecs/image.h"
#include "ui/menus.h"
#include "ui/menu_state.h"
#include "ui/canvas.h"
#include "ui/shortcuts.h"
#include "engine/prefs.h"
#include "ui/i18n.h"

/* The ui.set fields, byte-for-byte from control.rs UI_SET_FIELDS, so an unknown field gives the same
 * error. Fields this scoped build cannot yet apply return PC_ERR_UNSUPPORTED (never silently drop). */
static const char *const UI_SET_FIELDS[] = {"backgroundJobs",
                                            "toolOptions",
                                            "tool",
                                            "panels",
                                            "dock",
                                            "dockTabs",
                                            "dockWidth",
                                            "colorPanel",
                                            "maskTarget",
                                            "vectorMaskTarget",
                                            "selectionMode",
                                            "zoom",
                                            "center",
                                            "rotation",
                                            "fit",
                                            "theme",
                                            "brushSection",
                                            "timeline",
                                            "slices",
                                            "brushTab",
                                            "brushesView",
                                            "brushPicker",
                                            "brushPickerName",
                                            "brushPickerStroke",
                                            "brushPickerTip",
                                            "brushPickerScale",
                                            "brushSize",
                                            "gradientBlendMode",
                                            "gradientClassic",
                                            "eyedropperSampleSize",
                                            "eyedropperSample",
                                            "eyedropperRing",
                                            "cropOverlay",
                                            "cropOverlayShow",
                                            "cropOverlayOrientation",
                                            "cropShield",
                                            "shapeStroke",
                                            "camTransformOnDrop",
                                            "tileGrid"};
static const size_t UI_SET_FIELDS_N = sizeof UI_SET_FIELDS / sizeof UI_SET_FIELDS[0];

static bool is_ui_set_field(const char *k)
{
    for (size_t i = 0; i < UI_SET_FIELDS_N; i++) {
        if (strcmp(k, UI_SET_FIELDS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static pc_status wrap(yyjson_mut_doc *doc, yyjson_mut_val *root, pc_json_value *out, pc_error *err)
{
    if (doc == NULL || root == NULL) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_doc_set_root(doc, root);
    pc_status rc = pc_json_value_from_mut(root, out, err);
    yyjson_mut_doc_free(doc);
    return rc;
}

/* An empty-object success result. */
static pc_status ok_empty(pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    return wrap(doc, doc ? yyjson_mut_obj(doc) : NULL, out, err);
}

/* ---- inspect ---- */

static void add_color(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, pc_ui_color c)
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_uint(d, a, c.r);
    yyjson_mut_arr_add_uint(d, a, c.g);
    yyjson_mut_arr_add_uint(d, a, c.b);
    yyjson_mut_arr_add_uint(d, a, c.a);
    yyjson_mut_obj_add_val(d, o, key, a);
}

static yyjson_mut_val *panels_json(yyjson_mut_doc *d, const pc_ui_panels *p)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, o, "layers", p->layers);
    yyjson_mut_obj_add_bool(d, o, "history", p->history);
    yyjson_mut_obj_add_bool(d, o, "properties", p->properties);
    yyjson_mut_obj_add_bool(d, o, "color", p->color);
    yyjson_mut_obj_add_bool(d, o, "navigator", p->navigator);
    yyjson_mut_obj_add_bool(d, o, "character", p->character);
    yyjson_mut_obj_add_bool(d, o, "toolbar", p->toolbar);
    yyjson_mut_obj_add_bool(d, o, "optionsBar", p->options_bar);
    yyjson_mut_obj_add_bool(d, o, "statusBar", p->status_bar);
    return o;
}

static yyjson_mut_val *extras_json(yyjson_mut_doc *d, const pc_ui_extras *e)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, o, "rulers", e->rulers);
    yyjson_mut_obj_add_bool(d, o, "grid", e->grid);
    yyjson_mut_obj_add_bool(d, o, "guides", e->guides);
    yyjson_mut_obj_add_bool(d, o, "snap", e->snap);
    yyjson_mut_obj_add_bool(d, o, "lockGuides", e->lock_guides);
    return o;
}

static yyjson_mut_val *view_json(yyjson_mut_doc *d, const pc_ui_view *v)
{
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_real(d, o, "zoom", v->zoom);
    yyjson_mut_val *c = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, c, v->center[0]);
    yyjson_mut_arr_add_real(d, c, v->center[1]);
    yyjson_mut_obj_add_val(d, o, "center", c);
    yyjson_mut_obj_add_real(d, o, "rotation", v->rotation);
    return o;
}

/* Copy a pc_json_value's root into doc under key (NULL value becomes JSON null). */
static void add_json_value(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, const pc_json_value *v)
{
    yyjson_mut_val *root = v ? pc_json_value_root(v) : NULL;
    yyjson_mut_val *copy = root ? pc_json_normalized_copy(d, root) : yyjson_mut_null(d);
    yyjson_mut_obj_add_val(d, o, key, copy ? copy : yyjson_mut_null(d));
}

pc_status pc_ui_control_inspect(pc_ui_shell_app *a, pc_json_value *out, pc_error *err)
{
    pc_status sync = pc_ui_shell_app_sync_views(a, err);
    if (sync != PC_OK) {
        return sync;
    }
    pc_engine_session *s = pc_ui_shell_app_session(a);
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);

    float ww, wh, ppp;
    pc_ui_shell_app_window_size(a, &ww, &wh, &ppp);
    yyjson_mut_val *win = yyjson_mut_obj(d);
    yyjson_mut_obj_add_real(d, win, "width", ww);
    yyjson_mut_obj_add_real(d, win, "height", wh);
    yyjson_mut_obj_add_real(d, win, "pixelsPerPoint", ppp);
    yyjson_mut_obj_add_val(d, root, "window", win);

    yyjson_mut_obj_add_bool(d, root, "quitRequested", pc_ui_shell_exit_requested(a));
    yyjson_mut_obj_add_strcpy(d, root, "tool", pc_ui_tool_serde(ui->tool));
    yyjson_mut_val *edit = yyjson_mut_null(d);
    if (ui->has_text_edit && pc_ui_text_edit_to_json(&ui->text_edit, d, &edit, err) != PC_OK) {
        yyjson_mut_doc_free(d);
        return err ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_obj_add_val(d, root, "textEdit", edit);
    yyjson_mut_val *type_panels = NULL;
    if (pc_ui_type_panels_to_json(&ui->type_panels, d, &type_panels, err) != PC_OK) {
        yyjson_mut_doc_free(d);
        return err ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_obj_add_val(d, root, "typePanels", type_panels);
    yyjson_mut_val *pen = yyjson_mut_null(d);
    if (ui->has_pen && pc_ui_pen_to_json(&ui->pen, d, &pen, err) != PC_OK) {
        yyjson_mut_doc_free(d);
        return err ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_obj_add_val(d, root, "pen", pen);
    yyjson_mut_val *direct = NULL;
    if (pc_ui_direct_to_json(&ui->direct, d, &direct, err) != PC_OK) {
        yyjson_mut_doc_free(d);
        return err ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_obj_add_val(d, root, "direct", direct);
    yyjson_mut_val *crop_rect = ui->has_crop ? yyjson_mut_arr(d) : yyjson_mut_null(d);
    if (ui->has_crop)
        for (size_t i = 0; i < 4; i++)
            yyjson_mut_arr_add_real(d, crop_rect, ui->crop_rect[i]);
    yyjson_mut_obj_add_val(d, root, "cropRect", crop_rect);
    yyjson_mut_obj_add_real(d, root, "cropAngle", ui->crop_angle);
    yyjson_mut_obj_add_bool(d, root, "cropEditing", ui->crop_editing);
    yyjson_mut_val *context = NULL;
    pc_status context_status = pc_ui_shell_context_json(a, d, &context, err);
    if (context_status != PC_OK || !yyjson_mut_obj_add_val(d, root, "canvasToolMenu", context)) {
        yyjson_mut_doc_free(d);
        return context_status != PC_OK ? context_status : pc_fail(err, PC_ERR_NOMEM, "context state");
    }
    yyjson_mut_val *transform = NULL;
    pc_status transform_status = pc_ui_transform_to_json(&ui->transform, d, &transform, err);
    if (transform_status != PC_OK || !yyjson_mut_obj_add_val(d, root, "transform", transform)) {
        yyjson_mut_doc_free(d);
        return transform_status != PC_OK ? transform_status : pc_fail(err, PC_ERR_NOMEM, "transform state");
    }
    yyjson_mut_obj_add_val(d, root, "selectedPath",
                           ui->selected_path ? yyjson_mut_strcpy(d, ui->selected_path) : yyjson_mut_null(d));
    yyjson_mut_val *dialogs = yyjson_mut_arr(d);
    for (size_t i = 0; i < ui->n_dialogs; i++) {
        yyjson_mut_val *row = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, row, "id", ui->dialogs[i].id);
        yyjson_mut_obj_add_str(d, row, "kind", pc_ui_dialog_kind_serde(ui->dialogs[i].kind));
        yyjson_mut_val *fields = yyjson_mut_doc_get_root(ui->dialogs[i].fields);
        yyjson_mut_obj_add_val(d, row, "fields",
                               fields ? pc_json_normalized_copy(d, fields) : yyjson_mut_obj(d));
        yyjson_mut_arr_add_val(dialogs, row);
    }
    yyjson_mut_obj_add_val(d, root, "dialogs", dialogs);
    yyjson_mut_val *tool_options = NULL;
    pc_status options_status = pc_ui_tool_options_to_json(&ui->tool_options, d, &tool_options, err);
    if (options_status != PC_OK) {
        yyjson_mut_doc_free(d);
        return options_status;
    }
    yyjson_mut_obj_add_val(d, root, "toolOptions", tool_options);
    yyjson_mut_obj_add_val(d, root, "timeline", pc_ui_timeline_json(&ui->timeline, d));
    yyjson_mut_obj_add_val(d, root, "panels", panels_json(d, &ui->panels));
    yyjson_mut_val *dock = NULL;
    pc_status dock_status = pc_ui_dock_layout_to_json(&ui->dock, d, &dock, err);
    if (dock_status != PC_OK) {
        yyjson_mut_doc_free(d);
        return dock_status;
    }
    yyjson_mut_obj_add_val(d, root, "dock", dock);
    yyjson_mut_obj_add_real(d, root, "dockWidth", ui->dock_width);
    yyjson_mut_obj_add_val(d, root, "extras", extras_json(d, &ui->extras));
    yyjson_mut_val *view_options = NULL;
    pc_status view_rc = pc_ui_view_options_to_json(&ui->view, d, &view_options, err);
    if (view_rc != PC_OK) {
        yyjson_mut_doc_free(d);
        return view_rc;
    }
    yyjson_mut_obj_add_val(d, root, "view", view_options);
    size_t active_view = 0;
    bool has_view = pc_engine_session_active_index(s, &active_view) && active_view < ui->n_views;
    yyjson_mut_obj_add_val(d, root, "activeView",
                           has_view ? view_json(d, &ui->views[active_view]) : yyjson_mut_null(d));

    yyjson_mut_val *views = yyjson_mut_arr(d);
    for (size_t i = 0; i < ui->n_views; i++) {
        yyjson_mut_arr_add_val(views, view_json(d, &ui->views[i]));
    }
    yyjson_mut_obj_add_val(d, root, "views", views);

    yyjson_mut_val *dt = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, dt, "properties", ui->dock_tabs.properties);
    yyjson_mut_obj_add_uint(d, dt, "color", ui->dock_tabs.color);
    yyjson_mut_obj_add_uint(d, dt, "layers", ui->dock_tabs.layers);
    yyjson_mut_obj_add_uint(d, dt, "navigator", ui->dock_tabs.navigator);
    yyjson_mut_obj_add_uint(d, dt, "history", ui->dock_tabs.history);
    yyjson_mut_obj_add_uint(d, dt, "character", ui->dock_tabs.character);
    yyjson_mut_obj_add_val(d, root, "dockTabs", dt);

    yyjson_mut_obj_add_bool(d, root, "maskTarget", ui->mask_target);
    yyjson_mut_obj_add_bool(d, root, "vectorMaskTarget", ui->vector_mask_target);
    yyjson_mut_obj_add_uint(d, root, "selectionMode", ui->selection_mode);
    yyjson_mut_obj_add_strcpy(d, root, "theme", pc_ui_theme_kind_name(ui->theme));
    if (ui->status != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, "status", ui->status);
    } else {
        yyjson_mut_obj_add_null(d, root, "status");
    }
    yyjson_mut_obj_add_bool(d, root, "statusError", ui->status_error);
    pc_json_value notices = PC_JSON_VALUE_INIT;
    if (pc_ui_shell_notices_json(a, &notices, NULL) == PC_OK)
        add_json_value(d, root, "notices", &notices);
    pc_json_value_fini(&notices);
    if (ui->gpu_fallback_notice != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, "gpuFallbackNotice", ui->gpu_fallback_notice);
    } else {
        yyjson_mut_obj_add_null(d, root, "gpuFallbackNotice");
    }
    pc_json_value gpu = PC_JSON_VALUE_INIT;
    if (pc_ui_shell_app_gpu_info(a, &gpu, NULL) == PC_OK) {
        add_json_value(d, root, "gpuInfo", &gpu);
    }
    pc_json_value_fini(&gpu);
    pc_ui_jobs *jobs = pc_ui_shell_jobs_runtime(a);
    yyjson_mut_val *job_info = yyjson_mut_obj(d),
                   *job_rows = yyjson_mut_obj_get(pc_json_value_root(&jobs->info), "jobs");
    if (job_info) {
        yyjson_mut_obj_add_bool(d, job_info, "background", jobs->enabled);
        yyjson_mut_obj_add_bool(d, job_info, "locked", jobs->locked);
        yyjson_mut_obj_add_bool(d, job_info, "modal", jobs->modal);
        yyjson_mut_obj_add_val(d, job_info, "items",
                               job_rows ? yyjson_mut_val_mut_copy(d, job_rows) : yyjson_mut_arr(d));
        yyjson_mut_obj_add_val(d, root, "jobs", job_info);
    }
    yyjson_mut_obj_add_uint(d, root, "frame", pc_ui_shell_app_frame(a));

    /* The theme tokens, so a snapshot's palette can be checked without a display. */
    yyjson_mut_val *th = yyjson_mut_obj(d);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(a);
    add_color(d, th, "canvas", t->canvas);
    add_color(d, th, "chrome", t->chrome);
    add_color(d, th, "accent", t->accent);
    yyjson_mut_obj_add_val(d, root, "themeTokens", th);

    yyjson_mut_val *slices = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, slices, "hideAuto", ui->slices.hide_auto);
    yyjson_mut_obj_add_bool(d, slices, "dragging", ui->slices.dragging);
    yyjson_mut_obj_add_val(d, slices, "selected",
                           ui->slices.has_selected ? yyjson_mut_uint(d, ui->slices.selected)
                                                   : yyjson_mut_null(d));
    yyjson_mut_obj_add_val(d, root, "slices", slices);
    pc_ui_sampling_tool *sampling = pc_ui_shell_sampling_runtime(a);
    pc_ui_sampling_sync(sampling, pc_ui_shell_app_session(a), ui);
    yyjson_mut_val *sample = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, sample, "active", sampling->active);
    yyjson_mut_obj_add_bool(d, sample, "hasSample", sampling->has_sample);
    yyjson_mut_obj_add_bool(d, sample, "background", sampling->background);
    yyjson_mut_obj_add_val(d, root, "sampling", sample);
    pc_ui_layer_picker_prune(pc_ui_shell_app_session(a), ui);
    yyjson_mut_val *layer_picker = NULL;
    pc_status layer_picker_status = pc_ui_layer_picker_to_json(&ui->layer_picker, d, &layer_picker, err);
    if (layer_picker_status != PC_OK) {
        yyjson_mut_doc_free(d);
        return layer_picker_status;
    }
    yyjson_mut_obj_add_val(d, root, "layerPicker", layer_picker);
    pc_ui_lasso_sync(s, ui);
    yyjson_mut_val *lasso_state = NULL;
    if (pc_ui_lasso_to_json(&ui->lasso, d, &lasso_state, err) != PC_OK) {
        yyjson_mut_doc_free(d);
        return err ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_obj_add_val(d, root, "lasso", lasso_state);
    /* Engine views (session + active document). */
    pc_json_value sess = PC_JSON_VALUE_INIT;
    if (pc_engine_inspect_session(s, &sess, NULL) == PC_OK) {
        add_json_value(d, root, "session", &sess);
    } else {
        yyjson_mut_obj_add_null(d, root, "session");
    }
    pc_json_value_fini(&sess);

    const pc_engine_doc_state *active = pc_engine_session_active(s);
    if (active != NULL) {
        pc_json_value doc = PC_JSON_VALUE_INIT;
        if (pc_engine_inspect_document(active, &doc, NULL) == PC_OK) {
            add_json_value(d, root, "document", &doc);
        } else {
            yyjson_mut_obj_add_null(d, root, "document");
        }
        pc_json_value_fini(&doc);
    } else {
        yyjson_mut_obj_add_null(d, root, "document");
    }

    return wrap(d, root, out, err);
}

/* ---- ui.menu.list ---- */

static pc_status menu_list(pc_ui_shell_app *a, pc_json_value *out, pc_error *err)
{
    pc_ui_menu_item *items = NULL;
    size_t n = 0;
    if (pc_ui_menu_state_items(pc_ui_shell_app_session(a), pc_ui_shell_app_state(a), &items, &n, err) !=
        PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        pc_ui_menu_items_free(items, n);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < n; i++) {
        const pc_ui_menu_item *it = &items[i];
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, o, "id", it->id);
        yyjson_mut_obj_add_strcpy(d, o, "label", it->label);
        yyjson_mut_val *path = yyjson_mut_arr(d);
        for (size_t k = 0; k < it->depth; k++) {
            if (it->path[k] != NULL) {
                yyjson_mut_arr_add_strcpy(d, path, it->path[k]);
            }
        }
        yyjson_mut_obj_add_val(d, o, "path", path);
        if (it->shortcut != NULL) {
            yyjson_mut_obj_add_strcpy(d, o, "shortcut", it->shortcut);
        } else {
            yyjson_mut_obj_add_null(d, o, "shortcut");
        }
        yyjson_mut_obj_add_bool(d, o, "enabled",
                                it->enabled &&
                                    (!pc_ui_shell_app_state(a)->n_dialogs || pc_ui_is_nav_command(it->id)));
        if (it->checked >= 0) {
            yyjson_mut_obj_add_bool(d, o, "checked", it->checked != 0);
        }
        yyjson_mut_arr_add_val(arr, o);
    }
    pc_ui_menu_items_free(items, n);
    return wrap(d, arr, out, err);
}

/* ---- ui.set ---- */

static bool control_unsigned(yyjson_val *value, uint64_t *out)
{
    if (pc_json_as_u64(value, out)) {
        return true;
    }
    double number = 0;
    if (!pc_json_as_f64(value, &number) || !isfinite(number) || number < 0 ||
        number >= 18446744073709551616.0 || floor(number) != number) {
        return false;
    }
    *out = (uint64_t)number;
    return true;
}

static pc_status ui_set(pc_ui_shell_app *a, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    if (p != NULL && yyjson_is_obj(p)) {
        /* Validate every field name first, so a rejected call applies nothing (#412). */
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(p, idx, max, key, val)
        {
            const char *k = yyjson_get_str(key);
            if (k == NULL || !is_ui_set_field(k)) {
                return pc_fail(err, PC_ERR_INVALID, "unknown field `%s`", k ? k : "");
            }
        }
        /* Validate the supported fields' values before mutating. */
        pc_ui_brush_picker next_picker = ui->brush_picker;
        bool next_grid = ui->brushes_grid, has_brush_size = false;
        double brush_size = 0;
        pc_status picker_status =
            pc_ui_brush_picker_configure(&next_picker, p, &next_grid, &has_brush_size, &brush_size, err);
        if (picker_status != PC_OK)
            return picker_status;
        bool slice_selected = ui->slices.has_selected, slice_hide_auto = ui->slices.hide_auto;
        uint64_t slice_number = ui->slices.selected;
        yyjson_val *slice_patch = pc_json_get(p, "slices");
        if (slice_patch) {
            if (!yyjson_is_obj(slice_patch))
                return pc_fail(err, PC_ERR_INVALID, "slices must be an object");
            size_t i, count;
            yyjson_val *slice_key, *value;
            yyjson_obj_foreach(slice_patch, i, count, slice_key, value)
            {
                const char *name = yyjson_get_str(slice_key);
                if (!strcmp(name, "selected")) {
                    slice_selected = !yyjson_is_null(value);
                    if (slice_selected &&
                        (!pc_json_as_u64(value, &slice_number) || !slice_number || slice_number > 1000000))
                        return pc_fail(err, PC_ERR_INVALID, "invalid selected slice");
                } else if (!strcmp(name, "hide_auto")) {
                    if (!pc_json_as_bool(value, &slice_hide_auto))
                        return pc_fail(err, PC_ERR_INVALID, "invalid slice visibility");
                } else
                    return pc_fail(err, PC_ERR_INVALID, "unknown slice field");
            }
        }
        yyjson_val *slice_choice = pc_json_get(slice_patch, "selected");
        if (slice_choice && slice_selected) {
            pc_ui_slice_state probe = {.has_selected = true, .selected = (size_t)slice_number};
            pc_status status =
                pc_ui_slice_resolve(&probe, pc_engine_session_active(pc_ui_shell_app_session(a)), err);
            bool found = status == PC_OK && probe.has_selected;
            pc_ui_slice_fini(&probe);
            if (!found)
                return status != PC_OK ? status : pc_fail(err, PC_ERR_INVALID, "no such selected slice");
        }
        pc_ui_timeline next_timeline = ui->timeline;
        yyjson_val *timeline = pc_json_get(p, "timeline");
        if (timeline) {
            pc_status status = pc_ui_timeline_configure(&next_timeline, timeline, err);
            if (status != PC_OK)
                return status;
            const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(a));
            if (next_timeline.playing && (!state || !pc_doc_arc_get(state->doc)->has_timeline))
                return pc_fail(err, PC_ERR_INVALID, "create a timeline before starting playback");
        }
        yyjson_val *background_value = pc_json_get(p, "backgroundJobs");
        if (background_value && !yyjson_is_bool(background_value))
            return pc_fail(err, PC_ERR_INVALID, "backgroundJobs must be a boolean");
        yyjson_val *v;
        pc_ui_tool tool = ui->tool;
        bool set_tool = false;
        if ((v = pc_json_get(p, "tool")) != NULL) {
            const char *name = pc_json_as_str(v, NULL);
            if (name == NULL || !pc_ui_tool_from_name(name, &tool)) {
                return pc_fail(err, PC_ERR_INVALID, "unknown tool `%s`", name ? name : "");
            }
            set_tool = true;
        }
        pc_ui_theme_kind theme = ui->theme;
        bool set_theme = false;
        if ((v = pc_json_get(p, "theme")) != NULL) {
            const char *name = pc_json_as_str(v, NULL);
            if (name == NULL || !pc_ui_theme_kind_from_name(name, &theme)) {
                return pc_fail(err, PC_ERR_INVALID, "unknown theme `%s`", name ? name : "");
            }
            set_theme = true;
        }
        uint64_t sel = ui->selection_mode;
        if ((v = pc_json_get(p, "selectionMode")) != NULL) {
            if (!pc_json_as_u64(v, &sel) || sel > 3) {
                return pc_fail(err, PC_ERR_INVALID, "selectionMode must be 0..3");
            }
        }
        double zoom = 0;
        bool set_zoom = false;
        if ((v = pc_json_get(p, "zoom")) != NULL) {
            if (!pc_json_as_f64(v, &zoom) || !isfinite(zoom) || zoom > FLT_MAX || !(zoom > 0.0)) {
                return pc_fail(err, PC_ERR_INVALID, "zoom must be a positive number");
            }
            set_zoom = true;
        }
        double rotation = 0;
        bool set_rot = false;
        if ((v = pc_json_get(p, "rotation")) != NULL) {
            if (!pc_json_as_f64(v, &rotation) || !isfinite(rotation) || fabs(rotation) > FLT_MAX) {
                return pc_fail(err, PC_ERR_INVALID, "rotation must be a number");
            }
            set_rot = true;
        }
        double cx = 0, cy = 0;
        yyjson_val *center = pc_json_get(p, "center");
        if (center && (!yyjson_is_arr(center) || yyjson_arr_size(center) != 2 ||
                       !pc_json_as_f64(yyjson_arr_get(center, 0), &cx) ||
                       !pc_json_as_f64(yyjson_arr_get(center, 1), &cy) || !isfinite(cx) || !isfinite(cy) ||
                       fabs(cx) > FLT_MAX || fabs(cy) > FLT_MAX)) {
            return pc_fail(err, PC_ERR_INVALID, "center must be two finite numbers representable as f32");
        }
        bool fit = false;
        yyjson_val *fit_value = pc_json_get(p, "fit");
        if (fit_value && !pc_json_as_bool(fit_value, &fit)) {
            return pc_fail(err, PC_ERR_INVALID, "fit must be a boolean");
        }
        double dock_width = ui->dock_width;
        bool tile_grid = ui->view.tile_grid;
        v = pc_json_get(p, "dockWidth");
        if (v && (!pc_json_as_f64(v, &dock_width) || !isfinite(dock_width))) {
            return pc_fail(err, PC_ERR_INVALID, "dockWidth must be finite");
        }
        pc_ui_panels next_panels = ui->panels;
        pc_ui_dock_tabs next_tabs = ui->dock_tabs;
        pc_ui_color_panel next_color = ui->color_panel;
        bool next_mask = ui->mask_target, next_vector_mask = ui->vector_mask_target;
        const char *bool_names[] = {"maskTarget", "vectorMaskTarget"};
        bool *bool_values[] = {&next_mask, &next_vector_mask};
        for (size_t i = 0; i < 2; i++) {
            v = pc_json_get(p, bool_names[i]);
            if (v && !pc_json_as_bool(v, bool_values[i])) {
                return pc_fail(err, PC_ERR_INVALID, "%s must be a boolean", bool_names[i]);
            }
        }
        v = pc_json_get(p, "tileGrid");
        if (v && !pc_json_as_bool(v, &tile_grid)) {
            return pc_fail(err, PC_ERR_INVALID, "tileGrid must be a boolean");
        }
        v = pc_json_get(p, "dock");
        if (v && !yyjson_is_obj(v)) {
            return pc_fail(err, PC_ERR_INVALID, "dock must be an object");
        }
        v = pc_json_get(p, "panels");
        if (v) {
            if (!yyjson_is_obj(v)) {
                return pc_fail(err, PC_ERR_INVALID, "panels must be an object");
            }
            struct {
                const char *name;
                bool *value;
            } fields[] = {{"layers", &next_panels.layers},
                          {"history", &next_panels.history},
                          {"properties", &next_panels.properties},
                          {"color", &next_panels.color},
                          {"navigator", &next_panels.navigator},
                          {"toolbar", &next_panels.toolbar},
                          {"options_bar", &next_panels.options_bar},
                          {"status_bar", &next_panels.status_bar},
                          {"brush_settings", &next_panels.brush_settings},
                          {"character", &next_panels.character},
                          {"toolbar_double", &next_panels.toolbar_double},
                          {"dock", &next_panels.dock},
                          {"rail", &next_panels.rail},
                          {"menu_bar", &next_panels.menu_bar},
                          {"gradient_editor", &next_panels.gradient_editor}};
            size_t pi, pn;
            yyjson_val *pk, *pv;
            yyjson_obj_foreach(v, pi, pn, pk, pv)
            {
                const char *name = yyjson_get_str(pk);
                bool found = false;
                for (size_t j = 0; j < sizeof fields / sizeof *fields; j++) {
                    if (strcmp(name, fields[j].name) == 0) {
                        found = true;
                        if (!pc_json_as_bool(pv, fields[j].value)) {
                            return pc_fail(err, PC_ERR_INVALID, "panels.%s must be a boolean", name);
                        }
                    }
                }
                if (strcmp(name, "hidden_by_tab") == 0) {
                    found = true;
                    next_panels.has_hidden_by_tab = !yyjson_is_null(pv);
                    if (next_panels.has_hidden_by_tab) {
                        if (!yyjson_is_arr(pv) || yyjson_arr_size(pv) != 3) {
                            return pc_fail(err, PC_ERR_INVALID,
                                           "panels.hidden_by_tab must be three booleans");
                        }
                        for (size_t j = 0; j < 3; j++) {
                            if (!pc_json_as_bool(yyjson_arr_get(pv, j), &next_panels.hidden_by_tab[j])) {
                                return pc_fail(err, PC_ERR_INVALID,
                                               "panels.hidden_by_tab must be three booleans");
                            }
                        }
                    }
                }
                if (!found) {
                    return pc_fail(err, PC_ERR_INVALID, "unknown panels field `%s`", name);
                }
            }
        }
        v = pc_json_get(p, "dockTabs");
        if (v) {
            if (!yyjson_is_obj(v)) {
                return pc_fail(err, PC_ERR_INVALID, "dockTabs must be an object");
            }
            struct {
                const char *name;
                size_t *value;
            } fields[] = {{"properties", &next_tabs.properties}, {"color", &next_tabs.color},
                          {"layers", &next_tabs.layers},         {"navigator", &next_tabs.navigator},
                          {"history", &next_tabs.history},       {"character", &next_tabs.character}};
            size_t di, dn;
            yyjson_val *dk, *dv;
            yyjson_obj_foreach(v, di, dn, dk, dv)
            {
                const char *name = yyjson_get_str(dk);
                bool found = false;
                uint64_t number = 0;
                for (size_t j = 0; j < sizeof fields / sizeof *fields; j++) {
                    if (strcmp(name, fields[j].name) == 0) {
                        found = true;
                        if (!control_unsigned(dv, &number) || number > SIZE_MAX) {
                            return pc_fail(err, PC_ERR_INVALID, "dockTabs.%s must be a non-negative integer",
                                           name);
                        }
                        *fields[j].value = (size_t)number;
                    }
                }
                if (!found) {
                    return pc_fail(err, PC_ERR_INVALID, "unknown dockTabs field `%s`", name);
                }
            }
        }
        v = pc_json_get(p, "colorPanel");
        if (v) {
            if (!yyjson_is_obj(v)) {
                return pc_fail(err, PC_ERR_INVALID, "colorPanel must be an object");
            }
            next_color.background = false;
            size_t ci, cn;
            yyjson_val *ck, *cv;
            yyjson_obj_foreach(v, ci, cn, ck, cv)
            {
                if (strcmp(yyjson_get_str(ck), "background") != 0 ||
                    !pc_json_as_bool(cv, &next_color.background)) {
                    return pc_fail(err, PC_ERR_INVALID, "colorPanel.background must be a boolean");
                }
            }
        }
        size_t next_section = ui->brush_section, next_tab = ui->brush_tab;
        const char *number_names[] = {"brushSection", "brushTab"};
        size_t *numbers[] = {&next_section, &next_tab};
        for (size_t i = 0; i < 2; i++) {
            v = pc_json_get(p, number_names[i]);
            uint64_t number = 0;
            if (v) {
                if (!control_unsigned(v, &number) || number > SIZE_MAX) {
                    return pc_fail(err, PC_ERR_INVALID, "%s must be a non-negative integer", number_names[i]);
                }
                *numbers[i] = (size_t)number;
            }
        }
        pc_ui_tool_options next_options = {0};
        pc_status prepared = pc_ui_control_options_prepare(&ui->tool_options, p, &next_options, err);
        if (prepared != PC_OK) {
            return prepared;
        }
        pc_ui_dock_layout next_dock = {0};
        bool set_dock = pc_json_get(p, "dock") != NULL;
        if (set_dock) {
            prepared = pc_ui_dock_layout_parse(&next_dock, pc_json_get(p, "dock"), err);
            if (prepared != PC_OK) {
                pc_ui_tool_options_fini(&next_options);
                return prepared;
            }
        }
        pc_status sync = pc_ui_shell_app_sync_views(a, err);
        if (sync != PC_OK) {
            pc_ui_tool_options_fini(&next_options);
            pc_ui_dock_layout_fini(&next_dock);
            return sync;
        }

        if (has_brush_size) {
            char request[128];
            (void)snprintf(request, sizeof request, "{\"size\":%.17g}", brush_size);
            pc_json_value result = PC_JSON_VALUE_INIT;
            pc_status status = pc_engine_session_execute_json(pc_ui_shell_app_session(a), "tools.setBrush",
                                                              request, &result, err);
            pc_json_value_fini(&result);
            if (status != PC_OK) {
                pc_ui_tool_options_fini(&next_options);
                pc_ui_dock_layout_fini(&next_dock);
                return status;
            }
        }
        if (set_theme) {
            char request[192];
            (void)snprintf(request, sizeof request, "{\"path\":\"interface.theme\",\"value\":\"%s\"}",
                           pc_ui_theme_kind_id(theme));
            pc_json_value result = PC_JSON_VALUE_INIT;
            pc_status status = pc_engine_session_execute_json(pc_ui_shell_app_session(a), "prefs.set",
                                                              request, &result, err);
            pc_json_value_fini(&result);
            if (status != PC_OK) {
                pc_ui_tool_options_fini(&next_options);
                pc_ui_dock_layout_fini(&next_dock);
                return status;
            }
        }
        if (set_tool && tool != ui->tool && (ui->has_pen || ui->has_text_edit)) {
            pc_json_value committed = PC_JSON_VALUE_INIT;
            pc_status status = PC_OK;
            if (ui->has_pen && !pc_ui_pen_drawn_with(&ui->pen, tool)) {
                status = pc_ui_pen_commit(pc_ui_shell_app_session(a), ui, false, &committed, err);
            }
            if (status == PC_OK && ui->has_text_edit) {
                status = pc_ui_type_commit(pc_ui_shell_app_session(a), ui, 0, &committed, err);
            }
            pc_json_value_fini(&committed);
            if (status != PC_OK) {
                pc_ui_tool_options_fini(&next_options);
                pc_ui_dock_layout_fini(&next_dock);
                return status;
            }
            if (!pc_json_get(p, "maskTarget")) {
                next_mask = ui->mask_target;
            }
            if (!pc_json_get(p, "vectorMaskTarget")) {
                next_vector_mask = ui->vector_mask_target;
            }
        }
        if (ui->transform.active && tool != ui->tool) {
            char id[80];
            pc_ui_tool_select_id(tool, id, sizeof id);
            pc_json_value ignored = PC_JSON_VALUE_INIT;
            pc_status transform_status = pc_ui_shell_transform_command(a, id, NULL, &ignored, err);
            pc_json_value_fini(&ignored);
            if (transform_status != PC_OK && transform_status != PC_ERR_NOT_FOUND) {
                pc_ui_tool_options_fini(&next_options);
                pc_ui_dock_layout_fini(&next_dock);
                return transform_status;
            }
        }
        if (ui->crop_editing && tool != ui->tool) {
            char id[80];
            pc_ui_tool_select_id(tool, id, sizeof id);
            pc_json_value ignored = PC_JSON_VALUE_INIT;
            pc_status crop_status = pc_ui_shell_crop_command(a, id, NULL, &ignored, err);
            pc_json_value_fini(&ignored);
            if (crop_status != PC_OK && crop_status != PC_ERR_NOT_FOUND) {
                pc_ui_tool_options_fini(&next_options);
                pc_ui_dock_layout_fini(&next_dock);
                return crop_status;
            }
        }
        /* Apply (nothing above failed). */
        if (set_dock) {
            pc_ui_dock_layout_fini(&ui->dock);
            ui->dock = next_dock;
        }
        ui->dock_width = (float)fmax(250, fmin(520, dock_width));
        ui->view.tile_grid = tile_grid;
        pc_ui_shell_app_changed(a);
        pc_ui_tool_options_fini(&ui->tool_options);
        ui->tool_options = next_options;
        ui->panels = next_panels;
        ui->dock_tabs = next_tabs;
        ui->color_panel = next_color;
        ui->mask_target = next_mask;
        ui->vector_mask_target = next_vector_mask;
        ui->brush_section = next_section;
        ui->brush_tab = next_tab;
        ui->timeline = next_timeline;
        if (slice_choice &&
            (ui->slices.has_selected != slice_selected || ui->slices.selected != slice_number))
            ui->slices.dragging = false;
        ui->slices.has_selected = slice_selected;
        ui->slices.selected = (size_t)slice_number;
        ui->slices.hide_auto = slice_hide_auto;
        ui->brush_picker = next_picker;
        ui->brushes_grid = next_grid;
        if (set_tool) {
            if (tool != ui->tool)
                pc_ui_direct_cancel(&ui->direct);
            ui->tool = tool;
            pc_ui_layer_picker_prune(pc_ui_shell_app_session(a), ui);
            pc_ui_state_remember_group_tool(ui);
        }
        if (set_theme) {
            ui->theme = theme;
            pc_ui_shell_app_apply_theme(a);
        }
        if (background_value)
            pc_ui_shell_app_background_jobs(a, yyjson_get_bool(background_value));
        ui->selection_mode = (uint8_t)sel;
        size_t active = 0;
        (void)pc_engine_session_active_index(pc_ui_shell_app_session(a), &active);
        pc_ui_view *view = active < ui->n_views ? &ui->views[active] : NULL;
        if (view) {
            if (set_zoom) {
                view->zoom = pc_ui_zoom_clamp((float)zoom, view->doc_size);
            }
            if (set_rot) {
                view->rotation = pc_ui_rotation_wrap((float)rotation);
            }
            if (center) {
                view->center[0] = (float)cx;
                view->center[1] = (float)cy;
            }
            if (set_zoom || center) {
                view->fit_pending = false;
                view->fill_pending = false;
            }
            if (fit_value) {
                view->fit_pending = fit;
            }
        }
    }
    pc_ui_shell_sync_targets(a);
    return ok_empty(out, err);
}

/* ---- ui.pointer (validated; the gesture wiring to tools lands with U4/U6/U7) ---- */

static pc_status input_fields(yyjson_val *object, const char *const *allowed, size_t count, pc_error *err)
{
    if (!yyjson_is_obj(object)) {
        return pc_fail(err, PC_ERR_INVALID, "input must be an object");
    }
    size_t i, n;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, i, n, key, value)
    {
        (void)value;
        const char *name = yyjson_get_str(key);
        bool found = false;
        for (size_t j = 0; j < count; j++) {
            if (!strcmp(name, allowed[j])) {
                found = true;
                break;
            }
        }
        if (!found) {
            return pc_fail(err, PC_ERR_INVALID, "unknown input field `%s`", name);
        }
    }
    return PC_OK;
}
static bool input_number(yyjson_val *object, const char *key, double fallback, double min, double max,
                         float *out)
{
    yyjson_val *value = pc_json_get(object, key);
    double number = fallback;
    if (value && !yyjson_is_null(value) && !pc_json_as_f64(value, &number)) {
        return false;
    }
    if (!isfinite(number) || number < min || number > max) {
        return false;
    }
    *out = (float)number;
    return true;
}
static pc_status ui_pointer(pc_ui_shell_app *app, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *const top[] = {"events", "tool",    "modifiers", "button", "shift",
                               "alt",    "command", "ctrl",      "space"};
    const char *const mod_names[] = {"shift", "alt", "command", "ctrl", "space"};
    const unsigned mod_bits[] = {PC_UI_MOD_SHIFT, PC_UI_MOD_ALT, PC_UI_MOD_COMMAND, PC_UI_MOD_CTRL, 0};
    const char *const event_names[] = {"kind", "x", "y", "pressure", "tiltX", "tiltY", "rotation"};
    pc_status status = input_fields(p, top, sizeof top / sizeof *top, err);
    if (status != PC_OK) {
        return status;
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_tool tool = ui->tool;
    yyjson_val *tv = pc_json_get(p, "tool");
    if (tv && !yyjson_is_null(tv) &&
        (!yyjson_is_str(tv) || !pc_ui_tool_from_name(yyjson_get_str(tv), &tool))) {
        return pc_fail(err, PC_ERR_INVALID, "unknown pointer tool");
    }
    yyjson_val *mods = pc_json_get(p, "modifiers");
    if (mods && !yyjson_is_null(mods)) {
        status = input_fields(mods, mod_names, 5, err);
        if (status != PC_OK) {
            return status;
        }
    }
    unsigned modifiers = 0;
    bool space = false;
    for (size_t i = 0; i < 5; i++) {
        yyjson_val *v = pc_json_get(mods, mod_names[i]);
        yyjson_val *top_value = pc_json_get(p, mod_names[i]);
        if (top_value && !yyjson_is_null(top_value) && !yyjson_is_bool(top_value)) {
            return pc_fail(err, PC_ERR_INVALID, "%s must be boolean", mod_names[i]);
        }
        if (v && !yyjson_is_null(v) && !yyjson_is_bool(v)) {
            return pc_fail(err, PC_ERR_INVALID, "modifiers.%s must be boolean", mod_names[i]);
        }
        bool enabled = yyjson_is_bool(v) ? yyjson_get_bool(v) : yyjson_get_bool(top_value);
        if (enabled) {
            modifiers |= mod_bits[i];
            if (i == 4) {
                space = true;
            }
        }
    }
    yyjson_val *button = pc_json_get(p, "button");
    const char *button_name = yyjson_get_str(button);
    bool secondary = button_name && (!strcmp(button_name, "secondary") || !strcmp(button_name, "right"));
    if (button && !yyjson_is_null(button) &&
        (!button_name || (!secondary && strcmp(button_name, "left") && strcmp(button_name, "primary"))))
        return pc_fail(err, PC_ERR_INVALID, "unknown pointer button");
    if (space && !(tool == PC_TOOL_CROP && pc_ui_shell_crop_runtime(app)->drag.active &&
                   pc_ui_shell_crop_runtime(app)->drag.draw)) {
        return pc_fail(err, PC_ERR_UNSUPPORTED,
                       "Space gesture translation is not available for this gesture yet");
    }
    yyjson_val *events = pc_json_get(p, "events");
    if (!yyjson_is_arr(events)) {
        return pc_fail(err, PC_ERR_INVALID, "`events` must be an array");
    }
    size_t count = yyjson_arr_size(events);
    if (count > 100000) {
        return pc_fail(err, PC_ERR_LIMIT, "too many pointer events");
    }
    typedef struct parsed_pointer {
        pc_ui_pointer_kind kind;
        pc_ui_tool_point point;
    } parsed_pointer;
    parsed_pointer *parsed = calloc(count ? count : 1, sizeof *parsed);
    if (!parsed) {
        return pc_fail(err, PC_ERR_NOMEM, "pointer events");
    }
    for (size_t i = 0; i < count; i++) {
        yyjson_val *ev = yyjson_arr_get(events, i);
        status = input_fields(ev, event_names, sizeof event_names / sizeof *event_names, err);
        if (status != PC_OK) {
            break;
        }
        yyjson_val *kind = pc_json_get(ev, "kind");
        const char *name = yyjson_get_str(kind);
        parsed[i].kind = PC_UI_POINTER_MOVE;
        if (kind && !yyjson_is_null(kind)) {
            if (name && !strcmp(name, "down")) {
                parsed[i].kind = PC_UI_POINTER_DOWN;
            } else if (name && !strcmp(name, "up")) {
                parsed[i].kind = PC_UI_POINTER_UP;
            } else if (!name || strcmp(name, "move")) {
                status = pc_fail(err, PC_ERR_INVALID, "unknown event kind");
                break;
            }
        }
        pc_ui_tool_point *point = &parsed[i].point;
        point->wheel = 1;
        if (!input_number(ev, "x", 0, -1e8, 1e8, &point->x) ||
            !input_number(ev, "y", 0, -1e8, 1e8, &point->y) ||
            !input_number(ev, "pressure", 1, 0, 1, &point->pressure) ||
            !input_number(ev, "tiltX", 0, -90, 90, &point->tilt_x) ||
            !input_number(ev, "tiltY", 0, -90, 90, &point->tilt_y) ||
            !input_number(ev, "rotation", 0, 0, 360, &point->rotation)) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid pointer coordinate, pressure or pen angle");
            break;
        }
    }
    if (status == PC_OK && count && !secondary && !ui->transform.active &&
        !pc_ui_tool_gesture_supported(tool) && !pc_ui_tool_is_type(tool) && !pc_ui_pen_tool(tool) &&
        !pc_ui_direct_tool(tool) && !pc_ui_lasso_tool(tool) && !pc_ui_slice_tool(tool) &&
        tool != PC_TOOL_ARTBOARD && !pc_ui_layer_picker_armed(pc_ui_shell_app_session(app), ui) &&
        !pc_ui_sampling_tool_kind(tool) && !pc_ui_alt_samples_color(tool, modifiers)) {
        status = pc_fail(err, PC_ERR_UNSUPPORTED, "tool pointer routing is not available yet");
    }
    if (status == PC_OK && ui->n_dialogs && count) {
        status = pc_fail(err, PC_ERR_INVALID, "confirm or cancel the open dialog first");
    }
    yyjson_mut_doc *d = status == PC_OK ? yyjson_mut_doc_new(NULL) : NULL;
    if (status == PC_OK && !d) {
        status = pc_fail(err, PC_ERR_NOMEM, "pointer result");
    }
    if (status == PC_OK && ui->has_pen && !pc_ui_pen_drawn_with(&ui->pen, tool)) {
        pc_json_value committed = PC_JSON_VALUE_INIT;
        status = pc_ui_pen_commit(pc_ui_shell_app_session(app), ui, false, &committed, err);
        pc_json_value_fini(&committed);
    }
    if (status == PC_OK) {
        if (tool != ui->tool)
            pc_ui_direct_cancel(&ui->direct);
        ui->tool = tool;
        bool old_space = pc_ui_shell_crop_runtime(app)->space;
        pc_ui_shell_crop_runtime(app)->space = space;
        for (size_t i = 0; i < count; i++) {
            pc_json_value result = PC_JSON_VALUE_INIT;
            status = secondary ? pc_ui_shell_secondary_pointer(app, parsed[i].kind, parsed[i].point,
                                                               modifiers, &result, err)
                               : pc_ui_shell_app_pointer(app, parsed[i].kind, parsed[i].point, modifiers,
                                                         &result, err);
            pc_json_value_fini(&result);
            if (status != PC_OK) {
                break;
            }
        }
        pc_ui_shell_crop_runtime(app)->space = old_space;
    }
    free(parsed);
    if (status != PC_OK) {
        yyjson_mut_doc_free(d);
        return status;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "queued", count);
    return wrap(d, root, out, err);
}

/* ---- ui.screenshot ---- */

static pc_status screenshot(pc_ui_shell_app *a, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    uint8_t *rgba = NULL;
    int w = 0, h = 0;
    if (pc_ui_shell_app_capture(a, &rgba, &w, &h, err) != PC_OK) {
        return err ? err->code : PC_ERR_IO;
    }
    pc_cimage img;
    pc_status rc = pc_cimage_from_copy((uint32_t)w, (uint32_t)h, PC_CL_RGBA, PC_CS_U8, rgba,
                                       (size_t)w * (size_t)h * 4, &img, err);
    free(rgba);
    if (rc != PC_OK) {
        return rc;
    }
    pc_buf png = PC_BUF_INIT;
    pc_cencode_opts opts = pc_cencode_opts_default();
    rc = pc_cencode(&img, PC_CF_PNG, &opts, &png, err);
    pc_cimage_fini(&img);
    if (rc != PC_OK) {
        pc_buf_fini(&png);
        return rc;
    }

    const char *path = p ? pc_json_as_str(pc_json_get(p, "path"), NULL) : NULL;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        pc_buf_fini(&png);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    if (path != NULL) {
        /* The capability-scoped write root is enforced by the control server (P1); the shell writes
         * the bytes atomically. */
        rc = pc_file_write_atomic(path, png.data, png.len, err);
        if (rc != PC_OK) {
            pc_buf_fini(&png);
            yyjson_mut_doc_free(d);
            return rc;
        }
        yyjson_mut_obj_add_strcpy(d, root, "path", path);
        yyjson_mut_obj_add_uint(d, root, "width", (uint64_t)w);
        yyjson_mut_obj_add_uint(d, root, "height", (uint64_t)h);
    } else {
        pc_buf b64 = PC_BUF_INIT;
        rc = pc_base64_encode(png.data, png.len, &b64, err);
        if (rc != PC_OK) {
            pc_buf_fini(&png);
            pc_buf_fini(&b64);
            yyjson_mut_doc_free(d);
            return rc;
        }
        yyjson_mut_obj_add_strncpy(d, root, "base64", (const char *)b64.data, b64.len);
        yyjson_mut_obj_add_strcpy(d, root, "mime", "image/png");
        yyjson_mut_obj_add_uint(d, root, "width", (uint64_t)w);
        yyjson_mut_obj_add_uint(d, root, "height", (uint64_t)h);
        pc_buf_fini(&b64);
    }
    pc_buf_fini(&png);
    return wrap(d, root, out, err);
}

static pc_status ui_text(pc_ui_shell_app *app, yyjson_val *params, pc_json_value *out, pc_error *err)
{
    const char *const fields[] = {"text"};
    pc_status status = input_fields(params, fields, 1, err);
    if (status != PC_OK) {
        return status;
    }
    size_t length = 0;
    const char *text = pc_json_as_str(pc_json_get(params, "text"), &length);
    if (!text || length > ((size_t)1 << 20)) {
        return pc_fail(err, PC_ERR_INVALID, "text must be a string of at most 1 MiB");
    }
    for (size_t i = 0; i < length;) {
        uint32_t scalar;
        if (!pc_utf8_next((const uint8_t *)text, length, &i, &scalar) || !scalar) {
            return pc_fail(err, PC_ERR_INVALID, "text must be valid UTF-8 without NUL");
        }
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->has_text_edit && !ui->n_dialogs && !pc_ui_shell_keyboard_captured(app)) {
        status = pc_ui_type_text_event(pc_ui_shell_app_session(app), ui, text, length, out, err);
        pc_ui_shell_app_changed(app);
        return status;
    }
    if (!pc_ui_shell_app_nk(app)) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "no text input target");
    }
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = text;
    pc_ui_shell_input_event(app, &event);
    return ok_empty(out, err);
}
static pc_status ui_key(pc_ui_shell_app *app, yyjson_val *params, pc_json_value *out, pc_error *err)
{
    const char *const fields[] = {"key", "modifiers", "shift", "alt", "command", "ctrl"};
    const char *const names[] = {"shift", "alt", "command", "ctrl"};
    const unsigned bits[] = {PC_UI_MOD_SHIFT, PC_UI_MOD_ALT, PC_UI_MOD_COMMAND, PC_UI_MOD_CTRL};
    pc_status status = input_fields(params, fields, 6, err);
    if (status != PC_OK) {
        return status;
    }
    const char *name = pc_json_as_str(pc_json_get(params, "key"), NULL);
    pc_ui_key key = name ? pc_ui_key_from_name(name) : PC_KEY_NONE;
    if (key == PC_KEY_NONE) {
        return pc_fail(err, PC_ERR_INVALID, "unknown key");
    }
    yyjson_val *group = pc_json_get(params, "modifiers");
    unsigned modifiers = 0;
    if (group && !yyjson_is_null(group)) {
        status = input_fields(group, names, 4, err);
        if (status != PC_OK) {
            return status;
        }
    }
    for (size_t i = 0; i < 4; i++) {
        yyjson_val *value = pc_json_get(params, names[i]), *nested = pc_json_get(group, names[i]);
        if ((value && !yyjson_is_null(value) && !yyjson_is_bool(value)) ||
            (nested && !yyjson_is_null(nested) && !yyjson_is_bool(nested))) {
            return pc_fail(err, PC_ERR_INVALID, "modifiers must be boolean");
        }
        if (yyjson_is_bool(nested)) {
            value = nested;
        }
        if (yyjson_get_bool(value)) {
            modifiers |= bits[i];
        }
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->context_menu.open || ui->transform.active || ui->tool == PC_TOOL_CROP ||
        ui->tool == PC_TOOL_ARTBOARD || pc_ui_slice_tool(ui->tool) ||
        pc_ui_shell_sampling_runtime(app)->active || ui->layer_picker.armed) {
        SDL_Keycode code = key == PC_KEY_ARROWLEFT    ? SDLK_LEFT
                           : key == PC_KEY_ARROWRIGHT ? SDLK_RIGHT
                           : key == PC_KEY_ARROWUP    ? SDLK_UP
                           : key == PC_KEY_ARROWDOWN  ? SDLK_DOWN
                           : key == PC_KEY_ENTER      ? SDLK_RETURN
                           : key == PC_KEY_ESCAPE     ? SDLK_ESCAPE
                                                      : SDL_GetKeyFromName(name);
        if (code) {
            SDL_Event event;
            SDL_zero(event);
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.key = code;
            event.key.down = true;
            if (modifiers & PC_UI_MOD_SHIFT)
                event.key.mod |= SDL_KMOD_SHIFT;
            if (modifiers & PC_UI_MOD_ALT)
                event.key.mod |= SDL_KMOD_ALT;
            if (modifiers & (PC_UI_MOD_COMMAND | PC_UI_MOD_CTRL))
                event.key.mod |= SDL_KMOD_CTRL;
            if (pc_ui_shell_app_nk(app)) {
                pc_ui_shell_input_event(app, &event);
                event.type = SDL_EVENT_KEY_UP;
                event.key.down = false;
                pc_ui_shell_input_event(app, &event);
                return ok_empty(out, err);
            }
            if (pc_ui_shell_layer_picker_event(app, &event) || pc_ui_shell_sampling_event(app, &event) ||
                pc_ui_shell_artboard_event(app, &event) || pc_ui_shell_slice_event(app, &event) ||
                pc_ui_shell_lasso_event(app, &event) || pc_ui_shell_context_event(app, &event) ||
                pc_ui_shell_crop_event(app, &event) || pc_ui_shell_transform_key(app, &event))
                return ok_empty(out, err);
        }
    }
    if (!ui->n_dialogs && !pc_ui_shell_keyboard_captured(app)) {
        status = pc_ui_lasso_key(pc_ui_shell_app_session(app), ui, key, modifiers, 1, out, err);
        if (status != PC_ERR_NOT_FOUND)
            return status;
    }
    if (!ui->n_dialogs && !pc_ui_shell_keyboard_captured(app) &&
        (ui->direct.active || pc_ui_direct_handles(ui, modifiers))) {
        status = pc_ui_direct_key(pc_ui_shell_app_session(app), ui, key, modifiers, out, err);
        if (status != PC_ERR_NOT_FOUND) {
            return status;
        }
    }
    if (ui->has_pen && !ui->has_text_edit && !ui->n_dialogs && !pc_ui_shell_keyboard_captured(app)) {
        status = pc_ui_pen_key(pc_ui_shell_app_session(app), ui, key, modifiers, out, err);
        if (status != PC_ERR_NOT_FOUND) {
            pc_ui_shell_app_changed(app);
            return status;
        }
    }
    if (ui->has_text_edit && !ui->n_dialogs && !pc_ui_shell_keyboard_captured(app)) {
        status = pc_ui_type_key(pc_ui_shell_app_session(app), ui, &pc_ui_shell_type_pointer(app)->layout, key,
                                modifiers, out, err);
        pc_ui_shell_app_changed(app);
        return status == PC_ERR_NOT_FOUND ? ok_empty(out, err) : status;
    }
    if (pc_ui_shell_app_nk(app)) {
        SDL_Keycode code = SDL_GetKeyFromName(name);
        switch (key) {
        case PC_KEY_ARROWLEFT:
            code = SDLK_LEFT;
            break;
        case PC_KEY_ARROWRIGHT:
            code = SDLK_RIGHT;
            break;
        case PC_KEY_ARROWUP:
            code = SDLK_UP;
            break;
        case PC_KEY_ARROWDOWN:
            code = SDLK_DOWN;
            break;
        case PC_KEY_ENTER:
            code = SDLK_RETURN;
            break;
        case PC_KEY_ESCAPE:
            code = SDLK_ESCAPE;
            break;
        default:
            break;
        }
        if (!code) {
            return pc_fail(err, PC_ERR_UNSUPPORTED, "key is unavailable in native input");
        }
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.down = true;
        if (modifiers & PC_UI_MOD_SHIFT) {
            event.key.mod |= SDL_KMOD_SHIFT;
        }
        if (modifiers & PC_UI_MOD_ALT) {
            event.key.mod |= SDL_KMOD_ALT;
        }
        if (modifiers & PC_UI_MOD_COMMAND) {
#if defined(__APPLE__)
            event.key.mod |= SDL_KMOD_GUI;
#else
            event.key.mod |= SDL_KMOD_CTRL;
#endif
        }
        if (modifiers & PC_UI_MOD_CTRL) {
            event.key.mod |= SDL_KMOD_CTRL;
        }
        pc_ui_shell_input_event(app, &event);
        event.type = SDL_EVENT_KEY_UP;
        event.key.down = false;
        pc_ui_shell_input_event(app, &event);
        return ok_empty(out, err);
    }
    if (!ui->n_dialogs && !pc_ui_shell_keyboard_captured(app)) {
        status = pc_ui_move_key(pc_ui_shell_app_session(app), ui, key, modifiers, out, err);
        if (status != PC_ERR_NOT_FOUND)
            return status;
    }
    pc_ui_binding *bindings = NULL;
    size_t count = 0;
    status = pc_ui_bindings(pc_ui_shell_app_session(app), &bindings, &count, err);
    if (status != PC_OK) {
        return status;
    }
    for (size_t i = 0; i < count; i++) {
        if (pc_ui_shortcut_key_matches(&bindings[i].sc, key, modifiers)) {
            if (pc_ui_command_disabled(pc_ui_shell_app_session(app), ui, bindings[i].id)) {
                free(bindings);
                return pc_fail(err, PC_ERR_INVALID, "key command is disabled");
            }
            status = pc_ui_shell_app_invoke(app, bindings[i].id, NULL, out, err);
            free(bindings);
            return status;
        }
    }
    free(bindings);
    return ok_empty(out, err);
}

/* ---- dispatch ---- */

pc_status pc_ui_control_handle(pc_ui_shell_app *a, const char *method, yyjson_val *params, pc_json_value *out,
                               pc_error *err)
{
    if (a == NULL || method == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing method");
    }
    pc_ui_shell_apply_preferences(a);
    if (!strcmp(method, "ui.context.choose"))
        return pc_ui_shell_app_invoke(a, method, params, out, err);
    if (strncmp(method, "ui.dialog.", 10) == 0) {
        return pc_ui_shell_dialog_control(a, method, params, out, err);
    }
    if (strcmp(method, "ui.gpu.simulateLoss") == 0) {
        bool error = false;
        yyjson_val *value = pc_json_get(params, "error");
        if (value && !pc_json_as_bool(value, &error)) {
            return pc_fail(err, PC_ERR_INVALID, "error must be a boolean");
        }
        return pc_ui_shell_app_gpu_simulate_loss(a, error, out, err);
    }
    if (!strcmp(method, "app.quit")) {
        return pc_ui_shell_app_invoke(a, "file.exit", params, out, err);
    }
    if (!strcmp(method, "ui.notice.dismiss"))
        return pc_ui_shell_app_invoke(a, method, params, out, err);
    if (strcmp(method, "ui.inspect") == 0) {
        pc_ui_shell_jobs_tick(a);
        return pc_ui_control_inspect(a, out, err);
    }
    if (strcmp(method, "ui.menu.list") == 0) {
        return menu_list(a, out, err);
    }
    if (strcmp(method, "ui.set") == 0) {
        return ui_set(a, params, out, err);
    }
    if (!strcmp(method, "ui.type")) {
        return ui_text(a, params, out, err);
    }
    if (!strcmp(method, "ui.key")) {
        return ui_key(a, params, out, err);
    }
    if (strcmp(method, "ui.pointer") == 0) {
        return ui_pointer(a, params, out, err);
    }
    if (strcmp(method, "ui.resize") == 0) {
        double w = 0, h = 0;
        if (!pc_json_as_f64(pc_json_get(params, "width"), &w) ||
            !pc_json_as_f64(pc_json_get(params, "height"), &h) || !(w > 0.0) || !(h > 0.0)) {
            return pc_fail(err, PC_ERR_INVALID, "width and height must be positive numbers");
        }
        pc_ui_shell_app_set_window_size(a, (float)w, (float)h);
        return ok_empty(out, err);
    }
    if (strcmp(method, "ui.screenshot") == 0) {
        return screenshot(a, params, out, err);
    }
    if (strcmp(method, "engine.execute") == 0 || strcmp(method, "ui.menu.invoke") == 0) {
        const char *id = pc_json_as_str(pc_json_get(params, "command"), NULL);
        if (id == NULL) {
            id = pc_json_as_str(pc_json_get(params, "id"), NULL);
        }
        if (id == NULL) {
            return pc_fail(err, PC_ERR_INVALID, "missing `command`");
        }
        yyjson_val *cmd_params = pc_json_get(params, "params");
        if (pc_ui_shell_app_state(a)->transform.active &&
            (!strcmp(id, "edit.undo") || !strcmp(id, "edit.redo") || !strcmp(id, "edit.toggleLastState")))
            return pc_ui_shell_transform_command(a, id, cmd_params, out, err);
        if (pc_ui_shell_app_state(a)->has_crop && !pc_ui_shell_app_state(a)->transform.active &&
            (pc_ui_shell_app_state(a)->crop_editing || pc_ui_shell_app_state(a)->crop_can_undo ||
             pc_ui_shell_app_state(a)->crop_can_redo) &&
            (!strcmp(id, "edit.undo") || !strcmp(id, "edit.redo")))
            return pc_ui_shell_crop_command(a, id, cmd_params, out, err);
        if (pc_ui_shell_app_state(a)->lasso.active && !strcmp(id, "edit.undo")) {
            pc_status status = pc_ui_shell_lasso_command(a, id, out, err);
            if (status != PC_ERR_NOT_FOUND)
                return status;
        }
        /* engine.execute runs engine commands directly; ui.menu.invoke follows the menu routing. */
        if (strcmp(method, "engine.execute") == 0 && pc_engine_command_find(id) != NULL) {
            pc_status status =
                pc_engine_session_execute(pc_ui_shell_app_session(a), id, cmd_params, out, err);
            if (status == PC_OK) {
                pc_ui_shell_app_changed(a);
                pc_ui_shell_apply_preferences(a);
                pc_ui_shell_result_warnings(a, id, out);
            }
            return status;
        }
        return pc_ui_shell_app_invoke(a, id, cmd_params, out, err);
    }
    if (strcmp(method, "engine.commands") == 0) {
        return pc_engine_session_execute(pc_ui_shell_app_session(a), "command.list", NULL, out, err);
    }
    return pc_fail(err, PC_ERR_NOT_FOUND, "method `%s` is not supported in this build yet", method);
}
