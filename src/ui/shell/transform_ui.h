#ifndef PC_UI_SHELL_TRANSFORM_UI_H
#define PC_UI_SHELL_TRANSFORM_UI_H
#include "shell_internal.h"
#include "ui/canvas.h"
#include "ui/filter_preview.h"
#include "ui/tool_gesture.h"
typedef struct pc_ui_transform_runtime {
    pc_ui_filter_preview preview;
    pc_doc_arc *committing_preview;
    bool dragging, committing, numeric_edit;
    unsigned reference;
    pc_ui_transform_frame numeric_before;
    uint64_t session;
    pc_ui_tool tool;
    pc_ui_transform_drag drag;
    pc_ui_transform_frame undo[128], redo[128];
    size_t nundo, nredo;
    pc_layer_id selected[4096];
    size_t nselected;
} pc_ui_transform_runtime;
pc_ui_transform_runtime *pc_ui_shell_transform_runtime(pc_ui_shell_app *app);
void pc_ui_shell_transform_fini(pc_ui_transform_runtime *runtime);
pc_status pc_ui_shell_transform_command(pc_ui_shell_app *app, const char *command, yyjson_val *params,
                                        pc_json_value *out, pc_error *err);
pc_status pc_ui_shell_transform_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                        unsigned modifiers, pc_json_value *out, pc_error *err);
bool pc_ui_shell_transform_canvas(pc_ui_shell_app *app, const pc_ui_canvas_xform *transform,
                                  const SDL_Event *event, float factor);
bool pc_ui_shell_transform_key(pc_ui_shell_app *app, const SDL_Event *event);
pc_doc_arc *pc_ui_shell_transform_preview(pc_ui_shell_app *app, uint64_t *generation);
bool pc_ui_shell_transform_controls_frame(pc_ui_shell_app *app, pc_ui_transform_frame *frame);
bool pc_ui_shell_transform_controls_hit(const pc_ui_transform_frame *frame, const double point[2],
                                        double zoom);
void pc_ui_shell_transform_controls_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *transform);
void pc_ui_shell_transform_draw(pc_ui_shell_app *app, const pc_ui_canvas_xform *transform);
void pc_ui_shell_transform_options(pc_ui_shell_app *app);
SDL_SystemCursor pc_ui_shell_transform_cursor(pc_ui_shell_app *app, const pc_ui_canvas_xform *transform,
                                              float x, float y);
SDL_SystemCursor pc_ui_shell_frame_cursor(const pc_ui_transform_frame *frame, bool rotate,
                                          const pc_ui_canvas_xform *xf, float x, float y);
#endif
