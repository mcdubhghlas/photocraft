/* Persistent display options, commands, and scrollbar gesture contracts. */
#include <math.h>
#include <string.h>
#include "harness.h"
#include "ui/canvas.h"
#include "ui/view_options.h"

static void test_persistence(void)
{
    pc_ui_view_options o;
    pc_ui_view_options_init(&o);
    CHECK(o.extras && o.pixel_aspect_correction && !o.flip_horizontal);
    CHECK(pc_ui_view_standard(&o) && !pc_ui_view_hides_chrome(&o));
    CHECK(!o.show[0] && o.show[1] && !o.show[8]);
    const char *text = "{\"screen_mode\":\"future\\u0000mode\",\"pixel_aspect\":\"custom:1.5:test\","
                       "\"flip_horizontal\":true,\"show\":{\"pixel_grid\":false},\"guide_layout\":{\"columns\":3}}";
    yyjson_doc *doc = NULL;
    CHECK(pc_json_parse(text, strlen(text), &doc, NULL) == PC_OK);
    CHECK(pc_ui_view_options_from_json(yyjson_doc_get_root(doc), &o, NULL) == PC_OK);
    CHECK(o.flip_horizontal && !pc_ui_view_standard(&o));
    CHECK(!*pc_ui_view_show_flag(&o, "pixelGrid"));
    CHECK(*pc_ui_view_snap_flag(&o, "documentBounds"));
    CHECK(pc_ui_view_show_flag(&o, "unknown") == NULL);
    CHECK(o.screen_mode.len == 11);
    CHECK_NEAR(pc_ui_view_display_aspect(&o), 1.5f, 0);
    yyjson_mut_doc *saved = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_view_options_to_json(&o, saved, &root, NULL) == PC_OK);
    CHECK(yyjson_mut_get_len(yyjson_mut_obj_get(root, "screen_mode")) == 11);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(yyjson_mut_obj_get(root, "guide_layout"), "columns")) == 3);
    yyjson_mut_doc_free(saved);
    yyjson_doc_free(doc);
    text = "{\"flip_horizontal\":false,\"screen_mode\":42}";
    CHECK(pc_json_parse(text, strlen(text), &doc, NULL) == PC_OK);
    CHECK(pc_ui_view_options_from_json(yyjson_doc_get_root(doc), &o, NULL) == PC_ERR_INVALID);
    CHECK(o.flip_horizontal && o.screen_mode.len == 11); /* transactional failure */
    yyjson_doc_free(doc);
    pc_ui_view_options_fini(&o);
}

static void test_commands(void)
{
    pc_ui_view_options o;
    pc_ui_view_options_init(&o);
    pc_json_value result = PC_JSON_VALUE_INIT;
    CHECK(pc_ui_view_options_command(&o, "view.flipHorizontal", NULL, &result, NULL) == PC_OK);
    CHECK(o.flip_horizontal && yyjson_mut_is_true(pc_json_value_root(&result)));
    pc_json_value_fini(&result);
    const char *modes[] = {"fullScreenWithMenuBar", "fullScreen", "standard"};
    for (size_t i = 0; i < 3; i++) {
        CHECK(pc_ui_view_options_command(&o, "view.screenMode.cycle", NULL, &result, NULL) == PC_OK);
        CHECK(strcmp(pc_ui_view_screen_mode(&o), modes[i]) == 0);
        CHECK(pc_ui_view_hides_chrome(&o) == (i == 1));
        pc_json_value_fini(&result);
    }
    CHECK(pc_ui_view_options_command(&o, "view.pixelAspectRatio.anamorphic2To1", NULL, &result, NULL) ==
          PC_OK);
    CHECK_NEAR(pc_ui_view_display_aspect(&o), 2, 0);
    pc_json_value_fini(&result);
    CHECK(pc_ui_view_options_command(&o, "view.pixelAspectRatioCorrection", NULL, &result, NULL) == PC_OK);
    CHECK_NEAR(pc_ui_view_display_aspect(&o), 1, 0);
    pc_json_value_fini(&result);
    CHECK(pc_ui_view_options_command(&o, "view.screenMode.bogus", NULL, &result, NULL) == PC_ERR_NOT_FOUND);
    CHECK(pc_ui_view_standard(&o) && !result.doc);
    CHECK_NEAR(pc_ui_pixel_aspect_ratio("custom:0x2p0"), 1, 0);
    CHECK_NEAR(pc_ui_pixel_aspect_ratio("custom: 2"), 1, 0);
    CHECK_NEAR(pc_ui_pixel_aspect_ratio("custom:2e0:label"), 2, 0);
    pc_ui_view_options_fini(&o);
}

static void test_scroll_gestures(void)
{
    uint32_t size[2] = {2000, 1000};
    pc_ui_canvas_xform xf = {
        .x = 20, .y = 30, .width = 512, .height = 212, .zoom = 1, .aspect = 1, .center = {1000, 500}};
    pc_ui_scrollbar bars[2];
    CHECK(pc_ui_scrollbars_layout(&xf, size, false, bars));
    CHECK(bars[0].shown && bars[1].shown);
    CHECK_NEAR(bars[0].rect[1], 230, 0);
    float x = bars[0].rect[0] + bars[0].thumb[0] + bars[0].thumb[1] / 2, y = 236;
    pc_ui_scroll_drag drag = {0};
    CHECK(pc_ui_scrollbar_press(bars, x, y, &drag) && drag.thumb);
    float center = -1, scale = drag.scale;
    CHECK(pc_ui_scrollbar_motion(&drag, x + 25, y, false, &center));
    CHECK_NEAR(center, 1000 + 25 * scale, 1e-4);
    /* A new layout does not change the captured drag scale/extent. */
    xf.center[0] = center;
    CHECK(pc_ui_scrollbars_layout(&xf, size, true, bars));
    CHECK(pc_ui_scrollbar_motion(&drag, x + 50, y, true, &center));
    CHECK_NEAR(center, 1000 + 50 * scale, 1e-4);
    CHECK(!drag.active);
    xf.center[0] = 1000;
    CHECK(pc_ui_scrollbars_layout(&xf, size, false, bars));
    CHECK(pc_ui_scrollbar_press(bars, 21, y, &drag) && !drag.thumb);
    CHECK(!pc_ui_scrollbar_motion(&drag, 21, y, false, &center));
    CHECK(pc_ui_scrollbar_motion(&drag, 21, y, true, &center));
    CHECK_NEAR(center, 488, 0); /* page = 512 document pixels */
    CHECK(pc_ui_scrollbar_press(bars, 21, y, &drag));
    CHECK(!pc_ui_scrollbar_motion(&drag, 26, y, true, &center)); /* track drag isn't a click */
    xf.flip = true;
    CHECK(pc_ui_scrollbars_layout(&xf, size, false, bars));
    CHECK(pc_ui_scrollbar_press(bars, x, y, &drag));
    CHECK(pc_ui_scrollbar_motion(&drag, x + 25, y, true, &center));
    CHECK_NEAR(center, 1000 - 25 * scale, 1e-4);
    CHECK(pc_ui_scrollbar_press(bars, x, y, &drag));
    CHECK(!pc_ui_scrollbar_motion(&drag, NAN, y, true, &center) && !drag.active);
    pc_ui_view view;
    pc_ui_view_default(&view);
    view.doc_size[0] = 2000;
    view.doc_size[1] = 1000;
    view.zoom = 1;
    view.center[0] = -100;
    view.center[1] = 2000;
    CHECK(pc_ui_scroll_clamp_view(&view, 512, 212, 1, 2));
    CHECK_NEAR(view.center[0], 128, 0);
    CHECK_NEAR(view.center[1], 894, 0);
    view.rotation = 90;
    view.center[0] = -1000;
    view.center[1] = 2000;
    CHECK(pc_ui_scroll_clamp_view(&view, 512, 212, 1, 1));
    CHECK(!pc_ui_scroll_clamp_view(&view, 512, 212, 1, 1));
    xf.width = NAN;
    CHECK(!pc_ui_scrollbars_layout(&xf, size, true, bars));
}

int main(int argc, char **argv)
{
    RUN(test_persistence);
    RUN(test_commands);
    RUN(test_scroll_gestures);
    return pc_t_finish();
}
