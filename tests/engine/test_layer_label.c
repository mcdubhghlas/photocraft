/* Layer colour labels (port of layer_label_cmds behaviour). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "engine/layer_cmds.h"
#include "harness.h"

/* Background, A, Group{B, C}. */
static pc_engine_session *build(pc_layer_id *a, pc_layer_id *g, pc_layer_id *b, pc_layer_id *c)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(16, 16), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1, 1, 1, 1), NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    pc_layer la;
    CHECK(pc_layer_raster(&la, "A", fmt, NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &la, a, NULL) == PC_OK);
    pc_layer *kids = malloc(2 * sizeof *kids);
    CHECK(pc_layer_raster(&kids[0], "B", fmt, NULL) == PC_OK);
    *b = kids[0].id;
    CHECK(pc_layer_raster(&kids[1], "C", fmt, NULL) == PC_OK);
    *c = kids[1].id;
    pc_layer grp;
    CHECK(pc_layer_group(&grp, "Group", kids, 2, NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &grp, g, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_label_color label_of(pc_engine_session *s, pc_layer_id id)
{
    return pc_document_layer(pc_doc_arc_get(pc_engine_session_active(s)->doc), id)->label;
}

static void test_group_labels_descendants(void)
{
    pc_layer_id a, g, b, c;
    pc_engine_session *s = build(&a, &g, &b, &c);
    char buf[80];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"color\":\"red\"}", (unsigned long long)g);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.setLabelColor", buf, &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(root, "changed")));
    CHECK_EQ_UINT(yyjson_mut_arr_size(yyjson_mut_obj_get(root, "layers")), 3); /* group + B + C */
    pc_json_value_fini(&out);
    CHECK(label_of(s, g) == PC_LABEL_RED && label_of(s, b) == PC_LABEL_RED && label_of(s, c) == PC_LABEL_RED);
    CHECK(label_of(s, a) == PC_LABEL_NONE);

    /* Re-applying the same colour reports changed=false and adds no history. */
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    size_t steps = pc_history_entry_count(d->history);
    CHECK(pc_engine_session_execute_json(s, "layer.setLabelColor", buf, &out, NULL) == PC_OK);
    CHECK(!yyjson_mut_get_bool(yyjson_mut_obj_get(pc_json_value_root(&out), "changed")));
    pc_json_value_fini(&out);
    CHECK_EQ_UINT(pc_history_entry_count(pc_engine_session_active(s)->history), steps);
    pc_engine_session_free(s);
}

static void test_selection_and_undo(void)
{
    pc_layer_id a, g, b, c;
    pc_engine_session *s = build(&a, &g, &b, &c);
    pc_layer_id sel[1] = {a};
    CHECK(pc_engine_set_selection(s, sel, 1, true, a, true, a, NULL) == PC_OK);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.setLabelColor", "{\"color\":\"blue\"}", &out, NULL) ==
          PC_OK);
    pc_json_value_fini(&out);
    CHECK(label_of(s, a) == PC_LABEL_BLUE);
    CHECK(pc_engine_session_undo(s));
    CHECK(label_of(s, a) == PC_LABEL_NONE);
    pc_engine_session_free(s);
}

static void test_bad_params(void)
{
    pc_layer_id a, g, b, c;
    pc_engine_session *s = build(&a, &g, &b, &c);
    CHECK(pc_engine_session_select_layer(s, a, NULL) == PC_OK);
    pc_json_value out;
    pc_error err = {0};
    CHECK(pc_engine_session_execute_json(s, "layer.setLabelColor", "{\"color\":\"chartreuse\"}", &out,
                                         &err) == PC_ERR_INVALID);
    CHECK(pc_engine_session_execute_json(s, "layer.setLabelColor", "{}", &out, &err) == PC_ERR_INVALID);
    CHECK(pc_engine_session_execute_json(s, "layer.setLabelColor", "{\"color\":5}", &out, &err) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "layer.setLabelColor", "[1,2]", &out, &err) != PC_OK);
    CHECK(pc_engine_session_execute_json(s, "layer.setLabelColor", "{\"color\":\"red\",\"layer\":9999}",
                                         &out, &err) != PC_OK);
    pc_engine_session_free(s);

    /* Disabled without a document / selection. */
    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    CHECK(!pc_engine_session_is_enabled(empty, "layer.setLabelColor"));
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    RUN(test_group_labels_descendants);
    RUN(test_selection_and_undo);
    RUN(test_bad_params);
    return pc_t_finish();
}
