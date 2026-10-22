#include "harness.h"
#include "ui/wide_angle.h"
#include <math.h>
#include <string.h>

static pc_rect FRAME = {0, 0, 160, 100};

static void test_pointer_add_and_orientation(void)
{
    pc_ui_wide_dialog s = {0};
    CHECK(pc_ui_wide_begin(&s, 1, 7, FRAME, NULL) == PC_OK);
    CHECK(s.preview && s.params.n_constraints == 0);

    /* a long drag adds a Free constraint */
    CHECK(pc_ui_wide_pointer(&s, PC_UI_POINTER_DOWN, 20, 20, 1.0, false, NULL) == PC_OK);
    CHECK(pc_ui_wide_pointer(&s, PC_UI_POINTER_MOVE, 100, 22, 1.0, false, NULL) == PC_OK);
    CHECK(pc_ui_wide_pointer(&s, PC_UI_POINTER_UP, 100, 22, 1.0, false, NULL) == PC_OK);
    CHECK(s.params.n_constraints == 1);
    CHECK(s.params.constraints[0].orientation == PC_ALGO_WIDE_FREE);

    /* a tiny drag adds nothing */
    CHECK(pc_ui_wide_pointer(&s, PC_UI_POINTER_DOWN, 10, 10, 1.0, false, NULL) == PC_OK);
    CHECK(pc_ui_wide_pointer(&s, PC_UI_POINTER_UP, 11, 11, 1.0, false, NULL) == PC_OK);
    CHECK(s.params.n_constraints == 1);

    /* shift constrains orientation (horizontal when wider than tall) */
    CHECK(pc_ui_wide_pointer(&s, PC_UI_POINTER_DOWN, 10, 50, 1.0, true, NULL) == PC_OK);
    CHECK(pc_ui_wide_pointer(&s, PC_UI_POINTER_UP, 90, 52, 1.0, true, NULL) == PC_OK);
    CHECK(s.params.n_constraints == 2);
    CHECK(s.params.constraints[1].orientation == PC_ALGO_WIDE_HORIZONTAL);

    pc_ui_wide_fini(&s);
}

static void test_set_remove_undo_redo(void)
{
    pc_ui_wide_dialog s = {0};
    CHECK(pc_ui_wide_begin(&s, 1, 1, FRAME, NULL) == PC_OK);

    /* ui.set model via merge */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *set = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, set, "model", "fisheye");
    yyjson_mut_doc_set_root(d, set);
    yyjson_doc *imm = yyjson_mut_doc_imut_copy(d, NULL);
    CHECK(pc_ui_wide_apply_json(&s, yyjson_doc_get_root(imm), NULL) == PC_OK);
    CHECK(s.params.model == PC_ALGO_WIDE_FISHEYE);
    yyjson_doc_free(imm);
    yyjson_mut_doc_free(d);

    double a[2] = {10, 10}, b[2] = {100, 12};
    CHECK(pc_ui_wide_add(&s, a, b, PC_ALGO_WIDE_FREE, NULL) == PC_OK);
    CHECK(pc_ui_wide_add(&s, a, b, PC_ALGO_WIDE_VERTICAL, NULL) == PC_OK);
    CHECK(s.params.n_constraints == 2);

    /* undo steps the dialog params */
    CHECK(pc_ui_wide_undo(&s) && s.params.n_constraints == 1);
    CHECK(pc_ui_wide_redo(&s) && s.params.n_constraints == 2);

    /* remove by index + undo */
    CHECK(pc_ui_wide_remove(&s, 0, NULL) == PC_OK && s.params.n_constraints == 1);
    CHECK(pc_ui_wide_remove(&s, 99, NULL) == PC_OK && s.params.n_constraints == 1); /* out of range no-op */
    CHECK(pc_ui_wide_undo(&s) && s.params.n_constraints == 2);

    pc_ui_wide_fini(&s);
}

static void test_nearest_and_commit(void)
{
    pc_ui_wide_dialog s = {0};
    CHECK(pc_ui_wide_begin(&s, 1, 55, FRAME, NULL) == PC_OK);
    double a[2] = {30, 50}, b[2] = {120, 50};
    CHECK(pc_ui_wide_add(&s, a, b, PC_ALGO_WIDE_FREE, NULL) == PC_OK);
    size_t idx = 999;
    CHECK(pc_ui_wide_nearest(&s, 75, 50, 1.0, &idx) && idx == 0);
    CHECK(!pc_ui_wide_nearest(&s, 75, 95, 1.0, &idx)); /* far away */

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = NULL;
    CHECK(pc_ui_wide_commit_params(&s, doc, &params, NULL) == PC_OK);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(params, "layer")) == 55);
    CHECK(yyjson_mut_obj_get(params, "constraints") != NULL);
    yyjson_mut_doc_free(doc);
    pc_ui_wide_fini(&s);
}

static void test_hostile(void)
{
    pc_ui_wide_dialog s = {0};
    CHECK(pc_ui_wide_begin(&s, 1, 1, FRAME, NULL) == PC_OK);
    CHECK(pc_ui_wide_pointer(&s, PC_UI_POINTER_DOWN, NAN, 10, 1.0, false, NULL) != PC_OK);
    double a[2] = {NAN, 0}, b[2] = {1, 1};
    CHECK(pc_ui_wide_add(&s, a, b, PC_ALGO_WIDE_FREE, NULL) != PC_OK);
    CHECK(pc_ui_wide_apply_json(&s, NULL, NULL) != PC_OK);
    pc_ui_wide_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_pointer_add_and_orientation);
    RUN(test_set_remove_undo_redo);
    RUN(test_nearest_and_commit);
    RUN(test_hostile);
    return pc_t_finish();
}
