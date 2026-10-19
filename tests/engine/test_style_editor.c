#include "harness.h"
#include "engine/style_editor.h"
#include "engine/engine_internal.h"
#include <string.h>
#include <math.h>
#include "cms/synth.h"
#include "color/convert.h"
static pc_status ex(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error error = {0};
    pc_status status = pc_engine_session_execute_json(s, id, json, &out, &error);
    if (status != PC_OK)
        fprintf(stderr, "%s: %s\n", id, error.msg);
    pc_json_value_fini(&out);
    return status;
}
static pc_engine_session *fresh(const char *mode, unsigned depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char json[192];
    (void)snprintf(json, sizeof json,
                   "{\"width\":40,\"height\":32,\"mode\":\"%s\",\"depth\":%u,\"background\":\"transparent\"}",
                   mode, depth);
    CHECK(ex(s, "file.new", json) == PC_OK);
    return s;
}
static void test_style_snapshot_without_deltas_preserves_every_mode_and_depth(void)
{
    const char *modes[] = {"rgb", "grayscale", "cmyk", "lab"};
    const unsigned depths[] = {8, 16, 32};
    for (size_t m = 0; m < 4; m++)
        for (size_t z = 0; z < 3; z++) {
            pc_engine_session *s = fresh(modes[m], depths[z]);
            CHECK(ex(s, "layer.layerStyle.dropShadow",
                     "{\"color\":[0.12345,0.45678,0.78912],\"opacity\":43,\"size\":7,\"noise\":13}") ==
                  PC_OK);
            CHECK(ex(s, "layer.layerStyle.innerGlow", "{\"color\":[0.8,0.1,0.4],\"size\":13,\"range\":63}") ==
                  PC_OK);
            const pc_engine_doc_state *state = pc_engine_session_active(s);
            pc_doc_arc *before = pc_doc_arc_retain(state->doc);
            size_t history = pc_history_past_len(state->history), journal = s->njournal;
            yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
            yyjson_mut_val *root = NULL;
            CHECK(pc_engine_style_editor_initial(s, NULL, d, &root, NULL) == PC_OK);
            CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(root, "effects")) == 2);
            CHECK(yyjson_mut_obj_size(yyjson_mut_obj_get(root, "blending")) == 0);
            pc_buf json = PC_BUF_INIT;
            CHECK(pc_json_write(root, 0, &json, NULL) == PC_OK);
            CHECK(ex(s, "layer.layerStyle.edit", pc_buf_cstr(&json)) == PC_OK);
            state = pc_engine_session_active(s);
            CHECK(pc_document_eq(pc_doc_arc_get(before), pc_doc_arc_get(state->doc)));
            CHECK(pc_history_past_len(state->history) == history + 1);
            CHECK(s->njournal == journal + 1);
            CHECK_EQ_STR(s->journal[journal].id, "layer.layerStyle.edit");
            CHECK(pc_engine_session_undo(s));
            CHECK(pc_document_eq(pc_doc_arc_get(before), pc_doc_arc_get(pc_engine_session_active(s)->doc)));
            pc_buf_fini(&json);
            yyjson_mut_doc_free(d);
            pc_doc_arc_release(before);
            pc_engine_session_free(s);
        }
}
static pc_status imported_metadata(pc_document *doc, bool *has, pc_layer_id *active, void *context,
                                   pc_error *err)
{
    (void)has;
    (void)context;
    pc_layer *layer = pc_document_layer_mut(doc, *active);
    layer->effects.enabled = false;
    layer->effects.has_reference = true;
    layer->effects.reference[0] = 12.5;
    layer->effects.reference[1] = 19.75;
    return pc_blob_new("preserved-fx-block", 18, &layer->effects.psd_raw, err);
}
static void test_imported_effect_metadata_and_pattern_snapshot_survive(void)
{
    pc_engine_session *s = fresh("cmyk", 16);
    CHECK(ex(s, "layer.layerStyle.patternOverlay", "{\"pattern\":\"Bricks\",\"scale\":37,\"opacity\":62}") ==
          PC_OK);
    CHECK(pc_engine_session_edit(s, "Imported metadata", imported_metadata, NULL, NULL) == PC_OK);
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_engine_style_editor_initial(s, NULL, d, &root, NULL) == PC_OK);
    pc_buf json = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &json, NULL) == PC_OK);
    CHECK(ex(s, "layer.layerStyle.edit", pc_buf_cstr(&json)) == PC_OK);
    CHECK(pc_document_eq(pc_doc_arc_get(before), pc_doc_arc_get(pc_engine_session_active(s)->doc)));
    CHECK(pc_engine_session_undo(s));
    CHECK(pc_document_eq(pc_doc_arc_get(before), pc_doc_arc_get(pc_engine_session_active(s)->doc)));
    pc_doc_arc_release(before);
    pc_buf_fini(&json);
    yyjson_mut_doc_free(d);
    pc_engine_session_free(s);
}
static void test_atomic_blending_effects_light_and_rollback(void)
{
    pc_engine_session *s = fresh("rgb", 8);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    pc_doc_arc *before = pc_doc_arc_retain(state->doc);
    size_t history = pc_history_past_len(state->history), journal = s->njournal;
    CHECK(ex(s, "layer.layerStyle.edit",
             "{\"blending\":{\"opacity\":42,\"blend\":\"multiply\"},\"effects\":[{\"kind\":\"dropShadow\","
             "\"on\":false,\"params\":{\"size\":13}}],\"globalLight\":{\"angle\":33}}") == PC_OK);
    state = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(state->doc);
    const pc_layer *layer = pc_document_layer(doc, state->active_layer);
    CHECK(layer->opacity == .42f && layer->blend == PC_BLEND_MULTIPLY);
    CHECK(layer->effects.nitems == 1 && !pc_effect_enabled(&layer->effects.items[0]) &&
          layer->effects.items[0].u.shadow.size == 13);
    CHECK(doc->global_light.angle == 33);
    CHECK(pc_history_past_len(state->history) == history + 1);
    CHECK(s->njournal == journal + 1);
    CHECK(pc_engine_session_undo(s));
    CHECK(pc_document_eq(pc_doc_arc_get(before), pc_doc_arc_get(pc_engine_session_active(s)->doc)));
    history = pc_history_past_len(pc_engine_session_active(s)->history);
    journal = s->njournal;
    CHECK(ex(s, "layer.layerStyle.edit",
             "{\"blending\":{\"opacity\":42},\"effects\":[{\"kind\":\"unknown\"}]}") != PC_OK);
    CHECK(pc_document_eq(pc_doc_arc_get(before), pc_doc_arc_get(pc_engine_session_active(s)->doc)));
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == history);
    CHECK(s->njournal == journal);
    pc_doc_arc_release(before);
    pc_engine_session_free(s);
}
static void test_factories_and_hostile_groups(void)
{
    pc_engine_session *s = fresh("rgb", 16);
    const char *kinds[] = {"dropShadow",   "innerShadow",     "outerGlow",      "innerGlow", "stroke",
                           "colorOverlay", "gradientOverlay", "patternOverlay", "satin",     "bevelEmboss"};
    for (size_t i = 0; i < 10; i++) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = NULL;
        CHECK(pc_engine_style_editor_initial(s, kinds[i], doc, &root, NULL) == PC_OK);
        CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(root, "effects")) == 1);
        CHECK(yyjson_mut_obj_size(yyjson_mut_obj_get(
                  yyjson_mut_arr_get(yyjson_mut_obj_get(root, "effects"), 0), "params")) == 0);
        yyjson_mut_doc_free(doc);
    }
    const char *bad[] = {"null",
                         "[]",
                         "{\"effects\":1}",
                         "{\"effects\":[],\"blending\":false}",
                         "{\"effects\":[null]}",
                         "{\"effects\":[{\"kind\":\"satin\",\"params\":false}]}",
                         "{\"effects\":[],\"globalLight\":[]} "};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(ex(s, "layer.layerStyle.edit", bad[i]) != PC_OK);
    pc_engine_session_free(s);
}
static pc_status attach_profile(pc_document *doc, bool *has, pc_layer_id *active, void *context,
                                pc_error *err)
{
    (void)has;
    (void)active;
    const pc_buf *bytes = context;
    pc_blob *blob = NULL;
    pc_status status = pc_blob_new(bytes->data, bytes->len, &blob, err);
    if (status == PC_OK) {
        pc_blob_release(doc->icc_profile);
        doc->icc_profile = blob;
    }
    return status;
}
static void test_style_colors_and_display_use_the_document_cmyk_profile(void)
{
    pc_cmyk_params params = pc_cmyk_params_default();
    params.tac = 1;
    params.k_max = .3;
    params.grid_a2b = 3;
    params.grid_b2a = 3;
    pc_profile profile = {0};
    CHECK(pc_synth_cmyk_profile(&params, &profile, NULL) == PC_OK);
    pc_buf bytes = PC_BUF_INIT;
    CHECK(pc_profile_to_bytes(&profile, &bytes, NULL) == PC_OK);
    pc_engine_session *s = fresh("cmyk", 16);
    CHECK(pc_engine_session_edit(s, "Profile", attach_profile, &bytes, NULL) == PC_OK);
    pc_cmyk_space *space = pc_cmyk_space_for_profile(bytes.data, bytes.len);
    CHECK(space != NULL);
    pc_cmyk_space *previous = pc_cmyk_space_active(), *token = pc_cmyk_space_enter(space);
    float rgb[3] = {.2f, .4f, .7f}, expected[4], display[3];
    pc_rgb_to_cmyk(rgb, expected);
    pc_cmyk_to_rgb(expected, display);
    pc_cmyk_space_leave(token);
    CHECK(ex(s, "layer.layerStyle.colorOverlay", "{\"color\":[0.2,0.4,0.7]}") == PC_OK);
    CHECK(pc_cmyk_space_active() == previous);
    const pc_engine_doc_state *state = pc_engine_session_active(s);
    const pc_layer *layer = pc_document_layer(pc_doc_arc_get(state->doc), state->active_layer);
    const pc_color *actual = &layer->effects.items[0].u.color_overlay.color;
    CHECK(actual->mode == PC_COLOR_MODE_CMYK);
    for (size_t i = 0; i < 4; i++)
        CHECK(fabsf(actual->c[i] - expected[i]) < 1e-6f);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_engine_style_editor_initial(s, NULL, d, &root, NULL) == PC_OK);
    yyjson_mut_val *values = yyjson_mut_obj_get(yyjson_mut_arr_get(yyjson_mut_obj_get(root, "effects"), 0),
                                                "values"),
                   *color = yyjson_mut_obj_get(values, "color");
    for (size_t i = 0; i < 3; i++)
        CHECK(fabs(yyjson_mut_get_num(yyjson_mut_arr_get(color, i)) - display[i]) < 1e-6);
    CHECK(pc_cmyk_space_active() == previous);
    yyjson_mut_doc_free(d);
    pc_engine_session_free(s);
    pc_cmyk_space_release(space);
    pc_buf_fini(&bytes);
    pc_profile_fini(&profile);
}
static void test_user_defaults_without_a_document_and_contour_sampling(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.layerStyle.defaultFor", "{\"kind\":\"dropShadow\"}", &out,
                                         NULL) == PC_OK);
    CHECK(yyjson_mut_is_false(yyjson_mut_obj_get(pc_json_value_root(&out), "user")));
    CHECK(yyjson_mut_is_obj(yyjson_mut_obj_get(pc_json_value_root(&out), "params")));
    pc_json_value_fini(&out);
    CHECK(
        ex(s, "layer.layerStyle.makeDefault",
           "{\"kind\":\"dropShadow\",\"params\":{\"size\":27,\"color\":\"#22cc55\",\"contour\":\"Cone\"}}") ==
        PC_OK);
    CHECK(pc_engine_session_execute_json(s, "layer.layerStyle.defaultFor", "{\"kind\":\"dropShadow\"}", &out,
                                         NULL) == PC_OK);
    CHECK(yyjson_mut_is_true(yyjson_mut_obj_get(pc_json_value_root(&out), "user")));
    CHECK(yyjson_mut_get_num(
              yyjson_mut_obj_get(yyjson_mut_obj_get(pc_json_value_root(&out), "params"), "size")) == 27);
    pc_json_value_fini(&out);
    CHECK(ex(s, "layer.layerStyle.makeDefault", "{\"kind\":\"dropShadow\",\"params\":null}") ==
          PC_ERR_INVALID);
    CHECK(ex(s, "layer.layerStyle.defaultFor", "{\"kind\":\"unknown\"}") == PC_ERR_INVALID);
    CHECK(ex(s, "layer.layerStyle.defaultFor", "{\"kind\":\"dropShadow\\u0000bad\"}") == PC_ERR_INVALID);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *levels = NULL, *corners = NULL;
    float samples[256];
    CHECK(pc_engine_style_contour_view(NULL, d, &levels, &corners, samples, NULL) == PC_OK);
    CHECK(yyjson_mut_arr_size(levels) == 2);
    for (size_t i = 0; i < 256; i++)
        CHECK(fabsf(samples[i] - (float)i / 255) < 1e-6f);
    yyjson_doc *param = NULL;
    CHECK(pc_json_parse("\"Cone\"", 6, &param, NULL) == PC_OK);
    CHECK(pc_engine_style_contour_view(yyjson_doc_get_root(param), d, &levels, &corners, samples, NULL) ==
          PC_OK);
    CHECK(yyjson_mut_arr_size(levels) >= 3);
    CHECK(samples[0] < .01f && samples[128] > .98f && samples[255] < .01f);
    yyjson_doc_free(param);
    yyjson_mut_doc_free(d);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_user_defaults_without_a_document_and_contour_sampling);
    RUN(test_style_colors_and_display_use_the_document_cmyk_profile);
    RUN(test_imported_effect_metadata_and_pattern_snapshot_survive);
    RUN(test_style_snapshot_without_deltas_preserves_every_mode_and_depth);
    RUN(test_atomic_blending_effects_light_and_rollback);
    RUN(test_factories_and_hostile_groups);
    return pc_t_finish();
}
