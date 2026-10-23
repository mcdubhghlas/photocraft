/* Filter > Vanishing Point: filter.vanishingPoint (vp_cmds.c).
 *
 * Planes + paste + clone run in one undoable step; non-convex planes, no planes, no document and a
 * non-pixel layer are graceful errors (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdio.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/vp_cmds.h"
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

/* A document with a gray "floor" pixel layer. */
static pc_engine_session *mk(pc_layer_id *floor)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(400, 300), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{\"name\":\"floor\"}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    *floor = doc_of(s)->layers[doc_of(s)->nlayers - 1].id;
    CHECK(run_json(s, "select.all", "{}") == PC_OK);
    CHECK(run_json(s, "edit.fill", "{\"contents\":\"color\",\"color\":\"#404040\"}") == PC_OK);
    CHECK(run_json(s, "select.deselect", "{}") == PC_OK);
    return s;
}

static const char *PLANES =
    "\"planes\":[{\"corners\":[[120,120],[280,120],[360,280],[40,280]]},"
    "{\"from\":0,\"edge\":\"top\",\"angle\":90}]";

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_vp_specs(&n);
    CHECK_EQ_UINT(n, 1);
    CHECK(pc_engine_command_find("filter.vanishingPoint") != NULL);
}

static void test_paste_and_clone(void)
{
    pc_layer_id floor = 0;
    pc_engine_session *s = mk(&floor);
    /* A red sticker layer to paste. */
    pc_json_value o = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layer", "{\"name\":\"sticker\"}", &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
    pc_layer_id sticker = doc_of(s)->layers[doc_of(s)->nlayers - 1].id;
    CHECK(run_json(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":40,\"height\":20}") == PC_OK);
    CHECK(run_json(s, "edit.fill", "{\"contents\":\"color\",\"color\":\"#ff0000\"}") == PC_OK);
    CHECK(run_json(s, "select.deselect", "{}") == PC_OK);
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)floor);
    CHECK(run_json(s, "layer.select", buf) == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "filter.vanishingPoint"));
    char params[512];
    (void)snprintf(params, sizeof params,
                   "{%s,\"paste\":[{\"plane\":0,\"layer\":%llu,\"at\":[0.3,0.4],\"width\":0.4}]}", PLANES,
                   (unsigned long long)sticker);
    CHECK(run_json(s, "filter.vanishingPoint", params) == PC_OK);
    /* Something red landed somewhere on the floor plane. */
    const pc_surface *l = pc_layer_surface(pc_document_layer(doc_of(s), floor));
    CHECK(l != NULL);
    bool red = false;
    for (int32_t y = 120; y < 280 && !red; y += 4) {
        for (int32_t x = 60; x < 360 && !red; x += 4) {
            float got[4];
            pc_surface_rgba(l, x, y, got);
            if (got[0] > 0.6f && got[1] < 0.4f && got[2] < 0.4f) {
                red = true;
            }
        }
    }
    CHECK(red); /* the red sticker was pasted onto the plane */
    CHECK(pc_engine_session_undo(s));
    /* Clone + newLayer. */
    char p2[512];
    (void)snprintf(p2, sizeof p2, "{%s,\"newLayer\":true}", PLANES);
    size_t before = doc_of(s)->nlayers;
    CHECK(run_json(s, "filter.vanishingPoint", p2) == PC_OK);
    CHECK(doc_of(s)->nlayers == before + 1);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "filter.vanishingPoint"));
    CHECK(run_json(s, "filter.vanishingPoint", "{}") != PC_OK);
    pc_engine_session_free(s);

    pc_layer_id floor = 0;
    s = mk(&floor);
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)floor);
    CHECK(run_json(s, "layer.select", buf) == PC_OK);
    /* No planes. */
    CHECK(run_json(s, "filter.vanishingPoint", "{}") != PC_OK);
    /* A non-convex (bow-tie) plane. */
    CHECK(run_json(s, "filter.vanishingPoint",
                   "{\"planes\":[{\"corners\":[[20,20],[180,180],[180,20],[20,180]]}]}") != PC_OK);
    /* Too few corners. */
    CHECK(run_json(s, "filter.vanishingPoint", "{\"planes\":[{\"corners\":[[0,0],[1,1]]}]}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_paste_and_clone);
    RUN(test_graceful);
    return pc_t_finish();
}
