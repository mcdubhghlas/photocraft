#include "harness.h"
#include "ui/workspace.h"
#include <string.h>
static void test_layout_snapshot_restore_and_host_chrome(void)
{
    pc_ui_state s;
    pc_ui_state_init(&s);
    s.tool = PC_TOOL_BRUSH;
    s.panels.rail = false;
    s.panels.menu_bar = false;
    s.panels.history = true;
    s.dock_width = 417;
    s.timeline.open = true;
    s.dock_tabs.layers = 2;
    pc_ui_dock_set_collapsed(&s.dock, PC_UI_GROUP_COLOR, true);
    pc_ui_dock_move_group(&s.dock, PC_UI_GROUP_LAYERS, &(pc_ui_group){PC_UI_GROUP_COLOR});
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *saved = NULL;
    CHECK(pc_ui_workspace_snapshot(&s, d, &saved, NULL) == PC_OK);
    CHECK(!yyjson_mut_obj_get(yyjson_mut_obj_get(saved, "panels"), "rail"));
    CHECK(!yyjson_mut_obj_get(yyjson_mut_obj_get(saved, "panels"), "menu_bar"));
    yyjson_doc *read = yyjson_mut_val_imut_copy(saved, NULL);
    CHECK(read != NULL);
    CHECK(pc_ui_workspace_apply(&s, "Painting", NULL, NULL) == PC_OK);
    CHECK(!s.panels.properties && !s.panels.history && s.panels.color);
    CHECK(!s.panels.rail && !s.panels.menu_bar && s.tool == PC_TOOL_BRUSH);
    CHECK(pc_ui_workspace_apply(&s, "Mine", yyjson_doc_get_root(read), NULL) == PC_OK);
    CHECK(s.panels.history && s.dock_width == 417 && s.dock_tabs.layers == 2);
    CHECK(s.timeline.open && !s.timeline.playing);
    CHECK(pc_ui_dock_is_collapsed(&s.dock, PC_UI_GROUP_COLOR));
    pc_ui_group order[PC_UI_GROUP_N];
    pc_ui_dock_order(&s.dock, order);
    CHECK(order[0] == PC_UI_GROUP_LAYERS);
    CHECK(!s.panels.rail && !s.panels.menu_bar && s.tool == PC_TOOL_BRUSH);
    CHECK_EQ_STR(s.workspace, "Mine");
    yyjson_doc_free(read);
    yyjson_mut_doc_free(d);
    pc_ui_state_fini(&s);
}
static void test_presets_and_invalid_layout_are_atomic(void)
{
    pc_ui_state s;
    pc_ui_state_init(&s);
    s.panels.toolbar = false;
    s.panels.options_bar = false;
    s.panels.status_bar = false;
    CHECK(pc_ui_workspace_apply(&s, "Motion", NULL, NULL) == PC_OK);
    CHECK(s.timeline.open);
    CHECK(pc_ui_workspace_apply(&s, "Photography", NULL, NULL) == PC_OK);
    CHECK(!s.timeline.open);
    CHECK(s.panels.navigator && s.panels.history && !s.panels.color);
    CHECK(!s.panels.toolbar && !s.panels.options_bar && !s.panels.status_bar);
    CHECK(pc_ui_workspace_apply(&s, "Graphic and Web", NULL, NULL) == PC_OK);
    CHECK(s.panels.character && s.panels.properties && !s.panels.color);
    yyjson_doc *read = NULL;
    const char text[] = "{\"panels\":null}";
    CHECK(pc_json_parse(text, sizeof text - 1, &read, NULL) == PC_OK);
    CHECK(pc_ui_workspace_apply(&s, "Bad", yyjson_doc_get_root(read), NULL) != PC_OK);
    CHECK_EQ_STR(s.workspace, "Graphic and Web");
    CHECK(s.panels.character && !s.panels.color);
    CHECK(pc_ui_workspace_apply(&s, "Missing", NULL, NULL) != PC_OK);
    CHECK(pc_ui_workspace_builtin("Essentials") && !pc_ui_workspace_builtin("Mine"));
    yyjson_doc_free(read);
    pc_ui_state_fini(&s);
}
int main(int argc, char **argv)
{
    RUN(test_layout_snapshot_restore_and_host_chrome);
    RUN(test_presets_and_invalid_layout_are_atomic);
    return pc_t_finish();
}
