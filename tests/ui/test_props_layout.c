/* Properties-panel layout helpers (port of the pure parts of crates/ui-egui/src/props_layout.rs
 * tests; the egui header/section rendering lands with the Properties panel in U5). */
#include "harness.h"

#include <string.h>

#include "ui/props_layout.h"

static void test_kind_icon(void)
{
    CHECK_EQ_STR(pc_ui_props_kind_icon(PC_LAYER_ADJUSTMENT), "sliders-horizontal");
    CHECK_EQ_STR(pc_ui_props_kind_icon(PC_LAYER_GROUP), "folder");
    CHECK_EQ_STR(pc_ui_props_kind_icon(PC_LAYER_FILL), "paint-bucket");
    CHECK_EQ_STR(pc_ui_props_kind_icon(PC_LAYER_TEXT), "type");
    CHECK_EQ_STR(pc_ui_props_kind_icon(PC_LAYER_SHAPE), "pentagon");
    CHECK_EQ_STR(pc_ui_props_kind_icon(PC_LAYER_SMART), "app-window");
    CHECK_EQ_STR(pc_ui_props_kind_icon(PC_LAYER_RASTER), "image");
}

static void test_field_width(void)
{
    /* One column: avail - (label_w + gap). */
    float w1 = pc_ui_props_field_width(200.0f, 1, PC_UI_PROPS_LABEL_W);
    CHECK_NEAR(w1, 200.0f - (PC_UI_PROPS_LABEL_W + PC_UI_PROPS_LABEL_GAP), 1e-3);
    /* Two columns share the row, minus the between-column gap. */
    float w2 = pc_ui_props_field_width(360.0f, 2, PC_UI_PROPS_LABEL_W);
    float expect = (360.0f - 2.0f * (PC_UI_PROPS_LABEL_W + PC_UI_PROPS_LABEL_GAP) - PC_UI_PROPS_COL_GAP) / 2.0f;
    CHECK_NEAR(w2, expect, 1e-3);
    /* Never below the 36 px floor, and cols==0 is treated as 1 (no divide by zero). */
    CHECK_NEAR(pc_ui_props_field_width(10.0f, 4, PC_UI_PROPS_LABEL_W), 36.0f, 1e-3);
    CHECK(pc_ui_props_field_width(200.0f, 0, PC_UI_PROPS_LABEL_W) > 0.0f);
}

static void test_quick_actions(void)
{
    size_t n;
    const pc_ui_quick_action *a = pc_ui_props_quick_actions(PC_LAYER_RASTER, &n);
    CHECK_EQ_UINT(n, 2);
    CHECK_EQ_STR(a[0].id, "layer.removeBackground");
    CHECK_EQ_STR(a[1].id, "select.subject");
    a = pc_ui_props_quick_actions(PC_LAYER_TEXT, &n);
    CHECK_EQ_UINT(n, 5);
    CHECK_EQ_STR(a[0].label, "Convert to Shape");
    a = pc_ui_props_quick_actions(PC_LAYER_ADJUSTMENT, &n);
    CHECK_EQ_UINT(n, 0);
    CHECK(a == NULL);
    a = pc_ui_props_quick_actions(PC_LAYER_FILL, &n);
    CHECK_EQ_UINT(n, 1);
    CHECK_EQ_STR(a[0].id, "layer.rasterize.fillContent");
}

int main(int argc, char **argv)
{
    RUN(test_kind_icon);
    RUN(test_field_width);
    RUN(test_quick_actions);
    return pc_t_finish();
}
