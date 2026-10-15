/* extra_cmds (E12): Stroke, Reselect, Equalize, Layer From Background, Hide/Show All Effects, the
 * layer-mask toggles, Rasterize, Delete Hidden / Empty Layers, Ungroup, Hide/Show Layers, Lock
 * Transparent Pixels, Show Only This Layer, Average and Clouds (port of extra_cmds.rs tests). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "engine/extra_cmds.h"
#include "engine/layer_cmds.h"
#include "harness.h"

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

/* Execute expecting success, freeing the result. */
static void exj(pc_engine_session *s, const char *id, const char *p)
{
    pc_json_value o;
    CHECK(pc_engine_session_execute_json(s, id, p, &o, NULL) == PC_OK);
    pc_json_value_fini(&o);
}

/* A session with a Background and `nlayers` empty raster layers above it (ids returned in `ids`). */
static pc_engine_session *mk(size_t nlayers, pc_layer_id *ids)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(32, 24), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    for (size_t i = 0; i < nlayers; i++) {
        pc_layer l;
        char nm[32];
        (void)snprintf(nm, sizeof nm, "L%zu", i);
        CHECK(pc_layer_raster(&l, nm, fmt, NULL) == PC_OK);
        CHECK(pc_document_insert_above(&doc, NULL, &l, &ids[i], NULL) == PC_OK);
    }
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static bool name_is(const pc_layer *l, const char *want)
{
    return l != NULL && l->name.len == strlen(want) && memcmp(l->name.ptr, want, l->name.len) == 0;
}

/* ---- Layer From Background + the background_to_layer export -------------------------------- */

static void test_layer_from_background(void)
{
    pc_layer_id ids[1];
    pc_engine_session *s = mk(0, ids);
    const pc_document *doc = doc_of(s);
    const pc_layer *bg = pc_document_background(doc);
    CHECK(bg != NULL);
    CHECK(bg->locks.transparency && bg->locks.position);
    pc_layer_id bgid = bg->id;

    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.new.layerFromBackground", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer")),
                  (uint64_t)bgid);
    pc_json_value_fini(&out);
    doc = doc_of(s);
    CHECK(pc_document_background(doc) == NULL); /* no longer a Background */
    const pc_layer *l0 = pc_document_layer(doc, bgid);
    CHECK(l0 != NULL && !l0->locks.transparency && !l0->locks.position);
    CHECK(name_is(l0, "Layer 0"));
    pc_engine_session_free(s);

    /* With no Background the command is disabled (enabled = has_background): execute refuses. The
     * ifPresent "{layer:null}" branch in run() is reachable only through the actions-replay path,
     * which skips the enabled gate (deferred to E12-automate). */
    pc_engine_session *s2 = mk(0, ids);
    CHECK(pc_engine_session_execute_json(s2, "layer.new.layerFromBackground", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(!pc_engine_session_is_enabled(s2, "layer.new.layerFromBackground"));
    CHECK(pc_engine_session_execute_json(s2, "layer.new.layerFromBackground", "{\"ifPresent\":true}",
                                         &out, NULL) != PC_OK);
    pc_engine_session_free(s2);

    /* The exported helper used by other chunks: a non-Background id is left alone. */
    pc_engine_session *s3 = mk(1, ids);
    pc_document clone;
    CHECK(pc_document_clone(doc_of(s3), &clone, NULL) == PC_OK);
    pc_layer_id bid = pc_document_background(&clone)->id;
    pc_engine_background_to_layer(&clone, ids[0]); /* not the Background: no change */
    CHECK(pc_document_background(&clone) != NULL);
    pc_engine_background_to_layer(&clone, bid); /* the Background: unlocked + renamed */
    CHECK(pc_document_background(&clone) == NULL);
    CHECK(name_is(pc_document_layer(&clone, bid), "Layer 0"));
    pc_document_fini(&clone);
    pc_engine_session_free(s3);
}

/* ---- Ungroup ------------------------------------------------------------------------------ */

static void test_ungroup(void)
{
    pc_layer_id ids[2];
    pc_engine_session *s = mk(2, ids);
    CHECK(pc_engine_set_selection(s, ids, 2, true, ids[1], true, ids[1], NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.new.groupFromLayers", "{}", &out, NULL) == PC_OK);
    uint64_t gid = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer"));
    pc_json_value_fini(&out);
    const pc_document *doc = doc_of(s);
    const pc_layer *g = pc_document_layer(doc, (pc_layer_id)gid);
    CHECK(g != NULL && g->content.kind == PC_LAYER_GROUP);

    size_t before = 0;
    CHECK(pc_document_layer_count(doc, &before, NULL) == PC_OK);
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)gid);
    CHECK(pc_engine_session_execute_json(s, "layer.ungroupLayers", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    doc = doc_of(s);
    CHECK(pc_document_layer(doc, (pc_layer_id)gid) == NULL); /* group gone */
    CHECK(pc_document_layer(doc, ids[0]) != NULL && pc_document_layer(doc, ids[1]) != NULL);
    /* Active becomes the top child (ids[1]). */
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d->has_active_layer && d->active_layer == ids[1]);
    pc_engine_session_free(s);
}

/* ---- Layer-mask toggles ------------------------------------------------------------------- */

static void test_mask_toggles(void)
{
    pc_layer_id ids[1];
    pc_engine_session *s = mk(1, ids);
    CHECK(pc_engine_session_select_layer(s, ids[0], NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.layerMask.revealAll", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_document_layer(doc_of(s), ids[0])->has_mask);

    /* enabled: explicit false then toggle back to true. */
    CHECK(pc_engine_session_execute_json(s, "layer.layerMask.enabled", "{\"enabled\":false}", &out,
                                         NULL) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "enabled")));
    pc_json_value_fini(&out);
    CHECK(!pc_document_layer(doc_of(s), ids[0])->mask.enabled);
    CHECK(pc_engine_session_execute_json(s, "layer.layerMask.enabled", "{}", &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "enabled")));
    pc_json_value_fini(&out);

    /* linked toggles from true to false. */
    CHECK(pc_engine_session_execute_json(s, "layer.layerMask.linked", "{}", &out, NULL) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "linked")));
    pc_json_value_fini(&out);
    CHECK(!pc_document_layer(doc_of(s), ids[0])->mask.linked);
    pc_engine_session_free(s);
}

/* ---- Delete Hidden / Empty, and the last-layer guard -------------------------------------- */

static void test_delete(void)
{
    pc_layer_id ids[2];
    pc_engine_session *s = mk(2, ids);
    /* Hide the top layer; Delete Hidden removes exactly one. */
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)ids[1]);
    exj(s, "layer.hideLayers", buf);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.delete.hiddenLayers", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "deleted")),
                  1u);
    pc_json_value_fini(&out);
    CHECK(pc_document_layer(doc_of(s), ids[1]) == NULL);
    CHECK(pc_document_layer(doc_of(s), ids[0]) != NULL);
    pc_engine_session_free(s);

    /* Delete All Empty Layers: the two empty raster layers go; the white Background stays. */
    pc_engine_session *s2 = mk(2, ids);
    CHECK(pc_engine_session_execute_json(s2, "file.scripts.deleteAllEmptyLayers", "{}", &out, NULL)
          == PC_OK);
    CHECK_EQ_UINT((uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "deleted")),
                  2u);
    pc_json_value_fini(&out);
    pc_engine_session_free(s2);

    /* A one-layer document: hiding and deleting it refuses (keep at least one layer). */
    pc_engine_session *s3 = mk(0, ids);
    pc_layer_id bg = pc_document_background(doc_of(s3))->id;
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)bg);
    exj(s3, "layer.hideLayers", buf);
    CHECK(pc_engine_session_execute_json(s3, "layer.delete.hiddenLayers", "{}", &out, NULL) != PC_OK);
    CHECK(pc_document_layer(doc_of(s3), bg) != NULL); /* unchanged */
    pc_engine_session_free(s3);
}

/* ---- Hide/Show Layers, Lock Transparency, Show Only --------------------------------------- */

static void test_visibility_and_lock(void)
{
    pc_layer_id ids[2];
    pc_engine_session *s = mk(2, ids);
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)ids[0]);
    /* Hide, then Hide again toggles to show (every targeted layer already hidden). */
    exj(s, "layer.hideLayers", buf);
    CHECK(!pc_document_layer(doc_of(s), ids[0])->visible);
    exj(s, "layer.hideLayers", buf);
    CHECK(pc_document_layer(doc_of(s), ids[0])->visible);

    /* Lock Transparent Pixels toggles. */
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.toggleLockTransparency", buf, &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "locked")));
    pc_json_value_fini(&out);
    CHECK(pc_document_layer(doc_of(s), ids[0])->locks.transparency);
    CHECK(pc_engine_session_execute_json(s, "layer.toggleLockTransparency", buf, &out, NULL) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "locked")));
    pc_json_value_fini(&out);

    /* The Background can't lock transparent pixels. */
    pc_layer_id bg = pc_document_background(doc_of(s))->id;
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)bg);
    CHECK(pc_engine_session_execute_json(s, "layer.toggleLockTransparency", buf, &out, NULL) != PC_OK);

    /* Show Only This Layer solos ids[1]; running it again reveals everything. */
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)ids[1]);
    CHECK(pc_engine_session_execute_json(s, "layer.showOnly", buf, &out, NULL) == PC_OK);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "shownAlone")));
    pc_json_value_fini(&out);
    CHECK(pc_document_layer(doc_of(s), ids[1])->visible);
    CHECK(!pc_document_layer(doc_of(s), ids[0])->visible);
    CHECK(!pc_document_layer(doc_of(s), bg)->visible);
    CHECK(pc_engine_session_execute_json(s, "layer.showOnly", buf, &out, NULL) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "shownAlone")));
    pc_json_value_fini(&out);
    CHECK(pc_document_layer(doc_of(s), ids[0])->visible && pc_document_layer(doc_of(s), bg)->visible);
    pc_engine_session_free(s);
}

/* ---- Reselect ----------------------------------------------------------------------------- */

static void test_reselect(void)
{
    pc_layer_id ids[1];
    pc_engine_session *s = mk(1, ids);
    CHECK(!pc_engine_session_is_enabled(s, "select.reselect")); /* nothing to restore yet */
    exj(s, "select.all", "{}");
    CHECK(doc_of(s)->has_selection);
    exj(s, "select.deselect", "{}");
    CHECK(!doc_of(s)->has_selection);
    CHECK(pc_engine_session_is_enabled(s, "select.reselect"));
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "select.reselect", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(doc_of(s)->has_selection);
    pc_engine_session_free(s);
}

/* ---- Equalize / Average / Clouds / Stroke (never-crash, run on real pixels) ---------------- */

static void test_pixel_ops(void)
{
    pc_layer_id ids[1];
    pc_engine_session *s = mk(0, ids); /* active = the white Background (a raster layer) */
    CHECK(pc_engine_session_is_enabled(s, "image.adjustments.equalize"));
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "image.adjustments.equalize", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "filter.blur.average", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "filter.render.clouds", "{\"seed\":7}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "filter.render.differenceClouds", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);

    /* Stroke along a selection edge draws onto the active layer. */
    CHECK(pc_engine_session_execute_json(s, "select.rect", "{\"x\":4,\"y\":4,\"width\":10,\"height\":8}",
                                         &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(pc_engine_session_execute_json(s, "edit.stroke", "{\"width\":3,\"color\":\"#ff0000\"}", &out,
                                         NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* ---- Rasterize ---------------------------------------------------------------------------- */

static void test_rasterize(void)
{
    pc_layer_id ids[1];
    pc_engine_session *s = mk(1, ids);
    CHECK(pc_engine_session_select_layer(s, ids[0], NULL) == PC_OK);
    pc_json_value out;
    /* A raster layer has nothing to rasterize: a graceful error, never a crash. */
    CHECK(pc_engine_session_execute_json(s, "layer.rasterize.layer", "{}", &out, NULL) != PC_OK);
    /* All Layers across an all-raster document: rasterized 0. */
    CHECK(pc_engine_session_execute_json(s, "layer.rasterize.allLayers", "{}", &out, NULL) == PC_OK);
    CHECK_EQ_UINT(
        (uint64_t)yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "rasterized")), 0u);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* ---- Adversarial params: every registered command, no crash ------------------------------- */

static void test_adversarial(void)
{
    size_t n = 0;
    const pc_engine_command *specs = pc_engine_extra_specs(&n);
    CHECK(n == 23);
    const char *const hostile[] = {
        "null", "true", "123", "\"x\"", "[]", "{}",
        "{\"layer\":-1}", "{\"layer\":\"nope\"}", "{\"layer\":999999}",
        "{\"width\":-5}", "{\"width\":1e9}", "{\"opacity\":\"x\"}", "{\"mode\":\"bogus\"}",
        "{\"mode\":123}", "{\"enabled\":7}", "{\"linked\":\"no\"}", "{\"seed\":-1}",
        "{\"select\":5}", "{\"location\":42}", "{\"ifPresent\":[]}",
    };
    for (size_t i = 0; i < n; i++) {
        pc_engine_session *s = NULL;
        CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
        /* Once with no document, once with a small document with a mask and a selection. */
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                pc_layer_id ids[1];
                pc_document doc;
                CHECK(pc_document_with_background(&doc, "U", pc_size_make(8, 8), PC_COLOR_MODE_RGB,
                                                  PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL)
                      == PC_OK);
                pc_layer l;
                CHECK(pc_layer_raster(&l, "L", pc_document_pixel_format(&doc), NULL) == PC_OK);
                CHECK(pc_document_insert_above(&doc, NULL, &l, &ids[0], NULL) == PC_OK);
                CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
                CHECK(pc_engine_session_select_layer(s, ids[0], NULL) == PC_OK);
                pc_json_value o;
                (void)pc_engine_session_execute_json(s, "layer.layerMask.revealAll", "{}", &o, NULL);
                pc_json_value_fini(&o);
                { pc_json_value oo; if (pc_engine_session_execute_json(s, "select.all", "{}", &oo, NULL) == PC_OK) pc_json_value_fini(&oo); }
            }
            for (size_t h = 0; h < sizeof hostile / sizeof hostile[0]; h++) {
                pc_json_value o;
                pc_error e;
                pc_status rc = pc_engine_session_execute_json(s, specs[i].id, hostile[h], &o, &e);
                if (rc == PC_OK) {
                    pc_json_value_fini(&o);
                }
                /* The only requirement: it returned, it did not crash. */
            }
        }
        pc_engine_session_free(s);
    }
}

int main(int argc, char **argv)
{
    RUN(test_layer_from_background);
    RUN(test_ungroup);
    RUN(test_mask_toggles);
    RUN(test_delete);
    RUN(test_visibility_and_lock);
    RUN(test_reselect);
    RUN(test_pixel_ops);
    RUN(test_rasterize);
    RUN(test_adversarial);
    return pc_t_finish();
}
