/* The brush model and stroke data (port of crates/paint/src/brush.rs and lib.rs's data types).
 *
 * A brush is a tip shape plus Photoshop-style dynamics sections, all plain data. Every section is
 * optional (an `enabled` flag or a neutral default). Units: sizes are pixels, angles degrees,
 * every percentage a 0..1 fraction (scatter and texture scale may exceed 1).
 *
 * Ownership: the only owned parts are gray tiles (sampled tips, texture tiles), which are shared
 * immutable blocks (tile.h). So copying a brush (pc_paint_brush_copy) shares them and cannot
 * fail; pc_paint_brush_fini releases them. Structs are initialised by their *_default function
 * (which allocates nothing) or by a copy; assigning one section over another must go through the
 * *_assign helpers so references stay balanced.
 *
 * Equality (*_eq) is the reference's PartialEq: float fields compare with ==, so NaN != NaN and
 * 0.0 == -0.0. Thread safety: plain data; concurrent const use is fine. */
#ifndef PC_PAINT_BRUSH_H
#define PC_PAINT_BRUSH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "base/str.h"
#include "color/blend.h"
#include "geom/geom.h"
#include "paint/tile.h"

/* Largest brush diameter accepted by the rasterizer and brush controls. */
#define PC_PAINT_MAX_BRUSH_SIZE 5000.0f
/* Largest Scatter amount (primary and Dual Brush): 10 = 1000 %, the slider's top. */
#define PC_PAINT_MAX_SCATTER 10.0f

/* What drives a dynamic parameter (Photoshop's "Control" pop-ups). */
typedef enum pc_paint_control {
    PC_PAINT_CONTROL_OFF,
    PC_PAINT_CONTROL_FADE, /* ramp over fade_steps spacing steps (full -> minimum) */
    PC_PAINT_CONTROL_PEN_PRESSURE,
    PC_PAINT_CONTROL_PEN_TILT,
    PC_PAINT_CONTROL_STYLUS_WHEEL,
    PC_PAINT_CONTROL_ROTATION,          /* barrel rotation */
    PC_PAINT_CONTROL_INITIAL_DIRECTION, /* angle only: direction of the first segment */
    PC_PAINT_CONTROL_DIRECTION          /* angle only: current stroke direction */
} pc_paint_control;
#define PC_PAINT_CONTROL_COUNT 8

/* One dynamic parameter: random jitter plus a controller with a floor. */
typedef struct pc_paint_dynamic {
    float jitter; /* 0..1 per dab (for scatter: the amount, may exceed 1) */
    pc_paint_control control;
    uint32_t fade_steps;
    float minimum; /* 0..1 floor */
} pc_paint_dynamic;

pc_paint_dynamic pc_paint_dynamic_default(void); /* {0, Off, 25, 0} */
pc_paint_dynamic pc_paint_dynamic_controlled(pc_paint_control c);
pc_paint_dynamic pc_paint_dynamic_jitter(float jitter);
bool pc_paint_dynamic_is_active(const pc_paint_dynamic *d);
bool pc_paint_dynamic_eq(const pc_paint_dynamic *a, const pc_paint_dynamic *b);

/* The brush tip bitmap source. */
typedef enum pc_paint_tip_kind { PC_PAINT_TIP_ROUND, PC_PAINT_TIP_SAMPLED } pc_paint_tip_kind;
typedef struct pc_paint_tip {
    pc_paint_tip_kind kind;
    pc_paint_gray_tile tile; /* SAMPLED: 1 = full paint, scaled so its larger side = size */
} pc_paint_tip;

typedef struct pc_paint_shape_dynamics {
    bool enabled;
    pc_paint_dynamic size;      /* minimum = Minimum Diameter */
    float tilt_scale;           /* 0..2: with size on Pen Tilt, squashes the tip height */
    pc_paint_dynamic angle;     /* jitter = fraction of +-180 degrees */
    pc_paint_dynamic roundness; /* minimum = Minimum Roundness */
    bool flip_x_jitter;
    bool flip_y_jitter;
    bool brush_projection; /* pen tilt and rotation applied to the tip as a projection */
} pc_paint_shape_dynamics;

typedef struct pc_paint_scattering {
    bool enabled;
    pc_paint_dynamic scatter; /* jitter = amount (1 = up to one radius) */
    bool both_axes;
    uint32_t count; /* dabs per spacing step, 1..16 */
    pc_paint_dynamic count_jitter;
} pc_paint_scattering;

/* How a grayscale mask (texture or dual tip) combines with the brush coverage. */
typedef enum pc_paint_mask_mode {
    PC_PAINT_MASK_MULTIPLY,
    PC_PAINT_MASK_SUBTRACT,
    PC_PAINT_MASK_DARKEN,
    PC_PAINT_MASK_OVERLAY,
    PC_PAINT_MASK_COLOR_DODGE,
    PC_PAINT_MASK_COLOR_BURN,
    PC_PAINT_MASK_LINEAR_BURN,
    PC_PAINT_MASK_HARD_MIX,
    PC_PAINT_MASK_LINEAR_HEIGHT,
    PC_PAINT_MASK_HEIGHT
} pc_paint_mask_mode;
#define PC_PAINT_MASK_MODE_COUNT 10

/* Procedural pattern styles (generated deterministically). */
typedef enum pc_paint_pattern_style {
    PC_PAINT_STYLE_NOISE,
    PC_PAINT_STYLE_CANVAS,
    PC_PAINT_STYLE_PAPER,
    PC_PAINT_STYLE_DOTS
} pc_paint_pattern_style;
#define PC_PAINT_PATTERN_STYLE_COUNT 4

/* A texture pattern: a procedural generator or an explicit grayscale tile (tiled infinitely). */
typedef enum pc_paint_pattern_kind {
    PC_PAINT_PATTERN_PROCEDURAL,
    PC_PAINT_PATTERN_TILE
} pc_paint_pattern_kind;
typedef struct pc_paint_pattern {
    pc_paint_pattern_kind kind;
    pc_paint_pattern_style style; /* PROCEDURAL */
    uint32_t size;                /* PROCEDURAL */
    uint32_t seed;                /* PROCEDURAL */
    pc_paint_gray_tile tile;      /* TILE */
} pc_paint_pattern;

typedef struct pc_paint_texture {
    bool enabled;
    pc_paint_pattern pattern;
    bool invert;
    float scale;      /* 1 = 100 % */
    float brightness; /* -1..1 */
    float contrast;   /* -1..1 */
    bool each_tip;    /* texture every dab (enables depth jitter) instead of the stroke */
    pc_paint_mask_mode mode;
    float depth; /* 0..1 */
    pc_paint_dynamic depth_jitter;
} pc_paint_texture;

typedef struct pc_paint_dual_brush {
    bool enabled;
    pc_paint_mask_mode mode;
    pc_paint_tip tip;
    float size;
    float hardness;
    float roundness;
    float angle;
    float spacing;
    float scatter;
    bool both_axes;
    uint32_t count;
    bool flip;
} pc_paint_dual_brush;

typedef struct pc_paint_color_dynamics {
    bool enabled;
    bool per_tip; /* vary every dab (true) or once per stroke */
    pc_paint_dynamic fg_bg;
    float hue_jitter; /* fraction of +-half the hue wheel */
    float saturation_jitter;
    float brightness_jitter;
    float purity; /* -1..1 */
} pc_paint_color_dynamics;

typedef struct pc_paint_transfer {
    bool enabled;
    pc_paint_dynamic opacity;
    pc_paint_dynamic flow;
    pc_paint_dynamic wetness; /* Mixer Brush: scales Wet per dab */
    pc_paint_dynamic mix;     /* Mixer Brush: scales Mix per dab */
} pc_paint_transfer;

/* Sections whose settings stay when another preset is picked. */
typedef struct pc_paint_section_locks {
    bool shape_dynamics, scattering, texture, dual_brush, color_dynamics, transfer, pose, noise, wet_edges,
        build_up, smoothing, protect_texture;
} pc_paint_section_locks;

/* Brush Pose: stylus values used instead of (override) device data. */
typedef struct pc_paint_pose {
    bool enabled;
    float tilt_x, tilt_y; /* degrees -90..90 */
    float rotation;       /* degrees 0..360 */
    float pressure;       /* 0..1 */
    bool override_tilt, override_rotation, override_pressure;
} pc_paint_pose;

typedef struct pc_paint_smoothing {
    float amount;       /* 0..1 */
    bool pulled_string; /* lazy mouse: paint only when a string of amount * 100 px is taut */
    bool catch_up;      /* keep catching up while the pointer pauses (timestamps) */
    bool catch_up_on_end;
    bool adjust_for_zoom; /* divide the string length by the view zoom */
} pc_paint_smoothing;

/* Mixer Brush options (all 0..1); used by the Mixer Brush (src/paint mixer, chunk L3). */
typedef struct pc_paint_mixer_settings {
    float wet, load, mix, flow;
    bool sample_all_layers;
} pc_paint_mixer_settings;

/* A complete brush. */
typedef struct pc_paint_brush {
    float size;           /* diameter, pixels */
    float hardness;       /* 0..1, computed tips only */
    float spacing;        /* fraction of the diameter */
    bool spacing_enabled; /* off: pointer speed sets the spacing */
    float opacity;        /* stroke maximum */
    float flow;           /* per dab */
    bool pressure_size;
    bool pressure_opacity;
    float color[4];      /* straight RGBA */
    float background[4]; /* for fg/bg jitter */
    bool erase;
    pc_blend_mode mode;
    pc_paint_tip tip;
    float angle; /* degrees, counter-clockwise */
    float roundness;
    bool flip_x, flip_y;
    bool aliased; /* Pencil: coverage is 0 or 1 */
    pc_paint_shape_dynamics shape_dynamics;
    pc_paint_scattering scattering;
    pc_paint_texture texture;
    pc_paint_dual_brush dual_brush;
    pc_paint_color_dynamics color_dynamics;
    pc_paint_transfer transfer;
    pc_paint_pose pose;
    bool noise;
    bool wet_edges;
    bool build_up;
    float build_up_rate; /* dabs per second */
    pc_paint_smoothing smoothing;
    bool protect_texture;
    pc_paint_section_locks locks;
    pc_paint_mixer_settings mixer;
    uint64_t seed;
} pc_paint_brush;

/* ---- defaults (allocate nothing) ---- */
void pc_paint_tip_default(pc_paint_tip *t);
void pc_paint_shape_dynamics_default(pc_paint_shape_dynamics *s);
void pc_paint_scattering_default(pc_paint_scattering *s);
void pc_paint_pattern_default(pc_paint_pattern *p); /* Procedural { Paper, 128, 1 } */
void pc_paint_texture_default(pc_paint_texture *t);
void pc_paint_dual_brush_default(pc_paint_dual_brush *d);
void pc_paint_color_dynamics_default(pc_paint_color_dynamics *c);
void pc_paint_transfer_default(pc_paint_transfer *t);
void pc_paint_section_locks_default(pc_paint_section_locks *l);
void pc_paint_pose_default(pc_paint_pose *p);
void pc_paint_smoothing_default(pc_paint_smoothing *s);
void pc_paint_mixer_default(pc_paint_mixer_settings *m);
void pc_paint_brush_default(pc_paint_brush *b);

/* ---- copies (share tiles; never fail), finalisers, assignment ---- */
void pc_paint_tip_copy(pc_paint_tip *dst, const pc_paint_tip *src);
void pc_paint_tip_fini(pc_paint_tip *t);
void pc_paint_pattern_copy(pc_paint_pattern *dst, const pc_paint_pattern *src);
void pc_paint_pattern_fini(pc_paint_pattern *p);
void pc_paint_texture_copy(pc_paint_texture *dst, const pc_paint_texture *src);
void pc_paint_texture_fini(pc_paint_texture *t);
void pc_paint_dual_brush_copy(pc_paint_dual_brush *dst, const pc_paint_dual_brush *src);
void pc_paint_dual_brush_fini(pc_paint_dual_brush *d);
void pc_paint_brush_copy(pc_paint_brush *dst, const pc_paint_brush *src);
void pc_paint_brush_fini(pc_paint_brush *b); /* NULL-safe */
/* Replace an initialised *dst with a copy of src (src may alias dst). */
void pc_paint_tip_assign(pc_paint_tip *dst, const pc_paint_tip *src);
void pc_paint_pattern_assign(pc_paint_pattern *dst, const pc_paint_pattern *src);
void pc_paint_texture_assign(pc_paint_texture *dst, const pc_paint_texture *src);
void pc_paint_dual_brush_assign(pc_paint_dual_brush *dst, const pc_paint_dual_brush *src);
void pc_paint_brush_assign(pc_paint_brush *dst, const pc_paint_brush *src);

/* ---- equality (PartialEq) ---- */
bool pc_paint_tip_eq(const pc_paint_tip *a, const pc_paint_tip *b);
bool pc_paint_shape_dynamics_eq(const pc_paint_shape_dynamics *a, const pc_paint_shape_dynamics *b);
bool pc_paint_scattering_eq(const pc_paint_scattering *a, const pc_paint_scattering *b);
bool pc_paint_pattern_eq(const pc_paint_pattern *a, const pc_paint_pattern *b);
bool pc_paint_texture_eq(const pc_paint_texture *a, const pc_paint_texture *b);
bool pc_paint_dual_brush_eq(const pc_paint_dual_brush *a, const pc_paint_dual_brush *b);
bool pc_paint_color_dynamics_eq(const pc_paint_color_dynamics *a, const pc_paint_color_dynamics *b);
bool pc_paint_transfer_eq(const pc_paint_transfer *a, const pc_paint_transfer *b);
bool pc_paint_section_locks_eq(const pc_paint_section_locks *a, const pc_paint_section_locks *b);
bool pc_paint_pose_eq(const pc_paint_pose *a, const pc_paint_pose *b);
bool pc_paint_smoothing_eq(const pc_paint_smoothing *a, const pc_paint_smoothing *b);
bool pc_paint_mixer_eq(const pc_paint_mixer_settings *a, const pc_paint_mixer_settings *b);
bool pc_paint_brush_eq(const pc_paint_brush *a, const pc_paint_brush *b);

/* ---- brush operations ---- */

/* *out = a copy of b with the primary and dual diameters and Scatter amounts constrained for
 * rasterization (size: finite -> clamp 0.5..MAX_BRUSH_SIZE, else 0.5; scatter: NaN -> 0, else
 * clamp 0..MAX_SCATTER). out is overwritten, not finalised; out may not alias b. */
void pc_paint_brush_bounded(const pc_paint_brush *b, pc_paint_brush *out);
/* b (a preset) picked while `current` is the tool's brush: Smoothing stays the tool's, a
 * protected texture stays, and so does every section `current` has locked (the locks carry
 * over). Modifies b in place. */
void pc_paint_brush_picked_over(pc_paint_brush *b, const pc_paint_brush *current);
/* Protect Texture: if `current` protects its texture, a textured b keeps current's pattern and
 * scale (and protects it too). In place. */
void pc_paint_brush_with_protected_texture(pc_paint_brush *b, const pc_paint_brush *current);

/* ---- stroke data (lib.rs) ---- */

/* One input sample. Missing stylus data defaults to "mouse": full pressure, no tilt. */
typedef struct pc_paint_point {
    double x, y;
    float pressure; /* 0..1, 1 for mouse */
    float tilt_x;   /* degrees -90..90 (W3C tiltX/tiltY) */
    float tilt_y;
    float rotation; /* barrel rotation, degrees 0..360 */
    float wheel;    /* airbrush wheel 0..1 */
    double time;    /* milliseconds */
} pc_paint_point;

pc_paint_point pc_paint_point_default(void);
pc_paint_point pc_paint_point_make(double x, double y, float pressure);
bool pc_paint_point_eq(const pc_paint_point *a, const pc_paint_point *b);

/* A recorded stroke: a brush and its input points (owned). */
typedef struct pc_paint_stroke {
    pc_paint_brush brush;
    pc_paint_point *points; /* malloc'd; NULL when count is 0 */
    size_t count;
} pc_paint_stroke;

void pc_paint_stroke_fini(pc_paint_stroke *s); /* NULL-safe */
bool pc_paint_stroke_eq(const pc_paint_stroke *a, const pc_paint_stroke *b);

/* A dab placed along the stroke (after dynamics). */
typedef struct pc_paint_dab {
    pc_point center;
    float radius;
    float alpha; /* flow: per-dab build-up amount */
    float angle; /* radians, counter-clockwise */
    float roundness;
    bool flip_x, flip_y;
    float opacity;    /* ceiling this dab builds up to */
    float color[4];   /* straight RGBA paint colour */
    float depth;      /* texture depth for per-tip texturing */
    uint64_t index;   /* position in the stroke */
    float proj_angle; /* Brush Projection direction (radians, CCW, y up) */
    float proj_scale; /* foreshortening factor, 1 = none */
    float wet, mix;   /* Mixer Brush multipliers */
} pc_paint_dab;

/* A plain round dab. */
pc_paint_dab pc_paint_dab_round(pc_point center, float radius, float alpha);
bool pc_paint_dab_eq(const pc_paint_dab *a, const pc_paint_dab *b);

/* Growable arrays used for dab and point output. Zero-initialised = empty. */
typedef struct pc_paint_dab_vec {
    pc_paint_dab *v;
    size_t len, cap;
} pc_paint_dab_vec;
typedef struct pc_paint_point_vec {
    pc_paint_point *v;
    size_t len, cap;
} pc_paint_point_vec;

pc_status pc_paint_dab_vec_push(pc_paint_dab_vec *a, const pc_paint_dab *d, pc_error *err);
void pc_paint_dab_vec_fini(pc_paint_dab_vec *a);
pc_status pc_paint_point_vec_push(pc_paint_point_vec *a, const pc_paint_point *p, pc_error *err);
void pc_paint_point_vec_fini(pc_paint_point_vec *a);

/* Coverage of a round dab at distance d from its centre (anti-aliased edge, hardness falloff). */
float pc_paint_dab_coverage(float d, float radius, float hardness);

/* Exponential moving-average smoothing (the simplified lib.rs `smooth`); appends n points to
 * *out (on failure *out keeps what it had). */
pc_status pc_paint_smooth(const pc_paint_point *pts, size_t n, float amount, pc_paint_point_vec *out,
                          pc_error *err);

#endif
