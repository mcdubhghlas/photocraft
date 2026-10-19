#include "harness.h"
#include "ui/state.h"
#include <math.h>
#include <string.h>
static pc_status patch(pc_ui_tool_options *options, const char *text)
{
    yyjson_doc *doc = NULL;
    pc_status rc = pc_json_parse(text, strlen(text), &doc, NULL);
    if (rc == PC_OK) {
        rc = pc_ui_tool_options_patch(options, yyjson_doc_get_root(doc), NULL);
    }
    yyjson_doc_free(doc);
    return rc;
}
static void encode(const pc_ui_tool_options *options, pc_buf *text)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    CHECK(doc != NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_tool_options_to_json(options, doc, &root, NULL) == PC_OK);
    CHECK(yyjson_mut_obj_size(root) == 96);
    CHECK(pc_json_write(root, PC_JSON_SORT_KEYS, text, NULL) == PC_OK);
    yyjson_mut_doc_free(doc);
}
static void test_defaults_clone_and_state_roundtrip(void)
{
    pc_ui_state state, read;
    pc_ui_state_init(&state);
    pc_ui_state_init(&read);
    pc_ui_tool_options *o = &state.tool_options;
    CHECK(o->tolerance == 32 && o->anti_alias && o->move_auto_select && !o->gradient_classic);
    CHECK_EQ_STR(pc_str_c(&o->type_font), "Inter");
    CHECK_EQ_STR(pc_str_c(&o->move_target), "group");
    CHECK(o->shape_stroke.miter_limit == 100 && o->shape_stroke.opacity == 1);
    CHECK(o->magnetic_frequency == 57 && o->eyedropper_size == 1 && o->crop_shield.opacity == 75);
    CHECK(patch(o, "{\"type_font\":\"日本語 "
                   "Font\",\"type_size\":63.5,\"shape_stroke\":{\"dashes\":[4,2],\"cap\":\"Round\"},\"crop_"
                   "shield\":{\"opacity\":42},\"move_auto_select\":false}") == PC_OK);
    pc_ui_tool_options clone = {0};
    CHECK(pc_ui_tool_options_clone(o, &clone, NULL) == PC_OK);
    CHECK(clone.shape_stroke.dashes != o->shape_stroke.dashes && clone.type_font.ptr != o->type_font.ptr);
    CHECK(clone.shape_stroke.ndashes == 2 && clone.crop_shield.enabled && clone.crop_shield.opacity == 42);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(&state, doc, &root, NULL) == PC_OK);
    pc_buf text = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &text, NULL) == PC_OK);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, NULL) == PC_OK);
    CHECK(pc_ui_state_from_json(&read, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    pc_buf expected = PC_BUF_INIT, actual = PC_BUF_INIT;
    encode(o, &expected);
    encode(&read.tool_options, &actual);
    CHECK_EQ_STR(pc_buf_cstr(&expected), pc_buf_cstr(&actual));
    pc_buf_fini(&expected);
    pc_buf_fini(&actual);
    pc_buf_fini(&text);
    yyjson_doc_free(parsed);
    yyjson_mut_doc_free(doc);
    pc_ui_tool_options_fini(&clone);
    pc_ui_state_fini(&state);
    pc_ui_state_fini(&read);
}
static void test_every_field_rejects_wrong_types_atomically(void)
{
    pc_ui_tool_options options;
    pc_ui_tool_options_init(&options);
    pc_buf expected = PC_BUF_INIT;
    encode(&options, &expected);
    yyjson_doc *doc = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&expected), expected.len, &doc, NULL) == PC_OK);
    size_t i, max;
    yyjson_val *key, *value;
    yyjson_obj_foreach(yyjson_doc_get_root(doc), i, max, key, value)
    {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_val *bad = yyjson_is_num(value)    ? yyjson_mut_str(d, "bad")
                              : yyjson_is_bool(value) ? yyjson_mut_uint(d, 7)
                                                      : yyjson_mut_null(d);
        yyjson_mut_obj_add_real(d, root, "feather", 9);
        yyjson_mut_obj_add_val(d, root, yyjson_get_str(key), bad);
        pc_buf text = PC_BUF_INIT;
        CHECK(pc_json_write(root, 0, &text, NULL) == PC_OK);
        CHECK(patch(&options, pc_buf_cstr(&text)) == PC_ERR_INVALID);
        pc_buf actual = PC_BUF_INIT;
        encode(&options, &actual);
        CHECK_EQ_STR(pc_buf_cstr(&expected), pc_buf_cstr(&actual));
        pc_buf_fini(&actual);
        pc_buf_fini(&text);
        yyjson_mut_doc_free(d);
    }
    CHECK(patch(&options, "{\"shape_stroke\":{\"dashes\":[0,0]}}") == PC_ERR_INVALID);
    CHECK(patch(&options, "{\"shape_stroke\":{\"miter_limit\":0}}") == PC_ERR_INVALID);
    CHECK(patch(&options, "{\"crop_shield\":{\"custom_color\":[256,0,0]}}") == PC_ERR_INVALID);
    CHECK(patch(&options, "{\"type_size\":1e100}") == PC_ERR_INVALID);
    CHECK(patch(&options, "{\"eyedropper_size\":-1}") == PC_ERR_INVALID);
    CHECK(patch(&options, "{\"crop_overlay\":\"unknown\"}") == PC_ERR_INVALID);
    CHECK(patch(&options, "{\"futureOption\":true}") == PC_OK);
    yyjson_doc_free(doc);
    pc_buf_fini(&expected);
    pc_ui_tool_options_fini(&options);
}
static void test_crop_shield_and_orientations(void)
{
    pc_ui_crop_shield shield;
    pc_ui_crop_shield_default(&shield);
    pc_ui_color canvas = {10, 20, 30, 40}, result = {0};
    CHECK(pc_ui_crop_shield_fill(&shield, canvas, false, &result));
    CHECK(result.a == 191 && result.r == 10);
    CHECK(pc_ui_crop_shield_fill(&shield, canvas, true, &result));
    CHECK(result.a == 96);
    shield.opacity = NAN;
    CHECK(pc_ui_crop_shield_fill(&shield, canvas, false, &result) && result.a == 191);
    shield.opacity = 200;
    CHECK(pc_ui_crop_shield_fill(&shield, canvas, false, &result) && result.a == 255);
    shield.show_cropped_area = false;
    shield.enabled = false;
    CHECK(pc_ui_crop_shield_fill(&shield, canvas, true, &result) && result.a == 255);
    shield.classic_mode = true;
    CHECK(!pc_ui_crop_shield_fill(&shield, canvas, false, &result));
    shield.enabled = true;
    shield.custom = true;
    shield.custom_color[0] = 200;
    CHECK(pc_ui_crop_shield_fill(&shield, canvas, false, &result) && result.r == 200);
    CHECK(pc_ui_crop_overlay_orientations(PC_UI_CROP_GOLDEN_SPIRAL) == 4);
    CHECK(pc_ui_crop_overlay_orientations(PC_UI_CROP_TRIANGLE) == 2);
    CHECK(pc_ui_crop_overlay_orientations(PC_UI_CROP_GRID) == 1);
}
int main(int argc, char **argv)
{
    RUN(test_defaults_clone_and_state_roundtrip);
    RUN(test_every_field_rejects_wrong_types_atomically);
    RUN(test_crop_shield_and_orientations);
    return pc_t_finish();
}
