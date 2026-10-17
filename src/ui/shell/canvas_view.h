/* The SDL/GL canvas adapter. Owned by PhotocraftApp; finalise before destroying its GL context. */
#ifndef PC_UI_SHELL_CANVAS_VIEW_H
#define PC_UI_SHELL_CANVAS_VIEW_H
#include "shell_internal.h"
#include "gl33.h"
#include "ui/canvas.h"

typedef struct pc_ui_canvas_view {
    pc_ui_canvas_cache cache;
    pc_doc_id *documents;
    size_t n_documents;
    pc_ui_canvas_xform xf;
    bool laid_out, dragging;
    float last_pointer[2], press_pointer[2], press_zoom, press_rotation, press_angle;
    pc_ui_tool drag_tool;
    uint8_t drag_button;
    bool moved;
    GLuint program, vao, source, target, framebuffer;
    uint64_t uploaded;
    int width, height;
} pc_ui_canvas_view;

pc_status pc_ui_canvas_sync(pc_ui_shell_app *app, pc_ui_canvas_view *view, pc_error *err);
pc_ui_view *pc_ui_canvas_active_view(pc_ui_shell_app *app);
void pc_ui_canvas_view_fini(pc_ui_canvas_view *view, const gl33 *gl);
pc_status pc_ui_canvas_view_draw(pc_ui_shell_app *app, pc_ui_canvas_view *view, const gl33 *gl,
                                 struct nk_rect rect, pc_error *err);
bool pc_ui_canvas_view_event(pc_ui_shell_app *app, pc_ui_canvas_view *view, const SDL_Event *event);
/* Returns NOT_FOUND for commands outside the view camera's responsibility. */
pc_status pc_ui_canvas_view_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                    pc_json_value *out, pc_error *err);
#endif
