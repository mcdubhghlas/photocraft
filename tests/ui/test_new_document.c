#include "harness.h"
#include "ui/new_document.h"
#include "ui/state.h"
#include <math.h>
#include <string.h>
static float number(yyjson_mut_val *fields, const char *key)
{
    return (float)yyjson_mut_get_num(yyjson_mut_obj_get(fields, key));
}
static void test_units_and_typed_sizes(void)
{
    size_t count = 0;
    const pc_ui_new_unit *units = pc_ui_new_units(&count);
    CHECK(count == 6);
    for (size_t i = 0; i < count; i++)
        for (unsigned dpi = 1; dpi < 600; dpi += 37)
            for (unsigned n = 1; n < 300000; n += 7919) {
                float shown = pc_ui_new_to_unit((float)n, units[i].id, (float)dpi);
                CHECK_NEAR(pc_ui_new_from_unit(shown, units[i].id, (float)dpi), (float)n, 1);
            }
    CHECK(pc_ui_new_pixel_value(NAN) == 1 && pc_ui_new_pixel_value(INFINITY) == 1);
    CHECK(pc_ui_new_pixel_value(-10) == 1 && pc_ui_new_pixel_value(1e9f) == 300000);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    CHECK(doc != NULL);
    yyjson_mut_val *fields = pc_ui_state_new_document_fields(doc);
    CHECK(pc_ui_new_set_size(doc, fields, "width", 841, "mm", 72, NULL) == PC_OK);
    CHECK(pc_ui_new_set_size(doc, fields, "height", 1189, "mm", 72, NULL) == PC_OK);
    CHECK_NEAR(pc_ui_new_shown_size(fields, "width", 0, "mm", 72), 841, 0);
    CHECK_NEAR(pc_ui_new_shown_size(fields, "height", 0, "mm", 72), 1189, 0);
    CHECK(pc_ui_new_swap_size(doc, fields, NULL) == PC_OK);
    CHECK_NEAR(pc_ui_new_shown_size(fields, "width", 0, "mm", 72), 1189, 0);
    CHECK_NEAR(pc_ui_new_shown_size(fields, "height", 0, "mm", 72), 841, 0);
    yyjson_mut_obj_add_str(doc, fields, "__unit", "mm");
    yyjson_mut_obj_add_uint(doc, fields, "resolution", 72);
    float width = number(fields, "width"), height = number(fields, "height");
    CHECK(pc_ui_new_set_resolution(doc, fields, 144, NULL) == PC_OK);
    CHECK(number(fields, "width") == 2 * width && number(fields, "height") == 2 * height);
    pc_buf before = PC_BUF_INIT, after = PC_BUF_INIT;
    CHECK(pc_json_write(fields, 0, &before, NULL) == PC_OK);
    CHECK(pc_ui_new_set_size(doc, fields, "width", NAN, "mm", 72, NULL) == PC_ERR_INVALID);
    CHECK(pc_ui_new_set_resolution(doc, fields, INFINITY, NULL) == PC_ERR_INVALID);
    CHECK(pc_json_write(fields, 0, &after, NULL) == PC_OK);
    CHECK_EQ_STR(pc_buf_cstr(&before), pc_buf_cstr(&after));
    pc_buf_fini(&before);
    pc_buf_fini(&after);
    yyjson_mut_doc_free(doc);
}
static void test_presets_and_settings(void)
{
    size_t count = 0;
    const pc_ui_new_preset *presets = pc_ui_new_presets(&count);
    CHECK(count == 35);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *fields = pc_ui_state_new_document_fields(doc);
    size_t letter = 0;
    for (size_t i = 0; i < count; i++) {
        CHECK(pc_ui_new_apply_preset(doc, fields, &presets[i], 200, 96, NULL) == PC_OK);
        CHECK(number(fields, "width") > 0 && number(fields, "height") > 0);
        if (!strcmp(presets[i].name, "Letter")) {
            letter = i;
        }
    }
    CHECK(pc_ui_new_apply_preset(doc, fields, &presets[letter], 200, 96, NULL) == PC_OK);
    CHECK(number(fields, "width") == 1700 && number(fields, "height") == 2200 &&
          number(fields, "resolution") == 200);
    yyjson_mut_obj_add_str(doc, fields, "__orientation", "landscape");
    CHECK(pc_ui_new_apply_preset(doc, fields, &presets[letter], 200, 96, NULL) == PC_OK);
    CHECK(number(fields, "width") == 2200 && number(fields, "height") == 1700);
    yyjson_mut_val *settings = pc_ui_new_settings(doc, fields);
    CHECK(settings != NULL);
    CHECK(yyjson_mut_obj_get(settings, "__orientation") == NULL);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(settings, "unit")), "in");
    yyjson_mut_doc *other = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *read = pc_ui_state_new_document_fields(other);
    CHECK(pc_ui_new_apply_settings(other, read, settings, NULL) == PC_OK);
    yyjson_mut_doc_free(doc);
    CHECK(number(read, "width") == 2200 && number(read, "height") == 1700 &&
          number(read, "resolution") == 200);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(read, "__unit")), "in");
    yyjson_mut_doc_free(other);
}
static void test_clipboard_uses_exact_pixels_and_resolution(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK(d != NULL);
    yyjson_mut_val *fields = pc_ui_state_new_document_fields(d);
    CHECK(fields != NULL);
    CHECK(yyjson_mut_obj_add_str(d, fields, "__orientation", "portrait"));
    CHECK(pc_ui_new_offer_clipboard(d, fields, 320, 200, NULL) == PC_OK);
    CHECK(number(fields, "width") == 320 && number(fields, "height") == 200);
    CHECK(number(fields, "resolution") == 72);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(fields, "__preset")), "Clipboard");
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(fields, "__clipboard")) == 2);
    const pc_ui_new_preset clipboard = {"Recent", "Clipboard", 320, 200, 72};
    CHECK(pc_ui_new_apply_preset(d, fields, &clipboard, 300, 144, NULL) == PC_OK);
    CHECK(number(fields, "width") == 320 && number(fields, "height") == 200 &&
          number(fields, "resolution") == 72);
    CHECK(pc_ui_new_offer_clipboard(d, fields, 0, 200, NULL) == PC_ERR_INVALID);
    CHECK(number(fields, "width") == 320);
    CHECK(pc_ui_new_offer_clipboard(d, fields, UINT32_MAX, 1, NULL) == PC_ERR_INVALID);
    yyjson_mut_doc_free(d);
}

int main(int argc, char **argv)
{
    RUN(test_clipboard_uses_exact_pixels_and_resolution);
    RUN(test_units_and_typed_sizes);
    RUN(test_presets_and_settings);
    return pc_t_finish();
}
