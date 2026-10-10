/* features.rs: Harris, BRIEF, matching, fits, RANSAC and registration. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/features.h"
#include "analysis_gen.h"
#include "harness.h"

static void test_harris_finds_square_corners(void)
{
    size_t w = 80, h = 80, x, y, n = 0, i;
    float *img = calloc(w * h, sizeof *img);
    pc_algo_corner *c = NULL;
    const float want[2][2] = {{30.0f, 30.0f}, {49.0f, 49.0f}};
    for (y = 30; y < 50; y++) {
        for (x = 30; x < 50; x++) {
            img[y * w + x] = 1.0f;
        }
    }
    CHECK(pc_algo_features_harris(w, h, img, 10, 5.0f, NULL, &c, &n, NULL) == PC_OK);
    CHECK(n >= 4);
    for (i = 0; i < 2; i++) {
        bool any = false;
        size_t k;
        for (k = 0; k < n; k++) {
            any = any || (fabsf(c[k].x - want[i][0]) <= 2.0f && fabsf(c[k].y - want[i][1]) <= 2.0f);
        }
        CHECK(any);
    }
    free(c);
    free(img);
}

static void apply_all(const pc_algo_homography *h, const double (*src)[2], double (*dst)[2], size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        pc_algo_homography_apply(h, src[i][0], src[i][1], &dst[i][0], &dst[i][1]);
    }
}

static void test_fits_are_exact_on_clean_data(void)
{
    const double src[5][2] = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {5, 3}};
    pc_algo_homography truth = {{1.1, 0.05, 3.0, -0.02, 0.95, -2.0, 0.001, 0.0005, 1.0}};
    pc_algo_homography sim = {{0.9, -0.2, 4.0, 0.2, 0.9, 1.0, 0.0, 0.0, 1.0}}, h;
    double dst[5][2];
    size_t i;
    apply_all(&truth, src, dst, 5);
    CHECK(pc_algo_features_fit(PC_ALGO_MODEL_HOMOGRAPHY, src, (const double (*)[2])dst, 5, &h));
    for (i = 0; i < 5; i++) {
        double ax, ay, bx, by;
        pc_algo_homography_apply(&h, src[i][0], src[i][1], &ax, &ay);
        pc_algo_homography_apply(&truth, src[i][0], src[i][1], &bx, &by);
        CHECK(fabs(ax - bx) < 1e-6 && fabs(ay - by) < 1e-6);
    }
    apply_all(&sim, src, dst, 5);
    CHECK(pc_algo_features_fit(PC_ALGO_MODEL_SIMILARITY, src, (const double (*)[2])dst, 5, &h));
    for (i = 0; i < 9; i++) {
        CHECK(fabs(h.m[i] - sim.m[i]) < 1e-9);
    }
}

static void test_ransac_rejects_outliers(void)
{
    double src[40][2], dst[40][2];
    size_t i, *inl = NULL, ninl = 0;
    pc_algo_homography h;
    bool found = false;
    for (i = 0; i < 40; i++) {
        double p0 = (double)(i * 7 % 50), p1 = (double)(i * 13 % 50);
        src[i][0] = p0, src[i][1] = p1;
        if (i % 4 == 0) {
            dst[i][0] = p0 * 3.0 + 40.0, dst[i][1] = -p1;
        } else {
            dst[i][0] = p0 + 12.0, dst[i][1] = p1 - 5.0;
        }
    }
    CHECK(pc_algo_features_ransac(PC_ALGO_MODEL_TRANSLATION, (const double (*)[2])src,
                                  (const double (*)[2])dst, 40, 100, 1.0, 1, &found, &h, &inl, &ninl,
                                  NULL) == PC_OK);
    CHECK(found);
    CHECK_EQ_UINT(ninl, 30);
    CHECK(fabs(h.m[2] - 12.0) < 1e-9 && fabs(h.m[5] + 5.0) < 1e-9);
    free(inl);
}

static void test_registers_translation_and_similarity(void)
{
    size_t w = 192, h = 160, n = 0;
    float *ref = gen_texture(w, h), *mov;
    pc_algo_homography shift = {{1.0, 0.0, 9.0, 0.0, 1.0, -6.0, 0.0, 0.0, 1.0}}, inv, m, sim;
    double a = 4.0 * (3.141592653589793 / 180.0), c = cos(a) * 1.03, s = sin(a) * 1.03;
    double cx = (double)w / 2.0, cy = (double)h / 2.0;
    const double pts[3][2] = {{40.0, 40.0}, {150.0, 120.0}, {96.0, 80.0}};
    bool found = false;
    int i;
    /* The moving image is the reference shifted by (+9, -6): registration maps it back. */
    CHECK(pc_algo_homography_inverse(&shift, &inv));
    mov = gen_warp(w, h, ref, &inv);
    CHECK(pc_algo_features_register(w, h, ref, mov, NULL, NULL, PC_ALGO_MODEL_TRANSLATION, &found, &m, &n,
                                    NULL) == PC_OK);
    CHECK(found && n >= 6);
    CHECK(fabs(m.m[2] + 9.0) < 1.0 && fabs(m.m[5] - 6.0) < 1.0);
    free(mov);
    /* Rotated 4 degrees and scaled 1.03 about the centre. */
    sim = (pc_algo_homography){{c, -s, cx - c * cx + s * cy, s, c, cy - s * cx - c * cy, 0.0, 0.0, 1.0}};
    CHECK(pc_algo_homography_inverse(&sim, &inv));
    mov = gen_warp(w, h, ref, &inv);
    found = false;
    CHECK(pc_algo_features_register(w, h, ref, mov, NULL, NULL, PC_ALGO_MODEL_SIMILARITY, &found, &m, &n,
                                    NULL) == PC_OK);
    CHECK(found);
    for (i = 0; found && i < 3; i++) {
        double x, y, bx, by;
        pc_algo_homography_apply(&sim, pts[i][0], pts[i][1], &x, &y);
        pc_algo_homography_apply(&m, x, y, &bx, &by);
        CHECK(fabs(bx - pts[i][0]) < 1.5 && fabs(by - pts[i][1]) < 1.5);
    }
    free(mov);
    free(ref);
}

static void test_descriptors_are_deterministic(void)
{
    size_t w = 96, h = 96, n = 0;
    float *img = gen_texture(w, h);
    pc_algo_corner *c = NULL;
    pc_algo_descriptor *d1 = NULL, *d2 = NULL;
    pc_algo_descriptor z = {{0, 0, 0, 0}}, o = {{1, 3, 0, UINT64_MAX}};
    CHECK(pc_algo_features_harris(w, h, img, 50, 6.0f, NULL, &c, &n, NULL) == PC_OK);
    CHECK(n > 0);
    CHECK(pc_algo_features_describe(w, h, img, c, n, false, &d1, NULL) == PC_OK);
    CHECK(pc_algo_features_describe(w, h, img, c, n, false, &d2, NULL) == PC_OK);
    CHECK(d1 != NULL && d2 != NULL && memcmp(d1, d2, n * sizeof *d1) == 0);
    CHECK_EQ_UINT(pc_algo_features_hamming(&z, &o), 67);
    free(c), free(d1), free(d2), free(img);
}

static void test_never_crash(void)
{
    size_t n = 7;
    pc_algo_corner *c = (pc_algo_corner *)1, corner = {0.0f, 0.0f, 1.0f};
    pc_algo_descriptor *d = NULL;
    pc_algo_match *m = (pc_algo_match *)1;
    float *flat = calloc(64 * 64, sizeof *flat), *nanimg = malloc(64 * 64 * sizeof *nanimg);
    bool *valid = calloc(64 * 64, sizeof *valid), found = true;
    const double one[1][2] = {{1.0, 2.0}};
    pc_algo_homography h;
    size_t *inl = NULL, ninl = 9, i;
    for (i = 0; i < 64 * 64; i++) {
        nanimg[i] = (i % 7 == 0) ? NAN : (float)(i % 13) / 13.0f;
    }
    /* Tiny, empty, flat, NaN and fully masked images find no corners or behave. */
    CHECK(pc_algo_features_harris(0, 0, NULL, 10, 3.0f, NULL, &c, &n, NULL) == PC_OK && n == 0 && c == NULL);
    CHECK(pc_algo_features_harris(64, 64, flat, 10, 3.0f, NULL, &c, &n, NULL) == PC_OK && n == 0);
    CHECK(pc_algo_features_harris(64, 64, nanimg, 10, NAN, NULL, &c, &n, NULL) == PC_OK);
    free(c);
    CHECK(pc_algo_features_harris(64, 64, nanimg, 0, INFINITY, valid, &c, &n, NULL) == PC_OK && n == 0);
    free(c);
    CHECK(pc_algo_features_harris(64, 64, nanimg, 0, -5.0f, NULL, &c, &n, NULL) == PC_OK && n <= 1);
    free(c);
    /* A corner outside the image is sampled at the clamped edge. */
    corner.x = 1e30f, corner.y = -1e30f;
    CHECK(pc_algo_features_describe(64, 64, flat, &corner, 1, false, &d, NULL) == PC_OK);
    free(d);
    CHECK(pc_algo_features_describe(0, 64, flat, &corner, 1, false, &d, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_features_match(NULL, 0, NULL, 0, 0.8f, &m, &n, NULL) == PC_OK && n == 0 && m == NULL);
    CHECK(!pc_algo_features_fit(PC_ALGO_MODEL_TRANSLATION, one, one, 0, &h));
    CHECK(!pc_algo_features_fit(PC_ALGO_MODEL_SIMILARITY, one, one, 1, &h));
    CHECK(!pc_algo_features_fit(PC_ALGO_MODEL_HOMOGRAPHY, one, one, 1, &h));
    CHECK(pc_algo_features_ransac(PC_ALGO_MODEL_HOMOGRAPHY, one, one, 1, 10, 1.0, 0, &found, &h, &inl, &ninl,
                                  NULL) == PC_OK &&
          !found);
    /* Exactly the minimal set, and degenerate (all identical) points. */
    {
        const double same[4][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}};
        CHECK(pc_algo_features_ransac(PC_ALGO_MODEL_HOMOGRAPHY, same, same, 4, 5, 1.0, 3, &found, &h, &inl,
                                      &ninl, NULL) == PC_OK &&
              !found);
        CHECK(pc_algo_features_ransac(PC_ALGO_MODEL_TRANSLATION, same, same, 1, 0, 1.0, 3, &found, &h, &inl,
                                      &ninl, NULL) == PC_OK &&
              found && ninl == 1);
        free(inl);
    }
    CHECK(pc_algo_features_register(64, 64, flat, flat, NULL, NULL, PC_ALGO_MODEL_HOMOGRAPHY, &found, &h, &n,
                                    NULL) == PC_OK &&
          !found);
    CHECK(pc_algo_features_register(8, 8, flat, flat, NULL, NULL, PC_ALGO_MODEL_HOMOGRAPHY, &found, &h, &n,
                                    NULL) == PC_OK &&
          !found);
    free(flat), free(nanimg), free(valid);
}

int main(int argc, char **argv)
{
    RUN(test_harris_finds_square_corners);
    RUN(test_fits_are_exact_on_clean_data);
    RUN(test_ransac_rejects_outliers);
    RUN(test_registers_translation_and_similarity);
    RUN(test_descriptors_are_deterministic);
    RUN(test_never_crash);
    return pc_t_finish();
}
