/* ui.* control-channel handler: see control.h. */
#include "control.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/base64.h"
#include "base/buf.h"
#include "base/file.h"
#include "codecs/codecs.h"
#include "codecs/image.h"
#include "ui/menus.h"
#include "ui/canvas.h"
#include "ui/shortcuts.h"

/* The ui.set fields, byte-for-byte from control.rs UI_SET_FIELDS, so an unknown field gives the same
 * error. Fields this scoped build cannot yet apply return PC_ERR_UNSUPPORTED (never silently drop). */
static const char *const UI_SET_FIELDS[] = {
    "tool",          "panels",         "dock",         "dockTabs",      "dockWidth",
    "colorPanel",    "maskTarget",     "vectorMaskTarget", "selectionMode", "zoom",
    "center",        "rotation",       "fit",          "theme",         "brushSection",
    "brushTab",      "brushesView",    "brushPicker",  "brushPickerName", "brushPickerStroke",
    "brushPickerTip","brushPickerScale","brushSize",   "gradientBlendMode", "gradientClassic",
    "eyedropperSampleSize", "eyedropperSample", "eyedropperRing", "cropOverlay", "cropOverlayShow",
    "cropOverlayOrientation", "cropShield", "shapeStroke", "camTransformOnDrop", "tileGrid"};
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
    const pc_ui_state *ui = pc_ui_shell_app_state(a);
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

    yyjson_mut_obj_add_strcpy(d, root, "tool", pc_ui_tool_serde(ui->tool));
    yyjson_mut_obj_add_val(d, root, "panels", panels_json(d, &ui->panels));
    yyjson_mut_obj_add_val(d, root, "extras", extras_json(d, &ui->extras));
    yyjson_mut_val *view_options = NULL;
    pc_status view_rc = pc_ui_view_options_to_json(&ui->view, d, &view_options, err);
    if (view_rc != PC_OK) { yyjson_mut_doc_free(d); return view_rc; }
    yyjson_mut_obj_add_val(d, root, "view", view_options);
    size_t active_view = 0;
    bool has_view = pc_engine_session_active_index(s, &active_view) && active_view < ui->n_views;
    yyjson_mut_obj_add_val(d, root, "activeView", has_view ? view_json(d, &ui->views[active_view]) : yyjson_mut_null(d));

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
    if (ui->gpu_fallback_notice != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, "gpuFallbackNotice", ui->gpu_fallback_notice);
    } else {
        yyjson_mut_obj_add_null(d, root, "gpuFallbackNotice");
    }
    yyjson_mut_obj_add_uint(d, root, "frame", pc_ui_shell_app_frame(a));

    /* The theme tokens, so a snapshot's palette can be checked without a display. */
    yyjson_mut_val *th = yyjson_mut_obj(d);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(a);
    add_color(d, th, "canvas", t->canvas);
    add_color(d, th, "chrome", t->chrome);
    add_color(d, th, "accent", t->accent);
    yyjson_mut_obj_add_val(d, root, "themeTokens", th);

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
    if (pc_ui_menu_items(pc_ui_shell_app_session(a), &items, &n, err) != PC_OK) {
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
        yyjson_mut_obj_add_bool(d, o, "enabled", it->enabled);
        if (it->checked >= 0) {
            yyjson_mut_obj_add_bool(d, o, "checked", it->checked != 0);
        }
        yyjson_mut_arr_add_val(arr, o);
    }
    pc_ui_menu_items_free(items, n);
    return wrap(d, arr, out, err);
}

/* ---- ui.set ---- */

static pc_status ui_set(pc_ui_shell_app *a, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(a);
    if (p != NULL && yyjson_is_obj(p)) {
        /* Validate every field name first, so a rejected call applies nothing (#412). */
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(p, idx, max, key, val) {
            const char *k = yyjson_get_str(key);
            if (k == NULL || !is_ui_set_field(k)) {
                return pc_fail(err, PC_ERR_INVALID, "unknown field `%s`", k ? k : "");
            }
        }
        /* Validate the supported fields' values before mutating. */
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
        /* Fields whose state this build does not model yet: a clear error, no partial apply. */
        static const char *unsupported[] = {"dock",         "dockWidth",      "brushPicker",
                                            "brushPickerName", "brushPickerStroke", "brushPickerTip",
                                            "brushPickerScale", "brushesView",  "gradientBlendMode",
                                            "gradientClassic", "eyedropperSampleSize", "eyedropperSample",
                                            "eyedropperRing", "cropOverlay",   "cropOverlayShow",
                                            "cropOverlayOrientation", "cropShield", "shapeStroke",
                                            "camTransformOnDrop", "tileGrid", "brushSize"};
        for (size_t i = 0; i < sizeof unsupported / sizeof unsupported[0]; i++) {
            if (pc_json_get(p, unsupported[i]) != NULL) {
                return pc_fail(err, PC_ERR_UNSUPPORTED,
                               "`%s` is not available in this build yet", unsupported[i]);
            }
        }

        pc_status sync = pc_ui_shell_app_sync_views(a, err);
        if (sync != PC_OK) { return sync; }

        /* Apply (nothing above failed). */
        if (set_tool) {
            ui->tool = tool;
            pc_ui_state_remember_group_tool(ui);
        }
        if (set_theme) {
            ui->theme = theme;
            pc_ui_shell_app_apply_theme(a);
        }
        ui->selection_mode = (uint8_t)sel;
        bool mt;
        if ((v = pc_json_get(p, "maskTarget")) != NULL && pc_json_as_bool(v, &mt)) {
            ui->mask_target = mt;
        }
        if ((v = pc_json_get(p, "vectorMaskTarget")) != NULL && pc_json_as_bool(v, &mt)) {
            ui->vector_mask_target = mt;
        }
        if ((v = pc_json_get(p, "panels")) != NULL && yyjson_is_obj(v)) {
            struct {
                const char *k;
                bool *slot;
            } m[] = {{"layers", &ui->panels.layers},     {"history", &ui->panels.history},
                     {"properties", &ui->panels.properties}, {"color", &ui->panels.color},
                     {"navigator", &ui->panels.navigator}, {"character", &ui->panels.character},
                     {"toolbar", &ui->panels.toolbar}};
            for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
                yyjson_val *pv = pc_json_get(v, m[i].k);
                bool b;
                if (pv != NULL && pc_json_as_bool(pv, &b)) {
                    *m[i].slot = b;
                }
            }
        }
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
    return ok_empty(out, err);
}

/* ---- ui.pointer (validated; the gesture wiring to tools lands with U4/U6/U7) ---- */

static pc_status ui_pointer(yyjson_val *p, pc_json_value *out, pc_error *err)
{
    static const char *kinds[] = {"down", "move", "up"};
    size_t queued = 0;
    yyjson_val *events = p ? pc_json_get(p, "events") : NULL;
    if (events == NULL || !yyjson_is_arr(events)) {
        return pc_fail(err, PC_ERR_INVALID, "`events` must be an array");
    }
    size_t idx, max;
    yyjson_val *ev;
    yyjson_arr_foreach(events, idx, max, ev) {
        if (!yyjson_is_obj(ev)) {
            return pc_fail(err, PC_ERR_INVALID, "each event must be an object");
        }
        /* x and y are required numbers. */
        double x, y;
        if (!pc_json_as_f64(pc_json_get(ev, "x"), &x) || !pc_json_as_f64(pc_json_get(ev, "y"), &y)) {
            return pc_fail(err, PC_ERR_INVALID, "event needs numeric `x` and `y`");
        }
        yyjson_val *kv = pc_json_get(ev, "kind");
        if (kv != NULL) {
            const char *kind = pc_json_as_str(kv, NULL);
            bool ok = false;
            for (size_t i = 0; i < 3; i++) {
                if (kind != NULL && strcmp(kind, kinds[i]) == 0) {
                    ok = true;
                }
            }
            if (!ok) {
                return pc_fail(err, PC_ERR_INVALID, "unknown event kind");
            }
        }
        double pressure;
        yyjson_val *pv = pc_json_get(ev, "pressure");
        if (pv != NULL && (!pc_json_as_f64(pv, &pressure) || pressure < 0.0 || pressure > 1.0)) {
            return pc_fail(err, PC_ERR_INVALID, "pressure must be 0..1");
        }
        queued++;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "queued", queued);
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

/* ---- dispatch ---- */

pc_status pc_ui_control_handle(pc_ui_shell_app *a, const char *method, yyjson_val *params,
                               pc_json_value *out, pc_error *err)
{
    if (a == NULL || method == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing method");
    }
    if (strcmp(method, "ui.inspect") == 0) {
        return pc_ui_control_inspect(a, out, err);
    }
    if (strcmp(method, "ui.menu.list") == 0) {
        return menu_list(a, out, err);
    }
    if (strcmp(method, "ui.set") == 0) {
        return ui_set(a, params, out, err);
    }
    if (strcmp(method, "ui.pointer") == 0) {
        return ui_pointer(params, out, err);
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
        /* engine.execute runs engine commands directly; ui.menu.invoke follows the menu routing. */
        if (strcmp(method, "engine.execute") == 0 && pc_engine_command_find(id) != NULL) {
            return pc_engine_session_execute(pc_ui_shell_app_session(a), id, cmd_params, out, err);
        }
        return pc_ui_shell_app_invoke(a, id, cmd_params, out, err);
    }
    if (strcmp(method, "engine.commands") == 0) {
        return pc_engine_session_execute(pc_ui_shell_app_session(a), "command.list", NULL, out, err);
    }
    return pc_fail(err, PC_ERR_NOT_FOUND, "method `%s` is not supported in this build yet", method);
}
