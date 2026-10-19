#include "ui/canvas.h"
#include <math.h>
#include <stdlib.h>
#include "base/mem.h"
#include "base/num.h"

void pc_ui_canvas_tiles_fini(pc_ui_canvas_tiles *tiles)
{
    if (tiles) {
        for (size_t i = 0; i < tiles->count; i++) {
            pc_ui_canvas_cache_fini(&tiles->items[i].cache);
        }
        *tiles = (pc_ui_canvas_tiles){0};
    }
}

pc_status pc_ui_canvas_tiles_update(pc_ui_canvas_tiles *tiles, const pc_engine_doc_state *state,
                                    const pc_ui_canvas_xform *xf, pc_error *err)
{
    return pc_ui_canvas_tiles_update_gpu(tiles, state, xf, NULL, err);
}
pc_status pc_ui_canvas_tiles_update_gpu(pc_ui_canvas_tiles *tiles, const pc_engine_doc_state *state,
                                        const pc_ui_canvas_xform *xf, pc_gpu_compositor *gpu, pc_error *err)
{
    if (!tiles || !state || !state->doc || !xf) {
        return pc_fail(err, PC_ERR_INVALID, "invalid canvas tiles");
    }
    double lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
    for (unsigned corner = 0; corner < 4; corner++) {
        float p[2];
        if (pc_ui_canvas_to_doc(xf, xf->x + ((corner & 1) ? xf->width : 0),
                                xf->y + ((corner & 2) ? xf->height : 0), p) != PC_OK) {
            return pc_fail(err, PC_ERR_INVALID, "invalid canvas tile viewport");
        }
        for (size_t axis = 0; axis < 2; axis++) {
            lo[axis] = fmin(lo[axis], p[axis]);
            hi[axis] = fmax(hi[axis], p[axis]);
        }
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    pc_rect bounds = pc_document_bounds(doc);
    pc_rect visible =
        pc_rect_intersect(bounds, pc_rect_make(pc_f64_to_i32(floor(lo[0])), pc_f64_to_i32(floor(lo[1])),
                                               pc_f64_to_i32(ceil(hi[0])), pc_f64_to_i32(ceil(hi[1]))));
    if (xf->zoom < .5f || pc_rect_is_empty(visible) ||
        (pc_rect_width(visible) + 512 <= 4096 && pc_rect_height(visible) + 512 <= 4096)) {
        /* Retain the monotonic stamp so replacing a tile never reuses an old GPU generation. */
        uint64_t generation = tiles->generation;
        pc_ui_canvas_tiles_fini(tiles);
        tiles->generation = generation;
        return PC_OK;
    }
    int32_t x0 = (visible.x0 / 1024) * 1024, y0 = (visible.y0 / 1024) * 1024;
    uint64_t columns = ((uint64_t)(visible.x1 - x0) + 1023) / 1024;
    uint64_t rows = ((uint64_t)(visible.y1 - y0) + 1023) / 1024;
    if (columns * rows > PC_UI_CANVAS_TILE_CAP ||
        columns * rows * UINT64_C(1028) * 1028 > UINT64_C(33554432)) {
        return pc_fail(err, PC_ERR_LIMIT, "full-resolution canvas tile budget exceeded");
    }
    pc_ui_canvas_tiles *next = pc_calloc_array(1, sizeof *next);
    if (!next) {
        return pc_fail(err, PC_ERR_NOMEM, "canvas tiles");
    }
    bool reused[PC_UI_CANVAS_TILE_CAP] = {false};
    size_t origins[PC_UI_CANVAS_TILE_CAP];
    pc_status rc = PC_OK;
    next->generation = tiles->generation;
    for (uint64_t row = 0; rc == PC_OK && row < rows; row++) {
        for (uint64_t col = 0; rc == PC_OK && col < columns; col++) {
            int32_t x = (int32_t)((int64_t)x0 + (int64_t)col * 1024);
            int32_t y = (int32_t)((int64_t)y0 + (int64_t)row * 1024);
            pc_rect coverage = pc_rect_make(x, y, (int32_t)fmin((double)x + 1024, bounds.x1),
                                            (int32_t)fmin((double)y + 1024, bounds.y1));
            pc_rect source = pc_rect_make(x > 1 ? x - 2 : 0, y > 1 ? y - 2 : 0,
                                          (int32_t)fmin((double)coverage.x1 + 2, bounds.x1),
                                          (int32_t)fmin((double)coverage.y1 + 2, bounds.y1));
            size_t old = tiles->count;
            for (size_t i = 0; i < tiles->count; i++) {
                const pc_ui_canvas_cache *c = &tiles->items[i].cache;
                pc_rect damage;
                if (c->document == doc->id &&
                    (c->revision == state->revision ||
                     (pc_ui_canvas_cache_damage_rect(c, state, &damage) && pc_rect_is_empty(damage))) &&
                    c->render_key == pc_ui_canvas_render_key() && pc_rect_eq(c->source_rect, source)) {
                    old = i;
                    break;
                }
            }
            pc_ui_canvas_tile *tile = &next->items[next->count];
            origins[next->count] = old;
            tile->coverage = coverage;
            if (old < tiles->count) {
                tile->cache = tiles->items[old].cache;
                tile->cache.revision = state->revision;
                reused[old] = true;
            } else {
                pc_ui_canvas_cache *c = &tile->cache;
                rc = pc_ui_canvas_compose(doc, source, gpu, &c->image, err);
                if (rc != PC_OK) {
                    break;
                }
                for (size_t i = 0; i < c->image.len; i++) {
                    float *pixel = c->image.px[i];
                    pixel[3] = isfinite(pixel[3]) ? fminf(1, fmaxf(0, pixel[3])) : 0;
                    for (size_t ch = 0; ch < 3; ch++) {
                        pixel[ch] = isfinite(pixel[ch]) ? pixel[ch] * pixel[3] : 0;
                    }
                }
                c->source_rect = source;
                c->last_render = source;
                c->document = doc->id;
                c->revision = state->revision;
                c->render_key = pc_ui_canvas_render_key();
                c->generation = ++next->generation;
                c->valid = c->region = true;
            }
            next->count++;
        }
    }
    if (rc == PC_OK) {
        for (size_t i = 0; i < tiles->count; i++) {
            if (!reused[i]) {
                pc_ui_canvas_cache_fini(&tiles->items[i].cache);
            }
        }
        *tiles = *next;
    } else {
        for (size_t i = 0; i < next->count; i++) {
            if (origins[i] == tiles->count) {
                pc_ui_canvas_cache_fini(&next->items[i].cache);
            }
        }
    }
    free(next);
    return rc;
}
