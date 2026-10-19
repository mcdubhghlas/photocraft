#include "harness.h"
#include "engine/engine_internal.h"
#include "engine/preview.h"
#include <string.h>
static pc_status ex(pc_engine_session *s, const char *id, const char *text)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = pc_engine_session_execute_json(s, id, text, &out, &err);
    if (status != PC_OK)
        fprintf(stderr, "%s: %s\n", id, err.msg);
    pc_json_value_fini(&out);
    return status;
}
static const pc_document *doc(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}
static float at(pc_engine_session *s, int32_t x, int32_t y)
{
    const pc_document *d = doc(s);
    return d->has_selection ? pc_surface_sample_channel(&d->selection, x, y, 0) : 0;
}
static pc_engine_session *fresh(const char *mode, unsigned depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char json[160];
    (void)snprintf(json, sizeof json,
                   "{\"width\":16,\"height\":12,\"mode\":\"%s\",\"depth\":%u,\"background\":\"white\"}", mode,
                   depth);
    CHECK(ex(s, "file.new", json) == PC_OK);
    CHECK(ex(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":8,\"height\":12}") == PC_OK);
    CHECK(ex(s, "edit.fill", "{\"color\":\"#332211\"}") == PC_OK);
    CHECK(ex(s, "select.deselect", "{}") == PC_OK);
    return s;
}
static void test_sampled_points_all_depths_models_and_undo(void)
{
    const char *modes[] = {"rgb", "grayscale", "cmyk", "lab"};
    unsigned depths[] = {8, 16, 32};
    for (size_t m = 0; m < 4; m++)
        for (size_t d = 0; d < 3; d++) {
            pc_engine_session *s = fresh(modes[m], depths[d]);
            size_t history = pc_history_past_len(pc_engine_session_active(s)->history), journal = s->njournal;
            CHECK(ex(s, "select.colorRange", "{\"points\":[[2,3]],\"fuzziness\":0}") == PC_OK);
            CHECK(at(s, 2, 3) > .99f && at(s, 12, 3) < .01f);
            CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == history + 1);
            CHECK(s->njournal == journal + 1);
            CHECK(pc_engine_session_undo(s));
            CHECK(!doc(s)->has_selection);
            CHECK(ex(s, "select.colorRange", "{\"points\":[[2,3]],\"localized\":true,\"range\":20}") ==
                  PC_OK);
            CHECK(at(s, 2, 3) > .99f && at(s, 7, 11) < .01f);
            pc_engine_session_free(s);
        }
}
static void test_tones_invert_scope_order_and_bad_params(void)
{
    pc_engine_session *s = fresh("rgb", 8);
    CHECK(ex(s, "select.colorRange", "{\"select\":\"highlights\",\"fuzziness\":0}") == PC_OK);
    CHECK(at(s, 12, 3) > .99f && at(s, 2, 3) < .01f);
    CHECK(ex(s, "select.deselect", "{}") == PC_OK);
    CHECK(ex(s, "select.colorRange",
             "{\"points\":[[2,3],[12,3]],\"subtractPoints\":[[12,3]],\"order\":[\"+\",\"+\",\"-\"],"
             "\"fuzziness\":10}") == PC_OK);
    CHECK(at(s, 2, 3) > at(s, 12, 3));
    CHECK(at(s, 12, 3) < .99f);
    CHECK(ex(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":8,\"height\":12}") == PC_OK);
    CHECK(ex(s, "select.colorRange", "{\"select\":\"highlights\",\"invert\":true}") == PC_OK);
    CHECK(at(s, 2, 3) > .99f && at(s, 12, 3) < .01f);
    const char *bad[] = {"{\"select\":null}",
                         "{\"select\":\"skinTones\"}",
                         "{\"points\":[[-1,0]]}",
                         "{\"points\":[[12,0]]}",
                         "{\"points\":[[2,3]],\"order\":[\"-\"]}",
                         "{\"points\":{}}",
                         "{\"localized\":true}",
                         "{\"select\":\"midtones\",\"tonalRange\":[150,100]}",
                         "{\"colors\":[\"#xx0000\"]}"};
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(ex(s, "select.colorRange", bad[i]) != PC_OK);
        CHECK(pc_document_eq(pc_doc_arc_get(before), doc(s)));
    }
    pc_doc_arc_release(before);
    pc_engine_session_free(s);
}
static void test_preview_preserves_proof_and_hdr_views(void)
{
    pc_engine_session *s = fresh("rgb", 32);
    CHECK(ex(s, "view.proofSetup", "{\"preset\":\"working-cmyk\"}") == PC_OK);
    CHECK(ex(s, "view.proofColors", "{\"on\":true}") == PC_OK);
    pc_engine_session *preview = NULL;
    CHECK(pc_engine_preview_clone(s, &preview, NULL) == PC_OK);
    CHECK(preview->color.nproofs == s->color.nproofs && preview->color.nproofs > 0);
    CHECK(preview->color.proofs[0].view.setup.name != s->color.proofs[0].view.setup.name);
    CHECK_EQ_STR(preview->color.proofs[0].view.setup.name, s->color.proofs[0].view.setup.name);
    CHECK(preview->color.proofs[0].view.enabled == s->color.proofs[0].view.enabled);
    CHECK(ex(s, "select.colorRange", "{\"select\":\"outOfGamut\"}") == PC_OK);
    CHECK(ex(preview, "select.colorRange", "{\"select\":\"outOfGamut\"}") == PC_OK);
    CHECK(pc_document_eq(doc(s), doc(preview)));
    pc_engine_session_free(preview);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_sampled_points_all_depths_models_and_undo);
    RUN(test_tones_invert_scope_order_and_bad_params);
    RUN(test_preview_preserves_proof_and_hdr_views);
    return pc_t_finish();
}
