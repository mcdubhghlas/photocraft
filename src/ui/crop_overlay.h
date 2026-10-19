#ifndef PC_UI_CROP_OVERLAY_H
#define PC_UI_CROP_OVERLAY_H
#include "ui/tool_options.h"
#define PC_UI_CROP_GUIDE_MAX 256
/* Unit-square line segments, bounded independently of screen dimensions. */
size_t pc_ui_crop_guides(pc_ui_crop_overlay kind, unsigned orientation, float width, float height,
                         float segments[PC_UI_CROP_GUIDE_MAX][4]);
#endif
