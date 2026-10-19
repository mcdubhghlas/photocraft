#ifndef PC_GPU_FX_PAINT_H
#define PC_GPU_FX_PAINT_H
#include "compose/compose.h"
#include "compose/pattern.h"
typedef enum pc_gpu_paint_kind {
    PC_GPU_PAINT_COLOR,
    PC_GPU_PAINT_GRADIENT,
    PC_GPU_PAINT_PATTERN,
    PC_GPU_PAINT_GLOW
} pc_gpu_paint_kind;
typedef struct pc_gpu_paint {
    pc_gpu_paint_kind kind;
    float color[4], gain;
    const pc_gradient *gradient;
    const pc_pattern *pattern;
    pc_compose_placement placement;
    pc_rect frame;
} pc_gpu_paint;
pc_status pc_gpu_paint_pixels(const pc_gpu_paint *paint, pc_color_mode mode, pc_rect region,
                              pc_compose_buffer *out, pc_error *err);
#endif
