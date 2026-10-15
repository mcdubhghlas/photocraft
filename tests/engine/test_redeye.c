/* Red Eye tool (port of a subset of redeye_cmds tests, E10). */
#include <math.h>
#include <stdlib.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "raster/pixel.h"
#include "raster/surface.h"
#include "harness.h"

static pc_engine_session *mk(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "U", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

struct discctx {
    int cx, cy, r;
};

static pc_status disc_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *v, pc_error *e)
{
    (void)ha;
    struct discctx *c = v;
    pc_surface *surf = pc_layer_surface_mut(pc_document_layer_mut(doc, *a));
    CHECK(surf != NULL);
    pc_pixel_format fmt = pc_surface_format(surf);
    pc_rect b = pc_document_bounds(doc);
    size_t ch = pc_surface_channels(surf);
    size_t w = pc_rect_width(b), h = pc_rect_height(b);
    float *data = calloc(w * h * ch, sizeof(float));
    CHECK(data != NULL);
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            float dx = (float)((int)x - c->cx), dy = (float)((int)y - c->cy);
            float d = sqrtf(dx * dx + dy * dy);
            float rgba[4];
            if (d <= 1.2f) {
                rgba[0] = rgba[1] = rgba[2] = rgba[3] = 1.0f;
            } else if (d <= (float)c->r) {
                rgba[0] = 0.95f;
                rgba[1] = 0.10f;
                rgba[2] = 0.10f;
                rgba[3] = 1.0f;
            } else {
                rgba[0] = 0.38f;
                rgba[1] = 0.28f;
                rgba[2] = 0.16f;
                rgba[3] = 1.0f;
            }
            float tmp[PC_RASTER_MAX_CHANNELS];
            size_t n = pc_from_rgba(fmt, rgba, tmp);
            for (size_t cc = 0; cc < ch && cc < n; cc++) {
                data[(y * w + x) * ch + cc] = tmp[cc];
            }
        }
    }
    pc_status rc = pc_surface_write_region(surf, b, data, w * h * ch, e);
    free(data);
    return rc;
}

static void rgba(pc_engine_session *s, int x, int y, float out[4])
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_surface_rgba(pc_layer_surface(pc_document_layer(doc, d->active_layer)), x, y, out);
}

static pc_status rn(pc_engine_session *s, const char *json, pc_json_value *out, pc_error *err)
{
    return pc_engine_session_execute_json(s, "paint.redEye", json, out, err);
}

static void test_redeye_corrects_and_undoes(void)
{
    pc_engine_session *s = mk(64, 64);
    struct discctx c = {32, 32, 8};
    CHECK(pc_engine_session_edit(s, "setup", disc_edit, &c, NULL) == PC_OK);
    float before[4];
    rgba(s, 32, 26, before);
    CHECK(before[0] > 0.8f);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    CHECK_EQ_STR(rn(s, "{\"x\":32,\"y\":32}", &out, &e) == PC_OK ? "ok" : e.msg, "ok");
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(root, "pixels")) > 0);
    pc_json_value_fini(&out);
    float after[4];
    rgba(s, 32, 26, after);
    CHECK(after[0] < before[0] - 0.2f);
    float catch_[4];
    rgba(s, 32, 32, catch_);
    CHECK(catch_[0] >= 0.9f && catch_[1] >= 0.9f);
    CHECK(pc_engine_session_undo(s));
    float restored[4];
    rgba(s, 32, 26, restored);
    for (int i = 0; i < 3; i++) {
        CHECK(fabsf(restored[i] - before[i]) < 0.02f);
    }
    pc_engine_session_free(s);
}

static void test_redeye_errors(void)
{
    pc_engine_session *s = mk(32, 32);
    struct discctx c = {100, 100, 1}; /* no red near center */
    CHECK(pc_engine_session_edit(s, "setup", disc_edit, &c, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_error e = {0};
    /* a miss is an error */
    CHECK(rn(s, "{\"x\":16,\"y\":16}", &out, &e) != PC_OK);
    pc_json_value_fini(&out);
    CHECK(rn(s, "{}", &out, &e) != PC_OK);
    pc_json_value_fini(&out);
    CHECK(rn(s, "{\"x\":null,\"y\":1}", &out, &e) != PC_OK);
    pc_json_value_fini(&out);
    CHECK(rn(s, "{\"points\":[]}", &out, &e) != PC_OK);
    pc_json_value_fini(&out);
    /* a huge pupil window is rejected */
    CHECK(rn(s, "{\"x\":8,\"y\":8,\"pupilSize\":10000}", &out, &e) != PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_redeye_corrects_and_undoes);
    RUN(test_redeye_errors);
    return pc_t_finish();
}
