/* Retouching tools as commands (port of a subset of crates/engine/src/retouch_cmds tests, plus the
 * mandatory adversarial-params sweep). Covers Clone Stamp, Pattern Stamp, Healing Brush, Spot
 * Healing, Dodge/Burn/Sponge/Blur/Sharpen/Smudge and History Brush: registration + pixel-layer
 * gating, exact clone, tone direction, history restore, pattern paint, and never-crash on hostile
 * params. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

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

static void ok(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    pc_json_value_fini(&out);
}

static void px(pc_engine_session *s, int x, int y, float out[4])
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    pc_surface_rgba(pc_layer_surface(l), x, y, out);
}

/* ---- painting the active layer from a function ---- */

typedef void (*pixel_fn)(int x, int y, float out[4]);

struct paint_ctx {
    pixel_fn fn;
    uint32_t w, h;
};

static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *e)
{
    (void)ha;
    struct paint_ctx *c = v;
    pc_surface *surf = pc_layer_surface_mut(pc_document_layer_mut(doc, *a));
    if (surf == NULL) {
        return pc_fail(e, PC_ERR_INVALID, "no surface");
    }
    size_t n = (size_t)c->w * c->h * 4;
    float *buf = malloc(n * sizeof(float));
    if (buf == NULL) {
        return pc_fail(e, PC_ERR_NOMEM, "buf");
    }
    for (uint32_t y = 0; y < c->h; y++) {
        for (uint32_t x = 0; x < c->w; x++) {
            c->fn((int)x, (int)y, &buf[(y * c->w + x) * 4]);
        }
    }
    pc_status rc =
        pc_surface_write_region(surf, pc_rect_from_xywh(0, 0, c->w, c->h), buf, n, e);
    free(buf);
    return rc;
}

static void paint(pc_engine_session *s, uint32_t w, uint32_t h, pixel_fn fn)
{
    struct paint_ctx c = {fn, w, h};
    CHECK(pc_engine_session_edit(s, "setup", paint_edit, &c, NULL) == PC_OK);
}

/* ---- textures ---- */

static void tex(int x, int y, float out[4])
{
    out[0] = (float)(x % 17) / 16.0f;
    out[1] = (float)(y % 13) / 12.0f;
    out[2] = 0.5f;
    out[3] = 1.0f;
}

static void midgray(int x, int y, float out[4])
{
    (void)x;
    (void)y;
    out[0] = out[1] = out[2] = 0.5f;
    out[3] = 1.0f;
}

static void gray_with_blemish(int x, int y, float out[4])
{
    midgray(x, y, out);
    if (x >= 30 && x < 34 && y >= 30 && y < 34) {
        out[0] = out[1] = out[2] = 0.95f; /* a bright spot */
    }
}

/* ---- tests ---- */

static const char *IDS[] = {"paint.cloneStamp", "paint.patternStamp", "paint.healingBrush",
                            "paint.spotHealing", "paint.dodge",       "paint.burn",
                            "paint.sponge",      "paint.blur",        "paint.sharpen",
                            "paint.smudge",      "paint.historyBrush"};

static void test_registered_and_need_pixel_layer(void)
{
    pc_engine_session *s = mk(64, 64);
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        /* With a raster background layer the command is enabled. */
        CHECK(pc_engine_session_is_enabled(s, IDS[i]));
    }
    pc_engine_session_free(s);

    /* Without a document nothing is enabled, but the ids are still registered (disabled, not
     * unknown). */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        pc_error why = {0};
        CHECK(!pc_engine_session_disabled_reason(empty, IDS[i], &why) ||
              why.code != PC_ERR_INVALID || strstr(why.msg, "unknown") == NULL);
    }
    pc_engine_session_free(empty);
}

static void test_clone_stamp_copies_translation(void)
{
    pc_engine_session *s = mk(96, 64);
    paint(s, 96, 64, tex);
    /* A single hard dab at (60,32) sampling 40 px to its left copies that pixel exactly. */
    ok(s, "paint.cloneStamp",
       "{\"points\":[[60,32]],\"size\":9,\"hardness\":100,\"opacity\":100,\"flow\":100,"
       "\"offset\":[-40,0],\"aligned\":false}");
    float got[4], want[4];
    px(s, 60, 32, got);
    tex(20, 32, want);
    CHECK_NEAR(got[0], want[0], 2.0f / 255.0f);
    CHECK_NEAR(got[1], want[1], 2.0f / 255.0f);
    CHECK_NEAR(got[2], want[2], 2.0f / 255.0f);
    pc_engine_session_free(s);
}

static void test_clone_far_source_is_a_plain_error(void)
{
    /* Issue #1111: a translation whose offset saturates Rect::translate must be a plain bad-params
     * error, never a crash, and leave the layer untouched. A source far outside the canvas but
     * inside the i32 range still works (it clones transparency). */
    const char *cmds[] = {"paint.cloneStamp", "paint.healingBrush"};
    const char *bad_cases[] = {
        "{\"points\":[[10,10]],\"source\":[3e9,0]}",
        "{\"points\":[[10,10]],\"source\":[0,-3e9]}",
        "{\"points\":[[10,10]],\"offset\":[2147483647,0]}",
        "{\"points\":[[10,10],[30,10]],\"source\":[2147483640,0]}",
    };
    for (size_t ci = 0; ci < 2; ci++) {
        pc_engine_session *s = mk(40, 30);
        paint(s, 40, 30, tex);
        float before[4];
        px(s, 10, 10, before);
        for (size_t i = 0; i < sizeof bad_cases / sizeof bad_cases[0]; i++) {
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error err = {0};
            pc_status rc = pc_engine_session_execute_json(s, cmds[ci], bad_cases[i], &out, &err);
            CHECK(rc != PC_OK);
            pc_json_value_fini(&out);
        }
        float after[4];
        px(s, 10, 10, after);
        CHECK_NEAR(after[0], before[0], 1e-6f); /* refused strokes left the layer alone */
        /* A far-but-in-range source is fine. */
        ok(s, cmds[ci], "{\"points\":[[10,10]],\"source\":[1000000,1000000]}");
        pc_engine_session_free(s);
    }
}

static void test_healing_brush_runs(void)
{
    pc_engine_session *s = mk(64, 64);
    paint(s, 64, 64, tex);
    ok(s, "paint.healingBrush",
       "{\"points\":[[40,32]],\"size\":11,\"hardness\":80,\"offset\":[-20,0]}");
    pc_engine_session_free(s);
}

static void test_spot_healing_reduces_a_blemish(void)
{
    const char *types[] = {"contentAware", "createTexture", "proximityMatch"};
    for (size_t i = 0; i < 3; i++) {
        pc_engine_session *s = mk(64, 64);
        paint(s, 64, 64, gray_with_blemish);
        float before[4];
        px(s, 32, 32, before);
        char json[160];
        snprintf(json, sizeof json,
                 "{\"points\":[[32,32]],\"size\":15,\"hardness\":90,\"type\":\"%s\"}", types[i]);
        ok(s, "paint.spotHealing", json);
        float after[4];
        px(s, 32, 32, after);
        /* The blemish (0.95) is pulled toward the 0.5 surroundings. */
        CHECK(fabsf(after[0] - 0.5f) < fabsf(before[0] - 0.5f));
        pc_engine_session_free(s);
    }
}

static void test_dodge_brightens_and_burn_darkens(void)
{
    pc_engine_session *s = mk(48, 48);
    paint(s, 48, 48, midgray);
    float before[4];
    px(s, 24, 24, before);
    ok(s, "paint.dodge",
       "{\"points\":[[24,24]],\"size\":11,\"hardness\":100,\"exposure\":80,\"range\":\"midtones\"}");
    float dodged[4];
    px(s, 24, 24, dodged);
    CHECK(dodged[0] > before[0] + 0.01f);
    pc_engine_session_free(s);

    pc_engine_session *b = mk(48, 48);
    paint(b, 48, 48, midgray);
    ok(b, "paint.burn",
       "{\"points\":[[24,24]],\"size\":11,\"hardness\":100,\"exposure\":80,\"range\":\"midtones\"}");
    float burned[4];
    px(b, 24, 24, burned);
    CHECK(burned[0] < before[0] - 0.01f);
    pc_engine_session_free(b);
}

static void test_sponge_blur_sharpen_smudge_run(void)
{
    pc_engine_session *s = mk(64, 64);
    paint(s, 64, 64, tex);
    ok(s, "paint.sponge", "{\"points\":[[32,32]],\"size\":11,\"mode\":\"desaturate\"}");
    ok(s, "paint.blur", "{\"points\":[[20,20],[40,40]],\"size\":9,\"strength\":80}");
    ok(s, "paint.sharpen", "{\"points\":[[20,20],[40,40]],\"size\":9,\"strength\":80}");
    ok(s, "paint.smudge", "{\"points\":[[10,10],[50,50]],\"size\":9,\"strength\":70}");
    pc_engine_session_free(s);
}

static void test_history_brush_restores_earlier_pixels(void)
{
    pc_engine_session *s = mk(48, 48);
    /* State 0 is the white "Open" snapshot; paint a black dab over it (one history step). */
    ok(s, "paint.stroke", "{\"points\":[[24,24]],\"size\":15,\"hardness\":100,\"opacity\":100,\"flow\":100}");
    float painted[4];
    px(s, 24, 24, painted);
    CHECK(painted[0] < 0.5f); /* darkened */
    ok(s, "paint.historyBrush",
       "{\"points\":[[24,24]],\"size\":15,\"hardness\":100,\"opacity\":100,\"flow\":100,\"state\":0}");
    float restored[4];
    px(s, 24, 24, restored);
    CHECK(restored[0] > 0.9f); /* back to white */
    pc_engine_session_free(s);
}

static void test_pattern_stamp_paints(void)
{
    pc_engine_session *s = mk(64, 64);
    paint(s, 64, 64, midgray);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = pc_engine_session_execute_json(
        s, "paint.patternStamp", "{\"points\":[[20,20],[44,44]],\"size\":13,\"opacity\":100,\"flow\":100}",
        &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(yyjson_mut_obj_get(root, "damage") != NULL);
    CHECK(yyjson_mut_obj_get(root, "pattern") != NULL);
    pc_json_value_fini(&out);
    /* A pattern (not mid-grey) was laid down along the stroke centre. */
    float c[4];
    px(s, 32, 32, c);
    CHECK(fabsf(c[0] - 0.5f) + fabsf(c[1] - 0.5f) + fabsf(c[2] - 0.5f) > 0.01f);
    pc_engine_session_free(s);
}

static void test_adversarial_params(void)
{
    static const char *HOSTILE[] = {
        "{}",
        "{\"points\":[]}",
        "{\"points\":[[1,1,1,1,1]]}",
        "{\"points\":[[1e18,1e18]]}",
        "{\"points\":[[1,1]],\"size\":-5}",
        "{\"points\":[[1,1]],\"size\":999999}",
        "{\"points\":[[1,1]],\"mode\":\"bogus\"}",
        "{\"points\":[[1,1]],\"source\":[1e300,1e300]}",
        "{\"points\":[[1,1]],\"offset\":[2000000000,2000000000]}",
        "{\"points\":[[1,1]],\"range\":\"bogus\"}",
        "{\"points\":[[1,1]],\"type\":\"bogus\"}",
        "{\"points\":[[1,1]],\"scale\":0}",
        "{\"points\":[[1,1]],\"phase\":[\"x\"]}",
        "{\"points\":[[1,1]],\"state\":99999}",
        "{\"points\":[[1,1]],\"sampleLayer\":\"bogus\"}",
        "{\"points\":\"nope\"}",
        "{\"points\":[[1,1]],\"target\":{\"channel\":9999}}",
        "null",
    };
    for (size_t i = 0; i < sizeof IDS / sizeof IDS[0]; i++) {
        for (size_t j = 0; j < sizeof HOSTILE / sizeof HOSTILE[0]; j++) {
            pc_engine_session *s = mk(40, 40);
            paint(s, 40, 40, midgray);
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error err = {0};
            /* Must return a status (not crash) for any params or document state. */
            (void)pc_engine_session_execute_json(s, IDS[i], HOSTILE[j], &out, &err);
            pc_json_value_fini(&out);
            pc_engine_session_free(s);
        }
    }
    CHECK(true);
}

int main(int argc, char **argv)
{
    RUN(test_registered_and_need_pixel_layer);
    RUN(test_clone_stamp_copies_translation);
    RUN(test_clone_far_source_is_a_plain_error);
    RUN(test_healing_brush_runs);
    RUN(test_spot_healing_reduces_a_blemish);
    RUN(test_dodge_brightens_and_burn_darkens);
    RUN(test_sponge_blur_sharpen_smudge_run);
    RUN(test_history_brush_restores_earlier_pixels);
    RUN(test_pattern_stamp_paints);
    RUN(test_adversarial_params);
    return pc_t_finish();
}
