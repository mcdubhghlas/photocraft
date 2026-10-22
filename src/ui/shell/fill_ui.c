#include "fill_ui.h"
#include "pattern_picker_ui.h"
#include "shell_internal.h"
#include "ui/fill_dialog.h"
#include "ui/color_picker.h"
#include "ui/i18n.h"
#include "ui/widgets.h"
#include "color/blend.h"
#include <stdio.h>
#include <string.h>

static void put(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key, yyjson_mut_val *value)
{
    if (!value) {
        return;
    }
    (void)yyjson_mut_obj_remove_str(object, key);
    (void)yyjson_mut_obj_add_val(doc, object, key, value);
}
pc_status pc_ui_shell_fill_open(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (!d) {
        return pc_fail(err, PC_ERR_NOMEM, "fill dialog");
    }
    yyjson_mut_val *root = pc_ui_fill_default_fields(d, tools ? tools->foreground : NULL);
    bool ok = root && yyjson_mut_obj_add_strcpy(d, root, "__command", "edit.fill") &&
              yyjson_mut_obj_add_strcpy(d, root, "__label", "Fill") &&
              yyjson_mut_obj_add_bool(d, root, "__fill", true);
    if (!ok) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "fill dialog");
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
}
static void color_picker(pc_ui_shell_app *app, uint64_t id)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = doc ? yyjson_mut_obj(doc) : NULL;
    if (params && yyjson_mut_obj_add_uint(doc, params, "dialog", id) &&
        yyjson_mut_obj_add_strcpy(doc, params, "field", "color")) {
        pc_ui_shell_queue(app, "ui.dialog.colorField", params);
    }
    yyjson_mut_doc_free(doc);
}
void pc_ui_shell_fill_body(pc_ui_shell_app *app, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    const char *contents = yyjson_mut_get_str(yyjson_mut_obj_get(root, "contents"));
    if (!contents) {
        contents = "foreground";
    }
    /* Contents dropdown. */
    int selected = 0;
    const char *labels[9];
    for (int i = 0; i < 9; i++) {
        labels[i] = pc_i18n_t(PC_UI_FILL_CONTENT_LABELS[i]);
        if (!strcmp(contents, PC_UI_FILL_CONTENTS[i])) {
            selected = i;
        }
    }
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t("Contents:"), NK_TEXT_LEFT);
    pc_ui_shell_widget_mapping(app, "fill.contents", nk_widget_bounds(ctx));
    int next = nk_combo(ctx, labels, 9, selected, 24, nk_vec2(220, 280));
    if (next != selected && next >= 0 && next < 9) {
        selected = next;
        contents = PC_UI_FILL_CONTENTS[selected];
        put(dialog->fields, root, "contents", yyjson_mut_strcpy(dialog->fields, contents));
        if (!strcmp(contents, "color")) {
            color_picker(app, dialog->id);
        }
    }
    /* Per-contents row. */
    if (!strcmp(contents, "color")) {
        const char *hex = yyjson_mut_get_str(yyjson_mut_obj_get(root, "color"));
        float rgb[3] = {0, 0, 0};
        if (hex) {
            (void)pc_ui_picker_parse_hex(hex, strlen(hex), rgb);
        }
        nk_layout_row_dynamic(ctx, 26, 2);
        nk_label(ctx, pc_i18n_t("Color:"), NK_TEXT_LEFT);
        pc_ui_shell_widget_mapping(app, "fill.color", nk_widget_bounds(ctx));
        struct nk_color color =
            nk_rgba_f(fminf(1, fmaxf(0, rgb[0])), fminf(1, fmaxf(0, rgb[1])), fminf(1, fmaxf(0, rgb[2])), 1);
        if (nk_button_color(ctx, color)) {
            color_picker(app, dialog->id);
        }
    } else if (!strcmp(contents, "pattern")) {
        nk_layout_row_dynamic(ctx, 26, 2);
        nk_label(ctx, pc_i18n_t("Custom Pattern:"), NK_TEXT_LEFT);
        pc_ui_shell_pattern_picker(app, dialog, root, "pattern",
                                   yyjson_mut_get_str(yyjson_mut_obj_get(root, "pattern")), "fill");
    } else if (!strcmp(contents, "contentAware")) {
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_bool adapt = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "colorAdaptation"));
        if (nk_checkbox_label(ctx, pc_i18n_t("Color Adaptation"), &adapt)) {
            put(dialog->fields, root, "colorAdaptation", yyjson_mut_bool(dialog->fields, adapt != 0));
        }
    }
    /* Blending section. */
    const char *mode_name = yyjson_mut_get_str(yyjson_mut_obj_get(root, "mode"));
    pc_blend_mode mode = PC_BLEND_NORMAL;
    if (mode_name) {
        (void)pc_blend_from_name(mode_name, strlen(mode_name), &mode);
    }
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_label(ctx, pc_i18n_t("Mode:"), NK_TEXT_LEFT);
    pc_ui_shell_widget_mapping(app, "fill.mode", nk_widget_bounds(ctx));
    if (nk_combo_begin_label(ctx, pc_blend_label(mode), nk_vec2(240, 430))) {
        nk_layout_row_dynamic(ctx, 22, 1);
        for (size_t i = 0; i < PC_BLEND_LAYER_MODE_COUNT; i++) {
            if (nk_combo_item_label(ctx, pc_blend_label(PC_BLEND_LAYER_MODES[i]), NK_TEXT_LEFT)) {
                put(dialog->fields, root, "mode",
                    yyjson_mut_strcpy(dialog->fields, pc_blend_name(PC_BLEND_LAYER_MODES[i])));
            }
        }
        nk_combo_end(ctx);
    }
    nk_layout_row_dynamic(ctx, 26, 1);
    double opacity = yyjson_mut_get_num(yyjson_mut_obj_get(root, "opacity"));
    double before = opacity;
    nk_property_double(ctx, pc_i18n_t("Opacity"), 0, &opacity, 100, 1, .25f);
    if (opacity != before) {
        put(dialog->fields, root, "opacity", yyjson_mut_real(dialog->fields, opacity));
    }
    nk_layout_row_dynamic(ctx, 26, 1);
    nk_bool preserve = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "preserveTransparency"));
    if (nk_checkbox_label(ctx, pc_i18n_t("Preserve Transparency"), &preserve)) {
        put(dialog->fields, root, "preserveTransparency", yyjson_mut_bool(dialog->fields, preserve != 0));
    }
}
