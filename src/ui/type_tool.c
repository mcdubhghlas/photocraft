#include "type_tool.h"
#include "engine/type_cmds.h"
#include "engine/prefs.h"
#include "base/utf8.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TEXT_LIMIT ((size_t)1 << 20)
static pc_status execute(pc_engine_session *session, const char *id, yyjson_mut_val *params,
                         pc_json_value *out, pc_error *err)
{
    pc_buf json = PC_BUF_INIT;
    pc_status status = pc_json_write(params, 0, &json, err);
    if (status == PC_OK) {
        status = pc_engine_session_execute_json(session, id, pc_buf_cstr(&json), out, err);
    }
    pc_buf_fini(&json);
    return status;
}
static size_t characters(const pc_str *text)
{
    return pc_text_char_index(pc_str_c(text), text->len, text->len);
}
static void clear_edit(pc_ui_state *ui)
{
    pc_ui_text_edit_fini(&ui->text_edit);
    ui->has_text_edit = false;
}
static pc_status current(pc_engine_session *session, pc_ui_state *ui, const pc_engine_doc_state **state,
                         const pc_text_layer **text, pc_error *err)
{
    if (!ui->has_text_edit) {
        return PC_ERR_NOT_FOUND;
    }
    *state = pc_engine_session_active(session);
    if (!*state) {
        return pc_fail(err, PC_ERR_INVALID, "the text document was closed");
    }
    pc_ui_text_edit *edit = &ui->text_edit;
    const pc_document *doc = pc_doc_arc_get((*state)->doc);
    const pc_layer *layer = pc_document_layer(doc, edit->layer);
    if ((edit->document && edit->document != doc->id) || !layer || layer->content.kind != PC_LAYER_TEXT ||
        !(*state)->has_active_layer || (*state)->active_layer != edit->layer) {
        return pc_fail(err, PC_ERR_INVALID, "the text document or target changed");
    }
    if (edit->revision && (*state)->revision != edit->revision &&
        (!(*state)->coalesce || strcmp((*state)->coalesce, pc_str_c(&edit->session)))) {
        return pc_fail(err, PC_ERR_INVALID, "the document changed outside the text editing session");
    }
    edit->document = doc->id;
    edit->revision = (*state)->revision;
    *text = &layer->content.u.text;
    size_t n = characters(&(*text)->text);
    if (edit->caret > n) {
        edit->caret = n;
    }
    if (edit->anchor > n) {
        edit->anchor = n;
    }
    return PC_OK;
}
void pc_ui_type_layout_fini(pc_ui_type_layout *cache)
{
    if (cache) {
        pc_doc_arc_release(cache->document);
        pc_text_layout_fini(&cache->layout);
        memset(cache, 0, sizeof *cache);
    }
}
pc_status pc_ui_type_layout_get(pc_ui_type_layout *cache, const pc_engine_doc_state *state, pc_layer_id id,
                                pc_error *err)
{
    if (!cache || !state) {
        return pc_fail(err, PC_ERR_INVALID, "missing type layout context");
    }
    if (cache->document == state->doc && cache->layer == id && cache->revision == state->revision) {
        return PC_OK;
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const pc_layer *layer = pc_document_layer(doc, id);
    if (!layer || layer->content.kind != PC_LAYER_TEXT) {
        return pc_fail(err, PC_ERR_INVALID, "the target is not a type layer");
    }
    pc_text_engine *engine = NULL;
    pc_status status = pc_text_engine_shared_lock(&engine, err);
    if (status != PC_OK) {
        return status;
    }
    pc_text_layout layout = {0};
    status = pc_text_engine_layout(engine, &layer->content.u.text, doc->resolution_dpi, &layout, err);
    pc_text_engine_shared_unlock();
    if (status != PC_OK) {
        pc_text_layout_fini(&layout);
        return status;
    }
    pc_ui_type_layout_fini(cache);
    cache->document = pc_doc_arc_retain(state->doc);
    cache->layer = id;
    cache->revision = state->revision;
    cache->layout = layout;
    return PC_OK;
}
static pc_status session_key(pc_ui_state *ui, pc_str *out, pc_error *err)
{
    uint64_t id = pc_ui_state_alloc_id(ui);
    if (!id) {
        return pc_fail(err, PC_ERR_LIMIT, "text editing session id exhausted");
    }
    char key[64];
    int n = snprintf(key, sizeof key, "type-%llu", (unsigned long long)id);
    return n > 0 && (size_t)n < sizeof key ? pc_str_set(out, key, (size_t)n, err)
                                           : pc_fail(err, PC_ERR_LIMIT, "text editing key");
}
static bool add_key(yyjson_mut_doc *doc, yyjson_mut_val *params, const pc_ui_text_edit *edit)
{
    return yyjson_mut_obj_add_uint(doc, params, "layer", edit->layer) &&
           yyjson_mut_obj_add_strcpy(doc, params, "coalesce", pc_str_c(&edit->session));
}
pc_status pc_ui_type_begin(pc_engine_session *session, pc_ui_state *ui, pc_layer_id id, bool all,
                           pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const pc_layer *layer = pc_document_layer(doc, id);
    if (!layer || layer->content.kind != PC_LAYER_TEXT) {
        return pc_fail(err, PC_ERR_INVALID, "the target is not a type layer");
    }
    if (ui->has_text_edit && ui->text_edit.document == doc->id && ui->text_edit.layer == id) {
        size_t n = characters(&layer->content.u.text.text);
        ui->text_edit.caret = n;
        if (all) {
            ui->text_edit.anchor = 0;
        }
        return PC_OK;
    }
    if (ui->has_text_edit) {
        pc_json_value result = PC_JSON_VALUE_INIT;
        pc_status status = pc_ui_type_commit(session, ui, 0, &result, err);
        pc_json_value_fini(&result);
        if (status != PC_OK) {
            return status;
        }
    }
    pc_ui_text_edit edit = {.layer = id};
    pc_status status = session_key(ui, &edit.session, err);
    if (status != PC_OK) {
        return status;
    }
    yyjson_mut_doc *params = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = params ? yyjson_mut_obj(params) : NULL;
    if (!root || !add_key(params, root, &edit)) {
        yyjson_mut_doc_free(params);
        pc_ui_text_edit_fini(&edit);
        return pc_fail(err, PC_ERR_NOMEM, "begin type editing");
    }
    pc_json_value result = PC_JSON_VALUE_INIT;
    status = execute(session, "type.edit", root, &result, err);
    pc_json_value_fini(&result);
    yyjson_mut_doc_free(params);
    if (status != PC_OK) {
        pc_ui_text_edit_fini(&edit);
        return status;
    }
    status = pc_engine_session_select_layer(session, id, err);
    if (status != PC_OK) {
        pc_ui_text_edit_fini(&edit);
        return status;
    }
    state = pc_engine_session_active(session);
    doc = pc_doc_arc_get(state->doc);
    layer = pc_document_layer(doc, id);
    edit.document = doc->id;
    edit.revision = state->revision;
    edit.caret = characters(&layer->content.u.text.text);
    edit.anchor = all ? 0 : edit.caret;
    clear_edit(ui);
    ui->text_edit = edit;
    ui->has_text_edit = true;
    ui->tool = layer->content.u.text.orientation == PC_ORIENT_VERTICAL ? PC_TOOL_VERTICAL_TYPE : PC_TOOL_TYPE;
    ui->mask_target = ui->vector_mask_target = false;
    return PC_OK;
}
pc_status pc_ui_type_create(pc_engine_session *session, pc_ui_state *ui, const float start[2],
                            const float end[2], float zoom, pc_json_value *out, pc_error *err)
{
    if (!session || !ui || !start || !end || !out || !pc_ui_tool_is_type(ui->tool) || !isfinite(zoom) ||
        zoom <= 0 || !isfinite(start[0]) || !isfinite(start[1]) || !isfinite(end[0]) || !isfinite(end[1])) {
        return pc_fail(err, PC_ERR_INVALID, "invalid Type tool gesture");
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    if (ui->n_dialogs || ui->has_text_edit) {
        return pc_fail(err, PC_ERR_INVALID, "finish the current editing session first");
    }
    pc_ui_text_edit edit = {.created = true, .mask = pc_ui_tool_is_type_mask(ui->tool)};
    pc_status status = session_key(ui, &edit.session, err);
    if (status != PC_OK) {
        return status;
    }
    bool placeholder = !edit.mask;
    pc_json_value pref = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(session, "type.fillNewTypeLayersWithPlaceholder", &pref, NULL) == PC_OK) {
        placeholder = placeholder && yyjson_mut_get_bool(pc_json_value_root(&pref));
    }
    pc_json_value_fini(&pref);
    const char *text = placeholder ? "Lorem Ipsum" : "";
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = doc ? yyjson_mut_obj(doc) : NULL;
    const pc_ui_tool_options *options = &ui->tool_options;
    bool ok = params && yyjson_mut_obj_add_str(doc, params, "text", text) &&
              yyjson_mut_obj_add_str(doc, params, "orientation",
                                     pc_ui_tool_is_vertical_type(ui->tool) ? "vertical" : "horizontal") &&
              yyjson_mut_obj_add_strcpy(doc, params, "font", pc_str_c(&options->type_font)) &&
              yyjson_mut_obj_add_strcpy(doc, params, "fontStyle", pc_str_c(&options->type_style)) &&
              yyjson_mut_obj_add_real(doc, params, "size", options->type_size) &&
              yyjson_mut_obj_add_strcpy(doc, params, "align", pc_str_c(&options->type_align)) &&
              yyjson_mut_obj_add_strcpy(doc, params, "coalesce", pc_str_c(&edit.session));
    yyjson_mut_val *color = doc ? yyjson_mut_arr(doc) : NULL;
    const float *fg = pc_engine_session_tools(session)->foreground;
    for (size_t i = 0; ok && i < 4; i++) {
        ok = yyjson_mut_arr_add_real(doc, color, fg[i]);
    }
    ok = ok && yyjson_mut_obj_add_val(doc, params, "color", color);
    double width = fabs((double)end[0] - start[0]), height = fabs((double)end[1] - start[1]);
    if (width >= 4 / zoom && height >= 4 / zoom) {
        yyjson_mut_val *box = doc ? yyjson_mut_arr(doc) : NULL;
        ok = ok && box && yyjson_mut_arr_add_real(doc, box, round(fmin(start[0], end[0]))) &&
             yyjson_mut_arr_add_real(doc, box, round(fmin(start[1], end[1]))) &&
             yyjson_mut_arr_add_real(doc, box, round(width)) &&
             yyjson_mut_arr_add_real(doc, box, round(height)) &&
             yyjson_mut_obj_add_val(doc, params, "box", box);
    } else {
        ok = ok && yyjson_mut_obj_add_real(doc, params, "x", round(start[0])) &&
             yyjson_mut_obj_add_real(doc, params, "y", round(start[1]));
    }
    if (!ok) {
        yyjson_mut_doc_free(doc);
        pc_ui_text_edit_fini(&edit);
        return pc_fail(err, PC_ERR_NOMEM, "Type tool creation");
    }
    status = execute(session, "type.create", params, out, err);
    yyjson_mut_doc_free(doc);
    if (status != PC_OK) {
        pc_ui_text_edit_fini(&edit);
        return status;
    }
    state = pc_engine_session_active(session);
    edit.layer = state->active_layer;
    edit.document = pc_doc_arc_get(state->doc)->id;
    edit.revision = state->revision;
    edit.caret = strlen(text);
    edit.anchor = 0;
    ui->has_text_edit = true;
    ui->text_edit = edit;
    ui->mask_target = ui->vector_mask_target = false;
    for (unsigned step = 0; step < 2 && status == PC_OK; step++) {
        bool hide = step == 1;
        if ((hide && !edit.mask) || (!hide && !strcmp(pc_str_c(&options->type_aa), "sharp"))) {
            continue;
        }
        yyjson_mut_doc *patch = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = patch ? yyjson_mut_obj(patch) : NULL;
        if (p && add_key(patch, p, &edit)) {
            bool added = hide ? yyjson_mut_obj_add_bool(patch, p, "visible", false)
                              : yyjson_mut_obj_add_strcpy(patch, p, "antialias", pc_str_c(&options->type_aa));
            pc_json_value result = PC_JSON_VALUE_INIT;
            status = added ? execute(session, hide ? "layer.setProps" : "type.edit", p, &result, err)
                           : pc_fail(err, PC_ERR_NOMEM, "Type tool settings");
            pc_json_value_fini(&result);
            ui->text_edit.revision = pc_engine_session_active(session)->revision;
        } else {
            status = pc_fail(err, PC_ERR_NOMEM, "Type tool settings");
        }
        yyjson_mut_doc_free(patch);
    }
    return status;
}
static pc_status replace(pc_engine_session *session, pc_ui_state *ui, size_t start, size_t end,
                         const char *text, size_t length, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *state;
    const pc_text_layer *layer;
    pc_status status = current(session, ui, &state, &layer, err);
    if (status != PC_OK) {
        return status;
    }
    if ((length && !text) || length > TEXT_LIMIT) {
        return pc_fail(err, PC_ERR_LIMIT, "typed text is too long");
    }
    size_t count = 0;
    for (size_t i = 0; i < length;) {
        uint32_t cp = 0;
        if (!pc_utf8_next((const uint8_t *)text, length, &i, &cp)) {
            return pc_fail(err, PC_ERR_INVALID, "typed text is not valid UTF-8");
        }
        count++;
    }
    size_t n = characters(&layer->text);
    if (start > n || end < start || end > n) {
        return pc_fail(err, PC_ERR_INVALID, "text range is out of bounds");
    }
    size_t bytes = pc_text_byte_index(pc_str_c(&layer->text), layer->text.len, end) -
                   pc_text_byte_index(pc_str_c(&layer->text), layer->text.len, start);
    if (layer->text.len - bytes > TEXT_LIMIT - length) {
        return pc_fail(err, PC_ERR_LIMIT, "inline text exceeds the editing budget");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = doc ? yyjson_mut_obj(doc) : NULL, *range = doc ? yyjson_mut_obj(doc) : NULL;
    if (!params || !range || !add_key(doc, params, &ui->text_edit) ||
        !yyjson_mut_obj_add_uint(doc, range, "start", start) ||
        !yyjson_mut_obj_add_uint(doc, range, "end", end) ||
        !yyjson_mut_obj_add_strncpy(doc, range, "text", text ? text : "", length) ||
        !yyjson_mut_obj_add_val(doc, params, "replace", range)) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "type replacement");
    }
    status = execute(session, "type.edit", params, out, err);
    yyjson_mut_doc_free(doc);
    if (status == PC_OK) {
        ui->text_edit.caret = ui->text_edit.anchor = start + count;
        ui->text_edit.revision = pc_engine_session_active(session)->revision;
    }
    return status;
}
pc_status pc_ui_type_insert(pc_engine_session *session, pc_ui_state *ui, const char *text, size_t length,
                            pc_json_value *out, pc_error *err)
{
    if (!ui->has_text_edit) {
        return PC_ERR_NOT_FOUND;
    }
    const pc_engine_doc_state *source;
    const pc_text_layer *current_text;
    pc_status checked = current(session, ui, &source, &current_text, err);
    if (checked != PC_OK) {
        return checked;
    }
    pc_ui_text_edit *edit = &ui->text_edit;
    size_t start = edit->caret < edit->anchor ? edit->caret : edit->anchor,
           end = edit->caret > edit->anchor ? edit->caret : edit->anchor;
    if (edit->has_preedit) {
        start = edit->preedit_start;
        end = start + edit->preedit_length;
    }
    pc_status status = replace(session, ui, start, end, text, length, out, err);
    if (status == PC_OK) {
        edit->has_preedit = false;
    }
    return status;
}
pc_status pc_ui_type_preedit(pc_engine_session *session, pc_ui_state *ui, const char *text, size_t length,
                             pc_json_value *out, pc_error *err)
{
    if (!ui->has_text_edit) {
        return PC_ERR_NOT_FOUND;
    }
    const pc_engine_doc_state *source;
    const pc_text_layer *current_text;
    pc_status checked = current(session, ui, &source, &current_text, err);
    if (checked != PC_OK) {
        return checked;
    }
    pc_ui_text_edit *edit = &ui->text_edit;
    size_t start = edit->has_preedit            ? edit->preedit_start
                   : edit->caret < edit->anchor ? edit->caret
                                                : edit->anchor;
    size_t end = edit->has_preedit            ? start + edit->preedit_length
                 : edit->caret > edit->anchor ? edit->caret
                                              : edit->anchor;
    pc_status status = replace(session, ui, start, end, text, length, out, err);
    if (status == PC_OK) {
        edit->has_preedit = length != 0;
        edit->preedit_start = start;
        edit->preedit_length = edit->caret - start;
    }
    return status;
}
pc_status pc_ui_type_cancel(pc_engine_session *session, pc_ui_state *ui, pc_error *err)
{
    if (!ui->has_text_edit) {
        return PC_OK;
    }
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    pc_ui_text_edit *edit = &ui->text_edit;
    if (state && pc_doc_arc_get(state->doc)->id == edit->document && state->coalesce &&
        !strcmp(state->coalesce, pc_str_c(&edit->session))) {
        if (!pc_engine_session_undo(session)) {
            return pc_fail(err, PC_ERR_INVALID, "could not undo the text editing session");
        }
    }
    clear_edit(ui);
    return PC_OK;
}
pc_status pc_ui_type_commit(pc_engine_session *session, pc_ui_state *ui, unsigned modifiers,
                            pc_json_value *out, pc_error *err)
{
    if (!ui->has_text_edit) {
        return PC_OK;
    }
    const pc_engine_doc_state *state;
    const pc_text_layer *layer;
    pc_status status = current(session, ui, &state, &layer, err);
    if (status != PC_OK) {
        return status;
    }
    size_t start = 0;
    bool empty = pc_utf8_trim(pc_str_c(&layer->text), layer->text.len, &start) == 0;
    if (ui->text_edit.mask && empty) {
        return pc_ui_type_cancel(session, ui, err);
    }
    if (ui->text_edit.mask || ui->text_edit.created) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *params = doc ? yyjson_mut_obj(doc) : NULL;
        if (!params || !add_key(doc, params, &ui->text_edit)) {
            yyjson_mut_doc_free(doc);
            return pc_fail(err, PC_ERR_NOMEM, "commit type editing");
        }
        const char *id = "type.edit";
        if (ui->text_edit.mask) {
            const char *operation = (modifiers & PC_UI_MOD_SHIFT)
                                        ? ((modifiers & PC_UI_MOD_ALT) ? "intersect" : "add")
                                    : (modifiers & PC_UI_MOD_ALT) ? "subtract"
                                                                  : "new";
            yyjson_mut_obj_add_bool(doc, params, "removeLayer", true);
            yyjson_mut_obj_add_str(doc, params, "operation", operation);
            id = "type.createSelection";
        } else if (empty) {
            id = "layer.delete";
        } else {
            char name[256];
            pc_engine_type_layer_name(pc_str_c(&layer->text), layer->text.len, name, sizeof name);
            yyjson_mut_obj_add_strcpy(doc, params, "name", name);
        }
        status = execute(session, id, params, out, err);
        yyjson_mut_doc_free(doc);
        if (status != PC_OK) {
            return status;
        }
    }
    clear_edit(ui);
    return PC_OK;
}
static pc_status format_key(pc_engine_session *session, pc_ui_state *ui, const pc_text_layer *text,
                            pc_ui_key key, unsigned modifiers, pc_json_value *out, pc_error *err)
{
    bool command = (modifiers & (PC_UI_MOD_COMMAND | PC_UI_MOD_CTRL)) != 0,
         shift = (modifiers & PC_UI_MOD_SHIFT) != 0, alt = (modifiers & PC_UI_MOD_ALT) != 0;
    size_t a = ui->text_edit.caret < ui->text_edit.anchor ? ui->text_edit.caret : ui->text_edit.anchor,
           b = ui->text_edit.caret > ui->text_edit.anchor ? ui->text_edit.caret : ui->text_edit.anchor;
    const char *toggle = NULL, *metric = NULL;
    double step = 0;
    bool kerning = false, hyphenate = false;
    if (command && shift && !alt) {
        toggle = key == PC_KEY_K       ? "caps"
                 : key == PC_KEY_U     ? "underline"
                 : key == PC_KEY_B     ? "fauxBold"
                 : key == PC_KEY_I     ? "fauxItalic"
                 : key == PC_KEY_SLASH ? "strikethrough"
                                       : NULL;
    }
    if (command && shift && alt && key == PC_KEY_H) {
        hyphenate = true;
    }
    if (alt && (key == PC_KEY_ARROWUP || key == PC_KEY_ARROWDOWN)) {
        metric = shift ? "baselineShiftBy" : "leadingBy";
        step = command ? 10 : 2;
        if ((shift && key == PC_KEY_ARROWDOWN) || (!shift && key == PC_KEY_ARROWUP)) {
            step = -step;
        }
    }
    if (alt && !shift && (key == PC_KEY_ARROWLEFT || key == PC_KEY_ARROWRIGHT)) {
        metric = "trackingBy";
        step = command ? 100 : 20;
        if (key == PC_KEY_ARROWLEFT) {
            step = -step;
        }
        kerning = a == b;
    }
    if (!toggle && !metric && !hyphenate) {
        return PC_ERR_NOT_FOUND;
    }
    if (a == b && text->text.len && !kerning && !hyphenate) {
        return PC_OK;
    }
    if (kerning && (a == 0 || a >= characters(&text->text))) {
        return PC_OK;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
    if (!p) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "type formatting");
    }
    bool ok = yyjson_mut_obj_add_uint(doc, p, "layer", ui->text_edit.layer);
    if (kerning) {
        yyjson_mut_val *pair = yyjson_mut_obj(doc);
        ok = ok && pair && yyjson_mut_obj_add_uint(doc, pair, "at", a) &&
             yyjson_mut_obj_add_real(doc, pair, "by", step) &&
             yyjson_mut_obj_add_val(doc, p, "kernPair", pair);
    } else {
        yyjson_mut_val *range = yyjson_mut_arr(doc);
        ok = ok && range && yyjson_mut_arr_add_uint(doc, range, a) &&
             yyjson_mut_arr_add_uint(doc, range, b) && yyjson_mut_obj_add_val(doc, p, "range", range);
        if (metric) {
            ok = ok && yyjson_mut_obj_add_real(doc, p, metric, step) &&
                 yyjson_mut_obj_add_bool(doc, p, "metricsAsShown", !strcmp(metric, "leadingBy"));
        }
        if (hyphenate) {
            size_t byte = pc_text_byte_index(pc_str_c(&text->text), text->text.len, a), offset = 0, index = 0;
            for (; index + 1 < text->nparagraphs; index++) {
                size_t len = text->paragraphs[index].len;
                if (len > SIZE_MAX - offset || byte < offset + len) {
                    break;
                }
                offset += len;
            }
            bool enabled = text->nparagraphs && text->paragraphs[index].style.hyphenate;
            ok = ok && yyjson_mut_obj_add_bool(doc, p, "hyphenate", !enabled);
        }
        if (toggle) {
            bool all = true, any = false;
            size_t offset = 0;
            size_t low = pc_text_byte_index(pc_str_c(&text->text), text->text.len, a),
                   high = pc_text_byte_index(pc_str_c(&text->text), text->text.len, b);
            for (size_t i = 0; i < text->nruns; i++) {
                size_t length = text->runs[i].len,
                       end = length > SIZE_MAX - offset ? SIZE_MAX : offset + length;
                if ((end > low && offset < high) || !text->text.len) {
                    const pc_char_style *style = &text->runs[i].style;
                    bool enabled = key == PC_KEY_K   ? style->caps == PC_CAPS_ALL_CAPS
                                   : key == PC_KEY_U ? style->underline
                                   : key == PC_KEY_B ? style->faux_bold
                                   : key == PC_KEY_I ? style->faux_italic
                                                     : style->strikethrough;
                    all = all && enabled;
                    any = true;
                }
                offset = end;
            }
            if (key == PC_KEY_K) {
                ok = ok && yyjson_mut_obj_add_str(doc, p, toggle, all && any ? "normal" : "all");
            } else {
                ok = ok && yyjson_mut_obj_add_bool(doc, p, toggle, !(all && any));
            }
        }
    }
    pc_status status = ok ? execute(session, kerning ? "type.edit" : "type.setStyle", p, out, err)
                          : pc_fail(err, PC_ERR_NOMEM, "type formatting");
    yyjson_mut_doc_free(doc);
    if (status == PC_OK) {
        ui->text_edit.revision = pc_engine_session_active(session)->revision;
    }
    return status;
}
pc_status pc_ui_type_key(pc_engine_session *session, pc_ui_state *ui, pc_ui_type_layout *cache, pc_ui_key key,
                         unsigned modifiers, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *state;
    const pc_text_layer *text;
    pc_status status = current(session, ui, &state, &text, err);
    if (status != PC_OK) {
        return status;
    }
    pc_ui_text_edit *edit = &ui->text_edit;
    bool shift = (modifiers & PC_UI_MOD_SHIFT) != 0,
         command = (modifiers & (PC_UI_MOD_COMMAND | PC_UI_MOD_CTRL)) != 0;
    size_t n = characters(&text->text), a = edit->caret < edit->anchor ? edit->caret : edit->anchor,
           b = edit->caret > edit->anchor ? edit->caret : edit->anchor;
    if (key == PC_KEY_ESCAPE) {
        if (edit->has_preedit) {
            return pc_ui_type_preedit(session, ui, "", 0, out, err);
        }
        pc_json_value pref = PC_JSON_VALUE_INIT;
        bool commit = false;
        if (!edit->mask && pc_engine_prefs_get(session, "type.useEscToCommit", &pref, NULL) == PC_OK) {
            commit = yyjson_mut_get_bool(pc_json_value_root(&pref));
        }
        pc_json_value_fini(&pref);
        return commit ? pc_ui_type_commit(session, ui, modifiers, out, err)
                      : pc_ui_type_cancel(session, ui, err);
    }
    pc_status formatting = format_key(session, ui, text, key, modifiers, out, err);
    if (formatting != PC_ERR_NOT_FOUND) {
        return formatting;
    }
    if (key == PC_KEY_ENTER && command) {
        return pc_ui_type_commit(session, ui, modifiers, out, err);
    }
    if (key == PC_KEY_A && command) {
        edit->anchor = 0;
        edit->caret = n;
        return PC_OK;
    }
    if ((key == PC_KEY_Z || key == PC_KEY_Y) && command) {
        status = pc_ui_type_commit(session, ui, 0, out, err);
        if (status != PC_OK) {
            return status;
        }
        if (key == PC_KEY_Y || shift) {
            (void)pc_engine_session_redo(session);
        } else {
            (void)pc_engine_session_undo(session);
        }
        return PC_OK;
    }
    if (key == PC_KEY_ENTER) {
        return edit->has_preedit ? PC_OK : pc_ui_type_insert(session, ui, "\n", 1, out, err);
    }
    if (key == PC_KEY_TAB) {
        return !modifiers ? pc_ui_type_insert(session, ui, "\t", 1, out, err) : PC_OK;
    }
    if (key == PC_KEY_BACKSPACE || key == PC_KEY_DELETE) {
        if (a == b) {
            if (key == PC_KEY_BACKSPACE) {
                a = command || (modifiers & PC_UI_MOD_ALT)
                        ? pc_text_word_boundary(pc_str_c(&text->text), text->text.len, a, false)
                    : a ? a - 1
                        : 0;
            } else {
                b = command || (modifiers & PC_UI_MOD_ALT)
                        ? pc_text_word_boundary(pc_str_c(&text->text), text->text.len, b, true)
                    : b < n ? b + 1
                            : n;
            }
        }
        if (a == b) {
            return PC_OK;
        }
        return replace(session, ui, a, b, "", 0, out, err);
    }
    if (text->orientation == PC_ORIENT_VERTICAL) {
        if (key == PC_KEY_ARROWUP) {
            key = PC_KEY_ARROWLEFT;
        } else if (key == PC_KEY_ARROWDOWN) {
            key = PC_KEY_ARROWRIGHT;
        } else if (key == PC_KEY_ARROWLEFT) {
            key = PC_KEY_ARROWDOWN;
        } else if (key == PC_KEY_ARROWRIGHT) {
            key = PC_KEY_ARROWUP;
        }
    }
    size_t to = edit->caret;
    if (key == PC_KEY_ARROWLEFT || key == PC_KEY_ARROWRIGHT) {
        bool forward = key == PC_KEY_ARROWRIGHT;
        if (command || (modifiers & PC_UI_MOD_ALT)) {
            to = pc_text_word_boundary(pc_str_c(&text->text), text->text.len, to, forward);
        } else if (a != b && !shift) {
            to = forward ? b : a;
        } else {
            to = forward ? to < n ? to + 1 : n : to ? to - 1 : 0;
        }
    } else if (key == PC_KEY_ARROWUP || key == PC_KEY_ARROWDOWN || key == PC_KEY_HOME || key == PC_KEY_END) {
        status = pc_ui_type_layout_get(cache, state, edit->layer, err);
        if (status != PC_OK) {
            return status;
        }
        if (key == PC_KEY_HOME || key == PC_KEY_END) {
            to = command ? key == PC_KEY_END ? n : 0
                         : pc_text_line_edge(&cache->layout, pc_str_c(&text->text), text->text.len, to,
                                             key == PC_KEY_END);
        } else if (command) {
            to = key == PC_KEY_ARROWDOWN ? n : 0;
        } else {
            float x = 0, top = 0, bottom = 0;
            pc_text_layout_caret(&cache->layout,
                                 pc_text_byte_index(pc_str_c(&text->text), text->text.len, to), &x, &top,
                                 &bottom);
            to = pc_text_line_step(&cache->layout, pc_str_c(&text->text), text->text.len, to, x,
                                   key == PC_KEY_ARROWDOWN ? 1 : -1);
        }
    } else {
        return PC_ERR_NOT_FOUND;
    }
    edit->caret = to > n ? n : to;
    if (!shift) {
        edit->anchor = edit->caret;
    }
    return PC_OK;
}

pc_status pc_ui_type_edit_command(pc_engine_session *session, pc_ui_state *ui, const char *id,
                                  yyjson_val *params, bool coalesce, bool range, pc_json_value *out,
                                  pc_error *err)
{
    const pc_engine_doc_state *state;
    const pc_text_layer *text;
    pc_status status = current(session, ui, &state, &text, err);
    if (status != PC_OK) {
        return status;
    }
    if (params && !yyjson_is_obj(params)) {
        return pc_fail(err, PC_ERR_INVALID, "type options must be an object");
    }
    yyjson_val *target = pc_json_get(params, "layer");
    uint64_t layer = ui->text_edit.layer;
    if (target && (!pc_json_as_u64(target, &layer) || layer != ui->text_edit.layer)) {
        return pc_fail(err, PC_ERR_INVALID, "type target changed");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? (params ? yyjson_val_mut_copy(doc, params) : yyjson_mut_obj(doc)) : NULL;
    if (!p) {
        yyjson_mut_doc_free(doc);
        return pc_fail(err, PC_ERR_NOMEM, "type options");
    }
    (void)yyjson_mut_obj_remove_str(p, "layer");
    (void)yyjson_mut_obj_remove_str(p, "coalesce");
    bool ok = yyjson_mut_obj_add_uint(doc, p, "layer", layer);
    if (coalesce) {
        ok = ok && yyjson_mut_obj_add_strcpy(doc, p, "coalesce", pc_str_c(&ui->text_edit.session));
    }
    if (range && !pc_json_get(params, "range") &&
        (ui->text_edit.caret != ui->text_edit.anchor || !text->text.len)) {
        size_t a = ui->text_edit.caret, b = ui->text_edit.anchor;
        yyjson_mut_val *r = yyjson_mut_arr(doc);
        ok = ok && r && yyjson_mut_arr_add_uint(doc, r, a < b ? a : b) &&
             yyjson_mut_arr_add_uint(doc, r, a > b ? a : b) && yyjson_mut_obj_add_val(doc, p, "range", r);
    }
    status = ok ? execute(session, id, p, out, err) : pc_fail(err, PC_ERR_NOMEM, "type options");
    yyjson_mut_doc_free(doc);
    if (status == PC_OK) {
        ui->text_edit.revision = pc_engine_session_active(session)->revision;
    }
    return status;
}

pc_status pc_ui_type_add_scope(pc_engine_session *session, pc_ui_state *ui, yyjson_mut_doc *doc,
                               yyjson_mut_val *params, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state || !doc || !yyjson_mut_is_obj(params)) {
        return pc_fail(err, PC_ERR_INVALID, "missing type scope");
    }
    if (ui->has_text_edit) {
        const pc_text_layer *text;
        pc_status status = current(session, ui, &state, &text, err);
        if (status != PC_OK) {
            return status;
        }
        if (!add_key(doc, params, &ui->text_edit)) {
            return pc_fail(err, PC_ERR_NOMEM, "type scope");
        }
        if (ui->text_edit.caret != ui->text_edit.anchor) {
            size_t a = ui->text_edit.caret, b = ui->text_edit.anchor;
            yyjson_mut_val *range = yyjson_mut_arr(doc);
            if (!range || !yyjson_mut_arr_add_uint(doc, range, a < b ? a : b) ||
                !yyjson_mut_arr_add_uint(doc, range, a > b ? a : b) ||
                !yyjson_mut_obj_add_val(doc, params, "range", range)) {
                return pc_fail(err, PC_ERR_NOMEM, "type scope");
            }
        }
        return PC_OK;
    }
    pc_layer_id *ids = NULL;
    size_t count = 0;
    pc_status status = pc_engine_doc_state_selected_layers(state, &ids, &count, err);
    if (status != PC_OK) {
        return status;
    }
    yyjson_mut_val *layers = yyjson_mut_arr(doc);
    bool ok = layers != NULL;
    size_t types = 0;
    const pc_document *document = pc_doc_arc_get(state->doc);
    for (size_t i = 0; i < count; i++) {
        const pc_layer *layer = pc_document_layer(document, ids[i]);
        if (layer && layer->content.kind == PC_LAYER_TEXT) {
            ok = ok && yyjson_mut_arr_add_uint(doc, layers, layer->id);
            types++;
        }
    }
    free(ids);
    if (!types) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "select a type layer");
    }
    return ok && yyjson_mut_obj_add_val(doc, params, "layers", layers)
               ? PC_OK
               : pc_fail(err, PC_ERR_NOMEM, "type scope");
}

pc_status pc_ui_type_text_event(pc_engine_session *session, pc_ui_state *ui, const char *value, size_t length,
                                pc_json_value *out, pc_error *err)
{
    if (!value || length > TEXT_LIMIT) {
        return pc_fail(err, PC_ERR_INVALID, "typed text is too long");
    }
    pc_json_value preference = PC_JSON_VALUE_INIT;
    bool smart = false;
    if (pc_engine_prefs_get(session, "type.smartQuotes", &preference, NULL) == PC_OK) {
        smart = yyjson_mut_get_bool(pc_json_value_root(&preference));
    }
    pc_json_value_fini(&preference);
    if (!smart) {
        return pc_ui_type_insert(session, ui, value, length, out, err);
    }
    const pc_engine_doc_state *state;
    const pc_text_layer *text;
    pc_status status = current(session, ui, &state, &text, err);
    if (status != PC_OK) {
        return status;
    }
    size_t at = ui->text_edit.caret < ui->text_edit.anchor ? ui->text_edit.caret : ui->text_edit.anchor;
    size_t byte = pc_text_byte_index(pc_str_c(&text->text), text->text.len, at);
    uint32_t previous = 0;
    for (size_t i = 0; i < byte;) {
        uint32_t scalar;
        if (!pc_utf8_next((const uint8_t *)pc_str_c(&text->text), byte, &i, &scalar)) {
            return pc_fail(err, PC_ERR_INVALID, "invalid type layer text");
        }
        previous = scalar;
    }
    pc_buf converted = PC_BUF_INIT;
    for (size_t i = 0; i < length;) {
        uint32_t scalar;
        if (!pc_utf8_next((const uint8_t *)value, length, &i, &scalar) || !scalar) {
            status = pc_fail(err, PC_ERR_INVALID, "typed text is not UTF-8");
            break;
        }
        if (scalar == '\'' || scalar == '"') {
            bool opens = !previous || pc_unicode_is_space(previous) || previous == '(' || previous == '[' ||
                         previous == '{' || previous == '-' || previous == 0x2013 || previous == 0x2014;
            scalar = scalar == '"' ? (opens ? 0x201c : 0x201d) : (opens ? 0x2018 : 0x2019);
        }
        status = pc_utf8_put(&converted, scalar, err);
        if (status != PC_OK) {
            break;
        }
        if (converted.len > TEXT_LIMIT) {
            status = pc_fail(err, PC_ERR_LIMIT, "typed text exceeds 1 MiB");
            break;
        }
        previous = scalar;
    }
    if (status == PC_OK) {
        status = pc_ui_type_insert(session, ui, pc_buf_cstr(&converted), converted.len, out, err);
    }
    pc_buf_fini(&converted);
    return status;
}

pc_status pc_ui_type_foreground_changed(pc_engine_session *session, pc_ui_state *ui, const float previous[4],
                                        pc_error *err)
{
    if (!ui->has_text_edit || ui->text_edit.caret == ui->text_edit.anchor) {
        return PC_OK;
    }
    const float *color = pc_engine_session_tools(session)->foreground;
    if (!memcmp(color, previous, 4 * sizeof *color)) {
        return PC_OK;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL, *rgba = doc ? yyjson_mut_arr(doc) : NULL;
    bool ok = p && rgba;
    for (size_t i = 0; i < 4 && ok; i++) {
        ok = yyjson_mut_arr_add_real(doc, rgba, color[i]);
    }
    ok = ok && yyjson_mut_obj_add_val(doc, p, "color", rgba);
    pc_status status = ok ? pc_ui_type_add_scope(session, ui, doc, p, err)
                          : pc_fail(err, PC_ERR_NOMEM, "text foreground color");
    pc_json_value out = PC_JSON_VALUE_INIT;
    if (status == PC_OK) {
        status = execute(session, "type.setStyle", p, &out, err);
    }
    if (status == PC_OK) {
        ui->text_edit.revision = pc_engine_session_active(session)->revision;
    }
    pc_json_value_fini(&out);
    yyjson_mut_doc_free(doc);
    return status;
}
