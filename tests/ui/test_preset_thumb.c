#include "harness.h"
#include "ui/preset_thumb.h"
#include <math.h>

static void test_thumb_px_clamps(void)
{
    /* In range: rounds logical * scale. */
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(40, 1.0), 40);
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(40, 1.5), 60);
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(40.4, 1.0), 40);
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(40.6, 1.0), 41);
    /* Below the floor clamps up to the engine minimum. */
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(4, 1.0), PC_UI_PRESET_THUMB_MIN);
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(8, 1.0), PC_UI_PRESET_THUMB_MIN);
    /* Above the ceiling clamps down to the engine maximum. */
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(40, 100.0), PC_UI_PRESET_THUMB_MAX);
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(1000, 1.0), PC_UI_PRESET_THUMB_MAX);
    /* Hostile inputs never escape the range. */
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(-5, 2.0), PC_UI_PRESET_THUMB_MIN);
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(40, -1.0), PC_UI_PRESET_THUMB_MIN);
    /* Non-finite products clamp to the safe floor (never a huge or junk request). */
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(NAN, 1.0), PC_UI_PRESET_THUMB_MIN);
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(40, INFINITY), PC_UI_PRESET_THUMB_MIN);
    CHECK_EQ_UINT(pc_ui_preset_thumb_px(INFINITY, 1.0), PC_UI_PRESET_THUMB_MIN);
}

int main(int argc, char **argv)
{
    RUN(test_thumb_px_clamps);
    return pc_t_finish();
}
