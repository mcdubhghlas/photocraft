/* Note tool annotations (port of crates/engine/src/notes_cmds.rs tests, E12). */
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "format/format.h"
#include "harness.h"

/* Execute ignoring the result; returns the status. */
static pc_status ex(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, p, &o, NULL);
    pc_json_value_fini(&o);
    return rc;
}

static pc_engine_session *session_100x80(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(ex(s, "file.new", "{\"width\":100,\"height\":80}") == PC_OK);
    return s;
}

static yyjson_mut_val *obj_get(pc_json_value *v, const char *k)
{
    return yyjson_mut_obj_get(pc_json_value_root(v), k);
}

static void test_add_edit_move_delete_with_undo(void)
{
    pc_engine_session *s = session_100x80();
    CHECK(!pc_engine_session_is_enabled(s, "notes.delete"));

    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "notes.add",
              "{\"x\":10,\"y\":12,\"text\":\"fix this\",\"author\":\"QA\",\"color\":\"#ff0000\"}", &out,
              NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(obj_get(&out, "index")), 0u);
    yyjson_mut_val *note = obj_get(&out, "note");
    yyjson_mut_val *col = yyjson_mut_obj_get(note, "color");
    CHECK(yyjson_mut_is_arr(col));
    CHECK_NEAR(yyjson_mut_get_real(yyjson_mut_arr_get(col, 0)), 1.0, 1e-6);
    CHECK_NEAR(yyjson_mut_get_real(yyjson_mut_arr_get(col, 1)), 0.0, 1e-6);
    pc_json_value_fini(&out);

    CHECK(ex(s, "notes.add", "{\"x\":50,\"y\":40}") == PC_OK);
    CHECK(ex(s, "notes.set", "{\"index\":0,\"text\":\"fixed\",\"x\":20}") == PC_OK);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    CHECK_EQ_STR(pc_str_c(&doc->notes[0].text), "fixed");
    CHECK_NEAR(doc->notes[0].position[0], 20.0, 1e-9);
    CHECK_NEAR(doc->notes[0].position[1], 12.0, 1e-9);
    CHECK_NEAR(doc->notes[0].popup[0], 40.0, 1e-9); /* 20 moved +20 popup offset - x shift */

    /* Unchanged edits don't add history. */
    size_t before = pc_history_past_len(d->history);
    CHECK(pc_engine_session_execute_json(s, "notes.set", "{\"index\":0,\"text\":\"fixed\"}", &out,
                                         NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(obj_get(&out, "changed")) == false);
    pc_json_value_fini(&out);
    d = pc_engine_session_active(s);
    CHECK_EQ_UINT(pc_history_past_len(d->history), before);

    CHECK(ex(s, "notes.delete", "{\"index\":1}") == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "notes.list", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_arr_size(obj_get(&out, "notes")), 1u);
    pc_json_value_fini(&out);

    CHECK(pc_engine_session_undo(s));
    d = pc_engine_session_active(s);
    CHECK_EQ_UINT(pc_doc_arc_get(d->doc)->nnotes, 2u);

    CHECK(ex(s, "notes.delete", "{\"all\":true}") == PC_OK);
    d = pc_engine_session_active(s);
    CHECK_EQ_UINT(pc_doc_arc_get(d->doc)->nnotes, 0u);
    CHECK(ex(s, "notes.set", "{\"index\":0}") != PC_OK);

    pc_engine_session_free(s);
}

static void test_bad_params(void)
{
    pc_engine_session *s = session_100x80();
    CHECK(ex(s, "notes.add", "{\"x\":1}") != PC_OK);
    CHECK(ex(s, "notes.add", "{\"x\":1,\"y\":1}") == PC_OK);
    CHECK(ex(s, "notes.set", "{\"index\":5,\"text\":\"x\"}") != PC_OK);
    CHECK(ex(s, "notes.set", "{}") != PC_OK);
    CHECK(ex(s, "file.import.notes", "{}") != PC_OK);
    CHECK(ex(s, "file.import.notes", "{\"path\":\"/nonexistent/x.psd\"}") != PC_OK);
    /* adversarial: non-finite, wrong types */
    CHECK(ex(s, "notes.add", "{\"x\":1e999,\"y\":0}") != PC_OK);
    CHECK(ex(s, "notes.add", "{\"x\":\"nope\",\"y\":true}") != PC_OK);
    CHECK(ex(s, "notes.set", "{\"index\":-1}") != PC_OK);
    (void)ex(s, "notes.delete", "[]"); /* bad root: must not crash */
    pc_engine_session_free(s);
}

static void test_import_notes_from_pcraft(void)
{
    /* Build a source document with a note outside the 100x80 canvas, save to .pcraft bytes,
     * then import its notes and check they are pulled inside. */
    pc_document src;
    CHECK(pc_document_init(&src, "src", pc_size_make(400, 400), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                           NULL) == PC_OK);
    pc_note n;
    pc_note_default(&n);
    CHECK(pc_str_set_c(&n.text, "from pcraft", NULL) == PC_OK);
    n.position[0] = 300.0;
    n.position[1] = 300.0;
    pc_note *arr = malloc(sizeof *arr);
    CHECK(arr != NULL);
    arr[0] = n;
    src.notes = arr;
    src.nnotes = 1;

    pc_buf bytes = PC_BUF_INIT;
    CHECK(pc_format_save_to_bytes(&src, NULL, &bytes, NULL) == PC_OK);
    pc_document_fini(&src);

    pc_engine_session *s = session_100x80();
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_import_notes_from(s, "a.pcraft", bytes.data, bytes.len, &out, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(obj_get(&out, "imported")), 1u);
    pc_json_value_fini(&out);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    CHECK_EQ_UINT(doc->nnotes, 1u);
    CHECK_EQ_STR(pc_str_c(&doc->notes[0].text), "from pcraft");
    CHECK(doc->notes[0].position[0] <= 84.0 && doc->notes[0].position[1] <= 60.0);

    /* A file without notes is an error. */
    pc_document empty;
    CHECK(pc_document_init(&empty, "empty", pc_size_make(10, 10), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                           NULL) == PC_OK);
    pc_buf eb = PC_BUF_INIT;
    CHECK(pc_format_save_to_bytes(&empty, NULL, &eb, NULL) == PC_OK);
    pc_document_fini(&empty);
    CHECK(pc_engine_import_notes_from(s, "b.pcraft", eb.data, eb.len, NULL, NULL) != PC_OK);
    pc_buf_fini(&eb);
    pc_buf_fini(&bytes);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_add_edit_move_delete_with_undo);
    RUN(test_bad_params);
    RUN(test_import_notes_from_pcraft);
    return pc_t_finish();
}
