/* Filter Gallery, render2, Trap and mesh warps: ports of the #[test]s in
 * crates/algo/src/{artistic_fx.rs, render2.rs, trap.rs, warp.rs}. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/algo.h"
#include "harness.h"

/* ---- Filter Gallery (artistic_fx.rs) ---- */

static void gpattern(pc_surface *s, pc_sample_type st)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, st, true};
    int32_t x, y;
    pc_surface_init(s, f);
    for (y = 0; y < 40; y++) {
        for (x = 0; x < 56; x++) {
            float v = (float)((x * 7 + y * 3) % 23) / 22.0f;
            float a[4] = {v, 0.3f, 1.0f - v, 1.0f}, b[4] = {0.9f, v * 0.5f, 0.2f, 1.0f};
            CHECK(pc_surface_write_pixel(s, x, y, (x / 9 + y / 7) % 2 == 0 ? a : b, 4, NULL) == PC_OK);
        }
    }
}

static float *run_effect(const pc_surface *s, const pc_algo_gallery_effect *e, int32_t tile, size_t *n)
{
    pc_rect b = pc_rect_make(0, 0, 56, 40);
    pc_algo_filter_gallery_params p = {{(pc_algo_gallery_effect *)(void *)(uintptr_t)e, 1}};
    pc_algo_filter f;
    pc_surface o;
    float *v = NULL;
    CHECK(pc_algo_filter_make(&pc_algo_def_filter_gallery, &p, &f, NULL) == PC_OK);
    CHECK(pc_algo_apply_tiled(s, &f, b, b, NULL, tile, &b, &o, NULL) == PC_OK);
    CHECK(pc_surface_read_region(&o, b, &v, n, NULL) == PC_OK);
    pc_surface_fini(&o);
    pc_algo_filter_fini(&f);
    return v;
}

static float worst(const float *a, const float *b, size_t n)
{
    float m = 0.0f;
    size_t i;
    for (i = 0; i < n; i++) {
        float d = fabsf(a[i] - b[i]);
        m = d > m ? d : m;
    }
    return m;
}

static void test_every_effect_changes_pixels_and_is_tile_independent(void)
{
    const pc_sample_type sts[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    size_t si, fi, n, i;
    for (si = 0; si < 3; si++) {
        pc_surface s;
        float *before;
        gpattern(&s, sts[si]);
        CHECK(pc_surface_read_region(&s, pc_rect_make(0, 0, 56, 40), &before, &n, NULL) == PC_OK);
        for (fi = 0; fi < PC_ALGO_GALLERY_FILTERS; fi++) {
            pc_algo_gallery_effect e;
            float *a;
            size_t na;
            CHECK(pc_algo_gallery_effect_new((pc_algo_gallery_filter)fi, &e, NULL) == PC_OK);
            a = run_effect(&s, &e, 256, &na);
            if (na == n && memcmp(a, before, n * sizeof(float)) == 0) {
                PC_T_FAIL("%s changed nothing", pc_algo_gallery_filter_key(e.filter));
            }
            for (i = 0; i < na; i++) {
                if (!isfinite(a[i])) {
                    PC_T_FAIL("%s not finite", pc_algo_gallery_filter_key(e.filter));
                    break;
                }
            }
            if (sts[si] == PC_SAMPLE_TYPE_F32) {
                size_t nt;
                float *t = run_effect(&s, &e, 16, &nt);
                if (worst(a, t, na) >= 1e-3f) {
                    PC_T_FAIL("%s differs across tilings", pc_algo_gallery_filter_key(e.filter));
                }
                free(t);
            }
            free(a);
            pc_algo_gallery_effect_fini(&e);
        }
        free(before);
        pc_surface_fini(&s);
    }
}

static void test_rough_pastels_and_angled_strokes_respond_to_every_slider(void)
{
    const pc_algo_gallery_filter fs[2] = {PC_ALGO_GAL_ROUGH_PASTELS, PC_ALGO_GAL_ANGLED_STROKES};
    pc_surface s;
    size_t k, i, n;
    gpattern(&s, PC_SAMPLE_TYPE_F32);
    for (k = 0; k < 2; k++) {
        pc_algo_gallery_effect e0;
        pc_algo_gallery_param ps[PC_ALGO_GALLERY_MAX_PARAMS];
        size_t np = pc_algo_gallery_params(fs[k], ps, PC_ALGO_GALLERY_MAX_PARAMS);
        float *base;
        CHECK(pc_algo_gallery_effect_new(fs[k], &e0, NULL) == PC_OK);
        base = run_effect(&s, &e0, 256, &n);
        for (i = 0; i < np; i++) {
            int m;
            if (ps[i].kind != PC_ALGO_GALLERY_RANGE) {
                continue;
            }
            for (m = 0; m < 2; m++) {
                pc_algo_gallery_effect e;
                float *a, *t;
                size_t na, nt;
                CHECK(pc_algo_gallery_effect_new(fs[k], &e, NULL) == PC_OK);
                CHECK(pc_algo_gallery_effect_set(&e, ps[i].key, m ? ps[i].max : ps[i].min, NULL) == PC_OK);
                a = run_effect(&s, &e, 256, &na);
                t = run_effect(&s, &e, 16, &nt);
                if (memcmp(a, base, n * sizeof(float)) == 0) {
                    PC_T_FAIL("%s %s changed nothing", pc_algo_gallery_filter_key(fs[k]), ps[i].key);
                }
                CHECK(worst(a, t, na) < 1e-3f);
                free(a), free(t);
                pc_algo_gallery_effect_fini(&e);
            }
        }
        free(base);
        pc_algo_gallery_effect_fini(&e0);
    }
    pc_surface_fini(&s);
}

static void test_params_parse_and_clamp(void)
{
    size_t i;
    pc_algo_gallery_effect e;
    char cb[PC_ALGO_GALLERY_NAME_MAX];
    bool ok = false;
    for (i = 0; i < PC_ALGO_GALLERY_FILTERS; i++) {
        pc_algo_gallery_param ps[PC_ALGO_GALLERY_MAX_PARAMS];
        pc_algo_gallery_filter f = (pc_algo_gallery_filter)i, g;
        const char *k = pc_algo_gallery_filter_key(f), *id = pc_algo_gallery_filter_command_id(f);
        CHECK(pc_algo_gallery_params(f, ps, PC_ALGO_GALLERY_MAX_PARAMS) > 0);
        CHECK(pc_algo_gallery_filter_from_key(k, strlen(k), &g) && g == f);
        CHECK(pc_algo_gallery_filter_from_key(id, strlen(id), &g) && g == f);
    }
    CHECK(pc_algo_gallery_effect_new(PC_ALGO_GAL_TEXTURIZER, &e, NULL) == PC_OK);
    CHECK(strcmp(pc_algo_gallery_effect_choice(&e, "texture", cb), "canvas") == 0);
    CHECK(strcmp(pc_algo_gallery_effect_choice(&e, "light", cb), "top") == 0);
    CHECK(pc_algo_gallery_effect_set_choice(&e, "texture", "brick", &ok, NULL) == PC_OK && ok);
    CHECK(strcmp(pc_algo_gallery_effect_choice(&e, "texture", cb), "brick") == 0);
    CHECK(pc_algo_gallery_effect_set(&e, "relief", 500.0f, NULL) == PC_OK);
    CHECK(pc_algo_gallery_effect_get(&e, "relief") == 50.0f);
    pc_algo_gallery_effect_fini(&e);
}

/* ---- render2.rs ---- */

static void test_composite_draws_a_disc_and_respects_the_clip(void)
{
    pc_surface s;
    const float c[2] = {20.0f, 20.0f}, red[4] = {1, 0, 0, 1};
    pc_algo_prim p = pc_algo_prim_disc(c, 8.0f, red);
    pc_rect used;
    float q[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_algo_render_composite(&s, &p, 1, pc_rect_make(0, 0, 20, 40), NULL, &used, NULL) == PC_OK);
    CHECK(used.x1 == 20);
    pc_surface_read_pixel(&s, 18, 20, q);
    CHECK(q[0] > 0.99f && q[3] > 0.99f);
    pc_surface_read_pixel(&s, 22, 20, q);
    CHECK(q[3] == 0.0f);
    pc_surface_read_pixel(&s, 2, 2, q);
    CHECK(q[3] == 0.0f);
    pc_surface_fini(&s);
}

static bool prims_eq(const pc_algo_prims *a, const pc_algo_prims *b)
{
    return a->n == b->n && (a->n == 0 || memcmp(a->v, b->v, a->n * sizeof *a->v) == 0);
}

static void test_renderers_are_deterministic_and_seeded(void)
{
    pc_rect canvas = pc_rect_make(0, 0, 400, 300);
    pc_algo_tree_spec d = pc_algo_tree_spec_default(), s7 = d;
    pc_algo_prims a, b, c;
    float path[33][2];
    pc_algo_polyline pl;
    uint32_t k;
    s7.seed = 7;
    CHECK(pc_algo_tree(&d, canvas, &a, NULL) == PC_OK);
    CHECK(pc_algo_tree(&d, canvas, &b, NULL) == PC_OK);
    CHECK(pc_algo_tree(&s7, canvas, &c, NULL) == PC_OK);
    CHECK(prims_eq(&a, &b) && !prims_eq(&a, &c));
    pc_algo_prims_fini(&a), pc_algo_prims_fini(&b), pc_algo_prims_fini(&c);
    for (k = 1; k <= PC_ALGO_TREE_TYPES; k++) {
        pc_algo_tree_spec t = d;
        t.base_tree_type = k;
        CHECK(pc_algo_tree(&t, canvas, &a, NULL) == PC_OK && a.n > 0);
        pc_algo_prims_fini(&a);
    }
    for (k = 1; k <= PC_ALGO_FRAME_STYLES; k++) {
        pc_algo_frame_spec f = pc_algo_frame_spec_default();
        f.frame = k;
        CHECK(pc_algo_picture_frame(&f, canvas, &a, NULL) == PC_OK && a.n > 0);
        pc_algo_prims_fini(&a);
    }
    pc_algo_default_flame_path(canvas, path);
    pl.v = path, pl.n = 33;
    for (k = 0; k < 6; k++) {
        pc_algo_flame_spec f = pc_algo_flame_spec_default();
        f.flame_type = (pc_algo_flame_type)k;
        CHECK(pc_algo_flame(&f, &pl, 1, &a, NULL) == PC_OK && a.n > 0);
        pc_algo_prims_fini(&a);
    }
}

static void test_renderers_refuse_absurd_canvases(void)
{
    pc_algo_frame_spec f = pc_algo_frame_spec_default();
    pc_algo_prims a;
    /* An extreme aspect ratio would need billions of ornaments. */
    CHECK(pc_algo_picture_frame(&f, pc_rect_make(0, 0, INT32_MAX, 8), &a, NULL) == PC_ERR_LIMIT);
}

/* ---- trap.rs ---- */

static float *two_blocks(void)
{
    float *px = calloc(8 * 4 * 4, sizeof(float));
    size_t x, y;
    for (y = 0; px != NULL && y < 4; y++) {
        for (x = 0; x < 8; x++) {
            px[(y * 8 + x) * 4 + (x < 4 ? 0 : 1)] = 1.0f;
        }
    }
    return px;
}

static void test_traps_the_internal_colour_edge(void)
{
    float *px = two_blocks();
    CHECK(pc_algo_trap(px, 128, 8, 4, 4, 1, NULL) == PC_OK);
    CHECK(px[(0 * 8 + 3) * 4 + 1] > 0.5f);
    CHECK(px[(0 * 8 + 4) * 4 + 0] > 0.5f);
    CHECK(px[(0 * 8 + 0) * 4 + 1] == 0.0f);
    CHECK(px[(0 * 8 + 7) * 4 + 0] == 0.0f);
    free(px);
}

static void test_leaves_paper_untouched(void)
{
    float px[6 * 3 * 4] = {0}, before[6 * 3 * 4];
    size_t i;
    px[(6 + 3) * 4] = 1.0f;
    memcpy(before, px, sizeof px);
    CHECK(pc_algo_trap(px, 72, 6, 3, 4, 1, NULL) == PC_OK);
    for (i = 0; i < 18; i++) {
        if (before[i * 4] == 0.0f) {
            CHECK(px[i * 4] == 0.0f);
        }
    }
}

static void test_width_zero_is_identity(void)
{
    float *px = two_blocks(), *orig = two_blocks();
    CHECK(pc_algo_trap(px, 128, 8, 4, 4, 0, NULL) == PC_OK);
    CHECK(memcmp(px, orig, 128 * sizeof(float)) == 0);
    CHECK(pc_algo_trap(px, 127, 8, 4, 4, 1, NULL) == PC_ERR_INVALID);
    free(px), free(orig);
}

/* ---- warp.rs ---- */

static void wsample(pc_surface *s, pc_sample_type st)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, st, true};
    int32_t x, y;
    pc_surface_init(s, f);
    for (y = 0; y < 24; y++) {
        for (x = 0; x < 40; x++) {
            float v = (float)((x * 7 + y * 13) % 17) / 16.0f, p[4] = {v, 1.0f - v, (float)x / 40.0f, 1.0f};
            CHECK(pc_surface_write_pixel(s, x + 5, y + 3, p, 4, NULL) == PC_OK);
        }
    }
}

static void shift_map(void *ctx, double x, double y, double *ox, double *oy)
{
    const double *d = ctx;
    *ox = x + d[0];
    *oy = y + d[1];
}

static void warp_map(void *ctx, double x, double y, double *ox, double *oy)
{
    pc_point p = pc_warp_map(ctx, x, y);
    *ox = p.x, *oy = p.y;
}

static void test_a_mesh_warp_far_off_canvas_writes_only_the_two_regions(void)
{
    const pc_sample_type sts[2] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_F32};
    const double ds[3] = {100000.0, -100000.0, 300.0};
    const float one = 1.0f;
    size_t si, di;
    for (si = 0; si < 2; si++) {
        pc_pixel_format f = {PC_COLOR_MODE_GRAYSCALE, sts[si], false};
        pc_surface sel;
        pc_surface_init(&sel, f);
        CHECK(pc_surface_fill_rect(&sel, pc_rect_make(8, 8, 56, 56), &one, 1, NULL) == PC_OK);
        for (di = 0; di < 3; di++) {
            double d[2] = {ds[di], ds[di]};
            int32_t k = (int32_t)ds[di];
            pc_surface out;
            CHECK(pc_algo_warp_mesh_gray(&sel, shift_map, d, PC_ALGO_INTERP_BILINEAR, &out, NULL) == PC_OK);
            CHECK(pc_rect_eq(pc_surface_content_bounds(&out), pc_rect_make(8 + k, 8 + k, 56 + k, 56 + k)));
            CHECK(pc_surface_sample_channel(&out, 30 + k, 30 + k, 0) == 1.0f);
            CHECK(pc_surface_sample_channel(&out, 30, 30, 0) == 0.0f);
            pc_surface_fini(&out);
        }
        pc_surface_fini(&sel);
    }
}

static void test_identity_warp_is_a_no_op_at_every_depth(void)
{
    const pc_sample_type sts[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    size_t si, wi, ii;
    for (si = 0; si < 3; si++) {
        pc_surface src;
        pc_rect r;
        double b[4];
        wsample(&src, sts[si]);
        r = pc_surface_content_bounds(&src);
        b[0] = r.x0, b[1] = r.y0, b[2] = r.x1, b[3] = r.y1;
        for (wi = 0; wi < 3; wi++) {
            pc_warp w;
            if (wi == 0) {
                pc_warp_none(&w, b);
            } else if (wi == 1) {
                pc_mesh m;
                CHECK(pc_mesh_identity(b, 1, 1, &m, NULL) == PC_OK);
                pc_warp_custom(&w, &m, b);
            } else {
                pc_warp_preset(&w, PC_WARP_ARC, 0.0, b);
            }
            for (ii = 0; ii < 3; ii++) {
                pc_surface out;
                float *a = NULL, *o = NULL;
                size_t na, no, i;
                float wst = 0.0f;
                CHECK(pc_algo_warp_mesh_surface(&src, r, warp_map, &w, (pc_algo_interp)ii, &out, NULL) ==
                      PC_OK);
                CHECK(pc_rect_eq(pc_surface_content_bounds(&out), r));
                CHECK(pc_surface_read_region(&src, r, &a, &na, NULL) == PC_OK);
                CHECK(pc_surface_read_region(&out, r, &o, &no, NULL) == PC_OK);
                for (i = 0; i < na && i < no; i++) {
                    float d = fabsf(a[i] - o[i]);
                    wst = d > wst ? d : wst;
                }
                CHECK(wst <= 1.0f / 255.0f);
                free(a), free(o);
                pc_surface_fini(&out);
            }
            pc_warp_fini(&w);
        }
        pc_surface_fini(&src);
    }
}

static void test_translation_and_known_arc_points(void)
{
    pc_surface src, out, arc;
    pc_rect r;
    double d[2] = {10.0, -2.0}, b[4];
    float p[4], q[4];
    pc_warp w;
    int32_t cx, k;
    pc_point e;
    wsample(&src, PC_SAMPLE_TYPE_U8);
    r = pc_surface_content_bounds(&src);
    CHECK(pc_algo_warp_mesh_surface(&src, r, shift_map, d, PC_ALGO_INTERP_BILINEAR, &out, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_surface_content_bounds(&out), pc_rect_translate(r, 10, -2)));
    pc_surface_read_pixel(&out, 20, 10, p);
    pc_surface_read_pixel(&src, 10, 12, q);
    CHECK(memcmp(p, q, sizeof p) == 0);
    b[0] = r.x0, b[1] = r.y0, b[2] = r.x1, b[3] = r.y1;
    pc_warp_preset(&w, PC_WARP_ARC, 50.0, b);
    CHECK(pc_algo_warp_mesh_surface(&src, r, warp_map, &w, PC_ALGO_INTERP_BILINEAR, &arc, NULL) == PC_OK);
    cx = (r.x0 + r.x1) / 2;
    pc_surface_read_pixel(&arc, cx, 10, p);
    pc_surface_read_pixel(&src, cx, 10, q);
    for (k = 0; k < 4; k++) {
        CHECK(fabsf(p[k] - q[k]) < 0.05f);
    }
    e = pc_warp_map(&w, r.x0 + 2.5, r.y0 + 2.5);
    pc_surface_read_pixel(&arc, (int32_t)floor(e.x), (int32_t)floor(e.y), p);
    CHECK(p[3] > 0.2f);
    CHECK(pc_surface_content_bounds(&arc).y1 > r.y1 + 2);
    pc_warp_fini(&w);
    pc_surface_fini(&src), pc_surface_fini(&out), pc_surface_fini(&arc);
}

static void test_gray_masks_keep_their_default(void)
{
    const float one = 1.0f, zero = 0.0f;
    double d[2] = {5.0, 0.0};
    pc_surface m, o;
    pc_surface_init_default(&m, PC_PIXEL_FORMAT_GRAY8, &one, 1);
    CHECK(pc_surface_fill_rect(&m, pc_rect_make(4, 4, 10, 10), &zero, 1, NULL) == PC_OK);
    CHECK(pc_algo_warp_mesh_gray(&m, shift_map, d, PC_ALGO_INTERP_BILINEAR, &o, NULL) == PC_OK);
    CHECK(pc_surface_sample_channel(&o, 0, 0, 0) == 1.0f);
    CHECK(pc_surface_sample_channel(&o, 12, 6, 0) == 0.0f);
    CHECK(pc_surface_sample_channel(&o, 5, 6, 0) == 1.0f);
    pc_surface_fini(&m), pc_surface_fini(&o);
    /* Never crash: multi-channel masks and bad triangle indices are errors. */
    {
        pc_surface s;
        static const pc_algo_warp_vertex v[1] = {{{0, 0}, {0, 0}}};
        static const size_t tris[1][3] = {{0, 0, 3}};
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        CHECK(pc_algo_warp_mesh_gray(&s, shift_map, d, PC_ALGO_INTERP_BILINEAR, &o, NULL) == PC_ERR_INVALID);
        CHECK(pc_algo_warp_triangles(&s, pc_rect_make(0, 0, 4, 4), v, 1, tris, 1, PC_ALGO_INTERP_NEAREST, &o,
                                     NULL) == PC_ERR_INVALID);
        pc_surface_fini(&s);
    }
}

int main(int argc, char **argv)
{
    (void)argc, (void)argv;
    RUN(test_every_effect_changes_pixels_and_is_tile_independent);
    RUN(test_rough_pastels_and_angled_strokes_respond_to_every_slider);
    RUN(test_params_parse_and_clamp);
    RUN(test_composite_draws_a_disc_and_respects_the_clip);
    RUN(test_renderers_are_deterministic_and_seeded);
    RUN(test_renderers_refuse_absurd_canvases);
    RUN(test_traps_the_internal_colour_edge);
    RUN(test_leaves_paper_untouched);
    RUN(test_width_zero_is_identity);
    RUN(test_a_mesh_warp_far_off_canvas_writes_only_the_two_regions);
    RUN(test_identity_warp_is_a_no_op_at_every_depth);
    RUN(test_translation_and_known_arc_points);
    RUN(test_gray_masks_keep_their_default);
    return pc_t_finish();
}
