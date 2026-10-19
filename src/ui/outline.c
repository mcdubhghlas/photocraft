#include "outline.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "base/mem.h"

void pc_ui_outline_fini(pc_ui_outline *o)
{
    if (o) {
        free(o->segments);
        *o = (pc_ui_outline){0};
    }
}
static int64_t floor_div(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return a % b < 0 ? q - 1 : q;
}
static int32_t coordinate(int64_t cell, int64_t step, int32_t lo, int32_t hi)
{
    int64_t v = cell * step;
    return v < lo ? lo : v > hi ? hi : (int32_t)v;
}
static pc_status append(pc_ui_outline *o, pc_ui_segment s, pc_error *err)
{
    if (o->count >= (1u << 20)) {
        return pc_fail(err, PC_ERR_LIMIT, "selection outline has too many segments");
    }
    if (o->count == o->capacity) {
        void *next = pc_grow_array(o->segments, &o->capacity, o->count + 1, sizeof s);
        if (!next) {
            return pc_fail(err, PC_ERR_NOMEM, "selection outline");
        }
        o->segments = next;
    }
    o->segments[o->count++] = s;
    return PC_OK;
}
pc_status pc_ui_outline_trace(const pc_surface *mask, pc_rect bounds, uint32_t step, pc_ui_outline *out,
                              pc_error *err)
{
    if (!mask || !out) {
        return pc_fail(err, PC_ERR_INVALID, "missing outline mask/output");
    }
    pc_ui_outline next = {0};
    if (pc_rect_is_empty(bounds)) {
        pc_ui_outline_fini(out);
        *out = next;
        return PC_OK;
    }
    int64_t stride = step ? step : 1;
    int64_t gx = floor_div(bounds.x0, stride) - 1, gy = floor_div(bounds.y0, stride) - 1;
    int64_t right = -floor_div(-(int64_t)bounds.x1, stride) + 1;
    int64_t bottom = -floor_div(-(int64_t)bounds.y1, stride) + 1;
    uint64_t w64 = (uint64_t)(right - gx), h64 = (uint64_t)(bottom - gy);
    if (w64 > (1u << 24) || h64 > (1u << 24) || w64 * h64 > (1u << 24)) {
        return pc_fail(err, PC_ERR_LIMIT, "selection outline sampling exceeds 16M cells");
    }
    size_t w = (size_t)w64, h = (size_t)h64;
    unsigned char *grid = pc_calloc_array(w * h, 1);
    float *row = pc_malloc_array(w, sizeof *row);
    if (!grid || !row) {
        free(grid);
        free(row);
        return pc_fail(err, PC_ERR_NOMEM, "selection outline grid");
    }
    for (size_t y = 0; y < h; y++) {
        int64_t sy = (gy + (int64_t)y) * stride + stride / 2;
        if (sy < bounds.y0 || sy >= bounds.y1) {
            continue;
        }
        int64_t sx = gx * stride + stride / 2;
        if (sx >= INT32_MIN && sx <= INT32_MAX && stride <= INT32_MAX) {
            pc_surface_sample_row_strided(mask, (int32_t)sy, (int32_t)sx, (int32_t)stride, 0, row, w);
            for (size_t x = 0; x < w; x++) {
                int64_t px = sx + (int64_t)x * stride;
                grid[y * w + x] = (unsigned char)(px >= bounds.x0 && px < bounds.x1 && row[x] > .5f);
            }
        } else {
            /* Padded coordinates can lie beyond i32 even when the selection is valid. */
            for (size_t x = 0; x < w; x++) {
                int64_t px = sx + (int64_t)x * stride;
                if (px >= bounds.x0 && px < bounds.x1) {
                    grid[y * w + x] =
                        (unsigned char)(pc_surface_sample_channel(mask, (int32_t)px, (int32_t)sy, 0) > .5f);
                }
            }
        }
    }
    free(row);
    pc_status rc = PC_OK;
    for (size_t y = 1; y < h && rc == PC_OK; y++) {
        size_t run = SIZE_MAX;
        for (size_t x = 0; x <= w; x++) {
            bool edge = x < w && grid[y * w + x] != grid[(y - 1) * w + x];
            if (edge && run == SIZE_MAX) {
                run = x;
            } else if (!edge && run != SIZE_MAX) {
                int32_t yy = coordinate(gy + (int64_t)y, stride, bounds.y0, bounds.y1);
                pc_ui_segment s = {{coordinate(gx + (int64_t)run, stride, bounds.x0, bounds.x1), yy},
                                   {coordinate(gx + (int64_t)x, stride, bounds.x0, bounds.x1), yy}};
                rc = append(&next, s, err);
                run = SIZE_MAX;
                if (rc != PC_OK) {
                    break;
                }
            }
        }
    }
    for (size_t x = 1; x < w && rc == PC_OK; x++) {
        size_t run = SIZE_MAX;
        for (size_t y = 0; y <= h; y++) {
            bool edge = y < h && grid[y * w + x] != grid[y * w + x - 1];
            if (edge && run == SIZE_MAX) {
                run = y;
            } else if (!edge && run != SIZE_MAX) {
                int32_t xx = coordinate(gx + (int64_t)x, stride, bounds.x0, bounds.x1);
                pc_ui_segment s = {{xx, coordinate(gy + (int64_t)run, stride, bounds.y0, bounds.y1)},
                                   {xx, coordinate(gy + (int64_t)y, stride, bounds.y0, bounds.y1)}};
                rc = append(&next, s, err);
                run = SIZE_MAX;
                if (rc != PC_OK) {
                    break;
                }
            }
        }
    }
    free(grid);
    if (rc == PC_OK) {
        pc_ui_outline_fini(out);
        *out = next;
    } else {
        pc_ui_outline_fini(&next);
    }
    return rc;
}
