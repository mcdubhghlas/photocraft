/* Shell wiring for the four warp editors. See warp_ui.h. */
#include "warp_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "jobs_ui.h"
#include "canvas_ink.h"
#include "base/buf.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "doc/doc.h"
#include "raster/surface.h"
#include "ui/i18n.h"
#include "ui/widgets.h"

/* ------------------------------------------------------------------ small helpers */

static const pc_document *active_doc(pc_ui_shell_app *app)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    return state ? pc_doc_arc_get(state->doc) : NULL;
}

/* The active layer (pixel/content layer) and its id, if any. */
static const pc_layer *active_layer(pc_ui_shell_app *app, pc_layer_id *id)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state || !state->has_active_layer)
        return NULL;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (!doc)
        return NULL;
    if (id)
        *id = state->active_layer;
    return pc_document_layer(doc, state->active_layer);
}

static bool empty_params(yyjson_val *p)
{
    return !p || (yyjson_is_obj(p) && yyjson_obj_size(p) == 0);
}

/* Clamp a screen coordinate into a safe range well inside the Nuklear 16-bit draw space. */
static float clip16(float v)
{
    if (!(v > -16000.0f))
        return -16000.0f;
    if (!(v < 16000.0f))
        return 16000.0f;
    return v;
}

/* A black/white two-pass line so overlays read on any background (document coords in). */
static void doc_line(struct nk_command_buffer *cv, const pc_ui_canvas_xform *xf, double x0, double y0,
                     double x1, double y1)
{
    float a[2], b[2];
    if (pc_ui_canvas_to_screen(xf, (float)x0, (float)y0, a) != PC_OK ||
        pc_ui_canvas_to_screen(xf, (float)x1, (float)y1, b) != PC_OK)
        return;
    for (int pass = 0; pass < 2; pass++) {
        float w = pass ? 1.0f : 3.0f;
        struct nk_color c = pass ? nk_rgb(255, 255, 255) : nk_rgba(0, 0, 0, 160);
        pc_ui_canvas_ink_line(cv, xf, clip16(a[0]), clip16(a[1]), clip16(b[0]), clip16(b[1]), w, c);
    }
}

/* A square handle centred on a document point. */
static void doc_handle(struct nk_command_buffer *cv, const pc_ui_canvas_xform *xf, double x, double y,
                       struct nk_color fill)
{
    float p[2];
    if (pc_ui_canvas_to_screen(xf, (float)x, (float)y, p) != PC_OK || !pc_ui_canvas_ink_visible(xf, p[0], p[1], 8))
        return;
    struct nk_rect box = nk_rect(clip16(p[0] - 4), clip16(p[1] - 4), 8, 8);
    nk_fill_rect(cv, box, 1, fill);
    nk_stroke_rect(cv, box, 1, 1.0f, nk_rgba(0, 0, 0, 200));
}

static double view_zoom(pc_ui_shell_app *app)
{
    pc_ui_canvas_xform xf;
    if (pc_ui_shell_app_canvas_mapping(app, &xf) && xf.zoom > 0)
        return (double)xf.zoom;
    return 1.0;
}

/* ------------------------------------------------------------------ lifecycle */

/* Longest side of the Liquify preview proxy, in proxy pixels (matches the reference's PROXY_SIDE). */
#define PC_UI_LIQUIFY_PROXY_SIDE 1600

static void liq_preview_free(pc_ui_liquify_preview *p)
{
    if (!p)
        return;
    pc_algo_liquify_proxy_fini(&p->proxy);
    free(p->out);
    memset(p, 0, sizeof *p);
}

void pc_ui_shell_warp_fini(pc_ui_warp_runtime *r)
{
    if (!r)
        return;
    pc_ui_persp_fini(&r->persp);
    pc_ui_puppet_fini(&r->puppet);
    pc_ui_liquify_fini(&r->liquify);
    pc_ui_wide_fini(&r->wide);
    liq_preview_free(&r->liq_preview);
    memset(r, 0, sizeof *r);
}

static void warp_stop(pc_ui_shell_app *app)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    pc_ui_shell_warp_fini(r);
    pc_ui_shell_app_changed(app);
}

bool pc_ui_shell_warp_active(pc_ui_shell_app *app)
{
    return pc_ui_shell_warp_runtime(app)->kind != PC_UI_WARP_NONE;
}

/* Cancel the editor if the active document changed under it (never operate on a stale target). */
static void warp_sync(pc_ui_shell_app *app)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    if (r->kind == PC_UI_WARP_NONE)
        return;
    const pc_document *doc = active_doc(app);
    if (!doc || doc->id != r->document)
        pc_ui_shell_warp_fini(r);
}

/* ------------------------------------------------------------------ begin */

static pc_status begin_persp(pc_ui_shell_app *app, const char *command, pc_error *err)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    const pc_document *doc = active_doc(app);
    if (!doc)
        return pc_fail(err, PC_ERR_INVALID, "open a document first");
    pc_layer_id id = 0;
    const pc_layer *l = active_layer(app, &id);
    pc_rect box = pc_document_bounds(doc);
    if (l) {
        const pc_surface *surf = pc_layer_surface(l);
        if (surf) {
            pc_rect cb = pc_surface_content_bounds(surf);
            if (!pc_rect_is_empty(cb))
                box = cb;
        }
    }
    double bounds[4] = {(double)box.x0, (double)box.y0, (double)box.x1, (double)box.y1};
    pc_ui_shell_warp_fini(r);
    pc_status rc = pc_ui_persp_begin(&r->persp, doc->id, id, command, bounds, err);
    if (rc != PC_OK)
        return rc;
    r->kind = PC_UI_WARP_PERSP;
    r->document = doc->id;
    return PC_OK;
}

static pc_status begin_puppet(pc_ui_shell_app *app, const char *command, pc_error *err)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    const pc_document *doc = active_doc(app);
    if (!doc)
        return pc_fail(err, PC_ERR_INVALID, "open a document first");
    pc_layer_id id = 0;
    const pc_layer *l = active_layer(app, &id);
    const pc_surface *surf = l ? pc_layer_surface(l) : NULL;
    if (!surf)
        return pc_fail(err, PC_ERR_INVALID, "Puppet Warp needs a pixel layer");
    pc_rect cb = pc_surface_content_bounds(surf);
    double bounds[4] = {(double)cb.x0, (double)cb.y0, (double)cb.x1, (double)cb.y1};
    pc_ui_shell_warp_fini(r);
    pc_status rc = pc_ui_puppet_begin(&r->puppet, doc->id, id, command, bounds, surf, err);
    if (rc != PC_OK)
        return rc;
    r->kind = PC_UI_WARP_PUPPET;
    r->document = doc->id;
    return PC_OK;
}

static pc_status begin_liquify(pc_ui_shell_app *app, pc_error *err)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    const pc_document *doc = active_doc(app);
    if (!doc)
        return pc_fail(err, PC_ERR_INVALID, "open a document first");
    pc_layer_id id = 0;
    (void)active_layer(app, &id);
    pc_rect canvas = pc_document_bounds(doc);
    pc_ui_shell_warp_fini(r);
    pc_status rc = pc_ui_liquify_begin(&r->liquify, id, canvas, NULL, err);
    if (rc != PC_OK)
        return rc;
    r->kind = PC_UI_WARP_LIQUIFY;
    r->document = doc->id;
    return PC_OK;
}

static pc_status begin_wide(pc_ui_shell_app *app, pc_error *err)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    const pc_document *doc = active_doc(app);
    if (!doc)
        return pc_fail(err, PC_ERR_INVALID, "open a document first");
    pc_layer_id id = 0;
    (void)active_layer(app, &id);
    pc_rect frame = pc_document_bounds(doc);
    pc_ui_shell_warp_fini(r);
    pc_status rc = pc_ui_wide_begin(&r->wide, doc->id, id, frame, err);
    if (rc != PC_OK)
        return rc;
    r->kind = PC_UI_WARP_WIDE;
    r->document = doc->id;
    return PC_OK;
}

/* ------------------------------------------------------------------ commit / cancel */

static const char *commit_command(const pc_ui_warp_runtime *r)
{
    switch (r->kind) {
    case PC_UI_WARP_PERSP:
        return r->persp.command;
    case PC_UI_WARP_PUPPET:
        return r->puppet.command;
    case PC_UI_WARP_LIQUIFY:
        return "filter.liquify";
    case PC_UI_WARP_WIDE:
        return "filter.adaptiveWideAngle";
    case PC_UI_WARP_NONE:
    default:
        return NULL;
    }
}

static pc_status build_commit_params(pc_ui_warp_runtime *r, yyjson_mut_doc *d, yyjson_mut_val **out,
                                     bool *run, pc_error *err)
{
    *out = NULL;
    *run = true;
    switch (r->kind) {
    case PC_UI_WARP_PERSP:
        return pc_ui_persp_commit_params(&r->persp, d, out, run, err);
    case PC_UI_WARP_PUPPET:
        return pc_ui_puppet_commit_params(&r->puppet, d, out, run, err);
    case PC_UI_WARP_LIQUIFY:
        return pc_ui_liquify_commit_params(&r->liquify, d, out, run, err);
    case PC_UI_WARP_WIDE:
        *run = true;
        return pc_ui_wide_commit_params(&r->wide, d, out, err);
    case PC_UI_WARP_NONE:
    default:
        return pc_fail(err, PC_ERR_INVALID, "no warp editor");
    }
}

static pc_status warp_commit(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    if (r->kind == PC_UI_WARP_NONE)
        return pc_fail(err, PC_ERR_INVALID, "no warp editor");
    const char *command = commit_command(r);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    yyjson_mut_val *params = NULL;
    bool run = true;
    pc_status rc = build_commit_params(r, d, &params, &run, err);
    if (rc != PC_OK) {
        yyjson_mut_doc_free(d);
        return rc;
    }
    if (!run || !params || !command) {
        /* nothing to apply: just close the editor */
        yyjson_mut_doc_free(d);
        warp_stop(app);
        return PC_OK;
    }
    pc_buf json = PC_BUF_INIT;
    rc = pc_json_write(params, 0, &json, err);
    if (rc == PC_OK)
        rc = pc_engine_session_execute_json(pc_ui_shell_app_session(app), command, pc_buf_cstr(&json), out, err);
    pc_buf_fini(&json);
    yyjson_mut_doc_free(d);
    if (rc == PC_OK)
        warp_stop(app);
    return rc;
}

/* ------------------------------------------------------------------ command front end */

static bool is_persp(const char *id)
{
    return !strcmp(id, "edit.perspectiveWarp") || !strcmp(id, "layer.smartObjects.perspectiveWarp");
}
static bool is_puppet(const char *id)
{
    return !strcmp(id, "edit.puppetWarp") || !strcmp(id, "layer.smartObjects.puppetWarp");
}

pc_status pc_ui_shell_warp_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                   pc_json_value *out, pc_error *err)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    warp_sync(app);

    /* Control-channel / UI verbs. */
    if (!strcmp(id, "ui.warp.commit")) {
        if (r->kind == PC_UI_WARP_NONE)
            return pc_fail(err, PC_ERR_INVALID, "no warp editor to commit");
        return warp_commit(app, out, err);
    }
    if (!strcmp(id, "ui.warp.cancel")) {
        if (r->kind == PC_UI_WARP_NONE)
            return pc_fail(err, PC_ERR_INVALID, "no warp editor to cancel");
        warp_stop(app);
        return PC_OK;
    }

    /* Session-local undo/redo while an editor owns the canvas. */
    if (r->kind != PC_UI_WARP_NONE && (!strcmp(id, "edit.undo") || !strcmp(id, "edit.redo"))) {
        bool redo = !strcmp(id, "edit.redo");
        bool changed = false;
        if (r->kind == PC_UI_WARP_PUPPET)
            changed = redo ? pc_ui_puppet_redo(&r->puppet) : pc_ui_puppet_undo(&r->puppet);
        else if (r->kind == PC_UI_WARP_WIDE)
            changed = redo ? pc_ui_wide_redo(&r->wide) : pc_ui_wide_undo(&r->wide);
        else if (r->kind == PC_UI_WARP_LIQUIFY)
            changed = (redo ? pc_ui_liquify_redo(&r->liquify, NULL) : pc_ui_liquify_undo(&r->liquify, NULL)) ==
                      PC_OK;
        if (changed)
            pc_ui_shell_app_changed(app);
        return PC_OK; /* consumed even when there is nothing to undo */
    }

    /* Begin on a menu click (no params). A scripted call with params runs the engine command. */
    bool begin = is_persp(id) || is_puppet(id) || !strcmp(id, "filter.liquify") ||
                 !strcmp(id, "filter.adaptiveWideAngle");
    if (begin && empty_params(params)) {
        pc_status rc;
        if (is_persp(id))
            rc = begin_persp(app, id, err);
        else if (is_puppet(id))
            rc = begin_puppet(app, id, err);
        else if (!strcmp(id, "filter.liquify"))
            rc = begin_liquify(app, err);
        else
            rc = begin_wide(app, err);
        if (rc == PC_OK)
            pc_ui_shell_app_changed(app);
        return rc;
    }
    return pc_fail(err, PC_ERR_NOT_FOUND, "not a warp command");
}

/* ------------------------------------------------------------------ pointer */

pc_status pc_ui_shell_warp_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                   unsigned modifiers, pc_json_value *out, pc_error *err)
{
    (void)out;
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    if (r->kind == PC_UI_WARP_NONE)
        return PC_ERR_NOT_FOUND;
    warp_sync(app);
    if (r->kind == PC_UI_WARP_NONE)
        return PC_ERR_NOT_FOUND;

    double zoom = view_zoom(app);
    bool alt = (modifiers & PC_UI_MOD_ALT) != 0;
    bool shift = (modifiers & PC_UI_MOD_SHIFT) != 0;
    double x = point.x, y = point.y;

    if (isfinite(x) && isfinite(y)) {
        r->cursor[0] = x;
        r->cursor[1] = y;
        r->has_cursor = true;
    }
    if (kind == PC_UI_POINTER_DOWN)
        r->dragging = true;
    else if (kind == PC_UI_POINTER_UP || kind == PC_UI_POINTER_CANCEL)
        r->dragging = false;

    pc_status rc;
    switch (r->kind) {
    case PC_UI_WARP_PERSP:
        rc = pc_ui_persp_pointer(&r->persp, kind, x, y, 8.0 / fmax(0.01, zoom), err);
        break;
    case PC_UI_WARP_PUPPET:
        rc = pc_ui_puppet_pointer(&r->puppet, kind, x, y, 12.0 / fmax(0.01, zoom), alt, err);
        break;
    case PC_UI_WARP_LIQUIFY: {
        double pressure = isfinite(point.pressure) ? point.pressure : 1.0;
        rc = pc_ui_liquify_pointer(&r->liquify, kind, x, y, pressure, alt, err);
        break;
    }
    case PC_UI_WARP_WIDE:
        rc = pc_ui_wide_pointer(&r->wide, kind, x, y, zoom, shift, err);
        break;
    case PC_UI_WARP_NONE:
    default:
        return PC_ERR_NOT_FOUND;
    }
    if (rc == PC_OK)
        pc_ui_shell_app_changed(app);
    return rc; /* consumed (PC_OK or a reportable error), never PC_ERR_NOT_FOUND */
}

/* ------------------------------------------------------------------ canvas input */

bool pc_ui_shell_warp_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                             float factor)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    if (r->kind == PC_UI_WARP_NONE || pc_ui_shell_jobs_locked(app))
        return false;
    bool moving = event->type == SDL_EVENT_MOUSE_MOTION, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!moving && !down && event->type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    if (!moving && event->button.button != SDL_BUTTON_LEFT)
        return false;
    float x = (moving ? event->motion.x : event->button.x) * factor,
          y = (moving ? event->motion.y : event->button.y) * factor;
    if (!r->dragging && (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float doc[2];
    if (pc_ui_canvas_to_doc(xf, x, y, doc) != PC_OK)
        return r->dragging;
    unsigned mods = pc_ui_shell_input_modifiers(app);
    pc_json_value res = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_ui_shell_app_pointer(app,
                                           down     ? PC_UI_POINTER_DOWN
                                           : moving ? PC_UI_POINTER_MOVE
                                                    : PC_UI_POINTER_UP,
                                           (pc_ui_tool_point){.x = doc[0], .y = doc[1], .pressure = 1}, mods,
                                           &res, &e);
    pc_json_value_fini(&res);
    if (rc != PC_OK && rc != PC_ERR_NOT_FOUND)
        pc_ui_shell_report(app, "Warp", e.msg, true);
    return rc != PC_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ keys */

bool pc_ui_shell_warp_key(pc_ui_shell_app *app, const SDL_Event *event)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    if (r->kind == PC_UI_WARP_NONE || event->type != SDL_EVENT_KEY_DOWN || event->key.repeat)
        return false;
    bool commit = event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER;
    bool cancel = event->key.key == SDLK_ESCAPE;
    if (!commit && !cancel)
        return false;
    if (commit && pc_ui_shell_keyboard_captured(app))
        return false; /* let a focused field take Enter */
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = PC_OK;
    if (commit)
        rc = warp_commit(app, &out, &err);
    else
        warp_stop(app);
    pc_json_value_fini(&out);
    if (rc != PC_OK)
        pc_ui_shell_report(app, "Warp", err.msg, true);
    return true;
}

/* ------------------------------------------------------------------ options bars */

static void commit_cancel_buttons(pc_ui_shell_app *app, struct nk_context *ctx)
{
    nk_layout_row_push(ctx, 70);
    if (nk_button_label(ctx, pc_i18n_t("Cancel"))) {
        pc_ui_shell_queue(app, "ui.warp.cancel", NULL);
    }
    nk_layout_row_push(ctx, 70);
    if (nk_button_label(ctx, pc_i18n_t("OK"))) {
        pc_ui_shell_queue(app, "ui.warp.commit", NULL);
    }
}

static void persp_options(pc_ui_shell_app *app, struct nk_context *ctx)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 10);
    nk_layout_row_push(ctx, 70);
    if (nk_option_label(ctx, pc_i18n_t("Layout"), r->persp.mode == PC_UI_PERSP_LAYOUT))
        (void)pc_ui_persp_set_mode(&r->persp, PC_UI_PERSP_LAYOUT, NULL);
    nk_layout_row_push(ctx, 70);
    if (nk_option_label(ctx, pc_i18n_t("Warp"), r->persp.mode == PC_UI_PERSP_WARP))
        (void)pc_ui_persp_set_mode(&r->persp, PC_UI_PERSP_WARP, NULL);
    if (r->persp.mode == PC_UI_PERSP_WARP) {
        nk_layout_row_push(ctx, 40);
        if (nk_button_label(ctx, "H"))
            (void)pc_ui_persp_straighten(&r->persp, PC_ALGO_PERSP_HORIZONTAL, NULL);
        nk_layout_row_push(ctx, 40);
        if (nk_button_label(ctx, "V"))
            (void)pc_ui_persp_straighten(&r->persp, PC_ALGO_PERSP_VERTICAL, NULL);
        nk_layout_row_push(ctx, 50);
        if (nk_button_label(ctx, pc_i18n_t("Auto")))
            (void)pc_ui_persp_straighten(&r->persp, PC_ALGO_PERSP_AUTO, NULL);
    }
    nk_layout_row_push(ctx, 110);
    if (nk_button_label(ctx, pc_i18n_t("Remove Planes")))
        pc_ui_persp_clear(&r->persp);
    commit_cancel_buttons(app, ctx);
    nk_layout_row_end(ctx);
}

static void puppet_options(pc_ui_shell_app *app, struct nk_context *ctx)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    static const char *const modes[3] = {"Rigid", "Normal", "Distort"};
    static const char *const dens[3] = {"Fewer Points", "Normal", "More Points"};
    nk_layout_row_begin(ctx, NK_STATIC, 24, 14);
    nk_layout_row_push(ctx, 120);
    int mode = (int)r->puppet.warp.mode;
    int nm = nk_combo(ctx, modes, 3, mode, 22, nk_vec2(160, 120));
    if (nm >= 0 && nm != mode)
        (void)pc_ui_puppet_set_mode(&r->puppet, (pc_algo_puppet_mode)nm, NULL);
    nk_layout_row_push(ctx, 130);
    int d = (int)r->puppet.warp.density;
    int nd = nk_combo(ctx, dens, 3, d, 22, nk_vec2(160, 120));
    if (nd >= 0 && nd != d)
        (void)pc_ui_puppet_set_density(&r->puppet, (pc_algo_puppet_density)nd, NULL);
    nk_layout_row_push(ctx, 150);
    double exp = r->puppet.warp.expansion;
    nk_property_double(ctx, pc_i18n_t("Expansion"), -200, &exp, 200, 1, 0.5f);
    if (exp != r->puppet.warp.expansion)
        (void)pc_ui_puppet_set_expansion(&r->puppet, exp, NULL);
    nk_layout_row_push(ctx, 110);
    nk_bool mesh = r->puppet.show_mesh;
    if (nk_checkbox_label(ctx, pc_i18n_t("Show Mesh"), &mesh))
        pc_ui_puppet_set_show_mesh(&r->puppet, mesh != 0);
    if (r->puppet.has_selected) {
        nk_layout_row_push(ctx, 40);
        if (nk_button_label(ctx, "-"))
            (void)pc_ui_puppet_nudge_depth(&r->puppet, -1, NULL);
        nk_layout_row_push(ctx, 40);
        if (nk_button_label(ctx, "+"))
            (void)pc_ui_puppet_nudge_depth(&r->puppet, 1, NULL);
    }
    nk_layout_row_push(ctx, 110);
    if (nk_button_label(ctx, pc_i18n_t("Remove All Pins")))
        (void)pc_ui_puppet_remove_all(&r->puppet, NULL);
    commit_cancel_buttons(app, ctx);
    nk_layout_row_end(ctx);
}

static void liquify_options(pc_ui_shell_app *app, struct nk_context *ctx)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    pc_ui_liquify_opts *o = &r->liquify.opts;
    const char *tools[11];
    int cur = 0;
    for (int i = 0; i < 11; i++) {
        tools[i] = pc_algo_liquify_tool_label(pc_algo_liquify_tools_all[i]);
        if (pc_algo_liquify_tools_all[i] == o->tool)
            cur = i;
    }
    nk_layout_row_begin(ctx, NK_STATIC, 24, 12);
    nk_layout_row_push(ctx, 150);
    int sel = nk_combo(ctx, tools, 11, cur, 22, nk_vec2(220, 300));
    if (sel >= 0 && sel < 11)
        o->tool = pc_algo_liquify_tools_all[sel];
    nk_layout_row_push(ctx, 125);
    nk_property_double(ctx, pc_i18n_t("Size"), 1, &o->size, 15000, 1, 1.0f);
    nk_layout_row_push(ctx, 125);
    nk_property_double(ctx, pc_i18n_t("Density"), 0, &o->density, 100, 1, 0.5f);
    nk_layout_row_push(ctx, 125);
    nk_property_double(ctx, pc_i18n_t("Pressure"), 0, &o->pressure, 100, 1, 0.5f);
    nk_layout_row_push(ctx, 95);
    nk_bool mesh = o->show_mesh;
    if (nk_checkbox_label(ctx, pc_i18n_t("Show Mesh"), &mesh))
        o->show_mesh = mesh != 0;
    nk_layout_row_push(ctx, 95);
    nk_bool mask = o->show_mask;
    if (nk_checkbox_label(ctx, pc_i18n_t("Show Mask"), &mask))
        o->show_mask = mask != 0;
    /* Mask Options: Mask All / None / Invert All (whole-field freeze operations). */
    nk_layout_row_push(ctx, 78);
    if (nk_button_label(ctx, pc_i18n_t("Mask All")))
        (void)pc_ui_liquify_global(&r->liquify, PC_ALGO_LIQUIFY_FREEZE_ALL, false, 0.0, NULL);
    nk_layout_row_push(ctx, 58);
    if (nk_button_label(ctx, pc_i18n_t("None")))
        (void)pc_ui_liquify_global(&r->liquify, PC_ALGO_LIQUIFY_THAW_ALL, false, 0.0, NULL);
    nk_layout_row_push(ctx, 78);
    if (nk_button_label(ctx, pc_i18n_t("Invert All")))
        (void)pc_ui_liquify_global(&r->liquify, PC_ALGO_LIQUIFY_INVERT_FREEZE, false, 0.0, NULL);
    nk_layout_row_push(ctx, 95);
    if (nk_button_label(ctx, pc_i18n_t("Restore All")))
        (void)pc_ui_liquify_restore_all(&r->liquify, NULL);
    commit_cancel_buttons(app, ctx);
    nk_layout_row_end(ctx);
}

static void wide_options(pc_ui_shell_app *app, struct nk_context *ctx)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    static const char *const models[4] = {"Auto", "Fisheye", "Perspective", "Full Spherical"};
    nk_layout_row_begin(ctx, NK_STATIC, 24, 10);
    nk_layout_row_push(ctx, 140);
    int m = (int)r->wide.params.model;
    int nm = nk_combo(ctx, models, 4, m, 22, nk_vec2(180, 140));
    if (nm >= 0 && nm != m)
        r->wide.params.model = (pc_algo_wide_model)nm;
    nk_layout_row_push(ctx, 160);
    nk_property_double(ctx, pc_i18n_t("Scale"), 1, &r->wide.params.scale, 1000, 1, 0.5f);
    nk_layout_row_push(ctx, 160);
    nk_property_double(ctx, pc_i18n_t("Focal Length"), 0, &r->wide.params.focal_length, 2000, 1, 0.5f);
    nk_layout_row_push(ctx, 110);
    nk_bool preview = r->wide.preview;
    if (nk_checkbox_label(ctx, pc_i18n_t("Preview"), &preview))
        pc_ui_wide_set_preview(&r->wide, preview != 0);
    commit_cancel_buttons(app, ctx);
    nk_layout_row_end(ctx);
}

bool pc_ui_shell_warp_options(pc_ui_shell_app *app)
{
    warp_sync(app);
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    if (r->kind == PC_UI_WARP_NONE)
        return false;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    switch (r->kind) {
    case PC_UI_WARP_PERSP:
        persp_options(app, ctx);
        break;
    case PC_UI_WARP_PUPPET:
        puppet_options(app, ctx);
        break;
    case PC_UI_WARP_LIQUIFY:
        liquify_options(app, ctx);
        break;
    case PC_UI_WARP_WIDE:
        wide_options(app, ctx);
        break;
    case PC_UI_WARP_NONE:
    default:
        break;
    }
    return true;
}

/* ------------------------------------------------------------------ overlays */

static void draw_persp(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, struct nk_command_buffer *cv)
{
    pc_ui_persp_session *s = &pc_ui_shell_warp_runtime(app)->persp;
    bool warp = s->mode == PC_UI_PERSP_WARP;
    /* Preview grid (destination mesh) in Warp mode: draw a subsampled lattice. */
    if (warp && s->nplanes > 0) {
        const int N = PC_UI_PERSP_GRID_N;
        for (int j = 0; j < N; j += 4) {
            for (int i = 0; i + 1 < N; i++) {
                size_t a = (size_t)j * (size_t)N + (size_t)i, b = a + 1;
                doc_line(cv, xf, s->grid[a][0], s->grid[a][1], s->grid[b][0], s->grid[b][1]);
            }
        }
        for (int i = 0; i < N; i += 4) {
            for (int j = 0; j + 1 < N; j++) {
                size_t a = (size_t)j * (size_t)N + (size_t)i, b = a + (size_t)N;
                doc_line(cv, xf, s->grid[a][0], s->grid[a][1], s->grid[b][0], s->grid[b][1]);
            }
        }
    }
    /* Plane quads + corner handles. */
    struct nk_color hcol = pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent);
    for (size_t p = 0; p < s->nplanes; p++) {
        const double(*q)[2] = (const double(*)[2])(warp ? s->planes[p].dst : s->planes[p].src);
        for (int k = 0; k < 4; k++) {
            int n = (k + 1) & 3;
            doc_line(cv, xf, q[k][0], q[k][1], q[n][0], q[n][1]);
        }
        for (int k = 0; k < 4; k++)
            doc_handle(cv, xf, q[k][0], q[k][1], hcol);
    }
    /* New-plane rubber band. */
    if (s->drag == PC_UI_PERSP_DRAG_NEWPLANE) {
        double x0 = s->new_start[0], y0 = s->new_start[1], x1 = s->new_cur[0], y1 = s->new_cur[1];
        doc_line(cv, xf, x0, y0, x1, y0);
        doc_line(cv, xf, x1, y0, x1, y1);
        doc_line(cv, xf, x1, y1, x0, y1);
        doc_line(cv, xf, x0, y1, x0, y0);
    }
}

static void draw_puppet(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, struct nk_command_buffer *cv)
{
    pc_ui_puppet_session *s = &pc_ui_shell_warp_runtime(app)->puppet;
    if (s->show_mesh && s->solver && s->deformed) {
        const pc_algo_puppet_mesh *m = pc_algo_puppet_solver_mesh(s->solver);
        if (m->ntris <= 200000) {
            for (size_t t = 0; t < m->ntris; t++) {
                size_t i0 = m->tris[t][0], i1 = m->tris[t][1], i2 = m->tris[t][2];
                if (i0 >= s->ndeformed || i1 >= s->ndeformed || i2 >= s->ndeformed)
                    continue;
                doc_line(cv, xf, s->deformed[i0][0], s->deformed[i0][1], s->deformed[i1][0],
                         s->deformed[i1][1]);
                doc_line(cv, xf, s->deformed[i1][0], s->deformed[i1][1], s->deformed[i2][0],
                         s->deformed[i2][1]);
                doc_line(cv, xf, s->deformed[i2][0], s->deformed[i2][1], s->deformed[i0][0],
                         s->deformed[i0][1]);
            }
        }
    }
    /* Pins. */
    for (size_t k = 0; k < s->warp.npins; k++) {
        float p[2];
        if (pc_ui_canvas_to_screen(xf, (float)s->warp.pins[k].dst[0], (float)s->warp.pins[k].dst[1], p) !=
                PC_OK ||
            !pc_ui_canvas_ink_visible(xf, p[0], p[1], 10))
            continue;
        bool selected = s->has_selected && s->selected == k;
        float rad = selected ? 6.0f : 4.0f;
        struct nk_color fill = selected ? nk_rgb(255, 220, 40) : nk_rgb(255, 255, 255);
        nk_fill_circle(cv, nk_rect(clip16(p[0] - rad), clip16(p[1] - rad), rad * 2, rad * 2), fill);
        nk_stroke_circle(cv, nk_rect(clip16(p[0] - rad), clip16(p[1] - rad), rad * 2, rad * 2), 1.5f,
                         nk_rgba(0, 0, 0, 220));
    }
}

static void draw_liquify(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, struct nk_command_buffer *cv)
{
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    pc_ui_liquify_dialog *s = &r->liquify;
    /* Frozen regions (red overlay), drawn under the mesh so it reads through. The freeze mask is in
     * source pixel space, so the quads use the undisplaced node positions; subsampled like the mesh
     * to keep the quad count bounded (Rule 9). */
    if (s->opts.show_mask && s->field.w > 1 && s->field.h > 1 && s->field.freeze) {
        size_t step = s->field.w / 48 + 1;
        size_t vstep = s->field.h / 48 + 1;
        for (size_t j = 0; j + vstep < s->field.h; j += vstep) {
            for (size_t i = 0; i + step < s->field.w; i += step) {
                const float *fz = s->field.freeze;
                float f00 = fz[j * s->field.w + i], f10 = fz[j * s->field.w + (i + step)],
                      f01 = fz[(j + vstep) * s->field.w + i],
                      f11 = fz[(j + vstep) * s->field.w + (i + step)];
                float avg = (f00 + f10 + f01 + f11) * 0.25f;
                if (!(avg > 0.5f))
                    continue;
                double c00[2], c10[2], c01[2], c11[2];
                pc_algo_liquify_node_pos(&s->field, i, j, c00);
                pc_algo_liquify_node_pos(&s->field, i + step, j, c10);
                pc_algo_liquify_node_pos(&s->field, i + step, j + vstep, c11);
                pc_algo_liquify_node_pos(&s->field, i, j + vstep, c01);
                float p[8];
                float sc[2];
                bool ok = pc_ui_canvas_to_screen(xf, (float)c00[0], (float)c00[1], sc) == PC_OK;
                p[0] = clip16(sc[0]);
                p[1] = clip16(sc[1]);
                ok = ok && pc_ui_canvas_to_screen(xf, (float)c10[0], (float)c10[1], sc) == PC_OK;
                p[2] = clip16(sc[0]);
                p[3] = clip16(sc[1]);
                ok = ok && pc_ui_canvas_to_screen(xf, (float)c11[0], (float)c11[1], sc) == PC_OK;
                p[4] = clip16(sc[0]);
                p[5] = clip16(sc[1]);
                ok = ok && pc_ui_canvas_to_screen(xf, (float)c01[0], (float)c01[1], sc) == PC_OK;
                p[6] = clip16(sc[0]);
                p[7] = clip16(sc[1]);
                if (ok)
                    nk_fill_polygon(cv, p, 4, nk_rgba(170, 30, 30, 110));
            }
        }
    }
    /* Field mesh preview (displaced lattice). */
    if (s->opts.show_mesh && s->field.w > 1 && s->field.h > 1 && s->field.d) {
        size_t step = s->field.w / 48 + 1; /* keep the line count bounded */
        size_t vstep = s->field.h / 48 + 1;
        for (size_t j = 0; j < s->field.h; j += vstep) {
            for (size_t i = 0; i + step < s->field.w; i += step) {
                double a[2], b[2];
                pc_algo_liquify_node_pos(&s->field, i, j, a);
                pc_algo_liquify_node_pos(&s->field, i + step, j, b);
                size_t ka = j * s->field.w + i, kb = j * s->field.w + (i + step);
                doc_line(cv, xf, a[0] + s->field.d[ka][0], a[1] + s->field.d[ka][1],
                         b[0] + s->field.d[kb][0], b[1] + s->field.d[kb][1]);
            }
        }
        for (size_t i = 0; i < s->field.w; i += step) {
            for (size_t j = 0; j + vstep < s->field.h; j += vstep) {
                double a[2], b[2];
                pc_algo_liquify_node_pos(&s->field, i, j, a);
                pc_algo_liquify_node_pos(&s->field, i, j + vstep, b);
                size_t ka = j * s->field.w + i, kb = (j + vstep) * s->field.w + i;
                doc_line(cv, xf, a[0] + s->field.d[ka][0], a[1] + s->field.d[ka][1],
                         b[0] + s->field.d[kb][0], b[1] + s->field.d[kb][1]);
            }
        }
    }
    /* Brush cursor. */
    if (r->has_cursor) {
        float p[2];
        if (pc_ui_canvas_to_screen(xf, (float)r->cursor[0], (float)r->cursor[1], p) == PC_OK) {
            float rad = clip16((float)(s->opts.size * 0.5 * (double)xf->zoom));
            if (rad < 1.0f)
                rad = 1.0f;
            nk_stroke_circle(cv, nk_rect(clip16(p[0] - rad), clip16(p[1] - rad), rad * 2, rad * 2), 1.5f,
                             nk_rgb(255, 255, 255));
            nk_stroke_circle(cv, nk_rect(clip16(p[0] - rad - 1), clip16(p[1] - rad - 1), rad * 2 + 2,
                                         rad * 2 + 2),
                             1.0f, nk_rgba(0, 0, 0, 160));
        }
    }
}

static void draw_wide(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, struct nk_command_buffer *cv)
{
    pc_ui_wide_dialog *s = &pc_ui_shell_warp_runtime(app)->wide;
    pc_algo_wide_camera cam;
    pc_algo_wide_camera_init(&cam, &s->params, s->frame);
    for (size_t i = 0; i < s->params.n_constraints; i++) {
        const pc_algo_wide_constraint *c = &s->params.constraints[i];
        double(*arc)[2] = NULL;
        size_t count = 0;
        if (pc_algo_wide_camera_arc(&cam, c->a, c->b, 32, &arc, &count, NULL) == PC_OK && arc) {
            for (size_t k = 0; k + 1 < count; k++)
                doc_line(cv, xf, arc[k][0], arc[k][1], arc[k + 1][0], arc[k + 1][1]);
            free(arc);
        } else {
            doc_line(cv, xf, c->a[0], c->a[1], c->b[0], c->b[1]);
        }
        struct nk_color hc = pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent);
        doc_handle(cv, xf, c->a[0], c->a[1], hc);
        doc_handle(cv, xf, c->b[0], c->b[1], hc);
    }
    if (s->has_drag)
        doc_line(cv, xf, s->drag_a[0], s->drag_a[1], s->drag_b[0], s->drag_b[1]);
}

/* ------------------------------------------------------------------ live proxy preview */

static bool rect_eq(pc_rect a, pc_rect b)
{
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

/* Document-space bounding box of the current stroke's points in [from, npoints), plus the previous
 * end, inflated by the brush radius and a field cell so a dab's whole reach is covered. Returns
 * false when there is nothing to bound. */
static bool cur_stroke_dirty_rect(const pc_ui_liquify_dialog *s, size_t from, const double prev[2],
                                  pc_rect *out)
{
    const pc_algo_liquify_stroke *st = &s->cur;
    if (!st->off || st->npoints == 0)
        return false;
    double lo[2] = {prev[0], prev[1]}, hi[2] = {prev[0], prev[1]};
    bool any = isfinite(prev[0]) && isfinite(prev[1]);
    if (from >= st->npoints)
        from = st->npoints - 1;
    for (size_t i = from; i < st->npoints; i++) {
        size_t base = st->off[i];
        double x = st->coords[base], y = st->coords[base + 1];
        if (!isfinite(x) || !isfinite(y))
            continue;
        if (!any) {
            lo[0] = hi[0] = x;
            lo[1] = hi[1] = y;
            any = true;
        } else {
            lo[0] = fmin(lo[0], x);
            lo[1] = fmin(lo[1], y);
            hi[0] = fmax(hi[0], x);
            hi[1] = fmax(hi[1], y);
        }
    }
    if (!any)
        return false;
    double margin = fabs(st->size) * 0.5 + fmax(1.0, s->field.cell) * 2.0 + 1.0;
    if (!isfinite(margin) || margin > 1e7)
        margin = 1e7;
    double x0 = floor(lo[0] - margin), y0 = floor(lo[1] - margin);
    double x1 = ceil(hi[0] + margin), y1 = ceil(hi[1] + margin);
    x0 = pc_clampd(x0, -2e9, 2e9);
    y0 = pc_clampd(y0, -2e9, 2e9);
    x1 = pc_clampd(x1, -2e9, 2e9);
    y1 = pc_clampd(y1, -2e9, 2e9);
    *out = pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1);
    return true;
}

bool pc_ui_shell_warp_live_overlay(pc_ui_shell_app *app, pc_ui_warp_overlay_frame *frame)
{
    if (!frame)
        return false;
    memset(frame, 0, sizeof *frame);
    warp_sync(app);
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    pc_ui_liquify_preview *p = &r->liq_preview;
    if (r->kind != PC_UI_WARP_LIQUIFY) {
        liq_preview_free(p);
        return false;
    }
    pc_ui_liquify_dialog *dlg = &r->liquify;
    const pc_algo_liquify_field *field = &dlg->field;
    if (field->w < 2 || field->h < 2 || !field->d) {
        return false;
    }
    /* No warp yet (identity field): draw nothing, so the crisp canvas composite shows, not a
     * downsampled copy of it. The mesh/mask overlays still convey a pending freeze. */
    if (dlg->nstrokes == 0 && !dlg->has_cur) {
        return false;
    }
    pc_layer_id id = 0;
    const pc_layer *layer = active_layer(app, &id);
    const pc_surface *surf = layer ? pc_layer_surface(layer) : NULL;
    if (!surf) {
        /* No pixel layer under the field: the mesh/mask overlays still show the edit. */
        return false;
    }
    pc_rect bounds = field->bounds;
    pc_error err = {0};

    /* (Re)build the proxy when the target layer or the field geometry changes. */
    bool rebuilt = false;
    if (!p->ready || !p->proxy.px || p->layer != (uint64_t)id || !rect_eq(p->bounds, bounds) ||
        p->fw != field->w || p->fh != field->h) {
        pc_algo_liquify_proxy_fini(&p->proxy);
        if (pc_algo_liquify_proxy_init(&p->proxy, surf, bounds, PC_UI_LIQUIFY_PROXY_SIDE, &err) != PC_OK) {
            p->ready = false;
            return false; /* over budget / OOM: fall back to the mesh overlay */
        }
        size_t need = p->proxy.w * p->proxy.h; /* bounded by proxy_init's PC_MAX_ALLOC check */
        if (need > p->out_cap) {
            uint8_t(*nb)[4] = pc_malloc_array(need, sizeof *nb);
            if (!nb) {
                pc_algo_liquify_proxy_fini(&p->proxy);
                p->ready = false;
                return false;
            }
            free(p->out);
            p->out = nb;
            p->out_cap = need;
        }
        p->ready = true;
        p->layer = (uint64_t)id;
        p->bounds = bounds;
        p->fw = field->w;
        p->fh = field->h;
        p->valid_sig = false;
        rebuilt = true;
    }

    size_t w = p->proxy.w, h = p->proxy.h;
    size_t cur_np = dlg->has_cur ? dlg->cur.npoints : 0;

    /* Decide what changed since the previous frame. */
    bool changed = true, full = true;
    if (!rebuilt && p->valid_sig) {
        bool same = dlg->nstrokes == p->nstrokes && dlg->has_cur == p->has_cur && cur_np == p->cur_np &&
                    dlg->last_point[0] == p->last_point[0] && dlg->last_point[1] == p->last_point[1];
        bool cont = dlg->has_cur && p->has_cur && dlg->nstrokes == p->nstrokes && cur_np >= p->cur_np &&
                    !same;
        if (same) {
            changed = false;
            full = false;
        } else if (cont) {
            full = false; /* a continuing brush stroke: re-render only the touched region */
        }
    }

    size_t dirty[4] = {0, 0, w, h};
    if (changed && !full) {
        pc_rect box;
        double prev[2] = {p->last_point[0], p->last_point[1]};
        size_t from = p->cur_np > 0 ? p->cur_np - 1 : 0;
        if (cur_stroke_dirty_rect(dlg, from, prev, &box)) {
            pc_algo_liquify_proxy_rect(&p->proxy, box, dirty);
        }
        /* else: keep the full-proxy dirty rect the initialiser set, re-rendering everything. */
    }

    if (changed && p->out) {
        if (pc_algo_liquify_proxy_render(&p->proxy, field, dirty, p->out, p->out_cap, &err) != PC_OK) {
            /* A degenerate render: keep the last good pixels, draw nothing new this frame. */
            changed = false;
            dirty[0] = dirty[2] = 0;
            dirty[1] = dirty[3] = 0;
        }
    }

    /* Record the signature for the next frame. */
    p->nstrokes = dlg->nstrokes;
    p->has_cur = dlg->has_cur;
    p->cur_np = cur_np;
    p->last_point[0] = dlg->last_point[0];
    p->last_point[1] = dlg->last_point[1];
    p->valid_sig = true;

    frame->px = (const uint8_t(*)[4])p->out;
    frame->w = w;
    frame->h = h;
    frame->doc_bounds = bounds;
    if (changed) {
        frame->dirty[0] = dirty[0];
        frame->dirty[1] = dirty[1];
        frame->dirty[2] = dirty[2];
        frame->dirty[3] = dirty[3];
    } /* else all-zero: nothing to upload, reuse the texture */
    frame->opacity = 1.0f;
    return p->out != NULL;
}

void pc_ui_shell_warp_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    warp_sync(app);
    pc_ui_warp_runtime *r = pc_ui_shell_warp_runtime(app);
    if (r->kind == PC_UI_WARP_NONE)
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    switch (r->kind) {
    case PC_UI_WARP_PERSP:
        draw_persp(app, xf, cv);
        break;
    case PC_UI_WARP_PUPPET:
        draw_puppet(app, xf, cv);
        break;
    case PC_UI_WARP_LIQUIFY:
        draw_liquify(app, xf, cv);
        break;
    case PC_UI_WARP_WIDE:
        draw_wide(app, xf, cv);
        break;
    case PC_UI_WARP_NONE:
    default:
        break;
    }
}
