/* Web-optimised writers for Save for Web (port of crates/codecs/src/web.rs): indexed and
 * animated GIF (own LZW encoder, GIF89a), WBMP type 0 and baseline/progressive JPEG from 8-bit
 * RGB. Indexed PNG is pc_cencode_png_indexed. Every writer has a reader (GIF through the regular
 * decoder, WBMP through pc_web_decode_wbmp). Thread-safe. */
#ifndef PC_CODECS_WEB_H
#define PC_CODECS_WEB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/buf.h"
#include "base/status.h"

/* GIF variable-code-length LZW of indices (each < 2^min_code_size); *out replaced. */
pc_status pc_web_lzw_encode(const uint8_t *indices, size_t n, uint8_t min_code_size, pc_buf *out, pc_error *err);

/* An indexed GIF89a: palette 1..=256 entries, one index per pixel, optional transparent index
 * (-1 for none) and interlacing. "invalid image: ..." on bad sizes or indices. */
pc_status pc_web_encode_gif_indexed(uint32_t w, uint32_t h, const uint8_t *indices, size_t n,
                                    const uint8_t (*palette)[3], size_t npal, int transparent, bool interlaced,
                                    pc_buf *out, pc_error *err);

typedef struct pc_gif_frame {
    const uint8_t *indices; /* w * h */
    size_t n;
    const uint8_t (*palette)[3];
    size_t npal;
    int transparent; /* -1 for none */
    uint16_t delay_cs;
} pc_gif_frame;

/* An animated GIF89a; each frame carries its own colour table; loop_forever adds NETSCAPE2.0. */
pc_status pc_web_encode_gif_animated(uint32_t w, uint32_t h, const pc_gif_frame *frames, size_t nframes,
                                     bool loop_forever, pc_buf *out, pc_error *err);

/* WBMP type 0 (1 bit per pixel, rows padded to bytes, 1 = white): white[i] per pixel. */
pc_status pc_web_encode_wbmp(uint32_t w, uint32_t h, const bool *white, size_t n, pc_buf *out, pc_error *err);
/* Reads a WBMP type 0: size and a new array of w * h booleans. */
pc_status pc_web_decode_wbmp(const uint8_t *b, size_t n, uint32_t *w, uint32_t *h, bool **white, pc_error *err);

/* JPEG from 8-bit RGB (3 * w * h bytes): quality 1..=100, progressive scans, optimised Huffman
 * tables, ICC (NULL for none), density (has_dpi) and XMP (NULL for none). *out replaced. */
pc_status pc_web_encode_jpeg_rgb8(uint32_t w, uint32_t h, const uint8_t *rgb, size_t n, uint8_t quality,
                                  bool progressive, bool optimized, const uint8_t *icc, size_t icc_len, bool has_dpi,
                                  float dpi, const char *xmp, size_t xmp_len, pc_buf *out, pc_error *err);

#endif
