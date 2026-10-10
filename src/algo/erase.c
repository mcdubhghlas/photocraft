/* Magic Eraser (port of crates/algo/src/erase.rs); see erase.h. */
#include "algo/erase.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "color/color.h"

typedef struct tile_out {
    pc_rect r;
    uint8_t *bytes; /* NULL: the region doesn't touch this tile */
    size_t len;
    pc_error err;
} tile_out;

typedef struct erase_job {
    const pc_surface *src;
    const pc_algo_region *region;
    size_t bw;
    float amount;
    const pc_surface *selection;
    const float *lock; /* nc values, or NULL */
    size_t nc;
    bool has_alpha;
    pc_sample_type sample;
    size_t bpp;
    tile_out *tiles;
} erase_job;

static uint8_t coverage(const erase_job *j, int32_t x, int32_t y)
{
    const pc_algo_region *rg = j->region;
    size_t i = (size_t)((int64_t)y - rg->bbox.y0) * j->bw + (size_t)((int64_t)x - rg->bbox.x0);
    return i < rg->len ? rg->mask[i] : 0;
}

static pc_status erase_tile(const erase_job *j, tile_out *t)
{
    pc_rect r = t->r;
    size_t w = pc_rect_width(r), sc = 0, sel_len = 0, len = 0;
    int32_t x, y;
    bool any = false;
    uint8_t *bytes = NULL;
    float *sel = NULL;
    pc_status st;
    /* Skip tiles the region doesn't touch. */
    for (y = r.y0; y < r.y1 && !any; y++) {
        for (x = r.x0; x < r.x1 && !any; x++) {
            any = coverage(j, x, y) != 0;
        }
    }
    if (!any) {
        return PC_OK;
    }
    if ((st = pc_surface_to_interleaved(j->src, r, &bytes, &len, &t->err)) != PC_OK) {
        return st;
    }
    if (j->selection != NULL) {
        sc = pc_surface_channels(j->selection);
        if ((st = pc_surface_read_region(j->selection, r, &sel, &sel_len, &t->err)) != PC_OK) {
            free(bytes);
            return st;
        }
    }
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1; x++) {
            uint8_t c = coverage(j, x, y);
            size_t i = (size_t)(y - r.y0) * w + (size_t)(x - r.x0), ch;
            float s = 1.0f, k;
            uint8_t *px;
            if (c == 0) {
                continue;
            }
            if (j->selection != NULL) {
                s = i * sc < sel_len ? sel[i * sc] : 0.0f;
            }
            k = (float)c / 255.0f * j->amount * s;
            if (k <= 0.0f || (i + 1) * j->bpp > len) {
                continue;
            }
            px = bytes + i * j->bpp;
            if (j->lock != NULL) {
                for (ch = 0; ch < j->nc; ch++) {
                    float v = pc_read_sample(px, j->bpp, j->sample, ch);
                    pc_write_sample(px, j->bpp, j->sample, ch, v + (j->lock[ch] - v) * k);
                }
            } else if (j->has_alpha) {
                float v = pc_read_sample(px, j->bpp, j->sample, j->nc) * (1.0f - k);
                if (k >= 1.0f || v <= 0.0f) {
                    /* Fully erased pixels become the empty pixel, so tiles can be pruned. */
                    memset(px, 0, j->bpp);
                } else {
                    pc_write_sample(px, j->bpp, j->sample, j->nc, v);
                }
            }
        }
    }
    free(sel);
    t->bytes = bytes;
    t->len = len;
    return PC_OK;
}

static pc_status erase_run(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const erase_job *j = ctx;
    size_t i;
    pc_status first = PC_OK;
    (void)chunk;
    for (i = begin; i < end; i++) {
        pc_status st = erase_tile(j, &j->tiles[i]);
        first = first == PC_OK ? st : first;
    }
    return first;
}

pc_status pc_algo_erase_magic(pc_surface *target, const pc_algo_region *region, float amount,
                              const pc_surface *selection, const float *lock_color, size_t lock_len,
                              pc_rect *damaged, pc_error *err)
{
    pc_pixel_format fmt;
    pc_rect bbox, dmg = PC_RECT_EMPTY;
    pc_tile first, last;
    size_t bw, area, ntiles = 0, i;
    float lock[PC_RASTER_MAX_CHANNELS];
    erase_job j;
    pc_status st = PC_OK;
    int64_t tx, ty;
    if (target == NULL || region == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "missing surface or region");
    }
    amount = isfinite(amount) ? pc_clampf(amount, 0.0f, 1.0f) : 0.0f;
    bbox = region->bbox;
    bw = pc_rect_width(bbox);
    if (pc_rect_is_empty(bbox) || amount <= 0.0f || !pc_size_mul(bw, pc_rect_height(bbox), &area) ||
        region->len != area || region->mask == NULL) {
        *damaged = PC_RECT_EMPTY;
        return PC_OK;
    }
    fmt = pc_surface_format(target);
    memset(&j, 0, sizeof j);
    j.nc = pc_color_mode_channels(fmt.mode);
    j.has_alpha = fmt.alpha;
    /* Without alpha and without a lock colour there is nothing an eraser can do. */
    if (!j.has_alpha && lock_color == NULL) {
        *damaged = PC_RECT_EMPTY;
        return PC_OK;
    }
    if (lock_color != NULL) {
        for (i = 0; i < j.nc && i < PC_RASTER_MAX_CHANNELS; i++) {
            lock[i] = i < lock_len ? lock_color[i] : 0.0f;
        }
        j.lock = lock;
    }
    j.src = target, j.region = region, j.bw = bw, j.amount = amount, j.selection = selection;
    j.sample = fmt.sample, j.bpp = pc_pixel_format_bytes_per_pixel(fmt);
    if (!pc_rect_tile_range(bbox, &first, &last)) {
        *damaged = PC_RECT_EMPTY;
        return PC_OK;
    }
    j.tiles = pc_calloc_array((size_t)((int64_t)last.ty - first.ty + 1),
                              (size_t)((int64_t)last.tx - first.tx + 1) * sizeof *j.tiles);
    if (j.tiles == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (ty = first.ty; ty <= last.ty; ty++) {
        for (tx = first.tx; tx <= last.tx; tx++) {
            pc_rect r = pc_rect_intersect(pc_tile_rect((pc_tile){(int32_t)tx, (int32_t)ty}), bbox);
            if (!pc_rect_is_empty(r)) {
                j.tiles[ntiles++].r = r;
            }
        }
    }
    /* Each tile is edited as encoded bytes, in parallel; the results are copied back serially. */
    st = pc_algo_par_for(ntiles, 1, erase_run, &j);
    if (st != PC_OK) {
        for (i = 0; i < ntiles; i++) {
            if (j.tiles[i].err.code != PC_OK) {
                if (err != NULL) {
                    *err = j.tiles[i].err;
                }
                break;
            }
        }
        goto cleanup;
    }
    for (i = 0; i < ntiles; i++) {
        if (j.tiles[i].bytes != NULL) {
            if ((st = pc_surface_write_interleaved(target, j.tiles[i].r, j.tiles[i].bytes, j.tiles[i].len,
                                                   err)) != PC_OK) {
                goto cleanup;
            }
            dmg = pc_rect_union(dmg, j.tiles[i].r);
        }
    }
    if (j.lock == NULL) {
        pc_surface_prune(target);
    }
    *damaged = dmg;
cleanup:
    for (i = 0; i < ntiles; i++) {
        free(j.tiles[i].bytes);
    }
    free(j.tiles);
    return st;
}
