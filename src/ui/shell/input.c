#include "layer_picker_ui.h"
#include "sampling_ui.h"
#include "ui/move_mods.h"
#include "artboard_ui.h"
#include "lasso_ui.h"
#include "slice_ui.h"
#include "transform_ui.h"
#include "context_ui.h"
#include "crop_ui.h"
#include "brush_picker_ui.h"
/* SDL3 -> Nuklear input translation and keyboard-shortcut dispatch for the frame loop. Kept apart
 * from main.c so the control test (which links the shell objects without main) still resolves
 * pc_ui_shell_app_run's reference to it. Never crashes on any event. */
#include <stdlib.h>
#include <string.h>

#include "shell_internal.h"
#include "pen_ui.h"
#include "jobs_ui.h"
#include "ui/enable_rules.h"
#include "base/num.h"
#include "ui/shortcut_dispatch.h"
#include "ui/shortcuts.h"

bool pc_ui_shell_keyboard_captured(pc_ui_shell_app *app)
{
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    if (!ctx) {
        return false;
    }
    for (const struct nk_window *window = ctx->begin; window; window = window->next) {
        const struct nk_window *current = window;
        for (size_t depth = 0; current && depth < 16; depth++) {
            if (!(current->flags & (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED)) && current->edit.active) {
                return true;
            }
            current = current->popup.active ? current->popup.win : NULL;
        }
    }
    return false;
}

static float to_ui(pc_ui_shell_app *a, float window_coord)
{
    SDL_Window *w = pc_ui_shell_app_sdl_window(a);
    float density = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
    float scale = pc_ui_shell_app_ui_scale(a);
    return window_coord * density / (scale > 0.0f ? scale : 1.0f);
}

/* Nuklear popups are separate child windows and are absent from ctx->begin. A popup's
 * first click outside it dismisses it; that click must not also start a canvas stroke. */
static bool popup_pointer_capture(pc_ui_shell_app *app, const struct nk_context *ctx, const SDL_Event *event)
{
    if (event->type != SDL_EVENT_MOUSE_MOTION && event->type != SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event->type != SDL_EVENT_MOUSE_BUTTON_UP && event->type != SDL_EVENT_MOUSE_WHEEL) {
        return false;
    }
    if (pc_ui_shell_canvas_pointer_active(app)) {
        return false;
    }
    float x = ctx->input.mouse.pos.x, y = ctx->input.mouse.pos.y;
    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        x = to_ui(app, event->motion.x);
        y = to_ui(app, event->motion.y);
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        x = to_ui(app, event->button.x);
        y = to_ui(app, event->button.y);
    }
    for (const struct nk_window *window = ctx->begin; window; window = window->next) {
        if (strcmp(window->name_string, "PhotoCraft") &&
            !(window->flags & (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED))) {
            bool inside = x >= window->bounds.x && y >= window->bounds.y &&
                          x < window->bounds.x + window->bounds.w && y < window->bounds.y + window->bounds.h;
            if (inside || (ctx->active == window && ctx->input.mouse.buttons[NK_BUTTON_LEFT].down)) {
                return true;
            }
        }
        const struct nk_window *current = window;
        for (size_t depth = 0; current && depth < 16; depth++) {
            if (current->popup.active && current->popup.win &&
                !(current->popup.win->flags & (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED))) {
                return true;
            }
            current = current->popup.active ? current->popup.win : NULL;
        }
    }
    return false;
}

/* Map an SDL keycode to a pc_ui_key (the subset shortcuts use). */
pc_ui_key pc_ui_shell_input_key(SDL_Keycode k)
{
    if (k >= SDLK_A && k <= SDLK_Z) {
        char name[2] = {(char)('A' + (k - SDLK_A)), '\0'};
        return pc_ui_key_from_name(name);
    }
    if (k >= SDLK_0 && k <= SDLK_9) {
        char name[2] = {(char)('0' + (k - SDLK_0)), '\0'};
        return pc_ui_key_from_name(name);
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) {
        char name[4];
        (void)snprintf(name, sizeof name, "F%d", 1 + (int)(k - SDLK_F1));
        return pc_ui_key_from_name(name);
    }
    switch (k) {
    case SDLK_RETURN:
        return PC_KEY_ENTER;
    case SDLK_TAB:
        return PC_KEY_TAB;
    case SDLK_SPACE:
        return PC_KEY_SPACE;
    case SDLK_BACKSPACE:
        return PC_KEY_BACKSPACE;
    case SDLK_DELETE:
        return PC_KEY_DELETE;
    case SDLK_ESCAPE:
        return PC_KEY_ESCAPE;
    case SDLK_LEFT:
        return PC_KEY_ARROWLEFT;
    case SDLK_RIGHT:
        return PC_KEY_ARROWRIGHT;
    case SDLK_UP:
        return PC_KEY_ARROWUP;
    case SDLK_DOWN:
        return PC_KEY_ARROWDOWN;
    case SDLK_MINUS:
        return PC_KEY_MINUS;
    case SDLK_EQUALS:
        return PC_KEY_EQUALS;
    case SDLK_GRAVE:
        return PC_KEY_BACKTICK;
    case SDLK_INSERT:
        return PC_KEY_INSERT;
    case SDLK_HOME:
        return PC_KEY_HOME;
    case SDLK_END:
        return PC_KEY_END;
    case SDLK_PAGEUP:
        return PC_KEY_PAGEUP;
    case SDLK_PAGEDOWN:
        return PC_KEY_PAGEDOWN;
    case SDLK_SEMICOLON:
        return PC_KEY_SEMICOLON;
    case SDLK_APOSTROPHE:
        return PC_KEY_QUOTE;
    case SDLK_BACKSLASH:
        return PC_KEY_BACKSLASH;
    case SDLK_SLASH:
        return PC_KEY_SLASH;
    case SDLK_COMMA:
        return PC_KEY_COMMA;
    case SDLK_PERIOD:
        return PC_KEY_PERIOD;
    case SDLK_LEFTBRACKET:
        return PC_KEY_OPENBRACKET;
    case SDLK_RIGHTBRACKET:
        return PC_KEY_CLOSEBRACKET;
    default:
        return PC_KEY_NONE;
    }
}

unsigned pc_ui_shell_input_mods(SDL_Keymod mod)
{
    unsigned m = 0;
    if (mod & SDL_KMOD_SHIFT) {
        m |= PC_UI_MOD_SHIFT;
    }
    if (mod & SDL_KMOD_ALT) {
        m |= PC_UI_MOD_ALT;
    }
#if defined(__APPLE__)
    if (mod & SDL_KMOD_GUI) {
        m |= PC_UI_MOD_COMMAND;
    }
    if (mod & SDL_KMOD_CTRL) {
        m |= PC_UI_MOD_CTRL;
    }
#else
    if (mod & SDL_KMOD_CTRL) {
        m |= PC_UI_MOD_COMMAND;
    }
#endif
    return m;
}

/* Resolve from this app's current preferences on each key-down. The old process-global table
 * kept another session's overrides and never noticed Edit > Keyboard Shortcuts changes. */
static void dispatch_shortcut(pc_ui_shell_app *a, SDL_Keycode code, SDL_Keymod mod)
{
    pc_ui_key key = pc_ui_shell_input_key(code);
    if (key == PC_KEY_NONE) {
        return;
    }
    pc_ui_binding *bindings = NULL;
    size_t count = 0;
    if (pc_ui_bindings(pc_ui_shell_app_session(a), &bindings, &count, NULL) != PC_OK) {
        return;
    }
    unsigned mods = pc_ui_shell_input_mods(mod);
    for (size_t i = 0; i < count; i++) {
        if (pc_ui_shortcut_key_matches(&bindings[i].sc, key, mods)) {
            if (pc_ui_command_disabled(pc_ui_shell_app_session(a), pc_ui_shell_app_state(a),
                                       bindings[i].id)) {
                break;
            }
            pc_json_value result = PC_JSON_VALUE_INIT;
            (void)pc_ui_shell_app_invoke(a, bindings[i].id, NULL, &result, NULL);
            pc_json_value_fini(&result);
            break;
        }
    }
    free(bindings);
}
void pc_ui_shell_input_cleanup(void)
{
    /* Tables are scoped to a single dispatch. Retained for the shell entrypoint's API. */
}

static bool close_popup_on_escape(struct nk_context *ctx, const SDL_Event *event)
{
    if (event->type != SDL_EVENT_KEY_DOWN || event->key.key != SDLK_ESCAPE || event->key.repeat)
        return false;
    for (struct nk_window *window = ctx->begin; window; window = window->next) {
        if (window->flags & (NK_WINDOW_HIDDEN | NK_WINDOW_CLOSED))
            continue;
        struct nk_window *parent = window;
        for (size_t depth = 0; depth < 16 && parent->popup.active && parent->popup.win; depth++) {
            struct nk_window *popup = parent->popup.win;
            if (popup->popup.active && popup->popup.win) {
                parent = popup;
                continue;
            }
            popup->flags |= NK_WINDOW_HIDDEN;
            parent->popup.active = false;
            parent->popup.type = NK_PANEL_NONE;
            parent->flags &= ~(nk_flags)NK_WINDOW_ROM;
            parent->flags |= NK_WINDOW_REMOVE_ROM;
            if (ctx->active == popup)
                ctx->active = parent;
            return true;
        }
    }
    return false;
}
void pc_ui_shell_input_event(pc_ui_shell_app *a, const SDL_Event *e)
{
    struct nk_context *c = pc_ui_shell_app_nk(a);
    if (c == NULL || e == NULL) {
        return;
    }
    pc_ui_shell_input_track(a, e);
    if ((e->type == SDL_EVENT_MOUSE_MOTION && e->motion.which == SDL_PEN_MOUSEID) ||
        ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
         e->button.which == SDL_PEN_MOUSEID)) {
        return;
    }
    if (close_popup_on_escape(c, e))
        return;
    if (pc_ui_shell_context_event(a, e))
        return;
    if (pc_ui_shell_layer_picker_event(a, e))
        return;
    if (pc_ui_shell_sampling_event(a, e))
        return;
    if (pc_ui_shell_artboard_event(a, e))
        return;
    if (pc_ui_shell_slice_event(a, e))
        return;
    if (pc_ui_shell_lasso_event(a, e))
        return;
    if (pc_ui_shell_crop_event(a, e))
        return;
    if (pc_ui_shell_transform_key(a, e))
        return;
    if (pc_ui_shell_brush_picker_event(a, e))
        return;
    if (pc_ui_shell_doc_size_event(a, e))
        return;
    if (pc_ui_shell_jobs_event(a, e))
        return;
    if (pc_ui_shell_pen_event(a, e)) {
        return;
    }
    if (pc_ui_shell_dock_cancel(a, e)) {
        return;
    }
    if (pc_ui_shell_dialog_key(a, e)) {
        return;
    }
    if (pc_ui_shell_menu_key(a, e)) {
        return;
    }
    if (pc_ui_shell_type_input(a, e)) {
        return;
    }
    if (pc_ui_shell_pen_input(a, e)) {
        return;
    }
    if ((e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_WINDOW_FOCUS_LOST) &&
        pc_ui_shell_layers_event(a, e))
        return;
    if (pc_ui_shell_opacity_event(a, e))
        return;
    if (e->type == SDL_EVENT_KEY_DOWN && !pc_ui_shell_keyboard_captured(a) && !pc_ui_shell_jobs_locked(a)) {
        pc_json_value out = PC_JSON_VALUE_INIT;
        pc_error err = {0};
        pc_status status =
            pc_ui_move_key(pc_ui_shell_app_session(a), pc_ui_shell_app_state(a),
                           pc_ui_shell_input_key(e->key.key), pc_ui_shell_input_modifiers(a), &out, &err);
        pc_json_value_fini(&out);
        if (status != PC_ERR_NOT_FOUND) {
            if (status != PC_OK)
                pc_ui_shell_report(a, "Move", err.msg, true);
            pc_ui_shell_app_changed(a);
            return;
        }
    }
    if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) {
        nk_input_key(c, NK_KEY_SHIFT, (e->key.mod & SDL_KMOD_SHIFT) != 0);
        nk_input_key(c, NK_KEY_CTRL, (e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0);
    } else if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        nk_input_key(c, NK_KEY_SHIFT, false);
        nk_input_key(c, NK_KEY_CTRL, false);
        for (size_t i = 0; i < NK_BUTTON_MAX; i++) {
            c->input.mouse.buttons[i].down = false;
            c->input.mouse.buttons[i].clicked = 0;
        }
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION) {
        nk_input_motion(c, pc_f32_to_i32(to_ui(a, e->motion.x)), pc_f32_to_i32(to_ui(a, e->motion.y)));
    } else if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        nk_input_motion(c, pc_f32_to_i32(to_ui(a, e->button.x)), pc_f32_to_i32(to_ui(a, e->button.y)));
    }
    if (!pc_ui_shell_menu_capture(a, e) && !popup_pointer_capture(a, c, e) &&
        !pc_ui_shell_dock_capture(a, e) &&
        (pc_ui_shell_layers_event(a, e) || pc_ui_shell_canvas_event(a, e))) {
        return;
    }
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        nk_input_motion(c, pc_f32_to_i32(to_ui(a, e->motion.x)), pc_f32_to_i32(to_ui(a, e->motion.y)));
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        enum nk_buttons b = e->button.button == SDL_BUTTON_RIGHT    ? NK_BUTTON_RIGHT
                            : e->button.button == SDL_BUTTON_MIDDLE ? NK_BUTTON_MIDDLE
                                                                    : NK_BUTTON_LEFT;
        int x = pc_f32_to_i32(to_ui(a, e->button.x)), y = pc_f32_to_i32(to_ui(a, e->button.y));
        if (b == NK_BUTTON_LEFT && e->button.clicks > 1 && e->button.down) {
            nk_input_button(c, NK_BUTTON_DOUBLE, x, y, true);
        }
        if (b == NK_BUTTON_LEFT && !e->button.down) {
            nk_input_button(c, NK_BUTTON_DOUBLE, x, y, false);
        }
        nk_input_button(c, b, x, y, e->button.down);
        if (b == NK_BUTTON_LEFT)
            nk_input_button(c, NK_BUTTON_DOUBLE, x, y, e->button.down && e->button.clicks >= 2);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        nk_input_scroll(c, nk_vec2(e->wheel.x, e->wheel.y));
        break;
    case SDL_EVENT_TEXT_INPUT: {
        const char *t = e->text.text;
        if (t == NULL) {
            break;
        }
        int len = (int)strlen(t), i = 0;
        while (i < len) {
            nk_rune r = 0;
            int n = nk_utf_decode(t + i, &r, len - i);
            if (n <= 0) {
                break;
            }
            nk_input_unicode(c, r);
            i += n;
        }
        break;
    }
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = e->key.down;
        bool ctrl = (e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
        switch (e->key.key) {
        case SDLK_A:
            nk_input_key(c, NK_KEY_TEXT_SELECT_ALL, down && ctrl);
            break;
        case SDLK_Z:
            nk_input_key(c, NK_KEY_TEXT_UNDO, down && ctrl && !(e->key.mod & SDL_KMOD_SHIFT));
            nk_input_key(c, NK_KEY_TEXT_REDO, down && ctrl && (e->key.mod & SDL_KMOD_SHIFT));
            break;
        case SDLK_Y:
            nk_input_key(c, NK_KEY_TEXT_REDO, down && ctrl);
            break;
        case SDLK_HOME:
            nk_input_key(c, NK_KEY_TEXT_LINE_START, down);
            break;
        case SDLK_END:
            nk_input_key(c, NK_KEY_TEXT_LINE_END, down);
            break;
        case SDLK_BACKSPACE:
            nk_input_key(c, NK_KEY_BACKSPACE, down);
            break;
        case SDLK_DELETE:
            nk_input_key(c, NK_KEY_DEL, down);
            break;
        case SDLK_RETURN:
            nk_input_key(c, NK_KEY_ENTER, down);
            break;
        case SDLK_TAB:
            nk_input_key(c, NK_KEY_TAB, down);
            break;
        case SDLK_LEFT:
            nk_input_key(c, NK_KEY_LEFT, down);
            break;
        case SDLK_RIGHT:
            nk_input_key(c, NK_KEY_RIGHT, down);
            break;
        case SDLK_UP:
            nk_input_key(c, NK_KEY_UP, down);
            break;
        case SDLK_DOWN:
            nk_input_key(c, NK_KEY_DOWN, down);
            break;
        case SDLK_C:
            nk_input_key(c, NK_KEY_COPY, down && ctrl);
            break;
        case SDLK_V:
            nk_input_key(c, NK_KEY_PASTE, down && ctrl);
            break;
        case SDLK_X:
            nk_input_key(c, NK_KEY_CUT, down && ctrl);
            break;
        default:
            break;
        }
        /* A shortcut fires on key down when no text widget is focused. */
        if (down && !pc_ui_shell_keyboard_captured(a)) {
            dispatch_shortcut(a, e->key.key, e->key.mod);
        }
        break;
    }
    default:
        break;
    }
}
