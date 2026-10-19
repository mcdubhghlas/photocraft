#ifndef PC_UI_SHELL_BRUSH_PANEL_H
#define PC_UI_SHELL_BRUSH_PANEL_H
#include "app.h"
typedef struct pc_ui_brush_panel {
    char name[256], search[128];
    uint64_t gesture;
    pc_paint_brush preview_brush;
    bool has_preview, shown;
    uint8_t pixels[256 * 64 * 4];
    uint8_t key[32];
} pc_ui_brush_panel;
void pc_ui_brush_panel_fini(pc_ui_brush_panel *panel);
void pc_ui_brush_panel_draw(pc_ui_shell_app *app, pc_ui_brush_panel *panel);
#endif
