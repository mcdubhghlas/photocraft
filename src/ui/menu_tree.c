#include "menu_tree.h"
#include <string.h>
pc_status pc_ui_menu_tree_rows(const pc_ui_menu_item *items, size_t count, const char *const *path,
                               size_t depth, pc_ui_tree_row *rows, size_t capacity, size_t *out_count,
                               pc_error *err)
{
    if (!out_count || !path || depth < 1 || depth > 3 || (capacity && !rows) || (count && !items)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid menu path");
    }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        const pc_ui_menu_item *item = &items[i];
        if (item->depth < depth || item->depth > 3) {
            continue;
        }
        bool match = true;
        for (size_t p = 0; p < depth; p++) {
            if (!path[p] || !item->path[p] || strcmp(path[p], item->path[p])) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }
        bool submenu = item->depth > depth;
        const char *label = submenu ? item->path[depth] : item->label;
        if (!label) {
            continue;
        }
        if (submenu) {
            size_t r = 0;
            for (; r < used; r++) {
                if (rows[r].submenu && !strcmp(rows[r].label, label)) {
                    rows[r].enabled |= item->enabled;
                    break;
                }
            }
            if (r < used) {
                continue;
            }
        }
        if (used >= capacity) {
            *out_count = 0;
            return pc_fail(err, PC_ERR_LIMIT, "menu exceeds %zu rows", capacity);
        }
        rows[used++] = (pc_ui_tree_row){label, i, submenu, item->enabled};
    }
    *out_count = used;
    return PC_OK;
}
