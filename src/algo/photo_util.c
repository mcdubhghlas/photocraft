/* Shared numeric helpers for the photography modules (port of crates/algo/src/photo_util.rs). */
#include "algo/photo_util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* |v| bit pattern: f64::total_cmp on non-negative values (abs clears the sign, NaN included) is
 * the unsigned order of the bits. */
static uint64_t abs_bits(double v)
{
    uint64_t u;
    double a = fabs(v);
    memcpy(&u, &a, sizeof u);
    return u;
}

bool pc_algo_photo_solve_dense(size_t n, double *a, double *b, double *x)
{
    size_t col, r, k;
    if (n == 0) {
        return true;
    }
    for (col = 0; col < n; col++) {
        /* (col..n).max_by(|i, j| |a[i]|.total_cmp(|a[j]|)): the last maximum wins. */
        size_t piv = col;
        uint64_t best = abs_bits(a[col * n + col]);
        double d;
        for (r = col + 1; r < n; r++) {
            uint64_t v = abs_bits(a[r * n + col]);
            if (v >= best) {
                best = v;
                piv = r;
            }
        }
        if (fabs(a[piv * n + col]) < 1e-14) {
            return false;
        }
        if (piv != col) {
            for (k = 0; k < n; k++) {
                double t = a[col * n + k];
                a[col * n + k] = a[piv * n + k];
                a[piv * n + k] = t;
            }
            {
                double t = b[col];
                b[col] = b[piv];
                b[piv] = t;
            }
        }
        d = a[col * n + col];
        for (r = col + 1; r < n; r++) {
            double f = a[r * n + col] / d;
            if (f == 0.0) {
                continue;
            }
            for (k = col; k < n; k++) {
                a[r * n + k] -= f * a[col * n + k];
            }
            b[r] -= f * b[col];
        }
    }
    /* Back substitution in place: b[k] for k > r already holds x[k]. */
    for (r = n; r-- > 0;) {
        double s = -0.0;
        for (k = r + 1; k < n; k++) {
            s += a[r * n + k] * b[k];
        }
        b[r] = (b[r] - s) / a[r * n + r];
    }
    for (r = 0; r < n; r++) {
        if (!isfinite(b[r])) {
            return false;
        }
    }
    memcpy(x, b, n * sizeof *x);
    return true;
}

pc_status pc_algo_photo_normal_init(pc_algo_photo_normal *ne, size_t n, pc_error *err)
{
    size_t nn;
    double *ata, *atb;
    if (!pc_size_mul(n, n, &nn) || nn > PC_MAX_ALLOC / sizeof(double)) {
        return pc_fail(err, PC_ERR_LIMIT, "normal equations with %zu unknowns are too large", n);
    }
    ata = pc_calloc_array(nn > 0 ? nn : 1, sizeof(double));
    atb = pc_calloc_array(n > 0 ? n : 1, sizeof(double));
    if (ata == NULL || atb == NULL) {
        free(ata);
        free(atb);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for normal equations");
    }
    ne->n = n;
    ne->ata = ata;
    ne->atb = atb;
    return PC_OK;
}

void pc_algo_photo_normal_fini(pc_algo_photo_normal *ne)
{
    if (ne != NULL) {
        free(ne->ata);
        free(ne->atb);
        ne->ata = ne->atb = NULL;
        ne->n = 0;
    }
}

void pc_algo_photo_normal_add(pc_algo_photo_normal *ne, const pc_algo_photo_term *row, size_t count,
                              double rhs, double w)
{
    double w2 = w * w;
    size_t p, q;
    for (p = 0; p < count; p++) {
        size_t i = row[p].i;
        double a = row[p].c;
        if (i >= ne->n) {
            continue;
        }
        ne->atb[i] += w2 * a * rhs;
        for (q = 0; q < count; q++) {
            if (row[q].i < ne->n) {
                ne->ata[i * ne->n + row[q].i] += w2 * a * row[q].c;
            }
        }
    }
}

bool pc_algo_photo_normal_solve(pc_algo_photo_normal *ne, double *x)
{
    return pc_algo_photo_solve_dense(ne->n, ne->ata, ne->atb, x);
}

void pc_algo_photo_sparse_ls_init(pc_algo_photo_sparse_ls *ls, size_t n)
{
    memset(ls, 0, sizeof *ls);
    ls->n = n;
}

void pc_algo_photo_sparse_ls_fini(pc_algo_photo_sparse_ls *ls)
{
    if (ls != NULL) {
        free(ls->terms);
        free(ls->row_end);
        free(ls->rhs);
        pc_algo_photo_sparse_ls_init(ls, 0);
    }
}

pc_status pc_algo_photo_sparse_ls_add(pc_algo_photo_sparse_ls *ls, const pc_algo_photo_term *row,
                                      size_t count, double rhs, double w, pc_error *err)
{
    size_t k, need;
    if (!pc_size_add(ls->nterms, count, &need)) {
        return pc_fail(err, PC_ERR_LIMIT, "too many least-squares terms");
    }
    if (need > ls->terms_cap) {
        pc_algo_photo_term *t = pc_grow_array(ls->terms, &ls->terms_cap, need, sizeof *t);
        if (t == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for least-squares terms");
        }
        ls->terms = t;
    }
    if (ls->nrows + 1 > ls->rows_cap) {
        size_t cap = ls->rows_cap, cap2;
        size_t *e = pc_grow_array(ls->row_end, &cap, ls->nrows + 1, sizeof *e);
        double *r;
        if (e == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for least-squares rows");
        }
        ls->row_end = e;
        cap2 = ls->rows_cap;
        r = pc_grow_array(ls->rhs, &cap2, cap, sizeof *r);
        if (r == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for least-squares rows");
        }
        ls->rhs = r;
        ls->rows_cap = cap < cap2 ? cap : cap2;
    }
    for (k = 0; k < count; k++) {
        if (row[k].i < ls->n) {
            ls->terms[ls->nterms].i = row[k].i;
            ls->terms[ls->nterms].c = row[k].c * w;
            ls->nterms++;
        }
    }
    ls->rhs[ls->nrows] = rhs * w;
    ls->row_end[ls->nrows] = ls->nterms;
    ls->nrows++;
    return PC_OK;
}

/* out[r] = sum over row r of c * x[i] (sum from -0.0). */
static void ls_ax(const pc_algo_photo_sparse_ls *ls, const double *x, double *out)
{
    size_t r, k = 0;
    for (r = 0; r < ls->nrows; r++) {
        double s = -0.0;
        for (; k < ls->row_end[r]; k++) {
            s += ls->terms[k].c * x[ls->terms[k].i];
        }
        out[r] = s;
    }
}

static void ls_atv(const pc_algo_photo_sparse_ls *ls, const double *v, double *out)
{
    size_t r, k = 0;
    for (r = 0; r < ls->n; r++) {
        out[r] = 0.0;
    }
    for (r = 0; r < ls->nrows; r++) {
        for (; k < ls->row_end[r]; k++) {
            out[ls->terms[k].i] += ls->terms[k].c * v[r];
        }
    }
}

static double sumsq(const double *v, size_t n)
{
    double s = -0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        s += v[i] * v[i];
    }
    return s;
}

pc_status pc_algo_photo_sparse_ls_solve(const pc_algo_photo_sparse_ls *ls, const double *x0, size_t iters,
                                        double *x, pc_error *err)
{
    size_t n = ls->n, m = ls->nrows, i, it;
    size_t nn = n > 0 ? n : 1, mm = m > 0 ? m : 1;
    double *xs = pc_malloc_array(nn, sizeof(double)), *r = pc_malloc_array(mm, sizeof(double));
    double *s = pc_malloc_array(nn, sizeof(double)), *p = pc_malloc_array(nn, sizeof(double));
    double *q = pc_malloc_array(mm, sizeof(double));
    double gamma, g0;
    if (xs == NULL || r == NULL || s == NULL || p == NULL || q == NULL) {
        free(xs), free(r), free(s), free(p), free(q);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for the least-squares solver");
    }
    memcpy(xs, x0, n * sizeof *xs);
    ls_ax(ls, xs, q);
    for (i = 0; i < m; i++) {
        r[i] = ls->rhs[i] - q[i];
    }
    ls_atv(ls, r, s);
    memcpy(p, s, n * sizeof *p);
    gamma = sumsq(s, n);
    g0 = pc_rs_max_k(gamma, 1e-30);
    for (it = 0; it < iters; it++) {
        double qq, alpha, g1, beta;
        if (gamma <= g0 * 1e-20) {
            break;
        }
        ls_ax(ls, p, q);
        qq = sumsq(q, m);
        if (qq <= 1e-300) {
            break;
        }
        alpha = gamma / qq;
        for (i = 0; i < n; i++) {
            xs[i] += alpha * p[i];
        }
        for (i = 0; i < m; i++) {
            r[i] -= alpha * q[i];
        }
        ls_atv(ls, r, s);
        g1 = sumsq(s, n);
        beta = g1 / gamma;
        gamma = g1;
        for (i = 0; i < n; i++) {
            p[i] = s[i] + beta * p[i];
        }
    }
    memcpy(x, xs, n * sizeof *x);
    free(xs), free(r), free(s), free(p), free(q);
    return PC_OK;
}

uint64_t pc_algo_photo_rng_next(pc_algo_photo_rng *r)
{
    uint64_t z;
    r->s += 0x9E3779B97F4A7C15ull;
    z = r->s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

size_t pc_algo_photo_rng_below(pc_algo_photo_rng *r, size_t n)
{
    uint64_t m = n > 0 ? (uint64_t)n : 1u;
    return (size_t)(pc_algo_photo_rng_next(r) % m);
}

double pc_algo_photo_rng_unit(pc_algo_photo_rng *r)
{
    return (double)(pc_algo_photo_rng_next(r) >> 11) / 9007199254740992.0;
}

float pc_algo_photo_hash01(int64_t x, int64_t y, uint64_t seed)
{
    uint64_t z = ((uint64_t)x * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)y * 0xC2B2AE3D27D4EB4Full) ^
                 (seed * 0x165667B19E3779F9ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (float)(z >> 40) / 16777216.0f;
}

float pc_algo_photo_srgb_to_linear(float v)
{
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

float pc_algo_photo_linear_to_srgb(float v)
{
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

float pc_algo_photo_bilinear(const float *buf, size_t len, size_t w, size_t h, size_t ch, size_t c, float x,
                             float y)
{
    size_t need, x0, y0, x1, y1;
    float tx, ty, top, bot;
    if (w == 0 || h == 0 || c >= ch || !pc_size_mul3(w, h, ch, &need) || need > len) {
        return 0.0f;
    }
    x = pc_clampf(x, 0.0f, (float)(w - 1));
    y = pc_clampf(y, 0.0f, (float)(h - 1));
    x0 = pc_f32_to_size(floorf(x));
    y0 = pc_f32_to_size(floorf(y));
    if (x0 > w - 1) { /* only for huge w where (w - 1) as f32 rounds up */
        x0 = w - 1;
    }
    if (y0 > h - 1) {
        y0 = h - 1;
    }
    x1 = x0 + 1 < w - 1 ? x0 + 1 : w - 1;
    y1 = y0 + 1 < h - 1 ? y0 + 1 : h - 1;
    tx = x - (float)x0;
    ty = y - (float)y0;
#define P(xx, yy) buf[((yy) * w + (xx)) * ch + c]
    top = P(x0, y0) + (P(x1, y0) - P(x0, y0)) * tx;
    bot = P(x0, y1) + (P(x1, y1) - P(x0, y1)) * tx;
#undef P
    return top + (bot - top) * ty;
}

void pc_algo_photo_catmull_rom(float t, float out[4])
{
    float t2 = t * t, t3 = t2 * t;
    out[0] = -0.5f * t3 + t2 - 0.5f * t;
    out[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
    out[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    out[3] = 0.5f * t3 - 0.5f * t2;
}
