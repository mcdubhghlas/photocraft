/* ZSoft PCX (Paintbrush) reader (port of crates/codecs/src/codecs/pcx.rs; own code per
 * REWRITE_PLAN.md D24): strictly bounded scanline RLE, 16/256-colour palettes and 8-bit
 * three-plane RGB, decoded to RGB U8. Read-only: modern saves should use PNG. */
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "codecs/internal.h"

#define F PC_CF_PCX
#define HEADER 128

bool cx_pcx_looks_like(const uint8_t *b, size_t n)
{
    unsigned bits;
    unsigned planes;
    uint32_t stride;
    uint16_t x0;
    uint16_t y0;
    uint16_t x1;
    uint16_t y1;
    uint32_t width;
    uint32_t min_stride;
    if (n < HEADER || b[0] != 0x0a || !(b[1] == 0 || b[1] == 2 || b[1] == 3 || b[1] == 5) || b[2] != 1) {
        return false;
    }
    bits = b[3];
    planes = b[65];
    stride = cx_le16(b + 66);
    x0 = cx_le16(b + 4);
    y0 = cx_le16(b + 6);
    x1 = cx_le16(b + 8);
    y1 = cx_le16(b + 10);
    if (x1 < x0 || y1 < y0 || !(bits == 1 || bits == 2 || bits == 4 || bits == 8) || planes < 1 ||
        planes > 4) {
        return false;
    }
    width = (uint32_t)(x1 - x0) + 1;
    min_stride = (width * bits + 7) / 8;
    return stride >= min_stride && stride > 0 && (bits != 8 || planes == 1 || planes == 3) &&
           (bits == 8 || bits * planes <= 4);
}

/* One compressed plane row; RLE packets never cross a scanline. */
static pc_status read_row(const uint8_t *b, size_t end, size_t *pos, uint8_t *out, size_t len, pc_error *err)
{
    size_t n = 0;
    while (n < len) {
        uint8_t code;
        size_t count = 1;
        uint8_t value;
        if (*pos >= end) {
            return cx_malformed(err, F, "truncated pixel RLE");
        }
        code = b[(*pos)++];
        value = code;
        if (code >= 0xc0) {
            count = code & 0x3f;
            if (*pos >= end) {
                return cx_malformed(err, F, "truncated RLE packet");
            }
            value = b[(*pos)++];
        }
        if (count == 0 || count > len - n) {
            return cx_malformed(err, F, "RLE packet crosses a scanline or has zero length");
        }
        memset(out + n, value, count);
        n += count;
    }
    return PC_OK;
}

pc_status cx_pcx_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    uint32_t w;
    uint32_t h;
    size_t bits;
    size_t planes;
    size_t stride;
    size_t row_len;
    size_t total;
    bool vga;
    size_t image_end;
    uint8_t *row = NULL;
    uint8_t *pixels = NULL;
    size_t p = HEADER;
    size_t x;
    size_t y;
    pc_cimage img;
    pc_status st;
    if (!cx_pcx_looks_like(b, n)) {
        return cx_malformed(err, F, "invalid PCX header or unsupported pixel layout");
    }
    w = (uint32_t)(cx_le16(b + 8) - cx_le16(b + 4)) + 1;
    h = (uint32_t)(cx_le16(b + 10) - cx_le16(b + 6)) + 1;
    if ((st = pc_climits_check(l, w, h, PC_CL_RGB, PC_CS_U8, err)) != PC_OK) {
        return st;
    }
    bits = b[3];
    planes = b[65];
    stride = cx_le16(b + 66);
    if (!pc_size_mul(stride, planes, &row_len)) {
        return cx_malformed(err, F, "row size overflow");
    }
    if (!pc_size_mul3(w, h, 3, &total)) {
        return cx_malformed(err, F, "pixel size overflow");
    }
    if (row_len == 0 || total == 0) {
        return cx_malformed(err, F, "invalid PCX header or unsupported pixel layout");
    }
    /* The last 769 bytes are the optional VGA palette of an 8-bit indexed image. */
    vga = bits == 8 && planes == 1 && n >= HEADER + 769 && b[n - 769] == 0x0c;
    if (bits == 8 && planes == 1 && !vga && cx_le16(b + 68) != 2) {
        return cx_malformed(err, F, "8-bit indexed PCX has no 256-colour palette");
    }
    image_end = vga ? n - 769 : n;
    row = calloc(row_len, 1);
    pixels = malloc(total);
    if (row == NULL || pixels == NULL) {
        free(row);
        free(pixels);
        return cx_nomem(err);
    }
    for (y = 0; y < h; y++) {
        size_t k;
        for (k = 0; k < planes; k++) {
            if ((st = read_row(b, image_end, &p, row + k * stride, stride, err)) != PC_OK) {
                free(row);
                free(pixels);
                return st;
            }
        }
        for (x = 0; x < w; x++) {
            uint8_t *o = pixels + (y * w + x) * 3;
            if (bits == 8 && planes == 3) {
                o[0] = row[x];
                o[1] = row[stride + x];
                o[2] = row[2 * stride + x];
            } else {
                size_t index;
                if (bits == 8) {
                    index = row[x];
                } else {
                    size_t bit = x * bits;
                    unsigned shift = (unsigned)(8 - bits - bit % 8);
                    size_t mask = ((size_t)1 << bits) - 1;
                    index = 0;
                    for (k = 0; k < planes; k++) {
                        size_t sample = ((size_t)row[k * stride + bit / 8] >> shift) & mask;
                        index |= sample << (k * bits);
                    }
                }
                if (vga) {
                    memcpy(o, b + image_end + 1 + index * 3, 3);
                } else if (bits == 8) {
                    o[0] = o[1] = o[2] = (uint8_t)index; /* palette info 2: gray scale */
                } else {
                    memcpy(o, b + 16 + index * 3, 3);
                }
            }
        }
    }
    free(row);
    if ((st = pc_cimage_from_raw(w, h, PC_CL_RGB, PC_CS_U8, pixels, total, &img, err)) != PC_OK) {
        return st;
    }
    if (cx_le16(b + 12) > 0 && cx_le16(b + 14) > 0) {
        img.meta.has_dpi = true;
        img.meta.dpi_x = (float)cx_le16(b + 12);
        img.meta.dpi_y = (float)cx_le16(b + 14);
    }
    *out = img;
    return PC_OK;
}
