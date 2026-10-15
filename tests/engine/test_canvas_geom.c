/* Canvas-geometry parity (crates/engine/src/canvas_geom.rs). The reference's turn tests drive the
 * image-rotation commands (E3) over type/shape/smart content; E1 covers the geometry primitives
 * (map_angle, Turn affines, map_rect) and turn_canvas on a raster document directly. */
#include <stdlib.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static void test_map_angle_follows_the_canvas(void)
{
    pc_affine a = pc_engine_turn_affine(PC_ENGINE_TURN_CW90, 10.0, 20.0);
    CHECK(pc_engine_map_angle(&a, 90.0f) == 0.0f);
    pc_affine fh = pc_engine_turn_affine(PC_ENGINE_TURN_FLIP_H, 10.0, 20.0);
    CHECK(pc_engine_map_angle(&fh, 0.0f) == 180.0f);
    pc_affine fv = pc_engine_turn_affine(PC_ENGINE_TURN_FLIP_V, 10.0, 20.0);
    CHECK(pc_engine_map_angle(&fv, 30.0f) == -30.0f);
    pc_affine ccw = pc_engine_turn_affine(PC_ENGINE_TURN_CCW90, 10.0, 20.0);
    CHECK(pc_engine_map_angle(&ccw, 0.0f) == 90.0f);
}

static void test_map_rect(void)
{
    /* Cw90 on a 40x30 canvas: a slice [1,2 .. 11,8] -> x' = H - y. */
    pc_affine a = pc_engine_turn_affine(PC_ENGINE_TURN_CW90, 40.0, 30.0);
    pc_rect r = pc_engine_map_rect(&a, pc_rect_make(1, 2, 11, 8));
    CHECK(r.x0 == 30 - 8 && r.y0 == 1 && r.x1 == 30 - 2 && r.y1 == 11);
    CHECK(pc_rect_is_empty(pc_engine_map_rect(&a, PC_RECT_EMPTY)));
}

/* Distinct per-pixel value so a remap is detectable. */
static void fill_pattern(pc_surface *s, int32_t w, int32_t h)
{
    size_t n = (size_t)w * (size_t)h * 4;
    float *px = malloc(n * sizeof *px);
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            size_t i = ((size_t)y * (size_t)w + (size_t)x) * 4;
            px[i] = (float)x / (float)w;
            px[i + 1] = (float)y / (float)h;
            px[i + 2] = (float)((x * 7 + y * 13) % 31) / 30.0f;
            px[i + 3] = 1.0f;
        }
    }
    CHECK(pc_surface_write_region(s, pc_rect_make(0, 0, w, h), px, n, NULL) == PC_OK);
    free(px);
}

static void test_turn_canvas_remaps_raster_exactly(void)
{
    const int32_t W = 4, H = 3;
    pc_document doc;
    CHECK(pc_document_init(&doc, "t", pc_size_make((uint32_t)W, (uint32_t)H), PC_COLOR_MODE_RGB,
                           PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    pc_layer l;
    CHECK(pc_layer_raster(&l, "L", pc_document_pixel_format(&doc), NULL) == PC_OK);
    fill_pattern(pc_layer_surface_mut(&l), W, H);
    pc_layer_id id;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);

    /* Snapshot the original pixels. */
    float *before = malloc((size_t)W * (size_t)H * 4u * sizeof *before);
    CHECK(pc_surface_read_region_into(pc_layer_surface(pc_document_layer(&doc, id)),
                                      pc_rect_make(0, 0, W, H), before, (size_t)W * (size_t)H * 4u, NULL) == PC_OK);

    CHECK(pc_engine_turn_canvas(&doc, PC_ENGINE_TURN_CW90, NULL) == PC_OK);
    CHECK(doc.size.width == (uint32_t)H && doc.size.height == (uint32_t)W); /* swapped */
    const int32_t NW = H, NH = W;
    float *after = malloc((size_t)NW * (size_t)NH * 4u * sizeof *after);
    CHECK(pc_surface_read_region_into(pc_layer_surface(pc_document_layer(&doc, id)),
                                      pc_rect_make(0, 0, NW, NH), after, (size_t)NW * (size_t)NH * 4u, NULL) == PC_OK);
    /* Cw90: (x, y) -> (H-1-y, x). */
    for (int32_t y = 0; y < H; y++) {
        for (int32_t x = 0; x < W; x++) {
            int32_t nx = H - 1 - y, ny = x;
            size_t src = ((size_t)y * (size_t)W + (size_t)x) * 4;
            size_t dst = ((size_t)ny * (size_t)NW + (size_t)nx) * 4;
            for (size_t c = 0; c < 4; c++) {
                CHECK(after[dst + c] == before[src + c]);
            }
        }
    }
    free(after);
    free(before);
    pc_document_fini(&doc);
}

static void test_flip_twice_is_identity(void)
{
    const int32_t W = 5, H = 4;
    pc_document doc;
    CHECK(pc_document_init(&doc, "t", pc_size_make((uint32_t)W, (uint32_t)H), PC_COLOR_MODE_RGB,
                           PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    pc_layer l;
    CHECK(pc_layer_raster(&l, "L", pc_document_pixel_format(&doc), NULL) == PC_OK);
    fill_pattern(pc_layer_surface_mut(&l), W, H);
    pc_layer_id id;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);
    size_t n = (size_t)W * (size_t)H * 4u;
    float *before = malloc(n * sizeof *before);
    CHECK(pc_surface_read_region_into(pc_layer_surface(pc_document_layer(&doc, id)),
                                      pc_rect_make(0, 0, W, H), before, n, NULL) == PC_OK);
    CHECK(pc_engine_turn_canvas(&doc, PC_ENGINE_TURN_FLIP_V, NULL) == PC_OK);
    CHECK(pc_engine_turn_canvas(&doc, PC_ENGINE_TURN_FLIP_V, NULL) == PC_OK);
    CHECK(doc.size.width == (uint32_t)W && doc.size.height == (uint32_t)H);
    float *after = malloc(n * sizeof *after);
    CHECK(pc_surface_read_region_into(pc_layer_surface(pc_document_layer(&doc, id)),
                                      pc_rect_make(0, 0, W, H), after, n, NULL) == PC_OK);
    for (size_t i = 0; i < n; i++) {
        CHECK(after[i] == before[i]);
    }
    free(after);
    free(before);
    pc_document_fini(&doc);
}

int main(int argc, char **argv)
{
    RUN(test_map_angle_follows_the_canvas);
    RUN(test_map_rect);
    RUN(test_turn_canvas_remaps_raster_exactly);
    RUN(test_flip_twice_is_identity);
    return pc_t_finish();
}
