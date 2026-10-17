/* SDL3 -> Nuklear input translation and keyboard-shortcut dispatch for the frame loop. Kept apart
 * from main.c so the control test (which links the shell objects without main) still resolves
 * pc_ui_shell_app_run's reference to it. Never crashes on any event. */
#include <stdlib.h>
#include <string.h>

#include "shell_internal.h"
#include "base/num.h"
#include "ui/shortcut_dispatch.h"
#include "ui/shortcuts.h"

static float to_ui(pc_ui_shell_app *a, float window_coord)
{
    SDL_Window *w = pc_ui_shell_app_sdl_window(a);
    float density = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
    float scale = pc_ui_shell_app_ui_scale(a);
    return window_coord * density / (scale > 0.0f ? scale : 1.0f);
}

/* Map an SDL keycode to a pc_ui_key (the subset shortcuts use). */
static pc_ui_key map_key(SDL_Keycode k)
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
    case SDLK_RETURN: return PC_KEY_ENTER;
    case SDLK_TAB: return PC_KEY_TAB;
    case SDLK_SPACE: return PC_KEY_SPACE;
    case SDLK_BACKSPACE: return PC_KEY_BACKSPACE;
    case SDLK_DELETE: return PC_KEY_DELETE;
    case SDLK_ESCAPE: return PC_KEY_ESCAPE;
    case SDLK_LEFT: return PC_KEY_ARROWLEFT;
    case SDLK_RIGHT: return PC_KEY_ARROWRIGHT;
    case SDLK_UP: return PC_KEY_ARROWUP;
    case SDLK_DOWN: return PC_KEY_ARROWDOWN;
    case SDLK_MINUS: return PC_KEY_MINUS;
    case SDLK_EQUALS: return PC_KEY_EQUALS;
    case SDLK_LEFTBRACKET: return PC_KEY_OPENBRACKET;
    case SDLK_RIGHTBRACKET: return PC_KEY_CLOSEBRACKET;
    default: return PC_KEY_NONE;
    }
}

static unsigned map_mods(SDL_Keymod mod)
{
    unsigned m = 0;
    if (mod & SDL_KMOD_SHIFT) {
        m |= PC_UI_MOD_SHIFT;
    }
    if (mod & SDL_KMOD_ALT) {
        m |= PC_UI_MOD_ALT;
    }
    if (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) {
        m |= PC_UI_MOD_COMMAND; /* off the Mac, the primary modifier is Ctrl */
    }
    return m;
}

/* The binding table, built once from the app's session (overrides applied). */
static pc_ui_binding *g_bindings;
static size_t g_nbindings;
static bool g_bindings_tried;

static void dispatch_shortcut(pc_ui_shell_app *a, SDL_Keycode code, SDL_Keymod mod)
{
    if (!g_bindings_tried) {
        g_bindings_tried = true;
        pc_error e;
        if (pc_ui_bindings(pc_ui_shell_app_session(a), &g_bindings, &g_nbindings, &e) != PC_OK) {
            g_bindings = NULL;
            g_nbindings = 0;
        }
    }
    pc_ui_key key = map_key(code);
    if (key == PC_KEY_NONE) {
        return;
    }
    unsigned mods = map_mods(mod);
    for (size_t i = 0; i < g_nbindings; i++) {
        if (pc_ui_shortcut_key_matches(&g_bindings[i].sc, key, mods)) {
            pc_json_value r = PC_JSON_VALUE_INIT;
            pc_error e;
            (void)pc_ui_shell_app_invoke(a, g_bindings[i].id, NULL, &r, &e);
            pc_json_value_fini(&r);
            return;
        }
    }
}

void pc_ui_shell_input_cleanup(void)
{
    free(g_bindings);
    g_bindings = NULL;
    g_nbindings = 0;
    g_bindings_tried = false;
}

void pc_ui_shell_input_event(pc_ui_shell_app *a, const SDL_Event *e)
{
    struct nk_context *c = pc_ui_shell_app_nk(a);
    if (c == NULL || e == NULL) {
        return;
    }
    if (pc_ui_shell_canvas_event(a, e)) {
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
        nk_input_button(c, b, x, y, e->button.down);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        nk_input_scroll(c, nk_vec2(e->wheel.x, e->wheel.y));
        break;
    case SDL_EVENT_TEXT_INPUT: {
        const char *t = e->text.text;
        if (t == NULL) { break; }
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
        case SDLK_BACKSPACE: nk_input_key(c, NK_KEY_BACKSPACE, down); break;
        case SDLK_DELETE: nk_input_key(c, NK_KEY_DEL, down); break;
        case SDLK_RETURN: nk_input_key(c, NK_KEY_ENTER, down); break;
        case SDLK_TAB: nk_input_key(c, NK_KEY_TAB, down); break;
        case SDLK_LEFT: nk_input_key(c, NK_KEY_LEFT, down); break;
        case SDLK_RIGHT: nk_input_key(c, NK_KEY_RIGHT, down); break;
        case SDLK_UP: nk_input_key(c, NK_KEY_UP, down); break;
        case SDLK_DOWN: nk_input_key(c, NK_KEY_DOWN, down); break;
        case SDLK_C: nk_input_key(c, NK_KEY_COPY, down && ctrl); break;
        case SDLK_V: nk_input_key(c, NK_KEY_PASTE, down && ctrl); break;
        case SDLK_X: nk_input_key(c, NK_KEY_CUT, down && ctrl); break;
        default: break;
        }
        /* A shortcut fires on key down when no text widget is focused. */
        if (down && !nk_item_is_any_active(c)) {
            dispatch_shortcut(a, e->key.key, e->key.mod);
        }
        break;
    }
    default:
        break;
    }
}
