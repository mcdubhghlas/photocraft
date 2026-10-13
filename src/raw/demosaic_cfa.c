/* Demosaicing for any periodic CFA (Fujifilm X-Trans' 6x6 pattern), a port of demosaic_cfa in
 * crates/raw/src/demosaic.rs: a pattern-agnostic edge-directed interpolation.
 *
 * 1. Green at non-green sites: per axis (horizontal, vertical), the mean of the green neighbours on
 *    that axis, weighted by the inverse square of the local gradient along it (mean absolute
 *    difference of same-colour sample pairs on the axis within a 5x5 window).
 * 2. Red and blue everywhere from colour differences: the green estimate plus the weighted mean of
 *    (sample - green) at the same-colour sites of the 5x5 window, weighted by inverse squared
 *    distance and by green similarity.
 *
 * The pattern breaks under mirror padding when its period is not 2, so this reads only interior
 * samples and drops taps outside the image. Taps are listed in the reference's order so every
 * float sum adds up in the same order. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "raw/internal.h"

#define RADIUS 2
#define WIN ((2 * RADIUS + 1) * (2 * RADIUS + 1))
/* Regularizes the inverse-gradient weights (about 1/256 of full scale). */
#define GRAD_EPS (1.0f / 256.0f)
/* Regularizes the green-similarity weights of the colour differences. */
#define DIFF_EPS (1.0f / 64.0f)

void rx_pattern_of(const pc_raw_cfa *cfa, size_t x0, size_t y0, rx_pattern *out)
{
    size_t w = cfa->width < 1 ? 1 : cfa->width > 16 ? 16 : cfa->width;
    size_t h = cfa->height < 1 ? 1 : cfa->height > 16 ? 16 : cfa->height;
    size_t i;
    out->w = w;
    out->h = h;
    out->n = w * h;
    for (i = 0; i < w * h; i++) {
        uint8_t c = pc_raw_cfa_color(cfa, x0 + i % w, y0 + i / w);
        out->colors[i] = c < 2 ? c : 2;
    }
}

uint8_t rx_pattern_at(const rx_pattern *p, ptrdiff_t x, ptrdiff_t y)
{
    ptrdiff_t w = (ptrdiff_t)(p->w > 0 ? p->w : 1);
    ptrdiff_t h = (ptrdiff_t)(p->h > 0 ? p->h : 1);
    ptrdiff_t xm = ((x % w) + w) % w;
    ptrdiff_t ym = ((y % h) + h) % h;
    size_t i = (size_t)(ym * w + xm);
    return i < p->n ? p->colors[i] : 1;
}

typedef struct off {
    signed char dx, dy;
} off;

typedef struct pairtap {
    off a, b;
    float k;
} pairtap;

typedef struct axis {
    off near[2];
    size_t nnear;
    pairtap pairs[2 * WIN];
    size_t npairs;
} axis;

typedef struct taps {
    uint8_t own;
    axis axes[2];
    off greens[WIN];
    size_t ngreens;
    off sites[3][WIN];
    float site_w[3][WIN];
    size_t nsites[3];
} taps;

static void phase_taps(const rx_pattern *pat, ptrdiff_t px, ptrdiff_t py, taps *t)
{
    off window[WIN];
    size_t nw = 0;
    int dy;
    int dx;
    int a;
    size_t i;
    int c;
#define COL(ddx, ddy) rx_pattern_at(pat, px + (ddx), py + (ddy))
    for (dy = -RADIUS; dy <= RADIUS; dy++) {
        for (dx = -RADIUS; dx <= RADIUS; dx++) {
            window[nw].dx = (signed char)dx;
            window[nw].dy = (signed char)dy;
            nw++;
        }
    }
    for (a = 0; a < 2; a++) {
        int ax = a == 0 ? 1 : 0;
        int ay = a == 0 ? 0 : 1;
        axis *x = &t->axes[a];
        int s;
        x->nnear = 0;
        x->npairs = 0;
        for (s = 0; s < 2; s++) {
            int ddx = s == 0 ? ax : -ax;
            int ddy = s == 0 ? ay : -ay;
            if (COL(ddx, ddy) == 1) {
                x->near[x->nnear].dx = (signed char)ddx;
                x->near[x->nnear].dy = (signed char)ddy;
                x->nnear++;
            }
        }
        for (i = 0; i < nw; i++) {
            int wx = window[i].dx;
            int wy = window[i].dy;
            int k;
            /* Pairs on the axis line or the lines next to it. */
            if ((ax == 1 && abs(wy) > 1) || (ay == 1 && abs(wx) > 1)) {
                continue;
            }
            for (k = 1; k <= 2; k++) {
                int bx = wx + k * ax;
                int by = wy + k * ay;
                if (abs(bx) <= RADIUS && abs(by) <= RADIUS && COL(wx, wy) == COL(bx, by)) {
                    pairtap *p = &x->pairs[x->npairs++];
                    p->a = window[i];
                    p->b.dx = (signed char)bx;
                    p->b.dy = (signed char)by;
                    p->k = 1.0f / (float)k;
                }
            }
        }
    }
    /* Greens of the 3x3 window, or of the 5x5 one when the 3x3 has none. */
    t->ngreens = 0;
    for (i = 0; i < nw; i++) {
        int wx = window[i].dx;
        int wy = window[i].dy;
        if (abs(wx) <= 1 && abs(wy) <= 1 && (wx != 0 || wy != 0) && COL(wx, wy) == 1) {
            t->greens[t->ngreens++] = window[i];
        }
    }
    if (t->ngreens == 0) {
        for (i = 0; i < nw; i++) {
            int wx = window[i].dx;
            int wy = window[i].dy;
            if ((wx != 0 || wy != 0) && COL(wx, wy) == 1) {
                t->greens[t->ngreens++] = window[i];
            }
        }
    }
    for (c = 0; c < 3; c++) {
        t->nsites[c] = 0;
        for (i = 0; i < nw; i++) {
            int wx = window[i].dx;
            int wy = window[i].dy;
            if ((wx != 0 || wy != 0) && COL(wx, wy) == c) {
                t->sites[c][t->nsites[c]] = window[i];
                t->site_w[c][t->nsites[c]] = 1.0f / (float)(wx * wx + wy * wy);
                t->nsites[c]++;
            }
        }
    }
    t->own = COL(0, 0);
#undef COL
}

typedef struct dctx {
    const rx_padded *p;
    const rx_pattern *pat;
    const taps *taps;
    float *green;
    float *out;
    size_t band;
} dctx;

/* Sample of plane (w x h, stride, pad) at (x + o.dx, y + o.dy), or false outside the image. */
static bool get(const float *plane, size_t stride, size_t pad, size_t w, size_t h, size_t x, size_t y, off o,
                float *v)
{
    ptrdiff_t xx = (ptrdiff_t)x + o.dx;
    ptrdiff_t yy = (ptrdiff_t)y + o.dy;
    if (xx < 0 || yy < 0 || (size_t)xx >= w || (size_t)yy >= h) {
        return false;
    }
    *v = plane[((size_t)yy + pad) * stride + pad + (size_t)xx];
    return true;
}

static const taps *tap_at(const dctx *c, size_t x, size_t y)
{
    return &c->taps[(y % c->pat->h) * c->pat->w + x % c->pat->w];
}

static pc_status green_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const dctx *c = ctx;
    const rx_padded *p = c->p;
    size_t w = p->w;
    size_t h = p->h;
    size_t y;
    static const off ZERO = {0, 0};
    (void)chunk;
    for (y = begin; y < end; y++) {
        size_t x;
        for (x = 0; x < w; x++) {
            const taps *t = tap_at(c, x, y);
            float v = 0.0f;
            float num = 0.0f;
            float den = 0.0f;
            int a;
            float *g = &c->green[y * w + x];
            (void)get(p->data, p->stride, RX_PAD, w, h, x, y, ZERO, &v);
            if (t->own == 1) {
                *g = v;
                continue;
            }
            for (a = 0; a < 2; a++) {
                const axis *ax = &t->axes[a];
                float sum = 0.0f;
                uint32_t n = 0;
                float grad = 0.0f;
                float m = 0.0f;
                float wt;
                size_t i;
                for (i = 0; i < ax->nnear; i++) {
                    float q;
                    if (get(p->data, p->stride, RX_PAD, w, h, x, y, ax->near[i], &q)) {
                        sum += q;
                        n++;
                    }
                }
                if (n == 0) {
                    continue;
                }
                for (i = 0; i < ax->npairs; i++) {
                    float va;
                    float vb;
                    if (get(p->data, p->stride, RX_PAD, w, h, x, y, ax->pairs[i].a, &va) &&
                        get(p->data, p->stride, RX_PAD, w, h, x, y, ax->pairs[i].b, &vb)) {
                        grad += fabsf(va - vb) * ax->pairs[i].k;
                        m += 1.0f;
                    }
                }
                grad = m > 0.0f ? grad / m : 0.0f;
                wt = 1.0f / ((GRAD_EPS + grad) * (GRAD_EPS + grad));
                num += wt * sum / (float)n;
                den += wt;
            }
            if (den > 0.0f) {
                *g = num / den;
            } else {
                float sum = 0.0f;
                uint32_t n = 0;
                size_t i;
                for (i = 0; i < t->ngreens; i++) {
                    float q;
                    if (get(p->data, p->stride, RX_PAD, w, h, x, y, t->greens[i], &q)) {
                        sum += q;
                        n++;
                    }
                }
                *g = n > 0 ? sum / (float)n : v;
            }
        }
    }
    return PC_OK;
}

static pc_status color_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const dctx *c = ctx;
    const rx_padded *p = c->p;
    size_t w = p->w;
    size_t h = p->h;
    size_t y;
    static const off ZERO = {0, 0};
    (void)chunk;
    for (y = begin; y < end; y++) {
        size_t x;
        for (x = 0; x < w; x++) {
            const taps *t = tap_at(c, x, y);
            float g = c->green[y * w + x];
            float v = 0.0f;
            float *o = c->out + (y * w + x) * 3;
            int cc;
            int k;
            (void)get(p->data, p->stride, RX_PAD, w, h, x, y, ZERO, &v);
            o[1] = g;
            for (cc = 0; cc <= 2; cc += 2) {
                float num = 0.0f;
                float den = 0.0f;
                size_t i;
                if ((int)t->own == cc) {
                    o[cc] = v;
                    continue;
                }
                for (i = 0; i < t->nsites[cc]; i++) {
                    float vq;
                    float gq;
                    if (get(p->data, p->stride, RX_PAD, w, h, x, y, t->sites[cc][i], &vq) &&
                        get(c->green, w, 0, w, h, x, y, t->sites[cc][i], &gq)) {
                        float wt = t->site_w[cc][i] / (DIFF_EPS + fabsf(gq - g));
                        num += wt * (vq - gq);
                        den += wt;
                    }
                }
                o[cc] = den > 0.0f ? g + num / den : g;
            }
            for (k = 0; k < 3; k++) {
                o[k] = pc_clampf(o[k], 0.0f, 1.0f);
            }
        }
    }
    return PC_OK;
}

pc_status rx_demosaic_cfa(const rx_padded *p, const rx_pattern *pat, float **out, pc_error *err)
{
    size_t w = p->w;
    size_t h = p->h;
    size_t n3;
    taps *tp = NULL;
    dctx c;
    size_t i;
    if (!pc_size_mul3(w, h, 3, &n3)) {
        return rx_nomem(err);
    }
    c.out = pc_calloc_array(n3 > 0 ? n3 : 1, sizeof *c.out);
    if (c.out == NULL) {
        return rx_nomem(err);
    }
    if (w == 0 || h == 0 || pat->w == 0 || pat->h == 0 || pat->n < pat->w * pat->h || p->data == NULL) {
        *out = c.out;
        return PC_OK;
    }
    tp = pc_malloc_array(pat->w * pat->h, sizeof *tp);
    c.green = pc_calloc_array(w * h, sizeof *c.green);
    if (tp == NULL || c.green == NULL) {
        free(tp);
        free(c.green);
        free(c.out);
        return rx_nomem(err);
    }
    for (i = 0; i < pat->w * pat->h; i++) {
        phase_taps(pat, (ptrdiff_t)(i % pat->w), (ptrdiff_t)(i / pat->w), &tp[i]);
    }
    c.p = p;
    c.pat = pat;
    c.taps = tp;
    c.band = rx_band_rows(w);
    (void)pc_pool_for(pc_pool_global(), h, c.band, green_rows, &c);
    (void)pc_pool_for(pc_pool_global(), h, c.band, color_rows, &c);
    free(tp);
    free(c.green);
    *out = c.out;
    return PC_OK;
}
