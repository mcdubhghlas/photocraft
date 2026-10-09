/* zlib/deflate/gzip (zlib) and zstd (libzstd) wrappers. Replace flate2, crc32fast and ruzstd
 * (REWRITE_PLAN.md D15). Thread-safe.
 *
 * Decompression mirrors the reference's `Read::take(max)` pattern: output stops at max_out
 * bytes, at the end of the stream, or when the input runs out. Only corrupt data is an error;
 * a truncated stream or one longer than max_out is *not* (check info->stream_end and the output
 * length, as the Rust callers do). Error messages match flate2's ("corrupt deflate stream").
 * Compressed bytes differ from the reference's encoders; they decompress identically (§7). */
#ifndef PC_BASE_COMPRESS_H
#define PC_BASE_COMPRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "status.h"

typedef enum pc_zfmt {
    PC_Z_ZLIB, /* RFC 1950 (flate2 Zlib*), Adler-32 checked */
    PC_Z_RAW,  /* RFC 1951 raw deflate (flate2 Deflate*) */
    PC_Z_GZIP  /* RFC 1952, first member only (flate2 GzDecoder) */
} pc_zfmt;

/* flate2::Compression levels. */
enum { PC_Z_FAST = 1, PC_Z_DEFAULT = 6, PC_Z_BEST = 9 };

typedef struct pc_decomp_info {
    bool stream_end; /* the (first) stream/frame ended within the input */
    size_t consumed; /* input bytes used */
} pc_decomp_info;

/* Compress data (zlib or raw only; level 0-9) and append to *out. */
pc_status pc_deflate(pc_zfmt fmt, int level, const uint8_t *data, size_t len, pc_buf *out, pc_error *err);

/* Decompress at most max_out bytes into *out (replacing it on success; unchanged on failure).
 * info may be NULL. */
pc_status pc_inflate(pc_zfmt fmt, const uint8_t *data, size_t len, size_t max_out, pc_buf *out,
                     pc_decomp_info *info, pc_error *err);

/* As pc_inflate but discards the output, counting it (stops counting at max_out). For
 * validating a stream without holding it. */
pc_status pc_inflate_count(pc_zfmt fmt, const uint8_t *data, size_t len, size_t max_out, size_t *count,
                           pc_decomp_info *info, pc_error *err);

/* CRC-32 (IEEE, as zlib/crc32fast), continuing from crc (start with 0). */
uint32_t pc_crc32(uint32_t crc, const uint8_t *data, size_t len);

/* zstd-compress data at `level` (1 = the reference's CompressionLevel::Fastest) and append. */
pc_status pc_zstd_compress(int level, const uint8_t *data, size_t len, pc_buf *out, pc_error *err);

/* Decode ONE zstd frame (ruzstd StreamingDecoder) into at most max_out bytes, replacing *out.
 * window_log_max limits the frame's window (memory); 0 means 27 (128 MiB). Input that ends
 * mid-frame is an error ("zstd: truncated frame"), as in ruzstd. info->consumed tells where the
 * next frame starts. */
pc_status pc_zstd_decompress(const uint8_t *data, size_t len, size_t max_out, int window_log_max, pc_buf *out,
                             pc_decomp_info *info, pc_error *err);

#endif
