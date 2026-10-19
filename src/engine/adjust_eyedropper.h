#ifndef PC_ENGINE_ADJUST_EYEDROPPER_H
#define PC_ENGINE_ADJUST_EYEDROPPER_H
#include "engine/engine.h"
typedef enum pc_engine_tone_picker {
    PC_TONE_PICK_BLACK,
    PC_TONE_PICK_GRAY,
    PC_TONE_PICK_WHITE
} pc_engine_tone_picker;
bool pc_engine_tone_picker_supported(pc_color_mode mode, pc_engine_tone_picker picker);
pc_status pc_engine_adjustment_pick(const char *kind, yyjson_val *params, const pc_adjustment *base,
                                    pc_engine_tone_picker picker, const float sample[3], pc_color_mode mode,
                                    pc_adjustment *out, pc_error *err);
/* No eyedropper key: present=false, out untouched. A layer pick samples the composite with that
 * layer hidden, matching its Properties histogram. Other commands sample the visible composite. */
pc_status pc_engine_adjustment_eyedropper(const pc_engine_session *session, const char *kind,
                                          yyjson_val *params, const pc_adjustment *base, bool hide,
                                          pc_layer_id layer, bool *present, pc_adjustment *out,
                                          pc_error *err);
#endif
