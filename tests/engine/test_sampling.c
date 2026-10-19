#include "harness.h"
#include "engine/sampling.h"
#include "engine/engine_internal.h"
#include <math.h>
#include <string.h>
static pc_status command(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = pc_engine_session_execute_json(s, id, json, &out, &err);
    if (status != PC_OK)
        fprintf(stderr, "sampling command %s: %s\n", id, err.msg);
    pc_json_value_fini(&out);
    return status;
}
static pc_engine_session *session(const char *mode, unsigned depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char text[160];
    (void)snprintf(text, sizeof text,
                   "{\"width\":40,\"height\":40,\"mode\":\"%s\",\"depth\":%u,\"background\":\"transparent\"}",
                   mode, depth);
    CHECK(command(s, "file.new", text) == PC_OK);
    return s;
}
static void test_alpha_weighted_average_quantization_and_nonfinite(void)
{
    const float pixels[][4] = {{1, 0, 0, 1}, {0, 0, 1, .5f}, {0, 1, 0, 0}, {NAN, 1, 0, 1}};
    float out[4];
    pc_engine_sample_average(pixels, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, out);
    CHECK(out[0] == 170.f / 255 && out[1] == 0 && out[2] == 85.f / 255 && out[3] == 96.f / 255);
    pc_engine_sample_average(pixels, 3, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, out);
    CHECK(fabsf(out[0] - 2.f / 3) < 1e-6 && fabsf(out[2] - 1.f / 3) < 1e-6 && out[3] == .5f);
    const float gray[][4] = {{0, 0, 0, 1}, {1, 1, 1, 1}};
    pc_engine_sample_average(gray, 2, PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, out);
    CHECK(out[0] == 32768.f / 65535 && out[0] == out[1] && out[0] == out[2] && out[3] == 1);
    pc_engine_sample_average(NULL, 0, PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U8, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
}
static pc_status black_corner(pc_document *doc, bool *has_active, pc_layer_id *active, void *context,
                              pc_error *err)
{
    (void)has_active;
    (void)context;
    pc_layer *layer = pc_document_layer_mut(doc, *active);
    const float black[4] = {0, 0, 0, 1};
    return pc_surface_fill_rect(pc_layer_surface_mut(layer), pc_rect_make(0, 0, 3, 3), black, 4, err);
}
static void test_average_square_clipping_and_source_immutability(void)
{
    pc_engine_session *s = session("rgb", 8);
    CHECK(command(s, "edit.fill", "{\"color\":\"#ffffff\"}") == PC_OK);
    CHECK(pc_engine_session_edit(s, "black corner", black_corner, NULL, NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    const pc_doc_arc *before = state->doc;
    size_t history = pc_history_past_len(state->history);
    float color[4];
    const pc_document *doc = pc_doc_arc_get(before);
    CHECK(pc_engine_sample_color(doc, true, state->active_layer, 3.5, 3.5, 1, PC_SAMPLE_ALL, color, NULL) ==
          PC_OK);
    CHECK(color[0] == 1 && color[3] == 1);
    CHECK(pc_engine_sample_color(doc, true, state->active_layer, 3.5, 3.5, 3, PC_SAMPLE_ALL, color, NULL) ==
          PC_OK);
    CHECK(color[0] == 227.f / 255 && color[0] == color[1] && color[3] == 1);
    CHECK(pc_engine_sample_color(doc, true, state->active_layer, .5, .5, 5, PC_SAMPLE_ALL, color, NULL) ==
          PC_OK);
    CHECK(color[0] == 0 && color[3] == 1);
    CHECK(pc_engine_sample_color(doc, true, state->active_layer, 1, 1, 5, PC_SAMPLE_ALL, color, NULL) ==
          PC_OK);
    CHECK(color[0] == 112.f / 255 && color[3] == 1);
    CHECK(pc_engine_sample_color(doc, true, state->active_layer, 20, 20, 101, PC_SAMPLE_ALL, color, NULL) ==
          PC_OK);
    CHECK(color[0] == 254.f / 255);
    CHECK(state->doc == before && pc_history_past_len(state->history) == history);
    pc_engine_session_free(s);
}
static void test_scope_current_below_adjustments_and_mask(void)
{
    pc_engine_session *s = session("rgb", 32);
    CHECK(command(s, "edit.fill", "{\"color\":\"#ff0000\"}") == PC_OK);
    pc_layer_id red = pc_engine_session_active(s)->active_layer;
    CHECK(command(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(command(s, "edit.fill", "{\"color\":\"#00ff00\"}") == PC_OK);
    CHECK(command(s, "layer.setProps", "{\"opacity\":0.5}") == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    pc_layer_id green = state->active_layer;
    float c[4];
    const pc_document *doc = pc_doc_arc_get(state->doc);
    CHECK(pc_engine_sample_color(doc, true, green, 10, 10, 1, PC_SAMPLE_CURRENT, c, NULL) == PC_OK);
    CHECK(c[0] == 0 && c[1] == 1 && c[3] == 1);
    CHECK(pc_engine_sample_color(doc, true, red, 10, 10, 1, PC_SAMPLE_CURRENT_AND_BELOW, c, NULL) == PC_OK);
    CHECK(c[0] == 1 && c[1] == 0 && c[3] == 1);
    CHECK(pc_engine_sample_color(doc, true, green, 10, 10, 1, PC_SAMPLE_ALL, c, NULL) == PC_OK);
    CHECK(c[0] == .5f && c[1] == .5f && c[3] == 1);
    CHECK(command(s, "layer.newAdjustmentLayer.invert", "{}") == PC_OK);
    doc = pc_doc_arc_get(state->doc);
    CHECK(pc_engine_sample_color(doc, true, state->active_layer, 10, 10, 1, PC_SAMPLE_ALL, c, NULL) == PC_OK);
    CHECK(c[2] == 1);
    CHECK(pc_engine_sample_color(doc, true, state->active_layer, 10, 10, 1, PC_SAMPLE_ALL_NO_ADJUSTMENTS, c,
                                 NULL) == PC_OK);
    CHECK(c[0] == .5f && c[1] == .5f && c[2] == 0);
    char text[80];
    (void)snprintf(text, sizeof text, "{\"layer\":%llu}", (unsigned long long)green);
    CHECK(command(s, "layer.select", text) == PC_OK);
    CHECK(command(s, "layer.layerMask.hideAll", "{}") == PC_OK);
    doc = pc_doc_arc_get(state->doc);
    CHECK(pc_engine_sample_color(doc, true, green, 10, 10, 1, PC_SAMPLE_CURRENT, c, NULL) == PC_OK);
    CHECK(c[3] == 0);
    CHECK(pc_engine_sample_color(doc, true, green, 10, 10, 1, PC_SAMPLE_CURRENT_AND_BELOW_NO_ADJUSTMENTS, c,
                                 NULL) == PC_OK);
    CHECK(c[0] == 1 && c[1] == 0 && c[3] == 1);
    pc_engine_session_free(s);
}
static void test_all_models_depths_flat_point_and_average(void)
{
    const char *modes[] = {"rgb", "gray", "cmyk", "lab"};
    const unsigned depths[] = {8, 16, 32};
    for (size_t m = 0; m < 4; m++)
        for (size_t d = 0; d < 3; d++) {
            pc_engine_session *s = session(modes[m], depths[d]);
            CHECK(command(s, "edit.fill", "{\"color\":\"#a54c78\"}") == PC_OK);
            const pc_engine_doc_state *state = pc_engine_session_active(s);
            float a[4], b[4];
            CHECK(pc_engine_sample_color(pc_doc_arc_get(state->doc), true, state->active_layer, 10, 10, 1,
                                         PC_SAMPLE_ALL, a, NULL) == PC_OK);
            CHECK(pc_engine_sample_color(pc_doc_arc_get(state->doc), true, state->active_layer, 10, 10, 11,
                                         PC_SAMPLE_ALL, b, NULL) == PC_OK);
            for (size_t c = 0; c < 4; c++)
                CHECK(fabsf(a[c] - b[c]) < (depths[d] == 8 ? .012f : depths[d] == 16 ? .0001f : .00001f));
            pc_engine_session_free(s);
        }
}
static void test_command_validation_off_canvas_and_readonly(void)
{
    pc_engine_session *s = session("rgb", 8);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    const pc_doc_arc *before = state->doc;
    uint64_t revision = state->revision;
    const char *bad[] = {"{}", "{\"x\":1,\"y\":1,\"size\":2}", "{\"x\":1,\"y\":1,\"sampleLayer\":\"bogus\"}",
                         "{\"x\":\"bad\",\"y\":1}", "{\"x\":1,\"y\":1,\"size\":10000000000}"};
    pc_json_value out = PC_JSON_VALUE_INIT;
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(pc_engine_session_execute_json(s, "document.sampleColor", bad[i], &out, NULL) ==
              PC_ERR_INVALID);
    CHECK(pc_engine_session_execute_json(s, "document.sampleColor", "{\"x\":-1,\"y\":0}", &out, NULL) ==
          PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(yyjson_mut_is_arr(root) && yyjson_mut_arr_size(root) == 4);
    for (size_t i = 0; i < 4; i++)
        CHECK(yyjson_mut_get_num(yyjson_mut_arr_get(root, i)) == 0);
    CHECK(state->doc == before && state->revision == revision);
    float c[4];
    CHECK(pc_engine_sample_color(pc_doc_arc_get(before), false, 0, NAN, 0, 1, PC_SAMPLE_ALL, c, NULL) ==
              PC_OK &&
          c[3] == 0);
    CHECK(pc_engine_sample_color(pc_doc_arc_get(before), false, 0, 1, 1, 1, PC_SAMPLE_CURRENT, c, NULL) ==
              PC_OK &&
          c[3] == 0);
    CHECK(pc_engine_sample_size(NULL, NULL, NULL) == PC_ERR_INVALID);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_alpha_weighted_average_quantization_and_nonfinite);
    RUN(test_average_square_clipping_and_source_immutability);
    RUN(test_scope_current_below_adjustments_and_mask);
    RUN(test_all_models_depths_flat_point_and_average);
    RUN(test_command_validation_off_canvas_and_readonly);
    return pc_t_finish();
}
