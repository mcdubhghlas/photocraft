/* Mosaic filter tests (split from test_fx.c to keep files short): the reference's legacy
 * oracle, depth/colour/alpha preservation, tile independence and zero channels. */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "base/num.h"
#include "raster/pixel.h"
#include "harness.h"

/* The previous traversal, kept as the value oracle (alpha math included). */
static void legacy_mosaic(const pc_algo_image *src, pc_rect out, const pc_algo_ctx *ctx, float cell,
                          float *res)
{
    size_t n = src->ch, k = 0, c;
    int32_t cs = (int32_t)roundf(cell > 1.0f ? cell : 1.0f), x, y, ccx = 0, ccy = 0;
    pc_rect b = ctx->bounds;
    bool have = false;
    float acc[8];
    for (y = out.y0; y < out.y1; y++) {
        for (x = out.x0; x < out.x1; x++) {
            int32_t cx = (x - b.x0 - ((x - b.x0) % cs + cs) % cs) / cs,
                    cy = (y - b.y0 - ((y - b.y0) % cs + cs) % cs) / cs;
            if (!have || cx != ccx || cy != ccy) {
                pc_rect r = pc_rect_intersect(
                    pc_rect_make(b.x0 + cx * cs, b.y0 + cy * cs, b.x0 + (cx + 1) * cs, b.y0 + (cy + 1) * cs),
                    b);
                float count = 0.0f;
                int32_t xx, yy;
                memset(acc, 0, sizeof acc);
                for (yy = r.y0; yy < r.y1; yy++) {
                    for (xx = r.x0; xx < r.x1; xx++) {
                        float a = ctx->alpha ? pc_algo_image_get(src, xx, yy, n - 1) : 1.0f;
                        for (c = 0; c < n; c++) {
                            float s = pc_algo_image_get(src, xx, yy, c);
                            acc[c] += ctx->alpha && c < n - 1 ? s * a : s;
                        }
                        count += 1.0f;
                    }
                }
                if (count > 0.0f) {
                    for (c = 0; c < n; c++) {
                        acc[c] /= count;
                    }
                }
                if (ctx->alpha) {
                    float a = acc[n - 1];
                    for (c = 0; c + 1 < n; c++) {
                        acc[c] = a > 1e-7f ? acc[c] / a : 0.0f;
                    }
                }
                have = true, ccx = cx, ccy = cy;
            }
            for (c = 0; c < n; c++) {
                res[k++] = acc[c];
            }
        }
    }
}

static void image_pattern(pc_algo_image *img, pc_rect rect, size_t ch)
{
    uint32_t seed = 0x72e1905u;
    size_t i;
    CHECK(pc_algo_image_init(img, rect, ch, NULL) == PC_OK);
    for (i = 0; i < img->len; i++) {
        seed = seed * 1664525u + 1013904223u;
        img->data[i] = i % ch == ch - 1 && i % 11 == 0 ? 0.0f : (float)(seed >> 8) / 16777215.0f;
    }
}

static void test_mosaic_matches_legacy_for_partial_cells_and_offset_bounds(void)
{
    const pc_rect bs[2] = {{0, 0, 47, 61}, {-5, 7, 42, 68}};
    const float sizes[9] = {0.0f, 1.0f, 1.5f, 2.0f, 7.0f, 13.0f, 50.0f, 80.0f, 180.0f};
    size_t bi, ch, si, oi, ri;
    for (bi = 0; bi < 2; bi++) {
        pc_rect b = bs[bi];
        pc_rect outs[7] = {b,
                           pc_rect_make(b.x0 + 4, b.y0 + 2, b.x1 - 3, b.y1 - 4),
                           pc_rect_make(b.x0 + 9, b.y0 + 9, b.x0 + 16, b.y0 + 17),
                           pc_rect_make(b.x0 - 3, b.y0 - 4, b.x1 + 6, b.y1 + 8),
                           pc_rect_make(b.x1 + 2, b.y1 - 3, b.x1 + 12, b.y1 + 5),
                           pc_rect_make(b.x0, b.y0, b.x0, b.y1),
                           pc_rect_make(b.x0, b.y0, b.x1, b.y0)};
        pc_rect rects[2] = {b, pc_rect_make(b.x0 + 3, b.y0 + 5, b.x1 - 6, b.y1 - 8)};
        for (ch = 1; ch <= 5; ch++) {
            int al;
            for (al = 0; al < 2; al++) {
                for (ri = 0; ri < 2; ri++) {
                    pc_algo_image src;
                    pc_algo_ctx ctx = {b, PC_COLOR_MODE_RGB, al != 0};
                    image_pattern(&src, rects[ri], ch);
                    for (oi = 0; oi < 7; oi++) {
                        for (si = 0; si < 9; si++) {
                            pc_algo_mosaic_params mp = {sizes[si]};
                            pc_algo_filter f;
                            float *got = NULL, *want;
                            size_t len = 0,
                                   wn = (size_t)pc_rect_width(outs[oi]) * pc_rect_height(outs[oi]) * ch;
                            CHECK(pc_algo_filter_make(&pc_algo_def_mosaic, &mp, &f, NULL) == PC_OK);
                            CHECK(pc_algo_kernel(&f, &src, outs[oi], &ctx, &got, &len, NULL) == PC_OK);
                            want = malloc(wn * sizeof(float) + 4);
                            legacy_mosaic(&src, outs[oi], &ctx, sizes[si], want);
                            CHECK_EQ_UINT(len, wn);
                            if (len == wn && wn > 0 && memcmp(got, want, wn * sizeof(float)) != 0) {
                                PC_T_FAIL("mosaic differs: b %zu ch %zu alpha %d out %zu cell %g", bi, ch, al,
                                          oi, (double)sizes[si]);
                            }
                            free(got), free(want);
                            pc_algo_filter_fini(&f);
                        }
                    }
                    pc_algo_image_fini(&src);
                }
            }
        }
    }
}

static void test_mosaic_preserves_depth_color_alpha_and_tile_independence(void)
{
    const pc_rect b = {13, -7, 92, 54};
    const pc_sample_type st[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const pc_color_mode md[4] = {PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_RGB, PC_COLOR_MODE_CMYK,
                                 PC_COLOR_MODE_LAB};
    const float cells[4] = {6.0f, 13.0f, 50.0f, 180.0f};
    size_t si, mi, ci;
    int al;
    for (si = 0; si < 3; si++) {
        for (mi = 0; mi < 4; mi++) {
            for (al = 0; al < 2; al++) {
                pc_pixel_format fmt = {md[mi], st[si], al != 0};
                size_t n = pc_pixel_format_channels(fmt);
                pc_algo_image pat, src;
                pc_surface input;
                pc_algo_ctx ctx = {b, md[mi], al != 0};
                image_pattern(&pat, b, n);
                pc_surface_init(&input, fmt);
                CHECK(pc_surface_write_region(&input, b, pat.data, pat.len, NULL) == PC_OK);
                CHECK(pc_algo_image_read(&input, b, &src, NULL) == PC_OK);
                for (ci = 0; ci < 4; ci++) {
                    pc_algo_mosaic_params mp = {cells[ci]};
                    pc_algo_filter f;
                    pc_surface expected;
                    float *want = malloc(src.len * sizeof(float) + 4);
                    uint8_t *eb = NULL;
                    size_t el = 0;
                    int t, e;
                    legacy_mosaic(&src, b, &ctx, cells[ci], want);
                    pc_surface_init(&expected, fmt);
                    CHECK(pc_surface_write_region(&expected, b, want, src.len, NULL) == PC_OK);
                    CHECK(pc_surface_to_interleaved(&expected, b, &eb, &el, NULL) == PC_OK);
                    CHECK(pc_algo_filter_make(&pc_algo_def_mosaic, &mp, &f, NULL) == PC_OK);
                    for (t = 0; t < 2; t++) {
                        for (e = 0; e < 2; e++) {
                            pc_surface got;
                            uint8_t *gb = NULL;
                            size_t gl = 0;
                            CHECK(pc_algo_apply_tiled(&input, &f, b, b, NULL, t ? 256 : 17, e ? &b : NULL,
                                                      &got, NULL) == PC_OK);
                            CHECK(pc_surface_to_interleaved(&got, b, &gb, &gl, NULL) == PC_OK);
                            CHECK(gl == el && memcmp(gb, eb, el) == 0);
                            free(gb);
                            pc_surface_fini(&got);
                        }
                    }
                    free(eb), free(want);
                    pc_surface_fini(&expected);
                    pc_algo_filter_fini(&f);
                }
                pc_algo_image_fini(&pat), pc_algo_image_fini(&src);
                pc_surface_fini(&input);
            }
        }
    }
}

/* The four-channel row reads keep HDR values, negative zeros, subnormals and zero alpha bit-exact,
 * with the source covering all or part of the bounds. */
static void test_mosaic_row_reads_preserve_hdr_alpha_and_partial_source_bits(void)
{
    const pc_rect bounds = {-7, 3, 30, 26};
    const pc_rect rects[2] = {{-7, 3, 30, 26}, {-3, 7, 23, 21}};
    const float cells[5] = {2.0f, 4.0f, 10.0f, 50.0f, 180.0f};
    size_t ri, i, ci;
    int al;
    for (ri = 0; ri < 2; ri++) {
        pc_algo_image src;
        image_pattern(&src, rects[ri], 4);
        for (i = 0; i < src.len / 4; i++) {
            float *p = src.data + i * 4;
            p[0] = p[0] * 8.0f - 4.0f;
            p[1] = i % 3 == 0 ? -0.0f : p[1] * 16.0f;
            p[2] = i % 7 == 0 ? FLT_MIN : -p[2];
            p[3] = i % 5 == 0 ? 0.0f : p[3];
        }
        for (al = 0; al < 2; al++) {
            pc_algo_ctx ctx = {bounds, PC_COLOR_MODE_RGB, al != 0};
            for (ci = 0; ci < 5; ci++) {
                pc_algo_mosaic_params mp = {cells[ci]};
                pc_algo_filter f;
                size_t len = 0, wn = (size_t)pc_rect_width(bounds) * pc_rect_height(bounds) * 4;
                float *got = NULL, *want = malloc(wn * sizeof(float));
                CHECK(pc_algo_filter_make(&pc_algo_def_mosaic, &mp, &f, NULL) == PC_OK);
                CHECK(pc_algo_kernel(&f, &src, bounds, &ctx, &got, &len, NULL) == PC_OK);
                legacy_mosaic(&src, bounds, &ctx, cells[ci], want);
                CHECK_EQ_UINT(len, wn);
                CHECK(len == wn && memcmp(got, want, wn * sizeof(float)) == 0);
                free(got), free(want);
                pc_algo_filter_fini(&f);
            }
        }
        pc_algo_image_fini(&src);
    }
}

static void test_mosaic_handles_zero_channels(void)
{
    pc_algo_image src = {{0, 0, 3, 3}, 0, NULL, 0};
    int al;
    for (al = 0; al < 2; al++) {
        pc_algo_ctx ctx = {src.rect, PC_COLOR_MODE_RGB, al != 0};
        pc_algo_mosaic_params mp = {2.0f};
        pc_algo_filter f;
        float *d = NULL;
        size_t len = 7;
        CHECK(pc_algo_filter_make(&pc_algo_def_mosaic, &mp, &f, NULL) == PC_OK);
        CHECK(pc_algo_kernel(&f, &src, src.rect, &ctx, &d, &len, NULL) == PC_OK);
        CHECK_EQ_UINT(len, 0);
        free(d);
        pc_algo_filter_fini(&f);
    }
}

int main(int argc, char **argv)
{
    RUN(test_mosaic_matches_legacy_for_partial_cells_and_offset_bounds);
    RUN(test_mosaic_preserves_depth_color_alpha_and_tile_independence);
    RUN(test_mosaic_row_reads_preserve_hdr_alpha_and_partial_source_bits);
    RUN(test_mosaic_handles_zero_channels);
    return pc_t_finish();
}
