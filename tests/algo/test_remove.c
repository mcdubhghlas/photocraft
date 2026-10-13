/* The Remove Tool (src/algo/remove.c): the crates/algo/src/remove.rs tests plus never-crash cases. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "algo/remove.h"
#include "harness.h"

typedef bool (*cell_fn)(size_t x, size_t y);

static bool *grid(size_t w, size_t h, cell_fn f)
{
    bool *m = malloc(w * h);
    size_t i;
    for (i = 0; i < w * h; i++) {
        m[i] = f(i % w, i / w);
    }
    return m;
}

static size_t count(const bool *m, size_t n)
{
    size_t i, c = 0;
    for (i = 0; i < n; i++) {
        c += m[i];
    }
    return c;
}

static bool ring_cell(size_t x, size_t y)
{
    float d = hypotf((float)x - 20.0f, (float)y - 20.0f);
    return d >= 10.0f && d < 12.0f;
}
static bool open_line(size_t x, size_t y)
{
    return y == 20 && x >= 5 && x < 35;
}

static void test_a_ring_closes_into_a_disc_and_an_open_line_does_not(void)
{
    size_t w = 40, h = 40;
    bool *ring = grid(w, h, ring_cell), *line = grid(w, h, open_line), *closed = NULL, *l2 = NULL;
    CHECK(pc_algo_remove_close_loops(w, h, ring, w * h, &closed, NULL) == PC_OK);
    CHECK(closed != NULL && closed[20 * w + 20]); /* the centre is enclosed */
    CHECK(closed != NULL && !closed[2 * w + 2]);  /* the outside stays outside */
    /* An open stroke encloses nothing. */
    CHECK(pc_algo_remove_close_loops(w, h, line, w * h, &l2, NULL) == PC_OK);
    CHECK(l2 != NULL && memcmp(l2, line, w * h) == 0);
    free(ring), free(line), free(closed), free(l2);
}

static bool square(size_t x, size_t y)
{
    return (x == 5 || x == 14 || y == 5 || y == 14) && x >= 5 && x <= 14 && y >= 5 && y <= 14;
}

static void test_a_loop_open_at_one_pixel_encloses_nothing(void)
{
    size_t w = 20, h = 20;
    bool *sq = grid(w, h, square), *out = NULL;
    sq[5 * w + 9] = false;
    CHECK(pc_algo_remove_close_loops(w, h, sq, w * h, &out, NULL) == PC_OK);
    CHECK(out != NULL && !out[10 * w + 10]);
    free(sq), free(out);
}

static bool dot(size_t x, size_t y)
{
    return x == 5 && y == 5;
}

static void test_dilate_grows_by_the_radius(void)
{
    size_t w = 11, h = 11;
    bool *d0 = grid(w, h, dot), *d = NULL, *z = NULL;
    CHECK(pc_algo_remove_dilate(w, h, d0, w * h, 2, &d, NULL) == PC_OK);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK_EQ_UINT(count(d, w * h), 25);
        CHECK(d[3 * w + 3] && d[7 * w + 7] && !d[2 * w + 5]);
    }
    CHECK(pc_algo_remove_dilate(w, h, d0, w * h, 0, &z, NULL) == PC_OK);
    CHECK(z != NULL && memcmp(z, d0, w * h) == 0);
    free(d0), free(d), free(z);
}

static bool line10(size_t x, size_t y)
{
    return y == 10 && x >= 5 && x < 25;
}
static bool block(size_t x, size_t y)
{
    return x >= 5 && x < 16 && y >= 5 && y < 16;
}
static bool everything(size_t x, size_t y)
{
    (void)x, (void)y;
    return true;
}

static void test_radius_measures_the_thickest_part(void)
{
    size_t w = 30, h = 30;
    bool *line = grid(w, h, line10), *blk = grid(w, h, block), *all = grid(w, h, everything), one = true;
    uint32_t r = 7;
    CHECK(pc_algo_remove_hole_radius(w, h, line, w * h, &r, NULL) == PC_OK);
    CHECK_EQ_UINT(r, 1);
    CHECK(pc_algo_remove_hole_radius(w, h, blk, w * h, &r, NULL) == PC_OK);
    CHECK_EQ_UINT(r, 6);
    CHECK(pc_algo_remove_hole_radius(w, h, all, w * h, &r, NULL) == PC_OK);
    CHECK_EQ_UINT(r, UINT32_MAX);
    /* A mask of the wrong size. */
    CHECK(pc_algo_remove_hole_radius(w, h, &one, 1, &r, NULL) == PC_OK);
    CHECK_EQ_UINT(r, 0);
    free(line), free(blk), free(all);
}

static bool thin_hole(size_t x, size_t y)
{
    return y == 24 && x >= 10 && x < 54;
}
static bool wide_hole(size_t x, size_t y)
{
    return x >= 24 && x < 40 && y >= 16 && y < 32;
}

/* A hole in a horizontal gradient is filled close to the gradient, both thin and wide. */
static void test_removes_thin_and_wide_holes_from_a_gradient(void)
{
    size_t w = 64, h = 48, ch = 2, n = w * h, i, k;
    float *truth = malloc(n * ch * sizeof(float)), *img = malloc(n * ch * sizeof(float));
    cell_fn holes[2] = {thin_hole, wide_hole};
    for (i = 0; i < n; i++) {
        truth[i * 2] = (float)(i % w) / (float)w, truth[i * 2 + 1] = 1.0f;
    }
    for (k = 0; k < 2; k++) {
        bool *hole = grid(w, h, holes[k]), near = true, untouched = true;
        float *out = NULL;
        memcpy(img, truth, n * ch * sizeof(float));
        for (i = 0; i < n; i++) {
            if (hole[i]) {
                img[i * ch] = 1.0f;
            }
        }
        CHECK(pc_algo_remove(w, h, ch, img, n * ch, hole, n, 7, NULL, &out, NULL) == PC_OK);
        for (i = 0; out != NULL && i < n; i++) {
            if (hole[i]) {
                near = near && fabsf(out[i * ch] - truth[i * ch]) < 0.12f;
            } else {
                untouched = untouched && memcmp(&out[i * ch], &img[i * ch], ch * sizeof(float)) == 0;
            }
        }
        CHECK(out != NULL && near && untouched);
        free(hole), free(out);
    }
    free(truth), free(img);
}

static bool diagonal(size_t x, size_t y)
{
    return fabsf((float)x - (float)y * 1.2f - 20.0f) < 3.0f && y >= 40 && y < 680;
}

/* A long, thin diagonal hole goes tile by tile; every cell is filled, the rest is untouched. */
static void test_a_long_thin_hole_is_filled_in_tiles(void)
{
    size_t w = 900, h = 700, n = w * h, i;
    float *holed = malloc(n * sizeof(float)), *out = NULL;
    bool *hole = grid(w, h, diagonal), filled = true, untouched = true;
    for (i = 0; i < n; i++) {
        holed[i] = hole[i] ? 1.0f : 0.3f + 0.4f * (float)(((i % w) / 7 + (i / w) / 5) % 2);
    }
    CHECK(pc_algo_remove(w, h, 1, holed, n, hole, n, 3, NULL, &out, NULL) == PC_OK);
    for (i = 0; out != NULL && i < n; i++) {
        if (hole[i]) {
            filled = filled && out[i] >= 0.29f && out[i] <= 0.71f;
        } else {
            untouched = untouched && out[i] == holed[i];
        }
    }
    CHECK(out != NULL && filled && untouched);
    free(holed), free(hole), free(out);
}

static void test_hostile_sizes_return_the_input(void)
{
    float img[12], *out = NULL;
    bool t1 = true, t4[4] = {true, true, true, true}, f6[6] = {false}, *m = NULL;
    size_t i;
    for (i = 0; i < 12; i++) {
        img[i] = 0.5f;
    }
    CHECK(pc_algo_remove(2, 2, 3, img, 12, &t1, 1, 1, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL && memcmp(out, img, sizeof img) == 0);
    free(out);
    out = NULL;
    CHECK(pc_algo_remove(2, 2, 0, img, 12, t4, 4, 1, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL && memcmp(out, img, sizeof img) == 0);
    free(out);
    out = NULL;
    CHECK(pc_algo_remove(3, 2, 2, img, 12, f6, 6, 1, NULL, &out, NULL) == PC_OK);
    CHECK(out != NULL && memcmp(out, img, sizeof img) == 0);
    free(out);
    out = NULL;
    CHECK(pc_algo_remove_close_loops(0, 0, NULL, 0, &m, NULL) == PC_OK);
    free(m);
    m = NULL;
    CHECK(pc_algo_remove_dilate(3, 3, &t1, 1, 1, &m, NULL) == PC_OK);
    CHECK(m != NULL && m[0]);
    free(m);
}

static bool yes(void *ctx)
{
    (void)ctx;
    return true;
}

static void test_never_crash(void)
{
    size_t w = 40, h = 30, n = w * h, i;
    float *img = malloc(n * 3 * sizeof(float)), *out = NULL;
    bool *hole = grid(w, h, block), *all = grid(w, h, everything), *m = NULL;
    pc_interrupt stop = {yes, NULL, NULL};
    uint32_t r;
    for (i = 0; i < n * 3; i++) {
        img[i] = i % 7 == 0 ? NAN : (i % 5 == 0 ? -INFINITY : (float)(i % 17) / 16.0f);
    }
    /* NaN / infinite pixels, everything a hole (diffused), cancelled, NULL buffers, absurd sizes. */
    CHECK(pc_algo_remove(w, h, 3, img, n * 3, hole, n, 1, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    CHECK(pc_algo_remove(w, h, 3, img, n * 3, all, n, UINT64_MAX, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    CHECK(pc_algo_remove(w, h, 3, img, n * 3, hole, n, 1, &stop, &out, NULL) == PC_ERR_CANCELLED);
    CHECK(out == NULL);
    CHECK(pc_algo_remove(w, h, 3, NULL, n * 3, hole, n, 1, NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_remove(SIZE_MAX, SIZE_MAX, 3, img, n * 3, hole, n, 1, NULL, &out, NULL) == PC_OK);
    free(out);
    out = NULL;
    CHECK(pc_algo_remove(0, 0, 3, NULL, 0, NULL, 0, 1, NULL, &out, NULL) == PC_OK);
    free(out);
    /* Masks: NULL, huge radii, wrong sizes, overflowing dimensions. */
    CHECK(pc_algo_remove_close_loops(w, h, NULL, n, &m, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_remove_dilate(w, h, hole, n, SIZE_MAX, &m, NULL) == PC_OK);
    CHECK(m != NULL && count(m, n) == n);
    free(m);
    m = NULL;
    CHECK(pc_algo_remove_close_loops(SIZE_MAX, 2, hole, n, &m, NULL) == PC_OK);
    CHECK(m != NULL && memcmp(m, hole, n) == 0);
    free(m);
    CHECK(pc_algo_remove_hole_radius(SIZE_MAX, 2, hole, n, &r, NULL) == PC_OK);
    CHECK_EQ_UINT(r, 0);
    free(img), free(hole), free(all);
}

int main(int argc, char **argv)
{
    RUN(test_a_ring_closes_into_a_disc_and_an_open_line_does_not);
    RUN(test_a_loop_open_at_one_pixel_encloses_nothing);
    RUN(test_dilate_grows_by_the_radius);
    RUN(test_radius_measures_the_thickest_part);
    RUN(test_removes_thin_and_wide_holes_from_a_gradient);
    RUN(test_a_long_thin_hole_is_filled_in_tiles);
    RUN(test_hostile_sizes_return_the_input);
    RUN(test_never_crash);
    return pc_t_finish();
}
