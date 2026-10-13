/* Binary Euclidean distance transform (port of crates/algo/src/selection/distance.rs).
 *
 * Nearest seeds along columns (two sweeps, integer distances), then the lower envelope of
 * squared-distance parabolas along rows (Felzenszwalb and Huttenlocher, 2012) with f64 boundaries,
 * so adjacent sites far along a PSB-sized row stay distinct. Columns and 32-row bands run on the
 * global pool for large masks; every value depends only on its own column / row, so results don't
 * depend on the thread count. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/selection.h"
#include "base/mem.h"
#include "base/num.h"

#define NO_SEED UINT32_MAX
/* The finite result for a mask with no selected pixel. */
#define EMPTY_DISTANCE 1e10f
#define BAND_ROWS 32
/* Below this many pixels the passes run on the calling thread. */
#define PAR_MIN 65536

typedef struct edt_ctx {
    const bool *inside;
    size_t w, h;
    uint32_t *vertical; /* column-major */
    float *out;
} edt_ctx;

static pc_status columns(void *ctx, size_t chunk, size_t begin, size_t end)
{
    edt_ctx *c = ctx;
    size_t x, y;
    (void)chunk;
    for (x = begin; x < end; x++) {
        uint32_t *col = c->vertical + x * c->h;
        bool have = false;
        size_t seed = 0;
        for (y = 0; y < c->h; y++) {
            if (c->inside[y * c->w + x]) {
                have = true;
                seed = y;
            }
            if (have) {
                col[y] = (uint32_t)(y - seed);
            }
        }
        have = false;
        for (y = c->h; y-- > 0;) {
            if (col[y] == 0) {
                have = true;
                seed = y;
            }
            if (have && (uint32_t)(seed - y) < col[y]) {
                col[y] = (uint32_t)(seed - y);
            }
        }
    }
    return PC_OK;
}

/* Lower envelope of parabolas with integer vertical distances: O(row length). */
static void row_distance(const uint32_t *costs, size_t w, float *out, size_t *sites, double *starts)
{
    size_t count = 0, q, k, x;
    for (q = 0; q < w; q++) {
        double fq, start = -INFINITY;
        if (costs[q] == NO_SEED) {
            continue;
        }
        fq = (double)costs[q] * (double)costs[q];
        while (count > 0) {
            size_t p = sites[count - 1];
            double dp = (double)costs[p];
            /* (q^2 - p^2)/(2(q-p)) = (q+p)/2, avoiding large rounded coordinate squares. */
            start = (fq - dp * dp) / (2.0 * (double)(q - p)) + ((double)q + (double)p) * 0.5;
            if (start > starts[count - 1]) {
                break;
            }
            count--;
        }
        if (count == 0) {
            start = -INFINITY;
        }
        sites[count] = q;
        starts[count] = start;
        count++;
    }
    if (count == 0) {
        for (x = 0; x < w; x++) {
            out[x] = EMPTY_DISTANCE;
        }
        return;
    }
    k = 0;
    for (x = 0; x < w; x++) {
        double dx, v;
        while (k + 1 < count && starts[k + 1] < (double)x) {
            k++;
        }
        dx = (double)x - (double)sites[k];
        v = (double)costs[sites[k]];
        out[x] = (float)sqrt(dx * dx + v * v);
    }
}

static pc_status bands(void *ctx, size_t chunk, size_t begin, size_t end)
{
    edt_ctx *c = ctx;
    size_t w = c->w, h = c->h, band;
    uint32_t *costs = pc_malloc_array(w * BAND_ROWS, sizeof(uint32_t));
    size_t *sites = pc_malloc_array(w, sizeof(size_t));
    double *starts = pc_malloc_array(w, sizeof(double));
    (void)chunk;
    if (costs == NULL || sites == NULL || starts == NULL) {
        free(costs);
        free(sites);
        free(starts);
        return PC_ERR_NOMEM;
    }
    for (band = begin; band < end; band++) {
        size_t y0 = band * BAND_ROWS, rows = h - y0 < BAND_ROWS ? h - y0 : BAND_ROWS, x, y;
        for (x = 0; x < w; x++) {
            const uint32_t *span = c->vertical + x * h + y0;
            for (y = 0; y < rows; y++) {
                costs[y * w + x] = span[y];
            }
        }
        for (y = 0; y < rows; y++) {
            row_distance(costs + y * w, w, c->out + (y0 + y) * w, sites, starts);
        }
    }
    free(costs);
    free(sites);
    free(starts);
    return PC_OK;
}

pc_status pc_algo_selection_edt(const bool *inside, size_t w, size_t h, float **out, pc_error *err)
{
    size_t n, i, nb;
    bool any = false, all = true;
    edt_ctx c;
    float *o;
    pc_status st;
    if (!pc_size_mul(w, h, &n) || n > PC_MAX_ALLOC / sizeof(float) || w > UINT32_MAX || h > UINT32_MAX) {
        return pc_fail(err, PC_ERR_LIMIT, "distance transform too large");
    }
    if (n == 0) {
        *out = NULL;
        return PC_OK;
    }
    for (i = 0; i < n; i++) {
        any |= inside[i];
        all &= inside[i];
    }
    o = pc_malloc_array(n, sizeof(float));
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a distance transform");
    }
    if (!any || all) {
        for (i = 0; i < n; i++) {
            o[i] = any ? 0.0f : EMPTY_DISTANCE;
        }
        *out = o;
        return PC_OK;
    }
    c.inside = inside;
    c.w = w;
    c.h = h;
    c.out = o;
    c.vertical = pc_malloc_array(n, sizeof(uint32_t));
    if (c.vertical == NULL) {
        free(o);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a distance transform");
    }
    for (i = 0; i < n; i++) {
        c.vertical[i] = NO_SEED;
    }
    nb = (h + BAND_ROWS - 1) / BAND_ROWS;
    if (n < PAR_MIN) {
        (void)columns(&c, 0, 0, w);
        st = bands(&c, 0, 0, nb);
    } else {
        (void)pc_algo_par_for(w, 1, columns, &c);
        st = pc_algo_par_for(nb, 1, bands, &c);
    }
    free(c.vertical);
    if (st != PC_OK) {
        free(o);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a distance transform");
    }
    *out = o;
    return PC_OK;
}
