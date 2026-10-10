/* Content-aware scaling by seam carving, Edit > Content-Aware Scale (port of
 * crates/algo/src/seam.rs).
 *
 * S. Avidan, A. Shamir, "Seam Carving for Content-Aware Image Resizing", SIGGRAPH 2007: a seam is
 * an 8-connected path of one pixel per row found by dynamic programming over a gradient-magnitude
 * energy map. Shrinking removes the lowest-energy seams one at a time (the energy map is updated
 * only next to each removed seam); enlarging finds the k lowest seams on a shrinking copy and
 * duplicates them (averaged with the right neighbour). Heights are handled by transposing.
 *
 * A protect map (0..=1 per pixel, may be NULL) adds a large energy so seams avoid protected
 * pixels. Buffers are interleaved w x h x ch normalised floats (`len` must be w * h * ch);
 * deterministic; outputs newly allocated (free()). Limits as poisson.h for the input and the
 * output (new sides above PC_ALGO_HEAL_MAX_SIDE are PC_ERR_LIMIT). */
#ifndef PC_ALGO_SEAM_H
#define PC_ALGO_SEAM_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"
#include "raster/interrupt.h"

/* Gradient-magnitude energy (|dx| + |dy| summed over channels, forward and backward differences
 * halved): *out holds w * h values. */
pc_status pc_algo_seam_energy(size_t w, size_t h, size_t ch, const float *img, size_t len, float **out,
                              pc_error *err);

/* The lowest-energy vertical seam: one x per row (*out holds h values). A zero width or a short
 * energy map gives all zeros, as the reference does. */
pc_status pc_algo_seam_find(size_t w, size_t h, const float *energy, size_t len, size_t **out, pc_error *err);

/* Change the width to new_w (max(new_w, 1)) by removing or duplicating vertical seams: *out holds
 * new_w * h * ch floats (*out_len), or a copy of the input when w or h is 0. ctl (may be NULL) is
 * checked before every seam (PC_ERR_CANCELLED) and gets progress per seam. */
pc_status pc_algo_seam_carve_width(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                   const float *protect, size_t new_w, const pc_interrupt *ctl, float **out,
                                   size_t *out_len, pc_error *err);

/* Content-aware resize to new_w x new_h (each at least 1; width first, then height by
 * transposing). An empty input gives an empty output. */
pc_status pc_algo_seam_carve(size_t w, size_t h, size_t ch, const float *img, size_t len,
                             const float *protect, size_t new_w, size_t new_h, const pc_interrupt *ctl,
                             float **out, size_t *out_len, pc_error *err);

/* Plain bilinear resize (the non-content-aware part of the Amount blend): nw * nh * ch floats
 * (zeros when the input is empty). */
pc_status pc_algo_seam_resize_bilinear(size_t w, size_t h, size_t ch, const float *img, size_t len, size_t nw,
                                       size_t nh, float **out, pc_error *err);

/* Skin-tone protection map (1 = skin, w * h values) for RGB(A) buffers: the explicit RGB rule of
 * Kovac, Peer, Solina (EUROCON 2003). All zeros when ch < 3. */
pc_status pc_algo_seam_skin_mask(size_t w, size_t h, size_t ch, const float *img, size_t len, float **out,
                                 pc_error *err);

#endif
