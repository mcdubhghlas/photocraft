#include "harness.h"
#include "ui/type_tool.h"
#include "ui/type_overlay.h"
#include "ui/type_properties.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>
static pc_engine_session *fresh(pc_ui_state *ui)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":400,\"height\":300,\"resolution\":72}",
                                         &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_state_init(ui);
    ui->tool = PC_TOOL_TYPE;
    ui->tool_options.type_size = 18;
    return s;
}
static const pc_document *document(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}
static const pc_text_layer *text(pc_engine_session *s, pc_layer_id id)
{
    const pc_layer *layer = pc_document_layer(document(s), id);
    CHECK(layer && layer->content.kind == PC_LAYER_TEXT);
    return &layer->content.u.text;
}
static void insert(pc_engine_session *s, pc_ui_state *ui, const char *value)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_type_insert(s, ui, value, strlen(value), &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}
static void press(pc_engine_session *s, pc_ui_state *ui, pc_ui_type_layout *layout, pc_ui_key key,
                  unsigned modifiers)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_type_key(s, ui, layout, key, modifiers, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}
static void create(pc_engine_session *s, pc_ui_state *ui, const float start[2], const float end[2])
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_type_create(s, ui, start, end, 1, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}
static void test_unicode_edit_preedit_and_one_undo(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    pc_ui_type_layout layout = {0};
    size_t layers = document(s)->nlayers;
    const float start[] = {30, 60};
    create(s, &ui, start, start);
    CHECK(ui.has_text_edit && ui.text_edit.created && ui.text_edit.anchor == 0 && ui.text_edit.caret == 11);
    pc_layer_id id = ui.text_edit.layer;
    insert(s, &ui, "Hello \xce\xa9 \xd0\x96");
    CHECK_EQ_STR(pc_str_c(&text(s, id)->text), "Hello \xce\xa9 \xd0\x96");
    press(s, &ui, &layout, PC_KEY_ARROWLEFT, PC_UI_MOD_SHIFT);
    insert(s, &ui, "!");
    CHECK_EQ_STR(pc_str_c(&text(s, id)->text), "Hello \xce\xa9 !");
    press(s, &ui, &layout, PC_KEY_A, PC_UI_MOD_COMMAND);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_type_preedit(s, &ui, "n", 1, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_type_preedit(s, &ui, "\xe6\x97\xa5", 3, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(ui.text_edit.has_preedit && ui.text_edit.preedit_length == 1);
    insert(s, &ui, "\xe6\x97\xa5\xe6\x9c\xac");
    CHECK(!ui.text_edit.has_preedit);
    CHECK_EQ_STR(pc_str_c(&text(s, id)->text), "\xe6\x97\xa5\xe6\x9c\xac");
    CHECK(pc_ui_type_commit(s, &ui, 0, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(!ui.has_text_edit && document(s)->nlayers == layers + 1);
    CHECK(pc_engine_session_undo(s));
    CHECK(document(s)->nlayers == layers && !pc_document_layer(document(s), id));
    CHECK(pc_engine_session_redo(s));
    CHECK_EQ_STR(pc_str_c(&text(s, id)->text), "\xe6\x97\xa5\xe6\x9c\xac");
    pc_ui_type_layout_fini(&layout);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_existing_edit_cancel_invalid_input_and_document_guard(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    const float start[] = {30, 60};
    create(s, &ui, start, start);
    insert(s, &ui, "Original");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_type_commit(s, &ui, 0, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_layer_id id = pc_engine_session_active(s)->active_layer;
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    CHECK(pc_ui_type_begin(s, &ui, id, true, NULL) == PC_OK);
    insert(s, &ui, "Edited");
    CHECK(pc_ui_type_cancel(s, &ui, NULL) == PC_OK);
    CHECK(pc_document_eq(pc_doc_arc_get(before), document(s)) && !ui.has_text_edit);
    pc_doc_arc_release(before);
    CHECK(pc_ui_type_begin(s, &ui, id, true, NULL) == PC_OK);
    uint64_t revision = pc_engine_session_active(s)->revision;
    CHECK(pc_ui_type_insert(s, &ui, "\xc0\xaf", 2, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_engine_session_active(s)->revision == revision);
    CHECK(pc_ui_type_insert(s, &ui, "x", ((size_t)1 << 20) + 1, &out, NULL) == PC_ERR_LIMIT);
    ui.text_edit.caret = ui.text_edit.anchor = SIZE_MAX;
    insert(s, &ui, "!");
    CHECK_EQ_STR(pc_str_c(&text(s, id)->text), "Original!");
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":20,\"height\":20}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    revision = pc_engine_session_active(s)->revision;
    CHECK(pc_ui_type_insert(s, &ui, "wrong doc", 9, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_engine_session_active(s)->revision == revision);
    CHECK(pc_ui_type_cancel(s, &ui, NULL) == PC_OK && !ui.has_text_edit);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_paragraph_vertical_and_type_mask_commit(void)
{
    pc_ui_state ui;
    pc_engine_session *s = fresh(&ui);
    ui.tool = PC_TOOL_VERTICAL_TYPE;
    float start[] = {100, 100}, end[] = {20, 40};
    create(s, &ui, start, end);
    pc_layer_id id = ui.text_edit.layer;
    CHECK(text(s, id)->shape.kind == PC_TEXT_SHAPE_BOX && text(s, id)->shape.width == 80 &&
          text(s, id)->shape.height == 60 && text(s, id)->orientation == PC_ORIENT_VERTICAL);
    insert(s, &ui, "ABCD");
    pc_ui_type_layout layout = {0};
    press(s, &ui, &layout, PC_KEY_ARROWUP, 0);
    CHECK(ui.text_edit.caret == 3);
    press(s, &ui, &layout, PC_KEY_ARROWDOWN, PC_UI_MOD_SHIFT);
    CHECK(ui.text_edit.caret == 4 && ui.text_edit.anchor == 3);
    CHECK(pc_ui_type_cancel(s, &ui, NULL) == PC_OK);
    ui.tool = PC_TOOL_HORIZONTAL_TYPE_MASK;
    size_t count = document(s)->nlayers;
    create(s, &ui, start, start);
    id = ui.text_edit.layer;
    CHECK(ui.text_edit.mask && !pc_document_layer(document(s), id)->visible && text(s, id)->text.len == 0);
    insert(s, &ui, "MASK");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_type_commit(s, &ui, 0, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(!ui.has_text_edit && !pc_document_layer(document(s), id) && document(s)->nlayers == count &&
          document(s)->has_selection);
    CHECK(pc_engine_session_undo(s));
    CHECK(!document(s)->has_selection && document(s)->nlayers == count);
    pc_ui_type_layout_fini(&layout);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(s);
}
static void test_pointer_hit_selection_drag_and_guards(void)
{
    pc_ui_state ui;
    pc_engine_session *session = fresh(&ui);
    pc_ui_type_pointer pointer = {0};
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_ui_tool_point point = {.x = 30, .y = 60};
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_DOWN, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(pointer.pressed && !ui.has_text_edit);
    point.x = 200;
    point.y = 150;
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_UP, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(ui.has_text_edit && text(session, ui.text_edit.layer)->shape.kind == PC_TEXT_SHAPE_BOX);
    insert(session, &ui, "Selectable");
    pc_layer_id id = ui.text_edit.layer;
    CHECK(pc_ui_type_commit(session, &ui, 0, &out, NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    CHECK(pc_ui_type_layout_get(&pointer.layout, state, id, NULL) == PC_OK);
    const pc_text_layer *t = text(session, id);
    float seg[4];
    pc_text_layout_caret_segment(&pointer.layout.layout, 0, seg);
    pc_point doc = pc_affine_apply(t->transform, (pc_point){seg[0], (seg[1] + seg[3]) * .5});
    point.x = (float)doc.x;
    point.y = (float)doc.y;
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_DOWN, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(ui.has_text_edit && pointer.selecting && ui.text_edit.layer == id && ui.text_edit.caret == 0);
    pc_text_layout_caret_segment(&pointer.layout.layout, t->text.len, seg);
    doc = pc_affine_apply(t->transform, (pc_point){seg[0], (seg[1] + seg[3]) * .5});
    point.x = (float)doc.x;
    point.y = (float)doc.y;
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_UP, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(ui.text_edit.anchor == 0 && ui.text_edit.caret == 10 && !pointer.pressed);
    point.x = 350;
    point.y = 250;
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_DOWN, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(!ui.has_text_edit && !pointer.pressed);
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_DOWN, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(pc_engine_session_execute_json(session, "layer.new.layer", "{}", &out, NULL) == PC_OK);
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_UP, point, 0, 1, &out, NULL) ==
          PC_ERR_INVALID);
    CHECK(!pointer.pressed);
    pc_json_value_fini(&out);
    pc_ui_type_pointer_fini(&pointer);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session);
}
static void test_type_mask_display_cache_and_clear(void)
{
    pc_ui_state ui;
    pc_engine_session *session = fresh(&ui);
    ui.tool = PC_TOOL_HORIZONTAL_TYPE_MASK;
    float point[2] = {30, 60};
    create(session, &ui, point, point);
    pc_ui_canvas_cache source = {0};
    pc_ui_channel_cache cache = {0};
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    CHECK(pc_ui_canvas_cache_update(&source, state, NULL) == PC_OK);
    pc_ui_channel_overlay overlay = pc_ui_type_mask_overlay(session, &ui);
    CHECK(overlay.active && overlay.opacity > 0);
    CHECK(pc_ui_channel_cache_update_overlay(&cache, state, &source, false, &overlay, NULL) == PC_OK);
    CHECK(cache.active && cache.image.len > 0);
    CHECK_NEAR(cache.image.px[0][0], overlay.opacity, 1.f / 255);
    CHECK_NEAR(cache.image.px[0][3], overlay.opacity, 1.f / 255);
    uint64_t generation = cache.generation;
    CHECK(pc_ui_channel_cache_update_overlay(&cache, state, &source, false, &overlay, NULL) == PC_OK &&
          cache.generation == generation);
    insert(session, &ui, "MASK");
    state = pc_engine_session_active(session);
    CHECK(pc_ui_canvas_cache_update(&source, state, NULL) == PC_OK);
    overlay = pc_ui_type_mask_overlay(session, &ui);
    CHECK(pc_ui_channel_cache_update_overlay(&cache, state, &source, false, &overlay, NULL) == PC_OK &&
          cache.generation != generation);
    bool hole = false;
    for (size_t i = 0; i < cache.image.len; i++) {
        if (cache.image.px[i][3] < overlay.opacity - .1f) {
            hole = true;
            break;
        }
    }
    CHECK(hole);
    CHECK(pc_ui_type_cancel(session, &ui, NULL) == PC_OK);
    state = pc_engine_session_active(session);
    CHECK(pc_ui_canvas_cache_update(&source, state, NULL) == PC_OK);
    overlay = pc_ui_type_mask_overlay(session, &ui);
    CHECK(!overlay.active);
    CHECK(pc_ui_channel_cache_update_overlay(&cache, state, &source, false, &overlay, NULL) == PC_OK &&
          !cache.active);
    pc_ui_channel_cache_fini(&cache);
    pc_ui_canvas_cache_fini(&source);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session);
}
static void test_paragraph_handles_coalesce_and_format_target(void)
{
    pc_ui_state ui;
    pc_engine_session *session = fresh(&ui);
    pc_ui_type_pointer pointer = {0};
    float start[2] = {30, 60}, end[2] = {200, 150};
    create(session, &ui, start, end);
    insert(session, &ui, "Formatted");
    const pc_text_layer *t = text(session, ui.text_edit.layer);
    float width = t->shape.width, height = t->shape.height;
    pc_point handle = pc_affine_apply(t->transform, (pc_point){t->shape.x + width, t->shape.y + height / 2});
    pc_ui_tool_point point = {.x = (float)handle.x, .y = (float)handle.y};
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_DOWN, point, 0, 1, &out, NULL) ==
          PC_OK);
    CHECK(ui.text_edit.has_resize && ui.text_edit.resize == 5);
    point.x += 40;
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_MOVE, point, 0, 1, &out, NULL) ==
          PC_OK);
    point.x += 20;
    CHECK(pc_ui_type_pointer_event(&pointer, session, &ui, PC_UI_POINTER_UP, point, 0, 1, &out, NULL) ==
          PC_OK);
    t = text(session, ui.text_edit.layer);
    CHECK_NEAR(t->shape.width, width + 60, .01);
    CHECK_NEAR(t->shape.height, height, .01);
    CHECK(!ui.text_edit.has_resize && !pointer.pressed);
    ui.text_edit.anchor = 0;
    ui.text_edit.caret = 4;
    yyjson_doc *params = NULL;
    CHECK(pc_json_parse("{\"size\":36}", 11, &params, NULL) == PC_OK);
    CHECK(pc_ui_type_edit_command(session, &ui, "type.setStyle", yyjson_doc_get_root(params), true, true,
                                  &out, NULL) == PC_OK);
    yyjson_doc_free(params);
    t = text(session, ui.text_edit.layer);
    CHECK(t->nruns >= 2 && t->runs[0].len == 4 && t->runs[0].style.size_pt == 36 &&
          t->runs[1].style.size_pt == 18);
    insert(session, &ui, "Bold");
    CHECK(pc_ui_type_cancel(session, &ui, NULL) == PC_OK);
    CHECK(document(session)->nlayers == 1);
    pc_json_value_fini(&out);
    pc_ui_type_pointer_fini(&pointer);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session);
}
static void test_type_properties_format_keys_and_escape_preference(void)
{
    pc_ui_state ui;
    pc_engine_session *session = fresh(&ui);
    pc_ui_type_layout layout = {0};
    float point[2] = {30, 60};
    create(session, &ui, point, point);
    insert(session, &ui, "Hello Ω");
    ui.text_edit.anchor = 6;
    ui.text_edit.caret = 7;
    pc_layer_id id = ui.text_edit.layer;
    press(session, &ui, &layout, PC_KEY_B, PC_UI_MOD_COMMAND | PC_UI_MOD_SHIFT);
    const pc_text_layer *t = text(session, id);
    const pc_char_style *style = pc_ui_type_representative(t, 7, 6);
    CHECK(style && style->faux_bold && !pc_ui_type_representative(t, 0, 0)->faux_bold);
    CHECK(pc_ui_type_representative(t, SIZE_MAX, SIZE_MAX)->faux_bold);
    press(session, &ui, &layout, PC_KEY_U, PC_UI_MOD_COMMAND | PC_UI_MOD_SHIFT);
    style = pc_ui_type_representative(text(session, id), 7, 6);
    CHECK(style && style->underline && style->faux_bold);
    press(session, &ui, &layout, PC_KEY_ARROWRIGHT, PC_UI_MOD_ALT);
    style = pc_ui_type_representative(text(session, id), 7, 6);
    CHECK(style && style->tracking == 20);
    press(session, &ui, &layout, PC_KEY_ARROWUP, PC_UI_MOD_ALT | PC_UI_MOD_SHIFT);
    style = pc_ui_type_representative(text(session, id), 7, 6);
    CHECK(style && style->baseline_shift_pt == 2);
    CHECK_NEAR(pc_ui_type_unit_factor("pixels", 144), 2, 0);
    CHECK_NEAR(pc_ui_type_unit_factor("mm", 72), 25.4 / 72, 1e-6);
    CHECK(pc_ui_type_unit_factor("pixels", NAN) == 1);
    press(session, &ui, &layout, PC_KEY_ESCAPE, 0);
    CHECK(!ui.has_text_edit && pc_document_layer(document(session), id));
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, "prefs.set",
                                         "{\"path\":\"type.useEscToCommit\",\"value\":false}", &out,
                                         NULL) == PC_OK);
    CHECK(pc_ui_type_begin(session, &ui, id, true, NULL) == PC_OK);
    insert(session, &ui, "Changed");
    press(session, &ui, &layout, PC_KEY_ESCAPE, 0);
    CHECK(!ui.has_text_edit);
    CHECK_EQ_STR(pc_str_c(&text(session, id)->text), "Hello Ω");
    pc_json_value_fini(&out);
    pc_ui_type_layout_fini(&layout);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session);
}
static void test_typed_smart_quotes_do_not_rewrite_paste(void)
{
    pc_ui_state ui;
    pc_engine_session *session = fresh(&ui);
    float point[2] = {30, 60};
    create(session, &ui, point, point);
    pc_json_value out = PC_JSON_VALUE_INIT;
    const char *typed = "\"Hello\" don't";
    CHECK(pc_ui_type_text_event(session, &ui, typed, strlen(typed), &out, NULL) == PC_OK);
    CHECK_EQ_STR(pc_str_c(&text(session, ui.text_edit.layer)->text), "“Hello” don’t");
    insert(session, &ui, " \"literal\"");
    CHECK_CONTAINS(pc_str_c(&text(session, ui.text_edit.layer)->text), "\"literal\"");
    CHECK(pc_engine_session_execute_json(
              session, "prefs.set", "{\"path\":\"type.smartQuotes\",\"value\":false}", &out, NULL) == PC_OK);
    CHECK(pc_ui_type_text_event(session, &ui, " \"plain\"", 8, &out, NULL) == PC_OK);
    CHECK_CONTAINS(pc_str_c(&text(session, ui.text_edit.layer)->text), "\"plain\"");
    pc_json_value_fini(&out);
    pc_ui_state_fini(&ui);
    pc_engine_session_free(session);
}
int main(int argc, char **argv)
{
    RUN(test_typed_smart_quotes_do_not_rewrite_paste);
    RUN(test_type_properties_format_keys_and_escape_preference);
    RUN(test_paragraph_handles_coalesce_and_format_target);
    RUN(test_pointer_hit_selection_drag_and_guards);
    RUN(test_type_mask_display_cache_and_clear);
    RUN(test_unicode_edit_preedit_and_one_undo);
    RUN(test_existing_edit_cancel_invalid_input_and_document_guard);
    RUN(test_paragraph_vertical_and_type_mask_commit);
    return pc_t_finish();
}
