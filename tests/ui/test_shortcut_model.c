/* Keyboard Shortcuts editor model (B8): the command list and key capture. */
#include "harness.h"
#include "engine/engine.h"
#include "engine/prefs.h"
#include "ui/menus.h"
#include "ui/shortcut_model.h"
#include "ui/shortcuts.h"
#include "ui/state.h"
#include <stdlib.h>
#include <string.h>

static const pc_ui_shortcut_item *find(const pc_ui_shortcut_item *items, size_t n, const char *id)
{
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(items[i].id, id)) {
            return &items[i];
        }
    }
    return NULL;
}

static void test_items(void)
{
    pc_engine_session *s = NULL;
    pc_error e;
    CHECK(pc_engine_session_new(&s, &e) == PC_OK);
    pc_ui_shortcut_item *items = NULL;
    size_t n = 0;
    CHECK(pc_ui_shortcut_items(s, &items, &n, &e) == PC_OK);
    CHECK(n > 200); /* the full Photoshop command surface */

    /* No duplicate ids. */
    size_t dups = 0;
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (!strcmp(items[i].id, items[j].id)) {
                dups++;
            }
        }
    }
    CHECK_EQ_UINT(dups, 0u);

    /* A menu command with a default shortcut. */
    const pc_ui_shortcut_item *newdoc = find(items, n, "file.new");
    CHECK(newdoc != NULL);
    CHECK(newdoc && newdoc->depth >= 1);
    CHECK(newdoc && newdoc->def && newdoc->def[0]);

    /* Every toolbar tool is listed under the Tools group. */
    char move[96];
    CHECK(pc_ui_tool_select_id(PC_TOOL_MOVE, move, sizeof move) < sizeof move);
    const pc_ui_shortcut_item *mv = find(items, n, move);
    CHECK(mv != NULL);
    CHECK(mv && mv->depth >= 1 && !strcmp(mv->path[0], "Tools"));

    /* The held temporary tools are listed. */
    size_t tn = 0;
    const pc_engine_temporary_tool *temp = pc_engine_temporary_tools(&tn);
    CHECK(tn > 0);
    if (tn) {
        const pc_ui_shortcut_item *t0 = find(items, n, temp[0].id);
        CHECK(t0 != NULL);
        CHECK(t0 && t0->depth == 2 && !strcmp(t0->path[0], "Tools") && !strcmp(t0->path[1], "Temporary"));
    }

    pc_ui_shortcut_items_free(items, n);
    pc_engine_session_free(s);
}

static void test_capture(void)
{
    char buf[128];
    CHECK(pc_ui_shortcut_capture(PC_KEY_N, PC_UI_MOD_COMMAND, buf, sizeof buf));
    CHECK_CONTAINS(buf, "N");
    CHECK_CONTAINS(buf, "Cmd");

    CHECK(pc_ui_shortcut_capture(PC_KEY_K, PC_UI_MOD_COMMAND | PC_UI_MOD_SHIFT, buf, sizeof buf));
    CHECK_CONTAINS(buf, "Shift");
    CHECK_CONTAINS(buf, "K");

    /* Punctuation keys record their symbol. */
    CHECK(pc_ui_shortcut_capture(PC_KEY_EQUALS, PC_UI_MOD_COMMAND, buf, sizeof buf));
    CHECK_CONTAINS(buf, "=");

    /* A bare key with no modifiers still captures. */
    CHECK(pc_ui_shortcut_capture(PC_KEY_B, PC_UI_MOD_NONE, buf, sizeof buf));
    CHECK_CONTAINS(buf, "B");

    /* No key -> nothing captured, output cleared. */
    buf[0] = 'x';
    CHECK(!pc_ui_shortcut_capture(PC_KEY_NONE, PC_UI_MOD_COMMAND, buf, sizeof buf));
    CHECK(buf[0] == '\0');

    /* Round-trips through the parser. */
    CHECK(pc_ui_shortcut_capture(PC_KEY_N, PC_UI_MOD_COMMAND, buf, sizeof buf));
    pc_ui_shortcut sc;
    CHECK(pc_ui_shortcut_parse(buf, &sc));
    CHECK(sc.key == PC_KEY_N);
    CHECK((sc.mods & PC_UI_MOD_COMMAND) != 0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_items);
    RUN(test_capture);
    return pc_t_finish();
}
