/* DisplayInfo resources <-> alpha / spot channel options (port of crates/io/src/channel_map.rs). */
#include "io/channel_map.h"

#include <math.h>
#include <string.h>

#include "base/num.h"

enum { SPACE_RGB = 0, SPACE_HSB = 1, SPACE_CMYK = 2, SPACE_LAB = 7, SPACE_GRAY = 8 };

static pc_status put_be16(pc_buf *b, uint16_t v, pc_error *err)
{
    uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return pc_buf_append(b, x, 2, err);
}

pc_status pc_io_display_info(const pc_alpha_channel *const *channels, size_t n, bool transparency,
                             pc_buf *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    static const uint8_t version[4] = {0, 0, 0, 1};
    pc_status st = pc_buf_append(&b, version, 4, err);
    if (st == PC_OK && transparency) {
        /* As Photoshop writes it: red, 100 %, masked areas (13 bytes). */
        static const uint8_t trec[13] = {0, 0, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 100, 1};
        st = pc_buf_append(&b, trec, 13, err);
    }
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
            st = pc_buf_append(&b, &kind, 1, err);
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

void pc_io_apply_display_info(const uint8_t *data, size_t len, bool versioned, size_t skip,
                              pc_alpha_channel *channels, size_t n)
{
    const uint8_t *body = data;
    size_t blen = len;
    size_t size = versioned ? 13 : 14;
    if (versioned) {
        if (len < 4) {
            return;
        }
        body = data + 4;
        blen = len - 4;
    }
    for (size_t i = 0; i < n; i++) {
        /* Skip the first `skip` records (the merged transparency channel's), then take one
         * per channel while whole records remain. */
        size_t idx = i + skip;
        const uint8_t *rec;
        uint16_t comps[4];
        pc_color color;
        uint16_t op;
        float opacity;
        pc_alpha_channel *c;
        if ((idx + 1) > blen / size) {
            break;
        }
        rec = body + idx * size;
        comps[0] = be16(rec + 2);
        comps[1] = be16(rec + 4);
        comps[2] = be16(rec + 6);
        comps[3] = be16(rec + 8);
        color = pc_io_decode_color(be16(rec), comps);
        op = be16(rec + 10);
        opacity = (float)(op < 100 ? op : 100) / 100.0f;
        c = &channels[i];
        if (rec[12] == 2) {
            c->has_spot = true;
            c->spot_color = color;
            c->spot_solidity = opacity;
        } else {
            /* An alpha channel, not an ink (a Multichannel import starts every channel as one). */
            c->has_spot = false;
            c->color = color;
            c->opacity = opacity;
            c->indicates = rec[12] == 0 ? PC_INDICATES_SELECTED_AREAS : PC_INDICATES_MASKED_AREAS;
        }
    }
}

/* Rust's f32::rem_euclid: a non-negative remainder in [0, |b|). */
static float rem_euclidf(float a, float b)
{
    float r = fmodf(a, b);
    return r < 0.0f ? r + fabsf(b) : r;
}

/* HSB (hue in degrees, saturation and brightness 0..1) -> RGB. */
static void hsb_to_rgb(float h, float s, float v, float out[3])
{
    float f, p, q, t;
    int sector;
    h = isfinite(h) ? rem_euclidf(h, 360.0f) / 60.0f : 0.0f;
    s = pc_clampf(s, 0.0f, 1.0f);
    v = pc_clampf(v, 0.0f, 1.0f);
    f = h - floorf(h);
    p = v * (1.0f - s);
    q = v * (1.0f - s * f);
    t = v * (1.0f - s * (1.0f - f));
    sector = (int)floorf(h);
    switch (sector) {
    case 0:
        out[0] = v, out[1] = t, out[2] = p;
        break;
    case 1:
        out[0] = q, out[1] = v, out[2] = p;
        break;
    case 2:
        out[0] = p, out[1] = v, out[2] = t;
        break;
    case 3:
        out[0] = p, out[1] = q, out[2] = v;
        break;
    case 4:
        out[0] = t, out[1] = p, out[2] = v;
        break;
    default:
        out[0] = v, out[1] = p, out[2] = q;
        break;
    }
}

bool pc_io_decode_known_color(uint16_t space, const uint16_t c[4], pc_color *out)
{
    float rgb[3];
    switch (space) {
    case SPACE_RGB:
        *out = pc_color_rgb((float)c[0] / 65535.0f, (float)c[1] / 65535.0f, (float)c[2] / 65535.0f);
        return true;
    case SPACE_HSB: {
        float hsb[3];
        hsb_to_rgb((float)c[0] / 65535.0f * 360.0f, (float)c[1] / 65535.0f, (float)c[2] / 65535.0f, hsb);
        *out = pc_color_rgb(hsb[0], hsb[1], hsb[2]);
        return true;
    }
    case SPACE_CMYK: {
        /* Photoshop colour structures store CMYK inverted (65535 = no ink). */
        pc_color cmyk;
        cmyk.mode = PC_COLOR_MODE_CMYK;
        for (size_t i = 0; i < 4; i++) {
            cmyk.c[i] = 1.0f - (float)c[i] / 65535.0f;
        }
        cmyk.alpha = 1.0f;
        pc_color_to_rgb(&cmyk, rgb);
        *out = pc_color_rgb(rgb[0], rgb[1], rgb[2]);
        return true;
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
        *out = pc_color_rgb(rgb[0], rgb[1], rgb[2]);
        return true;
    }
    case SPACE_GRAY: {
        float v = 1.0f - (float)(c[0] < 10000 ? c[0] : 10000) / 10000.0f;
        *out = pc_color_rgb(v, v, v);
        return true;
    }
    default:
        return false;
    }
}

pc_color pc_io_decode_color(uint16_t space, const uint16_t c[4])
{
    pc_color out;
    if (pc_io_decode_known_color(space, c, &out)) {
        return out;
    }
    return pc_color_rgb((float)c[0] / 65535.0f, (float)c[1] / 65535.0f, (float)c[2] / 65535.0f);
}

