/* Filter menu commands (port of crates/engine/src/filters.rs tests, E9). The reference tests drive
 * edit.fill/select.*; here the layer is painted directly so the test needs only E1 + E9. */
#include <stdlib.h>
#include <string.h>

#include "base/str.h"
#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

#define W 48
#define H 32

static pc_engine_session *painted(pc_sample_type depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(W, H), PC_COLOR_MODE_RGB, depth,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    pc_layer l;
    CHECK(pc_layer_raster(&l, "layer", fmt, NULL) == PC_OK);
    pc_layer_id id;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);
    /* Paint a non-uniform pattern. */
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
            px[2] = (float)(x % 3) / 2.0f;
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

static float *read_active(pc_engine_session *s, size_t *len)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    const pc_surface *surf = pc_layer_surface(l);
    float *px = NULL;
    if (pc_surface_read_region(surf, pc_rect_make(0, 0, W, H), &px, len, NULL) != PC_OK) {
        return NULL;
    }
    return px;
}

static bool differs(const float *a, const float *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return true;
        }
    }
    return false;
}

static bool same(const float *a, const float *b, size_t n)
{
    return !differs(a, b, n);
}

static const char *const ALL[] = {
    "filter.blur.gaussianBlur", "filter.blur.boxBlur",      "filter.blur.motionBlur",
    "filter.blur.radialBlur",   "filter.blur.surfaceBlur",  "filter.sharpen.unsharpMask",
    "filter.sharpen.smartSharpen", "filter.noise.addNoise", "filter.noise.median",
    "filter.noise.dustAndScratches", "filter.pixelate.mosaic", "filter.stylize.emboss",
    "filter.stylize.findEdges", "filter.stylize.solarize",  "filter.distort.twirl",
    "filter.distort.pinch",     "filter.distort.spherize",  "filter.distort.wave",
    "filter.distort.ripple",    "filter.distort.polarCoordinates", "filter.other.highPass",
    "filter.other.minimum",     "filter.other.maximum",     "filter.other.offset",
};

static const char *params_for_id(const char *id)
{
    if (strcmp(id, "filter.other.offset") == 0) {
        return "{\"horizontal\":5,\"vertical\":2}";
    }
    if (strcmp(id, "filter.noise.dustAndScratches") == 0) {
        return "{\"radius\":2,\"threshold\":0}";
    }
    if (strcmp(id, "filter.noise.median") == 0) {
        return "{\"radius\":2}";
    }
    return "{}";
}

static void test_every_filter_runs_and_undoes(void)
{
    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
        pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8);
        size_t n0 = 0;
        float *before = read_active(s, &n0);
        CHECK(before != NULL);
        pc_json_value out;
        pc_error err;
        pc_status rc = pc_engine_session_execute_json(s, ALL[i], params_for_id(ALL[i]), &out, &err);
        CHECK(rc == PC_OK);
        if (rc == PC_OK) {
            CHECK(yyjson_mut_obj_get(pc_json_value_root(&out), "filter") != NULL);
            pc_json_value_fini(&out);
            size_t n1 = 0;
            float *after = read_active(s, &n1);
            CHECK(after != NULL && n1 == n0 && differs(before, after, n0));
            free(after);
            CHECK(pc_engine_session_execute_json(s, "edit.undo", "{}", &out, NULL) == PC_OK);
            pc_json_value_fini(&out);
            size_t n2 = 0;
            float *undone = read_active(s, &n2);
            CHECK(undone != NULL && n2 == n0 && same(before, undone, n0));
            free(undone);
        }
        free(before);
        pc_engine_session_free(s);
    }
}

static void test_preset_filters_name_their_step(void)
{
    struct {
        const char *id;
        const char *name;
    } presets[] = {
        {"filter.blur.blur", "Blur"},         {"filter.blur.blurMore", "Blur More"},
        {"filter.sharpen.sharpen", "Sharpen"}, {"filter.sharpen.sharpenMore", "Sharpen More"},
        {"filter.sharpen.sharpenEdges", "Sharpen Edges"}, {"filter.noise.despeckle", "Despeckle"},
        {"filter.blur.gaussianBlur", "Gaussian Blur"}, {"filter.sharpen.unsharpMask", "Unsharp Mask"},
        {"filter.blur.surfaceBlur", "Surface Blur"},
    };
    for (size_t i = 0; i < sizeof presets / sizeof presets[0]; i++) {
        pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8);
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, presets[i].id, "{}", &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        const pc_engine_doc_state *d = pc_engine_session_active(s);
        size_t nh = pc_history_entry_count(d->history);
        CHECK(nh > 0);
        const pc_str *last = pc_history_entry(d->history, nh - 1);
        CHECK_EQ_STR(pc_str_c(last), presets[i].name);
        pc_engine_session_free(s);
    }
}

static void test_params_map_to_algorithm_units(void)
{
    pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "filter.blur.gaussianBlur", "{\"radius\":4.5}", &out, NULL) ==
          PC_OK);
    yyjson_mut_val *f = yyjson_mut_obj_get(pc_json_value_root(&out), "filter");
    CHECK(f != NULL);
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_obj_get(f, "radius")), 4.5, 1e-6);
    pc_json_value_fini(&out);
    /* u32 seed survives (not saturated through i32). */
    CHECK(pc_engine_session_execute_json(s, "filter.noise.addNoise", "{\"seed\":4294967295}", &out,
                                         NULL) == PC_OK);
    f = yyjson_mut_obj_get(pc_json_value_root(&out), "filter");
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(f, "seed")), (uint64_t)4294967295ULL);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_validation_rejects_bad_params(void)
{
    pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8);
    const char *bad[] = {
        "{\"radus\":3}",                  /* unknown key */
        "{\"radius\":\"big\"}",           /* wrong type */
        "{\"radius\":5000}",              /* out of range */
        "{\"radius\":-4}",                /* out of range */
    };
    for (size_t i = 0; i < 4; i++) {
        pc_json_value out;
        pc_error err;
        pc_status rc = pc_engine_session_execute_json(s, "filter.blur.gaussianBlur", bad[i], &out, &err);
        CHECK(rc != PC_OK);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
    }
    /* enum/bool/seed validation. */
    const char *bad2[][2] = {
        {"filter.blur.radialBlur", "{\"method\":\"typo\"}"},
        {"filter.noise.addNoise", "{\"monochromatic\":\"true\"}"},
        {"filter.noise.addNoise", "{\"seed\":-1}"},
        {"filter.noise.addNoise", "{\"seed\":4294967296}"},
        {"filter.blur.gaussianBlur", "{\"layer\":\"2\"}"},
        {"filter.blur.gaussianBlur", "{\"target\":\"typo\"}"},
    };
    for (size_t i = 0; i < 6; i++) {
        pc_json_value out;
        pc_status rc = pc_engine_session_execute_json(s, bad2[i][0], bad2[i][1], &out, NULL);
        CHECK(rc != PC_OK);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
    }
    pc_engine_session_free(s);
}

static void test_last_filter_repeats(void)
{
    pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8);
    CHECK(!pc_engine_session_is_enabled(s, "filter.lastFilter"));
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "filter.other.offset", "{\"horizontal\":3}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    size_t n1 = 0;
    float *once = read_active(s, &n1);
    CHECK(pc_engine_session_is_enabled(s, "filter.lastFilter"));
    CHECK(pc_engine_session_execute_json(s, "filter.lastFilter", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    size_t n2 = 0;
    float *twice = read_active(s, &n2);
    CHECK(once != NULL && twice != NULL && differs(once, twice, n1));
    free(once);
    /* offsetting 3 twice == offsetting 6 once. */
    pc_engine_session *s2 = painted(PC_SAMPLE_TYPE_U8);
    CHECK(pc_engine_session_execute_json(s2, "filter.other.offset", "{\"horizontal\":6}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    size_t n3 = 0;
    float *six = read_active(s2, &n3);
    CHECK(six != NULL && n3 == n2 && same(twice, six, n2));
    free(twice);
    free(six);
    pc_engine_session_free(s);
    pc_engine_session_free(s2);
}

static void test_huge_offsets_clamp(void)
{
    pc_engine_session *a = painted(PC_SAMPLE_TYPE_U8);
    pc_engine_session *b = painted(PC_SAMPLE_TYPE_U8);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(a, "filter.other.offset", "{\"horizontal\":3000000000,\"vertical\":0}",
                                         &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(b, "filter.other.offset", "{\"horizontal\":2147483647,\"vertical\":0}",
                                         &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    size_t na = 0, nb = 0;
    float *pa = read_active(a, &na);
    float *pb = read_active(b, &nb);
    CHECK(pa != NULL && pb != NULL && na == nb && same(pa, pb, na));
    free(pa);
    free(pb);
    pc_engine_session_free(a);
    pc_engine_session_free(b);
}

static void test_disabled_without_pixel_layer(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "filter.blur.gaussianBlur"));
    pc_engine_session_free(s);
}

/* Adversarial params never crash (Rule 9). */
static void test_hostile_params(void)
{
    pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8);
    const char *cases[] = {"null", "[]", "{\"radius\":1e308}", "{\"radius\":null}",
                           "{\"amount\":true}", "{\"seed\":99999999999999}"};
    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
        for (size_t j = 0; j < sizeof cases / sizeof cases[0]; j++) {
            pc_json_value out;
            pc_status rc = pc_engine_session_execute_json(s, ALL[i], cases[j], &out, NULL);
            if (rc == PC_OK) {
                pc_json_value_fini(&out);
            }
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_every_filter_runs_and_undoes);
    RUN(test_preset_filters_name_their_step);
    RUN(test_params_map_to_algorithm_units);
    RUN(test_validation_rejects_bad_params);
    RUN(test_last_filter_repeats);
    RUN(test_huge_offsets_clamp);
    RUN(test_disabled_without_pixel_layer);
    RUN(test_hostile_params);
    return pc_t_finish();
}
