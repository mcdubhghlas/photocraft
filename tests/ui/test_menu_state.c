#include "harness.h"
#include "ui/menu_state.h"
#include "ui/recent.h"
#include <stdlib.h>
#include <string.h>
static const pc_ui_menu_item *find(const pc_ui_menu_item *items, size_t count, const char *id)
{
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(items[i].id, id)) {
            return &items[i];
        }
    }
    return NULL;
}
static void test_recent_owned_bounded_and_menu_state(void)
{
    pc_ui_state state;
    pc_ui_state_init(&state);
    char path[] = "/tmp/first.png";
    CHECK(pc_ui_recent_push(&state, path, 3, NULL) == PC_OK);
    path[5] = 'X';
    CHECK(!strcmp(state.recent_files[0], "/tmp/first.png"));
    CHECK(pc_ui_recent_push(&state, "/tmp/second.png", 3, NULL) == PC_OK);
    CHECK(pc_ui_recent_push(&state, "/tmp/first.png", 3, NULL) == PC_OK);
    CHECK(state.n_recent_files == 2 && !strcmp(state.recent_files[0], "/tmp/first.png"));
    CHECK(pc_ui_recent_push(&state, "/tmp/third.png", 1, NULL) == PC_OK && state.n_recent_files == 1);
    CHECK(pc_ui_recent_push(&state, "", 1, NULL) == PC_ERR_INVALID && state.n_recent_files == 1);
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, "file.new", "{\"width\":10,\"height\":10}", &result,
                                         NULL) == PC_OK);
    pc_json_value_fini(&result);
    state.extras.grid = true;
    state.panels.toolbar = false;
    pc_ui_menu_item *items = NULL;
    size_t count = 0;
    CHECK(pc_ui_menu_state_items(session, &state, &items, &count, NULL) == PC_OK);
    const pc_ui_menu_item *item = find(items, count, "window.document.0");
    CHECK(item && item->enabled && item->checked == 1);
    item = find(items, count, "file.openRecent.0");
    CHECK(item && !strcmp(item->label, "third.png") && item->depth == 2);
    item = find(items, count, "view.show.grid");
    CHECK(item && item->checked == 1);
    item = find(items, count, "window.panel.tools");
    CHECK(item && item->checked == 0);
    item = find(items, count, "image.mode.rgb");
    CHECK(item && item->checked == 1);
    item = find(items, count, "layer.flattenImage");
    CHECK(item && !item->enabled);
    pc_ui_menu_items_free(items, count);
    state.n_dialogs = 1;
    CHECK(pc_ui_menu_state_items(session, &state, &items, &count, NULL) == PC_OK);
    item = find(items, count, "window.document.0");
    CHECK(item && !item->enabled);
    item = find(items, count, "file.openRecent.0");
    CHECK(item && !item->enabled);
    pc_ui_menu_items_free(items, count);
    state.n_dialogs = 0;
    CHECK(pc_ui_recent_push(&state, "/tmp/unused.png", 0, NULL) == PC_OK && !state.n_recent_files);
    pc_engine_session_free(session);
    pc_ui_state_fini(&state);
}
int main(int argc, char **argv)
{
    RUN(test_recent_owned_bounded_and_menu_state);
    return pc_t_finish();
}
