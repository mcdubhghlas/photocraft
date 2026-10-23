#include "layer_picker_ui.h"
#include "artboard_ui.h"
#include "ui/i18n.h"
#include "doc_props_ui.h"
/* Native panel widgets. Every document action is queued until all panels finish borrowing it. */
#include "panels.h"
#include "layer_interaction.h"
#include "layer_thumbs_ui.h"
#include "type_controls.h"
#include "pen_ui.h"
#include "gradient_ui.h"
#include "pattern_picker_ui.h"
#include "preset_browser.h"
#include "ui/preset_thumb.h"
#include "ui/preset_ui.h"
#include "shell_internal.h"
#include "ui/layer_tree.h"
#include "ui/preset_swatch.h"
#include "ui/props_layout.h"
#include "ui/widgets.h"
#include "engine/channel_display.h"
#include "base/mem.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void pc_ui_shell_panel_hit(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const char *command,
                           pc_layer_id layer)
{
    if (runtime->nhits == 1024) {
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_rect rect = nk_widget_bounds(ctx), clip = ctx->current->layout->clip;
    float x = fmaxf(rect.x, clip.x), y = fmaxf(rect.y, clip.y);
    float right = fminf(rect.x + rect.w, clip.x + clip.w), bottom = fminf(rect.y + rect.h, clip.y + clip.h);
    if (right - x < 4 || bottom - y < 4) {
        return;
    }
    pc_ui_panel_hit *item = &runtime->hits[runtime->nhits++];
    (void)snprintf(item->command, sizeof item->command, "%s", command);
    item->layer = layer;
    item->rect[0] = x;
    item->rect[1] = y;
    item->rect[2] = right - x;
    item->rect[3] = bottom - y;
}
static void status(pc_ui_shell_app *app, const char *message)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    char *copy = pc_strndup(message, strlen(message));
    if (copy) {
        free(ui->status);
        ui->status = copy;
    }
    ui->status_error = true;
}
static void enqueue(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const char *id,
                    const yyjson_mut_val *params)
{
    pc_error err;
    if (pc_ui_command_queue_push(&runtime->commands, id, params, &err) != PC_OK) {
        status(app, err.msg);
    }
}
static yyjson_mut_doc *params(pc_ui_shell_app *app, yyjson_mut_val **root)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!*root) {
        yyjson_mut_doc_free(doc);
        status(app, "Not enough memory for this action");
        return NULL;
    }
    return doc;
}
static void command_button(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const char *label,
                           const char *id)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    bool enabled = pc_engine_session_is_enabled(pc_ui_shell_app_session(app), id);
    if (!enabled) {
        nk_widget_disable_begin(ctx);
    }
    pc_ui_shell_panel_hit(app, runtime, id, 0);
    if (nk_button_label(ctx, label) && enabled) {
        enqueue(app, runtime, id, NULL);
    }
    if (!enabled) {
        nk_widget_disable_end(ctx);
    }
}
static const pc_document *document(pc_ui_shell_app *app, const pc_engine_doc_state **state)
{
    *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    return *state ? pc_doc_arc_get((*state)->doc) : NULL;
}
static void layer_flag(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, pc_layer_id layer,
                       const char *field, bool value)
{
    yyjson_mut_val *root;
    yyjson_mut_doc *doc = params(app, &root);
    if (!doc) {
        return;
    }
    yyjson_mut_obj_add_uint(doc, root, "layer", layer);
    yyjson_mut_obj_add_bool(doc, root, field, value);
    enqueue(app, runtime, "layer.setProps", root);
    yyjson_mut_doc_free(doc);
}
static bool selected(const pc_layer_id *ids, size_t n, pc_layer_id id)
{
    for (size_t i = 0; i < n; i++) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}
static void filter_toggle(pc_ui_shell_app *app, const char *kind)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    for (size_t i = 0; i < ui->n_layer_filter; i++) {
        if (strcmp(ui->layer_filter[i], kind) == 0) {
            free(ui->layer_filter[i]);
            memmove(ui->layer_filter + i, ui->layer_filter + i + 1,
                    (ui->n_layer_filter - i - 1) * sizeof *ui->layer_filter);
            ui->n_layer_filter--;
            return;
        }
    }
    char *name = pc_strndup(kind, strlen(kind));
    if (!name) {
        status(app, "Not enough memory for layer filter");
        return;
    }
    char **grown = realloc(ui->layer_filter, (ui->n_layer_filter + 1) * sizeof *grown);
    if (!grown) {
        free(name);
        status(app, "Not enough memory for layer filter");
        return;
    }
    ui->layer_filter = grown;
    ui->layer_filter[ui->n_layer_filter++] = name;
}
static void layer_percent(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const pc_layer *layer,
                          bool is_fill)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float *value = is_fill ? &runtime->fill : &runtime->opacity;
    bool *pending = is_fill ? &runtime->fill_pending : &runtime->opacity_pending;
    if (!*pending) {
        *value = (is_fill ? layer->fill_opacity : layer->opacity) * 100;
    }
    float before = *value;
    pc_ui_shell_panel_hit(app, runtime, is_fill ? "layer.fill" : "layer.opacity", layer->id);
    nk_property_float(ctx, is_fill ? "Fill %" : "Opacity", 0, value, 100, 1, 1);
    if (*value != before) {
        *pending = true;
    }
    if (*pending && !nk_input_is_mouse_down(&ctx->input, NK_BUTTON_LEFT)) {
        yyjson_mut_val *root;
        yyjson_mut_doc *doc = params(app, &root);
        if (doc) {
            yyjson_mut_obj_add_uint(doc, root, "layer", layer->id);
            yyjson_mut_obj_add_real(doc, root, is_fill ? "fill" : "opacity", *value / 100);
            enqueue(app, runtime, "layer.setProps", root);
            yyjson_mut_doc_free(doc);
        }
        *pending = false;
    }
}
static void layers(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_doc_state *state;
    const pc_document *doc = document(app, &state);
    if (!doc) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("No document"), NK_TEXT_LEFT);
        return;
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const char *kinds[] = {"pixel", "adjustment", "type", "shape", "smart"};
    const char *labels[] = {"Pixel", "Adj", "Type", "Shape", "Smart"};
    nk_layout_row_dynamic(ctx, 20, 5);
    for (size_t i = 0; i < 5; i++) {
        nk_bool on = false;
        for (size_t j = 0; j < ui->n_layer_filter; j++) {
            on = on || strcmp(ui->layer_filter[j], kinds[i]) == 0;
        }
        nk_bool changed = on;
        if (nk_selectable_label(ctx, labels[i], NK_TEXT_CENTERED, &changed)) {
            filter_toggle(app, kinds[i]);
        }
    }
    const pc_layer *active = state->has_active_layer ? pc_document_layer(doc, state->active_layer) : NULL;
    if (active) {
        if (!runtime->editing || runtime->editing_layer != active->id) {
            runtime->editing = true;
            runtime->editing_layer = active->id;
            runtime->opacity_pending = runtime->fill_pending = false;
        }
        bool background = pc_document_is_background(doc, active->id);
        if (background) {
            nk_widget_disable_begin(ctx);
        }
        pc_blend_mode modes[PC_BLEND_COUNT];
        size_t n_modes = 0;
        for (pc_blend_mode mode = pc_layer_is_group(active) ? PC_BLEND_PASS_THROUGH : PC_BLEND_NORMAL;
             mode < PC_BLEND_COUNT; mode++) {
            modes[n_modes++] = mode;
        }
        nk_layout_row_dynamic(ctx, 24, 1);
        const char *label = pc_blend_label(active->blend);
        if (nk_combo_begin_label(ctx, label ? label : "Normal", nk_vec2(250, 300))) {
            nk_layout_row_dynamic(ctx, 20, 1);
            for (size_t i = 0; i < n_modes; i++) {
                if (nk_combo_item_label(ctx, pc_blend_label(modes[i]), NK_TEXT_LEFT)) {
                    yyjson_mut_val *root;
                    yyjson_mut_doc *p = params(app, &root);
                    if (p) {
                        yyjson_mut_obj_add_uint(p, root, "layer", active->id);
                        yyjson_mut_obj_add_str(p, root, "blend", pc_blend_name(modes[i]));
                        enqueue(app, runtime, "layer.setProps", root);
                        yyjson_mut_doc_free(p);
                    }
                }
            }
            nk_combo_end(ctx);
        }
        nk_layout_row_dynamic(ctx, 24, 2);
        layer_percent(app, runtime, active, false);
        layer_percent(app, runtime, active, true);
        if (background) {
            nk_widget_disable_end(ctx);
        }
    }
    pc_ui_layer_rows rows = {0};
    pc_error err;
    if (pc_ui_layer_rows_build(doc, (const char *const *)ui->layer_filter, ui->n_layer_filter, NULL, &rows,
                               &err) != PC_OK) {
        status(app, err.msg);
        return;
    }
    pc_layer_id *ids = NULL;
    size_t count = 0;
    (void)pc_engine_doc_state_selected_layers(state, &ids, &count, NULL);
    for (size_t i = 0; i < rows.count; i++) {
        const pc_ui_layer_row *row = &rows.items[i];
        const pc_layer *layer = row->layer;
        float indent = fminf((float)row->depth * 12, 90);
        int masks = (layer->has_mask ? 1 : 0) + (layer->has_vector_mask ? 1 : 0);
        nk_layout_row_begin(ctx, NK_STATIC, 32, 4 + masks * 2);
        nk_layout_row_push(ctx, 16 + indent);
        if (pc_layer_is_group(layer)) {
            pc_ui_shell_panel_hit(app, runtime, "layer.setExpanded", layer->id);
            if (nk_button_label(ctx, layer->content.u.group.expanded ? "v" : ">")) {
                yyjson_mut_val *root;
                yyjson_mut_doc *p = params(app, &root);
                if (p) {
                    yyjson_mut_obj_add_uint(p, root, "layer", layer->id);
                    yyjson_mut_obj_add_bool(p, root, "expanded", !layer->content.u.group.expanded);
                    yyjson_mut_obj_add_bool(p, root, "all",
                                            (pc_ui_shell_input_modifiers(app) & PC_UI_MOD_ALT) != 0);
                    enqueue(app, runtime, "layer.setExpanded", root);
                    yyjson_mut_doc_free(p);
                }
            }
        } else {
            nk_spacing(ctx, 1);
        }
        nk_layout_row_push(ctx, 24);
        pc_ui_shell_panel_hit(app, runtime, "layer.visible", layer->id);
        nk_bool visible = layer->visible;
        if (nk_checkbox_label(ctx, pc_i18n_t(""), &visible)) {
            layer_flag(app, runtime, layer->id, "visible", visible);
        }
        nk_layout_row_push(ctx, 30);
        pc_ui_layer_thumb_widget(app, runtime, state, layer, PC_UI_THUMB_LAYER);
        if (layer->has_mask) {
            nk_layout_row_push(ctx, 16);
            pc_ui_layer_mask_chain(app, runtime, layer, false);
            nk_layout_row_push(ctx, 30);
            pc_ui_layer_thumb_widget(app, runtime, state, layer, PC_UI_THUMB_PIXEL_MASK);
        }
        if (layer->has_vector_mask) {
            nk_layout_row_push(ctx, 16);
            pc_ui_layer_mask_chain(app, runtime, layer, true);
            nk_layout_row_push(ctx, 30);
            pc_ui_layer_thumb_widget(app, runtime, state, layer, PC_UI_THUMB_VECTOR_MASK);
        }
        nk_layout_row_push(ctx,
                           fmaxf(40, nk_window_get_content_region(ctx).w - indent - 100 - (float)masks * 50));
        pc_ui_shell_panel_hit(app, runtime, "layer.select", layer->id);
        nk_bool is_selected = selected(ids, count, layer->id);
        if (!pc_ui_layer_rename_row(app, runtime, state, layer) &&
            nk_selectable_label(ctx, pc_str_c(&layer->name), NK_TEXT_LEFT, &is_selected)) {
            yyjson_mut_val *root;
            yyjson_mut_doc *p = params(app, &root);
            if (p) {
                unsigned mods = pc_ui_shell_input_modifiers(app);
                yyjson_mut_obj_add_uint(p, root, "layer", layer->id);
                yyjson_mut_obj_add_str(p, root, "mode",
                                       mods & PC_UI_MOD_SHIFT     ? "range"
                                       : mods & PC_UI_MOD_COMMAND ? "toggle"
                                                                  : "replace");
                enqueue(app, runtime, "layer.select", root);
                yyjson_mut_doc_free(p);
            }
        }
        nk_layout_row_end(ctx);
    }
    free(ids);
    pc_ui_layer_rows_fini(&rows);
    pc_ui_layer_drag_feedback(app, runtime);
    nk_layout_row_dynamic(ctx, 24, 4);
    command_button(app, runtime, "+", "layer.new.layer");
    command_button(app, runtime, "Group", "layer.new.group");
    command_button(app, runtime, "Mask", "layer.layerMask.revealAll");
    command_button(app, runtime, "Delete", "layer.delete");
}
static void colors(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    nk_layout_row_dynamic(ctx, 22, 2);
    if (nk_option_label(ctx, pc_i18n_t("Foreground"), !ui->color_panel.background)) {
        ui->color_panel.background = false;
    }
    if (nk_option_label(ctx, pc_i18n_t("Background"), ui->color_panel.background)) {
        ui->color_panel.background = true;
    }
    const float *source = ui->color_panel.background ? tools->background : tools->foreground;
    struct nk_colorf before = {source[0], source[1], source[2], source[3]};
    nk_layout_row_dynamic(ctx, 100, 1);
    struct nk_colorf picked = nk_color_picker(ctx, before, NK_RGB);
    if (memcmp(&picked, &before, sizeof picked) != 0) {
        yyjson_mut_val *root;
        yyjson_mut_doc *p = params(app, &root);
        if (p) {
            yyjson_mut_val *rgba = yyjson_mut_arr(p);
            yyjson_mut_arr_add_real(p, rgba, picked.r);
            yyjson_mut_arr_add_real(p, rgba, picked.g);
            yyjson_mut_arr_add_real(p, rgba, picked.b);
            yyjson_mut_arr_add_real(p, rgba, picked.a);
            yyjson_mut_obj_add_val(p, root, ui->color_panel.background ? "background" : "foreground", rgba);
            enqueue(app, runtime, "tools.setColors", root);
            yyjson_mut_doc_free(p);
        }
    }
    nk_layout_row_dynamic(ctx, 22, 2);
    command_button(app, runtime, "Swap", "tools.swapColors");
    command_button(app, runtime, "Default", "tools.defaultColors");
}
static void properties(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_doc_state *state;
    const pc_document *doc = document(app, &state);
    nk_layout_row_dynamic(ctx, 22, 1);
    if (!doc) {
        nk_label(ctx, pc_i18n_t("No document"), NK_TEXT_LEFT);
        return;
    }
    if (pc_ui_shell_doc_properties(app, runtime, state))
        return;
    const pc_layer *layer = state->has_active_layer ? pc_document_layer(doc, state->active_layer) : NULL;
    char line[256];
    (void)snprintf(line, sizeof line, "%u x %u px", doc->size.width, doc->size.height);
    nk_label(ctx, line, NK_TEXT_LEFT);
    if (!layer) {
        return;
    }
    if (runtime->name_layer != layer->id) {
        runtime->name_layer = layer->id;
        (void)snprintf(runtime->name, sizeof runtime->name, "%s", pc_str_c(&layer->name));
    }
    if (!runtime->name[0]) {
        (void)snprintf(runtime->name, sizeof runtime->name, "%s", pc_str_c(&layer->name));
    }
    nk_flags edited =
        nk_edit_string_zero_terminated(ctx, (nk_flags)NK_EDIT_FIELD | (nk_flags)NK_EDIT_SIG_ENTER,
                                       runtime->name, sizeof runtime->name, nk_filter_default);
    if (edited & NK_EDIT_COMMITED) {
        yyjson_mut_val *root;
        yyjson_mut_doc *p = params(app, &root);
        if (p) {
            yyjson_mut_obj_add_uint(p, root, "layer", layer->id);
            yyjson_mut_obj_add_strcpy(p, root, "name", runtime->name);
            enqueue(app, runtime, "layer.renameLayer", root);
            yyjson_mut_doc_free(p);
        }
    }
    nk_layout_row_dynamic(ctx, 22, 1);
    pc_ui_shell_layer_picker_controls(app, layer);
    if (pc_layer_artboard(layer)) {
        pc_ui_shell_artboard_properties(app, layer);
        return;
    }
    nk_label(ctx, pc_ui_layer_filter_kind(layer->content.kind), NK_TEXT_LEFT);
    if (layer->content.kind == PC_LAYER_FILL && layer->content.u.fill.kind == PC_FILL_GRADIENT) {
        pc_ui_shell_gradient_properties(app, layer);
    }
    size_t count;
    const pc_ui_quick_action *actions = pc_ui_props_quick_actions(layer->content.kind, &count);
    for (size_t i = 0; i < count; i++) {
        nk_layout_row_dynamic(ctx, 24, 1);
        command_button(app, runtime, actions[i].label, actions[i].id);
    }
}
static yyjson_mut_val *query(pc_ui_shell_app *app, const char *id, pc_json_value *out)
{
    pc_error err;
    if (pc_engine_session_execute(pc_ui_shell_app_session(app), id, NULL, out, &err) != PC_OK) {
        status(app, err.msg);
        return NULL;
    }
    return pc_json_value_root(out);
}
static const char *string_field(yyjson_mut_val *object, const char *field)
{
    const char *text = yyjson_mut_get_str(yyjson_mut_obj_get(object, field));
    return text ? text : "";
}
static void channel_row(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const char *label,
                        const char *name, size_t index, bool numeric, bool visible, bool target, bool eye)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_begin(ctx, NK_STATIC, 26, 2);
    nk_layout_row_push(ctx, 26);
    nk_bool shown = visible;
    bool changed = eye ? nk_checkbox_label(ctx, pc_i18n_t(""), &shown) : false;
    if (!eye) {
        nk_spacing(ctx, 1);
    }
    nk_layout_row_push(ctx, fmaxf(40, nk_window_get_content_region(ctx).w - 42));
    nk_bool chosen = target;
    bool clicked = nk_selectable_label(ctx, label, NK_TEXT_LEFT, &chosen);
    nk_layout_row_end(ctx);
    if (!changed && !clicked) {
        return;
    }
    yyjson_mut_val *root;
    yyjson_mut_doc *p = params(app, &root);
    if (!p) {
        return;
    }
    if (numeric) {
        yyjson_mut_obj_add_uint(p, root, "channel", index);
    } else {
        yyjson_mut_obj_add_strcpy(p, root, "channel", name);
    }
    const char *id = "channel.target";
    if (changed) {
        id = "channel.setVisible";
        yyjson_mut_obj_add_bool(p, root, "visible", shown);
    } else if ((pc_ui_shell_input_modifiers(app) & PC_UI_MOD_COMMAND) != 0) {
        id = "select.loadSelection";
        unsigned mods = pc_ui_shell_input_modifiers(app);
        yyjson_mut_obj_add_str(p, root, "operation",
                               (mods & PC_UI_MOD_SHIFT) && (mods & PC_UI_MOD_ALT) ? "intersect"
                               : mods & PC_UI_MOD_SHIFT                           ? "add"
                               : mods & PC_UI_MOD_ALT                             ? "subtract"
                                                                                  : "new");
    }
    enqueue(app, runtime, id, root);
    yyjson_mut_doc_free(p);
}
static void channels(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_doc_state *state;
    const pc_document *doc = document(app, &state);
    if (!doc) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("No document"), NK_TEXT_LEFT);
        return;
    }
    pc_json_value data = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = query(app, "channel.list", &data);
    if (!root) {
        return;
    }
    yyjson_mut_val *target = yyjson_mut_obj_get(root, "target");
    const char *kind = string_field(target, "kind");
    size_t targeted = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(target, "index"));
    bool multi = doc->mode == PC_COLOR_MODE_MULTICHANNEL;
    if (!multi) {
        channel_row(app, runtime, string_field(root, "composite"), "composite", 0, false,
                    yyjson_mut_get_bool(yyjson_mut_obj_get(root, "compositeVisible")),
                    strcmp(kind, "composite") == 0, true);
    }
    yyjson_mut_val *items = yyjson_mut_obj_get(root, "colors");
    size_t count = yyjson_mut_arr_size(items);
    if (!multi && count > 1) {
        for (size_t i = 0; i < count; i++) {
            yyjson_mut_val *row = yyjson_mut_arr_get(items, i);
            const char *name = string_field(row, "name");
            channel_row(app, runtime, name, name, 0, false,
                        yyjson_mut_get_bool(yyjson_mut_obj_get(row, "visible")),
                        strcmp(kind, "color") == 0 && targeted == i, true);
        }
    }
    const pc_layer *layer = state->has_active_layer ? pc_document_layer(doc, state->active_layer) : NULL;
    if (layer && layer->has_mask) {
        channel_row(app, runtime, "Layer Mask", "mask", 0, false, true, strcmp(kind, "layerMask") == 0,
                    false);
    }
    items = yyjson_mut_obj_get(root, "alpha");
    count = yyjson_mut_arr_size(items);
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(items, i);
        channel_row(app, runtime, string_field(row, "name"), NULL, i, true,
                    yyjson_mut_get_bool(yyjson_mut_obj_get(row, "visible")),
                    strcmp(kind, "alpha") == 0 && targeted == i, true);
    }
    yyjson_mut_val *quick = yyjson_mut_obj_get(root, "quickMask");
    if (yyjson_mut_is_obj(quick)) {
        channel_row(app, runtime, "Quick Mask", "quickMask", 0, false,
                    yyjson_mut_get_bool(yyjson_mut_obj_get(quick, "visible")), false, true);
    }
    nk_layout_row_dynamic(ctx, 24, 3);
    command_button(app, runtime, "Save selection", "select.saveSelection");
    command_button(app, runtime, "New", "channel.new");
    command_button(app, runtime, "Delete", "channel.delete");
    pc_json_value_fini(&data);
}
static void swatches(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_json_value data = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = query(app, "swatches.list", &data);
    yyjson_mut_val *groups = yyjson_mut_obj_get(root, "groups");
    for (size_t g = 0; g < yyjson_mut_arr_size(groups); g++) {
        yyjson_mut_val *group = yyjson_mut_arr_get(groups, g);
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, string_field(group, "name"), NK_TEXT_LEFT);
        yyjson_mut_val *items = yyjson_mut_obj_get(group, "swatches");
        size_t count = yyjson_mut_arr_size(items);
        nk_layout_row_dynamic(ctx, 24, 8);
        for (size_t i = 0; i < count; i++) {
            yyjson_mut_val *swatch = yyjson_mut_arr_get(items, i);
            const char *hex = string_field(swatch, "hex");
            size_t hex_len = strlen(hex);
            struct nk_color color =
                hex_len == 6 || (hex_len == 7 && hex[0] == '#') ? nk_rgb_hex(hex) : nk_rgb(0, 0, 0);
            if (nk_button_color(ctx, color)) {
                yyjson_mut_val *p;
                yyjson_mut_doc *d = params(app, &p);
                if (d) {
                    yyjson_mut_obj_add_strcpy(d, p, "swatch", string_field(swatch, "name"));
                    yyjson_mut_obj_add_strcpy(d, p, "group", string_field(group, "name"));
                    yyjson_mut_obj_add_str(d, p, "target",
                                           (pc_ui_shell_input_modifiers(app) & PC_UI_MOD_ALT) ? "background"
                                                                                              : "foreground");
                    enqueue(app, runtime, "swatches.use", p);
                    yyjson_mut_doc_free(d);
                }
            }
            if (nk_widget_is_hovered(ctx)) {
                nk_tooltip(ctx, string_field(swatch, "name"));
            }
        }
    }
    nk_layout_row_dynamic(ctx, 24, 2);
    command_button(app, runtime, "New swatch", "swatches.add");
    command_button(app, runtime, "New group", "swatches.newGroup");
    pc_json_value_fini(&data);
}
static void actions(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_json_value data = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = query(app, "actions.list", &data);
    yyjson_mut_val *items = yyjson_mut_obj_get(root, "actions");
    size_t count = yyjson_mut_arr_size(items);
    for (size_t i = 0; i < count; i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(items, i);
        nk_bool chosen = runtime->action == i;
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_selectable_label(ctx, string_field(row, "name"), NK_TEXT_LEFT, &chosen)) {
            runtime->action = i;
        }
    }
    nk_layout_row_dynamic(ctx, 24, 4);
    command_button(app, runtime, "Record", "actions.record");
    command_button(app, runtime, "Stop", "actions.stop");
    bool enabled = runtime->action < count;
    if (!enabled) {
        nk_widget_disable_begin(ctx);
    }
    bool play = nk_button_label(ctx, pc_i18n_t("Play")), remove = nk_button_label(ctx, pc_i18n_t("Delete"));
    if (!enabled) {
        nk_widget_disable_end(ctx);
    }
    if (enabled && (play || remove)) {
        yyjson_mut_val *p;
        yyjson_mut_doc *d = params(app, &p);
        if (d) {
            yyjson_mut_obj_add_uint(d, p, "action", runtime->action);
            enqueue(app, runtime, play ? "actions.play" : "actions.delete", p);
            yyjson_mut_doc_free(d);
        }
    }
    pc_json_value_fini(&data);
}
static void comps(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_doc_state *state;
    const pc_document *doc = document(app, &state);
    if (!doc) {
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, pc_i18n_t("No document"), NK_TEXT_LEFT);
        return;
    }
    for (size_t i = 0; i < doc->nlayer_comps; i++) {
        nk_layout_row_dynamic(ctx, 24, 1);
        if (nk_button_label(ctx, pc_str_c(&doc->layer_comps[i].name))) {
            yyjson_mut_val *p;
            yyjson_mut_doc *d = params(app, &p);
            if (d) {
                yyjson_mut_obj_add_uint(d, p, "comp", doc->layer_comps[i].id);
                enqueue(app, runtime, "layerComp.apply", p);
                yyjson_mut_doc_free(d);
            }
        }
    }
    nk_layout_row_dynamic(ctx, 24, 3);
    command_button(app, runtime, "New", "layerComp.new");
    command_button(app, runtime, "Update", "layerComp.update");
    command_button(app, runtime, "Delete", "layerComp.delete");
}
static void history(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_doc_state *state;
    const pc_document *doc = document(app, &state);
    nk_layout_row_dynamic(ctx, 24, 1);
    if (!doc) {
        nk_label(ctx, pc_i18n_t("No document"), NK_TEXT_LEFT);
        return;
    }
    nk_label(ctx, pc_str_c(&doc->name), NK_TEXT_LEFT);
    pc_json_value data = PC_JSON_VALUE_INIT;
    if (pc_engine_inspect_history(state, &data, NULL) != PC_OK) {
        return;
    }
    yyjson_mut_val *root = pc_json_value_root(&data), *snapshots = yyjson_mut_obj_get(root, "snapshots");
    for (size_t i = 0; i < yyjson_mut_arr_size(snapshots); i++) {
        yyjson_mut_val *row = yyjson_mut_arr_get(snapshots, i);
        uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(row, "id"));
        nk_layout_row_dynamic(ctx, 24, 1);
        pc_ui_shell_panel_hit(app, runtime, "history.selectSnapshot", id);
        if (nk_button_label(ctx, string_field(row, "name"))) {
            runtime->snapshot_selected = true;
            runtime->snapshot = id;
            yyjson_mut_val *p;
            yyjson_mut_doc *d = params(app, &p);
            if (d) {
                yyjson_mut_obj_add_uint(d, p, "snapshot", id);
                enqueue(app, runtime, "history.selectSnapshot", p);
                yyjson_mut_doc_free(d);
            }
        }
    }
    yyjson_mut_val *states = yyjson_mut_obj_get(root, "states"), *redo = yyjson_mut_obj_get(root, "redo");
    size_t count = yyjson_mut_arr_size(states), future = yyjson_mut_arr_size(redo);
    size_t current = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "current"));
    for (size_t i = 0; i < count + future; i++) {
        const char *label = yyjson_mut_get_str(i < count ? yyjson_mut_arr_get(states, i)
                                                         : yyjson_mut_arr_get(redo, i - count));
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_bool chosen = !runtime->snapshot_selected && i == current;
        pc_ui_shell_panel_hit(app, runtime, "history.selectState", i);
        if (nk_selectable_label(ctx, label ? label : "", NK_TEXT_LEFT, &chosen)) {
            runtime->snapshot_selected = false;
            yyjson_mut_val *p;
            yyjson_mut_doc *d = params(app, &p);
            if (d) {
                yyjson_mut_obj_add_uint(d, p, "state", i);
                enqueue(app, runtime, "history.selectState", p);
                yyjson_mut_doc_free(d);
            }
        }
    }
    nk_layout_row_dynamic(ctx, 24, 3);
    command_button(app, runtime, "Snapshot", "history.newSnapshot");
    command_button(app, runtime, "New doc", "history.newDocument");
    if (runtime->snapshot_selected) {
        pc_ui_shell_panel_hit(app, runtime, "history.deleteSnapshot", runtime->snapshot);
        if (nk_button_label(ctx, pc_i18n_t("Delete"))) {
            yyjson_mut_val *p;
            yyjson_mut_doc *d = params(app, &p);
            if (d) {
                yyjson_mut_obj_add_uint(d, p, "snapshot", runtime->snapshot);
                enqueue(app, runtime, "history.deleteSnapshot", p);
                yyjson_mut_doc_free(d);
                runtime->snapshot_selected = false;
            }
        }
    } else {
        command_button(app, runtime, "Delete", "history.deleteState");
    }
    pc_json_value_fini(&data);
}
static void enqueue_str(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const char *id,
                        const char *field, const char *value)
{
    yyjson_mut_val *root;
    yyjson_mut_doc *d = params(app, &root);
    if (!d) {
        return;
    }
    yyjson_mut_obj_add_strcpy(d, root, field, value);
    enqueue(app, runtime, id, root);
    yyjson_mut_doc_free(d);
}
static void draw_gradient_bar(struct nk_context *ctx, struct nk_rect bar, yyjson_mut_val *stops,
                              const float *fg, const float *bg)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
    if (!canvas || bar.w <= 0 || bar.h <= 0) {
        return;
    }
    float locs[64], rgb[64][3];
    size_t n = 0, count = yyjson_mut_arr_size(stops);
    for (size_t i = 0; i < count && n < 64; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr_get(stops, i);
        if (!yyjson_mut_is_arr(pair) || yyjson_mut_arr_size(pair) < 2) {
            continue;
        }
        const char *tok = yyjson_mut_get_str(yyjson_mut_arr_get(pair, 1));
        float c[3];
        if (!tok || !pc_ui_preset_stop_color(tok, fg, bg, c)) {
            continue;
        }
        locs[n] = (float)yyjson_mut_get_num(yyjson_mut_arr_get(pair, 0));
        rgb[n][0] = c[0];
        rgb[n][1] = c[1];
        rgb[n][2] = c[2];
        n++;
    }
    if (n == 0) {
        nk_fill_rect(canvas, bar, 0, nk_rgb(128, 128, 128));
        return;
    }
    const float slices = 96.0f;
    for (int i = 0; i < 96; i++) {
        float t = (float)i / (slices - 1.0f), c[3];
        pc_ui_preset_gradient_sample(locs, (const float(*)[3])rgb, n, t, c);
        nk_fill_rect(canvas,
                     nk_rect(bar.x + (float)i / slices * bar.w, bar.y, fmaxf(1.0f, bar.w / slices), bar.h), 0,
                     nk_rgb_f(c[0], c[1], c[2]));
    }
}
/* U5 browser thumbnails. `user` carries the list-result JSON kept alive for the whole browser call,
 * so (gi,ii) index back into it in library order. */
struct gradient_thumb_user {
    yyjson_mut_val *groups;
    const float *fg, *bg;
};
static void gradient_thumb(pc_ui_shell_app *app, void *user, struct nk_rect r, size_t gi, size_t ii,
                           bool list)
{
    (void)list;
    struct gradient_thumb_user *u = user;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *presets = yyjson_mut_obj_get(yyjson_mut_arr_get(u->groups, gi), "presets");
    yyjson_mut_val *preset = yyjson_mut_arr_get(presets, ii);
    draw_gradient_bar(ctx, r, yyjson_mut_obj_get(preset, "stops"), u->fg, u->bg);
}
static void pattern_thumb(pc_ui_shell_app *app, void *user, struct nk_rect r, size_t gi, size_t ii,
                          bool list)
{
    const pc_ui_preset_group_data *gd = user;
    if (gi >= gd->ngroups || ii >= gd->groups[gi].n) {
        return;
    }
    const char *id = gd->groups[gi].items[ii].key;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *cv = nk_window_get_canvas(ctx);
    uint32_t tpx = pc_ui_preset_thumb_px(list ? 20 : fmaxf(8, r.w), (double)pc_ui_shell_app_ui_scale(app));
    struct nk_image thumb;
    if (pc_ui_shell_pattern_thumbnail_image(app, id, tpx, &thumb)) {
        nk_draw_image(cv, r, &thumb, nk_rgba(255, 255, 255, 255));
    } else {
        nk_fill_rect(cv, r, 2, nk_rgb(96, 96, 96));
    }
}
/* Apply a Select/Activate/Drop event from the shared browser for the Gradients/Patterns panels:
 * record the selection and queue the engine command (document mutation). */
static void dispatch_preset(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const char *panel,
                            const char *prefix, const char *field, const pc_ui_preset_event *ev)
{
    pc_ui_preset_ui_set_selected(&pc_ui_shell_app_state(app)->presets_ui, panel, ev->key);
    char id[96];
    (void)snprintf(id, sizeof id, "%s.%s", prefix,
                   ev->kind == PC_UI_PRESET_EV_SELECT ? "select" : "apply");
    enqueue_str(app, runtime, id, field, ev->key);
}
static void gradients(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
    const float *fg = tools->foreground, *bg = tools->background;
    pc_json_value data = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = query(app, "gradient.presets.list", &data);
    if (!root) {
        return;
    }
    yyjson_mut_val *cur = yyjson_mut_obj_get(root, "current");
    if (cur) {
        nk_layout_row_dynamic(ctx, 20, 1);
        struct nk_rect bar;
        (void)nk_widget(&bar, ctx);
        draw_gradient_bar(ctx, bar, yyjson_mut_obj_get(cur, "stops"), fg, bg);
    }
    pc_ui_preset_group_data gd;
    if (pc_ui_preset_groups_from_json(root, "presets", "name", &gd)) {
        struct gradient_thumb_user u = {yyjson_mut_obj_get(root, "groups"), fg, bg};
        pc_ui_preset_browser info = {.panel = "gradients",
                                     .prefix = "gradient.presets",
                                     .new_tip = pc_i18n_t("Create new gradient"),
                                     .new_name = "Custom",
                                     .rename_after_new = true,
                                     .groups = gd.groups,
                                     .ngroups = gd.ngroups,
                                     .thumb = gradient_thumb,
                                     .user = &u};
        pc_ui_preset_event ev;
        if (pc_ui_shell_preset_browser(app, runtime, &info, &ev)) {
            dispatch_preset(app, runtime, "gradients", "gradient.presets", "preset", &ev);
        }
        pc_ui_preset_group_data_fini(&gd);
    }
    pc_json_value_fini(&data);
}
static void patterns(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    pc_json_value data = PC_JSON_VALUE_INIT;
    yyjson_mut_val *root = query(app, "pattern.presets.list", &data);
    if (!root) {
        return;
    }
    pc_ui_preset_group_data gd;
    if (pc_ui_preset_groups_from_json(root, "patterns", "id", &gd)) {
        pc_ui_preset_browser info = {.panel = "patterns",
                                     .prefix = "pattern.presets",
                                     .new_tip = pc_i18n_t("Create new pattern from the selection"),
                                     .new_name = NULL,
                                     .rename_after_new = false,
                                     .groups = gd.groups,
                                     .ngroups = gd.ngroups,
                                     .thumb = pattern_thumb,
                                     .user = &gd};
        pc_ui_preset_event ev;
        if (pc_ui_shell_preset_browser(app, runtime, &info, &ev)) {
            dispatch_preset(app, runtime, "patterns", "pattern.presets", "pattern", &ev);
        }
        pc_ui_preset_group_data_fini(&gd);
    }
    pc_json_value_fini(&data);
}
static pc_status invoke(void *app, const char *id, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return pc_ui_shell_app_invoke(app, id, p, out, err);
}
void pc_ui_shell_panel_apply(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime)
{
    pc_error err;
    if (pc_ui_command_queue_apply(&runtime->commands, invoke, app, &err) != PC_OK) {
        status(app, err.msg);
    }
}
void pc_ui_shell_panel_draw(pc_ui_shell_app *app, pc_ui_panel_runtime *runtime, const char *panel)
{
    if (strcmp(panel, "Layers") == 0) {
        layers(app, runtime);
    } else if (strcmp(panel, "Color") == 0) {
        colors(app, runtime);
    } else if (strcmp(panel, "Channels") == 0) {
        channels(app, runtime);
    } else if (strcmp(panel, "Swatches") == 0) {
        swatches(app, runtime);
    } else if (strcmp(panel, "History") == 0) {
        history(app, runtime);
    } else if (strcmp(panel, "Actions") == 0) {
        actions(app, runtime);
    } else if (strcmp(panel, "Layer Comps") == 0) {
        comps(app, runtime);
    } else if (strcmp(panel, "Gradients") == 0) {
        gradients(app, runtime);
    } else if (strcmp(panel, "Patterns") == 0) {
        patterns(app, runtime);
    } else if (!strcmp(panel, "Paths")) {
        pc_ui_shell_paths_panel(app);
    } else if (!strcmp(panel, "Character") || !strcmp(panel, "Paragraph")) {
        pc_ui_shell_type_panel(app, !strcmp(panel, "Paragraph"));
    } else if (strcmp(panel, "Properties") == 0) {
        properties(app, runtime);
    } else {
        struct nk_context *ctx = pc_ui_shell_app_nk(app);
        nk_layout_row_dynamic(ctx, 20, 1);
        nk_label(ctx, panel, NK_TEXT_LEFT);
    }
}
