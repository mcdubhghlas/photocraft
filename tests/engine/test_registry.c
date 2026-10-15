/* Command registry, dispatch, session edit/undo/redo, inspect and hidden-target refusal
 * (ported from the machinery assertions of crates/engine's lib.rs/commands.rs/inspect.rs). */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "harness.h"

static pc_engine_session *white(uint32_t w, uint32_t h)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "Untitled", pc_size_make(w, h), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1.0f, 1.0f, 1.0f, 1.0f), NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static void test_registry_lookup(void)
{
    size_t n = 0;
    const pc_engine_command *all = pc_engine_commands(&n);
    CHECK(all != NULL && n >= 12);
    CHECK(pc_engine_command_find("edit.undo") != NULL);
    CHECK(pc_engine_command_find("jobs.list") != NULL);
    CHECK(pc_engine_command_find("document.pixel") != NULL);
    CHECK(pc_engine_command_find("no.such.command") == NULL);
    /* Metadata is preserved exactly. */
    const pc_engine_command *undo = pc_engine_command_find("edit.undo");
    CHECK_EQ_STR(undo->label, "Undo");
    CHECK_EQ_STR(undo->shortcut, "Cmd+Z");
    CHECK(undo->menu_len == 1 && strcmp(undo->menu[0], "Edit") == 0);
}

static void test_unknown_and_bad_params(void)
{
    pc_engine_session *s = white(16, 16);
    pc_json_value out;
    pc_error err = {0};
    CHECK(pc_engine_session_execute(s, "no.such.command", NULL, &out, &err) == PC_ERR_NOT_FOUND);
    CHECK(strstr(err.msg, "unknown command") != NULL && strstr(err.msg, "no.such.command") != NULL);
    /* Non-object params are rejected before running. */
    yyjson_doc *jd = NULL;
    CHECK(pc_json_parse("[3]", 3, &jd, NULL) == PC_OK);
    err.code = PC_OK;
    CHECK(pc_engine_session_execute(s, "select.all", yyjson_doc_get_root(jd), &out, &err) == PC_ERR_INVALID);
    CHECK(strstr(err.msg, "must be a JSON object") != NULL);
    yyjson_doc_free(jd);
    pc_engine_session_free(s);
}

static void test_select_undo_redo(void)
{
    pc_engine_session *s = white(32, 24);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    uint64_t rev0 = d->revision;
    CHECK(!pc_engine_session_is_enabled(s, "edit.undo")); /* nothing to undo yet */
    pc_json_value out;
    CHECK(pc_engine_session_execute(s, "select.all", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    d = pc_engine_session_active(s);
    CHECK(pc_doc_arc_get(d->doc)->has_selection);
    CHECK(d->revision > rev0);
    CHECK(pc_engine_session_is_enabled(s, "edit.undo"));
    CHECK(pc_engine_session_is_enabled(s, "select.deselect"));
    /* Undo removes the selection; redo restores it. */
    CHECK(pc_engine_session_undo(s));
    CHECK(!pc_doc_arc_get(pc_engine_session_active(s)->doc)->has_selection);
    CHECK(pc_engine_session_redo(s));
    CHECK(pc_doc_arc_get(pc_engine_session_active(s)->doc)->has_selection);
    /* Deselect, then inverse requires a selection. */
    CHECK(pc_engine_session_execute(s, "select.deselect", NULL, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_error why = {0};
    CHECK(pc_engine_session_disabled_reason(s, "select.deselect", &why));
    CHECK(strstr(why.msg, "no selection") != NULL);
    pc_engine_session_free(s);
}

static void test_disabled_messages(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_error why = {0};
    CHECK(pc_engine_session_disabled_reason(s, "select.all", &why));
    CHECK(strstr(why.msg, "no document open") != NULL);
    CHECK(pc_engine_session_disabled_reason(s, "edit.undo", &why));
    CHECK(strstr(why.msg, "nothing to undo") != NULL);
    pc_engine_session_free(s);
}

static void test_document_pixel_and_inspect(void)
{
    pc_engine_session *s = white(8, 8);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "document.pixel", "{\"x\":0,\"y\":0}", &out, NULL) == PC_OK);
    yyjson_mut_val *arr = pc_json_value_root(&out);
    CHECK(yyjson_mut_is_arr(arr) && yyjson_mut_arr_size(arr) == 4);
    for (size_t i = 0; i < 4; i++) {
        CHECK(yyjson_mut_get_real(yyjson_mut_arr_get(arr, i)) == 1.0);
    }
    pc_json_value_fini(&out);

    CHECK(pc_engine_session_execute(s, "session.inspect", NULL, &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    CHECK(yyjson_mut_obj_get(root, "documents") != NULL);
    CHECK(yyjson_mut_obj_get(root, "foreground") != NULL);
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

/* A raster layer with a raster mask, and one with a disabled vector mask, nested in a group. */
static void test_inspect_reports_masks_when_nested(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_document doc;
    CHECK(pc_document_init(&doc, "T", pc_size_make(4, 4), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    /* Two children. */
    pc_layer masked, vmasked;
    CHECK(pc_layer_raster(&masked, "Masked", fmt, NULL) == PC_OK);
    pc_layer_mask_reveal_all(&masked.mask);
    masked.has_mask = true;
    CHECK(pc_layer_raster(&vmasked, "VMasked", fmt, NULL) == PC_OK);
    vmasked.has_vector_mask = true;
    memset(&vmasked.vector_mask, 0, sizeof vmasked.vector_mask);
    vmasked.vector_mask.enabled = false;
    pc_layer *children = malloc(2 * sizeof *children);
    children[0] = masked;
    children[1] = vmasked;
    pc_layer group;
    CHECK(pc_layer_group(&group, "G", children, 2, NULL) == PC_OK);
    pc_layer_id gid;
    CHECK(pc_document_insert_above(&doc, NULL, &group, &gid, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);

    pc_json_value out;
    CHECK(pc_engine_session_execute(s, "document.inspect", NULL, &out, NULL) == PC_OK);
    yyjson_mut_val *root = pc_json_value_root(&out);
    yyjson_mut_val *layers = yyjson_mut_obj_get(root, "layers");
    CHECK(layers != NULL && yyjson_mut_arr_size(layers) == 1); /* the group is the only root layer */
    yyjson_mut_val *g = yyjson_mut_arr_get(layers, 0);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(g, "kind")), "Group");
    yyjson_mut_val *kids = yyjson_mut_obj_get(g, "children");
    CHECK(kids != NULL && yyjson_mut_arr_size(kids) == 2);
    /* Display order is top-to-bottom: VMasked above Masked. */
    yyjson_mut_val *top = yyjson_mut_arr_get(kids, 0);
    yyjson_mut_val *bottom = yyjson_mut_arr_get(kids, 1);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(top, "hasVectorMask")) == true);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(top, "vectorMaskEnabled")) == false);
    CHECK(yyjson_mut_get_bool(yyjson_mut_obj_get(bottom, "hasMask")) == true);
    CHECK(yyjson_mut_is_null(yyjson_mut_obj_get(bottom, "vectorMaskEnabled")));
    pc_json_value_fini(&out);
    pc_engine_session_free(s);
}

static void test_hidden_target_refusal(void)
{
    pc_engine_session *s = white(16, 16);
    /* Add a hidden layer and make it active. */
    pc_engine_doc_state *d = pc_engine_session_active_mut(s);
    pc_document *doc = NULL;
    CHECK(pc_doc_arc_make_mut(&d->doc, &doc, NULL) == PC_OK);
    pc_layer hidden;
    CHECK(pc_layer_raster(&hidden, "Hidden", pc_document_pixel_format(doc), NULL) == PC_OK);
    hidden.visible = false;
    pc_layer_id hid;
    CHECK(pc_document_insert_above(doc, NULL, &hidden, &hid, NULL) == PC_OK);
    d->has_active_layer = true;
    d->active_layer = hid;

    yyjson_doc *jd = NULL;
    CHECK(pc_json_parse("{}", 2, &jd, NULL) == PC_OK);
    yyjson_val *p = yyjson_doc_get_root(jd);
    CHECK(pc_engine_hidden_target_refusal(s, "paint.stroke", p) != NULL);
    CHECK(pc_engine_hidden_target_refusal(s, "layer.translate", p) != NULL);
    CHECK(pc_engine_hidden_target_refusal(s, "edit.undo", p) == NULL); /* not a gated command */
    /* Shown again: no refusal. */
    pc_document_layer_mut(doc, hid)->visible = true;
    CHECK(pc_engine_hidden_target_refusal(s, "paint.stroke", p) == NULL);
    yyjson_doc_free(jd);
    pc_engine_session_free(s);
}

static void test_move_layer_id(void)
{
    pc_layer_id v[4] = {10, 20, 30, 40};
    size_t to;
    CHECK(pc_engine_move_layer_id(v, 4, 0, 2, &to) && to == 2);
    CHECK(v[0] == 20 && v[1] == 30 && v[2] == 10 && v[3] == 40);
    CHECK(pc_engine_move_layer_id(v, 4, 3, 99, &to) && to == 3); /* clamps to end */
    CHECK(!pc_engine_move_layer_id(v, 4, 9, 0, &to));
}

int main(int argc, char **argv)
{
    RUN(test_registry_lookup);
    RUN(test_unknown_and_bad_params);
    RUN(test_select_undo_redo);
    RUN(test_disabled_messages);
    RUN(test_document_pixel_and_inspect);
    RUN(test_inspect_reports_masks_when_nested);
    RUN(test_hidden_target_refusal);
    RUN(test_move_layer_id);
    return pc_t_finish();
}
