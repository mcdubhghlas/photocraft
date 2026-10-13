/* The unit tests of crates/compose/src/{psblend,fill_layout,masks,multichannel,proxy}.rs, plus
 * content bounds, layer queries and the C2 / C3 stubs' contract. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "vector/vector.h"

/* ---- psblend.rs ---- */

static void composite3(pc_blend_mode m, float cb, float cs, float gamma, float out[4])
{
    const float b[4] = {cb, cb, cb, 1.0f};
    const float s[4] = {cs, cs, cs, 1.0f};
    pc_psblend_composite_gamma(m, b, s, 1.0f, gamma, out);
}

static void test_text_gamma_lightens_dark_edges(void)
{
    /* 69 % blue (0, 51, 153) over grey 129: Photoshop shows (57, 80, 146). */
    const float g = 129.0f / 255.0f;
    const float bd[4] = {g, g, g, 1.0f};
    const float s1[4] = {0.0f, 0.2f, 0.6f, 0.686f};
    const float s2[4] = {0.0f, 0.2f, 0.6f, 0.937f};
    const float mb[4] = {0.2f, 0.4f, 0.6f, 0.7f};
    const float ms[4] = {0.5f, 0.1f, 0.9f, 0.5f};
    float r[4];
    float q[4];
    pc_psblend_composite_gamma(PC_BLEND_NORMAL, bd, s1, 1.0f, PC_PSBLEND_TEXT_GAMMA, r);
    CHECK(fabsf(r[0] * 255.0f - 57.0f) < 1.0f && fabsf(r[1] * 255.0f - 80.0f) < 1.0f &&
          fabsf(r[2] * 255.0f - 146.0f) < 1.0f);
    /* Near-opaque edges stay dark: 94 % coverage shows (13, 57, 152). */
    pc_psblend_composite_gamma(PC_BLEND_NORMAL, bd, s2, 1.0f, PC_PSBLEND_TEXT_GAMMA, r);
    CHECK(fabsf(r[0] * 255.0f - 13.0f) < 1.0f && fabsf(r[1] * 255.0f - 57.0f) < 1.0f &&
          fabsf(r[2] * 255.0f - 152.0f) < 1.0f);
    pc_psblend_composite_gamma(PC_BLEND_MULTIPLY, mb, ms, 0.8f, 1.0f, r);
    pc_psblend_composite(PC_BLEND_MULTIPLY, mb, ms, 0.8f, q);
    CHECK(memcmp(r, q, sizeof r) == 0);
}

static float rgb1(pc_blend_mode m, float cb, float cs)
{
    const float b[3] = {cb, cb, cb};
    const float s[3] = {cs, cs, cs};
    float o[3];
    pc_psblend_rgb(m, b, s, o);
    return o[0];
}

static void test_vivid_light_source_extremes_win(void)
{
    CHECK(rgb1(PC_BLEND_VIVID_LIGHT, 0.0f, 1.0f) == 1.0f);
    CHECK(rgb1(PC_BLEND_VIVID_LIGHT, 1.0f, 0.0f) == 0.0f);
    CHECK(fabsf(rgb1(PC_BLEND_VIVID_LIGHT, 0.5f, 0.75f) - 1.0f) < 1e-6f);
    CHECK(fabsf(rgb1(PC_BLEND_VIVID_LIGHT, 0.5f, 0.25f)) < 1e-6f);
}

static void test_hard_mix_edges_match_photoshop(void)
{
    CHECK(rgb1(PC_BLEND_HARD_MIX, 1.0f, 0.0f) == 1.0f);
    CHECK(rgb1(PC_BLEND_HARD_MIX, 0.0f, 1.0f) == 0.0f);
    CHECK(rgb1(PC_BLEND_HARD_MIX, 0.6f, 0.6f) == 1.0f);
    CHECK(rgb1(PC_BLEND_HARD_MIX, 0.3f, 0.3f) == 0.0f);
}

static void test_burn_and_dodge_treat_near_extremes_as_exact(void)
{
    CHECK(rgb1(PC_BLEND_COLOR_BURN, 1.0f - 1e-6f, 0.0f) == 1.0f);
    CHECK(rgb1(PC_BLEND_COLOR_DODGE, 1e-6f, 1.0f) == 0.0f);
    CHECK(fabsf(rgb1(PC_BLEND_COLOR_BURN, 0.5f, 0.5f)) < 1e-6f);
    CHECK(fabsf(rgb1(PC_BLEND_COLOR_DODGE, 0.25f, 0.5f) - 0.5f) < 1e-6f);
}

static void test_other_modes_delegate(void)
{
    size_t i;
    for (i = 0; i < PC_BLEND_LAYER_MODE_COUNT; i++) {
        pc_blend_mode m = PC_BLEND_LAYER_MODES[i];
        const float cb[3] = {0.2f, 0.5f, 0.9f};
        const float cs[3] = {0.7f, 0.1f, 0.4f};
        const float b4[4] = {0.2f, 0.5f, 0.9f, 0.7f};
        const float s4[4] = {0.7f, 0.1f, 0.4f, 0.6f};
        float x[3];
        float y[3];
        float a[4];
        float b[4];
        int c;
        if (m == PC_BLEND_VIVID_LIGHT || m == PC_BLEND_HARD_MIX || m == PC_BLEND_COLOR_BURN ||
            m == PC_BLEND_COLOR_DODGE) {
            continue;
        }
        pc_psblend_rgb(m, cb, cs, x);
        pc_blend_rgb(m, cb, cs, y);
        CHECK(memcmp(x, y, sizeof x) == 0);
        pc_psblend_composite(m, b4, s4, 0.8f, a);
        pc_blend_composite(m, b4, s4, 0.8f, b);
        for (c = 0; c < 4; c++) {
            CHECK(fabsf(a[c] - b[c]) < 1e-6f);
        }
    }
}

static void test_generic_vivid_light_and_hard_mix_composites_match_photoshop(void)
{
    const struct {
        pc_blend_mode m;
        float cb, cs;
    } cases[8] = {
        {PC_BLEND_HARD_MIX, 0.0f, 1.0f},      {PC_BLEND_HARD_MIX, 1.0f, 0.0f},
        {PC_BLEND_HARD_MIX, 0.6f, 0.4f},      {PC_BLEND_HARD_MIX, 0.6f, 0.3f},
        {PC_BLEND_VIVID_LIGHT, 1.0f, 0.0f},   {PC_BLEND_VIVID_LIGHT, 0.0f, 1.0f},
        {PC_BLEND_VIVID_LIGHT, 0.75f, 0.25f}, {PC_BLEND_VIVID_LIGHT, 0.25f, 0.75f},
    };
    int i;
    for (i = 0; i < 8; i++) {
        const float b[4] = {cases[i].cb, cases[i].cb, cases[i].cb, 1.0f};
        const float s[4] = {cases[i].cs, cases[i].cs, cases[i].cs, 1.0f};
        float g[4];
        float p[4];
        int c;
        pc_blend_composite(cases[i].m, b, s, 1.0f, g);
        composite3(cases[i].m, cases[i].cb, cases[i].cs, 1.0f, p);
        for (c = 0; c < 4; c++) {
            CHECK(fabsf(g[c] - p[c]) < 1e-6f);
        }
    }
}

static void test_text_gamma_setting_is_clamped(void)
{
    float g0 = pc_psblend_text_gamma();
    CHECK(g0 == PC_PSBLEND_TEXT_GAMMA);
    pc_psblend_set_text_gamma(5.0f);
    CHECK(pc_psblend_text_gamma() == 2.2f);
    pc_psblend_set_text_gamma(0.1f);
    CHECK(pc_psblend_text_gamma() == 1.0f);
    pc_psblend_set_text_gamma(NAN);
    CHECK(pc_psblend_text_gamma() == PC_PSBLEND_TEXT_GAMMA);
}

/* ---- fill_layout.rs ---- */

static void grid_t(pc_gradient_style style, float angle, pc_rect frame, float *out)
{
    int32_t x;
    int32_t y;
    size_t i = 0;
    for (y = frame.y0; y < frame.y1; y++) {
        for (x = frame.x0; x < frame.x1; x++) {
            out[i++] = pc_compose_fill_gradient_t(style, angle, 1.0f, false, frame, (float)x + 0.5f,
                                                  (float)y + 0.5f);
        }
    }
}

static void test_small_reflected_30_degrees_snaps_to_the_corner(void)
{
    /* Photoshop's 4 x 4 30 degree Reflected fill: t = |x - y| / 4. */
    float t[16];
    int x;
    int y;
    grid_t(PC_GRADIENT_REFLECTED, 30.0f, pc_rect_make(0, 0, 4, 4), t);
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            float want = fabsf((float)x - (float)y) / 4.0f;
            CHECK(fabsf(t[y * 4 + x] - want) < 1e-4f);
        }
    }
}

static void test_exact_layouts_are_unchanged(void)
{
    const struct {
        pc_gradient_style s;
        float a;
        pc_rect f;
    } cases[4] = {
        {PC_GRADIENT_LINEAR, 90.0f, {0, 0, 33, 20}},
        {PC_GRADIENT_LINEAR, 0.0f, {-5, 3, 40, 17}},
        {PC_GRADIENT_REFLECTED, 45.0f, {0, 0, 32, 32}},
        {PC_GRADIENT_LINEAR, -90.0f, {10, 10, 210, 210}},
    };
    const float zero[2] = {0, 0};
    int i;
    for (i = 0; i < 4; i++) {
        pc_rect f = cases[i].f;
        const int32_t ys[3] = {f.y0, (f.y0 + f.y1) / 2, f.y1 - 1};
        const int32_t xs[3] = {f.x0, (f.x0 + f.x1) / 2, f.x1 - 1};
        int a;
        int b;
        for (a = 0; a < 3; a++) {
            for (b = 0; b < 3; b++) {
                float px = (float)xs[b] + 0.5f;
                float py = (float)ys[a] + 0.5f;
                float u = pc_compose_fill_gradient_t(cases[i].s, cases[i].a, 1.0f, false, f, px, py);
                float v = pc_compose_gradient_t(cases[i].s, cases[i].a, 1.0f, false, zero, f, px, py);
                CHECK(fabsf(u - v) < 1e-3f);
            }
        }
    }
}

static bool geom_eq(pc_compose_gradient_geom a, pc_compose_gradient_geom b)
{
    return a.angle == b.angle && a.scale == b.scale && a.offset[0] == b.offset[0] &&
           a.offset[1] == b.offset[1];
}

static void test_offset_centres_snap_like_the_frame_centre(void)
{
    pc_rect f = pc_rect_make(0, 0, 7, 5);
    pc_rect big = pc_rect_make(0, 0, 40, 20);
    const float zero[2] = {0, 0};
    const float off[2] = {0.25f, -0.1f};
    const float off2[2] = {0.2f, 0.1f};
    const float bad[2] = {NAN, 0.0f};
    pc_compose_gradient_geom g0;
    pc_compose_gradient_geom g1;
    pc_compose_gradient_geom g;
    /* A zero offset is the plain layout exactly. */
    CHECK(geom_eq(pc_compose_gradient_layout(PC_GRADIENT_LINEAR, 30.0f, 1.0f, zero, f),
                  pc_compose_fill_gradient_layout(PC_GRADIENT_LINEAR, 30.0f, 1.0f, f)));
    CHECK(geom_eq(pc_compose_gradient_layout(PC_GRADIENT_REFLECTED, 30.0f, 1.0f, zero, f),
                  pc_compose_fill_gradient_layout(PC_GRADIENT_REFLECTED, 30.0f, 1.0f, f)));
    /* Moving the centre by whole pixels moves the snapped layout by the same pixels. */
    g0 = pc_compose_gradient_layout(PC_GRADIENT_LINEAR, 30.0f, 0.5f, zero, big);
    g1 = pc_compose_gradient_layout(PC_GRADIENT_LINEAR, 30.0f, 0.5f, off, big);
    CHECK(fabsf(g0.angle - g1.angle) < 1e-4f && fabsf(g0.scale - g1.scale) < 1e-4f);
    CHECK(fabsf(g1.offset[0] - g0.offset[0] - 0.25f) < 1e-4f &&
          fabsf(g1.offset[1] - g0.offset[1] + 0.1f) < 1e-4f);
    /* Non-snapping styles and bad offsets pass through. */
    g = pc_compose_gradient_layout(PC_GRADIENT_RADIAL, 30.0f, 1.0f, off2, big);
    CHECK(g.angle == 30.0f && g.scale == 1.0f && g.offset[0] == 0.2f && g.offset[1] == 0.1f);
    g = pc_compose_gradient_layout(PC_GRADIENT_LINEAR, 30.0f, 1.0f, bad, big);
    CHECK(isnan(g.offset[0]));
}

static void test_large_frames_barely_move_and_bad_input_is_harmless(void)
{
    pc_rect f = pc_rect_make(0, 0, 3000, 2000);
    pc_compose_gradient_geom g = pc_compose_fill_gradient_layout(PC_GRADIENT_LINEAR, 30.0f, 1.0f, f);
    const float cases[4][2] = {{NAN, 1.0f}, {30.0f, INFINITY}, {30.0f, 0.0f}, {1e30f, 1.0f}};
    int i;
    CHECK(fabsf(g.angle - 30.0f) < 0.05f && fabsf(g.scale - 1.0f) < 1e-3f && fabsf(g.offset[0]) < 1e-3f &&
          fabsf(g.offset[1]) < 1e-3f);
    for (i = 0; i < 4; i++) {
        float t = pc_compose_fill_gradient_t(PC_GRADIENT_LINEAR, cases[i][0], cases[i][1], false,
                                             pc_rect_make(0, 0, 4, 4), 1.5f, 1.5f);
        CHECK(isnan(t) || (t >= 0.0f && t <= 1.0f));
    }
    g = pc_compose_fill_gradient_layout(PC_GRADIENT_RADIAL, 30.0f, 1.0f, f);
    CHECK(g.angle == 30.0f && g.scale == 1.0f && g.offset[0] == 0.0f && g.offset[1] == 0.0f);
}

/* ---- masks.rs ---- */

static void test_pixel_edits_refresh_cached_masks_at_every_depth(void)
{
    pc_rect canvas = pc_rect_make(0, 0, 64, 64);
    const pc_sample_type samples[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    int i;
    for (i = 0; i < 3; i++) {
        pc_layer layer = ch_raster("edited mask", PC_PIXEL_FORMAT_RGBA8);
        pc_pixel_format mf = {PC_COLOR_MODE_GRAYSCALE, samples[i], false};
        const float one = 1.0f;
        const float zero = 0.0f;
        const float half = 0.5f;
        pc_surface s;
        pc_surface warm;
        bool found = false;
        size_t c1 = 0;
        size_t c2 = 0;
        pc_tile t1;
        pc_tile t2;
        const pc_raster_tile *a;
        const pc_raster_tile *b;
        bool shared = true;
        pc_layer_mask_hide_all(&layer.mask);
        pc_surface_fini(&layer.mask.surface);
        pc_surface_init(&layer.mask.surface, mf);
        CHECK(pc_surface_fill_rect(&layer.mask.surface, canvas, &one, 1, NULL) == PC_OK);
        layer.mask.feather = 4.0f;
        layer.has_mask = true;
        CHECK(pc_compose_combined_mask(&layer, canvas, &found, &s, NULL) == PC_OK && found);
        CHECK(fabsf(pc_surface_sample_channel(&s, 32, 32, 0) - 1.0f) < 1e-6f);
        pc_surface_fini(&s);
        /* No snapshot shares the input: the write may reuse the tile address, and the cache must
         * not return the old white mask. */
        CHECK(pc_surface_fill_rect(&layer.mask.surface, canvas, &zero, 1, NULL) == PC_OK);
        CHECK(pc_compose_combined_mask(&layer, canvas, &found, &s, NULL) == PC_OK && found);
        CHECK(pc_surface_sample_channel(&s, 32, 32, 0) == 0.0f);
        pc_surface_fini(&s);
        CHECK(pc_surface_fill_rect(&layer.mask.surface, canvas, &half, 1, NULL) == PC_OK);
        CHECK(pc_compose_combined_mask(&layer, canvas, &found, &s, NULL) == PC_OK && found);
        CHECK(fabsf(pc_surface_sample_channel(&s, 32, 32, 0) - 0.5f) < 0.005f);
        CHECK(pc_compose_combined_mask(&layer, canvas, &found, &warm, NULL) == PC_OK && found);
        while (pc_surface_next_tile(&s, &c1, &t1, &a) && pc_surface_next_tile(&warm, &c2, &t2, &b)) {
            shared = shared && a == b;
        }
        CHECK(shared);
        pc_surface_fini(&s);
        pc_surface_fini(&warm);
        pc_layer_fini(&layer);
    }
}

static void test_matches_the_cpu_mask_and_is_cached(void)
{
    pc_layer l = ch_raster("l", PC_PIXEL_FORMAT_RGBA8);
    const double pts[3][2] = {{5, 5}, {30, 6}, {20, 28}};
    pc_path p = ch_polygon(pts, 3);
    pc_rect canvas = pc_rect_make(0, 0, 40, 32);
    pc_surface s;
    pc_surface again;
    float *want = NULL;
    size_t n = 0;
    float got[40 * 32];
    bool found = false;
    size_t i;
    bool ok = true;
    size_t c1 = 0;
    size_t c2 = 0;
    pc_tile t1;
    pc_tile t2;
    const pc_raster_tile *a;
    const pc_raster_tile *b;
    pc_vector_mask_init(&l.vector_mask, &p);
    l.vector_mask.density = 0.9f;
    l.has_vector_mask = true;
    CHECK(pc_compose_combined_mask(&l, canvas, &found, &s, NULL) == PC_OK && found);
    CHECK(pc_vec_mask_values(&l.vector_mask, canvas, &want, &n, NULL) == PC_OK && n == 40 * 32);
    CHECK(pc_surface_read_region_into(&s, canvas, got, 40 * 32, NULL) == PC_OK);
    for (i = 0; want != NULL && i < n; i++) {
        ok = ok && fabsf(got[i] - want[i]) <= 1.0f / 65535.0f; /* the cache stores U16 */
    }
    CHECK(ok);
    free(want);
    CHECK(pc_compose_combined_mask(&l, canvas, &found, &again, NULL) == PC_OK && found);
    ok = true;
    while (pc_surface_next_tile(&s, &c1, &t1, &a) && pc_surface_next_tile(&again, &c2, &t2, &b)) {
        ok = ok && a == b;
    }
    CHECK(ok);
    pc_surface_fini(&s);
    pc_surface_fini(&again);
    pc_vector_mask_fini(&l.vector_mask);
    l.has_vector_mask = false;
    found = true;
    CHECK(pc_compose_combined_mask(&l, canvas, &found, &s, NULL) == PC_OK && !found);
    pc_layer_fini(&l);
}

/* ---- multichannel.rs ---- */

static void test_inks_multiply_over_paper(void)
{
    pc_document d = ch_doc(4, 1, PC_COLOR_MODE_MULTICHANNEL, PC_SAMPLE_TYPE_U8);
    pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false};
    const float av[4] = {0, 1, 1, 0};
    const float bv[4] = {0, 0, 1, 1};
    pc_surface a;
    pc_surface b;
    pc_alpha_channel *ch = calloc(2, sizeof *ch);
    pc_compose_buffer r;
    const float want[4][4] = {{1, 1, 1, 1}, {0, 1, 1, 1}, {0, 0, 1, 1}, {1, 0, 1, 1}};
    int i;
    pc_surface_init(&a, fmt);
    pc_surface_init(&b, fmt);
    CHECK(pc_surface_write_region(&a, pc_rect_make(0, 0, 4, 1), av, 4, NULL) == PC_OK);
    CHECK(pc_surface_write_region(&b, pc_rect_make(0, 0, 4, 1), bv, 4, NULL) == PC_OK);
    CHECK(ch != NULL && pc_alpha_channel_init(&ch[0], "Cyan", &a, NULL) == PC_OK);
    CHECK(pc_alpha_channel_init(&ch[1], "Magenta", &b, NULL) == PC_OK);
    ch[0].has_spot = true;
    ch[0].spot_color = pc_color_rgb(0, 1, 1);
    ch[0].spot_solidity = 0.0f;
    ch[1].has_spot = true;
    ch[1].spot_color = pc_color_rgb(1, 0, 1);
    ch[1].spot_solidity = 0.0f;
    d.channels = ch;
    d.nchannels = 2;
    CHECK(pc_compose_flatten(&d, &r, NULL) == PC_OK);
    for (i = 0; i < 4; i++) {
        CHECK(memcmp(r.px[i], want[i], 16) == 0);
    }
    pc_compose_buffer_fini(&r);
    /* Alpha channels and other modes are not inks. */
    d.mode = PC_COLOR_MODE_RGB;
    CHECK(pc_compose_flatten(&d, &r, NULL) == PC_OK);
    CHECK(r.px[1][3] == 0.0f);
    pc_compose_buffer_fini(&r);
    pc_document_fini(&d);
}

/* ---- proxy.rs ---- */

static void test_downsample_picks_every_kth_pixel(void)
{
    pc_surface s;
    pc_surface d;
    const float red[4] = {1, 0, 0, 1};
    const float blue[4] = {0, 0, 1, 1};
    float p[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 8, 8), red, 4, NULL) == PC_OK);
    CHECK(pc_surface_write_pixel(&s, 4, 4, blue, 4, NULL) == PC_OK);
    CHECK(pc_compose_downsample(&s, 4, &d, NULL) == PC_OK);
    pc_surface_read_pixel(&d, 0, 0, p);
    CHECK(memcmp(p, red, 16) == 0);
    pc_surface_read_pixel(&d, 1, 1, p);
    CHECK(memcmp(p, blue, 16) == 0);
    pc_surface_read_pixel(&d, 2, 2, p);
    CHECK(p[3] == 0.0f);
    pc_surface_fini(&s);
    pc_surface_fini(&d);
}

static void test_downsample_handles_negative_coordinates(void)
{
    pc_surface s;
    pc_surface d;
    const float green[4] = {0, 1, 0, 1};
    float p[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(-8, -8, 0, 0), green, 4, NULL) == PC_OK);
    CHECK(pc_compose_downsample(&s, 4, &d, NULL) == PC_OK);
    pc_surface_read_pixel(&d, -1, -1, p);
    CHECK(memcmp(p, green, 16) == 0);
    pc_surface_read_pixel(&d, 0, 0, p);
    CHECK(p[3] == 0.0f);
    pc_surface_fini(&s);
    pc_surface_fini(&d);
}

static void test_proxy_keeps_structure_and_ids(void)
{
    pc_document d = ch_doc_white(4000, 3000);
    pc_document p;
    float px[4];
    CHECK(pc_compose_proxy_document(&d, 4, &p, NULL) == PC_OK);
    CHECK(p.size.width == 1000 && p.size.height == 750);
    CHECK(p.layers[0].id == d.layers[0].id);
    pc_surface_read_pixel(pc_layer_surface(&p.layers[0]), 999, 749, px);
    CHECK(px[0] == 1.0f && px[1] == 1.0f && px[2] == 1.0f && px[3] == 1.0f);
    CHECK(pc_compose_proxy_faithful(&d));
    pc_document_fini(&p);
    pc_document_fini(&d);
}

static void test_proxy_scales_the_selection(void)
{
    pc_document d = ch_doc_white(64, 64);
    pc_document p;
    pc_document n = ch_doc(8, 8, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    const float one = 1.0f;
    pc_surface_init(&d.selection, PC_PIXEL_FORMAT_GRAY8);
    d.has_selection = true;
    CHECK(pc_surface_fill_rect(&d.selection, pc_rect_make(0, 0, 32, 64), &one, 1, NULL) == PC_OK);
    CHECK(pc_compose_proxy_document(&d, 4, &p, NULL) == PC_OK);
    CHECK(p.has_selection);
    CHECK(pc_surface_sample_channel(&p.selection, 7, 15, 0) == 1.0f);
    CHECK(pc_surface_sample_channel(&p.selection, 8, 0, 0) == 0.0f);
    pc_document_fini(&p);
    CHECK(pc_compose_proxy_document(&n, 4, &p, NULL) == PC_OK);
    CHECK(!p.has_selection);
    pc_document_fini(&p);
    pc_document_fini(&n);
    pc_document_fini(&d);
}

/* ---- bounds.rs and queries ---- */

static void test_content_bounds_cached_matches_raster(void)
{
    pc_surface s;
    const float c[4] = {0.3f, 0.2f, 0.1f, 1};
    pc_rect a;
    pc_rect b;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_rect_is_empty(pc_compose_content_bounds(&s)));
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(-300, 17, 5, 600), c, 4, NULL) == PC_OK);
    a = pc_compose_content_bounds(&s);
    b = pc_surface_content_bounds(&s);
    CHECK(pc_rect_eq(a, b) && pc_rect_eq(a, pc_rect_make(-300, 17, 5, 600)));
    /* A hit after the tile changed must not return the old answer. */
    CHECK(pc_surface_write_pixel(&s, 700, 900, c, 4, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_compose_content_bounds(&s), pc_surface_content_bounds(&s)));
    pc_compose_purge_bounds_cache();
    CHECK(pc_rect_eq(pc_compose_content_bounds(&s), pc_surface_content_bounds(&s)));
    pc_surface_fini(&s);
}

static void test_layer_queries(void)
{
    pc_rect canvas = pc_rect_make(0, 0, 100, 100);
    pc_layer a = ch_solid("a", pc_rect_make(10, 10, 20, 20), 1, 0, 0, 1);
    pc_layer b = ch_solid("b", pc_rect_make(50, 60, 70, 80), 0, 1, 0, 1);
    pc_layer kids[2];
    pc_layer g;
    pc_layer f;
    pc_layer adj;
    pc_layer_content c;
    pc_rect r;
    kids[0] = a;
    kids[1] = b;
    g = ch_group("g", kids, 2);
    CHECK(pc_rect_eq(pc_compose_layer_bounds(&g, canvas), pc_rect_make(10, 10, 70, 80)));
    CHECK(pc_compose_composite_bounds(&g, canvas, &r) && pc_rect_eq(r, pc_rect_make(10, 10, 70, 80)));
    CHECK(pc_compose_change_bounds(&g, canvas, &r) && pc_rect_eq(r, pc_rect_make(10, 10, 70, 80)));
    CHECK(pc_compose_transparent_outside(&g));
    g.content.u.group.children[1].visible = false;
    CHECK(pc_rect_eq(pc_compose_layer_bounds(&g, canvas), pc_rect_make(10, 10, 20, 20)));
    /* Fill layers cover the canvas and can occlude. */
    c.kind = PC_LAYER_FILL;
    c.u.fill = pc_fill_solid(pc_color_rgb(1, 1, 1));
    CHECK(pc_layer_init(&f, "f", &c, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_compose_layer_bounds(&f, canvas), canvas));
    CHECK(pc_compose_occludes_below(&f, PC_COLOR_MODE_RGB));
    f.opacity = 0.5f;
    CHECK(!pc_compose_occludes_below(&f, PC_COLOR_MODE_RGB));
    CHECK(pc_rect_eq(pc_compose_fill_frame(&f, canvas), canvas));
    /* An adjustment layer reaches everything beneath it. */
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_ADJUSTMENT;
    c.u.adjustment.kind = PC_ADJ_INVERT;
    CHECK(pc_layer_init(&adj, "inv", &c, NULL) == PC_OK);
    CHECK(!pc_compose_change_bounds(&adj, canvas, &r));
    CHECK(pc_compose_composite_bounds(&adj, canvas, &r) && pc_rect_is_empty(r));
    CHECK(pc_compose_text_gamma(&adj) == 1.0f);
    pc_layer_fini(&adj);
    pc_layer_fini(&f);
    pc_layer_fini(&g);
}

/* ---- the C3 stub's contract (C2 has landed) ---- */

static void test_unported_features_fail_cleanly(void)
{
    pc_document d = ch_doc_white(4, 4);
    pc_layer adj;
    pc_layer_content c;
    pc_compose_buffer b;
    pc_error e;
    pc_effect fx;
    pc_layer *l;
    pc_status st;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_ADJUSTMENT;
    c.u.adjustment.kind = PC_ADJ_INVERT;
    CHECK(pc_layer_init(&adj, "inv", &c, NULL) == PC_OK);
    ch_push(&d, adj);
    /* Adjustment layers render (C2): white inverted to black. */
    CHECK(pc_compose_flatten(&d, &b, &e) == PC_OK);
    CHECK(b.len == 16 && b.px[0][0] == 0.0f && b.px[0][3] == 1.0f);
    pc_compose_buffer_fini(&b);
    d.layers[1].visible = false;
    l = ch_push(&d, ch_solid("fx", pc_rect_make(0, 0, 2, 2), 1, 0, 0, 1));
    pc_effect_default_drop_shadow(&fx);
    l->effects.items = malloc(sizeof fx);
    CHECK(l->effects.items != NULL);
    if (l->effects.items != NULL) {
        l->effects.items[0] = fx;
        l->effects.nitems = 1;
    }
    /* Layer effects render (C3). */
    st = pc_compose_flatten(&d, &b, &e);
    CHECK(st == PC_OK);
    if (st == PC_OK) {
        pc_compose_buffer_fini(&b);
    }
    /* Knockout renders (C2). */
    d.layers[2].advanced.knockout = PC_KNOCKOUT_DEEP;
    d.layers[2].effects.enabled = false;
    CHECK(pc_compose_flatten(&d, &b, &e) == PC_OK);
    pc_compose_buffer_fini(&b);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_text_gamma_lightens_dark_edges);
    RUN(test_vivid_light_source_extremes_win);
    RUN(test_hard_mix_edges_match_photoshop);
    RUN(test_burn_and_dodge_treat_near_extremes_as_exact);
    RUN(test_other_modes_delegate);
    RUN(test_generic_vivid_light_and_hard_mix_composites_match_photoshop);
    RUN(test_text_gamma_setting_is_clamped);
    RUN(test_small_reflected_30_degrees_snaps_to_the_corner);
    RUN(test_exact_layouts_are_unchanged);
    RUN(test_offset_centres_snap_like_the_frame_centre);
    RUN(test_large_frames_barely_move_and_bad_input_is_harmless);
    RUN(test_pixel_edits_refresh_cached_masks_at_every_depth);
    RUN(test_matches_the_cpu_mask_and_is_cached);
    RUN(test_inks_multiply_over_paper);
    RUN(test_downsample_picks_every_kth_pixel);
    RUN(test_downsample_handles_negative_coordinates);
    RUN(test_proxy_keeps_structure_and_ids);
    RUN(test_proxy_scales_the_selection);
    RUN(test_content_bounds_cached_matches_raster);
    RUN(test_layer_queries);
    RUN(test_unported_features_fail_cleanly);
    return pc_t_finish();
}
