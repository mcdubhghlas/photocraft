/* Blending Options: Blend If ranges and Advanced Blending switches (port of
 * crates/doc/src/blend_if.rs and advanced.rs). Conventions: doc/adjust.h. */
#ifndef PC_DOC_BLEND_IF_H
#define PC_DOC_BLEND_IF_H

#include "doc/adjust.h"

/* One Blend If slider pair on 0..=255: black = (low, high), white = (low, high). */
typedef struct pc_blend_range {
    uint8_t black[2];
    uint8_t white[2];
} pc_blend_range;

#define PC_BLEND_RANGE_FULL ((pc_blend_range){{0, 0}, {255, 255}})

bool pc_blend_range_eq(pc_blend_range a, pc_blend_range b);
bool pc_blend_range_is_full(pc_blend_range r);
pc_blend_range pc_blend_range_from_bytes(const uint8_t b[4]); /* PSD order: bl, bh, wl, wh */
void pc_blend_range_to_bytes(pc_blend_range r, uint8_t out[4]);
/* How much of a pixel with value v (0..=255) shows. */
float pc_blend_range_weight(pc_blend_range r, float v);

/* ranges[0] = Gray, then one per colour channel; each [this layer, underlying]. Empty = all
 * pixels blend. */
typedef struct pc_blend_if {
    pc_blend_range (*ranges)[2];
    size_t nranges;
} pc_blend_if;

void pc_blend_if_fini(pc_blend_if *b);
pc_status pc_blend_if_clone(const pc_blend_if *b, pc_blend_if *out, pc_error *err);
bool pc_blend_if_eq(const pc_blend_if *a, const pc_blend_if *b);
bool pc_blend_if_is_default(const pc_blend_if *b);
void pc_blend_if_get(const pc_blend_if *b, size_t i, pc_blend_range out[2]);
/* Sets entry i (growing with full ranges), then drops trailing all-full entries. On failure
 * (NOMEM, or i too large to allocate: PC_ERR_LIMIT) b is unchanged. */
pc_status pc_blend_if_set(pc_blend_if *b, size_t i, const pc_blend_range pair[2], pc_error *err);

typedef enum pc_knockout { PC_KNOCKOUT_NONE, PC_KNOCKOUT_SHALLOW, PC_KNOCKOUT_DEEP } pc_knockout;

uint8_t pc_knockout_to_psd(pc_knockout k);
pc_knockout pc_knockout_from_psd(uint8_t v); /* > 2 reads as deep */
const char *pc_knockout_name(pc_knockout k); /* "none", "shallow", "deep" */
bool pc_knockout_from_name(const char *s, size_t len, pc_knockout *out); /* ASCII case-insensitive */

typedef struct pc_advanced_blending {
    pc_knockout knockout;
    bool blend_interior;
    bool blend_clipped;       /* default on */
    bool transparency_shapes; /* default on */
    bool layer_mask_hides_effects;
    bool vector_mask_hides_effects;
} pc_advanced_blending;

pc_advanced_blending pc_advanced_blending_default(void);
bool pc_advanced_blending_eq(const pc_advanced_blending *a, const pc_advanced_blending *b);
bool pc_advanced_blending_is_default(const pc_advanced_blending *a);

#endif
