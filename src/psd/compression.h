/* Channel image data codecs: raw, RLE (PackBits), zip and zip with prediction (port of
 * crates/psd/src/compression.rs).
 *
 * Decoded data is *planar* bytes in the file's big-endian sample order: planes × height ×
 * row_bytes (depth 1 packs 8 pixels per byte, MSB first). Decoders bound their output by their
 * input and by PC_PSD_MAX_DECODED_BYTES, and report a machine without enough memory as
 * PC_ERR_LIMIT ("not enough memory for the decoded channel data") instead of failing hard.
 * Zip output bytes differ from the reference's encoder (zlib vs miniz_oxide); they decode
 * identically (REWRITE_PLAN.md §7). Thread-safe. */
#ifndef PC_PSD_COMPRESSION_H
#define PC_PSD_COMPRESSION_H

#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"
#include "psd/types.h"

/* Geometry of a set of planes. */
typedef struct pc_psd_layout {
    size_t planes; /* 1 for a layer channel, the channel count for the merged image */
    size_t width;
    size_t height;
    uint16_t depth;         /* 1, 8, 16, 32 */
    pc_psd_version version; /* PSB uses 4-byte RLE row counts */
} pc_psd_layout;

size_t pc_psd_layout_row_bytes(const pc_psd_layout *l);
/* Total decoded bytes without the cap (PC_ERR_LIMIT "decoded size overflow"). */
pc_status pc_psd_layout_total_bytes(const pc_psd_layout *l, uint64_t *out, pc_error *err);
/* Total decoded bytes, checked against PC_PSD_MAX_DECODED_BYTES. */
pc_status pc_psd_layout_decoded_len(const pc_psd_layout *l, size_t *out, pc_error *err);

/* Decodes planar data (data excludes the 2-byte compression field). *out is replaced. */
pc_status pc_psd_decode_planes(uint16_t compression, const uint8_t *data, size_t len, const pc_psd_layout *l,
                               pc_psd_bytes *out, pc_error *err);
/* Encodes planar data (`len` must equal the layout's decoded length: PC_ERR_INVALID otherwise). */
pc_status pc_psd_encode_planes(uint16_t compression, const uint8_t *decoded, size_t len, const pc_psd_layout *l,
                               pc_psd_bytes *out, pc_error *err);
/* Checks that `data` holds a structurally complete encoding (RLE counts and bytes present, a
 * full zlib stream, enough raw bytes). Unknown methods pass. */
pc_status pc_psd_validate_planes(uint16_t compression, const uint8_t *data, size_t len, const pc_psd_layout *l,
                                 pc_error *err);

/* PackBits (never emits the -128 no-op); decode produces exactly `expected` bytes (trailing
 * input ignored, -128 no-ops skipped). */
pc_status pc_psd_packbits_encode(const uint8_t *src, size_t n, pc_buf *out, pc_error *err); /* appends */
pc_status pc_psd_packbits_decode(const uint8_t *src, size_t n, size_t expected, pc_psd_bytes *out, pc_error *err);

/* zlib at the default level; decompress returns exactly `expected` bytes (stream must hold at
 * least that many). */
pc_status pc_psd_zip_compress(const uint8_t *data, size_t n, pc_psd_bytes *out, pc_error *err);
pc_status pc_psd_zip_decompress(const uint8_t *data, size_t n, size_t expected, pc_psd_bytes *out, pc_error *err);

/* Horizontal delta prediction in place over len bytes of whole rows (1/8-bit byte deltas,
 * 16-bit BE sample deltas, 32-bit byte-plane shuffle then byte deltas). Other depths:
 * PC_ERR_UNSUPPORTED. */
pc_status pc_psd_predict(uint8_t *buf, size_t len, const pc_psd_layout *l, pc_error *err);
pc_status pc_psd_unpredict(uint8_t *buf, size_t len, const pc_psd_layout *l, pc_error *err);

#endif
