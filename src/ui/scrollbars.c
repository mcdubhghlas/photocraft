#include "canvas.h"
#include <math.h>

bool pc_ui_scroll_span_make(float length, float center, float size, float zoom, pc_ui_scroll_span *out)
{
    if (!out || !isfinite(zoom) || zoom <= 0.f || !isfinite(size) || size <= 0.f) {
        return false;
    }
    float half = size / zoom / 2.f;
    pc_ui_scroll_span s = {length, center - half, center + half};
    if (!isfinite(length) || length <= 0.f || !isfinite(s.first) || !isfinite(s.last) || s.last <= s.first) {
        return false;
    }
    *out = s;
    return true;
}

void pc_ui_scroll_extent(pc_ui_scroll_span s, bool overscroll, float out[2])
{
    out[0] = fminf(s.first, 0.f);
    out[1] = fmaxf(s.last, s.length);
    if (overscroll) {
        float visible = s.last - s.first;
        out[0] = fminf(out[0], s.length - visible);
        out[1] = fmaxf(out[1], visible);
    }
}

void pc_ui_scroll_thumb(pc_ui_scroll_span s, const float e[2], float track, float out[2])
{
    out[0] = 0;
    out[1] = 0;
    if (!isfinite(track) || track <= 0.f || !isfinite(e[0]) || !isfinite(e[1]) ||
        !isfinite(s.last - s.first) || s.last <= s.first || e[1] < e[0]) {
        return;
    }
    float total = fmaxf(e[1] - e[0], 1e-6f), visible = fminf(s.last - s.first, total);
    float length = fminf(track, fmaxf(fminf(24.f, track), visible / total * track));
    float travel = fmaxf(track - length, 0.f), room = total - visible;
    out[0] = room > 1e-6f ? fminf(1.f, fmaxf(0.f, (s.first - e[0]) / room)) * travel : 0.f;
    out[1] = length;
}

float pc_ui_scroll_doc_per_point(pc_ui_scroll_span s, const float e[2], float track)
{
    float thumb[2];
    pc_ui_scroll_thumb(s, e, track, thumb);
    float travel = track - thumb[1], room = e[1] - e[0] - (s.last - s.first);
    return isfinite(travel) && isfinite(room) && travel > 1e-3f && room > 0.f ? room / travel : 0.f;
}

float pc_ui_scroll_clamp(float center, float length, float size, float zoom)
{
    if (!isfinite(zoom) || zoom <= 0.f || !isfinite(length) || length <= 0.f || !isfinite(size) ||
        size <= 0.f) {
        return center;
    }
    if (!isfinite(center)) {
        return length / 2.f;
    }
    float half = size / zoom / 2.f, other = length - half;
    return fminf(fmaxf(half, other), fmaxf(fminf(half, other), center));
}

bool pc_ui_scroll_clamp_view(pc_ui_view *v, float width, float height, float ppp, float aspect)
{
    if (!v || !v->doc_size[0] || !v->doc_size[1] || !isfinite(width) || width <= 0.f || !isfinite(height) ||
        height <= 0.f) {
        return false;
    }
    if (!isfinite(ppp) || ppp <= 0.f) {
        ppp = 1.f;
    }
    if (!isfinite(aspect) || aspect <= 0.f) {
        aspect = 1.f;
    }
    float zoom = v->zoom / ppp, cx = v->center[0], cy = v->center[1];
    if (!isfinite(zoom) || zoom <= 0.f) {
        return false;
    }
    float w = (float)v->doc_size[0], h = (float)v->doc_size[1];
    if (fabsf(v->rotation) < 1e-8f) {
        cx = pc_ui_scroll_clamp(cx, w, width / aspect, zoom);
        cy = pc_ui_scroll_clamp(cy, h, height, zoom);
    } else {
        pc_ui_canvas_xform xf = {.width = width,
                                 .height = height,
                                 .zoom = zoom,
                                 .aspect = aspect,
                                 .rotation = v->rotation,
                                 .center = {isfinite(cx) ? cx : w / 2.f, isfinite(cy) ? cy : h / 2.f}};
        float low[2] = {0}, high[2] = {0};
        for (size_t i = 0; i < 4; i++) {
            float point[2];
            if (pc_ui_canvas_to_screen(&xf, (i & 1) ? w : 0.f, (i & 2) ? h : 0.f, point) != PC_OK) {
                return false;
            }
            if (!i) {
                low[0] = high[0] = point[0];
                low[1] = high[1] = point[1];
            } else {
                for (size_t a = 0; a < 2; a++) {
                    low[a] = fminf(low[a], point[a]);
                    high[a] = fmaxf(high[a], point[a]);
                }
            }
        }
        float offset[2] = {width / 2.f - low[0], height / 2.f - low[1]};
        float dx = pc_ui_scroll_clamp(offset[0], high[0] - low[0], width, 1.f) - offset[0];
        float dy = pc_ui_scroll_clamp(offset[1], high[1] - low[1], height, 1.f) - offset[1];
        pc_ui_view next = *v;
        next.center[0] = xf.center[0];
        next.center[1] = xf.center[1];
        if (pc_ui_canvas_pan(&next, &xf, -dx, -dy) != PC_OK) {
            return false;
        }
        cx = next.center[0];
        cy = next.center[1];
    }
    bool changed = !isfinite(v->center[0]) || !isfinite(v->center[1]) || fabsf(cx - v->center[0]) > 1e-3f ||
                   fabsf(cy - v->center[1]) > 1e-3f;
    if (changed && isfinite(cx) && isfinite(cy)) {
        v->center[0] = cx;
        v->center[1] = cy;
        return true;
    }
    return false;
}

bool pc_ui_scrollbars_layout(const pc_ui_canvas_xform *xf, const uint32_t size[2], bool overscroll,
                             pc_ui_scrollbar out[2])
{
    if (!xf || !size || !out || !isfinite(xf->x) || !isfinite(xf->y) || !isfinite(xf->width) ||
        !isfinite(xf->height) || xf->width <= 12.f || xf->height <= 12.f) {
        return false;
    }
    float aspect = isfinite(xf->aspect) && xf->aspect > 0.f ? xf->aspect : 1.f;
    pc_ui_scrollbar bars[2] = {0};
    for (unsigned axis = 0; axis < 2; axis++) {
        pc_ui_scrollbar *b = &bars[axis];
        b->axis = axis;
        b->flipped = !axis && xf->flip;
        b->center = b->flipped ? (float)size[axis] - xf->center[axis] : xf->center[axis];
        float visible = axis ? xf->height : xf->width / aspect;
        b->shown = pc_ui_scroll_span_make((float)size[axis], b->center, visible, xf->zoom, &b->span);
    }
    float right = xf->x + xf->width - (bars[1].shown ? 12.f : 0.f);
    float bottom = xf->y + xf->height - (bars[0].shown ? 12.f : 0.f);
    bars[0].rect[0] = xf->x;
    bars[0].rect[1] = bottom;
    bars[0].rect[2] = right - xf->x;
    bars[0].rect[3] = 12.f;
    bars[1].rect[0] = right;
    bars[1].rect[1] = xf->y;
    bars[1].rect[2] = 12.f;
    bars[1].rect[3] = bottom - xf->y;
    for (size_t axis = 0; axis < 2; axis++) {
        pc_ui_scrollbar *b = &bars[axis];
        if (b->shown) {
            pc_ui_scroll_extent(b->span, overscroll, b->extent);
            pc_ui_scroll_thumb(b->span, b->extent, b->rect[axis ? 3 : 2], b->thumb);
        }
    }
    out[0] = bars[0];
    out[1] = bars[1];
    return true;
}

bool pc_ui_scrollbar_press(const pc_ui_scrollbar bars[2], float x, float y, pc_ui_scroll_drag *drag)
{
    if (!bars || !drag || !isfinite(x) || !isfinite(y)) {
        return false;
    }
    for (size_t i = 0; i < 2; i++) {
        const pc_ui_scrollbar *b = &bars[i];
        const float *r = b->rect;
        if (!b->shown || x < r[0] || y < r[1] || x >= r[0] + r[2] || y >= r[1] + r[3]) {
            continue;
        }
        float along = b->axis ? y - r[1] : x - r[0];
        *drag = (pc_ui_scroll_drag){
            .bar = *b,
            .origin = along,
            .press_cross = b->axis ? x : y,
            .active = true,
            .thumb = along >= b->thumb[0] && along <= b->thumb[0] + b->thumb[1],
            .scale = pc_ui_scroll_doc_per_point(b->span, b->extent, b->rect[b->axis ? 3 : 2])};
        return true;
    }
    return false;
}

bool pc_ui_scrollbar_motion(pc_ui_scroll_drag *d, float x, float y, bool release, float *out)
{
    if (!d || !d->active || !out) {
        return false;
    }
    if (!isfinite(x) || !isfinite(y)) {
        if (release) {
            d->active = false;
        }
        return false;
    }
    const pc_ui_scrollbar *b = &d->bar;
    float along = b->axis ? y - b->rect[1] : x - b->rect[0], cross = b->axis ? x : y;
    if (fabsf(along - d->origin) + fabsf(cross - d->press_cross) > 2.f) {
        d->moved = true;
    }
    float center = b->center;
    bool changed = false;
    if (d->thumb) {
        center += (along - d->origin) * d->scale;
        changed = true;
    } else if (release && !d->moved && x >= b->rect[0] && y >= b->rect[1] && x < b->rect[0] + b->rect[2] &&
               y < b->rect[1] + b->rect[3]) {
        float page = b->span.last - b->span.first;
        if (along < b->thumb[0]) {
            center = fmaxf(center - page, b->extent[0] + page / 2.f);
        } else if (along > b->thumb[0] + b->thumb[1]) {
            center = fminf(center + page, b->extent[1] - page / 2.f);
        }
        changed = true;
    }
    if (release) {
        d->active = false;
    }
    if (!changed || !isfinite(center)) {
        return false;
    }
    center = b->flipped ? b->span.length - center : center;
    if (!isfinite(center)) {
        return false;
    }
    *out = center;
    return true;
}
