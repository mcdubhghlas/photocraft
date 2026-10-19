#include "doc_props_ui.h"
#include "shell_internal.h"
#include "ui/sizing.h"
#include "ui/i18n.h"
#include "engine/prefs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static void resize(pc_ui_shell_app *app, double width, double height)
{
    if (!isfinite(width) || !isfinite(height) || width < 1 || height < 1 || width > 300000 || height > 300000)
        return;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    if (p && yyjson_mut_obj_add_uint(d, p, "width", (uint64_t)round(width)) &&
        yyjson_mut_obj_add_uint(d, p, "height", (uint64_t)round(height)))
        pc_ui_shell_queue(app, "image.canvasSize", p);
    yyjson_mut_doc_free(d);
}
static void toggle(pc_ui_shell_app *app, const char *label, const char *id, bool current)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    nk_bool next = current;
    if (nk_checkbox_label(ctx, pc_i18n_t(label), &next))
        pc_ui_shell_queue(app, id, NULL);
}
bool pc_ui_shell_doc_properties(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime,
                                const pc_engine_doc_state *state)
{
    if (!state)
        return false;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const pc_layer *layer = state->has_active_layer ? pc_document_layer(doc, state->active_layer) : NULL;
    if (layer && !pc_document_is_background(doc, layer->id)) {
        runtime->document_size.pending = false;
        return false;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_label(ctx, pc_i18n_t("Document"), NK_TEXT_LEFT);
    pc_json_value units = PC_JSON_VALUE_INIT;
    const char *unit_names[] = {"pixels", "inches", "cm", "mm", "points", "picas", "percent"};
    const char *unit_keys[] = {"px", "in", "cm", "mm", "pt", "pica", "percent"};
    const char *unit_labels[] = {"Pixels", "Inches", "Centimeters", "Millimeters",
                                 "Points", "Picas",  "Percent"};
    int unit = 0;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "unitsAndRulers.rulers", &units, NULL) == PC_OK) {
        const char *value = yyjson_mut_get_str(pc_json_value_root(&units));
        for (int i = 0; value && i < 7; i++)
            if (!strcmp(value, unit_names[i]))
                unit = i;
    }
    pc_json_value_fini(&units);
    if (runtime->document_size.pending &&
        (runtime->document_size.document != doc->id || runtime->document_size.revision != state->revision ||
         runtime->document_size.unit != unit))
        runtime->document_size.pending = false;
    double width = doc->size.width, height = doc->size.height;
    if (width >= 1 && height >= 1) {
        double shown_w = pc_ui_to_unit(width, unit_keys[unit], width, doc->resolution_dpi),
               shown_h = pc_ui_to_unit(height, unit_keys[unit], height, doc->resolution_dpi);
        if (runtime->document_size.pending) {
            shown_w =
                pc_ui_to_unit(runtime->document_size.width, unit_keys[unit], width, doc->resolution_dpi);
            shown_h =
                pc_ui_to_unit(runtime->document_size.height, unit_keys[unit], height, doc->resolution_dpi);
        }
        double next_w = shown_w, next_h = shown_h;
        nk_layout_row_dynamic(ctx, 26, 1);
        pc_ui_shell_widget_mapping(app, "document.width", nk_widget_bounds(ctx));
        nk_property_double(ctx, pc_i18n_t("Width"),
                           pc_ui_to_unit(1, unit_keys[unit], width, doc->resolution_dpi), &next_w,
                           pc_ui_to_unit(300000, unit_keys[unit], width, doc->resolution_dpi),
                           unit == 0 ? 1 : .1, unit == 0 ? 1 : .01f);
        pc_ui_shell_widget_mapping(app, "document.height", nk_widget_bounds(ctx));
        nk_property_double(ctx, pc_i18n_t("Height"),
                           pc_ui_to_unit(1, unit_keys[unit], height, doc->resolution_dpi), &next_h,
                           pc_ui_to_unit(300000, unit_keys[unit], height, doc->resolution_dpi),
                           unit == 0 ? 1 : .1, unit == 0 ? 1 : .01f);
        if (runtime->document_size.cancelled) {
            runtime->document_size.cancelled = runtime->document_size.pending = false;
            ctx->current->property.active = 0;
            ctx->current->edit.active = false;
        } else {
            if (next_w != shown_w || next_h != shown_h) {
                double px_w = pc_ui_from_unit(next_w, unit_keys[unit], width, doc->resolution_dpi),
                       px_h = pc_ui_from_unit(next_h, unit_keys[unit], height, doc->resolution_dpi);
                if (runtime->document_linked) {
                    if (next_w != shown_w)
                        px_h = height * px_w / width;
                    else
                        px_w = width * px_h / height;
                }
                runtime->document_size.pending = true;
                runtime->document_size.document = doc->id;
                runtime->document_size.revision = state->revision;
                runtime->document_size.unit = unit;
                runtime->document_size.width = px_w;
                runtime->document_size.height = px_h;
            }
            if (runtime->document_size.pending && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT) &&
                !ctx->current->property.active) {
                resize(app, runtime->document_size.width, runtime->document_size.height);
                runtime->document_size.pending = false;
            }
        }
        nk_layout_row_dynamic(ctx, 26, 1);
        nk_bool linked = runtime->document_linked;
        pc_ui_shell_widget_mapping(app, "document.link", nk_widget_bounds(ctx));
        if (nk_checkbox_label(ctx, pc_i18n_t("Link width and height"), &linked))
            runtime->document_linked = linked != 0;
        nk_layout_row_dynamic(ctx, 26, 2);
        pc_ui_shell_widget_mapping(app, "document.portrait", nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t("Portrait")) && width > height)
            resize(app, height, width);
        pc_ui_shell_widget_mapping(app, "document.landscape", nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t("Landscape")) && height > width)
            resize(app, height, width);
    }
    char resolution[96];
    (void)snprintf(resolution, sizeof resolution, "%.2f pixels/inch", (double)doc->resolution_dpi);
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_label(ctx, resolution, NK_TEXT_LEFT);
    const char *mode_labels[] = {"Bitmap",     "Grayscale", "Indexed Color", "RGB Color",
                                 "CMYK Color", "Lab Color", "Multichannel",  "Duotone"};
    const char *mode_commands[] = {
        "image.mode.bitmap", "image.mode.grayscale", "image.mode.indexedColor", "image.mode.rgb",
        "image.mode.cmyk",   "image.mode.lab",       "image.mode.multichannel", "image.mode.duotone"};
    int mode = doc->mode >= 0 && doc->mode < PC_COLOR_MODE_COUNT ? (int)doc->mode : 0;
    pc_ui_shell_widget_mapping(app, "document.mode", nk_widget_bounds(ctx));
    int next_mode = nk_combo(ctx, mode_labels, 8, mode, 24, nk_vec2(240, 250));
    if (next_mode >= 0 && next_mode < 8 && next_mode != mode)
        pc_ui_shell_queue(app, mode_commands[next_mode], NULL);
    const char *depth_labels[] = {"8 Bits/Channel", "16 Bits/Channel", "32 Bits/Channel"};
    const char *depth_commands[] = {"image.mode.bits8", "image.mode.bits16", "image.mode.bits32"};
    int depth = doc->depth == PC_SAMPLE_TYPE_U8 ? 0 : doc->depth == PC_SAMPLE_TYPE_U16 ? 1 : 2;
    pc_ui_shell_widget_mapping(app, "document.depth", nk_widget_bounds(ctx));
    int next_depth = nk_combo(ctx, depth_labels, 3, depth, 24, nk_vec2(240, 130));
    if (next_depth >= 0 && next_depth < 3 && next_depth != depth)
        pc_ui_shell_queue(app, depth_commands[next_depth], NULL);
    nk_layout_row_dynamic(ctx, 26, 1);
    pc_ui_shell_widget_mapping(app, "document.units", nk_widget_bounds(ctx));
    int next_unit = nk_combo(ctx, unit_labels, 7, unit, 24, nk_vec2(240, 240));
    if (next_unit >= 0 && next_unit < 7 && next_unit != unit) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
        if (p && yyjson_mut_obj_add_str(d, p, "path", "unitsAndRulers.rulers") &&
            yyjson_mut_obj_add_str(d, p, "value", unit_names[next_unit]))
            pc_ui_shell_queue(app, "prefs.set", p);
        yyjson_mut_doc_free(d);
    }
    nk_layout_row_dynamic(ctx, 26, 2);
    toggle(app, "Rulers", "view.rulers", ui->extras.rulers);
    toggle(app, "Grid", "view.show.grid", ui->extras.grid);
    toggle(app, "Guides", "view.show.guides", ui->extras.guides);
    toggle(app, "Lock Guides", "view.lockGuides", ui->extras.lock_guides);
    return true;
}
