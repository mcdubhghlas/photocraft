/* The document-level C2 tests of crates/compose/src/tests.rs: adjustment layers in documents
 * (opacity, masks, clipping, pass-through groups, channel restrictions, Blend If, rounding to the
 * document depth), gradient and pattern fill layers, shape-layer gradients. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "compose/adjust.h"
#include "compose/gradient_fill.h"
#include "compose/pattern.h"
#include "vector/vector.h"

static pc_layer adj_layer(pc_adjustment a)
{
    pc_layer l;
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_ADJUSTMENT;
    c.u.adjustment = a;
    if (pc_layer_init(&l, "adj", &c, NULL) != PC_OK) {
        abort();
    }
    return l;
}

static pc_layer invert(void)
{
    pc_adjustment a;
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_INVERT;
    return adj_layer(a);
}

static pc_layer fill_layer(pc_fill f)
{
    pc_layer l;
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_FILL;
    c.u.fill = f;
    if (pc_layer_init(&l, "fill", &c, NULL) != PC_OK) {
        abort();
    }
    return l;
}

static void fill_bg(pc_document *d, pc_rect r, float cr, float cg, float cb)
{
    const float px[4] = {cr, cg, cb, 1};
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&d->layers[0]), r, px, 4, NULL) == PC_OK);
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

/* Below 100 % fill Photoshop renders a pass-through group isolated, like Normal, so an
 * adjustment inside it no longer changes the layers beneath. */
static float pass_through_at(float fill)
{
    pc_document d = ch_doc_white(2, 2);
    pc_layer inv = invert();
    pc_layer g = ch_group("g", &inv, 1);
    float p[4];
    g.blend = PC_BLEND_PASS_THROUGH;
    g.fill_opacity = fill;
    ch_push(&d, g);
    ch_px(&d, 0, 0, p);
    pc_document_fini(&d);
    return p[0];
}

static void test_pass_through_group_below_full_fill_is_isolated(void)
{
    CHECK(fabsf(pass_through_at(1.0f)) <= CH_E);        /* the invert reaches the background */
    CHECK(fabsf(pass_through_at(0.4f) - 1.0f) <= CH_E); /* isolated: nothing beneath to invert */
}

static void test_invert_adjustment_layer(void)
{
    pc_document d = ch_doc_white(2, 2);
    fill_bg(&d, pc_rect_make(0, 0, 2, 2), 0.2f, 0.4f, 0.6f);
    ch_push(&d, invert());
    CHECK(ch_px_close(&d, 0, 0, 0.8f, 0.6f, 0.4f, 1));
    pc_document_fini(&d);
}

static void test_adjustment_opacity_and_mask(void)
{
    pc_document d = ch_doc_white(4, 1);
    pc_layer adj = invert();
    const float zero = 0.0f;
    float p[4];
    fill_bg(&d, pc_rect_make(0, 0, 4, 1), 0, 0, 0);
    adj.opacity = 0.5f;
    pc_layer_mask_reveal_all(&adj.mask);
    adj.has_mask = true;
    CHECK(pc_surface_fill_rect(&adj.mask.surface, pc_rect_make(2, 0, 4, 1), &zero, 1, NULL) == PC_OK);
    ch_push(&d, adj);
    ch_px(&d, 0, 0, p);
    CHECK(fabsf(p[0] - 0.5f) <= CH_E);
    ch_px(&d, 3, 0, p);
    CHECK(fabsf(p[0]) <= CH_E);
    pc_document_fini(&d);
}

static void test_clipped_adjustment_only_affects_base(void)
{
    pc_document d = ch_doc_white(4, 1);
    pc_layer adj = invert();
    ch_push(&d, ch_solid("base", pc_rect_make(0, 0, 2, 1), 0, 0, 0, 1));
    adj.clipped = true;
    ch_push(&d, adj);
    CHECK(ch_px_close(&d, 0, 0, 1, 1, 1, 1)); /* base inverted to white */
    CHECK(ch_px_close(&d, 3, 0, 1, 1, 1, 1)); /* background untouched */
    pc_document_fini(&d);
}

static void test_solid_and_gradient_fill_layers(void)
{
    pc_document d = ch_doc_white(10, 1);
    pc_color_stop stops[2] = {{0.0f, PC_COLOR_BLACK}, {1.0f, PC_COLOR_WHITE}};
    pc_fill g;
    pc_compose_buffer buf;
    pc_compose_buffer one;
    ch_push(&d, fill_layer(pc_fill_solid(pc_color_rgb(0, 0, 1))));
    CHECK(ch_px_close(&d, 5, 0, 0, 0, 1, 1));
    CHECK(pc_fill_gradient(stops, 2, 0.0f, 1.0f, PC_GRADIENT_LINEAR, false, &g, NULL) == PC_OK);
    CHECK(pc_compose_gradient_fill_render(&g, pc_rect_make(0, 0, 10, 1), pc_rect_make(0, 0, 10, 1), &buf,
                                          NULL) == PC_OK);
    /* Tile independence: a 1 px render of the right edge equals the full render. */
    CHECK(pc_compose_gradient_fill_render(&g, pc_rect_make(9, 0, 10, 1), pc_rect_make(0, 0, 10, 1), &one,
                                          NULL) == PC_OK);
    CHECK(memcmp(one.px[0], buf.px[9], sizeof buf.px[9]) == 0);
    CHECK(buf.px[0][0] < buf.px[9][0]); /* left dark, right light */
    pc_compose_buffer_fini(&buf);
    pc_compose_buffer_fini(&one);
    pc_fill_fini(&g);
    pc_document_fini(&d);
}

static void test_channel_restrictions_on_adjustment_layers(void)
{
    /* The adjustment-layer half of channel_restrictions_keep_the_backdrop. */
    pc_document d = ch_doc_white(8, 8);
    pc_layer *l = ch_push(&d, ch_solid("dark", pc_rect_make(0, 0, 8, 8), 0.2f, 0.3f, 0.4f, 1));
    pc_layer inv = invert();
    l->excluded_channels = 4;
    CHECK(ch_px_close(&d, 1, 1, 0.2f, 0.3f, 1, 1));
    inv.excluded_channels = 1; /* adjustment layers honour it too */
    ch_push(&d, inv);
    CHECK(ch_px_close(&d, 1, 1, 0.2f, 0.7f, 0.0f, 1));
    pc_document_fini(&d);
}

static void test_blend_if_on_adjustment_layers(void)
{
    /* Invert, but only where the backdrop is dark (Underlying white point at 128). */
    pc_document d = ch_doc_white(8, 8);
    pc_layer inv = invert();
    ch_push(&d, ch_solid("dark", pc_rect_make(0, 0, 4, 8), 0.1f, 0.1f, 0.1f, 1));
    set_bi(&inv, 0, PC_BLEND_RANGE_FULL, range(0, 0, 128, 128));
    ch_push(&d, inv);
    CHECK(ch_px_close(&d, 1, 1, 0.9f, 0.9f, 0.9f, 1));
    CHECK(ch_px_close(&d, 6, 1, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_adjustment_results_are_rounded_to_the_document_depth(void)
{
    static const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    static const float qs[3] = {255.0f, 32768.0f, 0.0f};
    size_t i;
    int c;
    for (i = 0; i < 3; i++) {
        pc_document d;
        pc_adjustment a;
        float p[4];
        memset(&a, 0, sizeof a);
        a.kind = PC_ADJ_EXPOSURE;
        a.u.exposure.exposure = 0.37f;
        a.u.exposure.gamma = 1.0f;
        CHECK(pc_document_with_background(&d, "q", pc_size_make(4, 4), PC_COLOR_MODE_RGB, depths[i],
                                          pc_color_rgb(0.3f, 0.6f, 0.9f), NULL) == PC_OK);
        ch_push(&d, adj_layer(a));
        ch_px(&d, 1, 1, p);
        if (qs[i] > 0.0f) {
            bool ok = true;
            for (c = 0; c < 4; c++) {
                ok = ok && fabsf(roundf(p[c] * qs[i]) - p[c] * qs[i]) < 1e-3f;
            }
            CHECK(ok);
        } else {
            bool any = false;
            for (c = 0; c < 3; c++) {
                any = any || fabsf(roundf(p[c] * 255.0f) - p[c] * 255.0f) > 1e-3f;
            }
            CHECK(any);
        }
        pc_document_fini(&d);
    }
}

static void test_exposure_scales_32_bit_samples_as_stored(void)
{
    /* 32-bit samples are linear light: +1 stop doubles them as stored. */
    static const pc_sample_type depths[2] = {PC_SAMPLE_TYPE_F32, PC_SAMPLE_TYPE_U8};
    static const float want[2] = {0.4f, 0.2744f};
    size_t i;
    for (i = 0; i < 2; i++) {
        pc_document d = ch_doc(1, 1, PC_COLOR_MODE_RGB, depths[i]);
        pc_pixel_format f = PC_PIXEL_FORMAT_RGBA8;
        pc_layer l;
        pc_adjustment a;
        const float px[4] = {0.2f, 0.2f, 0.2f, 1};
        float p[4];
        f.sample = depths[i];
        l = ch_raster("px", f);
        ch_fill(&l, pc_rect_make(0, 0, 1, 1), px, 4);
        ch_push(&d, l);
        memset(&a, 0, sizeof a);
        a.kind = PC_ADJ_EXPOSURE;
        a.u.exposure.exposure = 1.0f;
        a.u.exposure.gamma = 1.0f;
        ch_push(&d, adj_layer(a));
        ch_px(&d, 0, 0, p);
        CHECK(fabsf(p[0] - want[i]) < 2e-3f);
        pc_document_fini(&d);
    }
}

static void test_small_gradient_fill_matches_photoshop_at_all_depths(void)
{
    /* A 30 degree Reflected gradient fill on a 4 x 4 canvas renders as Photoshop's (its end point
     * snaps to the corner: t = |x - y| / 4) at every depth. */
    static const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    pc_color_stop stops[2] = {{0.0f, PC_COLOR_BLACK}, {1.0f, PC_COLOR_WHITE}};
    size_t i;
    int x;
    int y;
    for (i = 0; i < 3; i++) {
        pc_document d = ch_doc(4, 4, PC_COLOR_MODE_RGB, depths[i]);
        pc_fill f;
        pc_compose_buffer out;
        bool ok = true;
        CHECK(pc_fill_gradient(stops, 2, 30.0f, 1.0f, PC_GRADIENT_REFLECTED, false, &f, NULL) == PC_OK);
        ch_push(&d, fill_layer(f));
        CHECK(pc_compose_flatten(&d, &out, NULL) == PC_OK);
        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++) {
                float want = fabsf((float)x - (float)y) / 4.0f;
                ok = ok && fabsf(out.px[y * 4 + x][0] - want) < 1.0f / 255.0f;
            }
        }
        CHECK(ok);
        pc_compose_buffer_fini(&out);
        pc_document_fini(&d);
    }
}

/* A shape layer's gradient fill renders the same pixels as gradient fill layers for every
 * non-default gradient field and all five styles, in both frames (vector carries a copy of the
 * ramp and geometry; this keeps the two in step on interior pixels). */
static void test_shape_layer_gradients_match_fill_layers(void)
{
    static const pc_gradient_style styles[5] = {PC_GRADIENT_LINEAR, PC_GRADIENT_RADIAL, PC_GRADIENT_ANGLE,
                                                PC_GRADIENT_REFLECTED, PC_GRADIENT_DIAMOND};
    pc_rect canvas = pc_rect_make(0, 0, 64, 48);
    pc_rect layer_frame = pc_rect_make(16, 8, 64, 48);
    pc_path path;
    size_t s;
    int c;
    int align;
    CHECK(pc_vec_shape_rect(16.0, 8.0, 48.0, 40.0, &path, NULL) == PC_OK);
    for (s = 0; s < 5; s++) {
        for (c = 0; c < 6; c++) {
            for (align = 0; align < 2; align++) {
                pc_color_stop stops[3] = {
                    {0.0f, PC_COLOR_BLACK}, {0.5f, pc_color_rgb(1, 0.25f, 0.5f)}, {1.0f, PC_COLOR_WHITE}};
                float mids2[2] = {0.25f, 0.9f};
                float mids1[1] = {0.75f};
                pc_opacity_stop op3[3] = {{0, 1}, {0.5f, 0.2f}, {1, 0.9f}};
                pc_opacity_stop op2[2] = {{0, 1}, {1, 0.4f}};
                pc_fill f;
                pc_shape_layer sh;
                pc_surface surf;
                pc_compose_buffer want;
                float worst = 0.0f;
                int x;
                int y;
                int ch;
                memset(&f, 0, sizeof f);
                f.kind = PC_FILL_GRADIENT;
                if (c == 5) { /* unsorted */
                    pc_color_stop t = stops[0];
                    stops[0] = stops[2];
                    stops[2] = t;
                }
                f.u.gradient.stops = stops;
                f.u.gradient.nstops = 3;
                f.u.gradient.angle = 30.0f;
                f.u.gradient.scale = 1.7f;
                f.u.gradient.style = styles[s];
                f.u.gradient.align = align == 0;
                if (c == 1) {
                    f.u.gradient.midpoints = mids2;
                    f.u.gradient.nmidpoints = 2;
                } else if (c == 5) {
                    f.u.gradient.midpoints = mids1;
                    f.u.gradient.nmidpoints = 1;
                }
                if (c == 2) {
                    f.u.gradient.opacity_stops = op3;
                    f.u.gradient.nopacity_stops = 3;
                } else if (c == 5) {
                    f.u.gradient.opacity_stops = op2;
                    f.u.gradient.nopacity_stops = 2;
                }
                if (c == 3) {
                    f.u.gradient.offset[0] = 0.3f;
                    f.u.gradient.offset[1] = -0.2f;
                } else if (c == 5) {
                    f.u.gradient.offset[0] = 0.1f;
                    f.u.gradient.offset[1] = 0.1f;
                }
                f.u.gradient.dither = c == 4 || c == 5;
                CHECK(pc_compose_gradient_fill_render(&f, canvas, align == 0 ? layer_frame : canvas, &want,
                                                      NULL) == PC_OK);
                memset(&sh, 0, sizeof sh);
                sh.path = path;
                sh.has_fill = true;
                sh.fill = f;
                CHECK(pc_vec_render_shape(&sh, PC_PIXEL_FORMAT_RGBA8, canvas, &surf, NULL) == PC_OK);
                for (y = 9; y < 47; y++) {
                    for (x = 17; x < 63; x++) {
                        const float *p = want.px[y * 64 + x];
                        for (ch = 0; ch < 4; ch++) {
                            float got = pc_surface_sample_channel(&surf, x, y, (size_t)ch);
                            float dlt = fabsf(got - p[ch]);
                            worst = dlt > worst ? dlt : worst;
                        }
                    }
                }
                if (!(worst <= 1.5f / 255.0f)) {
                    PC_T_FAIL("style %zu case %d align %d: worst delta %g", s, c, align, (double)worst);
                }
                pc_t_checks++;
                pc_surface_fini(&surf);
                pc_compose_buffer_fini(&want);
            }
        }
    }
    pc_path_fini(&path);
}

static void test_clipped_brightness_and_desaturation_whiten_a_lighter_color_logo(void)
{
    /* A yellow logo in Lighter Color over the same yellow, with Brightness/Contrast +150 and
     * Vibrance > Saturation -100 clipped to it: Photoshop shows it white (254). */
    const float yellow[4] = {251.0f / 255.0f, 201.0f / 255.0f, 0.0f, 1.0f};
    pc_document d = ch_doc_white(2, 1);
    pc_layer logo = ch_solid("logo", pc_rect_make(0, 0, 1, 1), yellow[0], yellow[1], yellow[2], 1);
    pc_adjustment a;
    pc_layer l;
    float p[4];
    fill_bg(&d, pc_rect_make(0, 0, 2, 1), yellow[0], yellow[1], yellow[2]);
    logo.blend = PC_BLEND_LIGHTER_COLOR;
    ch_push(&d, logo);
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_BRIGHTNESS_CONTRAST;
    a.u.brightness_contrast.brightness = 150.0f;
    l = adj_layer(a);
    l.clipped = true;
    ch_push(&d, l);
    memset(&a, 0, sizeof a);
    a.kind = PC_ADJ_VIBRANCE;
    a.u.vibrance.saturation = -100.0f;
    l = adj_layer(a);
    l.clipped = true;
    ch_push(&d, l);
    ch_px(&d, 0, 0, p);
    CHECK(p[0] * 255.0f >= 252.0f && p[1] * 255.0f >= 252.0f && p[2] * 255.0f >= 252.0f);
    ch_px(&d, 1, 0, p);
    CHECK(ch_close4(p, yellow));
    pc_document_fini(&d);
}

static pc_fill pattern_fill(const pc_pattern *p, bool link, float scale)
{
    pc_fill f;
    memset(&f, 0, sizeof f);
    f.kind = PC_FILL_PATTERN;
    if (pc_str_set(&f.u.pattern.id, p->id.ptr, p->id.len, NULL) != PC_OK ||
        pc_str_set(&f.u.pattern.name, p->name.ptr, p->name.len, NULL) != PC_OK) {
        abort();
    }
    f.u.pattern.scale = scale;
    f.u.pattern.link = link;
    return f;
}

static void test_pattern_fill_layers_tile_and_missing_patterns_are_transparent(void)
{
    /* A 2 x 2 checker tiled over the canvas from its origin; a fill naming a pattern the
     * document lacks shows nothing. */
    pc_document d = ch_doc(6, 2, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_surface s;
    pc_pattern pat;
    const float red[4] = {1, 0, 0, 1};
    const float blue[4] = {0, 0, 1, 1};
    pc_compose_buffer b;
    pc_fill missing;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 2, 2), red, 4, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(1, 0, 2, 1), blue, 4, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 1, 1, 2), blue, 4, NULL) == PC_OK);
    CHECK(pc_pattern_new("chk", 3, &s, 2, 2, &pat, NULL) == PC_OK);
    d.patterns = malloc(sizeof pat);
    if (d.patterns == NULL) {
        abort();
    }
    d.patterns[0] = pat;
    d.npatterns = 1;
    ch_push(&d, fill_layer(pattern_fill(&pat, true, 1.0f)));
    CHECK(ch_px_close(&d, 0, 0, 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 1, 0, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 4, 1, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 5, 1, 1, 0, 0, 1));
    /* What a PSD fill layer stores: the fill over the canvas. */
    CHECK(pc_compose_render_fill_content(&d.layers[0], &d.layers[0].content.u.fill, pc_document_bounds(&d),
                                         d.patterns, d.npatterns, &b, NULL) == PC_OK);
    CHECK(b.len == 12 && ch_close4(b.px[1], blue));
    pc_compose_buffer_fini(&b);
    missing = pattern_fill(&pat, false, 1.0f);
    pc_str_fini(&missing.u.pattern.id);
    pc_str_fini(&missing.u.pattern.name);
    CHECK(pc_str_set(&missing.u.pattern.id, "gone", 4, NULL) == PC_OK);
    CHECK(pc_str_set(&missing.u.pattern.name, "gone", 4, NULL) == PC_OK);
    pc_fill_fini(&d.layers[0].content.u.fill);
    d.layers[0].content.u.fill = missing;
    CHECK(ch_px_close(&d, 1, 0, 0, 0, 0, 0));
    pc_document_fini(&d);
}

/* Many tiles render at once: they share the prepared patterns (converted once under the cache's
 * locks) and the shape-split cache; any tiling gives the same pixels (and TSan stays quiet). */
static void test_parallel_tiles_share_patterns_and_split_shapes(void)
{
    pc_document d = ch_doc(300, 200, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16);
    pc_surface s;
    pc_pattern pats[2];
    const float red[4] = {1, 0, 0, 1};
    const float blue[4] = {0, 0, 1, 0.5f};
    const double tri[3][2] = {{10.5, 10.5}, {290.5, 40.25}, {120.5, 190.5}};
    pc_layer_content c;
    pc_layer clip = ch_solid("clip", pc_rect_make(0, 0, 300, 200), 0, 1, 0, 0.5f);
    pc_compose_buffer whole;
    pc_compose_buffer tiled;
    size_t i;
    for (i = 0; i < 2; i++) {
        pc_surface_init(&s, i == 0 ? PC_PIXEL_FORMAT_RGBA8 : PC_PIXEL_FORMAT_RGBA32F);
        CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 3, 2), red, 4, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&s, pc_rect_make(1, 0, 2, 2), blue, 4, NULL) == PC_OK);
        CHECK(pc_pattern_new(i == 0 ? "a" : "b", 1, &s, 3 + (uint32_t)i, 2, &pats[i], NULL) == PC_OK);
    }
    d.patterns = malloc(sizeof pats);
    if (d.patterns == NULL) {
        abort();
    }
    memcpy(d.patterns, pats, sizeof pats);
    d.npatterns = 2;
    ch_push(&d, fill_layer(pattern_fill(&pats[0], true, 1.5f)));
    ch_push(&d, fill_layer(pattern_fill(&pats[1], false, 0.7f)))->opacity = 0.5f;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SHAPE;
    c.u.shape.path = ch_polygon(tri, 3);
    c.u.shape.has_fill = true;
    c.u.shape.fill = pc_fill_solid(pc_color_rgb(0.2f, 0.4f, 0.6f));
    c.u.shape.has_stroke = true;
    pc_shape_stroke_default(&c.u.shape.stroke);
    {
        pc_layer l;
        if (pc_layer_init(&l, "shape", &c, NULL) != PC_OK) {
            abort();
        }
        ch_push(&d, l);
    }
    clip.clipped = true;
    ch_push(&d, clip);
    whole = ch_render_tiled(&d, pc_document_bounds(&d), 10000);
    tiled = ch_render_tiled(&d, pc_document_bounds(&d), 16);
    CHECK(pc_compose_buffer_eq(&whole, &tiled));
    pc_compose_buffer_fini(&tiled);
    pc_compose_buffer_fini(&whole);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_pass_through_group_below_full_fill_is_isolated);
    RUN(test_invert_adjustment_layer);
    RUN(test_adjustment_opacity_and_mask);
    RUN(test_clipped_adjustment_only_affects_base);
    RUN(test_solid_and_gradient_fill_layers);
    RUN(test_channel_restrictions_on_adjustment_layers);
    RUN(test_blend_if_on_adjustment_layers);
    RUN(test_adjustment_results_are_rounded_to_the_document_depth);
    RUN(test_exposure_scales_32_bit_samples_as_stored);
    RUN(test_small_gradient_fill_matches_photoshop_at_all_depths);
    RUN(test_shape_layer_gradients_match_fill_layers);
    RUN(test_clipped_brightness_and_desaturation_whiten_a_lighter_color_logo);
    RUN(test_pattern_fill_layers_tile_and_missing_patterns_are_transparent);
    RUN(test_parallel_tiles_share_patterns_and_split_shapes);
    return pc_t_finish();
}
