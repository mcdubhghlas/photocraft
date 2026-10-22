#include "gradient_ui.h"
#include "shell_internal.h"
#include "canvas_view.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "ui/gradient_edit.h"
#include "ui/gradient_widget.h"
#include "ui/color_picker.h"
#include "color/blend.h"
#include "base/json.h"
#include "engine/preview.h"
#include "engine/channel_display.h"
#include "jobs_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool style_button(pc_ui_shell_app *app, const char *icon, const char *fallback, const char *tip,
                         bool selected)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    nk_layout_row_push(c, 30);
    struct nk_rect r = nk_widget_bounds(c);
    char mapping[48];
    (void)snprintf(mapping, sizeof mapping, "gradient.style.%s", fallback);
    pc_ui_shell_widget_mapping(app, mapping, r);
    struct nk_style_item before = c->style.button.normal;
    if (selected) {
        c->style.button.normal = nk_style_item_color(pc_ui_nk_color(t->accent_soft));
    }
    struct nk_image image;
    bool clicked;
    if (pc_ui_shell_icon_image(app, icon, 18, &image)) {
        clicked = nk_button_label(c, "") != 0;
        struct nk_command_buffer *canvas = nk_window_get_canvas(c);
        struct nk_rect ir = nk_rect(r.x + (r.w - 18) * .5f, r.y + (r.h - 18) * .5f, 18, 18);
        nk_draw_image(canvas, ir, &image, pc_ui_nk_color(t->icon));
    } else {
        clicked = nk_button_label(c, fallback) != 0;
    }
    c->style.button.normal = before;
    if (tip && nk_input_is_mouse_hovering_rect(&c->input, r)) {
        nk_tooltip(c, pc_i18n_t(tip));
    }
    return clicked;
}
static bool current_gradient(pc_ui_shell_app *app, pc_ui_gradient *g);
static void checker(struct nk_command_buffer *c, struct nk_rect r);
/* A gradient thumbnail: a checkerboard (so transparency shows) with the gradient sampled across it. */
static void grad_swatch(struct nk_command_buffer *c, struct nk_rect r, const pc_ui_gradient *g)
{
    if (r.w < 1 || r.h < 1) {
        return;
    }
    checker(c, r);
    int steps = (int)r.w;
    for (int i = 0; i < steps; i++) {
        float ta = (float)i / (float)steps;
        float rgba[4];
        pc_ui_gradient_sample(g, ta, rgba);
        nk_fill_rect(c, nk_rect(r.x + (float)i, r.y, 1.5f, r.h), 0, nk_rgba_f(rgba[0], rgba[1], rgba[2], rgba[3]));
    }
}
void pc_ui_shell_gradient_options(pc_ui_shell_app *app)
{
    struct nk_context *c = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_tool_options *o = &ui->tool_options;
    nk_layout_row_begin(c, NK_STATIC, 24, 24);
    nk_layout_row_push(c, 100);
    nk_label(c, pc_i18n_t(pc_ui_tool_label(PC_TOOL_GRADIENT)), NK_TEXT_LEFT);
    /* Mode: live Gradient Fill layer vs. Classic (paints pixels). */
    nk_layout_row_push(c, 130);
    const char *modes[] = {"Gradient", "Classic gradient"};
    int mode = o->gradient_classic ? 1 : 0;
    pc_ui_shell_widget_mapping(app, "gradient.classic", nk_widget_bounds(c));
    int next_mode = nk_combo(c, modes, 2, mode, 24, nk_vec2(170, 70));
    if (next_mode >= 0 && next_mode < 2) {
        o->gradient_classic = next_mode == 1;
    }
    /* Preset swatch (gradient thumbnail): opens the Gradient Editor. */
    nk_layout_row_push(c, 100);
    struct nk_rect swatch = nk_widget_bounds(c);
    pc_ui_shell_widget_mapping(app, "gradient.editor", swatch);
    if (nk_button_label(c, "")) {
        ui->panels.gradient_editor = !ui->panels.gradient_editor;
    }
    {
        pc_ui_gradient g;
        (void)current_gradient(app, &g);
        struct nk_rect inner = nk_rect(swatch.x + 3, swatch.y + 3, swatch.w - 6, swatch.h - 6);
        grad_swatch(nk_window_get_canvas(c), inner, &g);
        nk_stroke_rect(nk_window_get_canvas(c), inner, 0, 1,
                       pc_ui_nk_color(pc_ui_shell_app_tokens(app)->field_border));
    }
    /* Five style buttons. */
    struct {
        const char *style, *icon, *tip;
    } styles[] = {{"linear", "gradient", "Linear Gradient"},
                  {"radial", "circle", "Radial Gradient"},
                  {"angle", "rotate-cw", "Angle Gradient"},
                  {"reflected", "arrow-left-right", "Reflected Gradient"},
                  {"diamond", "diamond", "Diamond Gradient"}};
    for (size_t i = 0; i < 5; i++) {
        bool selected = pc_str_eq_c(&o->gradient_style, styles[i].style);
        if (style_button(app, styles[i].icon, styles[i].style, styles[i].tip, selected)) {
            (void)pc_str_set_c(&o->gradient_style, styles[i].style, NULL);
        }
    }
    /* Blend mode. */
    nk_layout_row_push(c, 50);
    nk_label(c, pc_i18n_t("Mode"), NK_TEXT_LEFT);
    nk_layout_row_push(c, 130);
    pc_ui_shell_widget_mapping(app, "gradient.blendMode", nk_widget_bounds(c));
    if (nk_combo_begin_label(c, pc_blend_label(o->gradient_blend_mode), nk_vec2(220, 430))) {
        nk_layout_row_dynamic(c, 22, 1);
        for (size_t i = 0; i < PC_BLEND_LAYER_MODE_COUNT; i++) {
            if (nk_combo_item_label(c, pc_blend_label(PC_BLEND_LAYER_MODES[i]), NK_TEXT_LEFT)) {
                o->gradient_blend_mode = PC_BLEND_LAYER_MODES[i];
            }
        }
        nk_combo_end(c);
    }
    /* Opacity. */
    nk_layout_row_push(c, 140);
    nk_property_float(c, pc_i18n_t("Opacity"), 1, &o->fill_opacity, 100, 1, .25f);
    /* Reverse / Dither. */
    nk_layout_row_push(c, 90);
    nk_bool reverse = o->gradient_reverse;
    if (nk_checkbox_label(c, pc_i18n_t("Reverse"), &reverse)) {
        o->gradient_reverse = reverse != 0;
    }
    nk_layout_row_push(c, 80);
    nk_bool dither = o->gradient_dither;
    if (nk_checkbox_label(c, pc_i18n_t("Dither"), &dither)) {
        o->gradient_dither = dither != 0;
    }
    nk_layout_row_end(c);
}
/* Selection is stored by location (not index) so it follows a stop across sort/add/delete. */
static struct {
    int kind; /* 0 none, 1 color, 2 opacity */
    float loc;
} g_sel;

/* Queue a gradient.presets.stop edit with the given action/kind and optional location/opacity/color. */
static void stop_edit(pc_ui_shell_app *app, const char *action, const char *kind, int index, bool has_loc,
                      double loc, bool has_opacity, double opacity, const char *color)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    if (!p) {
        yyjson_mut_doc_free(d);
        return;
    }
    yyjson_mut_obj_add_strcpy(d, p, "action", action);
    if (kind) {
        yyjson_mut_obj_add_strcpy(d, p, "kind", kind);
    }
    if (index >= 0) {
        yyjson_mut_obj_add_int(d, p, "index", index);
    }
    if (has_loc) {
        yyjson_mut_obj_add_real(d, p, "location", loc);
    }
    if (has_opacity) {
        yyjson_mut_obj_add_real(d, p, "opacity", opacity);
    }
    if (color) {
        yyjson_mut_obj_add_strcpy(d, p, "color", color);
    }
    pc_ui_shell_queue(app, "gradient.presets.stop", p);
    yyjson_mut_doc_free(d);
}
/* Read the Gradient tool's current gradient into *g; returns false if unavailable. */
static bool current_gradient(pc_ui_shell_app *app, pc_ui_gradient *g)
{
    pc_engine_session *session = pc_ui_shell_app_session(app);
    const pc_engine_tool_state *tools = pc_engine_session_tools(session);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(session, "gradient.presets.list", "{}", &out, &err);
    bool ok = false;
    if (rc == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&out);
        yyjson_mut_val *current = root ? yyjson_mut_obj_get(root, "current") : NULL;
        ok = pc_ui_gradient_parse(g, current, tools ? tools->foreground : NULL,
                                  tools ? tools->background : NULL);
    }
    pc_json_value_fini(&out);
    if (!ok) {
        pc_ui_gradient_init(g);
    }
    return rc == PC_OK;
}
static void checker(struct nk_command_buffer *c, struct nk_rect r)
{
    int cell = 5;
    for (int y = 0; (float)y < r.h; y += cell) {
        for (int x = 0; (float)x < r.w; x += cell) {
            bool dark = ((x / cell) + (y / cell)) % 2 == 0;
            float w = fminf((float)cell, r.w - (float)x), h = fminf((float)cell, r.h - (float)y);
            nk_fill_rect(c, nk_rect(r.x + (float)x, r.y + (float)y, w, h), 0,
                         dark ? nk_rgb(150, 150, 150) : nk_rgb(200, 200, 200));
        }
    }
}
static void draw_strip(pc_ui_shell_app *app, const pc_ui_gradient *g, int sel_color, int sel_opacity)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    nk_layout_row_dynamic(ctx, 58, 1);
    struct nk_rect outer;
    if (!nk_widget(&outer, ctx)) {
        return;
    }
    pc_ui_shell_widget_mapping(app, "gradient.editor.strip", outer);
    struct nk_command_buffer *c = nk_window_get_canvas(ctx);
    struct nk_rect bar = nk_rect(outer.x + 8, outer.y + 16, outer.w - 16, 24);
    if (bar.w < 1) {
        return;
    }
    checker(c, bar);
    int steps = (int)bar.w;
    for (int i = 0; i < steps; i++) {
        float ta = (float)i / (float)steps;
        float rgba[4];
        pc_ui_gradient_sample(g, ta, rgba);
        struct nk_color col = nk_rgba_f(rgba[0], rgba[1], rgba[2], rgba[3]);
        nk_fill_rect(c, nk_rect(bar.x + (float)i, bar.y, 1.5f, bar.h), 0, col);
    }
    nk_stroke_rect(c, bar, 0, 1, pc_ui_nk_color(t->field_border));
    struct nk_color accent = pc_ui_nk_color(t->accent), dim = pc_ui_nk_color(t->text_dim);
    for (size_t i = 0; i < g->ncolors; i++) {
        float x = bar.x + g->colors[i].loc * bar.w;
        struct nk_color body =
            nk_rgb_f(g->colors[i].rgb[0], g->colors[i].rgb[1], g->colors[i].rgb[2]);
        nk_fill_rect(c, nk_rect(x - 5, bar.y + bar.h + 2, 10, 10), 0, body);
        nk_stroke_rect(c, nk_rect(x - 5, bar.y + bar.h + 2, 10, 10), 0, 1,
                       (int)i == sel_color ? accent : dim);
    }
    for (size_t i = 0; i < g->nopacity; i++) {
        float x = bar.x + g->opacity[i].loc * bar.w;
        nk_byte v = (nk_byte)(g->opacity[i].opacity * 255);
        nk_fill_rect(c, nk_rect(x - 5, bar.y - 12, 10, 10), 0, nk_rgb(v, v, v));
        nk_stroke_rect(c, nk_rect(x - 5, bar.y - 12, 10, 10), 0, 1,
                       (int)i == sel_opacity ? accent : dim);
    }
    /* Click to select a marker, or on empty space to add a stop. */
    if (nk_input_mouse_clicked(&ctx->input, NK_BUTTON_LEFT, outer)) {
        float mx = ctx->input.mouse.pos.x, my = ctx->input.mouse.pos.y;
        float loc = (mx - bar.x) / bar.w;
        loc = loc < 0 ? 0 : (loc > 1 ? 1 : loc);
        float tol = 6.0f / bar.w;
        bool opacity_region = my < bar.y + bar.h * .5f;
        if (opacity_region) {
            int hit = pc_ui_gradient_nearest_opacity(g, loc, tol);
            if (hit >= 0) {
                g_sel.kind = 2;
                g_sel.loc = g->opacity[(size_t)hit].loc;
            } else {
                stop_edit(app, "add", "opacity", -1, true, loc, false, 0, NULL);
                g_sel.kind = 2;
                g_sel.loc = loc;
            }
        } else {
            int hit = pc_ui_gradient_nearest_color(g, loc, tol);
            if (hit >= 0) {
                g_sel.kind = 1;
                g_sel.loc = g->colors[(size_t)hit].loc;
            } else {
                stop_edit(app, "add", NULL, -1, true, loc, false, 0, NULL);
                g_sel.kind = 1;
                g_sel.loc = loc;
            }
        }
    }
}
void pc_ui_shell_gradient_editor_draw(pc_ui_shell_app *app, float width, float height)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->panels.gradient_editor) {
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float w = 340, h = 320;
    struct nk_rect rect = nk_rect((width - w) * .5f, (height - h) * .5f, w, h);
    if (nk_begin_titled(ctx, "gradient-editor", pc_i18n_t("Gradient Editor"), rect,
                        NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE |
                            NK_WINDOW_NO_SCROLLBAR)) {
        pc_ui_gradient g;
        bool ok = current_gradient(app, &g);
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("The gradient the Gradient tool paints with"), NK_TEXT_LEFT);
        if (!ok) {
            nk_label(ctx, pc_i18n_t("No gradient available"), NK_TEXT_LEFT);
        }
        /* Resolve selection index from its stored location. */
        int sel_color = g_sel.kind == 1 ? pc_ui_gradient_nearest_color(&g, g_sel.loc, 0.03f) : -1;
        int sel_opacity = g_sel.kind == 2 ? pc_ui_gradient_nearest_opacity(&g, g_sel.loc, 0.03f) : -1;
        if (g_sel.kind == 1 && sel_color < 0 && g.ncolors) {
            sel_color = 0;
            g_sel.loc = g.colors[0].loc;
        }
        if (g_sel.kind == 2 && sel_opacity < 0 && g.nopacity) {
            sel_opacity = 0;
            g_sel.loc = g.opacity[0].loc;
        }
        if (g_sel.kind == 0) {
            g_sel.kind = 1;
            sel_color = 0;
            g_sel.loc = g.ncolors ? g.colors[0].loc : 0;
        }
        draw_strip(app, &g, sel_color, sel_opacity);
        /* Numeric fields for the selected stop. */
        if (g_sel.kind == 1 && sel_color >= 0 && (size_t)sel_color < g.ncolors) {
            pc_ui_grad_cstop *s = &g.colors[(size_t)sel_color];
            nk_layout_row_dynamic(ctx, 26, 3);
            for (int ch = 0; ch < 3; ch++) {
                const char *names[] = {"R", "G", "B"};
                int v = (int)lroundf(s->rgb[ch] * 255);
                int before = v;
                nk_property_int(ctx, names[ch], 0, &v, 255, 1, 1);
                if (v != before) {
                    s->rgb[ch] = (float)v / 255.f;
                    char hex[8];
                    pc_ui_picker_hex(s->rgb, hex);
                    stop_edit(app, "color", NULL, sel_color, false, 0, false, 0, hex);
                }
            }
            nk_layout_row_dynamic(ctx, 26, 1);
            double loc = s->loc * 100, bl = loc;
            nk_property_double(ctx, pc_i18n_t("Location"), 0, &loc, 100, 1, .5f);
            if (loc != bl) {
                g_sel.loc = (float)(loc / 100);
                stop_edit(app, "move", NULL, sel_color, true, loc / 100, false, 0, NULL);
            }
        } else if (g_sel.kind == 2 && sel_opacity >= 0 && (size_t)sel_opacity < g.nopacity) {
            pc_ui_grad_ostop *s = &g.opacity[(size_t)sel_opacity];
            nk_layout_row_dynamic(ctx, 26, 1);
            double op = s->opacity * 100, bo = op;
            nk_property_double(ctx, pc_i18n_t("Opacity"), 0, &op, 100, 1, .5f);
            if (op != bo) {
                stop_edit(app, "opacity", "opacity", sel_opacity, false, 0, true, op, NULL);
            }
            nk_layout_row_dynamic(ctx, 26, 1);
            double loc = s->loc * 100, bl = loc;
            nk_property_double(ctx, pc_i18n_t("Location"), 0, &loc, 100, 1, .5f);
            if (loc != bl) {
                g_sel.loc = (float)(loc / 100);
                stop_edit(app, "move", "opacity", sel_opacity, true, loc / 100, false, 0, NULL);
            }
        }
        /* Delete (needs > 2 of its kind). */
        nk_layout_row_dynamic(ctx, 26, 2);
        bool can_delete = (g_sel.kind == 1 && g.ncolors > 2 && sel_color >= 0) ||
                          (g_sel.kind == 2 && g.nopacity > 2 && sel_opacity >= 0);
        if (!can_delete) {
            nk_widget_disable_begin(ctx);
        }
        pc_ui_shell_widget_mapping(app, "gradient.editor.delete", nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t("Delete")) && can_delete) {
            stop_edit(app, "delete", g_sel.kind == 2 ? "opacity" : NULL,
                      g_sel.kind == 2 ? sel_opacity : sel_color, false, 0, false, 0, NULL);
            g_sel.kind = 0;
        }
        if (!can_delete) {
            nk_widget_disable_end(ctx);
        }
        if (nk_button_label(ctx, pc_i18n_t("Save As Preset"))) {
            pc_ui_shell_queue(app, "gradient.presets.new", NULL);
        }
        nk_layout_row_dynamic(ctx, 26, 1);
        pc_ui_shell_widget_mapping(app, "window.panel.gradients", nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t("Gradients"))) {
            pc_ui_shell_queue(app, "window.panel.gradients", NULL);
        }
    } else {
        ui->panels.gradient_editor = false;
    }
    nk_end(ctx);
}

/* ================================================================================================ */
/* Live Gradient Fill tool: on-canvas widget (create / edit a Gradient Fill layer).                  */
/* Port of the Gradient-tool fill-layer mode in crates/ui-egui/src/gradient_ui.rs + canvas.rs. The   */
/* geometry/hit-test maths live in ui/gradient_widget.c (headless-tested); this is the thin shell     */
/* routing + drawing. Dispatches gradient.fill.create / gradient.fill.set / gradient.fill.stop.      */
/* ================================================================================================ */

#define PC_GRAD_GRAB_PX 8.0
#define PC_GRAD_TEAR_PX 36.0
#define PC_GRAD_DRAG_PX 2.0

/* The Gradient tool is in its live fill-layer mode (non-classic). */
static bool grad_live_mode(pc_ui_shell_app *app)
{
    const pc_ui_state *ui = pc_ui_shell_app_state(app);
    return ui->tool == PC_TOOL_GRADIENT && !ui->tool_options.gradient_classic;
}

/* Parse a gradient.fill.get result into a widget + style name + layer id. Returns false if the
 * result is not a usable gradient fill. */
static bool grad_parse(yyjson_mut_val *root, pc_ui_grad_widget *w, char style[16], pc_layer_id *out_layer)
{
    if (!root || !w) {
        return false;
    }
    *w = (pc_ui_grad_widget){0};
    yyjson_mut_val *from = yyjson_mut_obj_get(root, "from");
    yyjson_mut_val *to = yyjson_mut_obj_get(root, "to");
    if (yyjson_mut_arr_size(from) != 2 || yyjson_mut_arr_size(to) != 2) {
        return false;
    }
    w->from[0] = yyjson_mut_get_num(yyjson_mut_arr_get(from, 0));
    w->from[1] = yyjson_mut_get_num(yyjson_mut_arr_get(from, 1));
    w->to[0] = yyjson_mut_get_num(yyjson_mut_arr_get(to, 0));
    w->to[1] = yyjson_mut_get_num(yyjson_mut_arr_get(to, 1));
    if (!isfinite(w->from[0]) || !isfinite(w->from[1]) || !isfinite(w->to[0]) || !isfinite(w->to[1])) {
        return false;
    }
    w->reverse = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "reverse"));
    size_t i, max;
    yyjson_mut_val *it;
    yyjson_mut_val *stops = yyjson_mut_obj_get(root, "stops");
    yyjson_mut_arr_foreach(stops, i, max, it)
    {
        if (w->nstops < PC_UI_GRAD_WIDGET_MAX && yyjson_mut_arr_size(it) >= 1) {
            w->stops[w->nstops++] = yyjson_mut_get_num(yyjson_mut_arr_get(it, 0));
        }
    }
    yyjson_mut_val *mids = yyjson_mut_obj_get(root, "midpoints");
    yyjson_mut_arr_foreach(mids, i, max, it)
    {
        if (w->nmids < PC_UI_GRAD_WIDGET_MAX) {
            w->mids[w->nmids++] = yyjson_mut_get_num(it);
        }
    }
    if (style) {
        const char *sn = yyjson_mut_get_str(yyjson_mut_obj_get(root, "style"));
        (void)snprintf(style, 16, "%s", sn ? sn : "linear");
    }
    if (out_layer) {
        *out_layer = (pc_layer_id)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "layer"));
    }
    return true;
}

/* Query the gradient fill of a layer (have_layer=false -> the active layer). */
static bool grad_fill_get(pc_ui_shell_app *app, bool have_layer, pc_layer_id layer, pc_ui_grad_widget *w,
                          char style[16], pc_layer_id *out_layer)
{
    pc_engine_session *s = pc_ui_shell_app_session(app);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    if (!p) {
        yyjson_mut_doc_free(d);
        return false;
    }
    if (have_layer) {
        yyjson_mut_obj_add_uint(d, p, "layer", (uint64_t)layer);
    }
    pc_buf text = PC_BUF_INIT;
    bool ok = false;
    if (pc_json_write(p, 0, &text, NULL) == PC_OK) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        if (pc_engine_session_execute_json(s, "gradient.fill.get", pc_buf_cstr(&text), &out, NULL) == PC_OK) {
            ok = grad_parse(pc_json_value_root(&out), w, style, out_layer);
        }
        pc_json_value_fini(&out);
    }
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
    return ok;
}

static void grad_clear_preview(pc_ui_canvas_view *cv)
{
    pc_doc_arc_release(cv->grad_preview);
    cv->grad_preview = NULL;
}

static yyjson_mut_val *grad_pair(yyjson_mut_doc *d, double x, double y)
{
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, a, x);
    yyjson_mut_arr_add_real(d, a, y);
    return a;
}

/* Fill `root` with the gradient.fill.create / gradient.fill.set params for the current drag. */
static const char *grad_geometry_params(pc_ui_shell_app *app, pc_ui_canvas_view *cv, yyjson_mut_doc *d,
                                        yyjson_mut_val *root)
{
    pc_ui_grad_drag *g = &cv->grad_drag;
    const pc_ui_tool_options *o = &pc_ui_shell_app_state(app)->tool_options;
    if (g->creating) {
        yyjson_mut_obj_add_val(d, root, "from", grad_pair(d, g->cur_from[0], g->cur_from[1]));
        yyjson_mut_obj_add_val(d, root, "to", grad_pair(d, g->cur_to[0], g->cur_to[1]));
        yyjson_mut_obj_add_strcpy(d, root, "style", pc_str_c(&o->gradient_style));
        yyjson_mut_obj_add_bool(d, root, "reverse", o->gradient_reverse);
        yyjson_mut_obj_add_bool(d, root, "dither", o->gradient_dither);
        yyjson_mut_obj_add_real(d, root, "opacity", o->fill_opacity);
        yyjson_mut_obj_add_str(d, root, "mode", pc_blend_name(o->gradient_blend_mode));
        return "gradient.fill.create";
    }
    yyjson_mut_obj_add_uint(d, root, "layer", (uint64_t)g->layer);
    yyjson_mut_obj_add_val(d, root, "from", grad_pair(d, g->cur_from[0], g->cur_from[1]));
    yyjson_mut_obj_add_val(d, root, "to", grad_pair(d, g->cur_to[0], g->cur_to[1]));
    return "gradient.fill.set";
}

/* Live engine preview of a geometry drag (create / redraw / endpoint / line translate). */
static void grad_build_preview(pc_ui_shell_app *app, pc_ui_canvas_view *cv)
{
    pc_ui_grad_drag *g = &cv->grad_drag;
    if (g->grab.kind == PC_UI_GRAD_GRAB_STOP || g->grab.kind == PC_UI_GRAD_GRAB_MID) {
        grad_clear_preview(cv); /* stop drags are shown by the overlay markers only */
        return;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (!root) {
        yyjson_mut_doc_free(d);
        return;
    }
    const char *cmd = grad_geometry_params(app, cv, d, root);
    pc_buf text = PC_BUF_INIT;
    if (pc_json_write(root, 0, &text, NULL) == PC_OK) {
        yyjson_doc *params = NULL;
        if (pc_json_parse(pc_buf_cstr(&text), text.len, &params, NULL) == PC_OK) {
            pc_doc_arc *doc = NULL;
            if (pc_engine_preview_sync(pc_ui_shell_app_session(app), cmd, yyjson_doc_get_root(params), &doc,
                                       NULL) == PC_OK) {
                pc_doc_arc_release(cv->grad_preview);
                cv->grad_preview = doc;
                cv->grad_preview_rev++;
            } else {
                grad_clear_preview(cv);
            }
        }
        yyjson_doc_free(params);
    }
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
}

/* Execute one gradient.fill.* command built in `root`; reports an error through the status line. */
static void grad_execute(pc_ui_shell_app *app, const char *cmd, yyjson_mut_val *root)
{
    pc_buf text = PC_BUF_INIT;
    pc_error err = {0};
    if (pc_json_write(root, 0, &text, &err) == PC_OK) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        pc_status rc = pc_engine_session_execute_json(pc_ui_shell_app_session(app), cmd, pc_buf_cstr(&text),
                                                      &out, &err);
        pc_json_value_fini(&out);
        if (rc != PC_OK) {
            pc_ui_shell_report(app, "Gradient", err.msg, true);
        }
    }
    pc_buf_fini(&text);
}

/* Commit the finished drag (on pointer up). */
static void grad_commit(pc_ui_shell_app *app, pc_ui_canvas_view *cv)
{
    pc_ui_grad_drag *g = &cv->grad_drag;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (!root) {
        yyjson_mut_doc_free(d);
        return;
    }
    if (g->grab.kind == PC_UI_GRAD_GRAB_STOP || g->grab.kind == PC_UI_GRAD_GRAB_MID) {
        /* Re-read the layer for the reverse flag and stop locations. */
        pc_ui_grad_widget w;
        char style[16];
        bool ok = grad_fill_get(app, true, g->layer, &w, style, NULL);
        yyjson_mut_obj_add_uint(d, root, "layer", (uint64_t)g->layer);
        double t = w.reverse ? 1.0 - g->cur_u : g->cur_u; /* gradient location */
        if (g->grab.kind == PC_UI_GRAD_GRAB_STOP) {
            if (g->tear_off && ok && w.nstops > 2) {
                yyjson_mut_obj_add_str(d, root, "action", "delete");
                yyjson_mut_obj_add_int(d, root, "index", g->grab.index);
            } else {
                yyjson_mut_obj_add_str(d, root, "action", "move");
                yyjson_mut_obj_add_int(d, root, "index", g->grab.index);
                yyjson_mut_obj_add_real(d, root, "location", t < 0 ? 0 : (t > 1 ? 1 : t));
            }
        } else {
            double loc = 0.5;
            size_t seg = (size_t)(g->grab.index < 0 ? 0 : g->grab.index);
            if (ok && seg + 1 < w.nstops) {
                double a = w.stops[seg], b = w.stops[seg + 1];
                if (b - a > 1e-4) {
                    loc = (t - a) / (b - a);
                }
            }
            loc = loc < 0.05 ? 0.05 : (loc > 0.95 ? 0.95 : loc);
            yyjson_mut_obj_add_str(d, root, "action", "midpoint");
            yyjson_mut_obj_add_int(d, root, "index", g->grab.index);
            yyjson_mut_obj_add_real(d, root, "location", loc);
        }
        grad_execute(app, "gradient.fill.stop", root);
    } else {
        const char *cmd = grad_geometry_params(app, cv, d, root);
        grad_execute(app, cmd, root);
    }
    yyjson_mut_doc_free(d);
    pc_ui_shell_app_changed(app);
}

/* A click (no movement) on the gradient line adds a colour stop there. */
static void grad_click(pc_ui_shell_app *app, pc_ui_canvas_view *cv)
{
    pc_ui_grad_drag *g = &cv->grad_drag;
    if (g->grab.kind != PC_UI_GRAD_GRAB_LINE) {
        return;
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (!root) {
        yyjson_mut_doc_free(d);
        return;
    }
    yyjson_mut_obj_add_uint(d, root, "layer", (uint64_t)g->layer);
    yyjson_mut_obj_add_str(d, root, "action", "add");
    double t = g->grab.t;
    yyjson_mut_obj_add_real(d, root, "location", t < 0 ? 0 : (t > 1 ? 1 : t));
    grad_execute(app, "gradient.fill.stop", root);
    yyjson_mut_doc_free(d);
    pc_ui_shell_app_changed(app);
}

bool pc_ui_shell_gradient_canvas(pc_ui_shell_app *app, pc_ui_canvas_view *cv, const pc_ui_canvas_xform *xf,
                                 const SDL_Event *event, float factor)
{
    if (!app || !cv || !xf || !event) {
        return false;
    }
    pc_ui_grad_drag *g = &cv->grad_drag;
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        g->active = false;
        grad_clear_preview(cv);
        return false;
    }
    if (!grad_live_mode(app)) {
        if (g->active) {
            g->active = false;
            grad_clear_preview(cv);
        }
        return false;
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    bool down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT;
    bool up = event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_LEFT;
    bool moving = event->type == SDL_EVENT_MOUSE_MOTION;
    if (!down && !up && !moving) {
        return false;
    }
    if (down && (ui->n_dialogs || pc_ui_shell_jobs_locked(app) || pc_ui_shell_keyboard_captured(app))) {
        return false;
    }
    float sx = (moving ? event->motion.x : event->button.x) * factor;
    float sy = (moving ? event->motion.y : event->button.y) * factor;
    float dc[2];
    if (pc_ui_canvas_to_doc(xf, sx, sy, dc) != PC_OK) {
        return g->active;
    }
    double px = dc[0], py = dc[1];
    bool shift = (cv->held_mods & PC_UI_MOD_SHIFT) != 0;
    if (down) {
        if (sx < xf->x || sy < xf->y || sx >= xf->x + xf->width || sy >= xf->y + xf->height) {
            return false;
        }
        const pc_engine_doc_state *st = pc_engine_session_active(pc_ui_shell_app_session(app));
        if (!st) {
            return false;
        }
        double tol = PC_GRAD_GRAB_PX;
        float a[2], b[2];
        if (pc_ui_canvas_to_doc(xf, sx, sy, a) == PC_OK && pc_ui_canvas_to_doc(xf, sx + 8, sy, b) == PC_OK) {
            double t = hypot((double)b[0] - a[0], (double)b[1] - a[1]);
            if (isfinite(t) && t > 0) {
                tol = t;
            }
        }
        pc_ui_grad_widget w;
        char style[16] = "linear";
        pc_layer_id id = 0;
        bool active = grad_fill_get(app, false, 0, &w, style, &id);
        bool mask = ui->mask_target || pc_engine_targeted_mask(pc_ui_shell_app_session(app));
        pc_ui_grad_grab grab = {PC_UI_GRAD_GRAB_NONE, -1, 0.0};
        if (active) {
            grab = pc_ui_grad_widget_hit(&w, px, py, tol);
        }
        *g = (pc_ui_grad_drag){0};
        g->document = pc_doc_arc_get(st->doc)->id;
        g->modifiers = cv->held_mods;
        g->down[0] = px;
        g->down[1] = py;
        if (active && grab.kind != PC_UI_GRAD_GRAB_NONE) {
            g->active = true;
            g->creating = false;
            g->layer = id;
            g->grab = grab;
            memcpy(g->from, w.from, sizeof g->from);
            memcpy(g->to, w.to, sizeof g->to);
            memcpy(g->cur_from, w.from, sizeof g->cur_from);
            memcpy(g->cur_to, w.to, sizeof g->cur_to);
            (void)snprintf(g->style, sizeof g->style, "%s", style);
        } else {
            if (mask && !active) {
                g->active = false;
                return false; /* paint a classic gradient into the mask instead */
            }
            g->active = true;
            g->creating = !active;
            g->layer = active ? id : 0;
            g->grab.kind = PC_UI_GRAD_GRAB_NONE;
            g->from[0] = g->to[0] = g->cur_from[0] = g->cur_to[0] = px;
            g->from[1] = g->to[1] = g->cur_from[1] = g->cur_to[1] = py;
            if (active) {
                (void)snprintf(g->style, sizeof g->style, "%s", style);
            } else {
                (void)snprintf(g->style, sizeof g->style, "%s",
                               pc_str_c(&ui->tool_options.gradient_style));
            }
        }
        return true;
    }
    if (!g->active) {
        return false;
    }
    /* Has the pointer moved far enough to be a drag (screen distance)? */
    float ds[2], ps[2];
    if (pc_ui_canvas_to_screen(xf, (float)g->down[0], (float)g->down[1], ds) == PC_OK &&
        pc_ui_canvas_to_screen(xf, (float)px, (float)py, ps) == PC_OK) {
        if (hypot((double)ps[0] - ds[0], (double)ps[1] - ds[1]) >= PC_GRAD_DRAG_PX) {
            g->moved = true;
        }
    }
    double p[2] = {px, py};
    switch (g->grab.kind) {
    case PC_UI_GRAD_GRAB_START: {
        double snapped[2];
        pc_ui_grad_snap45(g->to, p, shift, snapped);
        memcpy(g->cur_from, snapped, sizeof g->cur_from);
        memcpy(g->cur_to, g->to, sizeof g->cur_to);
        break;
    }
    case PC_UI_GRAD_GRAB_END: {
        double snapped[2];
        pc_ui_grad_snap45(g->from, p, shift, snapped);
        memcpy(g->cur_to, snapped, sizeof g->cur_to);
        memcpy(g->cur_from, g->from, sizeof g->cur_from);
        break;
    }
    case PC_UI_GRAD_GRAB_LINE: {
        double dx = p[0] - g->down[0], dy = p[1] - g->down[1];
        g->cur_from[0] = g->from[0] + dx;
        g->cur_from[1] = g->from[1] + dy;
        g->cur_to[0] = g->to[0] + dx;
        g->cur_to[1] = g->to[1] + dy;
        break;
    }
    case PC_UI_GRAD_GRAB_STOP:
    case PC_UI_GRAD_GRAB_MID: {
        pc_ui_grad_widget base = {0};
        memcpy(base.from, g->from, sizeof base.from);
        memcpy(base.to, g->to, sizeof base.to);
        double u = 0, perp = 0;
        pc_ui_grad_widget_project(&base, px, py, &u, &perp);
        g->cur_u = u;
        double zoom_perp = perp;
        float e0[2], e1[2];
        if (pc_ui_canvas_to_screen(xf, (float)g->from[0], (float)g->from[1], e0) == PC_OK &&
            pc_ui_canvas_to_screen(xf, (float)(g->from[0] + 1), (float)g->from[1], e1) == PC_OK) {
            double ppd = hypot((double)e1[0] - e0[0], (double)e1[1] - e0[1]);
            zoom_perp = perp * (ppd > 0 ? ppd : 1);
        }
        g->tear_off = g->grab.kind == PC_UI_GRAD_GRAB_STOP && zoom_perp > PC_GRAD_TEAR_PX;
        memcpy(g->cur_from, g->from, sizeof g->cur_from);
        memcpy(g->cur_to, g->to, sizeof g->cur_to);
        break;
    }
    default: { /* Draw (create / redraw): the line runs from the press to the pointer. */
        double snapped[2];
        pc_ui_grad_snap45(g->down, p, shift, snapped);
        memcpy(g->cur_from, g->down, sizeof g->cur_from);
        memcpy(g->cur_to, snapped, sizeof g->cur_to);
        break;
    }
    }
    if (moving) {
        if (g->moved) {
            grad_build_preview(app, cv);
        }
        return true;
    }
    /* pointer up */
    grad_clear_preview(cv);
    if (g->moved) {
        grad_commit(app, cv);
    } else {
        grad_click(app, cv);
    }
    g->active = false;
    return true;
}

/* ---- overlay drawing ---- */

static float grad_clampc(float v)
{
    return v < -16000.f ? -16000.f : (v > 16000.f ? 16000.f : v);
}
static bool grad_screen(const pc_ui_canvas_xform *xf, const double p[2], float out[2])
{
    float s[2];
    if (pc_ui_canvas_to_screen(xf, (float)p[0], (float)p[1], s) != PC_OK || !isfinite(s[0]) ||
        !isfinite(s[1])) {
        return false;
    }
    out[0] = grad_clampc(s[0]);
    out[1] = grad_clampc(s[1]);
    return true;
}
static void grad_line(struct nk_command_buffer *c, const float a[2], const float b[2], struct nk_color col)
{
    nk_stroke_line(c, a[0], a[1], b[0], b[1], 3, nk_rgba(0, 0, 0, 90));
    nk_stroke_line(c, a[0], a[1], b[0], b[1], 1.5f, col);
}
static void grad_ring(struct nk_command_buffer *c, const float p[2], float r, struct nk_color col)
{
    nk_stroke_circle(c, nk_rect(p[0] - r - 1, p[1] - r - 1, 2 * (r + 1), 2 * (r + 1)), 3,
                     nk_rgba(0, 0, 0, 90));
    nk_stroke_circle(c, nk_rect(p[0] - r, p[1] - r, 2 * r, 2 * r), 2, col);
}

void pc_ui_shell_gradient_widget_draw(pc_ui_shell_app *app, pc_ui_canvas_view *cv,
                                      const pc_ui_canvas_xform *xf)
{
    if (!app || !cv || !xf || !grad_live_mode(app)) {
        return;
    }
    pc_ui_grad_drag *g = &cv->grad_drag;
    pc_ui_grad_widget w;
    char style[16] = "linear";
    bool have = false;
    if (g->active) {
        w = (pc_ui_grad_widget){0};
        memcpy(w.from, g->cur_from, sizeof w.from);
        memcpy(w.to, g->cur_to, sizeof w.to);
        (void)snprintf(style, sizeof style, "%s", g->style);
        have = true;
    } else {
        have = grad_fill_get(app, false, 0, &w, style, NULL);
    }
    if (!have) {
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *c = nk_window_get_canvas(ctx);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    struct nk_color white = nk_rgb(255, 255, 255);
    struct nk_color accent = pc_ui_nk_color(t->accent);
    float s0[2], s1[2];
    if (!grad_screen(xf, w.from, s0) || !grad_screen(xf, w.to, s1)) {
        return;
    }
    grad_line(c, s0, s1, white);
    /* Per-style decoration. */
    if (!strcmp(style, "radial") || !strcmp(style, "angle")) {
        double center[2], radius;
        if (pc_ui_grad_decor_circle(&w, center, &radius)) {
            float sc[2];
            if (grad_screen(xf, center, sc)) {
                double rr = hypot((double)s1[0] - s0[0], (double)s1[1] - s0[1]);
                float r = grad_clampc((float)rr);
                nk_stroke_circle(c, nk_rect(sc[0] - r - 1, sc[1] - r - 1, 2 * (r + 1), 2 * (r + 1)), 3,
                                 nk_rgba(0, 0, 0, 90));
                nk_stroke_circle(c, nk_rect(sc[0] - r, sc[1] - r, 2 * r, 2 * r), 1.5f, white);
            }
        }
    } else if (!strcmp(style, "diamond")) {
        double corners[4][2];
        if (pc_ui_grad_decor_diamond(&w, corners)) {
            float poly[10];
            bool okp = true;
            for (size_t i = 0; i < 4 && okp; i++) {
                float sc[2];
                okp = grad_screen(xf, corners[i], sc);
                poly[i * 2] = sc[0];
                poly[i * 2 + 1] = sc[1];
            }
            if (okp) {
                poly[8] = poly[0];
                poly[9] = poly[1];
                nk_stroke_polyline(c, poly, 5, 3, nk_rgba(0, 0, 0, 90));
                nk_stroke_polyline(c, poly, 5, 1.5f, white);
            }
        }
    } else if (!strcmp(style, "reflected")) {
        double mirror[2];
        float sm[2];
        if (pc_ui_grad_decor_reflected(&w, mirror) && grad_screen(xf, mirror, sm)) {
            nk_stroke_line(c, s0[0], s0[1], sm[0], sm[1], 1.5f, nk_rgba(255, 255, 255, 170));
        }
    }
    /* Colour stops along the line. */
    for (size_t i = 0; i < w.nstops; i++) {
        double at[2];
        pc_ui_grad_widget_at(&w, w.stops[i], at);
        float sp[2];
        if (grad_screen(xf, at, sp)) {
            bool sel = g->active && g->grab.kind == PC_UI_GRAD_GRAB_STOP && g->grab.index == (int)i;
            nk_fill_circle(c, nk_rect(sp[0] - 4, sp[1] - 4, 8, 8), nk_rgba(0, 0, 0, 110));
            nk_stroke_circle(c, nk_rect(sp[0] - 4, sp[1] - 4, 8, 8), 1.5f, sel ? accent : white);
        }
    }
    /* End handles. */
    bool gs = g->active && g->grab.kind == PC_UI_GRAD_GRAB_START;
    bool ge = g->active && g->grab.kind == PC_UI_GRAD_GRAB_END;
    grad_ring(c, s0, 8, gs ? accent : white);
    grad_ring(c, s1, 8, ge ? accent : white);
}

/* ---- Properties-panel section ---- */

/* Queue a gradient.fill.set with one numeric/bool/string field for the given layer. */
static void grad_prop_set(pc_ui_shell_app *app, pc_layer_id layer, const char *key, double num, bool is_bool,
                          bool bval, const char *str)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    if (!p) {
        yyjson_mut_doc_free(d);
        return;
    }
    yyjson_mut_obj_add_uint(d, p, "layer", (uint64_t)layer);
    if (str) {
        yyjson_mut_obj_add_strcpy(d, p, key, str);
    } else if (is_bool) {
        yyjson_mut_obj_add_bool(d, p, key, bval);
    } else {
        yyjson_mut_obj_add_real(d, p, key, num);
    }
    pc_ui_shell_queue(app, "gradient.fill.set", p);
    yyjson_mut_doc_free(d);
}

void pc_ui_shell_gradient_properties(pc_ui_shell_app *app, const pc_layer *layer)
{
    if (!app || !layer) {
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_grad_widget w;
    char style[16] = "linear";
    pc_layer_id id = layer->id;
    pc_json_value out = PC_JSON_VALUE_INIT;
    yyjson_mut_doc *req = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *rp = req ? yyjson_mut_obj(req) : NULL;
    double angle = 0, scale = 100;
    bool reverse = false, dither = true, align = false, ok = false;
    if (rp) {
        yyjson_mut_obj_add_uint(req, rp, "layer", (uint64_t)id);
        pc_buf text = PC_BUF_INIT;
        if (pc_json_write(rp, 0, &text, NULL) == PC_OK &&
            pc_engine_session_execute_json(pc_ui_shell_app_session(app), "gradient.fill.get",
                                           pc_buf_cstr(&text), &out, NULL) == PC_OK) {
            yyjson_mut_val *root = pc_json_value_root(&out);
            ok = grad_parse(root, &w, style, NULL);
            angle = yyjson_mut_get_num(yyjson_mut_obj_get(root, "angle"));
            scale = yyjson_mut_get_num(yyjson_mut_obj_get(root, "scale"));
            reverse = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "reverse"));
            dither = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "dither"));
            align = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "align"));
        }
        pc_buf_fini(&text);
    }
    pc_json_value_fini(&out);
    yyjson_mut_doc_free(req);
    if (!ok) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("Not a gradient fill"), NK_TEXT_LEFT);
        return;
    }
    nk_layout_row_dynamic(ctx, 20, 1);
    nk_label(ctx, pc_i18n_t("Gradient Options"), NK_TEXT_LEFT);
    /* Style buttons. */
    const char *styles[] = {"linear", "radial", "angle", "reflected", "diamond"};
    const char *labels[] = {"Linear", "Radial", "Angle", "Reflected", "Diamond"};
    nk_layout_row_dynamic(ctx, 24, 5);
    for (size_t i = 0; i < 5; i++) {
        struct nk_style_button before = ctx->style.button;
        if (!strcmp(style, styles[i])) {
            ctx->style.button.normal =
                nk_style_item_color(pc_ui_nk_color(pc_ui_shell_app_tokens(app)->accent_soft));
        }
        char map[48];
        (void)snprintf(map, sizeof map, "gradient.fill.style.%s", styles[i]);
        pc_ui_shell_widget_mapping(app, map, nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t(labels[i]))) {
            grad_prop_set(app, id, "style", 0, false, false, styles[i]);
        }
        ctx->style.button = before;
    }
    nk_layout_row_dynamic(ctx, 26, 1);
    double a = angle, ba = a;
    nk_property_double(ctx, pc_i18n_t("Angle"), -180, &a, 180, 1, .5f);
    if (a != ba) {
        grad_prop_set(app, id, "angle", a, false, false, NULL);
    }
    double sc = scale, bsc = sc;
    nk_property_double(ctx, pc_i18n_t("Scale"), 10, &sc, 150, 1, .5f);
    if (sc != bsc) {
        grad_prop_set(app, id, "scale", sc, false, false, NULL);
    }
    nk_layout_row_dynamic(ctx, 24, 2);
    nk_bool rv = reverse;
    if (nk_checkbox_label(ctx, pc_i18n_t("Reverse"), &rv)) {
        grad_prop_set(app, id, "reverse", 0, true, rv != 0, NULL);
    }
    nk_bool dt = dither;
    if (nk_checkbox_label(ctx, pc_i18n_t("Dither"), &dt)) {
        grad_prop_set(app, id, "dither", 0, true, dt != 0, NULL);
    }
    nk_layout_row_dynamic(ctx, 24, 2);
    nk_bool al = align;
    if (nk_checkbox_label(ctx, pc_i18n_t("Align with layer"), &al)) {
        grad_prop_set(app, id, "align", 0, true, al != 0, NULL);
    }
    pc_ui_shell_widget_mapping(app, "gradient.fill.resetAlignment", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Reset Alignment"))) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        if (p) {
            yyjson_mut_obj_add_uint(d, p, "layer", (uint64_t)id);
            yyjson_mut_obj_add_val(d, p, "offset", grad_pair(d, 0, 0));
            pc_ui_shell_queue(app, "gradient.fill.set", p);
        }
        yyjson_mut_doc_free(d);
    }
}
