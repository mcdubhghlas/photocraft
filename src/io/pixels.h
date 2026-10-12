/* Conversions between PSD planar big-endian channel data and interleaved native-endian surface
 * bytes (port of crates/io/src/pixels.rs). The reference's par_map/bands helpers are not
 * ported: callers parallelise with pc_pool (results never depend on the thread count).
 * Thread-safe (no shared state; the shuffles use pc_pool_global). */
#ifndef PC_IO_PIXELS_H
#define PC_IO_PIXELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/status.h"
#include "color/color.h"

/* Top of Photoshop's 16-bit Lab a* and b* scale (32768 + 256·a spans -128..127 over 0..65280). */
#define PC_IO_LAB16_CHROMA_MAX 65280.0f

/* PSD depth in bits for a sample type (8, 16, 32). */
uint16_t pc_io_psd_depth(pc_sample_type s);
/* Sample type for a PSD depth: 16 -> U16, 32 -> F32, anything else (1, 8) -> U8. */
pc_sample_type pc_io_sample_for_depth(uint16_t depth);

/* One source plane of pc_io_interleave: big-endian samples, or absent (present false). */
typedef struct pc_io_plane {
    bool present;
    const uint8_t *data;
    size_t len;
} pc_io_plane;

/* Interleaves `ch` planes of n pixels into native-endian samples (*out: n * ch * bytes bytes,
 * malloc'd, caller frees). A plane that is absent or too short for a pixel gives fill[c] (a
 * native-endian encoded sample) there; channels with invert[c] are inverted (PSD stores CMYK as
 * max - ink). PC_ERR_LIMIT when the output size overflows or exceeds PC_MAX_ALLOC. */
pc_status pc_io_interleave(const pc_io_plane *planes, size_t ch, const uint8_t (*fill)[4], size_t n,
                           pc_sample_type s, const bool *invert, uint8_t **out, size_t *out_len,
                           pc_error *err);

/* Splits interleaved native-endian bytes of `ch` channels into big-endian planes. The planes
 * are contiguous in *out (plane c at c * *plane_len), n = len / (ch * bytes) pixels each (a
 * partial trailing pixel is ignored). *out is malloc'd (caller frees; NULL when empty). */
pc_status pc_io_deinterleave(const uint8_t *bytes, size_t len, size_t ch, pc_sample_type s,
                             const bool *invert, uint8_t **out, size_t *plane_len, pc_error *err);

/* Native-endian encoding of the maximum sample value / of zero; returns the byte count. */
size_t pc_io_max_sample(pc_sample_type s, uint8_t out[4]);
size_t pc_io_zero_sample(pc_sample_type s, uint8_t out[4]);

/* Rescales the a* and b* channels (1 and 2) of interleaved native-endian 16-bit Lab pixels with
 * `ch` channels: PSD -> document when to_doc, else document -> PSD. Exact round trip; buffers
 * with fewer than 3 channels are left alone. */
void pc_io_lab16_chroma(uint8_t *bytes, size_t len, size_t ch, bool to_doc);

/* Quantises a normalised value to big-endian bytes; returns the byte count (1, 2 or 4). */
size_t pc_io_encode_be(float v, pc_sample_type s, uint8_t out[4]);
/* The big-endian sample at index (in samples) as a normalised value; 0 when out of range. */
float pc_io_decode_be(const uint8_t *bytes, size_t len, size_t index, pc_sample_type s);

/* Photoshop mattes the merged image of a transparent document against white:
 * m = c * a + w * (1 - a). unmatte inverts it (white where a <= 0) and keeps HDR values. */
float pc_io_matte(float c, float a, float white);
float pc_io_unmatte(float m, float a, float white);

#endif
