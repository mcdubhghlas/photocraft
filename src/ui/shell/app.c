#include "layer_picker_ui.h"
#include "sampling_ui.h"
#include "ui/move_mods.h"
#include "engine/floating.h"
#include "artboard_ui.h"
#include "transform_ui.h"
#include "context_ui.h"
#include "crop_ui.h"
#include "snap_ui.h"
#include "lasso_ui.h"
#include "slice_ui.h"
#include "brush_picker_ui.h"
#include "rasterize_ui.h"
#include "command_form.h"
#include "variables_ui.h"
/* PhotocraftApp shell: see app.h. Reimplementation of the egui app root (crates/ui-egui/src/lib.rs)
 * against SDL3 + OpenGL 3.3 core + Nuklear + FreeType, built on the B3 spike's helpers. */
#include "app.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "engine/prefs.h"
#include "engine/file_ui.h"
#include "engine/channel_display.h"
#include "canvas_view.h"
#include "panels.h"
#include "chrome.h"
#include "icon_cache.h"
#include "native_menus.h"
#include "docking.h"
#include "discard.h"
#include <math.h>
#include "dialogs.h"
#include "filter_ui.h"
#include "layer_style_ui.h"
#include "ui/filter_dialog.h"
#include "preferences.h"
#include "codecs/codecs.h"
#include "codecs/image.h"
#include "compose/compose.h"
#include "font.h" /* spike helper (-Isrc/ui/spike) */
#include "gl33.h" /* spike helper */
#include "photocraft_nuklear.h"
#include "render.h" /* spike helper */
#include "shell_internal.h"
#include "workspace_ui.h"
#include "jobs_ui.h"
#include "system_info.h"
#include "ui/dock.h"
#include "ui/menus.h"
#include "ui/menu_state.h"
#include "ui/recent.h"
#include "ui/notices.h"
#include "ui/tab_strip.h"
#include "ui/widgets.h"
#include "ui/i18n.h"
#include "ui/type_tool.h"
#include "ui/pen_tool.h"
#include "ui/direct_tool.h"
#include "ui/opacity_keys.h"
#include "type_controls.h"
#include "type_panels_ui.h"
#include "image_cache.h"
#include "brush_panel_ui.h"
#include "layer_interaction.h"
#include "timeline_ui.h"

enum { DEFAULT_W = 1280, DEFAULT_H = 800, MENU_BAR_H = 26, DOCK_W = 300, STRIP_H = 28 };

struct pc_ui_shell_app {
    pc_engine_session *session; /* owned */
    pc_ui_state ui;             /* U1 UiState */
    pc_ui_tokens tokens;        /* tokens for ui.theme */
    pc_ui_native_menus menus;
    pc_ui_type_pointer type_pointer;
    pc_ui_type_controls type_controls;
    pc_ui_type_panels_runtime type_panels_runtime;
    pc_ui_image_cache image_cache;
    pc_ui_docking docking;
    pc_ui_snap snap;
    pc_ui_canvas_view canvas; /* U4: camera, revision cache and display texture */
    pc_ui_panel_runtime panel_runtime;
    pc_ui_icon_cache icon_cache;
    pc_ui_dialog_runtime dialog_runtime;
    pc_ui_jobs jobs;
    pc_ui_opacity_pending opacity_keys;
    pc_ui_brush_panel brush_panel;
    pc_ui_brush_picker_runtime brush_picker;
    pc_ui_transform_runtime transform;
    pc_ui_secondary_runtime secondary;
    pc_ui_crop_runtime crop;
    pc_ui_artboard_layout artboard_layout;
    pc_ui_sampling_tool sampling;
    pc_ui_notices notices; /* U3: lower-right notice stack */
    bool notices_hovered;
    uint64_t frame;
    uint64_t applied_prefs;
    SDL_SystemTheme system_theme;
    const pc_i18n_lang *language;
    float win_w, win_h; /* UI units (logical) */

    /* Cached menu model, rebuilt when menu_dirty (after invokes / theme / doc changes). */
    pc_ui_menu_item *items;
    size_t nitems;
    bool menu_dirty;

    /* GUI stack (NULL / 0 until a window is opened). */
    bool quit_requested;
    bool have_window;
    SDL_Window *win;
    SDL_GLContext glc;
    gl33 gl;
    int gl_ready;
    ui_font font;
    int font_ready;
    ui_renderer rr;
    int rr_ready;
    struct nk_context ctx;
    int ctx_ready;
    float ui_scale; /* framebuffer pixels per UI unit */
    char *font_path;
    float forced_scale;
};

bool pc_ui_shell_opacity_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        app->opacity_keys.pending = false;
        return false;
    }
    if (event->type != SDL_EVENT_KEY_DOWN || app->ui.n_dialogs || app->menus.levels ||
        pc_ui_shell_keyboard_captured(app) || pc_ui_shell_jobs_locked(app) ||
        (event->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI | SDL_KMOD_ALT)))
        return false;
    unsigned digit;
    if (event->key.scancode >= SDL_SCANCODE_1 && event->key.scancode <= SDL_SCANCODE_9)
        digit = (unsigned)(event->key.scancode - SDL_SCANCODE_1) + 1;
    else if (event->key.scancode == SDL_SCANCODE_0)
        digit = 0;
    else if (event->key.key >= SDLK_0 && event->key.key <= SDLK_9)
        digit = (unsigned)(event->key.key - SDLK_0);
    else
        return false;
    pc_ui_opacity_change change =
        pc_ui_opacity_press(&app->opacity_keys, app->ui.tool, digit, (event->key.mod & SDL_KMOD_SHIFT) != 0,
                            event->key.repeat, SDL_GetTicks());
    if (change.target == PC_UI_OPACITY_NONE)
        return true;
    if (change.target == PC_UI_OPACITY_TOOL) {
        app->ui.tool_options.fill_opacity = change.value * 100;
        return true;
    }
    const pc_engine_doc_state *state = pc_engine_session_active(app->session);
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    if ((change.target == PC_UI_OPACITY_LAYER || change.target == PC_UI_OPACITY_FILL) &&
        (!state || !state->has_active_layer || pc_document_is_background(doc, state->active_layer)))
        return true;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = d ? yyjson_mut_obj(d) : NULL;
    bool brush = change.target == PC_UI_OPACITY_BRUSH || change.target == PC_UI_OPACITY_FLOW;
    yyjson_mut_val *fields = brush && d ? yyjson_mut_obj(d) : p;
    const char *key = change.target == PC_UI_OPACITY_FLOW   ? "flow"
                      : change.target == PC_UI_OPACITY_FILL ? "fill"
                                                            : "opacity";
    bool ok = p && fields && yyjson_mut_obj_add_real(d, fields, key, change.value);
    if (brush)
        ok = ok && yyjson_mut_obj_add_val(d, p, "brush", fields);
    else
        ok = ok && yyjson_mut_obj_add_uint(d, p, "layer", state->active_layer);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_buf json = PC_BUF_INIT;
    if (ok && pc_json_write(p, 0, &json, NULL) == PC_OK) {
        yyjson_doc *immutable = NULL;
        if (pc_json_parse(pc_buf_cstr(&json), json.len, &immutable, NULL) == PC_OK) {
            (void)pc_ui_shell_app_invoke(app, brush ? "tools.setBrush" : "layer.setProps",
                                         yyjson_doc_get_root(immutable), &out, NULL);
            yyjson_doc_free(immutable);
        }
    }
    pc_buf_fini(&json);
    pc_json_value_fini(&out);
    yyjson_mut_doc_free(d);
    return true;
}

/* ---- lifecycle ---- */

static pc_status replay_view(void *ctx, const char *id, yyjson_val *params, pc_json_value *out, pc_error *err)
{
    return pc_ui_shell_app_invoke(ctx, id, params, out, err);
}

pc_status pc_ui_shell_app_new(pc_ui_shell_app **out, pc_error *err)
{
    pc_ui_shell_app *a = pc_calloc_array(1, sizeof *a);
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (pc_engine_session_new(&a->session, err) != PC_OK) {
        free(a);
        return err ? err->code : PC_ERR_NOMEM;
    }
    pc_ui_state_init(&a->ui);
    pc_ui_notices_init(&a->notices);
    a->tokens = pc_ui_tokens_for_kind(a->ui.theme);
    pc_ui_menu_nav_reset(&a->menus.nav);
    a->win_w = (float)DEFAULT_W;
    a->win_h = (float)DEFAULT_H;
    a->ui_scale = 1.0f;
    a->menu_dirty = true;
    a->applied_prefs = UINT64_MAX;
    pc_engine_session_set_shell_dispatch(a->session, replay_view, a);
    *out = a;
    return PC_OK;
}

static void free_menu_items(pc_ui_shell_app *a)
{
    pc_ui_menu_items_free(a->items, a->nitems);
    a->items = NULL;
    a->nitems = 0;
}

void pc_ui_shell_app_free(pc_ui_shell_app *a)
{
    if (a == NULL) {
        return;
    }
    free_menu_items(a);
    pc_ui_shell_jobs_fini(&a->jobs);
    pc_ui_type_pointer_fini(&a->type_pointer);
    pc_ui_type_controls_fini(&a->type_controls);
    pc_ui_type_panels_runtime_fini(&a->type_panels_runtime);
    pc_ui_brush_panel_fini(&a->brush_panel);
    pc_ui_shell_transform_fini(&a->transform);
    pc_ui_artboard_layout_fini(&a->artboard_layout);
    pc_ui_snap_fini(&a->snap);
    pc_ui_image_cache_fini(&a->image_cache, a->gl_ready ? &a->gl : NULL);
    pc_ui_icon_cache_fini(&a->icon_cache, a->gl_ready ? &a->gl : NULL);
    pc_ui_command_queue_fini(&a->panel_runtime.commands);
    pc_ui_layer_interaction_fini(&a->panel_runtime);
    pc_ui_shell_dialogs_fini(&a->dialog_runtime);
    pc_ui_canvas_view_fini(&a->canvas, a->gl_ready ? &a->gl : NULL);
    if (a->ctx_ready) {
        nk_free(&a->ctx);
    }
    if (a->rr_ready) {
        ui_renderer_fini(&a->rr);
    }
    if (a->font_ready) {
        ui_font_fini(&a->font);
    }
    if (a->glc != NULL) {
        SDL_GL_DestroyContext(a->glc);
    }
    if (a->win != NULL) {
        SDL_DestroyWindow(a->win);
    }
    free(a->font_path);
    pc_ui_notices_fini(&a->notices);
    pc_ui_state_fini(&a->ui);
    pc_engine_session_free(a->session);
    free(a);
}

/* ---- accessors ---- */

pc_engine_session *pc_ui_shell_app_session(pc_ui_shell_app *a)
{
    return a->session;
}
pc_ui_state *pc_ui_shell_app_state(pc_ui_shell_app *a)
{
    return &a->ui;
}
const pc_ui_tokens *pc_ui_shell_app_tokens(const pc_ui_shell_app *a)
{
    return &a->tokens;
}
uint64_t pc_ui_shell_app_frame(const pc_ui_shell_app *a)
{
    return a->frame;
}
pc_ui_menu_nav *pc_ui_shell_app_nav(pc_ui_shell_app *a)
{
    return &a->menus.nav;
}

void pc_ui_shell_app_window_size(const pc_ui_shell_app *a, float *w, float *h, float *ppp)
{
    if (w) {
        *w = a->win_w;
    }
    if (h) {
        *h = a->win_h;
    }
    if (ppp) {
        *ppp = a->ui_scale > 0.0f ? a->ui_scale : 1.0f;
    }
}

void pc_ui_shell_app_set_window_size(pc_ui_shell_app *a, float w, float h)
{
    if (w > 0.0f) {
        a->win_w = w;
    }
    if (h > 0.0f) {
        a->win_h = h;
    }
}

void pc_ui_shell_app_apply_theme(pc_ui_shell_app *a)
{
    a->tokens = pc_ui_tokens_for_kind(a->ui.theme);
    if (a->ctx_ready) {
        pc_ui_apply_nk_style(&a->ctx, &a->tokens);
    }
}

bool pc_ui_shell_icon_image(pc_ui_shell_app *a, const char *name, float side, struct nk_image *out)
{
    float pixels = ceilf(side * a->ui_scale);
    if (pixels < 8) {
        pixels = 8;
    }
    if (pixels > 256) {
        pixels = 256;
    }
    return pc_ui_icon_cache_get(&a->icon_cache, &a->gl, name, (uint16_t)pixels, a->frame, out, NULL) == PC_OK;
}
void pc_ui_shell_queue(pc_ui_shell_app *a, const char *command, const yyjson_mut_val *params)
{
    pc_error err;
    if (pc_ui_command_queue_push(&a->panel_runtime.commands, command, params, &err) != PC_OK) {
        char *copy = strdup(err.msg);
        if (copy) {
            free(a->ui.status);
            a->ui.status = copy;
        }
        a->ui.status_error = true;
    }
}
void pc_ui_shell_widget_mapping(pc_ui_shell_app *a, const char *command, struct nk_rect r)
{
    if (a->panel_runtime.nhits >= 1024) {
        return;
    }
    pc_ui_panel_hit *hit = &a->panel_runtime.hits[a->panel_runtime.nhits++];
    (void)snprintf(hit->command, sizeof hit->command, "%s", command);
    hit->layer = 0;
    hit->rect[0] = r.x;
    hit->rect[1] = r.y;
    hit->rect[2] = r.w;
    hit->rect[3] = r.h;
}

void pc_ui_shell_request_exit(pc_ui_shell_app *a)
{
    if (a) {
        a->quit_requested = true;
    }
}
bool pc_ui_shell_exit_requested(const pc_ui_shell_app *a)
{
    return a && a->quit_requested;
}
void pc_ui_shell_sync_targets(pc_ui_shell_app *app)
{
    pc_ui_layer_picker_prune(app->session, &app->ui);
    const pc_engine_doc_state *state = pc_engine_session_active(app->session);
    const pc_document *doc = state ? pc_doc_arc_get(state->doc) : NULL;
    const pc_layer *layer =
        state && state->has_active_layer ? pc_document_layer(doc, state->active_layer) : NULL;
    if (!layer || !layer->has_mask)
        app->ui.mask_target = false;
    if (!layer || !layer->has_vector_mask)
        app->ui.vector_mask_target = false;
    bool quick = false;
    size_t alpha = 0;
    bool separate = pc_engine_targeted_channel(app->session, &quick, &alpha);
    bool mask = layer && layer->has_mask && !doc->has_quick_mask && !separate &&
                pc_engine_targeted_color(app->session) == SIZE_MAX &&
                (app->ui.mask_target || pc_engine_targeted_mask(app->session) ||
                 (layer->content.kind == PC_LAYER_ADJUSTMENT && !app->ui.vector_mask_target &&
                  !pc_engine_targeted_vector_mask(app->session)));
    (void)pc_engine_session_target_mask(app->session, mask);
}

void pc_ui_shell_app_changed(pc_ui_shell_app *app)
{
    if (app) {
        app->menu_dirty = true;
    }
}

void pc_ui_shell_apply_preferences(pc_ui_shell_app *a)
{
    uint64_t revision = pc_engine_prefs_revision(a->session);
    SDL_SystemTheme system =
        (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) ? SDL_GetSystemTheme() : SDL_SYSTEM_THEME_UNKNOWN;
    if (revision != a->applied_prefs || system != a->system_theme) {
        pc_ui_shell_brush_picker_restore(a, &a->brush_picker);
        pc_json_value value = PC_JSON_VALUE_INIT;
        if (pc_engine_prefs_get(a->session, "interface", &value, NULL) == PC_OK) {
            yyjson_mut_val *root = pc_json_value_root(&value);
            const char *appearance = yyjson_mut_get_str(yyjson_mut_obj_get(root, "appearanceMode"));
            bool light = appearance && (!strcmp(appearance, "light") ||
                                        (!strcmp(appearance, "auto") && system == SDL_SYSTEM_THEME_LIGHT));
            const char *theme =
                yyjson_mut_get_str(yyjson_mut_obj_get(root, light ? "lightTheme" : "darkTheme"));
            pc_ui_theme_kind kind;
            if (pc_ui_theme_kind_from_name(theme, &kind) && kind != a->ui.theme) {
                a->ui.theme = kind;
                pc_ui_shell_app_apply_theme(a);
            }
            const char *language = yyjson_mut_get_str(yyjson_mut_obj_get(root, "language"));
            a->language = pc_i18n_from_pref(language ? language : "auto");
            a->applied_prefs = revision;
            a->system_theme = system;
            a->menu_dirty = true;
        }
        pc_json_value_fini(&value);
    }
    pc_i18n_set_current(a->language ? a->language : pc_i18n_en());
}

/* ---- command routing (shell menus::invoke) ---- */

/* The shell-handled UI commands: theme switching and panel toggles change UiState; the rest of the
 * UI-command table (zoom/rotate/screen-mode, workspace, ...) is handled by its view/panel modules
 * as later chunks land. An engine command runs through the session. */
static size_t *group_tab_slot(pc_ui_shell_app *a, pc_ui_group g);
static bool *group_flag(pc_ui_panels *p, pc_ui_group g)
{
    switch (g) {
    case PC_UI_GROUP_COLOR:
        return &p->color;
    case PC_UI_GROUP_PROPERTIES:
        return &p->properties;
    case PC_UI_GROUP_CHARACTER:
        return &p->character;
    case PC_UI_GROUP_NAVIGATOR:
        return &p->navigator;
    case PC_UI_GROUP_HISTORY:
        return &p->history;
    case PC_UI_GROUP_LAYERS:
        return &p->layers;
    default:
        return NULL;
    }
}
static pc_status show_panel(pc_ui_shell_app *a, const char *id)
{
    static const struct {
        const char *id;
        pc_ui_group group;
        const char *tab;
    } panels[] = {{"color", PC_UI_GROUP_COLOR, "Color"},
                  {"swatches", PC_UI_GROUP_COLOR, "Swatches"},
                  {"gradients", PC_UI_GROUP_COLOR, "Gradients"},
                  {"patterns", PC_UI_GROUP_COLOR, "Patterns"},
                  {"properties", PC_UI_GROUP_PROPERTIES, "Properties"},
                  {"adjustments", PC_UI_GROUP_PROPERTIES, "Adjustments"},
                  {"character", PC_UI_GROUP_CHARACTER, "Character"},
                  {"paragraph", PC_UI_GROUP_CHARACTER, "Paragraph"},
                  {"navigator", PC_UI_GROUP_NAVIGATOR, "Navigator"},
                  {"histogram", PC_UI_GROUP_NAVIGATOR, "Histogram"},
                  {"info", PC_UI_GROUP_NAVIGATOR, "Info"},
                  {"history", PC_UI_GROUP_HISTORY, "History"},
                  {"actions", PC_UI_GROUP_HISTORY, "Actions"},
                  {"layerComps", PC_UI_GROUP_HISTORY, "Layer Comps"},
                  {"layers", PC_UI_GROUP_LAYERS, "Layers"},
                  {"channels", PC_UI_GROUP_LAYERS, "Channels"},
                  {"paths", PC_UI_GROUP_LAYERS, "Paths"}};
    for (size_t i = 0; i < sizeof panels / sizeof panels[0]; i++) {
        if (strcmp(id, panels[i].id)) {
            continue;
        }
        size_t count = 0;
        const char *const *tabs = pc_ui_group_tabs(panels[i].group, a->tokens.pro, &count);
        for (size_t t = 0; t < count; t++) {
            if (!strcmp(tabs[t], panels[i].tab)) {
                *group_flag(&a->ui.panels, panels[i].group) = true;
                *group_tab_slot(a, panels[i].group) = t;
                pc_ui_dock_show_tab(&a->ui.dock, panels[i].group, t, a->tokens.pro);
                pc_ui_dock_set_collapsed(&a->ui.dock, panels[i].group, false);
                a->ui.panels.dock = true;
                return PC_OK;
            }
        }
    }
    return PC_ERR_NOT_FOUND;
}
static pc_status invoke_ui_command(pc_ui_shell_app *a, const char *id, pc_json_value *out, pc_error *err)
{
    if (!strcmp(id, "type.panels.character")) {
        return show_panel(a, "character");
    }
    if (!strcmp(id, "type.panels.paragraph")) {
        return show_panel(a, "paragraph");
    }
    if (!strcmp(id, "window.toolbar.double")) {
        a->ui.panels.toolbar_double = !a->ui.panels.toolbar_double;
        return PC_OK;
    }
    /* window.theme.<id> selects an interface theme. */
    static const char theme_prefix[] = "window.theme.";
    if (strncmp(id, theme_prefix, sizeof theme_prefix - 1) == 0) {
        pc_ui_theme_kind k;
        if (!pc_ui_theme_kind_from_name(id + sizeof theme_prefix - 1, &k)) {
            return pc_fail(err, PC_ERR_INVALID, "unknown interface theme");
        }
        char params[192];
        (void)snprintf(params, sizeof params, "{\"path\":\"interface.theme\",\"value\":\"%s\"}",
                       pc_ui_theme_kind_id(k));
        pc_json_value result = PC_JSON_VALUE_INIT;
        pc_status status = pc_engine_session_execute_json(a->session, "prefs.set", params, &result, err);
        pc_json_value_fini(&result);
        if (status != PC_OK) {
            return status;
        }
        a->ui.theme = k;
        pc_ui_shell_app_apply_theme(a);
        return PC_OK;
    }
    if (!strcmp(id, "window.togglePanels")) {
        pc_ui_panels *p = &a->ui.panels;
        if (p->has_hidden_by_tab) {
            p->toolbar = p->hidden_by_tab[0];
            p->options_bar = p->hidden_by_tab[1];
            p->dock = p->hidden_by_tab[2];
            p->has_hidden_by_tab = false;
        } else {
            p->hidden_by_tab[0] = p->toolbar;
            p->hidden_by_tab[1] = p->options_bar;
            p->hidden_by_tab[2] = p->dock;
            p->toolbar = false;
            p->options_bar = false;
            p->dock = false;
            p->has_hidden_by_tab = true;
        }
        return PC_OK;
    }
    if (!strcmp(id, "window.panel.timeline")) {
        a->ui.timeline.open = !a->ui.timeline.open;
        if (!a->ui.timeline.open)
            a->ui.timeline.playing = false;
        return PC_OK;
    }
    if (!strcmp(id, "window.panel.brushes") || !strcmp(id, "window.panel.brushSettings")) {
        a->ui.brush_tab = !strcmp(id, "window.panel.brushes") ? 1 : 0;
        a->ui.panels.brush_settings = true;
        return PC_OK;
    }
    if (!strncmp(id, "window.panel.", 13)) {
        pc_status status = show_panel(a, id + 13);
        if (status == PC_OK) {
            return status;
        }
        const char *alias = pc_ui_panel_alias(id);
        if (alias) {
            return invoke_ui_command(a, alias, out, err);
        }
    }
    if (!strcmp(id, "type.panels.character")) {
        return show_panel(a, "character");
    }
    if (!strcmp(id, "type.panels.paragraph")) {
        return show_panel(a, "paragraph");
    }
    /* window.toggle.<panel> flips a panel's visibility. */
    static const char toggle_prefix[] = "window.toggle.";
    if (strncmp(id, toggle_prefix, sizeof toggle_prefix - 1) == 0) {
        const char *p = id + sizeof toggle_prefix - 1;
        pc_ui_panels *pn = &a->ui.panels;
        bool *slot = NULL;
        if (strcmp(p, "layers") == 0) {
            slot = &pn->layers;
        } else if (strcmp(p, "history") == 0) {
            slot = &pn->history;
        } else if (strcmp(p, "properties") == 0) {
            slot = &pn->properties;
        } else if (strcmp(p, "color") == 0) {
            slot = &pn->color;
        } else if (strcmp(p, "navigator") == 0) {
            slot = &pn->navigator;
        } else if (strcmp(p, "character") == 0) {
            slot = &pn->character;
        }
        if (!strcmp(p, "toolbar")) {
            slot = &pn->toolbar;
        } else if (!strcmp(p, "options")) {
            slot = &pn->options_bar;
        } else if (!strcmp(p, "statusBar")) {
            slot = &pn->status_bar;
        } else if (!strcmp(p, "dock")) {
            slot = &pn->dock;
        } else if (!strcmp(p, "brushSettings")) {
            slot = &pn->brush_settings;
        }
        if (slot != NULL) {
            *slot = !*slot;
            return PC_OK;
        }
        return pc_fail(err, PC_ERR_UNSUPPORTED, "panel toggle `%s` is unavailable", p);
    }
    /* A toolbar tool-select id activates that tool. */
    pc_ui_tool tool;
    if (pc_ui_tool_from_select_id(id, &tool)) {
        a->ui.tool = tool;
        pc_ui_layer_picker_prune(a->session, &a->ui);
        pc_ui_state_remember_group_tool(&a->ui);
        return PC_OK;
    }
    (void)out;
    return pc_fail(err, PC_ERR_UNSUPPORTED, "interface command `%s` is not implemented", id);
}

pc_status pc_ui_shell_app_invoke(pc_ui_shell_app *a, const char *id, yyjson_val *params, pc_json_value *out,
                                 pc_error *err)
{
    if (id == NULL || id[0] == '\0') {
        return pc_fail(err, PC_ERR_INVALID, "missing `command`");
    }
    if (!strcmp(id, "ui.context.choose")) {
        size_t length = 0;
        const char *choice = pc_json_as_str(pc_json_get(params, "id"), &length);
        if (!choice || !length || length > 255 || strlen(choice) != length)
            return pc_fail(err, PC_ERR_INVALID, "invalid context action id");
        return pc_ui_shell_context_choose(a, choice, out, err);
    }
    pc_status layer_picker = pc_ui_shell_layer_picker_command(a, id, params, out, err);
    if (layer_picker != PC_ERR_NOT_FOUND)
        return layer_picker;
    pc_status lasso = pc_ui_shell_lasso_command(a, id, out, err);
    if (lasso != PC_ERR_NOT_FOUND)
        return lasso;
    pc_status slice = pc_ui_shell_slice_command(a, id, out, err);
    if (slice != PC_ERR_NOT_FOUND)
        return slice;
    pc_status crop = pc_ui_shell_crop_command(a, id, params, out, err);
    if (crop != PC_ERR_NOT_FOUND)
        return crop;
    pc_status transform = pc_ui_shell_transform_command(a, id, params, out, err);
    if (transform != PC_ERR_NOT_FOUND)
        return transform;
    pc_status artboard = pc_ui_shell_artboard_command(a, id, params, out, err);
    if (artboard != PC_ERR_NOT_FOUND)
        return artboard;
    if (!strcmp(id, "ui.notice.dismiss")) {
        uint64_t notice = 0;
        if (!pc_json_as_u64(pc_json_get(params, "notice"), &notice))
            return pc_fail(err, PC_ERR_INVALID, "notice must be an id");
        char preference[256];
        if (!pc_ui_notices_dismiss(&a->notices, notice, preference, sizeof preference))
            return pc_fail(err, PC_ERR_NOT_FOUND, "notice no longer exists");
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL;
        pc_status status = root && yyjson_mut_obj_add_uint(d, root, "notice", notice) &&
                                   yyjson_mut_obj_add_bool(d, root, "dismissed", true)
                               ? pc_json_value_from_mut(root, out, err)
                               : pc_fail(err, PC_ERR_NOMEM, "notice reply");
        yyjson_mut_doc_free(d);
        return status;
    }
    if (!strcmp(id, "ui.pen.cancel")) {
        pc_ui_pen_cancel(&a->ui);
        return PC_OK;
    }
    if (!strcmp(id, "ui.pen.finish")) {
        return pc_ui_pen_commit(a->session, &a->ui, false, out, err);
    }
    if (!strcmp(id, "layer.renameLayer") && !pc_json_get(params, "name")) {
        const pc_engine_doc_state *state = pc_engine_session_active(a->session);
        uint64_t layer = state && state->has_active_layer ? state->active_layer : 0;
        yyjson_val *given = pc_json_get(params, "layer");
        if (given && !pc_json_as_u64(given, &layer))
            return pc_fail(err, PC_ERR_INVALID, "rename layer id must be an unsigned integer");
        pc_status status = pc_ui_layer_rename_start(a, &a->panel_runtime, layer, err);
        if (status == PC_OK) {
            pc_json_value_fini(out);
            *out = (pc_json_value)PC_JSON_VALUE_INIT;
        }
        return status;
    }
    if (a->ui.has_pen && !strcmp(id, "edit.undo") && !a->ui.n_dialogs) {
        return pc_ui_pen_key(a->session, &a->ui, PC_KEY_BACKSPACE, 0, out, err);
    }
    if (!strcmp(id, "ui.type.insertGlyph")) {
        size_t length = 0;
        const char *text = pc_json_as_str(pc_json_get(params, "text"), &length);
        if (!text) {
            return pc_fail(err, PC_ERR_INVALID, "missing glyph text");
        }
        if (a->ui.n_dialogs) {
            return pc_fail(err, PC_ERR_INVALID, "confirm or cancel the open dialog first");
        }
        pc_status status = a->ui.has_text_edit
                               ? pc_ui_type_insert(a->session, &a->ui, text, length, out, err)
                               : pc_engine_session_execute(a->session, "type.insertText", params, out, err);
        if (status == PC_OK) {
            (void)pc_ui_type_glyph_used(&a->ui.type_panels, text, length, NULL);
            a->menu_dirty = true;
        }
        return status;
    }
    pc_status type_panel = pc_ui_shell_type_panels_menu(a, id, params, out, err);
    if (type_panel != PC_ERR_NOT_FOUND) {
        return type_panel;
    }
    if (!strcmp(id, "ui.type.styleColor")) {
        uint64_t style = 0;
        if (!pc_json_as_u64(pc_json_get(params, "id"), &style) || style > UINT32_MAX) {
            return pc_fail(err, PC_ERR_INVALID, "invalid character style");
        }
        return pc_ui_shell_type_style_color(a, (uint32_t)style, out, err);
    }
    if (!strcmp(id, "ui.type.color")) {
        return pc_ui_shell_type_color(a, out, err);
    }
    if (!strcmp(id, "ui.type.commit")) {
        return pc_ui_type_commit(a->session, &a->ui, 0, out, err);
    }
    if (!strcmp(id, "ui.type.cancel")) {
        return pc_ui_type_cancel(a->session, &a->ui, err);
    }
    bool type_format = !strcmp(id, "type.setStyle") || !strcmp(id, "type.edit") ||
                       !strncmp(id, "type.increaseSize", 17) || !strncmp(id, "type.decreaseSize", 17) ||
                       !strncmp(id, "type.characterStyle.", 20) || !strncmp(id, "type.paragraphStyle.", 20);
    if (a->ui.has_text_edit && type_format && !a->ui.n_dialogs) {
        a->menu_dirty = true;
        return pc_ui_type_edit_command(a->session, &a->ui, id, params,
                                       pc_json_get(params, "coalesce") != NULL, strcmp(id, "type.edit") != 0,
                                       out, err);
    }
    pc_ui_tool next_tool = PC_TOOL_MOVE;
    bool selecting_tool = pc_ui_tool_from_select_id(id, &next_tool);
    const pc_engine_command *engine_command = pc_engine_command_find(id);
    bool engine_edit = engine_command && engine_command->journal && strncmp(id, "tools.", 6) &&
                       strncmp(id, "prefs.", 6) && strncmp(id, "view.", 5);
    bool leave_text = engine_edit || !strncmp(id, "layer.select", 12) || selecting_tool ||
                      !strncmp(id, "file.", 5) || !strncmp(id, "window.document.", 16) ||
                      !strcmp(id, "window.arrange.nextDocument") ||
                      !strcmp(id, "window.arrange.previousDocument");
    if (leave_text && a->ui.direct.active)
        pc_ui_direct_cancel(&a->ui.direct);
    bool leave_pen = leave_text && (!selecting_tool || !pc_ui_pen_drawn_with(&a->ui.pen, next_tool));
    if (a->ui.has_pen && leave_pen && !a->ui.n_dialogs) {
        pc_json_value committed = PC_JSON_VALUE_INIT;
        pc_status status = pc_ui_pen_commit(a->session, &a->ui, false, &committed, err);
        pc_json_value_fini(&committed);
        if (status != PC_OK) {
            return status;
        }
    }
    if (a->ui.has_text_edit && leave_text && !a->ui.n_dialogs) {
        pc_json_value committed = PC_JSON_VALUE_INIT;
        pc_status status = pc_ui_type_commit(a->session, &a->ui, 0, &committed, err);
        pc_json_value_fini(&committed);
        if (status != PC_OK) {
            return status;
        }
    }
    a->menu_dirty = true;
    pc_status discard = pc_ui_shell_discard_intercept(a, id, params, out, err);
    if (discard != PC_ERR_NOT_FOUND) {
        return discard;
    }
    if (!strcmp(id, "file.exit")) {
        a->quit_requested = true;
        return PC_OK;
    }
    if (!strncmp(id, "file.openRecent.", 16)) {
        if (a->ui.n_dialogs) {
            return pc_fail(err, PC_ERR_INVALID, "confirm or cancel the open dialog first");
        }
        size_t index = 0;
        const char *number = id + 16;
        if (!*number) {
            return pc_fail(err, PC_ERR_INVALID, "missing recent file index");
        }
        for (const char *p = number; *p; p++) {
            if (*p < '0' || *p > '9' || index > (SIZE_MAX - (size_t)(*p - '0')) / 10) {
                return pc_fail(err, PC_ERR_INVALID, "invalid recent file index");
            }
            index = index * 10 + (size_t)(*p - '0');
        }
        if (index >= a->ui.n_recent_files) {
            return pc_fail(err, PC_ERR_NOT_FOUND, "no such recent file");
        }
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (!d) {
            return pc_fail(err, PC_ERR_NOMEM, "recent file request");
        }
        yyjson_mut_val *p = yyjson_mut_obj(d);
        pc_buf text = PC_BUF_INIT;
        yyjson_doc *read = NULL;
        pc_status status = yyjson_mut_obj_add_strcpy(d, p, "path", a->ui.recent_files[index])
                               ? pc_json_write(p, 0, &text, err)
                               : pc_fail(err, PC_ERR_NOMEM, "recent file path");
        if (status == PC_OK) {
            status = pc_json_parse(pc_buf_cstr(&text), text.len, &read, err);
        }
        if (status == PC_OK) {
            status = pc_ui_shell_file_execute(a, "file.open", yyjson_doc_get_root(read), out, err);
        }
        if (status == PC_OK) {
            status = pc_ui_shell_app_sync_views(a, err);
        }
        yyjson_doc_free(read);
        pc_buf_fini(&text);
        yyjson_mut_doc_free(d);
        return status;
    }
    bool next_doc = !strcmp(id, "window.nextDocument"), previous_doc = !strcmp(id, "window.previousDocument");
    if (!strncmp(id, "window.document.", 16) || next_doc || previous_doc) {
        if (a->ui.n_dialogs) {
            return pc_fail(err, PC_ERR_INVALID, "confirm or cancel the open dialog first");
        }
        size_t count = 0, index = 0;
        (void)pc_engine_session_documents(a->session, &count);
        if (!count || !pc_engine_session_active_index(a->session, &index)) {
            return pc_fail(err, PC_ERR_INVALID, "no document open");
        }
        if (next_doc) {
            index = (index + 1) % count;
        } else if (previous_doc) {
            index = (index + count - 1) % count;
        } else {
            const char *number = id + 16;
            index = 0;
            if (!*number) {
                return pc_fail(err, PC_ERR_INVALID, "missing document index");
            }
            for (const char *p = number; *p; p++) {
                if (*p < '0' || *p > '9' || index > (SIZE_MAX - (size_t)(*p - '0')) / 10) {
                    return pc_fail(err, PC_ERR_INVALID, "invalid document index");
                }
                index = index * 10 + (size_t)(*p - '0');
            }
        }
        if (index >= count) {
            return pc_fail(err, PC_ERR_INVALID, "no such document");
        }
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (!d) {
            return pc_fail(err, PC_ERR_NOMEM, "document switch");
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, root, "document", index);
        pc_status status = pc_json_value_from_mut(root, out, err);
        yyjson_mut_doc_free(d);
        if (status != PC_OK) {
            return status;
        }
        (void)pc_engine_session_set_active(a->session, index);
        return pc_ui_shell_app_sync_views(a, err);
    }
    if (!strcmp(id, "ui.dialog.colorField")) {
        return pc_ui_shell_dialog_color_field(a, params, out, err);
    }
    if (!strcmp(id, "ui.dialog.open")) {
        return pc_ui_shell_dialog_control(a, id, params, out, err);
    }
    if (a->ui.n_dialogs && !pc_ui_is_nav_command(id)) {
        return pc_fail(err, PC_ERR_INVALID, "confirm or cancel the open dialog first");
    }
    if (pc_ui_shell_workspace_handles(id))
        return pc_ui_shell_workspace_command(a, id, params, out, err);
    pc_status rasterize_filter = pc_ui_shell_rasterize_filter(a, id, params, out, err);
    if (rasterize_filter != PC_ERR_NOT_FOUND)
        return rasterize_filter;
    if (!params && pc_ui_shell_form_command(id))
        return pc_ui_shell_form_open(a, id, out, err);
    if (!params && pc_ui_shell_variables_command(id))
        return pc_ui_shell_variables_open(a, id, out, err);
    const char *style_kind = NULL;
    if (!params && pc_ui_shell_style_command(id, &style_kind)) {
        return pc_ui_shell_style_open(a, style_kind, out, err);
    }
    if (!params && pc_ui_filter_dialog_command(id)) {
        return pc_ui_shell_filter_open(a, id, out, err);
    }
    if (strcmp(id, "file.new") == 0 && !params) {
        return pc_ui_shell_dialog_new(a, out, err);
    }
    if (!strncmp(id, "edit.preferences.", 17)) {
        return pc_ui_shell_preferences_open(a, id + 17, out, err);
    }
    if (!params && (!strcmp(id, "file.open") || !strcmp(id, "file.openAs") || !strcmp(id, "file.saveAs") ||
                    !strcmp(id, "file.saveACopy") || !strcmp(id, "file.placeEmbedded") ||
                    !strcmp(id, "file.placeLinked"))) {
        return pc_ui_shell_dialog_file(a, id, out, err);
    }
    if (!params && !strcmp(id, "file.save")) {
        const pc_engine_doc_state *active = pc_engine_session_active(a->session);
        if (active &&
            (!active->path || !pc_engine_file_can_write_back(active->path) || active->source_read_only)) {
            return pc_ui_shell_dialog_file(a, "file.saveAs", out, err);
        }
    }
    if (!strcmp(id, "file.open") || !strcmp(id, "file.save") || !strcmp(id, "file.saveAs")) {
        pc_status status = pc_ui_shell_file_execute(a, id, params, out, err);
        if (status == PC_OK) {
            status = pc_ui_shell_app_sync_views(a, err);
        }
        return status;
    }
    if (!strcmp(id, "help.systemInfo"))
        return pc_ui_shell_system_info_open(a, out, err);
    if (strcmp(id, "help.about") == 0) {
        yyjson_doc *request = NULL;
        pc_status status =
            pc_json_parse("{\"kind\":\"about\"}", sizeof "{\"kind\":\"about\"}" - 1, &request, err);
        if (status == PC_OK) {
            status = pc_ui_shell_dialog_control(a, "ui.dialog.open", yyjson_doc_get_root(request), out, err);
        }
        yyjson_doc_free(request);
        return status;
    }
    a->menu_dirty = true;
    pc_status sync = pc_ui_canvas_sync(a, &a->canvas, err);
    if (sync != PC_OK) {
        return sync;
    }
    pc_str saved_mode = PC_STR_INIT;
    bool screen = strncmp(id, "view.screenMode.", 16) == 0;
    if (screen && a->ui.view.screen_mode.ptr &&
        pc_str_set(&saved_mode, a->ui.view.screen_mode.ptr, a->ui.view.screen_mode.len, err) != PC_OK) {
        return err ? err->code : PC_ERR_NOMEM;
    }
    pc_json_value display_result = PC_JSON_VALUE_INIT;
    pc_status display = pc_ui_view_options_command(&a->ui.view, id, params, &display_result, err);
    if (display == PC_OK) {
        if (strcmp(id, "view.show.all") == 0) {
            a->ui.extras.grid = true;
            a->ui.extras.guides = true;
        }
        if (screen && a->have_window && !(SDL_GetWindowFlags(a->win) & SDL_WINDOW_HIDDEN) &&
            !SDL_SetWindowFullscreen(a->win, !pc_ui_view_standard(&a->ui.view))) {
            pc_str_fini(&a->ui.view.screen_mode);
            a->ui.view.screen_mode = saved_mode;
            pc_json_value_fini(&display_result);
            return pc_fail(err, PC_ERR_INVALID, "screen mode: %s", SDL_GetError());
        }
        a->canvas.dragging = false;
        a->canvas.scroll_drag.active = false;
        *out = display_result;
    }
    pc_str_fini(&saved_mode);
    if (display != PC_ERR_NOT_FOUND) {
        return display;
    }
    pc_status camera = pc_ui_canvas_view_command(a, id, params, out, err);
    if (camera != PC_ERR_NOT_FOUND) {
        if (camera == PC_OK) {
            a->canvas.dragging = false;
            a->canvas.scroll_drag.active = false;
        }
        return camera;
    }
    if (pc_engine_command_find(id) != NULL) {
        float foreground[4];
        memcpy(foreground, pc_engine_session_tools(a->session)->foreground, sizeof foreground);
        pc_status status = pc_ui_shell_jobs_execute(a, id, params, out, err);
        if (status == PC_OK) {
            if (!strncmp(id, "channel.target", 14)) {
                a->ui.mask_target = pc_engine_targeted_mask(a->session);
                a->ui.vector_mask_target = pc_engine_targeted_vector_mask(a->session);
            }
            pc_ui_shell_sync_targets(a);
        }
        if (status == PC_OK && !strncmp(id, "tools.", 6)) {
            status = pc_ui_type_foreground_changed(a->session, &a->ui, foreground, err);
        }
        if (status == PC_OK && a->ui.has_text_edit && !strncmp(id, "view.", 5)) {
            const pc_engine_doc_state *state = pc_engine_session_active(a->session);
            if (state && pc_doc_arc_get(state->doc)->id == a->ui.text_edit.document) {
                a->ui.text_edit.revision = state->revision;
            }
        }
        return status;
    }
    if (pc_ui_command_find(id) != NULL || pc_ui_is_live(id)) {
        pc_status rc = invoke_ui_command(a, id, out, err);
        if (rc == PC_OK) {
            out->doc = NULL; /* UI commands have no JSON result */
        }
        return rc;
    }
    return pc_fail(err, PC_ERR_NOT_FOUND, "unknown command `%s`", id);
}

bool pc_ui_shell_dialog_key(pc_ui_shell_app *a, const SDL_Event *event)
{
    if (!a->ui.n_dialogs || event->type != SDL_EVENT_KEY_DOWN || event->key.repeat) {
        return false;
    }
    if (event->key.key == SDLK_ESCAPE) {
        a->dialog_runtime.cancel_requested = true;
        return true;
    }
    if (event->key.key == SDLK_RETURN || event->key.key == SDLK_KP_ENTER) {
        a->dialog_runtime.confirm_requested = true;
    }
    return false;
}
void pc_ui_shell_modal_changed(pc_ui_shell_app *a)
{
    pc_ui_menu_nav_reset(&a->menus.nav);
    a->menus.levels = 0;
    a->type_pointer.pressed = a->type_pointer.selecting = false;
    a->ui.pen.dragging = a->ui.pen.has_moving = false;
    pc_ui_direct_cancel(&a->ui.direct);
    a->canvas.dragging = false;
    a->canvas.scroll_drag.active = false;
    a->canvas.guide_drag.active = false;
    pc_ui_tool_gesture_fini(&a->canvas.gesture);
    memset(a->canvas.fingers, 0, sizeof a->canvas.fingers);
    a->dialog_runtime.laid_out = false;
    a->dialog_runtime.error[0] = 0;
    a->dialog_runtime.confirm_requested = a->dialog_runtime.cancel_requested = false;
}
bool pc_ui_shell_dialog_at(const pc_ui_shell_app *a, float x, float y)
{
    const pc_ui_dialog_runtime *r = &a->dialog_runtime;
    return a->ui.n_dialogs && r->laid_out && x >= r->rect[0] && y >= r->rect[1] &&
           x < r->rect[0] + r->rect[2] && y < r->rect[1] + r->rect[3];
}
bool pc_ui_shell_app_dialog_action_mapping(const pc_ui_shell_app *a, const char *action, float rect[4])
{
    if (!a || !action || !a->ui.n_dialogs || !a->dialog_runtime.laid_out || !rect) {
        return false;
    }
    const float *source =
        !strcmp(action, "confirm")  ? a->dialog_runtime.confirm
        : !strcmp(action, "cancel") ? a->dialog_runtime.cancel
        : !strcmp(action, "apply") &&
                pc_ui_shell_is_preferences(yyjson_mut_doc_get_root(a->ui.dialogs[a->ui.n_dialogs - 1].fields))
            ? a->dialog_runtime.apply
        : !strcmp(action, "discard") &&
                pc_ui_shell_is_discard(yyjson_mut_doc_get_root(a->ui.dialogs[a->ui.n_dialogs - 1].fields))
            ? a->dialog_runtime.apply
            : NULL;
    if (!source || source[2] <= 0 || source[3] <= 0) {
        return false;
    }
    memcpy(rect, source, 4 * sizeof *rect);
    return true;
}
bool pc_ui_shell_app_dialog_mapping(const pc_ui_shell_app *a, bool confirm, float rect[4])
{
    return pc_ui_shell_app_dialog_action_mapping(a, confirm ? "confirm" : "cancel", rect);
}

unsigned pc_ui_shell_input_modifiers(const pc_ui_shell_app *a)
{
    return a ? a->canvas.held_mods : 0;
}
bool pc_ui_shell_app_panel_mapping(const pc_ui_shell_app *a, const char *command, pc_layer_id layer,
                                   float rect[4])
{
    if (!a || !command || !rect) {
        return false;
    }
    for (size_t i = 0; i < a->panel_runtime.nhits; i++) {
        const pc_ui_panel_hit *hit = &a->panel_runtime.hits[i];
        if (hit->layer == layer && strcmp(hit->command, command) == 0) {
            memcpy(rect, hit->rect, sizeof hit->rect);
            return true;
        }
    }
    return false;
}
void pc_ui_shell_app_gpu_check(pc_ui_shell_app *a)
{
    bool lost = false;
    char detail[256];
    if (!a->canvas.gpu || !pc_gpu_compositor_fault_detail(a->canvas.gpu, &lost, detail) ||
        a->ui.gpu_fallback_notice) {
        return;
    }
    char message[320];
    (void)snprintf(message, sizeof message, "%s%s%s", lost ? "GPU device lost" : "GPU error",
                   detail[0] ? ": " : "", detail);
    a->ui.gpu_fallback_notice = pc_strndup(message, strlen(message));
    const char *title =
        lost ? "GPU device was lost; using the CPU renderer." : "GPU error; using the CPU renderer.";
    free(a->ui.status);
    a->ui.status = pc_strndup(title, strlen(title));
    a->ui.status_error = true;
    const char *lines[] = {message};
    (void)pc_ui_notices_post(&a->notices, title, lines, 1, true, NULL);
}
pc_status pc_ui_shell_app_gpu_info(pc_ui_shell_app *a, pc_json_value *out, pc_error *err)
{
    if (!a || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing app");
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return pc_fail(err, PC_ERR_NOMEM, "GPU diagnostics");
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    pc_json_value preference = PC_JSON_VALUE_INIT, enabled = PC_JSON_VALUE_INIT;
    (void)pc_engine_prefs_get(a->session, "performance.gpuBackend", &preference, NULL);
    (void)pc_engine_prefs_get(a->session, "performance.useGpu", &enabled, NULL);
    const char *wanted = yyjson_mut_get_str(pc_json_value_root(&preference));
    if (!wanted) {
        wanted = "auto";
    }
    bool allowed = strcmp(wanted, "cpu") != 0 && (!yyjson_mut_is_bool(pc_json_value_root(&enabled)) ||
                                                  yyjson_mut_get_bool(pc_json_value_root(&enabled)));
    bool healthy = allowed && pc_gpu_compositor_healthy(a->canvas.gpu);
    yyjson_mut_obj_add_str(doc, root, "canvas", healthy ? "gpu" : "cpu");
    yyjson_mut_obj_add_str(doc, root, "backend", a->have_window ? "gl" : "none");
    const GLubyte *version = a->have_window ? a->gl.glGetString(GL_VERSION) : NULL;
    const GLubyte *renderer = a->have_window ? a->gl.glGetString(0x1F01) : NULL; /* GL_RENDERER */
    yyjson_mut_obj_add_strcpy(doc, root, "adapter", renderer ? (const char *)renderer : "");
    yyjson_mut_obj_add_strcpy(doc, root, "driver", version ? (const char *)version : "");
    yyjson_mut_obj_add_str(doc, root, "deviceType", "other"); /* GL 3.3 has no portable query. */
    yyjson_mut_obj_add_strcpy(doc, root, "preference", wanted);
    yyjson_mut_obj_add_str(doc, root, "selected", healthy ? "gl" : "cpu");
    if (allowed && strcmp(wanted, "auto") != 0 && strcmp(wanted, "gl") != 0) {
        yyjson_mut_obj_add_str(doc, root, "fallback", "This build uses OpenGL 3.3.");
    } else {
        yyjson_mut_obj_add_null(doc, root, "fallback");
    }
    yyjson_mut_obj_add_uint(doc, root, "submissions", pc_gpu_compositor_submissions(a->canvas.gpu));
    yyjson_mut_obj_add_uint(doc, root, "sourceCacheHits", pc_gpu_compositor_source_hits(a->canvas.gpu));
    yyjson_mut_obj_add_uint(doc, root, "sourceUploads", pc_gpu_compositor_source_uploads(a->canvas.gpu));
    yyjson_mut_obj_add_uint(doc, root, "sourceCacheBytes", pc_gpu_compositor_source_bytes(a->canvas.gpu));
    yyjson_mut_obj_add_uint(doc, root, "prefixCacheBytes", pc_gpu_compositor_prefix_bytes(a->canvas.gpu));
    yyjson_mut_obj_add_uint(doc, root, "prefixCacheHits", pc_gpu_compositor_prefix_hits(a->canvas.gpu));
    yyjson_mut_obj_add_uint(doc, root, "prefixCacheDocument", pc_gpu_compositor_document(a->canvas.gpu));
    yyjson_mut_obj_add_uint(doc, root, "effectFragments", pc_gpu_compositor_effect_fragments(a->canvas.gpu));
    yyjson_mut_obj_add_uint(doc, root, "effectMapBuilds", pc_gpu_compositor_effect_builds(a->canvas.gpu));
    yyjson_mut_obj_add_uint(doc, root, "effectCacheHits", pc_gpu_compositor_effect_hits(a->canvas.gpu));
    pc_json_value_fini(&preference);
    pc_json_value_fini(&enabled);
    bool lost;
    char detail[256];
    if (pc_gpu_compositor_fault_detail(a->canvas.gpu, &lost, detail)) {
        char reason[320];
        (void)snprintf(reason, sizeof reason, "%s%s%s", lost ? "GPU device lost" : "GPU error",
                       detail[0] ? ": " : "", detail);
        yyjson_mut_obj_add_strcpy(doc, root, "lost", reason);
    } else {
        yyjson_mut_obj_add_null(doc, root, "lost");
    }
    pc_status rc = pc_json_value_from_mut(root, out, err);
    yyjson_mut_doc_free(doc);
    return rc;
}
pc_status pc_ui_shell_app_gpu_simulate_loss(pc_ui_shell_app *a, bool error, pc_json_value *out, pc_error *err)
{
    if (!a || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing app");
    }
    pc_json_value before = PC_JSON_VALUE_INIT;
    pc_status inspected = pc_ui_shell_app_gpu_info(a, &before, err);
    if (inspected != PC_OK) {
        return inspected;
    }
    yyjson_mut_val *canvas = yyjson_mut_obj_get(pc_json_value_root(&before), "canvas");
    const char *renderer = yyjson_mut_get_str(canvas);
    bool active = renderer && strcmp(renderer, "gpu") == 0;
    pc_json_value_fini(&before);
    if (a->canvas.gpu) {
        pc_gpu_compositor_fault(a->canvas.gpu, !error,
                                error ? "simulated error (ui.gpu.simulateLoss)"
                                      : "simulated (ui.gpu.simulateLoss)");
    }
    pc_ui_shell_app_gpu_check(a);
    pc_json_value info = PC_JSON_VALUE_INIT;
    pc_status rc = pc_ui_shell_app_gpu_info(a, &info, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        pc_json_value_fini(&info);
        return pc_fail(err, PC_ERR_NOMEM, "GPU diagnostics");
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_obj_add_bool(doc, root, "wasActive", active);
    yyjson_mut_obj_add_val(doc, root, "gpuInfo", pc_json_normalized_copy(doc, pc_json_value_root(&info)));
    rc = pc_json_value_from_mut(root, out, err);
    yyjson_mut_doc_free(doc);
    pc_json_value_fini(&info);
    return rc;
}

/* ---- menu model cache ---- */

static pc_status refresh_menu(pc_ui_shell_app *a, pc_error *err)
{
    if (!a->menu_dirty && a->items != NULL) {
        return PC_OK;
    }
    pc_ui_menu_item *items = NULL;
    size_t n = 0;
    pc_status rc = pc_ui_menu_state_items(a->session, &a->ui, &items, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    free_menu_items(a);
    a->items = items;
    a->nitems = n;
    a->menu_dirty = false;
    return PC_OK;
}

/* ---- the frame UI (Nuklear) ---- */

static void draw_menu_bar(pc_ui_shell_app *a)
{
    nk_menubar_begin(&a->ctx);
    pc_ui_native_menu_bar(a, &a->menus);
    nk_menubar_end(&a->ctx);
}
bool pc_ui_shell_menu_key(pc_ui_shell_app *a, const SDL_Event *event)
{
    if (refresh_menu(a, NULL) != PC_OK) {
        return false;
    }
    return pc_ui_native_menu_key(a, &a->menus, a->items, a->nitems, event);
}
bool pc_ui_shell_menu_capture(pc_ui_shell_app *a, const SDL_Event *event)
{
    return pc_ui_native_menu_capture(a, &a->menus, event);
}

bool pc_ui_shell_dock_cancel(pc_ui_shell_app *a, const SDL_Event *event)
{
    return pc_ui_docking_cancel(a, &a->docking, event);
}
bool pc_ui_shell_dock_capture(pc_ui_shell_app *a, const SDL_Event *event)
{
    return pc_ui_docking_capture(a, &a->docking, event);
}

/* Is group `g`'s panel currently shown? (panels.<key>) */
static bool group_shown(const pc_ui_shell_app *a, pc_ui_group g)
{
    const pc_ui_panels *p = &a->ui.panels;
    switch (g) {
    case PC_UI_GROUP_COLOR:
        return p->color;
    case PC_UI_GROUP_PROPERTIES:
        return p->properties;
    case PC_UI_GROUP_CHARACTER:
        return p->character;
    case PC_UI_GROUP_NAVIGATOR:
        return p->navigator;
    case PC_UI_GROUP_HISTORY:
        return p->history;
    case PC_UI_GROUP_LAYERS:
        return p->layers;
    default:
        return false;
    }
}

/* The selected-tab slot for group `g` in UiState.dock_tabs. */
static size_t *group_tab_slot(pc_ui_shell_app *a, pc_ui_group g)
{
    pc_ui_dock_tabs *t = &a->ui.dock_tabs;
    switch (g) {
    case PC_UI_GROUP_COLOR:
        return &t->color;
    case PC_UI_GROUP_PROPERTIES:
        return &t->properties;
    case PC_UI_GROUP_CHARACTER:
        return &t->character;
    case PC_UI_GROUP_NAVIGATOR:
        return &t->navigator;
    case PC_UI_GROUP_HISTORY:
        return &t->history;
    case PC_UI_GROUP_LAYERS:
        return &t->layers;
    default:
        return &t->layers;
    }
}

/* Canvas display: the CPU oracle is cached by revision; the GL camera draws the document. */
static void draw_canvas_body(pc_ui_shell_app *a)
{
    struct nk_context *c = &a->ctx;
    size_t n = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(a->session, &n);
    bool tabs = pc_ui_view_standard(&a->ui.view);
    if (tabs) {
        nk_layout_row_dynamic(c, 24, n > 0 && n < 8 ? (int)n : 1);
        for (size_t i = 0; i < n; i++) {
            const pc_document *doc = pc_doc_arc_get(docs[i].doc);
            char tab[160];
            (void)snprintf(tab, sizeof tab, "%.120s%s", pc_str_c(&doc->name),
                           pc_engine_doc_state_is_dirty(&docs[i]) ? " *" : "");
            if (nk_button_label(c, tab)) {
                char id[64];
                (void)snprintf(id, sizeof id, "window.document.%zu", i);
                pc_ui_shell_queue(a, id, NULL);
            }
        }
    }
    struct nk_rect r = nk_window_get_content_region(c);
    r.y += tabs ? 28.f : 0.f;
    r.h -= tabs ? 28.f : 0.f;
    if (r.w < 1.f || r.h < 1.f) {
        return;
    }
    struct nk_command_buffer *cv = nk_window_get_canvas(c);
    nk_fill_rect(cv, r, 0, pc_ui_nk_color(pc_ui_shell_canvas_color(a)));
    if (n) {
        pc_error err;
        if (pc_ui_canvas_view_draw(a, &a->canvas, &a->gl, r, &err) != PC_OK) {
            nk_draw_text(cv, nk_rect(r.x + 16, r.y + 16, r.w - 32, 24), err.msg, (int)strlen(err.msg),
                         &a->font.nk, pc_ui_nk_color(a->tokens.canvas), pc_ui_nk_color(a->tokens.danger));
        }
        pc_ui_shell_type_draw(a, cv);
    } else {
        const char *hint = "Open or create a document (File > New)";
        nk_draw_text(cv, nk_rect(r.x + 16, r.y + 16, r.w - 32, 20), hint, (int)strlen(hint), &a->font.nk,
                     pc_ui_nk_color(a->tokens.canvas), pc_ui_nk_color(a->tokens.text_dim));
    }
}

/* One dock group: its tab strip (document tabs fitted to the width) and the active tab's body. */
static void draw_dock_group(pc_ui_shell_app *a, pc_ui_group g, float w)
{
    struct nk_context *c = &a->ctx;
    bool pro = pc_ui_theme_kind_is_pro(a->ui.theme);
    bool collapsed = pc_ui_dock_is_collapsed(&a->ui.dock, g);
    size_t vis[PC_UI_DOCK_MAX_TABS];
    size_t nvis = pc_ui_dock_visible_tabs(&a->ui.dock, g, pro, vis);
    size_t ntabs = 0;
    const char *const *tabs = pc_ui_group_tabs(g, pro, &ntabs);
    size_t *selected = group_tab_slot(a, g);

    /* Tab strip row: fit tab widths into the width less a 22 px panel-menu button. */
    float natural[PC_UI_DOCK_MAX_TABS];
    for (size_t i = 0; i < nvis && i < PC_UI_TAB_MAX; i++) {
        natural[i] = (float)strlen(tabs[vis[i]]) * 7.0f + 20.0f;
    }
    size_t sel_vis = 0;
    for (size_t i = 0; i < nvis; i++) {
        if (vis[i] == *selected) {
            sel_vis = i;
        }
    }
    pc_ui_tab_fit fit = pc_ui_tab_fit_compute(natural, nvis, sel_vis, w - 26.0f, 44.0f, PC_UI_TAB_CHEVRON_W);

    nk_layout_row_begin(c, NK_STATIC, (float)STRIP_H - 6.0f, (int)fit.n_shown + 1);
    for (size_t i = 0; i < fit.n_shown; i++) {
        size_t tab = vis[fit.shown[i].index];
        nk_layout_row_push(c, fit.shown[i].width);
        bool is_sel = tab == *selected && !collapsed;
        struct nk_style_button sb = c->style.button;
        if (is_sel) {
            c->style.button.normal = nk_style_item_color(pc_ui_nk_color(a->tokens.card));
            c->style.button.text_normal = pc_ui_nk_color(a->tokens.text);
        }
        struct nk_rect tab_rect = nk_widget_bounds(c);
        pc_ui_docking_tab(&a->docking, g, tab, tab_rect);
        char mapping[96];
        (void)snprintf(mapping, sizeof mapping, "dock.tab.%s", tabs[tab]);
        pc_ui_shell_widget_mapping(a, mapping, tab_rect);
        if (nk_button_label(c, tabs[tab])) {
            *selected = tab;
            a->menu_dirty = true;
        }
        c->style.button = sb;
    }
    /* Panel menu / collapse toggle. */
    nk_layout_row_push(c, 22.0f);
    if (nk_button_label(c, collapsed ? ">" : "v")) {
        pc_ui_dock_set_collapsed(&a->ui.dock, g, !collapsed);
    }
    nk_layout_row_end(c);

    if (!collapsed) {
        size_t cur = *selected < ntabs ? *selected : 0;
        struct nk_rect region = nk_window_get_content_region(c);
        struct nk_rect next = nk_widget_bounds(c);
        float height = region.y + region.h - next.y - 4;
        nk_layout_row_dynamic(c, height > 20 ? height : 20, 1);
        char body_id[64];
        (void)snprintf(body_id, sizeof body_id, "panel-%s", tabs[cur]);
        if (nk_group_begin(c, body_id, 0)) {
            pc_ui_shell_panel_draw(a, &a->panel_runtime, tabs[cur]);
            nk_group_end(c);
        }
    }
}

/* The content below the menu bar: the canvas on the left, the right dock column on the right. */
static void draw_content(pc_ui_shell_app *a)
{
    struct nk_context *c = &a->ctx;
    struct nk_rect reg = nk_window_get_content_region(c);
    float aw = reg.w, ah = reg.h;
    if (aw < 1.0f || ah < 1.0f) {
        return;
    }
    pc_ui_docking_begin(&a->docking);
    bool dock = a->ui.panels.dock && !pc_ui_view_hides_chrome(&a->ui.view);
    float dock_w = dock ? a->ui.dock_width : 0.f;
    if (dock_w > aw * 0.5f) {
        dock_w = aw * 0.5f;
    }
    bool chrome = !pc_ui_view_hides_chrome(&a->ui.view);
    float tool_w = chrome && a->ui.panels.toolbar ? (a->tokens.pro ? 40.f : 50.f) : 0.f;
    if (tool_w > 0 && a->ui.panels.toolbar_double) {
        tool_w += a->tokens.pro ? 32.f : 38.f;
    }
    float options_h = chrome && a->ui.panels.options_bar
                          ? (a->ui.transform.active || a->ui.tool == PC_TOOL_CROP ? 64.f : 34.f)
                          : 0.f;
    float status_h = chrome && a->ui.panels.status_bar ? 22.f : 0.f;
    float body_h = ah - options_h - status_h;
    if (body_h < 1) {
        body_h = 1;
    }
    float canvas_w = aw - dock_w - tool_w;
    if (canvas_w < 1) {
        canvas_w = 1;
    }

    /* Which groups are shown, in display order; their heights fill the column. */
    pc_ui_group order[PC_UI_GROUP_N];
    pc_ui_dock_order(&a->ui.dock, order);
    pc_ui_group shown[PC_UI_GROUP_N];
    size_t nshown = 0;
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        if (dock && group_shown(a, order[i])) {
            shown[nshown++] = order[i];
        }
    }
    float heights[PC_UI_GROUP_N];
    pc_ui_dock_heights_for(&a->ui.dock, shown, nshown, body_h, (float)STRIP_H, heights);

    nk_layout_space_begin(c, NK_STATIC, ah, (int)nshown + 4);
    if (options_h > 0) {
        nk_layout_space_push(c, nk_rect(0, 0, aw, options_h));
        if (nk_group_begin(c, "options-bar", NK_WINDOW_NO_SCROLLBAR)) {
            pc_ui_shell_options(a, aw);
            nk_group_end(c);
        }
    }
    if (tool_w > 0) {
        struct nk_vec2 group_padding = c->style.window.group_padding;
        c->style.window.group_padding = nk_vec2(2, 2);
        nk_layout_space_push(c, nk_rect(0, options_h, tool_w, body_h));
        if (nk_group_begin(c, "toolbar", NK_WINDOW_NO_SCROLLBAR)) {
            pc_ui_shell_toolbar(a, tool_w, body_h);
            nk_group_end(c);
        }
        c->style.window.group_padding = group_padding;
    }
    if (status_h > 0) {
        nk_layout_space_push(c, nk_rect(tool_w, ah - status_h, aw - tool_w, status_h));
        if (nk_group_begin(c, "status-bar", NK_WINDOW_NO_SCROLLBAR)) {
            pc_ui_shell_status(a);
            nk_group_end(c);
        }
    }
    nk_layout_space_push(c, nk_rect(tool_w, options_h, canvas_w, body_h));
    if (nk_group_begin(c, "work-area", NK_WINDOW_NO_SCROLLBAR)) {
        draw_canvas_body(a);
        nk_group_end(c);
    }
    float y = options_h;
    for (size_t i = 0; i < nshown; i++) {
        float gh = heights[i];
        if (gh < 1.0f) {
            gh = 1.0f;
        }
        nk_layout_space_push(c, nk_rect(tool_w + canvas_w, y, dock_w, gh));
        pc_ui_docking_group(&a->docking, shown[i], nk_rect(reg.x + tool_w + canvas_w, reg.y + y, dock_w, gh),
                            gh);
        char id[32];
        (void)snprintf(id, sizeof id, "dock-%s", pc_ui_group_key(shown[i]));
        if (nk_group_begin_titled(c, id, "", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
            draw_dock_group(a, shown[i], dock_w);
            nk_group_end(c);
        }
        y += gh + PC_UI_DOCK_GAP;
    }
    nk_layout_space_end(c);
    pc_ui_docking_draw(a, &a->docking,
                       dock ? nk_rect(reg.x + tool_w + canvas_w - 3, reg.y + options_h, 6, body_h)
                            : nk_rect(0, 0, 0, 0));
}

/* The lower-right notice stack (toasts/banners). */
static void draw_notices(pc_ui_shell_app *a, float w, float h)
{
    a->notices_hovered = false;
    if (a->notices.n == 0) {
        return;
    }
    struct nk_context *c = &a->ctx;
    float cw = 320.0f;
    if (cw > w - 24.0f) {
        cw = w - 24.0f;
    }
    if (cw < 100 || h < 60)
        return;
    float x = w - cw - 16.0f;
    float y = h - 44.0f;
    for (size_t i = a->notices.n; i-- > 0;) {
        const pc_ui_notice *n = &a->notices.items[i];
        pc_buf title = PC_BUF_INIT;
        pc_ui_notice_text(n, n->title, &title);
        size_t extra = n->n_lines < PC_UI_NOTICE_MAX_LINES ? n->n_lines : PC_UI_NOTICE_MAX_LINES;
        float ch = 30.0f + (float)extra * 16.0f;
        y -= ch + 6.0f;
        char window_id[64];
        (void)snprintf(window_id, sizeof window_id, "notice-%llu", (unsigned long long)n->id);
        if (nk_begin(c, window_id, nk_rect(x, y, cw, ch), NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
            struct nk_command_buffer *cv = nk_window_get_canvas(c);
            struct nk_rect b = nk_window_get_bounds(c);
            nk_fill_rect(cv, b, a->tokens.radius, pc_ui_nk_color(a->tokens.card));
            nk_stroke_rect(cv, b, a->tokens.radius, 1.0f,
                           pc_ui_nk_color(n->error ? a->tokens.warning : a->tokens.card_border));
            a->notices_hovered = a->notices_hovered || nk_input_is_mouse_hovering_rect(&c->input, b);
            nk_layout_row_begin(c, NK_DYNAMIC, 18, 2);
            nk_layout_row_push(c, .9f);
            nk_label_colored(c, pc_buf_cstr(&title), NK_TEXT_LEFT,
                             pc_ui_nk_color(n->error ? a->tokens.warning : a->tokens.text));
            nk_layout_row_push(c, .1f);
            char mapping[80];
            (void)snprintf(mapping, sizeof mapping, "ui.notice.dismiss.%llu", (unsigned long long)n->id);
            pc_ui_shell_widget_mapping(a, mapping, nk_widget_bounds(c));
            if (nk_button_label(c, pc_i18n_t("×"))) {
                yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
                yyjson_mut_val *params = d ? yyjson_mut_obj(d) : NULL;
                if (params && yyjson_mut_obj_add_uint(d, params, "notice", n->id))
                    pc_ui_shell_queue(a, "ui.notice.dismiss", params);
                yyjson_mut_doc_free(d);
            }
            nk_layout_row_end(c);
            for (size_t l = 0; l < extra; l++) {
                pc_buf line = PC_BUF_INIT;
                pc_ui_notice_text(n, n->lines[l], &line);
                nk_layout_row_dynamic(c, 15, 1);
                nk_label_colored(c, pc_buf_cstr(&line), NK_TEXT_LEFT, pc_ui_nk_color(a->tokens.text_dim));
                pc_buf_fini(&line);
            }
        }
        nk_end(c);
        pc_buf_fini(&title);
    }
}

static void build_ui(pc_ui_shell_app *a, float w, float h)
{
    pc_ui_shell_discard_poll(a);
    pc_ui_shell_apply_preferences(a);
    a->panel_runtime.nhits = 0;
    a->canvas.cursor.used = false;
    pc_ui_shell_app_gpu_check(a);
    pc_error e;
    (void)refresh_menu(a, &e);
    struct nk_context *c = &a->ctx;
    if (nk_begin(c, "PhotoCraft", nk_rect(0, 0, w, h), NK_WINDOW_NO_SCROLLBAR)) {
        if (a->ui.n_dialogs || a->jobs.modal) {
            nk_widget_disable_begin(c);
        }
        if (!pc_ui_view_hides_chrome(&a->ui.view) && a->ui.panels.menu_bar) {
            draw_menu_bar(a);
        }
        bool menu_open = pc_ui_menu_nav_is_open(&a->menus.nav);
        if (menu_open) {
            nk_widget_disable_begin(c);
        }
        draw_content(a);
        if (menu_open) {
            nk_widget_disable_end(c);
        }
        if (a->ui.n_dialogs || a->jobs.modal) {
            nk_widget_disable_end(c);
        }
    }
    nk_end(c);
    if (!a->ui.n_dialogs && !a->jobs.modal) {
        pc_ui_shell_type_panels_draw(a);
        pc_ui_brush_panel_draw(a, &a->brush_panel);
        pc_ui_shell_brush_picker_draw(a, &a->brush_picker, w, h);
        pc_ui_shell_timeline_draw(a);
        pc_ui_shell_context_draw(a, w, h);
        pc_ui_native_menu_popups(a, &a->menus, a->items, a->nitems, w, h);
    }
    pc_ui_shell_dialogs_draw(a, &a->dialog_runtime, w, h);
    pc_ui_shell_jobs_draw(a, w, h);
    draw_notices(a, w, h);
    pc_ui_native_cursor_end(&a->canvas.cursor);
    /* Auto-hide notices once their delay is up (the shell has no input time; use the frame as a
     * coarse clock so the stack clears on its own in the interactive loop). */
    bool has_next;
    (void)pc_ui_notices_expire(&a->notices, (double)SDL_GetTicks() / 1000.0, a->notices_hovered, false, true,
                               6.0, &has_next);
}

pc_status pc_ui_shell_app_render_frame(pc_ui_shell_app *a, int fb_w, int fb_h, pc_error *err)
{
    if (!a->have_window || !a->rr_ready) {
        return pc_fail(err, PC_ERR_INVALID, "no window");
    }
    if (a->frame == UINT64_MAX) {
        a->frame = 1;
        for (size_t i = 0; i < 256; i++) {
            a->icon_cache.entries[i].frame = 0;
        }
        for (size_t i = 0; i < 512; i++) {
            a->image_cache.entries[i].frame = 0;
        }
    } else {
        a->frame++;
    }
    pc_ui_shell_jobs_tick(a);
    pc_doc_id checkpoint_owner = pc_gpu_compositor_document(a->canvas.gpu);
    if (checkpoint_owner) {
        size_t count = 0;
        bool open = false;
        const pc_engine_doc_state *docs = pc_engine_session_documents(a->session, &count);
        for (size_t i = 0; i < count; i++)
            if (pc_doc_arc_get(docs[i].doc)->id == checkpoint_owner)
                open = true;
        if (!open)
            pc_gpu_compositor_forget_document(a->canvas.gpu, checkpoint_owner);
    }
    pc_ui_shell_sync_targets(a);
    float lw = (float)fb_w / a->ui_scale;
    float lh = (float)fb_h / a->ui_scale;
    build_ui(a, lw, lh);
    a->gl.glClearColor((float)a->tokens.canvas.r / 255.0f, (float)a->tokens.canvas.g / 255.0f,
                       (float)a->tokens.canvas.b / 255.0f, 1.0f);
    a->gl.glClear(GL_COLOR_BUFFER_BIT);
    pc_status rendered = ui_renderer_draw(&a->rr, &a->ctx, fb_w, fb_h, a->ui_scale, err);
    if (rendered == PC_OK) {
        pc_ui_shell_panel_apply(a, &a->panel_runtime);
    }
    return rendered;
}

/* ---- window / GL ---- */

pc_status pc_ui_shell_app_open_window(pc_ui_shell_app *a, const char *font_path, float forced_scale,
                                      bool hidden, pc_error *err)
{
    if (a->have_window) {
        return PC_OK;
    }
    a->forced_scale = forced_scale;
    free(a->font_path);
    a->font_path = font_path ? SDL_strdup(font_path) : NULL;

    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    a->win = SDL_CreateWindow("PhotoCraft", DEFAULT_W, DEFAULT_H,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                  (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (a->win == NULL) {
        return pc_fail(err, PC_ERR_IO, "SDL_CreateWindow: %s", SDL_GetError());
    }
    a->glc = SDL_GL_CreateContext(a->win);
    if (a->glc == NULL) {
        return pc_fail(err, PC_ERR_IO, "OpenGL 3.3 core context: %s", SDL_GetError());
    }
    const char *missing = NULL;
    if (!gl33_load(&a->gl, &missing)) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "OpenGL function missing: %s", missing ? missing : "");
    }
    a->gl_ready = 1;
    float scale = forced_scale > 0.0f ? forced_scale : SDL_GetWindowDisplayScale(a->win);
    if (!(scale > 0.0f)) {
        scale = 1.0f;
    }
    a->ui_scale = scale;
    const char *fp = a->font_path ? a->font_path : "assets/fonts/Inter-Regular.ttf";
    if (ui_font_init(&a->font, fp, 14.0f, scale, err) != PC_OK) {
        return err ? err->code : PC_ERR_IO;
    }
    a->font_ready = 1;
    if (ui_renderer_init(&a->rr, &a->gl, &a->font, err) != PC_OK) {
        return err ? err->code : PC_ERR_IO;
    }
    a->rr_ready = 1;
    if (!nk_init_default(&a->ctx, &a->font.nk)) {
        return pc_fail(err, PC_ERR_NOMEM, "Nuklear init failed");
    }
    a->ctx_ready = 1;
    pc_ui_apply_nk_style(&a->ctx, &a->tokens);
    (void)SDL_StartTextInput(a->win);
    a->have_window = true;
    a->jobs.enabled = !hidden;
    return PC_OK;
}

/* ---- capture ---- */

/* CPU fallback: a window-sized RGBA image of the theme canvas colour, with the active document's
 * thumbnail composited in centred (so a headless control screenshot is a valid, meaningful PNG). */
static pc_status capture_cpu(pc_ui_shell_app *a, uint8_t **out_rgba, int *out_w, int *out_h, pc_error *err)
{
    int w = (int)(a->win_w + 0.5f);
    int h = (int)(a->win_h + 0.5f);
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    uint8_t *px = pc_malloc_array((size_t)w * (size_t)h, 4);
    if (px == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_ui_color bg = pc_ui_shell_canvas_color(a);
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        px[i * 4 + 0] = bg.r;
        px[i * 4 + 1] = bg.g;
        px[i * 4 + 2] = bg.b;
        px[i * 4 + 3] = 255;
    }
    size_t ndocs = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(a->session, &ndocs);
    size_t active = 0;
    if (pc_engine_session_active_index(a->session, &active) && active < ndocs) {
        const pc_document *doc = pc_doc_arc_get(docs[active].doc);
        pc_doc_arc *floating = NULL;
        pc_status floating_status = pc_engine_floating_document(a->session, &floating, err);
        if (floating_status != PC_OK) {
            free(px);
            return floating_status;
        }
        if (floating)
            doc = pc_doc_arc_get(floating);
        if ((a->canvas.gesture.live || a->canvas.gesture.move_preview) &&
            a->canvas.gesture.document == doc->id && a->canvas.gesture.revision == docs[active].revision) {
            doc =
                pc_doc_arc_get(a->canvas.gesture.live ? pc_engine_live_stroke_document(a->canvas.gesture.live)
                                                      : a->canvas.gesture.move_preview);
        }
        uint32_t side = (uint32_t)(w < h ? w : h);
        if (side > 1) {
            side = (uint32_t)((float)side * 0.6f);
        }
        if (side < 1) {
            side = 1;
        }
        pc_rgba8_image img = {0, 0, NULL};
        pc_error te;
        if (pc_compose_thumbnail(doc, side, &img, &te) == PC_OK && img.pixels != NULL) {
            int ox = (w - (int)img.width) / 2;
            int oy = (h - (int)img.height) / 2;
            for (uint32_t y = 0; y < img.height; y++) {
                int dy = oy + (int)y;
                if (dy < 0 || dy >= h) {
                    continue;
                }
                for (uint32_t x = 0; x < img.width; x++) {
                    int dx = ox + (int)x;
                    if (dx < 0 || dx >= w) {
                        continue;
                    }
                    const uint8_t *s = &img.pixels[((size_t)y * img.width + x) * 4];
                    uint8_t *d = &px[((size_t)dy * (size_t)w + (size_t)dx) * 4];
                    /* Source-over onto the opaque surround. */
                    unsigned sa = s[3];
                    for (int ch = 0; ch < 3; ch++) {
                        d[ch] = (uint8_t)((s[ch] * sa + d[ch] * (255 - sa)) / 255);
                    }
                    d[3] = 255;
                }
            }
        }
        free(img.pixels);
    }
    *out_rgba = px;
    *out_w = w;
    *out_h = h;
    return PC_OK;
}

static pc_status capture_gl(pc_ui_shell_app *a, uint8_t **out_rgba, int *out_w, int *out_h, pc_error *err)
{
    const gl33 *gl = &a->gl;
    int fw = (int)(a->win_w * a->ui_scale + 0.5f);
    int fh = (int)(a->win_h * a->ui_scale + 0.5f);
    if (fw < 1) {
        fw = 1;
    }
    if (fh < 1) {
        fh = 1;
    }
    GLuint fbo = 0, tex = 0;
    uint8_t *px = NULL, *flipped = NULL;
    pc_status rc = PC_ERR_IO;

    gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl->glGenFramebuffers(1, &fbo);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        (void)pc_fail(err, PC_ERR_IO, "framebuffer incomplete");
        goto cleanup;
    }
    for (int frame = 0; frame < 2; frame++) { /* Nuklear lays out on the first frame */
        nk_input_begin(&a->ctx);
        nk_input_end(&a->ctx);
        if (pc_ui_shell_app_render_frame(a, fw, fh, err) != PC_OK) {
            rc = err ? err->code : PC_ERR_IO;
            goto cleanup;
        }
    }
    px = pc_malloc_array((size_t)fw * (size_t)fh, 4);
    flipped = pc_malloc_array((size_t)fw * (size_t)fh, 4);
    if (px == NULL || flipped == NULL) {
        (void)pc_fail(err, PC_ERR_NOMEM, "out of memory");
        rc = PC_ERR_NOMEM;
        goto cleanup;
    }
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < fh; y++) { /* GL rows are bottom-up */
        memcpy(flipped + (size_t)y * (size_t)fw * 4, px + (size_t)(fh - 1 - y) * (size_t)fw * 4,
               (size_t)fw * 4);
    }
    *out_rgba = flipped;
    flipped = NULL;
    *out_w = fw;
    *out_h = fh;
    rc = PC_OK;
cleanup:
    free(px);
    free(flipped);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glDeleteFramebuffers(1, &fbo);
    gl->glDeleteTextures(1, &tex);
    return rc;
}

pc_status pc_ui_shell_app_capture(pc_ui_shell_app *a, uint8_t **out_rgba, int *out_w, int *out_h,
                                  pc_error *err)
{
    if (a->have_window && a->gl_ready && a->rr_ready) {
        return capture_gl(a, out_rgba, out_w, out_h, err);
    }
    return capture_cpu(a, out_rgba, out_w, out_h, err);
}

/* ---- interactive frame loop (defined in main.c's companion input translation) ---- */

int pc_ui_shell_app_run(pc_ui_shell_app *a)
{
    if (!a->have_window) {
        (void)fprintf(stderr, "photocraft-ui-shell: no window\n");
        return 1;
    }
    bool running = true;
    while (running && !a->quit_requested) {
        SDL_Event e;
        int fw, fh;
        pc_error err;
        nk_input_begin(&a->ctx);
        if (!SDL_WaitEventTimeout(&e, 16)) {
            e.type = 0;
        }
        do {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                pc_json_value result = PC_JSON_VALUE_INIT;
                (void)pc_ui_shell_app_invoke(a, "file.exit", NULL, &result, NULL);
                pc_json_value_fini(&result);
                running = !a->quit_requested;
            } else if (e.type != 0) {
                pc_ui_shell_input_event(a, &e);
            }
        } while (running && SDL_PollEvent(&e));
        nk_input_end(&a->ctx);
        if (!SDL_GetWindowSizeInPixels(a->win, &fw, &fh) || fw <= 0 || fh <= 0) {
            continue;
        }
        if (pc_ui_shell_app_render_frame(a, fw, fh, &err) != PC_OK) {
            (void)fprintf(stderr, "photocraft-ui-shell: render: %s\n", err.msg);
            return 1;
        }
        SDL_GL_SwapWindow(a->win);
    }
    return 0;
}

/* These let main.c translate SDL input without reaching into the struct. */
struct nk_context *pc_ui_shell_app_nk(pc_ui_shell_app *a)
{
    return a->ctx_ready ? &a->ctx : NULL;
}
SDL_Window *pc_ui_shell_app_sdl_window(pc_ui_shell_app *a)
{
    return a->win;
}
float pc_ui_shell_app_ui_scale(const pc_ui_shell_app *a)
{
    return a->ui_scale;
}

pc_status pc_ui_shell_app_sync_views(pc_ui_shell_app *a, pc_error *err)
{
    return pc_ui_canvas_sync(a, &a->canvas, err);
}

bool pc_ui_shell_canvas_event(pc_ui_shell_app *a, const SDL_Event *e)
{
    return pc_ui_canvas_view_event(a, &a->canvas, e);
}

bool pc_ui_shell_app_canvas_mapping(const pc_ui_shell_app *a, pc_ui_canvas_xform *out)
{
    if (!a || !out || !a->canvas.laid_out) {
        return false;
    }
    *out = a->canvas.xf;
    return true;
}

static pc_status pointer_impl(pc_ui_shell_app *a, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                              unsigned modifiers, pc_json_value *out, pc_error *err)
{
    if (pc_ui_shell_jobs_locked(a) && a->ui.tool != PC_TOOL_HAND && a->ui.tool != PC_TOOL_ZOOM &&
        a->ui.tool != PC_TOOL_ROTATE_VIEW)
        return pc_fail(err, PC_ERR_INVALID, "wait for the document's job or cancel it first");
    pc_status sync = pc_ui_canvas_sync(a, &a->canvas, err);
    if (sync != PC_OK) {
        return sync;
    }
    pc_ui_view *view = pc_ui_canvas_active_view(a);
    if (kind == PC_UI_POINTER_DOWN || kind == PC_UI_POINTER_UP) {
        a->menu_dirty = true;
    }
    pc_status transform = pc_ui_shell_transform_pointer(a, kind, point, modifiers, out, err);
    if (transform != PC_ERR_NOT_FOUND)
        return transform;
    pc_status layer_picker = pc_ui_layer_picker_pointer(a->session, &a->ui, kind, point, out, err);
    if (layer_picker != PC_ERR_NOT_FOUND)
        return layer_picker;
    pc_status sampling = pc_ui_sampling_pointer(&a->sampling, a->session, &a->ui, kind, point, modifiers,
                                                view ? view->zoom / a->ui_scale : 1, out, err);
    if (sampling != PC_ERR_NOT_FOUND)
        return sampling;
    pc_status crop = pc_ui_shell_crop_pointer(a, kind, point, modifiers, out, err);
    if (crop != PC_ERR_NOT_FOUND)
        return crop;
    pc_status artboard = pc_ui_artboard_pointer(a->session, &a->ui, kind, point, modifiers,
                                                view ? view->zoom / a->ui_scale : 1, out, err);
    if (artboard != PC_ERR_NOT_FOUND)
        return artboard;
    pc_status slice = pc_ui_slice_pointer(a->session, &a->ui, kind, point, out, err);
    if (slice != PC_ERR_NOT_FOUND)
        return slice;
    unsigned handoff = kind == PC_UI_POINTER_DOWN
                           ? pc_ui_selection_move_kind(a->session, &a->ui, point.x, point.y, modifiers)
                       : a->canvas.gesture.active ? a->canvas.gesture.handoff
                                                  : 0;
    if (handoff)
        return pc_ui_tool_gesture_selection_event(&a->canvas.gesture, a->session, &a->ui, handoff, kind,
                                                  point, modifiers, view ? view->zoom / a->ui_scale : 1, out,
                                                  err);
    if (pc_ui_lasso_tool(a->ui.tool)) {
        pc_status lasso = pc_ui_lasso_pointer(a->session, &a->ui, kind, point, modifiers,
                                              view ? view->zoom / a->ui_scale : 1, out, err);
        if (lasso != PC_ERR_NOT_FOUND)
            return lasso;
    }
    if (kind == PC_UI_POINTER_DOWN) {
        pc_status rasterize = pc_ui_shell_rasterize_pointer(a, point, out, err);
        if (rasterize != PC_ERR_NOT_FOUND)
            return rasterize;
    }
    if (kind == PC_UI_POINTER_DOWN && !modifiers) {
        pc_status auto_status =
            pc_ui_pen_auto(a->session, &a->ui, point, view ? view->zoom / a->ui_scale : 1, out, err);
        if (auto_status != PC_ERR_NOT_FOUND)
            return auto_status;
    }
    if (a->ui.direct.active || pc_ui_direct_handles(&a->ui, modifiers)) {
        return pc_ui_direct_pointer(a->session, &a->ui, kind, point, modifiers,
                                    view ? view->zoom / a->ui_scale : 1, out, err);
    }
    if (pc_ui_pen_tool(a->ui.tool) || (a->ui.has_pen && a->ui.pen.dragging)) {
        return pc_ui_pen_pointer(a->session, &a->ui, kind, point, modifiers,
                                 view ? view->zoom / a->ui_scale : 1, out, err);
    }
    if (a->type_pointer.pressed || pc_ui_tool_is_type(a->ui.tool)) {
        return pc_ui_type_pointer_event(&a->type_pointer, a->session, &a->ui, kind, point, modifiers,
                                        view ? view->zoom / a->ui_scale : 1, out, err);
    }
    return pc_ui_tool_gesture_event(&a->canvas.gesture, a->session, &a->ui, kind, point, modifiers,
                                    view ? view->zoom : 1, out, err);
}

pc_ui_snap *pc_ui_shell_snap_runtime(pc_ui_shell_app *app)
{
    return &app->snap;
}
pc_status pc_ui_shell_app_pointer(pc_ui_shell_app *app, pc_ui_pointer_kind kind, pc_ui_tool_point point,
                                  unsigned modifiers, pc_json_value *out, pc_error *err)
{
    pc_status status = pc_ui_canvas_sync(app, &app->canvas, err);
    if (status != PC_OK)
        return status;
    bool sampling = pc_ui_layer_picker_armed(app->session, &app->ui) || app->sampling.active ||
                    pc_ui_sampling_tool_kind(app->ui.tool) ||
                    pc_ui_alt_samples_color(app->ui.tool, modifiers);
    if (sampling)
        pc_ui_snap_fini(&app->snap);
    else
        pc_ui_shell_snap_filter(app, kind, &point, modifiers);
    status = pointer_impl(app, kind, point, modifiers, out, err);
    if (kind == PC_UI_POINTER_DOWN && status == PC_OK && !sampling)
        pc_ui_shell_snap_retarget_move(app, point);
    if (kind == PC_UI_POINTER_UP || kind == PC_UI_POINTER_CANCEL || status != PC_OK)
        pc_ui_snap_fini(&app->snap);
    return status;
}

bool pc_ui_shell_pen_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    return pc_ui_canvas_pen_event(app, &app->canvas, event);
}

pc_ui_type_pointer *pc_ui_shell_type_pointer(pc_ui_shell_app *app)
{
    return &app->type_pointer;
}

pc_ui_type_controls *pc_ui_shell_type_controls(pc_ui_shell_app *app)
{
    return &app->type_controls;
}

pc_ui_type_panels_runtime *pc_ui_shell_type_panels_runtime(pc_ui_shell_app *app)
{
    return &app->type_panels_runtime;
}
pc_status pc_ui_shell_bitmap(pc_ui_shell_app *app, const uint8_t key[32], uint16_t width, uint16_t height,
                             const uint8_t *rgba, struct nk_image *out, pc_error *err)
{
    if (!app->gl_ready) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "UI image needs OpenGL");
    }
    return pc_ui_image_cache_get(&app->image_cache, &app->gl, key, width, height, rgba, app->frame, out, err);
}

bool pc_ui_shell_layers_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    return pc_ui_layer_interaction_event(app, &app->panel_runtime, event);
}
bool pc_ui_shell_canvas_pointer_active(const pc_ui_shell_app *app)
{
    return app->ui.layer_picker.down || app->sampling.active || app->ui.artboard_tool.active ||
           app->ui.slices.dragging || app->ui.lasso.down || app->crop.drag.active ||
           app->crop.straightening || app->transform.dragging || app->panel_runtime.drag.active ||
           app->canvas.brush_resize.active || app->canvas.gesture.active || app->canvas.dragging ||
           app->canvas.guide_drag.active || app->canvas.scroll_drag.active || app->type_pointer.pressed ||
           app->ui.direct.active || (app->ui.has_pen && app->ui.pen.dragging);
}

void pc_ui_shell_input_track(pc_ui_shell_app *app, const SDL_Event *event)
{
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
        pc_ui_key key = pc_ui_shell_input_key(event->key.key);
        if (key > PC_KEY_NONE && key <= PC_KEY_EXCLAMATIONMARK) {
            app->canvas.held_keys[key] = event->key.down;
        }
        app->canvas.held_mods = pc_ui_shell_input_mods(event->key.mod);
    } else if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        memset(app->canvas.held_keys, 0, sizeof app->canvas.held_keys);
        app->canvas.held_mods = 0;
    }
}

pc_ui_dialog_runtime *pc_ui_shell_dialog_runtime(pc_ui_shell_app *app)
{
    return &app->dialog_runtime;
}

bool pc_ui_shell_canvas_preview(pc_ui_shell_app *app, struct nk_image *out, float *aspect)
{
    const pc_ui_canvas_view *v = &app->canvas;
    const pc_engine_doc_state *state = pc_engine_session_active(app->session);
    if (!state || !out || !aspect || !v->target || v->width <= 0 || v->height <= 0 || v->width > 65535 ||
        v->height > 65535 || v->xf.width <= 0 || v->xf.height <= 0)
        return false;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    float min_x = INFINITY, min_y = INFINITY, max_x = -INFINITY, max_y = -INFINITY;
    for (size_t i = 0; i < 4; i++) {
        float point[2];
        if (pc_ui_canvas_to_screen(&v->xf, i & 1 ? (float)doc->size.width : 0,
                                   i & 2 ? (float)doc->size.height : 0, point) != PC_OK)
            return false;
        min_x = fminf(min_x, point[0]);
        max_x = fmaxf(max_x, point[0]);
        min_y = fminf(min_y, point[1]);
        max_y = fmaxf(max_y, point[1]);
    }
    float x0 = fmaxf(0, (min_x - v->xf.x) / v->xf.width), x1 = fminf(1, (max_x - v->xf.x) / v->xf.width),
          y0 = fmaxf(0, (min_y - v->xf.y) / v->xf.height), y1 = fminf(1, (max_y - v->xf.y) / v->xf.height);
    if (x1 <= x0 || y1 <= y0)
        return false;
    struct nk_rect region = nk_rect(x0 * (float)v->width, y0 * (float)v->height, (x1 - x0) * (float)v->width,
                                    (y1 - y0) * (float)v->height);
    *out = nk_subimage_id((int)v->target, (unsigned short)v->width, (unsigned short)v->height, region);
    *aspect = region.w / region.h;
    return true;
}

bool pc_ui_shell_preview_doc_point(pc_ui_shell_app *app, const struct nk_image *image, float u, float v,
                                   float out[2])
{
    const pc_ui_canvas_view *canvas = &app->canvas;
    if (!image || !out || !isfinite(u) || !isfinite(v) || canvas->width <= 0 || canvas->height <= 0)
        return false;
    float x = canvas->xf.x + ((float)image->region[0] + u * (float)image->region[2]) / (float)canvas->width *
                                 canvas->xf.width,
          y = canvas->xf.y + ((float)image->region[1] + v * (float)image->region[3]) / (float)canvas->height *
                                 canvas->xf.height;
    return pc_ui_canvas_to_doc(&canvas->xf, x, y, out) == PC_OK;
}

pc_ui_jobs *pc_ui_shell_jobs_runtime(pc_ui_shell_app *app)
{
    return &app->jobs;
}
void pc_ui_shell_app_background_jobs(pc_ui_shell_app *app, bool enabled)
{
    if (app)
        app->jobs.enabled = enabled;
}
void pc_ui_shell_cancel_canvas_gesture(pc_ui_shell_app *app)
{
    pc_ui_tool_gesture_fini(&app->canvas.gesture);
    app->canvas.brush_resize.active = false;
    pc_ui_direct_cancel(&app->ui.direct);
}
void pc_ui_shell_report(pc_ui_shell_app *app, const char *title, const char *details, bool error)
{
    const char *message = details ? details : title ? title : "";
    char *copy = pc_strndup(message, strlen(message));
    if (copy) {
        free(app->ui.status);
        app->ui.status = copy;
        app->ui.status_error = error;
    }
    const char *lines[] = {message};
    (void)pc_ui_notices_post(&app->notices, title ? title : "PhotoCraft", lines, 1, error, NULL);
}

pc_status pc_ui_shell_notices_json(pc_ui_shell_app *app, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *rows = d ? yyjson_mut_arr(d) : NULL;
    bool ok = rows != NULL;
    for (size_t i = 0; ok && i < app->notices.n; i++) {
        const pc_ui_notice *notice = &app->notices.items[i];
        yyjson_mut_val *row = yyjson_mut_obj(d), *lines = yyjson_mut_arr(d);
        ok = row && lines && yyjson_mut_obj_add_uint(d, row, "id", notice->id) &&
             yyjson_mut_obj_add_strcpy(d, row, "title", notice->title) &&
             yyjson_mut_obj_add_bool(d, row, "error", notice->error);
        for (size_t j = 0; ok && j < notice->n_lines; j++)
            ok = yyjson_mut_arr_add_strcpy(d, lines, notice->lines[j]);
        ok = ok && yyjson_mut_obj_add_val(d, row, "lines", lines) && yyjson_mut_arr_add_val(rows, row);
    }
    pc_status status =
        ok ? pc_json_value_from_mut(rows, out, err) : pc_fail(err, PC_ERR_NOMEM, "notice state");
    yyjson_mut_doc_free(d);
    return status;
}

bool pc_ui_shell_doc_size_event(pc_ui_shell_app *app, const SDL_Event *event)
{
    if (!app->panel_runtime.document_size.pending)
        return false;
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST ||
        (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_ESCAPE)) {
        app->panel_runtime.document_size.pending = false;
        app->panel_runtime.document_size.cancelled = true;
        return event->type == SDL_EVENT_KEY_DOWN;
    }
    return false;
}

void pc_ui_shell_result_warnings(pc_ui_shell_app *app, const char *command, const pc_json_value *result)
{
    if (!command || strncmp(command, "file.", 5))
        return;
    yyjson_mut_val *root = pc_json_value_root(result), *warnings = yyjson_mut_obj_get(root, "warnings");
    const char *lines[128];
    size_t count = 0;
    for (size_t i = 0; i < yyjson_mut_arr_size(warnings) && count < 128; i++) {
        const char *line = yyjson_mut_get_str(yyjson_mut_arr_get(warnings, i));
        if (line && *line)
            lines[count++] = line;
    }
    if (!count)
        return;
    const pc_engine_command *spec = pc_engine_command_find(command);
    pc_buf status = PC_BUF_INIT;
    pc_ui_notices_io_warnings(&app->notices, spec && spec->label ? pc_i18n_t(spec->label) : "File operation",
                              lines, count, &status, &app->ui.status_error);
    char *copy = pc_strndup(pc_buf_cstr(&status), status.len);
    if (copy) {
        free(app->ui.status);
        app->ui.status = copy;
    }
    pc_buf_fini(&status);
}

pc_ui_transform_runtime *pc_ui_shell_transform_runtime(pc_ui_shell_app *app)
{
    return &app->transform;
}

pc_ui_secondary_runtime *pc_ui_shell_secondary_runtime(pc_ui_shell_app *app)
{
    return &app->secondary;
}

pc_ui_crop_runtime *pc_ui_shell_crop_runtime(pc_ui_shell_app *app)
{
    return &app->crop;
}

pc_ui_artboard_layout *pc_ui_shell_artboard_layout(pc_ui_shell_app *app)
{
    return &app->artboard_layout;
}

pc_ui_sampling_tool *pc_ui_shell_sampling_runtime(pc_ui_shell_app *app)
{
    return &app->sampling;
}
