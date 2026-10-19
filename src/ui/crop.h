#ifndef PC_UI_CROP_H
#define PC_UI_CROP_H
#include "ui/transform.h"
typedef struct pc_ui_crop_frame {
    bool present;
    double rect[4], angle;
} pc_ui_crop_frame;
typedef struct pc_ui_crop_drag {
    bool active, draw, drawn, image;
    double anchor[2], last[2];
    pc_ui_crop_frame before;
    pc_ui_transform_drag transform;
} pc_ui_crop_drag;
bool pc_ui_crop_valid(const pc_ui_crop_frame *frame);
void pc_ui_crop_quad(const pc_ui_crop_frame *frame, pc_ui_transform_frame *out);
bool pc_ui_crop_begin(pc_ui_crop_drag *drag, const pc_ui_crop_frame *frame, const double point[2],
                      double tolerance, bool default_frame, bool classic);
bool pc_ui_crop_motion(pc_ui_crop_frame *frame, pc_ui_crop_drag *drag, const double point[2], double ratio,
                       bool shift, bool alt, bool space);
bool pc_ui_crop_swap(pc_ui_crop_frame *frame);
#endif
