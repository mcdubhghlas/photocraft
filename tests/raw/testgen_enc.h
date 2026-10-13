/* Encoders for the synthetic DNG storages the reference's testgen gets from Rust crates (flate2 for
 * compression 8, the `image` crate's JPEG encoder for 34892): here zlib and libjpeg-turbo. Their
 * bytes differ from the reference's, so the parity test loads those files from tests/fixtures/raw/
 * (written by `rustref raw-fixtures`); these encoders serve the round-trip tests. Included by
 * testgen.h. */
#ifndef PC_TESTS_RAW_TESTGEN_ENC_H
#define PC_TESTS_RAW_TESTGEN_ENC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <zlib.h>

/* zlib (RFC 1950) at the default level; aborts on failure (tests only). */
static inline uint8_t *tg_zlib(const uint8_t *data, size_t n, size_t *out_len)
{
    uLongf cap = compressBound((uLong)n);
    uint8_t *out = malloc(cap > 0 ? cap : 1);
    if (out == NULL || compress2(out, &cap, data, (uLong)n, Z_DEFAULT_COMPRESSION) != Z_OK) {
        abort();
    }
    *out_len = cap;
    return out;
}

/* A baseline JPEG of w x h 8-bit samples (1 = gray, 3 = RGB) at quality; malloc'd. */
static inline uint8_t *tg_jpeg(const uint8_t *px, size_t w, size_t h, int comps, int quality, size_t *out_len)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr e;
    unsigned char *buf = NULL;
    unsigned long len = 0;
    uint8_t *out;
    size_t y;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &buf, &len);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = comps;
    c.in_color_space = comps == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    jpeg_start_compress(&c, TRUE);
    for (y = 0; y < h; y++) {
        JSAMPROW row = (JSAMPROW)(uintptr_t)(px + y * w * (size_t)comps);
        (void)jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    out = malloc(len > 0 ? len : 1);
    if (out == NULL) {
        abort();
    }
    memcpy(out, buf, len);
    free(buf);
    *out_len = len;
    return out;
}

#endif
