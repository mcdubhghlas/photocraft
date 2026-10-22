#include "prefs_modals.h"
#include "shell_internal.h"
#include "ui/shortcut_model.h"
#include "ui/menus.h"
#include "ui/state.h"
#include "ui/i18n.h"
#include "engine/prefs.h"
#include "base/buf.h"
#include "base/json.h"
#include <stdio.h>
#include <string.h>

/* Run an engine command from an owned mutable params value (serialise + reparse to an immutable
 * tree the engine accepts). The reply is discarded. */
static pc_status run_cmd(pc_ui_shell_app *app, const char *cmd, yyjson_mut_val *params, pc_error *err)
{
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    pc_json_value reply = PC_JSON_VALUE_INIT;
    pc_status rc = pc_json_write(params, 0, &text, err);
    if (rc == PC_OK) {
        rc = pc_json_parse(pc_buf_cstr(&text), text.len, &read, err);
    }
    if (rc == PC_OK) {
        rc = pc_engine_session_execute(pc_ui_shell_app_session(app), cmd, yyjson_doc_get_root(read), &reply,
                                       err);
    }
    pc_json_value_fini(&reply);
    yyjson_doc_free(read);
    pc_buf_fini(&text);
    return rc;
}

static bool put(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, yyjson_mut_val *value)
{
    yyjson_mut_val *k = yyjson_mut_strcpy(doc, key);
    return k && value && yyjson_mut_obj_put(obj, k, value);
}
static yyjson_mut_val *ensure_obj(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(root, key);
    if (!yyjson_mut_is_obj(v)) {
        v = yyjson_mut_obj(doc);
        if (v) {
            (void)put(doc, root, key, v);
        }
    }
    return v;
}
static yyjson_mut_val *ensure_arr(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(root, key);
    if (!yyjson_mut_is_arr(v)) {
        v = yyjson_mut_arr(doc);
        if (v) {
            (void)put(doc, root, key, v);
        }
    }
    return v;
}
static bool arr_has(yyjson_mut_val *arr, const char *s)
{
    size_t i, n;
    yyjson_mut_val *item;
    yyjson_mut_arr_foreach(arr, i, n, item)
    {
        const char *v = yyjson_mut_get_str(item);
        if (v && s && !strcmp(v, s)) {
            return true;
        }
    }
    return false;
}
static void arr_remove(yyjson_mut_val *arr, const char *s)
{
    for (size_t i = yyjson_mut_arr_size(arr); i-- > 0;) {
        const char *v = yyjson_mut_get_str(yyjson_mut_arr_get(arr, i));
        if (v && s && !strcmp(v, s)) {
            (void)yyjson_mut_arr_remove(arr, i);
        }
    }
}
static void arr_add(yyjson_mut_doc *doc, yyjson_mut_val *arr, const char *s)
{
    if (!arr_has(arr, s)) {
        yyjson_mut_val *v = yyjson_mut_strcpy(doc, s);
        if (v) {
            (void)yyjson_mut_arr_append(arr, v);
        }
    }
}
/* The binding in effect in the draft: overrides[id] (empty string = none) else the default. Returns
 * true and fills buf when there is one. */
static bool effective(yyjson_mut_val *overrides, const char *id, const char *def, char *buf, size_t cap)
{
    const char *ov = yyjson_mut_get_str(yyjson_mut_obj_get(overrides, id));
    if (ov) {
        if (!*ov) {
            return false;
        }
        (void)snprintf(buf, cap, "%s", ov);
        return true;
    }
    if (def && *def) {
        (void)snprintf(buf, cap, "%s", def);
        return true;
    }
    return false;
}
/* ASCII case-insensitive substring. */
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

pc_status pc_ui_shell_shortcuts_open(pc_ui_shell_app *app, uint64_t tab, pc_json_value *out, pc_error *err)
{
    pc_engine_session *s = pc_ui_shell_app_session(app);
    pc_json_value shortcuts = PC_JSON_VALUE_INIT, menus = PC_JSON_VALUE_INIT, toolbar = PC_JSON_VALUE_INIT;
    pc_status status = pc_engine_prefs_get(s, "shortcuts", &shortcuts, err);
    if (status == PC_OK) {
        status = pc_engine_prefs_get(s, "menus", &menus, err);
    }
    if (status == PC_OK) {
        status = pc_engine_prefs_get(s, "toolbar", &toolbar, err);
    }
    yyjson_mut_doc *d = status == PC_OK ? yyjson_mut_doc_new(NULL) : NULL;
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
    if (status == PC_OK && !root) {
        status = pc_fail(err, PC_ERR_NOMEM, "shortcuts editor");
    }
    if (status == PC_OK) {
        yyjson_mut_val *menus_root = pc_json_value_root(&menus);
        yyjson_mut_val *hidden = menus_root ? yyjson_mut_obj_get(menus_root, "hidden") : NULL;
        yyjson_mut_val *colors = menus_root ? yyjson_mut_obj_get(menus_root, "colors") : NULL;
        yyjson_mut_val *tb = pc_json_value_root(&toolbar);
        yyjson_mut_val *tbhidden = tb ? yyjson_mut_obj_get(tb, "hidden") : NULL;
        bool ok =
            yyjson_mut_obj_add_str(d, root, "__prefsui", "shortcuts") &&
            yyjson_mut_obj_add_str(d, root, "__label", "Keyboard Shortcuts and Menus") &&
            yyjson_mut_obj_add_uint(d, root, "tab", tab > 2 ? 0 : tab) &&
            yyjson_mut_obj_add_str(d, root, "filter", "") &&
            yyjson_mut_obj_add_str(d, root, "selected", "") &&
            yyjson_mut_obj_add_bool(d, root, "capture", false) &&
            yyjson_mut_obj_add_str(d, root, "message", "") &&
            yyjson_mut_obj_add_val(d, root, "overrides",
                                   yyjson_mut_val_mut_copy(d, pc_json_value_root(&shortcuts))) &&
            yyjson_mut_obj_add_val(d, root, "hidden",
                                   hidden ? yyjson_mut_val_mut_copy(d, hidden) : yyjson_mut_arr(d)) &&
            yyjson_mut_obj_add_val(d, root, "colors",
                                   colors ? yyjson_mut_val_mut_copy(d, colors) : yyjson_mut_obj(d)) &&
            yyjson_mut_obj_add_val(d, root, "toolbarHidden",
                                   tbhidden ? yyjson_mut_val_mut_copy(d, tbhidden) : yyjson_mut_arr(d));
        if (!ok || !yyjson_mut_is_obj(yyjson_mut_obj_get(root, "overrides"))) {
            status = pc_fail(err, PC_ERR_NOMEM, "shortcuts editor");
        }
    }
    if (status == PC_OK) {
        status = pc_ui_shell_dialog_store(app, PC_DIALOG_COMMAND, d, root, out, err);
        d = NULL;
    }
    yyjson_mut_doc_free(d);
    pc_json_value_fini(&shortcuts);
    pc_json_value_fini(&menus);
    pc_json_value_fini(&toolbar);
    return status;
}

bool pc_ui_shell_shortcuts_owns(yyjson_mut_val *fields)
{
    const char *kind = yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__prefsui"));
    return kind && !strcmp(kind, "shortcuts");
}

static const char *const MENU_COLORS[] = {"none",  "red",  "orange", "yellow",
                                          "green", "blue", "violet", "gray"};

static void shortcuts_tab(struct nk_context *ctx, pc_ui_shell_app *app, pc_ui_dialog *dialog, int tab,
                          const char *filter)
{
    yyjson_mut_doc *doc = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *overrides = ensure_obj(doc, root, "overrides");
    yyjson_mut_val *hidden = ensure_arr(doc, root, "hidden");
    yyjson_mut_val *colors = ensure_obj(doc, root, "colors");
    const char *selected = yyjson_mut_get_str(yyjson_mut_obj_get(root, "selected"));
    bool capture = yyjson_mut_get_bool(yyjson_mut_obj_get(root, "capture"));
    pc_ui_shortcut_item *items = NULL;
    size_t n = 0;
    if (pc_ui_shortcut_items(pc_ui_shell_app_session(app), &items, &n, NULL) != PC_OK) {
        nk_layout_row_dynamic(ctx, 24, 1);
        nk_label(ctx, pc_i18n_t("Could not list commands"), NK_TEXT_LEFT);
        return;
    }
    const char *last_group = NULL;
    for (size_t i = 0; i < n; i++) {
        pc_ui_shortcut_item *it = &items[i];
        if (tab == 1) {
            pc_ui_tool tool;
            if (pc_ui_tool_from_select_id(it->id, &tool)) {
                continue; /* tools are on the Toolbar tab, not the Menus tab */
            }
        }
        char current[128] = {0};
        bool has = effective(overrides, it->id, it->def, current, sizeof current);
        if (tab == 0 && it->depth == 0 && !it->def && !has) {
            continue;
        }
        if (filter && *filter &&
            !contains_ci(it->label, filter) && !contains_ci(it->id, filter) &&
            !contains_ci(it->path[0], filter) && !contains_ci(current, filter)) {
            continue;
        }
        const char *group = it->depth ? it->path[0] : pc_i18n_t("Other");
        if (!last_group || strcmp(last_group, group)) {
            nk_layout_row_dynamic(ctx, 22, 1);
            nk_label(ctx, pc_i18n_t(group), NK_TEXT_LEFT);
            last_group = group;
        }
        /* Row label: "Sub › Item" (menu depth beyond the group) or just the command label. */
        char name[256];
        if (it->depth >= 2) {
            (void)snprintf(name, sizeof name, "   %s \xe2\x80\xba %s", pc_i18n_t(it->path[1]),
                           pc_i18n_t(it->label));
        } else {
            (void)snprintf(name, sizeof name, "   %s", pc_i18n_t(it->label));
        }
        bool sel = selected && !strcmp(selected, it->id);
        if (tab == 0) {
            nk_layout_row_template_begin(ctx, 24);
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_push_static(ctx, 150);
            nk_layout_row_template_end(ctx);
            if (nk_select_label(ctx, name, NK_TEXT_LEFT, sel)) {
                (void)put(doc, root, "selected", yyjson_mut_strcpy(doc, it->id));
                selected = yyjson_mut_get_str(yyjson_mut_obj_get(root, "selected"));
                sel = true;
            }
            char pretty[160];
            if (sel && capture) {
                (void)snprintf(pretty, sizeof pretty, "%s", pc_i18n_t("Press keys…"));
            } else if (has) {
                (void)pc_ui_shortcut_pretty(current, pretty, sizeof pretty);
            } else {
                pretty[0] = '\0';
            }
            pc_ui_shell_widget_mapping(app, it->id, nk_widget_bounds(ctx));
            if (nk_button_label(ctx, pretty)) {
                (void)put(doc, root, "selected", yyjson_mut_strcpy(doc, it->id));
                (void)put(doc, root, "capture", yyjson_mut_bool(doc, true));
                (void)put(doc, root, "message", yyjson_mut_str(doc, ""));
                selected = yyjson_mut_get_str(yyjson_mut_obj_get(root, "selected"));
                capture = true;
            }
        } else {
            nk_layout_row_template_begin(ctx, 24);
            nk_layout_row_template_push_dynamic(ctx);
            nk_layout_row_template_push_static(ctx, 110);
            nk_layout_row_template_end(ctx);
            nk_bool visible = !arr_has(hidden, it->id);
            if (nk_checkbox_label(ctx, name, &visible)) {
                if (visible) {
                    arr_remove(hidden, it->id);
                } else {
                    arr_add(doc, hidden, it->id);
                }
            }
            const char *col = yyjson_mut_get_str(yyjson_mut_obj_get(colors, it->id));
            if (!col) {
                col = "none";
            }
            if (nk_combo_begin_label(ctx, pc_i18n_t(col), nk_vec2(110, 220))) {
                nk_layout_row_dynamic(ctx, 22, 1);
                for (size_t c = 0; c < sizeof MENU_COLORS / sizeof *MENU_COLORS; c++) {
                    if (nk_combo_item_label(ctx, pc_i18n_t(MENU_COLORS[c]), NK_TEXT_LEFT)) {
                        if (!strcmp(MENU_COLORS[c], "none")) {
                            (void)yyjson_mut_obj_remove_str(colors, it->id);
                        } else {
                            (void)put(doc, colors, it->id, yyjson_mut_str(doc, MENU_COLORS[c]));
                        }
                    }
                }
                nk_combo_end(ctx);
            }
        }
    }
    pc_ui_shortcut_items_free(items, n);
}

static void toolbar_tab(struct nk_context *ctx, pc_ui_dialog *dialog)
{
    yyjson_mut_doc *doc = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    yyjson_mut_val *hidden = ensure_arr(doc, root, "toolbarHidden");
    for (size_t i = 0; i < PC_TOOL_N; i++) {
        pc_ui_tool tool = PC_UI_TOOL_ALL[i];
        const char *name = pc_ui_tool_serde(tool);
        nk_layout_row_template_begin(ctx, 24);
        nk_layout_row_template_push_dynamic(ctx);
        nk_layout_row_template_push_static(ctx, 40);
        nk_layout_row_template_end(ctx);
        nk_bool on = !arr_has(hidden, name);
        if (nk_checkbox_label(ctx, pc_i18n_t(pc_ui_tool_label(tool)), &on)) {
            if (on) {
                arr_remove(hidden, name);
            } else {
                arr_add(doc, hidden, name);
            }
        }
        char key[2] = {pc_ui_tool_key(tool), 0};
        nk_label(ctx, key[0] ? key : "", NK_TEXT_LEFT);
    }
}

void pc_ui_shell_shortcuts_body(pc_ui_shell_app *app, pc_ui_dialog_runtime *runtime, pc_ui_dialog *dialog)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    yyjson_mut_doc *doc = dialog->fields;
    yyjson_mut_val *root = yyjson_mut_doc_get_root(doc);
    int tab = (int)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "tab"));
    /* Tab strip. */
    const char *tabs[] = {"Keyboard Shortcuts", "Menus", "Toolbar"};
    nk_layout_row_dynamic(ctx, 26, 3);
    for (int i = 0; i < 3; i++) {
        if (nk_select_label(ctx, pc_i18n_t(tabs[i]), NK_TEXT_CENTERED, tab == i) && tab != i) {
            tab = i;
            (void)put(doc, root, "tab", yyjson_mut_uint(doc, (uint64_t)i));
            (void)put(doc, root, "capture", yyjson_mut_bool(doc, false));
        }
    }
    if (tab == 2) {
        float h = fmaxf(160, nk_window_get_content_region(ctx).h - 90);
        nk_layout_row_dynamic(ctx, h, 1);
        if (nk_group_begin(ctx, "toolbar-list", NK_WINDOW_BORDER)) {
            toolbar_tab(ctx, dialog);
            nk_group_end(ctx);
        }
        nk_layout_row_dynamic(ctx, 28, 1);
        if (nk_button_label(ctx, pc_i18n_t("Restore Defaults"))) {
            (void)put(doc, root, "toolbarHidden", yyjson_mut_arr(doc));
        }
        return;
    }
    /* Search box. */
    const char *stored = yyjson_mut_get_str(yyjson_mut_obj_get(root, "filter"));
    if (!stored) {
        stored = "";
    }
    const char *filter = stored;
    pc_ui_dialog_text *sb = pc_ui_dialog_text_buffer(runtime, dialog->id, "filter", stored);
    nk_layout_row_dynamic(ctx, 26, 1);
    if (sb) {
        nk_flags edited = nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, (char *)sb->text.data,
                                                         (int)sb->text.cap, nk_filter_default);
        sb->active = (edited & NK_EDIT_ACTIVE) != 0;
        sb->text.len = strlen(pc_buf_cstr(&sb->text));
        if (strcmp(stored, pc_buf_cstr(&sb->text))) {
            (void)put(doc, root, "filter", yyjson_mut_strncpy(doc, pc_buf_cstr(&sb->text), sb->text.len));
        }
        filter = pc_buf_cstr(&sb->text);
    } else {
        nk_label(ctx, pc_i18n_t("command or shortcut"), NK_TEXT_LEFT);
    }
    float h = fmaxf(160, nk_window_get_content_region(ctx).h - 120);
    nk_layout_row_dynamic(ctx, h, 1);
    if (nk_group_begin(ctx, "shortcut-list", NK_WINDOW_BORDER)) {
        shortcuts_tab(ctx, app, dialog, tab, filter);
        nk_group_end(ctx);
    }
    const char *selected = yyjson_mut_get_str(yyjson_mut_obj_get(root, "selected"));
    yyjson_mut_val *overrides = ensure_obj(doc, root, "overrides");
    if (tab == 0) {
        nk_layout_row_dynamic(ctx, 28, 3);
        bool has_sel = selected && *selected;
        if (!has_sel) {
            nk_widget_disable_begin(ctx);
        }
        if (nk_button_label(ctx, pc_i18n_t("Use Default")) && has_sel) {
            (void)yyjson_mut_obj_remove_str(overrides, selected);
            (void)put(doc, root, "message", yyjson_mut_str(doc, "Default restored."));
        }
        if (nk_button_label(ctx, pc_i18n_t("Delete Shortcut")) && has_sel) {
            (void)put(doc, overrides, selected, yyjson_mut_str(doc, ""));
            (void)put(doc, root, "message", yyjson_mut_str(doc, "Shortcut removed."));
        }
        if (!has_sel) {
            nk_widget_disable_end(ctx);
        }
        if (nk_button_label(ctx, pc_i18n_t("Reset All to Defaults"))) {
            (void)put(doc, root, "overrides", yyjson_mut_obj(doc));
            (void)put(doc, root, "message", yyjson_mut_str(doc, "All shortcuts reset to PhotoCraft defaults."));
        }
    } else {
        nk_layout_row_dynamic(ctx, 28, 1);
        if (nk_button_label(ctx, pc_i18n_t("Show All Menu Items"))) {
            (void)put(doc, root, "hidden", yyjson_mut_arr(doc));
            (void)put(doc, root, "colors", yyjson_mut_obj(doc));
        }
    }
    const char *message = yyjson_mut_get_str(yyjson_mut_obj_get(root, "message"));
    if (message && *message) {
        nk_layout_row_dynamic(ctx, 22, 1);
        nk_label(ctx, pc_i18n_t(message), NK_TEXT_LEFT);
    }
}

pc_status pc_ui_shell_shortcuts_confirm(pc_ui_shell_app *app, uint64_t id, pc_json_value *out, pc_error *err)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_dialog *dialog = pc_ui_state_dialog_mut(ui, id);
    yyjson_mut_val *root = dialog ? yyjson_mut_doc_get_root(dialog->fields) : NULL;
    if (!root || !pc_ui_shell_shortcuts_owns(root)) {
        return pc_fail(err, PC_ERR_INVALID, "not the shortcuts editor");
    }
    yyjson_mut_val *overrides = yyjson_mut_obj_get(root, "overrides");
    if (!yyjson_mut_is_obj(overrides)) {
        return pc_fail(err, PC_ERR_INVALID, "overrides must map command ids to shortcut strings");
    }
    /* 1. edit.keyboardShortcuts: clear every override, apply the draft, let the engine strip clashes. */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL;
    pc_status status =
        params && yyjson_mut_obj_add_bool(d, params, "reset", true) &&
                yyjson_mut_obj_add_bool(d, params, "allowUnknown", true) &&
                yyjson_mut_obj_add_bool(d, params, "removeConflicts", true) &&
                yyjson_mut_obj_add_val(d, params, "set", yyjson_mut_val_mut_copy(d, overrides))
            ? PC_OK
            : pc_fail(err, PC_ERR_NOMEM, "shortcuts");
    if (status == PC_OK) {
        status = run_cmd(app, "edit.keyboardShortcuts", params, err);
    }
    yyjson_mut_doc_free(d);
    /* 2. prefs.set: menu visibility/colour and the toolbar tool set. */
    if (status == PC_OK) {
        yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *proot = pd ? yyjson_mut_obj(pd) : NULL, *values = pd ? yyjson_mut_obj(pd) : NULL;
        yyjson_mut_val *hidden = yyjson_mut_obj_get(root, "hidden");
        yyjson_mut_val *colors = yyjson_mut_obj_get(root, "colors");
        yyjson_mut_val *tb = yyjson_mut_obj_get(root, "toolbarHidden");
        bool ok = proot && values &&
                  yyjson_mut_obj_add_val(pd, values, "menus.hidden",
                                         hidden ? yyjson_mut_val_mut_copy(pd, hidden) : yyjson_mut_arr(pd)) &&
                  yyjson_mut_obj_add_val(pd, values, "menus.colors",
                                         colors ? yyjson_mut_val_mut_copy(pd, colors) : yyjson_mut_obj(pd)) &&
                  yyjson_mut_obj_add_val(pd, values, "toolbar.hidden",
                                         tb ? yyjson_mut_val_mut_copy(pd, tb) : yyjson_mut_arr(pd)) &&
                  yyjson_mut_obj_add_val(pd, proot, "values", values);
        if (!ok) {
            status = pc_fail(err, PC_ERR_NOMEM, "menu preferences");
        } else {
            status = run_cmd(app, "prefs.set", proot, err);
        }
        yyjson_mut_doc_free(pd);
    }
    if (status == PC_OK) {
        pc_ui_shell_apply_preferences(app);
        pc_ui_state_close_dialog(ui, id);
        yyjson_mut_doc *rd = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *rr = rd ? yyjson_mut_obj(rd) : NULL;
        if (rr && yyjson_mut_obj_add_bool(rd, rr, "ok", true)) {
            status = pc_json_value_from_mut(rr, out, err);
        } else {
            status = pc_fail(err, PC_ERR_NOMEM, "shortcuts result");
        }
        yyjson_mut_doc_free(rd);
    }
    return status;
}

bool pc_ui_shell_shortcuts_key(pc_ui_shell_app *app, const SDL_Event *event)
{
    if (!event || event->type != SDL_EVENT_KEY_DOWN) {
        return false;
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->n_dialogs) {
        return false;
    }
    pc_ui_dialog *dialog = &ui->dialogs[ui->n_dialogs - 1];
    yyjson_mut_val *root = yyjson_mut_doc_get_root(dialog->fields);
    if (!pc_ui_shell_shortcuts_owns(root) ||
        (int)yyjson_mut_get_uint(yyjson_mut_obj_get(root, "tab")) != 0 ||
        !yyjson_mut_get_bool(yyjson_mut_obj_get(root, "capture"))) {
        return false;
    }
    const char *selected = yyjson_mut_get_str(yyjson_mut_obj_get(root, "selected"));
    if (!selected || !*selected) {
        return false;
    }
    yyjson_mut_doc *doc = dialog->fields;
    if (event->key.key == SDLK_ESCAPE) {
        (void)put(doc, root, "capture", yyjson_mut_bool(doc, false));
        return true;
    }
    unsigned mods = 0;
    SDL_Keymod km = event->key.mod;
    if (km & SDL_KMOD_ALT) {
        mods |= PC_UI_MOD_ALT;
    }
    if (km & SDL_KMOD_SHIFT) {
        mods |= PC_UI_MOD_SHIFT;
    }
    if (km & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) {
        mods |= PC_UI_MOD_COMMAND;
    }
    if ((event->key.key == SDLK_BACKSPACE || event->key.key == SDLK_DELETE) && mods == 0) {
        (void)put(doc, ensure_obj(doc, root, "overrides"), selected, yyjson_mut_str(doc, ""));
        (void)put(doc, root, "capture", yyjson_mut_bool(doc, false));
        (void)put(doc, root, "message", yyjson_mut_str(doc, "Shortcut removed."));
        return true;
    }
    pc_ui_key key = pc_ui_shell_input_key(event->key.key);
    char sc[128];
    if (key != PC_KEY_NONE && pc_ui_shortcut_capture(key, mods, sc, sizeof sc)) {
        char pretty[160];
        (void)pc_ui_shortcut_pretty(sc, pretty, sizeof pretty);
        char message[256];
        (void)snprintf(message, sizeof message, "%s assigned.", pretty);
        (void)put(doc, ensure_obj(doc, root, "overrides"), selected, yyjson_mut_strcpy(doc, sc));
        (void)put(doc, root, "capture", yyjson_mut_bool(doc, false));
        (void)put(doc, root, "message", yyjson_mut_strcpy(doc, message));
        return true;
    }
    return true; /* swallow other keys while capturing */
}
