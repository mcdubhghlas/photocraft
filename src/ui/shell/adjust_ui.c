#include "ui/i18n.h"
#include "adjust_ui.h"
#include "shell_internal.h"
#include "ui/widgets.h"
#include "ui/filter_dialog.h"
#include "engine/channel_display.h"
#include "engine/adjust_params.h"
#include "engine/adjust_eyedropper.h"
#include "ui/level_auto.h"
#include "ui/canvas.h"
#include "base/hash.h"
#include "base/buf.h"
#include "base/mem.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
static void set_value(pc_ui_dialog *dialog, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(dialog->fields, key);
    if (k && value)
        (void)yyjson_mut_obj_put(yyjson_mut_doc_get_root(dialog->fields), k, value);
}
static void data_key(yyjson_mut_val *value, uint8_t hash[32])
{
    memset(hash, 0, 32);
    pc_buf text = PC_BUF_INIT;
    if (value && pc_json_write(value, 0, &text, NULL) == PC_OK)
        pc_blake3(text.data, text.len, hash);
    pc_buf_fini(&text);
}
static pc_status change_mode(pc_ui_shell_app *app, pc_ui_dialog *dialog, bool pencil, pc_error *err)
{
    pc_buf json = PC_BUF_INIT;
    yyjson_doc *read = NULL, *patch = NULL;
    pc_adjustment base = {0}, next = {0};
    pc_status status = pc_ui_filter_dialog_params(yyjson_mut_doc_get_root(dialog->fields), &json, err);
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&json), json.len, &read, err);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    pc_color_mode mode = state ? pc_doc_arc_get(state->doc)->mode : PC_COLOR_MODE_RGB;
    if (status == PC_OK)
        status =
            pc_engine_adjustment_from_params("curves", yyjson_doc_get_root(read), NULL, mode, &base, err);
    const char *text = pencil ? "{\"pencil\":true}" : "{\"pencil\":false}";
    if (status == PC_OK)
        status = pc_json_parse(text, strlen(text), &patch, err);
    if (status == PC_OK)
        status =
            pc_engine_adjustment_from_params("curves", yyjson_doc_get_root(patch), &base, mode, &next, err);
    yyjson_mut_val *values = NULL;
    if (status == PC_OK)
        status = pc_engine_adjustment_to_params(&next, dialog->fields, &values, err);
    if (status == PC_OK) {
        const char *clear[] = {"points", "gray", "red",  "green",   "blue",   "lightness",
                               "a",      "b",    "cyan", "magenta", "yellow", "black"};
        yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
        for (size_t i = 0; i < sizeof clear / sizeof *clear; i++)
            (void)yyjson_mut_obj_remove_str(fields, clear[i]);
        set_value(dialog, "pencil", yyjson_mut_bool(dialog->fields, pencil));
        size_t i, max;
        yyjson_mut_val *key, *value;
        yyjson_mut_obj_foreach(values, i, max, key, value)
        {
            set_value(dialog, yyjson_mut_get_str(key), yyjson_mut_val_mut_copy(dialog->fields, value));
        }
    }
    pc_adjustment_fini(&base);
    pc_adjustment_fini(&next);
    yyjson_doc_free(read);
    yyjson_doc_free(patch);
    pc_buf_fini(&json);
    return status;
}
/* Apply a Levels/Curves Set Black/Gray/White Point eyedropper (A35/A36): sample the merged
 * composite at image-space (x, y) through the engine eyedropper primitive, which rewrites the
 * dialog's working adjustment, then fold the resulting params back into the dialog fields so the
 * live preview updates. Returns Err on invalid sample/document; never panics. */
static pc_status apply_eyedropper(pc_ui_shell_app *app, pc_ui_dialog *dialog, const char *kind,
                                  const char *point, double x, double y, pc_error *err)
{
    pc_buf json = PC_BUF_INIT, text = PC_BUF_INIT;
    yyjson_doc *read = NULL, *full = NULL;
    yyjson_mut_doc *md = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *values = NULL;
    pc_adjustment picked = {0};
    bool present = false;
    pc_status status = md ? pc_ui_filter_dialog_params(yyjson_mut_doc_get_root(dialog->fields), &json, err)
                          : pc_fail(err, PC_ERR_NOMEM, "eyedropper request");
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&json), json.len, &read, err);
    if (status == PC_OK) {
        yyjson_val *src = yyjson_doc_get_root(read);
        yyjson_mut_val *robj = yyjson_is_obj(src) ? yyjson_val_mut_copy(md, src) : yyjson_mut_obj(md);
        yyjson_mut_val *eye = yyjson_mut_obj(md), *at = yyjson_mut_arr(md);
        bool built = robj && eye && at && yyjson_mut_obj_add_strcpy(md, eye, "point", point) &&
                     yyjson_mut_arr_add_real(md, at, x) && yyjson_mut_arr_add_real(md, at, y) &&
                     yyjson_mut_obj_add_val(md, eye, "at", at) &&
                     yyjson_mut_obj_add_val(md, robj, "eyedropper", eye);
        if (built)
            yyjson_mut_doc_set_root(md, robj);
        else
            status = pc_fail(err, PC_ERR_NOMEM, "eyedropper request");
    }
    if (status == PC_OK)
        status = pc_json_write(yyjson_mut_doc_get_root(md), 0, &text, err);
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &full, err);
    if (status == PC_OK)
        status = pc_engine_adjustment_eyedropper(pc_ui_shell_app_session(app), kind,
                                                 yyjson_doc_get_root(full), NULL, false, 0, &present, &picked,
                                                 err);
    if (status == PC_OK && present)
        status = pc_engine_adjustment_to_params(&picked, dialog->fields, &values, err);
    if (status == PC_OK && present) {
        const char *clear[] = {"points", "gray",  "red",     "green",    "blue",   "lightness",
                               "a",      "b",     "cyan",    "magenta",  "yellow", "black",
                               "inBlack", "gamma", "inWhite", "outBlack", "outWhite"};
        yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
        for (size_t i = 0; i < sizeof clear / sizeof *clear; i++)
            (void)yyjson_mut_obj_remove_str(fields, clear[i]);
        size_t i, max;
        yyjson_mut_val *k, *v;
        yyjson_mut_obj_foreach(values, i, max, k, v)
        {
            set_value(dialog, yyjson_mut_get_str(k), yyjson_mut_val_mut_copy(dialog->fields, v));
        }
    }
    pc_adjustment_fini(&picked);
    yyjson_doc_free(full);
    yyjson_doc_free(read);
    pc_buf_fini(&text);
    pc_buf_fini(&json);
    yyjson_mut_doc_free(md);
    return status;
}
static void histogram(struct nk_command_buffer *canvas, struct nk_rect r, const uint32_t bins[256],
                      struct nk_color color)
{
    uint32_t maximum = 0;
    for (size_t i = 0; i < 256; i++)
        if (bins[i] > maximum)
            maximum = bins[i];
    if (!maximum)
        return;
    for (size_t i = 0; i < 256; i++) {
        float x = r.x + (float)i / 256 * r.w, height = (float)bins[i] / (float)maximum * r.h;
        if (height > 0)
            nk_fill_rect(canvas, nk_rect(x, r.y + r.h - height, fmaxf(1, r.w / 256), height), 0, color);
    }
}
/* A triangular slider handle whose apex points up at (x, y) with a 9px-tall base below it. */
static void level_handle(struct nk_command_buffer *cb, float x, float y, struct nk_color fill,
                         struct nk_color stroke)
{
    nk_fill_triangle(cb, x, y, x + 5.5f, y + 9, x - 5.5f, y + 9, fill);
    nk_stroke_triangle(cb, x, y, x + 5.5f, y + 9, x - 5.5f, y + 9, 1, stroke);
}

/* The output-ramp "to" colour for a tone channel (black -> this at full output). */
static struct nk_color level_channel_color(const char *key)
{
    if (key && !strcmp(key, "red"))
        return nk_rgb(225, 70, 70);
    if (key && !strcmp(key, "green"))
        return nk_rgb(80, 210, 100);
    if (key && !strcmp(key, "blue"))
        return nk_rgb(90, 130, 255);
    return nk_rgb(255, 255, 255);
}

static void map_line(float values[256], double x0, double y0, double x1, double y1)
{
    int a = (int)fmin(255, fmax(0, round(x0))), b = (int)fmin(255, fmax(0, round(x1)));
    if (a > b) {
        int t = a;
        a = b;
        b = t;
        double y = y0;
        y0 = y1;
        y1 = y;
    }
    for (int i = a; i <= b; i++) {
        double t = b == a ? 1 : (double)(i - a) / (b - a);
        values[i] = (float)fmin(255, fmax(0, y0 + (y1 - y0) * t));
    }
}
void pc_ui_shell_adjust_graph(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    const char *command = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__command"));
    bool curves = command && !strcmp(command, "image.adjustments.curves");
    bool levels = command && !strcmp(command, "image.adjustments.levels");
    bool tone = curves || (command && (!strcmp(command, "image.adjustments.levels") ||
                                       !strcmp(command, "image.adjustments.threshold")));
    if (!tone)
        return;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    pc_color_mode mode = state ? pc_doc_arc_get(state->doc)->mode : PC_COLOR_MODE_RGB;
    bool quick = false;
    size_t alpha = 0;
    if (pc_engine_targeted_mask(pc_ui_shell_app_session(app)) ||
        pc_engine_targeted_channel(pc_ui_shell_app_session(app), &quick, &alpha))
        mode = PC_COLOR_MODE_GRAYSCALE;
    const char *rgb_names[] = {"Composite", "Red", "Green", "Blue"},
               *rgb_keys[] = {"points", "red", "green", "blue"};
    const char *cmyk_names[] = {"Composite", "Cyan", "Magenta", "Yellow", "Black"},
               *cmyk_keys[] = {"points", "cyan", "magenta", "yellow", "black"};
    const char *lab_names[] = {"Lightness", "a", "b"}, *lab_keys[] = {"lightness", "a", "b"};
    const char *gray_names[] = {"Gray"}, *gray_keys[] = {"points"};
    const char *const *names = mode == PC_COLOR_MODE_CMYK        ? cmyk_names
                               : mode == PC_COLOR_MODE_LAB       ? lab_names
                               : mode == PC_COLOR_MODE_GRAYSCALE ? gray_names
                                                                 : rgb_names;
    const char *const *keys = mode == PC_COLOR_MODE_CMYK        ? cmyk_keys
                              : mode == PC_COLOR_MODE_LAB       ? lab_keys
                              : mode == PC_COLOR_MODE_GRAYSCALE ? gray_keys
                                                                : rgb_keys;
    size_t channels = mode == PC_COLOR_MODE_CMYK        ? 5
                      : mode == PC_COLOR_MODE_LAB       ? 3
                      : mode == PC_COLOR_MODE_GRAYSCALE ? 1
                                                        : 4;
    if (r->curve_dialog != dialog->id) {
        r->curve_dialog = dialog->id;
        r->tone_channel = 0;
        r->curve_valid = false;
        r->curve_focus = false;
        r->map_dragging = false;
        r->levels_drag = 0;
        r->levels_handle = 0;
    }
    if (r->tone_channel >= channels)
        r->tone_channel = 0;
    if (curves || levels) {
        nk_layout_row_dynamic(ctx, 26, 2);
        nk_label(ctx, pc_i18n_t("Channel"), NK_TEXT_LEFT);
        int selected = (int)r->tone_channel;
        int next = nk_combo(ctx, names, (int)channels, selected, 24, nk_vec2(180, 180));
        if (next >= 0 && (size_t)next < channels && next != selected) {
            r->tone_channel = (size_t)next;
            r->curve_valid = false;
            r->map_dragging = false;
        }
    }
    /* Set Black/Neutral Gray/White Point eyedroppers (A35/A36): arm one, then click the canvas to
     * sample. Supported for RGB (all three) and Grayscale (black/white); hidden for CMYK/Lab. */
    if (curves || levels) {
        const struct {
            const char *id, *label;
            pc_engine_tone_picker picker;
        } picks[] = {{"black", "Set Black Point", PC_TONE_PICK_BLACK},
                     {"gray", "Set Gray Point", PC_TONE_PICK_GRAY},
                     {"white", "Set White Point", PC_TONE_PICK_WHITE}};
        int count = 0;
        for (size_t i = 0; i < 3; i++)
            if (pc_engine_tone_picker_supported(mode, picks[i].picker))
                count++;
        if (count > 0) {
            const char *armed = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__eyedropper"));
            nk_layout_row_dynamic(ctx, 26, count);
            for (size_t i = 0; i < 3; i++) {
                if (!pc_engine_tone_picker_supported(mode, picks[i].picker))
                    continue;
                bool on = armed && !strcmp(armed, picks[i].id);
                char mapping[64];
                (void)snprintf(mapping, sizeof mapping, "dialog.eyedropper.%s", picks[i].id);
                pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
                char label[48];
                (void)snprintf(label, sizeof label, "%s%s", on ? "\xe2\x97\x8f " : "",
                               pc_i18n_t(picks[i].label));
                if (nk_button_label(ctx, label)) {
                    if (on)
                        (void)yyjson_mut_obj_remove_str(fields, "__eyedropper");
                    else
                        set_value(dialog, "__eyedropper", yyjson_mut_strcpy(dialog->fields, picks[i].id));
                }
            }
        }
    }
    const char *key = keys[r->tone_channel];
    bool pencil = curves && yyjson_mut_get_bool(yyjson_mut_obj_get(fields, "pencil"));
    if (curves) {
        const char *modes[] = {"Point Curve", "Draw Curve"};
        nk_layout_row_dynamic(ctx, 26, 1);
        int next = pencil ? 1 : 0;
        pc_ui_shell_widget_mapping(app, "dialog.curve.mode", nk_widget_bounds(ctx));
        if (nk_combo_begin_label(ctx, modes[next], nk_vec2(180, 100))) {
            r->graph_blocked = true;
            nk_layout_row_dynamic(ctx, 24, 1);
            for (int i = 0; i < 2; i++) {
                pc_ui_shell_widget_mapping(app, i ? "dialog.curve.mode.draw" : "dialog.curve.mode.points",
                                           nk_widget_bounds(ctx));
                if (nk_button_label(ctx, modes[i])) {
                    next = i;
                    nk_combo_close(ctx);
                }
            }
            nk_combo_end(ctx);
        }
        if ((next == 1) != pencil) {
            pc_error error = {0};
            if (change_mode(app, dialog, next == 1, &error) == PC_OK) {
                pencil = next == 1;
                r->curve_valid = false;
                r->curve.dragging = false;
                r->map_dragging = false;
            } else
                (void)snprintf(r->error, sizeof r->error, "%s", error.msg);
        }
    }
    yyjson_mut_val *values = yyjson_mut_obj_get(fields, key);
    if (curves && !values && !strcmp(key, "lightness"))
        values = yyjson_mut_obj_get(fields, "points");
    if (curves && !values && mode == PC_COLOR_MODE_GRAYSCALE)
        values = yyjson_mut_obj_get(fields, "gray");
    uint8_t hash[32];
    data_key(values, hash);
    if (curves && (!r->curve_valid || strcmp(r->curve_key, key) || memcmp(r->curve_hash, hash, 32))) {
        if (pencil) {
            bool valid = !values || yyjson_mut_is_null(values) ||
                         (yyjson_mut_is_arr(values) && yyjson_mut_arr_size(values) == 256);
            for (size_t i = 0; valid && i < 256; i++) {
                yyjson_mut_val *v = values ? yyjson_mut_arr_get(values, i) : NULL;
                double n = v ? yyjson_mut_get_num(v) : (double)i;
                valid = (!v || yyjson_mut_is_num(v)) && isfinite(n) && n >= 0 && n <= 255;
                r->curve_map[i] = (float)n;
            }
            r->curve_valid = valid;
        } else
            r->curve_valid = pc_ui_point_curve_load(&r->curve, values, NULL) == PC_OK;
        if (!r->curve_valid) {
            nk_layout_row_dynamic(ctx, 32, 1);
            nk_label_wrap(ctx, pc_i18n_t("The curve contains invalid points."));
            return;
        }
        memcpy(r->curve_hash, hash, 32);
        (void)snprintf(r->curve_key, sizeof r->curve_key, "%s", key);
    }
    nk_layout_row_dynamic(ctx, curves ? 260 : 130, 1);
    struct nk_rect graph;
    enum nk_widget_layout_states layout = nk_widget(&graph, ctx);
    if (curves) {
        graph.w = fminf(graph.w, graph.h);
        graph.x += (nk_window_get_content_region(ctx).w - graph.w) / 2;
    }
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    nk_fill_rect(canvas, graph, 0, pc_ui_nk_color(tokens->field));
    size_t channel = curves || levels ? r->tone_channel : 0;
    if (r->preview.has_histogram && channel < r->preview.histogram.channels)
        histogram(canvas, graph, r->preview.histogram.bins[channel], pc_ui_nk_color(tokens->text_faint));
    for (size_t i = 1; i < 4; i++) {
        float at = (float)i / 4;
        nk_stroke_line(canvas, graph.x + graph.w * at, graph.y, graph.x + graph.w * at, graph.y + graph.h, 1,
                       pc_ui_nk_color(tokens->separator));
        nk_stroke_line(canvas, graph.x, graph.y + graph.h * at, graph.x + graph.w, graph.y + graph.h * at, 1,
                       pc_ui_nk_color(tokens->separator));
    }
    nk_stroke_rect(canvas, graph, 0, 1, pc_ui_nk_color(tokens->field_border));
    if (levels) {
        const char *nested =
            r->tone_channel == 0 ? (mode == PC_COLOR_MODE_LAB ? "lightness" : NULL) : keys[r->tone_channel];
        yyjson_mut_val *level_values = nested ? yyjson_mut_obj_get(fields, nested) : fields;
        if (nested && !level_values && mode == PC_COLOR_MODE_LAB && r->tone_channel == 0)
            level_values = fields;
        const char *param[] = {"inBlack", "gamma", "inWhite", "outBlack", "outWhite"};
        const char *labels[] = {"Input Black", "Gamma", "Input White", "Output Black", "Output White"};
        double numbers[5] = {0, 1, 255, 0, 255}, next[5];
        for (size_t i = 0; i < 5; i++) {
            yyjson_mut_val *v = yyjson_mut_obj_get(level_values, param[i]);
            if (yyjson_mut_is_num(v) && isfinite(yyjson_mut_get_num(v)))
                numbers[i] = yyjson_mut_get_num(v);
            next[i] = numbers[i];
        }
        /* Auto: input black/white that clip 0.1% at each end of this channel's histogram. */
        nk_layout_row_dynamic(ctx, 26, 1);
        pc_ui_shell_widget_mapping(app, "dialog.levelsAuto", nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t("Auto")) && r->preview.has_histogram &&
            channel < r->preview.histogram.channels) {
            double lo = 0, hi = 255;
            pc_ui_auto_levels(r->preview.histogram.bins[channel], &lo, &hi);
            next[0] = lo;
            next[2] = hi;
            next[1] = 1;
        }
        /* Input-level slider: triangle handles (black, gamma, white) under the histogram. */
        nk_layout_row_dynamic(ctx, 16, 1);
        struct nk_rect itrack;
        enum nk_widget_layout_states ilayout = nk_widget(&itrack, ctx);
        {
            double ix[3] = {graph.x + next[0] / 255 * graph.w,
                            graph.x + pc_ui_levels_gamma_pos(next[0], next[2], next[1]) / 255 * graph.w,
                            graph.x + next[2] / 255 * graph.w};
            const struct nk_mouse_button *il = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
            if (ilayout != NK_WIDGET_INVALID && il->clicked && r->levels_drag == 0 &&
                il->clicked_pos.y >= itrack.y - 2 && il->clicked_pos.y <= itrack.y + 12 &&
                il->clicked_pos.x >= graph.x - 8 && il->clicked_pos.x <= graph.x + graph.w + 8) {
                size_t h = pc_ui_slider_nearest(ix, 3, il->clicked_pos.x);
                if (h != SIZE_MAX) {
                    r->levels_handle = h;
                    r->levels_drag = 1;
                }
            }
            if (r->levels_drag == 1) {
                double val = fmin(255, fmax(0, (ctx->input.mouse.pos.x - graph.x) / graph.w * 255));
                if (r->levels_handle == 0)
                    next[0] = round(fmin(val, next[2] - 2));
                else if (r->levels_handle == 2)
                    next[2] = round(fmax(val, next[0] + 2));
                else
                    next[1] = round(pc_ui_levels_gamma_from_pos(next[0], next[2], val) * 100) / 100;
                if (!il->down)
                    r->levels_drag = 0;
            }
            float hy = itrack.y + 1;
            struct nk_color edge = pc_ui_nk_color(tokens->text_dim);
            level_handle(canvas, (float)(graph.x + next[0] / 255 * graph.w), hy, nk_rgb(0, 0, 0), edge);
            level_handle(canvas,
                         (float)(graph.x + pc_ui_levels_gamma_pos(next[0], next[2], next[1]) / 255 * graph.w),
                         hy, nk_rgb(128, 128, 128), edge);
            level_handle(canvas, (float)(graph.x + next[2] / 255 * graph.w), hy, nk_rgb(255, 255, 255), edge);
        }
        for (size_t i = 0; i < 5; i++) {
            nk_layout_row_dynamic(ctx, 26, 1);
            char mapping[96];
            (void)snprintf(mapping, sizeof mapping, "dialog.%s", param[i]);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            double min = i == 1   ? .01
                         : i == 2 ? fmax(2, next[0] + 2)
                                  : 0,
                   max = i == 1   ? 9.99
                         : i == 0 ? fmin(253, next[2] - 2)
                                  : 255;
            nk_property_double(ctx, labels[i], min, &next[i], max, i == 1 ? .01 : 1, i == 1 ? .01f : 1);
        }
        /* Output levels: a black->channel gradient ramp with two handles (outBlack, outWhite). */
        nk_layout_row_dynamic(ctx, 10, 1);
        struct nk_rect oramp;
        if (nk_widget(&oramp, ctx) != NK_WIDGET_INVALID) {
            struct nk_color to = level_channel_color(keys[r->tone_channel]);
            for (size_t i = 0; i < 64; i++) {
                float f = (float)i / 63;
                struct nk_color c = nk_rgb((int)((float)to.r * f), (int)((float)to.g * f), (int)((float)to.b * f));
                nk_fill_rect(canvas,
                             nk_rect(graph.x + (float)i / 64 * graph.w, oramp.y, fmaxf(1, graph.w / 64), oramp.h),
                             0, c);
            }
        }
        nk_layout_row_dynamic(ctx, 16, 1);
        struct nk_rect otrack;
        enum nk_widget_layout_states olayout = nk_widget(&otrack, ctx);
        {
            double ox[2] = {graph.x + next[3] / 255 * graph.w, graph.x + next[4] / 255 * graph.w};
            const struct nk_mouse_button *ol = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
            if (olayout != NK_WIDGET_INVALID && ol->clicked && r->levels_drag == 0 &&
                ol->clicked_pos.y >= otrack.y - 2 && ol->clicked_pos.y <= otrack.y + 12 &&
                ol->clicked_pos.x >= graph.x - 8 && ol->clicked_pos.x <= graph.x + graph.w + 8) {
                size_t h = pc_ui_slider_nearest(ox, 2, ol->clicked_pos.x);
                if (h != SIZE_MAX) {
                    r->levels_handle = h;
                    r->levels_drag = 2;
                }
            }
            if (r->levels_drag == 2) {
                double val = round(fmin(255, fmax(0, (ctx->input.mouse.pos.x - graph.x) / graph.w * 255)));
                next[r->levels_handle == 0 ? 3 : 4] = val;
                if (!ol->down)
                    r->levels_drag = 0;
            }
            float hy = otrack.y + 1;
            struct nk_color edge = pc_ui_nk_color(tokens->text_dim);
            level_handle(canvas, (float)(graph.x + next[3] / 255 * graph.w), hy, nk_rgb(0, 0, 0), edge);
            level_handle(canvas, (float)(graph.x + next[4] / 255 * graph.w), hy, nk_rgb(255, 255, 255), edge);
        }
        bool changed = false;
        for (size_t i = 0; i < 5; i++)
            changed = changed || next[i] != numbers[i];
        if (changed) {
            yyjson_mut_val *target = nested ? yyjson_mut_obj(dialog->fields) : fields;
            if (target) {
                for (size_t i = 0; i < 5; i++) {
                    yyjson_mut_val *name = yyjson_mut_strcpy(dialog->fields, param[i]),
                                   *v = yyjson_mut_real(dialog->fields, next[i]);
                    if (name && v)
                        (void)yyjson_mut_obj_put(target, name, v);
                }
                if (nested)
                    set_value(dialog, nested, target);
            }
        }
    }
    if (!curves)
        return;
    pc_ui_shell_widget_mapping(app, "dialog.curve", graph);
    bool changed = false;
    float rect[4] = {graph.x, graph.y, graph.w, graph.h};
    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT],
                                 *right = &ctx->input.mouse.buttons[NK_BUTTON_RIGHT];
    bool press = left->clicked && !r->curve.dragging && !r->map_dragging;
    bool inside = left->clicked_pos.x >= graph.x && left->clicked_pos.x <= graph.x + graph.w &&
                  left->clicked_pos.y >= graph.y && left->clicked_pos.y <= graph.y + graph.h;
    unsigned mods = pc_ui_shell_input_modifiers(app);
    if (left->clicked && !inside)
        r->curve_focus = false;
    if (layout != NK_WIDGET_INVALID && press && inside) {
        r->curve_focus = true;
        if (pencil) {
            r->map_dragging = true;
            r->map_start[0] = r->map_last[0] =
                fmin(255, fmax(0, (left->clicked_pos.x - graph.x) / graph.w * 255));
            r->map_start[1] = r->map_last[1] =
                fmin(255, fmax(0, (graph.y + graph.h - left->clicked_pos.y) / graph.h * 255));
            memcpy(r->map_base, r->curve_map, sizeof r->map_base);
            map_line(r->curve_map, r->map_last[0], r->map_last[1], r->map_last[0], r->map_last[1]);
            changed = true;
        } else
            changed =
                pc_ui_point_curve_pointer(&r->curve, rect, 0, left->clicked_pos.x, left->clicked_pos.y, mods)
                    .changed;
    }
    if (!r->graph_blocked && layout == NK_WIDGET_VALID && right->clicked && right->clicked_pos.x >= graph.x &&
        right->clicked_pos.x <= graph.x + graph.w && right->clicked_pos.y >= graph.y &&
        right->clicked_pos.y <= graph.y + graph.h && !pencil)
        changed =
            pc_ui_point_curve_pointer(&r->curve, rect, 3, right->clicked_pos.x, right->clicked_pos.y, mods)
                .changed ||
            changed;
    if (r->map_dragging) {
        double x = fmin(255, fmax(0, (ctx->input.mouse.pos.x - graph.x) / graph.w * 255)),
               y = fmin(255, fmax(0, (graph.y + graph.h - ctx->input.mouse.pos.y) / graph.h * 255));
        if (mods & PC_UI_MOD_SHIFT) {
            memcpy(r->curve_map, r->map_base, sizeof r->curve_map);
            map_line(r->curve_map, r->map_start[0], r->map_start[1], x, y);
        } else
            map_line(r->curve_map, r->map_last[0], r->map_last[1], x, y);
        r->map_last[0] = x;
        r->map_last[1] = y;
        changed = true;
        if (!left->down)
            r->map_dragging = false;
    } else if (r->curve.dragging) {
        pc_ui_curve_edit edit = pc_ui_point_curve_pointer(
            &r->curve, rect, left->down ? 1u : 2u, ctx->input.mouse.pos.x, ctx->input.mouse.pos.y, mods);
        changed = changed || edit.changed;
    }
    if (r->curve_focus && !pencil) {
        const enum nk_keys native[] = {NK_KEY_LEFT, NK_KEY_RIGHT, NK_KEY_UP,
                                       NK_KEY_DOWN, NK_KEY_DEL,   NK_KEY_BACKSPACE};
        const pc_ui_key logical[] = {PC_KEY_ARROWLEFT, PC_KEY_ARROWRIGHT, PC_KEY_ARROWUP,
                                     PC_KEY_ARROWDOWN, PC_KEY_DELETE,     PC_KEY_BACKSPACE};
        for (size_t i = 0; i < 6; i++)
            if (nk_input_is_key_pressed(&ctx->input, native[i]))
                changed = pc_ui_point_curve_key(&r->curve, logical[i], mods).changed || changed;
    }
    if (!pencil && r->curve.has_selected) {
        size_t i = r->curve.selected;
        nk_layout_row_dynamic(ctx, 26, 2);
        double x = r->curve.points[i][0], y = r->curve.points[i][1], oldx = x, oldy = y;
        nk_property_double(ctx, "Input", 0, &x, 255, 1, 1);
        nk_property_double(ctx, "Output", 0, &y, 255, 1, 1);
        if (x != oldx || y != oldy)
            changed = pc_ui_point_curve_move(&r->curve, i, x, y) || changed;
    }
    float sampled[256];
    bool sampled_ok = true;
    if (pencil)
        for (size_t i = 0; i < 256; i++)
            sampled[i] = r->curve_map[i] / 255;
    else
        sampled_ok = pc_engine_tone_curve_sample((const double (*)[2])r->curve.points, r->curve.count,
                                                 sampled, NULL) == PC_OK;
    if (sampled_ok)
        for (size_t i = 1; i < 256; i++)
            nk_stroke_line(canvas, graph.x + (float)(i - 1) / 255 * graph.w,
                           graph.y + graph.h * (1 - sampled[i - 1]), graph.x + (float)i / 255 * graph.w,
                           graph.y + graph.h * (1 - sampled[i]), 2, pc_ui_nk_color(tokens->accent));
    if (!pencil)
        for (size_t i = 0; i < r->curve.count; i++) {
            float x = graph.x + (float)(r->curve.points[i][0] / 255) * graph.w,
                  y = graph.y + graph.h * (1 - (float)(r->curve.points[i][1] / 255));
            struct nk_rect anchor = nk_rect(x - 3, y - 3, 6, 6);
            if (r->curve.has_selected && r->curve.selected == i)
                nk_fill_circle(canvas, anchor, pc_ui_nk_color(tokens->accent));
            else
                nk_stroke_circle(canvas, anchor, 1, pc_ui_nk_color(tokens->text));
        }
    if (changed) {
        yyjson_mut_val *array = NULL;
        if (pencil) {
            array = yyjson_mut_arr(dialog->fields);
            for (size_t i = 0; array && i < 256; i++)
                if (!yyjson_mut_arr_add_real(dialog->fields, array, r->curve_map[i]))
                    array = NULL;
        } else if (pc_ui_point_curve_json(&r->curve, dialog->fields, &array, NULL) != PC_OK)
            array = NULL;
        if (array) {
            set_value(dialog, key, array);
            if (!strcmp(key, "lightness"))
                (void)yyjson_mut_obj_remove_str(fields, "points");
            data_key(array, r->curve_hash);
        }
    }
}
/* Route a canvas left-click to an open Levels/Curves dialog whose eyedropper is armed (A35/A36):
 * map to image space and apply the Set Black/Gray/White Point sample. Returns true if consumed. */
bool pc_ui_shell_adjust_eyedropper_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf,
                                          const SDL_Event *e, float factor)
{
    if (!app || !xf || !e || e->type != SDL_EVENT_MOUSE_BUTTON_DOWN || e->button.button != SDL_BUTTON_LEFT ||
        !e->button.down || !isfinite(factor))
        return false;
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->n_dialogs)
        return false;
    pc_ui_dialog *dialog = &ui->dialogs[ui->n_dialogs - 1];
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *command = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__command"));
    const char *point = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__eyedropper"));
    const char *kind = NULL;
    if (command && !strcmp(command, "image.adjustments.curves"))
        kind = "curves";
    else if (command && !strcmp(command, "image.adjustments.levels"))
        kind = "levels";
    if (!kind || !point)
        return false;
    float x = e->button.x * factor, y = e->button.y * factor;
    if (!isfinite(x) || !isfinite(y) || x < xf->x || y < xf->y || x >= xf->x + xf->width ||
        y >= xf->y + xf->height)
        return false;
    float at[2];
    if (pc_ui_canvas_to_doc(xf, x, y, at) != PC_OK)
        return false;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state)
        return false;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (at[0] < 0 || at[1] < 0 || at[0] >= (float)doc->size.width || at[1] >= (float)doc->size.height)
        return true; /* Inside the canvas but outside the image: swallow, do not sample. */
    pc_error err = {0};
    if (apply_eyedropper(app, dialog, kind, point, at[0], at[1], &err) != PC_OK) {
        free(ui->status);
        ui->status = pc_strndup(err.msg, strlen(err.msg));
        ui->status_error = true;
    }
    return true;
}
