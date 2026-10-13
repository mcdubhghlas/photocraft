/* The tiled filter driver: apply / apply_in / apply_tiled(_with) and selection mixing (port of
 * crates/algo/src/lib.rs). Tiles of a group run in parallel on the global pool; results are
 * written in tile order after the group, so output never depends on scheduling. */
#include "algo/filter.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"

void pc_algo_mix_selection(float *data, pc_rect t, const pc_surface *sel, const pc_algo_image *orig,
                           bool alpha)
{
    size_t n = orig->ch > 0 ? orig->ch : 1;
    size_t w = pc_rect_width(t), h = pc_rect_height(t), i, c;
    size_t npx = w * h;
    for (i = 0; i < npx; i++) {
        int32_t x = (int32_t)((int64_t)t.x0 + (int64_t)(i % w));
        int32_t y = (int32_t)((int64_t)t.y0 + (int64_t)(i / w));
        float k = pc_clampf(pc_surface_sample_channel(sel, x, y, 0), 0.0f, 1.0f);
        float *px = data + i * n;
        if (k >= 1.0f) {
            continue;
        }
        if (k <= 0.0f) {
            for (c = 0; c < n; c++) {
                px[c] = pc_algo_image_get(orig, x, y, c);
            }
            continue;
        }
        if (alpha) {
            size_t a = n - 1;
            float original_alpha = pc_algo_image_get(orig, x, y, a), filtered_alpha = px[a];
            if (original_alpha != filtered_alpha) {
                float mixed_alpha = original_alpha + (filtered_alpha - original_alpha) * k;
                float original_weight = original_alpha * (1.0f - k), filtered_weight = filtered_alpha * k;
                for (c = 0; c < a; c++) {
                    px[c] =
                        mixed_alpha > 0.0f
                            ? (pc_algo_image_get(orig, x, y, c) * original_weight + px[c] * filtered_weight) /
                                  mixed_alpha
                            : 0.0f;
                }
                px[a] = mixed_alpha;
                continue;
            }
        }
        for (c = 0; c < n; c++) {
            float o = pc_algo_image_get(orig, x, y, c);
            px[c] = o + (px[c] - o) * k;
        }
    }
}

/* One group of tiles run in parallel. */
typedef struct run_ctx {
    const pc_surface *surface;
    const pc_algo_filter *f;
    const pc_surface *selection;
    const pc_rect *extent;
    const pc_interrupt *ctl;
    const pc_algo_image *shared; /* global filters: the bounds read once */
    const void *prep;
    pc_algo_ctx ctx;
    pc_algo_halo halo;
    pc_rect area;
    pc_rect bounds;
    int32_t tile;
    uint64_t nx;
    uint64_t first;  /* index of the group's first tile */
    float **results; /* per tile in the group; NULL when skipped (cancelled) */
    atomic_size_t *finished;
    size_t total;
    /* The failure of the lowest-index tile, so the message doesn't depend on scheduling. */
    pthread_mutex_t lock;
    size_t err_index;
    pc_error err;
} run_ctx;

static void record_error(run_ctx *r, size_t k, pc_status st, pc_error *e)
{
    e->code = st;
    if (pthread_mutex_lock(&r->lock) != 0) {
        return;
    }
    if (k < r->err_index) {
        r->err_index = k;
        r->err = *e;
    }
    (void)pthread_mutex_unlock(&r->lock);
}

static pc_rect tile_rect(const run_ctx *r, uint64_t idx)
{
    uint64_t ty = idx / r->nx, tx = idx % r->nx;
    int64_t x0 = (int64_t)r->area.x0 + (int64_t)tx * r->tile;
    int64_t y0 = (int64_t)r->area.y0 + (int64_t)ty * r->tile;
    int64_t x1 = x0 + r->tile, y1 = y0 + r->tile;
    if (x1 > r->area.x1) {
        x1 = r->area.x1;
    }
    if (y1 > r->area.y1) {
        y1 = r->area.y1;
    }
    return pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1);
}

static pc_status run_tile(void *vctx, size_t chunk, size_t begin, size_t end)
{
    run_ctx *r = vctx;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        pc_rect t = tile_rect(r, r->first + k);
        pc_algo_image owned = {PC_RECT_EMPTY, 0, NULL, 0};
        const pc_algo_image *src;
        float *data;
        size_t n, done;
        pc_status st;
        pc_error err;
        err.code = PC_OK;
        err.sys_errno = 0;
        err.msg[0] = '\0';
        /* Cancelled: skip the remaining tiles of the group (the result is discarded). */
        if (pc_interrupt_cancelled(r->ctl)) {
            continue;
        }
        if (r->shared != NULL) {
            src = r->shared;
        } else {
            pc_rect grown = pc_rect_inflate(t, r->halo.radius);
            st = r->extent != NULL ? pc_algo_image_read_clamped(r->surface, grown, *r->extent, &owned, &err)
                                   : pc_algo_image_read(r->surface, grown, &owned, &err);
            if (st != PC_OK) {
                record_error(r, k, st, &err);
                return st;
            }
            src = &owned;
        }
        if (!pc_algo_float_count(t, src->ch, &n)) {
            pc_algo_image_fini(&owned);
            st = pc_fail(&err, PC_ERR_LIMIT, "%s: tile too large", r->f->def->tag);
            record_error(r, k, st, &err);
            return st;
        }
        data = pc_calloc_array(n > 0 ? n : 1, sizeof(float));
        if (data == NULL) {
            pc_algo_image_fini(&owned);
            st = pc_fail(&err, PC_ERR_NOMEM, "out of memory running %s", r->f->def->tag);
            record_error(r, k, st, &err);
            return st;
        }
        st = r->f->def->kernel(r->f->params, r->prep, src, t, &r->ctx, data, &err);
        if (st != PC_OK) {
            record_error(r, k, st, &err);
            free(data);
            pc_algo_image_fini(&owned);
            return st;
        }
        if (r->selection != NULL) {
            pc_algo_mix_selection(data, t, r->selection, src, r->ctx.alpha);
        }
        pc_algo_image_fini(&owned);
        r->results[k] = data;
        done = atomic_fetch_add_explicit(r->finished, 1, memory_order_relaxed) + 1;
        /* The last few percent are the writes after each group. */
        pc_interrupt_progress(r->ctl, 0.98f * (float)done / (float)r->total);
    }
    return PC_OK;
}

/* Most tiles held at once (the reference's group has no cap; results don't depend on it). */
#define MAX_GROUP 65536

pc_status pc_algo_apply_tiled_with(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                                   const pc_surface *selection, int32_t tile, const pc_rect *extent,
                                   const pc_interrupt *ctl, pc_surface *out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(s);
    pc_surface res;
    pc_algo_image shared = {PC_RECT_EMPTY, 0, NULL, 0};
    void *prep = NULL;
    run_ctx r;
    atomic_size_t finished;
    uint64_t ntiles, g0, ny;
    size_t per_tile, group, threads, i;
    float **results = NULL;
    pc_status st;
    if (f == NULL || f->def == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no filter");
    }
    if (pc_interrupt_cancelled(ctl)) {
        return pc_fail(err, PC_ERR_CANCELLED, "cancelled");
    }
    if (pc_rect_is_empty(area)) {
        return pc_surface_clone(s, out, err);
    }
    if (f->def->apply != NULL) {
        pc_algo_apply_args a = {s, f, area, bounds, selection, tile, extent, ctl};
        bool handled = false;
        st = f->def->apply(&a, out, &handled, err);
        if (handled) {
            return st;
        }
    }
    if (f->def->kernel == NULL) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "filter `%s` has no kernel", f->def->tag);
    }
    /* A zero or negative tile would never advance (the reference loops forever). */
    if ((f->def->flags & PC_ALGO_FILTER_CLAMP_TILE) != 0) {
        tile = pc_clamp_i32(tile, 1, 2048);
    } else if (tile < 1) {
        tile = 1;
    }
    memset(&r, 0, sizeof r);
    r.surface = s;
    r.f = f;
    r.selection = selection;
    r.extent = extent;
    r.ctl = ctl;
    r.ctx.bounds = bounds;
    r.ctx.mode = fmt.mode;
    r.ctx.alpha = fmt.alpha;
    r.halo = pc_algo_filter_halo_for(f, bounds);
    r.area = area;
    r.bounds = bounds;
    r.tile = tile;
    r.nx = ((uint64_t)pc_rect_width(area) + (uint64_t)tile - 1) / (uint64_t)tile;
    ny = ((uint64_t)pc_rect_height(area) + (uint64_t)tile - 1) / (uint64_t)tile;
    ntiles = r.nx * ny;
    if (r.halo.bounds) {
        st = pc_algo_image_read(s, pc_rect_union(bounds, area), &shared, err);
        if (st != PC_OK) {
            return st;
        }
        r.shared = &shared;
    }
    /* Shape rasterisation depends only on the parameters, not on a tile's pixels. */
    if (f->def->prepare != NULL) {
        if (pc_interrupt_cancelled(ctl)) {
            pc_algo_image_fini(&shared);
            return pc_fail(err, PC_ERR_CANCELLED, "cancelled");
        }
        st = f->def->prepare(f->params, &prep, err);
        if (st != PC_OK) {
            pc_algo_image_fini(&shared);
            return st;
        }
        r.prep = prep;
    }
    st = pc_surface_clone(s, &res, err);
    if (st != PC_OK) {
        goto done_noresult;
    }
    atomic_init(&finished, 0);
    r.finished = &finished;
    r.err_index = SIZE_MAX;
    if (pthread_mutex_init(&r.lock, NULL) != 0) {
        pc_surface_fini(&res);
        st = pc_fail(err, PC_ERR_NOMEM, "cannot create a lock");
        goto done_noresult;
    }
    r.total = ntiles > SIZE_MAX ? SIZE_MAX : (size_t)ntiles;
    if (r.total == 0) {
        r.total = 1;
    }
    /* Groups of about RESULT_BUDGET bytes of results (at least one tile per thread), each written
     * before the next starts, so a huge layer never holds all its results as floats at once. */
    threads = pc_pool_threads(pc_pool_global());
    if (!pc_size_mul3((size_t)tile, (size_t)tile, pc_pixel_format_channels(fmt) * sizeof(float), &per_tile)) {
        per_tile = SIZE_MAX;
    }
    group = PC_ALGO_RESULT_BUDGET / (per_tile > 0 ? per_tile : 1);
    if (group < threads) {
        group = threads;
    }
    if (group < 1) {
        group = 1;
    }
    if (group > MAX_GROUP) {
        group = MAX_GROUP;
    }
    if ((uint64_t)group > ntiles) {
        group = (size_t)ntiles;
    }
    results = pc_calloc_array(group > 0 ? group : 1, sizeof *results);
    if (results == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory running %s", f->def->tag);
        goto done;
    }
    r.results = results;
    for (g0 = 0; g0 < ntiles; g0 += group) {
        size_t gn = (size_t)(ntiles - g0 < (uint64_t)group ? ntiles - g0 : (uint64_t)group);
        if (pc_interrupt_cancelled(ctl)) {
            st = pc_fail(err, PC_ERR_CANCELLED, "cancelled");
            goto done;
        }
        memset(results, 0, gn * sizeof *results);
        r.first = g0;
        st = pc_pool_for(pc_pool_global(), gn, 1, run_tile, &r);
        if (st == PC_OK && pc_interrupt_cancelled(ctl)) {
            st = pc_fail(err, PC_ERR_CANCELLED, "cancelled");
        } else if (st != PC_OK) {
            if (err != NULL && r.err_index != SIZE_MAX) {
                *err = r.err;
            }
            st = r.err_index != SIZE_MAX ? r.err.code : st;
        }
        for (i = 0; i < gn && st == PC_OK; i++) {
            pc_rect t = tile_rect(&r, g0 + i);
            size_t n;
            if (results[i] == NULL || !pc_algo_float_count(t, pc_surface_channels(s), &n)) {
                st = pc_fail(err, PC_ERR_INVALID, "%s produced no output for a tile", f->def->tag);
                break;
            }
            st = pc_surface_write_region(&res, t, results[i], n, err);
        }
        for (i = 0; i < gn; i++) {
            free(results[i]);
            results[i] = NULL;
        }
        if (st != PC_OK) {
            goto done;
        }
    }
    pc_surface_prune(&res);
    pc_interrupt_progress(ctl, 1.0f);
done:
    (void)pthread_mutex_destroy(&r.lock);
    free(results);
    if (st == PC_OK) {
        *out = res;
    } else {
        pc_surface_fini(&res);
    }
done_noresult:
    if (prep != NULL && f->def->prep_free != NULL) {
        f->def->prep_free(prep);
    }
    pc_algo_image_fini(&shared);
    return st;
}

pc_status pc_algo_apply_tiled(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                              const pc_surface *selection, int32_t tile, const pc_rect *extent,
                              pc_surface *out, pc_error *err)
{
    pc_status st = pc_algo_apply_tiled_with(s, f, area, bounds, selection, tile, extent, NULL, out, err);
    /* Preserve the input when a filter that asks for it (Shape Blur) cannot complete. */
    if (st != PC_OK && st != PC_ERR_NOMEM && f != NULL && f->def != NULL &&
        (f->def->flags & PC_ALGO_FILTER_KEEP_ON_FAIL) != 0) {
        return pc_surface_clone(s, out, err);
    }
    return st;
}

pc_status pc_algo_apply(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                        const pc_surface *selection, pc_surface *out, pc_error *err)
{
    if (f == NULL || f->def == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no filter");
    }
    return pc_algo_apply_tiled(s, f, area, bounds, selection, pc_algo_auto_tile(f, bounds), NULL, out, err);
}

pc_status pc_algo_apply_in(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                           const pc_surface *selection, pc_rect extent, pc_surface *out, pc_error *err)
{
    if (f == NULL || f->def == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no filter");
    }
    return pc_algo_apply_tiled(s, f, pc_rect_intersect(area, extent), bounds, selection,
                               pc_algo_auto_tile(f, bounds), &extent, out, err);
}

pc_status pc_algo_apply_in_with(const pc_surface *s, const pc_algo_filter *f, pc_rect area, pc_rect bounds,
                                const pc_surface *selection, pc_rect extent, const pc_interrupt *ctl,
                                pc_surface *out, pc_error *err)
{
    if (f == NULL || f->def == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no filter");
    }
    return pc_algo_apply_tiled_with(s, f, pc_rect_intersect(area, extent), bounds, selection,
                                    pc_algo_auto_tile(f, bounds), &extent, ctl, out, err);
}
