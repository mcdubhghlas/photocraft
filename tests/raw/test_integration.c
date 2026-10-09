/* Integration tests of the camera-raw decoder on synthetic files: a port of the reference's
 * crates/raw/tests/{dng,hostile,vendor_codecs,vendors}.rs. Every synthetic file is built with
 * tests/raw/testgen.h (the port of crates/raw/src/testgen.rs). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "raw/raw.h"
#include "testgen.h"

/* ---- shared helpers ---- */

static const uint8_t RGGB[4] = {0, 1, 1, 2};

/* Decodes with the default limits; reports the error and returns false on failure. */
static bool decode_ok(const uint8_t *b, size_t n, pc_raw_sensor *s)
{
    pc_raw_limits lim = pc_raw_limits_default();
    pc_error e;
    if (pc_raw_decode(b, n, &lim, s, &e) != PC_OK) {
        PC_T_FAIL("decode failed: %s", e.msg);
        return false;
    }
    return true;
}

/* Decodes with the given limits and returns the status (the sensor is freed on success). */
static pc_status decode_status(const uint8_t *b, size_t n, const pc_raw_limits *lim, pc_error *e)
{
    pc_raw_sensor s;
    pc_status st = pc_raw_decode(b, n, lim, &s, e);
    if (st == PC_OK) {
        pc_raw_sensor_fini(&s);
    }
    return st;
}

static pc_status decode_default_status(const uint8_t *b, size_t n, pc_error *e)
{
    pc_raw_limits lim = pc_raw_limits_default();
    return decode_status(b, n, &lim, e);
}

static bool develop_ok(const uint8_t *b, size_t n, const pc_raw_develop_options *o, pc_raw_developed *d)
{
    pc_error e;
    if (pc_raw_develop(b, n, o, d, &e) != PC_OK) {
        PC_T_FAIL("develop failed: %s", e.msg);
        return false;
    }
    return true;
}

static bool develop_sensor_ok(const pc_raw_sensor *s, const pc_raw_develop_options *o, pc_raw_developed *d)
{
    pc_error e;
    if (pc_raw_develop_sensor(s, o, d, &e) != PC_OK) {
        PC_T_FAIL("develop_sensor failed: %s", e.msg);
        return false;
    }
    return true;
}

static bool phase_is(const pc_raw_sensor *s, size_t x, size_t y, const uint8_t want[4])
{
    uint8_t p[4];
    if (!s->has_cfa) {
        return false;
    }
    pc_raw_cfa_phase(&s->cfa, x, y, p);
    return memcmp(p, want, 4) == 0;
}

static bool black_values_are(const pc_raw_sensor *s, const float *want, size_t n)
{
    size_t i;
    if (s->black.nvalues != n) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (s->black.values[i] != want[i]) {
            return false;
        }
    }
    return true;
}

static bool white_is(const pc_raw_sensor *s, float v)
{
    return s->white[0] == v && s->white[1] == v && s->white[2] == v;
}

static bool camera_wb_is(const pc_raw_sensor *s, double r, double g, double b)
{
    return s->has_camera_wb && s->camera_wb[0] == r && s->camera_wb[1] == g && s->camera_wb[2] == b;
}

static bool data_is(const pc_raw_sensor *s, const uint16_t *want, size_t n)
{
    return s->ndata == n && memcmp(s->data, want, n * sizeof *want) == 0;
}

static void dump_warnings(const pc_raw_strings *w)
{
    size_t i;
    for (i = 0; i < w->n; i++) {
        (void)fprintf(stderr, "    warning: %s\n", w->v[i]);
    }
}

/* Some warning contains both needles. */
static bool any_warning2(const pc_raw_strings *w, const char *a, const char *b)
{
    size_t i;
    for (i = 0; i < w->n; i++) {
        if (strstr(w->v[i], a) != NULL && strstr(w->v[i], b) != NULL) {
            return true;
        }
    }
    return false;
}

/* An exact-size heap copy of b[0..n] (so ASan sees reads past the end). */
static uint8_t *copy_exact(const uint8_t *b, size_t n)
{
    uint8_t *c = tg_xalloc(n);
    if (n > 0) {
        memcpy(c, b, n);
    }
    return c;
}

static uint16_t *fill16(size_t n, uint16_t v)
{
    uint16_t *d = tg_xalloc(n * sizeof *d);
    size_t i;
    for (i = 0; i < n; i++) {
        d[i] = v;
    }
    return d;
}

static uint16_t *mosaic_of(const float (*rgb)[3], size_t n, size_t w, const uint8_t cfa[4], uint16_t black,
                           uint16_t white)
{
    return tg_mosaic(rgb, n, w, cfa, black, white);
}

static uint16_t *scene_mosaic(size_t w, size_t h, const uint8_t cfa[4], uint16_t black, uint16_t white)
{
    float(*sc)[3] = tg_scene(w, h);
    uint16_t *m = mosaic_of((const float(*)[3])sc, w * h, w, cfa, black, white);
    free(sc);
    return m;
}

static double lin(uint16_t v) { return pow((double)v / 65535.0, 1.8); }

/* ======================================================================================
 * dng.rs: synthetic DNG files decode to the exact sensor samples, and development
 * reproduces known colours.
 * ====================================================================================== */

/* Deterministic sensor-like samples below 2^bits. */
static uint16_t *noise(size_t n, uint16_t bits)
{
    uint64_t s = 0x9E3779B97F4A7C15ull;
    uint32_t max = (1u << bits) - 1;
    uint16_t *v = tg_xalloc(n * sizeof *v);
    size_t i;
    for (i = 0; i < n; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        v[i] = (uint16_t)(((uint32_t)i * 13 + (uint32_t)(s >> 40) % 200) % (max + 1));
    }
    return v;
}

static void test_identifies_dng(void)
{
    uint16_t *z = fill16(16, 0);
    tg_dng spec = tg_dng_cfa(4, 4, z);
    tg_bytes b = tg_dng_build(&spec);
    pc_raw_format f;
    static const uint8_t TIFF[10] = {'I', 'I', '*', 0, 0x08, 0, 0, 0, 0, 0};
    static const uint8_t PNG[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    CHECK(pc_raw_identify(b.d, b.n, &f) && f == PC_RAW_DNG);
    CHECK(pc_raw_is_raw(b.d, b.n));
    CHECK(!pc_raw_is_raw(TIFF, sizeof TIFF));
    CHECK(!pc_raw_is_raw(PNG, sizeof PNG));
    tg_free(&b);
    free(z);
}

static void test_storage_layouts_decode_exactly(void)
{
    size_t w = 37; /* odd sizes exercise partial strips and edge tiles */
    size_t h = 23;
    static const struct {
        const char *name;
        uint16_t bits;
        tg_storage st;
        size_t a;
        size_t b;
        bool be;
    } CASES[8] = {
        {"16-bit strips", 16, TG_STRIPS, 5, 0, false},
        {"16-bit big-endian", 16, TG_STRIPS, 23, 0, true},
        {"12-bit packed", 12, TG_STRIPS, 4, 0, false},
        {"10-bit packed big-endian", 10, TG_STRIPS, 7, 0, true},
        {"8-bit", 8, TG_STRIPS, 9, 0, false},
        {"LJ92 tiles", 14, TG_LJ92_TILES, 16, 8, false},
        {"LJ92 tiles big-endian", 16, TG_LJ92_TILES, 32, 32, true},
        {"LJ92 strips", 12, TG_LJ92_STRIPS, 6, 0, false},
    };
    size_t k;
    for (k = 0; k < 8; k++) {
        uint16_t *data = noise(w * h, CASES[k].bits);
        tg_dng spec = tg_dng_cfa(w, h, data);
        tg_bytes b;
        pc_raw_sensor s;
        spec.bits = CASES[k].bits;
        spec.storage = CASES[k].st;
        if (CASES[k].st == TG_LJ92_TILES) {
            spec.tile_w = CASES[k].a;
            spec.tile_h = CASES[k].b;
        } else {
            spec.rows = CASES[k].a;
        }
        spec.big_endian = CASES[k].be;
        spec.white = (1u << CASES[k].bits) - 1;
        b = tg_dng_build(&spec);
        if (decode_ok(b.d, b.n, &s)) {
            if (!(s.width == w && s.height == h && s.samples == 1)) {
                PC_T_FAIL("%s: %zux%zu x%zu", CASES[k].name, s.width, s.height, s.samples);
            }
            if (!data_is(&s, data, w * h)) {
                PC_T_FAIL("%s: data differs", CASES[k].name);
            }
            pc_t_checks += 2;
            CHECK(s.format == PC_RAW_DNG);
            pc_raw_sensor_fini(&s);
        }
        tg_free(&b);
        free(data);
    }
}

static void test_linear_raw_decodes_and_develops(void)
{
    size_t w = 20;
    size_t h = 10;
    uint16_t *data = noise(w * h * 3, 16);
    tg_dng spec = tg_dng_cfa(w, h, data);
    tg_bytes b;
    pc_raw_sensor s;
    spec.samples = 3;
    spec.storage = TG_LJ92_TILES;
    spec.tile_w = 16;
    spec.tile_h = 16;
    b = tg_dng_build(&spec);
    if (decode_ok(b.d, b.n, &s)) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        CHECK(s.samples == 3 && !s.has_cfa);
        CHECK(data_is(&s, data, w * h * 3));
        if (develop_sensor_ok(&s, &o, &d)) {
            CHECK(d.width == 20 && d.height == 10 && d.nrgb == 600);
            CHECK(!d.info.has_cfa);
            pc_raw_developed_fini(&d);
        }
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(data);
}

static void test_metadata_is_read(void)
{
    size_t w = 40;
    size_t h = 30;
    uint16_t *data = fill16(w * h, 1000);
    uint16_t *linz = tg_xalloc(4096 * sizeof *linz);
    tg_dng spec = tg_dng_cfa(w, h, data);
    tg_bytes b;
    pc_raw_sensor s;
    uint32_t v;
    static const uint8_t GRBG[4] = {1, 0, 2, 1};
    static const float BLACK[4] = {100.0f, 101.0f, 102.0f, 103.0f};
    for (v = 0; v < 4096; v++) {
        linz[v] = (uint16_t)(v * 3 < 65535 ? v * 3 : 65535);
    }
    spec.bits = 14;
    spec.white = 15000;
    spec.black[0] = 100;
    spec.black[1] = 101;
    spec.black[2] = 102;
    spec.black[3] = 103;
    spec.nblack = 4;
    spec.black_repeat[0] = 2;
    spec.black_repeat[1] = 2;
    spec.has_active = true; /* top, left, bottom, right */
    spec.active[0] = 2;
    spec.active[1] = 4;
    spec.active[2] = 28;
    spec.active[3] = 36;
    spec.has_crop = true;
    spec.crop_origin[0] = 2;
    spec.crop_origin[1] = 1;
    spec.crop_size[0] = 20;
    spec.crop_size[1] = 16;
    memcpy(spec.cfa, GRBG, 4); /* GRBG at the active-area origin */
    spec.has_neutral = true;
    spec.neutral[0] = 0.5;
    spec.neutral[1] = 1.0;
    spec.neutral[2] = 0.8;
    spec.has_baseline = true;
    spec.baseline = 0.5;
    spec.orientation = 6;
    spec.linearization = linz;
    spec.nlinearization = 4096;
    b = tg_dng_build(&spec);
    if (decode_ok(b.d, b.n, &s)) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        CHECK(white_is(&s, 15000.0f));
        CHECK(black_values_are(&s, BLACK, 4));
        CHECK(pc_raw_rect_eq(s.active, pc_raw_rect_make(4, 2, 32, 26)));
        CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(6, 3, 20, 16)));
        /* Pattern anchored at the active area origin (4, 2): GRBG there. */
        CHECK(phase_is(&s, 4, 2, GRBG));
        CHECK(s.color.has_as_shot_neutral && s.color.as_shot_neutral[0] == 0.5 &&
              s.color.as_shot_neutral[1] == 1.0 && s.color.as_shot_neutral[2] == 0.8);
        CHECK(fabs(s.baseline_exposure - 0.5) < 1e-6);
        CHECK_EQ_UINT(s.orientation, 6);
        CHECK(s.linearization != NULL && s.nlinearization == 4096);
        if (develop_sensor_ok(&s, &o, &d)) {
            /* Orientation 6 swaps the cropped 20x16 to 16x20. */
            CHECK(d.width == 16 && d.height == 20);
            CHECK(d.info.wb_multipliers[0] == 2.0 && d.info.wb_multipliers[1] == 1.0 &&
                  d.info.wb_multipliers[2] == 1.25);
            pc_raw_developed_fini(&d);
        }
        o.orient = false;
        if (develop_sensor_ok(&s, &o, &d)) {
            CHECK(d.width == 20 && d.height == 16);
            pc_raw_developed_fini(&d);
        }
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(linz);
    free(data);
}

static tg_dng offset_active_spec(uint16_t *data)
{
    tg_dng spec = tg_dng_cfa(16, 12, data);
    spec.has_active = true;
    spec.active[0] = 2;
    spec.active[1] = 3;
    spec.active[2] = 10;
    spec.active[3] = 14;
    return spec;
}

static void test_enormous_double_crop_origin_is_rejected(void)
{
    uint16_t *data = fill16(16 * 12, 1000);
    tg_dng spec = offset_active_spec(data);
    tg_bytes b;
    pc_error e;
    spec.has_crop = true;
    spec.crop_origin[0] = 0;
    spec.crop_origin[1] = 0;
    spec.crop_size[0] = 11;
    spec.crop_size[1] = 8;
    spec.has_origin_double = true;
    spec.origin_double[0] = 1e300;
    spec.origin_double[1] = 0.0;
    b = tg_dng_build(&spec);
    CHECK_EQ_INT(decode_default_status(b.d, b.n, &e), PC_ERR_FORMAT);
    tg_free(&b);
    free(data);
}

static void test_out_of_range_default_crop_falls_back_to_active_area_with_warning(void)
{
    uint16_t *data = fill16(16 * 12, 1000);
    tg_dng spec = offset_active_spec(data);
    tg_bytes b;
    pc_raw_sensor s;
    spec.has_crop = true;
    spec.crop_origin[0] = 10;
    spec.crop_origin[1] = 0;
    spec.crop_size[0] = 5;
    spec.crop_size[1] = 4;
    b = tg_dng_build(&spec);
    if (decode_ok(b.d, b.n, &s)) {
        CHECK(pc_raw_rect_eq(s.crop, s.active));
        CHECK(any_warning2(&s.warnings, "out-of-range", "default crop"));
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(data);
}

/* DefaultCropSize defaults to the whole image, which can't fit once the origin moves in. */
static void test_default_crop_origin_without_size_falls_back_to_active_area_with_warning(void)
{
    uint16_t *data = fill16(16 * 12, 1000);
    tg_dng spec = offset_active_spec(data);
    tg_bytes b;
    pc_raw_sensor s;
    spec.has_origin_double = true;
    spec.origin_double[0] = 1.0;
    spec.origin_double[1] = 1.0;
    b = tg_dng_build(&spec);
    if (decode_ok(b.d, b.n, &s)) {
        CHECK(pc_raw_rect_eq(s.crop, s.active));
        CHECK(any_warning2(&s.warnings, "out-of-range", "default crop"));
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(data);
}

/* #950: DefaultCropOrigin defaults to (0, 0), so a size-only crop starts at the active area's
 * top-left corner. */
static void test_default_crop_size_without_origin_crops_from_the_active_area_origin(void)
{
    uint16_t *data = tg_xalloc(64 * sizeof *data);
    uint16_t *data2 = fill16(16 * 12, 1000);
    tg_dng spec = tg_dng_cfa(8, 8, data);
    tg_bytes b;
    pc_raw_sensor s;
    size_t i;
    for (i = 0; i < 64; i++) {
        data[i] = (uint16_t)(i * 37);
    }
    spec.has_active = true;
    spec.active[0] = 0;
    spec.active[1] = 0;
    spec.active[2] = 8;
    spec.active[3] = 8;
    spec.has_size_only = true;
    spec.size_only[0] = 4;
    spec.size_only[1] = 4;
    b = tg_dng_build(&spec);
    if (decode_ok(b.d, b.n, &s)) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(0, 0, 4, 4)));
        CHECK(!pc_raw_strings_any(&s.warnings, "default crop"));
        if (develop_ok(b.d, b.n, &o, &d)) {
            CHECK(d.width == 4 && d.height == 4);
            pc_raw_developed_fini(&d);
        }
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);

    /* Inside an offset active area the crop starts at that area's corner. */
    spec = offset_active_spec(data2);
    spec.has_size_only = true;
    spec.size_only[0] = 6;
    spec.size_only[1] = 4;
    b = tg_dng_build(&spec);
    if (decode_ok(b.d, b.n, &s)) {
        CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(3, 2, 6, 4)));
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(data);
    free(data2);
}

/* Linear sRGB (D65) -> XYZ, from the sRGB primaries (IEC 61966-2-1). */
static const double SRGB_TO_XYZ[3][3] = {
    {0.4124564, 0.3575761, 0.1804375}, {0.2126729, 0.7151522, 0.0721750}, {0.0193339, 0.1191920, 0.9503041}};

static void invert(const double m[3][3], double out[3][3])
{
    double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1],
           i = m[2][2];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    out[0][0] = (e * i - f * h) / det;
    out[0][1] = (c * h - b * i) / det;
    out[0][2] = (b * f - c * e) / det;
    out[1][0] = (f * g - d * i) / det;
    out[1][1] = (a * i - c * g) / det;
    out[1][2] = (c * d - a * f) / det;
    out[2][0] = (d * h - e * g) / det;
    out[2][1] = (b * g - a * h) / det;
    out[2][2] = (a * e - b * d) / det;
}

static void flat9(const double m[3][3], double out[9])
{
    int i;
    for (i = 0; i < 9; i++) {
        out[i] = m[i / 3][i % 3];
    }
}

static void ap(const double m[3][3], const double v[3], double out[3])
{
    double r[3];
    int i;
    for (i = 0; i < 3; i++) {
        r[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    }
    memcpy(out, r, sizeof r);
}

/* The DNG model computed independently: camera XYZ of `camera` under an illuminant whose camera
 * neutral is `neutral`, Bradford-adapted from that white (normalized to Y = 1) to D50, then
 * XYZ D50 -> linear ProPhoto. */
static void reference_prophoto(const double camera[3], const double neutral[3], double out[3])
{
    static const double BRADFORD[3][3] = {
        {0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}};
    static const double D50[3] = {0.9642, 1.0, 0.8249};
    static const double XYZ_D50_TO_PROPHOTO[3][3] = {
        {1.3459433, -0.2556075, -0.0511118}, {-0.5445989, 1.5081673, 0.0205351}, {0.0, 0.0, 1.2118128}};
    double white[3];
    double xyz[3];
    double nxyz[3];
    double s[3];
    double d[3];
    double cone[3];
    double binv[3][3];
    double scaled[3];
    double adapted[3];
    double wy;
    int c;
    ap(SRGB_TO_XYZ, neutral, white);
    wy = white[1];
    for (c = 0; c < 3; c++) {
        white[c] /= wy;
    }
    ap(SRGB_TO_XYZ, camera, xyz);
    ap(SRGB_TO_XYZ, neutral, nxyz);
    for (c = 0; c < 3; c++) {
        xyz[c] /= nxyz[1];
    }
    ap(BRADFORD, white, s);
    ap(BRADFORD, D50, d);
    ap(BRADFORD, xyz, cone);
    for (c = 0; c < 3; c++) {
        scaled[c] = cone[c] * d[c] / s[c];
    }
    invert(BRADFORD, binv);
    ap((const double(*)[3])binv, scaled, adapted);
    ap(XYZ_D50_TO_PROPHOTO, adapted, out);
}

static void test_colour_matrix_development_matches_reference(void)
{
    /* A camera whose native RGB is linear sRGB: ColorMatrix = XYZ -> sRGB at D65. The scene is
     * captured with per-channel gains (the "white balance" the camera must undo), recorded as
     * AsShotNeutral. */
    size_t w = 48;
    size_t h = 32;
    static const float GAINS[3] = {0.5f, 1.0f, 0.7f};
    static const float PATCHES[4][3] = {
        {0.18f, 0.18f, 0.18f}, {0.40f, 0.10f, 0.05f}, {0.05f, 0.30f, 0.10f}, {0.10f, 0.12f, 0.45f}};
    static const pc_raw_demosaic METHODS[3] = {PC_RAW_BILINEAR, PC_RAW_MHC, PC_RAW_AHD};
    float(*rgb)[3] = tg_xalloc(w * h * sizeof *rgb);
    uint16_t *data;
    tg_dng spec;
    double cm[3][3];
    size_t i;
    int st;
    for (i = 0; i < w * h; i++) {
        const float *p = PATCHES[((i % w) / (w / 2)) + 2 * ((i / w) / (h / 2))];
        int c;
        for (c = 0; c < 3; c++) {
            rgb[i][c] = p[c] * GAINS[c];
        }
    }
    data = mosaic_of((const float(*)[3])rgb, w * h, w, RGGB, 256, 16383);
    spec = tg_dng_cfa(w, h, data);
    spec.bits = 14;
    spec.black[0] = 256;
    spec.white = 16383;
    invert(SRGB_TO_XYZ, cm);
    spec.has_cm1 = true;
    spec.cm1_ill = 21;
    flat9((const double(*)[3])cm, spec.cm1);
    spec.has_neutral = true;
    for (i = 0; i < 3; i++) {
        spec.neutral[i] = (double)GAINS[i];
    }
    for (st = 0; st < 2; st++) {
        tg_bytes b;
        int mi;
        if (st == 0) {
            spec.storage = TG_STRIPS;
            spec.rows = 32;
        } else {
            spec.storage = TG_LJ92_TILES;
            spec.tile_w = 16;
            spec.tile_h = 16;
        }
        b = tg_dng_build(&spec);
        for (mi = 0; mi < 3; mi++) {
            pc_raw_develop_options o = pc_raw_develop_options_default();
            pc_raw_developed d;
            size_t pi;
            o.demosaic = METHODS[mi];
            if (!develop_ok(b.d, b.n, &o, &d)) {
                continue;
            }
            for (pi = 0; pi < 4; pi++) {
                /* Centre of each patch. */
                size_t cx = (pi % 2) * (w / 2) + w / 4;
                size_t cy = (pi / 2) * (h / 2) + h / 4;
                const uint16_t *px = &d.rgb[(cy * w + cx) * 3];
                double g[3];
                double cam[3];
                double want[3];
                int c;
                for (c = 0; c < 3; c++) {
                    g[c] = (double)GAINS[c];
                    cam[c] = (double)PATCHES[pi][c] * g[c];
                }
                reference_prophoto(cam, g, want);
                for (c = 0; c < 3; c++) {
                    double got = pow((double)px[c] / 65535.0, 1.8);
                    pc_t_checks++;
                    if (!(fabs(got - want[c]) < 0.006)) {
                        PC_T_FAIL("storage %d method %d patch %zu c %d: got %.4f want %.4f", st, mi, pi, c, got,
                                  want[c]);
                    }
                }
            }
            pc_raw_developed_fini(&d);
        }
        tg_free(&b);
    }
    free(rgb);
    free(data);
}

static void test_forward_matrix_path_maps_neutral_to_white(void)
{
    size_t w = 16;
    size_t h = 16;
    float(*rgb)[3] = tg_xalloc(w * h * sizeof *rgb); /* a grey under a green-ish light */
    uint16_t *data;
    tg_dng spec;
    double cm[3][3];
    /* ForwardMatrix: balanced camera -> XYZ D50 (sRGB adapted to D50; white -> D50). */
    static const double FM[9] = {0.4360747, 0.3850649, 0.1430804, 0.2225045, 0.7168786,
                                 0.0606169, 0.0139322, 0.0971045, 0.7141733};
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_raw_developed d;
    tg_bytes b;
    size_t i;
    for (i = 0; i < w * h; i++) {
        rgb[i][0] = 0.3f;
        rgb[i][1] = 0.6f;
        rgb[i][2] = 0.45f;
    }
    data = mosaic_of((const float(*)[3])rgb, w * h, w, RGGB, 0, 65535);
    spec = tg_dng_cfa(w, h, data);
    invert(SRGB_TO_XYZ, cm);
    spec.has_cm1 = true;
    spec.cm1_ill = 17;
    flat9((const double(*)[3])cm, spec.cm1);
    spec.has_cm2 = true;
    spec.cm2_ill = 21;
    flat9((const double(*)[3])cm, spec.cm2);
    spec.has_fm1 = true;
    memcpy(spec.fm1, FM, sizeof FM);
    spec.has_fm2 = true;
    memcpy(spec.fm2, FM, sizeof FM);
    spec.has_neutral = true;
    spec.neutral[0] = 0.5;
    spec.neutral[1] = 1.0;
    spec.neutral[2] = 0.75;
    b = tg_dng_build(&spec);
    if (develop_ok(b.d, b.n, &o, &d)) {
        const uint16_t *px = &d.rgb[(8 * w + 8) * 3];
        int c;
        /* Balanced grey 0.6 -> ProPhoto (0.6, 0.6, 0.6). */
        for (c = 0; c < 3; c++) {
            double got = pow((double)px[c] / 65535.0, 1.8);
            CHECK(fabs(got - 0.6) < 0.004);
        }
        pc_raw_developed_fini(&d);
    }
    tg_free(&b);
    free(rgb);
    free(data);
}

static void test_highlights_clip_to_white_and_exposure_applies(void)
{
    size_t w = 8;
    size_t h = 8;
    uint16_t *full = fill16(w * h, 65535);
    uint16_t *grey = fill16(w * h, 16384); /* 0.25 linear grey */
    tg_dng spec = tg_dng_cfa(w, h, full);
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_raw_developed d;
    tg_bytes b;
    spec.has_neutral = true;
    spec.neutral[0] = 0.5;
    spec.neutral[1] = 1.0;
    spec.neutral[2] = 0.6;
    b = tg_dng_build(&spec);
    if (develop_ok(b.d, b.n, &o, &d)) {
        size_t i;
        bool all = true;
        for (i = 0; i < d.nrgb; i++) {
            all = all && d.rgb[i] >= 65500;
        }
        CHECK(all); /* clipped sensor must render white */
        pc_raw_developed_fini(&d);
    }
    tg_free(&b);
    spec = tg_dng_cfa(w, h, grey);
    spec.has_neutral = true;
    spec.neutral[0] = 1.0;
    spec.neutral[1] = 1.0;
    spec.neutral[2] = 1.0;
    b = tg_dng_build(&spec);
    if (develop_ok(b.d, b.n, &o, &d)) {
        uint16_t base = d.rgb[27];
        pc_raw_developed_fini(&d);
        o.exposure = 1.0;
        if (develop_ok(b.d, b.n, &o, &d)) {
            uint16_t up = d.rgb[27];
            pc_t_checks++;
            if (!(fabs(lin(up) / lin(base) - 2.0) < 0.02)) {
                PC_T_FAIL("%g vs %g", lin(up), lin(base));
            }
            pc_raw_developed_fini(&d);
        }
    }
    tg_free(&b);
    free(full);
    free(grey);
}

static void test_gain_map_opcode_is_applied(void)
{
    /* Uniform 0.25 grey; a gain map that doubles everything over the whole image. */
    size_t w = 16;
    size_t h = 12;
    uint16_t *data = fill16(w * h, 16384);
    tg_dng spec = tg_dng_cfa(w, h, data);
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_raw_developed d;
    tg_bytes b;
    tg_bytes ol;
    uint16_t base = 0;
    uint32_t area[4] = {0, 0, (uint32_t)h, (uint32_t)w};
    static const float G[2][2] = {{2.0f, 2.0f}, {2.0f, 2.0f}};
    spec.has_neutral = true;
    spec.neutral[0] = 1.0;
    spec.neutral[1] = 1.0;
    spec.neutral[2] = 1.0;
    b = tg_dng_build(&spec);
    if (develop_ok(b.d, b.n, &o, &d)) {
        base = d.rgb[(6 * w + 8) * 3 + 1];
        pc_raw_developed_fini(&d);
    }
    tg_free(&b);
    ol = tg_gain_map_opcode_list(area, 1, G);
    spec.opcode_list2 = ol.d;
    spec.nopcode_list2 = ol.n;
    b = tg_dng_build(&spec);
    if (develop_ok(b.d, b.n, &o, &d)) {
        uint16_t gained = d.rgb[(6 * w + 8) * 3 + 1];
        CHECK(!pc_raw_strings_any(&d.warnings, "Opcode"));
        pc_t_checks++;
        if (!(fabs(lin(gained) / lin(base) - 2.0) < 0.02)) {
            PC_T_FAIL("%g vs %g", lin(gained), lin(base));
        }
        pc_raw_developed_fini(&d);
    }
    tg_free(&b);
    tg_free(&ol);
    free(data);
}

static void test_white_balance_options(void)
{
    size_t w = 16;
    size_t h = 16;
    float(*rgb)[3] = tg_xalloc(w * h * sizeof *rgb);
    uint16_t *data;
    tg_dng spec;
    tg_bytes b;
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_raw_developed d;
    pc_error e;
    size_t i;
    for (i = 0; i < w * h; i++) {
        rgb[i][0] = 0.2f;
        rgb[i][1] = 0.4f;
        rgb[i][2] = 0.3f;
    }
    data = mosaic_of((const float(*)[3])rgb, w * h, w, RGGB, 0, 65535);
    spec = tg_dng_cfa(w, h, data);
    b = tg_dng_build(&spec);
    /* No AsShotNeutral: grey world -> neutral output. */
    if (develop_ok(b.d, b.n, &o, &d)) {
        const uint16_t *px = &d.rgb[(8 * w + 8) * 3];
        CHECK(pc_raw_strings_any(&d.warnings, "grey world"));
        pc_t_checks++;
        if (!(abs((int)px[0] - (int)px[1]) < 300 && abs((int)px[2] - (int)px[1]) < 300)) {
            PC_T_FAIL("[%u, %u, %u]", px[0], px[1], px[2]);
        }
        pc_raw_developed_fini(&d);
    }
    o.white_balance.kind = PC_RAW_WB_MULTIPLIERS;
    o.white_balance.multipliers[0] = 2.0;
    o.white_balance.multipliers[1] = 1.0;
    o.white_balance.multipliers[2] = 4.0 / 3.0;
    if (develop_ok(b.d, b.n, &o, &d)) {
        CHECK(round(d.info.wb_multipliers[0] * 1000.0) == 2000.0 &&
              round(d.info.wb_multipliers[1] * 1000.0) == 1000.0 &&
              round(d.info.wb_multipliers[2] * 1000.0) == 1333.0);
        pc_raw_developed_fini(&d);
    }
    o.white_balance.multipliers[0] = 0.0;
    o.white_balance.multipliers[1] = 1.0;
    o.white_balance.multipliers[2] = 1.0;
    if (pc_raw_develop(b.d, b.n, &o, &d, &e) == PC_OK) {
        PC_T_FAIL("zero multiplier developed");
        pc_raw_developed_fini(&d);
    }
    pc_t_checks++;
    tg_free(&b);
    free(rgb);
    free(data);
}

static void test_smooth_scene_develops_with_every_method(void)
{
    size_t w = 64;
    size_t h = 48;
    static const uint8_t BGGR[4] = {2, 1, 1, 0};
    static const pc_raw_demosaic METHODS[3] = {PC_RAW_BILINEAR, PC_RAW_MHC, PC_RAW_AHD};
    uint16_t *data = scene_mosaic(w, h, BGGR, 0, 65535);
    tg_dng spec = tg_dng_cfa(w, h, data);
    pc_raw_developed outs[3];
    bool ok[3];
    tg_bytes b;
    int m;
    memcpy(spec.cfa, BGGR, 4);
    spec.has_neutral = true;
    spec.neutral[0] = 1.0;
    spec.neutral[1] = 1.0;
    spec.neutral[2] = 1.0;
    b = tg_dng_build(&spec);
    for (m = 0; m < 3; m++) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        o.demosaic = METHODS[m];
        ok[m] = develop_ok(b.d, b.n, &o, &outs[m]);
    }
    if (ok[0] && ok[1] && ok[2]) {
        /* Interior pixels agree closely across methods on a smooth scene. */
        size_t x;
        size_t y;
        size_t bad = 0;
        for (y = 4; y < h - 4; y++) {
            for (x = 4; x < w - 4; x++) {
                int c;
                for (c = 0; c < 3; c++) {
                    size_t i = (y * w + x) * 3 + (size_t)c;
                    int a = outs[0].rgb[i];
                    int bb = outs[1].rgb[i];
                    int dd = outs[2].rgb[i];
                    if (!(abs(a - bb) < 800 && abs(a - dd) < 800)) {
                        if (bad++ < 5) {
                            PC_T_FAIL("(%zu,%zu) c%d: %d %d %d", x, y, c, a, bb, dd);
                        }
                    }
                }
            }
        }
        CHECK_EQ_UINT(bad, 0);
    }
    for (m = 0; m < 3; m++) {
        if (ok[m]) {
            pc_raw_developed_fini(&outs[m]);
        }
    }
    tg_free(&b);
    free(data);
}

static void test_unsupported_variants_say_so(void)
{
    uint16_t *data = fill16(64, 0);
    tg_dng spec = tg_dng_cfa(8, 8, data);
    static const uint8_t NOT_BAYER[4] = {0, 1, 2, 1};
    tg_bytes b;
    pc_error e;
    memcpy(spec.cfa, NOT_BAYER, 4); /* not Bayer */
    b = tg_dng_build(&spec);
    CHECK_EQ_INT(decode_default_status(b.d, b.n, &e), PC_ERR_UNSUPPORTED);
    tg_free(&b);
    free(data);
}

static void test_limits_are_enforced_before_allocating(void)
{
    uint16_t *data = fill16(64 * 64, 0);
    tg_dng spec = tg_dng_cfa(64, 64, data);
    tg_bytes b = tg_dng_build(&spec);
    pc_raw_limits tiny = pc_raw_limits_default();
    pc_error e;
    tiny.max_width = 32;
    CHECK_EQ_INT(decode_status(b.d, b.n, &tiny, &e), PC_ERR_LIMIT);
    tiny = pc_raw_limits_default();
    tiny.max_alloc = 1000;
    CHECK_EQ_INT(decode_status(b.d, b.n, &tiny, &e), PC_ERR_LIMIT);
    tg_free(&b);
    free(data);
}

/* ======================================================================================
 * hostile.rs: truncated and corrupted raw files must fail cleanly, never crash, and never
 * allocate beyond the limits.
 * ====================================================================================== */

#define NSAMPLES 8

static size_t samples(tg_bytes out[NSAMPLES])
{
    size_t w = 24;
    size_t h = 12;
    uint16_t *cfa = scene_mosaic(w, h, RGGB, 64, 4000);
    tg_dng d = tg_dng_cfa(w, h, cfa);
    static const double CM[9] = {3.2, -1.5, -0.5, -1.0, 1.9, 0.04, 0.06, -0.2, 1.06};
    static const uint16_t CURVE[4] = {8000, 10400, 12900, 14100};
    uint16_t *data;
    uint16_t *codes;
    uint16_t *m30;
    size_t i;
    size_t n = 0;
    tg_cr2 c;
    d.bits = 12;
    d.white = 4000;
    d.black[0] = 64;
    d.has_neutral = true;
    d.neutral[0] = 0.5;
    d.neutral[1] = 1.0;
    d.neutral[2] = 0.7;
    d.has_cm1 = true;
    d.cm1_ill = 21;
    memcpy(d.cm1, CM, sizeof CM);
    out[n++] = tg_dng_build(&d);
    d.storage = TG_LJ92_TILES;
    d.tile_w = 16;
    d.tile_h = 8;
    d.has_active = true;
    d.active[0] = 1;
    d.active[1] = 2;
    d.active[2] = 11;
    d.active[3] = 22;
    d.has_crop = true;
    d.crop_origin[0] = 1;
    d.crop_origin[1] = 1;
    d.crop_size[0] = 16;
    d.crop_size[1] = 8;
    out[n++] = tg_dng_build(&d);
    d.storage = TG_STRIPS;
    d.rows = 5;
    d.big_endian = true;
    out[n++] = tg_dng_build(&d);
    data = tg_xalloc(400 * sizeof *data);
    for (i = 0; i < 400; i++) {
        data[i] = (uint16_t)(600 + (uint16_t)(i * 7) % 3000);
    }
    memset(&c, 0, sizeof c);
    c.width = 40;
    c.height = 10;
    c.data = data;
    c.precision = 14;
    c.components = 2;
    c.slices[0] = 16;
    c.slices[1] = 16;
    c.slices[2] = 8;
    c.nslices = 3;
    c.has_borders = true;
    c.borders[0] = 8;
    c.borders[1] = 2;
    c.borders[2] = 39;
    c.borders[3] = 9;
    c.has_wb = true;
    c.wb_rggb[0] = 2000;
    c.wb_rggb[1] = 1024;
    c.wb_rggb[2] = 1024;
    c.wb_rggb[3] = 1500;
    c.orientation = 8;
    out[n++] = tg_cr2_build(&c);
    free(data);
    out[n++] = tg_tiff_ep("NIKON", w, h, cfa, RGGB, 12, NULL, 0);
    codes = scene_mosaic(64, 6, RGGB, 256, 1900);
    out[n++] = tg_sony_craw(64, 6, codes, CURVE);
    m30 = scene_mosaic(30, 8, RGGB, 128, 4095);
    out[n++] = tg_rw2(30, 8, m30, 12);
    out[n++] = tg_orf(w, h, cfa);
    free(cfa);
    free(codes);
    free(m30);
    return n;
}

static void free_samples(tg_bytes *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        tg_free(&s[i]);
    }
}

static void exercise(const uint8_t *b, size_t n)
{
    pc_raw_develop_options opts = pc_raw_develop_options_default();
    pc_raw_format f;
    pc_raw_preview p;
    pc_raw_sensor s;
    pc_error e;
    opts.limits.max_alloc = 64u << 20;
    (void)pc_raw_identify(b, n, &f);
    (void)pc_raw_embedded_preview(b, n, &p);
    if (pc_raw_decode(b, n, &opts.limits, &s, &e) == PC_OK) {
        static const pc_raw_demosaic M[2] = {PC_RAW_BILINEAR, PC_RAW_AHD};
        int k;
        for (k = 0; k < 2; k++) {
            pc_raw_develop_options o = opts;
            pc_raw_developed d;
            o.demosaic = M[k];
            if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
                pc_raw_developed_fini(&d);
            }
        }
        pc_raw_sensor_fini(&s);
    }
}

static void test_originals_develop(void)
{
    tg_bytes s[NSAMPLES];
    size_t n = samples(s);
    size_t i;
    for (i = 0; i < n; i++) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        pc_error e;
        pc_t_checks++;
        if (pc_raw_develop(s[i].d, s[i].n, &o, &d, &e) == PC_OK) {
            pc_raw_developed_fini(&d);
        } else {
            PC_T_FAIL("sample %zu: %s", i, e.msg);
        }
    }
    free_samples(s, n);
}

static void test_every_truncation_fails_cleanly(void)
{
    tg_bytes s[NSAMPLES];
    size_t n = samples(s);
    size_t i;
    for (i = 0; i < n; i++) {
        size_t k;
        for (k = 0; k < s[i].n; k += 3) {
            uint8_t *cut = copy_exact(s[i].d, k);
            exercise(cut, k);
            free(cut);
        }
    }
    free_samples(s, n);
}

static void test_random_corruption_never_panics(void)
{
    tg_bytes s[NSAMPLES];
    size_t n = samples(s);
    size_t i;
    uint64_t st = 0xDEADBEEFull;
#define NEXT() (st = st * 6364136223846793005ull + 1442695040888963407ull, st >> 33)
    for (i = 0; i < n; i++) {
        int it;
        for (it = 0; it < 400; it++) {
            uint8_t *c = copy_exact(s[i].d, s[i].n);
            uint64_t m = 1 + NEXT() % 6;
            uint64_t j;
            for (j = 0; j < m; j++) {
                size_t at = (size_t)(NEXT() % s[i].n);
                uint64_t r = NEXT() % 4;
                c[at] = (uint8_t)(r == 0 ? 0xFF : r == 1 ? 0 : NEXT());
            }
            exercise(c, s[i].n);
            free(c);
        }
    }
#undef NEXT
    free_samples(s, n);
}

static void test_absurd_dimensions_are_rejected_without_allocating(void)
{
    uint16_t *data = fill16(16, 0);
    tg_dng d = tg_dng_cfa(4, 4, data);
    tg_bytes b;
    size_t patched = 0;
    size_t i;
    pc_error e;
    d.storage = TG_STRIPS;
    d.rows = 4;
    b = tg_dng_build(&d);
    /* Patch the raw IFD's ImageWidth / ImageLength (LONG, inline) to 200000 x 200000. */
    for (i = 0; i + 12 < b.n; i++) {
        uint8_t *p = b.d + i;
        if (p[0] == 0x00 && p[1] == 0x01 && p[2] == 4 && p[3] == 0 && p[8] == 4 && p[9] == 0 && p[10] == 0 &&
            p[11] == 0) {
            static const uint8_t BIG[4] = {0x40, 0x0D, 0x03, 0x00}; /* 200000 LE */
            if (i + 24 > b.n) {
                PC_T_FAIL("match at %zu too close to the end", i);
                break;
            }
            memcpy(p + 8, BIG, 4);
            memcpy(p + 20, BIG, 4);
            patched++;
        }
    }
    CHECK(patched > 0);
    CHECK_EQ_INT(decode_default_status(b.d, b.n, &e), PC_ERR_LIMIT);
    tg_free(&b);
    free(data);
}

/* ======================================================================================
 * vendor_codecs.rs: Sony compressed ARW (cRAW), Panasonic RW2 (RawFormat 5) and uncompressed
 * Olympus ORF (with its maker-note preview), round-tripped through the synthetic encoders.
 * ====================================================================================== */

static const uint16_t CURVE[4] = {8000, 10400, 12900, 14100};

/* The cRAW tone curve as the decoder documents it (independent re-statement for the tests):
 * step 1, 2, 4, 8, 16 per 1/8 code between the knots, / 4. */
static uint16_t tone(uint16_t code)
{
    static const uint32_t PTS[6] = {0, 8000, 10400, 12900, 14100, 16384};
    uint32_t x = (uint32_t)code * 8;
    uint32_t y = 0;
    uint32_t i;
    for (i = 0; i < 5; i++) {
        if (x > PTS[i]) {
            uint32_t top = x < PTS[i + 1] ? x : PTS[i + 1];
            y += (top - PTS[i]) << i;
        }
    }
    y /= 4;
    return (uint16_t)(y < 16383 ? y : 16383);
}

/* 11-bit codes over a smooth scene: every 16-pixel run spans < 128 codes, so cRAW is exact. */
static uint16_t *smooth_codes(size_t w, size_t h) { return scene_mosaic(w, h, RGGB, 256, 1900); }

static void test_craw_round_trip_is_exact_on_smooth_data(void)
{
    size_t w = 640;
    size_t h = 8;
    uint16_t *codes = smooth_codes(w, h);
    tg_bytes b = tg_sony_craw(w, h, codes, CURVE);
    uint16_t *want = tg_xalloc(w * h * sizeof *want);
    static const float BLACK[4] = {512.0f, 512.0f, 512.0f, 512.0f};
    pc_raw_format f;
    pc_raw_sensor s;
    size_t i;
    CHECK(pc_raw_identify(b.d, b.n, &f) && f == PC_RAW_ARW);
    for (i = 0; i < w * h; i++) {
        want[i] = tone(codes[i]);
    }
    if (decode_ok(b.d, b.n, &s)) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        CHECK(s.width == w && s.height == h);
        CHECK(data_is(&s, want, w * h));
        CHECK(black_values_are(&s, BLACK, 4));
        CHECK(white_is(&s, 16383.0f));
        CHECK(camera_wb_is(&s, 2.0, 1.0, 1.5));
        if (develop_sensor_ok(&s, &o, &d)) {
            CHECK((size_t)d.width == w && (size_t)d.height == h);
            pc_raw_developed_fini(&d);
        }
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(want);
    free(codes);
}

static void test_craw_wide_ranges_lose_only_low_bits(void)
{
    /* Pseudo-random codes: wide ranges force steps of 2..16. */
    size_t w = 64;
    size_t h = 8;
    uint32_t st = 12345;
    uint16_t *codes = tg_xalloc(w * h * sizeof *codes);
    tg_bytes b;
    pc_raw_sensor d;
    size_t i;
    for (i = 0; i < w * h; i++) {
        st = st * 1103515245u + 12345u;
        codes[i] = (uint16_t)((st >> 16) % 2000);
    }
    b = tg_sony_craw(w, h, codes, CURVE);
    if (decode_ok(b.d, b.n, &d)) {
        /* Re-derive each pixel's step from its block and check the code error. */
        size_t y;
        size_t bad = 0;
        for (y = 0; y < h; y++) {
            size_t g;
            for (g = 0; g < w / 32; g++) {
                size_t parity;
                for (parity = 0; parity < 2; parity++) {
                    uint16_t run[16];
                    uint16_t mx;
                    uint16_t mn;
                    uint16_t range;
                    uint16_t step = 0;
                    unsigned k;
                    for (i = 0; i < 16; i++) {
                        run[i] = codes[y * w + g * 32 + 2 * i + parity];
                    }
                    mx = run[0];
                    mn = run[0];
                    for (i = 1; i < 16; i++) {
                        mx = run[i] > mx ? run[i] : mx;
                        mn = run[i] < mn ? run[i] : mn;
                    }
                    range = (uint16_t)(mx - mn);
                    for (k = 0; k < 5; k++) {
                        if (128u * (1u << k) > range) {
                            step = (uint16_t)(1u << k);
                            break;
                        }
                    }
                    if (step == 0) {
                        PC_T_FAIL("range %u has no step", range);
                        continue;
                    }
                    for (i = 0; i < 16; i++) {
                        uint16_t c = run[i];
                        uint16_t got = d.data[y * w + g * 32 + 2 * i + parity];
                        uint16_t lo = tone(c >= step - 1 ? (uint16_t)(c - (step - 1)) : 0);
                        if (!(got <= tone(c) && got >= lo)) {
                            if (bad++ < 5) {
                                PC_T_FAIL("code %u step %u: got %u, want %u..=%u", c, step, got, lo, tone(c));
                            }
                        }
                    }
                }
            }
        }
        CHECK_EQ_UINT(bad, 0);
        pc_raw_sensor_fini(&d);
    }
    tg_free(&b);
    free(codes);
}

/* Bits [at, at + n) of a little-endian 128-bit value. */
static uint32_t bits_le(const uint8_t b[16], uint32_t at, uint32_t n)
{
    uint32_t v = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint32_t bit = at + i;
        v |= (((uint32_t)b[bit / 8] >> (bit % 8)) & 1u) << i;
    }
    return v;
}

static void test_craw_block_layout(void)
{
    /* Max 300 at 3, min 100 at 9, others 100 + 2k: range 200 -> step 2. */
    uint16_t codes[16];
    uint8_t b[16];
    size_t i;
    for (i = 0; i < 16; i++) {
        codes[i] = (uint16_t)(100 + 2 * i);
    }
    codes[3] = 300;
    codes[9] = 100;
    codes[0] = 101; /* odd: rounds down to 100 at step 2 */
    tg_craw_block(codes, b);
    CHECK_EQ_UINT(bits_le(b, 0, 11), 300);
    CHECK_EQ_UINT(bits_le(b, 11, 11), 100);
    CHECK_EQ_UINT(bits_le(b, 22, 4), 3);
    CHECK_EQ_UINT(bits_le(b, 26, 4), 9);
    CHECK_EQ_UINT(bits_le(b, 30, 7), 0); /* pixel 0: (101 - 100) >> 1 */
}

static void test_craw_unusual_variants_are_unsupported(void)
{
    size_t h = 4;
    uint16_t *codes32 = fill16(32 * h, 300);
    static const uint16_t BAD_CURVE[4] = {9000, 8000, 12000, 13000};
    tg_bytes ok = tg_sony_craw(32, h, codes32, CURVE);
    tg_bytes bad;
    uint8_t *cut;
    pc_error e;
    CHECK_EQ_INT(decode_default_status(ok.d, ok.n, &e), PC_OK);
    /* A non-increasing tone curve is not guessed at. */
    bad = tg_sony_craw(32, h, codes32, BAD_CURVE);
    CHECK_EQ_INT(decode_default_status(bad.d, bad.n, &e), PC_ERR_UNSUPPORTED);
    /* Truncated strip. */
    cut = copy_exact(ok.d, ok.n - 20);
    CHECK(decode_default_status(cut, ok.n - 20, &e) != PC_OK);
    free(cut);
    tg_free(&ok);
    tg_free(&bad);
    free(codes32);
}

static void test_rw2_round_trip_12_and_14_bit(void)
{
    static const struct {
        uint32_t bits;
        size_t w;
        size_t h;
    } CASES[2] = {{12, 120, 300}, {14, 117, 260}};
    static const float BLACK[4] = {128.0f, 129.0f, 129.0f, 130.0f};
    int k;
    for (k = 0; k < 2; k++) {
        uint32_t bits = CASES[k].bits;
        size_t w = CASES[k].w;
        size_t h = CASES[k].h;
        uint16_t max = (uint16_t)((1u << bits) - 1);
        uint16_t *data = scene_mosaic(w, h, RGGB, 128, max);
        tg_bytes b = tg_rw2(w, h, data, bits);
        pc_raw_format f;
        pc_raw_sensor s;
        CHECK(b.n > 3 * 0x4000); /* spans several pages */
        CHECK(pc_raw_identify(b.d, b.n, &f) && f == PC_RAW_RW2);
        if (decode_ok(b.d, b.n, &s)) {
            pc_raw_develop_options o = pc_raw_develop_options_default();
            pc_raw_developed d;
            CHECK(s.width == w && s.height == h);
            pc_t_checks++;
            if (!data_is(&s, data, w * h)) {
                PC_T_FAIL("%u-bit: data differs", bits);
            }
            CHECK(phase_is(&s, 0, 0, RGGB));
            CHECK(black_values_are(&s, BLACK, 4));
            CHECK(white_is(&s, (float)max));
            CHECK(camera_wb_is(&s, 2.0, 1.0, 1.5));
            CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(2, 2, w - 4, h - 4)));
            if (develop_sensor_ok(&s, &o, &d)) {
                CHECK((size_t)d.width == w - 4 && (size_t)d.height == h - 4);
                pc_raw_developed_fini(&d);
            }
            pc_raw_sensor_fini(&s);
        }
        tg_free(&b);
        free(data);
    }
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Sets the value of a SHORT entry of IFD0 in a little-endian TIFF-like file. */
static bool patch_ifd0_short(tg_bytes *b, uint16_t tag, uint16_t v)
{
    size_t ifd = rd32(b->d + 4);
    size_t n = rd16(b->d + ifd);
    size_t i;
    for (i = 0; i < n; i++) {
        size_t e = ifd + 2 + 12 * i;
        if (rd16(b->d + e) == tag) {
            b->d[e + 8] = (uint8_t)v;
            b->d[e + 9] = (uint8_t)(v >> 8);
            return true;
        }
    }
    PC_T_FAIL("tag %#x not found", tag);
    return false;
}

static void test_rw2_compressed_formats_fall_back(void)
{
    uint16_t *data = fill16(20 * 4, 200);
    tg_bytes b = tg_rw2(20, 4, data, 12);
    pc_raw_limits tight = pc_raw_limits_default();
    pc_error e;
    CHECK_EQ_INT(decode_default_status(b.d, b.n, &e), PC_OK);
    if (patch_ifd0_short(&b, 0x002D, 4)) {
        CHECK_EQ_INT(decode_default_status(b.d, b.n, &e), PC_ERR_UNSUPPORTED);
        CHECK_CONTAINS(e.msg, "raw format 4");
    }
    tg_free(&b);
    /* Width not made of whole blocks. */
    b = tg_rw2(20, 4, data, 12);
    if (patch_ifd0_short(&b, 0x0002, 19)) {
        CHECK_EQ_INT(decode_default_status(b.d, b.n, &e), PC_ERR_UNSUPPORTED);
    }
    tg_free(&b);
    /* Sizes beyond the limits are refused before allocating. */
    b = tg_rw2(20, 4, data, 12);
    if (patch_ifd0_short(&b, 0x0003, 60000)) {
        tight.max_pixels = 1u << 16;
        CHECK_EQ_INT(decode_status(b.d, b.n, &tight, &e), PC_ERR_LIMIT);
    }
    tg_free(&b);
    free(data);
}

static void test_orf_uncompressed_round_trip(void)
{
    size_t w = 40;
    size_t h = 24;
    static const uint8_t GRBG[4] = {1, 0, 2, 1};
    static const float BLACK[4] = {64.0f, 64.0f, 64.0f, 64.0f};
    uint16_t *data = scene_mosaic(w, h, GRBG, 64, 4095);
    tg_bytes b = tg_orf(w, h, data);
    pc_raw_format f;
    pc_raw_sensor s;
    pc_raw_preview p;
    CHECK(pc_raw_identify(b.d, b.n, &f) && f == PC_RAW_ORF);
    if (decode_ok(b.d, b.n, &s)) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        CHECK(data_is(&s, data, w * h)); /* left-justified samples are shifted back */
        CHECK(phase_is(&s, 0, 0, GRBG));
        CHECK(black_values_are(&s, BLACK, 4));
        CHECK(white_is(&s, 4095.0f));
        CHECK(camera_wb_is(&s, 2.0, 1.0, 1.5));
        CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(2, 2, w - 4, h - 4)));
        CHECK_EQ_UINT(s.warnings.n, 0);
        dump_warnings(&s.warnings);
        pc_t_checks++;
        if (develop_sensor_ok(&s, &o, &d)) {
            pc_raw_developed_fini(&d);
        }
        pc_raw_sensor_fini(&s);
    }
    /* The maker-note preview is found too (used when the data is compressed). */
    CHECK(pc_raw_embedded_preview(b.d, b.n, &p) && p.width == 16 && p.height == 8);
    tg_free(&b);
    free(data);
}

/* Position of IFD0's entry for tag (SIZE_MAX if absent). */
static size_t ifd0_entry(const tg_bytes *b, uint16_t tag)
{
    size_t ifd = rd32(b->d + 4);
    size_t n = rd16(b->d + ifd);
    size_t i;
    for (i = 0; i < n; i++) {
        size_t e = ifd + 2 + 12 * i;
        if (rd16(b->d + e) == tag) {
            return e;
        }
    }
    return SIZE_MAX;
}

/* A file cut short inside IFD0 keeps its width, height and compression entries but loses the
 * strip byte counts; their empty sum used to read as "fewer than 16 bits per sample" and the
 * damaged file was reported as the undecoded compressed variant. Damage is malformed, whichever
 * byte it lands on. */
static void test_orf_truncated_in_ifd0_is_malformed_not_the_compressed_variant(void)
{
    size_t w = 40;
    size_t h = 24;
    static const uint8_t GRBG[4] = {1, 0, 2, 1};
    uint16_t *data = scene_mosaic(w, h, GRBG, 64, 4095);
    tg_bytes b = tg_orf(w, h, data);
    size_t cut;
    size_t comp;
    pc_error e;
    CHECK_EQ_INT(decode_default_status(b.d, b.n, &e), PC_OK);
    /* Cut right before the StripByteCounts entry: width, height and compression survive. */
    cut = ifd0_entry(&b, 279);
    comp = ifd0_entry(&b, 259);
    CHECK(cut != SIZE_MAX && comp != SIZE_MAX);
    if (cut != SIZE_MAX && comp != SIZE_MAX) {
        uint8_t *c = copy_exact(b.d, cut);
        pc_status st;
        CHECK(cut > comp); /* compression precedes the byte counts */
        st = decode_default_status(c, cut, &e);
        CHECK_EQ_INT(st, PC_ERR_FORMAT);
        if (st != PC_ERR_FORMAT && st != PC_OK) {
            (void)fprintf(stderr, "    error: %s\n", e.msg);
        }
        free(c);
    }
    /* No prefix of the file reads as the compressed variant: the compression declaration is
     * intact or gone, never "compressed". */
    for (cut = 0; cut < b.n; cut++) {
        uint8_t *c = copy_exact(b.d, cut);
        if (decode_default_status(c, cut, &e) == PC_ERR_UNSUPPORTED && strstr(e.msg, "compressed") != NULL) {
            PC_T_FAIL("prefix %zu: %s", cut, e.msg);
        }
        free(c);
    }
    pc_t_checks++;
    tg_free(&b);
    free(data);
}

static void test_orf_packed_or_compressed_falls_back(void)
{
    size_t w = 40;
    size_t h = 24;
    uint16_t *data = fill16(w * h, 1000);
    tg_bytes b = tg_orf(w, h, data);
    /* Halve the declared strip size: no longer 16 bits per sample. */
    size_t ifd = rd32(b.d + 4);
    size_t n = rd16(b.d + ifd);
    size_t i;
    uint32_t v = (uint32_t)(w * h);
    pc_error e;
    for (i = 0; i < n; i++) {
        size_t en = ifd + 2 + 12 * i;
        if (rd16(b.d + en) == 279) {
            b.d[en + 8] = (uint8_t)v;
            b.d[en + 9] = (uint8_t)(v >> 8);
            b.d[en + 10] = (uint8_t)(v >> 16);
            b.d[en + 11] = (uint8_t)(v >> 24);
        }
    }
    CHECK_EQ_INT(decode_default_status(b.d, b.n, &e), PC_ERR_UNSUPPORTED);
    tg_free(&b);
    free(data);
}

/* ======================================================================================
 * vendors.rs: synthetic CR2 and TIFF/EP (NEF / ARW-like) files.
 * ====================================================================================== */

/* Left / top borders of 16 / 4 masked pixels at black level 512. spec.data is malloc'd. */
static tg_cr2 cr2_spec(size_t w, size_t h, size_t comps, const size_t *slices, size_t nslices)
{
    size_t l = 16;
    size_t t = 4;
    uint16_t *img = scene_mosaic(w - l, h - t, RGGB, 512, 13000);
    uint16_t *data = tg_xalloc(w * h * sizeof *data);
    size_t x;
    size_t y;
    tg_cr2 s;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            data[y * w + x] = y >= t && x >= l ? img[(y - t) * (w - l) + (x - l)] : 512;
        }
    }
    free(img);
    memset(&s, 0, sizeof s);
    s.width = w;
    s.height = h;
    s.data = data;
    s.precision = 14;
    s.components = comps;
    if (nslices > 0) {
        memcpy(s.slices, slices, nslices * sizeof *slices);
    }
    s.nslices = nslices;
    s.has_borders = true;
    s.borders[0] = (uint16_t)l;
    s.borders[1] = (uint16_t)t;
    s.borders[2] = (uint16_t)(w - 1);
    s.borders[3] = (uint16_t)(h - 1);
    s.has_wb = true;
    s.wb_rggb[0] = 2048;
    s.wb_rggb[1] = 1024;
    s.wb_rggb[2] = 1024;
    s.wb_rggb[3] = 1536;
    s.orientation = 1;
    return s;
}

static tg_bytes cr2_build(size_t w, size_t h, size_t comps, const size_t *slices, size_t nslices)
{
    tg_cr2 s = cr2_spec(w, h, comps, slices, nslices);
    tg_bytes b = tg_cr2_build(&s);
    free(s.data);
    return b;
}

static void test_cr2_slices_decode_exactly(void)
{
    static const struct {
        size_t comps;
        size_t slices[3];
        size_t n;
    } CASES[4] = {{2, {32, 32, 24}, 3}, {4, {40, 48, 0}, 2}, {2, {0, 0, 0}, 0}, {4, {88, 0, 0}, 1}};
    int k;
    for (k = 0; k < 4; k++) {
        tg_cr2 spec = cr2_spec(88, 30, CASES[k].comps, CASES[k].slices, CASES[k].n);
        tg_bytes b = tg_cr2_build(&spec);
        pc_raw_format f;
        pc_raw_sensor s;
        CHECK(pc_raw_identify(b.d, b.n, &f) && f == PC_RAW_CR2);
        if (decode_ok(b.d, b.n, &s)) {
            size_t i;
            bool black = true;
            pc_t_checks += 2;
            if (!(s.width == 88 && s.height == 30)) {
                PC_T_FAIL("case %d: %zux%zu", k, s.width, s.height);
            }
            if (!data_is(&s, spec.data, 88 * 30)) {
                PC_T_FAIL("case %d (%zu components, %zu slices): data differs", k, CASES[k].comps, CASES[k].n);
            }
            CHECK(pc_raw_rect_eq(s.active, pc_raw_rect_make(16, 4, 72, 26)));
            CHECK(camera_wb_is(&s, 2.0, 1.0, 1.5));
            for (i = 0; i < s.black.nvalues; i++) {
                black = black && fabsf(s.black.values[i] - 512.0f) < 1e-3f;
            }
            CHECK(black);
            CHECK(phase_is(&s, 16, 4, RGGB));
            pc_raw_sensor_fini(&s);
        }
        tg_free(&b);
        free(spec.data);
    }
}

static void test_cr2_develops(void)
{
    static const size_t SL[3] = {32, 32, 24};
    tg_bytes b = cr2_build(88, 30, 2, SL, 3);
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_raw_developed d;
    if (develop_ok(b.d, b.n, &o, &d)) {
        CHECK(d.width == 72 && d.height == 26);
        CHECK(d.info.format == PC_RAW_CR2);
        CHECK(d.info.wb_multipliers[0] == 2.0 && d.info.wb_multipliers[1] == 1.0 &&
              d.info.wb_multipliers[2] == 1.5);
        CHECK(pc_raw_strings_any(&d.warnings, "sRGB primaries"));
        pc_raw_developed_fini(&d);
    }
    tg_free(&b);
}

static void test_cr2_srgb_raw_is_unsupported(void)
{
    /* A subsampled (sRAW-style) frame: patch the first component's sampling factors. */
    tg_bytes b = cr2_build(32, 8, 2, NULL, 0);
    size_t sof = SIZE_MAX;
    size_t i;
    pc_error e;
    for (i = 0; i + 1 < b.n; i++) {
        if (b.d[i] == 0xFF && b.d[i + 1] == 0xC3) {
            sof = i;
            break;
        }
    }
    CHECK(sof != SIZE_MAX && sof + 11 < b.n);
    if (sof != SIZE_MAX && sof + 11 < b.n) {
        b.d[sof + 11] = 0x21;
        CHECK_EQ_INT(decode_default_status(b.d, b.n, &e), PC_ERR_UNSUPPORTED);
        CHECK_CONTAINS(e.msg, "sRAW");
    }
    tg_free(&b);
}

static void test_nef_like_with_maker_tags(void)
{
    size_t w = 24;
    size_t h = 16;
    static const uint8_t BGGR[4] = {2, 1, 1, 0};
    uint16_t *data = scene_mosaic(w, h, BGGR, 0, 4095);
    tg_bytes b = tg_tiff_ep("NIKON CORPORATION", w, h, data, BGGR, 12, NULL, 0);
    pc_raw_format f;
    pc_raw_sensor s;
    CHECK(pc_raw_identify(b.d, b.n, &f) && f == PC_RAW_NEF);
    if (decode_ok(b.d, b.n, &s)) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        CHECK(data_is(&s, data, w * h));
        CHECK(phase_is(&s, 0, 0, BGGR));
        CHECK(pc_raw_strings_any(&s.warnings, "black level"));
        pc_t_checks++;
        if (develop_sensor_ok(&s, &o, &d)) {
            pc_raw_developed_fini(&d);
        }
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(data);
}

static void test_arw_like_reads_sony_levels_and_crop(void)
{
    size_t w = 24;
    size_t h = 16;
    uint16_t *data = scene_mosaic(w, h, RGGB, 512, 16383);
    static const uint16_t BLACK16[4] = {512, 512, 512, 512};
    static const uint16_t WB[4] = {2048, 1024, 1024, 1536};
    static const uint32_t ORG[2] = {2, 2};
    static const uint32_t SZ[2] = {20, 12};
    static const float BLACK[4] = {512.0f, 512.0f, 512.0f, 512.0f};
    tg_entry extra[5];
    tg_bytes b;
    pc_raw_format f;
    pc_raw_sensor s;
    extra[0].tag = 0x7310;
    extra[0].val = tg_v_short(BLACK16, 4);
    extra[1].tag = 0x7313;
    extra[1].val = tg_v_short(WB, 4);
    extra[2].tag = 50717;
    extra[2].val = tg_v_short1(16383);
    extra[3].tag = 50719;
    extra[3].val = tg_v_long(ORG, 2);
    extra[4].tag = 50720;
    extra[4].val = tg_v_long(SZ, 2);
    b = tg_tiff_ep("SONY", w, h, data, RGGB, 14, extra, 5);
    CHECK(pc_raw_identify(b.d, b.n, &f) && f == PC_RAW_ARW);
    if (decode_ok(b.d, b.n, &s)) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        CHECK(black_values_are(&s, BLACK, 4));
        CHECK(white_is(&s, 16383.0f));
        CHECK(camera_wb_is(&s, 2.0, 1.0, 1.5));
        CHECK(pc_raw_rect_eq(s.crop, pc_raw_rect_make(2, 2, 20, 12)));
        CHECK_EQ_UINT(s.warnings.n, 0);
        dump_warnings(&s.warnings);
        if (develop_sensor_ok(&s, &o, &d)) {
            CHECK(d.width == 20 && d.height == 12);
            pc_raw_developed_fini(&d);
        }
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(data);
}

static void test_other_raw_containers_are_recognised_but_unsupported(void)
{
    uint8_t cr3[24] = {0, 0, 0, 24, 'f', 't', 'y', 'p', 'c', 'r', 'x', ' '};
    uint8_t raf[200];
    static const char RAF_MAGIC[] = "FUJIFILMCCD-RAW 0201FF383501";
    static const char NOT_RAW[] = "not a raw file";
    pc_raw_format f;
    pc_error e;
    CHECK(pc_raw_identify(cr3, sizeof cr3, &f) && f == PC_RAW_CR3);
    CHECK_EQ_INT(decode_default_status(cr3, sizeof cr3, &e), PC_ERR_UNSUPPORTED);
    memset(raf, 0, sizeof raf);
    memcpy(raf, RAF_MAGIC, sizeof RAF_MAGIC - 1);
    CHECK(pc_raw_identify(raf, sizeof raf, &f) && f == PC_RAW_RAF);
    CHECK_EQ_INT(decode_default_status(raf, sizeof raf, &e), PC_ERR_UNSUPPORTED);
    CHECK_EQ_INT(decode_default_status((const uint8_t *)NOT_RAW, sizeof NOT_RAW - 1, &e), PC_ERR_NOT_FOUND);
    CHECK_EQ_STR(e.msg, "not a camera raw file");
}

static void test_embedded_preview_is_found(void)
{
    /* A RAF header pointing at a minimal baseline JPEG (SOI, SOF0, EOI). */
    static const uint8_t JPEG[17] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 0x20,
                                     0x00, 0x30, 1,    1,    0x11, 0,    0xFF, 0xD9};
    static const char RAF_MAGIC[] = "FUJIFILMCCD-RAW 0201FF383501";
    uint8_t raf[100 + sizeof JPEG];
    pc_raw_preview p;
    tg_bytes cr2;
    memset(raf, 0, sizeof raf);
    memcpy(raf, RAF_MAGIC, sizeof RAF_MAGIC - 1);
    raf[84] = 0;
    raf[85] = 0;
    raf[86] = 0;
    raf[87] = 100;
    raf[88] = 0;
    raf[89] = 0;
    raf[90] = 0;
    raf[91] = (uint8_t)sizeof JPEG;
    memcpy(raf + 100, JPEG, sizeof JPEG);
    if (pc_raw_embedded_preview(raf, sizeof raf, &p)) {
        CHECK(p.width == 0x30 && p.height == 0x20);
        CHECK(p.len == sizeof JPEG && memcmp(p.jpeg, JPEG, sizeof JPEG) == 0);
    } else {
        PC_T_FAIL("no preview found");
    }
    /* The lossless-JPEG raw data of a CR2 is not mistaken for a preview. */
    cr2 = cr2_build(32, 8, 2, NULL, 0);
    CHECK(!pc_raw_embedded_preview(cr2.d, cr2.n, &p));
    tg_free(&cr2);
}

/* #193: a CR2 whose sensor data is mosaicked with `cfa` (row-major 2x2 colours) anchored at data
 * (0, 0), image area from (l, t). Left half: a colourful scene; right half: a neutral grey patch.
 * Raw values are scene / white-balance gain, as a camera records them. `flat` sets every sample
 * to black. */
static tg_bytes cr2_phase_build(const uint8_t cfa[4], size_t l, size_t t, bool has_id, uint32_t id, bool flat)
{
    size_t w = 192;
    size_t h = 96;
    float(*col)[3] = tg_scene(w, h);
    float(*rgb)[3] = tg_xalloc(w * h * sizeof *rgb);
    static const float WB[3] = {2.0f, 1.0f, 1.5f};
    uint16_t *data;
    size_t i;
    size_t x;
    size_t y;
    tg_cr2 s;
    tg_bytes b;
    for (i = 0; i < w * h; i++) {
        int k;
        for (k = 0; k < 3; k++) {
            float p = i % w >= w / 2 ? 0.4f : col[i][k];
            rgb[i][k] = p / WB[k];
        }
    }
    data = mosaic_of((const float(*)[3])rgb, w * h, w, cfa, 512, 13000);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (x < l || y < t || flat) {
                data[y * w + x] = 512;
            }
        }
    }
    memset(&s, 0, sizeof s);
    s.width = w;
    s.height = h;
    s.data = data;
    s.precision = 14;
    s.components = 2;
    s.slices[0] = 96;
    s.slices[1] = 96;
    s.nslices = 2;
    s.has_borders = true;
    s.borders[0] = (uint16_t)l;
    s.borders[1] = (uint16_t)t;
    s.borders[2] = (uint16_t)(w - 1);
    s.borders[3] = (uint16_t)(h - 1);
    s.has_wb = true;
    s.wb_rggb[0] = 2048;
    s.wb_rggb[1] = 1024;
    s.wb_rggb[2] = 1024;
    s.wb_rggb[3] = 1536;
    s.orientation = 1;
    s.has_model_id = has_id;
    s.model_id = id;
    b = tg_cr2_build(&s);
    free(col);
    free(rgb);
    free(data);
    return b;
}

/* Mean developed RGB over the interior of the grey patch (from column x0 of the image area). */
static void grey_patch(const pc_raw_developed *d, size_t x0, double out[3])
{
    size_t w = d->width;
    size_t h = d->height;
    double sum[3] = {0.0, 0.0, 0.0};
    double n = 0.0;
    size_t x;
    size_t y;
    int c;
    for (y = 8; y + 8 < h; y++) {
        for (x = x0 + 8; x + 8 < w; x++) {
            for (c = 0; c < 3; c++) {
                sum[c] += (double)d->rgb[(y * w + x) * 3 + (size_t)c];
            }
            n += 1.0;
        }
    }
    for (c = 0; c < 3; c++) {
        out[c] = sum[c] / n;
    }
}

static void test_cr2_cfa_phase_is_measured_for_every_row_phase_and_border_parity(void)
{
    /* Canon data has red in the even columns; the red rows are even (RGGB at the data origin:
     * 40D, 5D Mark III, ...) or odd (GBRG: 7D, 550D, 5D Mark II, ...). Odd image-area borders used
     * to shift the assumed pattern and swap green with red/blue. */
    static const uint8_t CFAS[2][4] = {{0, 1, 1, 2}, {1, 2, 0, 1}};
    static const size_t LT[4][2] = {{16, 4}, {17, 4}, {16, 5}, {17, 5}};
    int ci;
    int k;
    for (ci = 0; ci < 2; ci++) {
        for (k = 0; k < 4; k++) {
            size_t l = LT[k][0];
            size_t t = LT[k][1];
            tg_bytes b = cr2_phase_build(CFAS[ci], l, t, false, 0, false);
            pc_raw_sensor s;
            if (decode_ok(b.d, b.n, &s)) {
                pc_raw_develop_options o = pc_raw_develop_options_default();
                pc_raw_developed d;
                pc_t_checks++;
                if (!phase_is(&s, 0, 0, CFAS[ci])) {
                    PC_T_FAIL("cfa %d borders (%zu, %zu): wrong phase", ci, l, t);
                }
                CHECK_EQ_UINT(s.warnings.n, 0);
                dump_warnings(&s.warnings);
                o.orient = false;
                if (develop_sensor_ok(&s, &o, &d)) {
                    double m[3];
                    grey_patch(&d, 192 / 2 - l, m);
                    /* Neutral grey decodes neutral (a swapped phase gives a strong magenta or
                     * green cast). */
                    pc_t_checks++;
                    if (!(fabs(m[0] / m[1] - 1.0) < 0.02 && fabs(m[2] / m[1] - 1.0) < 0.02)) {
                        PC_T_FAIL("cfa %d borders (%zu, %zu): grey patch %.0f %.0f %.0f", ci, l, t, m[0], m[1],
                                  m[2]);
                    }
                    pc_raw_developed_fini(&d);
                }
                pc_raw_sensor_fini(&s);
            }
            tg_free(&b);
        }
    }
}

static bool flat_cr2(bool has_id, uint32_t id, pc_raw_sensor *s)
{
    tg_bytes b = cr2_phase_build(RGGB, 16, 4, has_id, id, true);
    bool ok = decode_ok(b.d, b.n, s);
    tg_free(&b);
    return ok;
}

static void test_cr2_cfa_phase_falls_back_to_the_model_table(void)
{
    /* A flat black frame gives the measurement nothing to go on. */
    static const uint8_t GBRG[4] = {1, 2, 0, 1};
    pc_raw_sensor s;
    /* EOS 7D (Canon model ID 0x80000250): green/blue first. */
    if (flat_cr2(true, 0x80000250u, &s)) {
        CHECK(phase_is(&s, 0, 0, GBRG));
        CHECK(!pc_raw_strings_any(&s.warnings, "colour filter phase"));
        pc_raw_sensor_fini(&s);
    }
    /* Unknown model: red/green first, with a warning. */
    if (flat_cr2(true, 0x80000001u, &s)) {
        CHECK(phase_is(&s, 0, 0, RGGB));
        CHECK(pc_raw_strings_any(&s.warnings, "colour filter phase"));
        pc_raw_sensor_fini(&s);
    }
    if (flat_cr2(false, 0, &s)) {
        CHECK(phase_is(&s, 0, 0, RGGB));
        pc_raw_sensor_fini(&s);
    }
}

int main(int argc, char **argv)
{
    /* dng.rs */
    RUN(test_identifies_dng);
    RUN(test_storage_layouts_decode_exactly);
    RUN(test_linear_raw_decodes_and_develops);
    RUN(test_metadata_is_read);
    RUN(test_enormous_double_crop_origin_is_rejected);
    RUN(test_out_of_range_default_crop_falls_back_to_active_area_with_warning);
    RUN(test_default_crop_origin_without_size_falls_back_to_active_area_with_warning);
    RUN(test_default_crop_size_without_origin_crops_from_the_active_area_origin);
    RUN(test_colour_matrix_development_matches_reference);
    RUN(test_forward_matrix_path_maps_neutral_to_white);
    RUN(test_highlights_clip_to_white_and_exposure_applies);
    RUN(test_gain_map_opcode_is_applied);
    RUN(test_white_balance_options);
    RUN(test_smooth_scene_develops_with_every_method);
    RUN(test_unsupported_variants_say_so);
    RUN(test_limits_are_enforced_before_allocating);
    /* hostile.rs */
    RUN(test_originals_develop);
    RUN(test_every_truncation_fails_cleanly);
    RUN(test_random_corruption_never_panics);
    RUN(test_absurd_dimensions_are_rejected_without_allocating);
    /* vendor_codecs.rs */
    RUN(test_craw_round_trip_is_exact_on_smooth_data);
    RUN(test_craw_wide_ranges_lose_only_low_bits);
    RUN(test_craw_block_layout);
    RUN(test_craw_unusual_variants_are_unsupported);
    RUN(test_rw2_round_trip_12_and_14_bit);
    RUN(test_rw2_compressed_formats_fall_back);
    RUN(test_orf_uncompressed_round_trip);
    RUN(test_orf_truncated_in_ifd0_is_malformed_not_the_compressed_variant);
    RUN(test_orf_packed_or_compressed_falls_back);
    /* vendors.rs */
    RUN(test_cr2_slices_decode_exactly);
    RUN(test_cr2_develops);
    RUN(test_cr2_srgb_raw_is_unsupported);
    RUN(test_nef_like_with_maker_tags);
    RUN(test_arw_like_reads_sony_levels_and_crop);
    RUN(test_other_raw_containers_are_recognised_but_unsupported);
    RUN(test_embedded_preview_is_found);
    RUN(test_cr2_cfa_phase_is_measured_for_every_row_phase_and_border_parity);
    RUN(test_cr2_cfa_phase_falls_back_to_the_model_table);
    return pc_t_finish();
}
