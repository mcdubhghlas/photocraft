#include "harness.h"
#include "ui/color_picker.h"
#include "color/convert.h"
#include <math.h>
#include <string.h>
static void test_hex_hsv_and_field_inverses(void)
{
    float rgb[3] = {9, 9, 9};
    CHECK(pc_ui_picker_parse_hex("  ##3366CC  ", 12, rgb));
    CHECK_NEAR(rgb[0], .2f, 1e-7);
    CHECK_NEAR(rgb[2], .8f, 1e-7);
    CHECK(!pc_ui_picker_parse_hex("#é1122", strlen("#é1122"), rgb));
    CHECK_NEAR(rgb[0], .2f, 1e-7);
    CHECK(!pc_ui_picker_parse_hex("#123", 4, rgb));
    const char *const modes[] = {"h", "s", "v", "r", "g", "b", "lab_l", "lab_a", "lab_b"};
    for (unsigned r = 0; r <= 8; r++) {
        for (unsigned g = 0; g <= 8; g++) {
            for (unsigned b = 0; b <= 8; b++) {
                pc_ui_picker_components c = {.rgb = {(float)r / 8, (float)g / 8, (float)b / 8}};
                pc_ui_picker_rgb_to_hsv(c.rgb, c.hsv);
                pc_srgb_to_lab(c.rgb, c.lab);
                float back[3];
                pc_ui_picker_hsv_to_rgb(c.hsv, back);
                for (size_t i = 0; i < 3; i++) {
                    CHECK_NEAR(back[i], c.rgb[i], 1e-6);
                }
                for (size_t mode = 0; mode < 9; mode++) {
                    float axes[3];
                    pc_ui_picker_axes(modes[mode], &c, axes);
                    pc_ui_picker_field_color(modes[mode], axes[2], axes[0], axes[1], back);
                    for (size_t i = 0; i < 3; i++) {
                        CHECK_NEAR(back[i], c.rgb[i], 2e-4);
                    }
                }
            }
        }
    }
    float hsv[3] = {-120, 1, 1}, back[3];
    pc_ui_picker_hsv_to_rgb(hsv, back);
    CHECK_NEAR(back[2], 1, 1e-6);
}
static void test_retained_components_and_web(void)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    float gray[3] = {.5f, .5f, .5f};
    yyjson_mut_val *fields = pc_ui_picker_initial(doc, "foreground", "Picker", gray);
    float hsv[3] = {200, 0, .5f};
    CHECK(pc_ui_picker_set_rgb(doc, fields, gray, PC_UI_PICKER_HSV, hsv, NULL) == PC_OK);
    pc_ui_picker_components current;
    pc_ui_picker_current(fields, &current);
    CHECK_NEAR(current.hsv[0], 200, 0);
    float cyan[4] = {1, 0, 0, 0}, rgb[3];
    pc_cmyk_to_rgb(cyan, rgb);
    CHECK(pc_ui_picker_set_rgb(doc, fields, rgb, PC_UI_PICKER_CMYK, cyan, NULL) == PC_OK);
    pc_ui_picker_current(fields, &current);
    CHECK_NEAR(current.cmyk[0], 1, 0);
    CHECK_NEAR(current.cmyk[3], 0, 0);
    float lab[3] = {60, 127, 127};
    pc_lab_to_srgb(lab, rgb);
    CHECK(pc_ui_picker_set_rgb(doc, fields, rgb, PC_UI_PICKER_LAB, lab, NULL) == PC_OK);
    pc_ui_picker_current(fields, &current);
    CHECK_NEAR(current.lab[1], 127, 0);
    yyjson_mut_obj_put(fields, yyjson_mut_str(doc, "__webOnly"), yyjson_mut_bool(doc, true));
    float swatch[3] = {.443f, .263f, .263f};
    CHECK(pc_ui_picker_set_rgb(doc, fields, swatch, PC_UI_PICKER_HSV, hsv, NULL) == PC_OK);
    CHECK_EQ_STR(yyjson_mut_get_str(yyjson_mut_obj_get(fields, "color")), "#663333");
    CHECK(yyjson_mut_obj_get(fields, "__hsv") == NULL && yyjson_mut_obj_get(fields, "__lab") == NULL);
    float nearest[3];
    CHECK(pc_ui_picker_not_web_safe(swatch, nearest));
    CHECK(!pc_ui_picker_not_web_safe(nearest, rgb));
    float black[3] = {0};
    CHECK(!pc_ui_picker_out_of_gamut(black, nearest));
    yyjson_mut_doc_free(doc);
}
int main(int argc, char **argv)
{
    RUN(test_hex_hsv_and_field_inverses);
    RUN(test_retained_components_and_web);
    return pc_t_finish();
}
