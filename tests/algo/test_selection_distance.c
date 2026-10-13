/* The exact distance transform behind Expand / Contract (port of
 * crates/algo/src/selection/distance/tests.rs) and the Elliptical Marquee / surface combining
 * (the test in crates/algo/src/selection/marquee.rs).
 *
 * Not ported: distance_transform_release_comparison (an #[ignore]d release timing on 24-36 MP
 * masks; its exactness checks are what the oracle comparisons here do on small masks). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "base/rmath.h"
#include "harness.h"

/* ---- distance transform (selection/distance/tests.rs) ---- */

#define EMPTY_DISTANCE 1e10f

static float *brute(const bool *inside, size_t w, size_t h)
{
    float *out = malloc(w * h * sizeof(float));
    size_t i, j, nsites = 0, *sites = malloc(w * h * sizeof(size_t));
    for (i = 0; i < w * h; i++) {
        if (inside[i]) {
            sites[nsites++] = i;
        }
    }
    for (i = 0; i < w * h; i++) {
        float best = EMPTY_DISTANCE;
        for (j = 0; j < nsites; j++) {
            double dx = (double)(i % w) - (double)(sites[j] % w),
                   dy = (double)(i / w) - (double)(sites[j] / w);
            best = pc_rs_minf(best, (float)sqrt(dx * dx + dy * dy));
        }
        out[i] = best;
    }
    free(sites);
    return out;
}

/* 1D squared distance transform (Felzenszwalb & Huttenlocher), the previous version. */
static void old_dt1(const float *f, size_t n, float *out, size_t *v, float *z)
{
    size_t k = 0, q;
    v[0] = 0;
    z[0] = -INFINITY;
    z[1] = INFINITY;
    for (q = 1; q < n; q++) {
        for (;;) {
            size_t p = v[k];
            float s = ((f[q] + (float)(q * q)) - (f[p] + (float)(p * p))) / (2.0f * ((float)q - (float)p));
            if (s <= z[k] && k > 0) {
                k--;
                continue;
            }
            if (s <= z[k]) {
                v[0] = q;
                z[0] = -INFINITY;
                z[1] = INFINITY;
                break;
            }
            k++;
            v[k] = q;
            z[k] = s;
            z[k + 1] = INFINITY;
            break;
        }
    }
    k = 0;
    for (q = 0; q < n; q++) {
        float d;
        while (z[k + 1] < (float)q) {
            k++;
        }
        d = (float)q - (float)v[k];
        out[q] = d * d + f[v[k]];
    }
}

static float *old_edt(const bool *inside, size_t w, size_t h)
{
    size_t n = w > h ? w : h, x, y;
    float *g = malloc(w * h * sizeof(float) + 4), *f, *o, *z;
    size_t *v;
    n = n > 0 ? n : 1;
    f = calloc(n, sizeof(float));
    o = calloc(n, sizeof(float));
    z = malloc((n + 1) * sizeof(float));
    v = malloc(n * sizeof(size_t));
    for (x = 0; x < w * h; x++) {
        g[x] = inside[x] ? 0.0f : 1e20f;
    }
    for (x = 0; x < w; x++) {
        for (y = 0; y < h; y++) {
            f[y] = g[y * w + x];
        }
        old_dt1(f, h, o, v, z);
        for (y = 0; y < h; y++) {
            g[y * w + x] = o[y];
        }
    }
    for (y = 0; y < h; y++) {
        memcpy(f, g + y * w, w * sizeof(float));
        old_dt1(f, w, o, v, z);
        for (x = 0; x < w; x++) {
            g[y * w + x] = sqrtf(o[x]);
        }
    }
    free(f);
    free(o);
    free(z);
    free(v);
    return g;
}

/* edt equals want exactly (-0.0 and 0.0 alike, as Rust's ==). */
static bool edt_is(const bool *inside, size_t w, size_t h, const float *want)
{
    float *d = NULL;
    size_t i;
    bool ok = pc_algo_selection_edt(inside, w, h, &d, NULL) == PC_OK && d != NULL;
    for (i = 0; ok && i < w * h; i++) {
        ok = d[i] == want[i];
    }
    free(d);
    return ok;
}

static void test_edt_matches_brute_force_for_every_small_mask(void)
{
    static const size_t dims[5][2] = {{1, 1}, {1, 8}, {8, 1}, {3, 3}, {4, 3}};
    bool inside[12];
    size_t k, bits, i;
    int bad = 0;
    for (k = 0; k < 5; k++) {
        size_t w = dims[k][0], h = dims[k][1];
        for (bits = 0; bits < (size_t)1 << (w * h); bits++) {
            float *b;
            for (i = 0; i < w * h; i++) {
                inside[i] = (bits >> i) & 1u;
            }
            b = brute(inside, w, h);
            if (!edt_is(inside, w, h, b)) {
                bad++;
                PC_T_FAIL("%zux%zu: %zu", w, h, bits);
            }
            free(b);
        }
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_edt_rectangular_and_partial_bands_match_the_previous_transform(void)
{
    static const size_t dims[6][2] = {{1, 101}, {101, 1}, {17, 33}, {73, 65}, {9, 255}, {513, 137}};
    size_t k, i;
    int kind;
    for (k = 0; k < 6; k++) {
        size_t w = dims[k][0], h = dims[k][1], n = w * h;
        bool *inside = malloc(n);
        for (kind = 0; kind < 4; kind++) {
            float *o;
            for (i = 0; i < n; i++) {
                inside[i] = kind == 0   ? false
                            : kind == 1 ? true
                            : kind == 2 ? i == 0 || i == n - 1
                                        : (i * 101 + i / w * 37) % 29 == 0;
            }
            o = old_edt(inside, w, h);
            if (!edt_is(inside, w, h, o)) {
                PC_T_FAIL("%zux%zu kind %d", w, h, kind);
            }
            free(o);
        }
        free(inside);
    }
}

static void test_edt_large_coordinates_keep_nearby_parabolas_distinct(void)
{
    /* Adjacent sites late in a PSB-sized row lost their half-pixel boundaries when the previous
     * transform subtracted f32 coordinate squares. */
    static const size_t dims[2][2] = {{65537, 2}, {2, 65537}};
    size_t k;
    for (k = 0; k < 2; k++) {
        size_t w = dims[k][0], h = dims[k][1];
        bool *inside = calloc(w * h, 1);
        float *b;
        if (w > h) {
            inside[w - 3] = inside[w - 2] = inside[2 * w - 1] = true;
        } else {
            inside[(h - 3) * w] = inside[(h - 2) * w] = inside[h * w - 1] = true;
        }
        b = brute(inside, w, h);
        CHECK(edt_is(inside, w, h, b));
        free(b);
        free(inside);
    }
}

static void test_edt_selected_pixels_stay_at_zero_past_coordinate_4096(void)
{
    bool *inside = malloc(6000);
    float *zero = calloc(6000, sizeof(float)), *o;
    size_t i;
    bool any = false;
    memset(inside, 1, 6000);
    CHECK(edt_is(inside, 6000, 1, zero));
    o = old_edt(inside, 6000, 1);
    for (i = 0; i < 6000; i++) {
        any |= o[i] != 0.0f;
    }
    CHECK(any); /* the previous transform reproduces the regression */
    free(o);
    free(zero);
    free(inside);
}

/* The reference returns an empty vector for empty, inconsistent and impossible sizes; in C an
 * empty mask gives NULL and an impossible one PC_ERR_LIMIT (a C buffer has no length to be
 * inconsistent). */
static void test_edt_inconsistent_and_empty_dimensions_do_not_panic(void)
{
    static const size_t dims[3][2] = {{0, 0}, {1, 0}, {0, 1}};
    bool one = true;
    float *d = &(float){0};
    size_t k;
    for (k = 0; k < 3; k++) {
        d = &(float){0};
        CHECK(pc_algo_selection_edt(&one, dims[k][0], dims[k][1], &d, NULL) == PC_OK && d == NULL);
    }
    d = NULL;
    CHECK(pc_algo_selection_edt(&one, SIZE_MAX, 2, &d, NULL) == PC_ERR_LIMIT && d == NULL);
}

static float *reference_expand(const float *m, size_t w, size_t h, float r)
{
    bool *inside = calloc(w * h, 1);
    float *d, *out = malloc(w * h * sizeof(float));
    size_t i;
    for (i = 0; i < w * h; i++) {
        inside[i] = m[i] >= 0.5f;
    }
    d = old_edt(inside, w, h);
    for (i = 0; i < w * h; i++) {
        out[i] = pc_rs_maxf(m[i], pc_clampf(r + 1.0f - d[i], 0.0f, 1.0f));
    }
    free(inside);
    free(d);
    return out;
}

static void test_selection_modifiers_keep_fractional_coverage(void)
{
    /* Border is Photoshop's soft band now (test_border_matches_photoshop), so only these two. */
    static const float radii[5] = {0.0f, 1.0f, 1.25f, 3.75f, 16.0f};
    const size_t w = 47, h = 39, n = w * h;
    float *m = malloc(n * sizeof(float)), *inv = malloc(n * sizeof(float));
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        m[i] = i % w >= 8 && i % w < 31 && i / w >= 5 && i / w < 27 ? 0.8f : 0.2f;
        inv[i] = 1.0f - m[i];
    }
    for (k = 0; k < 5; k++) {
        float *e = NULL, *c = NULL, *re = reference_expand(m, w, h, radii[k]),
              *rc = reference_expand(inv, w, h, radii[k]);
        bool ok = true;
        CHECK(pc_algo_selection_expand(m, w, h, radii[k], &e, NULL) == PC_OK);
        CHECK(pc_algo_selection_contract(m, w, h, radii[k], &c, NULL) == PC_OK);
        for (i = 0; e != NULL && c != NULL && i < n; i++) {
            ok &= e[i] == re[i] && c[i] == 1.0f - rc[i];
        }
        if (!ok || e == NULL || c == NULL) {
            PC_T_FAIL("r %g", (double)radii[k]);
        }
        free(e);
        free(c);
        free(re);
        free(rc);
    }
    free(m);
    free(inv);
}

/* results_do_not_depend_on_the_worker_count: the C pool size is fixed per process, so this runs
 * at whatever PHOTOCRAFT_THREADS says (the gates run 1 and 8) against the single-threaded oracle. */
static void test_edt_results_do_not_depend_on_the_worker_count(void)
{
    const size_t w = 521, h = 139;
    bool *inside = malloc(w * h);
    float *o;
    size_t i;
    for (i = 0; i < w * h; i++) {
        inside[i] = (i * 101 + i / w * 37) % 97 == 0;
    }
    o = old_edt(inside, w, h);
    CHECK(edt_is(inside, w, h, o));
    free(o);
    free(inside);
}

/* ---- Elliptical Marquee and surface combining (selection/marquee.rs) ---- */

static bool ell_inside(float x, float y, float cx, float cy, float rx, float ry)
{
    float dx = (x - cx) / rx, dy = (y - cy) / ry;
    return dx * dx + dy * dy <= 1.0f;
}

/* The Elliptical Marquee as it was: every pixel tested and written one at a time. */
static void old_ellipse(pc_rect r, pc_rect area, bool aa, pc_surface *shape)
{
    pc_rect cut = pc_rect_intersect(r, area);
    float cx = (float)(r.x0 + r.x1) / 2.0f, cy = (float)(r.y0 + r.y1) / 2.0f;
    float rx = (float)pc_rect_width(r) / 2.0f, ry = (float)pc_rect_height(r) / 2.0f;
    int32_t x, y;
    pc_surface_init(shape, PC_PIXEL_FORMAT_GRAY8);
    for (y = cut.y0; y < cut.y1; y++) {
        for (x = cut.x0; x < cut.x1; x++) {
            float fx = (float)x, fy = (float)y, cov;
            int corners = ell_inside(fx, fy, cx, cy, rx, ry) + ell_inside(fx + 1.0f, fy, cx, cy, rx, ry) +
                          ell_inside(fx, fy + 1.0f, cx, cy, rx, ry) +
                          ell_inside(fx + 1.0f, fy + 1.0f, cx, cy, rx, ry);
            if (!aa) {
                cov = ell_inside(fx + 0.5f, fy + 0.5f, cx, cy, rx, ry) ? 1.0f : 0.0f;
            } else if (corners == 4) {
                cov = 1.0f;
            } else {
                int i, c = 0;
                for (i = 0; i < 16; i++) {
                    c += ell_inside(fx + ((float)(i % 4) + 0.5f) / 4.0f, fy + ((float)(i / 4) + 0.5f) / 4.0f,
                                    cx, cy, rx, ry);
                }
                cov = (float)c / 16.0f;
            }
            if (cov > 0.0f) {
                (void)pc_surface_write_pixel(shape, x, y, &cov, 1, NULL);
            }
        }
    }
}

static float op_max(void *ctx, float a, float b)
{
    (void)ctx;
    return pc_rs_maxf(a, b);
}

static float op_sub(void *ctx, float a, float b)
{
    (void)ctx;
    return a * (1.0f - b);
}

static float op_min(void *ctx, float a, float b)
{
    (void)ctx;
    return pc_rs_minf(a, b);
}

/* Combining two selections as it was: whole-area f32 buffers. */
static void old_combine_surfaces(const pc_surface *a, const pc_surface *b, pc_rect area, pc_algo_combine_fn f,
                                 pc_surface *out)
{
    float *ra = NULL, *rb = NULL;
    size_t la = 0, lb = 0, i;
    (void)pc_surface_read_region(a, area, &ra, &la, NULL);
    (void)pc_surface_read_region(b, area, &rb, &lb, NULL);
    for (i = 0; i < la && i < lb; i++) {
        ra[i] = f(NULL, ra[i], rb[i]);
    }
    pc_surface_init(out, PC_PIXEL_FORMAT_GRAY8);
    (void)pc_surface_write_region(out, area, ra, la, NULL);
    pc_surface_prune(out);
    free(ra);
    free(rb);
}

static void test_marquee_matches_the_per_pixel_version(void)
{
    static const int32_t shapes[9][4] = {
        {40, 30, 1000, 620}, /* whole tiles inside */
        {3, 5, 1, 1},
        {10, 10, 2, 7},
        {100, 200, 37, 301},
        {255, 255, 258, 2},
        {-300, -200, 900, 600},   /* dragged past the canvas */
        {-500, -500, 2100, 1700}, /* larger than the canvas */
        {1090, 690, 40, 40},
        {2000, 0, 50, 50}, /* entirely outside */
    };
    static const pc_algo_combine_fn ops[3] = {op_max, op_sub, op_min};
    const pc_rect area = {0, 0, 1100, 700};
    pc_surface rect, ell;
    const pc_surface *pairs[3][2];
    float one = 1.0f;
    int k, aa, p;
    for (k = 0; k < 9; k++) {
        pc_rect r =
            pc_rect_from_xywh(shapes[k][0], shapes[k][1], (uint32_t)shapes[k][2], (uint32_t)shapes[k][3]);
        for (aa = 1; aa >= 0; aa--) {
            pc_surface fresh, old;
            CHECK(pc_algo_selection_ellipse_surface(r, area, aa, &fresh, NULL) == PC_OK);
            old_ellipse(r, area, aa, &old);
            if (!pc_surface_eq(&fresh, &old)) {
                PC_T_FAIL("ellipse %d aa=%d", k, aa);
            }
            pc_surface_fini(&fresh);
            pc_surface_fini(&old);
        }
    }
    /* The other modes, against a rectangle and an anti-aliased ellipse with a partial edge. */
    pc_surface_init(&rect, PC_PIXEL_FORMAT_GRAY8);
    (void)pc_surface_fill_rect(&rect, pc_rect_from_xywh(200, 100, 600, 520), &one, 1, NULL);
    old_ellipse(pc_rect_from_xywh(-60, 250, 700, 500), area, true, &ell);
    pairs[0][0] = &rect, pairs[0][1] = &ell;
    pairs[1][0] = &ell, pairs[1][1] = &rect;
    pairs[2][0] = &ell, pairs[2][1] = &ell;
    for (p = 0; p < 3; p++) {
        for (k = 0; k < 3; k++) {
            pc_surface fresh, old;
            CHECK(pc_algo_selection_combine_surfaces(pairs[p][0], pairs[p][1], area, ops[k], NULL, &fresh,
                                                     NULL) == PC_OK);
            old_combine_surfaces(pairs[p][0], pairs[p][1], area, ops[k], &old);
            if (!pc_surface_eq(&fresh, &old)) {
                PC_T_FAIL("pair %d op %d", p, k);
            }
            pc_surface_fini(&fresh);
            pc_surface_fini(&old);
        }
    }
    pc_surface_fini(&rect);
    pc_surface_fini(&ell);
}

int main(int argc, char **argv)
{
    RUN(test_edt_matches_brute_force_for_every_small_mask);
    RUN(test_edt_rectangular_and_partial_bands_match_the_previous_transform);
    RUN(test_edt_large_coordinates_keep_nearby_parabolas_distinct);
    RUN(test_edt_selected_pixels_stay_at_zero_past_coordinate_4096);
    RUN(test_edt_inconsistent_and_empty_dimensions_do_not_panic);
    RUN(test_selection_modifiers_keep_fractional_coverage);
    RUN(test_edt_results_do_not_depend_on_the_worker_count);
    RUN(test_marquee_matches_the_per_pixel_version);
    return pc_t_finish();
}
