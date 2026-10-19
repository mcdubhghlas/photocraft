#include "ui/i18n.h"
#include "type_panels_ui.h"
#include "type_controls.h"
#include "dialogs.h"
#include "base/utf8.h"
#include "base/mem.h"
#include "base/hash.h"
#include "text/glyphs.h"
#include "doc/json.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
void pc_ui_type_panels_runtime_fini(pc_ui_type_panels_runtime *runtime)
{
    pc_str_fini(&runtime->charmap_family);
    pc_str_fini(&runtime->charmap_style);
    pc_str_fini(&runtime->category);
    free(runtime->charmap);
    free(runtime->filtered);
    memset(runtime, 0, sizeof *runtime);
}
static pc_status execute(pc_ui_shell_app *app, const char *id, yyjson_mut_val *p, pc_json_value *out,
                         pc_error *err)
{
    pc_buf json = PC_BUF_INIT;
    pc_status status = pc_json_write(p, 0, &json, err);
    if (status == PC_OK) {
        status =
            pc_engine_session_execute_json(pc_ui_shell_app_session(app), id, pc_buf_cstr(&json), out, err);
    }
    pc_buf_fini(&json);
    return status;
}
static pc_status spell_refresh(pc_ui_shell_app *app, pc_error *err)
{
    pc_ui_type_panels *panels = &pc_ui_shell_app_state(app)->type_panels;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    pc_ui_type_panels_runtime *runtime = pc_ui_shell_type_panels_runtime(app);
    if (!state || (runtime->spell_document && pc_doc_arc_get(state->doc)->id != runtime->spell_document)) {
        return pc_fail(err, PC_ERR_INVALID, "the spelling document changed");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL, *ignore = doc ? yyjson_mut_arr(doc) : NULL;
    bool ok = p && ignore && yyjson_mut_obj_add_str(doc, p, "action", "list") &&
              yyjson_mut_obj_add_bool(doc, p, "allLayers", panels->spell.all_layers);
    for (size_t i = 0; ok && i < panels->spell.ignore.n; i++) {
        ok = yyjson_mut_arr_add_strcpy(doc, ignore, panels->spell.ignore.v[i]);
    }
    ok = ok && yyjson_mut_obj_add_val(doc, p, "ignore", ignore);
    pc_json_value result = PC_JSON_VALUE_INIT;
    pc_status status = ok ? execute(app, "edit.checkSpelling", p, &result, err)
                          : pc_fail(err, PC_ERR_NOMEM, "spelling request");
    if (status == PC_OK) {
        yyjson_mut_val *items = yyjson_mut_obj_get(pc_json_value_root(&result), "misspellings");
        if (!yyjson_mut_is_arr(items)) {
            status = pc_fail(err, PC_ERR_INVALID, "invalid spelling result");
        } else {
            pc_json_value next = PC_JSON_VALUE_INIT;
            status = pc_json_value_from_mut(items, &next, err);
            if (status == PC_OK) {
                pc_json_value_fini(&panels->spell.items);
                panels->spell.items = next;
                if (panels->spell.index >= yyjson_mut_arr_size(items)) {
                    panels->spell.index = 0;
                }
                yyjson_mut_val *item = yyjson_mut_arr_get(items, panels->spell.index);
                const char *suggestion =
                    yyjson_mut_get_str(yyjson_mut_arr_get_first(yyjson_mut_obj_get(item, "suggestions")));
                status = pc_str_set_c(&panels->spell.change_to, suggestion ? suggestion : "", err);
                (void)snprintf(pc_ui_shell_type_panels_runtime(app)->replacement, 1024, "%s",
                               pc_str_c(&panels->spell.change_to));
            }
        }
    }
    pc_json_value_fini(&result);
    yyjson_mut_doc_free(doc);
    return status;
}
pc_status pc_ui_shell_type_panels_menu(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                       pc_json_value *out, pc_error *err)
{
    bool glyph = !strcmp(id, "type.panels.glyphs") || !strcmp(id, "window.panel.glyphs"),
         character =
             !strcmp(id, "type.panels.characterStyles") || !strcmp(id, "window.panel.characterStyles"),
         paragraph =
             !strcmp(id, "type.panels.paragraphStyles") || !strcmp(id, "window.panel.paragraphStyles");
    bool spelling = !strcmp(id, "edit.checkSpelling") && !pc_json_get(params, "action") &&
                    (!pc_json_get(params, "ui") || !yyjson_is_bool(pc_json_get(params, "ui")) ||
                     yyjson_get_bool(pc_json_get(params, "ui")));
    if (!glyph && !character && !paragraph && !spelling) {
        return PC_ERR_NOT_FOUND;
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (ui->n_dialogs) {
        return pc_fail(err, PC_ERR_INVALID, "confirm or cancel the open dialog first");
    }
    pc_ui_type_panels *p = &ui->type_panels;
    if (spelling) {
        bool all = true;
        yyjson_val *value = pc_json_get(params, "allLayers");
        if (value && !pc_json_as_bool(value, &all)) {
            return pc_fail(err, PC_ERR_INVALID, "allLayers must be boolean");
        }
        pc_ui_type_panels next = {0};
        next.has_spell = true;
        next.spell.all_layers = all;
        pc_ui_type_panels_runtime *runtime = pc_ui_shell_type_panels_runtime(app);
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        pc_doc_id old_document = runtime->spell_document;
        runtime->spell_document = state ? pc_doc_arc_get(state->doc)->id : 0;
        pc_ui_spell_dialog old = p->spell;
        p->spell = next.spell;
        pc_status status = spell_refresh(app, err);
        if (status != PC_OK) {
            next.spell = p->spell;
            pc_ui_type_panels_fini(&next);
            p->spell = old;
            runtime->spell_document = old_document;
            return status;
        }
        next.spell = old;
        pc_ui_type_panels_fini(&next);
        p->has_spell = true;
        yyjson_mut_doc *result = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = result ? yyjson_mut_obj(result) : NULL;
        bool ok =
            root && yyjson_mut_obj_add_str(result, root, "dialog", "checkSpelling") &&
            yyjson_mut_obj_add_val(result, root, "misspellings",
                                   yyjson_mut_val_mut_copy(result, pc_json_value_root(&p->spell.items)));
        status = ok ? pc_json_value_from_mut(root, out, err) : pc_fail(err, PC_ERR_NOMEM, "spelling result");
        yyjson_mut_doc_free(result);
        return status;
    }
    bool show = glyph ? !p->glyphs : !p->styles || p->styles_tab != (size_t)paragraph;
    yyjson_val *value = pc_json_get(params, "show");
    if (value && !pc_json_as_bool(value, &show)) {
        return pc_fail(err, PC_ERR_INVALID, "show must be boolean");
    }
    if (glyph) {
        p->glyphs = show;
    } else {
        p->styles = show;
        p->styles_tab = paragraph ? 1 : 0;
    }
    pc_ui_shell_app_changed(app);
    return PC_OK;
}
static yyjson_mut_doc *request(pc_ui_shell_app *app, yyjson_mut_val **params, bool scope)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    *params = doc ? yyjson_mut_obj(doc) : NULL;
    if (*params && scope) {
        (void)pc_ui_type_add_scope(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app), doc, *params,
                                   NULL);
    }
    return doc;
}
static void style_action(pc_ui_shell_app *app, bool paragraph, const char *suffix, uint32_t id, bool with_id)
{
    char command[80];
    (void)snprintf(command, sizeof command, "type.%sStyle.%s", paragraph ? "paragraph" : "character", suffix);
    yyjson_mut_val *p;
    yyjson_mut_doc *doc = request(app, &p,
                                  !strcmp(suffix, "apply") || !strcmp(suffix, "new") ||
                                      !strcmp(suffix, "redefine") || !strcmp(suffix, "clearOverride"));
    if (p) {
        if (with_id) {
            yyjson_mut_obj_add_uint(doc, p, "id", id);
        }
        if (!strcmp(suffix, "apply")) {
            yyjson_mut_obj_add_bool(doc, p, "clearOverrides", (SDL_GetModState() & SDL_KMOD_ALT) != 0);
        }
        if (!strcmp(suffix, "new")) {
            yyjson_mut_obj_add_bool(doc, p, "apply", true);
        }
        pc_ui_shell_queue(app, command, p);
    }
    yyjson_mut_doc_free(doc);
}
static yyjson_mut_val *styles(pc_ui_shell_app *app, bool paragraph, pc_json_value *result)
{
    char command[64];
    (void)snprintf(command, sizeof command, "type.%sStyle.list", paragraph ? "paragraph" : "character");
    yyjson_mut_val *p;
    yyjson_mut_doc *doc = request(app, &p, true);
    if (p) {
        (void)execute(app, command, p, result, NULL);
    }
    yyjson_mut_doc_free(doc);
    return yyjson_mut_obj_get(pc_json_value_root(result), "styles");
}
static void styles_window(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    if (!ui->type_panels.styles) {
        pc_ui_shell_type_panels_runtime(app)->shown[0] = false;
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_type_panels_runtime *runtime = pc_ui_shell_type_panels_runtime(app);
    bool first = !runtime->shown[0];
    runtime->shown[0] = true;
    const char *title = "Type Styles";
    if (nk_begin(ctx, title, nk_rect(600, 140, 330, 420),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                     NK_WINDOW_CLOSABLE)) {
        if (first) {
            nk_widget_disable_begin(ctx);
        }
        nk_layout_row_dynamic(ctx, 26, 2);
        if (nk_button_label(ctx, pc_i18n_t("Character Styles"))) {
            ui->type_panels.styles_tab = 0;
        }
        if (nk_button_label(ctx, pc_i18n_t("Paragraph Styles"))) {
            ui->type_panels.styles_tab = 1;
        }
        bool paragraph = ui->type_panels.styles_tab == 1;
        pc_json_value result = PC_JSON_VALUE_INIT;
        yyjson_mut_val *items = styles(app, paragraph, &result),
                       *current = yyjson_mut_obj_get(pc_json_value_root(&result), "current");
        uint64_t active =
            yyjson_mut_get_uint(yyjson_mut_obj_get(current, paragraph ? "paragraph" : "character"));
        nk_layout_row_dynamic(ctx, 240, 1);
        if (nk_group_begin(ctx, "style-list", NK_WINDOW_BORDER)) {
            size_t i, n;
            yyjson_mut_val *item;
            yyjson_mut_arr_foreach(items, i, n, item)
            {
                const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(item, "name"));
                uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(item, "id"));
                if (!name || id > UINT32_MAX) {
                    continue;
                }
                char label[300];
                (void)snprintf(label, sizeof label, "%s%s", name,
                               id == active &&
                                       yyjson_mut_get_bool(yyjson_mut_obj_get(
                                           current, paragraph ? "paragraphOverride" : "characterOverride"))
                                   ? " +"
                                   : "");
                nk_layout_row_dynamic(ctx, 25, 1);
                nk_bool selected = id == active;
                if (nk_selectable_label(ctx, label, NK_TEXT_LEFT, &selected)) {
                    runtime->selected[paragraph] = (uint32_t)id;
                    style_action(app, paragraph, "apply", (uint32_t)id, true);
                }
            }
            nk_group_end(ctx);
        }
        nk_layout_row_dynamic(ctx, 26, 3);
        if (nk_button_label(ctx, pc_i18n_t("New"))) {
            style_action(app, paragraph, "new", 0, false);
        }
        if (nk_button_label(ctx, pc_i18n_t("Duplicate"))) {
            style_action(app, paragraph, "duplicate", (uint32_t)active, true);
        }
        if (nk_button_label(ctx, pc_i18n_t("Delete"))) {
            style_action(app, paragraph, "delete", (uint32_t)active, true);
        }
        pc_ui_shell_widget_mapping(app, "type.style.options", nk_widget_bounds(ctx));
        if (nk_button_label(ctx, pc_i18n_t("Options"))) {
            ui->type_panels.has_options = true;
            ui->type_panels.options_paragraph = paragraph;
            ui->type_panels.options_id = (uint32_t)active;
            runtime->name[0] = 0;
            const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
            runtime->options_document = state ? pc_doc_arc_get(state->doc)->id : 0;
        }
        if (nk_button_label(ctx, pc_i18n_t("Redefine"))) {
            style_action(app, paragraph, "redefine", (uint32_t)active, true);
        }
        if (nk_button_label(ctx, pc_i18n_t("Clear Overrides"))) {
            style_action(app, paragraph, "clearOverride", 0, false);
        }
        pc_json_value_fini(&result);
        if (first) {
            nk_widget_disable_end(ctx);
        }
    }
    nk_end(ctx);
    if (nk_window_is_closed(ctx, title)) {
        ui->type_panels.styles = false;
        nk_window_show(ctx, title, NK_SHOWN);
    }
}
static void options_window(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_type_panels *panels = &ui->type_panels;
    if (!panels->has_options) {
        pc_ui_shell_type_panels_runtime(app)->shown[1] = false;
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_type_panels_runtime *runtime = pc_ui_shell_type_panels_runtime(app);
    bool first = !runtime->shown[1];
    runtime->shown[1] = true;
    const char *title = "Type Style Options";
    if (nk_begin(ctx, title, nk_rect(460, 100, 420, 620),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                     NK_WINDOW_CLOSABLE)) {
        if (first) {
            nk_widget_disable_begin(ctx);
        }
        pc_json_value result = PC_JSON_VALUE_INIT;
        yyjson_mut_val *items = styles(app, panels->options_paragraph, &result), *selected = NULL;
        size_t i, n;
        yyjson_mut_val *item;
        yyjson_mut_arr_foreach(items, i, n, item)
        {
            if (yyjson_mut_get_uint(yyjson_mut_obj_get(item, "id")) == panels->options_id) {
                selected = item;
                break;
            }
        }
        nk_layout_row_dynamic(ctx, 24, 1);
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        pc_doc_id document = state ? pc_doc_arc_get(state->doc)->id : 0;
        if (!runtime->options_document) {
            runtime->options_document = document;
        }
        if (runtime->options_document != document) {
            nk_label(ctx, pc_i18n_t("The style document changed"), NK_TEXT_LEFT);
        } else if (!selected || (!panels->options_paragraph && !panels->options_id)) {
            nk_label(ctx, pc_i18n_t("Select an editable style"), NK_TEXT_LEFT);
        } else {
            const char *name = yyjson_mut_get_str(yyjson_mut_obj_get(selected, "name"));
            if (!runtime->name[0] || runtime->options_id != panels->options_id ||
                runtime->options_paragraph != panels->options_paragraph) {
                (void)snprintf(runtime->name, sizeof runtime->name, "%s", name ? name : "");
                runtime->options_id = panels->options_id;
                runtime->options_paragraph = panels->options_paragraph;
            }
            nk_layout_row_dynamic(ctx, 24, 2);
            (void)nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, runtime->name, sizeof runtime->name,
                                                 nk_filter_default);
            if (nk_button_label(ctx, pc_i18n_t("Rename"))) {
                yyjson_mut_val *p;
                yyjson_mut_doc *doc = request(app, &p, false);
                if (p && yyjson_mut_obj_add_uint(doc, p, "id", panels->options_id) &&
                    yyjson_mut_obj_add_strcpy(doc, p, "name", runtime->name)) {
                    pc_ui_shell_queue(app,
                                      panels->options_paragraph ? "type.paragraphStyle.rename"
                                                                : "type.characterStyle.rename",
                                      p);
                }
                yyjson_mut_doc_free(doc);
            }
            pc_buf json = PC_BUF_INIT;
            yyjson_doc *parsed = NULL;
            pc_char_style character;
            pc_char_style_default(&character);
            pc_paragraph_style paragraph = pc_paragraph_style_default();
            yyjson_mut_val *resolved = yyjson_mut_obj_get(selected, "resolved");
            pc_status status = pc_json_write(resolved, 0, &json, NULL);
            if (status == PC_OK) {
                status = pc_json_parse(pc_buf_cstr(&json), json.len, &parsed, NULL);
            }
            if (status == PC_OK) {
                if (panels->options_paragraph) {
                    status = pc_paragraph_style_from_json(yyjson_doc_get_root(parsed), &paragraph, NULL);
                } else {
                    pc_char_style_fini(&character);
                    status = pc_char_style_from_json(yyjson_doc_get_root(parsed), &character, NULL);
                }
            }
            if (status == PC_OK) {
                pc_ui_shell_type_style_editor(app, panels->options_paragraph, panels->options_id, &character,
                                              &paragraph);
            }
            pc_char_style_fini(&character);
            pc_buf_fini(&json);
            yyjson_doc_free(parsed);
        }
        pc_json_value_fini(&result);
        if (first) {
            nk_widget_disable_end(ctx);
        }
    }
    nk_end(ctx);
    if (nk_window_is_closed(ctx, title)) {
        panels->has_options = false;
    }
}
static pc_status glyph_image(pc_ui_shell_app *app, const char *family, const char *face, uint32_t scalar,
                             uint16_t side, struct nk_image *out)
{
    pc_buf signature = PC_BUF_INIT;
    pc_status status = pc_buf_append(&signature, family, strlen(family) + 1, NULL);
    if (status == PC_OK) {
        status = pc_buf_append(&signature, face, strlen(face) + 1, NULL);
    }
    if (status == PC_OK) {
        status = pc_buf_append(&signature, &scalar, sizeof scalar, NULL);
    }
    uint8_t key[32];
    if (status == PC_OK) {
        pc_blake3(signature.data, signature.len, key);
    }
    pc_buf_fini(&signature);
    if (status != PC_OK) {
        return status;
    }
    status = pc_ui_shell_bitmap(app, key, side, side, NULL, out, NULL);
    if (status == PC_OK) {
        return status;
    }
    pc_text_layer text;
    pc_text_layer_default(&text);
    pc_buf utf8 = PC_BUF_INIT;
    status = pc_utf8_put(&utf8, scalar, NULL);
    if (status == PC_OK) {
        status = pc_str_set(&text.text, pc_buf_cstr(&utf8), utf8.len, NULL);
    }
    if (status == PC_OK) {
        status = pc_str_set_c(&text.font_family, family, NULL);
    }
    text.size_pt = (float)side * .72f;
    pc_text_run run = {0};
    pc_char_style_default(&run.style);
    if (status == PC_OK) {
        status = pc_str_set_c(&run.style.font_family, family, NULL);
    }
    if (status == PC_OK) {
        status = pc_str_set_c(&run.style.font_style, face, NULL);
    }
    run.style.size_pt = text.size_pt;
    run.len = text.text.len;
    pc_text_engine *engine = NULL;
    pc_text_layout layout = {0};
    uint8_t *alpha = NULL, *rgba = NULL;
    uint32_t size = 0;
    if (status == PC_OK) {
        status = pc_text_engine_shared_lock(&engine, NULL);
    }
    if (status == PC_OK) {
        text.runs = &run;
        text.nruns = 1;
        status = pc_text_engine_layout(engine, &text, 72, &layout, NULL);
        text.runs = NULL;
        text.nruns = 0;
        pc_text_engine_shared_unlock();
    }
    if (status == PC_OK) {
        status = pc_glyph_preview_from_layout(&layout, side, &size, &alpha, NULL);
    }
    if (status == PC_OK) {
        rgba = pc_malloc_array((size_t)size * size, 4);
        if (!rgba) {
            status = PC_ERR_NOMEM;
        } else {
            for (size_t i = 0; i < (size_t)size * size; i++) {
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 255;
                rgba[i * 4 + 3] = alpha[i];
            }
            status = pc_ui_shell_bitmap(app, key, (uint16_t)size, (uint16_t)size, rgba, out, NULL);
        }
    }
    free(rgba);
    free(alpha);
    pc_text_layout_fini(&layout);
    pc_text_layer_fini(&text);
    pc_char_style_fini(&run.style);
    pc_buf_fini(&utf8);
    return status;
}
static void glyph_insert(pc_ui_shell_app *app, uint32_t scalar)
{
    pc_buf text = PC_BUF_INIT;
    if (pc_utf8_put(&text, scalar, NULL) != PC_OK) {
        pc_buf_fini(&text);
        return;
    }
    yyjson_mut_val *p;
    yyjson_mut_doc *doc = request(app, &p, false);
    if (p && yyjson_mut_obj_add_strcpy(doc, p, "text", pc_buf_cstr(&text))) {
        pc_ui_shell_queue(app, "ui.type.insertGlyph", p);
    }
    yyjson_mut_doc_free(doc);
    pc_buf_fini(&text);
}
static bool charmap(pc_ui_shell_app *app, const char *family, const char *face)
{
    pc_ui_type_panels_runtime *r = pc_ui_shell_type_panels_runtime(app);
    if (r->charmap && pc_str_eq_c(&r->charmap_family, family) && pc_str_eq_c(&r->charmap_style, face)) {
        return true;
    }
    pc_char_style style;
    pc_char_style_default(&style);
    uint32_t *points = NULL;
    size_t n = 0;
    pc_status status = pc_str_set_c(&style.font_family, family, NULL);
    if (status == PC_OK) {
        status = pc_str_set_c(&style.font_style, face, NULL);
    }
    pc_text_engine *engine = NULL;
    if (status == PC_OK) {
        status = pc_text_engine_shared_lock(&engine, NULL);
    }
    if (status == PC_OK) {
        status = pc_fontdb_charmap_for_style(pc_text_engine_fonts(engine), &style, &points, &n, NULL);
        pc_text_engine_shared_unlock();
    }
    pc_char_style_fini(&style);
    if (status != PC_OK) {
        free(points);
        return false;
    }
    pc_str next_family = PC_STR_INIT, next_face = PC_STR_INIT;
    if (pc_str_set_c(&next_family, family, NULL) != PC_OK || pc_str_set_c(&next_face, face, NULL) != PC_OK) {
        pc_str_fini(&next_family);
        pc_str_fini(&next_face);
        free(points);
        return false;
    }
    free(r->charmap);
    free(r->filtered);
    r->filtered = NULL;
    r->nfiltered = 0;
    pc_str_fini(&r->charmap_family);
    pc_str_fini(&r->charmap_style);
    pc_str_fini(&r->category);
    r->charmap = points;
    r->ncharmap = n;
    r->charmap_family = next_family;
    r->charmap_style = next_face;
    return true;
}
static void glyphs_window(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_type_panels *panels = &ui->type_panels;
    if (!panels->glyphs) {
        pc_ui_shell_type_panels_runtime(app)->shown[2] = false;
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_type_panels_runtime *runtime = pc_ui_shell_type_panels_runtime(app);
    bool first = !runtime->shown[2];
    runtime->shown[2] = true;
    const char *title = "Glyphs";
    if (nk_begin(ctx, title, nk_rect(120, 140, 440, 500),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE |
                     NK_WINDOW_CLOSABLE)) {
        if (first) {
            nk_widget_disable_begin(ctx);
        }
        const struct nk_window *glyph_window = ctx->current;
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        const pc_layer *layer =
            state ? pc_document_layer(pc_doc_arc_get(state->doc),
                                      ui->has_text_edit ? ui->text_edit.layer : state->active_layer)
                  : NULL;
        const pc_text_layer *target =
            layer && layer->content.kind == PC_LAYER_TEXT ? &layer->content.u.text : NULL;
        const char *family = panels->glyph_family.len ? pc_str_c(&panels->glyph_family)
                             : target                 ? pc_str_c(&target->font_family)
                                                      : pc_str_c(&ui->tool_options.type_font);
        const char *face = panels->glyph_style.len   ? pc_str_c(&panels->glyph_style)
                           : target && target->nruns ? pc_str_c(&target->runs[0].style.font_style)
                                                     : pc_str_c(&ui->tool_options.type_style);
        pc_str owned_family = PC_STR_INIT, owned_face = PC_STR_INIT;
        bool owned = pc_str_set_c(&owned_family, family, NULL) == PC_OK &&
                     pc_str_set_c(&owned_face, face, NULL) == PC_OK;
        family = owned ? pc_str_c(&owned_family) : "Inter";
        face = owned ? pc_str_c(&owned_face) : "Regular";
        if (!runtime->glyph_family[0]) {
            (void)snprintf(runtime->glyph_family, sizeof runtime->glyph_family, "%s", family);
        }
        if (!runtime->glyph_style[0]) {
            (void)snprintf(runtime->glyph_style, sizeof runtime->glyph_style, "%s", face);
        }
        nk_layout_row_dynamic(ctx, 24, 2);
        (void)nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, runtime->glyph_family,
                                             sizeof runtime->glyph_family, nk_filter_default);
        (void)nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, runtime->glyph_style,
                                             sizeof runtime->glyph_style, nk_filter_default);
        if (nk_button_label(ctx, pc_i18n_t("Choose Font"))) {
            (void)pc_str_set_c(&panels->glyph_family, runtime->glyph_family, NULL);
            (void)pc_str_set_c(&panels->glyph_style, runtime->glyph_style, NULL);
        }
        if (nk_button_label(ctx, pc_i18n_t("Follow Type Selection"))) {
            pc_str_fini(&panels->glyph_family);
            pc_str_fini(&panels->glyph_style);
            runtime->glyph_family[0] = runtime->glyph_style[0] = 0;
        }
        int category = 0;
        for (int i = 0; i < 14; i++) {
            if (pc_str_eq_c(&panels->glyph_category, PC_GLYPH_CATEGORIES[i])) {
                category = i;
            }
        }
        int next = nk_combo(ctx, PC_GLYPH_CATEGORIES, 14, category, 24, nk_vec2(220, 300));
        if (next >= 0 && next < 14 && next != category) {
            (void)pc_str_set_c(&panels->glyph_category, PC_GLYPH_CATEGORIES[next], NULL);
        }
        float side = panels->glyph_size > 0 ? panels->glyph_size : 40;
        nk_property_float(ctx, "Cell Size", 16, &side, 72, 4, .2f);
        panels->glyph_size = side;
        if (charmap(app, family, face)) {
            const char *filter =
                panels->glyph_category.len ? pc_str_c(&panels->glyph_category) : PC_GLYPH_CATEGORIES[0];
            if (!runtime->filtered || !pc_str_eq_c(&runtime->category, filter)) {
                uint32_t *points = pc_malloc_array(runtime->ncharmap ? runtime->ncharmap : 1, sizeof *points);
                if (points) {
                    size_t count = 0;
                    for (size_t i = 0; i < runtime->ncharmap; i++) {
                        if (pc_glyph_in_category(runtime->charmap[i], filter)) {
                            points[count++] = runtime->charmap[i];
                        }
                    }
                    free(runtime->filtered);
                    runtime->filtered = points;
                    runtime->nfiltered = count;
                    (void)pc_str_set_c(&runtime->category, filter, NULL);
                }
            }
            nk_layout_row_dynamic(ctx, 26, 8);
            for (size_t recent = 0; recent < 8; recent++) {
                if (recent >= panels->glyph_recent.n) {
                    nk_spacing(ctx, 1);
                    continue;
                }
                const char *value = panels->glyph_recent.v[recent];
                size_t offset = 0;
                uint32_t scalar = 0;
                if (pc_utf8_next((const uint8_t *)value, strlen(value), &offset, &scalar) &&
                    nk_button_label(ctx, value)) {
                    glyph_insert(app, scalar);
                }
            }
            nk_layout_row_dynamic(ctx, 260, 1);
            struct nk_list_view view;
            float available = nk_window_get_content_region(ctx).w - 24;
            int columns = (int)fmaxf(1, fminf(32, floorf(available / side)));
            if (nk_list_view_begin(ctx, &view, "glyph-grid", NK_WINDOW_BORDER, (int)side + 6,
                                   (int)((runtime->nfiltered + (size_t)columns - 1) / (size_t)columns))) {
                for (int row = view.begin; row < view.end; row++) {
                    if (row < 0) {
                        continue;
                    }
                    nk_layout_row_dynamic(ctx, side, columns);
                    for (int col = 0; col < columns; col++) {
                        size_t index = (size_t)row * (size_t)columns + (size_t)col;
                        if (!runtime->filtered || index >= runtime->nfiltered) {
                            nk_spacing(ctx, 1);
                            continue;
                        }
                        uint32_t scalar = runtime->filtered[index];
                        struct nk_image image;
                        uint16_t pixels = (uint16_t)fminf(512, ceilf(side * pc_ui_shell_app_ui_scale(app)));
                        char id[64];
                        (void)snprintf(id, sizeof id, "type.glyph.%u", scalar);
                        struct nk_rect rect = nk_widget_bounds(ctx);
                        pc_ui_shell_widget_mapping(app, id, rect);
                        bool double_click = nk_input_is_mouse_pressed(&ctx->input, NK_BUTTON_DOUBLE) &&
                                            nk_input_is_mouse_hovering_rect(&ctx->input, rect) && !first &&
                                            ctx->active == glyph_window;
                        bool clicked = false;
                        if (glyph_image(app, family, face, scalar, pixels, &image) == PC_OK) {
                            clicked = nk_button_image(ctx, image);
                        } else {
                            char label[24];
                            (void)snprintf(label, sizeof label, "U+%04X", scalar);
                            clicked = nk_button_label(ctx, label);
                        }
                        if (double_click) {
                            glyph_insert(app, scalar);
                        } else if (clicked) {
                            pc_buf selected = PC_BUF_INIT;
                            if (pc_utf8_put(&selected, scalar, NULL) == PC_OK &&
                                pc_str_set(&panels->glyph_selected, pc_buf_cstr(&selected), selected.len,
                                           NULL) == PC_OK) {
                                panels->has_glyph_selected = true;
                            }
                            pc_buf_fini(&selected);
                        }
                    }
                }
                nk_list_view_end(&view);
            }
        }
        pc_str_fini(&owned_family);
        pc_str_fini(&owned_face);
        if (first) {
            nk_widget_disable_end(ctx);
        }
    }
    nk_end(ctx);
    if (nk_window_is_closed(ctx, title)) {
        panels->glyphs = false;
    }
}
static void spelling_window(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    pc_ui_type_panels *panels = &ui->type_panels;
    if (!panels->has_spell) {
        pc_ui_shell_type_panels_runtime(app)->shown[3] = false;
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_type_panels_runtime *runtime = pc_ui_shell_type_panels_runtime(app);
    bool first = !runtime->shown[3];
    runtime->shown[3] = true;
    const char *title = "Check Spelling";
    if (nk_begin(ctx, title, nk_rect(420, 170, 440, 390),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE)) {
        if (first) {
            nk_widget_disable_begin(ctx);
        }
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        pc_doc_id document = state ? pc_doc_arc_get(state->doc)->id : 0;
        if (!runtime->spell_document && document) {
            runtime->spell_document = document;
            (void)spell_refresh(app, NULL);
        }
        bool wrong_document = !document || runtime->spell_document != document;
        if (wrong_document) {
            nk_layout_row_dynamic(ctx, 26, 1);
            nk_label(ctx, pc_i18n_t("The spelling document changed"), NK_TEXT_LEFT);
            if (nk_button_label(ctx, pc_i18n_t("Done"))) {
                panels->has_spell = false;
            }
        } else {
            yyjson_mut_val *items = pc_json_value_root(&panels->spell.items),
                           *item = yyjson_mut_arr_get(items, panels->spell.index);
            const char *word = yyjson_mut_get_str(yyjson_mut_obj_get(item, "word"));
            nk_layout_row_dynamic(ctx, 26, 1);
            nk_label(ctx, word ? word : "Spelling check complete", NK_TEXT_LEFT);
            (void)nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, runtime->replacement,
                                                 sizeof runtime->replacement, nk_filter_default);
            (void)pc_str_set_c(&panels->spell.change_to, runtime->replacement, NULL);
            yyjson_mut_val *suggestions = yyjson_mut_obj_get(item, "suggestions");
            size_t i, n;
            yyjson_mut_val *suggestion;
            nk_layout_row_dynamic(ctx, 110, 1);
            if (nk_group_begin(ctx, "spelling-suggestions", NK_WINDOW_BORDER)) {
                yyjson_mut_arr_foreach(suggestions, i, n, suggestion)
                {
                    const char *text = yyjson_mut_get_str(suggestion);
                    if (!text) {
                        continue;
                    }
                    nk_layout_row_dynamic(ctx, 24, 1);
                    if (nk_button_label(ctx, text)) {
                        (void)snprintf(runtime->replacement, sizeof runtime->replacement, "%s", text);
                    }
                }
                nk_group_end(ctx);
            }
            const char *action = NULL;
            bool refresh = false;
            nk_layout_row_dynamic(ctx, 24, 1);
            nk_bool all = panels->spell.all_layers;
            nk_checkbox_label(ctx, pc_i18n_t("Check All Type Layers"), &all);
            if ((all != 0) != panels->spell.all_layers) {
                panels->spell.all_layers = all != 0;
                panels->spell.index = 0;
                refresh = true;
            }
            nk_layout_row_dynamic(ctx, 26, 3);
            if (nk_button_label(ctx, pc_i18n_t("Ignore")) && word) {
                panels->spell.index++;
                if (panels->spell.index >= yyjson_mut_arr_size(items)) {
                    panels->spell.index = yyjson_mut_arr_size(items);
                }
            }
            if (nk_button_label(ctx, pc_i18n_t("Ignore All")) && word) {
                if (panels->spell.ignore.n < 4096) {
                    (void)pc_strlist_push(&panels->spell.ignore, word, strlen(word), NULL);
                    refresh = true;
                } else {
                    (void)pc_str_set_c(&panels->spell.message, "The ignore list is full", NULL);
                }
            }
            if (nk_button_label(ctx, pc_i18n_t("Add")) && word) {
                action = "addToDictionary";
            }
            if (nk_button_label(ctx, pc_i18n_t("Change")) && word) {
                action = "change";
            }
            if (nk_button_label(ctx, pc_i18n_t("Change All")) && word) {
                action = "changeAll";
            }
            if (nk_button_label(ctx, pc_i18n_t("Done"))) {
                panels->has_spell = false;
            }
            if (action) {
                yyjson_mut_val *p;
                yyjson_mut_doc *doc = request(app, &p, false);
                bool ok = p && yyjson_mut_obj_add_str(doc, p, "action", action) &&
                          yyjson_mut_obj_add_strcpy(doc, p, "word", word) &&
                          yyjson_mut_obj_add_bool(doc, p, "allLayers", panels->spell.all_layers);
                if (strcmp(action, "addToDictionary")) {
                    ok = ok && yyjson_mut_obj_add_strcpy(doc, p, "replace", runtime->replacement);
                }
                if (!strcmp(action, "change")) {
                    const char *keys[] = {"layer", "start", "end"};
                    for (size_t j = 0; j < 3 && ok; j++) {
                        ok = yyjson_mut_obj_add_val(
                            doc, p, keys[j], yyjson_mut_val_mut_copy(doc, yyjson_mut_obj_get(item, keys[j])));
                    }
                }
                pc_json_value out = PC_JSON_VALUE_INIT;
                pc_error error = {0};
                pc_status status = ok ? execute(app, "edit.checkSpelling", p, &out, &error)
                                      : pc_fail(&error, PC_ERR_NOMEM, "spelling request");
                if (status != PC_OK) {
                    (void)pc_str_set_c(&panels->spell.message, error.msg, NULL);
                } else {
                    refresh = true;
                    const pc_engine_doc_state *changed =
                        pc_engine_session_active(pc_ui_shell_app_session(app));
                    if (ui->has_text_edit && changed &&
                        pc_doc_arc_get(changed->doc)->id == ui->text_edit.document) {
                        ui->text_edit.revision = changed->revision;
                    }
                    pc_ui_shell_app_changed(app);
                }
                pc_json_value_fini(&out);
                yyjson_mut_doc_free(doc);
            }
            if (refresh) {
                pc_error error = {0};
                if (spell_refresh(app, &error) != PC_OK) {
                    (void)pc_str_set_c(&panels->spell.message, error.msg, NULL);
                }
            }
            nk_layout_row_dynamic(ctx, 30, 1);
            nk_label_wrap(ctx, pc_str_c(&panels->spell.message));
        }
        if (first) {
            nk_widget_disable_end(ctx);
        }
    }
    nk_end(ctx);
    if (nk_window_is_closed(ctx, title)) {
        panels->has_spell = false;
    }
}
void pc_ui_shell_type_panels_draw(pc_ui_shell_app *app)
{
    styles_window(app);
    options_window(app);
    glyphs_window(app);
    spelling_window(app);
}

pc_status pc_ui_shell_type_style_color(pc_ui_shell_app *app, uint32_t id, pc_json_value *out, pc_error *err)
{
    if (!id || pc_ui_shell_app_state(app)->n_dialogs) {
        return pc_fail(err, PC_ERR_INVALID, "select a character style and close the open dialog");
    }
    pc_json_value result = PC_JSON_VALUE_INIT;
    yyjson_mut_val *items = styles(app, false, &result), *color_value = NULL;
    size_t i, n;
    yyjson_mut_val *item;
    yyjson_mut_arr_foreach(items, i, n, item)
    {
        if (yyjson_mut_get_uint(yyjson_mut_obj_get(item, "id")) == id) {
            color_value = yyjson_mut_obj_get(yyjson_mut_obj_get(item, "resolved"), "color");
            break;
        }
    }
    pc_buf json = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    pc_color color = PC_COLOR_BLACK;
    pc_status status = color_value ? pc_json_write(color_value, 0, &json, err)
                                   : pc_fail(err, PC_ERR_NOT_FOUND, "no such character style");
    if (status == PC_OK) {
        status = pc_json_parse(pc_buf_cstr(&json), json.len, &parsed, err);
    }
    if (status == PC_OK) {
        status = pc_color_from_json(yyjson_doc_get_root(parsed), &color, err);
    }
    yyjson_doc_free(parsed);
    pc_buf_fini(&json);
    pc_json_value_fini(&result);
    if (status == PC_OK) {
        float rgba[4] = {0, 0, 0, 1};
        pc_color_to_rgb(&color, rgba);
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
        if (!p || !yyjson_mut_obj_add_uint(doc, p, "id", id)) {
            status = pc_fail(err, PC_ERR_NOMEM, "character style color");
        } else {
            status = pc_ui_shell_dialog_color_command(app, "Color Picker (Character Style)", rgba,
                                                      "type.characterStyle.set", p, "attrs.color", out, err);
        }
        yyjson_mut_doc_free(doc);
    }
    return status;
}
