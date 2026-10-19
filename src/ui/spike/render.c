#include "render.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct ui_vertex {
    float pos[2];
    float uv[2];
    nk_byte col[4];
} ui_vertex;

static const GLchar *const VERTEX_SRC = "#version 330 core\n"
                                        "uniform mat4 proj;\n"
                                        "in vec2 pos;\n"
                                        "in vec2 uv;\n"
                                        "in vec4 col;\n"
                                        "out vec2 f_uv;\n"
                                        "out vec4 f_col;\n"
                                        "void main() {\n"
                                        "    f_uv = uv;\n"
                                        "    f_col = col;\n"
                                        "    gl_Position = proj * vec4(pos, 0.0, 1.0);\n"
                                        "}\n";

static const GLchar *const FRAGMENT_SRC = "#version 330 core\n"
                                          "uniform sampler2D tex;\n"
                                          "in vec2 f_uv;\n"
                                          "in vec4 f_col;\n"
                                          "out vec4 out_col;\n"
                                          "void main() {\n"
                                          "    out_col = f_col * texture(tex, f_uv);\n"
                                          "}\n";

static pc_status compile(const gl33 *gl, GLenum type, const GLchar *src, GLuint *out, pc_error *err)
{
    GLuint s = gl->glCreateShader(type);
    GLint ok = 0;
    gl->glShaderSource(s, 1, &src, NULL);
    gl->glCompileShader(s);
    gl->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLchar log[512];
        log[0] = '\0';
        gl->glGetShaderInfoLog(s, (GLsizei)sizeof log, NULL, log);
        gl->glDeleteShader(s);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "UI shader failed to compile: %s", log);
    }
    *out = s;
    return PC_OK;
}

pc_status ui_renderer_init(ui_renderer *r, const gl33 *gl, ui_font *font, pc_error *err)
{
    GLuint vs = 0;
    GLuint fs = 0;
    GLint ok = 0;
    GLint a_pos;
    GLint a_uv;
    GLint a_col;
    pc_status s;

    memset(r, 0, sizeof *r);
    r->gl = gl;
    s = compile(gl, GL_VERTEX_SHADER, VERTEX_SRC, &vs, err);
    if (s == PC_OK) {
        s = compile(gl, GL_FRAGMENT_SHADER, FRAGMENT_SRC, &fs, err);
    }
    if (s != PC_OK) {
        if (vs != 0) {
            gl->glDeleteShader(vs);
        }
        return s;
    }
    r->prog = gl->glCreateProgram();
    gl->glAttachShader(r->prog, vs);
    gl->glAttachShader(r->prog, fs);
    gl->glLinkProgram(r->prog);
    gl->glDeleteShader(vs);
    gl->glDeleteShader(fs);
    gl->glGetProgramiv(r->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLchar log[512];
        log[0] = '\0';
        gl->glGetProgramInfoLog(r->prog, (GLsizei)sizeof log, NULL, log);
        gl->glDeleteProgram(r->prog);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "UI shader failed to link: %s", log);
    }
    r->u_tex = gl->glGetUniformLocation(r->prog, "tex");
    r->u_proj = gl->glGetUniformLocation(r->prog, "proj");
    a_pos = gl->glGetAttribLocation(r->prog, "pos");
    a_uv = gl->glGetAttribLocation(r->prog, "uv");
    a_col = gl->glGetAttribLocation(r->prog, "col");
    if (a_pos < 0 || a_uv < 0 || a_col < 0) {
        gl->glDeleteProgram(r->prog);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "UI shader is missing an attribute");
    }

    gl->glGenVertexArrays(1, &r->vao);
    gl->glGenBuffers(1, &r->vbo);
    gl->glGenBuffers(1, &r->ebo);
    gl->glBindVertexArray(r->vao);
    gl->glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ebo);
    gl->glEnableVertexAttribArray((GLuint)a_pos);
    gl->glEnableVertexAttribArray((GLuint)a_uv);
    gl->glEnableVertexAttribArray((GLuint)a_col);
    gl->glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, 0, sizeof(ui_vertex),
                              (const void *)offsetof(ui_vertex, pos));
    gl->glVertexAttribPointer((GLuint)a_uv, 2, GL_FLOAT, 0, sizeof(ui_vertex),
                              (const void *)offsetof(ui_vertex, uv));
    gl->glVertexAttribPointer((GLuint)a_col, 4, GL_UNSIGNED_BYTE, 1, sizeof(ui_vertex),
                              (const void *)offsetof(ui_vertex, col));
    gl->glBindVertexArray(0);

    gl->glGenTextures(1, &r->font_tex);
    gl->glBindTexture(GL_TEXTURE_2D, r->font_tex);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, font->atlas_w, font->atlas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     font->atlas);
    r->font = font;
    font->dirty = false;
    font->nk.texture = nk_handle_id((int)r->font_tex);
    r->null_tex.texture = nk_handle_id((int)r->font_tex);
    r->null_tex.uv = font->white_uv;
    nk_buffer_init_default(&r->cmds);
    r->initialised = 1;
    return PC_OK;
}

pc_status ui_renderer_draw(ui_renderer *r, struct nk_context *ctx, int fb_w, int fb_h, float scale,
                           pc_error *err)
{
    static const struct nk_draw_vertex_layout_element layout[] = {
        {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, offsetof(ui_vertex, pos)},
        {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, offsetof(ui_vertex, uv)},
        {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, offsetof(ui_vertex, col)},
        {NK_VERTEX_LAYOUT_END},
    };
    const gl33 *gl = r->gl;
    struct nk_convert_config cfg;
    struct nk_buffer vbuf;
    struct nk_buffer ebuf;
    struct nk_memory_status vinfo;
    struct nk_memory_status einfo;
    const struct nk_draw_command *cmd;
    size_t offset = 0; /* bytes into the element buffer */
    float w = (float)fb_w / scale;
    float h = (float)fb_h / scale;
    GLfloat proj[16];
    nk_flags res;

    memset(proj, 0, sizeof proj);
    proj[0] = 2.0f / w;
    proj[5] = -2.0f / h;
    proj[10] = -1.0f;
    proj[12] = -1.0f;
    proj[13] = 1.0f;
    proj[15] = 1.0f;

    gl->glViewport(0, 0, fb_w, fb_h);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    gl->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl->glDisable(GL_CULL_FACE);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glEnable(GL_SCISSOR_TEST);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glUseProgram(r->prog);
    gl->glUniform1i(r->u_tex, 0);
    gl->glUniformMatrix4fv(r->u_proj, 1, 0, proj);

    memset(&cfg, 0, sizeof cfg);
    cfg.vertex_layout = layout;
    cfg.vertex_size = sizeof(ui_vertex);
    cfg.vertex_alignment = _Alignof(ui_vertex);
    cfg.tex_null = r->null_tex;
    cfg.circle_segment_count = 22;
    cfg.curve_segment_count = 22;
    cfg.arc_segment_count = 22;
    cfg.global_alpha = 1.0f;
    cfg.shape_AA = NK_ANTI_ALIASING_ON;
    cfg.line_AA = NK_ANTI_ALIASING_ON;

    nk_buffer_init_default(&vbuf);
    nk_buffer_init_default(&ebuf);
    res = nk_convert(ctx, &r->cmds, &vbuf, &ebuf, &cfg);
    if (res != NK_CONVERT_SUCCESS) {
        nk_buffer_free(&vbuf);
        nk_buffer_free(&ebuf);
        nk_clear(ctx);
        return pc_fail(err, PC_ERR_NOMEM, "UI geometry conversion failed (%u)", (unsigned)res);
    }
    if (r->font && r->font->dirty) {
        ui_font *font = r->font;
        int top = font->dirty_top, bottom = font->dirty_bottom;
        if (top >= 0 && bottom > top && bottom <= font->atlas_h) {
            gl->glBindTexture(GL_TEXTURE_2D, r->font_tex);
            gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, top, font->atlas_w, bottom - top, GL_RGBA,
                                GL_UNSIGNED_BYTE, font->atlas + (size_t)top * (size_t)font->atlas_w * 4);
        }
        font->dirty = false;
    }
    nk_buffer_info(&vinfo, &vbuf);
    nk_buffer_info(&einfo, &ebuf);
    gl->glBindVertexArray(r->vao);
    gl->glBindBuffer(GL_ARRAY_BUFFER, r->vbo);
    gl->glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vinfo.allocated, nk_buffer_memory_const(&vbuf),
                     GL_STREAM_DRAW);
    gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->ebo);
    gl->glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)einfo.allocated, nk_buffer_memory_const(&ebuf),
                     GL_STREAM_DRAW);

    nk_draw_foreach(cmd, ctx, &r->cmds)
    {
        if (cmd->elem_count == 0) {
            continue;
        }
        gl->glBindTexture(GL_TEXTURE_2D, (GLuint)cmd->texture.id);
        gl->glScissor((GLint)lroundf(cmd->clip_rect.x * scale),
                      (GLint)lroundf((float)fb_h - (cmd->clip_rect.y + cmd->clip_rect.h) * scale),
                      (GLint)lroundf(cmd->clip_rect.w * scale), (GLint)lroundf(cmd->clip_rect.h * scale));
        gl->glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count, GL_UNSIGNED_INT,
                           (const void *)(uintptr_t)offset);
        offset += (size_t)cmd->elem_count * sizeof(nk_draw_index);
    }
    nk_clear(ctx);
    nk_buffer_clear(&r->cmds);
    nk_buffer_free(&vbuf);
    nk_buffer_free(&ebuf);
    gl->glBindVertexArray(0);
    gl->glDisable(GL_SCISSOR_TEST);
    return PC_OK;
}

void ui_renderer_fini(ui_renderer *r)
{
    const gl33 *gl = r->gl;
    if (!r->initialised) {
        return;
    }
    nk_buffer_free(&r->cmds);
    gl->glDeleteTextures(1, &r->font_tex);
    gl->glDeleteBuffers(1, &r->vbo);
    gl->glDeleteBuffers(1, &r->ebo);
    gl->glDeleteVertexArrays(1, &r->vao);
    gl->glDeleteProgram(r->prog);
    memset(r, 0, sizeof *r);
}
