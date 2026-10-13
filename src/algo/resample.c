/* Separable resampling of surfaces (port of crates/algo/src/resample.rs). */
#include "algo/resample.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/filter.h"
#include "algo/fxutil.h"
#include "algo/image.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "base/rmath.h"

/* Most taps one output sample may span before folding (the reference allocates them all). */
#define MAX_TAPS ((int64_t)1 << 24)
/* Largest output side (Photoshop's limit is 300,000 px). */
#define MAX_DIM ((uint32_t)1 << 22)
#define BAND 64

/* Sub-pixel positions Photoshop's filters resolve: a sample's offset is rounded to 1/128 pixel. */
#define PHASES 128.0

/* Photoshop's bicubic filters, measured on Photoshop 25.4: Bicubic a = -0.75; Smoother stretched
 * by 1.15, a = -0.625 (- 0.5 (1 - s) reducing); Sharper stretched by 1.05, a = -1 (- 1.6 (1 - s)
 * reducing). Reducing widens the filter by 1/s, down to 1/4; below that the taps first average the
 * image down to four times the target. */
pc_algo_rs_kernel pc_algo_resample_keys(pc_algo_resample method, double s)
{
    pc_algo_rs_kernel k;
    double a, stretch;
    s = pc_clampd(s, 0.25, 1.0);
    switch (method) {
    case PC_ALGO_RESAMPLE_BICUBIC_SMOOTHER:
        a = -0.625 - 0.5 * (1.0 - s), stretch = 1.15;
        break;
    case PC_ALGO_RESAMPLE_BICUBIC_SHARPER:
        a = -1.0 - 1.6 * (1.0 - s), stretch = 1.05;
        break;
    default:
        a = -0.75, stretch = 1.0;
        break;
    }
    k.kind = PC_ALGO_KERNEL_KEYS;
    k.a = a;
    k.stretch = stretch / s;
    return k;
}

static pc_algo_rs_kernel kernel_of(pc_algo_rs_kernel_kind kind)
{
    pc_algo_rs_kernel k;
    k.kind = kind;
    k.a = 0.0;
    k.stretch = 1.0;
    return k;
}

static double keys_weight(double x, double a)
{
    x = fabs(x);
    if (x < 1.0) {
        return (a + 2.0) * x * x * x - (a + 3.0) * x * x + 1.0;
    }
    if (x < 2.0) {
        return a * (x * x * x - 5.0 * x * x + 8.0 * x - 4.0);
    }
    return 0.0;
}

static double lanczos_weight(double x)
{
    x = fabs(x);
    if (x < 1e-9) {
        return 1.0;
    }
    if (x < 3.0) {
        double px = 3.14159265358979323846 * x;
        return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
    }
    return 0.0;
}

/* One resampling pass: scale factors and the filter of each axis. */
typedef struct pass {
    double sx, sy;
    pc_algo_rs_kernel kx, ky;
} pass;

static pass mkpass(double sx, double sy, pc_algo_rs_kernel kx, pc_algo_rs_kernel ky)
{
    pass p;
    p.sx = sx, p.sy = sy, p.kx = kx, p.ky = ky;
    return p;
}

/* Photoshop's passes for `filter` (plan in resample.rs): a bicubic filter is designed for the
 * height's scale (the width's when the height stays) and serves both axes, the smaller scale
 * first; Bicubic Automatic with one axis growing and one shrinking is two Image Sizes (Sharper on
 * the shrinking axis, then Smoother); Nearest, Bilinear and Lanczos run both axes in one pass. */
static size_t plan(pc_algo_resample filter, double sx, double sy, pass out[2])
{
    pc_algo_rs_kernel k, nn = kernel_of(PC_ALGO_KERNEL_NEAREST);
    switch (filter) {
    case PC_ALGO_RESAMPLE_NEAREST:
        out[0] = mkpass(sx, sy, nn, nn);
        return 1;
    case PC_ALGO_RESAMPLE_BILINEAR:
        k = kernel_of(PC_ALGO_KERNEL_BILINEAR);
        out[0] = mkpass(sx, sy, k, k);
        return 1;
    case PC_ALGO_RESAMPLE_LANCZOS:
        k = kernel_of(PC_ALGO_KERNEL_LANCZOS);
        out[0] = mkpass(sx, sy, k, k);
        return 1;
    case PC_ALGO_RESAMPLE_PRESERVE_DETAILS:
        if (sx > 1.0 || sy > 1.0) {
            return plan(PC_ALGO_RESAMPLE_LANCZOS, sx, sy, out);
        }
        return plan(PC_ALGO_RESAMPLE_BICUBIC_SHARPER, sx, sy, out);
    case PC_ALGO_RESAMPLE_BICUBIC_AUTOMATIC:
        if (sx <= 1.0 && sy <= 1.0) {
            return plan(PC_ALGO_RESAMPLE_BICUBIC_SHARPER, sx, sy, out);
        }
        if (sx >= 1.0 && sy >= 1.0) {
            return plan(PC_ALGO_RESAMPLE_BICUBIC_SMOOTHER, sx, sy, out);
        }
        if (sx < 1.0) {
            out[0] = mkpass(sx, 1.0, pc_algo_resample_keys(PC_ALGO_RESAMPLE_BICUBIC_SHARPER, sx), nn);
            out[1] = mkpass(1.0, sy, nn, pc_algo_resample_keys(PC_ALGO_RESAMPLE_BICUBIC_SMOOTHER, sy));
        } else {
            out[0] = mkpass(1.0, sy, nn, pc_algo_resample_keys(PC_ALGO_RESAMPLE_BICUBIC_SHARPER, sy));
            out[1] = mkpass(sx, 1.0, pc_algo_resample_keys(PC_ALGO_RESAMPLE_BICUBIC_SMOOTHER, sx), nn);
        }
        return 2;
    case PC_ALGO_RESAMPLE_BICUBIC:
    case PC_ALGO_RESAMPLE_BICUBIC_SMOOTHER:
    case PC_ALGO_RESAMPLE_BICUBIC_SHARPER:
    default:
        k = pc_algo_resample_keys(filter, sy != 1.0 ? sy : sx);
        if (sx == 1.0 || sy == 1.0) {
            out[0] = mkpass(sx, sy, k, k);
            return 1;
        }
        if (sy < sx) {
            out[0] = mkpass(1.0, sy, k, k);
            out[1] = mkpass(sx, 1.0, k, k);
        } else {
            out[0] = mkpass(sx, 1.0, k, k);
            out[1] = mkpass(1.0, sy, k, k);
        }
        return 2;
    }
}

/* The source position of output sample o, (o + 0.5) / scale - 0.5, kept on the edge samples when
 * there is an edge, and rounded to Photoshop's 1/128 pixel. */
static double position(int32_t o, double scale, const int32_t *edge)
{
    double u = ((double)o + 0.5) / scale - 0.5, j;
    if (edge != NULL) {
        u = pc_clampd(u, (double)edge[0], (double)(edge[1] > edge[0] ? edge[1] : edge[0]));
    }
    j = floor(u);
    return j + round((u - j) * PHASES) / PHASES;
}

/* The source pixels [floor(o / scale), floor((o + 1) / scale)) output pixel o covers. */
static void cover(int32_t o, double scale, int32_t *a, int32_t *b)
{
    *a = pc_f64_to_i32(floor((double)o / scale + 1e-9));
    *b = pc_f64_to_i32(pc_rs_max(floor(((double)o + 1.0) / scale + 1e-9), (double)*a + 1.0));
}

/* A growable weight list. */
typedef struct wbuf {
    double *v;
    size_t n, cap;
} wbuf;

static bool wb_resize(wbuf *w, int64_t n)
{
    size_t want;
    if (n < 0) {
        n = 0;
    }
    if (n > MAX_TAPS) {
        return false;
    }
    want = n > 0 ? (size_t)n : 1; /* never NULL, even for no taps */
    if (want > w->cap || w->v == NULL) {
        double *nv = realloc(w->v, want * sizeof *nv);
        if (nv == NULL) {
            return false;
        }
        w->v = nv;
        w->cap = want;
    }
    w->n = (size_t)n;
    return true;
}

/* Unclamped weights of output sample o over source samples *lo .. *lo + w->n. LIMIT when one
 * sample would need more than MAX_TAPS taps (NOMEM when they can't be allocated). */
static pc_status weights(int32_t o, double scale, pc_algo_rs_kernel kernel, const int32_t *edge, int32_t *lo,
                         wbuf *w, wbuf *tmp, int depth)
{
    size_t i;
    switch (kernel.kind) {
    case PC_ALGO_KERNEL_NEAREST: {
        double u = ((double)o + 0.5) / scale - 0.5;
        *lo = pc_f64_to_i32(floor(u + 0.5));
        if (!wb_resize(w, 1)) {
            return PC_ERR_NOMEM;
        }
        w->v[0] = 1.0;
        return PC_OK;
    }
    case PC_ALGO_KERNEL_BILINEAR:
        if (scale < 0.5) {
            int32_t a, b;
            cover(o, scale, &a, &b);
            if ((int64_t)b - a > MAX_TAPS) {
                return PC_ERR_LIMIT;
            }
            if (!wb_resize(w, (int64_t)b - a)) {
                return PC_ERR_NOMEM;
            }
            for (i = 0; i < w->n; i++) {
                w->v[i] = 1.0 / (double)((int64_t)b - a);
            }
            *lo = a;
        } else {
            double u = position(o, scale, edge), j = floor(u);
            *lo = pc_f64_to_i32(j);
            if (!wb_resize(w, 2)) {
                return PC_ERR_NOMEM;
            }
            w->v[0] = 1.0 - (u - j);
            w->v[1] = u - j;
        }
        return PC_OK;
    case PC_ALGO_KERNEL_KEYS:
        if (scale < 0.25 && depth == 0 && tmp != NULL) {
            /* Averaged down to four times the target first (pixels of `inner`), then filtered. */
            double inner = 4.0 * scale;
            int32_t ie[2], k0, blo = 0, bhi = 0, k;
            const int32_t *iedge = NULL;
            size_t nk;
            bool any = false;
            pc_status st;
            if (edge != NULL) {
                ie[0] = pc_f64_to_i32(ceil((double)edge[0] * inner - 1e-9));
                ie[1] = pc_f64_to_i32(ceil(((double)edge[1] + 1.0) * inner - 1e-9)) - 1;
                iedge = ie;
            }
            st = weights(o, 0.25, kernel, iedge, &k0, tmp, NULL, depth + 1);
            if (st != PC_OK) {
                return st;
            }
            nk = tmp->n;
            for (i = 0; i < nk; i++) {
                int32_t a, b;
                k = (int32_t)((int64_t)k0 + (int64_t)i);
                if (iedge != NULL) {
                    k = pc_clamp_i32(k, iedge[0], iedge[1] > iedge[0] ? iedge[1] : iedge[0]);
                }
                cover(k, inner, &a, &b);
                if (!any || a < blo) {
                    blo = a;
                }
                if (!any || b > bhi) {
                    bhi = b;
                }
                any = true;
            }
            if (!any) {
                blo = 0, bhi = 0;
            }
            if ((int64_t)bhi - blo > MAX_TAPS) {
                return PC_ERR_LIMIT;
            }
            if (!wb_resize(w, (int64_t)bhi - blo)) {
                return PC_ERR_NOMEM;
            }
            memset(w->v, 0, w->n * sizeof *w->v);
            for (i = 0; i < nk; i++) {
                int32_t a, b, j;
                k = (int32_t)((int64_t)k0 + (int64_t)i);
                if (iedge != NULL) {
                    k = pc_clamp_i32(k, iedge[0], iedge[1] > iedge[0] ? iedge[1] : iedge[0]);
                }
                cover(k, inner, &a, &b);
                for (j = a; j < b; j++) {
                    w->v[j - blo] += tmp->v[i] / (double)((int64_t)b - a);
                }
            }
            *lo = blo;
        } else {
            double u = position(o, scale, edge), sum = -0.0;
            int64_t l = pc_f64_to_i32(floor(u - 2.0 * kernel.stretch));
            int64_t h = pc_f64_to_i32(ceil(u + 2.0 * kernel.stretch)), j;
            if (h - l + 1 > MAX_TAPS) {
                return PC_ERR_LIMIT;
            }
            if (!wb_resize(w, h - l + 1)) {
                return PC_ERR_NOMEM;
            }
            for (i = 0; i < w->n; i++) {
                j = l + (int64_t)i;
                w->v[i] = keys_weight((u - (double)j) / kernel.stretch, kernel.a);
                sum += w->v[i];
            }
            for (i = 0; i < w->n; i++) {
                w->v[i] = w->v[i] / sum;
            }
            *lo = (int32_t)l;
        }
        return PC_OK;
    case PC_ALGO_KERNEL_LANCZOS:
    default: {
        double stretch = scale < 1.0 ? 1.0 / scale : 1.0, sum = -0.0, kk;
        double u = ((double)o + 0.5) / scale - 0.5;
        int64_t l, h, j;
        if (edge != NULL) {
            u = pc_clampd(u, (double)edge[0], (double)(edge[1] > edge[0] ? edge[1] : edge[0]));
        }
        l = pc_f64_to_i32(ceil(u - 3.0 * stretch));
        h = pc_f64_to_i32(floor(u + 3.0 * stretch));
        if (h - l + 1 > MAX_TAPS) {
            return PC_ERR_LIMIT;
        }
        if (!wb_resize(w, h - l + 1)) {
            return PC_ERR_NOMEM;
        }
        for (i = 0; i < w->n; i++) {
            j = l + (int64_t)i;
            w->v[i] = lanczos_weight(((double)j - u) / stretch);
            sum += w->v[i];
        }
        kk = fabs(sum) > 1e-12 ? 1.0 / sum : 1.0;
        for (i = 0; i < w->n; i++) {
            w->v[i] = w->v[i] * kk;
        }
        *lo = (int32_t)l;
        return PC_OK;
    }
    }
}

pc_rect pc_algo_scaled_rect(pc_rect r, double sx, double sy)
{
    if (pc_rect_is_empty(r)) {
        return PC_RECT_EMPTY;
    }
    return pc_rect_make(pc_f64_to_i32(floor((double)r.x0 * sx)), pc_f64_to_i32(floor((double)r.y0 * sy)),
                        pc_f64_to_i32(ceil((double)r.x1 * sx)), pc_f64_to_i32(ceil((double)r.y1 * sy)));
}

void pc_algo_taps_fini(pc_algo_taps *t)
{
    if (t == NULL) {
        return;
    }
    free(t->start);
    free(t->off);
    free(t->len);
    free(t->outside);
    free(t->w);
    memset(t, 0, sizeof *t);
}

static int32_t clamp_edge(const int32_t *edge, int32_t i)
{
    return edge != NULL ? pc_clamp_i32(i, edge[0], edge[1]) : i;
}

pc_status pc_algo_taps_make(int32_t dst0, int32_t dst1, double scale, pc_algo_rs_kernel kernel,
                            const int32_t *edge, const int32_t content[2], pc_algo_taps *out, pc_error *err)
{
    pc_algo_taps t;
    size_t n = dst1 > dst0 ? (size_t)((int64_t)dst1 - dst0) : 0, k, wcap = 0, wlen = 0;
    wbuf wd = {NULL, 0, 0}, tmp = {NULL, 0, 0};
    float *folded = NULL;
    size_t fcap = 0;
    pc_status st = PC_OK;
    memset(&t, 0, sizeof t);
    if (scale == 1.0) {
        kernel = kernel_of(PC_ALGO_KERNEL_NEAREST);
    }
    if (n > 0) {
        t.start = pc_malloc_array(n, sizeof *t.start);
        t.off = pc_malloc_array(n, sizeof *t.off);
        t.len = pc_malloc_array(n, sizeof *t.len);
        t.outside = pc_malloc_array(n, sizeof *t.outside);
        if (t.start == NULL || t.off == NULL || t.len == NULL || t.outside == NULL) {
            pc_algo_taps_fini(&t);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for resampling taps");
        }
    }
    t.n = n;
    for (k = 0; k < n; k++) {
        int32_t o = (int32_t)((int64_t)dst0 + (int64_t)k), lo, start, a, b;
        int64_t hi, flen, i;
        float outside;
        size_t i0, i1;
        t.off[k] = wlen;
        st = weights(o, scale, kernel, edge, &lo, &wd, &tmp, 0);
        if (st != PC_OK) {
            st = st == PC_ERR_LIMIT
                     ? pc_fail(err, PC_ERR_LIMIT, "resampling scale %g needs too many taps", scale)
                     : pc_fail(err, PC_ERR_NOMEM, "out of memory for resampling taps");
            goto fail;
        }
        hi = (int64_t)lo + (int64_t)wd.n - 1;
        /* Fold the weights of taps beyond the edge onto the edge sample. */
        start = clamp_edge(edge, lo);
        flen =
            (int64_t)clamp_edge(edge, (int32_t)pc_clamp_i64(hi, INT32_MIN, INT32_MAX)) - (int64_t)start + 1;
        if (flen < 1) {
            flen = 1;
        }
        if (flen > MAX_TAPS) {
            st = pc_fail(err, PC_ERR_LIMIT, "resampling scale %g needs too many taps", scale);
            goto fail;
        }
        if ((size_t)flen > fcap) {
            float *nf = realloc(folded, (size_t)flen * sizeof *folded);
            if (nf == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for resampling taps");
                goto fail;
            }
            folded = nf;
            fcap = (size_t)flen;
        }
        for (i = 0; i < flen; i++) {
            folded[i] = 0.0f;
        }
        for (i = 0; i < (int64_t)wd.n; i++) {
            int64_t slot =
                (int64_t)clamp_edge(edge, (int32_t)pc_clamp_i64((int64_t)lo + i, INT32_MIN, INT32_MAX)) -
                start;
            if (slot >= 0 && slot < flen) {
                folded[slot] += (float)wd.v[i];
            }
        }
        /* Keep the part inside the content; the rest reads the default pixel. */
        a = start > content[0] ? start : content[0];
        b = (int32_t)((int64_t)start + flen - 1 < content[1] ? (int64_t)start + flen - 1 : content[1]);
        if (a > b) {
            outside = -0.0f;
            for (i = 0; i < flen; i++) {
                outside += folded[i];
            }
            t.start[k] = content[0];
            t.len[k] = 0;
            t.outside[k] = outside;
            continue;
        }
        i0 = (size_t)((int64_t)a - start);
        i1 = (size_t)((int64_t)b - start + 1);
        /* folded[..i0] then folded[i1..], in index order. */
        outside = -0.0f;
        for (i = 0; i < flen; i++) {
            if (i < (int64_t)i0 || i >= (int64_t)i1) {
                outside += folded[i];
            }
        }
        {
            float *nw = pc_grow_array(t.w, &wcap, wlen + (i1 - i0), sizeof(float));
            if (nw == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory for resampling taps");
                goto fail;
            }
            t.w = nw;
        }
        memcpy(t.w + wlen, folded + i0, (i1 - i0) * sizeof(float));
        wlen += i1 - i0;
        t.start[k] = a;
        t.len[k] = i1 - i0;
        t.outside[k] = outside;
    }
    free(wd.v), free(tmp.v), free(folded);
    *out = t;
    return PC_OK;
fail:
    free(wd.v), free(tmp.v), free(folded);
    pc_algo_taps_fini(&t);
    return st;
}

void pc_algo_resample_horizontal(const float *row, size_t row_len, float *out, const pc_algo_taps *t,
                                 int32_t x_lo, const float *def, size_t n)
{
    size_t k, j, c;
    float acc[PC_ALGO_MAXC];
    if (n > PC_ALGO_MAXC) {
        return;
    }
    for (k = 0; k < t->n; k++) {
        const float *w = t->w + t->off[k];
        float *d = out + k * n;
        for (c = 0; c < n; c++) {
            acc[c] = 0.0f;
        }
        for (j = 0; j < t->len[k]; j++) {
            int64_t p = (int64_t)t->start[k] + (int64_t)j - x_lo;
            if (p < 0 || (uint64_t)(p + 1) * n > row_len) {
                continue;
            }
            for (c = 0; c < n; c++) {
                acc[c] += row[(size_t)p * n + c] * w[j];
            }
        }
        for (c = 0; c < n; c++) {
            d[c] = acc[c] + def[c] * t->outside[k];
        }
    }
}

/* ---- the resize ---- */

typedef struct rs_ctx {
    const pc_surface *s;
    const pc_algo_taps *hx, *vy;
    const float *def, *def_row;
    pc_rect area, dst;
    int32_t x_lo, x_hi, rows_per_read;
    size_t n, dw;
    bool alpha;
    size_t first_band, nbands;
    float **res;    /* per band of the group */
    pc_error *errs; /* per band of the group */
} rs_ctx;

/* Adds source row y (after the horizontal pass) into every output row of the band using it. */
static void add_row(const rs_ctx *r, size_t b0, size_t b1, float *res, int32_t y, const float *row)
{
    size_t k, i, m = r->dw * r->n;
    for (k = b0; k < b1; k++) {
        int32_t s0 = r->vy->start[k];
        int64_t off = (int64_t)y - s0;
        float w, *d = res + (k - b0) * m;
        if (y < s0 || off >= (int64_t)r->vy->len[k]) {
            continue;
        }
        w = r->vy->w[r->vy->off[k] + (size_t)off];
        for (i = 0; i < m; i++) {
            d[i] += row[i] * w;
        }
    }
}

static pc_status run_band(const rs_ctx *r, size_t band, float **out, pc_error *err)
{
    size_t b0 = band * BAND, b1 = b0 + BAND < r->vy->n ? b0 + BAND : r->vy->n, k, i, m = r->dw * r->n;
    int32_t y_lo = r->area.y0, y_hi = r->area.y0, y;
    bool any_lo = false, any_hi = false;
    float *res = NULL, *rows = NULL, *hrows = NULL;
    size_t rows_cap = 0, hrows_cap = 0, nres;
    pc_status st = PC_OK;
    for (k = b0; k < b1; k++) {
        int32_t end = (int32_t)((int64_t)r->vy->start[k] + (int64_t)r->vy->len[k]);
        if (r->vy->len[k] > 0 && (!any_lo || r->vy->start[k] < y_lo)) {
            y_lo = r->vy->start[k];
            any_lo = true;
        }
        if (!any_hi || end > y_hi) {
            y_hi = end;
            any_hi = true;
        }
    }
    if (y_hi < y_lo) {
        y_hi = y_lo;
    }
    if (!pc_size_mul(b1 - b0, m, &nres)) {
        return pc_fail(err, PC_ERR_LIMIT, "resampled band is too large");
    }
    res = pc_calloc_array(nres > 0 ? nres : 1, sizeof(float));
    if (res == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory resampling");
    }
    /* Source rows stream through in bounded reads, ascending, straight into the output rows that
     * use them (#713). */
    y = y_lo;
    while (y < y_hi) {
        int32_t y1 = (int32_t)((((int64_t)y >= 0 ? (int64_t)y / PC_TILE_SIZE
                                                 : -((-(int64_t)y + PC_TILE_SIZE - 1) / PC_TILE_SIZE)) +
                                1) *
                               PC_TILE_SIZE);
        if (y1 > y_hi) {
            y1 = y_hi;
        }
        if (pc_surface_has_tiles_in(r->s, pc_rect_make(r->x_lo, y, r->x_hi, y1))) {
            int32_t c0 = y;
            while (c0 < y1) {
                int32_t c1 = c0 + r->rows_per_read < y1 ? c0 + r->rows_per_read : y1;
                pc_rect read = pc_rect_make(r->x_lo, c0, r->x_hi, c1);
                size_t rn, hn, rw = (size_t)pc_rect_width(read) * r->n, nrows = (size_t)((int64_t)c1 - c0);
                if (!pc_algo_float_count(read, r->n, &rn) || !pc_size_mul(nrows, m, &hn)) {
                    st = pc_fail(err, PC_ERR_LIMIT, "resampling read is too large");
                    goto done;
                }
                if (rn > rows_cap) {
                    float *nr = realloc(rows, rn * sizeof(float));
                    if (nr == NULL) {
                        st = pc_fail(err, PC_ERR_NOMEM, "out of memory resampling");
                        goto done;
                    }
                    rows = nr;
                    rows_cap = rn;
                }
                if (hn > hrows_cap) {
                    float *nh = realloc(hrows, hn * sizeof(float));
                    if (nh == NULL) {
                        st = pc_fail(err, PC_ERR_NOMEM, "out of memory resampling");
                        goto done;
                    }
                    hrows = nh;
                    hrows_cap = hn;
                }
                if (rn > 0) {
                    st = pc_surface_read_region_into(r->s, read, rows, rn, err);
                    if (st != PC_OK) {
                        goto done;
                    }
                    pc_algo_premultiply(rows, rn, r->n, r->alpha);
                }
                if (hrows == NULL || (rn > 0 && rows == NULL)) {
                    st = pc_fail(err, PC_ERR_NOMEM, "out of memory resampling");
                    goto done;
                }
                memset(hrows, 0, hn * sizeof(float));
                if (rw > 0 && rows != NULL) {
                    for (i = 0; i < nrows; i++) {
                        pc_algo_resample_horizontal(rows + i * rw, rw, hrows + i * m, r->hx, r->x_lo, r->def,
                                                    r->n);
                    }
                }
                for (i = 0; i < nrows; i++) {
                    add_row(r, b0, b1, res, (int32_t)((int64_t)c0 + (int64_t)i), hrows + i * m);
                }
                c0 = c1;
            }
        } else {
            /* Nothing stored in this tile row: each output row takes the default row once, with
             * the summed weight of those rows. */
            for (k = b0; k < b1; k++) {
                size_t len = r->vy->len[k], a, b, j;
                int64_t da = (int64_t)y - r->vy->start[k], db = (int64_t)y1 - r->vy->start[k];
                float w = -0.0f, *d = res + (k - b0) * m;
                a = da > 0 ? (size_t)da : 0;
                b = db > 0 ? (size_t)db : 0;
                if (a > len) {
                    a = len;
                }
                if (b > len) {
                    b = len;
                }
                for (j = a; j < b; j++) {
                    w += r->vy->w[r->vy->off[k] + j];
                }
                if (w != 0.0f) {
                    for (i = 0; i < m; i++) {
                        d[i] += r->def_row[i] * w;
                    }
                }
            }
        }
        y = y1;
    }
    for (k = b0; k < b1; k++) {
        float *d = res + (k - b0) * m, o = r->vy->outside[k];
        for (i = 0; i < m; i++) {
            d[i] += r->def_row[i] * o;
        }
    }
    if (r->alpha) {
        for (i = r->n - 1; i < nres; i += r->n) {
            res[i] = pc_clampf(res[i], 0.0f, 1.0f);
        }
    }
    pc_algo_unpremultiply(res, nres, r->n, r->alpha);
done:
    free(rows);
    free(hrows);
    if (st != PC_OK) {
        free(res);
        return st;
    }
    *out = res;
    return PC_OK;
}

static pc_status band_job(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const rs_ctx *r = vctx;
    size_t k;
    (void)chunk;
    for (k = begin; k < end; k++) {
        pc_status st = run_band(r, r->first_band + k, &r->res[k], &r->errs[k]);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

/* The Preserve Details sharpening pass (Unsharp Mask, chunk L5). */
static pc_status preserve_details(pc_surface *out, double sx, double sy, pc_error *err)
{
    const pc_algo_filter_def *def = pc_algo_filter_def_find("unsharpMask");
    yyjson_mut_doc *d;
    yyjson_mut_val *o;
    yyjson_doc *doc = NULL;
    pc_algo_filter f;
    pc_surface sharp;
    pc_rect content, area;
    pc_buf text;
    pc_status st;
    if (def == NULL) {
        return pc_fail(err, PC_ERR_UNSUPPORTED, "Preserve Details needs the Unsharp Mask filter");
    }
    /* Built through JSON so this file needs nothing from the filter's own header. */
    d = yyjson_mut_doc_new(NULL);
    o = d != NULL ? yyjson_mut_obj(d) : NULL;
    if (o == NULL || !yyjson_mut_obj_add_str(d, o, "filter", "unsharpMask") ||
        !yyjson_mut_obj_add_real(d, o, "amount", 30.0) ||
        !yyjson_mut_obj_add_real(d, o, "radius", (double)(0.6f * (float)pc_rs_max(sx, sy))) ||
        !yyjson_mut_obj_add_real(d, o, "threshold", 0.0)) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_buf_init(&text);
    st = pc_json_write(o, 0, &text, err);
    yyjson_mut_doc_free(d);
    if (st == PC_OK) {
        st = pc_json_parse((const char *)text.data, text.len, &doc, err);
    }
    pc_buf_fini(&text);
    if (st != PC_OK) {
        return st;
    }
    st = pc_algo_filter_from_json(yyjson_doc_get_root(doc), &f, err);
    yyjson_doc_free(doc);
    if (st != PC_OK) {
        return st;
    }
    content = pc_surface_content_bounds(out);
    area = pc_algo_output_area(&f, content, content, NULL);
    st = pc_algo_apply(out, &f, area, content, NULL, &sharp, err);
    pc_algo_filter_fini(&f);
    if (st != PC_OK) {
        return st;
    }
    pc_surface_fini(out);
    *out = sharp;
    return PC_OK;
}

/* One pass. With `canvas`, the content and the canvas repeat their edge outwards. */
static pc_status resize_pass(const pc_surface *s, pass p, const pc_rect *canvas, size_t budget,
                             pc_surface *out, pc_error *err)
{
    double sx = p.sx, sy = p.sy;
    pc_rect edge_r;
    const pc_rect *edge = NULL;
    pc_pixel_format fmt = pc_surface_format(s);
    float defpx[PC_RASTER_MAX_CHANNELS];
    pc_surface res;
    pc_rect src, dst, area;
    pc_algo_taps hx, vy;
    int32_t ex[2], ey[2], cx[2], cy[2];
    float *def_row = NULL, *row = NULL, **parts = NULL;
    pc_error *errs = NULL;
    size_t n = pc_pixel_format_channels(fmt), dw, k, nbands, group, g0, span, rpr, m;
    rs_ctx r;
    pc_status st;
    pc_surface_default_pixel(s, defpx);
    pc_surface_init_default(&res, fmt, defpx, n);
    src = pc_surface_content_bounds(s);
    if (canvas != NULL) {
        edge_r = pc_rect_union(src, *canvas);
        edge = pc_rect_is_empty(edge_r) ? NULL : &edge_r;
    }
    dst = pc_algo_scaled_rect(src, sx, sy);
    if (pc_rect_is_empty(dst)) {
        *out = res;
        return PC_OK;
    }
    if (!isfinite(sx) || !isfinite(sy) || pc_rect_width(dst) > MAX_DIM || pc_rect_height(dst) > MAX_DIM) {
        pc_surface_fini(&res);
        return pc_fail(err, PC_ERR_LIMIT, "resampled size %ux%u is too large", pc_rect_width(dst),
                       pc_rect_height(dst));
    }
    /* With an edge, every tap is clamped into the edge area, which holds the content. */
    area = edge != NULL ? *edge : src;
    ex[0] = area.x0, ex[1] = area.x1 - 1, ey[0] = area.y0, ey[1] = area.y1 - 1;
    cx[0] = area.x0, cx[1] = area.x1 - 1, cy[0] = area.y0, cy[1] = area.y1 - 1;
    memset(&hx, 0, sizeof hx);
    memset(&vy, 0, sizeof vy);
    memset(&r, 0, sizeof r);
    st = pc_algo_taps_make(dst.x0, dst.x1, sx, p.kx, edge != NULL ? ex : NULL, cx, &hx, err);
    if (st == PC_OK) {
        st = pc_algo_taps_make(dst.y0, dst.y1, sy, p.ky, edge != NULL ? ey : NULL, cy, &vy, err);
    }
    if (st != PC_OK) {
        goto done;
    }
    r.x_lo = area.x0;
    r.x_hi = area.x0;
    {
        bool any_lo = false, any_hi = false;
        for (k = 0; k < hx.n; k++) {
            int32_t end = (int32_t)((int64_t)hx.start[k] + (int64_t)hx.len[k]);
            if (hx.len[k] > 0 && (!any_lo || hx.start[k] < r.x_lo)) {
                r.x_lo = hx.start[k];
                any_lo = true;
            }
            if (!any_hi || end > r.x_hi) {
                r.x_hi = end;
                any_hi = true;
            }
        }
        if (r.x_hi < r.x_lo) {
            r.x_hi = r.x_lo;
        }
    }
    pc_algo_premultiply(defpx, n, n, fmt.alpha);
    dw = pc_rect_width(dst);
    m = dw * n;
    span = (size_t)((int64_t)r.x_hi - r.x_lo);
    /* A source row of nothing but the default pixel, after the horizontal pass. */
    if (!pc_size_mul(span, n, &k) || k > PC_MAX_ALLOC / sizeof(float)) {
        st = pc_fail(err, PC_ERR_LIMIT, "resampling window is too large");
        goto done;
    }
    row = pc_malloc_array(k > 0 ? k : 1, sizeof(float));
    def_row = pc_calloc_array(m, sizeof(float));
    if (row == NULL || def_row == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory resampling");
        goto done;
    }
    {
        size_t i;
        for (i = 0; i < k; i++) {
            row[i] = defpx[i % n];
        }
    }
    pc_algo_resample_horizontal(row, k, def_row, &hx, r.x_lo, defpx, n);
    free(row);
    row = NULL;
    /* At most one tile row per read anyway. */
    {
        size_t a = (span > 0 ? span : 1) * n;
        size_t den = a > m ? a : m;
        rpr = budget / (den > 0 ? den : 1);
        if (rpr < 1) {
            rpr = 1;
        }
        if (rpr > PC_TILE_SIZE) {
            rpr = PC_TILE_SIZE;
        }
    }
    r.s = s;
    r.hx = &hx;
    r.vy = &vy;
    r.def = defpx;
    r.def_row = def_row;
    r.area = area;
    r.dst = dst;
    r.rows_per_read = (int32_t)rpr;
    r.n = n;
    r.dw = dw;
    r.alpha = fmt.alpha;
    nbands = (vy.n + BAND - 1) / BAND;
    /* Bands go into the output a group at a time, so the f32 rows held at once are bounded by the
     * group, not by the whole destination (#1544). */
    group = pc_pool_threads(pc_pool_global()) * 2;
    if (group < 1) {
        group = 1;
    }
    parts = pc_calloc_array(group, sizeof *parts);
    errs = pc_calloc_array(group, sizeof *errs);
    if (parts == NULL || errs == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory resampling");
        goto done;
    }
    r.res = parts;
    r.errs = errs;
    for (g0 = 0; g0 < nbands && st == PC_OK; g0 += group) {
        size_t gn = nbands - g0 < group ? nbands - g0 : group, i;
        memset(parts, 0, gn * sizeof *parts);
        r.first_band = g0;
        st = pc_pool_for(pc_pool_global(), gn, 1, band_job, &r);
        if (st != PC_OK) {
            for (i = 0; i < gn; i++) {
                if (errs[i].code != PC_OK && parts[i] == NULL) {
                    if (err != NULL) {
                        *err = errs[i];
                    }
                    st = errs[i].code;
                    break;
                }
            }
        }
        for (i = 0; i < gn && st == PC_OK; i++) {
            size_t b0 = (g0 + i) * BAND, b1 = b0 + BAND < vy.n ? b0 + BAND : vy.n;
            pc_rect band = pc_rect_make(dst.x0, (int32_t)((int64_t)dst.y0 + (int64_t)b0), dst.x1,
                                        (int32_t)((int64_t)dst.y0 + (int64_t)b1));
            st = pc_surface_write_region(&res, band, parts[i], (b1 - b0) * m, err);
        }
        for (i = 0; i < gn; i++) {
            free(parts[i]);
            parts[i] = NULL;
        }
    }
done:
    free(parts);
    free(errs);
    free(row);
    free(def_row);
    pc_algo_taps_fini(&hx);
    pc_algo_taps_fini(&vy);
    if (st != PC_OK) {
        pc_surface_fini(&res);
        return st;
    }
    *out = res;
    return PC_OK;
}

/* Runs filter's passes. Between two passes the image is stored in its own format, which clips it
 * (and rounds it at 8 and 16 bits) as Photoshop does. */
pc_status pc_algo_resize_with_budget(const pc_surface *s, double sx, double sy, pc_algo_resample filter,
                                     const pc_rect *canvas, size_t budget, pc_surface *out, pc_error *err)
{
    pass passes[2];
    size_t np = plan(filter, sx, sy, passes), i;
    pc_surface cur;
    bool have = false;
    pc_rect cv;
    pc_status st = PC_OK;
    if (canvas != NULL) {
        cv = *canvas;
    }
    for (i = 0; i < np; i++) {
        pc_surface next;
        st = resize_pass(have ? &cur : s, passes[i], canvas != NULL ? &cv : NULL, budget, &next, err);
        if (have) {
            pc_surface_fini(&cur);
        }
        if (st != PC_OK) {
            return st;
        }
        if (canvas != NULL) {
            cv = pc_algo_scaled_rect(cv, passes[i].sx, passes[i].sy);
        }
        cur = next;
        have = true;
    }
    if (!have) {
        st = pc_surface_clone(s, &cur, err);
        if (st != PC_OK) {
            return st;
        }
    }
    if (filter == PC_ALGO_RESAMPLE_PRESERVE_DETAILS && (sx > 1.0 || sy > 1.0)) {
        st = preserve_details(&cur, sx, sy, err);
        if (st != PC_OK) {
            pc_surface_fini(&cur);
            return st;
        }
    }
    pc_surface_prune(&cur);
    *out = cur;
    return PC_OK;
}

pc_status pc_algo_resize_surface(const pc_surface *s, double sx, double sy, pc_algo_resample filter,
                                 pc_surface *out, pc_error *err)
{
    return pc_algo_resize_with_budget(s, sx, sy, filter, NULL, PC_ALGO_RESAMPLE_READ_BUDGET, out, err);
}

pc_status pc_algo_resize_surface_in_canvas(const pc_surface *s, double sx, double sy, pc_algo_resample filter,
                                           pc_rect canvas, pc_surface *out, pc_error *err)
{
    return pc_algo_resize_with_budget(s, sx, sy, filter, &canvas, PC_ALGO_RESAMPLE_READ_BUDGET, out, err);
}

pc_status pc_algo_translate_surface(const pc_surface *s, int32_t dx, int32_t dy, pc_surface *out,
                                    pc_error *err)
{
    return pc_surface_translated(s, dx, dy, pc_surface_content_bounds(s), out, err);
}

pc_status pc_algo_crop_surface(const pc_surface *s, pc_rect keep, pc_surface *out, pc_error *err)
{
    float defpx[PC_RASTER_MAX_CHANNELS];
    pc_pixel_format fmt = pc_surface_format(s);
    pc_surface res;
    pc_rect r = pc_rect_intersect(pc_surface_content_bounds(s), keep);
    pc_surface_default_pixel(s, defpx);
    pc_surface_init_default(&res, fmt, defpx, pc_pixel_format_channels(fmt));
    if (!pc_rect_is_empty(r)) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        pc_status st = pc_surface_to_interleaved(s, r, &bytes, &len, err);
        if (st == PC_OK) {
            st = pc_surface_write_interleaved(&res, r, bytes, len, err);
        }
        free(bytes);
        if (st != PC_OK) {
            pc_surface_fini(&res);
            return st;
        }
    }
    *out = res;
    return PC_OK;
}
