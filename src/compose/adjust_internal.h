/* Private to src/compose (C2): helpers shared by adjust.c and adjust_tone.c. */
#ifndef PC_COMPOSE_ADJUST_INTERNAL_H
#define PC_COMPOSE_ADJUST_INTERNAL_H

#include <stddef.h>

#include "compose/adjust.h"

/* Linear interpolation in a table of len >= 1 samples over 0..=1 (adjust.rs lut). */
float pc_cmp_lut(const float *table, size_t len, float v);
/* Applies channel LUTs (4 rows) to a display-RGB colour in CMYK or Lab space (tone_in_space). */
void pc_cmp_tone_in_space(pc_tone_space space, const float *luts, const float c[3], float out[3]);

#endif
