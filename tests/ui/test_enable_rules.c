#include "harness.h"
#include "ui/enable_rules.h"
#include <stdlib.h>
static pc_engine_session *fresh(void)
{
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, "file.new", "{\"width\":20,\"height\":20}", &result,
                                         NULL) == PC_OK);
    pc_json_value_fini(&result);
    return session;
}
static void run(pc_engine_session *session, const char *id, const char *params)
{
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, id, params, &result, NULL) == PC_OK);
    pc_json_value_fini(&result);
}
static void test_background_channel_and_selection_enablement(void)
{
    pc_engine_session *session = fresh();
    pc_ui_state state;
    pc_ui_state_init(&state);
    const char *off[] = {"layer.layerStyle.dropShadow", "layer.layerMask.revealAll",
                         "layer.arrange.bringForward",  "layer.mergeDown",
                         "edit.freeTransform",          "edit.transform.scale",
                         "layer.flattenImage",          "layer.mergeVisible",
                         "layer.createClippingMask",    "layer.rasterize.layer",
                         "layer.combineShapes.unite",   "layer.delete.hiddenLayers"};
    for (size_t i = 0; i < sizeof off / sizeof off[0]; i++) {
        CHECK(pc_ui_command_disabled(session, &state, off[i]));
    }
    CHECK(!pc_ui_command_disabled(session, &state, "layer.layerStyle.globalLight"));
    run(session, "select.all", "{}");
    CHECK(!pc_ui_command_disabled(session, &state, "edit.freeTransform"));
    run(session, "select.deselect", "{}");
    run(session, "channel.new", "{}");
    run(session, "channel.target", "{\"channel\":0}");
    CHECK(!pc_ui_command_disabled(session, &state, "edit.freeTransform"));
    CHECK(!pc_ui_command_disabled(session, &state, "edit.transform.flipHorizontal"));
    pc_ui_state_fini(&state);
    pc_engine_session_free(session);
}
static void test_rasterize_uses_all_selected_layers(void)
{
    pc_engine_session *session = fresh();
    run(session, "layer.new.layer", "{}");
    pc_layer_id raster = pc_engine_session_active(session)->active_layer;
    run(session, "type.create", "{\"text\":\"Hi\",\"size\":12,\"x\":2,\"y\":12}");
    pc_layer_id text = pc_engine_session_active(session)->active_layer;
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(session)->doc);
    const pc_layer *active = pc_document_layer(doc, raster);
    pc_layer_id selected[] = {text, raster};
    CHECK(!pc_ui_disabled_for_selection(doc, selected, 2, active, "layer.rasterize.layer"));
    CHECK(!pc_ui_disabled_for_selection(doc, selected, 2, active, "layer.rasterize.type"));
    CHECK(pc_ui_disabled_for_selection(doc, selected, 2, active, "layer.rasterize.shape"));
    CHECK(pc_ui_disabled_for_selection(doc, selected, 2, active, "layer.rasterize.smartObject"));
    CHECK(pc_ui_disabled_for_selection(doc, &raster, 1, active, "layer.rasterize.layer"));
    CHECK(
        !pc_ui_disabled_for_selection(doc, &text, 1, pc_document_layer(doc, text), "layer.rasterize.layer"));
    CHECK(pc_ui_command_disabled(session, NULL, "edit.transform.distort"));
    pc_engine_session_free(session);
}
static void test_empty_session_and_tool_specific_enablement(void)
{
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    pc_ui_state state;
    pc_ui_state_init(&state);
    CHECK(pc_ui_command_disabled(session, &state, "edit.freeTransform"));
    CHECK(pc_ui_command_disabled(session, &state, "type.editText"));
    CHECK(pc_ui_command_disabled(session, &state, "view.zoomIn"));
    CHECK(!pc_ui_command_disabled(session, &state, "window.toggle.layers"));
    CHECK(!pc_ui_command_disabled(session, &state, "tools.increaseBrushHardness"));
    state.tool = PC_TOOL_HAND;
    CHECK(pc_ui_command_disabled(session, &state, "tools.increaseBrushHardness"));
    pc_ui_state_fini(&state);
    pc_engine_session_free(session);
}
int main(int argc, char **argv)
{
    RUN(test_empty_session_and_tool_specific_enablement);
    RUN(test_background_channel_and_selection_enablement);
    RUN(test_rasterize_uses_all_selected_layers);
    return pc_t_finish();
}
