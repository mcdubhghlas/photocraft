#include <math.h>
#include "harness.h"
#include "ui/canvas.h"

static void test_tiled_detail_and_reuse(void)
{
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "tiles", pc_size_make(12000, 24), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(0, 0, 0), NULL) == PC_OK);
    float red[4] = {1, 0, 0, 1};
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[0]), 5120, 8, red, 4, NULL) == PC_OK);
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(session, &doc, NULL, &index, NULL) == PC_OK);
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    pc_ui_canvas_tiles tiles = {0};
    pc_ui_canvas_xform xf = {.width = 6000, .height = 24, .zoom = 1, .aspect = 1, .center = {6000, 12}};
    CHECK(pc_ui_canvas_tiles_update(&tiles, state, &xf, NULL) == PC_OK);
    CHECK(tiles.count == 7);
    size_t found = 0;
    for (size_t i = 0; i < tiles.count; i++) {
        const pc_ui_canvas_tile *tile = &tiles.items[i];
        CHECK(tile->cache.region && tile->cache.valid);
        CHECK(pc_rect_width(tile->cache.source_rect) <= 1028);
        if (tile->cache.source_rect.x0 <= 5120 && tile->cache.source_rect.x1 > 5120) {
            const float *pixel = pc_compose_buffer_get(&tile->cache.image, 5120, 8);
            CHECK(pixel != NULL);
            CHECK_NEAR(pixel[0], 1, 0);
            pixel = pc_compose_buffer_get(&tile->cache.image, 5121, 8);
            CHECK_NEAR(pixel[0], 0, 0);
            found++;
        }
    }
    CHECK(found == 2); /* A boundary pixel is also in the adjacent tile's gutter. */
    uint64_t generation = tiles.generation;
    const void *pixels = tiles.items[1].cache.image.px;
    CHECK(pc_ui_canvas_tiles_update(&tiles, state, &xf, NULL) == PC_OK);
    CHECK(tiles.generation == generation && tiles.items[1].cache.image.px == pixels);
    xf.center[0] += 1024;
    CHECK(pc_ui_canvas_tiles_update(&tiles, state, &xf, NULL) == PC_OK);
    CHECK(tiles.generation == generation + 1);
    CHECK(tiles.items[0].cache.image.px == pixels);
    generation = tiles.generation;
    /* Damage outside one tile keeps its exact pixels and texture generation, while a
     * boundary edit invalidates both neighboring gutters. */
    pc_engine_doc_state *mutable = pc_engine_session_active_mut(session);
    mutable->revision++;
    mutable->has_last_damage = true;
    mutable->last_damage = pc_rect_make(5120, 8, 5121, 9);
    const void *unchanged = tiles.items[0].cache.image.px;
    CHECK(pc_ui_canvas_tiles_update(&tiles, state, &xf, NULL) == PC_OK);
    CHECK(tiles.generation == generation + 2);
    CHECK(tiles.items[0].cache.image.px == unchanged);
    for (size_t i = 0; i < tiles.count; i++)
        CHECK(tiles.items[i].cache.revision == state->revision);
    generation = tiles.generation;
    xf.width = NAN;
    CHECK(pc_ui_canvas_tiles_update(&tiles, state, &xf, NULL) == PC_ERR_INVALID);
    CHECK(tiles.generation == generation);
    xf.width = 6000;
    xf.height = 24;
    xf.zoom = .25f;
    CHECK(pc_ui_canvas_tiles_update(&tiles, state, &xf, NULL) == PC_OK);
    CHECK(tiles.count == 0 && tiles.generation == generation);
    xf.zoom = 1;
    CHECK(pc_ui_canvas_tiles_update(&tiles, state, &xf, NULL) == PC_OK);
    CHECK(tiles.generation > generation);
    pc_ui_canvas_tiles_fini(&tiles);
    pc_engine_session_free(session);
}
static void test_tile_budget(void)
{
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "budget", pc_size_make(100000, 100000), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(0, 0, 0), NULL) == PC_OK);
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(session, &doc, NULL, &index, NULL) == PC_OK);
    pc_ui_canvas_tiles tiles = {0};
    pc_ui_canvas_xform xf = {
        .width = 100000, .height = 100000, .zoom = 1, .aspect = 1, .center = {50000, 50000}};
    CHECK(pc_ui_canvas_tiles_update(&tiles, pc_engine_session_active(session), &xf, NULL) == PC_ERR_LIMIT);
    CHECK(tiles.count == 0 && tiles.generation == 0);
    pc_ui_canvas_tiles_fini(&tiles);
    pc_engine_session_free(session);
}
int main(int argc, char **argv)
{
    RUN(test_tiled_detail_and_reuse);
    RUN(test_tile_budget);
    return pc_t_finish();
}
