/* Focus-stack fusion (port of fuse_stack in crates/algo/src/pyramid.rs).
 *
 * Adelson, Anderson, Bergen, Burt, Ogden, "Pyramid methods in image processing", RCA Engineer 1984
 * (the multifocus composite: per Laplacian coefficient keep the image with the most band-pass
 * energy, then collapse) with the selection rules of Wang & Chang, "A Multi-focus Image Fusion
 * Method Based on Laplacian Pyramid", Journal of Computers 6(12), 2011 (maximum region energy for
 * the band-pass levels, local deviation and entropy for the residual), plus halo control. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "algo/pyramid.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define HALO_REF 1e-4f   /* reference energy the halo weights are relative to */
#define HALO_FLOOR 1e-8f /* floor added to the energy before weighing */
#define COVERED 0.5f     /* coverage below which an image has no say */
#define NO_ENERGY -1.0f  /* an uncovered image's energy: below any real one */
/* Binomial windows wider than this are refused (the reference's O(r^2) table never ends). */
#define MAX_ENERGY_RADIUS 4096

pc_algo_stack_fusion pc_algo_stack_fusion_default(void)
{
    pc_algo_stack_fusion p;
    p.energy_radius = 1;
    p.halo = 2.0f;
    p.guide = 2;
    p.has_levels = false;
    p.levels = 0;
    return p;
}

size_t pc_algo_stack_levels(size_t w, size_t h)
{
    size_t n = 1, s = w < h ? w : h;
    while (s / 2 + s % 2 >= 32 && n < 10) {
        s = s / 2 + s % 2;
        n++;
    }
    return n;
}

float pc_algo_halo_weight(float re, float p)
{
    return expf(pc_clampf(p * logf((pc_rs_maxf_k(re, 0.0f) + HALO_FLOOR) / HALO_REF), -60.0f, 60.0f));
}

pc_status pc_algo_binomial(size_t r, float **out, size_t *len, pc_error *err)
{
    size_t n, i, j;
    uint64_t *row, s = 0;
    float *wt;
    if (r > MAX_ENERGY_RADIUS) {
        return pc_fail(err, PC_ERR_LIMIT, "energy radius %zu is too large", r);
    }
    n = 2 * r + 1;
    row = pc_malloc_array(n, sizeof *row);
    wt = pc_malloc_array(n, sizeof *wt);
    if (row == NULL || wt == NULL) {
        free(row), free(wt);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a binomial window");
    }
    for (i = 0; i < n; i++) {
        row[i] = 1;
    }
    for (i = 1; i < n; i++) {
        for (j = i - 1; j >= 1; j--) {
            row[j] += row[j - 1]; /* wraps past row 67, as the reference's release build does */
        }
    }
    for (i = 0; i < n; i++) {
        s += row[i];
    }
    for (i = 0; i < n; i++) {
        wt[i] = (float)row[i] / (float)s;
    }
    free(row);
    *out = wt;
    *len = n;
    return PC_OK;
}

typedef struct ws_ctx {
    size_t w, h, r, nwt;
    const float *src, *wt;
    float *tmp, *out;
} ws_ctx;

static size_t grain_rows(size_t w)
{
    size_t g = w > 0 ? 16384 / w : 1;
    return g > 0 ? g : 1;
}

static pc_status ws_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const ws_ctx *k = vctx;
    size_t y, x, t;
    (void)chunk;
    for (y = begin; y < end; y++) {
        const float *s = k->src + y * k->w;
        for (x = 0; x < k->w; x++) {
            float acc = -0.0f; /* Iterator::sum */
            for (t = 0; t < k->nwt; t++) {
                size_t sx = x + t >= k->r ? x + t - k->r : 0;
                sx = sx < k->w - 1 ? sx : k->w - 1;
                acc += k->wt[t] * s[sx];
            }
            k->tmp[y * k->w + x] = acc;
        }
    }
    return PC_OK;
}

static pc_status ws_cols(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const ws_ctx *k = vctx;
    size_t y, x, t;
    (void)chunk;
    for (y = begin; y < end; y++) {
        float *row = k->out + y * k->w;
        for (x = 0; x < k->w; x++) {
            row[x] = 0.0f;
        }
        for (t = 0; t < k->nwt; t++) {
            size_t sy = y + t >= k->r ? y + t - k->r : 0;
            const float *v;
            sy = sy < k->h - 1 ? sy : k->h - 1;
            v = k->tmp + sy * k->w;
            for (x = 0; x < k->w; x++) {
                row[x] += k->wt[t] * v[x];
            }
        }
    }
    return PC_OK;
}

pc_status pc_algo_window_sum(size_t w, size_t h, const float *src, const float *wt, size_t nwt, float **out,
                             pc_error *err)
{
    size_t n = w * h;
    ws_ctx k;
    float *res, *tmp;
    pc_status st;
    res = pc_malloc_array(n > 0 ? n : 1, sizeof *res);
    if (res == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a window sum");
    }
    if (nwt / 2 == 0 || w == 0 || h == 0) {
        if (n > 0) {
            memcpy(res, src, n * sizeof *res);
        }
        *out = res;
        return PC_OK;
    }
    tmp = pc_malloc_array(n, sizeof *tmp);
    if (tmp == NULL) {
        free(res);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a window sum");
    }
    k.w = w, k.h = h, k.r = nwt / 2, k.nwt = nwt, k.src = src, k.wt = wt, k.tmp = tmp, k.out = res;
    st = pc_algo_par_for(h, grain_rows(w), ws_rows, &k);
    if (st == PC_OK) {
        st = pc_algo_par_for(h, grain_rows(w), ws_cols, &k);
    }
    free(tmp);
    if (st != PC_OK) {
        free(res);
        return pc_fail(err, st, "window sum failed");
    }
    *out = res;
    return PC_OK;
}

/* Region energy of a band-pass luminance level: the window sum of L^2 (Wang & Chang eq. 13);
 * NO_ENERGY where the coverage is below COVERED. */
static pc_status region_energy(const pc_algo_level *l, const pc_algo_level *cover, const float *win,
                               size_t nwin, float **out, pc_error *err)
{
    size_t n = l->w * l->h, i;
    float *e = pc_malloc_array(n > 0 ? n : 1, sizeof *e), *re = NULL;
    pc_status st;
    if (e == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for region energy");
    }
    for (i = 0; i < n; i++) {
        e[i] = l->px[i] * l->px[i];
    }
    st = pc_algo_window_sum(l->w, l->h, e, win, nwin, &re, err);
    free(e);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < n && i < cover->w * cover->h; i++) {
        if (cover->px[i] < COVERED) {
            re[i] = NO_ENERGY;
        }
    }
    *out = re;
    return PC_OK;
}

typedef struct de_ctx {
    size_t w, h;
    const float *y;
    float *dev, *ent;
} de_ctx;

static pc_status de_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    enum { R = 2, BINS = 256, N = (2 * R + 1) * (2 * R + 1) };
    const de_ctx *k = vctx;
    const float inv_n = 1.0f / (float)N;
    uint32_t hist[BINS];
    size_t touched[N], nt, i, j;
    (void)chunk;
    memset(hist, 0, sizeof hist);
    for (i = begin; i < end; i++) {
        for (j = 0; j < k->w; j++) {
            float s = 0.0f, s2 = 0.0f, mean, e = 0.0f;
            size_t dy, dx, t;
            nt = 0;
            for (dy = 0; dy <= 2 * R; dy++) {
                size_t yy = i + dy >= R ? i + dy - R : 0;
                yy = yy < k->h - 1 ? yy : k->h - 1;
                for (dx = 0; dx <= 2 * R; dx++) {
                    size_t xx = j + dx >= R ? j + dx - R : 0, q;
                    float v;
                    xx = xx < k->w - 1 ? xx : k->w - 1;
                    v = k->y[yy * k->w + xx];
                    s += v;
                    s2 += v * v;
                    q = pc_f32_to_size(pc_clampf(v, 0.0f, 1.0f) * (float)(BINS - 1) + 0.5f);
                    q = q < BINS - 1 ? q : BINS - 1;
                    if (hist[q] == 0) {
                        touched[nt++] = q;
                    }
                    hist[q]++;
                }
            }
            mean = s * inv_n;
            k->dev[i * k->w + j] = pc_rs_maxf_k(s2 * inv_n - mean * mean, 0.0f);
            for (t = 0; t < nt; t++) {
                float p = (float)hist[touched[t]] * inv_n;
                e -= p * logf(p);
                hist[touched[t]] = 0;
            }
            k->ent[i * k->w + j] = e;
        }
    }
    return PC_OK;
}

/* Local deviation (Wang & Chang eq. 10) and entropy (eq. 11, 256 levels) over a 5 x 5 window,
 * clamped borders. Exposed for tests. */
pc_status pc_algo_deviation_entropy(size_t w, size_t h, const float *y, float **dev, float **ent,
                                    pc_error *err)
{
    size_t n = w * h;
    de_ctx k;
    pc_status st;
    *dev = pc_calloc_array(n > 0 ? n : 1, sizeof **dev);
    *ent = pc_calloc_array(n > 0 ? n : 1, sizeof **ent);
    if (*dev == NULL || *ent == NULL) {
        free(*dev), free(*ent);
        *dev = *ent = NULL;
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for local measures");
    }
    if (w == 0 || h == 0) {
        return PC_OK;
    }
    k.w = w, k.h = h, k.y = y, k.dev = *dev, k.ent = *ent;
    st = pc_algo_par_for(h, grain_rows(w * 25), de_rows, &k);
    if (st != PC_OK) {
        free(*dev), free(*ent);
        *dev = *ent = NULL;
        return pc_fail(err, st, "local measures failed");
    }
    return PC_OK;
}

/* One image's residual level with its luminance and coverage there. */
typedef struct residual {
    pc_algo_level px;
    float *luma, *cover;
} residual;

static void residual_fini(residual *r)
{
    pc_algo_level_fini(&r->px);
    free(r->luma), free(r->cover);
    r->luma = r->cover = NULL;
}

/* Wang & Chang's region-information rule (eq. 12) generalised to N images as a Pareto rule: an
 * image is dominated where another covering image is at least as good on deviation and entropy and
 * strictly better on one; the result is the mean of the non-dominated images. */
static pc_status fuse_residuals(const residual *tops, size_t n, size_t ch, pc_algo_level *out, pc_error *err)
{
    size_t w, h, i, a, b, c, np;
    float **dev, **ent, *res;
    bool *keep;
    pc_status st = PC_OK;
    if (n == 0) {
        out->w = out->h = 0;
        out->px = NULL;
        return PC_OK;
    }
    w = tops[0].px.w, h = tops[0].px.h, np = w * h;
    dev = pc_calloc_array(n, sizeof *dev);
    ent = pc_calloc_array(n, sizeof *ent);
    keep = pc_calloc_array(n, sizeof *keep);
    res = pc_calloc_array(np * ch > 0 ? np * ch : 1, sizeof *res);
    if (dev == NULL || ent == NULL || keep == NULL || res == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory fusing residuals");
        goto done;
    }
    for (a = 0; a < n && st == PC_OK; a++) {
        st = pc_algo_deviation_entropy(w, h, tops[a].luma, &dev[a], &ent[a], err);
    }
    if (st != PC_OK) {
        goto done;
    }
    for (i = 0; i < np; i++) {
        bool any_cover = false;
        size_t kept = 0;
        float inv;
        for (a = 0; a < n; a++) {
            any_cover = any_cover || tops[a].cover[i] >= COVERED;
        }
        for (a = 0; a < n; a++) {
            bool elig = !any_cover || tops[a].cover[i] >= COVERED, dominated = false;
            float da = dev[a][i], ea = ent[a][i];
            for (b = 0; b < n && elig && !dominated; b++) {
                float db = dev[b][i], eb = ent[b][i];
                bool eb_ok = !any_cover || tops[b].cover[i] >= COVERED;
                dominated = b != a && eb_ok && db >= da && eb >= ea && (db > da || eb > ea);
            }
            keep[a] = elig && !dominated;
            kept += keep[a] ? 1u : 0u;
        }
        inv = 1.0f / (float)(kept > 1 ? kept : 1);
        for (a = 0; a < n; a++) {
            if (keep[a]) {
                for (c = 0; c < ch; c++) {
                    res[i * ch + c] += tops[a].px.px[i * ch + c] * inv;
                }
            }
        }
    }
    out->w = w, out->h = h, out->px = res;
    res = NULL;
done:
    if (dev != NULL) {
        for (a = 0; a < n; a++) {
            free(dev[a]);
        }
    }
    if (ent != NULL) {
        for (a = 0; a < n; a++) {
            free(ent[a]);
        }
    }
    free(dev), free(ent), free(keep), free(res);
    return st;
}

/* Halo control: fold the levels coarser than the guide g as sum w L and sum w, with the guide's
 * weights REDUCEd to each level. src == NULL scales the first image's levels (already in acc). */
static pc_status fold_halo(pc_algo_pyramid *acc, float **wsum, size_t *nwsum, const pc_algo_pyramid *src,
                           size_t g, pc_algo_level wgt, size_t ch, pc_error *err)
{
    size_t li, i, c;
    pc_status st = PC_OK;
    for (li = g + 1; li < acc->n; li++) {
        pc_algo_level next;
        pc_algo_level *a = &acc->levels[li];
        size_t np = a->w * a->h;
        st = pc_algo_pyramid_reduce(&wgt, 1, &next, err);
        if (st != PC_OK) {
            break;
        }
        pc_algo_level_fini(&wgt);
        wgt = next;
        if (wgt.w * wgt.h != np) {
            st = pc_fail(err, PC_ERR_INVALID, "halo weights do not match the pyramid");
            break;
        }
        if (src != NULL) {
            const float *sp = src->levels[li].px;
            if (li - g - 1 < *nwsum) {
                float *s = wsum[li - g - 1];
                for (i = 0; i < np; i++) {
                    s[i] += wgt.px[i];
                }
            }
            for (i = 0; i < np; i++) {
                for (c = 0; c < ch; c++) {
                    a->px[i * ch + c] += wgt.px[i] * sp[i * ch + c];
                }
            }
        } else {
            float *copy = pc_malloc_array(np > 0 ? np : 1, sizeof *copy);
            if (copy == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for halo weights");
                break;
            }
            if (np > 0) {
                memcpy(copy, wgt.px, np * sizeof *copy);
            }
            for (i = 0; i < np; i++) {
                for (c = 0; c < ch; c++) {
                    a->px[i * ch + c] *= wgt.px[i];
                }
            }
            wsum[(*nwsum)++] = copy;
        }
    }
    pc_algo_level_fini(&wgt);
    return st;
}

pc_status pc_algo_fuse_stack(size_t w, size_t h, size_t ch, const float *const *images,
                             const float *const *luma, const float *const *alpha, size_t count,
                             pc_algo_stack_fusion p, float **out, pc_error *err)
{
    size_t levels, bands, nsel, g = 0, nwin = 0, k, li, i, ntops = 0, nwsum = 0, np, len = 0;
    bool halo;
    float hard = 0.0f, *win = NULL, **best = NULL, **wsum = NULL, *res = NULL;
    pc_algo_pyramid acc = {NULL, 0};
    residual *tops = NULL;
    pc_status st;
    if (ch == 0 || !pc_size_mul3(w, h, ch, &np)) {
        return pc_fail(err, PC_ERR_LIMIT, "focus stack of %zux%zux%zu is too large", w, h, ch);
    }
    /* Levels the pyramid can actually have (REDUCE stops at 1 x 1). */
    {
        size_t want = p.has_levels ? p.levels : pc_algo_stack_levels(w, h), a = w, b = h;
        want = want > 1 ? want : 1;
        levels = 1;
        while ((a > 1 || b > 1) && levels < want) {
            a = a / 2 + a % 2, b = b / 2 + b % 2, levels++;
        }
    }
    bands = levels - 1;
    halo = bands > 0 && p.halo > 0.0f;
    if (halo) {
        g = p.guide < bands - 1 ? p.guide : bands - 1;
        hard = pc_rs_minf_k(p.halo, PC_ALGO_HALO_MAX);
    }
    nsel = halo ? g + 1 : bands;
    st = pc_algo_binomial(p.energy_radius, &win, &nwin, err);
    if (st != PC_OK) {
        return st;
    }
    best = pc_calloc_array(nsel > 0 ? nsel : 1, sizeof *best);
    wsum = pc_calloc_array(levels, sizeof *wsum);
    tops = pc_calloc_array(count > 0 ? count : 1, sizeof *tops);
    if (best == NULL || wsum == NULL || tops == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a focus stack");
        goto done;
    }
    for (k = 0; k < count && st == PC_OK; k++) {
        pc_algo_pyramid lp = {NULL, 0}, ll = {NULL, 0}, ga = {NULL, 0};
        float **energies = NULL;
        pc_algo_level wgt = {0, 0, NULL};
        st = pc_algo_pyramid_laplacian(w, h, ch, images[k], levels, &lp, err);
        if (st == PC_OK) {
            st = pc_algo_pyramid_laplacian(w, h, 1, luma[k], levels, &ll, err);
        }
        if (st == PC_OK) {
            st = pc_algo_pyramid_gaussian(w, h, 1, alpha[k], levels, &ga, err);
        }
        if (st != PC_OK || lp.n == 0 || ll.n == 0 || ga.n == 0 || lp.n != levels || ll.n != levels ||
            ga.n != levels) {
            goto next;
        }
        if (!halo) {
            const pc_algo_level *top = &lp.levels[levels - 1];
            size_t tn = top->w * top->h;
            residual *r = &tops[ntops];
            r->px.w = top->w, r->px.h = top->h;
            r->px.px = pc_malloc_array(tn * ch > 0 ? tn * ch : 1, sizeof(float));
            r->luma = pc_malloc_array(tn > 0 ? tn : 1, sizeof(float));
            r->cover = pc_malloc_array(tn > 0 ? tn : 1, sizeof(float));
            ntops++;
            if (r->px.px == NULL || r->luma == NULL || r->cover == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a focus stack");
                goto next;
            }
            memcpy(r->px.px, top->px, tn * ch * sizeof(float));
            memcpy(r->luma, ll.levels[levels - 1].px, tn * sizeof(float));
            memcpy(r->cover, ga.levels[levels - 1].px, tn * sizeof(float));
        }
        energies = pc_calloc_array(nsel > 0 ? nsel : 1, sizeof *energies);
        if (energies == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a focus stack");
            goto next;
        }
        for (li = 0; li < nsel && st == PC_OK; li++) {
            st = region_energy(&ll.levels[li], &ga.levels[li], win, nwin, &energies[li], err);
        }
        if (st != PC_OK) {
            goto next;
        }
        if (halo) {
            size_t wn = ll.levels[g].w * ll.levels[g].h;
            wgt.w = ll.levels[g].w, wgt.h = ll.levels[g].h;
            wgt.px = pc_malloc_array(wn > 0 ? wn : 1, sizeof *wgt.px);
            if (wgt.px == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a focus stack");
                goto next;
            }
            for (i = 0; i < wn; i++) {
                float e = energies[g][i];
                wgt.px[i] = e < 0.0f ? 0.0f : pc_algo_halo_weight(e, hard);
            }
        }
        if (acc.n == 0) {
            acc = lp;
            lp.levels = NULL, lp.n = 0;
            for (li = 0; li < nsel; li++) {
                best[li] = energies[li];
                energies[li] = NULL;
            }
            if (halo) {
                st = fold_halo(&acc, wsum, &nwsum, NULL, g, wgt, ch, err);
                wgt.px = NULL;
            }
            goto next;
        }
        for (li = 0; li < nsel; li++) {
            const pc_algo_level *lvl = &lp.levels[li];
            size_t n = lvl->w * lvl->h;
            for (i = 0; i < n; i++) {
                if (energies[li][i] > best[li][i]) {
                    best[li][i] = energies[li][i];
                    memcpy(acc.levels[li].px + i * ch, lvl->px + i * ch, ch * sizeof(float));
                }
            }
        }
        if (halo) {
            st = fold_halo(&acc, wsum, &nwsum, &lp, g, wgt, ch, err);
            wgt.px = NULL;
        }
    next:
        if (energies != NULL) {
            for (li = 0; li < nsel; li++) {
                free(energies[li]);
            }
            free(energies);
        }
        pc_algo_level_fini(&wgt);
        pc_algo_pyramid_fini(&lp), pc_algo_pyramid_fini(&ll), pc_algo_pyramid_fini(&ga);
    }
    if (st != PC_OK) {
        goto done;
    }
    if (acc.n == 0) {
        res = pc_calloc_array(np > 0 ? np : 1, sizeof *res);
        if (res == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a focus stack");
            goto done;
        }
        *out = res;
        res = NULL;
        goto done;
    }
    if (halo) {
        /* The weighted means: sum w L / sum w. */
        for (li = g + 1; li < levels; li++) {
            pc_algo_level *a = &acc.levels[li];
            const float *s = wsum[li - g - 1];
            size_t n = a->w * a->h, c;
            for (i = 0; i < n; i++) {
                float kk = s[i];
                for (c = 0; c < ch; c++) {
                    a->px[i * ch + c] = kk > 0.0f ? a->px[i * ch + c] / kk : 0.0f;
                }
            }
        }
    } else {
        pc_algo_level fused;
        st = fuse_residuals(tops, ntops, ch, &fused, err);
        if (st != PC_OK) {
            goto done;
        }
        pc_algo_level_fini(&acc.levels[acc.n - 1]);
        acc.levels[acc.n - 1] = fused;
    }
    st = pc_algo_pyramid_collapse(&acc, ch, &res, &len, err);
    if (st != PC_OK) {
        goto done;
    }
    for (i = 0; i < len; i++) {
        res[i] = pc_clampf(res[i], 0.0f, 1.0f);
    }
    *out = res;
    res = NULL;
done:
    free(win);
    if (best != NULL) {
        for (li = 0; li < nsel; li++) {
            free(best[li]);
        }
    }
    if (wsum != NULL) {
        for (li = 0; li < nwsum; li++) {
            free(wsum[li]);
        }
    }
    if (tops != NULL) {
        for (k = 0; k < ntops; k++) {
            residual_fini(&tops[k]);
        }
    }
    free(best), free(wsum), free(tops), free(res);
    pc_algo_pyramid_fini(&acc);
    return st;
}
