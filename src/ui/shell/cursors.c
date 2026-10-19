#include "layer_picker_ui.h"
#include "transform_ui.h"
#include "artboard_ui.h"
#include "crop_ui.h"
#include "cursors.h"
#include "canvas_view.h"
#include <math.h>
#include <string.h>
#include <blake3.h>
#include "engine/prefs.h"
#include "ui/hold_keys.h"

void pc_ui_native_cursor_fini(pc_ui_native_cursor *cursor)
{
    if (!cursor) {
        return;
    }
    if (cursor->hidden)
        (void)SDL_ShowCursor();
    if (cursor->bitmap && SDL_GetCursor() == cursor->bitmap) {
        (void)SDL_SetCursor(NULL);
    }
    SDL_DestroyCursor(cursor->bitmap);
    for (size_t i = 0; i < SDL_SYSTEM_CURSOR_COUNT; i++) {
        if (cursor->system[i] && SDL_GetCursor() == cursor->system[i]) {
            (void)SDL_SetCursor(NULL);
        }
        SDL_DestroyCursor(cursor->system[i]);
    }
    if (cursor->brush_valid) {
        pc_paint_brush_fini(&cursor->brush);
    }
    pc_paint_outline_fini(&cursor->outline);
    *cursor = (pc_ui_native_cursor){0};
}
static void system_cursor(pc_ui_native_cursor *cursor, SDL_SystemCursor kind)
{
    if (!cursor->system[kind]) {
        cursor->system[kind] = SDL_CreateSystemCursor(kind);
    }
    (void)SDL_SetCursor(cursor->system[kind]);
    cursor->used = true;
}
void pc_ui_native_cursor_end(pc_ui_native_cursor *cursor)
{
    if (cursor->hidden && !cursor->used) {
        (void)SDL_ShowCursor();
        cursor->hidden = false;
    }
    if (!cursor->used && SDL_GetCursor() != SDL_GetDefaultCursor()) {
        (void)SDL_SetCursor(NULL);
    }
}
static bool preference(pc_ui_shell_app *app, const char *path, const char *text)
{
    pc_json_value value = PC_JSON_VALUE_INIT;
    bool result = false;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), path, &value, NULL) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&value);
        result = text ? yyjson_mut_is_str(root) && strcmp(yyjson_mut_get_str(root), text) == 0
                      : yyjson_mut_is_bool(root) && yyjson_mut_get_bool(root);
    }
    pc_json_value_fini(&value);
    return result;
}
static void shape_key(const pc_ui_cursor_shape *shape, float scale, uint8_t out[32])
{
    blake3_hasher hash;
    blake3_hasher_init(&hash);
    blake3_hasher_update(&hash, &shape->kind, sizeof shape->kind);
    blake3_hasher_update(&hash, &shape->radius, sizeof shape->radius);
    blake3_hasher_update(&hash, &shape->length, sizeof shape->length);
    blake3_hasher_update(&hash, &shape->gap, sizeof shape->gap);
    blake3_hasher_update(&hash, &shape->center, sizeof shape->center);
    blake3_hasher_update(&hash, &shape->closed, sizeof shape->closed);
    blake3_hasher_update(&hash, &shape->badge, sizeof shape->badge);
    blake3_hasher_update(&hash, &scale, sizeof scale);
    for (size_t i = 0; i < shape->count; i++) {
        blake3_hasher_update(&hash, &shape->rings[i].n, sizeof shape->rings[i].n);
        blake3_hasher_update(&hash, shape->rings[i].pts, shape->rings[i].n * sizeof *shape->rings[i].pts);
    }
    blake3_hasher_finalize(&hash, out, 32);
}
static bool native_bitmap(pc_ui_native_cursor *cursor, const pc_ui_cursor_shape *shape, float scale)
{
    uint8_t key[32];
    shape_key(shape, scale, key);
    if (cursor->valid && memcmp(key, cursor->key, 32) == 0) {
        (void)SDL_SetCursor(cursor->bitmap);
        cursor->used = true;
        return true;
    }
    pc_ui_cursor_bitmap bitmap = {0};
#if defined(__APPLE__)
    const uint16_t limit = 2047;
#else
    const uint16_t limit = 255;
#endif
    if (pc_ui_cursor_rasterize(shape, scale, limit, &bitmap, NULL) != PC_OK) {
        return false;
    }
    SDL_Surface *surface = SDL_CreateSurfaceFrom(bitmap.side, bitmap.side, SDL_PIXELFORMAT_RGBA32,
                                                 bitmap.rgba, (int)bitmap.side * 4);
    SDL_Cursor *native = surface ? SDL_CreateColorCursor(surface, bitmap.hotspot, bitmap.hotspot) : NULL;
    SDL_DestroySurface(surface);
    pc_ui_cursor_bitmap_fini(&bitmap);
    if (!native) {
        return false;
    }
    (void)SDL_SetCursor(native);
    SDL_DestroyCursor(cursor->bitmap);
    cursor->bitmap = native;
    cursor->used = cursor->valid = true;
    memcpy(cursor->key, key, 32);
    return true;
}
static void paint_fallback(pc_ui_shell_app *app, const pc_ui_cursor_shape *shape, struct nk_vec2 point)
{
    struct nk_command_buffer *commands = nk_window_get_canvas(pc_ui_shell_app_nk(app));
    for (unsigned pass = 0; pass < 2; pass++) {
        struct nk_color color = pass ? nk_rgba(255, 255, 255, 235) : nk_rgba(0, 0, 0, 160);
        float width = pass ? 1.f : 3.f;
        if (shape->kind == PC_UI_CURSOR_CIRCLE) {
            float r = shape->radius;
            if (isfinite(r) && r > 0 && r < 1e7f) {
                nk_stroke_circle(commands, nk_rect(point.x - r, point.y - r, r * 2, r * 2), width, color);
            }
        } else if (shape->kind == PC_UI_CURSOR_OUTLINE) {
            for (size_t r = 0; r < shape->count; r++) {
                const pc_paint_ring *ring = &shape->rings[r];
                for (size_t i = 0; ring->n >= 2 && i < ring->n; i++) {
                    const float *a = ring->pts[i], *b = ring->pts[(i + 1) % ring->n];
                    nk_stroke_line(commands, point.x + a[0], point.y + a[1], point.x + b[0], point.y + b[1],
                                   width, color);
                }
            }
        }
    }
}
void pc_ui_native_cursor_draw(pc_ui_shell_app *app, struct pc_ui_canvas_view *v)
{
    if (v->brush_resize.active) {
        (void)SDL_HideCursor();
        v->cursor.hidden = true;
        v->cursor.used = true;
        return;
    }
    if (v->cursor.hidden) {
        (void)SDL_ShowCursor();
        v->cursor.hidden = false;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_vec2 point = ctx->input.mouse.pos;
    const pc_ui_canvas_xform *xf = &v->xf;
    if (!v->laid_out || point.x < xf->x || point.y < xf->y || point.x >= xf->x + xf->width ||
        point.y >= xf->y + xf->height || pc_ui_shell_keyboard_captured(app)) {
        return;
    }
    pc_ui_native_cursor *cursor = &v->cursor;
    if (pc_ui_shell_app_state(app)->transform.active) {
        system_cursor(cursor, pc_ui_shell_transform_cursor(app, xf, point.x, point.y));
        return;
    }
    if (pc_ui_layer_picker_armed(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app))) {
        system_cursor(cursor, SDL_SYSTEM_CURSOR_CROSSHAIR);
        return;
    }
    pc_ui_tool tool = pc_ui_shell_app_state(app)->tool;
    pc_ui_transform_frame controls;
    if (pc_ui_shell_transform_controls_frame(app, &controls)) {
        float doc[2];
        if (pc_ui_canvas_to_doc(xf, point.x, point.y, doc) == PC_OK) {
            const double p[2] = {doc[0], doc[1]};
            if (pc_ui_shell_transform_controls_hit(&controls, p, xf->zoom)) {
                system_cursor(cursor, pc_ui_shell_frame_cursor(&controls, false, xf, point.x, point.y));
                return;
            }
        }
    }
    if (tool == PC_TOOL_ARTBOARD) {
        pc_layer_id id;
        pc_rect rect;
        if (pc_ui_artboard_active(pc_engine_session_active(pc_ui_shell_app_session(app)), &id, &rect)) {
            double bounds[4] = {rect.x0, rect.y0, rect.x1, rect.y1};
            pc_ui_transform_frame frame;
            pc_ui_transform_frame_rect(&frame, bounds);
            system_cursor(cursor, pc_ui_shell_frame_cursor(&frame, false, xf, point.x, point.y));
        } else
            system_cursor(cursor, SDL_SYSTEM_CURSOR_CROSSHAIR);
        return;
    }
    if (tool == PC_TOOL_CROP && pc_ui_shell_app_state(app)->has_crop) {
        const pc_ui_state *ui = pc_ui_shell_app_state(app);
        pc_ui_crop_frame crop = {.present = true, .angle = ui->crop_angle};
        memcpy(crop.rect, ui->crop_rect, sizeof crop.rect);
        pc_ui_transform_frame frame;
        pc_ui_crop_quad(&crop, &frame);
        system_cursor(cursor, pc_ui_shell_frame_cursor(&frame, false, xf, point.x, point.y));
        return;
    }
    pc_ui_temporary temporary;
    if (v->dragging) {
        tool = v->drag_tool;
    } else if (v->gesture.active) {
        tool = v->gesture.tool;
    } else if (pc_ui_temporary_resolve(pc_ui_shell_app_session(app), tool, v->held_keys, v->held_mods, false,
                                       true, &temporary)) {
        tool = pc_ui_temporary_tool(temporary);
    }
    pc_ui_cursor_shape shape = {.kind = PC_UI_CURSOR_CROSS, .length = 6};
    float points[512][2];
    pc_paint_ring rings[512];
    if (v->guide_drag.active) {
        system_cursor(cursor,
                      v->guide_drag.vertical ? SDL_SYSTEM_CURSOR_EW_RESIZE : SDL_SYSTEM_CURSOR_NS_RESIZE);
        return;
    }
    if (tool == PC_TOOL_HAND) {
        shape.kind = PC_UI_CURSOR_HAND;
        shape.closed = v->dragging;
    } else if (pc_ui_tool_is_type(tool)) {
        system_cursor(cursor, SDL_SYSTEM_CURSOR_TEXT);
        return;
    } else if (tool == PC_TOOL_MOVE || tool == PC_TOOL_ARTBOARD) {
        system_cursor(cursor, SDL_SYSTEM_CURSOR_MOVE);
        return;
    } else if (pc_ui_tool_is_brushlike(tool) || tool == PC_TOOL_QUICK_SELECTION) {
        bool caps = (SDL_GetModState() & SDL_KMOD_CAPS) != 0;
        if (!caps && preference(app, "cursors.painting", "standard")) {
            system_cursor(cursor, SDL_SYSTEM_CURSOR_DEFAULT);
            return;
        }
        if (!caps && !preference(app, "cursors.painting", "precise") &&
            !(v->dragging && preference(app, "cursors.showOnlyCrosshairWhilePainting", NULL))) {
            const pc_paint_brush *brush = &pc_engine_session_tools(pc_ui_shell_app_session(app))->brush;
            bool full = preference(app, "cursors.painting", "fullSizeTip");
            if (!cursor->brush_valid || cursor->full != full || !pc_paint_brush_eq(&cursor->brush, brush)) {
                pc_paint_outline next = {0};
                if (pc_paint_tip_outline(brush, full, &next, NULL) == PC_OK) {
                    pc_paint_outline_fini(&cursor->outline);
                    cursor->outline = next;
                    if (cursor->brush_valid) {
                        pc_paint_brush_fini(&cursor->brush);
                    }
                    pc_paint_brush_copy(&cursor->brush, brush);
                    cursor->brush_valid = true;
                    cursor->full = full;
                }
            }
            float radius = fmaxf(1, brush->size / 2 * xf->zoom);
            shape.kind = PC_UI_CURSOR_CIRCLE;
            shape.radius = full ? radius : fmaxf(1, pc_paint_half_coverage_radius(radius, brush->hardness));
            shape.center = preference(app, "cursors.showCrosshairInBrushTip", NULL);
            if (cursor->outline.count && cursor->outline.count <= 512) {
                /* Map vectors without subtracting large document coordinates. */
                float angle = xf->rotation * .017453292519943295f;
                float cosine = cosf(angle), sine = sinf(angle), extent = 0;
                size_t count = 0;
                for (size_t r = 0; r < cursor->outline.count; r++) {
                    const pc_paint_ring *source = &cursor->outline.rings[r];
                    if (source->n > 512 - count) {
                        break;
                    }
                    rings[r] = (pc_paint_ring){.pts = &points[count], .n = source->n};
                    for (size_t i = 0; i < source->n; i++) {
                        float x = source->pts[i][0] * radius * xf->aspect * (xf->flip ? -1 : 1);
                        float y = source->pts[i][1] * radius;
                        points[count][0] = cosine * x - sine * y;
                        points[count][1] = sine * x + cosine * y;
                        extent = fmaxf(extent, hypotf(points[count][0], points[count][1]));
                        count++;
                    }
                    shape.count++;
                }
                if (shape.count) {
                    shape.kind = PC_UI_CURSOR_OUTLINE;
                    shape.rings = rings;
                    if (extent < 3) {
                        shape = (pc_ui_cursor_shape){.kind = PC_UI_CURSOR_CROSS, .length = 4};
                    }
                }
            }
        }
    } else if (pc_ui_is_selection_tool(tool)) {
        shape.length = 8;
        shape.gap = 2;
        const pc_engine_doc_state *active = pc_engine_session_active(pc_ui_shell_app_session(app));
        const pc_document *doc = active ? pc_doc_arc_get(active->doc) : NULL;
        bool selected =
            doc && doc->has_selection && !pc_rect_is_empty(pc_surface_content_bounds(&doc->selection));
        if (!(v->held_mods & PC_UI_MOD_COMMAND)) {
            shape.badge = pc_ui_badge_from_mode(pc_ui_selection_mode(
                tool, pc_ui_shell_app_state(app)->selection_mode, v->held_mods, selected));
        }
    } else if (tool == PC_TOOL_ADD_ANCHOR_POINT || tool == PC_TOOL_DELETE_ANCHOR_POINT) {
        bool add = tool == PC_TOOL_ADD_ANCHOR_POINT;
        if (v->held_mods & PC_UI_MOD_ALT) {
            add = !add;
        }
        shape.badge = add ? PC_UI_BADGE_ADD : PC_UI_BADGE_SUBTRACT;
    }
    float scale = pc_ui_shell_app_ui_scale(app);
    if (!native_bitmap(cursor, &shape, scale)) {
        paint_fallback(app, &shape, point);
        pc_ui_cursor_shape cross = {.kind = PC_UI_CURSOR_CROSS, .length = 6, .gap = 2};
        if (!native_bitmap(cursor, &cross, scale)) {
            system_cursor(cursor, SDL_SYSTEM_CURSOR_CROSSHAIR);
        }
    }
}
