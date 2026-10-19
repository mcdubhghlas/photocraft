#include "ui/i18n.h"
#include "type_controls.h"
#include "shell_internal.h"
#include "ui/type_properties.h"
#include "engine/prefs.h"
#include "dialogs.h"
#include "base/mem.h"
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
void pc_ui_type_controls_fini(pc_ui_type_controls *c)
{
    pc_json_value_fini(&c->families);
    pc_json_value_fini(&c->faces);
    pc_str_fini(&c->face_family);
    pc_str_fini(&c->filter_query);
    free(c->filtered);
    memset(c, 0, sizeof *c);
}
static const pc_text_layer *target(pc_ui_shell_app *app)
{
    const pc_ui_type_controls *controls = pc_ui_shell_type_controls(app);
    if (controls->override_text) {
        return controls->override_text;
    }
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!state) {
        return NULL;
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (ui->has_text_edit && ui->text_edit.document && ui->text_edit.document != doc->id) {
        return NULL;
    }
    const pc_layer *layer =
        pc_document_layer(doc, ui->has_text_edit ? ui->text_edit.layer : state->active_layer);
    return layer && layer->content.kind == PC_LAYER_TEXT ? &layer->content.u.text : NULL;
}
static const pc_char_style *representative(pc_ui_shell_app *app, const pc_text_layer *text)
{
    const pc_ui_state *ui = pc_ui_shell_app_state(app);
    return pc_ui_type_representative(text, ui->has_text_edit ? ui->text_edit.caret : 0,
                                     ui->has_text_edit ? ui->text_edit.anchor : 0);
}
static const char *family_name(pc_ui_shell_app *app, const pc_text_layer *text)
{
    const pc_char_style *style = representative(app, text);
    return style  ? pc_str_c(&style->font_family)
           : text ? pc_str_c(&text->font_family)
                  : pc_str_c(&pc_ui_shell_app_state(app)->tool_options.type_font);
}
static void submit(pc_ui_shell_app *app, yyjson_mut_doc *doc, yyjson_mut_val *params, const char *command)
{
    pc_ui_type_controls *controls = pc_ui_shell_type_controls(app);
    if (controls->editing_style) {
        (void)yyjson_mut_obj_remove_str(params, "metricsAsShown");
        yyjson_mut_val *p = yyjson_mut_obj(doc), *attrs = yyjson_mut_val_mut_copy(doc, params);
        if (p && attrs && yyjson_mut_obj_add_uint(doc, p, "id", controls->style_id) &&
            yyjson_mut_obj_add_val(doc, p, "attrs", attrs)) {
            pc_ui_shell_queue(
                app, controls->style_paragraph ? "type.paragraphStyle.set" : "type.characterStyle.set", p);
        }
        return;
    }
    if (pc_ui_type_add_scope(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app), doc, params, NULL) ==
        PC_OK) {
        pc_ui_shell_queue(app, command, params);
    }
}
static void set_string(pc_ui_shell_app *app, const char *key, const char *value, const char *command)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
    if (p && yyjson_mut_obj_add_strcpy(doc, p, key, value)) {
        submit(app, doc, p, command);
    }
    yyjson_mut_doc_free(doc);
}
static void set_number(pc_ui_shell_app *app, const char *key, double value)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
    if (p && yyjson_mut_obj_add_real(doc, p, key, value) &&
        yyjson_mut_obj_add_bool(doc, p, "metricsAsShown", true)) {
        submit(app, doc, p, "type.setStyle");
    }
    yyjson_mut_doc_free(doc);
}
static void number(pc_ui_shell_app *app, const char *label, const char *key, float value, float min,
                   float max, float step)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    float next = value;
    char id[80];
    (void)snprintf(id, sizeof id, "type.%s", key);
    pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
    nk_property_float(ctx, label, min, &next, max, step, .1f);
    if (next != value) {
        set_number(app, key, next);
    }
}
static void boolean(pc_ui_shell_app *app, const char *label, const char *key, bool value)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_bool next = value;
    nk_checkbox_label(ctx, label, &next);
    if ((next != 0) != value) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
        if (p && yyjson_mut_obj_add_bool(doc, p, key, next != 0)) {
            submit(app, doc, p, "type.setStyle");
        }
        yyjson_mut_doc_free(doc);
    }
}
static bool matches(const char *value, const char *search)
{
    size_t n = strlen(search);
    if (!n) {
        return true;
    }
    for (size_t i = 0; value[i]; i++) {
        size_t j = 0;
        while (j < n && value[i + j] &&
               tolower((unsigned char)value[i + j]) == tolower((unsigned char)search[j])) {
            j++;
        }
        if (j == n) {
            return true;
        }
    }
    return false;
}
static void font_picker(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_type_controls *cache = pc_ui_shell_type_controls(app);
    pc_ui_tool_options *options = &pc_ui_shell_app_state(app)->tool_options;
    const pc_text_layer *text = target(app);
    const char *family = family_name(app, text);
    if (!cache->families.doc) {
        (void)pc_engine_session_execute(pc_ui_shell_app_session(app), "type.fonts", NULL, &cache->families,
                                        NULL);
    }
    pc_ui_shell_widget_mapping(app, "type.font", nk_widget_bounds(ctx));
    if (nk_combo_begin_label(ctx, family, nk_vec2(300, 350))) {
        nk_layout_row_dynamic(ctx, 24, 1);
        pc_ui_shell_widget_mapping(app, "type.font.search", nk_widget_bounds(ctx));
        (void)nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, cache->search, sizeof cache->search,
                                             nk_filter_default);
        yyjson_mut_val *items = yyjson_mut_obj_get(pc_json_value_root(&cache->families), "families");
        if (!cache->filtered || !pc_str_eq_c(&cache->filter_query, cache->search)) {
            size_t count = yyjson_mut_arr_size(items);
            if (count > 65536) {
                count = 65536;
            }
            size_t *indices = pc_malloc_array(count ? count : 1, sizeof *indices);
            pc_str query = PC_STR_INIT;
            if (indices && pc_str_set_c(&query, cache->search, NULL) == PC_OK) {
                size_t n = 0;
                for (size_t i = 0; i < count; i++) {
                    const char *name = yyjson_mut_get_str(yyjson_mut_arr_get(items, i));
                    if (name && matches(name, cache->search)) {
                        indices[n++] = i;
                    }
                }
                free(cache->filtered);
                pc_str_fini(&cache->filter_query);
                cache->filtered = indices;
                cache->nfiltered = n;
                cache->filter_query = query;
            } else {
                free(indices);
                pc_str_fini(&query);
            }
        }
        bool chosen = false;
        nk_layout_row_dynamic(ctx, 286, 1);
        struct nk_list_view view;
        if (nk_list_view_begin(ctx, &view, "font-families", 0, 24, (int)cache->nfiltered)) {
            for (int row = view.begin; row < view.end; row++) {
                if (!cache->filtered || row < 0 || (size_t)row >= cache->nfiltered) {
                    continue;
                }
                size_t index = cache->filtered[row];
                const char *name = yyjson_mut_get_str(yyjson_mut_arr_get(items, index));
                if (!name) {
                    continue;
                }
                nk_layout_row_dynamic(ctx, 24, 1);
                char id[64];
                (void)snprintf(id, sizeof id, "type.font.family.%zu", index);
                pc_ui_shell_widget_mapping(app, id, nk_widget_bounds(ctx));
                if (nk_button_label(ctx, name)) {
                    if (!cache->editing_style) {
                        (void)pc_str_set_c(&options->type_font, name, NULL);
                        (void)pc_str_set_c(&options->type_style, "Regular", NULL);
                    }
                    set_string(app, "font", name, "type.setStyle");
                    cache->search[0] = 0;
                    chosen = true;
                }
            }
            nk_list_view_end(&view);
        }
        if (chosen) {
            nk_combo_close(ctx);
        }
        nk_combo_end(ctx);
    }
}
static void style_picker(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_type_controls *cache = pc_ui_shell_type_controls(app);
    pc_ui_tool_options *o = &pc_ui_shell_app_state(app)->tool_options;
    const pc_text_layer *text = target(app);
    const char *family = family_name(app, text);
    const pc_char_style *style_at = representative(app, text);
    const char *current = style_at ? pc_str_c(&style_at->font_style) : pc_str_c(&o->type_style);
    if (!pc_str_eq_c(&cache->face_family, family)) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
        if (p && yyjson_mut_obj_add_strcpy(doc, p, "family", family)) {
            pc_buf json = PC_BUF_INIT;
            if (pc_json_write(p, 0, &json, NULL) == PC_OK) {
                pc_json_value next = PC_JSON_VALUE_INIT;
                if (pc_engine_session_execute_json(pc_ui_shell_app_session(app), "type.fonts",
                                                   pc_buf_cstr(&json), &next, NULL) == PC_OK) {
                    pc_json_value_fini(&cache->faces);
                    cache->faces = next;
                    (void)pc_str_set_c(&cache->face_family, family, NULL);
                }
            }
            pc_buf_fini(&json);
        }
        yyjson_mut_doc_free(doc);
    }
    pc_ui_shell_widget_mapping(app, "type.fontStyle", nk_widget_bounds(ctx));
    if (nk_combo_begin_label(ctx, *current ? current : "Regular", nk_vec2(240, 250))) {
        yyjson_mut_val *items = yyjson_mut_obj_get(pc_json_value_root(&cache->faces), "faces");
        size_t i, n;
        yyjson_mut_val *value;
        yyjson_mut_arr_foreach(items, i, n, value)
        {
            const char *style = yyjson_mut_get_str(yyjson_mut_obj_get(value, "style"));
            if (!style) {
                continue;
            }
            nk_layout_row_dynamic(ctx, 24, 1);
            if (nk_combo_item_label(ctx, style, NK_TEXT_LEFT)) {
                if (!cache->editing_style) {
                    (void)pc_str_set_c(&o->type_style, style, NULL);
                }
                set_string(app, "fontStyle", style, "type.setStyle");
            }
        }
        nk_combo_end(ctx);
    }
}
static void size_field(pc_ui_shell_app *app)
{
    const pc_text_layer *text = target(app);
    pc_ui_tool_options *o = &pc_ui_shell_app_state(app)->tool_options;
    const pc_char_style *style = representative(app, text);
    float size = style ? style->size_pt : text ? text->size_pt : o->type_size;
    const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
    float dpi = state ? pc_doc_arc_get(state->doc)->resolution_dpi : 72;
    pc_json_value pref = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(pc_ui_shell_app_session(app), "unitsAndRulers.typeUnits", &pref, NULL);
    const char *unit = yyjson_mut_get_str(pc_json_value_root(&pref));
    float factor = pc_ui_type_unit_factor(unit, dpi);
    char label[32];
    (void)snprintf(label, sizeof label, "Size %s",
                   unit && !strcmp(unit, "pixels") ? "px"
                   : unit && !strcmp(unit, "mm")   ? "mm"
                                                   : "pt");
    pc_json_value_fini(&pref);
    float value = (text ? size * pc_ui_type_shown_scale(text) : size) * factor, next = value;
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_shell_widget_mapping(app, "type.size", nk_widget_bounds(ctx));
    nk_property_float(ctx, label, .1f * factor, &next, 1296 * factor, 1, .1f);
    if (next != value) {
        if (!pc_ui_shell_type_controls(app)->editing_style) {
            o->type_size = next / factor;
        }
        set_number(app, "size", next / factor);
    }
}
static void align_picker(pc_ui_shell_app *app)
{
    const char *const labels[] = {"Left",           "Center",        "Right",      "Justify Left",
                                  "Justify Center", "Justify Right", "Justify All"};
    const char *const keys[] = {"left",          "center",       "right",     "justify",
                                "justifyCenter", "justifyRight", "justifyAll"};
    pc_ui_tool_options *o = &pc_ui_shell_app_state(app)->tool_options;
    const pc_text_layer *text = target(app);
    int selected = text && text->nparagraphs ? (int)text->paragraphs[0].style.align : 0;
    if (!text) {
        for (int i = 0; i < 7; i++) {
            if (pc_str_eq_c(&o->type_align, keys[i])) {
                selected = i;
            }
        }
    }
    int next = nk_combo(pc_ui_shell_app_nk(app), labels, 7, selected, 24, nk_vec2(200, 210));
    if (next != selected && next >= 0 && next < 7) {
        if (!pc_ui_shell_type_controls(app)->editing_style) {
            (void)pc_str_set_c(&o->type_align, keys[next], NULL);
        }
        set_string(app, "align", keys[next], "type.setStyle");
    }
}
pc_status pc_ui_shell_type_color(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    const pc_text_layer *text = target(app);
    const pc_char_style *style = representative(app, text);
    if (!text) {
        return pc_fail(err, PC_ERR_INVALID, "select a type layer");
    }
    float rgba[4] = {0, 0, 0, 1};
    pc_color_to_rgb(style ? &style->color : &text->color, rgba);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
    pc_status scoped =
        pc_ui_type_add_scope(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app), doc, p, err);
    bool ok = scoped == PC_OK;
    pc_status status = ok ? pc_ui_shell_dialog_color_command(app, "Color Picker (Text Color)", rgba,
                                                             "type.setStyle", p, "color", out, err)
                          : pc_fail(err, PC_ERR_NOMEM, "text color target");
    yyjson_mut_doc_free(doc);
    return status;
}
static void color_button(pc_ui_shell_app *app)
{
    const pc_text_layer *text = target(app);
    const pc_char_style *style = representative(app, text);
    float rgba[4] = {0, 0, 0, 1};
    if (text) {
        pc_color_to_rgb(style ? &style->color : &text->color, rgba);
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    pc_ui_shell_widget_mapping(app, "type.color", nk_widget_bounds(ctx));
    if (nk_button_color(ctx, nk_rgba_f(rgba[0], rgba[1], rgba[2], 1))) {
        const pc_ui_type_controls *controls = pc_ui_shell_type_controls(app);
        if (controls->editing_style) {
            yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
            if (p && yyjson_mut_obj_add_uint(doc, p, "id", controls->style_id)) {
                pc_ui_shell_queue(app, "ui.type.styleColor", p);
            }
            yyjson_mut_doc_free(doc);
        } else if (text) {
            pc_ui_shell_queue(app, "ui.type.color", NULL);
        } else {
            yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *p = doc ? yyjson_mut_obj(doc) : NULL;
            if (p && yyjson_mut_obj_add_str(doc, p, "kind", "colorPicker") &&
                yyjson_mut_obj_add_str(doc, p, "target", "foreground")) {
                pc_ui_shell_queue(app, "ui.dialog.open", p);
            }
            yyjson_mut_doc_free(doc);
        }
    }
}
void pc_ui_shell_type_options(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    nk_layout_row_push(ctx, 175);
    font_picker(app);
    nk_layout_row_push(ctx, 120);
    style_picker(app);
    nk_layout_row_push(ctx, 130);
    size_field(app);
    nk_layout_row_push(ctx, 110);
    align_picker(app);
    nk_layout_row_push(ctx, 28);
    color_button(app);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    const char *aa[] = {"none", "sharp", "crisp", "strong", "smooth"};
    int selected = 0;
    for (int i = 0; i < 5; i++) {
        if (pc_str_eq_c(&ui->tool_options.type_aa, aa[i])) {
            selected = i;
        }
    }
    const pc_text_layer *text = target(app);
    if (text) {
        selected = text->antialias < PC_AA_WINDOWS ? (int)text->antialias : 4;
    }
    nk_layout_row_push(ctx, 100);
    int next = nk_combo(ctx, aa, 5, selected, 24, nk_vec2(130, 160));
    if (next != selected && next >= 0 && next < 5) {
        (void)pc_str_set_c(&ui->tool_options.type_aa, aa[next], NULL);
        set_string(app, "antialias", aa[next], "type.edit");
    }
    if (ui->has_text_edit) {
        nk_layout_row_push(ctx, 65);
        if (nk_button_label(ctx, pc_i18n_t("Cancel"))) {
            pc_ui_shell_queue(app, "ui.type.cancel", NULL);
        }
        nk_layout_row_push(ctx, 65);
        if (nk_button_label(ctx, pc_i18n_t("Commit"))) {
            pc_ui_shell_queue(app, "ui.type.commit", NULL);
        }
    }
}
void pc_ui_shell_type_panel(pc_ui_shell_app *app, bool paragraph)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    const pc_text_layer *text = target(app);
    nk_layout_row_dynamic(ctx, 24, 1);
    if (!text && !pc_ui_tool_is_type(pc_ui_shell_app_state(app)->tool)) {
        nk_label(ctx, pc_i18n_t("Select a type layer"), NK_TEXT_LEFT);
        return;
    }
    if (paragraph) {
        align_picker(app);
        pc_paragraph_style style =
            text && text->nparagraphs ? text->paragraphs[0].style : pc_paragraph_style_default();
        nk_layout_row_dynamic(ctx, 24, 1);
        number(app, "First Line", "firstLineIndent", style.first_line_indent_pt, -5000, 5000, 1);
        number(app, "Left Indent", "startIndent", style.start_indent_pt, -5000, 5000, 1);
        number(app, "Right Indent", "endIndent", style.end_indent_pt, -5000, 5000, 1);
        number(app, "Space Before", "spaceBefore", style.space_before_pt, 0, 5000, 1);
        number(app, "Space After", "spaceAfter", style.space_after_pt, 0, 5000, 1);
        number(app, "Auto Leading %", "autoLeading", style.auto_leading * 100, 50, 500, 1);
        boolean(app, "Hyphenate", "hyphenate", style.hyphenate);
        const char *labels[] = {"Auto direction", "Left to right", "Right to left"},
                   *keys[] = {"auto", "ltr", "rtl"};
        int next = nk_combo(ctx, labels, 3, (int)style.direction, 24, nk_vec2(180, 120));
        if (next != (int)style.direction && next >= 0 && next < 3) {
            set_string(app, "direction", keys[next], "type.setStyle");
        }
    } else {
        font_picker(app);
        style_picker(app);
        size_field(app);
        color_button(app);
        pc_char_style fallback;
        pc_char_style_default(&fallback);
        const pc_char_style *style = representative(app, text);
        if (!style) {
            style = &fallback;
        }
        float scale = pc_ui_type_shown_scale(text);
        nk_layout_row_dynamic(ctx, 24, 1);
        number(app, "Tracking", "tracking", style->tracking, -1000, 10000, 10);
        if (style->has_leading) {
            number(app, "Leading", "leading", style->leading_pt * scale, .01f, 5000, 1);
        }
        nk_bool automatic = !style->has_leading;
        nk_checkbox_label(ctx, pc_i18n_t("Auto Leading"), &automatic);
        if ((automatic != 0) == style->has_leading) {
            if (automatic) {
                set_string(app, "leading", "auto", "type.setStyle");
            } else {
                set_number(app, "leading", text ? text->size_pt * scale * 1.2 : 14.4);
            }
        }
        number(app, "Baseline", "baselineShift", style->baseline_shift_pt, -5000, 5000, 1);
        number(app, "Horizontal %", "horizontalScale", style->horizontal_scale * 100, 1, 1000, 1);
        number(app, "Vertical %", "verticalScale", style->vertical_scale * 100, 1, 1000, 1);
        nk_layout_row_dynamic(ctx, 24, 2);
        boolean(app, "Faux Bold", "fauxBold", style->faux_bold);
        boolean(app, "Faux Italic", "fauxItalic", style->faux_italic);
        boolean(app, "Underline", "underline", style->underline);
        boolean(app, "Strike", "strikethrough", style->strikethrough);
        boolean(app, "Ligatures", "ligatures", style->ligatures);
        boolean(app, "Discretionary", "discretionaryLigatures", style->discretionary_ligatures);
        pc_char_style_fini(&fallback);
    }
}

void pc_ui_shell_type_style_editor(pc_ui_shell_app *app, bool paragraph, uint32_t id,
                                   const pc_char_style *character, const pc_paragraph_style *para)
{
    pc_ui_type_controls *controls = pc_ui_shell_type_controls(app);
    pc_char_style fallback;
    pc_char_style_default(&fallback);
    pc_text_layer text = {0};
    pc_text_run run = {.style = character ? *character : fallback};
    pc_paragraph_run prun = {.style = para ? *para : pc_paragraph_style_default()};
    text.runs = &run;
    text.nruns = 1;
    text.font_family = run.style.font_family;
    text.size_pt = run.style.size_pt;
    text.paragraphs = &prun;
    text.nparagraphs = 1;
    text.transform = pc_affine_scale(1);
    controls->override_text = &text;
    controls->editing_style = true;
    controls->style_id = id;
    controls->style_paragraph = paragraph;
    pc_ui_shell_type_panel(app, paragraph);
    controls->override_text = NULL;
    controls->editing_style = false;
    pc_char_style_fini(&fallback);
}
