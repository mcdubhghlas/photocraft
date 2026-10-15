/* Image > Mode > Multichannel and the conversion back out (ported from the reachable parts of
 * crates/engine/src/multichannel_cmds/tests.rs; the Rust tests that drive image.mode.rgb/cmyk/...,
 * duotone, edit.fill, PSD/.pcraft round trips need chunks E4/E5/E8 and io/format, so here the
 * conversion back is called through the internal pc_engine_multichannel_convert_from). */
#include <math.h>
#include <string.h>

#include "color/color.h"
#include "doc/doc.h"
#include "raster/surface.h"
#include "engine/engine_internal.h"
#include "harness.h"

static float tol(int depth)
{
    return depth == 8 ? 0.5f / 255.0f + 1e-6f : 1e-4f;
}

/* Build a `mode`/`depth` document with a deterministic pattern, like the Rust session() helper. */
static pc_status setup_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ha;
    (void)ctx;
    pc_rect b = pc_document_bounds(doc);
    pc_layer *l = pc_document_layer_mut(doc, *active);
    CHECK(l != NULL);
    pc_surface *surf = pc_layer_surface_mut(l);
    CHECK(surf != NULL);
    size_t ch = pc_surface_channels(surf);
    size_t w = pc_rect_width(b), h = pc_rect_height(b);
    float *data = malloc(w * h * ch * sizeof(float));
    CHECK(data != NULL);
    size_t idx = 0;
    for (int32_t y = b.y0; y < b.y1; y++) {
        for (int32_t x = b.x0; x < b.x1; x++) {
            for (size_t c = 0; c + 1 < ch; c++) {
                data[idx++] = (float)(((x * 3 + y * 5 + (int32_t)c * 7) % 16)) / 15.0f;
            }
            data[idx++] = 1.0f;
        }
    }
    pc_status rc = pc_surface_write_region(surf, b, data, w * h * ch, err);
    free(data);
    return rc;
}

static pc_engine_session *session(const char *mode, int depth)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char buf[128];
    snprintf(buf, sizeof buf, "{\"width\":12,\"height\":8,\"depth\":%d,\"mode\":\"%s\"}", depth, mode);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.new", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_edit(s, "setup", setup_edit, NULL, NULL) == PC_OK);
    return s;
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

/* Read the active layer's pixels over the canvas (newly allocated). */
static float *read_pixels(pc_engine_session *s, size_t *len)
{
    const pc_document *d = doc_of(s);
    const pc_surface *surf = pc_layer_surface(&d->layers[0]);
    float *data = NULL;
    CHECK(pc_surface_read_region(surf, pc_document_bounds(d), &data, len, NULL) == PC_OK);
    return data;
}

static void channels_are(pc_json_value *r, const char *const *want, size_t n)
{
    yyjson_mut_val *arr = yyjson_mut_obj_get(pc_json_value_root(r), "channels");
    CHECK(arr != NULL && yyjson_mut_arr_size(arr) == n);
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_arr_get(arr, i)), want[i]);
    }
}

static void test_rgb_becomes_cmy_and_back(void)
{
    int depths[] = {8, 16, 32};
    for (size_t di = 0; di < 3; di++) {
        int depth = depths[di];
        pc_engine_session *s = session("rgb", depth);
        size_t blen = 0;
        float *before = read_pixels(s, &blen);

        CHECK(pc_engine_session_is_enabled(s, "image.mode.multichannel"));
        pc_json_value r;
        CHECK(pc_engine_session_execute_json(s, "image.mode.multichannel", "{}", &r, NULL) == PC_OK);
        const char *want[] = {"Cyan", "Magenta", "Yellow"};
        channels_are(&r, want, 3);
        pc_json_value_fini(&r);

        const pc_document *d = doc_of(s);
        CHECK(d->mode == PC_COLOR_MODE_MULTICHANNEL);
        CHECK(d->nlayers == 0);
        CHECK(d->nchannels == 3 && d->channels[0].has_spot);
        /* ink_names (used by inspect, E7) returns the spot-channel names. */
        char **names = NULL;
        size_t nnames = 0;
        CHECK(pc_engine_multichannel_ink_names(d, &names, &nnames, NULL) == PC_OK);
        CHECK(nnames == 3);
        if (nnames == 3) {
            CHECK_EQ_STR(names[0], "Cyan");
            CHECK_EQ_STR(names[1], "Magenta");
            CHECK_EQ_STR(names[2], "Yellow");
        }
        for (size_t i = 0; i < nnames; i++) {
            free(names[i]);
        }
        free(names);
        /* Cyan ink = the red data read as ink (1 - red) at (4,3). */
        float cyan[4];
        pc_surface_rgba(&d->channels[0].surface, 4, 3, cyan);
        float red = before[(3 * 12 + 4) * 4];
        CHECK(fabsf(cyan[0] - (1.0f - red)) <= tol(depth));
        CHECK(!pc_engine_session_is_enabled(s, "image.mode.multichannel"));

        /* Back to RGB through the internal conversion (E8 color_cmds will call this). */
        pc_json_value cr;
        CHECK(pc_engine_multichannel_convert_from(s, PC_COLOR_MODE_RGB, NULL, &cr, NULL) == PC_OK);
        CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&cr), "converted")),
                     "channels");
        pc_json_value_fini(&cr);
        CHECK(doc_of(s)->mode == PC_COLOR_MODE_RGB && doc_of(s)->nchannels == 0);

        size_t alen = 0;
        float *after = read_pixels(s, &alen);
        CHECK(alen == blen);
        for (size_t i = 0; i < alen && i < blen; i++) {
            CHECK(fabsf(after[i] - before[i]) <= 2.0f * tol(depth));
        }
        free(after);
        free(before);
        pc_engine_session_free(s);
    }
}

static void test_cmyk_lab_gray_sets(void)
{
    struct {
        const char *mode;
        pc_color_mode back;
        const char *names[4];
        size_t n;
    } cases[] = {
        {"cmyk", PC_COLOR_MODE_CMYK, {"Cyan", "Magenta", "Yellow", "Black"}, 4},
        {"lab", PC_COLOR_MODE_LAB, {"Alpha 1", "Alpha 2", "Alpha 3", NULL}, 3},
        {"grayscale", PC_COLOR_MODE_GRAYSCALE, {"Black", NULL, NULL, NULL}, 1},
    };
    for (size_t ci = 0; ci < 3; ci++) {
        for (int depth = 8; depth <= 16; depth += 8) {
            pc_engine_session *s = session(cases[ci].mode, depth);
            size_t blen = 0;
            float *before = read_pixels(s, &blen);
            pc_json_value r;
            CHECK(pc_engine_session_execute_json(s, "image.mode.multichannel", "{}", &r, NULL) == PC_OK);
            channels_are(&r, cases[ci].names, cases[ci].n);
            pc_json_value_fini(&r);
            CHECK(doc_of(s)->depth == (depth == 8 ? PC_SAMPLE_TYPE_U8 : PC_SAMPLE_TYPE_U16));
            pc_json_value cr;
            CHECK(pc_engine_multichannel_convert_from(s, cases[ci].back, NULL, &cr, NULL) == PC_OK);
            pc_json_value_fini(&cr);
            size_t alen = 0;
            float *after = read_pixels(s, &alen);
            CHECK(alen == blen);
            for (size_t i = 0; i < alen && i < blen; i++) {
                CHECK(fabsf(after[i] - before[i]) <= 2.0f * tol(depth));
            }
            free(after);
            free(before);
            pc_engine_session_free(s);
        }
    }
}

/* An existing alpha channel survives the conversion and stays an alpha channel. */
static pc_status add_alpha(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ha;
    (void)active;
    (void)ctx;
    pc_pixel_format f = {PC_COLOR_MODE_GRAYSCALE, doc->depth, false};
    pc_surface surf;
    pc_surface_init(&surf, f);
    pc_alpha_channel ch;
    pc_status rc = pc_alpha_channel_init(&ch, "Alpha 1", &surf, err);
    if (rc != PC_OK) {
        pc_surface_fini(&surf);
        return rc;
    }
    pc_alpha_channel *grown = realloc(doc->channels, (doc->nchannels + 1) * sizeof(pc_alpha_channel));
    if (grown == NULL) {
        pc_alpha_channel_fini(&ch);
        return pc_fail(err, PC_ERR_NOMEM, "test");
    }
    doc->channels = grown;
    doc->channels[doc->nchannels++] = ch;
    return PC_OK;
}

static void test_keeps_alpha_channels(void)
{
    pc_engine_session *s = session("rgb", 8);
    CHECK(pc_engine_session_edit(s, "add alpha", add_alpha, NULL, NULL) == PC_OK);
    CHECK(doc_of(s)->nchannels == 1);
    pc_json_value r;
    CHECK(pc_engine_session_execute_json(s, "image.mode.multichannel", "{}", &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    const pc_document *d = doc_of(s);
    CHECK(d->nchannels == 4);
    CHECK(!d->channels[3].has_spot); /* the saved selection stays an alpha channel */
    CHECK(d->channels[0].has_spot && d->channels[1].has_spot && d->channels[2].has_spot);
    pc_engine_session_free(s);
}

/* With a channel count that can't be reinterpreted, the printed look is converted. */
static pc_status drop_to_two(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ha;
    (void)active;
    (void)ctx;
    (void)err;
    for (size_t j = 2; j < doc->nchannels; j++) {
        pc_alpha_channel_fini(&doc->channels[j]);
    }
    doc->nchannels = doc->nchannels > 2 ? 2 : doc->nchannels;
    return PC_OK;
}

static void test_mismatch_converts_appearance(void)
{
    pc_engine_session *s = session("rgb", 8);
    pc_json_value r;
    CHECK(pc_engine_session_execute_json(s, "image.mode.multichannel", "{}", &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    CHECK(pc_engine_session_edit(s, "drop", drop_to_two, NULL, NULL) == PC_OK);
    CHECK(doc_of(s)->nchannels == 2);
    pc_json_value cr;
    CHECK(pc_engine_multichannel_convert_from(s, PC_COLOR_MODE_RGB, NULL, &cr, NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(&cr), "converted")),
                 "appearance");
    pc_json_value_fini(&cr);
    CHECK(doc_of(s)->mode == PC_COLOR_MODE_RGB && doc_of(s)->nlayers == 1);
    pc_engine_session_free(s);
}

static void test_enabled_only_for_source_modes(void)
{
    pc_engine_session *s = session("rgb", 8);
    pc_json_value r;
    CHECK(pc_engine_session_execute_json(s, "image.mode.multichannel", "{}", &r, NULL) == PC_OK);
    pc_json_value_fini(&r);
    /* Already multichannel: disabled, with a readable reason. */
    pc_error why = {0};
    CHECK(pc_engine_session_disabled_reason(s, "image.mode.multichannel", &why));
    CHECK(strstr(why.msg, "Multichannel needs") != NULL);
    pc_engine_session_free(s);
    /* No document: disabled. */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(empty, "image.mode.multichannel"));
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    RUN(test_rgb_becomes_cmy_and_back);
    RUN(test_cmyk_lab_gray_sets);
    RUN(test_keeps_alpha_channels);
    RUN(test_mismatch_converts_appearance);
    RUN(test_enabled_only_for_source_modes);
    return pc_t_finish();
}
