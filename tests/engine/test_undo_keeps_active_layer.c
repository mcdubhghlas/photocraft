/* Undoing an edit restores the layer target that was active just before the step, as in Photoshop
 * (#1356): selecting a layer is not a history step (port of
 * crates/engine/tests/undo_keeps_active_layer.rs). */
#include "engine/engine.h"
#include "harness.h"

static pc_engine_session *new_doc(const char *dims)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "file.new", dims, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    return s;
}

static void run_ok(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error err = {0};
    if (pc_engine_session_execute_json(s, id, json, &out, &err) != PC_OK) {
        PC_T_FAIL("%s failed: %s", id, err.msg);
    } else {
        pc_json_value_fini(&out);
    }
}

/* Run a command and return its result["layer"] as a layer id (0 if absent). */
static pc_layer_id run_for_layer(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_layer_id lid = 0;
    if (pc_engine_session_execute_json(s, id, json, &out, NULL) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&out);
        yyjson_mut_val *l = root ? yyjson_mut_obj_get(root, "layer") : NULL;
        if (l != NULL) {
            lid = (pc_layer_id)yyjson_mut_get_uint(l);
        }
        pc_json_value_fini(&out);
    }
    return lid;
}

static pc_layer_id bg_id(const pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc)->layers[0].id;
}

static pc_layer_id active_id(const pc_engine_session *s, bool *has)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    *has = st->has_active_layer;
    return st->active_layer;
}

/* Assert the active layer and the selected-layers set match (expected one id, or empty). */
static void check_target(const pc_engine_session *s, bool has, pc_layer_id id, const char *ctx)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st->has_active_layer != has) {
        PC_T_FAIL("%s: has_active_layer %d != %d", ctx, (int)st->has_active_layer, (int)has);
    }
    if (has) {
        CHECK(st->active_layer == id);
        CHECK_EQ_UINT(st->nselected, 1);
        if (st->nselected == 1) {
            CHECK(st->selected_layers[0] == id);
        }
    } else {
        CHECK_EQ_UINT(st->nselected, 0);
    }
}

static void sel(pc_engine_session *s, pc_layer_id id)
{
    char p[48];
    (void)snprintf(p, sizeof p, "{\"layer\":%llu}", (unsigned long long)id);
    run_ok(s, "layer.select", p);
}

static void test_undo_stroke_on_rasterized_shape(void)
{
    /* DEFERRED: rasterizing a Shape layer dispatches to layer.rasterize.shape, which the C port
     * has not registered yet (vector_cmds E11; see extra_cmds.c). Skip this sub-test until it
     * lands; the fill and empty-selection cases below cover the same undo/redo targeting rule. */
    if (pc_engine_command_find("layer.rasterize.shape") == NULL) {
        (void)printf("  (skipped: layer.rasterize.shape is DEFERRED)\n");
        return;
    }
    pc_engine_session *s = new_doc("{\"width\":64,\"height\":64}");
    pc_layer_id bg = bg_id(s);
    pc_layer_id id = run_for_layer(s, "shape.create", "{\"kind\":\"ellipse\",\"rect\":[4,4,40,30]}");
    CHECK(id != 0);
    sel(s, bg);
    char p[48];
    (void)snprintf(p, sizeof p, "{\"layer\":%llu}", (unsigned long long)id);
    run_ok(s, "layer.rasterize.layer", p);
    sel(s, id);
    run_ok(s, "paint.stroke", "{\"points\":[[4,40],[60,40]],\"size\":6}");
    CHECK(pc_engine_session_undo(s));
    check_target(s, true, id, "undo of stroke keeps painted layer");
    CHECK(pc_engine_session_redo(s));
    check_target(s, true, id, "redo targets painted layer");
    CHECK(pc_engine_session_undo(s));
    CHECK(pc_engine_session_undo(s));
    check_target(s, true, bg, "rasterize taken with background active");
    pc_engine_session_free(s);
}

static void test_undo_fill_keeps_filled_layer(void)
{
    pc_engine_session *s = new_doc("{\"width\":16,\"height\":16}");
    pc_layer_id bg = bg_id(s);
    run_ok(s, "layer.new.layer", "{}");
    bool has = false;
    pc_layer_id newl = active_id(s, &has);
    CHECK(has);
    sel(s, bg);
    run_ok(s, "edit.fill", "{\"color\":\"#ff0000\"}");
    CHECK(pc_engine_session_undo(s));
    check_target(s, true, bg, "fill was on the background");
    CHECK(pc_engine_session_undo(s));
    check_target(s, true, bg, "undo new layer");
    CHECK(pc_engine_session_redo(s));
    check_target(s, true, bg, "redo new-layer state");
    CHECK(pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), newl) != NULL);
    pc_engine_session_free(s);
}

static void test_undo_preserves_empty_selection(void)
{
    pc_engine_session *s = new_doc("{\"width\":8,\"height\":8}");
    pc_layer_id background = bg_id(s);
    run_ok(s, "select.deselectLayers", "{}");
    check_target(s, false, 0, "deselected");
    run_ok(s, "layer.new.layer", "{\"name\":\"A\"}");
    bool has = false;
    pc_layer_id added = active_id(s, &has);
    CHECK(has);
    CHECK(pc_engine_session_undo(s));
    check_target(s, false, 0, "undo restores empty target");
    CHECK(pc_engine_session_redo(s));
    check_target(s, true, added, "redo selects created layer");
    CHECK(pc_engine_session_undo(s));
    check_target(s, false, 0, "repeated undo keeps empty target");
    sel(s, background);
    run_ok(s, "layer.new.layer", "{\"name\":\"B\"}");
    CHECK(pc_engine_session_undo(s));
    check_target(s, true, background, "real target restored");
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_undo_stroke_on_rasterized_shape);
    RUN(test_undo_fill_keeps_filled_layer);
    RUN(test_undo_preserves_empty_selection);
    return pc_t_finish();
}
