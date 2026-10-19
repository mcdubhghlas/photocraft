/* Image > Variables and Data Sets. Port of crates/engine/src/variables_cmds/tests.rs (E12). */
#include <stdio.h>
#include <string.h>

#include "doc/doc.h"
#include "doc/text.h"
#include "doc/variables.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status ex(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, NULL);
    pc_json_value_fini(&o);
    return rc;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

static bool visible(pc_engine_session *s, pc_layer_id id)
{
    return pc_document_layer(doc_of(s), id)->visible;
}

static const char *text_of(pc_engine_session *s, pc_layer_id id)
{
    const pc_layer *l = pc_document_layer(doc_of(s), id);
    CHECK(l != NULL && l->content.kind == PC_LAYER_TEXT);
    return pc_str_c(&l->content.u.text.text);
}

static void test_define_apply_visibility_text(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":64,\"height\":48}") == PC_OK);
    /* badge = pixel layer */
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    pc_layer_id badge = pc_engine_session_active(s)->active_layer;
    /* title = text layer */
    CHECK(ex(s, "type.create", "{\"x\":4,\"y\":20,\"text\":\"Old\"}") == PC_OK);
    pc_layer_id title = pc_engine_session_active(s)->active_layer;
    CHECK(pc_document_layer(doc_of(s), title)->content.kind == PC_LAYER_TEXT);

    char buf[256];
    snprintf(buf, sizeof buf,
             "{\"defs\":[{\"name\":\"showBadge\",\"layer\":%llu,\"type\":\"visibility\"},"
             "{\"name\":\"headline\",\"layer\":%llu,\"type\":\"textReplacement\"}]}",
             (unsigned long long)badge, (unsigned long long)title);
    CHECK(ex(s, "image.variables.define", buf) == PC_OK);
    CHECK(ex(s, "image.variables.dataSets",
             "{\"dataSets\":[{\"name\":\"A\",\"values\":["
             "{\"variable\":\"showBadge\",\"kind\":\"visibility\",\"value\":false},"
             "{\"variable\":\"headline\",\"kind\":\"text\",\"value\":\"Hello\"}]}]}") == PC_OK);
    /* defining/data-sets do not change the canvas */
    CHECK(visible(s, badge));
    CHECK_EQ_STR(text_of(s, title), "Old");

    pc_json_value r = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "image.applyDataSet", "{\"name\":\"A\"}", &r, NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&r), "applied")), "A");
    pc_json_value_fini(&r);
    CHECK(!visible(s, badge));
    CHECK_EQ_STR(text_of(s, title), "Hello");
    CHECK(doc_of(s)->variables.has_active && doc_of(s)->variables.active == 0);

    /* one undo step */
    CHECK(pc_engine_session_undo(s));
    CHECK(visible(s, badge));
    CHECK_EQ_STR(text_of(s, title), "Old");
    pc_engine_session_free(s);
}

static void test_list_and_import(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":32,\"height\":32}") == PC_OK);
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    pc_layer_id a = pc_engine_session_active(s)->active_layer;
    char buf[160];
    snprintf(buf, sizeof buf, "{\"defs\":[{\"name\":\"show\",\"layer\":%llu,\"type\":\"visibility\"}]}",
             (unsigned long long)a);
    CHECK(ex(s, "image.variables.define", buf) == PC_OK);
    pc_json_value l = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "variables.list", "{}", &l, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_arr_size(yyjson_mut_obj_get(pc_json_value_root(&l), "defs")), 1u);
    pc_json_value_fini(&l);
    pc_engine_session_free(s);
}

static void test_bad_and_adversarial(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "image.variables.define", "{}") != PC_OK); /* no doc */
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    CHECK(ex(s, "image.variables.define", "{}") != PC_OK);                            /* need defs */
    CHECK(ex(s, "image.variables.define", "{\"defs\":[{\"name\":\"x\"}]}") != PC_OK); /* need layer */
    CHECK(ex(s, "image.applyDataSet", "{}") != PC_OK);
    CHECK(ex(s, "image.applyDataSet", "{\"name\":\"nope\"}") != PC_OK);
    const char *ids[] = {"image.variables.define",       "image.variables.dataSets",    "image.applyDataSet",
                         "file.import.variableDataSets", "file.export.dataSetsAsFiles", "variables.list"};
    const char *ps[] = {
        "{}", "[]", "null", "123", "{\"defs\":5}", "{\"dataSets\":[{}]}", "{\"name\":5}", "{\"index\":-1}"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof ps / sizeof ps[0]; j++) {
            (void)ex(s, ids[i], ps[j]); /* must not crash */
        }
    }
    pc_engine_session_free(s);
}

static unsigned path_checks;
static pc_status deny_pixels(const char *id, yyjson_val *params, pc_error *err)
{
    if (!strcmp(id, "image.applyDataSet") && pc_json_get(params, "path")) {
        path_checks++;
        return pc_fail(err, PC_ERR_INVALID, "pixel file denied");
    }
    return PC_OK;
}
static void test_atomic_dialog_commit_and_failed_application(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":8,\"height\":8}") == PC_OK);
    CHECK(ex(s, "layer.new.layer", "{}") == PC_OK);
    pc_layer_id layer = pc_engine_session_active(s)->active_layer;
    char params[1024];
    snprintf(params, sizeof params,
             "{\"defs\":[{\"name\":\"show\",\"layer\":%llu,\"type\":\"visibility\"}],"
             "\"dataSets\":[{\"name\":\"Hidden\",\"values\":[{\"variable\":\"show\","
             "\"kind\":\"visibility\",\"value\":false}]}],\"apply\":\"Hidden\"}",
             (unsigned long long)layer);
    size_t history = pc_history_past_len(pc_engine_session_active(s)->history), journal = s->njournal;
    CHECK(ex(s, "image.variables.edit", params) == PC_OK);
    CHECK(!visible(s, layer));
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == history + 1);
    CHECK(s->njournal == journal + 1);
    CHECK_EQ_STR(s->journal[journal].id, "image.variables.edit");
    CHECK(pc_engine_session_undo(s));
    CHECK(visible(s, layer));
    CHECK(doc_of(s)->variables.ndefs == 0);
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    uint64_t revision = pc_engine_session_active(s)->revision;
    CHECK(ex(s, "image.variables.edit", "{\"defs\":[],\"dataSets\":[{}]}") != PC_OK);
    CHECK(ex(s, "image.variables.edit", "{\"defs\":[],\"dataSets\":[],\"apply\":\"missing\"}") != PC_OK);
    CHECK(pc_engine_session_active(s)->doc == before);
    CHECK(pc_engine_session_active(s)->revision == revision);
    snprintf(params, sizeof params,
             "{\"defs\":[{\"name\":\"pixels\",\"layer\":%llu,\"type\":\"pixelReplacement\"}],"
             "\"dataSets\":[{\"name\":\"A\",\"values\":[{\"variable\":\"pixels\","
             "\"kind\":\"pixels\",\"value\":\"/denied.png\"}]}],\"apply\":\"A\"}",
             (unsigned long long)layer);
    s->authorize = deny_pixels;
    path_checks = 0;
    CHECK(ex(s, "image.variables.edit", params) != PC_OK);
    CHECK(path_checks == 1);
    CHECK(pc_engine_session_active(s)->doc == before);
    CHECK(pc_engine_session_active(s)->revision == revision);
    pc_doc_arc_release(before);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_define_apply_visibility_text);
    RUN(test_list_and_import);
    RUN(test_atomic_dialog_commit_and_failed_application);
    RUN(test_bad_and_adversarial);
    return pc_t_finish();
}
