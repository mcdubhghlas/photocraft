/* Bit-exact parity with crates/color: replays `rustref color-vectors` (tools/rustref) and
 * compares FNV-1a hashes of every output's bits. Expected values come from rustc 1.95. */
#include <float.h>
#include <math.h>
#include <string.h>

#include "base/buf.h"
#include "cms/profile.h"
#include "cms/synth.h"
#include "color/blend.h"
#include "color/color.h"
#include "color/convert.h"
#include "harness.h"

#define FNV0 0xcbf29ce484222325u

static void fnv(uint64_t *h, const uint8_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3u;
    }
}

static void h_f32(uint64_t *h, const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t u;
        uint8_t b[4];
        memcpy(&u, &v[i], 4);
        b[0] = (uint8_t)u;
        b[1] = (uint8_t)(u >> 8);
        b[2] = (uint8_t)(u >> 16);
        b[3] = (uint8_t)(u >> 24);
        fnv(h, b, 4);
    }
}

typedef struct lcg {
    uint64_t s;
} lcg;

static float next(lcg *r)
{
    r->s = r->s * 6364136223846793005u + 1442695040888963407u;
    return (float)(r->s >> 40) / 16777216.0f;
}

static const float V[19] = {-0.5f, -0.0f, 0.0f, 1e-5f, 0.1f, 0.25f, 0.3f, 0.5f, 0.50000006f, 0.6f,
                            0.75f, 0.9f,  1.0f - 1e-5f, 1.0f, 1.5f, 2.0f, FLT_MAX, INFINITY, NAN};

static void test_constants_and_blend_channel(void)
{
    uint64_t h = FNV0;
    unsigned m;
    size_t a;
    size_t b;
    /* Pass Through + the Photoshop modes (rustref's sweep); Paint.NET modes: blend-pdn. */
    for (m = 0; m < PC_BLEND_PSD_COUNT; m++) {
        for (a = 0; a < 19; a++) {
            for (b = 0; b < 19; b++) {
                float o[2];
                o[0] = pc_blend_channel((pc_blend_mode)m, V[a], V[b]);
                o[1] = pc_blend_channel_hdr((pc_blend_mode)m, V[a], V[b]);
                h_f32(&h, o, 2);
            }
        }
    }
    CHECK_EQ_UINT(h, 0xa81975973a462793u);
}

static float draw(lcg *r, int i)
{
    return i % 7 == 0 ? (next(r) * 1.4f - 0.2f) * 1.0f : next(r);
}

static void test_blend_rgb_and_composite(void)
{
    uint64_t hr = FNV0;
    uint64_t hc = FNV0;
    lcg r = {7};
    unsigned m;
    for (m = 0; m < PC_BLEND_PSD_COUNT; m++) {
        pc_blend_mode mode = (pc_blend_mode)m;
        int i;
        size_t k;
        for (i = 0; i < 3000; i++) {
            float cb[3];
            float cs[3];
            float ab;
            float as;
            float op;
            float o[3];
            float bd[4];
            float sr[4];
            float c4[4];
            for (k = 0; k < 3; k++) {
                cb[k] = draw(&r, i);
            }
            for (k = 0; k < 3; k++) {
                cs[k] = draw(&r, i);
            }
            ab = draw(&r, i);
            as = draw(&r, i);
            op = draw(&r, i);
            pc_blend_rgb(mode, cb, cs, o);
            h_f32(&hr, o, 3);
            pc_blend_rgb_hdr(mode, cb, cs, o);
            h_f32(&hr, o, 3);
            if (i % 11 == 0) {
                ab = 0.0f;
            }
            memcpy(bd, cb, sizeof cb);
            bd[3] = ab;
            memcpy(sr, cs, sizeof cs);
            sr[3] = as;
            pc_blend_composite(mode, bd, sr, op, c4);
            h_f32(&hc, c4, 4);
        }
        for (k = 0; k < 19; k++) {
            float g = V[k];
            float a[3];
            float b[3];
            float o[3];
            a[0] = g;
            a[1] = g;
            a[2] = 0.5f;
            b[0] = 0.5f;
            b[1] = g;
            b[2] = g;
            pc_blend_rgb(mode, a, b, o);
            h_f32(&hr, o, 3);
            a[0] = g;
            a[1] = 0.2f;
            a[2] = g;
            pc_blend_set_lum(a, 0.5f, o);
            h_f32(&hr, o, 3);
        }
    }
    CHECK_EQ_UINT(hr, 0x119aad840822706bu);
    CHECK_EQ_UINT(hc, 0xe5be77554b8b1e6du);
}

static void test_transfer_and_conversions(void)
{
    uint64_t ht = FNV0;
    uint64_t hl = FNV0;
    uint64_t hn = FNV0;
    uint64_t hm = FNV0;
    uint64_t hg = FNV0;
    lcg r = {11};
    uint32_t i;
    size_t k;
    static const float zero[3] = {0, 0, 0};
    float o4[4];
    for (i = 0; i <= 65535u; i++) {
        float x = (float)i / 65535.0f;
        float o[2];
        o[0] = pc_srgb_to_linear(x);
        o[1] = pc_linear_to_srgb(x);
        h_f32(&ht, o, 2);
    }
    for (k = 0; k < 19; k++) {
        float o[2];
        o[0] = pc_srgb_to_linear(V[k]);
        o[1] = pc_linear_to_srgb(V[k]);
        h_f32(&ht, o, 2);
    }
    CHECK_EQ_UINT(ht, 0xa70059f21a84a24eu);
    for (i = 0; i < 20000; i++) {
        float rgb[3];
        float lab[3];
        float c4[4];
        float o[4];
        float g;
        for (k = 0; k < 3; k++) {
            rgb[k] = next(&r) * 1.2f - 0.1f;
        }
        lab[0] = next(&r) * 100.0f;
        lab[1] = next(&r) * 256.0f - 128.0f;
        lab[2] = next(&r) * 256.0f - 128.0f;
        pc_srgb_to_lab(rgb, o);
        h_f32(&hl, o, 3);
        pc_lab_to_srgb(lab, o);
        h_f32(&hl, o, 3);
        for (k = 0; k < 4; k++) {
            c4[k] = next(&r);
        }
        pc_cmyk_to_rgb_naive(c4, o);
        h_f32(&hn, o, 3);
        pc_rgb_to_cmyk_naive(rgb, o);
        h_f32(&hn, o, 4);
        g = pc_rgb_to_gray(rgb);
        h_f32(&hg, &g, 1);
        if (i < 5000) {
            pc_cmyk_to_rgb(c4, o);
            h_f32(&hm, o, 3);
            pc_rgb_to_cmyk(rgb, o);
            h_f32(&hm, o, 4);
        }
    }
    pc_rgb_to_cmyk_naive(zero, o4);
    h_f32(&hn, o4, 4);
    CHECK_EQ_UINT(hl, 0xe4b5cb1fcd3f623fu);
    CHECK_EQ_UINT(hn, 0xf975347ddff7bf7bu);
    CHECK_EQ_UINT(hm, 0x645d1cfe6f045cacu);
    CHECK_EQ_UINT(hg, 0xe229edba0a042f74u);
}

static void test_document_cmyk_space(void)
{
    pc_cmyk_params params = pc_cmyk_params_default();
    pc_profile p;
    pc_buf bytes = PC_BUF_INIT;
    pc_cmyk_space *space;
    pc_cmyk_space *prev;
    uint64_t hd = FNV0;
    lcg r = {13};
    int i;
    params.description = "Test Uncoated";
    params.tvi[0] = params.tvi[1] = params.tvi[2] = 0.26;
    params.tvi[3] = 0.3;
    params.grid_a2b = 5;
    params.grid_b2a = 9;
    if (pc_synth_cmyk_profile(&params, &p, NULL) != PC_OK) {
        PC_T_FAIL("%s", "synth");
        return;
    }
    CHECK_EQ_INT(pc_profile_to_bytes(&p, &bytes, NULL), PC_OK);
    space = pc_cmyk_space_for_profile(bytes.data, bytes.len);
    CHECK(space != NULL);
    prev = pc_cmyk_space_enter(space);
    for (i = 0; i < 3000; i++) {
        float c4[4];
        float rgb[3];
        float o[4];
        int k;
        for (k = 0; k < 4; k++) {
            c4[k] = next(&r);
        }
        rgb[0] = next(&r) * 1.2f - 0.1f;
        rgb[1] = next(&r);
        rgb[2] = next(&r);
        pc_cmyk_to_rgb(c4, o);
        h_f32(&hd, o, 3);
        pc_rgb_to_cmyk(rgb, o);
        h_f32(&hd, o, 4);
    }
    pc_cmyk_space_leave(prev);
    CHECK_EQ_UINT(hd, 0x83c3bada1770fa16u);
    pc_cmyk_space_release(space);
    pc_profile_fini(&p);
    pc_buf_fini(&bytes);
}

static void test_dither_samples_and_colors(void)
{
    static const int32_t pairs[3][2] = {{INT32_MIN, INT32_MAX}, {INT32_MAX, 0}, {12345, -98765}};
    uint64_t hx = FNV0;
    uint64_t hs = FNV0;
    uint64_t hq = FNV0;
    lcg r = {17};
    int32_t x;
    int32_t y;
    int i;
    unsigned s;
    unsigned m;
    for (y = -60; y < 60; y++) {
        for (x = -60; x < 60; x++) {
            float d = pc_dither_noise(x, y);
            h_f32(&hx, &d, 1);
        }
    }
    for (i = 0; i < 3; i++) {
        float d = pc_dither_noise(pairs[i][0], pairs[i][1]);
        h_f32(&hx, &d, 1);
    }
    CHECK_EQ_UINT(hx, 0xd436168c85200b01u);
    for (s = 0; s < PC_SAMPLE_TYPE_COUNT; s++) {
        uint8_t buf[256];
        memset(buf, 0, sizeof buf);
        for (i = 0; i < 59; i++) {
            float val = i < 19 ? V[i] : next(&r) * 1.4f - 0.2f;
            float back;
            pc_write_sample(buf, sizeof buf, (pc_sample_type)s, (size_t)i, val);
            back = pc_read_sample(buf, sizeof buf, (pc_sample_type)s, (size_t)i);
            h_f32(&hs, &back, 1);
        }
        fnv(&hs, buf, sizeof buf);
    }
    CHECK_EQ_UINT(hs, 0x8b721f7c7b736ed0u);
    r.s = 19;
    for (m = 0; m < PC_COLOR_MODE_COUNT; m++) {
        for (i = 0; i < 500; i++) {
            pc_color c;
            float rgb[3];
            uint8_t q[4];
            int k;
            c.mode = (pc_color_mode)m;
            for (k = 0; k < 4; k++) {
                c.c[k] = next(&r);
            }
            c.alpha = next(&r) * 1.2f - 0.1f;
            pc_color_to_rgb(&c, rgb);
            h_f32(&hq, rgb, 3);
            pc_color_to_rgba8(&c, q);
            fnv(&hq, q, 4);
        }
    }
    CHECK_EQ_UINT(hq, 0xcddef6aeea8c40a6u);
}

static float draw5(lcg *r, int i)
{
    return i % 5 == 0 ? (next(r) * 1.4f - 0.2f) * 1.0f : next(r);
}

/* rustref color-vectors `blend-pdn`: the Paint.NET modes. */
static void test_paint_net_modes(void)
{
    uint64_t h = FNV0;
    lcg r = {23};
    size_t m;
    for (m = 0; m < PC_BLEND_PAINT_NET_MODE_COUNT; m++) {
        pc_blend_mode mode = PC_BLEND_PAINT_NET_MODES[m];
        size_t a;
        size_t b;
        int i;
        for (a = 0; a < 19; a++) {
            for (b = 0; b < 19; b++) {
                float o[2];
                o[0] = pc_blend_channel(mode, V[a], V[b]);
                o[1] = pc_blend_channel_hdr(mode, V[a], V[b]);
                h_f32(&h, o, 2);
            }
        }
        for (i = 0; i < 2000; i++) {
            float cb[3];
            float cs[3];
            float o[4];
            float bd[4];
            float sr[4];
            float ab;
            float as;
            float op;
            size_t k;
            for (k = 0; k < 3; k++) {
                cb[k] = draw5(&r, i);
            }
            for (k = 0; k < 3; k++) {
                cs[k] = draw5(&r, i);
            }
            ab = draw5(&r, i);
            as = draw5(&r, i);
            op = draw5(&r, i);
            pc_blend_rgb(mode, cb, cs, o);
            h_f32(&h, o, 3);
            pc_blend_rgb_hdr(mode, cb, cs, o);
            h_f32(&h, o, 3);
            memcpy(bd, cb, sizeof cb);
            bd[3] = ab;
            memcpy(sr, cs, sizeof cs);
            sr[3] = as;
            pc_blend_composite(mode, bd, sr, op, o);
            h_f32(&h, o, 4);
        }
    }
    CHECK_EQ_UINT(h, 0x9c7158d7c521f99du);
}

/* rustref color-vectors `blend-within`: Hue/Saturation/Color/Luminosity clipped at 1, 2 and
 * FLT_MAX, HDR inputs and greys (the guarded ClipColor). */
static void test_blend_rgb_within(void)
{
    static const pc_blend_mode modes[4] = {PC_BLEND_HUE, PC_BLEND_SATURATION, PC_BLEND_COLOR,
                                           PC_BLEND_LUMINOSITY};
    static const float tops[3] = {1.0f, 2.0f, FLT_MAX};
    uint64_t h = FNV0;
    lcg r = {29};
    size_t m;
    for (m = 0; m < 4; m++) {
        int i;
        size_t g;
        for (i = 0; i < 3000; i++) {
            float k = i % 3 == 0 ? 4.0f : 1.2f;
            float cb[3];
            float cs[3];
            float o[3];
            size_t c;
            for (c = 0; c < 3; c++) {
                cb[c] = next(&r) * k - 0.1f;
            }
            for (c = 0; c < 3; c++) {
                cs[c] = next(&r) * k - 0.1f;
            }
            pc_blend_rgb_within(modes[m], cb, cs, tops[i % 3], o);
            h_f32(&h, o, 3);
        }
        for (g = 0; g < 19; g++) {
            size_t t;
            for (t = 0; t < 3; t++) {
                float grey[3] = {V[g], V[g], V[g]};
                float col[3] = {0.5f, 0.25f, 0.125f};
                float near[3] = {V[g], V[g], 1.999999f};
                float o[3];
                pc_blend_rgb_within(modes[m], grey, col, tops[t], o);
                h_f32(&h, o, 3);
                pc_blend_rgb_within(modes[m], col, near, tops[t], o);
                h_f32(&h, o, 3);
            }
        }
    }
    CHECK_EQ_UINT(h, 0x4af56bf1f6d6d438u);
}

/* rustref color-vectors `in-mode`: pc_color_in_mode between every pair of modes. */
static void test_color_in_mode(void)
{
    uint64_t h = FNV0;
    lcg r = {31};
    unsigned from;
    unsigned to;
    for (from = 0; from < PC_COLOR_MODE_COUNT; from++) {
        for (to = 0; to < PC_COLOR_MODE_COUNT; to++) {
            int i;
            for (i = 0; i < 60; i++) {
                pc_color c;
                pc_color o;
                uint8_t mb;
                int k;
                c.mode = (pc_color_mode)from;
                for (k = 0; k < 4; k++) {
                    c.c[k] = next(&r);
                }
                c.alpha = next(&r);
                o = pc_color_in_mode(&c, (pc_color_mode)to);
                mb = (uint8_t)o.mode;
                fnv(&h, &mb, 1);
                h_f32(&h, o.c, 4);
                h_f32(&h, &o.alpha, 1);
            }
        }
    }
    CHECK_EQ_UINT(h, 0x6dbc8eed39540b66u);
}

int main(int argc, char **argv)
{
    RUN(test_constants_and_blend_channel);
    RUN(test_blend_rgb_and_composite);
    RUN(test_transfer_and_conversions);
    RUN(test_document_cmyk_space);
    RUN(test_dither_samples_and_colors);
    RUN(test_paint_net_modes);
    RUN(test_blend_rgb_within);
    RUN(test_color_in_mode);
    return pc_t_finish();
}
