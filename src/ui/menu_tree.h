#ifndef PC_UI_MENU_TREE_H
#define PC_UI_MENU_TREE_H
#include "ui/menus.h"
#define PC_UI_MENU_ROWS 256
typedef struct pc_ui_tree_row {
    const char *label;
    size_t item;
    bool submenu, enabled;
} pc_ui_tree_row;
/* Direct children of path[0..depth). Preserves catalog order and coalesces submenu headings. */
pc_status pc_ui_menu_tree_rows(const pc_ui_menu_item *items, size_t count, const char *const *path,
                               size_t depth, pc_ui_tree_row *rows, size_t capacity, size_t *out_count,
                               pc_error *err);
#endif
