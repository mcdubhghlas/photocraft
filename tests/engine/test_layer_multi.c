/* Multi-layer selection and structural commands (port of the machinery assertions of
 * crates/engine/src/layer_multi_cmds.rs). Documents are built through the doc API because the
 * single-layer commands (layer.new.layer, layer.select) are registered by sibling chunks that are
 * not yet ported. */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "engine/layer_cmds.h"
#include "harness.h"

/* A session: a white Background plus `n` named raster layers, bottom-to-top. ids[i] is names[i]. */
static pc_engine_session *mk(uint32_t w, uint32_t h, const char *const *names, size_t n, pc_layer_id *ids)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    for (size_t i = 0; i < n; i++) {
        pc_layer l;
        CHECK(pc_layer_raster(&l, names[i], fmt, NULL) == PC_OK);
        pc_layer_id id = 0;
        CHECK(pc_document_insert_above(&doc, NULL, &l, &id, NULL) == PC_OK);
        ids[i] = id;
    }
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static size_t sel_count(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_doc_state_selected_layers(d, &sel, &n, NULL);
    free(sel);
    return n;
}

static bool is_selected(pc_engine_session *s, pc_layer_id id)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    pc_layer_id *sel = NULL;
    size_t n = 0;
    (void)pc_engine_doc_state_selected_layers(d, &sel, &n, NULL);
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (sel[i] == id) {
            found = true;
        }
    }
    free(sel);
    return found;
}

static void select_two(pc_engine_session *s, pc_layer_id a, pc_layer_id b)
{
    pc_layer_id ids[2] = {a, b};
    CHECK(pc_engine_set_selection(s, ids, 2, true, b, true, b, NULL) == PC_OK);
}

static void test_select_all_and_deselect(void)
{
    pc_layer_id ids[2];
    const char *const names[] = {"A", "B"};
    pc_engine_session *s = mk(32, 32, names, 2, ids);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "select.allLayers", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    /* Two non-background layers selected; the top one (B) active. */
    CHECK_EQ_UINT(sel_count(s), 2);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    CHECK(d->has_active_layer && d->active_layer == ids[1]);
    CHECK(is_selected(s, ids[0]) && is_selected(s, ids[1]));

    CHECK(pc_engine_session_execute_json(s, "select.deselectLayers", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    d = pc_engine_session_active(s);
    CHECK(!d->has_active_layer);
    CHECK_EQ_UINT(d->nselected, 0);
    pc_engine_session_free(s);
}

static void test_find_layers(void)
{
    pc_layer_id ids[3];
    const char *const names[] = {"Red", "Blue", "reddish"};
    pc_engine_session *s = mk(16, 16, names, 3, ids);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "select.findLayers", "{\"name\":\"red\"}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(sel_count(s), 2);
    CHECK(is_selected(s, ids[0]) && is_selected(s, ids[2]) && !is_selected(s, ids[1]));
    /* No match is a graceful error. */
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "select.findLayers", "{\"name\":\"zzz\"}", &out, &err) != PC_OK);
    /* Missing name. */
    CHECK(pc_engine_session_execute_json(s, "select.findLayers", "{}", &out, &err) == PC_ERR_INVALID);
    pc_engine_session_free(s);
}

static void test_link_and_select_linked(void)
{
    pc_layer_id ids[3];
    const char *const names[] = {"A", "B", "C"};
    pc_engine_session *s = mk(16, 16, names, 3, ids);
    select_two(s, ids[0], ids[1]);
    CHECK(pc_engine_session_is_enabled(s, "layer.linkLayers"));
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.linkLayers", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    const pc_layer *a = pc_document_layer(doc, ids[0]);
    const pc_layer *b = pc_document_layer(doc, ids[1]);
    CHECK(a->has_link_group && b->has_link_group && a->link_group == b->link_group);
    /* Select just A, then Select Linked Layers pulls in B. */
    CHECK(pc_engine_session_select_layer(s, ids[0], NULL) == PC_OK);
    CHECK(pc_engine_session_is_enabled(s, "layer.selectLinkedLayers"));
    CHECK(pc_engine_session_execute_json(s, "layer.selectLinkedLayers", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(is_selected(s, ids[0]) && is_selected(s, ids[1]));
    /* Link again with both selected unlinks (toggle). */
    select_two(s, ids[0], ids[1]);
    CHECK(pc_engine_session_execute_json(s, "layer.linkLayers", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(!pc_document_layer(doc, ids[0])->has_link_group);
    CHECK(!pc_engine_session_is_enabled(s, "layer.selectLinkedLayers"));
    pc_engine_session_free(s);
}

static void test_lock_layers(void)
{
    pc_layer_id ids[2];
    const char *const names[] = {"A", "B"};
    pc_engine_session *s = mk(16, 16, names, 2, ids);
    select_two(s, ids[0], ids[1]);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.lockLayers", "{\"all\":true}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(pc_document_layer(doc, ids[0])->locks.all && pc_document_layer(doc, ids[1])->locks.all);
    /* No keys: toggles lock-all; already all locked -> unlock. */
    CHECK(pc_engine_session_execute_json(s, "layer.lockLayers", "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(!pc_document_layer(doc, ids[0])->locks.all);
    /* A single key sets only that lock. */
    CHECK(pc_engine_session_execute_json(s, "layer.lockLayers", "{\"pixels\":true}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK(pc_document_layer(doc, ids[0])->locks.pixels && !pc_document_layer(doc, ids[0])->locks.all);
    pc_engine_session_free(s);
}

static void test_rename_layer(void)
{
    pc_layer_id ids[1];
    const char *const names[] = {"A"};
    pc_engine_session *s = mk(16, 16, names, 1, ids);
    CHECK(pc_engine_session_select_layer(s, ids[0], NULL) == PC_OK);
    pc_json_value out;
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"name\":\"B\"}", (unsigned long long)ids[0]);
    CHECK(pc_engine_session_execute_json(s, "layer.renameLayer", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK_EQ_STR(pc_str_c(&pc_document_layer(doc, ids[0])->name), "B");
    CHECK(pc_engine_session_undo(s));
    doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    CHECK_EQ_STR(pc_str_c(&pc_document_layer(doc, ids[0])->name), "A");
    /* A blank name is rejected. */
    pc_error err = {0};
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"name\":\"   \"}", (unsigned long long)ids[0]);
    CHECK(pc_engine_session_execute_json(s, "layer.renameLayer", buf, &out, &err) == PC_ERR_INVALID);
    pc_engine_session_free(s);
}

static void test_group_from_layers(void)
{
    pc_layer_id ids[2];
    const char *const names[] = {"A", "B"};
    pc_engine_session *s = mk(16, 16, names, 2, ids);
    select_two(s, ids[0], ids[1]);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.new.groupFromLayers", "{\"name\":\"G\"}", &out, NULL) ==
          PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    uint64_t gid = yyjson_mut_get_uint(yyjson_mut_obj_get(root, "layer"));
    pc_json_value_fini(&out);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    /* Root now holds Background + the new group. */
    CHECK_EQ_UINT(doc->nlayers, 2);
    const pc_layer *g = pc_document_layer(doc, gid);
    CHECK(g != NULL && g->content.kind == PC_LAYER_GROUP);
    CHECK_EQ_UINT(g->content.u.group.nchildren, 2);
    CHECK(pc_engine_session_active(s)->has_active_layer &&
          pc_engine_session_active(s)->active_layer == gid);
    /* Undo restores the flat document. */
    CHECK(pc_engine_session_undo(s));
    CHECK_EQ_UINT(pc_doc_arc_get(pc_engine_session_active(s)->doc)->nlayers, 3);
    pc_engine_session_free(s);
}

static void test_merge_layers(void)
{
    pc_layer_id ids[2];
    const char *const names[] = {"A", "B"};
    pc_engine_session *s = mk(16, 16, names, 2, ids);
    select_two(s, ids[0], ids[1]);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.mergeLayers", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    uint64_t mid = yyjson_mut_get_uint(yyjson_mut_obj_get(root, "layer"));
    pc_json_value_fini(&out);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    /* Background + one merged layer. The merge creates a fresh raster layer (Layer::raster) named
     * after the top layer; both sources are gone. */
    CHECK_EQ_UINT(doc->nlayers, 2);
    CHECK(pc_document_layer(doc, mid) != NULL);
    CHECK(pc_document_layer(doc, ids[0]) == NULL && pc_document_layer(doc, ids[1]) == NULL);
    CHECK_EQ_STR(pc_str_c(&pc_document_layer(doc, mid)->name), "B");
    pc_engine_session_free(s);
}

/* top_level dedups a selected child whose parent is also selected. */
static void test_top_level_and_same_pixels(void)
{
    pc_layer_id ids[2];
    const char *const names[] = {"A", "B"};
    pc_engine_session *s = mk(16, 16, names, 2, ids);
    select_two(s, ids[0], ids[1]);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.new.groupFromLayers", "{}", &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    uint64_t gid = yyjson_mut_get_uint(yyjson_mut_obj_get(root, "layer"));
    pc_json_value_fini(&out);
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    /* Selecting the group and one of its children collapses to just the group. */
    pc_layer_id sel[2] = {(pc_layer_id)gid, ids[0]};
    pc_layer_id *top = NULL;
    size_t ntop = 0;
    CHECK(pc_engine_top_level(doc, sel, 2, &top, &ntop, NULL) == PC_OK);
    CHECK_EQ_UINT(ntop, 1);
    CHECK(top != NULL && top[0] == (pc_layer_id)gid);
    free(top);

    /* same_pixels: a document equals its own clone, and a visibility change breaks it. */
    pc_document clone;
    CHECK(pc_document_clone(doc, &clone, NULL) == PC_OK);
    CHECK(pc_engine_same_pixels(doc, &clone));
    pc_layer *l = pc_document_layer_mut(&clone, (pc_layer_id)gid);
    CHECK(l != NULL);
    l->visible = !l->visible;
    CHECK(!pc_engine_same_pixels(doc, &clone));
    pc_document_fini(&clone);
    pc_engine_session_free(s);
}

/* step_damage: a label change (metadata) leaves pixels unchanged -> bounded, empty damage. */
static void test_step_damage_bounded(void)
{
    pc_layer_id ids[1];
    const char *const names[] = {"A"};
    pc_engine_session *s = mk(16, 16, names, 1, ids);
    const pc_document *before = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    pc_document after;
    CHECK(pc_document_clone(before, &after, NULL) == PC_OK);
    pc_layer *l = pc_document_layer_mut(&after, ids[0]);
    l->label = PC_LABEL_RED;
    bool present = false;
    pc_rect dmg = {0};
    pc_engine_hook_step_damage(before, &after, &present, &dmg);
    /* Only the (ignored) label changed: pixels identical -> bounded empty damage. */
    CHECK(present);
    CHECK(pc_rect_is_empty(dmg));
    pc_document_fini(&after);
    pc_engine_session_free(s);
}

static void test_adversarial(void)
{
    pc_json_value out;
    pc_error err = {0};
    /* Empty session: every E5 command errors, never crashes. */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    const char *const cmds[] = {"select.allLayers",  "select.deselectLayers", "select.findLayers",
                                "layer.selectLinkedLayers", "layer.linkLayers",  "layer.mergeLayers",
                                "layer.new.groupFromLayers", "layer.lockLayers", "layer.renameLayer"};
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        err.code = PC_OK;
        CHECK(pc_engine_session_execute_json(empty, cmds[i], "{}", &out, &err) != PC_OK);
    }
    pc_engine_session_free(empty);

    /* With a document, adversarial params for each command still never crash. */
    pc_layer_id ids[2];
    const char *const names[] = {"A", "B"};
    pc_engine_session *s = mk(8, 8, names, 2, ids);
    const char *const bad[] = {
        "{\"name\":123}",       "{\"name\":[1,2]}",  "{\"layer\":\"nope\"}",
        "{\"layer\":-1}",       "{\"all\":3}",        "{\"color\":\"chartreuse\"}",
        "{\"name\":null}",      "[1,2,3]",            "null",
    };
    for (size_t c = 0; c < sizeof cmds / sizeof cmds[0]; c++) {
        for (size_t b = 0; b < sizeof bad / sizeof bad[0]; b++) {
            err.code = PC_OK;
            /* Just must not crash; success or a clean error are both fine. */
            pc_status rc = pc_engine_session_execute_json(s, cmds[c], bad[b], &out, &err);
            if (rc == PC_OK) {
                pc_json_value_fini(&out);
            }
        }
    }
    pc_engine_session_free(s);
}

int main(int argc, char **argv)
{
    RUN(test_select_all_and_deselect);
    RUN(test_find_layers);
    RUN(test_link_and_select_linked);
    RUN(test_lock_layers);
    RUN(test_rename_layer);
    RUN(test_group_from_layers);
    RUN(test_merge_layers);
    RUN(test_top_level_and_same_pixels);
    RUN(test_step_damage_bounded);
    RUN(test_adversarial);
    return pc_t_finish();
}
