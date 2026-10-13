/* Never crash (AGENTS.md) for the C2 parts of the compositor: hostile adjustment parameters,
 * gradient and pattern fills, stroked shapes and Advanced Blending must render or fail with a
 * status, never crash or hang. Runs under ASan / UBSan in `make sanitize`. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "compose/adjust.h"
#include "compose/gradient_fill.h"
#include "compose/pattern.h"
#include "compose/shape_split.h"

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

/* Renders and requires a finite-or-NaN result of the right size, or a clean error. */
static void renders(const pc_document *d)
{
    pc_compose_buffer b;
    pc_error e;
    pc_status st = pc_compose_flatten(d, &b, &e);
    CHECK(st == PC_OK || st == PC_ERR_LIMIT || st == PC_ERR_NOMEM);
    if (st == PC_OK) {
        CHECK(b.len == (size_t)pc_rect_width(b.rect) * pc_rect_height(b.rect));
        pc_compose_buffer_fini(&b);
    }
}

static void test_hostile_adjustment_parameters(void)
{
    static const float bad[6] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -0.0f};
    size_t i;
    int k;
    for (i = 0; i < 6; i++) {
        float v = bad[i];
        for (k = 0; k < PC_ADJ_KIND_COUNT; k++) {
            pc_document d = ch_doc_white(5, 3);
            pc_adjustment a;
            size_t j;
            memset(&a, 0, sizeof a);
            a.kind = (pc_adjustment_kind)k;
            switch (a.kind) {
            case PC_ADJ_BRIGHTNESS_CONTRAST:
                a.u.brightness_contrast.brightness = v;
                a.u.brightness_contrast.contrast = v;
                a.u.brightness_contrast.legacy = i % 2 == 0;
                break;
            case PC_ADJ_LEVELS:
                a.u.levels.master = (pc_levels_channel){v, v, v, v, v};
                a.u.levels.space = (pc_tone_space)(i % 3);
                break;
            case PC_ADJ_EXPOSURE:
                a.u.exposure.exposure = v;
                a.u.exposure.offset = v;
                a.u.exposure.gamma = v;
                break;
            case PC_ADJ_VIBRANCE:
                a.u.vibrance.vibrance = v;
                a.u.vibrance.saturation = v;
                break;
            case PC_ADJ_HUE_SATURATION:
                pc_hue_range_defaults(a.u.hue_saturation.ranges);
                a.u.hue_saturation.hue = v;
                a.u.hue_saturation.ranges[2].saturation = v;
                a.u.hue_saturation.ranges[2].bounds[0] = v;
                break;
            case PC_ADJ_PHOTO_FILTER:
                a.u.photo_filter.color[0] = v;
                a.u.photo_filter.density = v;
                a.u.photo_filter.preserve_luminosity = true;
                break;
            case PC_ADJ_POSTERIZE:
                a.u.posterize.levels = i % 2 == 0 ? 0 : UINT32_MAX;
                break;
            case PC_ADJ_THRESHOLD:
                a.u.threshold.level = v;
                break;
            case PC_ADJ_SELECTIVE_COLOR:
                for (j = 0; j < 9; j++) {
                    a.u.selective_color.adjustments[j][j % 4] = v;
                }
                break;
            case PC_ADJ_COLOR_LOOKUP:
                a.u.color_lookup.size = i % 2 == 0 ? UINT32_MAX : 1;
                break;
            default:
                break;
            }
            ch_push(&d, adj_layer(a));
            renders(&d);
            pc_document_fini(&d);
        }
    }
}

static void test_huge_curves_and_lookups_stay_fast(void)
{
    /* 200,000 curve points (sorted descending, duplicates): the LUT is built in linear time
     * after the sort. */
    size_t n = 200000;
    pc_curve_point *pts = malloc(n * sizeof *pts);
    float *lut = malloc(PC_COMPOSE_LUT_SIZE * sizeof *lut);
    size_t i;
    CHECK(pts != NULL && lut != NULL);
    if (pts == NULL || lut == NULL) {
        free(pts);
        free(lut);
        return;
    }
    for (i = 0; i < n; i++) {
        pts[i].input = (float)(n - i) / (float)n;
        pts[i].output = (float)(i % 7) / 7.0f;
    }
    CHECK(pc_compose_curve_lut(pts, n, lut, NULL) == PC_OK);
    for (i = 0; i < n; i++) {
        pts[i].input = (float)(i % 3) / 2.0f; /* only three distinct inputs */
    }
    CHECK(pc_compose_curve_lut(pts, n, lut, NULL) == PC_OK);
    free(pts);
    free(lut);
}

static void test_hostile_gradient_fills(void)
{
    static const float bad[4] = {NAN, INFINITY, -1e30f, 0.0f};
    size_t i;
    for (i = 0; i < 4; i++) {
        pc_document d = ch_doc_white(6, 4);
        pc_color_stop stops[3] = {{bad[i], PC_COLOR_BLACK}, {0.5f, PC_COLOR_WHITE}, {NAN, PC_COLOR_BLACK}};
        pc_opacity_stop ops[2] = {{NAN, bad[i]}, {0.3f, 2.0f}};
        float mids[4] = {NAN, -5.0f, 5.0f, bad[i]};
        pc_fill f;
        CHECK(pc_fill_gradient(stops, 3, bad[i], bad[i], (pc_gradient_style)(i % 5), true, &f, NULL) ==
              PC_OK);
        f.u.gradient.offset[0] = bad[i];
        f.u.gradient.dither = true;
        f.u.gradient.opacity_stops = malloc(sizeof ops);
        f.u.gradient.midpoints = malloc(sizeof mids);
        if (f.u.gradient.opacity_stops != NULL && f.u.gradient.midpoints != NULL) {
            memcpy(f.u.gradient.opacity_stops, ops, sizeof ops);
            f.u.gradient.nopacity_stops = 2;
            memcpy(f.u.gradient.midpoints, mids, sizeof mids);
            f.u.gradient.nmidpoints = 4;
        }
        ch_push(&d, fill_layer(f));
        renders(&d);
        pc_document_fini(&d);
    }
}

static void test_hostile_pattern_fills(void)
{
    pc_document d = ch_doc_white(6, 4);
    pc_surface s;
    pc_pattern big;
    pc_fill f;
    pc_compose_buffer b;
    pc_error e;
    pc_status st;
    const float px[4] = {1, 0, 0, 1};
    /* A pattern claiming 70000 x 70000 pixels (as a hostile file could): refused with
     * PC_ERR_LIMIT, not allocated. */
    memset(&big, 0, sizeof big);
    pc_surface_init(&big.surface, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&big.surface, pc_rect_make(0, 0, 2, 2), px, 4, NULL) == PC_OK);
    CHECK(pc_str_set(&big.name, "big", 3, NULL) == PC_OK && pc_str_set(&big.id, "big-id", 6, NULL) == PC_OK);
    big.width = 70000;
    big.height = 70000;
    d.patterns = malloc(sizeof big);
    if (d.patterns == NULL) {
        abort();
    }
    d.patterns[0] = big;
    d.npatterns = 1;
    memset(&f, 0, sizeof f);
    f.kind = PC_FILL_PATTERN;
    CHECK(pc_str_set(&f.u.pattern.name, "big", 3, NULL) == PC_OK);
    f.u.pattern.scale = NAN;
    f.u.pattern.angle = INFINITY;
    f.u.pattern.link = true;
    f.u.pattern.phase[0] = 1e30f;
    ch_push(&d, fill_layer(f));
    /* Too large to paint (pc_pattern_is_paintable): skipped without allocating, transparent. */
    st = pc_compose_flatten(&d, &b, &e);
    CHECK(st == PC_OK);
    if (st == PC_OK) {
        pc_compose_buffer_fini(&b);
    }
    /* A sane pattern with hostile placement renders. */
    pc_pattern_fini(&d.patterns[0]);
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 2, 2), px, 4, NULL) == PC_OK);
    CHECK(pc_pattern_new("big", 3, &s, 2, 2, &d.patterns[0], NULL) == PC_OK);
    renders(&d);
    pc_document_fini(&d);
}

static void test_hostile_stroked_shapes(void)
{
    static const double pts[3][2] = {{1e300, -1e300}, {NAN, 3.0}, {2.0, INFINITY}};
    pc_shape_layer sh;
    bool found = false;
    pc_surface fs;
    pc_surface ss;
    pc_status st;
    memset(&sh, 0, sizeof sh);
    sh.path = ch_polygon(pts, 3);
    sh.has_fill = true;
    sh.fill = pc_fill_solid(pc_color_rgb(1, 0, 0));
    sh.has_stroke = true;
    pc_shape_stroke_default(&sh.stroke);
    sh.stroke.width = NAN;
    st = pc_compose_shape_split(&sh, pc_rect_make(0, 0, 8, 8), PC_SAMPLE_TYPE_U16, &found, &fs, &ss, NULL);
    CHECK(st == PC_OK || st == PC_ERR_LIMIT || st == PC_ERR_INVALID);
    if (st == PC_OK && found) {
        pc_surface_fini(&fs);
        pc_surface_fini(&ss);
    }
    pc_shape_layer_fini(&sh);
}

static void test_knockouts_in_deep_groups(void)
{
    /* A knockout at the bottom of 100 nested pass-through groups over a hidden Background. */
    pc_document d = ch_doc_white(4, 4);
    pc_layer l = ch_solid("k", pc_rect_make(0, 0, 2, 2), 0, 0, 1, 1);
    int i;
    l.advanced.knockout = PC_KNOCKOUT_DEEP;
    l.advanced.transparency_shapes = false;
    l.fill_opacity = 0.0f;
    l.opacity = NAN;
    for (i = 0; i < 100; i++) {
        pc_layer g = ch_group("g", &l, 1);
        g.advanced.knockout = i % 3 == 0 ? PC_KNOCKOUT_SHALLOW : PC_KNOCKOUT_NONE;
        g.advanced.blend_clipped = false;
        l = g;
    }
    ch_push(&d, l);
    d.layers[0].visible = false;
    renders(&d);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_hostile_adjustment_parameters);
    RUN(test_huge_curves_and_lookups_stay_fast);
    RUN(test_hostile_gradient_fills);
    RUN(test_hostile_pattern_fills);
    RUN(test_hostile_stroked_shapes);
    RUN(test_knockouts_in_deep_groups);
    return pc_t_finish();
}
