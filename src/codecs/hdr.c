/* Radiance RGBE (.hdr) (REWRITE_PLAN.md D24): "#?RADIANCE" / "#?RGBE", 32-bit_rle_rgbe, the
 * "-Y h +X w" orientation, flat, old-RLE-free and new RLE scanlines. Decoding gives RGB f32
 * (c * 2^(e - 136), as the reference's image crate); writing uses new-style RLE scanlines. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "base/rmath.h"
#include "codecs/internal.h"

#define F PC_CF_HDR

/* Reads one header line (without its newline); false at the end of the data. */
static bool line(const uint8_t *b, size_t n, size_t *at, const uint8_t **s, size_t *len)
{
    const uint8_t *nl;
    if (*at >= n) {
        return false;
    }
    nl = memchr(b + *at, '\n', n - *at);
    if (nl == NULL) {
        return false;
    }
    *s = b + *at;
    *len = (size_t)(nl - (b + *at));
    *at = (size_t)(nl - b) + 1;
    return true;
}

static bool parse_uint(const uint8_t *s, size_t n, size_t *i, uint32_t *v)
{
    uint64_t x = 0;
    size_t start = *i;
    while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
        x = x * 10 + (uint64_t)(s[*i] - '0');
        if (x > UINT32_MAX) {
            return false;
        }
        (*i)++;
    }
    *v = (uint32_t)x;
    return *i > start;
}

static void to_float(const uint8_t *rgbe, float *o)
{
    if (rgbe[3] == 0) {
        o[0] = o[1] = o[2] = 0.0f;
        return;
    }
    o[0] = ldexpf((float)rgbe[0], (int)rgbe[3] - 136);
    o[1] = ldexpf((float)rgbe[1], (int)rgbe[3] - 136);
    o[2] = ldexpf((float)rgbe[2], (int)rgbe[3] - 136);
}

pc_status cx_hdr_decode(const uint8_t *b, size_t n, const pc_climits *l, pc_cimage *out, pc_error *err)
{
    size_t at = 0;
    const uint8_t *s;
    size_t len;
    uint32_t w = 0;
    uint32_t h = 0;
    uint8_t *scan = NULL;
    float *px;
    size_t y;
    pc_cimage img;
    pc_status st;
    if (!line(b, n, &at, &s, &len) ||
        !((len >= 10 && memcmp(s, "#?RADIANCE", 10) == 0) || (len >= 6 && memcmp(s, "#?RGBE", 6) == 0))) {
        return cx_malformed(err, F, "not a Radiance HDR file");
    }
    for (;;) {
        if (!line(b, n, &at, &s, &len)) {
            return cx_malformed(err, F, "file ends inside the header");
        }
        if (len == 0) {
            break;
        }
        if (len >= 7 && memcmp(s, "FORMAT=", 7) == 0 && !(len - 7 >= 15 && memcmp(s + 7, "32-bit_rle_rgbe", 15) == 0)) {
            return cx_unsupported(err, F, "pixel format %.*s", (int)(len - 7 > 40 ? 40 : len - 7), s + 7);
        }
    }
    if (!line(b, n, &at, &s, &len)) {
        return cx_malformed(err, F, "file ends before the image size");
    }
    {
        size_t i = 0;
        if (len < 3 || memcmp(s, "-Y ", 3) != 0) {
            return cx_unsupported(err, F, "image orientation other than -Y +X");
        }
        i = 3;
        if (!parse_uint(s, len, &i, &h) || i + 4 > len || memcmp(s + i, " +X ", 4) != 0) {
            return cx_unsupported(err, F, "image orientation other than -Y +X");
        }
        i += 4;
        if (!parse_uint(s, len, &i, &w) || i != len) {
            return cx_malformed(err, F, "invalid image size line");
        }
    }
    if ((st = pc_climits_check(l, w, h, PC_CL_RGB, PC_CS_F32, err)) != PC_OK) {
        return st;
    }
    if ((st = pc_cimage_new(w, h, PC_CL_RGB, PC_CS_F32, &img, err)) != PC_OK) {
        return st;
    }
    px = (float *)(void *)img.data;
    scan = malloc((size_t)w * 4);
    if (scan == NULL) {
        pc_cimage_fini(&img);
        return cx_nomem(err);
    }
    for (y = 0; y < h; y++) {
        size_t x;
        if (w >= 8 && w <= 0x7FFF && n - at >= 4 && b[at] == 2 && b[at + 1] == 2 && !(b[at + 2] & 0x80)) {
            int c;
            if (((size_t)b[at + 2] << 8 | b[at + 3]) != w) {
                st = cx_malformed(err, F, "wrong scanline width");
                goto fail;
            }
            at += 4;
            for (c = 0; c < 4; c++) {
                x = 0;
                while (x < w) {
                    uint8_t cnt;
                    if (at >= n) {
                        st = cx_malformed(err, F, "image data is truncated");
                        goto fail;
                    }
                    cnt = b[at++];
                    if (cnt > 128) {
                        size_t k = cnt - 128u;
                        if (at >= n || k > w - x) {
                            st = cx_malformed(err, F, "image data is truncated");
                            goto fail;
                        }
                        while (k-- > 0) {
                            scan[4 * x++ + (size_t)c] = b[at];
                        }
                        at++;
                    } else {
                        size_t k = cnt;
                        if (k == 0 || k > w - x || n - at < k) {
                            st = cx_malformed(err, F, "image data is truncated");
                            goto fail;
                        }
                        while (k-- > 0) {
                            scan[4 * x++ + (size_t)c] = b[at++];
                        }
                    }
                }
            }
        } else {
            if ((n - at) / 4 < w) {
                st = cx_malformed(err, F, "image data is truncated");
                goto fail;
            }
            memcpy(scan, b + at, (size_t)w * 4);
            at += (size_t)w * 4;
        }
        for (x = 0; x < w; x++) {
            to_float(scan + 4 * x, px + (y * w + x) * 3);
        }
    }
    free(scan);
    *out = img;
    return PC_OK;
fail:
    free(scan);
    pc_cimage_fini(&img);
    return st;
}

static void to_rgbe(const float *p, uint8_t *o)
{
    float mx = pc_rs_maxf(p[0], pc_rs_maxf(p[1], p[2]));
    int e;
    float scale;
    int k;
    if (!(mx > 1e-32f) || !isfinite(mx)) {
        o[0] = o[1] = o[2] = o[3] = 0;
        if (isinf(mx) && mx > 0) {
            o[0] = o[1] = o[2] = 255;
            o[3] = 255;
        }
        return;
    }
    (void)frexpf(mx, &e);
    if (e + 128 > 255) {
        o[0] = o[1] = o[2] = o[3] = 255;
        return;
    }
    if (e + 128 < 1) {
        o[0] = o[1] = o[2] = o[3] = 0;
        return;
    }
    scale = ldexpf(256.0f, -e);
    for (k = 0; k < 3; k++) {
        o[k] = pc_f32_to_u8(p[k] * scale);
    }
    o[3] = (uint8_t)(e + 128);
}

pc_status cx_hdr_encode(const pc_cimage *src, cx_plan p, pc_buf *out, pc_error *err)
{
    pc_cimage tmp;
    const pc_cimage *img;
    cx_w w = {out, PC_OK, err};
    char hdr[96];
    uint8_t *scan;
    const float *px;
    size_t y;
    int hn;
    pc_status st = pc_cimage_converted(src, p.layout, p.sample, &tmp, &img, err);
    if (st != PC_OK) {
        return st;
    }
    if (img->layout != PC_CL_RGB || img->sample != PC_CS_F32) {
        pc_cimage_fini(&tmp);
        return cx_encode(err, F, "unsupported %s %s", pc_clayout_name(img->layout), pc_csample_name(img->sample));
    }
    hn = snprintf(hdr, sizeof hdr, "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %u +X %u\n", (unsigned)img->height,
                  (unsigned)img->width);
    cx_w_put(&w, hdr, (size_t)hn);
    scan = malloc((size_t)img->width * 4);
    if (scan == NULL) {
        pc_cimage_fini(&tmp);
        return cx_nomem(err);
    }
    px = (const float *)(const void *)img->data;
    for (y = 0; y < img->height; y++) {
        size_t x;
        for (x = 0; x < img->width; x++) {
            to_rgbe(px + (y * img->width + x) * 3, scan + 4 * x);
        }
        if (img->width < 8 || img->width > 0x7FFF) {
            cx_w_put(&w, scan, (size_t)img->width * 4);
            continue;
        }
        cx_w_byte(&w, 2);
        cx_w_byte(&w, 2);
        cx_w_byte(&w, (uint8_t)(img->width >> 8));
        cx_w_byte(&w, (uint8_t)img->width);
        {
            int c;
            for (c = 0; c < 4; c++) {
                x = 0;
                while (x < img->width) {
                    size_t run = 1;
                    while (x + run < img->width && run < 127 && scan[4 * (x + run) + (size_t)c] == scan[4 * x + (size_t)c]) {
                        run++;
                    }
                    if (run >= 3) {
                        cx_w_byte(&w, (uint8_t)(128 + run));
                        cx_w_byte(&w, scan[4 * x + (size_t)c]);
                        x += run;
                    } else {
                        size_t start = x;
                        size_t k;
                        while (x < img->width && x - start < 128) {
                            size_t r = 1;
                            while (x + r < img->width && r < 3 && scan[4 * (x + r) + (size_t)c] == scan[4 * x + (size_t)c]) {
                                r++;
                            }
                            if (r >= 3) {
                                break;
                            }
                            x++;
                        }
                        cx_w_byte(&w, (uint8_t)(x - start));
                        for (k = start; k < x; k++) {
                            cx_w_byte(&w, scan[4 * k + (size_t)c]);
                        }
                    }
                }
            }
        }
    }
    free(scan);
    pc_cimage_fini(&tmp);
    return w.st;
}
