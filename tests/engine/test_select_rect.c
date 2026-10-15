/* select.rect: the Marquee selection primitive (E4). */
#include <stdlib.h>

#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "raster/surface.h"
#include "harness.h"

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return rc;
}

static pc_engine_session *mk(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(exec(s, "file.new", "{\"width\":40,\"height\":30,\"background\":\"white\"}") == PC_OK);
    return s;
}

static float sel_at(pc_engine_session *s, int32_t x, int32_t y)
{
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    if (!doc->has_selection) {
        return -1.0f;
    }
    return pc_surface_sample_channel(&doc->selection, x, y, 0);
}

static void test_rectangle_modes(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":5,\"y\":5,\"width\":10,\"height\":10}") == PC_OK);
    CHECK(sel_at(s, 8, 8) > 0.9f);
    CHECK(sel_at(s, 20, 20) <= 0.0f);
    /* Add a second rectangle. */
    CHECK(exec(s, "select.rect", "{\"x\":20,\"y\":20,\"width\":5,\"height\":5,\"mode\":\"add\"}") == PC_OK);
    CHECK(sel_at(s, 8, 8) > 0.9f);
    CHECK(sel_at(s, 22, 22) > 0.9f);
    /* Intersect leaves only the overlap (none here) -> deselects. */
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":3,\"height\":3,\"mode\":\"intersect\"}") ==
          PC_OK);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(!doc->has_selection);
    pc_engine_session_free(s);
}

static void test_ellipse_and_feather(void)
{
    pc_engine_session *s = mk();
    CHECK(exec(s, "select.rect", "{\"x\":5,\"y\":5,\"width\":20,\"height\":20,\"ellipse\":true}") == PC_OK);
    /* Centre is inside the ellipse, a corner is not. */
    CHECK(sel_at(s, 15, 15) > 0.9f);
    CHECK(sel_at(s, 6, 6) <= 0.5f);
    CHECK(exec(s, "select.rect", "{\"x\":10,\"y\":10,\"width\":10,\"height\":10,\"feather\":3}") == PC_OK);
    CHECK(sel_at(s, 15, 15) > 0.5f);
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk();
    const char *bad[] = {
        "{}",                                         /* missing x */
        "{\"x\":1,\"y\":2}",                          /* missing width/height */
        "{\"x\":\"a\",\"y\":2,\"width\":3,\"height\":3}",
        "{\"x\":1,\"y\":2,\"width\":3,\"height\":3,\"line\":\"diag\"}",
        "{\"x\":5000000000,\"y\":0,\"width\":1,\"height\":1}", /* out of i32 range */
        "[]", "\"x\"", "3",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(exec(s, "select.rect", bad[i]) != PC_OK);
    }
    /* A zero-size rectangle with no existing selection deselects cleanly, no crash. */
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":0,\"height\":0}") == PC_OK);
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(exec(empty, "select.rect", "{\"x\":0,\"y\":0,\"width\":1,\"height\":1}") != PC_OK);
    pc_engine_session_free(empty);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_rectangle_modes);
    RUN(test_ellipse_and_feather);
    RUN(test_adversarial);
    return pc_t_finish();
}
