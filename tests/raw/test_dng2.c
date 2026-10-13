/* DNG features added upstream after the F14-F16 port (crates/raw/tests/dng.rs, hostile.rs,
 * vendor_codecs.rs, vendors.rs as of the R3 resync): monochrome LinearRaw, LONG orientations,
 * deflate (compression 8) with predictors, JPEG XL (52546, via libjxl), lossy JPEG tiles (34892),
 * ProfileGainTableMap, the lossless-JPEG intermediate budget, cRAW without a BlackLevel tag, and
 * no crash on the new hostile samples. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "raw/raw.h"
#include "testgen.h"
#include "testgen_raf.h"

#ifndef PC_NO_JXL
#include <jxl/encode.h>
#endif

static const uint8_t RGGB[4] = {0, 1, 1, 2};

static bool decode_ok(const tg_bytes *b, pc_raw_sensor *s)
{
    pc_raw_limits lim = pc_raw_limits_default();
    pc_error e;
    if (pc_raw_decode(b->d, b->n, &lim, s, &e) != PC_OK) {
        PC_T_FAIL("decode: %s", e.msg);
        return false;
    }
    return true;
}

static uint16_t *ramp(size_t w, size_t h, uint16_t lo, uint16_t hi)
{
    uint16_t *v = tg_xalloc(w * h * sizeof *v);
    size_t i;
    for (i = 0; i < w * h; i++) {
        v[i] = (uint16_t)(lo + (uint32_t)(hi - lo) * (uint32_t)(i % w) / (uint32_t)(w - 1));
    }
    return v;
}

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

/* Every pixel R = G = B, the first row rising by more than 10000; returns the first row's ends. */
static void neutral_ramp(const pc_raw_developed *d, const char *name, uint16_t *first, uint16_t *last)
{
    size_t i;
    bool equal = true;
    bool mono = true;
    for (i = 0; i < (size_t)d->width * d->height; i++) {
        equal = equal && d->rgb[i * 3] == d->rgb[i * 3 + 1] && d->rgb[i * 3 + 1] == d->rgb[i * 3 + 2];
    }
    for (i = 1; i < d->width; i++) {
        mono = mono && d->rgb[i * 3] >= d->rgb[(i - 1) * 3];
    }
    if (!equal || !mono) {
        PC_T_FAIL("%s: channels differ or not monotonic", name);
    }
    CHECK(equal && mono);
    *first = d->rgb[0];
    *last = d->rgb[(d->width - 1) * 3];
    CHECK(*last > *first + 10000);
}

static void test_monochrome_linear_raw_develops_to_neutral_grey(void)
{
    static const struct {
        const char *name;
        uint16_t bits;
        tg_storage st;
        size_t rows, tw, th;
    } C[7] = {{"16-bit strips", 16, TG_STRIPS, 5, 0, 0},
              {"12-bit packed", 12, TG_STRIPS, 4, 0, 0},
              {"8-bit", 8, TG_STRIPS, 23, 0, 0},
              {"LJ92 strips", 14, TG_LJ92_STRIPS, 6, 0, 0},
              {"LJ92 tiles", 16, TG_LJ92_TILES, 0, 16, 8},
              {"deflate predictor 2", 16, TG_DEFLATE_STRIPS, 7, 0, 0},
              {"lossy JPEG tile", 16, TG_LOSSY_TILES, 0, 37, 23}};
    size_t i;
    for (i = 0; i < 7; i++) {
        uint16_t white = (uint16_t)((1u << C[i].bits) - 1);
        uint16_t *data = ramp(37, 23, 0, white);
        tg_dng spec = tg_dng_mono(37, 23, data);
        bool lossy = C[i].st == TG_LOSSY_TILES;
        tg_bytes b;
        pc_raw_sensor s;
        pc_raw_developed d;
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_error e;
        uint16_t lo;
        uint16_t hi;
        spec.bits = C[i].bits;
        spec.white = white;
        spec.storage = C[i].st;
        spec.rows = C[i].rows;
        spec.tile_w = C[i].tw;
        spec.tile_h = C[i].th;
        spec.predictor = 2;
        b = tg_dng_build(&spec);
        if (decode_ok(&b, &s)) {
            CHECK(s.samples == 1 && !s.has_cfa && pc_raw_sensor_is_monochrome(&s));
            if (!lossy) {
                CHECK_EQ_MEM(s.data, data, 37 * 23 * sizeof *data);
            }
            if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
                CHECK(d.width == 37 && d.height == 23 && !d.info.has_cfa);
                CHECK(d.info.wb_multipliers[0] == 1.0 && d.info.wb_multipliers[1] == 1.0 &&
                      d.info.wb_multipliers[2] == 1.0);
                CHECK(!pc_raw_strings_any(&d.warnings, "colour") &&
                      !pc_raw_strings_any(&d.warnings, "white balance"));
                neutral_ramp(&d, C[i].name, &lo, &hi);
                if (!lossy) {
                    CHECK_EQ_UINT(lo, 0);
                    CHECK_EQ_UINT(hi, 65535);
                }
                pc_raw_developed_fini(&d);
            } else {
                PC_T_FAIL("%s: %s", C[i].name, e.msg);
            }
            pc_raw_sensor_fini(&s);
        }
        tg_free(&b);
        free(data);
    }
}

static void test_monochrome_levels_and_white_balance(void)
{
    uint16_t *data = ramp(32, 4, 1000, 60000);
    tg_dng spec = tg_dng_mono(32, 4, data);
    tg_bytes b;
    pc_raw_sensor s;
    int k;
    spec.black[0] = 1000;
    spec.white = 60000;
    b = tg_dng_build(&spec);
    if (decode_ok(&b, &s)) {
        for (k = 0; k < 4; k++) {
            pc_raw_develop_options o = pc_raw_develop_options_default();
            pc_raw_developed d;
            pc_error e;
            uint16_t lo;
            uint16_t hi;
            if (k == 1) {
                o.white_balance.kind = PC_RAW_WB_AUTO;
            } else if (k == 2) {
                o.white_balance.kind = PC_RAW_WB_MULTIPLIERS;
                o.white_balance.multipliers[0] = 2.0;
                o.white_balance.multipliers[1] = 1.0;
                o.white_balance.multipliers[2] = 1.5;
            } else if (k == 3) {
                o.exposure = -1.0;
            }
            if (pc_raw_develop_sensor(&s, &o, &d, &e) != PC_OK) {
                PC_T_FAIL("%s", e.msg);
                continue;
            }
            neutral_ramp(&d, "levels", &lo, &hi);
            if (k < 3) {
                CHECK_EQ_UINT(lo, 0);
                CHECK_EQ_UINT(hi, 65535);
            } else {
                CHECK(hi < 60000);
            }
            pc_raw_developed_fini(&d);
        }
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(data);
}

static void test_broken_monochrome_linear_raw_errors(void)
{
    uint16_t *data = ramp(16, 6, 100, 4000);
    uint16_t *two = ramp(32, 6, 100, 4000);
    tg_dng spec = tg_dng_mono(16, 6, data);
    tg_bytes b = tg_dng_build(&spec);
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_raw_developed d;
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_error e;
    size_t n;
    size_t at;
    uint8_t first[8];
    for (n = 0; n < b.n; n++) {
        if (pc_raw_decode(b.d, n, &lim, &s, &e) == PC_OK) {
            if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
                pc_raw_developed_fini(&d);
            }
            pc_raw_sensor_fini(&s);
        }
    }
    for (n = 0; n < 4; n++) {
        first[2 * n] = (uint8_t)data[n];
        first[2 * n + 1] = (uint8_t)(data[n] >> 8);
    }
    for (at = 0; at + 8 <= b.n && memcmp(b.d + at, first, 8) != 0; at++) {
    }
    CHECK(at + 8 <= b.n);
    CHECK(pc_raw_decode(b.d, at + 16, &lim, &s, &e) != PC_OK);
    tg_free(&b);
    spec.samples = 2;
    spec.data = two;
    b = tg_dng_build(&spec);
    CHECK(pc_raw_decode(b.d, b.n, &lim, &s, &e) != PC_OK);
    CHECK_CONTAINS(e.msg, "2 samples per pixel");
    tg_free(&b);
    spec.samples = 1;
    spec.data = data;
    b = tg_dng_build(&spec);
    if (decode_ok(&b, &s)) {
        s.ndata = 10;
        CHECK(pc_raw_develop_sensor(&s, &o, &d, &e) != PC_OK);
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    free(data);
    free(two);
}

/* spec's file with its Orientation entry (a SHORT from) rewritten as a LONG to. */
static uint16_t long_orientation(uint32_t to)
{
    uint16_t v[48];
    tg_dng spec;
    tg_bytes b;
    static const uint8_t SHORT[12] = {0x12, 0x01, 3, 0, 1, 0, 0, 0, 2, 0, 0, 0};
    size_t at;
    pc_raw_sensor s;
    uint16_t o = 0;
    size_t i;
    for (i = 0; i < 48; i++) {
        v[i] = 500;
    }
    spec = tg_dng_cfa(8, 6, v);
    spec.orientation = 2;
    b = tg_dng_build(&spec);
    for (at = 0; at + 12 <= b.n && memcmp(b.d + at, SHORT, 12) != 0; at++) {
    }
    if (at + 12 <= b.n) {
        b.d[at + 2] = 4;
        for (i = 0; i < 4; i++) {
            b.d[at + 8 + i] = (uint8_t)(to >> (8 * i));
        }
        if (decode_ok(&b, &s)) {
            o = s.orientation;
            pc_raw_sensor_fini(&s);
        }
    }
    tg_free(&b);
    return o;
}

static void test_an_orientation_past_u16_is_not_truncated_into_range(void)
{
    CHECK_EQ_UINT(long_orientation(65538), 1);
    CHECK_EQ_UINT(long_orientation(6), 6);
}

static void test_deflate_strips_round_trip(void)
{
    static const struct {
        uint16_t bits;
        size_t rows;
        uint16_t pred;
        bool be;
        size_t samples;
    } C[5] = {{16, 16, 1, false, 1},
              {16, 16, 2, false, 1},
              {12, 12, 2, false, 1},
              {16, 16, 2, true, 1},
              {16, 40, 2, false, 3}};
    size_t i;
    for (i = 0; i < 5; i++) {
        uint16_t *data = noise(48 * 40 * C[i].samples, C[i].bits);
        tg_dng spec = tg_dng_cfa(48, 40, data);
        tg_bytes b;
        pc_raw_sensor s;
        spec.samples = C[i].samples;
        spec.bits = C[i].bits;
        spec.storage = TG_DEFLATE_STRIPS;
        spec.rows = C[i].rows;
        spec.predictor = C[i].pred;
        spec.big_endian = C[i].be;
        spec.white = (1u << C[i].bits) - 1;
        b = tg_dng_build(&spec);
        if (decode_ok(&b, &s)) {
            CHECK(s.width == 48 && s.height == 40 && s.samples == C[i].samples);
            CHECK_EQ_MEM(s.data, data, 48 * 40 * C[i].samples * sizeof *data);
            pc_raw_sensor_fini(&s);
        }
        tg_free(&b);
        free(data);
    }
}

static void test_deflate_hostile_fails_cleanly(void)
{
    uint16_t *data = noise(32 * 32, 16);
    tg_dng spec = tg_dng_cfa(32, 32, data);
    tg_bytes b;
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_error e;
    size_t at;
    spec.storage = TG_DEFLATE_STRIPS;
    spec.rows = 32;
    spec.predictor = 2;
    b = tg_dng_build(&spec);
    for (at = 0; at + 2 <= b.n && !(b.d[at] == 0x78 && b.d[at + 1] == 0x9c); at++) {
    }
    CHECK(at + 2 <= b.n);
    at += 2;
    CHECK(pc_raw_decode(b.d, at + (b.n - at) / 2, &lim, &s, &e) != PC_OK);
    /* A corrupt stream (every byte after the header flipped) fails, never crashes. */
    {
        size_t i;
        for (i = at; i < b.n; i++) {
            b.d[i] ^= 0x5A;
        }
        if (pc_raw_decode(b.d, b.n, &lim, &s, &e) == PC_OK) {
            pc_raw_sensor_fini(&s);
        }
    }
    tg_free(&b);
    free(data);
}

static void test_deflate_8_bit_predictor_and_packed_12_bit(void)
{
    size_t w = 16;
    size_t h = 4;
    uint16_t d8[64];
    uint16_t d12[64];
    tg_dng spec;
    tg_bytes b;
    pc_raw_sensor s;
    size_t i;
    for (i = 0; i < w * h; i++) {
        d8[i] = i % w < w / 2 ? 250 : 10;
        d12[i] = (uint16_t)((i * 61) & 0xFFF);
    }
    spec = tg_dng_cfa(w, h, d8);
    spec.bits = 8;
    spec.white = 255;
    spec.storage = TG_DEFLATE_STRIPS;
    spec.rows = h;
    spec.predictor = 2;
    b = tg_dng_build(&spec);
    if (decode_ok(&b, &s)) {
        CHECK_EQ_MEM(s.data, d8, sizeof d8);
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    /* 12-bit packed inside the zlib stream: swap the 16-bit containers for a packed stream. */
    spec = tg_dng_cfa(w, h, d12);
    spec.bits = 12;
    spec.white = 4095;
    spec.storage = TG_DEFLATE_STRIPS;
    spec.rows = h;
    spec.predictor = 1;
    b = tg_dng_build(&spec);
    {
        uint8_t cont[128];
        uint8_t packed[96];
        size_t ol;
        size_t nl;
        uint8_t *old;
        uint8_t *nw;
        size_t at;
        for (i = 0; i < 64; i++) {
            cont[2 * i] = (uint8_t)d12[i];
            cont[2 * i + 1] = (uint8_t)(d12[i] >> 8);
        }
        for (i = 0; i < 32; i++) {
            uint16_t p0 = d12[2 * i];
            uint16_t p1 = d12[2 * i + 1];
            packed[3 * i] = (uint8_t)(p0 >> 4);
            packed[3 * i + 1] = (uint8_t)((p0 & 0xF) << 4 | p1 >> 8);
            packed[3 * i + 2] = (uint8_t)p1;
        }
        old = tg_zlib(cont, sizeof cont, &ol);
        nw = tg_zlib(packed, sizeof packed, &nl);
        for (at = 0; at + ol <= b.n && memcmp(b.d + at, old, ol) != 0; at++) {
        }
        CHECK(at + ol <= b.n && nl <= ol);
        if (at + ol <= b.n && nl <= ol) {
            memset(b.d + at, 0, ol);
            memcpy(b.d + at, nw, nl);
            if (decode_ok(&b, &s)) {
                CHECK_EQ_MEM(s.data, d12, sizeof d12);
                pc_raw_sensor_fini(&s);
            }
        }
        free(old);
        free(nw);
    }
    tg_free(&b);
}

/* ---- JPEG XL ---- */

#ifndef PC_NO_JXL
/* Losslessly encodes w x h samples (1 = grey, 3 = RGB) at 8 or 16 bits with libjxl. */
static tg_bytes jxl(const uint16_t *v, size_t w, size_t h, size_t ch, uint16_t bits)
{
    tg_bytes out = {NULL, 0, 0};
    JxlEncoder *enc = JxlEncoderCreate(NULL);
    JxlBasicInfo info;
    JxlColorEncoding ce;
    JxlPixelFormat fmt = {(uint32_t)ch, bits == 8 ? JXL_TYPE_UINT8 : JXL_TYPE_UINT16, JXL_NATIVE_ENDIAN, 0};
    JxlEncoderFrameSettings *fs;
    size_t n = w * h * ch;
    void *buf;
    JxlEncoderStatus st;
    size_t i;
    if (enc == NULL) {
        abort();
    }
    JxlEncoderInitBasicInfo(&info);
    info.xsize = (uint32_t)w;
    info.ysize = (uint32_t)h;
    info.bits_per_sample = bits;
    info.num_color_channels = (uint32_t)ch;
    info.uses_original_profile = JXL_TRUE;
    if (JxlEncoderSetBasicInfo(enc, &info) != JXL_ENC_SUCCESS) {
        abort();
    }
    JxlColorEncodingSetToSRGB(&ce, ch == 1 ? JXL_TRUE : JXL_FALSE);
    (void)JxlEncoderSetColorEncoding(enc, &ce);
    fs = JxlEncoderFrameSettingsCreate(enc, NULL);
    (void)JxlEncoderSetFrameLossless(fs, JXL_TRUE);
    if (bits == 8) {
        uint8_t *b8 = tg_xalloc(n);
        for (i = 0; i < n; i++) {
            b8[i] = (uint8_t)v[i];
        }
        buf = b8;
        st = JxlEncoderAddImageFrame(fs, &fmt, buf, n);
    } else {
        uint16_t *b16 = tg_xalloc(n * 2);
        memcpy(b16, v, n * 2);
        buf = b16;
        st = JxlEncoderAddImageFrame(fs, &fmt, buf, n * 2);
    }
    if (st != JXL_ENC_SUCCESS) {
        abort();
    }
    JxlEncoderCloseInput(enc);
    for (;;) {
        uint8_t chunk[4096];
        uint8_t *next = chunk;
        size_t avail = sizeof chunk;
        st = JxlEncoderProcessOutput(enc, &next, &avail);
        tg_put(&out, chunk, sizeof chunk - avail);
        if (st != JXL_ENC_NEED_MORE_OUTPUT) {
            break;
        }
    }
    if (st != JXL_ENC_SUCCESS) {
        abort();
    }
    free(buf);
    JxlEncoderDestroy(enc);
    return out;
}

static void test_jpeg_xl_layouts_decode_exactly(void)
{
    static const struct {
        size_t spp;
        uint16_t bits;
        bool tiled;
    } C[6] = {{3, 16, false}, {3, 16, true}, {1, 16, false}, {1, 16, true}, {3, 8, false}, {1, 8, true}};
    size_t w = 37;
    size_t h = 23;
    size_t i;
    for (i = 0; i < 6; i++) {
        uint16_t *data = noise(w * h * C[i].spp, C[i].bits);
        tg_dng spec = tg_dng_cfa(w, h, data);
        tg_bytes segs[16];
        size_t nseg = 0;
        tg_bytes b;
        pc_raw_sensor s;
        size_t k;
        spec.samples = C[i].spp;
        spec.bits = C[i].bits;
        spec.white = (1u << C[i].bits) - 1;
        if (C[i].tiled) {
            size_t ty;
            size_t tx;
            for (ty = 0; ty < h; ty += 16) {
                for (tx = 0; tx < w; tx += 16) {
                    uint16_t *t = tg_xalloc(16 * 16 * C[i].spp * sizeof *t);
                    size_t y;
                    size_t x;
                    for (y = 0; y < 16; y++) {
                        for (x = 0; x < 16; x++) {
                            size_t sy = ty + y < h - 1 ? ty + y : h - 1;
                            size_t sx = tx + x < w - 1 ? tx + x : w - 1;
                            memcpy(t + (y * 16 + x) * C[i].spp, data + (sy * w + sx) * C[i].spp,
                                   C[i].spp * sizeof *t);
                        }
                    }
                    segs[nseg++] = jxl(t, 16, 16, C[i].spp, C[i].bits);
                    free(t);
                }
            }
            spec.storage = TG_JXL_TILES;
            spec.tile_w = 16;
            spec.tile_h = 16;
        } else {
            size_t y;
            for (y = 0; y < h; y += 10) {
                size_t r = y + 10 < h ? 10 : h - y;
                segs[nseg++] = jxl(data + y * w * C[i].spp, w, r, C[i].spp, C[i].bits);
            }
            spec.storage = TG_JXL_STRIPS;
            spec.rows = 10;
        }
        spec.segments = segs;
        spec.nsegments = nseg;
        b = tg_dng_build(&spec);
        if (decode_ok(&b, &s)) {
            CHECK(s.width == w && s.height == h && s.samples == C[i].spp);
            CHECK_EQ_MEM(s.data, data, w * h * C[i].spp * sizeof *data);
            pc_raw_sensor_fini(&s);
        }
        tg_free(&b);
        for (k = 0; k < nseg; k++) {
            tg_free(&segs[k]);
        }
        free(data);
    }
}
#endif

/* JPEG XL fixtures (tests/fixtures/raw, encoded by the reference's zune-jpegxl): exact with libjxl,
 * the reference's "unsupported" text without it. */
static void test_jpeg_xl_fixtures(void)
{
    static const char *const NAMES[8] = {"jxl0", "jxl1", "jxl2", "jxl3", "jxl4", "jxl5", "jxl_dev", "h_jxl"};
    size_t i;
    for (i = 0; i < 8; i++) {
        char path[128];
        FILE *f;
        tg_bytes b = {NULL, 0, 0};
        uint8_t buf[4096];
        size_t n;
        pc_raw_limits lim = pc_raw_limits_default();
        pc_raw_sensor s;
        pc_error e;
        pc_status st;
        (void)snprintf(path, sizeof path, "tests/fixtures/raw/%s.bin", NAMES[i]);
        f = fopen(path, "rb");
        if (f == NULL) {
            PC_T_FAIL("missing %s", path);
            continue;
        }
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
            tg_put(&b, buf, n);
        }
        (void)fclose(f);
        st = pc_raw_decode(b.d, b.n, &lim, &s, &e);
#ifdef PC_NO_JXL
        CHECK_EQ_INT(st, PC_ERR_UNSUPPORTED);
        CHECK_CONTAINS(e.msg, "JPEG XL-compressed DNG (DNG 1.7) can't be opened");
#else
        CHECK_EQ_INT(st, PC_OK);
        if (st == PC_OK) {
            pc_raw_sensor_fini(&s);
        }
#endif
        /* Truncations never crash. */
        for (n = 0; n < b.n; n += 7) {
            if (pc_raw_decode(b.d, n, &lim, &s, &e) == PC_OK) {
                pc_raw_sensor_fini(&s);
            }
        }
        tg_free(&b);
    }
}

static void test_profile_gain_table_map_is_reported(void)
{
    uint16_t v[64];
    static const uint8_t ZERO[64] = {0};
    tg_dng spec;
    tg_bytes b;
    pc_raw_sensor s;
    size_t i;
    for (i = 0; i < 64; i++) {
        v[i] = 500;
    }
    spec = tg_dng_cfa(8, 8, v);
    b = tg_dng_build(&spec);
    if (decode_ok(&b, &s)) {
        CHECK(!pc_raw_strings_any(&s.warnings, "ProfileGainTableMap"));
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    spec.pgtm = ZERO;
    spec.npgtm = 64;
    b = tg_dng_build(&spec);
    if (decode_ok(&b, &s)) {
        CHECK(pc_raw_strings_any(&s.warnings, "ProfileGainTableMap"));
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
}

/* ---- lossy JPEG tiles ---- */

static unsigned worst_vs_quantised(const pc_raw_sensor *s, const uint16_t *want, size_t n)
{
    unsigned worst = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        unsigned q = (unsigned)(want[i] >> 8) << 8;
        unsigned d = s->data[i] > q ? s->data[i] - q : q - s->data[i];
        worst = d > worst ? d : worst;
    }
    return worst;
}

static void test_lossy_jpeg_tiles(void)
{
    uint16_t data[32 * 32];
    uint16_t rgb[32 * 32 * 3];
    tg_dng spec;
    tg_bytes b;
    pc_raw_sensor s;
    pc_raw_limits lim = pc_raw_limits_default();
    pc_error e;
    size_t i;
    for (i = 0; i < 32 * 32; i++) {
        data[i] = (uint16_t)(4096 + (i / 32) * 16);
    }
    spec = tg_dng_cfa(32, 32, data);
    spec.storage = TG_LOSSY_TILES;
    spec.tile_w = 16;
    spec.tile_h = 16;
    b = tg_dng_build(&spec);
    if (decode_ok(&b, &s)) {
        CHECK(s.width == 32 && s.height == 32 && s.samples == 1);
        CHECK(worst_vs_quantised(&s, data, 32 * 32) <= 4 * 256);
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    /* A tile cut mid-stream fails cleanly. */
    spec.tile_w = 32;
    spec.tile_h = 32;
    b = tg_dng_build(&spec);
    for (i = 0; i + 2 <= b.n && !(b.d[i] == 0xFF && b.d[i + 1] == 0xD8); i++) {
    }
    CHECK(pc_raw_decode(b.d, i + 2 + (b.n - i - 2) / 2, &lim, &s, &e) != PC_OK);
    /* A tile whose header lies about its size: the size check comes before any pixel decode. */
    {
        uint8_t px[16 * 16];
        size_t jl;
        uint8_t *j;
        size_t soi = i;
        size_t eoi;
        tg_bytes nb = {NULL, 0, 0};
        for (eoi = soi; eoi + 2 <= b.n && !(b.d[eoi] == 0xFF && b.d[eoi + 1] == 0xD9); eoi++) {
        }
        eoi += 2;
        for (i = 0; i < 256; i++) {
            px[i] = (uint8_t)((i % 16) * 16);
        }
        j = tg_jpeg(px, 16, 16, 1, 60, &jl);
        tg_put(&nb, b.d, soi);
        tg_put(&nb, j, jl);
        if (eoi < b.n) {
            tg_put(&nb, b.d + eoi, b.n - eoi);
        }
        free(j);
        CHECK(pc_raw_decode(nb.d, nb.n, &lim, &s, &e) != PC_OK);
        CHECK_CONTAINS(e.msg, "16x16");
        CHECK_CONTAINS(e.msg, "32x32");
        tg_free(&nb);
    }
    tg_free(&b);
    /* 3-sample LinearRaw (YCbCr on disk) decodes to RGB. */
    for (i = 0; i < 32 * 32 * 3; i++) {
        rgb[i] = (uint16_t)(4096 + (i / (32 * 3)) * 16 + (i % 3) * 64 * 256);
    }
    spec = tg_dng_cfa(32, 32, rgb);
    spec.samples = 3;
    spec.storage = TG_LOSSY_TILES;
    spec.tile_w = 16;
    spec.tile_h = 16;
    b = tg_dng_build(&spec);
    if (decode_ok(&b, &s)) {
        bool apart = true;
        CHECK(s.samples == 3);
        CHECK(worst_vs_quantised(&s, rgb, 32 * 32 * 3) <= 8 * 256);
        for (i = 0; i < 32 * 32; i++) {
            unsigned r = s.data[i * 3];
            unsigned bl = s.data[i * 3 + 2];
            apart = apart && (r > bl ? r - bl : bl - r) >= 64 * 256 - 8 * 256;
        }
        CHECK(apart);
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
}

/* ---- hostile.rs ---- */

static tg_bytes oversized_lj92_tile(void)
{
    float (*sc)[3] = tg_scene(8, 8);
    uint16_t *cfa = tg_mosaic((const float (*)[3])sc, 64, 8, RGGB, 64, 4000);
    tg_dng d = tg_dng_cfa(8, 8, cfa);
    tg_bytes b;
    d.bits = 12;
    d.white = 4000;
    d.black[0] = 64;
    d.storage = TG_LJ92_TILES;
    d.tile_w = 512;
    d.tile_h = 512;
    b = tg_dng_build(&d);
    free(cfa);
    free(sc);
    return b;
}

static bool patch_long(tg_bytes *b, uint16_t tag, uint32_t to)
{
    size_t i;
    int patched = 0;
    for (i = 0; i + 12 <= b->n; i++) {
        if (b->d[i] == (uint8_t)tag && b->d[i + 1] == (uint8_t)(tag >> 8) && b->d[i + 2] == 4 &&
            b->d[i + 3] == 0 && b->d[i + 4] == 1 && b->d[i + 5] == 0 && b->d[i + 6] == 0 &&
            b->d[i + 7] == 0) {
            int k;
            for (k = 0; k < 4; k++) {
                b->d[i + 8 + (size_t)k] = (uint8_t)(to >> (8 * k));
            }
            patched++;
        }
    }
    return patched == 1;
}

static void test_lossless_jpeg_intermediate_is_charged_against_max_alloc(void)
{
    tg_bytes b = oversized_lj92_tile();
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_error e;
    CHECK(pc_raw_decode(b.d, b.n, &lim, &s, &e) == PC_OK);
    pc_raw_sensor_fini(&s);
    lim.max_alloc = 64 << 10;
    CHECK_EQ_INT(pc_raw_decode(b.d, b.n, &lim, &s, &e), PC_ERR_LIMIT);
    lim.max_alloc = 4 << 20;
    CHECK(pc_raw_decode(b.d, b.n, &lim, &s, &e) == PC_OK);
    pc_raw_sensor_fini(&s);
    tg_free(&b);
    {
        float (*sc)[3] = tg_scene(8, 8);
        uint16_t *cfa = tg_mosaic((const float (*)[3])sc, 64, 8, RGGB, 64, 4000);
        tg_dng d = tg_dng_cfa(8, 8, cfa);
        d.bits = 12;
        d.white = 4000;
        d.black[0] = 64;
        d.storage = TG_LJ92_TILES;
        d.tile_w = 4;
        d.tile_h = 4;
        b = tg_dng_build(&d);
        lim.max_alloc = 4096;
        CHECK(pc_raw_decode(b.d, b.n, &lim, &s, &e) == PC_OK);
        pc_raw_sensor_fini(&s);
        tg_free(&b);
        free(cfa);
        free(sc);
    }
}

static void test_lossless_jpeg_frame_larger_than_the_budget_is_refused(void)
{
    tg_bytes b = oversized_lj92_tile();
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_error e;
    CHECK(patch_long(&b, 322, 256));
    CHECK(patch_long(&b, 323, 256));
    lim.max_alloc = 256 << 10;
    CHECK_EQ_INT(pc_raw_decode(b.d, b.n, &lim, &s, &e), PC_ERR_LIMIT);
    tg_free(&b);
}

/* Truncations and byte mutations of every new hostile sample: never a crash. */
static void test_new_hostile_samples_never_crash(void)
{
    size_t w = 24;
    size_t h = 12;
    float (*sc)[3] = tg_scene(w, h);
    uint16_t *grey = tg_xalloc(w * h * sizeof *grey);
    uint16_t *cfa = tg_mosaic((const float (*)[3])sc, w * h, w, RGGB, 64, 4000);
    uint16_t *xt = tg_mosaic_cfa((const float (*)[3])sc, w * h, w, TG_XTRANS, 36, 6, 256, 4000);
    tg_bytes files[6];
    tg_dng m;
    tg_raf r;
    size_t nf = 0;
    size_t i;
    uint64_t st = 99;
    for (i = 0; i < w * h; i++) {
        grey[i] = (uint16_t)(64 + pc_f32_to_u16(sc[i][1] * 3900.0f));
    }
    m = tg_dng_mono(w, h, grey);
    m.bits = 12;
    m.white = 4000;
    m.black[0] = 64;
    m.storage = TG_LJ92_TILES;
    m.tile_w = 16;
    m.tile_h = 8;
    files[nf++] = tg_dng_build(&m);
    m.storage = TG_DEFLATE_STRIPS;
    m.rows = 5;
    m.predictor = 2;
    files[nf++] = tg_dng_build(&m);
    m.storage = TG_LOSSY_TILES;
    m.tile_w = 16;
    m.tile_h = 12;
    files[nf++] = tg_dng_build(&m);
    r = tg_raf_new(w, h, xt, TG_XTRANS, 36);
    r.bits = 12;
    r.has_crop = true;
    r.crop[0] = 1;
    r.crop[1] = 2;
    r.crop[2] = 10;
    r.crop[3] = 20;
    r.orientation = 8;
    files[nf++] = tg_raf_build(&r);
    r.packing = TG_RAF_LSB12;
    files[nf++] = tg_raf_build(&r);
    r = tg_raf_new(w, h, cfa, RGGB, 4);
    r.packing = TG_RAF_WORDS14;
    r.has_bias = true;
    r.bias[0] = -100;
    r.bias[1] = 100;
    files[nf++] = tg_raf_build(&r);
    for (i = 0; i < nf; i++) {
        size_t n;
        int it;
        for (n = 0; n < files[i].n; n += 5) {
            pc_raw_limits lim = pc_raw_limits_default();
            pc_raw_sensor s;
            pc_error e;
            if (pc_raw_decode(files[i].d, n, &lim, &s, &e) == PC_OK) {
                pc_raw_sensor_fini(&s);
            }
        }
        for (it = 0; it < 100; it++) {
            uint8_t *c = tg_xalloc(files[i].n);
            pc_raw_develop_options o = pc_raw_develop_options_default();
            pc_raw_sensor s;
            pc_raw_developed d;
            pc_error e;
            int k;
            memcpy(c, files[i].d, files[i].n);
            for (k = 0; k < 4; k++) {
                st = st * 6364136223846793005ull + 1442695040888963407ull;
                c[(st >> 33) % files[i].n] = (uint8_t)(st >> 50);
            }
            o.limits.max_alloc = (uint64_t)64 << 20;
            if (pc_raw_decode(c, files[i].n, &o.limits, &s, &e) == PC_OK) {
                o.tone.shadows = 50.0f;
                o.tone.dehaze = 40.0f;
                if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
                    pc_raw_developed_fini(&d);
                }
                pc_raw_sensor_fini(&s);
            }
            free(c);
        }
        tg_free(&files[i]);
    }
    free(grey);
    free(cfa);
    free(xt);
    free(sc);
}

/* ---- vendor_codecs.rs, vendors.rs ---- */

static void hide_raw_sub_ifd_tag(tg_bytes *b, uint16_t tag)
{
    size_t ifd = (size_t)b->d[4] | (size_t)b->d[5] << 8 | (size_t)b->d[6] << 16 | (size_t)b->d[7] << 24;
    size_t n = (size_t)b->d[ifd] | (size_t)b->d[ifd + 1] << 8;
    size_t i;
    for (i = 0; i < n; i++) {
        size_t e = ifd + 2 + 12 * i;
        if ((b->d[e] | b->d[e + 1] << 8) == 330) {
            size_t sub = (size_t)b->d[e + 8] | (size_t)b->d[e + 9] << 8 | (size_t)b->d[e + 10] << 16 |
                         (size_t)b->d[e + 11] << 24;
            size_t sn = (size_t)b->d[sub] | (size_t)b->d[sub + 1] << 8;
            size_t j;
            for (j = 0; j < sn; j++) {
                size_t se = sub + 2 + 12 * j;
                if ((b->d[se] | b->d[se + 1] << 8) == tag) {
                    b->d[se] = 0xFE;
                    b->d[se + 1] = 0xFF;
                    return;
                }
            }
        }
    }
    PC_T_FAIL("tag %#x not found in the raw SubIFD", tag);
}

static void test_craw_without_a_black_level_tag_reads_black_from_the_tone_curve(void)
{
    static const uint16_t CURVE[4] = {8000, 10400, 12900, 14100};
    uint16_t codes[64 * 4];
    tg_bytes b;
    pc_raw_sensor s;
    size_t i;
    for (i = 0; i < 64 * 4; i++) {
        codes[i] = 1024;
    }
    b = tg_sony_craw(64, 4, codes, CURVE);
    hide_raw_sub_ifd_tag(&b, 0x7310);
    if (decode_ok(&b, &s)) {
        CHECK(s.black.nvalues == 1 && s.black.values[0] == 512.0f);
        CHECK(!pc_raw_strings_any(&s.warnings, "black level"));
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
    b = tg_sony_craw(64, 4, codes, CURVE);
    if (decode_ok(&b, &s)) {
        CHECK(s.black.nvalues == 4 && s.black.values[0] == 512.0f && s.black.values[3] == 512.0f);
        pc_raw_sensor_fini(&s);
    }
    tg_free(&b);
}

static void test_a_bare_raf_header_is_recognised_but_unsupported(void)
{
    uint8_t raf[200];
    pc_raw_format f;
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor s;
    pc_error e;
    memset(raf, 0, sizeof raf);
    memcpy(raf, "FUJIFILMCCD-RAW 0201FF383501", 28);
    CHECK(pc_raw_identify(raf, sizeof raf, &f) && f == PC_RAW_RAF);
    CHECK_EQ_INT(pc_raw_decode(raf, sizeof raf, &lim, &s, &e), PC_ERR_UNSUPPORTED);
}

int main(int argc, char **argv)
{
    RUN(test_monochrome_linear_raw_develops_to_neutral_grey);
    RUN(test_monochrome_levels_and_white_balance);
    RUN(test_broken_monochrome_linear_raw_errors);
    RUN(test_an_orientation_past_u16_is_not_truncated_into_range);
    RUN(test_deflate_strips_round_trip);
    RUN(test_deflate_hostile_fails_cleanly);
    RUN(test_deflate_8_bit_predictor_and_packed_12_bit);
#ifndef PC_NO_JXL
    RUN(test_jpeg_xl_layouts_decode_exactly);
#endif
    RUN(test_jpeg_xl_fixtures);
    RUN(test_profile_gain_table_map_is_reported);
    RUN(test_lossy_jpeg_tiles);
    RUN(test_lossless_jpeg_intermediate_is_charged_against_max_alloc);
    RUN(test_lossless_jpeg_frame_larger_than_the_budget_is_refused);
    RUN(test_new_hostile_samples_never_crash);
    RUN(test_craw_without_a_black_level_tag_reads_black_from_the_tone_curve);
    RUN(test_a_bare_raf_header_is_recognised_but_unsupported);
    return pc_t_finish();
}
