#include "harness.h"
#include "ui/brush_settings.h"
#include "engine/engine.h"
#include "paint/tile.h"

static void test_scalar_snapshot_excludes_resources(void)
{
    pc_paint_brush brush;
    pc_paint_brush_default(&brush);
    brush.tip.kind = PC_PAINT_TIP_SAMPLED;
    const float pixels[4] = {0, 1, 1, 0};
    CHECK(pc_paint_gray_tile_from_f32(2, 2, pixels, 4, &brush.tip.tile, NULL) == PC_OK);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK(d != NULL);
    yyjson_mut_val *fields = pc_ui_brush_settings_fields(d, &brush);
    CHECK(fields != NULL);
    CHECK(yyjson_mut_obj_get(fields, "tip") == NULL);
    CHECK(yyjson_mut_obj_get(fields, "color") == NULL);
    CHECK(yyjson_mut_obj_get(yyjson_mut_obj_get(fields, "dualBrush"), "tip") == NULL);
    CHECK(yyjson_mut_obj_get(yyjson_mut_obj_get(fields, "texture"), "pattern") == NULL);
    CHECK(brush.tip.kind == PC_PAINT_TIP_SAMPLED);
    CHECK(pc_paint_tip_bitmap(&brush.tip) != NULL);
    CHECK(yyjson_mut_is_obj(yyjson_mut_obj_get(fields, "shapeDynamics")));
    CHECK(yyjson_mut_is_obj(yyjson_mut_obj_get(fields, "locks")));
    yyjson_mut_doc_free(d);
    pc_paint_brush_fini(&brush);
}
static void test_patch_changes_only_its_setting(void)
{
    pc_engine_session *s = NULL;
    CHECK(pc_engine_session_new(&s, NULL) == PC_OK);
    pc_json_value out = PC_JSON_VALUE_INIT;
    CHECK(pc_engine_session_execute_json(
              s, "tools.setBrush",
              "{\"brush\":{\"size\":123,\"hardness\":0.25,\"texture\":{\"enabled\":true}}}", &out,
              NULL) == PC_OK);
    pc_json_value_fini(&out);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    CHECK(d != NULL);
    yyjson_mut_val *patch = NULL;
    CHECK(pc_ui_brush_setting_patch(d, "transfer.opacity.minimum", yyjson_mut_real(d, .37), &patch, NULL) ==
          PC_OK);
    CHECK(yyjson_mut_get_num(yyjson_mut_obj_get(
              yyjson_mut_obj_get(yyjson_mut_obj_get(patch, "transfer"), "opacity"), "minimum")) == .37);
    yyjson_mut_val *params = yyjson_mut_obj(d);
    CHECK(yyjson_mut_obj_add_val(d, params, "brush", patch));
    pc_buf json = PC_BUF_INIT;
    CHECK(pc_json_write(params, 0, &json, NULL) == PC_OK);
    CHECK(pc_engine_session_execute_json(s, "tools.setBrush", pc_buf_cstr(&json), &out, NULL) == PC_OK);
    const pc_paint_brush *brush = &pc_engine_session_tools(s)->brush;
    CHECK_NEAR(brush->transfer.opacity.minimum, .37f, 0);
    CHECK_NEAR(brush->size, 123, 0);
    CHECK_NEAR(brush->hardness, .25f, 0);
    CHECK(brush->texture.enabled);
    CHECK(pc_ui_brush_setting_patch(d, "bad..path", yyjson_mut_bool(d, true), &patch, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_ui_brush_setting_patch(d, "a.b.c.d.e.f.g.h.i", yyjson_mut_bool(d, true), &patch, NULL) ==
          PC_ERR_LIMIT);
    pc_buf_fini(&json);
    pc_json_value_fini(&out);
    yyjson_mut_doc_free(d);
    pc_engine_session_free(s);
}
int main(int argc, char **argv)
{
    RUN(test_scalar_snapshot_excludes_resources);
    RUN(test_patch_changes_only_its_setting);
    return pc_t_finish();
}
