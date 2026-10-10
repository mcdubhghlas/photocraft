/* Seam carving (port of crates/algo/src/seam.rs); see seam.h. */
#include "algo/seam.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/poisson.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Energy added per unit of protection: far above any gradient magnitude. */
#define PROTECT_ENERGY 1.0e4f
/* Seam carving costs about (seams x pixels): a 24 MP image shrunk or enlarged by half its width is
 * near 2^37. Past this the reference would run for hours (plan section 7). */
#define MAX_WORK ((uint64_t)1 << 40)
/* Below this many pixels the per-row loops run serially (same results either way). */
#define PAR_MIN ((size_t)64 * 64)

/* energy of one pixel of a cw-wide image whose rows are `stride` pixels apart; shared by the full
 * map and the per-seam updates so both give bit-identical values. */
static float energy_at(const float *img, size_t stride, size_t cw, size_t h, size_t ch, size_t x, size_t y)
{
    size_t xl = x > 0 ? x - 1 : 0, xr = x + 1 < cw - 1 ? x + 1 : cw - 1;
    size_t yu = y > 0 ? y - 1 : 0, yd = y + 1 < h - 1 ? y + 1 : h - 1, c;
    float s = 0.0f;
    for (c = 0; c < ch; c++) {
        float v = img[(y * stride + x) * ch + c];
        s += fabsf(img[(y * stride + xr) * ch + c] - v) + fabsf(v - img[(y * stride + xl) * ch + c]);
        s += fabsf(img[(yd * stride + x) * ch + c] - v) + fabsf(v - img[(yu * stride + x) * ch + c]);
    }
    return s * 0.5f;
}

/* f32::total_cmp as an unsigned key. */
static uint32_t total_key(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

/* Runs fn over rows [0, h): in parallel on the global pool for big images. */
static void rows_for(size_t h, size_t px, pc_pool_fn fn, void *ctx)
{
    if (px >= PAR_MIN) {
        (void)pc_algo_par_for(h, 8, fn, ctx);
    } else {
        (void)fn(ctx, 0, 0, h);
    }
}

typedef struct energy_job {
    const float *img;
    size_t w, h, ch;
    float *e;
} energy_job;

static pc_status energy_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const energy_job *j = ctx;
    size_t x, y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        for (x = 0; x < j->w; x++) {
            j->e[y * j->w + x] = energy_at(j->img, j->w, j->w, j->h, j->ch, x, y);
        }
    }
    return PC_OK;
}

static float *energy_map(size_t w, size_t h, size_t ch, const float *img)
{
    float *e = pc_calloc_array(w * h, sizeof *e);
    energy_job j = {img, w, h, ch, e};
    if (e != NULL && w > 0 && h > 0) {
        rows_for(h, w * h, energy_rows, &j);
    }
    return e;
}

pc_status pc_algo_seam_energy(size_t w, size_t h, size_t ch, const float *img, size_t len, float **out,
                              pc_error *err)
{
    float *e;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    if (img == NULL && len > 0) {
        return pc_fail(err, PC_ERR_INVALID, "missing buffer");
    }
    e = energy_map(w, h, ch, img);
    if (e == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = e;
    return PC_OK;
}

/* ---- seam search ------------------------------------------------------------------------------- */

/* Scratch buffers for the seam search, reused across seams. */
typedef struct dp {
    float *cost, *next;
    uint8_t *from; /* where the cheapest path came from: 0 up-left, 1 up, 2 up-right */
} dp;

static bool dp_init(dp *d, size_t w, size_t h)
{
    d->cost = pc_malloc_array(w, sizeof(float));
    d->next = pc_malloc_array(w, sizeof(float));
    d->from = pc_malloc_array(w * h, 1);
    return d->cost != NULL && d->next != NULL && d->from != NULL;
}

static void dp_fini(dp *d)
{
    free(d->cost);
    free(d->next);
    free(d->from);
}

/* The lowest-energy seam of a cw-wide (cw >= 1), h-tall energy map with rows `stride` apart. Ties
 * go to the pixel straight up, then up-left, then up-right; at the bottom, to the leftmost end. */
static void dp_seam(dp *d, size_t stride, size_t cw, size_t h, const float *energy, size_t *seam)
{
    size_t x, y, best = 0;
    uint32_t bk;
    memcpy(d->cost, energy, cw * sizeof(float));
    for (y = 1; y < h; y++) {
        const float *cost = d->cost, *e = energy + y * stride;
        float *next = d->next, *t;
        uint8_t *from = d->from + y * cw;
        if (cw == 1) {
            next[0] = cost[0] + e[0];
            from[0] = 1;
        } else {
            float b;
            uint8_t dd;
            if (cost[1] < cost[0]) {
                b = cost[1], dd = 2;
            } else {
                b = cost[0], dd = 1;
            }
            next[0] = b + e[0];
            from[0] = dd;
            for (x = 1; x + 1 < cw; x++) {
                if (cost[x - 1] < cost[x]) {
                    b = cost[x - 1], dd = 0;
                } else {
                    b = cost[x], dd = 1;
                }
                if (cost[x + 1] < b) {
                    b = cost[x + 1], dd = 2;
                }
                next[x] = b + e[x];
                from[x] = dd;
            }
            if (cost[cw - 2] < cost[cw - 1]) {
                b = cost[cw - 2], dd = 0;
            } else {
                b = cost[cw - 1], dd = 1;
            }
            next[cw - 1] = b + e[cw - 1];
            from[cw - 1] = dd;
        }
        t = d->cost;
        d->cost = d->next;
        d->next = t;
    }
    bk = total_key(d->cost[0]);
    for (x = 1; x < cw; x++) {
        uint32_t k = total_key(d->cost[x]);
        if (k < bk) {
            bk = k, best = x;
        }
    }
    x = best;
    for (y = h; y-- > 0;) {
        seam[y] = x;
        if (y > 0) {
            uint8_t f = d->from[y * cw + x];
            x = f == 0 ? x - 1 : (f == 2 ? x + 1 : x);
        }
    }
}

pc_status pc_algo_seam_find(size_t w, size_t h, const float *energy, size_t len, size_t **out, pc_error *err)
{
    size_t *seam;
    dp d;
    pc_status st = pc_algo_heal_dims(w, h, 1, SIZE_MAX, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    seam = pc_calloc_array(h, sizeof *seam);
    if (seam == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (w == 0 || h == 0 || energy == NULL || len < w * h) {
        *out = seam;
        return PC_OK;
    }
    if (!dp_init(&d, w, h)) {
        dp_fini(&d);
        free(seam);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    dp_seam(&d, w, w, h, energy, seam);
    dp_fini(&d);
    *out = seam;
    return PC_OK;
}

/* ---- the in-place carver ----------------------------------------------------------------------- */

/* Shrinks an image one seam at a time in place. Rows keep their original stride, so removing a
 * seam shifts only the rest of each row, and the energy map is updated only next to the removed
 * seam: a pixel's gradient changes only when a neighbour does. */
typedef struct carver {
    size_t stride, cw, h, ch;
    float *img;
    float *prot; /* NULL: no protection */
    float *e;    /* energy plus protection, as the seam search sees it */
    size_t *idx; /* original column of each pixel, when tracking; else NULL */
    size_t *seam;
    dp dp;
} carver;

static void carver_free(carver *c)
{
    free(c->img);
    free(c->prot);
    free(c->e);
    free(c->idx);
    free(c->seam);
    dp_fini(&c->dp);
}

/* Takes ownership of img and prot (freed on failure too). */
static pc_status carver_new(carver *c, size_t w, size_t h, size_t ch, float *img, float *prot, bool track,
                            pc_error *err)
{
    size_t i, x, y;
    memset(c, 0, sizeof *c);
    c->stride = w, c->cw = w, c->h = h, c->ch = ch, c->img = img, c->prot = prot;
    c->e = energy_map(w, h, ch, img);
    c->seam = pc_malloc_array(h, sizeof *c->seam);
    if (c->e == NULL || c->seam == NULL || !dp_init(&c->dp, w, h)) {
        carver_free(c);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (prot != NULL) {
        for (i = 0; i < w * h; i++) {
            c->e[i] += pc_clampf(prot[i], 0.0f, 1.0f) * PROTECT_ENERGY;
        }
    }
    if (track) {
        c->idx = pc_malloc_array(w * h, sizeof *c->idx);
        if (c->idx == NULL) {
            carver_free(c);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                c->idx[y * w + x] = x;
            }
        }
    }
    return PC_OK;
}

typedef struct shift_job {
    carver *c;
} shift_job;

/* Remove pixel seam[y] from each row by shifting the rest of the row left (all buffers). */
static pc_status shift_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    carver *c = ((shift_job *)ctx)->c;
    size_t y, cw = c->cw, ch = c->ch, st = c->stride;
    (void)chunk;
    for (y = begin; y < end; y++) {
        size_t x = c->seam[y], tail = cw - (x + 1);
        memmove(&c->img[(y * st + x) * ch], &c->img[(y * st + x + 1) * ch], tail * ch * sizeof(float));
        memmove(&c->e[y * st + x], &c->e[y * st + x + 1], tail * sizeof(float));
        if (c->prot != NULL) {
            memmove(&c->prot[y * st + x], &c->prot[y * st + x + 1], tail * sizeof(float));
        }
        if (c->idx != NULL) {
            memmove(&c->idx[y * st + x], &c->idx[y * st + x + 1], tail * sizeof(size_t));
        }
    }
    return PC_OK;
}

/* Recompute the energy of the pixels whose neighbours changed when the seam was removed: in row
 * y, from one left of the leftmost seam x in rows y-1..=y+1 to the rightmost. */
static pc_status update_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    carver *c = ((shift_job *)ctx)->c;
    size_t y, stride = c->stride, cw = c->cw, h = c->h;
    (void)chunk;
    for (y = begin; y < end; y++) {
        size_t a = y > 0 ? y - 1 : 0, b = y + 2 < h ? y + 2 : h, k, lo = SIZE_MAX, hi = 0, x;
        for (k = a; k < b; k++) {
            lo = c->seam[k] < lo ? c->seam[k] : lo;
            hi = c->seam[k] > hi ? c->seam[k] : hi;
        }
        lo = lo > 0 ? lo - 1 : 0;
        hi = hi < cw - 1 ? hi : cw - 1;
        for (x = lo; x <= hi; x++) {
            float v = energy_at(c->img, stride, cw, h, c->ch, x, y);
            if (c->prot != NULL) {
                v += pc_clampf(c->prot[y * stride + x], 0.0f, 1.0f) * PROTECT_ENERGY;
            }
            c->e[y * stride + x] = v;
        }
    }
    return PC_OK;
}

/* Remove the lowest-energy seam; c->seam holds the removed columns (current coordinates). When
 * `removed` is non-NULL it receives the original columns (tracking). */
static void carver_remove(carver *c, size_t *removed)
{
    size_t y;
    shift_job j = {c};
    dp_seam(&c->dp, c->stride, c->cw, c->h, c->e, c->seam);
    if (removed != NULL) {
        for (y = 0; y < c->h; y++) {
            removed[y] = c->idx != NULL ? c->idx[y * c->stride + c->seam[y]] : c->seam[y];
        }
    }
    rows_for(c->h, c->cw * c->h, shift_rows, &j);
    c->cw--;
    if (c->cw > 0) {
        rows_for(c->h, c->cw * c->h, update_rows, &j);
    }
}

/* The carved pixels, cw x h x ch, rows packed (in place). */
static float *carver_take_pixels(carver *c)
{
    size_t y, row = c->stride * c->ch, used = c->cw * c->ch;
    float *img = c->img;
    if (row != used) {
        for (y = 1; y < c->h; y++) {
            memmove(&img[y * used], &img[y * row], used * sizeof(float));
        }
    }
    c->img = NULL;
    return img;
}

static float *dup_floats(const float *v, size_t n)
{
    float *o = pc_malloc_array(n, sizeof *o);
    if (o != NULL && n > 0) {
        memcpy(o, v, n * sizeof *o);
    }
    return o;
}

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static pc_status carve_width_impl(size_t w, size_t h, size_t ch, const float *img, const float *protect,
                                  size_t new_w, const pc_interrupt *ctl, float **out, size_t *out_len,
                                  pc_error *err)
{
    size_t cw = w, n = w * h;
    float seams, *cur = NULL, *prot = NULL;
    pc_status st;
    carver c;
    new_w = new_w > 1 ? new_w : 1;
    if (new_w == w || w == 0 || h == 0) {
        float *o = dup_floats(img, n * ch);
        if (o == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        *out = o, *out_len = n * ch;
        return PC_OK;
    }
    if ((st = pc_algo_heal_dims(new_w, h, ch, SIZE_MAX, NULL, err)) != PC_OK) {
        return st;
    }
    seams = (float)(new_w > w ? new_w - w : w - new_w);
    seams = seams > 1.0f ? seams : 1.0f;
    cur = dup_floats(img, n * ch);
    prot = protect != NULL ? dup_floats(protect, n) : NULL;
    if (cur == NULL || (protect != NULL && prot == NULL)) {
        free(cur);
        free(prot);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (new_w < w) {
        if ((st = carver_new(&c, w, h, ch, cur, prot, false, err)) != PC_OK) {
            return st;
        }
        while (c.cw > new_w) {
            if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
                carver_free(&c);
                return st;
            }
            pc_interrupt_progress(ctl, (float)(w - c.cw) / seams);
            carver_remove(&c, NULL);
        }
        *out = carver_take_pixels(&c);
        *out_len = new_w * h * ch;
        carver_free(&c);
        return PC_OK;
    }
    /* Enlarge in rounds of at most half the current width, each duplicating the k lowest seams. */
    while (cw < new_w) {
        size_t k = new_w - cw < (cw / 2 > 1 ? cw / 2 : 1) ? new_w - cw : (cw / 2 > 1 ? cw / 2 : 1);
        size_t added = 0, nw, y, x, q, *dup, *removed;
        float *ncur, *nprot = NULL, *o, *np;
        /* Find k seams on a shrinking copy, mapping each back to original columns. */
        float *ccopy = dup_floats(cur, cw * h * ch), *pcopy = prot != NULL ? dup_floats(prot, cw * h) : NULL;
        dup = pc_malloc_array(h, k * sizeof(size_t));
        removed = pc_malloc_array(h, sizeof(size_t));
        if (ccopy == NULL || (prot != NULL && pcopy == NULL) || dup == NULL || removed == NULL) {
            free(ccopy), free(pcopy), free(dup), free(removed), free(cur), free(prot);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        if ((st = carver_new(&c, cw, h, ch, ccopy, pcopy, true, err)) != PC_OK) {
            free(dup), free(removed), free(cur), free(prot);
            return st;
        }
        for (q = 0; q < k; q++) {
            /* A 1 px wide image's only seam is its one column: take it, so it is duplicated. */
            if (c.cw == 0) {
                break;
            }
            if ((st = pc_interrupt_check(ctl, err)) != PC_OK) {
                carver_free(&c), free(dup), free(removed), free(cur), free(prot);
                return st;
            }
            {
                size_t done = cw + (cw - c.cw);
                pc_interrupt_progress(ctl, (float)(done > w ? done - w : 0) / seams);
            }
            carver_remove(&c, removed);
            for (y = 0; y < h; y++) {
                dup[y * k + added] = removed[y];
            }
            added++;
        }
        carver_free(&c);
        free(removed);
        if (added == 0) {
            free(dup);
            break;
        }
        nw = cw + added;
        ncur = pc_malloc_array(nw * h, ch * sizeof(float));
        if (prot != NULL) {
            nprot = pc_malloc_array(nw, h * sizeof(float));
        }
        if (ncur == NULL || (prot != NULL && nprot == NULL)) {
            free(ncur), free(nprot), free(dup), free(cur), free(prot);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        o = ncur, np = nprot;
        for (y = 0; y < h; y++) {
            size_t *d = &dup[y * k], di = 0, ci;
            const float *row = &cur[y * cw * ch];
            qsort(d, added, sizeof *d, cmp_size);
            for (x = 0; x < cw; x++) {
                memcpy(o, &row[x * ch], ch * sizeof(float));
                o += ch;
                if (np != NULL) {
                    *np++ = prot[y * cw + x];
                }
                while (di < added && d[di] == x) {
                    size_t xr = x + 1 < cw - 1 ? x + 1 : cw - 1;
                    for (ci = 0; ci < ch; ci++) {
                        *o++ = (row[x * ch + ci] + row[xr * ch + ci]) * 0.5f;
                    }
                    if (np != NULL) {
                        *np++ = prot[y * cw + x];
                    }
                    di++;
                }
            }
        }
        free(dup);
        free(cur);
        free(prot);
        cur = ncur, prot = nprot, cw = nw;
    }
    free(prot);
    *out = cur;
    *out_len = cw * h * ch;
    return PC_OK;
}

/* Rough cost of changing a w-wide, h-tall image to new_w by seams. */
static uint64_t carve_work(size_t w, size_t h, size_t new_w)
{
    uint64_t seams = new_w > w ? (uint64_t)(new_w - w) : (uint64_t)(w - new_w);
    uint64_t wide = new_w > w ? (uint64_t)new_w : (uint64_t)w;
    return seams * wide * (uint64_t)h; /* each factor <= 2^24: no overflow */
}

static pc_status check_input(size_t w, size_t h, size_t ch, const float *img, size_t len, size_t new_w,
                             size_t new_h, pc_error *err)
{
    pc_status st = pc_algo_heal_dims(w, h, ch, len, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    if (img == NULL && len > 0) {
        return pc_fail(err, PC_ERR_INVALID, "missing buffer");
    }
    if (new_w > PC_ALGO_HEAL_MAX_SIDE || new_h > PC_ALGO_HEAL_MAX_SIDE) {
        return pc_fail(err, PC_ERR_LIMIT, "target size %zu x %zu is too large", new_w, new_h);
    }
    if (w > 0 && h > 0 &&
        carve_work(w, h, new_w > 1 ? new_w : 1) +
                carve_work(h, new_w > 1 ? new_w : 1, new_h > 1 ? new_h : 1) >
            MAX_WORK) {
        return pc_fail(err, PC_ERR_LIMIT, "seam carving %zu x %zu to %zu x %zu is too much work", w, h, new_w,
                       new_h);
    }
    return PC_OK;
}

pc_status pc_algo_seam_carve_width(size_t w, size_t h, size_t ch, const float *img, size_t len,
                                   const float *protect, size_t new_w, const pc_interrupt *ctl, float **out,
                                   size_t *out_len, pc_error *err)
{
    pc_status st = check_input(w, h, ch, img, len, new_w, h, err);
    if (st != PC_OK) {
        return st;
    }
    return carve_width_impl(w, h, ch, img, protect, new_w, ctl, out, out_len, err);
}

static float *transpose(size_t w, size_t h, size_t ch, const float *img)
{
    size_t x, y;
    float *o = pc_malloc_array(w * h, ch * sizeof(float));
    if (o == NULL) {
        return NULL;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            memcpy(&o[(x * h + y) * ch], &img[(y * w + x) * ch], ch * sizeof(float));
        }
    }
    return o;
}

static float *resize_bilinear(size_t w, size_t h, size_t ch, const float *img, size_t nw, size_t nh)
{
    size_t x, y, c;
    float *o = pc_calloc_array(nw * nh, ch * sizeof(float));
    if (o == NULL || w == 0 || h == 0) {
        return o;
    }
    for (y = 0; y < nh; y++) {
        float fy = pc_clampf(((float)y + 0.5f) * (float)h / (float)nh - 0.5f, 0.0f, (float)(h - 1));
        size_t y0 = pc_f32_to_size(floorf(fy)), y1 = y0 + 1 < h - 1 ? y0 + 1 : h - 1;
        float ty = fy - truncf(fy);
        for (x = 0; x < nw; x++) {
            float fx = pc_clampf(((float)x + 0.5f) * (float)w / (float)nw - 0.5f, 0.0f, (float)(w - 1));
            size_t x0 = pc_f32_to_size(floorf(fx)), x1 = x0 + 1 < w - 1 ? x0 + 1 : w - 1;
            float tx = fx - truncf(fx);
            for (c = 0; c < ch; c++) {
                float p00 = img[(y0 * w + x0) * ch + c], p10 = img[(y0 * w + x1) * ch + c];
                float p01 = img[(y1 * w + x0) * ch + c], p11 = img[(y1 * w + x1) * ch + c];
                float top = p00 + (p10 - p00) * tx;
                float bot = p01 + (p11 - p01) * tx;
                o[(y * nw + x) * ch + c] = top + (bot - top) * ty;
            }
        }
    }
    return o;
}

pc_status pc_algo_seam_resize_bilinear(size_t w, size_t h, size_t ch, const float *img, size_t len, size_t nw,
                                       size_t nh, float **out, pc_error *err)
{
    float *o;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, NULL, err);
    if (st != PC_OK || (st = pc_algo_heal_dims(nw, nh, ch, SIZE_MAX, NULL, err)) != PC_OK) {
        return st;
    }
    if (img == NULL && len > 0) {
        return pc_fail(err, PC_ERR_INVALID, "missing buffer");
    }
    o = resize_bilinear(w, h, ch, img, nw, nh);
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = o;
    return PC_OK;
}

/* Progress split between the width and height passes. */
typedef struct split_ctl {
    const pc_interrupt *outer;
    float offset, scale;
} split_ctl;

static bool split_cancel(void *ctx)
{
    return pc_interrupt_cancelled(((const split_ctl *)ctx)->outer);
}

static void split_progress(void *ctx, float f)
{
    const split_ctl *s = ctx;
    pc_interrupt_progress(s->outer, s->offset + f * s->scale);
}

pc_status pc_algo_seam_carve(size_t w, size_t h, size_t ch, const float *img, size_t len,
                             const float *protect, size_t new_w, size_t new_h, const pc_interrupt *ctl,
                             float **out, size_t *out_len, pc_error *err)
{
    float sw, shh, split, *wide = NULL, *prot_t = NULL, *t = NULL, *tall = NULL, *res;
    size_t wide_len = 0, tall_len = 0;
    split_ctl s1, s2;
    pc_interrupt i1, i2;
    pc_status st = check_input(w, h, ch, img, len, new_w, new_h, err);
    if (st != PC_OK) {
        return st;
    }
    new_w = new_w > 1 ? new_w : 1;
    new_h = new_h > 1 ? new_h : 1;
    if (w > 0 && h > 0 && (st = pc_algo_heal_dims(new_w, new_h, ch, SIZE_MAX, NULL, err)) != PC_OK) {
        return st;
    }
    if (w == 0 || h == 0) {
        /* The reference indexes past the empty buffer here; an empty input stays empty. */
        res = pc_malloc(0);
        if (res == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        *out = res, *out_len = 0;
        return PC_OK;
    }
    /* Split progress between the passes by their seam counts. */
    sw = (float)(new_w > w ? new_w - w : w - new_w);
    shh = (float)(new_h > h ? new_h - h : h - new_h);
    split = sw + shh > 0.0f ? sw / (sw + shh) : 1.0f;
    s1 = (split_ctl){ctl, 0.0f, split};
    i1 = (pc_interrupt){split_cancel, split_progress, &s1};
    if ((st = carve_width_impl(w, h, ch, img, protect, new_w, &i1, &wide, &wide_len, err)) != PC_OK) {
        return st;
    }
    if (new_h == h) {
        *out = wide, *out_len = wide_len;
        return PC_OK;
    }
    if ((st = pc_algo_heal_dims(new_w, new_h, ch, SIZE_MAX, NULL, err)) != PC_OK) {
        free(wide);
        return st;
    }
    /* Protection follows the width change by plain resampling (it only steers seams). */
    if (protect != NULL) {
        float *r = resize_bilinear(w, h, 1, protect, new_w, h);
        prot_t = r != NULL ? transpose(new_w, h, 1, r) : NULL;
        free(r);
        if (prot_t == NULL) {
            free(wide);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    t = transpose(new_w, h, ch, wide);
    free(wide);
    if (t == NULL) {
        free(prot_t);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s2 = (split_ctl){ctl, split, 1.0f - split};
    i2 = (pc_interrupt){split_cancel, split_progress, &s2};
    st = carve_width_impl(h, new_w, ch, t, prot_t, new_h, &i2, &tall, &tall_len, err);
    free(t);
    free(prot_t);
    if (st != PC_OK) {
        return st;
    }
    res = transpose(new_h, new_w, ch, tall);
    free(tall);
    if (res == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    *out = res, *out_len = new_w * new_h * ch;
    return PC_OK;
}

pc_status pc_algo_seam_skin_mask(size_t w, size_t h, size_t ch, const float *img, size_t len, float **out,
                                 pc_error *err)
{
    size_t n, i;
    float *m;
    pc_status st = pc_algo_heal_dims(w, h, ch, len, &n, err);
    if (st != PC_OK) {
        return st;
    }
    m = pc_calloc_array(n, sizeof *m);
    if (m == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (ch >= 3) {
        for (i = 0; i < n; i++) {
            const float *p = &img[i * ch];
            float r = p[0] * 255.0f, g = p[1] * 255.0f, b = p[2] * 255.0f;
            float mx = pc_rs_maxf(pc_rs_maxf(r, g), b), mn = pc_rs_minf(pc_rs_minf(r, g), b);
            if (r > 95.0f && g > 40.0f && b > 20.0f && mx - mn > 15.0f && fabsf(r - g) > 15.0f && r > g &&
                r > b) {
                m[i] = 1.0f;
            }
        }
    }
    *out = m;
    return PC_OK;
}
