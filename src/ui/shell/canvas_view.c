#include "layer_picker_ui.h"
#include "sampling_ui.h"
#include "engine/floating.h"
#include "artboard_ui.h"
#include "lasso_ui.h"
#include "slice_ui.h"
#include "snap_ui.h"
#include "transform_ui.h"
#include "context_ui.h"
#include "crop_ui.h"
#include "jobs_ui.h"
#include "canvas_view.h"
#include "dialogs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "ui/widgets.h"
#include "ui/hold_keys.h"
#include "engine/prefs.h"
#include "color/convert.h"
#include "ui/type_overlay.h"
#include "pen_ui.h"
#include "brush_resize_ui.h"

static bool preference_bool(pc_ui_shell_app *app, const char *path, bool fallback);

pc_ui_view *pc_ui_canvas_active_view(pc_ui_shell_app *app)
{
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    size_t i;
    return pc_engine_session_active_index(pc_ui_shell_app_session(app), &i) && i < ui->n_views ? &ui->views[i]
                                                                                               : NULL;
}

pc_status pc_ui_canvas_sync(pc_ui_shell_app *app, pc_ui_canvas_view *cv, pc_error *err)
{
    size_t n = 0;
    const pc_engine_doc_state *docs = pc_engine_session_documents(pc_ui_shell_app_session(app), &n);
    pc_ui_state *ui = pc_ui_shell_app_state(app);
    bool same = n == cv->n_documents && n == ui->n_views;
    for (size_t i = 0; same && i < n; i++) {
        same = cv->documents[i] == pc_doc_arc_get(docs[i].doc)->id;
    }
    if (!same) {
        pc_ui_tool_gesture_fini(&cv->gesture);
        pc_ui_view *views = n ? pc_calloc_array(n, sizeof *views) : NULL;
        pc_doc_id *ids = n ? pc_malloc_array(n, sizeof *ids) : NULL;
        if (n && (!views || !ids)) {
            free(views);
            free(ids);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (size_t i = 0; i < n; i++) {
            ids[i] = pc_doc_arc_get(docs[i].doc)->id;
            pc_ui_view_default(&views[i]);
            if (cv->n_documents == 0 && ui->n_views == n) {
                views[i] = ui->views[i];
            }
            for (size_t j = 0; j < cv->n_documents && j < ui->n_views; j++) {
                if (ids[i] == cv->documents[j]) {
                    views[i] = ui->views[j];
                    break;
                }
            }
        }
        free(ui->views);
        free(cv->documents);
        ui->views = views;
        ui->n_views = n;
        cv->documents = ids;
        cv->n_documents = n;
        cv->dragging = false;
        cv->scroll_drag.active = false;
        cv->guide_drag.active = false;
        cv->laid_out = false;
    }
    for (size_t i = 0; i < n; i++) {
        const pc_document *doc = pc_doc_arc_get(docs[i].doc);
        ui->views[i].doc_size[0] = doc->size.width;
        ui->views[i].doc_size[1] = doc->size.height;
    }
    return PC_OK;
}

void pc_ui_canvas_view_fini(pc_ui_canvas_view *v, const gl33 *gl)
{
    pc_gpu_compositor_free(v->gpu);
    pc_ui_native_cursor_fini(&v->cursor);
    pc_ui_vector_preview_fini(&v->vector_preview);
    pc_ui_vector_preview_fini(&v->pen_preview);
    pc_ui_tool_gesture_fini(&v->gesture);
    pc_doc_arc_release(v->floating_display);
    for (size_t i = 0; i < PC_UI_CANVAS_TILE_CAP; i++) {
        pc_ui_canvas_tile_upload *upload = &v->tile_uploads[i];
        pc_ui_channel_cache_fini(&upload->channels);
        if (gl && upload->source) {
            gl->glDeleteTextures(1, &upload->source);
        }
        if (gl && upload->channels_texture) {
            gl->glDeleteTextures(1, &upload->channels_texture);
        }
    }
    pc_ui_canvas_tiles_fini(&v->tiles);
    pc_engine_display_fini(&v->display);
    pc_ui_channel_cache_fini(&v->channels);
    if (gl) {
        if (v->channels_texture) {
            gl->glDeleteTextures(1, &v->channels_texture);
        }
        if (v->display_texture) {
            gl->glDeleteTextures(1, &v->display_texture);
        }
        if (v->source) {
            gl->glDeleteTextures(1, &v->source);
        }
        if (v->target) {
            gl->glDeleteTextures(1, &v->target);
        }
        if (v->framebuffer) {
            gl->glDeleteFramebuffers(1, &v->framebuffer);
        }
        if (v->vao) {
            gl->glDeleteVertexArrays(1, &v->vao);
        }
        if (v->program) {
            gl->glDeleteProgram(v->program);
        }
    }
    pc_ui_canvas_cache_fini(&v->cache);
    pc_ui_outline_fini(&v->overlays.selection);
    free(v->documents);
    *v = (pc_ui_canvas_view){0};
}

/* Render the camera into a texture, which becomes a normal clipped Nuklear image. The fragment
 * shader maps each viewport point to document space, so rotation/aspect/flip and checkerboard
 * agree with pointer mapping without a CPU viewport resample. This is canvas display, not the
 * G1/G2 layer compositor: the source texture is produced by the CPU oracle. */
static const char *const vertex_src =
    "#version 330 core\n"
    "out vec2 uv; void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
    "uv=p; gl_Position=vec4(p*2.0-1.0,0,1);}";
static const char *const fragment_src =
    "#version 330 core\n"
    "uniform sampler2D source; uniform sampler2D channelView;uniform int showChannels; uniform sampler3D "
    "displayLut; uniform vec4 displayParams; uniform vec4 "
    "gamutWarning; uniform vec2 lutMap; uniform vec2 origin,axisX,axisY,docSize;uniform vec4 sourceRect;"
    "uniform vec4 tileClip;uniform int tilePass;uniform vec4 surround,lightCheck,darkCheck; uniform int "
    "pixelGrid;uniform int showOutside; in vec2 uv; out vec4 color;"
    "vec3 encode(vec3 v){return "
    "mix(12.92*v,1.055*pow(max(v,vec3(0)),vec3(1.0/2.4))-0.055,step(vec3(0.0031308),v));}"
    "vec3 decode(vec3 v){return "
    "mix(v/12.92,pow(max((v+0.055)/1.055,vec3(0)),vec3(2.4)),step(vec3(0.04045),v));}"
    "void main(){vec2 d=origin+axisX*uv.x+axisY*uv.y;"
    "if(tilePass!=0&&(any(lessThan(d,tileClip.xy))||any(greaterThanEqual(d,tileClip.zw)))){discard;}"
    "if(showOutside==0&&(any(lessThan(d,vec2(0)))||any(greaterThanEqual(d,docSize)))){color=surround;return;}"
    "if(showOutside!=0&&(any(lessThan(d,sourceRect.xy))||any(greaterThanEqual(d,sourceRect.xy+sourceRect.zw))"
    ")){color=surround;return;}"
    "vec4 p=texture(source,(d-sourceRect.xy)/sourceRect.zw);"
    "vec3 rgb=p.a>1e-8?p.rgb/p.a:vec3(0);"
    "if(displayParams.x>0.5){rgb=encode(clamp(rgb,0.0,65504.0));}"
    "if(displayParams.y>0.5){rgb=encode(clamp(pow(max(vec3(0),decode(rgb)*exp2(displayParams.z)),vec3(1.0/"
    "displayParams.w)),0.0,1.0));}"
    "vec4 managed=texture(displayLut,clamp(rgb,0.0,1.0)*lutMap.x+lutMap.y);"
    "if(gamutWarning.a>0.0 && managed.a>0.5){managed.rgb=mix(managed.rgb,gamutWarning.rgb,gamutWarning.a);}"
    "p.rgb=managed.rgb*p.a;"
    "float k=mod(floor(d.x/8.0)+floor(d.y/8.0),2.0);"
    "vec3 bg=mix(lightCheck.rgb,darkCheck.rgb,k);"
    "vec3 shown=clamp(p.rgb+bg*(1.0-p.a),0.0,1.0);"
    "if(showChannels!=0){vec4 "
    "overlay=texture(channelView,(d-sourceRect.xy)/sourceRect.zw);shown=overlay.rgb+shown*(1.0-overlay.a);}"
    "vec2 f=fract(d);vec2 edge=min(f,1.0-f)/max(fwidth(d),vec2(1e-6));"
    "if(pixelGrid!=0 && p.a>0.0 && min(edge.x,edge.y)<0.5){"
    "float luma=dot(shown,vec3(0.2126,0.7152,0.0722));"
    "float black=round(smoothstep(0.35,0.75,luma)*8.0)/8.0;"
    "float a=0.25*mix(1.0,0.64,black);shown=mix(shown,vec3(1.0-black),a);}"
    "color=vec4(shown,1);}";

static pc_status compile_shader(const gl33 *gl, GLenum type, const char *source, GLuint *out, pc_error *err)
{
    GLuint shader = gl->glCreateShader(type);
    if (!shader) {
        return pc_fail(err, PC_ERR_IO, "canvas shader allocation failed");
    }
    gl->glShaderSource(shader, 1, &source, NULL);
    gl->glCompileShader(shader);
    GLint ok = 0;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {0};
        gl->glGetShaderInfoLog(shader, (GLsizei)sizeof log, NULL, log);
        gl->glDeleteShader(shader);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "canvas shader: %.200s", log);
    }
    *out = shader;
    return PC_OK;
}

static pc_status init_gpu(pc_ui_canvas_view *v, const gl33 *gl, pc_error *err)
{
    if (v->program) {
        return PC_OK;
    }
    GLuint vs = 0, fs = 0;
    pc_status rc = compile_shader(gl, GL_VERTEX_SHADER, vertex_src, &vs, err);
    if (rc == PC_OK) {
        rc = compile_shader(gl, GL_FRAGMENT_SHADER, fragment_src, &fs, err);
    }
    GLuint program = 0;
    if (rc == PC_OK) {
        program = gl->glCreateProgram();
        gl->glAttachShader(program, vs);
        gl->glAttachShader(program, fs);
        gl->glLinkProgram(program);
        GLint ok = 0;
        gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            rc = pc_fail(err, PC_ERR_UNSUPPORTED, "canvas shader link failed");
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
    v->program = program;
    gl->glGenVertexArrays(1, &v->vao);
    gl->glGenTextures(1, &v->source);
    gl->glGenTextures(1, &v->display_texture);
    gl->glGenTextures(1, &v->channels_texture);
    gl->glGenTextures(1, &v->target);
    gl->glGenFramebuffers(1, &v->framebuffer);
    return PC_OK;
}

static void color_uniform(const gl33 *gl, GLuint program, const char *name, pc_ui_color color)
{
    gl->glUniform4f(gl->glGetUniformLocation(program, name), (float)color.r / 255.f, (float)color.g / 255.f,
                    (float)color.b / 255.f, 1.f);
}

static void texture_params(const gl33 *gl)
{
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static pc_status render_camera(pc_ui_shell_app *app, pc_ui_canvas_view *v, const gl33 *gl,
                               pc_ui_canvas_cache *cache, pc_ui_canvas_tile_upload *tile,
                               const pc_engine_doc_state *state, pc_error *err)
{
    pc_status rc = init_gpu(v, gl, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_ui_channel_cache *channels = tile ? &tile->channels : &v->channels;
    uint64_t *channels_uploaded = tile ? &tile->channels_uploaded : &v->channels_uploaded;
    uint64_t *uploaded = tile ? &tile->uploaded : &v->uploaded;
    if (tile && !tile->source) {
        gl->glGenTextures(1, &tile->source);
        gl->glGenTextures(1, &tile->channels_texture);
    }
    GLuint source = tile ? tile->source : v->source;
    GLuint channels_texture = tile ? tile->channels_texture : v->channels_texture;
    uint64_t display_key = pc_engine_display_key(pc_ui_shell_app_session(app));
    if (!v->display_valid || display_key != v->display_key) {
        rc = pc_engine_display_build(pc_ui_shell_app_session(app), 33, &v->display, err);
        if (rc != PC_OK) {
            return rc;
        }
        gl->glActiveTexture(GL_TEXTURE0 + 1);
        gl->glBindTexture(GL_TEXTURE_3D, v->display_texture);
        gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        int n = (int)v->display.lut.size;
        gl->glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA32F, n, n, n, 0, GL_RGBA, GL_FLOAT, v->display.lut.data);
        if (gl->glGetError() != GL_NO_ERROR) {
            gl->glActiveTexture(GL_TEXTURE0);
            return pc_fail(err, PC_ERR_IO, "display LUT upload failed");
        }
        v->display_valid = true;
        v->display_key = display_key;
    }
    gl->glActiveTexture(GL_TEXTURE0 + 1);
    gl->glBindTexture(GL_TEXTURE_3D, v->display_texture);
    gl->glActiveTexture(GL_TEXTURE0);
    GLint bound = 0, maximum = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    float scale = pc_ui_shell_app_ui_scale(app);
    double width = ceil((double)v->xf.width * scale), height = ceil((double)v->xf.height * scale);
    if (width < 1 || height < 1 || width > maximum || height > maximum || width * height > 67108864.) {
        return pc_fail(err, PC_ERR_LIMIT, "canvas viewport exceeds texture budget");
    }
    int w = (int)width, h = (int)height;
    pc_ui_channel_overlay type_overlay =
        pc_ui_type_mask_overlay(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app));
    rc = pc_ui_channel_cache_update_overlay(channels, state, cache,
                                            preference_bool(app, "interface.showChannelsInColor", false),
                                            &type_overlay, err);
    if (rc != PC_OK) {
        return rc;
    }
    gl->glActiveTexture(GL_TEXTURE0 + 2);
    gl->glBindTexture(GL_TEXTURE_2D, channels_texture);
    if (!channels->active && (*channels_uploaded) == 0) {
        const float transparent[4] = {0};
        texture_params(gl);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, transparent);
        (*channels_uploaded) = UINT64_MAX;
    }
    if (channels->active && (*channels_uploaded) != channels->generation) {
        const pc_compose_buffer *image = &channels->image;
        texture_params(gl);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (int)pc_rect_width(image->rect),
                         (int)pc_rect_height(image->rect), 0, GL_RGBA, GL_FLOAT, image->px);
        if (gl->glGetError() != GL_NO_ERROR) {
            gl->glActiveTexture(GL_TEXTURE0);
            return pc_fail(err, PC_ERR_IO, "channel-view upload failed");
        }
        (*channels_uploaded) = channels->generation;
    }
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, source);
    texture_params(gl);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    bool *source_linear = tile ? &tile->source_linear : &v->source_linear;
    if ((*uploaded) != cache->generation || *source_linear != v->display.encode_linear) {
        const pc_compose_buffer *img = &cache->image;
        uint32_t iw = pc_rect_width(img->rect), ih = pc_rect_height(img->rect);
        if (iw > (uint32_t)maximum || ih > (uint32_t)maximum) {
            return pc_fail(err, PC_ERR_LIMIT, "canvas source exceeds GPU texture size");
        }
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        bool partial = cache->partial && !pc_rect_is_empty(cache->last_render) && *uploaded != UINT64_MAX &&
                       *uploaded + 1 == cache->generation && *source_linear == v->display.encode_linear;
        pc_rect region = partial ? cache->last_render : img->rect;
        uint32_t rw = pc_rect_width(region), rh = pc_rect_height(region);
        float (*encoded)[4] = NULL;
        const float *pixels = pc_compose_buffer_get(img, region.x0, region.y0);
        if (v->display.encode_linear) {
            encoded = pc_malloc_array((size_t)rw * rh, sizeof *encoded);
            if (!encoded) {
                return pc_fail(err, PC_ERR_NOMEM, "linear canvas upload");
            }
            for (size_t row = 0; row < rh; row++) {
                const float *line = pc_compose_buffer_get(img, region.x0, region.y0 + (int32_t)row);
                for (size_t col = 0; col < rw; col++) {
                    size_t i = row * rw + col;
                    float alpha = line[col * 4 + 3];
                    encoded[i][3] = alpha;
                    for (size_t ch = 0; ch < 3; ch++) {
                        float value = alpha > 0 ? line[col * 4 + ch] / alpha : 0;
                        encoded[i][ch] = pc_linear_to_srgb(fminf(65504, fmaxf(0, value))) * alpha;
                    }
                }
            }
            pixels = &encoded[0][0];
        }
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, encoded ? 0 : (GLint)iw);
        if (partial)
            gl->glTexSubImage2D(GL_TEXTURE_2D, 0, region.x0 - img->rect.x0, region.y0 - img->rect.y0,
                                (GLsizei)rw, (GLsizei)rh, GL_RGBA, GL_FLOAT, pixels);
        else
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (GLsizei)iw, (GLsizei)ih, 0, GL_RGBA, GL_FLOAT,
                             pixels);
        gl->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        free(encoded);
        *source_linear = v->display.encode_linear;
        gl->glGenerateMipmap(GL_TEXTURE_2D);
        if (gl->glGetError() != GL_NO_ERROR) {
            return pc_fail(err, PC_ERR_IO, "canvas upload failed");
        }
        (*uploaded) = cache->generation;
    }
    if (w != v->width || h != v->height) {
        gl->glBindTexture(GL_TEXTURE_2D, v->target);
        texture_params(gl);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        if (gl->glGetError() != GL_NO_ERROR) {
            return pc_fail(err, PC_ERR_IO, "canvas target allocation failed");
        }
        v->width = w;
        v->height = h;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, v->framebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, v->target, 0);
    if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
        return pc_fail(err, PC_ERR_IO, "canvas framebuffer incomplete");
    }
    float origin[2], right[2], bottom[2];
    if (pc_ui_canvas_to_doc(&v->xf, v->xf.x, v->xf.y, origin) != PC_OK ||
        pc_ui_canvas_to_doc(&v->xf, v->xf.x + v->xf.width, v->xf.y, right) != PC_OK ||
        pc_ui_canvas_to_doc(&v->xf, v->xf.x, v->xf.y + v->xf.height, bottom) != PC_OK) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
        return pc_fail(err, PC_ERR_INVALID, "invalid canvas camera");
    }
    gl->glViewport(0, 0, w, h);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glDisable(GL_BLEND);
    gl->glUseProgram(v->program);
    gl->glUniform1i(gl->glGetUniformLocation(v->program, "tilePass"), tile ? 1 : 0);
    if (tile) {
        gl->glUniform4f(gl->glGetUniformLocation(v->program, "tileClip"), (float)tile->coverage.x0,
                        (float)tile->coverage.y0, (float)tile->coverage.x1, (float)tile->coverage.y1);
        /* Restrict the full-screen triangle to the tile's rotated screen bounding box. */
        float low[2] = {INFINITY, INFINITY}, high[2] = {-INFINITY, -INFINITY};
        for (unsigned corner = 0; corner < 4; corner++) {
            float p[2];
            if (pc_ui_canvas_to_screen(&v->xf, (float)((corner & 1) ? tile->coverage.x1 : tile->coverage.x0),
                                       (float)((corner & 2) ? tile->coverage.y1 : tile->coverage.y0),
                                       p) == PC_OK) {
                for (size_t axis = 0; axis < 2; axis++) {
                    float origin_axis = axis ? v->xf.y : v->xf.x;
                    low[axis] = fminf(low[axis], (p[axis] - origin_axis) * scale);
                    high[axis] = fmaxf(high[axis], (p[axis] - origin_axis) * scale);
                }
            }
        }
        int x0 = pc_f32_to_i32(fmaxf(0, floorf(low[0])));
        int y0 = pc_f32_to_i32(fmaxf(0, floorf(low[1])));
        int x1 = pc_f32_to_i32(fminf((float)w, ceilf(high[0])));
        int y1 = pc_f32_to_i32(fminf((float)h, ceilf(high[1])));
        gl->glEnable(GL_SCISSOR_TEST);
        gl->glScissor(x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0);
    }
    gl->glUniform1i(gl->glGetUniformLocation(v->program, "channelView"), 2);
    gl->glUniform1i(gl->glGetUniformLocation(v->program, "showChannels"), channels->active ? 1 : 0);
    gl->glUniform1i(gl->glGetUniformLocation(v->program, "displayLut"), 1);
    pc_hdr_preview hdr = v->display.hdr;
    bool hdr_active = !hdr.highlight_compression && (hdr.exposure != 0 || hdr.gamma != 1);
    gl->glUniform4f(gl->glGetUniformLocation(v->program, "displayParams"), 0.f,
                    /* source uploads encode linear composites before filtering */ hdr_active ? 1.f : 0.f,
                    hdr.exposure, hdr.gamma);
    float size = (float)v->display.lut.size;
    gl->glUniform2f(gl->glGetUniformLocation(v->program, "lutMap"), (size - 1) / size, .5f / size);
    gl->glUniform4f(gl->glGetUniformLocation(v->program, "gamutWarning"), v->display.warning[0],
                    v->display.warning[1], v->display.warning[2],
                    v->display.gamut_warning ? v->display.warning_opacity : 0.f);
    gl->glUniform1i(gl->glGetUniformLocation(v->program, "source"), 0);
    gl->glUniform2f(gl->glGetUniformLocation(v->program, "origin"), origin[0], origin[1]);
    gl->glUniform2f(gl->glGetUniformLocation(v->program, "axisX"), right[0] - origin[0],
                    right[1] - origin[1]);
    gl->glUniform2f(gl->glGetUniformLocation(v->program, "axisY"), bottom[0] - origin[0],
                    bottom[1] - origin[1]);
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    pc_rect area = cache->source_rect;
    gl->glUniform4f(gl->glGetUniformLocation(v->program, "sourceRect"), (float)area.x0, (float)area.y0,
                    (float)pc_rect_width(area), (float)pc_rect_height(area));
    gl->glUniform2f(gl->glGetUniformLocation(v->program, "docSize"), (float)view->doc_size[0],
                    (float)view->doc_size[1]);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    gl->glUniform1i(gl->glGetUniformLocation(v->program, "showOutside"),
                    pc_ui_shell_app_state(app)->tool == PC_TOOL_CROP &&
                            pc_ui_shell_app_state(app)->crop_editing &&
                            !pc_ui_shell_app_state(app)->transform.active
                        ? 1
                        : 0);
    color_uniform(gl, v->program, "surround", pc_ui_shell_canvas_color(app));
    color_uniform(gl, v->program, "lightCheck", t->field);
    color_uniform(gl, v->program, "darkCheck", t->card_border);
    const pc_ui_state *ui = pc_ui_shell_app_state(app);
    const pc_ui_view *camera = pc_ui_canvas_active_view(app);
    gl->glUniform1i(gl->glGetUniformLocation(v->program, "pixelGrid"),
                    ui->view.extras && ui->view.show[3] && camera && camera->zoom > 5.f ? 1 : 0);
    gl->glBindTexture(GL_TEXTURE_2D, source);
    gl->glBindVertexArray(v->vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glBindVertexArray(0);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
    return gl->glGetError() == GL_NO_ERROR ? PC_OK : pc_fail(err, PC_ERR_IO, "canvas drawing failed");
}

static double point_size(pc_ui_shell_app *app)
{
    pc_json_value pref = PC_JSON_VALUE_INIT;
    double ppi = 72;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "unitsAndRulers.pointSize", &pref, NULL) == PC_OK) {
        const char *name = yyjson_mut_get_str(pc_json_value_root(&pref));
        if (name && strcmp(name, "traditional") == 0) {
            ppi = 72.27;
        }
    }
    pc_json_value_fini(&pref);
    return ppi;
}

static void draw_rulers(pc_ui_shell_app *app, const pc_ui_canvas_xform *xf)
{
    pc_ui_canvas_xform level;
    if (pc_ui_canvas_ruler_xform(xf, &level) != PC_OK) {
        return;
    }
    struct nk_context *ctx = pc_ui_shell_app_nk(app);
    struct nk_command_buffer *commands = nk_window_get_canvas(ctx);
    const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
    pc_pref_unit unit = PC_UNIT_PIXELS;
    pc_json_value pref = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "unitsAndRulers.rulers", &pref, NULL) == PC_OK) {
        const char *name = yyjson_mut_get_str(pc_json_value_root(&pref));
        if (name) {
            (void)pc_pref_unit_from_suffix(name, strlen(name), &unit);
        }
    }
    pc_json_value_fini(&pref);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(app))->doc);
    double dpi = isfinite(doc->resolution_dpi) && doc->resolution_dpi > 0 ? doc->resolution_dpi : 72.;
    struct nk_rect saved = commands->clip;
    for (size_t axis = 0; axis < 2; axis++) {
        struct nk_rect strip = axis ? nk_rect(xf->x - 16.f, xf->y, 16.f, xf->height)
                                    : nk_rect(xf->x, xf->y - 16.f, xf->width, 16.f);
        nk_push_scissor(commands, strip);
        nk_fill_rect(commands, strip, 0, pc_ui_nk_color(tokens->chrome));
        double pixels =
            pc_pref_unit_to_px(unit, 1, dpi, axis ? doc->size.height : doc->size.width, point_size(app));
        if (!(isfinite(pixels) && pixels > 0.)) {
            pixels = 1.;
        }
        double step = pc_ui_ruler_nice_step(60. / ((double)xf->zoom * pixels), unit == PC_UNIT_PIXELS);
        float first[2], last[2];
        if (pc_ui_canvas_to_doc(&level, xf->x, xf->y, first) != PC_OK ||
            pc_ui_canvas_to_doc(&level, xf->x + xf->width, xf->y + xf->height, last) != PC_OK) {
            continue;
        }
        double low = fmin(first[axis], last[axis]) / pixels, high = fmax(first[axis], last[axis]) / pixels;
        double at = floor(low / step) * step;
        for (size_t tick = 0; tick < 4096 && at <= high; tick++, at += step) {
            float pos[2];
            if (pc_ui_canvas_to_screen(&level, axis ? 0.f : pc_f64_to_f32(at * pixels),
                                       axis ? pc_f64_to_f32(at * pixels) : 0.f, pos) != PC_OK) {
                continue;
            }
            struct nk_color ink = pc_ui_nk_color(tokens->text_dim);
            if (axis) {
                nk_stroke_line(commands, xf->x - 5.f, pos[1], xf->x, pos[1], 1, ink);
            } else {
                nk_stroke_line(commands, pos[0], xf->y - 5.f, pos[0], xf->y, 1, ink);
            }
            char label[40];
            (void)snprintf(label, sizeof label, "%.6g", at == 0. ? 0. : at);
            if (axis) {
                for (size_t ch = 0; label[ch] != '\0'; ch++) {
                    struct nk_rect text = nk_rect(xf->x - 15.f, pos[1] + 2.f + (float)ch * 12.f, 15, 14);
                    nk_draw_text(commands, text, &label[ch], 1, ctx->style.font,
                                 pc_ui_nk_color(tokens->chrome), ink);
                }
            } else {
                nk_draw_text(commands, nk_rect(pos[0] + 2.f, xf->y - 16.f, 58, 12), label, (int)strlen(label),
                             ctx->style.font, pc_ui_nk_color(tokens->chrome), ink);
            }
        }
    }
    nk_push_scissor(commands, saved);
}

pc_status pc_ui_canvas_view_draw(pc_ui_shell_app *app, pc_ui_canvas_view *v, const gl33 *gl, struct nk_rect r,
                                 pc_error *err)
{
    pc_status rc = pc_ui_canvas_sync(app, v, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Reclaim a closed document's checkpoint with this window's GL context current. */
    pc_doc_id retained = pc_gpu_compositor_document(v->gpu);
    bool open = false;
    for (size_t i = 0; retained && i < v->n_documents; i++)
        if (v->documents[i] == retained)
            open = true;
    if (retained && !open)
        pc_gpu_compositor_forget_document(v->gpu, retained);
    pc_ui_lasso_sync(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app));
    pc_ui_shell_crop_tick(app);
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    if (!view || r.w < 1 || r.h < 1) {
        v->laid_out = false;
        pc_ui_tool_gesture_fini(&v->gesture);
        return PC_OK;
    }
    const pc_engine_doc_state *active = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (v->guide_drag.active &&
        (v->guide_drag.document != pc_doc_arc_get(active->doc)->id ||
         v->guide_drag.revision != active->revision || pc_ui_shell_app_state(app)->extras.lock_guides)) {
        v->guide_drag.active = false;
    }
    if (v->gesture.active &&
        (v->gesture.document != pc_doc_arc_get(active->doc)->id || v->gesture.revision != active->revision)) {
        pc_ui_tool_gesture_fini(&v->gesture);
    }
    pc_engine_doc_state preview;
    pc_doc_arc *floating = NULL;
    rc = pc_engine_floating_document(pc_ui_shell_app_session(app), &floating, err);
    if (rc != PC_OK)
        return rc;
    if (floating != v->floating_display) {
        pc_doc_arc *retained_float = pc_doc_arc_retain(floating);
        pc_doc_arc_release(v->floating_display);
        v->floating_display = retained_float;
        v->floating_generation++;
    }
    if (floating) {
        preview = *active;
        preview.doc = floating;
        preview.revision = UINT64_MAX / 4 - v->floating_generation;
        active = &preview;
    }
    uint64_t dialog_generation = 0;
    pc_doc_arc *dialog_preview = pc_ui_shell_dialog_preview(app, &dialog_generation);
    if (dialog_preview) {
        preview = *active;
        preview.doc = dialog_preview;
        preview.revision = UINT64_MAX / 2 - dialog_generation;
        active = &preview;
    }
    uint64_t transform_generation = 0;
    pc_doc_arc *transform_preview = pc_ui_shell_transform_preview(app, &transform_generation);
    if (transform_preview) {
        preview = *active;
        preview.doc = transform_preview;
        preview.revision = UINT64_MAX / 3 - transform_generation;
        active = &preview;
    }
    if (v->gesture.live || v->gesture.move_preview) {
        preview = *active;
        preview.doc =
            v->gesture.live ? pc_engine_live_stroke_document(v->gesture.live) : v->gesture.move_preview;
        preview.revision = UINT64_MAX - v->gesture.preview_revision;
        if (v->gesture.move_preview && v->gesture.preview_layer) {
            preview.has_active_layer = true;
            preview.active_layer = v->gesture.preview_layer;
        }
        active = &preview;
    }
    bool rulers = pc_ui_shell_app_state(app)->extras.rulers;
    if (rulers) {
        r.x += 16.f;
        r.y += 16.f;
        r.w -= 16.f;
        r.h -= 16.f;
    }
    if (r.w < 1.f || r.h < 1.f) {
        v->laid_out = false;
        return PC_OK;
    }
    float ppp = pc_ui_shell_app_ui_scale(app);
    const pc_ui_view_options *options = &pc_ui_shell_app_state(app)->view;
    float aspect = pc_ui_view_display_aspect(options);
    if (view->fit_pending || view->fill_pending) {
        pc_ui_canvas_fit(view, r.w, r.h, ppp, aspect, view->fill_pending);
    }
    view->zoom = pc_ui_zoom_clamp(view->zoom, view->doc_size);
    view->rotation = pc_ui_rotation_wrap(view->rotation);
    if (!isfinite(view->center[0])) {
        view->center[0] = (float)view->doc_size[0] / 2.f;
    }
    if (!isfinite(view->center[1])) {
        view->center[1] = (float)view->doc_size[1] / 2.f;
    }
    if (!preference_bool(app, "tools.overscroll", true)) {
        (void)pc_ui_scroll_clamp_view(view, r.w, r.h, ppp, aspect);
    }
    if (v->xf.x != r.x || v->xf.y != r.y || v->xf.width != r.w || v->xf.height != r.h ||
        v->xf.aspect != aspect || v->xf.zoom != view->zoom / ppp ||
        v->cache.document !=
            pc_doc_arc_get(pc_engine_session_active(pc_ui_shell_app_session(app))->doc)->id ||
        v->xf.flip != options->flip_horizontal || v->xf.rotation != view->rotation ||
        !pc_ui_view_standard(options)) {
        v->scroll_drag.active = false;
    }
    v->xf = (pc_ui_canvas_xform){.x = r.x,
                                 .y = r.y,
                                 .width = r.w,
                                 .height = r.h,
                                 .zoom = view->zoom / ppp,
                                 .center = {view->center[0], view->center[1]},
                                 .rotation = view->rotation,
                                 .aspect = aspect,
                                 .flip = options->flip_horizontal};
    v->laid_out = true;
    pc_json_value backend = PC_JSON_VALUE_INIT;
    bool prefer_cpu = false;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "performance.gpuBackend", &backend, NULL) ==
        PC_OK) {
        const char *name = yyjson_mut_get_str(pc_json_value_root(&backend));
        prefer_cpu = name && strcmp(name, "cpu") == 0;
    }
    pc_json_value_fini(&backend);
    bool use_gpu = preference_bool(app, "performance.useGpu", true) && !prefer_cpu;
    if (use_gpu && !v->gpu_tried) {
        v->gpu_tried = true;
        (void)pc_gpu_compositor_new(gl, &v->gpu, NULL);
    }
    bool crop_plane = pc_ui_shell_app_state(app)->tool == PC_TOOL_CROP &&
                      pc_ui_shell_app_state(app)->crop_editing &&
                      !pc_ui_shell_app_state(app)->transform.active;
    rc = crop_plane
             ? pc_ui_canvas_cache_update_plane(&v->cache, active, &v->xf, use_gpu ? v->gpu : NULL, err)
             : pc_ui_canvas_cache_update_view_gpu(&v->cache, active, &v->xf, use_gpu ? v->gpu : NULL, err);
    if (rc == PC_OK) {
        rc = render_camera(app, v, gl, &v->cache, NULL, active, err);
    }
    if (rc == PC_OK && !crop_plane) {
        pc_status tiled =
            pc_ui_canvas_tiles_update_gpu(&v->tiles, active, &v->xf, use_gpu ? v->gpu : NULL, err);
        if (tiled != PC_OK && tiled != PC_ERR_LIMIT) {
            rc = tiled;
        } else if (tiled == PC_OK) {
            for (size_t i = 0; rc == PC_OK && i < v->tiles.count; i++) {
                v->tile_uploads[i].coverage = v->tiles.items[i].coverage;
                rc = render_camera(app, v, gl, &v->tiles.items[i].cache, &v->tile_uploads[i], active, err);
            }
        }
    }
    if (rc == PC_OK) {
        struct nk_image image = nk_image_id((int)v->target);
        nk_draw_image(nk_window_get_canvas(pc_ui_shell_app_nk(app)), r, &image, nk_rgba(255, 255, 255, 255));
    }
    if (rc == PC_OK)
        pc_ui_shell_artboard_pasteboard(app, &v->xf);
    if (rc == PC_OK) {
        rc = pc_ui_overlays_draw(app, active, &v->xf, &v->overlays, &v->guide_drag, err);
    }
    if (rc == PC_OK) {
        rc = pc_ui_shell_vector_preview(app, &v->vector_preview, &v->xf, &v->gesture, err);
    }
    if (rc == PC_OK) {
        rc = pc_ui_shell_pen_draw(app, &v->pen_preview, &v->xf, err);
    }
    if (rc == PC_OK) {
        pc_ui_shell_transform_controls_draw(app, &v->xf);
        pc_ui_shell_transform_draw(app, &v->xf);
        pc_ui_shell_crop_draw(app, &v->xf);
        pc_ui_shell_snap_draw(app, &v->xf);
        pc_ui_shell_lasso_draw(app, &v->xf);
        pc_ui_shell_slice_draw(app, &v->xf);
        pc_ui_shell_artboard_frames(app, &v->xf);
        pc_ui_shell_sampling_draw(app, &v->xf);
    }
    if (rulers && rc == PC_OK) {
        draw_rulers(app, &v->xf);
    }
    memset(v->bars, 0, sizeof v->bars);
    if (rc == PC_OK && pc_ui_view_standard(options) &&
        pc_ui_scrollbars_layout(&v->xf, view->doc_size, preference_bool(app, "tools.overscroll", true),
                                v->bars)) {
        const pc_ui_tokens *tokens = pc_ui_shell_app_tokens(app);
        struct nk_command_buffer *commands = nk_window_get_canvas(pc_ui_shell_app_nk(app));
        for (size_t axis = 0; axis < 2; axis++) {
            pc_ui_scrollbar *bar = &v->bars[axis];
            if (!bar->shown) {
                continue;
            }
            if (v->scroll_drag.active && v->scroll_drag.bar.axis == axis) {
                pc_ui_scroll_thumb(bar->span, v->scroll_drag.bar.extent, bar->rect[axis ? 3 : 2], bar->thumb);
            }
            const float *rbar = bar->rect;
            struct nk_rect track = nk_rect(rbar[0], rbar[1], rbar[2], rbar[3]);
            nk_fill_rect(commands, track, 0, pc_ui_nk_color(tokens->chrome));
            nk_stroke_line(commands, track.x, track.y, axis ? track.x : track.x + track.w,
                           axis ? track.y + track.h : track.y, 1, pc_ui_nk_color(tokens->separator));
            struct nk_rect thumb = track;
            if (axis) {
                thumb.y += bar->thumb[0];
                thumb.h = bar->thumb[1];
            } else {
                thumb.x += bar->thumb[0];
                thumb.w = bar->thumb[1];
            }
            pc_ui_shell_widget_mapping(
                app, axis ? "view.scroll.vertical.thumb" : "view.scroll.horizontal.thumb", thumb);
            pc_ui_shell_widget_mapping(
                app, axis ? "view.scroll.vertical.track" : "view.scroll.horizontal.track", track);
            thumb.x += 3;
            thumb.y += 3;
            thumb.w = fmaxf(0, thumb.w - 6);
            thumb.h = fmaxf(0, thumb.h - 6);
            struct nk_vec2 mouse = pc_ui_shell_app_nk(app)->input.mouse.pos;
            bool hot = (v->scroll_drag.active && v->scroll_drag.bar.axis == axis) ||
                       (mouse.x >= thumb.x - 3 && mouse.x < thumb.x + thumb.w + 3 && mouse.y >= thumb.y - 3 &&
                        mouse.y < thumb.y + thumb.h + 3);
            nk_fill_rect(commands, thumb, 3, pc_ui_nk_color(hot ? tokens->text_dim : tokens->text_faint));
        }
    }
    pc_ui_shell_app_gpu_check(app);
    if (rc == PC_OK) {
        pc_ui_brush_resize_draw(app, v);
        pc_ui_native_cursor_draw(app, v);
    }
    return rc;
}

static bool preference_bool(pc_ui_shell_app *app, const char *path, bool fallback)
{
    pc_json_value value = PC_JSON_VALUE_INIT;
    if (pc_engine_prefs_get(pc_ui_shell_app_session(app), path, &value, NULL) != PC_OK) {
        return fallback;
    }
    yyjson_mut_val *root = pc_json_value_root(&value);
    bool result = yyjson_mut_is_bool(root) ? yyjson_mut_get_bool(root) : fallback;
    pc_json_value_fini(&value);
    return result;
}

static float pointer_angle(const pc_ui_canvas_xform *xf, float x, float y)
{
    return atan2f(y - (xf->y + xf->height / 2.f), x - (xf->x + xf->width / 2.f)) * 57.29577951308232f;
}

bool pc_ui_canvas_view_event(pc_ui_shell_app *app, pc_ui_canvas_view *cv, const SDL_Event *e)
{
    if (!e) {
        return false;
    }
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST)
        cv->brush_resize.active = false;
    if (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) {
        pc_ui_key key = pc_ui_shell_input_key(e->key.key);
        if (key > PC_KEY_NONE && key <= PC_KEY_EXCLAMATIONMARK) {
            cv->held_keys[key] = e->key.down;
        }
        cv->held_mods = pc_ui_shell_input_mods(e->key.mod);
    } else if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        memset(cv->held_keys, 0, sizeof cv->held_keys);
        cv->held_mods = 0;
        memset(cv->fingers, 0, sizeof cv->fingers);
    }
    if ((e->type == SDL_EVENT_MOUSE_MOTION && e->motion.which == SDL_TOUCH_MOUSEID) ||
        ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
         e->button.which == SDL_TOUCH_MOUSEID)) {
        return false;
    }
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        pc_ui_tool_gesture_fini(&cv->gesture);
        cv->pen_down = false;
    }
    if (!cv->laid_out) {
        return false;
    }
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    if (!view) {
        cv->dragging = false;
        cv->scroll_drag.active = false;
        return false;
    }
    pc_ui_canvas_xform current = cv->xf;
    float ppp = pc_ui_shell_app_ui_scale(app);
    current.zoom = view->zoom / ppp;
    current.center[0] = view->center[0];
    current.center[1] = view->center[1];
    current.rotation = view->rotation;
    current.aspect = pc_ui_view_display_aspect(&pc_ui_shell_app_state(app)->view);
    current.flip = pc_ui_shell_app_state(app)->view.flip_horizontal;
    if (!pc_ui_view_standard(&pc_ui_shell_app_state(app)->view)) {
        cv->scroll_drag.active = false;
    }
    const pc_engine_doc_state *active = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!active || pc_doc_arc_get(active->doc)->id != cv->cache.document) {
        cv->dragging = false;
        cv->scroll_drag.active = false;
        return false;
    }
    const pc_ui_canvas_xform *xf = &current;
    float density = SDL_GetWindowPixelDensity(pc_ui_shell_app_sdl_window(app));
    float factor = density / ppp, x, y;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        pc_ui_tool_gesture_fini(&cv->gesture);
        cv->dragging = false;
        cv->scroll_drag.active = false;
        cv->guide_drag.active = false;
        cv->pen_down = false;
        return false;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP ||
        e->type == SDL_EVENT_MOUSE_MOTION) {
        float event_x = (e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.x : e->button.x) * factor;
        float event_y = (e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.y : e->button.y) * factor;
        if (!cv->dragging && pc_ui_shell_dialog_at(app, event_x, event_y)) {
            return false;
        }
    }
    if (e->type == SDL_EVENT_MOUSE_WHEEL &&
        pc_ui_shell_dialog_at(app, e->wheel.mouse_x * factor, e->wheel.mouse_y * factor)) {
        return false;
    }
    if (pc_ui_brush_resize_event(app, cv, xf, e, factor))
        return true;
    if (pc_ui_canvas_touch_event(app, cv, xf, e)) {
        return true;
    }
    if (!pc_ui_shell_app_state(app)->n_dialogs && pc_ui_guide_event(app, xf, &cv->guide_drag, e)) {
        return true;
    }
    if (!pc_ui_shell_app_state(app)->n_dialogs && e->type == SDL_EVENT_KEY_DOWN &&
        e->key.key == SDLK_ESCAPE && pc_ui_shell_app_state(app)->tool == PC_TOOL_ROTATE_VIEW) {
        view->rotation = 0;
        cv->dragging = false;
        cv->scroll_drag.active = false;
        return true;
    }
    if (cv->gesture.active && e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) {
        pc_ui_tool_gesture_fini(&cv->gesture);
        return true;
    }
    if (cv->gesture.active && cv->gesture.has_shape_preview && cv->gesture.count &&
        (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_KEY_UP) &&
        cv->gesture.modifiers != cv->held_mods) {
        pc_ui_tool_point point = cv->gesture.points[cv->gesture.count - 1];
        pc_ui_state temporary = *pc_ui_shell_app_state(app);
        temporary.tool = cv->gesture.tool;
        pc_json_value out = PC_JSON_VALUE_INIT;
        pc_error error = {0};
        pc_status status =
            pc_ui_tool_gesture_event(&cv->gesture, pc_ui_shell_app_session(app), &temporary,
                                     PC_UI_POINTER_MOVE, point, cv->held_mods, view->zoom, &out, &error);
        pc_json_value_fini(&out);
        if (status != PC_OK) {
            pc_ui_state *ui = pc_ui_shell_app_state(app);
            free(ui->status);
            ui->status = pc_strndup(error.msg, strlen(error.msg));
            ui->status_error = true;
        }
    }
    if (cv->scroll_drag.active &&
        (e->type == SDL_EVENT_MOUSE_MOTION ||
         (e->type == SDL_EVENT_MOUSE_BUTTON_UP && e->button.button == SDL_BUTTON_LEFT))) {
        bool release = e->type == SDL_EVENT_MOUSE_BUTTON_UP;
        x = (release ? e->button.x : e->motion.x) * factor;
        y = (release ? e->button.y : e->motion.y) * factor;
        unsigned axis = cv->scroll_drag.bar.axis;
        float center;
        if (pc_ui_scrollbar_motion(&cv->scroll_drag, x, y, release, &center)) {
            view->center[axis] = center;
            view->fit_pending = view->fill_pending = false;
        }
        return true;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_LEFT &&
        pc_ui_view_standard(&pc_ui_shell_app_state(app)->view) &&
        pc_ui_scrollbar_press(cv->bars, e->button.x * factor, e->button.y * factor, &cv->scroll_drag)) {
        cv->dragging = false;
        return true;
    }
    if (e->type == SDL_EVENT_MOUSE_WHEEL) {
        (void)SDL_GetMouseState(&x, &y);
        x *= factor;
        y *= factor;
        if (x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height) {
            return false;
        }
        SDL_Keymod mods = SDL_GetModState();
        double now = (double)e->wheel.timestamp * 1e-9;
        bool ctrl = (mods & SDL_KMOD_CTRL) != 0;
        bool recent = cv->last_pinch > 0 && now >= cv->last_pinch && now - cv->last_pinch < .25;
        if (ctrl && (recent || e->wheel.x != truncf(e->wheel.x) || e->wheel.y != truncf(e->wheel.y))) {
            cv->last_pinch = now;
            if (preference_bool(app, "enhancedControls.zoomWithTrackpadPinch", true)) {
                float exponent = fminf(40, fmaxf(-40, .2f * (e->wheel.x + e->wheel.y)));
                float zoom = expf(e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -exponent : exponent);
                (void)pc_ui_canvas_anchor(view, xf, x, y, view->zoom * zoom, ppp);
            }
            return true;
        }
        float dx = e->wheel.x * 40.f, dy = e->wheel.y * 40.f;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            dx = -dx;
            dy = -dy;
        }
        bool sideways = (mods & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
        pc_ui_wheel_input in = {.scroll = {sideways ? dx + dy : dx, sideways ? 0.f : dy},
                                .raw = {sideways ? dx + dy : dx, sideways ? 0.f : dy},
                                .notch = 40.f,
                                .alt = (mods & SDL_KMOD_ALT) != 0 && !sideways,
                                .zoom_with_wheel =
                                    preference_bool(app, "general.zoomWithScrollWheel", false)};
        float result[2];
        pc_ui_wheel_kind kind = pc_ui_wheel_classify(in, result);
        if (kind == PC_UI_WHEEL_ZOOM) {
            (void)pc_ui_canvas_anchor(view, xf, x, y, view->zoom * result[0], ppp);
        } else if (kind == PC_UI_WHEEL_PAN) {
            (void)pc_ui_canvas_pan(view, xf, result[0], result[1]);
        }
        return true;
    }
    if (pc_ui_shell_context_canvas(app, xf, e, factor))
        return true;
    pc_ui_temporary snap_held_tool;
    pc_ui_tool chosen_tool = pc_ui_shell_app_state(app)->tool;
    bool tool_override =
        !pc_ui_shell_app_state(app)->lasso.down && !pc_ui_shell_sampling_runtime(app)->active &&
        !pc_ui_shell_app_state(app)->slices.dragging && !pc_ui_shell_app_state(app)->artboard_tool.active &&
        !pc_ui_shell_crop_runtime(app)->drag.active && !pc_ui_shell_crop_runtime(app)->straightening &&
        !pc_ui_shell_transform_runtime(app)->dragging &&
        pc_ui_temporary_resolve(pc_ui_shell_app_session(app), chosen_tool, cv->held_keys, cv->held_mods,
                                pc_ui_shell_keyboard_captured(app), true, &snap_held_tool) &&
        pc_ui_temporary_tool(snap_held_tool) != chosen_tool;
    if (pc_ui_shell_layer_picker_canvas(app, xf, e, factor))
        return true;
    if (!tool_override && pc_ui_shell_sampling_canvas(app, xf, e, factor))
        return true;
    if (!tool_override && pc_ui_shell_artboard_canvas(app, xf, e, factor))
        return true;
    if (!tool_override && pc_ui_shell_slice_canvas(app, xf, e, factor))
        return true;
    if (!tool_override && pc_ui_shell_lasso_canvas(app, xf, e, factor))
        return true;
    if (!tool_override && pc_ui_shell_crop_canvas(app, xf, e, factor))
        return true;
    if (pc_ui_shell_app_state(app)->transform.active && !cv->dragging) {
        pc_ui_temporary temporary;
        bool held = pc_ui_temporary_resolve(pc_ui_shell_app_session(app), pc_ui_shell_app_state(app)->tool,
                                            cv->held_keys, cv->held_mods, pc_ui_shell_keyboard_captured(app),
                                            true, &temporary);
        pc_ui_tool tool = held ? pc_ui_temporary_tool(temporary) : PC_TOOL_MOVE;
        bool navigation =
            held && (tool == PC_TOOL_HAND || tool == PC_TOOL_ZOOM || tool == PC_TOOL_ROTATE_VIEW);
        if ((!navigation || pc_ui_shell_transform_runtime(app)->dragging) &&
            pc_ui_shell_transform_canvas(app, xf, e, factor))
            return true;
    }
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        x = e->button.x * factor;
        y = e->button.y * factor;
        if (!e->button.down && cv->dragging && e->button.button == cv->drag_button) {
            if (cv->drag_tool == PC_TOOL_ZOOM && !cv->moved) {
                int direction = cv->zoom_out ? -1 : 1;
                (void)pc_ui_canvas_anchor(view, xf, cv->press_pointer[0], cv->press_pointer[1],
                                          pc_ui_zoom_step(view->zoom, direction, view->doc_size), ppp);
            }
            cv->dragging = false;
            cv->scroll_drag.active = false;
            return true;
        }
        if (x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height) {
            return false;
        }
        pc_ui_tool tool = pc_ui_shell_app_state(app)->tool;
        pc_ui_temporary temporary;
        bool held_tool =
            pc_ui_temporary_resolve(pc_ui_shell_app_session(app), tool, cv->held_keys, cv->held_mods,
                                    pc_ui_shell_keyboard_captured(app), true, &temporary);
        cv->zoom_out = (cv->held_mods & PC_UI_MOD_ALT) != 0;
        if (held_tool) {
            tool = pc_ui_temporary_tool(temporary);
            cv->zoom_out = temporary == PC_UI_TEMP_ZOOM_OUT;
        }
        if (e->button.button == SDL_BUTTON_MIDDLE) {
            tool = PC_TOOL_HAND;
        }
        if (pc_ui_shell_app_state(app)->n_dialogs && tool == PC_TOOL_ROTATE_VIEW) {
            return false;
        }
        if (e->button.down && e->button.button == SDL_BUTTON_RIGHT && !cv->gesture.active &&
            !pc_ui_shell_app_state(app)->n_dialogs && !pc_ui_shell_jobs_locked(app) &&
            (pc_ui_tool_is_brushlike(tool) || tool == PC_TOOL_QUICK_SELECTION)) {
            pc_ui_brush_picker *picker = &pc_ui_shell_app_state(app)->brush_picker;
            picker->open = true;
            picker->point[0] = x;
            picker->point[1] = y;
            return true;
        }
        if (e->button.down &&
            (e->button.button == SDL_BUTTON_LEFT || e->button.button == SDL_BUTTON_MIDDLE) &&
            (tool == PC_TOOL_HAND || tool == PC_TOOL_ZOOM || tool == PC_TOOL_ROTATE_VIEW)) {
            pc_ui_tool_gesture_fini(&cv->gesture);
            cv->dragging = true;
            cv->drag_tool = tool;
            cv->drag_button = e->button.button;
            cv->moved = false;
            cv->last_pointer[0] = x;
            cv->last_pointer[1] = y;
            cv->press_pointer[0] = x;
            cv->press_pointer[1] = y;
            cv->press_zoom = view->zoom;
            cv->press_rotation = view->rotation;
            cv->press_angle = pointer_angle(xf, x, y);
            return true;
        }
    }
    if (e->type == SDL_EVENT_MOUSE_MOTION && cv->dragging) {
        x = e->motion.x * factor;
        y = e->motion.y * factor;
        if (!isfinite(x) || !isfinite(y)) {
            return true;
        }
        if (fabsf(x - cv->press_pointer[0]) + fabsf(y - cv->press_pointer[1]) > 2.f) {
            cv->moved = true;
        }
        if (cv->drag_tool == PC_TOOL_HAND) {
            (void)pc_ui_canvas_pan(view, xf, x - cv->last_pointer[0], y - cv->last_pointer[1]);
        } else if (cv->drag_tool == PC_TOOL_ROTATE_VIEW) {
            float delta = pc_ui_rotation_wrap(pointer_angle(xf, x, y) - cv->press_angle);
            float angle = pc_ui_rotation_wrap(cv->press_rotation + (xf->flip ? -delta : delta));
            view->rotation = (SDL_GetModState() & SDL_KMOD_SHIFT) ? pc_ui_rotation_snap(angle, 15) : angle;
        } else if (cv->drag_tool == PC_TOOL_ZOOM && cv->moved) {
            float exponent = fminf(40.f, fmaxf(-40.f, (x - cv->press_pointer[0]) / 100.f));
            (void)pc_ui_canvas_anchor(view, xf, cv->press_pointer[0], cv->press_pointer[1],
                                      cv->press_zoom * exp2f(exponent), ppp);
        }
        cv->last_pointer[0] = x;
        cv->last_pointer[1] = y;
        return true;
    }
    if (pc_ui_shell_pen_canvas(app, xf, e, factor)) {
        return true;
    }
    if (pc_ui_shell_type_canvas(app, xf, e, factor)) {
        return true;
    }
    if (!pc_ui_shell_app_state(app)->n_dialogs && !pc_ui_shell_jobs_locked(app) &&
        !pc_ui_shell_keyboard_captured(app) &&
        (cv->gesture.active || pc_ui_tool_gesture_supported(pc_ui_shell_app_state(app)->tool) ||
         (tool_override && pc_ui_tool_gesture_supported(pc_ui_temporary_tool(snap_held_tool)))) &&
        (e->type == SDL_EVENT_MOUSE_MOTION ||
         ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
          e->button.button == SDL_BUTTON_LEFT))) {
        bool moving = e->type == SDL_EVENT_MOUSE_MOTION;
        x = (moving ? e->motion.x : e->button.x) * factor;
        y = (moving ? e->motion.y : e->button.y) * factor;
        bool down = e->type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        if (!cv->gesture.active &&
            (!down || x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height)) {
            return false;
        }
        float doc[2];
        if (pc_ui_canvas_to_doc(xf, x, y, doc) != PC_OK) {
            return cv->gesture.active;
        }
        pc_json_value result = PC_JSON_VALUE_INIT;
        pc_error error;
        pc_ui_tool_point point = cv->pen_event ? cv->pen : (pc_ui_tool_point){.pressure = 1, .wheel = 1};
        point.x = doc[0];
        point.y = doc[1];
        point.time_ms = (float)((double)e->common.timestamp * 1e-6);
        const pc_ui_state *state = pc_ui_shell_app_state(app);
        pc_ui_state temporary = *state;
        if (cv->gesture.active) {
            temporary.tool = cv->gesture.handoff ? cv->gesture.handoff_tool : cv->gesture.tool;
        } else {
            pc_ui_temporary held;
            if (pc_ui_temporary_resolve(pc_ui_shell_app_session(app), state->tool, cv->held_keys,
                                        cv->held_mods, false, true, &held)) {
                temporary.tool = pc_ui_temporary_tool(held);
            } else if ((cv->pen_event && cv->pen_eraser) ||
                       pc_ui_erase_held(pc_ui_shell_app_session(app), state->tool, cv->held_keys,
                                        cv->held_mods, false)) {
                temporary.tool = PC_TOOL_ERASER;
            }
        }
        pc_ui_state *actual = pc_ui_shell_app_state(app);
        pc_ui_tool chosen = actual->tool;
        actual->tool = temporary.tool;
        pc_status status = pc_ui_shell_app_pointer(app,
                                                   down     ? PC_UI_POINTER_DOWN
                                                   : moving ? PC_UI_POINTER_MOVE
                                                            : PC_UI_POINTER_UP,
                                                   point, cv->held_mods, &result, &error);
        actual->tool = chosen;
        pc_json_value_fini(&result);
        if (!moving && status == PC_OK) {
            pc_ui_shell_app_changed(app);
        }
        if (status != PC_OK && status != PC_ERR_NOT_FOUND) {
            pc_ui_state *ui = pc_ui_shell_app_state(app);
            free(ui->status);
            ui->status = pc_strndup(error.msg, strlen(error.msg));
            ui->status_error = true;
        }
        return status != PC_ERR_NOT_FOUND;
    }
    return false;
}

pc_status pc_ui_canvas_view_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                    pc_json_value *out, pc_error *err)
{
    pc_ui_extras *extras = &pc_ui_shell_app_state(app)->extras;
    bool *toggle = strcmp(id, "view.rulers") == 0        ? &extras->rulers
                   : strcmp(id, "view.snap") == 0        ? &extras->snap
                   : strcmp(id, "view.show.grid") == 0   ? &extras->grid
                   : strcmp(id, "view.show.guides") == 0 ? &extras->guides
                   : strcmp(id, "view.lockGuides") == 0  ? &extras->lock_guides
                                                         : NULL;
    if (toggle) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        bool on;
        if (!pc_json_as_bool(pc_json_get(params, "on"), &on)) {
            on = !*toggle;
        }
        yyjson_mut_val *value = yyjson_mut_bool(doc, on);
        pc_status rc = pc_json_value_from_mut(value, out, err);
        yyjson_mut_doc_free(doc);
        if (rc == PC_OK) {
            *toggle = on;
            if (on && (toggle == &extras->grid || toggle == &extras->guides)) {
                pc_ui_shell_app_state(app)->view.extras = true;
            }
        }
        return rc;
    }
    bool in = strcmp(id, "view.zoomIn") == 0, zoom_out = strcmp(id, "view.zoomOut") == 0;
    bool fit = strcmp(id, "view.fitOnScreen") == 0, actual = strcmp(id, "view.actualPixels") == 0;
    bool twice = strcmp(id, "view.twoHundredPercent") == 0, reset = strcmp(id, "view.resetView") == 0;
    bool rotate = strcmp(id, "view.rotateView") == 0;
    bool fill = !strcmp(id, "view.fillScreen"), print = !strcmp(id, "view.printSize");
    if (!in && !zoom_out && !fit && !actual && !twice && !reset && !rotate && !fill && !print) {
        return PC_ERR_NOT_FOUND;
    }
    pc_ui_view *v = pc_ui_canvas_active_view(app);
    if (!v) {
        return pc_fail(err, PC_ERR_INVALID, rotate || reset ? "no document open" : "no document");
    }
    pc_ui_view next = *v;
    if (rotate) {
        float angle;
        pc_status rc = pc_ui_rotation_params(v->rotation, params, &angle, err);
        if (rc != PC_OK) {
            return rc;
        }
        next.rotation = angle;
    } else if (reset) {
        next.rotation = 0;
    } else if (fit || fill) {
        next.fit_pending = !fill;
        next.fill_pending = fill;
    } else {
        next.zoom = pc_ui_zoom_clamp(actual  ? 1.f
                                     : twice ? 2.f
                                             : pc_ui_zoom_step(v->zoom, in ? 1 : -1, v->doc_size),
                                     v->doc_size);
        next.fit_pending = false;
        next.fill_pending = false;
    }
    if (print) {
        const pc_engine_doc_state *state = pc_engine_session_active(pc_ui_shell_app_session(app));
        double dpi = state ? pc_doc_arc_get(state->doc)->resolution_dpi : 72, screen = 72;
        pc_json_value value = PC_JSON_VALUE_INIT;
        if (pc_engine_prefs_get(pc_ui_shell_app_session(app), "unitsAndRulers.screenResolution", &value,
                                NULL) == PC_OK)
            screen = yyjson_mut_get_num(pc_json_value_root(&value));
        pc_json_value_fini(&value);
        next.zoom = pc_ui_zoom_clamp((float)(fmax(1, screen) / fmax(1, dpi)), v->doc_size);
    }
    yyjson_val *point = pc_json_get(params, "point");
    if ((in || zoom_out) && point) {
        double anchor[2];
        if (!yyjson_is_arr(point) || yyjson_arr_size(point) != 2 ||
            !pc_json_as_f64(yyjson_arr_get(point, 0), &anchor[0]) ||
            !pc_json_as_f64(yyjson_arr_get(point, 1), &anchor[1]) || !isfinite(anchor[0]) ||
            !isfinite(anchor[1]) || fabs(anchor[0]) > 1000000 || fabs(anchor[1]) > 1000000)
            return pc_fail(err, PC_ERR_INVALID, "invalid zoom anchor");
        pc_ui_canvas_xform xf;
        float position[2];
        if (pc_ui_shell_app_canvas_mapping(app, &xf) &&
            pc_ui_canvas_to_screen(&xf, (float)anchor[0], (float)anchor[1], position) == PC_OK)
            (void)pc_ui_canvas_anchor(&next, &xf, position[0], position[1], next.zoom,
                                      pc_ui_shell_app_ui_scale(app));
    }
    if (!rotate && !reset && !fill)
        next.fill_pending = false;
    if (rotate || reset || twice || print || fill) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *value = yyjson_mut_obj(doc);
        if (rotate || reset) {
            yyjson_mut_obj_add_real(doc, value, "rotation", next.rotation);
        }
        if (reset || twice || print) {
            yyjson_mut_obj_add_real(doc, value, "zoom", next.zoom);
        }
        if (reset) {
            yyjson_mut_val *center = yyjson_mut_arr(doc);
            yyjson_mut_arr_add_real(doc, center, next.center[0]);
            yyjson_mut_arr_add_real(doc, center, next.center[1]);
            yyjson_mut_obj_add_val(doc, value, "center", center);
        }
        pc_status rc = pc_json_value_from_mut(value, out, err);
        yyjson_mut_doc_free(doc);
        if (rc != PC_OK) {
            return rc;
        }
    } else {
        *out = (pc_json_value)PC_JSON_VALUE_INIT;
    }
    if (in || zoom_out || fit || actual || twice || print || fill) {
        pc_status rc = pc_engine_session_record_view(pc_ui_shell_app_session(app), id, params, err);
        if (rc != PC_OK) {
            pc_json_value_fini(out);
            return rc;
        }
    }
    *v = next;
    return PC_OK;
}
