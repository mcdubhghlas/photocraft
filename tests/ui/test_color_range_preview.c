#include "harness.h"
#include "ui/color_range_preview.h"
#include <math.h>

static void test_mode_index(void)
{
    CHECK(pc_ui_color_range_preview_index("none") == 0);
    CHECK(pc_ui_color_range_preview_index("grayscale") == 1);
    CHECK(pc_ui_color_range_preview_index("quickMask") == 4);
    CHECK(pc_ui_color_range_preview_index(NULL) == 0);
    CHECK(pc_ui_color_range_preview_index("bogus") == 0);
}

static void test_overlay_pixels(void)
{
    uint8_t p[4] = {9, 9, 9, 9};
    /* None / unknown => fully transparent. */
    pc_ui_color_range_overlay_pixel("none", 0.5, p);
    CHECK(p[3] == 0);
    pc_ui_color_range_overlay_pixel(NULL, 0.5, p);
    CHECK(p[3] == 0);

    /* Grayscale: selected white, unselected black, opaque. */
    pc_ui_color_range_overlay_pixel("grayscale", 1.0, p);
    CHECK(p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255);
    pc_ui_color_range_overlay_pixel("grayscale", 0.0, p);
    CHECK(p[0] == 0 && p[3] == 255);

    /* Mattes cover unselected pixels (alpha high where coverage low). */
    pc_ui_color_range_overlay_pixel("blackMatte", 0.0, p);
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 255);
    pc_ui_color_range_overlay_pixel("blackMatte", 1.0, p);
    CHECK(p[3] == 0);
    pc_ui_color_range_overlay_pixel("whiteMatte", 0.0, p);
    CHECK(p[0] == 255 && p[1] == 255 && p[2] == 255 && p[3] == 255);

    /* Quick Mask: red, half strength over unselected. */
    pc_ui_color_range_overlay_pixel("quickMask", 0.0, p);
    CHECK(p[0] == 255 && p[1] == 0 && p[2] == 0);
    CHECK(p[3] >= 127 && p[3] <= 128);
    pc_ui_color_range_overlay_pixel("quickMask", 1.0, p);
    CHECK(p[3] == 0);

    /* Out-of-range / NaN coverage is clamped, never crashes. */
    pc_ui_color_range_overlay_pixel("grayscale", NAN, p);
    CHECK(p[0] == 0);
    pc_ui_color_range_overlay_pixel("grayscale", 5.0, p);
    CHECK(p[0] == 255);
    pc_ui_color_range_overlay_pixel("grayscale", 0.5, NULL); /* no crash */
}

int main(int argc, char **argv)
{
    RUN(test_mode_index);
    RUN(test_overlay_pixels);
    return pc_t_finish();
}
