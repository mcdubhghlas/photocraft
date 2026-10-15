/* View > layer mask (port of mask_view_cmds tests). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "engine/layer_cmds.h"
#include "harness.h"

/* Background, M (with a reveal-all layer mask), P (plain). */
static pc_engine_session *build(pc_layer_id *m, pc_layer_id *pl)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(16, 16), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    pc_layer lm;
    CHECK(pc_layer_raster(&lm, "Masked", fmt, NULL) == PC_OK);
    lm.has_mask = true;
    pc_layer_mask_reveal_all(&lm.mask);
    CHECK(pc_document_insert_above(&doc, NULL, &lm, m, NULL) == PC_OK);
    pc_layer lp;
    CHECK(pc_layer_raster(&lp, "Plain", fmt, NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &lp, pl, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

/* The document.inspect "layerMaskView" mode, or NULL when off. Writes into mode_buf. */
static const char *inspect_mode(pc_engine_session *s, char *mode_buf, size_t cap, uint64_t *layer)
{
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "document.inspect", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *v = yyjson_mut_obj_get(pc_json_value_root(&out), "layerMaskView");
    const char *mode = NULL;
    if (v != NULL && !yyjson_mut_is_null(v)) {
        const char *mstr = yyjson_mut_get_str(yyjson_mut_obj_get(v, "mode"));
        (void)snprintf(mode_buf, cap, "%s", mstr != NULL ? mstr : "?");
        mode = mode_buf;
        if (layer != NULL) {
            *layer = yyjson_mut_get_uint(yyjson_mut_obj_get(v, "layer"));
        }
    }
    pc_json_value_fini(&out);
    return mode;
}

static const char *result_mode(pc_json_value *out)
{
    const char *m = yyjson_mut_get_str(yyjson_mut_obj_get(pc_json_value_root(out), "mode"));
    return m != NULL ? m : "?";
}

static void test_gray_overlay_and_off(void)
{
    pc_layer_id m, p;
    pc_engine_session *s = build(&m, &p);
    CHECK(pc_engine_session_select_layer(s, m, NULL) == PC_OK);
    char buf[16];
    uint64_t lid = 0;

    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "view.layerMask", "{\"mode\":\"gray\"}", &out, NULL) == PC_OK);
    CHECK_EQ_STR(result_mode(&out), "gray");
    pc_json_value_fini(&out);
    CHECK_EQ_STR(inspect_mode(s, buf, sizeof buf, &lid), "gray");
    CHECK(lid == m);

    CHECK(pc_engine_session_execute_json(s, "view.layerMask", "{\"mode\":\"overlay\"}", &out, NULL) == PC_OK);
    CHECK_EQ_STR(result_mode(&out), "overlay");
    pc_json_value_fini(&out);
    CHECK_EQ_STR(inspect_mode(s, buf, sizeof buf, NULL), "overlay");

    CHECK(pc_engine_session_execute_json(s, "view.layerMask", "{\"mode\":\"off\"}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(inspect_mode(s, buf, sizeof buf, NULL) == NULL); /* off */
    pc_engine_session_free(s);
}

static void test_toggle_default_and_overlay_command(void)
{
    pc_layer_id m, p;
    pc_engine_session *s = build(&m, &p);
    CHECK(pc_engine_session_select_layer(s, m, NULL) == PC_OK);
    char buf[16];
    pc_json_value out;
    /* Default mode is toggleGray. */
    CHECK(pc_engine_session_execute_json(s, "view.layerMask", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_STR(result_mode(&out), "gray");
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "view.layerMask", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_STR(result_mode(&out), "off");
    pc_json_value_fini(&out);
    CHECK(inspect_mode(s, buf, sizeof buf, NULL) == NULL);

    /* The `\` overlay toggle. */
    CHECK(pc_engine_session_is_enabled(s, "layer.toggleMaskOverlay"));
    CHECK(pc_engine_session_execute_json(s, "layer.toggleMaskOverlay", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_STR(result_mode(&out), "overlay");
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "layer.toggleMaskOverlay", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_STR(result_mode(&out), "off");
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_no_mask_and_cross_layer_select(void)
{
    pc_layer_id m, p;
    pc_engine_session *s = build(&m, &p);
    pc_json_value out;
    pc_error err = {0};
    /* `\` disabled on a layer without a mask; a direct gray view errors. */
    CHECK(pc_engine_session_select_layer(s, p, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "layer.toggleMaskOverlay"));
    CHECK(pc_engine_session_execute_json(s, "view.layerMask", "{\"mode\":\"gray\"}", &out, &err) != PC_OK);

    /* ⌥-clicking the masked layer's thumbnail selects it and shows the mask. */
    char buf[80];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"mode\":\"gray\"}", (unsigned long long)m);
    CHECK(pc_engine_session_execute_json(s, "view.layerMask", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_active(s)->active_layer == m);

    /* current() liveness: switching the active layer away hides the mask view. */
    char mbuf[16];
    CHECK_EQ_STR(inspect_mode(s, mbuf, sizeof mbuf, NULL), "gray");
    CHECK(pc_engine_session_select_layer(s, p, NULL) == PC_OK);
    CHECK(inspect_mode(s, mbuf, sizeof mbuf, NULL) == NULL);
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_layer_id m, p;
    pc_engine_session *s = build(&m, &p);
    CHECK(pc_engine_session_select_layer(s, m, NULL) == PC_OK);
    pc_json_value out;
    pc_error err = {0};
    const char *const bad[] = {"{\"mode\":123}",  "{\"mode\":\"sideways\"}", "{\"layer\":\"x\"}",
                               "{\"layer\":-1}",  "[1,2]",                   "null"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        err.code = PC_OK;
        pc_status rc = pc_engine_session_execute_json(s, "view.layerMask", bad[i], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
        rc = pc_engine_session_execute_json(s, "layer.toggleMaskOverlay", bad[i], &out, &err);
        if (rc == PC_OK) {
            pc_json_value_fini(&out);
        }
    }
    pc_engine_session_free(s);

    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(empty, "view.layerMask"));
    CHECK(pc_engine_session_execute_json(empty, "view.layerMask", "{}", &out, &err) != PC_OK);
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    RUN(test_gray_overlay_and_off);
    RUN(test_toggle_default_and_overlay_command);
    RUN(test_no_mask_and_cross_layer_select);
    RUN(test_adversarial);
    return pc_t_finish();
}
