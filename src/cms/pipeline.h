/* Float evaluation pipelines: stages mapping normalised device values to the PCS or back (port
 * of crates/cms/src/pipeline.rs). This is the exact reference path; transforms build faster
 * tables from it.
 *
 * Ownership: a stage owns its curves / matrix / CLUT; a stage list owns its stages; a pipeline
 * owns its list. Functions taking `pc_cms_stage *` to push move it (the source is emptied, also
 * on failure, when it is freed). */
#ifndef PC_CMS_PIPELINE_H
#define PC_CMS_PIPELINE_H

#include <stddef.h>

#include "base/status.h"
#include "clut.h"
#include "curve.h"
#include "math.h"

/* Largest channel count flowing through a pipeline. */
#define PC_PIPE_MAX 16

typedef enum pc_cms_stage_kind {
    PC_STAGE_CURVES,     /* per-channel curves */
    PC_STAGE_INV_CURVES, /* per-channel inverse curves */
    PC_STAGE_MATRIX,     /* out[r] = sum m[r][c] in[c] + offset[r], rows x cols */
    PC_STAGE_CLUT,
    PC_STAGE_LAB_TO_XYZ, /* float Lab (L 0..100) -> XYZ (D50, Y = 1) */
    PC_STAGE_XYZ_TO_LAB,
    PC_STAGE_DECODE_LAB_V4, /* ICC v4 / 8-bit Lab encoding (normalised) <-> float Lab */
    PC_STAGE_ENCODE_LAB_V4,
    PC_STAGE_DECODE_LAB_V2, /* ICC v2 legacy 16-bit Lab encoding (lut16Type) <-> float Lab */
    PC_STAGE_ENCODE_LAB_V2,
    PC_STAGE_DECODE_XYZ, /* u1Fixed15 XYZ encoding (normalised over 0..0xFFFF) <-> XYZ */
    PC_STAGE_ENCODE_XYZ,
    PC_STAGE_CLAMP01 /* clamp every channel to [0, 1] */
} pc_cms_stage_kind;

typedef struct pc_cms_stage {
    pc_cms_stage_kind kind;
    pc_cms_curve *curves; /* CURVES / INV_CURVES: owned array */
    size_t ncurves;
    size_t rows, cols; /* MATRIX (each <= PC_PIPE_MAX) */
    double *m;         /* owned, rows * cols */
    double *offset;    /* owned, rows */
    pc_cms_clut clut;  /* CLUT */
} pc_cms_stage;

typedef struct pc_cms_stages {
    pc_cms_stage *v;
    size_t n;
    size_t cap;
} pc_cms_stages;

#define PC_CMS_STAGES_INIT {NULL, 0, 0}

/* Stage constructors. They fill *out (no allocation for simple kinds). */
pc_cms_stage pc_cms_stage_simple(pc_cms_stage_kind kind);
/* Takes ownership of the curve array (n curves). */
pc_cms_stage pc_cms_stage_curves(pc_cms_stage_kind kind, pc_cms_curve *curves, size_t n);
/* Copies the curves. */
pc_status pc_cms_stage_curves_copy(pc_cms_stage_kind kind, const pc_cms_curve *curves, size_t n, pc_cms_stage *out,
                                   pc_error *err);
/* Copies m (rows * cols) and offset (rows; NULL = zeros). */
pc_status pc_cms_stage_matrix(size_t rows, size_t cols, const double *m, const double *offset, pc_cms_stage *out,
                              pc_error *err);
pc_status pc_cms_stage_matrix3(const pc_mat3 *m, pc_cms_stage *out, pc_error *err);
/* Takes ownership of the CLUT. */
pc_cms_stage pc_cms_stage_clut(pc_cms_clut *clut);

void pc_cms_stage_fini(pc_cms_stage *s);
pc_status pc_cms_stage_clone(const pc_cms_stage *s, pc_cms_stage *out, pc_error *err);
/* Output channel count given n inputs. */
size_t pc_cms_stage_outputs(const pc_cms_stage *s, size_t n);

/* Append (moving *s). */
pc_status pc_cms_stages_push(pc_cms_stages *list, pc_cms_stage *s, pc_error *err);
/* Append every stage of *from (moved; *from is left empty). */
pc_status pc_cms_stages_append(pc_cms_stages *list, pc_cms_stages *from, pc_error *err);
/* Append copies of from's stages. */
pc_status pc_cms_stages_append_copy(pc_cms_stages *list, const pc_cms_stages *from, pc_error *err);
pc_status pc_cms_stages_clone(const pc_cms_stages *list, pc_cms_stages *out, pc_error *err);
void pc_cms_stages_fini(pc_cms_stages *list);

typedef struct pc_cms_pipeline {
    size_t inputs;
    size_t outputs;
    pc_cms_stages stages;
} pc_cms_pipeline;

/* Takes ownership of *stages. */
void pc_cms_pipeline_init(pc_cms_pipeline *p, size_t inputs, pc_cms_stages *stages);
void pc_cms_pipeline_fini(pc_cms_pipeline *p);
pc_status pc_cms_pipeline_clone(const pc_cms_pipeline *p, pc_cms_pipeline *out, pc_error *err);
/* Evaluate one pixel: in has `inputs` values, out receives `outputs`. */
void pc_cms_pipeline_eval(const pc_cms_pipeline *p, const float *in, float *out);
/* Remove no-op stages, cancel adjacent inverse pairs, merge consecutive 3x3 matrices. On
 * failure (out of memory) the pipeline is unchanged. */
pc_status pc_cms_pipeline_optimize(pc_cms_pipeline *p, pc_error *err);

#endif
