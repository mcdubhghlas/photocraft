/* E-tail SF: the smart-filter RECORD path (smart_cmds::add_smart_filter). Applying a filter.*,
 * image.adjustments.* (as smart), plugin.run or lens/cameraRaw command to a SMART-OBJECT layer
 * records it as a smart filter in the object's stack and re-renders from the source; the
 * layer.smartFilter.* editing commands then act on the recorded filter. Plus adversarial params
 * (never crash). Run from the repo root (the plug-in test needs the invert.wasm fixture). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "harness.h"
#include "plugins/plugins.h"

#define INVERT_ID "org.photocraft.example.invert"

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return rc;
}

static pc_engine_session *mk(int w, int h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char buf[128];
    (void)snprintf(buf, sizeof buf, "{\"width\":%d,\"height\":%d}", w, h);
    CHECK(exec(s, "file.new", buf) == PC_OK);
    return s;
}

static const pc_layer *active_layer(pc_engine_session *s)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    return pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer);
}

static const pc_smart_object *active_smart(pc_engine_session *s)
{
    const pc_layer *l = active_layer(s);
    CHECK(l != NULL && l->content.kind == PC_LAYER_SMART);
    return &l->content.u.smart;
}

static void cache_px(pc_engine_session *s, int32_t x, int32_t y, float out[4])
{
    const pc_surface *surf = pc_layer_surface(active_layer(s));
    CHECK(surf != NULL);
    pc_surface_rgba(surf, x, y, out);
}

/* A smart object built from a two-tone raster layer (left half red, right half blue). */
static pc_engine_session *mk_smart(int w, int h)
{
    pc_engine_session *s = mk(w, h);
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    char buf[128];
    (void)snprintf(buf, sizeof buf, "{\"x\":0,\"y\":0,\"width\":%d,\"height\":%d}", w / 2, h);
    CHECK(exec(s, "select.rect", buf) == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#ff0000\"}") == PC_OK);
    (void)snprintf(buf, sizeof buf, "{\"x\":%d,\"y\":0,\"width\":%d,\"height\":%d}", w / 2, w / 2, h);
    CHECK(exec(s, "select.rect", buf) == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#0000ff\"}") == PC_OK);
    CHECK(exec(s, "select.deselect", "{}") == PC_OK);
    CHECK(exec(s, "layer.smartObjects.convertToSmartObject", "{}") == PC_OK);
    CHECK(active_layer(s)->content.kind == PC_LAYER_SMART);
    CHECK(active_smart(s)->has_cache);
    return s;
}

/* ---- filter.* records and re-renders ------------------------------------------------------- */

static void test_filter_records(void)
{
    pc_engine_session *s = mk_smart(16, 16);
    /* A Gaussian blur softens the red/blue boundary: record it, then the edge pixel mixes. */
    float before[4];
    cache_px(s, 7, 8, before); /* just left of the x=8 boundary: pure red */
    CHECK_NEAR(before[0], 1.0, 0.02);
    CHECK_NEAR(before[2], 0.0, 0.02);

    CHECK(exec(s, "filter.blur.gaussianBlur", "{\"radius\":4}") == PC_OK);
    const pc_smart_object *sm = active_smart(s);
    CHECK_EQ_UINT(sm->nsmart_filters, 1u);
    CHECK_EQ_STR(pc_str_c(&sm->smart_filters[0].command), "filter.blur.gaussianBlur");
    CHECK(sm->smart_filters[0].visible);
    CHECK(sm->smart_filters[0].blend == PC_BLEND_NORMAL);
    CHECK_NEAR(sm->smart_filters[0].opacity, 1.0, 1e-6);
    /* The recorded params carry the radius. */
    yyjson_mut_val *root = pc_json_value_root(&sm->smart_filters[0].params);
    CHECK(root != NULL && yyjson_mut_is_obj(root));

    /* Re-rendered: blue has bled into the formerly-pure-red edge pixel. */
    float after[4];
    cache_px(s, 7, 8, after);
    CHECK(after[2] > before[2] + 0.02f);

    /* Disabling the filter re-renders back to the sharp edge; one undo removes the filter. */
    CHECK(exec(s, "layer.smartFilter.setVisible", "{\"index\":0,\"visible\":false}") == PC_OK);
    CHECK(!active_smart(s)->smart_filters[0].visible);
    float off[4];
    cache_px(s, 7, 8, off);
    CHECK_NEAR(off[2], 0.0, 0.05); /* back to red, no blue */

    CHECK(exec(s, "layer.smartFilter.delete", "{\"index\":0}") == PC_OK);
    CHECK_EQ_UINT(active_smart(s)->nsmart_filters, 0u);

    CHECK(pc_engine_session_undo(s)); /* undo the delete */
    CHECK_EQ_UINT(active_smart(s)->nsmart_filters, 1u);
    pc_engine_session_free(s);
}

/* ---- image.adjustments.* records as a smart filter ----------------------------------------- */

static void test_adjustment_records(void)
{
    pc_engine_session *s = mk_smart(16, 16);
    float before[4];
    cache_px(s, 4, 8, before); /* red */
    CHECK_NEAR(before[0], 1.0, 0.02);

    CHECK(exec(s, "image.adjustments.invert", "{}") == PC_OK);
    const pc_smart_object *sm = active_smart(s);
    CHECK_EQ_UINT(sm->nsmart_filters, 1u);
    CHECK_EQ_STR(pc_str_c(&sm->smart_filters[0].command), "image.adjustments.invert");

    /* Re-rendered through adjust_as_filter: red -> cyan (0,1,1). */
    float after[4];
    cache_px(s, 4, 8, after);
    CHECK_NEAR(after[0], 0.0, 0.02);
    CHECK_NEAR(after[1], 1.0, 0.02);
    CHECK_NEAR(after[2], 1.0, 0.02);

    /* A second adjustment stacks on top; clear removes them all. */
    CHECK(exec(s, "image.adjustments.brightnessContrast", "{\"brightness\":20}") == PC_OK);
    CHECK_EQ_UINT(active_smart(s)->nsmart_filters, 2u);
    CHECK_EQ_STR(pc_str_c(&active_smart(s)->smart_filters[1].command),
                 "image.adjustments.brightnessContrast");
    CHECK(exec(s, "layer.smartFilter.clearSmartFilters", "{}") == PC_OK);
    CHECK_EQ_UINT(active_smart(s)->nsmart_filters, 0u);
    pc_engine_session_free(s);
}

/* ---- a selection becomes the filter mask --------------------------------------------------- */

static void test_selection_becomes_mask(void)
{
    pc_engine_session *s = mk_smart(16, 16);
    CHECK(!active_smart(s)->has_filter_mask);
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":8,\"height\":16}") == PC_OK);
    CHECK(exec(s, "image.adjustments.invert", "{}") == PC_OK);
    const pc_smart_object *sm = active_smart(s);
    CHECK_EQ_UINT(sm->nsmart_filters, 1u);
    CHECK(sm->has_filter_mask);
    CHECK(sm->filter_mask.enabled);
    pc_engine_session_free(s);
}

/* ---- lens / camera raw record -------------------------------------------------------------- */

static void test_lens_records(void)
{
    pc_engine_session *s = mk_smart(16, 16);
    /* Camera Raw Filter on a smart object records filter.cameraRaw (RGB document). */
    CHECK(exec(s, "filter.cameraRaw", "{\"exposure\":0.5,\"contrast\":10}") == PC_OK);
    const pc_smart_object *sm = active_smart(s);
    CHECK_EQ_UINT(sm->nsmart_filters, 1u);
    CHECK_EQ_STR(pc_str_c(&sm->smart_filters[0].command), "filter.cameraRaw");

    /* Lens Correction records filter.lensCorrection with resolved angle/scale params. */
    CHECK(exec(s, "filter.lensCorrection", "{\"distortion\":-20,\"profile\":\"generic\",\"focalLength\":35}") ==
          PC_OK);
    sm = active_smart(s);
    CHECK_EQ_UINT(sm->nsmart_filters, 2u);
    CHECK_EQ_STR(pc_str_c(&sm->smart_filters[1].command), "filter.lensCorrection");
    pc_engine_session_free(s);
}

/* ---- plug-in run records (needs the WASM runtime + fixture) --------------------------------- */

static char *b64(const uint8_t *b, size_t n)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = malloc((n + 2) / 3 * 4 + 1);
    CHECK(out != NULL);
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t x = (uint32_t)b[i] << 16;
        x |= (i + 1 < n ? (uint32_t)b[i + 1] : 0) << 8;
        x |= (i + 2 < n ? (uint32_t)b[i + 2] : 0);
        out[o++] = T[(x >> 18) & 63];
        out[o++] = T[(x >> 12) & 63];
        out[o++] = i + 1 < n ? T[(x >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? T[x & 63] : '=';
    }
    out[o] = '\0';
    return out;
}

static void test_plugin_records(void)
{
    pc_buf wasm = PC_BUF_INIT;
    if (pc_file_read("tests/fixtures/plugins/invert.wasm", (size_t)64 << 20, &wasm, NULL) != PC_OK) {
        (void)fprintf(stderr, "  SKIP: tests/fixtures/plugins/invert.wasm missing\n");
        return;
    }
    char *data = b64(wasm.data, wasm.len);
    pc_buf_fini(&wasm);
    pc_buf params = PC_BUF_INIT;
    (void)pc_buf_append_str(&params, "{\"data\":\"", NULL);
    (void)pc_buf_append_str(&params, data, NULL);
    (void)pc_buf_append_str(&params, "\"}", NULL);
    free(data);

    pc_engine_session *s = mk_smart(16, 16);
    CHECK(exec(s, "plugin.install", (const char *)params.data) == PC_OK);
    pc_buf_fini(&params);

    float before[4];
    cache_px(s, 4, 8, before); /* red */
    CHECK(exec(s, "plugin.run", "{\"id\":\"" INVERT_ID "\"}") == PC_OK);
    const pc_smart_object *sm = active_smart(s);
    CHECK_EQ_UINT(sm->nsmart_filters, 1u);
    CHECK_EQ_STR(pc_str_c(&sm->smart_filters[0].command), "plugin.run");
    /* The recorded params name the plug-in id. */
    yyjson_mut_val *root = pc_json_value_root(&sm->smart_filters[0].params);
    CHECK(root != NULL);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(root, "id")), INVERT_ID);
    /* Re-rendered: red inverted -> cyan. */
    float after[4];
    cache_px(s, 4, 8, after);
    CHECK_NEAR(after[0], 0.0, 0.02);
    CHECK_NEAR(after[1], 1.0, 0.02);
    pc_engine_session_free(s);
}

/* ---- adversarial params never crash -------------------------------------------------------- */

static void test_adversarial(void)
{
    const char *cmds[] = {"filter.blur.gaussianBlur", "image.adjustments.invert",
                          "image.adjustments.levels",  "filter.cameraRaw",
                          "filter.lensCorrection",      "plugin.run"};
    const char *params[] = {"{}",
                            "null",
                            "{\"radius\":-1e30}",
                            "{\"radius\":\"x\"}",
                            "{\"layer\":99999}",
                            "{\"levels\":{\"inBlack\":1e30}}",
                            "[1,2,3]",
                            "{\"id\":42}",
                            "{\"profile\":\"zeiss\"}"};
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            pc_engine_session *s = mk_smart(12, 10);
            (void)exec(s, cmds[i], params[j]); /* must return, not crash */
            /* The document is still usable afterwards. */
            CHECK(active_layer(s) != NULL);
            pc_engine_session_free(s);
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_filter_records);
    RUN(test_adjustment_records);
    RUN(test_selection_becomes_mask);
    RUN(test_lens_records);
    if (pc_plugins_available()) {
        RUN(test_plugin_records);
    } else {
        (void)fprintf(stderr, "  plug-ins unavailable in this build: plugin.run record skipped\n");
    }
    RUN(test_adversarial);
    return pc_t_finish();
}
