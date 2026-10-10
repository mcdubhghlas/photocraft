/* Procedural (generated, deterministic) texture patterns and sampled tips for the built-in
 * presets (port of crates/paint/src/procedural.rs). Everything is original: seeded value noise
 * and simple geometry, no bundled brush files.
 *
 * Ported with chunk L2 because the stroke renderer (textures) and the built-in presets (tips)
 * need it; chunk L3 only has to port its tests' remaining callers.
 * Pure functions; thread-safe. Failure: PC_ERR_NOMEM, or PC_ERR_LIMIT when n * n (or a count)
 * is too large to allocate; out-params are written only on success. */
#ifndef PC_PAINT_PROCEDURAL_H
#define PC_PAINT_PROCEDURAL_H

#include <stdint.h>

#include "base/status.h"
#include "paint/brush.h"
#include "paint/tile.h"

/* A seamless pattern tile, size clamped to 8..1024. One reference to a new image. */
pc_status pc_paint_pattern_generate(pc_paint_pattern_style style, uint32_t size, uint32_t seed,
                                    pc_paint_pattern_image **out, pc_error *err);

/* Sampled tips, n x n. */
pc_status pc_paint_chalk_tip(uint32_t n, uint32_t seed, pc_paint_gray_tile *out, pc_error *err);
pc_status pc_paint_spatter_tip(uint32_t n, uint32_t seed, uint32_t drops, pc_paint_gray_tile *out,
                               pc_error *err);
pc_status pc_paint_bristle_tip(uint32_t n, uint32_t seed, pc_paint_gray_tile *out, pc_error *err);
pc_status pc_paint_charcoal_tip(uint32_t n, uint32_t seed, pc_paint_gray_tile *out, pc_error *err);
pc_status pc_paint_leaf_tip(uint32_t n, pc_paint_gray_tile *out, pc_error *err);
pc_status pc_paint_grass_tip(uint32_t n, uint32_t seed, uint32_t blades, pc_paint_gray_tile *out,
                             pc_error *err);
pc_status pc_paint_sponge_tip(uint32_t n, uint32_t seed, pc_paint_gray_tile *out, pc_error *err);
pc_status pc_paint_star_tip(uint32_t n, pc_paint_gray_tile *out, pc_error *err);
pc_status pc_paint_rake_tip(uint32_t n, uint32_t seed, uint32_t bristles, pc_paint_gray_tile *out,
                            pc_error *err);

#endif
