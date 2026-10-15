/* Keyboard layer navigation (port of layer_nav_cmds tests). The test document is built through the
 * doc API: Background, A, Group{B, C}, D (bottom-to-top). Panel rows top-to-bottom: D, Group, C, B,
 * A, Background. */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/engine_internal.h"
#include "engine/layer_cmds.h"
#include "harness.h"

static pc_engine_session *build(pc_layer_id *a, pc_layer_id *g, pc_layer_id *b, pc_layer_id *c,
                                pc_layer_id *dd)
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
    CHECK(kids != NULL);
    CHECK(pc_layer_raster(&kids[0], "B", fmt, NULL) == PC_OK);
    *b = kids[0].id;
    CHECK(pc_layer_raster(&kids[1], "C", fmt, NULL) == PC_OK);
    *c = kids[1].id;
    pc_layer grp;
    CHECK(pc_layer_group(&grp, "Group", kids, 2, NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &grp, g, NULL) == PC_OK);
    pc_layer ld;
    CHECK(pc_layer_raster(&ld, "D", fmt, NULL) == PC_OK);
    CHECK(pc_document_insert_above(&doc, NULL, &ld, dd, NULL) == PC_OK);
    CHECK(pc_engine_session_add_document(s, &doc, NULL, NULL, NULL) == PC_OK);
    return s;
}

static pc_layer_id active(pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d->has_active_layer ? d->active_layer : 0;
}

static void nav(pc_engine_session *s, const char *id)
{
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, id, "{}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
}

static void test_panel_rows_and_steps(void)
{
    pc_layer_id a, g, b, c, d;
    pc_engine_session *s = build(&a, &g, &b, &c, &d);
    pc_layer_id *rows = NULL;
    size_t n = 0;
    CHECK(pc_engine_panel_rows(pc_doc_arc_get(pc_engine_session_active(s)->doc), &rows, &n, NULL) == PC_OK);
    const pc_layer *bg = pc_document_background(pc_doc_arc_get(pc_engine_session_active(s)->doc));
    CHECK_EQ_UINT(n, 6);
    CHECK(rows[0] == d && rows[1] == g && rows[2] == c && rows[3] == b && rows[4] == a &&
          rows[5] == bg->id);
    free(rows);

    CHECK(pc_engine_session_select_layer(s, d, NULL) == PC_OK);
    pc_layer_id below[] = {g, c, b, a, bg->id, bg->id};
    for (size_t i = 0; i < 6; i++) {
        nav(s, "layer.selectBelow");
        CHECK(active(s) == below[i]);
    }
    pc_layer_id above[] = {a, b, c, g, d, d};
    for (size_t i = 0; i < 6; i++) {
        nav(s, "layer.selectAbove");
        CHECK(active(s) == above[i]);
    }
    nav(s, "layer.selectBottom");
    CHECK(active(s) == bg->id);
    nav(s, "layer.selectTop");
    CHECK(active(s) == d);
    pc_engine_session_free(s);
}

static void test_collapsed_group_is_one_row_and_add_extends(void)
{
    pc_layer_id a, g, b, c, d;
    pc_engine_session *s = build(&a, &g, &b, &c, &d);
    /* Collapse the group. */
    char buf[64];
    (void)snprintf(buf, sizeof buf, "{\"layer\":%llu,\"expanded\":false}", (unsigned long long)g);
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.setExpanded", buf, &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    pc_layer_id *rows = NULL;
    size_t n = 0;
    CHECK(pc_engine_panel_rows(pc_doc_arc_get(pc_engine_session_active(s)->doc), &rows, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 4); /* D, Group, A, Background */
    free(rows);

    /* A layer inside the collapsed group steps from the group's row. */
    CHECK(pc_engine_session_select_layer(s, c, NULL) == PC_OK);
    nav(s, "layer.selectBelow");
    CHECK(active(s) == a);
    nav(s, "layer.addAboveToSelection");
    nav(s, "layer.addAboveToSelection");
    CHECK(active(s) == d);
    const pc_engine_doc_state *ds = pc_engine_session_active(s);
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    (void)pc_engine_doc_state_selected_layers(ds, &sel, &nsel, NULL);
    CHECK_EQ_UINT(nsel, 3);
    free(sel);
    nav(s, "layer.addBelowToSelection");
    CHECK(active(s) == g); /* active moves; the selection keeps every layer */
    ds = pc_engine_session_active(s);
    (void)pc_engine_doc_state_selected_layers(ds, &sel, &nsel, NULL);
    CHECK_EQ_UINT(nsel, 3);
    free(sel);
    pc_engine_session_free(s);
}

static void test_never_dirties_and_empty_errors(void)
{
    pc_layer_id a, g, b, c, d;
    pc_engine_session *s = build(&a, &g, &b, &c, &d);
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    st->saved_revision = st->revision;
    pc_json_value out;
    CHECK(pc_engine_session_execute_json(s, "layer.selectTop", "{\"junk\":[1,2]}", &out, NULL) == PC_OK);
    pc_json_value_fini(&out);
    CHECK(!pc_engine_doc_state_is_dirty(pc_engine_session_active(s))); /* selecting is not an edit */
    pc_engine_session_free(s);

    pc_engine_session *empty = NULL;
    CHECK(pc_engine_session_new(&empty, NULL) == PC_OK);
    const char *const cmds[] = {"layer.selectAbove",  "layer.selectBelow",       "layer.selectTop",
                                "layer.selectBottom", "layer.addAboveToSelection", "layer.addBelowToSelection"};
    pc_error err = {0};
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        CHECK(!pc_engine_session_is_enabled(empty, cmds[i]));
        CHECK(pc_engine_session_execute_json(empty, cmds[i], "{}", &out, &err) != PC_OK);
    }
    pc_engine_session_free(empty);
}

int main(int argc, char **argv)
{
    RUN(test_panel_rows_and_steps);
    RUN(test_collapsed_group_is_one_row_and_add_extends);
    RUN(test_never_dirties_and_empty_errors);
    return pc_t_finish();
}
