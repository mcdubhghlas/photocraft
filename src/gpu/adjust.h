#ifndef PC_GPU_ADJUST_H
#define PC_GPU_ADJUST_H
#include "compose/adjust.h"
typedef struct pc_gpu_adjust_program {
    int kind;
    float params[4][4];
    float (*lut)[4];
    size_t width, height;
} pc_gpu_adjust_program;
void pc_gpu_adjust_fini(pc_gpu_adjust_program *program);
pc_status pc_gpu_adjust_build(const pc_adjustment *adjustment, const pc_document *doc,
                              pc_gpu_adjust_program *out, pc_error *err);
#endif
