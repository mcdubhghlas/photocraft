#include "ui/i18n.h"
#include "adjust_controls.h"
#include "shell_internal.h"
#include "ui/widgets.h"
#include "ui/channel_mixer.h"
#include "ui/hue_spectrum.h"
#include "ui/level_auto.h"
#include "ui/filter_dialog.h"
#include "engine/adjust_params.h"
#include "compose/adjust.h"
#include "base/json.h"
#include "base/buf.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
/* The live adjusted-spectrum bar under the Hue/Saturation editor (A38): the full-saturation hue
 * ribbon run through the dialog's current Hue/Saturation adjustment (master + per-range + Colorize)
 * on a small sRGB RGBA8 strip, so the user sees how the edit remaps every hue. Falls back to the
 * unadjusted input ribbon on any error (never blank, never crashes). */
#define PC_HUE_SPECTRUM_SAMPLES 72
static void hue_adjusted_bar(pc_ui_shell_app *app, yyjson_mut_val *root)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    uint8_t strip[PC_HUE_SPECTRUM_SAMPLES * 4];
    pc_ui_hue_spectrum_fill(strip, PC_HUE_SPECTRUM_SAMPLES);
    pc_buf json = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    pc_adjustment adj;
    memset(&adj, 0, sizeof adj);
    pc_error err = {0};
    bool built = pc_ui_filter_dialog_params(root, &json, &err) == PC_OK &&
                 pc_json_parse(pc_buf_cstr(&json), json.len, &parsed, &err) == PC_OK &&
                 pc_engine_adjustment_from_params("hueSaturation", yyjson_doc_get_root(parsed), NULL,
                                                  PC_COLOR_MODE_RGB, &adj, &err) == PC_OK;
    if (built)
        (void)pc_compose_adjust_apply_rgba8(&adj, strip, PC_HUE_SPECTRUM_SAMPLES, 1, &err);
    if (built)
        pc_adjustment_fini(&adj);
    yyjson_doc_free(parsed);
    pc_buf_fini(&json);
    nk_layout_row_dynamic(ctx, 18, 1);
    struct nk_rect bar;
    if (nk_widget(&bar, ctx) == NK_WIDGET_INVALID)
        return;
    for (size_t i = 0; i < PC_HUE_SPECTRUM_SAMPLES; i++)
        nk_fill_rect(canvas,
                     nk_rect(bar.x + (float)i / PC_HUE_SPECTRUM_SAMPLES * bar.w, bar.y,
                             fmaxf(1, bar.w / PC_HUE_SPECTRUM_SAMPLES), bar.h),
                     0, nk_rgb(strip[i * 4], strip[i * 4 + 1], strip[i * 4 + 2]));
}
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
    /* Total %: the sum of this output channel's three source weights, warning past 100% (clips). */
    yyjson_mut_val *outrow = yyjson_mut_obj_get(root, mono ? "gray" : keys[r->adjustment_range]);
    double rgbw[3];
    for (size_t i = 0; i < 3; i++) {
        yyjson_mut_val *vv = yyjson_mut_arr_get(outrow, i);
        rgbw[i] = yyjson_mut_is_num(vv) && isfinite(yyjson_mut_get_num(vv)) ? yyjson_mut_get_num(vv)
                                                                            : defaults[i];
    }
    double total = pc_ui_channel_mixer_total(rgbw[0], rgbw[1], rgbw[2]);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    char total_label[80];
    (void)snprintf(total_label, sizeof total_label, "%s %+.0f%%", pc_i18n_t("Total:"), total);
    nk_layout_row_dynamic(ctx, 22, 1);
    nk_label_colored(ctx, total_label, NK_TEXT_LEFT,
                     pc_ui_nk_color(pc_ui_channel_mixer_overload(total) ? tokens->warning : tokens->text));
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
        /* Colorize wraps hue into 0..360 and nudges a flat image to a visible tint (sat >= 25). */
        double hue_v = value(root, "hue", 0);
        if (colorize) {
            double h = fmod(hue_v, 360);
            if (h < 0)
                h += 360;
            put(dialog, root, "hue", yyjson_mut_real(dialog->fields, h));
            put(dialog, root, "saturation",
                yyjson_mut_real(dialog->fields, pc_ui_hue_colorize_saturation(value(root, "saturation", 25))));
        } else if (hue_v > 180)
            put(dialog, root, "hue", yyjson_mut_real(dialog->fields, hue_v - 360));
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
    /* Input hue spectrum bar (all hues at full saturation); the range band overlays it. */
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    nk_layout_row_dynamic(ctx, 18, 1);
    struct nk_rect spectrum;
    enum nk_widget_layout_states slayout = nk_widget(&spectrum, ctx);
    for (size_t i = 0; i < 72; i++) {
        float rgb[3];
        pc_ui_hue_color((double)i / 72 * 360, rgb);
        nk_fill_rect(canvas, nk_rect(spectrum.x + (float)i / 72 * spectrum.w, spectrum.y,
                                     fmaxf(1, spectrum.w / 72), spectrum.h),
                     0, nk_rgb_f(rgb[0], rgb[1], rgb[2]));
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
        /* Draggable range band over the spectrum: outer fall-off, inner full-strength, 4 handles. */
        float xof[4];
        for (size_t i = 0; i < 4; i++) {
            double deg = fmod(boundaries[i], 360);
            if (deg < 0)
                deg += 360;
            xof[i] = spectrum.x + (float)(deg / 360) * spectrum.w;
        }
        float mid = spectrum.y + spectrum.h / 2;
        nk_fill_rect(canvas, nk_rect(fminf(xof[0], xof[3]), mid - 3, fabsf(xof[3] - xof[0]), 6), 0,
                     pc_ui_nk_color(tokens->text_faint));
        nk_fill_rect(canvas, nk_rect(fminf(xof[1], xof[2]), mid - 3, fabsf(xof[2] - xof[1]), 6), 0,
                     pc_ui_nk_color(tokens->text_dim));
        for (size_t i = 0; i < 4; i++)
            nk_fill_rect(canvas, nk_rect(xof[i] - 2, mid - 6, 5, 12), 1,
                         pc_ui_nk_color(i == 1 || i == 2 ? tokens->text : tokens->field));
        const struct nk_mouse_button *left = &ctx->input.mouse.buttons[NK_BUTTON_LEFT];
        double xd[4] = {xof[0], xof[1], xof[2], xof[3]};
        if (slayout != NK_WIDGET_INVALID && left->clicked && !r->hue_dragging &&
            left->clicked_pos.y >= spectrum.y - 2 && left->clicked_pos.y <= spectrum.y + spectrum.h + 2 &&
            left->clicked_pos.x >= spectrum.x - 8 && left->clicked_pos.x <= spectrum.x + spectrum.w + 8) {
            size_t h = pc_ui_slider_nearest(xd, 4, left->clicked_pos.x);
            if (h != SIZE_MAX && fabs(xd[h] - left->clicked_pos.x) < 12) {
                r->hue_handle = h;
                r->hue_dragging = true;
            }
        }
        if (r->hue_dragging && r->hue_handle < 4) {
            size_t h = r->hue_handle;
            double deg = (ctx->input.mouse.pos.x - spectrum.x) / spectrum.w * 360;
            if (deg < 0)
                deg = 0;
            else if (deg > 360)
                deg = 360;
            /* Keep the dragged handle near its old value across the 0/360 wrap. */
            while (deg - boundaries[h] > 180)
                deg -= 360;
            while (boundaries[h] - deg > 180)
                deg += 360;
            double lo = h ? boundaries[h - 1] : -360, hi = h + 1 < 4 ? boundaries[h + 1] : 720;
            if (deg < lo)
                deg = lo;
            else if (deg > hi)
                deg = hi;
            if (deg != boundaries[h]) {
                boundaries[h] = deg;
                moved = true;
            }
            if (!left->down)
                r->hue_dragging = false;
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
    hue_adjusted_bar(app, root);
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
