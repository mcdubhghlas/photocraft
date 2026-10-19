#ifndef PC_UI_SHELL_NATIVE_MENUS_H
#define PC_UI_SHELL_NATIVE_MENUS_H
#include "app.h"
#include "ui/menu_tree.h"
#include "shell_internal.h"
typedef struct pc_ui_native_menus {
    pc_ui_menu_nav nav;
    char prefix[2][256];
    float top_rect[10][4], rect[3][4];
    float anchor_y[3];
    size_t levels;
    bool pointer_valid;
    float last_x, last_y;
    uint64_t hover_since;
} pc_ui_native_menus;
void pc_ui_native_menu_bar(pc_ui_shell_app *app, pc_ui_native_menus *runtime);
void pc_ui_native_menu_popups(pc_ui_shell_app *app, pc_ui_native_menus *runtime, const pc_ui_menu_item *items,
                              size_t count, float width, float height);
bool pc_ui_native_menu_key(pc_ui_shell_app *app, pc_ui_native_menus *runtime, const pc_ui_menu_item *items,
                           size_t count, const SDL_Event *event);
bool pc_ui_native_menu_capture(pc_ui_shell_app *app, pc_ui_native_menus *runtime, const SDL_Event *event);
#endif
