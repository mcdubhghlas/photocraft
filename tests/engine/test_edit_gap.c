/* Edit menu gap: Paste Into / Paste Outside / Transform Again (edit_cmds.c additions).
 *
 * Paste Into/Outside paste a new masked layer; Transform Again replays the last transform; no
 * selection, no clipboard, no prior transform and no document are graceful (never a crash, rule 9). */
#include <stdio.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_status run_json(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    return pc_doc_arc_get(d->doc);
}

/* A document with a Background and a painted layer "L". */
static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(32, 32), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{\"name\":\"L\"}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    CHECK(run_json(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":8,\"height\":8}") == PC_OK);
    CHECK(run_json(s, "edit.fill", "{\"contents\":\"color\",\"color\":\"#00ff00\"}") == PC_OK);
    CHECK(run_json(s, "select.deselect", "{}") == PC_OK);
    return s;
}

static void test_registry(void)
{
    CHECK(pc_engine_command_find("edit.pasteSpecial.pasteInto") != NULL);
    CHECK(pc_engine_command_find("edit.pasteSpecial.pasteOutside") != NULL);
    CHECK(pc_engine_command_find("edit.transform.again") != NULL);
}

static void test_paste_into_and_outside(void)
{
    pc_engine_session *s = mk();
    /* Copy a region to the clipboard. */
    CHECK(run_json(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":8,\"height\":8}") == PC_OK);
    CHECK(run_json(s, "edit.copy", "{}") == PC_OK);
    /* A new selection elsewhere, then Paste Into: a new masked layer. */
    CHECK(run_json(s, "select.rect", "{\"x\":16,\"y\":16,\"width\":10,\"height\":10}") == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "edit.pasteSpecial.pasteInto"));
    size_t before = doc_of(s)->nlayers;
    CHECK(run_json(s, "edit.pasteSpecial.pasteInto", "{}") == PC_OK);
    const pc_document *d = doc_of(s);
    CHECK(d->nlayers == before + 1);
    const pc_layer *top = &d->layers[d->nlayers - 1];
    CHECK(top->has_mask); /* masked to the selection */
    /* Undo returns to one step. */
    CHECK(pc_engine_session_undo(s));
    CHECK(doc_of(s)->nlayers == before);
    /* Paste Outside also makes a masked layer. */
    CHECK(run_json(s, "select.rect", "{\"x\":16,\"y\":16,\"width\":10,\"height\":10}") == PC_OK);
    CHECK(run_json(s, "edit.pasteSpecial.pasteOutside", "{}") == PC_OK);
    d = doc_of(s);
    CHECK(d->layers[d->nlayers - 1].has_mask);
    pc_engine_session_free(s);
}

static void test_transform_again(void)
{
    pc_engine_session *s = mk();
    /* Select the layer with pixels and run a transform, then repeat it. */
    char buf[64];
    const pc_document *d = doc_of(s);
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}",
                   (unsigned long long)d->layers[d->nlayers - 1].id);
    CHECK(run_json(s, "layer.select", buf) == PC_OK);
    /* A rect->quad free transform (the geometry Transform Again repeats). */
    CHECK(run_json(s, "edit.transform",
                   "{\"rect\":[4,4,12,12],\"quad\":[[5,5],[13,4],[14,13],[4,14]]}") == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "edit.transform.again"));
    CHECK(run_json(s, "edit.transform.again", "{}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "edit.pasteSpecial.pasteInto", "{}") != PC_OK);
    CHECK(run_json(s, "edit.transform.again", "{}") != PC_OK);
    pc_engine_session_free(s);

    /* A document but no clipboard / no prior transform. */
    s = mk();
    CHECK(run_json(s, "select.rect", "{\"x\":1,\"y\":1,\"width\":4,\"height\":4}") == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "edit.pasteSpecial.pasteInto")); /* empty clipboard */
    CHECK(run_json(s, "edit.pasteSpecial.pasteInto", "{}") != PC_OK);
    CHECK(run_json(s, "edit.transform.again", "{}") != PC_OK); /* no transform to repeat */
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_paste_into_and_outside);
    RUN(test_transform_again);
    RUN(test_graceful);
    return pc_t_finish();
}
