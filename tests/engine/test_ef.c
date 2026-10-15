/* EF (engine foundations): the Session/DocState EXTENSION fields and the commands they unblock --
 *   - Edit > Fade (edit.fade) + the edit_menu after-command hook that records its source;
 *   - Color Sampler points (view.colorSamplers.*) in DocState.color_samplers, incl. the sampler
 *     map applied by a canvas-size edit and the compound rollback;
 *   - Smart object Edit Contents / Save Contents (Session.smart_links) + the on-close auto-commit.
 * Behaviour plus adversarial params (never crash; REWRITE_PLAN.md rule 9). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "raster/surface.h"
#include "harness.h"

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    pc_json_value_fini(&out);
    return rc;
}

static pc_engine_session *mk(int w, int h, const char *bg)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    char buf[128];
    (void)snprintf(buf, sizeof buf, "{\"width\":%d,\"height\":%d,\"background\":\"%s\"}", w, h, bg);
    CHECK(exec(s, "file.new", buf) == PC_OK);
    return s;
}

static const pc_engine_doc_state *active(pc_engine_session *s)
{
    return pc_engine_session_active(s);
}

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(active(s)->doc);
}

/* ---- Color Sampler points ----------------------------------------------------------------- */

static size_t sampler_count(pc_engine_session *s)
{
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "view.colorSamplers.list", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *arr = yyjson_mut_obj_get(pc_json_value_root(&out), "samplers");
    size_t n = yyjson_mut_arr_size(arr);
    pc_json_value_fini(&out);
    return n;
}

static void test_sampler_add_and_list(void)
{
    pc_engine_session *s = mk(100, 80, "white");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "view.colorSamplers.add", "{\"x\":5,\"y\":6}", &out, NULL) ==
          PC_OK);
    yyjson_mut_val *r = pc_json_value_root(&out);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(r, "index")) == 0);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(r, "count")) == 1);
    yyjson_mut_val *pt = yyjson_mut_obj_get(r, "point");
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(pt, "number")) == 1);
    yyjson_mut_val *rgb = yyjson_mut_obj_get(pt, "rgb");
    CHECK(yyjson_mut_get_int(yyjson_mut_arr_get(rgb, 0)) == 255);
    CHECK(yyjson_mut_get_int(yyjson_mut_arr_get(rgb, 1)) == 255);
    CHECK(yyjson_mut_get_int(yyjson_mut_arr_get(rgb, 2)) == 255);
    yyjson_mut_val *cmyk = yyjson_mut_obj_get(pt, "cmyk");
    for (size_t i = 0; i < 4; i++) {
        CHECK(yyjson_mut_get_int(yyjson_mut_arr_get(cmyk, i)) == 0);
    }
    pc_json_value_fini(&out);
    CHECK(sampler_count(s) == 1);
    pc_engine_session_free(s);
}

static void test_sampler_floors_and_off_canvas(void)
{
    pc_engine_session *s = mk(100, 80, "white");
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "view.colorSamplers.add", "{\"x\":5.9,\"y\":6.1}", &out,
                                         NULL) == PC_OK);
    yyjson_mut_val *pos = yyjson_mut_obj_get(yyjson_mut_obj_get(pc_json_value_root(&out), "point"), "position");
    CHECK(yyjson_mut_get_real(yyjson_mut_arr_get(pos, 0)) == 5.0);
    CHECK(yyjson_mut_get_real(yyjson_mut_arr_get(pos, 1)) == 6.0);
    pc_json_value_fini(&out);
    /* Off the canvas places nothing. */
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":5,\"y\":1000}") != PC_OK);
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":-1,\"y\":5}") != PC_OK);
    CHECK(sampler_count(s) == 1);
    pc_engine_session_free(s);
}

static void test_sampler_limit(void)
{
    pc_engine_session *s = mk(100, 80, "white");
    for (int i = 0; i < 10; i++) {
        char buf[64];
        (void)snprintf(buf, sizeof buf, "{\"x\":%d,\"y\":0}", i);
        CHECK(exec(s, "view.colorSamplers.add", buf) == PC_OK);
    }
    CHECK(sampler_count(s) == 10);
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":20,\"y\":0}") != PC_OK); /* the limit */
    CHECK(sampler_count(s) == 10);
    pc_engine_session_free(s);
}

static void test_sampler_move_delete_clear(void)
{
    pc_engine_session *s = mk(100, 80, "white");
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":5,\"y\":5}") == PC_OK);
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":50,\"y\":50}") == PC_OK);
    CHECK(exec(s, "view.colorSamplers.move", "{\"index\":0,\"x\":10,\"y\":20}") == PC_OK);
    CHECK(exec(s, "view.colorSamplers.delete", "{\"index\":1}") == PC_OK);
    CHECK(sampler_count(s) == 1);
    CHECK(exec(s, "view.colorSamplers.clear", "{}") == PC_OK);
    CHECK(sampler_count(s) == 0);
    pc_engine_session_free(s);
}

static void test_sampler_no_history_no_pixels(void)
{
    pc_engine_session *s = mk(40, 40, "white");
    size_t past = pc_history_past_len(active(s)->history);
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":5,\"y\":5}") == PC_OK);
    CHECK(exec(s, "view.colorSamplers.move", "{\"index\":0,\"x\":9,\"y\":9}") == PC_OK);
    CHECK(pc_history_past_len(active(s)->history) == past); /* no steps */
    CHECK(!pc_engine_doc_state_is_dirty(active(s)));        /* a clean document stays clean */
    pc_engine_session_free(s);
}

static void test_sampler_belongs_to_document(void)
{
    pc_engine_session *s = mk(100, 80, "white");
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":5,\"y\":5}") == PC_OK);
    CHECK(exec(s, "file.new", "{\"width\":40,\"height\":40,\"background\":\"white\"}") == PC_OK);
    CHECK(sampler_count(s) == 0); /* the new document has no points */
    CHECK(pc_engine_session_set_active(s, 0));
    CHECK(sampler_count(s) == 1); /* the first document kept its point */
    pc_engine_session_free(s);
}

static void test_sampler_crop_moves_and_drops(void)
{
    /* A crop from a non-zero origin moves the canvas: the inside point follows its pixel, the
     * outside point is dropped (the Session sampler map applied in edit()). */
    pc_engine_session *s = mk(100, 80, "white");
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":45,\"y\":35}") == PC_OK);
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":90,\"y\":70}") == PC_OK);
    CHECK(exec(s, "image.crop", "{\"x\":40,\"y\":30,\"width\":20,\"height\":20}") == PC_OK);
    CHECK(sampler_count(s) == 1); /* the outside point was removed */
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "view.colorSamplers.list", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *pos = yyjson_mut_obj_get(
        yyjson_mut_arr_get(yyjson_mut_obj_get(pc_json_value_root(&out), "samplers"), 0), "position");
    CHECK(yyjson_mut_get_real(yyjson_mut_arr_get(pos, 0)) == 5.0); /* moved by (-40, -30) */
    CHECK(yyjson_mut_get_real(yyjson_mut_arr_get(pos, 1)) == 5.0);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_sampler_adversarial(void)
{
    /* No document. */
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    const char *ids[] = {"view.colorSamplers.add", "view.colorSamplers.move", "view.colorSamplers.delete",
                         "view.colorSamplers.clear", "view.colorSamplers.list"};
    for (size_t i = 0; i < 5; i++) {
        CHECK(exec(s, ids[i], "{}") != PC_OK);
    }
    pc_engine_session_free(s);
    /* Hostile params never crash and never place a point. */
    s = mk(100, 80, "white");
    const char *bad[] = {
        "{}",
        "{\"x\":\"a\",\"y\":1}",
        "{\"x\":1e300,\"y\":1}",
        "{\"x\":5,\"y\":null}",
        "{\"x\":5}",
    };
    for (size_t i = 0; i < 5; i++) {
        CHECK(exec(s, "view.colorSamplers.add", bad[i]) != PC_OK);
    }
    CHECK(exec(s, "view.colorSamplers.move", "{\"index\":0,\"x\":1,\"y\":1}") != PC_OK);
    CHECK(exec(s, "view.colorSamplers.delete", "{\"index\":0}") != PC_OK);
    CHECK(exec(s, "view.colorSamplers.delete", "{}") != PC_OK);
    CHECK(exec(s, "view.colorSamplers.move", "{\"index\":4294967296,\"x\":1,\"y\":1}") != PC_OK);
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":1.5,\"y\":2}") == PC_OK); /* fractional ok */
    CHECK(sampler_count(s) == 1);
    pc_engine_session_free(s);
}

/* ---- Edit > Fade -------------------------------------------------------------------------- */

static void test_fade_pixel_path(void)
{
    /* Invert a white document (-> black), then Fade 50 % Normal -> mid gray (128). */
    pc_engine_session *s = mk(16, 16, "white");
    CHECK(!pc_engine_session_is_enabled(s, "edit.fade")); /* nothing to fade yet */
    CHECK(exec(s, "image.adjustments.invert", "{}") == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "edit.fade")); /* a fadeable step was recorded */
    CHECK(exec(s, "edit.fade", "{\"opacity\":50}") == PC_OK);
    float p[4];
    pc_surface_rgba(pc_layer_surface(&doc_of(s)->layers[0]), 8, 8, p);
    CHECK(p[0] > 0.45f && p[0] < 0.55f); /* halfway between white and black */
    /* Fade is only available right after the step: another command disables it. */
    CHECK(exec(s, "view.colorSamplers.add", "{\"x\":1,\"y\":1}") == PC_OK);
    CHECK(!pc_engine_session_is_enabled(s, "edit.fade"));
    pc_engine_session_free(s);
}

static void test_fade_full_opacity_round_trips(void)
{
    pc_engine_session *s = mk(16, 16, "white");
    CHECK(exec(s, "image.adjustments.invert", "{}") == PC_OK);
    CHECK(exec(s, "edit.fade", "{\"opacity\":0}") == PC_OK); /* fade to nothing -> back to white */
    float p[4];
    pc_surface_rgba(pc_layer_surface(&doc_of(s)->layers[0]), 8, 8, p);
    CHECK(p[0] > 0.99f);
    pc_engine_session_free(s);
}

static void test_fade_with_mask_present(void)
{
    /* A layer with a mask, where a pixel adjustment ran: Fade operates on the pixels (the mask was
     * not touched), exercising the mask-vs-pixels fingerprint (mask=false here). */
    pc_engine_session *s = mk(16, 16, "white");
    CHECK(exec(s, "layer.layerMask.revealAll", "{}") == PC_OK);
    CHECK(doc_of(s)->layers[0].has_mask);
    CHECK(exec(s, "image.adjustments.invert", "{}") == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "edit.fade"));
    CHECK(exec(s, "edit.fade", "{\"opacity\":50,\"mode\":\"normal\"}") == PC_OK);
    pc_engine_session_free(s);
}

static void test_fade_adversarial(void)
{
    pc_engine_session *s = mk(16, 16, "white");
    CHECK(exec(s, "edit.fade", "{}") != PC_OK); /* nothing to fade */
    CHECK(exec(s, "image.adjustments.invert", "{}") == PC_OK);
    CHECK(exec(s, "edit.fade", "{\"mode\":\"nonsense\"}") != PC_OK); /* unknown blend mode */
    /* the failed fade left the state unchanged, so Fade is still available */
    CHECK(pc_engine_session_is_enabled(s, "edit.fade"));
    CHECK(exec(s, "edit.fade", "{\"opacity\":\"x\"}") != PC_OK);
    CHECK(exec(s, "edit.fade", "{\"opacity\":1e30}") == PC_OK); /* out-of-range clamps to 100 */
    pc_engine_session_free(s);
}

/* ---- Smart object Edit Contents / Save Contents ------------------------------------------- */

/* Make a smart object from the Background and return the session with it active. */
static pc_engine_session *mk_smart(void)
{
    pc_engine_session *s = mk(24, 24, "white");
    CHECK(exec(s, "layer.smartObjects.convertToSmartObject", "{}") == PC_OK);
    return s;
}

static void test_edit_contents_opens_child(void)
{
    pc_engine_session *s = mk_smart();
    size_t before = 0;
    (void)pc_engine_session_documents(s, &before);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.smartObjects.editContents", "{}", &out, NULL) == PC_OK);
    size_t after = 0;
    (void)pc_engine_session_documents(s, &after);
    CHECK(after == before + 1); /* a child document opened */
    size_t childidx = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "document"));
    pc_json_value_fini(&out);
    size_t act = 0;
    CHECK(pc_engine_session_active_index(s, &act) && act == childidx); /* it became active */
    /* Save Contents is available in the child, not nonsense. */
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.saveContents"));

    /* A second Edit Contents returns the same open document, not another copy. */
    CHECK(pc_engine_session_set_active(s, 0)); /* back to the parent */
    pc_json_value out2 = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.smartObjects.editContents", "{}", &out2, NULL) == PC_OK);
    CHECK((size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out2), "document")) == childidx);
    pc_json_value_fini(&out2);
    size_t after2 = 0;
    (void)pc_engine_session_documents(s, &after2);
    CHECK(after2 == after); /* no new document */
    pc_engine_session_free(s);
}

static void test_save_contents(void)
{
    pc_engine_session *s = mk_smart();
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.smartObjects.editContents", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    /* Edit the child, then Save Contents re-embeds it. */
    CHECK(exec(s, "image.adjustments.invert", "{}") == PC_OK);
    pc_json_value sr = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.smartObjects.saveContents", "{}", &sr, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&sr), "updated")));
    pc_json_value_fini(&sr);
    /* Save Contents on a plain (non-child) document is refused, not a crash. */
    pc_engine_session *plain = mk(8, 8, "white");
    CHECK(!pc_engine_session_is_enabled(plain, "layer.smartObjects.saveContents"));
    CHECK(exec(plain, "layer.smartObjects.saveContents", "{}") != PC_OK);
    pc_engine_session_free(plain);
    pc_engine_session_free(s);
}

static void test_on_close_commits_and_drops_link(void)
{
    pc_engine_session *s = mk_smart();
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(s, "layer.smartObjects.editContents", "{}", &out, NULL) == PC_OK);
    size_t childidx = (size_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "document"));
    pc_json_value_fini(&out);
    /* Edit the child (now dirty), then close it: the on-close hook auto-commits and drops the link. */
    CHECK(exec(s, "image.adjustments.invert", "{}") == PC_OK);
    CHECK(pc_engine_session_close(s, childidx));
    /* The parent remains; nothing dangling (ASan would catch a use-after-free). */
    CHECK(pc_engine_session_active(s) != NULL);
    pc_engine_session_free(s);
}

static void test_smart_contents_adversarial(void)
{
    /* editContents on a non-smart layer, saveContents on a normal document: refused, not a crash. */
    pc_engine_session *s = mk(16, 16, "white");
    CHECK(exec(s, "layer.smartObjects.editContents", "{}") != PC_OK);
    CHECK(exec(s, "layer.smartObjects.saveContents", "{}") != PC_OK);
    CHECK(exec(s, "layer.smartObjects.editContents", "{\"layer\":999999}") != PC_OK);
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_sampler_add_and_list);
    RUN(test_sampler_floors_and_off_canvas);
    RUN(test_sampler_limit);
    RUN(test_sampler_move_delete_clear);
    RUN(test_sampler_no_history_no_pixels);
    RUN(test_sampler_belongs_to_document);
    RUN(test_sampler_crop_moves_and_drops);
    RUN(test_sampler_adversarial);
    RUN(test_fade_pixel_path);
    RUN(test_fade_full_opacity_round_trips);
    RUN(test_fade_with_mask_present);
    RUN(test_fade_adversarial);
    RUN(test_edit_contents_opens_child);
    RUN(test_save_contents);
    RUN(test_on_close_commits_and_drops_link);
    RUN(test_smart_contents_adversarial);
    return pc_t_finish();
}
