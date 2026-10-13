/* One min cut over the 8-connected pixel grid (port of segment/mod.rs grid_cut* and
 * segment/edge_starts.rs).
 *
 * Only free pixels become graph nodes; links to fixed neighbours fold into terminal capacities.
 * When few pixels are free, only the canonical links touching one are visited: their source
 * pixels are the free pixels merged with their left, upper, upper-left and upper-right
 * neighbours, five sorted streams merged without a dense bitmap. Otherwise the original raster
 * traversal (and its arithmetic order) is kept. Either visits the same links in the same order,
 * so the graph and the cut are identical. */
#include <math.h>
#include <stdlib.h>

#include "algo/segment.h"
#include "base/mem.h"
#include "base/num.h"

#define SQRT_2_F 1.41421356237309504880f

static const int32_t NB[4][2] = {{1, 0}, {0, 1}, {1, 1}, {-1, 1}};
static const float NB_LEN[4] = {1.0f, 1.0f, SQRT_2_F, SQRT_2_F};

static float d2(const float a[3], const float b[3])
{
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z;
}

/* ---- edge starts ---- */

static void advance(pc_algo_edge_starts *s, int k)
{
    s->has[k] = false;
    while (s->at[k] < s->n) {
        size_t i = s->free_px[s->at[k]++], x = i % s->w;
        switch (k) {
        case 0:
            s->peek[k] = i, s->has[k] = true;
            break;
        case 1:
            if (x > 0) {
                s->peek[k] = i - 1, s->has[k] = true;
            }
            break;
        case 2:
            if (i >= s->w) {
                s->peek[k] = i - s->w, s->has[k] = true;
            }
            break;
        case 3:
            if (i >= s->w && x > 0) {
                s->peek[k] = i - s->w - 1, s->has[k] = true;
            }
            break;
        default:
            if (i >= s->w && x + 1 < s->w) {
                s->peek[k] = i - s->w + 1, s->has[k] = true;
            }
            break;
        }
        if (s->has[k]) {
            return;
        }
    }
}

void pc_algo_segment_edge_starts_init(pc_algo_edge_starts *s, const size_t *free_px, size_t n, size_t w)
{
    int k;
    s->free_px = free_px;
    s->n = w > 0 ? n : 0;
    s->w = w;
    for (k = 0; k < 5; k++) {
        s->at[k] = 0;
        advance(s, k);
    }
}

bool pc_algo_segment_edge_starts_next(pc_algo_edge_starts *s, size_t *out)
{
    size_t first = SIZE_MAX;
    bool any = false;
    int k;
    for (k = 0; k < 5; k++) {
        if (s->has[k] && (!any || s->peek[k] < first)) {
            first = s->peek[k];
            any = true;
        }
    }
    if (!any) {
        return false;
    }
    for (k = 0; k < 5; k++) {
        if (s->has[k] && s->peek[k] == first) {
            advance(s, k);
        }
    }
    *out = first;
    return true;
}

/* ---- grid cut ---- */

static void fixed_link(pc_algo_maxflow *g, size_t node, uint8_t fixed, float wt)
{
    if (fixed == PC_ALGO_SEGMENT_HARD_FG) {
        /* Labelling node background cuts the link. */
        pc_algo_maxflow_add_tweights(g, node, wt, 0.0f);
    } else {
        pc_algo_maxflow_add_tweights(g, node, 0.0f, wt);
    }
}

typedef struct cut_job {
    const pc_algo_rgb_image *img;
    const uint8_t *fixed;
    const uint32_t *idx;
    pc_algo_cut_weight_fn weight;
    void *wctx;
    pc_algo_maxflow *g;
    bool optimized;
} cut_job;

static pc_status visit(const cut_job *c, size_t i, int64_t x, int64_t y, pc_error *err)
{
    int64_t w = (int64_t)c->img->w, h = (int64_t)c->img->h;
    const float *a = c->img->px[i];
    int k;
    for (k = 0; k < 4; k++) {
        int64_t nx = x + NB[k][0], ny = y + NB[k][1];
        size_t j;
        float wt;
        bool fi, fj;
        if (nx < 0 || nx >= w || ny >= h) {
            continue;
        }
        j = (size_t)ny * c->img->w + (size_t)nx;
        fi = c->idx[i] != UINT32_MAX;
        fj = c->idx[j] != UINT32_MAX;
        if (c->optimized && !fi && !fj) {
            continue;
        }
        wt = c->weight(c->wctx, d2(a, c->img->px[j])) / NB_LEN[k];
        if (fi && fj) {
            pc_status st = pc_algo_maxflow_add_edge(c->g, c->idx[i], c->idx[j], wt, wt, err);
            if (st != PC_OK) {
                return st;
            }
        } else if (fi) {
            /* Neighbour fixed: disagreeing with it costs wt. */
            fixed_link(c->g, c->idx[i], c->fixed[j], wt);
        } else if (fj) {
            fixed_link(c->g, c->idx[j], c->fixed[i], wt);
        }
    }
    return PC_OK;
}

pc_status pc_algo_segment_grid_cut_impl(const pc_algo_rgb_image *img, const float *cost_fg,
                                        const float *cost_bg, const uint8_t *fixed,
                                        pc_algo_cut_weight_fn weight, void *wctx, const pc_interrupt *ctl,
                                        bool optimized, bool *out, pc_error *err)
{
    size_t n, i, count = 0, edges, *free_px = NULL, nfree = 0;
    uint32_t *idx;
    cut_job c;
    double flow;
    bool *res;
    pc_status st = PC_OK;
    if (!pc_size_mul(img->w, img->h, &n) || n >= (size_t)UINT32_MAX - 2 || img->w > INT32_MAX ||
        img->h > INT32_MAX || n > PC_MAX_ALLOC / sizeof(size_t)) {
        return pc_fail(err, PC_ERR_LIMIT, "graph cut too large");
    }
    if (n > 0 && (img->px == NULL || cost_fg == NULL || cost_bg == NULL || fixed == NULL)) {
        return pc_fail(err, PC_ERR_INVALID, "graph cut inputs missing");
    }
    idx = pc_malloc_array(n ? n : 1, sizeof(uint32_t));
    res = pc_malloc_array(n ? n : 1, sizeof(bool));
    if (idx == NULL || res == NULL) {
        free(idx);
        free(res);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a graph cut");
    }
    for (i = 0; i < n; i++) {
        res[i] = fixed[i] == PC_ALGO_SEGMENT_HARD_FG;
        idx[i] = fixed[i] == PC_ALGO_SEGMENT_FREE ? (uint32_t)count++ : UINT32_MAX;
    }
    c.g = NULL;
    if (count == 0) {
        goto done;
    }
    edges = count * 4;
    if ((st = pc_algo_maxflow_new(count, edges, &c.g, err)) != PC_OK) {
        goto fail;
    }
    for (i = 0; i < n; i++) {
        if (idx[i] != UINT32_MAX) {
            pc_algo_maxflow_add_tweights(c.g, idx[i], cost_bg[i], cost_fg[i]);
        }
    }
    c.img = img;
    c.fixed = fixed;
    c.idx = idx;
    c.weight = weight;
    c.wctx = wctx;
    c.optimized = optimized;
    if (optimized && count < n / 8) {
        pc_algo_edge_starts it;
        size_t s;
        free_px = pc_malloc_array(count, sizeof(size_t));
        if (free_px == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a graph cut");
            goto fail;
        }
        for (i = 0; i < n; i++) {
            if (idx[i] != UINT32_MAX) {
                free_px[nfree++] = i;
            }
        }
        pc_algo_segment_edge_starts_init(&it, free_px, nfree, img->w);
        while (pc_algo_segment_edge_starts_next(&it, &s)) {
            if ((st = visit(&c, s, (int64_t)(s % img->w), (int64_t)(s / img->w), err)) != PC_OK) {
                goto fail;
            }
        }
    } else {
        int64_t x, y;
        /* The original dense raster traversal, including its arithmetic order. */
        for (y = 0; y < (int64_t)img->h; y++) {
            for (x = 0; x < (int64_t)img->w; x++) {
                if ((st = visit(&c, (size_t)y * img->w + (size_t)x, x, y, err)) != PC_OK) {
                    goto fail;
                }
            }
        }
    }
    st = optimized ? pc_algo_maxflow_run_with(c.g, ctl, &flow, err) : pc_algo_maxflow_run_bk(c.g, &flow, err);
    if (st != PC_OK) {
        goto fail;
    }
    for (i = 0; i < n; i++) {
        if (idx[i] != UINT32_MAX) {
            res[i] = pc_algo_maxflow_in_source(c.g, idx[i]);
        }
    }
done:
    for (i = 0; i < n; i++) {
        out[i] = res[i];
    }
fail:
    pc_algo_maxflow_free(c.g);
    free(free_px);
    free(idx);
    free(res);
    return st;
}

typedef struct gauss_w {
    float gamma, beta;
} gauss_w;

static float gauss_weight(void *ctx, float dd)
{
    const gauss_w *g = ctx;
    return g->gamma * expf(-g->beta * dd);
}

pc_status pc_algo_segment_grid_cut(const pc_algo_rgb_image *img, const float *cost_fg, const float *cost_bg,
                                   const uint8_t *fixed, float gamma, float beta, bool *out, pc_error *err)
{
    gauss_w g = {gamma, beta};
    return pc_algo_segment_grid_cut_impl(img, cost_fg, cost_bg, fixed, gauss_weight, &g, NULL, true, out,
                                         err);
}

pc_status pc_algo_segment_grid_cut_with(const pc_algo_rgb_image *img, const float *cost_fg,
                                        const float *cost_bg, const uint8_t *fixed,
                                        pc_algo_cut_weight_fn weight, void *wctx, const pc_interrupt *ctl,
                                        bool *out, pc_error *err)
{
    return pc_algo_segment_grid_cut_impl(img, cost_fg, cost_bg, fixed, weight, wctx, ctl, true, out, err);
}
