/* Headless port of crates/gpu/src/bounds.rs: layer bounds as photocraft-compose computes them,
 * plus the GPU planner's effect_region (the "keep the whole region or clip it to the padded
 * canvas" choice, #761). No GL context is needed; these are pure geometry queries. */
#include <stdlib.h>
#include "harness.h"
#include "base/mem.h"
#include "gpu/gpu.h"
#include "compose/compose.h"
#include "compose/proxy.h"
#include "compose/effects.h"
#include "doc/doc.h"
#include "raster/surface.h"

/* bounds.rs::matches_surface_content_bounds — the cached, tile-keyed scan equals the surface's
 * own content_bounds, before and after edits, for several formats. */
static void test_content_bounds_matches_surface(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_rect_eq(pc_compose_content_bounds(&s), pc_surface_content_bounds(&s)));
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float blue[4] = {0.0f, 0.0f, 1.0f, 0.5f};
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(10, 300, 40, 700), red, 4, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(-50, 5, -3, 9), blue, 4, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_compose_content_bounds(&s), pc_surface_content_bounds(&s)));
    /* Cached a second time, and after a change. */
    CHECK(pc_rect_eq(pc_compose_content_bounds(&s), pc_surface_content_bounds(&s)));
    const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(600, 600, 601, 601), green, 4, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_compose_content_bounds(&s), pc_surface_content_bounds(&s)));
    pc_surface_fini(&s);

    pc_surface g;
    pc_surface_init(&g, PC_PIXEL_FORMAT_GRAY8);
    const float grey[1] = {0.5f};
    CHECK(pc_surface_fill_rect(&g, pc_rect_make(3, 3, 9, 9), grey, 1, NULL) == PC_OK);
    CHECK(pc_rect_eq(pc_compose_content_bounds(&g), pc_surface_content_bounds(&g)));
    pc_surface_fini(&g);
}

/* Attach one enabled default drop shadow to a layer so it has a non-zero effect margin. */
static void add_drop_shadow(pc_layer *layer)
{
    pc_effect *items = pc_calloc_array(1, sizeof *items);
    CHECK(items != NULL);
    pc_effect_default_drop_shadow(&items[0]);
    layer->effects.enabled = true;
    layer->effects.items = items;
    layer->effects.nitems = 1;
}

/* bounds.rs::effect_region — a layer whose effect source stays near the canvas keeps its whole
 * padded region (so its maps move with it instead of being rebuilt every frame); a layer reaching
 * far past the canvas keeps only the part inside the padded canvas. The maps are exact either way. */
static void test_effect_region_whole_vs_window(void)
{
    const pc_rect canvas = pc_rect_make(0, 0, 100, 100);

    /* Small content inside the canvas: the whole padded region is kept. */
    pc_layer small;
    CHECK(pc_layer_raster(&small, "small", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&small), pc_rect_make(10, 10, 30, 30), red, 4, NULL) ==
          PC_OK);
    add_drop_shadow(&small);
    int32_t margin = pc_compose_effects_margin(&small);
    CHECK(margin > 0);
    pc_rect source = pc_compose_effect_source_bounds(&small, canvas);
    CHECK(pc_rect_eq(source, pc_rect_make(10, 10, 30, 30)));
    pc_rect region = pc_gpu_effect_region(&small, canvas);
    CHECK(pc_rect_eq(region, pc_rect_inflate(source, margin)));
    /* It must not have been clamped to the padded canvas. */
    CHECK(!pc_rect_eq(region, pc_rect_inflate(canvas, margin)));
    pc_layer_fini(&small);

    /* Content reaching far past the canvas in every direction: the region is clamped to the padded
     * canvas (the whole padded region would be far larger than the canvas plus a quarter). */
    pc_layer big;
    CHECK(pc_layer_raster(&big, "big", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&big), pc_rect_make(-500, -500, 600, 600), red, 4,
                               NULL) == PC_OK);
    add_drop_shadow(&big);
    int32_t big_margin = pc_compose_effects_margin(&big);
    pc_rect big_region = pc_gpu_effect_region(&big, canvas);
    CHECK(pc_rect_eq(big_region, pc_rect_inflate(canvas, big_margin)));
    pc_layer_fini(&big);

    /* A layer about the canvas's size keeps its whole region even where it overhangs the edge, so
     * the region keeps its size as the layer moves (#761): overhanging by less than a quarter of
     * the canvas area stays on the whole-region branch. */
    pc_layer near;
    CHECK(pc_layer_raster(&near, "near", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&near), pc_rect_make(-5, -5, 105, 105), red, 4,
                               NULL) == PC_OK);
    add_drop_shadow(&near);
    int32_t near_margin = pc_compose_effects_margin(&near);
    pc_rect near_source = pc_compose_effect_source_bounds(&near, canvas);
    pc_rect near_region = pc_gpu_effect_region(&near, canvas);
    CHECK(pc_rect_eq(near_region, pc_rect_inflate(near_source, near_margin)));
    pc_layer_fini(&near);
}

/* A layer with no effects has a zero-growth margin, so its effect region is its own bounds. */
static void test_effect_region_no_effects(void)
{
    const pc_rect canvas = pc_rect_make(0, 0, 64, 64);
    pc_layer plain;
    CHECK(pc_layer_raster(&plain, "plain", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&plain), pc_rect_make(5, 5, 20, 20), red, 4, NULL) ==
          PC_OK);
    /* Margin is 2 even with no effects (the fixed padding), matching the compose query. */
    int32_t margin = pc_compose_effects_margin(&plain);
    pc_rect source = pc_compose_effect_source_bounds(&plain, canvas);
    pc_rect region = pc_gpu_effect_region(&plain, canvas);
    CHECK(pc_rect_eq(region, pc_rect_inflate(source, margin)));
    pc_layer_fini(&plain);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_content_bounds_matches_surface);
    RUN(test_effect_region_whole_vs_window);
    RUN(test_effect_region_no_effects);
    return pc_t_finish();
}
