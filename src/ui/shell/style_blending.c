#include "ui/i18n.h"
#include "layer_style_ui.h"
#include "shell_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
static void put(pc_ui_dialog *dialog, yyjson_mut_val *object, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *name = yyjson_mut_strcpy(dialog->fields, key);
    if (name && value)
        (void)yyjson_mut_obj_put(object, name, value);
}
static size_t channel_of(yyjson_mut_val *entry, const char *const *names, size_t count)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(entry, "channel");
    if (yyjson_mut_is_uint(v))
        return (size_t)yyjson_mut_get_uint(v);
    if (yyjson_mut_is_sint(v) && yyjson_mut_get_sint(v) >= 0)
        return (size_t)yyjson_mut_get_sint(v);
    const char *text = yyjson_mut_get_str(v);
    for (size_t i = 0; text && i < count; i++)
        if (!strcasecmp(text, names[i]))
            return i;
    return SIZE_MAX;
}
static yyjson_mut_val *find(yyjson_mut_val *list, size_t channel, const char *const *names, size_t count)
{
    if (yyjson_mut_is_obj(list))
        return channel_of(list, names, count) == channel ? list : NULL;
    for (size_t i = 0; i < yyjson_mut_arr_size(list); i++) {
        yyjson_mut_val *entry = yyjson_mut_arr_get(list, i);
        if (channel_of(entry, names, count) == channel)
            return entry;
    }
    return NULL;
}
static yyjson_mut_val *entry(pc_ui_dialog *dialog, size_t channel, const double numbers[2][4])
{
    yyjson_mut_doc *d = dialog->fields;
    yyjson_mut_val *o = yyjson_mut_obj(d), *ranges[2] = {yyjson_mut_arr(d), yyjson_mut_arr(d)};
    bool ok = o && ranges[0] && ranges[1] && yyjson_mut_obj_add_uint(d, o, "channel", channel);
    for (size_t r = 0; r < 2; r++)
        for (size_t i = 0; ok && i < 4; i++)
            ok = yyjson_mut_arr_add_uint(d, ranges[r], (uint64_t)fmin(255, fmax(0, round(numbers[r][i]))));
    ok = ok && yyjson_mut_obj_add_val(d, o, "thisLayer", ranges[0]) &&
         yyjson_mut_obj_add_val(d, o, "underlying", ranges[1]);
    return ok ? o : NULL;
}
void pc_ui_shell_style_blend_if(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields),
                   *params = yyjson_mut_obj_get(root, "blending"),
                   *base = yyjson_mut_obj_get(root, "blendingValues");
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    pc_color_mode mode = state ? pc_doc_arc_get(state->doc)->mode : PC_COLOR_MODE_RGB;
    const char *rgb[] = {"Gray", "Red", "Green", "Blue"},
               *cmyk[] = {"Gray", "Cyan", "Magenta", "Yellow", "Black"},
               *lab[] = {"Gray", "Lightness", "a", "b"}, *gray[] = {"Gray"};
    const char *const *names = mode == PC_COLOR_MODE_CMYK        ? cmyk
                               : mode == PC_COLOR_MODE_LAB       ? lab
                               : mode == PC_COLOR_MODE_GRAYSCALE ? gray
                                                                 : rgb;
    size_t count = mode == PC_COLOR_MODE_CMYK ? 5 : mode == PC_COLOR_MODE_GRAYSCALE ? 1 : 4;
    if (runtime->style_blend_channel >= count)
        runtime->style_blend_channel = 0;
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t("Blend If"), NK_TEXT_LEFT);
    pc_ui_shell_widget_mapping(app, "style.blendIf.channel", nk_widget_bounds(ctx));
    int selected = (int)runtime->style_blend_channel,
        next = nk_combo(ctx, names, (int)count, selected, 24, nk_vec2(220, 180));
    if (next >= 0 && (size_t)next < count)
        runtime->style_blend_channel = (size_t)next;
    yyjson_mut_val *delta = yyjson_mut_obj_get(params, "blendIf"),
                   *original = yyjson_mut_obj_get(base, "blendIf");
    bool reset = delta && yyjson_mut_is_null(delta);
    yyjson_mut_val *shown = find(delta, runtime->style_blend_channel, names, count),
                   *fallback = reset ? NULL : find(original, runtime->style_blend_channel, names, count);
    const char *keys[] = {"thisLayer", "underlying"}, *labels[] = {"This Layer", "Underlying Layer"};
    double numbers[2][4];
    bool changed = false;
    for (size_t r = 0; r < 2; r++) {
        yyjson_mut_val *array = yyjson_mut_obj_get(shown, keys[r]);
        if (!array)
            array = yyjson_mut_obj_get(fallback, keys[r]);
        for (size_t i = 0; i < 4; i++) {
            yyjson_mut_val *v = yyjson_mut_arr_get(array, i);
            numbers[r][i] = yyjson_mut_is_num(v) && isfinite(yyjson_mut_get_num(v))
                                ? fmin(255, fmax(0, yyjson_mut_get_num(v)))
                            : i < 2 ? 0
                                    : 255;
        }
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, labels[r], NK_TEXT_LEFT);
        const char *parts[] = {"Black Start", "Black End", "White Start", "White End"};
        for (size_t i = 0; i < 4; i++) {
            nk_layout_row_dynamic(ctx, 26, 1);
            char id[128];
            (void)snprintf(id, sizeof id, "style.blendIf.%s.%zu", keys[r], i);
            pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
            double old = numbers[r][i];
            nk_property_double(ctx, parts[i], i ? numbers[r][i - 1] : 0, &numbers[r][i],
                               i + 1 < 4 ? fmax(numbers[r][i], numbers[r][i + 1]) : 255, 1, 1);
            changed = changed || numbers[r][i] != old;
        }
    }
    nk_layout_row_dynamic(ctx, 26, 1);
    if (nk_button_label(ctx, pc_i18n_t("Reset This Channel"))) {
        for (size_t r = 0; r < 2; r++)
            for (size_t i = 0; i < 4; i++)
                numbers[r][i] = i < 2 ? 0 : 255;
        changed = true;
    }
    if (changed) {
        yyjson_mut_val *array = yyjson_mut_arr(dialog->fields);
        bool ok = array != NULL;
        if (reset) {
            size_t total = pc_color_mode_channels(mode) + 1;
            for (size_t i = 0; ok && i < total; i++)
                if (i != runtime->style_blend_channel) {
                    const double full[2][4] = {{0, 0, 255, 255}, {0, 0, 255, 255}};
                    yyjson_mut_val *v = entry(dialog, i, full);
                    ok = v && yyjson_mut_arr_add_val(array, v);
                }
        } else if (yyjson_mut_is_arr(delta)) {
            for (size_t i = 0; ok && i < yyjson_mut_arr_size(delta); i++) {
                yyjson_mut_val *v = yyjson_mut_arr_get(delta, i);
                if (channel_of(v, names, count) != runtime->style_blend_channel)
                    ok = yyjson_mut_arr_add_val(array, yyjson_mut_val_mut_copy(dialog->fields, v));
            }
        } else if (yyjson_mut_is_obj(delta) &&
                   channel_of(delta, names, count) != runtime->style_blend_channel)
            ok = yyjson_mut_arr_add_val(array, yyjson_mut_val_mut_copy(dialog->fields, delta));
        yyjson_mut_val *v = entry(dialog, runtime->style_blend_channel, (const double (*)[4])numbers);
        ok = ok && v && yyjson_mut_arr_add_val(array, v);
        if (ok)
            put(dialog, params, "blendIf", array);
    }
}
