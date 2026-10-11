/* crates/compose/src/advanced_tests.rs: Advanced Blending (knockout, blend clipped / interior as
 * group, transparency shapes, masks hide effects) on small synthetic documents with
 * hand-computed pixels, at 8, 16 and 32 bits. Tests that need layer effects skip (with a note)
 * while effects are not ported (C3). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "compose/advanced.h"

static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
static const float WHITE[4] = {1, 1, 1, 1};
static const float RED[4] = {1, 0, 0, 1};
static const float GREEN[4] = {0, 1, 0, 1};
static const float BLUE[4] = {0, 0, 1, 1};
static const float GRAY[4] = {0.5f, 0.5f, 0.5f, 1};
static const float CLEAR[4] = {0, 0, 0, 0};

/* Effects render once C3 has landed; until then the tests that need them skip. */
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

static const char *depth_name(pc_sample_type d)
{
    return d == PC_SAMPLE_TYPE_U8 ? "U8" : (d == PC_SAMPLE_TYPE_U16 ? "U16" : "F32");
}

/* The pixel matches, and tiled (3 px) and whole-canvas renders agree (knockout targets are per
 * tile). */
static void check_at(const pc_document *d, int32_t x, int32_t y, const float want[4], const char *what,
                     int line)
{
    float got[4];
    pc_compose_buffer tiled;
    pc_compose_buffer whole;
    size_t i;
    bool same = true;
    pc_t_checks++;
    ch_px(d, x, y, got);
    if (!ch_close4(got, want)) {
        PC_T_FAIL("line %d: %s (%s) at (%d,%d): got %g %g %g %g, want %g %g %g %g", line, what,
                  depth_name(d->depth), x, y, (double)got[0], (double)got[1], (double)got[2], (double)got[3],
                  (double)want[0], (double)want[1], (double)want[2], (double)want[3]);
    }
    tiled = ch_render_tiled(d, pc_document_bounds(d), 3);
    whole = ch_render_tiled(d, pc_document_bounds(d), 256);
    pc_t_checks++;
    same = tiled.len == whole.len;
    for (i = 0; same && i < tiled.len; i++) {
        same = ch_close4(tiled.px[i], whole.px[i]);
    }
    if (!same) {
        PC_T_FAIL("line %d: %s: tiles differ from the whole render", line, what);
    }
    pc_compose_buffer_fini(&tiled);
    pc_compose_buffer_fini(&whole);
}

#define CHECK_AT(d, x, y, want, what) check_at(d, x, y, want, what, __LINE__)

/* A 12 x 8 document at depth with a white Background (or none). */
static pc_document doc_d(pc_sample_type depth, bool background)
{
    pc_document d;
    pc_status st = background
                       ? pc_document_with_background(&d, "t", pc_size_make(12, 8), PC_COLOR_MODE_RGB, depth,
                                                     PC_COLOR_WHITE, NULL)
                       : pc_document_init(&d, "t", pc_size_make(12, 8), PC_COLOR_MODE_RGB, depth, NULL);
    if (st != PC_OK) {
        abort(); /* test setup only */
    }
    return d;
}

static pc_layer solid_d(const pc_document *d, const char *name, pc_rect r, const float c[4])
{
    pc_layer l = ch_raster(name, pc_document_pixel_format(d));
    ch_fill(&l, r, c, 4);
    return l;
}

static pc_rect full(const pc_document *d)
{
    return pc_document_bounds(d);
}

/* The knocking-out layer: blue over the left half (x < 6), fill 0. */
static pc_layer knocker(const pc_document *d, pc_knockout k)
{
    pc_layer l = solid_d(d, "knock", pc_rect_make(0, 0, 6, 8), BLUE);
    l.advanced.knockout = k;
    l.fill_opacity = 0.0f;
    return l;
}

static pc_layer group2(const char *name, pc_layer a, pc_layer b)
{
    pc_layer kids[2];
    kids[0] = a;
    kids[1] = b;
    return ch_group(name, kids, 2);
}

static pc_layer *child(pc_document *d, size_t top, size_t i)
{
    return &d->layers[top].content.u.group.children[i];
}

/* A mask showing x < x1 at depth. */
static void mask_left(pc_layer *l, pc_sample_type depth, int32_t x1)
{
    const float one = 1.0f;
    pc_layer_mask_reveal_all(&l->mask);
    pc_surface_fini(&l->mask.surface);
    pc_surface_init(&l->mask.surface, (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, depth, false});
    CHECK(pc_surface_fill_rect(&l->mask.surface, pc_rect_make(0, 0, x1, 8), &one, 1, NULL) == PC_OK);
    l->has_mask = true;
}

static void add_effect(pc_layer *l, pc_effect e)
{
    pc_effect *items = realloc(l->effects.items, (l->effects.nitems + 1) * sizeof *items);
    if (items == NULL) {
        abort();
    }
    items[l->effects.nitems++] = e;
    l->effects.items = items;
}

static pc_effect overlay(float r, float g, float b)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_COLOR_OVERLAY;
    e.u.color_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    e.u.color_overlay.color = pc_color_rgb(r, g, b);
    return e;
}

static pc_effect stroke_fx(float size, pc_stroke_position pos, float r, float g, float b)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_STROKE;
    e.u.stroke.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    e.u.stroke.size = size;
    e.u.stroke.position = pos;
    e.u.stroke.paint.kind = PC_FX_PAINT_COLOR;
    e.u.stroke.paint.u.color = pc_color_rgb(r, g, b);
    return e;
}

static void test_shallow_knockout_stops_at_the_bottom_of_an_isolated_group(void)
{
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer green = solid_d(&d, "green", full(&d), GREEN);
        pc_layer red = solid_d(&d, "red", full(&d), RED);
        pc_layer g = group2("g", red, knocker(&d, PC_KNOCKOUT_SHALLOW));
        const float half[4] = {0, 0.5f, 0.5f, 1};
        const float none[4] = {0.5f, 0, 0.5f, 1};
        g.blend = PC_BLEND_NORMAL;
        ch_push(&d, green);
        ch_push(&d, g);
        /* The group's own buffer starts transparent: the layers beneath the group show. */
        CHECK_AT(&d, 2, 2, GREEN, "shallow, fill 0");
        CHECK_AT(&d, 9, 2, RED, "outside the shape");
        child(&d, 2, 1)->fill_opacity = 0.5f;
        CHECK_AT(&d, 2, 2, half, "shallow, fill 50 %");
        child(&d, 2, 1)->advanced.knockout = PC_KNOCKOUT_NONE;
        CHECK_AT(&d, 2, 2, none, "no knockout");
        pc_document_fini(&d);
    }
}

static void test_deep_knockout_reaches_the_background_through_groups(void)
{
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer green = solid_d(&d, "green", full(&d), GREEN);
        pc_layer red = solid_d(&d, "red", full(&d), RED);
        pc_layer k = knocker(&d, PC_KNOCKOUT_DEEP);
        pc_layer inner = ch_group("inner", &k, 1);
        pc_layer outer = group2("outer", red, inner);
        ch_push(&d, green);
        ch_push(&d, outer);
        CHECK_AT(&d, 2, 2, WHITE, "deep reveals the Background");
        CHECK_AT(&d, 9, 2, RED, "outside the shape");
        /* An isolated group stops it at its own (transparent) bottom (psd-tools
         * knockout-deep-nested): the layers beneath the group show. */
        d.layers[2].blend = PC_BLEND_NORMAL;
        CHECK_AT(&d, 2, 2, GREEN, "deep inside an isolated group");
        d.layers[2].blend = PC_BLEND_PASS_THROUGH;
        child(&d, 2, 1)->blend = PC_BLEND_NORMAL;
        /* The knockout is the inner group's only layer: its buffer stays transparent. */
        CHECK_AT(&d, 2, 2, RED, "deep inside an isolated inner group");
        pc_document_fini(&d);
    }
}

static void test_top_level_shallow_knockout_reaches_the_background_or_transparency(void)
{
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_document nobg = doc_d(DEPTHS[i], false);
        const float half[4] = {0.5f, 1, 0.5f, 1};
        ch_push(&d, solid_d(&d, "green", full(&d), GREEN));
        ch_push(&d, knocker(&d, PC_KNOCKOUT_SHALLOW));
        CHECK_AT(&d, 2, 2, WHITE, "top-level shallow");
        /* Layer opacity sets the knockout's strength. */
        d.layers[2].opacity = 0.5f;
        CHECK_AT(&d, 2, 2, half, "half-strength knockout");
        /* Without a Background a deep knockout reveals transparency. */
        ch_push(&nobg, solid_d(&nobg, "green", full(&nobg), GREEN));
        ch_push(&nobg, knocker(&nobg, PC_KNOCKOUT_DEEP));
        CHECK_AT(&nobg, 2, 2, CLEAR, "deep without a Background");
        CHECK_AT(&nobg, 9, 2, GREEN, "outside the shape");
        /* A hidden Background doesn't show through either. */
        d.layers[0].visible = false;
        d.layers[2].opacity = 1.0f;
        CHECK_AT(&d, 2, 2, CLEAR, "hidden Background");
        pc_document_fini(&d);
        pc_document_fini(&nobg);
    }
}

static void test_shallow_knockout_in_a_pass_through_group_reveals_what_is_beneath_it(void)
{
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer green = solid_d(&d, "green", full(&d), GREEN);
        pc_layer red = solid_d(&d, "red", full(&d), RED);
        pc_layer g = group2("pt", red, knocker(&d, PC_KNOCKOUT_SHALLOW));
        CHECK(g.blend == PC_BLEND_PASS_THROUGH);
        ch_push(&d, green);
        ch_push(&d, g);
        CHECK_AT(&d, 2, 2, GREEN, "pass-through shallow");
        child(&d, 2, 1)->advanced.knockout = PC_KNOCKOUT_DEEP;
        CHECK_AT(&d, 2, 2, WHITE, "pass-through deep");
        pc_document_fini(&d);
    }
}

static void test_knockout_on_a_group_uses_its_composite_shape_and_fill(void)
{
    /* psd-tools knockout-deep-normal: a group with Deep knockout at 50 % fill. */
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer green = solid_d(&d, "green", full(&d), GREEN);
        pc_layer blue = solid_d(&d, "blue", pc_rect_make(0, 0, 6, 8), BLUE);
        pc_layer g = ch_group("g", &blue, 1);
        const float want[4] = {0.5f, 0.5f, 1, 1};
        g.blend = PC_BLEND_NORMAL;
        g.fill_opacity = 0.5f;
        g.advanced.knockout = PC_KNOCKOUT_DEEP;
        ch_push(&d, green);
        ch_push(&d, g);
        CHECK_AT(&d, 2, 2, want, "blue at 50 % over the Background");
        CHECK_AT(&d, 9, 2, GREEN, "outside the group's pixels");
        pc_document_fini(&d);
    }
}

static void test_knockout_follows_masks_and_transparency_shapes(void)
{
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer k = knocker(&d, PC_KNOCKOUT_DEEP);
        ch_push(&d, solid_d(&d, "green", full(&d), GREEN));
        /* A mask hiding x >= 3: only x < 3 knocks out. */
        mask_left(&k, DEPTHS[i], 3);
        ch_push(&d, k);
        CHECK_AT(&d, 1, 2, WHITE, "inside the mask");
        CHECK_AT(&d, 4, 2, GREEN, "masked out");
        /* Transparency Shapes Layer off: the knockout covers the whole layer. */
        pc_layer_mask_fini(&d.layers[2].mask);
        d.layers[2].has_mask = false;
        d.layers[2].advanced.transparency_shapes = false;
        CHECK_AT(&d, 9, 2, WHITE, "beyond the pixels, tsly off");
        pc_document_fini(&d);
    }
}

static void test_clipped_layer_shallow_knockout_stops_at_the_base(void)
{
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer mid = solid_d(&d, "mid", full(&d), GREEN);
        pc_layer k = knocker(&d, PC_KNOCKOUT_SHALLOW);
        mid.clipped = true;
        k.clipped = true;
        ch_push(&d, solid_d(&d, "base", full(&d), RED));
        ch_push(&d, mid);
        ch_push(&d, k);
        CHECK_AT(&d, 2, 2, RED, "the base shows through the clipped layer above it");
        CHECK_AT(&d, 9, 2, GREEN, "outside");
        d.layers[3].advanced.knockout = PC_KNOCKOUT_DEEP;
        CHECK_AT(&d, 2, 2, RED, "clipped deep stops at the base too");
        pc_document_fini(&d);
    }
}

static void test_blend_clipped_layers_individually(void)
{
    size_t i;
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], false);
        pc_layer base = solid_d(&d, "base", pc_rect_make(0, 0, 6, 8), RED);
        pc_layer clip = solid_d(&d, "clip", full(&d), BLUE);
        const float ind[4] = {0, 0, 0.5f, 1};
        const float half[4] = {0.375f, 0.25f, 0.5f, 1};
        base.blend = PC_BLEND_SCREEN;
        clip.clipped = true;
        clip.blend = PC_BLEND_MULTIPLY;
        ch_push(&d, solid_d(&d, "gray", full(&d), GRAY));
        ch_push(&d, base);
        ch_push(&d, clip);
        /* As a group: red x blue = black, screened onto gray = gray. */
        CHECK_AT(&d, 2, 2, GRAY, "as group");
        /* One by one: red screened onto gray = (1, .5, .5), then x blue = (0, 0, .5). */
        d.layers[1].advanced.blend_clipped = false;
        CHECK_AT(&d, 2, 2, ind, "individually");
        CHECK_AT(&d, 9, 2, GRAY, "outside the base");
        /* Base at 50 %: (.75, .5, .5); the multiply then mixes in at 50 %. */
        d.layers[1].opacity = 0.5f;
        CHECK_AT(&d, 2, 2, half, "half-opacity base");
        pc_document_fini(&d);
    }
}

static void test_blend_interior_effects_as_group_puts_overlays_under_fill_opacity(void)
{
    size_t i;
    NEEDS_FX();
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer l = solid_d(&d, "l", pc_rect_make(0, 0, 6, 8), BLUE);
        const float pink[4] = {1, 0.5f, 0.5f, 1};
        add_effect(&l, overlay(1, 0, 0));
        l.fill_opacity = 0.0f;
        ch_push(&d, l);
        CHECK_AT(&d, 2, 2, RED, "default: overlays ignore fill");
        d.layers[1].advanced.blend_interior = true;
        CHECK_AT(&d, 2, 2, WHITE, "as group at fill 0");
        d.layers[1].fill_opacity = 0.5f;
        CHECK_AT(&d, 2, 2, pink, "as group at fill 50 %");
        d.layers[1].advanced.blend_interior = false;
        CHECK_AT(&d, 2, 2, RED, "default at fill 50 %");
        pc_document_fini(&d);
    }
}

static void test_transparency_shapes_off_spreads_effects_over_the_whole_layer(void)
{
    size_t i;
    NEEDS_FX();
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer l = solid_d(&d, "l", pc_rect_make(0, 0, 6, 8), BLUE);
        add_effect(&l, overlay(1, 0, 0));
        ch_push(&d, l);
        CHECK_AT(&d, 9, 2, WHITE, "default: the overlay follows the pixels");
        d.layers[1].advanced.transparency_shapes = false;
        CHECK_AT(&d, 9, 2, RED, "tsly off: the overlay covers the layer");
        CHECK_AT(&d, 2, 2, RED, "inside");
        d.layers[1].fill_opacity = 0.0f;
        CHECK_AT(&d, 9, 2, RED, "tsly off, fill 0");
        pc_document_fini(&d);
    }
}

static void test_layer_mask_hides_effects(void)
{
    size_t i;
    NEEDS_FX();
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer l = solid_d(&d, "l", full(&d), BLUE);
        add_effect(&l, stroke_fx(2.0f, PC_STROKE_INSIDE, 1, 0, 0));
        /* The mask shows x < 8. */
        mask_left(&l, DEPTHS[i], 8);
        ch_push(&d, l);
        /* Default: the stroke follows the masked shape, so it runs along the mask's edge. */
        CHECK_AT(&d, 7, 4, RED, "stroke along the mask edge");
        CHECK_AT(&d, 9, 4, WHITE, "masked out");
        CHECK_AT(&d, 4, 4, BLUE, "interior");
        /* Layer Mask Hides Effects: the stroke is built from the whole layer (canvas edges only)
         * and the mask then hides the result. */
        d.layers[1].advanced.layer_mask_hides_effects = true;
        CHECK_AT(&d, 7, 4, BLUE, "no stroke at the mask edge");
        CHECK_AT(&d, 4, 0, RED, "the canvas-edge stroke stays");
        CHECK_AT(&d, 9, 4, WHITE, "still masked out");
        CHECK_AT(&d, 11, 0, WHITE, "the stroke beyond the mask is hidden");
        /* The vector-mask switch alone changes nothing for a pixel mask. */
        d.layers[1].advanced.layer_mask_hides_effects = false;
        d.layers[1].advanced.vector_mask_hides_effects = true;
        CHECK_AT(&d, 7, 4, RED, "vmgm without a vector mask");
        pc_document_fini(&d);
    }
}

static void test_vector_mask_hides_effects(void)
{
    static const double pts[4][2] = {{-4, -4}, {8, -4}, {8, 12}, {-4, 12}};
    size_t i;
    NEEDS_FX();
    for (i = 0; i < 3; i++) {
        pc_document d = doc_d(DEPTHS[i], true);
        pc_layer l = solid_d(&d, "l", full(&d), BLUE);
        pc_path p = ch_polygon(pts, 4);
        add_effect(&l, stroke_fx(2.0f, PC_STROKE_INSIDE, 1, 0, 0));
        pc_vector_mask_init(&l.vector_mask, &p);
        l.has_vector_mask = true;
        ch_push(&d, l);
        CHECK_AT(&d, 7, 4, RED, "stroke along the vector mask");
        d.layers[1].advanced.vector_mask_hides_effects = true;
        CHECK_AT(&d, 7, 4, BLUE, "vector mask hides the stroke");
        CHECK_AT(&d, 9, 4, WHITE, "masked out");
        pc_document_fini(&d);
    }
}

static void test_defaults_take_the_plain_paths(void)
{
    pc_document d = doc_d(PC_SAMPLE_TYPE_U8, true);
    pc_layer l = solid_d(&d, "l", full(&d), BLUE);
    pc_layer c = solid_d(&d, "c", full(&d), RED);
    CHECK(!pc_compose_advanced_active(&l, NULL, 0));
    l.advanced.knockout = PC_KNOCKOUT_SHALLOW;
    CHECK(pc_compose_advanced_active(&l, NULL, 0));
    l.advanced.knockout = PC_KNOCKOUT_NONE;
    /* Switches that change nothing for this layer (no effects, no clipped layers) stay inactive. */
    l.advanced.blend_interior = true;
    l.advanced.layer_mask_hides_effects = true;
    l.advanced.blend_clipped = false;
    CHECK(!pc_compose_advanced_active(&l, NULL, 0));
    c.clipped = true;
    CHECK(pc_compose_advanced_active(&l, &c, 1));
    c.visible = false;
    CHECK(!pc_compose_advanced_active(&l, &c, 1));
    add_effect(&l, overlay(1, 0, 0));
    CHECK(pc_compose_advanced_active(&l, NULL, 0)); /* blend interior with effects */
    /* shapeless and the Background convention. */
    CHECK(!pc_compose_shapeless(&l));
    l.advanced.transparency_shapes = false;
    CHECK(pc_compose_shapeless(&l));
    CHECK(pc_compose_is_background(&d.layers[0]));
    CHECK(!pc_compose_is_background(&c));
    pc_layer_fini(&l);
    pc_layer_fini(&c);
    pc_document_fini(&d);
}

static void test_a_knockout_background_does_not_recurse(void)
{
    /* A Background carrying a knockout (only reachable through a file) renders without looping. */
    pc_document d = doc_d(PC_SAMPLE_TYPE_U8, true);
    float got[4];
    d.layers[0].advanced.knockout = PC_KNOCKOUT_DEEP;
    d.layers[0].fill_opacity = 0.0f;
    ch_px(&d, 0, 0, got);
    CHECK(got[3] <= 1.0f);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_shallow_knockout_stops_at_the_bottom_of_an_isolated_group);
    RUN(test_deep_knockout_reaches_the_background_through_groups);
    RUN(test_top_level_shallow_knockout_reaches_the_background_or_transparency);
    RUN(test_shallow_knockout_in_a_pass_through_group_reveals_what_is_beneath_it);
    RUN(test_knockout_on_a_group_uses_its_composite_shape_and_fill);
    RUN(test_knockout_follows_masks_and_transparency_shapes);
    RUN(test_clipped_layer_shallow_knockout_stops_at_the_base);
    RUN(test_blend_clipped_layers_individually);
    RUN(test_blend_interior_effects_as_group_puts_overlays_under_fill_opacity);
    RUN(test_transparency_shapes_off_spreads_effects_over_the_whole_layer);
    RUN(test_layer_mask_hides_effects);
    RUN(test_vector_mask_hides_effects);
    RUN(test_defaults_take_the_plain_paths);
    RUN(test_a_knockout_background_does_not_recurse);
    return pc_t_finish();
}
