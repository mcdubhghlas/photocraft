#include "ui/i18n.h"
#include "adjust_controls.h"
#include "shell_internal.h"
#include "ui/filter_dialog.h"
#include "ui/color_picker.h"
#include "ui/widgets.h"
#include "engine/adjust_params.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
static void store(pc_ui_dialog *dialog, const pc_gradient_map_stop *stops, size_t count)
{
    yyjson_mut_doc *doc = dialog->fields;
    yyjson_mut_val *array = yyjson_mut_arr(doc);
    bool ok = array != NULL;
    for (size_t i = 0; ok && i < count; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(doc), *color = yyjson_mut_arr(doc);
        ok = pair && color && yyjson_mut_arr_add_real(doc, pair, stops[i].pos);
        for (size_t c = 0; ok && c < 3; c++)
            ok = yyjson_mut_arr_add_real(doc, color, stops[i].rgb[c]);
        ok = ok && yyjson_mut_arr_add_val(pair, color) && yyjson_mut_arr_add_val(array, pair);
    }
    yyjson_mut_val *key = ok ? yyjson_mut_str(doc, "stops") : NULL;
    if (key)
        (void)yyjson_mut_obj_put(yyjson_mut_doc_get_root(doc), key, array);
}
static void sample(const pc_gradient_map_stop *s, size_t n, float t, float rgb[3])
{
    if (t <= s[0].pos) {
        memcpy(rgb, s[0].rgb, 3 * sizeof(float));
        return;
    }
    for (size_t i = 1; i < n; i++)
        if (t <= s[i].pos) {
            float d = s[i].pos - s[i - 1].pos, u = d > 0 ? (t - s[i - 1].pos) / d : 1;
            for (size_t c = 0; c < 3; c++)
                rgb[c] = s[i - 1].rgb[c] + (s[i].rgb[c] - s[i - 1].rgb[c]) * u;
            return;
        }
    memcpy(rgb, s[n - 1].rgb, 3 * sizeof(float));
}
static size_t reorder(pc_gradient_map_stop *stops, size_t n, size_t index)
{
    while (index && stops[index].pos < stops[index - 1].pos) {
        pc_gradient_map_stop t = stops[index];
        stops[index] = stops[index - 1];
        stops[--index] = t;
    }
    while (index + 1 < n && stops[index].pos > stops[index + 1].pos) {
        pc_gradient_map_stop t = stops[index];
        stops[index] = stops[index + 1];
        stops[++index] = t;
    }
    return index;
}
void pc_ui_shell_adjust_gradient(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *command = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__command"));
    if (!command || strcmp(command, "image.adjustments.gradientMap"))
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_buf json = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    pc_adjustment adjustment = {0};
    pc_error error = {0};
    pc_status status = pc_ui_filter_dialog_params(root, &json, &error);
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&json), json.len, &read, &error);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (status == PC_OK)
        status = pc_engine_adjustment_from_params(
            "gradientMap", yyjson_doc_get_root(read), NULL,
            state ? pc_doc_arc_get(state->doc)->mode : PC_COLOR_MODE_RGB, &adjustment, &error);
    pc_buf_fini(&json);
    yyjson_doc_free(read);
    if (status != PC_OK) {
        nk_layout_row_dynamic(ctx, 36, 1);
        nk_label_wrap(ctx, error.msg);
        return;
    }
    pc_gradient_map_stop stops[64];
    size_t count = adjustment.u.gradient_map.nstops;
    if (count < 2 || count > 64) {
        pc_adjustment_fini(&adjustment);
        return;
    }
    memcpy(stops, adjustment.u.gradient_map.stops, count * sizeof stops[0]);
    pc_adjustment_fini(&adjustment);
    if (count < 2 || count > 64)
        return;
    if (!yyjson_mut_obj_get(root, "stops"))
        store(dialog, stops, count);
    if (r->gradient_stop >= count)
        r->gradient_stop = count - 1;
    nk_layout_row_dynamic(ctx, 48, 1);
    struct nk_rect bar;
    enum nk_widget_layout_states layout = nk_widget(&bar, ctx);
    bar.h -= 12;
    if (bar.w <= 0 || bar.h <= 0)
        return;
    pc_ui_shell_widget_mapping(app, "dialog.gradient", bar);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    for (size_t i = 0; i < 256; i++) {
        float rgb[3];
        sample(stops, count, (float)i / 255, rgb);
        nk_fill_rect(canvas, nk_rect(bar.x + (float)i / 256 * bar.w, bar.y, fmaxf(1, bar.w / 256), bar.h), 0,
                     nk_rgb_f(rgb[0], rgb[1], rgb[2]));
    }
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    for (size_t i = 0; i < count; i++) {
        float x = bar.x + stops[i].pos * bar.w;
        struct nk_color color = pc_ui_nk_color(i == r->gradient_stop ? tokens->accent : tokens->text);
        nk_fill_triangle(canvas, x, bar.y + bar.h + 2, x - 5, bar.y + bar.h + 10, x + 5, bar.y + bar.h + 10,
                         color);
    }
    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
    bool changed = false;
    if (layout != NK_WIDGET_INVALID && left->clicked && !r->gradient_dragging) {
        float x = left->clicked_pos.x, y = left->clicked_pos.y;
        if (y >= bar.y && y <= bar.y + bar.h + 12 && x >= bar.x - 8 && x <= bar.x + bar.w + 8) {
            size_t index = SIZE_MAX;
            float distance = 9;
            for (size_t i = 0; i < count; i++) {
                float d = fabsf(x - bar.x - stops[i].pos * bar.w);
                if (d < distance) {
                    index = i;
                    distance = d;
                }
            }
            if (index == SIZE_MAX && count < 64) {
                index = count++;
                stops[index].pos = fminf(1, fmaxf(0, (x - bar.x) / bar.w));
                sample(stops, count - 1, stops[index].pos, stops[index].rgb);
                changed = true;
            }
            if (index != SIZE_MAX) {
                r->gradient_stop = reorder(stops, count, index);
                r->gradient_dragging = left->down;
            }
        }
    }
    if (r->gradient_dragging) {
        float next = fminf(1, fmaxf(0, (ctx->input.mouse.pos.x - bar.x) / bar.w));
        if (next != stops[r->gradient_stop].pos) {
            stops[r->gradient_stop].pos = next;
            r->gradient_stop = reorder(stops, count, r->gradient_stop);
            changed = true;
        }
        if (!left->down)
            r->gradient_dragging = false;
    }
    char labels[64][64];
    const char *names[64];
    for (size_t i = 0; i < count; i++) {
        (void)snprintf(labels[i], sizeof labels[i], "Stop %zu (%.1f%%)", i + 1, (double)stops[i].pos * 100);
        names[i] = labels[i];
    }
    nk_layout_row_dynamic(ctx, 26, 1);
    int selected = (int)r->gradient_stop,
        next = nk_combo(ctx, names, (int)count, selected, 24, nk_vec2(300, 220));
    if (next >= 0 && (size_t)next < count)
        r->gradient_stop = (size_t)next;
    nk_layout_row_dynamic(ctx, 26, 1);
    double position = (double)stops[r->gradient_stop].pos * 100, old = position;
    pc_ui_shell_widget_mapping(app, "dialog.gradient.position", nk_widget_bounds(ctx));
    nk_property_double(ctx, "Location %", 0, &position, 100, 1, 1);
    if (position != old) {
        stops[r->gradient_stop].pos = (float)(position / 100);
        r->gradient_stop = reorder(stops, count, r->gradient_stop);
        changed = true;
    }
    nk_layout_row_dynamic(ctx, 26, 3);
    pc_ui_shell_widget_mapping(app, "dialog.gradient.color", nk_widget_bounds(ctx));
    bool color = nk_button_color(ctx, nk_rgb_f(stops[r->gradient_stop].rgb[0], stops[r->gradient_stop].rgb[1],
                                               stops[r->gradient_stop].rgb[2]));
    pc_ui_shell_widget_mapping(app, "dialog.gradient.add", nk_widget_bounds(ctx));
    bool add = nk_button_label(ctx, pc_i18n_t("Add"));
    pc_ui_shell_widget_mapping(app, "dialog.gradient.delete", nk_widget_bounds(ctx));
    bool remove = nk_button_label(ctx, pc_i18n_t("Delete"));
    if (add && count < 64) {
        size_t near = r->gradient_stop;
        float t = near + 1 < count ? (stops[near].pos + stops[near + 1].pos) / 2
                  : near           ? (stops[near - 1].pos + stops[near].pos) / 2
                                   : .5f;
        stops[count].pos = t;
        sample(stops, count, t, stops[count].rgb);
        r->gradient_stop = reorder(stops, count + 1, count);
        count++;
        changed = true;
    }
    if (remove && count > 2) {
        size_t i = r->gradient_stop;
        memmove(stops + i, stops + i + 1, (count - i - 1) * sizeof stops[0]);
        count--;
        if (i >= count)
            r->gradient_stop = count - 1;
        changed = true;
    }
    if (changed || color)
        store(dialog, stops, count);
    if (color) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
        if (p && yyjson_mut_obj_add_uint(doc, p, "dialog", dialog->id) &&
            yyjson_mut_obj_add_str(doc, p, "field", "stops") &&
            yyjson_mut_obj_add_uint(doc, p, "stop", r->gradient_stop))
            pc_ui_shell_queue(app, "ui.dialog.colorField", p);
        yyjson_mut_doc_free(doc);
    }
}
