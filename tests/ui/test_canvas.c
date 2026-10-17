/* Ports of zoom_levels, rotate_view, wheel_nav, scrollbars and ViewXform numeric contracts.
 * Integration cases exercise a real engine document and revision invalidation. */
#include <float.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "harness.h"
#include "ui/canvas.h"

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
        pc_engine_session_free(session);
    }
}

int main(int argc, char **argv)
{
    RUN(test_rulers);
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
