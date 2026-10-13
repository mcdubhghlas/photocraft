/* Bit-exact parity with the Rust reference: tests/golden/raw-vectors.txt was written by
 * `rustref raw-vectors` (tools/rustref/src/raw_vectors.rs). Every synthetic file is rebuilt here
 * with tests/raw/testgen.h; its bytes, the decoded sensor (every field), the developed images and
 * the truncation / mutation sweeps must hash the same. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "raw/raw.h"
#include "testgen.h"

#include "rawhash.h"

/* ---- the sample files (raw_vectors.rs samples()) ---- */

typedef struct sample {
    char name[48];
    tg_bytes b;
} sample;

static sample all[96];
static size_t nall;

static void add(const char *name, tg_bytes b)
{
    (void)snprintf(all[nall].name, sizeof all[nall].name, "%s", name);
    all[nall].b = b;
    nall++;
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

static const double SRGB_TO_XYZ[3][3] = {
    {0.4124564, 0.3575761, 0.1804375}, {0.2126729, 0.7151522, 0.0721750}, {0.0193339, 0.1191920, 0.9503041}};

static void inv3_flat(const double m[3][3], double out[9])
{
    double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1],
           i = m[2][2];
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    out[0] = (e * i - f * h) / det;
    out[1] = (c * h - b * i) / det;
    out[2] = (b * f - c * e) / det;
    out[3] = (f * g - d * i) / det;
    out[4] = (a * i - c * g) / det;
    out[5] = (c * d - a * f) / det;
    out[6] = (d * h - e * g) / det;
    out[7] = (b * g - a * h) / det;
    out[8] = (a * e - b * d) / det;
}

static tg_bytes cr2_spec_build(size_t w, size_t h, size_t comps, const size_t *slices, size_t nslices)
{
    size_t l = 16;
    size_t t = 4;
    float(*rgb)[3] = tg_scene(w - l, h - t);
    static const uint8_t RGGB[4] = {0, 1, 1, 2};
    uint16_t *img = tg_mosaic((const float(*)[3])rgb, (w - l) * (h - t), w - l, RGGB, 512, 13000);
    uint16_t *data = tg_xalloc(w * h * sizeof *data);
    size_t x;
    size_t y;
    tg_cr2 s;
    tg_bytes b;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            data[y * w + x] = y >= t && x >= l ? img[(y - t) * (w - l) + (x - l)] : 512;
        }
    }
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
    b = tg_cr2_build(&s);
    free(rgb);
    free(img);
    free(data);
    return b;
}

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
    data = tg_mosaic((const float(*)[3])rgb, w * h, w, cfa, 512, 13000);
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

/* tests/hostile.rs samples() */
static size_t hostile(tg_bytes out[8])
{
    size_t w = 24;
    size_t h = 12;
    static const uint8_t RGGB[4] = {0, 1, 1, 2};
    float(*sc)[3] = tg_scene(w, h);
    uint16_t *cfa = tg_mosaic((const float(*)[3])sc, w * h, w, RGGB, 64, 4000);
    tg_dng d = tg_dng_cfa(w, h, cfa);
    static const double CM[9] = {3.2, -1.5, -0.5, -1.0, 1.9, 0.04, 0.06, -0.2, 1.06};
    uint16_t *data;
    size_t i;
    size_t n = 0;
    tg_cr2 c;
    float(*s64)[3];
    uint16_t *codes;
    float(*s30)[3];
    uint16_t *m30;
    static const uint16_t CURVE[4] = {8000, 10400, 12900, 14100};
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
    s64 = tg_scene(64, 6);
    codes = tg_mosaic((const float(*)[3])s64, 64 * 6, 64, RGGB, 256, 1900);
    out[n++] = tg_sony_craw(64, 6, codes, CURVE);
    s30 = tg_scene(30, 8);
    m30 = tg_mosaic((const float(*)[3])s30, 30 * 8, 30, RGGB, 128, 4095);
    out[n++] = tg_rw2(30, 8, m30, 12);
    out[n++] = tg_orf(w, h, cfa);
    free(sc);
    free(cfa);
    free(s64);
    free(codes);
    free(s30);
    free(m30);
    return n;
}

static void build_samples(void)
{
    tg_bytes hs[8];
    size_t n = hostile(hs);
    size_t i;
    char name[48];
    static const struct {
        const char *name;
        uint16_t bits;
        tg_storage st;
        size_t a;
        size_t b;
        bool be;
    } CASES[8] = {{"dng16", 16, TG_STRIPS, 5, 0, false},          {"dng16be", 16, TG_STRIPS, 23, 0, true},
                  {"dng12", 12, TG_STRIPS, 4, 0, false},          {"dng10be", 10, TG_STRIPS, 7, 0, true},
                  {"dng8", 8, TG_STRIPS, 9, 0, false},            {"dngtiles14", 14, TG_LJ92_TILES, 16, 8, false},
                  {"dngtiles16be", 16, TG_LJ92_TILES, 32, 32, true}, {"dnglj12", 12, TG_LJ92_STRIPS, 6, 0, false}};
    static const uint8_t RGGB[4] = {0, 1, 1, 2};
    for (i = 0; i < n; i++) {
        (void)snprintf(name, sizeof name, "hostile%zu", i);
        add(name, hs[i]);
    }
    for (i = 0; i < 8; i++) {
        uint16_t *v = noise(37 * 23, CASES[i].bits);
        tg_dng s = tg_dng_cfa(37, 23, v);
        s.bits = CASES[i].bits;
        s.storage = CASES[i].st;
        s.rows = CASES[i].a;
        s.tile_w = CASES[i].a;
        s.tile_h = CASES[i].b;
        s.big_endian = CASES[i].be;
        s.white = (1u << CASES[i].bits) - 1;
        add(CASES[i].name, tg_dng_build(&s));
        free(v);
    }
    {
        uint16_t *v = noise(600, 16);
        tg_dng s = tg_dng_cfa(20, 10, v);
        s.samples = 3;
        s.storage = TG_LJ92_TILES;
        s.tile_w = 16;
        s.tile_h = 16;
        add("linear", tg_dng_build(&s));
        free(v);
    }
    {
        uint16_t *v = tg_xalloc(1200 * sizeof *v);
        uint16_t *lin = tg_xalloc(4096 * sizeof *lin);
        tg_dng s;
        for (i = 0; i < 1200; i++) {
            v[i] = 1000;
        }
        for (i = 0; i < 4096; i++) {
            lin[i] = (uint16_t)(i * 3 < 65535 ? i * 3 : 65535);
        }
        s = tg_dng_cfa(40, 30, v);
        s.bits = 14;
        s.white = 15000;
        s.black[0] = 100;
        s.black[1] = 101;
        s.black[2] = 102;
        s.black[3] = 103;
        s.nblack = 4;
        s.black_repeat[0] = 2;
        s.black_repeat[1] = 2;
        s.has_active = true;
        s.active[0] = 2;
        s.active[1] = 4;
        s.active[2] = 28;
        s.active[3] = 36;
        s.has_crop = true;
        s.crop_origin[0] = 2;
        s.crop_origin[1] = 1;
        s.crop_size[0] = 20;
        s.crop_size[1] = 16;
        s.cfa[0] = 1;
        s.cfa[1] = 0;
        s.cfa[2] = 2;
        s.cfa[3] = 1;
        s.has_neutral = true;
        s.neutral[0] = 0.5;
        s.neutral[1] = 1.0;
        s.neutral[2] = 0.8;
        s.has_baseline = true;
        s.baseline = 0.5;
        s.orientation = 6;
        s.linearization = lin;
        s.nlinearization = 4096;
        add("meta", tg_dng_build(&s));
        free(v);
        free(lin);
    }
    {
        size_t cw = 48;
        size_t ch = 32;
        static const float GAINS[3] = {0.5f, 1.0f, 0.7f};
        static const float PATCHES[4][3] = {
            {0.18f, 0.18f, 0.18f}, {0.40f, 0.10f, 0.05f}, {0.05f, 0.30f, 0.10f}, {0.10f, 0.12f, 0.45f}};
        float(*rgb)[3] = tg_xalloc(cw * ch * sizeof *rgb);
        uint16_t *m;
        tg_dng s;
        for (i = 0; i < cw * ch; i++) {
            const float *p = PATCHES[((i % cw) / (cw / 2)) + 2 * ((i / cw) / (ch / 2))];
            int k;
            for (k = 0; k < 3; k++) {
                rgb[i][k] = p[k] * GAINS[k];
            }
        }
        m = tg_mosaic((const float(*)[3])rgb, cw * ch, cw, RGGB, 256, 16383);
        s = tg_dng_cfa(cw, ch, m);
        s.bits = 14;
        s.black[0] = 256;
        s.white = 16383;
        s.has_cm1 = true;
        s.cm1_ill = 21;
        inv3_flat(SRGB_TO_XYZ, s.cm1);
        s.has_neutral = true;
        s.neutral[0] = (double)GAINS[0];
        s.neutral[1] = (double)GAINS[1];
        s.neutral[2] = (double)GAINS[2];
        s.storage = TG_STRIPS;
        s.rows = 32;
        add("cm_strips", tg_dng_build(&s));
        s.storage = TG_LJ92_TILES;
        s.tile_w = 16;
        s.tile_h = 16;
        add("cm_tiles", tg_dng_build(&s));
        free(rgb);
        free(m);
    }
    {
        float(*rgb)[3] = tg_xalloc(256 * sizeof *rgb);
        uint16_t *m;
        tg_dng s;
        static const double FM[9] = {0.4360747, 0.3850649, 0.1430804, 0.2225045, 0.7168786,
                                     0.0606169, 0.0139322, 0.0971045, 0.7141733};
        for (i = 0; i < 256; i++) {
            rgb[i][0] = 0.3f;
            rgb[i][1] = 0.6f;
            rgb[i][2] = 0.45f;
        }
        m = tg_mosaic((const float(*)[3])rgb, 256, 16, RGGB, 0, 65535);
        s = tg_dng_cfa(16, 16, m);
        s.has_cm1 = true;
        s.cm1_ill = 17;
        inv3_flat(SRGB_TO_XYZ, s.cm1);
        s.has_cm2 = true;
        s.cm2_ill = 21;
        inv3_flat(SRGB_TO_XYZ, s.cm2);
        s.has_fm1 = true;
        memcpy(s.fm1, FM, sizeof FM);
        s.has_fm2 = true;
        memcpy(s.fm2, FM, sizeof FM);
        s.has_neutral = true;
        s.neutral[0] = 0.5;
        s.neutral[1] = 1.0;
        s.neutral[2] = 0.75;
        add("forward", tg_dng_build(&s));
        free(rgb);
        free(m);
    }
    {
        uint16_t *v = tg_xalloc(192 * sizeof *v);
        static const uint32_t AREA[4] = {0, 0, 12, 16};
        static const float G[2][2] = {{2.0f, 1.5f}, {1.25f, 0.5f}};
        tg_bytes ol = tg_gain_map_opcode_list(AREA, 1, G);
        tg_dng s;
        for (i = 0; i < 192; i++) {
            v[i] = 16384;
        }
        s = tg_dng_cfa(16, 12, v);
        s.has_neutral = true;
        s.neutral[0] = s.neutral[1] = s.neutral[2] = 1.0;
        s.opcode_list2 = ol.d;
        s.nopcode_list2 = ol.n;
        add("gainmap", tg_dng_build(&s));
        tg_free(&ol);
        free(v);
    }
    {
        float(*rgb)[3] = tg_xalloc(256 * sizeof *rgb);
        uint16_t *m;
        tg_dng s;
        for (i = 0; i < 256; i++) {
            rgb[i][0] = 0.2f;
            rgb[i][1] = 0.4f;
            rgb[i][2] = 0.3f;
        }
        m = tg_mosaic((const float(*)[3])rgb, 256, 16, RGGB, 0, 65535);
        s = tg_dng_cfa(16, 16, m);
        add("greyworld", tg_dng_build(&s));
        free(rgb);
        free(m);
    }
    {
        static const uint8_t BGGR[4] = {2, 1, 1, 0};
        float(*sc)[3] = tg_scene(64, 48);
        uint16_t *m = tg_mosaic((const float(*)[3])sc, 64 * 48, 64, BGGR, 0, 65535);
        tg_dng s = tg_dng_cfa(64, 48, m);
        memcpy(s.cfa, BGGR, 4);
        s.has_neutral = true;
        s.neutral[0] = s.neutral[1] = s.neutral[2] = 1.0;
        add("scene", tg_dng_build(&s));
        free(sc);
        free(m);
    }
    {
        uint16_t v[64];
        tg_dng s;
        for (i = 0; i < 64; i++) {
            v[i] = 65535;
        }
        s = tg_dng_cfa(8, 8, v);
        s.has_neutral = true;
        s.neutral[0] = 0.5;
        s.neutral[1] = 1.0;
        s.neutral[2] = 0.6;
        add("clipped", tg_dng_build(&s));
    }
    {
        static const size_t SA[3] = {32, 32, 24};
        static const size_t SB[2] = {40, 48};
        static const size_t SD[1] = {88};
        add("cr2_a", cr2_spec_build(88, 30, 2, SA, 3));
        add("cr2_b", cr2_spec_build(88, 30, 4, SB, 2));
        add("cr2_c", cr2_spec_build(88, 30, 2, NULL, 0));
        add("cr2_d", cr2_spec_build(88, 30, 4, SD, 1));
    }
    {
        static const uint8_t CFAS[2][4] = {{0, 1, 1, 2}, {1, 2, 0, 1}};
        static const size_t LT[2][2] = {{16, 4}, {17, 5}};
        size_t a;
        size_t b;
        for (a = 0; a < 2; a++) {
            for (b = 0; b < 2; b++) {
                (void)snprintf(name, sizeof name, "cr2phase_%u%u%u%u_%zu_%zu", CFAS[a][0], CFAS[a][1], CFAS[a][2],
                               CFAS[a][3], LT[b][0], LT[b][1]);
                add(name, cr2_phase_build(CFAS[a], LT[b][0], LT[b][1], false, 0, false));
            }
        }
        add("cr2flat_80000250", cr2_phase_build(RGGB, 16, 4, true, 0x80000250u, true));
        add("cr2flat_80000001", cr2_phase_build(RGGB, 16, 4, true, 0x80000001u, true));
        add("cr2flat_0", cr2_phase_build(RGGB, 16, 4, false, 0, true));
    }
    {
        static const uint8_t BGGR[4] = {2, 1, 1, 0};
        float(*sc)[3] = tg_scene(24, 16);
        uint16_t *m = tg_mosaic((const float(*)[3])sc, 24 * 16, 24, BGGR, 0, 4095);
        uint16_t *m2 = tg_mosaic((const float(*)[3])sc, 24 * 16, 24, RGGB, 512, 16383);
        tg_entry extra[5];
        uint16_t black[4] = {512, 512, 512, 512};
        uint16_t wb[4] = {2048, 1024, 1024, 1536};
        uint32_t org[2] = {2, 2};
        uint32_t sz[2] = {20, 12};
        size_t k;
        add("nef_like", tg_tiff_ep("NIKON CORPORATION", 24, 16, m, BGGR, 12, NULL, 0));
        extra[0].tag = 0x7310;
        extra[0].val = tg_v_short(black, 4);
        extra[1].tag = 0x7313;
        extra[1].val = tg_v_short(wb, 4);
        extra[2].tag = 50717;
        extra[2].val = tg_v_short1(16383);
        extra[3].tag = 50719;
        extra[3].val = tg_v_long(org, 2);
        extra[4].tag = 50720;
        extra[4].val = tg_v_long(sz, 2);
        add("arw_like", tg_tiff_ep("SONY", 24, 16, m2, RGGB, 14, extra, 5));
        (void)k;
        free(sc);
        free(m);
        free(m2);
    }
    {
        static const uint16_t CURVE[4] = {8000, 10400, 12900, 14100};
        float(*sc)[3] = tg_scene(640, 8);
        uint16_t *codes = tg_mosaic((const float(*)[3])sc, 640 * 8, 640, RGGB, 256, 1900);
        uint16_t wide[64 * 8];
        uint32_t s = 12345;
        add("craw", tg_sony_craw(640, 8, codes, CURVE));
        for (i = 0; i < 64 * 8; i++) {
            s = s * 1103515245u + 12345u;
            wide[i] = (uint16_t)((s >> 16) % 2000);
        }
        add("craw_wide", tg_sony_craw(64, 8, wide, CURVE));
        free(sc);
        free(codes);
    }
    {
        static const uint8_t GRBG[4] = {1, 0, 2, 1};
        float(*a)[3] = tg_scene(120, 300);
        float(*b)[3] = tg_scene(117, 260);
        float(*c)[3] = tg_scene(40, 24);
        uint16_t *ma = tg_mosaic((const float(*)[3])a, 120 * 300, 120, RGGB, 128, 4095);
        uint16_t *mb = tg_mosaic((const float(*)[3])b, 117 * 260, 117, RGGB, 128, 16383);
        uint16_t *mc = tg_mosaic((const float(*)[3])c, 40 * 24, 40, GRBG, 64, 4095);
        add("rw2_12", tg_rw2(120, 300, ma, 12));
        add("rw2_14", tg_rw2(117, 260, mb, 14));
        add("orf", tg_orf(40, 24, mc));
        free(a);
        free(b);
        free(c);
        free(ma);
        free(mb);
        free(mc);
    }
}

/* ---- golden ---- */

static char **gold;
static size_t ngold;
static size_t gi;

static void load_golden(void)
{
    FILE *f = fopen("tests/golden/raw-vectors.txt", "r");
    char line[8192];
    if (f == NULL) {
        PC_T_FAIL("cannot open tests/golden/raw-vectors.txt");
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);
        char **g;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        /* The R3 section is test_parity2.c's. */
        if (strcmp(line, "section r3") == 0) {
            break;
        }
        g = realloc(gold, (ngold + 1) * sizeof *gold);
        if (g == NULL) {
            break;
        }
        gold = g;
        gold[ngold] = malloc(n + 1);
        if (gold[ngold] == NULL) {
            break;
        }
        memcpy(gold[ngold], line, n + 1);
        ngold++;
    }
    (void)fclose(f);
}

/* Compares the next golden line with got. */
static void expect(const char *got)
{
    if (gi >= ngold) {
        PC_T_FAIL("golden ran out at: %s", got);
        return;
    }
    CHECK(strcmp(gold[gi], got) == 0);
    if (strcmp(gold[gi], got) != 0) {
        PC_T_FAIL("line %zu\n  want %s\n  got  %s", gi + 1, gold[gi], got);
    }
    gi++;
}

static const char *const METHODS[3] = {"bilinear", "mhc", "ahd"};

static void test_files_decode_and_develop_like_the_reference(void)
{
    size_t i;
    char line[8192];
    for (i = 0; i < nall; i++) {
        uint64_t h = H0;
        pc_raw_limits lim = pc_raw_limits_default();
        pc_raw_sensor s;
        pc_error e;
        fnv(&h, all[i].b.d, all[i].b.n);
        (void)snprintf(line, sizeof line, "file %s %zu %016llx", all[i].name, all[i].b.n, (unsigned long long)h);
        expect(line);
        if (pc_raw_decode(all[i].b.d, all[i].b.n, &lim, &s, &e) == PC_OK) {
            int m;
            (void)snprintf(line, sizeof line, "decode %s %016llx", all[i].name, (unsigned long long)sensor_hash(&s));
            expect(line);
            for (m = 0; m < 3; m++) {
                pc_raw_develop_options o = pc_raw_develop_options_default();
                pc_raw_developed d;
                o.demosaic = (pc_raw_demosaic)m;
                if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
                    (void)snprintf(line, sizeof line, "develop %s %s %016llx", all[i].name, METHODS[m],
                                   (unsigned long long)developed_hash(&d));
                    pc_raw_developed_fini(&d);
                } else {
                    (void)snprintf(line, sizeof line, "develop %s %s err %s", all[i].name, METHODS[m], e.msg);
                }
                expect(line);
            }
            pc_raw_sensor_fini(&s);
        } else {
            (void)snprintf(line, sizeof line, "decode %s err %s", all[i].name, e.msg);
            expect(line);
        }
    }
}

static const tg_bytes *find(const char *name)
{
    size_t i;
    for (i = 0; i < nall; i++) {
        if (strcmp(all[i].name, name) == 0) {
            return &all[i].b;
        }
    }
    return NULL;
}

static void test_development_options_match(void)
{
    static const char *const NAMES[5] = {"hostile0", "meta", "greyworld", "linear", "gainmap"};
    static const char *const OPTS[5] = {"auto", "mult", "badmult", "exposure", "noorient"};
    size_t i;
    char line[8192];
    for (i = 0; i < 5; i++) {
        const tg_bytes *b = find(NAMES[i]);
        pc_raw_limits lim = pc_raw_limits_default();
        pc_raw_sensor s;
        int k;
        if (b == NULL || pc_raw_decode(b->d, b->n, &lim, &s, NULL) != PC_OK) {
            PC_T_FAIL("%s does not decode", NAMES[i]);
            continue;
        }
        for (k = 0; k < 5; k++) {
            pc_raw_develop_options o = pc_raw_develop_options_default();
            pc_raw_developed d;
            pc_error e;
            switch (k) {
            case 0:
                o.white_balance.kind = PC_RAW_WB_AUTO;
                break;
            case 1:
                o.white_balance.kind = PC_RAW_WB_MULTIPLIERS;
                o.white_balance.multipliers[0] = 2.0;
                o.white_balance.multipliers[1] = 1.0;
                o.white_balance.multipliers[2] = 4.0 / 3.0;
                break;
            case 2:
                o.white_balance.kind = PC_RAW_WB_MULTIPLIERS;
                o.white_balance.multipliers[0] = 0.0;
                o.white_balance.multipliers[1] = 1.0;
                o.white_balance.multipliers[2] = 1.0;
                break;
            case 3:
                o.exposure = 1.0;
                break;
            default:
                o.orient = false;
                o.exposure = -0.7;
            }
            if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
                (void)snprintf(line, sizeof line, "option %s %s %016llx", NAMES[i], OPTS[k],
                               (unsigned long long)developed_hash(&d));
                pc_raw_developed_fini(&d);
            } else {
                (void)snprintf(line, sizeof line, "option %s %s err %s", NAMES[i], OPTS[k], e.msg);
            }
            expect(line);
        }
        pc_raw_sensor_fini(&s);
    }
}

static void exercise(uint64_t *h, const uint8_t *b, size_t n)
{
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_format f;
    pc_raw_preview p;
    pc_raw_sensor s;
    pc_error e;
    uint8_t id;
    uint8_t ee = 0xEE;
    lim.max_alloc = 64u << 20;
    id = pc_raw_identify(b, n, &f) ? (uint8_t)f : 0xFF;
    fnv(h, &id, 1);
    if (pc_raw_embedded_preview(b, n, &p)) {
        u64le(h, p.width);
        u64le(h, p.height);
        u64le(h, p.len);
    } else {
        fnv(h, &ee, 1);
    }
    if (pc_raw_decode(b, n, &lim, &s, &e) == PC_OK) {
        pc_raw_develop_options o = pc_raw_develop_options_default();
        pc_raw_developed d;
        u64le(h, sensor_hash(&s));
        o.demosaic = PC_RAW_BILINEAR;
        o.limits = lim;
        if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
            u64le(h, developed_hash(&d));
            pc_raw_developed_fini(&d);
        } else {
            str0(h, e.msg);
        }
        pc_raw_sensor_fini(&s);
    } else {
        str0(h, e.msg);
    }
}

static void test_truncation_and_mutation_sweeps_match(void)
{
    tg_bytes hs[8];
    size_t n = hostile(hs);
    size_t i;
    char line[8192];
    for (i = 0; i < n; i++) {
        uint64_t h = H0;
        size_t k;
        uint64_t st = 0xDEADBEEFull ^ i;
        uint8_t *c = tg_xalloc(hs[i].n);
        int it;
        for (k = 0; k < hs[i].n; k += 3) {
            /* Exact-size copies so ASan sees any read past the cut. */
            uint8_t *cut = tg_xalloc(k);
            memcpy(cut, hs[i].d, k);
            exercise(&h, cut, k);
            free(cut);
        }
        (void)snprintf(line, sizeof line, "truncate hostile%zu %016llx", i, (unsigned long long)h);
        expect(line);
        h = H0;
#define NEXT() (st = st * 6364136223846793005ull + 1442695040888963407ull, st >> 33)
        for (it = 0; it < 150; it++) {
            uint64_t m;
            uint64_t j;
            memcpy(c, hs[i].d, hs[i].n);
            m = 1 + NEXT() % 6;
            for (j = 0; j < m; j++) {
                size_t at = (size_t)(NEXT() % hs[i].n);
                uint64_t r = NEXT() % 4;
                c[at] = (uint8_t)(r == 0 ? 0xFF : r == 1 ? 0 : NEXT());
            }
            exercise(&h, c, hs[i].n);
        }
#undef NEXT
        (void)snprintf(line, sizeof line, "mutate hostile%zu %016llx", i, (unsigned long long)h);
        expect(line);
        free(c);
        tg_free(&hs[i]);
    }
}

int main(int argc, char **argv)
{
    size_t i;
    load_golden();
    build_samples();
    RUN(test_files_decode_and_develop_like_the_reference);
    RUN(test_development_options_match);
    RUN(test_truncation_and_mutation_sweeps_match);
    CHECK_EQ_UINT(gi, ngold);
    if (gi != ngold) {
        PC_T_FAIL("%zu golden lines left unchecked", ngold - gi);
    }
    for (i = 0; i < nall; i++) {
        tg_free(&all[i].b);
    }
    for (i = 0; i < ngold; i++) {
        free(gold[i]);
    }
    free(gold);
    return pc_t_finish();
}
