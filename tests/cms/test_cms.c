/* Ports crates/cms/tests/cms.rs: built-in profiles, known colorimetric values, round trips,
 * CMYK behaviour, intents, BPC, interpolation accuracy, malformed input, real-world profiles.
 * The two moxcms oracle tests are not ported (Rust-only dev-dependency); bit-exact parity with
 * the Rust implementation (test_parity.c) covers them transitively. */
#define _XOPEN_SOURCE 700 /* opendir/readdir are POSIX; harmless here */
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/mem.h"
#include "cms/builtin.h"
#include "cms/clut.h"
#include "cms/math.h"
#include "cms/profile.h"
#include "cms/transform.h"
#include "cms/lut3d.h"
#include "harness.h"

static const pc_profile *lab(void)
{
    return pc_builtin_profile(PC_BUILTIN_LAB_D50);
}
static const pc_profile *srgb(void)
{
    return pc_builtin_profile(PC_BUILTIN_SRGB);
}
static const pc_profile *cmyk(void)
{
    return pc_builtin_profile(PC_BUILTIN_COATED_CMYK);
}

/* Normalised device Lab (v4 encoding) -> float Lab, and back. */
static void dec(const float *o, double out[3])
{
    out[0] = (double)o[0] * 100.0;
    out[1] = (double)o[1] * 255.0 - 128.0;
    out[2] = (double)o[2] * 255.0 - 128.0;
}
static void enc(const double l[3], float out[3])
{
    out[0] = (float)(l[0] / 100.0);
    out[1] = (float)((l[1] + 128.0) / 255.0);
    out[2] = (float)((l[2] + 128.0) / 255.0);
}

static pc_transform *mk(const pc_profile *s, const pc_profile *d, pc_intent intent, bool bpc)
{
    pc_transform *t = NULL;
    pc_error e;
    if (pc_transform_new(s, d, intent, bpc, &t, &e) != PC_OK) {
        PC_T_FAIL("transform: %s", e.msg);
        return NULL;
    }
    return t;
}

/* Evaluates through t, or zeroes out when t is NULL (already reported). */
static void ev(const pc_transform *t, const float *in, float *out, size_t nout)
{
    if (t == NULL) {
        memset(out, 0, nout * sizeof *out);
        return;
    }
    pc_transform_eval(t, in, out);
}

static void to_lab(const pc_profile *p, const float *dev, pc_intent intent, double out[3])
{
    pc_transform *t = mk(p, lab(), intent, false);
    float o[3];
    ev(t, dev, o, 3);
    dec(o, out);
    pc_transform_release(t);
}

static bool all_finite(const float *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!isfinite(v[i])) {
            return false;
        }
    }
    return true;
}

static void test_builtins_encode_and_reparse(void)
{
    pc_builtin b;
    for (b = PC_BUILTIN_SRGB; b < PC_BUILTIN_COUNT; b = (pc_builtin)(b + 1)) {
        const pc_profile *p = pc_builtin_profile(b);
        pc_buf bytes = PC_BUF_INIT;
        pc_buf again = PC_BUF_INIT;
        pc_profile q;
        pc_builtin back;
        pc_transform *t;
        CHECK(p != NULL);
        if (p == NULL) {
            continue;
        }
        CHECK_EQ_INT(pc_profile_to_bytes(p, &bytes, NULL), PC_OK);
        CHECK_EQ_INT(pc_profile_parse(bytes.data, bytes.len, &q, NULL), PC_OK);
        CHECK_EQ_STR(q.description, pc_builtin_description(b));
        CHECK_EQ_UINT(q.space, p->space);
        CHECK_EQ_INT(pc_profile_to_bytes(&q, &again, NULL), PC_OK);
        CHECK(again.len == bytes.len && memcmp(again.data, bytes.data, bytes.len) == 0); /* kept verbatim */
        CHECK(pc_builtin_from_id(pc_builtin_id(b), strlen(pc_builtin_id(b)), &back) && back == b);
        t = mk(p, lab(), PC_INTENT_RELATIVE, false);
        pc_transform_release(t);
        t = mk(lab(), p, PC_INTENT_RELATIVE, false);
        pc_transform_release(t);
        pc_profile_fini(&q);
        pc_buf_fini(&bytes);
        pc_buf_fini(&again);
    }
}

static void test_srgb_known_lab_values(void)
{
    static const float white[3] = {1, 1, 1};
    static const float black[3] = {0, 0, 0};
    static const float red[3] = {1, 0, 0};
    static const float green[3] = {0, 1, 0};
    static const float blue[3] = {0, 0, 1};
    static const float gray[3] = {0.5f, 0.5f, 0.5f};
    static const double red_lab[3] = {54.29, 80.80, 69.89};
    static const double green_lab[3] = {87.82, -79.29, 80.99};
    static const double blue_lab[3] = {29.57, 68.30, -112.03};
    static const pc_builtin rgbs[6] = {PC_BUILTIN_SRGB,        PC_BUILTIN_DISPLAY_P3,  PC_BUILTIN_ADOBE_RGB_COMPAT,
                                       PC_BUILTIN_PROPHOTO_COMPAT, PC_BUILTIN_LINEAR_SRGB, PC_BUILTIN_REC2020};
    double l[3];
    int i;
    to_lab(srgb(), white, PC_INTENT_RELATIVE, l);
    CHECK(fabs(l[0] - 100.0) < 0.01 && fabs(l[1]) < 0.01 && fabs(l[2]) < 0.01);
    to_lab(srgb(), black, PC_INTENT_RELATIVE, l);
    CHECK(fabs(l[0]) < 0.01);
    /* ICC PCS is D50: sRGB red Bradford-adapted to D50. */
    to_lab(srgb(), red, PC_INTENT_RELATIVE, l);
    CHECK(pc_delta_e76(l, red_lab) < 0.15);
    to_lab(srgb(), green, PC_INTENT_RELATIVE, l);
    CHECK(pc_delta_e76(l, green_lab) < 0.3);
    to_lab(srgb(), blue, PC_INTENT_RELATIVE, l);
    CHECK(pc_delta_e76(l, blue_lab) < 0.3);
    for (i = 0; i < 6; i++) {
        to_lab(pc_builtin_profile(rgbs[i]), gray, PC_INTENT_RELATIVE, l);
        CHECK(fabs(l[1]) < 0.02 && fabs(l[2]) < 0.02);
    }
}

static void test_rgb_lab_rgb_roundtrip_8bit_and_float(void)
{
    pc_transform *to = mk(srgb(), lab(), PC_INTENT_RELATIVE, false);
    pc_transform *back = mk(lab(), srgb(), PC_INTENT_RELATIVE, false);
    size_t n = 0;
    size_t cap = 52 * 37 * 29 * 3;
    uint8_t *src8 = pc_malloc_array(cap, 1);
    float *f = pc_malloc_array(cap, sizeof *f);
    float *labf = pc_malloc_array(cap, sizeof *labf);
    float *rgbf = pc_malloc_array(cap, sizeof *rgbf);
    uint16_t *lab16 = pc_malloc_array(cap, sizeof *lab16);
    uint16_t *rgb16 = pc_malloc_array(cap, sizeof *rgb16);
    uint8_t *lab8 = pc_malloc_array(cap, 1);
    int bad8 = 0;
    int bad16 = 0;
    int badf = 0;
    int badq = 0;
    int r;
    int g;
    int b;
    size_t i;
    if (to == NULL || back == NULL || !src8 || !f || !labf || !rgbf || !lab16 || !rgb16 || !lab8) {
        PC_T_FAIL("%s", "setup");
        goto done;
    }
    for (r = 0; r < 256; r += 5) {
        for (g = 0; g < 256; g += 7) {
            for (b = 0; b < 256; b += 9) {
                src8[n++] = (uint8_t)r;
                src8[n++] = (uint8_t)g;
                src8[n++] = (uint8_t)b;
            }
        }
    }
    for (i = 0; i < n; i++) {
        f[i] = (float)src8[i] / 255.0f;
    }
    CHECK_EQ_INT(pc_transform_convert_f32(to, f, n, 3, labf, n, 3, false, NULL), PC_OK);
    CHECK_EQ_INT(pc_transform_convert_f32(back, labf, n, 3, rgbf, n, 3, false, NULL), PC_OK);
    for (i = 0; i < n; i++) {
        bad8 += fabsf((float)src8[i] - rgbf[i] * 255.0f) > 1.0f;
        badf += fabsf(f[i] - rgbf[i]) >= 1e-4f;
    }
    /* 8-bit RGB stored as 16-bit Lab, then back through the lookup tables. */
    for (i = 0; i < n; i++) {
        lab16[i] = (uint16_t)(fminf(fmaxf(labf[i], 0.0f), 1.0f) * 65535.0f + 0.5f);
    }
    CHECK_EQ_INT(pc_transform_convert_u16(back, lab16, n, 3, rgb16, n, 3, false, NULL), PC_OK);
    for (i = 0; i < n; i++) {
        bad16 += fabsf((float)src8[i] - (float)rgb16[i] / 257.0f) > 1.0f;
    }
    /* Fast 8-bit path vs the exact float Lab, within one level of the Lab encoding. */
    CHECK_EQ_INT(pc_transform_convert_u8(to, src8, n, 3, lab8, n, 3, false, NULL), PC_OK);
    for (i = 0; i < n; i++) {
        badq += fabsf((float)lab8[i] - labf[i] * 255.0f) > 1.0f;
    }
    CHECK_EQ_INT(bad8, 0);
    CHECK_EQ_INT(badf, 0);
    CHECK_EQ_INT(bad16, 0);
    CHECK_EQ_INT(badq, 0);
done:
    free(src8);
    free(f);
    free(labf);
    free(rgbf);
    free(lab16);
    free(rgb16);
    free(lab8);
    pc_transform_release(to);
    pc_transform_release(back);
}

static void test_rgb_to_rgb_matrix_shaper_paths_agree(void)
{
    static const pc_builtin dsts[4] = {PC_BUILTIN_ADOBE_RGB_COMPAT, PC_BUILTIN_PROPHOTO_COMPAT, PC_BUILTIN_DISPLAY_P3,
                                       PC_BUILTIN_LINEAR_SRGB};
    int d;
    for (d = 0; d < 4; d++) {
        pc_transform *t = mk(srgb(), pc_builtin_profile(dsts[d]), PC_INTENT_RELATIVE, false);
        uint8_t src[256 * 3];
        uint8_t out[256 * 3];
        int bad = 0;
        int v;
        if (t == NULL) {
            continue;
        }
        for (v = 0; v < 256; v++) {
            src[v * 3] = (uint8_t)v;
            src[v * 3 + 1] = (uint8_t)(v / 2);
            src[v * 3 + 2] = (uint8_t)(255 - v);
        }
        CHECK_EQ_INT(pc_transform_convert_u8(t, src, sizeof src, 3, out, sizeof out, 3, false, NULL), PC_OK);
        for (v = 0; v < 256; v++) {
            float in[3];
            float e[3];
            int k;
            for (k = 0; k < 3; k++) {
                in[k] = (float)src[v * 3 + k] / 255.0f;
            }
            pc_transform_eval(t, in, e);
            for (k = 0; k < 3; k++) {
                bad += fabsf((float)out[v * 3 + k] - fminf(fmaxf(e[k], 0.0f), 1.0f) * 255.0f) > 0.6f;
            }
        }
        CHECK_EQ_INT(bad, 0);
        pc_transform_release(t);
    }
}

static void test_identity_transform_is_identity(void)
{
    pc_transform *t = mk(srgb(), srgb(), PC_INTENT_RELATIVE, true);
    uint8_t src[256 * 4];
    uint8_t out[256 * 4];
    int v;
    for (v = 0; v < 256; v++) {
        src[v * 4] = (uint8_t)v;
        src[v * 4 + 1] = (uint8_t)(255 - v);
        src[v * 4 + 2] = (uint8_t)(v / 3);
        src[v * 4 + 3] = 77;
    }
    if (t != NULL) {
        CHECK_EQ_INT(pc_transform_convert_u8(t, src, sizeof src, 4, out, sizeof out, 4, true, NULL), PC_OK);
        CHECK_EQ_MEM(src, out, sizeof src);
    }
    pc_transform_release(t);
}

static void test_cmyk_profile_is_monotonic_and_in_range(void)
{
    static const float paper[4] = {0, 0, 0, 0};
    static const double paper_lab[3] = {100, 0, 0};
    static const pc_intent intents[2] = {PC_INTENT_PERCEPTUAL, PC_INTENT_RELATIVE};
    const pc_profile *p = cmyk();
    double w[3];
    int ink;
    int k;
    pc_transform *t;
    uint8_t *src = pc_malloc_array(4096 * 3, 1);
    uint8_t *out = pc_malloc_array(4096 * 4, 1);
    uint32_t i;
    int bad = 0;
    CHECK_EQ_UINT(p->space, PC_CS_CMYK);
    for (ink = 0; ink < 4; ink++) {
        double prev = INFINITY;
        int s;
        for (s = 0; s <= 32; s++) {
            float v[4] = {0.1f, 0.1f, 0.1f, 0.1f};
            double l[3];
            v[ink] = (float)s / 32.0f;
            to_lab(p, v, PC_INTENT_RELATIVE, l);
            CHECK(l[0] <= prev + 0.05);
            prev = l[0];
        }
    }
    to_lab(p, paper, PC_INTENT_RELATIVE, w);
    CHECK(pc_delta_e76(w, paper_lab) < 0.5);
    /* sRGB gray ramp -> CMYK: in range, K never decreases, TAC within the limit. */
    for (k = 0; k < 2; k++) {
        pc_transform *back = mk(p, lab(), PC_INTENT_RELATIVE, false);
        float prev_k = -1.0f;
        double prev_l = INFINITY;
        int v;
        t = mk(srgb(), p, intents[k], true);
        for (v = 255; v >= 0; v -= 5) {
            float g = (float)v / 255.0f;
            float in[3];
            float o[4];
            float l[3];
            double lb[3];
            int c;
            in[0] = g;
            in[1] = g;
            in[2] = g;
            ev(t, in, o, 4);
            for (c = 0; c < 4; c++) {
                CHECK(o[c] >= -1e-6f && o[c] <= 1.0f + 1e-6f);
            }
            CHECK(o[0] + o[1] + o[2] + o[3] <= 3.0f + 0.02f);
            CHECK(o[3] >= prev_k - 0.01f);
            prev_k = o[3];
            ev(back, o, l, 3);
            dec(l, lb);
            CHECK(lb[0] <= prev_l + 0.3);
            CHECK(fabs(lb[1]) < 3.0 && fabs(lb[2]) < 3.0);
            prev_l = lb[0];
        }
        pc_transform_release(t);
        pc_transform_release(back);
    }
    /* Arbitrary colours on the u8 path: total ink within the limit. */
    t = mk(srgb(), p, PC_INTENT_RELATIVE, true);
    if (t != NULL && src != NULL && out != NULL) {
        for (i = 0; i < 4096; i++) {
            src[i * 3] = (uint8_t)(i * 37 % 256);
            src[i * 3 + 1] = (uint8_t)(i * 91 % 256);
            src[i * 3 + 2] = (uint8_t)(i * 13 % 256);
        }
        CHECK_EQ_INT(pc_transform_convert_u8(t, src, 4096 * 3, 3, out, 4096 * 4, 4, false, NULL), PC_OK);
        for (i = 0; i < 4096; i++) {
            const uint8_t *px = out + i * 4;
            bad += (uint32_t)px[0] + px[1] + px[2] + px[3] > 3u * 255u + 8u;
        }
        CHECK_EQ_INT(bad, 0);
    }
    pc_transform_release(t);
    free(src);
    free(out);
}

static void test_in_gamut_colours_survive_cmyk_roundtrip(void)
{
    static const double targets[5][3] = {{50, 0, 0}, {70, 10, 10}, {60, -20, -20}, {80, 0, 40}, {40, 30, -10}};
    pc_transform *to = mk(lab(), cmyk(), PC_INTENT_RELATIVE, false);
    pc_transform *back = mk(cmyk(), lab(), PC_INTENT_RELATIVE, false);
    int i;
    for (i = 0; i < 5; i++) {
        float in[3];
        float c[4];
        float l[3];
        double got[3];
        enc(targets[i], in);
        ev(to, in, c, 4);
        ev(back, c, l, 3);
        dec(l, got);
        CHECK(pc_delta_e76(got, targets[i]) < 2.0);
    }
    pc_transform_release(to);
    pc_transform_release(back);
}

static void test_intents_differ_as_expected(void)
{
    static const float blue[3] = {0.0f, 0.2f, 1.0f};
    static const float paper[4] = {0, 0, 0, 0};
    static const double paper_lab[3] = {95.0, 0.0, -2.0};
    pc_transform *per = mk(srgb(), cmyk(), PC_INTENT_PERCEPTUAL, false);
    pc_transform *rel = mk(srgb(), cmyk(), PC_INTENT_RELATIVE, false);
    pc_transform *sat = mk(srgb(), cmyk(), PC_INTENT_SATURATION, false);
    pc_transform *rel2 = mk(cmyk(), srgb(), PC_INTENT_RELATIVE, false);
    pc_transform *abs2 = mk(cmyk(), srgb(), PC_INTENT_ABSOLUTE, false);
    float a[4];
    float b[4];
    float s[4];
    float w1[3];
    float w2[3];
    double l[3];
    float d = 0.0f;
    int k;
    /* Out-of-gamut sRGB blue: perceptual compresses, relative clips. */
    ev(per, blue, a, 4);
    ev(rel, blue, b, 4);
    ev(sat, blue, s, 4);
    for (k = 0; k < 4; k++) {
        d += fabsf(a[k] - b[k]);
    }
    CHECK(d > 0.03f);
    CHECK_EQ_MEM(a, s, sizeof a); /* saturation shares the perceptual table */
    /* Paper white: relative is white, absolute shows the paper tint. */
    ev(rel2, paper, w1, 3);
    ev(abs2, paper, w2, 3);
    CHECK(w1[0] > 0.995f && w1[1] > 0.995f && w1[2] > 0.995f);
    CHECK(w2[0] < 0.97f && w2[2] > w2[0]);
    to_lab(cmyk(), paper, PC_INTENT_ABSOLUTE, l);
    CHECK(pc_delta_e76(l, paper_lab) < 0.6);
    pc_transform_release(per);
    pc_transform_release(rel);
    pc_transform_release(sat);
    pc_transform_release(rel2);
    pc_transform_release(abs2);
}

static uint32_t be32(const uint8_t *b)
{
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}
static void put_be32(uint8_t *b, uint32_t v)
{
    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
}

/* The built-in sRGB bytes (a v4 display profile) with the three wtpt values replaced. */
static bool srgb_with_wtpt(const int32_t xyz[3], pc_profile *out)
{
    pc_buf b = PC_BUF_INIT;
    uint32_t count;
    uint32_t i;
    bool ok = false;
    if (pc_profile_to_bytes(srgb(), &b, NULL) != PC_OK || b.len < 132) {
        goto done;
    }
    count = be32(b.data + 128);
    for (i = 0; i < count && 132 + 12 * (size_t)i + 12 <= b.len; i++) {
        const uint8_t *rec = b.data + 132 + 12 * i;
        if (memcmp(rec, "wtpt", 4) == 0) {
            uint32_t off = be32(rec + 4);
            int k;
            if ((size_t)off + 20 > b.len || memcmp(b.data + off, "XYZ ", 4) != 0) {
                goto done;
            }
            for (k = 0; k < 3; k++) {
                put_be32(b.data + off + 8 + 4 * k, (uint32_t)xyz[k]);
            }
            ok = pc_profile_parse(b.data, b.len, out, NULL) == PC_OK;
            break;
        }
    }
done:
    pc_buf_fini(&b);
    CHECK(ok);
    return ok;
}

/* A zero, negative or NaN media white is refused for absolute colorimetric (it used to turn
 * every colour into NaN); the other intents never divide by it and still work. */
static void test_absolute_colorimetric_refuses_a_degenerate_media_white(void)
{
    static const int32_t z[3] = {0, 0, 0};
    static const int32_t neg[3] = {-0x10000, 0x10000, 0x10000};
    static const int32_t d50w[3] = {0xF6D6, 0x10000, 0xD32D};
    static const float in[3] = {0.2f, 0.4f, 0.6f};
    static const char *names[4] = {"zero", "parsed zero", "negative", "nan"};
    pc_profile bad[4];
    pc_profile d50;
    double mw[3];
    float out[3];
    int i;
    int dir;
    pc_transform *t = NULL;
    pc_error e;
    CHECK_EQ_INT(pc_profile_clone(srgb(), &bad[0], NULL), PC_OK);
    bad[0].white_point[0] = bad[0].white_point[1] = bad[0].white_point[2] = 0.0;
    pc_profile_media_white(&bad[0], mw);
    CHECK(mw[0] == 0.0 && mw[1] == 0.0 && mw[2] == 0.0);
    if (!srgb_with_wtpt(z, &bad[1]) || !srgb_with_wtpt(neg, &bad[2])) {
        return;
    }
    pc_profile_media_white(&bad[1], mw);
    CHECK(mw[0] == 0.0 && mw[1] == 0.0 && mw[2] == 0.0); /* a v4 display wtpt is used verbatim */
    CHECK_EQ_INT(pc_profile_clone(srgb(), &bad[3], NULL), PC_OK);
    bad[3].white_point[0] = NAN;
    bad[3].white_point[1] = 1.0;
    bad[3].white_point[2] = 1.0;
    for (i = 0; i < 4; i++) {
        pc_t_name = names[i];
        for (dir = 0; dir < 2; dir++) {
            const pc_profile *s = dir == 0 ? srgb() : &bad[i];
            const pc_profile *d = dir == 0 ? &bad[i] : srgb();
            CHECK_EQ_INT(pc_transform_new(s, d, PC_INTENT_ABSOLUTE, false, &t, &e), PC_ERR_FORMAT); /* CmsError::Invalid */
            CHECK_CONTAINS(e.msg, "media white");
        }
        /* Proof Colors with Simulate Paper Color proofs through the absolute intent. */
        CHECK(pc_transform_proof(srgb(), &bad[i], srgb(), PC_INTENT_ABSOLUTE, false, true, &t, NULL) != PC_OK);
        CHECK(pc_transform_proof(srgb(), &bad[i], srgb(), PC_INTENT_RELATIVE, true, true, &t, NULL) != PC_OK);
        /* Without paper simulation the proof leg is relative and stays finite. */
        t = NULL;
        CHECK_EQ_INT(pc_transform_proof(srgb(), &bad[i], srgb(), PC_INTENT_RELATIVE, true, false, &t, NULL), PC_OK);
        ev(t, in, out, 3);
        CHECK(all_finite(out, 3));
        pc_transform_release(t);
        t = mk(srgb(), &bad[i], PC_INTENT_RELATIVE, false);
        ev(t, in, out, 3);
        CHECK(all_finite(out, 3));
        pc_transform_release(t);
        pc_profile_fini(&bad[i]);
    }
    /* A sane patched white still builds and scales. */
    if (srgb_with_wtpt(d50w, &d50)) {
        t = mk(srgb(), &d50, PC_INTENT_ABSOLUTE, false);
        ev(t, in, out, 3);
        CHECK(t != NULL && all_finite(out, 3));
        pc_transform_release(t);
        pc_profile_fini(&d50);
    }
}

static double l_of(const pc_transform *t, const pc_transform *back, float v)
{
    float in[3];
    float c[4];
    float l[3];
    double lb[3];
    in[0] = v;
    in[1] = v;
    in[2] = v;
    ev(t, in, c, 4);
    ev(back, c, l, 3);
    dec(l, lb);
    return lb[0];
}

static void test_black_point_compensation(void)
{
    static const float black[3] = {0, 0, 0};
    double bp = -1.0;
    double xyz[3];
    double l[3];
    pc_transform *with = mk(srgb(), cmyk(), PC_INTENT_RELATIVE, true);
    pc_transform *without = mk(srgb(), cmyk(), PC_INTENT_RELATIVE, false);
    pc_transform *back = mk(cmyk(), lab(), PC_INTENT_RELATIVE, false);
    pc_transform *rgb_with = mk(cmyk(), srgb(), PC_INTENT_RELATIVE, true);
    pc_transform *rgb_without = mk(cmyk(), srgb(), PC_INTENT_RELATIVE, false);
    double w0;
    double w1;
    double n0;
    double n1;
    float dark[4];
    float a[3];
    float b[3];
    CHECK_EQ_INT(pc_black_point(cmyk(), PC_INTENT_RELATIVE, false, &bp, NULL), PC_OK);
    xyz[0] = bp * 0.9642;
    xyz[1] = bp;
    xyz[2] = bp * 0.8249;
    pc_xyz_to_lab(xyz, PC_D50, l);
    CHECK(l[0] > 4.0 && l[0] < 20.0);
    CHECK_EQ_INT(pc_black_point(srgb(), PC_INTENT_RELATIVE, true, &bp, NULL), PC_OK);
    CHECK(bp == 0.0);
    /* sRGB -> CMYK: without BPC deep shadows clip together; with BPC they stay distinct. */
    w0 = l_of(with, back, 0.0f);
    w1 = l_of(with, back, 0.08f);
    n0 = l_of(without, back, 0.0f);
    n1 = l_of(without, back, 0.08f);
    CHECK(w1 - w0 > 1.5);
    CHECK(fabs(n1 - n0) < w1 - w0);
    /* CMYK -> sRGB: with BPC the darkest CMYK maps to (near) sRGB black. */
    ev(with, black, dark, 4);
    ev(rgb_with, dark, a, 3);
    ev(rgb_without, dark, b, 3);
    CHECK(a[1] < 0.03f && b[1] > a[1] + 0.03f);
    pc_transform_release(with);
    pc_transform_release(without);
    pc_transform_release(back);
    pc_transform_release(rgb_with);
    pc_transform_release(rgb_without);
}

static void eval_cb(void *ctx, const float *in, float *out)
{
    pc_transform_eval(ctx, in, out);
}

static void test_tetrahedral_beats_trilinear(void)
{
    static const size_t grid[3] = {9, 9, 9};
    pc_transform *exact = mk(srgb(), lab(), PC_INTENT_RELATIVE, false);
    pc_cms_clut clut;
    double tet_max = 0.0;
    double tet_sum = 0.0;
    double tri_sum = 0.0;
    double tet_neutral = 0.0;
    uint32_t seed = 12345u;
    int i;
    if (exact == NULL) {
        return;
    }
    CHECK_EQ_INT(pc_cms_clut_sample(grid, 3, 3, eval_cb, exact, &clut, NULL), PC_OK);
    for (i = 0; i < 4000; i++) {
        float p[3];
        float e[3];
        float a[3];
        float b[3];
        double de[3];
        double da[3];
        double db[3];
        double ea;
        int k;
        if (i < 200) {
            p[0] = p[1] = p[2] = (float)i / 199.0f;
        } else {
            for (k = 0; k < 3; k++) {
                seed = seed * 1664525u + 1013904223u;
                p[k] = (float)(seed >> 8) / (float)(1 << 24);
            }
        }
        pc_transform_eval(exact, p, e);
        pc_cms_clut_eval(&clut, p, a);
        pc_cms_clut_eval_multilinear(&clut, p, b);
        dec(e, de);
        dec(a, da);
        dec(b, db);
        if (i < 200) {
            tet_neutral = fmax(tet_neutral, fmax(fabs(da[1]), fabs(da[2])));
        }
        ea = pc_delta_e76(da, de);
        tet_max = fmax(tet_max, ea);
        tet_sum += ea;
        tri_sum += pc_delta_e76(db, de);
    }
    CHECK(tet_neutral < 0.01);
    CHECK(tet_sum <= tri_sum * 1.05);
    CHECK(tet_max < 4.0);
    pc_cms_clut_fini(&clut);
    pc_transform_release(exact);
}

static void test_proof_and_display_lut(void)
{
    static const float gray[3] = {0.5f, 0.5f, 0.5f};
    static const float blue[3] = {0, 0, 1};
    static const float white[3] = {1, 1, 1};
    pc_transform *p = NULL;
    pc_transform *t;
    pc_lut3d lut;
    pc_buf bytes = PC_BUF_INIT;
    float o[3];
    float worst = 0.0f;
    uint32_t i;
    CHECK_EQ_INT(pc_transform_proof(srgb(), cmyk(), srgb(), PC_INTENT_RELATIVE, true, false, &p, NULL), PC_OK);
    ev(p, gray, o, 3);
    CHECK(fabsf(o[0] - o[1]) < 0.02f && fabsf(o[1] - o[2]) < 0.03f);
    ev(p, blue, o, 3);
    CHECK(o[0] > 0.1f || o[1] > 0.1f);
    pc_transform_release(p);
    p = NULL;
    CHECK_EQ_INT(pc_transform_proof(srgb(), cmyk(), srgb(), PC_INTENT_RELATIVE, true, true, &p, NULL), PC_OK);
    ev(p, white, o, 3);
    CHECK(o[0] < 0.97f);
    pc_transform_release(p);

    t = mk(srgb(), pc_builtin_profile(PC_BUILTIN_DISPLAY_P3), PC_INTENT_RELATIVE, false);
    if (t == NULL || pc_lut3d_from_transform(t, 33, &lut, NULL) != PC_OK) {
        PC_T_FAIL("%s", "display lut");
        pc_transform_release(t);
        return;
    }
    CHECK_EQ_INT(pc_lut3d_to_rgba16f(&lut, &bytes, NULL), PC_OK);
    CHECK_EQ_UINT(bytes.len, 33 * 33 * 33 * 8);
    for (i = 0; i < 500; i++) {
        float c[3];
        float e[3];
        float s[3];
        int k;
        c[0] = (float)(i * 7 % 101) / 100.0f;
        c[1] = (float)(i * 13 % 101) / 100.0f;
        c[2] = (float)(i * 29 % 101) / 100.0f;
        pc_transform_eval(t, c, e);
        pc_lut3d_sample(&lut, c, s);
        for (k = 0; k < 3; k++) {
            worst = fmaxf(worst, fabsf(s[k] - e[k]));
        }
    }
    CHECK(worst < 2.0f / 255.0f);
    pc_buf_fini(&bytes);
    pc_lut3d_fini(&lut);
    pc_transform_release(t);
}

static void test_gray_profiles(void)
{
    static const float vs[5] = {0.0f, 0.2f, 0.5f, 0.9f, 1.0f};
    static const float one[3] = {1, 1, 1};
    static const float zero[3] = {0, 0, 0};
    pc_transform *t = mk(pc_builtin_profile(PC_BUILTIN_SGRAY), srgb(), PC_INTENT_RELATIVE, false);
    float o[3];
    int i;
    int k;
    for (i = 0; i < 5; i++) {
        ev(t, &vs[i], o, 3);
        for (k = 0; k < 3; k++) {
            CHECK_NEAR(o[k], vs[i], 1e-3);
        }
    }
    pc_transform_release(t);
    t = mk(srgb(), pc_builtin_profile(PC_BUILTIN_GRAY_GAMMA22), PC_INTENT_RELATIVE, false);
    ev(t, one, o, 1);
    CHECK_NEAR(o[0], 1.0, 1e-3);
    ev(t, zero, o, 1);
    CHECK_NEAR(o[0], 0.0, 1e-3);
    pc_transform_release(t);
}

static void test_hdr_floats_survive_matrix_transforms(void)
{
    static const float src[3] = {4.0f, 2.0f, 0.5f};
    const pc_profile *lin = pc_builtin_profile(PC_BUILTIN_LINEAR_SRGB);
    const pc_profile *r2020 = pc_builtin_profile(PC_BUILTIN_REC2020);
    pc_transform_options o = pc_transform_options_default();
    pc_transform *t = NULL;
    pc_transform *back = mk(r2020, lin, PC_INTENT_RELATIVE, false);
    float mid[3];
    float out[3];
    int k;
    o.intent = PC_INTENT_RELATIVE;
    o.bpc = false;
    o.precise_float = true;
    CHECK_EQ_INT(pc_transform_with_options(lin, r2020, o, &t, NULL), PC_OK);
    ev(t, src, mid, 3);
    ev(back, mid, out, 3);
    for (k = 0; k < 3; k++) {
        CHECK(fabsf(out[k] - src[k]) < 1e-3f * fmaxf(src[k], 1.0f));
    }
    pc_transform_release(t);
    pc_transform_release(back);
}

static void test_malformed_profiles_are_rejected_without_panicking(void)
{
    static const uint8_t zeros[200];
    pc_profile p;
    pc_buf good = PC_BUF_INIT;
    pc_buf c = PC_BUF_INIT;
    uint8_t *bad;
    size_t cuts[5];
    size_t i;
    CHECK(pc_profile_parse(NULL, 0, &p, NULL) != PC_OK);
    CHECK(pc_profile_parse(zeros, sizeof zeros, &p, NULL) != PC_OK);
    if (pc_profile_to_bytes(srgb(), &good, NULL) != PC_OK || pc_profile_to_bytes(cmyk(), &c, NULL) != PC_OK) {
        PC_T_FAIL("%s", "bytes");
        goto done;
    }
    cuts[0] = 100;
    cuts[1] = 131;
    cuts[2] = 140;
    cuts[3] = 200;
    cuts[4] = good.len - 20;
    for (i = 0; i < 5; i++) {
        if (pc_profile_parse(good.data, cuts[i], &p, NULL) == PC_OK) {
            pc_profile_fini(&p);
        }
    }
    /* Exact-size copies so ASan catches any read past the end. */
    bad = pc_malloc_array(good.len, 1);
    if (bad != NULL) {
        memcpy(bad, good.data, good.len);
        for (i = 128; i < good.len; i += 7) {
            bad[i] ^= 0x5A;
            if (pc_profile_parse(bad, good.len, &p, NULL) == PC_OK) {
                pc_profile_fini(&p);
            }
        }
        free(bad);
    }
    bad = pc_malloc_array(c.len, 1);
    if (bad != NULL) {
        memcpy(bad, c.data, c.len);
        for (i = 128; i < 400 && i < c.len; i += 3) {
            bad[i] = 0xFF;
            if (pc_profile_parse(bad, c.len, &p, NULL) == PC_OK) {
                pc_transform *t = NULL;
                if (pc_transform_new(&p, srgb(), PC_INTENT_PERCEPTUAL, true, &t, NULL) == PC_OK) {
                    pc_transform_release(t);
                }
                pc_profile_fini(&p);
            }
        }
        free(bad);
    }
done:
    pc_buf_fini(&good);
    pc_buf_fini(&c);
}

static bool icc_ext(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (strcmp(name + n - 4, ".icc") == 0 || strcmp(name + n - 4, ".icm") == 0 ||
                     strcmp(name + n - 4, ".ICC") == 0 || strcmp(name + n - 4, ".ICM") == 0);
}

static void check_system_profile(const char *path, const uint8_t *bytes, size_t len)
{
    static const float paper[4] = {0, 0, 0, 0};
    static const float k100[4] = {0, 0, 0, 1};
    static const float white[3] = {1, 1, 1};
    static const double target[3] = {60.0, 10.0, -10.0};
    pc_profile p;
    pc_error e;
    int intent;
    if (pc_profile_parse(bytes, len, &p, &e) != PC_OK) {
        /* Named-colour profiles are never used for conversions. */
        if (!(len >= 16 && memcmp(bytes + 12, "nmcl", 4) == 0)) {
            PC_T_FAIL("%s: %s", path, e.msg);
        }
        return;
    }
    pc_t_name = path;
    if (p.class_sig != PC_CLASS_INPUT && p.class_sig != PC_CLASS_DISPLAY && p.class_sig != PC_CLASS_OUTPUT &&
        p.class_sig != PC_CLASS_COLOR_SPACE) {
        pc_profile_fini(&p);
        return;
    }
    CHECK(p.description_len > 0);
    if (p.space == PC_CS_RGB || p.space == PC_CS_GRAY || p.space == PC_CS_CMYK || p.space == PC_CS_LAB) {
        for (intent = 0; intent < 4; intent++) {
            pc_transform *t = NULL;
            float half[16];
            float o[3];
            size_t k;
            CHECK_EQ_INT(pc_transform_new(&p, lab(), (pc_intent)intent, true, &t, &e), PC_OK);
            for (k = 0; k < 16; k++) {
                half[k] = 0.5f;
            }
            ev(t, half, o, 3);
            CHECK(all_finite(o, 3));
            pc_transform_release(t);
        }
        if (p.space == PC_CS_RGB) {
            double w[3];
            to_lab(&p, white, PC_INTENT_RELATIVE, w);
            CHECK(fabs(w[0] - 100.0) < 1.5);
        }
    }
    if (p.space == PC_CS_CMYK) {
        double w[3];
        double k[3];
        to_lab(&p, paper, PC_INTENT_RELATIVE, w);
        to_lab(&p, k100, PC_INTENT_RELATIVE, k);
        CHECK(w[0] > 90.0 && k[0] < 40.0);
        if (p.b2a[1] != NULL) {
            pc_transform *to = mk(lab(), &p, PC_INTENT_RELATIVE, false);
            pc_transform *back = mk(&p, lab(), PC_INTENT_RELATIVE, false);
            float in[3];
            float c[4];
            float l[3];
            double got[3];
            enc(target, in);
            ev(to, in, c, 4);
            ev(back, c, l, 3);
            dec(l, got);
            CHECK(pc_delta_e76(got, target) < 3.0);
            pc_transform_release(to);
            pc_transform_release(back);
        }
    }
    pc_profile_fini(&p);
}

/* Parses every profile the OS ships (read-only) and converts through each. */
static void test_system_profiles(void)
{
    static const char *dirs[4] = {"/System/Library/ColorSync/Profiles", "/Library/ColorSync/Profiles",
                                  "/usr/share/color/icc", "/usr/share/color/icc/colord"};
    const char *name = pc_t_name;
    int d;
    for (d = 0; d < 4; d++) {
        DIR *dir = opendir(dirs[d]);
        struct dirent *ent;
        if (dir == NULL) {
            continue;
        }
        while ((ent = readdir(dir)) != NULL) {
            char path[4096];
            pc_buf bytes = PC_BUF_INIT;
            if (!icc_ext(ent->d_name)) {
                continue;
            }
            (void)snprintf(path, sizeof path, "%s/%s", dirs[d], ent->d_name);
            if (pc_file_read(path, (size_t)64 << 20, &bytes, NULL) == PC_OK) {
                check_system_profile(path, bytes.data, bytes.len);
            }
            pc_buf_fini(&bytes);
        }
        closedir(dir);
    }
    pc_t_name = name;
}

/* `icc` with one more tag appended (the tag table grows by 12 bytes, so offsets shift). */
static bool with_tag(const pc_buf *icc, const char sig[4], const uint8_t *data, size_t n, pc_buf *out)
{
    uint32_t count;
    size_t table_end;
    size_t body;
    size_t pad;
    uint32_t i;
    uint8_t w[4];
    if (icc->len < 132) {
        return false;
    }
    count = be32(icc->data + 128);
    table_end = 132 + (size_t)count * 12;
    if (table_end > icc->len) {
        return false;
    }
    body = icc->len - table_end;
    pad = (4 - body % 4) % 4;
    if (pc_buf_append(out, icc->data, 128, NULL) != PC_OK) {
        return false;
    }
    put_be32(w, count + 1);
    (void)pc_buf_append(out, w, 4, NULL);
    for (i = 0; i < count; i++) {
        const uint8_t *rec = icc->data + 132 + 12 * i;
        (void)pc_buf_append(out, rec, 4, NULL);
        put_be32(w, be32(rec + 4) + 12);
        (void)pc_buf_append(out, w, 4, NULL);
        (void)pc_buf_append(out, rec + 8, 4, NULL);
    }
    (void)pc_buf_append(out, sig, 4, NULL);
    put_be32(w, (uint32_t)(table_end + 12 + body + pad));
    (void)pc_buf_append(out, w, 4, NULL);
    put_be32(w, (uint32_t)n);
    (void)pc_buf_append(out, w, 4, NULL);
    (void)pc_buf_append(out, icc->data + table_end, body, NULL);
    for (i = 0; i < pad; i++) {
        (void)pc_buf_append(out, "", 1, NULL);
    }
    if (pc_buf_append(out, data, n, NULL) != PC_OK) {
        return false;
    }
    put_be32(out->data, (uint32_t)out->len);
    return true;
}

/* A lutBtoA ("mBA ") tag, 1 -> 3 channels, using every element: B, matrix, M, CLUT, A. */
static size_t mba_1_to_3(uint8_t d[168])
{
    static const uint8_t curv[12] = {'c', 'u', 'r', 'v', 0, 0, 0, 0, 0, 0, 0, 0};
    static const uint32_t offs[5] = {32, 44, 92, 104, 132};
    static const uint8_t clut_tail[12] = {1, 0, 0, 0, 0, 0, 0, 255, 255, 255, 0, 0};
    size_t n = 0;
    int k;
    memcpy(d, "mBA \0\0\0\0", 8);
    n = 8;
    d[n++] = 1;
    d[n++] = 3;
    d[n++] = 0;
    d[n++] = 0;
    for (k = 0; k < 5; k++) {
        put_be32(d + n, offs[k]);
        n += 4;
    }
    memcpy(d + n, curv, 12); /* B @ 32 */
    n += 12;
    for (k = 0; k < 12; k++) {
        put_be32(d + n, (k % 4 == 0 && k < 9) ? 0x10000u : 0u); /* matrix @ 44 */
        n += 4;
    }
    memcpy(d + n, curv, 12); /* M @ 92 */
    n += 12;
    memset(d + n, 0, 16); /* CLUT @ 104: grid 2 */
    d[n] = 2;
    n += 16;
    memcpy(d + n, clut_tail, 12); /* 8-bit precision, 2 nodes x 3 outputs, pad to 132 */
    n += 12;
    for (k = 0; k < 3; k++) {
        memcpy(d + n, curv, 12); /* A @ 132 */
        n += 12;
    }
    return n;
}

/* Regression: a gray profile whose A2B0 tag is stored as "mBA " parsed, but re-encoding its
 * RGB view hit an "unsupported lutAtoB stage layout" panic in the ICC writer. */
static void test_gray_profile_with_reversed_lut_type_reencodes(void)
{
    pc_buf sgray = PC_BUF_INIT;
    pc_buf icc = PC_BUF_INIT;
    pc_buf rb = PC_BUF_INIT;
    pc_buf ob = PC_BUF_INIT;
    uint8_t tag[168];
    size_t n = mba_1_to_3(tag);
    pc_profile p;
    pc_profile rgb;
    pc_profile back;
    pc_profile odd;
    bool ok = false;
    CHECK_EQ_UINT(n, 168);
    if (pc_profile_to_bytes(pc_builtin_profile(PC_BUILTIN_SGRAY), &sgray, NULL) != PC_OK ||
        !with_tag(&sgray, "A2B0", tag, n, &icc) || pc_profile_parse(icc.data, icc.len, &p, NULL) != PC_OK) {
        PC_T_FAIL("%s", "crafted profile parses");
        goto done;
    }
    CHECK(p.a2b[0] != NULL);
    CHECK_EQ_INT(pc_profile_gray_as_rgb(&p, &rgb, &ok, NULL), PC_OK);
    CHECK(ok);
    if (ok) {
        CHECK(pc_profile_is_matrix_shaper(&rgb));
        CHECK_EQ_INT(pc_profile_to_bytes(&rgb, &rb, NULL), PC_OK);
        CHECK_EQ_INT(pc_profile_parse(rb.data, rb.len, &back, NULL), PC_OK);
        CHECK_EQ_UINT(back.space, PC_CS_RGB);
        pc_profile_fini(&back);
        pc_profile_fini(&rgb);
    }
    /* The writer itself must not fail on a LUT it can't store either. */
    CHECK_EQ_INT(pc_profile_clone(&p, &odd, NULL), PC_OK);
    free(odd.description);
    odd.description = pc_strndup("re-encoded", 10);
    odd.description_len = 10;
    CHECK_EQ_INT(pc_profile_encode_bytes(&odd, NULL), PC_OK);
    CHECK_EQ_INT(pc_profile_to_bytes(&odd, &ob, NULL), PC_OK);
    CHECK_EQ_INT(pc_profile_parse(ob.data, ob.len, &back, NULL), PC_OK);
    pc_profile_fini(&back);
    pc_profile_fini(&odd);
    pc_profile_fini(&p);
done:
    pc_buf_fini(&sgray);
    pc_buf_fini(&icc);
    pc_buf_fini(&rb);
    pc_buf_fini(&ob);
}

/* sRGB the way Photoshop embeds it: v2, TRCs as 1024-entry 16-bit tables. */
static bool srgb_as_v2_tables(pc_profile *out)
{
    pc_profile p;
    int c;
    if (pc_profile_clone(srgb(), &p, NULL) != PC_OK) {
        return false;
    }
    for (c = 0; c < 3; c++) {
        float *t = pc_malloc_array(1024, sizeof *t);
        int i;
        if (t == NULL) {
            pc_profile_fini(&p);
            return false;
        }
        for (i = 0; i < 1024; i++) {
            t[i] = (float)(round(pc_cms_curve_eval64(&p.trc[c], (double)i / 1023.0) * 65535.0) / 65535.0);
        }
        pc_cms_curve_fini(&p.trc[c]);
        p.trc[c] = pc_cms_curve_table(t, 1024);
    }
    p.version_major = 2;
    p.version_minor = 0x10;
    if (pc_profile_encode_bytes(&p, NULL) != PC_OK) {
        pc_profile_fini(&p);
        return false;
    }
    *out = p;
    return true;
}

static bool reparse(const pc_profile *p, pc_profile *out)
{
    pc_buf b = PC_BUF_INIT;
    bool ok = pc_profile_to_bytes(p, &b, NULL) == PC_OK && pc_profile_parse(b.data, b.len, out, NULL) == PC_OK;
    pc_buf_fini(&b);
    return ok;
}

static void test_same_colors_ignores_the_encoding(void)
{
    pc_profile v2raw;
    pc_profile v2;
    uint64_t h1 = 0;
    uint64_t h2 = 0;
    pc_builtin b;
    if (!srgb_as_v2_tables(&v2raw)) {
        PC_T_FAIL("%s", "v2 sRGB");
        return;
    }
    CHECK(reparse(&v2raw, &v2));
    pc_profile_fini(&v2raw);
    CHECK_EQ_INT(pc_profile_content_hash(&v2, &h1, NULL), PC_OK);
    CHECK_EQ_INT(pc_profile_content_hash(srgb(), &h2, NULL), PC_OK);
    CHECK(h1 != h2);
    CHECK(pc_profile_same_colors(&v2, srgb()) && pc_profile_same_colors(srgb(), &v2));
    pc_profile_fini(&v2);
    for (b = PC_BUILTIN_SRGB; b < PC_BUILTIN_COUNT; b = (pc_builtin)(b + 1)) {
        const pc_profile *p = pc_builtin_profile(b);
        pc_profile c;
        pc_profile again;
        CHECK(pc_profile_same_colors(p, p));
        /* Re-encoded (fresh bytes, same model): CMYK too, where a round trip is not an identity. */
        if (pc_profile_clone(p, &c, NULL) != PC_OK) {
            PC_T_FAIL("%s", "clone");
            continue;
        }
        CHECK_EQ_INT(pc_profile_encode_bytes(&c, NULL), PC_OK);
        if (reparse(&c, &again)) {
            CHECK(pc_profile_same_colors(&again, p));
            pc_profile_fini(&again);
        } else {
            PC_T_FAIL("%s", "reparse");
        }
        pc_profile_fini(&c);
    }
}

static void test_same_colors_tells_different_spaces_apart(void)
{
    static const pc_builtin others[6] = {PC_BUILTIN_DISPLAY_P3,  PC_BUILTIN_LINEAR_SRGB, PC_BUILTIN_ADOBE_RGB_COMPAT,
                                         PC_BUILTIN_REC2020,     PC_BUILTIN_COATED_CMYK, PC_BUILTIN_GRAY_GAMMA22};
    pc_profile moved;
    int i;
    for (i = 0; i < 6; i++) {
        CHECK(!pc_profile_same_colors(srgb(), pc_builtin_profile(others[i])));
    }
    CHECK(!pc_profile_same_colors(pc_builtin_profile(PC_BUILTIN_GRAY_GAMMA22), pc_builtin_profile(PC_BUILTIN_SGRAY)));
    /* Scaling the red primary's XYZ by 2% makes a different space too. */
    if (pc_profile_clone(srgb(), &moved, NULL) == PC_OK) {
        for (i = 0; i < 3; i++) {
            moved.matrix.m[i][0] *= 1.02;
        }
        CHECK_EQ_INT(pc_profile_encode_bytes(&moved, NULL), PC_OK);
        CHECK(!pc_profile_same_colors(&moved, srgb()));
        pc_profile_fini(&moved);
    }
}

int main(int argc, char **argv)
{
    RUN(test_builtins_encode_and_reparse);
    RUN(test_srgb_known_lab_values);
    RUN(test_rgb_lab_rgb_roundtrip_8bit_and_float);
    RUN(test_rgb_to_rgb_matrix_shaper_paths_agree);
    RUN(test_identity_transform_is_identity);
    RUN(test_cmyk_profile_is_monotonic_and_in_range);
    RUN(test_in_gamut_colours_survive_cmyk_roundtrip);
    RUN(test_intents_differ_as_expected);
    RUN(test_absolute_colorimetric_refuses_a_degenerate_media_white);
    RUN(test_black_point_compensation);
    RUN(test_tetrahedral_beats_trilinear);
    RUN(test_proof_and_display_lut);
    RUN(test_gray_profiles);
    RUN(test_hdr_floats_survive_matrix_transforms);
    RUN(test_malformed_profiles_are_rejected_without_panicking);
    RUN(test_system_profiles);
    RUN(test_gray_profile_with_reversed_lut_type_reencodes);
    RUN(test_same_colors_ignores_the_encoding);
    RUN(test_same_colors_tells_different_spaces_apart);
    return pc_t_finish();
}
