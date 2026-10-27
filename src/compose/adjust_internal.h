/* Private to src/compose (C2): helpers shared by adjust.c and adjust_tone.c. */
#ifndef PC_COMPOSE_ADJUST_INTERNAL_H
#define PC_COMPOSE_ADJUST_INTERNAL_H

#include <stddef.h>

#include "compose/adjust.h"

/* Linear interpolation in a table of len >= 1 samples over 0..=1 (adjust.rs lut). */
float pc_cmp_lut(const float *table, size_t len, float v);
/* Applies channel LUTs (4 rows) to a display-RGB colour in CMYK or Lab space (tone_in_space). */
void pc_cmp_tone_in_space(pc_tone_space space, const float *luts, const float c[3], float out[3]);

/* Gradient Map's colour at t (adjust::gradient). */
void pc_cmp_gradient_map_color(const pc_gradient_map_stop *s, size_t n, float t, float out[3]);
/* Legacy Brightness/Contrast as one channel's tone curve. */
typedef struct pc_cmp_legacy_bc {
    float k;
    float pre;
    float post;
} pc_cmp_legacy_bc;
pc_cmp_legacy_bc pc_cmp_legacy_bc_new(float brightness, float contrast);
float pc_cmp_legacy_bc_apply(const pc_cmp_legacy_bc *l, float v);
/* Modern (CS3+) Brightness/Contrast as a PC_COMPOSE_LUT_SIZE lookup table over 0..=1. */
pc_status pc_cmp_modern_lut(float brightness, float contrast, float *out, pc_error *err);

/* --- CMYK/Lab stored-channel helpers (adjust_lab.c), shared with the ink/Lab native paths --- */
/* Whether a CMYK document evaluates adj on C, M and Y as inverted RGB: Hue/Saturation (not
 * colorize) and Color Balance (adjust.rs cmy_adjustment). */
bool pc_cmp_cmy_adjustment(const pc_adjustment *adj);
/* Invert, Posterize or Threshold on a pixel's own channels (L,a,b or C,M,Y,K); gray is the luma
 * CMYK Threshold compares (pc_cmp_ink_luma). adjust.rs stored_channels. */
void pc_cmp_stored_channels(const pc_adjustment *adj, pc_color_mode mode, bool has_depth,
                            pc_sample_type depth, float *ch, size_t nch, float gray);
/* Hue/Saturation (not colorize) and Color Balance on straight inks (C,M,Y,K; K kept). */
pc_status pc_cmp_apply_cmy(const pc_adjustment *adj, float (*inks)[4], size_t n, pc_sample_type depth,
                           pc_error *err);
/* One CMYK ink through a channel tone table built over the stored value 1 - ink. */
float pc_cmp_ink_through(const float *table, float ink);
/* Brightness/Contrast as the ink tone table (modern LUT, or the legacy stored-value curve). */
pc_status pc_cmp_ink_bc_lut(float brightness, float contrast, bool legacy_flag, float *table, pc_error *err);
/* The luma CMYK Threshold compares (Rec. 601 on the inks' plain complement). */
float pc_cmp_ink_luma(const float ink[4]);

#endif
