/* Interactive Bezier mesh edits: pull a surface point, drag a control point (port of
 * BezierMesh::pull / drag_point / smooth_lines in crates/geom/src/warp.rs). Expressions mirror the
 * reference operation for operation, so results are bit-identical. */
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "warp.h"
#include "warp_internal.h"

/* std::f64::consts::PI (M_PI is not ISO C). */
static const double PI = 3.14159265358979323846;

static void add_scaled(pc_point *p, pc_point d, double k)
{
    p->x += d.x * k;
    p->y += d.y * k;
}

/* Makes the tangent equal on both sides of every interior grid line, for the control points in
 * columns i0..i1 and rows j0..j1 (exclusive ends, clamped to the grid): each pair of handles takes
 * the average of the two tangents it had. */
static void smooth_lines(pc_mesh *m, size_t i0, size_t i1, size_t j0, size_t j1)
{
    size_t nx = pc_mesh_nx(m);
    size_t ny = pc_mesh_ny(m);
    size_t a;
    size_t i;
    size_t j;
    if (i1 > nx) {
        i1 = nx;
    }
    if (j1 > ny) {
        j1 = ny;
    }
    /* Vertical lines first, then horizontal ones (the reference's order). */
    for (a = 1; a + 1 < m->nus; a++) {
        if (3 * a >= i0 && 3 * a < i1) {
            double hl = m->us[a] - m->us[a - 1];
            double hr = m->us[a + 1] - m->us[a];
            for (j = j0; j < j1; j++) {
                pc_point *pl = &m->points[j * nx + 3 * a - 1];
                pc_point pc = m->points[j * nx + 3 * a];
                pc_point *pr = &m->points[j * nx + 3 * a + 1];
                double tx = ((pc.x - pl->x) / hl + (pr->x - pc.x) / hr) / 2.0;
                double ty = ((pc.y - pl->y) / hl + (pr->y - pc.y) / hr) / 2.0;
                *pl = pc_point_make(pc.x - tx * hl, pc.y - ty * hl);
                *pr = pc_point_make(pc.x + tx * hr, pc.y + ty * hr);
            }
        }
    }
    for (a = 1; a + 1 < m->nvs; a++) {
        if (3 * a >= j0 && 3 * a < j1) {
            double hl = m->vs[a] - m->vs[a - 1];
            double hr = m->vs[a + 1] - m->vs[a];
            for (i = i0; i < i1; i++) {
                pc_point *pl = &m->points[(3 * a - 1) * nx + i];
                pc_point pc = m->points[3 * a * nx + i];
                pc_point *pr = &m->points[(3 * a + 1) * nx + i];
                double tx = ((pc.x - pl->x) / hl + (pr->x - pc.x) / hr) / 2.0;
                double ty = ((pc.y - pl->y) / hl + (pr->y - pc.y) / hr) / 2.0;
                *pl = pc_point_make(pc.x - tx * hl, pc.y - ty * hl);
                *pr = pc_point_make(pc.x + tx * hr, pc.y + ty * hr);
            }
        }
    }
}

typedef struct pull_corner {
    size_t ai, aj;
    double w;
} pull_corner;

typedef struct pull_edge {
    size_t idx;
    double w;
} pull_edge;

pc_status pc_mesh_pull(pc_mesh *m, const pc_point *base, size_t nbase, double s, double t, pc_point d,
                       pc_error *err)
{
    /* Within this distance of the mesh border (0..1 box units) a grab starts to drag the border
     * with it; further in, the border stays exactly where it is. */
    const double border_zone = 0.12;
    /* Falloff radius, in the grabbed patch's size. */
    const double reach_k = 1.8;
    size_t cols;
    size_t rows;
    size_t nx;
    size_t ny;
    size_t pi;
    size_t pj;
    size_t i;
    size_t j;
    size_t k;
    size_t nshare;
    size_t nedge = 0;
    double a;
    double b;
    double ba[4];
    double bb[4];
    double size = 1.0;
    double radius;
    double inner_w;
    double interior;
    double to_border;
    double near_border;
    double norm;
    double corner_norm;
    double inner_part;
    double edge_norm;
    double edge_part;
    double inner_share;
    double corner_share;
    double *share;
    pc_point at0;
    pc_point target;
    pull_corner corner[4];
    pull_edge edge[8];
    int it;

    if (!pc_geom_mesh_structural(m)) {
        return pc_fail(err, PC_ERR_INVALID, "malformed warp mesh");
    }
    if (base == NULL || nbase != m->npoints || !isfinite(d.x) || !isfinite(d.y)) {
        return PC_OK; /* the reference ignores such a pull */
    }
    cols = m->nus - 1;
    rows = m->nvs - 1;
    if (!pc_size_mul(cols + 1, rows + 1, &nshare) ||
        (share = pc_calloc_array(nshare, sizeof *share)) == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory pulling a warp mesh");
    }
    memcpy(m->points, base, nbase * sizeof *base);
    nx = pc_mesh_nx(m);
    ny = pc_mesh_ny(m);
    pi = pc_geom_mesh_locate(m->us, m->nus, s, &a);
    pj = pc_geom_mesh_locate(m->vs, m->nvs, t, &b);
    pc_geom_bernstein(a, ba);
    pc_geom_bernstein(b, bb);
#define ANCHOR(ai_, aj_)                                                                                     \
    (3 * (aj_) * nx + 3 * (ai_) < nbase ? base[3 * (aj_) * nx + 3 * (ai_)] : pc_point_make(0.0, 0.0))
    at0 = pc_mesh_eval(m, s, t);
    target = pc_point_make(at0.x + d.x, at0.y + d.y);
    /* The grabbed patch's size and its "interior-ness": 1 in the middle, 0 on its edges. */
    for (k = 0; k < 2; k++) {
        pc_point q[4];
        double lo = DBL_MAX;
        double hi = -DBL_MAX;
        size_t c;
        q[0] = ANCHOR(pi, pj);
        q[1] = ANCHOR(pi + 1, pj);
        q[2] = ANCHOR(pi, pj + 1);
        q[3] = ANCHOR(pi + 1, pj + 1);
        for (c = 0; c < 4; c++) {
            double v = k == 0 ? q[c].x : q[c].y;
            lo = pc_rs_min(lo, v);
            hi = pc_rs_max(hi, v);
        }
        size = pc_rs_max(size, hi - lo);
    }
    radius = reach_k * size;
    inner_w = (ba[1] + ba[2]) * (bb[1] + bb[2]);
    interior = pc_clampd(inner_w / 0.5625, 0.0, 1.0);
    to_border = pc_rs_min(pc_rs_min(s, 1.0 - s), pc_rs_min(t, 1.0 - t));
    near_border = pc_clampd(1.0 - to_border / border_zone, 0.0, 1.0);
    /* How much of the pull each corner of the grabbed patch is responsible for. A corner on the
     * picture's border only takes part once the grab is near that border; the picture's four
     * corners never move in a pull. */
#define MESH_CORNER(ai_, aj_) (((ai_) == 0 || (ai_) == cols) && ((aj_) == 0 || (aj_) == rows))
#define ON_BORDER(ai_, aj_) ((ai_) == 0 || (ai_) == cols || (aj_) == 0 || (aj_) == rows)
#define REACH(ai_, aj_, wa_, wb_)                                                                            \
    (MESH_CORNER(ai_, aj_) ? 0.0 : (wa_) * (wb_) * (ON_BORDER(ai_, aj_) ? near_border : 1.0))
    corner[0] = (pull_corner){pi, pj, REACH(pi, pj, ba[0] + ba[1], bb[0] + bb[1])};
    corner[1] = (pull_corner){pi + 1, pj, REACH(pi + 1, pj, ba[2] + ba[3], bb[0] + bb[1])};
    corner[2] = (pull_corner){pi, pj + 1, REACH(pi, pj + 1, ba[0] + ba[1], bb[2] + bb[3])};
    corner[3] = (pull_corner){pi + 1, pj + 1, REACH(pi + 1, pj + 1, ba[2] + ba[3], bb[2] + bb[3])};
    /* Share of d for each anchor. */
    for (j = 0; j <= rows; j++) {
        for (i = 0; i <= cols; i++) {
            pc_point p = ANCHOR(i, j);
            double x = pc_clampd(hypot(p.x - at0.x, p.y - at0.y) / radius, 0.0, 1.0);
            double fall = 0.5 * (1.0 + cos(PI * x));
            double e = MESH_CORNER(i, j) ? 0.0 : (ON_BORDER(i, j) ? near_border : 1.0);
            share[j * (cols + 1) + i] = fall * e;
        }
    }
    /* The grabbed patch's own corners: near one, that corner follows the pointer. */
    for (k = 0; k < 4; k++) {
        double *f = &share[corner[k].aj * (cols + 1) + corner[k].ai];
        *f = pc_rs_max(*f, (1.0 - interior) * corner[k].w);
    }
    /* Every control point moves with the anchor it is closest to. */
    for (j = 0; j < ny; j++) {
        for (i = 0; i < nx; i++) {
            size_t ai = (i + 1) / 3 < cols ? (i + 1) / 3 : cols;
            size_t aj = (j + 1) / 3 < rows ? (j + 1) / 3 : rows;
            add_scaled(&m->points[j * nx + i], d, share[aj * (cols + 1) + ai]);
        }
    }
    free(share);
    /* Close the gap exactly with three groups of control points sharing the work: the grabbed
     * patch's four inner points, the corner blocks (an anchor with its handles, moving as a whole
     * so lines stay smooth) and, near the picture's border, that border's own handles. Closing
     * nudges the tangents across the grid lines, so they are evened out again and the gap
     * re-closed. Sums start at -0.0 like Rust's Iterator::sum. */
    norm = -0.0;
    for (j = 1; j <= 2; j++) {
        for (i = 1; i <= 2; i++) {
            norm += (ba[i] * bb[j]) * (ba[i] * bb[j]);
        }
    }
    corner_norm = -0.0;
    for (k = 0; k < 4; k++) {
        corner_norm += corner[k].w * corner[k].w;
    }
    /* With no free corner to help (a one-patch grid grabbed away from its border) the four inner
     * points do all of it. */
    inner_part = corner_norm > 1e-12 ? pc_rs_min_k(inner_w / 0.15, 1.0) : 1.0;
    for (k = 1; k <= 2; k++) {
        if (pi == 0) {
            edge[nedge++] = (pull_edge){(3 * pj + k) * nx + 3 * pi, ba[0] * bb[k]};
        }
        if (pi + 1 == cols) {
            edge[nedge++] = (pull_edge){(3 * pj + k) * nx + 3 * pi + 3, ba[3] * bb[k]};
        }
        if (pj == 0) {
            edge[nedge++] = (pull_edge){3 * pj * nx + 3 * pi + k, ba[k] * bb[0]};
        }
        if (pj + 1 == rows) {
            edge[nedge++] = (pull_edge){(3 * pj + 3) * nx + 3 * pi + k, ba[k] * bb[3]};
        }
    }
    edge_norm = -0.0;
    for (k = 0; k < nedge; k++) {
        edge_norm += edge[k].w * edge[k].w;
    }
    edge_part = edge_norm > 1e-12 ? near_border : 0.0;
    inner_share = (1.0 - edge_part) * inner_part;
    corner_share = (1.0 - edge_part) * (1.0 - inner_part);
    for (it = 0; it < 60; it++) {
        pc_point now = pc_mesh_eval(m, s, t);
        pc_point rem = pc_point_make(target.x - now.x, target.y - now.y);
        int q;
        if (hypot(rem.x, rem.y) < 1e-9) {
            break;
        }
        if (inner_share > 0.0 && norm > 0.0) {
            for (j = 1; j <= 2; j++) {
                for (i = 1; i <= 2; i++) {
                    add_scaled(&m->points[(3 * pj + j) * nx + 3 * pi + i], rem,
                               ba[i] * bb[j] / norm * inner_share);
                }
            }
        }
        if (edge_part > 0.0) {
            for (k = 0; k < nedge; k++) {
                if (edge[k].idx < m->npoints) {
                    add_scaled(&m->points[edge[k].idx], rem, edge[k].w / edge_norm * edge_part);
                }
            }
        }
        if (corner_share > 0.0 && corner_norm > 0.0) {
            for (k = 0; k < 4; k++) {
                double kk = corner[k].w / corner_norm * corner_share;
                int di;
                int dj;
                for (dj = -1; dj <= 1; dj++) {
                    for (di = -1; di <= 1; di++) {
                        size_t ci = 3 * corner[k].ai;
                        size_t cj = 3 * corner[k].aj;
                        if ((di < 0 && ci == 0) || (dj < 0 && cj == 0)) {
                            continue;
                        }
                        ci = di < 0 ? ci - 1 : ci + (size_t)di;
                        cj = dj < 0 ? cj - 1 : cj + (size_t)dj;
                        if (ci < nx && cj < ny) {
                            add_scaled(&m->points[cj * nx + ci], rem, kk);
                        }
                    }
                }
            }
        }
        for (q = 0; q < 3; q++) {
            smooth_lines(m, 3 * pi >= 3 ? 3 * pi - 3 : 0, 3 * pi + 7, 3 * pj >= 3 ? 3 * pj - 3 : 0,
                         3 * pj + 7);
        }
    }
#undef ANCHOR
#undef MESH_CORNER
#undef ON_BORDER
#undef REACH
    return PC_OK;
}

/* Moves point (i, j) by d when it lies on the grid. */
static void shift_point(pc_mesh *m, size_t nx, size_t ny, ptrdiff_t i, ptrdiff_t j, pc_point d)
{
    if (i >= 0 && j >= 0 && (size_t)i < nx && (size_t)j < ny) {
        pc_point *p = &m->points[(size_t)j * nx + (size_t)i];
        p->x += d.x;
        p->y += d.y;
    }
}

/* Spans either side of interior knot k. */
static bool knot_span(const double *knots, size_t n, size_t k, double *hl, double *hr)
{
    if (k > 0 && k + 1 < n) {
        *hl = knots[k] - knots[k - 1];
        *hr = knots[k + 1] - knots[k];
        return true;
    }
    return false;
}

void pc_mesh_drag_point(pc_mesh *m, const pc_point *base, size_t nbase, size_t index, pc_point d)
{
    size_t nx;
    size_t ny;
    size_t i;
    size_t j;
    ptrdiff_t ii;
    ptrdiff_t jj;
    ptrdiff_t q;
    double hl;
    double hr;
    if (!pc_geom_mesh_structural(m) || base == NULL || nbase != m->npoints || index >= nbase ||
        !isfinite(d.x) || !isfinite(d.y)) {
        return;
    }
    memcpy(m->points, base, nbase * sizeof *base);
    nx = pc_mesh_nx(m);
    ny = pc_mesh_ny(m);
    i = index % nx;
    j = index / nx;
    ii = (ptrdiff_t)i;
    jj = (ptrdiff_t)j;
    if (i % 3 == 0 && j % 3 == 0) {
        ptrdiff_t dj;
        for (dj = -1; dj <= 1; dj++) {
            for (q = -1; q <= 1; q++) {
                shift_point(m, nx, ny, ii + q, jj + dj, d);
            }
        }
    } else if (i % 3 == 0) {
        /* A handle along a vertical line: it takes the two points beside it across the line,
         * and the opposite handle of the same anchor turns the other way. */
        size_t r = j % 3;
        for (q = -1; q <= 1; q++) {
            shift_point(m, nx, ny, ii + q, jj, d);
        }
        if (knot_span(m->vs, m->nvs, j / 3 + (r == 2 ? 1u : 0u), &hl, &hr)) {
            ptrdiff_t other = r == 1 ? jj - 2 : jj + 2;
            double k = r == 1 ? hl / hr : hr / hl;
            for (q = -1; q <= 1; q++) {
                shift_point(m, nx, ny, ii + q, other, pc_point_make(-d.x * k, -d.y * k));
            }
        }
    } else if (j % 3 == 0) {
        /* A handle along a horizontal line, likewise. */
        size_t r = i % 3;
        for (q = -1; q <= 1; q++) {
            shift_point(m, nx, ny, ii, jj + q, d);
        }
        if (knot_span(m->us, m->nus, i / 3 + (r == 2 ? 1u : 0u), &hl, &hr)) {
            ptrdiff_t other = r == 1 ? ii - 2 : ii + 2;
            double k = r == 1 ? hl / hr : hr / hl;
            for (q = -1; q <= 1; q++) {
                shift_point(m, nx, ny, other, jj + q, pc_point_make(-d.x * k, -d.y * k));
            }
        }
    } else {
        /* An inner point of a patch has no handle of its own. */
        shift_point(m, nx, ny, ii, jj, d);
    }
}
