/* photocraft-ui-snapshot (REWRITE_PLAN.md D23): render one themed widget panel offscreen and
 * write it as a PNG, so U1 and later chunks can verify the look without a display.
 *
 *   photocraft-ui-snapshot --snapshot OUT.png [--scale S] [--theme ID] [--font PATH]
 *
 * It follows the photocraft-spike --snapshot pattern (hidden SDL3 window, GL 3.3 core, the
 * SDL_VIDEO_DRIVER=offscreen driver for headless CI), but drives Nuklear through the real theme
 * tokens (ui/widgets.h) instead of the spike's hard-coded palette, so the snapshot shows what a
 * given theme looks like. The GL/font/PNG/renderer helpers are shared with the spike. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "ui/spike/font.h"
#include "ui/spike/gl33.h"
#include "ui/spike/png.h"
#include "ui/spike/render.h"
#include "ui/theme.h"
#include "ui/widgets.h"

enum { WIN_W = 560, WIN_H = 470 };

static const char *const BLEND_MODES[] = {"Normal", "Multiply", "Screen", "Overlay", "Soft Light"};

static int fail(const char *what, const char *detail)
{
    (void)fprintf(stderr, "photocraft-ui-snapshot: %s: %s\n", what, detail ? detail : "");
    return 1;
}

/* A representative panel: labels, buttons, a checkbox, a property, a combo, an edit field, a
 * progress bar, a colour swatch and a grouped card. Every widget is styled by the theme. */
static void draw_panel(struct nk_context *c, const ui_font *font, const pc_ui_tokens *t, float w, float h)
{
    static int check = 1;
    static float opacity = 75.0f;
    static int blend = 1;
    static char name[48] = "Background copy";
    if (nk_begin(c, "PhotoCraft", nk_rect(0, 0, w, h), NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
        nk_layout_row_dynamic(c, 22, 1);
        nk_label(c, pc_ui_theme_kind_label(t->kind), NK_TEXT_LEFT);
        nk_label(c, "\xc3\x89" "bauche \xc2\xb7 \xe2\x80\x9cquotes\xe2\x80\x9d \xe2\x80\xa6 1\xe2\x80\x93" "2 \xe2\x82\xac", NK_TEXT_LEFT);

        nk_layout_row_dynamic(c, 28, 3);
        (void)nk_button_label(c, "Open\xe2\x80\xa6");
        (void)nk_button_label(c, "Save");
        (void)nk_button_label(c, "Export");

        nk_layout_row_dynamic(c, 26, 3);
        nk_checkbox_label(c, "Snap", &check);
        nk_property_float(c, "Opacity:", 0.0f, &opacity, 100.0f, 1.0f, 0.5f);
        blend = nk_combo(c, BLEND_MODES, (int)(sizeof BLEND_MODES / sizeof BLEND_MODES[0]), blend, 22, nk_vec2(180, 180));

        nk_layout_row_dynamic(c, 26, 2);
        nk_label(c, "Layer name:", NK_TEXT_LEFT);
        nk_edit_string_zero_terminated(c, NK_EDIT_FIELD, name, (int)sizeof name, nk_filter_default);

        nk_layout_row_dynamic(c, 18, 1);
        {
            nk_size p = (nk_size)(opacity * 10.0f);
            nk_progress(c, &p, 1000, NK_FIXED);
        }

        /* An accent colour swatch drawn directly, like a tool colour chip. */
        nk_layout_row_dynamic(c, 40, 1);
        {
            struct nk_command_buffer *canvas = nk_window_get_canvas(c);
            struct nk_rect r;
            if (nk_widget(&r, c) != NK_WIDGET_INVALID) {
                nk_fill_rect(canvas, r, t->radius, pc_ui_nk_color(t->accent));
                nk_draw_text(canvas, nk_rect(r.x + 8, r.y + 10, r.w - 16, 20), "Accent", 6, &font->nk,
                             pc_ui_nk_color(t->accent), pc_ui_nk_color(t->accent_text));
            }
        }

        if (nk_group_begin(c, "Card", NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {
            nk_layout_row_dynamic(c, 22, 1);
            nk_label(c, "Grouped card", NK_TEXT_LEFT);
            nk_label(c, "flat panel \xc2\xb7 tab strip", NK_TEXT_LEFT);
            nk_group_end(c);
        }
    }
    nk_end(c);
}

static int render(const gl33 *gl, ui_renderer *rr, struct nk_context *ctx, const ui_font *font,
                  const pc_ui_tokens *t, float scale, const char *out)
{
    int fw = (int)((float)WIN_W * scale + 0.5f);
    int fh = (int)((float)WIN_H * scale + 0.5f);
    GLuint fbo = 0, tex = 0;
    uint8_t *px = NULL, *flipped = NULL;
    pc_error err;
    int rc = 1;

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
    for (int frame = 0; frame < 3; frame++) { /* Nuklear lays out on the first frame */
        nk_input_begin(ctx);
        nk_input_end(ctx);
        draw_panel(ctx, font, t, WIN_W, WIN_H);
        gl->glClearColor((float)t->canvas.r / 255.0f, (float)t->canvas.g / 255.0f, (float)t->canvas.b / 255.0f, 1.0f);
        gl->glClear(GL_COLOR_BUFFER_BIT);
        if (ui_renderer_draw(rr, ctx, fw, fh, scale, &err) != PC_OK) {
            rc = fail("render", err.msg);
            goto cleanup;
        }
    }
    px = pc_malloc_array((size_t)fw * (size_t)fh, 4);
    flipped = pc_malloc_array((size_t)fw * (size_t)fh, 4);
    if (!px || !flipped) {
        rc = fail("snapshot", "out of memory");
        goto cleanup;
    }
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < fh; y++) { /* GL rows are bottom-up */
        memcpy(flipped + (size_t)y * (size_t)fw * 4, px + (size_t)(fh - 1 - y) * (size_t)fw * 4, (size_t)fw * 4);
    }
    if (spike_png_write(out, flipped, (uint32_t)fw, (uint32_t)fh, (size_t)fw * 4, &err) != PC_OK) {
        rc = fail("snapshot", err.msg);
        goto cleanup;
    }
    (void)printf("wrote %s (%dx%d, scale %.2f, theme %s)\n", out, fw, fh, (double)scale, pc_ui_theme_kind_id(t->kind));
    rc = 0;
cleanup:
    free(px);
    free(flipped);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl->glDeleteFramebuffers(1, &fbo);
    gl->glDeleteTextures(1, &tex);
    return rc;
}

int main(int argc, char **argv)
{
    const char *out = NULL, *font_path = "assets/fonts/Inter-Regular.ttf", *missing = NULL;
    float scale = 1.0f;
    pc_ui_theme_kind theme = PC_UI_THEME_DEFAULT;
    pc_error err;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = (float)SDL_strtod(argv[++i], NULL);
            if (!(scale >= 0.5f && scale <= 4.0f)) { return fail("--scale", "must be 0.5..4"); }
        } else if (strcmp(argv[i], "--theme") == 0 && i + 1 < argc) {
            if (!pc_ui_theme_kind_from_name(argv[++i], &theme)) { return fail("--theme", "unknown theme"); }
        } else if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            font_path = argv[++i];
        } else {
            (void)fprintf(stderr, "usage: %s --snapshot OUT.png [--scale S] [--theme ID] [--font PATH]\n", argv[0]);
            return 2;
        }
    }
    if (!out) { return fail("usage", "--snapshot OUT.png is required"); }

    if (!SDL_Init(SDL_INIT_VIDEO)) { return fail("SDL_Init", SDL_GetError()); }
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    (void)SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_Window *win = SDL_CreateWindow("photocraft-ui-snapshot", WIN_W, WIN_H, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    int rc = 1;
    SDL_GLContext glc = NULL;
    gl33 gl;
    ui_font font;
    ui_renderer rr;
    struct nk_context ctx;
    int ctx_ready = 0, rr_ready = 0, font_ready = 0;
    memset(&font, 0, sizeof font);
    memset(&rr, 0, sizeof rr);
    if (!win) { rc = fail("SDL_CreateWindow", SDL_GetError()); goto done; }
    glc = SDL_GL_CreateContext(win);
    if (!glc) { rc = fail("OpenGL 3.3 core context", SDL_GetError()); goto done; }
    if (!gl33_load(&gl, &missing)) { rc = fail("OpenGL function missing", missing); goto done; }
    if (ui_font_init(&font, font_path, 14.0f, scale, &err) != PC_OK) { rc = fail("font", err.msg); goto done; }
    font_ready = 1;
    if (ui_renderer_init(&rr, &gl, &font, &err) != PC_OK) { rc = fail("renderer", err.msg); goto done; }
    rr_ready = 1;
    if (!nk_init_default(&ctx, &font.nk)) { rc = fail("Nuklear", "init failed"); goto done; }
    ctx_ready = 1;
    pc_ui_tokens t = pc_ui_tokens_for_kind(theme);
    pc_ui_apply_nk_style(&ctx, &t);
    rc = render(&gl, &rr, &ctx, &font, &t, scale, out);
done:
    if (ctx_ready) { nk_free(&ctx); }
    if (rr_ready) { ui_renderer_fini(&rr); }
    if (font_ready) { ui_font_fini(&font); }
    if (glc) { SDL_GL_DestroyContext(glc); }
    if (win) { SDL_DestroyWindow(win); }
    SDL_Quit();
    return rc;
}
