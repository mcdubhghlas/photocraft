/* Window > Layer Comps. Port of the layerComp.* tests of crates/engine/src/comps_cmds/tests.rs
 * (E12). Position-capture via layer.translate and the file-export command are exercised by the
 * owning chunks; here comps are driven through layer.setProps (visibility/appearance). */
#include "doc/comps.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status ex(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, NULL);
    pc_json_value_fini(&o);
    return rc;
}

static uint64_t exu(pc_engine_session *s, const char *id, const char *p, const char *key)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, id, p, &o, NULL) == PC_OK);
    uint64_t v = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&o), key));
    pc_json_value_fini(&o);
    return v;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

/* a doc with two extra raster layers A (bottom) and B (top); returns their ids. */
static pc_engine_session *setup(pc_layer_id *a, pc_layer_id *b)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":64,\"height\":48}") == PC_OK);
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    *a = pc_engine_session_active(s)->active_layer;
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    *b = pc_engine_session_active(s)->active_layer;
    return s;
}

static bool visible(pc_engine_session *s, pc_layer_id id)
{
    return pc_document_layer(doc_of(s), id)->visible;
}

static void test_manage_and_cycle(void)
{
    pc_layer_id a, b;
    pc_engine_session *s = setup(&a, &b);
    char buf[128];
    uint64_t c1 = exu(s, "layerComp.new", "{\"name\":\"One\",\"comment\":\"first\"}", "comp");
    /* hide B */
    snprintf(buf, sizeof buf, "{\"layer\":%llu,\"visible\":false}", (unsigned long long)b);
    CHECK(ex(s, "layer.setProps", buf) == PC_OK);
    uint64_t c2 = exu(s, "layerComp.new", "{\"name\":\"Two\"}", "comp");
    snprintf(buf, sizeof buf, "{\"comp\":%llu}", (unsigned long long)c1);
    uint64_t c3 = exu(s, "layerComp.duplicate", buf, "comp");
    const pc_document *doc = doc_of(s);
    CHECK_EQ_UINT(doc->nlayer_comps, 3u);
    CHECK_EQ_STR(pc_str_c(&doc->layer_comps[0].name), "One");
    CHECK_EQ_STR(pc_str_c(&doc->layer_comps[1].name), "One copy");
    CHECK_EQ_STR(pc_str_c(&doc->layer_comps[2].name), "Two");

    snprintf(buf, sizeof buf, "{\"comp\":%llu,\"name\":\"Three\"}", (unsigned long long)c3);
    CHECK(ex(s, "layerComp.rename", buf) == PC_OK);
    CHECK(ex(s, "layerComp.setComment", "{\"comp\":\"Three\",\"comment\":\"dup\"}") == PC_OK);
    CHECK_EQ_STR(pc_str_c(&pc_document_comp(doc_of(s), (uint32_t)c3)->comment), "dup");

    /* cycle: apply Two (B hidden), next -> One (B visible), previous -> Two */
    snprintf(buf, sizeof buf, "{\"comp\":%llu}", (unsigned long long)c2);
    CHECK(ex(s, "layerComp.apply", buf) == PC_OK);
    CHECK(ex(s, "layerComp.next", "{}") == PC_OK);
    CHECK(doc_of(s)->last_applied_comp == (uint32_t)c1);
    CHECK(visible(s, b));
    CHECK(ex(s, "layerComp.previous", "{}") == PC_OK);
    CHECK(doc_of(s)->last_applied_comp == (uint32_t)c2);
    CHECK(!visible(s, b));

    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layerComp.list", "{}", &o, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&o), "comps")), 3u);
    pc_json_value_fini(&o);

    /* update visibility on One: now records B hidden */
    snprintf(buf, sizeof buf, "{\"comp\":%llu,\"what\":\"visibility\"}", (unsigned long long)c1);
    CHECK(ex(s, "layerComp.update", buf) == PC_OK);
    const pc_comp_layer_state *st = pc_layer_comp_state(pc_document_comp(doc_of(s), (uint32_t)c1), b);
    CHECK(st != NULL && st->has_visible && st->visible == false);
    CHECK(ex(s, "layerComp.update", "{\"comp\":\"*\"}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_restore_last_document_state(void)
{
    pc_layer_id a, b;
    pc_engine_session *s = setup(&a, &b);
    CHECK(ex(s, "layerComp.new", "{}") == PC_OK);
    CHECK_EQ_STR(pc_str_c(&doc_of(s)->layer_comps[0].name), "Layer Comp 1");
    /* edit B hidden, then apply the comp (restores B visible), remembering the edited state */
    char buf[96];
    snprintf(buf, sizeof buf, "{\"layer\":%llu,\"visible\":false}", (unsigned long long)b);
    CHECK(ex(s, "layer.setProps", buf) == PC_OK);
    CHECK(ex(s, "layerComp.apply", "{}") == PC_OK);
    CHECK(visible(s, b));
    CHECK(doc_of(s)->has_last_document_state);
    CHECK(pc_engine_session_is_enabled(s, "layerComp.restoreLastDocumentState"));
    CHECK(ex(s, "layerComp.restoreLastDocumentState", "{}") == PC_OK);
    CHECK(!visible(s, b)); /* edited state restored */
    CHECK(!doc_of(s)->has_last_applied_comp);
    pc_engine_session_free(s);
}

static void test_warnings(void)
{
    pc_layer_id a, b;
    pc_engine_session *s = setup(&a, &b);
    CHECK(ex(s, "layerComp.new", "{}") == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layerComp.updateWarnings", "{}", &o, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&o), "count")), 0u);
    pc_json_value_fini(&o);
    /* delete layer A */
    char buf[64];
    snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)a);
    CHECK(ex(s, "layer.delete", buf) == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "layerComp.updateWarnings", "{}", &o, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&o), "count")), 1u);
    pc_json_value_fini(&o);
    /* apply reports missing */
    CHECK_EQ_UINT(exu(s, "layerComp.apply", "{}", "missingLayers"), 1u);
    CHECK(ex(s, "layerComp.updateWarnings", "{\"clear\":true}") == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "layerComp.updateWarnings", "{}", &o, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&o), "count")), 0u);
    pc_json_value_fini(&o);
    pc_engine_session_free(s);
}

static void test_disabled_and_bad_params(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "layerComp.new"));
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "layerComp.new"));
    const char *disabled[] = {"layerComp.apply", "layerComp.update", "layerComp.next",
                              "layerComp.delete", "layerComp.restoreLastDocumentState"};
    for (size_t i = 0; i < sizeof disabled / sizeof disabled[0]; i++) {
        CHECK(!pc_engine_session_is_enabled(s, disabled[i]));
    }
    CHECK(ex(s, "layerComp.new", "{}") == PC_OK);
    CHECK(ex(s, "layerComp.apply", "{\"comp\":999}") != PC_OK);
    CHECK(ex(s, "layerComp.apply", "{\"comp\":\"nope\"}") != PC_OK);
    CHECK(ex(s, "layerComp.update", "{\"what\":\"colour\"}") != PC_OK);
    CHECK(ex(s, "layerComp.rename", "{}") != PC_OK);
    CHECK(ex(s, "layerComp.setOptions", "{}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_out_of_range_ids(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":16,\"height\":16}") == PC_OK);
    uint64_t c = exu(s, "layerComp.new", "{\"name\":\"Start\"}", "comp");
    uint64_t wrapped = (1ull << 32) + c;
    char buf[64];
    snprintf(buf, sizeof buf, "{\"comp\":%llu}", (unsigned long long)wrapped);
    CHECK(ex(s, "layerComp.delete", buf) != PC_OK);
    snprintf(buf, sizeof buf, "{\"comp\":%llu,\"name\":\"renamed\"}", (unsigned long long)wrapped);
    CHECK(ex(s, "layerComp.setOptions", buf) != PC_OK);
    CHECK_EQ_UINT(doc_of(s)->nlayer_comps, 1u);
    CHECK_EQ_STR(pc_str_c(&doc_of(s)->layer_comps[0].name), "Start");
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_layer_id a, b;
    pc_engine_session *s = setup(&a, &b);
    (void)ex(s, "layerComp.new", "{}");
    const char *ids[] = {"layerComp.new",     "layerComp.update",   "layerComp.apply",
                         "layerComp.previous", "layerComp.next",     "layerComp.delete",
                         "layerComp.duplicate", "layerComp.rename",  "layerComp.setComment",
                         "layerComp.setOptions", "layerComp.restoreLastDocumentState",
                         "layerComp.updateWarnings", "layerComp.list"};
    const char *ps[] = {"{}", "[]", "null", "123", "{\"comp\":-1}", "{\"comp\":1.5}",
                        "{\"comp\":true}", "{\"comp\":[1]}", "{\"what\":42}", "{\"name\":5}"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof ps / sizeof ps[0]; j++) {
            (void)ex(s, ids[i], ps[j]); /* must not crash */
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_manage_and_cycle);
    RUN(test_restore_last_document_state);
    RUN(test_warnings);
    RUN(test_disabled_and_bad_params);
    RUN(test_out_of_range_ids);
    RUN(test_adversarial);
    return pc_t_finish();
}
