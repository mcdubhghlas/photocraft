/* Photoshop blend modes, scalar reference implementation (port of crates/color/src/blend.rs).
 *
 * Formulas follow ISO 32000-2 and W3C Compositing, with Photoshop's variants where they differ
 * (Soft Light, Hard Mix, Darker/Lighter Color, Linear Burn/Dodge, Vivid/Linear/Pin Light,
 * Subtract, Divide). This is the CPU reference the GPU kernels must match, bit for bit with
 * the Rust reference. Pure functions; thread-safe. */
#ifndef PC_COLOR_BLEND_H
#define PC_COLOR_BLEND_H

#include <stdbool.h>
#include <stddef.h>

typedef enum pc_blend_mode {
    PC_BLEND_PASS_THROUGH, /* groups only: children blend directly into the group's backdrop */
    PC_BLEND_NORMAL,
    PC_BLEND_DISSOLVE,
    PC_BLEND_DARKEN,
    PC_BLEND_MULTIPLY,
    PC_BLEND_COLOR_BURN,
    PC_BLEND_LINEAR_BURN,
    PC_BLEND_DARKER_COLOR,
    PC_BLEND_LIGHTEN,
    PC_BLEND_SCREEN,
    PC_BLEND_COLOR_DODGE,
    PC_BLEND_LINEAR_DODGE,
    PC_BLEND_LIGHTER_COLOR,
    PC_BLEND_OVERLAY,
    PC_BLEND_SOFT_LIGHT,
    PC_BLEND_HARD_LIGHT,
    PC_BLEND_VIVID_LIGHT,
    PC_BLEND_LINEAR_LIGHT,
    PC_BLEND_PIN_LIGHT,
    PC_BLEND_HARD_MIX,
    PC_BLEND_DIFFERENCE,
    PC_BLEND_EXCLUSION,
    PC_BLEND_SUBTRACT,
    PC_BLEND_DIVIDE,
    PC_BLEND_HUE,
    PC_BLEND_SATURATION,
    PC_BLEND_COLOR,
    PC_BLEND_LUMINOSITY
} pc_blend_mode;
#define PC_BLEND_COUNT 28

/* All modes except Pass Through, in Photoshop menu order (= enum order from PC_BLEND_NORMAL). */
#define PC_BLEND_LAYER_MODE_COUNT 27
extern const pc_blend_mode PC_BLEND_LAYER_MODES[PC_BLEND_LAYER_MODE_COUNT];

const char *pc_blend_label(pc_blend_mode m); /* "Linear Dodge (Add)", ... */
const char *pc_blend_name(pc_blend_mode m);  /* serde variant name: "LinearDodge", ... */
bool pc_blend_from_name(const char *s, size_t len, pc_blend_mode *out);
/* PSD 4-character blend key ("norm", "mul ", ...), not NUL-terminated. */
const char *pc_blend_psd_key(pc_blend_mode m);
bool pc_blend_from_psd_key(const char key[4], pc_blend_mode *out);
bool pc_blend_is_separable(pc_blend_mode m);

/* Separable per-channel B(cb, cs). Non-separable modes return cs (use pc_blend_rgb). */
float pc_blend_channel(pc_blend_mode m, float cb, float cs);
/* For 32-bit float documents: Linear Dodge (Add) and Divide clip at FLT_MAX instead of 1, so
 * light adds up past 1 and stays finite. Every other mode equals pc_blend_channel. */
float pc_blend_channel_hdr(pc_blend_mode m, float cb, float cs);

/* W3C Lum (0.3, 0.59, 0.11). */
float pc_blend_lum(const float c[3]);
/* `c` with its luminosity moved to `l`, clipped back towards grey (SetLum + ClipColor). */
void pc_blend_set_lum(const float c[3], float l, float out[3]);

/* B(Cb, Cs) for a whole RGB triple, non-separable modes included. `out` may alias. */
void pc_blend_rgb(pc_blend_mode m, const float cb[3], const float cs[3], float out[3]);
/* pc_blend_rgb for 32-bit float documents (separable modes use pc_blend_channel_hdr). */
void pc_blend_rgb_hdr(pc_blend_mode m, const float cb[3], const float cs[3], float out[3]);

/* Straight-alpha source over straight-alpha backdrop with mode and opacity (W3C/PDF general
 * formula). Dissolve must be resolved by the caller first. `out` may alias either input. */
void pc_blend_composite(pc_blend_mode m, const float backdrop[4], const float source[4], float opacity,
                        float out[4]);

#endif
