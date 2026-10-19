#include "ui/rasterize.h"
#include "menu_state.h"
#include "engine/prefs.h"
#include "ui/shortcuts.h"
#include "ui/enable_rules.h"
#include "base/mem.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int panel_check(const pc_ui_state *state, const char *id)
{
    const char *alias = pc_ui_panel_alias(id);
    if (alias) {
        id = alias;
    }
    static const struct {
        const char *key;
        size_t offset;
    } fields[] = {
#define P(key, field) {"window.toggle." key, offsetof(pc_ui_panels, field)}
        P("layers", layers),
        P("history", history),
        P("properties", properties),
        P("color", color),
        P("navigator", navigator),
        P("character", character),
        P("toolbar", toolbar),
        P("options", options_bar),
        P("dock", dock),
        P("brushSettings", brush_settings)
#undef P
    };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        if (!strcmp(id, fields[i].key)) {
            return *(const bool *)((const char *)&state->panels + fields[i].offset);
        }
    }
    if (!strcmp(id, "window.togglePanels")) {
        return state->panels.toolbar || state->panels.options_bar || state->panels.dock;
    }
    return -1;
}
static int checked(const pc_engine_session *session, const pc_ui_state *state, const char *id)
{
    if (!strcmp(id, "window.panel.timeline"))
        return state->timeline.open;
    if (!strcmp(id, "type.panels.glyphs") || !strcmp(id, "window.panel.glyphs")) {
        return state->type_panels.glyphs;
    }
    if (!strcmp(id, "type.panels.characterStyles") || !strcmp(id, "window.panel.characterStyles")) {
        return state->type_panels.styles && state->type_panels.styles_tab == 0;
    }
    if (!strcmp(id, "type.panels.paragraphStyles") || !strcmp(id, "window.panel.paragraphStyles")) {
        return state->type_panels.styles && state->type_panels.styles_tab == 1;
    }
    if (!strcmp(id, "window.workspace.lockWorkspace")) {
        pc_json_value locked = PC_JSON_VALUE_INIT;
        bool on = pc_engine_prefs_get(session, "workspaceLocked", &locked, NULL) == PC_OK &&
                  yyjson_mut_get_bool(pc_json_value_root(&locked));
        pc_json_value_fini(&locked);
        return on;
    }
    const char *workspace = NULL;
    if (strcmp(id, "window.workspace.resetWorkspace") && pc_ui_workspace_name(id, &workspace))
        return state->workspace &&
               !strcmp(state->workspace, workspace && *workspace ? workspace : "Essentials");
    int panel = panel_check(state, id);
    if (panel >= 0) {
        return panel;
    }
    pc_ui_view_options view = state->view;
    bool *flag = NULL;
    if (!strcmp(id, "view.rulers")) {
        return state->extras.rulers;
    }
    if (!strcmp(id, "view.show.grid")) {
        return state->extras.grid;
    }
    if (!strcmp(id, "view.show.guides")) {
        return state->extras.guides;
    }
    if (!strcmp(id, "view.snap")) {
        return state->extras.snap;
    }
    if (!strcmp(id, "view.lockGuides")) {
        return state->extras.lock_guides;
    }
    if (!strcmp(id, "view.extras")) {
        return view.extras;
    }
    if (!strcmp(id, "view.flipHorizontal")) {
        return view.flip_horizontal;
    }
    if (!strcmp(id, "view.pixelAspectRatioCorrection")) {
        return view.pixel_aspect_correction;
    }
    if (!strncmp(id, "view.show.", 10)) {
        flag = pc_ui_view_show_flag(&view, id + 10);
    }
    if (!strncmp(id, "view.snapTo.", 12)) {
        flag = pc_ui_view_snap_flag(&view, id + 12);
    }
    if (flag) {
        return *flag;
    }
    if (!strncmp(id, "view.screenMode.", 16)) {
        return !strcmp(id + 16, pc_ui_view_screen_mode(&view));
    }
    if (!strncmp(id, "window.theme.", 13)) {
        return !strcmp(id + 13, pc_ui_theme_kind_id(state->theme));
    }
    if (!strncmp(id, "image.mode.", 11)) {
        const pc_engine_doc_state *active = pc_engine_session_active(session);
        const pc_document *doc = active ? pc_doc_arc_get(active->doc) : NULL;
        static const struct {
            const char *id;
            pc_color_mode mode;
        } modes[] = {{"rgb", PC_COLOR_MODE_RGB},
                     {"grayscale", PC_COLOR_MODE_GRAYSCALE},
                     {"cmyk", PC_COLOR_MODE_CMYK},
                     {"lab", PC_COLOR_MODE_LAB},
                     {"multichannel", PC_COLOR_MODE_MULTICHANNEL},
                     {"indexedColor", PC_COLOR_MODE_INDEXED},
                     {"bitmap", PC_COLOR_MODE_BITMAP},
                     {"duotone", PC_COLOR_MODE_DUOTONE}};
        for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
            if (!strcmp(id + 11, modes[i].id)) {
                return doc && doc->mode == modes[i].mode;
            }
        }
        if (!strcmp(id + 11, "bits8")) {
            return doc && doc->depth == PC_SAMPLE_TYPE_U8;
        }
        if (!strcmp(id + 11, "bits16")) {
            return doc && doc->depth == PC_SAMPLE_TYPE_U16;
        }
        if (!strcmp(id + 11, "bits32")) {
            return doc && doc->depth == PC_SAMPLE_TYPE_F32;
        }
    }
    pc_ui_tool tool;
    if (pc_ui_tool_from_select_id(id, &tool)) {
        return state->tool == tool;
    }
    return -1;
}
static pc_status add_dynamic(pc_ui_menu_item *items, size_t *used, const char *id, const char *label,
                             const char *top, const char *sub, int check, pc_error *err)
{
    pc_ui_menu_item *item = &items[(*used)++];
    *item = (pc_ui_menu_item){0};
    item->id = pc_strndup(id, strlen(id));
    item->label = pc_strndup(label, strlen(label));
    item->path[0] = pc_strndup(top, strlen(top));
    item->depth = 1;
    if (sub) {
        item->path[1] = pc_strndup(sub, strlen(sub));
        item->depth = 2;
    }
    item->checked = check;
    item->enabled = true;
    if (!item->id || !item->label || !item->path[0] || (sub && !item->path[1])) {
        return pc_fail(err, PC_ERR_NOMEM, "dynamic menu item");
    }
    return PC_OK;
}
pc_status pc_ui_menu_state_items(const pc_engine_session *session, const pc_ui_state *state,
                                 pc_ui_menu_item **out, size_t *count, pc_error *err)
{
    if (!session || !state || !out || !count) {
        return pc_fail(err, PC_ERR_INVALID, "missing menu state");
    }
    pc_ui_menu_item *items = NULL;
    size_t n = 0;
    pc_status status = pc_ui_menu_items(session, &items, &n, err);
    if (status != PC_OK) {
        return status;
    }
    size_t ndocs = 0;
    (void)pc_engine_session_documents(session, &ndocs);
    if (ndocs > 4096 || state->n_recent_files > 100) {
        pc_ui_menu_items_free(items, n);
        return pc_fail(err, PC_ERR_LIMIT, "too many dynamic menu items");
    }
    size_t capacity = n;
    pc_ui_menu_item *grown =
        pc_grow_array(items, &capacity, n + ndocs + state->n_recent_files, sizeof *items);
    if (!grown) {
        pc_ui_menu_items_free(items, n);
        return pc_fail(err, PC_ERR_NOMEM, "menu state");
    }
    items = grown;
    bool active = pc_engine_session_active(session) != NULL;
    for (size_t i = 0; i < n; i++) {
        items[i].checked = checked(session, state, items[i].id);
        if(!state->transform.active && state->has_crop && (state->crop_editing || state->crop_can_undo || state->crop_can_redo)) {
            if(!strcmp(items[i].id,"edit.undo"))items[i].enabled=state->crop_can_undo;
            if(!strcmp(items[i].id,"edit.redo"))items[i].enabled=state->crop_can_redo;
        }
        if (state->transform.active && !strcmp(items[i].id, "edit.undo"))
            items[i].enabled = state->transform.can_undo;
        if (state->transform.active && !strcmp(items[i].id, "edit.redo"))
            items[i].enabled = state->transform.can_redo;
        if (pc_ui_rasterize_filter_needed(session, items[i].id, NULL))
            items[i].enabled = true;
        if (pc_ui_command_disabled(session, state, items[i].id)) {
            items[i].enabled = false;
        }
        if (state->n_dialogs && !pc_ui_is_nav_command(items[i].id)) {
            items[i].enabled = false;
        }
        if (!active && (!strncmp(items[i].id, "view.zoom", 9) || !strcmp(items[i].id, "view.fitOnScreen") ||
                        !strcmp(items[i].id, "view.actualPixels") ||
                        !strcmp(items[i].id, "view.rotateView") || !strcmp(items[i].id, "view.resetView"))) {
            items[i].enabled = false;
        }
    }
    const pc_engine_doc_state *docs = pc_engine_session_documents(session, &ndocs);
    size_t selected = SIZE_MAX;
    (void)pc_engine_session_active_index(session, &selected);
    for (size_t i = 0; i < ndocs; i++) {
        char id[64];
        (void)snprintf(id, sizeof id, "window.document.%zu", i);
        status = add_dynamic(items, &n, id, pc_str_c(&pc_doc_arc_get(docs[i].doc)->name), "Window", NULL,
                             selected == i, err);
        if (status != PC_OK) {
            goto fail;
        }
        if (state->n_dialogs) {
            items[n - 1].enabled = false;
        }
    }
    for (size_t i = 0; i < state->n_recent_files; i++) {
        char id[64];
        (void)snprintf(id, sizeof id, "file.openRecent.%zu", i);
        const char *name = strrchr(state->recent_files[i], '/');
        name = name ? name + 1 : state->recent_files[i];
        status = add_dynamic(items, &n, id, name, "File", "Open Recent", -1, err);
        if (status != PC_OK) {
            goto fail;
        }
        if (state->n_dialogs) {
            items[n - 1].enabled = false;
        }
    }
    pc_json_value saved = PC_JSON_VALUE_INIT;
    status = pc_engine_prefs_get(session, "workspaces", &saved, err);
    if (status == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&saved);
        size_t wanted = yyjson_mut_obj_size(root);
        if (wanted > 128)
            wanted = 128;
        pc_ui_menu_item *next = pc_grow_array(items, &capacity, n + wanted, sizeof *items);
        if (!next)
            status = pc_fail(err, PC_ERR_NOMEM, "workspace menu");
        else
            items = next;
        size_t insertion = n;
        for (size_t i = 0; i < n; i++)
            if (!strcmp(items[i].id, "window.workspace.resetWorkspace")) {
                insertion = i;
                break;
            }
        yyjson_mut_obj_iter iter = yyjson_mut_obj_iter_with(root);
        yyjson_mut_val *key;
        for (size_t i = 0; status == PC_OK && i < wanted && (key = yyjson_mut_obj_iter_next(&iter)); i++) {
            const char *name = yyjson_mut_get_str(key);
            size_t length = yyjson_mut_get_len(key);
            if (!name || !length || length > 128 || strlen(name) != length)
                continue;
            char id[160];
            (void)snprintf(id, sizeof id, "window.workspace.saved.%s", name);
            status = add_dynamic(items, &n, id, name, "Window", "Workspace",
                                 state->workspace && !strcmp(state->workspace, name), err);
            if (status == PC_OK && state->n_dialogs)
                items[n - 1].enabled = false;
            if (status == PC_OK && insertion < n - 1) {
                pc_ui_menu_item added = items[n - 1];
                memmove(&items[insertion + 1], &items[insertion], (n - 1 - insertion) * sizeof *items);
                items[insertion++] = added;
            }
        }
        for (size_t i = 0; i < n; i++)
            if (!strcmp(items[i].id, "window.workspace.deleteWorkspace")) {
                bool available = false;
                yyjson_mut_obj_iter choices = yyjson_mut_obj_iter_with(root);
                yyjson_mut_val *name;
                while ((name = yyjson_mut_obj_iter_next(&choices)))
                    if (state->workspace && strcmp(state->workspace, yyjson_mut_get_str(name)))
                        available = true;
                items[i].enabled = available && !state->n_dialogs;
            }
    }
    pc_json_value_fini(&saved);
    if (status != PC_OK)
        goto fail;
    pc_json_value prefs = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(session, "export", &prefs, NULL) == PC_OK) {
        const char *format =
            yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&prefs), "quickExportFormat"));
        const char *display = format && !strcmp(format, "jpg")    ? "JPEG"
                              : format && !strcmp(format, "gif")  ? "GIF"
                              : format && !strcmp(format, "webp") ? "WebP"
                                                                  : "PNG";
        for (size_t i = 0; i < n; i++) {
            if (!strcmp(items[i].id, "file.export.quickExportAsPng")) {
                char label[64];
                (void)snprintf(label, sizeof label, "Quick Export as %s", display);
                char *copy = pc_strndup(label, strlen(label));
                if (!copy) {
                    status = pc_fail(err, PC_ERR_NOMEM, "menu label");
                    break;
                }
                free(items[i].label);
                items[i].label = copy;
            }
        }
    }
    pc_json_value_fini(&prefs);
    if (status != PC_OK) {
        goto fail;
    }
    *out = items;
    *count = n;
    return PC_OK;
fail:
    pc_ui_menu_items_free(items, n);
    return status;
}
