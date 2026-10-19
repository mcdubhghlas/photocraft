#include "harness.h"
#include "ui/opacity_keys.h"

static void test_digits_and_repeat(void)
{
    pc_ui_opacity_pending p = {0};
    pc_ui_opacity_change out = pc_ui_opacity_press(&p, PC_TOOL_BRUSH, 4, false, false, 100);
    CHECK(out.target == PC_UI_OPACITY_BRUSH);
    CHECK_NEAR(out.value, .4f, 0);
    out = pc_ui_opacity_press(&p, PC_TOOL_BRUSH, 4, false, true, 200);
    CHECK(out.target == PC_UI_OPACITY_NONE && p.pending);
    out = pc_ui_opacity_press(&p, PC_TOOL_BRUSH, 5, false, false, 899);
    CHECK_NEAR(out.value, .45f, 0);
    CHECK(!p.pending);
    (void)pc_ui_opacity_press(&p, PC_TOOL_ERASER, 0, true, false, 1000);
    out = pc_ui_opacity_press(&p, PC_TOOL_ERASER, 0, true, false, 1100);
    CHECK(out.target == PC_UI_OPACITY_FLOW);
    CHECK_NEAR(out.value, .01f, 0);
    out = pc_ui_opacity_press(&p, PC_TOOL_PENCIL, 2, true, false, 2000);
    CHECK(out.target == PC_UI_OPACITY_NONE);
    out = pc_ui_opacity_press(&p, PC_TOOL_MOVE, 8, false, false, 3000);
    CHECK(out.target == PC_UI_OPACITY_LAYER);
    CHECK_NEAR(out.value, .8f, 0);
    out = pc_ui_opacity_press(&p, PC_TOOL_MOVE, 6, true, false, 3010);
    CHECK(out.target == PC_UI_OPACITY_FILL);
    CHECK_NEAR(out.value, .6f, 0);
}
static void test_boundaries_and_targets(void)
{
    pc_ui_opacity_pending p = {0};
    (void)pc_ui_opacity_press(&p, PC_TOOL_BRUSH, 2, false, false, 0);
    pc_ui_opacity_change out = pc_ui_opacity_press(&p, PC_TOOL_BRUSH, 3, false, false, 800);
    CHECK_NEAR(out.value, .3f, 0);
    out = pc_ui_opacity_press(&p, PC_TOOL_BRUSH, 0, false, false, 799);
    CHECK_NEAR(out.value, 1, 0); /* Clock reversal cannot combine. */
    out = pc_ui_opacity_press(&p, PC_TOOL_GRADIENT, 5, false, false, 2000);
    CHECK(out.target == PC_UI_OPACITY_TOOL);
    out = pc_ui_opacity_press(&p, PC_TOOL_PAINT_BUCKET, 7, true, false, 3000);
    CHECK(out.target == PC_UI_OPACITY_NONE);
    const pc_ui_tool tools[] = {PC_TOOL_SMUDGE, PC_TOOL_BLUR, PC_TOOL_DODGE, PC_TOOL_MAGIC_ERASER};
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
        out = pc_ui_opacity_press(&p, tools[i], 7, false, false, 4000 + i * 1000);
        CHECK(out.target == PC_UI_OPACITY_NONE);
    }
    pc_ui_opacity_pending saved = p;
    out = pc_ui_opacity_press(&p, PC_TOOL_BRUSH, 10, false, false, 9000);
    CHECK(out.target == PC_UI_OPACITY_NONE && p.time == saved.time);
    CHECK(pc_ui_opacity_press(NULL, PC_TOOL_BRUSH, 1, false, false, 0).target == PC_UI_OPACITY_NONE);
}
int main(int argc, char **argv)
{
    RUN(test_digits_and_repeat);
    RUN(test_boundaries_and_targets);
    return pc_t_finish();
}
