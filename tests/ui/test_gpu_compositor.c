#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "harness.h"
#include "base/mem.h"
#include "gpu/gpu.h"
#include "gpu/fx_gl.h"
#include "compose/fx_internal.h"
#include "compose/psblend.h"
#include "vector/vector.h"
#include "ui/spike/gl33.h"

typedef struct fixture {
    SDL_Window *window;
    SDL_GLContext context;
    gl33 gl;
    pc_gpu_compositor *gpu;
} fixture;
static bool start(fixture *f)
{
    if (!getenv("PC_TEST_GL")) {
        return false;
    }
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    CHECK(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3));
    CHECK(SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3));
    CHECK(SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE));
    f->window = SDL_CreateWindow("PhotoCraft GPU tests", 32, 32, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    CHECK(f->window != NULL);
    if (!f->window) {
        return false;
    }
    f->context = SDL_GL_CreateContext(f->window);
    CHECK(f->context != NULL);
    const char *missing = NULL;
    CHECK(gl33_load(&f->gl, &missing));
    pc_error err;
    pc_status rc = pc_gpu_compositor_new(&f->gl, &f->gpu, &err);
    if (rc != PC_OK) {
        PC_T_FAIL("GPU init: %s", err.msg);
    }
    return rc == PC_OK;
}
static void finish(fixture *f)
{
    pc_gpu_compositor_free(f->gpu);
    SDL_GL_DestroyContext(f->context);
    SDL_DestroyWindow(f->window);
    SDL_Quit();
}
static void compare(fixture *f, const pc_document *doc, pc_rect rect)
{
    pc_compose_buffer cpu = {0}, gpu = {0};
    CHECK(pc_compose_render(doc, rect, &cpu, NULL) == PC_OK);
    pc_error err;
    pc_status rc = pc_gpu_render(f->gpu, doc, rect, &gpu, &err);
    CHECK(rc == PC_OK);
    if (rc != PC_OK) {
        PC_T_FAIL("GPU render: %s", err.msg);
    }
    CHECK(cpu.len == gpu.len);
    CHECK(pc_rect_eq(gpu.rect, rect));
    if (cpu.len == gpu.len) {
        float worst = 0;
        bool finite = true;
        for (size_t i = 0; i < cpu.len; i++) {
            for (size_t c = 0; c < 4; c++) {
                float a = c == 3 ? cpu.px[i][c] : cpu.px[i][c] * cpu.px[i][3];
                float b = c == 3 ? gpu.px[i][c] : gpu.px[i][c] * gpu.px[i][3];
                finite = finite && isfinite(a) && isfinite(b);
                worst = fmaxf(worst, fabsf(a - b));
            }
        }
        CHECK(finite);
        CHECK_NEAR(worst, 0, 1.f / 255 + 4 * FLT_EPSILON);
        if (worst > 1.f / 255 + 4 * FLT_EPSILON) {
            const pc_layer *top = doc->nlayers ? &doc->layers[doc->nlayers - 1] : NULL;
            PC_T_FAIL("GPU mismatch depth=%d kind=%d adjustment=%d worst=%g", (int)doc->depth,
                      top ? (int)top->content.kind : -1,
                      top && top->content.kind == PC_LAYER_ADJUSTMENT ? (int)top->content.u.adjustment.kind
                                                                      : -1,
                      (double)worst);
        }
    }
    pc_compose_buffer_fini(&cpu);
    pc_compose_buffer_fini(&gpu);
}
static void test_gpu_blends_and_chunks(void)
{
    fixture f = {0};
    if (!start(&f)) {
        return;
    }
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    pc_blend_mode modes[PC_BLEND_ALL_LAYER_MODE_COUNT];
    size_t n_modes = 0;
    for (pc_blend_mode mode = PC_BLEND_NORMAL; mode < PC_BLEND_COUNT; mode++) {
        modes[n_modes++] = mode;
    }
    for (size_t depth = 0; depth < 3; depth++) {
        pc_document doc;
        CHECK(pc_document_init(&doc, "GPU", pc_size_make(530, 270), PC_COLOR_MODE_RGB, depths[depth], NULL) ==
              PC_OK);
        doc.layers = pc_calloc_array(2, sizeof *doc.layers);
        CHECK(doc.layers != NULL);
        doc.nlayers = 2;
        for (size_t i = 0; i < 2; i++) {
            CHECK(pc_layer_raster(&doc.layers[i], "pixels", pc_document_pixel_format(&doc), NULL) == PC_OK);
            for (int32_t y = 0; y < 270; y++) {
                for (int32_t x = 0; x < 530; x++) {
                    float pixel[4] = {(float)((x + (int32_t)i * 73) % 256) / 255,
                                      (float)((y + (int32_t)i * 37) % 256) / 255, .2f + .5f * (float)i,
                                      (float)((x + y + (int32_t)i * 17) % 256) / 255};
                    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[i]), x, y, pixel, 4,
                                                 NULL) == PC_OK);
                }
            }
        }
        for (size_t mode = 0; mode < sizeof modes / sizeof *modes; mode++) {
            doc.layers[1].blend = modes[mode];
            doc.layers[1].opacity = .6f;
            doc.layers[1].fill_opacity = .7f;
            compare(&f, &doc, pc_rect_make(-3, -5, 537, 277));
        }
        doc.layers[1].blend = PC_BLEND_SCREEN;
        doc.layers[1].excluded_channels = 5;
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_blend_range pair[2] = {PC_BLEND_RANGE_FULL, PC_BLEND_RANGE_FULL};
        pair[0].black[0] = 10;
        pair[0].black[1] = 50;
        pair[0].white[0] = 190;
        pair[0].white[1] = 240;
        CHECK(pc_blend_if_set(&doc.layers[1].blend_if, 0, pair, NULL) == PC_OK);
        compare(&f, &doc, pc_document_bounds(&doc));
        doc.layers[1].excluded_channels = 0;
        doc.layers[1].clipped = true;
        doc.layers[1].blend = PC_BLEND_MULTIPLY;
        compare(&f, &doc, pc_document_bounds(&doc));
        doc.layers[0].visible = false;
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_document_fini(&doc);
    }
    CHECK(pc_gpu_compositor_submissions(f.gpu) == 3 * (n_modes * 12 + 4 * 6));
    finish(&f);
}
static void test_gpu_groups_and_device_fault(void)
{
    fixture f = {0};
    if (!start(&f)) {
        return;
    }
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "groups", pc_size_make(8, 8), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(.2f, .4f, .6f), NULL) == PC_OK);
    doc.layers = realloc(doc.layers, 2 * sizeof *doc.layers);
    CHECK(doc.layers != NULL);
    pc_layer *children = pc_calloc_array(1, sizeof *children);
    CHECK(children != NULL);
    CHECK(pc_layer_raster(children, "red", PC_PIXEL_FORMAT_RGBA32F, NULL) == PC_OK);
    float red[4] = {1, 0, 0, .7f};
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(children), pc_rect_make(1, 1, 7, 7), red, 4, NULL) ==
          PC_OK);
    CHECK(pc_layer_group(&doc.layers[1], "group", children, 1, NULL) == PC_OK);
    doc.nlayers = 2;
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[1].opacity = .6f;
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[1].has_mask = true;
    pc_layer_mask_reveal_all(&doc.layers[1].mask);
    float half[1] = {.4f};
    CHECK(pc_surface_fill_rect(&doc.layers[1].mask.surface, pc_rect_make(2, 2, 6, 6), half, 1, NULL) ==
          PC_OK);
    doc.layers[1].mask.density = .7f;
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[1].mask.feather = 1.5f;
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[1].fill_opacity = .5f;
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[1].fill_opacity = 1;
    doc.layers[1].blend = PC_BLEND_MULTIPLY;
    compare(&f, &doc, pc_document_bounds(&doc));
    children[0].has_mask = true;
    pc_layer_mask_reveal_all(&children[0].mask);
    CHECK(pc_surface_fill_rect(&children[0].mask.surface, pc_rect_make(1, 1, 4, 4), half, 1, NULL) == PC_OK);
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[1].content.u.group.has_artboard = true;
    pc_artboard_init(&doc.layers[1].content.u.group.artboard, pc_rect_make(2, 1, 7, 6));
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[1].blend = PC_BLEND_PASS_THROUGH;
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[1].content.u.group.artboard.background.kind = PC_ARTBOARD_TRANSPARENT;
    compare(&f, &doc, pc_document_bounds(&doc));
    pc_artboard_fini(&doc.layers[1].content.u.group.artboard);
    doc.layers[1].content.u.group.has_artboard = false;
    /* A clipped child above a pass-through group contributes its premultiplied difference. */
    pc_layer *grown = realloc(doc.layers, 3 * sizeof *doc.layers);
    CHECK(grown != NULL);
    doc.layers = grown;
    CHECK(pc_layer_raster(&doc.layers[2], "clip", PC_PIXEL_FORMAT_RGBA32F, NULL) == PC_OK);
    doc.layers[2].clipped = true;
    doc.layers[2].blend = PC_BLEND_SCREEN;
    const float green[4] = {.1f, .8f, .2f, .6f};
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[2]), pc_rect_make(0, 0, 8, 8), green, 4,
                               NULL) == PC_OK);
    doc.nlayers = 3;
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[2].visible = false;
    /* Cached text blends coverage at its configured gamma. */
    pc_layer_content_fini(&doc.layers[2].content);
    doc.layers[2].content = (pc_layer_content){.kind = PC_LAYER_TEXT};
    pc_text_layer_default(&doc.layers[2].content.u.text);
    doc.layers[2].content.u.text.has_cache = true;
    pc_surface_init(&doc.layers[2].content.u.text.cache, PC_PIXEL_FORMAT_RGBA32F);
    CHECK(pc_surface_fill_rect(&doc.layers[2].content.u.text.cache, pc_rect_make(0, 0, 8, 8), green, 4,
                               NULL) == PC_OK);
    doc.layers[2].visible = true;
    for (unsigned i = 0; i < 2; i++) {
        doc.layers[2].clipped = i != 0;
        pc_psblend_set_text_gamma(1.8f);
        compare(&f, &doc, pc_document_bounds(&doc));
    }
    pc_psblend_set_text_gamma(PC_PSBLEND_TEXT_GAMMA);
    pc_compose_buffer out = {0};
    CHECK(pc_compose_buffer_filled(pc_rect_make(0, 0, 1, 1), red, &out, NULL) == PC_OK);
    const void *saved = out.px;
    doc.layers[1].advanced.knockout = PC_KNOCKOUT_DEEP;
    uint64_t submissions = pc_gpu_compositor_submissions(f.gpu);
    CHECK(pc_gpu_render(f.gpu, &doc, pc_document_bounds(&doc), &out, NULL) == PC_ERR_UNSUPPORTED);
    CHECK(out.px == saved && pc_gpu_compositor_submissions(f.gpu) == submissions);
    doc.layers[1].advanced.knockout = PC_KNOCKOUT_NONE;
    pc_gpu_compositor_fault(f.gpu, false, "\n  first error\nmore");
    pc_gpu_compositor_fault(f.gpu, false, "ignored");
    char detail[256];
    bool lost = true;
    CHECK(pc_gpu_compositor_fault_detail(f.gpu, &lost, detail) && !lost);
    CHECK_EQ_STR(detail, "first error");
    CHECK(!pc_gpu_compositor_healthy(f.gpu));
    CHECK(pc_gpu_render(f.gpu, &doc, pc_document_bounds(&doc), &out, NULL) == PC_ERR_IO);
    CHECK(out.px == saved && pc_gpu_compositor_submissions(f.gpu) == submissions);
    pc_gpu_compositor_fault(f.gpu, true, "reset");
    CHECK(pc_gpu_compositor_fault_detail(f.gpu, &lost, detail) && lost);
    CHECK_EQ_STR(detail, "reset");
    pc_gpu_compositor_fault(f.gpu, true, "later");
    CHECK(pc_gpu_compositor_fault_detail(f.gpu, &lost, detail));
    CHECK_EQ_STR(detail, "reset");
    pc_compose_buffer_fini(&out);
    pc_document_fini(&doc);
    finish(&f);
}
static void test_gpu_adjustments(void)
{
    fixture f = {0};
    if (!start(&f)) {
        return;
    }
    pc_adjustment cases[25] = {0};
    cases[0].kind = PC_ADJ_INVERT;
    cases[1].kind = PC_ADJ_THRESHOLD;
    cases[1].u.threshold.level = .5f;
    cases[2].kind = PC_ADJ_POSTERIZE;
    cases[2].u.posterize.levels = 5;
    cases[3].kind = PC_ADJ_BRIGHTNESS_CONTRAST;
    cases[3].u.brightness_contrast.brightness = 30;
    cases[3].u.brightness_contrast.contrast = 40;
    cases[4] = cases[3];
    cases[4].u.brightness_contrast.legacy = true;
    cases[5].kind = PC_ADJ_EXPOSURE;
    cases[5].u.exposure.exposure = .7f;
    cases[5].u.exposure.offset = -.02f;
    cases[5].u.exposure.gamma = 1.3f;
    pc_adjustment_identity_levels(&cases[6]);
    cases[6].u.levels.master.in_black = .1f;
    cases[6].u.levels.master.gamma = .7f;
    cases[6].u.levels.per_channel[1].out_white = .8f;
    CHECK(pc_adjustment_identity_curves(&cases[7], NULL) == PC_OK);
    cases[7].u.curves.master.pts[1].output = .8f;
    cases[8].kind = PC_ADJ_CHANNEL_MIXER;
    cases[8].u.channel_mixer.matrix[0][0] = .8f;
    cases[8].u.channel_mixer.matrix[0][1] = .3f;
    cases[8].u.channel_mixer.matrix[1][1] = 1;
    cases[8].u.channel_mixer.matrix[2][2] = .7f;
    cases[9].kind = PC_ADJ_PHOTO_FILTER;
    cases[9].u.photo_filter.color[0] = 1;
    cases[9].u.photo_filter.color[1] = .4f;
    cases[9].u.photo_filter.density = .4f;
    cases[9].u.photo_filter.preserve_luminosity = true;
    cases[10].kind = PC_ADJ_BLACK_WHITE;
    const float weights[6] = {40, 60, 40, 60, 20, 80};
    memcpy(cases[10].u.black_white.weights, weights, sizeof weights);
    cases[10].u.black_white.has_tint = true;
    cases[10].u.black_white.tint[0] = 1;
    cases[10].u.black_white.tint[1] = .2f;
    cases[11].kind = PC_ADJ_COLOR_BALANCE;
    cases[11].u.color_balance.midtones[0] = 20;
    cases[11].u.color_balance.shadows[2] = -10;
    cases[11].u.color_balance.preserve_luminosity = true;
    pc_adjustment_default_hue_saturation(&cases[12]);
    cases[12].u.hue_saturation.hue = 37;
    cases[12].u.hue_saturation.saturation = 30;
    cases[12].u.hue_saturation.lightness = 20;
    cases[12].u.hue_saturation.ranges[2].hue = -15;
    cases[12].u.hue_saturation.ranges[2].lightness = -10;
    cases[13] = cases[12];
    cases[13].u.hue_saturation.colorize = true;
    cases[13].u.hue_saturation.hue = 260;
    cases[13].u.hue_saturation.saturation = 60;
    cases[13].u.hue_saturation.lightness = -30;
    cases[14].kind = PC_ADJ_VIBRANCE;
    cases[14].u.vibrance.vibrance = 50;
    cases[14].u.vibrance.saturation = 10;
    cases[15] = cases[14];
    cases[15].u.vibrance.vibrance = -40;
    cases[15].u.vibrance.saturation = -20;
    cases[16].kind = PC_ADJ_SELECTIVE_COLOR;
    cases[16].u.selective_color.adjustments[0][0] = 20;
    cases[16].u.selective_color.adjustments[6][3] = 10;
    cases[16].u.selective_color.adjustments[7][1] = -20;
    cases[17] = cases[16];
    cases[17].u.selective_color.relative = true;
    cases[18].kind = PC_ADJ_GRADIENT_MAP;
    cases[18].u.gradient_map.nstops = 3;
    cases[18].u.gradient_map.stops = pc_calloc_array(3, sizeof(pc_gradient_map_stop));
    CHECK(cases[18].u.gradient_map.stops != NULL);
    cases[18].u.gradient_map.stops[0] = (pc_gradient_map_stop){.pos = 0, .rgb = {.1f, .2f, .3f}};
    cases[18].u.gradient_map.stops[1] = (pc_gradient_map_stop){.pos = .3f, .rgb = {.7f, .3f, .1f}};
    cases[18].u.gradient_map.stops[2] = (pc_gradient_map_stop){.pos = 1, .rgb = {1, 1, .6f}};
    float table[3 * 3 * 3 * 3];
    for (size_t b = 0; b < 3; b++) {
        for (size_t g = 0; g < 3; g++) {
            for (size_t r = 0; r < 3; r++) {
                size_t at = ((b * 3 + g) * 3 + r) * 3;
                table[at] = .1f + .5f * (float)(r * r) / 4;
                table[at + 1] = .2f + .7f * (float)(g * g) / 4;
                table[at + 2] = .05f + .9f * (float)b / 2;
            }
        }
    }
    cases[19].kind = PC_ADJ_COLOR_LOOKUP;
    cases[19].u.color_lookup.size = 3;
    CHECK(pc_blob_new(table, sizeof table, &cases[19].u.color_lookup.lut, NULL) == PC_OK);
    CHECK(pc_adjustment_clone(&cases[19], &cases[20], NULL) == PC_OK);
    cases[20].u.color_lookup.tetrahedral = true;
    cases[20].u.color_lookup.dither = true;
    CHECK(pc_adjustment_clone(&cases[18], &cases[21], NULL) == PC_OK);
    cases[21].u.gradient_map.reverse = cases[21].u.gradient_map.dither = true;
    cases[22] = cases[9];
    cases[22].u.photo_filter.preserve_luminosity = false;
    cases[23] = cases[10];
    cases[23].u.black_white.has_tint = false;
    cases[24] = cases[11];
    cases[24].u.color_balance.preserve_luminosity = false;
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t depth = 0; depth < 3; depth++) {
        pc_document doc;
        CHECK(pc_document_init(&doc, "adjustments", pc_size_make(57, 31), PC_COLOR_MODE_RGB, depths[depth],
                               NULL) == PC_OK);
        doc.layers = pc_calloc_array(2, sizeof *doc.layers);
        CHECK(doc.layers != NULL);
        doc.nlayers = 2;
        CHECK(pc_layer_raster(&doc.layers[0], "pixels", pc_document_pixel_format(&doc), NULL) == PC_OK);
        for (int32_t y = 0; y < 31; y++) {
            for (int32_t x = 0; x < 57; x++) {
                float pixel[4] = {(float)x / 56, (float)y / 30, (float)((x + y) % 31) / 30,
                                  (float)((x + y) % 10) / 9};
                if (depth == 2) {
                    for (size_t ch = 0; ch < 3; ch++) {
                        pixel[ch] = pixel[ch] * 3 - .2f;
                    }
                }
                CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[0]), x, y, pixel, 4, NULL) ==
                      PC_OK);
            }
        }
        pc_layer_content content = {.kind = PC_LAYER_ADJUSTMENT};
        content.u.adjustment.kind = PC_ADJ_INVERT;
        CHECK(pc_layer_init(&doc.layers[1], "adjustment", &content, NULL) == PC_OK);
        doc.layers[1].opacity = .7f;
        doc.layers[1].fill_opacity = .6f;
        doc.layers[1].has_mask = true;
        pc_layer_mask_reveal_all(&doc.layers[1].mask);
        const float half[1] = {.5f};
        CHECK(pc_surface_fill_rect(&doc.layers[1].mask.surface, pc_rect_make(10, 5, 30, 25), half, 1, NULL) ==
              PC_OK);
        for (size_t j = 0; j < 25; j++) {
            pc_adjustment_fini(&doc.layers[1].content.u.adjustment);
            CHECK(pc_adjustment_clone(&cases[j], &doc.layers[1].content.u.adjustment, NULL) == PC_OK);
            compare(&f, &doc, pc_document_bounds(&doc));
        }
        pc_document_fini(&doc);
    }
    for (size_t i = 0; i < 25; i++) {
        pc_adjustment_fini(&cases[i]);
    }
    finish(&f);
}

static void test_gpu_colour_modes_and_vector_mask(void)
{
    fixture f = {0};
    if (!start(&f)) {
        return;
    }
    for (pc_color_mode mode = PC_COLOR_MODE_BITMAP; mode < PC_COLOR_MODE_COUNT; mode++) {
        if (mode == PC_COLOR_MODE_MULTICHANNEL) {
            continue;
        }
        pc_document doc;
        CHECK(pc_document_init(&doc, "modes", pc_size_make(17, 13), mode, PC_SAMPLE_TYPE_U16, NULL) == PC_OK);
        doc.layers = pc_calloc_array(2, sizeof *doc.layers);
        CHECK(doc.layers != NULL);
        doc.nlayers = 2;
        pc_pixel_format fmt = pc_document_pixel_format(&doc);
        for (size_t i = 0; i < 2; i++) {
            CHECK(pc_layer_raster(&doc.layers[i], "pixels", fmt, NULL) == PC_OK);
            size_t channels = pc_surface_channels(pc_layer_surface_mut(&doc.layers[i]));
            float pixel[5] = {.2f + .3f * (float)i, .4f, .7f, .1f, .6f};
            pixel[channels - 1] = .6f;
            CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[i]), pc_document_bounds(&doc), pixel,
                                       channels, NULL) == PC_OK);
        }
        compare(&f, &doc, pc_document_bounds(&doc));
        doc.layers[1].blend = PC_BLEND_SCREEN;
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_path path = {0};
        path.subpaths = pc_calloc_array(1, sizeof *path.subpaths);
        path.nsubpaths = 1;
        const double points[3][2] = {{2, 1}, {15, 3}, {8, 12}};
        CHECK(pc_subpath_polygon(points, 3, path.subpaths, NULL) == PC_OK);
        pc_vector_mask_init(&doc.layers[1].vector_mask, &path);
        doc.layers[1].has_vector_mask = true;
        doc.layers[1].vector_mask.density = .7f;
        doc.layers[1].vector_mask.feather = .8f;
        doc.layers[1].clipped = true;
        doc.layers[1].blend = PC_BLEND_NORMAL;
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_document_fini(&doc);
    }
    finish(&f);
}
static void test_gpu_stroked_shape_clipping(void)
{
    fixture f = {0};
    if (!start(&f)) {
        return;
    }
    pc_document doc;
    CHECK(pc_document_init(&doc, "shape", pc_size_make(32, 24), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16,
                           NULL) == PC_OK);
    doc.layers = pc_calloc_array(2, sizeof *doc.layers);
    CHECK(doc.layers != NULL);
    doc.nlayers = 2;
    pc_layer_content content = {.kind = PC_LAYER_SHAPE};
    pc_shape_layer *shape = &content.u.shape;
    shape->path.subpaths = pc_calloc_array(1, sizeof *shape->path.subpaths);
    CHECK(shape->path.subpaths != NULL);
    shape->path.nsubpaths = 1;
    const double points[3][2] = {{5, 3}, {27, 6}, {17, 21}};
    CHECK(pc_subpath_polygon(points, 3, shape->path.subpaths, NULL) == PC_OK);
    shape->has_fill = shape->has_stroke = true;
    shape->fill = pc_fill_solid(pc_color_rgb(.7f, .2f, .1f));
    pc_shape_stroke_default(&shape->stroke);
    shape->stroke.width = 3;
    CHECK(pc_vec_render_shape(shape, pc_document_pixel_format(&doc), pc_document_bounds(&doc), &shape->cache,
                              NULL) == PC_OK);
    shape->has_cache = true;
    CHECK(pc_layer_init(&doc.layers[0], "shape", &content, NULL) == PC_OK);
    CHECK(pc_layer_raster(&doc.layers[1], "clip", pc_document_pixel_format(&doc), NULL) == PC_OK);
    const float blue[4] = {.1f, .3f, .9f, .8f};
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[1]), pc_document_bounds(&doc), blue, 4,
                               NULL) == PC_OK);
    doc.layers[1].clipped = true;
    doc.layers[0].opacity = .7f;
    compare(&f, &doc, pc_document_bounds(&doc));
    doc.layers[0].has_mask = true;
    pc_layer_mask_reveal_all(&doc.layers[0].mask);
    const float half[1] = {.3f};
    CHECK(pc_surface_fill_rect(&doc.layers[0].mask.surface, pc_rect_make(0, 0, 16, 24), half, 1, NULL) ==
          PC_OK);
    compare(&f, &doc, pc_document_bounds(&doc));
    pc_document_fini(&doc);
    finish(&f);
}

static unsigned map_case;
static void compare_map(pc_gpu_fx_context *context, const pc_gpu_fx_output *output, size_t map,
                        const pc_fxm *cpu)
{
    float *pixels = NULL;
    size_t length = 0;
    CHECK(pc_gpu_fx_readback(context, output, map, &pixels, &length, NULL) == PC_OK);
    CHECK(length == cpu->w * cpu->h);
    float worst = 0;
    size_t index = 0;
    bool finite = true;
    for (size_t i = 0; pixels && i < length; i++) {
        finite = finite && isfinite(pixels[i]);
        float difference = fabsf(pixels[i] - cpu->v[i]);
        if (difference > worst) {
            worst = difference;
            index = i;
        }
    }
    CHECK(finite);
    CHECK_NEAR(worst, 0, 2e-4);
    if (pixels && worst > 2e-4) {
        PC_T_FAIL("effect case %u at %zu: GPU=%g CPU=%g", map_case, index, (double)pixels[index],
                  (double)cpu->v[index]);
    }
    free(pixels);
}
static void compare_effect_window(pc_gpu_fx_context *context, const pc_gpu_fx_program *program,
                                  pc_rect region, const float *shape, size_t length,
                                  const pc_gpu_fx_output *full)
{
    pc_rect area = pc_rect_make(region.x0 + 29, region.y0 + 19, region.x0 + 35, region.y0 + 25);
    pc_gpu_fx_output window = {0};
    uint64_t before = pc_gpu_fx_fragments(context), hits = pc_gpu_fx_cache_hits(context);
    CHECK(pc_gpu_fx_render_window(context, program, region, area, shape, length, PC_COLOR_MODE_RGB, &window,
                                  NULL) == PC_OK);
    CHECK(pc_rect_eq(window.valid, area));
    CHECK(window.textures[0] != full->textures[0]);
    CHECK(!program->count || pc_gpu_fx_fragments(context) > before || pc_gpu_fx_cache_hits(context) > hits);
    CHECK(pc_gpu_fx_fragments(context) - before <= length * program->count);
    if (map_case == 0)
        CHECK(pc_gpu_fx_fragments(context) - before < length * program->count);
    for (size_t map = 0; map < window.count; map++) {
        float *expected = NULL, *actual = NULL;
        size_t n = 0, m = 0;
        CHECK(pc_gpu_fx_readback(context, full, map, &expected, &n, NULL) == PC_OK);
        CHECK(pc_gpu_fx_readback(context, &window, map, &actual, &m, NULL) == PC_OK);
        CHECK(n == m);
        float worst = 0;
        for (int32_t y = area.y0; y < area.y1; y++) {
            for (int32_t x = area.x0; x < area.x1; x++) {
                size_t i = (size_t)(y - region.y0) * pc_rect_width(region) + (size_t)(x - region.x0);
                if (expected && actual && i < n)
                    worst = fmaxf(worst, fabsf(expected[i] - actual[i]));
            }
        }
        CHECK_NEAR(worst, 0, 1e-7);
        free(expected);
        free(actual);
    }
    uint64_t fragments = pc_gpu_fx_fragments(context);
    CHECK(pc_gpu_fx_render_window(context, program, region, area, shape, length, PC_COLOR_MODE_RGB, &window,
                                  NULL) == PC_OK);
    CHECK(pc_gpu_fx_fragments(context) == fragments);
    /* A new area cannot reuse pixels left unwritten in the first window. */
    hits = pc_gpu_fx_cache_hits(context);
    CHECK(pc_gpu_fx_render_window(context, program, region,
                                  pc_rect_make(region.x0, region.y0, region.x0 + 1, region.y0 + 1), shape,
                                  length, PC_COLOR_MODE_RGB, &window, NULL) == PC_OK);
    CHECK(!program->count || pc_gpu_fx_fragments(context) > fragments ||
          pc_gpu_fx_cache_hits(context) > hits);
    pc_gpu_fx_output_fini(context, &window);
}

static void test_gpu_effect_maps(void)
{
    fixture f = {0};
    if (!start(&f)) {
        return;
    }
    pc_gpu_fx_context *context = NULL;
    CHECK(pc_gpu_fx_context_new(&f.gl, &context, NULL) == PC_OK);
    pc_fxm shape = {0};
    CHECK(pc_fxm_new(64, 48, 0, &shape, NULL) == PC_OK);
    for (size_t y = 0; y < 48; y++) {
        for (size_t x = 0; x < 64; x++) {
            float distance = hypotf((float)x - 32, (float)y - 24);
            shape.v[y * 64 + x] = fminf(1, fmaxf(0, (18 - distance) * .8f));
            if (x > 30 && x < 39 && y > 19 && y < 27) {
                shape.v[y * 64 + x] = .4f;
            }
        }
    }
    pc_rect region = pc_rect_make(-23, 17, 41, 65);
    const double anchor[2] = {0};
    pc_global_light light = pc_global_light_default();
    for (unsigned variant = 0; variant < 13; variant++) {
        map_case = variant;
        pc_effect effect = {0};
        if (variant < 2) {
            pc_effect_default_drop_shadow(&effect);
            effect.kind = variant ? PC_FX_INNER_SHADOW : PC_FX_DROP_SHADOW;
            effect.u.shadow.size = 8;
            effect.u.shadow.distance = 5;
            effect.u.shadow.spread = .4f;
            effect.u.shadow.noise = 0;
        } else if (variant < 10) {
            effect.kind = variant < 6 ? PC_FX_OUTER_GLOW : PC_FX_INNER_GLOW;
            effect.u.glow.size = 8;
            effect.u.glow.spread = .2f;
            effect.u.glow.range = .5f;
            effect.u.glow.technique = variant & 1 ? PC_GLOW_PRECISE : PC_GLOW_SOFTER;
            effect.u.glow.source = variant & 2 ? PC_GLOW_SOURCE_CENTER : PC_GLOW_SOURCE_EDGE;
            effect.u.glow.noise = 0;
        } else {
            effect.kind = PC_FX_SATIN;
            effect.u.satin.size = 7;
            effect.u.satin.distance = 4;
            effect.u.satin.angle = 35;
            effect.u.satin.invert = variant & 1;
        }
        pc_gpu_fx_program program = {0};
        CHECK(pc_gpu_fx_program_build(&effect, light, false, NULL, 0, anchor, &program, NULL) == PC_OK);
        pc_gpu_fx_output output = {0};
        pc_error err;
        pc_status status =
            pc_gpu_fx_render(context, &program, region, shape.v, 64 * 48, PC_COLOR_MODE_RGB, &output, &err);
        CHECK(status == PC_OK);
        if (status != PC_OK) {
            PC_T_FAIL("effect %u: %s", variant, err.msg);
        }
        if (status == PC_OK) {
            uint64_t builds = pc_gpu_fx_map_builds(context), hits = pc_gpu_fx_cache_hits(context);
            GLuint texture = output.textures[0];
            CHECK(pc_gpu_fx_render(context, &program, region, shape.v, 64 * 48, PC_COLOR_MODE_RGB, &output,
                                   NULL) == PC_OK);
            CHECK(output.textures[0] == texture);
            CHECK(pc_gpu_fx_map_builds(context) == builds);
            CHECK(pc_gpu_fx_cache_hits(context) == hits + 1);
        }
        pc_fxm cpu = {0};
        if (variant < 2) {
            CHECK(pc_fx_shadow_map(&shape, &effect.u.shadow, &light, variant != 0, region.x0, region.y0, &cpu,
                                   NULL) == PC_OK);
        } else if (variant < 10) {
            CHECK(pc_fx_glow_map(&shape, &effect.u.glow, effect.kind == PC_FX_INNER_GLOW, region.x0,
                                 region.y0, &cpu, NULL) == PC_OK);
        } else {
            CHECK(pc_fx_satin_map(&shape, &effect.u.satin, &cpu, NULL) == PC_OK);
        }
        if (status == PC_OK) {
            compare_map(context, &output, 0, &cpu);
            compare_effect_window(context, &program, region, shape.v, 64 * 48, &output);
        }
        pc_fxm_free(&cpu);
        pc_gpu_fx_output_fini(context, &output);
        pc_gpu_fx_program_fini(&program);
        pc_effect_fini(&effect);
    }
    for (pc_bevel_style style = PC_BEVEL_OUTER; style <= PC_BEVEL_STROKE_EMBOSS; style++) {
        for (pc_bevel_technique technique = PC_BEVEL_SMOOTH; technique <= PC_BEVEL_CHISEL_SOFT; technique++) {
            map_case = 100u + (unsigned)style * 3 + (unsigned)technique;
            pc_effect effect = {.kind = PC_FX_BEVEL_EMBOSS};
            pc_bevel *bevel = &effect.u.bevel;
            bevel->size = 5;
            bevel->depth = 1;
            bevel->up = true;
            bevel->soften = 2;
            bevel->style = style;
            bevel->technique = technique;
            bevel->use_global_light = true;
            pc_gpu_fx_program program = {0};
            CHECK(pc_gpu_fx_program_build(&effect, light, false, NULL, 0, anchor, &program, NULL) == PC_OK);
            pc_gpu_fx_output output = {0};
            pc_error err;
            pc_status status = pc_gpu_fx_render(context, &program, region, shape.v, 64 * 48,
                                                PC_COLOR_MODE_RGB, &output, &err);
            CHECK(status == PC_OK);
            if (status != PC_OK) {
                PC_T_FAIL("bevel %d/%d: %s", (int)style, (int)technique, err.msg);
            }
            pc_fxm cpu[2] = {{0}, {0}};
            pc_compose_bevel_paint paint;
            pc_compose_texture_ctx texture = {.rect = region};
            CHECK(pc_fx_bevel_maps(&shape, bevel, &light, &texture, NULL, cpu, &paint, NULL) == PC_OK);
            if (status == PC_OK) {
                compare_map(context, &output, 0, &cpu[0]);
                compare_map(context, &output, 1, &cpu[1]);
            }
            pc_fxm_free(&cpu[0]);
            pc_fxm_free(&cpu[1]);
            pc_gpu_fx_output_fini(context, &output);
            pc_gpu_fx_program_fini(&program);
            pc_effect_fini(&effect);
        }
    }
    pc_fxm_free(&shape);
    pc_gpu_fx_context_free(context);
    finish(&f);
}

static void test_gpu_effect_composition(void)
{
    fixture f = {0};
    if (!start(&f)) {
        return;
    }
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "effects", pc_size_make(96, 64), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(.2f, .4f, .6f), NULL) == PC_OK);
    pc_layer *grown = realloc(doc.layers, 2 * sizeof *doc.layers);
    CHECK(grown != NULL);
    if (!grown) {
        pc_document_fini(&doc);
        finish(&f);
        return;
    }
    doc.layers = grown;
    doc.nlayers = 2;
    pc_layer *layer = &doc.layers[1];
    CHECK(pc_layer_raster(layer, "styled", PC_PIXEL_FORMAT_RGBA32F, NULL) == PC_OK);
    for (int y = 15; y < 46; y++) {
        for (int x = 20; x < 70; x++) {
            float alpha = x == 20 || y == 15 ? .35f : .8f;
            float pixel[4] = {.8f, .15f, .25f, alpha};
            CHECK(pc_surface_write_pixel(pc_layer_surface_mut(layer), x, y, pixel, 4, NULL) == PC_OK);
        }
    }
    layer->effects.items = calloc(1, sizeof *layer->effects.items);
    CHECK(layer->effects.items != NULL);
    layer->effects.nitems = 1;
    for (pc_effect_kind kind = PC_FX_DROP_SHADOW; kind < PC_FX_KIND_COUNT; kind++) {
        pc_effect *e = layer->effects.items;
        *e = (pc_effect){.kind = kind};
        if (kind == PC_FX_DROP_SHADOW || kind == PC_FX_INNER_SHADOW) {
            pc_effect_default_drop_shadow(e);
            e->kind = kind;
            e->u.shadow.size = 6;
            e->u.shadow.distance = 4;
            e->u.shadow.spread = .3f;
        } else if (kind == PC_FX_OUTER_GLOW || kind == PC_FX_INNER_GLOW) {
            e->u.glow.common = pc_fx_common_new(PC_BLEND_SCREEN, .7f);
            e->u.glow.paint.u.color = pc_color_rgb(.9f, .8f, .1f);
            e->u.glow.size = 5;
            e->u.glow.range = .5f;
            e->u.glow.spread = .2f;
        } else if (kind == PC_FX_STROKE) {
            e->u.stroke.common = pc_fx_common_new(PC_BLEND_MULTIPLY, .7f);
            e->u.stroke.size = 5;
            e->u.stroke.position = PC_STROKE_CENTER;
            e->u.stroke.paint.u.color = pc_color_rgb(.1f, .8f, .3f);
        } else if (kind == PC_FX_COLOR_OVERLAY) {
            e->u.color_overlay.common = pc_fx_common_new(PC_BLEND_OVERLAY, .7f);
            e->u.color_overlay.color = pc_color_rgb(.1f, .8f, .3f);
        } else if (kind == PC_FX_GRADIENT_OVERLAY) {
            e->u.gradient_overlay.common = pc_fx_common_new(PC_BLEND_MULTIPLY, .8f);
            CHECK(pc_gradient_default(&e->u.gradient_overlay.gradient, NULL) == PC_OK);
            e->u.gradient_overlay.gradient.angle = 35;
        } else if (kind == PC_FX_PATTERN_OVERLAY) {
            e->u.pattern_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, .7f);
            e->u.pattern_overlay.scale = 1; // missing pattern is transparent
        } else if (kind == PC_FX_SATIN) {
            e->u.satin.common = pc_fx_common_new(PC_BLEND_MULTIPLY, .7f);
            e->u.satin.color = pc_color_rgb(.2f, .1f, .6f);
            e->u.satin.size = 6;
            e->u.satin.distance = 4;
            e->u.satin.angle = 35;
        } else if (kind == PC_FX_BEVEL_EMBOSS) {
            e->u.bevel.highlight = pc_fx_common_new(PC_BLEND_SCREEN, .7f);
            e->u.bevel.shadow = pc_fx_common_new(PC_BLEND_MULTIPLY, .7f);
            e->u.bevel.highlight_color = pc_color_rgb(1, 1, 1);
            e->u.bevel.shadow_color = pc_color_rgb(0, 0, 0);
            e->u.bevel.size = 5;
            e->u.bevel.depth = 1;
            e->u.bevel.up = true;
            e->u.bevel.style = PC_BEVEL_INNER;
            e->u.bevel.use_global_light = true;
        }
        for (unsigned variant = 0; variant < 4; variant++) {
            layer->fill_opacity = variant & 1 ? .3f : 1;
            layer->opacity = variant & 2 ? .6f : 1;
            layer->blend = variant & 2 ? PC_BLEND_COLOR : PC_BLEND_NORMAL;
            fprintf(stderr, "effect composition %d/%u\n", (int)kind, variant);
            compare(&f, &doc, pc_rect_make(-3, -5, 105, 73));
        }
        if (kind == PC_FX_STROKE) {
            for (pc_stroke_position position = PC_STROKE_OUTSIDE; position <= PC_STROKE_CENTER; position++) {
                e->u.stroke.position = position;
                compare(&f, &doc, pc_document_bounds(&doc));
            }
            e->u.stroke.paint.kind = PC_FX_PAINT_GRADIENT;
            CHECK(pc_gradient_default(&e->u.stroke.paint.u.gradient, NULL) == PC_OK);
            compare(&f, &doc, pc_document_bounds(&doc));
        }
        if (kind == PC_FX_BEVEL_EMBOSS) {
            for (pc_bevel_style style = PC_BEVEL_OUTER; style <= PC_BEVEL_STROKE_EMBOSS; style++) {
                for (pc_bevel_technique technique = PC_BEVEL_SMOOTH; technique <= PC_BEVEL_CHISEL_SOFT;
                     technique++) {
                    e->u.bevel.style = style;
                    e->u.bevel.technique = technique;
                    fprintf(stderr, "bevel composition %d/%d\n", (int)style, (int)technique);
                    compare(&f, &doc, pc_document_bounds(&doc));
                }
            }
        }
        if (kind == PC_FX_OUTER_GLOW || kind == PC_FX_INNER_GLOW) {
            e->u.glow.paint.kind = PC_FX_PAINT_GRADIENT;
            CHECK(pc_gradient_default(&e->u.glow.paint.u.gradient, NULL) == PC_OK);
            compare(&f, &doc, pc_document_bounds(&doc));
            e->u.glow.technique = PC_GLOW_PRECISE;
            compare(&f, &doc, pc_document_bounds(&doc));
        }
        layer->has_mask = true;
        pc_layer_mask_reveal_all(&layer->mask);
        const float alpha[1] = {.4f};
        CHECK(pc_surface_fill_rect(&layer->mask.surface, pc_rect_make(40, 25, 65, 45), alpha, 1, NULL) ==
              PC_OK);
        layer->mask.feather = 1.5f;
        compare(&f, &doc, pc_document_bounds(&doc));
        compare(&f, &doc, pc_rect_make(42, 27, 48, 33));
        pc_layer_mask_fini(&layer->mask);
        layer->has_mask = false;
        layer->clipped = true;
        compare(&f, &doc, pc_document_bounds(&doc));
        layer->clipped = false;
        pc_effect_fini(e);
    }
    pc_document_fini(&doc);
    finish(&f);
}

static void test_gpu_prefix_reuse(void)
{
    fixture f = {0};
    if (!start(&f)) {
        return;
    }
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "prefix", pc_size_make(530, 270), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(.2f, .4f, .6f), NULL) == PC_OK);
    pc_layer *grown = realloc(doc.layers, 3 * sizeof *doc.layers);
    CHECK(grown != NULL);
    if (!grown) {
        pc_document_fini(&doc);
        finish(&f);
        return;
    }
    doc.layers = grown;
    doc.nlayers = 3;
    for (size_t i = 1; i < 3; i++) {
        CHECK(pc_layer_raster(&doc.layers[i], "layer", PC_PIXEL_FORMAT_RGBA32F, NULL) == PC_OK);
        float pixel[4] = {.2f * (float)i, .2f, .8f, .4f};
        CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[i]), pc_rect_make(10, 10, 520, 260),
                                   pixel, 4, NULL) == PC_OK);
    }
    pc_rect area = pc_rect_make(-3, -5, 537, 277);
    compare(&f, &doc, area);
    CHECK(pc_gpu_compositor_prefix_hits(f.gpu) == 0);
    CHECK(pc_gpu_compositor_prefix_bytes(f.gpu) == 12 * 256 * 256 * 16);
    compare(&f, &doc, area);
    CHECK(pc_gpu_compositor_prefix_hits(f.gpu) == 12);
    const float red[4] = {1, 0, 0, 1};
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[2]), 300, 20, red, 4, NULL) == PC_OK);
    compare(&f, &doc, pc_rect_make(295, 10, 315, 30));
    CHECK(pc_gpu_compositor_prefix_hits(f.gpu) == 13);
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[0]), 300, 20, red, 4, NULL) == PC_OK);
    compare(&f, &doc, pc_rect_make(295, 10, 315, 30));
    CHECK(pc_gpu_compositor_prefix_hits(f.gpu) == 13);
    CHECK(pc_gpu_compositor_prefix_bytes(f.gpu) == 0);
    doc.resolution_dpi = 144;
    compare(&f, &doc, pc_rect_make(300, 20, 315, 30));
    CHECK(pc_gpu_compositor_prefix_bytes(f.gpu) == 256 * 256 * 16);
    compare(&f, &doc, pc_rect_make(302, 22, 310, 28));
    CHECK(pc_gpu_compositor_prefix_hits(f.gpu) == 14);
    // A hole in a cell must not be treated as cached.
    compare(&f, &doc, pc_rect_make(400, 22, 410, 28));
    CHECK(pc_gpu_compositor_prefix_hits(f.gpu) == 14);
    pc_document_fini(&doc);
    finish(&f);
}

static void test_gpu_resident_sources(void)
{
    fixture f = {0};
    if (!start(&f))
        return;
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "resident", pc_size_make(520, 270), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(.2f, .4f, .6f), NULL) == PC_OK);
    pc_rect area = pc_rect_make(-3, -5, 527, 277);
    compare(&f, &doc, area);
    uint64_t uploads = pc_gpu_compositor_source_uploads(f.gpu);
    CHECK(uploads > 0);
    uint64_t hits = pc_gpu_compositor_source_hits(f.gpu);
    doc.layers[0].opacity = .7f;
    compare(&f, &doc, area);
    CHECK(pc_gpu_compositor_source_hits(f.gpu) > hits);
    CHECK(pc_gpu_compositor_source_uploads(f.gpu) == uploads);
    const float red[4] = {1, 0, 0, .8f};
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[0]), 300, 20, red, 4, NULL) == PC_OK);
    doc.layers[0].opacity = .8f;
    compare(&f, &doc, area);
    CHECK(pc_gpu_compositor_source_uploads(f.gpu) == uploads + 1);
    uploads = pc_gpu_compositor_source_uploads(f.gpu);
    pc_document snapshot;
    CHECK(pc_document_clone(&doc, &snapshot, NULL) == PC_OK);
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[0]), 300, 20, red, 4, NULL) == PC_OK);
    doc.layers[0].opacity = .9f;
    compare(&f, &doc, area);
    CHECK(pc_gpu_compositor_source_uploads(f.gpu) == uploads + 1);
    /* An undo snapshot still owns the previous immutable source tile. */
    uploads = pc_gpu_compositor_source_uploads(f.gpu);
    compare(&f, &snapshot, area);
    CHECK(pc_gpu_compositor_source_uploads(f.gpu) == uploads);
    /* A different surface default must not reuse an absent tile's pixels. */
    pc_surface *surface = pc_layer_surface_mut(&doc.layers[0]);
    pc_surface_fini(surface);
    pc_surface_init_default(surface, PC_PIXEL_FORMAT_RGBA32F, red, 4);
    compare(&f, &doc, area);
    CHECK(pc_gpu_compositor_source_uploads(f.gpu) > uploads);
    CHECK(pc_gpu_compositor_source_bytes(f.gpu) <= UINT64_C(128) * 256 * 256 * 16);
    pc_document_fini(&snapshot);
    pc_document_fini(&doc);
    finish(&f);
}

static void test_gpu_effect_damage_region_execution(void)
{
    fixture f = {0};
    if (!start(&f))
        return;
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "effect windows", pc_size_make(800, 400), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(0, 0, 0), NULL) == PC_OK);
    pc_layer layer;
    CHECK(pc_layer_raster(&layer, "shape", PC_PIXEL_FORMAT_RGBA32F, NULL) == PC_OK);
    const float pixel[4] = {.8f, .3f, .2f, .7f};
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&layer), pc_rect_make(100, 80, 700, 320), pixel, 4,
                               NULL) == PC_OK);
    layer.effects.items = calloc(1, sizeof *layer.effects.items);
    CHECK(layer.effects.items != NULL);
    layer.effects.nitems = 1;
    pc_effect_default_drop_shadow(layer.effects.items);
    layer.effects.items[0].u.shadow.size = 4;
    layer.effects.items[0].u.shadow.distance = 7;
    pc_layer *grown = realloc(doc.layers, 2 * sizeof *doc.layers);
    CHECK(grown != NULL);
    if (!grown) {
        pc_layer_fini(&layer);
        pc_document_fini(&doc);
        finish(&f);
        return;
    }
    doc.layers = grown;
    doc.layers[1] = layer;
    doc.nlayers = 2;
    compare(&f, &doc, pc_document_bounds(&doc));
    uint64_t full = pc_gpu_compositor_effect_fragments(f.gpu);
    CHECK(full > 0);
    pc_rect area = pc_rect_make(295, 95, 310, 115);
    const float hole[4] = {0, 0, 0, 0};
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[1]), 300, 100, hole, 4, NULL) == PC_OK);
    compare(&f, &doc, area);
    uint64_t fragments = pc_gpu_compositor_effect_fragments(f.gpu) - full;
    CHECK(fragments > 0 && fragments < full / 4);
    uint64_t builds = pc_gpu_compositor_effect_builds(f.gpu);
    compare(&f, &doc, area);
    CHECK(pc_gpu_compositor_effect_builds(f.gpu) == builds);
    CHECK(pc_gpu_compositor_effect_hits(f.gpu) > 0);
    pc_document_fini(&doc);
    finish(&f);
}

static void test_gpu_sparse_texture_limit_and_cache_eviction(void)
{
    fixture f = {0};
    if (!start(&f))
        return;
    pc_document doc = {0};
    CHECK(pc_document_init(&doc, "Sparse beyond device texture limits", pc_size_make(100000, 32),
                           PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, NULL) == PC_OK);
    doc.layers = pc_calloc_array(1, sizeof *doc.layers);
    CHECK(doc.layers != NULL);
    doc.nlayers = 1;
    CHECK(pc_layer_raster(doc.layers, "default", PC_PIXEL_FORMAT_RGBA32F, NULL) == PC_OK);
    const float color[4] = {.2f, .4f, .7f, .8f};
    pc_surface *surface = pc_layer_surface_mut(doc.layers);
    pc_surface_fini(surface);
    pc_surface_init_default(surface, PC_PIXEL_FORMAT_RGBA32F, color, 4);
    CHECK(pc_surface_write_pixel(surface, 0, 0, color, 4, NULL) == PC_OK);
    CHECK(pc_surface_write_pixel(surface, 99999, 31, color, 4, NULL) == PC_OK);
    compare(&f, &doc, pc_rect_make(85000, 0, 85257, 32));
    for (int32_t tile = 0; tile < 140; tile++) {
        int32_t x = tile * 256;
        doc.layers[0].opacity = (tile & 1) ? .8f : .9f;
        compare(&f, &doc, pc_rect_make(x, 0, x + 256, 1));
        CHECK(pc_gpu_compositor_source_bytes(f.gpu) <= 128u * 1024u * 1024u);
    }
    uint64_t uploads = pc_gpu_compositor_source_uploads(f.gpu);
    doc.layers[0].opacity = .51f;
    compare(&f, &doc, pc_rect_make(0, 0, 256, 1));
    CHECK(pc_gpu_compositor_source_uploads(f.gpu) > uploads);
    uint64_t hits = pc_gpu_compositor_source_hits(f.gpu);
    doc.layers[0].opacity = .52f;
    compare(&f, &doc, pc_rect_make(0, 0, 256, 1));
    CHECK(pc_gpu_compositor_source_hits(f.gpu) > hits);
    doc.layers[0].has_mask = true;
    pc_layer_mask_reveal_all(&doc.layers[0].mask);
    const float half[1] = {.25f};
    CHECK(pc_surface_write_pixel(&doc.layers[0].mask.surface, 85012, 8, half, 1, NULL) == PC_OK);
    compare(&f, &doc, pc_rect_make(85000, 0, 85257, 32));
    pc_surface_fini(&doc.layers[0].mask.surface);
    pc_surface_init_default(&doc.layers[0].mask.surface, PC_PIXEL_FORMAT_GRAY8, half, 1);
    compare(&f, &doc, pc_rect_make(85000, 0, 85257, 32));
    pc_document_fini(&doc);
    finish(&f);
}
static void test_gpu_hdr_blends_and_hard_stop_effects(void)
{
    fixture f = {0};
    if (!start(&f))
        return;
    pc_document doc = {0};
    CHECK(pc_document_init(&doc, "HDR", pc_size_make(32, 16), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, NULL) ==
          PC_OK);
    doc.layers = pc_calloc_array(2, sizeof *doc.layers);
    CHECK(doc.layers != NULL);
    doc.nlayers = 2;
    const float pixels[2][4] = {{-.3f, 1.4f, 3.2f, .7f}, {1.8f, -.2f, .4f, .6f}};
    for (size_t i = 0; i < 2; i++) {
        CHECK(pc_layer_raster(&doc.layers[i], "HDR pixels", PC_PIXEL_FORMAT_RGBA32F, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[i]), pc_document_bounds(&doc), pixels[i],
                                   4, NULL) == PC_OK);
    }
    for (pc_blend_mode mode = PC_BLEND_NORMAL; mode < PC_BLEND_COUNT; mode++) {
        doc.layers[1].blend = mode;
        compare(&f, &doc, pc_document_bounds(&doc));
    }
    doc.layers[1].blend = PC_BLEND_NORMAL;
    doc.layers[1].effects.items = pc_calloc_array(1, sizeof *doc.layers[1].effects.items);
    CHECK(doc.layers[1].effects.items != NULL);
    doc.layers[1].effects.nitems = 1;
    doc.layers[1].effects.enabled = true;
    pc_effect *effect = doc.layers[1].effects.items;
    effect->kind = PC_FX_GRADIENT_OVERLAY;
    effect->u.gradient_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, .8f);
    pc_gradient *gradient = &effect->u.gradient_overlay.gradient;
    CHECK(pc_gradient_default(gradient, NULL) == PC_OK);
    free(gradient->stops);
    gradient->stops = pc_calloc_array(4, sizeof *gradient->stops);
    CHECK(gradient->stops != NULL);
    gradient->nstops = 4;
    gradient->stops[0] = (pc_color_stop){0, pc_color_rgb(1, 0, 0)};
    gradient->stops[1] = (pc_color_stop){.5f, pc_color_rgb(1, 0, 0)};
    gradient->stops[2] = (pc_color_stop){.5f, pc_color_rgb(0, 1, 0)};
    gradient->stops[3] = (pc_color_stop){1, pc_color_rgb(0, 0, 1)};
    gradient->angle = 37;
    for (pc_gradient_style style = PC_GRADIENT_LINEAR; style <= PC_GRADIENT_DIAMOND; style++) {
        gradient->style = style;
        for (unsigned reverse = 0; reverse < 2; reverse++) {
            gradient->reverse = reverse != 0;
            compare(&f, &doc, pc_document_bounds(&doc));
            compare(&f, &doc, pc_rect_make(10, 4, 17, 11));
        }
    }
    pc_document_fini(&doc);
    finish(&f);
}

static double gpu_bench_render(fixture *f, const pc_document *doc, pc_rect rect)
{
    pc_compose_buffer pixels = {0};
    uint64_t before = SDL_GetPerformanceCounter();
    CHECK(pc_gpu_render(f->gpu, doc, rect, &pixels, NULL) == PC_OK);
    uint64_t after = SDL_GetPerformanceCounter();
    pc_compose_buffer_fini(&pixels);
    return (double)(after - before) * 1000 / (double)SDL_GetPerformanceFrequency();
}
/* Reference checkpoint regressions: every depth, exact cold/warm accumulator, COW undo,
 * complete clipping/group units, context changes and disjoint partial coverage. */
static void exact_cold(fixture *f, const pc_document *doc, pc_rect rect)
{
    pc_gpu_compositor *cold = NULL;
    CHECK(pc_gpu_compositor_new(&f->gl, &cold, NULL) == PC_OK);
    pc_compose_buffer warm = {0}, fresh = {0};
    CHECK(pc_gpu_render(f->gpu, doc, rect, &warm, NULL) == PC_OK);
    CHECK(pc_gpu_render(cold, doc, rect, &fresh, NULL) == PC_OK);
    CHECK(warm.len == fresh.len);
    if (warm.len == fresh.len)
        CHECK(!memcmp(warm.px, fresh.px, warm.len * sizeof *warm.px));
    pc_compose_buffer_fini(&warm);
    pc_compose_buffer_fini(&fresh);
    pc_gpu_compositor_free(cold);
}
static void test_gpu_checkpoint_reference_matrix(void)
{
    fixture f = {0};
    if (!start(&f))
        return;
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t depth = 0; depth < 3; depth++) {
        pc_document doc;
        CHECK(pc_document_with_background(&doc, "checkpoints", pc_size_make(300, 70), PC_COLOR_MODE_RGB,
                                          depths[depth], pc_color_rgb(.2f, .4f, .6f), NULL) == PC_OK);
        pc_layer *grown = realloc(doc.layers, 4 * sizeof *doc.layers);
        CHECK(grown != NULL);
        if (!grown) {
            pc_document_fini(&doc);
            continue;
        }
        doc.layers = grown;
        doc.nlayers = 4;
        pc_pixel_format format = pc_document_pixel_format(&doc);
        for (size_t i = 1; i < 4; i++) {
            CHECK(pc_layer_raster(&doc.layers[i], "layer", format, NULL) == PC_OK);
            const float rgba[4] = {.2f * (float)i, .3f, .8f, .4f};
            float encoded[PC_RASTER_MAX_CHANNELS];
            size_t channels = pc_from_rgba(format, rgba, encoded);
            CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[i]), pc_rect_make(0, 0, 300, 70),
                                       encoded, channels, NULL) == PC_OK);
        }
        compare(&f, &doc, pc_document_bounds(&doc));
        for (unsigned variant = 0; variant < 4; variant++) {
            doc.layers[3].opacity = .3f + .2f * (float)variant;
            doc.layers[3].fill_opacity = variant & 1 ? .5f : 1;
            doc.layers[3].blend = variant & 1 ? PC_BLEND_MULTIPLY : PC_BLEND_NORMAL;
            doc.layers[3].visible = variant != 2;
            uint64_t hits = pc_gpu_compositor_prefix_hits(f.gpu);
            compare(&f, &doc, pc_document_bounds(&doc));
            CHECK(pc_gpu_compositor_prefix_hits(f.gpu) > hits);
            exact_cold(&f, &doc, pc_document_bounds(&doc));
        }
        pc_document old;
        CHECK(pc_document_clone(&doc, &old, NULL) == PC_OK);
        const float rgba[4] = {.8f, .2f, .6f, .7f};
        float encoded[PC_RASTER_MAX_CHANNELS];
        size_t channels = pc_from_rgba(format, rgba, encoded);
        CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[1]), 5, 5, encoded, channels, NULL) ==
              PC_OK);
        compare(&f, &doc, pc_document_bounds(&doc));
        doc.layers[1].opacity = .3f;
        compare(&f, &doc, pc_document_bounds(&doc));
        exact_cold(&f, &doc, pc_document_bounds(&doc));
        compare(&f, &old, pc_document_bounds(&old));
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_document_fini(&old);
        for (unsigned pass = 0; pass < 3; pass++) {
            doc.layers[3].visible = true;
            doc.layers[3].opacity = .4f + .1f * (float)pass;
            for (int i = 0; i < 12; i++)
                compare(&f, &doc, pc_rect_make(10 + 20 * i, 5, 18 + 20 * i, 20));
        }
        compare(&f, &doc, pc_document_bounds(&doc));
        exact_cold(&f, &doc, pc_document_bounds(&doc));
        doc.global_light.angle += 15;
        uint64_t hits = pc_gpu_compositor_prefix_hits(f.gpu);
        compare(&f, &doc, pc_document_bounds(&doc));
        CHECK(pc_gpu_compositor_prefix_hits(f.gpu) == hits);
        pc_layer_content saved = doc.layers[3].content;
        doc.layers[3].content =
            (pc_layer_content){.kind = PC_LAYER_FILL, .u.fill = pc_fill_solid(pc_color_rgb(1, 1, 1))};
        doc.layers[3].opacity = 1;
        doc.layers[3].fill_opacity = 1;
        doc.layers[3].blend = PC_BLEND_NORMAL;
        compare(&f, &doc, pc_document_bounds(&doc));
        exact_cold(&f, &doc, pc_document_bounds(&doc));
        doc.layers[3].opacity = .5f;
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_layer_content_fini(&doc.layers[3].content);
        doc.layers[3].content = saved;
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_layer swap = doc.layers[0];
        doc.layers[0] = doc.layers[1];
        doc.layers[1] = swap;
        compare(&f, &doc, pc_document_bounds(&doc));
        exact_cold(&f, &doc, pc_document_bounds(&doc));
        pc_layer_fini(&doc.layers[1]);
        memmove(doc.layers + 1, doc.layers + 2, 2 * sizeof *doc.layers);
        doc.nlayers--;
        compare(&f, &doc, pc_document_bounds(&doc));
        doc.size = pc_size_make(200, 60);
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_gpu_compositor_set_prefix_budget(f.gpu, 1024);
        CHECK(pc_gpu_compositor_prefix_bytes(f.gpu) == 0);
        uint64_t hits_before = pc_gpu_compositor_prefix_hits(f.gpu);
        compare(&f, &doc, pc_document_bounds(&doc));
        CHECK(pc_gpu_compositor_prefix_bytes(f.gpu) == 0 &&
              pc_gpu_compositor_prefix_hits(f.gpu) == hits_before);
        pc_gpu_compositor_set_prefix_budget(f.gpu, 2 * 1024 * 1024);
        compare(&f, &doc, pc_document_bounds(&doc));
        CHECK(pc_gpu_compositor_prefix_bytes(f.gpu) > 0 &&
              pc_gpu_compositor_prefix_bytes(f.gpu) <= 2 * 1024 * 1024);
        CHECK(pc_gpu_compositor_document(f.gpu) == doc.id);
        pc_gpu_compositor_forget_document(f.gpu, doc.id);
        CHECK(pc_gpu_compositor_document(f.gpu) == 0);
        CHECK(pc_gpu_compositor_prefix_bytes(f.gpu) == 0);
        compare(&f, &doc, pc_document_bounds(&doc));
        exact_cold(&f, &doc, pc_document_bounds(&doc));
        doc.layers[0].advanced.knockout = PC_KNOCKOUT_DEEP;
        pc_compose_buffer unsupported = {0};
        CHECK(pc_gpu_render(f.gpu, &doc, pc_document_bounds(&doc), &unsupported, NULL) == PC_ERR_UNSUPPORTED);
        CHECK(pc_gpu_compositor_prefix_bytes(f.gpu) == 0);
        pc_compose_buffer_fini(&unsupported);
        pc_gpu_compositor_set_prefix_budget(f.gpu, 128 * 1024 * 1024);
        pc_document_fini(&doc);
    }
    finish(&f);
}

static void test_gpu_group_checkpoint_effects_and_clipping(void)
{
    fixture f = {0};
    if (!start(&f))
        return;
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t depth = 0; depth < 3; depth++)
        for (unsigned isolated = 0; isolated < 2; isolated++) {
            pc_document doc;
            CHECK(pc_document_with_background(&doc, "group effects", pc_size_make(72, 48), PC_COLOR_MODE_RGB,
                                              depths[depth], pc_color_rgb(.2f, .4f, .6f), NULL) == PC_OK);
            pc_layer *grown = realloc(doc.layers, 4 * sizeof *doc.layers);
            CHECK(grown != NULL);
            if (!grown) {
                pc_document_fini(&doc);
                continue;
            }
            doc.layers = grown;
            doc.nlayers = 4;
            pc_layer *children = calloc(3, sizeof *children);
            CHECK(children != NULL);
            pc_pixel_format format = pc_document_pixel_format(&doc);
            for (size_t i = 0; i < 2; i++) {
                CHECK(pc_layer_raster(&children[i], "child", format, NULL) == PC_OK);
                const float rgba[4] = {.2f + .5f * (float)i, .3f, .7f, .7f};
                float encoded[PC_RASTER_MAX_CHANNELS];
                size_t channels = pc_from_rgba(format, rgba, encoded);
                CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&children[i]),
                                           pc_rect_make(-8 + (int32_t)i * 5, 10, 60, 35), encoded, channels,
                                           NULL) == PC_OK);
            }
            children[1].clipped = true;
            pc_layer_content adjustment = {.kind = PC_LAYER_ADJUSTMENT};
            adjustment.u.adjustment.kind = PC_ADJ_INVERT;
            CHECK(pc_layer_init(&children[2], "invert", &adjustment, NULL) == PC_OK);
            children[2].opacity = .2f;
            CHECK(pc_layer_group(&doc.layers[1], "group", children, 3, NULL) == PC_OK);
            pc_layer *group = &doc.layers[1];
            group->blend = isolated ? PC_BLEND_NORMAL : PC_BLEND_PASS_THROUGH;
            group->opacity = .8f;
            group->has_mask = true;
            pc_layer_mask_reveal_all(&group->mask);
            const float half[1] = {.4f};
            CHECK(pc_surface_fill_rect(&group->mask.surface, pc_rect_make(0, 0, 30, 48), half, 1, NULL) ==
                  PC_OK);
            for (size_t i = 2; i < 4; i++) {
                CHECK(pc_layer_raster(&doc.layers[i], "top", format, NULL) == PC_OK);
                const float rgba[4] = {.8f, .2f, .1f, .6f};
                float encoded[PC_RASTER_MAX_CHANNELS];
                size_t channels = pc_from_rgba(format, rgba, encoded);
                CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[i]), pc_rect_make(2, 3, 64, 42),
                                           encoded, channels, NULL) == PC_OK);
            }
            doc.layers[2].clipped = true;
            compare(&f, &doc, pc_document_bounds(&doc));
            doc.layers[3].opacity = .4f;
            compare(&f, &doc, pc_document_bounds(&doc));
            exact_cold(&f, &doc, pc_document_bounds(&doc));
            doc.layers[2].opacity = .3f;
            compare(&f, &doc, pc_document_bounds(&doc));
            exact_cold(&f, &doc, pc_document_bounds(&doc));
            group->mask.density = .3f;
            compare(&f, &doc, pc_document_bounds(&doc));
            children[2].content.u.adjustment.kind = PC_ADJ_EXPOSURE;
            children[2].content.u.adjustment.u.exposure.exposure = .2f;
            children[2].content.u.adjustment.u.exposure.gamma = 1;
            compare(&f, &doc, pc_document_bounds(&doc));
            /* Styled groups and their clipped siblings share exactly the same checkpoint unit. */
            group->effects.items = calloc(2, sizeof *group->effects.items);
            CHECK(group->effects.items != NULL);
            group->effects.nitems = 2;
            pc_effect_default_drop_shadow(&group->effects.items[0]);
            group->effects.items[0].u.shadow.size = 3;
            group->effects.items[0].u.shadow.distance = 2;
            pc_effect *stroke = &group->effects.items[1];
            stroke->kind = PC_FX_STROKE;
            stroke->u.stroke.common = pc_fx_common_new(PC_BLEND_MULTIPLY, .6f);
            stroke->u.stroke.size = 2;
            stroke->u.stroke.position = PC_STROKE_OUTSIDE;
            stroke->u.stroke.paint.u.color = pc_color_rgb(.1f, .8f, .3f);
            compare(&f, &doc, pc_rect_make(-4, -3, 78, 55));
            compare(&f, &doc, pc_rect_make(5, 8, 22, 29));
            group->fill_opacity = .3f;
            compare(&f, &doc, pc_document_bounds(&doc));
            doc.layers[3].opacity = .7f;
            compare(&f, &doc, pc_document_bounds(&doc));
            exact_cold(&f, &doc, pc_document_bounds(&doc));
            group->effects.items[0].u.shadow.color = pc_color_rgb(.8f, .1f, .6f);
            compare(&f, &doc, pc_document_bounds(&doc));
            pc_document_fini(&doc);
        }
    finish(&f);
}

static void test_gpu_real_patterns_fills_effects_and_bevel_texture(void)
{
    fixture f = {0};
    if (!start(&f))
        return;
    const pc_color_mode modes[] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                   PC_COLOR_MODE_LAB};
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t mode = 0; mode < 4; mode++)
        for (size_t depth = 0; depth < 3; depth++) {
            pc_document doc;
            CHECK(pc_document_with_background(&doc, "patterns", pc_size_make(32, 24), modes[mode],
                                              depths[depth], pc_color_rgb(.2f, .4f, .6f), NULL) == PC_OK);
            doc.patterns = calloc(1, sizeof *doc.patterns);
            CHECK(doc.patterns != NULL);
            doc.npatterns = 1;
            pc_surface tile;
            pc_surface_init(&tile, PC_PIXEL_FORMAT_RGBA32F);
            for (int y = 0; y < 3; y++)
                for (int x = 0; x < 4; x++) {
                    float pixel[4] = {x & 1 ? .9f : .1f, y & 1 ? .8f : .2f, .3f + (float)x * .1f,
                                      x == 3 ? .6f : 1};
                    CHECK(pc_surface_write_pixel(&tile, x, y, pixel, 4, NULL) == PC_OK);
                }
            CHECK(pc_pattern_new("stripes", 7, &tile, 4, 3, doc.patterns, NULL) == PC_OK);
            pc_layer *grown = realloc(doc.layers, 2 * sizeof *doc.layers);
            CHECK(grown != NULL);
            doc.layers = grown;
            doc.nlayers = 2;
            pc_layer_content content = {.kind = PC_LAYER_FILL};
            content.u.fill.kind = PC_FILL_PATTERN;
            CHECK(pc_str_clone(&doc.patterns[0].id, &content.u.fill.u.pattern.id, NULL) == PC_OK);
            content.u.fill.u.pattern.scale = .7f;
            content.u.fill.u.pattern.angle = 17;
            CHECK(pc_layer_init(&doc.layers[1], "pattern fill", &content, NULL) == PC_OK);
            for (unsigned variant = 0; variant < 2; variant++) {
                doc.layers[1].content.u.fill.u.pattern.link = variant != 0;
                doc.layers[1].content.u.fill.u.pattern.phase[0] = 2.5f;
                compare(&f, &doc, pc_document_bounds(&doc));
            }
            pc_layer_fini(&doc.layers[1]);
            CHECK(pc_layer_raster(&doc.layers[1], "styled", pc_document_pixel_format(&doc), NULL) == PC_OK);
            const float rgba[4] = {.7f, .2f, .3f, .8f};
            float encoded[PC_RASTER_MAX_CHANNELS];
            size_t channels = pc_from_rgba(pc_document_pixel_format(&doc), rgba, encoded);
            CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[1]), pc_rect_make(4, 4, 26, 20),
                                       encoded, channels, NULL) == PC_OK);
            pc_layer *layer = &doc.layers[1];
            layer->effects.items = calloc(3, sizeof *layer->effects.items);
            CHECK(layer->effects.items != NULL);
            layer->effects.nitems = 3;
            pc_effect *overlay = &layer->effects.items[0], *stroke = &layer->effects.items[1],
                      *bevel = &layer->effects.items[2];
            overlay->kind = PC_FX_PATTERN_OVERLAY;
            overlay->u.pattern_overlay.common = pc_fx_common_new(PC_BLEND_MULTIPLY, .7f);
            CHECK(pc_str_clone(&doc.patterns[0].id, &overlay->u.pattern_overlay.id, NULL) == PC_OK);
            overlay->u.pattern_overlay.scale = .8f;
            overlay->u.pattern_overlay.angle = 27;
            overlay->u.pattern_overlay.phase[0] = 1.5f;
            stroke->kind = PC_FX_STROKE;
            stroke->u.stroke.common = pc_fx_common_new(PC_BLEND_NORMAL, .8f);
            stroke->u.stroke.size = 2;
            stroke->u.stroke.position = PC_STROKE_OUTSIDE;
            stroke->u.stroke.paint.kind = PC_FX_PAINT_PATTERN;
            CHECK(pc_str_clone(&doc.patterns[0].id, &stroke->u.stroke.paint.u.pattern.id, NULL) == PC_OK);
            stroke->u.stroke.paint.u.pattern.scale = .75f;
            bevel->kind = PC_FX_BEVEL_EMBOSS;
            bevel->u.bevel.highlight = pc_fx_common_new(PC_BLEND_SCREEN, .7f);
            bevel->u.bevel.shadow = pc_fx_common_new(PC_BLEND_MULTIPLY, .7f);
            bevel->u.bevel.highlight_color = pc_color_rgb(1, 1, 1);
            bevel->u.bevel.shadow_color = pc_color_rgb(0, 0, 0);
            bevel->u.bevel.size = 2;
            bevel->u.bevel.depth = 1;
            bevel->u.bevel.up = true;
            bevel->u.bevel.use_global_light = true;
            bevel->u.bevel.has_texture = true;
            CHECK(pc_str_clone(&doc.patterns[0].id, &bevel->u.bevel.texture.id, NULL) == PC_OK);
            bevel->u.bevel.texture.depth = .5f;
            bevel->u.bevel.texture.scale = .9f;
            pc_effect_in_mode(bevel, doc.mode);
            for (unsigned variant = 0; variant < 2; variant++) {
                overlay->u.pattern_overlay.link = variant != 0;
                bevel->u.bevel.texture.link = variant != 0;
                compare(&f, &doc, pc_rect_make(-2, -2, 34, 26));
                compare(&f, &doc, pc_rect_make(8, 7, 15, 14));
            }
            pc_document_fini(&doc);
        }
    finish(&f);
}

static void test_gpu_duplicate_growth_mask_clear_and_undo_residency(void)
{
    fixture f = {0};
    if (!start(&f))
        return;
    pc_gpu_compositor_set_prefix_budget(f.gpu, 0);
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t depth = 0; depth < 3; depth++) {
        pc_document doc;
        CHECK(pc_document_with_background(&doc, "residency", pc_size_make(520, 280), PC_COLOR_MODE_RGB,
                                          depths[depth], pc_color_rgb(.1f, .3f, .5f), NULL) == PC_OK);
        pc_layer *grown = realloc(doc.layers, 3 * sizeof *doc.layers);
        CHECK(grown != NULL);
        doc.layers = grown;
        pc_pixel_format format = pc_document_pixel_format(&doc);
        CHECK(pc_layer_raster(&doc.layers[1], "paint", format, NULL) == PC_OK);
        doc.nlayers = 2;
        const float colors[][4] = {{.2f, .4f, .9f, .7f}, {.8f, .3f, .2f, .6f}};
        float encoded[PC_RASTER_MAX_CHANNELS];
        size_t channels = pc_from_rgba(format, colors[0], encoded);
        CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[1]), pc_rect_make(270, 20, 300, 50),
                                   encoded, channels, NULL) == PC_OK);
        doc.layers[1].has_mask = true;
        pc_layer_mask_reveal_all(&doc.layers[1].mask);
        compare(&f, &doc, pc_document_bounds(&doc));
        uint64_t uploads = pc_gpu_compositor_source_uploads(f.gpu);
        CHECK(pc_layer_clone(&doc.layers[1], &doc.layers[2], NULL) == PC_OK);
        doc.nlayers = 3;
        doc.layers[2].opacity = .6f;
        compare(&f, &doc, pc_document_bounds(&doc));
        CHECK(pc_gpu_compositor_source_uploads(f.gpu) == uploads);
        pc_document small;
        CHECK(pc_document_clone(&doc, &small, NULL) == PC_OK);
        const pc_rect dabs[] = {
            {400, 230, 420, 250}, {20, 12, 45, 35}, {280, 25, 310, 60}, {500, 260, 520, 280}};
        (void)pc_from_rgba(format, colors[1], encoded);
        for (size_t dab = 0; dab < 4; dab++) {
            CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[1]), dabs[dab], encoded, channels,
                                       NULL) == PC_OK);
            compare(&f, &doc, pc_document_bounds(&doc));
            exact_cold(&f, &doc, pc_document_bounds(&doc));
        }
        compare(&f, &small, pc_document_bounds(&small));
        exact_cold(&f, &small, pc_document_bounds(&small));
        const float half[1] = {.25f};
        CHECK(pc_surface_fill_rect(&doc.layers[1].mask.surface, pc_document_bounds(&doc), half, 1, NULL) ==
              PC_OK);
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_surface_fini(&doc.layers[1].mask.surface);
        const float white[1] = {1};
        pc_surface_init_default(&doc.layers[1].mask.surface, PC_PIXEL_FORMAT_GRAY8, white, 1);
        compare(&f, &doc, pc_document_bounds(&doc));
        pc_surface_fini(pc_layer_surface_mut(&doc.layers[1]));
        pc_surface_init(pc_layer_surface_mut(&doc.layers[1]), format);
        compare(&f, &doc, pc_document_bounds(&doc));
        CHECK(pc_gpu_compositor_source_bytes(f.gpu) <= UINT64_C(128) * 1024 * 1024);
        pc_document_fini(&small);
        pc_document_fini(&doc);
    }
    finish(&f);
}

static void test_gpu_release_large_canvas_benchmark(void)
{
    if (!getenv("PC_GPU_BENCH"))
        return;
    const uint32_t heights[] = {4000, 6000};
    for (size_t i = 0; i < 2; i++) {
        fixture f = {0};
        if (!start(&f))
            return;
        pc_document doc = {0};
        CHECK(pc_document_with_background(&doc, "Two raster layers", pc_size_make(6000, heights[i]),
                                          PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, pc_color_rgb(.2f, .4f, .7f),
                                          NULL) == PC_OK);
        pc_layer *grown = realloc(doc.layers, 2 * sizeof *doc.layers);
        CHECK(grown != NULL);
        doc.layers = grown;
        CHECK(pc_layer_raster(&doc.layers[1], "Overlay", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
        doc.nlayers = 2;
        doc.layers[1].blend = PC_BLEND_MULTIPLY;
        const float color[4] = {.8f, .2f, .4f, .6f};
        CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[1]), pc_rect_make(1000, 1000, 4000, 3000),
                                   color, 4, NULL) == PC_OK);
        double cold = gpu_bench_render(&f, &doc, pc_document_bounds(&doc));
        double warm = gpu_bench_render(&f, &doc, pc_document_bounds(&doc));
        double viewport = gpu_bench_render(&f, &doc, pc_rect_make(1000, 1000, 2280, 1720));
        const float edit[4] = {.2f, .9f, .7f, .8f};
        CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&doc.layers[1]), pc_rect_make(1200, 1200, 1232, 1232),
                                   edit, 4, NULL) == PC_OK);
        double patch = gpu_bench_render(&f, &doc, pc_rect_make(1200, 1200, 1232, 1232));
        fprintf(stderr,
                "GPU bench %ux%u two U8 raster layers: cold=%.3fms warm=%.3fms viewport1280x720=%.3fms "
                "requested32x32=%.3fms\n",
                doc.size.width, doc.size.height, cold, warm, viewport, patch);
        pc_document_fini(&doc);
        finish(&f);
    }
}

int main(int argc, char **argv)
{
    RUN(test_gpu_duplicate_growth_mask_clear_and_undo_residency);
    RUN(test_gpu_real_patterns_fills_effects_and_bevel_texture);
    RUN(test_gpu_group_checkpoint_effects_and_clipping);
    RUN(test_gpu_checkpoint_reference_matrix);
    RUN(test_gpu_release_large_canvas_benchmark);
    RUN(test_gpu_sparse_texture_limit_and_cache_eviction);
    RUN(test_gpu_hdr_blends_and_hard_stop_effects);
    RUN(test_gpu_effect_damage_region_execution);
    RUN(test_gpu_resident_sources);
    RUN(test_gpu_prefix_reuse);
    RUN(test_gpu_blends_and_chunks);
    RUN(test_gpu_adjustments);
    RUN(test_gpu_effect_maps);
    RUN(test_gpu_effect_composition);
    RUN(test_gpu_colour_modes_and_vector_mask);
    RUN(test_gpu_stroked_shape_clipping);
    RUN(test_gpu_groups_and_device_fault);
    return pc_t_finish();
}
