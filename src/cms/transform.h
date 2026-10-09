/* Colour transforms between profiles through the PCS: rendering intents, black point
 * compensation, soft-proof chains, and lookup-table evaluation of pixel buffers (port of
 * crates/cms/src/transform.rs).
 *
 * A pc_transform is immutable after construction and reference counted: build it once and use
 * it from any number of threads; release it with pc_transform_release. Buffers are interleaved,
 * colour channels first; further channels (alpha, spot) are copied when copy_extra is set.
 * Lengths are in samples; whole pixels only (n = min(src_len / src_stride, dst_len / dst_stride)). */
#ifndef PC_CMS_TRANSFORM_H
#define PC_CMS_TRANSFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "cms.h"
#include "pipeline.h"
#include "profile.h"

/* Grid sizes of the device-link tables built for integer (and fast float) evaluation. */
#define PC_GRID_3D 33
#define PC_GRID_4D 17

typedef struct pc_transform_options {
    pc_intent intent;
    bool bpc;           /* Adobe-style black point compensation (ignored for absolute) */
    bool precise_float; /* float buffers through the exact pipeline (default) or the tables */
} pc_transform_options;

/* {relative colorimetric, BPC on, precise float}. */
pc_transform_options pc_transform_options_default(void);

typedef struct pc_transform pc_transform;

typedef enum pc_sample_kind { PC_SAMPLE_U8, PC_SAMPLE_U16, PC_SAMPLE_F32 } pc_sample_kind;

pc_status pc_transform_new(const pc_profile *src, const pc_profile *dst, pc_intent intent, bool bpc,
                           pc_transform **out, pc_error *err);
pc_status pc_transform_with_options(const pc_profile *src, const pc_profile *dst, pc_transform_options opts,
                                    pc_transform **out, pc_error *err);
/* Soft proof src -> proof -> dst (View > Proof Setup): intent/bpc apply to src -> proof; the
 * proof -> display step is relative colorimetric with BPC, or absolute without BPC when
 * simulate_paper (paper colour and black ink simulated). */
pc_status pc_transform_proof(const pc_profile *src, const pc_profile *proof, const pc_profile *dst, pc_intent intent,
                             bool bpc, bool simulate_paper, pc_transform **out, pc_error *err);
/* From an arbitrary pipeline over normalised values (taken; emptied even on failure). */
pc_status pc_transform_from_pipeline(pc_cms_pipeline *pipeline, pc_transform_options opts, pc_transform **out,
                                     pc_error *err);

/* Add / drop a reference. Release frees on the last reference; NULL is ignored. */
pc_transform *pc_transform_retain(pc_transform *t);
void pc_transform_release(pc_transform *t);

size_t pc_transform_inputs(const pc_transform *t);
size_t pc_transform_outputs(const pc_transform *t);
pc_transform_options pc_transform_get_options(const pc_transform *t);
/* The exact evaluation pipeline (borrowed for the transform's lifetime). */
const pc_cms_pipeline *pc_transform_pipeline(const pc_transform *t);

/* Exact evaluation of one colour (inputs -> outputs normalised values). */
void pc_transform_eval(const pc_transform *t, const float *in, float *out);
/* Lookup-table evaluation of one colour (what integer buffers use). */
void pc_transform_eval_fast(const pc_transform *t, const float *in, float *out);

/* Buffer conversions. PC_ERR_INVALID when a stride is smaller than the channel count. */
pc_status pc_transform_convert_f32(const pc_transform *t, const float *src, size_t src_len, size_t src_stride,
                                   float *dst, size_t dst_len, size_t dst_stride, bool copy_extra, pc_error *err);
pc_status pc_transform_convert_u8(const pc_transform *t, const uint8_t *src, size_t src_len, size_t src_stride,
                                  uint8_t *dst, size_t dst_len, size_t dst_stride, bool copy_extra, pc_error *err);
pc_status pc_transform_convert_u16(const pc_transform *t, const uint16_t *src, size_t src_len, size_t src_stride,
                                   uint16_t *dst, size_t dst_len, size_t dst_stride, bool copy_extra,
                                   pc_error *err);
/* In-place float conversion; stride >= max(inputs, outputs). */
pc_status pc_transform_apply(const pc_transform *t, float *buf, size_t len, size_t stride, pc_error *err);

/* Many independent buffers of native-endian samples in one parallel pass (e.g. every tile of a
 * surface). Lengths in bytes. */
typedef struct pc_convert_job {
    const uint8_t *src;
    size_t src_len;
    uint8_t *dst;
    size_t dst_len;
} pc_convert_job;
pc_status pc_transform_convert_bytes_many(const pc_transform *t, pc_sample_kind sample, pc_convert_job *jobs,
                                          size_t njobs, size_t src_stride, size_t dst_stride, bool copy_extra,
                                          pc_error *err);

/* Luminance (Y, D50-relative) of p's black point for intent (Adobe BPC structure). */
pc_status pc_black_point(const pc_profile *p, pc_intent intent, bool input, double *out, pc_error *err);

/* Process-wide cache keyed by profile content and options. The result carries a reference
 * for the caller (release it). Thread-safe. */
pc_status pc_transform_cached(const pc_profile *src, const pc_profile *dst, pc_transform_options opts,
                              pc_transform **out, pc_error *err);

/* The stages linking src device values to dst device values (gamut.c). *out must be empty. */
pc_status pc_cms_link_stages(const pc_profile *src, const pc_profile *dst, pc_intent intent, bool bpc,
                             pc_cms_stages *out, pc_error *err);

#endif
