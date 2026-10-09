/* Unit tests of src/raw: ports of the `#[cfg(test)]` modules in crates/raw/src (tiff, sensor,
 * color, cameras, tiffep padding, cr2 and its wb_words, ljpeg, nefc, opcodes, rw2, sony,
 * demosaic, develop). The corpus-only cr2 test (wb_debug) is not ported. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "raw/internal.h"
#include "raw/raw.h"
#include "testgen.h"

static void put_le16(tg_bytes *b, uint16_t v) { tg_le16(b, v); }
static void put_le32(tg_bytes *b, uint32_t v) { tg_le32(b, v); }

/* ======================================================================== tiff.rs */

static void test_tiff_header_variants(void)
{
    rx_tiff t;
    CHECK(rx_tiff_new((const uint8_t *)"II*\0\x08\0\0\0", 8, &t));
    CHECK(rx_tiff_new((const uint8_t *)"MM\0*\0\0\0\x08", 8, &t));
    CHECK(rx_tiff_new((const uint8_t *)"IIRO\x08\0\0\0", 8, &t));
    CHECK(rx_tiff_new((const uint8_t *)"IIU\0\x08\0\0\0", 8, &t));
    CHECK(!rx_tiff_new((const uint8_t *)"II*\0", 4, &t));
    CHECK(!rx_tiff_new((const uint8_t *)"PNG\0\0\0\0\0", 8, &t));
}

static void test_tiff_self_referencing_ifds_terminate(void)
{
    /* IFD0 at 8 with one entry whose SubIFD points at itself, and a next pointer that also points
     * at itself. */
    tg_bytes b = {NULL, 0, 0};
    rx_tiff t;
    rx_ifd ifds[RX_MAX_IFDS];
    tg_put(&b, "II*\0\x08\0\0\0", 8);
    put_le16(&b, 1);
    put_le16(&b, 330);
    put_le16(&b, 4);
    put_le32(&b, 1);
    put_le32(&b, 8);
    put_le32(&b, 8);
    CHECK(rx_tiff_new(b.d, b.n, &t));
    CHECK_EQ_UINT(rx_all_ifds(&t, ifds), 1);
    tg_free(&b);
}

static void test_tiff_rational_with_zero_denominator_is_none(void)
{
    tg_bytes b = {NULL, 0, 0};
    rx_tiff t;
    rx_ifd ifd;
    rx_entry e;
    double v = 0.0;
    tg_put(&b, "II*\0\x08\0\0\0", 8);
    put_le16(&b, 1);
    put_le16(&b, 50730);
    put_le16(&b, 10);
    put_le32(&b, 1);
    put_le32(&b, 26);
    put_le32(&b, 0);
    put_le32(&b, 5);
    put_le32(&b, 0);
    CHECK(rx_tiff_new(b.d, b.n, &t));
    CHECK(rx_ifd_at(&t, 8, 0, &ifd));
    CHECK(rx_ifd_get(&t, &ifd, 50730, &e));
    CHECK(!rx_float(&t, &e, 0, &v));
    tg_free(&b);
}

/* ======================================================================== sensor.rs */

static void test_sensor_bayer_patterns(void)
{
    static const char *const YES[4] = {"RGGB", "BGGR", "GRBG", "GBRG"};
    static const char *const NO[3] = {"RGBG", "RRGB", "GGRB"};
    pc_raw_cfa c;
    uint8_t ph[4];
    size_t i;
    for (i = 0; i < 4; i++) {
        bool ok = pc_raw_cfa_bayer(YES[i], &c);
        CHECK(ok);
        if (ok && !pc_raw_cfa_is_bayer(&c)) {
            PC_T_FAIL("%s is Bayer", YES[i]);
        }
    }
    for (i = 0; i < 3; i++) {
        bool ok = pc_raw_cfa_bayer(NO[i], &c);
        CHECK(ok);
        if (ok && pc_raw_cfa_is_bayer(&c)) {
            PC_T_FAIL("%s is not Bayer", NO[i]);
        }
    }
    CHECK(pc_raw_cfa_bayer("RGGB", &c));
    c.origin_x = 1;
    pc_raw_cfa_phase(&c, 0, 0, ph); /* GRBG */
    CHECK_EQ_UINT(ph[0], 1);
    CHECK_EQ_UINT(ph[1], 0);
    CHECK_EQ_UINT(ph[2], 2);
    CHECK_EQ_UINT(ph[3], 1);
}

static void test_sensor_unpack_12_bit(void)
{
    /* Two 12-bit samples 0xABC, 0x123 packed MSB first. */
    static const uint8_t PACKED[3] = {0xAB, 0xC1, 0x23};
    /* 12-bit data in 16-bit containers. */
    static const uint8_t WIDE[4] = {0xBC, 0x0A, 0x23, 0x01};
    static const uint8_t SHORT[1] = {0xAB};
    uint16_t out[2] = {0, 0};
    pc_error e;
    CHECK_EQ_INT(rx_unpack(PACKED, 3, 2, 1, 1, 12, true, out, &e), PC_OK);
    CHECK_EQ_UINT(out[0], 0xABC);
    CHECK_EQ_UINT(out[1], 0x123);
    out[0] = out[1] = 0;
    CHECK_EQ_INT(rx_unpack(WIDE, 4, 2, 1, 1, 12, true, out, &e), PC_OK);
    CHECK_EQ_UINT(out[0], 0xABC);
    CHECK_EQ_UINT(out[1], 0x123);
    CHECK(rx_unpack(SHORT, 1, 2, 1, 1, 12, true, out, &e) != PC_OK);
}

/* ======================================================================== color.rs */

static bool close_to(double a, double b, double eps) { return fabs(a - b) < eps; }

static void test_color_inverse_roundtrip(void)
{
    pc_raw_mat3 m = rx_srgb_to_xyz_d50();
    pc_raw_mat3 i;
    pc_raw_mat3 p;
    pc_raw_mat3 zero;
    int r;
    int c;
    CHECK(rx_invert(&m, &i));
    p = rx_mul(&m, &i);
    for (r = 0; r < 3; r++) {
        for (c = 0; c < 3; c++) {
            CHECK(close_to(p.m[r][c], r == c ? 1.0 : 0.0, 1e-9));
        }
    }
    memset(&zero, 0, sizeof zero);
    CHECK(!rx_invert(&zero, &i));
}

static void test_color_white_points_map_to_d50(void)
{
    static const double ONES[3] = {1.0, 1.0, 1.0};
    pc_raw_mat3 m = rx_srgb_to_xyz_d50();
    pc_raw_mat3 pp = rx_xyz_d50_to_prophoto();
    double w[3];
    double d50[3];
    double p[3];
    rx_apply(&m, ONES, w);
    CHECK(close_to(w[0], 0.9642, 2e-3) && close_to(w[1], 1.0, 1e-9) && close_to(w[2], 0.8249, 2e-3));
    rx_xy_to_xyz(RX_D50_XY, d50);
    rx_apply(&pp, d50, p);
    CHECK(close_to(p[0], 1.0, 1e-9) && close_to(p[1], 1.0, 1e-9) && close_to(p[2], 1.0, 1e-9));
}

static void test_color_mccamy_reference_points(void)
{
    static const double A[2] = {0.44757, 0.40745}; /* illuminant A */
    CHECK(close_to(rx_cct(RX_D65_XY), 6504.0, 15.0));
    CHECK(close_to(rx_cct(A), 2856.0, 15.0));
}

static void test_color_dng_color_matrix_roundtrip(void)
{
    /* A synthetic camera whose XYZ -> camera matrix is the inverse of sRGB: with a D65 neutral,
     * the balanced pipeline must reproduce sRGB -> XYZ D50. */
    static const double REC709[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}};
    static const double ONES[3] = {1.0, 1.0, 1.0};
    pc_raw_mat3 cam_to_xyz = rx_rgb_to_xyz(REC709, RX_D65_XY);
    pc_raw_mat3 cm;
    pc_raw_color_info info;
    double neutral[3];
    double xy[2];
    pc_raw_mat3 m;
    double w[3];
    CHECK(rx_invert(&cam_to_xyz, &cm));
    memset(&info, 0, sizeof info);
    info.ncalibrations = 1;
    info.calibrations[0].temperature = 6504.0;
    info.calibrations[0].color_matrix = cm;
    info.calibrations[0].has_forward_matrix = false;
    info.calibrations[0].camera_calibration = RX_IDENTITY;
    CHECK(rx_xy_to_neutral(&info, RX_D65_XY, neutral));
    CHECK(close_to(neutral[0], 1.0, 1e-3) && close_to(neutral[1], 1.0, 1e-3) && close_to(neutral[2], 1.0, 1e-3));
    rx_neutral_to_xy(&info, neutral, xy);
    CHECK(close_to(xy[0], RX_D65_XY[0], 1e-4) && close_to(xy[1], RX_D65_XY[1], 1e-4));
    CHECK(pc_raw_balanced_to_xyz_d50(&info, neutral, &m));
    rx_apply(&m, ONES, w);
    CHECK(close_to(w[0], 0.9642, 3e-3) && close_to(w[1], 1.0, 1e-9) && close_to(w[2], 0.8249, 3e-3));
}

/* ======================================================================== cameras.rs */

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = tg_xalloc(n);
    memcpy(d, s, n);
    return d;
}

static void nef_sensor(pc_raw_sensor *s, const char *model, size_t width, size_t height)
{
    pc_error e;
    pc_raw_sensor_init(s);
    s->format = PC_RAW_NEF;
    s->make = xstrdup("NIKON CORPORATION");
    s->model = xstrdup(model);
    s->width = width;
    s->height = height;
    s->samples = 1;
    CHECK_EQ_INT(rx_black_uniform(&s->black, 0.0f, &e), PC_OK);
    s->white[0] = s->white[1] = s->white[2] = 16383.0f;
    s->active = pc_raw_rect_make(0, 0, width, height);
    s->crop = pc_raw_rect_make(0, 0, width, height);
    s->has_camera_wb = true;
    s->camera_wb[0] = 2.0;
    s->camera_wb[1] = 1.0;
    s->camera_wb[2] = 1.4;
    s->orientation = 1;
    CHECK_EQ_INT(rx_warn(&s->warnings, &e, "%s", "NEF: black level not recorded in a documented tag; assumed 0"),
                 PC_OK);
}

static void test_cameras_a_d4_nef_gets_its_crop_levels_matrix_and_curve(void)
{
    static const double N[3] = {0.5, 1.0, 0.7};
    pc_raw_sensor s;
    pc_error e;
    pc_raw_mat3 m;
    size_t ntone;
    (void)rx_d4_tone(&ntone);
    nef_sensor(&s, "NIKON D4", 4992, 3292);
    CHECK_EQ_INT(rx_cameras_apply(&s, &e), PC_OK);
    CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(8, 6, 4928, 3280)));
    CHECK(s.white[0] == 15520.0f && s.white[1] == 15520.0f && s.white[2] == 15520.0f);
    CHECK_EQ_UINT(s.color.ncalibrations, 1);
    CHECK(s.color.calibrations[0].has_forward_matrix);
    CHECK_EQ_MEM(&s.color.calibrations[0].forward_matrix, rx_d4_forward(), sizeof(pc_raw_mat3));
    CHECK_EQ_UINT(s.ntone_curve, ntone);
    /* The D4's black level is known. */
    CHECK_EQ_UINT(s.warnings.n, 0);
    /* The profile gives a colour conversion for the as-shot white balance. */
    CHECK(pc_raw_balanced_to_xyz_d50(&s.color, N, &m));
    pc_raw_sensor_fini(&s);
}

static void test_cameras_other_cameras_and_sizes_are_left_alone(void)
{
    static const struct {
        const char *model;
        size_t w, h;
    } CASES[3] = {{"NIKON D4S", 4992, 3292}, {"NIKON D4", 2496, 1646}, {"NIKON D800", 7424, 4924}};
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_raw_sensor s;
        pc_error e;
        nef_sensor(&s, CASES[i].model, CASES[i].w, CASES[i].h);
        CHECK_EQ_INT(rx_cameras_apply(&s, &e), PC_OK);
        if (!pc_raw_rect_eq(s.crop, pc_raw_rect_make(0, 0, CASES[i].w, CASES[i].h))) {
            PC_T_FAIL("%s %zux%zu crop changed", CASES[i].model, CASES[i].w, CASES[i].h);
        }
        CHECK(s.ntone_curve == 0 && s.tone_curve == NULL && s.color.ncalibrations == 0 && s.white[0] == 16383.0f &&
              s.white[1] == 16383.0f && s.white[2] == 16383.0f);
        CHECK_EQ_UINT(s.warnings.n, 1);
        pc_raw_sensor_fini(&s);
    }
}

static void test_cameras_d4_forward_matrix_maps_neutral_to_d50_and_curve_reaches_white(void)
{
    static const double ONES[3] = {1.0, 1.0, 1.0};
    double n[3];
    size_t nt;
    const float(*tone)[2] = rx_d4_tone(&nt);
    size_t i;
    bool increasing = true;
    rx_apply(rx_d4_forward(), ONES, n);
    CHECK(fabs(n[0] - 0.9642) < 1e-3 && fabs(n[1] - 1.0) < 1e-3 && fabs(n[2] - 0.8252) < 1e-3);
    for (i = 0; i + 1 < nt; i++) {
        increasing = increasing && tone[i + 1][0] > tone[i][0] && tone[i + 1][1] >= tone[i][1];
    }
    CHECK(increasing);
    CHECK(nt > 0);
    if (nt > 0) {
        CHECK(tone[nt - 1][0] == 1.0f && tone[nt - 1][1] == 1.0f);
    }
}

/* ======================================================================== tiffep.rs (padding_tests) */

/* w x h image data whose last pad columns hold fill (one row in 20 off by 2). */
static uint16_t *padded_data(size_t w, size_t h, size_t pad, uint16_t fill)
{
    uint16_t *v = tg_xalloc(w * h * sizeof *v);
    size_t i;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w;
        size_t y = i / w;
        if (x >= w - pad) {
            v[i] = y % 20 == 7 ? (uint16_t)(fill + 2) : fill;
        } else {
            v[i] = (uint16_t)((x * 37 + y * 101) % 3000);
        }
    }
    return v;
}

static size_t padded_width_of(size_t w, size_t h, size_t pad, uint16_t fill)
{
    uint16_t *v = padded_data(w, h, pad, fill);
    size_t r = rx_padded_width(v, w * h, w, h, 12);
    free(v);
    return r;
}

static void test_tiffep_trailing_padding_is_trimmed(void)
{
    CHECK_EQ_UINT(padded_width_of(300, 40, 46, 256), 254);
    /* odd padding keeps the CFA phase */
    CHECK_EQ_UINT(padded_width_of(300, 40, 45, 256), 254);
    CHECK_EQ_UINT(padded_width_of(300, 40, 0, 256), 300);
}

static void test_tiffep_bright_or_varied_edges_and_bad_input_are_kept(void)
{
    static const uint16_t ZEROS[10] = {0};
    /* an edge clipped to white is image, not padding */
    CHECK_EQ_UINT(padded_width_of(300, 40, 46, 4095), 300);
    /* at most 128 columns are trimmed */
    CHECK_EQ_UINT(padded_width_of(300, 40, 200, 256), 172);
    /* short data, tiny images: unchanged, no crash */
    CHECK_EQ_UINT(rx_padded_width(ZEROS, 10, 300, 40, 12), 300);
    CHECK_EQ_UINT(padded_width_of(100, 4, 20, 256), 100);
    CHECK_EQ_UINT(rx_padded_width(NULL, 0, 0, 0, 12), 0);
}

/* ======================================================================== cr2.rs */

static void test_cr2_red_row_measurement_never_panics_on_odd_geometry(void)
{
    pc_raw_rect areas[5];
    uint16_t *data = tg_xalloc(64 * 32 * sizeof *data);
    size_t out = 99;
    size_t i;
    for (i = 0; i < 64 * 32; i++) {
        data[i] = 1000;
    }
    areas[0] = pc_raw_rect_make(0, 0, 0, 0);
    areas[1] = pc_raw_rect_make(0, 0, 64, 32);
    areas[2] = pc_raw_rect_make(60, 30, 100, 100);
    areas[3] = pc_raw_rect_make(SIZE_MAX - 1, 0, 4, 4);
    areas[4] = pc_raw_rect_make(0, SIZE_MAX - 1, 4, 4);
    for (i = 0; i < 5; i++) {
        /* Flat data: inconclusive, never a guess. */
        if (rx_measured_red_row(data, 64 * 32, 64, areas[i], 0.0f, 4000.0f, &out)) {
            PC_T_FAIL("area %zu gave red row %zu", i, out);
        }
    }
    CHECK(!rx_measured_red_row(NULL, 0, 64, pc_raw_rect_make(0, 0, 64, 32), 0.0f, 4000.0f, &out));
    CHECK(!rx_measured_red_row(data, 64 * 32, 0, pc_raw_rect_make(0, 0, 64, 32), 0.0f, 4000.0f, &out));
    CHECK(!rx_measured_red_row(data, 64 * 32, 64, pc_raw_rect_make(0, 0, 64, 32), NAN, NAN, &out));
    free(data);
}

static void test_cr2_red_row_measurement_finds_both_phases(void)
{
    /* Grey scene under white-balance gains R 2, B 1.5: green 1000, red 500, blue 667, plus texture. */
    static const float LEVEL[3] = {500.0f, 1000.0f, 667.0f};
    size_t red_row;
    for (red_row = 0; red_row < 2; red_row++) {
        size_t w = 64;
        size_t h = 64;
        uint16_t *data = tg_xalloc(w * h * sizeof *data);
        size_t i;
        size_t out = 99;
        for (i = 0; i < w * h; i++) {
            size_t x = i % w;
            size_t y = i / w;
            float tex = 1.0f + 0.2f * (float)(((x / 3) ^ (y / 5)) & 1);
            size_t c = (y & 1) == red_row ? ((x & 1) == 0 ? 0u : 1u) : ((x & 1) == 0 ? 1u : 2u);
            data[i] = (uint16_t)(100.0f + LEVEL[c] * tex);
        }
        CHECK(rx_measured_red_row(data, w * h, w, pc_raw_rect_make(0, 0, w, h), 100.0f, 4000.0f, &out));
        CHECK_EQ_UINT(out, red_row);
        free(data);
    }
}

/* cr2.rs wb_words */

static tg_cr2 minimal_cr2(uint16_t *data)
{
    tg_cr2 s;
    size_t i;
    memset(&s, 0, sizeof s);
    for (i = 0; i < 64; i++) {
        data[i] = 2048;
    }
    s.width = 8;
    s.height = 8;
    s.data = data;
    s.precision = 12;
    s.components = 1;
    s.nslices = 0;
    s.has_borders = false;
    s.has_wb = false;
    s.orientation = 1;
    s.has_model_id = false;
    return s;
}

/* ColorData arrives as UNDEFINED bytes that are 16-bit words; the offsets count words. */
static void test_cr2_color_data_wb_as_words(void)
{
    uint16_t data[64];
    tg_cr2 spec = minimal_cr2(data);
    tg_bytes b;
    pc_raw_limits l = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_error e;
    spec.has_wb = true;
    spec.wb_rggb[0] = 1613;
    spec.wb_rggb[1] = 951;
    spec.wb_rggb[2] = 951;
    spec.wb_rggb[3] = 1605;
    b = tg_cr2_build(&spec);
    pc_raw_sensor_init(&s);
    CHECK_EQ_INT(pc_raw_decode(b.d, b.n, &l, &s, &e), PC_OK);
    CHECK(s.has_camera_wb);
    if (s.has_camera_wb) {
        CHECK(fabs(s.camera_wb[0] - 1613.0 / 951.0) < 1e-9 && s.camera_wb[1] == 1.0 &&
              fabs(s.camera_wb[2] - 1605.0 / 951.0) < 1e-9);
    }
    pc_raw_sensor_fini(&s);
    tg_free(&b);
}

/* A ColorData entry too short for the offsets reads no balance instead of running past it. */
static void test_cr2_short_color_data_reads_no_wb(void)
{
    uint16_t data[64];
    tg_cr2 spec = minimal_cr2(data);
    tg_bytes b = tg_cr2_build(&spec);
    pc_raw_limits l = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_error e;
    pc_raw_sensor_init(&s);
    CHECK_EQ_INT(pc_raw_decode(b.d, b.n, &l, &s, &e), PC_OK);
    CHECK(!s.has_camera_wb);
    pc_raw_sensor_fini(&s);
    tg_free(&b);
}

/* ======================================================================== ljpeg.rs */

/* Deterministic pseudo-random samples with some structure (smooth + noise). */
static uint16_t *lj_samples(size_t n, uint8_t precision, uint64_t seed)
{
    uint16_t *v = tg_xalloc((n > 0 ? n : 1) * sizeof *v);
    uint64_t s = seed;
    uint32_t max = (uint32_t)((1u << precision) - 1);
    size_t i;
    for (i = 0; i < n; i++) {
        uint32_t noise;
        uint32_t base;
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        noise = (uint32_t)(s >> 33) % 64;
        base = ((uint32_t)i * 37u) % (max + 1);
        v[i] = (uint16_t)((base + noise) % (max + 1));
    }
    return v;
}

static void test_ljpeg_roundtrip_all_predictors_and_precisions(void)
{
    static const uint8_t PRECISIONS[5] = {2, 8, 12, 14, 16};
    size_t pi;
    for (pi = 0; pi < 5; pi++) {
        uint8_t precision = PRECISIONS[pi];
        uint8_t predictor;
        for (predictor = 1; predictor <= 7; predictor++) {
            size_t comps;
            for (comps = 1; comps <= 4; comps++) {
                size_t w = 13;
                size_t h = 7;
                uint16_t *v = lj_samples(w * h * comps, precision, (uint64_t)predictor * 31 + comps);
                tg_bytes j = tg_lj92_encode(v, w, h, comps, precision, predictor, 0);
                rx_frame f;
                uint16_t *out = NULL;
                size_t nout = 0;
                pc_error e;
                pc_status st = rx_ljpeg_decode(j.d, j.n, SIZE_MAX, &f, &out, &nout, &e);
                CHECK_EQ_INT(st, PC_OK);
                if (st == PC_OK) {
                    CHECK(f.precision == precision && f.width == w && f.height == h && f.components == comps);
                    if (nout != w * h * comps || memcmp(out, v, nout * sizeof *out) != 0) {
                        PC_T_FAIL("P=%u pred=%u comps=%zu", (unsigned)precision, (unsigned)predictor, comps);
                    }
                    free(out);
                }
                tg_free(&j);
                free(v);
            }
        }
    }
}

static void test_ljpeg_roundtrip_with_restarts_and_extreme_values(void)
{
    static const size_t ROWS[3] = {1, 2, 4};
    size_t w = 16;
    size_t h = 9;
    size_t n = w * h * 2;
    uint16_t *v = tg_xalloc(n * sizeof *v);
    size_t i;
    /* Alternating 0 / 65535 forces the 16-bit and 32768 difference categories. */
    for (i = 0; i < n; i++) {
        v[i] = (i / 3) % 2 == 0 ? 0 : 65535;
    }
    for (i = 0; i < 3; i++) {
        tg_bytes j = tg_lj92_encode(v, w, h, 2, 16, 1, ROWS[i]);
        rx_frame f;
        uint16_t *out = NULL;
        size_t nout = 0;
        pc_error e;
        pc_status st = rx_ljpeg_decode(j.d, j.n, SIZE_MAX, &f, &out, &nout, &e);
        CHECK_EQ_INT(st, PC_OK);
        if (st == PC_OK) {
            if (nout != n || memcmp(out, v, n * sizeof *out) != 0) {
                PC_T_FAIL("restart every %zu rows", ROWS[i]);
            }
            free(out);
        }
        tg_free(&j);
    }
    free(v);
}

static void ljpeg_try(const uint8_t *d, size_t n, size_t max, pc_status *st_out, pc_error *e)
{
    rx_frame f;
    uint16_t *out = NULL;
    size_t nout = 0;
    pc_status st = rx_ljpeg_decode(d, n, max, &f, &out, &nout, e);
    if (st == PC_OK) {
        free(out);
    }
    if (st_out != NULL) {
        *st_out = st;
    }
}

static void test_ljpeg_hostile_streams_error_without_panicking(void)
{
    static const uint8_t LOSSY[15] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 8, 0, 1, 0, 1, 1, 1, 0x11, 0};
    uint16_t *v = lj_samples(32 * 8, 14, 7);
    tg_bytes j = tg_lj92_encode(v, 32, 8, 1, 14, 1, 2);
    uint8_t *b = tg_xalloc(j.n);
    uint64_t s = 0x12345678ull;
    size_t n;
    int round;
    pc_status st;
    pc_error e;
    for (n = 0; n < j.n; n++) {
        ljpeg_try(j.d, n, SIZE_MAX, NULL, &e);
    }
    for (round = 0; round < 2000; round++) {
        int k;
        memcpy(b, j.d, j.n);
        for (k = 0; k < 4; k++) {
            size_t at;
            s = s * 6364136223846793005ull + 1;
            at = (size_t)(s >> 33) % j.n;
            b[at] = (uint8_t)(s >> 20);
        }
        ljpeg_try(b, j.n, (size_t)1 << 20, NULL, &e);
    }
    /* A frame larger than allowed is refused before allocating. */
    ljpeg_try(j.d, j.n, 10, &st, &e);
    CHECK_EQ_INT(st, PC_ERR_LIMIT);
    /* A huge frame with a tiny entropy segment is refused as truncated. */
    memcpy(b, j.d, j.n);
    b[7] = (uint8_t)(60000 >> 8); /* height */
    b[8] = (uint8_t)(60000 & 0xFF);
    b[9] = (uint8_t)(60000 >> 8); /* width */
    b[10] = (uint8_t)(60000 & 0xFF);
    ljpeg_try(b, j.n, SIZE_MAX, &st, &e);
    CHECK(st != PC_OK);
    /* Lossy JPEG markers are reported as unsupported. */
    ljpeg_try(LOSSY, sizeof LOSSY, SIZE_MAX, &st, &e);
    CHECK_EQ_INT(st, PC_ERR_UNSUPPORTED);
    free(b);
    tg_free(&j);
    free(v);
}

/* ======================================================================== nefc.rs */

/* The Huffman tables of src/raw/nefc.c (static there). */
#define UNSEEN 0xFF
typedef struct ntable {
    const uint8_t *counts;
    size_t ncounts;
    const uint8_t *symbols;
    size_t nsymbols;
} ntable;

static const uint8_t LL12_C[] = {0, 1, 4, 2, 3, 1, 2};
static const uint8_t LL12_S[] = {5, 4, 6, 3, 7, 2, 8, 1, 9, 0, 10, 11, 12};
static const uint8_t LL14_C[] = {0, 1, 4, 2, 2, 3, 1, 2};
static const uint8_t LL14_S[] = {7, 6, 8, 5, 9, 4, 10, 3, 11, 12, 2, 0, 1, 13, 14};
static const uint8_t LY12_C[] = {0, 1, 5, 1, 1, 1, 1, 1, 1};
static const uint8_t LY12_S[] = {5, 4, 3, 6, 2, 7, 1, 0, 8, 9, UNSEEN, 10};
static const uint8_t LY14_C[] = {0, 1, 4, 3, 1, 1, 1, 1, 1};
static const uint8_t LY14_S[] = {5, 6, 4, 7, 8, 3, 9, 2, 1, 0, 10, 11, 12};

#define NT(c, s) {c, sizeof c, s, sizeof s}
static const ntable LOSSLESS_12 = NT(LL12_C, LL12_S);
static const ntable LOSSLESS_14 = NT(LL14_C, LL14_S);
static const ntable LOSSY_12 = NT(LY12_C, LY12_S);
static const ntable LOSSY_14 = NT(LY14_C, LY14_S);
#undef NT

/* MSB-first bit writer for the test encoder. */
typedef struct nwriter {
    tg_bytes out;
    uint64_t acc;
    uint32_t n;
} nwriter;

static void nw_put(nwriter *w, uint32_t v, uint32_t len)
{
    uint32_t i;
    for (i = len; i-- > 0;) {
        w->acc = (w->acc << 1) | ((v >> i) & 1u);
        w->n++;
        if (w->n == 8) {
            tg_byte(&w->out, (uint8_t)w->acc);
            w->acc = 0;
            w->n = 0;
        }
    }
}

static tg_bytes nw_finish(nwriter *w)
{
    static const uint8_t ZEROS[4] = {0, 0, 0, 0};
    if (w->n > 0) {
        tg_byte(&w->out, (uint8_t)(w->acc << (8 - w->n)));
    }
    tg_put(&w->out, ZEROS, 4);
    return w->out;
}

/* (code, length) per category, from the canonical table. */
static void nef_codes(const ntable *t, uint32_t code_of[17], uint32_t len_of[17])
{
    uint32_t code = 0;
    size_t sym = 0;
    size_t i;
    memset(code_of, 0, 17 * sizeof *code_of);
    memset(len_of, 0, 17 * sizeof *len_of);
    for (i = 0; i < t->ncounts; i++) {
        uint8_t k;
        for (k = 0; k < t->counts[i]; k++) {
            uint8_t s = sym < t->nsymbols ? t->symbols[sym] : UNSEEN;
            sym++;
            if (s != UNSEEN && s < 17) {
                code_of[s] = code;
                len_of[s] = (uint32_t)i + 1;
            }
            code++;
        }
        code <<= 1;
    }
}

/* Encodes values (samples or curve indices) the way the camera does. */
static tg_bytes nef_encode(const int32_t *values, size_t n, size_t w, const ntable *t, const int32_t seeds[2][2])
{
    uint32_t code_of[17];
    uint32_t len_of[17];
    nwriter wr;
    int32_t vpred[2][2];
    size_t i;
    memset(&wr, 0, sizeof wr);
    memcpy(vpred, seeds, sizeof vpred);
    nef_codes(t, code_of, len_of);
    for (i = 0; i < n; i++) {
        size_t y = i / w;
        size_t x = i % w;
        int32_t v = values[i];
        int32_t pred = x < 2 ? vpred[y & 1][x] : values[i - 2];
        int32_t d;
        uint32_t a;
        uint32_t k = 0;
        if (x < 2) {
            vpred[y & 1][x] = v;
        }
        d = v - pred;
        a = d < 0 ? (uint32_t)(-(int64_t)d) : (uint32_t)d;
        while (a >> k != 0) {
            k++;
        }
        if (len_of[k] == 0) {
            PC_T_FAIL("no code for category %u", (unsigned)k);
        }
        nw_put(&wr, code_of[k], len_of[k]);
        nw_put(&wr, d < 0 ? (uint32_t)(d + (1 << k) - 1) : (uint32_t)d, k);
    }
    return nw_finish(&wr);
}

/* Smooth gradient + texture + hard edges, within 0..=max. */
static int32_t *nef_image(size_t w, size_t h, int32_t max, uint32_t seed)
{
    int32_t *v = tg_xalloc(w * h * sizeof *v);
    uint32_t s = seed;
    size_t i;
    for (i = 0; i < w * h; i++) {
        int32_t x = (int32_t)(i % w);
        int32_t y = (int32_t)(i / w);
        int32_t noise;
        int32_t base;
        int32_t p;
        s = s * 1664525u + 1013904223u;
        noise = (int32_t)(s >> 24) % 9 - 4;
        base = (x * 37 + y * 11) % (max / 2) + ((x / 7 + y / 5) % 3 == 0 ? max / 3 : 0);
        p = base + noise;
        v[i] = p < 0 ? 0 : p > max ? max : p;
    }
    return v;
}

static rx_nef_table nef_lossless(uint32_t bits)
{
    rx_nef_table t;
    int32_t s = 1 << (bits - 3);
    memset(&t, 0, sizeof t);
    t.seeds[0][0] = t.seeds[0][1] = t.seeds[1][0] = t.seeds[1][1] = s;
    return t;
}

static void put16(tg_bytes *b, uint16_t v, bool le)
{
    if (le) {
        tg_le16(b, v);
    } else {
        tg_be16(b, v);
    }
}

static tg_bytes nef_lossy_table(uint32_t bits, bool le)
{
    tg_bytes t = {NULL, 0, 0};
    double max = (double)((1u << bits) - 1);
    uint32_t i;
    tg_byte(&t, 0x44);
    tg_byte(&t, 0x20);
    for (i = 0; i < 4; i++) {
        put16(&t, 300, le);
    }
    put16(&t, 257, le);
    for (i = 0; i < 257; i++) {
        double f = (double)i / 64.0;
        put16(&t, (uint16_t)(max * sqrt(f < 1.0 ? f : 1.0)), le);
    }
    tg_resize(&t, 624);
    return t;
}

/* A 14-bit lossless table (version 0x46) with the given seeds. */
static tg_bytes nef_lossless_table(uint16_t seed, bool le)
{
    tg_bytes t = {NULL, 0, 0};
    int i;
    tg_byte(&t, 0x46);
    tg_byte(&t, 0x30);
    for (i = 0; i < 4; i++) {
        put16(&t, seed, le);
    }
    tg_resize(&t, 46);
    return t;
}

/* The decoded samples equal the expected values. */
static bool same16(const uint16_t *a, const int32_t *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if ((int32_t)a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static void test_nefc_tables_are_prefix_codes(void)
{
    const ntable *ts[4] = {&LOSSLESS_12, &LOSSLESS_14, &LOSSY_12, &LOSSY_14};
    static const uint32_t BITS[4] = {12, 14, 12, 14};
    size_t k;
    for (k = 0; k < 4; k++) {
        const ntable *t = ts[k];
        size_t sum = 0;
        double kraft = 0.0;
        size_t i;
        size_t w = 48;
        size_t h = 7;
        int32_t *img;
        tg_bytes src;
        rx_nef_table tbl = nef_lossless(BITS[k]);
        uint16_t *out = NULL;
        pc_error e;
        pc_status st;
        for (i = 0; i < t->ncounts; i++) {
            sum += t->counts[i];
            kraft += (double)t->counts[i] / (double)(1u << (i + 1));
        }
        CHECK_EQ_UINT(sum, t->nsymbols);
        CHECK(kraft <= 1.0);
        /* Huffman::new is internal to nefc.c: the table must decode its own code words. */
        img = nef_image(w, h, 1023, 7);
        if (k >= 2) {
            size_t c;
            tbl.ncurve = ((size_t)1 << BITS[k]) + 1;
            tbl.curve = tg_xalloc(tbl.ncurve * sizeof *tbl.curve);
            for (c = 0; c < tbl.ncurve; c++) {
                tbl.curve[c] = (uint16_t)c;
            }
        }
        src = nef_encode(img, w * h, w, t, (const int32_t(*)[2])tbl.seeds);
        st = rx_nef_decode(src.d, src.n, w, h, BITS[k], &tbl, &out, &e);
        CHECK_EQ_INT(st, PC_OK);
        if (st == PC_OK) {
            CHECK(same16(out, img, w * h));
            free(out);
        }
        rx_nef_table_fini(&tbl);
        tg_free(&src);
        free(img);
    }
}

static void test_nefc_lossless_round_trip_12_and_14(void)
{
    static const uint32_t BITS[2] = {12, 14};
    const ntable *ts[2] = {&LOSSLESS_12, &LOSSLESS_14};
    size_t k;
    for (k = 0; k < 2; k++) {
        uint32_t bits = BITS[k];
        size_t w = 64;
        size_t h = 9;
        int32_t max = (1 << bits) - 1;
        int32_t *img = nef_image(w, h, max, bits);
        rx_nef_table table = nef_lossless(bits);
        tg_bytes src;
        uint16_t *out = NULL;
        pc_error e;
        pc_status st;
        img[5] = 0;
        img[7] = max;
        img[w + 9] = max;
        src = nef_encode(img, w * h, w, ts[k], (const int32_t(*)[2])table.seeds);
        st = rx_nef_decode(src.d, src.n, w, h, bits, &table, &out, &e);
        CHECK_EQ_INT(st, PC_OK);
        if (st == PC_OK) {
            if (!same16(out, img, w * h)) {
                PC_T_FAIL("%u-bit", (unsigned)bits);
            }
            free(out);
        }
        tg_free(&src);
        free(img);
    }
}

static void test_nefc_lossy_type2_curve_and_round_trip(void)
{
    static const uint32_t BITS[2] = {12, 14};
    const ntable *ts[2] = {&LOSSY_12, &LOSSY_14};
    size_t k;
    for (k = 0; k < 2; k++) {
        uint32_t bits = BITS[k];
        int le;
        for (le = 0; le < 2; le++) {
            tg_bytes lt = nef_lossy_table(bits, le != 0);
            rx_nef_table table;
            pc_error e;
            size_t step = ((size_t)1 << bits) / 256;
            size_t i;
            bool mono = true;
            size_t w = 48;
            size_t h = 7;
            int32_t *img;
            tg_bytes src;
            uint16_t *out = NULL;
            pc_status st = rx_nef_parse_table(lt.d, lt.n, le != 0, bits, &table, &e);
            CHECK_EQ_INT(st, PC_OK);
            tg_free(&lt);
            if (st != PC_OK) {
                continue;
            }
            CHECK(table.seeds[0][0] == 300 && table.seeds[0][1] == 300 && table.seeds[1][0] == 300 &&
                  table.seeds[1][1] == 300);
            CHECK(table.curve != NULL);
            if (table.curve == NULL) {
                continue;
            }
            CHECK_EQ_UINT(table.ncurve, ((size_t)1 << bits) + 1);
            CHECK_EQ_UINT(table.curve[64 * step], (1u << bits) - 1);
            for (i = 0; i + 1 < table.ncurve; i++) {
                mono = mono && table.curve[i] <= table.curve[i + 1];
            }
            CHECK(mono);
            img = nef_image(w, h, (int32_t)(64 * step) - 1, 7);
            src = nef_encode(img, w * h, w, ts[k], (const int32_t(*)[2])table.seeds);
            st = rx_nef_decode(src.d, src.n, w, h, bits, &table, &out, &e);
            CHECK_EQ_INT(st, PC_OK);
            if (st == PC_OK) {
                bool same = true;
                for (i = 0; i < w * h; i++) {
                    same = same && out[i] == table.curve[img[i]];
                }
                CHECK(same);
                free(out);
            }
            tg_free(&src);
            free(img);
            rx_nef_table_fini(&table);
        }
    }
}

static void test_nefc_lossy_type1_full_curve(void)
{
    static const uint8_t HEAD[12] = {0x44, 0x10, 0, 10, 0, 10, 0, 10, 0, 10, 0, 5};
    static const uint16_t CURVE[5] = {0, 100, 400, 900, 1600};
    static const int32_t IMG[8] = {0, 4, 1, 3, 2, 2, 4, 0};
    static const uint16_t WANT[8] = {0, 1600, 100, 900, 400, 400, 1600, 0};
    tg_bytes t = {NULL, 0, 0};
    rx_nef_table table;
    pc_error e;
    tg_bytes src;
    uint16_t *out = NULL;
    size_t i;
    pc_status st;
    tg_put(&t, HEAD, sizeof HEAD);
    for (i = 0; i < 5; i++) {
        tg_be16(&t, CURVE[i]);
    }
    st = rx_nef_parse_table(t.d, t.n, false, 12, &table, &e);
    tg_free(&t);
    CHECK_EQ_INT(st, PC_OK);
    if (st != PC_OK) {
        return;
    }
    CHECK(table.curve != NULL && table.ncurve == 5 && memcmp(table.curve, CURVE, sizeof CURVE) == 0);
    src = nef_encode(IMG, 8, 4, &LOSSY_12, (const int32_t(*)[2])table.seeds);
    st = rx_nef_decode(src.d, src.n, 4, 2, 12, &table, &out, &e);
    CHECK_EQ_INT(st, PC_OK);
    if (st == PC_OK) {
        CHECK_EQ_MEM(out, WANT, sizeof WANT);
        free(out);
    }
    tg_free(&src);
    rx_nef_table_fini(&table);
}

static pc_status parse_only(const uint8_t *t, size_t n, bool le, uint32_t bits)
{
    rx_nef_table table;
    pc_error e;
    pc_status st = rx_nef_parse_table(t, n, le, bits, &table, &e);
    if (st == PC_OK) {
        rx_nef_table_fini(&table);
    }
    return st;
}

static void test_nefc_unsupported_and_malformed_tables(void)
{
    static const uint8_t V49[12] = {0x49, 0x30, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t SHORT46[3] = {0x46, 0x30, 8};
    static const uint8_t ONE_POINT[14] = {0x44, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
    tg_bytes split = nef_lossy_table(12, false);
    tg_bytes good = nef_lossy_table(12, false);
    tg_bytes odd = nef_lossy_table(12, false);
    split.d[562] = (uint8_t)(345 >> 8);
    split.d[563] = (uint8_t)(345 & 0xFF);
    CHECK_EQ_INT(parse_only(split.d, split.n, false, 12), PC_ERR_UNSUPPORTED);
    CHECK_EQ_INT(parse_only(V49, sizeof V49, false, 12), PC_ERR_UNSUPPORTED);
    CHECK_EQ_INT(parse_only(good.d, good.n, false, 16), PC_ERR_UNSUPPORTED);
    CHECK_EQ_INT(parse_only(NULL, 0, false, 12), PC_ERR_FORMAT);
    CHECK_EQ_INT(parse_only(SHORT46, sizeof SHORT46, false, 14), PC_ERR_FORMAT);
    CHECK_EQ_INT(parse_only(good.d, 100, false, 12), PC_ERR_FORMAT);
    odd.d[10] = (uint8_t)(300 >> 8);
    odd.d[11] = (uint8_t)(300 & 0xFF);
    CHECK(parse_only(odd.d, odd.n, false, 12) != PC_OK);
    CHECK(parse_only(ONE_POINT, sizeof ONE_POINT, false, 12) != PC_OK);
    tg_free(&split);
    tg_free(&good);
    tg_free(&odd);
}

static tg_entry note_undefined(uint16_t tag, const tg_bytes *b)
{
    tg_entry e;
    e.tag = tag;
    e.val = tg_v_bytes(TG_UNDEFINED, b->d, b->n);
    return e;
}

static tg_entry note_shorts(uint16_t tag, const uint16_t *v, size_t n)
{
    tg_entry e;
    e.tag = tag;
    e.val = tg_v_short(v, n);
    return e;
}

static void notes_free(tg_entry *e, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        tg_val_free(&e[i].val);
    }
}

static bool black_is(const pc_raw_black *b, size_t n, float v)
{
    size_t i;
    if (b->nvalues != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (b->values[i] != v) {
            return false;
        }
    }
    return true;
}

static void test_nefc_decodes_through_the_nef_container(void)
{
    static const uint16_t BLACK[4] = {600, 600, 600, 600};
    static const uint8_t CFA_RGGB[4] = {0, 1, 1, 2};
    static const uint8_t CFA_BGGR[4] = {2, 1, 1, 0};
    static const int32_t SEEDS[2][2] = {{2048, 2048}, {2048, 2048}};
    static const uint16_t ONE[1] = {0};
    size_t w = 40;
    size_t h = 6;
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_format fmt = PC_RAW_DNG;
    pc_raw_sensor s;
    pc_error e;
    pc_status st;
    tg_entry note[2];
    tg_bytes ltab;
    tg_bytes src;
    tg_bytes b;
    tg_bytes table;
    rx_nef_table t;
    int32_t *img;
    size_t i;

    /* lossless 14-bit, big-endian maker note, with a BlackLevel tag */
    img = nef_image(w, h, 16383, 11);
    src = nef_encode(img, w * h, w, &LOSSLESS_14, SEEDS);
    ltab = nef_lossless_table(2048, false);
    note[0] = note_undefined(0x0096, &ltab);
    note[1] = note_shorts(0x003d, BLACK, 4);
    b = tg_nef_compressed(w, h, 14, src.d, src.n, CFA_RGGB, true, note, 2);
    notes_free(note, 2);
    tg_free(&ltab);
    tg_free(&src);
    CHECK(pc_raw_identify(b.d, b.n, &fmt) && fmt == PC_RAW_NEF);
    st = pc_raw_decode(b.d, b.n, &lim, &s, &e);
    CHECK_EQ_INT(st, PC_OK);
    if (st == PC_OK) {
        CHECK(s.ndata == w * h && same16(s.data, img, w * h));
        CHECK(black_is(&s.black, 4, 600.0f));
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(img);

    /* lossy 12-bit type 2, little-endian maker note (newer bodies); the 12-bit BlackLevel is
     * scaled from 14-bit units */
    table = nef_lossy_table(12, true);
    st = rx_nef_parse_table(table.d, table.n, true, 12, &t, &e);
    CHECK_EQ_INT(st, PC_OK);
    if (st != PC_OK || t.curve == NULL) {
        tg_free(&table);
        return;
    }
    img = nef_image(w, h, 1023, 5);
    src = nef_encode(img, w * h, w, &LOSSY_12, (const int32_t(*)[2])t.seeds);
    note[0] = note_undefined(0x0096, &table);
    note[1] = note_shorts(0x003d, BLACK, 4);
    b = tg_nef_compressed(w, h, 12, src.d, src.n, CFA_BGGR, false, note, 2);
    notes_free(note, 2);
    st = pc_raw_decode(b.d, b.n, &lim, &s, &e);
    CHECK_EQ_INT(st, PC_OK);
    if (st == PC_OK) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        bool same = s.ndata == w * h;
        for (i = 0; same && i < w * h; i++) {
            same = s.data[i] == t.curve[img[i]];
        }
        CHECK(same);
        CHECK(black_is(&s.black, 4, 150.0f));
        st = pc_raw_develop_sensor(&s, &o, &d, &e);
        CHECK_EQ_INT(st, PC_OK);
        if (st == PC_OK) {
            pc_raw_developed_fini(&d);
        }
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(img);
    rx_nef_table_fini(&t);

    /* "lossy after split" and a missing table stay unsupported (preview fallback) */
    table.d[562] = 3;
    table.d[563] = 0;
    note[0] = note_undefined(0x0096, &table);
    b = tg_nef_compressed(w, h, 12, src.d, src.n, CFA_BGGR, false, note, 1);
    notes_free(note, 1);
    st = pc_raw_decode(b.d, b.n, &lim, &s, &e);
    CHECK_EQ_INT(st, PC_ERR_UNSUPPORTED);
    if (st == PC_OK) {
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    note[0] = note_shorts(0x0001, ONE, 1);
    b = tg_nef_compressed(w, h, 12, src.d, src.n, CFA_BGGR, false, note, 1);
    notes_free(note, 1);
    st = pc_raw_decode(b.d, b.n, &lim, &s, &e);
    CHECK_EQ_INT(st, PC_ERR_UNSUPPORTED);
    if (st == PC_OK) {
        pc_raw_sensor_fini(&s);
    } else {
        CHECK_CONTAINS(e.msg, "Nikon compressed NEF");
    }
    tg_free(&b);
    tg_free(&src);
    tg_free(&table);
}

static tg_bytes nef_lossless_file(size_t w, size_t h, uint32_t seed)
{
    static const uint8_t CFA_RGGB[4] = {0, 1, 1, 2};
    static const int32_t SEEDS[2][2] = {{2048, 2048}, {2048, 2048}};
    int32_t *img = nef_image(w, h, 16383, seed);
    tg_bytes src = nef_encode(img, w * h, w, &LOSSLESS_14, SEEDS);
    tg_bytes ltab = nef_lossless_table(2048, false);
    tg_entry note = note_undefined(0x0096, &ltab);
    tg_bytes b = tg_nef_compressed(w, h, 14, src.d, src.n, CFA_RGGB, true, &note, 1);
    notes_free(&note, 1);
    tg_free(&ltab);
    tg_free(&src);
    free(img);
    return b;
}

static void test_nefc_nikon_without_black_level_tag_is_black_subtracted(void)
{
    tg_bytes b = nef_lossless_file(32, 8, 4);
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_error e;
    pc_status st = pc_raw_decode(b.d, b.n, &lim, &s, &e);
    CHECK_EQ_INT(st, PC_OK);
    if (st == PC_OK) {
        CHECK(black_is(&s.black, 1, 0.0f));
        CHECK(!pc_raw_strings_any(&s.warnings, "black level"));
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
}

static void test_nefc_truncated_container_never_panics(void)
{
    tg_bytes b = nef_lossless_file(32, 8, 2);
    pc_raw_limits lim = pc_raw_limits_default();
    size_t n;
    for (n = 0; n < b.n; n++) {
        pc_raw_sensor s;
        pc_error e;
        if (pc_raw_decode(b.d, n, &lim, &s, &e) == PC_OK) {
            pc_raw_sensor_fini(&s);
        }
    }
    tg_free(&b);
}

static pc_status nef_decode_only(const uint8_t *src, size_t n, size_t w, size_t h, uint32_t bits,
                                 const rx_nef_table *t)
{
    uint16_t *out = NULL;
    pc_error e;
    pc_status st = rx_nef_decode(src, n, w, h, bits, t, &out, &e);
    if (st == PC_OK) {
        free(out);
    }
    return st;
}

static void test_nefc_truncated_and_corrupt_streams_error_never_panic(void)
{
    static const uint8_t FF[64] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    size_t w = 64;
    size_t h = 16;
    rx_nef_table table = nef_lossless(14);
    rx_nef_table table12 = nef_lossless(12);
    int32_t *img = nef_image(w, h, 16383, 3);
    tg_bytes src = nef_encode(img, w * h, w, &LOSSLESS_14, (const int32_t(*)[2])table.seeds);
    size_t cuts[5];
    uint8_t *bad = tg_xalloc(src.n);
    uint8_t *noise = tg_xalloc(src.n);
    uint32_t s = 12345;
    uint32_t i;
    size_t c;
    tg_bytes lt;
    rx_nef_table lossy;
    pc_error e;
    cuts[0] = 0;
    cuts[1] = 1;
    cuts[2] = 10;
    cuts[3] = src.n / 2;
    cuts[4] = src.n - 40;
    for (c = 0; c < 5; c++) {
        if (nef_decode_only(src.d, cuts[c], w, h, 14, &table) == PC_OK) {
            PC_T_FAIL("cut %zu decoded", cuts[c]);
        }
    }
    for (i = 0; i < 200; i++) {
        uint32_t k;
        size_t q;
        memcpy(bad, src.d, src.n);
        for (k = 0; k < 1 + i % 8; k++) {
            size_t at;
            s = s * 1103515245u + 12345u;
            at = (size_t)(s >> 8) % src.n;
            bad[at] = (uint8_t)(bad[at] ^ (1u << (s % 8)));
        }
        (void)nef_decode_only(bad, src.n, w, h, 14, &table);
        for (q = 0; q < src.n; q++) {
            noise[q] = (uint8_t)(((uint32_t)q * 2654435761u) >> (i % 24));
        }
        (void)nef_decode_only(noise, src.n, w, h, 14, &table);
        (void)nef_decode_only(noise, src.n, w, h, 12, &table12);
    }
    lt = nef_lossy_table(12, false);
    CHECK_EQ_INT(rx_nef_parse_table(lt.d, lt.n, false, 12, &lossy, &e), PC_OK);
    CHECK(nef_decode_only(FF, sizeof FF, 8, 8, 12, &lossy) != PC_OK);
    rx_nef_table_fini(&lossy);
    tg_free(&lt);
    CHECK(nef_decode_only(src.d, src.n, SIZE_MAX, 2, 14, &table) != PC_OK);
    CHECK(nef_decode_only(src.d, src.n, 0, 2, 14, &table) != PC_OK);
    free(bad);
    free(noise);
    tg_free(&src);
    free(img);
}

/* ======================================================================== opcodes.rs */

typedef struct op {
    uint32_t id;
    tg_bytes params;
} op;

static tg_bytes opcode_list(const op *ops, size_t n)
{
    tg_bytes b = {NULL, 0, 0};
    size_t i;
    tg_be32(&b, (uint32_t)n);
    for (i = 0; i < n; i++) {
        tg_be32(&b, ops[i].id);
        tg_be32(&b, 0x01030000u);
        tg_be32(&b, 1);
        tg_be32(&b, (uint32_t)ops[i].params.n);
        tg_put(&b, ops[i].params.d, ops[i].params.n);
    }
    return b;
}

static tg_bytes gain_map_params(const uint32_t area[4], uint32_t pitch, const float gains[2][2])
{
    tg_bytes p = {NULL, 0, 0};
    const uint32_t head[10] = {area[0], area[1], area[2], area[3], 0, 1, pitch, pitch, 2, 2};
    static const double DV[4] = {1.0, 1.0, 0.0, 0.0};
    int i;
    for (i = 0; i < 10; i++) {
        tg_be32(&p, head[i]);
    }
    for (i = 0; i < 4; i++) {
        uint64_t u;
        memcpy(&u, &DV[i], 8);
        tg_be32(&p, (uint32_t)(u >> 32));
        tg_be32(&p, (uint32_t)u);
    }
    tg_be32(&p, 1);
    for (i = 0; i < 4; i++) {
        uint32_t u;
        memcpy(&u, &gains[i / 2][i % 2], 4);
        tg_be32(&p, u);
    }
    return p;
}

static void free_maps(pc_raw_gain_map *maps, size_t n, pc_raw_strings *skipped)
{
    size_t i;
    for (i = 0; i < n; i++) {
        rx_gain_map_fini(&maps[i]);
    }
    free(maps);
    pc_raw_strings_fini(skipped);
}

static void test_opcodes_parses_gain_maps_and_names_others(void)
{
    static const uint32_t AREA[4] = {0, 0, 10, 10};
    static const float G[2][2] = {{1.0f, 2.0f}, {3.0f, 4.0f}};
    static const uint8_t ZEROS[8] = {0};
    op ops[2];
    tg_bytes b;
    pc_raw_gain_map *maps = NULL;
    size_t nmaps = 0;
    pc_raw_strings skipped = {NULL, 0};
    pc_error e;
    ops[0].id = 9;
    ops[0].params = gain_map_params(AREA, 1, G);
    ops[1].id = 1;
    memset(&ops[1].params, 0, sizeof ops[1].params);
    tg_put(&ops[1].params, ZEROS, 8);
    b = opcode_list(ops, 2);
    tg_free(&ops[0].params);
    tg_free(&ops[1].params);
    CHECK_EQ_INT(rx_opcodes_parse(b.d, b.n, &maps, &nmaps, &skipped, &e), PC_OK);
    CHECK_EQ_UINT(nmaps, 1);
    CHECK_EQ_UINT(skipped.n, 1);
    if (skipped.n == 1) {
        CHECK_EQ_STR(skipped.v[0], "WarpRectilinear");
    }
    if (nmaps == 1) {
        const pc_raw_gain_map *m = &maps[0];
        float row[2];
        CHECK_EQ_UINT(m->points_h, 2);
        /* Bilinear: centre of the 2x2 grid. */
        rx_gain_map_row(m, 0.5, 0, row);
        CHECK(fabsf(rx_gain_map_at(m, row, 0.5) - 2.5f) < 1e-6f);
        rx_gain_map_row(m, 0.0, 0, row);
        CHECK(fabsf(rx_gain_map_at(m, row, 0.0) - 1.0f) < 1e-6f);
        rx_gain_map_row(m, 1.0, 0, row);
        CHECK(fabsf(rx_gain_map_at(m, row, 1.0) - 4.0f) < 1e-6f);
        /* Outside the grid clamps. */
        rx_gain_map_row(m, -3.0, 0, row);
        CHECK(fabsf(rx_gain_map_at(m, row, 9.0) - 2.0f) < 1e-6f);
    }
    free_maps(maps, nmaps, &skipped);
    tg_free(&b);
}

static void test_opcodes_hostile_lists_do_not_panic(void)
{
    static const uint32_t AREA[4] = {0, 0, 10, 10};
    static const float G[2][2] = {{1.0f, 2.0f}, {3.0f, 4.0f}};
    op o;
    tg_bytes b;
    size_t n;
    pc_raw_gain_map *maps = NULL;
    size_t nmaps = 0;
    pc_raw_strings skipped = {NULL, 0};
    pc_error e;
    o.id = 9;
    o.params = gain_map_params(AREA, 2, G);
    b = opcode_list(&o, 1);
    tg_free(&o.params);
    for (n = 0; n < b.n; n++) {
        if (rx_opcodes_parse(b.d, n, &maps, &nmaps, &skipped, &e) == PC_OK) {
            free_maps(maps, nmaps, &skipped);
        }
    }
    memset(b.d + 4 + 16 + 32, 0xFF, 8); /* points */
    CHECK_EQ_INT(rx_opcodes_parse(b.d, b.n, &maps, &nmaps, &skipped, &e), PC_OK);
    CHECK_EQ_UINT(nmaps, 0);
    free_maps(maps, nmaps, &skipped);
    tg_free(&b);
}

/* ======================================================================== rw2.rs */

static void test_rw2_pages_are_split(void)
{
    CHECK_EQ_UINT(rx_rw2_physical(0), 0x1FF8);
    CHECK_EQ_UINT(rx_rw2_physical(7), 0x1FFF);
    CHECK_EQ_UINT(rx_rw2_physical(8), 0x2000);
    CHECK_EQ_UINT(rx_rw2_physical(0x2007), 0x3FFF);
    CHECK_EQ_UINT(rx_rw2_physical(0x2008), 0);
    CHECK_EQ_UINT(rx_rw2_physical(0x3FFF), 0x1FF7);
    CHECK_EQ_UINT(rx_rw2_physical(0x4000), 0x5FF8);
}

static void test_rw2_unpacks_lsb_first(void)
{
    /* The 128-bit little-endian value with (0x100 + i) at bit 12 i. */
    uint8_t b[16] = {0};
    uint8_t ff[16];
    uint16_t out[12] = {0};
    uint32_t i;
    for (i = 0; i < 10; i++) {
        uint32_t v = 0x100 + i;
        uint32_t k;
        for (k = 0; k < 12; k++) {
            if ((v >> k) & 1u) {
                uint32_t at = 12 * i + k;
                b[at / 8] = (uint8_t)(b[at / 8] | (1u << (at % 8)));
            }
        }
    }
    rx_rw2_unpack_block(b, 12, out, 10);
    for (i = 0; i < 10; i++) {
        CHECK_EQ_UINT(out[i], 0x100 + i);
    }
    memset(ff, 0xFF, sizeof ff);
    memset(out, 0, sizeof out);
    rx_rw2_unpack_block(ff, 14, out, 12);
    for (i = 0; i < 9; i++) {
        CHECK_EQ_UINT(out[i], 0x3FFF);
    }
    for (i = 9; i < 12; i++) {
        CHECK_EQ_UINT(out[i], 0);
    }
}

/* ======================================================================== sony.rs */

static void test_sony_tone_curve_matches_observed_anchors(void)
{
    /* Knots written by an ILCE-7M3. */
    static const uint32_t KNOTS[4] = {8000, 10400, 12900, 14100};
    static const uint32_t BAD1[4] = {9000, 8000, 12000, 13000};
    static const uint32_t BAD2[3] = {1, 2, 3};
    static const uint32_t BAD3[4] = {0, 0, 0, 20000};
    uint16_t lut[2048];
    size_t i;
    bool mono = true;
    CHECK(rx_sony_tone_lut(KNOTS, 4, lut));
    CHECK_EQ_UINT(lut[0], 0);
    CHECK_EQ_UINT(lut[256], 512); /* black */
    CHECK_EQ_UINT(lut[999], 1998);
    /* Steps double at each knot. */
    CHECK_EQ_INT((int)lut[1001] - (int)lut[1000], 2 * ((int)lut[1000] - (int)lut[999]));
    CHECK(lut[2021] >= 16300); /* clipping code ~ white */
    CHECK_EQ_UINT(lut[2047], 16383);
    for (i = 0; i + 1 < 2048; i++) {
        mono = mono && lut[i] <= lut[i + 1];
    }
    CHECK(mono);
    CHECK(!rx_sony_tone_lut(BAD1, 4, lut));
    CHECK(!rx_sony_tone_lut(BAD2, 3, lut));
    CHECK(!rx_sony_tone_lut(BAD3, 4, lut));
}

static void test_sony_hostile_blocks_do_not_panic(void)
{
    static const uint8_t FILLS[3] = {0xFF, 0x00, 0x55};
    size_t i;
    for (i = 0; i < 3; i++) {
        uint8_t b[16];
        uint16_t out[16];
        memset(b, FILLS[i], sizeof b);
        rx_sony_decode_block(b, out);
    }
    CHECK(true);
}

/* ======================================================================== demosaic.rs */

static rx_padded padded_from_plane(const float *plane, size_t w, size_t h)
{
    rx_padded p;
    pc_error e;
    size_t y;
    memset(&p, 0, sizeof p);
    if (rx_padded_new(w, h, &p, &e) != PC_OK) {
        PC_T_FAIL("rx_padded_new %zux%zu", w, h);
        return p;
    }
    for (y = 0; y < h; y++) {
        memcpy(p.data + (y + RX_PAD) * p.stride + RX_PAD, plane + y * w, w * sizeof *plane);
    }
    rx_padded_fill_borders(&p);
    return p;
}

/* Mosaics an RGB image with the given phase. */
static float *mosaic(const float (*rgb)[3], size_t w, size_t h, const uint8_t phase[4])
{
    float *v = tg_xalloc((w * h > 0 ? w * h : 1) * sizeof *v);
    size_t i;
    for (i = 0; i < w * h; i++) {
        v[i] = rgb[i][phase[((i / w) & 1) * 2 + ((i % w) & 1)]];
    }
    return v;
}

static float *demosaic_run(const float *cfa, size_t w, size_t h, const uint8_t phase[4], pc_raw_demosaic m)
{
    rx_padded p = padded_from_plane(cfa, w, h);
    float *out = NULL;
    pc_error e;
    pc_status st = rx_demosaic(&p, phase, m, &RX_IDENTITY, &out, &e);
    CHECK_EQ_INT(st, PC_OK);
    rx_padded_fini(&p);
    return st == PC_OK ? out : NULL;
}

static double psnr(const float *a, const float (*b)[3], size_t w, size_t h, size_t border)
{
    double se = 0.0;
    size_t n = 0;
    size_t y;
    for (y = border; y < h - border; y++) {
        size_t x;
        for (x = border; x < w - border; x++) {
            int c;
            for (c = 0; c < 3; c++) {
                double d = (double)(a[(y * w + x) * 3 + (size_t)c] - b[y * w + x][c]);
                se += d * d;
                n++;
            }
        }
    }
    {
        double mse = se / (double)n;
        return 10.0 * log10(1.0 / (mse > 1e-12 ? mse : 1e-12));
    }
}

static const pc_raw_demosaic METHODS[3] = {PC_RAW_BILINEAR, PC_RAW_MHC, PC_RAW_AHD};

static void test_demosaic_padding_reflects(void)
{
    static const float PLANE[6] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    static const float ROW0[7] = {3.0f, 2.0f, 1.0f, 2.0f, 3.0f, 2.0f, 1.0f};
    static const float ROWM1[3] = {4.0f, 5.0f, 6.0f};
    rx_padded p = padded_from_plane(PLANE, 3, 2);
    size_t r = RX_PAD - 1;
    if (p.data == NULL) {
        return;
    }
    CHECK_EQ_MEM(p.data + RX_PAD * p.stride + RX_PAD - 2, ROW0, sizeof ROW0);
    /* Row -1 mirrors row 1. */
    CHECK_EQ_MEM(p.data + r * p.stride + RX_PAD, ROWM1, sizeof ROWM1);
    rx_padded_fini(&p);
}

static void test_demosaic_flat_colour_is_exact(void)
{
    static const uint8_t PHASES[4][4] = {{0, 1, 1, 2}, {2, 1, 1, 0}, {1, 0, 2, 1}, {1, 2, 0, 1}};
    size_t w = 16;
    size_t h = 12;
    float(*rgb)[3] = tg_xalloc(w * h * sizeof *rgb);
    size_t i;
    size_t ph;
    for (i = 0; i < w * h; i++) {
        rgb[i][0] = 0.2f;
        rgb[i][1] = 0.5f;
        rgb[i][2] = 0.8f;
    }
    for (ph = 0; ph < 4; ph++) {
        float *cfa = mosaic((const float(*)[3])rgb, w, h, PHASES[ph]);
        size_t m;
        for (m = 0; m < 3; m++) {
            float *out = demosaic_run(cfa, w, h, PHASES[ph], METHODS[m]);
            int fails = 0;
            if (out == NULL) {
                continue;
            }
            for (i = 0; i < w * h && fails < 3; i++) {
                int c;
                for (c = 0; c < 3; c++) {
                    if (!(fabsf(out[i * 3 + (size_t)c] - rgb[i][c]) < 1e-5f)) {
                        PC_T_FAIL("%s phase %zu px %zu c %d: %g", pc_raw_demosaic_id(METHODS[m]), ph, i, c,
                                  (double)out[i * 3 + (size_t)c]);
                        fails++;
                    }
                }
            }
            CHECK(fails == 0);
            free(out);
        }
        free(cfa);
    }
    free(rgb);
}

static void test_demosaic_smooth_gradient_quality(void)
{
    /* A smooth colour field: every method should be accurate. */
    static const uint8_t PHASE[4] = {0, 1, 1, 2};
    size_t w = 64;
    size_t h = 48;
    float(*rgb)[3] = tg_xalloc(w * h * sizeof *rgb);
    float *cfa;
    size_t i;
    size_t m;
    for (i = 0; i < w * h; i++) {
        float x = (float)(i % w) / (float)w;
        float y = (float)(i / w) / (float)h;
        rgb[i][0] = 0.2f + 0.6f * x;
        rgb[i][1] = 0.3f + 0.4f * (x * y);
        rgb[i][2] = 0.8f - 0.5f * y;
    }
    cfa = mosaic((const float(*)[3])rgb, w, h, PHASE);
    for (m = 0; m < 3; m++) {
        float *out = demosaic_run(cfa, w, h, PHASE, METHODS[m]);
        if (out != NULL) {
            double p = psnr(out, (const float(*)[3])rgb, w, h, 2);
            if (!(p > 40.0)) {
                PC_T_FAIL("%s: %.1f dB", pc_raw_demosaic_id(METHODS[m]), p);
            }
            CHECK(p > 40.0);
            free(out);
        }
    }
    free(cfa);
    free(rgb);
}

static void test_demosaic_edges_ahd_beats_bilinear(void)
{
    /* Vertical and horizontal black/white stripes of width 3: AHD's directional choice must
     * reduce error versus bilinear. */
    static const uint8_t PHASE[4] = {0, 1, 1, 2};
    size_t w = 48;
    size_t h = 48;
    float(*rgb)[3] = tg_xalloc(w * h * sizeof *rgb);
    float *cfa;
    double ps[3] = {0.0, 0.0, 0.0};
    size_t i;
    size_t m;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w;
        size_t y = i / w;
        float v = y < h / 2 ? (float)((x / 3) % 2) : (float)((y / 3) % 2);
        rgb[i][0] = rgb[i][1] = rgb[i][2] = 0.1f + 0.8f * v;
    }
    cfa = mosaic((const float(*)[3])rgb, w, h, PHASE);
    for (m = 0; m < 3; m++) {
        float *out = demosaic_run(cfa, w, h, PHASE, METHODS[m]);
        if (out != NULL) {
            ps[m] = psnr(out, (const float(*)[3])rgb, w, h, 3);
            free(out);
        }
    }
    if (!(ps[2] > ps[0] + 3.0)) {
        PC_T_FAIL("ahd %.1f dB vs bilinear %.1f dB", ps[2], ps[0]);
    }
    CHECK(ps[2] > ps[0] + 3.0);
    if (!(ps[1] > ps[0])) {
        PC_T_FAIL("mhc %.1f dB vs bilinear %.1f dB", ps[1], ps[0]);
    }
    CHECK(ps[1] > ps[0]);
    free(cfa);
    free(rgb);
}

static void test_demosaic_bilinear_fast_path_matches_kernels(void)
{
    /* The direct bilinear code must equal the generic kernel evaluation. */
    static const uint8_t PHASES[2][4] = {{0, 1, 1, 2}, {1, 2, 0, 1}};
    size_t w = 20;
    size_t h = 14;
    float *cfa = tg_xalloc(w * h * sizeof *cfa);
    size_t i;
    size_t ph;
    for (i = 0; i < w * h; i++) {
        cfa[i] = (float)((i * 7919) % 1000) / 1000.0f;
    }
    for (ph = 0; ph < 2; ph++) {
        rx_padded p = padded_from_plane(cfa, w, h);
        float *fast = NULL;
        pc_error e;
        size_t y;
        int fails = 0;
        if (p.data == NULL || rx_demosaic(&p, PHASES[ph], PC_RAW_BILINEAR, &RX_IDENTITY, &fast, &e) != PC_OK) {
            PC_T_FAIL("demosaic");
            rx_padded_fini(&p);
            continue;
        }
        for (y = 0; y < h; y++) {
            size_t x;
            for (x = 0; x < w; x++) {
                size_t at = (y + RX_PAD) * p.stride + RX_PAD + x;
                size_t c;
                for (c = 0; c < 3; c++) {
                    ptrdiff_t offs[11];
                    float wts[11];
                    size_t nt = rx_demosaic_kernel(PHASES[ph], PC_RAW_BILINEAR, p.stride, (y & 1) * 2 + (x & 1), c,
                                                   offs, wts);
                    float v = 0.0f;
                    size_t k;
                    for (k = 0; k < nt; k++) {
                        v += wts[k] * p.data[(size_t)((ptrdiff_t)at + offs[k])];
                    }
                    if (!(fabsf(fast[(y * w + x) * 3 + c] - v) < 1e-6f)) {
                        PC_T_FAIL("(%zu,%zu) c%zu", x, y, c);
                        fails++;
                    }
                }
            }
        }
        CHECK(fails == 0);
        free(fast);
        rx_padded_fini(&p);
    }
    free(cfa);
}

static void test_demosaic_lab_table_is_accurate(void)
{
    float *table = tg_xalloc(RX_LABF_ENTRIES * sizeof *table);
    int i;
    int fails = 0;
    rx_labf_table(table);
    for (i = 0; i < 10000; i++) {
        float t = (float)i / 4000.0f;
        if (!(fabsf(rx_labf(table, t) - rx_labf_exact(t)) < 2e-4f)) {
            PC_T_FAIL("%g", (double)t);
            fails++;
        }
    }
    CHECK(fails == 0);
    free(table);
}

static void test_demosaic_tiny_planes_do_not_panic(void)
{
    static const size_t SIZES[7][2] = {{1, 1}, {2, 1}, {1, 3}, {2, 2}, {3, 3}, {7, 9}, {40, 1}};
    static const uint8_t PHASE[4] = {0, 1, 1, 2};
    size_t k;
    for (k = 0; k < 7; k++) {
        size_t w = SIZES[k][0];
        size_t h = SIZES[k][1];
        float *cfa = tg_xalloc(w * h * sizeof *cfa);
        size_t i;
        size_t m;
        for (i = 0; i < w * h; i++) {
            cfa[i] = 0.5f;
        }
        for (m = 0; m < 3; m++) {
            /* The output buffer holds w * h * 3 values (ASan checks the reads). */
            float *out = demosaic_run(cfa, w, h, PHASE, METHODS[m]);
            CHECK(out != NULL);
            if (out != NULL) {
                volatile float sink = out[w * h * 3 - 1];
                (void)sink;
                free(out);
            }
        }
        free(cfa);
    }
}

/* ======================================================================== develop.rs */

static void test_develop_gamma_table_matches_powf(void)
{
    float *lut = tg_xalloc((RX_GAMMA_STEPS + 2) * sizeof *lut);
    int i;
    int fails = 0;
    rx_gamma_table(NULL, 0, lut);
    for (i = 0; i <= 100000; i++) {
        float v = (float)i / 100000.0f;
        float exact = powf(v, 1.0f / 1.8f) * 65535.0f;
        uint16_t got = rx_gamma_encode(lut, v);
        if (!(fabsf((float)got - exact) <= 1.0f)) {
            PC_T_FAIL("%g: %u vs %g", (double)v, (unsigned)got, (double)exact);
            fails++;
            if (fails > 5) {
                break;
            }
        }
    }
    CHECK(fails == 0);
    CHECK_EQ_UINT(rx_gamma_encode(lut, -1.0f), 0);
    CHECK_EQ_UINT(rx_gamma_encode(lut, 2.0f), 65535);
    CHECK_EQ_UINT(rx_gamma_encode(lut, NAN), 0);
    free(lut);
}

static float gamma_code(float v) { return powf(v, 1.0f / 1.8f) * 65535.0f; }
static bool near_code(uint16_t got, float want) { return fabsf((float)got - want) <= 2.0f; }

static void test_develop_tone_curve_is_folded_into_the_gamma_table(void)
{
    static const float CURVE[2][2] = {{0.25f, 0.5f}, {1.0f, 1.0f}};
    float *lut = tg_xalloc((RX_GAMMA_STEPS + 2) * sizeof *lut);
    int i;
    bool mono = true;
    uint16_t prev = 0;
    rx_gamma_table(CURVE, 2, lut);
    /* On a point, between points (linear in log2 of the input), on the line through 0 below the
     * first. */
    CHECK(near_code(rx_gamma_encode(lut, 0.25f), gamma_code(0.5f)));
    CHECK(near_code(rx_gamma_encode(lut, 0.5f), gamma_code(0.75f)));
    CHECK(near_code(rx_gamma_encode(lut, 0.125f), gamma_code(0.25f)));
    CHECK_EQ_UINT(rx_gamma_encode(lut, 0.0f), 0);
    CHECK_EQ_UINT(rx_gamma_encode(lut, 1.0f), 65535);
    for (i = 0; i < 1000; i++) {
        uint16_t c = rx_gamma_encode(lut, (float)i / 999.0f);
        if (i > 0) {
            mono = mono && c >= prev;
        }
        prev = c;
    }
    CHECK(mono);
    /* No curve: the plain gamma 1.8. */
    rx_gamma_table(NULL, 0, lut);
    CHECK(near_code(rx_gamma_encode(lut, 0.25f), gamma_code(0.25f)));
    free(lut);
}

/* Orients the 3x2 test image (pixel value = index); writes the first channel of each pixel. */
static void orient_px(uint16_t o, uint16_t px[6], size_t *ow, size_t *oh)
{
    uint16_t src[18];
    uint16_t *out = NULL;
    const uint16_t *v;
    pc_error e;
    size_t i;
    for (i = 0; i < 18; i++) {
        src[i] = (uint16_t)(i / 3);
    }
    CHECK_EQ_INT(rx_orient(src, 3, 2, o, &out, ow, oh, &e), PC_OK);
    v = out != NULL ? out : src;
    for (i = 0; i < 6; i++) {
        px[i] = v[i * 3];
    }
    free(out);
}

static void check_px(uint16_t o, const uint16_t want[6])
{
    uint16_t px[6];
    size_t ow;
    size_t oh;
    orient_px(o, px, &ow, &oh);
    if (memcmp(px, want, sizeof px) != 0) {
        PC_T_FAIL("orientation %u: %u %u %u %u %u %u", (unsigned)o, px[0], px[1], px[2], px[3], px[4], px[5]);
    }
    CHECK(true);
}

static void test_develop_orientations(void)
{
    static const uint16_t O1[6] = {0, 1, 2, 3, 4, 5};
    static const uint16_t O2[6] = {2, 1, 0, 5, 4, 3};
    static const uint16_t O3[6] = {5, 4, 3, 2, 1, 0};
    static const uint16_t O4[6] = {3, 4, 5, 0, 1, 2};
    static const uint16_t O6[6] = {3, 0, 4, 1, 5, 2};
    static const uint16_t O8[6] = {2, 5, 1, 4, 0, 3};
    static const uint16_t O5[6] = {0, 3, 1, 4, 2, 5};
    static const uint16_t O7[6] = {5, 2, 4, 1, 3, 0};
    uint16_t px[6];
    size_t ow;
    size_t oh;
    check_px(1, O1);
    check_px(2, O2);
    check_px(3, O3);
    check_px(4, O4);
    orient_px(6, px, &ow, &oh);
    CHECK(ow == 2 && oh == 3);
    /* Rotated 90 degrees clockwise: the left column (0, 3) becomes the top row (3, 0). */
    CHECK_EQ_MEM(px, O6, sizeof px);
    check_px(8, O8);
    check_px(5, O5);
    check_px(7, O7);
}

int main(int argc, char **argv)
{
    /* tiff.rs */
    RUN(test_tiff_header_variants);
    RUN(test_tiff_self_referencing_ifds_terminate);
    RUN(test_tiff_rational_with_zero_denominator_is_none);
    /* sensor.rs */
    RUN(test_sensor_bayer_patterns);
    RUN(test_sensor_unpack_12_bit);
    /* color.rs */
    RUN(test_color_inverse_roundtrip);
    RUN(test_color_white_points_map_to_d50);
    RUN(test_color_mccamy_reference_points);
    RUN(test_color_dng_color_matrix_roundtrip);
    /* cameras.rs */
    RUN(test_cameras_a_d4_nef_gets_its_crop_levels_matrix_and_curve);
    RUN(test_cameras_other_cameras_and_sizes_are_left_alone);
    RUN(test_cameras_d4_forward_matrix_maps_neutral_to_d50_and_curve_reaches_white);
    /* tiffep.rs */
    RUN(test_tiffep_trailing_padding_is_trimmed);
    RUN(test_tiffep_bright_or_varied_edges_and_bad_input_are_kept);
    /* cr2.rs */
    RUN(test_cr2_red_row_measurement_never_panics_on_odd_geometry);
    RUN(test_cr2_red_row_measurement_finds_both_phases);
    RUN(test_cr2_color_data_wb_as_words);
    RUN(test_cr2_short_color_data_reads_no_wb);
    /* ljpeg.rs */
    RUN(test_ljpeg_roundtrip_all_predictors_and_precisions);
    RUN(test_ljpeg_roundtrip_with_restarts_and_extreme_values);
    RUN(test_ljpeg_hostile_streams_error_without_panicking);
    /* nefc.rs */
    RUN(test_nefc_tables_are_prefix_codes);
    RUN(test_nefc_lossless_round_trip_12_and_14);
    RUN(test_nefc_lossy_type2_curve_and_round_trip);
    RUN(test_nefc_lossy_type1_full_curve);
    RUN(test_nefc_unsupported_and_malformed_tables);
    RUN(test_nefc_decodes_through_the_nef_container);
    RUN(test_nefc_nikon_without_black_level_tag_is_black_subtracted);
    RUN(test_nefc_truncated_container_never_panics);
    RUN(test_nefc_truncated_and_corrupt_streams_error_never_panic);
    /* opcodes.rs */
    RUN(test_opcodes_parses_gain_maps_and_names_others);
    RUN(test_opcodes_hostile_lists_do_not_panic);
    /* rw2.rs */
    RUN(test_rw2_pages_are_split);
    RUN(test_rw2_unpacks_lsb_first);
    /* sony.rs */
    RUN(test_sony_tone_curve_matches_observed_anchors);
    RUN(test_sony_hostile_blocks_do_not_panic);
    /* demosaic.rs */
    RUN(test_demosaic_padding_reflects);
    RUN(test_demosaic_flat_colour_is_exact);
    RUN(test_demosaic_smooth_gradient_quality);
    RUN(test_demosaic_edges_ahd_beats_bilinear);
    RUN(test_demosaic_bilinear_fast_path_matches_kernels);
    RUN(test_demosaic_lab_table_is_accurate);
    RUN(test_demosaic_tiny_planes_do_not_panic);
    /* develop.rs */
    RUN(test_develop_gamma_table_matches_powf);
    RUN(test_develop_tone_curve_is_folded_into_the_gamma_table);
    RUN(test_develop_orientations);
    return pc_t_finish();
}
