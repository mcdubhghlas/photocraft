/* Image > Mode palette / ink modes (ported from the reachable parts of
 * crates/engine/src/mode_cmds/tests): Indexed Color, Color Table, Bitmap, Duotone. Each registered
 * command gets an adversarial-params sweep (REWRITE_PLAN rule 9). Image Rotation › Arbitrary is
 * deferred to E4 (transform_cmds) and is NOT registered, so it is not tested here. */
#include <math.h>
#include <string.h>

#include "color/color.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "raster/surface.h"
#include "engine/engine_internal.h"
#include "harness.h"

static const pc_document *doc_of(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d != NULL ? pc_doc_arc_get(d->doc) : NULL;
}

/* Fill the active (background) surface with a deterministic multicolour pattern. */
static pc_status paint_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *ctx, pc_error *err)
{
    (void)ha;
    (void)ctx;
    pc_layer *l = pc_document_layer_mut(doc, *active);
    if (l == NULL) {
        l = &doc->layers[0];
    }
    pc_surface *surf = pc_layer_surface_mut(l);
    if (surf == NULL) {
        return PC_OK;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    pc_rect b = pc_document_bounds(doc);
    size_t w = pc_rect_width(b), h = pc_rect_height(b);
    size_t nch = pc_pixel_format_channels(fmt);
    float *data = malloc(w * h * nch * sizeof(float));
    if (data == NULL) {
        return PC_ERR_NOMEM;
    }
    size_t idx = 0;
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            float rgba[4] = {(float)((x * 37 + y * 11) % 256) / 255.0f,
                             (float)((x * 5 + y * 53) % 256) / 255.0f,
                             (float)((x * 19 + y * 7) % 256) / 255.0f, 1.0f};
            float enc[PC_RASTER_MAX_CHANNELS];
            size_t m = pc_from_rgba(fmt, rgba, enc);
            memcpy(&data[idx], enc, m * sizeof(float));
            idx += nch;
        }
    }
    pc_status rc = pc_surface_write_region(surf, b, data, w * h * nch, err);
    free(data);
    return rc;
}

static pc_engine_session *session(const char *mode)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char buf[128];
    snprintf(buf, sizeof buf, "{\"width\":16,\"height\":16,\"mode\":\"%s\",\"background\":\"white\"}",
             mode);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "file.new", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_edit(s, "paint", paint_edit, NULL, NULL) == PC_OK);
    return s;
}

/* ---- Indexed Color ------------------------------------------------------------------------ */

static void test_indexed(void)
{
    pc_engine_session *s = session("rgb");
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "image.mode.indexedColor", "{\"palette\":\"adaptive\",\"colors\":16}", &out, NULL) ==
          PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    uint64_t colors = yyjson_mut_get_uint(yyjson_mut_obj_get(root, "colors"));
    CHECK(colors >= 1 && colors <= 256);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->mode == PC_COLOR_MODE_INDEXED);
    CHECK(doc_of(s)->has_color_table);
    CHECK_EQ_UINT(doc_of(s)->nlayers, 1);
    pc_engine_session_free(s);
}

static void test_color_table(void)
{
    pc_engine_session *s = session("rgb");
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.mode.indexedColor", "{\"colors\":8}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    /* Replace entry 0 with red and mark it transparent. */
    CHECK(pc_engine_session_execute_json(
              s, "image.mode.colorTable", "{\"entries\":{\"0\":\"#ff0000\"},\"transparent\":0}", &out,
              NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(yyjson_mut_is_arr(yyjson_mut_obj_get(root, "colors")));
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "transparent")), 0);
    pc_json_value_fini(&out);
    /* A grayscale preset table also applies. */
    CHECK(pc_engine_session_execute_json(s, "image.mode.colorTable", "{\"table\":\"grayscale\"}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    /* Unknown preset is an error. */
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "image.mode.colorTable", "{\"table\":\"nope\"}", &out,
                                         &err) != PC_OK);
    pc_engine_session_free(s);
}

/* ---- Bitmap ------------------------------------------------------------------------------- */

static void test_bitmap(void)
{
    const char *methods[] = {"threshold", "pattern", "diffusion", "halftone"};
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        pc_engine_session *s = session("grayscale");
        char p[96];
        snprintf(p, sizeof p, "{\"method\":\"%s\"}", methods[i]);
        pc_json_value out;
        CHECK(pc_engine_session_execute_json(s, "image.mode.bitmap", p, &out, NULL) == PC_OK);
        pc_json_value_fini(&out);
        CHECK(doc_of(s)->mode == PC_COLOR_MODE_BITMAP);
        CHECK_EQ_UINT(doc_of(s)->nlayers, 1);
        pc_engine_session_free(s);
    }
    /* Bitmap needs a grayscale source: refused on an RGB document (enabled predicate). */
    pc_engine_session *rgb = session("rgb");
    CHECK(!pc_engine_session_is_enabled(rgb, "image.mode.bitmap"));
    pc_engine_session_free(rgb);
    /* A halftone angle outside -180..180 is a bad-params error. */
    pc_engine_session *s = session("grayscale");
    pc_error err = {0};
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "image.mode.bitmap", "{\"method\":\"halftone\",\"angle\":200}", &out, &err) != PC_OK);
    CHECK(strstr(err.msg, "-180") != NULL);
    pc_engine_session_free(s);
}

/* ---- Duotone ------------------------------------------------------------------------------ */

static void test_duotone(void)
{
    pc_engine_session *s = session("grayscale");
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(
              s, "image.mode.duotone",
              "{\"type\":\"tritone\",\"inks\":[{\"name\":\"K\"},{\"color\":\"#ff0000\"}]}", &out,
              NULL) == PC_OK);
    yyjson_mut_val *inks = yyjson_mut_obj_get(pc_json_value_root(&out), "inks");
    CHECK(yyjson_mut_is_arr(inks));
    CHECK_EQ_UINT(yyjson_mut_arr_size(inks), 3);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->mode == PC_COLOR_MODE_DUOTONE);
    CHECK(doc_of(s)->has_duotone);
    CHECK_EQ_UINT(doc_of(s)->duotone.ninks, 3);
    /* An unknown type is a bad-params error. */
    pc_engine_session *g = session("grayscale");
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(g, "image.mode.duotone", "{\"type\":\"pentatone\"}", &out,
                                         &err) != PC_OK);
    pc_engine_session_free(g);
    pc_engine_session_free(s);
}

/* ---- adversarial params ------------------------------------------------------------------- */

static void run_adversarial(const char *id, const char *mode)
{
    const char *params[] = {
        "{}",
        "null",
        "{\"palette\":42,\"colors\":-5,\"forced\":true,\"dither\":[]}",
        "{\"colors\":1000,\"amount\":\"x\"}",
        "{\"method\":99,\"frequency\":-1,\"angle\":\"NaN\",\"shape\":{}}",
        "{\"type\":5,\"inks\":\"nope\"}",
        "{\"table\":[],\"colors\":[1,2],\"entries\":5,\"transparent\":-7}",
        "{\"inks\":[{\"curve\":[[\"a\",\"b\"]]}]}",
    };
    for (size_t i = 0; i < sizeof params / sizeof params[0]; i++) {
        pc_engine_session *s = session(mode);
        pc_json_value out;
        pc_error err = {0};
        pc_status rc = pc_engine_session_execute_json(s, id, params[i], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
        pc_engine_session_free(s);
        /* No document: never crashes. */
        pc_engine_session *empty = NULL;
        CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
        err = (pc_error){0};
        rc = pc_engine_session_execute_json(empty, id, params[i], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
        pc_engine_session_free(empty);
    }
}

static void test_adversarial(void)
{
    CHECK(pc_engine_command_find("image.mode.indexedColor") != NULL);
    CHECK(pc_engine_command_find("image.mode.colorTable") != NULL);
    CHECK(pc_engine_command_find("image.mode.bitmap") != NULL);
    CHECK(pc_engine_command_find("image.mode.duotone") != NULL);
    /* image.rotation.arbitrary is now wired (ET3). */
    CHECK(pc_engine_command_find("image.rotation.arbitrary") != NULL);
    run_adversarial("image.mode.indexedColor", "rgb");
    run_adversarial("image.mode.colorTable", "rgb");
    run_adversarial("image.mode.bitmap", "grayscale");
    run_adversarial("image.mode.duotone", "grayscale");
    run_adversarial("image.rotation.arbitrary", "rgb");
}

int main(int argc, char **argv)
{
    RUN(test_indexed);
    RUN(test_color_table);
    RUN(test_bitmap);
    RUN(test_duotone);
    RUN(test_adversarial);
    return pc_t_finish();
}
