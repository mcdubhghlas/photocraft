/* Live Gradient Fill layers + gradient.presets.apply (port of a subset, E10). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status run(pc_engine_session *s, const char *id, const char *json, pc_json_value *out)
{
    pc_error e = {0};
    pc_json_value local = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, out ? out : &local, &e);
    if (out == NULL) {
        pc_json_value_fini(&local);
    }
    return rc;
}

static void test_create_and_get(void)
{
    pc_engine_session *s = mk(40, 20);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "gradient.fill.create",
              "{\"from\":[0,0],\"to\":[40,0],\"stops\":[[0,\"#000000\"],[1,\"#ffffff\"]]}", &o) == PC_OK);
    uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&o), "layer"));
    CHECK(id != 0);
    pc_json_value_fini(&o);
    /* it is a gradient fill layer */
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), id);
    CHECK(l != NULL && l->content.kind == PC_LAYER_FILL && l->content.u.fill.kind == PC_FILL_GRADIENT);
    /* get reports style/stops */
    pc_json_value g = PC_JSON_VALUE_INIT;
    char gj[64];
    snprintf(gj, sizeof gj, "{\"layer\":%llu}", (unsigned long long)id);
    CHECK(run(s, "gradient.fill.get", gj, &g) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&g);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "style")), "linear");
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(root, "stops")) == 2);
    pc_json_value_fini(&g);
    pc_engine_session_free(s);
}

static void test_set_and_stop(void)
{
    pc_engine_session *s = mk(40, 20);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "gradient.fill.create", "{\"from\":[0,0],\"to\":[40,0]}", &o) == PC_OK);
    uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&o), "layer"));
    pc_json_value_fini(&o);
    char base[64];
    snprintf(base, sizeof base, "%llu", (unsigned long long)id);
    /* set style=radial, scale=50 */
    char sj[128];
    snprintf(sj, sizeof sj, "{\"layer\":%s,\"style\":\"radial\",\"scale\":50}", base);
    pc_json_value o2 = PC_JSON_VALUE_INIT;
    CHECK(run(s, "gradient.fill.set", sj, &o2) == PC_OK);
    yyjson_mut_val *r2 = pc_json_value_root(&o2);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(r2, "style")), "radial");
    CHECK_NEAR((float)yyjson_mut_get_real(yyjson_mut_obj_get(r2, "scale")), 50.0f, 0.5f);
    pc_json_value_fini(&o2);
    /* add a colour stop in the middle */
    char aj[128];
    snprintf(aj, sizeof aj, "{\"layer\":%s,\"action\":\"add\",\"location\":0.5,\"color\":\"#ff0000\"}", base);
    pc_json_value o3 = PC_JSON_VALUE_INIT;
    CHECK(run(s, "gradient.fill.stop", aj, &o3) == PC_OK);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&o3), "stops")) == 3);
    pc_json_value_fini(&o3);
    /* delete it */
    char dj[128];
    snprintf(dj, sizeof dj, "{\"layer\":%s,\"action\":\"delete\",\"index\":1}", base);
    pc_json_value o4 = PC_JSON_VALUE_INIT;
    CHECK(run(s, "gradient.fill.stop", dj, &o4) == PC_OK);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&o4), "stops")) == 2);
    pc_json_value_fini(&o4);
    /* keeping at least 2 stops */
    CHECK(run(s, "gradient.fill.stop", dj, NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_presets_apply(void)
{
    pc_engine_session *s = mk(40, 20);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(run(s, "gradient.presets.apply", "{\"gradient\":\"Black, White\",\"angle\":0}", &o) == PC_OK);
    uint64_t id = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&o), "layer"));
    CHECK(id != 0);
    CHECK(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&o), "preset")) != NULL);
    pc_json_value_fini(&o);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(d->doc), id);
    CHECK(l != NULL && l->content.kind == PC_LAYER_FILL && l->content.u.fill.kind == PC_FILL_GRADIENT);
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk(16, 16);
    /* create one to exercise set/stop/get adversarially */
    pc_json_value o = PC_JSON_VALUE_INIT;
    (void)run(s, "gradient.fill.create", "{\"from\":[0,0],\"to\":[16,0]}", &o);
    pc_json_value_fini(&o);
    const char *cmds[] = {"gradient.fill.create", "gradient.fill.set", "gradient.fill.stop",
                          "gradient.fill.get", "gradient.presets.apply"};
    const char *params[] = {"{}", "[]", "null", "\"x\"", "{\"from\":[0,0]}", "{\"stops\":[[0.5,\"#zz\"]]}",
                            "{\"action\":\"bogus\"}", "{\"action\":\"delete\",\"index\":999}",
                            "{\"scale\":-5}", "{\"style\":7}", "{\"gradient\":\"nope\"}",
                            "{\"midpoints\":[2,2,2,2,2]}", "{\"action\":\"add\",\"location\":1e308}"};
    for (size_t c = 0; c < sizeof cmds / sizeof cmds[0]; c++) {
        for (size_t pi = 0; pi < sizeof params / sizeof params[0]; pi++) {
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error e = {0};
            (void)pc_engine_session_execute_json(s, cmds[c], params[pi], &out, &e);
            pc_json_value_fini(&out);
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_create_and_get);
    RUN(test_set_and_stop);
    RUN(test_presets_apply);
    RUN(test_adversarial);
    return pc_t_finish();
}
