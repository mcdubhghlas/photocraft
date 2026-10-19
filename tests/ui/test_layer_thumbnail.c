#include "harness.h"
#include "ui/layer_thumbnail.h"
#include <stdlib.h>
#include <math.h>

static void test_native_modes_depths_and_disabled_pixel_mask(void)
{
    const pc_color_mode modes[] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                   PC_COLOR_MODE_LAB};
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t mode = 0; mode < 4; mode++) {
        for (size_t depth = 0; depth < 3; depth++) {
            pc_document doc;
            CHECK(pc_document_with_background(&doc, "thumb", pc_size_make(32, 16), modes[mode], depths[depth],
                                              pc_color_rgb(.2f, .4f, .8f), NULL) == PC_OK);
            pc_layer *layer = &doc.layers[0];
            pc_rgba8_image image = {0};
            CHECK(pc_ui_layer_thumbnail(&doc, layer, PC_UI_THUMB_LAYER, 16, &image, NULL) == PC_OK);
            CHECK(image.width == 16 && image.height == 16);
            CHECK(image.pixels[3] == 0); /* Letterboxed above the document. */
            float expected[4];
            pc_surface_rgba(pc_layer_surface(layer), 17, 9, expected);
            for (size_t ch = 0; ch < 4; ch++)
                CHECK_NEAR(image.pixels[(8 * 16 + 8) * 4 + ch],
                           floorf(fminf(1, fmaxf(0, expected[ch])) * 255 + .5f), 1);
            layer->has_mask = true;
            pc_layer_mask_reveal_all(&layer->mask);
            layer->mask.enabled = false;
            const float value[1] = {.25f};
            CHECK(pc_surface_fill_rect(&layer->mask.surface, pc_rect_make(0, 0, 32, 16), value, 1, NULL) ==
                  PC_OK);
            CHECK(pc_ui_layer_thumbnail(&doc, layer, PC_UI_THUMB_PIXEL_MASK, 16, &image, NULL) == PC_OK);
            CHECK(image.pixels[(8 * 16 + 8) * 4] == 64);
            CHECK(image.pixels[(8 * 16 + 8) * 4 + 3] == 255);
            uint8_t *pixels = image.pixels;
            CHECK(pc_ui_layer_thumbnail(&doc, layer, PC_UI_THUMB_LAYER, 129, &image, NULL) == PC_ERR_INVALID);
            CHECK(image.pixels == pixels);
            pc_rgba8_image_fini(&image);
            pc_document_fini(&doc);
        }
    }
}
static void test_vector_geometry_and_disabled_mask(void)
{
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "vector", pc_size_make(64, 32), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgb(1, 1, 1), NULL) == PC_OK);
    pc_path path = {0};
    path.subpaths = calloc(1, sizeof *path.subpaths);
    CHECK(path.subpaths != NULL);
    path.nsubpaths = 1;
    const double points[][2] = {{0, 0}, {32, 0}, {32, 32}, {0, 32}};
    CHECK(pc_subpath_polygon(points, 4, &path.subpaths[0], NULL) == PC_OK);
    doc.layers[0].has_vector_mask = true;
    pc_vector_mask_init(&doc.layers[0].vector_mask, &path);
    doc.layers[0].vector_mask.enabled = false;
    pc_rgba8_image image = {0};
    CHECK(pc_ui_layer_thumbnail(&doc, &doc.layers[0], PC_UI_THUMB_VECTOR_MASK, 64, &image, NULL) == PC_OK);
    CHECK(image.pixels[(32 * 64 + 16) * 4] == 255);
    CHECK(image.pixels[(32 * 64 + 48) * 4] == 128);
    CHECK(image.pixels[3] == 0);
    CHECK(!doc.layers[0].vector_mask.enabled);
    pc_rgba8_image_fini(&image);
    pc_document_fini(&doc);
}
int main(int argc, char **argv)
{
    RUN(test_native_modes_depths_and_disabled_pixel_mask);
    RUN(test_vector_geometry_and_disabled_mask);
    return pc_t_finish();
}
