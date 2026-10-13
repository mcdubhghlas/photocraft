/* Max-flow solvers and the sparse grid cut: ports of the push/relabel tests
 * (crates/algo/src/segment/maxflow/push_relabel/tests.rs), the edge-start merge tests
 * (segment/edge_starts/tests.rs) and maxflow_can_be_cancelled_and_otherwise_cuts_the_same
 * (segment/tests.rs). The remaining max-flow tests of segment/tests.rs are in test_segment.c.
 *
 * Not ported: solver_workload_release_comparison (an #[ignore]d release timing comparison; its
 * equality assertions are covered by float_capacity_graphs_match_bk and the cut tests here). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

/* ---- helpers ---- */

/* The input graph as the test built it, to price a labelling (the reference's `energy` reads the
 * graph's residual terminal capacities and arcs; the full cut cost is the same number). */
typedef struct edge {
    size_t i, j;
    float cap, rev;
} edge;

typedef struct spec {
    size_t n, nedges;
    float *src, *snk;
    edge *edges;
} spec;

static void spec_init(spec *s, size_t n, size_t max_edges)
{
    s->n = n;
    s->nedges = 0;
    s->src = calloc(n, sizeof(float));
    s->snk = calloc(n, sizeof(float));
    s->edges = calloc(max_edges ? max_edges : 1, sizeof(edge));
}

static void spec_fini(spec *s)
{
    free(s->src);
    free(s->snk);
    free(s->edges);
}

static void add_t(pc_algo_maxflow *g, spec *s, size_t i, float a, float b)
{
    pc_algo_maxflow_add_tweights(g, i, a, b);
    s->src[i] += a;
    s->snk[i] += b;
}

static void add_e(pc_algo_maxflow *g, spec *s, size_t i, size_t j, float cap, float rev)
{
    CHECK(pc_algo_maxflow_add_edge(g, i, j, cap, rev, NULL) == PC_OK);
    s->edges[s->nedges++] = (edge){i, j, cap, rev};
}

/* Cost of the cut that puts labels[i] (true: source side) on each node. */
static double energy(const spec *s, const bool *labels)
{
    double sum = 0.0;
    size_t i;
    for (i = 0; i < s->n; i++) {
        sum += (double)(labels[i] ? s->snk[i] : s->src[i]);
    }
    for (i = 0; i < s->nedges; i++) {
        const edge *e = &s->edges[i];
        if (labels[e->i] && !labels[e->j]) {
            sum += (double)e->cap;
        }
        if (labels[e->j] && !labels[e->i]) {
            sum += (double)e->rev;
        }
    }
    return sum;
}

static double run(pc_algo_maxflow *g, int solver)
{
    double f = -1.0;
    pc_status st = solver == 0   ? pc_algo_maxflow_run_push_relabel(g, &f, NULL)
                   : solver == 1 ? pc_algo_maxflow_run_bk(g, &f, NULL)
                                 : pc_algo_maxflow_run(g, &f, NULL);
    CHECK(st == PC_OK);
    return f;
}

static pc_algo_maxflow *clone(const pc_algo_maxflow *g)
{
    pc_algo_maxflow *c = NULL;
    CHECK(pc_algo_maxflow_clone(g, &c, NULL) == PC_OK);
    return c;
}

static bool same_side(const pc_algo_maxflow *a, const pc_algo_maxflow *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (pc_algo_maxflow_in_source(a, i) != pc_algo_maxflow_in_source(b, i)) {
            return false;
        }
    }
    return true;
}

/* ---- push/relabel (push_relabel/tests.rs) ---- */

static uint32_t xs32(uint32_t *r)
{
    *r ^= *r << 13;
    *r ^= *r >> 17;
    *r ^= *r << 5;
    return *r;
}

static void test_preflow_matches_bk_on_integer_graphs_and_returns_unused_source(void)
{
    uint32_t rng = 7;
    size_t n, i, j;
    int t, bad = 0;
    for (n = 1; n < 40; n++) {
        for (t = 0; t < 30; t++) {
            pc_algo_maxflow *g = NULL, *old;
            double a, b;
            CHECK(pc_algo_maxflow_new(n, n * 4, &g, NULL) == PC_OK);
            for (i = 0; i < n; i++) {
                float s = (float)(xs32(&rng) % 10);
                pc_algo_maxflow_add_tweights(g, i, s, (float)(xs32(&rng) % 10));
            }
            for (i = 0; i < n; i++) {
                for (j = i + 1; j < n; j++) {
                    if (xs32(&rng) % 7 == 0) {
                        float c = (float)(xs32(&rng) % 8);
                        (void)pc_algo_maxflow_add_edge(g, i, j, c, (float)(xs32(&rng) % 8), NULL);
                    }
                }
            }
            old = clone(g);
            a = run(g, 0);
            b = run(old, 1);
            bad += a != b || !same_side(g, old, n) || pc_algo_maxflow_residual_out_of_source_side(g) != 0.0f;
            b = pc_algo_maxflow_flow(g);
            bad += run(g, 0) != b;
            pc_algo_maxflow_free(g);
            pc_algo_maxflow_free(old);
        }
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_fractional_flows_match_brute_force_minimum_cuts(void)
{
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(911);
    size_t n, i, j, code;
    int t, bad = 0;
    bool labels[12], smallest[12];
    for (n = 1; n <= 12; n++) {
        for (t = 0; t < 20; t++) {
            pc_algo_maxflow *g = NULL;
            spec s;
            double optimum = INFINITY, f;
            spec_init(&s, n, n * n);
            CHECK(pc_algo_maxflow_new(n, n * n, &g, NULL) == PC_OK);
            for (i = 0; i < n; i++) {
                float a = (float)(pc_algo_segment_rng_next(&rng) % 31) / 32.0f;
                add_t(g, &s, i, a, (float)(pc_algo_segment_rng_next(&rng) % 31) / 32.0f);
                for (j = 0; j < i; j++) {
                    if (pc_algo_segment_rng_next(&rng) % 5 == 0) {
                        float c = (float)(pc_algo_segment_rng_next(&rng) % 31) / 32.0f;
                        add_e(g, &s, i, j, c, (float)(pc_algo_segment_rng_next(&rng) % 31) / 32.0f);
                    }
                }
            }
            for (i = 0; i < n; i++) {
                smallest[i] = true;
            }
            for (code = 0; code < (size_t)1 << n; code++) {
                double v;
                for (i = 0; i < n; i++) {
                    labels[i] = (code & ((size_t)1 << i)) != 0;
                }
                v = energy(&s, labels);
                if (v < optimum) {
                    optimum = v;
                    memcpy(smallest, labels, n * sizeof(bool));
                } else if (v == optimum) {
                    for (i = 0; i < n; i++) {
                        smallest[i] = smallest[i] && labels[i];
                    }
                }
            }
            f = run(g, 0);
            for (i = 0; i < n; i++) {
                labels[i] = pc_algo_maxflow_in_source(g, i);
            }
            if (f != optimum || energy(&s, labels) != optimum || memcmp(labels, smallest, n * sizeof(bool))) {
                bad++;
                PC_T_FAIL("n=%zu: flow %g, optimum %g", n, f, optimum);
            }
            pc_algo_maxflow_free(g);
            spec_fini(&s);
        }
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_float_capacity_graphs_match_bk(void)
{
    static const size_t ns[] = {1, 2, 7, 16, 39, 64};
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(819);
    size_t k, i, j;
    int c, bad = 0;
    for (k = 0; k < sizeof ns / sizeof ns[0]; k++) {
        size_t n = ns[k];
        for (c = 0; c < 100; c++) {
            pc_algo_maxflow *g = NULL, *old;
            double a, b;
            CHECK(pc_algo_maxflow_new(n, n * 4, &g, NULL) == PC_OK);
            for (i = 0; i < n; i++) {
                float s = (float)(pc_algo_segment_rng_next(&rng) % 1001) / 97.0f;
                pc_algo_maxflow_add_tweights(g, i, s, (float)(pc_algo_segment_rng_next(&rng) % 1001) / 97.0f);
                for (j = 0; j < i; j++) {
                    if (pc_algo_segment_rng_next(&rng) % 7 == 0) {
                        float e = (float)(pc_algo_segment_rng_next(&rng) % 1001) / 113.0f;
                        (void)pc_algo_maxflow_add_edge(
                            g, i, j, e, (float)(pc_algo_segment_rng_next(&rng) % 1001) / 113.0f, NULL);
                    }
                }
            }
            old = clone(g);
            a = run(g, 0);
            b = run(old, 1);
            if (!(fabs(a - b) < 0.001) || !same_side(g, old, n)) {
                bad++;
                PC_T_FAIL("n=%zu case=%d: flow %g vs %g", n, c, a, b);
            }
            pc_algo_maxflow_free(g);
            pc_algo_maxflow_free(old);
        }
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_source_return_survives_long_disconnected_chains(void)
{
    const size_t n = 5000;
    pc_algo_maxflow *g = NULL;
    size_t i;
    int bad = 0;
    CHECK(pc_algo_maxflow_new(n, n, &g, NULL) == PC_OK);
    pc_algo_maxflow_add_tweights(g, 0, 200.0f, 0.0f);
    pc_algo_maxflow_add_tweights(g, n - 1, 0.0f, 100.0f);
    for (i = 1; i < n - 1; i++) {
        (void)pc_algo_maxflow_add_edge(g, i - 1, i, 100.0f, 100.0f, NULL);
    }
    CHECK(run(g, 0) == 0.0);
    for (i = 0; i < n; i++) {
        bad += pc_algo_maxflow_in_source(g, i) != (i != n - 1);
    }
    CHECK_EQ_INT(bad, 0);
    CHECK(run(g, 0) == 0.0);
    CHECK(pc_algo_maxflow_add_edge(g, n - 2, n - 1, 30.0f, 30.0f, NULL) == PC_OK);
    CHECK(run(g, 0) == 30.0);
    CHECK(pc_algo_maxflow_residual_out_of_source_side(g) == 0.0f);
    pc_algo_maxflow_free(g);
}

/* ---- cancellation (segment/tests.rs) ---- */

static bool yes(void *ctx)
{
    (void)ctx;
    return true;
}

static bool no(void *ctx)
{
    (void)ctx;
    return false;
}

static float lcg(uint64_t *z)
{
    *z = *z * 6364136223846793005ull + 1442695040888963407ull;
    return (float)(*z >> 40) / (float)(1ull << 24);
}

static void test_maxflow_can_be_cancelled_and_otherwise_cuts_the_same(void)
{
    static const size_t dims[2][2] = {{120, 100}, {20, 15}};
    pc_interrupt never = {no, NULL, NULL}, always = {yes, NULL, NULL};
    size_t k, i;
    for (k = 0; k < 2; k++) {
        size_t w = dims[k][0], h = dims[k][1];
        pc_algo_maxflow *g = NULL, *plain, *asked;
        uint64_t z = 7;
        double flow, f2 = -1.0, f3 = -1.0;
        CHECK(pc_algo_maxflow_new(w * h, w * h * 2, &g, NULL) == PC_OK);
        for (i = 0; i < w * h; i++) {
            float a = lcg(&z) * 2.0f;
            pc_algo_maxflow_add_tweights(g, i, a, lcg(&z) * 2.0f);
            if (i % w + 1 < w) {
                float c = lcg(&z);
                (void)pc_algo_maxflow_add_edge(g, i, i + 1, c, lcg(&z), NULL);
            }
            if (i + w < w * h) {
                float c = lcg(&z);
                (void)pc_algo_maxflow_add_edge(g, i, i + w, c, lcg(&z), NULL);
            }
        }
        plain = clone(g);
        asked = clone(g);
        flow = run(plain, 2);
        CHECK(pc_algo_maxflow_run_with(asked, &never, &f2, NULL) == PC_OK);
        CHECK(f2 == flow);
        CHECK(same_side(plain, asked, w * h));
        if (w * h > 4096) {
            pc_error e;
            CHECK(pc_algo_maxflow_run_with(g, &always, &f3, &e) == PC_ERR_CANCELLED);
            CHECK(f3 == -1.0);
        }
        pc_algo_maxflow_free(g);
        pc_algo_maxflow_free(plain);
        pc_algo_maxflow_free(asked);
    }
}

/* ---- edge starts (edge_starts/tests.rs) ---- */

static const int32_t NB[4][2] = {{1, 0}, {0, 1}, {1, 1}, {-1, 1}};

static void test_merged_starts_equal_all_live_edge_sources(void)
{
    static const size_t dims[5][2] = {{1, 1}, {1, 7}, {7, 1}, {2, 3}, {4, 3}};
    size_t k, bits, free_px[16], expected[16], got[16];
    int bad = 0;
    for (k = 0; k < 5; k++) {
        size_t w = dims[k][0], h = dims[k][1];
        for (bits = 0; bits < (size_t)1 << (w * h); bits++) {
            size_t nfree = 0, ne = 0, ng = 0, i, x, y, v;
            pc_algo_edge_starts it;
            for (i = 0; i < w * h; i++) {
                if (bits & ((size_t)1 << i)) {
                    free_px[nfree++] = i;
                }
            }
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    bool live = (bits >> (y * w + x)) & 1u;
                    int d;
                    for (d = 0; d < 4 && !live; d++) {
                        int64_t xx = (int64_t)x + NB[d][0], yy = (int64_t)y + NB[d][1];
                        live = xx >= 0 && xx < (int64_t)w && yy < (int64_t)h &&
                               ((bits >> ((size_t)yy * w + (size_t)xx)) & 1u);
                    }
                    if (live) {
                        expected[ne++] = y * w + x;
                    }
                }
            }
            pc_algo_segment_edge_starts_init(&it, free_px, nfree, w);
            while (ng < 16 && pc_algo_segment_edge_starts_next(&it, &v)) {
                got[ng++] = v;
            }
            if (ng != ne || memcmp(got, expected, ne * sizeof(size_t)) != 0) {
                bad++;
                PC_T_FAIL("%zux%zu bits=%zu", w, h, bits);
            }
        }
    }
    CHECK_EQ_INT(bad, 0);
}

typedef struct wctx {
    int kind;
    size_t calls;
} wctx;

static float weight(void *ctx, float d)
{
    wctx *c = ctx;
    c->calls++;
    switch (c->kind) {
    case 0:
        return 1.0f;
    case 1:
        return 0.3f * expf(-2.0f * d);
    default:
        return 0.02f + 1.0f / (1.0f + powf(sqrtf(d) / 0.05f, 1.98f));
    }
}

static void test_every_small_constraint_mask_matches_the_previous_graph(void)
{
    static const size_t dims[4][2] = {{1, 1}, {1, 6}, {6, 1}, {2, 3}};
    static const uint8_t lab[3] = {PC_ALGO_SEGMENT_FREE, PC_ALGO_SEGMENT_HARD_FG, PC_ALGO_SEGMENT_HARD_BG};
    size_t k, code;
    int bad = 0;
    for (k = 0; k < 4; k++) {
        size_t w = dims[k][0], h = dims[k][1], n = w * h, i, x, y, total = 1;
        pc_algo_rgb_image img;
        float fg[6], bg[6];
        uint8_t fixed[6];
        bool a[6], b[6];
        CHECK(pc_algo_rgb_image_init(&img, w, h, NULL) == PC_OK);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                float *p = img.px[y * w + x];
                p[0] = (float)(x % 3) / 2.0f;
                p[1] = (float)(y % 4) / 3.0f;
                p[2] = (float)((x + y) % 5) / 4.0f;
            }
        }
        for (i = 0; i < n; i++) {
            fg[i] = (float)(i % 3) * 0.1f;
            bg[i] = (float)(i % 4) * 0.15f;
            total *= 3;
        }
        for (code = 0; code < total; code++) {
            size_t state = code;
            int kind;
            for (i = 0; i < n; i++) {
                fixed[i] = lab[state % 3];
                state /= 3;
            }
            for (kind = 0; kind < 3; kind++) {
                wctx c = {kind, 0};
                pc_status s1 =
                    pc_algo_segment_grid_cut_impl(&img, fg, bg, fixed, weight, &c, NULL, true, a, NULL);
                pc_status s2 =
                    pc_algo_segment_grid_cut_impl(&img, fg, bg, fixed, weight, &c, NULL, false, b, NULL);
                if (s1 != PC_OK || s2 != PC_OK || memcmp(a, b, n * sizeof(bool)) != 0) {
                    bad++;
                    PC_T_FAIL("%zux%zu constraints=%zu weight=%d", w, h, code, kind);
                }
            }
        }
        pc_algo_rgb_image_fini(&img);
    }
    CHECK_EQ_INT(bad, 0);
}

static void test_sparse_graph_keeps_exact_masks_and_avoids_irrelevant_weights(void)
{
    const size_t w = 101, h = 93, n = w * h;
    pc_algo_rgb_image img;
    float *cost = malloc(n * sizeof(float));
    uint8_t *fixed = malloc(n);
    bool *a = malloc(n), *b = malloc(n);
    size_t i, x, y;
    int kind;
    CHECK(pc_algo_rgb_image_init(&img, w, h, NULL) == PC_OK);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float v = (float)((x * 13 + y * 7) % 257) / 256.0f;
            img.px[y * w + x][0] = img.px[y * w + x][1] = img.px[y * w + x][2] = v;
        }
    }
    for (i = 0; i < n; i++) {
        cost[i] = (float)(i % 11) * 0.01f;
    }
    for (kind = 0; kind < 4; kind++) {
        wctx before = {1, 0}, after = {1, 0};
        for (i = 0; i < n; i++) {
            bool f = (kind == 0 && i % 503 == 0) || (kind == 1 && i % w >= 48 && i % w <= 52) ||
                     (kind == 2 && i / w == 0) || (kind == 3 && i % w == w - 1);
            fixed[i] = f            ? PC_ALGO_SEGMENT_FREE
                       : i % w < 50 ? PC_ALGO_SEGMENT_HARD_FG
                                    : PC_ALGO_SEGMENT_HARD_BG;
        }
        CHECK(pc_algo_segment_grid_cut_impl(&img, cost, cost, fixed, weight, &before, NULL, false, a, NULL) ==
              PC_OK);
        CHECK(pc_algo_segment_grid_cut_impl(&img, cost, cost, fixed, weight, &after, NULL, true, b, NULL) ==
              PC_OK);
        CHECK(memcmp(a, b, n) == 0);
        if (!(after.calls * 5 < before.calls)) {
            PC_T_FAIL("kind=%d counts %zu -> %zu", kind, before.calls, after.calls);
        }
    }
    pc_algo_rgb_image_fini(&img);
    free(cost);
    free(fixed);
    free(a);
    free(b);
}

/* The reference returns an empty cut for impossible dimensions and short inputs; the C port
 * reports PC_ERR_LIMIT / PC_ERR_INVALID (a C buffer has no length to be short: missing ones are
 * NULL) and never touches the output. */
static void test_malformed_grid_dimensions_and_short_inputs_return_empty(void)
{
    pc_algo_rgb_image img = {SIZE_MAX, 2, NULL};
    float cost[4] = {0};
    uint8_t fixed[4] = {0};
    bool out[4] = {true, true, true, true};
    wctx c = {0, 0};
    CHECK(pc_algo_segment_grid_cut(&img, NULL, NULL, NULL, 1.0f, 1.0f, out, NULL) == PC_ERR_LIMIT);
    img.w = (size_t)INT32_MAX + 1;
    img.h = 0;
    CHECK(pc_algo_segment_grid_cut(&img, NULL, NULL, NULL, 1.0f, 1.0f, out, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_rgb_image_init(&img, 2, 2, NULL) == PC_OK);
    CHECK(pc_algo_segment_grid_cut_with(&img, NULL, cost, fixed, weight, &c, NULL, out, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_segment_grid_cut_with(&img, cost, NULL, fixed, weight, &c, NULL, out, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_algo_segment_grid_cut_with(&img, cost, cost, NULL, weight, &c, NULL, out, NULL) ==
          PC_ERR_INVALID);
    CHECK(out[0] && out[1] && out[2] && out[3]);
    pc_algo_rgb_image_fini(&img);
}

int main(int argc, char **argv)
{
    RUN(test_preflow_matches_bk_on_integer_graphs_and_returns_unused_source);
    RUN(test_fractional_flows_match_brute_force_minimum_cuts);
    RUN(test_float_capacity_graphs_match_bk);
    RUN(test_source_return_survives_long_disconnected_chains);
    RUN(test_maxflow_can_be_cancelled_and_otherwise_cuts_the_same);
    RUN(test_merged_starts_equal_all_live_edge_sources);
    RUN(test_every_small_constraint_mask_matches_the_previous_graph);
    RUN(test_sparse_graph_keeps_exact_masks_and_avoids_irrelevant_weights);
    RUN(test_malformed_grid_dimensions_and_short_inputs_return_empty);
    return pc_t_finish();
}
