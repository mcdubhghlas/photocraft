#ifndef PC_UI_SHELL_CURSORS_H
#define PC_UI_SHELL_CURSORS_H
#include "shell_internal.h"
#include "ui/tool_cursor.h"
typedef struct pc_ui_native_cursor {
    SDL_Cursor *bitmap, *system[SDL_SYSTEM_CURSOR_COUNT];
    uint8_t key[32];
    bool valid, used, brush_valid, full, hidden;
    pc_paint_brush brush;
    pc_paint_outline outline;
} pc_ui_native_cursor;
struct pc_ui_canvas_view;
void pc_ui_native_cursor_fini(pc_ui_native_cursor *cursor);
void pc_ui_native_cursor_draw(pc_ui_shell_app *app, struct pc_ui_canvas_view *view);
void pc_ui_native_cursor_end(pc_ui_native_cursor *cursor);
#endif
