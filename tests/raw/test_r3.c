/* Unit and round-trip tests of the modules added in the R3 resync (crates/raw/tests/raf.rs and the
 * unit tests of raf.rs, demosaic.rs, cameras.rs, look.rs, tone.rs and tiff.rs as of the resync):
 * Fujifilm RAF, the generic CFA demosaic, measured image areas, camera looks, the tone sliders,
 * retone and the select port. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "harness.h"
#include "raw/internal.h"
#include "raw/raw.h"
#include "testgen.h"
#include "testgen_raf.h"

static const uint8_t RGGB[4] = {0, 1, 1, 2};

/* ---- RAF (tests/raf.rs) ---- */

static tg_raf xtrans(tg_raf_packing packing, uint32_t bits, uint16_t **data)
{
    float (*sc)[3] = tg_scene(30, 18);
    uint16_t white = (uint16_t)((1u << bits) - 1);
    tg_raf r;
    size_t i;
    *data = tg_mosaic_cfa((const float (*)[3])sc, 30 * 18, 30, TG_XTRANS, 36, 6, 256, white);
    free(sc);
    r = tg_raf_new(30, 18, *data, TG_XTRANS, 36);
    r.bits = bits;
    r.packing = packing;
    for (i = 0; i < 36; i++) {
        r.black[i] = 256;
    }
    r.nblack = 36;
    r.has_crop = true;
    r.crop[0] = 2;
    r.crop[1] = 2;
    r.crop[2] = 14;
    r.crop[3] = 26;
    return r;
}

static void test_raf_every_storage_decodes_exactly(void)
{
    static const tg_raf_packing P[4] = {TG_RAF_U16_LE, TG_RAF_U16_BE, TG_RAF_LSB12, TG_RAF_WORDS14};
    static const uint32_t B[4] = {14, 16, 12, 14};
    size_t i;
    for (i = 0; i < 4; i++) {
        uint16_t *d;
        tg_raf r = xtrans(P[i], B[i], &d);
        tg_bytes b = tg_raf_build(&r);
        pc_raw_format f;
        pc_raw_limits lim = pc_raw_limits_default();
        pc_raw_sensor s;
        pc_error e;
        CHECK(pc_raw_identify(b.d, b.n, &f) && f == PC_RAW_RAF);
        if (pc_raw_decode(b.d, b.n, &lim, &s, &e) == PC_OK) {
            CHECK(s.width == 30 && s.height == 18);
            CHECK_EQ_MEM(s.data, d, 30 * 18 * sizeof *d);
            pc_raw_sensor_fini(&s);
        } else {
            PC_T_FAIL("packing %zu: %s", i, e.msg);
        }
        tg_free(&b);
        free(d);
    }
}

static void test_raf_xtrans_metadata_and_development(void)
{
    uint16_t *d;
    tg_raf r = xtrans(TG_RAF_U16_LE, 14, &d);
    tg_bytes b;
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_error e;
    size_t i;
    r.has_bias = true;
    r.bias[0] = -72;
    r.bias[1] = 100;
    r.orientation = 6;
    b = tg_raf_build(&r);
    if (pc_raw_decode(b.d, b.n, &lim, &s, &e) != PC_OK) {
        PC_T_FAIL("%s", e.msg);
    } else {
        int m;
        CHECK(s.has_cfa && s.cfa.width == 6 && s.cfa.height == 6);
        for (i = 0; i < 36; i++) {
            CHECK_EQ_UINT(pc_raw_cfa_color(&s.cfa, i % 6, i / 6), TG_XTRANS[i]);
        }
        CHECK(!pc_raw_cfa_is_bayer(&s.cfa));
        CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(2, 2, 26, 14)));
        CHECK(s.black.nvalues == 1 && s.black.values[0] == 256.0f);
        CHECK(s.has_camera_wb && s.camera_wb[0] == 2.0 && s.camera_wb[1] == 1.0 && s.camera_wb[2] == 1.5);
        CHECK_NEAR(s.baseline_exposure, 0.72, 1e-9);
        CHECK_EQ_UINT(s.orientation, 6);
        CHECK_EQ_STR(s.make, "FUJIFILM");
        CHECK_EQ_STR(s.model, "X-Synthetic");
        CHECK_EQ_UINT(s.warnings.n, 0);
        s.baseline_exposure = 0.0;
        for (m = 0; m < 3; m++) {
            pc_raw_develop_options o = pc_raw_develop_options_default();
            pc_raw_developed dv;
            o.demosaic = (pc_raw_demosaic)m;
            if (pc_raw_develop_sensor(&s, &o, &dv, &e) == PC_OK) {
                CHECK(dv.width == 14 && dv.height == 26 && dv.info.format == PC_RAW_RAF);
                CHECK_EQ_STR(dv.info.cfa, "GRBGBR/BGGRGG/RGGBGG/GBRGRB/RGGBGG/BGGRGG");
                CHECK(dv.info.wb_multipliers[0] == 2.0 && dv.info.wb_multipliers[2] == 1.5);
                pc_raw_developed_fini(&dv);
            } else {
                PC_T_FAIL("%s", e.msg);
            }
        }
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(d);
}

static void test_raf_xtrans_white_balance_is_per_site(void)
{
    float (*rgb)[3] = tg_xalloc(36 * 24 * sizeof *rgb);
    uint16_t *d;
    tg_raf r;
    tg_bytes b;
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_raw_developed dv;
    pc_error e;
    size_t i;
    for (i = 0; i < 36 * 24; i++) {
        rgb[i][0] = 0.4f / 2.0f;
        rgb[i][1] = 0.4f;
        rgb[i][2] = 0.4f / 1.5f;
    }
    d = tg_mosaic_cfa((const float (*)[3])rgb, 36 * 24, 36, TG_XTRANS, 36, 6, 256, 16383);
    r = tg_raf_new(36, 24, d, TG_XTRANS, 36);
    r.black[0] = 256;
    b = tg_raf_build(&r);
    if (pc_raw_develop(b.d, b.n, &o, &dv, &e) == PC_OK) {
        bool neutral = true;
        for (i = 0; i < (size_t)dv.width * dv.height; i++) {
            int g = dv.rgb[i * 3 + 1];
            neutral = neutral && abs(dv.rgb[i * 3] - g) <= 2 && abs(dv.rgb[i * 3 + 2] - g) <= 2;
        }
        CHECK(neutral);
        pc_raw_developed_fini(&dv);
    } else {
        PC_T_FAIL("%s", e.msg);
    }
    tg_free(&b);
    free(d);
    free(rgb);
}

static void test_raf_bayer_layouts(void)
{
    static const uint8_t CFAS[2][4] = {{0, 1, 1, 2}, {2, 1, 1, 0}};
    static const tg_raf_packing P[3] = {TG_RAF_LSB12, TG_RAF_WORDS14, TG_RAF_U16_LE};
    static const uint32_t B[3] = {12, 14, 14};
    float (*sc)[3] = tg_scene(24, 16);
    size_t c;
    size_t k;
    for (c = 0; c < 2; c++) {
        for (k = 0; k < 3; k++) {
            uint16_t *d = tg_mosaic_cfa((const float (*)[3])sc, 24 * 16, 24, CFAS[c], 4, 2, 64,
                                        (uint16_t)((1u << B[k]) - 1));
            tg_raf r = tg_raf_new(24, 16, d, CFAS[c], 4);
            tg_bytes b;
            pc_raw_limits lim = pc_raw_limits_default();
            pc_raw_sensor s;
            pc_error e;
            size_t i;
            r.bits = B[k];
            r.packing = P[k];
            for (i = 0; i < 4; i++) {
                r.black[i] = 64;
            }
            r.nblack = 4;
            b = tg_raf_build(&r);
            if (pc_raw_decode(b.d, b.n, &lim, &s, &e) == PC_OK) {
                uint8_t ph[4];
                pc_raw_develop_options o = pc_raw_develop_options_default();
                pc_raw_developed dv;
                CHECK_EQ_MEM(s.data, d, 24 * 16 * sizeof *d);
                pc_raw_cfa_phase(&s.cfa, 0, 0, ph);
                CHECK_EQ_MEM(ph, CFAS[c], 4);
                if (pc_raw_develop_sensor(&s, &o, &dv, &e) == PC_OK) {
                    CHECK_EQ_STR(dv.info.cfa, c == 0 ? "RGGB" : "BGGR");
                    pc_raw_developed_fini(&dv);
                }
                pc_raw_sensor_fini(&s);
            } else {
                PC_T_FAIL("%s", e.msg);
            }
            tg_free(&b);
            free(d);
        }
    }
    free(sc);
}

static void test_raf_unsupported_variants_and_bad_files(void)
{
    uint16_t *d;
    tg_raf r = xtrans(TG_RAF_COMPRESSED, 14, &d);
    tg_bytes b = tg_raf_build(&r);
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_raw_preview p;
    pc_error e;
    uint16_t v[32];
    static const uint8_t BAD[4] = {0, 0, 2, 2};
    size_t i;
    CHECK_EQ_INT(pc_raw_decode(b.d, b.n, &lim, &s, &e), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "compressed");
    CHECK(pc_raw_embedded_preview(b.d, b.n, &p) && p.width == 32 && p.height == 24);
    tg_free(&b);
    r.packing = TG_RAF_U16_LE;
    r.cfa_tiff = false;
    b = tg_raf_build(&r);
    CHECK_EQ_INT(pc_raw_decode(b.d, b.n, &lim, &s, &e), PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "FinePix");
    tg_free(&b);
    free(d);
    for (i = 0; i < 32; i++) {
        v[i] = 100;
    }
    r = tg_raf_new(8, 4, v, BAD, 4);
    b = tg_raf_build(&r);
    CHECK_EQ_INT(pc_raw_decode(b.d, b.n, &lim, &s, &e), PC_ERR_UNSUPPORTED);
    tg_free(&b);
    r = xtrans(TG_RAF_U16_LE, 14, &d);
    b = tg_raf_build(&r);
    lim.max_pixels = 100;
    CHECK_EQ_INT(pc_raw_decode(b.d, b.n, &lim, &s, &e), PC_ERR_LIMIT);
    lim = pc_raw_limits_default();
    CHECK_EQ_INT(pc_raw_decode(b.d, b.n - 7, &lim, &s, &e), PC_ERR_FORMAT);
    tg_free(&b);
    free(d);
}

/* raf.rs unit tests: the packings. */
static void test_raf_packings(void)
{
    static const uint8_t LSB[3] = {0xBC, 0x3A, 0x12};
    static const uint16_t VALS[4] = {0x3FFF, 0x0001, 0x2AAA, 0x1555};
    uint64_t acc = 0;
    uint8_t logical[8];
    uint8_t phys[8];
    uint16_t *out;
    pc_error e;
    size_t i;
    if (rx_raf_unpack(LSB, 3, 2, 1, 12, RX_RAF_LSB_FIRST, &out, &e) == PC_OK) {
        CHECK(out[0] == 0xABC && out[1] == 0x123);
        free(out);
    }
    for (i = 0; i < 4; i++) {
        acc = (acc << 14) | VALS[i];
    }
    for (i = 0; i < 7; i++) {
        logical[i] = (uint8_t)(acc >> (8 * (6 - i)));
    }
    logical[7] = 0;
    for (i = 0; i < 8; i++) {
        phys[i] = logical[(i & ~(size_t)3) + 3 - (i & 3)];
    }
    if (rx_raf_unpack(phys, 8, 4, 1, 14, RX_RAF_WORDS32_MSB_FIRST, &out, &e) == PC_OK) {
        CHECK_EQ_MEM(out, VALS, sizeof VALS);
        free(out);
    }
    /* A short buffer reads as zeros. */
    if (rx_raf_unpack(phys, 3, 4, 1, 14, RX_RAF_WORDS32_MSB_FIRST, &out, &e) == PC_OK) {
        free(out);
    }
}

/* ---- the generic CFA demosaic (demosaic.rs) ---- */

static void padded_from(const float *plane, size_t w, size_t h, rx_padded *p)
{
    size_t y;
    pc_error e;
    if (rx_padded_new(w, h, p, &e) != PC_OK) {
        abort();
    }
    for (y = 0; y < h; y++) {
        memcpy(p->data + (y + RX_PAD) * p->stride + RX_PAD, plane + y * w, w * sizeof *plane);
    }
    rx_padded_fill_borders(p);
}

static void pattern_rows(const char *const *rows, size_t n, rx_pattern *p)
{
    size_t w = strlen(rows[0]);
    size_t i;
    memset(p, 0, sizeof *p);
    p->w = w;
    p->h = n;
    p->n = w * n;
    for (i = 0; i < w * n; i++) {
        char c = rows[i / w][i % w];
        p->colors[i] = c == 'R' ? 0 : c == 'G' ? 1 : 2;
    }
}

static const char *const XT_ROWS[6] = {"GGRGGB", "GGBGGR", "BRGRBG", "GGBGGR", "GGRGGB", "RBGBRG"};

static float *mosaic_pat(const float (*rgb)[3], size_t n, size_t w, const rx_pattern *p)
{
    float *v = tg_xalloc(n * sizeof *v);
    size_t i;
    for (i = 0; i < n; i++) {
        v[i] = rgb[i][rx_pattern_at(p, (ptrdiff_t)(i % w), (ptrdiff_t)(i / w))];
    }
    return v;
}

static double psnr(const float *out, const float (*rgb)[3], size_t w, size_t h, size_t border)
{
    double se = 0.0;
    size_t n = 0;
    size_t x;
    size_t y;
    int c;
    for (y = border; y + border < h; y++) {
        for (x = border; x + border < w; x++) {
            for (c = 0; c < 3; c++) {
                double d = (double)out[(y * w + x) * 3 + (size_t)c] - (double)rgb[y * w + x][c];
                se += d * d;
                n++;
            }
        }
    }
    return 10.0 * log10(1.0 / (se / (double)n + 1e-12));
}

static void test_xtrans_flat_colour_is_exact_and_quality(void)
{
    rx_pattern pat;
    size_t w = 25;
    size_t h = 19;
    float (*rgb)[3] = tg_xalloc(66 * 48 * sizeof *rgb);
    static const size_t SH[4][2] = {{0, 0}, {1, 0}, {2, 3}, {5, 5}};
    size_t i;
    size_t s;
    pc_error e;
    pattern_rows(XT_ROWS, 6, &pat);
    for (i = 0; i < w * h; i++) {
        rgb[i][0] = 0.2f;
        rgb[i][1] = 0.5f;
        rgb[i][2] = 0.8f;
    }
    for (s = 0; s < 4; s++) {
        rx_pattern sp = pat;
        float *plane;
        rx_padded p;
        float *out;
        bool exact = true;
        for (i = 0; i < 36; i++) {
            sp.colors[i] = rx_pattern_at(&pat, (ptrdiff_t)(i % 6 + SH[s][0]), (ptrdiff_t)(i / 6 + SH[s][1]));
        }
        plane = mosaic_pat((const float (*)[3])rgb, w * h, w, &sp);
        padded_from(plane, w, h, &p);
        if (rx_demosaic_cfa(&p, &sp, &out, &e) == PC_OK) {
            for (i = 0; i < w * h * 3; i++) {
                exact = exact && fabsf(out[i] - rgb[i / 3][i % 3]) < 1e-5f;
            }
            free(out);
        }
        CHECK(exact);
        rx_padded_fini(&p);
        free(plane);
    }
    /* A smooth colour field reconstructs above 40 dB. */
    w = 66;
    h = 48;
    for (i = 0; i < w * h; i++) {
        float x = (float)(i % w) / (float)w;
        float y = (float)(i / w) / (float)h;
        rgb[i][0] = 0.2f + 0.6f * x;
        rgb[i][1] = 0.3f + 0.4f * (x * y);
        rgb[i][2] = 0.8f - 0.5f * y;
    }
    {
        float *plane = mosaic_pat((const float (*)[3])rgb, w * h, w, &pat);
        rx_padded p;
        float *out;
        padded_from(plane, w, h, &p);
        if (rx_demosaic_cfa(&p, &pat, &out, &e) == PC_OK) {
            double q = psnr(out, (const float (*)[3])rgb, w, h, 2);
            CHECK(q > 40.0);
            free(out);
        }
        rx_padded_fini(&p);
        free(plane);
    }
    free(rgb);
}

static void test_odd_patterns_do_not_crash(void)
{
    static const char *const P1[3] = {"RGG", "GGR", "GRG"};
    static const char *const P2[1] = {"G"};
    static const char *const P3[2] = {"RG", "BR"};
    static const size_t SZ[4][2] = {{1, 1}, {2, 3}, {7, 5}, {13, 1}};
    const char *const *ps[3] = {P1, P2, P3};
    static const size_t PN[3] = {3, 1, 2};
    size_t a;
    size_t k;
    pc_error e;
    for (a = 0; a < 3; a++) {
        rx_pattern pat;
        pattern_rows(ps[a], PN[a], &pat);
        for (k = 0; k < 4; k++) {
            size_t w = SZ[k][0];
            size_t h = SZ[k][1];
            float plane[91];
            rx_padded p;
            float *out;
            size_t i;
            for (i = 0; i < w * h; i++) {
                plane[i] = 0.5f;
            }
            padded_from(plane, w, h, &p);
            if (rx_demosaic_cfa(&p, &pat, &out, &e) == PC_OK) {
                bool finite = true;
                for (i = 0; i < w * h * 3; i++) {
                    finite = finite && isfinite(out[i]);
                }
                CHECK(finite);
                free(out);
            }
            rx_padded_fini(&p);
        }
    }
    /* A degenerate pattern gives zeros, not an out-of-range read. */
    {
        rx_pattern bad;
        float plane[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        rx_padded p;
        float *out;
        memset(&bad, 0, sizeof bad);
        bad.w = 6;
        bad.h = 6;
        bad.n = 4;
        padded_from(plane, 2, 2, &p);
        if (rx_demosaic_cfa(&p, &bad, &out, &e) == PC_OK) {
            size_t i;
            bool zero = true;
            for (i = 0; i < 12; i++) {
                zero = zero && out[i] == 0.0f;
            }
            CHECK(zero);
            free(out);
        }
        rx_padded_fini(&p);
    }
}

/* ---- cameras.rs ---- */

static pc_raw_sensor fake_sensor(const char *make, const char *model, size_t w, size_t h)
{
    pc_raw_sensor s;
    pc_error e;
    pc_raw_sensor_init(&s);
    s.format = PC_RAW_NEF;
    s.make = pc_strndup(make, strlen(make));
    s.model = pc_strndup(model, strlen(model));
    s.width = w;
    s.height = h;
    s.samples = 1;
    s.has_cfa = true;
    (void)pc_raw_cfa_bayer("RGGB", &s.cfa);
    (void)rx_black_uniform(&s.black, 0.0f, &e);
    s.white[0] = s.white[1] = s.white[2] = 16383.0f;
    s.active = pc_raw_rect_make(0, 0, w, h);
    s.crop = s.active;
    return s;
}

static void test_measured_image_areas(void)
{
    pc_raw_sensor s = fake_sensor("NIKON CORPORATION", "NIKON D5100", 4992, 3280);
    pc_error e;
    s.active = pc_raw_rect_make(0, 0, 4984, 3280);
    s.crop = s.active;
    CHECK(rx_cameras_apply(&s, &e) == PC_OK);
    CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(8, 8, 4928, 3264)));
    CHECK(s.color.ncalibrations == 0 && s.ntone_curve == 0 && s.white[0] == 16383.0f);
    s.crop = pc_raw_rect_make(16, 12, 4800, 3200);
    (void)rx_cameras_apply(&s, &e);
    CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(16, 12, 4800, 3200)));
    pc_raw_sensor_fini(&s);
    s = fake_sensor("NIKON CORPORATION", "NIKON D800E", 7424, 4924);
    (void)rx_cameras_apply(&s, &e);
    CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(8, 6, 7360, 4912)));
    pc_raw_sensor_fini(&s);
    s = fake_sensor("SONY", "ILCE-7", 6048, 4024);
    (void)rx_cameras_apply(&s, &e);
    CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(12, 12, 6000, 4000)));
    pc_raw_sensor_fini(&s);
    s = fake_sensor("NIKON CORPORATION", "NIKON D800", 7424, 4924);
    (void)rx_cameras_apply(&s, &e);
    CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(0, 0, 7424, 4924)));
    pc_raw_sensor_fini(&s);
    s = fake_sensor("NIKON CORPORATION", "NIKON D4", 4992, 3292);
    (void)rx_cameras_apply(&s, &e);
    CHECK(s.black_point == 0.33f && s.ntone_curve == 53);
    pc_raw_sensor_fini(&s);
}

/* ---- look.rs ---- */

static void test_tone_curve_validates_and_interpolates(void)
{
    static const double A[1][2] = {{0.0, 0.0}};
    static const double B[4][2] = {{0, 0}, {0.5, 0.4}, {0.5, 0.6}, {1, 1}};
    static const double C[4][2] = {{0, 0}, {0.5, 0.6}, {0.7, 0.4}, {1, 1}};
    static const double D[2][2] = {{0.1, 0.0}, {1, 1}};
    static const double OK[3][2] = {{0, 0}, {0.04, 0.16}, {1, 1}};
    double N[3][2] = {{0, 0}, {0.5, 0}, {1, 1}};
    pc_raw_look_curve *c = malloc(sizeof *c);
    double k[3][2];
    if (c == NULL) {
        return;
    }
    N[1][1] = NAN;
    CHECK(!pc_raw_look_curve_new(A, 1, c));
    CHECK(!pc_raw_look_curve_new(B, 4, c));
    CHECK(!pc_raw_look_curve_new(C, 4, c));
    CHECK(!pc_raw_look_curve_new((const double (*)[2])N, 3, c));
    CHECK(!pc_raw_look_curve_new(D, 2, c));
    CHECK(pc_raw_look_curve_new(OK, 3, c));
    CHECK_NEAR(pc_raw_look_curve_eval(c, 0.04), 0.16, 1e-12);
    CHECK_NEAR(pc_raw_look_curve_eval(c, 0.01), 0.04, 1e-12);
    CHECK(pc_raw_look_curve_eval(c, -1.0) == 0.0);
    CHECK(pc_raw_look_curve_eval(c, 2.0) == 1.0);
    CHECK(pc_raw_look_curve_eval(c, NAN) == 0.0);
    pc_raw_look_curve_knots(c, k);
    CHECK_NEAR(k[1][0], 0.04, 1e-12);
    CHECK_NEAR(k[1][1], 0.16, 1e-12);
    free(c);
}

/* The synthetic camera of look.rs's tests: patches with tonal ramps, a known matrix and curve. */
static void test_look_recovers_a_known_matrix_and_curve(void)
{
    static const double WB[3] = {1.5, 1.0, 1.25};
    static const double K[3][3] = {{1.35, -0.25, -0.1}, {-0.15, 1.25, -0.1}, {0.0, -0.3, 1.3}};
    size_t w = 480;
    size_t h = 320;
    float (*cam)[3] = tg_xalloc(w * h * sizeof *cam);
    float colours[96][3];
    uint32_t seed = 12345;
    uint16_t *data;
    tg_bytes raw;
    uint8_t *rgb = tg_xalloc(w * h * 3);
    pc_raw_mat3 a = rx_xyz_d50_to_prophoto();
    pc_raw_mat3 b = rx_srgb_to_xyz_d50();
    pc_raw_mat3 ab = rx_mul(&a, &b);
    pc_raw_mat3 k3;
    pc_raw_mat3 m;
    pc_raw_mat3 to_srgb;
    pc_raw_mat3 romm = rx_rgb_to_xyz(RX_ROMM, RX_D50_XY);
    pc_raw_mat3 p2s;
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_raw_camera_look *look = malloc(sizeof *look);
    pc_raw_reference r;
    pc_error e;
    size_t i;
    int c;
    memcpy(k3.m, K, sizeof K);
    m = rx_mul(&ab, &k3);
    (void)rx_invert(&b, &to_srgb);
    p2s = rx_mul(&to_srgb, &romm);
    for (i = 0; i < 96; i++) {
        for (c = 0; c < 3; c++) {
            seed = seed * 1664525u + 1013904223u;
            colours[i][c] = 0.15f + 0.45f * ((float)(seed >> 8) / (float)(1u << 24));
        }
    }
    for (i = 0; i < w * h; i++) {
        size_t x = i % w;
        size_t y = i / w;
        float rmp = 0.15f + 0.85f * (float)((x * 12) % w) / (float)w;
        double bal[3];
        double pro[3];
        double sr[3];
        for (c = 0; c < 3; c++) {
            cam[i][c] = colours[(y * 8 / h) * 12 + x * 12 / w][c] * rmp / (float)WB[c];
            bal[c] = (double)cam[i][c] * WB[c];
        }
        rx_apply(&m, bal, pro);
        for (c = 0; c < 3; c++) {
            double v = pc_clampd(pro[c], 0.0, 1.0);
            pro[c] = 1.6 * v / (1.0 + 0.6 * v);
        }
        rx_apply(&p2s, pro, sr);
        for (c = 0; c < 3; c++) {
            double v = pc_clampd(sr[c], 0.0, 1.0);
            double en = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
            rgb[i * 3 + (size_t)c] = pc_f64_to_u8(round(en * 255.0));
        }
    }
    data = tg_mosaic((const float (*)[3])cam, w * h, w, RGGB, 0, 4095);
    raw = tg_tiff_ep("NIKON CORPORATION", w, h, data, RGGB, 12, NULL, 0);
    o.white_balance.kind = PC_RAW_WB_MULTIPLIERS;
    memcpy(o.white_balance.multipliers, WB, sizeof WB);
    r.width = (uint32_t)w;
    r.height = (uint32_t)h;
    r.rgb = rgb;
    r.len = w * h * 3;
    if (look != NULL && pc_raw_decode(raw.d, raw.n, &lim, &s, &e) == PC_OK) {
        if (pc_raw_fit_look(&s, &o, &r, look, &e) == PC_OK) {
            pc_raw_mat3 fitted = rx_mul(&a, &look->to_xyz);
            bool close = true;
            static const double X[4] = {0.02, 0.1, 0.3, 0.6};
            pc_raw_developed d1;
            pc_raw_developed d2;
            int q;
            for (q = 0; q < 9; q++) {
                close = close && fabs(fitted.m[q / 3][q % 3] - m.m[q / 3][q % 3]) < 0.06;
            }
            CHECK(close);
            for (q = 0; q < 4; q++) {
                CHECK_NEAR(pc_raw_look_curve_eval(&look->tone, X[q]), 1.6 * X[q] / (1.0 + 0.6 * X[q]), 0.03);
            }
            CHECK(look->delta_e < 1.5 && look->delta_e < 0.2 * look->delta_e_fallback);
            o.camera_look = look;
            if (pc_raw_develop_sensor(&s, &o, &d1, &e) == PC_OK) {
                CHECK(pc_raw_strings_any(&d1.warnings, "fitted to the camera's embedded JPEG"));
                CHECK(!pc_raw_strings_any(&d1.warnings, "sRGB primaries"));
                o.camera_look = NULL;
                if (pc_raw_develop_sensor(&s, &o, &d2, &e) == PC_OK) {
                    CHECK(pc_raw_strings_any(&d2.warnings, "sRGB primaries"));
                    CHECK(memcmp(d1.rgb, d2.rgb, d1.nrgb * 2) != 0);
                    pc_raw_developed_fini(&d2);
                }
                pc_raw_developed_fini(&d1);
            }
        } else {
            PC_T_FAIL("%s", e.msg);
        }
        /* Developing ignores an unusable look. */
        memset(look, 0, sizeof *look);
        look->to_xyz.m[0][0] = NAN;
        o.camera_look = look;
        {
            pc_raw_developed d;
            if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
                CHECK(pc_raw_strings_any(&d.warnings, "not usable"));
                CHECK(pc_raw_strings_any(&d.warnings, "sRGB primaries"));
                pc_raw_developed_fini(&d);
            }
        }
        pc_raw_sensor_fini(&s);
    }
    free(look);
    free(data);
    free(cam);
    free(rgb);
    tg_free(&raw);
}

/* ---- tone.rs ---- */

typedef struct lum_img {
    const float *y;
} lum_img;

static void lum_of(const void *ctx, size_t i, float out[3])
{
    const lum_img *l = ctx;
    out[0] = l->y[i];
}

static float *bands(size_t w, size_t h, float scale)
{
    static const float Y[3] = {0.0078125f, 0.0625f, 0.5f};
    float *y = tg_xalloc(w * h * sizeof *y);
    size_t i;
    for (i = 0; i < w * h; i++) {
        y[i] = scale * Y[(i % w) * 3 / w];
    }
    return y;
}

static float *gains(const float *y, size_t w, size_t h, const pc_raw_tone *t)
{
    rx_tonemap m;
    bool some = false;
    lum_img l;
    pc_error e;
    float *g = tg_xalloc(w * h * sizeof *g);
    size_t i;
    l.y = y;
    if (rx_tonemap_new(w, h, lum_of, &l, t, 0.0f, &m, &some, &e) != PC_OK) {
        PC_T_FAIL("%s", e.msg);
    }
    for (i = 0; i < w * h; i++) {
        g[i] = some ? rx_tonemap_gain(&m, i % w, i / w, y[i]) : 1.0f;
    }
    if (some) {
        rx_tonemap_fini(&m);
    }
    return g;
}

static void test_tone_shadows_highlights_and_exposure(void)
{
    size_t w = 300;
    size_t h = 120;
    float *y = bands(w, h, 1.0f);
    float *y4 = bands(w, h, 0.25f);
    pc_raw_tone sh = {100.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    pc_raw_tone hi = {0.0f, -100.0f, 0.0f, 0.0f, 0.0f};
    pc_raw_tone mix = {-60.0f, 40.0f, 50.0f, -40.0f, 0.0f};
    float *g = gains(y, w, h, &sh);
    float dark = log2f(g[60 * w + 50]);
    float mid = g[60 * w + 150];
    float bright = log2f(g[60 * w + 250]);
    const pc_raw_tone *ts[3] = {&sh, &hi, &mix};
    size_t k;
    CHECK(dark > 1.8f && dark < 2.3f);
    CHECK(mid > 1.0f && mid < g[60 * w + 50]);
    CHECK(bright < 0.06f);
    free(g);
    g = gains(y, w, h, &hi);
    CHECK(log2f(g[60 * w + 50]) < 0.0f && log2f(g[60 * w + 50]) > -0.15f);
    CHECK(log2f(g[60 * w + 250]) < -0.8f && log2f(g[60 * w + 250]) > -1.3f);
    free(g);
    for (k = 0; k < 3; k++) {
        float *a = gains(y, w, h, ts[k]);
        float *b = gains(y4, w, h, ts[k]);
        size_t i;
        bool same = true;
        for (i = 0; i < w * h; i++) {
            same = same && fabsf(a[i] - b[i]) < 1e-3f * a[i];
        }
        CHECK(same);
        free(a);
        free(b);
    }
    free(y);
    free(y4);
}

static void px_of(const void *ctx, size_t i, float out[3])
{
    const float *px = ctx;
    memcpy(out, px + 3 * i, 3 * sizeof *out);
}

static void test_tone_black_point_and_dehaze(void)
{
    size_t w = 300;
    size_t h = 120;
    float *y = bands(w, h, 1.0f);
    pc_raw_tone neutral = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    rx_tonemap m;
    rx_haze z;
    bool some;
    lum_img l;
    pc_error e;
    float (*px)[3] = tg_xalloc(400 * 300 * sizeof *px);
    size_t i;
    l.y = y;
    CHECK(rx_tonemap_new(w, h, lum_of, &l, &neutral, 0.33f, &m, &some, &e) == PC_OK && some);
    if (some) {
        CHECK(fabsf(m.black - 0.33f * 0.0078125f) < 1e-7f);
        CHECK(rx_tonemap_unblack(&m, m.black * 0.5f) == 0.0f);
        CHECK(fabsf(rx_tonemap_unblack(&m, 1.0f) - 1.0f) < 1e-6f);
        rx_tonemap_fini(&m);
    }
    CHECK(rx_tonemap_new(w, h, lum_of, &l, &neutral, 0.0f, &m, &some, &e) == PC_OK && !some);
    free(y);
    for (i = 0; i < 400 * 300; i++) {
        bool sky = i / 400 < 150;
        px[i][0] = sky ? 0.55f : 0.12f;
        px[i][1] = sky ? 0.6f : 0.08f;
        px[i][2] = sky ? 0.7f : 0.03f;
    }
    CHECK(rx_haze_new(400, 300, px_of, px, 100.0f, &z, &some, &e) == PC_OK && some);
    if (some) {
        float sky[3];
        float ground[3];
        rx_haze_apply(&z, 200, 40, px[40 * 400 + 200], sky);
        rx_haze_apply(&z, 200, 260, px[260 * 400 + 200], ground);
        for (i = 0; i < 3; i++) {
            CHECK(fabsf(sky[i] - px[40 * 400 + 200][i]) < 0.02f);
        }
        CHECK(ground[2] < 0.03f && ground[0] <= 0.12f);
        rx_haze_fini(&z);
    }
    CHECK(rx_haze_new(400, 300, px_of, px, -100.0f, &z, &some, &e) == PC_OK && some);
    if (some) {
        float v[3];
        rx_haze_apply(&z, 200, 260, px[260 * 400 + 200], v);
        for (i = 0; i < 3; i++) {
            CHECK(v[i] > px[260 * 400 + 200][i]);
        }
        rx_haze_fini(&z);
    }
    CHECK(rx_haze_new(400, 300, px_of, px, 0.0f, &z, &some, &e) == PC_OK && !some);
    free(px);
}

static void test_retone_changes_nothing_at_zero(void)
{
    static const float C[3][2] = {{0.01f, 0.02f}, {0.1f, 0.25f}, {1.0f, 1.0f}};
    size_t w = 300;
    size_t h = 120;
    float *y = bands(w, h, 1.0f);
    float (*px)[4] = tg_xalloc(w * h * sizeof *px);
    float (*out)[4] = tg_xalloc(w * h * sizeof *out);
    pc_raw_tone neutral = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    pc_raw_tone sh = {100.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float *g = gains(y, w, h, &sh);
    pc_error e;
    size_t i;
    for (i = 0; i < w * h; i++) {
        float v = powf(rx_tone(C, 3, y[i]), 1.0f / 1.8f);
        px[i][0] = px[i][1] = px[i][2] = v;
        px[i][3] = 1.0f;
    }
    memcpy(out, px, w * h * sizeof *px);
    CHECK(pc_raw_retone(out, w, h, C, 3, &neutral, &e) == PC_OK);
    CHECK_EQ_MEM(out, px, w * h * sizeof *px);
    CHECK(pc_raw_retone(out, w, h, C, 3, &sh, &e) == PC_OK);
    {
        static const size_t AT[3] = {60 * 300 + 50, 60 * 300 + 150, 60 * 300 + 250};
        size_t k;
        for (k = 0; k < 3; k++) {
            float want = powf(rx_tone(C, 3, y[AT[k]] * g[AT[k]]), 1.0f / 1.8f);
            CHECK_NEAR(out[AT[k]][0], want, 0.003);
        }
    }
    free(g);
    free(y);
    free(px);
    free(out);
}

/* ---- select ---- */

static bool less_u(void *ctx, size_t a, size_t b)
{
    (void)ctx;
    return a < b;
}

static void test_select_places_the_nth_element(void)
{
    uint64_t st = 7;
    int round;
    for (round = 0; round < 200; round++) {
        size_t n = 1 + (size_t)(round * 37 % 3000);
        size_t *v = tg_xalloc(n * sizeof *v);
        size_t k;
        size_t i;
        bool ok = true;
        for (i = 0; i < n; i++) {
            st = st * 6364136223846793005ull + 1442695040888963407ull;
            v[i] = (size_t)(st >> 40) % (round % 3 == 0 ? 5 : 100000);
        }
        k = (size_t)(st >> 20) % n;
        rx_select_nth(v, n, k, less_u, NULL);
        for (i = 0; i < n; i++) {
            ok = ok && (i < k ? v[i] <= v[k] : v[i] >= v[k]);
        }
        CHECK(ok);
        free(v);
    }
}

int main(int argc, char **argv)
{
    RUN(test_raf_every_storage_decodes_exactly);
    RUN(test_raf_xtrans_metadata_and_development);
    RUN(test_raf_xtrans_white_balance_is_per_site);
    RUN(test_raf_bayer_layouts);
    RUN(test_raf_unsupported_variants_and_bad_files);
    RUN(test_raf_packings);
    RUN(test_xtrans_flat_colour_is_exact_and_quality);
    RUN(test_odd_patterns_do_not_crash);
    RUN(test_measured_image_areas);
    RUN(test_tone_curve_validates_and_interpolates);
    RUN(test_look_recovers_a_known_matrix_and_curve);
    RUN(test_tone_shadows_highlights_and_exposure);
    RUN(test_tone_black_point_and_dehaze);
    RUN(test_retone_changes_nothing_at_zero);
    RUN(test_select_places_the_nth_element);
    return pc_t_finish();
}
