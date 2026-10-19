#ifndef PC_UI_SHELL_VECTOR_PREVIEW_H
#define PC_UI_SHELL_VECTOR_PREVIEW_H
#include "app.h"
#include "ui/tool_gesture.h"
#include "vector/vector.h"
typedef struct pc_ui_vector_preview {
    bool valid;
    uint8_t key[32];
    float scale;
    pc_vec_compiled_shape compiled;
    pc_vec_polyline *paths;
    size_t npaths;
    pc_rect clip;
} pc_ui_vector_preview;
void pc_ui_vector_preview_fini(pc_ui_vector_preview *preview);
pc_status pc_ui_shell_vector_preview(pc_ui_shell_app *app, pc_ui_vector_preview *preview,
                                     const pc_ui_canvas_xform *xf, const pc_ui_tool_gesture *gesture,
                                     pc_error *err);
#endif
