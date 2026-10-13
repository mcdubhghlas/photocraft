/* pyramid.rs fuse_stack tests: focus-stack fusion (Adelson et al. 1984, Wang & Chang 2011) with
 * halo control. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/pyramid.h"
#include "harness.h"

static const float K5[5] = {1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};

static float *checker_img(size_t w, size_t h, size_t cell, size_t ch)
{
    float *px = calloc(w * h * ch, sizeof *px);
    size_t x, y, c;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float v = ((x / cell) + (y / cell)) % 2 == 0 ? 0.2f : 0.8f;
            for (c = 0; c < ch; c++) {
                px[(y * w + x) * ch + c] = c == 0 ? v : c == 1 ? v * 0.9f : c == 2 ? v * 0.5f : 1.0f;
            }
        }
    }
    return px;
}

static float *luma_of(size_t w, size_t h, size_t ch, const float *px)
{
    float *l = malloc(w * h * sizeof *l);
    size_t i;
    for (i = 0; i < w * h; i++) {
        l[i] =
            ch >= 3 ? 0.2126f * px[i * ch] + 0.7152f * px[i * ch + 1] + 0.0722f * px[i * ch + 2] : px[i * ch];
    }
    return l;
}

/* Blur every channel with the 5-tap kernel `passes` times (no decimation). */
static float *blur_img(size_t w, size_t h, size_t ch, const float *px, size_t passes)
{
    float *out = malloc(w * h * ch * sizeof *out), *plane = malloc(w * h * sizeof *plane);
    size_t c, i, k;
    for (c = 0; c < ch; c++) {
        for (i = 0; i < w * h; i++) {
            plane[i] = px[i * ch + c];
        }
        for (k = 0; k < passes; k++) {
            float *next = NULL;
            CHECK(pc_algo_window_sum(w, h, plane, K5, 5, &next, NULL) == PC_OK);
            free(plane);
            plane = next;
        }
        for (i = 0; i < w * h; i++) {
            out[i * ch + c] = plane[i];
        }
    }
    free(plane);
    return out;
}

static float rms(const float *a, const float *b, size_t n)
{
    double s = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        s += (double)((a[i] - b[i]) * (a[i] - b[i]));
    }
    return (float)sqrt(s / (double)n);
}

static float *ones(size_t n)
{
    float *v = malloc(n * sizeof *v);
    size_t i;
    for (i = 0; i < n; i++) {
        v[i] = 1.0f;
    }
    return v;
}

static pc_algo_stack_fusion fusion(size_t er, float halo, size_t guide, size_t levels)
{
    pc_algo_stack_fusion p = pc_algo_stack_fusion_default();
    p.energy_radius = er, p.halo = halo, p.guide = guide;
    p.has_levels = levels > 0, p.levels = levels;
    return p;
}

static float *fuse(size_t w, size_t h, size_t ch, const float *const *im, const float *const *lm,
                   const float *const *al, size_t n, pc_algo_stack_fusion p)
{
    float *out = NULL;
    CHECK(pc_algo_fuse_stack(w, h, ch, im, lm, al, n, p, &out, NULL) == PC_OK);
    return out;
}

static void test_stack_levels_keep_the_residual_above_32_px(void)
{
    CHECK_EQ_UINT(pc_algo_stack_levels(64, 64), 2);
    CHECK_EQ_UINT(pc_algo_stack_levels(48, 100), 1);
    CHECK_EQ_UINT(pc_algo_stack_levels(2070, 1378), 6);
    CHECK_EQ_UINT(pc_algo_stack_levels(8280, 5520), 8);
    CHECK_EQ_UINT(pc_algo_stack_levels(100000, 100000), 10);
}

static void test_binomial_window_and_window_sum(void)
{
    float *b = NULL, *flat = malloc(35 * sizeof *flat), *s = NULL;
    size_t n = 0, i;
    CHECK(pc_algo_binomial(0, &b, &n, NULL) == PC_OK && n == 1 && b[0] == 1.0f);
    free(b);
    CHECK(pc_algo_binomial(1, &b, &n, NULL) == PC_OK && n == 3 && b[0] == 0.25f && b[1] == 0.5f &&
          b[2] == 0.25f);
    for (i = 0; i < 35; i++) {
        flat[i] = 0.3f;
    }
    CHECK(pc_algo_window_sum(7, 5, flat, b, n, &s, NULL) == PC_OK);
    for (i = 0; i < 35; i++) {
        CHECK(fabsf(s[i] - 0.3f) < 1e-6f); /* a window of unit weight keeps a flat plane */
    }
    free(b), free(s);
    CHECK(pc_algo_binomial(2, &b, &n, NULL) == PC_OK && n == 5);
    for (i = 0; i < 5; i++) {
        CHECK(fabsf(b[i] - K5[i]) < 1e-6f);
    }
    free(b);
    CHECK(pc_algo_binomial(0, &b, &n, NULL) == PC_OK);
    CHECK(pc_algo_window_sum(7, 5, flat, b, n, &s, NULL) == PC_OK);
    CHECK(memcmp(s, flat, 35 * sizeof *s) == 0);
    free(b), free(s), free(flat);
    /* The reference's O(r^2) table never ends for absurd radii. */
    CHECK(pc_algo_binomial((size_t)1 << 40, &b, &n, NULL) == PC_ERR_LIMIT);
}

static void test_deviation_and_entropy_measure_local_structure(void)
{
    const size_t w = 20, h = 20;
    float flat[400], *d = NULL, *e = NULL, *chk = checker_img(w, h, 1, 1), *l = luma_of(w, h, 1, chk);
    size_t i;
    for (i = 0; i < 400; i++) {
        flat[i] = 0.5f;
    }
    CHECK(pc_algo_deviation_entropy(w, h, flat, &d, &e, NULL) == PC_OK);
    for (i = 0; i < 400; i++) {
        CHECK(d[i] == 0.0f && e[i] == 0.0f);
    }
    free(d), free(e);
    CHECK(pc_algo_deviation_entropy(w, h, l, &d, &e, NULL) == PC_OK);
    CHECK(d[10 * w + 10] > 0.08f && e[10 * w + 10] > 0.6f);
    free(d), free(e), free(chk), free(l);
}

static void test_fuse_stack_identical_images_are_a_fixed_point(void)
{
    const size_t w = 70, h = 50, ch = 3;
    float *im = checker_img(w, h, 7, ch), *lm = luma_of(w, h, ch, im), *one = ones(w * h), *out;
    const float *ims[3] = {im, im, im}, *lms[3] = {lm, lm, lm}, *als[3] = {one, one, one};
    pc_algo_stack_fusion three = fusion(1, 2.0f, 2, 3), ps[4];
    size_t k;
    ps[0] = three, ps[1] = three, ps[2] = three, ps[3] = three;
    ps[1].halo = 0.0f, ps[2].guide = 1, ps[3].energy_radius = 0;
    for (k = 0; k < 4; k++) {
        out = fuse(w, h, ch, ims, lms, als, 3, ps[k]);
        CHECK(out != NULL && rms(out, im, w * h * ch) < 1e-5f);
        free(out);
    }
    /* One image, and none, are fine too. */
    out = fuse(w, h, ch, ims, lms, als, 1, three);
    CHECK(out != NULL && rms(out, im, w * h * ch) < 1e-5f);
    free(out);
    out = fuse(w, h, ch, ims, lms, als, 0, three);
    if (out != NULL) {
        bool zero = true;
        for (k = 0; k < w * h * ch; k++) {
            zero = zero && out[k] == 0.0f;
        }
        CHECK(zero);
    }
    free(out);
    /* More levels than the image has are clamped. */
    out = fuse(w, h, ch, ims, lms, als, 1, fusion(1, 2.0f, 2, 40));
    CHECK(out != NULL && rms(out, im, w * h * ch) < 1e-5f);
    free(out), free(im), free(lm), free(one);
}

static void test_fuse_stack_picks_the_sharp_half_from_each_image(void)
{
    const size_t w = 96, h = 64, ch = 4;
    float *sharp = checker_img(w, h, 6, ch), *soft = blur_img(w, h, ch, sharp, 6);
    float *a = malloc(w * h * ch * sizeof *a), *b = malloc(w * h * ch * sizeof *b), *la, *lb,
          *one = ones(w * h);
    const float *ims[2], *lms[2], *als[2];
    pc_algo_stack_fusion ps[3];
    float *out, *wts[2] = {NULL, NULL}, *old = NULL;
    size_t i, c, k;
    for (i = 0; i < w * h; i++) {
        for (c = 0; c < ch; c++) {
            bool right = i % w >= w / 2;
            a[i * ch + c] = right ? soft[i * ch + c] : sharp[i * ch + c];
            b[i * ch + c] = right ? sharp[i * ch + c] : soft[i * ch + c];
        }
    }
    la = luma_of(w, h, ch, a), lb = luma_of(w, h, ch, b);
    ims[0] = a, ims[1] = b, lms[0] = la, lms[1] = lb, als[0] = one, als[1] = one;
    ps[0] = fusion(1, 0.0f, 2, 0), ps[1] = fusion(0, 0.0f, 2, 0), ps[2] = fusion(1, 2.0f, 0, 0);
    for (k = 0; k < 3; k++) {
        float err, e1, e2, tol = ps[k].halo > 0.0f ? 1e-3f : 0.1f;
        bool opaque = true;
        out = fuse(w, h, ch, ims, lms, als, 2, ps[k]);
        if (out == NULL) {
            continue;
        }
        err = rms(out, sharp, w * h * ch);
        CHECK(err < 0.3f * rms(a, sharp, w * h * ch));
        for (i = 3; i < w * h * ch; i += ch) {
            opaque = opaque && fabsf(out[i] - 1.0f) < 1e-4f;
        }
        CHECK(opaque);
        e1 = fabsf(out[(20 * w + 10) * ch] - sharp[(20 * w + 10) * ch]);
        e2 = fabsf(out[(20 * w + 85) * ch + 1] - sharp[(20 * w + 85) * ch + 1]);
        CHECK(e1 < tol && e2 < tol);
        free(out);
    }
    /* The old full-resolution weights and spline blend do worse on the same pair. */
    CHECK(pc_algo_stack_weights(w, h, lms, als, 2, wts, NULL) == PC_OK);
    CHECK(pc_algo_pyramid_blend(w, h, ch, ims, (const float *const *)wts, 2,
                                pc_algo_pyramid_auto_levels(w, h), &old, NULL) == PC_OK);
    out = fuse(w, h, ch, ims, lms, als, 2, pc_algo_stack_fusion_default());
    CHECK(out != NULL && old != NULL && rms(out, sharp, w * h * ch) <= rms(old, sharp, w * h * ch));
    free(out), free(old), free(wts[0]), free(wts[1]);
    free(sharp), free(soft), free(a), free(b), free(la), free(lb), free(one);
}

static void test_fuse_stack_ignores_uncovered_pixels(void)
{
    const size_t w = 96, h = 64, ch = 4;
    static const size_t xs[3] = {60, 70, 90};
    float *sharp = checker_img(w, h, 6, ch), *a = malloc(w * h * ch * sizeof *a),
          *b = malloc(w * h * ch * sizeof *b);
    float *alpha_b = malloc(w * h * sizeof *alpha_b), *one = ones(w * h), *la, *lb, *out;
    const float *ims[2], *lms[2], *als[2];
    pc_algo_stack_fusion ps[2];
    size_t i, k, x, c;
    for (i = 0; i < w * h * ch; i++) {
        a[i] = i % ch == 3 ? 1.0f : 0.5f;
        b[i] = (i / ch) % w < w / 2 ? sharp[i] : i % ch == 3 ? 0.0f : 1.0f;
    }
    for (i = 0; i < w * h; i++) {
        alpha_b[i] = i % w < w / 2 ? 1.0f : 0.0f;
    }
    la = luma_of(w, h, ch, a), lb = luma_of(w, h, ch, b);
    ims[0] = a, ims[1] = b, lms[0] = la, lms[1] = lb, als[0] = one, als[1] = alpha_b;
    ps[0] = fusion(1, 0.0f, 2, 0), ps[1] = fusion(1, 2.0f, 0, 0);
    for (k = 0; k < 2; k++) {
        out = fuse(w, h, ch, ims, lms, als, 2, ps[k]);
        if (out == NULL) {
            continue;
        }
        CHECK(fabsf(out[(30 * w + 12) * ch] - sharp[(30 * w + 12) * ch]) < 0.05f);
        for (x = 0; x < 3; x++) {
            for (c = 0; c < 3; c++) {
                CHECK(fabsf(out[(30 * w + xs[x]) * ch + c] - 0.5f) < 0.03f);
            }
        }
        free(out);
    }
    free(sharp), free(a), free(b), free(alpha_b), free(one), free(la), free(lb);
}

static float halo_fuse(size_t w, size_t h, size_t ch, const float *x, const float *y, float halo,
                       const float *ref)
{
    float *lx = luma_of(w, h, ch, x), *ly = luma_of(w, h, ch, y), *one = ones(w * h), *out, r = 1e9f;
    const float *ims[2] = {x, y}, *lms[2] = {lx, ly}, *als[2] = {one, one};
    out = fuse(w, h, ch, ims, lms, als, 2, fusion(1, halo, 2, 5));
    if (out != NULL) {
        r = rms(out, ref, w * h * ch);
    }
    free(out), free(lx), free(ly), free(one);
    return r;
}

/* halo_control_keeps_the_coarse_levels_with_the_guide. */
static void test_halo_control_keeps_the_coarse_levels_with_the_guide(void)
{
    const size_t w = 192, h = 144, ch = 3;
    static const float halos[5] = {1.0f, 2.0f, 4.0f, 8.0f, 100.0f};
    float *a = checker_img(w, h, 6, ch), *b = blur_img(w, h, ch, a, 64), off;
    size_t x, y, c, k;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float dx = (float)x - 96.0f, dy = (float)y - 72.0f;
            float bump = 0.35f * expf(-(dx * dx + dy * dy) / (2.0f * (20.0f * 20.0f)));
            for (c = 0; c < ch; c++) {
                float *v = &b[(y * w + x) * ch + c];
                *v = fminf(*v + bump, 1.0f);
            }
        }
    }
    off = halo_fuse(w, h, ch, a, b, 0.0f, a);
    CHECK(off > 0.05f);
    for (k = 0; k < 5; k++) {
        CHECK(halo_fuse(w, h, ch, a, b, halos[k], a) < 0.15f * off);
    }
    CHECK(halo_fuse(w, h, ch, b, a, 2.0f, a) < 0.15f * off);
    CHECK(fabsf(pc_algo_halo_weight(1e-4f - 1e-8f, 3.0f) - 1.0f) < 1e-5f);
    CHECK(pc_algo_halo_weight(1e-3f, 2.0f) > pc_algo_halo_weight(1e-4f, 2.0f));
    CHECK(pc_algo_halo_weight(0.0f, 8.0f) > 0.0f && isfinite(pc_algo_halo_weight(1e3f, 8.0f)));
    free(a), free(b);
}

static void test_residual_rule_takes_the_image_that_wins_deviation_and_entropy(void)
{
    const size_t w = 40, h = 40, ch = 3, n = w * h * ch;
    float *a = checker_img(w, h, 2, ch), *b = malloc(n * sizeof *b), *la, *lb, *one = ones(w * h), *out,
          *mean;
    const float *ims[2], *lms[2], *als[2] = {one, one};
    size_t i;
    for (i = 0; i < n; i++) {
        b[i] = 0.5f;
    }
    la = luma_of(w, h, ch, a), lb = luma_of(w, h, ch, b);
    ims[0] = b, ims[1] = a, lms[0] = lb, lms[1] = la;
    out = fuse(w, h, ch, ims, lms, als, 2, fusion(1, 0.0f, 2, 1));
    CHECK(out != NULL && rms(out, a, n) < 1e-6f);
    free(out);
    /* Identical measures average (the Pareto rule keeps both). */
    lms[0] = la;
    out = fuse(w, h, ch, ims, lms, als, 2, fusion(1, 0.0f, 2, 1));
    mean = malloc(n * sizeof *mean);
    for (i = 0; i < n; i++) {
        mean[i] = (a[i] + b[i]) / 2.0f;
    }
    CHECK(out != NULL && rms(out, mean, n) < 1e-6f);
    free(out), free(mean), free(a), free(b), free(la), free(lb), free(one);
}

int main(int argc, char **argv)
{
    RUN(test_stack_levels_keep_the_residual_above_32_px);
    RUN(test_binomial_window_and_window_sum);
    RUN(test_deviation_and_entropy_measure_local_structure);
    RUN(test_fuse_stack_identical_images_are_a_fixed_point);
    RUN(test_fuse_stack_picks_the_sharp_half_from_each_image);
    RUN(test_fuse_stack_ignores_uncovered_pixels);
    RUN(test_halo_control_keeps_the_coarse_levels_with_the_guide);
    RUN(test_residual_rule_takes_the_image_that_wins_deviation_and_entropy);
    return pc_t_finish();
}
