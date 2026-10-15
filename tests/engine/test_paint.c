/* Paint Bucket and Gradient tool (port of a subset of paint_cmds tests, E10). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h, pc_sample_type depth, float bg)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(w, h), PC_COLOR_MODE_RGB, depth,
                                      pc_color_rgba(bg, bg, bg, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_status run(pc_engine_session *s, const char *id, const char *json)
{
    pc_error e = {0};
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, &e);
    pc_json_value_fini(&out);
    return rc;
}

static void layer_px(pc_engine_session *s, int x, int y, float out[4])
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_surface_rgba(pc_layer_surface(pc_document_layer(doc, d->active_layer)), x, y, out);
}

static void test_bucket_contiguous(void)
{
    pc_engine_session *s = mk(20, 10, PC_SAMPLE_TYPE_U8, 1.0f);
    /* a vertical black wall at x=10 */
    CHECK(run(s, "select.rect", "{\"x\":10,\"y\":0,\"width\":1,\"height\":10}") == PC_OK);
    CHECK(run(s, "edit.fill", "{\"color\":\"#000000\"}") == PC_OK);
    CHECK(run(s, "select.deselect", "{}") == PC_OK);
    CHECK(run(s, "paint.bucket", "{\"x\":2,\"y\":2,\"color\":\"#ff0000\",\"antiAlias\":false}") == PC_OK);
    float a[4], b[4];
    layer_px(s, 5, 5, a);
    layer_px(s, 15, 5, b);
    CHECK_NEAR(a[0], 1.0f, 0.02f);
    CHECK_NEAR(a[1], 0.0f, 0.02f);
    CHECK_NEAR(b[0], 1.0f, 0.02f); /* right of wall stays white */
    CHECK_NEAR(b[1], 1.0f, 0.02f);
    CHECK(run(s, "paint.bucket", "{\"x\":2,\"y\":2,\"color\":\"#0000ff\",\"contiguous\":false,\"antiAlias\":false,\"tolerance\":0}") == PC_OK);
    layer_px(s, 5, 5, a);
    CHECK_NEAR(a[2], 1.0f, 0.02f);
    pc_engine_session_free(s);
}

static void test_bucket_pattern(void)
{
    pc_engine_session *s = mk(20, 10, PC_SAMPLE_TYPE_U8, 1.0f);
    CHECK(run(s, "select.rect", "{\"x\":10,\"y\":0,\"width\":1,\"height\":10}") == PC_OK);
    CHECK(run(s, "edit.fill", "{\"color\":\"#000000\"}") == PC_OK);
    CHECK(run(s, "select.deselect", "{}") == PC_OK);
    CHECK(run(s, "paint.bucket",
              "{\"x\":2,\"y\":2,\"contents\":\"pattern\",\"pattern\":\"Diagonal Lines\",\"antiAlias\":false,\"tolerance\":0}") == PC_OK);
    bool dark = false, light = false;
    for (int x = 0; x < 10; x++) {
        float p[4];
        layer_px(s, x, 0, p);
        if (p[0] < 0.3f) {
            dark = true;
        }
        if (p[0] > 0.9f) {
            light = true;
        }
    }
    CHECK(dark && light); /* pattern varies in the fill */
    float wall[4];
    layer_px(s, 15, 5, wall);
    CHECK_NEAR(wall[0], 1.0f, 0.02f); /* past the wall untouched */
    pc_engine_session_free(s);
}

static void test_bucket_sample_all_layers(void)
{
    pc_engine_session *s = mk(20, 10, PC_SAMPLE_TYPE_U8, 1.0f);
    /* black wall on the Background */
    CHECK(run(s, "select.rect", "{\"x\":10,\"y\":0,\"width\":1,\"height\":10}") == PC_OK);
    CHECK(run(s, "edit.fill", "{\"color\":\"#000000\"}") == PC_OK);
    CHECK(run(s, "select.deselect", "{}") == PC_OK);
    /* empty layer above */
    CHECK(run(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(run(s, "paint.bucket",
              "{\"x\":2,\"y\":2,\"color\":\"#ff0000\",\"antiAlias\":false,\"tolerance\":0,\"sampleAllLayers\":true}") == PC_OK);
    float a[4], b[4];
    layer_px(s, 5, 5, a);
    layer_px(s, 15, 5, b);
    CHECK_NEAR(a[0], 1.0f, 0.02f); /* filled within the visible boundary */
    CHECK(b[3] < 0.02f);           /* beyond the wall: the active layer is still transparent */
    /* an off-canvas sample fills nothing */
    CHECK(run(s, "paint.bucket", "{\"x\":-1,\"y\":2,\"sampleAllLayers\":true}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_gradient_selection_reverse(void)
{
    pc_engine_session *s = mk(20, 10, PC_SAMPLE_TYPE_U8, 1.0f);
    CHECK(run(s, "paint.gradient", "{\"from\":[0,0],\"to\":[20,0],\"colors\":[\"#000000\",\"#ffffff\"]}") == PC_OK);
    float l[4], r[4];
    layer_px(s, 0, 5, l);
    layer_px(s, 19, 5, r);
    CHECK(l[0] < 0.05f && r[0] > 0.95f);
    CHECK(run(s, "paint.gradient", "{\"from\":[0,0],\"to\":[20,0],\"colors\":[\"#000000\",\"#ffffff\"],\"reverse\":true}") == PC_OK);
    layer_px(s, 0, 5, l);
    CHECK(l[0] > 0.95f);
    /* missing `to` errors */
    CHECK(run(s, "paint.gradient", "{\"from\":[0,0]}") != PC_OK);
    pc_engine_session_free(s);
}

static void test_opacity_clamp(void)
{
    /* #4167: opacity above 100 clamps; below 1 clamps; non-numbers error without editing. */
    for (int ci = 0; ci < 2; ci++) {
        const char *cmd = ci == 0 ? "paint.bucket" : "paint.gradient";
        pc_engine_session *full = mk(20, 10, PC_SAMPLE_TYPE_U8, 1.0f);
        pc_engine_session *over = mk(20, 10, PC_SAMPLE_TYPE_U8, 1.0f);
        char j100[160], jover[160];
        if (ci == 0) {
            snprintf(j100, sizeof j100, "{\"x\":2,\"y\":2,\"color\":\"#808080\",\"opacity\":100}");
            snprintf(jover, sizeof jover, "{\"x\":2,\"y\":2,\"color\":\"#808080\",\"opacity\":200}");
        } else {
            snprintf(j100, sizeof j100, "{\"from\":[0,0],\"to\":[19,0],\"colors\":[\"#808080\",\"#808080\"],\"dither\":false,\"opacity\":100}");
            snprintf(jover, sizeof jover, "{\"from\":[0,0],\"to\":[19,0],\"colors\":[\"#808080\",\"#808080\"],\"dither\":false,\"opacity\":200}");
        }
        CHECK(run(full, cmd, j100) == PC_OK);
        CHECK(run(over, cmd, jover) == PC_OK);
        float pf[4], po[4];
        layer_px(full, 5, 5, pf);
        layer_px(over, 5, 5, po);
        CHECK_NEAR(pf[0], 128.0f / 255.0f, 0.02f);
        CHECK_NEAR(po[0], pf[0], 0.01f); /* 200 clamps to 100 */
        /* non-numeric opacity errors, leaves pixels unchanged */
        pc_engine_session *bad = mk(20, 10, PC_SAMPLE_TYPE_U8, 1.0f);
        char jbad[200];
        if (ci == 0) {
            snprintf(jbad, sizeof jbad, "{\"x\":2,\"y\":2,\"color\":\"#808080\",\"opacity\":\"50\"}");
        } else {
            snprintf(jbad, sizeof jbad, "{\"from\":[0,0],\"to\":[19,0],\"colors\":[\"#808080\"],\"opacity\":true}");
        }
        float before[4];
        layer_px(bad, 5, 5, before);
        CHECK(run(bad, cmd, jbad) != PC_OK);
        float after[4];
        layer_px(bad, 5, 5, after);
        CHECK_NEAR(after[0], before[0], 0.001f);
        pc_engine_session_free(full);
        pc_engine_session_free(over);
        pc_engine_session_free(bad);
    }
}

static void test_adversarial(void)
{
    pc_engine_session *s = mk(16, 16, PC_SAMPLE_TYPE_U8, 1.0f);
    const char *cmds[] = {"paint.bucket", "paint.gradient"};
    const char *params[] = {
        "{}", "[]", "null", "\"x\"", "{\"x\":1e308,\"y\":-1e308}",
        "{\"from\":[1e308,0],\"to\":[0,0]}", "{\"gradient\":\"nope\"}",
        "{\"stops\":[]}", "{\"stops\":[[0.5,\"#zz\"]]}", "{\"from\":[0,0],\"to\":[0,0],\"method\":\"bogus\"}",
        "{\"contents\":\"pattern\",\"pattern\":\"nope\"}", "{\"transparency\":\"x\"}",
        "{\"from\":[0,0],\"to\":[1,1],\"colors\":[]}",
    };
    for (size_t c = 0; c < 2; c++) {
        for (size_t pi = 0; pi < sizeof params / sizeof params[0]; pi++) {
            pc_json_value out = PC_JSON_VALUE_INIT;
            pc_error e = {0};
            (void)pc_engine_session_execute_json(s, cmds[c], params[pi], &out, &e);
            pc_json_value_fini(&out);
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_bucket_contiguous);
    RUN(test_bucket_pattern);
    RUN(test_bucket_sample_all_layers);
    RUN(test_gradient_selection_reverse);
    RUN(test_opacity_clamp);
    RUN(test_adversarial);
    return pc_t_finish();
}
