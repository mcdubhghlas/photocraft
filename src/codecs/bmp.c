/* BMP (REWRITE_PLAN.md D24): BITMAPCOREHEADER through BITMAPV5HEADER; 1/4/8-bit palettes,
 * RLE8 and RLE4, 16/24/32-bit with default or BITFIELDS masks, bottom-up or top-down. Output is
 * RGB, or RGBA when a 16/32-bit file has an alpha mask (and always for ICO entries, whose AND
 * mask gives the transparency), as the reference's image crate does. Writing: 24-bit BI_RGB
 * for RGB, 32-bit BITFIELDS with an alpha mask (BITMAPV4HEADER) for RGBA. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "codecs/internal.h"

#define F PC_CF_BMP

static pc_status trunc_err(pc_error *err, pc_cformat f) { return cx_malformed(err, f, "image data is truncated"); }

static unsigned mask_shift(uint32_t m)
{
    unsigned s = 0;
    if (m == 0) {
        return 0;
    }
    while ((m & 1) == 0) {
        m >>= 1;
        s++;
    }
    return s;
}

static unsigned mask_len(uint32_t m)
{
    unsigned n = 0;
    m >>= mask_shift(m);
    while (m & 1) {
        m >>= 1;
        n++;
    }
    return n;
}

static uint8_t field(uint32_t v, uint32_t m)
{
    unsigned len = mask_len(m);
    uint32_t x;
    uint32_t max;
    if (m == 0 || len == 0) {
        return 0;
    }
    x = (v & m) >> mask_shift(m);
    if (len >= 8) {
        return (uint8_t)(x >> (len - 8));
    }
    max = (1u << len) - 1;
    return (uint8_t)((x * 255 + max / 2) / max);
}

/* Decodes a DIB starting at b[dib], with pixel data at data_off (SIZE_MAX: right after the
 * palette, as in ICO entries). */
static pc_status decode(const uint8_t *b, size_t n, size_t dib, size_t data_off, bool ico, pc_cformat fmt,
                        const pc_climits *l, pc_cimage *out, pc_error *err)
{
    uint32_t hsize;
    int64_t w;
    int64_t h;
    uint16_t bpp;
    uint32_t comp = 0;
    uint32_t clr_used = 0;
    bool core;
    bool top_down = false;
    uint32_t masks[4] = {0, 0, 0, 0};
    size_t pal_at;
    size_t pal_entry;
    size_t npal = 0;
    const uint8_t *pal = NULL;
    bool alpha;
    size_t stride;
    size_t x;
    size_t y;
    uint32_t uw;
    uint32_t uh;
    pc_cimage img;
    pc_status st;
    if (n - dib < 4) {
        return trunc_err(err, fmt);
    }
    hsize = cx_le32(b + dib);
    if (!(hsize == 12 || hsize == 16 || hsize == 40 || hsize == 52 || hsize == 56 || hsize == 64 || hsize == 108 ||
          hsize == 124)) {
        return cx_unsupported(err, fmt, "BMP header size %u", (unsigned)hsize);
    }
    if (n - dib < hsize) {
        return trunc_err(err, fmt);
    }
    core = hsize == 12;
    if (core) {
        w = cx_le16(b + dib + 4);
        h = cx_le16(b + dib + 6);
        bpp = cx_le16(b + dib + 10);
    } else {
        w = (int32_t)cx_le32(b + dib + 4);
        h = (int32_t)cx_le32(b + dib + 8);
        bpp = cx_le16(b + dib + 14);
        if (hsize >= 20) {
            comp = cx_le32(b + dib + 16);
        }
        if (hsize >= 36) {
            clr_used = cx_le32(b + dib + 32);
        }
    }
    if (h < 0) {
        top_down = true;
        h = -h;
    }
    if (ico) {
        h /= 2; /* the XOR image, then the AND mask */
    }
    if (w <= 0 || h <= 0 || w > UINT32_MAX || h > UINT32_MAX) {
        return cx_malformed(err, fmt, "invalid image size %lldx%lld", (long long)w, (long long)h);
    }
    uw = (uint32_t)w;
    uh = (uint32_t)h;
    if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) {
        return cx_unsupported(err, fmt, "%u bits per pixel", (unsigned)bpp);
    }
    if (!(comp == 0 || (comp == 1 && bpp == 8) || (comp == 2 && bpp == 4) ||
          ((comp == 3 || comp == 6) && (bpp == 16 || bpp == 32)))) {
        return cx_unsupported(err, fmt, "compression %u with %u bits per pixel", (unsigned)comp, (unsigned)bpp);
    }
    pal_at = dib + hsize;
    if (comp == 3 || comp == 6) {
        if (hsize == 40) {
            size_t k = comp == 6 ? 4 : 3;
            size_t i;
            if (n - pal_at < 4 * k) {
                return trunc_err(err, fmt);
            }
            for (i = 0; i < k; i++) {
                masks[i] = cx_le32(b + pal_at + 4 * i);
            }
            pal_at += 4 * k;
        } else {
            masks[0] = cx_le32(b + dib + 40);
            masks[1] = cx_le32(b + dib + 44);
            masks[2] = cx_le32(b + dib + 48);
            masks[3] = hsize >= 56 ? cx_le32(b + dib + 52) : 0;
        }
    } else if (bpp == 16) {
        masks[0] = 0x7C00;
        masks[1] = 0x03E0;
        masks[2] = 0x001F;
    } else if (bpp == 32) {
        masks[0] = 0x00FF0000;
        masks[1] = 0x0000FF00;
        masks[2] = 0x000000FF;
    }
    pal_entry = core ? 3 : 4;
    if (bpp <= 8) {
        npal = clr_used == 0 || clr_used > (1u << bpp) ? (size_t)1 << bpp : clr_used;
        if ((n - pal_at) / pal_entry < npal) {
            return trunc_err(err, fmt);
        }
        pal = b + pal_at;
    }
    if (data_off == SIZE_MAX) {
        data_off = pal_at + npal * pal_entry;
    }
    alpha = ico || ((bpp == 16 || bpp == 32) && masks[3] != 0);
    if ((st = pc_climits_check(l, uw, uh, alpha ? PC_CL_RGBA : PC_CL_RGB, PC_CS_U8, err)) != PC_OK) {
        return st;
    }
    stride = (((size_t)uw * bpp + 31) / 32) * 4;
    if ((st = pc_cimage_new(uw, uh, alpha ? PC_CL_RGBA : PC_CL_RGB, PC_CS_U8, &img, err)) != PC_OK) {
        return st;
    }
    if (data_off > n) {
        pc_cimage_fini(&img);
        return trunc_err(err, fmt);
    }
    {
        size_t ch = alpha ? 4 : 3;
        const uint8_t *d = b + data_off;
        size_t dn = n - data_off;
#define OUTPX(xx, yy) (img.data + (((top_down ? (yy) : uh - 1 - (yy))) * (size_t)uw + (xx)) * ch)
        if (comp == 1 || comp == 2) {
            /* RLE: missing pixels stay black (transparent with alpha) */
            size_t i = 0;
            size_t xx = 0;
            size_t yy = 0;
            bool rle4 = comp == 2;
            while (yy < uh) {
                uint8_t c0;
                uint8_t c1;
                if (dn - i < 2) {
                    pc_cimage_fini(&img);
                    return trunc_err(err, fmt);
                }
                c0 = d[i];
                c1 = d[i + 1];
                i += 2;
                if (c0 > 0) {
                    size_t k;
                    for (k = 0; k < c0; k++) {
                        uint8_t idx = rle4 ? (uint8_t)(k % 2 == 0 ? c1 >> 4 : c1 & 15) : c1;
                        if (xx < uw && idx < npal) {
                            uint8_t *p = OUTPX(xx, yy);
                            p[0] = pal[idx * pal_entry + 2];
                            p[1] = pal[idx * pal_entry + 1];
                            p[2] = pal[idx * pal_entry];
                            if (alpha) {
                                p[3] = 255;
                            }
                        }
                        xx++;
                    }
                } else if (c1 == 0) {
                    xx = 0;
                    yy++;
                } else if (c1 == 1) {
                    break;
                } else if (c1 == 2) {
                    if (dn - i < 2) {
                        pc_cimage_fini(&img);
                        return trunc_err(err, fmt);
                    }
                    xx += d[i];
                    yy += d[i + 1];
                    i += 2;
                } else {
                    size_t k;
                    size_t bytes = rle4 ? ((size_t)c1 + 1) / 2 : c1;
                    if (dn - i < bytes) {
                        pc_cimage_fini(&img);
                        return trunc_err(err, fmt);
                    }
                    for (k = 0; k < c1; k++) {
                        uint8_t idx = rle4 ? (uint8_t)(k % 2 == 0 ? d[i + k / 2] >> 4 : d[i + k / 2] & 15) : d[i + k];
                        if (xx < uw && yy < uh && idx < npal) {
                            uint8_t *p = OUTPX(xx, yy);
                            p[0] = pal[idx * pal_entry + 2];
                            p[1] = pal[idx * pal_entry + 1];
                            p[2] = pal[idx * pal_entry];
                            if (alpha) {
                                p[3] = 255;
                            }
                        }
                        xx++;
                    }
                    i += (bytes + 1) & ~(size_t)1; /* runs are word aligned */
                }
            }
        } else {
            size_t need;
            if (!pc_size_mul(stride, uh, &need) || dn < need) {
                pc_cimage_fini(&img);
                return trunc_err(err, fmt);
            }
            for (y = 0; y < uh; y++) {
                const uint8_t *row = d + y * stride;
                for (x = 0; x < uw; x++) {
                    uint8_t *p = OUTPX(x, y);
                    if (bpp <= 8) {
                        size_t bit = x * bpp;
                        uint8_t idx = (uint8_t)(((unsigned)row[bit / 8] >> (8 - bpp - bit % 8)) & ((1u << bpp) - 1));
                        if (idx < npal) {
                            p[0] = pal[idx * pal_entry + 2];
                            p[1] = pal[idx * pal_entry + 1];
                            p[2] = pal[idx * pal_entry];
                        }
                        if (alpha) {
                            p[3] = 255;
                        }
                    } else if (bpp == 24) {
                        p[0] = row[3 * x + 2];
                        p[1] = row[3 * x + 1];
                        p[2] = row[3 * x];
                        if (alpha) {
                            p[3] = 255;
                        }
                    } else {
                        uint32_t v = bpp == 16 ? cx_le16(row + 2 * x) : cx_le32(row + 4 * x);
                        p[0] = field(v, masks[0]);
                        p[1] = field(v, masks[1]);
                        p[2] = field(v, masks[2]);
                        if (alpha) {
                            p[3] = masks[3] != 0 ? field(v, masks[3]) : (ico && bpp == 32 ? row[4 * x + 3] : 255);
                        }
                    }
                }
            }
            if (ico && bpp != 32) {
                /* the AND mask: 1 bit per pixel, rows padded to 4 bytes; a set bit is transparent */
                size_t mstride = (((size_t)uw + 31) / 32) * 4;
                const uint8_t *m = d + need;
                size_t mneed;
                if (pc_size_mul(mstride, uh, &mneed) && dn - need >= mneed) {
                    for (y = 0; y < uh; y++) {
                        for (x = 0; x < uw; x++) {
                            if ((m[y * mstride + x / 8] >> (7 - x % 8)) & 1) {
                                OUTPX(x, y)[3] = 0;
                            }
                        }
                    }
                }
            }
        }
#undef OUTPX
    }
    *out = img;
    return PC_OK;
}

pc_status cx_bmp_decode_dib(const uint8_t *b, size_t n, bool ico, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    return decode(b, n, 0, SIZE_MAX, ico, PC_CF_ICO, l, out, err);
}

pc_status cx_bmp_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    uint32_t off;
    if (n < 18 || b[0] != 'B' || b[1] != 'M') {
        return cx_malformed(err, F, "not a BMP file");
    }
    off = cx_le32(b + 10);
    return decode(b, n, 14, off, false, F, l, out, err);
}

pc_status cx_bmp_encode(const pc_cimage *src, cx_plan p, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    cx_w w = {out, PC_OK, err};
    bool rgba;
    size_t bpp;
    size_t stride;
    size_t hsize;
    size_t data_size;
    size_t y;
    size_t x;
    uint8_t pad[3] = {0, 0, 0};
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    rgba = img->layout == PC_CL_RGBA;
    if (img->layout != PC_CL_RGB && !rgba) {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "unsupported layout %s", pc_clayout_name(img->layout));
    }
    bpp = rgba ? 32 : 24;
    stride = (((size_t)img->width * bpp + 31) / 32) * 4;
    hsize = rgba ? 108 : 40;
    if (!pc_size_mul(stride, img->height, &data_size) || data_size > UINT32_MAX - 14 - hsize) {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "image too large for BMP");
    }
    cx_w_put(&w, "BM", 2);
    cx_w_le32(&w, (uint32_t)(14 + hsize + data_size));
    cx_w_le32(&w, 0);
    cx_w_le32(&w, (uint32_t)(14 + hsize));
    cx_w_le32(&w, (uint32_t)hsize);
    cx_w_le32(&w, img->width);
    cx_w_le32(&w, img->height); /* bottom-up */
    cx_w_le16(&w, 1);
    cx_w_le16(&w, (uint16_t)bpp);
    cx_w_le32(&w, rgba ? 3 : 0);
    cx_w_le32(&w, (uint32_t)data_size);
    cx_w_le32(&w, 2835); /* 72 dpi */
    cx_w_le32(&w, 2835);
    cx_w_le32(&w, 0);
    cx_w_le32(&w, 0);
    if (rgba) {
        uint8_t rest[108 - 56];
        cx_w_le32(&w, 0x00FF0000);
        cx_w_le32(&w, 0x0000FF00);
        cx_w_le32(&w, 0x000000FF);
        cx_w_le32(&w, 0xFF000000);
        memset(rest, 0, sizeof rest);
        memcpy(rest, "BGRs", 4); /* LCS_sRGB */
        cx_w_put(&w, rest, sizeof rest);
    }
    for (y = img->height; y-- > 0;) {
        const uint8_t *row = img->data + y * (size_t)img->width * (rgba ? 4 : 3);
        for (x = 0; x < img->width; x++) {
            const uint8_t *px = row + x * (rgba ? 4 : 3);
            uint8_t o[4] = {px[2], px[1], px[0], rgba ? px[3] : 0};
            cx_w_put(&w, o, rgba ? 4 : 3);
        }
        cx_w_put(&w, pad, stride - (size_t)img->width * (bpp / 8));
    }
    pc_cimage_fini(&tmp);
    return w.st;
}

pc_status pc_cbmp_palette(const uint8_t *b, size_t n, bool *has, uint8_t (**palette)[3], size_t *npal,
                          pc_error *err)
{
    size_t dib = 14;
    uint32_t hsize;
    uint16_t bpp;
    uint32_t comp = 0;
    uint32_t used = 0;
    size_t entry;
    size_t count;
    size_t avail;
    size_t at;
    size_t i;
    uint8_t (*pal)[3];
    *has = false;
    *palette = NULL;
    *npal = 0;
    if (n < 18 || b[0] != 'B' || b[1] != 'M') {
        return cx_malformed(err, F, "BMP signature not found");
    }
    hsize = cx_le32(b + dib);
    if (!(hsize == 12 || hsize == 16 || hsize == 40 || hsize == 52 || hsize == 56 || hsize == 64 ||
          hsize == 108 || hsize == 124)) {
        return cx_unsupported(err, F, "BMP header size %u", (unsigned)hsize);
    }
    if (n - dib < hsize) {
        return trunc_err(err, F);
    }
    if (hsize == 12) {
        bpp = cx_le16(b + dib + 10);
    } else {
        bpp = cx_le16(b + dib + 14);
        if (hsize >= 20) {
            comp = cx_le32(b + dib + 16);
        }
        if (hsize >= 36) {
            used = cx_le32(b + dib + 32);
        }
    }
    /* Only indexed images (1/4/8-bit RGB, RLE8, RLE4) carry a colour table. */
    if (!(bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8) ||
        !(comp == 0 || (comp == 1 && bpp == 8) || (comp == 2 && bpp == 4))) {
        return PC_OK;
    }
    if (used > (1u << bpp)) {
        return cx_malformed(err, F, "palette size %u exceeds the maximum for %u bits per pixel",
                            (unsigned)used, (unsigned)bpp);
    }
    entry = hsize == 12 ? 3 : 4;
    count = used == 0 ? (size_t)1 << bpp : used;
    at = dib + hsize;
    avail = n - at;
    if (avail / entry < count) {
        return trunc_err(err, F);
    }
    pal = calloc(256, sizeof *pal);
    if (pal == NULL) {
        return cx_nomem(err);
    }
    for (i = 0; i < count; i++) {
        pal[i][0] = b[at + i * entry + 2];
        pal[i][1] = b[at + i * entry + 1];
        pal[i][2] = b[at + i * entry];
    }
    *has = true;
    *palette = pal;
    *npal = 256;
    return PC_OK;
}
