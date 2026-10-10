/* Optical kerning (port of crates/text/src/optical.rs; Photoshop's Character panel > Kerning >
 * Optical): pair spacing computed from glyph outlines instead of the font's kerning table.
 *
 * Every glyph gets a horizontal profile: at PC_OPTICAL_SAMPLES heights across the em, the
 * distance from the start of its advance to its leftmost ink and from its rightmost ink to the
 * end of its advance (em units). A pair is described by its closest approach and the mean white
 * space beyond it (capped); the kerning is a linear function of how those differ from the font's
 * own "oo" pair, plus a size term (the reference's coefficients, fitted to Photoshop's output).
 *
 * Pure functions are thread-safe; a pc_optical_cache is single-owner (not synchronised). */
#ifndef PC_TEXT_OPTICAL_H
#define PC_TEXT_OPTICAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "text/font.h"

#define PC_OPTICAL_SAMPLES 48

/* Left/right side distances at each sampled height (em); has_* false where there is no ink. */
typedef struct pc_optical_profile {
    bool has_left[PC_OPTICAL_SAMPLES];
    float left[PC_OPTICAL_SAMPLES];
    bool has_right[PC_OPTICAL_SAMPLES];
    float right[PC_OPTICAL_SAMPLES];
    float advance; /* em */
} pc_optical_profile;

/* Height of sample i (em above the baseline). */
float pc_optical_sample_y(size_t i);
bool pc_optical_profile_is_empty(const pc_optical_profile *p);

/* The profile of glyph gid (unscaled outline, flattened). PC_ERR_NOT_FOUND when the font has no
 * usable units-per-em or no outline for gid (the reference's None), PC_ERR_NOMEM. */
pc_status pc_optical_profile_of(pc_font *font, const int16_t *coords, size_t ncoords, uint32_t gid,
                                pc_optical_profile *out, pc_error *err);

/* Closest approach and mean white space beyond it (capped), em; false when the glyphs share no
 * inked height inside the band. */
bool pc_optical_pair_features(const pc_optical_profile *l, const pc_optical_profile *r, float out[2]);
/* Size term (1/1000 em): 11 * ln(95 / clamp(size, 15, 95)); 0 for non-finite or <= 0 sizes. */
float pc_optical_size_adjust(float size_pt);
/* Kerning (1/1000 em, clamped to +-400) of a pair with features f against the font's reference
 * features; 0 when not finite. */
float pc_optical_kern_from_features(const float f[2], const float reference[2]);

/* Per-font cache of profiles and reference ("oo") features, keyed by font and variation
 * coordinates; cleared when it grows past 50,000 profiles. */
typedef struct pc_optical_cache pc_optical_cache;

pc_status pc_optical_cache_new(pc_optical_cache **out, pc_error *err);
void pc_optical_cache_free(pc_optical_cache *c); /* NULL-safe */
/* Optical kerning of glyphs left, right of one face in 1/1000 em. False when it doesn't apply
 * (spaces, glyphs that share no inked height, unreadable glyphs) or memory runs out. */
bool pc_optical_cache_pair(pc_optical_cache *c, pc_font *font, const int16_t *coords, size_t ncoords,
                           uint32_t left, uint32_t right, float *out);

#endif
