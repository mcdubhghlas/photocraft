#include "ui/i18n.h"
#include "filter_ui.h"
#include "gallery_ui.h"
#include "camera_raw_ui.h"
#include "color_range_ui.h"
#include "adjust_ui.h"
#include "adjust_controls.h"
#include "shell_internal.h"
#include "ui/filter_dialog.h"
#include "ui/color_picker.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
pc_status pc_ui_shell_filter_open(pc_ui_shell_app *app, const char *command, pc_json_value *out,
                                  pc_error *err)
{
    yyjson_mut_doc *fields = NULL;
    pc_status status = pc_ui_filter_dialog_fields(pc_ui_shell_app_session(app), command, &fields, err);
    if (status == PC_OK && !strcmp(command, "filter.filterGallery"))
        status = pc_ui_shell_gallery_init(app, fields, err);
    if (status == PC_OK && !strcmp(command, "select.colorRange"))
        status = pc_ui_shell_color_range_init(app, fields, err);
    if (status != PC_OK) {
        yyjson_mut_doc_free(fields);
        return status;
    }
    return pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, fields, yyjson_mut_doc_get_root(fields), out,
                                    err);
}
void pc_ui_shell_parameter_label(const char *key, char out[128])
{
    static const char *const ids[] = {"png", "jpg", "webp", "tif", "tga", "ico"};
    static const char *const names[] = {"PNG", "JPG", "WebP", "TIFF", "TGA", "ICO"};
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++)
        if (!strcmp(key, ids[i])) {
            (void)snprintf(out, 128, "%s", names[i]);
            return;
        }
    size_t n = 0;
    for (size_t i = 0; key[i] && n + 2 < 128; i++) {
        unsigned char c = (unsigned char)key[i];
        if (i && isupper(c))
            out[n++] = ' ';
        out[n++] = i ? (char)c : (char)toupper(c);
    }
    out[n] = 0;
}
static void put(yyjson_mut_doc *doc, yyjson_mut_val *o, const char *key, yyjson_mut_val *value)
{
    if (value) {
        yyjson_mut_val *k = yyjson_mut_strcpy(doc, key);
        if (k)
            (void)yyjson_mut_obj_put(o, k, value);
    }
}
static void number(struct nk_context *ctx, pc_ui_shell_app *app, pc_ui_dialog *dialog, const char *key,
                   const char *label, double min, double max, double def, bool integer)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields), *v = yyjson_mut_obj_get(root, key);
    double value = yyjson_mut_is_num(v) ? yyjson_mut_get_num(v) : def;
    if (!isfinite(value))
        value = def;
    char mapping[192];
    (void)snprintf(mapping, sizeof mapping, "dialog.%s", key);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    double next = value;
    nk_property_double(ctx, pc_i18n_t(label), min, &next, max, (max - min) > 10 ? 1 : .01f, .1f);
    if (next != value)
        put(dialog->fields, root, key,
            integer ? yyjson_mut_sint(dialog->fields, (int64_t)round(fmin(max, fmax(min, next))))
                    : yyjson_mut_real(dialog->fields, next));
}
static void text(struct nk_context *ctx, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog,
                 const pc_ui_param *param)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields), *v = yyjson_mut_obj_get(root, param->key);
    const char *value = yyjson_mut_get_str(v);
    if (!value)
        value = "";
    pc_ui_dialog_text *buffer = pc_ui_dialog_text_buffer(r, dialog->id, param->key, value);
    if (!buffer)
        return;
    char label[128];
    pc_ui_shell_parameter_label(param->key, label);
    nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
    nk_flags flags = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)buffer->text.data,
                                                    (int)buffer->text.cap, nk_filter_default);
    buffer->active = (flags & NK_EDIT_ACTIVE) != 0;
    buffer->text.len = strlen(pc_buf_cstr(&buffer->text));
    if (strcmp(value, pc_buf_cstr(&buffer->text)))
        put(dialog->fields, root, param->key, yyjson_mut_strcpy(dialog->fields, pc_buf_cstr(&buffer->text)));
}
static void color_field(pc_ui_shell_app *app, pc_ui_dialog *dialog, const pc_ui_param *p)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields),
                   *value = yyjson_mut_obj_get(fields, p->key);
    const char *hex = yyjson_mut_get_str(value);
    float rgb[3] = {0};
    if (hex)
        (void)pc_ui_picker_parse_hex(hex, strlen(hex), rgb);
    else if (yyjson_mut_is_arr(value))
        for (size_t i = 0; i < 3; i++)
            rgb[i] = (float)yyjson_mut_get_num(yyjson_mut_arr_get(value, i));
    else if (!strcmp(p->key, "tintColor")) {
        rgb[0] = .882f;
        rgb[1] = .827f;
        rgb[2] = .702f;
    }
    nk_layout_row_dynamic(ctx, 26, 2);
    char label[128], mapping[160];
    pc_ui_shell_parameter_label(p->key, label);
    nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
    (void)snprintf(mapping, sizeof mapping, "dialog.%s.color", p->key);
    pc_ui_shell_widget_mapping(app, mapping, nk_widget_bounds(ctx));
    struct nk_color color =
        nk_rgba_f(fminf(1, fmaxf(0, rgb[0])), fminf(1, fmaxf(0, rgb[1])), fminf(1, fmaxf(0, rgb[2])), 1);
    if (nk_button_color(ctx, color)) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *params = doc ? yyjson_mut_obj(doc) : NULL;
        if (params && yyjson_mut_obj_add_uint(doc, params, "dialog", dialog->id) &&
            yyjson_mut_obj_add_strcpy(doc, params, "field", p->key))
            pc_ui_shell_queue(app, "ui.dialog.colorField", params);
        yyjson_mut_doc_free(doc);
    }
}
static void grid(struct nk_context *ctx, pc_ui_dialog *dialog, const pc_ui_param *p)
{
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields), *array = yyjson_mut_obj_get(root, p->key);
    char label[128];
    pc_ui_shell_parameter_label(p->key, label);
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
    size_t side = (size_t)sqrt((double)p->grid_size);
    if (side * side != p->grid_size || side > 16)
        return;
    if (!yyjson_mut_is_arr(array)) {
        array = yyjson_mut_arr(dialog->fields);
        if (!array)
            return;
        for (size_t i = 0; i < p->grid_size; i++)
            if (!yyjson_mut_arr_add_int(dialog->fields, array, i == p->grid_size / 2 ? 1 : 0))
                return;
        put(dialog->fields, root, p->key, array);
    }
    nk_layout_row_dynamic(ctx, 24, (int)side);
    for (size_t i = 0; i < p->grid_size; i++) {
        yyjson_mut_val *v = yyjson_mut_arr_get(array, i);
        double value = yyjson_mut_get_num(v), next = value;
        nk_property_double(ctx, "#", -9999, &next, 9999, 1, 1);
        if (next != value)
            (void)yyjson_mut_arr_replace(array, i, yyjson_mut_real(dialog->fields, next));
    }
}
void pc_ui_shell_filter_fields(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog,
                               const char *command)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    bool curves = command && !strcmp(command, "image.adjustments.curves");
    bool levels = command && !strcmp(command, "image.adjustments.levels");
    for (size_t i = 0; i < r->schema.count; i++) {
        const pc_ui_param *p = &r->schema.items[i];
        if (yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__export")) &&
            (!strcmp(p->key, "estimate") || !strcmp(p->key, "path")))
            continue;
        if (pc_ui_shell_camera_raw_hidden(dialog, command, p->key) ||
            pc_ui_shell_adjust_owns(command, p->key))
            continue;
        if (!strcmp(p->key, "tgaBits")) {
            const char *format = yyjson_mut_get_str(yyjson_mut_obj_get(root, "format"));
            if (!format || strcmp(format, "tga"))
                continue;
        }
        if (p->kind == PC_UI_PARAM_STRUCTURED)
            continue;
        if (levels && (!strcmp(p->key, "inBlack") || !strcmp(p->key, "gamma") || !strcmp(p->key, "inWhite") ||
                       !strcmp(p->key, "outBlack") || !strcmp(p->key, "outWhite")))
            continue;
        if (curves && !strcmp(p->key, "pencil"))
            continue;
        char label[128];
        pc_ui_shell_parameter_label(p->key, label);
        yyjson_mut_val *v = yyjson_mut_obj_get(root, p->key);
        nk_layout_row_dynamic(ctx, 26, 1);
        if (p->kind == PC_UI_PARAM_RANGE)
            number(ctx, app, dialog, p->key, label, p->min, p->max, p->number, false);
        else if (p->kind == PC_UI_PARAM_INTEGER)
            number(ctx, app, dialog, p->key, label, p->min, p->max, p->number, true);
        else if (p->kind == PC_UI_PARAM_BOOL) {
            nk_bool value = v ? yyjson_mut_get_bool(v) : p->boolean, next = value;
            nk_checkbox_label(ctx, pc_i18n_t(label), &next);
            if (value != next)
                put(dialog->fields, root, p->key, yyjson_mut_bool(dialog->fields, next != 0));
        } else if (p->kind == PC_UI_PARAM_CHOICE) {
            const char *value = yyjson_mut_get_str(v);
            int selected = 0;
            for (size_t j = 0; j < p->nchoices; j++)
                if (value && !strcmp(value, p->choices[j]))
                    selected = (int)j;
            nk_layout_row_dynamic(ctx, 26, 2);
            nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
            const char *labels[64];
            char names[64][128];
            for (size_t j = 0; j < p->nchoices; j++) {
                pc_ui_shell_parameter_label(p->choices[j], names[j]);
                labels[j] = pc_i18n_t(names[j]);
            }
            int next = nk_combo(ctx, labels, (int)p->nchoices, selected, 24, nk_vec2(240, 280));
            if (next != selected && next >= 0 && (size_t)next < p->nchoices)
                put(dialog->fields, root, p->key, yyjson_mut_strcpy(dialog->fields, p->choices[next]));
        } else if (p->kind == PC_UI_PARAM_COLOR)
            color_field(app, dialog, p);
        else if (p->kind == PC_UI_PARAM_TEXT) {
            nk_layout_row_dynamic(ctx, 26, 2);
            text(ctx, r, dialog, p);
        } else if (p->kind == PC_UI_PARAM_GRID)
            grid(ctx, dialog, p);
        else if (p->kind == PC_UI_PARAM_DOCUMENT) {
            size_t n = 0;
            const pc_engine_doc_state *docs = pc_engine_session_documents(pc_ui_shell_app_session(app), &n);
            const char *names[128];
            if (n > 128)
                n = 128;
            for (size_t j = 0; j < n; j++)
                names[j] = pc_str_c(&pc_doc_arc_get(docs[j].doc)->name);
            nk_layout_row_dynamic(ctx, 26, 2);
            nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
            int selected = (int)yyjson_mut_get_uint(v);
            if (selected < 0 || (size_t)selected >= n)
                selected = 0;
            int next = nk_combo(ctx, names, (int)n, selected, 24, nk_vec2(240, 280));
            if (next >= 0 && next != selected)
                put(dialog->fields, root, p->key, yyjson_mut_uint(dialog->fields, (uint64_t)next));
        }
    }
}
void pc_ui_shell_filter_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *r, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    if (yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__gallery"))) {
        pc_ui_shell_gallery_body(app, r, dialog);
        return;
    }
    if (yyjson_mut_get_bool(yyjson_mut_obj_get(root, "__colorRange"))) {
        pc_ui_shell_color_range_body(app, r, dialog);
        return;
    }
    if (r->schema_dialog != dialog->id) {
        r->adjustment_range = 0;
        r->gradient_stop = 0;
        r->gradient_dragging = false;
        pc_ui_parameter_schema_fini(&r->schema);
        const char *spec = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__spec"));
        if (pc_ui_parameter_schema_parse(&r->schema, spec, NULL) == PC_OK)
            r->schema_dialog = dialog->id;
    }
    pc_ui_shell_adjust_graph(app, r, dialog);
    pc_ui_shell_adjust_controls(app, r, dialog);
    pc_ui_shell_adjust_gradient(app, r, dialog);
    const char *command = yyjson_mut_get_str(yyjson_mut_obj_get(root, "__command"));
    if (command && !strcmp(command, "filter.cameraRaw")) {
        pc_ui_shell_camera_raw_body(app, r, dialog);
        return;
    }
    pc_ui_shell_filter_fields(app, r, dialog, command);
    nk_layout_row_dynamic(ctx, 26, 2);
    nk_bool preview = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "preview"));
    if (nk_checkbox_label(ctx, pc_i18n_t("Preview"), &preview))
        put(dialog->fields, root, "preview", yyjson_mut_bool(dialog->fields, preview != 0));
    if (nk_button_label(ctx, pc_i18n_t("Reset"))) {
        yyjson_mut_doc *defaults = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *o = defaults ? yyjson_mut_obj(defaults) : NULL;
        if (o && pc_ui_parameter_schema_defaults(&r->schema, defaults, o, NULL) == PC_OK) {
            for (size_t i = 0; i < r->schema.count; i++) {
                const char *key = r->schema.items[i].key;
                (void)yyjson_mut_obj_remove_str(root, key);
                yyjson_mut_val *v = yyjson_mut_obj_get(o, key);
                if (v)
                    put(dialog->fields, root, key, yyjson_mut_val_mut_copy(dialog->fields, v));
            }
        }
        yyjson_mut_doc_free(defaults);
    }
    nk_layout_row_dynamic(ctx, 24, 1);
    if (pc_ui_filter_preview_busy(&r->preview))
        nk_label(ctx, pc_i18n_t("Updating preview..."), NK_TEXT_LEFT);
    else if (r->preview.has_ready && r->preview.status != PC_OK)
        nk_label_wrap(ctx, r->preview.error.msg);
}
