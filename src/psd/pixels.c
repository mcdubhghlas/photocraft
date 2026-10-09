#include "psd/pixels.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"
#include "psd/internal.h"

void pc_psd_image8_fini(pc_psd_image8 *img)
{
    if (img != NULL) {
        pc_psd_bytes_fini(&img->data);
    }
}

pc_status pc_psd_unpack_bits(const uint8_t *bytes, size_t len, size_t width, size_t height, pc_psd_bytes *out,
                             pc_error *err)
{
    size_t rb = width / 8 + (width % 8 != 0);
    size_t n;
    size_t x;
    size_t y;
    uint8_t *o = NULL;
    if (!pc_size_mul(width, height, &n) || n > PC_PSD_MAX_DECODED_BYTES) {
        return psd_limit(err, "decoded size overflow");
    }
    if (n == 0) {
        pc_psd_bytes_fini(out);
        return PC_OK;
    }
    o = malloc(n);
    if (o == NULL) {
        return psd_nomem(err);
    }
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            size_t at = y * rb + x / 8;
            uint8_t b = at < len ? bytes[at] : 0;
            o[y * width + x] = (uint8_t)((b >> (7 - x % 8)) & 1);
        }
    }
    pc_psd_bytes_fini(out);
    out->data = o;
    out->len = n;
    return PC_OK;
}

pc_status pc_psd_plane_to_u8(const uint8_t *bytes, size_t len, uint16_t depth, size_t width, size_t height,
                             pc_psd_bytes *out, pc_error *err)
{
    size_t i;
    size_t n;
    uint8_t *o = NULL;
    pc_status st;
    switch (depth) {
    case 1:
        st = pc_psd_unpack_bits(bytes, len, width, height, out, err);
        for (i = 0; st == PC_OK && i < out->len; i++) {
            out->data[i] = out->data[i] == 1 ? 0 : 255;
        }
        return st;
    case 8:
        return pc_psd_bytes_set(out, bytes, len, err);
    case 16:
    case 32:
        n = len / (depth / 8u);
        if (n > 0) {
            o = malloc(n);
            if (o == NULL) {
                return psd_nomem(err);
            }
        }
        for (i = 0; i < n; i++) {
            if (depth == 16) {
                uint32_t v = pc_load_be16(bytes + 2 * i);
                o[i] = (uint8_t)((v * 255u + 32767u) / 65535u);
            } else {
                float v = pc_f32_from_bits(pc_load_be32(bytes + 4 * i));
                o[i] = isnan(v) ? 0 : pc_f32_to_u8(roundf(pc_clampf(v, 0.0f, 1.0f) * 255.0f));
            }
        }
        pc_psd_bytes_fini(out);
        out->data = o;
        out->len = n;
        return PC_OK;
    default:
        return psd_unsupported(err, "depth %u", (unsigned)depth);
    }
}

static const char *mode_debug(uint16_t m, char buf[32])
{
    switch (m) {
    case PC_PSD_MODE_BITMAP:
        return "Bitmap";
    case PC_PSD_MODE_GRAYSCALE:
        return "Grayscale";
    case PC_PSD_MODE_INDEXED:
        return "Indexed";
    case PC_PSD_MODE_RGB:
        return "Rgb";
    case PC_PSD_MODE_CMYK:
        return "Cmyk";
    case PC_PSD_MODE_MULTICHANNEL:
        return "Multichannel";
    case PC_PSD_MODE_DUOTONE:
        return "Duotone";
    case PC_PSD_MODE_LAB:
        return "Lab";
    default:
        (void)snprintf(buf, 32, "Unknown(%u)", (unsigned)m);
        return buf;
    }
}

static pc_status color_channel_count(uint16_t mode, size_t *out, pc_error *err)
{
    char buf[32];
    switch (mode) {
    case PC_PSD_MODE_RGB:
        *out = 3;
        return PC_OK;
    case PC_PSD_MODE_CMYK:
        *out = 4;
        return PC_OK;
    case PC_PSD_MODE_GRAYSCALE:
    case PC_PSD_MODE_INDEXED:
    case PC_PSD_MODE_BITMAP:
    case PC_PSD_MODE_DUOTONE:
        *out = 1;
        return PC_OK;
    default:
        return psd_unsupported(err, "rgba8 for color mode %s", mode_debug(mode, buf));
    }
}

static pc_status interleave(uint16_t mode, pc_psd_bytes *color, const pc_psd_bytes *alpha, const pc_psd_bytes *palette,
                            size_t n, pc_psd_bytes *out, pc_error *err)
{
    uint8_t *o = NULL;
    size_t i;
    size_t need = mode == PC_PSD_MODE_RGB ? 3 : mode == PC_PSD_MODE_CMYK ? 4 : 1;
    if (n == 0) {
        pc_psd_bytes_fini(out);
        return PC_OK;
    }
    /* Every plane read below must hold n samples (the reference would panic otherwise). */
    for (i = 0; i < need; i++) {
        if (color[i].data == NULL || color[i].len < n) {
            return psd_invalid(err, "channel size mismatch");
        }
    }
    if (alpha != NULL && (alpha->data == NULL || alpha->len < n)) {
        return psd_invalid(err, "channel size mismatch");
    }
    o = pc_malloc_array(n, 4);
    if (o == NULL) {
        return psd_nomem(err);
    }
    for (i = 0; i < n; i++) {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        if (mode == PC_PSD_MODE_RGB) {
            r = color[0].data[i];
            g = color[1].data[i];
            b = color[2].data[i];
        } else if (mode == PC_PSD_MODE_INDEXED) {
            size_t idx = color[0].data[i];
            r = idx < palette->len ? palette->data[idx] : 0;
            g = 256 + idx < palette->len ? palette->data[256 + idx] : 0;
            b = 512 + idx < palette->len ? palette->data[512 + idx] : 0;
        } else if (mode == PC_PSD_MODE_CMYK) {
            uint32_t k = color[3].data[i];
            r = (uint8_t)((color[0].data[i] * k + 127u) / 255u);
            g = (uint8_t)((color[1].data[i] * k + 127u) / 255u);
            b = (uint8_t)((color[2].data[i] * k + 127u) / 255u);
        } else {
            r = g = b = color[0].data[i];
        }
        o[i * 4] = r;
        o[i * 4 + 1] = g;
        o[i * 4 + 2] = b;
        o[i * 4 + 3] = alpha != NULL ? alpha->data[i] : 255;
    }
    pc_psd_bytes_fini(out);
    out->data = o;
    out->len = n * 4;
    return PC_OK;
}

pc_status pc_psd_layer_rgba8(const pc_psd_file *f, size_t index, pc_psd_image8 *out, pc_error *err)
{
    size_t nl;
    const pc_psd_layer *layers = pc_psd_file_layers(f, &nl);
    const pc_psd_layer *l;
    size_t w;
    size_t h;
    size_t n;
    size_t cc;
    size_t c;
    pc_psd_bytes color[4];
    pc_psd_bytes alpha = {NULL, 0};
    bool has_alpha = false;
    pc_psd_bytes tmp = {NULL, 0};
    pc_psd_image8 img;
    uint16_t mode = f->header.color_mode;
    uint16_t depth = f->header.depth;
    pc_status st;
    memset(color, 0, sizeof color);
    memset(&img, 0, sizeof img);
    if (index >= nl) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "no layer %zu", index);
    }
    l = &layers[index];
    CK(pc_psd_rect_size(l->rect, &w, &h, err));
    n = w * h;
    CK(color_channel_count(mode, &cc, err));
    for (c = 0; c < cc; c++) {
        if (pc_psd_layer_channel(l, (int16_t)c) != NULL) {
            CK(pc_psd_layer_decode_channel(l, (int16_t)c, depth, f->header.version, &tmp, err));
            CK(pc_psd_plane_to_u8(tmp.data, tmp.len, depth, w, h, &color[c], err));
            pc_psd_bytes_fini(&tmp);
        } else {
            color[c].data = n > 0 ? malloc(n) : NULL;
            if (n > 0 && color[c].data == NULL) {
                st = psd_nomem(err);
                goto fail;
            }
            if (n > 0) {
                memset(color[c].data, mode == PC_PSD_MODE_CMYK ? 255 : 0, n);
            }
            color[c].len = n;
        }
        if (color[c].len != n) {
            st = psd_invalid(err, "channel size mismatch");
            goto fail;
        }
    }
    if (pc_psd_layer_channel(l, PC_PSD_CH_TRANSPARENCY) != NULL) {
        CK(pc_psd_layer_decode_channel(l, PC_PSD_CH_TRANSPARENCY, depth, f->header.version, &tmp, err));
        CK(pc_psd_plane_to_u8(tmp.data, tmp.len, depth, w, h, &alpha, err));
        pc_psd_bytes_fini(&tmp);
        has_alpha = true;
        if (alpha.len < n) { /* the reference would panic indexing it */
            st = psd_invalid(err, "channel size mismatch");
            goto fail;
        }
    }
    CK(interleave(mode, color, has_alpha ? &alpha : NULL, &tmp, n, &img.data, err));
    img.left = l->rect.left;
    img.top = l->rect.top;
    img.width = (uint32_t)w;
    img.height = (uint32_t)h;
    *out = img;
    st = PC_OK;
    goto done;
fail:
    pc_psd_image8_fini(&img);
done:
    for (c = 0; c < 4; c++) {
        pc_psd_bytes_fini(&color[c]);
    }
    pc_psd_bytes_fini(&alpha);
    pc_psd_bytes_fini(&tmp);
    return st;
}

pc_status pc_psd_layer_user_mask(const pc_psd_file *f, size_t index, pc_psd_image8 *out, pc_error *err)
{
    size_t nl;
    const pc_psd_layer *layers = pc_psd_file_layers(f, &nl);
    const pc_psd_layer *l;
    const pc_psd_layer_mask *m;
    int16_t id;
    pc_psd_rect rect;
    size_t w;
    size_t h;
    pc_psd_bytes tmp = {NULL, 0};
    pc_psd_image8 img;
    pc_status st;
    memset(&img, 0, sizeof img);
    if (index >= nl) {
        return pc_fail(err, PC_ERR_NOT_FOUND, "no layer %zu", index);
    }
    l = &layers[index];
    if (pc_psd_layer_channel(l, PC_PSD_CH_REAL_USER_MASK) != NULL) {
        id = PC_PSD_CH_REAL_USER_MASK;
    } else if (pc_psd_layer_channel(l, PC_PSD_CH_USER_MASK) != NULL) {
        id = PC_PSD_CH_USER_MASK;
    } else {
        return pc_fail(err, PC_ERR_NOT_FOUND, "layer has no mask channel");
    }
    rect = pc_psd_layer_channel_rect(l, id);
    CK(pc_psd_rect_size(rect, &w, &h, err));
    CK(pc_psd_layer_decode_channel(l, id, f->header.depth, f->header.version, &tmp, err));
    CK(pc_psd_plane_to_u8(tmp.data, tmp.len, f->header.depth, w, h, &img.data, err));
    m = pc_psd_layer_get_mask(l);
    if (m == NULL) {
        img.default_value = 0;
    } else if (id == PC_PSD_CH_REAL_USER_MASK) {
        img.default_value = m->has_real ? m->real.background : m->default_color;
    } else {
        img.default_value = m->default_color;
    }
    img.left = rect.left;
    img.top = rect.top;
    img.width = (uint32_t)w;
    img.height = (uint32_t)h;
    pc_psd_bytes_fini(&tmp);
    *out = img;
    return PC_OK;
fail:
    pc_psd_bytes_fini(&tmp);
    pc_psd_image8_fini(&img);
    return st;
}

pc_status pc_psd_composite_rgba8(const pc_psd_file *f, pc_psd_image8 *out, pc_error *err)
{
    const pc_psd_header *h = &f->header;
    size_t w = h->width;
    size_t hh = h->height;
    size_t n = w * hh;
    size_t cc;
    size_t c;
    size_t plane;
    pc_psd_bytes all = {NULL, 0};
    pc_psd_bytes color[4];
    pc_psd_bytes alpha = {NULL, 0};
    bool has_alpha;
    pc_psd_image8 img;
    pc_status st;
    memset(color, 0, sizeof color);
    memset(&img, 0, sizeof img);
    CK(color_channel_count(h->color_mode, &cc, err));
    if (h->channels < cc) {
        st = psd_invalid(err, "fewer channels than the color mode requires");
        goto fail;
    }
    CK(pc_psd_file_decode_merged(f, &all, err));
    plane = pc_psd_header_row_bytes(h) * hh;
    for (c = 0; c < cc; c++) {
        CK(pc_psd_plane_to_u8(all.data + c * plane, plane, h->depth, w, hh, &color[c], err));
    }
    has_alpha = pc_psd_file_merged_has_alpha(f);
    if (has_alpha) {
        CK(pc_psd_plane_to_u8(all.data + cc * plane, plane, h->depth, w, hh, &alpha, err));
    }
    CK(interleave(h->color_mode, color, has_alpha ? &alpha : NULL, &f->color_mode_data, n, &img.data, err));
    img.width = h->width;
    img.height = h->height;
    *out = img;
    st = PC_OK;
    goto done;
fail:
    pc_psd_image8_fini(&img);
done:
    for (c = 0; c < 4; c++) {
        pc_psd_bytes_fini(&color[c]);
    }
    pc_psd_bytes_fini(&alpha);
    pc_psd_bytes_fini(&all);
    return st;
}
