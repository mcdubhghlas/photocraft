/* DNG segment codecs backed by system libraries (part of crates/raw/src/sensor.rs's read_plane):
 * compression 8 (zlib, RFC 1950; the reference uses flate2) and compression 34892 (baseline JPEG
 * tiles; the reference uses zune-jpeg, here libjpeg-turbo). Lossy JPEG pixels may differ from the
 * reference by the decoders' rounding (REWRITE_PLAN.md §7). */
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <zlib.h>

#include "base/mem.h"
#include "base/num.h"
#include "raw/internal.h"

pc_status rx_inflate_zlib(const uint8_t *src, size_t n, uint8_t *out, size_t cap, size_t *got, pc_error *err)
{
    z_stream z;
    size_t in_left = n;
    size_t out_left = cap;
    bool bad = false;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) {
        return rx_nomem(err);
    }
    z.next_in = (Bytef *)(uintptr_t)src;
    z.next_out = out;
    /* zlib counts in uInt: feed and drain in slices until the stream ends, the segment is full or
     * no progress is possible (truncated input: the caller sees the short length). */
    while (out_left > 0) {
        uInt ai = in_left > 0x40000000u ? 0x40000000u : (uInt)in_left;
        uInt ao = out_left > 0x40000000u ? 0x40000000u : (uInt)out_left;
        int rc;
        z.avail_in = ai;
        z.avail_out = ao;
        rc = inflate(&z, (size_t)ai == in_left ? Z_FINISH : Z_NO_FLUSH);
        in_left -= ai - z.avail_in;
        out_left -= ao - z.avail_out;
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            bad = true;
            break;
        }
        if (z.avail_in == ai && z.avail_out == ao) {
            break;
        }
    }
    *got = cap - out_left;
    (void)inflateEnd(&z);
    if (bad) {
        return rx_malformed(err, "deflate tile does not decompress");
    }
    return PC_OK;
}

/* ---- baseline JPEG tiles ---- */

typedef struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];
} jerr;

static void on_error(j_common_ptr c)
{
    jerr *e = (jerr *)(void *)c->err;
    (*c->err->format_message)(c, e->msg);
    longjmp(e->jb, 1);
}

/* Corrupt-data warnings (premature end, bad Huffman code...) fail the tile instead of decoding
 * grey or garbage. */
static void on_message(j_common_ptr c, int level)
{
    if (level < 0) {
        on_error(c);
    }
}

typedef struct jjob {
    const uint8_t *src;
    size_t n;
    size_t w;
    size_t rows;
    size_t samples;
    uint16_t *out;
    uint8_t *line;
    pc_error *err;
    pc_status st;
} jjob;

/* Runs the decode; everything setjmp must see lives in *j (no locals change after setjmp). */
static void jdecode(jjob *j)
{
    struct jpeg_decompress_struct c;
    jerr e;
    memset(&c, 0, sizeof c);
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = on_error;
    e.pub.emit_message = on_message;
    e.msg[0] = '\0';
    if (setjmp(e.jb)) {
        jpeg_destroy_decompress(&c);
        if (j->st == PC_OK) {
            j->st = rx_malformed(j->err, "lossy JPEG tile: %s", e.msg);
        }
        return;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, j->src, (unsigned long)j->n);
    if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&c);
        j->st = rx_malformed(j->err, "lossy JPEG tile has no header");
        return;
    }
    if ((size_t)c.image_width != j->w || (size_t)c.image_height != j->rows) {
        j->st = rx_malformed(j->err, "lossy JPEG tile is %ux%u, expected %zux%zu", (unsigned)c.image_width,
                             (unsigned)c.image_height, j->w, j->rows);
        jpeg_destroy_decompress(&c);
        return;
    }
    c.out_color_space = j->samples == 1 ? JCS_GRAYSCALE : JCS_RGB;
    (void)jpeg_start_decompress(&c);
    if ((size_t)c.output_components != j->samples || (size_t)c.output_width != j->w) {
        j->st = rx_malformed(j->err, "lossy JPEG tile holds %zu samples, expected %zu",
                             (size_t)c.output_width * c.output_height * (size_t)c.output_components,
                             j->w * j->rows * j->samples);
        jpeg_destroy_decompress(&c);
        return;
    }
    while (c.output_scanline < c.output_height) {
        size_t y = c.output_scanline;
        JSAMPROW row = j->line;
        size_t i;
        if (jpeg_read_scanlines(&c, &row, 1) != 1) {
            break;
        }
        for (i = 0; i < j->w * j->samples; i++) {
            /* The 8-bit samples widen to the 16-bit range (0xFF -> 0xFFFF). */
            j->out[y * j->w * j->samples + i] = (uint16_t)(j->line[i] * 257u);
        }
    }
    (void)jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
}

pc_status rx_lossy_jpeg_segment(const uint8_t *src, size_t n, size_t w, size_t rows, size_t samples,
                                uint16_t **out, pc_error *err)
{
    jjob j;
    size_t total;
    if (samples != 1 && samples != 3) {
        return rx_unsupported(err, "lossy DNG with %zu samples per pixel (1 or 3 are supported)", samples);
    }
    if (n > (size_t)(unsigned long)-1) {
        return rx_malformed(err, "lossy JPEG tile is too large");
    }
    if (!pc_size_mul3(w, rows, samples, &total)) {
        return rx_malformed(err, "lossy JPEG tile is too large");
    }
    memset(&j, 0, sizeof j);
    j.src = src;
    j.n = n;
    j.w = w;
    j.rows = rows;
    j.samples = samples;
    j.err = err;
    j.st = PC_OK;
    j.out = pc_calloc_array(total > 0 ? total : 1, sizeof *j.out);
    j.line = pc_malloc_array(w * samples > 0 ? w * samples : 1, 1);
    if (j.out == NULL || j.line == NULL) {
        free(j.out);
        free(j.line);
        return rx_nomem(err);
    }
    jdecode(&j);
    free(j.line);
    if (j.st != PC_OK) {
        free(j.out);
        return j.st;
    }
    *out = j.out;
    return PC_OK;
}
