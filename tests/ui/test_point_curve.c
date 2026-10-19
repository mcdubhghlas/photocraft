#include "harness.h"
#include "ui/point_curve.h"
#include <math.h>
static void test_insert_move_delete_limits(void)
{
    pc_ui_point_curve c = {0};
    CHECK(pc_ui_point_curve_load(&c, NULL, NULL) == PC_OK);
    size_t index = 99;
    CHECK(pc_ui_point_curve_insert(&c, 100, 150, &index));
    CHECK(index == 1 && c.count == 3);
    CHECK(pc_ui_point_curve_insert(&c, 101, 50, &index));
    CHECK(index == 1 && c.count == 3 && c.points[1][1] == 150);
    CHECK(pc_ui_point_curve_move(&c, 1, 999, -10));
    CHECK(c.points[1][0] == 254 && c.points[1][1] == 0);
    CHECK(!pc_ui_point_curve_delete(&c, 0));
    CHECK(!pc_ui_point_curve_delete(&c, 2));
    CHECK(pc_ui_point_curve_delete(&c, 1));
    CHECK(c.count == 2);
    CHECK(!pc_ui_point_curve_move(&c, SIZE_MAX, 1, 1));
    CHECK(!pc_ui_point_curve_insert(&c, NAN, 5, &index));
    for (size_t i = 1; i < 15; i++)
        CHECK(pc_ui_point_curve_insert(&c, (double)i * 16, 100, &index));
    CHECK(c.count == 16);
    CHECK(!pc_ui_point_curve_insert(&c, 250, 100, &index));
}
static void test_drag_offset_remove_and_reentry(void)
{
    pc_ui_point_curve c = {0};
    CHECK(pc_ui_point_curve_load(&c, NULL, NULL) == PC_OK);
    float r[4] = {10, 20, 255, 255};
    size_t index;
    CHECK(pc_ui_point_curve_insert(&c, 100, 150, &index));
    pc_ui_curve_edit e = pc_ui_point_curve_pointer(&c, r, 0, 114, 128, 0);
    CHECK(!e.changed && c.dragging && c.drag_index == 1);
    e = pc_ui_point_curve_pointer(&c, r, 1, 124, 118, 0);
    CHECK(e.changed && c.points[1][0] == 110 && c.points[1][1] == 160);
    e = pc_ui_point_curve_pointer(&c, r, 1, -20, 100, 0);
    CHECK(e.changed && c.count == 2 && c.removed && !c.has_selected);
    e = pc_ui_point_curve_pointer(&c, r, 1, 140, 120, 0);
    CHECK(e.changed && c.count == 3 && c.has_selected);
    e = pc_ui_point_curve_pointer(&c, r, 2, 140, 120, 0);
    CHECK(e.commit && !c.dragging);
    CHECK(pc_ui_point_curve_key(&c, PC_KEY_ARROWUP, PC_UI_MOD_SHIFT).changed);
    CHECK(c.points[c.selected][1] == 168);
    CHECK(pc_ui_point_curve_key(&c, PC_KEY_DELETE, 0).changed);
    CHECK(c.count == 2);
}
static void test_endpoints_and_json_rejection_atomicity(void)
{
    pc_ui_point_curve c = {0};
    CHECK(pc_ui_point_curve_load(&c, NULL, NULL) == PC_OK);
    float r[4] = {0, 0, 255, 255};
    CHECK(!pc_ui_point_curve_pointer(&c, r, 3, 0, 255, 0).changed);
    CHECK(c.count == 2);
    (void)pc_ui_point_curve_pointer(&c, r, 0, 0, 255, 0);
    (void)pc_ui_point_curve_pointer(&c, r, 1, -100, 400, 0);
    CHECK(c.count == 2 && c.points[0][0] == 0 && c.points[0][1] == 0);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *points = yyjson_mut_arr(doc);
    CHECK(pc_ui_point_curve_json(&c, doc, &points, NULL) == PC_OK);
    pc_ui_point_curve copy = {0};
    CHECK(pc_ui_point_curve_load(&copy, points, NULL) == PC_OK);
    CHECK(copy.count == 2 && !copy.dragging);
    yyjson_mut_val *invalid = yyjson_mut_arr(doc);
    CHECK(yyjson_mut_arr_add_int(doc, invalid, 0));
    CHECK(pc_ui_point_curve_load(&copy, invalid, NULL) != PC_OK);
    CHECK(copy.count == 2);
    float bad[4] = {0, 0, NAN, 1};
    CHECK(!pc_ui_point_curve_pointer(&c, bad, 0, 1, 1, 0).changed);
    yyjson_mut_doc_free(doc);
}
int main(int argc, char **argv)
{
    RUN(test_insert_move_delete_limits);
    RUN(test_drag_offset_remove_and_reentry);
    RUN(test_endpoints_and_json_rejection_atomicity);
    return pc_t_finish();
}
