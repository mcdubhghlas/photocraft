#ifndef PC_UI_SHELL_BRUSH_PICKER_UI_H
#define PC_UI_SHELL_BRUSH_PICKER_UI_H
#include "shell_internal.h"
typedef struct pc_ui_brush_picker_runtime {
    bool shown, prefs_loaded;
    pc_ui_brush_picker saved;
    float point[2];
    char search[128];
} pc_ui_brush_picker_runtime;
bool pc_ui_brush_preset_matches(const char *name, const char *search);
void pc_ui_shell_brush_preset_card(pc_ui_shell_app *app, size_t index, const pc_engine_tool_state *tools,
                                   const pc_ui_brush_picker *picker);
void pc_ui_shell_brush_picker_restore(pc_ui_shell_app *app, pc_ui_brush_picker_runtime *runtime);
void pc_ui_shell_brush_picker_draw(pc_ui_shell_app *app, pc_ui_brush_picker_runtime *runtime, float width,
                                   float height);
bool pc_ui_shell_brush_picker_event(pc_ui_shell_app *app, const SDL_Event *event);
#endif
