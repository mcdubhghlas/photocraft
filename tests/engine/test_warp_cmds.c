/* Edit > Transform > Warp, the split warps and the warp grid (warp_cmds.c command surface).
 *
 * A preset warp deforms the active pixel layer and undoes exactly; an identity warp is a no-op; the
 * split/grid commands edit a mesh passed inline and return it; adversarial params and no-document
 * are graceful errors (never a crash, REWRITE_PLAN.md rule 9). */
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine.h"
#include "engine/engine_internal.h"
#include "engine/warp_cmds.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h, pc_sample_type depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB, depth,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

struct addctx {
    pc_layer_id id;
    pc_pixel_format fmt;
};
static pc_status add_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *e)
{
    struct addctx *c = v;
    pc_layer l;
    if (pc_layer_raster(&l, "L", c->fmt, e) != PC_OK) {
        return e != NULL ? e->code : PC_ERR_NOMEM;
    }
    pc_surface *su = pc_layer_surface_mut(&l);
    float col[4] = {0.2f, 0.4f, 0.8f, 1.0f};
    if (su != NULL) {
        (void)pc_surface_fill_rect(su, pc_rect_from_xywh(8, 8, 40, 30), col, 4, e);
    }
    pc_layer_id id = 0;
    pc_status rc = pc_document_insert_above(doc, NULL, &l, &id, e);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    c->id = id;
    *ha = true;
    *active = id;
    return PC_OK;
}

static pc_layer_id add_layer(pc_engine_session *s, pc_pixel_format fmt)
{
    struct addctx c = {0, fmt};
    CHECK(pc_engine_session_edit(s, "add", add_edit, &c, NULL) == PC_OK);
    return c.id;
}

static pc_rect active_bounds(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d != NULL);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    CHECK(l != NULL);
    const pc_surface *su = pc_layer_surface(l);
    return su != NULL ? pc_surface_content_bounds(su) : PC_RECT_EMPTY;
}

static pc_status run_json(pc_engine_session *s, const char *id, const char *params, bool *changed)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, params, &out, &e);
    if (changed != NULL) {
        *changed = false;
        yyjson_mut_val *root = pc_json_value_root(&out);
        if (root != NULL && yyjson_mut_is_obj(root)) {
            yyjson_mut_val *c = yyjson_mut_obj_get(root, "changed");
            if (c != NULL) {
                *changed = yyjson_mut_get_bool(c);
            }
        }
    }
    pc_json_value_fini(&out);
    return rc;
}

/* Execute and return the length of result.warp.mesh.us (0 if absent). */
static size_t result_mesh_us(pc_engine_session *s, const char *id, const char *params, size_t *vs_len)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    size_t us = 0;
    if (vs_len != NULL) {
        *vs_len = 0;
    }
    if (pc_engine_session_execute_json(s, id, params, &out, &e) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&out);
        yyjson_mut_val *w = root != NULL ? yyjson_mut_obj_get(root, "warp") : NULL;
        yyjson_mut_val *m = w != NULL ? yyjson_mut_obj_get(w, "mesh") : NULL;
        if (m != NULL) {
            yyjson_mut_val *u = yyjson_mut_obj_get(m, "us");
            yyjson_mut_val *vv = yyjson_mut_obj_get(m, "vs");
            if (u != NULL) {
                us = yyjson_mut_arr_size(u);
            }
            if (vs_len != NULL && vv != NULL) {
                *vs_len = yyjson_mut_arr_size(vv);
            }
        }
    }
    pc_json_value_fini(&out);
    return us;
}

static void test_registry(void)
{
    size_t n = 0;
    (void)pc_engine_warp_specs(&n);
    CHECK_EQ_UINT(n, 7);
    CHECK(pc_engine_command_find("edit.transform.warp") != NULL);
    CHECK(pc_engine_command_find("layer.smartObjects.warp") != NULL);
    CHECK(pc_engine_command_find("edit.transform.splitWarpCrosswise") != NULL);
    CHECK(pc_engine_command_find("edit.transform.splitWarpHorizontally") != NULL);
    CHECK(pc_engine_command_find("edit.transform.splitWarpVertically") != NULL);
    CHECK(pc_engine_command_find("edit.transform.removeWarpSplit") != NULL);
    CHECK(pc_engine_command_find("edit.transform.warpGrid") != NULL);
}

/* A preset warp with bend deforms the layer at 8/16/32 bits and undoes exactly. */
static void test_preset_warps_and_undoes(void)
{
    const pc_pixel_format fmts[] = {PC_PIXEL_FORMAT_RGBA8, PC_PIXEL_FORMAT_RGBA16,
                                    PC_PIXEL_FORMAT_RGBA32F};
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t i = 0; i < 3; i++) {
        pc_engine_session *s = mk(64, 48, depths[i]);
        (void)add_layer(s, fmts[i]);
        pc_rect before = active_bounds(s);
        bool changed = false;
        CHECK(run_json(s, "edit.transform.warp", "{\"style\":\"arc\",\"bend\":60}", &changed) == PC_OK);
        CHECK(changed);
        pc_rect after = active_bounds(s);
        /* The arc spreads the content: it is no longer the original box. */
        CHECK(after.x0 != before.x0 || after.y0 != before.y0 || after.x1 != before.x1 ||
              after.y1 != before.y1);
        CHECK(pc_engine_session_undo(s));
        pc_rect undone = active_bounds(s);
        CHECK_EQ_INT(undone.x0, before.x0);
        CHECK_EQ_INT(undone.y0, before.y0);
        CHECK_EQ_INT(undone.x1, before.x1);
        CHECK_EQ_INT(undone.y1, before.y1);
        pc_engine_session_free(s);
    }
}

/* An identity warp (bend 0, no mesh move) reports changed=false on a pixel layer. */
static void test_identity_is_no_op(void)
{
    pc_engine_session *s = mk(64, 48, PC_SAMPLE_TYPE_U8);
    (void)add_layer(s, PC_PIXEL_FORMAT_RGBA8);
    bool changed = true;
    CHECK(run_json(s, "edit.transform.warp", "{\"style\":\"flag\",\"bend\":0}", &changed) == PC_OK);
    CHECK(!changed);
    pc_engine_session_free(s);
}

/* A custom mesh that moves a corner warps the layer. */
static void test_custom_mesh_moves_corner(void)
{
    pc_engine_session *s = mk(64, 48, PC_SAMPLE_TYPE_U8);
    (void)add_layer(s, PC_PIXEL_FORMAT_RGBA8);
    pc_rect before = active_bounds(s);
    bool changed = false;
    /* 1x1 identity mesh over [8,8,48,38] with the bottom-right corner pulled out. */
    CHECK(run_json(s, "edit.transform.warp",
                   "{\"rect\":[8,8,48,38],\"mesh\":{\"us\":[0,1],\"vs\":[0,1],"
                   "\"points\":[[8,8],[21.3,8],[34.6,8],[48,8],"
                   "[8,18],[21.3,18],[34.6,18],[48,18],"
                   "[8,28],[21.3,28],[34.6,28],[48,28],"
                   "[8,38],[21.3,38],[34.6,38],[60,50]]}}",
                   &changed) == PC_OK);
    CHECK(changed);
    pc_rect after = active_bounds(s);
    CHECK(after.x1 >= before.x1); /* the corner pushed the content outwards */
    pc_engine_session_free(s);
}

/* splitWarpCrosswise on an identity custom warp divides the thirds, then the centre: 5 x 5 knots. */
static void test_split_crosswise_inline(void)
{
    pc_engine_session *s = mk(64, 48, PC_SAMPLE_TYPE_U8);
    (void)add_layer(s, PC_PIXEL_FORMAT_RGBA8);
    size_t vs = 0;
    size_t us = result_mesh_us(s, "edit.transform.splitWarpCrosswise",
                               "{\"style\":\"custom\",\"rect\":[0,0,90,60]}", &vs);
    CHECK_EQ_UINT(us, 5);
    CHECK_EQ_UINT(vs, 5);
    pc_engine_session_free(s);
}

/* warpGrid with size 4 on an inline warp fits a 4x4 patch grid: 5 knots per axis. */
static void test_warp_grid_inline(void)
{
    pc_engine_session *s = mk(64, 48, PC_SAMPLE_TYPE_U8);
    (void)add_layer(s, PC_PIXEL_FORMAT_RGBA8);
    size_t vs = 0;
    size_t us = result_mesh_us(s, "edit.transform.warpGrid",
                               "{\"warp\":{\"style\":\"custom\",\"bounds\":[0,0,60,40],"
                               "\"mesh\":{\"us\":[0,1],\"vs\":[0,1],"
                               "\"points\":[[0,0],[20,0],[40,0],[60,0],"
                               "[0,13.3],[20,13.3],[40,13.3],[60,13.3],"
                               "[0,26.6],[20,26.6],[40,26.6],[60,26.6],"
                               "[0,40],[20,40],[40,40],[60,40]]}},\"size\":4}",
                               &vs);
    CHECK_EQ_UINT(us, 5);
    CHECK_EQ_UINT(vs, 5);
    pc_engine_session_free(s);
}

static void test_graceful(void)
{
    /* No document: every warp command is a clean error, not a crash. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(run_json(s, "edit.transform.warp", "{\"style\":\"arc\"}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.transform.splitWarpCrosswise", "{}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.transform.warpGrid", "{\"size\":3}", NULL) != PC_OK);
    pc_engine_session_free(s);

    /* Adversarial params on a real document are errors, never crashes. */
    s = mk(32, 32, PC_SAMPLE_TYPE_U8);
    (void)add_layer(s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(run_json(s, "edit.transform.warp", "{}", NULL) != PC_OK);              /* no style/mesh/warp */
    CHECK(run_json(s, "edit.transform.warp", "{\"style\":\"nope\"}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.transform.warp",
                   "{\"style\":\"arc\",\"rect\":[-1e30,-1e30,1e30,1e30]}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.transform.warp",
                   "{\"mesh\":{\"us\":[0,1],\"vs\":[0,1],\"points\":[[0,0]]}}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.transform.warpGrid", "{\"size\":0}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.transform.warpGrid", "{\"size\":65}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.transform.warpGrid", "{\"size\":\"custom\"}", NULL) != PC_OK);
    CHECK(run_json(s, "edit.transform.warpGrid", "{\"grid\":[3,3]}", NULL) != PC_OK);
    /* Split with no inline warp on a non-smart layer is an error. */
    CHECK(run_json(s, "edit.transform.splitWarpVertically", "{}", NULL) != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registry);
    RUN(test_preset_warps_and_undoes);
    RUN(test_identity_is_no_op);
    RUN(test_custom_mesh_moves_corner);
    RUN(test_split_crosswise_inline);
    RUN(test_warp_grid_inline);
    RUN(test_graceful);
    return pc_t_finish();
}
