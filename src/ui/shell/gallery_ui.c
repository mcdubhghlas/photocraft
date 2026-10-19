#include "ui/i18n.h"
#include "gallery_ui.h"
#include "filter_ui.h"
#include "shell_internal.h"
#include "engine/gallery_catalog.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static void put(yyjson_mut_doc *d, yyjson_mut_val *o, const char *key, yyjson_mut_val *v)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(d, key);
    if (k && v)
        (void)yyjson_mut_obj_put(o, k, v);
}
static const char *str(yyjson_mut_val *o, const char *key)
{
    const char *s = yyjson_mut_get_str(yyjson_mut_obj_get(o, key));
    return s ? s : "";
}
static yyjson_mut_val *effect(yyjson_mut_doc *d, yyjson_mut_val *catalogue)
{
    pc_ui_parameter_schema schema = {0};
    if (pc_ui_parameter_schema_parse(&schema, str(catalogue, "params"), NULL) != PC_OK)
        return NULL;
    yyjson_mut_val *o = yyjson_mut_obj(d), *params = yyjson_mut_obj(d);
    bool ok = o && params && pc_ui_parameter_schema_defaults(&schema, d, params, NULL) == PC_OK &&
              yyjson_mut_obj_add_strcpy(d, o, "filter", str(catalogue, "key")) &&
              yyjson_mut_obj_add_val(d, o, "params", params) &&
              yyjson_mut_obj_add_bool(d, o, "visible", true);
    pc_ui_parameter_schema_fini(&schema);
    return ok ? o : NULL;
}
pc_status pc_ui_shell_gallery_init(pc_ui_shell_app *app, yyjson_mut_doc *d, pc_error *err)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_gallery_catalogue(&out, err);
    yyjson_mut_val *catalogue = status == PC_OK ? yyjson_mut_val_mut_copy(d, pc_json_value_root(&out)) : NULL,
                   *root = yyjson_mut_doc_get_root(d), *effects = yyjson_mut_arr(d),
                   *filters = yyjson_mut_obj_get(catalogue, "filters"),
                   *first = yyjson_mut_arr_size(filters) ? effect(d, yyjson_mut_arr_get(filters, 0)) : NULL;
    if (status == PC_OK && (!catalogue || !effects || !first || !yyjson_mut_arr_add_val(effects, first)))
        status = pc_fail(err, PC_ERR_NOMEM, "Filter Gallery catalogue");
    if (status == PC_OK) {
        put(d, root, "__gallery", yyjson_mut_bool(d, true));
        put(d, root, "__catalogue", catalogue);
        put(d, root, "__gallerySelected", yyjson_mut_uint(d, 0));
        put(d, root, "effects", effects);
        (void)yyjson_mut_obj_remove_str(root, "list");
        const pc_engine_tool_state *tools = pc_engine_session_tools(pc_ui_shell_app_session(app));
        const char *keys[] = {"foreground", "background"};
        const float *colors[] = {tools->foreground, tools->background};
        for (size_t c = 0; c < 2; c++) {
            yyjson_mut_val *rgba = yyjson_mut_arr(d);
            for (size_t i = 0; rgba && i < 4; i++)
                if (!yyjson_mut_arr_add_real(d, rgba, colors[c][i]))
                    rgba = NULL;
            if (!rgba) {
                status = pc_fail(err, PC_ERR_NOMEM, "Filter Gallery colours");
                break;
            }
            put(d, root, keys[c], rgba);
        }
    }
    pc_json_value_fini(&out);
    return status;
}
static void controls(pc_ui_shell_app *app, pc_ui_dialog *dialog, yyjson_mut_val *item,
                     yyjson_mut_val *catalogue)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_parameter_schema schema = {0};
    if (pc_ui_parameter_schema_parse(&schema, str(catalogue, "params"), NULL) != PC_OK)
        return;
    yyjson_mut_val *params = yyjson_mut_obj_get(item, "params");
    nk_layout_row_dynamic(ctx, 28, 1);
    nk_label(ctx, str(catalogue, "name"), NK_TEXT_LEFT);
    for (size_t i = 0; i < schema.count; i++) {
        const pc_ui_param *p = &schema.items[i];
        if (!strcmp(p->key, "layer") || p->kind == PC_UI_PARAM_STRUCTURED)
            continue;
        char label[128];
        pc_ui_shell_parameter_label(p->key, label);
        yyjson_mut_val *v = yyjson_mut_obj_get(params, p->key);
        nk_layout_row_dynamic(ctx, 28, 1);
        char mapping[128];
        (void)snprintf(mapping, sizeof mapping, "gallery.%s", p->key);
        pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
        if (p->kind == PC_UI_PARAM_RANGE || p->kind == PC_UI_PARAM_INTEGER) {
            double value = yyjson_mut_is_num(v) ? yyjson_mut_get_num(v) : p->number;
            if (!isfinite(value))
                value = p->number;
            double next = value, min = p->kind == PC_UI_PARAM_RANGE ? p->min : 0,
                   max = p->kind == PC_UI_PARAM_RANGE ? p->max : 2147483647;
            nk_property_double(ctx, label, min, &next, max, 1, .1f);
            if (next != value)
                put(dialog->fields, params, p->key,
                    p->kind == PC_UI_PARAM_INTEGER
                        ? yyjson_mut_sint(dialog->fields, (int64_t)fmin(p->max, fmax(p->min, round(next))))
                        : yyjson_mut_real(dialog->fields, next));
        } else if (p->kind == PC_UI_PARAM_BOOL) {
            nk_bool old = v ? yyjson_mut_get_bool(v) : p->boolean, next = old;
            if (nk_checkbox_label(ctx, label, &next))
                put(dialog->fields, params, p->key, yyjson_mut_bool(dialog->fields, next != 0));
        } else if (p->kind == PC_UI_PARAM_CHOICE) {
            int selected = 0;
            const char *current = yyjson_mut_get_str(v);
            for (size_t j = 0; current && j < p->nchoices; j++)
                if (!strcmp(current, p->choices[j]))
                    selected = (int)j;
            nk_layout_row_dynamic(ctx, 26, 2);
            nk_label(ctx, label, NK_TEXT_LEFT);
            int next = nk_combo(ctx, (const char *const *)p->choices, (int)p->nchoices, selected, 24,
                                nk_vec2(220, 240));
            if (next >= 0 && (size_t)next < p->nchoices && next != selected)
                put(dialog->fields, params, p->key, yyjson_mut_strcpy(dialog->fields, p->choices[next]));
        }
    }
    pc_ui_parameter_schema_fini(&schema);
}
void pc_ui_shell_gallery_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_doc *d = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(d),
                   *filters = yyjson_mut_obj_get(yyjson_mut_obj_get(root, "__catalogue"), "filters"),
                   *effects = yyjson_mut_obj_get(root, "effects");
    size_t count = yyjson_mut_arr_size(effects), nf = yyjson_mut_arr_size(filters),
           selected = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "__gallerySelected"));
    if (!count || count > 32 || !nf || nf > 128) {
        nk_layout_row_dynamic(ctx, 36, 1);
        nk_label_wrap(ctx, pc_i18n_t("The Filter Gallery stack is invalid."));
        return;
    }
    if (selected >= count)
        selected = count - 1;
    nk_layout_row_begin(ctx, NK_DYNAMIC, 490, 3);
    nk_layout_row_push(ctx, .4f);
    if (nk_group_begin(ctx, "gallery-preview", NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_label(ctx, pc_i18n_t("Canvas Preview"), NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 400, 1);
        struct nk_rect bounds;
        (void)nk_widget(&bounds, ctx);
        struct nk_image image;
        float aspect;
        if (pc_ui_shell_canvas_preview(app, &image, &aspect)) {
            float w = fminf(bounds.w, bounds.h * aspect), h = w / aspect;
            struct nk_rect rect = nk_rect(bounds.x + (bounds.w - w) / 2, bounds.y + (bounds.h - h) / 2, w, h);
            pc_ui_shell_widget_mapping(app, "gallery.preview", rect);
            nk_draw_image(nk_window_get_canvas(ctx), rect, &image, nk_rgba(255, 255, 255, 255));
        }
        nk_group_end(ctx);
    }
    nk_layout_row_push(ctx, .28f);
    if (nk_group_begin(ctx, "gallery-catalogue", NK_WINDOW_BORDER)) {
        const char *category = "";
        for (size_t i = 0; i < nf; i++) {
            yyjson_mut_val *row = yyjson_mut_arr_get(filters, i);
            const char *next = str(row, "category");
            if (strcmp(category, next)) {
                nk_layout_row_dynamic(ctx, 28, 1);
                nk_label(ctx, next, NK_TEXT_LEFT);
                category = next;
            }
            nk_layout_row_dynamic(ctx, 28, 1);
            char mapping[128];
            (void)snprintf(mapping, sizeof mapping, "gallery.choose.%s", str(row, "key"));
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            if (nk_button_label(ctx, str(row, "name"))) {
                yyjson_mut_val *replacement = effect(d, row);
                if (replacement)
                    (void)yyjson_mut_arr_replace(effects, selected, replacement);
            }
        }
        nk_group_end(ctx);
    }
    nk_layout_row_push(ctx, .32f);
    if (nk_group_begin(ctx, "gallery-properties", NK_WINDOW_BORDER)) {
        yyjson_mut_val *item = yyjson_mut_arr_get(effects, selected), *catalogue = NULL;
        for (size_t i = 0; i < nf; i++) {
            yyjson_mut_val *row = yyjson_mut_arr_get(filters, i);
            if (!strcmp(str(row, "key"), str(item, "filter"))) {
                catalogue = row;
                break;
            }
        }
        if (catalogue)
            controls(app, dialog, item, catalogue);
        nk_layout_row_dynamic(ctx, 28, 1);
        nk_label(ctx, pc_i18n_t("Effect Layers (bottom runs first)"), NK_TEXT_LEFT);
        for (size_t i = count; i-- > 0;) {
            yyjson_mut_val *row = yyjson_mut_arr_get(effects, i);
            nk_layout_row_begin(ctx, NK_DYNAMIC, 26, 2);
            nk_layout_row_push(ctx, .14f);
            nk_bool visible = !yyjson_mut_is_false(yyjson_mut_obj_get(row, "visible"));
            char mapping[128];
            (void)snprintf(mapping, sizeof mapping, "gallery.visible.%zu", i);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            if (nk_checkbox_label(ctx, pc_i18n_t(""), &visible))
                put(d, row, "visible", yyjson_mut_bool(d, visible != 0));
            nk_layout_row_push(ctx, .86f);
            (void)snprintf(mapping, sizeof mapping, "gallery.select.%zu", i);
            pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
            const char *name = str(row, "filter");
            for (size_t j = 0; j < nf; j++)
                if (!strcmp(name, str(yyjson_mut_arr_get(filters, j), "key"))) {
                    name = str(yyjson_mut_arr_get(filters, j), "name");
                    break;
                }
            if (nk_button_label(ctx, name))
                selected = i;
            nk_layout_row_end(ctx);
        }
        nk_layout_row_dynamic(ctx, 28, 4);
        pc_ui_shell_widget_mapping(app, "gallery.add", nk_widget_bounds(ctx));
        bool add = nk_button_label(ctx, pc_i18n_t("New"));
        pc_ui_shell_widget_mapping(app, "gallery.delete", nk_widget_bounds(ctx));
        bool remove = nk_button_label(ctx, pc_i18n_t("Delete"));
        pc_ui_shell_widget_mapping(app, "gallery.up", nk_widget_bounds(ctx));
        bool up = nk_button_label(ctx, pc_i18n_t("Up"));
        pc_ui_shell_widget_mapping(app, "gallery.down", nk_widget_bounds(ctx));
        bool down = nk_button_label(ctx, pc_i18n_t("Down"));
        if (add && count < 32) {
            yyjson_mut_val *copy = yyjson_mut_val_mut_copy(d, yyjson_mut_arr_get(effects, selected));
            if (copy && yyjson_mut_arr_add_val(effects, copy))
                selected = count++;
        } else if (remove && count > 1) {
            (void)yyjson_mut_arr_remove(effects, selected);
            if (selected >= --count)
                selected = count - 1;
        } else if ((up && selected + 1 < count) || (down && selected > 0)) {
            size_t other = up ? selected + 1 : selected - 1;
            yyjson_mut_val *a = yyjson_mut_val_mut_copy(d, yyjson_mut_arr_get(effects, selected)),
                           *b = yyjson_mut_val_mut_copy(d, yyjson_mut_arr_get(effects, other));
            if (a && b) {
                (void)yyjson_mut_arr_replace(effects, selected, b);
                (void)yyjson_mut_arr_replace(effects, other, a);
                selected = other;
            }
        }
        put(d, root, "__gallerySelected", yyjson_mut_uint(d, selected));
        nk_group_end(ctx);
    }
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_bool preview = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "preview"));
    if (nk_checkbox_label(ctx, pc_i18n_t("Preview"), &preview))
        put(d, root, "preview", yyjson_mut_bool(d, preview != 0));
    if (pc_ui_filter_preview_busy(&runtime->preview))
        nk_label(ctx, pc_i18n_t("Updating preview..."), NK_TEXT_LEFT);
    else if (runtime->preview.has_ready && runtime->preview.status != PC_OK)
        nk_label_wrap(ctx, runtime->preview.error.msg);
}
