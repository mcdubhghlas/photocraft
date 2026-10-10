/* Merge to HDR Pro (port of the tests in crates/algo/src/hdr.rs) plus never-crash checks. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

/* A radiance map with a wide range (12 stops left to right). */
static float (*scene(size_t w, size_t h))[3]
{
    float (*s)[3] = malloc(w * h * sizeof *s);
    size_t i;
    for (i = 0; i < w * h; i++) {
        float x = (float)(i % w) / (float)w, y = (float)(i / w) / (float)h;
        float base = 0.002f * exp2f(x * 12.0f), tex = 1.0f + 0.3f * sinf(y * 40.0f);
        s[i][0] = base * tex;
        s[i][1] = base * tex * 0.8f;
        s[i][2] = base * tex * 0.6f;
    }
    return s;
}

static float camera(float e)
{
    float v = powf(e > 0.0f ? e : 0.0f, 0.45f);
    return v < 1.0f ? v : 1.0f;
}

static float (*shoot(const float (*sc)[3], size_t n, float dt))[4]
{
    float (*px)[4] = malloc(n * sizeof *px);
    size_t i;
    int c;
    for (i = 0; i < n; i++) {
        for (c = 0; c < 3; c++) {
            px[i][c] = roundf(camera(sc[i][c] * dt) * 255.0f) / 255.0f;
        }
        px[i][3] = roundf(1.0f * 255.0f) / 255.0f;
    }
    return px;
}

static void test_exposure_estimation_and_merge_recover_radiance(void)
{
    const size_t w = 192, h = 64, n = w * h;
    float (*sc)[3] = scene(w, h), (*shots[4])[4];
    const float dts[4] = {0.25f, 1.0f, 4.0f, 16.0f};
    double est[4], ex[4], k0;
    pc_algo_hdr_img refs[4];
    pc_algo_hdr_merge_opts o;
    pc_algo_hdr_merged m;
    size_t k, xs[5] = {10, 50, 100, 150, 170};
    for (k = 0; k < 4; k++) {
        shots[k] = shoot((const float (*)[3])sc, n, dts[k]);
        refs[k].px = (const float (*)[4])shots[k];
        refs[k].n = n;
        ex[k] = (double)dts[k];
    }
    CHECK(pc_algo_hdr_estimate_exposures(refs, 4, est, NULL) == PC_OK);
    for (k = 1; k < 4; k++) {
        double r = est[k] / est[k - 1];
        CHECK(r >= 2.0 && r < 8.0);
    }
    memset(&o, 0, sizeof o);
    o.exposures = ex;
    o.n_exposures = 4;
    CHECK(pc_algo_hdr_merge(w, h, refs, 4, &o, &m, NULL) == PC_OK);
#define PROBE(x) ((double)m.px[(h / 2) * w + (x)][1] / (double)sc[(h / 2) * w + (x)][1])
    k0 = PROBE(w / 2);
    for (k = 0; k < 5; k++) {
        double r = PROBE(xs[k]) / k0;
        CHECK(fabs(log(r)) < 0.25);
    }
#undef PROBE
    CHECK(m.stops > 9.0);
    for (k = 1; k < 256; k++) {
        CHECK(m.response[0][k] > m.response[0][k - 1]);
    }
    pc_algo_hdr_merged_fini(&m);
    for (k = 0; k < 4; k++) {
        free(shots[k]);
    }
    free(sc);
}

static void test_ghosts_come_from_the_base_exposure(void)
{
    const size_t w = 64, h = 64, n = w * h;
    float (*sc)[3] = malloc(n * sizeof *sc), (*shots[3])[4];
    const float dts[3] = {0.5f, 1.0f, 2.0f};
    double ex[3] = {0.5, 1.0, 2.0};
    pc_algo_hdr_img refs[3];
    pc_algo_hdr_merge_opts o;
    pc_algo_hdr_merged plain, clean;
    size_t i, x, y;
    float bg;
    for (i = 0; i < n; i++) {
        sc[i][0] = sc[i][1] = sc[i][2] = 0.2f;
    }
    for (i = 0; i < 3; i++) {
        shots[i] = shoot((const float (*)[3])sc, n, dts[i]);
        refs[i].px = (const float (*)[4])shots[i];
        refs[i].n = n;
    }
    /* A "person" only in the third exposure. */
    for (y = 20; y < 40; y++) {
        for (x = 20; x < 40; x++) {
            shots[2][y * w + x][0] = shots[2][y * w + x][1] = shots[2][y * w + x][2] = 0.2f;
            shots[2][y * w + x][3] = 1.0f;
        }
    }
    memset(&o, 0, sizeof o);
    o.exposures = ex;
    o.n_exposures = 3;
    o.has_ghost_base = true;
    o.ghost_base = 1;
    CHECK(pc_algo_hdr_merge(w, h, refs, 3, &o, &plain, NULL) == PC_OK);
    o.remove_ghosts = true;
    CHECK(pc_algo_hdr_merge(w, h, refs, 3, &o, &clean, NULL) == PC_OK);
    bg = clean.px[5 * w + 5][1];
    CHECK(fabsf(clean.px[30 * w + 30][1] / bg - 1.0f) < 0.15f);
    CHECK(fabsf(plain.px[30 * w + 30][1] / plain.px[5 * w + 5][1] - 1.0f) > 0.2f);
    CHECK(clean.ghost_fraction > 0.05 && clean.ghost_base == 1);
    pc_algo_hdr_merged_fini(&plain), pc_algo_hdr_merged_fini(&clean);
    for (i = 0; i < 3; i++) {
        free(shots[i]);
    }
    free(sc);
}

static void test_mtb_finds_shifts(void)
{
    const size_t w = 128, h = 96, n = w * h;
    float *img = malloc(n * sizeof *img), *shifted = malloc(n * sizeof *shifted);
    const int32_t dx = 5, dy = -3;
    int32_t ox = 0, oy = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        float x = (float)(i % w), y = (float)(i / w);
        img[i] = 0.5f + 0.4f * (sinf(x * 0.21f) * cosf(y * 0.17f));
    }
    for (i = 0; i < n; i++) {
        int32_t x = (int32_t)(i % w) - dx, y = (int32_t)(i / w) - dy;
        x = x < 0 ? 0 : (x > (int32_t)w - 1 ? (int32_t)w - 1 : x);
        y = y < 0 ? 0 : (y > (int32_t)h - 1 ? (int32_t)h - 1 : y);
        /* Darker exposure of the same scene, shifted. */
        shifted[i] = img[(size_t)y * w + (size_t)x] * 0.6f;
    }
    CHECK(pc_algo_hdr_mtb_offset(w, h, shifted, img, 4, &ox, &oy, NULL) == PC_OK);
    CHECK_EQ_INT(ox, dx);
    CHECK_EQ_INT(oy, dy);
    free(img), free(shifted);
}

static void test_tone_mapping_methods_produce_display_range(void)
{
    const size_t w = 96, h = 32, n = w * h;
    float (*sc)[3] = scene(w, h), (*px)[4] = malloc(n * sizeof *px);
    pc_algo_hdr_tone_method m;
    int k;
    size_t i;
    for (k = 0; k < 4; k++) {
        memset(&m, 0, sizeof m);
        m.kind = (pc_algo_hdr_tone_kind)k;
        m.local = pc_algo_hdr_toning_default();
        m.local.radius = 7.0f;
        m.local.strength = 0.52f;
        m.exposure = 0.0f;
        m.gamma = 1.0f;
        for (i = 0; i < n; i++) {
            px[i][0] = sc[i][0], px[i][1] = sc[i][1], px[i][2] = sc[i][2], px[i][3] = 1.0f;
        }
        CHECK(pc_algo_hdr_tone_map(px, n, w, h, &m, NULL) == PC_OK);
        for (i = 0; i < n; i++) {
            int c;
            for (c = 0; c < 3; c++) {
                CHECK(px[i][c] >= 0.0f && px[i][c] <= 1.0f);
            }
        }
#define ROW(x) px[(h / 2) * w + (x)][1]
        CHECK(ROW(w - 2) > ROW(2));
        if (m.kind != PC_ALGO_HDR_EXPOSURE_GAMMA) {
            /* Compression keeps both ends of a 12-stop ramp visible. */
            CHECK(ROW(4) > 0.01f && ROW(w - 4) < 0.999f);
        }
#undef ROW
    }
    free(px), free(sc);
}

static void test_hostile_inputs_never_crash(void)
{
    float px[16][4], *grey = calloc(256, sizeof *grey);
    double ex[2] = {NAN, -1.0}, g[256];
    pc_algo_hdr_img refs[2] = {{(const float (*)[4])px, 16}, {(const float (*)[4])px, 16}};
    pc_algo_hdr_merge_opts o;
    pc_algo_hdr_merged m;
    pc_algo_hdr_tone_method t;
    int32_t ox, oy;
    size_t i, *samples = NULL, ns = 0;
    int k;
    uint8_t z[4] = {0, 255, 128, 7};
    double ldt[2] = {0.0, INFINITY};
    for (i = 0; i < 16; i++) {
        px[i][0] = i % 2 ? NAN : INFINITY, px[i][1] = -5.0f, px[i][2] = 0.5f, px[i][3] = (float)i / 15.0f;
    }
    memset(&o, 0, sizeof o);
    o.exposures = ex;
    o.n_exposures = 2;
    o.remove_ghosts = true;
    CHECK(pc_algo_hdr_merge(4, 4, refs, 2, &o, &m, NULL) == PC_OK);
    pc_algo_hdr_merged_fini(&m);
    CHECK(pc_algo_hdr_merge(4, 4, refs, 0, &o, &m, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_hdr_merge(5, 4, refs, 2, &o, &m, NULL) == PC_ERR_INVALID);
    o.n_exposures = 1;
    CHECK(pc_algo_hdr_merge(4, 4, refs, 2, &o, &m, NULL) == PC_ERR_INVALID);
    o.n_exposures = 2;
    CHECK(pc_algo_hdr_merge(0, 0, (pc_algo_hdr_img[2]){{NULL, 0}, {NULL, 0}}, 2, &o, &m, NULL) == PC_OK);
    pc_algo_hdr_merged_fini(&m);
    CHECK(pc_algo_hdr_estimate_exposures(refs, 2, ex, NULL) == PC_OK);
    CHECK(pc_algo_hdr_estimate_exposures(refs, 0, ex, NULL) == PC_OK);
    CHECK(pc_algo_hdr_mtb_offset(16, 16, grey, grey, 1000, &ox, &oy, NULL) == PC_OK);
    CHECK(pc_algo_hdr_sample_pixels(4, 4, (const float (*)[4])px, 0, &samples, &ns, NULL) == PC_OK);
    free(samples);
    CHECK(pc_algo_hdr_response_curve(z, 2, 2, ldt, NAN, g, NULL) == PC_OK);
    for (k = 0; k < 4; k++) {
        float q[16][4];
        memcpy(q, px, sizeof q);
        memset(&t, 0, sizeof t);
        t.kind = (pc_algo_hdr_tone_kind)k;
        t.local = pc_algo_hdr_toning_default();
        t.exposure = NAN;
        t.gamma = -INFINITY;
        CHECK(pc_algo_hdr_tone_map(q, 16, 4, 4, &t, NULL) == PC_OK);
        CHECK(pc_algo_hdr_tone_map(q, 15, 4, 4, &t, NULL) == PC_ERR_INVALID);
        CHECK(pc_algo_hdr_tone_map(NULL, 0, 0, 0, &t, NULL) == PC_OK);
    }
    free(grey);
}

int main(int argc, char **argv)
{
    RUN(test_exposure_estimation_and_merge_recover_radiance);
    RUN(test_ghosts_come_from_the_base_exposure);
    RUN(test_mtb_finds_shifts);
    RUN(test_tone_mapping_methods_produce_display_range);
    RUN(test_hostile_inputs_never_crash);
    return pc_t_finish();
}
