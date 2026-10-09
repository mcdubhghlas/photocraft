/* QOI (REWRITE_PLAN.md D24), per the QOI specification 1.0: 3 or 4 channels; the 8-byte end
 * padding is required, as the reference's qoi crate does. */
#include <string.h>

#include "base/num.h"
#include "codecs/internal.h"

#define F PC_CF_QOI

static unsigned hash(const uint8_t *p) { return (unsigned)(p[0] * 3 + p[1] * 5 + p[2] * 7 + p[3] * 11) % 64; }

static const uint8_t PADDING[8] = {0, 0, 0, 0, 0, 0, 0, 1};

pc_status cx_qoi_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    uint32_t w;
    uint32_t h;
    uint8_t ch;
    pc_clayout layout;
    uint8_t index[64][4];
    uint8_t px[4] = {0, 0, 0, 255};
    size_t npx;
    size_t i = 14;
    size_t p = 0;
    pc_cimage img;
    pc_status st;
    if (n < 14 || memcmp(b, "qoif", 4) != 0) {
        return cx_malformed(err, F, "invalid magic");
    }
    w = cx_be32(b + 4);
    h = cx_be32(b + 8);
    ch = b[12];
    if (ch != 3 && ch != 4) {
        return cx_malformed(err, F, "invalid number of channels: %u", (unsigned)ch);
    }
    if (b[13] > 1) {
        return cx_malformed(err, F, "invalid color space: %u", (unsigned)b[13]);
    }
    layout = ch == 4 ? PC_CL_RGBA : PC_CL_RGB;
    if ((st = pc_climits_check(l, w, h, layout, PC_CS_U8, err)) != PC_OK) {
        return st;
    }
    npx = (size_t)w * h;
    if ((st = pc_cimage_new(w, h, layout, PC_CS_U8, &img, err)) != PC_OK) {
        return st;
    }
    memset(index, 0, sizeof index);
    while (p < npx) {
        uint8_t op;
        size_t run = 1;
        if (i >= n) {
            pc_cimage_fini(&img);
            return cx_malformed(err, F, "unexpected buffer end");
        }
        op = b[i++];
        if (op == 0xFE || op == 0xFF) {
            size_t k = op == 0xFE ? 3 : 4;
            if (n - i < k) {
                pc_cimage_fini(&img);
                return cx_malformed(err, F, "unexpected buffer end");
            }
            memcpy(px, b + i, k);
            i += k;
        } else if ((op & 0xC0) == 0x00) {
            memcpy(px, index[op], 4);
        } else if ((op & 0xC0) == 0x40) {
            px[0] = (uint8_t)(px[0] + ((op >> 4) & 3) - 2);
            px[1] = (uint8_t)(px[1] + ((op >> 2) & 3) - 2);
            px[2] = (uint8_t)(px[2] + (op & 3) - 2);
        } else if ((op & 0xC0) == 0x80) {
            int dg;
            uint8_t b2;
            if (i >= n) {
                pc_cimage_fini(&img);
                return cx_malformed(err, F, "unexpected buffer end");
            }
            b2 = b[i++];
            dg = (op & 0x3F) - 32;
            px[0] = (uint8_t)(px[0] + dg - 8 + ((b2 >> 4) & 15));
            px[1] = (uint8_t)(px[1] + dg);
            px[2] = (uint8_t)(px[2] + dg - 8 + (b2 & 15));
        } else {
            run = (size_t)(op & 0x3F) + 1;
        }
        memcpy(index[hash(px)], px, 4);
        while (run-- > 0 && p < npx) {
            memcpy(img.data + p * ch, px, ch);
            p++;
        }
    }
    if (n - i < 8) {
        pc_cimage_fini(&img);
        return cx_malformed(err, F, "unexpected buffer end");
    }
    if (memcmp(b + i, PADDING, 8) != 0) {
        pc_cimage_fini(&img);
        return cx_malformed(err, F, "invalid padding (stream end marker mismatch)");
    }
    *out = img;
    return PC_OK;
}

pc_status cx_qoi_encode(const pc_cimage *src, cx_plan p, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    cx_w w = {out, PC_OK, err};
    uint8_t index[64][4];
    uint8_t prev[4] = {0, 0, 0, 255};
    size_t ch;
    size_t npx;
    size_t i;
    unsigned run = 0;
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    ch = pc_clayout_channels(img->layout);
    npx = pc_cimage_pixel_count(img);
    memset(index, 0, sizeof index);
    cx_w_put(&w, "qoif", 4);
    cx_w_be32(&w, img->width);
    cx_w_be32(&w, img->height);
    cx_w_byte(&w, (uint8_t)ch);
    cx_w_byte(&w, 0);
    for (i = 0; i < npx; i++) {
        uint8_t px[4] = {0, 0, 0, 255};
        memcpy(px, img->data + i * ch, ch);
        if (memcmp(px, prev, 4) == 0) {
            run++;
            if (run == 62 || i + 1 == npx) {
                cx_w_byte(&w, (uint8_t)(0xC0 | (run - 1)));
                run = 0;
            }
            continue;
        }
        if (run > 0) {
            cx_w_byte(&w, (uint8_t)(0xC0 | (run - 1)));
            run = 0;
        }
        {
            unsigned hsh = hash(px);
            if (memcmp(index[hsh], px, 4) == 0) {
                cx_w_byte(&w, (uint8_t)hsh);
            } else {
                memcpy(index[hsh], px, 4);
                if (px[3] == prev[3]) {
                    int dr = (int8_t)(uint8_t)(px[0] - prev[0]);
                    int dg = (int8_t)(uint8_t)(px[1] - prev[1]);
                    int db = (int8_t)(uint8_t)(px[2] - prev[2]);
                    int dr_dg = dr - dg;
                    int db_dg = db - dg;
                    if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                        cx_w_byte(&w, (uint8_t)(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
                    } else if (dg >= -32 && dg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
                        cx_w_byte(&w, (uint8_t)(0x80 | (dg + 32)));
                        cx_w_byte(&w, (uint8_t)(((dr_dg + 8) << 4) | (db_dg + 8)));
                    } else {
                        cx_w_byte(&w, 0xFE);
                        cx_w_put(&w, px, 3);
                    }
                } else {
                    cx_w_byte(&w, 0xFF);
                    cx_w_put(&w, px, 4);
                }
            }
        }
        memcpy(prev, px, 4);
    }
    cx_w_put(&w, PADDING, 8);
    pc_cimage_fini(&tmp);
    return w.st;
}
