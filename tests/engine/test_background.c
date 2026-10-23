/* Layer > New > Background from Layer: layer.new.backgroundFromLayer (background_cmds.c).
 *
 * A normal layer becomes the opaque, locked Background at the bottom of the stack in one undoable
 * step; transparent pixels take the background colour; a second conversion, a group/adjustment
 * layer, no document and bad params are graceful errors (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/background_cmds.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "harness.h"

/* A document with a white Background plus two empty layers A, B on top; A gets a red square. */
static pc_engine_session *mk(pc_layer_id ids[3])
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_init(&doc, "Untitled", pc_size_make(16, 12), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8,
                           NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    /* Start from a layer-less document, then add L0 (becomes normal), A, B. */
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{\"name\":\"L0\"}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    o = (pc_json_value)PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{\"name\":\"A\"}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    o = (pc_json_value)PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{\"name\":\"B\"}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *dd = pc_doc_arc_get(d->doc);
    CHECK(dd->nlayers == 3);
    ids[0] = dd->layers[0].id; /* L0 */
    ids[1] = dd->layers[1].id; /* A */
    ids[2] = dd->layers[2].id; /* B */
    /* A red square into A. */
    o = (pc_json_value)PC_JSON_VALUE_INIT;
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)ids[1]);
    CHECK(pc_engine_session_execute_json(s, "layer.select", buf, &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    o = (pc_json_value)PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "select.rect", "{\"x\":2,\"y\":2,\"width\":4,\"height\":4}",
                                         &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    o = (pc_json_value)PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "edit.fill", "{\"contents\":\"color\",\"color\":\"#ff0000\"}",
                                         &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    o = (pc_json_value)PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "select.deselect", "{}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    return s;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    return pc_doc_arc_get(d->doc);
}

static pc_status run_json(pc_engine_session *s, const char *id, const char *params)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_background_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("layer.new.backgroundFromLayer") != NULL);
}

static void test_makes_background_at_bottom(void)
{
    pc_layer_id ids[3];
    pc_engine_session *s = mk(ids);
    /* Background colour blue. */
    s->tools.background[0] = 0.0f;
    s->tools.background[1] = 0.0f;
    s->tools.background[2] = 1.0f;
    s->tools.background[3] = 1.0f;
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)ids[1]);
    CHECK(run_json(s, "layer.select", buf) == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "layer.new.backgroundFromLayer"));
    CHECK(run_json(s, "layer.new.backgroundFromLayer", "{}") == PC_OK);
    const pc_document *d = doc_of(s);
    /* A dropped to the bottom and is now the Background, keeping its id. */
    CHECK(d->nlayers == 3);
    CHECK(d->layers[0].id == ids[1]);
    CHECK(d->layers[1].id == ids[0]);
    CHECK(d->layers[2].id == ids[2]);
    CHECK(pc_document_is_background(d, ids[1]));
    const pc_layer *bg = &d->layers[0];
    const pc_surface *su = pc_layer_surface(bg);
    CHECK(su != NULL);
    /* Red where the square was, blue where A was transparent. */
    CHECK(pc_surface_sample_channel(su, 3, 3, 0) > 0.9f);   /* red R */
    CHECK(pc_surface_sample_channel(su, 3, 3, 2) < 0.1f);   /* red B */
    CHECK(pc_surface_sample_channel(su, 10, 10, 0) < 0.1f); /* blue R */
    CHECK(pc_surface_sample_channel(su, 10, 10, 2) > 0.9f); /* blue B */
    /* Now there is a Background: the command greys. */
    CHECK(!pc_engine_session_is_enabled(s, "layer.new.backgroundFromLayer"));
    /* Undo restores the original order and A as a normal layer. */
    CHECK(pc_engine_session_undo(s));
    d = doc_of(s);
    CHECK(d->layers[0].id == ids[0] && d->layers[1].id == ids[1] && d->layers[2].id == ids[2]);
    CHECK(!pc_document_is_background(d, ids[1]));
    CHECK(pc_engine_session_redo(s));
    CHECK(pc_document_is_background(doc_of(s), ids[1]));
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    /* No document. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "layer.new.backgroundFromLayer"));
    CHECK(run_json(s, "layer.new.backgroundFromLayer", "{}") != PC_OK);
    pc_engine_session_free(s);

    pc_layer_id ids[3];
    s = mk(ids);
    /* Bad params. */
    CHECK(run_json(s, "layer.new.backgroundFromLayer", "{\"layer\":\"A\"}") != PC_OK);
    CHECK(run_json(s, "layer.new.backgroundFromLayer", "{\"layer\":-1}") != PC_OK);
    CHECK(run_json(s, "layer.new.backgroundFromLayer", "{\"layer\":99999}") != PC_OK);
    CHECK(pc_document_background(doc_of(s)) == NULL);
    /* A group can't become the Background. */
    char buf[96];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)ids[1]);
    CHECK(run_json(s, "layer.select", buf) == PC_OK);
    CHECK(run_json(s, "layer.groupLayers", "{}") == PC_OK);
    CHECK(run_json(s, "layer.new.backgroundFromLayer", "{}") != PC_OK);
    CHECK(pc_document_background(doc_of(s)) == NULL);
    /* Convert A (first ungroup so A is a normal layer), then a second attempt fails. */
    CHECK(run_json(s, "edit.undo", "{}") == PC_OK); /* undo group */
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)ids[1]);
    CHECK(run_json(s, "layer.select", buf) == PC_OK);
    CHECK(run_json(s, "layer.new.backgroundFromLayer", "{}") == PC_OK);
    CHECK(run_json(s, "layer.new.backgroundFromLayer", "{\"layer\":99999}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_makes_background_at_bottom);
    RUN(test_graceful);
    return pc_t_finish();
}
