/* Lens Correction (port of the tests in crates/algo/src/lens.rs) plus never-crash checks. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/rmath.h"
#include "harness.h"
#include "raster/pixel.h"

static void grid_surface(pc_surface *s, pc_pixel_format fmt, int32_t w, int32_t h)
{
    size_t n = pc_pixel_format_channels(fmt);
    float *row = calloc((size_t)w * n, sizeof *row);
    int32_t x, y;
    pc_surface_init(s, fmt);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float v = (x % 16 < 2 || y % 16 < 2) ? 0.9f : 0.3f, rgba[4] = {v, v * 0.8f, v * 0.6f, 1.0f};
            float px[PC_RASTER_MAX_CHANNELS];
            (void)pc_from_rgba(fmt, rgba, px);
            memcpy(row + (size_t)x * n, px, n * sizeof *px);
        }
        CHECK(pc_surface_write_region(s, pc_rect_make(0, y, w, y + 1), row, (size_t)w * n, NULL) == PC_OK);
    }
    free(row);
}

static pc_algo_lens_correction lcd(void)
{
    pc_algo_lens_correction lc;
    pc_algo_lens_correction_default(&lc);
    return lc;
}

static void test_identity_is_lossless_at_all_depths(void)
{
    pc_sample_type st[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    int k;
    for (k = 0; k < 3; k++) {
        pc_pixel_format fmt = {PC_COLOR_MODE_RGB, st[k], true};
        pc_surface s, out;
        pc_algo_lens_correction lc = lcd();
        pc_rect r = pc_rect_make(0, 0, 96, 64);
        float *a = NULL, *b = NULL, e = 0.0f;
        size_t la = 0, lb = 0, i;
        grid_surface(&s, fmt, 96, 64);
        CHECK(pc_algo_lens_correct(&s, r, &lc, &out, NULL) == PC_OK);
        CHECK(pc_surface_read_region(&s, r, &a, &la, NULL) == PC_OK);
        CHECK(pc_surface_read_region(&out, r, &b, &lb, NULL) == PC_OK);
        CHECK(la == lb);
        for (i = 0; i < la && i < lb; i++) {
            e = pc_rs_maxf(e, fabsf(a[i] - b[i]));
        }
        CHECK(e < 2.0f / 255.0f);
        free(a), free(b);
        pc_surface_fini(&s), pc_surface_fini(&out);
    }
}

static void test_distortion_round_trip_and_edges(void)
{
    pc_rect frame = pc_rect_make(0, 0, 200, 100);
    pc_algo_lens_correction lc = lcd(), neg;
    pc_algo_lens_map m;
    pc_algo_lens_sample s, c;
    pc_surface src, t, e, col;
    float p[4];
    lc.distortion = 50.0;
    pc_algo_lens_map_init(&m, &lc, frame);
    CHECK(pc_algo_lens_map_sample(&m, 100.0, 50.0, &s));
    CHECK(fabs(s.p[0] - 100.0) < 1e-9 && fabs(s.p[1] - 50.0) < 1e-9);
    CHECK(pc_algo_lens_map_sample(&m, 0.0, 0.0, &c));
    CHECK(c.p[0] > 5.0 && c.p[1] > 2.0);
    grid_surface(&src, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false}, 200, 100);
    neg = lcd();
    neg.distortion = -60.0;
    CHECK(pc_algo_lens_correct(&src, frame, &neg, &t, NULL) == PC_OK);
    CHECK(pc_surface_format(&t).alpha);
    pc_surface_read_pixel(&t, 0, 0, p);
    CHECK(p[3] < 0.01f);
    pc_surface_read_pixel(&t, 100, 50, p);
    CHECK(p[3] > 0.99f);
    neg.edge.kind = PC_ALGO_LENS_EDGE_EXTENSION;
    CHECK(pc_algo_lens_correct(&src, frame, &neg, &e, NULL) == PC_OK);
    CHECK(!pc_surface_format(&e).alpha);
    neg.edge.kind = PC_ALGO_LENS_EDGE_COLOR;
    neg.edge.color[0] = 1.0f, neg.edge.color[1] = 0.0f, neg.edge.color[2] = 0.0f, neg.edge.color[3] = 1.0f;
    CHECK(pc_algo_lens_correct(&src, frame, &neg, &col, NULL) == PC_OK);
    pc_surface_read_pixel(&col, 0, 0, p);
    CHECK(p[0] > 0.99f && p[1] < 0.01f);
    pc_surface_fini(&src), pc_surface_fini(&t), pc_surface_fini(&e), pc_surface_fini(&col);
}

static void test_vignette_ca_perspective_and_straighten(void)
{
    pc_rect frame = pc_rect_make(0, 0, 300, 200);
    pc_algo_lens_correction lc = lcd();
    pc_algo_lens_map m;
    pc_algo_lens_sample s, tl, tr, bl, br;
    double a, sc, p0[2] = {0.0, 0.0}, p1[2], p2[2] = {3.0, 100.0};
    lc.vignette_amount = 100.0;
    pc_algo_lens_map_init(&m, &lc, frame);
    CHECK(pc_algo_lens_map_sample(&m, 150.0, 100.0, &s) && fabsf(s.gain - 1.0f) < 1e-6f);
    CHECK(pc_algo_lens_map_sample(&m, 1.0, 1.0, &s) && s.gain > 2.0f);
    lc.vignette_amount = -100.0;
    pc_algo_lens_map_init(&m, &lc, frame);
    CHECK(pc_algo_lens_map_sample(&m, 1.0, 1.0, &s) && s.gain < 0.5f);
    /* Lateral CA scales red outward relative to green. */
    lc = lcd(), lc.red_cyan = -100.0;
    pc_algo_lens_map_init(&m, &lc, frame);
    CHECK(pc_algo_lens_map_sample(&m, 10.0, 10.0, &s) && s.pr[0] < s.p[0]);
    /* Vertical perspective -: the top edge samples a narrower source span. */
    lc = lcd(), lc.vertical = -50.0;
    pc_algo_lens_map_init(&m, &lc, frame);
    CHECK(pc_algo_lens_map_sample(&m, 0.0, 0.0, &tl) && pc_algo_lens_map_sample(&m, 300.0, 0.0, &tr));
    CHECK(pc_algo_lens_map_sample(&m, 0.0, 200.0, &bl) && pc_algo_lens_map_sample(&m, 300.0, 200.0, &br));
    CHECK(tr.p[0] - tl.p[0] < br.p[0] - bl.p[0]);
    /* Straighten a line tilted by 5 degrees -> -5 correction; Auto Scale hides the corners. */
    p1[0] = 100.0;
    p1[1] = 100.0 * tan(5.0 * (3.14159265358979323846 / 180.0));
    a = pc_algo_lens_straighten_angle(p0, p1);
    CHECK(fabs(a + 5.0) < 1e-9);
    CHECK(fabs(pc_algo_lens_straighten_angle(p0, p2) - 1.718) < 0.01);
    lc = lcd(), lc.angle = 5.0;
    sc = pc_algo_lens_auto_scale(&lc, frame);
    CHECK(sc > 105.0 && sc < 130.0);
    lc.scale = sc;
    pc_algo_lens_map_init(&m, &lc, frame);
    CHECK(pc_algo_lens_map_sample(&m, 0.0, 0.0, &s) && s.p[0] >= 0.0 && s.p[0] <= 300.0);
}

static void test_generic_profiles_and_cmyk(void)
{
    pc_algo_lens_profile wide, tele, mid;
    pc_pixel_format fmt = {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, false};
    const float ink[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    pc_surface s, out;
    pc_algo_lens_correction lc = lcd();
    float p[5];
    pc_algo_lens_generic_profile(16.0, &wide);
    pc_algo_lens_generic_profile(135.0, &tele);
    CHECK(wide.k1 < -0.05 && tele.k1 > 0.0 && wide.vignette[0] < tele.vignette[0]);
    pc_algo_lens_generic_profile(30.0, &mid);
    CHECK(mid.k1 < -0.025 && mid.k1 > -0.06);
    CHECK_EQ_STR(mid.name, "Generic Lens (30 mm equivalent)");
    /* CMYK vignette lightening reduces ink. */
    pc_surface_init(&s, fmt);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 40, 40), ink, 4, NULL) == PC_OK);
    lc.vignette_amount = 100.0;
    lc.edge.kind = PC_ALGO_LENS_EDGE_EXTENSION;
    CHECK(pc_algo_lens_correct(&s, pc_rect_make(0, 0, 40, 40), &lc, &out, NULL) == PC_OK);
    pc_surface_read_pixel(&out, 0, 0, p);
    CHECK(p[0] < 0.4f);
    pc_surface_read_pixel(&out, 20, 20, p);
    CHECK(fabsf(p[0] - 0.5f) < 0.02f);
    pc_surface_fini(&s), pc_surface_fini(&out);
}

static void test_hostile_parameters_never_crash(void)
{
    pc_rect frame = pc_rect_make(0, 0, 40, 30);
    double vals[5] = {NAN, INFINITY, -INFINITY, 1e300, -1e300};
    pc_surface s, out;
    pc_algo_lens_profile p;
    int k, f;
    grid_surface(&s, PC_PIXEL_FORMAT_RGBA8, 40, 30);
    for (k = 0; k < 5; k++) {
        pc_algo_lens_generic_profile(vals[k], &p);
        for (f = 0; f < 12; f++) {
            pc_algo_lens_correction lc = lcd();
            double *fields[12] = {&lc.distortion,
                                  &lc.red_cyan,
                                  &lc.blue_yellow,
                                  &lc.vignette_amount,
                                  &lc.vignette_midpoint,
                                  &lc.vertical,
                                  &lc.horizontal,
                                  &lc.angle,
                                  &lc.scale,
                                  &lc.profile.k1,
                                  &lc.profile.vignette[0],
                                  &lc.profile.ca_red};
            lc.has_profile = true;
            *fields[f] = vals[k];
            CHECK(pc_algo_lens_correct(&s, frame, &lc, &out, NULL) == PC_OK);
            pc_surface_fini(&out);
            (void)pc_algo_lens_auto_scale(&lc, frame);
        }
    }
    {
        /* scale <= 0 would grow forever in the reference */
        pc_algo_lens_correction lc = lcd();
        lc.scale = 0.0;
        lc.distortion = -100.0;
        (void)pc_algo_lens_auto_scale(&lc, frame);
        lc.scale = -5.0;
        (void)pc_algo_lens_auto_scale(&lc, frame);
    }
    {
        pc_algo_lens_correction lc = lcd();
        CHECK(pc_algo_lens_correct(&s, PC_RECT_EMPTY, &lc, &out, NULL) == PC_OK);
        CHECK(pc_rect_is_empty(pc_surface_content_bounds(&out)));
        pc_surface_fini(&out);
        CHECK(pc_algo_lens_correct(&s, pc_rect_make(1000, 1000, 1001, 1001), &lc, &out, NULL) == PC_OK);
        pc_surface_fini(&out);
    }
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_identity_is_lossless_at_all_depths);
    RUN(test_distortion_round_trip_and_edges);
    RUN(test_vignette_ca_perspective_and_straighten);
    RUN(test_generic_profiles_and_cmyk);
    RUN(test_hostile_parameters_never_crash);
    return pc_t_finish();
}
