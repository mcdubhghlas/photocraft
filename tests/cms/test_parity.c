/* Bit-exact parity with the Rust reference: replays `rustref cms-vectors` (same LCG, same
 * order, FNV-1a over raw little-endian bits) and `rustref icc-info` for the system ICC profiles
 * recorded in tests/golden/icc-info/system.txt (skipped when a file is absent or differs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/file.h"
#include "base/hash.h"
#include "base/mem.h"
#include "cms/builtin.h"
#include "cms/clut.h"
#include "cms/gamut.h"
#include "cms/lut3d.h"
#include "cms/lutfile.h"
#include "cms/synth.h"
#include "cms/transform.h"
#include "harness.h"

#define FNV0 0xcbf29ce484222325u

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
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
        uint8_t t[4];
        pc_store_le32(t, pc_f32_to_bits(v[i]));
        fnv(h, t, 4);
    }
}

static void h_f64(uint64_t *h, double v)
{
    uint8_t t[8];
    pc_store_le64(t, pc_f64_to_bits(v));
    fnv(h, t, 8);
}

static void h_byte(uint64_t *h, int v)
{
    uint8_t b = (uint8_t)(v != 0);
    fnv(h, &b, 1);
}

static void h_str(uint64_t *h, const char *s)
{
    fnv(h, s, strlen(s));
}

typedef struct lcg {
    uint64_t s;
} lcg;

static float lcg_next(lcg *r)
{
    r->s = r->s * 6364136223846793005u + 1442695040888963407u;
    return (float)(r->s >> 40) / 16777216.0f;
}

static void sweep_transform(uint64_t *h, const pc_transform *t)
{
    size_t ni = pc_transform_inputs(t);
    size_t no = pc_transform_outputs(t);
    lcg rng;
    float out[16];
    uint8_t *src8;
    uint8_t *dst8;
    uint16_t *src16;
    uint16_t *dst16;
    size_t i;
    size_t k;
    rng.s = (uint64_t)ni * 31 + no;
    memset(out, 0, sizeof out);
    for (i = 0; i < 64; i++) {
        float inp[16];
        memset(inp, 0, sizeof inp);
        for (k = 0; k < ni; k++) {
            inp[k] = i == 0 ? 0.0f : (i == 1 ? 1.0f : lcg_next(&rng));
        }
        pc_transform_eval(t, inp, out);
        h_f32(h, out, no);
        pc_transform_eval_fast(t, inp, out);
        h_f32(h, out, no);
    }
    src8 = pc_malloc_array(256, ni + 1);
    dst8 = pc_calloc_array(256, no + 1);
    src16 = pc_malloc_array(64 * ni + 1, sizeof *src16);
    dst16 = pc_calloc_array(64 * no + 1, sizeof *dst16);
    if (src8 == NULL || dst8 == NULL || src16 == NULL || dst16 == NULL) {
        PC_T_FAIL("%s", "out of memory");
        free(src8);
        free(dst8);
        free(src16);
        free(dst16);
        return;
    }
    for (i = 0; i < 256 * (ni + 1); i++) {
        src8[i] = (uint8_t)((i * 37 + 11) % 256);
    }
    CHECK_EQ_INT(pc_transform_convert_u8(t, src8, 256 * (ni + 1), ni + 1, dst8, 256 * (no + 1), no + 1, true, NULL),
                 PC_OK);
    fnv(h, dst8, 256 * (no + 1));
    for (i = 0; i < 64 * ni; i++) {
        src16[i] = (uint16_t)(((uint64_t)i * 2654435761u) % 65536u);
    }
    CHECK_EQ_INT(pc_transform_convert_u16(t, src16, 64 * ni, ni, dst16, 64 * no, no, false, NULL), PC_OK);
    for (i = 0; i < 64 * no; i++) {
        uint8_t b[2];
        pc_store_le16(b, dst16[i]);
        fnv(h, b, 2);
    }
    free(src8);
    free(dst8);
    free(src16);
    free(dst16);
}

static const pc_profile *bp(pc_builtin b)
{
    const pc_profile *p = pc_builtin_profile(b);
    if (p == NULL) {
        (void)fprintf(stderr, "built-in profile %d missing\n", (int)b);
    }
    return p;
}

static void blake3_hex_of(const pc_profile *p, char hex[65])
{
    pc_buf b = PC_BUF_INIT;
    hex[0] = '\0';
    if (pc_profile_to_bytes(p, &b, NULL) == PC_OK) {
        pc_blake3_hex(b.data, b.len, hex);
    }
    pc_buf_fini(&b);
}

static void test_builtin_bytes(void)
{
    static const char *const want[PC_BUILTIN_COUNT] = {
        "6b86846d783ee11c7932e9628c696fadc3d07f8550a48834d693ad139026a2c1",
        "04e0b689fd784aceb657638bcc1d320eb7bd1c4d3a4c6b05aabaaf27aa2d0c21",
        "857716b1182b32fae1f69de944e87e07dc6fb65008a4e649936dc6ef019223fc",
        "4121e975bc3508693770d2cf3d00f882c4a443fcc70b815d32f32341f59f48a3",
        "d55514a2164a301c32a6b5a7a26f05312b6b5e92ca8e4de60a439191322f2b54",
        "143a8b348d25f9a6e03c72111524863985c9f5156fe395b93155d300405e11e1",
        "6959484daefb448b4c48f7644c3d58b466663116cba2d0046e5d56806d671973",
        "c763f17cf887e41970f893a8d7e6fde6ab5439e7c669d903e1739ac262fde2b2",
        "9018919d2c0496d3dcb34d1d63c4e61dc0ba91d3e39c705e74032ff48c375bfb",
        "419ca84f4cec00a92dfa99cad4f7f51a8a4bb9c038c8d8f6150a8fdec0faed1a",
    };
    int b;
    for (b = 0; b < PC_BUILTIN_COUNT; b++) {
        const pc_profile *p = bp((pc_builtin)b);
        char hex[65];
        CHECK(p != NULL);
        if (p == NULL) {
            continue;
        }
        blake3_hex_of(p, hex);
        if (strcmp(hex, want[b]) != 0) {
            (void)fprintf(stderr, "  builtin %s differs\n", pc_builtin_id((pc_builtin)b));
        }
        CHECK_EQ_STR(hex, want[b]);
    }
}

static void test_transforms(void)
{
    static const pc_builtin set[7] = {PC_BUILTIN_SRGB, PC_BUILTIN_ADOBE_RGB_COMPAT, PC_BUILTIN_PROPHOTO_COMPAT,
                                      PC_BUILTIN_GRAY_GAMMA22, PC_BUILTIN_SGRAY, PC_BUILTIN_LAB_D50,
                                      PC_BUILTIN_COATED_CMYK};
    uint64_t ht = FNV0;
    uint64_t hp = FNV0;
    int s;
    int d;
    int intent;
    int bpc;
    int sim;
    for (s = 0; s < 7; s++) {
        for (d = 0; d < 7; d++) {
            for (intent = 0; intent < 4; intent++) {
                for (bpc = 0; bpc < 2; bpc++) {
                    pc_transform *t = NULL;
                    pc_error e;
                    if (pc_transform_new(bp(set[s]), bp(set[d]), (pc_intent)intent, bpc != 0, &t, &e) == PC_OK) {
                        sweep_transform(&ht, t);
                        pc_transform_release(t);
                    } else {
                        h_str(&ht, e.msg);
                    }
                }
            }
        }
    }
    CHECK_EQ_UINT(ht, 0x813b9aa7ef975315u);
    for (sim = 0; sim < 2; sim++) {
        for (intent = 0; intent < 4; intent++) {
            pc_transform *t = NULL;
            CHECK_EQ_INT(pc_transform_proof(bp(PC_BUILTIN_SRGB), bp(PC_BUILTIN_COATED_CMYK), bp(PC_BUILTIN_SRGB),
                                            (pc_intent)intent, true, sim != 0, &t, NULL),
                         PC_OK);
            if (t != NULL) {
                sweep_transform(&hp, t);
                pc_transform_release(t);
            }
        }
    }
    CHECK_EQ_UINT(hp, 0x062d04601dbafe48u);
}

static void test_curves(void)
{
    uint64_t hc = FNV0;
    double p1[7] = {2.0, 0.9, 0.1, 0.0, 0.0, 0.0, 0.0};
    double p2[7] = {2.0, 0.9, 0.1, 0.05, 0.0, 0.0, 0.0};
    double p3[7] = {2.4, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    double p4[7] = {2.4, 0.9, 0.05, 0.08, 0.04, 0.01, 0.002};
    double p5[7] = {2.4, 0.0, 0.05, 0.08, 0.04, 0.01, 0.002};
    static const float t1[7] = {0.0f, 0.1f, 0.1f, 0.5f, 0.9f, 1.0f, 1.0f};
    static const float t2[3] = {1.0f, 0.5f, 0.0f};
    static const float t3[1] = {0.3f};
    pc_cms_curve curves[12];
    float *tabs[3];
    size_t k;
    int i;
    tabs[0] = pc_malloc_array(7, sizeof(float));
    tabs[1] = pc_malloc_array(3, sizeof(float));
    tabs[2] = pc_malloc_array(1, sizeof(float));
    if (tabs[0] == NULL || tabs[1] == NULL || tabs[2] == NULL) {
        PC_T_FAIL("%s", "out of memory");
        return;
    }
    memcpy(tabs[0], t1, sizeof t1);
    memcpy(tabs[1], t2, sizeof t2);
    memcpy(tabs[2], t3, sizeof t3);
    curves[0] = pc_cms_curve_identity();
    curves[1] = pc_cms_curve_gamma(2.2);
    curves[2] = pc_cms_curve_gamma(0.0);
    curves[3] = pc_cms_srgb_trc();
    curves[4] = pc_cms_curve_parametric(1, p1);
    curves[5] = pc_cms_curve_parametric(2, p2);
    curves[6] = pc_cms_curve_parametric(3, p3);
    curves[7] = pc_cms_curve_parametric(4, p4);
    curves[8] = pc_cms_curve_parametric(4, p5);
    curves[9] = pc_cms_curve_table(tabs[0], 7);
    curves[10] = pc_cms_curve_table(tabs[1], 3);
    curves[11] = pc_cms_curve_table(tabs[2], 1);
    for (k = 0; k < 12; k++) {
        float s17[17];
        for (i = -20; i <= 140; i++) {
            double x = (double)i / 100.0;
            float f2[2];
            h_f64(&hc, pc_cms_curve_eval64(&curves[k], x));
            h_f64(&hc, pc_cms_curve_eval_inverse64(&curves[k], x));
            f2[0] = pc_cms_curve_eval(&curves[k], (float)x);
            f2[1] = pc_cms_curve_eval_inverse(&curves[k], (float)x);
            h_f32(&hc, f2, 2);
        }
        h_byte(&hc, pc_cms_curve_is_identity(&curves[k]));
        h_byte(&hc, pc_cms_curve_is_monotonic(&curves[k]));
        pc_cms_curve_sample(&curves[k], 17, s17);
        h_f32(&hc, s17, 17);
    }
    CHECK_EQ_UINT(hc, 0x086d561021c04f27u);
    for (k = 0; k < 12; k++) {
        pc_cms_curve_fini(&curves[k]);
    }
}

static void clut_fn(void *ctx, const float *p, float *o)
{
    size_t n = *(const size_t *)ctx;
    float s = -0.0f; /* Rust's f32 Sum */
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        s += p[i] * ((float)i + 1.0f) * 0.1f;
    }
    for (k = 0; k < 3; k++) {
        o[k] = sinf(s * ((float)k + 1.0f)) * 0.5f + 0.5f;
    }
}

static void test_cluts(void)
{
    static const size_t grids[5][5] = {{9}, {5, 6}, {9, 9, 9}, {5, 4, 3, 6}, {3, 3, 3, 3, 3}};
    static const size_t dims[5] = {1, 2, 3, 4, 5};
    uint64_t hl = FNV0;
    size_t gi;
    for (gi = 0; gi < 5; gi++) {
        pc_cms_clut c;
        lcg rng;
        float o[3];
        int it;
        size_t n = dims[gi];
        CHECK_EQ_INT(pc_cms_clut_sample(grids[gi], n, 3, clut_fn, &n, &c, NULL), PC_OK);
        rng.s = n;
        for (it = 0; it < 200; it++) {
            float inp[5];
            size_t d;
            for (d = 0; d < n; d++) {
                inp[d] = lcg_next(&rng) * 1.2f - 0.1f;
            }
            pc_cms_clut_eval(&c, inp, o);
            h_f32(&hl, o, 3);
            pc_cms_clut_eval_multilinear(&c, inp, o);
            h_f32(&hl, o, 3);
        }
        h_f32(&hl, c.data, c.len);
        pc_cms_clut_fini(&c);
    }
    CHECK_EQ_UINT(hl, 0xcf866b5826f8c5b4u);
}

static void test_profiles(void)
{
    uint64_t hb = FNV0;
    int b;
    int o;
    int intent;
    int input;
    pc_profile gray;
    pc_profile v2;
    bool ok = false;
    pc_buf bytes = PC_BUF_INIT;
    for (b = 0; b < PC_BUILTIN_COUNT; b++) {
        for (intent = 0; intent < 4; intent++) {
            for (input = 0; input < 2; input++) {
                double y = -1.0;
                if (pc_black_point(bp((pc_builtin)b), (pc_intent)intent, input != 0, &y, NULL) != PC_OK) {
                    y = -1.0;
                }
                h_f64(&hb, y);
            }
        }
        for (o = 0; o < PC_BUILTIN_COUNT; o++) {
            h_byte(&hb, pc_profile_same_colors(bp((pc_builtin)b), bp((pc_builtin)o)));
        }
        h_byte(&hb, pc_profile_is_matrix_shaper(bp((pc_builtin)b)));
    }
    CHECK_EQ_INT(pc_profile_gray_as_rgb(bp(PC_BUILTIN_SGRAY), &gray, &ok, NULL), PC_OK);
    CHECK(ok);
    CHECK_EQ_INT(pc_profile_to_bytes(&gray, &bytes, NULL), PC_OK);
    fnv(&hb, bytes.data, bytes.len);
    pc_buf_truncate(&bytes, 0);
    pc_profile_fini(&gray);
    /* sRGB the way Photoshop embeds it: v2, 1024-entry 16-bit TRC tables. */
    CHECK_EQ_INT(pc_profile_clone(bp(PC_BUILTIN_SRGB), &v2, NULL), PC_OK);
    for (b = 0; b < 3; b++) {
        float *t = pc_malloc_array(1024, sizeof *t);
        size_t i;
        if (t == NULL) {
            continue;
        }
        for (i = 0; i < 1024; i++) {
            t[i] = (float)(round(pc_cms_curve_eval64(&v2.trc[b], (double)i / 1023.0) * 65535.0) / 65535.0);
        }
        pc_cms_curve_fini(&v2.trc[b]);
        v2.trc[b] = pc_cms_curve_table(t, 1024);
    }
    v2.version_major = 2;
    v2.version_minor = 0x10;
    CHECK_EQ_INT(pc_profile_encode_bytes(&v2, NULL), PC_OK);
    CHECK_EQ_INT(pc_profile_to_bytes(&v2, &bytes, NULL), PC_OK);
    fnv(&hb, bytes.data, bytes.len);
    h_byte(&hb, pc_profile_same_colors(&v2, bp(PC_BUILTIN_SRGB)));
    CHECK_EQ_UINT(hb, 0xbe5b6da7ea3838eeu);
    pc_profile_fini(&v2);
    pc_buf_fini(&bytes);
}

static void test_gamut_and_lut3d(void)
{
    uint64_t hg = FNV0;
    pc_gamut_check *g = NULL;
    pc_transform *t = NULL;
    pc_lut3d lut;
    pc_buf b = PC_BUF_INIT;
    lcg rng;
    int i;
    CHECK_EQ_INT(pc_gamut_check_new(bp(PC_BUILTIN_SRGB), bp(PC_BUILTIN_COATED_CMYK), 4.0f, &g, NULL), PC_OK);
    rng.s = 99;
    for (i = 0; g != NULL && i < 500; i++) {
        float p[3];
        float de;
        p[0] = lcg_next(&rng);
        p[1] = lcg_next(&rng);
        p[2] = lcg_next(&rng);
        de = pc_gamut_check_delta_e(g, p);
        h_f32(&hg, &de, 1);
    }
    pc_gamut_check_free(g);
    CHECK_EQ_INT(pc_transform_new(bp(PC_BUILTIN_SRGB), bp(PC_BUILTIN_DISPLAY_P3), PC_INTENT_RELATIVE, false, &t, NULL),
                 PC_OK);
    CHECK_EQ_INT(pc_lut3d_from_transform(t, 17, &lut, NULL), PC_OK);
    CHECK_EQ_INT(pc_lut3d_to_rgba16f(&lut, &b, NULL), PC_OK);
    fnv(&hg, b.data, b.len);
    pc_buf_truncate(&b, 0);
    CHECK_EQ_INT(pc_lut3d_to_rgba8(&lut, &b, NULL), PC_OK);
    fnv(&hg, b.data, b.len);
    for (i = 0; i < 200; i++) {
        float p[3];
        float o[3];
        p[0] = lcg_next(&rng);
        p[1] = lcg_next(&rng);
        p[2] = lcg_next(&rng);
        pc_lut3d_sample(&lut, p, o);
        h_f32(&hg, o, 3);
    }
    CHECK_EQ_UINT(hg, 0xfc58d4c7720cdc05u);
    pc_lut3d_fini(&lut);
    pc_transform_release(t);
    pc_buf_fini(&b);
}

static void test_lutfiles(void)
{
    static const char *const texts[6][2] = {
        {"a.cube", "TITLE \"t\"\nLUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"},
        {"b.cube", "LUT_1D_SIZE 3\nLUT_1D_INPUT_RANGE 0.1 0.9\n0 0 0\n0.25 0.5 0.75\n1 1 1\n"},
        {"c.cube", "DOMAIN_MIN 0 0 0.5\nDOMAIN_MAX 1 1 1\nLUT_1D_SIZE 2\n0 0 0\n1 1 1\n"},
        {"d.3dl", "0 512 1023\n0 0 0\n0 0 1023\n0 1023 0\n0 1023 1023\n1023 0 0\n1023 0 1023\n1023 1023 0\n1023 1023 1023\n"},
        {"e.cube", "LUT_3D_SIZE 2\n0 0 0\n"},
        {"f.cube", "DOMAIN_MIN 1 0 0\nDOMAIN_MAX 1 1 1\nLUT_3D_SIZE 2\n"},
    };
    uint64_t hf = FNV0;
    size_t i;
    for (i = 0; i < PC_LUTFILE_BUILTIN_COUNT; i++) {
        pc_lutfile l;
        pc_buf b = PC_BUF_INIT;
        CHECK_EQ_INT(pc_lutfile_builtin(pc_lutfile_builtin_id(i), &l, NULL), PC_OK);
        h_f32(&hf, l.data, l.ndata);
        CHECK_EQ_INT(pc_lutfile_write_cube(&l, &b, NULL), PC_OK);
        fnv(&hf, b.data, b.len);
        pc_buf_fini(&b);
        pc_lutfile_fini(&l);
    }
    for (i = 0; i < 6; i++) {
        pc_lutfile l;
        pc_error e;
        if (pc_lutfile_parse(texts[i][0], (const uint8_t *)texts[i][1], strlen(texts[i][1]), &l, &e) == PC_OK) {
            h_f32(&hf, l.data, l.ndata);
            h_str(&hf, l.title);
            h_f32(&hf, l.domain_min, 3);
            h_f32(&hf, l.domain_max, 3);
            pc_lutfile_fini(&l);
        } else {
            h_str(&hf, e.msg);
        }
    }
    CHECK_EQ_UINT(hf, 0xb769d4bbca272df1u);
}

static void test_synth(void)
{
    static const double targets[5][3] = {{50, 0, 0}, {70, 10, 10}, {30, -20, 40}, {95, 0, 0}, {10, 0, 0}};
    uint64_t hs = FNV0;
    pc_cmyk_params p = pc_cmyk_params_default();
    pc_cmyk_model m;
    pc_profile prof;
    lcg rng;
    char hex[65];
    int i;
    int k;
    p.grid_a2b = 5;
    p.grid_b2a = 5;
    pc_cmyk_model_coated(p.tvi, &m);
    rng.s = 7;
    for (i = 0; i < 50; i++) {
        double c[4];
        double lab[3];
        for (k = 0; k < 4; k++) {
            c[k] = (double)lcg_next(&rng);
        }
        pc_cmyk_model_lab(&m, c, lab);
        for (k = 0; k < 3; k++) {
            h_f64(&hs, lab[k]);
        }
    }
    for (i = 0; i < 5; i++) {
        double cmyk[4];
        double de = pc_cmyk_separate(&m, &p, targets[i], NULL, cmyk);
        for (k = 0; k < 4; k++) {
            h_f64(&hs, cmyk[k]);
        }
        h_f64(&hs, de);
    }
    CHECK_EQ_UINT(hs, 0x905c6c4c02383504u);
    CHECK_EQ_INT(pc_synth_cmyk_profile(&p, &prof, NULL), PC_OK);
    blake3_hex_of(&prof, hex);
    CHECK_EQ_STR(hex, "954967cf6c196f24abc1e95553078d62a99361f6d985e358942eb3facd903623");
    pc_profile_fini(&prof);
}

/* The full default generator reproduces the current Rust generator byte for byte (BLAKE3 of
 * `synth::coated_cmyk().to_bytes()` from rustc 1.95). The shipped profile was made by an older
 * toolchain: it differs from today's output in 2 CLUT bytes, by one code each. Slow-ish (all
 * cores, ~0.2 s wall, ~6 s CPU): opt in with PHOTOCRAFT_SLOW_TESTS=1. */
static void test_synth_reproduces_shipped_profile(void)
{
    pc_profile prof;
    pc_buf b = PC_BUF_INIT;
    char hex[65];
    size_t i;
    size_t diffs = 0;
    int worst = 0;
    const char *slow = getenv("PHOTOCRAFT_SLOW_TESTS");
    if (slow == NULL || strcmp(slow, "1") != 0) {
        (void)printf("(slow synth regeneration skipped; PHOTOCRAFT_SLOW_TESTS=1 runs it) ");
        return;
    }
    CHECK_EQ_INT(pc_synth_coated_cmyk(&prof, NULL), PC_OK);
    CHECK_EQ_INT(pc_profile_to_bytes(&prof, &b, NULL), PC_OK);
    pc_blake3_hex(b.data, b.len, hex);
    CHECK_EQ_STR(hex, "07434dfc0beb4b5f6a22fb332dac22e2dbca0890c117ee930f689012d2e5f335");
    CHECK_EQ_UINT(b.len, pc_coated_cmyk_icc_len);
    for (i = 0; i < b.len && i < pc_coated_cmyk_icc_len; i++) {
        int d = (int)b.data[i] - (int)pc_coated_cmyk_icc[i];
        if (d != 0) {
            diffs++;
            worst = abs(d) > worst ? abs(d) : worst;
        }
    }
    CHECK(diffs <= 2 && worst <= 1);
    pc_buf_fini(&b);
    pc_profile_fini(&prof);
}

/* ---- icc-info: one line per profile, compared with goldens from the reference ---- */

static void icc_info_line(const uint8_t *bytes, size_t len, pc_buf *out)
{
    pc_profile p;
    pc_error e;
    uint64_t h = FNV0;
    static const pc_builtin dsts[2] = {PC_BUILTIN_LAB_D50, PC_BUILTIN_SRGB};
    int intent;
    int bpc;
    int d;
    int k;
    char luts[7];
    if (pc_profile_parse(bytes, len, &p, &e) != PC_OK) {
        (void)pc_buf_appendf(out, NULL, "error=%s", e.msg);
        return;
    }
    for (intent = 0; intent < 4; intent++) {
        for (bpc = 0; bpc < 2; bpc++) {
            for (d = 0; d < 2; d++) {
                pc_transform *t = NULL;
                if (pc_transform_new(&p, bp(dsts[d]), (pc_intent)intent, bpc != 0, &t, &e) == PC_OK) {
                    sweep_transform(&h, t);
                    pc_transform_release(t);
                } else {
                    h_str(&h, e.msg);
                }
                if (pc_transform_new(bp(dsts[d]), &p, (pc_intent)intent, bpc != 0, &t, &e) == PC_OK) {
                    sweep_transform(&h, t);
                    pc_transform_release(t);
                } else {
                    h_str(&h, e.msg);
                }
            }
        }
    }
    for (k = 0; k < 6; k++) {
        const pc_lut *l = k < 3 ? p.a2b[k] : p.b2a[k - 3];
        luts[k] = l == NULL ? '-' : (l->kind == PC_LUT8 ? '8' : (l->kind == PC_LUT16 ? 'w' : 'a'));
    }
    luts[6] = '\0';
    (void)pc_buf_appendf(out, NULL,
                         "desc=%s|cprt=%s|ver=%u.%u|class=%08lx|space=%08lx|pcs=%s|intent=%u|matrix=%d|trc=%d|gray=%d|"
                         "luts=%s|wtpt=[%llu, %llu, %llu]|hash=%016llx",
                         p.description, p.copyright, (unsigned)p.version_major, (unsigned)p.version_minor,
                         (unsigned long)p.class_sig, (unsigned long)p.space, p.pcs == PC_PCS_LAB ? "lab" : "xyz",
                         (unsigned)p.rendering_intent, p.has_matrix, p.has_trc, p.has_gray_trc, luts,
                         (unsigned long long)pc_f64_to_bits(p.white_point[0]),
                         (unsigned long long)pc_f64_to_bits(p.white_point[1]),
                         (unsigned long long)pc_f64_to_bits(p.white_point[2]), (unsigned long long)h);
    pc_profile_fini(&p);
}

static void test_system_profiles_match_reference(void)
{
    pc_buf golden = PC_BUF_INIT;
    const char *line;
    int compared = 0;
    int skipped = 0;
    if (pc_file_read("tests/golden/icc-info/system.txt", 1 << 22, &golden, NULL) != PC_OK) {
        (void)printf("(no tests/golden/icc-info/system.txt) ");
        return;
    }
    line = pc_buf_cstr(&golden);
    while (*line != '\0') {
        const char *end = strchr(line, '\n');
        size_t n = end != NULL ? (size_t)(end - line) : strlen(line);
        char *l = pc_strndup(line, n);
        char *sha;
        char *path;
        char *want;
        pc_buf file = PC_BUF_INIT;
        line += n + (end != NULL ? 1 : 0);
        if (l == NULL) {
            continue;
        }
        /* "<blake3>\t<path>\t<info>" */
        sha = l;
        path = strchr(sha, '\t');
        want = path != NULL ? strchr(path + 1, '\t') : NULL;
        if (path == NULL || want == NULL) {
            free(l);
            continue;
        }
        *path++ = '\0';
        *want++ = '\0';
        if (pc_file_read(path, 1 << 26, &file, NULL) == PC_OK) {
            char hex[65];
            /* The golden was made from a file with this content; skip if the system copy differs. */
            pc_blake3_hex(file.data, file.len, hex);
            if (strcmp(hex, sha) == 0) {
                pc_buf got = PC_BUF_INIT;
                icc_info_line(file.data, file.len, &got);
                if (strcmp(pc_buf_cstr(&got), want) != 0) {
                    (void)fprintf(stderr, "  %s\n    want %s\n    got  %s\n", path, want, pc_buf_cstr(&got));
                }
                CHECK_EQ_STR(pc_buf_cstr(&got), want);
                pc_buf_fini(&got);
                compared++;
            } else {
                skipped++;
            }
        } else {
            skipped++;
        }
        pc_buf_fini(&file);
        free(l);
    }
    (void)printf("(%d system profiles compared, %d skipped) ", compared, skipped);
    pc_buf_fini(&golden);
}

int main(int argc, char **argv)
{
    RUN(test_builtin_bytes);
    RUN(test_transforms);
    RUN(test_curves);
    RUN(test_cluts);
    RUN(test_profiles);
    RUN(test_gamut_and_lut3d);
    RUN(test_lutfiles);
    RUN(test_synth);
    RUN(test_synth_reproduces_shipped_profile);
    RUN(test_system_profiles_match_reference);
    return pc_t_finish();
}
