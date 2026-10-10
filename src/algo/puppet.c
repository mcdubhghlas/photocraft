/* Edit > Puppet Warp (port of crates/algo/src/puppet.rs): the mesh, the ARAP solver (banded
 * Cholesky with a conjugate-gradient fallback) and the deformation. Rendering is in
 * puppet_render.c, JSON in liquify_puppet_json.c.
 *
 * Every floating-point expression keeps the reference's operand order: the solver's sums are
 * sequential, so results are bit-identical to the reference whatever the thread count. */
#include "algo/puppet.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define DEG2RAD (3.14159265358979323846 / 180.0)
#define NONE SIZE_MAX

/* ---- enums and the warp description ---- */

static const char *const MODE_NAMES[3] = {"rigid", "normal", "distort"};
static const char *const DENSITY_NAMES[3] = {"fewer", "normal", "more"};
const pc_algo_enum_desc pc_algo_puppet_mode_desc = {"PuppetMode", MODE_NAMES, 3, PC_ALGO_PUPPET_NORMAL};
const pc_algo_enum_desc pc_algo_puppet_density_desc = {"PuppetDensity", DENSITY_NAMES, 3,
                                                       PC_ALGO_PUPPET_DENSITY_NORMAL};

static bool eq_ascii_ci(const char *s, size_t len, const char *lit)
{
    size_t i;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (lit[i] == '\0' || c != lit[i]) {
            return false;
        }
    }
    return lit[len] == '\0';
}

bool pc_algo_puppet_mode_parse(const char *s, size_t len, pc_algo_puppet_mode *out)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (eq_ascii_ci(s, len, MODE_NAMES[i])) {
            *out = (pc_algo_puppet_mode)i;
            return true;
        }
    }
    return false;
}

bool pc_algo_puppet_density_parse(const char *s, size_t len, pc_algo_puppet_density *out)
{
    if (eq_ascii_ci(s, len, "fewer") || eq_ascii_ci(s, len, "fewerpoints")) {
        *out = PC_ALGO_PUPPET_FEWER;
    } else if (eq_ascii_ci(s, len, "normal")) {
        *out = PC_ALGO_PUPPET_DENSITY_NORMAL;
    } else if (eq_ascii_ci(s, len, "more") || eq_ascii_ci(s, len, "morepoints")) {
        *out = PC_ALGO_PUPPET_MORE;
    } else {
        return false;
    }
    return true;
}

static double density_cells(pc_algo_puppet_density d)
{
    return d == PC_ALGO_PUPPET_FEWER ? 12.0 : (d == PC_ALGO_PUPPET_MORE ? 32.0 : 20.0);
}

pc_algo_puppet_warp pc_algo_puppet_warp_make(void)
{
    pc_algo_puppet_warp w;
    memset(&w, 0, sizeof w);
    w.mode = PC_ALGO_PUPPET_NORMAL;
    w.density = PC_ALGO_PUPPET_DENSITY_NORMAL;
    w.expansion = 2.0;
    return w;
}

void pc_algo_puppet_warp_fini(pc_algo_puppet_warp *w)
{
    if (w == NULL) {
        return;
    }
    free(w->pins);
    w->pins = NULL;
    w->npins = 0;
}

bool pc_algo_puppet_warp_is_identity(const pc_algo_puppet_warp *w)
{
    size_t i;
    for (i = 0; i < w->npins; i++) {
        const pc_algo_puppet_pin *p = &w->pins[i];
        if (!(p->src[0] == p->dst[0] && p->src[1] == p->dst[1])) {
            return false;
        }
        if (p->rotate.set) {
            double r = fmod(p->rotate.deg, 360.0); /* f64::rem_euclid */
            if (r < 0.0) {
                r += 360.0;
            }
            if (r != 0.0) {
                return false;
            }
        }
    }
    return true;
}

/* ---- mesh ---- */

void pc_algo_puppet_mesh_fini(pc_algo_puppet_mesh *m)
{
    if (m == NULL) {
        return;
    }
    free(m->verts);
    free(m->tris);
    memset(m, 0, sizeof *m);
}

bool pc_algo_puppet_mesh_eq(const pc_algo_puppet_mesh *a, const pc_algo_puppet_mesh *b)
{
    size_t i;
    if (a->nverts != b->nverts || a->ntris != b->ntris || a->spacing != b->spacing) {
        return false;
    }
    for (i = 0; i < a->nverts; i++) {
        if (a->verts[i][0] != b->verts[i][0] || a->verts[i][1] != b->verts[i][1]) {
            return false;
        }
    }
    return a->ntris == 0 || memcmp(a->tris, b->tris, a->ntris * sizeof *a->tris) == 0;
}

/* Coverage threshold: alpha above this counts as part of the puppet. */
#define ALPHA_MIN 0.05f

typedef struct cov_ctx {
    const pc_surface *src;
    pc_rect area, bounds;
    int32_t g;
    size_t bw, n;
    bool *cov;
    pc_error *errs;
} cov_ctx;

/* One block row of coverage (rows are independent). */
static pc_status cov_rows(void *ctx, size_t chunk, size_t begin, size_t end)
{
    const cov_ctx *c = ctx;
    pc_status first = PC_OK;
    size_t by;
    (void)chunk;
    for (by = begin; by < end; by++) {
        int32_t y0 = pc_i32_wrapping_add(c->area.y0, pc_i32_wrapping_mul((int32_t)by, c->g));
        pc_rect r = pc_rect_intersect(
            pc_rect_make(c->area.x0, y0,
                         pc_i32_wrapping_add(c->area.x0, pc_i32_wrapping_mul((int32_t)c->bw, c->g)),
                         pc_i32_wrapping_add(y0, c->g)),
            c->bounds);
        float *row = NULL;
        size_t len = 0, i, rw, n = c->n;
        pc_status st;
        if (pc_rect_is_empty(r) || !pc_surface_has_tiles_in(c->src, r)) {
            continue;
        }
        st = pc_surface_read_region(c->src, r, &row, &len, &c->errs[by]);
        if (st != PC_OK) {
            first = first == PC_OK ? st : first;
            continue;
        }
        rw = pc_rect_width(r);
        for (i = 0; i * n + n <= len; i++) {
            if (row[i * n + n - 1] > ALPHA_MIN) {
                int32_t x = r.x0 + (int32_t)(i % rw);
                size_t bx = (size_t)((x - c->area.x0) / c->g);
                if (bx < c->bw) {
                    c->cov[by * c->bw + bx] = true;
                }
            }
        }
        free(row);
    }
    return first;
}

/* Index of grid vertex k, creating it at (i, j) on first use. */
static pc_status mesh_vertex(pc_algo_puppet_mesh *m, size_t *cap, size_t *index, size_t k, size_t i, size_t j,
                             double ox, double oy, size_t *out)
{
    if (index[k] == NONE) {
        if (m->nverts == *cap) {
            void *nv = pc_grow_array(m->verts, cap, m->nverts + 1, sizeof *m->verts);
            if (nv == NULL) {
                return PC_ERR_NOMEM;
            }
            m->verts = nv;
        }
        index[k] = m->nverts;
        m->verts[m->nverts][0] = ox + (double)i * m->spacing;
        m->verts[m->nverts][1] = oy + (double)j * m->spacing;
        m->nverts++;
    }
    *out = index[k];
    return PC_OK;
}

pc_status pc_algo_puppet_build_mesh(const pc_surface *src, pc_rect bounds, pc_algo_puppet_density density,
                                    double expansion, pc_algo_puppet_mesh *out, pc_error *err)
{
    pc_algo_puppet_mesh mesh;
    double spacing, e, ox, oy;
    int32_t g, pad, bw_i, bh_i, eb;
    pc_rect area;
    size_t bw, bh, ncov, cw, ch, nidx, ci, cj, vcap = 0, tcap = 0;
    bool *cov = NULL, *src_cov = NULL;
    size_t *index = NULL;
    pc_pixel_format fmt;
    pc_status st = PC_OK;
    memset(&mesh, 0, sizeof mesh);
    if (pc_rect_is_empty(bounds)) {
        *out = mesh;
        return PC_OK;
    }
    {
        uint32_t w = pc_rect_width(bounds), h = pc_rect_height(bounds);
        spacing = pc_rs_max_k((double)(w > h ? w : h) / density_cells(density), 2.0);
    }
    e = pc_clampd(expansion, -200.0, 200.0);
    /* Coverage on a block grid (block = a quarter cell or less). */
    g = pc_f64_to_i32(floor(spacing / 4.0));
    g = g > 1 ? g : 1;
    pad = pc_i32_wrapping_add(pc_f64_to_i32(ceil(pc_rs_max_k(e, 0.0))), g);
    area = pc_rect_inflate(bounds, pad);
    bw_i = pc_i32_wrapping_sub(pc_i32_wrapping_add(pc_u32_as_i32(pc_rect_width(area)), g), 1) / g;
    bh_i = pc_i32_wrapping_sub(pc_i32_wrapping_add(pc_u32_as_i32(pc_rect_height(area)), g), 1) / g;
    /* A negative count is a huge usize in the reference, which then fails to allocate; a zero
     * one (widths past i32::MAX wrap) makes its par_chunks_mut(0) panic. */
    if (bw_i <= 0 || bh_i <= 0 || !pc_size_mul((size_t)bw_i, (size_t)bh_i, &ncov) ||
        ncov > PC_MAX_ALLOC / 2) {
        return pc_fail(err, PC_ERR_LIMIT, "puppet warp: the mesh grid over %ux%u px is too large",
                       pc_rect_width(bounds), pc_rect_height(bounds));
    }
    bw = (size_t)bw_i;
    bh = (size_t)bh_i;
    mesh.spacing = spacing;
    cov = pc_calloc_array(ncov > 0 ? ncov : 1, sizeof *cov);
    if (cov == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
        goto cleanup;
    }
    fmt = pc_surface_format(src);
    if (!fmt.alpha) {
        size_t bx, by;
        for (by = 0; by < bh; by++) {
            for (bx = 0; bx < bw; bx++) {
                int32_t x0 = pc_i32_wrapping_add(area.x0, pc_i32_wrapping_mul((int32_t)bx, g));
                int32_t y0 = pc_i32_wrapping_add(area.y0, pc_i32_wrapping_mul((int32_t)by, g));
                pc_rect r = pc_rect_make(x0, y0, pc_i32_wrapping_add(x0, g), pc_i32_wrapping_add(y0, g));
                cov[by * bw + bx] = !pc_rect_is_empty(pc_rect_intersect(r, bounds));
            }
        }
    } else {
        cov_ctx c;
        c.src = src;
        c.area = area;
        c.bounds = bounds;
        c.g = g;
        c.bw = bw;
        c.n = pc_surface_channels(src);
        c.cov = cov;
        c.errs = pc_calloc_array(bh > 0 ? bh : 1, sizeof *c.errs);
        if (c.errs == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
            goto cleanup;
        }
        st = pc_algo_par_for(bh, 1, cov_rows, &c);
        if (st != PC_OK) {
            size_t by;
            for (by = 0; by < bh; by++) {
                if (c.errs[by].code != PC_OK) {
                    if (err != NULL) {
                        *err = c.errs[by];
                    }
                    st = c.errs[by].code;
                    break;
                }
            }
        }
        free(c.errs);
        if (st != PC_OK) {
            goto cleanup;
        }
    }
    /* Grow / shrink by the expansion (round stamp in block units). */
    eb = pc_f64_to_i32(round(fabs(e) / (double)g));
    if (eb > 0) {
        bool grow = e > 0.0;
        int32_t bx, by, dx, dy;
        int64_t eb2 = (int64_t)eb * eb;
        src_cov = pc_malloc_array(ncov > 0 ? ncov : 1, sizeof *src_cov);
        if (src_cov == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
            goto cleanup;
        }
        memcpy(src_cov, cov, ncov * sizeof *cov);
        for (by = 0; by < bh_i; by++) {
            for (bx = 0; bx < bw_i; bx++) {
                bool hit = !grow, done = false;
                for (dy = -eb; dy <= eb && !done; dy++) {
                    for (dx = -eb; dx <= eb; dx++) {
                        int64_t x = (int64_t)bx + dx, y = (int64_t)by + dy;
                        bool v;
                        if ((int64_t)dx * dx + (int64_t)dy * dy > eb2) {
                            continue;
                        }
                        v = x >= 0 && y >= 0 && x < bw_i && y < bh_i && src_cov[(size_t)y * bw + (size_t)x];
                        if (grow && v) {
                            hit = true;
                            done = true;
                            break;
                        }
                        if (!grow && !v) {
                            hit = false;
                            done = true;
                            break;
                        }
                    }
                }
                cov[(size_t)by * bw + (size_t)bx] = hit;
            }
        }
    }
    /* Grid cells touching covered blocks. */
    ox = (double)area.x0;
    oy = (double)area.y0;
    cw = pc_f64_to_size(ceil((double)pc_i32_wrapping_mul(bw_i, g) / spacing));
    ch = pc_f64_to_size(ceil((double)pc_i32_wrapping_mul(bh_i, g) / spacing));
    if (!pc_size_mul(cw + 1, ch + 1, &nidx) || nidx > PC_MAX_ALLOC / sizeof *index) {
        st = pc_fail(err, PC_ERR_LIMIT, "puppet warp: the mesh grid is too large");
        goto cleanup;
    }
    index = pc_malloc_array(nidx, sizeof *index);
    if (index == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
        goto cleanup;
    }
    memset(index, 0xff, nidx * sizeof *index);
    for (cj = 0; cj < ch; cj++) {
        for (ci = 0; ci < cw; ci++) {
            double gd = (double)g;
            size_t bx0 = pc_f64_to_size(floor(((double)ci * spacing) / gd));
            size_t by0 = pc_f64_to_size(floor(((double)cj * spacing) / gd));
            size_t bx1 = pc_f64_to_size(pc_rs_min(ceil(((double)(ci + 1) * spacing) / gd), (double)bw));
            size_t by1 = pc_f64_to_size(pc_rs_min(ceil(((double)(cj + 1) * spacing) / gd), (double)bh));
            size_t x, y, a, b, c, d;
            bool any = false;
            for (y = by0; y < by1 && !any; y++) {
                for (x = bx0; x < bx1; x++) {
                    if (cov[y * bw + x]) {
                        any = true;
                        break;
                    }
                }
            }
            if (!any) {
                continue;
            }
            if (mesh_vertex(&mesh, &vcap, index, cj * (cw + 1) + ci, ci, cj, ox, oy, &a) != PC_OK ||
                mesh_vertex(&mesh, &vcap, index, cj * (cw + 1) + ci + 1, ci + 1, cj, ox, oy, &b) != PC_OK ||
                mesh_vertex(&mesh, &vcap, index, (cj + 1) * (cw + 1) + ci + 1, ci + 1, cj + 1, ox, oy, &c) !=
                    PC_OK ||
                mesh_vertex(&mesh, &vcap, index, (cj + 1) * (cw + 1) + ci, ci, cj + 1, ox, oy, &d) != PC_OK) {
                st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
                goto cleanup;
            }
            if (mesh.ntris + 2 > tcap) {
                void *nt = pc_grow_array(mesh.tris, &tcap, mesh.ntris + 2, sizeof *mesh.tris);
                if (nt == NULL) {
                    st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
                    goto cleanup;
                }
                mesh.tris = nt;
            }
            if ((ci + cj) % 2 == 0) {
                size_t t0[3] = {a, b, c}, t1[3] = {a, c, d};
                memcpy(mesh.tris[mesh.ntris++], t0, sizeof t0);
                memcpy(mesh.tris[mesh.ntris++], t1, sizeof t1);
            } else {
                size_t t0[3] = {a, b, d}, t1[3] = {b, c, d};
                memcpy(mesh.tris[mesh.ntris++], t0, sizeof t0);
                memcpy(mesh.tris[mesh.ntris++], t1, sizeof t1);
            }
        }
    }
cleanup:
    free(cov);
    free(src_cov);
    free(index);
    if (st != PC_OK) {
        pc_algo_puppet_mesh_fini(&mesh);
        return st;
    }
    *out = mesh;
    return PC_OK;
}

/* ---- sparse matrix (CSR, symmetric) with Jacobi-preconditioned CG ---- */

typedef struct trip {
    size_t i, j;
    double v;
} trip;

typedef struct csr {
    size_t n;
    size_t *rows; /* n + 1 */
    size_t *cols;
    double *vals;
    double *diag;
} csr;

static bool trip_le(const trip *a, const trip *b)
{
    return a->i < b->i || (a->i == b->i && a->j <= b->j);
}

/* Stable merge sort by (i, j): duplicates are summed in their original order, as the reference's
 * stable sort_by_key does, which fixes the rounding of the sums. */
static bool sort_trips(trip *t, size_t n)
{
    trip *tmp, *a = t, *b;
    size_t width;
    if (n < 2) {
        return true;
    }
    tmp = pc_malloc_array(n, sizeof *tmp);
    if (tmp == NULL) {
        return false;
    }
    b = tmp;
    for (width = 1; width < n; width *= 2) {
        size_t lo;
        for (lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n, hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                b[k++] = trip_le(&a[i], &a[j]) ? a[i++] : a[j++];
            }
            while (i < mid) {
                b[k++] = a[i++];
            }
            while (j < hi) {
                b[k++] = a[j++];
            }
        }
        {
            trip *s = a;
            a = b;
            b = s;
        }
        if (width > n / 2) {
            break;
        }
    }
    if (a != t) {
        memcpy(t, a, n * sizeof *t);
    }
    free(tmp);
    return true;
}

static void csr_fini(csr *m)
{
    free(m->rows);
    free(m->cols);
    free(m->vals);
    free(m->diag);
    memset(m, 0, sizeof *m);
}

static pc_status csr_from_triplets(size_t n, trip *t, size_t nt, csr *out)
{
    csr m;
    size_t lasti = NONE, lastj = NONE, k, i, nnz = 0;
    memset(&m, 0, sizeof m);
    m.n = n;
    if (!sort_trips(t, nt)) {
        return PC_ERR_NOMEM;
    }
    m.rows = pc_calloc_array(n + 1, sizeof *m.rows);
    m.cols = pc_malloc_array(nt > 0 ? nt : 1, sizeof *m.cols);
    m.vals = pc_malloc_array(nt > 0 ? nt : 1, sizeof *m.vals);
    m.diag = pc_malloc_array(n > 0 ? n : 1, sizeof *m.diag);
    if (m.rows == NULL || m.cols == NULL || m.vals == NULL || m.diag == NULL) {
        csr_fini(&m);
        return PC_ERR_NOMEM;
    }
    for (k = 0; k < nt; k++) {
        if (t[k].i == lasti && t[k].j == lastj) {
            m.vals[nnz - 1] += t[k].v;
            continue;
        }
        lasti = t[k].i;
        lastj = t[k].j;
        m.cols[nnz] = t[k].j;
        m.vals[nnz] = t[k].v;
        nnz++;
        m.rows[t[k].i + 1] = nnz;
    }
    for (i = 0; i < n; i++) {
        m.rows[i + 1] = m.rows[i + 1] > m.rows[i] ? m.rows[i + 1] : m.rows[i];
    }
    for (i = 0; i < n; i++) {
        m.diag[i] = 1.0;
        for (k = m.rows[i]; k < m.rows[i + 1]; k++) {
            if (m.cols[k] == i && m.vals[k] > 0.0) {
                m.diag[i] = m.vals[k];
            }
        }
    }
    *out = m;
    return PC_OK;
}

static void csr_mul(const csr *m, const double *x, double *out)
{
    size_t i, k;
    for (i = 0; i < m->n; i++) {
        double s = 0.0;
        for (k = m->rows[i]; k < m->rows[i + 1]; k++) {
            s += m->vals[k] * x[m->cols[k]];
        }
        out[i] = s;
    }
}

/* Workspace for one global step: 6 vectors of n. */
typedef struct work {
    double *r, *z, *p, *ap, *y, *bx, *by, *sx, *sy;
} work;

static double dot_sum(const double *a, const double *b, size_t n)
{
    double s = -0.0; /* Iterator::sum */
    size_t i;
    for (i = 0; i < n; i++) {
        s += a[i] * b[i];
    }
    return s;
}

/* Jacobi-preconditioned conjugate gradients, warm-started from x. */
static void csr_solve(const csr *m, const double *b, double *x, const work *w)
{
    size_t n = m->n, i, it, iters;
    double bn, rz;
    double *r = w->r, *z = w->z, *p = w->p, *ap = w->ap;
    csr_mul(m, x, r);
    for (i = 0; i < n; i++) {
        r[i] = b[i] - r[i];
    }
    bn = pc_rs_max_k(sqrt(dot_sum(b, b, n)), 1e-30);
    for (i = 0; i < n; i++) {
        z[i] = r[i] / m->diag[i];
        p[i] = z[i];
    }
    rz = dot_sum(r, z, n);
    iters = n > SIZE_MAX / 4 ? 2000 : 4 * n;
    iters = iters < 50 ? 50 : (iters > 2000 ? 2000 : iters);
    for (it = 0; it < iters; it++) {
        double pap, alpha, rz2, beta;
        if (sqrt(dot_sum(r, r, n)) <= 1e-12 * bn) {
            break;
        }
        csr_mul(m, p, ap);
        pap = dot_sum(p, ap, n);
        if (fabs(pap) < 1e-300) {
            break;
        }
        alpha = rz / pap;
        for (i = 0; i < n; i++) {
            x[i] += alpha * p[i];
            r[i] -= alpha * ap[i];
        }
        for (i = 0; i < n; i++) {
            z[i] = r[i] / m->diag[i];
        }
        rz2 = dot_sum(r, z, n);
        beta = rz2 / rz;
        rz = rz2;
        for (i = 0; i < n; i++) {
            p[i] = z[i] + beta * p[i];
        }
    }
}

/* Banded Cholesky factor A = L L^T: row i holds L[i][i - p ..= i]. */
typedef struct band_chol {
    size_t n, p;
    double *l;
} band_chol;

/* False (and no factor) when the band is too large or the matrix isn't positive definite; NOMEM
 * is reported through *st. */
static bool band_chol_new(const csr *a, band_chol *out, pc_status *st)
{
    size_t n = a->n, p = 0, i, k, w, j, total;
    double *l;
    *st = PC_OK;
    for (i = 0; i < n; i++) {
        for (k = a->rows[i]; k < a->rows[i + 1]; k++) {
            size_t d = i > a->cols[k] ? i - a->cols[k] : a->cols[k] - i;
            p = p > d ? p : d;
        }
    }
    if (!pc_size_mul(n, p + 1, &total) || total > 40000000u) {
        return false;
    }
    w = p + 1;
    l = pc_calloc_array(total > 0 ? total : 1, sizeof *l);
    if (l == NULL) {
        *st = PC_ERR_NOMEM;
        return false;
    }
    for (i = 0; i < n; i++) {
        for (k = a->rows[i]; k < a->rows[i + 1]; k++) {
            j = a->cols[k];
            if (j <= i) {
                l[i * w + (j + p - i)] = a->vals[k];
            }
        }
    }
    for (i = 0; i < n; i++) {
        size_t lo = i > p ? i - p : 0;
        for (j = lo; j <= i; j++) {
            size_t jlo = j > p ? j - p : 0;
            double s = l[i * w + (j + p - i)];
            jlo = jlo > lo ? jlo : lo;
            for (k = jlo; k < j; k++) {
                s -= l[i * w + (k + p - i)] * l[j * w + (k + p - j)];
            }
            if (i == j) {
                if (s <= 0.0 || !isfinite(s)) {
                    free(l);
                    return false;
                }
                l[i * w + p] = sqrt(s);
            } else {
                l[i * w + (j + p - i)] = s / l[j * w + p];
            }
        }
    }
    out->n = n;
    out->p = p;
    out->l = l;
    return true;
}

static void band_chol_solve(const band_chol *c, const double *b, double *x, double *y)
{
    size_t n = c->n, p = c->p, w = c->p + 1, i, k;
    memcpy(y, b, n * sizeof *y);
    for (i = 0; i < n; i++) {
        double s = y[i];
        for (k = i > p ? i - p : 0; k < i; k++) {
            s -= c->l[i * w + (k + p - i)] * y[k];
        }
        y[i] = s / c->l[i * w + p];
    }
    for (i = n; i-- > 0;) {
        double s = y[i];
        size_t end = i + p + 1 < n ? i + p + 1 : n;
        for (k = i + 1; k < end; k++) {
            s -= c->l[k * w + (i + p - k)] * x[k];
        }
        x[i] = s / c->l[i * w + p];
    }
}

/* ---- solver ---- */

/* A pin bound to the mesh: barycentric weights on a triangle's vertices. */
typedef struct bound_pin {
    size_t verts[3];
    double bary[3];
} bound_pin;

struct pc_algo_puppet_solver {
    pc_algo_puppet_mesh mesh;
    double (*w)[3]; /* cotangent weight of the edge opposite corner k */
    bound_pin *bound;
    size_t nbound;
    size_t **pin_tris; /* per bound pin: triangles whose rotation it fixes */
    size_t *npin_tris;
    bool *anchored; /* vertices of components without pins (held at rest) */
    csr matrix;
    bool has_chol;
    band_chol chol;
    double pin_weight;
};

static double cot(const double *a, const double *b, const double *c)
{
    /* Cotangent of the angle at a. */
    double u0 = b[0] - a[0], u1 = b[1] - a[1], v0 = c[0] - a[0], v1 = c[1] - a[1];
    double cross = fabs(u0 * v1 - u1 * v0);
    return cross < 1e-12 ? 0.0 : (u0 * v0 + u1 * v1) / cross;
}

static double dist2(const double *a, const double *b)
{
    double dx = a[0] - b[0], dy = a[1] - b[1];
    return dx * dx + dy * dy;
}

/* f64::total_cmp(a, b) < 0. */
static bool total_lt(double a, double b)
{
    int64_t ia, ib;
    memcpy(&ia, &a, 8);
    memcpy(&ib, &b, 8);
    ia ^= (int64_t)((uint64_t)(ia >> 63) >> 1);
    ib ^= (int64_t)((uint64_t)(ib >> 63) >> 1);
    return ia < ib;
}

static bound_pin locate(const pc_algo_puppet_mesh *m, const double *p)
{
    bound_pin b;
    size_t t, i, best = 0;
    for (t = 0; t < m->ntris; t++) {
        const double *a = m->verts[m->tris[t][0]], *bb = m->verts[m->tris[t][1]],
                     *c = m->verts[m->tris[t][2]];
        double det = (bb[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (bb[1] - a[1]), l0, l1, l2;
        if (fabs(det) < 1e-12) {
            continue;
        }
        l1 = ((p[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (p[1] - a[1])) / det;
        l2 = ((bb[0] - a[0]) * (p[1] - a[1]) - (p[0] - a[0]) * (bb[1] - a[1])) / det;
        l0 = 1.0 - l1 - l2;
        if (l0 >= -1e-9 && l1 >= -1e-9 && l2 >= -1e-9) {
            memcpy(b.verts, m->tris[t], sizeof b.verts);
            b.bary[0] = l0;
            b.bary[1] = l1;
            b.bary[2] = l2;
            return b;
        }
    }
    /* Outside the mesh: the nearest vertex (the first of equals). */
    for (i = 1; i < m->nverts; i++) {
        if (total_lt(dist2(m->verts[i], p), dist2(m->verts[best], p))) {
            best = i;
        }
    }
    b.verts[0] = b.verts[1] = b.verts[2] = best;
    b.bary[0] = 1.0;
    b.bary[1] = 0.0;
    b.bary[2] = 0.0;
    return b;
}

static size_t uf_find(size_t *p, size_t i)
{
    while (p[i] != i) {
        p[i] = p[p[i]];
        i = p[i];
    }
    return i;
}

void pc_algo_puppet_solver_free(pc_algo_puppet_solver *s)
{
    size_t k;
    if (s == NULL) {
        return;
    }
    pc_algo_puppet_mesh_fini(&s->mesh);
    free(s->w);
    free(s->bound);
    if (s->pin_tris != NULL) {
        for (k = 0; k < s->nbound; k++) {
            free(s->pin_tris[k]);
        }
    }
    free(s->pin_tris);
    free(s->npin_tris);
    free(s->anchored);
    csr_fini(&s->matrix);
    free(s->chol.l);
    free(s);
}

pc_status pc_algo_puppet_solver_new(pc_algo_puppet_mesh *mesh, const double (*pin_src)[2], size_t npins,
                                    const bool *fixed_rotation, size_t nfixed, pc_algo_puppet_solver **out,
                                    pc_error *err)
{
    pc_algo_puppet_solver *s = calloc(1, sizeof *s);
    size_t nv = mesh->nverts, nt = mesh->ntris, t, k, v, ntrip = 0, cap;
    size_t *parent = NULL;
    bool *pinned_root = NULL;
    trip *tr = NULL;
    double diag_sum = 0.0;
    pc_status st;
    if (s == NULL) {
        pc_algo_puppet_mesh_fini(mesh);
        return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    }
    s->mesh = *mesh;
    memset(mesh, 0, sizeof *mesh);
    s->w = pc_malloc_array(nt > 0 ? nt : 1, sizeof *s->w);
    /* Without vertices there is nothing to bind a pin to (#951): the pins are then ignored. */
    s->nbound = nv == 0 ? 0 : npins;
    s->bound = pc_malloc_array(s->nbound > 0 ? s->nbound : 1, sizeof *s->bound);
    s->pin_tris = pc_calloc_array(s->nbound > 0 ? s->nbound : 1, sizeof *s->pin_tris);
    s->npin_tris = pc_calloc_array(s->nbound > 0 ? s->nbound : 1, sizeof *s->npin_tris);
    s->anchored = pc_malloc_array(nv > 0 ? nv : 1, sizeof *s->anchored);
    parent = pc_malloc_array(nv > 0 ? nv : 1, sizeof *parent);
    pinned_root = pc_calloc_array(nv > 0 ? nv : 1, sizeof *pinned_root);
    if (s->w == NULL || s->bound == NULL || s->pin_tris == NULL || s->npin_tris == NULL ||
        s->anchored == NULL || parent == NULL || pinned_root == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
        goto fail;
    }
    for (t = 0; t < nt; t++) {
        const double *a = s->mesh.verts[s->mesh.tris[t][0]], *b = s->mesh.verts[s->mesh.tris[t][1]];
        const double *c = s->mesh.verts[s->mesh.tris[t][2]];
        s->w[t][0] = 0.5 * cot(a, b, c);
        s->w[t][1] = 0.5 * cot(b, c, a);
        s->w[t][2] = 0.5 * cot(c, a, b);
    }
    for (k = 0; k < s->nbound; k++) {
        s->bound[k] = locate(&s->mesh, pin_src[k]);
    }
    /* Connected components (union-find over triangle edges). */
    for (v = 0; v < nv; v++) {
        parent[v] = v;
    }
    for (t = 0; t < nt; t++) {
        for (k = 1; k < 3; k++) {
            size_t a = uf_find(parent, s->mesh.tris[t][0]), b = uf_find(parent, s->mesh.tris[t][k]);
            if (a != b) {
                parent[a] = b;
            }
        }
    }
    for (k = 0; k < s->nbound; k++) {
        for (v = 0; v < 3; v++) {
            pinned_root[uf_find(parent, s->bound[k].verts[v])] = true;
        }
    }
    for (v = 0; v < nv; v++) {
        s->anchored[v] = !pinned_root[uf_find(parent, v)];
    }
    /* Rotation-fixed pins: the triangles around the pin's nearest vertex. */
    for (k = 0; k < s->nbound; k++) {
        const bound_pin *b = &s->bound[k];
        size_t near = b->verts[0], cnt = 0, q;
        if (!(k < nfixed && fixed_rotation[k])) {
            continue;
        }
        for (q = 1; q < 3; q++) {
            if (total_lt(dist2(s->mesh.verts[b->verts[q]], pin_src[k]),
                         dist2(s->mesh.verts[near], pin_src[k]))) {
                near = b->verts[q];
            }
        }
        for (t = 0; t < nt; t++) {
            cnt += (s->mesh.tris[t][0] == near || s->mesh.tris[t][1] == near || s->mesh.tris[t][2] == near);
        }
        s->pin_tris[k] = pc_malloc_array(cnt > 0 ? cnt : 1, sizeof **s->pin_tris);
        if (s->pin_tris[k] == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
            goto fail;
        }
        for (t = 0; t < nt; t++) {
            if (s->mesh.tris[t][0] == near || s->mesh.tris[t][1] == near || s->mesh.tris[t][2] == near) {
                s->pin_tris[k][s->npin_tris[k]++] = t;
            }
        }
    }
    if (nt > (PC_MAX_ALLOC / sizeof *tr) / 12 || s->nbound > (PC_MAX_ALLOC / sizeof *tr) / 9) {
        st = pc_fail(err, PC_ERR_LIMIT, "puppet warp: too many triangles or pins");
        goto fail;
    }
    cap = nt * 12 + s->nbound * 9 + nv;
    tr = pc_malloc_array(cap > 0 ? cap : 1, sizeof *tr);
    if (tr == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
        goto fail;
    }
    for (t = 0; t < nt; t++) {
        for (k = 0; k < 3; k++) {
            size_t i = s->mesh.tris[t][(k + 1) % 3], j = s->mesh.tris[t][(k + 2) % 3];
            double wk = s->w[t][k];
            tr[ntrip++] = (trip){i, i, wk};
            tr[ntrip++] = (trip){j, j, wk};
            tr[ntrip++] = (trip){i, j, -wk};
            tr[ntrip++] = (trip){j, i, -wk};
            diag_sum += 2.0 * wk;
        }
    }
    s->pin_weight = 1e4 * pc_rs_max_k(diag_sum / (double)(nv > 1 ? nv : 1), 1.0);
    for (k = 0; k < s->nbound; k++) {
        size_t a, c;
        for (a = 0; a < 3; a++) {
            for (c = 0; c < 3; c++) {
                tr[ntrip++] = (trip){s->bound[k].verts[a], s->bound[k].verts[c],
                                     s->pin_weight * s->bound[k].bary[a] * s->bound[k].bary[c]};
            }
        }
    }
    for (v = 0; v < nv; v++) {
        if (s->anchored[v]) {
            tr[ntrip++] = (trip){v, v, s->pin_weight};
        }
    }
    if (csr_from_triplets(nv, tr, ntrip, &s->matrix) != PC_OK) {
        st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
        goto fail;
    }
    s->has_chol = band_chol_new(&s->matrix, &s->chol, &st);
    if (st != PC_OK) {
        st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
        goto fail;
    }
    free(tr);
    free(parent);
    free(pinned_root);
    *out = s;
    return PC_OK;
fail:
    free(tr);
    free(parent);
    free(pinned_root);
    pc_algo_puppet_solver_free(s);
    return st;
}

const pc_algo_puppet_mesh *pc_algo_puppet_solver_mesh(const pc_algo_puppet_solver *s)
{
    return &s->mesh;
}

bool pc_algo_puppet_solver_factored(const pc_algo_puppet_solver *s)
{
    return s->has_chol;
}

size_t pc_algo_puppet_solver_bound_count(const pc_algo_puppet_solver *s)
{
    return s->nbound;
}

static void bary_point(const bound_pin *b, const double (*v)[2], double out[2])
{
    double a0 = 0.0, a1 = 0.0;
    int k;
    for (k = 0; k < 3; k++) {
        a0 = a0 + b->bary[k] * v[b->verts[k]][0];
        a1 = a1 + b->bary[k] * v[b->verts[k]][1];
    }
    out[0] = a0;
    out[1] = a1;
}

bool pc_algo_puppet_solver_pin_point(const pc_algo_puppet_solver *s, size_t k, const double (*v)[2],
                                     double out[2])
{
    if (k >= s->nbound) {
        return false;
    }
    bary_point(&s->bound[k], v, out);
    return true;
}

/* The ARAP energy of v for the given per-triangle linear maps (plus the pin terms). */
static double energy_with(const pc_algo_puppet_solver *s, const double (*v)[2], const double (*rots)[4],
                          const double (*pin_dst)[2], size_t ndst)
{
    const double (*x)[2] = (const double (*)[2])s->mesh.verts;
    double e = 0.0;
    size_t t, k, nb = s->nbound < ndst ? s->nbound : ndst;
    for (t = 0; t < s->mesh.ntris; t++) {
        const double *r = rots[t];
        for (k = 0; k < 3; k++) {
            size_t i = s->mesh.tris[t][(k + 1) % 3], j = s->mesh.tris[t][(k + 2) % 3];
            double d0 = v[i][0] - v[j][0], d1 = v[i][1] - v[j][1];
            double ex0 = x[i][0] - x[j][0], ex1 = x[i][1] - x[j][1];
            double re0 = r[0] * ex0 + r[1] * ex1, re1 = r[2] * ex0 + r[3] * ex1;
            double q0 = d0 - re0, q1 = d1 - re1;
            e += s->w[t][k] * (q0 * q0 + q1 * q1);
        }
    }
    for (k = 0; k < nb; k++) {
        double p[2];
        bary_point(&s->bound[k], v, p);
        e += s->pin_weight * dist2(p, pin_dst[k]);
    }
    return e;
}

static void local_step(const pc_algo_puppet_solver *s, const double (*v)[2], pc_algo_puppet_mode mode,
                       const pc_algo_puppet_rot *fixed, size_t nfixed, double (*rots)[4])
{
    const double (*x)[2] = (const double (*)[2])s->mesh.verts;
    size_t t, k;
    for (t = 0; t < s->mesh.ntris; t++) {
        double sm[4] = {0.0, 0.0, 0.0, 0.0}, norm = 0.0, c, sn, th, scale, sth, cth;
        for (k = 0; k < 3; k++) {
            size_t i = s->mesh.tris[t][(k + 1) % 3], j = s->mesh.tris[t][(k + 2) % 3];
            double d0 = v[i][0] - v[j][0], d1 = v[i][1] - v[j][1];
            double e0 = x[i][0] - x[j][0], e1 = x[i][1] - x[j][1], wk = s->w[t][k];
            sm[0] += wk * d0 * e0;
            sm[1] += wk * d0 * e1;
            sm[2] += wk * d1 * e0;
            sm[3] += wk * d1 * e1;
            norm += wk * (e0 * e0 + e1 * e1);
        }
        c = sm[0] + sm[3];
        sn = sm[2] - sm[1];
        th = atan2(sn, c);
        if (mode == PC_ALGO_PUPPET_RIGID) {
            scale = 1.0;
        } else if (mode == PC_ALGO_PUPPET_DISTORT) {
            scale = hypot(c, sn) / pc_rs_max_k(norm, 1e-12);
        } else {
            scale = sqrt(hypot(c, sn) / pc_rs_max_k(norm, 1e-12));
        }
        scale = isfinite(scale) && scale > 0.0 ? scale : 1.0;
        sth = sin(th);
        cth = cos(th);
        rots[t][0] = scale * cth;
        rots[t][1] = -scale * sth;
        rots[t][2] = scale * sth;
        rots[t][3] = scale * cth;
    }
    for (k = 0; k < s->nbound; k++) {
        if (k < nfixed && fixed[k].set) {
            double rad = fixed[k].deg * DEG2RAD, sv = sin(rad), cv = cos(rad);
            size_t q;
            for (q = 0; q < s->npin_tris[k]; q++) {
                double *r = rots[s->pin_tris[k][q]];
                r[0] = cv;
                r[1] = -sv;
                r[2] = sv;
                r[3] = cv;
            }
        }
    }
}

static void global_step(const pc_algo_puppet_solver *s, const double (*rots)[4], const double (*pin_dst)[2],
                        size_t ndst, double (*v)[2], const work *w)
{
    const double (*x)[2] = (const double (*)[2])s->mesh.verts;
    size_t nv = s->mesh.nverts, t, k, i, nb = s->nbound < ndst ? s->nbound : ndst;
    double *bx = w->bx, *by = w->by;
    for (i = 0; i < nv; i++) {
        bx[i] = 0.0;
        by[i] = 0.0;
    }
    for (t = 0; t < s->mesh.ntris; t++) {
        const double *r = rots[t];
        for (k = 0; k < 3; k++) {
            size_t a = s->mesh.tris[t][(k + 1) % 3], b = s->mesh.tris[t][(k + 2) % 3];
            double e0 = x[a][0] - x[b][0], e1 = x[a][1] - x[b][1], wk = s->w[t][k];
            double re0 = r[0] * e0 + r[1] * e1, re1 = r[2] * e0 + r[3] * e1;
            bx[a] += wk * re0;
            by[a] += wk * re1;
            bx[b] -= wk * re0;
            by[b] -= wk * re1;
        }
    }
    for (k = 0; k < nb; k++) {
        int q;
        for (q = 0; q < 3; q++) {
            bx[s->bound[k].verts[q]] += s->pin_weight * s->bound[k].bary[q] * pin_dst[k][0];
            by[s->bound[k].verts[q]] += s->pin_weight * s->bound[k].bary[q] * pin_dst[k][1];
        }
    }
    for (i = 0; i < nv; i++) {
        if (s->anchored[i]) {
            bx[i] += s->pin_weight * x[i][0];
            by[i] += s->pin_weight * x[i][1];
        }
    }
    for (i = 0; i < nv; i++) {
        w->sx[i] = v[i][0];
        w->sy[i] = v[i][1];
    }
    if (s->has_chol) {
        band_chol_solve(&s->chol, bx, w->sx, w->y);
        band_chol_solve(&s->chol, by, w->sy, w->y);
    } else {
        csr_solve(&s->matrix, bx, w->sx, w);
        csr_solve(&s->matrix, by, w->sy, w);
    }
    for (i = 0; i < nv; i++) {
        v[i][0] = w->sx[i];
        v[i][1] = w->sy[i];
    }
}

static void work_free(work *w)
{
    free(w->r);
    memset(w, 0, sizeof *w);
}

static bool work_alloc(work *w, size_t n)
{
    double *all = pc_malloc_array(n > 0 ? n * 9 : 1, sizeof *all);
    memset(w, 0, sizeof *w);
    if (all == NULL || n > SIZE_MAX / 9) {
        free(all);
        return false;
    }
    w->r = all;
    w->z = all + n;
    w->p = all + 2 * n;
    w->ap = all + 3 * n;
    w->y = all + 4 * n;
    w->bx = all + 5 * n;
    w->by = all + 6 * n;
    w->sx = all + 7 * n;
    w->sy = all + 8 * n;
    return true;
}

pc_status pc_algo_puppet_solve(const pc_algo_puppet_solver *s, const double (*pin_dst)[2], size_t ndst,
                               const pc_algo_puppet_rot *rotations, size_t nrot, pc_algo_puppet_mode mode,
                               size_t iterations, const double (*init)[2], size_t ninit, double (**v_out)[2],
                               double **energies, pc_error *err)
{
    size_t nv = s->mesh.nverts, nt = s->mesh.ntris, t, it, k;
    double (*v)[2] = NULL, (*rots)[4] = NULL, *en = NULL;
    work w;
    bool warm = init != NULL && ninit == nv;
    if (iterations > PC_ALGO_PUPPET_MAX_ITERATIONS) {
        return pc_fail(err, PC_ERR_LIMIT, "puppet warp: %zu iterations is more than %d", iterations,
                       PC_ALGO_PUPPET_MAX_ITERATIONS);
    }
    memset(&w, 0, sizeof w);
    v = pc_malloc_array(nv > 0 ? nv : 1, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    }
    if (nv > 0) {
        memcpy(v, warm ? init : (const double (*)[2])s->mesh.verts, nv * sizeof *v);
    }
    if (nv == 0) {
        *v_out = v;
        *energies = NULL;
        return PC_OK;
    }
    rots = pc_malloc_array(nt > 0 ? nt : 1, sizeof *rots);
    en = pc_malloc_array(iterations > 0 ? iterations : 1, sizeof *en);
    if (rots == NULL || en == NULL || !work_alloc(&w, nv)) {
        free(v);
        free(rots);
        free(en);
        return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    }
    if (!warm) {
        /* Start from R = I, or with rotated pins, each triangle turned by the inverse-distance
         * weighted angle of the rotated pins (ARAP alone propagates a turn only slowly). */
        size_t nb = s->nbound < nrot ? s->nbound : nrot, nrotated = 0;
        double (*rp)[3] = pc_malloc_array(nb > 0 ? nb : 1, sizeof *rp);
        if (rp == NULL) {
            free(v);
            free(rots);
            free(en);
            work_free(&w);
            return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
        }
        for (k = 0; k < nb; k++) {
            if (rotations[k].set) {
                double p[2];
                bary_point(&s->bound[k], (const double (*)[2])s->mesh.verts, p);
                rp[nrotated][0] = p[0];
                rp[nrotated][1] = p[1];
                rp[nrotated][2] = rotations[k].deg * DEG2RAD;
                nrotated++;
            }
        }
        for (t = 0; t < nt; t++) {
            double c[2] = {0.0, 0.0}, sw = 0.0, sa = 0.0, sv, cv;
            int q;
            if (nrotated == 0) {
                rots[t][0] = 1.0;
                rots[t][1] = 0.0;
                rots[t][2] = 0.0;
                rots[t][3] = 1.0;
                continue;
            }
            for (q = 0; q < 3; q++) {
                c[0] = c[0] + s->mesh.verts[s->mesh.tris[t][q]][0] / 3.0;
                c[1] = c[1] + s->mesh.verts[s->mesh.tris[t][q]][1] / 3.0;
            }
            for (k = 0; k < nrotated; k++) {
                double wt = 1.0 / (dist2(rp[k], c) + 1.0);
                sw += wt;
                sa += wt * rp[k][2];
            }
            sv = sin(sa / sw);
            cv = cos(sa / sw);
            rots[t][0] = cv;
            rots[t][1] = -sv;
            rots[t][2] = sv;
            rots[t][3] = cv;
        }
        free(rp);
        global_step(s, (const double (*)[4])rots, pin_dst, ndst, v, &w);
    }
    for (it = 0; it < iterations; it++) {
        local_step(s, (const double (*)[2])v, mode, rotations, nrot, rots);
        global_step(s, (const double (*)[4])rots, pin_dst, ndst, v, &w);
        local_step(s, (const double (*)[2])v, PC_ALGO_PUPPET_RIGID, rotations, nrot, rots);
        en[it] = energy_with(s, (const double (*)[2])v, (const double (*)[4])rots, pin_dst, ndst);
    }
    free(rots);
    work_free(&w);
    if (iterations == 0) {
        free(en);
        en = NULL;
    }
    *v_out = v;
    *energies = en;
    return PC_OK;
}

pc_status pc_algo_puppet_energy(const pc_algo_puppet_solver *s, const double (*v)[2],
                                const double (*pin_dst)[2], size_t ndst, const pc_algo_puppet_rot *rotations,
                                size_t nrot, double *out, pc_error *err)
{
    size_t nt = s->mesh.ntris;
    double (*rots)[4] = pc_malloc_array(nt > 0 ? nt : 1, sizeof *rots);
    if (rots == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    }
    local_step(s, v, PC_ALGO_PUPPET_RIGID, rotations, nrot, rots);
    *out = energy_with(s, v, (const double (*)[4])rots, pin_dst, ndst);
    free(rots);
    return PC_OK;
}

typedef struct order_key {
    int32_t depth;
    size_t t;
} order_key;

static int cmp_order(const void *pa, const void *pb)
{
    const order_key *a = pa, *b = pb;
    if (a->depth != b->depth) {
        return a->depth < b->depth ? -1 : 1;
    }
    return a->t < b->t ? -1 : (a->t > b->t ? 1 : 0); /* stable: original order */
}

pc_status pc_algo_puppet_draw_order(const pc_algo_puppet_solver *s, const int32_t *depths, size_t ndepths,
                                    const double (*pin_src)[2], size_t npins, size_t **order, pc_error *err)
{
    size_t nt = s->mesh.ntris, t, k;
    size_t *o = pc_malloc_array(nt > 0 ? nt : 1, sizeof *o);
    order_key *keys;
    bool all_zero = true;
    if (o == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    }
    for (t = 0; t < nt; t++) {
        o[t] = t;
    }
    for (k = 0; k < ndepths; k++) {
        all_zero = all_zero && depths[k] == 0;
    }
    if (all_zero || npins == 0 || nt == 0) {
        *order = o;
        return PC_OK;
    }
    keys = pc_malloc_array(nt, sizeof *keys);
    if (keys == NULL) {
        free(o);
        return pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
    }
    for (t = 0; t < nt; t++) {
        double c[2] = {0.0, 0.0};
        size_t best = 0;
        int q;
        for (q = 0; q < 3; q++) {
            c[0] = c[0] + s->mesh.verts[s->mesh.tris[t][q]][0] / 3.0;
            c[1] = c[1] + s->mesh.verts[s->mesh.tris[t][q]][1] / 3.0;
        }
        for (k = 1; k < npins; k++) {
            if (total_lt(dist2(pin_src[k], c), dist2(pin_src[best], c))) {
                best = k;
            }
        }
        keys[t].depth = best < ndepths ? depths[best] : 0;
        keys[t].t = t;
    }
    qsort(keys, nt, sizeof *keys, cmp_order);
    for (t = 0; t < nt; t++) {
        o[t] = keys[t].t;
    }
    free(keys);
    *order = o;
    return PC_OK;
}

pc_status pc_algo_puppet_deform(const pc_surface *src, pc_rect bounds, const pc_algo_puppet_warp *w,
                                size_t iterations, pc_algo_puppet_solver **solver, double (**v)[2],
                                size_t **order, pc_error *err)
{
    pc_algo_puppet_mesh mesh;
    pc_algo_puppet_solver *s = NULL;
    double (*pin_src)[2] = NULL, (*dst)[2] = NULL, (*vv)[2] = NULL, *en = NULL;
    bool *fixed = NULL;
    pc_algo_puppet_rot *rot = NULL;
    int32_t *depths = NULL;
    size_t n = w->npins, k, *ord = NULL;
    pc_status st = pc_algo_puppet_build_mesh(src, bounds, w->density, w->expansion, &mesh, err);
    if (st != PC_OK) {
        return st;
    }
    pin_src = pc_malloc_array(n > 0 ? n : 1, sizeof *pin_src);
    dst = pc_malloc_array(n > 0 ? n : 1, sizeof *dst);
    fixed = pc_malloc_array(n > 0 ? n : 1, sizeof *fixed);
    rot = pc_malloc_array(n > 0 ? n : 1, sizeof *rot);
    depths = pc_malloc_array(n > 0 ? n : 1, sizeof *depths);
    if (pin_src == NULL || dst == NULL || fixed == NULL || rot == NULL || depths == NULL) {
        pc_algo_puppet_mesh_fini(&mesh);
        st = pc_fail(err, PC_ERR_NOMEM, "puppet warp: out of memory");
        goto cleanup;
    }
    for (k = 0; k < n; k++) {
        memcpy(pin_src[k], w->pins[k].src, sizeof pin_src[k]);
        memcpy(dst[k], w->pins[k].dst, sizeof dst[k]);
        fixed[k] = w->pins[k].rotate.set;
        rot[k] = w->pins[k].rotate;
        depths[k] = w->pins[k].depth;
    }
    st = pc_algo_puppet_solver_new(&mesh, (const double (*)[2])pin_src, n, fixed, n, &s, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    st = pc_algo_puppet_solve(s, (const double (*)[2])dst, n, rot, n, w->mode, iterations, NULL, 0, &vv, &en,
                              err);
    if (st != PC_OK) {
        goto cleanup;
    }
    st = pc_algo_puppet_draw_order(s, depths, n, (const double (*)[2])pin_src, n, &ord, err);
cleanup:
    free(pin_src);
    free(dst);
    free(fixed);
    free(rot);
    free(depths);
    free(en);
    if (st != PC_OK) {
        pc_algo_puppet_solver_free(s);
        free(vv);
        return st;
    }
    *solver = s;
    *v = vv;
    *order = ord;
    return PC_OK;
}
