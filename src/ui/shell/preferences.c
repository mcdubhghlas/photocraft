/* Preferences edits an owned working copy, using the engine's enum/range/visibility metadata. */
#include "preferences.h"
#include "shell_internal.h"
#include "engine/prefs.h"
#include "ui/i18n.h"
#include "ui/color_picker.h"
#include "ui/theme_cards.h"
#include "ui/widgets.h"
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
    if (pc_pref_is_color(path) && yyjson_mut_is_str(value)) {
        const char *hex = yyjson_mut_get_str(value);
        if (!hex) {
            hex = "#000000";
        }
        float rgb[3];
        struct nk_color col = nk_rgb(40, 40, 40);
        if (pc_ui_picker_parse_hex(hex, strlen(hex), rgb)) {
            col = nk_rgb((int)(rgb[0] * 255 + 0.5f), (int)(rgb[1] * 255 + 0.5f),
                         (int)(rgb[2] * 255 + 0.5f));
        }
        nk_layout_row_template_begin(ctx, 26);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_push_static(ctx, 28);
        nk_layout_row_template_push_static(ctx, 92);
        nk_layout_row_template_end(ctx);
        nk_label(ctx, pc_i18n_t(label), NK_TEXT_LEFT);
        (void)nk_button_color(ctx, col);
        pc_ui_dialog_text *buffer = pc_ui_dialog_text_buffer(runtime, dialog->id, path, hex);
        if (buffer) {
            nk_flags result = nk_edit_string_zero_terminated(
                ctx, NK_EDIT_FIELD, (char *)buffer->text.data, (int)buffer->text.cap, nk_filter_default);
            buffer->active = (result & NK_EDIT_ACTIVE) != 0;
            buffer->text.len = strlen(pc_buf_cstr(&buffer->text));
            if (strcmp(hex, pc_buf_cstr(&buffer->text))) {
                replace(doc, parent, key, index,
                        yyjson_mut_strncpy(doc, pc_buf_cstr(&buffer->text), buffer->text.len));
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
/* Case-insensitive substring: does `haystack` contain `needle` (ASCII fold)? */
static bool contains_ci(const char *haystack, const char *needle)
{
    if (!needle || !*needle) {
        return true;
    }
    if (!haystack) {
        return false;
    }
    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen && h[i]) {
            unsigned char a = (unsigned char)h[i], b = (unsigned char)needle[i];
            if (a >= 'A' && a <= 'Z') {
                a = (unsigned char)(a + ('a' - 'A'));
            }
            if (b >= 'A' && b <= 'Z') {
                b = (unsigned char)(b + ('a' - 'A'));
            }
            if (a != b) {
                break;
            }
            i++;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}
/* Does the preference `key` (its id or humanised label) match the search query? */
static bool field_matches(const char *key, const char *query)
{
    if (!query || !*query) {
        return true;
    }
    char label[192];
    humanize(key, label, sizeof label);
    return contains_ci(key, query) || contains_ci(label, query);
}
/* Does a section (its label or any visible field) match the query? */
static bool section_matches(yyjson_mut_val *values, const char *id, const char *label, const char *query)
{
    if (!query || !*query || contains_ci(label, query)) {
        return true;
    }
    yyjson_mut_val *object = yyjson_mut_obj_get(values, id);
    if (!yyjson_mut_is_obj(object)) {
        return false;
    }
    size_t i, n;
    yyjson_mut_val *key, *value;
    yyjson_mut_obj_foreach(object, i, n, key, value)
    {
        const char *k = yyjson_mut_get_str(key);
        char path[256];
        int len = k ? snprintf(path, sizeof path, "%s.%s", id, k) : -1;
        if (len > 0 && (size_t)len < sizeof path && show_field(path, object, k) && field_matches(k, query)) {
            return true;
        }
    }
    return false;
}
/* Whether a section has any field the grid would actually draw (for the faint styling). */
static bool section_has_visible(yyjson_mut_val *values, const char *id)
{
    yyjson_mut_val *object = yyjson_mut_obj_get(values, id);
    if (!yyjson_mut_is_obj(object)) {
        return false;
    }
    size_t i, n;
    yyjson_mut_val *key, *value;
    yyjson_mut_obj_foreach(object, i, n, key, value)
    {
        const char *k = yyjson_mut_get_str(key);
        char path[256];
        int len = k ? snprintf(path, sizeof path, "%s.%s", id, k) : -1;
        if (len > 0 && (size_t)len < sizeof path && show_field(path, object, k)) {
            return true;
        }
    }
    return false;
}
bool pc_ui_shell_preferences_changed(pc_ui_shell_app *app, yyjson_mut_val *fields)
{
    yyjson_mut_val *draft = yyjson_mut_obj_get(fields, "values");
    if (!yyjson_mut_is_obj(draft)) {
        return false;
    }
    pc_json_value live = PC_JSON_VALUE_INIT;
    bool changed = true;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "", &live, NULL) == PC_OK) {
        yyjson_mut_doc *tmp = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *current = tmp ? editable_values(tmp, pc_json_value_root(&live)) : NULL;
        pc_buf a = PC_BUF_INIT, b = PC_BUF_INIT;
        if (current && pc_json_write(draft, PC_JSON_SORT_KEYS, &a, NULL) == PC_OK &&
            pc_json_write(current, PC_JSON_SORT_KEYS, &b, NULL) == PC_OK) {
            changed = a.len != b.len || memcmp(a.data, b.data, a.len) != 0;
        }
        pc_buf_fini(&a);
        pc_buf_fini(&b);
        yyjson_mut_doc_free(tmp);
    }
    pc_json_value_fini(&live);
    return changed;
}
/* Does the favourite-themes array contain `id`? */
static bool favorite_has(yyjson_mut_val *favorites, const char *id)
{
    if (!yyjson_mut_is_arr(favorites) || !id) {
        return false;
    }
    size_t i, n;
    yyjson_mut_val *item;
    yyjson_mut_arr_foreach(favorites, i, n, item)
    {
        const char *v = yyjson_mut_get_str(item);
        if (v && !strcmp(v, id)) {
            return true;
        }
    }
    return false;
}
/* Add `id` to the favourites (bounded to PC_UI_THEME_COUNT) or remove every copy of it. */
static void favorite_toggle(yyjson_mut_doc *doc, yyjson_mut_val *favorites, const char *id, bool add)
{
    if (!yyjson_mut_is_arr(favorites) || !id) {
        return;
    }
    if (add) {
        if (!favorite_has(favorites, id) && yyjson_mut_arr_size(favorites) < (size_t)PC_UI_THEME_COUNT) {
            yyjson_mut_val *v = yyjson_mut_strcpy(doc, id);
            if (v) {
                (void)yyjson_mut_arr_append(favorites, v);
            }
        }
        return;
    }
    for (size_t i = yyjson_mut_arr_size(favorites); i-- > 0;) {
        const char *v = yyjson_mut_get_str(yyjson_mut_arr_get(favorites, i));
        if (v && !strcmp(v, id)) {
            (void)yyjson_mut_arr_remove(favorites, i);
        }
    }
}
/* A small, non-interactive strip of the theme's key colours, so the card previews the look. */
static void theme_preview(struct nk_context *ctx, pc_ui_theme_kind kind)
{
    pc_ui_tokens tk = pc_ui_tokens_for_kind(kind);
    pc_ui_color colors[] = {tk.chrome, tk.card, tk.field, tk.text, tk.accent};
    nk_layout_row_static(ctx, 30, 30, (int)(sizeof colors / sizeof *colors));
    for (size_t i = 0; i < sizeof colors / sizeof *colors; i++) {
        (void)nk_button_color(ctx, pc_ui_nk_color(colors[i]));
    }
}
/* One theme card: a radio list of the slot's themes with a favourite star per row, over a preview
 * of the selected theme. Edits `slot_key` and `favoriteThemes` in the interface draft. */
static void theme_card(struct nk_context *ctx, yyjson_mut_doc *doc, yyjson_mut_val *iface,
                       const char *title, bool active, const char *slot_key, bool dark)
{
    const char *selected = yyjson_mut_get_str(yyjson_mut_obj_get(iface, slot_key));
    const pc_ui_theme_option *options = NULL;
    size_t count = pc_ui_theme_options(dark, &options);
    if (!selected || !count) {
        selected = count ? options[0].id : "";
    }
    yyjson_mut_val *favorites = yyjson_mut_obj_get(iface, "favoriteThemes");
    if (!yyjson_mut_is_arr(favorites)) {
        favorites = yyjson_mut_arr(doc);
        if (favorites) {
            (void)put(doc, iface, "favoriteThemes", favorites);
        }
    }
    nk_layout_row_dynamic(ctx, 22, 1);
    char heading[128];
    (void)snprintf(heading, sizeof heading, "%s%s", pc_i18n_t(title), active ? "  •" : "");
    nk_label(ctx, heading, NK_TEXT_LEFT);
    pc_ui_theme_kind preview = count ? options[0].kind : PC_UI_THEME_DEFAULT;
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(selected, options[i].id)) {
            preview = options[i].kind;
        }
    }
    theme_preview(ctx, preview);
    for (size_t i = 0; i < count; i++) {
        nk_layout_row_template_begin(ctx, 26);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_push_static(ctx, 28);
        nk_layout_row_template_end(ctx);
        nk_bool chosen = !strcmp(selected, options[i].id);
        if (nk_option_label(ctx, pc_i18n_t(pc_ui_theme_kind_label(options[i].kind)), chosen) && !chosen) {
            (void)put(doc, iface, slot_key, yyjson_mut_str(doc, options[i].id));
        }
        bool fav = favorite_has(favorites, options[i].id);
        if (nk_button_label(ctx, fav ? "\xe2\x98\x85" : "\xe2\x98\x86")) {
            favorite_toggle(doc, favorites, options[i].id, !fav);
        }
    }
}
/* Preferences › Interface appearance: the mode selector and the two theme cards (B7). Replaces the
 * generic combos for appearanceMode/darkTheme/lightTheme/favoriteThemes. */
static void appearance_rows(struct nk_context *ctx, yyjson_mut_doc *doc, yyjson_mut_val *iface)
{
    if (!yyjson_mut_is_obj(iface)) {
        return;
    }
    nk_layout_row_dynamic(ctx, 24, 1);
    nk_label(ctx, pc_i18n_t("Appearance"), NK_TEXT_LEFT);
    const char *mode = yyjson_mut_get_str(yyjson_mut_obj_get(iface, "appearanceMode"));
    if (!mode) {
        mode = "auto";
    }
    static const struct {
        const char *id, *label;
    } modes[] = {{"auto", "Sync with system"}, {"dark", "Dark"}, {"light", "Light"}};
    const char *shown = mode;
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        if (!strcmp(mode, modes[i].id)) {
            shown = modes[i].label;
        }
    }
    nk_layout_row_template_begin(ctx, 26);
    nk_layout_row_template_push_static(ctx, 160);
    nk_layout_row_template_push_static(ctx, 210);
    nk_layout_row_template_end(ctx);
    nk_label(ctx, pc_i18n_t("Appearance Mode"), NK_TEXT_LEFT);
    if (nk_combo_begin_label(ctx, pc_i18n_t(shown), nk_vec2(210, 120))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
            if (nk_combo_item_label(ctx, pc_i18n_t(modes[i].label), NK_TEXT_LEFT)) {
                (void)put(doc, iface, "appearanceMode", yyjson_mut_str(doc, modes[i].id));
                mode = modes[i].id;
            }
        }
        nk_combo_end(ctx);
    }
    bool system_light = (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) &&
                        SDL_GetSystemTheme() == SDL_SYSTEM_THEME_LIGHT;
    bool light_active = pc_ui_theme_light_active(mode, system_light);
    nk_layout_row_template_begin(ctx, 300);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    if (nk_group_begin(ctx, "theme-light", NK_WINDOW_BORDER)) {
        theme_card(ctx, doc, iface, "Light Theme", light_active, "lightTheme", false);
        nk_group_end(ctx);
    }
    if (nk_group_begin(ctx, "theme-dark", NK_WINDOW_BORDER)) {
        theme_card(ctx, doc, iface, "Dark Theme", !light_active, "darkTheme", true);
        nk_group_end(ctx);
    }
}
/* Interface theme keys the theme cards own, hidden from the generic field grid. */
static bool is_appearance_key(const char *section, const char *key)
{
    return section && key && !strcmp(section, "interface") &&
           (!strcmp(key, "theme") || !strcmp(key, "appearanceMode") || !strcmp(key, "darkTheme") ||
            !strcmp(key, "lightTheme") || !strcmp(key, "favoriteThemes"));
}
void pc_ui_shell_preferences_draw(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_val *fields = yyjson_mut_doc_get_root(dialog->fields);
    yyjson_mut_val *values = yyjson_mut_obj_get(fields, "values");
    /* Search field (B7): filters both the section list and the fields shown. */
    const char *stored = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__search"));
    if (!stored) {
        stored = "";
    }
    pc_ui_dialog_text *sb = pc_ui_dialog_text_buffer(runtime, dialog->id, "__search", stored);
    nk_layout_row_dynamic(ctx, 26, 1);
    const char *query = stored;
    if (sb) {
        nk_flags edited = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)sb->text.data,
                                                         (int)sb->text.cap, nk_filter_default);
        sb->active = (edited & NK_EDIT_ACTIVE) != 0;
        sb->text.len = strlen(pc_buf_cstr(&sb->text));
        if (strcmp(stored, pc_buf_cstr(&sb->text))) {
            (void)put(dialog->fields, fields, "__search",
                      yyjson_mut_strncpy(dialog->fields, pc_buf_cstr(&sb->text), sb->text.len));
        }
        query = pc_buf_cstr(&sb->text);
    } else {
        nk_label(ctx, pc_i18n_t("Search settings…"), NK_TEXT_LEFT);
    }
    /* Which sections survive the search, and which one is selected. */
    size_t matching[sizeof sections / sizeof *sections];
    size_t nmatch = 0;
    for (size_t i = 0; i < sizeof sections / sizeof *sections; i++) {
        if (section_matches(values, sections[i].id, sections[i].label, query)) {
            matching[nmatch++] = i;
        }
    }
    const char *section = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__section"));
    size_t selected = nmatch ? matching[0] : 0;
    bool selected_ok = false;
    for (size_t i = 0; section && i < nmatch; i++) {
        if (!strcmp(section, sections[matching[i]].id)) {
            selected = matching[i];
            selected_ok = true;
            break;
        }
    }
    if (!selected_ok && nmatch) {
        (void)put(dialog->fields, fields, "__section",
                  yyjson_mut_str(dialog->fields, sections[selected].id));
    }
    float height = fmaxf(220, nk_window_get_content_region(ctx).h - 70);
    nk_layout_row_template_begin(ctx, height);
    nk_layout_row_template_push_static(ctx, 170);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    if (nk_group_begin(ctx, "prefs-sections", NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, 24, 1);
        if (!nmatch) {
            nk_label(ctx, pc_i18n_t("No matching settings"), NK_TEXT_LEFT);
        }
        for (size_t i = 0; i < nmatch; i++) {
            size_t idx = matching[i];
            bool empty = !section_has_visible(values, sections[idx].id);
            if (empty) {
                nk_widget_disable_begin(ctx);
            }
            if (nk_select_label(ctx, pc_i18n_t(sections[idx].label), NK_TEXT_LEFT, idx == selected)) {
                (void)put(dialog->fields, fields, "__section",
                          yyjson_mut_str(dialog->fields, sections[idx].id));
            }
            if (empty) {
                nk_widget_disable_end(ctx);
            }
        }
        nk_group_end(ctx);
    }
    if (nk_group_begin(ctx, "prefs-fields", 0)) {
        yyjson_mut_val *object = yyjson_mut_obj_get(values, sections[selected].id);
        /* Within a section whose label already matches (or with no query), show every field; else
         * only the fields matching the query. */
        bool show_all = !query || !*query || contains_ci(sections[selected].label, query);
        /* Interface appearance: the theme cards replace the generic combos (B7). */
        if (!strcmp(sections[selected].id, "interface") &&
            (show_all || field_matches("appearance mode", query) || field_matches("theme", query))) {
            appearance_rows(ctx, dialog->fields, object);
        }
        size_t budget = 256, i, n, count = 0;
        yyjson_mut_val *key, *value;
        const char *keys[256];
        if (yyjson_mut_is_obj(object)) {
            yyjson_mut_obj_foreach(object, i, n, key, value)
            {
                if (!value || !key) {
                    break;
                }
                const char *k = yyjson_mut_get_str(key);
                if (is_appearance_key(sections[selected].id, k)) {
                    continue; /* owned by the theme cards above */
                }
                if (count < 256 && (show_all || field_matches(k, query))) {
                    keys[count++] = k;
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
                yyjson_mut_val *source =
                    yyjson_mut_obj_get(pc_json_value_root(&defaults), sections[selected].id);
                if (source) {
                    (void)put(dialog->fields, values, sections[selected].id,
                              yyjson_mut_val_mut_copy(dialog->fields, source));
                }
            }
            pc_json_value_fini(&defaults);
        }
        nk_group_end(ctx);
    }
}
