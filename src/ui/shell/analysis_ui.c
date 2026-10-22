#include "analysis_ui.h"
#include "app.h"
#include "canvas_ink.h"
#include "jobs_ui.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "base/json.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ small helpers */

static const pc_document *active_doc(pc_ui_shell_app *app)
{
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    return state ? pc_doc_arc_get(state->doc) : NULL;
}

static struct nk_color rgb_nk(const float rgb[3])
{
    return nk_rgba((int)lroundf(fminf(1, fmaxf(0, rgb[0])) * 255),
                   (int)lroundf(fminf(1, fmaxf(0, rgb[1])) * 255),
                   (int)lroundf(fminf(1, fmaxf(0, rgb[2])) * 255), 255);
}

static bool empty_params(yyjson_val *p)
{
    return !p || (yyjson_is_obj(p) && yyjson_obj_size(p) == 0);
}

/* Queue an engine command with no params. */
static void queue(pc_ui_shell_app *app, const char *id)
{
    pc_ui_shell_queue(app, id, NULL);
}

/* ------------------------------------------------------------------ command front end */

static void open_scale_dialog(pc_ui_shell_app *app, pc_ui_analysis *a, const pc_document *doc)
{
    const pc_measurement_scale *sc = &doc->measurement.scale;
    a->dialog = PC_UI_ANALYSIS_DIALOG_SCALE;
    a->scale_default = pc_measurement_scale_is_default(sc);
    a->scale_pixel = sc->pixel_length;
    a->scale_logical = sc->logical_length;
    (void)snprintf(a->scale_units, sizeof a->scale_units, "%s", pc_str_c(&sc->units));
    (void)app;
}

static void open_marker_dialog(pc_ui_shell_app *app, pc_ui_analysis *a, const pc_document *doc)
{
    a->dialog = PC_UI_ANALYSIS_DIALOG_SCALE_MARKER;
    a->marker_length = pc_ui_analysis_suggested_length(doc->size.width, &doc->measurement.scale);
    a->marker_font = 12.0;
    a->marker_display_text = true;
    a->marker_text_position = 0;
    a->marker_color = 0;
    (void)snprintf(a->marker_units, sizeof a->marker_units, "%s", pc_str_c(&doc->measurement.scale.units));
    (void)app;
}

static const char *dp_name(yyjson_mut_val *columns, const char *key)
{
    if (!columns || !yyjson_mut_is_arr(columns))
        return key;
    size_t i, n = yyjson_mut_arr_size(columns);
    yyjson_mut_val *c;
    yyjson_mut_arr_foreach (columns, i, n, c) {
        const char *k = yyjson_mut_get_str(yyjson_mut_obj_get(c, "key"));
        if (k && !strcmp(k, key)) {
            const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(c, "name"));
            return name ? name : key;
        }
    }
    return key;
}

static bool dp_selected(yyjson_mut_val *current, const char *key)
{
    if (!current || !yyjson_mut_is_arr(current))
        return false;
    size_t i, n = yyjson_mut_arr_size(current);
    yyjson_mut_val *v;
    yyjson_mut_arr_foreach (current, i, n, v) {
        const char *k = yyjson_mut_get_str(v);
        if (k && !strcmp(k, key))
            return true;
    }
    return false;
}

static void open_data_points_dialog(pc_ui_shell_app *app, pc_ui_analysis *a)
{
    a->dialog = PC_UI_ANALYSIS_DIALOG_DATA_POINTS;
    a->dp_nrows = 0;
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    if (pc_engine_session_execute_json(pc_ui_shell_app_session(app), "image.analysis.selectDataPoints",
                                       "{}", &out, &err) != PC_OK) {
        pc_json_value_fini(&out);
        return;
    }
    yyjson_mut_val *root = pc_json_value_root(&out);
    yyjson_mut_val *data = root ? yyjson_mut_obj_get(root, "dataPoints") : NULL;
    yyjson_mut_val *avail = root ? yyjson_mut_obj_get(root, "available") : NULL;
    yyjson_mut_val *columns = root ? yyjson_mut_obj_get(root, "columns") : NULL;
    static const char *const sources[3] = {"selection", "ruler", "count"};
    for (size_t si = 0; si < 3; si++) {
        yyjson_mut_val *av = avail ? yyjson_mut_obj_get(avail, sources[si]) : NULL;
        yyjson_mut_val *cur = data ? yyjson_mut_obj_get(data, sources[si]) : NULL;
        if (!av || !yyjson_mut_is_arr(av))
            continue;
        size_t i, n = yyjson_mut_arr_size(av);
        yyjson_mut_val *kv;
        yyjson_mut_arr_foreach (av, i, n, kv) {
            const char *key = yyjson_mut_get_str(kv);
            if (!key || a->dp_nrows >= PC_UI_ANALYSIS_DP_MAX)
                continue;
            pc_ui_analysis_dp_row *row = &a->dp_rows[a->dp_nrows++];
            (void)snprintf(row->source, sizeof row->source, "%s", sources[si]);
            (void)snprintf(row->key, sizeof row->key, "%s", key);
            (void)snprintf(row->name, sizeof row->name, "%s", dp_name(columns, key));
            row->selected = dp_selected(cur, key);
        }
    }
    pc_json_value_fini(&out);
}

pc_status pc_ui_shell_analysis_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                       pc_json_value *out, pc_error *err)
{
    pc_ui_analysis *a = pc_ui_shell_analysis_runtime(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_document *doc = active_doc(app);

    if (!strcmp(id, "window.panel.measurementLog") || !strcmp(id, "window.panel.notes")) {
        bool *slot = !strcmp(id, "window.panel.notes") ? &a->notes_panel : &a->measurement_log;
        yyjson_val *show = pc_json_get(params, "show");
        *slot = (show && yyjson_is_bool(show)) ? yyjson_get_bool(show) : !*slot;
        return PC_OK;
    }
    if (!strcmp(id, "image.analysis.rulerTool") || !strcmp(id, "image.analysis.countTool")) {
        if (!empty_params(params))
            return pc_fail(err, PC_ERR_NOT_FOUND, "engine ruler command"); /* let the engine set it */
        ui->tool = !strcmp(id, "image.analysis.countTool") ? PC_TOOL_COUNT_TOOL : PC_TOOL_RULER;
        pc_ui_state_remember_group_tool(ui);
        return PC_OK;
    }
    if (!strcmp(id, "image.analysis.setMeasurementScale")) {
        if (!empty_params(params) || !doc)
            return pc_fail(err, PC_ERR_NOT_FOUND, "engine scale command");
        open_scale_dialog(app, a, doc);
        return PC_OK;
    }
    if (!strcmp(id, "image.analysis.placeScaleMarker")) {
        if (!empty_params(params) || !doc)
            return pc_fail(err, PC_ERR_NOT_FOUND, "engine marker command");
        open_marker_dialog(app, a, doc);
        return PC_OK;
    }
    if (!strcmp(id, "image.analysis.selectDataPoints")) {
        if (!empty_params(params))
            return pc_fail(err, PC_ERR_NOT_FOUND, "engine data-points command");
        open_data_points_dialog(app, a);
        return PC_OK;
    }
    if (!strcmp(id, "image.analysis.recordMeasurements") && empty_params(params)) {
        const char *source = ui->tool == PC_TOOL_RULER     ? "ruler"
                             : ui->tool == PC_TOOL_COUNT_TOOL ? "count"
                                                              : "auto";
        char json[64];
        (void)snprintf(json, sizeof json, "{\"source\":\"%s\"}", source);
        pc_status rc = pc_engine_session_execute_json(pc_ui_shell_app_session(app),
                                                      "image.analysis.recordMeasurements", json, out, err);
        if (rc == PC_OK)
            a->measurement_log = true;
        return rc;
    }
    return pc_fail(err, PC_ERR_NOT_FOUND, "not an analysis command");
}

/* ------------------------------------------------------------------ canvas input */

bool pc_ui_shell_analysis_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                                 float factor)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!pc_ui_analysis_tool(ui->tool) || ui->n_dialogs || ui->transform.active ||
        pc_ui_shell_jobs_locked(app))
        return false;
    bool moving = event->type == SDL_EVENT_MOUSE_MOTION, down = event->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    if (!moving && !down && event->type != SDL_EVENT_MOUSE_BUTTON_UP)
        return false;
    if (!moving && event->button.button != SDL_BUTTON_LEFT)
        return false;
    float x = (moving ? event->motion.x : event->button.x) * factor,
          y = (moving ? event->motion.y : event->button.y) * factor;
    pc_ui_analysis *a = pc_ui_shell_analysis_runtime(app);
    bool dragging = a->drag != PC_UI_ANALYSIS_DRAG_NONE;
    if (!dragging && (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height))
        return false;
    float doc[2];
    if (pc_ui_canvas_to_doc(xf, x, y, doc) != PC_OK)
        return dragging;
    unsigned mods = pc_ui_shell_input_modifiers(app);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_ui_shell_app_pointer(app,
                                           down     ? PC_UI_POINTER_DOWN
                                           : moving ? PC_UI_POINTER_MOVE
                                                    : PC_UI_POINTER_UP,
                                           (pc_ui_tool_point){.x = doc[0], .y = doc[1], .pressure = 1}, mods,
                                           &out, &err);
    pc_json_value_fini(&out);
    if (rc != PC_OK && rc != PC_ERR_NOT_FOUND)
        pc_ui_shell_report(app, "Analysis", err.msg, true);
    if (!moving && rc == PC_OK)
        pc_ui_shell_app_changed(app);
    return rc != PC_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ options bars */

static void ruler_readout_field(struct nk_context *ctx, const pc_ui_tokens *t, const char *label,
                                double value, bool present)
{
    nk_layout_row_push(ctx, 22);
    nk_label_colored(ctx, label, NK_TEXT_LEFT, pc_ui_nk_color(t->text_dim));
    nk_layout_row_push(ctx, 58);
    char buf[32];
    if (present && isfinite(value))
        (void)snprintf(buf, sizeof buf, "%.2f", value);
    else
        buf[0] = '\0';
    nk_label(ctx, buf, NK_TEXT_LEFT);
}

static bool color_combo(struct nk_context *ctx, float rgb[3])
{
    struct nk_colorf cf = {rgb[0], rgb[1], rgb[2], 1.0f};
    bool changed = false;
    if (nk_combo_begin_color(ctx, nk_rgb_cf(cf), nk_vec2(220, 170))) {
        nk_layout_row_dynamic(ctx, 110, 1);
        struct nk_colorf nc = nk_color_picker(ctx, cf, NK_RGB);
        if (nc.r != cf.r || nc.g != cf.g || nc.b != cf.b) {
            rgb[0] = nc.r;
            rgb[1] = nc.g;
            rgb[2] = nc.b;
            changed = true;
        }
        nk_combo_end(ctx);
    }
    return changed;
}

static void queue_set_group(pc_ui_shell_app *app, size_t group, const char *key, double value,
                            const float *color)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d)
        return;
    yyjson_mut_val *p = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, p, "group", group);
    if (color) {
        yyjson_mut_val *ca = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, ca, (double)color[0]);
        yyjson_mut_arr_add_real(d, ca, (double)color[1]);
        yyjson_mut_arr_add_real(d, ca, (double)color[2]);
        yyjson_mut_obj_add_val(d, p, "color", ca);
    } else if (key) {
        if (!strcmp(key, "visible"))
            yyjson_mut_obj_add_bool(d, p, "visible", value != 0.0);
        else if (!strcmp(key, "active"))
            yyjson_mut_obj_add_bool(d, p, "active", true);
        else
            yyjson_mut_obj_add_uint(d, p, key, (uint64_t)(value < 0 ? 0 : value));
    }
    pc_ui_shell_queue(app, "count.setGroup", p);
    yyjson_mut_doc_free(d);
}

static void ruler_options(pc_ui_shell_app *app, struct nk_context *ctx, const pc_document *doc)
{
    pc_ui_analysis *a = pc_ui_shell_analysis_runtime(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    pc_ui_ruler_readout r;
    bool has = doc->measurement.has_ruler &&
               pc_ui_analysis_ruler_readout(&doc->measurement.ruler, &doc->measurement.scale,
                                            a->use_measurement_scale, &r);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 18);
    ruler_readout_field(ctx, t, "X:", has ? r.x : 0, has);
    ruler_readout_field(ctx, t, "Y:", has ? r.y : 0, has);
    ruler_readout_field(ctx, t, "W:", has ? r.w : 0, has);
    ruler_readout_field(ctx, t, "H:", has ? r.h : 0, has);
    ruler_readout_field(ctx, t, "A:", has ? r.angle : 0, has);
    ruler_readout_field(ctx, t, "L1:", has ? r.l1 : 0, has);
    ruler_readout_field(ctx, t, "L2:", has ? r.l2 : 0, has && r.has_protractor);
    nk_layout_row_push(ctx, 150);
    nk_bool use_scale = a->use_measurement_scale;
    if (nk_checkbox_label(ctx, pc_i18n_t("Use Measurement Scale"), &use_scale))
        a->use_measurement_scale = use_scale != 0;
    if (!has)
        nk_widget_disable_begin(ctx);
    nk_layout_row_push(ctx, 120);
    pc_ui_shell_widget_mapping(app, "image.analysis.straightenLayer", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Straighten Layer")) && has)
        queue(app, "image.analysis.straightenLayer");
    nk_layout_row_push(ctx, 70);
    if (nk_button_label(ctx, pc_i18n_t("Clear")) && has) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (d) {
            yyjson_mut_val *p = yyjson_mut_obj(d);
            yyjson_mut_obj_add_bool(d, p, "clear", true);
            pc_ui_shell_queue(app, "image.analysis.rulerTool", p);
            yyjson_mut_doc_free(d);
        }
    }
    if (!has)
        nk_widget_disable_end(ctx);
    nk_layout_row_end(ctx);
}

static void count_options(pc_ui_shell_app *app, struct nk_context *ctx, const pc_document *doc)
{
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    const pc_measurement *m = &doc->measurement;
    size_t total = pc_measurement_count_total(m);
    char label[48];
    (void)snprintf(label, sizeof label, "%s %zu", pc_i18n_t("Count:"), total);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 14);
    nk_layout_row_push(ctx, 90);
    nk_label_colored(ctx, label, NK_TEXT_LEFT, pc_ui_nk_color(t->text));
    if (m->ncount_groups > 0) {
        size_t gi = m->active_count_group < m->ncount_groups ? m->active_count_group : 0;
        const pc_count_group *g = &m->count_groups[gi];
        /* Group picker. */
        nk_layout_row_push(ctx, 150);
        const char *names[32];
        size_t ng = m->ncount_groups < 32 ? m->ncount_groups : 32;
        for (size_t i = 0; i < ng; i++)
            names[i] = pc_str_c(&m->count_groups[i].name);
        int sel = nk_combo(ctx, names, (int)ng, (int)gi, 22, nk_vec2(200, 200));
        if (sel >= 0 && (size_t)sel != gi)
            queue_set_group(app, (size_t)sel, "active", 0, NULL);
        /* Visibility. */
        nk_layout_row_push(ctx, 40);
        if (nk_button_label(ctx, g->visible ? pc_i18n_t("Hide") : pc_i18n_t("Show")))
            queue_set_group(app, gi, "visible", g->visible ? 0 : 1, NULL);
        /* Colour. */
        nk_layout_row_push(ctx, 60);
        float rgb[3];
        pc_color_to_rgb(&g->color, rgb);
        if (color_combo(ctx, rgb))
            queue_set_group(app, gi, NULL, 0, rgb);
        /* Delete group. */
        nk_layout_row_push(ctx, 60);
        if (nk_button_label(ctx, pc_i18n_t("Delete"))) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            if (d) {
                yyjson_mut_val *p = yyjson_mut_obj(d);
                yyjson_mut_obj_add_uint(d, p, "group", gi);
                pc_ui_shell_queue(app, "count.deleteGroup", p);
                yyjson_mut_doc_free(d);
            }
        }
        /* Marker / label size. */
        nk_layout_row_push(ctx, 150);
        int marker = (int)g->marker_size;
        nk_property_int(ctx, pc_i18n_t("Marker"), 1, &marker, 10, 1, 1);
        if (marker != (int)g->marker_size)
            queue_set_group(app, gi, "markerSize", (double)marker, NULL);
        nk_layout_row_push(ctx, 150);
        int lab = (int)g->label_size;
        nk_property_int(ctx, pc_i18n_t("Label"), 8, &lab, 72, 1, 1);
        if (lab != (int)g->label_size)
            queue_set_group(app, gi, "labelSize", (double)lab, NULL);
    }
    nk_layout_row_push(ctx, 90);
    if (nk_button_label(ctx, pc_i18n_t("New Group")))
        queue(app, "count.newGroup");
    nk_layout_row_push(ctx, 70);
    if (nk_button_label(ctx, pc_i18n_t("Clear")) && total > 0)
        queue(app, "count.clear");
    nk_layout_row_end(ctx);
}

static void note_options(pc_ui_shell_app *app, struct nk_context *ctx, const pc_document *doc)
{
    pc_ui_analysis *a = pc_ui_shell_analysis_runtime(app);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    nk_layout_row_begin(ctx, NK_STATIC, 24, 8);
    nk_layout_row_push(ctx, 60);
    nk_label_colored(ctx, pc_i18n_t("Author:"), NK_TEXT_LEFT, pc_ui_nk_color(t->text_dim));
    nk_layout_row_push(ctx, 150);
    nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, a->note_author, (int)sizeof a->note_author,
                                   nk_filter_default);
    nk_layout_row_push(ctx, 50);
    nk_label_colored(ctx, pc_i18n_t("Color:"), NK_TEXT_LEFT, pc_ui_nk_color(t->text_dim));
    nk_layout_row_push(ctx, 60);
    (void)color_combo(ctx, a->note_color);
    nk_layout_row_push(ctx, 80);
    if (nk_button_label(ctx, pc_i18n_t("Clear All")) && doc->nnotes > 0) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (d) {
            yyjson_mut_val *p = yyjson_mut_obj(d);
            yyjson_mut_obj_add_bool(d, p, "all", true);
            pc_ui_shell_queue(app, "notes.delete", p);
            yyjson_mut_doc_free(d);
        }
        a->has_note_selected = false;
    }
    nk_layout_row_push(ctx, 110);
    if (nk_button_label(ctx, pc_i18n_t("Notes Panel")))
        a->notes_panel = !a->notes_panel;
    nk_layout_row_end(ctx);
}

bool pc_ui_shell_analysis_options(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!pc_ui_analysis_tool(ui->tool))
        return false;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_document *doc = active_doc(app);
    if (!doc) {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t("Open a document to use this tool."), NK_TEXT_LEFT);
        return true;
    }
    if (ui->tool == PC_TOOL_RULER)
        ruler_options(app, ctx, doc);
    else if (ui->tool == PC_TOOL_COUNT_TOOL)
        count_options(app, ctx, doc);
    else
        note_options(app, ctx, doc);
    return true;
}

/* ------------------------------------------------------------------ canvas overlay */

static void plus_handle(struct nk_command_buffer *canvas, const pc_ui_canvas_xform *xf, float x, float y)
{
    for (int pass = 0; pass < 2; pass++) {
        float w = pass ? 1.0f : 3.0f;
        struct nk_color c = pass ? nk_rgb(255, 255, 255) : nk_rgb(0, 0, 0);
        pc_ui_canvas_ink_line(canvas, xf, x - 5, y, x + 5, y, w, c);
        pc_ui_canvas_ink_line(canvas, xf, x, y - 5, x, y + 5, w, c);
    }
}

void pc_ui_shell_analysis_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    const pc_document *doc = active_doc(app);
    if (!doc)
        return;
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    const pc_measurement *m = &doc->measurement;

    if (ui->tool == PC_TOOL_RULER && m->has_ruler) {
        const pc_ruler *r = &m->ruler;
        float s[2], e[2], q[2];
        bool have_s = pc_ui_canvas_to_screen(xf, (float)r->start[0], (float)r->start[1], s) == PC_OK;
        bool have_e = pc_ui_canvas_to_screen(xf, (float)r->end[0], (float)r->end[1], e) == PC_OK;
        if (have_s && have_e) {
            for (int pass = 0; pass < 2; pass++) {
                float w = pass ? 1.0f : 3.0f;
                struct nk_color c = pass ? nk_rgb(255, 255, 255) : nk_rgba(0, 0, 0, 160);
                pc_ui_canvas_ink_line(canvas, xf, s[0], s[1], e[0], e[1], w, c);
            }
            plus_handle(canvas, xf, s[0], s[1]);
            plus_handle(canvas, xf, e[0], e[1]);
            if (r->has_protractor &&
                pc_ui_canvas_to_screen(xf, (float)r->protractor[0], (float)r->protractor[1], q) == PC_OK) {
                for (int pass = 0; pass < 2; pass++) {
                    float w = pass ? 1.0f : 3.0f;
                    struct nk_color c = pass ? nk_rgb(255, 255, 255) : nk_rgba(0, 0, 0, 160);
                    pc_ui_canvas_ink_line(canvas, xf, s[0], s[1], q[0], q[1], w, c);
                }
                plus_handle(canvas, xf, q[0], q[1]);
            }
        }
    }

    /* Count markers. */
    for (size_t gi = 0; gi < m->ncount_groups; gi++) {
        const pc_count_group *g = &m->count_groups[gi];
        if (!g->visible)
            continue;
        float rgb[3];
        pc_color_to_rgb(&g->color, rgb);
        struct nk_color c = rgb_nk(rgb);
        float radius = 1.0f + (float)g->marker_size;
        for (size_t pi = 0; pi < g->npoints; pi++) {
            float p[2];
            if (pc_ui_canvas_to_screen(xf, (float)g->points[pi][0], (float)g->points[pi][1], p) != PC_OK ||
                !pc_ui_canvas_ink_visible(xf, p[0], p[1], radius + 20))
                continue;
            nk_fill_circle(canvas, nk_rect(p[0] - radius, p[1] - radius, radius * 2, radius * 2), c);
            char num[16];
            int n = snprintf(num, sizeof num, "%zu", pi + 1);
            if (n > 0)
                nk_draw_text(canvas, nk_rect(p[0] + radius + 2, p[1] - (float)g->label_size, 40,
                                             (float)g->label_size + 2),
                             num, n, ctx->style.font, nk_rgba(0, 0, 0, 0), c);
        }
    }

    /* Note icons (screen-fixed 16x20 size). */
    if (doc->nnotes > 0) {
        pc_ui_analysis *a = pc_ui_shell_analysis_runtime(app);
        for (size_t i = 0; i < doc->nnotes; i++) {
            float p[2];
            if (pc_ui_canvas_to_screen(xf, (float)doc->notes[i].position[0],
                                       (float)doc->notes[i].position[1], p) != PC_OK ||
                !pc_ui_canvas_ink_visible(xf, p[0] + 8, p[1] + 10, 16))
                continue;
            float rgb[3];
            pc_color_to_rgb(&doc->notes[i].color, rgb);
            struct nk_rect box = nk_rect(p[0], p[1], 16, 20);
            nk_fill_rect(canvas, box, 2, rgb_nk(rgb));
            bool selected = a->has_note_selected && a->note_selected == i;
            nk_stroke_rect(canvas, box, 2, selected ? 2.0f : 1.0f,
                           selected ? nk_rgb(255, 255, 255) : nk_rgba(0, 0, 0, 200));
            struct nk_color ink = nk_rgba(0, 0, 0, 140);
            nk_stroke_line(canvas, p[0] + 11, p[1], p[0] + 16, p[1] + 5, 1.0f, ink);
            for (int k = 0; k < 3; k++) {
                float y = p[1] + 7 + 4 * (float)k;
                nk_stroke_line(canvas, p[0] + 3, y, p[0] + 13, y, 1.0f, ink);
            }
        }
    }
    (void)tokens;
}

/* ------------------------------------------------------------------ panels and dialogs */

static void measurement_log_window(pc_ui_shell_app *app, struct nk_context *ctx)
{
    pc_ui_analysis *a = pc_ui_shell_analysis_runtime(app);
    if (nk_begin(ctx, "Measurement Log", nk_rect(120, 480, 640, 300),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE |
                     NK_WINDOW_SCALABLE)) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        pc_error err = {0};
        pc_status rc =
            pc_ui_shell_app_invoke(app, "measurementLog.list", NULL, &out, &err);
        yyjson_mut_val *root = rc == PC_OK ? pc_json_value_root(&out) : NULL;
        yyjson_mut_val *rows = root ? yyjson_mut_obj_get(root, "rows") : NULL;
        yyjson_mut_val *columns = root ? yyjson_mut_obj_get(root, "columns") : NULL;

        nk_layout_row_begin(ctx, NK_STATIC, 24, 4);
        nk_layout_row_push(ctx, 170);
        if (nk_button_label(ctx, pc_i18n_t("Record Measurements")))
            pc_ui_shell_queue(app, "image.analysis.recordMeasurements", NULL);
        nk_layout_row_push(ctx, 90);
        if (nk_button_label(ctx, pc_i18n_t("Select All")) && rows && yyjson_mut_is_arr(rows)) {
            size_t i, n = yyjson_mut_arr_size(rows), k = 0;
            uint64_t ids[256];
            yyjson_mut_val *ro;
            yyjson_mut_arr_foreach (rows, i, n, ro) {
                if (k >= 256)
                    break;
                ids[k++] = yyjson_mut_get_uint(yyjson_mut_obj_get(ro, "id"));
            }
            (void)pc_ui_analysis_log_set(a, ids, k);
        }
        nk_layout_row_push(ctx, 100);
        if (nk_button_label(ctx, pc_i18n_t("Deselect All")))
            pc_ui_analysis_log_clear(a);
        nk_layout_row_push(ctx, 80);
        if (nk_button_label(ctx, pc_i18n_t("Delete")) && a->nlog_selected > 0) {
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            if (d) {
                yyjson_mut_val *p = yyjson_mut_obj(d);
                yyjson_mut_val *arr = yyjson_mut_arr(d);
                for (size_t i = 0; i < a->nlog_selected; i++)
                    yyjson_mut_arr_add_uint(d, arr, a->log_selected[i]);
                yyjson_mut_obj_add_val(d, p, "rows", arr);
                pc_ui_shell_queue(app, "measurementLog.delete", p);
                yyjson_mut_doc_free(d);
            }
            pc_ui_analysis_log_clear(a);
        }
        nk_layout_row_end(ctx);

        /* Column count. */
        size_t ncol = 0;
        const char *colkeys[32];
        if (columns && yyjson_mut_is_arr(columns)) {
            size_t i, n = yyjson_mut_arr_size(columns);
            yyjson_mut_val *cv;
            yyjson_mut_arr_foreach (columns, i, n, cv) {
                const char *k = yyjson_mut_get_str(cv);
                if (k && strcmp(k, "histogram") && ncol < 32)
                    colkeys[ncol++] = k;
            }
        }
        if (ncol == 0) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label(ctx, pc_i18n_t("No measurements recorded."), NK_TEXT_LEFT);
        } else {
            const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
            nk_layout_row_dynamic(ctx, 22, (int)ncol);
            for (size_t i = 0; i < ncol; i++)
                nk_label_colored(ctx, colkeys[i], NK_TEXT_LEFT, pc_ui_nk_color(t->text_dim));
            if (rows && yyjson_mut_is_arr(rows)) {
                size_t ri, rn = yyjson_mut_arr_size(rows);
                yyjson_mut_val *ro;
                yyjson_mut_arr_foreach (rows, ri, rn, ro) {
                    uint64_t rid = yyjson_mut_get_uint(yyjson_mut_obj_get(ro, "id"));
                    yyjson_mut_val *values = yyjson_mut_obj_get(ro, "values");
                    bool on = pc_ui_analysis_log_contains(a, rid);
                    nk_layout_row_dynamic(ctx, 20, (int)ncol);
                    for (size_t ci = 0; ci < ncol; ci++) {
                        char cell[96];
                        yyjson_mut_val *v = values ? yyjson_mut_obj_get(values, colkeys[ci]) : NULL;
                        const char *s = v ? yyjson_mut_get_str(v) : NULL;
                        if (s)
                            (void)snprintf(cell, sizeof cell, "%s", s);
                        else if (v && yyjson_mut_is_num(v))
                            (void)snprintf(cell, sizeof cell, "%.4g", yyjson_mut_get_num(v));
                        else
                            cell[0] = '\0';
                        if (ci == 0) {
                            nk_bool sel = on;
                            if (nk_selectable_label(ctx, cell[0] ? cell : "-", NK_TEXT_LEFT, &sel) &&
                                (sel != 0) != on)
                                (void)pc_ui_analysis_log_toggle(a, rid);
                        } else {
                            nk_label(ctx, cell, NK_TEXT_LEFT);
                        }
                    }
                }
            }
        }
        pc_json_value_fini(&out);
    }
    nk_end(ctx);
    if (nk_window_is_closed(ctx, "Measurement Log")) {
        a->measurement_log = false;
        nk_window_show(ctx, "Measurement Log", NK_SHOWN);
    }
}

static void notes_window(pc_ui_shell_app *app, struct nk_context *ctx)
{
    pc_ui_analysis *a = pc_ui_shell_analysis_runtime(app);
    const pc_document *doc = active_doc(app);
    size_t n = doc ? doc->nnotes : 0;
    size_t sel = (a->has_note_selected && a->note_selected < n) ? a->note_selected : 0;
    if (nk_begin(ctx, "Notes", nk_rect(620, 320, 280, 260),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE)) {
        if (n == 0) {
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_label_wrap(ctx, pc_i18n_t("Click with the Note tool to add a note."));
        } else {
            const pc_note *note = &doc->notes[sel];
            nk_layout_row_dynamic(ctx, 20, 1);
            nk_label(ctx, note->author.ptr && note->author.len ? pc_str_c(&note->author)
                                                               : pc_i18n_t("No author"),
                     NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 110, 1);
            char text[4096];
            (void)snprintf(text, sizeof text, "%s", pc_str_c(&note->text));
            nk_flags evt = nk_edit_string_zero_terminated(ctx, NK_EDIT_BOX, text, (int)sizeof text,
                                                          nk_filter_default);
            if (evt & NK_EDIT_COMMITED) {
                yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                if (d) {
                    yyjson_mut_val *p = yyjson_mut_obj(d);
                    yyjson_mut_obj_add_uint(d, p, "index", sel);
                    yyjson_mut_obj_add_strcpy(d, p, "text", text);
                    pc_ui_shell_queue(app, "notes.set", p);
                    yyjson_mut_doc_free(d);
                }
            }
            nk_layout_row_begin(ctx, NK_STATIC, 24, 4);
            nk_layout_row_push(ctx, 40);
            if (nk_button_label(ctx, "<") && n > 1) {
                sel = (sel + n - 1) % n;
                a->has_note_selected = true;
                a->note_selected = sel;
            }
            nk_layout_row_push(ctx, 40);
            if (nk_button_label(ctx, ">") && n > 1) {
                sel = (sel + 1) % n;
                a->has_note_selected = true;
                a->note_selected = sel;
            }
            nk_layout_row_push(ctx, 80);
            char pos[48];
            (void)snprintf(pos, sizeof pos, "%zu / %zu", sel + 1, n);
            nk_label(ctx, pos, NK_TEXT_CENTERED);
            nk_layout_row_push(ctx, 70);
            if (nk_button_label(ctx, pc_i18n_t("Delete"))) {
                yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                if (d) {
                    yyjson_mut_val *p = yyjson_mut_obj(d);
                    yyjson_mut_obj_add_uint(d, p, "index", sel);
                    pc_ui_shell_queue(app, "notes.delete", p);
                    yyjson_mut_doc_free(d);
                }
                a->has_note_selected = n > 1;
                a->note_selected = sel > 0 ? sel - 1 : 0;
            }
            nk_layout_row_end(ctx);
        }
    }
    nk_end(ctx);
    if (nk_window_is_closed(ctx, "Notes")) {
        a->notes_panel = false;
        nk_window_show(ctx, "Notes", NK_SHOWN);
    }
}

static void dialog_buttons(pc_ui_shell_app *app, struct nk_context *ctx, pc_ui_analysis *a, bool *ok,
                           bool *cancel)
{
    (void)app;
    nk_layout_row_begin(ctx, NK_STATIC, 28, 2);
    nk_layout_row_push(ctx, 80);
    if (nk_button_label(ctx, pc_i18n_t("Cancel")))
        *cancel = true;
    nk_layout_row_push(ctx, 80);
    if (nk_button_label(ctx, pc_i18n_t("OK")))
        *ok = true;
    nk_layout_row_end(ctx);
    (void)a;
}

static void scale_dialog(pc_ui_shell_app *app, struct nk_context *ctx, pc_ui_analysis *a)
{
    bool ok = false, cancel = false;
    if (nk_begin(ctx, "Measurement Scale", nk_rect(360, 260, 320, 240),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 24, 2);
        nk_bool custom = !a->scale_default;
        if (nk_option_label(ctx, pc_i18n_t("Custom"), custom))
            a->scale_default = false;
        if (nk_option_label(ctx, pc_i18n_t("Default"), !custom))
            a->scale_default = true;
        if (a->scale_default)
            nk_widget_disable_begin(ctx);
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_property_double(ctx, pc_i18n_t("Pixel Length"), 0.001, &a->scale_pixel, 1e7, 1, 0.5f);
        nk_property_double(ctx, pc_i18n_t("Logical Length"), 0.0001, &a->scale_logical, 1e7, 1, 0.5f);
        nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, a->scale_units, (int)sizeof a->scale_units,
                                       nk_filter_default);
        if (a->scale_default)
            nk_widget_disable_end(ctx);
        dialog_buttons(app, ctx, a, &ok, &cancel);
    }
    nk_end(ctx);
    if (ok) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (d) {
            yyjson_mut_val *p = yyjson_mut_obj(d);
            if (a->scale_default) {
                yyjson_mut_obj_add_strcpy(d, p, "preset", "default");
            } else {
                yyjson_mut_obj_add_strcpy(d, p, "preset", "custom");
                yyjson_mut_obj_add_real(d, p, "pixelLength", a->scale_pixel);
                yyjson_mut_obj_add_real(d, p, "logicalLength", a->scale_logical);
                yyjson_mut_obj_add_strcpy(d, p, "units", a->scale_units[0] ? a->scale_units : "pixels");
            }
            pc_ui_shell_queue(app, "image.analysis.setMeasurementScale", p);
            yyjson_mut_doc_free(d);
        }
        a->dialog = PC_UI_ANALYSIS_DIALOG_NONE;
    } else if (cancel) {
        a->dialog = PC_UI_ANALYSIS_DIALOG_NONE;
    }
}

static void marker_dialog(pc_ui_shell_app *app, struct nk_context *ctx, pc_ui_analysis *a)
{
    bool ok = false, cancel = false;
    if (nk_begin(ctx, "Measurement Scale Marker", nk_rect(360, 240, 320, 260),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE)) {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_property_double(ctx, pc_i18n_t("Length"), 0.0001, &a->marker_length, 1e7, 1, 0.5f);
        nk_property_double(ctx, pc_i18n_t("Font Size"), 1, &a->marker_font, 1000, 1, 0.5f);
        nk_bool show = a->marker_display_text;
        if (nk_checkbox_label(ctx, pc_i18n_t("Display Text"), &show))
            a->marker_display_text = show != 0;
        nk_layout_row_dynamic(ctx, 24, 2);
        if (nk_option_label(ctx, pc_i18n_t("Bottom"), a->marker_text_position == 0))
            a->marker_text_position = 0;
        if (nk_option_label(ctx, pc_i18n_t("Top"), a->marker_text_position == 1))
            a->marker_text_position = 1;
        if (nk_option_label(ctx, pc_i18n_t("Black"), a->marker_color == 0))
            a->marker_color = 0;
        if (nk_option_label(ctx, pc_i18n_t("White"), a->marker_color == 1))
            a->marker_color = 1;
        dialog_buttons(app, ctx, a, &ok, &cancel);
    }
    nk_end(ctx);
    if (ok) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (d) {
            yyjson_mut_val *p = yyjson_mut_obj(d);
            yyjson_mut_obj_add_real(d, p, "length", a->marker_length);
            yyjson_mut_obj_add_real(d, p, "fontSize", a->marker_font);
            yyjson_mut_obj_add_bool(d, p, "displayText", a->marker_display_text);
            yyjson_mut_obj_add_strcpy(d, p, "textPosition", a->marker_text_position ? "top" : "bottom");
            yyjson_mut_obj_add_strcpy(d, p, "color", a->marker_color ? "white" : "black");
            pc_ui_shell_queue(app, "image.analysis.placeScaleMarker", p);
            yyjson_mut_doc_free(d);
        }
        a->dialog = PC_UI_ANALYSIS_DIALOG_NONE;
    } else if (cancel) {
        a->dialog = PC_UI_ANALYSIS_DIALOG_NONE;
    }
}

static void data_points_dialog(pc_ui_shell_app *app, struct nk_context *ctx, pc_ui_analysis *a)
{
    bool ok = false, cancel = false;
    if (nk_begin(ctx, "Select Data Points", nk_rect(340, 180, 360, 420),
                 NK_WINDOW_TITLE | NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE)) {
        const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
        static const char *const sources[3] = {"selection", "ruler", "count"};
        static const char *const titles[3] = {"Selections", "Ruler Tool", "Count Tool"};
        for (size_t si = 0; si < 3; si++) {
            bool any = false;
            for (size_t i = 0; i < a->dp_nrows; i++)
                if (!strcmp(a->dp_rows[i].source, sources[si])) {
                    any = true;
                    break;
                }
            if (!any)
                continue;
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label_colored(ctx, pc_i18n_t(titles[si]), NK_TEXT_LEFT, pc_ui_nk_color(t->accent));
            for (size_t i = 0; i < a->dp_nrows; i++) {
                pc_ui_analysis_dp_row *row = &a->dp_rows[i];
                if (strcmp(row->source, sources[si]))
                    continue;
                nk_layout_row_dynamic(ctx, 20, 1);
                nk_bool on = row->selected;
                if (nk_checkbox_label(ctx, row->name[0] ? row->name : row->key, &on))
                    row->selected = on != 0;
            }
        }
        dialog_buttons(app, ctx, a, &ok, &cancel);
    }
    nk_end(ctx);
    if (ok) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (d) {
            yyjson_mut_val *p = yyjson_mut_obj(d);
            static const char *const sources[3] = {"selection", "ruler", "count"};
            for (size_t si = 0; si < 3; si++) {
                yyjson_mut_val *arr = yyjson_mut_arr(d);
                for (size_t i = 0; i < a->dp_nrows; i++)
                    if (a->dp_rows[i].selected && !strcmp(a->dp_rows[i].source, sources[si]))
                        yyjson_mut_arr_add_strcpy(d, arr, a->dp_rows[i].key);
                yyjson_mut_obj_add_val(d, p, sources[si], arr);
            }
            pc_ui_shell_queue(app, "image.analysis.selectDataPoints", p);
            yyjson_mut_doc_free(d);
        }
        a->dialog = PC_UI_ANALYSIS_DIALOG_NONE;
    } else if (cancel) {
        a->dialog = PC_UI_ANALYSIS_DIALOG_NONE;
    }
}

void pc_ui_shell_analysis_windows(pc_ui_shell_app *app)
{
    pc_ui_analysis *a = pc_ui_shell_analysis_runtime(app);
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (a->measurement_log)
        measurement_log_window(app, ctx);
    if (a->notes_panel)
        notes_window(app, ctx);
    switch (a->dialog) {
    case PC_UI_ANALYSIS_DIALOG_SCALE:
        scale_dialog(app, ctx, a);
        break;
    case PC_UI_ANALYSIS_DIALOG_SCALE_MARKER:
        marker_dialog(app, ctx, a);
        break;
    case PC_UI_ANALYSIS_DIALOG_DATA_POINTS:
        data_points_dialog(app, ctx, a);
        break;
    case PC_UI_ANALYSIS_DIALOG_NONE:
        break;
    }
}
