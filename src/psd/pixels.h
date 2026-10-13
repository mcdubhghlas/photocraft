/* Sample helpers and RGBA8 extraction for previews and tests (port of
 * crates/psd/src/pixels.rs). No colour management: 16-bit samples are scaled with rounding,
 * 32-bit floats clamped to 0..1, CMYK converted naively from the stored (inverted) values.
 * Thread-safe. */
#ifndef PC_PSD_PIXELS_H
#define PC_PSD_PIXELS_H

#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/file.h"

/* An 8-bit image positioned in document coordinates (RGBA interleaved, or one channel). */
typedef struct pc_psd_image8 {
    int32_t left;
    int32_t top;
    uint32_t width;
    uint32_t height;
    pc_psd_bytes data;
    uint8_t default_value; /* masks: value outside the rectangle */
} pc_psd_image8;

void pc_psd_image8_fini(pc_psd_image8 *img);

/* A 1-bit plane (MSB first, rows padded to bytes) as one byte per pixel (0 or 1). */
pc_status pc_psd_unpack_bits(const uint8_t *bytes, size_t len, size_t width, size_t height, pc_psd_bytes *out,
                             pc_error *err);
/* One plane at `depth` to 8 bits (1: set bit = black 0, clear = 255). */
pc_status pc_psd_plane_to_u8(const uint8_t *bytes, size_t len, uint16_t depth, size_t width, size_t height,
                             pc_psd_bytes *out, pc_error *err);

/* Layer pixels as RGBA8 (transparency channel as alpha; the user mask is not applied). RGB,
 * grayscale and CMYK at 8/16/32 bits. */
pc_status pc_psd_layer_rgba8(const pc_psd_file *f, size_t index, pc_psd_image8 *out, pc_error *err);
/* The user mask (channel -3 if present, else -2) as 8-bit gray with its rect and default
 * value. PC_ERR_NOT_FOUND when the layer has no mask channel. */
pc_status pc_psd_layer_user_mask(const pc_psd_file *f, size_t index, pc_psd_image8 *out, pc_error *err);
/* The merged composite as RGBA8 (RGB, gray, CMYK, indexed, duotone, bitmap; alpha from the first
 * extra channel when pc_psd_file_merged_has_alpha). Removes the white matte from RGB composites
 * before reducing precision. An RGBA output over PC_PSD_MAX_DECODED_BYTES is PC_ERR_LIMIT, checked
 * before any plane is decoded (pc_psd_layer_rgba8 likewise). */
pc_status pc_psd_composite_rgba8(const pc_psd_file *f, pc_psd_image8 *out, pc_error *err);

#endif
