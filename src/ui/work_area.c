/* Visible work area geometry: see work_area.h. Port of crates/ui-egui/src/work_area.rs. */
#include "ui/work_area.h"

#include <math.h>

/* Below this, the guess about where the monitor is went wrong: use the whole window. */
#define MIN_VISIBLE 160.0f

static float rect_w(pc_ui_rect r) { return r.x1 - r.x0; }
static float rect_h(pc_ui_rect r) { return r.y1 - r.y0; }

static pc_ui_rect rect_intersect(pc_ui_rect a, pc_ui_rect b)
{
    pc_ui_rect r;
    r.x0 = a.x0 > b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 > b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
    return r;
}

static bool rect_positive(pc_ui_rect r) { return rect_w(r) > 0.0f && rect_h(r) > 0.0f; }

static bool rect_finite(pc_ui_rect r)
{
    return isfinite(r.x0) && isfinite(r.y0) && isfinite(r.x1) && isfinite(r.y1);
}

/* `content` cut to `usable`, unless that leaves too little to be a sensible guess. */
static pc_ui_rect trusted(pc_ui_rect content, pc_ui_rect usable)
{
    pc_ui_rect visible = rect_intersect(content, usable);
    float min_h = MIN_VISIBLE < rect_h(content) ? MIN_VISIBLE : rect_h(content);
    float min_w = MIN_VISIBLE < rect_w(content) ? MIN_VISIBLE : rect_w(content);
    if (!rect_positive(visible) || rect_h(visible) < min_h || rect_w(visible) < min_w) {
        return content;
    }
    return visible;
}

pc_ui_rect pc_ui_visible_part(pc_ui_rect content, bool has_monitor, float mx, float my,
                              bool has_inner, pc_ui_rect inner, pc_ui_window_state state)
{
    if (!has_monitor) {
        return content;
    }
    if (state == PC_UI_WIN_FULLSCREEN || !isfinite(mx) || !isfinite(my) || mx < 1.0f || my < 1.0f) {
        return content;
    }
    /* A maximized window with no known position (Wayland) is taken to fit. */
    if (state == PC_UI_WIN_MAXIMIZED && !has_inner) {
        return content;
    }
    if (!has_inner) {
        /* Only the monitor size is known: at best the content starts at the monitor's top. */
        pc_ui_rect room = {content.x0, content.y0, content.x0 + mx, content.y0 + (my - PC_UI_TASKBAR)};
        return trusted(content, room);
    }
    if (!rect_finite(inner)) {
        return content;
    }
    /* Assume monitors tile the desktop by their size. */
    float cx = (inner.x0 + inner.x1) * 0.5f;
    float cy = (inner.y0 + inner.y1) * 0.5f;
    float ox = floorf(cx / mx) * mx;
    float oy = floorf(cy / my) * my;
    pc_ui_rect usable = {ox, oy, ox + mx, oy + my - PC_UI_TASKBAR};
    /* Desktop -> window coordinates. */
    float sx = content.x0 - inner.x0;
    float sy = content.y0 - inner.y0;
    pc_ui_rect local = {usable.x0 + sx, usable.y0 + sy, usable.x1 + sx, usable.y1 + sy};
    return trusted(content, local);
}

bool pc_ui_window_fits(float ow, float oh, float mw, float mh)
{
    return ow <= mw + 0.5f && oh <= mh - PC_UI_TASKBAR + 0.5f;
}
