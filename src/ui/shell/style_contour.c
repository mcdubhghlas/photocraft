#include "ui/i18n.h"
#include "layer_style_ui.h"
#include "shell_internal.h"
#include "ui/widgets.h"
#include "engine/style_editor.h"
#include "base/hash.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static void put(pc_ui_dialog *d, yyjson_mut_val *o, const char *key, yyjson_mut_val *v)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(d->fields, key);
    if (k && v)
        (void)yyjson_mut_obj_put(o, k, v);
}
void pc_ui_shell_style_contour(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog,
                               yyjson_mut_val *entry, const char *key)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields),
                   *params = yyjson_mut_obj_get(entry, "params"), *original = yyjson_mut_obj_get(params, key),
                   *value =
                       original ? original : yyjson_mut_obj_get(yyjson_mut_obj_get(entry, "values"), key),
                   *editor = yyjson_mut_obj_get(root, "__contourEditor");
    const char *id = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "id")),
               *target = yyjson_mut_get_str(yyjson_mut_obj_get(editor, "effect")),
               *property = yyjson_mut_get_str(yyjson_mut_obj_get(editor, "key"));
    bool active = id && target && property && !strcmp(id, target) && !strcmp(key, property);
    nk_layout_row_dynamic(ctx, 26, 1);
    char mapping[96];
    (void)snprintf(mapping, sizeof mapping, "style.%s.edit", key);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    if (!active && nk_button_label(ctx, pc_i18n_t("Edit Contour..."))) {
        editor = yyjson_mut_obj(dialog->fields);
        bool ok = editor && id && yyjson_mut_obj_add_strcpy(dialog->fields, editor, "effect", id) &&
                  yyjson_mut_obj_add_strcpy(dialog->fields, editor, "key", key) &&
                  yyjson_mut_obj_add_bool(dialog->fields, editor, "hadOverride", original != NULL) &&
                  yyjson_mut_obj_add_val(dialog->fields, editor, "original",
                                         original ? yyjson_mut_val_mut_copy(dialog->fields, original)
                                                  : yyjson_mut_null(dialog->fields));
        if (ok) {
            put(dialog, root, "__contourEditor", editor);
            active = true;
            r->curve_valid = false;
        }
    }
    if (!active)
        return;
    pc_buf json = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    uint8_t hash[32] = {0};
    pc_status status = value ? pc_json_write(value, 0, &json, NULL) : PC_OK;
    if (status == PC_OK && value) {
        pc_blake3(json.data, json.len, hash);
        status = pc_json_parse(pc_buf_cstr(&json), json.len, &read, NULL);
    }
    yyjson_mut_val *levels = NULL, *flags = NULL;
    float samples[256];
    if (status == PC_OK)
        status = pc_engine_style_contour_view(read ? yyjson_doc_get_root(read) : NULL, dialog->fields,
                                              &levels, &flags, samples, NULL);
    if (status == PC_OK &&
        (!r->curve_valid || memcmp(r->curve_hash, hash, 32) || strcmp(r->curve_key, key))) {
        status = pc_ui_point_curve_load(&r->curve, levels, NULL);
        if (status == PC_OK) {
            memcpy(r->curve_hash, hash, 32);
            (void)snprintf(r->curve_key, sizeof r->curve_key, "%s", key);
            r->curve_valid = true;
        }
    }
    yyjson_doc_free(read);
    pc_buf_fini(&json);
    bool changed = false;
    if (status == PC_OK) {
        nk_layout_row_dynamic(ctx, 180, 1);
        struct nk_rect graph;
        enum nk_widget_layout_states layout = nk_widget(&graph, ctx);
        graph.w = fminf(graph.w, graph.h);
        (void)snprintf(mapping, sizeof mapping, "style.%s.graph", key);
        pc_ui_shell_widget_mapping(app, mapping, graph);
        struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
        nk_fill_rect(canvas, graph, 0, pc_ui_nk_color(tokens->field));
        for (size_t i = 1; i < 4; i++) {
            float f = (float)i / 4;
            nk_stroke_line(canvas, graph.x + f * graph.w, graph.y, graph.x + f * graph.w, graph.y + graph.h,
                           1, pc_ui_nk_color(tokens->separator));
            nk_stroke_line(canvas, graph.x, graph.y + f * graph.h, graph.x + graph.w, graph.y + f * graph.h,
                           1, pc_ui_nk_color(tokens->separator));
        }
        for (size_t i = 1; i < 256; i++)
            nk_stroke_line(canvas, graph.x + (float)(i - 1) / 255 * graph.w,
                           graph.y + graph.h * (1 - samples[i - 1]), graph.x + (float)i / 255 * graph.w,
                           graph.y + graph.h * (1 - samples[i]), 2, pc_ui_nk_color(tokens->accent));
        nk_stroke_rect(canvas, graph, 0, 1, pc_ui_nk_color(tokens->field_border));
        float rect[4] = {graph.x, graph.y, graph.w, graph.h};
        const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT],
                                     *right = &ctx->input.mouse.buttons[NK_BUTTON_RIGHT];
        bool inside = nk_input_is_mouse_hovering_rect(&ctx->input, graph);
        unsigned mods = pc_ui_shell_input_modifiers(app);
        if (!r->graph_blocked && layout == NK_WIDGET_VALID && inside && left->clicked && left->down)
            changed =
                pc_ui_point_curve_pointer(&r->curve, rect, 0, left->clicked_pos.x, left->clicked_pos.y, mods)
                    .changed;
        if (!r->graph_blocked && layout == NK_WIDGET_VALID && inside && right->clicked)
            changed = pc_ui_point_curve_pointer(&r->curve, rect, 3, right->clicked_pos.x,
                                                right->clicked_pos.y, mods)
                          .changed ||
                      changed;
        if (r->curve.dragging)
            changed = pc_ui_point_curve_pointer(&r->curve, rect, left->down ? 1u : 2u, ctx->input.mouse.pos.x,
                                                ctx->input.mouse.pos.y, mods)
                          .changed ||
                      changed;
        for (size_t i = 0; i < r->curve.count; i++) {
            float x = graph.x + (float)(r->curve.points[i][0] / 255) * graph.w,
                  y = graph.y + graph.h * (1 - (float)(r->curve.points[i][1] / 255));
            struct nk_rect point = nk_rect(x - 3, y - 3, 6, 6);
            if (r->curve.has_selected && r->curve.selected == i)
                nk_fill_circle(canvas, point, pc_ui_nk_color(tokens->accent));
            else
                nk_stroke_circle(canvas, point, 1, pc_ui_nk_color(tokens->text));
        }
        bool corner_changed = false;
        nk_bool corner = false;
        if (r->curve.has_selected && r->curve.selected < r->curve.count) {
            size_t selected = r->curve.selected;
            double x = r->curve.points[selected][0], y = r->curve.points[selected][1], oldx = x, oldy = y;
            nk_layout_row_dynamic(ctx, 26, 2);
            nk_property_double(ctx, "Input", 0, &x, 255, 1, 1);
            nk_property_double(ctx, "Output", 0, &y, 255, 1, 1);
            if (x != oldx || y != oldy)
                changed = pc_ui_point_curve_move(&r->curve, selected, x, y) || changed;
            corner = yyjson_mut_get_bool(yyjson_mut_arr_get(flags, selected));
            nk_layout_row_dynamic(ctx, 26, 1);
            (void)snprintf(mapping, sizeof mapping, "style.%s.corner", key);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            corner_changed = nk_checkbox_label(ctx, pc_i18n_t("Corner"), &corner) != 0;
        }
        if (changed || corner_changed) {
            yyjson_mut_val *custom = yyjson_mut_obj(dialog->fields), *points = yyjson_mut_arr(dialog->fields),
                           *corners = yyjson_mut_arr(dialog->fields);
            bool ok = custom && points && corners &&
                      yyjson_mut_obj_add_str(dialog->fields, custom, "name", "Custom");
            for (size_t i = 0; ok && i < r->curve.count; i++) {
                yyjson_mut_val *pair = yyjson_mut_arr(dialog->fields);
                bool flag = false;
                for (size_t j = 0; j < yyjson_mut_arr_size(levels); j++)
                    if (fabs(yyjson_mut_get_num(yyjson_mut_arr_get(yyjson_mut_arr_get(levels, j), 0)) -
                             r->curve.points[i][0]) < 1e-5)
                        flag = yyjson_mut_get_bool(yyjson_mut_arr_get(flags, j));
                if (r->curve.count == yyjson_mut_arr_size(flags))
                    flag = yyjson_mut_get_bool(yyjson_mut_arr_get(flags, i));
                if (r->curve.has_selected && i == r->curve.selected && corner_changed)
                    flag = corner != 0;
                ok = pair && yyjson_mut_arr_add_real(dialog->fields, pair, r->curve.points[i][0] / 255) &&
                     yyjson_mut_arr_add_real(dialog->fields, pair, r->curve.points[i][1] / 255) &&
                     yyjson_mut_arr_add_val(points, pair) &&
                     yyjson_mut_arr_add_bool(dialog->fields, corners, flag);
            }
            ok = ok && yyjson_mut_obj_add_val(dialog->fields, custom, "points", points) &&
                 yyjson_mut_obj_add_val(dialog->fields, custom, "corners", corners);
            if (ok) {
                put(dialog, params, key, custom);
                pc_buf encoded = PC_BUF_INIT;
                if (pc_json_write(custom, 0, &encoded, NULL) == PC_OK)
                    pc_blake3(encoded.data, encoded.len, r->curve_hash);
                pc_buf_fini(&encoded);
            }
        }
    } else {
        nk_layout_row_dynamic(ctx, 40, 1);
        nk_label_wrap(ctx, pc_i18n_t("This imported contour has more points than the interactive editor supports."));
    }
    nk_layout_row_dynamic(ctx, 26, 2);
    (void)snprintf(mapping, sizeof mapping, "style.%s.done", key);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    bool done = nk_button_label(ctx, pc_i18n_t("Done"));
    (void)snprintf(mapping, sizeof mapping, "style.%s.cancel", key);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    bool cancel = nk_button_label(ctx, pc_i18n_t("Revert Contour"));
    if (cancel) {
        if (yyjson_mut_get_bool(yyjson_mut_obj_get(editor, "hadOverride")))
            put(dialog, params, key,
                yyjson_mut_val_mut_copy(dialog->fields, yyjson_mut_obj_get(editor, "original")));
        else
            (void)yyjson_mut_obj_remove_str(params, key);
    }
    if (cancel || done) {
        (void)yyjson_mut_obj_remove_str(root, "__contourEditor");
        r->curve_valid = false;
    }
}
