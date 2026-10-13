/* L6 filters (distort, stylize, pixelate, render, Blur Gallery, other, video): ports of the
 * #[test]s in crates/algo/src/{tests.rs, tests_ext.rs, tests_mosaic.rs, other.rs, distort2.rs,
 * other2.rs} that cover them, plus never-crash checks. Parameters are built from the reference's
 * JSON so the cases read like the Rust ones. */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "raster/pixel.h"
#include "harness.h"

/* Long JSON cases are split across lines inside the case arrays. */
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wstring-concatenation"
#endif

static const pc_rect R = {0, 0, 40, 30};

static pc_pixel_format rgba(pc_sample_type s)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, s, true};
    return f;
}

/* tests.rs pattern(): a deterministic RGBA image over r. */
static void pattern(pc_surface *s, pc_sample_type st, pc_rect r)
{
    int32_t x, y;
    pc_surface_init(s, rgba(st));
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            float px[4] = {(float)((x * 7 + y * 3) % 64) / 63.0f, (float)((x * x + y) % 50) / 49.0f,
                           (float)(y % 9) / 8.0f, 1.0f};
            CHECK(pc_surface_write_pixel(s, x, y, px, 4, NULL) == PC_OK);
        }
    }
}

static void flat(pc_surface *s, pc_sample_type st, pc_rect r, float a, float b, float c, float d)
{
    const float px[4] = {a, b, c, d};
    pc_surface_init(s, rgba(st));
    CHECK(pc_surface_fill_rect(s, r, px, 4, NULL) == PC_OK);
}

static pc_algo_filter F(const char *json)
{
    pc_algo_filter f = {NULL, NULL};
    pc_error err;
    if (pc_algo_filter_from_json_str(json, strlen(json), &f, &err) != PC_OK) {
        PC_T_FAIL("parse %s: %s", json, err.msg);
    }
    return f;
}

/* output_area + apply over R. */
static pc_surface run(const pc_surface *s, const pc_algo_filter *f)
{
    pc_surface o;
    pc_rect area = pc_algo_output_area(f, pc_surface_content_bounds(s), R, NULL);
    if (pc_algo_apply(s, f, area, R, NULL, &o, NULL) != PC_OK) {
        PC_T_FAIL("apply %s failed", f->def != NULL ? f->def->tag : "?");
        pc_surface_init(&o, pc_surface_format(s));
    }
    return o;
}

static pc_surface runj(const pc_surface *s, const char *json)
{
    pc_algo_filter f = F(json);
    pc_surface o = run(s, &f);
    pc_algo_filter_fini(&f);
    return o;
}

static void px(const pc_surface *s, int32_t x, int32_t y, float out[4])
{
    pc_surface_read_pixel(s, x, y, out);
}

static float *region(const pc_surface *s, pc_rect r, size_t *len)
{
    float *v = NULL;
    CHECK(pc_surface_read_region(s, r, &v, len, NULL) == PC_OK);
    return v;
}

static float max_diff(const float *a, const float *b, size_t n)
{
    float m = 0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        float d = fabsf(a[i] - b[i]);
        m = d > m ? d : m;
    }
    return m;
}

static bool same_region(const pc_surface *a, const pc_surface *b, pc_rect r)
{
    size_t la, lb;
    float *x = region(a, r, &la), *y = region(b, r, &lb);
    bool eq = la == lb && (la == 0 || memcmp(x, y, la * sizeof(float)) == 0);
    free(x), free(y);
    return eq;
}

/* ---- tests.rs ---- */

static void test_minimum_maximum_spread(void)
{
    pc_surface s, mx, mn;
    float p[4];
    const float white[4] = {1, 1, 1, 1};
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0.5f, 0.5f, 0.5f, 1.0f);
    CHECK(pc_surface_write_pixel(&s, 10, 10, white, 4, NULL) == PC_OK);
    mx = runj(&s, "{\"filter\":\"maximum\",\"radius\":1,\"preserve\":\"squareness\"}");
    px(&mx, 11, 11, p);
    CHECK(fabsf(p[0] - 1.0f) < 1e-6f);
    px(&mx, 12, 10, p);
    CHECK(fabsf(p[0] - 0.5f) < 1e-6f);
    mn = runj(&s, "{\"filter\":\"minimum\",\"radius\":1,\"preserve\":\"squareness\"}");
    px(&mn, 10, 10, p);
    CHECK(fabsf(p[0] - 0.5f) < 1e-6f);
    pc_surface_fini(&s), pc_surface_fini(&mx), pc_surface_fini(&mn);
}

static void test_maximum_roundness_grows_a_point_into_a_disc(void)
{
    const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const float radii[4] = {1.0f, 2.0f, 3.5f, 6.0f}, white[4] = {1, 1, 1, 1};
    size_t d, k;
    for (d = 0; d < 3; d++) {
        pc_surface s;
        flat(&s, depths[d], R, 0, 0, 0, 1);
        CHECK(pc_surface_write_pixel(&s, 20, 15, white, 4, NULL) == PC_OK);
        for (k = 0; k < 4; k++) {
            pc_algo_minmax_params pr = {radii[k], PC_ALGO_PRESERVE_ROUNDNESS},
                                  ps = {radii[k], PC_ALGO_PRESERVE_SQUARENESS};
            pc_algo_filter fr, fs;
            pc_surface round, square;
            int32_t x, y, rs = (int32_t)roundf(radii[k]);
            CHECK(pc_algo_filter_make(&pc_algo_def_maximum, &pr, &fr, NULL) == PC_OK);
            CHECK(pc_algo_filter_make(&pc_algo_def_maximum, &ps, &fs, NULL) == PC_OK);
            round = run(&s, &fr);
            square = run(&s, &fs);
            for (y = R.y0; y < R.y1; y++) {
                for (x = R.x0; x < R.x1; x++) {
                    int32_t dx = x - 20, dy = y - 15;
                    float a[4], b[4];
                    bool in_disc = (float)(dx * dx + dy * dy) <= radii[k] * radii[k];
                    px(&round, x, y, a);
                    px(&square, x, y, b);
                    CHECK((a[0] > 0.5f) == in_disc);
                    CHECK((b[0] > 0.5f) == (abs(dx) <= rs && abs(dy) <= rs));
                }
            }
            pc_surface_fini(&round), pc_surface_fini(&square);
            pc_algo_filter_fini(&fr), pc_algo_filter_fini(&fs);
        }
        pc_surface_fini(&s);
    }
    {
        /* Radius 1: the four neighbours, not the diagonals. */
        pc_surface s, r1;
        float a[4];
        flat(&s, PC_SAMPLE_TYPE_U8, R, 0, 0, 0, 1);
        CHECK(pc_surface_write_pixel(&s, 20, 15, white, 4, NULL) == PC_OK);
        r1 = runj(&s, "{\"filter\":\"maximum\",\"radius\":1,\"preserve\":\"roundness\"}");
        px(&r1, 21, 15, a);
        CHECK(a[0] == 1.0f);
        px(&r1, 21, 16, a);
        CHECK(a[0] == 0.0f);
        pc_surface_fini(&s), pc_surface_fini(&r1);
    }
}

static void test_minimum_maximum_roundness_match_brute_force(void)
{
    pc_surface s;
    const float radii[4] = {1.0f, 1.5f, 2.9f, 4.0f};
    size_t len, k;
    float *img;
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    img = region(&s, R, &len);
    for (k = 0; k < 4; k++) {
        int32_t ri = (int32_t)floorf(radii[k]), mx;
        for (mx = 0; mx < 2; mx++) {
            pc_algo_minmax_params p = {radii[k], PC_ALGO_PRESERVE_ROUNDNESS};
            pc_algo_filter f;
            pc_surface got;
            int32_t x, y, c, dx, dy;
            CHECK(pc_algo_filter_make(mx ? &pc_algo_def_maximum : &pc_algo_def_minimum, &p, &f, NULL) ==
                  PC_OK);
            got = run(&s, &f);
            for (y = R.y0 + ri; y < R.y1 - ri; y++) {
                for (x = R.x0 + ri; x < R.x1 - ri; x++) {
                    float g[4];
                    px(&got, x, y, g);
                    for (c = 0; c < 3; c++) {
                        float want = mx ? -FLT_MAX : FLT_MAX;
                        for (dy = -ri; dy <= ri; dy++) {
                            for (dx = -ri; dx <= ri; dx++) {
                                if ((float)(dx * dx + dy * dy) <= radii[k] * radii[k] &&
                                    pc_rect_contains(R, x + dx, y + dy)) {
                                    float v = img[((y + dy) * 40 + x + dx) * 4 + c];
                                    want = mx ? (v > want ? v : want) : (v < want ? v : want);
                                }
                            }
                        }
                        CHECK(fabsf(g[c] - want) < 1e-6f);
                    }
                }
            }
            pc_surface_fini(&got);
            pc_algo_filter_fini(&f);
        }
    }
    free(img);
    pc_surface_fini(&s);
}

static void test_running_extreme_windows(void)
{
    const float v[8] = {3, 1, 4, 1, 5, 9, 2, 6};
    float out[8], scratch[16];
    size_t k, i, j;
    int mx;
    for (k = 1; k <= 8; k++) {
        for (mx = 0; mx < 2; mx++) {
            size_t n = pc_algo_other_running_extreme(v, 8, k, mx != 0, scratch, out);
            CHECK_EQ_UINT(n, 8 - k + 1);
            for (i = 0; i + k <= 8; i++) {
                float want = mx ? -FLT_MAX : FLT_MAX;
                for (j = i; j < i + k; j++) {
                    want = mx ? (v[j] > want ? v[j] : want) : (v[j] < want ? v[j] : want);
                }
                CHECK(out[i] == want);
            }
        }
    }
}

/* other.rs: square_min_max_matches_the_direct_window_exactly (through the filter kernel). */
static void test_square_min_max_matches_the_direct_window_exactly(void)
{
    pc_algo_image img;
    pc_rect outs[2] = {{0, 0, 25, 21}, {-14, -3, 3, 40}};
    const int32_t rs[5] = {1, 2, 3, 7, 20};
    uint32_t s = 0x2545f491u;
    size_t i, o, k;
    pc_algo_ctx ctx = {R, PC_COLOR_MODE_RGB, true};
    CHECK(pc_algo_image_init(&img, pc_rect_make(-12, -9, 37, 30), 4, NULL) == PC_OK);
    for (i = 0; i < img.len; i++) {
        s ^= s << 13, s ^= s >> 17, s ^= s << 5;
        img.data[i] = (float)(s % 1000) / 999.0f;
    }
    for (o = 0; o < 2; o++) {
        for (k = 0; k < 5; k++) {
            int mx;
            for (mx = 0; mx < 2; mx++) {
                pc_algo_minmax_params p = {(float)rs[k], PC_ALGO_PRESERVE_SQUARENESS};
                pc_algo_filter f;
                float *d = NULL;
                size_t len = 0, n = 0;
                int32_t x, y, xx, yy;
                size_t c;
                CHECK(pc_algo_filter_make(mx ? &pc_algo_def_maximum : &pc_algo_def_minimum, &p, &f, NULL) ==
                      PC_OK);
                CHECK(pc_algo_kernel(&f, &img, outs[o], &ctx, &d, &len, NULL) == PC_OK);
                for (y = outs[o].y0; y < outs[o].y1; y++) {
                    for (x = outs[o].x0; x < outs[o].x1; x++) {
                        for (c = 0; c < 4; c++) {
                            float m = mx ? -FLT_MAX : FLT_MAX;
                            for (yy = y - rs[k]; yy <= y + rs[k]; yy++) {
                                for (xx = x - rs[k]; xx <= x + rs[k]; xx++) {
                                    float v = pc_algo_image_get(&img, xx, yy, c);
                                    m = mx ? (v > m ? v : m) : (v < m ? v : m);
                                }
                            }
                            if (d[n++] != m) {
                                PC_T_FAIL("r %d max %d at %d,%d", (int)rs[k], mx, (int)x, (int)y);
                            }
                        }
                    }
                }
                CHECK_EQ_UINT(n, len);
                free(d);
                pc_algo_filter_fini(&f);
            }
        }
    }
    pc_algo_image_fini(&img);
}

static void test_minimum_maximum_old_params_default_to_squareness(void)
{
    pc_algo_filter f = F("{\"filter\":\"minimum\",\"radius\":3}");
    const pc_algo_minmax_params *p = f.params;
    CHECK(f.def == &pc_algo_def_minimum && p->radius == 3.0f && p->preserve == PC_ALGO_PRESERVE_SQUARENESS);
    pc_algo_filter_fini(&f);
    f = F("{\"filter\":\"maximum\",\"radius\":2,\"preserve\":\"roundness\"}");
    p = f.params;
    CHECK(f.def == &pc_algo_def_maximum && p->radius == 2.0f && p->preserve == PC_ALGO_PRESERVE_ROUNDNESS);
    pc_algo_filter_fini(&f);
}

static void test_offset_wrap_repeat_transparent(void)
{
    pc_surface s, full, t, rp;
    float a[4], b[4];
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    full = runj(&s, "{\"filter\":\"offset\",\"horizontal\":40,\"vertical\":30,\"undefined\":\"wrap\"}");
    CHECK(same_region(&full, &s, R));
    t = runj(&s, "{\"filter\":\"offset\",\"horizontal\":5,\"vertical\":0,\"undefined\":\"transparent\"}");
    px(&t, 2, 3, a);
    CHECK(a[3] == 0.0f);
    px(&t, 15, 3, a);
    px(&s, 10, 3, b);
    CHECK(memcmp(a, b, sizeof a) == 0);
    rp = runj(&s, "{\"filter\":\"offset\",\"horizontal\":5,\"vertical\":0,\"undefined\":\"repeat\"}");
    px(&rp, 2, 3, a);
    px(&s, 0, 3, b);
    CHECK(memcmp(a, b, sizeof a) == 0);
    pc_surface_fini(&s), pc_surface_fini(&full), pc_surface_fini(&t), pc_surface_fini(&rp);
}

static void test_mosaic_cells_are_uniform(void)
{
    pc_surface s, m;
    float a[4], b[4], c[4];
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    m = runj(&s, "{\"filter\":\"mosaic\",\"cellSize\":5}");
    px(&m, 0, 0, a), px(&m, 4, 4, b), px(&m, 5, 4, c);
    CHECK(memcmp(a, b, sizeof a) == 0);
    CHECK(memcmp(b, c, sizeof b) != 0);
    pc_surface_fini(&s), pc_surface_fini(&m);
}

static void test_stylize_basics(void)
{
    pc_surface s, o;
    float p[4];
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0.8f, 0.3f, 0.1f, 1.0f);
    o = runj(&s, "{\"filter\":\"emboss\",\"angle\":45,\"height\":3,\"amount\":100}");
    px(&o, 20, 15, p);
    CHECK(fabsf(p[0] - 0.5f) < 1e-5f && fabsf(p[1] - 0.5f) < 1e-5f);
    pc_surface_fini(&o);
    o = runj(&s, "{\"filter\":\"findEdges\"}");
    px(&o, 20, 15, p);
    CHECK(fabsf(p[0] - 1.0f) < 1e-6f);
    pc_surface_fini(&o);
    o = runj(&s, "{\"filter\":\"solarize\"}");
    px(&o, 3, 3, p);
    CHECK(fabsf(p[0] - 0.2f) < 1e-6f && fabsf(p[1] - 0.3f) < 1e-6f);
    pc_surface_fini(&o);
    o = runj(&s, "{\"filter\":\"desaturate\"}");
    px(&o, 3, 3, p);
    CHECK(fabsf(p[0] - p[1]) < 1e-6f && fabsf(p[1] - p[2]) < 1e-6f);
    pc_surface_fini(&o);
    pc_surface_fini(&s);
}

static void test_distortions_are_identity_at_zero(void)
{
    static const char *const cases[] = {"{\"filter\":\"twirl\",\"angle\":0}",
                                        "{\"filter\":\"pinch\",\"amount\":0}",
                                        "{\"filter\":\"spherize\",\"amount\":0,\"mode\":\"normal\"}",
                                        "{\"filter\":\"ripple\",\"amount\":0,\"size\":\"large\"}"};
    pc_surface s;
    size_t i;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    for (i = 0; i < 4; i++) {
        pc_surface o = runj(&s, cases[i]);
        CHECK(same_region(&o, &s, R));
        pc_surface_fini(&o);
    }
    pc_surface_fini(&s);
}

static void test_twirl_keeps_centre_and_outside(void)
{
    pc_rect b = pc_rect_make(0, 0, 40, 40);
    pc_surface s, o;
    pc_algo_filter f = F("{\"filter\":\"twirl\",\"angle\":180}");
    float a[4], c[4];
    pattern(&s, PC_SAMPLE_TYPE_F32, b);
    CHECK(pc_algo_apply(&s, &f, b, b, NULL, &o, NULL) == PC_OK);
    px(&o, 0, 0, a), px(&s, 0, 0, c);
    CHECK(memcmp(a, c, sizeof a) == 0);
    px(&o, 20, 12, a), px(&s, 20, 12, c);
    CHECK(memcmp(a, c, sizeof a) != 0);
    pc_surface_fini(&s), pc_surface_fini(&o);
    pc_algo_filter_fini(&f);
}

static void test_other_colour_modes_work(void)
{
    static const pc_color_mode modes[3] = {PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_LAB};
    static const char *const cases[] = {"{\"filter\":\"desaturate\"}",
                                        "{\"filter\":\"emboss\",\"angle\":0,\"height\":1,\"amount\":100}",
                                        "{\"filter\":\"twirl\",\"angle\":50}"};
    size_t m, i;
    for (m = 0; m < 3; m++) {
        pc_pixel_format f = {modes[m], PC_SAMPLE_TYPE_U8, true};
        size_t n = pc_pixel_format_channels(f);
        float p[8] = {0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f, 0.4f};
        pc_surface s;
        p[n - 1] = 1.0f;
        pc_surface_init(&s, f);
        CHECK(pc_surface_fill_rect(&s, R, p, n, NULL) == PC_OK);
        for (i = 0; i < 3; i++) {
            pc_surface o = runj(&s, cases[i]);
            pc_pixel_format of = pc_surface_format(&o);
            CHECK(of.mode == f.mode && of.sample == f.sample && of.alpha == f.alpha);
            pc_surface_fini(&o);
        }
        pc_surface_fini(&s);
    }
}

static void test_half_selection_mixes_halfway(void)
{
    pc_surface s, sel, o;
    pc_pixel_format g = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, false};
    const float half = 0.5f;
    float p[4];
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0.2f, 0.4f, 0.6f, 1.0f);
    pc_surface_init_default(&sel, g, &half, 1);
    CHECK(pc_algo_apply(&s, &(pc_algo_filter){&pc_algo_def_invert, NULL}, R, R, &sel, &o, NULL) == PC_OK);
    px(&o, 5, 5, p);
    CHECK(fabsf(p[0] - 0.5f) < 1e-5f && fabsf(p[2] - 0.5f) < 1e-5f);
    pc_surface_fini(&s), pc_surface_fini(&sel), pc_surface_fini(&o);
}

static void test_halos_and_output_area(void)
{
    pc_algo_filter tw = F("{\"filter\":\"twirl\",\"angle\":1}"), inv = F("{\"filter\":\"invert\"}");
    pc_rect content = pc_rect_make(10, 10, 20, 20);
    CHECK(pc_algo_filter_halo(&tw).bounds);
    CHECK(pc_rect_eq(pc_algo_output_area(&inv, content, R, NULL), content));
    CHECK(pc_rect_eq(pc_algo_output_area(&tw, content, R, NULL), R));
    CHECK(pc_rect_is_empty(pc_algo_output_area(&inv, PC_RECT_EMPTY, R, NULL)));
    pc_algo_filter_fini(&tw), pc_algo_filter_fini(&inv);
}

/* params_serde_roundtrip / new_params_serde_roundtrip for this chunk's variants. */
static void test_params_serde_roundtrip(void)
{
    static const char *const cases[] = {
        "{\"filter\":\"minimum\",\"radius\":2.5,\"preserve\":\"roundness\"}",
        "{\"filter\":\"offset\",\"horizontal\":7,\"vertical\":-3,\"undefined\":\"wrap\"}",
        "{\"filter\":\"mosaic\",\"cellSize\":6}",
        "{\"filter\":\"emboss\",\"angle\":135,\"height\":2,\"amount\":100}",
        "{\"filter\":\"findEdges\"}",
        "{\"filter\":\"wave\",\"generators\":3,\"wavelengthMin\":5,\"wavelengthMax\":20,\"amplitudeMin\":1,"
        "\"amplitudeMax\":4,\"waveType\":\"triangle\",\"undefined\":\"repeat\",\"seed\":9}",
        "{\"filter\":\"polarCoordinates\",\"mode\":\"polarToRectangular\"}",
        "{\"filter\":\"colorHalftone\",\"maxRadius\":4,\"angles\":[108,162,90,45]}",
        "{\"filter\":\"extrude\",\"kind\":\"pyramids\",\"size\":6,\"depth\":30,\"levelBased\":true,"
        "\"solidFront\":false,"
        "\"maskIncomplete\":true,\"seed\":5}",
        "{\"filter\":\"lightingEffects\",\"lights\":[{}],\"gloss\":0,\"metallic\":0,\"exposure\":0,"
        "\"ambience\":10,"
        "\"texture\":\"luminance\",\"height\":50,\"whiteIsHigh\":true}",
        "{\"filter\":\"shear\",\"points\":[[0,0],[0.5,0.3],[1,0]],\"undefined\":\"repeat\"}",
        "{\"filter\":\"pathBlur\",\"paths\":[{\"points\":[[0.1,0.2],[0.9,0.8]],\"speed\":8,\"taper\":30}]}",
        "{\"filter\":\"custom\",\"kernel\":[0,-1,5,-1,0],\"scale\":1,\"offset\":0}",
        "{\"filter\":\"filterGallery\",\"effects\":[{\"filter\":\"glass\",\"values\":{\"distortion\":7}}]}",
        "{\"filter\":\"ntscColors\"}",
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_algo_filter f = F(cases[i]), back, copy;
        pc_buf b;
        pc_buf_init(&b);
        CHECK(pc_algo_filter_to_json_text(&f, &b, NULL) == PC_OK);
        back = F(pc_buf_cstr(&b));
        CHECK(pc_algo_filter_eq(&f, &back));
        CHECK(pc_algo_filter_clone(&f, &copy, NULL) == PC_OK);
        CHECK(pc_algo_filter_eq(&f, &copy));
        pc_buf_fini(&b);
        pc_algo_filter_fini(&f), pc_algo_filter_fini(&back), pc_algo_filter_fini(&copy);
    }
}

/* ---- tests_ext.rs ---- */

static const char *const NEW_FILTERS[] = {
    "{\"filter\":\"colorHalftone\",\"maxRadius\":4,\"angles\":[108,162,90,45]}",
    "{\"filter\":\"crystallize\",\"cellSize\":6,\"seed\":1}",
    "{\"filter\":\"facet\"}",
    "{\"filter\":\"fragment\"}",
    "{\"filter\":\"mezzotint\",\"kind\":\"mediumLines\",\"seed\":2}",
    "{\"filter\":\"mezzotint\",\"kind\":\"grainyDots\",\"seed\":2}",
    "{\"filter\":\"pointillize\",\"cellSize\":5,\"seed\":3,\"background\":[1,1,1,1]}",
    "{\"filter\":\"diffuse\",\"mode\":\"normal\",\"seed\":4}",
    "{\"filter\":\"diffuse\",\"mode\":\"anisotropic\",\"seed\":4}",
    "{\"filter\":\"extrude\",\"kind\":\"blocks\",\"size\":6,\"depth\":20,\"levelBased\":false,\"solidFront\":"
    "false,"
    "\"maskIncomplete\":false,\"seed\":5}",
    "{\"filter\":\"extrude\",\"kind\":\"pyramids\",\"size\":6,\"depth\":30,\"levelBased\":true,"
    "\"solidFront\":false,"
    "\"maskIncomplete\":true,\"seed\":5}",
    "{\"filter\":\"oilPaint\",\"stylization\":3,\"cleanliness\":4,\"scale\":1,\"bristleDetail\":5,"
    "\"lighting\":true,"
    "\"angle\":-60,\"shine\":2}",
    "{\"filter\":\"tiles\",\"count\":4,\"maxOffset\":20,\"fill\":\"background\",\"foreground\":[0,0,0,1],"
    "\"background\":[1,1,1,1],\"seed\":6}",
    "{\"filter\":\"traceContour\",\"level\":128,\"upper\":true}",
    "{\"filter\":\"wind\",\"method\":\"blast\",\"fromRight\":false,\"seed\":7}",
    "{\"filter\":\"wind\",\"method\":\"stagger\",\"fromRight\":true,\"seed\":7}",
    "{\"filter\":\"shear\",\"points\":[[0,0],[0.5,0.3],[1,0]],\"undefined\":\"repeat\"}",
    "{\"filter\":\"zigZag\",\"amount\":50,\"ridges\":4,\"style\":\"pondRipples\"}",
    "{\"filter\":\"fibers\",\"variance\":16,\"strength\":4,\"seed\":8,\"foreground\":[0,0,0,1],"
    "\"background\":[1,1,1,1]}",
    "{\"filter\":\"lensFlare\",\"brightness\":100,\"centerX\":0.3,\"centerY\":0.3,\"lens\":\"zoom\"}",
    "{\"filter\":\"lightingEffects\",\"lights\":[{}],\"gloss\":0,\"metallic\":0,\"exposure\":0,\"ambience\":"
    "10,"
    "\"texture\":\"luminance\",\"height\":50,\"whiteIsHigh\":true}",
    "{\"filter\":\"tiltShift\",\"blur\":6,\"centerX\":0.5,\"centerY\":0.5,\"angle\":0,\"focus\":0.1,"
    "\"transition\":0.2}",
    "{\"filter\":\"irisBlur\",\"pins\":[{\"blur\":6}]}",
    "{\"filter\":\"fieldBlur\",\"pins\":[{\"x\":0.2,\"y\":0.2,\"blur\":0},{\"x\":0.8,\"y\":0.8,\"blur\":6}]}",
    "{\"filter\":\"spinBlur\",\"pins\":[{\"blurAngle\":30}]}",
    "{\"filter\":\"pathBlur\",\"paths\":[{\"points\":[[0.1,0.2],[0.9,0.8]],\"speed\":8,\"taper\":30}]}",
    "{\"filter\":\"custom\",\"kernel\":[0,0,0,0,0,0,0,-1,0,0,0,-1,5,-1,0,0,0,-1,0,0,0,0,0,0,0],\"scale\":1,"
    "\"offset\":0}",
    "{\"filter\":\"hsbHsl\",\"input\":\"rgb\",\"output\":\"hsl\"}",
    "{\"filter\":\"colorToAlpha\",\"color\":[1,1,1,1],\"transparencyThreshold\":0,\"opacityThreshold\":1}",
    "{\"filter\":\"colorToAlpha\",\"color\":[0.5,0.25,0.75,1],\"transparencyThreshold\":0.1,"
    "\"opacityThreshold\":0.9}",
    "{\"filter\":\"deInterlace\",\"eliminateEven\":false,\"interpolate\":true}",
    "{\"filter\":\"ntscColors\"}",
};
#define NNEW (sizeof NEW_FILTERS / sizeof NEW_FILTERS[0])

static void test_new_filters_are_tile_independent(void)
{
    pc_surface s;
    size_t i;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    for (i = 0; i < NNEW; i++) {
        pc_algo_filter f = F(NEW_FILTERS[i]);
        pc_rect area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL),
                big = pc_rect_union(area, R);
        pc_surface a, b;
        CHECK(pc_algo_apply_tiled(&s, &f, area, R, NULL, 256, NULL, &a, NULL) == PC_OK);
        CHECK(pc_algo_apply_tiled(&s, &f, area, R, NULL, 7, NULL, &b, NULL) == PC_OK);
        {
            /* Running-sum blurs may round differently by one level depending on where a window starts. */
            size_t na, nb;
            float *x = region(&a, big, &na), *y = region(&b, big, &nb);
            if (na != nb || max_diff(x, y, na) > 1.5f / 255.0f) {
                PC_T_FAIL("%s differs between tile sizes", pc_algo_filter_label(&f));
            }
            free(x), free(y);
        }
        pc_surface_fini(&a), pc_surface_fini(&b);
        pc_algo_filter_fini(&f);
    }
    pc_surface_fini(&s);
}

static void test_new_filters_change_pixels_and_respect_empty_selection(void)
{
    pc_surface s, none;
    size_t i;
    pattern(&s, PC_SAMPLE_TYPE_U16, R);
    pc_surface_init(&none, PC_PIXEL_FORMAT_GRAY8);
    for (i = 0; i < NNEW; i++) {
        pc_algo_filter f = F(NEW_FILTERS[i]);
        pc_surface o = run(&s, &f), masked;
        pc_rect area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL);
        if (pc_surface_eq(&o, &s)) {
            PC_T_FAIL("%s did nothing", pc_algo_filter_label(&f));
        }
        CHECK(pc_algo_apply(&s, &f, area, R, &none, &masked, NULL) == PC_OK);
        if (!same_region(&masked, &s, area)) {
            PC_T_FAIL("%s ignored an empty selection", pc_algo_filter_label(&f));
        }
        pc_surface_fini(&o), pc_surface_fini(&masked);
        pc_algo_filter_fini(&f);
    }
    pc_surface_fini(&s), pc_surface_fini(&none);
}

static void test_new_filters_agree_across_bit_depths(void)
{
    const pc_sample_type st[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    size_t i, k, n = 0, j;
    for (i = 0; i < NNEW; i++) {
        pc_algo_filter f = F(NEW_FILTERS[i]);
        const char *t = f.def->tag;
        bool th = strcmp(t, "facet") == 0 || strcmp(t, "oilPaint") == 0 || strcmp(t, "mezzotint") == 0 ||
                  strcmp(t, "traceContour") == 0 || strcmp(t, "colorHalftone") == 0 ||
                  strcmp(t, "diffuse") == 0 || strcmp(t, "wind") == 0 ||
                  (strcmp(t, "extrude") == 0 && strstr(NEW_FILTERS[i], "\"levelBased\":true"));
        float *outs[3];
        for (k = 0; k < 3; k++) {
            pc_surface s, o;
            pattern(&s, st[k], R);
            o = run(&s, &f);
            outs[k] = region(&o, R, &n);
            for (j = 0; j < n; j++) {
                outs[k][j] = pc_clampf(outs[k][j], 0.0f, 1.0f);
            }
            pc_surface_fini(&s), pc_surface_fini(&o);
        }
        for (k = 1; k < 3; k++) {
            float mx = max_diff(outs[0], outs[k], n), mean = 0.0f;
            for (j = 0; j < n; j++) {
                mean += fabsf(outs[0][j] - outs[k][j]);
            }
            mean /= (float)n;
            if (th ? !(mean < 0.06f) : !(mx < 0.06f && mean < 0.01f)) {
                PC_T_FAIL("%s: depth difference max %g mean %g", pc_algo_filter_label(&f), (double)mx,
                          (double)mean);
            }
        }
        for (k = 0; k < 3; k++) {
            free(outs[k]);
        }
        pc_algo_filter_fini(&f);
    }
}

static void test_new_filters_work_in_other_colour_modes(void)
{
    static const pc_color_mode modes[3] = {PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK, PC_COLOR_MODE_LAB};
    pc_surface src;
    size_t m, i, j, n;
    pattern(&src, PC_SAMPLE_TYPE_F32, R);
    for (m = 0; m < 3; m++) {
        pc_pixel_format f = {modes[m], PC_SAMPLE_TYPE_U8, true};
        pc_surface s;
        int32_t x, y;
        pc_surface_init(&s, f);
        for (y = R.y0; y < R.y1; y++) {
            for (x = R.x0; x < R.x1; x++) {
                float p[4], c4[4], q[PC_RASTER_MAX_CHANNELS];
                size_t k;
                px(&src, x, y, p);
                pc_to_rgba(pc_surface_format(&src), p, 4, c4);
                k = pc_from_rgba(f, c4, q);
                CHECK(pc_surface_write_pixel(&s, x, y, q, k, NULL) == PC_OK);
            }
        }
        for (i = 0; i < NNEW; i++) {
            pc_algo_filter fl = F(NEW_FILTERS[i]);
            pc_surface o = run(&s, &fl);
            float *v = region(&o, R, &n);
            for (j = 0; j < n; j++) {
                if (!isfinite(v[j])) {
                    PC_T_FAIL("%s mode %d not finite", pc_algo_filter_label(&fl), (int)m);
                    break;
                }
            }
            free(v);
            pc_surface_fini(&o);
            pc_algo_filter_fini(&fl);
        }
        pc_surface_fini(&s);
    }
    pc_surface_fini(&src);
}

static void test_identities_at_zero_strength(void)
{
    static const char *const cases[] = {
        "{\"filter\":\"displace\",\"horizontal\":50,\"vertical\":50,\"stretch\":true,\"undefined\":\"wrap\"}",
        "{\"filter\":\"shear\",\"points\":[[0,0],[1,0]],\"undefined\":\"wrap\"}",
        "{\"filter\":\"zigZag\",\"amount\":0,\"ridges\":5,\"style\":\"aroundCenter\"}",
        "{\"filter\":\"custom\",\"kernel\":[0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0],\"scale\":1,"
        "\"offset\":0}",
        "{\"filter\":\"hsbHsl\",\"input\":\"hsb\",\"output\":\"hsb\"}",
        "{\"filter\":\"tiltShift\",\"blur\":0,\"centerX\":0.5,\"centerY\":0.5,\"angle\":30,\"focus\":0.1,"
        "\"transition\":0.2}",
        "{\"filter\":\"irisBlur\",\"pins\":[{\"blur\":0}]}",
        "{\"filter\":\"fieldBlur\",\"pins\":[{\"blur\":0}]}",
        "{\"filter\":\"spinBlur\",\"pins\":[{\"blurAngle\":0}]}",
        "{\"filter\":\"pathBlur\",\"paths\":[{\"speed\":0}]}",
    };
    pc_surface s;
    size_t i, n;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_surface o = runj(&s, cases[i]);
        float *a = region(&o, R, &n), *b = region(&s, R, &n);
        if (max_diff(a, b, n) > 0.5f / 255.0f) {
            PC_T_FAIL("%s is not identity", cases[i]);
        }
        free(a), free(b);
        pc_surface_fini(&o);
    }
    /* Displace with a mid-grey map at zero scale. */
    {
        pc_algo_image map;
        pc_algo_displace_params p = {0.0f, 0.0f, true, PC_ALGO_UNDEF_WRAP, &map};
        pc_algo_filter f;
        pc_surface o;
        float *a, *b;
        CHECK(pc_algo_image_init(&map, pc_rect_make(0, 0, 8, 8), 4, NULL) == PC_OK);
        for (i = 0; i < map.len; i++) {
            map.data[i] = (i % 4) == 0 ? 0.9f : (i % 4 == 1 ? 0.1f : (i % 4 == 2 ? 0.0f : 1.0f));
        }
        CHECK(pc_algo_filter_make(&pc_algo_def_displace, &p, &f, NULL) == PC_OK);
        o = run(&s, &f);
        a = region(&o, R, &n), b = region(&s, R, &n);
        CHECK(max_diff(a, b, n) <= 0.5f / 255.0f);
        free(a), free(b);
        pc_surface_fini(&o);
        pc_algo_filter_fini(&f);
        pc_algo_image_fini(&map);
    }
    pc_surface_fini(&s);
}

static void test_flat_images_stay_flat(void)
{
    static const char *const cases[] = {
        "{\"filter\":\"crystallize\",\"cellSize\":6,\"seed\":1}",
        "{\"filter\":\"facet\"}",
        "{\"filter\":\"oilPaint\",\"stylization\":3,\"cleanliness\":4,\"scale\":1,\"bristleDetail\":0,"
        "\"lighting\":true,"
        "\"angle\":-60,\"shine\":2}",
        "{\"filter\":\"tiltShift\",\"blur\":5,\"centerX\":0.5,\"centerY\":0.5,\"angle\":0,\"focus\":0,"
        "\"transition\":0.1}",
        "{\"filter\":\"wind\",\"method\":\"blast\",\"fromRight\":false,\"seed\":0}",
        "{\"filter\":\"deInterlace\",\"eliminateEven\":true,\"interpolate\":true}",
    };
    const float c[4] = {0.3f, 0.55f, 0.8f, 1.0f};
    pc_surface s;
    size_t i;
    int k;
    flat(&s, PC_SAMPLE_TYPE_F32, R, c[0], c[1], c[2], c[3]);
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_surface o = runj(&s, cases[i]);
        float p[4];
        px(&o, 20, 15, p);
        for (k = 0; k < 4; k++) {
            if (!(fabsf(p[k] - c[k]) < 2e-3f)) {
                PC_T_FAIL("%s: not flat", cases[i]);
            }
        }
        pc_surface_fini(&o);
    }
    pc_surface_fini(&s);
}

static void test_mezzotint_is_binary_and_tracks_brightness(void)
{
    pc_surface s, o;
    size_t n, i;
    float *v, mean[3] = {0, 0, 0};
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0.25f, 0.5f, 0.75f, 1.0f);
    o = runj(&s, "{\"filter\":\"mezzotint\",\"kind\":\"fineDots\",\"seed\":1}");
    v = region(&o, R, &n);
    for (i = 0; i < n; i += 4) {
        size_t k;
        for (k = 0; k < 3; k++) {
            CHECK(v[i + k] == 0.0f || v[i + k] == 1.0f);
            mean[k] += v[i + k] / 1200.0f;
        }
    }
    CHECK(fabsf(mean[0] - 0.25f) < 0.06f && fabsf(mean[1] - 0.5f) < 0.06f && fabsf(mean[2] - 0.75f) < 0.06f);
    free(v);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static float mean_ch(const pc_surface *o, size_t c)
{
    size_t n, i;
    float *v = region(o, R, &n), m = 0.0f;
    for (i = 0; i < n; i += 4) {
        m += v[i + c];
    }
    free(v);
    return m / 1200.0f;
}

static void test_halftone_covers_by_ink(void)
{
    const char *j = "{\"filter\":\"colorHalftone\",\"maxRadius\":4,\"angles\":[108,162,90,45]}";
    pc_surface s, o;
    size_t n, i;
    float *v;
    flat(&s, PC_SAMPLE_TYPE_F32, R, 1, 1, 1, 1);
    o = runj(&s, j);
    v = region(&o, R, &n);
    for (i = 0; i < n; i++) {
        CHECK(fabsf(v[i] - 1.0f) < 1e-6f);
    }
    free(v);
    pc_surface_fini(&s), pc_surface_fini(&o);
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0, 0, 0, 1);
    o = runj(&s, j);
    CHECK(mean_ch(&o, 0) < 0.05f);
    pc_surface_fini(&s), pc_surface_fini(&o);
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0.5f, 0.5f, 0.5f, 1);
    o = runj(&s, j);
    CHECK(fabsf(mean_ch(&o, 1) - 0.5f) < 0.12f);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static void test_pointillize_shows_canvas(void)
{
    pc_surface s, o;
    size_t n, i;
    bool canvas = false, dots = false;
    float *v;
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0, 0, 1, 1);
    o = runj(&s, "{\"filter\":\"pointillize\",\"cellSize\":10,\"seed\":3,\"background\":[1,0,0,1]}");
    v = region(&o, R, &n);
    for (i = 0; i < n; i += 4) {
        canvas |= v[i] > 0.99f && v[i + 2] < 0.01f;
        dots |= v[i + 2] > 0.9f;
    }
    CHECK(canvas && dots);
    free(v);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static float l601(const float *p)
{
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
}

static void test_diffuse_only_moves_pixels_locally(void)
{
    pc_surface s, o, dark;
    int32_t x, y, dx, dy;
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    o = runj(&s, "{\"filter\":\"diffuse\",\"mode\":\"normal\",\"seed\":1}");
    dark = runj(&s, "{\"filter\":\"diffuse\",\"mode\":\"darkenOnly\",\"seed\":1}");
    for (y = 1; y < 29; y++) {
        for (x = 1; x < 39; x++) {
            float p[4], q[4], d[4];
            bool found = false;
            px(&o, x, y, p);
            for (dy = -1; dy <= 1; dy++) {
                for (dx = -1; dx <= 1; dx++) {
                    px(&s, x + dx, y + dy, q);
                    found |= memcmp(p, q, sizeof p) == 0;
                }
            }
            CHECK(found);
            px(&dark, x, y, d);
            px(&s, x, y, q);
            CHECK(l601(d) <= l601(q) + 1e-6f);
        }
    }
    pc_surface_fini(&s), pc_surface_fini(&o), pc_surface_fini(&dark);
}

static void test_trace_contour(void)
{
    pc_surface s, up, lo, o, off;
    const float bright[4] = {0.8f, 0.8f, 0.8f, 1.0f};
    float p[4];
    pc_algo_filter f = F("{\"filter\":\"traceContour\",\"level\":128,\"upper\":true}");
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0.2f, 0.2f, 0.2f, 1);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(20, 0, 40, 30), bright, 4, NULL) == PC_OK);
    up = run(&s, &f);
    px(&up, 20, 10, p);
    CHECK(p[0] == 0.0f);
    px(&up, 19, 10, p);
    CHECK(p[0] == 1.0f);
    px(&up, 30, 10, p);
    CHECK(p[0] == 1.0f);
    lo = runj(&s, "{\"filter\":\"traceContour\",\"level\":128,\"upper\":false}");
    px(&lo, 19, 10, p);
    CHECK(p[0] == 0.0f);
    /* trace_contour_off_canvas_does_not_panic */
    flat(&off, PC_SAMPLE_TYPE_F32, pc_rect_make(100, 100, 120, 120), 0.8f, 0.8f, 0.8f, 1);
    CHECK(pc_algo_apply_tiled(&off, &f, pc_surface_content_bounds(&off), R, NULL, 64, NULL, &o, NULL) ==
          PC_OK);
    pc_surface_fini(&o);
    pc_surface_fini(&off), pc_surface_fini(&s), pc_surface_fini(&up), pc_surface_fini(&lo);
    pc_algo_filter_fini(&f);
}

static void test_tiles_leave_gaps_with_fill(void)
{
    pc_surface s, o;
    size_t n, i, tiles = 0;
    bool gaps = false;
    float *v;
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0, 0, 1, 1);
    o = runj(
        &s,
        "{\"filter\":\"tiles\",\"count\":3,\"maxOffset\":40,\"fill\":\"foreground\",\"foreground\":[0,1,0,1],"
        "\"background\":[1,1,1,1],\"seed\":2}");
    v = region(&o, R, &n);
    for (i = 0; i < n; i += 4) {
        gaps |= v[i + 1] > 0.99f;
        tiles += v[i + 2] > 0.99f;
    }
    CHECK(gaps && tiles > 600);
    free(v);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static void test_extrude_blocks_and_pyramids_shade_cells(void)
{
    pc_surface s, blocks, fl, pyr;
    float a[4], b[4];
    bool same = false;
    int32_t x;
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    blocks =
        runj(&s, "{\"filter\":\"extrude\",\"kind\":\"blocks\",\"size\":8,\"depth\":40,\"levelBased\":false,"
                 "\"solidFront\":true,\"maskIncomplete\":false,\"seed\":1}");
    for (x = 5; x < 35; x++) {
        px(&blocks, x, 12, a), px(&blocks, x + 1, 12, b);
        same |= memcmp(a, b, sizeof a) == 0;
    }
    CHECK(same);
    flat(&fl, PC_SAMPLE_TYPE_F32, R, 0.5f, 0.5f, 0.5f, 1);
    pyr = runj(&fl,
               "{\"filter\":\"extrude\",\"kind\":\"pyramids\",\"size\":10,\"depth\":100,\"levelBased\":false,"
               "\"solidFront\":false,\"maskIncomplete\":false,\"seed\":1}");
    px(&pyr, 11, 15, a), px(&pyr, 18, 15, b);
    CHECK(a[0] > b[0]);
    pc_surface_fini(&s), pc_surface_fini(&blocks), pc_surface_fini(&fl), pc_surface_fini(&pyr);
}

static void test_fibers_lie_between_colours_and_are_seeded(void)
{
    const char *j1 =
        "{\"filter\":\"fibers\",\"variance\":16,\"strength\":4,\"seed\":1,\"foreground\":[1,0,0,1],"
        "\"background\":[0,0,1,1]}";
    const char *j2 =
        "{\"filter\":\"fibers\",\"variance\":16,\"strength\":4,\"seed\":2,\"foreground\":[1,0,0,1],"
        "\"background\":[0,0,1,1]}";
    pc_surface s, a, a2, b;
    size_t n, i;
    float *v;
    flat(&s, PC_SAMPLE_TYPE_F32, R, 1, 1, 1, 1);
    a = runj(&s, j1);
    a2 = runj(&s, j1);
    b = runj(&s, j2);
    CHECK(pc_surface_eq(&a, &a2));
    CHECK(!pc_surface_eq(&a, &b));
    v = region(&a, R, &n);
    for (i = 0; i < n; i += 4) {
        CHECK(fabsf(v[i] + v[i + 2] - 1.0f) < 1e-5f && fabsf(v[i + 1]) < 1e-6f);
    }
    free(v);
    pc_surface_fini(&s), pc_surface_fini(&a), pc_surface_fini(&a2), pc_surface_fini(&b);
}

static void test_lens_flare_is_brightest_at_its_centre(void)
{
    pc_surface s, o;
    float a[4], b[4];
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0.2f, 0.2f, 0.2f, 1);
    o = runj(&s, "{\"filter\":\"lensFlare\",\"brightness\":100,\"centerX\":0.25,\"centerY\":0.5,\"lens\":"
                 "\"prime105\"}");
    px(&o, 10, 15, a), px(&o, 38, 2, b);
    CHECK(a[0] > 0.9f && a[0] > b[0] && b[0] >= 0.2f - 1e-6f);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static void test_lighting_point_light_falls_off(void)
{
    pc_surface s, o;
    float a[4], b[4];
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0.8f, 0.8f, 0.8f, 1);
    o = runj(&s,
             "{\"filter\":\"lightingEffects\",\"lights\":[{\"kind\":\"point\",\"x\":0.1,\"y\":0.5,\"z\":0.2,"
             "\"radius\":1,\"intensity\":50}],\"gloss\":-100,\"metallic\":0,\"exposure\":0,\"ambience\":0,"
             "\"texture\":\"none\",\"height\":0,\"whiteIsHigh\":true}");
    px(&o, 4, 15, a), px(&o, 36, 15, b);
    CHECK(a[0] > b[0] + 0.1f);
    pc_surface_fini(&o);
    o = runj(&s,
             "{\"filter\":\"lightingEffects\",\"lights\":[{\"kind\":\"spot\",\"x\":0.5,\"y\":0.5,\"z\":0.3,"
             "\"targetX\":0.5,\"targetY\":0.5,\"cone\":20}],\"gloss\":0,\"metallic\":0,\"exposure\":0,"
             "\"ambience\":0,"
             "\"texture\":\"none\",\"height\":0,\"whiteIsHigh\":true}");
    px(&o, 20, 15, a), px(&o, 1, 1, b);
    CHECK(a[0] > 0.3f && b[0] < 1e-4f);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static bool px_eq(const pc_surface *a, const pc_surface *b, int32_t x, int32_t y)
{
    float p[4], q[4];
    px(a, x, y, p), px(b, x, y, q);
    return memcmp(p, q, sizeof p) == 0;
}

static void test_gallery_blurs_follow_their_geometry(void)
{
    pc_surface s, o, stripes;
    int32_t x, y;
    float p[4];
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    o = runj(&s,
             "{\"filter\":\"tiltShift\",\"blur\":6,\"centerX\":0.5,\"centerY\":0.5,\"angle\":0,\"focus\":0.1,"
             "\"transition\":0.2}");
    CHECK(px_eq(&o, &s, 10, 15) && !px_eq(&o, &s, 10, 1));
    pc_surface_fini(&o);
    o = runj(&s, "{\"filter\":\"irisBlur\",\"pins\":[{\"blur\":6,\"radiusX\":0.3,\"radiusY\":0.3}]}");
    CHECK(px_eq(&o, &s, 20, 15) && !px_eq(&o, &s, 1, 1));
    pc_surface_fini(&o);
    o = runj(&s, "{\"filter\":\"spinBlur\",\"pins\":[{\"radiusX\":0.3,\"radiusY\":0.3,\"blurAngle\":40}]}");
    CHECK(px_eq(&o, &s, 1, 1) && !px_eq(&o, &s, 24, 15));
    pc_surface_fini(&o);
    /* Horizontal path: vertical stripes get blurred, horizontal ones survive. */
    pc_surface_init(&stripes, rgba(PC_SAMPLE_TYPE_F32));
    for (y = 0; y < 30; y++) {
        for (x = 0; x < 40; x++) {
            float v = y % 2 == 0 ? 1.0f : 0.0f, q[4] = {v, v, v, 1};
            CHECK(pc_surface_write_pixel(&stripes, x, y, q, 4, NULL) == PC_OK);
        }
    }
    o = runj(&stripes,
             "{\"filter\":\"pathBlur\",\"paths\":[{\"points\":[[0,0.5],[1,0.5]],\"speed\":6,\"taper\":0}]}");
    px(&o, 20, 10, p);
    CHECK(fabsf(p[0] - 1.0f) < 1e-5f);
    px(&o, 20, 11, p);
    CHECK(p[0] < 1e-5f);
    pc_surface_fini(&o), pc_surface_fini(&stripes), pc_surface_fini(&s);
}

static void test_custom_kernel_shifts_and_offsets(void)
{
    pc_surface s, o, fl;
    float a[4], b[4];
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    o = runj(
        &s,
        "{\"filter\":\"custom\",\"kernel\":[0,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0],\"scale\":2,"
        "\"offset\":0}");
    px(&o, 10, 10, a), px(&s, 11, 10, b);
    CHECK(a[0] == b[0]);
    pc_surface_fini(&o);
    flat(&fl, PC_SAMPLE_TYPE_F32, R, 0.2f, 0.2f, 0.2f, 1);
    o = runj(
        &fl,
        "{\"filter\":\"custom\",\"kernel\":[0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0],\"scale\":1,"
        "\"offset\":51}");
    px(&o, 5, 5, a);
    CHECK(fabsf(a[0] - 0.4f) < 1e-5f);
    pc_surface_fini(&o), pc_surface_fini(&fl), pc_surface_fini(&s);
}

static void test_hsb_filter_encodes_and_decodes(void)
{
    pc_surface s, hsb, back;
    float p[4];
    flat(&s, PC_SAMPLE_TYPE_F32, R, 1, 0, 0, 1);
    hsb = runj(&s, "{\"filter\":\"hsbHsl\",\"input\":\"rgb\",\"output\":\"hsb\"}");
    px(&hsb, 3, 3, p);
    CHECK(p[0] == 0.0f && p[1] == 1.0f && p[2] == 1.0f && p[3] == 1.0f);
    back = runj(&hsb, "{\"filter\":\"hsbHsl\",\"input\":\"hsb\",\"output\":\"rgb\"}");
    CHECK(px_eq(&back, &s, 3, 3));
    pc_surface_fini(&s), pc_surface_fini(&hsb), pc_surface_fini(&back);
}

/* other2.rs: hsb_hsl_round_trip. */
static void test_hsb_hsl_round_trip(void)
{
    const float cs[5][3] = {
        {0.2f, 0.7f, 0.4f}, {1, 0, 0}, {0.5f, 0.5f, 0.5f}, {0.1f, 0.2f, 0.9f}, {0.9f, 0.8f, 0.05f}};
    float h[3], a[3], l[3], b[3];
    size_t i, k;
    for (i = 0; i < 5; i++) {
        pc_algo_other2_rgb_to_hsb(cs[i], h);
        pc_algo_hsb_to_rgb(h, a);
        pc_algo_rgb_to_hsl(cs[i], l);
        pc_algo_hsl_to_rgb(l, b);
        for (k = 0; k < 3; k++) {
            CHECK(fabsf(a[k] - cs[i][k]) < 1e-5f && fabsf(b[k] - cs[i][k]) < 1e-5f);
        }
    }
    pc_algo_other2_rgb_to_hsb(cs[1], h);
    CHECK(h[0] == 0.0f && h[1] == 1.0f && h[2] == 1.0f);
    {
        const float blue[3] = {0, 0, 1};
        pc_algo_rgb_to_hsl(blue, l);
        CHECK(fabsf(l[0] - 2.0f / 3.0f) < 1e-6f && fabsf(l[2] - 0.5f) < 1e-6f);
    }
}

static void test_deinterlace_rebuilds_a_field(void)
{
    pc_surface s, o;
    int32_t y;
    float p[4];
    pc_surface_init(&s, rgba(PC_SAMPLE_TYPE_F32));
    for (y = 0; y < 30; y++) {
        float v = y % 2 == 0 ? 0.8f : 0.2f, q[4] = {v, v, v, 1};
        CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, y, 40, y + 1), q, 4, NULL) == PC_OK);
    }
    o = runj(&s, "{\"filter\":\"deInterlace\",\"eliminateEven\":false,\"interpolate\":false}");
    for (y = 0; y < 30; y++) {
        px(&o, 10, y, p);
        CHECK(fabsf(p[0] - 0.2f) < 1e-6f);
    }
    pc_surface_fini(&o);
    o = runj(&s, "{\"filter\":\"deInterlace\",\"eliminateEven\":true,\"interpolate\":true}");
    px(&o, 10, 5, p);
    CHECK(fabsf(p[0] - 0.8f) < 1e-6f);
    pc_surface_fini(&o), pc_surface_fini(&s);
}

static void test_ntsc_limits_saturated_colours_only(void)
{
    pc_surface g, y, o;
    float p[4];
    const float yl[3] = {1, 1, 0};
    flat(&g, PC_SAMPLE_TYPE_F32, R, 0.5f, 0.5f, 0.5f, 1);
    o = runj(&g, "{\"filter\":\"ntscColors\"}");
    CHECK(px_eq(&o, &g, 3, 3));
    pc_surface_fini(&o);
    flat(&y, PC_SAMPLE_TYPE_F32, R, 1, 1, 0, 1);
    o = runj(&y, "{\"filter\":\"ntscColors\"}");
    px(&o, 3, 3, p);
    CHECK(p[2] > 0.05f && fabsf(l601(p) - l601(yl)) < 1e-3f);
    pc_surface_fini(&o), pc_surface_fini(&g), pc_surface_fini(&y);
}

static void test_displace_shifts_by_map_and_shear_by_curve(void)
{
    pc_surface s, o;
    pc_algo_image map;
    pc_algo_displace_params p = {25.0f, 0.0f, false, PC_ALGO_UNDEF_WRAP, &map};
    pc_algo_filter f;
    size_t i;
    int32_t x;
    pattern(&s, PC_SAMPLE_TYPE_F32, R);
    CHECK(pc_algo_image_init(&map, pc_rect_make(0, 0, 8, 8), 4, NULL) == PC_OK);
    for (i = 0; i < 64; i++) {
        map.data[i * 4] = 0.5f + 3.0f / 64.0f, map.data[i * 4 + 1] = 0.5f, map.data[i * 4 + 2] = 0.5f,
                     map.data[i * 4 + 3] = 1.0f;
    }
    CHECK(pc_algo_filter_make(&pc_algo_def_displace, &p, &f, NULL) == PC_OK);
    o = run(&s, &f);
    for (x = 0; x < 30; x++) {
        float a[4], b[4];
        px(&o, x, 7, a), px(&s, x + 3, 7, b);
        CHECK(memcmp(a, b, sizeof a) == 0);
    }
    pc_surface_fini(&o);
    pc_algo_filter_fini(&f);
    pc_algo_image_fini(&map);
    o = runj(&s, "{\"filter\":\"shear\",\"points\":[[0,0.1],[1,0.1]],\"undefined\":\"wrap\"}");
    {
        float a[4], b[4];
        px(&o, 10, 4, a), px(&s, 8, 4, b);
        CHECK(memcmp(a, b, sizeof a) == 0);
    }
    pc_surface_fini(&o), pc_surface_fini(&s);
}

static void test_wind_streaks_downwind_from_edges(void)
{
    pc_surface s, o;
    const float white[4] = {1, 1, 1, 1};
    float l = 0, r = 0, p[4];
    int32_t y;
    flat(&s, PC_SAMPLE_TYPE_F32, R, 0, 0, 0, 1);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(10, 0, 12, 30), white, 4, NULL) == PC_OK);
    o = runj(&s, "{\"filter\":\"wind\",\"method\":\"blast\",\"fromRight\":false,\"seed\":1}");
    for (y = 0; y < 30; y++) {
        px(&o, 8, y, p);
        l += p[0];
        px(&o, 14, y, p);
        r += p[0];
    }
    CHECK(r > l + 0.5f);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

static void test_oil_paint_smooths_along_strokes(void)
{
    /* tests_ext noisy(): Add Noise is chunk L5's; a hashed noise field stands in for it. */
    pc_surface s, o;
    int32_t x, y;
    double vs = 0, vo = 0, ms = 0, mo = 0;
    float p[4];
    size_t n = 0;
    pc_surface_init(&s, rgba(PC_SAMPLE_TYPE_F32));
    for (y = 0; y < 30; y++) {
        for (x = 0; x < 40; x++) {
            float q[4] = {0.5f + (pc_algo_hash01(x, y, 0, 4) - 0.5f) * 0.4f,
                          0.5f + (pc_algo_hash01(x, y, 1, 4) - 0.5f) * 0.4f,
                          0.5f + (pc_algo_hash01(x, y, 2, 4) - 0.5f) * 0.4f, 1.0f};
            CHECK(pc_surface_write_pixel(&s, x, y, q, 4, NULL) == PC_OK);
        }
    }
    o = runj(&s,
             "{\"filter\":\"oilPaint\",\"stylization\":5,\"cleanliness\":5,\"scale\":2,\"bristleDetail\":0,"
             "\"lighting\":false,\"angle\":0,\"shine\":0}");
    for (y = 8; y < 22; y++) {
        for (x = 8; x < 32; x++) {
            px(&s, x, y, p), ms += p[0];
            px(&o, x, y, p), mo += p[0];
            n++;
        }
    }
    ms /= (double)n, mo /= (double)n;
    for (y = 8; y < 22; y++) {
        for (x = 8; x < 32; x++) {
            px(&s, x, y, p), vs += (p[0] - ms) * (p[0] - ms);
            px(&o, x, y, p), vo += (p[0] - mo) * (p[0] - mo);
        }
    }
    CHECK(vo < vs * 0.5);
    pc_surface_fini(&s), pc_surface_fini(&o);
}

/* distort2.rs: curve_passes_through_points_and_clamps. */
static void test_curve_passes_through_points_and_clamps(void)
{
    const float pts[3][2] = {{0, 0}, {0.5f, 0.4f}, {1, -0.2f}}, flat2[2][2] = {{0, 0.3f}, {1, 0.3f}};
    CHECK(fabsf(pc_algo_distort2_curve_at(pts, 3, 0.5f) - 0.4f) < 1e-6f);
    CHECK(fabsf(pc_algo_distort2_curve_at(pts, 3, 1.0f) + 0.2f) < 1e-6f);
    CHECK(pc_algo_distort2_curve_at(pts, 3, -1.0f) == 0.0f);
    CHECK(pc_algo_distort2_curve_at(flat2, 2, 0.42f) == 0.3f);
}

/* ---- tests_mosaic.rs ---- */

/* ---- never crash ---- */

static void test_hostile_parameters_never_crash(void)
{
    static const char *const cases[] = {
        "{\"filter\":\"minimum\",\"radius\":1e30}",
        "{\"filter\":\"maximum\",\"radius\":1e30,\"preserve\":\"roundness\"}",
        "{\"filter\":\"mosaic\",\"cellSize\":1e30}",
        "{\"filter\":\"emboss\",\"angle\":1e30,\"height\":1e30,\"amount\":1e30}",
        "{\"filter\":\"offset\",\"horizontal\":-2147483648,\"vertical\":2147483647,\"undefined\":\"wrap\"}",
        "{\"filter\":\"wave\",\"generators\":4294967295,\"wavelengthMin\":0,\"wavelengthMax\":-1,"
        "\"amplitudeMin\":1e30,"
        "\"amplitudeMax\":1e30,\"waveType\":\"square\",\"undefined\":\"wrap\",\"seed\":0}",
        "{\"filter\":\"crystallize\",\"cellSize\":1e30,\"seed\":0}",
        "{\"filter\":\"pointillize\",\"cellSize\":-5,\"seed\":0,\"background\":[1e30,-1e30,0,2]}",
        "{\"filter\":\"extrude\",\"kind\":\"blocks\",\"size\":-1e30,\"depth\":1e30,\"levelBased\":true,"
        "\"solidFront\":false,\"maskIncomplete\":true,\"seed\":0}",
        "{\"filter\":\"tiles\",\"count\":0,\"maxOffset\":1e30,\"fill\":\"inverse\",\"foreground\":[0,0,0,1],"
        "\"background\":[1,1,1,1],\"seed\":0}",
        "{\"filter\":\"shear\",\"points\":[[1e30,1e30],[-1e30,0],[0.5,0.5],[0.5,0.5]],\"undefined\":"
        "\"transparent\"}",
        "{\"filter\":\"lightingEffects\",\"lights\":[{\"radius\":0,\"cone\":0}],\"gloss\":1e30,\"metallic\":-"
        "1e30,"
        "\"exposure\":1e30,\"ambience\":1e30,\"texture\":\"alpha\",\"height\":1e30,\"whiteIsHigh\":false}",
        "{\"filter\":\"spinBlur\",\"pins\":[{\"radiusX\":0,\"radiusY\":0,\"angle\":1e30,\"blurAngle\":1e30}]"
        "}",
        "{\"filter\":\"pathBlur\",\"paths\":[{\"points\":[[0,0],[0,0]],\"speed\":1e30,\"taper\":-1e30}]}",
        "{\"filter\":\"custom\",\"kernel\":[1e30,-1e30],\"scale\":0,\"offset\":1e30}",
        "{\"filter\":\"filterGallery\",\"effects\":[{\"filter\":\"glass\",\"values\":{\"distortion\":-5}},"
        "{\"filter\":\"tornEdges\",\"values\":{\"contrast\":0}},{\"filter\":\"craquelure\",\"values\":{"
        "\"crackSpacing\":-2}}]}",
    };
    pc_surface s;
    size_t i;
    pattern(&s, PC_SAMPLE_TYPE_U8, R);
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        pc_algo_filter f = F(cases[i]);
        pc_rect area = pc_algo_output_area(&f, pc_surface_content_bounds(&s), R, NULL);
        pc_surface o;
        pc_error err;
        /* Either a result or an error; never a crash or a hang. Clip absurd output areas. */
        area = pc_rect_intersect(area, pc_rect_make(-64, -64, 104, 94));
        if (pc_algo_apply(&s, &f, area, R, NULL, &o, &err) == PC_OK) {
            pc_surface_fini(&o);
        }
        pc_algo_filter_fini(&f);
    }
    /* NaN parameters: the reference panics for these; C reports an error. */
    {
        pc_algo_extrude_params e = {PC_ALGO_EXTRUDE_BLOCKS, NAN, 10.0f, false, false, false, 0};
        pc_algo_tilt_shift_params t = {NAN, 0.5f, 0.5f, 0.0f, 0.1f, 0.2f};
        pc_algo_filter f;
        pc_surface o;
        CHECK(pc_algo_filter_make(&pc_algo_def_extrude, &e, &f, NULL) == PC_OK);
        CHECK(pc_algo_apply(&s, &f, R, R, NULL, &o, NULL) == PC_ERR_INVALID);
        pc_algo_filter_fini(&f);
        /* A NaN Tilt-Shift amount blurs nothing (the reference now floors the sigma at 0). */
        CHECK(pc_algo_filter_make(&pc_algo_def_tilt_shift, &t, &f, NULL) == PC_OK);
        CHECK(pc_algo_apply(&s, &f, R, R, NULL, &o, NULL) == PC_OK);
        CHECK(same_region(&s, &o, R));
        pc_surface_fini(&o);
        pc_algo_filter_fini(&f);
    }
    /* A displace map with zero channels is ignored. */
    {
        pc_algo_image map = {{0, 0, 4, 4}, 0, NULL, 0};
        pc_algo_displace_params p = {10, 10, true, PC_ALGO_UNDEF_WRAP, &map};
        pc_algo_filter f;
        pc_surface o;
        CHECK(pc_algo_filter_make(&pc_algo_def_displace, &p, &f, NULL) == PC_OK);
        CHECK(pc_algo_apply(&s, &f, R, R, NULL, &o, NULL) == PC_OK);
        CHECK(same_region(&o, &s, R));
        pc_surface_fini(&o);
        pc_algo_filter_fini(&f);
    }
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    RUN(test_minimum_maximum_spread);
    RUN(test_maximum_roundness_grows_a_point_into_a_disc);
    RUN(test_minimum_maximum_roundness_match_brute_force);
    RUN(test_running_extreme_windows);
    RUN(test_square_min_max_matches_the_direct_window_exactly);
    RUN(test_minimum_maximum_old_params_default_to_squareness);
    RUN(test_offset_wrap_repeat_transparent);
    RUN(test_mosaic_cells_are_uniform);
    RUN(test_stylize_basics);
    RUN(test_distortions_are_identity_at_zero);
    RUN(test_twirl_keeps_centre_and_outside);
    RUN(test_other_colour_modes_work);
    RUN(test_half_selection_mixes_halfway);
    RUN(test_halos_and_output_area);
    RUN(test_params_serde_roundtrip);
    RUN(test_new_filters_are_tile_independent);
    RUN(test_new_filters_change_pixels_and_respect_empty_selection);
    RUN(test_new_filters_agree_across_bit_depths);
    RUN(test_new_filters_work_in_other_colour_modes);
    RUN(test_identities_at_zero_strength);
    RUN(test_flat_images_stay_flat);
    RUN(test_mezzotint_is_binary_and_tracks_brightness);
    RUN(test_halftone_covers_by_ink);
    RUN(test_pointillize_shows_canvas);
    RUN(test_diffuse_only_moves_pixels_locally);
    RUN(test_trace_contour);
    RUN(test_tiles_leave_gaps_with_fill);
    RUN(test_extrude_blocks_and_pyramids_shade_cells);
    RUN(test_fibers_lie_between_colours_and_are_seeded);
    RUN(test_lens_flare_is_brightest_at_its_centre);
    RUN(test_lighting_point_light_falls_off);
    RUN(test_gallery_blurs_follow_their_geometry);
    RUN(test_custom_kernel_shifts_and_offsets);
    RUN(test_hsb_filter_encodes_and_decodes);
    RUN(test_hsb_hsl_round_trip);
    RUN(test_deinterlace_rebuilds_a_field);
    RUN(test_ntsc_limits_saturated_colours_only);
    RUN(test_displace_shifts_by_map_and_shear_by_curve);
    RUN(test_wind_streaks_downwind_from_edges);
    RUN(test_oil_paint_smooths_along_strokes);
    RUN(test_curve_passes_through_points_and_clamps);
    RUN(test_hostile_parameters_never_crash);
    return pc_t_finish();
}
