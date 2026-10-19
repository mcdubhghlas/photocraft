#ifndef PC_UI_BRUSH_SETTINGS_H
#define PC_UI_BRUSH_SETTINGS_H
#include "base/json.h"
#include "paint/brush.h"
/* Scalar editor snapshot: excludes potentially large bitmap resources and colors. The
 * session's full brush remains the base for each command patch. */
yyjson_mut_val *pc_ui_brush_settings_fields(yyjson_mut_doc *doc, const pc_paint_brush *brush);
pc_status pc_ui_brush_setting_patch(yyjson_mut_doc *doc, const char *path, yyjson_mut_val *value,
                                    yyjson_mut_val **out, pc_error *err);
#endif
