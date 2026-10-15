/* Layers panel disclosure state (port of group_view_cmds tests). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "engine/layer_cmds.h"
#include "harness.h"

static pc_engine_session *mk(const char *const *names, size_t n, pc_layer_id *ids)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(32, 32), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    for (size_t i = 0; i < n; i++) {
        pc_layer l;
        CHECK(pc_layer_raster(&l, names[i], fmt, NULL) == PC_OK);
        CHECK(pc_document_insert_above(&doc, NULL, &l, &ids[i], NULL) == PC_OK);
    }
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static uint64_t group_sel(pc_engine_session *s, const pc_layer_id *ids, size_t n)
{
    CHECK(pc_engine_set_selection(s, ids, n, true, ids[n - 1], true, ids[n - 1], NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.new.groupFromLayers", "{}", &out, NULL) == PC_OK);
    uint64_t gid = yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "layer"));
    pc_json_value_fini(&out);
    return gid;
}

static bool expanded(pc_engine_session *s, uint64_t id)
{
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), (pc_layer_id)id);
    return l != NULL && l->content.kind == PC_LAYER_GROUP && l->content.u.group.expanded;
}

/* A document with outer{ inner{ B, C } } (A is also present outside). */
static pc_engine_session *nested(uint64_t *inner, uint64_t *outer)
{
    pc_layer_id ids[3];
    const char *const names[] = {"A", "B", "C"};
    pc_engine_session *s = mk(names, 3, ids);
    pc_layer_id bc[2] = {ids[1], ids[2]};
    *inner = group_sel(s, bc, 2);
    pc_layer_id in = (pc_layer_id)*inner;
    *outer = group_sel(s, &in, 1);
    return s;
}

static void test_toggles_without_history_or_dirtying(void)
{
    uint64_t inner, outer;
    pc_engine_session *s = nested(&inner, &outer);
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    st->saved_revision = st->revision; /* mark clean */
    size_t steps = pc_history_entry_count(st->history);
    CHECK(expanded(s, inner) && expanded(s, outer));

    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)inner);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", buf, &out, NULL) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "expanded")));
    pc_json_value_fini(&out);
    CHECK(!expanded(s, inner) && expanded(s, outer));
    st = pc_engine_session_active_mut(s);
    CHECK_EQ_UINT(pc_history_entry_count(st->history), steps); /* no history step */
    CHECK(st->saved_revision == st->revision);                /* still clean */

    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"expanded\":true}", (unsigned long long)inner);
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(expanded(s, inner));
    pc_engine_session_free(s);
}

static void test_all_sets_every_group(void)
{
    uint64_t inner, outer;
    pc_engine_session *s = nested(&inner, &outer);
    pc_json_value out;
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"all\":true}", (unsigned long long)outer);
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(!expanded(s, inner) && !expanded(s, outer));
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", "{\"all\":true}", &out, NULL) == PC_OK);
    CHECK_EQ_UINT(yyjson_mut_get_uint(yyjson_mut_obj_get(pc_json_value_root(&out), "groups")), 2);
    pc_json_value_fini(&out);
    CHECK(expanded(s, inner) && expanded(s, outer));
    pc_engine_session_free(s);
}

static void test_bad_params_and_no_groups(void)
{
    uint64_t inner, outer;
    pc_engine_session *s = nested(&inner, &outer);
    const pc_layer *bg = pc_document_background(pc_doc_arc_get(pc_engine_session_active(s)->doc));
    CHECK(bg != NULL);
    pc_json_value out;
    pc_error err = {0};
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu}", (unsigned long long)bg->id);
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", buf, &out, &err) != PC_OK); /* not a group */
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", "{\"layer\":9999}", &out, &err) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", "{\"layer\":\"x\"}", &out, &err) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", "{\"all\":3}", &out, &err) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", "{\"expanded\":\"yes\",\"all\":true}", &out,
                                         &err) != PC_OK);
    pc_engine_session_free(s);

    /* Empty session and a group-free document: disabled and an error. */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(empty, "layer.setExpanded"));
    CHECK(pc_engine_session_execute_json(empty, "layer.setExpanded", "{}", &out, &err) != PC_OK);
    pc_engine_session_free(empty);

    pc_layer_id ids[1];
    const char *const names[] = {"flat"};
    pc_engine_session *flat = mk(names, 1, ids);
    CHECK(!pc_engine_session_is_enabled(flat, "layer.setExpanded")); /* no groups */
    CHECK(pc_engine_session_execute_json(flat, "layer.setExpanded", "{\"all\":true}", &out, &err) != PC_OK);
    pc_engine_session_free(flat);
}

int main(int argc, char **argv)
{
    RUN(test_toggles_without_history_or_dirtying);
    RUN(test_all_sets_every_group);
    RUN(test_bad_params_and_no_groups);
    return pc_t_finish();
}
