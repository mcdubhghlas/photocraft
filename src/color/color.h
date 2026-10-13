/* Pixel formats, colour models and colour values (port of crates/color/src/lib.rs).
 *
 * Bit depth and colour model are runtime data: nothing downstream assumes 8-bit or RGB.
 * Everything here is plain data and pure functions; thread-safe. pc_color_to_rgb on a CMYK
 * colour goes through the colour-managed conversion in color/convert.h (and so honours the
 * thread's active document CMYK space). */
#ifndef PC_COLOR_COLOR_H
#define PC_COLOR_COLOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Storage type of one channel sample. */
typedef enum pc_sample_type { PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32 } pc_sample_type;
#define PC_SAMPLE_TYPE_COUNT 3

size_t pc_sample_type_bytes(pc_sample_type s); /* 1, 2, 4 */
uint32_t pc_sample_type_bits(pc_sample_type s); /* 8, 16, 32 */
const char *pc_sample_type_name(pc_sample_type s); /* serde variant name: "U8", "U16", "F32" */
bool pc_sample_type_from_name(const char *s, size_t len, pc_sample_type *out);

/* Document colour model (Photoshop "Image > Mode"). */
typedef enum pc_color_mode {
    PC_COLOR_MODE_BITMAP,
    PC_COLOR_MODE_GRAYSCALE,
    PC_COLOR_MODE_INDEXED,
    PC_COLOR_MODE_RGB,
    PC_COLOR_MODE_CMYK,
    PC_COLOR_MODE_LAB,
    PC_COLOR_MODE_MULTICHANNEL,
    PC_COLOR_MODE_DUOTONE
} pc_color_mode;
#define PC_COLOR_MODE_COUNT 8

/* Colour (non-alpha) channels used for pixel storage. Indexed and Bitmap documents are stored
 * expanded (RGB / Gray); Multichannel carries its count separately and defaults to 3. */
size_t pc_color_mode_channels(pc_color_mode m);
const char *pc_color_mode_name(pc_color_mode m); /* serde variant name: "Bitmap", "Rgb", ... */
bool pc_color_mode_from_name(const char *s, size_t len, pc_color_mode *out);

/* Layout of a pixel: colour model, sample type, and whether alpha follows the colour channels. */
typedef struct pc_pixel_format {
    pc_color_mode mode;
    pc_sample_type sample;
    bool alpha;
} pc_pixel_format;

#define PC_PIXEL_FORMAT_RGBA8 ((pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true})
#define PC_PIXEL_FORMAT_RGBA16 ((pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true})
#define PC_PIXEL_FORMAT_RGBA32F ((pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true})
#define PC_PIXEL_FORMAT_GRAY8 ((pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false})
#define PC_PIXEL_FORMAT_GRAYA8 ((pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, true})
#define PC_PIXEL_FORMAT_CMYKA8 ((pc_pixel_format){PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true})

size_t pc_pixel_format_channels(pc_pixel_format f);
size_t pc_pixel_format_bytes_per_pixel(pc_pixel_format f);
bool pc_pixel_format_eq(pc_pixel_format a, pc_pixel_format b);

/* Reads sample `index` (a sample index, not a byte index; native byte order) as a normalised
 * float: U8/U16 map to [0, 1], F32 is returned as stored. Out of range (index past `len`
 * bytes) reads 0 instead of crashing (the reference would panic). */
float pc_read_sample(const uint8_t *bytes, size_t len, pc_sample_type s, size_t index);
/* Writes a normalised sample; integer types are clamped and rounded (NaN stores 0). Out of
 * range writes nothing. */
void pc_write_sample(uint8_t *bytes, size_t len, pc_sample_type s, size_t index, float v);

/* Per-pixel gradient dither noise in [0, 1) from a position hash. Shared by the Gradient tool,
 * Gradient Fill layers and the GPU compositor, so they dither identically. */
float pc_dither_noise(int32_t x, int32_t y);

/* A colour in a document's model with alpha, as normalised floats. Up to 4 components (unused
 * ones 0); alpha kept separately. */
typedef struct pc_color {
    pc_color_mode mode;
    float c[4];
    float alpha;
} pc_color;

pc_color pc_color_rgb(float r, float g, float b);
pc_color pc_color_rgba(float r, float g, float b, float a);
pc_color pc_color_gray(float v);
#define PC_COLOR_BLACK ((pc_color){PC_COLOR_MODE_RGB, {0.0f, 0.0f, 0.0f, 0.0f}, 1.0f})
#define PC_COLOR_WHITE ((pc_color){PC_COLOR_MODE_RGB, {1.0f, 1.0f, 1.0f, 0.0f}, 1.0f})
#define PC_COLOR_TRANSPARENT ((pc_color){PC_COLOR_MODE_RGB, {0.0f, 0.0f, 0.0f, 0.0f}, 0.0f})

/* To sRGB (CMYK through the active/built-in CMYK profile, Lab via the D50 formulas; Lab
 * components are stored normalised: L/100, (a+128)/255, (b+128)/255). */
void pc_color_to_rgb(const pc_color *c, float out[3]);
void pc_color_to_rgba8(const pc_color *c, uint8_t out[4]);
/* This colour in `mode`'s model, converted as pixels written to a surface of that mode are
 * (through sRGB); unchanged when already in `mode`. Indexed and Multichannel keep RGB colours,
 * as pc_color_to_rgb reads them. Alpha is kept. */
pc_color pc_color_in_mode(const pc_color *c, pc_color_mode mode);

#endif
