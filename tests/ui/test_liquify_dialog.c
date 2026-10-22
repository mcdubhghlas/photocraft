#include "harness.h"
#include "ui/liquify_dialog.h"
#include <math.h>
#include <string.h>

static pc_rect CANVAS = {0, 0, 120, 80};

static void test_alt_tool_and_resize(void)
{
    CHECK(pc_ui_liquify_alt_tool(PC_ALGO_LIQUIFY_TWIRL_CW) == PC_ALGO_LIQUIFY_TWIRL_CCW);
    CHECK(pc_ui_liquify_alt_tool(PC_ALGO_LIQUIFY_TWIRL_CCW) == PC_ALGO_LIQUIFY_TWIRL_CW);
    CHECK(pc_ui_liquify_alt_tool(PC_ALGO_LIQUIFY_PUCKER) == PC_ALGO_LIQUIFY_BLOAT);
    CHECK(pc_ui_liquify_alt_tool(PC_ALGO_LIQUIFY_BLOAT) == PC_ALGO_LIQUIFY_PUCKER);
    CHECK(pc_ui_liquify_alt_tool(PC_ALGO_LIQUIFY_FORWARD_WARP) == PC_ALGO_LIQUIFY_FORWARD_WARP);

    CHECK(pc_ui_liquify_resize_gesture(true, false, true));   /* alt + secondary */
    CHECK(pc_ui_liquify_resize_gesture(true, true, false));   /* ctrl + alt + primary */
    CHECK(!pc_ui_liquify_resize_gesture(true, false, false)); /* alt + primary = paint */
    CHECK(!pc_ui_liquify_resize_gesture(false, false, true)); /* secondary alone */

    double sz, den;
    pc_ui_liquify_resized_brush(40, 50, 25, 200.0, &sz, &den);
    CHECK(sz == 65.0 && den == 100.0);
    pc_ui_liquify_resized_brush(40, 50, -100, 0, &sz, &den);
    CHECK(sz == 1.0 && den == 50.0);
}

static void test_opts_roundtrip_and_damaged(void)
{
    pc_ui_liquify_opts o;
    pc_ui_liquify_opts_default(&o);
    CHECK(o.density == 50.0 && o.tool == PC_ALGO_LIQUIFY_FORWARD_WARP);

    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *set = yyjson_mut_obj(d);
    yyjson_mut_obj_add_real(d, set, "density", 70.0);
    yyjson_mut_obj_add_str(d, set, "tool", "bloat");
    yyjson_mut_doc_set_root(d, set);
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(d, NULL);
    CHECK(pc_ui_liquify_opts_apply(&o, yyjson_doc_get_root(imm), NULL) == PC_OK);
    CHECK(o.density == 70.0 && o.tool == PC_ALGO_LIQUIFY_BLOAT);
    yyjson_doc_free(imm);
    yyjson_mut_doc_free(d);

    /* round-trip emits the camelCase keys */
    yyjson_mut_doc *d2 = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *j = pc_ui_liquify_opts_to_json(d2, &o);
    CHECK(j != NULL);
    CHECK(yyjson_mut_get_real(yyjson_mut_obj_get(j, "density")) == 70.0);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(j, "tool")), "bloat");
    yyjson_mut_doc_free(d2);

    /* damaged prefs: bad tool errors, other fields clamped/skipped */
    pc_ui_liquify_opts o2;
    pc_ui_liquify_opts_default(&o2);
    yyjson_mut_doc *d3 = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *bad = yyjson_mut_obj(d3);
    yyjson_mut_obj_add_str(d3, bad, "tool", "nope");
    yyjson_mut_obj_add_str(d3, bad, "density", "lots");
    yyjson_mut_obj_add_real(d3, bad, "size", 1e12);
    yyjson_mut_obj_add_real(d3, bad, "rate", 20.0);
    yyjson_mut_doc_set_root(d3, bad);
    yyjson_doc *imm3 = yyjson_mut_doc_imut_copy(d3, NULL);
    CHECK(pc_ui_liquify_opts_apply(&o2, yyjson_doc_get_root(imm3), NULL) != PC_OK);
    CHECK(o2.tool == PC_ALGO_LIQUIFY_FORWARD_WARP && o2.size == 15000.0);
    CHECK(o2.rate == 20.0 && o2.density == 50.0);
    yyjson_doc_free(imm3);
    yyjson_mut_doc_free(d3);
}

static void stroke(pc_ui_liquify_dialog *s, double x0, double y0, double x1, double y1)
{
    CHECK(pc_ui_liquify_pointer(s, PC_UI_POINTER_DOWN, x0, y0, 1.0, false, NULL) == PC_OK);
    CHECK(pc_ui_liquify_pointer(s, PC_UI_POINTER_MOVE, x1, y1, 1.0, false, NULL) == PC_OK);
    CHECK(pc_ui_liquify_pointer(s, PC_UI_POINTER_UP, x1, y1, 1.0, false, NULL) == PC_OK);
}

static void test_strokes_undo_redo_commit(void)
{
    pc_ui_liquify_dialog s = {0};
    CHECK(pc_ui_liquify_begin(&s, 9, CANVAS, NULL, NULL) == PC_OK);
    stroke(&s, 50, 40, 70, 40);
    stroke(&s, 30, 30, 30, 50);
    CHECK(s.nstrokes == 2 && s.nredo == 0);
    CHECK(pc_algo_liquify_max_displacement(&s.field) > 0.0f);

    CHECK(pc_ui_liquify_undo(&s, NULL) == PC_OK);
    CHECK(s.nstrokes == 1 && s.nredo == 1);
    CHECK(pc_ui_liquify_redo(&s, NULL) == PC_OK);
    CHECK(s.nstrokes == 2 && s.nredo == 0);

    /* a new stroke after undo clears redo */
    CHECK(pc_ui_liquify_undo(&s, NULL) == PC_OK);
    CHECK(s.nredo == 1);
    stroke(&s, 10, 10, 20, 20);
    CHECK(s.nredo == 0);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = NULL;
    bool run = false;
    CHECK(pc_ui_liquify_commit_params(&s, doc, &params, &run, NULL) == PC_OK);
    CHECK(run && params != NULL);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(params, "layer")) == 9);
    CHECK(yyjson_mut_arr_size(yyjson_mut_obj_get(params, "strokes")) == 2);
    CHECK(yyjson_mut_get_real(yyjson_mut_obj_get(params, "meshSize")) == s.cell);
    yyjson_mut_doc_free(doc);
    pc_ui_liquify_fini(&s);
}

static void test_redo_cap_and_restore(void)
{
    pc_ui_liquify_dialog s = {0};
    CHECK(pc_ui_liquify_begin(&s, 1, CANVAS, NULL, NULL) == PC_OK);
    for (int i = 0; i < 101; i++)
        stroke(&s, 40, 40, 44, 40);
    CHECK(s.nstrokes == 101);
    for (int i = 0; i < 101; i++)
        CHECK(pc_ui_liquify_undo(&s, NULL) == PC_OK);
    CHECK(s.nstrokes == 0);
    CHECK(s.nredo == PC_UI_LIQUIFY_REDO_LIMIT);

    /* restore all empties everything and resets the field to identity */
    CHECK(pc_ui_liquify_redo(&s, NULL) == PC_OK);
    CHECK(s.nstrokes == 1);
    CHECK(pc_ui_liquify_restore_all(&s, NULL) == PC_OK);
    CHECK(s.nstrokes == 0 && s.nredo == 0);
    CHECK(pc_algo_liquify_is_identity(&s.field));
    pc_ui_liquify_fini(&s);
}

static void test_freeze_lasso(void)
{
    pc_ui_liquify_dialog s = {0};
    CHECK(pc_ui_liquify_begin(&s, 1, CANVAS, NULL, NULL) == PC_OK);
    s.opts.tool = PC_ALGO_LIQUIFY_LASSO_MASK;
    CHECK(pc_ui_liquify_pointer(&s, PC_UI_POINTER_DOWN, 20, 20, 1.0, false, NULL) == PC_OK);
    CHECK(pc_ui_liquify_pointer(&s, PC_UI_POINTER_MOVE, 80, 20, 1.0, false, NULL) == PC_OK);
    CHECK(pc_ui_liquify_pointer(&s, PC_UI_POINTER_MOVE, 80, 60, 1.0, false, NULL) == PC_OK);
    CHECK(pc_ui_liquify_pointer(&s, PC_UI_POINTER_MOVE, 20, 60, 1.0, false, NULL) == PC_OK);
    CHECK(pc_ui_liquify_pointer(&s, PC_UI_POINTER_UP, 20, 60, 1.0, false, NULL) == PC_OK);
    CHECK(s.nstrokes == 1);
    CHECK(pc_algo_liquify_freeze_at(&s.field, 50, 40) == 1.0f);
    CHECK(pc_algo_liquify_freeze_at(&s.field, 5, 5) == 0.0f);

    /* a global reconstruct clears redo and records a stroke */
    CHECK(pc_ui_liquify_undo(&s, NULL) == PC_OK);
    CHECK(s.nredo == 1);
    CHECK(pc_ui_liquify_global(&s, PC_ALGO_LIQUIFY_FREEZE_ALL, false, 0.0, NULL) == PC_OK);
    CHECK(s.nredo == 0 && s.nstrokes == 1);
    pc_ui_liquify_fini(&s);
}

static void test_hostile(void)
{
    pc_ui_liquify_dialog s = {0};
    CHECK(pc_ui_liquify_begin(&s, 1, CANVAS, NULL, NULL) == PC_OK);
    CHECK(pc_ui_liquify_pointer(&s, PC_UI_POINTER_DOWN, NAN, 10, 1.0, false, NULL) != PC_OK);
    pc_rect empty = {0, 0, 0, 0};
    pc_ui_liquify_dialog s2 = {0};
    CHECK(pc_ui_liquify_begin(&s2, 1, empty, NULL, NULL) != PC_OK);
    pc_ui_liquify_fini(&s2);
    pc_ui_liquify_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_alt_tool_and_resize);
    RUN(test_opts_roundtrip_and_damaged);
    RUN(test_strokes_undo_redo_commit);
    RUN(test_redo_cap_and_restore);
    RUN(test_freeze_lasso);
    RUN(test_hostile);
    return pc_t_finish();
}
