#include "ui/i18n.h"
#include "color_range_ui.h"
#include "shell_internal.h"
#include "base/hash.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static void put(pc_ui_dialog *dialog, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(dialog->fields, key);
    if (k && value)
        (void)yyjson_mut_obj_put(yyjson_mut_doc_get_root(dialog->fields), k, value);
}
pc_status pc_ui_shell_color_range_init(pc_ui_shell_app *app, yyjson_mut_doc *d, pc_error *err)
{
    yyjson_mut_val *color = yyjson_mut_arr(d);
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    bool ok = color != NULL;
    for (size_t i = 0; ok && i < 3; i++)
        ok = yyjson_mut_arr_add_real(d, color, tools->foreground[i]);
    pc_ui_dialog dialog = {.fields = d};
    if (ok) {
        put(&dialog, "color", color);
        put(&dialog, "__colorRange", yyjson_mut_bool(d, true));
        put(&dialog, "__rangeTool", yyjson_mut_uint(d, 0));
        put(&dialog, "__rangeImage", yyjson_mut_bool(d, false));
        put(&dialog, "points", yyjson_mut_arr(d));
        put(&dialog, "subtractPoints", yyjson_mut_arr(d));
        put(&dialog, "order", yyjson_mut_arr(d));
    }
    return ok ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "Color Range fields");
}
static void number(pc_ui_shell_app *app, pc_ui_dialog *d, const char *key, const char *label, double min,
                   double max, double initial)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *v = yyjson_mut_obj_get(yyjson_mut_doc_get_root(d->fields), key);
    double value = yyjson_mut_is_num(v) && isfinite(yyjson_mut_get_num(v)) ? yyjson_mut_get_num(v) : initial,
           next = value;
    nk_layout_row_dynamic(ctx, 28, 1);
    char mapping[96];
    (void)snprintf(mapping, sizeof mapping, "range.%s", key);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    nk_property_double(ctx, label, min, &next, max, 1, .1f);
    if (value != next)
        put(d, key, yyjson_mut_real(d->fields, next));
}
static void sample(pc_ui_dialog *d, float x, float y, unsigned tool)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields);
    if (!tool) {
        put(d, "points", yyjson_mut_arr(d->fields));
        put(d, "subtractPoints", yyjson_mut_arr(d->fields));
        put(d, "order", yyjson_mut_arr(d->fields));
        (void)yyjson_mut_obj_remove_str(root, "color");
        (void)yyjson_mut_obj_remove_str(root, "colors");
    }
    const char *key = tool == 2 ? "subtractPoints" : "points";
    yyjson_mut_val *array = yyjson_mut_obj_get(root, key), *order = yyjson_mut_obj_get(root, "order");
    if (!yyjson_mut_is_arr(array) || !yyjson_mut_is_arr(order) || yyjson_mut_arr_size(array) >= 4096)
        return;
    yyjson_mut_val *point = yyjson_mut_arr(d->fields);
    if (point && yyjson_mut_arr_add_real(d->fields, point, floorf(x)) &&
        yyjson_mut_arr_add_real(d->fields, point, floorf(y)) && yyjson_mut_arr_add_val(array, point))
        (void)yyjson_mut_arr_add_str(d->fields, order, tool == 2 ? "-" : "+");
}
static void thumbnail(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d, bool sampled)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields);
    struct nk_image source;
    float aspect;
    nk_layout_row_dynamic(ctx, 220, 1);
    struct nk_rect bounds;
    enum nk_widget_layout_states layout = nk_widget(&bounds, ctx);
    if (!pc_ui_shell_canvas_preview(app, &source, &aspect))
        return;
    float w = fminf(bounds.w, bounds.h * aspect), h = w / aspect;
    struct nk_rect rect = nk_rect(bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2, w, h);
    pc_ui_shell_widget_mapping(app, "range.preview", rect);
    struct nk_image shown = source;
    bool image = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__rangeImage"));
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    const pc_document *doc = pc_doc_arc_get(r->preview.result ? r->preview.result : state->doc);
    if (!image) {
        uint16_t width = (uint16_t)fmaxf(1, fminf(200, 200 * aspect)),
                 height = (uint16_t)fmaxf(1, fminf(200, 200 / aspect));
        uint8_t pixels[200 * 200 * 4];
        float p0[2], px[2], py[2];
        bool ok = pc_ui_shell_preview_doc_point(app, &source, 0, 0, p0) &&
                  pc_ui_shell_preview_doc_point(app, &source, 1, 0, px) &&
                  pc_ui_shell_preview_doc_point(app, &source, 0, 1, py);
        if (ok) {
            for (size_t y = 0; y < height; y++)
                for (size_t x = 0; x < width; x++) {
                    float u = ((float)x + .5f) / (float)width, v = ((float)y + .5f) / (float)height,
                          dx = p0[0] + u * (px[0] - p0[0]) + v * (py[0] - p0[0]),
                          dy = p0[1] + u * (px[1] - p0[1]) + v * (py[1] - p0[1]);
                    float coverage = dx >= 0 && dy >= 0 && dx < (float)doc->size.width &&
                                             dy < (float)doc->size.height && doc->has_selection
                                         ? pc_surface_sample_channel(&doc->selection, (int32_t)floorf(dx),
                                                                     (int32_t)floorf(dy), 0)
                                         : 0;
                    uint8_t level = (uint8_t)fminf(255, fmaxf(0, roundf(coverage * 255)));
                    size_t index = (y * width + x) * 4;
                    pixels[index] = pixels[index + 1] = pixels[index + 2] = level;
                    pixels[index + 3] = 255;
                }
            pc_buf bytes = PC_BUF_INIT;
            uint64_t scope[] = {doc->id, state->revision, r->preview.generation, width, height};
            uint8_t key[32];
            ok = pc_buf_append(&bytes, scope, sizeof scope, NULL) == PC_OK &&
                 pc_buf_append(&bytes, p0, sizeof p0, NULL) == PC_OK &&
                 pc_buf_append(&bytes, px, sizeof px, NULL) == PC_OK &&
                 pc_buf_append(&bytes, py, sizeof py, NULL) == PC_OK;
            if (ok) {
                pc_blake3(bytes.data, bytes.len, key);
                (void)pc_ui_shell_bitmap(app, key, width, height, pixels, &shown, NULL);
            }
            pc_buf_fini(&bytes);
        }
    }
    nk_draw_image(nk_window_get_canvas(ctx), rect, &shown, nk_rgba(255, 255, 255, 255));
    const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
    if (sampled && !r->graph_blocked && layout == NK_WIDGET_VALID && left->clicked && left->down &&
        nk_input_is_mouse_hovering_rect(&ctx->input, rect)) {
        float point[2];
        if (pc_ui_shell_preview_doc_point(app, &source, (ctx->input.mouse.pos.x - rect.x) / rect.w,
                                          (ctx->input.mouse.pos.y - rect.y) / rect.h, point) &&
            point[0] >= 0 && point[1] >= 0 && point[0] < (float)doc->size.width &&
            point[1] < (float)doc->size.height) {
            unsigned modifiers = pc_ui_shell_input_modifiers(app),
                     tool = modifiers & PC_UI_MOD_ALT ? 2
                            : modifiers & PC_UI_MOD_SHIFT
                                ? 1
                                : (unsigned)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__rangeTool"));
            sample(d, point[0], point[1], tool);
        }
    }
}
void pc_ui_shell_color_range_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *d)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d->fields);
    const char *names[] = {"Sampled Colors", "Reds",       "Yellows",  "Greens",  "Cyans",       "Blues",
                           "Magentas",       "Highlights", "Midtones", "Shadows", "Out Of Gamut"},
               *values[] = {"sampledColors", "reds",       "yellows",  "greens",  "cyans",     "blues",
                            "magentas",      "highlights", "midtones", "shadows", "outOfGamut"};
    const char *old = yyjson_mut_get_str(yyjson_mut_obj_get(root, "select"));
    int selected = 0;
    for (size_t i = 0; i < 11; i++)
        if (old && !strcmp(old, values[i]))
            selected = (int)i;
    nk_layout_row_dynamic(ctx, 28, 1);
    pc_ui_shell_widget_mapping(app, "range.select", nk_widget_bounds(ctx));
    if (nk_combo_begin_label(ctx, names[selected], nk_vec2(260, 330))) {
        r->graph_blocked = true;
        nk_layout_row_dynamic(ctx, 25, 1);
        for (size_t i = 0; i < 11; i++) {
            char id[80];
            (void)snprintf(id, sizeof id, "range.select.%zu", i);
            pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
            if (nk_button_label(ctx, names[i])) {
                if (selected != (int)i) {
                    yyjson_mut_val *fuzz = yyjson_mut_obj_get(root, "fuzziness");
                    if (selected == 0 || (selected >= 7 && selected <= 9))
                        put(d, selected == 0 ? "__sampleFuzziness" : "__toneFuzziness",
                            fuzz ? yyjson_mut_val_mut_copy(d->fields, fuzz)
                                 : yyjson_mut_real(d->fields, selected == 0 ? 40 : 20));
                    yyjson_mut_val *cached = yyjson_mut_obj_get(root, "__rangeTones");
                    if (!yyjson_mut_is_obj(cached)) {
                        cached = yyjson_mut_obj(d->fields);
                        put(d, "__rangeTones", cached);
                    }
                    if (selected >= 7 && selected <= 9) {
                        yyjson_mut_val *range = yyjson_mut_obj_get(root, "tonalRange"),
                                       *k = yyjson_mut_strcpy(d->fields, values[selected]);
                        if (range && k)
                            (void)yyjson_mut_obj_put(cached, k, yyjson_mut_val_mut_copy(d->fields, range));
                    }
                    selected = (int)i;
                    put(d, "select", yyjson_mut_strcpy(d->fields, values[i]));
                    yyjson_mut_val *saved =
                        yyjson_mut_obj_get(root, i == 0 ? "__sampleFuzziness" : "__toneFuzziness");
                    put(d, "fuzziness",
                        saved ? yyjson_mut_val_mut_copy(d->fields, saved)
                              : yyjson_mut_real(d->fields, i == 0 ? 40 : 20));
                    yyjson_mut_val *range = yyjson_mut_obj_get(cached, values[i]);
                    if (range)
                        put(d, "tonalRange", yyjson_mut_val_mut_copy(d->fields, range));
                    else
                        (void)yyjson_mut_obj_remove_str(root, "tonalRange");
                }
                nk_combo_close(ctx);
            }
        }
        nk_combo_end(ctx);
    }
    bool sampled = selected == 0, tonal = selected >= 7 && selected <= 9;
    if (sampled || tonal)
        number(app, d, "fuzziness", "Fuzziness", 0, sampled ? 200 : 100, sampled ? 40 : 20);
    if (sampled) {
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_bool localized = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "localized"));
        if (nk_checkbox_label(ctx, pc_i18n_t("Localized Color Clusters"), &localized))
            put(d, "localized", yyjson_mut_bool(d->fields, localized != 0));
        if (localized)
            number(app, d, "range", "Range", 0, 100, 100);
        nk_layout_row_dynamic(ctx, 26, 3);
        const char *labels[] = {"Sample", "Add", "Subtract"};
        for (size_t i = 0; i < 3; i++) {
            char id[80];
            (void)snprintf(id, sizeof id, "range.tool.%zu", i);
            pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
            if (nk_button_label(ctx, labels[i]))
                put(d, "__rangeTool", yyjson_mut_uint(d->fields, i));
        }
    } else if (tonal) {
        yyjson_mut_val *range = yyjson_mut_obj_get(root, "tonalRange");
        double low = selected == 7 ? 190 : selected == 9 ? 65 : 105, high = 150;
        if (selected == 8) {
            if (yyjson_mut_is_num(yyjson_mut_arr_get(range, 0)))
                low = yyjson_mut_get_num(yyjson_mut_arr_get(range, 0));
            if (yyjson_mut_is_num(yyjson_mut_arr_get(range, 1)))
                high = yyjson_mut_get_num(yyjson_mut_arr_get(range, 1));
        } else if (yyjson_mut_is_num(range))
            low = yyjson_mut_get_num(range);
        double a = low, b = high;
        nk_layout_row_dynamic(ctx, 28, 1);
        pc_ui_shell_widget_mapping(app, "range.tonal.low", nk_widget_bounds(ctx));
        nk_property_double(ctx, selected == 8 ? "Low" : "Range", 0, &a, selected == 8 ? high : 255, 1, 1);
        if (selected == 8) {
            nk_layout_row_dynamic(ctx, 28, 1);
            pc_ui_shell_widget_mapping(app, "range.tonal.high", nk_widget_bounds(ctx));
            nk_property_double(ctx, "High", a, &b, 255, 1, 1);
        }
        if (a != low || b != high) {
            yyjson_mut_val *v = yyjson_mut_real(d->fields, a);
            if (selected == 8) {
                v = yyjson_mut_arr(d->fields);
                if (!v || !yyjson_mut_arr_add_real(d->fields, v, a) ||
                    !yyjson_mut_arr_add_real(d->fields, v, b))
                    v = NULL;
            }
            if (v)
                put(d, "tonalRange", v);
        }
    }
    thumbnail(app, r, d, sampled);
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_bool image = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__rangeImage"));
    pc_ui_shell_widget_mapping(app, "range.selection", nk_widget_bounds(ctx));
    if (nk_option_label(ctx, pc_i18n_t("Selection"), !image))
        put(d, "__rangeImage", yyjson_mut_bool(d->fields, false));
    pc_ui_shell_widget_mapping(app, "range.image", nk_widget_bounds(ctx));
    if (nk_option_label(ctx, pc_i18n_t("Image"), image))
        put(d, "__rangeImage", yyjson_mut_bool(d->fields, true));
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_bool invert = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "invert")),
            preview = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "preview"));
    pc_ui_shell_widget_mapping(app, "range.invert", nk_widget_bounds(ctx));
    if (nk_checkbox_label(ctx, pc_i18n_t("Invert"), &invert))
        put(d, "invert", yyjson_mut_bool(d->fields, invert != 0));
    if (nk_checkbox_label(ctx, pc_i18n_t("Preview"), &preview))
        put(d, "preview", yyjson_mut_bool(d->fields, preview != 0));
    nk_layout_row_dynamic(ctx, 24, 1);
    if (pc_ui_filter_preview_busy(&r->preview))
        nk_label(ctx, pc_i18n_t("Updating preview..."), NK_TEXT_LEFT);
    else if (r->preview.has_ready && r->preview.status != PC_OK)
        nk_label_wrap(ctx, r->preview.error.msg);
}
