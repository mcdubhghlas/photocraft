/* Laplacian pyramid blending, focus-stack and panorama weights (port of crates/algo/src/pyramid.rs). */
#include "algo/pyramid.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

static const float K[5] = {1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f};

void pc_algo_level_fini(pc_algo_level *l)
{
    if (l != NULL) {
        free(l->px);
        l->px = NULL;
        l->w = l->h = 0;
    }
}

void pc_algo_pyramid_fini(pc_algo_pyramid *p)
{
    size_t i;
    if (p == NULL) {
        return;
    }
    for (i = 0; i < p->n; i++) {
        pc_algo_level_fini(&p->levels[i]);
    }
    free(p->levels);
    p->levels = NULL;
    p->n = 0;
}

static pc_status alloc_plane(size_t w, size_t h, size_t ch, float **out, size_t *len, pc_error *err)
{
    size_t n;
    if (!pc_size_mul3(w, h, ch, &n) || n > PC_MAX_ALLOC / sizeof(float)) {
        return pc_fail(err, PC_ERR_LIMIT, "pyramid level of %zux%zu x %zu is too large", w, h, ch);
    }
    *out = pc_calloc_array(n > 0 ? n : 1, sizeof(float));
    if (*out == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a pyramid level");
    }
    if (len != NULL) {
        *len = n;
    }
    return PC_OK;
}

static size_t clampz(int64_t v, size_t hi)
{
    return v < 0 ? 0 : ((uint64_t)v > hi ? hi : (size_t)v);
}

/* ---- REDUCE ---- */

typedef struct pass_ctx {
    const float *src;
    float *dst;
    size_t sw, sh, w, ch; /* source level size, output row width */
} pass_ctx;

static size_t grain_for(size_t row)
{
    size_t g = row > 0 ? 16384 / row : 1;
    return g > 0 ? g : 1;
}

/* Horizontal pass on even columns: dst row y (w samples) from source row y. */
static pc_status reduce_h(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const pass_ctx *p = vctx;
    size_t y, x, k, c;
    (void)chunk;
    for (y = begin; y < end; y++) {
        float *trow = p->dst + y * p->w * p->ch;
        for (x = 0; x < p->w; x++) {
            for (k = 0; k < 5; k++) {
                size_t sx = clampz(2 * (int64_t)x + (int64_t)k - 2, p->sw - 1);
                for (c = 0; c < p->ch; c++) {
                    trow[x * p->ch + c] += K[k] * p->src[(y * p->sw + sx) * p->ch + c];
                }
            }
        }
    }
    return PC_OK;
}

/* Vertical pass on even rows: dst row y from tmp (w wide, sh rows). */
static pc_status reduce_v(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const pass_ctx *p = vctx;
    size_t y, x, k, c;
    (void)chunk;
    for (y = begin; y < end; y++) {
        float *prow = p->dst + y * p->w * p->ch;
        for (k = 0; k < 5; k++) {
            size_t sy = clampz(2 * (int64_t)y + (int64_t)k - 2, p->sh - 1);
            for (x = 0; x < p->w; x++) {
                for (c = 0; c < p->ch; c++) {
                    prow[x * p->ch + c] += K[k] * p->src[(sy * p->w + x) * p->ch + c];
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_algo_pyramid_reduce(const pc_algo_level *l, size_t ch, pc_algo_level *out, pc_error *err)
{
    size_t w = (l->w + 1) / 2, h = (l->h + 1) / 2;
    float *tmp = NULL, *px = NULL;
    pass_ctx p;
    pc_status st = alloc_plane(w, l->h, ch, &tmp, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    st = alloc_plane(w, h, ch, &px, NULL, err);
    if (st != PC_OK) {
        free(tmp);
        return st;
    }
    if (w * ch > 0 && l->h > 0) {
        p.src = l->px;
        p.dst = tmp;
        p.sw = l->w;
        p.sh = l->h;
        p.w = w;
        p.ch = ch;
        (void)pc_algo_par_for(l->h, grain_for(w * ch), reduce_h, &p);
        p.src = tmp;
        p.dst = px;
        (void)pc_algo_par_for(h, grain_for(w * ch), reduce_v, &p);
    }
    free(tmp);
    out->w = w;
    out->h = h;
    out->px = px;
    return PC_OK;
}

/* ---- EXPAND ---- */

static pc_status expand_h(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const pass_ctx *p = vctx;
    size_t y, x, k, c;
    (void)chunk;
    for (y = begin; y < end; y++) {
        float *trow = p->dst + y * p->w * p->ch;
        for (x = 0; x < p->w; x++) {
            for (k = 0; k < 5; k++) {
                int64_t t = (int64_t)x + (int64_t)k - 2;
                size_t sx;
                if (t % 2 != 0) {
                    continue;
                }
                sx = clampz(t / 2, p->sw - 1);
                for (c = 0; c < p->ch; c++) {
                    trow[x * p->ch + c] += 2.0f * K[k] * p->src[(y * p->sw + sx) * p->ch + c];
                }
            }
        }
    }
    return PC_OK;
}

static pc_status expand_v(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const pass_ctx *p = vctx;
    size_t y, x, k, c;
    (void)chunk;
    for (y = begin; y < end; y++) {
        float *orow = p->dst + y * p->w * p->ch;
        for (k = 0; k < 5; k++) {
            int64_t t = (int64_t)y + (int64_t)k - 2;
            size_t sy;
            if (t % 2 != 0) {
                continue;
            }
            sy = clampz(t / 2, p->sh - 1);
            for (x = 0; x < p->w; x++) {
                for (c = 0; c < p->ch; c++) {
                    orow[x * p->ch + c] += 2.0f * K[k] * p->src[(sy * p->w + x) * p->ch + c];
                }
            }
        }
    }
    return PC_OK;
}

pc_status pc_algo_pyramid_expand(const pc_algo_level *l, size_t ch, size_t w, size_t h, float **out,
                                 pc_error *err)
{
    float *tmp = NULL, *o = NULL;
    pass_ctx p;
    pc_status st;
    /* The reference clamps to [0, len - 1], which panics for an empty source. */
    if (w * ch > 0 && ((l->h > 0 && l->w == 0) || (h > 0 && l->h == 0))) {
        return pc_fail(err, PC_ERR_INVALID, "cannot expand an empty %zux%zu level to %zux%zu", l->w, l->h, w,
                       h);
    }
    st = alloc_plane(w, l->h, ch, &tmp, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    st = alloc_plane(w, h, ch, &o, NULL, err);
    if (st != PC_OK) {
        free(tmp);
        return st;
    }
    if (w * ch > 0) {
        p.src = l->px;
        p.dst = tmp;
        p.sw = l->w;
        p.sh = l->h;
        p.w = w;
        p.ch = ch;
        (void)pc_algo_par_for(l->h, grain_for(w * ch), expand_h, &p);
        p.src = tmp;
        p.dst = o;
        (void)pc_algo_par_for(h, grain_for(w * ch), expand_v, &p);
    }
    free(tmp);
    *out = o;
    return PC_OK;
}

/* ---- pyramids ---- */

static pc_status push_level(pc_algo_pyramid *p, size_t *cap, pc_algo_level l, pc_error *err)
{
    pc_algo_level *nl = pc_grow_array(p->levels, cap, p->n + 1, sizeof *nl);
    if (nl == NULL) {
        pc_algo_level_fini(&l);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a pyramid");
    }
    p->levels = nl;
    p->levels[p->n++] = l;
    return PC_OK;
}

pc_status pc_algo_pyramid_gaussian(size_t w, size_t h, size_t ch, const float *px, size_t levels,
                                   pc_algo_pyramid *out, pc_error *err)
{
    pc_algo_pyramid p = {NULL, 0};
    size_t cap = 0, n;
    pc_algo_level l0;
    pc_status st = alloc_plane(w, h, ch, &l0.px, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n > 0) {
        memcpy(l0.px, px, n * sizeof(float));
    }
    l0.w = w;
    l0.h = h;
    st = push_level(&p, &cap, l0, err);
    if (levels < 1) {
        levels = 1;
    }
    while (st == PC_OK && p.n < levels) {
        const pc_algo_level *last = &p.levels[p.n - 1];
        pc_algo_level next;
        if (last->w <= 1 && last->h <= 1) {
            break;
        }
        st = pc_algo_pyramid_reduce(last, ch, &next, err);
        if (st == PC_OK) {
            st = push_level(&p, &cap, next, err);
        }
    }
    if (st != PC_OK) {
        pc_algo_pyramid_fini(&p);
        return st;
    }
    *out = p;
    return PC_OK;
}

pc_status pc_algo_pyramid_laplacian(size_t w, size_t h, size_t ch, const float *px, size_t levels,
                                    pc_algo_pyramid *out, pc_error *err)
{
    pc_algo_pyramid g;
    size_t i, j;
    pc_status st = pc_algo_pyramid_gaussian(w, h, ch, px, levels, &g, err);
    if (st != PC_OK) {
        return st;
    }
    /* Level i becomes g[i] - expand(g[i + 1]), in place (g[i + 1] is still the Gaussian level). */
    for (i = 0; i + 1 < g.n; i++) {
        float *up = NULL;
        size_t n = g.levels[i].w * g.levels[i].h * ch;
        st = pc_algo_pyramid_expand(&g.levels[i + 1], ch, g.levels[i].w, g.levels[i].h, &up, err);
        if (st != PC_OK) {
            pc_algo_pyramid_fini(&g);
            return st;
        }
        for (j = 0; j < n; j++) {
            g.levels[i].px[j] = g.levels[i].px[j] - up[j];
        }
        free(up);
    }
    *out = g;
    return PC_OK;
}

pc_status pc_algo_pyramid_collapse(const pc_algo_pyramid *pyr, size_t ch, float **out, size_t *len,
                                   pc_error *err)
{
    pc_algo_level cur = {0, 0, NULL};
    size_t n, i, j;
    pc_status st;
    if (pyr->n == 0) {
        *out = NULL;
        *len = 0;
        return PC_OK;
    }
    cur.w = pyr->levels[pyr->n - 1].w;
    cur.h = pyr->levels[pyr->n - 1].h;
    st = alloc_plane(cur.w, cur.h, ch, &cur.px, &n, err);
    if (st != PC_OK) {
        return st;
    }
    if (n > 0) {
        memcpy(cur.px, pyr->levels[pyr->n - 1].px, n * sizeof(float));
    }
    for (i = pyr->n - 1; i-- > 0;) {
        const pc_algo_level *l = &pyr->levels[i];
        float *up = NULL;
        st = pc_algo_pyramid_expand(&cur, ch, l->w, l->h, &up, err);
        if (st != PC_OK) {
            pc_algo_level_fini(&cur);
            return st;
        }
        n = l->w * l->h * ch;
        for (j = 0; j < n; j++) {
            up[j] = l->px[j] + up[j];
        }
        pc_algo_level_fini(&cur);
        cur.w = l->w;
        cur.h = l->h;
        cur.px = up;
    }
    *out = cur.px;
    *len = cur.w * cur.h * ch;
    return PC_OK;
}

size_t pc_algo_pyramid_auto_levels(size_t w, size_t h)
{
    size_t n = 1, s = w < h ? w : h;
    while (s > 8 && n < 10) {
        s /= 2;
        n++;
    }
    return n;
}

pc_status pc_algo_pyramid_blend(size_t w, size_t h, size_t ch, const float *const *images,
                                const float *const *weights, size_t count, size_t levels, float **out,
                                pc_error *err)
{
    pc_algo_pyramid acc = {NULL, 0}, wsum = {NULL, 0};
    float *res = NULL;
    size_t k, li, i, c, len;
    pc_status st = PC_OK;
    if (levels < 1) {
        levels = 1;
    }
    for (k = 0; k < count && st == PC_OK; k++) {
        pc_algo_pyramid lp, gp;
        st = pc_algo_pyramid_laplacian(w, h, ch, images[k], levels, &lp, err);
        if (st != PC_OK) {
            break;
        }
        st = pc_algo_pyramid_gaussian(w, h, 1, weights[k], levels, &gp, err);
        if (st != PC_OK) {
            pc_algo_pyramid_fini(&lp);
            break;
        }
        if (acc.levels == NULL) {
            size_t ca = 0, cw = 0;
            for (li = 0; li < lp.n && st == PC_OK; li++) {
                pc_algo_level z = {lp.levels[li].w, lp.levels[li].h, NULL};
                st = alloc_plane(z.w, z.h, ch, &z.px, NULL, err);
                if (st == PC_OK) {
                    st = push_level(&acc, &ca, z, err);
                }
            }
            for (li = 0; li < gp.n && st == PC_OK; li++) {
                pc_algo_level z = {gp.levels[li].w, gp.levels[li].h, NULL};
                st = alloc_plane(z.w, z.h, 1, &z.px, NULL, err);
                if (st == PC_OK) {
                    st = push_level(&wsum, &cw, z, err);
                }
            }
        }
        for (li = 0; li < lp.n && li < gp.n && li < acc.n && li < wsum.n && st == PC_OK; li++) {
            const pc_algo_level *l = &lp.levels[li], *g = &gp.levels[li];
            for (i = 0; i < l->w * l->h; i++) {
                float kk = g->px[i];
                wsum.levels[li].px[i] += kk;
                for (c = 0; c < ch; c++) {
                    acc.levels[li].px[i * ch + c] += kk * l->px[i * ch + c];
                }
            }
        }
        pc_algo_pyramid_fini(&lp);
        pc_algo_pyramid_fini(&gp);
    }
    if (st != PC_OK) {
        goto done;
    }
    if (acc.levels == NULL) {
        st = alloc_plane(w, h, ch, &res, NULL, err);
        goto done;
    }
    for (li = 0; li < acc.n && li < wsum.n; li++) {
        pc_algo_level *l = &acc.levels[li];
        for (i = 0; i < l->w * l->h; i++) {
            float kk = wsum.levels[li].px[i];
            for (c = 0; c < ch; c++) {
                l->px[i * ch + c] = kk > 1e-6f ? l->px[i * ch + c] / kk : 0.0f;
            }
        }
    }
    st = pc_algo_pyramid_collapse(&acc, ch, &res, &len, err);
    if (st == PC_OK) {
        for (i = 0; i < len; i++) {
            res[i] = pc_clampf(res[i], 0.0f, 1.0f);
        }
    }
done:
    pc_algo_pyramid_fini(&acc);
    pc_algo_pyramid_fini(&wsum);
    if (st != PC_OK) {
        free(res);
        return st;
    }
    *out = res;
    return PC_OK;
}

/* ---- weights ---- */

/* Separable running-sum box blur of radius r (f32 sums, edges clamped); w, h > 0. */
static pc_status box_blur1(size_t w, size_t h, const float *v, size_t r, float **out, pc_error *err)
{
    float *tmp, *dst;
    size_t line, k, pass;
    float div = (float)(2 * r + 1);
    pc_status st = alloc_plane(w, h, 1, &tmp, NULL, err);
    if (st != PC_OK) {
        return st;
    }
    st = alloc_plane(w, h, 1, &dst, NULL, err);
    if (st != PC_OK) {
        free(tmp);
        return st;
    }
    for (pass = 0; pass < 2; pass++) {
        bool horizontal = pass == 0;
        const float *src = horizontal ? v : tmp;
        float *d = horizontal ? tmp : dst;
        size_t nl = horizontal ? h : w, m = horizontal ? w : h;
        for (line = 0; line < nl; line++) {
#define AT1(kk) (horizontal ? src[line * w + clampz((kk), m - 1)] : src[clampz((kk), m - 1) * w + line])
            float s = -0.0f; /* Iterator::sum */
            int64_t j, ri = (int64_t)r;
            for (j = -ri; j <= ri; j++) {
                s += AT1(j);
            }
            for (k = 0; k < m; k++) {
                size_t i = horizontal ? line * w + k : k * w + line;
                d[i] = s / div;
                s += AT1((int64_t)k + ri + 1) - AT1((int64_t)k - ri);
            }
#undef AT1
        }
    }
    free(tmp);
    *out = dst;
    return PC_OK;
}

static void free_planes(float **p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        free(p[i]);
        p[i] = NULL;
    }
}

/* One-hot winner maps: weight 1 for the image with the largest score at each pixel. */
static pc_status winner_take_all(float *const *scores, size_t count, size_t n, float **out, pc_error *err)
{
    size_t k, i;
    for (k = 0; k < count; k++) {
        pc_status st = alloc_plane(n, 1, 1, &out[k], NULL, err);
        if (st != PC_OK) {
            free_planes(out, k);
            return st;
        }
    }
    for (i = 0; i < n; i++) {
        size_t best = 0;
        float bv = -3.40282347e+38f;
        for (k = 0; k < count; k++) {
            if (scores[k][i] > bv) {
                best = k;
                bv = scores[k][i];
            }
        }
        if (bv > -3.40282347e+38f) {
            out[best][i] = 1.0f;
        }
    }
    return PC_OK;
}

pc_status pc_algo_stack_weights(size_t w, size_t h, const float *const *luma, const float *const *alpha,
                                size_t count, float **out, pc_error *err)
{
    size_t m = w < h ? w : h, radius = m / 64, k, x, y, n = w * h;
    float **scores;
    pc_status st = PC_OK;
    if (radius < 2) {
        radius = 2;
    }
    if (radius > 12) {
        radius = 12;
    }
    scores = pc_calloc_array(count > 0 ? count : 1, sizeof *scores);
    if (scores == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for stack weights");
    }
    for (k = 0; k < count && st == PC_OK; k++) {
        const float *l = luma[k];
        float *lap = NULL, *bl = NULL;
        st = alloc_plane(w, h, 1, &lap, NULL, err);
        if (st != PC_OK) {
            break;
        }
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t xl = x > 0 ? x - 1 : 0, xr = x + 1 < w ? x + 1 : w - 1;
                size_t yu = y > 0 ? y - 1 : 0, yd = y + 1 < h ? y + 1 : h - 1;
                lap[y * w + x] = fabsf(l[y * w + xl] + l[y * w + xr] + l[yu * w + x] + l[yd * w + x] -
                                       4.0f * l[y * w + x]);
            }
        }
        if (n > 0) {
            st = box_blur1(w, h, lap, radius, &bl, err);
        }
        free(lap);
        if (st != PC_OK) {
            break;
        }
        if (bl == NULL) {
            bl = pc_malloc(1);
            if (bl == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for stack weights");
                break;
            }
        }
        for (x = 0; x < n; x++) {
            bl[x] = alpha[k][x] > 0.5f ? bl[x] : -1.0f;
        }
        scores[k] = bl;
    }
    if (st == PC_OK) {
        st = winner_take_all(scores, count, n, out, err);
    }
    free_planes(scores, count);
    free(scores);
    return st;
}

pc_status pc_algo_panorama_weights(size_t w, size_t h, const float *const *alpha, size_t count, float **out,
                                   pc_error *err)
{
    size_t k, i, n = w * h;
    float **scores;
    pc_status st = PC_OK;
    scores = pc_calloc_array(count > 0 ? count : 1, sizeof *scores);
    if (scores == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for panorama weights");
    }
    for (k = 0; k < count && st == PC_OK; k++) {
        const float *a = alpha[k];
        float *d = NULL;
        int64_t x, y, W = (int64_t)w, H = (int64_t)h;
        st = alloc_plane(w, h, 1, &d, NULL, err);
        if (st != PC_OK) {
            break;
        }
        /* Two-pass 3-4 chamfer distance from transparent pixels / the border. */
        for (i = 0; i < n; i++) {
            d[i] = a[i] > 0.5f ? 1.0e9f : 0.0f;
        }
#define DAT(xx, yy)                                                                                          \
    (((xx) < 0 || (yy) < 0 || (xx) >= W || (yy) >= H) ? 0.0f : d[(size_t)(yy) * w + (size_t)(xx)])
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                size_t j = (size_t)y * w + (size_t)x;
                float m = pc_rs_minf(d[j], DAT(x - 1, y) + 3.0f);
                m = pc_rs_minf(m, DAT(x, y - 1) + 3.0f);
                m = pc_rs_minf(m, DAT(x - 1, y - 1) + 4.0f);
                m = pc_rs_minf(m, DAT(x + 1, y - 1) + 4.0f);
                d[j] = m;
            }
        }
        for (y = H - 1; y >= 0; y--) {
            for (x = W - 1; x >= 0; x--) {
                size_t j = (size_t)y * w + (size_t)x;
                float m = pc_rs_minf(d[j], DAT(x + 1, y) + 3.0f);
                m = pc_rs_minf(m, DAT(x, y + 1) + 3.0f);
                m = pc_rs_minf(m, DAT(x + 1, y + 1) + 4.0f);
                m = pc_rs_minf(m, DAT(x - 1, y + 1) + 4.0f);
                d[j] = m;
            }
        }
#undef DAT
        for (i = 0; i < n; i++) {
            d[i] = d[i] <= 0.0f ? -1.0f : d[i];
        }
        scores[k] = d;
    }
    if (st == PC_OK) {
        st = winner_take_all(scores, count, n, out, err);
    }
    free_planes(scores, count);
    free(scores);
    return st;
}
