#include "harness.h"
#include "engine/preview.h"
#include "engine/engine_internal.h"
#include <string.h>
static pc_status ex(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status status = pc_engine_session_execute_json(s, id, json, &out, &err);
    if (status != PC_OK)
        fprintf(stderr, "%s: %s\n", id, err.msg);
    pc_json_value_fini(&out);
    return status;
}
static pc_engine_session *scene(const char *mode, unsigned depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char json[160];
    (void)snprintf(json, sizeof json,
                   "{\"width\":16,\"height\":12,\"mode\":\"%s\",\"depth\":%u,\"background\":\"white\"}", mode,
                   depth);
    CHECK(ex(s, "file.new", json) == PC_OK);
    CHECK(ex(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":8,\"height\":12}") == PC_OK);
    CHECK(ex(s, "edit.fill", "{\"color\":\"#110022\"}") == PC_OK);
    CHECK(ex(s, "select.deselect", "{}") == PC_OK);
    return s;
}
static uint64_t start(pc_engine_session *s, const char *id, const char *json)
{
    yyjson_doc *params = NULL;
    CHECK(pc_json_parse(json, strlen(json), &params, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    bool job = false;
    uint64_t number = 0;
    CHECK(pc_engine_preview_start_job(s, id, yyjson_doc_get_root(params), &job, &number, &out, NULL) ==
          PC_OK);
    CHECK(job && number > 0);
    pc_json_value_fini(&out);
    yyjson_doc_free(params);
    return number;
}
static void test_background_matches_inline_every_mode_depth_journal_and_undo(void)
{
    const char *modes[] = {"rgb", "grayscale", "cmyk", "lab"};
    unsigned depths[] = {8, 16, 32};
    for (size_t m = 0; m < 4; m++)
        for (size_t z = 0; z < 3; z++) {
            pc_engine_session *s = scene(modes[m], depths[z]), *expected = NULL;
            CHECK(pc_engine_preview_clone(s, &expected, NULL) == PC_OK);
            const pc_engine_doc_state *state = pc_engine_session_active(s);
            pc_doc_arc *before = pc_doc_arc_retain(state->doc);
            size_t history = pc_history_past_len(state->history), journal = s->njournal;
            CHECK(ex(expected, "filter.blur.gaussianBlur", "{\"radius\":2}") == PC_OK);
            uint64_t id = start(s, "filter.blur.gaussianBlur", "{\"radius\":2}");
            CHECK(pc_engine_session_active(s)->doc == before);
            pc_json_value out = PC_JSON_VALUE_INIT;
            CHECK(pc_engine_session_wait_job(s, id, &out, NULL) == PC_OK);
            pc_json_value_fini(&out);
            state = pc_engine_session_active(s);
            CHECK(pc_document_eq(pc_doc_arc_get(state->doc),
                                 pc_doc_arc_get(pc_engine_session_active(expected)->doc)));
            CHECK(!pc_document_eq(pc_doc_arc_get(state->doc), pc_doc_arc_get(before)));
            CHECK(pc_history_past_len(state->history) == history + 1 && s->njournal == journal + 1);
            CHECK_EQ_STR(s->journal[journal].id, "filter.blur.gaussianBlur");
            CHECK(yyjson_mut_get_num(
                      yyjson_mut_obj_get(pc_json_value_root(&s->journal[journal].params), "radius")) == 2);
            CHECK(pc_engine_session_undo(s));
            CHECK(pc_document_eq(pc_doc_arc_get(pc_engine_session_active(s)->doc), pc_doc_arc_get(before)));
            pc_doc_arc_release(before);
            pc_engine_session_free(expected);
            pc_engine_session_free(s);
        }
}
static void test_background_completion_preserves_the_users_active_tab(void)
{
    pc_engine_session *s = scene("rgb", 8);
    uint64_t id = start(s, "filter.blur.gaussianBlur", "{\"radius\":3}");
    pc_document other = {0};
    CHECK(pc_document_with_background(&other, "Other", pc_size_make(8, 8), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgb(1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &other, NULL, NULL, NULL) == PC_OK);
    pc_doc_arc *second = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_wait_job(s, id, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_active(s)->doc == second);
    size_t active = 0;
    CHECK(pc_engine_session_active_index(s, &active) && active == 1);
    pc_doc_arc_release(second);
    pc_engine_session_free(s);
}
static void test_cancel_and_invalid_params_do_not_mutate_source(void)
{
    pc_engine_session *s = scene("rgb", 16);
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    size_t journal = s->njournal, history = pc_history_past_len(pc_engine_session_active(s)->history);
    uint64_t id = start(s, "filter.blur.gaussianBlur", "{\"radius\":10}");
    CHECK(pc_engine_session_cancel_job(s, id));
    CHECK(pc_engine_session_active(s)->doc == before && s->njournal == journal);
    id = start(s, "image.adjustments.exposure", "{\"gamma\":\"invalid\"}");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_wait_job(s, id, &out, NULL) != PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_active(s)->doc == before && s->njournal == journal);
    CHECK(pc_history_past_len(pc_engine_session_active(s)->history) == history);
    bool job = false;
    uint64_t bad = 0;
    CHECK(pc_engine_preview_start_job(s, "file.save", NULL, &job, &bad, &out, NULL) != PC_OK);
    pc_doc_arc_release(before);
    pc_engine_session_free(s);
}
static bool deny_command;
static unsigned authorizations;
static pc_status authorize_inline(const char *id, yyjson_val *params, pc_error *err)
{
    (void)id;
    (void)params;
    authorizations++;
    return deny_command ? pc_fail(err, PC_ERR_INVALID, "restricted command") : PC_OK;
}
static void test_restricted_jobs_preserve_policy_and_execute_inline(void)
{
    pc_engine_session *s = scene("rgb", 8);
    s->authorize = authorize_inline;
    authorizations = 0;
    deny_command = true;
    pc_doc_arc *before = pc_doc_arc_retain(pc_engine_session_active(s)->doc);
    pc_engine_session *clone = NULL;
    CHECK(pc_engine_preview_clone(s, &clone, NULL) == PC_OK);
    CHECK(ex(clone, "filter.blur.gaussianBlur", "{\"radius\":2}") != PC_OK);
    CHECK(authorizations == 0);
    pc_engine_session_free(clone);
    pc_json_value out = PC_JSON_VALUE_INIT;
    bool job = true;
    uint64_t id = 99;
    CHECK(pc_engine_preview_start_job(s, "filter.blur.gaussianBlur", NULL, &job, &id, &out, NULL) != PC_OK);
    CHECK(!job && id == 0 && authorizations == 1);
    CHECK(pc_engine_session_active(s)->doc == before);
    deny_command = false;
    CHECK(pc_engine_preview_start_job(s, "filter.blur.gaussianBlur", NULL, &job, &id, &out, NULL) == PC_OK);
    CHECK(!job && id == 0 && authorizations == 2);
    pc_json_value_fini(&out);
    pc_doc_arc_release(before);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_background_matches_inline_every_mode_depth_journal_and_undo);
    RUN(test_background_completion_preserves_the_users_active_tab);
    RUN(test_cancel_and_invalid_params_do_not_mutate_source);
    RUN(test_restricted_jobs_preserve_policy_and_execute_inline);
    return pc_t_finish();
}
