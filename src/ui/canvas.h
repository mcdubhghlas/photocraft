/* Canvas camera and CPU display cache. Coordinates are logical screen points; zoom in UiState
 * is device pixels per document pixel. These routines are independent of SDL/GL, allowing the
 * interactive and control paths to use exactly the same mapping. Not thread-safe. */
#ifndef PC_UI_CANVAS_H
#define PC_UI_CANVAS_H

#include "compose/compose.h"
#include "engine/engine.h"
#include "ui/state.h"
#include "gpu/gpu.h"

typedef struct pc_ui_canvas_xform {
    float x, y, width, height, zoom, center[2], rotation, aspect;
    bool flip;
} pc_ui_canvas_xform;

/* Clip screen-space segments to the viewport and native command-coordinate range.
 * Rejected/non-finite segments preserve both endpoints. */
bool pc_ui_canvas_clip_line(const pc_ui_canvas_xform *xf, float a[2], float b[2]);
float pc_ui_zoom_min(const uint32_t size[2]);
float pc_ui_zoom_clamp(float zoom, const uint32_t size[2]);
float pc_ui_zoom_step(float zoom, int direction, const uint32_t size[2]);
float pc_ui_rotation_wrap(float degrees);
float pc_ui_rotation_snap(float degrees, float step);
pc_status pc_ui_rotation_params(float current, yyjson_val *params, float *out, pc_error *err);
/* All mappings reject non-finite or degenerate transforms; outputs remain untouched on error. */
pc_status pc_ui_canvas_to_screen(const pc_ui_canvas_xform *xf, float x, float y, float out[2]);
pc_status pc_ui_canvas_to_doc(const pc_ui_canvas_xform *xf, float x, float y, float out[2]);
void pc_ui_canvas_fit(pc_ui_view *view, float width, float height, float ppp, float aspect, bool fill);
pc_status pc_ui_canvas_anchor(pc_ui_view *view, const pc_ui_canvas_xform *xf, float x, float y, float zoom,
                              float ppp);
pc_status pc_ui_canvas_pan(pc_ui_view *view, const pc_ui_canvas_xform *xf, float dx, float dy);
/* Apply a pinch/rotate/pan delta atomically: the document point under `from` stays under `to`.
 * rotation is in screen degrees, respecting horizontal view flip. */
pc_status pc_ui_canvas_gesture(pc_ui_view *view, const pc_ui_canvas_xform *xf, const float from[2],
                               const float to[2], float zoom_factor, float rotation_delta, float ppp);
/* Rulers stay axis-aligned during camera rotation, with zero anchored to the displayed document
 * origin. The step is in ruler units (pixels require whole steps). */
pc_status pc_ui_canvas_ruler_xform(const pc_ui_canvas_xform *xf, pc_ui_canvas_xform *out);
double pc_ui_ruler_nice_step(double minimum, bool whole);

typedef struct pc_ui_wheel_input {
    float scroll[2], raw[2], zoom_delta, notch;
    bool alt, zoom_with_wheel, pinch_zooms;
} pc_ui_wheel_input;
typedef enum pc_ui_wheel_kind { PC_UI_WHEEL_NONE, PC_UI_WHEEL_PAN, PC_UI_WHEEL_ZOOM } pc_ui_wheel_kind;
/* v[0] = zoom factor, or v = pan vector; outputs zero for NONE. */
pc_ui_wheel_kind pc_ui_wheel_classify(pc_ui_wheel_input input, float v[2]);

typedef struct pc_ui_scroll_span {
    float length, first, last;
} pc_ui_scroll_span;
bool pc_ui_scroll_span_make(float length, float center, float size, float zoom, pc_ui_scroll_span *out);
void pc_ui_scroll_extent(pc_ui_scroll_span span, bool overscroll, float out[2]);
void pc_ui_scroll_thumb(pc_ui_scroll_span span, const float extent[2], float track, float out[2]);
float pc_ui_scroll_doc_per_point(pc_ui_scroll_span span, const float extent[2], float track);
float pc_ui_scroll_clamp(float center, float length, float size, float zoom);
/* Overscroll-off bounds in displayed space, including camera rotation and pixel aspect. */
bool pc_ui_scroll_clamp_view(pc_ui_view *view, float width, float height, float ppp, float aspect);

typedef struct pc_ui_scrollbar {
    float rect[4]; /* x,y,width,height, in logical screen points */
    pc_ui_scroll_span span;
    float extent[2], thumb[2], center;
    unsigned axis;
    bool shown, flipped;
} pc_ui_scrollbar;
typedef struct pc_ui_scroll_drag {
    pc_ui_scrollbar bar; /* frozen geometry/extent at press */
    float origin, scale, press_cross;
    bool active, thumb, moved;
} pc_ui_scroll_drag;
/* Both tracks overlay the viewport (12 points); spans cover its full size, as in the reference.
 * Invalid input leaves output untouched. No allocation or document mutation. */
bool pc_ui_scrollbars_layout(const pc_ui_canvas_xform *xf, const uint32_t size[2], bool overscroll,
                             pc_ui_scrollbar out[2]);
bool pc_ui_scrollbar_press(const pc_ui_scrollbar bars[2], float x, float y, pc_ui_scroll_drag *drag);
/* On a changed centre, writes the document coordinate for drag.bar.axis. A page click applies
 * on release; dragging the thumb keeps its original extent. Focus loss cancels by clearing active. */
bool pc_ui_scrollbar_motion(pc_ui_scroll_drag *drag, float x, float y, bool release, float *center);

/* Cache at most 4096 pixels on either side; keyed by document identity + revision. Source pixels
 * remain f32 through composition/reduction, with premultiplied colour for correct texture interpolation. A
 * failed refresh preserves the previous owned image but never reports it as the new document. */
typedef struct pc_ui_canvas_cache {
    pc_doc_id document;
    uint64_t revision, generation;
    uint32_t render_key;
    pc_compose_buffer image;
    pc_rect source_rect; /* document coordinates covered by image */
    pc_rect last_render; /* actual latest composite region, empty on an unaffected revision */
    bool partial;
    bool valid, region;
} pc_ui_canvas_cache;
uint32_t pc_ui_canvas_render_key(void);
/* Requires the immediately following revision, an unchanged camera/render context and
 * bounded local damage. Spatial layer effects conservatively invalidate the whole cache. */
bool pc_ui_canvas_cache_damage_rect(const pc_ui_canvas_cache *cache, const pc_engine_doc_state *state,
                                    pc_rect *out);
bool pc_ui_canvas_cache_can_damage(const pc_ui_canvas_cache *cache, const pc_engine_doc_state *state);
void pc_ui_canvas_cache_fini(pc_ui_canvas_cache *cache);
pc_status pc_ui_canvas_cache_update(pc_ui_canvas_cache *cache, const pc_engine_doc_state *state,
                                    pc_error *err);

/* Retain full-resolution pixels around a zoomed-in viewport, with padded tile-aligned reuse. */
pc_status pc_ui_canvas_cache_update_view(pc_ui_canvas_cache *cache, const pc_engine_doc_state *state,
                                         const pc_ui_canvas_xform *xf, pc_error *err);

/* Full-resolution display tiles for source viewports wider than a single 4096-side texture.
 * Tiles include a two-pixel gutter, while coverage is disjoint. Cache hits retain generations
 * across pans; revisions invalidate all tiles. At most 64 tiles / 32M source pixels are retained.
 * A budget overflow returns LIMIT; the caller can keep the area-averaged document preview. */
#define PC_UI_CANVAS_TILE_CAP 64
typedef struct pc_ui_canvas_tile {
    pc_ui_canvas_cache cache;
    pc_rect coverage;
} pc_ui_canvas_tile;
typedef struct pc_ui_canvas_tiles {
    pc_ui_canvas_tile items[PC_UI_CANVAS_TILE_CAP];
    size_t count;
    uint64_t generation;
} pc_ui_canvas_tiles;
void pc_ui_canvas_tiles_fini(pc_ui_canvas_tiles *tiles);
pc_status pc_ui_canvas_tiles_update(pc_ui_canvas_tiles *tiles, const pc_engine_doc_state *state,
                                    const pc_ui_canvas_xform *xf, pc_error *err);

/* GPU-enabled variants retain the same cache contracts and automatically use the CPU oracle
 * for unsupported plans, resource limits or a faulted device. A NULL gpu is CPU-only. */
pc_status pc_ui_canvas_cache_update_view_gpu(pc_ui_canvas_cache *cache, const pc_engine_doc_state *state,
                                             const pc_ui_canvas_xform *xf, pc_gpu_compositor *gpu,
                                             pc_error *err);
pc_status pc_ui_canvas_tiles_update_gpu(pc_ui_canvas_tiles *tiles, const pc_engine_doc_state *state,
                                        const pc_ui_canvas_xform *xf, pc_gpu_compositor *gpu, pc_error *err);
/* Crop editing exposes retained pixels on the document plane, including outside the canvas.
 * Viewport padding/cache and reductions remain bounded to 4096 pixels per output edge. */
pc_status pc_ui_canvas_cache_update_plane(pc_ui_canvas_cache *cache, const pc_engine_doc_state *state,
                                          const pc_ui_canvas_xform *xf, pc_gpu_compositor *gpu,
                                          pc_error *err);
pc_status pc_ui_canvas_compose(const pc_document *doc, pc_rect rect, pc_gpu_compositor *gpu,
                               pc_compose_buffer *out, pc_error *err);
#endif
