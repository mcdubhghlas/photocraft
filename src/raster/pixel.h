/* Pixel encoding and model conversion helpers (the free functions of crates/raster/src/lib.rs).
 *
 * A pixel is `pc_pixel_format_channels(f)` normalised floats (colour channels, then alpha).
 * Functions that take `px, n` read missing channels (n < channels) as 0 where the reference
 * would have panicked. At most PC_RASTER_MAX_CHANNELS channels per pixel.
 * Pure functions; thread-safe (CMYK goes through color/convert.h and so honours the thread's
 * active document CMYK space). */
#ifndef PC_RASTER_PIXEL_H
#define PC_RASTER_PIXEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "color/color.h"

#define PC_RASTER_MAX_CHANNELS 5          /* CMYK + alpha */
#define PC_RASTER_MAX_PIXEL_BYTES (5 * 4) /* ... at 32-bit float */

/* Encodes min(n, channels) normalised floats into one pixel's bytes (`out` holds at least
 * bytes_per_pixel). Integer samples are clamped and rounded. */
void pc_encode_pixel(pc_pixel_format f, const float *px, size_t n, uint8_t *out);
/* Decodes one pixel (`bytes` holds bytes_per_pixel) into `channels` floats. */
void pc_decode_pixel(pc_pixel_format f, const uint8_t *bytes, float *out);

/* Straight RGBA (sRGB) of a pixel in format f: gray replicated, CMYK through the managed
 * conversion, Lab via the D50 formulas; alpha 1 when the format has none. */
void pc_to_rgba(pc_pixel_format f, const float *px, size_t n, float out[4]);
/* A pixel of format f from straight RGBA; writes channels values and returns that count. */
size_t pc_from_rgba(pc_pixel_format f, const float rgba[4], float out[PC_RASTER_MAX_CHANNELS]);
/* One pixel between formats (same model: channels copied, alpha added as 1 or dropped; else
 * via sRGB RGBA). Returns the channel count written to out. */
size_t pc_convert_pixel(pc_pixel_format from, pc_pixel_format to, const float *px, size_t n,
                        float out[PC_RASTER_MAX_CHANNELS]);

/* Can this sample type hold values above 1.0 (HDR)? */
bool pc_sample_is_hdr(pc_sample_type s);

/* A simple owned RGBA8 image (display, thumbnails): width * height * 4 bytes. */
typedef struct pc_rgba8_image {
    uint32_t width;
    uint32_t height;
    uint8_t *pixels; /* owned; NULL when empty */
} pc_rgba8_image;

/* Zero-filled image. PC_ERR_LIMIT when the size overflows or exceeds PC_MAX_ALLOC. */
pc_status pc_rgba8_image_init(pc_rgba8_image *img, uint32_t width, uint32_t height, pc_error *err);
void pc_rgba8_image_fini(pc_rgba8_image *img);

#endif
