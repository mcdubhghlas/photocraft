/* Preview proxy factor math (port of crates/ui-egui/src/proxy.rs). */
#include "ui/proxy.h"

#include <math.h>

uint32_t pc_ui_proxy_factor(uint64_t area)
{
    if (area <= PC_UI_PREVIEW_PIXELS * 2) {
        return 1;
    }
    /* ((area / PREVIEW).sqrt().ceil() as u32).max(2) */
    double k = ceil(sqrt((double)area / (double)PC_UI_PREVIEW_PIXELS));
    uint32_t f = (uint32_t)k;
    return f < 2 ? 2 : f;
}

uint32_t pc_ui_proxy_preview_factor(uint64_t area, bool reduced)
{
    return reduced ? pc_ui_proxy_factor(area) : 1;
}
