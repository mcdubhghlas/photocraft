#include "gpu/fx_paint.h"
#include "compose/effects_util.h"
#include "compose/fill_layout.h"

/* Prepare colour only. Coverage, effect opacity and all blending remain GPU operations. */
pc_status pc_gpu_paint_pixels(const pc_gpu_paint *paint, pc_color_mode mode, pc_rect region,
                              pc_compose_buffer *out, pc_error *err)
{
    if (!paint || !out) {
        return pc_fail(err, PC_ERR_INVALID, "invalid GPU effect paint");
    }
    if (paint->kind == PC_GPU_PAINT_COLOR) {
        return pc_compose_buffer_filled(region, paint->color, out, err);
    }
    if (paint->kind == PC_GPU_PAINT_PATTERN) {
        if (!paint->pattern) {
            return pc_compose_buffer_transparent(region, out, err);
        }
        pc_compose_pattern_tile *tile = NULL;
        pc_status rc = pc_compose_pattern_tile_new_in(paint->pattern, mode, &tile, err);
        if (rc == PC_OK) {
            rc = pc_compose_pattern_render(tile, &paint->placement, region, out, err);
        }
        pc_compose_pattern_tile_release(tile);
        return rc;
    }
    pc_status rc = pc_compose_buffer_transparent(region, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_gradient *g = paint->gradient;
    if (!g) {
        return PC_OK;
    }
    pc_compose_gradient_geom geometry =
        pc_compose_gradient_layout(g->style, g->angle, g->scale, g->offset, paint->frame);
    size_t w = pc_rect_width(region);
    for (size_t i = 0; i < out->len; i++) {
        float x = (float)((int64_t)region.x0 + (int64_t)(i % w)) + .5f;
        float y = (float)((int64_t)region.y0 + (int64_t)(i / w)) + .5f;
        float t = pc_compose_gradient_t(g->style, geometry.angle, geometry.scale, g->reverse, geometry.offset,
                                        paint->frame, x, y);
        pc_compose_fx_sample_gradient(g, t, out->px[i]);
    }
    return PC_OK;
}
