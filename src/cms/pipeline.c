#include "pipeline.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"

static const float XYZ_SCALE = 65535.0f / 32768.0f;
static const float V2_L = 100.0f * 65535.0f / 65280.0f;
static const float V2_AB = 255.0f * 65535.0f / 65280.0f;

pc_cms_stage pc_cms_stage_simple(pc_cms_stage_kind kind)
{
    pc_cms_stage s;
    memset(&s, 0, sizeof s);
    s.kind = kind;
    return s;
}

pc_cms_stage pc_cms_stage_curves(pc_cms_stage_kind kind, pc_cms_curve *curves, size_t n)
{
    pc_cms_stage s = pc_cms_stage_simple(kind);
    s.curves = curves;
    s.ncurves = n;
    return s;
}

pc_status pc_cms_stage_curves_copy(pc_cms_stage_kind kind, const pc_cms_curve *curves, size_t n, pc_cms_stage *out,
                                   pc_error *err)
{
    pc_cms_curve *c = pc_calloc_array(n, sizeof *c);
    size_t i;
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (i = 0; i < n; i++) {
        pc_status s = pc_cms_curve_clone(&curves[i], &c[i], err);
        if (s != PC_OK) {
            while (i-- > 0) {
                pc_cms_curve_fini(&c[i]);
            }
            free(c);
            return s;
        }
    }
    *out = pc_cms_stage_curves(kind, c, n);
    return PC_OK;
}

pc_status pc_cms_stage_matrix(size_t rows, size_t cols, const double *m, const double *offset, pc_cms_stage *out,
                              pc_error *err)
{
    pc_cms_stage s = pc_cms_stage_simple(PC_STAGE_MATRIX);
    if (rows < 1 || rows > PC_PIPE_MAX || cols < 1 || cols > PC_PIPE_MAX) {
        return pc_fail(err, PC_ERR_INVALID, "bad matrix stage size");
    }
    s.rows = rows;
    s.cols = cols;
    s.m = pc_malloc_array(rows * cols, sizeof *s.m);
    s.offset = pc_calloc_array(rows, sizeof *s.offset);
    if (s.m == NULL || s.offset == NULL) {
        free(s.m);
        free(s.offset);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memcpy(s.m, m, rows * cols * sizeof *s.m);
    if (offset != NULL) {
        memcpy(s.offset, offset, rows * sizeof *s.offset);
    }
    *out = s;
    return PC_OK;
}

pc_status pc_cms_stage_matrix3(const pc_mat3 *m, pc_cms_stage *out, pc_error *err)
{
    return pc_cms_stage_matrix(3, 3, &m->m[0][0], NULL, out, err);
}

pc_cms_stage pc_cms_stage_clut(pc_cms_clut *clut)
{
    pc_cms_stage s = pc_cms_stage_simple(PC_STAGE_CLUT);
    s.clut = *clut;
    memset(clut, 0, sizeof *clut);
    return s;
}

void pc_cms_stage_fini(pc_cms_stage *s)
{
    size_t i;
    if (s == NULL) {
        return;
    }
    for (i = 0; i < s->ncurves; i++) {
        pc_cms_curve_fini(&s->curves[i]);
    }
    free(s->curves);
    free(s->m);
    free(s->offset);
    pc_cms_clut_fini(&s->clut);
    memset(s, 0, sizeof *s);
}

pc_status pc_cms_stage_clone(const pc_cms_stage *s, pc_cms_stage *out, pc_error *err)
{
    switch (s->kind) {
    case PC_STAGE_CURVES:
    case PC_STAGE_INV_CURVES: return pc_cms_stage_curves_copy(s->kind, s->curves, s->ncurves, out, err);
    case PC_STAGE_MATRIX: return pc_cms_stage_matrix(s->rows, s->cols, s->m, s->offset, out, err);
    case PC_STAGE_CLUT: {
        pc_cms_clut c;
        pc_status st = pc_cms_clut_clone(&s->clut, &c, err);
        if (st == PC_OK) {
            *out = pc_cms_stage_clut(&c);
        }
        return st;
    }
    default: *out = pc_cms_stage_simple(s->kind); return PC_OK;
    }
}

size_t pc_cms_stage_outputs(const pc_cms_stage *s, size_t n)
{
    switch (s->kind) {
    case PC_STAGE_MATRIX: return s->rows;
    case PC_STAGE_CLUT: return s->clut.outputs;
    default: return n;
    }
}

pc_status pc_cms_stages_push(pc_cms_stages *list, pc_cms_stage *s, pc_error *err)
{
    if (list->n == list->cap) {
        pc_cms_stage *v = pc_grow_array(list->v, &list->cap, list->n + 1, sizeof *v);
        if (v == NULL) {
            pc_cms_stage_fini(s);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        list->v = v;
    }
    list->v[list->n++] = *s;
    memset(s, 0, sizeof *s);
    return PC_OK;
}

pc_status pc_cms_stages_append(pc_cms_stages *list, pc_cms_stages *from, pc_error *err)
{
    pc_status st = PC_OK;
    size_t i;
    for (i = 0; i < from->n; i++) {
        if (st == PC_OK) {
            st = pc_cms_stages_push(list, &from->v[i], err);
        } else {
            pc_cms_stage_fini(&from->v[i]);
        }
    }
    free(from->v);
    from->v = NULL;
    from->n = 0;
    from->cap = 0;
    return st;
}

pc_status pc_cms_stages_append_copy(pc_cms_stages *list, const pc_cms_stages *from, pc_error *err)
{
    size_t i;
    for (i = 0; i < from->n; i++) {
        pc_cms_stage c;
        pc_status st = pc_cms_stage_clone(&from->v[i], &c, err);
        if (st == PC_OK) {
            st = pc_cms_stages_push(list, &c, err);
        }
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

pc_status pc_cms_stages_clone(const pc_cms_stages *list, pc_cms_stages *out, pc_error *err)
{
    pc_cms_stages c = PC_CMS_STAGES_INIT;
    pc_status st = pc_cms_stages_append_copy(&c, list, err);
    if (st != PC_OK) {
        pc_cms_stages_fini(&c);
        return st;
    }
    *out = c;
    return PC_OK;
}

void pc_cms_stages_fini(pc_cms_stages *list)
{
    size_t i;
    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->n; i++) {
        pc_cms_stage_fini(&list->v[i]);
    }
    free(list->v);
    list->v = NULL;
    list->n = 0;
    list->cap = 0;
}

/* Run one stage on v (n live channels); returns the new channel count. */
static size_t run(const pc_cms_stage *s, float v[PC_PIPE_MAX], size_t n)
{
    size_t k;
    switch (s->kind) {
    case PC_STAGE_CURVES:
        for (k = 0; k < s->ncurves && k < PC_PIPE_MAX; k++) {
            v[k] = pc_cms_curve_eval(&s->curves[k], v[k]);
        }
        return n;
    case PC_STAGE_INV_CURVES:
        for (k = 0; k < s->ncurves && k < PC_PIPE_MAX; k++) {
            v[k] = pc_cms_curve_eval_inverse(&s->curves[k], v[k]);
        }
        return n;
    case PC_STAGE_MATRIX: {
        float o[PC_PIPE_MAX];
        size_t r;
        size_t c;
        for (r = 0; r < s->rows; r++) {
            double acc = s->offset[r];
            for (c = 0; c < s->cols; c++) {
                acc += s->m[r * s->cols + c] * (double)v[c];
            }
            o[r] = pc_f64_to_f32(acc);
        }
        memcpy(v, o, s->rows * sizeof *v);
        return s->rows;
    }
    case PC_STAGE_CLUT: {
        float o[PC_PIPE_MAX];
        float inp[PC_PIPE_MAX];
        memset(inp, 0, sizeof inp);
        memcpy(inp, v, n * sizeof *v);
        pc_cms_clut_eval(&s->clut, inp, o);
        memcpy(v, o, s->clut.outputs * sizeof *v);
        return s->clut.outputs;
    }
    case PC_STAGE_LAB_TO_XYZ: {
        float x[3];
        pc_lab_to_xyz_f32(v, x);
        memcpy(v, x, sizeof x);
        return 3;
    }
    case PC_STAGE_XYZ_TO_LAB: {
        float in[3];
        float l[3];
        in[0] = fmaxf(v[0], 0.0f);
        in[1] = fmaxf(v[1], 0.0f);
        in[2] = fmaxf(v[2], 0.0f);
        pc_xyz_to_lab_f32(in, l);
        memcpy(v, l, sizeof l);
        return 3;
    }
    case PC_STAGE_DECODE_LAB_V4:
        v[0] *= 100.0f;
        v[1] = v[1] * 255.0f - 128.0f;
        v[2] = v[2] * 255.0f - 128.0f;
        return 3;
    case PC_STAGE_ENCODE_LAB_V4:
        v[0] = pc_clampf(v[0] / 100.0f, 0.0f, 1.0f);
        v[1] = pc_clampf((v[1] + 128.0f) / 255.0f, 0.0f, 1.0f);
        v[2] = pc_clampf((v[2] + 128.0f) / 255.0f, 0.0f, 1.0f);
        return 3;
    case PC_STAGE_DECODE_LAB_V2:
        v[0] *= V2_L;
        v[1] = v[1] * V2_AB - 128.0f;
        v[2] = v[2] * V2_AB - 128.0f;
        return 3;
    case PC_STAGE_ENCODE_LAB_V2:
        v[0] = pc_clampf(v[0] / V2_L, 0.0f, 1.0f);
        v[1] = pc_clampf((v[1] + 128.0f) / V2_AB, 0.0f, 1.0f);
        v[2] = pc_clampf((v[2] + 128.0f) / V2_AB, 0.0f, 1.0f);
        return 3;
    case PC_STAGE_DECODE_XYZ:
        for (k = 0; k < 3; k++) {
            v[k] *= XYZ_SCALE;
        }
        return 3;
    case PC_STAGE_ENCODE_XYZ:
        for (k = 0; k < 3; k++) {
            v[k] = pc_clampf(v[k] / XYZ_SCALE, 0.0f, 1.0f);
        }
        return 3;
    case PC_STAGE_CLAMP01:
        for (k = 0; k < n; k++) {
            v[k] = pc_clampf(v[k], 0.0f, 1.0f);
        }
        return n;
    }
    return n;
}

void pc_cms_pipeline_init(pc_cms_pipeline *p, size_t inputs, pc_cms_stages *stages)
{
    size_t i;
    size_t n = inputs;
    p->inputs = inputs;
    p->stages = *stages;
    memset(stages, 0, sizeof *stages);
    for (i = 0; i < p->stages.n; i++) {
        n = pc_cms_stage_outputs(&p->stages.v[i], n);
    }
    p->outputs = n;
}

void pc_cms_pipeline_fini(pc_cms_pipeline *p)
{
    if (p != NULL) {
        pc_cms_stages_fini(&p->stages);
        p->inputs = 0;
        p->outputs = 0;
    }
}

pc_status pc_cms_pipeline_clone(const pc_cms_pipeline *p, pc_cms_pipeline *out, pc_error *err)
{
    pc_cms_stages s;
    pc_status st = pc_cms_stages_clone(&p->stages, &s, err);
    if (st != PC_OK) {
        return st;
    }
    out->inputs = p->inputs;
    out->outputs = p->outputs;
    out->stages = s;
    return PC_OK;
}

void pc_cms_pipeline_eval(const pc_cms_pipeline *p, const float *in, float *out)
{
    float v[PC_PIPE_MAX];
    size_t n = p->inputs < PC_PIPE_MAX ? p->inputs : PC_PIPE_MAX;
    size_t i;
    memset(v, 0, sizeof v);
    memcpy(v, in, n * sizeof *v);
    for (i = 0; i < p->stages.n; i++) {
        n = run(&p->stages.v[i], v, n);
    }
    memcpy(out, v, (p->outputs < PC_PIPE_MAX ? p->outputs : PC_PIPE_MAX) * sizeof *out);
}

static bool all_identity(const pc_cms_stage *s)
{
    size_t i;
    for (i = 0; i < s->ncurves; i++) {
        if (!pc_cms_curve_is_identity(&s->curves[i])) {
            return false;
        }
    }
    return true;
}

static bool is_noop(const pc_cms_stage *s)
{
    size_t i;
    if (s->kind == PC_STAGE_CURVES || s->kind == PC_STAGE_INV_CURVES) {
        return all_identity(s);
    }
    if (s->kind == PC_STAGE_MATRIX && s->rows == 3 && s->cols == 3) {
        for (i = 0; i < 3; i++) {
            if (!(fabs(s->offset[i]) < 1e-12)) {
                return false;
            }
        }
        for (i = 0; i < 9; i++) {
            if (!(fabs(s->m[i] - (i % 4 == 0 ? 1.0 : 0.0)) < 1e-12)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static bool cancels(pc_cms_stage_kind last, pc_cms_stage_kind next)
{
    return (last == PC_STAGE_LAB_TO_XYZ && next == PC_STAGE_XYZ_TO_LAB) ||
           (last == PC_STAGE_XYZ_TO_LAB && next == PC_STAGE_LAB_TO_XYZ) ||
           (last == PC_STAGE_ENCODE_LAB_V4 && next == PC_STAGE_DECODE_LAB_V4) ||
           (last == PC_STAGE_ENCODE_LAB_V2 && next == PC_STAGE_DECODE_LAB_V2) ||
           (last == PC_STAGE_ENCODE_XYZ && next == PC_STAGE_DECODE_XYZ) ||
           /* Decode -> encode of one encoding is exact inside the encoding's gamut, which always
            * holds for data coming out of a table. */
           (last == PC_STAGE_DECODE_LAB_V4 && next == PC_STAGE_ENCODE_LAB_V4) ||
           (last == PC_STAGE_DECODE_LAB_V2 && next == PC_STAGE_ENCODE_LAB_V2) ||
           (last == PC_STAGE_DECODE_XYZ && next == PC_STAGE_ENCODE_XYZ);
}

static bool is_mat3(const pc_cms_stage *s)
{
    return s->kind == PC_STAGE_MATRIX && s->rows == 3 && s->cols == 3;
}

pc_status pc_cms_pipeline_optimize(pc_cms_pipeline *p, pc_error *err)
{
    pc_cms_stages out = PC_CMS_STAGES_INIT;
    pc_cms_stages work;
    pc_status st;
    size_t i;
    /* Work on a copy so the pipeline is untouched if memory runs out. */
    st = pc_cms_stages_clone(&p->stages, &work, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < work.n; i++) {
        pc_cms_stage *s = &work.v[i];
        pc_cms_stage *last = out.n > 0 ? &out.v[out.n - 1] : NULL;
        if (is_noop(s)) {
            pc_cms_stage_fini(s);
            continue;
        }
        if (last != NULL && cancels(last->kind, s->kind)) {
            pc_cms_stage_fini(last);
            out.n--;
            pc_cms_stage_fini(s);
            continue;
        }
        if (last != NULL && is_mat3(last) && is_mat3(s)) {
            double m[9];
            double off[3];
            const double *a = last->m;
            const double *b = s->m;
            size_t r;
            size_t c;
            size_t k;
            pc_cms_stage merged;
            for (r = 0; r < 3; r++) {
                double sum;
                for (c = 0; c < 3; c++) {
                    sum = -0.0; /* Rust's f64 Sum starts from -0.0 */
                    for (k = 0; k < 3; k++) {
                        sum += b[r * 3 + k] * a[k * 3 + c];
                    }
                    m[r * 3 + c] = sum;
                }
                sum = -0.0;
                for (k = 0; k < 3; k++) {
                    sum += b[r * 3 + k] * last->offset[k];
                }
                off[r] = sum + s->offset[r];
            }
            st = pc_cms_stage_matrix(3, 3, m, off, &merged, err);
            if (st != PC_OK) {
                break;
            }
            pc_cms_stage_fini(last);
            *last = merged;
            pc_cms_stage_fini(s);
            continue;
        }
        st = pc_cms_stages_push(&out, s, err);
        if (st != PC_OK) {
            break;
        }
    }
    if (st != PC_OK) {
        for (; i < work.n; i++) {
            pc_cms_stage_fini(&work.v[i]);
        }
        free(work.v);
        pc_cms_stages_fini(&out);
        return st;
    }
    free(work.v);
    pc_cms_stages_fini(&p->stages);
    p->stages = out;
    return PC_OK;
}
