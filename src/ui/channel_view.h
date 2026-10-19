#ifndef PC_UI_CHANNEL_VIEW_H
#define PC_UI_CHANNEL_VIEW_H
#include "canvas.h"
typedef struct pc_ui_channel_overlay {
    bool active;
    const pc_surface *surface; /* alpha coverage; NULL means zero everywhere */
    float color[3], opacity;
    uint64_t key; /* changes with the overlay target; pixels follow the source revision */
} pc_ui_channel_overlay;
typedef struct pc_ui_channel_cache {
    pc_compose_buffer image; /* premultiplied overlay, normalised to the canvas texture region */
    pc_doc_id document;
    uint64_t key, source_generation, generation;
    bool valid, active, in_color;
    bool extra_active;
    uint64_t extra_key;
    float extra_color[3], extra_opacity;
} pc_ui_channel_cache;
pc_status pc_ui_channel_cache_update_overlay(pc_ui_channel_cache *cache, const pc_engine_doc_state *state,
                                             const pc_ui_canvas_cache *source, bool in_color,
                                             const pc_ui_channel_overlay *overlay, pc_error *err);
void pc_ui_channel_cache_fini(pc_ui_channel_cache *cache);
pc_status pc_ui_channel_cache_update(pc_ui_channel_cache *cache, const pc_engine_doc_state *state,
                                     const pc_ui_canvas_cache *composite, bool in_color, pc_error *err);
#endif
