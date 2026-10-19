#ifndef PC_UI_TRANSFORM_H
#define PC_UI_TRANSFORM_H
#include "base/json.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef enum pc_ui_transform_mode {
    PC_TRANSFORM_FREE = 0,
    PC_TRANSFORM_ROTATE,
    PC_TRANSFORM_SKEW,
    PC_TRANSFORM_DISTORT,
    PC_TRANSFORM_PERSPECTIVE
} pc_ui_transform_mode;
const char *pc_ui_transform_mode_serde(pc_ui_transform_mode mode);
pc_ui_transform_mode pc_ui_transform_mode_for_command(const char *command);
typedef struct pc_ui_transform_frame {
    double rect[4], quad[4][2], pivot[2];
} pc_ui_transform_frame;
typedef enum pc_ui_transform_part {
    PC_TRANSFORM_CORNER,
    PC_TRANSFORM_EDGE,
    PC_TRANSFORM_PIVOT,
    PC_TRANSFORM_INSIDE,
    PC_TRANSFORM_OUTSIDE
} pc_ui_transform_part;
typedef struct pc_ui_transform_hit {
    pc_ui_transform_part part;
    size_t index;
} pc_ui_transform_hit;
typedef struct pc_ui_transform_drag {
    pc_ui_transform_hit hit;
    double start[2];
    pc_ui_transform_frame before;
} pc_ui_transform_drag;
typedef struct pc_ui_transform {
    bool active, selection, type_layer, can_undo, can_redo, path, copy;
    char path_name[1025];
    uint64_t session, document, revision, layer;
    unsigned target; /* 0 layer pixels/linked mask; 1 mask; 2 Quick Mask; 3 alpha */
    size_t alpha;
    pc_ui_transform_mode mode;
    pc_ui_transform_frame frame;
    char interpolation[16];
} pc_ui_transform;
bool pc_ui_transform_frame_valid(const pc_ui_transform_frame *frame);
void pc_ui_transform_frame_rect(pc_ui_transform_frame *frame, const double rect[4]);
pc_ui_transform_hit pc_ui_transform_hit_test(const pc_ui_transform_frame *frame, const double point[2],
                                             double tolerance);
/* Pure, bounded drag maths. Invalid/degenerate results preserve the current frame. */
bool pc_ui_transform_apply(pc_ui_transform_frame *frame, const pc_ui_transform_drag *drag,
                           const double point[2], pc_ui_transform_mode mode, bool type_layer, bool shift,
                           bool alt, bool command, bool legacy, bool unlinked);
/* Reference point 0..8 is the 3x3 grid, 9 is the movable pivot. */
void pc_ui_transform_measure(const pc_ui_transform_frame *frame, unsigned reference, double values[7]);
bool pc_ui_transform_numeric(pc_ui_transform_frame *frame, unsigned reference, unsigned field, double value,
                             bool linked);
pc_status pc_ui_transform_to_json(const pc_ui_transform *transform, yyjson_mut_doc *doc, yyjson_mut_val **out,
                                  pc_error *err);
pc_status pc_ui_transform_from_json(pc_ui_transform *transform, yyjson_val *value, pc_error *err);
#endif
