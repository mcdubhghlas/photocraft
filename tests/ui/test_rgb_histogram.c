/* Tests for src/ui/rgb_histogram.c — port of crates/ui-egui/src/rgb_histogram.rs tests.
 * Verifies the smoothing/scaling curve core and the crossing-split ribbon geometry: shared scale,
 * order-independent overlap colours for every theme, edge-spike and quantization behaviour, empty
 * rectangles, and robustness to NULL/degenerate input. */
#include "../harness.h"

#include <math.h>

#include "algo/histogram.h"
#include "ui/rgb_histogram.h"
#include "ui/theme.h"

static int col_eq(pc_ui_color a, pc_ui_color b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static float tri_area(const float a[2], const float b[2], const float c[2])
{
    return fabsf((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])) * 0.5f;
}

/* Barycentric sign test against one triangle (matches the reference color_at). */
static int in_tri(const float a[2], const float b[2], const float c[2], float px, float py)
{
    float cr = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (fabsf(cr) < 1e-5f) {
        return 0;
    }
    float pa[2] = {px, py};
    float s0 = (b[0] - a[0]) * (pa[1] - a[1]) - (b[1] - a[1]) * (pa[0] - a[0]);
    float s1 = (c[0] - b[0]) * (pa[1] - b[1]) - (c[1] - b[1]) * (pa[0] - b[0]);
    float s2 = (a[0] - c[0]) * (pa[1] - c[1]) - (a[1] - c[1]) * (pa[0] - c[0]);
    int all_nonneg = s0 >= -1e-5f && s1 >= -1e-5f && s2 >= -1e-5f;
    int all_nonpos = s0 <= 1e-5f && s1 <= 1e-5f && s2 <= 1e-5f;
    return all_nonneg || all_nonpos;
}

/* First quad (in draw order) whose two triangles contain the point. */
static int color_at(const pc_ui_hist_mesh *m, float px, float py, pc_ui_color *out)
{
    for (size_t i = 0; i < m->count; i++) {
        const pc_ui_hist_quad *q = &m->quads[i];
        if (in_tri(q->p[0], q->p[1], q->p[2], px, py) || in_tri(q->p[0], q->p[2], q->p[3], px, py)) {
            *out = q->color;
            return 1;
        }
    }
    return 0;
}

static int covers(const pc_ui_hist_mesh *m, float px, float py)
{
    pc_ui_color c;
    return color_at(m, px, py, &c);
}

static void set_all(pc_algo_rgb_histogram *h, uint64_t r, uint64_t g, uint64_t b)
{
    memset(h, 0, sizeof *h);
    for (int i = 0; i < PC_UI_HIST_BINS; i++) {
        h->channels[0][i] = r;
        h->channels[1][i] = g;
        h->channels[2][i] = b;
    }
}

static void test_shared_scale_and_overlaps_every_theme(void)
{
    pc_algo_rgb_histogram h;
    set_all(&h, 3, 2, 1);
    pc_ui_hist_mesh m;
    pc_ui_hist_mesh_init(&m);
    for (size_t k = 0; k < PC_UI_THEME_COUNT; k++) {
        pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_ALL[k]);
        pc_ui_rgb_histogram_build(&m, 0.0f, 0.0f, 256.0f, 110.0f, &h, &t);
        struct {
            float y;
            uint8_t mask;
        } cases[3] = {{100.0f, 7}, {60.0f, 3}, {20.0f, 1}};
        for (int ci = 0; ci < 3; ci++) {
            pc_ui_color got;
            int found = color_at(&m, 128.7f, cases[ci].y, &got);
            CHECK(found);
            CHECK(found && col_eq(got, pc_ui_tokens_histogram_fill(&t, cases[ci].mask)));
        }
        /* every vertex stays inside the rect */
        int inside = 1;
        for (size_t i = 0; i < m.count; i++) {
            for (int v = 0; v < 4; v++) {
                float x = m.quads[i].p[v][0];
                float y = m.quads[i].p[v][1];
                if (x < -0.01f || x > 256.01f || y < -0.01f || y > 110.01f) {
                    inside = 0;
                }
            }
        }
        CHECK(inside);
        CHECK(!m.overflow);
    }
    pc_ui_hist_mesh_free(&m);
}

static void test_order_independent_overlaps(void)
{
    /* Blocks of each mask 1..7 placed along the axis (mirrors the reference). */
    pc_algo_rgb_histogram h;
    memset(&h, 0, sizeof h);
    for (int mask = 1; mask <= 7; mask++) {
        for (int c = 0; c < 3; c++) {
            uint64_t on = (mask & (1 << c)) != 0 ? 1u : 0u;
            for (int i = mask * 30; i < mask * 30 + 10; i++) {
                h.channels[c][i] = on;
            }
        }
    }
    pc_ui_hist_mesh m;
    pc_ui_hist_mesh_init(&m);
    for (size_t k = 0; k < PC_UI_THEME_COUNT; k++) {
        pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_ALL[k]);
        pc_ui_rgb_histogram_build(&m, 0.0f, 0.0f, 256.0f, 100.0f, &h, &t);
        for (int mask = 1; mask <= 7; mask++) {
            pc_ui_color got;
            float x = (float)(mask * 30 + 5) + 0.5f;
            int found = color_at(&m, x, 95.0f, &got);
            CHECK(found && col_eq(got, pc_ui_tokens_histogram_fill(&t, (uint8_t)mask)));
        }

        /* Direct span() with crossing channels; colours must be order-independent. */
        pc_ui_hist_mesh s;
        pc_ui_hist_mesh_init(&s);
        float a[3] = {1.0f, 0.25f, 0.75f};
        float b[3] = {0.25f, 1.0f, 0.75f};
        float xr[2] = {0.0f, 256.0f};
        pc_ui_rgb_histogram_span(&s, 0.0f, 0.0f, 256.0f, 100.0f, xr, a, b, &t);
        struct {
            float x, y;
            uint8_t mask;
        } pts[5] = {{25.6f, 90.0f, 7}, {25.6f, 50.0f, 5}, {25.6f, 10.0f, 1},
                    {230.4f, 50.0f, 6}, {230.4f, 10.0f, 2}};
        for (int pi = 0; pi < 5; pi++) {
            pc_ui_color got;
            int found = color_at(&s, pts[pi].x, pts[pi].y, &got);
            CHECK(found && col_eq(got, pc_ui_tokens_histogram_fill(&t, pts[pi].mask)));
        }
        /* Total filled area is the integral of max-channel over the span: 5/6 of the box. */
        float area = 0.0f;
        for (size_t i = 0; i < s.count; i++) {
            const pc_ui_hist_quad *q = &s.quads[i];
            area += tri_area(q->p[0], q->p[1], q->p[2]);
            area += tri_area(q->p[0], q->p[2], q->p[3]);
        }
        CHECK(fabsf(area - 256.0f * 100.0f * (5.0f / 6.0f)) < 0.5f);
        pc_ui_hist_mesh_free(&s);
    }
    pc_ui_hist_mesh_free(&m);
}

static void test_edge_spikes(void)
{
    pc_algo_rgb_histogram h;
    set_all(&h, 10, 10, 10);
    h.channels[0][0] = 1000000;
    h.channels[1][255] = 1000000;
    float curves[3][PC_UI_HIST_BINS];
    pc_ui_rgb_histogram_curves(&h, curves);
    CHECK(curves[0][0] == 1.0f);
    CHECK(curves[1][255] == 1.0f);
    CHECK(fabsf(curves[0][1] - 1.0f / 1.1f) < 0.0001f);
    CHECK(fabsf(curves[1][254] - 1.0f / 1.1f) < 0.0001f);

    pc_algo_rgb_histogram h2;
    memset(&h2, 0, sizeof h2);
    h2.channels[0][255] = 100;
    pc_ui_rgb_histogram_curves(&h2, curves);
    CHECK(curves[0][255] > 0.9f);
    CHECK(curves[0][254] == 0.0f);
}

static void test_empty_and_bad_rects(void)
{
    pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_ALL[0]);
    pc_algo_rgb_histogram h;
    memset(&h, 0, sizeof h);
    pc_ui_hist_mesh m;
    pc_ui_hist_mesh_init(&m);
    pc_ui_rgb_histogram_build(&m, 0.0f, 0.0f, 256.0f, 100.0f, &h, &t);
    CHECK_EQ_UINT(m.count, 0u); /* flat zero histogram draws nothing */
    pc_ui_rgb_histogram_build(&m, 10.0f, 10.0f, 10.0f, 5.0f, &h, &t); /* non-positive */
    CHECK_EQ_UINT(m.count, 0u);
    float inf = INFINITY;
    pc_ui_rgb_histogram_build(&m, 0.0f, 0.0f, inf, inf, &h, &t);
    CHECK_EQ_UINT(m.count, 0u);
    pc_ui_hist_mesh_free(&m);
}

static void test_periodic_quantization(void)
{
    pc_algo_rgb_histogram h;
    memset(&h, 0, sizeof h);
    for (int c = 0; c < 3; c++) {
        for (int i = 30; i < 220; i += 3) {
            h.channels[c][i] = 100;
        }
    }
    float curves[3][PC_UI_HIST_BINS];
    pc_ui_rgb_histogram_curves(&h, curves);
    float low = INFINITY, high = 0.0f;
    for (int i = 40; i < 210; i++) {
        low = fminf(low, curves[0][i]);
        high = fmaxf(high, curves[0][i]);
    }
    CHECK(high - low < 0.2f);
    CHECK(curves[0][240] == 0.0f);
}

static void test_isolated_quantization(void)
{
    pc_algo_rgb_histogram h;
    memset(&h, 0, sizeof h);
    for (int c = 0; c < 3; c++) {
        h.channels[c][119] = 100;
        h.channels[c][121] = 100;
    }
    pc_algo_rgb_histogram original = h;
    pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_ALL[0]);
    pc_ui_hist_mesh m;
    pc_ui_hist_mesh_init(&m);
    pc_ui_rgb_histogram_build(&m, 0.0f, 0.0f, 256.0f, 100.0f, &h, &t);
    CHECK(covers(&m, 120.5f, 85.0f));  /* isolated empty bin must not cut a stripe */
    CHECK(!covers(&m, 180.5f, 85.0f)); /* a real empty tonal range stays empty */
    CHECK(memcmp(&h, &original, sizeof h) == 0); /* display filtering never mutates stats */
    pc_ui_hist_mesh_free(&m);
}

static void test_outline_and_sample_and_nulls(void)
{
    pc_algo_rgb_histogram h;
    set_all(&h, 5, 0, 0);
    float curves[3][PC_UI_HIST_BINS];
    pc_ui_rgb_histogram_curves(&h, curves);
    float pts[PC_UI_HIST_BINS + 2][2];
    const float (*cc)[PC_UI_HIST_BINS] = (const float (*)[PC_UI_HIST_BINS])curves;
    size_t n = pc_ui_rgb_histogram_outline(cc, 0, 0.0f, 0.0f, 256.0f, 100.0f, pts,
                                           PC_UI_HIST_BINS + 2);
    CHECK_EQ_UINT(n, (size_t)(PC_UI_HIST_BINS + 2));
    /* empty channel -> no outline */
    CHECK_EQ_UINT(pc_ui_rgb_histogram_outline(cc, 1, 0.0f, 0.0f, 256.0f, 100.0f, pts,
                                              PC_UI_HIST_BINS + 2),
                  0u);
    /* tone marker: pure white maps to the right edge, pure black to the left */
    float white[3] = {1.0f, 1.0f, 1.0f};
    float black[3] = {0.0f, 0.0f, 0.0f};
    CHECK(fabsf(pc_ui_rgb_histogram_sample_x(0.0f, 256.0f, white) - 256.0f) < 0.01f);
    CHECK(fabsf(pc_ui_rgb_histogram_sample_x(0.0f, 256.0f, black) - 0.0f) < 0.01f);
    float bad[3] = {INFINITY, 0.0f, 0.0f};
    CHECK(pc_ui_rgb_histogram_sample_x(0.0f, 256.0f, bad) < 0.0f);

    /* NULL robustness (never crash) */
    pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_ALL[0]);
    pc_ui_rgb_histogram_curves(NULL, curves);
    pc_ui_hist_mesh_init(NULL);
    pc_ui_hist_mesh_free(NULL);
    pc_ui_hist_mesh_clear(NULL);
    pc_ui_rgb_histogram_build(NULL, 0, 0, 1, 1, &h, &t);
    pc_ui_hist_mesh m;
    pc_ui_hist_mesh_init(&m);
    pc_ui_rgb_histogram_build(&m, 0, 0, 1, 1, NULL, &t); /* NULL histogram -> zero curves */
    CHECK_EQ_UINT(m.count, 0u);
    pc_ui_hist_mesh_free(&m);
}

/* Rule 6 render check: rasterise the ribbon the shell paints (quads -> pixels) and assert it
 * produces non-trivial coverage and the expected band colours at sampled pixels. This mirrors the
 * shell draw (src/ui/shell/rgb_histogram_draw.c), which only forwards these quads to Nuklear. */
static void test_rasterized_render_has_pixels(void)
{
    pc_algo_rgb_histogram h;
    set_all(&h, 3, 2, 1);
    pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_ALL[0]);
    pc_ui_hist_mesh m;
    pc_ui_hist_mesh_init(&m);
    const int W = 256, H = 110;
    pc_ui_rgb_histogram_build(&m, 0.0f, 0.0f, (float)W, (float)H, &h, &t);
    CHECK(m.count > 0);
    size_t covered = 0;
    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            pc_ui_color c;
            if (color_at(&m, (float)px + 0.5f, (float)py + 0.5f, &c)) {
                covered++;
            }
        }
    }
    double frac = (double)covered / (double)(W * H);
    /* R curve ~0.909 of the height fills everywhere; expect the ribbon to cover most of the box. */
    CHECK(frac > 0.70);
    CHECK(frac < 0.98);
    /* Bottom row (just above the baseline) must be the all-channel overlap colour. */
    pc_ui_color got;
    CHECK(color_at(&m, 128.5f, (float)H - 2.0f, &got) &&
          col_eq(got, pc_ui_tokens_histogram_fill(&t, 7)));
    pc_ui_hist_mesh_free(&m);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_rasterized_render_has_pixels);
    RUN(test_shared_scale_and_overlaps_every_theme);
    RUN(test_order_independent_overlaps);
    RUN(test_edge_spikes);
    RUN(test_empty_and_bad_rects);
    RUN(test_periodic_quantization);
    RUN(test_isolated_quantization);
    RUN(test_outline_and_sample_and_nulls);
    return pc_t_finish();
}
