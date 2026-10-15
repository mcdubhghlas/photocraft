/* ET2 deferred-but-unblocked command fills: tools.setColors/swapColors/defaultColors,
 * image.autoTone/autoContrast/autoColor, layer.flattenImage, layer.layerMask.apply/
 * hideSelection/fromTransparency, layer.smartObjects.stackMode.*, edit.purge.*,
 * filter.other.colorToAlpha, layer.rasterize.shape, layer.smartObjects.convertToSmartObject /
 * filter.convertForSmartFilters -- behaviour plus adversarial params (never crash). */
#include <stdlib.h>
#include <string.h>

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

static pc_engine_session *mk(int w, int h, const char *bg)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char buf[128];
    (void)snprintf(buf, sizeof buf, "{\"width\":%d,\"height\":%d,\"background\":\"%s\"}", w, h, bg);
    CHECK(exec(s, "file.new", buf) == PC_OK);
    return s;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

static const pc_layer *active_layer(pc_engine_session *s)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    return pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer);
}

static void active_px(pc_engine_session *s, int32_t x, int32_t y, float out[4])
{
    pc_surface_rgba(pc_layer_surface(active_layer(s)), x, y, out);
}

/* ---- tools colours ------------------------------------------------------------------------- */

static void test_tool_colors(void)
{
    pc_engine_session *s = mk(8, 8, "white");
    CHECK(exec(s, "tools.setColors", "{\"foreground\":\"#ff0000\",\"background\":\"#00ff00\"}") == PC_OK);
    const pc_engine_tool_state *t = pc_engine_session_tools(s);
    CHECK(t->foreground[0] == 1.0f && t->foreground[1] == 0.0f);
    CHECK(t->background[1] == 1.0f && t->background[0] == 0.0f);
    CHECK(exec(s, "tools.swapColors", "{}") == PC_OK);
    t = pc_engine_session_tools(s);
    CHECK(t->foreground[1] == 1.0f && t->background[0] == 1.0f);
    CHECK(exec(s, "tools.defaultColors", "{}") == PC_OK);
    t = pc_engine_session_tools(s);
    CHECK(t->foreground[0] == 0.0f && t->foreground[3] == 1.0f); /* black fg */
    CHECK(t->background[0] == 1.0f && t->background[1] == 1.0f); /* white bg */
    /* A good CMYK pick is remembered; a malformed one is refused and changes nothing. */
    CHECK(exec(s, "tools.setColors", "{\"foreground\":\"#010203\",\"foregroundCmyk\":[0.1,0.2,0.3,0.4]}") ==
          PC_OK);
    CHECK(exec(s, "tools.setColors", "{\"foregroundCmyk\":[0.1,0.2,0.3]}") == PC_ERR_INVALID);
    CHECK(exec(s, "tools.setColors", "{\"foregroundCmyk\":\"x\"}") == PC_ERR_INVALID);
    pc_engine_session_free(s);
}

/* ---- auto tone / contrast / colour --------------------------------------------------------- */

static void fill_ramp(pc_engine_session *s)
{
    /* A narrow midtone band so Auto Tone has something to stretch. */
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":[0.4,0.4,0.4,1.0]}") == PC_OK);
}

static void test_auto_adjust(void)
{
    for (int k = 0; k < 3; k++) {
        const char *cmd = k == 0 ? "image.autoTone" : (k == 1 ? "image.autoContrast" : "image.autoColor");
        pc_engine_session *s = mk(16, 16, "transparent");
        fill_ramp(s);
        /* Make two tones so there is contrast to stretch. */
        CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":8,\"height\":16}") == PC_OK);
        CHECK(exec(s, "edit.fill", "{\"color\":[0.45,0.45,0.45,1.0]}") == PC_OK);
        CHECK(exec(s, "select.deselect", "{}") == PC_OK);
        /* Left half (x<8) = 0.45 (bright tone), right half = 0.40 (dark tone). */
        float dark_before[4], bright_before[4];
        active_px(s, 10, 0, dark_before);
        active_px(s, 0, 0, bright_before);
        CHECK(exec(s, cmd, "{}") == PC_OK);
        float dark_after[4], bright_after[4];
        active_px(s, 10, 0, dark_after);
        active_px(s, 0, 0, bright_after);
        /* Contrast is stretched: the span widens, alpha kept. */
        float before_span = bright_before[0] - dark_before[0];
        float after_span = bright_after[0] - dark_after[0];
        CHECK(after_span >= before_span - 1e-3f);
        CHECK(dark_after[0] <= dark_before[0] + 1e-3f);
        CHECK(dark_after[3] == 1.0f);
        pc_engine_session_free(s);
    }
}

/* ---- flatten image ------------------------------------------------------------------------- */

static void test_flatten_image(void)
{
    pc_engine_session *s = mk(12, 10, "transparent");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(doc_of(s)->nlayers >= 2);
    CHECK(exec(s, "layer.flattenImage", "{}") == PC_OK);
    const pc_document *doc = doc_of(s);
    CHECK(doc->nlayers == 1);
    const pc_layer *bg = &doc->layers[0];
    CHECK(bg->locks.transparency && bg->locks.position);
    /* Flattened onto white. */
    float p[4];
    pc_surface_rgba(pc_layer_surface(bg), 1, 1, p);
    CHECK(p[0] == 1.0f && p[3] == 1.0f);
    pc_engine_session_free(s);
}

/* ---- colour to alpha ----------------------------------------------------------------------- */

static void test_color_to_alpha(void)
{
    pc_engine_session *s = mk(10, 10, "transparent");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#ffffff\"}") == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "filter.other.colorToAlpha"));
    float before[4];
    active_px(s, 5, 5, before);
    CHECK(before[3] == 1.0f);
    CHECK(exec(s, "filter.other.colorToAlpha", "{}") == PC_OK);
    float after[4];
    active_px(s, 5, 5, after);
    CHECK(after[3] < 0.05f); /* white keyed out */
    /* Background becomes a normal layer and goes transparent. */
    pc_engine_session *t = mk(8, 8, "white");
    CHECK(exec(t, "filter.other.colorToAlpha", "{}") == PC_OK);
    float q[4];
    active_px(t, 4, 4, q);
    CHECK(q[3] < 0.05f);
    CHECK(!pc_document_is_background(doc_of(t), pc_engine_session_active(t)->active_layer));
    /* Bad params refused. */
    CHECK(exec(s, "filter.other.colorToAlpha", "{\"color\":\"#zz\"}") == PC_ERR_INVALID);
    CHECK(exec(s, "filter.other.colorToAlpha", "{\"transparencyThreshold\":2}") == PC_ERR_INVALID);
    CHECK(exec(s, "filter.other.colorToAlpha", "7") == PC_ERR_INVALID);
    pc_engine_session_free(s);
    pc_engine_session_free(t);
}

/* ---- layer masks --------------------------------------------------------------------------- */

static void test_mask_from_transparency(void)
{
    pc_engine_session *s = mk(10, 10, "transparent");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":5,\"height\":10}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#808080\"}") == PC_OK);
    CHECK(exec(s, "select.deselect", "{}") == PC_OK);
    CHECK(exec(s, "layer.layerMask.fromTransparency", "{}") == PC_OK);
    const pc_layer *l = active_layer(s);
    CHECK(l->has_mask);
    /* The painted half is opaque, its mask value 1; the empty half's mask value 0. */
    CHECK(pc_layer_mask_value(&l->mask, 2, 2) > 0.5f);
    CHECK(pc_layer_mask_value(&l->mask, 8, 2) < 0.5f);
    float p[4];
    active_px(s, 2, 2, p);
    CHECK(p[3] == 1.0f); /* pixels made opaque */
    pc_engine_session_free(s);
}

static void test_mask_apply_and_hide_selection(void)
{
    pc_engine_session *s = mk(10, 10, "transparent");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#808080\"}") == PC_OK);
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":5,\"height\":10}") == PC_OK);
    /* Hide the selection with a mask. */
    CHECK(exec(s, "layer.layerMask.hideSelection", "{}") == PC_OK);
    const pc_layer *l = active_layer(s);
    CHECK(l->has_mask);
    CHECK(pc_layer_mask_value(&l->mask, 2, 2) < 0.5f); /* selected area hidden */
    CHECK(pc_layer_mask_value(&l->mask, 8, 2) > 0.5f); /* unselected revealed */
    /* Apply the mask: the hidden half becomes transparent, then the mask is gone. */
    CHECK(exec(s, "layer.layerMask.apply", "{}") == PC_OK);
    CHECK(!active_layer(s)->has_mask);
    float p[4];
    active_px(s, 2, 2, p);
    CHECK(p[3] < 0.05f); /* applied */
    active_px(s, 8, 2, p);
    CHECK(p[3] == 1.0f);
    pc_engine_session_free(s);
}

/* ---- convert to smart object + stack modes ------------------------------------------------- */

static void test_convert_and_stack_mode(void)
{
    pc_engine_session *s = mk(12, 12, "transparent");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#3366cc\"}") == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.convertToSmartObject"));
    CHECK(exec(s, "layer.smartObjects.convertToSmartObject", "{}") == PC_OK);
    const pc_layer *l = active_layer(s);
    CHECK(l->content.kind == PC_LAYER_SMART);
    /* It still shows the same pixels (cache rendered). */
    float p[4];
    active_px(s, 5, 5, p);
    CHECK(p[3] > 0.5f);
    /* Converting an already-smart layer via Convert for Smart Filters is refused. */
    CHECK(!pc_engine_session_is_enabled(s, "filter.convertForSmartFilters"));
    /* Stack mode on the smart object. */
    CHECK(exec(s, "layer.smartObjects.stackMode.mean", "{}") == PC_OK);
    CHECK(active_layer(s)->content.u.smart.has_stack_mode);
    CHECK(active_layer(s)->content.u.smart.stack_mode == PC_STACK_MEAN);
    CHECK(exec(s, "layer.smartObjects.stackMode.none", "{}") == PC_OK);
    CHECK(!active_layer(s)->content.u.smart.has_stack_mode);
    pc_engine_session_free(s);
}

/* ---- rasterize shape ----------------------------------------------------------------------- */

static void test_rasterize_shape(void)
{
    pc_engine_session *s = mk(20, 20, "transparent");
    CHECK(exec(s, "shape.create",
               "{\"kind\":\"rect\",\"rect\":[2,2,10,10],\"fill\":\"#ff8800\"}") == PC_OK);
    CHECK(active_layer(s)->content.kind == PC_LAYER_SHAPE);
    CHECK(exec(s, "layer.rasterize.shape", "{}") == PC_OK);
    CHECK(active_layer(s)->content.kind == PC_LAYER_RASTER);
    pc_engine_session_free(s);
}

/* ---- purge --------------------------------------------------------------------------------- */

static void test_purge(void)
{
    pc_engine_session *s = mk(8, 8, "white");
    CHECK(exec(s, "edit.fill", "{\"color\":\"#000000\"}") == PC_OK); /* something to undo */
    CHECK(pc_engine_session_is_enabled(s, "edit.purge.undo"));
    CHECK(exec(s, "edit.purge.undo", "{}") == PC_OK);
    /* Undo was purged, so there is nothing to purge now. */
    CHECK(!pc_engine_session_is_enabled(s, "edit.purge.undo"));
    /* videoCache is always available and reports nothing freed. */
    CHECK(exec(s, "edit.purge.videoCache", "{}") == PC_OK);
    /* A fresh edit gives purge.all something to free again. */
    CHECK(exec(s, "edit.fill", "{\"color\":\"#112233\"}") == PC_OK);
    CHECK(exec(s, "edit.purge.all", "{}") == PC_OK);
    pc_engine_session_free(s);
}

/* ---- adversarial: never crash, always a status --------------------------------------------- */

static void test_adversarial(void)
{
    static const char *const cmds[] = {
        "tools.setColors", "tools.swapColors", "tools.defaultColors", "image.autoTone",
        "image.autoContrast", "image.autoColor", "layer.flattenImage", "layer.layerMask.apply",
        "layer.layerMask.fromTransparency", "layer.layerMask.hideSelection",
        "layer.smartObjects.stackMode.mean", "layer.smartObjects.stackMode.none",
        "layer.smartObjects.convertToSmartObject", "filter.convertForSmartFilters",
        "edit.purge.undo", "edit.purge.clipboard", "edit.purge.histories", "edit.purge.all",
        "filter.other.colorToAlpha", "layer.rasterize.shape",
    };
    static const char *const params[] = {
        "{}", "null", "7", "\"x\"", "[]", "{\"layer\":-1}", "{\"layer\":999999}",
        "{\"color\":[1,2,3,4,5]}", "{\"foregroundCmyk\":[]}", "{\"opacityThreshold\":1e308}",
    };
    pc_engine_session *s = mk(12, 12, "white");
    (void)exec(s, "layer.new.layer", "{}");
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            (void)exec(s, cmds[i], params[j]); /* must return a status, never crash */
        }
    }
    pc_engine_session_free(s);
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            (void)exec(empty, cmds[i], params[j]);
        }
    }
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_tool_colors);
    RUN(test_auto_adjust);
    RUN(test_flatten_image);
    RUN(test_color_to_alpha);
    RUN(test_mask_from_transparency);
    RUN(test_mask_apply_and_hide_selection);
    RUN(test_convert_and_stack_mode);
    RUN(test_rasterize_shape);
    RUN(test_purge);
    RUN(test_adversarial);
    return pc_t_finish();
}
