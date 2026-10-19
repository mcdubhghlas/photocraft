#include "harness.h"
#include "engine/adjust_eyedropper.h"
#include "engine/adjust_params.h"
#include "compose/compose.h"
#include "compose/adjust.h"
#include <math.h>
#include <string.h>
static pc_status command(pc_engine_session *session, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = pc_engine_session_execute_json(session, id, json, &out, &err);
    if (status != PC_OK)
        fprintf(stderr, "%s: %s\n", id, err.msg);
    pc_json_value_fini(&out);
    return status;
}
static pc_status pick(const char *kind, const char *params, pc_engine_tone_picker picker,
                      const float color[3], pc_color_mode mode, pc_adjustment *out)
{
    yyjson_doc *doc = NULL;
    CHECK(pc_json_parse(params, strlen(params), &doc, NULL) == PC_OK);
    pc_status status =
        pc_engine_adjustment_pick(kind, yyjson_doc_get_root(doc), NULL, picker, color, mode, out, NULL);
    yyjson_doc_free(doc);
    return status;
}
static pc_status roundtrip(const char *kind, const pc_adjustment *source, pc_color_mode mode,
                           pc_adjustment *out)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = NULL;
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *doc = NULL;
    pc_status status = pc_engine_adjustment_to_params(source, d, &params, NULL);
    if (status == PC_OK)
        status = pc_json_write(params, 0, &text, NULL);
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &doc, NULL);
    if (status == PC_OK)
        status = pc_engine_adjustment_from_params(kind, yyjson_doc_get_root(doc), NULL, mode, out, NULL);
    yyjson_doc_free(doc);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
    return status;
}
static void output(const pc_adjustment *adj, const float input[3], float out[3])
{
    float rgba[4] = {input[0], input[1], input[2], 1};
    pc_compose_buffer buffer = {0};
    CHECK(pc_compose_buffer_filled(pc_rect_make(0, 0, 1, 1), rgba, &buffer, NULL) == PC_OK);
    CHECK(pc_compose_adjust_apply(adj, &buffer, NULL) == PC_OK);
    if (buffer.len)
        memcpy(out, buffer.px[0], 3 * sizeof *out);
    pc_compose_buffer_fini(&buffer);
}
static float luma(const float rgb[3])
{
    return rgb[0] * .2126f + rgb[1] * .7152f + rgb[2] * .0722f;
}
static void test_endpoint_serialization_boundaries(void)
{
    const struct {
        pc_engine_tone_picker picker;
        float sample, expected;
    } cases[] = {{PC_TONE_PICK_WHITE, 0, 1},
                 {PC_TONE_PICK_WHITE, .001f, 1},
                 {PC_TONE_PICK_WHITE, .494f / 255, 1},
                 {PC_TONE_PICK_WHITE, .496f / 255, .992f},
                 {PC_TONE_PICK_WHITE, .2f, 1},
                 {PC_TONE_PICK_BLACK, 1, 0},
                 {PC_TONE_PICK_BLACK, .999f, 0},
                 {PC_TONE_PICK_BLACK, 1 - .494f / 255, 0},
                 {PC_TONE_PICK_BLACK, 1 - .496f / 255, .008f},
                 {PC_TONE_PICK_BLACK, .8f, 0}};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        pc_adjustment resolved = {0}, parsed = {0};
        float sample[3] = {cases[i].sample, cases[i].sample, cases[i].sample}, got[3];
        CHECK(pick("curves", "{}", cases[i].picker, sample, PC_COLOR_MODE_RGB, &resolved) == PC_OK);
        CHECK(roundtrip("curves", &resolved, PC_COLOR_MODE_RGB, &parsed) == PC_OK);
        const pc_curve *curves[] = {&parsed.u.curves.master, &parsed.u.curves.per_channel[0],
                                    &parsed.u.curves.per_channel[1], &parsed.u.curves.per_channel[2]};
        for (size_t c = 0; c < 4; c++) {
            CHECK(curves[c]->len >= 2 && curves[c]->len <= 19);
            for (size_t p = 1; p < curves[c]->len; p++)
                CHECK(curves[c]->pts[p].input - curves[c]->pts[p - 1].input >= .5f / 255);
        }
        output(&parsed, sample, got);
        for (size_t c = 0; c < 3; c++)
            CHECK(fabsf(got[c] - cases[i].expected) < .001f);
        pc_adjustment_fini(&resolved);
        pc_adjustment_fini(&parsed);
    }
}
static void test_endpoint_fallback_retains_dense_curve_edits(void)
{
    const char *base = "{\"points\":[[0,0],[64,50],[128,100],[192,200],[255,255]],\"red\":[[0,0],[64,50],["
                       "128,100],[192,200],[255,255]],\"green\":[[0,0],[64,50],[128,100],[192,200],[255,255]]"
                       ",\"blue\":[[0,0],[64,50],[128,100],[192,200],[255,255]]}";
    for (unsigned black = 0; black < 2; black++) {
        float value = black ? .995f : .005f, sample[3] = {value, value, value}, target = black ? 0 : 1;
        pc_adjustment resolved = {0}, parsed = {0};
        CHECK(pick("curves", base, black ? PC_TONE_PICK_BLACK : PC_TONE_PICK_WHITE, sample, PC_COLOR_MODE_RGB,
                   &resolved) == PC_OK);
        CHECK(roundtrip("curves", &resolved, PC_COLOR_MODE_RGB, &parsed) == PC_OK);
        const float inputs[] = {0, 64, 128, 192, 255};
        for (size_t c = 0; c < 3; c++) {
            const pc_curve *curve = &parsed.u.curves.per_channel[c];
            CHECK(curve->len == 5);
            if (curve->len != 5)
                continue;
            for (size_t p = 0; p < 5; p++)
                CHECK(fabsf(curve->pts[p].input * 255 - inputs[p]) < .001f);
            size_t start = black ? 3 : 0;
            CHECK(curve->pts[start].output == target && curve->pts[start + 1].output == target);
        }
        float got[3];
        output(&parsed, sample, got);
        for (size_t c = 0; c < 3; c++)
            CHECK(fabsf(got[c] - target) < .01f);
        pc_adjustment_fini(&resolved);
        pc_adjustment_fini(&parsed);
    }
}
static void test_gray_preserves_master_and_luma_including_tight_points(void)
{
    const char *base = "{\"points\":[[0,0],[96,140],[255,255]],\"red\":[[0,0],[80,95],[255,255]]}";
    const float samples[][3] = {
        {.88f, .76f, .63f}, {.17f, .105f, .075f}, {.004f, .006f, .008f}, {.992f, .996f, .999f}, {0, 0, 0},
        {1, 1, 1}};
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++) {
        pc_adjustment resolved = {0}, parsed = {0};
        CHECK(pick("curves", i < 2 ? base : "{}", PC_TONE_PICK_GRAY, samples[i], PC_COLOR_MODE_RGB,
                   &resolved) == PC_OK);
        CHECK(roundtrip("curves", &resolved, PC_COLOR_MODE_RGB, &parsed) == PC_OK);
        if (i < 2)
            CHECK(parsed.u.curves.master.len == 3 &&
                  fabsf(parsed.u.curves.master.pts[1].output - 140.f / 255) < 1e-6f);
        float got[3];
        output(&parsed, samples[i], got);
        CHECK(fabsf(got[0] - got[1]) < .002f && fabsf(got[1] - got[2]) < .002f);
        CHECK(fabsf(luma(got) - luma(samples[i])) < .003f);
        pc_adjustment_fini(&resolved);
        pc_adjustment_fini(&parsed);
    }
    const char *tight = "{\"red\":[[0,0],[100,100],[100.51,100],[255,255]],\"green\":[[0,0],[100,100],[100."
                        "51,100],[255,255]],\"blue\":[[0,0],[100,100],[100.51,100],[255,255]]}";
    const float sample[3] = {100.25f / 255, 100.1f / 255, 100.4f / 255};
    pc_adjustment resolved = {0}, parsed = {0};
    CHECK(pick("curves", tight, PC_TONE_PICK_GRAY, sample, PC_COLOR_MODE_RGB, &resolved) == PC_OK);
    CHECK(roundtrip("curves", &resolved, PC_COLOR_MODE_RGB, &parsed) == PC_OK);
    for (size_t c = 0; c < 3; c++) {
        const pc_curve *curve = &parsed.u.curves.per_channel[c];
        CHECK(curve->len == 4);
        bool plateau = false;
        for (size_t p = 1; p < curve->len; p++) {
            CHECK(curve->pts[p].input - curve->pts[p - 1].input >= .5f / 255);
            if (curve->pts[p - 1].input <= sample[c] && curve->pts[p].input >= sample[c]) {
                plateau = fabsf(curve->pts[p].output - curve->pts[p - 1].output) < 1e-6f &&
                          fabsf(curve->pts[p].output - luma(sample)) < .001f;
            }
        }
        CHECK(plateau);
    }
    pc_adjustment_fini(&resolved);
    pc_adjustment_fini(&parsed);
}
static void pixel(pc_engine_session *s, float out[4])
{
    pc_compose_buffer buffer = {0};
    CHECK(pc_compose_render(pc_doc_arc_get(pc_engine_session_active(s)->doc), pc_rect_make(2, 2, 3, 3),
                            &buffer, NULL) == PC_OK);
    if (buffer.len)
        memcpy(out, buffer.px[0], 4 * sizeof *out);
    pc_compose_buffer_fini(&buffer);
}
static void test_levels_curves_command_layer_and_smart_targets(void)
{
    const char *kinds[] = {"levels", "curves"};
    const unsigned depths[] = {8, 16, 32};
    for (size_t k = 0; k < 2; k++)
        for (size_t d = 0; d < 3; d++)
            for (unsigned target = 0; target < 3; target++) {
                pc_engine_session *s = NULL;
                CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
                char text[180], id[100];
                (void)snprintf(text, sizeof text,
                               "{\"width\":8,\"height\":8,\"depth\":%u,\"background\":\"transparent\"}",
                               depths[d]);
                CHECK(command(s, "file.new", text) == PC_OK);
                CHECK(command(s, "edit.fill", "{\"color\":[0.6,0.5,0.4]}") == PC_OK);
                if (target == 1) {
                    (void)snprintf(id, sizeof id, "layer.newAdjustmentLayer.%s", kinds[k]);
                    CHECK(command(s, id, "{}") == PC_OK);
                }
                if (target == 2)
                    CHECK(command(s, "layer.smartObjects.convertToSmartObject", "{}") == PC_OK);
                const pc_engine_doc_state *state = pc_engine_session_active(s);
                size_t history = pc_history_past_len(state->history);
                (void)snprintf(id, sizeof id, target == 1 ? "layer.setAdjustment" : "image.adjustments.%s",
                               kinds[k]);
                CHECK(command(s, id, "{\"eyedropper\":{\"point\":\"white\",\"at\":[2,2]}} ") == PC_OK);
                CHECK(pc_history_past_len(state->history) == history + 1);
                float got[4];
                pixel(s, got);
                CHECK(got[0] > .99f && got[1] > .99f && got[2] > .99f);
                if (target == 1) {
                    CHECK(command(s, id, "{\"eyedropper\":{\"point\":\"white\",\"at\":[2,2]}}") == PC_OK);
                    pixel(s, got);
                    CHECK(got[0] > .99f && got[1] > .99f && got[2] > .99f);
                }
                if (target == 2) {
                    CHECK(command(s, "layer.smartObjects.updateChanged", "{}") == PC_OK);
                    pixel(s, got);
                    CHECK(got[0] > .99f && got[1] > .99f && got[2] > .99f);
                }
                pc_engine_session_free(s);
            }
}
static void test_modes_transparency_and_bad_samples_leave_document(void)
{
    const float sample[3] = {.4f, .5f, .6f};
    pc_adjustment out = {0};
    CHECK(pick("levels", "{}", PC_TONE_PICK_BLACK, sample, PC_COLOR_MODE_GRAYSCALE, &out) == PC_OK);
    pc_adjustment_fini(&out);
    CHECK(pick("curves", "{}", PC_TONE_PICK_WHITE, sample, PC_COLOR_MODE_GRAYSCALE, &out) == PC_OK);
    pc_adjustment_fini(&out);
    CHECK(pick("curves", "{}", PC_TONE_PICK_GRAY, sample, PC_COLOR_MODE_GRAYSCALE, &out) == PC_ERR_INVALID);
    CHECK(pick("levels", "{}", PC_TONE_PICK_WHITE, sample, PC_COLOR_MODE_CMYK, &out) == PC_ERR_INVALID);
    CHECK(pick("curves", "{}", PC_TONE_PICK_WHITE, sample, PC_COLOR_MODE_LAB, &out) == PC_ERR_INVALID);
    const float bad[3] = {NAN, 0, 0};
    CHECK(pick("curves", "{}", PC_TONE_PICK_BLACK, bad, PC_COLOR_MODE_RGB, &out) == PC_ERR_INVALID);
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(command(s, "file.new", "{\"width\":8,\"height\":8,\"background\":\"transparent\"}") == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    const pc_doc_arc *before = state->doc;
    size_t history = pc_history_past_len(state->history);
    const char *params[] = {"{\"eyedropper\":{\"point\":\"white\",\"at\":[2,2]}}",
                            "{\"eyedropper\":{\"point\":\"bogus\",\"color\":[0,0,0]}}",
                            "{\"eyedropper\":{\"point\":\"white\",\"at\":[-1,0]}}", "{\"eyedropper\":null}",
                            "{\"eyedropper\":{\"point\":\"gray\",\"color\":[1,2]}}"};
    for (size_t i = 0; i < sizeof params / sizeof *params; i++) {
        CHECK(command(s, "image.adjustments.curves", params[i]) == PC_ERR_INVALID);
        CHECK(state->doc == before && pc_history_past_len(state->history) == history);
    }
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_endpoint_serialization_boundaries);
    RUN(test_endpoint_fallback_retains_dense_curve_edits);
    RUN(test_gray_preserves_master_and_luma_including_tight_points);
    RUN(test_levels_curves_command_layer_and_smart_targets);
    RUN(test_modes_transparency_and_bad_samples_leave_document);
    return pc_t_finish();
}
