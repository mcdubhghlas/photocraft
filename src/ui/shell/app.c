/* PhotocraftApp shell: see app.h. Reimplementation of the egui app root (crates/ui-egui/src/lib.rs)
 * against SDL3 + OpenGL 3.3 core + Nuklear + FreeType, built on the B3 spike's helpers. */
#include "app.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "codecs/codecs.h"
#include "codecs/image.h"
#include "compose/compose.h"
#include "font.h"   /* spike helper (-Isrc/ui/spike) */
#include "gl33.h"   /* spike helper */
#include "photocraft_nuklear.h"
#include "render.h" /* spike helper */
#include "shell_internal.h"
#include "ui/dock.h"
#include "ui/menus.h"
#include "ui/notices.h"
#include "ui/tab_strip.h"
#include "ui/widgets.h"

enum { DEFAULT_W = 1280, DEFAULT_H = 800, MENU_BAR_H = 26, DOCK_W = 300, STRIP_H = 28 };

struct pc_ui_shell_app {
    pc_engine_session *session; /* owned */
    pc_ui_state ui;             /* U1 UiState */
    pc_ui_tokens tokens;        /* tokens for ui.theme */
    pc_ui_menu_nav nav;
    pc_ui_notices notices;      /* U3: lower-right notice stack */
    uint64_t frame;
    float win_w, win_h; /* UI units (logical) */

    /* Cached menu model, rebuilt when menu_dirty (after invokes / theme / doc changes). */
    pc_ui_menu_item *items;
    size_t nitems;
    bool menu_dirty;

    /* GUI stack (NULL / 0 until a window is opened). */
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

/* ---- lifecycle ---- */

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
    pc_ui_menu_nav_reset(&a->nav);
    a->win_w = (float)DEFAULT_W;
    a->win_h = (float)DEFAULT_H;
    a->ui_scale = 1.0f;
    a->menu_dirty = true;
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

pc_engine_session *pc_ui_shell_app_session(pc_ui_shell_app *a) { return a->session; }
pc_ui_state *pc_ui_shell_app_state(pc_ui_shell_app *a) { return &a->ui; }
const pc_ui_tokens *pc_ui_shell_app_tokens(const pc_ui_shell_app *a) { return &a->tokens; }
uint64_t pc_ui_shell_app_frame(const pc_ui_shell_app *a) { return a->frame; }
pc_ui_menu_nav *pc_ui_shell_app_nav(pc_ui_shell_app *a) { return &a->nav; }

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

/* ---- command routing (shell menus::invoke) ---- */

/* The shell-handled UI commands: theme switching and panel toggles change UiState; the rest of the
 * UI-command table (zoom/rotate/screen-mode, workspace, ...) is handled by its view/panel modules
 * as later chunks land. An engine command runs through the session. */
static pc_status invoke_ui_command(pc_ui_shell_app *a, const char *id, pc_json_value *out)
{
    /* window.theme.<id> selects an interface theme. */
    static const char theme_prefix[] = "window.theme.";
    if (strncmp(id, theme_prefix, sizeof theme_prefix - 1) == 0) {
        pc_ui_theme_kind k;
        if (pc_ui_theme_kind_from_name(id + sizeof theme_prefix - 1, &k)) {
            a->ui.theme = k;
            pc_ui_shell_app_apply_theme(a);
        }
        return PC_OK;
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
        if (slot != NULL) {
            *slot = !*slot;
        }
        return PC_OK;
    }
    /* A toolbar tool-select id activates that tool. */
    pc_ui_tool tool;
    if (pc_ui_tool_from_select_id(id, &tool)) {
        a->ui.tool = tool;
        pc_ui_state_remember_group_tool(&a->ui);
        return PC_OK;
    }
    (void)out;
    return PC_OK; /* a UI command with no shell effect yet: a no-op success */
}

pc_status pc_ui_shell_app_invoke(pc_ui_shell_app *a, const char *id, yyjson_val *params,
                                 pc_json_value *out, pc_error *err)
{
    if (id == NULL || id[0] == '\0') {
        return pc_fail(err, PC_ERR_INVALID, "missing `command`");
    }
    a->menu_dirty = true;
    if (pc_engine_command_find(id) != NULL) {
        return pc_engine_session_execute(a->session, id, params, out, err);
    }
    if (pc_ui_command_find(id) != NULL || pc_ui_is_live(id)) {
        pc_status rc = invoke_ui_command(a, id, out);
        if (rc == PC_OK) {
            out->doc = NULL; /* UI commands have no JSON result */
        }
        return rc;
    }
    return pc_fail(err, PC_ERR_NOT_FOUND, "unknown command `%s`", id);
}

/* ---- menu model cache ---- */

static pc_status refresh_menu(pc_ui_shell_app *a, pc_error *err)
{
    if (!a->menu_dirty && a->items != NULL) {
        return PC_OK;
    }
    pc_ui_menu_item *items = NULL;
    size_t n = 0;
    pc_status rc = pc_ui_menu_items(a->session, &items, &n, err);
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

/* Count the menu's direct children for sizing the dropdown. */
static void draw_menu_for(pc_ui_shell_app *a, const char *top)
{
    struct nk_context *c = &a->ctx;
    for (size_t i = 0; i < a->nitems; i++) {
        const pc_ui_menu_item *it = &a->items[i];
        if (it->depth == 0 || it->path[0] == NULL || strcmp(it->path[0], top) != 0) {
            continue;
        }
        /* One level of flattening: deeper items are prefixed with their submenu name so every
         * command stays reachable and clickable (the real nested popups land with U3's chrome). */
        char label[160];
        if (it->depth >= 2 && it->path[1] != NULL) {
            (void)snprintf(label, sizeof label, "%s > %s", it->path[1], it->label);
        } else {
            (void)snprintf(label, sizeof label, "%s", it->label);
        }
        nk_layout_row_dynamic(c, 22, 1);
        if (it->enabled) {
            if (nk_menu_item_label(c, label, NK_TEXT_LEFT)) {
                pc_json_value r = PC_JSON_VALUE_INIT;
                pc_error e;
                e.msg[0] = '\0';
                if (pc_ui_shell_app_invoke(a, it->id, NULL, &r, &e) != PC_OK && e.msg[0] != '\0') {
                    (void)pc_ui_notices_post(&a->notices, e.msg, NULL, 0, true, NULL);
                }
                pc_json_value_fini(&r);
            }
        } else {
            /* A disabled row: show it dimmed and ignore the click. */
            nk_label_colored(c, label, NK_TEXT_LEFT, nk_rgb(120, 120, 120));
        }
    }
}

static void draw_menu_bar(pc_ui_shell_app *a)
{
    struct nk_context *c = &a->ctx;
    nk_menubar_begin(c);
    nk_layout_row_begin(c, NK_STATIC, (float)MENU_BAR_H - 4.0f, 10);
    for (size_t m = 0; m < 10; m++) {
        const char *top = PC_UI_TOP_MENUS[m];
        nk_layout_row_push(c, 72.0f);
        if (nk_menu_begin_label(c, top, NK_TEXT_LEFT, nk_vec2(320, 560))) {
            draw_menu_for(a, top);
            nk_menu_end(c);
        }
    }
    nk_layout_row_end(c);
    nk_menubar_end(c);
}

/* Is group `g`'s panel currently shown? (panels.<key>) */
static bool group_shown(const pc_ui_shell_app *a, pc_ui_group g)
{
    const pc_ui_panels *p = &a->ui.panels;
    switch (g) {
    case PC_UI_GROUP_COLOR: return p->color;
    case PC_UI_GROUP_PROPERTIES: return p->properties;
    case PC_UI_GROUP_CHARACTER: return p->character;
    case PC_UI_GROUP_NAVIGATOR: return p->navigator;
    case PC_UI_GROUP_HISTORY: return p->history;
    case PC_UI_GROUP_LAYERS: return p->layers;
    default: return false;
    }
}

/* The selected-tab slot for group `g` in UiState.dock_tabs. */
static size_t *group_tab_slot(pc_ui_shell_app *a, pc_ui_group g)
{
    pc_ui_dock_tabs *t = &a->ui.dock_tabs;
    switch (g) {
    case PC_UI_GROUP_COLOR: return &t->color;
    case PC_UI_GROUP_PROPERTIES: return &t->properties;
    case PC_UI_GROUP_CHARACTER: return &t->character;
    case PC_UI_GROUP_NAVIGATOR: return &t->navigator;
    case PC_UI_GROUP_HISTORY: return &t->history;
    case PC_UI_GROUP_LAYERS: return &t->layers;
    default: return &t->layers;
    }
}

/* The active document's canvas surround (the real canvas tiles/rulers/zoom are U4). */
static void draw_canvas_body(pc_ui_shell_app *a)
{
    struct nk_context *c = &a->ctx;
    char line[256];
    size_t ndocs = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(a->session, &ndocs);
    size_t active = 0;
    bool has_active = pc_engine_session_active_index(a->session, &active);

    nk_layout_row_dynamic(c, 22, 1);
    (void)snprintf(line, sizeof line, "PhotoCraft \xc2\xb7 %s \xc2\xb7 Tool: %s \xc2\xb7 %zu open",
                   pc_ui_theme_kind_label(a->ui.theme), pc_ui_tool_label(a->ui.tool), ndocs);
    nk_label(c, line, NK_TEXT_LEFT);

    struct nk_rect r = nk_window_get_content_region(c);
    r.y += 26.0f;
    r.h -= 26.0f;
    if (r.h < 40.0f) {
        r.h = 40.0f;
    }
    struct nk_command_buffer *cv = nk_window_get_canvas(c);
    nk_fill_rect(cv, r, 0, pc_ui_nk_color(a->tokens.canvas));
    if (has_active && active < ndocs) {
        const pc_document *doc = pc_doc_arc_get(docs[active].doc);
        (void)snprintf(line, sizeof line, "%u \xc3\x97 %u px", doc->size.width, doc->size.height);
        float cw = r.w * 0.6f, ch = r.h * 0.6f;
        struct nk_rect card = nk_rect(r.x + (r.w - cw) * 0.5f, r.y + (r.h - ch) * 0.5f, cw, ch);
        nk_fill_rect(cv, card, a->tokens.radius, nk_rgb(255, 255, 255));
        nk_stroke_rect(cv, card, a->tokens.radius, 1.0f, pc_ui_nk_color(a->tokens.card_border));
        nk_draw_text(cv, nk_rect(card.x + 8, card.y + 8, card.w - 16, 20), line, (int)strlen(line),
                     &a->font.nk, nk_rgb(255, 255, 255), pc_ui_nk_color(a->tokens.text));
    } else {
        const char *hint = "Open or create a document (File > New)";
        nk_draw_text(cv, nk_rect(r.x + 16, r.y + 16, r.w - 32, 20), hint, (int)strlen(hint),
                     &a->font.nk, pc_ui_nk_color(a->tokens.canvas), pc_ui_nk_color(a->tokens.text_dim));
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
        if (nk_button_label(c, tabs[tab])) {
            *selected = tab;
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
        /* Body: a placeholder naming the active panel (the real bodies land with U5+). */
        char line[128];
        size_t cur = *selected < ntabs ? *selected : 0;
        (void)snprintf(line, sizeof line, "%s", tabs[cur]);
        nk_layout_row_dynamic(c, 20, 1);
        nk_label(c, line, NK_TEXT_LEFT);
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
    float dock_w = (float)DOCK_W;
    if (dock_w > aw * 0.5f) {
        dock_w = aw * 0.5f;
    }
    float canvas_w = aw - dock_w;

    /* Which groups are shown, in display order; their heights fill the column. */
    pc_ui_group order[PC_UI_GROUP_N];
    pc_ui_dock_order(&a->ui.dock, order);
    pc_ui_group shown[PC_UI_GROUP_N];
    size_t nshown = 0;
    for (size_t i = 0; i < PC_UI_GROUP_N; i++) {
        if (group_shown(a, order[i])) {
            shown[nshown++] = order[i];
        }
    }
    float heights[PC_UI_GROUP_N];
    pc_ui_dock_heights_for(&a->ui.dock, shown, nshown, ah, (float)STRIP_H, heights);

    nk_layout_space_begin(c, NK_STATIC, ah, (int)nshown + 1);
    nk_layout_space_push(c, nk_rect(0, 0, canvas_w, ah));
    if (nk_group_begin(c, "work-area", NK_WINDOW_NO_SCROLLBAR)) {
        draw_canvas_body(a);
        nk_group_end(c);
    }
    float y = 0.0f;
    for (size_t i = 0; i < nshown; i++) {
        float gh = heights[i];
        if (gh < 1.0f) {
            gh = 1.0f;
        }
        nk_layout_space_push(c, nk_rect(canvas_w, y, dock_w, gh));
        char id[32];
        (void)snprintf(id, sizeof id, "dock-%s", pc_ui_group_key(shown[i]));
        if (nk_group_begin_titled(c, id, "", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
            draw_dock_group(a, shown[i], dock_w);
            nk_group_end(c);
        }
        y += gh + PC_UI_DOCK_GAP;
    }
    nk_layout_space_end(c);
}

/* The lower-right notice stack (toasts/banners). */
static void draw_notices(pc_ui_shell_app *a, float w, float h)
{
    if (a->notices.n == 0) {
        return;
    }
    struct nk_context *c = &a->ctx;
    float cw = 320.0f;
    if (cw > w - 24.0f) {
        cw = w - 24.0f;
    }
    float x = w - cw - 16.0f;
    float y = h - 44.0f;
    for (size_t i = a->notices.n; i-- > 0;) {
        const pc_ui_notice *n = &a->notices.items[i];
        pc_buf title = PC_BUF_INIT;
        pc_ui_notice_text(n, n->title, &title);
        size_t extra = n->n_lines < PC_UI_NOTICE_MAX_LINES ? n->n_lines : PC_UI_NOTICE_MAX_LINES;
        float ch = 30.0f + (float)extra * 16.0f;
        y -= ch + 6.0f;
        if (nk_begin(c, pc_buf_cstr(&title),
                     nk_rect(x, y, cw, ch),
                     NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
            struct nk_command_buffer *cv = nk_window_get_canvas(c);
            struct nk_rect b = nk_window_get_bounds(c);
            nk_fill_rect(cv, b, a->tokens.radius, pc_ui_nk_color(a->tokens.card));
            nk_stroke_rect(cv, b, a->tokens.radius, 1.0f,
                           pc_ui_nk_color(n->error ? a->tokens.warning : a->tokens.card_border));
            nk_layout_row_dynamic(c, 18, 1);
            nk_label_colored(c, pc_buf_cstr(&title), NK_TEXT_LEFT,
                             pc_ui_nk_color(n->error ? a->tokens.warning : a->tokens.text));
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
    pc_error e;
    (void)refresh_menu(a, &e);
    struct nk_context *c = &a->ctx;
    if (nk_begin(c, "PhotoCraft", nk_rect(0, 0, w, h), NK_WINDOW_NO_SCROLLBAR)) {
        draw_menu_bar(a);
        draw_content(a);
    }
    nk_end(c);
    draw_notices(a, w, h);
    /* Auto-hide notices once their delay is up (the shell has no input time; use the frame as a
     * coarse clock so the stack clears on its own in the interactive loop). */
    bool has_next;
    (void)pc_ui_notices_expire(&a->notices, (double)a->frame / 60.0, false, false, true, 6.0, &has_next);
}

pc_status pc_ui_shell_app_render_frame(pc_ui_shell_app *a, int fb_w, int fb_h, pc_error *err)
{
    if (!a->have_window || !a->rr_ready) {
        return pc_fail(err, PC_ERR_INVALID, "no window");
    }
    float lw = (float)fb_w / a->ui_scale;
    float lh = (float)fb_h / a->ui_scale;
    build_ui(a, lw, lh);
    a->gl.glClearColor((float)a->tokens.canvas.r / 255.0f, (float)a->tokens.canvas.g / 255.0f,
                       (float)a->tokens.canvas.b / 255.0f, 1.0f);
    a->gl.glClear(GL_COLOR_BUFFER_BIT);
    return ui_renderer_draw(&a->rr, &a->ctx, fb_w, fb_h, a->ui_scale, err);
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
    a->have_window = true;
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
    pc_ui_color bg = a->tokens.canvas;
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
    while (running) {
        SDL_Event e;
        int fw, fh;
        pc_error err;
        nk_input_begin(&a->ctx);
        if (!SDL_WaitEventTimeout(&e, 16)) {
            e.type = 0;
        }
        do {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if (e.type != 0) {
                pc_ui_shell_input_event(a, &e);
            }
        } while (running && SDL_PollEvent(&e));
        nk_input_end(&a->ctx);
        if (!SDL_GetWindowSizeInPixels(a->win, &fw, &fh) || fw <= 0 || fh <= 0) {
            continue;
        }
        a->frame++;
        if (pc_ui_shell_app_render_frame(a, fw, fh, &err) != PC_OK) {
            (void)fprintf(stderr, "photocraft-ui-shell: render: %s\n", err.msg);
            return 1;
        }
        SDL_GL_SwapWindow(a->win);
    }
    return 0;
}

/* These let main.c translate SDL input without reaching into the struct. */
struct nk_context *pc_ui_shell_app_nk(pc_ui_shell_app *a) { return a->ctx_ready ? &a->ctx : NULL; }
SDL_Window *pc_ui_shell_app_sdl_window(pc_ui_shell_app *a) { return a->win; }
float pc_ui_shell_app_ui_scale(const pc_ui_shell_app *a) { return a->ui_scale; }
