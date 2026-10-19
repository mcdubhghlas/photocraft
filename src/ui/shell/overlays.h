#ifndef PC_UI_SHELL_OVERLAYS_H
#define PC_UI_SHELL_OVERLAYS_H
#include "shell_internal.h"
#include "ui/canvas.h"
#include "ui/outline.h"
typedef struct pc_ui_overlay_cache {
    pc_ui_outline selection;
    pc_doc_id document;
    uint64_t revision;
    uint32_t step;
    pc_rect region;
    bool valid;
} pc_ui_overlay_cache;
typedef struct pc_ui_guide_drag {
    pc_doc_id document;
    uint64_t revision;
    size_t index;
    float position;
    bool active, vertical, existing;
} pc_ui_guide_drag;
pc_status pc_ui_overlays_draw(pc_ui_shell_app *app, const pc_engine_doc_state *st,
                              const pc_ui_canvas_xform *xf, pc_ui_overlay_cache *cache,
                              const pc_ui_guide_drag *drag, pc_error *err);
bool pc_ui_guide_event(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf, pc_ui_guide_drag *drag,
                       const SDL_Event *event);
#endif
