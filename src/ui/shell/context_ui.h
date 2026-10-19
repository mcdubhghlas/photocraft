#ifndef PC_UI_SHELL_CONTEXT_UI_H
#define PC_UI_SHELL_CONTEXT_UI_H
#include "shell_internal.h"
#include "ui/canvas.h"
#include "ui/tool_gesture.h"
typedef struct pc_ui_secondary_runtime {
    bool erase;
    pc_ui_tool tool;
} pc_ui_secondary_runtime;
pc_ui_secondary_runtime *pc_ui_shell_secondary_runtime(pc_ui_shell_app *app);
pc_status pc_ui_shell_secondary_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                        unsigned modifiers, pc_json_value *out, pc_error *err);
pc_status pc_ui_shell_context_open(pc_ui_shell_app *app, pc_ui_context_kind kind, float x, float y,
                                   const double anchor[2], pc_layer_id layer, pc_error *err);
pc_status pc_ui_shell_context_choose(pc_ui_shell_app *app, const char *command, pc_json_value *out,
                                     pc_error *err);
pc_status pc_ui_shell_context_json(pc_ui_shell_app *app, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                   pc_error *err);
bool pc_ui_shell_context_event(pc_ui_shell_app *app, const SDL_Event *event);
bool pc_ui_shell_context_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, const SDL_Event *event,
                                float factor);
void pc_ui_shell_context_draw(pc_ui_shell_app *app, float width, float height);
pc_ui_color pc_ui_shell_canvas_color(pc_ui_shell_app *app);
#endif
