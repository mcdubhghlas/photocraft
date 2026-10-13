#include "codecs/web.h"

#include <stdlib.h>
#include <string.h>

#include "codecs/internal.h"
#include "codecs/resolution.h"

pc_status cx_jpeg_encode_rgb8(uint32_t w, uint32_t h, const uint8_t *rgb, int quality, bool progressive,
                              bool optimized, const uint8_t *icc, size_t icc_len, bool has_dpi, float dpi,
                              const char *xmp, size_t xmp_len, pc_buf *out, pc_error *err);

/* ---- GIF LZW ---- */

typedef struct sink {
    pc_buf *out;
    uint32_t acc;
    uint32_t nbits;
    pc_status st;
} sink;

static void put(sink *s, uint16_t code, uint32_t size)
{
    s->acc |= (uint32_t)code << s->nbits;
    s->nbits += size;
    while (s->nbits >= 8) {
        if (s->st == PC_OK) {
            s->st = pc_buf_append_byte(s->out, (uint8_t)s->acc, NULL);
        }
        s->acc >>= 8;
        s->nbits -= 8;
    }
}

pc_status cx_gif_lzw_encode(const uint8_t *indices, size_t n, uint8_t min_code_size, pc_buf *out, pc_error *err)
{
    enum { HSIZE = 9001 };
    uint32_t *keys = malloc(HSIZE * sizeof *keys);
    uint16_t *vals = malloc(HSIZE * sizeof *vals);
    uint32_t min = min_code_size < 2 ? 2 : min_code_size > 8 ? 8 : min_code_size;
    uint16_t clear = (uint16_t)(1u << min);
    uint16_t eoi = (uint16_t)(clear + 1);
    uint32_t size = min + 1;
    uint32_t next = eoi + 1u;
    sink s = {out, 0, 0, PC_OK};
    uint16_t prefix;
    size_t i;
    if (keys == NULL || vals == NULL) {
        free(keys);
        free(vals);
        return cx_nomem(err);
    }
    for (i = 0; i < HSIZE; i++) {
        keys[i] = 0xFFFFFFFFu;
    }
    put(&s, clear, size);
    if (n == 0) {
        put(&s, eoi, size);
    } else {
        prefix = indices[0];
        for (i = 1; i < n; i++) {
            uint32_t key = ((uint32_t)prefix << 8) | indices[i];
            size_t h = key % HSIZE;
            while (keys[h] != 0xFFFFFFFFu && keys[h] != key) {
                h = h + 1 == HSIZE ? 0 : h + 1;
            }
            if (keys[h] == key) {
                prefix = vals[h];
                continue;
            }
            put(&s, prefix, size);
            /* The decoder adds its entries one code late: widen once the next free code no longer
             * fits (before assigning it), like the classic compress-based encoders. */
            if (next >= (1u << size) && size < 12) {
                size++;
            }
            if (next < 4096) {
                keys[h] = key;
                vals[h] = (uint16_t)next;
                next++;
            } else {
                put(&s, clear, size);
                for (h = 0; h < HSIZE; h++) {
                    keys[h] = 0xFFFFFFFFu;
                }
                size = min + 1;
                next = eoi + 1u;
            }
            prefix = indices[i];
        }
        put(&s, prefix, size);
        if (next >= (1u << size) && size < 12) {
            size++;
        }
        put(&s, eoi, size);
    }
    if (s.nbits > 0 && s.st == PC_OK) {
        s.st = pc_buf_append_byte(out, (uint8_t)s.acc, NULL);
    }
    free(keys);
    free(vals);
    return s.st == PC_OK ? PC_OK : cx_nomem(err);
}

pc_status pc_web_lzw_encode(const uint8_t *indices, size_t n, uint8_t min_code_size, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st = cx_gif_lzw_encode(indices, n, min_code_size, &b, err);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

/* ---- GIF writers ---- */

static uint8_t table_bits(size_t npal)
{
    uint8_t b;
    for (b = 1; b < 8; b++) {
        if (((size_t)1 << b) >= npal) {
            return b;
        }
    }
    return 8;
}

static void put_table(cx_w *w, const uint8_t (*palette)[3], size_t npal, uint8_t bits)
{
    size_t i;
    static const uint8_t black[3] = {0, 0, 0};
    for (i = 0; i < ((size_t)1 << bits); i++) {
        cx_w_put(w, i < npal ? palette[i] : black, 3);
    }
}

/* The LZW data of indices as GIF sub-blocks, then the terminator. */
static void put_image_data(cx_w *w, const uint8_t *idx, size_t n, uint8_t min)
{
    pc_buf z = PC_BUF_INIT;
    size_t i;
    if (w->st != PC_OK) {
        return;
    }
    cx_w_byte(w, min);
    w->st = cx_gif_lzw_encode(idx, n, min, &z, w->err);
    for (i = 0; w->st == PC_OK && i < z.len; i += 255) {
        size_t k = z.len - i < 255 ? z.len - i : 255;
        cx_w_byte(w, (uint8_t)k);
        cx_w_put(w, z.data + i, k);
    }
    cx_w_byte(w, 0);
    pc_buf_fini(&z);
}

static bool indices_ok(const uint8_t *idx, size_t n, size_t npal)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (idx[i] >= npal) {
            return false;
        }
    }
    return true;
}

pc_status pc_web_encode_gif_indexed(uint32_t width, uint32_t height, const uint8_t *indices, size_t n,
                                    const uint8_t (*palette)[3], size_t npal, int transparent, bool interlaced,
                                    pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    cx_w w = {&b, PC_OK, err};
    size_t wz = width;
    size_t hz = height;
    uint8_t bits;
    uint8_t min;
    if (width == 0 || height == 0 || width > 65535 || height > 65535 || n != wz * hz) {
        return cx_invalid(err, "GIF size must be 1..=65535 and match the index data");
    }
    if (npal == 0 || npal > 256 || !indices_ok(indices, n, npal)) {
        return cx_invalid(err, "palette must have 1..=256 entries covering every index");
    }
    bits = table_bits(npal);
    cx_w_put(&w, "GIF89a", 6);
    cx_w_le16(&w, (uint16_t)width);
    cx_w_le16(&w, (uint16_t)height);
    cx_w_byte(&w, (uint8_t)(0x80 | ((bits - 1) << 4) | (bits - 1)));
    cx_w_byte(&w, transparent >= 0 ? (uint8_t)transparent : 0);
    cx_w_byte(&w, 0);
    put_table(&w, palette, npal, bits);
    if (transparent >= 0) {
        uint8_t gce[8] = {0x21, 0xF9, 0x04, 0x01, 0, 0, (uint8_t)transparent, 0};
        cx_w_put(&w, gce, 8);
    }
    cx_w_byte(&w, 0x2C);
    cx_w_le16(&w, 0);
    cx_w_le16(&w, 0);
    cx_w_le16(&w, (uint16_t)width);
    cx_w_le16(&w, (uint16_t)height);
    cx_w_byte(&w, interlaced ? 0x40 : 0);
    min = bits < 2 ? 2 : bits;
    if (interlaced) {
        static const size_t PASS[4][2] = {{0, 8}, {4, 8}, {2, 4}, {1, 2}};
        uint8_t *v = malloc(n);
        size_t k = 0;
        int p;
        if (v == NULL) {
            pc_buf_fini(&b);
            return cx_nomem(err);
        }
        for (p = 0; p < 4; p++) {
            size_t y;
            for (y = PASS[p][0]; y < hz; y += PASS[p][1]) {
                memcpy(v + k, indices + y * wz, wz);
                k += wz;
            }
        }
        put_image_data(&w, v, n, min);
        free(v);
    } else {
        put_image_data(&w, indices, n, min);
    }
    cx_w_byte(&w, 0x3B);
    if (w.st != PC_OK) {
        pc_buf_fini(&b);
        return w.st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

pc_status pc_web_encode_gif_animated(uint32_t width, uint32_t height, const pc_gif_frame *frames, size_t nframes,
                                     bool loop_forever, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    cx_w w = {&b, PC_OK, err};
    size_t i;
    if (width == 0 || height == 0 || width > 65535 || height > 65535) {
        return cx_invalid(err, "GIF size must be 1..=65535");
    }
    if (nframes == 0) {
        return cx_invalid(err, "an animated GIF needs at least one frame");
    }
    cx_w_put(&w, "GIF89a", 6);
    cx_w_le16(&w, (uint16_t)width);
    cx_w_le16(&w, (uint16_t)height);
    cx_w_byte(&w, 0x70); /* no global colour table, 8-bit colour resolution */
    cx_w_byte(&w, 0);
    cx_w_byte(&w, 0);
    if (loop_forever) {
        static const uint8_t ext[19] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P',
                                        'E',  '2',  '.',  '0', 0x03, 0x01, 0x00, 0x00, 0x00};
        cx_w_put(&w, ext, sizeof ext);
    }
    for (i = 0; i < nframes; i++) {
        const pc_gif_frame *f = &frames[i];
        uint8_t bits;
        if (f->npal == 0 || f->npal > 256 || f->n != (size_t)width * height || !indices_ok(f->indices, f->n, f->npal)) {
            pc_buf_fini(&b);
            return cx_invalid(err, "every frame's palette must have 1..=256 entries covering its indices");
        }
        bits = table_bits(f->npal);
        /* Graphic Control Extension: delay and optional transparency. Every frame covers the whole
         * image, so an opaque one is left in place (disposal 1), while one with a transparent index
         * is cleared afterwards (disposal 2): otherwise the frames before it show through. */
        cx_w_byte(&w, 0x21);
        cx_w_byte(&w, 0xF9);
        cx_w_byte(&w, 0x04);
        cx_w_byte(&w, f->transparent >= 0 ? 0x09 : 0x04);
        cx_w_le16(&w, f->delay_cs);
        cx_w_byte(&w, f->transparent >= 0 ? (uint8_t)f->transparent : 0);
        cx_w_byte(&w, 0);
        /* Image Descriptor with a Local Colour Table. */
        cx_w_byte(&w, 0x2C);
        cx_w_le16(&w, 0);
        cx_w_le16(&w, 0);
        cx_w_le16(&w, (uint16_t)width);
        cx_w_le16(&w, (uint16_t)height);
        cx_w_byte(&w, (uint8_t)(0x80 | (bits - 1)));
        put_table(&w, f->palette, f->npal, bits);
        put_image_data(&w, f->indices, f->n, bits < 2 ? 2 : bits);
    }
    cx_w_byte(&w, 0x3B);
    if (w.st != PC_OK) {
        pc_buf_fini(&b);
        return w.st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

/* ---- WBMP ---- */

static void put_multibyte(cx_w *w, uint32_t v)
{
    uint8_t groups[5];
    int n = 0;
    groups[n++] = (uint8_t)(v & 0x7F);
    v >>= 7;
    while (v > 0) {
        groups[n++] = (uint8_t)((v & 0x7F) | 0x80);
        v >>= 7;
    }
    while (n > 0) {
        cx_w_byte(w, groups[--n]);
    }
}

pc_status pc_web_encode_wbmp(uint32_t width, uint32_t height, const bool *white, size_t n, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    cx_w w = {&b, PC_OK, err};
    size_t stride = ((size_t)width + 7) / 8;
    size_t y;
    uint8_t *row;
    if (width == 0 || height == 0 || n != (size_t)width * height) {
        return cx_invalid(err, "WBMP size must match the pixel data");
    }
    row = malloc(stride);
    if (row == NULL) {
        return cx_nomem(err);
    }
    cx_w_byte(&w, 0);
    cx_w_byte(&w, 0);
    put_multibyte(&w, width);
    put_multibyte(&w, height);
    for (y = 0; y < height; y++) {
        size_t x;
        memset(row, 0, stride);
        for (x = 0; x < width; x++) {
            if (white[y * width + x]) {
                row[x / 8] |= (uint8_t)(0x80 >> (x % 8));
            }
        }
        cx_w_put(&w, row, stride);
    }
    free(row);
    if (w.st != PC_OK) {
        pc_buf_fini(&b);
        return w.st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

pc_status pc_web_decode_wbmp(const uint8_t *b, size_t n, uint32_t *wout, uint32_t *hout, bool **white, pc_error *err)
{
    size_t i = 0;
    uint32_t dims[2];
    int d;
    size_t stride;
    size_t need;
    size_t x;
    size_t y;
    bool *px;
    if (n < 2 || b[0] != 0 || (b[1] & 0x9F) != 0) {
        return cx_invalid(err, "not a type 0 WBMP");
    }
    i = 2;
    for (d = 0; d < 2; d++) {
        uint32_t v = 0;
        int k;
        bool done = false;
        for (k = 0; k < 5; k++) {
            uint8_t c;
            if (i >= n || v > UINT32_MAX / 128) {
                return cx_invalid(err, "not a type 0 WBMP");
            }
            c = b[i++];
            v = v * 128 | (c & 0x7F);
            if ((c & 0x80) == 0) {
                done = true;
                break;
            }
        }
        if (!done) {
            return cx_invalid(err, "not a type 0 WBMP");
        }
        dims[d] = v;
    }
    if (dims[0] == 0 || dims[1] == 0 || (uint64_t)dims[0] * dims[1] > (1ull << 28)) {
        return cx_invalid(err, "not a type 0 WBMP");
    }
    stride = ((size_t)dims[0] + 7) / 8;
    need = stride * dims[1];
    if (n - i < need) {
        return cx_invalid(err, "not a type 0 WBMP");
    }
    px = malloc((size_t)dims[0] * dims[1] * sizeof *px);
    if (px == NULL) {
        return cx_nomem(err);
    }
    for (y = 0; y < dims[1]; y++) {
        for (x = 0; x < dims[0]; x++) {
            px[y * dims[0] + x] = (b[i + y * stride + x / 8] & (0x80 >> (x % 8))) != 0;
        }
    }
    *wout = dims[0];
    *hout = dims[1];
    *white = px;
    return PC_OK;
}

pc_status pc_web_encode_jpeg_rgb8(uint32_t w, uint32_t h, const uint8_t *rgb, size_t n, uint8_t quality,
                                  bool progressive, bool optimized, const uint8_t *icc, size_t icc_len, bool has_dpi,
                                  float dpi, const char *xmp, size_t xmp_len, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    char *ex = NULL;
    size_t exn = 0;
    if (w > 65535 || h > 65535) {
        return cx_encode(err, PC_CF_JPEG, "JPEG dimensions are limited to 65535");
    }
    if (n != (size_t)w * h * 3) {
        return cx_invalid(err, "RGB data does not match the size");
    }
    if (xmp != NULL) {
        /* Its tiff: resolution, if any, says what the JFIF density says (#1691). */
        float d[2] = {dpi, dpi};
        if ((st = pc_export_xmp(xmp, xmp_len, has_dpi && dpi >= 1.0f ? d : NULL, &ex, &exn, err)) != PC_OK) {
            return st;
        }
        if (ex != NULL) {
            xmp = ex;
            xmp_len = exn;
        }
    }
    st = cx_jpeg_encode_rgb8(w, h, rgb, quality, progressive, optimized, icc, icc_len, has_dpi, dpi, xmp, xmp_len, &b,
                             err);
    free(ex);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}
