#include "canvas_view.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "ui/widgets.h"
#include "engine/prefs.h"

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
    if (gl) {
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
    "uniform sampler2D source; uniform vec2 origin,axisX,axisY,docSize;"
    "uniform vec4 surround,lightCheck,darkCheck; in vec2 uv; out vec4 color;"
    "void main(){vec2 d=origin+axisX*uv.x+axisY*uv.y;"
    "if(any(lessThan(d,vec2(0)))||any(greaterThanEqual(d,docSize))){color=surround;return;}"
    "vec4 p=texture(source,d/docSize);"
    "float k=mod(floor(d.x/8.0)+floor(d.y/8.0),2.0);"
    "vec3 bg=mix(lightCheck.rgb,darkCheck.rgb,k);"
    "color=vec4(clamp(p.rgb+bg*(1.0-p.a),0.0,1.0),1);}";

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

static pc_status render_camera(pc_ui_shell_app *app, pc_ui_canvas_view *v, const gl33 *gl, pc_error *err)
{
    pc_status rc = init_gpu(v, gl, err);
    if (rc != PC_OK) {
        return rc;
    }
    GLint bound = 0, maximum = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    float scale = pc_ui_shell_app_ui_scale(app);
    double width = ceil((double)v->xf.width * scale), height = ceil((double)v->xf.height * scale);
    if (width < 1 || height < 1 || width > maximum || height > maximum || width * height > 67108864.) {
        return pc_fail(err, PC_ERR_LIMIT, "canvas viewport exceeds texture budget");
    }
    int w = (int)width, h = (int)height;
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, v->source);
    texture_params(gl);
    if (v->uploaded != v->cache.generation) {
        const pc_compose_buffer *img = &v->cache.image;
        uint32_t iw = pc_rect_width(img->rect), ih = pc_rect_height(img->rect);
        if (iw > (uint32_t)maximum || ih > (uint32_t)maximum) {
            return pc_fail(err, PC_ERR_LIMIT, "canvas source exceeds GPU texture size");
        }
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (int)iw, (int)ih, 0, GL_RGBA, GL_FLOAT, img->px);
        if (gl->glGetError() != GL_NO_ERROR) {
            return pc_fail(err, PC_ERR_IO, "canvas upload failed");
        }
        v->uploaded = v->cache.generation;
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
    gl->glUniform1i(gl->glGetUniformLocation(v->program, "source"), 0);
    gl->glUniform2f(gl->glGetUniformLocation(v->program, "origin"), origin[0], origin[1]);
    gl->glUniform2f(gl->glGetUniformLocation(v->program, "axisX"), right[0] - origin[0],
                    right[1] - origin[1]);
    gl->glUniform2f(gl->glGetUniformLocation(v->program, "axisY"), bottom[0] - origin[0],
                    bottom[1] - origin[1]);
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    gl->glUniform2f(gl->glGetUniformLocation(v->program, "docSize"), (float)view->doc_size[0],
                    (float)view->doc_size[1]);
    const pc_ui_tokens *t = pc_ui_shell_app_tokens(app);
    color_uniform(gl, v->program, "surround", t->canvas);
    color_uniform(gl, v->program, "lightCheck", t->field);
    color_uniform(gl, v->program, "darkCheck", t->card_border);
    gl->glBindTexture(GL_TEXTURE_2D, v->source);
    gl->glBindVertexArray(v->vao);
    gl->glDrawArrays(GL_TRIANGLES, 0, 3);
    gl->glBindVertexArray(0);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)bound);
    return gl->glGetError() == GL_NO_ERROR ? PC_OK : pc_fail(err, PC_ERR_IO, "canvas drawing failed");
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
        double pixels = pc_pref_unit_to_px(unit, 1, dpi, axis ? doc->size.height : doc->size.width, 72.);
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
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    if (!view || r.w < 1 || r.h < 1) {
        v->laid_out = false;
        return PC_OK;
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
    if (view->fit_pending || view->fill_pending) {
        pc_ui_canvas_fit(view, r.w, r.h, ppp, 1.f, view->fill_pending);
    }
    view->zoom = pc_ui_zoom_clamp(view->zoom, view->doc_size);
    view->rotation = pc_ui_rotation_wrap(view->rotation);
    if (!isfinite(view->center[0])) {
        view->center[0] = (float)view->doc_size[0] / 2.f;
    }
    if (!isfinite(view->center[1])) {
        view->center[1] = (float)view->doc_size[1] / 2.f;
    }
    v->xf = (pc_ui_canvas_xform){.x = r.x,
                                 .y = r.y,
                                 .width = r.w,
                                 .height = r.h,
                                 .zoom = view->zoom / ppp,
                                 .center = {view->center[0], view->center[1]},
                                 .rotation = view->rotation,
                                 .aspect = 1.f};
    v->laid_out = true;
    rc = pc_ui_canvas_cache_update(&v->cache, pc_engine_session_active(pc_ui_shell_app_session(app)), err);
    if (rc == PC_OK) {
        rc = render_camera(app, v, gl, err);
    }
    if (rc == PC_OK) {
        struct nk_image image = nk_image_id((int)v->target);
        nk_draw_image(nk_window_get_canvas(pc_ui_shell_app_nk(app)), r, &image, nk_rgba(255, 255, 255, 255));
    }
    if (rulers && rc == PC_OK) {
        draw_rulers(app, &v->xf);
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
    if (!e || !cv->laid_out) {
        return false;
    }
    pc_ui_view *view = pc_ui_canvas_active_view(app);
    if (!view) {
        cv->dragging = false;
        return false;
    }
    pc_ui_canvas_xform current = cv->xf;
    float ppp = pc_ui_shell_app_ui_scale(app);
    current.zoom = view->zoom / ppp;
    current.center[0] = view->center[0];
    current.center[1] = view->center[1];
    current.rotation = view->rotation;
    const pc_engine_doc_state *active = pc_engine_session_active(pc_ui_shell_app_session(app));
    if (!active || pc_doc_arc_get(active->doc)->id != cv->cache.document) {
        cv->dragging = false;
        return false;
    }
    const pc_ui_canvas_xform *xf = &current;
    float density = SDL_GetWindowPixelDensity(pc_ui_shell_app_sdl_window(app));
    float factor = density / ppp, x, y;
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        cv->dragging = false;
        return false;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE &&
        pc_ui_shell_app_state(app)->tool == PC_TOOL_ROTATE_VIEW) {
        view->rotation = 0;
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
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        x = e->button.x * factor;
        y = e->button.y * factor;
        if (!e->button.down && cv->dragging && e->button.button == cv->drag_button) {
            if (cv->drag_tool == PC_TOOL_ZOOM && !cv->moved) {
                int direction = (SDL_GetModState() & SDL_KMOD_ALT) ? -1 : 1;
                (void)pc_ui_canvas_anchor(view, xf, cv->press_pointer[0], cv->press_pointer[1],
                                          pc_ui_zoom_step(view->zoom, direction, view->doc_size), ppp);
            }
            cv->dragging = false;
            return true;
        }
        if (x < xf->x || y < xf->y || x >= xf->x + xf->width || y >= xf->y + xf->height) {
            return false;
        }
        pc_ui_tool tool = pc_ui_shell_app_state(app)->tool;
        const bool *keys = SDL_GetKeyboardState(NULL);
        if (e->button.button == SDL_BUTTON_MIDDLE || (keys && keys[SDL_SCANCODE_SPACE])) {
            tool = PC_TOOL_HAND;
        }
        if (e->button.down &&
            (e->button.button == SDL_BUTTON_LEFT || e->button.button == SDL_BUTTON_MIDDLE) &&
            (tool == PC_TOOL_HAND || tool == PC_TOOL_ZOOM || tool == PC_TOOL_ROTATE_VIEW)) {
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
    return false;
}

pc_status pc_ui_canvas_view_command(pc_ui_shell_app *app, const char *id, yyjson_val *params,
                                    pc_json_value *out, pc_error *err)
{
    pc_ui_extras *extras = &pc_ui_shell_app_state(app)->extras;
    bool *toggle = strcmp(id, "view.rulers") == 0       ? &extras->rulers
                   : strcmp(id, "view.snap") == 0       ? &extras->snap
                   : strcmp(id, "view.lockGuides") == 0 ? &extras->lock_guides
                                                        : NULL;
    if (toggle) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *value = yyjson_mut_bool(doc, !*toggle);
        pc_status rc = pc_json_value_from_mut(value, out, err);
        yyjson_mut_doc_free(doc);
        if (rc == PC_OK) {
            *toggle = !*toggle;
        }
        return rc;
    }
    bool in = strcmp(id, "view.zoomIn") == 0, zoom_out = strcmp(id, "view.zoomOut") == 0;
    bool fit = strcmp(id, "view.fitOnScreen") == 0, actual = strcmp(id, "view.actualPixels") == 0;
    bool twice = strcmp(id, "view.twoHundredPercent") == 0, reset = strcmp(id, "view.resetView") == 0;
    bool rotate = strcmp(id, "view.rotateView") == 0;
    if (!in && !zoom_out && !fit && !actual && !twice && !reset && !rotate) {
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
    } else if (fit) {
        next.fit_pending = true;
    } else {
        next.zoom = pc_ui_zoom_clamp(actual  ? 1.f
                                     : twice ? 2.f
                                             : pc_ui_zoom_step(v->zoom, in ? 1 : -1, v->doc_size),
                                     v->doc_size);
        next.fit_pending = false;
        next.fill_pending = false;
    }
    if (!rotate && !reset) {
        next.fill_pending = false;
    }
    if (rotate || reset || twice) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        yyjson_mut_val *value = yyjson_mut_obj(doc);
        if (rotate || reset) {
            yyjson_mut_obj_add_real(doc, value, "rotation", next.rotation);
        }
        if (reset || twice) {
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
    *v = next;
    return PC_OK;
}
