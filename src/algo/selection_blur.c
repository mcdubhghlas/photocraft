/* Select > Modify > Smooth and Feather (port of crates/algo/src/selection_blur.rs); see
 * selection_blur.h. The vertical passes walk strips of 64 columns row by row (no transpose);
 * windows lying entirely in a constant run are written without arithmetic. */
#include "algo/selection_blur.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/blur_int.h"
#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Cosine terms in the wide-kernel series (5 gives about 0.1/255 per pass). */
#define TERMS 5
/* Kernels up to this radius are convolved directly. */
#define DIRECT_MAX_R 12
/* Largest error (mask units, per pass) a cosine fit may have before falling back. */
#define FIT_TOL (0.3 / 255.0)
/* Sigma cap: far beyond any canvas, keeps the kernel arithmetic finite for absurd radii. */
#define MAX_SIGMA 100000.0f
/* Box radius cap for the same reason. */
#define MAX_BOX_R ((size_t)1 << 24)
/* Columns per task in the vertical pass. */
#define STRIP 64
#define PI_D 3.14159265358979323846

/* A window of the mask: columns x0..x1, rows y0..y1. */
typedef struct win {
    size_t x0, x1, y0, y1;
} win;

static size_t win_w(win b)
{
    return b.x1 - b.x0;
}
static size_t win_h(win b)
{
    return b.y1 - b.y0;
}

/* Bounding box of the non-zero samples (NaN counts as non-zero); false when all are 0. */
static bool nonzero_bounds(const float *m, size_t w, size_t h, win *out)
{
    bool found = false;
    size_t y, x;
    win b = {0, 0, 0, 0};
    for (y = 0; y < h; y++) {
        const float *row = m + y * w;
        size_t first = w, last = 0;
        for (x = 0; x < w; x++) {
            if (row[x] != 0.0f) {
                first = x;
                break;
            }
        }
        if (first == w) {
            continue;
        }
        for (x = w; x-- > first;) {
            if (row[x] != 0.0f) {
                last = x;
                break;
            }
        }
        if (!found) {
            b.x0 = first, b.x1 = last + 1, b.y0 = y, b.y1 = y + 1;
            found = true;
        } else {
            b.x0 = first < b.x0 ? first : b.x0;
            b.x1 = last + 1 > b.x1 ? last + 1 : b.x1;
            b.y1 = y + 1;
        }
    }
    *out = b;
    return found;
}

static win grow(win b, size_t g, size_t w, size_t h)
{
    win r;
    r.x0 = b.x0 > g ? b.x0 - g : 0;
    r.x1 = b.x1 + g < b.x1 || b.x1 + g > w ? w : b.x1 + g;
    r.y0 = b.y0 > g ? b.y0 - g : 0;
    r.y1 = b.y1 + g < b.y1 || b.y1 + g > h ? h : b.y1 + g;
    return r;
}

/* The horizontal pass's output as the vertical pass reads it: bh rows of gw samples at window
 * rows off..off + bh of a window gh rows tall (other rows are zero). */
typedef struct rows_view {
    const float *tmp;
    size_t gw, bh, off, gh;
} rows_view;

/* Columns c0..c0 + sw of window row yy, or NULL for a zero row. */
static const float *rows_get(const rows_view *r, size_t yy, size_t c0)
{
    if (r->tmp == NULL || yy < r->off || yy - r->off >= r->bh) {
        return NULL;
    }
    return r->tmp + (yy - r->off) * r->gw + c0;
}

static void add_row(double *acc, size_t sw, const rows_view *rows, size_t yy, size_t c0, double k)
{
    const float *row;
    size_t i;
    if (k == 0.0) {
        return;
    }
    row = rows_get(rows, yy, c0);
    if (row != NULL) {
        for (i = 0; i < sw; i++) {
            acc[i] += k * (double)row[i];
        }
    }
}

/* ---- Smooth ---- */

static float curve(float v)
{
    return pc_clampf((v - 0.5f) * 4.0f + 0.5f, 0.0f, 1.0f);
}

/* Running-sum box average of one line (radius r, edges replicated). */
static void box_line(const float *src, float *dst, size_t n, size_t r)
{
    size_t inside, i;
    double acc, sum = -0.0, norm;
    int64_t ri = (int64_t)r, last = (int64_t)n - 1, x;
    if (n == 0) {
        return;
    }
    if (r == 0) {
        memcpy(dst, src, n * sizeof(float));
        return;
    }
    inside = r < n - 1 ? r : n - 1;
    for (i = 0; i <= inside; i++) {
        sum += (double)src[i];
    }
    acc = (double)r * (double)src[0] + sum + (double)(r - inside) * (double)src[n - 1];
    norm = 1.0 / (double)(2 * r + 1);
    for (x = 0; x <= last; x++) {
        int64_t a = x + ri + 1, b = x - ri;
        a = a < 0 ? 0 : a > last ? last : a;
        b = b < 0 ? 0 : b > last ? last : b;
        dst[x] = (float)(acc * norm);
        acc += (double)src[a] - (double)src[b];
    }
}

typedef struct sm_job {
    const float *m;
    size_t w, r, gw;
    win b, wn;
    float *tmp;
    const rows_view *rows;
    float **strips;
    /* feather */
    const struct kernel *kh, *kv;
} sm_job;

static pc_status smooth_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const sm_job *j = vctx;
    size_t y;
    (void)chunk;
    for (y = begin; y < end; y++) {
        box_line(j->m + (j->b.y0 + y) * j->w + j->wn.x0, j->tmp + y * j->gw, j->gw, j->r);
    }
    return PC_OK;
}

/* The vertical box pass over columns c0..c0 + sw (edges replicated). */
static pc_status box_strip(const rows_view *rows, size_t c0, size_t sw, size_t r, float *out)
{
    size_t gh = rows->gh, inside, yy, i;
    double *acc, norm;
    if (gh == 0 || sw == 0) {
        return PC_OK;
    }
    acc = pc_calloc_array(sw, sizeof *acc);
    if (acc == NULL) {
        return PC_ERR_NOMEM;
    }
    inside = r < gh - 1 ? r : gh - 1;
    add_row(acc, sw, rows, 0, c0, (double)r);
    for (i = 0; i <= inside; i++) {
        add_row(acc, sw, rows, i, c0, 1.0);
    }
    add_row(acc, sw, rows, gh - 1, c0, (double)(r - inside));
    norm = 1.0 / (double)(2 * r + 1);
    for (yy = 0; yy < gh; yy++) {
        for (i = 0; i < sw; i++) {
            out[yy * sw + i] = (float)(acc[i] * norm);
        }
        add_row(acc, sw, rows, yy + r + 1 < gh - 1 ? yy + r + 1 : gh - 1, c0, 1.0);
        add_row(acc, sw, rows, yy > r ? yy - r : 0, c0, -1.0);
    }
    free(acc);
    return PC_OK;
}

static pc_status smooth_strips(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const sm_job *j = vctx;
    size_t s;
    (void)chunk;
    for (s = begin; s < end; s++) {
        size_t c0 = s * STRIP, sw = STRIP < j->gw - c0 ? STRIP : j->gw - c0;
        pc_status st = box_strip(j->rows, c0, sw, j->r, j->strips[s]);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

typedef struct asm_job {
    float *const *strips;
    win wn;
    size_t w;
    float *out;
    bool feather;
} asm_job;

static pc_status assemble_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const asm_job *j = vctx;
    size_t y, s, i, ns = (win_w(j->wn) + STRIP - 1) / STRIP;
    (void)chunk;
    for (y = begin; y < end; y++) {
        size_t yy;
        if (y < j->wn.y0 || y >= j->wn.y1) {
            continue;
        }
        yy = y - j->wn.y0;
        for (s = 0; s < ns; s++) {
            size_t c0 = s * STRIP, sw = STRIP < win_w(j->wn) - c0 ? STRIP : win_w(j->wn) - c0;
            const float *src = j->strips[s] + yy * sw;
            float *dst = j->out + y * j->w + j->wn.x0 + c0;
            for (i = 0; i < sw; i++) {
                /* Feather: rounding residue of the sliding sums is not coverage. */
                dst[i] = j->feather ? (fabsf(src[i]) < 1e-7f ? 0.0f : src[i]) : curve(src[i]);
            }
        }
    }
    return PC_OK;
}

/* Allocates the strips of a gw x gh window. */
static float **strips_new(size_t gw, size_t gh, size_t *ns)
{
    size_t n = (gw + STRIP - 1) / STRIP, s;
    float **v = pc_calloc_array(n + 1, sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    for (s = 0; s < n; s++) {
        size_t sw = STRIP < gw - s * STRIP ? STRIP : gw - s * STRIP;
        v[s] = pc_calloc_array(gh * sw + 1, sizeof(float));
        if (v[s] == NULL) {
            size_t k;
            for (k = 0; k < s; k++) {
                free(v[k]);
            }
            free(v);
            return NULL;
        }
    }
    *ns = n;
    return v;
}

static void strips_free(float **v, size_t n)
{
    size_t s;
    if (v != NULL) {
        for (s = 0; s < n; s++) {
            free(v[s]);
        }
        free(v);
    }
}

static pc_status copy_out(const float *m, size_t len, bool apply_curve, float fill, bool use_fill,
                          float **out, pc_error *err)
{
    float *v = pc_malloc_array(len > 0 ? len : 1, sizeof(float));
    size_t i;
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for a selection");
    }
    for (i = 0; i < len; i++) {
        v[i] = use_fill ? fill : apply_curve ? curve(m[i]) : m[i];
    }
    *out = v;
    return PC_OK;
}

pc_status pc_algo_selection_smooth(const float *m, size_t len, size_t w, size_t h, float r, float **out,
                                   pc_error *err)
{
    size_t n, rr, ns = 0;
    win b, wn;
    sm_job j;
    rows_view rows;
    asm_job aj;
    float **strips, *res;
    pc_status st;
    if (!pc_size_mul(w, h, &n)) {
        n = SIZE_MAX;
    }
    if (len != n || n == 0) {
        return copy_out(m, len, true, 0.0f, false, out, err);
    }
    if (isfinite(r)) {
        rr = pc_f32_to_size(roundf(pc_rs_maxf_k(r, 0.0f)));
        rr = rr < MAX_BOX_R ? rr : MAX_BOX_R;
    } else {
        rr = r > 0.0f ? MAX_BOX_R : 0;
    }
    if (!nonzero_bounds(m, w, h, &b)) {
        return copy_out(m, len, false, curve(0.0f), true, out, err);
    }
    /* Grown one past the radius, so a window edge inside the canvas sits on zeros and edge
     * replication there equals the zeros beyond it. */
    wn = grow(b, rr + 1, w, h);
    memset(&j, 0, sizeof j);
    j.m = m;
    j.w = w;
    j.r = rr;
    j.gw = win_w(wn);
    j.b = b;
    j.wn = wn;
    j.tmp = pc_calloc_array(win_h(b) * j.gw + 1, sizeof(float));
    res = pc_malloc_array(n, sizeof(float));
    strips = strips_new(j.gw, win_h(wn), &ns);
    if (j.tmp == NULL || res == NULL || strips == NULL) {
        free(j.tmp);
        free(res);
        strips_free(strips, ns);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Smooth");
    }
    /* Rows: only the bounding box's rows are non-zero. */
    st = pc_algo_par_for(win_h(b), 8, smooth_rows, &j);
    rows.tmp = j.tmp;
    rows.gw = j.gw;
    rows.bh = win_h(b);
    rows.off = b.y0 - wn.y0;
    rows.gh = win_h(wn);
    j.rows = &rows;
    j.strips = strips;
    if (st == PC_OK) {
        st = pc_algo_par_for(ns, 1, smooth_strips, &j);
    }
    if (st == PC_OK) {
        size_t i;
        float c0 = curve(0.0f);
        for (i = 0; i < n; i++) {
            res[i] = c0;
        }
        aj.strips = strips;
        aj.wn = wn;
        aj.w = w;
        aj.out = res;
        aj.feather = false;
        st = pc_algo_par_for(h, 64, assemble_rows, &aj);
    }
    free(j.tmp);
    strips_free(strips, ns);
    if (st != PC_OK) {
        free(res);
        return pc_fail(err, st, "out of memory for Smooth");
    }
    *out = res;
    return PC_OK;
}

/* ---- Feather ---- */

/* Sliding cosine-series form of a symmetric kernel over -r..=r. */
typedef struct cos_kernel {
    size_t r;
    double a0, a[TERMS];
    double rot[TERMS][2];   /* e^{-i w}: shifts the window by one sample */
    double out_w[TERMS][2]; /* e^{-i w r} (leaving sample) */
    double in_w[TERMS][2];  /* e^{i w r} (entering sample) */
    double full[TERMS];     /* sum_{|j| <= r} cos(w j): a window full of ones */
    double omega[TERMS];
} cos_kernel;

typedef struct kernel {
    bool is_cos;
    float *taps; /* direct: 2 r + 1 taps */
    size_t ntaps;
    cos_kernel c;
} kernel;

static int64_t total_key64(double v)
{
    int64_t i;
    memcpy(&i, &v, 8);
    return i ^ (int64_t)((uint64_t)(i >> 63) >> 1);
}

/* Solves the small dense system a x = b (partial pivoting); false when singular. */
static bool solve(double a[TERMS + 1][TERMS + 1], double b[TERMS + 1], double x[TERMS + 1])
{
    const size_t n = TERMS + 1;
    size_t c, i, j, p;
    for (c = 0; c < n; c++) {
        /* Iterator::max_by keeps the last of equal maxima. */
        for (p = c, i = c; i < n; i++) {
            if (total_key64(fabs(a[i][c])) >= total_key64(fabs(a[p][c]))) {
                p = i;
            }
        }
        if (fabs(a[p][c]) < 1e-12) {
            return false;
        }
        for (j = 0; j < n; j++) {
            double t = a[c][j];
            a[c][j] = a[p][j];
            a[p][j] = t;
        }
        {
            double t = b[c];
            b[c] = b[p];
            b[p] = t;
        }
        for (i = c + 1; i < n; i++) {
            double f = a[i][c] / a[c][c];
            for (j = c; j < n; j++) {
                a[i][j] -= f * a[c][j];
            }
            b[i] -= f * b[c];
        }
    }
    for (i = n; i-- > 0;) {
        double s = -0.0;
        for (j = i + 1; j < n; j++) {
            s += a[i][j] * x[j];
        }
        x[i] = (b[i] - s) / a[i][i];
    }
    for (i = 0; i < n; i++) {
        if (!isfinite(x[i])) {
            return false;
        }
    }
    return true;
}

static void kernel_fini(kernel *k)
{
    free(k->taps);
    k->taps = NULL;
}

static double basis(const double *omega, size_t q, size_t j)
{
    return q == 0 ? 1.0 : cos(omega[q - 1] * (double)j);
}

/* The feather kernel (sigma, truncation radius r) as applied along lines of length n. */
static pc_status kernel_new(double sigma, size_t r, size_t n, kernel *k)
{
    double total, sum = -0.0, *taps, l, c[TERMS + 1], a[TERMS + 1][TERMS + 1], b[TERMS + 1], err = -0.0, jf;
    size_t jr, j, q, p, kk;
    memset(k, 0, sizeof *k);
    for (j = 1; j <= r; j++) {
        double x = (double)j;
        sum += exp(-(x * x) / (2.0 * sigma * sigma));
    }
    total = 1.0 + 2.0 * sum;
    /* Only offsets inside the line matter. */
    jr = n > 0 && r > n - 1 ? n - 1 : (n == 0 ? 0 : r);
    if (jr == 0) {
        k->taps = malloc(sizeof(float));
        if (k->taps == NULL) {
            return PC_ERR_NOMEM;
        }
        k->taps[0] = (float)(1.0 / total);
        k->ntaps = 1;
        return PC_OK;
    }
    taps = pc_malloc_array(jr + 1, sizeof *taps);
    if (taps == NULL) {
        return PC_ERR_NOMEM;
    }
    for (j = 0; j <= jr; j++) {
        double x = (double)j;
        taps[j] = exp(-(x * x) / (2.0 * sigma * sigma)) / total;
    }
    if (jr > DIRECT_MAX_R) {
        /* Least squares over j = -jr..=jr (symmetric: weight 2 for j > 0). */
        l = (double)(jr + 1);
        for (kk = 0; kk < TERMS; kk++) {
            k->c.omega[kk] = PI_D * (double)(kk + 1) / l;
        }
        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        for (j = 0; j <= jr; j++) {
            double wt = j == 0 ? 1.0 : 2.0, phi[TERMS + 1];
            for (q = 0; q <= TERMS; q++) {
                phi[q] = basis(k->c.omega, q, j);
            }
            for (p = 0; p <= TERMS; p++) {
                b[p] += wt * phi[p] * taps[j];
                for (q = 0; q <= TERMS; q++) {
                    a[p][q] += wt * phi[p] * phi[q];
                }
            }
        }
        if (solve(a, b, c)) {
            /* Error bound per pass: half the L1 distance between the series and the kernel. */
            for (j = 0; j <= jr; j++) {
                double wt = j == 0 ? 1.0 : 2.0, s = -0.0;
                for (q = 0; q <= TERMS; q++) {
                    s += c[q] * basis(k->c.omega, q, j);
                }
                err += wt * fabs(s - taps[j]);
            }
            err /= 2.0;
            if (isfinite(err) && !(err > FIT_TOL)) {
                jf = (double)jr;
                k->is_cos = true;
                k->c.r = jr;
                k->c.a0 = c[0];
                for (kk = 0; kk < TERMS; kk++) {
                    double s = -0.0, w = k->c.omega[kk];
                    for (j = 1; j <= jr; j++) {
                        s += cos(w * (double)j);
                    }
                    k->c.full[kk] = 1.0 + 2.0 * s;
                    k->c.a[kk] = c[kk + 1];
                    k->c.rot[kk][0] = cos(w);
                    k->c.rot[kk][1] = -sin(w);
                    k->c.out_w[kk][0] = cos(w * jf);
                    k->c.out_w[kk][1] = -sin(w * jf);
                    k->c.in_w[kk][0] = cos(w * jf);
                    k->c.in_w[kk][1] = sin(w * jf);
                }
                free(taps);
                return PC_OK;
            }
        }
    }
    k->ntaps = 2 * jr + 1;
    k->taps = pc_malloc_array(k->ntaps, sizeof(float));
    if (k->taps == NULL) {
        free(taps);
        return PC_ERR_NOMEM;
    }
    for (j = 0; j < k->ntaps; j++) {
        k->taps[j] = (float)taps[j > jr ? j - jr : jr - j];
    }
    free(taps);
    return PC_OK;
}

static void cos_line(const cos_kernel *k, const float *src, float *dst, size_t n, size_t *runs)
{
    size_t r = k->r, x, i, q;
    bool have = false;
    double sb = 0.0, s[TERMS][2];
    memset(s, 0, sizeof s);
    /* runs[i]: end (exclusive) of the constant run containing i. */
    if (n > 0) {
        runs[n - 1] = n;
    }
    for (i = n > 0 ? n - 1 : 0; i-- > 0;) {
        runs[i] = src[i] == src[i + 1] ? runs[i + 1] : i + 1;
    }
    for (x = 0; x < n; x++) {
        size_t lo = x > r ? x - r : 0, hi = x + r + 1 < n ? x + r + 1 : n;
        float cv = src[lo];
        double v, sum = -0.0;
        if (runs[lo] >= hi && (cv == 0.0f || (x >= r && x + r < n))) {
            /* The whole window is one value (zeros may run off the line; ones may not). */
            double cf = (double)cv;
            dst[x] = cv;
            sb = cf * (double)(2 * r + 1);
            for (q = 0; q < TERMS; q++) {
                s[q][0] = cf * k->full[q];
                s[q][1] = 0.0;
            }
            have = true;
            continue;
        }
        if (have) {
            /* Slide from x - 1: drop src[x - 1 - r], take src[x + r]. */
            double gone = x > r ? (double)src[x - 1 - r] : 0.0;
            double come = x + r < n ? (double)src[x + r] : 0.0;
            sb += come - gone;
            for (q = 0; q < TERMS; q++) {
                double re = s[q][0] - gone * k->out_w[q][0], im = s[q][1] - gone * k->out_w[q][1];
                double cr = k->rot[q][0], ci = k->rot[q][1];
                s[q][0] = re * cr - im * ci + come * k->in_w[q][0];
                s[q][1] = re * ci + im * cr + come * k->in_w[q][1];
            }
        } else {
            /* Direct sums over the in-line part of the window. */
            sb = 0.0;
            memset(s, 0, sizeof s);
            for (i = lo; i < hi; i++) {
                double vv = (double)src[i], j = (double)i - (double)x;
                if (vv == 0.0) {
                    continue;
                }
                sb += vv;
                for (q = 0; q < TERMS; q++) {
                    s[q][0] += vv * cos(k->omega[q] * j);
                    s[q][1] += vv * sin(k->omega[q] * j);
                }
            }
            have = true;
        }
        for (q = 0; q < TERMS; q++) {
            sum += k->a[q] * s[q][0];
        }
        v = k->a0 * sb + sum;
        dst[x] = (float)v;
    }
}

/* Convolves one line (zero outside it) into dst. */
static void kernel_line(const kernel *k, const float *src, float *dst, size_t n, size_t *runs)
{
    size_t i, t;
    if (k->is_cos) {
        cos_line(&k->c, src, dst, n, runs);
        return;
    }
    {
        size_t r = k->ntaps / 2;
        memset(dst, 0, n * sizeof(float));
        for (i = 0; i < k->ntaps; i++) {
            size_t d0 = i >= r ? 0 : r - i, s0 = i >= r ? i - r : 0, len;
            if (d0 >= n || s0 >= n) {
                continue;
            }
            len = n - d0 < n - s0 ? n - d0 : n - s0;
            for (t = 0; t < len; t++) {
                dst[d0 + t] += k->taps[i] * src[s0 + t];
            }
        }
    }
}

/* cos_line down columns c0..c0 + sw, all columns of a row at once. */
static pc_status cos_strip(const cos_kernel *k, const rows_view *rows, size_t c0, size_t sw, float *out)
{
    size_t gh = rows->gh, r = k->r, yy, i, q;
    float *flagv = NULL;
    bool *flag = NULL, have = false;
    size_t *runs = NULL;
    double *sb = NULL, *re = NULL, *im = NULL, *acc = NULL;
    pc_status st = PC_OK;
    if (sw == 0 || gh == 0) {
        return PC_OK;
    }
    flagv = pc_malloc_array(gh, sizeof *flagv);
    flag = pc_malloc_array(gh, sizeof *flag);
    runs = pc_malloc_array(gh, sizeof *runs);
    sb = pc_calloc_array(sw, sizeof *sb);
    re = pc_calloc_array(sw * TERMS, sizeof *re);
    im = pc_calloc_array(sw * TERMS, sizeof *im);
    acc = pc_calloc_array(sw, sizeof *acc);
    if (flagv == NULL || flag == NULL || runs == NULL || sb == NULL || re == NULL || im == NULL ||
        acc == NULL) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    /* Rows whose strip segment is one value, and the runs of equal such rows. */
    for (yy = 0; yy < gh; yy++) {
        const float *s = rows_get(rows, yy, c0);
        if (s == NULL) {
            flag[yy] = true;
            flagv[yy] = 0.0f;
        } else {
            float c = s[0];
            flag[yy] = true;
            for (i = 0; i < sw; i++) {
                if (!(s[i] == c)) {
                    flag[yy] = false;
                    break;
                }
            }
            flagv[yy] = c;
        }
    }
    runs[gh - 1] = gh;
    for (i = gh - 1; i-- > 0;) {
        runs[i] = flag[i] && flag[i + 1] && flagv[i] == flagv[i + 1] ? runs[i + 1] : i + 1;
    }
    for (yy = 0; yy < gh; yy++) {
        size_t lo = yy > r ? yy - r : 0, hi = yy + r + 1 < gh ? yy + r + 1 : gh;
        float *dst = out + yy * sw;
        if (flag[lo] && runs[lo] >= hi && (flagv[lo] == 0.0f || (yy >= r && yy + r < gh))) {
            /* The whole window is one value (zero rows may run off the window; others may not). */
            double cf = (double)flagv[lo];
            for (i = 0; i < sw; i++) {
                dst[i] = flagv[lo];
                sb[i] = cf * (double)(2 * r + 1);
            }
            for (q = 0; q < TERMS; q++) {
                for (i = 0; i < sw; i++) {
                    re[q * sw + i] = cf * k->full[q];
                    im[q * sw + i] = 0.0;
                }
            }
            have = true;
            continue;
        }
        if (have) {
            /* Slide from yy - 1: drop row yy - 1 - r, take row yy + r. */
            const float *gone = yy > r ? rows_get(rows, yy - 1 - r, c0) : NULL;
            const float *come = rows_get(rows, yy + r, c0);
            for (i = 0; i < sw; i++) {
                sb[i] += (double)(come != NULL ? come[i] : 0.0f) - (double)(gone != NULL ? gone[i] : 0.0f);
            }
            for (q = 0; q < TERMS; q++) {
                double cr = k->rot[q][0], ci = k->rot[q][1], o0 = k->out_w[q][0], o1 = k->out_w[q][1];
                double i0 = k->in_w[q][0], i1 = k->in_w[q][1];
                for (i = 0; i < sw; i++) {
                    double g = gone != NULL ? (double)gone[i] : 0.0, c = come != NULL ? (double)come[i] : 0.0;
                    double a = re[q * sw + i] - g * o0, b = im[q * sw + i] - g * o1;
                    re[q * sw + i] = a * cr - b * ci + c * i0;
                    im[q * sw + i] = a * ci + b * cr + c * i1;
                }
            }
        } else {
            /* Direct sums over the in-window rows. */
            memset(sb, 0, sw * sizeof *sb);
            memset(re, 0, sw * TERMS * sizeof *re);
            memset(im, 0, sw * TERMS * sizeof *im);
            for (size_t ii = lo; ii < hi; ii++) {
                const float *row = rows_get(rows, ii, c0);
                double j = (double)ii - (double)yy;
                if (row == NULL) {
                    continue;
                }
                add_row(sb, sw, rows, ii, c0, 1.0);
                for (q = 0; q < TERMS; q++) {
                    double cs = cos(k->omega[q] * j), sn = sin(k->omega[q] * j);
                    for (i = 0; i < sw; i++) {
                        re[q * sw + i] += (double)row[i] * cs;
                        im[q * sw + i] += (double)row[i] * sn;
                    }
                }
            }
            have = true;
        }
        for (i = 0; i < sw; i++) {
            acc[i] = k->a0 * sb[i];
        }
        for (q = 0; q < TERMS; q++) {
            for (i = 0; i < sw; i++) {
                acc[i] += k->a[q] * re[q * sw + i];
            }
        }
        for (i = 0; i < sw; i++) {
            dst[i] = (float)acc[i];
        }
    }
done:
    free(flagv), free(flag), free(runs), free(sb), free(re), free(im), free(acc);
    return st;
}

/* The vertical pass over columns c0..c0 + sw (zero outside the window). */
static pc_status kernel_strip(const kernel *k, const rows_view *rows, size_t c0, size_t sw, float *out)
{
    size_t r, yy, i, t;
    if (k->is_cos) {
        return cos_strip(&k->c, rows, c0, sw, out);
    }
    r = k->ntaps / 2;
    for (yy = 0; yy < rows->gh && sw > 0; yy++) {
        float *dst = out + yy * sw;
        for (i = 0; i < k->ntaps; i++) {
            const float *src;
            if (yy + i < r) {
                continue;
            }
            src = rows_get(rows, yy + i - r, c0);
            if (src == NULL) {
                continue;
            }
            for (t = 0; t < sw; t++) {
                dst[t] += k->taps[i] * src[t];
            }
        }
    }
    return PC_OK;
}

static pc_status feather_rows(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const sm_job *j = vctx;
    size_t y, *runs = pc_malloc_array(j->gw + 1, sizeof *runs);
    (void)chunk;
    if (runs == NULL) {
        return PC_ERR_NOMEM;
    }
    for (y = begin; y < end; y++) {
        kernel_line(j->kh, j->m + (j->b.y0 + y) * j->w + j->wn.x0, j->tmp + y * j->gw, j->gw, runs);
    }
    free(runs);
    return PC_OK;
}

static pc_status feather_strips(void *vctx, size_t chunk, size_t begin, size_t end)
{
    const sm_job *j = vctx;
    size_t s;
    (void)chunk;
    for (s = begin; s < end; s++) {
        size_t c0 = s * STRIP, sw = STRIP < j->gw - c0 ? STRIP : j->gw - c0;
        pc_status st = kernel_strip(j->kv, j->rows, c0, sw, j->strips[s]);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

bool pc_algo_feather_kernel_is_cos(double sigma, size_t r, size_t n)
{
    kernel k;
    bool c = kernel_new(sigma, r, n, &k) == PC_OK && k.is_cos;
    kernel_fini(&k);
    return c;
}

pc_status pc_algo_selection_feather(const float *m, size_t len, size_t w, size_t h, float radius, float **out,
                                    pc_error *err)
{
    size_t n, r, ns = 0;
    float sigma, **strips = NULL, *res = NULL;
    win b, wn;
    kernel kh, kv;
    sm_job j;
    rows_view rows;
    asm_job aj;
    pc_status st;
    if (!pc_size_mul(w, h, &n)) {
        n = SIZE_MAX;
    }
    if (isfinite(radius)) {
        sigma = pc_rs_maxf_k(radius, 0.0f) / 2.0f;
    } else {
        sigma = radius > 0.0f ? MAX_SIGMA : 0.0f;
    }
    if (sigma < 0.1f || len != n || n == 0) {
        return copy_out(m, len, false, 0.0f, false, out, err);
    }
    sigma = pc_rs_minf_k(sigma, MAX_SIGMA);
    r = pc_f32_to_size(ceilf(sigma * 3.0f));
    if (!nonzero_bounds(m, w, h, &b)) {
        return copy_out(m, len, false, 0.0f, false, out, err);
    }
    wn = grow(b, r, w, h);
    memset(&kh, 0, sizeof kh);
    memset(&kv, 0, sizeof kv);
    if (kernel_new((double)sigma, r, win_w(wn), &kh) != PC_OK ||
        kernel_new((double)sigma, r, win_h(wn), &kv) != PC_OK) {
        kernel_fini(&kh);
        kernel_fini(&kv);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory for Feather");
    }
    memset(&j, 0, sizeof j);
    j.m = m;
    j.w = w;
    j.gw = win_w(wn);
    j.b = b;
    j.wn = wn;
    j.kh = &kh;
    j.kv = &kv;
    j.tmp = pc_calloc_array(win_h(b) * j.gw + 1, sizeof(float));
    res = pc_calloc_array(n, sizeof(float));
    strips = strips_new(j.gw, win_h(wn), &ns);
    if (j.tmp == NULL || res == NULL || strips == NULL) {
        st = PC_ERR_NOMEM;
        goto done;
    }
    /* Rows: only the bounding box's rows are non-zero. */
    st = pc_algo_par_for(win_h(b), 8, feather_rows, &j);
    rows.tmp = j.tmp;
    rows.gw = j.gw;
    rows.bh = win_h(b);
    rows.off = b.y0 - wn.y0;
    rows.gh = win_h(wn);
    j.rows = &rows;
    j.strips = strips;
    if (st == PC_OK) {
        st = pc_algo_par_for(ns, 1, feather_strips, &j);
    }
    if (st == PC_OK) {
        aj.strips = strips;
        aj.wn = wn;
        aj.w = w;
        aj.out = res;
        aj.feather = true;
        st = pc_algo_par_for(h, 64, assemble_rows, &aj);
    }
done:
    free(j.tmp);
    strips_free(strips, ns);
    kernel_fini(&kh);
    kernel_fini(&kv);
    if (st != PC_OK) {
        free(res);
        return pc_fail(err, st, "out of memory for Feather");
    }
    *out = res;
    return PC_OK;
}
