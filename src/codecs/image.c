#include "codecs/image.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/half.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "codecs/format.h"
#include "codecs/internal.h"

/* ---- layouts and sample types ---- */

size_t pc_clayout_channels(pc_clayout l)
{
    switch (l) {
    case PC_CL_GRAY:
        return 1;
    case PC_CL_GRAYA:
        return 2;
    case PC_CL_RGB:
        return 3;
    case PC_CL_RGBA:
    case PC_CL_CMYK:
        return 4;
    default:
        return 5;
    }
}

bool pc_clayout_has_alpha(pc_clayout l) { return l == PC_CL_GRAYA || l == PC_CL_RGBA || l == PC_CL_CMYKA; }
size_t pc_clayout_color_channels(pc_clayout l) { return pc_clayout_channels(l) - (pc_clayout_has_alpha(l) ? 1 : 0); }
bool pc_clayout_is_gray(pc_clayout l) { return l == PC_CL_GRAY || l == PC_CL_GRAYA; }
bool pc_clayout_is_rgb(pc_clayout l) { return l == PC_CL_RGB || l == PC_CL_RGBA; }
bool pc_clayout_is_cmyk(pc_clayout l) { return l == PC_CL_CMYK || l == PC_CL_CMYKA; }

pc_clayout pc_clayout_with_alpha(pc_clayout l)
{
    return pc_clayout_is_gray(l) ? PC_CL_GRAYA : pc_clayout_is_rgb(l) ? PC_CL_RGBA : PC_CL_CMYKA;
}

pc_clayout pc_clayout_without_alpha(pc_clayout l)
{
    return pc_clayout_is_gray(l) ? PC_CL_GRAY : pc_clayout_is_rgb(l) ? PC_CL_RGB : PC_CL_CMYK;
}

bool pc_clayout_same_model(pc_clayout a, pc_clayout b)
{
    return (pc_clayout_is_gray(a) && pc_clayout_is_gray(b)) || (pc_clayout_is_rgb(a) && pc_clayout_is_rgb(b)) ||
           (pc_clayout_is_cmyk(a) && pc_clayout_is_cmyk(b));
}

const char *pc_clayout_name(pc_clayout l)
{
    static const char *const N[PC_CL_COUNT] = {"Gray", "GrayA", "Rgb", "Rgba", "Cmyk", "CmykA"};
    return (unsigned)l < PC_CL_COUNT ? N[l] : "?";
}

size_t pc_csample_bytes(pc_csample s) { return s == PC_CS_U8 ? 1 : s == PC_CS_F32 ? 4 : 2; }
bool pc_csample_is_float(pc_csample s) { return s == PC_CS_F16 || s == PC_CS_F32; }

const char *pc_csample_name(pc_csample s)
{
    static const char *const N[PC_CS_COUNT] = {"U8", "U16", "F16", "F32"};
    return (unsigned)s < PC_CS_COUNT ? N[s] : "?";
}

/* ---- blobs, text, metadata ---- */

void pc_meta_blob_fini(pc_meta_blob *b)
{
    if (b != NULL) {
        free(b->data);
        b->data = NULL;
        b->len = 0;
        b->has = false;
    }
}

pc_status pc_meta_blob_set(pc_meta_blob *b, const void *data, size_t len, pc_error *err)
{
    uint8_t *p = malloc(len > 0 ? len : 1);
    if (p == NULL) {
        return cx_nomem(err);
    }
    if (len > 0) {
        memcpy(p, data, len);
    }
    free(b->data);
    b->data = p;
    b->len = len;
    b->has = true;
    return PC_OK;
}

pc_status pc_meta_blob_clone(const pc_meta_blob *b, pc_meta_blob *out, pc_error *err)
{
    pc_meta_blob c = {false, NULL, 0};
    if (b->has) {
        pc_status st = pc_meta_blob_set(&c, b->data, b->len, err);
        if (st != PC_OK) {
            return st;
        }
    }
    *out = c;
    return PC_OK;
}

bool pc_meta_blob_eq(const pc_meta_blob *a, const pc_meta_blob *b)
{
    return a->has == b->has && (!a->has || (a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0)));
}

static char *dup_n(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (p != NULL) {
        if (n > 0) {
            memcpy(p, s, n);
        }
        p[n] = '\0';
    }
    return p;
}

void pc_cmeta_fini(pc_cmeta *m)
{
    size_t i;
    if (m == NULL) {
        return;
    }
    pc_meta_blob_fini(&m->exif);
    pc_meta_blob_fini(&m->xmp);
    pc_meta_blob_fini(&m->photoshop_resources);
    pc_meta_blob_fini(&m->photoshop_layers);
    for (i = 0; i < m->ntext; i++) {
        free(m->text[i].key);
        free(m->text[i].value);
    }
    free(m->text);
    memset(m, 0, sizeof *m);
}

pc_status pc_cmeta_add_text(pc_cmeta *m, const char *key, size_t key_len, const char *value, size_t value_len,
                            pc_error *err)
{
    pc_ctext t;
    pc_ctext *g;
    if (key_len == SIZE_MAX || value_len == SIZE_MAX) {
        return cx_nomem(err);
    }
    t.key = dup_n(key, key_len);
    t.value = dup_n(value, value_len);
    t.key_len = key_len;
    t.value_len = value_len;
    g = t.key != NULL && t.value != NULL ? realloc(m->text, (m->ntext + 1) * sizeof *g) : NULL;
    if (g == NULL) {
        free(t.key);
        free(t.value);
        return cx_nomem(err);
    }
    m->text = g;
    m->text[m->ntext++] = t;
    return PC_OK;
}

pc_status pc_cmeta_clone(const pc_cmeta *m, pc_cmeta *out, pc_error *err)
{
    pc_cmeta c;
    size_t i;
    pc_status st;
    memset(&c, 0, sizeof c);
    c.has_dpi = m->has_dpi;
    c.dpi_x = m->dpi_x;
    c.dpi_y = m->dpi_y;
    CK(pc_meta_blob_clone(&m->exif, &c.exif, err));
    CK(pc_meta_blob_clone(&m->xmp, &c.xmp, err));
    CK(pc_meta_blob_clone(&m->photoshop_resources, &c.photoshop_resources, err));
    CK(pc_meta_blob_clone(&m->photoshop_layers, &c.photoshop_layers, err));
    for (i = 0; i < m->ntext; i++) {
        CK(pc_cmeta_add_text(&c, m->text[i].key, m->text[i].key_len, m->text[i].value, m->text[i].value_len, err));
    }
    *out = c;
    return PC_OK;
fail:
    pc_cmeta_fini(&c);
    return st;
}

static bool text_eq(const pc_ctext *a, const pc_ctext *b)
{
    return a->key_len == b->key_len && a->value_len == b->value_len && memcmp(a->key, b->key, a->key_len) == 0 &&
           memcmp(a->value, b->value, a->value_len) == 0;
}

bool pc_cmeta_eq(const pc_cmeta *a, const pc_cmeta *b)
{
    size_t i;
    if (!pc_meta_blob_eq(&a->exif, &b->exif) || !pc_meta_blob_eq(&a->xmp, &b->xmp) || a->has_dpi != b->has_dpi ||
        (a->has_dpi && !(a->dpi_x == b->dpi_x && a->dpi_y == b->dpi_y)) || a->ntext != b->ntext ||
        !pc_meta_blob_eq(&a->photoshop_resources, &b->photoshop_resources) ||
        !pc_meta_blob_eq(&a->photoshop_layers, &b->photoshop_layers)) {
        return false;
    }
    for (i = 0; i < a->ntext; i++) {
        if (!text_eq(&a->text[i], &b->text[i])) {
            return false;
        }
    }
    return true;
}

bool pc_cmeta_is_empty(const pc_cmeta *m)
{
    return !m->exif.has && !m->xmp.has && !m->has_dpi && m->ntext == 0 && !m->photoshop_resources.has &&
           !m->photoshop_layers.has;
}

/* ---- warnings ---- */

bool pc_cwarning_eq(const pc_cwarning *a, const pc_cwarning *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_CW_MORE_FRAMES:
    case PC_CW_MORE_PAGES:
    case PC_CW_MORE_PARTS:
        return a->has_total == b->has_total && (!a->has_total || a->total == b->total);
    case PC_CW_TRUNCATED:
        return a->format == b->format;
    default:
        return a->format == b->format && a->max_samples == b->max_samples;
    }
}

void pc_cwarning_text(const pc_cwarning *w, char *out, size_t cap)
{
    if (cap == 0) {
        return;
    }
    switch (w->kind) {
    case PC_CW_MORE_FRAMES:
        if (w->has_total) {
            (void)snprintf(out, cap, "only the first of %u frames was imported", (unsigned)w->total);
        } else {
            (void)snprintf(out, cap, "only the first frame of the animation was imported");
        }
        break;
    case PC_CW_MORE_PAGES:
        if (w->has_total) {
            (void)snprintf(out, cap, "only the first of %u pages was imported", (unsigned)w->total);
        } else {
            (void)snprintf(out, cap, "only the first page of the file was imported");
        }
        break;
    case PC_CW_MORE_PARTS:
        if (w->has_total) {
            (void)snprintf(out, cap, "only one of %u EXR parts was imported", (unsigned)w->total);
        } else {
            (void)snprintf(out, cap, "only one EXR part of the file was imported");
        }
        break;
    case PC_CW_TRUNCATED:
        (void)snprintf(out, cap,
                       "%s data ends early (the file is truncated or damaged); part of the image is missing",
                       pc_cformat_name((pc_cformat)w->format));
        break;
    default:
        (void)snprintf(out, cap,
                       "%s deep data was composited into a flat image (up to %u samples per pixel); per-pixel depth "
                       "is not kept",
                       pc_cformat_name((pc_cformat)w->format), (unsigned)w->max_samples);
    }
}

/* ---- images ---- */

void pc_cimage_init(pc_cimage *img)
{
    memset(img, 0, sizeof *img);
    img->layout = PC_CL_GRAY;
    img->sample = PC_CS_U8;
}

void pc_cimage_fini(pc_cimage *img)
{
    if (img == NULL) {
        return;
    }
    free(img->data);
    pc_meta_blob_fini(&img->icc);
    pc_cmeta_fini(&img->meta);
    free(img->warnings);
    pc_cimage_init(img);
}

static bool byte_len(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, size_t *out)
{
    size_t px;
    size_t sm;
    return pc_size_mul(w, h, &px) && pc_size_mul(px, pc_clayout_channels(l), &sm) &&
           pc_size_mul(sm, pc_csample_bytes(s), out);
}

pc_status pc_cimage_new(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, pc_cimage *out, pc_error *err)
{
    size_t len;
    uint8_t *d;
    if (!byte_len(w, h, l, s, &len)) {
        return cx_invalid(err, "image size overflows usize");
    }
    d = calloc(len > 0 ? len : 1, 1);
    if (d == NULL) {
        return cx_limit(err, "cannot allocate %zu bytes for the image", len);
    }
    pc_cimage_init(out);
    out->width = w;
    out->height = h;
    out->layout = l;
    out->sample = s;
    out->data = d;
    out->len = len;
    return PC_OK;
}

pc_status pc_cimage_from_raw(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, uint8_t *data, size_t len,
                             pc_cimage *out, pc_error *err)
{
    size_t want;
    if (!byte_len(w, h, l, s, &want)) {
        free(data);
        return cx_invalid(err, "image size overflows usize");
    }
    if (len != want) {
        free(data);
        return cx_invalid(err, "buffer has %zu bytes, expected %zu for %ux%u %s %s", len, want, (unsigned)w,
                          (unsigned)h, pc_clayout_name(l), pc_csample_name(s));
    }
    pc_cimage_init(out);
    out->width = w;
    out->height = h;
    out->layout = l;
    out->sample = s;
    out->data = data;
    out->len = len;
    return PC_OK;
}

pc_status pc_cimage_from_copy(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, const void *data, size_t len,
                              pc_cimage *out, pc_error *err)
{
    uint8_t *d = malloc(len > 0 ? len : 1);
    if (d == NULL) {
        return cx_nomem(err);
    }
    if (len > 0) {
        memcpy(d, data, len);
    }
    return pc_cimage_from_raw(w, h, l, s, d, len, out, err);
}

pc_status pc_cimage_from_normalized(uint32_t w, uint32_t h, pc_clayout l, pc_csample s, const float *values,
                                    size_t n, pc_cimage *out, pc_error *err)
{
    size_t bytes;
    uint8_t *d;
    if (!pc_size_mul(n, pc_csample_bytes(s), &bytes)) {
        return cx_invalid(err, "image size overflows usize");
    }
    d = malloc(bytes > 0 ? bytes : 1);
    if (d == NULL) {
        return cx_nomem(err);
    }
    cx_quantize(values, n, s, d);
    return pc_cimage_from_raw(w, h, l, s, d, bytes, out, err);
}

pc_status pc_cimage_clone(const pc_cimage *img, pc_cimage *out, pc_error *err)
{
    pc_cimage c;
    pc_status st;
    pc_cimage_init(&c);
    CK(pc_cimage_from_copy(img->width, img->height, img->layout, img->sample, img->data, img->len, &c, err));
    CK(pc_meta_blob_clone(&img->icc, &c.icc, err));
    CK(pc_cmeta_clone(&img->meta, &c.meta, err));
    if (img->nwarnings > 0) {
        c.warnings = pc_malloc_array(img->nwarnings, sizeof *c.warnings);
        if (c.warnings == NULL) {
            st = cx_nomem(err);
            goto fail;
        }
        memcpy(c.warnings, img->warnings, img->nwarnings * sizeof *c.warnings);
        c.nwarnings = img->nwarnings;
    }
    *out = c;
    return PC_OK;
fail:
    pc_cimage_fini(&c);
    return st;
}

bool pc_cimage_eq(const pc_cimage *a, const pc_cimage *b)
{
    size_t i;
    if (a->width != b->width || a->height != b->height || a->layout != b->layout || a->sample != b->sample ||
        a->len != b->len || (a->len > 0 && memcmp(a->data, b->data, a->len) != 0) || !pc_meta_blob_eq(&a->icc, &b->icc) ||
        !pc_cmeta_eq(&a->meta, &b->meta) || a->nwarnings != b->nwarnings) {
        return false;
    }
    for (i = 0; i < a->nwarnings; i++) {
        if (!pc_cwarning_eq(&a->warnings[i], &b->warnings[i])) {
            return false;
        }
    }
    return true;
}

pc_status pc_cimage_add_warning(pc_cimage *img, pc_cwarning w, pc_error *err)
{
    pc_cwarning *g = realloc(img->warnings, (img->nwarnings + 1) * sizeof *g);
    if (g == NULL) {
        return cx_nomem(err);
    }
    img->warnings = g;
    img->warnings[img->nwarnings++] = w;
    return PC_OK;
}

size_t pc_cimage_pixel_count(const pc_cimage *img) { return (size_t)img->width * img->height; }
size_t pc_cimage_sample_count(const pc_cimage *img) { return pc_cimage_pixel_count(img) * pc_clayout_channels(img->layout); }

/* ---- sample conversion ---- */

float cx_read_normalized(const uint8_t *data, pc_csample s, size_t i)
{
    switch (s) {
    case PC_CS_U8:
        return (float)data[i] / 255.0f;
    case PC_CS_U16:
        return (float)cx_ne16(data, i) / 65535.0f;
    case PC_CS_F16:
        return pc_f16_to_f32(cx_ne16(data, i));
    default: {
        float v;
        memcpy(&v, data + 4 * i, 4);
        return v;
    }
    }
}

float pc_cimage_sample_normalized(const pc_cimage *img, size_t index)
{
    return cx_read_normalized(img->data, img->sample, index);
}

float pc_cimage_get(const pc_cimage *img, uint32_t x, uint32_t y, size_t c)
{
    return pc_cimage_sample_normalized(img, ((size_t)y * img->width + x) * pc_clayout_channels(img->layout) + c);
}

pc_status pc_cimage_to_normalized(const pc_cimage *img, float **out, size_t *n, pc_error *err)
{
    size_t count = img->len / pc_csample_bytes(img->sample);
    size_t i;
    float *v = NULL;
    if (count > 0 && (v = pc_malloc_array(count, sizeof *v)) == NULL) {
        return cx_nomem(err);
    }
    for (i = 0; i < count; i++) {
        v[i] = cx_read_normalized(img->data, img->sample, i);
    }
    *out = v;
    *n = count;
    return PC_OK;
}

static float clamp01(float v) { return isnan(v) ? 0.0f : pc_clampf(v, 0.0f, 1.0f); }

void cx_quantize(const float *values, size_t n, pc_csample s, uint8_t *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        switch (s) {
        case PC_CS_U8:
            out[i] = (uint8_t)roundf(clamp01(values[i]) * 255.0f);
            break;
        case PC_CS_U16: {
            uint16_t v = (uint16_t)roundf(clamp01(values[i]) * 65535.0f);
            memcpy(out + 2 * i, &v, 2);
            break;
        }
        case PC_CS_F16: {
            uint16_t v = pc_f16_from_f32(values[i]);
            memcpy(out + 2 * i, &v, 2);
            break;
        }
        default:
            memcpy(out + 4 * i, &values[i], 4);
        }
    }
}

bool pc_cimage_has_translucency(const pc_cimage *img)
{
    size_t ch = pc_clayout_channels(img->layout);
    size_t p;
    size_t n = pc_cimage_pixel_count(img);
    if (!pc_clayout_has_alpha(img->layout)) {
        return false;
    }
    for (p = 0; p < n; p++) {
        if (pc_cimage_sample_normalized(img, p * ch + ch - 1) < 1.0f) {
            return true;
        }
    }
    return false;
}

bool pc_cimage_has_out_of_range(const pc_cimage *img)
{
    size_t i;
    size_t n = pc_cimage_sample_count(img);
    if (!pc_csample_is_float(img->sample)) {
        return false;
    }
    for (i = 0; i < n; i++) {
        float v = pc_cimage_sample_normalized(img, i);
        if (!(v >= 0.0f && v <= 1.0f)) {
            return true;
        }
    }
    return false;
}

/* Rec. 709 luma weights applied to encoded values. */
static const float LUMA[3] = {0.2126f, 0.7152f, 0.0722f};

void cx_convert_layout(const float *src, size_t n, pc_clayout from, pc_clayout to, float *out)
{
    size_t fc = pc_clayout_channels(from);
    size_t tc = pc_clayout_channels(to);
    size_t ccount = pc_clayout_color_channels(from);
    size_t p;
    if (n == 0) {
        return;
    }
    if (from == to) {
        memcpy(out, src, n * fc * sizeof *src);
        return;
    }
    for (p = 0; p < n; p++) {
        const float *px = src + p * fc;
        float *o = out + p * tc;
        float alpha = pc_clayout_has_alpha(from) ? px[fc - 1] : 1.0f;
        size_t k = 0;
        if (pc_clayout_same_model(from, to)) {
            memcpy(o, px, ccount * sizeof *px);
            k = ccount;
        } else {
            float rgb[3];
            if (pc_clayout_is_gray(from)) {
                rgb[0] = rgb[1] = rgb[2] = px[0];
            } else if (pc_clayout_is_rgb(from)) {
                rgb[0] = px[0];
                rgb[1] = px[1];
                rgb[2] = px[2];
            } else {
                rgb[0] = (1.0f - px[0]) * (1.0f - px[3]);
                rgb[1] = (1.0f - px[1]) * (1.0f - px[3]);
                rgb[2] = (1.0f - px[2]) * (1.0f - px[3]);
            }
            if (pc_clayout_is_gray(to)) {
                o[k++] = rgb[0] * LUMA[0] + rgb[1] * LUMA[1] + rgb[2] * LUMA[2];
            } else if (pc_clayout_is_rgb(to)) {
                o[k++] = rgb[0];
                o[k++] = rgb[1];
                o[k++] = rgb[2];
            } else {
                float kk = 1.0f - pc_rs_maxf(pc_rs_maxf(rgb[0], rgb[1]), rgb[2]);
                if (kk >= 1.0f) {
                    o[k++] = 0.0f;
                    o[k++] = 0.0f;
                    o[k++] = 0.0f;
                    o[k++] = 1.0f;
                } else {
                    float d = 1.0f - kk;
                    o[k++] = (1.0f - rgb[0] - kk) / d;
                    o[k++] = (1.0f - rgb[1] - kk) / d;
                    o[k++] = (1.0f - rgb[2] - kk) / d;
                    o[k++] = kk;
                }
            }
        }
        if (pc_clayout_has_alpha(to)) {
            o[k] = alpha;
        }
    }
}

/* Integer fast path of pc_cimage_convert for U8/U16 sources and targets whose result is a pure
 * channel copy: same colour model (alpha added as opaque, or dropped) or gray to RGB.
 * Bit-identical to the generic f32 route (the equivalence tests check every value) without the
 * two intermediate float buffers. False when it does not apply. */
static bool convert_int_direct(const pc_cimage *img, pc_clayout l, pc_csample s, uint8_t *out)
{
    size_t fc = pc_clayout_channels(img->layout);
    size_t sb = pc_csample_bytes(img->sample);
    size_t src_color = pc_clayout_color_channels(img->layout);
    size_t dst_color = pc_clayout_color_channels(l);
    bool src_alpha = pc_clayout_has_alpha(img->layout);
    bool dst_alpha = pc_clayout_has_alpha(l);
    size_t n;
    size_t p;
    size_t o = 0;
    if ((img->sample != PC_CS_U8 && img->sample != PC_CS_U16) || (s != PC_CS_U8 && s != PC_CS_U16)) {
        return false;
    }
    if (!(pc_clayout_is_gray(img->layout) && pc_clayout_is_rgb(l)) &&
        !pc_clayout_same_model(l, img->layout)) {
        return false;
    }
    n = img->len / (fc * sb);
    for (p = 0; p < n; p++) {
        const uint8_t *px = img->data + p * fc * sb;
        size_t j;
        for (j = 0; j < dst_color + (dst_alpha ? 1 : 0); j++) {
            const uint8_t *smp;
            uint16_t v16;
            if (j == dst_color) {
                if (!src_alpha) {
                    if (s == PC_CS_U8) {
                        out[o++] = 0xFF;
                    } else {
                        v16 = 0xFFFF;
                        memcpy(out + o, &v16, 2);
                        o += 2;
                    }
                    continue;
                }
                smp = px + src_color * sb;
            } else {
                smp = px + (src_color == 1 ? 0 : j) * sb;
            }
            if (img->sample == PC_CS_U8 && s == PC_CS_U8) {
                out[o++] = smp[0];
            } else if (img->sample == PC_CS_U16 && s == PC_CS_U16) {
                memcpy(out + o, smp, 2);
                o += 2;
            } else if (img->sample == PC_CS_U8) {
                v16 = (uint16_t)(smp[0] * 257u);
                memcpy(out + o, &v16, 2);
                o += 2;
            } else {
                memcpy(&v16, smp, 2);
                out[o++] = (uint8_t)(((uint32_t)v16 + 128u) / 257u);
            }
        }
    }
    return true;
}

pc_status pc_cimage_convert(const pc_cimage *img, pc_clayout l, pc_csample s, pc_cimage *out, pc_error *err)
{
    pc_cimage c;
    size_t px = pc_cimage_pixel_count(img);
    size_t len = 0;
    uint8_t *data = NULL;
    pc_status st;
    pc_cimage_init(&c);
    if (l == img->layout && s == img->sample) {
        return pc_cimage_clone(img, out, err);
    }
    if (!byte_len(img->width, img->height, img->layout, img->sample, &len) || len != img->len) {
        return cx_invalid(err, "buffer has %zu bytes, expected %zu", img->len, len);
    }
    if (!byte_len(img->width, img->height, l, s, &len)) {
        return cx_invalid(err, "image size overflows usize");
    }
    data = malloc(len > 0 ? len : 1);
    if (data == NULL) {
        return cx_limit(err, "cannot allocate %zu bytes for the image", len);
    }
    if (s == img->sample && !pc_clayout_has_alpha(img->layout) && l == pc_clayout_with_alpha(img->layout)) {
        /* Fast path (opening any opaque file): copy the samples, append an opaque alpha. */
        size_t bps = pc_csample_bytes(s);
        size_t srcb = pc_clayout_channels(img->layout) * bps;
        uint8_t one[4];
        size_t p;
        switch (s) {
        case PC_CS_U8:
            one[0] = 0xFF;
            break;
        case PC_CS_U16: {
            uint16_t v = 0xFFFF;
            memcpy(one, &v, 2);
            break;
        }
        case PC_CS_F16: {
            uint16_t v = 0x3C00;
            memcpy(one, &v, 2);
            break;
        }
        default: {
            float v = 1.0f;
            memcpy(one, &v, 4);
        }
        }
        for (p = 0; p < px; p++) {
            memcpy(data + p * (srcb + bps), img->data + p * srcb, srcb);
            memcpy(data + p * (srcb + bps) + srcb, one, bps);
        }
    } else if (convert_int_direct(img, l, s, data)) {
        /* done */
    } else {
        float *v = NULL;
        size_t n;
        CK(pc_cimage_to_normalized(img, &v, &n, err));
        if (l == img->layout) {
            cx_quantize(v, n, s, data);
        } else {
            size_t tn;
            float *t = NULL;
            if (!pc_size_mul(px, pc_clayout_channels(l), &tn) ||
                (t = pc_malloc_array(tn > 0 ? tn : 1, sizeof *t)) == NULL) {
                free(v);
                st = cx_nomem(err);
                goto fail;
            }
            cx_convert_layout(v, px, img->layout, l, t);
            cx_quantize(t, tn, s, data);
            free(t);
        }
        free(v);
    }
    c.width = img->width;
    c.height = img->height;
    c.layout = l;
    c.sample = s;
    c.data = data;
    c.len = len;
    data = NULL;
    if (pc_clayout_same_model(l, img->layout)) {
        CK(pc_meta_blob_clone(&img->icc, &c.icc, err));
    }
    CK(pc_cmeta_clone(&img->meta, &c.meta, err));
    if (img->nwarnings > 0) {
        c.warnings = pc_malloc_array(img->nwarnings, sizeof *c.warnings);
        if (c.warnings == NULL) {
            st = cx_nomem(err);
            goto fail;
        }
        memcpy(c.warnings, img->warnings, img->nwarnings * sizeof *c.warnings);
        c.nwarnings = img->nwarnings;
    }
    *out = c;
    return PC_OK;
fail:
    free(data);
    pc_cimage_fini(&c);
    return st;
}

pc_status pc_cimage_converted(const pc_cimage *img, pc_clayout l, pc_csample s, pc_cimage *tmp,
                              const pc_cimage **out, pc_error *err)
{
    pc_status st;
    pc_cimage_init(tmp);
    if (l == img->layout && s == img->sample) {
        *out = img;
        return PC_OK;
    }
    st = pc_cimage_convert(img, l, s, tmp, err);
    if (st == PC_OK) {
        *out = tmp;
    }
    return st;
}

pc_status pc_cimage_to_rgba8(const pc_cimage *img, uint8_t **out, size_t *n, pc_error *err)
{
    pc_cimage c;
    pc_status st = pc_cimage_convert(img, PC_CL_RGBA, PC_CS_U8, &c, err);
    if (st != PC_OK) {
        return st;
    }
    *out = c.data;
    *n = c.len;
    c.data = NULL;
    pc_cimage_fini(&c);
    return PC_OK;
}

pc_status pc_cimage_to_rgba16(const pc_cimage *img, uint16_t **out, size_t *n, pc_error *err)
{
    pc_cimage c;
    pc_status st = pc_cimage_convert(img, PC_CL_RGBA, PC_CS_U16, &c, err);
    if (st != PC_OK) {
        return st;
    }
    *out = (uint16_t *)(void *)c.data;
    *n = c.len / 2;
    c.data = NULL;
    pc_cimage_fini(&c);
    return PC_OK;
}

pc_status pc_cimage_to_rgba_f32(const pc_cimage *img, float **out, size_t *n, pc_error *err)
{
    float *v = NULL;
    float *t = NULL;
    size_t nv;
    size_t px = pc_cimage_pixel_count(img);
    pc_status st = pc_cimage_to_normalized(img, &v, &nv, err);
    if (st != PC_OK) {
        return st;
    }
    if (v == NULL || nv / pc_clayout_channels(img->layout) < px) {
        free(v);
        if (px == 0) {
            *out = NULL;
            *n = 0;
            return PC_OK;
        }
        return cx_invalid(err, "buffer has %zu samples, expected %zu", nv, px * pc_clayout_channels(img->layout));
    }
    if ((t = pc_malloc_array(px, 4 * sizeof *t)) == NULL) {
        free(v);
        return cx_nomem(err);
    }
    cx_convert_layout(v, px, img->layout, PC_CL_RGBA, t);
    free(v);
    *out = t;
    *n = px * 4;
    return PC_OK;
}

/* ---- deep images ---- */

void pc_cdeep_fini(pc_cdeep *d)
{
    size_t i;
    if (d == NULL) {
        return;
    }
    for (i = 0; i < d->nchannels; i++) {
        free(d->channels[i].name);
        free(d->channels[i].samples);
    }
    free(d->channels);
    free(d->counts);
    memset(d, 0, sizeof *d);
}

const pc_cdeep_channel *pc_cdeep_channel_named(const pc_cdeep *d, const char *name)
{
    size_t i;
    for (i = 0; i < d->nchannels; i++) {
        if (strcmp(d->channels[i].name, name) == 0) {
            return &d->channels[i];
        }
    }
    return NULL;
}

void pc_cdeep_sample_range(const pc_cdeep *d, size_t pixel, uint64_t *start, uint64_t *end)
{
    uint64_t e = pixel + 1 < d->ncounts ? d->counts[pixel + 1] : 0;
    *end = e;
    *start = pixel < d->ncounts ? d->counts[pixel] : e;
}

uint64_t pc_cdeep_total_samples(const pc_cdeep *d) { return d->ncounts > 0 ? d->counts[d->ncounts - 1] : 0; }

/* ---- multi-part OpenEXR and Cryptomatte ---- */

void pc_exr_part_info_fini(pc_exr_part_info *p)
{
    size_t i;
    if (p == NULL) {
        return;
    }
    free(p->name);
    free(p->view);
    for (i = 0; i < p->nchannels; i++) {
        free(p->channels[i].name);
    }
    free(p->channels);
    memset(p, 0, sizeof *p);
}

void pc_exr_parts_free(pc_exr_part_info *parts, size_t n)
{
    size_t i;
    for (i = 0; parts != NULL && i < n; i++) {
        pc_exr_part_info_fini(&parts[i]);
    }
    free(parts);
}

/* Strips an optional "layer." prefix ("diffuse.R" -> "R"). */
static const char *base_channel(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL ? dot + 1 : name;
}

static bool part_has(const pc_exr_part_info *p, const char *n)
{
    size_t i;
    for (i = 0; i < p->nchannels; i++) {
        if (strcmp(base_channel(p->channels[i].name), n) == 0) {
            return true;
        }
    }
    return false;
}

uint8_t pc_exr_part_color_rank(const pc_exr_part_info *p)
{
    if (part_has(p, "R") && part_has(p, "G") && part_has(p, "B")) {
        return part_has(p, "A") ? 4 : 3;
    }
    if (part_has(p, "Y")) {
        return part_has(p, "A") ? 2 : 1;
    }
    return p->nchannels == 1 ? 1 : 0;
}

void pc_crypto_layer_fini(pc_crypto_layer *l)
{
    size_t i;
    if (l == NULL) {
        return;
    }
    free(l->name);
    free(l->key);
    free(l->conversion);
    free(l->sidecar_manifest);
    for (i = 0; i < l->nmanifest; i++) {
        free(l->manifest[i].name);
    }
    free(l->manifest);
    for (i = 0; i < l->nchannels; i++) {
        free(l->channels[i]);
    }
    free(l->channels);
    memset(l, 0, sizeof *l);
}

void pc_crypto_layers_free(pc_crypto_layer *layers, size_t n)
{
    size_t i;
    for (i = 0; layers != NULL && i < n; i++) {
        pc_crypto_layer_fini(&layers[i]);
    }
    free(layers);
}

const char *pc_crypto_layer_name_of(const pc_crypto_layer *l, float id)
{
    size_t i;
    for (i = 0; i < l->nmanifest; i++) {
        if (l->manifest[i].id == id) {
            return l->manifest[i].name;
        }
    }
    return NULL;
}

void pc_crypto_buffer_fini(pc_crypto_buffer *b)
{
    if (b == NULL) {
        return;
    }
    free(b->pairs);
    free(b->start);
    memset(b, 0, sizeof *b);
}

const pc_crypto_pair *pc_crypto_buffer_at(const pc_crypto_buffer *b, uint32_t x, uint32_t y, size_t *n)
{
    /* As the reference: the flat index y * width + x, whatever x is. */
    size_t i = (size_t)y * b->width + x;
    *n = 0;
    if (b->start == NULL || i >= (size_t)b->width * b->height || i < x) {
        return NULL;
    }
    *n = b->start[i + 1] - b->start[i];
    return *n > 0 ? b->pairs + b->start[i] : NULL;
}
