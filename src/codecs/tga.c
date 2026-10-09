/* TGA (REWRITE_PLAN.md D24): colour-mapped, true-colour and gray, raw or RLE, any origin.
 * Colour types follow the reference's image crate: 32-bit (or 24 + 8 attribute bits) RGBA,
 * 24-bit RGB, 15/16-bit RGB (the attribute bit is not alpha), 8-bit gray, 8 + 8 gray + alpha.
 * Writing: RLE, top-left origin, gray / gray+alpha / BGR / BGRA. */
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "codecs/internal.h"

#define F PC_CF_TGA

static pc_status trunc_err(pc_error *err) { return cx_malformed(err, F, "image data is truncated"); }

/* One stored pixel (bpp bytes) to the output layout. */
static void put_px(const uint8_t *s, size_t bpp, unsigned depth, bool color, const uint8_t *cmap, size_t cmap_len,
                   size_t cmap_bpp, unsigned cmap_depth, size_t cmap_first, uint8_t *d, size_t och)
{
    if (cmap != NULL) {
        size_t idx = bpp == 1 ? s[0] : cx_le16(s);
        if (idx < cmap_first || idx - cmap_first >= cmap_len) {
            memset(d, 0, och);
            return;
        }
        s = cmap + (idx - cmap_first) * cmap_bpp;
        bpp = cmap_bpp;
        depth = cmap_depth;
    }
    if (!color) {
        d[0] = s[0];
        if (och == 2) {
            d[1] = bpp > 1 ? s[1] : 255;
        }
        return;
    }
    if (depth == 15 || depth == 16) {
        uint16_t v = cx_le16(s);
        unsigned r = (v >> 10) & 31;
        unsigned g = (v >> 5) & 31;
        unsigned b = v & 31;
        d[0] = (uint8_t)((r << 3) | (r >> 2));
        d[1] = (uint8_t)((g << 3) | (g >> 2));
        d[2] = (uint8_t)((b << 3) | (b >> 2));
        if (och == 4) {
            d[3] = 255;
        }
        return;
    }
    d[0] = s[2];
    d[1] = s[1];
    d[2] = s[0];
    if (och == 4) {
        d[3] = bpp >= 4 ? s[3] : 255;
    }
}

pc_status cx_tga_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    uint8_t id_len;
    uint8_t cmap_type;
    uint8_t type;
    size_t cmap_first;
    size_t cmap_len;
    unsigned cmap_depth;
    uint32_t w;
    uint32_t h;
    unsigned depth;
    uint8_t desc;
    bool rle;
    unsigned base;
    bool color;
    size_t at = 18;
    const uint8_t *cmap = NULL;
    size_t cmap_bpp = 0;
    unsigned attrib;
    unsigned total;
    pc_clayout layout;
    size_t och;
    size_t bpp;
    size_t npx;
    size_t i;
    pc_cimage img;
    uint8_t *tmp;
    pc_status st;
    if (n < 18) {
        return cx_malformed(err, F, "file ends inside the header");
    }
    id_len = b[0];
    cmap_type = b[1];
    type = b[2];
    cmap_first = cx_le16(b + 3);
    cmap_len = cx_le16(b + 5);
    cmap_depth = b[7];
    w = cx_le16(b + 12);
    h = cx_le16(b + 14);
    depth = b[16];
    desc = b[17];
    if (!(type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11)) {
        return cx_unsupported(err, F, "image type %u", (unsigned)type);
    }
    rle = type >= 9;
    base = type & 7;
    color = base != 3;
    at += id_len;
    if (cmap_type == 1) {
        cmap_bpp = (cmap_depth + 7) / 8;
        if (cmap_bpp == 0 || n < at || (n - at) / cmap_bpp < cmap_len) {
            return cx_malformed(err, F, "file ends inside the colour map");
        }
        if (base == 1) {
            cmap = b + at;
        }
        at += cmap_len * cmap_bpp;
    }
    if (base == 1 && (cmap == NULL || (depth != 8 && depth != 16))) {
        return cx_unsupported(err, F, "colour-mapped image without a usable colour map");
    }
    attrib = desc & 0x0F;
    total = cmap != NULL ? cmap_depth : depth;
    if (attrib > total) {
        return cx_malformed(err, F, "More alpha bits than pixel bits");
    }
    if (color && ((attrib == 0 && total == 32) || (attrib == 8 && total == 32))) {
        layout = PC_CL_RGBA;
    } else if (color && attrib == 0 && total == 24) {
        layout = PC_CL_RGB;
    } else if (color && (total == 16 || total == 15) && (attrib <= 1)) {
        layout = PC_CL_RGB;
    } else if (!color && attrib == 8 && total == 16) {
        layout = PC_CL_GRAYA;
    } else if (!color && attrib == 0 && total == 8) {
        layout = PC_CL_GRAY;
    } else if (!color && attrib == 8 && total == 8) {
        layout = PC_CL_GRAY;
    } else {
        return cx_unsupported(err, F, "pixel depth %u with %u attribute bits", total, attrib);
    }
    if ((st = pc_climits_check(l, w, h, layout, PC_CS_U8, err)) != PC_OK) {
        return st;
    }
    och = pc_clayout_channels(layout);
    bpp = (depth + 7) / 8;
    if (bpp == 0) {
        return cx_unsupported(err, F, "pixel depth 0");
    }
    npx = (size_t)w * h;
    if ((st = pc_cimage_new(w, h, layout, PC_CS_U8, &img, err)) != PC_OK) {
        return st;
    }
    tmp = img.data;
    if (at > n) {
        pc_cimage_fini(&img);
        return trunc_err(err);
    }
    if (!rle) {
        size_t need;
        if (!pc_size_mul(npx, bpp, &need) || n - at < need) {
            pc_cimage_fini(&img);
            return trunc_err(err);
        }
        for (i = 0; i < npx; i++) {
            put_px(b + at + i * bpp, bpp, depth, color, cmap, cmap_len, cmap_bpp, cmap_depth, cmap_first,
                   tmp + i * och, och);
        }
    } else {
        i = 0;
        while (i < npx) {
            uint8_t hd;
            size_t cnt;
            size_t k;
            if (at >= n) {
                pc_cimage_fini(&img);
                return trunc_err(err);
            }
            hd = b[at++];
            cnt = (size_t)(hd & 0x7F) + 1;
            if (cnt > npx - i) {
                cnt = npx - i;
            }
            if (hd & 0x80) {
                if (n - at < bpp) {
                    pc_cimage_fini(&img);
                    return trunc_err(err);
                }
                for (k = 0; k < cnt; k++) {
                    put_px(b + at, bpp, depth, color, cmap, cmap_len, cmap_bpp, cmap_depth, cmap_first,
                           tmp + (i + k) * och, och);
                }
                at += bpp;
            } else {
                if ((n - at) / bpp < cnt) {
                    pc_cimage_fini(&img);
                    return trunc_err(err);
                }
                for (k = 0; k < cnt; k++) {
                    put_px(b + at + k * bpp, bpp, depth, color, cmap, cmap_len, cmap_bpp, cmap_depth, cmap_first,
                           tmp + (i + k) * och, och);
                }
                at += cnt * bpp;
            }
            i += cnt;
        }
    }
    /* Bottom-left origin unless bit 5 is set; bit 4 flips horizontally. */
    if (!(desc & 0x20)) {
        size_t row = (size_t)w * och;
        uint8_t *t = malloc(row > 0 ? row : 1);
        size_t y;
        if (t == NULL) {
            pc_cimage_fini(&img);
            return cx_nomem(err);
        }
        for (y = 0; y < h / 2; y++) {
            memcpy(t, tmp + y * row, row);
            memcpy(tmp + y * row, tmp + (h - 1 - y) * row, row);
            memcpy(tmp + (h - 1 - y) * row, t, row);
        }
        free(t);
    }
    if (desc & 0x10) {
        size_t y;
        for (y = 0; y < h; y++) {
            size_t x;
            for (x = 0; x < w / 2; x++) {
                uint8_t t[4];
                uint8_t *a = tmp + (y * w + x) * och;
                uint8_t *c = tmp + (y * w + w - 1 - x) * och;
                memcpy(t, a, och);
                memcpy(a, c, och);
                memcpy(c, t, och);
            }
        }
    }
    *out = img;
    return PC_OK;
}

pc_status cx_tga_encode(const pc_cimage *src, cx_plan p, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    cx_w w = {out, PC_OK, err};
    size_t och;
    size_t y;
    uint8_t hdr[18];
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    if (img->width > 65535 || img->height > 65535) {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "dimensions exceed 65535x65535");
    }
    och = pc_clayout_channels(img->layout);
    memset(hdr, 0, sizeof hdr);
    hdr[2] = pc_clayout_is_gray(img->layout) ? 11 : 10;
    hdr[12] = (uint8_t)img->width;
    hdr[13] = (uint8_t)(img->width >> 8);
    hdr[14] = (uint8_t)img->height;
    hdr[15] = (uint8_t)(img->height >> 8);
    hdr[16] = (uint8_t)(och * 8);
    hdr[17] = (uint8_t)((pc_clayout_has_alpha(img->layout) ? 8 : 0) | 0x20); /* top-left origin */
    cx_w_put(&w, hdr, 18);
    for (y = 0; y < img->height; y++) {
        const uint8_t *row = img->data + y * (size_t)img->width * och;
        size_t x = 0;
        while (x < img->width) {
            size_t run = 1;
            while (x + run < img->width && run < 128 && memcmp(row + x * och, row + (x + run) * och, och) == 0) {
                run++;
            }
            if (run >= 2) {
                uint8_t px[4];
                cx_w_byte(&w, (uint8_t)(0x80 | (run - 1)));
                if (och >= 3) {
                    px[0] = row[x * och + 2];
                    px[1] = row[x * och + 1];
                    px[2] = row[x * och];
                    px[3] = och == 4 ? row[x * och + 3] : 0;
                } else {
                    memcpy(px, row + x * och, och);
                }
                cx_w_put(&w, px, och);
                x += run;
            } else {
                size_t start = x;
                size_t k;
                x++;
                while (x < img->width && x - start < 128 &&
                       !(x + 1 < img->width && memcmp(row + x * och, row + (x + 1) * och, och) == 0)) {
                    x++;
                }
                cx_w_byte(&w, (uint8_t)(x - start - 1));
                for (k = start; k < x; k++) {
                    uint8_t px[4];
                    if (och >= 3) {
                        px[0] = row[k * och + 2];
                        px[1] = row[k * och + 1];
                        px[2] = row[k * och];
                        px[3] = och == 4 ? row[k * och + 3] : 0;
                    } else {
                        memcpy(px, row + k * och, och);
                    }
                    cx_w_put(&w, px, och);
                }
            }
        }
    }
    pc_cimage_fini(&tmp);
    return w.st;
}
