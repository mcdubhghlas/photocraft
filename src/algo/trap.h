/* Image > Trap (prepress; port of crates/algo/src/trap.rs). Trapping spreads each ink slightly
 * into neighbouring inked areas so that, if the press misregisters the plates, the colours still
 * overlap at an edge instead of leaving a white gap: every ink is dilated by `width` px but only
 * under already-inked pixels, so internal colour edges get a trap band while the object-vs-paper
 * edge is left alone. Clean-room approximation of Photoshop's Trap on a flattened CMYK image. */
#ifndef PC_ALGO_TRAP_H
#define PC_ALGO_TRAP_H

#include <stddef.h>

#include "base/status.h"

/* Traps an interleaved CMYK(+alpha) buffer of w x h x ch floats in place (len = w * h * ch, else
 * PC_ERR_INVALID). Inks 0..4 are spread by `width` px; any alpha channel (ch > 4) is untouched; a
 * pixel with no ink at all (paper) keeps its value. width 0, an empty image or ch < 4 do nothing.
 * Radii past the image size act as the image size. Thread-safe (uses the global pool). */
pc_status pc_algo_trap(float *px, size_t len, size_t w, size_t h, size_t ch, size_t width, pc_error *err);

#endif
