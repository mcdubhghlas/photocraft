/* IEEE 754 binary16 ("half") conversions, bit-compatible with the `half` 2.7 crate's software
 * paths (replaces `half::f16`, REWRITE_PLAN.md D15). A half is passed around as its uint16_t bit
 * pattern. Pure functions; thread-safe.
 *
 * Rounding is to nearest, ties to even. f64 -> f16 rounds through f32 first, as the crate does on
 * x86 with F16C (the crate is platform-dependent here; the reference itself only uses f32 -> f16).
 * NaNs keep their sign and top payload bits and come out quiet. */
#ifndef PC_BASE_HALF_H
#define PC_BASE_HALF_H

#include <stdint.h>

uint16_t pc_f16_from_f32(float value);
uint16_t pc_f16_from_f64(double value);
float pc_f16_to_f32(uint16_t bits);
double pc_f16_to_f64(uint16_t bits);

#endif
