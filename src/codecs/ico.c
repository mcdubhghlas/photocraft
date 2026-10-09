/* ICO (REWRITE_PLAN.md D24): the best entry (the reference's image crate rule: the last entry,
 * replaced by any with a strictly higher (bits per pixel, area) score), PNG- or BMP-coded.
 * Writing: one PNG-coded RGBA entry of at most 256 x 256. */
#include <string.h>

#include "codecs/internal.h"

#define F PC_CF_ICO

pc_status cx_ico_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    size_t count;
    size_t i;
    size_t best;
    uint32_t bw;
    uint32_t bh;
    uint16_t bbpp;
    uint64_t barea;
    uint32_t size;
    uint32_t off;
    pc_cimage img;
    pc_status st;
    if (n < 6 || cx_le16(b) != 0 || cx_le16(b + 2) != 1) {
        return cx_malformed(err, F, "not an ICO file");
    }
    count = cx_le16(b + 4);
    if (count == 0) {
        return cx_malformed(err, F, "the file has no entries");
    }
    if ((n - 6) / 16 < count) {
        return cx_malformed(err, F, "file ends inside the directory");
    }
#define EW(k) (b[6 + 16 * (k)] == 0 ? 256u : (uint32_t)b[6 + 16 * (k)])
#define EH(k) (b[6 + 16 * (k) + 1] == 0 ? 256u : (uint32_t)b[6 + 16 * (k) + 1])
#define EB(k) cx_le16(b + 6 + 16 * (k) + 6)
    best = count - 1;
    bbpp = EB(best);
    barea = (uint64_t)EW(best) * EH(best);
    for (i = 0; i + 1 < count; i++) {
        uint16_t bp = EB(i);
        uint64_t area = (uint64_t)EW(i) * EH(i);
        if (bp > bbpp || (bp == bbpp && area > barea)) {
            best = i;
            bbpp = bp;
            barea = area;
        }
    }
    bw = EW(best);
    bh = EH(best);
#undef EW
#undef EH
#undef EB
    size = cx_le32(b + 6 + 16 * best + 8);
    off = cx_le32(b + 6 + 16 * best + 12);
    if (off > n || size > n - off) {
        return cx_malformed(err, F, "the entry's data is outside the file");
    }
    if (size >= 8 && memcmp(b + off, "\x89PNG\r\n\x1a\n", 8) == 0) {
        st = cx_png_decode(b + off, size, l, &img, err);
    } else {
        st = cx_bmp_decode_dib(b + off, size, true, l, &img, err);
    }
    if (st != PC_OK) {
        return st;
    }
    if (img.width != bw || img.height != bh) {
        pc_cimage_fini(&img);
        return cx_malformed(err, F, "the entry is %ux%u but its image is %ux%u", (unsigned)bw, (unsigned)bh,
                            (unsigned)img.width, (unsigned)img.height);
    }
    *out = img;
    return PC_OK;
}

pc_status cx_ico_encode(const pc_cimage *src, cx_plan p, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_buf png = PC_BUF_INIT;
    cx_w w = {out, PC_OK, err};
    cx_plan pp;
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    if (img->width > 256 || img->height > 256) {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "dimensions exceed 256x256");
    }
    o.embed_icc = false;
    o.embed_metadata = false;
    pp.layout = img->layout;
    pp.sample = img->sample;
    st = cx_png_encode(img, pp, &o, &png, err);
    if (st == PC_OK) {
        cx_w_le16(&w, 0);
        cx_w_le16(&w, 1);
        cx_w_le16(&w, 1);
        cx_w_byte(&w, (uint8_t)(img->width == 256 ? 0 : img->width));
        cx_w_byte(&w, (uint8_t)(img->height == 256 ? 0 : img->height));
        cx_w_byte(&w, 0);
        cx_w_byte(&w, 0);
        cx_w_le16(&w, 1);
        cx_w_le16(&w, 32);
        cx_w_le32(&w, (uint32_t)png.len);
        cx_w_le32(&w, 22);
        cx_w_put(&w, png.data, png.len);
        st = w.st;
    }
    pc_buf_fini(&png);
    pc_cimage_fini(&tmp);
    return st;
}
