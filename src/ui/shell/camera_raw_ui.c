#include "ui/i18n.h"
#include "camera_raw_ui.h"
#include "filter_ui.h"
#include "shell_internal.h"
#include "ui/widgets.h"
#include "base/hash.h"
#include "algo/camera_raw.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static void put(pc_ui_dialog *d, yyjson_mut_val *o, const char *key, yyjson_mut_val *v)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(d->fields, key);
    if (k && v)
        (void)yyjson_mut_obj_put(o, k, v);
}
static size_t page(pc_ui_dialog *d)
{
    size_t p =
        (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_doc_get_root(d->fields), "__cameraPage"));
    return p < 7 ? p : 0;
}
bool pc_ui_shell_camera_raw_hidden(pc_ui_dialog *dialog, const char *command, const char *key)
{
    if (!command || strcmp(command, "filter.cameraRaw"))
        return false;
    size_t group = 0;
    if (!strcmp(key, "temperature") || !strcmp(key, "tint") || !strcmp(key, "vibrance") ||
        !strcmp(key, "saturation"))
        group = 1;
    else if (!strcmp(key, "texture") || !strcmp(key, "clarity") || !strcmp(key, "dehaze") ||
             !strncmp(key, "grain", 5) || !strncmp(key, "vignette", 8))
        group = 2;
    else if (!strncmp(key, "curve", 5) || !strncmp(key, "pointCurve", 10))
        group = 3;
    else if (!strncmp(key, "hsl", 3))
        group = 4;
    else if (!strncmp(key, "grade", 5))
        group = 5;
    else if (!strncmp(key, "sharpen", 7) || !strncmp(key, "noise", 5))
        group = 6;
    else if (!strcmp(key, "seed"))
        group = 2;
    return group != page(dialog);
}
static int choice(pc_ui_shell_app *app, const char *id, const char *const *labels, size_t count, int old)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (old < 0 || (size_t)old >= count)
        old = 0;
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    int next = old;
    if (nk_combo_begin_label(ctx, pc_i18n_t(labels[old]), nk_vec2(250, 260))) {
        pc_ui_shell_dialog_runtime(app)->graph_blocked = true;
        nk_layout_row_dynamic(ctx, 26, 1);
        for (size_t i = 0; i < count; i++) {
            char mapping[128];
            (void)snprintf(mapping, sizeof mapping, "%s.%zu", id, i);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            if (nk_button_label(ctx, pc_i18n_t(labels[i]))) {
                next = (int)i;
                nk_combo_close(ctx);
            }
        }
        nk_combo_end(ctx);
    }
    return next;
}
static void array_controls(pc_ui_shell_app *app, pc_ui_dialog *d, const char *key, const char *const *labels,
                           size_t count, double min, double max, const double *defaults)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields), *old = yyjson_mut_obj_get(root, key),
                   *next = yyjson_mut_arr(d->fields);
    bool changed = false, ok = next != NULL;
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_val *v = yyjson_mut_arr_get(old, i);
        double value = yyjson_mut_is_num(v) && isfinite(yyjson_mut_get_num(v)) ? yyjson_mut_get_num(v)
                       : defaults                                              ? defaults[i]
                                                                               : 0,
               number = value;
        nk_layout_row_dynamic(ctx, 26, 1);
        char mapping[128];
        (void)snprintf(mapping, sizeof mapping, "camera.%s.%zu", key, i);
        pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
        double lo = min, hi = max;
        if (!strcmp(key, "curveSplits")) {
            if (i > 0)
                lo = fmax(lo, yyjson_mut_get_num(yyjson_mut_arr_get(next, i - 1)) + 1);
            if (i + 1 < count) {
                yyjson_mut_val *following = yyjson_mut_arr_get(old, i + 1);
                hi = fmin(
                    hi, (yyjson_mut_is_num(following) ? yyjson_mut_get_num(following) : defaults[i + 1]) - 1);
            }
        }
        nk_property_double(ctx, labels[i], lo, &number, fmax(lo, hi), 1, .1f);
        changed = changed || number != value;
        ok = ok && yyjson_mut_arr_add_real(d->fields, next, number);
    }
    if (changed && ok)
        put(d, root, key, next);
}
static void curve(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields);
    const char *labels[] = {"Composite", "Red", "Green", "Blue"},
               *keys[] = {"pointCurve", "pointCurveRed", "pointCurveGreen", "pointCurveBlue"};
    size_t channel = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__cameraCurve"));
    if (channel > 3)
        channel = 0;
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t("Point Curve"), NK_TEXT_LEFT);
    int next = choice(app, "camera.curve.channel", labels, 4, (int)channel);
    if ((size_t)next != channel) {
        channel = (size_t)next;
        put(d, root, "__cameraCurve", yyjson_mut_uint(d->fields, channel));
        r->curve_valid = false;
    }
    const char *key = keys[channel];
    yyjson_mut_val *points = yyjson_mut_obj_get(root, key);
    pc_buf encoded = PC_BUF_INIT;
    uint8_t hash[32] = {0};
    if (points && pc_json_write(points, 0, &encoded, NULL) == PC_OK)
        pc_blake3(encoded.data, encoded.len, hash);
    pc_buf_fini(&encoded);
    if (!r->curve_valid || strcmp(r->curve_key, key) || memcmp(r->curve_hash, hash, 32)) {
        r->curve_valid =
            pc_ui_point_curve_load(&r->curve, yyjson_mut_arr_size(points) ? points : NULL, NULL) == PC_OK;
        memcpy(r->curve_hash, hash, 32);
        (void)snprintf(r->curve_key, sizeof r->curve_key, "%s", key);
    }
    if (!r->curve_valid) {
        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label_wrap(ctx, pc_i18n_t("The point curve contains invalid data."));
        return;
    }
    nk_layout_row_dynamic(ctx, 220, 1);
    struct nk_rect graph;
    enum nk_widget_layout_states layout = nk_widget(&graph, ctx);
    graph.w = fminf(graph.w, graph.h);
    pc_ui_shell_widget_mapping(app, "camera.curve.graph", graph);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    nk_fill_rect(canvas, graph, 0, pc_ui_nk_color(tokens->field));
    for (size_t i = 1; i < 4; i++) {
        float f = (float)i / 4;
        nk_stroke_line(canvas, graph.x + graph.w * f, graph.y, graph.x + graph.w * f, graph.y + graph.h, 1,
                       pc_ui_nk_color(tokens->separator));
        nk_stroke_line(canvas, graph.x, graph.y + graph.h * f, graph.x + graph.w, graph.y + graph.h * f, 1,
                       pc_ui_nk_color(tokens->separator));
    }
    float input[PC_UI_CURVE_CAPACITY][2], samples[256];
    for (size_t i = 0; i < r->curve.count; i++)
        for (size_t a = 0; a < 2; a++)
            input[i][a] = (float)r->curve.points[i][a];
    if (pc_algo_cr_curve_lut((const float (*)[2])input, r->curve.count, samples, 256, NULL) == PC_OK)
        for (size_t i = 1; i < 256; i++)
            nk_stroke_line(canvas, graph.x + (float)(i - 1) / 255 * graph.w,
                           graph.y + graph.h * (1 - samples[i - 1]), graph.x + (float)i / 255 * graph.w,
                           graph.y + graph.h * (1 - samples[i]), 2, pc_ui_nk_color(tokens->accent));
    nk_stroke_rect(canvas, graph, 0, 1, pc_ui_nk_color(tokens->field_border));
    float rect[4] = {graph.x, graph.y, graph.w, graph.h};
    bool changed = false;
    unsigned modifiers = pc_ui_shell_input_modifiers(app);
    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT],
                                 *right = &ctx->input.mouse.buttons[NK_BUTTON_RIGHT];
    bool hover = nk_input_is_mouse_hovering_rect(&ctx->input, graph);
    if (!r->graph_blocked && layout == NK_WIDGET_VALID && hover && left->clicked && left->down)
        changed =
            pc_ui_point_curve_pointer(&r->curve, rect, 0, left->clicked_pos.x, left->clicked_pos.y, modifiers)
                .changed;
    if (!r->graph_blocked && layout == NK_WIDGET_VALID && hover && right->clicked)
        changed = pc_ui_point_curve_pointer(&r->curve, rect, 3, right->clicked_pos.x, right->clicked_pos.y,
                                            modifiers)
                      .changed ||
                  changed;
    if (r->curve.dragging)
        changed = pc_ui_point_curve_pointer(&r->curve, rect, left->down ? 1u : 2u, ctx->input.mouse.pos.x,
                                            ctx->input.mouse.pos.y, modifiers)
                      .changed ||
                  changed;
    for (size_t i = 0; i < r->curve.count; i++) {
        float x = graph.x + (float)(r->curve.points[i][0] / 255) * graph.w,
              y = graph.y + graph.h * (1 - (float)(r->curve.points[i][1] / 255));
        if (r->curve.has_selected && r->curve.selected == i)
            nk_fill_circle(canvas, nk_rect(x - 3, y - 3, 6, 6), pc_ui_nk_color(tokens->accent));
        else
            nk_stroke_circle(canvas, nk_rect(x - 3, y - 3, 6, 6), 1, pc_ui_nk_color(tokens->text));
    }
    if (r->curve.has_selected) {
        size_t i = r->curve.selected;
        double x = r->curve.points[i][0], y = r->curve.points[i][1], ox = x, oy = y;
        nk_layout_row_dynamic(ctx, 26, 2);
        nk_property_double(ctx, "Input", 0, &x, 255, 1, 1);
        nk_property_double(ctx, "Output", 0, &y, 255, 1, 1);
        if (x != ox || y != oy)
            changed = pc_ui_point_curve_move(&r->curve, i, x, y) || changed;
    }
    if (changed && pc_ui_point_curve_json(&r->curve, d->fields, &points, NULL) == PC_OK) {
        put(d, root, key, points);
        if (pc_json_write(points, 0, &encoded, NULL) == PC_OK)
            pc_blake3(encoded.data, encoded.len, r->curve_hash);
        pc_buf_fini(&encoded);
    }
    const char *splits[] = {"Shadow Split", "Midtone Split", "Highlight Split"};
    const double defaults[] = {25, 50, 75};
    array_controls(app, d, "curveSplits", splits, 3, 1, 99, defaults);
}
static void structured(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields);
    if (page(d) == 3)
        curve(app, r, d);
    else if (page(d) == 4) {
        const char *labels[] = {"Hue", "Saturation", "Luminance"}, *keys[] = {"hslHue", "hslSat", "hslLum"},
                   *bands[] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
        size_t tab = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__cameraMixer"));
        nk_layout_row_dynamic(ctx, 28, 1);
        int next = choice(app, "camera.mixer", labels, 3, (int)(tab < 3 ? tab : 0));
        put(d, root, "__cameraMixer", yyjson_mut_uint(d->fields, (size_t)next));
        array_controls(app, d, keys[next], bands, 8, -100, 100, NULL);
    } else if (page(d) == 5) {
        const char *labels[] = {"Shadows", "Midtones", "Highlights", "Global"},
                   *keys[] = {"gradeShadows", "gradeMidtones", "gradeHighlights", "gradeGlobal"};
        size_t tab = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__cameraGrade"));
        nk_layout_row_dynamic(ctx, 28, 1);
        int next = choice(app, "camera.grading", labels, 4, (int)(tab < 4 ? tab : 0));
        put(d, root, "__cameraGrade", yyjson_mut_uint(d->fields, (size_t)next));
        yyjson_mut_val *wheel = yyjson_mut_obj_get(root, keys[next]);
        if (!yyjson_mut_is_obj(wheel)) {
            wheel = yyjson_mut_obj(d->fields);
            put(d, root, keys[next], wheel);
        }
        const char *fields[] = {"hue", "sat", "lum"}, *names[] = {"Hue", "Saturation", "Luminance"};
        for (size_t i = 0; i < 3; i++) {
            double value = yyjson_mut_get_num(yyjson_mut_obj_get(wheel, fields[i]));
            if (!isfinite(value))
                value = 0;
            double number = value;
            nk_layout_row_dynamic(ctx, 28, 1);
            char mapping[128];
            (void)snprintf(mapping, sizeof mapping, "camera.%s.%s", keys[next], fields[i]);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            nk_property_double(ctx, names[i], i == 2 ? -100 : 0, &number, i == 0 ? 360 : 100, 1, .1f);
            if (value != number)
                put(d, wheel, fields[i], yyjson_mut_real(d->fields, number));
        }
    }
}
void pc_ui_shell_camera_raw_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields);
    nk_layout_row_begin(ctx, NK_DYNAMIC, 510, 2);
    nk_layout_row_push(ctx, .52f);
    if (nk_group_begin(ctx, "camera-preview", NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_label(ctx, pc_i18n_t("Canvas Preview"), NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 360, 1);
        struct nk_rect bounds;
        (void)nk_widget(&bounds, ctx);
        struct nk_image image;
        float aspect;
        if (pc_ui_shell_canvas_preview(app, &image, &aspect)) {
            float w = fminf(bounds.w, bounds.h * aspect), h = w / aspect;
            struct nk_rect rect = nk_rect(bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2, w, h);
            pc_ui_shell_widget_mapping(app, "camera.preview", rect);
            nk_draw_image(nk_window_get_canvas(ctx), rect, &image, nk_rgba(255, 255, 255, 255));
        }
        if (r->preview.has_histogram) {
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label(ctx, pc_i18n_t("Source Histogram"), NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 80, 1);
            struct nk_rect graph;
            (void)nk_widget(&graph, ctx);
            struct nk_color colors[] = {nk_rgb(240, 90, 90), nk_rgb(80, 230, 110), nk_rgb(90, 140, 240)};
            size_t offset = r->preview.histogram.channels == 4 ? 1 : 0;
            size_t count = r->preview.histogram.channels < 3 ? r->preview.histogram.channels : 3;
            uint32_t max = 1;
            for (size_t c = 0; c < count; c++)
                for (size_t i = 0; i < 256; i++)
                    if (r->preview.histogram.bins[c + offset][i] > max)
                        max = r->preview.histogram.bins[c + offset][i];
            struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
            for (size_t c = 0; c < count; c++)
                for (size_t i = 0; i < 256; i++) {
                    float x = graph.x + (float)i / 255 * graph.w,
                          height = (float)r->preview.histogram.bins[c + offset][i] / (float)max * graph.h;
                    nk_stroke_line(canvas, x, graph.y + graph.h, x, graph.y + graph.h - height, 1, colors[c]);
                }
        }
        nk_group_end(ctx);
    }
    nk_layout_row_push(ctx, .48f);
    if (nk_group_begin(ctx, "camera-controls", NK_WINDOW_BORDER)) {
        const char *pages[] = {"Light",       "Color",         "Effects", "Curve",
                               "Color Mixer", "Color Grading", "Detail"};
        nk_layout_row_dynamic(ctx, 28, 1);
        int selected = (int)page(d), next = choice(app, "camera.page", pages, 7, selected);
        if (next != selected)
            put(d, root, "__cameraPage", yyjson_mut_uint(d->fields, (size_t)next));
        structured(app, r, d);
        pc_ui_shell_filter_fields(app, r, d, "filter.cameraRaw");
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 28, 2);
    nk_bool preview = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "preview"));
    if (nk_checkbox_label(ctx, pc_i18n_t("Preview"), &preview))
        put(d, root, "preview", yyjson_mut_bool(d->fields, preview != 0));
    pc_ui_shell_widget_mapping(app, "camera.reset", nk_widget_bounds(ctx));
    if (nk_button_label(ctx, pc_i18n_t("Reset"))) {
        pc_algo_camera_raw settings;
        pc_algo_camera_raw_default(&settings);
        yyjson_mut_val *defaults = pc_algo_camera_raw_to_json(d->fields, &settings);
        yyjson_mut_obj_iter iter = yyjson_mut_obj_iter_with(defaults);
        yyjson_mut_val *key;
        while ((key = yyjson_mut_obj_iter_next(&iter)))
            put(d, root, yyjson_mut_get_str(key),
                yyjson_mut_val_mut_copy(d->fields, yyjson_mut_obj_iter_get_val(key)));
        pc_algo_camera_raw_fini(&settings);
        r->curve_valid = false;
    }
    nk_layout_row_dynamic(ctx, 24, 1);
    if (pc_ui_filter_preview_busy(&r->preview))
        nk_label(ctx, pc_i18n_t("Updating preview..."), NK_TEXT_LEFT);
    else if (r->preview.has_ready && r->preview.status != PC_OK)
        nk_label_wrap(ctx, r->preview.error.msg);
}
