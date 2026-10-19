#ifndef PC_GPU_FX_GL_H
#define PC_GPU_FX_GL_H
#include "gpu/fx.h"
#include "gpu/gl33.h"
typedef struct pc_gpu_fx_context pc_gpu_fx_context;
typedef struct pc_gpu_fx_output {
    GLuint textures[2];
    pc_rect region, valid; /* sampling is guaranteed only inside valid (in document coordinates) */
    size_t count;
    void *shared; /* private; shallow copying an output does not retain it */
} pc_gpu_fx_output;
pc_status pc_gpu_fx_context_new(const gl33 *gl, pc_gpu_fx_context **out, pc_error *err);
uint64_t pc_gpu_fx_cache_hits(const pc_gpu_fx_context *context);
uint64_t pc_gpu_fx_fragments(const pc_gpu_fx_context *context);
uint64_t pc_gpu_fx_map_builds(const pc_gpu_fx_context *context);
void pc_gpu_fx_context_abandon(pc_gpu_fx_context *context);
bool pc_gpu_fx_context_faulted(const pc_gpu_fx_context *context, bool *lost);
void pc_gpu_fx_context_free(pc_gpu_fx_context *context);
void pc_gpu_fx_output_fini(pc_gpu_fx_context *context, pc_gpu_fx_output *output);
/* The CPU shape is borrowed. Geometry fields are computed by the shared exact CPU routines;
 * the program runs on GL R32F textures. Failed builds preserve an existing output. */
pc_status pc_gpu_fx_render(pc_gpu_fx_context *context, const pc_gpu_fx_program *program, pc_rect region,
                           const float *shape, size_t length, pc_color_mode mode, pc_gpu_fx_output *out,
                           pc_error *err);
/* Builds only the propagated stage windows needed around area. Outside valid, map pixels
 * may be zero or intermediate results; callers must not use them as a complete map. */
pc_status pc_gpu_fx_render_window(pc_gpu_fx_context *context, const pc_gpu_fx_program *program,
                                  pc_rect region, pc_rect area, const float *shape, size_t length,
                                  pc_color_mode mode, pc_gpu_fx_output *out, pc_error *err);
pc_status pc_gpu_fx_readback(pc_gpu_fx_context *context, const pc_gpu_fx_output *output, size_t map,
                             float **pixels, size_t *length, pc_error *err);
#endif
