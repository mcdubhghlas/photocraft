#include "color.h"

#include <string.h>

#include "base/num.h"
#include "convert.h"

size_t pc_sample_type_bytes(pc_sample_type s)
{
    switch (s) {
    case PC_SAMPLE_TYPE_U8:
        return 1;
    case PC_SAMPLE_TYPE_U16:
        return 2;
    case PC_SAMPLE_TYPE_F32:
        return 4;
    }
    return 1;
}

uint32_t pc_sample_type_bits(pc_sample_type s)
{
    return (uint32_t)pc_sample_type_bytes(s) * 8u;
}

static const char *const SAMPLE_NAMES[PC_SAMPLE_TYPE_COUNT] = {"U8", "U16", "F32"};

const char *pc_sample_type_name(pc_sample_type s)
{
    return (unsigned)s < PC_SAMPLE_TYPE_COUNT ? SAMPLE_NAMES[s] : "U8";
}

static bool name_eq(const char *s, size_t len, const char *name)
{
    return s != NULL && strlen(name) == len && memcmp(s, name, len) == 0;
}

bool pc_sample_type_from_name(const char *s, size_t len, pc_sample_type *out)
{
    unsigned i;
    for (i = 0; i < PC_SAMPLE_TYPE_COUNT; i++) {
        if (name_eq(s, len, SAMPLE_NAMES[i])) {
            *out = (pc_sample_type)i;
            return true;
        }
    }
    return false;
}

size_t pc_color_mode_channels(pc_color_mode m)
{
    switch (m) {
    case PC_COLOR_MODE_BITMAP:
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_DUOTONE:
        return 1;
    case PC_COLOR_MODE_INDEXED:
    case PC_COLOR_MODE_RGB:
    case PC_COLOR_MODE_LAB:
    case PC_COLOR_MODE_MULTICHANNEL:
        return 3;
    case PC_COLOR_MODE_CMYK:
        return 4;
    }
    return 3;
}

static const char *const MODE_NAMES[PC_COLOR_MODE_COUNT] = {"Bitmap", "Grayscale", "Indexed",      "Rgb",
                                                            "Cmyk",   "Lab",       "Multichannel", "Duotone"};

const char *pc_color_mode_name(pc_color_mode m)
{
    return (unsigned)m < PC_COLOR_MODE_COUNT ? MODE_NAMES[m] : "Rgb";
}

bool pc_color_mode_from_name(const char *s, size_t len, pc_color_mode *out)
{
    unsigned i;
    for (i = 0; i < PC_COLOR_MODE_COUNT; i++) {
        if (name_eq(s, len, MODE_NAMES[i])) {
            *out = (pc_color_mode)i;
            return true;
        }
    }
    return false;
}

size_t pc_pixel_format_channels(pc_pixel_format f)
{
    return pc_color_mode_channels(f.mode) + (f.alpha ? 1u : 0u);
}

size_t pc_pixel_format_bytes_per_pixel(pc_pixel_format f)
{
    return pc_pixel_format_channels(f) * pc_sample_type_bytes(f.sample);
}

bool pc_pixel_format_eq(pc_pixel_format a, pc_pixel_format b)
{
    return a.mode == b.mode && a.sample == b.sample && a.alpha == b.alpha;
}

/* Byte offset of sample `index`, if the whole sample lies within `len`. */
static bool sample_at(size_t len, pc_sample_type s, size_t index, size_t *off)
{
    size_t n = pc_sample_type_bytes(s);
    size_t end;
    if (!pc_size_mul(index, n, off) || !pc_size_add(*off, n, &end) || end > len) {
        return false;
    }
    return true;
}

float pc_read_sample(const uint8_t *bytes, size_t len, pc_sample_type s, size_t index)
{
    size_t o;
    if (bytes == NULL || !sample_at(len, s, index, &o)) {
        return 0.0f;
    }
    switch (s) {
    case PC_SAMPLE_TYPE_U8:
        return (float)bytes[o] / 255.0f;
    case PC_SAMPLE_TYPE_U16: {
        uint16_t v;
        memcpy(&v, bytes + o, 2);
        return (float)v / 65535.0f;
    }
    case PC_SAMPLE_TYPE_F32: {
        float v;
        memcpy(&v, bytes + o, 4);
        return v;
    }
    }
    return 0.0f;
}

void pc_write_sample(uint8_t *bytes, size_t len, pc_sample_type s, size_t index, float v)
{
    size_t o;
    if (bytes == NULL || !sample_at(len, s, index, &o)) {
        return;
    }
    switch (s) {
    case PC_SAMPLE_TYPE_U8:
        bytes[o] = pc_f32_to_u8(pc_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        break;
    case PC_SAMPLE_TYPE_U16: {
        uint16_t q = pc_f32_to_u16(pc_clampf(v, 0.0f, 1.0f) * 65535.0f + 0.5f);
        memcpy(bytes + o, &q, 2);
        break;
    }
    case PC_SAMPLE_TYPE_F32:
        memcpy(bytes + o, &v, 4);
        break;
    }
}

float pc_dither_noise(int32_t x, int32_t y)
{
    uint32_t h = ((uint32_t)x * 0x9E3779B1u) ^ ((uint32_t)y * 0x85EBCA77u);
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    return (float)(uint16_t)(h & 0xFFFFu) / 65535.0f;
}

pc_color pc_color_rgb(float r, float g, float b)
{
    pc_color c = {PC_COLOR_MODE_RGB, {r, g, b, 0.0f}, 1.0f};
    return c;
}

pc_color pc_color_rgba(float r, float g, float b, float a)
{
    pc_color c = {PC_COLOR_MODE_RGB, {r, g, b, 0.0f}, a};
    return c;
}

pc_color pc_color_gray(float v)
{
    pc_color c = {PC_COLOR_MODE_GRAYSCALE, {v, 0.0f, 0.0f, 0.0f}, 1.0f};
    return c;
}

void pc_color_to_rgb(const pc_color *c, float out[3])
{
    switch (c->mode) {
    case PC_COLOR_MODE_RGB:
    case PC_COLOR_MODE_INDEXED:
    case PC_COLOR_MODE_MULTICHANNEL:
        out[0] = c->c[0];
        out[1] = c->c[1];
        out[2] = c->c[2];
        return;
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_BITMAP:
    case PC_COLOR_MODE_DUOTONE:
        out[0] = out[1] = out[2] = c->c[0];
        return;
    case PC_COLOR_MODE_CMYK:
        pc_cmyk_to_rgb(c->c, out);
        return;
    case PC_COLOR_MODE_LAB: {
        float lab[3];
        lab[0] = c->c[0] * 100.0f;
        lab[1] = c->c[1] * 255.0f - 128.0f;
        lab[2] = c->c[2] * 255.0f - 128.0f;
        pc_lab_to_srgb(lab, out);
        return;
    }
    }
    out[0] = out[1] = out[2] = 0.0f;
}

static uint8_t q8(float v)
{
    return pc_f32_to_u8(pc_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void pc_color_to_rgba8(const pc_color *c, uint8_t out[4])
{
    float rgb[3];
    pc_color_to_rgb(c, rgb);
    out[0] = q8(rgb[0]);
    out[1] = q8(rgb[1]);
    out[2] = q8(rgb[2]);
    out[3] = q8(c->alpha);
}
