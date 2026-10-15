/* The remaining standalone Filter-menu filters (port of crates/engine/src/filters_ext.rs tests,
 * E-tail EF2): every id applies and is undoable, choice validation rejects bad choices, Relight's
 * strict numeric validation, the colour-reading filters pick up the session colours, and adversarial
 * params never crash. The layer is painted directly so the test needs E1 + E9 + EF2 + algo. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

/* CHECK with a formatted message (reports which id failed). */
#define CHECKF(cond, ...)                                                                          \
    do {                                                                                           \
        pc_t_checks++;                                                                             \
        if (!(cond)) {                                                                             \
            PC_T_FAIL(__VA_ARGS__);                                                                \
        }                                                                                          \
    } while (0)

#define W 40
#define H 32

static pc_engine_session *painted(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(W, H), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    pc_layer l;
    CHECK(pc_layer_raster(&l, "layer", fmt, NULL) == PC_OK);
    pc_layer_id id;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);
    pc_layer *lm = pc_document_layer_mut(&doc, id);
    pc_surface *surf = pc_layer_surface_mut(lm);
    size_t n = pc_surface_channels(surf);
    float *data = calloc((size_t)W * H * n, sizeof *data);
    CHECK(data != NULL);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float v = (float)((x * 5 + y * 3) % 17) / 16.0f;
            float *px = &data[((size_t)y * W + (size_t)x) * n];
            px[0] = v;
            px[1] = 1.0f - v;
            px[2] = (float)(x % 4) / 3.0f;
            if (n >= 4) {
                px[3] = 1.0f;
            }
        }
    }
    CHECK(pc_surface_write_region(surf, pc_rect_make(0, 0, W, H), data, (size_t)W * H * n, NULL) == PC_OK);
    free(data);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static float *snapshot(pc_engine_session *s, size_t *len)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_surface *surf = pc_layer_surface(pc_document_layer(doc, d->active_layer));
    float *px = NULL;
    CHECK(pc_surface_read_region(surf, pc_rect_make(0, 0, W, H), &px, len, NULL) == PC_OK);
    return px;
}

static bool changed(pc_engine_session *s, const float *before)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_surface *surf = pc_layer_surface(pc_document_layer(doc, d->active_layer));
    float *now = NULL;
    size_t len = 0;
    if (pc_surface_read_region(surf, pc_rect_make(0, 0, W, H), &now, &len, NULL) != PC_OK) {
        return false;
    }
    bool differ = false;
    for (size_t i = 0; i < len; i++) {
        if (fabsf(now[i] - before[i]) > 1e-6f) {
            differ = true;
            break;
        }
    }
    free(now);
    return differ;
}

/* Every id this module registers. */
static const char *const ALL_IDS[] = {
    "filter.pixelate.colorHalftone", "filter.pixelate.crystallize",   "filter.pixelate.facet",
    "filter.pixelate.fragment",      "filter.pixelate.mezzotint",     "filter.pixelate.pointillize",
    "filter.stylize.diffuse",        "filter.stylize.extrude",        "filter.stylize.oilPaint",
    "filter.stylize.tiles",          "filter.stylize.traceContour",   "filter.stylize.wind",
    "filter.distort.shear",          "filter.distort.zigZag",         "filter.render.fibers",
    "filter.render.lensFlare",       "filter.render.lightingEffects", "filter.render.relight",
    "filter.noise.reduceNoise",      "filter.blur.smartBlur",         "filter.blur.lensBlur",
    "filter.blur.shapeBlur",         "filter.blurGallery.tiltShift",  "filter.blurGallery.irisBlur",
    "filter.blurGallery.fieldBlur",  "filter.blurGallery.spinBlur",   "filter.blurGallery.pathBlur",
    "filter.other.custom",           "filter.other.hsbHsl",           "filter.video.deInterlace",
    "filter.video.ntscColors",
};

/* Each id runs with default params, applies, and undoes cleanly. */
static void test_each_applies(void)
{
    for (size_t i = 0; i < sizeof ALL_IDS / sizeof ALL_IDS[0]; i++) {
        pc_engine_session *s = painted();
        size_t len = 0;
        float *before = snapshot(s, &len);
        pc_json_value out;
        pc_error err;
        pc_error_clear(&err);
        pc_status rc = pc_engine_session_execute_json(s, ALL_IDS[i], "{}", &out, &err);
        CHECKF(rc == PC_OK, "%s failed: %s", ALL_IDS[i], err.msg);
        pc_json_value_fini(&out);
        CHECK(pc_engine_session_undo(s));
        CHECK(!changed(s, before));
        free(before);
        pc_engine_session_free(s);
    }
}

/* The colour-reading filters apply with explicit colour params (fibers paints with them). */
static void test_colours(void)
{
    pc_engine_session *s = painted();
    size_t len = 0;
    float *before = snapshot(s, &len);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "filter.render.fibers",
                                         "{\"foreground\":[1,0,0,1],\"background\":\"#00ff00\"}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(changed(s, before));
    free(before);
    /* Tiles and Pointillize accept injected session colours (no explicit colour param). */
    CHECK(pc_engine_session_execute_json(s, "filter.stylize.tiles", "{\"count\":4}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "filter.pixelate.pointillize", "{\"cellSize\":8}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* Composite-array params (lights / pins / paths) pass through. */
static void test_composite_arrays(void)
{
    pc_engine_session *s = painted();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "filter.render.lightingEffects",
              "{\"lights\":[{\"kind\":\"point\",\"x\":0.3,\"y\":0.3,\"intensity\":50}]}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(
              s, "filter.blurGallery.irisBlur",
              "{\"pins\":[{\"x\":0.5,\"y\":0.5,\"radiusX\":0.4,\"radiusY\":0.3,\"blur\":10}]}", &out,
              NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(
              s, "filter.blurGallery.pathBlur",
              "{\"paths\":[{\"points\":[[0.1,0.1],[0.9,0.9]],\"speed\":80}]}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* Documented choices are validated; a bad choice is a BadParams error, not a clamp. */
static void test_choice_validation(void)
{
    pc_engine_session *s = painted();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "filter.pixelate.mezzotint", "{\"type\":\"nope\"}", &out,
                                         NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "filter.stylize.wind", "{\"method\":\"typhoon\"}", &out,
                                         NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "filter.blur.lensBlur", "{\"shape\":\"octopus\"}", &out,
                                         NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "filter.other.hsbHsl", "{\"inputMode\":\"cmyk\"}", &out,
                                         NULL) != PC_OK);
    /* A valid choice still applies. */
    CHECK(pc_engine_session_execute_json(s, "filter.stylize.wind", "{\"method\":\"blast\"}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* Relight's strict numeric validation. */
static void test_relight_validation(void)
{
    pc_engine_session *s = painted();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "filter.render.relight", "{\"angle\":999}", &out, NULL) !=
          PC_OK);
    CHECK(pc_engine_session_execute_json(s, "filter.render.relight", "{\"elevation\":-5}", &out, NULL) !=
          PC_OK);
    CHECK(pc_engine_session_execute_json(s, "filter.render.relight", "{\"intensity\":\"bright\"}", &out,
                                         NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "filter.render.relight", "{\"angle\":45,\"intensity\":60}",
                                         &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* Last Filter re-runs the most recent ext filter. */
static void test_last_filter(void)
{
    pc_engine_session *s = painted();
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "filter.stylize.wind", "{\"method\":\"blast\"}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "filter.lastFilter", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* Adversarial params for every id must never crash (Rule 9). */
static void test_hostile_params(void)
{
    const char *cases[] = {
        "null",
        "[]",
        "true",
        "42",
        "{\"seed\":-1}",
        "{\"seed\":99999999999999}",
        "{\"cellSize\":1e308}",
        "{\"maxRadius\":-1e308}",
        "{\"amount\":null}",
        "{\"radius\":\"big\"}",
        "{\"type\":123}",
        "{\"foreground\":\"#zzzzzz\"}",
        "{\"foreground\":[1,2]}",
        "{\"background\":{}}",
        "{\"kernel\":true}",
        "{\"kernel\":[1e308,-1e308,0]}",
        "{\"lights\":42}",
        "{\"lights\":[true,{\"x\":1e308}]}",
        "{\"pins\":[]}",
        "{\"pins\":[{\"blur\":1e308,\"radiusX\":-5}]}",
        "{\"paths\":[{\"speed\":1e308}]}",
        "{\"paths\":[{\"points\":[[1]]}]}",
        "{\"points\":[[0,0],[1]]}",
        "{\"angle\":9e9,\"blurAngle\":-9e9}",
    };
    for (size_t i = 0; i < sizeof ALL_IDS / sizeof ALL_IDS[0]; i++) {
        for (size_t j = 0; j < sizeof cases / sizeof cases[0]; j++) {
            pc_engine_session *s = painted();
            pc_json_value out;
            if (pc_engine_session_execute_json(s, ALL_IDS[i], cases[j], &out, NULL) == PC_OK) {
                pc_json_value_fini(&out);
            }
            pc_engine_session_free(s);
        }
    }
}

int main(int argc, char **argv)
{
    RUN(test_each_applies);
    RUN(test_colours);
    RUN(test_composite_arrays);
    RUN(test_choice_validation);
    RUN(test_relight_validation);
    RUN(test_last_filter);
    RUN(test_hostile_params);
    return pc_t_finish();
}
