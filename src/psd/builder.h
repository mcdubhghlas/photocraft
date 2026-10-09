/* PsdBuilder: construct PSD/PSB files from pixel buffers (port of crates/psd/src/builder.rs).
 *
 * The builder never composites: supply the merged image with pc_psd_builder_composite,
 * otherwise a white placeholder is written and resource 1057 says has_real_merged_data = false.
 * Layers are pushed bottom-to-top. Everything passed in is copied. Not thread-safe per builder
 * (distinct builders are independent). */
#ifndef PC_PSD_BUILDER_H
#define PC_PSD_BUILDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "psd/file.h"

/* Interleaved pixels with alpha. CMYK values are ink amounts (0 = no ink); the builder stores
 * them inverted as Photoshop does. `len` counts samples (bytes for 8-bit, uint16 for 16-bit). */
typedef enum pc_psd_px_kind {
    PC_PSD_PX_RGBA8,
    PC_PSD_PX_RGBA16,
    PC_PSD_PX_GRAYA8,
    PC_PSD_PX_GRAYA16,
    PC_PSD_PX_CMYKA8,
    PC_PSD_PX_CMYKA16
} pc_psd_px_kind;

typedef struct pc_psd_pixels {
    pc_psd_px_kind kind;
    const void *data; /* uint8_t[] or uint16_t[] by kind */
    size_t len;
} pc_psd_pixels;

/* A layer mask as 8-bit samples (rect width × height; 0 hidden, 255 shown). */
typedef struct pc_psd_mask_spec {
    pc_psd_rect rect;
    const uint8_t *data;
    size_t len;
    uint8_t default_color;
    bool disabled;
} pc_psd_mask_spec;

typedef struct pc_psd_layer_spec {
    const char *name; /* UTF-8; written as the legacy name and luni */
    int32_t left;
    int32_t top;
    uint32_t width;
    uint32_t height;
    pc_psd_pixels pixels;
    uint8_t blend[4];
    uint8_t opacity;
    bool has_fill_opacity; /* iOpa when set */
    uint8_t fill_opacity;
    bool visible;
    bool clipping;
    const pc_psd_mask_spec *mask; /* optional */
    const pc_psd_block *extra_blocks; /* appended verbatim */
    size_t nextra;
} pc_psd_layer_spec;

/* A visible normal layer at full opacity. */
pc_psd_layer_spec pc_psd_layer_spec_make(const char *name, int32_t left, int32_t top, uint32_t width, uint32_t height,
                                         pc_psd_pixels pixels);

typedef struct pc_psd_group_spec {
    const char *name;
    uint8_t blend[4]; /* default pass-through */
    uint8_t opacity;
    bool visible;
    bool open; /* expanded in the layers panel */
} pc_psd_group_spec;

pc_psd_group_spec pc_psd_group_spec_make(const char *name);

typedef struct pc_psd_builder pc_psd_builder;

/* An 8-bit RGB PSD using RLE. NULL on out of memory. */
pc_psd_builder *pc_psd_builder_new(uint32_t width, uint32_t height);
void pc_psd_builder_free(pc_psd_builder *b);
void pc_psd_builder_color_mode(pc_psd_builder *b, uint16_t mode); /* RGB, grayscale or CMYK */
void pc_psd_builder_depth(pc_psd_builder *b, uint16_t depth);     /* 8 or 16 */
void pc_psd_builder_version(pc_psd_builder *b, pc_psd_version v);
void pc_psd_builder_compression(pc_psd_builder *b, uint16_t c); /* all channels and the merged image */
pc_status pc_psd_builder_resolution(pc_psd_builder *b, double dpi, pc_error *err);
pc_status pc_psd_builder_icc(pc_psd_builder *b, const uint8_t *icc, size_t len, pc_error *err);
pc_status pc_psd_builder_resource(pc_psd_builder *b, const pc_psd_resource *r, pc_error *err);
pc_status pc_psd_builder_push_layer(pc_psd_builder *b, const pc_psd_layer_spec *spec, pc_error *err);
pc_status pc_psd_builder_begin_group(pc_psd_builder *b, const pc_psd_group_spec *spec, pc_error *err);
/* "end_group without begin_group" when no group is open. */
pc_status pc_psd_builder_end_group(pc_psd_builder *b, pc_error *err);
pc_status pc_psd_builder_composite(pc_psd_builder *b, pc_psd_pixels pixels, pc_error *err);

pc_status pc_psd_builder_build(const pc_psd_builder *b, pc_psd_file *out, pc_error *err);
pc_status pc_psd_builder_to_bytes(const pc_psd_builder *b, pc_psd_bytes *out, pc_error *err);

#endif
