#include "tool_cursor.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "base/mem.h"

void pc_ui_cursor_bitmap_fini(pc_ui_cursor_bitmap *bitmap)
{
    if (bitmap) {
        free(bitmap->rgba);
        *bitmap = (pc_ui_cursor_bitmap){0};
    }
}
static float clamp(float x, float low, float high)
{
    return fminf(high, fmaxf(low, x));
}
static float segment(float x, float y, const float a[2], const float b[2])
{
    float dx = b[0] - a[0], dy = b[1] - a[1];
    float t = clamp(((x - a[0]) * dx + (y - a[1]) * dy) / fmaxf(1e-12f, dx * dx + dy * dy), 0, 1);
    return hypotf(x - a[0] - dx * t, y - a[1] - dy * t);
}
static float cross_distance(float x, float y, float length, float gap)
{
    x = fabsf(x);
    y = fabsf(y);
    return fminf(hypotf(x - clamp(x, gap, length), y), hypotf(x, y - clamp(y, gap, length)));
}
static float badge_distance(pc_ui_badge badge, float x, float y)
{
    x -= 12;
    y -= 12;
    if (badge == PC_UI_BADGE_ADD) {
        return cross_distance(x, y, 3, 0);
    }
    if (badge == PC_UI_BADGE_SUBTRACT) {
        return hypotf(x - clamp(x, -3, 3), y);
    }
    if (badge == PC_UI_BADGE_INTERSECT) {
        const float a[2] = {-2.4f, -2.4f}, b[2] = {2.4f, 2.4f};
        return fminf(segment(x, y, a, b), segment(-x, y, a, b));
    }
    return INFINITY;
}

static float capsule(float x, float y, float ax, float ay, float bx, float by, float r)
{
    const float a[2] = {ax, ay}, b[2] = {bx, by};
    return segment(x, y, a, b) - r;
}
static float box_distance(float x, float y, float cx, float cy, float hx, float hy, float r)
{
    float qx = fabsf(x - cx) - hx + r, qy = fabsf(y - cy) - hy + r;
    return hypotf(fmaxf(0, qx), fmaxf(0, qy)) + fminf(0, fmaxf(qx, qy)) - r;
}
static void hand_distance(bool closed, float x, float y, float *edge, float *crease)
{
    static const float xs[4] = {-5.1f, -1.7f, 1.7f, 5.1f};
    static const float tips[4] = {-7, -9.5f, -8.5f, -5.5f};
    x -= 2;
    y -= .5f;
    float d = closed ? box_distance(x, y, 0, 3, 7.2f, 6.5f, 3.6f)
                     : box_distance(x, y, 0, 4.5f, 6.8f, 5.5f, 3);
    for (size_t i = 0; i < 4; i++) {
        d = fminf(d, capsule(x, y, xs[i], closed ? -1.f : 3.f, xs[i],
                             closed ? -2.2f : tips[i], closed ? 1.9f : 1.7f));
    }
    *edge = fminf(d, capsule(x, y, -4.5f, 7, closed ? -9.6f : -10.5f,
                             closed ? 3.6f : 1.5f, closed ? 2.f : 1.9f));
    d = closed ? capsule(x, y, -4, 4.4f, 6, 4.4f, 0) : INFINITY;
    for (size_t i = 0; i < 3; i++) {
        float fx = -3.4f + (float)i * 3.4f;
        d = fminf(d, capsule(x, y, fx, closed ? -2.6f : fmaxf(tips[i], tips[i + 1]) + 1,
                             fx, closed ? 4.4f : 3.5f, 0));
    }
    *crease = d;
}
static pc_status extent(const pc_ui_cursor_shape *shape, float *out, pc_error *err)
{
    float e = 0;
    if (shape->kind == PC_UI_CURSOR_CIRCLE) {
        if (!isfinite(shape->radius) || shape->radius <= 0) {
            return pc_fail(err, PC_ERR_INVALID, "invalid cursor radius");
        }
        e = shape->radius;
    } else if (shape->kind == PC_UI_CURSOR_CROSS) {
        if (!isfinite(shape->length) || !isfinite(shape->gap) || shape->gap < 0 ||
            shape->length < shape->gap) {
            return pc_fail(err, PC_ERR_INVALID, "invalid cursor crosshair");
        }
        e = shape->length;
    } else if (shape->kind == PC_UI_CURSOR_OUTLINE) {
        if (shape->count > 1024 || (shape->count && !shape->rings)) {
            return pc_fail(err, PC_ERR_LIMIT, "cursor outline rings");
        }
        size_t points = 0;
        for (size_t i = 0; i < shape->count; i++) {
            const pc_paint_ring *ring = &shape->rings[i];
            if (ring->n > 65536 - points || (ring->n && !ring->pts)) {
                return pc_fail(err, PC_ERR_LIMIT, "cursor outline points");
            }
            points += ring->n;
            for (size_t j = 0; j < ring->n; j++) {
                float x = ring->pts[j][0], y = ring->pts[j][1];
                if (!isfinite(x) || !isfinite(y)) {
                    return pc_fail(err, PC_ERR_INVALID, "invalid cursor outline point");
                }
                e = fmaxf(e, hypotf(x, y));
            }
        }
        if (e <= 0) {
            return pc_fail(err, PC_ERR_INVALID, "empty cursor outline");
        }
    } else if (shape->kind == PC_UI_CURSOR_HAND) {
        e = 12.5f;
    } else {
        return pc_fail(err, PC_ERR_INVALID, "unknown cursor shape");
    }
    if (shape->center) { e = fmaxf(e, 3); }
    if (shape->badge != PC_UI_BADGE_NONE) { e = fmaxf(e, 16); }
    *out = e;
    return PC_OK;
}
static void splat(float *distances, size_t side, float hot, const float a[2], const float b[2], float reach)
{
    float low_x = fminf(a[0], b[0]) + hot - reach, high_x = fmaxf(a[0], b[0]) + hot + reach;
    float low_y = fminf(a[1], b[1]) + hot - reach, high_y = fmaxf(a[1], b[1]) + hot + reach;
    if (high_x < 0 || high_y < 0 || low_x >= (float)side || low_y >= (float)side) { return; }
    size_t x0 = (size_t)floorf(fmaxf(0, low_x)), x1 = (size_t)ceilf(fminf((float)side - 1, high_x));
    size_t y0 = (size_t)floorf(fmaxf(0, low_y)), y1 = (size_t)ceilf(fminf((float)side - 1, high_y));
    for (size_t y = y0; y <= y1; y++) {
        for (size_t x = x0; x <= x1; x++) {
            size_t index = y * side + x;
            distances[index] = fminf(distances[index], segment((float)x - hot, (float)y - hot, a, b));
        }
    }
}
pc_status pc_ui_cursor_rasterize(const pc_ui_cursor_shape *shape, float scale, uint16_t max_side,
                                  pc_ui_cursor_bitmap *out, pc_error *err)
{
    if (!shape || !out || !isfinite(scale) || scale <= 0 || max_side < 3 || max_side > 2048) {
        return pc_fail(err, PC_ERR_INVALID, "invalid cursor bitmap request");
    }
    float e;
    pc_status rc = extent(shape, &e, err);
    if (rc != PC_OK) { return rc; }
    double half = ceil(((double)e + 1.5) * scale + 1);
    if (!isfinite(half) || half > (max_side - 1) / 2) {
        return pc_fail(err, PC_ERR_LIMIT, "cursor exceeds native bitmap size");
    }
    pc_ui_cursor_bitmap next = {.hotspot = (uint16_t)half, .side = (uint16_t)((unsigned)half * 2 + 1)};
    size_t count = (size_t)next.side * next.side;
    next.rgba = pc_calloc_array(count, 4);
    float *distances = shape->kind == PC_UI_CURSOR_OUTLINE ? pc_malloc_array(count, sizeof(float)) : NULL;
    if (!next.rgba || (shape->kind == PC_UI_CURSOR_OUTLINE && !distances)) {
        pc_ui_cursor_bitmap_fini(&next);
        free(distances);
        return pc_fail(err, PC_ERR_NOMEM, "cursor bitmap");
    }
    if (distances) {
        for (size_t i = 0; i < count; i++) { distances[i] = INFINITY; }
        for (size_t r = 0; r < shape->count; r++) {
            const pc_paint_ring *ring = &shape->rings[r];
            if (ring->n < 2) { continue; }
            for (size_t i = 0; i < ring->n; i++) {
                const float *aa = ring->pts[i], *bb = ring->pts[(i + 1) % ring->n];
                float a[2] = {aa[0] * scale, aa[1] * scale}, b[2] = {bb[0] * scale, bb[1] * scale};
                splat(distances, next.side, next.hotspot, a, b, 1.5f * scale + 2);
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        float x = (float)(i % next.side) - next.hotspot, y = (float)(i / next.side) - next.hotspot;
        float d = shape->kind == PC_UI_CURSOR_CIRCLE ? fabsf(hypotf(x, y) - shape->radius * scale)
                    : shape->kind == PC_UI_CURSOR_CROSS ? cross_distance(x, y, shape->length * scale, shape->gap * scale)
                    : distances ? distances[i] : INFINITY;
        if (shape->center) { d = fminf(d, cross_distance(x, y, 3 * scale, 0)); }
        d = fminf(d, badge_distance(shape->badge, x / scale, y / scale) * scale);
        float fill = clamp(.5f * scale + .5f - d, 0, 1);
        float dark = clamp(1.5f * scale + .5f - d, 0, 1);
        if (shape->kind == PC_UI_CURSOR_HAND) {
            float edge, crease;
            hand_distance(shape->closed, x / scale, y / scale, &edge, &crease);
            float inside = clamp(.5f - edge * scale, 0, 1);
            float line = clamp(.45f * scale + .5f - crease * scale, 0, 1);
            fill = inside * (1 - line);
            dark = fmaxf(clamp(scale + .5f - edge * scale, 0, 1), inside * line);
        }
        fill *= 235.f / 255;
        dark *= 160.f / 255;
        float alpha = fill + dark * (1 - fill);
        uint8_t white = alpha > 0 ? (uint8_t)roundf(255 * fill / alpha) : 0;
        next.rgba[i * 4] = next.rgba[i * 4 + 1] = next.rgba[i * 4 + 2] = white;
        next.rgba[i * 4 + 3] = (uint8_t)roundf(255 * alpha);
    }
    free(distances);
    pc_ui_cursor_bitmap_fini(out);
    *out = next;
    return PC_OK;
}

bool pc_ui_is_selection_tool(pc_ui_tool t)
{
    return (t >= PC_TOOL_RECT_MARQUEE && t <= PC_TOOL_MAGIC_WAND) || t == PC_TOOL_QUICK_SELECTION ||
           t == PC_TOOL_OBJECT_SELECTION;
}
const char *pc_ui_selection_mode(pc_ui_tool t, uint8_t bar, unsigned mods, bool has_selection)
{
    bool alt = (mods & PC_UI_MOD_ALT) != 0, shift = (mods & PC_UI_MOD_SHIFT) != 0;
    const char *mode = t == PC_TOOL_QUICK_SELECTION ? (alt ? "subtract" : "add")
                      : t == PC_TOOL_OBJECT_SELECTION ? (alt ? "subtract" : shift ? "add" : "replace")
                      : alt && shift ? "intersect" : alt ? "subtract" : shift ? "add"
                      : bar == 1 ? "add" : bar == 2 ? "subtract" : bar >= 3 ? "intersect" : "replace";
    if (!has_selection && alt && (strcmp(mode, "subtract") == 0 || strcmp(mode, "intersect") == 0) &&
        strcmp(pc_ui_selection_mode(t, bar, 0, true), "replace") == 0) {
        return "replace";
    }
    return mode;
}
pc_ui_badge pc_ui_badge_from_mode(const char *mode)
{
    return mode && strcmp(mode, "add") == 0 ? PC_UI_BADGE_ADD
           : mode && strcmp(mode, "subtract") == 0 ? PC_UI_BADGE_SUBTRACT
           : mode && strcmp(mode, "intersect") == 0 ? PC_UI_BADGE_INTERSECT : PC_UI_BADGE_NONE;
}
