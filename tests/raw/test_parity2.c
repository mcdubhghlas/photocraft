/* Bit-exact parity with the Rust reference for the R3 resync (tests/golden/raw-vectors.txt after its
 * "section r3" line, written by tools/rustref/src/raw_vectors_r3.rs): Fujifilm RAF, monochrome,
 * deflate, lossy and JPEG XL DNG, ProfileGainTableMap, LONG orientations, the tone sliders and black
 * point, retone, camera looks, preview lists and colour spaces, and hostile sweeps.
 *
 * Samples the C testgen builds itself (kind e) are compared byte for byte; samples from encoders
 * the C tree doesn't share are loaded from tests/fixtures/raw/ (f: exact; l: lossy JPEG, compared
 * with zune-jpeg's decode within a tolerance; j: JPEG XL, exact with libjxl, "unsupported"
 * without it). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "raw/internal.h"
#include "raw/raw.h"
#include "rawhash.h"
#include "testgen.h"
#include "testgen_raf.h"

/* ---- golden ---- */

static char **gold;
static size_t ngold;
static size_t gi;

static void load_golden(void)
{
    FILE *f = fopen("tests/golden/raw-vectors.txt", "r");
    char line[8192];
    bool on = false;
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
        if (!on) {
            on = strcmp(line, "section r3") == 0;
            continue;
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

/* Skips golden lines starting with prefix. */
static size_t skip(const char *prefix)
{
    size_t n = 0;
    while (gi < ngold && strncmp(gold[gi], prefix, strlen(prefix)) == 0) {
        gi++;
        n++;
    }
    return n;
}

/* ---- samples ---- */

typedef struct sample {
    char name[32];
    tg_bytes b;
    char kind;
} sample;

static sample all[128];
static size_t nall;

static void add(const char *name, tg_bytes b, char kind)
{
    (void)snprintf(all[nall].name, sizeof all[nall].name, "%s", name);
    all[nall].b = b;
    all[nall].kind = kind;
    nall++;
}

static bool read_file(const char *path, tg_bytes *out)
{
    FILE *f = fopen(path, "rb");
    uint8_t buf[65536];
    size_t n;
    tg_bytes b = {NULL, 0, 0};
    if (f == NULL) {
        return false;
    }
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        tg_put(&b, buf, n);
    }
    (void)fclose(f);
    *out = b;
    return true;
}

static void fixture(const char *name, char kind)
{
    char path[256];
    tg_bytes b = {NULL, 0, 0};
    (void)snprintf(path, sizeof path, "tests/fixtures/raw/%s.bin", name);
    if (!read_file(path, &b)) {
        PC_T_FAIL("missing fixture %s", path);
    }
    add(name, b, kind);
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

static const uint8_t RGGB[4] = {0, 1, 1, 2};

static tg_bytes raf_bytes(tg_raf *r, uint16_t *data)
{
    tg_bytes b = tg_raf_build(r);
    free(data);
    return b;
}

/* tests/raf.rs xtrans(): a 30 x 18 X-Trans RAF with a 2-pixel border. */
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

static void hostile_r3(bool sweep_only)
{
    size_t w = 24;
    size_t h = 12;
    float (*sc)[3] = tg_scene(w, h);
    uint16_t *cfa = tg_mosaic((const float (*)[3])sc, w * h, w, RGGB, 64, 4000);
    uint16_t *grey = tg_xalloc(w * h * sizeof *grey);
    uint16_t *xt;
    tg_dng m;
    tg_raf r;
    size_t i;
    int k;
    if (!sweep_only) {
        fixture("h_jxl", 'j');
    }
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
    add("h_mono_lj92", tg_dng_build(&m), 'e');
    free(grey);
    if (!sweep_only) {
        fixture("h_mono_deflate", 'f');
    }
    xt = tg_mosaic_cfa((const float (*)[3])sc, w * h, w, TG_XTRANS, 36, 6, 256, 4000);
    for (k = 0; k < 2; k++) {
        r = tg_raf_new(w, h, xt, TG_XTRANS, 36);
        r.bits = 12;
        r.packing = k == 0 ? TG_RAF_U16_LE : TG_RAF_LSB12;
        for (i = 0; i < 36; i++) {
            r.black[i] = 256;
        }
        r.nblack = 36;
        r.has_crop = true;
        r.crop[0] = 1;
        r.crop[1] = 2;
        r.crop[2] = 10;
        r.crop[3] = 20;
        r.orientation = 8;
        add(k == 0 ? "h_raf_u16" : "h_raf_lsb12", tg_raf_build(&r), 'e');
    }
    free(xt);
    r = tg_raf_new(w, h, cfa, RGGB, 4);
    r.packing = TG_RAF_WORDS14;
    r.has_bias = true;
    r.bias[0] = -100;
    r.bias[1] = 100;
    add("h_raf_words14", tg_raf_build(&r), 'e');
    free(cfa);
    free(sc);
}

static tg_entry srat_entry(uint16_t tag, const double *v, size_t n)
{
    uint64_t r[6];
    size_t i;
    tg_entry e;
    for (i = 0; i < n; i++) {
        r[2 * i] = (uint32_t)pc_f64_to_i32(v[i] * 10000.0);
        r[2 * i + 1] = 10000;
    }
    e.tag = tag;
    e.val = tg_v_nums(TG_SRATIONAL, r, n);
    return e;
}

static void build_samples(void)
{
    static const struct {
        const char *name;
        tg_raf_packing p;
        uint32_t bits;
    } XT[4] = {{"raf_u16le_14", TG_RAF_U16_LE, 14},
               {"raf_u16be_16", TG_RAF_U16_BE, 16},
               {"raf_lsb12", TG_RAF_LSB12, 12},
               {"raf_words14", TG_RAF_WORDS14, 14}};
    uint16_t *d;
    tg_raf r;
    size_t i;
    size_t k;
    char name[32];
    hostile_r3(false);
    for (i = 0; i < 4; i++) {
        r = xtrans(XT[i].p, XT[i].bits, &d);
        add(XT[i].name, raf_bytes(&r, d), 'e');
    }
    r = xtrans(TG_RAF_U16_LE, 14, &d);
    r.has_bias = true;
    r.bias[0] = -72;
    r.bias[1] = 100;
    r.orientation = 6;
    add("raf_meta", raf_bytes(&r, d), 'e');
    {
        float (*rgb)[3] = tg_xalloc(36 * 24 * sizeof *rgb);
        for (i = 0; i < 36 * 24; i++) {
            rgb[i][0] = 0.4f / 2.0f;
            rgb[i][1] = 0.4f;
            rgb[i][2] = 0.4f / 1.5f;
        }
        d = tg_mosaic_cfa((const float (*)[3])rgb, 36 * 24, 36, TG_XTRANS, 36, 6, 256, 16383);
        free(rgb);
        r = tg_raf_new(36, 24, d, TG_XTRANS, 36);
        r.black[0] = 256;
        add("raf_grey", raf_bytes(&r, d), 'e');
    }
    {
        static const uint8_t CFAS[2][4] = {{0, 1, 1, 2}, {2, 1, 1, 0}};
        static const char *const CN[2] = {"rggb", "bggr"};
        static const char *const PN[3] = {"lsb12", "words14", "u16"};
        static const tg_raf_packing PP[3] = {TG_RAF_LSB12, TG_RAF_WORDS14, TG_RAF_U16_LE};
        static const uint32_t PB[3] = {12, 14, 14};
        float (*sc)[3] = tg_scene(24, 16);
        size_t c;
        for (c = 0; c < 2; c++) {
            for (k = 0; k < 3; k++) {
                uint16_t white = (uint16_t)((1u << PB[k]) - 1);
                d = tg_mosaic_cfa((const float (*)[3])sc, 24 * 16, 24, CFAS[c], 4, 2, 64, white);
                r = tg_raf_new(24, 16, d, CFAS[c], 4);
                r.bits = PB[k];
                r.packing = PP[k];
                for (i = 0; i < 4; i++) {
                    r.black[i] = 64;
                }
                r.nblack = 4;
                (void)snprintf(name, sizeof name, "raf_%s_%s", CN[c], PN[k]);
                add(name, raf_bytes(&r, d), 'e');
            }
        }
        free(sc);
    }
    r = xtrans(TG_RAF_COMPRESSED, 14, &d);
    add("raf_compressed", tg_raf_build(&r), 'e');
    r.packing = TG_RAF_U16_LE;
    r.cfa_tiff = false;
    add("raf_nocfa", raf_bytes(&r, d), 'e');
    {
        static const uint8_t BAD[4] = {0, 0, 2, 2};
        uint16_t v[32];
        for (i = 0; i < 32; i++) {
            v[i] = 100;
        }
        r = tg_raf_new(8, 4, v, BAD, 4);
        add("raf_badlayout", tg_raf_build(&r), 'e');
    }
    r = xtrans(TG_RAF_U16_LE, 14, &d);
    {
        tg_bytes b = raf_bytes(&r, d);
        b.n -= 7;
        add("raf_trunc", b, 'e');
    }
    /* monochrome LinearRaw */
    {
        static const struct {
            const char *name;
            uint16_t bits;
            tg_storage st;
            size_t rows, tw, th;
        } M[5] = {{"mono_strips16", 16, TG_STRIPS, 5, 0, 0},
                  {"mono_packed12", 12, TG_STRIPS, 4, 0, 0},
                  {"mono_b8", 8, TG_STRIPS, 23, 0, 0},
                  {"mono_lj92s", 14, TG_LJ92_STRIPS, 6, 0, 0},
                  {"mono_lj92t", 16, TG_LJ92_TILES, 0, 16, 8}};
        for (i = 0; i < 5; i++) {
            uint16_t white = (uint16_t)((1u << M[i].bits) - 1);
            uint16_t *v = ramp(37, 23, 0, white);
            tg_dng s = tg_dng_mono(37, 23, v);
            s.bits = M[i].bits;
            s.white = white;
            s.storage = M[i].st;
            s.rows = M[i].rows;
            s.tile_w = M[i].tw;
            s.tile_h = M[i].th;
            add(M[i].name, tg_dng_build(&s), 'e');
            free(v);
        }
        fixture("mono_deflate", 'f');
        fixture("mono_lossy", 'l');
    }
    {
        uint16_t *v = ramp(32, 4, 1000, 60000);
        tg_dng s = tg_dng_mono(32, 4, v);
        s.black[0] = 1000;
        s.white = 60000;
        add("mono_levels", tg_dng_build(&s), 'e');
        free(v);
    }
    {
        uint16_t *v = ramp(16, 6, 100, 4000);
        uint16_t *v2 = ramp(32, 6, 100, 4000);
        static const double CM[9] = {3.2, -1.5, -0.5, -1.0, 1.9, 0.04, 0.06, -0.2, 1.06};
        for (k = 0; k < 10; k++) {
            tg_dng s = tg_dng_mono(16, 6, v);
            tg_entry ex[2];
            size_t nex = 0;
            if (k == 0) {
                static const double A[3] = {0.3, 0.6, 0.1};
                ex[nex++] = srat_entry(50721, A, 3);
                ex[nex].tag = 50778;
                ex[nex++].val = tg_v_short1(21);
            } else if (k == 1) {
                static const double A[1] = {1.0};
                ex[nex++] = srat_entry(50721, A, 1);
            } else if (k == 2) {
                uint64_t one[2] = {1, 1};
                ex[nex].tag = 50728;
                ex[nex++].val = tg_v_nums(TG_RATIONAL, one, 1);
            } else if (k == 3) {
                s.black[0] = 100;
                s.black[1] = 120;
                s.nblack = 2;
            } else if (k == 4) {
                s.black[0] = 100;
                s.black[1] = 110;
                s.black[2] = 120;
                s.nblack = 3;
            } else if (k == 5 || k == 6) {
                s.black[0] = 100;
                s.black[1] = 101;
                s.black[2] = 102;
                s.black[3] = 103;
                s.nblack = k == 5 ? 4 : 1;
                s.black_repeat[0] = 2;
                s.black_repeat[1] = 2;
            } else if (k == 7) {
                s.has_cm1 = true;
                s.cm1_ill = 21;
                memcpy(s.cm1, CM, sizeof CM);
            } else if (k == 8) {
                s.has_neutral = true;
                s.neutral[0] = 0.5;
                s.neutral[1] = 1.0;
                s.neutral[2] = 0.7;
            } else {
                s.samples = 2;
                s.data = v2;
            }
            s.ifd0_extra = ex;
            s.nifd0_extra = nex;
            if (k < 9) {
                (void)snprintf(name, sizeof name, "mono_odd%zu", k);
            } else {
                (void)snprintf(name, sizeof name, "mono_two");
            }
            add(name, tg_dng_build(&s), 'e');
            for (i = 0; i < nex; i++) {
                tg_val_free(&ex[i].val);
            }
        }
        free(v);
        free(v2);
    }
    for (i = 0; i < 5; i++) {
        (void)snprintf(name, sizeof name, "deflate%zu", i);
        fixture(name, 'f');
    }
    for (i = 0; i < 6; i++) {
        (void)snprintf(name, sizeof name, "jxl%zu", i);
        fixture(name, 'j');
    }
    fixture("jxl_dev", 'j');
    fixture("jxl_short", 'j');
    fixture("jxl_grey", 'j');
    fixture("jxl_junk", 'j');
    {
        uint16_t v[64];
        static const uint8_t ZERO[64] = {0};
        tg_dng s;
        tg_bytes b;
        for (i = 0; i < 64; i++) {
            v[i] = 500;
        }
        s = tg_dng_cfa(8, 8, v);
        s.pgtm = ZERO;
        s.npgtm = 64;
        add("pgtm", tg_dng_build(&s), 'e');
        s = tg_dng_cfa(8, 6, v);
        s.orientation = 2;
        for (k = 0; k < 2; k++) {
            static const uint8_t SHORT[12] = {0x12, 0x01, 3, 0, 1, 0, 0, 0, 2, 0, 0, 0};
            uint32_t to = k == 0 ? 65538u : 6u;
            size_t at;
            b = tg_dng_build(&s);
            for (at = 0; at + 12 <= b.n && memcmp(b.d + at, SHORT, 12) != 0; at++) {
            }
            CHECK(at + 12 <= b.n);
            if (at + 12 <= b.n) {
                b.d[at + 2] = 4;
                b.d[at + 8] = (uint8_t)to;
                b.d[at + 9] = (uint8_t)(to >> 8);
                b.d[at + 10] = (uint8_t)(to >> 16);
                b.d[at + 11] = (uint8_t)(to >> 24);
            }
            add(k == 0 ? "orient_long" : "orient_long6", b, 'e');
        }
    }
    fixture("lossy_cfa", 'l');
    fixture("lossy_rgb", 'l');
    {
        float (*sc)[3] = tg_scene(300, 200);
        uint16_t *v = tg_mosaic((const float (*)[3])sc, 300 * 200, 300, RGGB, 0, 65535);
        tg_dng s = tg_dng_cfa(300, 200, v);
        s.has_neutral = true;
        s.neutral[0] = s.neutral[1] = s.neutral[2] = 1.0;
        add("big", tg_dng_build(&s), 'e');
        free(v);
        free(sc);
    }
}

static const sample *find(const char *name)
{
    size_t i;
    for (i = 0; i < nall; i++) {
        if (strcmp(all[i].name, name) == 0) {
            return &all[i];
        }
    }
    return NULL;
}

static uint64_t sensor_hash2(const pc_raw_sensor *s)
{
    uint64_t h = H0;
    u64le(&h, sensor_hash(s));
    f32b(&h, s->black_point);
    return h;
}

static const char *const METHODS[3] = {"bilinear", "mhc", "ahd"};

#ifdef PC_NO_JXL
#define HAVE_JXL 0
#else
#define HAVE_JXL 1
#endif

/* Lossy JPEG tiles: libjpeg-turbo against zune-jpeg's decode of the same file. */
static void lossy_vs_zune(const sample *s)
{
    char path[256];
    tg_bytes z = {NULL, 0, 0};
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor sn;
    pc_error e;
    size_t i;
    unsigned worst = 0;
    double sum = 0.0;
    (void)snprintf(path, sizeof path, "tests/fixtures/raw/%.31s.zune", s->name);
    if (!read_file(path, &z)) {
        PC_T_FAIL("missing %s", path);
        return;
    }
    if (pc_raw_decode(s->b.d, s->b.n, &lim, &sn, &e) != PC_OK) {
        PC_T_FAIL("%s: %s", s->name, e.msg);
        tg_free(&z);
        return;
    }
    CHECK_EQ_UINT(sn.ndata * 2, z.n);
    for (i = 0; i < sn.ndata && 2 * i + 1 < z.n; i++) {
        unsigned want = (unsigned)(z.d[2 * i] | z.d[2 * i + 1] << 8);
        unsigned d = sn.data[i] > want ? sn.data[i] - want : want - sn.data[i];
        worst = d > worst ? d : worst;
        sum += d;
    }
    /* Both decoders implement ITU-T T.81; they may round the IDCT and colour conversion differently
     * by a code value or two (x 257 after widening). */
    CHECK(worst <= 3 * 257);
    printf("  %s: libjpeg-turbo vs zune-jpeg: max %u, mean %.2f (16-bit units)\n", s->name, worst,
           sn.ndata > 0 ? sum / (double)sn.ndata : 0.0);
    pc_raw_sensor_fini(&sn);
    tg_free(&z);
}

static void test_files_match(void)
{
    size_t i;
    char line[8192];
    for (i = 0; i < nall; i++) {
        const sample *s = &all[i];
        uint64_t h = H0;
        pc_raw_limits lim = pc_raw_limits_default();
        pc_raw_sensor sn;
        pc_error e;
        char prefix[64];
        fnv(&h, s->b.d, s->b.n);
        (void)snprintf(line, sizeof line, "file %.31s %zu %016llx %c", s->name, s->b.n, (unsigned long long)h,
                       s->kind);
        expect(line);
        if (s->kind == 'l') {
            lossy_vs_zune(s);
            continue;
        }
        if (s->kind == 'j' && !HAVE_JXL) {
            /* Without libjxl: the reference's text for builds without a decoder. */
            CHECK(pc_raw_decode(s->b.d, s->b.n, &lim, &sn, &e) == PC_ERR_UNSUPPORTED);
            CHECK_CONTAINS(e.msg, "JPEG XL-compressed DNG (DNG 1.7) can't be opened");
            (void)snprintf(prefix, sizeof prefix, "decode %.31s ", s->name);
            skip(prefix);
            (void)snprintf(prefix, sizeof prefix, "develop %.31s ", s->name);
            skip(prefix);
            continue;
        }
        if (pc_raw_decode(s->b.d, s->b.n, &lim, &sn, &e) == PC_OK) {
            int m;
            (void)snprintf(line, sizeof line, "decode %.31s %016llx", s->name,
                           (unsigned long long)sensor_hash2(&sn));
            expect(line);
            for (m = 0; m < 3; m++) {
                pc_raw_develop_options o = pc_raw_develop_options_default();
                pc_raw_developed d;
                o.demosaic = (pc_raw_demosaic)m;
                if (pc_raw_develop_sensor(&sn, &o, &d, &e) == PC_OK) {
                    (void)snprintf(line, sizeof line, "develop %.31s %s %016llx", s->name, METHODS[m],
                                   (unsigned long long)developed_hash(&d));
                    pc_raw_developed_fini(&d);
                } else {
                    (void)snprintf(line, sizeof line, "develop %.31s %s err %s", s->name, METHODS[m], e.msg);
                }
                expect(line);
            }
            pc_raw_sensor_fini(&sn);
        } else if (strcmp(s->name, "jxl_junk") == 0) {
            /* jxl-oxide's error text is its own: compare up to it. */
            (void)snprintf(line, sizeof line, "decode %.31s err %s", s->name, e.msg);
            static const char JUNK[] = "decode jxl_junk err malformed camera raw: JPEG XL raw data";
            CHECK_CONTAINS(line, JUNK);
            CHECK(gi < ngold && strncmp(gold[gi], JUNK, strlen(JUNK)) == 0);
            gi++;
        } else {
            (void)snprintf(line, sizeof line, "decode %.31s err %s", s->name, e.msg);
            expect(line);
        }
    }
}

static pc_raw_sensor decoded(const char *name)
{
    const sample *s = find(name);
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor sn;
    pc_error e;
    pc_raw_sensor_init(&sn);
    if (s == NULL || pc_raw_decode(s->b.d, s->b.n, &lim, &sn, &e) != PC_OK) {
        PC_T_FAIL("cannot decode %s", name);
        pc_raw_sensor_init(&sn);
    }
    return sn;
}

static void develop_line(const char *label, const pc_raw_sensor *s, const pc_raw_develop_options *o)
{
    pc_raw_developed d;
    pc_error e;
    char line[512];
    if (pc_raw_develop_sensor(s, o, &d, &e) == PC_OK) {
        (void)snprintf(line, sizeof line, "%s %016llx", label, (unsigned long long)developed_hash(&d));
        pc_raw_developed_fini(&d);
    } else {
        (void)snprintf(line, sizeof line, "%s err %s", label, e.msg);
    }
    expect(line);
}

typedef struct named_tone {
    const char *name;
    pc_raw_tone t;
} named_tone;

static const named_tone TONES[11] = {
    {"sh+100", {100.0f, 0.0f, 0.0f, 0.0f, 0.0f}},   {"sh-60", {-60.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    {"hi-100", {0.0f, -100.0f, 0.0f, 0.0f, 0.0f}},  {"hi+50", {0.0f, 50.0f, 0.0f, 0.0f, 0.0f}},
    {"cl+100", {0.0f, 0.0f, 100.0f, 0.0f, 0.0f}},   {"cl-100", {0.0f, 0.0f, -100.0f, 0.0f, 0.0f}},
    {"tx+100", {0.0f, 0.0f, 0.0f, 100.0f, 0.0f}},   {"tx-100", {0.0f, 0.0f, 0.0f, -100.0f, 0.0f}},
    {"dh+100", {0.0f, 0.0f, 0.0f, 0.0f, 100.0f}},   {"dh-100", {0.0f, 0.0f, 0.0f, 0.0f, -100.0f}},
    {"mix", {-60.0f, 40.0f, 50.0f, -40.0f, 30.0f}},
};

static void test_options_tone_and_black_point(void)
{
    static const char *const NAMES[5] = {"big", "h_raf_u16", "mono_strips16", "raf_meta", "h_mono_lj92"};
    pc_raw_sensor sn = decoded("mono_levels");
    pc_raw_develop_options o = pc_raw_develop_options_default();
    char label[128];
    size_t i;
    size_t k;
    o.white_balance.kind = PC_RAW_WB_AUTO;
    develop_line("option mono_levels auto", &sn, &o);
    o.white_balance.kind = PC_RAW_WB_MULTIPLIERS;
    o.white_balance.multipliers[0] = 2.0;
    o.white_balance.multipliers[1] = 1.0;
    o.white_balance.multipliers[2] = 1.5;
    develop_line("option mono_levels mult", &sn, &o);
    o = pc_raw_develop_options_default();
    o.exposure = -1.0;
    develop_line("option mono_levels exposure", &sn, &o);
    pc_raw_sensor_fini(&sn);
    for (i = 0; i < 5; i++) {
        sn = decoded(NAMES[i]);
        for (k = 0; k < 11; k++) {
            o = pc_raw_develop_options_default();
            o.demosaic = PC_RAW_BILINEAR;
            o.tone = TONES[k].t;
            (void)snprintf(label, sizeof label, "tone %s %s", NAMES[i], TONES[k].name);
            develop_line(label, &sn, &o);
        }
        pc_raw_sensor_fini(&sn);
    }
    sn = decoded("big");
    sn.black_point = 0.33f;
    free(sn.tone_curve);
    sn.tone_curve = malloc(3 * sizeof *sn.tone_curve);
    if (sn.tone_curve != NULL) {
        static const float C[3][2] = {{0.01f, 0.02f}, {0.1f, 0.25f}, {1.0f, 1.0f}};
        memcpy(sn.tone_curve, C, sizeof C);
        sn.ntone_curve = 3;
    }
    o = pc_raw_develop_options_default();
    o.demosaic = PC_RAW_BILINEAR;
    develop_line("blackpoint none", &sn, &o);
    o.tone = TONES[0].t;
    develop_line("blackpoint sh+100", &sn, &o);
    o.tone = TONES[8].t;
    develop_line("blackpoint dh+100", &sn, &o);
    pc_raw_sensor_fini(&sn);
}

static void test_retone(void)
{
    static const float C[3][2] = {{0.01f, 0.02f}, {0.1f, 0.25f}, {1.0f, 1.0f}};
    size_t w = 300;
    size_t h = 120;
    float (*px)[4] = malloc(w * h * sizeof *px);
    float (*out)[4] = malloc(w * h * sizeof *out);
    char line[128];
    size_t i;
    size_t k;
    if (px == NULL || out == NULL) {
        free(px);
        free(out);
        PC_T_FAIL("out of memory");
        return;
    }
    for (i = 0; i < w * h; i++) {
        static const float Y[3] = {0.0078125f, 0.0625f, 0.5f};
        float y = Y[(i % w) * 3 / w];
        float v[3];
        v[0] = y;
        v[1] = y * 0.9f;
        v[2] = y * (1.0f + (float)(i % 7) * 0.01f);
        for (k = 0; k < 3; k++) {
            px[i][k] = powf(rx_tone(C, 3, v[k]), 1.0f / 1.8f);
        }
        px[i][3] = 1.0f;
    }
    for (k = 0; k < 12; k++) {
        pc_raw_tone t = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        uint64_t hh = H0;
        pc_error e;
        if (k > 0) {
            t = TONES[k - 1].t;
        }
        memcpy(out, px, w * h * sizeof *px);
        CHECK(pc_raw_retone(out, w, h, C, 3, &t, &e) == PC_OK);
        for (i = 0; i < w * h * 4; i++) {
            f32b(&hh, out[i / 4][i % 4]);
        }
        (void)snprintf(line, sizeof line, "retone %s %016llx", k == 0 ? "neutral" : TONES[k - 1].name,
                       (unsigned long long)hh);
        expect(line);
    }
    free(px);
    free(out);
}

/* ---- camera looks: look.rs's synthetic camera ---- */

#define SHOT_W 480
#define SHOT_H 320
static const double WB[3] = {1.5, 1.0, 1.25};

static float (*shot_cam(void))[3]
{
    float (*cam)[3] = tg_xalloc(SHOT_W * SHOT_H * sizeof *cam);
    float colours[96][3];
    uint32_t seed = 12345;
    size_t i;
    int k;
    for (i = 0; i < 96; i++) {
        for (k = 0; k < 3; k++) {
            seed = seed * 1664525u + 1013904223u;
            colours[i][k] = 0.15f + 0.45f * ((float)(seed >> 8) / (float)(1u << 24));
        }
    }
    for (i = 0; i < SHOT_W * SHOT_H; i++) {
        size_t x = i % SHOT_W;
        size_t y = i / SHOT_W;
        size_t px = x * 12 / SHOT_W;
        size_t py = y * 8 / SHOT_H;
        float rmp = 0.15f + 0.85f * (float)((x * 12) % SHOT_W) / (float)SHOT_W;
        for (k = 0; k < 3; k++) {
            cam[i][k] = colours[py * 12 + px][k] * rmp / (float)WB[k];
        }
    }
    return cam;
}

static uint8_t enc(double v)
{
    double e;
    v = pc_clampd(v, 0.0, 1.0);
    e = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
    return pc_f64_to_u8(round(e * 255.0));
}

static uint8_t *shot_reference(const float (*cam)[3])
{
    static const double K[3][3] = {{1.35, -0.25, -0.1}, {-0.15, 1.25, -0.1}, {0.0, -0.3, 1.3}};
    pc_raw_mat3 a = rx_xyz_d50_to_prophoto();
    pc_raw_mat3 b = rx_srgb_to_xyz_d50();
    pc_raw_mat3 k3;
    pc_raw_mat3 ab = rx_mul(&a, &b);
    pc_raw_mat3 m;
    pc_raw_mat3 to_srgb;
    pc_raw_mat3 romm = rx_rgb_to_xyz(RX_ROMM, RX_D50_XY);
    pc_raw_mat3 pro_to_srgb;
    uint8_t *rgb = tg_xalloc(SHOT_W * SHOT_H * 3);
    size_t i;
    int c;
    memcpy(k3.m, K, sizeof K);
    m = rx_mul(&ab, &k3);
    (void)rx_invert(&b, &to_srgb);
    pro_to_srgb = rx_mul(&to_srgb, &romm);
    for (i = 0; i < SHOT_W * SHOT_H; i++) {
        double bal[3];
        double pro[3];
        double s[3];
        for (c = 0; c < 3; c++) {
            bal[c] = (double)cam[i][c] * WB[c];
        }
        rx_apply(&m, bal, pro);
        for (c = 0; c < 3; c++) {
            double x = pc_clampd(pro[c], 0.0, 1.0);
            pro[c] = 1.6 * x / (1.0 + 0.6 * x);
        }
        rx_apply(&pro_to_srgb, pro, s);
        for (c = 0; c < 3; c++) {
            rgb[i * 3 + (size_t)c] = enc(s[c]);
        }
    }
    return rgb;
}

static uint64_t look_hash(const pc_raw_camera_look *l)
{
    uint64_t h = H0;
    size_t i;
    mat(&h, &l->to_xyz);
    if (l->has_tone) {
        double (*k)[2] = malloc(l->tone.n * sizeof *k);
        u64le(&h, l->tone.n);
        if (k != NULL) {
            pc_raw_look_curve_knots(&l->tone, k);
            for (i = 0; i < l->tone.n; i++) {
                f64b(&h, k[i][0]);
                f64b(&h, k[i][1]);
            }
            free(k);
        }
    } else {
        uint8_t ff = 0xFF;
        fnv(&h, &ff, 1);
    }
    f64b(&h, l->delta_e);
    f64b(&h, l->delta_e_fallback);
    return h;
}

static bool look_line(const char *name, const pc_raw_sensor *s, const pc_raw_develop_options *o, uint32_t w,
                      uint32_t h, const uint8_t *rgb, size_t len, pc_raw_camera_look *out)
{
    pc_raw_reference r;
    pc_error e;
    char line[600];
    bool ok;
    r.width = w;
    r.height = h;
    r.rgb = rgb;
    r.len = len;
    ok = pc_raw_fit_look(s, o, &r, out, &e) == PC_OK;
    if (ok) {
        (void)snprintf(line, sizeof line, "look %s %016llx", name, (unsigned long long)look_hash(out));
    } else {
        (void)snprintf(line, sizeof line, "look %s err %s", name, e.msg);
    }
    expect(line);
    return ok;
}

static void test_looks(void)
{
    float (*cam)[3] = shot_cam();
    uint16_t *data = tg_mosaic((const float (*)[3])cam, SHOT_W * SHOT_H, SHOT_W, RGGB, 0, 4095);
    tg_bytes raw = tg_tiff_ep("NIKON CORPORATION", SHOT_W, SHOT_H, data, RGGB, 12, NULL, 0);
    uint8_t *rgb = shot_reference((const float (*)[3])cam);
    size_t len = SHOT_W * SHOT_H * 3;
    pc_raw_limits lim = pc_raw_limits_default();
    pc_raw_sensor sn;
    pc_raw_sensor so;
    pc_raw_develop_options wb = pc_raw_develop_options_default();
    pc_raw_camera_look *look = malloc(sizeof *look);
    uint8_t *up = tg_xalloc(len);
    uint8_t *noise_rgb = tg_xalloc(len);
    uint8_t *grey = tg_xalloc(len);
    pc_error e;
    size_t i;
    uint32_t seed = 7;
    free(data);
    free(cam);
    if (look == NULL || pc_raw_decode(raw.d, raw.n, &lim, &sn, &e) != PC_OK) {
        PC_T_FAIL("shot does not decode");
        free(look);
        free(rgb);
        free(up);
        free(noise_rgb);
        free(grey);
        tg_free(&raw);
        return;
    }
    wb.white_balance.kind = PC_RAW_WB_MULTIPLIERS;
    memcpy(wb.white_balance.multipliers, WB, sizeof WB);
    if (look_line("shot", &sn, &wb, SHOT_W, SHOT_H, rgb, len, look)) {
        pc_raw_develop_options o = wb;
        o.camera_look = look;
        o.demosaic = PC_RAW_BILINEAR;
        develop_line("lookdev shot", &sn, &o);
    }
    so = sn;
    so.orientation = 6;
    for (i = 0; i < len / 3; i++) {
        size_t y2 = i / SHOT_H;
        size_t x2 = i % SHOT_H;
        size_t x = y2;
        size_t y = SHOT_H - 1 - x2;
        memcpy(up + i * 3, rgb + (y * SHOT_W + x) * 3, 3);
    }
    (void)look_line("shot_o6", &so, &wb, SHOT_H, SHOT_W, up, len, look);
    (void)look_line("shot_o6_unrotated", &so, &wb, SHOT_W, SHOT_H, rgb, len, look);
    for (i = 0; i < len; i++) {
        seed = seed * 1103515245u + 12345u;
        noise_rgb[i] = (uint8_t)(seed >> 16);
        grey[i] = 118;
    }
    (void)look_line("noise", &sn, &wb, SHOT_W, SHOT_H, noise_rgb, len, look);
    (void)look_line("grey", &sn, &wb, SHOT_W, SHOT_H, grey, len, look);
    (void)look_line("square", &sn, &wb, 320, 320, rgb, 320 * 320 * 3, look);
    (void)look_line("short", &sn, &wb, SHOT_W, SHOT_H, rgb, len - 1, look);
    (void)look_line("tiny", &sn, &wb, 12, 8, rgb, 12 * 8 * 3, look);
    (void)look_line("empty", &sn, &wb, 0, 0, rgb, 0, look);
    (void)look_line("huge", &sn, &wb, UINT32_MAX, UINT32_MAX, rgb, len, look);
    so = sn;
    so.crop.width += 10;
    (void)look_line("badcrop", &so, &wb, SHOT_W, SHOT_H, rgb, len, look);
    {
        pc_raw_develop_options bad = pc_raw_develop_options_default();
        bad.white_balance.kind = PC_RAW_WB_MULTIPLIERS;
        bad.white_balance.multipliers[0] = 0.0;
        bad.white_balance.multipliers[1] = 1.0;
        bad.white_balance.multipliers[2] = 1.0;
        (void)look_line("badwb", &sn, &bad, SHOT_W, SHOT_H, rgb, len, look);
    }
    for (i = 0; i < 4; i++) {
        static const double M[4][3][3] = {{{NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN}},
                                          {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
                                          {{1e9, 0, 0}, {0, 1, 0}, {0, 0, 1}},
                                          {{0.4, 0.35, 0.18}, {0.21, 0.72, 0.07}, {0.02, 0.12, 0.8}}};
        pc_raw_develop_options o = wb;
        char label[64];
        memset(look, 0, sizeof *look);
        memcpy(look->to_xyz.m, M[i], sizeof M[i]);
        look->delta_e = 1.0;
        look->delta_e_fallback = 2.0;
        o.camera_look = look;
        o.demosaic = PC_RAW_BILINEAR;
        (void)snprintf(label, sizeof label, "lookdev manual%zu", i);
        develop_line(label, &sn, &o);
    }
    pc_raw_sensor_fini(&sn);
    free(look);
    free(rgb);
    free(up);
    free(noise_rgb);
    free(grey);
    tg_free(&raw);
}

static void test_previews_and_colour_space(void)
{
    char line[2048];
    size_t i;
    for (i = 0; i < nall; i++) {
        pc_raw_preview p[PC_RAW_MAX_PREVIEWS];
        size_t n = pc_raw_embedded_previews(all[i].b.d, all[i].b.n, p, PC_RAW_MAX_PREVIEWS);
        size_t k;
        size_t o = (size_t)snprintf(line, sizeof line, "previews %s %zu", all[i].name, n);
        for (k = 0; k < n && o < sizeof line; k++) {
            o += (size_t)snprintf(line + o, sizeof line - o, " %ux%u:%zu", (unsigned)p[k].width,
                                  (unsigned)p[k].height, p[k].len);
        }
        expect(line);
    }
    for (i = 0; i < 7; i++) {
        static const int CS[7] = {1, 0xFFFF, 0xFFFF, 0xFFFF, 2, -1, 0xFFFF};
        static const char *const IX[7] = {"R98", "R03", "R98", NULL, "R03", "R03", "R03"};
        tg_tiff t;
        tg_entry ex[2];
        tg_entry e0[3];
        size_t nex = 0;
        size_t ne0 = 0;
        size_t exif;
        size_t i0;
        tg_bytes b;
        pc_raw_preview_color_space c;
        memset(&t, 0, sizeof t);
        if (CS[i] >= 0) {
            ex[nex].tag = 40961;
            ex[nex++].val = tg_v_short1((uint16_t)CS[i]);
        }
        if (IX[i] != NULL) {
            tg_entry ie;
            size_t interop;
            ie.tag = 1;
            ie.val = tg_v_ascii(IX[i]);
            interop = tg_ifd_add(&t, &ie, 1);
            ex[nex].tag = 40965;
            ex[nex++].val = tg_v_ref(TG_IFDS, interop);
        }
        exif = tg_ifd_add(&t, ex, nex);
        e0[ne0].tag = 271;
        e0[ne0++].val = tg_v_ascii("SONY");
        e0[ne0].tag = 34665;
        e0[ne0++].val = tg_v_ref(TG_IFDS, exif);
        if (i == 6) {
            static const uint8_t V[4] = {1, 6, 0, 0};
            e0[ne0].tag = 50706;
            e0[ne0++].val = tg_v_bytes(TG_BYTE, V, 4);
        }
        i0 = tg_ifd_add(&t, e0, ne0);
        tg_set_chain(&t, &i0, 1);
        b = tg_tiff_build(&t);
        tg_tiff_free(&t);
        (void)snprintf(line, sizeof line, "cspace %zu %s", i,
                       pc_raw_embedded_preview_color_space(b.d, b.n, &c)
                           ? (c == PC_RAW_PREVIEW_SRGB ? "Srgb" : "AdobeRgb")
                           : "none");
        expect(line);
        tg_free(&b);
    }
}

/* ---- hostile sweeps ---- */

static void exercise2(uint64_t *h, const uint8_t *b, size_t n)
{
    pc_raw_develop_options o = pc_raw_develop_options_default();
    pc_raw_format f;
    pc_raw_preview p[PC_RAW_MAX_PREVIEWS];
    pc_raw_preview_color_space cs;
    pc_raw_sensor s;
    pc_error e;
    uint8_t c;
    size_t np;
    size_t k;
    o.limits.max_alloc = (uint64_t)64 << 20;
    o.demosaic = PC_RAW_BILINEAR;
    c = pc_raw_identify(b, n, &f) ? (uint8_t)f : 0xFF;
    fnv(h, &c, 1);
    np = pc_raw_embedded_previews(b, n, p, PC_RAW_MAX_PREVIEWS);
    u64le(h, np);
    for (k = 0; k < np; k++) {
        u64le(h, p[k].width);
        u64le(h, p[k].height);
        u64le(h, p[k].len);
    }
    c = pc_raw_embedded_preview_color_space(b, n, &cs) ? (uint8_t)cs : 0xEE;
    fnv(h, &c, 1);
    if (pc_raw_decode(b, n, &o.limits, &s, &e) == PC_OK) {
        pc_raw_developed d;
        pc_raw_reference r;
        pc_raw_camera_look *look = malloc(sizeof *look);
        size_t w = s.crop.width < 256 ? s.crop.width : 256;
        size_t hh = s.crop.height < 256 ? s.crop.height : 256;
        uint8_t *rgb = malloc(w * hh * 3 + 1);
        u64le(h, sensor_hash2(&s));
        if (pc_raw_develop_sensor(&s, &o, &d, &e) == PC_OK) {
            u64le(h, developed_hash(&d));
            pc_raw_developed_fini(&d);
        } else {
            str0(h, e.msg);
        }
        if (look != NULL && rgb != NULL) {
            for (k = 0; k < w * hh * 3; k++) {
                rgb[k] = (uint8_t)(k * 7 % 251);
            }
            r.width = (uint32_t)w;
            r.height = (uint32_t)hh;
            r.rgb = rgb;
            r.len = w * hh * 3;
            if (pc_raw_fit_look(&s, &o, &r, look, &e) == PC_OK) {
                u64le(h, look_hash(look));
            } else {
                str0(h, e.msg);
            }
        }
        free(look);
        free(rgb);
        pc_raw_sensor_fini(&s);
    } else {
        str0(h, e.msg);
    }
}

static void test_hostile_sweeps(void)
{
    size_t first = nall;
    size_t i;
    char line[128];
    hostile_r3(true);
    for (i = first; i < nall; i++) {
        const tg_bytes *b = &all[i].b;
        uint64_t h = H0;
        uint64_t st = 0xDEADBEEFull ^ (uint64_t)strlen(all[i].name);
        size_t n;
        int it;
        for (n = 0; n < b->n; n += 3) {
            exercise2(&h, b->d, n);
        }
        (void)snprintf(line, sizeof line, "truncate %s %016llx", all[i].name, (unsigned long long)h);
        expect(line);
        h = H0;
#define NEXT() (st = st * 6364136223846793005ull + 1442695040888963407ull, st >> 33)
        for (it = 0; it < 150; it++) {
            uint8_t *c = tg_xalloc(b->n);
            uint64_t m;
            uint64_t j;
            memcpy(c, b->d, b->n);
            m = 1 + NEXT() % 6;
            for (j = 0; j < m; j++) {
                size_t at = (size_t)(NEXT() % b->n);
                uint64_t r = NEXT() % 4;
                c[at] = (uint8_t)(r == 0 ? 0xFF : r == 1 ? 0 : NEXT());
            }
            exercise2(&h, c, b->n);
            free(c);
        }
#undef NEXT
        (void)snprintf(line, sizeof line, "mutate %s %016llx", all[i].name, (unsigned long long)h);
        expect(line);
    }
}

int main(int argc, char **argv)
{
    size_t i;
    (void)argc;
    (void)argv;
    load_golden();
    build_samples();
    RUN(test_files_match);
    RUN(test_options_tone_and_black_point);
    RUN(test_retone);
    RUN(test_looks);
    RUN(test_previews_and_colour_space);
    RUN(test_hostile_sweeps);
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
