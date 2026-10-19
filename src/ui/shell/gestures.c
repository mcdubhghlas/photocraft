/* SDL touch and native pinch input. The two tracked contacts belong to one device; an extra
 * finger never steals a tracked slot. Touch cancellation/focus loss clears navigation state. */
#include "canvas_view.h"
#include <math.h>
#include "engine/prefs.h"

static bool pinch_enabled(pc_ui_shell_app *app)
{
    pc_json_value pref = PC_JSON_VALUE_INIT;
    bool enabled = true;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "enhancedControls.zoomWithTrackpadPinch",
                            &pref, NULL) == PC_OK) {
        yyjson_mut_val *root = pc_json_value_root(&pref);
        enabled = !yyjson_mut_is_bool(root) || yyjson_mut_get_bool(root);
    }
    pc_json_value_fini(&pref);
    return enabled;
}
static bool inside(const pc_ui_canvas_xform *xf, float x, float y)
{
    return isfinite(x) && isfinite(y) && x >= xf->x && y >= xf->y &&
           x < xf->x + xf->width && y < xf->y + xf->height;
}

bool pc_ui_canvas_touch_event(pc_ui_shell_app *app, pc_ui_canvas_view *v,
                               const pc_ui_canvas_xform *xf, const SDL_Event *e)
{
    pc_ui_view *camera = pc_ui_canvas_active_view(app);
    if (!camera) {
        return false;
    }
#if SDL_VERSION_ATLEAST(3, 4, 0)
    if (e->type == SDL_EVENT_PINCH_UPDATE || e->type == SDL_EVENT_PINCH_BEGIN ||
        e->type == SDL_EVENT_PINCH_END) {
        float x, y;
        (void)SDL_GetMouseState(&x, &y);
        float ppp = pc_ui_shell_app_ui_scale(app);
        float factor = SDL_GetWindowPixelDensity(pc_ui_shell_app_sdl_window(app)) / ppp;
        x *= factor;
        y *= factor;
        if (!inside(xf, x, y) || pc_ui_shell_keyboard_captured(app)) {
            return false;
        }
        if (e->type == SDL_EVENT_PINCH_UPDATE && isfinite(e->pinch.scale) && e->pinch.scale > 0 &&
            pinch_enabled(app)) {
            (void)pc_ui_canvas_anchor(camera, xf, x, y, camera->zoom * e->pinch.scale, ppp);
        }
        return true;
    }
#endif
    if (e->type != SDL_EVENT_FINGER_DOWN && e->type != SDL_EVENT_FINGER_MOTION &&
        e->type != SDL_EVENT_FINGER_UP && e->type != SDL_EVENT_FINGER_CANCELED) {
        return false;
    }
    if(v->pen_down) { return true; }
    SDL_Window *window = pc_ui_shell_app_sdl_window(app);
    if (e->tfinger.windowID && e->tfinger.windowID != SDL_GetWindowID(window)) {
        return false;
    }
    float w, h;
    pc_ui_shell_app_window_size(app, &w, &h, NULL);
    float x = e->tfinger.x * w, y = e->tfinger.y * h;
    size_t slot = 2;
    for (size_t i = 0; i < 2; i++) {
        if (v->fingers[i].down && v->fingers[i].device == e->tfinger.touchID &&
            v->fingers[i].id == e->tfinger.fingerID) {
            slot = i;
            break;
        }
    }
    if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) {
        if (slot < 2) {
            v->fingers[slot].down = false;
            return true;
        }
        return false;
    }
    if (!isfinite(x) || !isfinite(y)) {
        return slot < 2;
    }
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (pc_ui_shell_dialog_at(app, x, y)) { return false; }
        if (slot < 2 || !inside(xf, x, y) || pc_ui_shell_keyboard_captured(app)) {
            return slot < 2;
        }
        for (size_t i = 0; i < 2; i++) {
            if (v->fingers[i].down && v->fingers[i].device != e->tfinger.touchID) {
                return false;
            }
        }
        slot = v->fingers[0].down ? 1 : 0;
        if (v->fingers[slot].down) {
            return false;
        }
        v->fingers[slot].down = true;
        v->fingers[slot].device = e->tfinger.touchID;
        v->fingers[slot].id = e->tfinger.fingerID;
        v->fingers[slot].point[0] = x;
        v->fingers[slot].point[1] = y;
        if (v->fingers[0].down && v->fingers[1].down) {
            v->dragging = v->scroll_drag.active = v->guide_drag.active = false;
            pc_ui_tool_gesture_fini(&v->gesture);
        }
        return true;
    }
    if (slot == 2) {
        return false;
    }
    float old[2] = {(v->fingers[0].point[0] + v->fingers[1].point[0]) * .5f,
                     (v->fingers[0].point[1] + v->fingers[1].point[1]) * .5f};
    float dx = v->fingers[1].point[0] - v->fingers[0].point[0];
    float dy = v->fingers[1].point[1] - v->fingers[0].point[1];
    float distance = hypotf(dx, dy), angle = atan2f(dy, dx);
    v->fingers[slot].point[0] = x;
    v->fingers[slot].point[1] = y;
    if (v->fingers[0].down && v->fingers[1].down) {
        float next[2] = {(v->fingers[0].point[0] + v->fingers[1].point[0]) * .5f,
                         (v->fingers[0].point[1] + v->fingers[1].point[1]) * .5f};
        dx = v->fingers[1].point[0] - v->fingers[0].point[0];
        dy = v->fingers[1].point[1] - v->fingers[0].point[1];
        float zoom = distance > 4 && hypotf(dx, dy) > 4 && pinch_enabled(app)
                         ? hypotf(dx, dy) / distance : 1;
        float rotation = distance > 4 && hypotf(dx, dy) > 4
                             ? pc_ui_rotation_wrap((atan2f(dy, dx) - angle) * 57.29577951308232f) : 0;
        (void)pc_ui_canvas_gesture(camera, xf, old, next, zoom, rotation, pc_ui_shell_app_ui_scale(app));
    }
    return true;
}
