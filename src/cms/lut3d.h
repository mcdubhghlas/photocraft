/* 3D lookup tables for applying a display transform on the GPU (port of crates/cms/src/lut3d.rs).
 *
 * size^3 RGBA entries: entry r + g*size + b*size^2 holds the output for (r, g, b) / (size - 1),
 * alpha 1 (the memory order of a 3D texture whose x axis is red). Owns its data. */
#ifndef PC_CMS_LUT3D_H
#define PC_CMS_LUT3D_H

#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "transform.h"

typedef struct pc_lut3d {
    size_t size;
    float *data; /* size^3 * 4 floats */
} pc_lut3d;

/* Sample a 3 -> 3 (or 3 -> 1, replicated to gray) transform; size >= 2 (and <= 256). */
pc_status pc_lut3d_from_transform(const pc_transform *t, size_t size, pc_lut3d *out, pc_error *err);
void pc_lut3d_fini(pc_lut3d *l);
/* Trilinear lookup (what GPU filtering computes). */
void pc_lut3d_sample(const pc_lut3d *l, const float rgb[3], float out[3]);
/* Little-endian RGBA16F texels (8 bytes each), appended. */
pc_status pc_lut3d_to_rgba16f(const pc_lut3d *l, pc_buf *out, pc_error *err);
/* RGBA8 texels, appended. */
pc_status pc_lut3d_to_rgba8(const pc_lut3d *l, pc_buf *out, pc_error *err);

/* This module's own binary16 conversion (round to nearest even; overflow -> +-inf; NaN kept),
 * which differs from src/base/half.h in NaN payloads, as in the reference. */
uint16_t pc_lut3d_f32_to_f16(float v);
float pc_lut3d_f16_to_f32(uint16_t h);

#endif
