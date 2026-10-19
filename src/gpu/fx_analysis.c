#include "gpu/fx.h"
#include <limits.h>
#include <math.h>
#include <string.h>

static int32_t add(int32_t a, int32_t b)
{
    int64_t sum = (int64_t)a + b;
    return sum > INT32_MAX ? INT32_MAX : sum < 0 ? 0 : (int32_t)sum;
}
int32_t pc_gpu_fx_field_radius(pc_compose_fx_field_kind kind, int32_t reach)
{
    return add(reach,
               kind == PC_COMPOSE_FIELD_BEVEL_INSIDE || kind == PC_COMPOSE_FIELD_BEVEL_OUTSIDE ? 9 : 3);
}
static bool valid_input(const pc_gpu_fx_program *p, size_t at, pc_gpu_fx_input input)
{
    switch (input.kind) {
    case PC_GPU_FX_NONE:
    case PC_GPU_FX_SHAPE:
        return true;
    case PC_GPU_FX_FIELD:
        return input.index < PC_GPU_FX_FIELD_COUNT && p->fields[input.index];
    case PC_GPU_FX_VALUE:
        return input.index < at;
    }
    return false;
}
static pc_status validate(const pc_gpu_fx_program *p, pc_error *err)
{
    if (!p || p->count > PC_GPU_FX_MAX_STAGES || p->maps > 2) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU effect stage count");
    }
    bool written[2] = {false, false};
    for (size_t i = 0; i < p->count; i++) {
        const pc_gpu_fx_stage *s = &p->stages[i];
        for (size_t row = 0; row < 4; row++) {
            for (size_t ch = 0; ch < 4; ch++) {
                if (!isfinite(s->params[row][ch])) {
                    return pc_fail(err, PC_ERR_INVALID, "non-finite GPU effect stage");
                }
            }
        }
        if ((unsigned)s->kernel > PC_GPU_FX_BEVEL_TEXTURE || s->lut_len > PC_COMPOSE_FX_LUT_SIZE ||
            (s->lut_len && !s->lut) ||
            (s->kernel == PC_GPU_FX_BLUR && (s->params[0][1] < 0 || s->params[0][1] > 512 ||
                                             s->lut_len != (size_t)(s->params[0][1] * 2 + 1)))) {
            return pc_fail(err, PC_ERR_INVALID, "invalid GPU effect kernel parameters");
        }
        for (size_t j = 0; j < s->lut_len; j++) {
            if (!isfinite(s->lut[j])) {
                return pc_fail(err, PC_ERR_INVALID, "non-finite GPU effect table");
            }
        }
        if (s->output >= 0 && s->output < 2) {
            if (written[s->output]) {
                return pc_fail(err, PC_ERR_INVALID, "duplicate GPU effect output");
            }
            written[s->output] = true;
        }
        if (!valid_input(p, i, s->a) || !valid_input(p, i, s->b) || !valid_input(p, i, s->shape) ||
            s->radius < 0 || s->output < -1 || (s->output >= 0 && (size_t)s->output >= p->maps)) {
            return pc_fail(err, PC_ERR_INVALID, "invalid GPU effect dependency");
        }
    }
    return PC_OK;
}
static int32_t input_reach(const pc_gpu_fx_program *p, pc_gpu_fx_input in, const int32_t *reach)
{
    if (in.kind == PC_GPU_FX_VALUE) {
        return reach[in.index];
    }
    if (in.kind == PC_GPU_FX_FIELD) {
        return pc_gpu_fx_field_radius((pc_compose_fx_field_kind)in.index, p->reach[in.index]);
    }
    return 0;
}
pc_status pc_gpu_fx_windows(const pc_gpu_fx_program *p, pc_rect damage, pc_rect region,
                            pc_rect windows[PC_GPU_FX_MAX_STAGES], pc_error *err)
{
    if (!windows) {
        return pc_fail(err, PC_ERR_INVALID, "missing GPU effect windows");
    }
    pc_status rc = validate(p, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect next[PC_GPU_FX_MAX_STAGES] = {0};
    int32_t reach[PC_GPU_FX_MAX_STAGES] = {0};
    for (size_t i = 0; i < p->count; i++) {
        const pc_gpu_fx_stage *s = &p->stages[i];
        const pc_gpu_fx_input inputs[3] = {s->a, s->b, s->shape};
        int32_t r = 0;
        for (size_t j = 0; j < 3; j++) {
            int32_t v = input_reach(p, inputs[j], reach);
            if (v > r) {
                r = v;
            }
        }
        reach[i] = add(r, s->radius);
        if (s->output >= 0 && !pc_rect_is_empty(damage)) {
            next[i] = pc_rect_intersect(pc_rect_inflate(damage, reach[i]), region);
        }
    }
    for (size_t reverse = p->count; reverse > 0; reverse--) {
        size_t i = reverse - 1;
        const pc_gpu_fx_stage *s = &p->stages[i];
        if (pc_rect_is_empty(next[i])) {
            continue;
        }
        const pc_gpu_fx_input inputs[3] = {s->a, s->b, s->shape};
        for (size_t j = 0; j < 3; j++) {
            if (inputs[j].kind == PC_GPU_FX_VALUE) {
                size_t index = inputs[j].index;
                pc_rect grown = pc_rect_intersect(pc_rect_inflate(next[i], s->radius), region);
                next[index] = pc_rect_is_empty(next[index]) ? grown : pc_rect_union(next[index], grown);
            }
        }
    }
    memcpy(windows, next, sizeof next);
    return PC_OK;
}
pc_status pc_gpu_fx_temporaries(const pc_gpu_fx_program *p, int temp[PC_GPU_FX_MAX_STAGES], size_t *count,
                                pc_error *err)
{
    if (!temp || !count) {
        return pc_fail(err, PC_ERR_INVALID, "missing GPU effect temporaries");
    }
    pc_status rc = validate(p, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t last[PC_GPU_FX_MAX_STAGES] = {0};
    bool used[PC_GPU_FX_MAX_STAGES] = {false}, released[PC_GPU_FX_MAX_STAGES] = {false};
    int next[PC_GPU_FX_MAX_STAGES];
    for (size_t i = 0; i < PC_GPU_FX_MAX_STAGES; i++) {
        next[i] = -1;
    }
    for (size_t i = 0; i < p->count; i++) {
        const pc_gpu_fx_stage *s = &p->stages[i];
        const pc_gpu_fx_input inputs[3] = {s->a, s->b, s->shape};
        for (size_t j = 0; j < 3; j++) {
            if (inputs[j].kind == PC_GPU_FX_VALUE) {
                last[inputs[j].index] = i;
            }
        }
    }
    size_t total = 0;
    for (size_t i = 0; i < p->count; i++) {
        for (size_t j = 0; j < i; j++) {
            if (next[j] >= 0 && !released[j] && last[j] < i) {
                used[(size_t)next[j]] = false;
                released[j] = true;
            }
        }
        if (p->stages[i].output >= 0) {
            continue;
        }
        size_t slot = 0;
        while (slot < total && used[slot]) {
            slot++;
        }
        used[slot] = true;
        if (slot == total) {
            total++;
        }
        next[i] = (int)slot;
    }
    memcpy(temp, next, sizeof next);
    *count = total;
    return PC_OK;
}
