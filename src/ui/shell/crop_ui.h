#ifndef PC_UI_SHELL_CROP_UI_H
#define PC_UI_SHELL_CROP_UI_H
#include "shell_internal.h"
#include "ui/crop.h"
#include "ui/canvas.h"
#include "ui/tool_gesture.h"
typedef struct pc_ui_crop_step {
    pc_ui_crop_frame frame;
    pc_ui_view view;
    bool editing, default_frame;
} pc_ui_crop_step;
typedef struct pc_ui_crop_runtime {
    bool scoped, editing, default_frame, space, straighten, straightening, has_press;
    uint64_t document, revision;
    pc_ui_crop_drag drag;
    pc_ui_crop_step press, undo[128], redo[128];
    size_t nundo, nredo;
    pc_ui_canvas_xform press_xf;
    double line[2][2], last_ratio;
    char width[65], height[65], resolution[65];
    bool text_active[3];
} pc_ui_crop_runtime;
pc_ui_crop_runtime *pc_ui_shell_crop_runtime(pc_ui_shell_app *app);
void pc_ui_shell_crop_tick(pc_ui_shell_app *app);
pc_status pc_ui_shell_crop_command(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                   pc_json_value *out, pc_error *err);
pc_status pc_ui_shell_crop_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                   unsigned modifiers, pc_json_value *out, pc_error *err);
bool pc_ui_shell_crop_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_crop_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                             float factor);
void pc_ui_shell_crop_options(pc_ui_shell_app *app);
void pc_ui_shell_crop_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf);
#endif
