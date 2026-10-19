/* Per-tool options and shared Crop/Shape settings, independent of the native widget toolkit. */
#ifndef PC_UI_TOOL_OPTIONS_H
#define PC_UI_TOOL_OPTIONS_H
#include "base/json.h"
#include "doc/doc.h"
#include "ui/theme.h"
typedef enum pc_ui_crop_overlay {
    PC_UI_CROP_THIRDS,
    PC_UI_CROP_GRID,
    PC_UI_CROP_DIAGONAL,
    PC_UI_CROP_TRIANGLE,
    PC_UI_CROP_GOLDEN_RATIO,
    PC_UI_CROP_GOLDEN_SPIRAL
} pc_ui_crop_overlay;
typedef enum pc_ui_overlay_show {
    PC_UI_OVERLAY_AUTO,
    PC_UI_OVERLAY_ALWAYS,
    PC_UI_OVERLAY_NEVER
} pc_ui_overlay_show;
typedef struct pc_ui_crop_shield {
    bool classic_mode, auto_center_preview, show_cropped_area, enabled, custom, auto_adjust;
    uint8_t custom_color[3];
    float opacity;
} pc_ui_crop_shield;
void pc_ui_crop_shield_default(pc_ui_crop_shield *out);
bool pc_ui_crop_shield_fill(const pc_ui_crop_shield *shield, pc_ui_color canvas, bool editing,
                            pc_ui_color *out);
const char *pc_ui_crop_overlay_id(pc_ui_crop_overlay overlay);
unsigned pc_ui_crop_overlay_orientations(pc_ui_crop_overlay overlay);
#define PC_UI_OPTION_TYPE_BOOL bool
#define PC_UI_OPTION_TYPE_FLOAT float
#define PC_UI_OPTION_TYPE_U32 uint32_t
#define PC_UI_OPTION_TYPE_U8 uint8_t
#define PC_UI_OPTION_TYPE_STRING pc_str
#define PC_UI_OPTION_TYPE_BLEND pc_blend_mode
#define PC_UI_OPTION_TYPE_STROKE pc_shape_stroke
#define PC_UI_OPTION_TYPE_SHIELD pc_ui_crop_shield
#define PC_UI_OPTION_TYPE_OVERLAY pc_ui_crop_overlay
#define PC_UI_OPTION_TYPE_SHOW pc_ui_overlay_show
typedef struct pc_ui_tool_options {
#define PC_UI_OPTION(kind, name, value) PC_UI_OPTION_TYPE_##kind name;
#include "ui/tool_options_fields.inc"
#undef PC_UI_OPTION
} pc_ui_tool_options;
#undef PC_UI_OPTION_TYPE_BOOL
#undef PC_UI_OPTION_TYPE_FLOAT
#undef PC_UI_OPTION_TYPE_U32
#undef PC_UI_OPTION_TYPE_U8
#undef PC_UI_OPTION_TYPE_STRING
#undef PC_UI_OPTION_TYPE_BLEND
#undef PC_UI_OPTION_TYPE_STROKE
#undef PC_UI_OPTION_TYPE_SHIELD
#undef PC_UI_OPTION_TYPE_OVERLAY
#undef PC_UI_OPTION_TYPE_SHOW
void pc_ui_tool_options_init(pc_ui_tool_options *options);
void pc_ui_tool_options_fini(pc_ui_tool_options *options);
pc_status pc_ui_tool_options_clone(const pc_ui_tool_options *source, pc_ui_tool_options *out, pc_error *err);
pc_status pc_ui_tool_options_to_json(const pc_ui_tool_options *options, yyjson_mut_doc *doc,
                                     yyjson_mut_val **out, pc_error *err);
/* Transactional; missing fields use defaults. Unknown fields are ignored, as serde does. */
pc_status pc_ui_tool_options_from_json(pc_ui_tool_options *options, yyjson_val *object, pc_error *err);
/* Transactional patch of the existing options; nested Shape/Crop settings are patched too. */
pc_status pc_ui_tool_options_patch(pc_ui_tool_options *options, yyjson_val *object, pc_error *err);
#endif
