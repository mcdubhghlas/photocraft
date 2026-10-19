/* Preferences edits an owned working copy, using the engine's enum/range/visibility metadata. */
#include "preferences.h"
#include "shell_internal.h"
#include "engine/prefs.h"
#include "ui/i18n.h"
#include "base/num.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static const struct {
    const char *id, *label;
} sections[] = {{"general", "General"},
                {"interface", "Interface"},
                {"workspace", "Workspace"},
                {"tools", "Tools"},
                {"historyLog", "History Log"},
                {"fileHandling", "File Handling"},
                {"export", "Export"},
                {"performance", "Performance"},
                {"scratchDisks", "Scratch Disks"},
                {"cursors", "Cursors"},
                {"transparencyAndGamut", "Transparency & Gamut"},
                {"unitsAndRulers", "Units & Rulers"},
                {"guidesGridAndSlices", "Guides, Grid & Slices"},
                {"plugIns", "Plug-ins"},
                {"type", "Type"},
                {"enhancedControls", "Enhanced Controls"},
                {"rawDefaults", "Camera Raw Defaults"},
                {"integrations", "Integrations"}};
static bool put(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(doc, key);
    return k && value && yyjson_mut_obj_put(root, k, value);
}
static yyjson_mut_val *editable_values(yyjson_mut_doc *doc, yyjson_mut_val *source)
{
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    if (!root) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof sections / sizeof *sections; i++) {
        yyjson_mut_val *value = yyjson_mut_obj_get(source, sections[i].id);
        if (value &&
            !yyjson_mut_obj_add_val(doc, root, sections[i].id, yyjson_mut_val_mut_copy(doc, value))) {
            return NULL;
        }
    }
    return root;
}
static pc_status execute(pc_ui_shell_app *app, const char *method, yyjson_mut_val *params, bool control,
                         pc_json_value *out, pc_error *err)
{
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    pc_status status = pc_json_write(params, 0, &text, err);
    if (status == PC_OK) {
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &read, err);
    }
    if (status == PC_OK) {
        status = control ? pc_ui_shell_dialog_control(app, method, yyjson_doc_get_root(read), out, err)
                         : pc_engine_session_execute(pc_ui_shell_app_session(app), method,
                                                     yyjson_doc_get_root(read), out, err);
    }
    yyjson_doc_free(read);
    pc_buf_fini(&text);
    return status;
}
bool pc_ui_shell_is_preferences(yyjson_mut_val *fields)
{
    const char *kind = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__prefsui"));
    return kind && !strcmp(kind, "prefs");
}
pc_status pc_ui_shell_preferences_open(pc_ui_shell_app *app, const char *section, pc_json_value *out,
                                       pc_error *err)
{
    if (!section) {
        section = "general";
    }
    bool valid = false;
    for (size_t i = 0; i < sizeof sections / sizeof *sections; i++) {
        if (!strcmp(section, sections[i].id)) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        return pc_fail(err, PC_ERR_INVALID, "unknown Preferences section");
    }
    pc_json_value prefs = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_prefs_get(pc_ui_shell_app_session(app), "", &prefs, err);
    yyjson_mut_doc *doc = status == PC_OK ? yyjson_mut_doc_new(NULL) : NULL;
    if (status == PC_OK && !doc) {
        status = pc_fail(err, PC_ERR_NOMEM, "Preferences draft");
    }
    if (status == PC_OK) {
        yyjson_mut_val *fields = yyjson_mut_obj(doc),
                       *values = editable_values(doc, pc_json_value_root(&prefs));
        bool ok = fields && values && yyjson_mut_obj_add_str(doc, fields, "__command", "prefs.set") &&
                  yyjson_mut_obj_add_str(doc, fields, "__prefsui", "prefs") &&
                  yyjson_mut_obj_add_str(doc, fields, "__label", "Preferences") &&
                  yyjson_mut_obj_add_strcpy(doc, fields, "__section", section) &&
                  yyjson_mut_obj_add_val(doc, fields, "values", values);
        if (ok) {
            status = pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, doc, fields, out, err);
            doc = NULL;
        } else {
            status = pc_fail(err, PC_ERR_NOMEM, "Preferences draft");
        }
    }
    yyjson_mut_doc_free(doc);
    pc_json_value_fini(&prefs);
    return status;
}
pc_status pc_ui_shell_preferences_commit(pc_ui_shell_app *app, uint64_t id, bool close, pc_json_value *out,
                                         pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    yyjson_mut_val *fields = dialog ? yyjson_mut_doc_get_root(dialog->fields) : NULL;
    if (!dialog || dialog->kind != PC_DIALOG_COMMAND || !pc_ui_shell_is_preferences(fields)) {
        return pc_fail(err, PC_ERR_INVALID, "Apply is only available for Preferences");
    }
    yyjson_mut_val *values = yyjson_mut_obj_get(fields, "values");
    if (!yyjson_mut_is_obj(values)) {
        return pc_fail(err, PC_ERR_INVALID, "Preferences values must be an object");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "Preferences parameters");
    }
    yyjson_mut_val *params = yyjson_mut_obj(doc), *copy = yyjson_mut_val_mut_copy(doc, values);
    bool ok = params && copy && yyjson_mut_obj_add_str(doc, params, "path", "") &&
              yyjson_mut_obj_add_val(doc, params, "value", copy);
    pc_status status = ok ? execute(app, "prefs.set", params, false, out, err)
                          : pc_fail(err, PC_ERR_NOMEM, "Preferences parameters");
    if (status == PC_OK) {
        if (close) {
            pc_ui_state_close_dialog(ui, id);
        } else {
            pc_json_value fresh = PC_JSON_VALUE_INIT;
            if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "", &fresh, NULL) == PC_OK) {
                yyjson_mut_val *current = editable_values(dialog->fields, pc_json_value_root(&fresh));
                if (current) {
                    (void)put(dialog->fields, fields, "values", current);
                }
            }
            pc_json_value_fini(&fresh);
        }
        pc_ui_shell_apply_preferences(app);
    }
    yyjson_mut_doc_free(doc);
    return status;
}
static void humanize(const char *key, char *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; key && key[i] && n + 1 < cap; i++) {
        unsigned char c = (unsigned char)key[i];
        if (i && c >= 'A' && c <= 'Z' && n + 2 < cap) {
            out[n++] = ' ';
        }
        out[n++] = i == 0 && c >= 'a' && c <= 'z' ? (char)(c - ('a' - 'A')) : c == '_' ? ' ' : (char)c;
    }
    out[n] = 0;
}
static void replace(yyjson_mut_doc *doc, yyjson_mut_val *parent, const char *key, size_t index,
                    yyjson_mut_val *value)
{
    if (!value) {
        return;
    }
    if (key) {
        (void)put(doc, parent, key, value);
    } else {
        (void)yyjson_mut_arr_replace(parent, index, value);
    }
}
static bool show_field(const char *path, yyjson_mut_val *parent, const char *key)
{
    if (pc_pref_is_hidden(path)) {
        return false;
    }
    if (!key || strncmp(path, "export.", 7)) {
        return true;
    }
    const char *format = yyjson_mut_get_str(yyjson_mut_obj_get(parent, "quickExportFormat"));
    if (!strcmp(key, "jpegQuality")) {
        return format && !strcmp(format, "jpg");
    }
    if (!strcmp(key, "webpLossless")) {
        return format && !strcmp(format, "webp");
    }
    if (!strcmp(key, "webpQuality")) {
        return format && !strcmp(format, "webp") &&
               !yyjson_mut_get_bool(yyjson_mut_obj_get(parent, "webpLossless"));
    }
    return true;
}
static void field(struct nk_context *ctx, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog,
                  yyjson_mut_val *parent, const char *key, size_t index, const char *path, unsigned depth,
                  size_t *budget)
{
    if (depth > 5 || !*budget || !show_field(path, parent, key)) {
        return;
    }
    (*budget)--;
    yyjson_mut_doc *doc = dialog->fields;
    yyjson_mut_val *value = key ? yyjson_mut_obj_get(parent, key) : yyjson_mut_arr_get(parent, index);
    char label[192];
    humanize(key ? key : "Value", label, sizeof label);
    if (yyjson_mut_is_obj(value)) {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
        const char *keys[256];
        size_t count = 0, i, n;
        yyjson_mut_val *child, *v;
        yyjson_mut_obj_foreach(value, i, n, child, v)
        {
            if (!v || !child) {
                break;
            }
            if (count < 256) {
                keys[count++] = yyjson_mut_get_str(child);
            }
        }
        for (size_t j = 0; j < count; j++) {
            char nested[256];
            int len = snprintf(nested, sizeof nested, "%s.%s", path, keys[j]);
            if (len > 0 && (size_t)len < sizeof nested) {
                field(ctx, runtime, dialog, value, keys[j], 0, nested, depth + 1, budget);
            }
        }
        return;
    }
    if (yyjson_mut_is_arr(value)) {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
        size_t count = yyjson_mut_arr_size(value);
        for (size_t i = 0; i < count && i < 64; i++) {
            char nested[256];
            int len = snprintf(nested, sizeof nested, "%s[%zu]", path, i);
            if (len > 0 && (size_t)len < sizeof nested) {
                field(ctx, runtime, dialog, value, NULL, i, nested, depth + 1, budget);
            }
            nk_layout_row_dynamic(ctx, 22, 1);
            if (nk_button_label(ctx, pc_i18n_t("Remove"))) {
                (void)yyjson_mut_arr_remove(value, i);
                break;
            }
        }
        nk_layout_row_dynamic(ctx, 24, 1);
        if (count < 64 && nk_button_label(ctx, pc_i18n_t("Add"))) {
            yyjson_mut_val *item = count ? yyjson_mut_val_mut_copy(doc, yyjson_mut_arr_get_first(value))
                                         : yyjson_mut_str(doc, "");
            if (item) {
                (void)yyjson_mut_arr_append(value, item);
            }
        }
        return;
    }
    nk_layout_row_dynamic(ctx, 26, yyjson_mut_is_bool(value) ? 1 : 2);
    if (yyjson_mut_is_bool(value)) {
        nk_bool enabled = yyjson_mut_get_bool(value);
        if (nk_checkbox_label(ctx, pc_i18n_t(label), &enabled)) {
            replace(doc, parent, key, index, yyjson_mut_bool(doc, enabled != 0));
        }
        return;
    }
    nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
    if (yyjson_mut_is_num(value)) {
        double number = yyjson_mut_get_num(value), before = number, low = -1e9, high = 1e9;
        (void)pc_pref_range(path, &low, &high);
        bool integer = yyjson_mut_is_int(value);
        nk_property_double(ctx, "#", low, &number, high, integer ? 1 : .01, .1f);
        if (number != before && isfinite(number)) {
            yyjson_mut_val *next =
                integer ? (yyjson_mut_is_uint(value) ? yyjson_mut_uint(doc, pc_f64_to_u64(round(number)))
                                                     : yyjson_mut_sint(doc, pc_f64_to_i64(round(number))))
                        : yyjson_mut_real(doc, number);
            replace(doc, parent, key, index, next);
        }
        return;
    }
    const char *text = yyjson_mut_get_str(value);
    if (!text) {
        text = "";
    }
    if (!strcmp(path, "performance.renderingMode") && !yyjson_mut_is_str(value)) {
        const char *backend = yyjson_mut_get_str(yyjson_mut_obj_get(parent, "gpuBackend"));
        yyjson_mut_val *gpu = yyjson_mut_obj_get(parent, "useGpu");
        text = (yyjson_mut_is_bool(gpu) && !yyjson_mut_get_bool(gpu)) || (backend && !strcmp(backend, "cpu"))
                   ? "cpu"
                   : "auto";
    }
    if (!strcmp(path, "interface.language")) {
        const pc_i18n_lang *language = pc_i18n_from_code(text);
        const char *title = language                ? pc_i18n_lang_name(language)
                            : !strcmp(text, "auto") ? "Automatic"
                                                    : text;
        if (nk_combo_begin_label(ctx, pc_i18n_t(title), nk_vec2(280, 300))) {
            nk_layout_row_dynamic(ctx, 24, 1);
            if (nk_combo_item_label(ctx, pc_i18n_t("Automatic"), NK_TEXT_LEFT)) {
                replace(doc, parent, key, index, yyjson_mut_str(doc, "auto"));
            }
            for (size_t i = 0; i < pc_i18n_count(); i++) {
                const pc_i18n_lang *lang = pc_i18n_at(i);
                if (nk_combo_item_label(ctx, pc_i18n_lang_name(lang), NK_TEXT_LEFT)) {
                    replace(doc, parent, key, index, yyjson_mut_str(doc, pc_i18n_lang_code(lang)));
                }
            }
            nk_combo_end(ctx);
        }
        return;
    }
    size_t count = 0;
    const char *const *choices = pc_pref_choices(path, &count);
    if (choices && count) {
        char choice_label[128];
        humanize(text, choice_label, sizeof choice_label);
        if (nk_combo_begin_label(ctx, pc_i18n_t(choice_label), nk_vec2(240, 220))) {
            nk_layout_row_dynamic(ctx, 24, 1);
            for (size_t i = 0; i < count; i++) {
                humanize(choices[i], choice_label, sizeof choice_label);
                if (nk_combo_item_label(ctx, pc_i18n_t(choice_label), NK_TEXT_LEFT)) {
                    replace(doc, parent, key, index, yyjson_mut_strcpy(doc, choices[i]));
                }
            }
            nk_combo_end(ctx);
        }
    } else {
        pc_ui_dialog_text *buffer = pc_ui_dialog_text_buffer(runtime, dialog->id, path, text);
        if (!buffer) {
            nk_label(ctx, pc_i18n_t(""), NK_TEXT_LEFT);
            return;
        }
        nk_flags result = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)buffer->text.data,
                                                         (int)buffer->text.cap, nk_filter_default);
        buffer->active = (result & NK_EDIT_ACTIVE) != 0;
        buffer->text.len = strlen(pc_buf_cstr(&buffer->text));
        if (strcmp(text, pc_buf_cstr(&buffer->text))) {
            replace(doc, parent, key, index,
                    yyjson_mut_strncpy(doc, pc_buf_cstr(&buffer->text), buffer->text.len));
        }
    }
}
void pc_ui_shell_preferences_draw(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    const char *section = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__section"));
    size_t selected = 0;
    for (size_t i = 0; section && i < sizeof sections / sizeof *sections; i++) {
        if (!strcmp(section, sections[i].id)) {
            selected = i;
            break;
        }
    }
    nk_layout_row_dynamic(ctx, 28, 1);
    if (nk_combo_begin_label(ctx, pc_i18n_t(sections[selected].label), nk_vec2(300, 340))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        for (size_t i = 0; i < sizeof sections / sizeof *sections; i++) {
            if (nk_combo_item_label(ctx, pc_i18n_t(sections[i].label), NK_TEXT_LEFT)) {
                selected = i;
                (void)put(dialog->fields, fields, "__section",
                          yyjson_mut_str(dialog->fields, sections[i].id));
            }
        }
        nk_combo_end(ctx);
    }
    yyjson_mut_val *values = yyjson_mut_obj_get(fields, "values"),
                   *object = yyjson_mut_obj_get(values, sections[selected].id);
    size_t budget = 256, i, n, count = 0;
    yyjson_mut_val *key, *value;
    const char *keys[256];
    if (yyjson_mut_is_obj(object)) {
        yyjson_mut_obj_foreach(object, i, n, key, value)
        {
            if (!value || !key) {
                break;
            }
            if (count < 256) {
                keys[count++] = yyjson_mut_get_str(key);
            }
        }
        for (size_t j = 0; j < count; j++) {
            char path[256];
            int len = snprintf(path, sizeof path, "%s.%s", sections[selected].id, keys[j]);
            if (len > 0 && (size_t)len < sizeof path) {
                field(ctx, runtime, dialog, object, keys[j], 0, path, 0, &budget);
            }
        }
    }
    nk_layout_row_dynamic(ctx, 28, 1);
    if (nk_button_label(ctx, pc_i18n_t("Reset Section"))) {
        pc_json_value defaults = PC_JSON_VALUE_INIT;
        if (pc_engine_prefs_defaults(&defaults, NULL) == PC_OK) {
            yyjson_mut_val *source = yyjson_mut_obj_get(pc_json_value_root(&defaults), sections[selected].id);
            if (source) {
                (void)put(dialog->fields, values, sections[selected].id,
                          yyjson_mut_val_mut_copy(dialog->fields, source));
            }
        }
        pc_json_value_fini(&defaults);
    }
}
