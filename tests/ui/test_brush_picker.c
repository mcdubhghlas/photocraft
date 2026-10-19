#include "harness.h"
#include "ui/state.h"
#include <string.h>
static void test_picker_validation_atomic_and_state_roundtrip(void)
{
    pc_ui_state state;
    pc_ui_state_init(&state);
    pc_ui_brush_picker *p = &state.brush_picker;
    CHECK(p->show_name && p->show_stroke && p->show_tip && p->scale == 1 && !p->open);
    const char *json = "{\"brushPicker\":[30,60],\"brushPickerName\":false,\"brushPickerStroke\":false,"
                       "\"brushPickerScale\":0.3,\"brushesView\":\"grid\",\"brushSize\":37}";
    yyjson_doc *params = NULL;
    CHECK(pc_json_parse(json, strlen(json), &params, NULL) == PC_OK);
    bool has_size = false;
    double size = 0;
    CHECK(pc_ui_brush_picker_configure(p, yyjson_doc_get_root(params), &state.brushes_grid, &has_size, &size,
                                       NULL) == PC_OK);
    CHECK(p->open && p->point[0] == 30 && p->point[1] == 60);
    CHECK(!p->show_name && !p->show_stroke && p->show_tip && state.brushes_grid && has_size && size == 37);
    yyjson_doc_free(params);
    const char *bad[] = {"{\"brushPickerTip\":false}", "{\"brushPickerScale\":0.1}", "{\"brushPicker\":[1]}",
                         "{\"brushPickerName\":5}",    "{\"brushSize\":5001}",       "{\"brushSize\":false}",
                         "{\"brushesView\":\"tiles\"}"};
    pc_ui_brush_picker before = *p;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        CHECK(pc_json_parse(bad[i], strlen(bad[i]), &params, NULL) == PC_OK);
        CHECK(pc_ui_brush_picker_configure(p, yyjson_doc_get_root(params), &state.brushes_grid, &has_size,
                                           &size, NULL) != PC_OK);
        CHECK(!memcmp(p, &before, sizeof before) && state.brushes_grid);
        yyjson_doc_free(params);
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(&state, d, &root, NULL) == PC_OK);
    pc_buf text = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &text, NULL) == PC_OK);
    CHECK(pc_json_parse(pc_buf_cstr(&text), text.len, &params, NULL) == PC_OK);
    pc_ui_state copy;
    pc_ui_state_init(&copy);
    CHECK(pc_ui_state_from_json(&copy, yyjson_doc_get_root(params), NULL) == PC_OK);
    CHECK(copy.brush_picker.open && copy.brush_picker.point[1] == 60 && copy.brushes_grid);
    CHECK(!copy.brush_picker.show_name && copy.brush_picker.show_tip);
    CHECK_NEAR(copy.brush_picker.scale, .3, .00001);
    pc_ui_state_fini(&copy);
    pc_ui_state_fini(&state);
    yyjson_doc_free(params);
    yyjson_mut_doc_free(d);
    pc_buf_fini(&text);
}
int main(int argc, char **argv)
{
    RUN(test_picker_validation_atomic_and_state_roundtrip);
    return pc_t_finish();
}
