#include "ui/i18n.h"
#include "adjust_controls.h"
#include "shell_internal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static void put(pc_ui_dialog *dialog, yyjson_mut_val *o, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *name = yyjson_mut_strcpy(dialog->fields, key);
    if (name && value)
        (void)yyjson_mut_obj_put(o, name, value);
}
static double value(yyjson_mut_val *o, const char *key, double fallback)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(o, key);
    double x = yyjson_mut_get_num(v);
    return yyjson_mut_is_num(v) && isfinite(x) ? x : fallback;
}
static size_t choose(pc_ui_shell_app *app, const char *id, const char *const *labels, size_t count,
                     size_t selected)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (selected >= count)
        selected = 0;
    nk_layout_row_dynamic(ctx, 26, 1);
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    if (nk_combo_begin_label(ctx, labels[selected], nk_vec2(240, 280))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        for (size_t i = 0; i < count; i++) {
            char item[128];
            (void)snprintf(item, sizeof item, "%s.%zu", id, i);
            pc_ui_shell_widget_mapping(app, item, nk_widget_bounds(ctx));
            if (nk_button_label(ctx, labels[i])) {
                selected = i;
                nk_combo_close(ctx);
            }
        }
        nk_combo_end(ctx);
    }
    return selected;
}
static bool property(pc_ui_shell_app *app, const char *id, const char *label, double *v, double min,
                     double max)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    double old = *v;
    nk_layout_row_dynamic(ctx, 26, 1);
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    nk_property_double(ctx, label, min, v, max, 1, 1);
    return *v != old;
}
static void array_editor(pc_ui_shell_app *app, pc_ui_dialog *dialog, const char *key,
                         const char *const *labels, size_t count, const double *defaults, double min,
                         double max)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields), *array = yyjson_mut_obj_get(root, key);
    double data[4] = {0};
    bool changed = false;
    for (size_t i = 0; i < count && i < 4; i++) {
        yyjson_mut_val *v = yyjson_mut_arr_get(array, i);
        data[i] =
            yyjson_mut_is_num(v) && isfinite(yyjson_mut_get_num(v)) ? yyjson_mut_get_num(v) : defaults[i];
        char mapping[128];
        (void)snprintf(mapping, sizeof mapping, "dialog.%s.%zu", key, i);
        changed = property(app, mapping, labels[i], &data[i], min, max) || changed;
    }
    if (changed) {
        yyjson_mut_val *next = yyjson_mut_arr(dialog->fields);
        bool ok = next != NULL;
        for (size_t i = 0; ok && i < count; i++)
            ok = yyjson_mut_arr_add_real(dialog->fields, next, data[i]);
        if (ok)
            put(dialog, root, key, next);
    }
}
static void balance(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    const char *labels[] = {"Shadows", "Midtones", "Highlights"},
               *keys[] = {"shadows", "midtones", "highlights"};
    r->adjustment_range = choose(app, "dialog.balanceTone", labels, 3, r->adjustment_range);
    const char *sliders[] = {"Cyan / Red", "Magenta / Green", "Yellow / Blue"};
    double defaults[3] = {0};
    array_editor(app, dialog, keys[r->adjustment_range], sliders, 3, defaults, -100, 100);
}
static void mixer(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    bool mono = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "monochrome"));
    nk_bool next = mono;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_dynamic(ctx, 26, 1);
    pc_ui_shell_widget_mapping(app, "dialog.monochrome", nk_widget_bounds(ctx));
    nk_checkbox_label(ctx, pc_i18n_t("Monochrome"), &next);
    if ((next != 0) != mono) {
        if (!next) {
            yyjson_mut_val *gray = yyjson_mut_obj_get(root, "gray");
            if (gray)
                put(dialog, root, "__mixerGray", yyjson_mut_val_mut_copy(dialog->fields, gray));
            (void)yyjson_mut_obj_remove_str(root, "gray");
        } else {
            yyjson_mut_val *gray = yyjson_mut_obj_get(root, "__mixerGray");
            if (gray)
                put(dialog, root, "gray", yyjson_mut_val_mut_copy(dialog->fields, gray));
        }
        mono = next != 0;
        put(dialog, root, "monochrome", yyjson_mut_bool(dialog->fields, mono));
    }
    const char *outputs[] = {"Red Output", "Green Output", "Blue Output"}, *keys[] = {"red", "green", "blue"};
    if (!mono)
        r->adjustment_range = choose(app, "dialog.mixerOutput", outputs, 3, r->adjustment_range);
    const char *sliders[] = {"Red", "Green", "Blue", "Constant"};
    double defaults[4] = {0};
    if (mono) {
        defaults[0] = 40;
        defaults[1] = 40;
        defaults[2] = 20;
    } else
        defaults[r->adjustment_range] = 100;
    array_editor(app, dialog, mono ? "gray" : keys[r->adjustment_range], sliders, 4, defaults, -200, 200);
}
static void hue(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    bool colorize = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "colorize"));
    nk_bool next = colorize;
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_checkbox_label(ctx, pc_i18n_t("Colorize"), &next);
    if ((next != 0) != colorize) {
        colorize = next != 0;
        put(dialog, root, "colorize", yyjson_mut_bool(dialog->fields, colorize));
    }
    const char *labels[] = {"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
    const char *keys[] = {"", "reds", "yellows", "greens", "cyans", "blues", "magentas"};
    if (colorize)
        r->adjustment_range = 0;
    else
        r->adjustment_range = choose(app, "dialog.hueRange", labels, 7, r->adjustment_range);
    bool master = r->adjustment_range == 0;
    const char *key = keys[r->adjustment_range];
    yyjson_mut_val *target = master ? root : yyjson_mut_obj_get(root, key);
    const char *params[] = {"hue", "saturation", "lightness"},
               *sliders[] = {"Hue", "Saturation", "Lightness"};
    double data[3];
    bool changed = false;
    for (size_t i = 0; i < 3; i++) {
        data[i] = value(target, params[i], 0);
        double min = i == 0               ? (colorize ? 0 : -180)
                     : i == 1 && colorize ? 0
                                          : -100,
               max = i == 0 ? (colorize ? 360 : 180) : 100;
        char id[128];
        (void)snprintf(id, sizeof id, "dialog.%s%s%s", master ? "" : key, master ? "" : ".", params[i]);
        changed = property(app, id, sliders[i], &data[i], min, max) || changed;
    }
    if (changed) {
        yyjson_mut_val *o = master                      ? root
                            : yyjson_mut_is_obj(target) ? yyjson_mut_val_mut_copy(dialog->fields, target)
                                                        : yyjson_mut_obj(dialog->fields);
        if (o) {
            for (size_t i = 0; i < 3; i++)
                put(dialog, o, params[i], yyjson_mut_real(dialog->fields, data[i]));
            if (!master)
                put(dialog, root, key, o);
        }
    }
    if (!master) {
        target = yyjson_mut_obj_get(root, key);
        yyjson_mut_val *range = yyjson_mut_obj_get(target, "range");
        double boundaries[4];
        double center = 60 * (double)(r->adjustment_range - 1);
        const double offsets[4] = {-45, -15, 15, 45};
        bool moved = false;
        const char *names[] = {"Fade In", "Full Start", "Full End", "Fade Out"};
        for (size_t i = 0; i < 4; i++) {
            yyjson_mut_val *v = yyjson_mut_arr_get(range, i);
            boundaries[i] = yyjson_mut_is_num(v) && isfinite(yyjson_mut_get_num(v)) ? yyjson_mut_get_num(v)
                                                                                    : center + offsets[i];
        }
        for (size_t i = 0; i < 4; i++) {
            char id[128];
            (void)snprintf(id, sizeof id, "dialog.%s.range.%zu", key, i);
            moved = property(app, id, names[i], &boundaries[i], i ? boundaries[i - 1] : -360,
                             i + 1 < 4 ? boundaries[i + 1] : 720) ||
                    moved;
        }
        if (moved) {
            yyjson_mut_val *array = yyjson_mut_arr(dialog->fields),
                           *o = yyjson_mut_is_obj(target) ? yyjson_mut_val_mut_copy(dialog->fields, target)
                                                          : yyjson_mut_obj(dialog->fields);
            bool ok = array && o;
            for (size_t i = 0; ok && i < 4; i++)
                ok = yyjson_mut_arr_add_real(dialog->fields, array, boundaries[i]);
            if (ok) {
                put(dialog, o, "range", array);
                put(dialog, root, key, o);
            }
        }
    }
}
static void selective(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *labels[] = {"Reds",     "Yellows", "Greens",   "Cyans", "Blues",
                            "Magentas", "Whites",  "Neutrals", "Blacks"};
    const char *keys[] = {"reds",     "yellows", "greens",   "cyans", "blues",
                          "magentas", "whites",  "neutrals", "blacks"},
               *components[] = {"cyan", "magenta", "yellow", "black"};
    /* Fold the command's legacy single-range controls into that range before switching views. */
    const char *legacy = yyjson_mut_get_str(yyjson_mut_obj_get(root, "colors"));
    if (legacy) {
        for (size_t i = 0; i < 9; i++)
            if (!strcmp(legacy, keys[i])) {
                yyjson_mut_val *old = yyjson_mut_obj_get(root, keys[i]),
                               *next = yyjson_mut_arr(dialog->fields);
                bool ok = next != NULL;
                for (size_t j = 0; ok && j < 4; j++) {
                    yyjson_mut_val *v = yyjson_mut_obj_get(root, components[j]),
                                   *prior = yyjson_mut_arr_get(old, j);
                    double n = yyjson_mut_is_num(v)       ? yyjson_mut_get_num(v)
                               : yyjson_mut_is_num(prior) ? yyjson_mut_get_num(prior)
                                                          : 0;
                    ok = yyjson_mut_arr_add_real(dialog->fields, next, n);
                }
                if (ok) {
                    put(dialog, root, keys[i], next);
                    for (size_t j = 0; j < 4; j++)
                        (void)yyjson_mut_obj_remove_str(root, components[j]);
                    (void)yyjson_mut_obj_remove_str(root, "colors");
                }
                break;
            }
    }
    r->adjustment_range = choose(app, "dialog.selectiveRange", labels, 9, r->adjustment_range);
    const char *sliders[] = {"Cyan", "Magenta", "Yellow", "Black"};
    double defaults[4] = {0};
    array_editor(app, dialog, keys[r->adjustment_range], sliders, 4, defaults, -100, 100);
}
bool pc_ui_shell_adjust_owns(const char *command, const char *key)
{
    if (!command)
        return false;
    if (!strcmp(command, "image.adjustments.hueSaturation"))
        return !strcmp(key, "hue") || !strcmp(key, "saturation") || !strcmp(key, "lightness") ||
               !strcmp(key, "colorize");
    if (!strcmp(command, "image.adjustments.channelMixer"))
        return !strcmp(key, "monochrome");
    if (!strcmp(command, "image.adjustments.selectiveColor"))
        return !strcmp(key, "colors") || !strcmp(key, "cyan") || !strcmp(key, "magenta") ||
               !strcmp(key, "yellow") || !strcmp(key, "black");
    return false;
}
void pc_ui_shell_adjust_controls(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    const char *command =
        yyjson_mut_get_str(yyjson_mut_obj_get(yyjson_mut_doc_get_root(dialog->fields), "__command"));
    if (!command)
        return;
    if (!strcmp(command, "image.adjustments.colorBalance"))
        balance(app, r, dialog);
    else if (!strcmp(command, "image.adjustments.channelMixer"))
        mixer(app, r, dialog);
    else if (!strcmp(command, "image.adjustments.hueSaturation"))
        hue(app, r, dialog);
    else if (!strcmp(command, "image.adjustments.selectiveColor"))
        selective(app, r, dialog);
}
