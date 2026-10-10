/* Colour reduction for Image > Mode > Indexed Color and Bitmap (port of
 * crates/algo/src/quantize.rs): palette construction (exact, system, web, uniform, perceptual,
 * selective, adaptive), forced colours, dithering (none, Floyd-Steinberg diffusion, ordered
 * pattern, noise) and 1-bit conversion methods.
 *
 * Buffers are straight RGBA in 0..1, row-major. Median cut follows Heckbert (1982), k-means
 * refinement is Lloyd's algorithm, error diffusion Floyd & Steinberg (1976). Palettes match the
 * reference entry for entry. Pure; thread-safe; results independent of the thread count. */
#ifndef PC_ALGO_QUANTIZE_H
#define PC_ALGO_QUANTIZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"

typedef enum pc_algo_palette_kind {
    PC_ALGO_PALETTE_EXACT, /* every colour of the image (fails above 256) */
    PC_ALGO_PALETTE_SYSTEM_MAC,
    PC_ALGO_PALETTE_SYSTEM_WINDOWS,
    PC_ALGO_PALETTE_WEB,
    PC_ALGO_PALETTE_UNIFORM,
    PC_ALGO_PALETTE_PERCEPTUAL, /* Lab median cut + k-means */
    PC_ALGO_PALETTE_SELECTIVE,  /* perceptual, entries near web-safe colours snapped to them */
    PC_ALGO_PALETTE_ADAPTIVE    /* RGB median cut weighted by frequency */
} pc_algo_palette_kind;

typedef enum pc_algo_forced {
    PC_ALGO_FORCED_NONE,
    PC_ALGO_FORCED_BLACK_WHITE,
    PC_ALGO_FORCED_PRIMARIES,
    PC_ALGO_FORCED_WEB
} pc_algo_forced;

typedef enum pc_algo_dither {
    PC_ALGO_DITHER_NONE,
    PC_ALGO_DITHER_DIFFUSION,
    PC_ALGO_DITHER_PATTERN,
    PC_ALGO_DITHER_NOISE
} pc_algo_dither;

typedef enum pc_algo_halftone_shape {
    PC_ALGO_HALFTONE_ROUND,
    PC_ALGO_HALFTONE_ELLIPSE,
    PC_ALGO_HALFTONE_LINE,
    PC_ALGO_HALFTONE_SQUARE,
    PC_ALGO_HALFTONE_DIAMOND,
    PC_ALGO_HALFTONE_CROSS
} pc_algo_halftone_shape;

typedef enum pc_algo_bitmap_kind {
    PC_ALGO_BITMAP_THRESHOLD,
    PC_ALGO_BITMAP_PATTERN,
    PC_ALGO_BITMAP_DIFFUSION,
    PC_ALGO_BITMAP_HALFTONE
} pc_algo_bitmap_kind;

/* BitmapMethod; cell / angle (degrees) / shape are used by HALFTONE only. */
typedef struct pc_algo_bitmap_method {
    pc_algo_bitmap_kind kind;
    float cell, angle;
    pc_algo_halftone_shape shape;
} pc_algo_bitmap_method;

/* At most 256 8-bit RGB entries. */
typedef struct pc_algo_palette {
    uint8_t c[256][3];
    size_t n;
} pc_algo_palette;

/* PaletteKind::from_id ("exact", "system"/"systemMac"/"systemMacOs", "systemWindows", "web",
 * "uniform", "perceptual"/"localPerceptual", "selective"/"localSelective",
 * "adaptive"/"localAdaptive"); false for anything else. */
bool pc_algo_palette_kind_from_id(const char *s, pc_algo_palette_kind *out);
/* HalftoneShape::from_id: unknown ids are Round. */
pc_algo_halftone_shape pc_algo_halftone_shape_from_id(const char *s);

/* The 216-colour web-safe cube, classic Mac OS (cube + red/green/blue/gray ramps, 256) and
 * Windows (20 static colours + cube + gray ramp, 256) palettes. */
void pc_algo_quantize_web_palette(pc_algo_palette *out);
void pc_algo_quantize_mac_palette(pc_algo_palette *out);
void pc_algo_quantize_windows_palette(pc_algo_palette *out);
/* Up to n colours on an equal-step RGB grid (k^3 entries, k = max(floor(cbrt n), 2)); malloc'd
 * (*out has *count * 3 bytes). PC_ERR_LIMIT past 2^24 entries (the reference runs out of
 * memory). */
pc_status pc_algo_quantize_uniform_palette(size_t n, uint8_t **out, size_t *count, pc_error *err);

/* Builds the palette for n pixels. `colors` (clamped to 2..256) is the requested count for the
 * computed kinds; forced colours come first and count toward it. Exact with more than 256
 * colours fails with PC_ERR_INVALID and the reference's message. */
pc_status pc_algo_quantize_build_palette(const float (*px)[4], size_t n, pc_algo_palette_kind kind,
                                         size_t colors, pc_algo_forced forced, pc_algo_palette *out,
                                         pc_error *err);

/* 8x8 Bayer threshold in -0.5..0.5. */
float pc_algo_quantize_bayer8(size_t x, size_t y);

/* Maps every opaque pixel (n pixels, rows `w` wide) to its palette entry in place and writes the
 * index per pixel to idx (n bytes). pal holds npal RGB triples (3 * npal bytes). Pixels with alpha < 1/2 get
 * index `transparent` (and alpha 0) when has_transparent; otherwise alpha is kept. `amount` (0..1) scales the
 * dither strength. PC_ERR_INVALID for an empty palette (the reference panics); NOMEM. */
pc_status pc_algo_quantize(float (*px)[4], size_t n, size_t w, const uint8_t *pal, size_t npal,
                           pc_algo_dither dither, float amount, bool has_transparent, size_t transparent,
                           uint8_t *idx, pc_error *err);

/* Converts gray values (0..1, n values, rows `w` wide) to 0/1 in place. PC_ERR_INVALID when
 * w == 0 for Pattern / Halftone with values (the reference divides by zero); NOMEM. */
pc_status pc_algo_quantize_to_bitmap(float *gray, size_t n, size_t w, pc_algo_bitmap_method method,
                                     pc_error *err);

#endif
