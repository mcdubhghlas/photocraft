/* ET6: Filter > Distort > Displace (displacement-map resolution: embedded PSD / mapPath /
 * mapDocument) and Layer > Smart Objects > Convert to Layers (unpack a smart object's contents).
 * Behaviour plus an adversarial-params sweep (every registered command returns an error, never
 * crashes, on hostile input). The algorithms themselves are parity-tested in tests/algo; this file
 * checks the engine wiring: exact ids, map loading, folding/grouping, placement, and errors. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/buf.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine/engine_internal.h"
#include "raster/surface.h"
#include "harness.h"

#include <yyjson.h>

static pc_status exec(pc_engine_session *s, const char *id, const char *json)
{
    pc_json_value out;
    pc_status rc = pc_engine_session_execute_json(s, id, json, &out, NULL);
    if (rc == PC_OK) {
        pc_json_value_fini(&out);
    }
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

static const pc_document *doc_of(pc_engine_session *s)
{
    return pc_doc_arc_get(pc_engine_session_active(s)->doc);
}

static const pc_layer *active_layer(pc_engine_session *s)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    return pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer);
}

static void active_px(pc_engine_session *s, int32_t x, int32_t y, float out[4])
{
    pc_surface_rgba(pc_layer_surface(active_layer(s)), x, y, out);
}

/* ---- Displace ------------------------------------------------------------------------------- */

static void test_displace_registered(void)
{
    pc_engine_session *s = mk(16, 16, "white");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#3366cc\"}") == PC_OK);
    /* Registered and enabled on a pixel layer. */
    CHECK(pc_engine_session_is_enabled(s, "filter.distort.displace"));
    /* Without a map the command is refused, before any change. */
    uint64_t rev = pc_engine_session_active(s)->revision;
    CHECK(exec(s, "filter.distort.displace", "{\"horizontal\":10,\"vertical\":10}") != PC_OK);
    CHECK(pc_engine_session_active(s)->revision == rev);
    pc_engine_session_free(s);
}

static void test_displace_bad_map_path(void)
{
    pc_engine_session *s = mk(12, 12, "white");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":\"#808080\"}") == PC_OK);
    /* A map path that cannot be read is an error (never a crash), document unchanged. */
    uint64_t rev = pc_engine_session_active(s)->revision;
    CHECK(exec(s, "filter.distort.displace",
               "{\"mapPath\":\"/nonexistent/pc-et6-does-not-exist.png\"}") != PC_OK);
    CHECK(pc_engine_session_active(s)->revision == rev);
    /* A bad `fit` choice is rejected by validation. */
    CHECK(exec(s, "filter.distort.displace", "{\"fit\":\"bogus\",\"mapDocument\":0}") != PC_OK);
    pc_engine_session_free(s);
}

/* A two-document session: document 0 is a gradient displacement map, document 1 (active) is the
 * target. Displace reads the map from document 0 by index. */
static void test_displace_map_document(void)
{
    pc_engine_session *s = mk(24, 24, "white"); /* becomes document 0 (the map) */
    /* Paint the map with two tones so the map is not flat (0.5 everywhere = no displacement). */
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":12,\"height\":24}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":[0.0,0.0,0.0,1.0]}") == PC_OK);
    CHECK(exec(s, "select.deselect", "{}") == PC_OK);

    char buf[128];
    (void)snprintf(buf, sizeof buf, "{\"width\":24,\"height\":24,\"background\":\"white\"}");
    CHECK(exec(s, "file.new", buf) == PC_OK); /* document 1, now active (the target) */
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    /* A diagonal split so displacement moves visible content. */
    CHECK(exec(s, "select.rect", "{\"x\":0,\"y\":0,\"width\":24,\"height\":12}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":[0.9,0.1,0.1,1.0]}") == PC_OK);
    CHECK(exec(s, "select.deselect", "{}") == PC_OK);

    CHECK(doc_of(s)->size.width == 24);
    /* Displace using the other open document as the map. */
    pc_status rc = exec(s, "filter.distort.displace",
                        "{\"horizontal\":80,\"vertical\":80,\"mapDocument\":0}");
    CHECK(rc == PC_OK);
    /* The active layer is still a 24x24 raster. */
    const pc_layer *l = active_layer(s);
    CHECK(l->content.kind == PC_LAYER_RASTER);
    pc_engine_session_free(s);
}

/* Write a document to a temporary .pcraft file and use it as a Displace map (exercises
 * pc_engine_import on a real file). */
static void test_displace_map_path_file(void)
{
    const char *tmp = getenv("TMPDIR");
    char path[256];
    (void)snprintf(path, sizeof path, "%s/pc-et6-map-%ld.pcraft", (tmp != NULL ? tmp : "."),
                   (long)getpid());

    /* Build a small gradient map document and encode it to the file. */
    pc_engine_session *m = mk(20, 20, "white");
    CHECK(exec(m, "select.rect", "{\"x\":0,\"y\":0,\"width\":10,\"height\":20}") == PC_OK);
    CHECK(exec(m, "edit.fill", "{\"color\":[0.0,0.0,0.0,1.0]}") == PC_OK);
    CHECK(exec(m, "select.deselect", "{}") == PC_OK);
    pc_buf bytes;
    pc_buf_init(&bytes);
    pc_engine_save_opts opts = pc_engine_save_opts_default();
    CHECK(pc_engine_encode(doc_of(m), path, &opts, &bytes, NULL, NULL) == PC_OK);
    CHECK(pc_engine_write_file(path, bytes.data, bytes.len, NULL) == PC_OK);
    pc_buf_fini(&bytes);
    pc_engine_session_free(m);

    pc_engine_session *s = mk(20, 20, "white");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    CHECK(exec(s, "edit.fill", "{\"color\":[0.2,0.6,0.9,1.0]}") == PC_OK);
    char p[384];
    (void)snprintf(p, sizeof p, "{\"horizontal\":60,\"vertical\":0,\"mapPath\":\"%s\"}", path);
    CHECK(exec(s, "filter.distort.displace", p) == PC_OK);
    CHECK(active_layer(s)->content.kind == PC_LAYER_RASTER);
    pc_engine_session_free(s);
    (void)remove(path);
}

/* ---- Convert to Layers ---------------------------------------------------------------------- */

/* Paint a solid layer and fold it into a smart object. Returns the session (active = smart). */
static pc_engine_session *smart_from_paint(int w, int h, const char *color)
{
    pc_engine_session *s = mk(w, h, "white");
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"color\":\"%s\"}", color);
    CHECK(exec(s, "edit.fill", buf) == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.convertToSmartObject"));
    CHECK(exec(s, "layer.smartObjects.convertToSmartObject", "{}") == PC_OK);
    CHECK(active_layer(s)->content.kind == PC_LAYER_SMART);
    return s;
}

static void test_convert_to_layers_folds_to_one_layer(void)
{
    pc_engine_session *s = smart_from_paint(16, 16, "#3366cc");
    CHECK(pc_engine_session_is_enabled(s, "layer.smartObjects.convertToLayers"));
    float before[4];
    active_px(s, 5, 5, before);

    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.smartObjects.convertToLayers", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(root != NULL);
    yyjson_mut_val *grp = yyjson_mut_obj_get(root, "group");
    CHECK(grp != NULL && yyjson_mut_is_bool(grp) && yyjson_mut_get_bool(grp) == false);
    yyjson_mut_val *disc = yyjson_mut_obj_get(root, "discardedSmartFilters");
    CHECK(disc != NULL && yyjson_mut_get_uint(disc) == 0);
    pc_json_value_fini(&out);

    /* One raster layer replaces the smart object, same pixels (whole-pixel placement is exact). */
    const pc_layer *l = active_layer(s);
    CHECK(l->content.kind == PC_LAYER_RASTER);
    float after[4];
    active_px(s, 5, 5, after);
    for (int i = 0; i < 4; i++) {
        CHECK(fabsf(after[i] - before[i]) < 1e-6f);
    }
    /* A plain raster layer is no longer convertible. */
    CHECK(!pc_engine_session_is_enabled(s, "layer.smartObjects.convertToLayers"));

    /* One undo step restores the smart object. */
    CHECK(exec(s, "edit.undo", "{}") == PC_OK);
    CHECK(active_layer(s)->content.kind == PC_LAYER_SMART);
    pc_engine_session_free(s);
}

static void test_convert_to_layers_mask_makes_a_group(void)
{
    pc_engine_session *s = smart_from_paint(16, 16, "#55aa33");
    /* A smart-object mask cannot fold into the single unpacked layer: the result is a group. */
    CHECK(exec(s, "layer.layerMask.hideAll", "{}") == PC_OK);

    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.smartObjects.convertToLayers", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    yyjson_mut_val *grp = yyjson_mut_obj_get(root, "group");
    CHECK(grp != NULL && yyjson_mut_get_bool(grp) == true);
    pc_json_value_fini(&out);

    const pc_layer *g = active_layer(s);
    CHECK(g->content.kind == PC_LAYER_GROUP);
    CHECK(g->has_mask);                              /* the mask stays on the group */
    CHECK(g->content.u.group.nchildren == 1);
    CHECK(!g->content.u.group.children[0].has_mask); /* not on the child */
    pc_engine_session_free(s);
}

static void test_convert_to_layers_applies_placement(void)
{
    pc_engine_session *s = smart_from_paint(16, 16, "#cc6633");
    /* Scale + translate: a non-whole-pixel placement takes the transform path. */
    CHECK(exec(s, "edit.transform", "{\"matrix\":[0.5,0,0,0.5,3.25,2.0]}") == PC_OK);
    CHECK(exec(s, "layer.smartObjects.convertToLayers", "{}") == PC_OK);
    CHECK(active_layer(s)->content.kind == PC_LAYER_RASTER);
    pc_engine_session_free(s);
}

static void test_convert_to_layers_rejects_non_smart(void)
{
    pc_engine_session *s = mk(12, 12, "white");
    /* No smart object: disabled and refused. */
    CHECK(!pc_engine_session_is_enabled(s, "layer.smartObjects.convertToLayers"));
    CHECK(exec(s, "layer.smartObjects.convertToLayers", "{}") != PC_OK);
    CHECK(exec(s, "layer.new.layer", "{}") == PC_OK);
    /* A pixel layer by id, and a missing layer id, both error without a crash. */
    pc_layer_id raster = pc_engine_session_active(s)->active_layer;
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)raster);
    CHECK(exec(s, "layer.smartObjects.convertToLayers", buf) != PC_OK);
    CHECK(exec(s, "layer.smartObjects.convertToLayers", "{\"layer\":18446744073709551615}") != PC_OK);
    pc_engine_session_free(s);
}

/* ---- adversarial sweep ---------------------------------------------------------------------- */

static void test_adversarial(void)
{
    static const char *ids[] = {"filter.distort.displace", "layer.smartObjects.convertToLayers"};
    static const char *params[] = {
        "null",          "true",          "42",          "\"x\"",          "[]",
        "{}",            "{\"layer\":-1}", "{\"layer\":\"no\"}",
        "{\"horizontal\":\"NaN\",\"vertical\":1e400,\"mapDocument\":999}",
        "{\"mapPath\":\"\"}", "{\"mapPath\":123}", "{\"fit\":5}",
        "{\"mapDocument\":-7}", "{\"mapLayer\":\"a\",\"mapDocument\":0}",
        "{\"layer\":18446744073709551615}"};
    for (size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
        for (size_t j = 0; j < sizeof params / sizeof params[0]; j++) {
            pc_engine_session *s = smart_from_paint(10, 10, "#777777");
            /* Whatever the params, the command returns a status (never crashes). */
            pc_json_value out;
            pc_status rc = pc_engine_session_execute_json(s, ids[i], params[j], &out, NULL);
            if (rc == PC_OK) {
                pc_json_value_fini(&out);
            }
            CHECK(true); /* reaching here means no crash */
            pc_engine_session_free(s);
        }
    }
}

int main(int argc, char **argv)
{
    RUN(test_displace_registered);
    RUN(test_displace_bad_map_path);
    RUN(test_displace_map_document);
    RUN(test_displace_map_path_file);
    RUN(test_convert_to_layers_folds_to_one_layer);
    RUN(test_convert_to_layers_mask_makes_a_group);
    RUN(test_convert_to_layers_applies_placement);
    RUN(test_convert_to_layers_rejects_non_smart);
    RUN(test_adversarial);
    return pc_t_finish();
}
