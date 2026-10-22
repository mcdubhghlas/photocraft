#include "ui/preset_thumb.h"

#include <math.h>

uint32_t pc_ui_preset_thumb_px(double logical_side, double ui_scale)
{
    double px = logical_side * ui_scale;
    if (!isfinite(px) || px <= (double)PC_UI_PRESET_THUMB_MIN)
        return PC_UI_PRESET_THUMB_MIN;
    if (px >= (double)PC_UI_PRESET_THUMB_MAX)
        return PC_UI_PRESET_THUMB_MAX;
    return (uint32_t)(px + 0.5);
}
