#ifndef PC_GPU_PLAN_INTERNAL_H
#define PC_GPU_PLAN_INTERNAL_H
#include "gpu/gpu.h"
typedef struct planner {
    pc_gpu_plan plan;
    uint32_t live[256];
    const pc_document *doc;
} planner;
pc_status pc_gpu_plan_emit(planner *p, pc_gpu_pass pass, uint32_t *out, pc_error *err);
pc_status pc_gpu_plan_source(planner *p, const pc_layer *layer, unsigned depth, uint32_t *out, pc_error *err);
pc_status pc_gpu_plan_clip(planner *p, const pc_layer *layer, unsigned depth, uint32_t base, uint32_t *out,
                           pc_error *err);
pc_status pc_gpu_plan_surface(planner *p, pc_surface *surface, uint32_t *out, pc_error *err);
pc_status pc_gpu_plan_effects(planner *p, const pc_layer *layer, const pc_layer *clips, size_t count,
                              unsigned depth, uint32_t backdrop, bool atop, uint32_t *out, pc_error *err);
#endif
