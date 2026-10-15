/* warp_cmds::warp_from_params primitive (E9), exercised directly (no command registered). */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "engine/engine_internal.h"
#include "engine/warp_cmds.h"
#include "geom/warp.h"
#include "harness.h"

static const double RECT[4] = {0.0, 0.0, 100.0, 50.0};

static pc_status build(const char *json, pc_warp *out, pc_error *err)
{
    yyjson_doc *jd = NULL;
    if (pc_json_parse(json, strlen(json), &jd, NULL) != PC_OK) {
        return PC_ERR_INVALID;
    }
    pc_status rc = pc_engine_warp_from_params("edit.transform", yyjson_doc_get_root(jd), RECT, out, err);
    yyjson_doc_free(jd);
    return rc;
}

static void test_preset_and_none(void)
{
    pc_warp w;
    CHECK(build("{\"style\":\"arc\",\"bend\":30}", &w, NULL) == PC_OK);
    CHECK_EQ_INT((int)w.style, (int)PC_WARP_ARC);
    CHECK(w.bend == 30.0);
    CHECK(!w.has_mesh);
    CHECK(!pc_warp_is_identity(&w)); /* arc with a bend changes points */
    pc_warp_fini(&w);

    CHECK(build("{\"style\":\"none\"}", &w, NULL) == PC_OK);
    CHECK_EQ_INT((int)w.style, (int)PC_WARP_NONE);
    CHECK(pc_warp_is_identity(&w));
    pc_point p = pc_warp_map(&w, 10.0, 20.0);
    CHECK_NEAR(p.x, 10.0, 1e-9);
    CHECK_NEAR(p.y, 20.0, 1e-9);
    pc_warp_fini(&w);
}

static void test_custom_grid(void)
{
    pc_warp w;
    /* grid implies custom; identity mesh over the rect is an identity warp. */
    CHECK(build("{\"grid\":[1,1]}", &w, NULL) == PC_OK);
    CHECK_EQ_INT((int)w.style, (int)PC_WARP_CUSTOM);
    CHECK(w.has_mesh);
    CHECK(pc_warp_is_identity(&w));
    pc_warp_fini(&w);

    CHECK(build("{\"style\":\"custom\",\"grid\":[2,3]}", &w, NULL) == PC_OK);
    CHECK(w.has_mesh);
    CHECK_EQ_UINT(pc_mesh_nx(&w.mesh), (size_t)(3 * 2 + 1));
    CHECK_EQ_UINT(pc_mesh_ny(&w.mesh), (size_t)(3 * 3 + 1));
    CHECK(pc_mesh_is_valid(&w.mesh));
    pc_warp_fini(&w);
}

static void test_full_warp_object(void)
{
    pc_warp w;
    CHECK(build("{\"warp\":{\"style\":\"none\",\"bounds\":[0,0,100,50]}}", &w, NULL) == PC_OK);
    CHECK_EQ_INT((int)w.style, (int)PC_WARP_NONE);
    CHECK(w.bounds[2] == 100.0 && w.bounds[3] == 50.0);
    CHECK(pc_warp_is_identity(&w));
    pc_warp_fini(&w);
}

static void test_errors(void)
{
    pc_warp w;
    pc_error err;
    CHECK(build("{}", &w, &err) != PC_OK); /* no style/mesh/grid/warp */

    CHECK(build("{\"style\":\"bogus\"}", &w, &err) != PC_OK);
    CHECK_CONTAINS(err.msg, "unknown warp style");

    /* A `warp` object whose mesh has the wrong point count is malformed. */
    CHECK(build("{\"warp\":{\"style\":\"custom\",\"bounds\":[0,0,100,50],"
                "\"mesh\":{\"us\":[0,1],\"vs\":[0,1],\"points\":[[0,0]]}}}",
                &w, &err) != PC_OK);
    CHECK_CONTAINS(err.msg, "malformed warp mesh");

    /* A bad `mesh` param. */
    CHECK(build("{\"style\":\"custom\",\"mesh\":{\"us\":[0,1]}}", &w, &err) != PC_OK);
}

static void test_hostile(void)
{
    const char *cases[] = {
        "null", "[]", "{\"style\":42}", "{\"style\":true,\"grid\":[1,1]}",
        "{\"warp\":42}", "{\"warp\":{}}", "{\"grid\":[-1,-2]}",
        "{\"style\":\"custom\",\"grid\":[99999999,99999999]}",
        "{\"style\":\"arc\",\"bend\":1e300}", "{\"mesh\":{\"us\":[],\"vs\":[],\"points\":[]}}",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_warp w;
        if (build(cases[i], &w, NULL) == PC_OK) {
            pc_warp_fini(&w);
        }
    }
}

int main(int argc, char **argv)
{
    RUN(test_preset_and_none);
    RUN(test_custom_grid);
    RUN(test_full_warp_object);
    RUN(test_errors);
    RUN(test_hostile);
    return pc_t_finish();
}
