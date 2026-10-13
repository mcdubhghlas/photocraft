/* The Filter menu's distort, stylize, pixelate, render, blur gallery, other and video filters
 * (port of crates/algo/src: other.rs, stylize.rs, stylize2.rs, distort.rs, distort2.rs,
 * pixelate.rs, oil.rs, render.rs, gallery.rs, other2.rs, video.rs). Filter Gallery is in
 * artistic.h.
 *
 * Every filter is one FilterParams variant: a typed parameter struct below and a
 * pc_algo_filter_def (registered in filters_l6.c), so callers can build a filter without JSON:
 *
 *     pc_algo_twirl_params p = {90.0f};
 *     pc_algo_filter f;
 *     pc_algo_filter_make(&pc_algo_def_twirl, &p, &f, err);
 *
 * Unit variants (Find Edges, Solarize, Invert, Desaturate, Facet, Fragment, NTSC Colors) take
 * NULL parameters. Struct members follow the reference's field order; owned members (points,
 * floats, lights, pins, paths) are released by pc_algo_filter_fini.
 *
 * Kernels never read outside their source image, never fail on hostile numbers (NaN, inf) and
 * report PC_ERR_LIMIT where the reference would loop for hours or exhaust memory.
 * Thread-safety: everything here is immutable or pure. */
#ifndef PC_ALGO_FX_H
#define PC_ALGO_FX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "algo/filter.h"
#include "algo/image.h"
#include "algo/params.h"

/* ---- parameter structs (FilterParams variants, Photoshop's dialog units) ---- */

/* minimum / maximum: radius px; preserve defaults to Squareness for older params. */
typedef struct pc_algo_minmax_params {
    float radius;
    pc_algo_preserve preserve;
} pc_algo_minmax_params;

typedef struct pc_algo_offset_params {
    int32_t horizontal, vertical;
    pc_algo_undefined_areas undefined;
} pc_algo_offset_params;

typedef struct pc_algo_mosaic_params {
    float cell_size;
} pc_algo_mosaic_params;

/* Angle degrees, height px, amount %. */
typedef struct pc_algo_emboss_params {
    float angle, height, amount;
} pc_algo_emboss_params;

typedef struct pc_algo_twirl_params {
    float angle; /* -999..999 degrees */
} pc_algo_twirl_params;

typedef struct pc_algo_pinch_params {
    float amount; /* -100..100 % */
} pc_algo_pinch_params;

typedef struct pc_algo_spherize_params {
    float amount;
    pc_algo_spherize_mode mode;
} pc_algo_spherize_params;

typedef struct pc_algo_wave_params {
    uint32_t generators;
    float wavelength_min, wavelength_max, amplitude_min, amplitude_max;
    pc_algo_wave_type wave_type;
    pc_algo_undefined_areas undefined;
    uint32_t seed;
} pc_algo_wave_params;

typedef struct pc_algo_ripple_params {
    float amount;
    pc_algo_ripple_size size;
} pc_algo_ripple_params;

typedef struct pc_algo_polar_params {
    pc_algo_polar_mode mode;
} pc_algo_polar_params;

/* Max dot radius px; screen angles per channel in degrees. */
typedef struct pc_algo_halftone_params {
    float max_radius;
    float angles[4];
} pc_algo_halftone_params;

typedef struct pc_algo_crystallize_params {
    float cell_size;
    uint32_t seed;
} pc_algo_crystallize_params;

typedef struct pc_algo_mezzotint_params {
    pc_algo_mezzotint_type kind;
    uint32_t seed;
} pc_algo_mezzotint_params;

/* background: straight sRGB RGBA. */
typedef struct pc_algo_pointillize_params {
    float cell_size;
    uint32_t seed;
    float background[4];
} pc_algo_pointillize_params;

typedef struct pc_algo_diffuse_params {
    pc_algo_diffuse_mode mode;
    uint32_t seed;
} pc_algo_diffuse_params;

typedef struct pc_algo_extrude_params {
    pc_algo_extrude_type kind;
    float size, depth;
    bool level_based, solid_front, mask_incomplete;
    uint32_t seed;
} pc_algo_extrude_params;

typedef struct pc_algo_oil_params {
    float stylization, cleanliness, scale, bristle_detail;
    bool lighting;
    float angle, shine;
} pc_algo_oil_params;

typedef struct pc_algo_tiles_params {
    uint32_t count;
    float max_offset;
    pc_algo_tile_fill fill;
    float foreground[4], background[4];
    uint32_t seed;
} pc_algo_tiles_params;

typedef struct pc_algo_trace_params {
    float level;
    bool upper;
} pc_algo_trace_params;

typedef struct pc_algo_wind_params {
    pc_algo_wind_method method;
    bool from_right;
    uint32_t seed;
} pc_algo_wind_params;

/* `map` is caller data (#[serde(skip)]): borrowed, kept alive by the caller while the filter
 * exists; NULL or empty = identity. Copies of the filter share the pointer. */
typedef struct pc_algo_displace_params {
    float horizontal, vertical;
    bool stretch;
    pc_algo_undefined_areas undefined;
    const pc_algo_image *map;
} pc_algo_displace_params;

typedef struct pc_algo_shear_params {
    pc_algo_points points;
    pc_algo_undefined_areas undefined;
} pc_algo_shear_params;

typedef struct pc_algo_zigzag_params {
    float amount, ridges;
    pc_algo_zigzag_style style;
} pc_algo_zigzag_params;

typedef struct pc_algo_fibers_params {
    float variance, strength;
    uint32_t seed;
    float foreground[4], background[4];
} pc_algo_fibers_params;

typedef struct pc_algo_flare_params {
    float brightness, center_x, center_y;
    pc_algo_lens_type lens;
} pc_algo_flare_params;

typedef struct pc_algo_lighting_params {
    pc_algo_lights lights;
    float gloss, metallic, exposure, ambience;
    pc_algo_texture_channel texture;
    float height;
    bool white_is_high;
} pc_algo_lighting_params;

typedef struct pc_algo_tilt_shift_params {
    float blur, center_x, center_y, angle, focus, transition;
} pc_algo_tilt_shift_params;

typedef struct pc_algo_iris_params {
    pc_algo_iris_pins pins;
} pc_algo_iris_params;

typedef struct pc_algo_field_params {
    pc_algo_field_pins pins;
} pc_algo_field_params;

typedef struct pc_algo_spin_params {
    pc_algo_spin_pins pins;
} pc_algo_spin_params;

typedef struct pc_algo_path_params {
    pc_algo_blur_paths paths;
} pc_algo_path_params;

/* 5x5 kernel (row-major, centre = index 12), divided by scale, plus offset levels. */
typedef struct pc_algo_custom_params {
    pc_algo_floats kernel;
    float scale, offset;
} pc_algo_custom_params;

typedef struct pc_algo_hsb_params {
    pc_algo_hsb_model input, output;
} pc_algo_hsb_params;

/* Color to Alpha: color is straight sRGB RGBA (alpha ignored); thresholds 0..1. */
typedef struct pc_algo_color_to_alpha_params {
    float color[4];
    float transparency_threshold, opacity_threshold;
} pc_algo_color_to_alpha_params;

typedef struct pc_algo_deinterlace_params {
    bool eliminate_even, interpolate;
} pc_algo_deinterlace_params;

/* ---- filter defs (registered in filters_l6.c) ---- */

extern const pc_algo_filter_def pc_algo_def_minimum, pc_algo_def_maximum, pc_algo_def_offset;
extern const pc_algo_filter_def pc_algo_def_mosaic, pc_algo_def_emboss, pc_algo_def_find_edges,
    pc_algo_def_solarize, pc_algo_def_invert, pc_algo_def_desaturate;
extern const pc_algo_filter_def pc_algo_def_twirl, pc_algo_def_pinch, pc_algo_def_spherize, pc_algo_def_wave,
    pc_algo_def_ripple, pc_algo_def_polar;
extern const pc_algo_filter_def pc_algo_def_displace, pc_algo_def_shear, pc_algo_def_zigzag;
extern const pc_algo_filter_def pc_algo_def_color_halftone, pc_algo_def_crystallize, pc_algo_def_facet,
    pc_algo_def_fragment, pc_algo_def_mezzotint, pc_algo_def_pointillize;
extern const pc_algo_filter_def pc_algo_def_diffuse, pc_algo_def_extrude, pc_algo_def_tiles,
    pc_algo_def_trace_contour, pc_algo_def_wind;
extern const pc_algo_filter_def pc_algo_def_oil_paint;
extern const pc_algo_filter_def pc_algo_def_fibers, pc_algo_def_lens_flare, pc_algo_def_lighting;
extern const pc_algo_filter_def pc_algo_def_tilt_shift, pc_algo_def_iris_blur, pc_algo_def_field_blur,
    pc_algo_def_spin_blur, pc_algo_def_path_blur;
extern const pc_algo_filter_def pc_algo_def_custom, pc_algo_def_hsb_hsl, pc_algo_def_color_to_alpha;

/* Test hook: true makes Extrude, Wind, Facet and Color Halftone skip their (bit-identical) fast
 * paths and run the direct per-pixel searches, so tests can compare the two. Default false. */
void pc_algo_fx_set_direct_only(bool on);
bool pc_algo_fx_direct_only(void);
extern const pc_algo_filter_def pc_algo_def_deinterlace, pc_algo_def_ntsc;

/* ---- helpers other modules and tests use ---- */

/* other::running_extreme: out[i] = extreme of v[i .. i + k) for every full window (van Herk /
 * Gil-Werman); k <= 1 copies v. Writes len - k + 1 values (none when len < k). `scratch` holds
 * 2 * len floats. Returns the number written. */
size_t pc_algo_other_running_extreme(const float *v, size_t len, size_t k, bool max, float *scratch,
                                     float *out);

/* distort2::curve_at: Catmull-Rom through sorted [t, value] points (clamped ends). */
float pc_algo_distort2_curve_at(const float (*pts)[2], size_t n, float t);

/* other2::rgb_to_hsb (all components 0..1). */
void pc_algo_other2_rgb_to_hsb(const float c[3], float out[3]);

/* stylize2 / oil reaches used by halos (px). */
float pc_algo_stylize2_extrude_reach(float depth);
float pc_algo_stylize2_wind_reach(pc_algo_wind_method method);
float pc_algo_oil_reach(float stylization, float scale);
/* gallery::reach: pixels read beyond a tile for a maximum Blur Gallery blur. */
float pc_algo_blur_gallery_reach(float blur);

#endif
