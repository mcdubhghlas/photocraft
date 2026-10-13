/* Tests for the segmentation stack (ports of crates/algo/src/segment/tests.rs and the tests in
 * segment/sky.rs) plus never-crash checks. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "base/rmath.h"
#include "harness.h"

/* ---- max-flow ---- */

static void test_maxflow_tiny_graph(void)
{
    pc_algo_maxflow *g = NULL;
    double f = 0;
    (void)pc_algo_maxflow_new(2, 1, &g, NULL);
    pc_algo_maxflow_add_tweights(g, 0, 3, 2);
    pc_algo_maxflow_add_tweights(g, 1, 2, 3);
    CHECK(pc_algo_maxflow_add_edge(g, 0, 1, 1, 0, NULL) == PC_OK);
    CHECK(pc_algo_maxflow_run(g, &f, NULL) == PC_OK);
    CHECK(f == 5.0);
    pc_algo_maxflow_free(g);
}

static void test_maxflow_clrs_network(void)
{
    pc_algo_maxflow *g = NULL;
    double f = 0;
    (void)pc_algo_maxflow_new(4, 5, &g, NULL);
    pc_algo_maxflow_add_tweights(g, 0, 16, 0);
    pc_algo_maxflow_add_tweights(g, 1, 13, 0);
    pc_algo_maxflow_add_tweights(g, 2, 0, 20);
    pc_algo_maxflow_add_tweights(g, 3, 0, 4);
    (void)pc_algo_maxflow_add_edge(g, 0, 1, 10, 4, NULL);
    (void)pc_algo_maxflow_add_edge(g, 0, 2, 12, 0, NULL);
    (void)pc_algo_maxflow_add_edge(g, 2, 1, 9, 0, NULL);
    (void)pc_algo_maxflow_add_edge(g, 1, 3, 14, 0, NULL);
    (void)pc_algo_maxflow_add_edge(g, 3, 2, 7, 0, NULL);
    (void)pc_algo_maxflow_run(g, &f, NULL);
    CHECK(f == 23.0);
    CHECK(pc_algo_maxflow_in_source(g, 0) && pc_algo_maxflow_in_source(g, 1) &&
          pc_algo_maxflow_in_source(g, 3));
    CHECK(!pc_algo_maxflow_in_source(g, 2));
    CHECK(pc_algo_maxflow_residual_out_of_source_side(g) == 0.0f);
    pc_algo_maxflow_free(g);
}

/* Reference max flow (Edmonds-Karp on a dense matrix; node 0 = s, n - 1 = t). */
static double edmonds_karp(double *cap, size_t n)
{
    double flow = 0.0;
    size_t *prev = malloc(n * sizeof(size_t)), *q = malloc(n * sizeof(size_t));
    for (;;) {
        size_t qh = 0, qt = 0, u, v;
        double b = 1e300;
        for (v = 0; v < n; v++) {
            prev[v] = SIZE_MAX;
        }
        prev[0] = 0;
        q[qt++] = 0;
        while (qh < qt) {
            u = q[qh++];
            for (v = 0; v < n; v++) {
                if (prev[v] == SIZE_MAX && cap[u * n + v] > 1e-9) {
                    prev[v] = u;
                    q[qt++] = v;
                }
            }
        }
        if (prev[n - 1] == SIZE_MAX) {
            break;
        }
        for (v = n - 1; v != 0; v = prev[v]) {
            b = b < cap[prev[v] * n + v] ? b : cap[prev[v] * n + v];
        }
        for (v = n - 1; v != 0; v = prev[v]) {
            cap[prev[v] * n + v] -= b;
            cap[v * n + prev[v]] += b;
        }
        flow += b;
    }
    free(prev);
    free(q);
    return flow;
}

static void test_maxflow_matches_reference_on_random_graphs(void)
{
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(7);
    size_t trial;
    for (trial = 0; trial < 60; trial++) {
        size_t n = 3 + (trial % 12), m = n + 2, i, j, ne = 0;
        double *cap = calloc(m * m, sizeof(double)), *orig = malloc(m * m * sizeof(double)), ref, f = 0,
               cut = 0;
        size_t (*edges)[2] = malloc(n * n * sizeof *edges);
        float (*ec)[2] = malloc(n * n * sizeof *ec);
        pc_algo_maxflow *g = NULL;
        (void)pc_algo_maxflow_new(n, n * 3, &g, NULL);
        for (i = 0; i < n; i++) {
            float s = roundf(pc_algo_segment_rng_f32(&rng) * 10.0f),
                  t = roundf(pc_algo_segment_rng_f32(&rng) * 10.0f);
            s = pc_algo_segment_rng_f32(&rng) < 0.4f ? 0.0f : s;
            t = pc_algo_segment_rng_f32(&rng) < 0.4f ? 0.0f : t;
            pc_algo_maxflow_add_tweights(g, i, s, t);
            cap[0 * m + i + 1] += s;
            cap[(i + 1) * m + n + 1] += t;
        }
        for (i = 0; i < n; i++) {
            for (j = i + 1; j < n; j++) {
                if (pc_algo_segment_rng_f32(&rng) < 0.35f) {
                    float a = roundf(pc_algo_segment_rng_f32(&rng) * 8.0f),
                          b = roundf(pc_algo_segment_rng_f32(&rng) * 8.0f);
                    (void)pc_algo_maxflow_add_edge(g, i, j, a, b, NULL);
                    cap[(i + 1) * m + j + 1] += a;
                    cap[(j + 1) * m + i + 1] += b;
                    edges[ne][0] = i;
                    edges[ne][1] = j;
                    ec[ne][0] = a;
                    ec[ne][1] = b;
                    ne++;
                }
            }
        }
        memcpy(orig, cap, m * m * sizeof(double));
        ref = edmonds_karp(cap, m);
        (void)pc_algo_maxflow_run(g, &f, NULL);
        CHECK_NEAR(f, ref, 1e-3);
        for (i = 0; i < n; i++) {
            cut += pc_algo_maxflow_in_source(g, i) ? orig[(i + 1) * m + n + 1] : orig[0 * m + i + 1];
        }
        for (i = 0; i < ne; i++) {
            bool si = pc_algo_maxflow_in_source(g, edges[i][0]),
                 sj = pc_algo_maxflow_in_source(g, edges[i][1]);
            if (si && !sj) {
                cut += ec[i][0];
            }
            if (sj && !si) {
                cut += ec[i][1];
            }
        }
        CHECK_NEAR(cut, ref, 1e-3);
        pc_algo_maxflow_free(g);
        free(cap);
        free(orig);
        free(edges);
        free(ec);
    }
}

static void test_grid_cut_separates_two_halves(void)
{
    pc_algo_rgb_image img;
    size_t i, n = 200;
    uint8_t fixed[200];
    float zeros[200];
    bool cut[200], ok = true;
    (void)pc_algo_rgb_image_init(&img, 20, 10, NULL);
    for (i = 0; i < n; i++) {
        bool l = i % 20 < 10;
        img.px[i][0] = l ? 0.9f : 0.1f;
        img.px[i][1] = 0.1f;
        img.px[i][2] = l ? 0.1f : 0.9f;
        zeros[i] = 0;
        fixed[i] = PC_ALGO_SEGMENT_FREE;
    }
    fixed[5 * 20 + 1] = PC_ALGO_SEGMENT_HARD_FG;
    fixed[5 * 20 + 18] = PC_ALGO_SEGMENT_HARD_BG;
    CHECK(pc_algo_segment_grid_cut(&img, zeros, zeros, fixed, 50.0f, pc_algo_segment_contrast_beta(&img), cut,
                                   NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        ok &= cut[i] == (i % 20 < 10);
    }
    CHECK(ok);
    pc_algo_rgb_image_fini(&img);
}

/* ---- GMM ---- */

static float d2(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

static void test_gmm_recovers_synthetic_clusters(void)
{
    static const float truth[3][5] = {
        {0.2f, 0.3f, 0.8f, 0.03f, 0.5f}, {0.8f, 0.2f, 0.1f, 0.05f, 0.3f}, {0.5f, 0.9f, 0.4f, 0.02f, 0.2f}};
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(3);
    float (*s)[3] = malloc(6000 * sizeof *s);
    const float z0[3] = {0.2f, 0.3f, 0.8f}, z1[3] = {0.5f, 0.5f, 0.5f};
    size_t n = 0, t, j, c;
    pc_algo_gmm g, r;
    bool ok = false;
    for (t = 0; t < 3; t++) {
        size_t cnt = (size_t)(truth[t][4] * 6000.0f);
        for (j = 0; j < cnt; j++) {
            for (c = 0; c < 3; c++) {
                s[n][c] = truth[t][c] + truth[t][3] * pc_algo_segment_rng_normal(&rng);
            }
            n++;
        }
    }
    CHECK(pc_algo_gmm_fit((const float (*)[3])s, n, 3, PC_ALGO_GMM_DEFAULT_REG, &g, &ok, NULL) == PC_OK &&
          ok);
    CHECK_EQ_UINT(g.n, 3);
    for (t = 0; t < 3; t++) {
        size_t best = 0;
        for (j = 1; j < g.n; j++) {
            if (d2(g.comps[j].mean, truth[t]) < d2(g.comps[best].mean, truth[t])) {
                best = j;
            }
        }
        CHECK(sqrtf(d2(g.comps[best].mean, truth[t])) < 0.01f);
        CHECK(fabsf(g.comps[best].weight - truth[t][4]) < 0.03f);
        CHECK(fabsf(sqrtf(g.comps[best].cov[0][0]) - truth[t][3]) < 0.01f);
    }
    CHECK(pc_algo_gmm_neg_log(&g, z0) < pc_algo_gmm_neg_log(&g, z1));
    CHECK(pc_algo_gmm_refit(&g, (const float (*)[3])s, n, &r, &ok, NULL) == PC_OK && ok && r.n == 3);
    pc_algo_gmm_fini(&r);
    pc_algo_gmm_fini(&g);
    free(s);
}

/* ---- SLIC ---- */

static void test_slic_counts_compactness_and_edges(void)
{
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(11);
    size_t w = 120, h = 120, x, y, l;
    pc_algo_rgb_image img;
    pc_algo_superpixels sp;
    size_t (*bb)[4], *left, *total;
    bool ok_size = true, ok_pure = true;
    int c;
    (void)pc_algo_rgb_image_init(&img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            static const float l0[3] = {0.8f, 0.3f, 0.2f}, r0[3] = {0.2f, 0.5f, 0.8f};
            for (c = 0; c < 3; c++) {
                img.px[y * w + x][c] = x < 60 ? l0[c] : r0[c];
            }
        }
    }
    for (x = 0; x < w * h; x++) {
        for (c = 0; c < 3; c++) {
            img.px[x][c] = pc_clampf(img.px[x][c] + 0.02f * pc_algo_segment_rng_normal(&rng), 0.0f, 1.0f);
        }
    }
    CHECK(pc_algo_slic(&img, 64, 10.0f, 10, &sp, NULL) == PC_OK);
    CHECK(sp.count >= 40 && sp.count <= 90);
    bb = malloc(sp.count * sizeof *bb);
    left = calloc(sp.count, sizeof(size_t));
    total = calloc(sp.count, sizeof(size_t));
    for (l = 0; l < sp.count; l++) {
        bb[l][0] = bb[l][1] = SIZE_MAX;
        bb[l][2] = bb[l][3] = 0;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            l = sp.labels[y * w + x];
            bb[l][0] = bb[l][0] < x ? bb[l][0] : x;
            bb[l][1] = bb[l][1] < y ? bb[l][1] : y;
            bb[l][2] = bb[l][2] > x ? bb[l][2] : x;
            bb[l][3] = bb[l][3] > y ? bb[l][3] : y;
            total[l]++;
            left[l] += x < 60;
        }
    }
    for (l = 0; l < sp.count; l++) {
        size_t big = left[l] > total[l] - left[l] ? left[l] : total[l] - left[l];
        ok_size &= total[l] > 0 && (float)(bb[l][2] - bb[l][0] + 1) <= 3.0f * sp.step &&
                   (float)(bb[l][3] - bb[l][1] + 1) <= 3.0f * sp.step;
        ok_pure &= total[l] > 0 && (float)big / (float)total[l] > 0.95f;
    }
    CHECK(ok_size);
    CHECK(ok_pure);
    free(bb);
    free(left);
    free(total);
    pc_algo_superpixels_fini(&sp);
    pc_algo_rgb_image_fini(&img);
}

/* ---- scenes ---- */

static bool *disc_scene(size_t w, size_t h, float cx, float cy, float r, uint64_t seed,
                        pc_algo_rgb_image *img)
{
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(seed);
    bool *truth = malloc(w * h * sizeof(bool));
    size_t x, y;
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, n, *p = img->px[y * w + x];
            bool inside = dx * dx + dy * dy <= r * r;
            int k;
            truth[y * w + x] = inside;
            n = 0.05f * pc_algo_segment_rng_normal(&rng);
            if (inside) {
                float t = pc_clampf(sinf((float)(x + y) * 0.35f) * 0.5f + 0.5f, 0.0f, 1.0f);
                p[0] = 0.85f + 0.05f * t + n;
                p[1] = 0.2f + 0.3f * t + n;
                p[2] = 0.15f + n;
            } else {
                p[0] = 0.25f + n;
                p[1] = 0.45f + n;
                p[2] = 0.7f + n;
            }
            for (k = 0; k < 3; k++) {
                p[k] = pc_clampf(p[k], 0.0f, 1.0f);
            }
        }
    }
    return truth;
}

static float iou(const pc_algo_region *r, const bool *truth, size_t w, size_t h)
{
    size_t inter = 0, uni = 0, x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            bool a = pc_algo_region_at(r, (int32_t)x, (int32_t)y) >= 0.5f, b = truth[y * w + x];
            inter += a && b;
            uni += a || b;
        }
    }
    return (float)inter / (float)(uni > 0 ? uni : 1);
}

static void test_grabcut_trimap_on_disc(void)
{
    pc_algo_rgb_image img;
    bool *truth = disc_scene(120, 100, 60, 50, 30, 1, &img), some = false;
    uint8_t *tri = calloc(12000, 1);
    size_t x, y, inter = 0, uni = 0, i;
    pc_algo_gmm f, b;
    for (y = 12; y < 88; y++) {
        for (x = 22; x < 98; x++) {
            tri[y * 120 + x] = PC_ALGO_GRABCUT_PR_FG;
        }
    }
    CHECK(pc_algo_grabcut(&img, tri, 8, &f, &b, &some, NULL) == PC_OK && some);
    for (i = 0; i < 12000; i++) {
        bool a = tri[i] == PC_ALGO_GRABCUT_PR_FG || tri[i] == PC_ALGO_GRABCUT_FG;
        inter += a && truth[i];
        uni += a || truth[i];
    }
    CHECK((float)inter / (float)uni > 0.95f);
    pc_algo_gmm_fini(&f);
    pc_algo_gmm_fini(&b);
    free(tri);
    free(truth);
    pc_algo_rgb_image_fini(&img);
}

static void test_object_select_disc_full_resolution(void)
{
    pc_algo_rgb_image img;
    bool *truth = disc_scene(200, 160, 100, 80, 50, 2, &img);
    pc_algo_image_sampler hold = {&img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region r;
    CHECK(pc_algo_grabcut_object_select(&s, pc_rect_make(0, 0, 200, 160), pc_rect_make(40, 20, 160, 140),
                                        1000000, &r, NULL) == PC_OK);
    CHECK(iou(&r, truth, 200, 160) > 0.95f);
    pc_algo_region_fini(&r);
    free(truth);
    pc_algo_rgb_image_fini(&img);
}

static void test_object_select_downsampled_with_band_refinement(void)
{
    pc_algo_rgb_image img;
    bool *truth = disc_scene(640, 480, 330, 250, 150, 3, &img);
    pc_algo_image_sampler hold = {&img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region r;
    int32_t x, y, far_wrong = 0;
    CHECK(pc_algo_grabcut_object_select(&s, pc_rect_make(0, 0, 640, 480), pc_rect_make(160, 80, 500, 420),
                                        20000, &r, NULL) == PC_OK);
    CHECK(iou(&r, truth, 640, 480) > 0.95f);
    for (y = 0; y < 480; y++) {
        for (x = 0; x < 640; x++) {
            float dx = (float)x + 0.5f - 330.0f, dy = (float)y + 0.5f - 250.0f,
                  d = sqrtf(dx * dx + dy * dy) - 150.0f;
            if (fabsf(d) > 2.0f && (pc_algo_region_at(&r, x, y) >= 0.5f) != truth[y * 640 + x]) {
                far_wrong++;
            }
        }
    }
    CHECK(far_wrong < 300);
    pc_algo_region_fini(&r);
    free(truth);
    pc_algo_rgb_image_fini(&img);
}

/* ---- Subject, focus ---- */

static void test_select_subject_finds_centred_object(void)
{
    pc_algo_rgb_image img;
    bool *truth = disc_scene(200, 150, 100, 75, 40, 6, &img);
    pc_algo_image_sampler hold = {&img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region r;
    CHECK(pc_algo_subject_select(&s, pc_rect_make(0, 0, 200, 150), &r, NULL) == PC_OK);
    CHECK(iou(&r, truth, 200, 150) > 0.9f);
    pc_algo_region_fini(&r);
    free(truth);
    pc_algo_rgb_image_fini(&img);
}

static void test_saliency_highlights_the_object(void)
{
    pc_algo_rgb_image img;
    bool *truth = disc_scene(120, 90, 60, 45, 22, 8, &img);
    float *sal = NULL, fi = 0, fn = 0, bi = 0, bn = 0;
    size_t i;
    CHECK(pc_algo_subject_saliency(&img, &sal, NULL) == PC_OK);
    for (i = 0; i < 120 * 90; i++) {
        if (truth[i]) {
            fi += sal[i];
            fn += 1;
        } else {
            bi += sal[i];
            bn += 1;
        }
    }
    CHECK(fi / fn > 0.6f && bi / bn < 0.25f);
    free(sal);
    free(truth);
    pc_algo_rgb_image_fini(&img);
}

static void test_focus_area_selects_sharp_half(void)
{
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(12);
    size_t w = 160, h = 100, x, y, l = 0, rr = 0;
    float *noise = malloc(w * h * sizeof(float)), *bl = NULL;
    pc_algo_rgb_image img;
    pc_algo_image_sampler hold = {&img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region r;
    for (x = 0; x < w * h; x++) {
        noise[x] = pc_algo_segment_rng_f32(&rng);
    }
    (void)pc_algo_matting_box_mean(noise, w, h, 4, &bl, NULL);
    (void)pc_algo_rgb_image_init(&img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float v = x < 80 ? noise[y * w + x] : bl[y * w + x];
            img.px[y * w + x][0] = v;
            img.px[y * w + x][1] = v * 0.8f;
            img.px[y * w + x][2] = 0.3f;
        }
    }
    CHECK(pc_algo_focus_area(&s, pc_rect_make(0, 0, 160, 100), 0.5f, 0.0f, &r, NULL) == PC_OK);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            if (pc_algo_region_at(&r, (int32_t)x, (int32_t)y) >= 0.5f) {
                if (x < 80) {
                    l++;
                } else {
                    rr++;
                }
            }
        }
    }
    CHECK((float)l > 0.85f * 80.0f * (float)h);
    CHECK((float)rr < 0.1f * 80.0f * (float)h);
    pc_algo_region_fini(&r);
    free(noise);
    free(bl);
    pc_algo_rgb_image_fini(&img);
}

/* ---- helpers ---- */

static void test_sampler_downsampling_matches_image_downsample(void)
{
    pc_algo_rgb_image img, a, b;
    pc_algo_image_sampler hold = {&img, 5, 3};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    size_t x, y, i;
    bool ok = true;
    (void)pc_algo_rgb_image_init(&img, 37, 29, NULL);
    for (y = 0; y < 29; y++) {
        for (x = 0; x < 37; x++) {
            img.px[y * 37 + x][0] = (float)x / 37.0f;
            img.px[y * 37 + x][1] = (float)y / 29.0f;
            img.px[y * 37 + x][2] = (float)((x * y) % 7) / 7.0f;
        }
    }
    (void)pc_algo_sampler_rgb_scaled(&s, pc_rect_make(5, 3, 42, 32), 4, &a, NULL);
    (void)pc_algo_rgb_image_downsample(&img, 4, &b, NULL);
    CHECK(a.w == b.w && a.h == b.h);
    for (i = 0; i < a.w * a.h * 3; i++) {
        ok &= fabsf(((float *)a.px)[i] - ((float *)b.px)[i]) < 1e-5f;
    }
    CHECK(ok);
    pc_algo_rgb_image_fini(&a);
    pc_algo_rgb_image_fini(&b);
    pc_algo_rgb_image_fini(&img);
}

static void test_clean_mask_drops_islands_and_fills_holes(void)
{
    size_t w = 40, h = 40, x, y;
    bool m[1600] = {false}, c[1600];
    for (y = 5; y < 30; y++) {
        for (x = 5; x < 30; x++) {
            m[y * w + x] = true;
        }
    }
    m[15 * w + 15] = false;
    m[36 * w + 36] = true;
    CHECK(pc_algo_segment_clean_mask(m, w, h, 0.1f, 0.05f, c, NULL) == PC_OK);
    CHECK(c[15 * w + 15]);
    CHECK(!c[36 * w + 36]);
}

/* ---- Sky (sky.rs tests) ---- */

static void landscape(size_t w, size_t h, pc_algo_rgb_image *img)
{
    size_t horizon = h * 3 / 5, x, y;
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float cx = (float)w * 0.7f, cy = (float)h * 0.35f, rr = (float)h * 0.18f, *p = img->px[y * w + x];
            float ddx = (float)x - cx, ddy = (float)y - cy;
            bool crown = sqrtf(ddx * ddx + ddy * ddy) < rr;
            bool trunk = fabsf((float)x - cx) < 3.0f && (float)y > cy && y < horizon;
            float n = ((float)((x * 7919 + y * 104729) % 97) / 97.0f - 0.5f) * 0.25f;
            if (crown) {
                p[0] = 0.1f + n * 0.5f, p[1] = 0.35f + n, p[2] = 0.1f + n * 0.5f;
            } else if (trunk) {
                p[0] = 0.3f + n * 0.3f, p[1] = 0.2f, p[2] = 0.1f;
            } else if (y < horizon) {
                float t = (float)y / (float)horizon;
                p[0] = 0.25f + 0.4f * t, p[1] = 0.5f + 0.3f * t, p[2] = 0.9f + 0.05f * t;
            } else {
                p[0] = 0.35f + n, p[1] = 0.3f + n, p[2] = 0.15f + n * 0.5f;
            }
        }
    }
}

static void test_sky_finds_sky_and_not_ground_or_tree(void)
{
    pc_algo_rgb_image img;
    pc_algo_image_sampler hold = {&img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region r;
    landscape(160, 100, &img);
    CHECK(pc_algo_sky_select(&s, pc_rect_make(0, 0, 160, 100), pc_algo_sky_params_default(), &r, NULL) ==
          PC_OK);
    CHECK(!pc_algo_region_is_none(&r));
    CHECK(pc_algo_region_at(&r, 10, 5) > 0.9f);
    CHECK(pc_algo_region_at(&r, 80, 50) > 0.9f);
    CHECK(pc_algo_region_at(&r, 10, 80) < 0.1f);
    CHECK(pc_algo_region_at(&r, 112, 35) < 0.1f);
    CHECK(r.bbox.y1 <= 64);
    pc_algo_region_fini(&r);
    pc_algo_rgb_image_fini(&img);
}

static void test_sky_none_in_a_ground_only_image(void)
{
    pc_algo_rgb_image img;
    pc_algo_image_sampler hold = {&img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region r;
    size_t x, y;
    (void)pc_algo_rgb_image_init(&img, 64, 64, NULL);
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            float n = ((float)((x * 7919 + y * 104729) % 97) / 97.0f - 0.5f) * 0.3f;
            img.px[y * 64 + x][0] = 0.35f + n;
            img.px[y * 64 + x][1] = 0.3f + n;
            img.px[y * 64 + x][2] = 0.12f;
        }
    }
    CHECK(pc_algo_sky_select(&s, pc_rect_make(0, 0, 64, 64), pc_algo_sky_params_default(), &r, NULL) ==
          PC_OK);
    CHECK(pc_algo_region_is_none(&r));
    pc_algo_rgb_image_fini(&img);
}

static void test_sky_overcast_is_found(void)
{
    pc_algo_rgb_image img;
    pc_algo_image_sampler hold = {&img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region r;
    size_t i;
    (void)pc_algo_rgb_image_init(&img, 64, 64, NULL);
    for (i = 0; i < 64 * 64; i++) {
        bool top = i / 64 < 30;
        img.px[i][0] = top ? 0.85f : 0.2f;
        img.px[i][1] = top ? 0.86f : 0.3f;
        img.px[i][2] = top ? 0.88f : 0.1f;
    }
    CHECK(pc_algo_sky_select(&s, pc_rect_make(0, 0, 64, 64), pc_algo_sky_params_default(), &r, NULL) ==
          PC_OK);
    CHECK(pc_algo_region_at(&r, 30, 10) > 0.9f && pc_algo_region_at(&r, 30, 50) < 0.1f);
    pc_algo_region_fini(&r);
    pc_algo_rgb_image_fini(&img);
}

static void test_sky_colour_prior_ranks_sky_over_foliage(void)
{
    const float a[3] = {0.4f, 0.6f, 0.95f}, b[3] = {0.15f, 0.4f, 0.15f}, c[3] = {0.9f, 0.9f, 0.92f};
    CHECK(pc_algo_sky_colour_prior(a) > 0.8f);
    CHECK(pc_algo_sky_colour_prior(b) < 0.1f);
    CHECK(pc_algo_sky_colour_prior(c) > 0.8f);
}

/* ---- never crash ---- */

static pc_status failing_rgba(const void *ctx, pc_rect r, float (*out)[4], size_t n, pc_error *err)
{
    (void)ctx;
    (void)r;
    (void)out;
    (void)n;
    return pc_fail(err, PC_ERR_IO, "sampler failed");
}

static pc_status nan_rgba(const void *ctx, pc_rect r, float (*out)[4], size_t n, pc_error *err)
{
    size_t i;
    (void)ctx;
    (void)r;
    (void)err;
    for (i = 0; i < n; i++) {
        out[i][0] = out[i][1] = out[i][2] = out[i][3] = i % 3 ? NAN : 1.0f;
    }
    return PC_OK;
}

static void test_hostile_inputs(void)
{
    pc_algo_sampler bad = {failing_rgba, NULL}, nanp = {nan_rgba, NULL};
    pc_algo_region r;
    pc_algo_maxflow *g = NULL;
    pc_algo_rgb_image img;
    float (*c)[3] = NULL, *o = NULL;
    size_t nc;
    const float pt[1][2] = {{NAN, NAN}}, pt2[1][2] = {{10, 10}};
    const float z[1][3] = {{0, 0, 0}};
    pc_algo_gmm gm;
    bool some = true;
    pc_rect cv = {0, 0, 64, 64};
    /* Sampler failures propagate as errors. */
    CHECK(pc_algo_grabcut_object_select(&bad, cv, pc_rect_make(10, 10, 50, 50), 1000, &r, NULL) == PC_ERR_IO);
    CHECK(pc_algo_quick_select(&bad, cv, pt2, 1, 8, &r, NULL) != PC_OK);
    CHECK(pc_algo_sky_select(&bad, cv, pc_algo_sky_params_default(), &r, NULL) != PC_OK);
    CHECK(pc_algo_subject_select(&bad, cv, &r, NULL) != PC_OK);
    CHECK(pc_algo_focus_area(&bad, cv, 0.5f, 0.0f, &r, NULL) != PC_OK);
    /* NaN pixels and points never crash. */
    CHECK(pc_algo_grabcut_object_select(&nanp, cv, pc_rect_make(10, 10, 50, 50), 1000, &r, NULL) == PC_OK);
    pc_algo_region_fini(&r);
    CHECK(pc_algo_quick_select(&nanp, cv, pt, 1, NAN, &r, NULL) == PC_OK);
    pc_algo_region_fini(&r);
    CHECK(pc_algo_quick_select(&nanp, cv, pt2, 1, 8, &r, NULL) == PC_OK);
    pc_algo_region_fini(&r);
    CHECK(pc_algo_sky_select(&nanp, cv, pc_algo_sky_params_default(), &r, NULL) == PC_OK);
    pc_algo_region_fini(&r);
    CHECK(pc_algo_subject_select(&nanp, cv, &r, NULL) == PC_OK);
    pc_algo_region_fini(&r);
    CHECK(pc_algo_focus_area(&nanp, cv, NAN, NAN, &r, NULL) == PC_OK);
    pc_algo_region_fini(&r);
    /* Degenerate canvases. */
    CHECK(pc_algo_subject_select(&nanp, pc_rect_make(0, 0, 3, 100), &r, NULL) == PC_OK &&
          pc_algo_region_is_none(&r));
    CHECK(pc_algo_quick_select(&nanp, cv, NULL, 0, 8, &r, NULL) == PC_OK && pc_algo_region_is_none(&r));
    /* Graph misuse. */
    (void)pc_algo_maxflow_new(2, 0, &g, NULL);
    CHECK(pc_algo_maxflow_add_edge(g, 0, 0, 1, 1, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_maxflow_add_edge(g, 0, 7, 1, 1, NULL) == PC_ERR_INVALID);
    pc_algo_maxflow_add_tweights(g, 9, 1, 1);
    CHECK(!pc_algo_maxflow_in_source(g, 9));
    pc_algo_maxflow_free(g);
    CHECK(pc_algo_maxflow_new(SIZE_MAX, 1, &g, NULL) == PC_ERR_LIMIT);
    /* Models and images. */
    CHECK(pc_algo_gmm_kmeans(NULL, 0, 3, 10, &c, &nc, NULL) == PC_ERR_INVALID);
    CHECK(pc_algo_gmm_fit(z, 1, 1, 0.0f, &gm, &some, NULL) == PC_OK && !some);
    CHECK(pc_algo_rgb_image_init(&img, SIZE_MAX / 2, 4, NULL) == PC_ERR_LIMIT);
    (void)pc_algo_rgb_image_init(&img, 0, 0, NULL);
    CHECK(pc_algo_focus_sharpness(&img, 0.5f, &o, NULL) == PC_OK && o == NULL);
    CHECK(pc_algo_subject_otsu(NULL, 0) == 0.5f / 255.0f);
}

int main(int argc, char **argv)
{
    RUN(test_maxflow_tiny_graph);
    RUN(test_maxflow_clrs_network);
    RUN(test_maxflow_matches_reference_on_random_graphs);
    RUN(test_grid_cut_separates_two_halves);
    RUN(test_gmm_recovers_synthetic_clusters);
    RUN(test_slic_counts_compactness_and_edges);
    RUN(test_grabcut_trimap_on_disc);
    RUN(test_object_select_disc_full_resolution);
    RUN(test_object_select_downsampled_with_band_refinement);
    RUN(test_select_subject_finds_centred_object);
    RUN(test_saliency_highlights_the_object);
    RUN(test_focus_area_selects_sharp_half);
    RUN(test_sampler_downsampling_matches_image_downsample);
    RUN(test_clean_mask_drops_islands_and_fills_holes);
    RUN(test_sky_finds_sky_and_not_ground_or_tree);
    RUN(test_sky_none_in_a_ground_only_image);
    RUN(test_sky_overcast_is_found);
    RUN(test_sky_colour_prior_ranks_sky_over_foliage);
    RUN(test_hostile_inputs);
    return pc_t_finish();
}
