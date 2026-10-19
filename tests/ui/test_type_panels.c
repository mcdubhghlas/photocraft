#include "harness.h"
#include "ui/state.h"
#include <string.h>
#include <stdlib.h>
static pc_status read_state(pc_ui_type_panels *state, const char *json)
{
    yyjson_doc *doc = NULL;
    pc_status status = pc_json_parse(json, strlen(json), &doc, NULL);
    if (status == PC_OK) {
        status = pc_ui_type_panels_from_json(state, yyjson_doc_get_root(doc), NULL);
    }
    yyjson_doc_free(doc);
    return status;
}
static void test_owned_type_panel_state_round_trip(void)
{
    pc_ui_type_panels state = {0};
    CHECK(read_state(&state, "{\"styles\":true,\"styles_tab\":1,\"glyphs\":true,\"options\":[true,7],\"glyph_"
                             "family\":\"DejaVu "
                             "Sans\",\"glyph_style\":\"Bold\",\"glyph_category\":\"Punctuation\",\"glyph_"
                             "size\":48,\"glyph_recent\":[\"Ω\",\"Ж\"],\"glyph_selected\":\"Ω\",\"spell\":{"
                             "\"items\":[{\"word\":\"wrod\"}],\"index\":0,\"ignore\":[\"ignore\"],\"change_"
                             "to\":\"word\",\"message\":\"ready\",\"all_layers\":true}}") == PC_OK);
    CHECK(state.styles && state.styles_tab == 1 && state.glyphs && state.options_id == 7 &&
          state.options_paragraph);
    CHECK(state.glyph_recent.n == 2 && state.has_glyph_selected && state.has_spell && state.spell.all_layers);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_type_panels_to_json(&state, doc, &root, NULL) == PC_OK);
    pc_buf json = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &json, NULL) == PC_OK);
    yyjson_mut_doc_free(doc);
    pc_ui_type_panels clone = {0};
    CHECK(read_state(&clone, pc_buf_cstr(&json)) == PC_OK);
    pc_buf_fini(&json);
    pc_ui_type_panels_fini(&state);
    CHECK_EQ_STR(pc_str_c(&clone.glyph_family), "DejaVu Sans");
    CHECK_EQ_STR(clone.glyph_recent.v[0], "Ω");
    CHECK_EQ_STR(pc_str_c(&clone.spell.change_to), "word");
    CHECK(yyjson_mut_arr_size(pc_json_value_root(&clone.spell.items)) == 1);
    pc_ui_type_panels_fini(&clone);
}
static void test_type_panel_hostile_input_is_atomic(void)
{
    pc_ui_type_panels state = {0};
    CHECK(read_state(&state, "{\"glyph_family\":\"original\",\"styles\":true}") == PC_OK);
    const char *bad[] = {"null",
                         "[]",
                         "{\"styles\":1}",
                         "{\"styles_tab\":2}",
                         "{\"styles_tab\":-1}",
                         "{\"options\":[false,4294967296]}",
                         "{\"options\":[true]}",
                         "{\"glyph_size\":1e300}",
                         "{\"glyph_size\":-1}",
                         "{\"glyph_family\":true}",
                         "{\"glyph_family\":\"x\\u0000y\"}",
                         "{\"glyph_recent\":[1]}",
                         "{\"glyph_selected\":3}",
                         "{\"spell\":[]}",
                         "{\"spell\":{\"items\":{}}}",
                         "{\"spell\":{\"index\":-1}}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        CHECK(read_state(&state, bad[i]) == PC_ERR_INVALID);
        CHECK(state.styles);
        CHECK_EQ_STR(pc_str_c(&state.glyph_family), "original");
    }
    pc_ui_type_panels_fini(&state);
}
static void test_recent_glyphs_bounded_unique_and_newest_first(void)
{
    pc_ui_type_panels state = {0};
    for (unsigned i = 0; i < 300; i++) {
        char text[32];
        int n = snprintf(text, sizeof text, "glyph-%u", i);
        CHECK(n > 0);
        CHECK(pc_ui_type_glyph_used(&state, text, (size_t)n, NULL) == PC_OK);
    }
    CHECK(state.glyph_recent.n == 256);
    CHECK_EQ_STR(state.glyph_recent.v[0], "glyph-299");
    CHECK_EQ_STR(state.glyph_recent.v[255], "glyph-44");
    CHECK(pc_ui_type_glyph_used(&state, "glyph-200", 9, NULL) == PC_OK);
    CHECK(state.glyph_recent.n == 256);
    CHECK_EQ_STR(state.glyph_recent.v[0], "glyph-200");
    CHECK_EQ_STR(state.glyph_recent.v[1], "glyph-299");
    CHECK(pc_ui_type_glyph_used(&state, "x\0y", 3, NULL) == PC_ERR_INVALID);
    CHECK(pc_ui_type_glyph_used(&state, "", 0, NULL) == PC_ERR_INVALID);
    CHECK_EQ_STR(pc_str_c(&state.glyph_selected), "glyph-200");
    pc_ui_type_panels_fini(&state);
}
static void test_ui_state_retains_type_panels(void)
{
    pc_ui_state state;
    pc_ui_state_init(&state);
    state.type_panels.styles = true;
    state.type_panels.styles_tab = 1;
    CHECK(pc_ui_type_glyph_used(&state.type_panels, "Ω", 2, NULL) == PC_OK);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(&state, doc, &root, NULL) == PC_OK);
    pc_buf json = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &json, NULL) == PC_OK);
    yyjson_mut_doc_free(doc);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&json), json.len, &parsed, NULL) == PC_OK);
    pc_ui_state clone;
    pc_ui_state_init(&clone);
    CHECK(pc_ui_state_from_json(&clone, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    yyjson_doc_free(parsed);
    pc_buf_fini(&json);
    pc_ui_state_fini(&state);
    CHECK(clone.type_panels.styles && clone.type_panels.styles_tab == 1);
    CHECK_EQ_STR(clone.type_panels.glyph_recent.v[0], "Ω");
    pc_ui_state_fini(&clone);
}
int main(int argc, char **argv)
{
    RUN(test_owned_type_panel_state_round_trip);
    RUN(test_type_panel_hostile_input_is_atomic);
    RUN(test_recent_glyphs_bounded_unique_and_newest_first);
    RUN(test_ui_state_retains_type_panels);
    return pc_t_finish();
}
