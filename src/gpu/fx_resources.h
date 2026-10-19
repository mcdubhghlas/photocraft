#ifndef PC_GPU_FX_RESOURCES_H
#define PC_GPU_FX_RESOURCES_H
#include "gpu/gpu.h"
#include "gpu/fx_gl.h"
typedef struct pc_gpu_fx_resource {
    const pc_layer *layer;
    const pc_effect *effect;
    pc_gpu_fx_output output;
} pc_gpu_fx_resource;
typedef struct pc_gpu_fx_resources {
    pc_gpu_fx_resource *items;
    size_t count, capacity, bytes;
} pc_gpu_fx_resources;
void pc_gpu_fx_resources_fini(pc_gpu_fx_context *ctx, pc_gpu_fx_resources *resources);
pc_status pc_gpu_fx_resources_build(const gl33 *gl, pc_gpu_fx_context *ctx, const pc_document *doc,
                                    const pc_gpu_plan *plan, pc_rect area, pc_gpu_fx_resources *out,
                                    pc_error *err);
const pc_gpu_fx_output *pc_gpu_fx_resources_find(const pc_gpu_fx_resources *r, const pc_layer *layer,
                                                 const pc_effect *effect);
#endif
