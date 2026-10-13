/* Content-Aware Fill (src/algo/content_aware.c, content_aware_auto.c): the
 * crates/algo/src/content_aware.rs tests plus never-crash cases. */
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

/* Vertical stripes (period 6) in grey, with a 7x7 hole in the middle. */
static void stripes(size_t w, size_t h, float **im, bool **hole)
{
    size_t i;
    *im = malloc(w * h * sizeof(float));
    *hole = malloc(w * h);
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        (*im)[i] = x % 6 < 3 ? 0.2f : 0.8f;
        (*hole)[i] = (x > w / 2 ? x - w / 2 : w / 2 - x) < 4 && (y > h / 2 ? y - h / 2 : h / 2 - y) < 4;
    }
}

static bool *all_of(size_t n, bool v)
{
    bool *m = malloc(n + 1);
    memset(m, v, n);
    return m;
}

/* I.i.d. noise (w * h * 3 values): every value occurs once, so a filled pixel equal to an image
 * pixel was copied. */
static float *noise(size_t w, size_t h, uint64_t seed)
{
    float *v = malloc(w * h * 3 * sizeof(float));
    uint64_t z = seed;
    size_t i;
    for (i = 0; i < w * h * 3; i++) {
        z = z * 6364136223846793005ull + 1442695040888963407ull;
        v[i] = 0.2f + 0.6f * ((float)(z >> 40) / (float)(1ull << 24));
    }
    return v;
}

static bool yes(void *ctx)
{
    (void)ctx;
    return true;
}

static atomic_uint LAST;
static atomic_bool BACKWARDS;

static void track(void *ctx, float f)
{
    unsigned int prev = atomic_load(&LAST), u;
    float last;
    (void)ctx;
    memcpy(&last, &prev, 4);
    if (f < last) {
        atomic_store(&BACKWARDS, true);
    }
    memcpy(&u, &f, 4);
    atomic_store(&LAST, u);
}

static void test_cancelled_fill_stops_and_progress_reaches_one(void)
{
    size_t w = 60, h = 40;
    float *im, *out = NULL, *plain = NULL, last;
    bool *hole, *src = all_of(w * h, true);
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    pc_interrupt stop = {yes, NULL, NULL}, prog = {NULL, track, NULL};
    unsigned int u;
    stripes(w, h, &im, &hole);
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, &stop, &out, NULL) == PC_ERR_CANCELLED);
    CHECK(out == NULL);
    atomic_store(&LAST, 0u);
    atomic_store(&BACKWARDS, false);
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, &prog, &out, NULL) == PC_OK);
    u = atomic_load(&LAST);
    memcpy(&last, &u, 4);
    CHECK(last == 1.0f);
    CHECK(!atomic_load(&BACKWARDS));
    /* Same result as the plain fill. */
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, NULL, &plain, NULL) == PC_OK);
    CHECK(out != NULL && plain != NULL && memcmp(out, plain, w * h * sizeof(float)) == 0);
    free(im), free(hole), free(src), free(out), free(plain);
}

static void test_filling_in_place_is_filling_a_copy(void)
{
    size_t w = 90, h = 70, n = w * h, i, k;
    float *rgb = noise(w, h, 4), *copy = NULL, *owned = malloc(n * 3 * sizeof(float));
    bool *hole = malloc(n), *source = malloc(n), *nowhere = all_of(n, false);
    pc_algo_ca_fill_options opts[2];
    for (i = 0; i < n * 3; i++) {
        rgb[i] = rgb[i] * 0.5f + 0.3f * (float)((i / 3 % w) % 11 < 5);
    }
    for (i = 0; i < n; i++) {
        hole[i] = i % w >= 30 && i % w < 52 && i / w >= 25 && i / w < 44;
        source[i] = i % w < 75;
    }
    opts[0] = pc_algo_ca_fill_options_default();
    opts[1] = pc_algo_ca_fill_options_default();
    opts[1].color_adaptation = PC_ALGO_ADAPT_NONE, opts[1].mirror = true;
    for (k = 0; k < 2; k++) {
        bool only_hole = true;
        CHECK(pc_algo_ca_fill(w, h, 3, rgb, n * 3, hole, source, n, &opts[k], NULL, &copy, NULL) == PC_OK);
        memcpy(owned, rgb, n * 3 * sizeof(float));
        CHECK(pc_algo_ca_fill_inplace(w, h, 3, owned, n * 3, hole, source, n, &opts[k], NULL, NULL) == PC_OK);
        CHECK(copy != NULL && memcmp(copy, owned, n * 3 * sizeof(float)) == 0);
        /* Only the hole changes. */
        for (i = 0; i < n * 3; i++) {
            only_hole = only_hole && (hole[i / 3] || memcmp(&owned[i], &rgb[i], 4) == 0);
        }
        CHECK(only_hole);
        free(copy);
        copy = NULL;
    }
    /* Nothing to copy from: the membrane, either way. */
    CHECK(pc_algo_ca_fill(w, h, 3, rgb, n * 3, hole, nowhere, n, &opts[0], NULL, &copy, NULL) == PC_OK);
    memcpy(owned, rgb, n * 3 * sizeof(float));
    CHECK(pc_algo_ca_fill_inplace(w, h, 3, owned, n * 3, hole, nowhere, n, &opts[0], NULL, NULL) == PC_OK);
    CHECK(copy != NULL && memcmp(copy, owned, n * 3 * sizeof(float)) == 0);
    free(rgb), free(copy), free(owned), free(hole), free(source), free(nowhere);
}

static void test_auto_sampling_can_be_cancelled(void)
{
    size_t w = 200, h = 160, n = w * h, i;
    float *im = noise(w, h, 5);
    bool *hole = malloc(n), *a = NULL, *b = NULL;
    pc_interrupt stop = {yes, NULL, NULL}, none = PC_INTERRUPT_NONE;
    for (i = 0; i < n * 3; i++) {
        im[i] = im[i] * 0.1f + ((i / 3) % w < 120 ? 0.3f : 0.6f);
    }
    for (i = 0; i < n; i++) {
        hole[i] = i % w >= 80 && i % w < 110 && i / w >= 60 && i / w < 90;
    }
    CHECK(pc_algo_ca_auto_sampling(w, h, 3, im, n * 3, hole, n, &stop, &a, NULL) == PC_ERR_CANCELLED);
    CHECK(a == NULL);
    CHECK(pc_algo_ca_auto_sampling(w, h, 3, im, n * 3, hole, n, &none, &a, NULL) == PC_OK);
    CHECK(pc_algo_ca_auto_sampling(w, h, 3, im, n * 3, hole, n, NULL, &b, NULL) == PC_OK);
    CHECK(a != NULL && b != NULL && memcmp(a, b, n) == 0);
    free(im), free(hole), free(a), free(b);
}

static void test_fills_only_the_hole_with_known_texture(void)
{
    size_t w = 48, h = 32, i;
    float *im, *out = NULL;
    bool *hole, *src = all_of(w * h, true), lo = false, hi = false, same = true;
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    stripes(w, h, &im, &hole);
    o.color_adaptation = PC_ALGO_ADAPT_NONE;
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, NULL, &out, NULL) == PC_OK);
    for (i = 0; out != NULL && i < w * h; i++) {
        if (!hole[i]) {
            same = same && out[i] == im[i];
        } else {
            lo = lo || out[i] < 0.35f;
            hi = hi || out[i] > 0.65f;
        }
    }
    CHECK(same);
    /* The hole is filled with stripe values, not a flat average. */
    CHECK(lo && hi);
    free(im), free(hole), free(src), free(out);
}

static void test_sampling_area_restricts_sources(void)
{
    /* Left half dark, right half bright; sampling only the right half fills bright. */
    size_t w = 40, h = 20, i;
    float *im = malloc(w * h * sizeof(float)), *out = NULL, mean = -0.0f;
    bool *hole = malloc(w * h), *right = malloc(w * h);
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    for (i = 0; i < w * h; i++) {
        im[i] = i % w < 20 ? 0.1f : 0.9f;
        hole[i] = i % w >= 16 && i % w < 24 && i / w >= 6 && i / w < 14;
        right[i] = i % w >= 24;
    }
    o.color_adaptation = PC_ALGO_ADAPT_NONE;
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, right, w * h, &o, NULL, &out, NULL) == PC_OK);
    for (i = 0; out != NULL && i < w * h; i++) {
        if (hole[i]) {
            mean += out[i];
        }
    }
    CHECK(mean / 64.0f > 0.8f);
    free(im), free(hole), free(right), free(out);
}

static void test_adaptations_run_and_stay_deterministic(void)
{
    size_t w = 32, h = 24, i;
    float *im, *rgb = malloc(w * h * 3 * sizeof(float)), *a = NULL, *b = NULL, low[3];
    bool *hole, *src = all_of(w * h, true), ok = true;
    pc_algo_ca_fill_options o;
    size_t nl = pc_algo_ca_rotation_level("low", low);
    stripes(w, h, &im, &hole);
    for (i = 0; i < w * h; i++) {
        rgb[i * 3] = im[i], rgb[i * 3 + 1] = im[i] * 0.5f, rgb[i * 3 + 2] = 1.0f - im[i];
    }
    o = (pc_algo_ca_fill_options){pc_algo_ca_color_level("high"), low, nl, true, true, 7};
    CHECK(pc_algo_ca_fill(w, h, 3, rgb, w * h * 3, hole, src, w * h, &o, NULL, &a, NULL) == PC_OK);
    CHECK(pc_algo_ca_fill(w, h, 3, rgb, w * h * 3, hole, src, w * h, &o, NULL, &b, NULL) == PC_OK);
    CHECK(a != NULL && b != NULL && memcmp(a, b, w * h * 3 * sizeof(float)) == 0);
    for (i = 0; a != NULL && i < w * h * 3; i++) {
        ok = ok && isfinite(a[i]) && a[i] >= 0.0f && a[i] <= 1.0f;
    }
    CHECK(ok);
    free(im), free(rgb), free(hole), free(src), free(a), free(b);
}

static void test_color_adaptation_follows_a_gradient(void)
{
    /* A horizontal brightness ramp with a hole: adaptation keeps the fill near the ramp. */
    size_t w = 40, h = 20, i;
    float *im = malloc(w * h * sizeof(float)), *out = NULL, err = -0.0f;
    bool *hole = malloc(w * h), *src = all_of(w * h, true);
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    for (i = 0; i < w * h; i++) {
        im[i] = (float)(i % w) / (float)w;
        hole[i] = i % w >= 14 && i % w < 26 && i / w >= 5 && i / w < 15;
    }
    o.color_adaptation = pc_algo_ca_color_level("veryHigh");
    CHECK(pc_algo_ca_fill(w, h, 1, im, w * h, hole, src, w * h, &o, NULL, &out, NULL) == PC_OK);
    for (i = 0; out != NULL && i < w * h; i++) {
        if (hole[i]) {
            err += fabsf(out[i] - im[i]);
        }
    }
    CHECK(err / 120.0f < 0.12f);
    free(im), free(hole), free(src), free(out);
}

static pc_rect sq(int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    return (pc_rect){x0, y0, x0 + w, y0 + h};
}

#define CHECK_WINDOW(hole, canvas, want) CHECK(pc_rect_eq(pc_algo_ca_sampling_window(hole, canvas), want))

static void test_sampling_window_is_photoshops(void)
{
    pc_rect canvas = {0, 0, 800, 800};
    /* Photoshop 25.4's sampling overlay: a square of side 4 sqrt(max(w,50) max(h,50)) centred on
     * the selection's bounds. */
    CHECK_WINDOW(sq(375, 375, 50, 50), canvas, sq(300, 300, 200, 200));
    CHECK_WINDOW(sq(350, 350, 100, 100), canvas, sq(200, 200, 400, 400));
    CHECK_WINDOW(sq(395, 395, 10, 10), canvas, sq(300, 300, 200, 200));
    CHECK_WINDOW(sq(370, 370, 60, 60), canvas, sq(280, 280, 240, 240));
    /* Long selections (Photoshop's overlay, read off the screen to a pixel: 281, 399, 691). */
    CHECK_WINDOW(sq(350, 385, 100, 30), canvas, sq(258, 258, 283, 283));
    CHECK_WINDOW(sq(300, 375, 200, 50), canvas, sq(200, 200, 400, 400));
    CHECK_WINDOW(sq(250, 350, 300, 100), canvas, sq(53, 53, 693, 693));
    /* At the canvas edge the square slides inwards rather than being cut. */
    CHECK_WINDOW(sq(10, 375, 50, 50), canvas, sq(0, 300, 200, 200));
    CHECK_WINDOW(sq(5, 5, 60, 60), canvas, sq(0, 0, 240, 240));
    /* A canvas smaller than the square clips it; a very long selection keeps its length. */
    CHECK_WINDOW(sq(40, 40, 50, 50), ((pc_rect){0, 0, 150, 120}), ((pc_rect){0, 0, 150, 120}));
    CHECK_WINDOW(sq(100, 995, 1800, 10), ((pc_rect){0, 0, 2000, 2000}), sq(100, 400, 1800, 1200));
    CHECK_WINDOW(PC_RECT_EMPTY, canvas, PC_RECT_EMPTY);
    /* Never crash: extreme rectangles. */
    (void)pc_algo_ca_sampling_window((pc_rect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX},
                                     (pc_rect){INT32_MIN, 0, INT32_MAX, 10});
    (void)pc_algo_ca_sampling_window(sq(0, 0, 3, 3), (pc_rect){INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX});
}

static float shade(size_t x, bool band)
{
    if (band) {
        return x >= 130 && x < 150 ? 0.55f : 0.35f;
    }
    return x < 135 ? 0.35f : 0.55f;
}

static float frac(size_t w, size_t n, const bool *hole, const bool *area, size_t x0, size_t x1)
{
    size_t i, in = 0, got = 0;
    for (i = 0; i < n; i++) {
        if (i % w >= x0 && i % w < x1) {
            in += !hole[i];
            got += area[i];
        }
    }
    return (float)got / (float)in;
}

static void test_auto_sampling_keeps_what_looks_like_the_surroundings(void)
{
    /* Grey texture, dark on the left, 50 levels brighter on the right; the hole is in the dark part
     * near the boundary (Photoshop's Auto leaves the bright part out). */
    size_t w = 200, h = 200, n = w * h, i;
    float *tex = noise(w, h, 3), *im = malloc(n * 3 * sizeof(float));
    bool *hole = malloc(n), *area = NULL;
    int b;
    for (i = 0; i < n; i++) {
        hole[i] = i % w >= 75 && i % w < 125 && i / w >= 75 && i / w < 125;
    }
    for (b = 0; b < 2; b++) {
        bool band = b == 1, never_hole = true;
        for (i = 0; i < n * 3; i++) {
            im[i] = shade((i / 3) % w, band) + (tex[i] - 0.5f) * 0.1f;
        }
        CHECK(pc_algo_ca_auto_sampling(w, h, 3, im, n * 3, hole, n, NULL, &area, NULL) == PC_OK);
        if (area == NULL) {
            continue;
        }
        for (i = 0; i < n; i++) {
            never_hole = never_hole && !(hole[i] && area[i]);
        }
        CHECK(never_hole);
        /* The dark side is sampled. */
        CHECK(frac(w, n, hole, area, 0, 125) > 0.95f);
        if (band) {
            /* A bright band through the window is left out; the dark area beyond it is not. */
            CHECK(frac(w, n, hole, area, 133, 147) < 0.05f);
            CHECK(frac(w, n, hole, area, 155, 200) > 0.9f);
        } else {
            CHECK(frac(w, n, hole, area, 140, 200) < 0.05f);
        }
        free(area);
        area = NULL;
    }
    free(tex), free(im), free(hole);
}

typedef struct key3 {
    int64_t v[3];
} key3;

static int key_cmp(const void *a, const void *b)
{
    const key3 *x = a, *y = b;
    int c;
    for (c = 0; c < 3; c++) {
        if (x->v[c] != y->v[c]) {
            return x->v[c] < y->v[c] ? -1 : 1;
        }
    }
    return 0;
}

/* Votes of identical copies average to the copy up to float rounding. */
static key3 key_of(const float *v, size_t i)
{
    key3 k;
    int c;
    for (c = 0; c < 3; c++) {
        k.v[c] = (int64_t)roundf(v[i * 3 + (size_t)c] * 1e5f);
    }
    return k;
}

static float lap(const float *v, size_t w, size_t x, size_t y)
{
#define P(xx, yy) v[((yy) * w + (xx)) * 3]
    return fabsf(4.0f * P(x, y) - P(x - 1, y) - P(x + 1, y) - P(x, y - 1) - P(x, y + 1));
#undef P
}

static float mean_lap(const float *out, const bool *hole, size_t w, size_t h, bool inside)
{
    float s = -0.0f;
    size_t x, y, cnt = 0;
    for (y = 1; y < h - 1; y++) {
        for (x = 1; x < w - 1; x++) {
            if (hole[y * w + x] == inside) {
                s += lap(out, w, x, y);
                cnt++;
            }
        }
    }
    return s / (float)cnt;
}

static void test_without_colour_adaptation_the_fill_copies_pixels_exactly(void)
{
    /* A smooth texture with fine noise, so that every value is unique: a filled pixel equal to an
     * image pixel was copied from it. Photoshop's fill (colour adaptation None) is such copies,
     * joined by narrow seams. */
    size_t w = 120, h = 120, n = w * h, i, nk = 0, exact = 0;
    float *fine = noise(w, h, 9), *im = malloc(n * 3 * sizeof(float)), *out = NULL;
    bool *hole = malloc(n), *all = all_of(n, true);
    key3 *known = malloc(n * sizeof *known);
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    for (i = 0; i < n * 3; i++) {
        float x = (float)((i / 3) % w), y = (float)((i / 3) / w), c = (float)(i % 3);
        im[i] = 0.5f + 0.2f * sinf(x * 0.21f + c) * cosf(y * 0.17f) + 0.1f * sinf(x * 0.05f - y * 0.08f) +
                (fine[i] - 0.5f) * 0.08f;
    }
    for (i = 0; i < n; i++) {
        hole[i] = i % w >= 40 && i % w < 80 && i / w >= 40 && i / w < 80;
        if (!hole[i]) {
            known[nk++] = key_of(im, i);
        }
    }
    qsort(known, nk, sizeof *known, key_cmp);
    o.color_adaptation = PC_ALGO_ADAPT_NONE;
    CHECK(pc_algo_ca_fill(w, h, 3, im, n * 3, hole, all, n, &o, NULL, &out, NULL) == PC_OK);
    if (out != NULL) {
        for (i = 0; i < n; i++) {
            key3 k = key_of(out, i);
            if (hole[i] && bsearch(&k, known, nk, sizeof *known, key_cmp) != NULL) {
                exact++;
            }
        }
        /* At least 20% of the 1600 filled pixels are exact copies. */
        CHECK((float)exact > 0.2f * 1600.0f);
        /* And the fill is as busy as the noise around it, not a blur of it. */
        CHECK(mean_lap(out, hole, w, h, true) > 0.6f * mean_lap(out, hole, w, h, false));
    }
    free(fine), free(im), free(out), free(hole), free(all), free(known);
}

static float patch_shift(const float *from, const float *to, float level, uint8_t c)
{
    float gb[1][2], s = -0.0f;
    size_t i;
    CHECK(pc_algo_ca_level_transform(1, from, 300, to, 300, pc_algo_ca_color_amount(c), gb, NULL) == PC_OK);
    for (i = 0; i < 300; i++) {
        s += gb[0][0] * from[i] + gb[0][1];
    }
    return level - s / 300.0f;
}

static void test_patch_color_shifts_up_to_about_four_hundredths_per_step(void)
{
    /* Content 0.4 brighter than where it lands (Photoshop's Patch, Color 3 and 10). */
    float from[300], to[300], level = -0.0f;
    size_t i;
    for (i = 0; i < 300; i++) {
        from[i] = 0.7f + 0.05f * ((float)(i % 7) / 7.0f);
        to[i] = from[i] - 0.4f;
        level += from[i];
    }
    level /= 300.0f;
    CHECK(patch_shift(from, to, level, 0) == 0.0f);
    CHECK(fabsf(patch_shift(from, to, level, 3) - 0.12f) < 0.01f);
    CHECK(fabsf(patch_shift(from, to, level, 10) - 0.4f) < 0.01f);
}

static void test_variant_geometry(void)
{
    float im[60], *vi = NULL;
    bool ok[60], *vo = NULL;
    size_t ow = 0, oh = 0, i, cnt = 0;
    for (i = 0; i < 60; i++) {
        im[i] = 0.5f, ok[i] = true;
    }
    CHECK(pc_algo_ca_variant(10, 6, 1, im, ok, 90.0f, 1.0f, false, NULL, &ow, &oh, &vi, &vo, NULL) == PC_OK);
    CHECK_EQ_UINT(ow, 6);
    CHECK_EQ_UINT(oh, 10);
    free(vi), free(vo);
    vi = NULL, vo = NULL;
    CHECK(pc_algo_ca_variant(10, 6, 1, im, ok, 0.0f, 0.8f, true, NULL, &ow, &oh, &vi, &vo, NULL) == PC_OK);
    CHECK_EQ_UINT(ow, 8);
    CHECK_EQ_UINT(oh, 5);
    for (i = 0; vo != NULL && i < ow * oh; i++) {
        cnt += vo[i] ? 1u : 0u;
    }
    CHECK(cnt > 20);
    free(vi), free(vo);
}

static void test_levels(void)
{
    float r[3];
    pc_algo_adapt a;
    CHECK_EQ_UINT(pc_algo_ca_rotation_level("full", r), 3);
    CHECK(r[0] == 90.0f && r[2] == 270.0f);
    CHECK_EQ_UINT(pc_algo_ca_rotation_level("medium", r), 2);
    CHECK(r[0] == -20.0f);
    CHECK_EQ_UINT(pc_algo_ca_rotation_level("bogus", r), 0);
    CHECK_EQ_UINT(pc_algo_ca_rotation_level(NULL, r), 0);
    CHECK(pc_algo_adapt_is_none(pc_algo_ca_color_level("none")));
    a = pc_algo_ca_color_level("veryHigh");
    CHECK(a.gain_lo == 0.67f && a.gain_hi == 1.5f && a.bias == 0.14f);
    a = pc_algo_ca_color_level(NULL);
    CHECK(a.gain_lo == 0.9f && a.gain_hi == 1.1f && a.bias == 0.04f);
    CHECK(pc_algo_adapt_is_none(pc_algo_ca_color_amount(0)));
    a = pc_algo_ca_color_amount(255);
    CHECK(a.bias == pc_algo_ca_color_amount(10).bias && a.gain_lo == pc_algo_ca_color_amount(10).gain_lo);
}

static void test_never_crash(void)
{
    float im[100], *out = NULL, rots[80], *vi = NULL, gb[2][2], s4[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    bool hole[100], src[100], none[100], *vo = NULL, *area = NULL;
    size_t i, ow, oh;
    pc_algo_ca_fill_options o = pc_algo_ca_fill_options_default();
    for (i = 0; i < 100; i++) {
        im[i] = i % 3 == 0 ? NAN : (float)i / 100.0f;
        hole[i] = i % 10 > 3 && i % 10 < 7 && i / 10 > 3 && i / 10 < 7;
        src[i] = i % 2 == 0, none[i] = false;
    }
    for (i = 0; i < 80; i++) {
        rots[i] = (float)i * 45.0f;
    }
    /* NaN pixels, all sources excluded, too many rotations. */
    o.rotations = rots, o.rotation_count = 8, o.scale = true, o.mirror = true;
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, none, 100, &o, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    o.rotation_count = 80;
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_ERR_LIMIT);
    o.rotation_count = 2, o.rotations = NULL;
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_ERR_INVALID);
    /* Non-finite rotations and unusable colour adaptation limits (the reference panics in clamp). */
    rots[0] = NAN;
    o.rotations = rots, o.rotation_count = 1;
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_ERR_INVALID);
    o = pc_algo_ca_fill_options_default();
    o.color_adaptation = (pc_algo_adapt){1.2f, 0.8f, 0.0f};
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_ERR_INVALID);
    o.color_adaptation = (pc_algo_adapt){NAN, 1.0f, 0.0f};
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_ERR_INVALID);
    o.color_adaptation = (pc_algo_adapt){1.0f, 1.0f, -0.1f};
    CHECK(pc_algo_ca_fill_inplace(10, 10, 1, im, 100, hole, src, 100, &o, NULL, NULL) == PC_ERR_INVALID);
    o.color_adaptation = (pc_algo_adapt){0.0f, 1e30f, 1e30f};
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    /* Bad lengths, NULL options, empty and absurd sizes. */
    o = pc_algo_ca_fill_options_default();
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 99, &o, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_ca_fill(10, 10, 1, im, 100, hole, src, 100, NULL, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_ca_fill(SIZE_MAX, 10, 1, im, 100, hole, src, 100, &o, NULL, &out, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_ca_fill(0, 0, 1, NULL, 0, NULL, NULL, 0, &o, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    /* Variants with non-finite angles and scales, and an empty source. */
    CHECK(pc_algo_ca_variant(10, 10, 1, im, src, NAN, 1.0f, false, NULL, &ow, &oh, &vi, &vo, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_ca_variant(10, 10, 1, im, src, 30.0f, INFINITY, false, NULL, &ow, &oh, &vi, &vo, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_ca_variant(10, 10, 1, im, src, 30.0f, 0.0f, true, NULL, &ow, &oh, &vi, &vo, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_ca_variant(0, 10, 1, im, src, 0.0f, 1.0f, false, NULL, &ow, &oh, &vi, &vo, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_ca_variant(10, 10, 1, im, src, 1e9f, 1e-30f, false, NULL, &ow, &oh, &vi, &vo, NULL) ==
          PC_OK);
    CHECK(ow == 1 && oh == 1);
    free(vi), free(vo);
    /* Auto sampling: NaN pixels, no hole, all hole, bad lengths, empty. */
    CHECK(pc_algo_ca_auto_sampling(10, 10, 1, im, 100, hole, 100, NULL, &area, NULL) == PC_OK);
    free(area);
    area = NULL;
    CHECK(pc_algo_ca_auto_sampling(10, 10, 1, im, 100, none, 100, NULL, &area, NULL) == PC_OK);
    free(area);
    area = NULL;
    for (i = 0; i < 100; i++) {
        src[i] = true;
    }
    CHECK(pc_algo_ca_auto_sampling(10, 10, 1, im, 100, src, 100, NULL, &area, NULL) == PC_OK);
    free(area);
    area = NULL;
    CHECK(pc_algo_ca_auto_sampling(10, 10, 1, im, 100, hole, 99, NULL, &area, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_ca_auto_sampling(10, 10, 0, im, 0, hole, 100, NULL, &area, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_ca_auto_sampling(0, 0, 3, NULL, 0, NULL, 0, NULL, &area, NULL) == PC_OK);
    free(area);
    /* Level transform: empty sets, uneven lengths, invalid limits, missing buffers. */
    CHECK(pc_algo_ca_level_transform(2, s4, 0, s4, 4, pc_algo_ca_color_amount(5), gb, NULL) == PC_OK);
    CHECK(gb[0][0] == 1.0f && gb[0][1] == 0.0f);
    CHECK(pc_algo_ca_level_transform(2, s4, 3, s4, 4, pc_algo_ca_color_amount(5), gb, NULL) == PC_OK);
    CHECK(pc_algo_ca_level_transform(2, s4, 4, s4, 4, (pc_algo_adapt){2.0f, 1.0f, 0.0f}, gb, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_ca_level_transform(2, NULL, 4, s4, 4, pc_algo_ca_color_amount(5), gb, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_ca_level_transform(0, s4, 4, s4, 4, pc_algo_ca_color_amount(5), NULL, NULL) == PC_OK);
}

int main(int argc, char **argv)
{
    RUN(test_cancelled_fill_stops_and_progress_reaches_one);
    RUN(test_filling_in_place_is_filling_a_copy);
    RUN(test_auto_sampling_can_be_cancelled);
    RUN(test_fills_only_the_hole_with_known_texture);
    RUN(test_sampling_area_restricts_sources);
    RUN(test_adaptations_run_and_stay_deterministic);
    RUN(test_color_adaptation_follows_a_gradient);
    RUN(test_sampling_window_is_photoshops);
    RUN(test_auto_sampling_keeps_what_looks_like_the_surroundings);
    RUN(test_without_colour_adaptation_the_fill_copies_pixels_exactly);
    RUN(test_patch_color_shifts_up_to_about_four_hundredths_per_step);
    RUN(test_variant_geometry);
    RUN(test_levels);
    RUN(test_never_crash);
    return pc_t_finish();
}
