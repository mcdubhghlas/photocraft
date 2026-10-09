/* B3 GUI stack spike (REWRITE_PLAN.md D4): SDL3 window + OpenGL 3.3 core + Nuklear, FreeType
 * text at HiDPI, SDL3 pen pressure/tilt, SDL3 file dialog.
 *
 *   photocraft-spike [--font PATH] [--scale S]          interactive window
 *   photocraft-spike --snapshot OUT.png [--scale S]     render one frame offscreen (hidden window,
 *                                                       no focus stealing) and exit
 *
 * Not product code: U1 builds the real shell from what this proves. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "font.h"
#include "gl33.h"
#include "png.h"
#include "render.h"

enum { WIN_W = 900, WIN_H = 640, MAX_DOTS = 8192 };

typedef struct dot {
    float x, y, r; /* canvas-relative UI units */
} dot;

typedef struct app {
    SDL_Window *win;
    SDL_GLContext glc;
    gl33 gl;
    ui_font font;
    ui_renderer rr;
    struct nk_context ctx;
    int ctx_ready;
    float ui_scale;      /* framebuffer pixels per UI unit */
    float scale_forced;  /* --scale, or 0 */
    const char *font_path;
    Uint32 dialog_event; /* user event type carrying a chosen path */
    /* demo state */
    float pressure, tilt_x, tilt_y;
    int pen_down, pen_seen;
    char opened[1024];
    char name[64];
    int snap;
    float opacity;
    int blend;
    dot *dots;
    size_t ndots;
    struct nk_rect canvas; /* where the canvas was last laid out, UI units */
} app;

static const char *const BLEND_MODES[] = {"Normal", "Dissolve", "Darken", "Multiply", "Color Burn",
                                          "Linear Burn", "Lighten", "Screen", "Color Dodge", "Overlay"};

static void set_theme(struct nk_context *ctx)
{
    /* Close to Photoshop's dark workspace; the real values come from theme tokens in U1. */
    struct nk_color t[NK_COLOR_COUNT];
    t[NK_COLOR_TEXT] = nk_rgb(222, 222, 222);
    t[NK_COLOR_WINDOW] = nk_rgb(50, 50, 50);
    t[NK_COLOR_HEADER] = nk_rgb(40, 40, 40);
    t[NK_COLOR_BORDER] = nk_rgb(30, 30, 30);
    t[NK_COLOR_BUTTON] = nk_rgb(72, 72, 72);
    t[NK_COLOR_BUTTON_HOVER] = nk_rgb(86, 86, 86);
    t[NK_COLOR_BUTTON_ACTIVE] = nk_rgb(56, 120, 220);
    t[NK_COLOR_TOGGLE] = nk_rgb(36, 36, 36);
    t[NK_COLOR_TOGGLE_HOVER] = nk_rgb(46, 46, 46);
    t[NK_COLOR_TOGGLE_CURSOR] = nk_rgb(56, 120, 220);
    t[NK_COLOR_SELECT] = nk_rgb(60, 60, 60);
    t[NK_COLOR_SELECT_ACTIVE] = nk_rgb(56, 120, 220);
    t[NK_COLOR_SLIDER] = nk_rgb(36, 36, 36);
    t[NK_COLOR_SLIDER_CURSOR] = nk_rgb(200, 200, 200);
    t[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgb(230, 230, 230);
    t[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgb(255, 255, 255);
    t[NK_COLOR_PROPERTY] = nk_rgb(36, 36, 36);
    t[NK_COLOR_EDIT] = nk_rgb(36, 36, 36);
    t[NK_COLOR_EDIT_CURSOR] = nk_rgb(222, 222, 222);
    t[NK_COLOR_COMBO] = nk_rgb(36, 36, 36);
    t[NK_COLOR_CHART] = nk_rgb(36, 36, 36);
    t[NK_COLOR_CHART_COLOR] = nk_rgb(56, 120, 220);
    t[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgb(255, 255, 255);
    t[NK_COLOR_SCROLLBAR] = nk_rgb(44, 44, 44);
    t[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgb(90, 90, 90);
    t[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgb(110, 110, 110);
    t[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgb(130, 130, 130);
    t[NK_COLOR_TAB_HEADER] = nk_rgb(40, 40, 40);
    t[NK_COLOR_KNOB] = nk_rgb(36, 36, 36);
    t[NK_COLOR_KNOB_CURSOR] = nk_rgb(200, 200, 200);
    t[NK_COLOR_KNOB_CURSOR_HOVER] = nk_rgb(230, 230, 230);
    t[NK_COLOR_KNOB_CURSOR_ACTIVE] = nk_rgb(255, 255, 255);
    nk_style_from_table(ctx, t);
}

/* ---- SDL -> Nuklear input ---- */

static float to_ui(const app *a, float window_coord)
{
    /* SDL event coordinates are window coordinates; convert to UI units. */
    return window_coord * SDL_GetWindowPixelDensity(a->win) / a->ui_scale;
}

static void key(app *a, SDL_Keycode k, SDL_Keymod mod, bool down)
{
    bool ctrl = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    struct nk_context *c = &a->ctx;
    switch (k) {
    case SDLK_BACKSPACE: nk_input_key(c, NK_KEY_BACKSPACE, down); break;
    case SDLK_DELETE: nk_input_key(c, NK_KEY_DEL, down); break;
    case SDLK_RETURN: nk_input_key(c, NK_KEY_ENTER, down); break;
    case SDLK_TAB: nk_input_key(c, NK_KEY_TAB, down); break;
    case SDLK_LEFT: nk_input_key(c, NK_KEY_LEFT, down); break;
    case SDLK_RIGHT: nk_input_key(c, NK_KEY_RIGHT, down); break;
    case SDLK_HOME: nk_input_key(c, NK_KEY_TEXT_START, down); break;
    case SDLK_END: nk_input_key(c, NK_KEY_TEXT_END, down); break;
    case SDLK_A: nk_input_key(c, NK_KEY_TEXT_SELECT_ALL, down && ctrl); break;
    case SDLK_C: nk_input_key(c, NK_KEY_COPY, down && ctrl); break;
    case SDLK_V: nk_input_key(c, NK_KEY_PASTE, down && ctrl); break;
    case SDLK_X: nk_input_key(c, NK_KEY_CUT, down && ctrl); break;
    case SDLK_Z: nk_input_key(c, NK_KEY_TEXT_UNDO, down && ctrl); break;
    default: break;
    }
}

static void add_dot(app *a, float wx, float wy)
{
    float x = to_ui(a, wx) - a->canvas.x;
    float y = to_ui(a, wy) - a->canvas.y;
    if (a->ndots >= MAX_DOTS || x < 0 || y < 0 || x > a->canvas.w || y > a->canvas.h) {
        return;
    }
    a->dots[a->ndots].x = x;
    a->dots[a->ndots].y = y;
    a->dots[a->ndots].r = 1.0f + 9.0f * a->pressure;
    a->ndots++;
}

static void SDLCALL on_dialog(void *userdata, const char *const *files, int filter)
{
    /* May run on another thread: hand the result to the event loop. */
    app *a = userdata;
    SDL_Event ev;
    (void)filter;
    if (files == NULL || files[0] == NULL) {
        return; /* error or cancelled */
    }
    SDL_zero(ev);
    ev.type = a->dialog_event;
    ev.user.data1 = SDL_strdup(files[0]);
    if (ev.user.data1 == NULL || !SDL_PushEvent(&ev)) {
        SDL_free(ev.user.data1);
    }
}

/* Returns false on quit. */
static bool handle_event(app *a, const SDL_Event *e)
{
    struct nk_context *c = &a->ctx;
    switch (e->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return false;
    case SDL_EVENT_MOUSE_MOTION: nk_input_motion(c, (int)to_ui(a, e->motion.x), (int)to_ui(a, e->motion.y)); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        enum nk_buttons b = e->button.button == SDL_BUTTON_RIGHT    ? NK_BUTTON_RIGHT
                            : e->button.button == SDL_BUTTON_MIDDLE ? NK_BUTTON_MIDDLE
                                                                    : NK_BUTTON_LEFT;
        int x = (int)to_ui(a, e->button.x);
        int y = (int)to_ui(a, e->button.y);
        if (b == NK_BUTTON_LEFT && e->button.clicks > 1 && e->button.down) {
            nk_input_button(c, NK_BUTTON_DOUBLE, x, y, true);
        }
        nk_input_button(c, b, x, y, e->button.down);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: nk_input_scroll(c, nk_vec2(e->wheel.x, e->wheel.y)); break;
    case SDL_EVENT_TEXT_INPUT: {
        const char *t = e->text.text;
        int len = (int)strlen(t);
        int i = 0;
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
    case SDL_EVENT_KEY_UP: key(a, e->key.key, e->key.mod, e->key.down); break;
    case SDL_EVENT_PEN_PROXIMITY_IN: a->pen_seen = 1; break;
    case SDL_EVENT_PEN_DOWN:
        a->pen_down = 1;
        add_dot(a, e->ptouch.x, e->ptouch.y);
        break;
    case SDL_EVENT_PEN_UP: a->pen_down = 0; break;
    case SDL_EVENT_PEN_MOTION:
        a->pen_seen = 1;
        if (a->pen_down) {
            add_dot(a, e->pmotion.x, e->pmotion.y);
        }
        break;
    case SDL_EVENT_PEN_AXIS:
        if (e->paxis.axis == SDL_PEN_AXIS_PRESSURE) {
            a->pressure = e->paxis.value;
        } else if (e->paxis.axis == SDL_PEN_AXIS_XTILT) {
            a->tilt_x = e->paxis.value;
        } else if (e->paxis.axis == SDL_PEN_AXIS_YTILT) {
            a->tilt_y = e->paxis.value;
        }
        break;
    default:
        if (e->type == a->dialog_event) {
            (void)snprintf(a->opened, sizeof a->opened, "%s", (const char *)e->user.data1);
            SDL_free(e->user.data1);
        }
        break;
    }
    return true;
}

/* ---- the UI ---- */

static void draw_ui(app *a, float w, float h)
{
    struct nk_context *c = &a->ctx;
    static const SDL_DialogFileFilter filters[] = {{"Images", "psd;psb;tif;tiff;png;jpg;jpeg;webp"},
                                                   {"All files", "*"}};
    if (nk_begin(c, "PhotoCraft - GUI stack spike", nk_rect(0, 0, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        char line[1200];
        nk_layout_row_dynamic(c, 22, 1);
        (void)snprintf(line, sizeof line, "Inter via FreeType, %.0f px per UI unit x %.2f scale = %.0f px",
                       (double)a->font.size, (double)a->ui_scale, (double)(a->font.size * a->ui_scale));
        nk_label(c, line, NK_TEXT_LEFT);
        nk_label(c, "\xc3\x89" "bauche \xc2\xb7 na\xc3\xafve fa\xc3\xa7" "ade \xc2\xb7 \xc3\x9c" "bergr\xc3\xb6\xc3\x9f" "e \xc2\xb7 "
                    "\xc5\x92uvre \xe2\x80\x94 \xe2\x80\x9cquotes\xe2\x80\x9d \xe2\x80\xa6 1\xe2\x80\x93" "2 \xe2\x82\xac",
                 NK_TEXT_LEFT);

        nk_layout_row_begin(c, NK_STATIC, 28, 2);
        nk_layout_row_push(c, 110);
        if (nk_button_label(c, "Open\xe2\x80\xa6")) {
            SDL_ShowOpenFileDialog(on_dialog, a, a->win, filters, 2, NULL, false);
        }
        nk_layout_row_push(c, w - 150);
        (void)snprintf(line, sizeof line, "Opened: %s", a->opened[0] != '\0' ? a->opened : "(nothing yet)");
        nk_label(c, line, NK_TEXT_LEFT);
        nk_layout_row_end(c);

        nk_layout_row_dynamic(c, 26, 3);
        nk_checkbox_label(c, "Snap", &a->snap);
        nk_property_float(c, "Opacity:", 0.0f, &a->opacity, 100.0f, 1.0f, 0.5f);
        a->blend = nk_combo(c, BLEND_MODES, (int)(sizeof BLEND_MODES / sizeof BLEND_MODES[0]), a->blend, 22,
                            nk_vec2(200, 240));

        nk_layout_row_dynamic(c, 26, 2);
        nk_label(c, "Layer name:", NK_TEXT_LEFT);
        nk_edit_string_zero_terminated(c, NK_EDIT_FIELD, a->name, (int)sizeof a->name, nk_filter_default);

        nk_layout_row_dynamic(c, 22, 1);
        (void)snprintf(line, sizeof line, "Pen: %s  pressure %.3f  tilt %.1f / %.1f",
                       !a->pen_seen ? "not seen yet" : (a->pen_down ? "down" : "up"), (double)a->pressure,
                       (double)a->tilt_x, (double)a->tilt_y);
        nk_label(c, line, NK_TEXT_LEFT);
        {
            nk_size p = (nk_size)(a->pressure * 1000.0f + 0.5f);
            nk_progress(c, &p, 1000, NK_FIXED);
        }

        nk_layout_row_dynamic(c, h - 290 > 80 ? h - 290 : 80, 1);
        {
            struct nk_command_buffer *canvas = nk_window_get_canvas(c);
            struct nk_rect r;
            size_t i;
            if (nk_widget(&r, c) != NK_WIDGET_INVALID) {
                a->canvas = r;
                nk_fill_rect(canvas, r, 0, nk_rgb(255, 255, 255));
                for (i = 0; i < a->ndots; i++) {
                    const dot *d = &a->dots[i];
                    nk_fill_circle(canvas, nk_rect(r.x + d->x - d->r, r.y + d->y - d->r, 2 * d->r, 2 * d->r),
                                   nk_rgba(20, 20, 20, 230));
                }
                nk_draw_text(canvas, nk_rect(r.x + 8, r.y + 6, r.w - 16, 20), "Draw here with a pen", 20,
                             &a->font.nk, nk_rgb(255, 255, 255), nk_rgb(120, 120, 120));
            }
        }
    }
    nk_end(c);
}

/* ---- setup / frame ---- */

static int fail(const char *what, const char *detail)
{
    (void)fprintf(stderr, "photocraft-spike: %s: %s\n", what, detail != NULL ? detail : "");
    return 1;
}

static void shutdown_app(app *a)
{
    if (a->ctx_ready) {
        nk_free(&a->ctx);
    }
    if (a->glc != NULL) {
        ui_renderer_fini(&a->rr);
        SDL_GL_DestroyContext(a->glc);
    }
    ui_font_fini(&a->font);
    if (a->win != NULL) {
        SDL_DestroyWindow(a->win);
    }
    free(a->dots);
    SDL_Quit();
}

static int render_snapshot(app *a, const char *out)
{
    const gl33 *gl = &a->gl;
    int fw = (int)((float)WIN_W * a->ui_scale + 0.5f);
    int fh = (int)((float)WIN_H * a->ui_scale + 0.5f);
    GLuint fbo = 0;
    GLuint tex = 0;
    uint8_t *px = NULL;
    uint8_t *flipped = NULL;
    pc_error err;
    int rc = 1;
    int frame;
    int y;

    gl->glGenTextures(1, &tex);
    gl->glBindTexture(GL_TEXTURE_2D, tex);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    gl->glGenFramebuffers(1, &fbo);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        rc = fail("snapshot", "framebuffer incomplete");
        goto cleanup;
    }
    /* Scripted state, so the snapshot shows every feature. */
    a->pen_seen = 1;
    a->pressure = 0.42f;
    a->tilt_x = 12.5f;
    a->tilt_y = -4.0f;
    a->opacity = 75.0f;
    a->snap = 1;
    a->blend = 3;
    (void)snprintf(a->opened, sizeof a->opened, "/home/user/Pictures/example.psd");
    (void)snprintf(a->name, sizeof a->name, "Background copy");
    for (frame = 0; frame < 3; frame++) { /* Nuklear lays out on the first frame */
        nk_input_begin(&a->ctx);
        nk_input_end(&a->ctx);
        draw_ui(a, WIN_W, WIN_H);
        if (frame == 0) {
            int i;
            for (i = 0; i < 300 && a->ndots < MAX_DOTS; i++) { /* a pressure-varying S curve */
                float t = (float)i / 299.0f;
                a->dots[a->ndots].x = 40.0f + t * (a->canvas.w - 80.0f);
                a->dots[a->ndots].y = a->canvas.h * (0.5f + 0.3f * SDL_sinf(t * 6.2832f));
                a->dots[a->ndots].r = 1.0f + 9.0f * SDL_sinf(t * 3.1416f);
                a->ndots++;
            }
        }
        gl->glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        if (ui_renderer_draw(&a->rr, &a->ctx, fw, fh, a->ui_scale, &err) != PC_OK) {
            rc = fail("render", err.msg);
            goto cleanup;
        }
    }
    px = pc_malloc_array((size_t)fw * (size_t)fh, 4);
    flipped = pc_malloc_array((size_t)fw * (size_t)fh, 4);
    if (px == NULL || flipped == NULL) {
        rc = fail("snapshot", "out of memory");
        goto cleanup;
    }
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (y = 0; y < fh; y++) { /* GL rows are bottom-up */
        memcpy(flipped + (size_t)y * (size_t)fw * 4, px + (size_t)(fh - 1 - y) * (size_t)fw * 4, (size_t)fw * 4);
    }
    if (spike_png_write(out, flipped, (uint32_t)fw, (uint32_t)fh, (size_t)fw * 4, &err) != PC_OK) {
        rc = fail("snapshot", err.msg);
        goto cleanup;
    }
    (void)printf("wrote %s (%dx%d, scale %.2f, GL %s)\n", out, fw, fh, (double)a->ui_scale,
                 (const char *)gl->glGetString(GL_VERSION));
    rc = 0;
cleanup:
    free(px);
    free(flipped);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glDeleteFramebuffers(1, &fbo);
    gl->glDeleteTextures(1, &tex);
    return rc;
}

/* The window moved to a display with another scale: rebuild the atlas so text stays crisp. */
static pc_status rescale(app *a, float scale, pc_error *err)
{
    ui_font nf;
    pc_status s = ui_font_init(&nf, a->font_path, 14.0f, scale, err);
    if (s != PC_OK) {
        return s;
    }
    ui_renderer_fini(&a->rr);
    ui_font_fini(&a->font);
    a->font = nf;
    a->font.nk.userdata = nk_handle_ptr(&a->font); /* it pointed at the local copy */
    a->ui_scale = scale;
    s = ui_renderer_init(&a->rr, &a->gl, &a->font, err);
    if (s == PC_OK) {
        nk_style_set_font(&a->ctx, &a->font.nk);
    }
    return s;
}

static int run_interactive(app *a)
{
    bool running = true;
    while (running) {
        SDL_Event e;
        int fw;
        int fh;
        float lw;
        float lh;
        pc_error err;
        nk_input_begin(&a->ctx);
        if (!SDL_WaitEventTimeout(&e, 16)) {
            e.type = 0;
        }
        do {
            if (e.type != 0 && !handle_event(a, &e)) {
                running = false;
            }
        } while (running && SDL_PollEvent(&e));
        nk_input_end(&a->ctx);
        if (!SDL_GetWindowSizeInPixels(a->win, &fw, &fh) || fw <= 0 || fh <= 0) {
            continue;
        }
        if (a->scale_forced <= 0.0f) {
            float s = SDL_GetWindowDisplayScale(a->win);
            if (s > 0.0f && s != a->ui_scale && rescale(a, s, &err) != PC_OK) {
                return fail("rescale", err.msg);
            }
        }
        lw = (float)fw / a->ui_scale;
        lh = (float)fh / a->ui_scale;
        draw_ui(a, lw, lh);
        a->gl.glClearColor(0.12f, 0.12f, 0.12f, 1.0f);
        a->gl.glClear(GL_COLOR_BUFFER_BIT);
        if (ui_renderer_draw(&a->rr, &a->ctx, fw, fh, a->ui_scale, &err) != PC_OK) {
            return fail("render", err.msg);
        }
        SDL_GL_SwapWindow(a->win);
    }
    return 0;
}

int main(int argc, char **argv)
{
    app a;
    const char *snapshot = NULL;
    const char *font_path = "assets/fonts/Inter-Regular.ttf";
    const char *missing = NULL;
    pc_error err;
    int i;
    int rc;

    memset(&a, 0, sizeof a);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            snapshot = argv[++i];
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            font_path = argv[++i];
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            a.scale_forced = (float)SDL_strtod(argv[++i], NULL);
            if (!(a.scale_forced >= 0.5f && a.scale_forced <= 4.0f)) {
                return fail("--scale", "must be between 0.5 and 4");
            }
        } else {
            (void)fprintf(stderr, "usage: %s [--snapshot OUT.png] [--scale S] [--font PATH]\n", argv[0]);
            return 2;
        }
    }
    a.dots = pc_calloc_array(MAX_DOTS, sizeof *a.dots);
    if (a.dots == NULL) {
        return fail("startup", "out of memory");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        free(a.dots);
        return fail("SDL_Init", SDL_GetError());
    }
    a.dialog_event = SDL_RegisterEvents(1);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); /* macOS */
    a.win = SDL_CreateWindow("PhotoCraft spike", WIN_W, WIN_H,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                 (snapshot != NULL ? SDL_WINDOW_HIDDEN : 0));
    if (a.win == NULL) {
        rc = fail("SDL_CreateWindow", SDL_GetError());
        goto done;
    }
    a.glc = SDL_GL_CreateContext(a.win);
    if (a.glc == NULL) {
        rc = fail("OpenGL 3.3 core context", SDL_GetError());
        goto done;
    }
    (void)SDL_GL_SetSwapInterval(1);
    if (!gl33_load(&a.gl, &missing)) {
        rc = fail("OpenGL function missing", missing);
        goto done;
    }
    a.ui_scale = a.scale_forced > 0.0f ? a.scale_forced : SDL_GetWindowDisplayScale(a.win);
    if (!(a.ui_scale > 0.0f)) {
        a.ui_scale = 1.0f;
    }
    a.font_path = font_path;
    if (ui_font_init(&a.font, font_path, 14.0f, a.ui_scale, &err) != PC_OK) {
        rc = fail("font", err.msg);
        goto done;
    }
    if (ui_renderer_init(&a.rr, &a.gl, &a.font, &err) != PC_OK) {
        rc = fail("renderer", err.msg);
        goto done;
    }
    if (!nk_init_default(&a.ctx, &a.font.nk)) {
        rc = fail("Nuklear", "init failed");
        goto done;
    }
    a.ctx_ready = 1;
    set_theme(&a.ctx);
    a.opacity = 100.0f;
    if (snapshot != NULL) {
        rc = render_snapshot(&a, snapshot);
    } else {
        (void)SDL_StartTextInput(a.win);
        rc = run_interactive(&a);
    }
done:
    shutdown_app(&a);
    return rc;
}
