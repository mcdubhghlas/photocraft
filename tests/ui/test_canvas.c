/* Ports of zoom_levels, rotate_view, wheel_nav, scrollbars and ViewXform numeric contracts.
 * Integration cases exercise a real engine document and revision invalidation. */
#include <float.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "harness.h"
#include "ui/canvas.h"
#include "compose/psblend.h"

static void test_zoom_ladder(void)
{
    const uint32_t photo[2] = {2000, 1500}, tiny[2] = {4, 4}, big[2] = {12000, 12000};
    CHECK_NEAR(pc_ui_zoom_min(tiny), .5f, 1e-7);
    CHECK_NEAR(pc_ui_zoom_min(photo), 2.f / 1500.f, 1e-8);
    const float up[] = {2, 3, 4, 5, 6, 7, 8, 12, 16, 32, 64, 128};
    float z = 1;
    for (size_t i = 0; i < sizeof up / sizeof up[0]; i++) {
        z = pc_ui_zoom_step(z, 1, photo);
        CHECK_NEAR(z, up[i], 1e-6);
    }
    CHECK_NEAR(pc_ui_zoom_step(z, 1, photo), 128, 0);
    CHECK_NEAR(pc_ui_zoom_step(8.8f, 1, photo), 12, 0);
    CHECK_NEAR(pc_ui_zoom_step(8.8f, -1, photo), 8, 0);
    CHECK_NEAR(pc_ui_zoom_step(.0007f, -1, big), .0007f, 1e-8);
    CHECK_NEAR(pc_ui_zoom_step(.001f, -1, big), .001f, 1e-8);
    CHECK_NEAR(pc_ui_zoom_clamp(NAN, photo), 1, 0);
    CHECK_NEAR(pc_ui_zoom_clamp(INFINITY, photo), 1, 0);
    CHECK_NEAR(pc_ui_zoom_clamp(500, photo), 128, 0);
    for (int i = 0; i < 40; i++) {
        z = pc_ui_zoom_step(z, -1, photo);
    }
    CHECK_NEAR(z, pc_ui_zoom_min(photo), 1e-8);
}

static void test_mapping_and_anchor(void)
{
    for (int flip = 0; flip < 2; flip++) {
        for (int angle = -180; angle <= 180; angle += 15) {
            pc_ui_canvas_xform xf = {.x = 10,
                                     .y = 20,
                                     .width = 900,
                                     .height = 600,
                                     .zoom = 2,
                                     .center = {300, 200},
                                     .rotation = (float)angle,
                                     .aspect = 1.4f,
                                     .flip = flip != 0};
            float screen[2], doc[2];
            CHECK(pc_ui_canvas_to_screen(&xf, 111, 222, screen) == PC_OK);
            CHECK(pc_ui_canvas_to_doc(&xf, screen[0], screen[1], doc) == PC_OK);
            CHECK_NEAR(doc[0], 111, 1e-3);
            CHECK_NEAR(doc[1], 222, 1e-3);
            pc_ui_view view = {
                .zoom = 4, .center = {300, 200}, .rotation = (float)angle, .doc_size = {2000, 1500}};
            CHECK(pc_ui_canvas_anchor(&view, &xf, screen[0], screen[1], 8, 2) == PC_OK);
            xf.zoom = view.zoom / 2.f;
            xf.center[0] = view.center[0];
            xf.center[1] = view.center[1];
            float anchored[2];
            CHECK(pc_ui_canvas_to_screen(&xf, 111, 222, anchored) == PC_OK);
            CHECK_NEAR(anchored[0], screen[0], 1e-3);
            CHECK_NEAR(anchored[1], screen[1], 1e-3);
        }
    }
    pc_ui_canvas_xform bad = {.width = 100, .height = 100, .zoom = 0};
    float unchanged[2] = {123, 456};
    CHECK(pc_ui_canvas_to_doc(&bad, 0, 0, unchanged) != PC_OK);
    CHECK_NEAR(unchanged[0], 123, 0);
    CHECK_NEAR(unchanged[1], 456, 0);
    bad.zoom = 1;
    bad.rotation = NAN;
    CHECK(pc_ui_canvas_to_screen(&bad, 0, 0, unchanged) != PC_OK);
}

static void test_fit_and_pan(void)
{
    pc_ui_view view = {.doc_size = {2000, 1000}, .fit_pending = true};
    pc_ui_canvas_fit(&view, 1040, 540, 2, 1, false);
    CHECK_NEAR(view.zoom, 1, 1e-6);
    CHECK_NEAR(view.center[0], 1000, 0);
    CHECK(!view.fit_pending);
    pc_ui_canvas_fit(&view, 1000, 1000, 2, 1, true);
    CHECK_NEAR(view.zoom, 2, 1e-6);
    pc_ui_canvas_xform xf = {
        .width = 1000, .height = 1000, .zoom = 1, .center = {1000, 500}, .rotation = 90, .aspect = 1};
    CHECK(pc_ui_canvas_pan(&view, &xf, 100, 0) == PC_OK);
    CHECK_NEAR(view.center[0], 1000, 1e-4);
    CHECK_NEAR(view.center[1], 600, 1e-4);
}

static void test_rotation(void)
{
    CHECK_NEAR(pc_ui_rotation_wrap(-180), 180, 0);
    CHECK_NEAR(pc_ui_rotation_wrap(540), 180, 0);
    CHECK_NEAR(pc_ui_rotation_wrap(NAN), 0, 0);
    CHECK_NEAR(pc_ui_rotation_snap(23, 15), 30, 0);
    const char *json[] = {"{\"angle\":370}",
                          "{\"delta\":20}",
                          "{\"reset\":true}",
                          "{}",
                          "{\"angle\":1e100}",
                          "{\"reset\":1}",
                          "{\"bogus\":1}",
                          "[]",
                          "{\"angle\":3e38,\"delta\":3e38}"};
    for (size_t i = 0; i < sizeof json / sizeof json[0]; i++) {
        yyjson_doc *d = NULL;
        pc_error err;
        float r = 99;
        CHECK(pc_json_parse(json[i], strlen(json[i]), &d, &err) == PC_OK);
        pc_status rc = pc_ui_rotation_params(10, yyjson_doc_get_root(d), &r, &err);
        if (i < 3) {
            CHECK(rc == PC_OK);
            CHECK_NEAR(r, i == 0 ? 10 : i == 1 ? 30 : 0, 1e-6);
        } else {
            CHECK(rc != PC_OK);
            CHECK_NEAR(r, 99, 0);
        }
        yyjson_doc_free(d);
    }
}

static void test_wheel(void)
{
    float v[2];
    pc_ui_wheel_input i = {
        .scroll = {0, 40}, .raw = {0, 40}, .notch = 40, .pinch_zooms = true, .zoom_delta = 1};
    CHECK(pc_ui_wheel_classify(i, v) == PC_UI_WHEEL_PAN);
    CHECK_NEAR(v[1], 40, 0);
    i.alt = true;
    CHECK(pc_ui_wheel_classify(i, v) == PC_UI_WHEEL_ZOOM);
    CHECK_NEAR(v[0], 1.1f, 1e-6);
    i.raw[1] = 0;
    CHECK(pc_ui_wheel_classify(i, v) == PC_UI_WHEEL_NONE);
    i.zoom_delta = 1.2f;
    CHECK(pc_ui_wheel_classify(i, v) == PC_UI_WHEEL_ZOOM);
    CHECK_NEAR(v[0], 1.2f, 1e-6);
    i.pinch_zooms = false;
    CHECK(pc_ui_wheel_classify(i, v) == PC_UI_WHEEL_NONE);
    i.zoom_with_wheel = true;
    CHECK(pc_ui_wheel_classify(i, v) == PC_UI_WHEEL_PAN);
    i.scroll[0] = NAN;
    CHECK(pc_ui_wheel_classify(i, v) == PC_UI_WHEEL_NONE);
}

static void test_scrollbars(void)
{
    pc_ui_scroll_span s;
    float e[2], thumb[2];
    CHECK(pc_ui_scroll_span_make(1000, 500, 500, .4f, &s));
    pc_ui_scroll_extent(s, true, e);
    CHECK_NEAR(e[0], -250, 1e-3);
    CHECK_NEAR(e[1], 1250, 1e-3);
    pc_ui_scroll_thumb(s, e, 300, thumb);
    CHECK_NEAR(thumb[0], 25, 1e-3);
    CHECK_NEAR(thumb[1], 250, 1e-3);
    CHECK_NEAR(pc_ui_scroll_doc_per_point(s, e, 300), 5, 1e-3);
    pc_ui_scroll_extent(s, false, e);
    pc_ui_scroll_thumb(s, e, 300, thumb);
    CHECK_NEAR(thumb[0], 0, 0);
    CHECK_NEAR(thumb[1], 300, 1e-3);
    CHECK(pc_ui_scroll_span_make(10000, 250, 500, 1, &s));
    pc_ui_scroll_extent(s, true, e);
    pc_ui_scroll_thumb(s, e, 300, thumb);
    CHECK_NEAR(thumb[0], 0, 0);
    CHECK_NEAR(thumb[1], 24, 1e-3);
    CHECK_NEAR(pc_ui_scroll_clamp(-400, 1000, 500, 1), 250, 0);
    CHECK_NEAR(pc_ui_scroll_clamp(NAN, 1000, 500, 1), 500, 0);
    CHECK(!pc_ui_scroll_span_make(1000, 500, 500, NAN, &s));
    pc_ui_scroll_thumb(s, e, 0, thumb);
    CHECK_NEAR(thumb[0], 0, 0);
    CHECK_NEAR(thumb[1], 0, 0);
}

static void test_revision_cache(void)
{
    pc_engine_session *s = NULL;
    pc_error err;
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_new(&s, &err) == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "file.new", "{\"width\":8,\"height\":4}", &out, &err) == PC_OK);
    pc_json_value_fini(&out);
    pc_ui_canvas_cache cache = {0};
    CHECK(pc_ui_canvas_cache_update(&cache, pc_engine_session_active(s), &err) == PC_OK);
    CHECK(pc_rect_width(cache.image.rect) == 8 && pc_rect_height(cache.image.rect) == 4);
    uint64_t generation = cache.generation;
    CHECK(pc_ui_canvas_cache_update(&cache, pc_engine_session_active(s), &err) == PC_OK);
    CHECK(cache.generation == generation);
    CHECK(pc_engine_session_execute_json(s, "image.canvasSize", "{\"width\":12,\"height\":6}", &out, &err) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_ui_canvas_cache_update(&cache, pc_engine_session_active(s), &err) == PC_OK);
    CHECK(cache.generation == generation + 1 && pc_rect_width(cache.image.rect) == 12);
    CHECK(pc_ui_canvas_cache_update(&cache, NULL, &err) != PC_OK);
    CHECK(cache.valid && pc_rect_width(cache.image.rect) == 12);
    pc_ui_canvas_cache_fini(&cache);
    pc_engine_session_free(s);
}

static void test_precision_and_colour_modes(void)
{
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const pc_color_mode modes[] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                   PC_COLOR_MODE_LAB};
    for (size_t m = 0; m < sizeof modes / sizeof modes[0]; m++) {
        for (size_t d = 0; d < sizeof depths / sizeof depths[0]; d++) {
            pc_document doc;
            pc_engine_session *s = NULL;
            pc_error err;
            CHECK(pc_document_with_background(&doc, "canvas", pc_size_make(8, 6), modes[m], depths[d],
                                              pc_color_rgb(.12345f, .23456f, .34567f), &err) == PC_OK);
            CHECK(pc_engine_session_new(&s, &err) == PC_OK);
            size_t index;
            CHECK(pc_engine_session_add_document(s, &doc, NULL, &index, &err) == PC_OK);
            const pc_engine_doc_state *state = pc_engine_session_active(s);
            pc_compose_buffer oracle = {0};
            CHECK(pc_compose_flatten(pc_doc_arc_get(state->doc), &oracle, &err) == PC_OK);
            pc_ui_canvas_cache cache = {0};
            CHECK(pc_ui_canvas_cache_update(&cache, state, &err) == PC_OK);
            CHECK(cache.image.len == oracle.len);
            for (size_t i = 0; i < oracle.len; i++) {
                CHECK_NEAR(cache.image.px[i][3], oracle.px[i][3], 1e-6);
                for (size_t ch = 0; ch < 3; ch++) {
                    CHECK_NEAR(cache.image.px[i][ch], oracle.px[i][ch] * oracle.px[i][3], 1e-6);
                }
            }
            if (m == 0 && d == 2) {
                float value = cache.image.px[0][0];
                CHECK_NEAR(value, .12345f, 1e-6);
                CHECK(fabsf(value * 255.f - roundf(value * 255.f)) > .01f);
            }
            pc_compose_buffer_fini(&oracle);
            pc_ui_canvas_cache_fini(&cache);
            pc_engine_session_free(s);
        }
    }
}

static void test_rulers(void)
{
    CHECK_NEAR(pc_ui_ruler_nice_step(2.1, false), 2.5, 0);
    CHECK_NEAR(pc_ui_ruler_nice_step(2.1, true), 5, 0);
    CHECK_NEAR(pc_ui_ruler_nice_step(NAN, true), 1, 0);
    CHECK(isfinite(pc_ui_ruler_nice_step(DBL_MAX, false)));
    for (int angle = -180; angle <= 180; angle += 30) {
        pc_ui_canvas_xform xf = {.x = 16,
                                 .y = 16,
                                 .width = 800,
                                 .height = 600,
                                 .zoom = .75f,
                                 .center = {450, 350},
                                 .rotation = (float)angle,
                                 .aspect = 1.25f,
                                 .flip = true};
        pc_ui_canvas_xform ruler;
        CHECK(pc_ui_canvas_ruler_xform(&xf, &ruler) == PC_OK);
        float a[2], b[2];
        CHECK(pc_ui_canvas_to_screen(&xf, 0, 0, a) == PC_OK);
        CHECK(pc_ui_canvas_to_screen(&ruler, 0, 0, b) == PC_OK);
        CHECK_NEAR(a[0], b[0], 1e-3);
        CHECK_NEAR(a[1], b[1], 1e-3);
        CHECK_NEAR(ruler.rotation, 0, 0);
    }
}

static void test_premultiplied_cache(void)
{
    pc_document doc;
    pc_engine_session *session = NULL;
    pc_error err;
    CHECK(pc_document_with_background(&doc, "alpha", pc_size_make(4, 4), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgba(.2f, .4f, .6f, .5f), &err) == PC_OK);
    CHECK(pc_engine_session_new(&session, &err) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(session, &doc, NULL, &index, &err) == PC_OK);
    pc_ui_canvas_cache cache = {0};
    CHECK(pc_ui_canvas_cache_update(&cache, pc_engine_session_active(session), &err) == PC_OK);
    for (size_t p = 0; p < cache.image.len; p++) {
        CHECK_NEAR(cache.image.px[p][0], .1f, 1e-6);
        CHECK_NEAR(cache.image.px[p][1], .2f, 1e-6);
        CHECK_NEAR(cache.image.px[p][2], .3f, 1e-6);
        CHECK_NEAR(cache.image.px[p][3], .5f, 1e-6);
    }
    pc_ui_canvas_cache_fini(&cache);
    pc_engine_session_free(session);
}

static pc_status patch_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx,
                            pc_error *err);
static void benchmark_canvas_cache(void)
{
    const uint32_t sizes[][2] = {{6000, 4000}, {6000, 6000}};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        pc_document doc;
        pc_engine_session *session = NULL;
        pc_error err;
        CHECK(pc_document_with_background(&doc, "benchmark", pc_size_make(sizes[i][0], sizes[i][1]),
                                          PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, pc_color_rgb(.2f, .4f, .6f),
                                          &err) == PC_OK);
        CHECK(pc_engine_session_new(&session, &err) == PC_OK);
        size_t index;
        CHECK(pc_engine_session_add_document(session, &doc, NULL, &index, &err) == PC_OK);
        const pc_engine_doc_state *state = pc_engine_session_active(session);
        pc_ui_canvas_cache cache = {0};
        struct timespec before, after, hits;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
        CHECK(pc_ui_canvas_cache_update(&cache, state, &err) == PC_OK);
        CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
        for (size_t hit = 0; hit < 10000; hit++) {
            CHECK(pc_ui_canvas_cache_update(&cache, state, &err) == PC_OK);
        }
        CHECK(clock_gettime(CLOCK_MONOTONIC, &hits) == 0);
        CHECK(cache.generation == 1);
        double cold =
            (double)(after.tv_sec - before.tv_sec) + (double)(after.tv_nsec - before.tv_nsec) * 1e-9;
        double warm = (double)(hits.tv_sec - after.tv_sec) + (double)(hits.tv_nsec - after.tv_nsec) * 1e-9;
        (void)printf("canvas %ux%u: cold %.3f ms; 10000 cache hits %.3f ms; preview %ux%u\n", sizes[i][0],
                     sizes[i][1], cold * 1000., warm * 1000., pc_rect_width(cache.image.rect),
                     pc_rect_height(cache.image.rect));
        pc_ui_canvas_cache_fini(&cache);
        pc_ui_canvas_xform xf = {.width = 1280,
                                 .height = 720,
                                 .zoom = 1,
                                 .aspect = 1,
                                 .center = {(float)sizes[i][0] / 2, (float)sizes[i][1] / 2}};
        CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
        CHECK(pc_ui_canvas_cache_update_view(&cache, state, &xf, &err) == PC_OK);
        CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
        pc_rect damage = pc_rect_make((int32_t)sizes[i][0] / 2, (int32_t)sizes[i][1] / 2,
                                      (int32_t)sizes[i][0] / 2 + 32, (int32_t)sizes[i][1] / 2 + 32);
        CHECK(pc_engine_session_edit(session, "benchmark patch", patch_edit, &damage, &err) == PC_OK);
        pc_engine_doc_state *mutable = pc_engine_session_active_mut(session);
        mutable->has_last_damage = true;
        mutable->last_damage = damage;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &hits) == 0);
        CHECK(pc_ui_canvas_cache_update_view(&cache, state, &xf, &err) == PC_OK);
        struct timespec patched;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &patched) == 0);
        CHECK(cache.partial && pc_rect_eq(cache.last_render, damage));
        double full =
            (double)(after.tv_sec - before.tv_sec) + (double)(after.tv_nsec - before.tv_nsec) * 1e-9;
        double patch =
            (double)(patched.tv_sec - hits.tv_sec) + (double)(patched.tv_nsec - hits.tv_nsec) * 1e-9;
        (void)printf("canvas %ux%u: full viewport %.3f ms; 32x32 damage %.3f ms\n", sizes[i][0], sizes[i][1],
                     full * 1000., patch * 1000.);
        pc_ui_canvas_cache_fini(&cache);
        pc_engine_session_free(session);
    }
}

static void test_full_resolution_region(void)
{
    pc_document doc;
    pc_engine_session *session = NULL;
    CHECK(pc_document_with_background(&doc, "large", pc_size_make(8192, 32), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(0, 0, 0), NULL) == PC_OK);
    float red[4] = {1, 0, 0, 1};
    pc_surface *surface = pc_layer_surface_mut(&doc.layers[0]);
    CHECK(surface != NULL);
    CHECK(pc_surface_write_pixel(surface, 5000, 8, red, 4, NULL) == PC_OK);
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(session, &doc, NULL, &index, NULL) == PC_OK);
    const pc_engine_doc_state *st = pc_engine_session_active(session);
    pc_ui_canvas_xform xf = {.width = 320, .height = 32, .zoom = 1, .aspect = 1, .center = {5000, 16}};
    pc_ui_canvas_cache cache = {0};
    CHECK(pc_ui_canvas_cache_update_view(&cache, st, &xf, NULL) == PC_OK && cache.region);
    CHECK(pc_rect_width(cache.source_rect) < 2048);
    const float *p = pc_compose_buffer_get(&cache.image, 5000, 8);
    CHECK_NEAR(p[0], 1, 0);
    p = pc_compose_buffer_get(&cache.image, 5001, 8);
    CHECK_NEAR(p[0], 0, 0);
    uint64_t generation = cache.generation;
    for (unsigned i = 0; i < 100; i++) {
        CHECK(pc_ui_canvas_cache_update_view(&cache, st, &xf, NULL) == PC_OK);
    }
    CHECK(cache.generation == generation);
    float old_gamma = pc_psblend_text_gamma();
    pc_psblend_set_text_gamma(1.7f);
    CHECK(pc_ui_canvas_cache_update_view(&cache, st, &xf, NULL) == PC_OK);
    CHECK(cache.generation == generation + 1);
    pc_psblend_set_text_gamma(old_gamma);
    CHECK(pc_ui_canvas_cache_update_view(&cache, st, &xf, NULL) == PC_OK);
    CHECK(cache.generation == generation + 2);
    generation = cache.generation;
    xf.center[0] = 5001;
    CHECK(pc_ui_canvas_cache_update_view(&cache, st, &xf, NULL) == PC_OK);
    CHECK(cache.generation == generation);
    xf.center[0] = 6000;
    CHECK(pc_ui_canvas_cache_update_view(&cache, st, &xf, NULL) == PC_OK);
    CHECK(cache.generation == generation + 1);
    CHECK(pc_ui_canvas_cache_update(&cache, st, NULL) == PC_OK && !cache.region);
    CHECK(pc_rect_width(cache.image.rect) == 4096 && pc_rect_width(cache.source_rect) == 8192);
    xf.width = NAN;
    generation = cache.generation;
    CHECK(pc_ui_canvas_cache_update_view(&cache, st, &xf, NULL) == PC_ERR_INVALID);
    CHECK(cache.generation == generation);
    pc_ui_canvas_cache_fini(&cache);
    pc_engine_session_free(session);
}

static void test_gesture_anchor(void)
{
    for (unsigned flip = 0; flip < 2; flip++) {
        for (int angle = -150; angle <= 150; angle += 30) {
            pc_ui_view view = {
                .doc_size = {8192, 4096}, .center = {4000, 2000}, .zoom = 2, .rotation = (float)angle};
            pc_ui_canvas_xform xf = {.x = 10,
                                     .y = 20,
                                     .width = 900,
                                     .height = 600,
                                     .aspect = 1.3f,
                                     .zoom = 1,
                                     .center = {4000, 2000},
                                     .rotation = (float)angle,
                                     .flip = flip != 0};
            float from[2] = {200, 220}, to[2] = {250, 270}, before[2], after[2];
            CHECK(pc_ui_canvas_to_doc(&xf, from[0], from[1], before) == PC_OK);
            CHECK(pc_ui_canvas_gesture(&view, &xf, from, to, 1.5f, 35, 2) == PC_OK);
            CHECK_NEAR(view.zoom, 3, 1e-6);
            xf.zoom = view.zoom / 2;
            xf.center[0] = view.center[0];
            xf.center[1] = view.center[1];
            xf.rotation = view.rotation;
            CHECK(pc_ui_canvas_to_doc(&xf, to[0], to[1], after) == PC_OK);
            CHECK_NEAR(after[0], before[0], .001);
            CHECK_NEAR(after[1], before[1], .001);
            pc_ui_view saved = view;
            CHECK(pc_ui_canvas_gesture(&view, &xf, from, to, NAN, 0, 2) == PC_ERR_INVALID);
            CHECK(memcmp(&view, &saved, sizeof view) == 0);
            to[0] = INFINITY;
            CHECK(pc_ui_canvas_gesture(&view, &xf, from, to, 1, 0, 2) == PC_ERR_INVALID);
            CHECK(memcmp(&view, &saved, sizeof view) == 0);
        }
    }
}

static pc_status patch_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)has_active;
    (void)active;
    const float green[4] = {0, 1, 0, .5f};
    return pc_surface_fill_rect(pc_layer_surface_mut(&doc->layers[0]), *(pc_rect *)ctx, green, 4, err);
}
static void test_damage_compositing(void)
{
    pc_document doc;
    pc_engine_session *session = NULL;
    CHECK(pc_document_with_background(&doc, "damage", pc_size_make(768, 64), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(1, 0, 0), NULL) == PC_OK);
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(session, &doc, NULL, &index, NULL) == PC_OK);
    pc_engine_doc_state *state = pc_engine_session_active_mut(session);
    pc_ui_canvas_cache cache = {0};
    CHECK(pc_ui_canvas_cache_update(&cache, state, NULL) == PC_OK);
    pc_rect damage = pc_rect_make(300, 10, 305, 15);
    CHECK(pc_engine_session_edit(session, "patch", patch_edit, &damage, NULL) == PC_OK);
    state->has_last_damage = true;
    state->last_damage = damage;
    CHECK(pc_ui_canvas_cache_update(&cache, state, NULL) == PC_OK);
    CHECK(cache.partial && pc_rect_eq(cache.last_render, damage));
    const float *pixel = pc_compose_buffer_get(&cache.image, 302, 12);
    CHECK_NEAR(pixel[0], 0, 0);
    CHECK_NEAR(pixel[1], .5f, 0);
    CHECK_NEAR(pixel[3], .5f, 0);
    pixel = pc_compose_buffer_get(&cache.image, 306, 12);
    CHECK_NEAR(pixel[0], 1, 0);
    uint64_t generation = cache.generation;
    state->revision++;
    state->last_damage = pc_rect_make(1000, 1000, 1001, 1001);
    CHECK(pc_ui_canvas_cache_update(&cache, state, NULL) == PC_OK);
    CHECK(cache.generation == generation && cache.revision == state->revision);
    /* Missing an intervening revision requires a complete composite. */
    state->revision += 2;
    state->last_damage = damage;
    CHECK(pc_ui_canvas_cache_update(&cache, state, NULL) == PC_OK);
    CHECK(!cache.partial && pc_rect_eq(cache.last_render, pc_rect_make(0, 0, 768, 64)));
    /* Unknown damage must also invalidate the complete image. */
    state->revision++;
    state->has_last_damage = false;
    CHECK(pc_ui_canvas_cache_update(&cache, state, NULL) == PC_OK);
    CHECK(!cache.partial);
    pc_ui_canvas_cache_fini(&cache);
    pc_engine_session_free(session);
}

static void test_damage_effect_halo_and_feather_fallback(void)
{
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "effect damage", pc_size_make(256, 96), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_F32, pc_color_rgb(1, 0, 0), NULL) == PC_OK);
    doc.layers[0].effects.items = calloc(1, sizeof *doc.layers[0].effects.items);
    CHECK(doc.layers[0].effects.items != NULL);
    doc.layers[0].effects.nitems = 1;
    pc_effect_default_drop_shadow(doc.layers[0].effects.items);
    doc.layers[0].effects.items[0].u.shadow.size = 4;
    doc.layers[0].effects.items[0].u.shadow.distance = 7;
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    size_t index;
    CHECK(pc_engine_session_add_document(session, &doc, NULL, &index, NULL) == PC_OK);
    pc_engine_doc_state *state = pc_engine_session_active_mut(session);
    pc_ui_canvas_cache cache = {0};
    CHECK(pc_ui_canvas_cache_update(&cache, state, NULL) == PC_OK);
    pc_rect damage = pc_rect_make(100, 40, 105, 45);
    CHECK(pc_engine_session_edit(session, "patch", patch_edit, &damage, NULL) == PC_OK);
    state->has_last_damage = true;
    state->last_damage = damage;
    CHECK(pc_ui_canvas_cache_update(&cache, state, NULL) == PC_OK);
    CHECK(cache.partial);
    CHECK(cache.last_render.x0 < damage.x0 && cache.last_render.x1 > damage.x1);
    pc_compose_buffer expected = {0};
    CHECK(pc_compose_render(pc_doc_arc_get(state->doc), pc_rect_make(0, 0, 256, 96), &expected, NULL) ==
          PC_OK);
    float worst = 0;
    for (size_t i = 0; i < expected.len; i++) {
        for (size_t ch = 0; ch < 4; ch++) {
            float value = ch == 3 ? expected.px[i][ch] : expected.px[i][ch] * expected.px[i][3];
            worst = fmaxf(worst, fabsf(value - cache.image.px[i][ch]));
        }
    }
    CHECK_NEAR(worst, 0, 1e-6);
    pc_compose_buffer_fini(&expected);
    pc_document changed;
    CHECK(pc_document_clone(pc_doc_arc_get(state->doc), &changed, NULL) == PC_OK);
    changed.layers[0].has_mask = true;
    pc_layer_mask_reveal_all(&changed.layers[0].mask);
    changed.layers[0].mask.feather = 2;
    pc_doc_arc *next = NULL;
    CHECK(pc_doc_arc_new(&changed, &next, NULL) == PC_OK);
    pc_doc_arc_release(state->doc);
    state->doc = next;
    state->revision++;
    CHECK(!pc_ui_canvas_cache_can_damage(&cache, state));
    CHECK(pc_ui_canvas_cache_update(&cache, state, NULL) == PC_OK);
    CHECK(!cache.partial);
    pc_ui_canvas_cache_fini(&cache);
    pc_engine_session_free(session);
}

static void test_crop_plane_retained_pixels_cache_and_bounds(void)
{
    pc_engine_session *session = NULL;
    CHECK(pc_engine_session_new(&session, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(session, "file.new", "{\"width\":64,\"height\":64}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    pc_rect damage = pc_rect_make(80, 20, 84, 24);
    CHECK(pc_engine_session_edit(session, "retained outside pixels", patch_edit, &damage, NULL) == PC_OK);
    pc_ui_canvas_cache cache = {0};
    pc_ui_canvas_xform xf = {.width = 200, .height = 160, .zoom = 1, .center = {32, 32}, .aspect = 1};
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    CHECK(pc_ui_canvas_cache_update_plane(&cache, state, &xf, NULL, NULL) == PC_OK);
    CHECK(cache.source_rect.x0 < 0 && cache.source_rect.x1 > 64 && cache.region);
    size_t x = (size_t)(81 - cache.image.rect.x0), y = (size_t)(21 - cache.image.rect.y0),
           width = pc_rect_width(cache.image.rect);
    CHECK(x < width && y < pc_rect_height(cache.image.rect));
    if (x < width && y < pc_rect_height(cache.image.rect)) {
        const float *pixel = cache.image.px[y * width + x];
        CHECK(pixel[1] > .49f && pixel[1] < .51f && pixel[0] < .01f && pixel[3] > .49f && pixel[3] < .51f);
    }
    uint64_t generation = cache.generation;
    CHECK(pc_ui_canvas_cache_update_plane(&cache, state, &xf, NULL, NULL) == PC_OK &&
          cache.generation == generation);
    xf.center[0] = 2000000;
    CHECK(pc_ui_canvas_cache_update_plane(&cache, state, &xf, NULL, NULL) == PC_ERR_LIMIT);
    CHECK(cache.generation == generation);
    CHECK(pc_ui_canvas_cache_update(&cache, state, NULL) == PC_OK);
    CHECK(!cache.region && pc_rect_eq(cache.source_rect, pc_rect_make(0, 0, 64, 64)));
    pc_ui_canvas_cache_fini(&cache);
    pc_engine_session_free(session);
}

static void test_screen_segment_clipping_extremes_and_transaction(void)
{
    pc_ui_canvas_xform xf = {.x = 10, .y = 20, .width = 100, .height = 80};
    float a[2] = {-1e12f, 50}, b[2] = {1e12f, 50};
    CHECK(pc_ui_canvas_clip_line(&xf, a, b));
    CHECK_NEAR(a[0], 10, .001);
    CHECK_NEAR(b[0], 110, .001);
    CHECK(a[1] == 50 && b[1] == 50);
    a[0] = b[0] = -200;
    a[1] = 30;
    b[1] = 90;
    CHECK(!pc_ui_canvas_clip_line(&xf, a, b) && a[0] == -200 && b[1] == 90);
    a[0] = NAN;
    CHECK(!pc_ui_canvas_clip_line(&xf, a, b) && isnan(a[0]));
    xf = (pc_ui_canvas_xform){.x = -1e7f, .y = -1e7f, .width = 2e7f, .height = 2e7f};
    a[0] = a[1] = -1e6f;
    b[0] = b[1] = 1e6f;
    CHECK(pc_ui_canvas_clip_line(&xf, a, b));
    CHECK(a[0] >= -32760 && b[0] <= 32760);
}
int main(int argc, char **argv)
{
    RUN(test_screen_segment_clipping_extremes_and_transaction);
    RUN(test_crop_plane_retained_pixels_cache_and_bounds);
    RUN(test_damage_effect_halo_and_feather_fallback);
    RUN(test_damage_compositing);
    RUN(test_rulers);
    RUN(test_gesture_anchor);
    RUN(test_full_resolution_region);
    RUN(test_zoom_ladder);
    RUN(test_mapping_and_anchor);
    RUN(test_fit_and_pan);
    RUN(test_rotation);
    RUN(test_wheel);
    RUN(test_scrollbars);
    RUN(test_revision_cache);
    RUN(test_precision_and_colour_modes);
    RUN(test_premultiplied_cache);
    if (getenv("PC_CANVAS_BENCH")) {
        RUN(benchmark_canvas_cache);
    }
    return pc_t_finish();
}
