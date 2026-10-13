/* Quick Selection: ports of the quick-selection tests of crates/algo/src/segment/tests.rs
 * (including the cases measured in Photoshop 25.4) and of segment/workflow_tests.rs.
 *
 * Not ported: dump_quick_scenes (an #[ignore]d helper writing the scenes as raw files for
 * measuring them in Photoshop), quick_selection_workflow_release_comparison and
 * quick_selection_surface_release_comparison (#[ignore]d release timings on 24-36 MP documents;
 * their optimized == previous assertions are what
 * test_quick_selection_preserves_textures_low_contrast_and_thin_features checks). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "harness.h"

/* ---- scenes and helpers ---- */

typedef float rgb[3];

/* Left: textured warm region; right: cool region; strong vertical edge at x = 60. */
static void two_regions(size_t w, size_t h, pc_algo_rgb_image *img)
{
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(5);
    size_t x, y;
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float n = 0.04f * pc_algo_segment_rng_normal(&rng), *p = img->px[y * w + x];
            float t = (sinf((float)x * 0.5f) * cosf((float)y * 0.3f)) * 0.05f;
            int k;
            if (x < 60) {
                p[0] = 0.8f + t + n;
                p[1] = 0.5f + n;
                p[2] = 0.3f + t;
            } else {
                p[0] = 0.2f + n;
                p[1] = 0.35f + t;
                p[2] = 0.6f + n;
            }
            for (k = 0; k < 3; k++) {
                p[k] = pc_clampf(p[k], 0.0f, 1.0f);
            }
        }
    }
}

/* A textured disc on a noisy background (segment/tests.rs disc_scene); returns the truth mask. */
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

static pc_status quick(const pc_algo_rgb_image *img, const float (*pts)[2], size_t n, float size,
                       pc_algo_region *out)
{
    pc_algo_image_sampler holder = {img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&holder);
    return pc_algo_quick_select(&s, pc_rect_make(0, 0, (int32_t)img->w, (int32_t)img->h), pts, n, size, out,
                                NULL);
}

#define QS_RED {200.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f}
#define QS_BLUE {60.0f / 255.0f, 60.0f / 255.0f, 200.0f / 255.0f}

/* A w x h image of bg with fg over [x0, x1) x [y0, y1). */
static void qs_scene(size_t w, size_t h, const float bg[3], const float fg[3], size_t x0, size_t y0,
                     size_t x1, size_t y1, pc_algo_rgb_image *img)
{
    size_t x, y;
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            memcpy(img->px[y * w + x], x >= x0 && x < x1 && y >= y0 && y < y1 ? fg : bg, sizeof(rgb));
        }
    }
}

/* A centred side x side square of fg on bg in a w x w image. */
static void qs_square(size_t w, size_t side, const float fg[3], const float bg[3], pc_algo_rgb_image *img)
{
    size_t x0 = (w - side) / 2;
    qs_scene(w, w, bg, fg, x0, x0, x0 + side, x0 + side, img);
}

/* Quick Selection on img (which it frees) that must give a selection. */
static void qs(pc_algo_rgb_image *img, const float (*pts)[2], size_t n, float size, pc_algo_region *r)
{
    CHECK(quick(img, pts, n, size, r) == PC_OK);
    CHECK(!pc_algo_region_is_none(r));
    pc_algo_rgb_image_fini(img);
}

static size_t count(const pc_algo_region *r)
{
    size_t i, c = 0;
    for (i = 0; i < r->len; i++) {
        c += r->mask[i] >= 128;
    }
    return c;
}

/* Selected pixels inside and outside [x0, x1) x [y0, y1). */
static void inside_outside(const pc_algo_region *r, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                           size_t *in, size_t *out)
{
    int32_t x, y;
    *in = *out = 0;
    for (y = r->bbox.y0; y < r->bbox.y1; y++) {
        for (x = r->bbox.x0; x < r->bbox.x1; x++) {
            if (pc_algo_region_at(r, x, y) >= 0.5f) {
                if (x >= x0 && x < x1 && y >= y0 && y < y1) {
                    ++*in;
                } else {
                    ++*out;
                }
            }
        }
    }
}

/* ---- segment/tests.rs ---- */

static void test_quick_select_with_points_far_off_canvas_selects_nothing(void)
{
    /* A cast saturates to i32::MAX; adding one to it once overflowed. */
    const float pts[3][1][2] = {{{3e9f, 0.0f}}, {{0.0f, 3e9f}}, {{3.40282347e38f, 3.40282347e38f}}};
    pc_algo_rgb_image img;
    int k;
    two_regions(140, 100, &img);
    for (k = 0; k < 3; k++) {
        pc_algo_region r;
        CHECK(quick(&img, pts[k], 1, 8.0f, &r) == PC_OK);
        CHECK(pc_algo_region_is_none(&r));
        pc_algo_region_fini(&r);
    }
    pc_algo_rgb_image_fini(&img);
}

static void test_quick_select_grows_without_leaking(void)
{
    const float pts[2][2] = {{20.0f, 50.0f}, {35.0f, 50.0f}};
    pc_algo_rgb_image img;
    pc_algo_region r;
    int32_t x, y;
    size_t inside = 0, leaks = 0;
    two_regions(140, 100, &img);
    qs(&img, pts, 2, 8.0f, &r);
    for (y = 0; y < 100; y++) {
        for (x = 0; x < 140; x++) {
            float v = pc_algo_region_at(&r, x, y);
            leaks += x >= 62 && v >= 0.5f;
            inside += x < 58 && v >= 0.5f;
        }
    }
    CHECK_EQ_UINT(leaks, 0);
    /* Grew over (nearly) the whole left region, far beyond the stroke. */
    CHECK((float)inside > 0.97f * (58.0f * 100.0f));
    pc_algo_region_fini(&r);
}

static void test_quick_select_stops_at_thin_line(void)
{
    /* Same colour on both sides of a dark 2-pixel line. */
    const float pts[1][2] = {{30.0f, 40.0f}};
    pc_algo_segment_rng rng = pc_algo_segment_rng_new(9);
    pc_algo_rgb_image img;
    pc_algo_region r;
    size_t x, y, crossed = 0;
    (void)pc_algo_rgb_image_init(&img, 120, 80, NULL);
    for (y = 0; y < 80; y++) {
        for (x = 0; x < 120; x++) {
            float n = 0.03f * pc_algo_segment_rng_normal(&rng), *p = img.px[y * 120 + x];
            if (x == 60 || x == 61) {
                p[0] = p[1] = p[2] = 0.05f;
            } else {
                p[0] = 0.6f + n;
                p[1] = 0.6f + n;
                p[2] = 0.55f + n;
            }
        }
    }
    qs(&img, pts, 1, 6.0f, &r);
    CHECK(pc_algo_region_at(&r, 10, 10) >= 0.5f && pc_algo_region_at(&r, 50, 70) >= 0.5f);
    for (y = 0; y < 80; y++) {
        for (x = 63; x < 120; x++) {
            crossed += pc_algo_region_at(&r, (int32_t)x, (int32_t)y) >= 0.5f;
        }
    }
    CHECK_EQ_UINT(crossed, 0);
    pc_algo_region_fini(&r);
}

static void test_quick_select_a_drag_selects_around_itself_not_a_big_object(void)
{
    /* Photoshop 25.4 on this very scene: an 80 px drag inside a 180 px radius disc selects about
     * the stroke (5285 px), not the disc (~100,000 px). */
    const float pts[2][2] = {{260.0f, 250.0f}, {340.0f, 250.0f}};
    pc_algo_rgb_image img;
    pc_algo_region r;
    bool *truth = disc_scene(600, 500, 300.0f, 250.0f, 180.0f, 4, &img);
    size_t n;
    qs(&img, pts, 2, 30.0f, &r);
    n = count(&r);
    if (!(n >= 3000 && n < 9000)) {
        PC_T_FAIL("%zu px", n);
    }
    CHECK(pc_algo_region_at(&r, 300, 250) >= 0.5f && pc_algo_region_at(&r, 262, 250) >= 0.5f);
    CHECK(iou(&r, truth, 600, 500) < 0.2f);
    pc_algo_region_fini(&r);
    free(truth);
}

/* ---- Photoshop's measured behaviour (segment/tests.rs) ----
 * Each case was run in Photoshop 25.4 (the same image and stroke, the mouse held 1 s so its
 * solver settles); the numbers in the comments are Photoshop's. */

static void test_quick_select_works_at_full_or_exactly_half_resolution(void)
{
    static const float red[3] = QS_RED, blue[3] = QS_BLUE;
    static const uint8_t want[7] = {0, 0, 0, 64, 191, 255, 255};
    const float pts[1][2] = {{320.0f, 320.0f}};
    pc_algo_rgb_image img;
    pc_algo_region r;
    int32_t x;
    CHECK_EQ_UINT(pc_algo_quick_working_step(512, 512), 1);
    CHECK_EQ_UINT(pc_algo_quick_working_step(513, 512), 2);
    CHECK_EQ_UINT(pc_algo_quick_working_step(520, 300), 1);
    CHECK_EQ_UINT(pc_algo_quick_working_step(900, 300), 2);
    CHECK_EQ_UINT(pc_algo_quick_working_step(5000, 3500), 2);
    /* Half resolution: the mask is scaled up bilinearly, so a selected square's edge steps
     * 64, 191, 255 (Photoshop's own values on a 640 px document). */
    qs_square(640, 200, red, blue, &img);
    qs(&img, pts, 1, 30.0f, &r);
    for (x = 216; x < 223; x++) {
        CHECK_EQ_UINT(pc_f32_to_u8(roundf(pc_algo_region_at(&r, x, 320) * 255.0f)), want[x - 216]);
    }
    pc_algo_region_fini(&r);
}

static void test_quick_select_click_in_a_flat_area_selects_about_the_brush(void)
{
    /* Photoshop: 1025-1120 px for a 30 px brush. */
    static const float red[3] = QS_RED;
    const float pts[1][2] = {{200.0f, 200.0f}};
    pc_algo_rgb_image img;
    pc_algo_region r;
    size_t n;
    qs_scene(400, 400, red, red, 0, 0, 0, 0, &img);
    qs(&img, pts, 1, 30.0f, &r);
    n = count(&r);
    if (!(n >= 700 && n < 1600)) {
        PC_T_FAIL("%zu px", n);
    }
    pc_algo_region_fini(&r);
}

static void test_quick_select_click_takes_an_object_only_while_it_is_small_enough(void)
{
    static const float red[3] = QS_RED, blue[3] = QS_BLUE;
    static const float lo[3] = {125.0f / 255.0f, 110.0f / 255.0f, 110.0f / 255.0f};
    static const float hi[3] = {110.0f / 255.0f, 110.0f / 255.0f, 125.0f / 255.0f};
    const float pts[1][2] = {{250.0f, 250.0f}};
    pc_algo_rgb_image img;
    pc_algo_region r;
    size_t i, o, n;
    /* 30 px brush, red on blue: Photoshop selects a 220 px square whole, not a 240 px one. */
    qs_square(500, 220, red, blue, &img);
    qs(&img, pts, 1, 30.0f, &r);
    inside_outside(&r, 140, 140, 360, 360, &i, &o);
    if (!(i >= 220 * 220 - 10 && o == 0)) {
        PC_T_FAIL("220: %zu in, %zu out", i, o);
    }
    pc_algo_region_fini(&r);
    qs_square(500, 240, red, blue, &img);
    qs(&img, pts, 1, 30.0f, &r);
    n = count(&r);
    if (!(n < 2000)) {
        PC_T_FAIL("240: %zu px", n);
    }
    pc_algo_region_fini(&r);
    /* A weak edge (15 levels) holds only a small square: 94 px yes, 120 px no. */
    qs_square(500, 94, lo, hi, &img);
    qs(&img, pts, 1, 30.0f, &r);
    inside_outside(&r, 203, 203, 297, 297, &i, &o);
    if (!(i >= 94 * 94 - 10 && o == 0)) {
        PC_T_FAIL("94: %zu in, %zu out", i, o);
    }
    pc_algo_region_fini(&r);
    qs_square(500, 120, lo, hi, &img);
    qs(&img, pts, 1, 30.0f, &r);
    n = count(&r);
    if (!(n < 2000)) {
        PC_T_FAIL("120: %zu px", n);
    }
    pc_algo_region_fini(&r);
}

static void test_quick_select_takes_a_narrow_bar_end_to_end_but_only_the_end_near_it(void)
{
    /* An 80 x 400 bar: a click in the middle selects all of it; a click 30 px from its end only
     * the end (Photoshop: 3762 px, cut just above the brush). */
    static const float red[3] = QS_RED, blue[3] = QS_BLUE;
    const float mid[1][2] = {{140.0f, 250.0f}}, end[1][2] = {{140.0f, 420.0f}};
    pc_algo_rgb_image img;
    pc_algo_region r;
    size_t i, o;
    qs_scene(500, 500, blue, red, 100, 50, 180, 450, &img);
    qs(&img, mid, 1, 30.0f, &r);
    inside_outside(&r, 100, 50, 180, 450, &i, &o);
    if (!(i >= 80 * 400 - 10 && o == 0)) {
        PC_T_FAIL("middle: %zu in, %zu out", i, o);
    }
    pc_algo_region_fini(&r);
    qs_scene(500, 500, blue, red, 100, 50, 180, 450, &img);
    qs(&img, end, 1, 30.0f, &r);
    inside_outside(&r, 100, 380, 180, 450, &i, &o);
    if (!(i > 2500 && o == 0)) {
        PC_T_FAIL("near the end: %zu in the end, %zu elsewhere", i, o);
    }
    pc_algo_region_fini(&r);
}

static void test_quick_select_a_short_drag_takes_what_a_click_does_not(void)
{
    /* A 300 px square: a click selects about the brush; a 60 px drag the whole square. */
    static const float red[3] = QS_RED, blue[3] = QS_BLUE;
    const float click[1][2] = {{250.0f, 250.0f}}, drag[2][2] = {{220.0f, 250.0f}, {280.0f, 250.0f}};
    pc_algo_rgb_image img;
    pc_algo_region r;
    size_t i, o;
    qs_square(500, 300, red, blue, &img);
    qs(&img, click, 1, 30.0f, &r);
    CHECK(count(&r) < 2000);
    pc_algo_region_fini(&r);
    qs_square(500, 300, red, blue, &img);
    qs(&img, drag, 2, 30.0f, &r);
    inside_outside(&r, 100, 100, 400, 400, &i, &o);
    if (!(i >= 300 * 300 - 10 && o == 0)) {
        PC_T_FAIL("%zu in, %zu out", i, o);
    }
    pc_algo_region_fini(&r);
}

static void test_quick_select_on_a_large_document_grows_its_window_to_the_object(void)
{
    /* A 1000 x 600 document (half resolution) with an 80 x 960 bar: Photoshop selects all of it
     * from a click in the middle (76800 px). The mouse on pixel (500, 300): its centre. */
    static const float red[3] = QS_RED, blue[3] = QS_BLUE;
    const float pts[1][2] = {{500.5f, 300.5f}};
    pc_algo_rgb_image img;
    pc_algo_region r;
    size_t i, o;
    qs_scene(1000, 600, blue, red, 20, 260, 980, 340, &img);
    qs(&img, pts, 1, 30.0f, &r);
    inside_outside(&r, 20, 260, 980, 340, &i, &o);
    if (!(i >= 960 * 80 * 98 / 100 && o < 2000)) {
        PC_T_FAIL("%zu in, %zu out", i, o);
    }
    pc_algo_region_fini(&r);
}

/* ---- workflow_tests.rs ---- */

static void test_quick_selection_preserves_textures_low_contrast_and_thin_features(void)
{
    static const float strokes[3][2][2] = {
        {{180.0f, 140.0f}}, {{90.0f, 140.0f}, {270.0f, 140.0f}}, {{1.0f, 1.0f}, {160.0f, 130.0f}}};
    static const size_t nstroke[3] = {1, 2, 2};
    static const float brush[3] = {12.0f, 40.0f, 64.0f};
    static const size_t direct[2] = {10000, 300000};
    const size_t w = 360, h = 280;
    const pc_rect canvas = pc_rect_make(0, 0, (int32_t)w, (int32_t)h);
    int kind, bad = 0;
    for (kind = 0; kind < 6; kind++) {
        pc_algo_rgb_image img;
        pc_algo_image_sampler holder = {&img, 0, 0};
        pc_algo_sampler s = pc_algo_image_sampler_make(&holder);
        size_t x, y, st, d;
        (void)pc_algo_rgb_image_init(&img, w, h, NULL);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                float fx = (float)x, fy = (float)y, *p = img.px[y * w + x];
                bool inside = kind == 2   ? hypotf(fx - 180.0f, fy - 140.0f) < 72.0f
                              : kind == 3 ? y > 130 && y < 145
                                          : x > 60 && x < 300 && y > 60 && y < 220;
                float t = (float)((x * 23 + y * 19) % 101) / 100.0f;
                switch (kind) {
                case 0:
                    p[0] = inside ? 0.6f + t * 0.1f : 0.4f;
                    p[1] = inside ? 0.3f : 0.3f + t * 0.1f;
                    p[2] = 0.2f;
                    break;
                case 1:
                    p[0] = 0.4f + fx / 1800.0f;
                    p[1] = 0.3f + fy / 1400.0f;
                    p[2] = t * 0.02f;
                    break;
                case 4:
                    p[0] = t;
                    p[1] = (float)((x * 7 + y * 31) % 79) / 78.0f;
                    p[2] = 0.4f;
                    break;
                case 5:
                    p[0] = p[1] = p[2] = 0.4f;
                    break;
                default:
                    p[0] = inside ? 0.8f + t * 0.1f : 0.2f;
                    p[1] = inside ? 0.3f : 0.3f + t * 0.1f;
                    p[2] = inside ? 0.2f : 0.8f;
                    break;
                }
            }
        }
        for (st = 0; st < 3; st++) {
            for (d = 0; d < 2; d++) {
                pc_algo_quick_tuning t = pc_algo_quick_tuning_default();
                pc_algo_region a, b;
                t.direct_px = direct[d];
                CHECK(pc_algo_quick_select_using(&s, canvas, strokes[st], nstroke[st], brush[st], &t, false,
                                                 &a, NULL) == PC_OK);
                CHECK(pc_algo_quick_select_using(&s, canvas, strokes[st], nstroke[st], brush[st], &t, true,
                                                 &b, NULL) == PC_OK);
                if (!pc_rect_eq(a.bbox, b.bbox) || a.len != b.len ||
                    (a.len > 0 && memcmp(a.mask, b.mask, a.len) != 0)) {
                    bad++;
                    PC_T_FAIL("kind=%d brush=%g direct=%zu", kind, (double)brush[st], direct[d]);
                }
                pc_algo_region_fini(&a);
                pc_algo_region_fini(&b);
            }
        }
        pc_algo_rgb_image_fini(&img);
    }
    CHECK_EQ_INT(bad, 0);
}

int main(int argc, char **argv)
{
    RUN(test_quick_select_with_points_far_off_canvas_selects_nothing);
    RUN(test_quick_select_grows_without_leaking);
    RUN(test_quick_select_stops_at_thin_line);
    RUN(test_quick_select_a_drag_selects_around_itself_not_a_big_object);
    RUN(test_quick_select_works_at_full_or_exactly_half_resolution);
    RUN(test_quick_select_click_in_a_flat_area_selects_about_the_brush);
    RUN(test_quick_select_click_takes_an_object_only_while_it_is_small_enough);
    RUN(test_quick_select_takes_a_narrow_bar_end_to_end_but_only_the_end_near_it);
    RUN(test_quick_select_a_short_drag_takes_what_a_click_does_not);
    RUN(test_quick_select_on_a_large_document_grows_its_window_to_the_object);
    RUN(test_quick_selection_preserves_textures_low_contrast_and_thin_features);
    return pc_t_finish();
}
