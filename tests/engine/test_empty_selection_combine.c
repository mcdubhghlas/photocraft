/* Subtract/Intersect never conjure a selection from nothing; Replace/Add still do (port of
 * crates/engine/tests/empty_selection_combine.rs). */
#include "engine/engine.h"
#include "harness.h"

static pc_engine_session *session(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":32,\"height\":24}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    return s;
}

static void rect(pc_engine_session *s, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "select.rect", json, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}

static void test_subtract_intersect_never_create(void)
{
    static const char *const modes[] = {"subtract", "intersect"};
    static const double feathers[] = {0.0, 2.0};
    for (size_t m = 0; m < 2; m++) {
        for (size_t f = 0; f < 2; f++) {
            pc_engine_session *s = session();
            char p[128];
            (void)snprintf(p, sizeof p,
                           "{\"x\":4,\"y\":5,\"width\":12,\"height\":10,\"mode\":\"%s\",\"feather\":%g}",
                           modes[m], feathers[f]);
            rect(s, p);
            const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
            if (doc->has_selection) {
                PC_T_FAIL("mode=%s feather=%g created a selection from none", modes[m], feathers[f]);
            }
            pc_engine_session_free(s);
        }
    }
}

static void test_replace_add_create(void)
{
    static const char *const modes[] = {"replace", "add"};
    for (size_t m = 0; m < 2; m++) {
        pc_engine_session *s = session();
        char p[128];
        (void)snprintf(p, sizeof p, "{\"x\":4,\"y\":5,\"width\":12,\"height\":10,\"mode\":\"%s\"}", modes[m]);
        rect(s, p);
        const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
        CHECK(doc->has_selection);
        if (doc->has_selection) {
            pc_rect b = pc_surface_content_bounds(&doc->selection);
            if (pc_rect_is_empty(b)) {
                PC_T_FAIL("mode=%s produced an empty selection", modes[m]);
            }
        }
        pc_engine_session_free(s);
    }
}

static void test_intersect_keeps_overlap(void)
{
    pc_engine_session *s = session();
    rect(s, "{\"x\":4,\"y\":5,\"width\":12,\"height\":10}");
    rect(s, "{\"x\":8,\"y\":9,\"width\":12,\"height\":10,\"mode\":\"intersect\"}");
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(doc->has_selection);
    if (doc->has_selection) {
        float inside = pc_surface_sample_channel(&doc->selection, 8, 9, 0);
        float outside = pc_surface_sample_channel(&doc->selection, 4, 5, 0);
        CHECK(inside > 0.0f);
        CHECK(outside == 0.0f);
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_subtract_intersect_never_create);
    RUN(test_replace_add_create);
    RUN(test_intersect_keeps_overlap);
    return pc_t_finish();
}
