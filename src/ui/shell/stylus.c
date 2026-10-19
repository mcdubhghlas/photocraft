/* SDL pen events share the mouse tool route, retaining pressure and axes per physical pen. */
#include "canvas_view.h"
#include <math.h>
#include <string.h>
static void pen_mouse(pc_ui_shell_app *app, pc_ui_canvas_view *v, Uint32 type, float x, float y,
                      Uint64 timestamp)
{
    if (!isfinite(x) || !isfinite(y)) {
        return;
    }
    SDL_Event mouse;
    SDL_zero(mouse);
    mouse.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        mouse.motion.timestamp = timestamp;
        mouse.motion.x = x;
        mouse.motion.y = y;
        mouse.motion.state = v->pen_down ? SDL_BUTTON_LMASK : 0;
    } else {
        mouse.button.timestamp = timestamp;
        mouse.button.x = x;
        mouse.button.y = y;
        mouse.button.button = SDL_BUTTON_LEFT;
        mouse.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
    }
    v->pen.x = x;
    v->pen.y = y;
    v->pen_event = true;
    pc_ui_shell_input_event(app, &mouse);
    v->pen_event = false;
}
bool pc_ui_canvas_pen_event(pc_ui_shell_app *app, pc_ui_canvas_view *v, const SDL_Event *e)
{
    SDL_PenID id = 0;
    float x = 0, y = 0;
    switch (e->type) {
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
        id = e->ptouch.which;
        x = e->ptouch.x;
        y = e->ptouch.y;
        break;
    case SDL_EVENT_PEN_MOTION:
        id = e->pmotion.which;
        x = e->pmotion.x;
        y = e->pmotion.y;
        break;
    case SDL_EVENT_PEN_AXIS:
        id = e->paxis.which;
        x = e->paxis.x;
        y = e->paxis.y;
        break;
    case SDL_EVENT_PEN_PROXIMITY_OUT:
        if (v->pen_id == e->pproximity.which) {
            v->pen_down = false;
            pc_ui_tool_gesture_fini(&v->gesture);
            v->dragging = false;
            pen_mouse(app, v, SDL_EVENT_MOUSE_BUTTON_UP, v->pen.x, v->pen.y, e->common.timestamp);
        }
        return true;
    case SDL_EVENT_PEN_PROXIMITY_IN:
        id = e->pproximity.which;
        break;
    default:
        return false;
    }
    if (v->pen_down && id != v->pen_id) {
        return true;
    }
    SDL_Window *window = pc_ui_shell_app_sdl_window(app);
    if (e->paxis.windowID && window && e->paxis.windowID != SDL_GetWindowID(window)) {
        return true;
    }
    if (id != v->pen_id) {
        v->pen_id = id;
        v->pen = (pc_ui_tool_point){.pressure = 1, .wheel = 1};
    }
    if (e->type == SDL_EVENT_PEN_AXIS) {
        float value = e->paxis.value;
        if (!isfinite(value)) {
            return true;
        }
        switch (e->paxis.axis) {
        case SDL_PEN_AXIS_PRESSURE:
            v->pen.pressure = fminf(1, fmaxf(0, value));
            break;
        case SDL_PEN_AXIS_XTILT:
            v->pen.tilt_x = fminf(90, fmaxf(-90, value));
            break;
        case SDL_PEN_AXIS_YTILT:
            v->pen.tilt_y = fminf(90, fmaxf(-90, value));
            break;
        case SDL_PEN_AXIS_ROTATION:
            v->pen.rotation = fmodf(fmodf(value, 360) + 360, 360);
            break;
        case SDL_PEN_AXIS_SLIDER:
            v->pen.wheel = fminf(1, fmaxf(0, value));
            break;
        default:
            return true;
        }
        if (v->pen_down) {
            pen_mouse(app, v, SDL_EVENT_MOUSE_MOTION, x, y, e->common.timestamp);
        }
    } else if (e->type == SDL_EVENT_PEN_DOWN || e->type == SDL_EVENT_PEN_UP) {
        v->pen_eraser = e->ptouch.eraser;
        v->pen_down = e->type == SDL_EVENT_PEN_DOWN;
        if (v->pen_down) {
            memset(v->fingers, 0, sizeof v->fingers);
        }
        pen_mouse(app, v, v->pen_down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP, x, y,
                  e->common.timestamp);
    } else if (e->type == SDL_EVENT_PEN_MOTION) {
        pen_mouse(app, v, SDL_EVENT_MOUSE_MOTION, x, y, e->common.timestamp);
    }
    return true;
}
