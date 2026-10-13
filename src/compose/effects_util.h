/* The public helpers of effects.rs (C3): the pieces of the layer-effect renderer the GPU
 * compositor shares with the CPU maps (kernels, contours, bevel geometry, distance fields) and
 * the small effect formulas tests and tools inspect. The effect maps themselves are in
 * compose/effects.h.
 *
 * Pure functions apart from pc_compose_fx_sample_gradient and the distance fields' allocation;
 * all thread-safe. Allocating functions return PC_ERR_NOMEM / PC_ERR_LIMIT and write their
 * out-params only on success; returned arrays are malloc'd (caller frees with free()). */
#ifndef PC_COMPOSE_EFFECTS_UTIL_H
#define PC_COMPOSE_EFFECTS_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doc/doc.h"

/* Entries of a contour / range table (adjust.rs LUT_SIZE). */
#define PC_COMPOSE_FX_LUT_SIZE 4096 /* == PC_COMPOSE_LUT_SIZE */
/* Height below which an outer bevel pixel counts as off the bevel (beyond the blur's reach). */
#define PC_COMPOSE_BEVEL_H_EPS 1e-5f
/* Alpha above which a pixel belongs to the layer's shape (half an 8-bit step). */
#define PC_COMPOSE_FX_INSIDE_EPS (0.5f / 255.0f)

/* A contour's table (adjust::curve_lut of its points; PC_COMPOSE_FX_LUT_SIZE entries), or
 * *out = NULL for Linear (identity). */
pc_status pc_compose_fx_contour_lut(const pc_contour *c, float **out, pc_error *err);
/* A contour over a Range: contour(min(v / range, 1)); *out = NULL means identity. Tables have
 * PC_COMPOSE_FX_LUT_SIZE entries. */
pc_status pc_compose_fx_ranged_lut(const pc_contour *c, float range, float **out, pc_error *err);
/* Satin's coverage transfer: the contour read from its far end, |c(1 - v) - c(1)| (Photoshop
 * 27.11: Cone and Cone - Inverted Satins are identical); *out = NULL means identity. */
pc_status pc_compose_fx_satin_lut(const pc_contour *c, float **out, pc_error *err);
/* The bevel Contour element's height transfer: the contour alone (its Range is not a v / range
 * stretch); always a table (identity for Linear). */
pc_status pc_compose_fx_bevel_contour_lut(const pc_bevel_contour *c, float **out, pc_error *err);
/* A glow's coverage transfer (its contour over its Range); *out = NULL means identity. */
pc_status pc_compose_fx_glow_lut(const pc_glow *g, float **out, pc_error *err);
/* Interpolated lookup in a 0..=1 table of n >= 1 entries (as the GPU's lut()). */
float pc_compose_fx_lut_at(const float *lut, size_t n, float v);

/* Deterministic per-pixel value in [0, 1) hashed from document coordinates (the GPU's
 * dissolve_noise), so effect noise is stable across renders and tiles. */
float pc_compose_fx_hash_noise(int64_t x, int64_t y);

/* Colour and opacity stops of g at t (straight RGBA; CMYK stops through the thread's active
 * CMYK space). */
void pc_compose_fx_sample_gradient(const pc_gradient *g, float t, float out[4]);

/* Spread / choke of a soft effect: the whole-pixel dilation radius round(size x spread) and the
 * blur width size x (1 - spread). */
void pc_compose_fx_spread_split(float size, float spread, float *radius, float *blur);
/* The opacity gain of a gradient glow: 1 / range^2 (range clamped to 0.01..1, 1 if not finite). */
float pc_compose_fx_glow_gradient_gain(float range);

/* The 1D kernel of two box passes of width w (a tent): radius r and 2r + 1 weights summing to 1.
 * Widths are clamped to 1..PC_COMPOSE_FX_MAX_REACH (NaN = 1, the identity). */
pc_status pc_compose_fx_tent_kernel(float w, int32_t *radius, float **weights, size_t *n, pc_error *err);

/* Where a bevel's maps paint: inside the shape, outside it, or both (emboss styles). */
typedef enum pc_compose_bevel_paint {
    PC_COMPOSE_BEVEL_PAINT_INNER,
    PC_COMPOSE_BEVEL_PAINT_OUTER,
    PC_COMPOSE_BEVEL_PAINT_BOTH
} pc_compose_bevel_paint;

/* Bevel geometry shared by the CPU maps and the GPU programs (effects::BevelGeom). */
typedef struct pc_compose_bevel_geom {
    pc_compose_bevel_paint paint;
    float width;       /* box width of the smooth height blur */
    float depth;       /* height-map gradient scale, signed by direction: a bevel rises
                        * depth x width / 2 px (Smooth: width = the tent; chisel: size / 2) */
    bool pillow;       /* pillow emboss: the outside slopes face the other way */
    float chisel_soft; /* Chisel Soft: extra tent width over the chiselled height map */
} pc_compose_bevel_geom;

pc_compose_bevel_geom pc_compose_fx_bevel_geom(const pc_bevel *b);
/* Chiselled height from the inside / outside distances (soft: Chisel Soft). A facet has the
 * constant slope depth x size / (2 (size + 1)) (Photoshop 27.11); Inner Hard's ramp runs from half
 * a pixel beyond the edge to size + 1/2, Inner Soft starts a pixel further out, Outer Hard ends a
 * pixel further out. A non-finite size (the reference panics on its NaN clamp bound for Outer Hard
 * and Inner Soft) gives a well-defined value: an invalid upper bound counts as 0. */
float pc_compose_fx_bevel_chisel_h(pc_compose_bevel_paint paint, float size, float din, float dout,
                                   bool soft);

/* Coverage of an outside stroke beneath a filled shape's edge pixel the outline covers cov of,
 * where the layer ends at alpha l. */
float pc_compose_fx_outline_share(float cov, float l);
/* (inside width, outside width) of a stroke. */
void pc_compose_fx_stroke_widths(const pc_stroke_fx *st, float *in_w, float *out_w);

/* A distance field of an effect shape, as the map builders compute it (effects::FieldKind). */
typedef enum pc_compose_fx_field_kind {
    PC_COMPOSE_FIELD_OUTSIDE,              /* exact distance outside the shape (precise outer glows) */
    PC_COMPOSE_FIELD_INSIDE,               /* exact distance inside the shape (precise inner glows) */
    PC_COMPOSE_FIELD_BEVEL_OUTSIDE,        /* chiselled bevels: outside, edges placed by coverage */
    PC_COMPOSE_FIELD_BEVEL_INSIDE,         /* chiselled bevels: inside, edges placed by coverage */
    PC_COMPOSE_FIELD_STROKE_OUTSIDE,       /* chamfer distance outside the shape */
    PC_COMPOSE_FIELD_CHOKE_INSIDE,         /* chamfer distance outside 1 - alpha */
    PC_COMPOSE_FIELD_STROKE_INSIDE,        /* chamfer distance inside the shape */
    PC_COMPOSE_FIELD_STROKE_OUTSIDE_VECTOR /* chamfer distance outside the local coverage */
} pc_compose_fx_field_kind;

/* The kind field of alpha (row-major w x h); *out has w * h values (NULL when that is 0). Values
 * past reach pixels from the shape's edge are only meant to be large (the bevel fields skip
 * refining them). PC_ERR_LIMIT past PC_COMPOSE_MAX_PIXELS. */
pc_status pc_compose_fx_distance_field(pc_compose_fx_field_kind kind, const float *alpha, size_t w, size_t h,
                                       float reach, float **out, pc_error *err);

#endif
