/* Brush engine commands and painting symmetry (port of a subset of brush_cmds / symmetry_cmds
 * tests, E10). Covers the session brush, strokes, brush presets, symmetry and graceful failure. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/brush_cmds.h"
#include "engine/engine_internal.h"
#include "engine/symmetry_cmds.h"
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

static pc_status rn(pc_engine_session *s, const char *id, const char *json, pc_json_value *out,
                    pc_error *err)
{
    return pc_engine_session_execute_json(s, id, json, out, err);
}

static void ok(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = rn(s, id, json, &out, &err);
    CHECK_EQ_STR(rc == PC_OK ? "ok" : err.msg, "ok");
    pc_json_value_fini(&out);
}

static void err_cmd(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    pc_status rc = rn(s, id, json, &out, &err);
    CHECK(rc != PC_OK);
    pc_json_value_fini(&out);
}

static void px(pc_engine_session *s, int x, int y, float out[4])
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    const pc_surface *surf = pc_layer_surface(l);
    pc_surface_rgba(surf, x, y, out);
}

static double brush_field(pc_engine_session *s, const char *key)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(rn(s, "brush.get", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    yyjson_mut_val *v = yyjson_mut_obj_get(root, key);
    double r = v != NULL ? yyjson_mut_get_num(v) : -999.0;
    pc_json_value_fini(&out);
    return r;
}

/* ---- the session brush ---- */

static void test_set_brush_deep_merge(void)
{
    pc_engine_session *s = mk(16, 16);
    ok(s, "tools.setBrush", "{\"size\": 42, \"scattering\": {\"enabled\": true, \"scatter\": {\"jitter\": 0.5}}}");
    CHECK_NEAR(brush_field(s, "size"), 42.0, 1e-4);
    /* A partial patch keeps the rest of the brush. */
    ok(s, "tools.setBrush", "{\"hardness\": 0.25}");
    CHECK_NEAR(brush_field(s, "size"), 42.0, 1e-4);
    CHECK_NEAR(brush_field(s, "hardness"), 0.25, 1e-4);
    pc_engine_session_free(s);
}

static void test_set_brush_rejects_bad_size(void)
{
    pc_engine_session *s = mk(16, 16);
    err_cmd(s, "tools.setBrush", "{\"size\": 999999}");
    /* The brush is unchanged after a rejected call. */
    double before = brush_field(s, "size");
    err_cmd(s, "tools.setBrush", "{\"scattering\": {\"enabled\": true, \"scatter\": {\"jitter\": 50}}}");
    CHECK_NEAR(brush_field(s, "size"), before, 1e-4);
    pc_engine_session_free(s);
}

/* ---- strokes ---- */

static void test_stroke_paints(void)
{
    pc_engine_session *s = mk(32, 32);
    ok(s, "paint.stroke",
       "{\"points\": [[8,8],[24,24]], \"size\": 6, \"hardness\": 1.0, \"color\": \"#ff0000\", \"smoothing\": 0.0}");
    float c[4];
    px(s, 8, 8, c);
    CHECK(c[0] > 0.8f && c[1] < 0.2f && c[2] < 0.2f);
    /* A corner the stroke never reached stays white. */
    px(s, 31, 0, c);
    CHECK(c[0] > 0.9f && c[1] > 0.9f && c[2] > 0.9f);
    /* Undo restores it. */
    CHECK(pc_engine_session_undo(s));
    px(s, 8, 8, c);
    CHECK(c[0] > 0.9f && c[1] > 0.9f && c[2] > 0.9f);
    pc_engine_session_free(s);
}

static void test_pencil_block(void)
{
    pc_engine_session *s = mk(40, 40);
    ok(s, "paint.pencil", "{\"points\": [[20,20]], \"block\": true, \"zoom\": 1, \"color\": \"#000000\"}");
    float c[4];
    px(s, 20, 20, c);
    CHECK(c[0] < 0.2f);
    pc_engine_session_free(s);
}

static void test_stroke_bad_params(void)
{
    pc_engine_session *s = mk(16, 16);
    err_cmd(s, "paint.stroke", "{}");                         /* missing points */
    err_cmd(s, "paint.stroke", "{\"points\": []}");           /* empty */
    err_cmd(s, "paint.stroke", "{\"points\": [[1e9, 1e9]]}"); /* out of range */
    /* Nothing painted. */
    float c[4];
    px(s, 1, 1, c);
    CHECK(c[0] > 0.9f);
    pc_engine_session_free(s);
}

/* ---- presets ---- */

static size_t preset_count(pc_engine_session *s)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(rn(s, "brush.presets.list", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    yyjson_mut_val *arr = yyjson_mut_obj_get(root, "presets");
    size_t n = yyjson_mut_arr_size(arr);
    pc_json_value_fini(&out);
    return n;
}

static void test_presets(void)
{
    pc_engine_session *s = mk(16, 16);
    size_t builtins = preset_count(s);
    CHECK(builtins > 0);
    ok(s, "tools.setBrush", "{\"size\": 33}");
    ok(s, "brush.presets.save", "{\"name\": \"My Brush\"}");
    CHECK_EQ_UINT(preset_count(s), builtins + 1);
    /* The saved brush is the current preset. */
    ok(s, "brush.presets.setCurrent", "{\"name\": \"My Brush\"}");
    ok(s, "brush.presets.update", "{\"brush\": {\"hardness\": 0.5}}");
    err_cmd(s, "brush.presets.delete", "{\"name\": \"nope\"}");
    ok(s, "brush.presets.delete", "{\"name\": \"My Brush\"}");
    CHECK_EQ_UINT(preset_count(s), builtins);
    pc_engine_session_free(s);
}

static void test_define_from_selection_needs_pixels(void)
{
    pc_engine_session *s = mk(16, 16);
    /* No selection -> disabled and refused. */
    CHECK(!pc_engine_session_is_enabled(s, "brush.defineFromSelection"));
    err_cmd(s, "brush.defineFromSelection", "{\"name\": \"x\"}");
    pc_engine_session_free(s);
}

/* ---- symmetry ---- */

static void test_symmetry_preset(void)
{
    pc_engine_session *s = mk(40, 30);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(rn(s, "paint.setSymmetry", "{\"mode\": \"vertical\"}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_active(s)->symmetry != NULL);
    /* Disable works and then refuses. */
    ok(s, "paint.symmetryDisable", "{}");
    CHECK(pc_engine_session_active(s)->symmetry == NULL);
    err_cmd(s, "paint.symmetryDisable", "{}");
    /* A bad mode fails without setting symmetry. */
    err_cmd(s, "paint.setSymmetry", "{\"mode\": \"bogus\"}");
    CHECK(pc_engine_session_active(s)->symmetry == NULL);
    pc_engine_session_free(s);
}

static void test_symmetry_from_path_errors(void)
{
    pc_engine_session *s = mk(32, 32);
    err_cmd(s, "paint.symmetryFromPath", "{\"name\": \"missing\"}");
    err_cmd(s, "paint.symmetryFromPath", "{\"name\": \"work\"}"); /* no work path */
    pc_engine_session_free(s);
}

static void test_symmetry_mirrors_stroke(void)
{
    pc_engine_session *s = mk(80, 60);
    /* A vertical axis down the middle of the canvas. */
    ok(s, "paint.setSymmetry", "{\"mode\": \"vertical\", \"center\": [40, 30]}");
    ok(s, "paint.stroke",
       "{\"points\": [[20,20],[30,30]], \"size\": 5, \"hardness\": 1.0, \"color\": \"#ff0000\", \"smoothing\": 0.0}");
    float left[4], right[4];
    px(s, 20, 20, left);
    px(s, 60, 20, right); /* the mirror of x=20 across x=40 */
    CHECK(left[1] < 0.3f);  /* red painted (green low) */
    CHECK(right[1] < 0.3f); /* mirror painted too */
    pc_engine_session_free(s);
}

/* ---- journal coalescing ---- */

static void test_coalesce_journal(void)
{
    pc_engine_session *s = mk(16, 16);
    ok(s, "tools.setBrush", "{\"size\": 10, \"coalesce\": \"drag1\"}");
    size_t after_first = pc_engine_session_journal_len(s);
    ok(s, "tools.setBrush", "{\"size\": 20, \"coalesce\": \"drag1\"}");
    ok(s, "tools.setBrush", "{\"size\": 30, \"coalesce\": \"drag1\"}");
    /* The three calls of one gesture collapse to a single journal entry. */
    CHECK_EQ_UINT(pc_engine_session_journal_len(s), after_first);
    CHECK_NEAR(brush_field(s, "size"), 30.0, 1e-4);
    /* A different key starts a new entry. */
    ok(s, "tools.setBrush", "{\"size\": 40, \"coalesce\": \"drag2\"}");
    CHECK_EQ_UINT(pc_engine_session_journal_len(s), after_first + 1);
    pc_engine_session_free(s);
}

/* ---- brush keys ---- */

static void test_brush_keys(void)
{
    pc_engine_session *s = mk(16, 16);
    ok(s, "tools.setBrush", "{\"brush\": {\"size\": 40, \"hardness\": 0.5}}");
    ok(s, "tools.increaseBrushSize", "{}");
    CHECK_NEAR(brush_field(s, "size"), 50.0, 1e-4);
    ok(s, "tools.decreaseBrushSize", "{}");
    CHECK_NEAR(brush_field(s, "size"), 40.0, 1e-4);
    /* `[` reaches 1 px on small brushes (#3229). */
    ok(s, "tools.setBrush", "{\"brush\": {\"size\": 3}}");
    double expect[3] = {2.0, 1.0, 1.0};
    for (int i = 0; i < 3; i++) {
        ok(s, "tools.decreaseBrushSize", "{}");
        CHECK_NEAR(brush_field(s, "size"), expect[i], 1e-4);
    }
    ok(s, "tools.increaseBrushHardness", "{}");
    CHECK_NEAR(brush_field(s, "hardness"), 0.75, 1e-4);
    /* rotate wraps and rejects bad params. */
    ok(s, "tools.rotateBrush", "{\"degrees\": -15}");
    CHECK_NEAR(brush_field(s, "angle"), -15.0, 1e-4);
    err_cmd(s, "tools.rotateBrush", "{}");
    err_cmd(s, "tools.rotateBrush", "{\"degrees\": \"x\"}");
    err_cmd(s, "tools.rotateBrush", "{\"degrees\": 1e9}");
    CHECK_NEAR(brush_field(s, "angle"), -15.0, 1e-4);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_set_brush_deep_merge);
    RUN(test_set_brush_rejects_bad_size);
    RUN(test_stroke_paints);
    RUN(test_pencil_block);
    RUN(test_stroke_bad_params);
    RUN(test_presets);
    RUN(test_define_from_selection_needs_pixels);
    RUN(test_symmetry_preset);
    RUN(test_symmetry_from_path_errors);
    RUN(test_symmetry_mirrors_stroke);
    RUN(test_coalesce_journal);
    RUN(test_brush_keys);
    return pc_t_finish();
}
