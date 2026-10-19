/* Layer-effect map programs. Shape and distance-field inputs are shared CPU geometry; all
 * map stages execute as GL fragment passes. Programs borrow pattern sources from the document. */
#ifndef PC_GPU_FX_H
#define PC_GPU_FX_H
#include "compose/effects_util.h"
#define PC_GPU_FX_MAX_STAGES 128
#define PC_GPU_FX_FIELD_COUNT 8

typedef enum pc_gpu_fx_kernel {
    PC_GPU_FX_SHIFT,
    PC_GPU_FX_DILATE,
    PC_GPU_FX_BLUR,
    PC_GPU_FX_GLOW,
    PC_GPU_FX_FINISH,
    PC_GPU_FX_BEVEL_HEIGHT,
    PC_GPU_FX_BEVEL_SHADE,
    PC_GPU_FX_STROKE,
    PC_GPU_FX_BEVEL_TEXTURE
} pc_gpu_fx_kernel;
typedef enum pc_gpu_fx_input_kind {
    PC_GPU_FX_NONE,
    PC_GPU_FX_SHAPE,
    PC_GPU_FX_FIELD,
    PC_GPU_FX_VALUE
} pc_gpu_fx_input_kind;
typedef struct pc_gpu_fx_input {
    pc_gpu_fx_input_kind kind;
    size_t index;
} pc_gpu_fx_input;
typedef struct pc_gpu_fx_stage {
    pc_gpu_fx_kernel kernel;
    pc_gpu_fx_input a, b, shape;
    float params[4][4]; /* p0,p1,p3,p4 in the reference */
    float *lut;
    size_t lut_len;
    const pc_pattern *pattern;
    int32_t radius;
    int output; /* -1 = temporary, otherwise final map 0 or 1 */
} pc_gpu_fx_stage;
typedef struct pc_gpu_fx_program {
    pc_gpu_fx_stage stages[PC_GPU_FX_MAX_STAGES];
    size_t count, maps;
    bool fields[PC_GPU_FX_FIELD_COUNT];
    int32_t reach[PC_GPU_FX_FIELD_COUNT];
    uint8_t key[32];
} pc_gpu_fx_program;
void pc_gpu_fx_program_key(const pc_gpu_fx_program *program, uint8_t key[32]);
void pc_gpu_fx_program_fini(pc_gpu_fx_program *program);
pc_status pc_gpu_fx_program_build(const pc_effect *effect, pc_global_light light, bool vector_shape,
                                  const pc_pattern *patterns, size_t count, const double anchor[2],
                                  pc_gpu_fx_program *out, pc_error *err);
/* Damage windows and temporary texture assignment use caller-owned fixed-size arrays.
 * `temp[i] = -1` means a final map. Returns the number of reusable temporary textures needed. */
pc_status pc_gpu_fx_windows(const pc_gpu_fx_program *program, pc_rect damage, pc_rect region,
                            pc_rect windows[PC_GPU_FX_MAX_STAGES], pc_error *err);
pc_status pc_gpu_fx_temporaries(const pc_gpu_fx_program *program, int temp[PC_GPU_FX_MAX_STAGES],
                                size_t *count, pc_error *err);
int32_t pc_gpu_fx_field_radius(pc_compose_fx_field_kind kind, int32_t reach);
#endif
