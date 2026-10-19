#include "gpu/gpu.h"
#include "gpu/fx_resources.h"
#include "gpu/prefix.h"
#include "gpu/resident.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "base/mem.h"
#include "compose/masks.h"
#include "compose/effects_util.h"
#include "compose/gradient_fill.h"
#include "compose/pattern.h"
#include "color/convert.h"

struct pc_gpu_compositor {
    gl33 gl;
    GLuint program, programs[15], vertex, vao, framebuffer, mask, adjustment, textures[256];
    pc_gpu_fx_context *effects;
    pc_gpu_fx_resources fx_resources;
    GLuint paint;
    pc_gpu_prefix prefix;
    pc_gpu_resident resident;
    GLint maximum;
    size_t ntextures;
    int width, height;
    pthread_mutex_t lock;
    atomic_bool faulted;
    atomic_uint_fast64_t submissions;
    bool lost;
    char detail[256];
};
static const char *const vertex =
    "#version 330 core\n"
    "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);}";
extern const unsigned char pc_gpu_compose_glsl[];
extern const size_t pc_gpu_compose_glsl_len;

void pc_gpu_compositor_fault(pc_gpu_compositor *gpu, bool lost, const char *detail)
{
    if (!gpu) {
        return;
    }
    pthread_mutex_lock(&gpu->lock);
    if (!atomic_load(&gpu->faulted) || (lost && !gpu->lost)) {
        gpu->lost = lost;
        const char *start = detail ? detail : "";
        while (*start == ' ' || *start == '\n' || *start == '\r' || *start == '\t') {
            start++;
        }
        size_t length = strcspn(start, "\r\n");
        if (length > 200) {
            length = 200;
        }
        memcpy(gpu->detail, start, length);
        gpu->detail[length] = '\0';
    }
    atomic_store(&gpu->faulted, true);
    pthread_mutex_unlock(&gpu->lock);
}
bool pc_gpu_compositor_healthy(const pc_gpu_compositor *gpu)
{
    return gpu && !atomic_load(&gpu->faulted);
}
bool pc_gpu_compositor_fault_detail(pc_gpu_compositor *gpu, bool *lost, char detail[256])
{
    if (!gpu || !atomic_load(&gpu->faulted)) {
        return false;
    }
    pthread_mutex_lock(&gpu->lock);
    if (lost) {
        *lost = gpu->lost;
    }
    if (detail) {
        memcpy(detail, gpu->detail, sizeof gpu->detail);
    }
    pthread_mutex_unlock(&gpu->lock);
    return true;
}
uint64_t pc_gpu_compositor_prefix_hits(const pc_gpu_compositor *gpu)
{
    return gpu ? gpu->prefix.hits : 0;
}
uint64_t pc_gpu_compositor_prefix_bytes(const pc_gpu_compositor *gpu)
{
    return gpu ? (uint64_t)gpu->prefix.count * 256 * 256 * 16 : 0;
}
uint64_t pc_gpu_compositor_prefix_budget(const pc_gpu_compositor *gpu)
{
    return gpu ? pc_gpu_prefix_budget(&gpu->prefix) : 0;
}
void pc_gpu_compositor_set_prefix_budget(pc_gpu_compositor *gpu, uint64_t bytes)
{
    if (!gpu)
        return;
    bool lost = false;
    (void)pc_gpu_compositor_fault_detail(gpu, &lost, NULL);
    pc_gpu_prefix_set_budget(&gpu->gl, &gpu->prefix, bytes, lost);
}
pc_doc_id pc_gpu_compositor_document(const pc_gpu_compositor *gpu)
{
    return gpu && gpu->prefix.has_snapshot ? gpu->prefix.snapshot.id : 0;
}
void pc_gpu_compositor_forget_document(pc_gpu_compositor *gpu, pc_doc_id document)
{
    if (!gpu || !gpu->prefix.has_snapshot || gpu->prefix.snapshot.id != document)
        return;
    bool lost = false;
    (void)pc_gpu_compositor_fault_detail(gpu, &lost, NULL);
    pc_gpu_prefix_invalidate(&gpu->gl, &gpu->prefix, lost);
}

uint64_t pc_gpu_compositor_effect_fragments(const pc_gpu_compositor *gpu)
{
    return gpu ? pc_gpu_fx_fragments(gpu->effects) : 0;
}
uint64_t pc_gpu_compositor_effect_builds(const pc_gpu_compositor *gpu)
{
    return gpu ? pc_gpu_fx_map_builds(gpu->effects) : 0;
}
uint64_t pc_gpu_compositor_effect_hits(const pc_gpu_compositor *gpu)
{
    return gpu ? pc_gpu_fx_cache_hits(gpu->effects) : 0;
}

uint64_t pc_gpu_compositor_source_hits(const pc_gpu_compositor *gpu)
{
    return gpu ? gpu->resident.hits : 0;
}
uint64_t pc_gpu_compositor_source_uploads(const pc_gpu_compositor *gpu)
{
    return gpu ? gpu->resident.uploads : 0;
}
uint64_t pc_gpu_compositor_source_bytes(const pc_gpu_compositor *gpu)
{
    return gpu ? gpu->resident.bytes : 0;
}
uint64_t pc_gpu_compositor_submissions(const pc_gpu_compositor *gpu)
{
    return gpu ? atomic_load(&gpu->submissions) : 0;
}
static pc_status gpu_error(pc_gpu_compositor *gpu, pc_error *err)
{
    GLenum code = gpu->gl.glGetError();
    if (code == GL_NO_ERROR) {
        return PC_OK;
    }
    char detail[64];
    (void)snprintf(detail, sizeof detail, "OpenGL error 0x%x", code);
    pc_gpu_compositor_fault(gpu, code == 0x0507, detail);
    return pc_fail(err, PC_ERR_IO, "%s", detail);
}
static pc_status shader(const gl33 *gl, GLenum kind, const char *text, GLint length, GLuint *out,
                        pc_error *err)
{
    GLuint id = gl->glCreateShader(kind);
    if (!id) {
        return pc_fail(err, PC_ERR_IO, "GPU shader allocation failed");
    }
    gl->glShaderSource(id, 1, &text, &length);
    gl->glCompileShader(id);
    GLint ok = 0;
    gl->glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[200] = {0};
        gl->glGetShaderInfoLog(id, (GLsizei)sizeof log, NULL, log);
        gl->glDeleteShader(id);
        return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU shader: %.190s", log);
    }
    *out = id;
    return PC_OK;
}
/* Each entrypoint specializes the shared source. A uniform-selected mega-shader made the
 * software driver compile all adjustments even for a simple mask or copy. GL programs are
 * cached per entrypoint and created only when the plan needs them. */
static pc_status program_for(pc_gpu_compositor *gpu, unsigned kernel, pc_error *err)
{
    if (kernel >= 15) {
        return pc_fail(err, PC_ERR_INVALID, "unknown GPU kernel");
    }
    const gl33 *gl = &gpu->gl;
    if (!gpu->programs[kernel]) {
        size_t size = pc_gpu_compose_glsl_len + 80;
        char *source = malloc(size);
        if (!source) {
            return pc_fail(err, PC_ERR_NOMEM, "GPU shader specialization");
        }
        int prefix = snprintf(source, size, "#version 330 core\n#define PC_KERNEL %u\n", kernel);
        const unsigned char *body = pc_gpu_compose_glsl;
        size_t skip = 0;
        while (skip < pc_gpu_compose_glsl_len && body[skip] != '\n') {
            skip++;
        }
        if (skip < pc_gpu_compose_glsl_len) {
            skip++;
        }
        size_t length = pc_gpu_compose_glsl_len - skip;
        memcpy(source + prefix, body + skip, length);
        GLuint fs = 0;
        pc_status rc = shader(gl, GL_FRAGMENT_SHADER, source, (GLint)((size_t)prefix + length), &fs, err);
        free(source);
        if (rc != PC_OK) {
            return rc;
        }
        GLuint program = gl->glCreateProgram();
        gl->glAttachShader(program, gpu->vertex);
        gl->glAttachShader(program, fs);
        gl->glLinkProgram(program);
        gl->glDeleteShader(fs);
        GLint ok = 0;
        gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            gl->glDeleteProgram(program);
            return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU kernel %u link failed", kernel);
        }
        gpu->programs[kernel] = program;
    }
    gpu->program = gpu->programs[kernel];
    gl->glUseProgram(gpu->program);
    return PC_OK;
}
void pc_gpu_compositor_free(pc_gpu_compositor *gpu)
{
    if (!gpu) {
        return;
    }
    const gl33 *gl = &gpu->gl;
    /* A lost device's names cannot safely be reused. Context destruction reclaims them. */
    bool lost = false;
    (void)pc_gpu_compositor_fault_detail(gpu, &lost, NULL);
    pc_gpu_prefix_fini(gl, &gpu->prefix, lost);
    pc_gpu_resident_fini(gl, &gpu->resident, lost);
    pc_gpu_fx_resources_fini(lost ? NULL : gpu->effects, &gpu->fx_resources);
    if (lost) {
        pc_gpu_fx_context_abandon(gpu->effects);
    }
    if (!lost) {
        pc_gpu_fx_context_free(gpu->effects);
        if (gpu->paint) {
            gl->glDeleteTextures(1, &gpu->paint);
        }
        if (gpu->ntextures) {
            gl->glDeleteTextures((GLsizei)gpu->ntextures, gpu->textures);
        }
        if (gpu->adjustment) {
            gl->glDeleteTextures(1, &gpu->adjustment);
        }
        if (gpu->mask) {
            gl->glDeleteTextures(1, &gpu->mask);
        }
        for (size_t i = 0; i < 15; i++) {
            if (gpu->programs[i]) {
                gl->glDeleteProgram(gpu->programs[i]);
            }
        }
        if (gpu->vertex) {
            gl->glDeleteShader(gpu->vertex);
        }
        if (gpu->vao) {
            gl->glDeleteVertexArrays(1, &gpu->vao);
        }
        if (gpu->framebuffer) {
            gl->glDeleteFramebuffers(1, &gpu->framebuffer);
        }
    }
    pthread_mutex_destroy(&gpu->lock);
    free(gpu);
}
pc_status pc_gpu_compositor_new(const gl33 *gl, pc_gpu_compositor **out, pc_error *err)
{
    if (!gl || !out) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU context");
    }
#define CHECK_ENTRY(ret, name, params)                                                                       \
    if (!gl->name) {                                                                                         \
        return pc_fail(err, PC_ERR_INVALID, "missing GL entry %s", #name);                                   \
    }
    GL33_FUNCS(CHECK_ENTRY)
#undef CHECK_ENTRY
    pc_gpu_compositor *gpu = calloc(1, sizeof *gpu);
    if (!gpu) {
        return pc_fail(err, PC_ERR_NOMEM, "GPU context");
    }
    gpu->gl = *gl;
    atomic_init(&gpu->faulted, false);
    atomic_init(&gpu->submissions, 0);
    if (pthread_mutex_init(&gpu->lock, NULL) != 0) {
        free(gpu);
        return pc_fail(err, PC_ERR_IO, "GPU health mutex");
    }
    pc_status rc = shader(gl, GL_VERTEX_SHADER, vertex, (GLint)strlen(vertex), &gpu->vertex, err);
    if (rc == PC_OK) {
        rc = program_for(gpu, 0, err);
    }
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &gpu->maximum);
    if (rc == PC_OK) {
        gl->glGenVertexArrays(1, &gpu->vao);
        gl->glGenFramebuffers(1, &gpu->framebuffer);
        gl->glGenTextures(1, &gpu->mask);
        gl->glGenTextures(1, &gpu->paint);
        gl->glGenTextures(1, &gpu->adjustment);
        rc = gpu_error(gpu, err);
    }
    if (rc != PC_OK) {
        pc_gpu_compositor_free(gpu);
        return rc;
    }
    *out = gpu;
    return PC_OK;
}
static void texture(const gl33 *gl, GLuint id, int w, int h, const void *pixels)
{
    gl->glBindTexture(GL_TEXTURE_2D, id);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, pixels);
}
static pc_status content(const pc_layer *layer, const pc_document *doc, const pc_compose_patterns *patterns,
                         pc_rect rect, pc_compose_buffer *out, pc_error *err)
{
    if (layer->content.kind != PC_LAYER_FILL) {
        const pc_surface *surface = pc_layer_surface(layer);
        return surface ? pc_compose_surface_to_buffer(surface, rect, out, err)
                       : pc_compose_buffer_transparent(rect, out, err);
    }
    const pc_fill *fill = &layer->content.u.fill;
    if (layer->has_fill_cache && pc_fill_eq(&layer->fill_cache.fill, fill)) {
        return pc_compose_surface_to_buffer(&layer->fill_cache.surface, rect, out, err);
    }
    pc_rect frame = pc_compose_fill_frame(layer, pc_document_bounds(doc));
    if (fill->kind == PC_FILL_GRADIENT) {
        float quantum = 0;
        bool quantized = pc_compose_adjustment_quantum(doc->depth, &quantum);
        return pc_compose_gradient_fill_render_quantized(fill, rect, frame, quantized, quantum, out, err);
    }
    if (fill->kind == PC_FILL_SOLID) {
        float rgba[4];
        pc_color_to_rgb(&fill->u.solid, rgba);
        rgba[3] = fill->u.solid.alpha;
        return pc_compose_buffer_filled(rect, rgba, out, err);
    }
    return pc_compose_pattern_fill_render(patterns, fill, frame, rect, out, err);
}
static pc_status upload_mask(pc_gpu_compositor *gpu, const pc_document *doc, const pc_layer *layer,
                             pc_rect rect, pc_error *err)
{
    pc_surface combined = {0};
    bool found = false;
    pc_status rc = pc_compose_combined_mask(layer, pc_document_bounds(doc), &found, &combined, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_surface *mask = found                                    ? &combined
                             : layer->has_mask && layer->mask.enabled ? &layer->mask.surface
                                                                      : NULL;
    float density = found || !mask ? 1.f : layer->mask.density;
    pc_compose_buffer buffer = {0};
    rc = pc_compose_buffer_transparent(rect, &buffer, err);
    if (rc == PC_OK) {
        for (size_t i = 0; i < buffer.len; i++) {
            int32_t x = rect.x0 + (int32_t)(i % pc_rect_width(rect));
            int32_t y = rect.y0 + (int32_t)(i / pc_rect_width(rect));
            float value = mask ? pc_surface_sample_channel(mask, x, y, 0) : 1;
            buffer.px[i][0] = 1 - density * (1 - value);
        }
        gpu->gl.glActiveTexture(GL_TEXTURE0 + 2);
        texture(&gpu->gl, gpu->mask, (int)pc_rect_width(rect), (int)pc_rect_height(rect), buffer.px);
    }
    pc_compose_buffer_fini(&buffer);
    if (found) {
        pc_surface_fini(&combined);
    }
    return rc;
}
static pc_status chunk(pc_gpu_compositor *gpu, const pc_gpu_plan *plan, pc_rect rect, float (*pixels)[4],
                       const pc_document *doc, const pc_compose_patterns *patterns, pc_error *err)
{
    const gl33 *gl = &gpu->gl;
    int w = (int)pc_rect_width(rect), h = (int)pc_rect_height(rect);
    gl->glActiveTexture(GL_TEXTURE0);
    for (size_t i = gpu->ntextures; i < plan->slots; i++) {
        gl->glGenTextures(1, &gpu->textures[i]);
        texture(gl, gpu->textures[i], w, h, NULL);
    }
    size_t old_count = gpu->ntextures;
    if (gpu->ntextures < plan->slots) {
        gpu->ntextures = plan->slots;
    }
    if (gpu->width != w || gpu->height != h) {
        for (size_t i = 0; i < old_count; i++) {
            texture(gl, gpu->textures[i], w, h, NULL);
        }
        gpu->width = w;
        gpu->height = h;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, gpu->framebuffer);
    gl->glViewport(0, 0, w, h);
    gl->glDisable(GL_BLEND);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_CULL_FACE);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    gl->glBindVertexArray(gpu->vao);
    size_t start_pass = 0;
    if (gpu->prefix.active && pc_gpu_prefix_copy(gl, &gpu->prefix, gpu->framebuffer, rect,
                                                 gpu->textures[gpu->prefix.checkpoint.slot])) {
        start_pass = gpu->prefix.checkpoint.pass_end;
    }
    for (size_t i = start_pass; i < plan->count; i++) {
        const pc_gpu_pass *pass = &plan->passes[i];
        if (!pc_gpu_compositor_healthy(gpu)) {
            return pc_fail(err, PC_ERR_IO, "GPU device faulted");
        }
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   gpu->textures[pass->dst], 0);
        if (gl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU float framebuffer unavailable");
        }
        if (pass->kernel == PC_GPU_CLEAR) {
            gl->glClearColor(0, 0, 0, 0);
            gl->glClear(GL_COLOR_BUFFER_BIT);
        } else if (pass->kernel == PC_GPU_CONTENT) {
            const pc_surface *surface = pass->has_surface ? &pass->surface : NULL;
            if (!surface && pass->layer && pass->layer->content.kind != PC_LAYER_FILL)
                surface = pc_layer_surface(pass->layer);
            if (!surface && pass->layer && pass->layer->content.kind == PC_LAYER_FILL &&
                pass->layer->has_fill_cache &&
                pc_fill_eq(&pass->layer->fill_cache.fill, &pass->layer->content.u.fill))
                surface = &pass->layer->fill_cache.surface;
            if (surface && pc_gpu_resident_copy(gl, &gpu->resident, gpu->framebuffer,
                                                gpu->textures[pass->dst], surface, rect))
                goto pass_done;
            pc_compose_buffer source = {0};
            pc_status rc =
                pass->has_surface ? pc_compose_surface_to_buffer(&pass->surface, rect, &source, err)
                : pass->layer     ? content(pass->layer, doc, patterns, rect, &source, err)
                                  : pc_compose_buffer_filled(rect, pass->adjustment.params[0], &source, err);
            if (rc != PC_OK) {
                return rc;
            }
            gl->glActiveTexture(GL_TEXTURE0);
            texture(gl, gpu->textures[pass->dst], w, h, source.px);
            if (surface)
                pc_gpu_resident_store(gl, &gpu->resident, surface, rect, source.px);
            pc_compose_buffer_fini(&source);
        } else {
            unsigned kernel = pass->kernel == PC_GPU_MASK              ? 1u
                              : pass->kernel == PC_GPU_LERP            ? 2u
                              : pass->kernel == PC_GPU_ATOP            ? 3u
                              : pass->kernel == PC_GPU_ADJUST          ? 4u
                              : pass->kernel == PC_GPU_ADJMIX          ? 5u
                              : pass->kernel == PC_GPU_CHANNELS        ? 6u
                              : pass->kernel == PC_GPU_BLEND_IF        ? 7u
                              : pass->kernel == PC_GPU_COPY_RECT       ? 8u
                              : pass->kernel == PC_GPU_ADD_DIFF        ? 9u
                              : pass->kernel == PC_GPU_FX_INIT         ? 10u
                              : pass->kernel == PC_GPU_FX_PAINT        ? 11u
                              : pass->kernel == PC_GPU_FX_MERGE        ? 12u
                              : pass->kernel == PC_GPU_FX_STROKE_PAINT ? 13u
                              : pass->kernel == PC_GPU_FX_STROKE_END   ? 14u
                                                                       : 0u;
            pc_status prepared = program_for(gpu, kernel, err);
            if (prepared != PC_OK) {
                return prepared;
            }
            gl->glUseProgram(gpu->program);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "backdrop"), 0);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "source"), 1);
            gl->glUniform2i(gl->glGetUniformLocation(gpu->program, "offset"), rect.x0, rect.y0);
            float quantum = 0;
            (void)pc_compose_adjustment_quantum(doc->depth, &quantum);
            gl->glUniform4f(gl->glGetUniformLocation(gpu->program, "depthParams"), quantum, 0, 0, 0);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "hdr"),
                            doc->depth == PC_SAMPLE_TYPE_F32 ? 1 : 0);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "labMix"),
                            doc->mode == PC_COLOR_MODE_LAB ? 1 : 0);
            if (pass->kernel == PC_GPU_ADJUST || pass->kernel == PC_GPU_CHANNELS ||
                pass->kernel == PC_GPU_BLEND_IF) {
                const pc_gpu_adjust_program *adjust = &pass->adjustment;
                gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "adjustKind"), adjust->kind);
                const char *names[] = {"p0", "p1", "p2", "p3"};
                for (size_t j = 0; j < 4; j++) {
                    const float *v = adjust->params[j];
                    gl->glUniform4f(gl->glGetUniformLocation(gpu->program, names[j]), v[0], v[1], v[2], v[3]);
                }
                gl->glActiveTexture(GL_TEXTURE0 + 3);
                const float zero[4] = {0};
                texture(gl, gpu->adjustment, adjust->width ? (int)adjust->width : 1,
                        adjust->height ? (int)adjust->height : 1,
                        adjust->lut ? (const void *)adjust->lut : (const void *)zero);
                gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "adjustLut"), 3);
            }
            if (kernel >= 10) {
                const pc_gpu_fx_output *maps =
                    pass->fx_layer
                        ? pc_gpu_fx_resources_find(&gpu->fx_resources, pass->fx_layer, pass->effect)
                        : NULL;
                gl->glActiveTexture(GL_TEXTURE0 + 6);
                size_t map = pass->map_index >= 0 ? (size_t)pass->map_index : 0;
                gl->glBindTexture(GL_TEXTURE_2D, maps && map < maps->count ? maps->textures[map] : gpu->mask);
                gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "effectMap"), 6);
                gl->glUniform2i(gl->glGetUniformLocation(gpu->program, "mapOrigin"),
                                maps ? maps->region.x0 : 0, maps ? maps->region.y0 : 0);
                gl->glUniform2i(gl->glGetUniformLocation(gpu->program, "mapSize"),
                                maps ? (int)pc_rect_width(maps->region) : 0,
                                maps ? (int)pc_rect_height(maps->region) : 0);
                gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "fxKind"),
                                pass->kernel == PC_GPU_FX_PAINT ? (pass->map_index < 0 ? 1 : 0)
                                                                : pass->fx_kind);
                gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "fxFlags"), (int)pass->flags);
                gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "paintKind"), (int)pass->paint.kind);
                gl->glUniform4f(gl->glGetUniformLocation(gpu->program, "fxParams"), pass->outside,
                                pass->knockout, pass->paint.gain,
                                pass->paint.kind == PC_GPU_PAINT_PATTERN && !pass->paint.pattern ? 1.f : 0.f);
                gl->glActiveTexture(GL_TEXTURE0 + 7);
                gl->glBindTexture(GL_TEXTURE_2D, gpu->textures[pass->has_d ? pass->d : pass->a]);
                gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "accumulateTexture"), 7);
                if (pass->kernel == PC_GPU_FX_PAINT || pass->kernel == PC_GPU_FX_STROKE_PAINT) {
                    pc_compose_buffer colors = {0};
                    pc_status rc = PC_OK;
                    if (pass->paint.kind == PC_GPU_PAINT_GLOW) {
                        rc = pc_compose_buffer_transparent(pc_rect_make(0, 0, 4096, 1), &colors, err);
                        if (rc == PC_OK) {
                            for (size_t t = 0; t < colors.len; t++) {
                                pc_compose_fx_sample_gradient(pass->paint.gradient, (float)t / 4095,
                                                              colors.px[t]);
                            }
                        }
                    } else {
                        rc = pc_gpu_paint_pixels(&pass->paint, doc->mode, rect, &colors, err);
                    }
                    if (rc != PC_OK) {
                        return rc;
                    }
                    gl->glActiveTexture(GL_TEXTURE0 + 5);
                    texture(gl, gpu->paint, (int)pc_rect_width(colors.rect), (int)pc_rect_height(colors.rect),
                            colors.px);
                    gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "paintTexture"), 5);
                    pc_compose_buffer_fini(&colors);
                }
            }
            bool masked = pass->layer && (pass->layer->has_mask || pass->layer->has_vector_mask);
            if (masked) {
                pc_status rc = upload_mask(gpu, doc, pass->layer, rect, err);
                if (rc != PC_OK) {
                    return rc;
                }
            }
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "maskTexture"), 2);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "hasMask"), masked ? 1 : 0);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "kernel"),
                            pass->kernel == PC_GPU_MASK        ? 1
                            : pass->kernel == PC_GPU_LERP      ? 2
                            : pass->kernel == PC_GPU_ATOP      ? 3
                            : pass->kernel == PC_GPU_ADJUST    ? 4
                            : pass->kernel == PC_GPU_ADJMIX    ? 5
                            : pass->kernel == PC_GPU_CHANNELS  ? 6
                            : pass->kernel == PC_GPU_BLEND_IF  ? 7
                            : pass->kernel == PC_GPU_COPY_RECT ? 8
                            : pass->kernel == PC_GPU_ADD_DIFF  ? 9
                                                               : 0);
            if (pass->kernel == PC_GPU_COPY_RECT) {
                gl->glUniform2i(gl->glGetUniformLocation(gpu->program, "clipMin"), pass->clip.x0,
                                pass->clip.y0);
                gl->glUniform2i(gl->glGetUniformLocation(gpu->program, "clipMax"), pass->clip.x1,
                                pass->clip.y1);
            }
            gl->glActiveTexture(GL_TEXTURE0 + 4);
            gl->glBindTexture(GL_TEXTURE_2D, gpu->textures[pass->has_c ? pass->c : pass->a]);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "ownTexture"), 4);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "hasOwn"), pass->has_c ? 1 : 0);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "oneChannel"),
                            doc->mode == PC_COLOR_MODE_RGB ? 0 : 1);
            gl->glActiveTexture(GL_TEXTURE0);
            gl->glBindTexture(GL_TEXTURE_2D, gpu->textures[pass->a]);
            gl->glActiveTexture(GL_TEXTURE0 + 1);
            gl->glBindTexture(
                GL_TEXTURE_2D,
                gpu->textures[pass->kernel == PC_GPU_MASK || pass->kernel == PC_GPU_ADJUST ? pass->a
                                                                                           : pass->b]);
            gl->glUniform1i(gl->glGetUniformLocation(gpu->program, "mode"), (int)pass->mode);
            gl->glUniform4f(gl->glGetUniformLocation(gpu->program, "params"), pass->opacity, pass->fill,
                            pass->gamma > 0 ? pass->gamma : 1, 0);
            gl->glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    pass_done:
        if (gpu->prefix.active && i + 1 == gpu->prefix.checkpoint.pass_end) {
            pc_gpu_prefix_capture(gl, &gpu->prefix, gpu->framebuffer, rect, gpu->textures[pass->dst]);
        }
    }
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpu->textures[plan->root],
                               0);
    gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_FLOAT, pixels);
    atomic_fetch_add(&gpu->submissions, 1);
    return gpu_error(gpu, err);
}
pc_status pc_gpu_render(pc_gpu_compositor *gpu, const pc_document *doc, pc_rect area, pc_compose_buffer *out,
                        pc_error *err)
{
    if (!gpu || !doc || !out) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU render input");
    }
    if (!pc_gpu_compositor_healthy(gpu)) {
        return pc_fail(err, PC_ERR_IO, "GPU device faulted");
    }
    pc_gpu_plan plan = {0};
    pc_status rc = pc_gpu_plan_build(doc, &plan, err);
    if (rc != PC_OK) {
        pc_gpu_prefix_invalidate(&gpu->gl, &gpu->prefix, false);
        return rc;
    }
    for (size_t i = 0; i < plan.count; i++) {
        const pc_gpu_adjust_program *adjust = &plan.passes[i].adjustment;
        if (gpu->maximum < 256 || adjust->width > (size_t)gpu->maximum ||
            adjust->height > (size_t)gpu->maximum) {
            pc_gpu_plan_fini(&plan);
            return pc_fail(err, PC_ERR_UNSUPPORTED, "GPU texture dimension limit");
        }
    }
    if (plan.slots > 64) {
        pc_gpu_plan_fini(&plan);
        return pc_fail(err, PC_ERR_LIMIT, "GPU live texture budget");
    }
    pc_compose_buffer result = {0};
    rc = pc_compose_buffer_transparent(area, &result, err);
    if (rc != PC_OK) {
        pc_gpu_plan_fini(&plan);
        return rc;
    }
    float (*pixels)[4] = pc_malloc_array(256 * 256, sizeof *pixels);
    if (!pixels) {
        pc_compose_buffer_fini(&result);
        pc_gpu_plan_fini(&plan);
        return pc_fail(err, PC_ERR_NOMEM, "GPU readback");
    }
    pc_compose_patterns *patterns = NULL;
    rc = pc_compose_patterns_new(doc->patterns, doc->npatterns, PC_COMPOSE_PREPARED_PATTERN_BYTES, &patterns,
                                 err);
    if (rc != PC_OK) {
        free(pixels);
        pc_gpu_plan_fini(&plan);
        pc_compose_buffer_fini(&result);
        return rc;
    }
    pc_compose_patterns_set_mode(patterns, doc->mode);
    pc_cmyk_space *space = pc_compose_cmyk_space(doc);
    pc_cmyk_space *old_space = pc_cmyk_space_enter(space);
    /* Cache allocation is an optimisation. An allocation failure keeps the full render path. */
    if (pc_gpu_prefix_prepare(&gpu->gl, &gpu->prefix, doc, &plan, NULL) != PC_OK) {
        gpu->prefix.active = false;
    }
    bool needs_effects = false;
    for (size_t i = 0; i < plan.count; i++) {
        needs_effects = needs_effects || plan.passes[i].fx_layer != NULL;
    }
    if (needs_effects && !gpu->effects) {
        rc = pc_gpu_fx_context_new(&gpu->gl, &gpu->effects, err);
    }
    if (needs_effects && rc == PC_OK) {
        rc = pc_gpu_fx_resources_build(&gpu->gl, gpu->effects, doc, &plan, area, &gpu->fx_resources, err);
    }
    GLint previous = 0;
    gpu->gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    for (int64_t y = area.y0; rc == PC_OK && y < area.y1;) {
        for (int64_t x = area.x0; rc == PC_OK && x < area.x1;) {
            pc_rect cell =
                pc_gpu_prefix_cell_of(pc_rect_make((int32_t)x, (int32_t)y, (int32_t)x, (int32_t)y));
            int64_t next_x = cell.x1 < area.x1 ? cell.x1 : area.x1;
            int64_t next_y = cell.y1 < area.y1 ? cell.y1 : area.y1;
            pc_rect rect = pc_rect_make((int32_t)x, (int32_t)y, (int32_t)next_x, (int32_t)next_y);
            rc = chunk(gpu, &plan, rect, pixels, doc, patterns, err);
            if (rc == PC_OK) {
                size_t w = pc_rect_width(rect), width = pc_rect_width(area);
                for (size_t row = 0; row < pc_rect_height(rect); row++) {
                    size_t offset = ((size_t)(y - area.y0) + row) * width + (size_t)(x - area.x0);
                    memcpy(&result.px[offset], &pixels[row * w], w * sizeof *pixels);
                }
            }
            x = next_x;
        }
        pc_rect cell = pc_gpu_prefix_cell_of(pc_rect_make(area.x0, (int32_t)y, area.x0, (int32_t)y));
        y = cell.y1 < area.y1 ? cell.y1 : area.y1;
    }
    gpu->gl.glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous);
    gpu->gl.glActiveTexture(GL_TEXTURE0);
    gpu->gl.glBindVertexArray(0);
    free(pixels);
    pc_compose_patterns_free(patterns);
    pc_cmyk_space_leave(old_space);
    pc_cmyk_space_release(space);
    bool fx_lost = false;
    if (pc_gpu_fx_context_faulted(gpu->effects, &fx_lost)) {
        pc_gpu_compositor_fault(gpu, fx_lost, "GPU effect renderer failed");
    }
    pc_gpu_fx_resources_fini(gpu->effects, &gpu->fx_resources);
    pc_gpu_plan_fini(&plan);
    if (rc != PC_OK) {
        pc_compose_buffer_fini(&result);
        return rc;
    }
    pc_compose_buffer_fini(out);
    *out = result;
    return PC_OK;
}
