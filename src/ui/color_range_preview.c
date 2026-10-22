#include "ui/color_range_preview.h"

#include <math.h>
#include <string.h>

const char *const PC_UI_COLOR_RANGE_PREVIEW_IDS[PC_UI_COLOR_RANGE_PREVIEW_MODES] = {
    "none", "grayscale", "blackMatte", "whiteMatte", "quickMask"};
const char *const PC_UI_COLOR_RANGE_PREVIEW_LABELS[PC_UI_COLOR_RANGE_PREVIEW_MODES] = {
    "None", "Grayscale", "Black Matte", "White Matte", "Quick Mask"};

size_t pc_ui_color_range_preview_index(const char *id)
{
    for (size_t i = 0; id && i < PC_UI_COLOR_RANGE_PREVIEW_MODES; i++)
        if (!strcmp(id, PC_UI_COLOR_RANGE_PREVIEW_IDS[i]))
            return i;
    return 0;
}

static uint8_t quant(double v)
{
    if (!(v > 0))
        return 0;
    if (v >= 1)
        return 255;
    return (uint8_t)(v * 255.0 + 0.5);
}

void pc_ui_color_range_overlay_pixel(const char *mode, double coverage, uint8_t rgba[4])
{
    if (!rgba)
        return;
    if (!isfinite(coverage) || coverage < 0)
        coverage = 0;
    else if (coverage > 1)
        coverage = 1;
    uint8_t cov = quant(coverage), inv = quant(1.0 - coverage);
    rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
    if (!mode)
        return;
    if (!strcmp(mode, "grayscale")) {
        rgba[0] = rgba[1] = rgba[2] = cov;
        rgba[3] = 255;
    } else if (!strcmp(mode, "blackMatte")) {
        rgba[3] = inv;
    } else if (!strcmp(mode, "whiteMatte")) {
        rgba[0] = rgba[1] = rgba[2] = 255;
        rgba[3] = inv;
    } else if (!strcmp(mode, "quickMask")) {
        rgba[0] = 255;
        rgba[3] = quant(0.5 * (1.0 - coverage));
    }
}
