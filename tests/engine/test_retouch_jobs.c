/* The Patch, Content-Aware Move and Remove tools as commands (ET4, port of
 * crates/engine/src/retouch_cmds/{patch,content_aware_move,remove}.rs). Covers registration +
 * gating, a working patch/move/remove on a selection, and the mandatory never-crash sweep over
 * hostile params. These run synchronously (§7) rather than as background jobs. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static const char *IDS[] = {"paint.patch", "paint.contentAwareMove", "paint.remove"};

static pc_engine_session *mk(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, &err);
    pc_json_value_fini(&out);
    return rc;
}

static void ok(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    pc_json_value_fini(&out);
}

/* Paint a few opaque pixels into the background so Content-Aware has a texture to sample. */
static pc_status noise_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *e)
{
    (void)ha;
    (void)v;
    pc_surface *surf = pc_layer_surface_mut(pc_document_layer_mut(doc, *a));
    if (surf == NULL) {
        return pc_fail(e, PC_ERR_INVALID, "no surface");
    }
    uint32_t w = (uint32_t)pc_rect_width(pc_document_bounds(doc));
    uint32_t h = (uint32_t)pc_rect_height(pc_document_bounds(doc));
    size_t n = (size_t)w * h * 4;
    float *buf = malloc(n * sizeof(float));
    if (buf == NULL) {
        return pc_fail(e, PC_ERR_NOMEM, "buf");
    }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            float g = (float)((x * 7 + y * 13) % 17) / 16.0f;
            float *p = &buf[(y * w + x) * 4];
            p[0] = g;
            p[1] = 1.0f - g;
            p[2] = 0.5f;
            p[3] = 1.0f;
        }
    }
    pc_status rc = pc_surface_write_region(surf, pc_rect_from_xywh(0, 0, w, h), buf, n, e);
    free(buf);
    return rc;
}

static void noise(pc_engine_session *s)
{
    pc_error err = {0};
    CHECK(pc_engine_session_edit(s, "noise", noise_edit, NULL, &err) == PC_OK);
}

/* ---- registration + gating ---- */

static void test_registered_and_gated(void)
{
    pc_engine_session *s = mk(40, 40);
    /* Without a selection, Patch and Content-Aware Move are disabled. */
    CHECK(!pc_engine_session_is_enabled(s, "paint.patch"));
    CHECK(!pc_engine_session_is_enabled(s, "paint.contentAwareMove"));
    /* Remove only needs a pixel layer. */
    CHECK(pc_engine_session_is_enabled(s, "paint.remove"));
    ok(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":10}");
    CHECK(pc_engine_session_is_enabled(s, "paint.patch"));
    CHECK(pc_engine_session_is_enabled(s, "paint.contentAwareMove"));
    pc_engine_session_free(s);
}

static void test_patch_runs(void)
{
    pc_engine_session *s = mk(48, 48);
    noise(s);
    ok(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":10,\"height\":10}");
    /* Source mode: repair the selection from pixels 16 px to the right/down. */
    ok(s, "paint.patch", "{\"offset\":[16,16]}");
    /* Destination mode too. */
    ok(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":10,\"height\":10}");
    ok(s, "paint.patch", "{\"offset\":[16,16],\"mode\":\"destination\"}");
    /* Content-Aware patch (the synchronous job path). */
    ok(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":10,\"height\":10}");
    ok(s, "paint.patch", "{\"offset\":[16,16],\"contentAware\":true,\"structure\":4,\"color\":3}");
    pc_engine_session_free(s);
}

static void test_content_aware_move_runs(void)
{
    pc_engine_session *s = mk(48, 48);
    noise(s);
    ok(s, "select.rect", "{\"x\":8,\"y\":8,\"width\":10,\"height\":10}");
    ok(s, "paint.contentAwareMove", "{\"offset\":[12,12],\"mode\":\"move\"}");
    /* Extend mode. */
    ok(s, "select.rect", "{\"x\":8,\"y\":8,\"width\":10,\"height\":10}");
    ok(s, "paint.contentAwareMove", "{\"offset\":[12,0],\"mode\":\"extend\"}");
    /* Transform On Drop (scale + rotate): exercises the projective path + selection warp. */
    ok(s, "select.rect", "{\"x\":8,\"y\":8,\"width\":10,\"height\":10}");
    ok(s, "paint.contentAwareMove", "{\"offset\":[6,6],\"scaleX\":120,\"scaleY\":120,\"rotate\":15}");
    pc_engine_session_free(s);
}

static void test_remove_runs(void)
{
    pc_engine_session *s = mk(48, 48);
    noise(s);
    /* A short stroke; the footprint is replaced from the surroundings. */
    ok(s, "paint.remove", "{\"points\":[[20,20],[24,24]],\"size\":6}");
    /* Several strokes in one removal. */
    ok(s, "paint.remove", "{\"strokes\":[[[10,10],[14,10]],[[30,30]]],\"size\":5}");
    /* New-layer variant. */
    ok(s, "paint.remove", "{\"points\":[[20,30],[24,30]],\"size\":6,\"newLayer\":true}");
    pc_engine_session_free(s);
}

/* ---- never-crash on hostile params ---- */

static void test_adversarial_params(void)
{
    static const char *HOSTILE[] = {
        "{}",
        "null",
        "[]",
        "{\"offset\":[0,0]}",
        "{\"offset\":[\"x\",\"y\"]}",
        "{\"offset\":[1e300,1e300]}",
        "{\"offset\":[2000000000,2000000000]}",
        "{\"offset\":[12,12],\"mode\":\"bogus\"}",
        "{\"offset\":[12,12],\"structure\":99}",
        "{\"offset\":[12,12],\"color\":-4}",
        "{\"offset\":[12,12],\"scaleX\":0}",
        "{\"offset\":[12,12],\"scaleX\":1e9}",
        "{\"offset\":[12,12],\"rotate\":1e9}",
        "{\"offset\":[12,12],\"scaleX\":\"x\"}",
        "{\"offset\":[12,12],\"contentAware\":true}",
        "{\"points\":[]}",
        "{\"points\":\"nope\"}",
        "{\"points\":[[1e18,1e18]]}",
        "{\"points\":[[1,1]],\"size\":-5}",
        "{\"points\":[[1,1]],\"size\":999999}",
        "{\"strokes\":[]}",
        "{\"strokes\":\"nope\"}",
        "{\"strokes\":[[[1,1]]],\"newLayer\":true}",
        "{\"points\":[[20,20]],\"target\":{\"channel\":9999}}",
        "{\"offset\":[12,12],\"layer\":999999}",
    };
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        for (int with_sel = 0; with_sel < 2; with_sel++) {
            for (size_t j = 0; j < sizeof HOSTILE / sizeof HOSTILE[0]; j++) {
                pc_engine_session *s = mk(40, 40);
                noise(s);
                if (with_sel) {
                    ok(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":12,\"height\":12}");
                }
                /* Must return a status (not crash) for any params or document state. */
                (void)exec(s, IDS[i], HOSTILE[j]);
                pc_engine_session_free(s);
            }
        }
    }
    CHECK(true);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_registered_and_gated);
    RUN(test_patch_runs);
    RUN(test_content_aware_move_runs);
    RUN(test_remove_runs);
    RUN(test_adversarial_params);
    return pc_t_finish();
}
