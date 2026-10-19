#ifndef PC_UI_CONTEXT_MENU_H
#define PC_UI_CONTEXT_MENU_H
#include "base/json.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum pc_ui_context_kind {
    PC_UI_CONTEXT_SELECTION,
    PC_UI_CONTEXT_EMPTY_SELECTION,
    PC_UI_CONTEXT_PEN,
    PC_UI_CONTEXT_VIEW,
    PC_UI_CONTEXT_TRANSFORM,
    PC_UI_CONTEXT_PASTEBOARD,
    PC_UI_CONTEXT_TEXT,
    PC_UI_CONTEXT_LAYER,
    PC_UI_CONTEXT_MASK
} pc_ui_context_kind;
typedef struct pc_ui_context_menu {
    bool open;
    float point[2];
    double anchor[2];
    uint64_t document, revision, layer;
    pc_ui_context_kind kind;
    unsigned tool;
    size_t selected;
    char path[1025];
} pc_ui_context_menu;
typedef struct pc_ui_context_row {
    const char *label, *command;
} pc_ui_context_row;
const pc_ui_context_row *pc_ui_context_rows(pc_ui_context_kind kind, size_t *count);
pc_status pc_ui_context_to_json(const pc_ui_context_menu *menu, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                pc_error *err);
#endif
