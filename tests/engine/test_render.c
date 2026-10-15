/* Filter > Render: Flame, Tree, Picture Frame (port of render_cmds tests, E9). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *session(pc_sample_type depth, uint32_t w, uint32_t h, pc_color_mode mode)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), mode, depth,
                                      pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    pc_layer l;
    CHECK(pc_layer_raster(&l, "layer", fmt, NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &l, NULL, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static size_t opaque_px(pc_engine_session *s, uint32_t w, uint32_t h)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    const pc_surface *surf = pc_layer_surface(l);
    if (surf == NULL) {
        return 0;
    }
    float *px = NULL;
    size_t len = 0;
    if (pc_surface_read_region(surf, pc_rect_make(0, 0, (int32_t)w, (int32_t)h), &px, &len, NULL) != PC_OK) {
        return 0;
    }
    size_t ch = pc_surface_channels(surf);
    size_t n = 0;
    for (size_t i = 0; i + ch <= len; i += ch) {
        if (px[i + ch - 1] > 0.05f) {
            n++;
        }
    }
    free(px);
    return n;
}

static uint64_t prims_of(pc_json_value *out)
{
    return yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(out), "primitives"));
}
static bool bounds_is_array(pc_json_value *out)
{
    return yyjson_mut_is_arr(yyjson_mut_obj_get(pc_json_value_root(out), "bounds"));
}

static void test_each_renderer_draws_and_undoes(void)
{
    const pc_sample_type depths[] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    struct {
        const char *id;
        const char *params;
    } cases[] = {
        {"filter.render.flame", "{}"},
        {"filter.render.tree", "{\"baseTreeType\":7}"},
        {"filter.render.pictureFrame", "{\"frame\":\"roses\"}"},
    };
    for (size_t di = 0; di < 3; di++) {
        for (size_t ci = 0; ci < 3; ci++) {
            pc_engine_session *s = session(depths[di], 160, 120, PC_COLOR_MODE_RGB);
            pc_json_value out;
            pc_error err;
            pc_status rc = pc_engine_session_execute_json(s, cases[ci].id, cases[ci].params, &out, &err);
            CHECK(rc == PC_OK);
            if (rc == PC_OK) {
                CHECK(prims_of(&out) > 0);
                CHECK(bounds_is_array(&out));
                pc_json_value_fini(&out);
                CHECK(opaque_px(s, 160, 120) > 50);
                /* undo removes everything. */
                CHECK(pc_engine_session_execute_json(s, "edit.undo", "{}", &out, NULL) == PC_OK);
                pc_json_value_fini(&out);
                CHECK_EQ_UINT(opaque_px(s, 160, 120), (size_t)0);
                /* deterministic: same params draw the same count. */
                CHECK(pc_engine_session_execute_json(s, cases[ci].id, cases[ci].params, &out, NULL) == PC_OK);
                pc_json_value_fini(&out);
                size_t first = opaque_px(s, 160, 120);
                CHECK(pc_engine_session_execute_json(s, "edit.undo", "{}", &out, NULL) == PC_OK);
                pc_json_value_fini(&out);
                /* a different seed changes the result. */
                char buf[64];
                (void)snprintf(buf, sizeof buf, "{\"seed\":5%s}",
                               ci == 1 ? ",\"baseTreeType\":7" : (ci == 2 ? ",\"frame\":\"roses\"" : ""));
                CHECK(pc_engine_session_execute_json(s, cases[ci].id, buf, &out, NULL) == PC_OK);
                pc_json_value_fini(&out);
                CHECK(opaque_px(s, 160, 120) != first || first == 0);
            }
            pc_engine_session_free(s);
        }
    }
}

static void test_cmyk_gray_lab(void)
{
    const pc_color_mode modes[] = {PC_COLOR_MODE_CMYK, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_LAB};
    for (size_t i = 0; i < 3; i++) {
        pc_engine_session *s = session(PC_SAMPLE_TYPE_U8, 96, 96, modes[i]);
        pc_json_value out;
        pc_status rc = pc_engine_session_execute_json(
            s, "filter.render.pictureFrame", "{\"frame\":13,\"newLayer\":true}", &out, NULL);
        CHECK(rc == PC_OK);
        if (rc == PC_OK) {
            CHECK(bounds_is_array(&out));
            pc_json_value_fini(&out);
        }
        pc_engine_session_free(s);
    }
}

/* #2501: integer (u32) fields accept whole floats and reject hostile values, never a panic. */
static void test_integer_params_and_hostile(void)
{
    pc_engine_session *s = session(PC_SAMPLE_TYPE_U8, 160, 120, PC_COLOR_MODE_RGB);
    pc_json_value out;
    /* whole floats draw what the integers draw. */
    CHECK(pc_engine_session_execute_json(
              s, "filter.render.tree", "{\"baseTreeType\":7.0,\"lightDirection\":3.0,\"seed\":0}", &out,
              NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "edit.undo", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);

    const char *bad[] = {
        "{\"baseTreeType\":-1.0}", "{\"lightDirection\":1e300}", "{\"seed\":-0.6}",
        "{\"lines\":-3.5}",        "{\"seed\":5e9}",             "{\"lines\":\"two\"}",
    };
    const char *ids[] = {
        "filter.render.tree",         "filter.render.tree",         "filter.render.tree",
        "filter.render.pictureFrame", "filter.render.pictureFrame", "filter.render.pictureFrame",
    };
    for (size_t i = 0; i < 6; i++) {
        pc_error err;
        pc_status rc = pc_engine_session_execute_json(s, ids[i], bad[i], &out, &err);
        CHECK(rc != PC_OK);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        } else {
            CHECK_CONTAINS(err.msg, "invalid parameters");
        }
    }
    pc_engine_session_free(s);
}

static void test_no_document(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "filter.render.flame", "{}", &out, NULL) != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_each_renderer_draws_and_undoes);
    RUN(test_cmyk_gray_lab);
    RUN(test_integer_params_and_hostile);
    RUN(test_no_document);
    return pc_t_finish();
}
