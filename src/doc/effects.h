/* Layer styles (effects), data only (port of crates/doc/src/effects.rs, plus the layer's
 * Effects container from lib.rs). Rendering lives in src/compose, PSD mapping in src/io.
 * Angles are degrees counter-clockwise from 3 o'clock (the direction light comes from); sizes
 * and distances are pixels; opacities and percentages 0..=1. Conventions: doc/adjust.h. */
#ifndef PC_DOC_EFFECTS_H
#define PC_DOC_EFFECTS_H

#include "color/blend.h"
#include "color/color.h"
#include "doc/adjust.h"

typedef enum pc_gradient_style {
    PC_GRADIENT_LINEAR,
    PC_GRADIENT_RADIAL,
    PC_GRADIENT_ANGLE,
    PC_GRADIENT_REFLECTED,
    PC_GRADIENT_DIAMOND
} pc_gradient_style;

/* (location, colour) and (location, opacity) stops, locations 0..=1. */
typedef struct pc_color_stop {
    float pos;
    pc_color color;
} pc_color_stop;

typedef struct pc_opacity_stop {
    float pos;
    float opacity;
} pc_opacity_stop;

typedef struct pc_gradient {
    pc_color_stop *stops;
    size_t nstops;
    pc_opacity_stop *opacity_stops; /* empty = fully opaque */
    size_t nopacity_stops;
    pc_gradient_style style;
    float angle;
    float scale;
    bool reverse;
    bool align;       /* align with the layer bounds (else the canvas) */
    float offset[2];  /* centre offset as a fraction of the bounds */
} pc_gradient;

/* Black -> white, linear, angle 90, scale 1, aligned. */
pc_status pc_gradient_default(pc_gradient *out, pc_error *err);
void pc_gradient_fini(pc_gradient *g);
pc_status pc_gradient_clone(const pc_gradient *g, pc_gradient *out, pc_error *err);
bool pc_gradient_eq(const pc_gradient *a, const pc_gradient *b);

/* Effect contour: Linear (custom == false), or a named custom curve. */
typedef struct pc_contour {
    bool custom;
    pc_str name;
    pc_curve points;
} pc_contour;

void pc_contour_fini(pc_contour *c);
pc_status pc_contour_clone(const pc_contour *c, pc_contour *out, pc_error *err);
bool pc_contour_eq(const pc_contour *a, const pc_contour *b);

typedef enum pc_fx_paint_kind { PC_FX_PAINT_COLOR, PC_FX_PAINT_GRADIENT, PC_FX_PAINT_PATTERN } pc_fx_paint_kind;

/* Paint used by strokes and glows. */
typedef struct pc_fx_paint {
    pc_fx_paint_kind kind;
    union {
        pc_color color;
        pc_gradient gradient;
        struct {
            pc_str name;
            pc_str id;
            float scale;
        } pattern;
    } u;
} pc_fx_paint;

void pc_fx_paint_fini(pc_fx_paint *p);
pc_status pc_fx_paint_clone(const pc_fx_paint *p, pc_fx_paint *out, pc_error *err);
bool pc_fx_paint_eq(const pc_fx_paint *a, const pc_fx_paint *b);

typedef struct pc_fx_common {
    bool enabled;
    pc_blend_mode blend;
    float opacity;
} pc_fx_common;

pc_fx_common pc_fx_common_new(pc_blend_mode blend, float opacity); /* enabled */

typedef struct pc_shadow {
    pc_fx_common common;
    pc_color color;
    float angle;
    bool use_global_light;
    float distance;
    float spread; /* spread (drop) or choke (inner), 0..=1 */
    float size;
    pc_contour contour;
    bool anti_alias;
    float noise;
    bool knocks_out; /* drop shadow only */
} pc_shadow;

typedef enum pc_glow_technique { PC_GLOW_SOFTER, PC_GLOW_PRECISE } pc_glow_technique;
typedef enum pc_glow_source { PC_GLOW_SOURCE_CENTER, PC_GLOW_SOURCE_EDGE } pc_glow_source; /* default Edge */

typedef struct pc_glow {
    pc_fx_common common;
    pc_fx_paint paint;
    pc_glow_technique technique;
    float spread;
    float size;
    pc_contour contour;
    bool anti_alias;
    float range;
    float jitter;
    float noise;
    pc_glow_source source; /* inner glow only */
} pc_glow;

typedef enum pc_stroke_position { PC_STROKE_OUTSIDE, PC_STROKE_INSIDE, PC_STROKE_CENTER } pc_stroke_position;

typedef struct pc_stroke_fx {
    pc_fx_common common;
    float size;
    pc_stroke_position position;
    pc_fx_paint paint;
} pc_stroke_fx;

typedef struct pc_satin {
    pc_fx_common common;
    pc_color color;
    float angle;
    float distance;
    float size;
    pc_contour contour;
    bool anti_alias;
    bool invert;
} pc_satin;

typedef enum pc_bevel_style {
    PC_BEVEL_OUTER,
    PC_BEVEL_INNER, /* default */
    PC_BEVEL_EMBOSS,
    PC_BEVEL_PILLOW_EMBOSS,
    PC_BEVEL_STROKE_EMBOSS
} pc_bevel_style;

typedef enum pc_bevel_technique { PC_BEVEL_SMOOTH, PC_BEVEL_CHISEL_HARD, PC_BEVEL_CHISEL_SOFT } pc_bevel_technique;

/* Bevel & Emboss > Contour. Default: Linear, range 0.5, no anti-alias. */
typedef struct pc_bevel_contour {
    pc_contour contour;
    float range;
    bool anti_alias;
} pc_bevel_contour;

pc_bevel_contour pc_bevel_contour_default(void);

/* Bevel & Emboss > Texture. */
typedef struct pc_bevel_texture {
    pc_str name;
    pc_str id;
    float scale;
    float depth;
    bool invert;
    bool link;
    float phase[2];
} pc_bevel_texture;

typedef struct pc_bevel {
    bool enabled;
    pc_bevel_style style;
    pc_bevel_technique technique;
    float depth; /* 0..=10, 1 = 100 % */
    bool up;
    float size;
    float soften;
    float angle;
    float altitude;
    bool use_global_light;
    pc_contour gloss_contour;
    pc_fx_common highlight;
    pc_color highlight_color;
    pc_fx_common shadow;
    pc_color shadow_color;
    bool has_contour;
    pc_bevel_contour contour;
    bool has_texture;
    pc_bevel_texture texture;
} pc_bevel;

typedef enum pc_effect_kind {
    PC_FX_DROP_SHADOW,
    PC_FX_INNER_SHADOW,
    PC_FX_OUTER_GLOW,
    PC_FX_INNER_GLOW,
    PC_FX_STROKE,
    PC_FX_COLOR_OVERLAY,
    PC_FX_GRADIENT_OVERLAY,
    PC_FX_PATTERN_OVERLAY,
    PC_FX_SATIN,
    PC_FX_BEVEL_EMBOSS
} pc_effect_kind;
#define PC_FX_KIND_COUNT 10

/* One layer effect; several of one kind are allowed. */
typedef struct pc_effect {
    pc_effect_kind kind;
    union {
        pc_shadow shadow; /* DROP_SHADOW, INNER_SHADOW */
        pc_glow glow;     /* OUTER_GLOW, INNER_GLOW */
        pc_stroke_fx stroke;
        struct {
            pc_fx_common common;
            pc_color color;
        } color_overlay;
        struct {
            pc_fx_common common;
            pc_gradient gradient;
            bool dither;
        } gradient_overlay;
        struct {
            pc_fx_common common;
            pc_str name;
            pc_str id;
            float scale;
            float angle;
            bool link; /* serde default true */
            float phase[2];
        } pattern_overlay;
        pc_satin satin;
        pc_bevel bevel;
    } u;
} pc_effect;

void pc_effect_fini(pc_effect *e);
pc_status pc_effect_clone(const pc_effect *e, pc_effect *out, pc_error *err);
bool pc_effect_eq(const pc_effect *a, const pc_effect *b);
bool pc_effect_enabled(const pc_effect *e);
const char *pc_effect_label(const pc_effect *e); /* "Drop Shadow", ... */
void pc_effect_default_drop_shadow(pc_effect *out);

/* A layer's styles (lib.rs Effects). */
typedef struct pc_effects {
    bool enabled;
    pc_effect *items;
    size_t nitems;
    pc_blob *psd_raw;      /* original lfx2/lfxs block data; NULL = None */
    bool has_reference;    /* effects reference point (PSD fxrp); None = the layer's top-left */
    double reference[2];
} pc_effects;

void pc_effects_fini(pc_effects *e);
pc_status pc_effects_clone(const pc_effects *e, pc_effects *out, pc_error *err);
bool pc_effects_eq(const pc_effects *a, const pc_effects *b);

/* Document-wide light. Default angle 120, altitude 30. */
typedef struct pc_global_light {
    float angle;
    float altitude;
} pc_global_light;

pc_global_light pc_global_light_default(void);

#endif
