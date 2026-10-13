/* Camera Raw internals shared by camera_raw_develop.c, camera_raw_tone.c (local Laplacian
 * Shadows/Highlights) and camera_raw_wb.c (Whites/Blacks). Not part of the public API. */
#ifndef PC_ALGO_CAMERA_RAW_INT_H
#define PC_ALGO_CAMERA_RAW_INT_H

#include <stddef.h>

#include "base/status.h"

/* How the pixels encode light (camera_raw.rs Encoding): 0 for the sRGB curve, else the gamma of
 * a pure power law (0.5..4). */
typedef float pc_algo_cr_encoding;

pc_algo_cr_encoding pc_algo_cr_encoding_of(float gamma);
float pc_algo_cr_decode(pc_algo_cr_encoding e, float v);
float pc_algo_cr_encode(pc_algo_cr_encoding e, float v);

/* Box mean of a single-channel buffer (radius r, edge clamped); *out newly allocated. */
pc_status pc_algo_cr_box_mean(const float *v, size_t w, size_t h, size_t r, float **out, pc_error *err);

/* local_tone::apply: Shadows and Highlights (-100..100) on straight RGBA pixels (w x h). */
pc_status pc_algo_cr_local_tone(float (*px)[4], size_t w, size_t h, float shadows, float highlights,
                                pc_algo_cr_encoding enc, pc_error *err);

/* Whites and Blacks (-100..100): the image's black/white points, the adapted internal amounts,
 * then the measured global curves. */
pc_status pc_algo_cr_whites_blacks(float (*px)[4], size_t w, size_t h, float whites, float blacks,
                                   pc_algo_cr_encoding enc, pc_error *err);
/* whites_blacks::{whites, blacks, amounts} (exposed for tests). */
float pc_algo_cr_whites_curve(float v, float x);
float pc_algo_cr_blacks_curve(float v, float x);
void pc_algo_cr_wb_amounts(float whites, float blacks, float bp, float wp, float *xw, float *xb);

#endif
