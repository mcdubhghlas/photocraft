/* crates/compose/src/tests.rs, the parts C1 covers: blend modes, opacity, masks, clipping,
 * groups, fills, Dissolve, tiling, bands, reductions, thumbnails, channel restrictions, Blend If.
 * Tests that need adjustment layers or gradient / pattern fills are in test_fills.c and
 * test_adjust.c (C2); layer effects belong to C3. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"

/* An Invert adjustment layer. */
static pc_layer invert_layer(void)
{
    pc_layer l;
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_ADJUSTMENT;
    c.u.adjustment.kind = PC_ADJ_INVERT;
    if (pc_layer_init(&l, "inv", &c, NULL) != PC_OK) {
        abort();
    }
    return l;
}

static void test_background_only(void)
{
    pc_document d = ch_doc_white(8, 8);
    CHECK(ch_px_close(&d, 3, 3, 1, 1, 1, 1));
    /* outside the canvas the background layer has no pixels */
    CHECK(ch_px_close(&d, 20, 3, 0, 0, 0, 0));
    pc_document_fini(&d);
}

static void test_normal_layer_over_background(void)
{
    pc_document d = ch_doc_white(8, 8);
    ch_push(&d, ch_solid("red", pc_rect_make(0, 0, 4, 8), 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 1, 1, 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 5, 1, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_opacity_and_fill_multiply(void)
{
    pc_document d = ch_doc_white(4, 4);
    pc_layer *l = ch_push(&d, ch_solid("k", pc_rect_make(0, 0, 4, 4), 0, 0, 0, 1));
    float p[4];
    l->opacity = 0.5f;
    l->fill_opacity = 0.5f;
    ch_px(&d, 0, 0, p);
    CHECK(fabsf(p[0] - 0.75f) <= CH_E);
    pc_document_fini(&d);
}

static void test_hidden_layers_skipped(void)
{
    pc_document d = ch_doc_white(4, 4);
    ch_push(&d, ch_solid("k", pc_rect_make(0, 0, 4, 4), 0, 0, 0, 1))->visible = false;
    CHECK(ch_px_close(&d, 0, 0, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_every_blend_mode_matches_reference_on_opaque_pixels(void)
{
    const float backdrop[4] = {0.6f, 0.3f, 0.2f, 1.0f};
    const float source[4] = {0.2f, 0.7f, 0.5f, 1.0f};
    size_t m;
    for (m = 0; m < PC_BLEND_LAYER_MODE_COUNT; m++) {
        pc_blend_mode mode = PC_BLEND_LAYER_MODES[m];
        pc_document d;
        pc_layer b;
        pc_layer t;
        float got[4];
        float want[3];
        int i;
        if (mode == PC_BLEND_DISSOLVE) {
            continue;
        }
        d = ch_doc(2, 2, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32);
        b = ch_raster("b", PC_PIXEL_FORMAT_RGBA32F);
        ch_fill(&b, pc_rect_make(0, 0, 2, 2), backdrop, 4);
        t = ch_raster("t", PC_PIXEL_FORMAT_RGBA32F);
        ch_fill(&t, pc_rect_make(0, 0, 2, 2), source, 4);
        t.blend = mode;
        ch_push(&d, b);
        ch_push(&d, t);
        ch_px(&d, 0, 0, got);
        /* A 32-bit document: Add / Divide don't clip at 1 (Divide gives 0.6 / 0.2 = 3 in red). */
        pc_psblend_set_hdr(true);
        pc_psblend_rgb(mode, backdrop, source, want);
        pc_psblend_set_hdr(false);
        for (i = 0; i < 3; i++) {
            CHECK(fabsf(got[i] - want[i]) < 1e-5f);
        }
        CHECK(fabsf(got[3] - 1.0f) < 1e-6f);
        pc_document_fini(&d);
    }
}

static void test_layer_mask_hides_pixels(void)
{
    pc_document d = ch_doc_white(4, 4);
    pc_layer *l = ch_push(&d, ch_solid("k", pc_rect_make(0, 0, 4, 4), 0, 0, 0, 1));
    const float zero = 0.0f;
    pc_layer_mask_reveal_all(&l->mask);
    l->has_mask = true;
    CHECK(pc_surface_fill_rect(&l->mask.surface, pc_rect_make(0, 0, 2, 4), &zero, 1, NULL) == PC_OK);
    CHECK(ch_px_close(&d, 0, 0, 1, 1, 1, 1));
    CHECK(ch_px_close(&d, 3, 0, 0, 0, 0, 1));
    pc_document_fini(&d);
}

static void test_disabled_mask_is_ignored(void)
{
    pc_document d = ch_doc_white(2, 2);
    pc_layer *l = ch_push(&d, ch_solid("k", pc_rect_make(0, 0, 2, 2), 0, 0, 0, 1));
    float p[4];
    pc_layer_mask_hide_all(&l->mask);
    l->mask.enabled = false;
    l->has_mask = true;
    ch_px(&d, 0, 0, p);
    CHECK(p[0] < 0.3f && fabsf(p[0] - p[1]) < 0.05f && fabsf(p[1] - p[2]) < 0.05f && p[3] == 1.0f);
    pc_document_fini(&d);
}

static void test_clipping_mask_restricts_to_base_alpha(void)
{
    pc_document d = ch_doc_white(8, 8);
    ch_push(&d, ch_solid("base", pc_rect_make(0, 0, 4, 8), 0, 0, 1, 1));
    ch_push(&d, ch_solid("clip", pc_rect_make(0, 0, 8, 8), 1, 0, 0, 1))->clipped = true;
    CHECK(ch_px_close(&d, 1, 1, 1, 0, 0, 1)); /* inside base: clipped layer visible */
    CHECK(ch_px_close(&d, 6, 1, 1, 1, 1, 1)); /* outside base: clipped layer hidden */
    pc_document_fini(&d);
}

static void test_hidden_base_hides_clipping_group(void)
{
    pc_document d = ch_doc_white(4, 4);
    ch_push(&d, ch_solid("base", pc_rect_make(0, 0, 4, 4), 0, 0, 1, 1))->visible = false;
    ch_push(&d, ch_solid("clip", pc_rect_make(0, 0, 4, 4), 1, 0, 0, 1))->clipped = true;
    CHECK(ch_px_close(&d, 1, 1, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static float isolated_make(pc_blend_mode blend)
{
    pc_document d = ch_doc_white(2, 2);
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    pc_layer m = ch_solid("m", pc_rect_make(0, 0, 2, 2), 0.5f, 0.5f, 0.5f, 1);
    pc_layer g;
    float p[4];
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&d.layers[0]), pc_rect_make(0, 0, 2, 2), grey, 4, NULL) ==
          PC_OK);
    m.blend = PC_BLEND_MULTIPLY;
    g = ch_group("g", &m, 1);
    g.blend = blend;
    ch_push(&d, g);
    ch_px(&d, 0, 0, p);
    pc_document_fini(&d);
    return p[0];
}

static void test_isolated_group_differs_from_pass_through(void)
{
    /* A Multiply layer inside a group: pass-through multiplies with the background, an isolated
     * (Normal) group multiplies only with its own (empty) contents. */
    CHECK(fabsf(isolated_make(PC_BLEND_PASS_THROUGH) - 0.25f) <= CH_E);
    CHECK(fabsf(isolated_make(PC_BLEND_NORMAL) - 0.5f) <= CH_E);
}

static void test_mask_feather_blurs_the_edge(void)
{
    pc_document d = ch_doc_white(64, 8);
    pc_layer *l = ch_push(&d, ch_solid("k", pc_rect_make(0, 0, 64, 8), 0, 0, 0, 1));
    const float one = 1.0f;
    float a[4];
    float b[4];
    pc_layer_mask_hide_all(&l->mask);
    l->has_mask = true;
    CHECK(pc_surface_fill_rect(&l->mask.surface, pc_rect_make(0, 0, 32, 8), &one, 1, NULL) == PC_OK);
    ch_px(&d, 31, 4, a);
    ch_px(&d, 32, 4, b);
    CHECK(a[0] < 0.01f && b[0] > 0.99f); /* sharp without feather */
    d.layers[1].mask.feather = 8.0f;
    ch_px(&d, 31, 4, a);
    ch_px(&d, 32, 4, b);
    CHECK(a[0] > 0.3f && a[0] < 0.5f && b[0] > 0.5f && b[0] < 0.7f); /* soft edge around 50 % */
    ch_px(&d, 4, 4, a);
    ch_px(&d, 60, 4, b);
    CHECK(a[0] < 0.01f && b[0] > 0.99f); /* far from the edge unchanged */
    pc_document_fini(&d);
}

static void test_group_opacity_applies_once(void)
{
    pc_document d = ch_doc_white(2, 2);
    pc_layer kids[2];
    pc_layer g;
    float p[4];
    kids[0] = ch_solid("a", pc_rect_make(0, 0, 2, 2), 0, 0, 0, 1);
    kids[1] = ch_solid("b", pc_rect_make(0, 0, 2, 2), 0, 0, 0, 1);
    g = ch_group("g", kids, 2);
    g.blend = PC_BLEND_NORMAL;
    g.opacity = 0.5f;
    ch_push(&d, g);
    ch_px(&d, 0, 0, p);
    CHECK(fabsf(p[0] - 0.5f) <= CH_E);
    pc_document_fini(&d);
}

static void test_pass_through_group_opacity_mixes(void)
{
    pc_document d = ch_doc_white(2, 2);
    pc_layer a = ch_solid("a", pc_rect_make(0, 0, 2, 2), 0, 0, 0, 1);
    pc_layer g = ch_group("g", &a, 1);
    pc_document t;
    pc_layer red;
    const float hdr[4] = {2.0f, 0.0f, 0.0f, 1.0f};
    float p[4];
    g.opacity = 0.25f;
    ch_push(&d, g);
    ch_px(&d, 0, 0, p);
    CHECK(fabsf(p[0] - 0.75f) <= CH_E);
    pc_document_fini(&d);
    /* Opaque backdrops hide straight-alpha interpolation errors. */
    t = ch_doc(2, 2, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32);
    red = ch_raster("HDR red", pc_document_pixel_format(&t));
    ch_fill(&red, pc_document_bounds(&t), hdr, 4);
    g = ch_group("half", &red, 1);
    g.opacity = 0.5f;
    ch_push(&t, g);
    CHECK(ch_px_close(&t, 0, 0, 2, 0, 0, 0.5f));
    pc_document_fini(&t);
}

static void test_solid_fill_layer(void)
{
    pc_document d = ch_doc_white(10, 1);
    pc_layer l;
    pc_layer_content c;
    c.kind = PC_LAYER_FILL;
    c.u.fill = pc_fill_solid(pc_color_rgb(0, 0, 1));
    CHECK(pc_layer_init(&l, "fill", &c, NULL) == PC_OK);
    ch_push(&d, l);
    CHECK(ch_px_close(&d, 5, 0, 0, 0, 1, 1));
    pc_document_fini(&d);
}

static void test_dissolve_coverage_matches_opacity(void)
{
    pc_document d = ch_doc(64, 64, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer *l = ch_push(&d, ch_solid("k", pc_rect_make(0, 0, 64, 64), 0, 0, 0, 1));
    pc_compose_buffer b;
    pc_compose_buffer b2;
    size_t covered = 0;
    size_t i;
    float frac;
    l->blend = PC_BLEND_DISSOLVE;
    l->opacity = 0.3f;
    CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK);
    for (i = 0; i < b.len; i++) {
        covered += b.px[i][3] > 0.5f;
    }
    frac = (float)covered / (float)b.len;
    CHECK(fabsf(frac - 0.3f) < 0.05f);
    /* deterministic */
    CHECK(pc_compose_flatten(&d, &b2, NULL) == PC_OK);
    CHECK(pc_compose_buffer_eq(&b, &b2));
    pc_compose_buffer_fini(&b);
    pc_compose_buffer_fini(&b2);
    pc_document_fini(&d);
}

static void test_render_is_tile_independent(void)
{
    pc_document d = ch_doc_white(300, 300);
    pc_layer *l = ch_push(&d, ch_solid("a", pc_rect_make(20, 20, 280, 280), 0.1f, 0.5f, 0.9f, 0.7f));
    pc_compose_buffer full;
    pc_compose_buffer sub;
    int32_t x;
    int32_t y;
    bool same = true;
    l->blend = PC_BLEND_OVERLAY;
    CHECK(pc_compose_flatten(&d, &full, NULL) == PC_OK);
    CHECK(pc_compose_render(&d, pc_rect_make(250, 250, 270, 262), &sub, NULL) == PC_OK);
    for (y = 250; y < 262; y++) {
        for (x = 250; x < 270; x++) {
            same = same &&
                   memcmp(pc_compose_buffer_get(&full, x, y), pc_compose_buffer_get(&sub, x, y), 16) == 0;
        }
    }
    CHECK(same);
    pc_compose_buffer_fini(&full);
    pc_compose_buffer_fini(&sub);
    pc_document_fini(&d);
}

static void test_cmyk_document_renders_via_rgb(void)
{
    pc_document d = ch_doc(2, 2, PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8);
    pc_layer l = ch_raster("k", pc_document_pixel_format(&d));
    const float k[5] = {0, 0, 0, 1, 1};
    float p[4];
    ch_fill(&l, pc_rect_make(0, 0, 2, 2), k, 5);
    ch_push(&d, l);
    /* 100 % K alone is a dark neutral through the built-in CMYK profile, not pure black. */
    ch_px(&d, 0, 0, p);
    CHECK(p[0] < 0.3f && fabsf(p[0] - p[1]) < 0.05f && fabsf(p[1] - p[2]) < 0.05f && p[3] == 1.0f);
    pc_document_fini(&d);
}

static void test_sixteen_bit_and_float_layers_composite(void)
{
    const pc_pixel_format fmts[2] = {PC_PIXEL_FORMAT_RGBA16, PC_PIXEL_FORMAT_RGBA32F};
    const float c[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    int i;
    for (i = 0; i < 2; i++) {
        pc_document d = ch_doc(2, 2, PC_COLOR_MODE_RGB, fmts[i].sample);
        pc_layer l = ch_raster("a", fmts[i]);
        ch_fill(&l, pc_rect_make(0, 0, 2, 2), c, 4);
        ch_push(&d, l);
        CHECK(ch_px_close(&d, 1, 1, 0.25f, 0.5f, 0.75f, 1));
        pc_document_fini(&d);
    }
}

/* top blended with mode over an opaque bg, both solid, in a 4x4 RGB document of depth. */
static pc_document two_solid_layers(pc_sample_type depth, pc_blend_mode mode, const float bg[3],
                                    const float top[3])
{
    pc_document d = ch_doc(4, 4, PC_COLOR_MODE_RGB, depth);
    pc_pixel_format fmt = pc_document_pixel_format(&d);
    pc_layer b = ch_raster("bg", fmt);
    pc_layer t = ch_raster("top", fmt);
    float rgba[4];
    float px[PC_RASTER_MAX_CHANNELS];
    size_t n;
    memcpy(rgba, bg, 3 * sizeof(float));
    rgba[3] = 1.0f;
    n = pc_from_rgba(fmt, rgba, px);
    ch_fill(&b, pc_rect_make(0, 0, 4, 4), px, n);
    memcpy(rgba, top, 3 * sizeof(float));
    n = pc_from_rgba(fmt, rgba, px);
    ch_fill(&t, pc_rect_make(0, 0, 4, 4), px, n);
    t.blend = mode;
    ch_push(&d, b);
    ch_push(&d, t);
    return d;
}

static void test_float_documents_add_and_divide_past_white(void)
{
    const float bg[3] = {2.0f, 0.5f, 4.0f};
    const float top[3] = {0.5f, 0.25f, 0.5f};
    const pc_blend_mode modes[3] = {PC_BLEND_LINEAR_DODGE, PC_BLEND_DIVIDE, PC_BLEND_MULTIPLY};
    const float want[3][4] = {{2.5f, 0.75f, 4.5f, 1}, {4.0f, 2.0f, 8.0f, 1}, {1.0f, 0.125f, 2.0f, 1}};
    const float bg2[3] = {0.8f, 0.5f, 0.6f};
    const float clip[4] = {1.0f, 0.75f, 1.0f, 1.0f};
    int m;
    pc_document d;
    pc_compose_buffer b;
    for (m = 0; m < 3; m++) {
        pc_compose_buffer bufs[2];
        int k;
        size_t i;
        d = two_solid_layers(PC_SAMPLE_TYPE_F32, modes[m], bg, top);
        CHECK(pc_compose_flatten(&d, &bufs[0], NULL) == PC_OK);
        bufs[1] = ch_render_tiled(&d, pc_document_bounds(&d), 2);
        for (k = 0; k < 2; k++) {
            for (i = 0; i < bufs[k].len; i++) {
                CHECK(ch_close4(bufs[k].px[i], want[m]));
            }
            pc_compose_buffer_fini(&bufs[k]);
        }
        pc_document_fini(&d);
    }
    /* The flag doesn't leak into later renders on this thread. */
    d = two_solid_layers(PC_SAMPLE_TYPE_U16, PC_BLEND_LINEAR_DODGE, bg2, top);
    CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK);
    CHECK(ch_close4(b.px[0], clip));
    pc_compose_buffer_fini(&b);
    pc_document_fini(&d);
}

static void test_integer_documents_clip_add_and_divide_at_white(void)
{
    const float bg[3] = {0.8f, 0.5f, 0.6f};
    const float top[3] = {0.5f, 0.25f, 0.5f};
    const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const pc_blend_mode modes[2] = {PC_BLEND_LINEAR_DODGE, PC_BLEND_DIVIDE};
    const float clipped[2][4] = {{1.0f, 0.75f, 1.0f, 1}, {1.0f, 1.0f, 1.0f, 1}};
    const float hdr[2][4] = {{1.3f, 0.75f, 1.1f, 1}, {1.6f, 2.0f, 1.2f, 1}};
    int di;
    int m;
    for (di = 0; di < 3; di++) {
        for (m = 0; m < 2; m++) {
            pc_document d = two_solid_layers(depths[di], modes[m], bg, top);
            pc_compose_buffer b;
            CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK);
            CHECK(ch_close4(b.px[5], depths[di] == PC_SAMPLE_TYPE_F32 ? hdr[m] : clipped[m]));
            pc_compose_buffer_fini(&b);
            pc_document_fini(&d);
        }
    }
}

static void test_thumbnail_dimensions(void)
{
    pc_document d = ch_doc_white(400, 200);
    pc_document small = ch_doc_white(10, 5);
    pc_rgba8_image t;
    CHECK(pc_compose_thumbnail(&d, 100, &t, NULL) == PC_OK);
    CHECK_EQ_UINT(t.width, 100);
    CHECK_EQ_UINT(t.height, 50);
    CHECK(t.pixels[0] == 255 && t.pixels[1] == 255 && t.pixels[2] == 255 && t.pixels[3] == 255);
    pc_rgba8_image_fini(&t);
    CHECK(pc_compose_thumbnail(&small, 100, &t, NULL) == PC_OK);
    CHECK_EQ_UINT(t.width, 10);
    CHECK_EQ_UINT(t.height, 5);
    pc_rgba8_image_fini(&t);
    pc_document_fini(&d);
    pc_document_fini(&small);
}

static void test_buffer_over_background(void)
{
    const float c[4] = {0, 0, 0, 0.5f};
    const float white[3] = {1, 1, 1};
    const float half[4] = {0.5f, 0.5f, 0.5f, 1};
    pc_compose_buffer b;
    pc_compose_buffer o;
    pc_rgba8_image img;
    CHECK(pc_compose_buffer_filled(pc_rect_make(0, 0, 1, 1), c, &b, NULL) == PC_OK);
    CHECK(pc_compose_buffer_clone(&b, &o, NULL) == PC_OK);
    pc_compose_buffer_over_background(&o, white);
    CHECK(ch_close4(o.px[0], half));
    CHECK(pc_compose_buffer_to_rgba8(&b, &img, NULL) == PC_OK);
    CHECK(img.pixels[0] == 0 && img.pixels[1] == 0 && img.pixels[2] == 0 && img.pixels[3] == 128);
    pc_rgba8_image_fini(&img);
    pc_compose_buffer_fini(&b);
    pc_compose_buffer_fini(&o);
}

static void test_parallel_tiles_match_single_pass(void)
{
    /* The reference's document without its stroke effect (C3): masks, clipping, groups, an
     * adjustment, fills. */
    pc_document d = ch_doc_white(97, 61);
    pc_layer *a = ch_push(&d, ch_solid("a", pc_rect_make(5, 5, 70, 50), 1, 0.2f, 0.1f, 0.8f));
    const float q = 0.25f;
    pc_layer in;
    pc_layer g;
    pc_layer f;
    pc_layer_content c;
    pc_rect r;
    pc_compose_buffer whole;
    pc_compose_buffer other;
    const int32_t tiles[3] = {7, 16, 33};
    int i;
    pc_layer_mask_reveal_all(&a->mask);
    a->has_mask = true;
    CHECK(pc_surface_fill_rect(&a->mask.surface, pc_rect_make(0, 0, 40, 61), &q, 1, NULL) == PC_OK);
    a->mask.density = 0.8f;
    a = ch_push(&d, ch_solid("clip", pc_rect_make(30, 0, 97, 61), 0, 1, 0, 1));
    a->clipped = true;
    a->blend = PC_BLEND_MULTIPLY;
    in = ch_solid("in", pc_rect_make(50, 20, 90, 60), 0.2f, 0.3f, 0.9f, 1);
    g = ch_group("g", &in, 1);
    g.opacity = 0.6f;
    ch_push(&d, g);
    ch_push(&d, invert_layer());
    c.kind = PC_LAYER_FILL;
    c.u.fill = pc_fill_solid(pc_color_rgb(0.5f, 0.5f, 0.0f));
    CHECK(pc_layer_init(&f, "fill", &c, NULL) == PC_OK);
    f.opacity = 0.3f;
    ch_push(&d, f);
    r = pc_document_bounds(&d);
    whole = ch_render_tiled(&d, r, 10000);
    for (i = 0; i < 3; i++) {
        other = ch_render_tiled(&d, r, tiles[i]);
        CHECK(pc_compose_buffer_eq(&other, &whole));
        pc_compose_buffer_fini(&other);
    }
    CHECK(pc_compose_render(&d, r, &other, NULL) == PC_OK);
    CHECK(pc_compose_buffer_eq(&other, &whole));
    pc_compose_buffer_fini(&other);
    pc_compose_buffer_fini(&whole);
    pc_document_fini(&d);
}

static void test_vector_mask_combines_with_pixel_mask(void)
{
    pc_document d = ch_doc_white(8, 8);
    pc_layer *l = ch_push(&d, ch_solid("k", pc_rect_make(0, 0, 8, 8), 0, 0, 0, 1));
    const double pts[4][2] = {{0, 0}, {4.5, 0}, {4.5, 8}, {0, 8}};
    pc_path p = ch_polygon(pts, 4);
    const float zero = 0.0f;
    pc_compose_buffer w3;
    pc_compose_buffer w256;
    /* The vector mask reveals x in 0..4 (and half of column 4); the pixel mask hides rows 0..2. */
    pc_vector_mask_init(&l->vector_mask, &p);
    l->has_vector_mask = true;
    pc_layer_mask_reveal_all(&l->mask);
    l->has_mask = true;
    CHECK(pc_surface_fill_rect(&l->mask.surface, pc_rect_make(0, 0, 8, 2), &zero, 1, NULL) == PC_OK);
    CHECK(ch_px_close(&d, 1, 4, 0, 0, 0, 1));
    CHECK(ch_px_close(&d, 6, 4, 1, 1, 1, 1));
    CHECK(ch_px_close(&d, 4, 4, 0.5f, 0.5f, 0.5f, 1));
    CHECK(ch_px_close(&d, 1, 1, 1, 1, 1, 1));
    /* Tiled rendering gives the same result. */
    w3 = ch_render_tiled(&d, pc_document_bounds(&d), 3);
    w256 = ch_render_tiled(&d, pc_document_bounds(&d), 256);
    CHECK(pc_compose_buffer_eq(&w3, &w256));
    pc_compose_buffer_fini(&w3);
    pc_compose_buffer_fini(&w256);
    /* Density and disabling. */
    l = &d.layers[d.nlayers - 1];
    l->vector_mask.density = 0.5f;
    CHECK(ch_px_close(&d, 6, 4, 0.5f, 0.5f, 0.5f, 1));
    l->vector_mask.enabled = false;
    CHECK(ch_px_close(&d, 6, 4, 0, 0, 0, 1));
    /* Without a pixel mask the vector mask alone applies. */
    pc_layer_mask_fini(&l->mask);
    l->has_mask = false;
    l->vector_mask.enabled = true;
    l->vector_mask.density = 1.0f;
    CHECK(ch_px_close(&d, 1, 1, 0, 0, 0, 1));
    CHECK(ch_px_close(&d, 6, 1, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_channel_restrictions_keep_the_backdrop(void)
{
    /* Blue left out (Advanced Blending > Channels: R, G only). The adjustment-layer half is in
     * test_fills.c. */
    pc_document d = ch_doc_white(8, 8);
    pc_layer *l = ch_push(&d, ch_solid("dark", pc_rect_make(0, 0, 8, 8), 0.2f, 0.3f, 0.4f, 1));
    float w[3];
    l->excluded_channels = 4;
    CHECK(ch_px_close(&d, 1, 1, 0.2f, 0.3f, 1, 1));
    /* CMYK documents composite in display RGB: no exact equivalent, ignored. */
    d.layers[1].excluded_channels = 1;
    CHECK(!pc_compose_channel_weights(&d.layers[1], PC_COLOR_MODE_CMYK, w));
    d.layers[1].excluded_channels = 4;
    CHECK(pc_compose_channel_weights(&d.layers[1], PC_COLOR_MODE_RGB, w));
    CHECK(w[0] == 1.0f && w[1] == 1.0f && w[2] == 0.0f);
    pc_document_fini(&d);
}

static pc_blend_range range(uint8_t b0, uint8_t b1, uint8_t w0, uint8_t w1)
{
    pc_blend_range r = {{b0, b1}, {w0, w1}};
    return r;
}

static void set_bi(pc_layer *l, size_t i, pc_blend_range a, pc_blend_range b)
{
    const pc_blend_range pair[2] = {a, b};
    CHECK(pc_blend_if_set(&l->blend_if, i, pair, NULL) == PC_OK);
}

static void test_blend_if_this_layer_hides_by_the_layers_own_value(void)
{
    /* Mid-grey backdrop; the layer is black on the left, white on the right. */
    pc_document d = ch_doc_white(8, 8);
    const float white[4] = {1, 1, 1, 1};
    pc_layer *l;
    ch_push(&d, ch_solid("grey", pc_rect_make(0, 0, 8, 8), 0.5f, 0.5f, 0.5f, 1));
    l = ch_push(&d, ch_solid("bw", pc_rect_make(0, 0, 4, 8), 0, 0, 0, 1));
    ch_fill(l, pc_rect_make(4, 0, 8, 8), white, 4);
    /* Gray > This Layer: black point at 50 hides the blacks. */
    set_bi(l, 0, range(50, 50, 255, 255), PC_BLEND_RANGE_FULL);
    CHECK(ch_px_close(&d, 1, 1, 0.5f, 0.5f, 0.5f, 1));
    CHECK(ch_px_close(&d, 6, 1, 1, 1, 1, 1));
    /* White point at 200 hides the whites as well. */
    set_bi(&d.layers[2], 0, range(50, 50, 200, 200), PC_BLEND_RANGE_FULL);
    CHECK(ch_px_close(&d, 6, 1, 0.5f, 0.5f, 0.5f, 1));
    /* Back to the defaults: everything shows again. */
    set_bi(&d.layers[2], 0, PC_BLEND_RANGE_FULL, PC_BLEND_RANGE_FULL);
    CHECK(pc_blend_if_is_default(&d.layers[2].blend_if));
    CHECK(ch_px_close(&d, 1, 1, 0, 0, 0, 1));
    pc_document_fini(&d);
}

static void test_blend_if_underlying_layer_hides_by_the_backdrop_value(void)
{
    pc_document d = ch_doc_white(8, 8);
    pc_layer *l;
    ch_push(&d, ch_solid("black", pc_rect_make(0, 0, 4, 8), 0, 0, 0, 1));
    l = ch_push(&d, ch_solid("red", pc_rect_make(0, 0, 8, 8), 1, 0, 0, 1));
    /* Gray > Underlying Layer: white point at 128 = only over the darks. */
    set_bi(l, 0, PC_BLEND_RANGE_FULL, range(0, 0, 128, 128));
    CHECK(ch_px_close(&d, 1, 1, 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 6, 1, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_blend_if_split_point_fades_like_opacity(void)
{
    /* A split black point at 50/150 shows a value-100 layer at half strength: exactly the layer
     * at 50 % opacity, also over a semi-transparent backdrop. */
    float v = 100.0f / 255.0f;
    pc_document d = ch_doc_white(4, 4);
    pc_document d2;
    const float bgc[4] = {0.0f, 0.2f, 1.0f, 0.5f};
    pc_layer l;
    pc_layer half;
    float a[4];
    float b[4];
    int i;
    bool ok = true;
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&d.layers[0]), pc_rect_make(0, 0, 4, 4), bgc, 4, NULL) ==
          PC_OK);
    l = ch_solid("v", pc_rect_make(0, 0, 4, 4), v, v, v, 1);
    l.blend = PC_BLEND_MULTIPLY;
    CHECK(pc_layer_clone(&l, &half, NULL) == PC_OK);
    half.opacity = 0.5f;
    set_bi(&l, 0, range(50, 150, 255, 255), PC_BLEND_RANGE_FULL);
    CHECK(pc_document_clone(&d, &d2, NULL) == PC_OK);
    ch_push(&d, l);
    ch_push(&d2, half);
    ch_px(&d, 1, 1, a);
    ch_px(&d2, 1, 1, b);
    for (i = 0; i < 4; i++) {
        ok = ok && fabsf(a[i] - b[i]) < 1e-4f;
    }
    CHECK(ok);
    pc_document_fini(&d);
    pc_document_fini(&d2);
}

static void test_blend_if_per_channel_ranges(void)
{
    pc_document d = ch_doc_white(8, 8);
    const float blue[4] = {0, 0, 1, 1};
    pc_layer *l = ch_push(&d, ch_solid("c", pc_rect_make(0, 0, 4, 8), 1, 0, 0, 1));
    ch_fill(l, pc_rect_make(4, 0, 8, 8), blue, 4);
    /* Blue > This Layer: hide pixels whose blue is above 100. */
    set_bi(l, 3, range(0, 0, 100, 100), PC_BLEND_RANGE_FULL);
    CHECK(ch_px_close(&d, 1, 1, 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 6, 1, 1, 1, 1, 1));
    /* Red > Underlying Layer: the backdrop's red (255) is above 254, hidden everywhere. */
    pc_blend_if_fini(&d.layers[1].blend_if);
    set_bi(&d.layers[1], 1, PC_BLEND_RANGE_FULL, range(0, 0, 254, 254));
    CHECK(ch_px_close(&d, 1, 1, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_blend_if_on_clipped_layers(void)
{
    /* A clipped layer judges "underlying" by its clipping base. (The adjustment-layer half is in
     * test_fills.c.) */
    pc_document d = ch_doc_white(8, 8);
    const float light[4] = {0.9f, 0.9f, 0.9f, 1};
    pc_layer *base = ch_push(&d, ch_solid("base", pc_rect_make(0, 0, 8, 8), 0, 0, 0, 1));
    pc_layer *clip;
    ch_fill(base, pc_rect_make(4, 0, 8, 8), light, 4);
    clip = ch_push(&d, ch_solid("clip", pc_rect_make(0, 0, 8, 8), 0, 1, 0, 1));
    clip->clipped = true;
    set_bi(clip, 0, PC_BLEND_RANGE_FULL, range(128, 128, 255, 255));
    CHECK(ch_px_close(&d, 1, 1, 0, 0, 0, 1));
    CHECK(ch_px_close(&d, 6, 1, 0, 1, 0, 1));
    pc_document_fini(&d);
}

static void test_blend_if_modes(void)
{
    pc_document d;
    pc_layer l;
    const float k[2] = {0, 1};
    CHECK(pc_document_with_background(&d, "g", pc_size_make(4, 4), PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8,
                                      PC_COLOR_WHITE, NULL) == PC_OK);
    l = ch_raster("k", pc_document_pixel_format(&d));
    ch_fill(&l, pc_rect_make(0, 0, 4, 4), k, 2);
    set_bi(&l, 1, range(10, 10, 255, 255), PC_BLEND_RANGE_FULL);
    ch_push(&d, l);
    CHECK(ch_px_close(&d, 1, 1, 1, 1, 1, 1));
    /* CMYK / Lab composite in display RGB: kept for round trip, not applied. */
    CHECK(pc_compose_blend_if_active(&d.layers[1], PC_COLOR_MODE_RGB));
    CHECK(pc_compose_blend_if_active(&d.layers[1], PC_COLOR_MODE_GRAYSCALE));
    CHECK(!pc_compose_blend_if_active(&d.layers[1], PC_COLOR_MODE_CMYK));
    CHECK(!pc_compose_blend_if_active(&d.layers[1], PC_COLOR_MODE_LAB));
    CHECK(!pc_compose_blend_if_active(&d.layers[0], PC_COLOR_MODE_RGB));
    pc_document_fini(&d);
}

/* A tall document with soft content, a translucent region and an adjustment (tall_doc). */
static pc_document tall_doc(uint32_t w, uint32_t h)
{
    pc_document d = ch_doc_white(w, h);
    pc_layer l = ch_raster("grad", PC_PIXEL_FORMAT_RGBA8);
    const float red[4] = {0.9f, 0.1f, 0.1f, 0.4f};
    int32_t y;
    for (y = 0; y < (int32_t)h; y++) {
        const float px[4] = {(float)y / (float)h, 0.3f, 1.0f - (float)y / (float)h, 1.0f};
        ch_fill(&l, pc_rect_make(0, y, (int32_t)w / 2, y + 1), px, 4);
    }
    ch_fill(&l, pc_rect_make((int32_t)w / 2, 0, (int32_t)w, (int32_t)h / 3), red, 4);
    ch_push(&d, l);
    ch_push(&d, invert_layer());
    return d;
}

static void test_render_tiled_in_place_matches_one_tile_with_many_bands(void)
{
    pc_document d = tall_doc(37, 900);
    pc_compose_buffer whole = ch_render_tiled(&d, pc_document_bounds(&d), 10000);
    const int32_t tiles[3] = {5, 64, 300};
    pc_compose_buffer a;
    pc_compose_buffer b;
    int i;
    for (i = 0; i < 3; i++) {
        a = ch_render_tiled(&d, pc_document_bounds(&d), tiles[i]);
        CHECK(pc_compose_buffer_eq(&a, &whole));
        pc_compose_buffer_fini(&a);
    }
    a = ch_render_tiled(&d, pc_rect_make(3, 17, 30, 811), 4);
    b = ch_render_tiled(&d, pc_rect_make(3, 17, 30, 811), 10000);
    CHECK(pc_compose_buffer_eq(&a, &b));
    pc_compose_buffer_fini(&a);
    pc_compose_buffer_fini(&b);
    a = ch_render_tiled(&d, pc_rect_make(5, 5, 5, 900), 4);
    CHECK(a.len == 0);
    pc_compose_buffer_fini(&a);
    pc_compose_buffer_fini(&whole);
    pc_document_fini(&d);
}

typedef struct band_log {
    int32_t rows[8][2];
    size_t n;
    pc_rect r;
    float (*px)[4];
    size_t len;
    int stop_after;
} band_log;

static pc_status band_sink(void *ctx, pc_compose_buffer *b, pc_error *err)
{
    band_log *l = ctx;
    CHECK(b->rect.x0 == l->r.x0 && b->rect.x1 == l->r.x1);
    if (l->n < 8) {
        l->rows[l->n][0] = b->rect.y0;
        l->rows[l->n][1] = b->rect.y1;
    }
    l->n++;
    if (l->stop_after > 0 && (int)l->n >= l->stop_after) {
        return pc_fail(err, PC_ERR_CANCELLED, "stop");
    }
    if (l->px != NULL) {
        memcpy(l->px + l->len, b->px, b->len * sizeof *b->px);
        l->len += b->len;
    }
    return PC_OK;
}

static void test_render_bands_cover_the_rect_in_order(void)
{
    pc_document d = tall_doc(20, 700);
    pc_rect r = pc_rect_make(2, 10, 18, 690);
    band_log log;
    pc_compose_buffer whole;
    pc_error e;
    memset(&log, 0, sizeof log);
    log.r = r;
    log.px = calloc(16 * 680, sizeof *log.px);
    CHECK(pc_compose_render_bands(&d, r, 100, NULL, band_sink, &log, NULL) == PC_OK);
    /* 100 rows round up to one 256-row tile band. */
    CHECK(log.n == 3);
    CHECK(log.rows[0][0] == 10 && log.rows[0][1] == 266);
    CHECK(log.rows[1][0] == 266 && log.rows[1][1] == 522);
    CHECK(log.rows[2][0] == 522 && log.rows[2][1] == 690);
    CHECK(pc_compose_render(&d, r, &whole, NULL) == PC_OK);
    CHECK(log.len == whole.len && memcmp(log.px, whole.px, whole.len * sizeof *whole.px) == 0);
    pc_compose_buffer_fini(&whole);
    free(log.px);
    /* The sink's error stops the render. */
    memset(&log, 0, sizeof log);
    log.r = r;
    log.stop_after = 1;
    CHECK(pc_compose_render_bands(&d, r, 1, NULL, band_sink, &log, &e) == PC_ERR_CANCELLED);
    CHECK(log.n == 1);
    CHECK(pc_compose_band_rows_for(14000, 0) % PC_COMPOSE_RENDER_TILE == 0);
    CHECK(pc_compose_band_rows_for(1, 0) > 0 && pc_compose_band_rows_for(0, -5) > 0);
    pc_document_fini(&d);
}

/* The previous thumbnail: flatten, then area-average (premultiplied). */
static float (*reference_reduce(const pc_document *doc, size_t w, size_t h))[4]
{
    pc_compose_buffer full;
    float (*out)[4] = calloc(w * h, sizeof *out);
    size_t fw;
    size_t fh;
    size_t tx;
    size_t ty;
    if (out == NULL || pc_compose_flatten(doc, &full, NULL) != PC_OK) {
        free(out);
        return NULL;
    }
    fw = (size_t)pc_rect_width(full.rect);
    fh = (size_t)pc_rect_height(full.rect);
    for (ty = 0; ty < h; ty++) {
        size_t y0 = ty * fh / h;
        size_t y1 = (ty + 1) * fh / h;
        y1 = y1 < y0 + 1 ? y0 + 1 : y1;
        y1 = y1 < fh ? y1 : fh;
        for (tx = 0; tx < w; tx++) {
            size_t x0 = tx * fw / w;
            size_t x1 = (tx + 1) * fw / w;
            float acc[4] = {0, 0, 0, 0};
            float n;
            size_t x;
            size_t y;
            x1 = x1 < x0 + 1 ? x0 + 1 : x1;
            x1 = x1 < fw ? x1 : fw;
            for (y = y0; y < y1; y++) {
                for (x = x0; x < x1; x++) {
                    const float *p = full.px[y * fw + x];
                    int c;
                    for (c = 0; c < 3; c++) {
                        acc[c] += p[c] * p[3];
                    }
                    acc[3] += p[3];
                }
            }
            n = (float)((y1 - y0) * (x1 - x0) > 0 ? (y1 - y0) * (x1 - x0) : 1);
            if (acc[3] > 0.0f) {
                out[ty * w + tx][0] = acc[0] / acc[3];
                out[ty * w + tx][1] = acc[1] / acc[3];
                out[ty * w + tx][2] = acc[2] / acc[3];
                out[ty * w + tx][3] = acc[3] / n;
            }
        }
    }
    pc_compose_buffer_fini(&full);
    return out;
}

static void test_reduced_render_matches_the_full_composite_averaged(void)
{
    pc_document d = tall_doc(53, 1000);
    const uint32_t cases[4][3] = {{10, 190, 0}, {7, 33, 256}, {53, 999, 512}, {1, 1, 256}};
    pc_compose_buffer got;
    pc_compose_buffer full;
    pc_document empty;
    int i;
    for (i = 0; i < 4; i++) {
        float (*want)[4] = reference_reduce(&d, cases[i][0], cases[i][1]);
        CHECK(pc_compose_render_reduced_in_bands(&d, pc_document_bounds(&d), cases[i][0], cases[i][1], NULL,
                                                 (int32_t)cases[i][2], &got, NULL) == PC_OK);
        CHECK(pc_rect_width(got.rect) == cases[i][0] && pc_rect_height(got.rect) == cases[i][1]);
        CHECK(want != NULL && memcmp(got.px, want, got.len * sizeof *want) == 0);
        free(want);
        pc_compose_buffer_fini(&got);
    }
    /* Full size is the composite itself; an empty document reduces to transparency. */
    CHECK(pc_compose_render_reduced(&d, 53, 1000, &got, NULL) == PC_OK);
    CHECK(pc_compose_flatten(&d, &full, NULL) == PC_OK);
    CHECK(got.len == full.len && memcmp(got.px, full.px, full.len * sizeof *full.px) == 0);
    pc_compose_buffer_fini(&got);
    pc_compose_buffer_fini(&full);
    empty = ch_doc(0, 0, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    CHECK(pc_compose_render_reduced(&empty, 4, 4, &got, NULL) == PC_OK);
    CHECK(got.len == 1 && got.px[0][0] == 0 && got.px[0][3] == 0);
    pc_compose_buffer_fini(&got);
    pc_document_fini(&empty);
    pc_document_fini(&d);
}

static void span(uint32_t t, uint32_t f, uint32_t n, uint32_t *a, uint32_t *z)
{
    uint32_t lo = t * f / n;
    uint32_t hi = (t + 1) * f / n;
    hi = hi < lo + 1 ? lo + 1 : hi;
    *a = lo;
    *z = hi < f ? hi : f;
}

static void test_reduced_damage_matches_the_whole_reduction(void)
{
    pc_document d = tall_doc(53, 1000);
    const uint32_t cases[5][3] = {{10, 190, 0}, {7, 33, 256}, {53, 999, 64}, {53, 1000, 0}, {1, 1, 256}};
    const pc_rect dmgs[5] = {
        {0, 0, 1, 1}, {5, 17, 6, 18}, {12, 300, 40, 701}, {-9, 990, 80, 2000}, {0, 0, 53, 1000}};
    int c;
    int k;
    for (c = 0; c < 5; c++) {
        uint32_t w = cases[c][0];
        uint32_t h = cases[c][1];
        int32_t band = (int32_t)cases[c][2];
        pc_compose_buffer all;
        pc_compose_buffer part;
        pc_rect miss = {60, 0, 70, 10};
        CHECK(pc_compose_render_reduced_in_bands(&d, pc_document_bounds(&d), w, h, NULL, band, &all, NULL) ==
              PC_OK);
        for (k = 0; k < 5; k++) {
            pc_rect r;
            pc_rect dd = pc_rect_intersect(dmgs[k], pc_rect_make(0, 0, 53, 1000));
            uint32_t x;
            uint32_t y;
            bool same = true;
            bool exact = true;
            CHECK(pc_compose_render_reduced_in_bands(&d, pc_document_bounds(&d), w, h, &dmgs[k], band, &part,
                                                     NULL) == PC_OK);
            r = part.rect;
            CHECK(!pc_rect_is_empty(r) && r.x0 >= 0 && r.y0 >= 0 && (uint32_t)r.x1 <= w &&
                  (uint32_t)r.y1 <= h);
            for (y = (uint32_t)r.y0; y < (uint32_t)r.y1; y++) {
                for (x = (uint32_t)r.x0; x < (uint32_t)r.x1; x++) {
                    const float *a = pc_compose_buffer_get(&part, (int32_t)x, (int32_t)y);
                    const float *b = pc_compose_buffer_get(&all, (int32_t)x, (int32_t)y);
                    same = same && a != NULL && b != NULL && memcmp(a, b, 16) == 0;
                }
            }
            CHECK(same);
            /* Exactly the output pixels whose source area meets the damage. */
            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    uint32_t x0;
                    uint32_t x1;
                    uint32_t y0;
                    uint32_t y1;
                    bool meets;
                    span(x, 53, w, &x0, &x1);
                    span(y, 1000, h, &y0, &y1);
                    meets = !pc_rect_is_empty(pc_rect_intersect(
                        pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1), dd));
                    exact = exact && pc_rect_contains(r, (int32_t)x, (int32_t)y) == meets;
                }
            }
            CHECK(exact);
            pc_compose_buffer_fini(&part);
        }
        CHECK(pc_compose_render_reduced_in_bands(&d, pc_document_bounds(&d), w, h, &miss, band, &part,
                                                 NULL) == PC_OK);
        CHECK(pc_rect_is_empty(part.rect));
        pc_compose_buffer_fini(&part);
        pc_compose_buffer_fini(&all);
    }
    pc_document_fini(&d);
}

static void paint_big(pc_document *d, int32_t dx, int32_t dy)
{
    pc_layer l = ch_raster("big", PC_PIXEL_FORMAT_RGBA8);
    int32_t x;
    int32_t y;
    for (y = -10; y < 40; y++) {
        for (x = -20; x < 60; x++) {
            float a = (x < 0 || y >= 30) ? 0.6f : 1.0f;
            const float px[4] = {(float)(x + 20) / 80.0f, (float)(y + 10) / 50.0f, 0.5f, a};
            ch_fill(&l, pc_rect_make(x + dx, y + dy, x + dx + 1, y + dy + 1), px, 4);
        }
    }
    ch_push(d, l);
    /* Adjustment layers apply out there too. */
    ch_push(d, invert_layer());
}

static void test_reduced_render_of_an_area_past_the_canvas(void)
{
    pc_document d = ch_doc(40, 30, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document whole = ch_doc(80, 50, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_rect area = pc_rect_make(-20, -10, 60, 40);
    const uint32_t sizes[4][2] = {{80, 50}, {8, 5}, {27, 13}, {1, 1}};
    pc_compose_buffer got;
    pc_compose_buffer want;
    size_t i;
    bool clear = true;
    paint_big(&d, 0, 0);
    paint_big(&whole, 20, 10);
    for (i = 0; i < 4; i++) {
        CHECK(pc_compose_render_reduced_rect(&d, area, sizes[i][0], sizes[i][1], &got, NULL) == PC_OK);
        CHECK(pc_rect_eq(got.rect, pc_rect_make(0, 0, (int32_t)sizes[i][0], (int32_t)sizes[i][1])));
        CHECK(pc_compose_render_reduced(&whole, sizes[i][0], sizes[i][1], &want, NULL) == PC_OK);
        CHECK(got.len == want.len && memcmp(got.px, want.px, got.len * sizeof *got.px) == 0);
        pc_compose_buffer_fini(&got);
        pc_compose_buffer_fini(&want);
    }
    /* Past every layer there is nothing to show. */
    CHECK(pc_compose_render_reduced_rect(&d, pc_rect_make(300, 300, 320, 310), 20, 10, &got, NULL) == PC_OK);
    for (i = 0; i < got.len; i++) {
        clear = clear && got.px[i][3] == 0.0f;
    }
    CHECK(clear);
    pc_compose_buffer_fini(&got);
    pc_document_fini(&d);
    pc_document_fini(&whole);
}

static void test_large_documents_thumbnail_from_a_proxy(void)
{
    /* 4200 x 4200 (17.6 MP) is above PC_COMPOSE_PROXY_THUMBNAIL_PIXELS: the proxy path must
     * still give a thumbnail of the right size whose colours match the exact reduction. */
    pc_document d = ch_doc_white(4200, 4200);
    pc_rgba8_image t;
    pc_effect fx;
    pc_layer *l;
    ch_push(&d, ch_solid("red", pc_rect_make(0, 0, 2100, 4200), 1, 0, 0, 1));
    CHECK(pc_compose_thumbnail(&d, 100, &t, NULL) == PC_OK);
    CHECK(t.width == 100 && t.height == 100);
    if (t.pixels != NULL) {
        const uint8_t *a = t.pixels + (50 * 100 + 10) * 4;
        const uint8_t *b = t.pixels + (50 * 100 + 90) * 4;
        CHECK(a[0] == 255 && a[1] == 0 && a[2] == 0 && a[3] == 255);
        CHECK(b[0] == 255 && b[1] == 255 && b[2] == 255 && b[3] == 255);
    }
    pc_rgba8_image_fini(&t);
    /* Effects don't scale with a proxy: such documents reduce the exact composite. */
    CHECK(pc_compose_proxy_faithful(&d));
    l = &d.layers[1];
    pc_effect_default_drop_shadow(&fx);
    {
        pc_effect *items = realloc(l->effects.items, (l->effects.nitems + 1) * sizeof *items);
        if (items != NULL) {
            items[l->effects.nitems++] = fx;
            l->effects.items = items;
        } else {
            pc_effect_fini(&fx);
        }
    }
    CHECK(!pc_compose_proxy_faithful(&d));
    pc_document_fini(&d);
}

static void test_artboard_clips_contents_and_paints_background(void)
{
    /* lib.rs composite_artboard: the board's background, then the group, only inside the board.
     * (crates/compose/tests/artboard.rs is C3's integration test.) */
    pc_document d = ch_doc(8, 8, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer kid = ch_solid("k", pc_rect_make(0, 0, 8, 8), 1, 0, 0, 0.5f);
    pc_layer g = ch_group("ab", &kid, 1);
    g.blend = PC_BLEND_NORMAL;
    pc_artboard_init(&g.content.u.group.artboard, pc_rect_make(2, 2, 6, 6));
    g.content.u.group.has_artboard = true;
    ch_push(&d, g);
    CHECK(ch_px_close(&d, 0, 0, 0, 0, 0, 0));       /* outside: untouched */
    CHECK(ch_px_close(&d, 3, 3, 1, 0.5f, 0.5f, 1)); /* red at 50 % over the white board */
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_background_only);
    RUN(test_normal_layer_over_background);
    RUN(test_opacity_and_fill_multiply);
    RUN(test_hidden_layers_skipped);
    RUN(test_every_blend_mode_matches_reference_on_opaque_pixels);
    RUN(test_layer_mask_hides_pixels);
    RUN(test_disabled_mask_is_ignored);
    RUN(test_clipping_mask_restricts_to_base_alpha);
    RUN(test_hidden_base_hides_clipping_group);
    RUN(test_isolated_group_differs_from_pass_through);
    RUN(test_mask_feather_blurs_the_edge);
    RUN(test_group_opacity_applies_once);
    RUN(test_pass_through_group_opacity_mixes);
    RUN(test_solid_fill_layer);
    RUN(test_dissolve_coverage_matches_opacity);
    RUN(test_render_is_tile_independent);
    RUN(test_cmyk_document_renders_via_rgb);
    RUN(test_sixteen_bit_and_float_layers_composite);
    RUN(test_float_documents_add_and_divide_past_white);
    RUN(test_integer_documents_clip_add_and_divide_at_white);
    RUN(test_thumbnail_dimensions);
    RUN(test_buffer_over_background);
    RUN(test_parallel_tiles_match_single_pass);
    RUN(test_vector_mask_combines_with_pixel_mask);
    RUN(test_channel_restrictions_keep_the_backdrop);
    RUN(test_blend_if_this_layer_hides_by_the_layers_own_value);
    RUN(test_blend_if_underlying_layer_hides_by_the_backdrop_value);
    RUN(test_blend_if_split_point_fades_like_opacity);
    RUN(test_blend_if_per_channel_ranges);
    RUN(test_blend_if_on_clipped_layers);
    RUN(test_blend_if_modes);
    RUN(test_render_tiled_in_place_matches_one_tile_with_many_bands);
    RUN(test_render_bands_cover_the_rect_in_order);
    RUN(test_reduced_render_matches_the_full_composite_averaged);
    RUN(test_reduced_damage_matches_the_whole_reduction);
    RUN(test_reduced_render_of_an_area_past_the_canvas);
    RUN(test_large_documents_thumbnail_from_a_proxy);
    RUN(test_artboard_clips_contents_and_paints_background);
    return pc_t_finish();
}
