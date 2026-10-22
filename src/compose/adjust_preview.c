/* UI preview helper: apply one adjustment to a small caller-provided RGBA8 buffer
 * (pc_compose_adjust_apply_rgba8). Lets the shell draw colour-tracked sliders, adjusted spectrum
 * bars and live mini-previews without a full document render. Never panics on any params or size
 * (REWRITE_PLAN.md rule 9): sizes are bounded before allocation and the buffer is left unchanged on
 * failure. */
#include "compose/adjust.h"

#include <stdlib.h>

#include "base/status.h"
#include "compose/compose.h"
#include "doc/adjust.h"
#include "geom/geom.h"

pc_status pc_compose_adjust_apply_rgba8(const pc_adjustment *adj, uint8_t *rgba, uint32_t w, uint32_t h,
                                        pc_error *err)
{
    if (adj == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "adjustment preview: no adjustment");
    }
    if (w == 0 || h == 0) {
        return PC_OK; /* nothing to adjust */
    }
    if (rgba == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "adjustment preview: no buffer");
    }
    uint64_t pixels = (uint64_t)w * (uint64_t)h;
    if (pixels > PC_COMPOSE_PREVIEW_MAX_PIXELS) {
        return pc_fail(err, PC_ERR_LIMIT, "adjustment preview: %llu pixels exceeds the limit",
                       (unsigned long long)pixels);
    }

    pc_compose_buffer buf;
    pc_rect rect = pc_rect_make(0, 0, (int32_t)w, (int32_t)h);
    pc_status st = pc_compose_buffer_transparent(rect, &buf, err);
    if (st != PC_OK) {
        return st;
    }
    /* Load the caller's straight-alpha RGBA8 into the float buffer. */
    for (size_t i = 0; i < buf.len; i++) {
        for (size_t c = 0; c < 4; c++) {
            buf.px[i][c] = (float)rgba[i * 4 + c] / 255.0f;
        }
    }
    st = pc_compose_adjust_apply(adj, &buf, err);
    if (st != PC_OK) {
        pc_compose_buffer_fini(&buf);
        return st;
    }
    /* Clamp to [0, 1] and round back to 8 bits. */
    for (size_t i = 0; i < buf.len; i++) {
        for (size_t c = 0; c < 4; c++) {
            float v = buf.px[i][c];
            if (!(v > 0.0f)) {
                v = 0.0f;
            } else if (v > 1.0f) {
                v = 1.0f;
            }
            rgba[i * 4 + c] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
    pc_compose_buffer_fini(&buf);
    return PC_OK;
}
