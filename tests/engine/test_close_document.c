/* Closing a background tab must preserve the active document, not its old index (port of
 * crates/engine/tests/close_document.rs). */
#include <stddef.h>
#include <stdint.h>

#include "engine/engine.h"
#include "harness.h"

static pc_engine_session *with_documents(size_t count, pc_doc_id *ids)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    for (size_t i = 0; i < count; i++) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        char p[64];
        (void)snprintf(p, sizeof p, "{\"name\":\"Document %zu\",\"width\":8,\"height\":8}", i);
        CHECK(pc_engine_session_execute_json(s, "file.new", p, &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        ids[i] = pc_doc_arc_get(pc_engine_session_active(s)->doc)->id;
    }
    return s;
}

static pc_doc_id active_id(const pc_engine_session *s)
{
    const pc_engine_doc_state *a = pc_engine_session_active(s);
    return a == NULL ? 0 : pc_doc_arc_get(a->doc)->id;
}

/* Index of id in the session's current document list, or SIZE_MAX. */
static size_t index_of(const pc_engine_session *s, pc_doc_id id)
{
    size_t n = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &n);
    for (size_t i = 0; i < n; i++) {
        if (pc_doc_arc_get(docs[i].doc)->id == id) {
            return i;
        }
    }
    return SIZE_MAX;
}

static void check_ids_equal(const pc_engine_session *s, const pc_doc_id *expect, size_t n)
{
    size_t got = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(s, &got);
    CHECK_EQ_UINT(got, n);
    for (size_t i = 0; i < n && i < got; i++) {
        CHECK_EQ_UINT(pc_doc_arc_get(docs[i].doc)->id, expect[i]);
    }
}

static void test_closing_background_keeps_active(void)
{
    static const size_t pairs[6][2] = {{0, 1}, {0, 2}, {1, 0}, {1, 2}, {2, 0}, {2, 1}};
    for (size_t k = 0; k < 6; k++) {
        size_t active = pairs[k][0], closing = pairs[k][1];
        pc_doc_id ids[3];
        pc_engine_session *s = with_documents(3, ids);
        CHECK(pc_engine_session_set_active(s, active));
        pc_doc_id aid = ids[active];
        /* Expected remaining ids: ids with `closing` removed. */
        pc_doc_id rem[2];
        size_t r = 0;
        for (size_t i = 0; i < 3; i++) {
            if (i != closing) {
                rem[r++] = ids[i];
            }
        }
        char p[48];
        (void)snprintf(p, sizeof p, "{\"document\":%zu}", closing);
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(pc_engine_session_execute_json(s, "file.close", p, &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        CHECK_EQ_UINT(active_id(s), aid);
        size_t ai = 0;
        CHECK(pc_engine_session_active_index(s, &ai));
        CHECK_EQ_UINT(ai, index_of(s, aid));
        check_ids_equal(s, rem, 2);
        pc_engine_session_free(s);
    }
}

static void test_closing_active_selects_neighbour(void)
{
    static const size_t pairs[3][2] = {{0, 1}, {1, 2}, {2, 1}}; /* closing -> next */
    for (size_t k = 0; k < 3; k++) {
        size_t closing = pairs[k][0], next = pairs[k][1];
        pc_doc_id ids[3];
        pc_engine_session *s = with_documents(3, ids);
        CHECK(pc_engine_session_set_active(s, closing));
        pc_doc_id next_id = ids[next];
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(pc_engine_session_execute_json(s, "file.close", "{}", &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        CHECK_EQ_UINT(active_id(s), next_id);
        size_t ai = 0;
        CHECK(pc_engine_session_active_index(s, &ai));
        CHECK_EQ_UINT(ai, index_of(s, next_id));
        pc_engine_session_free(s);
    }
}

static void test_closing_last_clears_active(void)
{
    pc_doc_id ids[1];
    pc_engine_session *s = with_documents(1, ids);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.close", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    size_t n = 1;
    (void)pc_engine_session_documents(s, &n);
    CHECK_EQ_UINT(n, 0);
    size_t ai = 0;
    CHECK(!pc_engine_session_active_index(s, &ai));
    CHECK(pc_engine_session_active(s) == NULL);
    CHECK(!pc_engine_session_close(s, 0));
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "file.close", "{}", &out, &err) != PC_OK);
    pc_engine_session_free(s);
}

static void test_closing_invalid_preserves_session(void)
{
    pc_doc_id ids[3];
    pc_engine_session *s = with_documents(3, ids);
    CHECK(pc_engine_session_set_active(s, 1));
    size_t bad[2] = {3, SIZE_MAX};
    for (size_t b = 0; b < 2; b++) {
        CHECK(!pc_engine_session_close(s, bad[b]));
        char p[48];
        (void)snprintf(p, sizeof p, "{\"document\":%zu}", bad[b]);
        pc_json_value out = PC_JSON_VALUE_INIT;
        CHECK(pc_engine_session_execute_json(s, "file.close", p, &out, NULL) != PC_OK);
        size_t ai = 0;
        CHECK(pc_engine_session_active_index(s, &ai) && ai == 1);
        CHECK_EQ_UINT(active_id(s), ids[1]);
        check_ids_equal(s, ids, 3);
    }
    pc_engine_session_free(s);
}

static void test_close_others_and_all(void)
{
    pc_doc_id ids[3];
    pc_engine_session *s = with_documents(3, ids);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.closeOthers", "{\"document\":1}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_doc_id only[1] = {ids[1]};
    check_ids_equal(s, only, 1);
    size_t ai = 0;
    CHECK(pc_engine_session_active_index(s, &ai) && ai == 0);
    CHECK(pc_engine_session_execute_json(s, "file.closeAll", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    size_t n = 1;
    (void)pc_engine_session_documents(s, &n);
    CHECK_EQ_UINT(n, 0);
    CHECK(!pc_engine_session_active_index(s, &ai));
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_closing_background_keeps_active);
    RUN(test_closing_active_selects_neighbour);
    RUN(test_closing_last_clears_active);
    RUN(test_closing_invalid_preserves_session);
    RUN(test_close_others_and_all);
    return pc_t_finish();
}
