/* Transforms (crates/cms/src/transform.rs). Table construction and per-pixel paths follow the
 * reference operation for operation so 8/16-bit and float results match it exactly. */
#include "transform.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "builtin.h"
#include "cms_internal.h"

enum {
    TABLE_1D = 4096,  /* 1-input device-link grid size, and shaper table segments */
    OUT_TABLE = 4096, /* output inverse-curve tables, indexed by x^(1/4) */
    PAR_MIN_PIXELS = 16 * 1024,
    PAR_CHUNK_PIXELS = 8 * 1024
};

/* ICC v4 perceptual reference medium black (ICC.1:2010 6.3.4.4), XYZ. */
static const double PERCEPTUAL_BLACK[3] = {0.00336, 0.0034731, 0.00287};

typedef enum core_kind { CORE_SHAPER, CORE_GRID, CORE_EXACT } core_kind;

typedef struct grid8_entry {
    uint32_t off;
    float frac;
} grid8_entry;

struct pc_transform {
    atomic_size_t refs;
    size_t inputs;
    size_t outputs;
    pc_transform_options opts;
    pc_cms_pipeline pipeline;
    core_kind core;
    /* CORE_SHAPER */
    float (*curves8)[256]; /* per input, or NULL for no curves */
    float *curves;         /* per input, TABLE_1D + 1 entries each, or NULL */
    size_t ncurves;
    bool has_matrix;
    size_t mrows, mcols;
    float mat[PC_PIPE_MAX * PC_PIPE_MAX];
    float moff[PC_PIPE_MAX];
    /* CORE_GRID */
    pc_cms_clut grid;
    grid8_entry (*grid8)[256]; /* per input (inputs >= 2), or NULL */
    /* output tables */
    float *post; /* outputs x OUT_TABLE, or NULL */
};

pc_transform_options pc_transform_options_default(void)
{
    pc_transform_options o;
    o.intent = PC_INTENT_RELATIVE;
    o.bpc = true;
    o.precise_float = true;
    return o;
}

static void transform_free(pc_transform *t)
{
    pc_cms_pipeline_fini(&t->pipeline);
    free(t->curves8);
    free(t->curves);
    pc_cms_clut_fini(&t->grid);
    free(t->grid8);
    free(t->post);
    free(t);
}

pc_transform *pc_transform_retain(pc_transform *t)
{
    if (t != NULL) {
        atomic_fetch_add(&t->refs, 1);
    }
    return t;
}

void pc_transform_release(pc_transform *t)
{
    if (t != NULL && atomic_fetch_sub(&t->refs, 1) == 1) {
        transform_free(t);
    }
}

size_t pc_transform_inputs(const pc_transform *t) { return t->inputs; }
size_t pc_transform_outputs(const pc_transform *t) { return t->outputs; }
pc_transform_options pc_transform_get_options(const pc_transform *t) { return t->opts; }
const pc_cms_pipeline *pc_transform_pipeline(const pc_transform *t) { return &t->pipeline; }

/* ---------------------------------------------------------------- per-pixel helpers */

static float post_eval(const pc_transform *t, size_t c, float x)
{
    const float *tab = t->post + c * OUT_TABLE;
    float u = sqrtf(sqrtf(pc_clampf(x, 0.0f, 1.0f))) * (float)(OUT_TABLE - 1);
    size_t i = pc_f32_to_size(u);
    if (i > OUT_TABLE - 2) {
        i = OUT_TABLE - 2;
    }
    return tab[i] + (tab[i + 1] - tab[i]) * (u - (float)i);
}

/* Sampled over [0, 1] with TABLE_1D segments; outside extrapolates linearly from the end
 * segments so HDR floats stay usable in the fast path. */
static float shaper_table(const float *tab, float x)
{
    const size_t n = TABLE_1D;
    float p = x * (float)n;
    size_t i = pc_f32_to_size(fmaxf(floorf(p), 0.0f));
    if (i > n - 1) {
        i = n - 1;
    }
    return tab[i] + (tab[i + 1] - tab[i]) * (p - (float)i);
}

static void apply_matrix(const pc_transform *t, const float v[PC_PIPE_MAX], float out[PC_PIPE_MAX], size_t n)
{
    size_t r;
    size_t c;
    if (!t->has_matrix) {
        memcpy(out, v, n * sizeof *out);
        return;
    }
    for (r = 0; r < t->mrows; r++) {
        float s = t->moff[r];
        for (c = 0; c < t->mcols; c++) {
            s += t->mat[r * t->mcols + c] * v[c];
        }
        out[r] = s;
    }
}

static void finish(const pc_transform *t, const float *mid, float *out)
{
    size_t k;
    if (t->post != NULL) {
        for (k = 0; k < t->outputs; k++) {
            out[k] = post_eval(t, k, mid[k]);
        }
    } else {
        memcpy(out, mid, t->outputs * sizeof *out);
    }
}

void pc_transform_eval(const pc_transform *t, const float *in, float *out)
{
    pc_cms_pipeline_eval(&t->pipeline, in, out);
}

void pc_transform_eval_fast(const pc_transform *t, const float *in, float *out)
{
    float mid[PC_PIPE_MAX];
    size_t k;
    memset(mid, 0, sizeof mid);
    switch (t->core) {
    case CORE_EXACT: pc_cms_pipeline_eval(&t->pipeline, in, out); return;
    case CORE_GRID: {
        float inp[PC_PIPE_MAX];
        memset(inp, 0, sizeof inp);
        for (k = 0; k < t->inputs && k < PC_PIPE_MAX; k++) {
            inp[k] = pc_clampf(in[k], 0.0f, 1.0f);
        }
        pc_cms_clut_eval(&t->grid, inp, mid);
        break;
    }
    case CORE_SHAPER: {
        float v[PC_PIPE_MAX];
        memset(v, 0, sizeof v);
        for (k = 0; k < t->inputs; k++) {
            v[k] = t->curves == NULL ? in[k] : shaper_table(t->curves + k * (TABLE_1D + 1), in[k]);
        }
        apply_matrix(t, v, mid, t->inputs);
        break;
    }
    }
    finish(t, mid, out);
}

/* ---------------------------------------------------------------- construction */

static pc_status build_shaper(pc_transform *t, const pc_cms_stage *curves, const pc_cms_stage *matrix, pc_error *err)
{
    size_t k;
    size_t i;
    t->core = CORE_SHAPER;
    if (curves != NULL && curves->ncurves > 0) {
        t->ncurves = curves->ncurves;
        t->curves8 = pc_malloc_array(curves->ncurves, sizeof *t->curves8);
        t->curves = pc_malloc_array(curves->ncurves, (TABLE_1D + 1) * sizeof *t->curves);
        if (t->curves8 == NULL || t->curves == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory building a transform");
        }
        for (k = 0; k < curves->ncurves; k++) {
            const pc_cms_curve *c = &curves->curves[k];
            for (i = 0; i < 256; i++) {
                t->curves8[k][i] = pc_f64_to_f32(pc_cms_curve_eval64(c, (double)i / 255.0));
            }
            for (i = 0; i <= TABLE_1D; i++) {
                t->curves[k * (TABLE_1D + 1) + i] = pc_f64_to_f32(pc_cms_curve_eval64(c, (double)i / (double)TABLE_1D));
            }
        }
    }
    if (matrix != NULL) {
        t->has_matrix = true;
        t->mrows = matrix->rows;
        t->mcols = matrix->cols;
        for (i = 0; i < matrix->rows * matrix->cols; i++) {
            t->mat[i] = pc_f64_to_f32(matrix->m[i]);
        }
        for (i = 0; i < matrix->rows; i++) {
            t->moff[i] = pc_f64_to_f32(matrix->offset[i]);
        }
    }
    return PC_OK;
}

static void eval_pipe(void *ctx, const float *in, float *out)
{
    pc_cms_pipeline_eval(ctx, in, out);
}

static bool has_clut(const pc_cms_stages *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        if (s->v[i].kind == PC_STAGE_CLUT) {
            return true;
        }
    }
    return false;
}

static pc_status build_core(pc_transform *t, pc_cms_stages *head, pc_error *err)
{
    pc_cms_pipeline head_pipe;
    size_t mid;
    pc_status s = PC_OK;
    size_t grid[4];
    size_t dims = 0;
    size_t gsize = 0;
    const pc_cms_stage *v = head->v;
    size_t n = head->n;

    if (n == 0) {
        return build_shaper(t, NULL, NULL, err);
    }
    if (n == 1 && v[0].kind == PC_STAGE_CURVES && v[0].ncurves == t->inputs) {
        return build_shaper(t, &v[0], NULL, err);
    }
    if (n == 1 && v[0].kind == PC_STAGE_MATRIX) {
        return build_shaper(t, NULL, &v[0], err);
    }
    if (n == 2 && v[0].kind == PC_STAGE_CURVES && v[0].ncurves == t->inputs && v[1].kind == PC_STAGE_MATRIX) {
        return build_shaper(t, &v[0], &v[1], err);
    }
    /* Purely analytic heads are evaluated exactly: a grid would amplify interpolation error
     * through steep output curves. */
    if (!has_clut(head)) {
        t->core = CORE_EXACT;
        return PC_OK;
    }
    switch (t->inputs) {
    case 1: dims = 1; gsize = TABLE_1D; break;
    case 2: dims = 2; gsize = PC_GRID_3D; break;
    case 3: dims = 3; gsize = PC_GRID_3D; break;
    case 4: dims = 4; gsize = PC_GRID_4D; break;
    default: t->core = CORE_EXACT; return PC_OK;
    }
    pc_cms_pipeline_init(&head_pipe, t->inputs, head);
    mid = head_pipe.outputs;
    if (mid < 1 || mid > PC_CLUT_MAX_CHANNELS) {
        /* Not representable as a table (the reference would abort here): evaluate exactly. */
        pc_cms_pipeline_fini(&head_pipe);
        t->core = CORE_EXACT;
        return PC_OK;
    }
    grid[0] = grid[1] = grid[2] = grid[3] = gsize;
    s = pc_cms_clut_sample(grid, dims, mid, eval_pipe, &head_pipe, &t->grid, err);
    pc_cms_pipeline_fini(&head_pipe);
    if (s != PC_OK) {
        return s;
    }
    t->core = CORE_GRID;
    if (t->grid.inputs >= 2) {
        const pc_cms_clut *c = &t->grid;
        size_t d;
        size_t prod = 1;
        t->grid8 = pc_malloc_array(c->inputs, sizeof *t->grid8);
        if (t->grid8 == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory building a transform");
        }
        for (d = 0; d < c->inputs; d++) {
            size_t gn = c->grid[d];
            size_t stride;
            size_t val;
            prod *= gn;
            stride = c->len / c->outputs / prod * c->outputs;
            for (val = 0; val < 256; val++) {
                float p = (float)val / 255.0f * (float)(gn - 1);
                size_t i = pc_f32_to_size(p);
                if (i > gn - 2) {
                    i = gn - 2;
                }
                t->grid8[d][val].off = (uint32_t)(i * stride);
                t->grid8[d][val].frac = p - (float)i;
            }
        }
    }
    return PC_OK;
}

pc_status pc_transform_from_pipeline(pc_cms_pipeline *pipeline, pc_transform_options opts, pc_transform **out,
                                     pc_error *err)
{
    pc_transform *t = calloc(1, sizeof *t);
    pc_cms_stages head = PC_CMS_STAGES_INIT;
    pc_status s;
    if (t == NULL) {
        pc_cms_pipeline_fini(pipeline);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory building a transform");
    }
    atomic_init(&t->refs, 1);
    t->opts = opts;
    t->pipeline = *pipeline;
    memset(pipeline, 0, sizeof *pipeline);
    s = pc_cms_pipeline_optimize(&t->pipeline, err);
    if (s == PC_OK) {
        t->inputs = t->pipeline.inputs;
        t->outputs = t->pipeline.outputs;
        if (t->inputs > PC_PIPE_MAX || t->outputs > PC_PIPE_MAX) {
            s = pc_fail(err, PC_ERR_UNSUPPORTED, "unsupported ICC feature: more than %d channels", PC_PIPE_MAX);
        }
    }
    if (s == PC_OK) {
        s = pc_cms_stages_clone(&t->pipeline.stages, &head, err);
    }
    /* Split trailing inverse curves into accurate output tables. */
    if (s == PC_OK && head.n > 0 && head.v[head.n - 1].kind == PC_STAGE_INV_CURVES &&
        head.v[head.n - 1].ncurves == t->outputs) {
        const pc_cms_stage *ic = &head.v[head.n - 1];
        size_t k;
        size_t i;
        t->post = pc_malloc_array(t->outputs, OUT_TABLE * sizeof *t->post);
        if (t->post == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory building a transform");
        } else {
            for (k = 0; k < t->outputs; k++) {
                for (i = 0; i < OUT_TABLE; i++) {
                    double u = (double)i / (double)(OUT_TABLE - 1);
                    double y = pc_cms_curve_eval_inverse64(&ic->curves[k], u * u * u * u);
                    t->post[k * OUT_TABLE + i] = pc_f64_to_f32(pc_clampd(y, 0.0, 1.0));
                }
            }
            pc_cms_stage_fini(&head.v[--head.n]);
            if (head.n > 0 && head.v[head.n - 1].kind == PC_STAGE_CLAMP01) {
                pc_cms_stage_fini(&head.v[--head.n]);
            }
        }
    }
    if (s == PC_OK) {
        s = build_core(t, &head, err);
    }
    pc_cms_stages_fini(&head);
    if (s != PC_OK) {
        transform_free(t);
        return s;
    }
    *out = t;
    return PC_OK;
}

/* Device white of a colour space (normalised), or 0 channels when it has none. */
static size_t device_white(uint32_t cs, float w[4])
{
    switch (cs) {
    case PC_CS_RGB: w[0] = w[1] = w[2] = 1.0f; return 3;
    case PC_CS_GRAY: w[0] = 1.0f; return 1;
    case PC_CS_CMYK: w[0] = w[1] = w[2] = w[3] = 0.0f; return 4;
    case PC_CS_CMY: w[0] = w[1] = w[2] = 0.0f; return 3;
    case PC_CS_LAB: w[0] = 1.0f; w[1] = 0.5f; w[2] = 0.5f; return 3;
    default: return 0;
    }
}

/* White preservation: the device-link node at the source's device white gets exactly the
 * destination's device white, so paper/white survive table quantization. */
static void fix_white(pc_transform *t, uint32_t src, uint32_t dst)
{
    float sw[4];
    float dw[4];
    size_t ns = device_white(src, sw);
    size_t nd = device_white(dst, dw);
    pc_cms_clut *c = &t->grid;
    size_t off = 0;
    size_t stride;
    size_t d;
    size_t k;
    if (ns == 0 || nd == 0) {
        return;
    }
    if (t->post != NULL) {
        /* Output curves map 1 -> 1 for additive spaces; the grid holds pre-curve values. */
        if (dst != PC_CS_RGB && dst != PC_CS_GRAY) {
            return;
        }
        for (k = 0; k < nd; k++) {
            dw[k] = 1.0f;
        }
    }
    if (t->core != CORE_GRID || c->inputs != ns || c->outputs != nd) {
        return;
    }
    stride = c->outputs;
    for (d = c->inputs; d-- > 0;) {
        size_t n = c->grid[d];
        float pos = sw[d] * (float)(n - 1);
        if (fabsf(pos - roundf(pos)) > 1e-6f) {
            return;
        }
        off += pc_f32_to_size(roundf(pos)) * stride;
        stride *= n;
    }
    memcpy(c->data + off, dw, c->outputs * sizeof *dw);
}

pc_status pc_transform_with_options(const pc_profile *src, const pc_profile *dst, pc_transform_options opts,
                                    pc_transform **out, pc_error *err)
{
    pc_cms_stages st = PC_CMS_STAGES_INIT;
    pc_cms_pipeline pipe;
    pc_transform *t;
    pc_status s = pc_cms_link_stages(src, dst, opts.intent, opts.bpc, &st, err);
    if (s != PC_OK) {
        return s;
    }
    pc_cms_pipeline_init(&pipe, pc_profile_channels(src), &st);
    s = pc_transform_from_pipeline(&pipe, opts, &t, err);
    if (s != PC_OK) {
        return s;
    }
    if (opts.intent != PC_INTENT_ABSOLUTE) {
        fix_white(t, src->space, dst->space);
    }
    *out = t;
    return PC_OK;
}

pc_status pc_transform_new(const pc_profile *src, const pc_profile *dst, pc_intent intent, bool bpc,
                           pc_transform **out, pc_error *err)
{
    pc_transform_options o = pc_transform_options_default();
    o.intent = intent;
    o.bpc = bpc;
    return pc_transform_with_options(src, dst, o, out, err);
}

pc_status pc_transform_proof(const pc_profile *src, const pc_profile *proof, const pc_profile *dst, pc_intent intent,
                             bool bpc, bool simulate_paper, pc_transform **out, pc_error *err)
{
    pc_cms_stages st = PC_CMS_STAGES_INIT;
    pc_cms_stages second = PC_CMS_STAGES_INIT;
    pc_cms_stage clamp = pc_cms_stage_simple(PC_STAGE_CLAMP01);
    pc_cms_pipeline pipe;
    pc_transform_options o;
    pc_status s = pc_cms_link_stages(src, proof, intent, bpc, &st, err);
    if (s == PC_OK) {
        s = pc_cms_stages_push(&st, &clamp, err);
    }
    if (s == PC_OK) {
        s = pc_cms_link_stages(proof, dst, simulate_paper ? PC_INTENT_ABSOLUTE : PC_INTENT_RELATIVE, !simulate_paper,
                               &second, err);
    }
    if (s == PC_OK) {
        s = pc_cms_stages_append(&st, &second, err);
    }
    if (s != PC_OK) {
        pc_cms_stages_fini(&st);
        pc_cms_stages_fini(&second);
        return s;
    }
    pc_cms_pipeline_init(&pipe, pc_profile_channels(src), &st);
    o.intent = intent;
    o.bpc = bpc;
    o.precise_float = true;
    return pc_transform_from_pipeline(&pipe, o, out, err);
}

/* ---------------------------------------------------------------- buffers */

static size_t extra_channels(const pc_transform *t, size_t ss, size_t ds, bool copy)
{
    size_t a;
    size_t b;
    if (!copy) {
        return 0;
    }
    a = ss - t->inputs;
    b = ds - t->outputs;
    return a < b ? a : b;
}

static pc_status check(const pc_transform *t, size_t ss, size_t ds, pc_error *err)
{
    if (ss < t->inputs || ds < t->outputs || ss == 0 || ds == 0) {
        return pc_fail(err, PC_ERR_INVALID, "pixel stride smaller than the channel count");
    }
    return PC_OK;
}

static uint8_t q8(float v)
{
    return pc_f32_to_u8(pc_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

static uint16_t q16(float v)
{
    return pc_f32_to_u16(pc_clampf(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
}

static void run_f32(const pc_transform *t, const float *s, float *d, size_t n, size_t ss, size_t ds, size_t extra)
{
    bool precise = t->opts.precise_float || t->core == CORE_EXACT;
    size_t p;
    size_t e;
    for (p = 0; p < n; p++) {
        const float *sp = s + p * ss;
        float *dp = d + p * ds;
        if (precise) {
            pc_cms_pipeline_eval(&t->pipeline, sp, dp);
        } else {
            pc_transform_eval_fast(t, sp, dp);
        }
        for (e = 0; e < extra; e++) {
            dp[t->outputs + e] = sp[t->inputs + e];
        }
    }
}

static void run_u8(const pc_transform *t, const uint8_t *s, uint8_t *d, size_t n, size_t ss, size_t ds,
                   size_t extra)
{
    float mid[PC_PIPE_MAX];
    float out[PC_PIPE_MAX];
    size_t p;
    size_t k;
    memset(mid, 0, sizeof mid);
    memset(out, 0, sizeof out);
    if (t->core == CORE_GRID && t->grid.inputs == 3) {
        const pc_cms_clut *c = &t->grid;
        size_t st[3];
        st[1] = c->outputs * c->grid[2];
        st[0] = st[1] * c->grid[1];
        st[2] = c->outputs;
        for (p = 0; p < n; p++) {
            const uint8_t *sp = s + p * ss;
            uint8_t *dp = d + p * ds;
            grid8_entry a = t->grid8[0][sp[0]];
            grid8_entry b = t->grid8[1][sp[1]];
            grid8_entry cc = t->grid8[2][sp[2]];
            float r[3];
            r[0] = a.frac;
            r[1] = b.frac;
            r[2] = cc.frac;
            pc_cms_clut_tetra(c, (size_t)a.off + b.off + cc.off, st, r, mid);
            finish(t, mid, out);
            for (k = 0; k < t->outputs; k++) {
                dp[k] = q8(out[k]);
            }
            for (k = 0; k < extra; k++) {
                dp[t->outputs + k] = sp[t->inputs + k];
            }
        }
        return;
    }
    if (t->core == CORE_GRID && t->grid.inputs == 4) {
        const pc_cms_clut *c = &t->grid;
        float hi[PC_PIPE_MAX];
        size_t st[3];
        size_t s0;
        memset(hi, 0, sizeof hi);
        st[2] = c->outputs;
        st[1] = st[2] * c->grid[3];
        st[0] = st[1] * c->grid[2];
        s0 = st[0] * c->grid[1];
        for (p = 0; p < n; p++) {
            const uint8_t *sp = s + p * ss;
            uint8_t *dp = d + p * ds;
            grid8_entry a = t->grid8[0][sp[0]];
            grid8_entry b = t->grid8[1][sp[1]];
            grid8_entry cc = t->grid8[2][sp[2]];
            grid8_entry k4 = t->grid8[3][sp[3]];
            size_t base = (size_t)a.off + b.off + cc.off + k4.off;
            float r[3];
            r[0] = b.frac;
            r[1] = cc.frac;
            r[2] = k4.frac;
            pc_cms_clut_tetra(c, base, st, r, mid);
            if (a.frac > 0.0f) {
                pc_cms_clut_tetra(c, base + s0, st, r, hi);
                for (k = 0; k < c->outputs; k++) {
                    mid[k] += (hi[k] - mid[k]) * a.frac;
                }
            }
            finish(t, mid, out);
            for (k = 0; k < t->outputs; k++) {
                dp[k] = q8(out[k]);
            }
            for (k = 0; k < extra; k++) {
                dp[t->outputs + k] = sp[t->inputs + k];
            }
        }
        return;
    }
    if (t->core == CORE_SHAPER) {
        float v[PC_PIPE_MAX];
        memset(v, 0, sizeof v);
        for (p = 0; p < n; p++) {
            const uint8_t *sp = s + p * ss;
            uint8_t *dp = d + p * ds;
            for (k = 0; k < t->inputs; k++) {
                v[k] = t->curves8 == NULL ? (float)sp[k] / 255.0f : t->curves8[k][sp[k]];
            }
            apply_matrix(t, v, mid, t->inputs);
            finish(t, mid, out);
            for (k = 0; k < t->outputs; k++) {
                dp[k] = q8(out[k]);
            }
            for (k = 0; k < extra; k++) {
                dp[t->outputs + k] = sp[t->inputs + k];
            }
        }
        return;
    }
    {
        float inp[PC_PIPE_MAX];
        memset(inp, 0, sizeof inp);
        for (p = 0; p < n; p++) {
            const uint8_t *sp = s + p * ss;
            uint8_t *dp = d + p * ds;
            for (k = 0; k < t->inputs; k++) {
                inp[k] = (float)sp[k] / 255.0f;
            }
            pc_transform_eval_fast(t, inp, out);
            for (k = 0; k < t->outputs; k++) {
                dp[k] = q8(out[k]);
            }
            for (k = 0; k < extra; k++) {
                dp[t->outputs + k] = sp[t->inputs + k];
            }
        }
    }
}

static void run_u16(const pc_transform *t, const uint16_t *s, uint16_t *d, size_t n, size_t ss, size_t ds,
                    size_t extra)
{
    float inp[PC_PIPE_MAX];
    float out[PC_PIPE_MAX];
    size_t p;
    size_t k;
    memset(inp, 0, sizeof inp);
    memset(out, 0, sizeof out);
    for (p = 0; p < n; p++) {
        const uint16_t *sp = s + p * ss;
        uint16_t *dp = d + p * ds;
        for (k = 0; k < t->inputs; k++) {
            inp[k] = (float)sp[k] / 65535.0f;
        }
        pc_transform_eval_fast(t, inp, out);
        for (k = 0; k < t->outputs; k++) {
            dp[k] = q16(out[k]);
        }
        for (k = 0; k < extra; k++) {
            dp[t->outputs + k] = sp[t->inputs + k];
        }
    }
}

typedef struct par_job {
    const pc_transform *t;
    pc_sample_kind kind;
    const void *src;
    void *dst;
    size_t ss;
    size_t ds;
    size_t extra;
} par_job;

static pc_status par_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const par_job *j = ctx;
    size_t n = end - begin;
    (void)chunk;
    switch (j->kind) {
    case PC_SAMPLE_U8:
        run_u8(j->t, (const uint8_t *)j->src + begin * j->ss, (uint8_t *)j->dst + begin * j->ds, n, j->ss, j->ds,
               j->extra);
        break;
    case PC_SAMPLE_U16:
        run_u16(j->t, (const uint16_t *)j->src + begin * j->ss, (uint16_t *)j->dst + begin * j->ds, n, j->ss, j->ds,
                j->extra);
        break;
    case PC_SAMPLE_F32:
        run_f32(j->t, (const float *)j->src + begin * j->ss, (float *)j->dst + begin * j->ds, n, j->ss, j->ds,
                j->extra);
        break;
    }
    return PC_OK;
}

/* Per-pixel results don't depend on chunking, so parallel output equals serial output. */
static void par_rows(par_job *j, size_t n)
{
    if (n < PAR_MIN_PIXELS) {
        (void)par_chunk(j, 0, 0, n);
        return;
    }
    (void)pc_pool_for(pc_pool_global(), n, PAR_CHUNK_PIXELS, par_chunk, j);
}

static pc_status convert(const pc_transform *t, pc_sample_kind kind, const void *src, size_t src_len, size_t ss,
                         void *dst, size_t dst_len, size_t ds, bool copy_extra, pc_error *err)
{
    par_job j;
    size_t a;
    size_t b;
    pc_status s = check(t, ss, ds, err);
    if (s != PC_OK) {
        return s;
    }
    a = src_len / ss;
    b = dst_len / ds;
    j.t = t;
    j.kind = kind;
    j.src = src;
    j.dst = dst;
    j.ss = ss;
    j.ds = ds;
    j.extra = extra_channels(t, ss, ds, copy_extra);
    par_rows(&j, a < b ? a : b);
    return PC_OK;
}

pc_status pc_transform_convert_f32(const pc_transform *t, const float *src, size_t src_len, size_t src_stride,
                                   float *dst, size_t dst_len, size_t dst_stride, bool copy_extra, pc_error *err)
{
    return convert(t, PC_SAMPLE_F32, src, src_len, src_stride, dst, dst_len, dst_stride, copy_extra, err);
}

pc_status pc_transform_convert_u8(const pc_transform *t, const uint8_t *src, size_t src_len, size_t src_stride,
                                  uint8_t *dst, size_t dst_len, size_t dst_stride, bool copy_extra, pc_error *err)
{
    return convert(t, PC_SAMPLE_U8, src, src_len, src_stride, dst, dst_len, dst_stride, copy_extra, err);
}

pc_status pc_transform_convert_u16(const pc_transform *t, const uint16_t *src, size_t src_len, size_t src_stride,
                                   uint16_t *dst, size_t dst_len, size_t dst_stride, bool copy_extra,
                                   pc_error *err)
{
    return convert(t, PC_SAMPLE_U16, src, src_len, src_stride, dst, dst_len, dst_stride, copy_extra, err);
}

typedef struct inplace_job {
    const pc_transform *t;
    float *buf;
    size_t stride;
} inplace_job;

static pc_status inplace_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const inplace_job *j = ctx;
    const pc_transform *t = j->t;
    bool precise = t->opts.precise_float || t->core == CORE_EXACT;
    float out[PC_PIPE_MAX];
    size_t p;
    (void)chunk;
    for (p = begin; p < end; p++) {
        float *px = j->buf + p * j->stride;
        if (precise) {
            pc_cms_pipeline_eval(&t->pipeline, px, out);
        } else {
            pc_transform_eval_fast(t, px, out);
        }
        memcpy(px, out, t->outputs * sizeof *out);
    }
    return PC_OK;
}

pc_status pc_transform_apply(const pc_transform *t, float *buf, size_t len, size_t stride, pc_error *err)
{
    inplace_job j;
    size_t n;
    size_t need = t->inputs > t->outputs ? t->inputs : t->outputs;
    if (stride < need || stride == 0) {
        return pc_fail(err, PC_ERR_INVALID, "stride too small");
    }
    n = len / stride;
    j.t = t;
    j.buf = buf;
    j.stride = stride;
    if (n < PAR_MIN_PIXELS) {
        return inplace_chunk(&j, 0, 0, n);
    }
    return pc_pool_for(pc_pool_global(), n, PAR_CHUNK_PIXELS, inplace_chunk, &j);
}

typedef struct many_job {
    const pc_transform *t;
    pc_sample_kind kind;
    pc_convert_job *jobs;
    size_t ss;
    size_t ds;
    size_t extra;
} many_job;

static pc_status many_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const many_job *m = ctx;
    size_t i;
    size_t bytes = m->kind == PC_SAMPLE_U8 ? 1 : (m->kind == PC_SAMPLE_U16 ? 2 : 4);
    (void)chunk;
    for (i = begin; i < end; i++) {
        const pc_convert_job *jb = &m->jobs[i];
        size_t ns = jb->src_len / bytes;
        size_t nd = jb->dst_len / bytes;
        size_t n = ns / m->ss < nd / m->ds ? ns / m->ss : nd / m->ds;
        if (m->kind == PC_SAMPLE_U8) {
            run_u8(m->t, jb->src, jb->dst, n, m->ss, m->ds, m->extra);
        } else if (m->kind == PC_SAMPLE_U16) {
            /* Copy through aligned native-endian buffers (job bytes may be unaligned). */
            uint16_t *a = pc_malloc_array(n * m->ss + 1, sizeof *a);
            uint16_t *o = pc_malloc_array(n * m->ds + 1, sizeof *o);
            if (a == NULL || o == NULL) {
                free(a);
                free(o);
                return pc_fail(NULL, PC_ERR_NOMEM, "out of memory");
            }
            memcpy(a, jb->src, n * m->ss * sizeof *a);
            memcpy(o, jb->dst, n * m->ds * sizeof *o);
            run_u16(m->t, a, o, n, m->ss, m->ds, m->extra);
            memcpy(jb->dst, o, n * m->ds * sizeof *o);
            free(a);
            free(o);
        } else {
            float *a = pc_malloc_array(n * m->ss + 1, sizeof *a);
            float *o = pc_malloc_array(n * m->ds + 1, sizeof *o);
            if (a == NULL || o == NULL) {
                free(a);
                free(o);
                return pc_fail(NULL, PC_ERR_NOMEM, "out of memory");
            }
            memcpy(a, jb->src, n * m->ss * sizeof *a);
            memcpy(o, jb->dst, n * m->ds * sizeof *o);
            run_f32(m->t, a, o, n, m->ss, m->ds, m->extra);
            memcpy(jb->dst, o, n * m->ds * sizeof *o);
            free(a);
            free(o);
        }
    }
    return PC_OK;
}

pc_status pc_transform_convert_bytes_many(const pc_transform *t, pc_sample_kind sample, pc_convert_job *jobs,
                                          size_t njobs, size_t src_stride, size_t dst_stride, bool copy_extra,
                                          pc_error *err)
{
    many_job m;
    pc_status s = check(t, src_stride, dst_stride, err);
    if (s != PC_OK) {
        return s;
    }
    m.t = t;
    m.kind = sample;
    m.jobs = jobs;
    m.ss = src_stride;
    m.ds = dst_stride;
    m.extra = extra_channels(t, src_stride, dst_stride, copy_extra);
    s = pc_pool_for(pc_pool_global(), njobs, 1, many_chunk, &m);
    return s == PC_OK ? PC_OK : pc_fail(err, s, "out of memory converting pixels");
}

/* ---------------------------------------------------------------- linking */

static void debug_vec3(const double v[3], char *out, size_t cap)
{
    char a[PC_JSON_NUM_MAX];
    char b[PC_JSON_NUM_MAX];
    char c[PC_JSON_NUM_MAX];
    const double *x = v;
    char *s[3] = {a, b, c};
    int i;
    for (i = 0; i < 3; i++) {
        if (isnan(x[i])) {
            (void)snprintf(s[i], PC_JSON_NUM_MAX, "NaN");
        } else if (isinf(x[i])) {
            (void)snprintf(s[i], PC_JSON_NUM_MAX, "%sinf", x[i] < 0 ? "-" : "");
        } else {
            (void)pc_json_format_f64(x[i], s[i]);
        }
    }
    (void)snprintf(out, cap, "[%s, %s, %s]", a, b, c);
}

/* Per-component XYZ scale and offset applied in the PCS. *have is false for the identity. */
static pc_status connection(const pc_profile *src, const pc_profile *dst, pc_intent intent, bool bpc, bool *have,
                            double scale[3], double offset[3], pc_error *err)
{
    int i;
    *have = false;
    if (intent == PC_INTENT_ABSOLUTE) {
        double ws[3];
        double wd[3];
        bool usable = true;
        bool ident = true;
        pc_profile_media_white(src, ws);
        pc_profile_media_white(dst, wd);
        /* wtpt comes straight from the file: a zero, negative or non-finite component would
         * poison every colour, so such a profile is refused for this intent. */
        for (i = 0; i < 3; i++) {
            usable = usable && isfinite(ws[i]) && ws[i] > 0.0 && isfinite(wd[i]) && wd[i] > 0.0;
        }
        if (!usable) {
            char a[160];
            char b[160];
            debug_vec3(ws, a, sizeof a);
            debug_vec3(wd, b, sizeof b);
            return cms_invalid(err, "absolute colorimetric needs a positive media white point (source %s, destination %s)",
                               a, b);
        }
        for (i = 0; i < 3; i++) {
            scale[i] = ws[i] / wd[i];
            offset[i] = 0.0;
            ident = ident && fabs(scale[i] - 1.0) < 1e-6;
        }
        *have = !ident;
        return PC_OK;
    }
    if (!bpc) {
        return PC_OK;
    }
    {
        double bs;
        double bd;
        pc_status s = pc_black_point(src, intent, true, &bs, err);
        if (s == PC_OK) {
            s = pc_black_point(dst, intent, false, &bd, err);
        }
        if (s != PC_OK) {
            return s;
        }
        if (fabs(bs - bd) < 1e-6) {
            return PC_OK;
        }
        /* Neutral black points (luminance only), mapped linearly in XYZ: source black lands on
         * destination black, white stays fixed. */
        for (i = 0; i < 3; i++) {
            double w = PC_D50[i];
            double sb = bs * w;
            double db = bd * w;
            double a = (w - db) / (w - sb);
            scale[i] = a;
            offset[i] = w * (1.0 - a);
        }
        *have = true;
    }
    return PC_OK;
}

static pc_status push_kind(pc_cms_stages *st, pc_cms_stage_kind kind, pc_error *err)
{
    pc_cms_stage s = pc_cms_stage_simple(kind);
    return pc_cms_stages_push(st, &s, err);
}

pc_status pc_cms_link_stages(const pc_profile *src, const pc_profile *dst, pc_intent intent, bool bpc,
                             pc_cms_stages *out, pc_error *err)
{
    const pc_profile *ends[2];
    pc_cms_stages st = PC_CMS_STAGES_INIT;
    pc_cms_stages dst_st = PC_CMS_STAGES_INIT;
    pc_pcs s_pcs;
    pc_pcs d_pcs;
    double scale[3];
    double offset[3];
    bool have = false;
    pc_status s;
    int i;
    ends[0] = src;
    ends[1] = dst;
    for (i = 0; i < 2; i++) {
        uint32_t c = ends[i]->class_sig;
        if ((c == PC_CLASS_DEVICE_LINK || c == PC_CLASS_NAMED_COLOR || c == PC_CLASS_ABSTRACT) &&
            ends[i]->space != PC_CS_LAB) {
            char cn[32];
            pc_class_debug_name(c, cn);
            return cms_unsupported(err, "%s profiles cannot be used as a source or destination", cn);
        }
    }
    s = pc_profile_device_to_pcs(src, intent, &st, &s_pcs, err);
    if (s == PC_OK) {
        s = pc_profile_pcs_to_device(dst, intent, &dst_st, &d_pcs, err);
    }
    if (s == PC_OK) {
        s = connection(src, dst, intent, bpc, &have, scale, offset, err);
    }
    if (s == PC_OK) {
        if (have) {
            double m[9] = {scale[0], 0.0, 0.0, 0.0, scale[1], 0.0, 0.0, 0.0, scale[2]};
            pc_cms_stage ms;
            if (s_pcs == PC_PCS_LAB) {
                s = push_kind(&st, PC_STAGE_LAB_TO_XYZ, err);
            }
            if (s == PC_OK) {
                s = pc_cms_stage_matrix(3, 3, m, offset, &ms, err);
            }
            if (s == PC_OK) {
                s = pc_cms_stages_push(&st, &ms, err);
            }
            if (s == PC_OK && d_pcs == PC_PCS_LAB) {
                s = push_kind(&st, PC_STAGE_XYZ_TO_LAB, err);
            }
        } else if (s_pcs == PC_PCS_LAB && d_pcs == PC_PCS_XYZ) {
            s = push_kind(&st, PC_STAGE_LAB_TO_XYZ, err);
        } else if (s_pcs == PC_PCS_XYZ && d_pcs == PC_PCS_LAB) {
            s = push_kind(&st, PC_STAGE_XYZ_TO_LAB, err);
        }
    }
    if (s == PC_OK) {
        s = pc_cms_stages_append(&st, &dst_st, err);
    }
    if (s != PC_OK) {
        pc_cms_stages_fini(&st);
        pc_cms_stages_fini(&dst_st);
        return s;
    }
    *out = st;
    return PC_OK;
}

/* Y of the PCS colour that stages `st` give for device values dev (consumes st). */
static double pcs_y(const pc_profile *p, pc_cms_stages *st, pc_pcs pcs, const float *dev)
{
    pc_cms_pipeline pipe;
    float o[PC_PIPE_MAX];
    double y;
    memset(o, 0, sizeof o);
    pc_cms_pipeline_init(&pipe, pc_profile_channels(p), st);
    pc_cms_pipeline_eval(&pipe, dev, o);
    pc_cms_pipeline_fini(&pipe);
    if (pcs == PC_PCS_XYZ) {
        y = (double)o[1];
    } else {
        double lab[3] = {(double)o[0], 0.0, 0.0};
        double xyz[3];
        pc_lab_to_xyz(lab, PC_D50, xyz);
        y = xyz[1];
    }
    return pc_clampd(y, 0.0, 0.5);
}

pc_status pc_black_point(const pc_profile *p, pc_intent intent, bool input, double *out, pc_error *err)
{
    pc_cms_stages to_pcs = PC_CMS_STAGES_INIT;
    pc_pcs pcs;
    bool lut_based;
    bool additive;
    float dev[PC_PIPE_MAX];
    pc_status s;
    (void)input;
    if (p->space == PC_CS_LAB || p->space == PC_CS_XYZ) {
        *out = 0.0;
        return PC_OK;
    }
    lut_based = p->a2b[0] != NULL || p->a2b[1] != NULL || p->a2b[2] != NULL;
    if (lut_based && p->version_major >= 4 && (intent == PC_INTENT_PERCEPTUAL || intent == PC_INTENT_SATURATION)) {
        *out = PERCEPTUAL_BLACK[1];
        return PC_OK;
    }
    s = pc_profile_device_to_pcs(p, intent, &to_pcs, &pcs, err);
    if (s != PC_OK) {
        return s;
    }
    memset(dev, 0, sizeof dev);
    additive = p->space == PC_CS_RGB || p->space == PC_CS_GRAY;
    if (!additive && lut_based) {
        /* Subtractive LUT profile: the darkest reachable colour (Lab 0 through BToA). */
        pc_cms_stages from = PC_CMS_STAGES_INIT;
        pc_pcs fpcs;
        pc_intent ci = intent == PC_INTENT_SATURATION ? PC_INTENT_SATURATION : PC_INTENT_RELATIVE;
        if (pc_profile_pcs_to_device(p, ci, &from, &fpcs, NULL) == PC_OK) {
            pc_cms_stages st = PC_CMS_STAGES_INIT;
            pc_cms_pipeline pipe;
            static const float zero[3] = {0.0f, 0.0f, 0.0f};
            s = PC_OK;
            if (fpcs == PC_PCS_XYZ) {
                s = push_kind(&st, PC_STAGE_LAB_TO_XYZ, err);
            }
            if (s == PC_OK) {
                s = pc_cms_stages_append(&st, &from, err);
            }
            if (s != PC_OK) {
                pc_cms_stages_fini(&st);
                pc_cms_stages_fini(&from);
                pc_cms_stages_fini(&to_pcs);
                return s;
            }
            pc_cms_pipeline_init(&pipe, 3, &st);
            pc_cms_pipeline_eval(&pipe, zero, dev);
            pc_cms_pipeline_fini(&pipe);
        } else {
            size_t k;
            for (k = 0; k < PC_PIPE_MAX; k++) {
                dev[k] = 1.0f;
            }
        }
    }
    *out = pcs_y(p, &to_pcs, pcs, dev);
    return PC_OK;
}

/* ---------------------------------------------------------------- same colours */

bool pc_profile_same_colors(const pc_profile *a, const pc_profile *b)
{
    uint64_t ha;
    uint64_t hb;
    size_t n = pc_profile_channels(a);
    const pc_profile *lab = pc_builtin_profile(PC_BUILTIN_LAB_D50);
    pc_transform *ta = NULL;
    pc_transform *tb = NULL;
    size_t steps;
    size_t total = 1;
    size_t i;
    bool same = true;
    if (pc_profile_content_hash(a, &ha, NULL) == PC_OK && pc_profile_content_hash(b, &hb, NULL) == PC_OK &&
        ha == hb) {
        return true;
    }
    if (a->space != b->space || n == 0 || n > 4 || lab == NULL) {
        return false;
    }
    if (pc_transform_new(a, lab, PC_INTENT_RELATIVE, false, &ta, NULL) != PC_OK ||
        pc_transform_new(b, lab, PC_INTENT_RELATIVE, false, &tb, NULL) != PC_OK) {
        pc_transform_release(ta);
        pc_transform_release(tb);
        return false;
    }
    /* 17 steps per channel catch a TRC that differs only in its toe; 9 keep CMYK at 6561. */
    steps = n <= 3 ? 17 : 9;
    for (i = 0; i < n; i++) {
        total *= steps;
    }
    for (i = 0; i < total && same; i++) {
        float input[4] = {0, 0, 0, 0};
        float la[PC_PIPE_MAX];
        float lb[PC_PIPE_MAX];
        double da[3];
        double db[3];
        size_t k = i;
        size_t c;
        for (c = 0; c < n; c++) {
            input[c] = (float)(k % steps) / (float)(steps - 1);
            k /= steps;
        }
        pc_transform_eval(ta, input, la);
        pc_transform_eval(tb, input, lb);
        da[0] = (double)la[0] * 100.0;
        da[1] = (double)la[1] * 255.0 - 128.0;
        da[2] = (double)la[2] * 255.0 - 128.0;
        db[0] = (double)lb[0] * 100.0;
        db[1] = (double)lb[1] * 255.0 - 128.0;
        db[2] = (double)lb[2] * 255.0 - 128.0;
        same = pc_delta_e76(da, db) <= PC_SAME_COLORS_MAX_DELTA_E;
    }
    pc_transform_release(ta);
    pc_transform_release(tb);
    return same;
}

/* ---------------------------------------------------------------- cache */

enum { CACHE_MAX = 64 };

typedef struct cache_entry {
    uint64_t hs;
    uint64_t hd;
    pc_transform_options opts;
    pc_transform *t;
} cache_entry;

static pthread_mutex_t cache_mu = PTHREAD_MUTEX_INITIALIZER;
static cache_entry cache[CACHE_MAX + 1];
static size_t cache_n;

static bool same_opts(pc_transform_options a, pc_transform_options b)
{
    return a.intent == b.intent && a.bpc == b.bpc && a.precise_float == b.precise_float;
}

pc_status pc_transform_cached(const pc_profile *src, const pc_profile *dst, pc_transform_options opts,
                              pc_transform **out, pc_error *err)
{
    uint64_t hs;
    uint64_t hd;
    size_t i;
    pc_transform *t;
    pc_status s = pc_profile_content_hash(src, &hs, err);
    if (s == PC_OK) {
        s = pc_profile_content_hash(dst, &hd, err);
    }
    if (s != PC_OK) {
        return s;
    }
    (void)pthread_mutex_lock(&cache_mu);
    for (i = 0; i < cache_n; i++) {
        if (cache[i].hs == hs && cache[i].hd == hd && same_opts(cache[i].opts, opts)) {
            *out = pc_transform_retain(cache[i].t);
            (void)pthread_mutex_unlock(&cache_mu);
            return PC_OK;
        }
    }
    (void)pthread_mutex_unlock(&cache_mu);
    s = pc_transform_with_options(src, dst, opts, &t, err);
    if (s != PC_OK) {
        return s;
    }
    (void)pthread_mutex_lock(&cache_mu);
    if (cache_n > CACHE_MAX) { /* the reference clears once it holds more than 64 */
        for (i = 0; i < cache_n; i++) {
            pc_transform_release(cache[i].t);
        }
        cache_n = 0;
    }
    cache[cache_n].hs = hs;
    cache[cache_n].hd = hd;
    cache[cache_n].opts = opts;
    cache[cache_n].t = pc_transform_retain(t);
    cache_n++;
    (void)pthread_mutex_unlock(&cache_mu);
    *out = t;
    return PC_OK;
}
