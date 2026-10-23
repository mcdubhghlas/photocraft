/* Doc-space textured overlay primitive for the GL canvas. See canvas_overlay.h. */
#include "canvas_overlay.h"

#include <math.h>

#include "base/num.h"

#ifndef GL_ONE
#define GL_ONE 1
#endif

/* Full-screen triangle; uv spans [0,1]^2 across the viewport (same as the camera pass). */
static const char *const overlay_vertex_src =
    "#version 330 core\n"
    "out vec2 uv; void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
    "uv=p; gl_Position=vec4(p*2.0-1.0,0,1);}";

/* Map each viewport point back to document space exactly like the camera shader, then sample the
 * overlay texture at that document point. Outside the overlay rect: discard (let the canvas show
 * through). The texture is premultiplied, so a straight opacity multiply keeps it premultiplied. */
static const char *const overlay_fragment_src =
    "#version 330 core\n"
    "uniform sampler2D tex; uniform vec2 origin,axisX,axisY,opacity; uniform vec4 rectv;"
    "in vec2 uv; out vec4 color;"
    "void main(){vec2 d=origin+axisX*uv.x+axisY*uv.y; vec2 t=(d-rectv.xy)/rectv.zw;"
    "if(any(lessThan(t,vec2(0.0)))||any(greaterThan(t,vec2(1.0)))){discard;}"
    "vec4 p=texture(tex,t); color=p*opacity.x;}";

static pc_status compile(const gl33 *gl, GLenum type, const char *src, GLuint *out, pc_error *err)
{
    GLuint shader = gl->glCreateShader(type);
    if (!shader) {
        return pc_fail(err, PC_ERR_IO, "overlay shader allocation failed");
    }
    gl->glShaderSource(shader, 1, &src, NULL);
    gl->glCompileShader(shader);
    GLint ok = 0;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        gl->glGetShaderInfoLog(shader, (GLsizei)sizeof log, NULL, log);
        gl->glDeleteShader(shader);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "overlay shader: %.200s", log);
    }
    *out = shader;
    return PC_OK;
}

static pc_status ensure_program(pc_ui_canvas_overlay *o, const gl33 *gl, pc_error *err)
{
    if (o->program) {
        return PC_OK;
    }
    GLuint vs = 0, fs = 0, program = 0;
    pc_status rc = compile(gl, GL_VERTEX_SHADER, overlay_vertex_src, &vs, err);
    if (rc == PC_OK) {
        rc = compile(gl, GL_FRAGMENT_SHADER, overlay_fragment_src, &fs, err);
    }
    if (rc == PC_OK) {
        program = gl->glCreateProgram();
        gl->glAttachShader(program, vs);
        gl->glAttachShader(program, fs);
        gl->glLinkProgram(program);
        GLint ok = 0;
        gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            rc = pc_fail(err, PC_ERR_UNSUPPORTED, "overlay shader link failed");
        }
    }
    if (vs) {
        gl->glDeleteShader(vs);
    }
    if (fs) {
        gl->glDeleteShader(fs);
    }
    if (rc != PC_OK) {
        if (program) {
            gl->glDeleteProgram(program);
        }
        return rc;
    }
    o->program = program;
    gl->glGenVertexArrays(1, &o->vao);
    gl->glGenTextures(1, &o->texture);
    return PC_OK;
}

pc_status pc_ui_canvas_overlay_prepare(pc_ui_canvas_overlay *o, const gl33 *gl, size_t w, size_t h,
                                       bool *needs_full, pc_error *err)
{
    if (needs_full) {
        *needs_full = false;
    }
    if (w < 1 || h < 1) {
        return pc_fail(err, PC_ERR_INVALID, "overlay: empty texture size");
    }
    pc_status rc = ensure_program(o, gl, err);
    if (rc != PC_OK) {
        return rc;
    }
    GLint maximum = 0;
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (maximum > 0 && (w > (size_t)maximum || h > (size_t)maximum)) {
        return pc_fail(err, PC_ERR_LIMIT, "overlay: texture exceeds GPU limit");
    }
    if ((int)w != o->tex_w || (int)h != o->tex_h) {
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, o->texture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         NULL);
        if (gl->glGetError() != GL_NO_ERROR) {
            return pc_fail(err, PC_ERR_IO, "overlay: texture allocation failed");
        }
        o->tex_w = (int)w;
        o->tex_h = (int)h;
        if (needs_full) {
            *needs_full = true;
        }
    }
    return PC_OK;
}

void pc_ui_canvas_overlay_upload(const pc_ui_canvas_overlay *o, const gl33 *gl, const uint8_t (*px)[4],
                                 size_t src_w, size_t x0, size_t y0, size_t x1, size_t y1)
{
    if (!o->texture || !px || src_w < 1) {
        return;
    }
    size_t tw = (size_t)(o->tex_w > 0 ? o->tex_w : 0), th = (size_t)(o->tex_h > 0 ? o->tex_h : 0);
    if (x1 > tw) {
        x1 = tw;
    }
    if (y1 > th) {
        y1 = th;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, o->texture);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    /* Upload the sub-rectangle directly from the source image using its row stride. */
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)src_w);
    gl->glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)x0, (GLint)y0, (GLsizei)(x1 - x0), (GLsizei)(y1 - y0),
                        GL_RGBA, GL_UNSIGNED_BYTE, &px[y0 * src_w + x0][0]);
    gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

pc_status pc_ui_canvas_overlay_draw(const pc_ui_canvas_overlay *o, const gl33 *gl,
                                    const pc_ui_canvas_xform *xf, pc_rect doc_rect, float opacity,
                                    pc_error *err)
{
    if (!o->program || !o->texture) {
        return pc_fail(err, PC_ERR_INVALID, "overlay: not prepared");
    }
    float rw = (float)pc_rect_width(doc_rect), rh = (float)pc_rect_height(doc_rect);
    if (!(rw > 0.f) || !(rh > 0.f)) {
        return pc_fail(err, PC_ERR_INVALID, "overlay: empty document rect");
    }
    /* Map viewport corners to document space exactly like render_camera. */
    float origin[2], right[2], bottom[2];
    if (pc_ui_canvas_to_doc(xf, xf->x, xf->y, origin) != PC_OK ||
        pc_ui_canvas_to_doc(xf, xf->x + xf->width, xf->y, right) != PC_OK ||
        pc_ui_canvas_to_doc(xf, xf->x, xf->y + xf->height, bottom) != PC_OK) {
        return pc_fail(err, PC_ERR_INVALID, "overlay: invalid camera");
    }
    if (!(opacity > 0.f)) {
        return PC_OK; /* fully transparent: nothing to draw */
    }
    if (opacity > 1.f) {
        opacity = 1.f;
    }
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glEnable(GL_BLEND);
    gl->glBlendEquation(GL_FUNC_ADD);
    gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); /* premultiplied-alpha over */
    gl->glUseProgram(o->program);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, o->texture);
    gl->glUniform1i(gl->glGetUniformLocation(o->program, "tex"), 0);
    gl->glUniform2f(gl->glGetUniformLocation(o->program, "origin"), origin[0], origin[1]);
    gl->glUniform2f(gl->glGetUniformLocation(o->program, "axisX"), right[0] - origin[0],
                    right[1] - origin[1]);
    gl->glUniform2f(gl->glGetUniformLocation(o->program, "axisY"), bottom[0] - origin[0],
                    bottom[1] - origin[1]);
    gl->glUniform4f(gl->glGetUniformLocation(o->program, "rectv"), (float)doc_rect.x0, (float)doc_rect.y0,
                    rw, rh);
    gl->glUniform2f(gl->glGetUniformLocation(o->program, "opacity"), opacity, 0.f);
    gl->glBindVertexArray(o->vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glBindVertexArray(0);
    gl->glDisable(GL_BLEND);
    return gl->glGetError() == GL_NO_ERROR ? PC_OK : pc_fail(err, PC_ERR_IO, "overlay: draw failed");
}

void pc_ui_canvas_overlay_fini(pc_ui_canvas_overlay *o, const gl33 *gl)
{
    if (!o) {
        return;
    }
    if (gl) {
        if (o->texture) {
            gl->glDeleteTextures(1, &o->texture);
        }
        if (o->vao) {
            gl->glDeleteVertexArrays(1, &o->vao);
        }
        if (o->program) {
            gl->glDeleteProgram(o->program);
        }
    }
    *o = (pc_ui_canvas_overlay){0};
}
