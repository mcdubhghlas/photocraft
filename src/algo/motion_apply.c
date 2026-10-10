/* Bounded tile scheduling of the FFT Motion Blur (port of crates/algo/src/blur/motion_apply.rs):
 * a whole-area override of the tiled driver that convolves long streaks by FFT when that is
 * clearly faster than the row kernel, and declines (the driver then runs the row kernel)
 * otherwise. Like the reference, the decision depends on the worker count (pc_pool threads), so
 * a long streak on a one-thread pool takes the row kernel; the two agree to FFT round-off. */
#include "algo/blur_int.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"

#define MAX_RESIDENT_BYTES ((size_t)512 << 20)
#define MIN_TILE 64
#define MAX_TILE 512

static bool valid_rect(pc_rect r)
{
    return !pc_rect_is_empty(r) && pc_rect_width(r) <= (uint32_t)PC_ALGO_MOTION_COORDINATE_LIMIT &&
           pc_rect_height(r) <= (uint32_t)PC_ALGO_MOTION_COORDINATE_LIMIT &&
           pc_algo_motion_ordinary_coordinates(r);
}

static bool rect_bytes(pc_rect r, size_t channels, size_t *out)
{
    size_t t;
    return pc_size_mul3(pc_rect_width(r), pc_rect_height(r), channels, &t) &&
           pc_size_mul(t, sizeof(float), out);
}

bool pc_algo_motion_group_size(size_t resident, size_t metadata, size_t worker, size_t output, size_t tiles,
                               size_t workers, size_t *out)
{
    size_t available, per_tile, memory_workers;
    if (tiles == 0 || workers == 0 || resident > MAX_RESIDENT_BYTES ||
        MAX_RESIDENT_BYTES - resident < metadata) {
        return false;
    }
    available = MAX_RESIDENT_BYTES - resident - metadata;
    if (!pc_size_add(worker, output, &per_tile)) {
        return false;
    }
    memory_workers = available / (per_tile > 0 ? per_tile : 1);
    if (memory_workers == 0) {
        return false;
    }
    memory_workers = memory_workers < workers ? memory_workers : workers;
    *out = memory_workers < tiles ? memory_workers : tiles;
    return true;
}

typedef struct mtile {
    pc_rect output, source;
} mtile;

typedef struct mrun {
    const pc_algo_apply_args *a;
    const pc_algo_motion_plan *plan;
    const mtile *tiles;
    size_t first, count, channels;
    bool alpha;
    float **results;
    atomic_size_t finished;
} mrun;

static pc_status run_tile(void *vctx, size_t chunk, size_t begin, size_t end)
{
    mrun *r = vctx;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        const mtile *t = &r->tiles[r->first + k];
        pc_algo_image src;
        float *data = NULL;
        size_t len = 0, want, done;
        pc_status st;
        if (pc_interrupt_cancelled(r->a->ctl)) {
            return PC_ERR_CANCELLED;
        }
        st = r->a->extent != NULL
                 ? pc_algo_image_read_clamped(r->a->surface, t->source, *r->a->extent, &src, NULL)
                 : pc_algo_image_read(r->a->surface, t->source, &src, NULL);
        if (st != PC_OK) {
            return PC_ERR_UNSUPPORTED; /* decline: the row pipeline retries */
        }
        /* Unsupported source values decline the whole specialisation. */
        if (pc_interrupt_cancelled(r->a->ctl) ||
            !pc_algo_motion_plan_filter(r->plan, &src, t->output, r->alpha, r->a->ctl, &data, &len) ||
            !pc_algo_float_count(t->output, r->channels, &want) || len != want) {
            free(data);
            pc_algo_image_fini(&src);
            return PC_ERR_UNSUPPORTED;
        }
        if (r->a->selection != NULL) {
            pc_algo_mix_selection(data, t->output, r->a->selection, &src);
        }
        pc_algo_image_fini(&src);
        if (pc_interrupt_cancelled(r->a->ctl)) {
            free(data);
            return PC_ERR_CANCELLED;
        }
        r->results[k] = data;
        done = atomic_fetch_add_explicit(&r->finished, 1, memory_order_relaxed) + 1;
        pc_interrupt_progress(r->a->ctl, 0.98f * (float)done / (float)r->count);
    }
    return PC_OK;
}

/* Result of the scheduler: a surface, a decline, or cancellation. */
enum { M_DONE, M_DECLINE, M_CANCEL };

static int schedule(const pc_algo_apply_args *a, float angle, float distance, bool force, pc_surface *out,
                    pc_status *st, pc_error *err)
{
    const pc_surface *surface = a->surface;
    pc_pixel_format fmt = pc_surface_format(surface);
    size_t channels = pc_pixel_format_channels(fmt), row_tile, workers, tile_u, columns, rows, count,
           metadata;
    size_t largest = 0, decode, worker_bytes, output_bytes, group, resident, i, g0;
    pc_algo_motion_plan *plan;
    mtile *tiles = NULL;
    float **results = NULL;
    pc_surface res;
    int32_t tile = a->tile, y;
    int outcome = M_DECLINE;
    bool ok;
    mrun r;
    if (pc_interrupt_cancelled(a->ctl)) {
        return M_CANCEL;
    }
    if (channels < 1 || channels > 5 || !valid_rect(a->area) || !valid_rect(a->bounds) ||
        (a->extent != NULL && !valid_rect(*a->extent)) ||
        /* read_clamped falls back to raw reads when its window misses the extent: keep the old
         * boundary policy for out-of-canvas output areas. */
        (a->extent != NULL && !pc_rect_eq(pc_rect_intersect(a->area, *a->extent), a->area)) ||
        (a->selection != NULL && pc_surface_channels(a->selection) == 0)) {
        return M_DECLINE;
    }
    if ((uint64_t)pc_rect_width(a->area) * pc_rect_height(a->area) < 4096 || tile < 0) {
        return M_DECLINE;
    }
    row_tile = (size_t)tile;
    workers = pc_pool_threads(pc_pool_global());
    tile = pc_clamp_i32(tile, MIN_TILE, MAX_TILE);
    while (tile > MIN_TILE &&
           (pc_rect_width(a->area) < (uint32_t)tile || pc_rect_height(a->area) < (uint32_t)tile)) {
        tile = tile / 2 > MIN_TILE ? tile / 2 : MIN_TILE;
    }
    tile_u = (size_t)tile;
    if (!force &&
        !pc_algo_motion_worthwhile(angle, distance, tile_u, a->area, row_tile, channels, workers, &ok)) {
        return M_DECLINE;
    }
    plan = pc_algo_motion_plan_new(angle, distance, tile_u, tile_u, NULL, a->ctl);
    if (plan == NULL) {
        return pc_interrupt_cancelled(a->ctl) ? M_CANCEL : M_DECLINE;
    }
    if (pc_interrupt_cancelled(a->ctl)) {
        pc_algo_motion_plan_free(plan);
        return M_CANCEL;
    }
    columns = (pc_rect_width(a->area) + tile_u - 1) / tile_u;
    rows = (pc_rect_height(a->area) + tile_u - 1) / tile_u;
    count = columns * rows;
    metadata = count * sizeof(mtile);
    tiles = pc_malloc_array(count + 1, sizeof *tiles);
    if (tiles == NULL) {
        goto done;
    }
    for (i = 0, y = a->area.y0; y < a->area.y1;) {
        int32_t y1 = (int32_t)((int64_t)y + tile < a->area.y1 ? (int64_t)y + tile : a->area.y1), x;
        for (x = a->area.x0; x < a->area.x1;) {
            int32_t x1 = (int32_t)((int64_t)x + tile < a->area.x1 ? (int64_t)x + tile : a->area.x1);
            size_t b;
            tiles[i].output = pc_rect_make(x, y, x1, y1);
            if (!pc_algo_motion_plan_read_rect(plan, tiles[i].output, &tiles[i].source) ||
                !valid_rect(tiles[i].source) || !rect_bytes(tiles[i].source, channels, &b)) {
                goto done;
            }
            largest = b > largest ? b : largest;
            i++;
            x = x1;
        }
        y = y1;
    }
    /* read_clamped holds both its inner read and the expanded image while decoding. */
    if (!pc_size_mul(largest, a->extent != NULL ? 2 : 1, &decode) ||
        !pc_algo_motion_plan_worker_bytes(plan, channels, tile_u, &worker_bytes) ||
        !pc_size_mul3(tile_u, tile_u, channels, &output_bytes) ||
        !pc_size_mul(output_bytes, 4, &output_bytes) ||
        !pc_algo_motion_plan_resident_bytes(plan, &resident)) {
        goto done;
    }
    worker_bytes = worker_bytes > decode ? worker_bytes : decode;
    /* A group's results stay live until its last worker finishes. */
    if (!pc_algo_motion_group_size(resident, metadata, worker_bytes, output_bytes, count, workers, &group)) {
        goto done;
    }
    results = pc_calloc_array(group, sizeof *results);
    if (results == NULL) {
        goto done;
    }
    *st = pc_surface_clone(surface, &res, err);
    if (*st != PC_OK) {
        outcome = M_DONE; /* report the clone's failure */
        goto done;
    }
    memset(&r, 0, sizeof r);
    r.a = a;
    r.plan = plan;
    r.tiles = tiles;
    r.count = count;
    r.channels = channels;
    r.alpha = fmt.alpha;
    r.results = results;
    atomic_init(&r.finished, 0);
    for (g0 = 0; g0 < count; g0 += group) {
        size_t gn = count - g0 < group ? count - g0 : group;
        pc_status ps;
        if (pc_interrupt_cancelled(a->ctl)) {
            outcome = M_CANCEL;
            break;
        }
        memset(results, 0, gn * sizeof *results);
        r.first = g0;
        ps = pc_algo_par_for(gn, 1, run_tile, &r);
        if (ps != PC_OK) {
            outcome = pc_interrupt_cancelled(a->ctl) ? M_CANCEL : M_DECLINE;
        }
        for (i = 0; i < gn && ps == PC_OK; i++) {
            size_t len;
            if (pc_interrupt_cancelled(a->ctl)) {
                outcome = M_CANCEL;
                ps = PC_ERR_CANCELLED;
                break;
            }
            (void)pc_algo_float_count(tiles[g0 + i].output, channels, &len);
            *st = pc_surface_write_region(&res, tiles[g0 + i].output, results[i], len, err);
            if (*st != PC_OK) {
                outcome = M_DONE;
                ps = *st;
            }
        }
        for (i = 0; i < gn; i++) {
            free(results[i]);
            results[i] = NULL;
        }
        if (ps != PC_OK) {
            break;
        }
        if (g0 + gn >= count) {
            outcome = M_DONE;
            *st = PC_OK;
        }
    }
    if (outcome == M_DONE && *st == PC_OK && pc_interrupt_cancelled(a->ctl)) {
        outcome = M_CANCEL;
    }
    if (outcome == M_DONE && *st == PC_OK) {
        pc_surface_prune(&res);
        if (pc_interrupt_cancelled(a->ctl)) {
            outcome = M_CANCEL;
        } else {
            pc_interrupt_progress(a->ctl, 1.0f);
            *out = res;
        }
    }
    if (!(outcome == M_DONE && *st == PC_OK)) {
        pc_surface_fini(&res);
    }
done:
    free(results);
    free(tiles);
    pc_algo_motion_plan_free(plan);
    return outcome;
}

pc_status pc_algo_motion_apply_ex(const pc_algo_apply_args *a, bool force, pc_surface *out, bool *handled,
                                  pc_error *err)
{
    const pc_algo_motion_params *p = a->filter->params;
    pc_status st = PC_OK;
    int r;
    *handled = false;
    if (pc_rect_is_empty(a->area)) {
        *handled = true;
        return pc_surface_clone(a->surface, out, err);
    }
    r = schedule(a, p->angle, p->distance, force, out, &st, err);
    if (r == M_DECLINE) {
        return PC_OK;
    }
    *handled = true;
    if (r == M_CANCEL) {
        return pc_fail(err, PC_ERR_CANCELLED, "cancelled");
    }
    return st;
}

pc_status pc_algo_motion_apply(const pc_algo_apply_args *a, pc_surface *out, bool *handled, pc_error *err)
{
    return pc_algo_motion_apply_ex(a, false, out, handled, err);
}
