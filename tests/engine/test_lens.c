/* Filter > Lens Correction / Adaptive Wide Angle / Camera Raw Filter (port of the lens_cmds.rs
 * tests, E-tail ET5). The layer is painted directly so the test needs only E1 + the algo
 * pipelines; commands are driven through the session. */
#include <stdlib.h>
#include <string.h>

#include "base/str.h"
#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

#define W 60
#define H 40

static pc_engine_session *painted(pc_sample_type depth, pc_color_mode mode)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(W, H), mode, depth,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    pc_layer l;
    CHECK(pc_layer_raster(&l, "grid", fmt, NULL) == PC_OK);
    pc_layer_id id;
    CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);
    pc_layer *lm = pc_document_layer_mut(&doc, id);
    pc_surface *surf = pc_layer_surface_mut(lm);
    size_t n = pc_surface_channels(surf);
    float *data = calloc((size_t)W * H * n, sizeof *data);
    CHECK(data != NULL);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float v = (x % 12 < 2 || y % 12 < 2) ? 0.9f : 0.35f;
            float *px = &data[((size_t)y * W + (size_t)x) * n];
            px[0] = v;
            if (n >= 3) {
                px[1] = v * 0.9f;
                px[2] = v * 0.7f;
            }
            px[n - 1] = 1.0f; /* alpha (or the last gray/cmyk channel) */
            if (!fmt.alpha && n >= 2) {
                px[n - 1] = v * 0.7f;
            }
        }
    }
    CHECK(pc_surface_write_region(surf, pc_rect_make(0, 0, W, H), data, (size_t)W * H * n, NULL) == PC_OK);
    free(data);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static float *read_active(pc_engine_session *s, size_t *len, size_t *channels)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    const pc_surface *surf = pc_layer_surface(l);
    *channels = pc_surface_channels(surf);
    float *px = NULL;
    if (pc_surface_read_region(surf, pc_rect_make(0, 0, W, H), &px, len, NULL) != PC_OK) {
        return NULL;
    }
    return px;
}

static bool any_transparent(const float *px, size_t len, size_t n)
{
    if (n == 0) {
        return false;
    }
    for (size_t i = n - 1; i < len; i += n) {
        if (px[i] < 0.5f) {
            return true;
        }
    }
    return false;
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

static double rnum(pc_json_value *out, const char *key)
{
    yyjson_mut_val *v = yyjson_mut_obj_get(pc_json_value_root(out), key);
    return yyjson_mut_get_num(v);
}

/* ---- Lens Correction ---------------------------------------------------------------------- */

static void test_lens_runs_undoes_and_resolves_profiles(void)
{
    pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t k = 0; k < 3; k++) {
        pc_engine_session *s = painted(depths[k], PC_COLOR_MODE_RGB);
        CHECK(pc_engine_session_is_enabled(s, "filter.lensCorrection"));
        size_t n0 = 0, c0 = 0;
        float *before = read_active(s, &n0, &c0);
        CHECK(before != NULL);
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "filter.lensCorrection",
                                             "{\"distortion\":-40,\"vignetteAmount\":-50}", &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        size_t n1 = 0, c1 = 0;
        float *after = read_active(s, &n1, &c1);
        CHECK(after != NULL);
        CHECK(any_transparent(after, n1, c1)); /* transparency edge clears the border */
        free(after);
        CHECK(pc_engine_session_execute_json(s, "edit.undo", "{}", &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        size_t n2 = 0, c2 = 0;
        float *undone = read_active(s, &n2, &c2);
        CHECK(undone != NULL && n2 == n0 && !differs(before, undone, n0));
        free(before);
        free(undone);
        pc_engine_session_free(s);
    }

    /* Auto profile falls back to focalLength when no EXIF is present. */
    pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8, PC_COLOR_MODE_RGB);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "filter.lensCorrection",
              "{\"profile\":\"auto\",\"focalLength\":24,\"autoScale\":true,\"edge\":\"edgeExtension\"}",
              &out, NULL) == PC_OK);
    yyjson_mut_val *params = yyjson_mut_obj_get(pc_json_value_root(&out), "params");
    CHECK(params != NULL);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(params, "profile")), "generic");
    CHECK_NEAR(yyjson_mut_get_num(yyjson_mut_obj_get(params, "focalLength")), 24.0, 1e-9);
    CHECK(rnum(&out, "scale") >= 100.0);
    pc_json_value_fini(&out);

    /* The Straighten tool adds the measured angle. */
    CHECK(pc_engine_session_execute_json(s, "filter.lensCorrection", "{\"straighten\":[[0,0],[100,5]]}",
                                         &out, NULL) == PC_OK);
    CHECK_NEAR(rnum(&out, "angle"), -2.862, 0.01);
    pc_json_value_fini(&out);

    /* Bad edge / profile are rejected. */
    CHECK(pc_engine_session_execute_json(s, "filter.lensCorrection", "{\"edge\":\"rainbow\"}", &out, NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "filter.lensCorrection", "{\"profile\":\"zeiss\"}", &out, NULL) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "filter.lensCorrection", "{\"straighten\":[[0,0]]}", &out, NULL) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- Adaptive Wide Angle ------------------------------------------------------------------ */

static void test_wide_angle_straightens(void)
{
    pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8, PC_COLOR_MODE_RGB);
    CHECK(pc_engine_session_is_enabled(s, "filter.adaptiveWideAngle"));
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "filter.adaptiveWideAngle",
              "{\"model\":\"fisheye\",\"focalLength\":10,"
              "\"constraints\":[{\"a\":[10,15],\"b\":[50,15],\"orientation\":\"horizontal\"}]}",
              &out, NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&out), "model")), "fisheye");
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "constraints")), 1u);
    pc_json_value_fini(&out);
    size_t n = 0, c = 0;
    float *after = read_active(s, &n, &c);
    CHECK(after != NULL && any_transparent(after, n, c)); /* outside the warped image is transparent */
    free(after);

    /* A constraint missing its second point is rejected. */
    CHECK(pc_engine_session_execute_json(s, "filter.adaptiveWideAngle", "{\"constraints\":[{\"a\":[0,0]}]}",
                                         &out, NULL) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- Camera Raw Filter -------------------------------------------------------------------- */

static void test_camera_raw_on_layers_and_selection(void)
{
    pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    for (size_t k = 0; k < 3; k++) {
        pc_engine_session *s = painted(depths[k], PC_COLOR_MODE_RGB);
        CHECK(pc_engine_session_is_enabled(s, "filter.cameraRaw"));
        size_t n0 = 0, c0 = 0;
        float *before = read_active(s, &n0, &c0);
        CHECK(before != NULL);
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(
                  s, "filter.cameraRaw", "{\"exposure\":1.0,\"vibrance\":30,\"clarity\":20}", &out,
                  NULL) == PC_OK);
        CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "identity")) == false);
        pc_json_value_fini(&out);
        size_t n1 = 0, c1 = 0;
        float *after = read_active(s, &n1, &c1);
        CHECK(after != NULL && n1 == n0 && differs(before, after, n0));
        free(before);
        free(after);
        pc_engine_session_free(s);
    }

    /* A selection limits the filter: inside changes, outside is untouched. */
    pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8, PC_COLOR_MODE_RGB);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":24,\"height\":40}",
                                         &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    size_t nb = 0, cb = 0;
    float *before = read_active(s, &nb, &cb);
    CHECK(before != NULL);
    CHECK(pc_engine_session_execute_json(s, "filter.cameraRaw", "{\"exposure\":-1.0}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    size_t na = 0, ca = 0;
    float *after = read_active(s, &na, &ca);
    CHECK(after != NULL && na == nb && ca == cb);
    size_t n = ca;
    /* pixel (10,40? -> within) inside selection differs; (50, y) outside is identical. */
    size_t inside = ((size_t)20 * W + 10) * n;
    size_t outside = ((size_t)20 * W + 50) * n;
    CHECK(before[inside] != after[inside]);
    CHECK(before[outside] == after[outside]);
    free(before);
    free(after);

    /* Bad params are rejected. */
    CHECK(pc_engine_session_execute_json(s, "filter.cameraRaw", "{\"exposure\":\"bright\"}", &out, NULL) != PC_OK);
    pc_engine_session_free(s);
}

static void test_camera_raw_requires_rgb_or_gray(void)
{
    pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8, PC_COLOR_MODE_CMYK);
    CHECK(!pc_engine_session_is_enabled(s, "filter.cameraRaw"));
    pc_engine_session_free(s);
    /* Grayscale is allowed. */
    s = painted(PC_SAMPLE_TYPE_U8, PC_COLOR_MODE_GRAYSCALE);
    CHECK(pc_engine_session_is_enabled(s, "filter.cameraRaw"));
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "filter.cameraRaw", "{\"exposure\":0.5}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_camera_raw_decodes_with_document_curve(void)
{
    /* Assigning ProPhoto (gamma 1.8) still develops; the encoding-gamma resolver runs. */
    pc_engine_session *s = painted(PC_SAMPLE_TYPE_U16, PC_COLOR_MODE_RGB);
    pc_json_value out;
    pc_status rc =
        pc_engine_session_execute_json(s, "edit.assignProfile", "{\"profile\":\"prophoto-compat\"}", &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
        CHECK(pc_engine_session_execute_json(s, "filter.cameraRaw", "{\"shadows\":40,\"whites\":-30}",
                                             &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
    }
    pc_engine_session_free(s);
}

/* ---- the re-render subtype hook (pc_engine_apply_filter_to_surface -> lens_apply_to_surface) --- */

static void make_surface(pc_surface *s)
{
    pc_surface_init(s, PC_PIXEL_FORMAT_RGBA8);
    size_t n = pc_surface_channels(s);
    float *data = calloc((size_t)W * H * n, sizeof *data);
    CHECK(data != NULL);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float *px = &data[((size_t)y * W + (size_t)x) * n];
            float v = (float)((x * 7 + y * 11) % 13) / 12.0f;
            px[0] = v;
            px[1] = 1.0f - v;
            px[2] = (float)(x % 5) / 4.0f;
            px[3] = 1.0f;
        }
    }
    CHECK(pc_surface_write_region(s, pc_rect_make(0, 0, W, H), data, (size_t)W * H * n, NULL) == PC_OK);
    free(data);
}

static void apply_case(const char *id, const char *json, bool expect_applied)
{
    pc_surface src;
    make_surface(&src);
    pc_rect area = pc_rect_make(0, 0, W, H);
    yyjson_doc *pd = NULL;
    CHECK(pc_json_parse(json, strlen(json), &pd, NULL) == PC_OK);
    pc_surface out;
    bool applied = false;
    pc_error e;
    pc_error_clear(&e);
    pc_status rc = pc_engine_apply_filter_to_surface(id, yyjson_doc_get_root(pd), &src, area, NULL, area,
                                                     &out, &applied, &e);
    CHECK(rc == PC_OK);
    CHECK(applied == expect_applied);
    if (applied) {
        pc_surface_fini(&out);
    }
    yyjson_doc_free(pd);
    pc_surface_fini(&src);
}

static void test_subtype_hook_rerenders_lens_wide_raw(void)
{
    apply_case("filter.lensCorrection", "{\"distortion\":30,\"profile\":\"generic\",\"focalLength\":35}", true);
    apply_case("filter.adaptiveWideAngle", "{\"model\":\"perspective\",\"scale\":90}", true);
    apply_case("filter.cameraRaw", "{\"exposure\":1,\"saturation\":-50}", true);
    /* A legacy point curve an older editor saved must still re-apply (lenient re-render). */
    apply_case("filter.cameraRaw", "{\"pointCurve\":[[0,0],[60,40],[60,200],[255,255]],\"exposure\":1}", true);
}

/* ---- adversarial params never crash (Rule 9) ---------------------------------------------- */

static void test_hostile_params(void)
{
    const char *ids[] = {"filter.lensCorrection", "filter.adaptiveWideAngle", "filter.cameraRaw"};
    const char *cases[] = {
        "null", "[]", "42", "\"x\"",
        "{\"distortion\":1e308}", "{\"scale\":-1e9}", "{\"angle\":null}",
        "{\"focalLength\":-5}", "{\"constraints\":[5,null]}", "{\"constraints\":42}",
        "{\"exposure\":1e308}", "{\"seed\":99999999999999}", "{\"vignetteStyle\":true}",
        "{\"straighten\":\"nope\"}", "{\"edge\":42}", "{\"profile\":99}",
        "{\"pointCurve\":[[0,0]]}", "{\"hslHue\":[1,2,3]}",
    };
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        pc_engine_session *s = painted(PC_SAMPLE_TYPE_U8, PC_COLOR_MODE_RGB);
        for (size_t j = 0; j < sizeof cases / sizeof cases[0]; j++) {
            pc_json_value out;
            pc_status rc = pc_engine_session_execute_json(s, ids[i], cases[j], &out, NULL);
            if (rc == PC_OK) {
                pc_json_value_fini(&out);
            }
        }
        pc_engine_session_free(s);
    }
}

int main(int argc, char **argv)
{
    RUN(test_lens_runs_undoes_and_resolves_profiles);
    RUN(test_wide_angle_straightens);
    RUN(test_camera_raw_on_layers_and_selection);
    RUN(test_camera_raw_requires_rgb_or_gray);
    RUN(test_camera_raw_decodes_with_document_curve);
    RUN(test_subtype_hook_rerenders_lens_wide_raw);
    RUN(test_hostile_params);
    return pc_t_finish();
}
