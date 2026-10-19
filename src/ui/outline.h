/* Selection-mask boundaries (outline.rs): threshold > .5, merged in document coordinates.
 * Output is single-owned; init to zero and fini after use. Trace replaces it only on success.
 * Serial sampling deliberately avoids waiting for the background filter worker pool. */
#ifndef PC_UI_OUTLINE_H
#define PC_UI_OUTLINE_H
#include "raster/surface.h"
typedef struct pc_ui_segment {
    int32_t a[2], b[2];
} pc_ui_segment;
typedef struct pc_ui_outline {
    pc_ui_segment *segments;
    size_t count, capacity;
} pc_ui_outline;
void pc_ui_outline_fini(pc_ui_outline *outline);
pc_status pc_ui_outline_trace(const pc_surface *mask, pc_rect bounds, uint32_t step, pc_ui_outline *out,
                              pc_error *err);
#endif
