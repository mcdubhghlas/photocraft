/* Resync R6 additions (ports of the upstream tests in crates/algo/src): Color to Alpha, the
 * bit-identical fast paths of Extrude / Wind / Facet / Color Halftone against their direct
 * searches, Trap's linear-time dilation, direct (intensity) registration for Auto-Align, and
 * Camera Raw's local-tone Shadows/Highlights and measured Whites/Blacks. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "algo/camera_raw_int.h"
#include "algo/color_to_alpha.h"
#include "algo/direct.h"
#include "algo/fx.h"
#include "base/json.h"
#include "harness.h"

static bool close_to(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

/* ---- Color to Alpha ---- */

static void test_the_key_colour_becomes_fully_transparent(void)
{
    float px[3] = {1, 1, 1}, key[3] = {1, 1, 1}, px2[3] = {0.2f, 0.4f, 0.6f};
    CHECK(pc_algo_c2a_unmix(px, key, 3, 0.0f, 1.0f) == 0.0f);
    CHECK(pc_algo_c2a_unmix(px2, (const float[3]){0.2f, 0.4f, 0.6f}, 3, 0.0f, 1.0f) == 0.0f);
}

static void test_grey_on_white_is_black_with_partial_alpha(void)
{
    float px[3] = {0.7f, 0.7f, 0.7f}, white[3] = {1, 1, 1}, black[3] = {0, 0, 0}, a;
    a = pc_algo_c2a_unmix(px, white, 3, 0.0f, 1.0f);
    CHECK(close_to(a, 0.3f, 1e-6f));
    CHECK(close_to(px[0], 0.0f, 1e-5f) && close_to(px[1], 0.0f, 1e-5f) && close_to(px[2], 0.0f, 1e-5f));
    CHECK(pc_algo_c2a_unmix(black, white, 3, 0.0f, 1.0f) == 1.0f);
    CHECK(black[0] == 0.0f && black[1] == 0.0f && black[2] == 0.0f);
}

static void test_unmixed_colour_composited_over_the_key_gives_back_the_pixel(void)
{
    static const float key[3] = {0.2f, 0.6f, 0.9f};
    static const float ps[4][3] = {
        {0.5f, 0.5f, 0.5f}, {0.1f, 0.9f, 0.95f}, {0.25f, 0.6f, 0.0f}, {1.0f, 0.0f, 0.3f}};
    int k, i;
    for (k = 0; k < 4; k++) {
        float q[3];
        float a;
        memcpy(q, ps[k], sizeof q);
        a = pc_algo_c2a_unmix(q, key, 3, 0.0f, 1.0f);
        for (i = 0; i < 3; i++) {
            CHECK(q[i] >= 0.0f && q[i] <= 1.0f);
            CHECK(close_to(a * q[i] + (1.0f - a) * key[i], ps[k][i], 1e-5f));
        }
    }
}

static void test_thresholds_clip_and_stretch_the_opacity(void)
{
    float one[3] = {1, 1, 1}, px[3] = {0.95f, 0.95f, 0.95f}, grey[3] = {0.4f, 0.4f, 0.4f};
    CHECK(pc_algo_c2a_remap(0.1f, 0.2f, 0.8f) == 0.0f);
    CHECK(pc_algo_c2a_remap(0.2f, 0.2f, 0.8f) == 0.0f);
    CHECK(pc_algo_c2a_remap(0.8f, 0.2f, 0.8f) == 1.0f);
    CHECK(close_to(pc_algo_c2a_remap(0.5f, 0.2f, 0.8f), 0.5f, 1e-6f));
    /* Crossed thresholds never divide by zero. */
    CHECK(pc_algo_c2a_remap(0.5f, 0.6f, 0.4f) == 0.0f);
    CHECK(pc_algo_c2a_remap(0.7f, 0.6f, 0.4f) == 1.0f);
    CHECK(pc_algo_c2a_remap(0.5f, 0.5f, 0.5f) == 0.0f);
    CHECK(pc_algo_c2a_unmix(px, one, 3, 0.1f, 1.0f) == 0.0f);
    CHECK(pc_algo_c2a_unmix(grey, one, 3, 0.0f, 0.5f) == 1.0f);
    CHECK(grey[0] == 0.4f && grey[1] == 0.4f && grey[2] == 0.4f);
}

static void test_hdr_and_non_finite_samples_stay_opaque_and_finite(void)
{
    float one[3] = {1, 1, 1}, zero[3] = {0, 0, 0}, hdr[3] = {1.5f, 1, 1}, nanp[3] = {NAN, 0.5f, 0.5f};
    float dark[3] = {0.25f, 0.5f, 0.0f}, a;
    CHECK(pc_algo_c2a_unmix(hdr, one, 3, 0.0f, 1.0f) == 1.0f);
    CHECK(hdr[0] == 1.5f && hdr[1] == 1.0f);
    a = pc_algo_c2a_unmix(nanp, one, 3, 0.0f, 1.0f);
    CHECK(isfinite(a) && close_to(a, 0.5f, 1e-6f));
    /* A black key: everything brighter is a mix of black and a brighter colour. */
    a = pc_algo_c2a_unmix(dark, zero, 3, 0.0f, 1.0f);
    CHECK(close_to(a, 0.5f, 1e-6f));
    CHECK(close_to(dark[0], 0.5f, 1e-6f) && close_to(dark[1], 1.0f, 1e-6f) && close_to(dark[2], 0.0f, 1e-6f));
}

static pc_surface c2a_run(const pc_surface *s, const float color[4], float t, float o, pc_rect r)
{
    pc_algo_color_to_alpha_params p;
    pc_algo_filter f;
    pc_surface out;
    pc_rect area;
    memcpy(p.color, color, sizeof p.color);
    p.transparency_threshold = t;
    p.opacity_threshold = o;
    CHECK(pc_algo_filter_make(&pc_algo_def_color_to_alpha, &p, &f, NULL) == PC_OK);
    CHECK(pc_algo_filter_makes_transparency(&f));
    CHECK(strcmp(pc_algo_filter_label(&f), "Color to Alpha") == 0);
    area = pc_algo_output_area(&f, pc_surface_content_bounds(s), r, NULL);
    CHECK(pc_algo_apply(s, &f, area, r, NULL, &out, NULL) == PC_OK);
    pc_algo_filter_fini(&f);
    return out;
}

static void test_color_to_alpha_works_at_every_depth_and_in_gray(void)
{
    static const pc_sample_type sts[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    static const float white[4] = {1, 1, 1, 1};
    int si;
    for (si = 0; si < 3; si++) {
        pc_pixel_format f = {PC_COLOR_MODE_RGB, sts[si], true}, g = {PC_COLOR_MODE_GRAYSCALE, sts[si], true};
        pc_rect r = pc_rect_make(0, 0, 4, 1), r2 = pc_rect_make(0, 0, 2, 1);
        const float d[16] = {1, 1, 1, 1, 0.6f, 0.6f, 0.6f, 1, 0, 0, 0, 1, 0.6f, 0.6f, 0.6f, 0.5f};
        const float gd[4] = {1, 1, 0.75f, 1};
        float tol = sts[si] == PC_SAMPLE_TYPE_U8 ? 2.0f / 255.0f : 1e-3f, p[4], q[4];
        pc_surface s, o, gs, go;
        pc_surface_init(&s, f);
        CHECK(pc_surface_write_region(&s, r, d, 16, NULL) == PC_OK);
        o = c2a_run(&s, white, 0.0f, 1.0f, r);
        pc_surface_read_pixel(&o, 0, 0, p);
        CHECK(close_to(p[3], 0.0f, tol));
        pc_surface_read_pixel(&o, 1, 0, p);
        CHECK(close_to(p[3], 0.4f, tol) && close_to(p[0], 0.0f, tol));
        pc_surface_read_pixel(&o, 2, 0, p);
        pc_surface_read_pixel(&s, 2, 0, q);
        CHECK(memcmp(p, q, sizeof p) == 0);
        pc_surface_read_pixel(&o, 3, 0, p);
        CHECK(close_to(p[3], 0.2f, tol));
        pc_surface_init(&gs, g);
        CHECK(pc_surface_write_region(&gs, r2, gd, 4, NULL) == PC_OK);
        go = c2a_run(&gs, white, 0.0f, 1.0f, r2);
        pc_surface_read_pixel(&go, 0, 0, p);
        CHECK(close_to(p[1], 0.0f, tol));
        pc_surface_read_pixel(&go, 1, 0, p);
        CHECK(close_to(p[0], 0.0f, tol) && close_to(p[1], 0.25f, tol));
        pc_surface_fini(&s), pc_surface_fini(&o), pc_surface_fini(&gs), pc_surface_fini(&go);
    }
}

static void test_coloured_key_and_surfaces_without_alpha(void)
{
    static const float blue[4] = {0.2f, 0.4f, 0.8f, 1.0f}, white[4] = {1, 1, 1, 1};
    const float d[8] = {0.2f, 0.4f, 0.8f, 1.0f, 0.1f, 0.2f, 0.4f, 1.0f}, one = 1.0f;
    pc_rect r = pc_rect_make(0, 0, 2, 1), r2 = pc_rect_make(0, 0, 2, 2);
    pc_pixel_format g8 = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false};
    pc_surface s, o, gs, go;
    float p[4], *a = NULL, *b = NULL;
    size_t la = 0, lb = 0;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA32F);
    CHECK(pc_surface_write_region(&s, r, d, 8, NULL) == PC_OK);
    o = c2a_run(&s, blue, 0.0f, 1.0f, r);
    pc_surface_read_pixel(&o, 0, 0, p);
    CHECK(p[3] < 1e-6f);
    pc_surface_read_pixel(&o, 1, 0, p);
    CHECK(close_to(p[3], 0.5f, 1e-5f) && close_to(p[0], 0.0f, 1e-5f) && close_to(p[2], 0.0f, 1e-5f));
    pc_surface_init(&gs, g8);
    CHECK(pc_surface_fill_rect(&gs, r2, &one, 1, NULL) == PC_OK);
    go = c2a_run(&gs, white, 0.0f, 1.0f, r2);
    CHECK(pc_surface_read_region(&gs, r2, &a, &la, NULL) == PC_OK);
    CHECK(pc_surface_read_region(&go, r2, &b, &lb, NULL) == PC_OK);
    CHECK(la == lb && memcmp(a, b, la * sizeof *a) == 0);
    free(a), free(b);
    pc_surface_fini(&s), pc_surface_fini(&o), pc_surface_fini(&gs), pc_surface_fini(&go);
}

/* ---- fast paths against the direct searches ---- */

static void image_fill(pc_algo_image *img, pc_rect rect, size_t n, int kind)
{
    size_t i;
    CHECK(pc_algo_image_init(img, rect, n, NULL) == PC_OK);
    for (i = 0; i < img->len; i++) {
        size_t px = i / n, c = i % n;
        int32_t x = rect.x0 + (int32_t)(px % pc_rect_width(rect)),
                y = rect.y0 + (int32_t)(px / pc_rect_width(rect));
        float v;
        if (kind == 0) {
            v = (float)((size_t)((x / 5 + y / 7) % 3 + 3) % 3 + c) / 4.0f;
        } else {
            v = (float)((i * 71 + px * 107) % 65536) / 65535.0f;
        }
        if (c == n - 1 && px % 11 == 0) {
            v = 0.0f;
        }
        img->data[i] = v;
    }
}

static bool kernel_both_ways(const char *json, const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx)
{
    pc_algo_filter f;
    float *a = NULL, *b = NULL;
    size_t la = 0, lb = 0;
    bool same;
    CHECK(pc_algo_filter_from_json_str(json, strlen(json), &f, NULL) == PC_OK);
    pc_algo_fx_set_direct_only(false);
    CHECK(pc_algo_kernel(&f, src, out, ctx, &a, &la, NULL) == PC_OK);
    pc_algo_fx_set_direct_only(true);
    CHECK(pc_algo_kernel(&f, src, out, ctx, &b, &lb, NULL) == PC_OK);
    pc_algo_fx_set_direct_only(false);
    same = la == lb && (la == 0 || memcmp(a, b, la * sizeof *a) == 0);
    if (!same) {
        fprintf(stderr, "  fast path differs: %s out %d,%d,%d,%d ch %zu alpha %d\n", json, out.x0, out.y0,
                out.x1, out.y1, src->ch, ctx->alpha);
    }
    free(a), free(b);
    pc_algo_filter_fini(&f);
    return same;
}

static void test_fast_paths_match_the_direct_searches(void)
{
    static const char *const jsons[] = {
        "{\"filter\":\"extrude\",\"kind\":\"blocks\",\"size\":9,\"depth\":30,\"levelBased\":false,"
        "\"solidFront\":false,\"maskIncomplete\":false,\"seed\":3}",
        "{\"filter\":\"extrude\",\"kind\":\"blocks\",\"size\":6,\"depth\":80,\"levelBased\":true,"
        "\"solidFront\":true,\"maskIncomplete\":true,\"seed\":1}",
        "{\"filter\":\"extrude\",\"kind\":\"blocks\",\"size\":2,\"depth\":255,\"levelBased\":true,"
        "\"solidFront\":false,\"maskIncomplete\":false,\"seed\":9}",
        "{\"filter\":\"wind\",\"method\":\"wind\",\"fromRight\":false,\"seed\":7}",
        "{\"filter\":\"wind\",\"method\":\"blast\",\"fromRight\":true,\"seed\":17}",
        "{\"filter\":\"wind\",\"method\":\"stagger\",\"fromRight\":true,\"seed\":2}",
        "{\"filter\":\"facet\"}",
        "{\"filter\":\"colorHalftone\",\"maxRadius\":4,\"angles\":[108,162,90,45]}",
        "{\"filter\":\"colorHalftone\",\"maxRadius\":1,\"angles\":[0,0,0,0]}",
    };
    static const pc_color_mode modes[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                           PC_COLOR_MODE_LAB};
    pc_rect b = pc_rect_make(-17, -11, 58, 45);
    pc_rect outs[4] = {pc_rect_make(-17, -11, 58, 45), pc_rect_make(-3, 2, 30, 31),
                       pc_rect_make(40, -11, 58, -8), pc_rect_make(-20, -14, 60, 48)};
    size_t j, m, o;
    int al;
    for (m = 0; m < 4; m++) {
        for (al = 0; al < 2; al++) {
            pc_pixel_format pf = {modes[m], PC_SAMPLE_TYPE_F32, al != 0};
            size_t n = pc_pixel_format_channels(pf);
            pc_algo_ctx ctx = {b, modes[m], al != 0};
            int kind;
            for (kind = 0; kind < 2; kind++) {
                pc_algo_image src;
                image_fill(&src, pc_rect_inflate(b, 60), n, kind);
                for (j = 0; j < sizeof jsons / sizeof jsons[0]; j++) {
                    for (o = 0; o < 4; o++) {
                        CHECK(kernel_both_ways(jsons[j], &src, outs[o], &ctx));
                    }
                }
                pc_algo_image_fini(&src);
            }
        }
    }
}

/* ---- Trap ---- */

static void test_trap_matches_the_direct_window_maximum(void)
{
    static const size_t shapes[5][3] = {{1, 31, 5}, {47, 1, 4}, {37, 29, 4}, {37, 29, 5}, {23, 19, 8}};
    static const size_t radii[4] = {1, 3, 10, 128};
    size_t si, ri;
    for (si = 0; si < 5; si++) {
        size_t w = shapes[si][0], h = shapes[si][1], ch = shapes[si][2], n = w * h * ch, i, x, y, c;
        for (ri = 0; ri < 4; ri++) {
            size_t r = radii[ri];
            float *src = malloc(n * sizeof *src), *want = malloc(n * sizeof *want),
                  *got = malloc(n * sizeof *got);
            for (i = 0; i < n; i++) {
                src[i] = (i / ch) % 5 == 0 ? 0.0f : (float)((i * 71 + i / ch * 103) % 65536) / 65535.0f;
            }
            memcpy(want, src, n * sizeof *src);
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    size_t idx = (y * w + x) * ch;
                    if (!(src[idx] > 1e-4f || src[idx + 1] > 1e-4f || src[idx + 2] > 1e-4f ||
                          src[idx + 3] > 1e-4f)) {
                        continue;
                    }
                    for (c = 0; c < 4; c++) {
                        float mx = 0.0f;
                        size_t yy, xx;
                        for (yy = y > r ? y - r : 0; yy <= (y + r < h - 1 ? y + r : h - 1); yy++) {
                            for (xx = x > r ? x - r : 0; xx <= (x + r < w - 1 ? x + r : w - 1); xx++) {
                                mx = fmaxf(mx, src[(yy * w + xx) * ch + c]);
                            }
                        }
                        want[idx + c] = fmaxf(src[idx + c], mx);
                    }
                }
            }
            memcpy(got, src, n * sizeof *src);
            CHECK(pc_algo_trap(got, n, w, h, ch, r, NULL) == PC_OK);
            CHECK(memcmp(got, want, n * sizeof *got) == 0);
            free(src), free(want), free(got);
        }
    }
}

static void test_trap_lines_with_negative_zero_and_specials_match(void)
{
    static const float vals[9] = {NAN, INFINITY, -INFINITY, -0.0f, 0.0f, -2.0f, 0.25f, 1.0f, 2.0f};
    uint32_t seed = 1234567u;
    size_t len, i, x;
    for (len = 1; len < 60; len += 7) {
        static const size_t radii[6] = {1, 3, 5, 16, 50, 128};
        float *row = malloc(len * 4 * sizeof *row), *got = malloc(len * 4 * sizeof *got);
        size_t ri;
        for (i = 0; i < len * 4; i++) {
            seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
            row[i] = vals[seed % 9];
        }
        for (ri = 0; ri < 6; ri++) {
            size_t r = radii[ri];
            memcpy(got, row, len * 4 * sizeof *row);
            /* Ink everywhere (channel values above 1e-4 somewhere), one row: the trap is the plain
             * clipped window maximum along x where the pixel is inked. */
            CHECK(pc_algo_trap(got, len * 4, len, 1, 4, r, NULL) == PC_OK);
            for (x = 0; x < len; x++) {
                const float *p = row + x * 4;
                int c;
                bool inked = p[0] > 1e-4f || p[1] > 1e-4f || p[2] > 1e-4f || p[3] > 1e-4f;
                for (c = 0; c < 4; c++) {
                    float m = 0.0f, want;
                    size_t k;
                    for (k = x > r ? x - r : 0; k <= (x + r < len - 1 ? x + r : len - 1); k++) {
                        float v = row[k * 4 + (size_t)c];
                        m = isnan(v) ? m : (isnan(m) ? v : (v > m ? v : m));
                    }
                    want = inked ? (isnan(p[c]) ? m : (m > p[c] ? m : p[c])) : p[c];
                    CHECK(isnan(want) ? isnan(got[x * 4 + (size_t)c]) : got[x * 4 + (size_t)c] == want);
                }
            }
        }
        free(row), free(got);
    }
}

static void test_trap_refuses_a_malformed_buffer(void)
{
    float px[20];
    size_t i;
    for (i = 0; i < 20; i++) {
        px[i] = 0.5f;
    }
    CHECK(pc_algo_trap(px, 20, SIZE_MAX, 2, 5, 10, NULL) != PC_OK);
    for (i = 0; i < 20; i++) {
        CHECK(px[i] == 0.5f);
    }
}

/* ---- direct registration ---- */

static float *texture(size_t w, size_t h, uint32_t seed)
{
    float *v = malloc(w * h * sizeof *v), s = (float)seed;
    size_t i;
    for (i = 0; i < w * h; i++) {
        float x = (float)(i % w), y = (float)(i / w);
        float t = sinf(x * 0.21f + s) * cosf(y * 0.17f - s) +
                  sinf(x * 0.071f + y * 0.093f + s * 0.5f) * 0.7f +
                  cosf((x * 0.33f - y * 0.29f) * 1.3f + s) * 0.4f;
        v[i] = 0.5f + 0.2f * t;
    }
    return v;
}

static void test_pose_matrix_is_the_identity_and_shifts_in_pixels(void)
{
    pc_algo_pose id = PC_ALGO_POSE_IDENTITY, p = id, q = id;
    pc_algo_homography m = pc_algo_pose_matrix(&id, 640, 480);
    double x, y, v[PC_ALGO_POSE_N];
    bool fr[PC_ALGO_POSE_N];
    int k, n = 0;
    CHECK(pc_algo_homography_is_identity(&m));
    p.dx = 0.1, p.dy = -0.05;
    m = pc_algo_pose_matrix(&p, 200, 100);
    pc_algo_homography_apply(&m, 50.0, 20.0, &x, &y);
    CHECK(fabs(x - 70.0) < 1e-9 && fabs(y - 15.0) < 1e-9);
    q.scale = 1.5, q.rot = 0.3;
    m = pc_algo_pose_matrix(&q, 200, 100);
    pc_algo_homography_apply(&m, 100.0, 50.0, &x, &y);
    CHECK(fabs(x - 100.0) < 1e-9 && fabs(y - 50.0) < 1e-9);
    pc_algo_pose_to_vec(&q, v);
    p = pc_algo_pose_from_vec(v);
    CHECK(memcmp(&p, &q, sizeof p) == 0);
    pc_algo_pose_free(PC_ALGO_MOTION_TRANSLATION, fr);
    for (k = 0; k < PC_ALGO_POSE_N; k++) {
        n += fr[k];
    }
    CHECK(n == 2);
    pc_algo_pose_free(PC_ALGO_MOTION_HOMOGRAPHY, fr);
    for (k = 0; k < PC_ALGO_POSE_N; k++) {
        CHECK(fr[k]);
    }
}

static void test_direct_warp_by_the_identity_is_exact_and_marks_coverage(void)
{
    const size_t w = 40, h = 30;
    float *src = texture(w, h, 1), *o = NULL;
    bool *v = NULL, *valid = malloc(w * h);
    pc_algo_direct_plane p = {w, h, src, NULL};
    pc_algo_pose id = PC_ALGO_POSE_IDENTITY, sh = id;
    size_t i, y;
    CHECK(pc_algo_direct_warp(&p, &id, &o, &v, NULL) == PC_OK);
    for (i = 0; i < w * h; i++) {
        CHECK(fabsf(o[i] - src[i]) < 1e-6f && v[i]);
    }
    free(o), free(v);
    sh.dx = 10.0 / (double)w;
    CHECK(pc_algo_direct_warp(&p, &sh, &o, &v, NULL) == PC_OK);
    for (y = 0; y < h; y++) {
        CHECK(!v[y * w] && !v[y * w + 9] && v[y * w + 10]);
    }
    free(o), free(v);
    for (i = 0; i < w * h; i++) {
        valid[i] = i % w < 20;
    }
    p.valid = valid;
    CHECK(pc_algo_direct_warp(&p, &id, &o, &v, NULL) == PC_OK);
    CHECK(v[5 * w + 10] && !v[5 * w + 30]);
    free(o), free(v), free(valid), free(src);
}

static double bowl(void *ctx, const double *x)
{
    (void)ctx;
    return (x[0] - 0.3) * (x[0] - 0.3) + 2.0 * (x[1] + 0.2) * (x[1] + 0.2);
}

static void test_nelder_mead_finds_the_minimum_of_a_bowl(void)
{
    const double x0[2] = {0, 0}, lo[2] = {-1, -1}, hi[2] = {1, 1}, hi2[2] = {0.1, 1}, st[2] = {0.1, 0.1},
                 tol[2] = {1e-6, 1e-6};
    double r[2];
    CHECK(pc_algo_nelder_mead(bowl, NULL, 2, x0, lo, hi, st, tol, r));
    CHECK(fabs(r[0] - 0.3) < 1e-4 && fabs(r[1] + 0.2) < 1e-4);
    CHECK(pc_algo_nelder_mead(bowl, NULL, 2, x0, lo, hi2, st, tol, r));
    CHECK(fabs(r[0] - 0.1) < 1e-6);
    CHECK(pc_algo_nelder_mead(bowl, NULL, 0, x0, lo, hi, st, tol, r));
}

/* A cubic B-spline sample (as direct.c's), for building shifted targets. */
static float bspline_at(const float *src, size_t w, size_t h, double sx, double sy)
{
    double x0 = floor(sx), y0 = floor(sy), tx = sx - x0, ty = sy - y0, wx[4], wy[4], acc = 0.0;
    int i, j;
    double *ws[2] = {wx, wy}, ts[2] = {tx, ty};
    for (i = 0; i < 2; i++) {
        double t = ts[i];
        ws[i][0] = ((-1.0 / 3.0 * t + 0.8) * t - 0.46666667) * t;
        ws[i][1] = ((t - 1.8) * t - 0.2) * t + 1.0;
        ws[i][2] = ((1.2 - t) * t + 0.8) * t;
        ws[i][3] = ((1.0 / 3.0 * t - 0.2) * t - 0.13333334) * t;
    }
    for (j = 0; j < 4; j++) {
        int64_t yy = (int64_t)y0 + j - 1;
        double r = 0.0;
        yy = yy < 0 ? 0 : (yy > (int64_t)h - 1 ? (int64_t)h - 1 : yy);
        for (i = 0; i < 4; i++) {
            int64_t xx = (int64_t)x0 + i - 1;
            xx = xx < 0 ? 0 : (xx > (int64_t)w - 1 ? (int64_t)w - 1 : xx);
            r += wx[i] * (double)src[(size_t)yy * w + (size_t)xx];
        }
        acc += wy[j] * r;
    }
    return (float)acc;
}

static void test_direct_registers_a_similarity_and_a_translation(void)
{
    const size_t w = 320, h = 240;
    float *ref = texture(w, h, 3), *tgt = malloc(w * h * sizeof *tgt), *doubled = malloc(w * h * sizeof *tgt);
    pc_algo_pose truth = PC_ALGO_POSE_IDENTITY;
    pc_algo_homography m;
    pc_algo_direct_plane pr = {w, h, ref, NULL}, pt = {w, h, tgt, NULL};
    pc_algo_registration r;
    size_t i;
    truth.dx = 3.3 / (double)w, truth.dy = -2.1 / (double)h, truth.scale = 1.012;
    truth.rot = 0.6 * 3.14159265358979323846 / 180.0;
    m = pc_algo_pose_matrix(&truth, w, h);
    for (i = 0; i < w * h; i++) {
        double x, y;
        pc_algo_homography_apply(&m, (double)(i % w), (double)(i / w), &x, &y);
        tgt[i] =
            bspline_at(ref, w, h, fmin(fmax(x, 0.0), (double)(w - 1)), fmin(fmax(y, 0.0), (double)(h - 1)));
    }
    CHECK(pc_algo_direct_register(&pr, &pt, PC_ALGO_MOTION_SIMILARITY, PC_ALGO_POSE_IDENTITY, 0, &r, NULL) ==
          PC_OK);
    {
        double tx, ty, rx, ry;
        pc_algo_homography_apply(&m, 0.0, 0.0, &tx, &ty);
        pc_algo_homography_apply(&r.h, 0.0, 0.0, &rx, &ry);
        CHECK(hypot(tx - rx, ty - ry) < 0.15);
        pc_algo_homography_apply(&m, (double)w, (double)h, &tx, &ty);
        pc_algo_homography_apply(&r.h, (double)w, (double)h, &rx, &ry);
        CHECK(hypot(tx - rx, ty - ry) < 0.15);
    }
    CHECK(r.rms < 0.01 && r.overlap > w * h / 2);
    /* Translation only: the shift is found, the rest stays put. */
    for (i = 0; i < w * h; i++) {
        tgt[i] = bspline_at(ref, w, h, fmin((double)(i % w) + 4.0, (double)(w - 1)),
                            fmin((double)(i / w) + 3.0, (double)(h - 1)));
    }
    CHECK(pc_algo_direct_register(&pr, &pt, PC_ALGO_MOTION_TRANSLATION, PC_ALGO_POSE_IDENTITY, 0, &r, NULL) ==
          PC_OK);
    CHECK(fabs(r.pose.dx * (double)w - 4.0) < 0.1 && fabs(r.pose.dy * (double)h - 3.0) < 0.1);
    CHECK(r.pose.scale == 1.0 && r.pose.rot == 0.0);
    /* A doubled exposure correlates fully at the truth. */
    for (i = 0; i < w * h; i++) {
        doubled[i] = ref[i] * 2.0f;
    }
    pt.luma = doubled;
    CHECK(pc_algo_direct_register(&pr, &pt, PC_ALGO_MOTION_SIMILARITY, PC_ALGO_POSE_IDENTITY, 0, &r, NULL) ==
          PC_OK);
    CHECK(r.correlation > 0.999);
    /* Hostile: mismatched or empty planes are refused. */
    pt.w = w - 1;
    CHECK(pc_algo_direct_register(&pr, &pt, PC_ALGO_MOTION_SIMILARITY, PC_ALGO_POSE_IDENTITY, 0, &r, NULL) ==
          PC_ERR_INVALID);
    free(ref), free(tgt), free(doubled);
}

/* ---- Camera Raw local tone and Whites/Blacks ---- */

static float (*grey_image(size_t w, size_t h, int kind))[4]
{
    float (*px)[4] = malloc(w * h * sizeof *px);
    size_t i;
    for (i = 0; i < w * h; i++) {
        size_t x = i % w, y = i / w;
        float v;
        if (kind == 0) {
            v = 0.4f;
        } else if (kind == 1) {
            v = x < w / 2 ? 0.1f : 0.6f;
        } else {
            v = (x >= 64 && x < 128 && y >= 64 && y < 128) ? 0.8f : 0.2f;
        }
        px[i][0] = px[i][1] = px[i][2] = v, px[i][3] = 1.0f;
    }
    return px;
}

static void test_local_tone_flat_step_and_bright_patch(void)
{
    float (*px)[4];
    size_t i, w, h;
    /* A flat image is unchanged. */
    px = grey_image(64, 48, 0);
    CHECK(pc_algo_cr_local_tone(px, 64, 48, 100.0f, -100.0f, pc_algo_cr_encoding_of(0.0f), NULL) == PC_OK);
    for (i = 0; i < 64 * 48; i++) {
        CHECK(fabsf(px[i][0] - 0.4f) < 1e-5f);
    }
    free(px);
    /* Shadows raise a dark area without a halo; the bright half stays. */
    w = 256, h = 128;
    px = grey_image(w, h, 1);
    CHECK(pc_algo_cr_local_tone(px, w, h, 100.0f, 0.0f, pc_algo_cr_encoding_of(0.0f), NULL) == PC_OK);
    {
        const float (*row)[4] = (const float (*)[4])px + 64 * w;
        CHECK(fabsf(row[w / 2 + 1][0] - 0.6f) < 0.002f && fabsf(row[w - 5][0] - 0.6f) < 0.002f);
        CHECK(fabsf(row[10][0] - 0.1f) < 0.004f && fabsf(row[10][0] - row[w / 2 - 1][0]) < 0.004f);
    }
    free(px);
    /* Highlights lower a bright area and keep the darkest. */
    w = h = 192;
    px = grey_image(w, h, 2);
    CHECK(pc_algo_cr_local_tone(px, w, h, 0.0f, -100.0f, pc_algo_cr_encoding_of(0.0f), NULL) == PC_OK);
    CHECK(px[96 * w + 96][0] < 0.75f);
    CHECK(fabsf(px[5 * w + 5][0] - 0.2f) < 0.01f);
    free(px);
    /* Degenerate sizes do nothing. */
    px = grey_image(1, 1, 0);
    CHECK(pc_algo_cr_local_tone(px, 1, 1, 50.0f, 50.0f, 0.0f, NULL) == PC_OK && px[0][0] == 0.4f);
    free(px);
}

static void test_whites_blacks_match_photoshop_and_adapt(void)
{
    float xw, xb;
    CHECK(fabsf(pc_algo_cr_whites_curve(97.1f / 255.0f, 100.0f) * 255.0f - 137.4f) < 0.6f);
    CHECK(fabsf(pc_algo_cr_whites_curve(194.3f / 255.0f, -100.0f) * 255.0f - 167.6f) < 0.6f);
    CHECK(fabsf(pc_algo_cr_blacks_curve(97.1f / 255.0f, -100.0f) * 255.0f - 60.0f) < 0.6f);
    CHECK(fabsf(pc_algo_cr_blacks_curve(48.6f / 255.0f, 100.0f) * 255.0f - 67.0f) < 0.6f);
    CHECK(pc_algo_cr_whites_curve(1.0f, -100.0f) == 1.0f);
    CHECK(pc_algo_cr_blacks_curve(0.0f, 100.0f) == 0.0f);
    pc_algo_cr_wb_amounts(50.0f, 0.0f, 0.05f, 0.6f, &xw, &xb);
    CHECK(fabsf(xw - 94.5f) < 2.0f);
    pc_algo_cr_wb_amounts(0.0f, -40.0f, 0.6f, 1.0f, &xw, &xb);
    CHECK(fabsf(xb + 58.0f) < 1.0f);
    pc_algo_cr_wb_amounts(0.0f, -50.0f, 0.0f, 1.0f, &xw, &xb);
    CHECK(fabsf(xb + 60.0f) < 1e-3f);
    pc_algo_cr_wb_amounts(-50.0f, 50.0f, 0.3f, 0.6f, &xw, &xb);
    CHECK(xw == -50.0f && xb == 50.0f);
    /* Hostile: NaN levels and amounts never crash. */
    (void)pc_algo_cr_whites_curve(NAN, NAN);
    pc_algo_cr_wb_amounts(NAN, NAN, NAN, NAN, &xw, &xb);
}

static void test_camera_raw_encoding_round_trips(void)
{
    pc_algo_cr_encoding s = pc_algo_cr_encoding_of(0.0f), p = pc_algo_cr_encoding_of(1.8f);
    CHECK(s == 0.0f && p == 1.8f);
    CHECK(pc_algo_cr_encoding_of(NAN) == 0.0f && pc_algo_cr_encoding_of(9.0f) == 4.0f);
    CHECK(fabsf(pc_algo_cr_encode(p, pc_algo_cr_decode(p, 0.5f)) - 0.5f) < 1e-6f);
    CHECK(fabsf(pc_algo_cr_encode(s, pc_algo_cr_decode(s, 0.5f)) - 0.5f) < 1e-6f);
}

int main(int argc, char **argv)
{
    RUN(test_the_key_colour_becomes_fully_transparent);
    RUN(test_grey_on_white_is_black_with_partial_alpha);
    RUN(test_unmixed_colour_composited_over_the_key_gives_back_the_pixel);
    RUN(test_thresholds_clip_and_stretch_the_opacity);
    RUN(test_hdr_and_non_finite_samples_stay_opaque_and_finite);
    RUN(test_color_to_alpha_works_at_every_depth_and_in_gray);
    RUN(test_coloured_key_and_surfaces_without_alpha);
    RUN(test_fast_paths_match_the_direct_searches);
    RUN(test_trap_matches_the_direct_window_maximum);
    RUN(test_trap_lines_with_negative_zero_and_specials_match);
    RUN(test_trap_refuses_a_malformed_buffer);
    RUN(test_pose_matrix_is_the_identity_and_shifts_in_pixels);
    RUN(test_direct_warp_by_the_identity_is_exact_and_marks_coverage);
    RUN(test_nelder_mead_finds_the_minimum_of_a_bowl);
    RUN(test_direct_registers_a_similarity_and_a_translation);
    RUN(test_local_tone_flat_step_and_bright_patch);
    RUN(test_whites_blacks_match_photoshop_and_adapt);
    RUN(test_camera_raw_encoding_round_trips);
    return pc_t_finish();
}
