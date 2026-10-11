/* crates/compose/src/stroke_tests.rs: stroke effects on shape layers, the vector stroke's place,
 * gradient frames, and the fill / stroke split of stroked shapes (shape_split.rs). Tests that
 * need layer effects skip (with a note) while effects are not ported (C3); the split itself is
 * also checked directly and through clipped layers, which take the same path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "compose/effects_util.h"
#include "compose/fill_layout.h"
#include "compose/shape_split.h"
#include "vector/vector.h"

static bool fx_ready(void)
{
    pc_document d = ch_doc_white(2, 2);
    pc_layer *l = ch_push(&d, ch_solid("l", pc_rect_make(0, 0, 1, 1), 1, 0, 0, 1));
    pc_compose_buffer b;
    pc_status st;
    l->effects.items = calloc(1, sizeof(pc_effect));
    if (l->effects.items == NULL) {
        abort();
    }
    pc_effect_default_drop_shadow(&l->effects.items[0]);
    l->effects.nitems = 1;
    st = pc_compose_flatten(&d, &b, NULL);
    if (st == PC_OK) {
        pc_compose_buffer_fini(&b);
    }
    pc_document_fini(&d);
    return st != PC_ERR_UNSUPPORTED;
}

#define NEEDS_FX()                                                                                           \
    do {                                                                                                     \
        if (!fx_ready()) {                                                                                   \
            (void)fprintf(stderr, "  %s: skipped (layer effects need C3)\n", __func__);                      \
            return;                                                                                          \
        }                                                                                                    \
    } while (0)

static void add_effect(pc_layer *l, pc_effect e)
{
    pc_effect *items = realloc(l->effects.items, (l->effects.nitems + 1) * sizeof *items);
    if (items == NULL) {
        abort();
    }
    items[l->effects.nitems++] = e;
    l->effects.items = items;
}

static pc_effect stroke_fx(float size, pc_stroke_position pos, pc_color c)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_STROKE;
    e.u.stroke.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    e.u.stroke.size = size;
    e.u.stroke.position = pos;
    e.u.stroke.paint.kind = PC_FX_PAINT_COLOR;
    e.u.stroke.paint.u.color = c;
    return e;
}

static pc_effect overlay(pc_color c)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_COLOR_OVERLAY;
    e.u.color_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    e.u.color_overlay.color = c;
    return e;
}

static pc_layer shape_layer(pc_shape_layer sh)
{
    pc_layer l;
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SHAPE;
    c.u.shape = sh;
    if (pc_layer_init(&l, "shape", &c, NULL) != PC_OK) {
        abort();
    }
    return l;
}

static pc_path square(double x0, double x1)
{
    const double pts[4][2] = {{x0, x0}, {x1, x0}, {x1, x1}, {x0, x1}};
    return ch_polygon(pts, 4);
}

static pc_path rect_path(double w, double h)
{
    const double pts[4][2] = {{0, 0}, {w, 0}, {w, h}, {0, h}};
    return ch_polygon(pts, 4);
}

/* A shape layer over the square x0..x1 with its Photoshop-style cached pixels (40 x 40). */
static pc_layer shape(double x0, double x1, pc_fill fill, const pc_shape_stroke *stroke)
{
    pc_shape_layer sh;
    memset(&sh, 0, sizeof sh);
    sh.path = square(x0, x1);
    sh.has_fill = true;
    sh.fill = fill;
    if (stroke != NULL) {
        sh.has_stroke = true;
        sh.stroke = *stroke;
    }
    CHECK(pc_vec_render_shape(&sh, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 40, 40), &sh.cache, NULL) ==
          PC_OK);
    sh.has_cache = true;
    return shape_layer(sh);
}

/* A w x h rectangle with a black-to-white linear gradient and a 4 px stroke. */
static pc_shape_layer gradient_shape(uint32_t w, uint32_t h)
{
    pc_color_stop stops[2] = {{0.0f, PC_COLOR_BLACK}, {1.0f, PC_COLOR_WHITE}};
    pc_shape_layer sh;
    memset(&sh, 0, sizeof sh);
    sh.path = rect_path((double)w, (double)h);
    sh.has_fill = true;
    CHECK(pc_fill_gradient(stops, 2, 0.0f, 1.0f, PC_GRADIENT_LINEAR, false, &sh.fill, NULL) == PC_OK);
    sh.has_stroke = true;
    pc_shape_stroke_default(&sh.stroke);
    sh.stroke.width = 4.0f;
    return sh;
}

static pc_shape_stroke inside_stroke(float width, pc_color c)
{
    pc_shape_stroke s;
    pc_shape_stroke_default(&s);
    s.width = width;
    s.align = PC_STROKE_ALIGN_INSIDE;
    s.paint = pc_fill_solid(c);
    return s;
}

static pc_document white_doc(uint32_t w, uint32_t h, pc_sample_type depth)
{
    pc_document d;
    if (pc_document_with_background(&d, "t", pc_size_make(w, h), PC_COLOR_MODE_RGB, depth, PC_COLOR_WHITE,
                                    NULL) != PC_OK) {
        abort();
    }
    return d;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void test_split_stroked_gradient_with_effect_keeps_document_precision(void)
{
    static const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    size_t i;
    NEEDS_FX();
    for (i = 0; i < 3; i++) {
        pc_document d = white_doc(4000, 40, depths[i]);
        pc_shape_layer sh = gradient_shape(4000, 40);
        pc_layer l;
        pc_compose_buffer row;
        uint32_t *levels = malloc(4000 * sizeof *levels);
        size_t n = 0;
        size_t k;
        if (levels == NULL) {
            abort();
        }
        CHECK(pc_vec_render_shape(&sh, pc_document_pixel_format(&d), pc_document_bounds(&d), &sh.cache,
                                  NULL) == PC_OK);
        sh.has_cache = true;
        l = shape_layer(sh);
        add_effect(&l, stroke_fx(1.0f, PC_STROKE_OUTSIDE, pc_color_rgb(0, 0, 1)));
        ch_push(&d, l);
        CHECK(pc_compose_render(&d, pc_rect_from_xywh(0, 20, 4000, 1), &row, NULL) == PC_OK);
        for (k = 0; k < row.len; k++) {
            levels[k] = (uint32_t)roundf(row.px[k][0] * 65535.0f);
        }
        qsort(levels, row.len, sizeof *levels, cmp_u32);
        for (k = 0; k < row.len; k++) {
            if (k == 0 || levels[k] != levels[k - 1]) {
                n++;
            }
        }
        CHECK(depths[i] == PC_SAMPLE_TYPE_U8 ? n <= 256 : n > 3000);
        free(levels);
        pc_compose_buffer_fini(&row);
        pc_document_fini(&d);
    }
}

static void test_split_cache_keeps_same_shape_separate_at_each_document_depth(void)
{
    static const pc_sample_type depths[4] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32,
                                             PC_SAMPLE_TYPE_U8};
    pc_rect canvas = pc_rect_make(0, 0, 64, 8);
    pc_shape_layer sh = gradient_shape(64, 8);
    size_t i;
    /* Directly: the split's fill at each depth is the shape rendered at that depth, from the cache
     * keyed by depth too (the shape and its cached tiles stay identical). */
    CHECK(pc_vec_render_shape(&sh, PC_PIXEL_FORMAT_RGBA32F, canvas, &sh.cache, NULL) == PC_OK);
    sh.has_cache = true;
    for (i = 0; i < 4; i++) {
        pc_pixel_format f = PC_PIXEL_FORMAT_RGBA8;
        pc_surface expected;
        pc_surface fill;
        pc_surface stroke;
        bool found = false;
        f.sample = depths[i];
        CHECK(pc_vec_render_shape(&sh, f, canvas, &expected, NULL) == PC_OK);
        CHECK(pc_compose_shape_split(&sh, canvas, depths[i], &found, &fill, &stroke, NULL) == PC_OK && found);
        if (found) {
            CHECK(pc_surface_format(&fill).sample == depths[i] &&
                  pc_surface_format(&stroke).sample == depths[i]);
            /* Inside the 4 px stroke the fill shows through: the fill-only part equals the shape. */
            CHECK(fabsf(pc_surface_sample_channel(&fill, 17, 4, 0) -
                        pc_surface_sample_channel(&expected, 17, 4, 0)) < 1e-6f);
            pc_surface_fini(&fill);
            pc_surface_fini(&stroke);
        }
        pc_surface_fini(&expected);
    }
    /* Through documents (the reference test; needs effects). */
    if (fx_ready()) {
        pc_layer layer = shape_layer(sh);
        add_effect(&layer, stroke_fx(1.0f, PC_STROKE_OUTSIDE, pc_color_rgb(0, 0, 1)));
        for (i = 0; i < 4; i++) {
            pc_document d = white_doc(64, 8, depths[i]);
            pc_layer copy;
            pc_pixel_format f = PC_PIXEL_FORMAT_RGBA8;
            pc_surface expected;
            float p[4];
            f.sample = depths[i];
            CHECK(pc_layer_clone(&layer, &copy, NULL) == PC_OK);
            ch_push(&d, copy);
            CHECK(pc_vec_render_shape(&layer.content.u.shape, f, canvas, &expected, NULL) == PC_OK);
            ch_px(&d, 17, 4, p);
            CHECK(fabsf(p[0] - pc_surface_sample_channel(&expected, 17, 4, 0)) < 1e-6f);
            pc_surface_fini(&expected);
            pc_document_fini(&d);
        }
        pc_layer_fini(&layer);
    } else {
        (void)fprintf(stderr, "  %s: document half skipped (layer effects need C3)\n", __func__);
        pc_shape_layer_fini(&sh);
    }
    /* No stroke: no split. */
    {
        pc_shape_layer plain;
        bool found = true;
        pc_surface a;
        pc_surface b;
        memset(&plain, 0, sizeof plain);
        CHECK(pc_compose_shape_split(&plain, canvas, PC_SAMPLE_TYPE_U8, &found, &a, &b, NULL) == PC_OK &&
              !found);
    }
}

static void test_outline_share_treats_stroke_and_shape_as_disjoint_areas(void)
{
    float s;
    /* Outside the shape the stroke covers the pixel; inside it nothing. */
    CHECK(pc_compose_fx_outline_share(0.0f, 0.0f) == 1.0f);
    CHECK(pc_compose_fx_outline_share(1.0f, 0.3f) == 0.0f);
    /* Beneath an opaque edge pixel the result ends opaque: share + l (1 - share) = 1 - cov + l. */
    s = pc_compose_fx_outline_share(0.6f, 0.6f);
    CHECK(fabsf(s + 0.6f * (1.0f - s) - fminf(1.0f - 0.6f + 0.6f, 1.0f)) < 1e-6f);
    /* A faded fill keeps the edge part of the stroke only. */
    s = pc_compose_fx_outline_share(0.6f, 0.0f);
    CHECK(fabsf(s - 0.4f) < 1e-6f);
    CHECK(pc_compose_fx_outline_share(0.5f, 1.0f) == 1.0f);
    s = pc_compose_fx_outline_share(NAN, 0.5f);
    CHECK(isfinite(s) || isnan(s));
}

static void test_filled_shape_stroke_follows_the_outline_where_the_fill_fades_out(void)
{
    pc_document d;
    pc_color transparent = pc_color_rgb(1, 0, 0);
    pc_color_stop stops[2];
    pc_fill fade;
    pc_layer l;
    float p[4];
    NEEDS_FX();
    d = white_doc(40, 40, PC_SAMPLE_TYPE_U8);
    transparent.alpha = 0.0f;
    stops[0] = (pc_color_stop){0.0f, pc_color_rgb(1, 0, 0)};
    stops[1] = (pc_color_stop){1.0f, transparent};
    CHECK(pc_fill_gradient(stops, 2, 0.0f, 1.0f, PC_GRADIENT_LINEAR, false, &fade, NULL) == PC_OK);
    l = shape(10.0, 30.0, fade, NULL);
    add_effect(&l, stroke_fx(3.0f, PC_STROKE_OUTSIDE, pc_color_rgb(0, 0, 1)));
    add_effect(&l, stroke_fx(2.0f, PC_STROKE_INSIDE, pc_color_rgb(0, 1, 0)));
    ch_push(&d, l);
    CHECK(ch_px_close(&d, 8, 20, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 31, 20, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 28, 20, 0, 1, 0, 1));
    ch_px(&d, 25, 20, p);
    CHECK(p[0] > 0.9f && p[1] > 0.5f);
    pc_document_fini(&d);
}

static void test_outside_stroke_fills_the_rest_of_a_shapes_edge_pixel(void)
{
    pc_document d;
    pc_layer l;
    float p[4];
    NEEDS_FX();
    d = ch_doc(40, 40, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    l = shape(10.0, 30.5, pc_fill_solid(pc_color_rgb(1, 0, 0)), NULL);
    add_effect(&l, stroke_fx(3.0f, PC_STROKE_OUTSIDE, pc_color_rgb(0, 0, 1)));
    ch_push(&d, l);
    ch_px(&d, 30, 20, p);
    CHECK(fabsf(p[3] - 1.0f) <= CH_E);
    CHECK(fabsf(p[0] - 0.5f) <= 0.02f && fabsf(p[2] - 0.5f) <= 0.02f);
    pc_document_fini(&d);
}

static void test_vector_stroke_stays_above_interior_effects_and_below_the_stroke_effect(void)
{
    pc_document d;
    pc_shape_stroke vs;
    pc_layer l;
    NEEDS_FX();
    d = white_doc(40, 40, PC_SAMPLE_TYPE_U8);
    vs = inside_stroke(4.0f, pc_color_rgb(0, 1, 0));
    l = shape(10.0, 30.0, pc_fill_solid(pc_color_rgb(1, 0, 0)), &vs);
    add_effect(&l, overlay(pc_color_rgb(0, 0, 1)));
    add_effect(&l, stroke_fx(1.0f, PC_STROKE_INSIDE, pc_color_rgb(1, 1, 0)));
    ch_push(&d, l);
    CHECK(ch_px_close(&d, 10, 20, 1, 1, 0, 1)); /* stroke effect on top */
    CHECK(ch_px_close(&d, 12, 20, 0, 1, 0, 1)); /* vector stroke over the overlay */
    CHECK(ch_px_close(&d, 20, 20, 0, 0, 1, 1)); /* overlay on the fill */
    pc_document_fini(&d);
}

static void test_a_stroked_shapes_mask_applies_to_fill_and_stroke_together(void)
{
    pc_document d = ch_doc(40, 40, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_shape_stroke vs = inside_stroke(4.0f, pc_color_rgb(0, 1, 0));
    pc_layer l = shape(10.0, 30.0, pc_fill_solid(pc_color_rgb(1, 0, 0)), &vs);
    pc_layer clip = ch_solid("clip", pc_rect_make(0, 0, 40, 40), 0, 0, 1, 1);
    pc_layer copy;
    const float half = 0.5f;
    pc_layer_mask_reveal_all(&l.mask);
    pc_surface_fini(&l.mask.surface);
    pc_surface_init_default(&l.mask.surface,
                            (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false}, &half, 1);
    l.has_mask = true;
    clip.clipped = true;
    CHECK(pc_layer_clone(&l, &copy, NULL) == PC_OK);
    ch_push(&d, copy);
    CHECK(pc_layer_clone(&clip, &copy, NULL) == PC_OK);
    ch_push(&d, copy);
    /* On the vector stroke: the stroke's colour at the mask's 50 %, not 75 %. */
    CHECK(ch_px_close(&d, 12, 20, 0, 1, 0, 0.5f));
    if (fx_ready()) {
        pc_document e = ch_doc(40, 40, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        add_effect(&l, overlay(pc_color_rgb(1, 1, 1)));
        CHECK(pc_layer_clone(&l, &copy, NULL) == PC_OK);
        ch_push(&e, copy);
        CHECK(ch_px_close(&e, 12, 20, 0, 1, 0, 0.5f)); /* without clipped layers */
        CHECK(pc_layer_clone(&clip, &copy, NULL) == PC_OK);
        ch_push(&e, copy);
        CHECK(ch_px_close(&e, 12, 20, 0, 1, 0, 0.5f));
        pc_document_fini(&e);
    } else {
        (void)fprintf(stderr, "  %s: effects half skipped (layer effects need C3)\n", __func__);
    }
    pc_layer_fini(&l);
    pc_layer_fini(&clip);
    pc_document_fini(&d);
}

static void test_gradient_stroke_spans_the_strokes_extent(void)
{
    pc_document d;
    pc_layer l;
    pc_effect e;
    pc_color_stop stops[2] = {{0.0f, PC_COLOR_BLACK}, {1.0f, PC_COLOR_WHITE}};
    pc_rect stroke_frame = pc_rect_make(7, 7, 33, 33);
    pc_rect layer_frame = pc_rect_make(10, 10, 30, 30);
    static const int ys[4] = {7, 9, 31, 32};
    const float zero[2] = {0, 0};
    size_t i;
    NEEDS_FX();
    d = white_doc(40, 40, PC_SAMPLE_TYPE_U8);
    l = ch_solid("sq", pc_rect_make(10, 10, 30, 30), 1, 0, 0, 1);
    e = stroke_fx(4.0f, PC_STROKE_OUTSIDE, PC_COLOR_BLACK);
    e.u.stroke.paint.kind = PC_FX_PAINT_GRADIENT;
    memset(&e.u.stroke.paint.u.gradient, 0, sizeof e.u.stroke.paint.u.gradient);
    e.u.stroke.paint.u.gradient.stops = malloc(sizeof stops);
    if (e.u.stroke.paint.u.gradient.stops == NULL) {
        abort();
    }
    memcpy(e.u.stroke.paint.u.gradient.stops, stops, sizeof stops);
    e.u.stroke.paint.u.gradient.nstops = 2;
    e.u.stroke.paint.u.gradient.angle = 90.0f;
    e.u.stroke.paint.u.gradient.scale = 1.0f;
    e.u.stroke.paint.u.gradient.align = true;
    add_effect(&l, e);
    ch_push(&d, l);
    /* The frame is the square grown by 3 px (7..33), laid out like every linear gradient. */
#define T_IN(frame, x, y)                                                                                    \
    (pc_compose_gradient_t(PC_GRADIENT_LINEAR, g.angle, g.scale, false, g.offset, frame, (float)(x) + 0.5f,  \
                           (float)(y) + 0.5f))
    for (i = 0; i < 4; i++) {
        pc_compose_gradient_geom g =
            pc_compose_gradient_layout(PC_GRADIENT_LINEAR, 90.0f, 1.0f, zero, stroke_frame);
        float p[4];
        ch_px(&d, 20, ys[i], p);
        CHECK(fabsf(p[0] - T_IN(stroke_frame, 20, ys[i])) <= 0.01f);
    }
    {
        pc_compose_gradient_geom g =
            pc_compose_gradient_layout(PC_GRADIENT_LINEAR, 90.0f, 1.0f, zero, stroke_frame);
        float a = T_IN(stroke_frame, 20, 9);
        g = pc_compose_gradient_layout(PC_GRADIENT_LINEAR, 90.0f, 1.0f, zero, layer_frame);
        CHECK(fabsf(a - T_IN(layer_frame, 20, 9)) > 0.05f);
    }
#undef T_IN
    pc_document_fini(&d);
}

static void test_several_strokes_stack_top_instance_first(void)
{
    pc_document d;
    pc_layer l;
    NEEDS_FX();
    d = white_doc(60, 60, PC_SAMPLE_TYPE_U8);
    l = ch_solid("sq", pc_rect_make(20, 20, 40, 40), 1, 0, 0, 1);
    add_effect(&l, stroke_fx(2.0f, PC_STROKE_OUTSIDE, pc_color_rgb(0, 1, 0)));
    add_effect(&l, stroke_fx(5.0f, PC_STROKE_OUTSIDE, pc_color_rgb(0, 0, 1)));
    add_effect(&l, stroke_fx(2.0f, PC_STROKE_INSIDE, pc_color_rgb(1, 1, 0)));
    ch_push(&d, l);
    CHECK(ch_px_close(&d, 19, 30, 0, 1, 0, 1));
    CHECK(ch_px_close(&d, 16, 30, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 20, 30, 1, 1, 0, 1));
    CHECK(ch_px_close(&d, 14, 30, 1, 1, 1, 1));
    pc_document_fini(&d);
}

/* The file's shape was snapped to whole pixels with a 0.25 px stroke: its edge pixels are opaque
 * and mostly fill. Fitting the stroke to the file's pixels keeps their colour, at every depth.
 * via_clip: a transparent clipped layer sends the shape through the split instead of an effect
 * (same split, no effects needed). */
static void hairline(bool via_clip)
{
    static const pc_sample_type samples[4] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32,
                                              PC_SAMPLE_TYPE_F32};
    static const float edges[4] = {0.86f, 0.86f, 0.86f, 2.0f};
    static const float fills[4] = {1.0f, 1.0f, 1.0f, 2.0f};
    static const int pts[4][2] = {{10, 20}, {29, 20}, {20, 10}, {20, 29}};
    const double poly[4][2] = {{10.3, 10.3}, {29.7, 10.3}, {29.7, 29.7}, {10.3, 29.7}};
    size_t i;
    size_t k;
    for (i = 0; i < 4; i++) {
        pc_document d = white_doc(40, 40, samples[i]);
        pc_pixel_format fmt = PC_PIXEL_FORMAT_RGBA8;
        pc_shape_layer sh;
        pc_layer l;
        float e = edges[i];
        float f = fills[i];
        const float epx[4] = {e, e, e, 1};
        const float fpx[4] = {f, f, f, 1};
        fmt.sample = samples[i];
        memset(&sh, 0, sizeof sh);
        sh.path = ch_polygon(poly, 4);
        sh.has_fill = true;
        sh.fill = pc_fill_solid(pc_color_rgb(f, f, f));
        sh.has_stroke = true;
        sh.stroke = inside_stroke(0.25f, pc_color_rgb(0.48f, 0.48f, 0.48f));
        sh.stroke.align = PC_STROKE_ALIGN_CENTER;
        pc_surface_init(&sh.cache, fmt);
        CHECK(pc_surface_fill_rect(&sh.cache, pc_rect_make(10, 10, 30, 30), epx, 4, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&sh.cache, pc_rect_make(11, 11, 29, 29), fpx, 4, NULL) == PC_OK);
        sh.has_cache = true;
        l = shape_layer(sh);
        if (via_clip) {
            ch_push(&d, l);
            ch_push(&d, ch_solid("clip", pc_rect_make(0, 0, 1, 1), 0, 0, 0, 0))->clipped = true;
        } else {
            pc_effect ds;
            pc_effect_default_drop_shadow(&ds);
            add_effect(&l, ds);
            ch_push(&d, l);
        }
        for (k = 0; k < 4; k++) {
            CHECK(ch_px_close(&d, pts[k][0], pts[k][1], e, e, e, 1));
        }
        CHECK(ch_px_close(&d, 20, 20, f, f, f, 1));
        pc_document_fini(&d);
    }
}

static void test_hairline_vector_stroke_keeps_the_files_edge_pixel_colour(void)
{
    hairline(true);
    NEEDS_FX();
    hairline(false);
}

int main(int argc, char **argv)
{
    RUN(test_split_stroked_gradient_with_effect_keeps_document_precision);
    RUN(test_split_cache_keeps_same_shape_separate_at_each_document_depth);
    RUN(test_outline_share_treats_stroke_and_shape_as_disjoint_areas);
    RUN(test_filled_shape_stroke_follows_the_outline_where_the_fill_fades_out);
    RUN(test_outside_stroke_fills_the_rest_of_a_shapes_edge_pixel);
    RUN(test_vector_stroke_stays_above_interior_effects_and_below_the_stroke_effect);
    RUN(test_a_stroked_shapes_mask_applies_to_fill_and_stroke_together);
    RUN(test_gradient_stroke_spans_the_strokes_extent);
    RUN(test_several_strokes_stack_top_instance_first);
    RUN(test_hairline_vector_stroke_keeps_the_files_edge_pixel_colour);
    return pc_t_finish();
}
