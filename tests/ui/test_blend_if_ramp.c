#include "harness.h"
#include "ui/blend_if_ramp.h"

static void test_nearest_and_coincident_side(void)
{
    uint8_t q[4] = {0, 0, 255, 255};
    /* Coinciding black pair at 0: pointer to the right grabs the high half (1). */
    CHECK(pc_ui_blend_if_nearest(q, 5) == 1);
    CHECK(pc_ui_blend_if_nearest(q, -5) == 0);
    /* Coinciding white pair at 255. */
    CHECK(pc_ui_blend_if_nearest(q, 260) == 3);
    CHECK(pc_ui_blend_if_nearest(q, 250) == 2);
    uint8_t spread[4] = {10, 40, 200, 230};
    CHECK(pc_ui_blend_if_nearest(spread, 38) == 1);
    CHECK(pc_ui_blend_if_nearest(spread, 205) == 2);
    CHECK(pc_ui_blend_if_nearest(NULL, 5) == 0);
}

static void test_unsplit_moves_pair_together(void)
{
    uint8_t q[4] = {10, 30, 255, 255};
    /* Dragging the black-low handle unsplit moves both black halves by the same delta. */
    CHECK(pc_ui_blend_if_drag(q, 0, 20, false));
    CHECK(q[0] == 20 && q[1] == 40);
    /* No change returns false. */
    CHECK(!pc_ui_blend_if_drag(q, 0, 20, false));
}

static void test_split_moves_one_half(void)
{
    uint8_t q[4] = {0, 0, 255, 255};
    CHECK(pc_ui_blend_if_drag(q, 1, 60, true));
    CHECK(q[0] == 0 && q[1] == 60);
    CHECK(q[2] == 255 && q[3] == 255);
}

static void test_order_is_restored(void)
{
    uint8_t q[4] = {0, 0, 100, 100};
    /* Push black-high past white-low: white handles get shoved up to keep order. */
    CHECK(pc_ui_blend_if_drag(q, 1, 150, true));
    CHECK(q[0] <= q[1] && q[1] <= q[2] && q[2] <= q[3]);
    CHECK(q[1] == 150 && q[2] >= 150 && q[3] >= 150);
    /* Clamp to 0..255. */
    uint8_t r[4] = {0, 0, 255, 255};
    CHECK(pc_ui_blend_if_drag(r, 3, 999, true) == false || r[3] == 255);
    CHECK(r[3] == 255);
    CHECK(!pc_ui_blend_if_drag(r, 9, 10, true));
}

int main(int argc, char **argv)
{
    RUN(test_nearest_and_coincident_side);
    RUN(test_unsplit_moves_pair_together);
    RUN(test_split_moves_one_half);
    RUN(test_order_is_restored);
    return pc_t_finish();
}
