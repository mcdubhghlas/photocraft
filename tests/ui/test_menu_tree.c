#include "harness.h"
#include "ui/menu_tree.h"
#include <string.h>
static size_t walk(const pc_ui_menu_item *items, size_t count, const char *path[3], size_t depth)
{
    pc_ui_tree_row rows[PC_UI_MENU_ROWS];
    size_t n = 0, total = 0;
    CHECK(pc_ui_menu_tree_rows(items, count, path, depth, rows, PC_UI_MENU_ROWS, &n, NULL) == PC_OK);
    for (size_t i = 0; i < n; i++) {
        CHECK(rows[i].label && rows[i].item < count);
        if (rows[i].submenu) {
            CHECK(depth < 3);
            path[depth] = rows[i].label;
            total += walk(items, count, path, depth + 1);
        } else {
            CHECK(items[rows[i].item].depth == depth);
            total++;
        }
    }
    return total;
}
static void test_complete_catalog_tree(void)
{
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    pc_ui_menu_item *items = NULL;
    size_t count = 0;
    CHECK(pc_ui_menu_items(session, &items, &count, NULL) == PC_OK);
    size_t leaves = 0, expected = 0;
    for (size_t t = 0; t < 10; t++) {
        const char *path[3] = {PC_UI_TOP_MENUS[t], NULL, NULL};
        leaves += walk(items, count, path, 1);
        for (size_t i = 0; i < count; i++) {
            if (items[i].depth && items[i].path[0] && !strcmp(items[i].path[0], path[0])) {
                expected++;
            }
        }
    }
    CHECK(leaves == expected && leaves > 700);
    const char *path[3] = {"File", NULL, NULL};
    pc_ui_tree_row one[1];
    size_t n = 42;
    CHECK(pc_ui_menu_tree_rows(items, count, path, 1, one, 1, &n, NULL) == PC_ERR_LIMIT && n == 0);
    CHECK(pc_ui_menu_tree_rows(items, count, path, 4, one, 1, &n, NULL) == PC_ERR_INVALID);
    pc_ui_menu_items_free(items, count);
    pc_engine_session_free(session);
}
int main(int argc, char **argv)
{
    RUN(test_complete_catalog_tree);
    return pc_t_finish();
}
