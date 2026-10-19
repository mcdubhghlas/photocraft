#include "gpu/fx_gl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <blake3.h>
#include "base/mem.h"
#include "compose/pattern.h"

extern const unsigned char pc_gpu_fx_glsl[];
extern const size_t pc_gpu_fx_glsl_len;
typedef struct shared_maps {
    size_t refs, bytes;
    uint8_t key[32];
    uint64_t age;
    pc_gpu_fx_output output;
} shared_maps;
struct pc_gpu_fx_context {
    gl33 gl;
    GLuint vertex, programs[9], vao, framebuffer;
    GLuint shape, fields[8], temps[PC_GPU_FX_MAX_STAGES], lut, pattern, dummy;
    size_t ntemps;
    GLint maximum;
    bool faulted, lost;
    shared_maps *cache[16];
    size_t cache_bytes;
    uint64_t clock, hits, builds, fragments;
};
static const char *const vertex = "#version 330 core\nvoid main(){vec2 "
                                  "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);}";
static pc_status check(pc_gpu_fx_context *ctx, pc_error *err)
{
    GLenum code = ctx->gl.glGetError();
    if (code == GL_NO_ERROR) {
        return PC_OK;
    }
    ctx->faulted = true;
    ctx->lost = code == 0x0507;
    return pc_fail(err, PC_ERR_IO, "GPU effect OpenGL error 0x%x", code);
}
static pc_status compile(const gl33 *gl, GLenum kind, const char *source, GLint length, GLuint *out,
                         pc_error *err)
{
    GLuint shader = gl->glCreateShader(kind);
    if (!shader) {
        return pc_fail(err, PC_ERR_IO, "GPU effect shader allocation");
    }
    gl->glShaderSource(shader, 1, &source, &length);
    gl->glCompileShader(shader);
    GLint ok = 0;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[200] = {0};
        gl->glGetShaderInfoLog(shader, (GLsizei)sizeof log, NULL, log);
        gl->glDeleteShader(shader);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU effect shader: %.180s", log);
    }
    *out = shader;
    return PC_OK;
}
static pc_status program(pc_gpu_fx_context *ctx, pc_gpu_fx_kernel kernel, GLuint *out, pc_error *err)
{
    if ((unsigned)kernel >= 9) {
        return pc_fail(err, PC_ERR_INVALID, "unknown GPU effect kernel");
    }
    const gl33 *gl = &ctx->gl;
    if (!ctx->programs[kernel]) {
        size_t size = pc_gpu_fx_glsl_len + 80;
        char *source = malloc(size);
        if (!source) {
            return pc_fail(err, PC_ERR_NOMEM, "GPU effect shader source");
        }
        int prefix = snprintf(source, size, "#version 330 core\n#define PC_FX_KERNEL %u\n", (unsigned)kernel);
        size_t skip = 0;
        while (skip < pc_gpu_fx_glsl_len && pc_gpu_fx_glsl[skip] != '\n') {
            skip++;
        }
        if (skip < pc_gpu_fx_glsl_len) {
            skip++;
        }
        size_t length = pc_gpu_fx_glsl_len - skip;
        memcpy(source + prefix, pc_gpu_fx_glsl + skip, length);
        GLuint fragment = 0;
        pc_status rc =
            compile(gl, GL_FRAGMENT_SHADER, source, (GLint)((size_t)prefix + length), &fragment, err);
        free(source);
        if (rc != PC_OK) {
            return rc;
        }
        GLuint pipeline = gl->glCreateProgram();
        gl->glAttachShader(pipeline, ctx->vertex);
        gl->glAttachShader(pipeline, fragment);
        gl->glLinkProgram(pipeline);
        gl->glDeleteShader(fragment);
        GLint ok = 0;
        gl->glGetProgramiv(pipeline, GL_LINK_STATUS, &ok);
        if (!ok) {
            gl->glDeleteProgram(pipeline);
            return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU effect shader link");
        }
        ctx->programs[kernel] = pipeline;
    }
    *out = ctx->programs[kernel];
    return PC_OK;
}
static void scalar_texture(const gl33 *gl, GLuint texture, int w, int h, const float *values)
{
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, w, h, 0, GL_RED, GL_FLOAT, values);
}
static void release_maps(pc_gpu_fx_context *ctx, shared_maps *shared)
{
    if (--shared->refs) {
        return;
    }
    if (ctx && !ctx->lost) {
        ctx->gl.glDeleteTextures(2, shared->output.textures);
    }
    free(shared);
}
void pc_gpu_fx_output_fini(pc_gpu_fx_context *ctx, pc_gpu_fx_output *out)
{
    if (!out) {
        return;
    }
    if (out->shared) {
        release_maps(ctx, out->shared);
    } else if (ctx && !ctx->lost) {
        ctx->gl.glDeleteTextures(2, out->textures);
    }
    *out = (pc_gpu_fx_output){0};
}
uint64_t pc_gpu_fx_cache_hits(const pc_gpu_fx_context *ctx)
{
    return ctx ? ctx->hits : 0;
}
uint64_t pc_gpu_fx_map_builds(const pc_gpu_fx_context *ctx)
{
    return ctx ? ctx->builds : 0;
}
static void evict(pc_gpu_fx_context *ctx, size_t slot)
{
    shared_maps *entry = ctx->cache[slot];
    if (!entry) {
        return;
    }
    ctx->cache_bytes -= entry->bytes;
    ctx->cache[slot] = NULL;
    release_maps(ctx, entry);
}
static size_t oldest(const pc_gpu_fx_context *ctx)
{
    size_t slot = 0;
    for (size_t i = 0; i < 16; i++) {
        if (!ctx->cache[i]) {
            return i;
        }
        if (ctx->cache[i]->age < ctx->cache[slot]->age) {
            slot = i;
        }
    }
    return slot;
}
void pc_gpu_fx_context_abandon(pc_gpu_fx_context *ctx)
{
    if (ctx) {
        ctx->lost = true;
        pc_gpu_fx_context_free(ctx);
    }
}
bool pc_gpu_fx_context_faulted(const pc_gpu_fx_context *ctx, bool *lost)
{
    if (!ctx || !ctx->faulted) {
        return false;
    }
    if (lost) {
        *lost = ctx->lost;
    }
    return true;
}
void pc_gpu_fx_context_free(pc_gpu_fx_context *ctx)
{
    if (!ctx) {
        return;
    }
    const gl33 *gl = &ctx->gl;
    for (size_t i = 0; i < 16; i++) {
        evict(ctx, i);
    }
    if (!ctx->lost) {
        gl->glDeleteTextures(1, &ctx->shape);
        gl->glDeleteTextures(8, ctx->fields);
        gl->glDeleteTextures((GLsizei)ctx->ntemps, ctx->temps);
        gl->glDeleteTextures(1, &ctx->lut);
        gl->glDeleteTextures(1, &ctx->pattern);
        gl->glDeleteTextures(1, &ctx->dummy);
        for (size_t i = 0; i < 9; i++) {
            if (ctx->programs[i]) {
                gl->glDeleteProgram(ctx->programs[i]);
            }
        }
        if (ctx->vertex) {
            gl->glDeleteShader(ctx->vertex);
        }
        if (ctx->framebuffer) {
            gl->glDeleteFramebuffers(1, &ctx->framebuffer);
        }
        if (ctx->vao) {
            gl->glDeleteVertexArrays(1, &ctx->vao);
        }
    }
    free(ctx);
}
pc_status pc_gpu_fx_context_new(const gl33 *gl, pc_gpu_fx_context **out, pc_error *err)
{
    if (!gl || !out) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU effect context");
    }
#define REQUIRE(ret, name, params)                                                                           \
    if (!gl->name) {                                                                                         \
        return pc_fail(err, PC_ERR_INVALID, "missing GL entry %s", #name);                                   \
    }
    GL33_FUNCS(REQUIRE)
#undef REQUIRE
    pc_gpu_fx_context *ctx = calloc(1, sizeof *ctx);
    if (!ctx) {
        return pc_fail(err, PC_ERR_NOMEM, "GPU effect context");
    }
    ctx->gl = *gl;
    pc_status rc = compile(gl, GL_VERTEX_SHADER, vertex, (GLint)strlen(vertex), &ctx->vertex, err);
    if (rc == PC_OK) {
        gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &ctx->maximum);
        gl->glGenVertexArrays(1, &ctx->vao);
        gl->glGenFramebuffers(1, &ctx->framebuffer);
        gl->glGenTextures(1, &ctx->shape);
        gl->glGenTextures(8, ctx->fields);
        gl->glGenTextures(1, &ctx->lut);
        gl->glGenTextures(1, &ctx->pattern);
        gl->glGenTextures(1, &ctx->dummy);
        gl->glActiveTexture(GL_TEXTURE0);
        const float zero[1] = {0};
        scalar_texture(gl, ctx->dummy, 1, 1, zero);
        rc = check(ctx, err);
    }
    if (rc != PC_OK) {
        pc_gpu_fx_context_free(ctx);
        return rc;
    }
    *out = ctx;
    return PC_OK;
}
static GLuint input(const pc_gpu_fx_context *ctx, const pc_gpu_fx_program *program, pc_gpu_fx_input value,
                    const int *temps, const pc_gpu_fx_output *out)
{
    switch (value.kind) {
    case PC_GPU_FX_SHAPE:
        return ctx->shape;
    case PC_GPU_FX_FIELD:
        return ctx->fields[value.index];
    case PC_GPU_FX_VALUE: {
        const pc_gpu_fx_stage *s = &program->stages[value.index];
        return s->output >= 0 ? out->textures[s->output] : ctx->temps[temps[value.index]];
    }
    default:
        return ctx->dummy;
    }
}
static void bind(const gl33 *gl, GLuint pipeline, const char *name, unsigned unit, GLuint texture)
{
    gl->glActiveTexture(GL_TEXTURE0 + unit);
    gl->glBindTexture(GL_TEXTURE_2D, texture);
    gl->glUniform1i(gl->glGetUniformLocation(pipeline, name), (int)unit);
}
static pc_status pattern_upload(pc_gpu_fx_context *ctx, const pc_pattern *pattern, pc_color_mode mode,
                                pc_error *err)
{
    float (*pixels)[4] = NULL;
    size_t n = 0;
    pc_status rc = pc_compose_pattern_rgba(pattern, mode, &pixels, &n, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (!pixels) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU effect pattern unavailable");
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t ch = 0; ch < 3; ch++) {
            pixels[i][ch] *= pixels[i][3];
        }
    }
    const gl33 *gl = &ctx->gl;
    gl->glBindTexture(GL_TEXTURE_2D, ctx->pattern);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (int)pattern->width, (int)pattern->height, 0, GL_RGBA,
                     GL_FLOAT, pixels);
    free(pixels);
    return check(ctx, err);
}
uint64_t pc_gpu_fx_fragments(const pc_gpu_fx_context *ctx)
{
    return ctx ? ctx->fragments : 0;
}

pc_status pc_gpu_fx_render(pc_gpu_fx_context *ctx, const pc_gpu_fx_program *p, pc_rect region,
                           const float *shape, size_t length, pc_color_mode mode, pc_gpu_fx_output *out,
                           pc_error *err)
{
    return pc_gpu_fx_render_window(ctx, p, region, region, shape, length, mode, out, err);
}
pc_status pc_gpu_fx_render_window(pc_gpu_fx_context *ctx, const pc_gpu_fx_program *p, pc_rect region,
                                  pc_rect area, const float *shape, size_t length, pc_color_mode mode,
                                  pc_gpu_fx_output *out, pc_error *err)
{
    if (!ctx || !out || ctx->faulted) {
        return pc_fail(err, PC_ERR_IO, "GPU effect context unavailable");
    }
    int temps[PC_GPU_FX_MAX_STAGES];
    size_t ntemps;
    pc_status rc = pc_gpu_fx_temporaries(p, temps, &ntemps, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect valid = pc_rect_intersect(area, region);
    pc_rect windows[PC_GPU_FX_MAX_STAGES];
    rc = pc_gpu_fx_windows(p, valid, region, windows, err);
    if (rc != PC_OK)
        return rc;
    uint64_t w = pc_rect_width(region), h = pc_rect_height(region);
    size_t fields = 0;
    for (size_t i = 0; i < 8; i++) {
        fields += p->fields[i] ? 1u : 0u;
    }
    if (!w || !h || w > (uint64_t)ctx->maximum || h > (uint64_t)ctx->maximum ||
        w > UINT64_C(268435456) / 4 / h / (1 + fields + ntemps + p->maps)) {
        return pc_fail(err, PC_ERR_LIMIT, "GPU effect map budget");
    }
    if (!shape || length != w * h) {
        return pc_fail(err, PC_ERR_INVALID, "GPU effect shape size mismatch");
    }
    for (size_t i = 0; i < p->count; i++) {
        const pc_pattern *pattern = p->stages[i].pattern;
        if (pattern && (pattern->width > (uint32_t)ctx->maximum || pattern->height > (uint32_t)ctx->maximum ||
                        pc_pattern_pixel_count(pattern) * 16 > UINT64_C(67108864))) {
            return pc_fail(err, PC_ERR_LIMIT, "GPU effect pattern budget");
        }
    }
    for (size_t i = 0; i < length; i++) {
        if (!isfinite(shape[i])) {
            return pc_fail(err, PC_ERR_INVALID, "non-finite GPU effect shape");
        }
    }
    blake3_hasher hash;
    uint8_t geometry[32], key[32];
    pc_gpu_fx_program_key(p, geometry);
    blake3_hasher_init(&hash);
    blake3_hasher_update(&hash, geometry, sizeof geometry);
    blake3_hasher_update(&hash, &region, sizeof region);
    blake3_hasher_update(&hash, &valid, sizeof valid);
    blake3_hasher_update(&hash, &mode, sizeof mode);
    blake3_hasher_update(&hash, shape, length * sizeof *shape);
    blake3_hasher_finalize(&hash, key, sizeof key);
    for (size_t i = 0; i < 16; i++) {
        shared_maps *entry = ctx->cache[i];
        if (entry && memcmp(entry->key, key, 32) == 0) {
            entry->refs++;
            entry->age = ++ctx->clock;
            ctx->hits++;
            pc_gpu_fx_output_fini(ctx, out);
            *out = entry->output;
            out->shared = entry;
            return PC_OK;
        }
    }
    size_t bytes = (size_t)(w * h * 4 * p->maps);
    while (ctx->cache_bytes && ctx->cache_bytes + bytes > ((size_t)128 << 20)) {
        size_t slot = 16;
        for (size_t i = 0; i < 16; i++) {
            if (ctx->cache[i] && (slot == 16 || ctx->cache[i]->age < ctx->cache[slot]->age)) {
                slot = i;
            }
        }
        if (slot == 16) {
            break;
        }
        evict(ctx, slot);
    }
    const gl33 *gl = &ctx->gl;
    GLint previous = 0;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    scalar_texture(gl, ctx->shape, (int)w, (int)h, shape);
    for (size_t i = 0; rc == PC_OK && i < 8; i++) {
        if (p->fields[i]) {
            float *distance = NULL;
            rc = pc_compose_fx_distance_field((pc_compose_fx_field_kind)i, shape, (size_t)w, (size_t)h,
                                              (float)p->reach[i], &distance, err);
            if (rc == PC_OK) {
                scalar_texture(gl, ctx->fields[i], (int)w, (int)h, distance);
            }
            free(distance);
        }
    }
    if (rc != PC_OK) {
        gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
        return rc;
    }
    for (size_t i = ctx->ntemps; i < ntemps; i++) {
        gl->glGenTextures(1, &ctx->temps[i]);
    }
    if (ntemps > ctx->ntemps) {
        ctx->ntemps = ntemps;
    }
    for (size_t i = 0; i < ntemps; i++) {
        scalar_texture(gl, ctx->temps[i], (int)w, (int)h, NULL);
    }
    pc_gpu_fx_output next = {.region = region, .valid = valid, .count = p->maps};
    gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx->framebuffer);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glBindVertexArray(ctx->vao);
    gl->glViewport(0, 0, (int)w, (int)h);
    for (size_t i = 0; i < next.count; i++) {
        gl->glGenTextures(1, &next.textures[i]);
        scalar_texture(gl, next.textures[i], (int)w, (int)h, NULL);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, next.textures[i], 0);
        gl->glClearColor(0, 0, 0, 0);
        gl->glClear(GL_COLOR_BUFFER_BIT);
    }
    if (rc == PC_OK) {
        rc = check(ctx, err);
    }
    for (size_t i = 0; rc == PC_OK && i < p->count; i++) {
        pc_rect window = windows[i];
        if (pc_rect_is_empty(window))
            continue;
        gl->glEnable(GL_SCISSOR_TEST);
        gl->glScissor(window.x0 - region.x0, window.y0 - region.y0, (GLsizei)pc_rect_width(window),
                      (GLsizei)pc_rect_height(window));
        ctx->fragments += (uint64_t)pc_rect_width(window) * pc_rect_height(window);
        const pc_gpu_fx_stage *s = &p->stages[i];
        GLuint pipeline;
        rc = program(ctx, s->kernel, &pipeline, err);
        if (rc != PC_OK) {
            break;
        }
        GLuint target = s->output >= 0 ? next.textures[s->output] : ctx->temps[temps[i]];
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            rc = pc_fail(err, PC_ERR_UNSUPPORTED, "GPU effect R32F framebuffer unavailable");
            break;
        }
        gl->glUseProgram(pipeline);
        const char *names[4] = {"p0", "p1", "p3", "p4"};
        for (size_t j = 0; j < 4; j++) {
            const float *v = s->params[j];
            gl->glUniform4f(gl->glGetUniformLocation(pipeline, names[j]), v[0], v[1], v[2], v[3]);
        }
        gl->glUniform2i(gl->glGetUniformLocation(pipeline, "size"), (int)w, (int)h);
        gl->glUniform2i(gl->glGetUniformLocation(pipeline, "origin"), region.x0, region.y0);
        bind(gl, pipeline, "inputA", 0, input(ctx, p, s->a, temps, &next));
        bind(gl, pipeline, "inputB", 1, input(ctx, p, s->b, temps, &next));
        bind(gl, pipeline, "shapeTex", 2, input(ctx, p, s->shape, temps, &next));
        const float zero[1] = {0};
        gl->glActiveTexture(GL_TEXTURE0 + 3);
        scalar_texture(gl, ctx->lut, s->lut_len ? (int)s->lut_len : 1, 1, s->lut ? s->lut : zero);
        bind(gl, pipeline, "lutTex", 3, ctx->lut);
        gl->glUniform1i(gl->glGetUniformLocation(pipeline, "lutSize"), s->lut_len ? (int)s->lut_len : 1);
        if (s->pattern) {
            gl->glActiveTexture(GL_TEXTURE0 + 4);
            rc = pattern_upload(ctx, s->pattern, mode, err);
            if (rc != PC_OK) {
                break;
            }
        }
        bind(gl, pipeline, "patternTex", 4, s->pattern ? ctx->pattern : ctx->dummy);
        gl->glDrawArrays(GL_TRIANGLES, 0, 3);
        rc = check(ctx, err);
    }
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
    gl->glBindVertexArray(0);
    gl->glActiveTexture(GL_TEXTURE0);
    if (rc != PC_OK) {
        pc_gpu_fx_output_fini(ctx, &next);
        return rc;
    }
    ctx->builds++;
    shared_maps *entry = bytes <= ((size_t)128 << 20) ? calloc(1, sizeof *entry) : NULL;
    if (entry) {
        size_t slot = oldest(ctx);
        evict(ctx, slot);
        *entry = (shared_maps){.refs = 2, .bytes = bytes, .age = ++ctx->clock, .output = next};
        memcpy(entry->key, key, 32);
        next.shared = entry;
        ctx->cache[slot] = entry;
        ctx->cache_bytes += bytes;
    }
    pc_gpu_fx_output_fini(ctx, out);
    *out = next;
    return PC_OK;
}
pc_status pc_gpu_fx_readback(pc_gpu_fx_context *ctx, const pc_gpu_fx_output *out, size_t map, float **pixels,
                             size_t *length, pc_error *err)
{
    if (!ctx || ctx->faulted || !out || !pixels || !length || map >= out->count || map >= 2) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU effect readback");
    }
    size_t w = pc_rect_width(out->region), h = pc_rect_height(out->region);
    if (!w || !h || w > (size_t)ctx->maximum || h > (size_t)ctx->maximum || w > ((size_t)64 << 20) / h ||
        !out->textures[map]) {
        return pc_fail(err, PC_ERR_LIMIT, "GPU effect readback dimensions");
    }
    float *values = pc_malloc_array(w * h, sizeof *values);
    if (!values) {
        return pc_fail(err, PC_ERR_NOMEM, "GPU effect readback");
    }
    const gl33 *gl = &ctx->gl;
    GLint previous;
    gl->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, ctx->framebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out->textures[map], 0);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glReadPixels(0, 0, (int)w, (int)h, GL_RED, GL_FLOAT, values);
    gl->glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
    pc_status rc = check(ctx, err);
    if (rc != PC_OK) {
        free(values);
        return rc;
    }
    *pixels = values;
    *length = w * h;
    return PC_OK;
}
