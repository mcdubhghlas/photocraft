/* Renders a Nuklear context with OpenGL 3.3 core.
 *
 * UI coordinates are in UI units; `scale` maps them to framebuffer pixels. The renderer owns its
 * GL objects (shader, buffers, the font atlas texture) and must be finalised while its GL context
 * is current. Not thread-safe. */
#ifndef PC_UI_RENDER_H
#define PC_UI_RENDER_H

#include "base/status.h"
#include "font.h"
#include "gl33.h"
#include "photocraft_nuklear.h"

typedef struct ui_renderer {
    ui_font *font;  /* borrowed; atlas updates after geometry conversion */
    const gl33 *gl; /* borrowed */
    GLuint prog;
    GLuint vao;
    GLuint vbo;
    GLuint ebo;
    GLuint font_tex;
    GLint u_tex;
    GLint u_proj;
    struct nk_buffer cmds;
    struct nk_draw_null_texture null_tex;
    int initialised;
} ui_renderer;

/* Compile shaders, create buffers and upload font's atlas; sets font->nk.texture. */
pc_status ui_renderer_init(ui_renderer *r, const gl33 *gl, ui_font *font, pc_error *err);

/* Draw ctx's commands into the currently bound framebuffer (fb_w x fb_h pixels), then clear
 * the context for the next frame. */
pc_status ui_renderer_draw(ui_renderer *r, struct nk_context *ctx, int fb_w, int fb_h, float scale,
                           pc_error *err);

void ui_renderer_fini(ui_renderer *r);

#endif
