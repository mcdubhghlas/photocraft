#include "pixel.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "color/convert.h"

void pc_encode_pixel(pc_pixel_format f, const float *px, size_t n, uint8_t *out)
{
    size_t ch = pc_pixel_format_channels(f);
    size_t bpp = pc_pixel_format_bytes_per_pixel(f);
    size_t i;
    for (i = 0; i < n && i < ch; i++) {
        pc_write_sample(out, bpp, f.sample, i, px[i]);
    }
}

void pc_decode_pixel(pc_pixel_format f, const uint8_t *bytes, float *out)
{
    size_t ch = pc_pixel_format_channels(f);
    size_t bpp = pc_pixel_format_bytes_per_pixel(f);
    size_t i;
    for (i = 0; i < ch; i++) {
        out[i] = pc_read_sample(bytes, bpp, f.sample, i);
    }
}

/* px padded with zeros to PC_RASTER_MAX_CHANNELS. */
static void pad(const float *px, size_t n, float out[PC_RASTER_MAX_CHANNELS])
{
    size_t i;
    for (i = 0; i < PC_RASTER_MAX_CHANNELS; i++) {
        out[i] = i < n ? px[i] : 0.0f;
    }
}

void pc_to_rgba(pc_pixel_format f, const float *px, size_t n, float out[4])
{
    float p[PC_RASTER_MAX_CHANNELS];
    size_t cc = pc_color_mode_channels(f.mode);
    float a;
    float rgb[3];
    pad(px, n, p);
    a = f.alpha ? p[cc] : 1.0f;
    switch (f.mode) {
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_BITMAP:
    case PC_COLOR_MODE_DUOTONE:
        rgb[0] = rgb[1] = rgb[2] = p[0];
        break;
    case PC_COLOR_MODE_CMYK:
        pc_cmyk_to_rgb(p, rgb);
        break;
    case PC_COLOR_MODE_LAB: {
        float lab[3];
        lab[0] = p[0] * 100.0f;
        lab[1] = p[1] * 255.0f - 128.0f;
        lab[2] = p[2] * 255.0f - 128.0f;
        pc_lab_to_srgb(lab, rgb);
        break;
    }
    default:
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = p[2];
        break;
    }
    out[0] = rgb[0];
    out[1] = rgb[1];
    out[2] = rgb[2];
    out[3] = a;
}

size_t pc_from_rgba(pc_pixel_format f, const float rgba[4], float out[PC_RASTER_MAX_CHANNELS])
{
    float rgb[3];
    size_t n;
    rgb[0] = rgba[0];
    rgb[1] = rgba[1];
    rgb[2] = rgba[2];
    switch (f.mode) {
    case PC_COLOR_MODE_GRAYSCALE:
    case PC_COLOR_MODE_BITMAP:
    case PC_COLOR_MODE_DUOTONE:
        out[0] = pc_rgb_to_gray(rgb);
        n = 1;
        break;
    case PC_COLOR_MODE_CMYK:
        pc_rgb_to_cmyk(rgb, out);
        n = 4;
        break;
    case PC_COLOR_MODE_LAB: {
        float l[3];
        pc_srgb_to_lab(rgb, l);
        out[0] = l[0] / 100.0f;
        out[1] = (l[1] + 128.0f) / 255.0f;
        out[2] = (l[2] + 128.0f) / 255.0f;
        n = 3;
        break;
    }
    default:
        out[0] = rgb[0];
        out[1] = rgb[1];
        out[2] = rgb[2];
        n = 3;
        break;
    }
    if (f.alpha) {
        out[n++] = rgba[3];
    }
    return n;
}

size_t pc_convert_pixel(pc_pixel_format from, pc_pixel_format to, const float *px, size_t n,
                        float out[PC_RASTER_MAX_CHANNELS])
{
    float p[PC_RASTER_MAX_CHANNELS];
    float rgba[4];
    pad(px, n, p);
    if (from.mode == to.mode) {
        size_t cc = pc_color_mode_channels(from.mode);
        size_t i;
        for (i = 0; i < cc; i++) {
            out[i] = p[i];
        }
        if (to.alpha) {
            out[cc] = from.alpha ? p[cc] : 1.0f;
            cc++;
        }
        return cc;
    }
    pc_to_rgba(from, p, PC_RASTER_MAX_CHANNELS, rgba);
    return pc_from_rgba(to, rgba, out);
}

bool pc_sample_is_hdr(pc_sample_type s)
{
    return s == PC_SAMPLE_TYPE_F32;
}

pc_status pc_rgba8_image_init(pc_rgba8_image *img, uint32_t width, uint32_t height, pc_error *err)
{
    size_t n;
    if (!pc_size_mul3(width, height, 4, &n) || n > PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "image of %ux%u is too large", (unsigned)width, (unsigned)height);
    }
    img->width = width;
    img->height = height;
    img->pixels = NULL;
    if (n > 0) {
        img->pixels = calloc(n, 1);
        if (img->pixels == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for a %ux%u image", (unsigned)width, (unsigned)height);
        }
    }
    return PC_OK;
}

void pc_rgba8_image_fini(pc_rgba8_image *img)
{
    if (img != NULL) {
        free(img->pixels);
        img->pixels = NULL;
        img->width = 0;
        img->height = 0;
    }
}
