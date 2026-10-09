#include "lut3d.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/mem.h"
#include "base/num.h"

pc_status pc_lut3d_from_transform(const pc_transform *t, size_t size, pc_lut3d *out, pc_error *err)
{
    size_t n;
    size_t r;
    size_t g;
    size_t b;
    float s;
    float o[PC_PIPE_MAX];
    float *data;
    size_t outs = pc_transform_outputs(t);
    if (pc_transform_inputs(t) != 3 || (outs != 3 && outs != 1)) {
        return pc_fail(err, PC_ERR_INVALID, "display LUTs need a 3\xe2\x86\x92" "3 or 3\xe2\x86\x92" "1 transform");
    }
    if (size < 2 || size > 256 || !pc_size_mul3(size, size, size, &n)) {
        return pc_fail(err, PC_ERR_INVALID, "LUT size");
    }
    data = pc_malloc_array(n, 4 * sizeof *data);
    if (data == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s = (float)(size - 1);
    memset(o, 0, sizeof o);
    for (b = 0; b < size; b++) {
        for (g = 0; g < size; g++) {
            for (r = 0; r < size; r++) {
                float in[3];
                float *px = data + (r + g * size + b * size * size) * 4;
                in[0] = (float)r / s;
                in[1] = (float)g / s;
                in[2] = (float)b / s;
                pc_transform_eval(t, in, o);
                if (outs == 1) {
                    px[0] = px[1] = px[2] = o[0];
                } else {
                    px[0] = o[0];
                    px[1] = o[1];
                    px[2] = o[2];
                }
                px[3] = 1.0f;
            }
        }
    }
    out->size = size;
    out->data = data;
    return PC_OK;
}

void pc_lut3d_fini(pc_lut3d *l)
{
    if (l != NULL) {
        free(l->data);
        l->data = NULL;
        l->size = 0;
    }
}

static void loc(size_t n, float v, size_t *i, float *f)
{
    float p = pc_clampf(v, 0.0f, 1.0f) * (float)(n - 1);
    size_t k = pc_f32_to_size(p);
    if (k > n - 2) {
        k = n - 2;
    }
    *i = k;
    *f = p - (float)k;
}

void pc_lut3d_sample(const pc_lut3d *l, const float rgb[3], float out[3])
{
    size_t n = l->size;
    size_t r0;
    size_t g0;
    size_t b0;
    float fr;
    float fg;
    float fb;
    int k;
    loc(n, rgb[0], &r0, &fr);
    loc(n, rgb[1], &g0, &fg);
    loc(n, rgb[2], &b0, &fb);
#define AT(r, g, b) (l->data[((r) + (g) * n + (b) * n * n) * 4 + (size_t)k])
    for (k = 0; k < 3; k++) {
        float x00 = AT(r0, g0, b0) + (AT(r0 + 1, g0, b0) - AT(r0, g0, b0)) * fr;
        float x10 = AT(r0, g0 + 1, b0) + (AT(r0 + 1, g0 + 1, b0) - AT(r0, g0 + 1, b0)) * fr;
        float x01 = AT(r0, g0, b0 + 1) + (AT(r0 + 1, g0, b0 + 1) - AT(r0, g0, b0 + 1)) * fr;
        float x11 = AT(r0, g0 + 1, b0 + 1) + (AT(r0 + 1, g0 + 1, b0 + 1) - AT(r0, g0 + 1, b0 + 1)) * fr;
        float y0 = x00 + (x10 - x00) * fg;
        float y1 = x01 + (x11 - x01) * fg;
        out[k] = y0 + (y1 - y0) * fb;
    }
#undef AT
}

uint16_t pc_lut3d_f32_to_f16(float v)
{
    uint32_t x = pc_f32_to_bits(v);
    uint16_t sign = (uint16_t)((x >> 16) & 0x8000u);
    int32_t exp = (int32_t)((x >> 23) & 0xFFu);
    uint32_t man = x & 0x7FFFFFu;
    int32_t e;
    uint32_t r;
    uint32_t rem;
    if (exp == 0xFF) {
        return (uint16_t)(sign | 0x7C00u | (man != 0 ? 0x200u : 0u));
    }
    e = exp - 127 + 15;
    if (e >= 0x1F) {
        return (uint16_t)(sign | 0x7C00u);
    }
    if (e <= 0) {
        uint32_t m;
        uint32_t shift;
        uint32_t half;
        if (e < -10) {
            return sign;
        }
        m = man | 0x800000u;
        shift = (uint32_t)(14 - e);
        half = 1u << (shift - 1);
        rem = m & ((1u << shift) - 1);
        r = m >> shift;
        if (rem > half || (rem == half && (r & 1u) == 1u)) {
            r++;
        }
        return (uint16_t)(sign | r);
    }
    r = ((uint32_t)e << 10) | (man >> 13);
    rem = man & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (r & 1u) == 1u)) {
        r++;
    }
    return (uint16_t)(sign | r);
}

float pc_lut3d_f16_to_f32(uint16_t h)
{
    float sign = (h & 0x8000u) != 0 ? -1.0f : 1.0f;
    int e = (h >> 10) & 0x1F;
    float m = (float)(h & 0x3FFu);
    if (e == 0) {
        return sign * m * ldexpf(1.0f, -24);
    }
    if (e == 0x1F) {
        return m == 0.0f ? sign * INFINITY : NAN;
    }
    return sign * (1.0f + m / 1024.0f) * ldexpf(1.0f, e - 15);
}

pc_status pc_lut3d_to_rgba16f(const pc_lut3d *l, pc_buf *out, pc_error *err)
{
    size_t n = l->size * l->size * l->size * 4;
    size_t i;
    size_t start = out->len;
    pc_status s = pc_buf_reserve(out, n * 2, err);
    for (i = 0; i < n && s == PC_OK; i++) {
        s = pc_buf_put_le16(out, pc_lut3d_f32_to_f16(l->data[i]), err);
    }
    if (s != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return s;
}

pc_status pc_lut3d_to_rgba8(const pc_lut3d *l, pc_buf *out, pc_error *err)
{
    size_t n = l->size * l->size * l->size * 4;
    size_t i;
    size_t start = out->len;
    pc_status s = pc_buf_reserve(out, n, err);
    for (i = 0; i < n && s == PC_OK; i++) {
        s = pc_buf_append_byte(out, pc_f32_to_u8(pc_clampf(l->data[i], 0.0f, 1.0f) * 255.0f + 0.5f), err);
    }
    if (s != PC_OK) {
        pc_buf_truncate(out, start);
    }
    return s;
}
