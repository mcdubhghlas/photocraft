/* DisplayInfo resources <-> alpha / spot channel options (port of crates/io/src/channel_map.rs). */
#include "io/channel_map.h"

#include <math.h>
#include <string.h>

#include "base/num.h"

enum { SPACE_RGB = 0, SPACE_CMYK = 2, SPACE_LAB = 7, SPACE_GRAY = 8 };

static pc_status put_be16(pc_buf *b, uint16_t v, pc_error *err)
{
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return pc_buf_append(b, x, 2, err);
}

pc_status pc_io_display_info(const pc_alpha_channel *const *channels, size_t n, pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    static const uint8_t version[4] = {0, 0, 0, 1};
    pc_status st = pc_buf_append(&b, version, 4, err);
    for (size_t i = 0; i < n && st == PC_OK; i++) {
        const pc_alpha_channel *c = channels[i];
        pc_color color;
        float opacity;
        uint8_t kind;
        float rgb[3];
        float comps[4];
        if (c->has_spot) {
            color = c->spot_color;
            opacity = c->spot_solidity;
            kind = 2;
        } else {
            color = c->color;
            opacity = c->opacity;
            kind = c->indicates == PC_INDICATES_SELECTED_AREAS ? 0 : 1;
        }
        pc_color_to_rgb(&color, rgb);
        comps[0] = rgb[0];
        comps[1] = rgb[1];
        comps[2] = rgb[2];
        comps[3] = 0.0f;
        st = put_be16(&b, SPACE_RGB, err);
        for (size_t k = 0; k < 4 && st == PC_OK; k++) {
            st = put_be16(&b, pc_f32_to_u16(roundf(pc_clampf(comps[k], 0.0f, 1.0f) * 65535.0f)), err);
        }
        if (st == PC_OK) {
            st = put_be16(&b, pc_f32_to_u16(roundf(pc_clampf(opacity, 0.0f, 1.0f) * 100.0f)), err);
        }
        if (st == PC_OK) {
            uint8_t tail[2] = {kind, 0};
            st = pc_buf_append(&b, tail, 2, err);
        }
    }
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

/* u16 bits as i16 (Rust `as i16`), without implementation-defined conversions. */
static int32_t as_i16(uint16_t v)
{
    return v >= 32768 ? (int32_t)v - 65536 : (int32_t)v;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

void pc_io_apply_display_info(const uint8_t *data, size_t len, bool versioned, pc_alpha_channel *channels,
                              size_t n)
{
    const uint8_t *body = data;
    size_t blen = len;
    if (versioned) {
        if (len < 4) {
            return;
        }
        body = data + 4;
        blen = len - 4;
    }
    for (size_t i = 0; i < n && (i + 1) * 14 <= blen; i++) {
        const uint8_t *rec = body + i * 14;
        uint16_t comps[4] = {be16(rec + 2), be16(rec + 4), be16(rec + 6), be16(rec + 8)};
        pc_color color = pc_io_decode_color(be16(rec), comps);
        uint16_t op = be16(rec + 10);
        float opacity = (float)(op < 100 ? op : 100) / 100.0f;
        pc_alpha_channel *c = &channels[i];
        if (rec[12] == 2) {
            c->has_spot = true;
            c->spot_color = color;
            c->spot_solidity = opacity;
        } else {
            c->color = color;
            c->opacity = opacity;
            c->indicates = rec[12] == 0 ? PC_INDICATES_SELECTED_AREAS : PC_INDICATES_MASKED_AREAS;
        }
    }
}

pc_color pc_io_decode_color(uint16_t space, const uint16_t c[4])
{
    float rgb[3];
    switch (space) {
    case SPACE_CMYK: {
        /* Photoshop colour structures store CMYK inverted (65535 = no ink). */
        pc_color cmyk;
        cmyk.mode = PC_COLOR_MODE_CMYK;
        for (size_t i = 0; i < 4; i++) {
            cmyk.c[i] = 1.0f - (float)c[i] / 65535.0f;
        }
        cmyk.alpha = 1.0f;
        pc_color_to_rgb(&cmyk, rgb);
        return pc_color_rgb(rgb[0], rgb[1], rgb[2]);
    }
    case SPACE_LAB: {
        pc_color lab;
        float l = (float)(c[0] < 10000 ? c[0] : 10000) / 10000.0f;
        float a = (float)as_i16(c[1]) / 100.0f;
        float b = (float)as_i16(c[2]) / 100.0f;
        lab.mode = PC_COLOR_MODE_LAB;
        lab.c[0] = l;
        lab.c[1] = (a + 128.0f) / 255.0f;
        lab.c[2] = (b + 128.0f) / 255.0f;
        lab.c[3] = 0.0f;
        lab.alpha = 1.0f;
        pc_color_to_rgb(&lab, rgb);
        return pc_color_rgb(rgb[0], rgb[1], rgb[2]);
    }
    case SPACE_GRAY: {
        float v = 1.0f - (float)(c[0] < 10000 ? c[0] : 10000) / 10000.0f;
        return pc_color_rgb(v, v, v);
    }
    default:
        return pc_color_rgb((float)c[0] / 65535.0f, (float)c[1] / 65535.0f, (float)c[2] / 65535.0f);
    }
}
