/* The SDL/GL canvas adapter. Owned by PhotocraftApp; finalise before destroying its GL context. */
#ifndef PC_UI_SHELL_CANVAS_VIEW_H
#define PC_UI_SHELL_CANVAS_VIEW_H
#include "shell_internal.h"
#include "gl33.h"
#include "ui/canvas.h"
#include "overlays.h"
#include "cursors.h"
#include "vector_preview.h"
#include "ui/channel_view.h"
#include "engine/display_color.h"
#include "ui/tool_gesture.h"

typedef struct pc_ui_canvas_tile_upload {
    pc_rect coverage;
    uint64_t uploaded, channels_uploaded;
    bool source_linear;
    GLuint source, channels_texture;
    pc_ui_channel_cache channels;
} pc_ui_canvas_tile_upload;

typedef struct pc_ui_canvas_view {
    pc_ui_tool_gesture gesture;
    pc_doc_arc *floating_display;
    uint64_t floating_generation;
    struct {
        bool active, vary;
        uint8_t button;
        pc_doc_id document;
        uint64_t revision, sequence;
        pc_ui_tool tool;
        float anchor[2], pointer[2], start[2], zoom;
    } brush_resize;
    pc_ui_vector_preview vector_preview, pen_preview;
    SDL_PenID pen_id;
    bool pen_down, pen_event, pen_eraser;
    pc_ui_tool_point pen;
    struct {
        SDL_TouchID device;
        SDL_FingerID id;
        float point[2];
        bool down;
    } fingers[2];
    double last_pinch;
    pc_ui_native_cursor cursor;
    bool held_keys[PC_KEY_EXCLAMATIONMARK + 1];
    unsigned held_mods;
    bool zoom_out;
    pc_gpu_compositor *gpu;
    bool gpu_tried;
    pc_ui_canvas_tiles tiles;
    pc_ui_canvas_tile_upload tile_uploads[PC_UI_CANVAS_TILE_CAP];
    pc_ui_canvas_cache cache;
    pc_engine_display display;
    pc_ui_channel_cache channels;
    uint64_t channels_uploaded;
    GLuint channels_texture;
    uint64_t display_key;
    bool display_valid;
    GLuint display_texture;
    pc_ui_overlay_cache overlays;
    pc_ui_guide_drag guide_drag;
    pc_doc_id *documents;
    size_t n_documents;
    pc_ui_canvas_xform xf;
    pc_ui_scrollbar bars[2];
    pc_ui_scroll_drag scroll_drag;
    bool laid_out, dragging;
    float last_pointer[2], press_pointer[2], press_zoom, press_rotation, press_angle;
    pc_ui_tool drag_tool;
    uint8_t drag_button;
    bool moved;
    GLuint program, vao, source, target, framebuffer;
    uint64_t uploaded;
    bool source_linear;
    int width, height;
} pc_ui_canvas_view;

pc_status pc_ui_canvas_sync(pc_ui_shell_app *app, pc_ui_canvas_view *view, pc_error *err);
pc_ui_view *pc_ui_canvas_active_view(pc_ui_shell_app *app);
void pc_ui_canvas_view_fini(pc_ui_canvas_view *view, const gl33 *gl);
pc_status pc_ui_canvas_view_draw(pc_ui_shell_app *app, pc_ui_canvas_view *view, const gl33 *gl,
                                 struct nk_rect rect, pc_error *err);
bool pc_ui_canvas_pen_event(pc_ui_shell_app *app, pc_ui_canvas_view *view, const SDL_Event *event);
bool pc_ui_canvas_touch_event(pc_ui_shell_app *app, pc_ui_canvas_view *view, const pc_ui_canvas_xform *xf,
                              const SDL_Event *event);
bool pc_ui_canvas_view_event(pc_ui_shell_app *app, pc_ui_canvas_view *view, const SDL_Event *event);
/* Returns NOT_FOUND for commands outside the view camera's responsibility. */
pc_status pc_ui_canvas_view_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                    pc_json_value *out, pc_error *err);
#endif
