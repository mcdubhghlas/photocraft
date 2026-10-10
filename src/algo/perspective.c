/* Perspective Warp (port of crates/algo/src/perspective.rs). See perspective.h. */
#include "algo/perspective.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

#define LINK_EPS 0.5
#define NO_PLANE ((size_t)-1)

static const double UNIT[4][2] = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}};

static bool unit_to_quad(const double q[4][2], pc_algo_homography *out)
{
    static const double r[4] = {0.0, 0.0, 1.0, 1.0};
    return pc_algo_homography_rect_to_quad(r, q, out);
}

static bool near(const double a[2], const double b[2])
{
    return fabs(a[0] - b[0]) <= LINK_EPS && fabs(a[1] - b[1]) <= LINK_EPS;
}

bool pc_algo_persp_straighten_parse(const char *s, size_t len, pc_algo_persp_straighten *out)
{
    static const struct {
        const char *name;
        pc_algo_persp_straighten v;
    } names[] = {{"h", PC_ALGO_PERSP_HORIZONTAL},      {"horizontal", PC_ALGO_PERSP_HORIZONTAL},
                 {"level", PC_ALGO_PERSP_HORIZONTAL},  {"v", PC_ALGO_PERSP_VERTICAL},
                 {"vertical", PC_ALGO_PERSP_VERTICAL}, {"auto", PC_ALGO_PERSP_AUTO},
                 {"both", PC_ALGO_PERSP_AUTO}};
    size_t i, k;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strlen(names[i].name) != len) {
            continue;
        }
        for (k = 0; k < len; k++) {
            char c = s[k];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            if (c != names[i].name[k]) {
                break;
            }
        }
        if (k == len) {
            *out = names[i].v;
            return true;
        }
    }
    return false;
}

pc_algo_persp_plane pc_algo_persp_plane_identity(const double q[4][2])
{
    pc_algo_persp_plane p;
    memcpy(p.src, q, sizeof p.src);
    memcpy(p.dst, q, sizeof p.dst);
    return p;
}

void pc_algo_persp_linked_corners(const pc_algo_persp_plane *planes, size_t n, size_t *group_of,
                                  size_t *n_groups)
{
    size_t ng = 0, pi, ci, g, k;
    /* The reference compares a corner with each group's FIRST member; that member is the earliest
     * corner given that group, so first[g] is the index of its first appearance. */
    for (pi = 0; pi < n; pi++) {
        for (ci = 0; ci < 4; ci++) {
            const double *c = planes[pi].src[ci];
            size_t found = NO_PLANE;
            for (g = 0; g < ng && found == NO_PLANE; g++) {
                for (k = 0; k < pi * 4 + ci; k++) {
                    if (group_of[k] == g) {
                        if (near(planes[k / 4].src[k % 4], c)) {
                            found = g;
                        }
                        break;
                    }
                }
            }
            if (found == NO_PLANE) {
                found = ng++;
            }
            group_of[pi * 4 + ci] = found;
        }
    }
    *n_groups = ng;
}

static pc_status groups_of(const pc_algo_persp_plane *planes, size_t n, size_t **group_of, size_t *ng,
                           pc_error *err)
{
    size_t *g = pc_malloc_array(n * 4 + 1, sizeof *g);
    if (g == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    pc_algo_persp_linked_corners(planes, n, g, ng);
    *group_of = g;
    return PC_OK;
}

pc_status pc_algo_persp_unify(pc_algo_persp_plane *planes, size_t n, pc_error *err)
{
    size_t *gof = NULL, ng = 0, g, k, cnt;
    pc_status st = groups_of(planes, n, &gof, &ng, err);
    if (st != PC_OK) {
        return st;
    }
    for (g = 0; g < ng; g++) {
        double s[2] = {0.0, 0.0}, d[2] = {0.0, 0.0}, nn;
        cnt = 0;
        for (k = 0; k < n * 4; k++) {
            cnt += gof[k] == g;
        }
        if (cnt < 2) {
            continue;
        }
        nn = (double)cnt;
        for (k = 0; k < n * 4; k++) {
            if (gof[k] == g) {
                s[0] = s[0] + planes[k / 4].src[k % 4][0] / nn;
                s[1] = s[1] + planes[k / 4].src[k % 4][1] / nn;
                d[0] = d[0] + planes[k / 4].dst[k % 4][0] / nn;
                d[1] = d[1] + planes[k / 4].dst[k % 4][1] / nn;
            }
        }
        for (k = 0; k < n * 4; k++) {
            if (gof[k] == g) {
                memcpy(planes[k / 4].src[k % 4], s, sizeof s);
                memcpy(planes[k / 4].dst[k % 4], d, sizeof d);
            }
        }
    }
    free(gof);
    return PC_OK;
}

pc_status pc_algo_persp_straighten_planes(pc_algo_persp_plane *planes, size_t n,
                                          pc_algo_persp_straighten mode, pc_error *err)
{
    size_t *gof = NULL, ng = 0, g, k, p;
    double (*pos)[2] = NULL;
    pc_status st;
    bool hor = mode == PC_ALGO_PERSP_HORIZONTAL || mode == PC_ALGO_PERSP_AUTO;
    bool ver = mode == PC_ALGO_PERSP_VERTICAL || mode == PC_ALGO_PERSP_AUTO;
    if ((st = pc_algo_persp_unify(planes, n, err)) != PC_OK ||
        (st = groups_of(planes, n, &gof, &ng, err)) != PC_OK) {
        return st;
    }
    pos = pc_malloc_array(ng + 1, sizeof *pos);
    if (pos == NULL) {
        free(gof);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (g = 0; g < ng; g++) {
        for (k = 0; k < n * 4; k++) {
            if (gof[k] == g) {
                memcpy(pos[g], planes[k / 4].dst[k % 4], sizeof pos[g]);
                break;
            }
        }
    }
    for (p = 0; p < n; p++) {
        for (k = 0; k < 4; k++) {
            size_t a = gof[p * 4 + k], b = gof[p * 4 + (k + 1) % 4];
            double pa[2] = {pos[a][0], pos[a][1]}, pb[2] = {pos[b][0], pos[b][1]};
            double dx = fabs(pb[0] - pa[0]), dy = fabs(pb[1] - pa[1]);
            if (dy <= dx && hor) {
                double y = (pa[1] + pb[1]) / 2.0;
                pos[a][1] = y;
                pos[b][1] = y;
            } else if (dx < dy && ver) {
                double x = (pa[0] + pb[0]) / 2.0;
                pos[a][0] = x;
                pos[b][0] = x;
            }
        }
    }
    for (k = 0; k < n * 4; k++) {
        memcpy(planes[k / 4].dst[k % 4], pos[gof[k]], sizeof pos[0]);
    }
    free(pos);
    free(gof);
    return PC_OK;
}

typedef struct plane_map {
    pc_algo_homography src, src_inv, dst;
    double quad[4][2];
    size_t shared[4]; /* neighbouring plane sharing edge k, or NO_PLANE */
} plane_map;

struct pc_algo_persp_map {
    plane_map *planes;
    size_t n;
};

pc_status pc_algo_persp_map_new(const pc_algo_persp_plane *planes, size_t n, pc_algo_persp_map **out,
                                pc_error *err)
{
    pc_algo_persp_plane *pl = NULL;
    pc_algo_persp_map *m = NULL;
    size_t i, j, k, q;
    pc_status st;
    if (n == 0) {
        return pc_fail(err, PC_ERR_INVALID, "no planes");
    }
    pl = pc_malloc_array(n, sizeof *pl);
    m = calloc(1, sizeof *m);
    if (pl == NULL || m == NULL || (m->planes = pc_malloc_array(n, sizeof *m->planes)) == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto fail;
    }
    memcpy(pl, planes, n * sizeof *pl);
    if ((st = pc_algo_persp_unify(pl, n, err)) != PC_OK) {
        goto fail;
    }
    m->n = n;
    for (i = 0; i < n; i++) {
        plane_map *pm = &m->planes[i];
        if (!unit_to_quad((const double (*)[2])pl[i].src, &pm->src) ||
            !pc_algo_homography_inverse(&pm->src, &pm->src_inv) ||
            !unit_to_quad((const double (*)[2])pl[i].dst, &pm->dst)) {
            st = pc_fail(err, PC_ERR_INVALID, "plane %zu is degenerate", i);
            goto fail;
        }
        memcpy(pm->quad, pl[i].src, sizeof pm->quad);
        for (k = 0; k < 4; k++) {
            const double *a = pl[i].src[k], *b = pl[i].src[(k + 1) % 4];
            pm->shared[k] = NO_PLANE;
            for (j = 0; j < n && pm->shared[k] == NO_PLANE; j++) {
                if (j == i) {
                    continue;
                }
                for (q = 0; q < 4; q++) {
                    const double *c = pl[j].src[q], *d = pl[j].src[(q + 1) % 4];
                    if ((near(a, c) && near(b, d)) || (near(a, d) && near(b, c))) {
                        pm->shared[k] = j;
                        break;
                    }
                }
            }
        }
    }
    free(pl);
    *out = m;
    return PC_OK;
fail:
    free(pl);
    pc_algo_persp_map_free(m);
    return st;
}

void pc_algo_persp_map_free(pc_algo_persp_map *m)
{
    if (m != NULL) {
        free(m->planes);
        free(m);
    }
}

static double dist_to_quad(const double q[4][2], double px, double py)
{
    double best = 1.7976931348623157e308;
    int k;
    for (k = 0; k < 4; k++) {
        const double *a = q[k], *b = q[(k + 1) % 4];
        double dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy, t, ex, ey;
        t = l2 > 0.0 ? pc_clampd(((px - a[0]) * dx + (py - a[1]) * dy) / l2, 0.0, 1.0) : 0.0;
        ex = a[0] + t * dx - px;
        ey = a[1] + t * dy - py;
        best = pc_rs_min(best, sqrt(ex * ex + ey * ey));
    }
    return best;
}

/* The plane containing (x, y) (or the nearest one) and the point's plane coordinates. */
static size_t locate(const pc_algo_persp_map *m, double x, double y, double *u_out, double *v_out)
{
    size_t i, bi = 0;
    double bd = 1.7976931348623157e308, bu = 0.0, bv = 0.0;
    for (i = 0; i < m->n; i++) {
        const plane_map *pm = &m->planes[i];
        double u, v, d;
        pc_algo_homography_apply(&pm->src_inv, x, y, &u, &v);
        if (u >= -1e-9 && u <= 1.0 + 1e-9 && v >= -1e-9 && v <= 1.0 + 1e-9) {
            *u_out = u;
            *v_out = v;
            return i;
        }
        d = dist_to_quad((const double (*)[2])pm->quad, x, y);
        if (d < bd) {
            bi = i;
            bd = d;
            bu = u;
            bv = v;
        }
    }
    *u_out = bu;
    *v_out = bv;
    return bi;
}

/* Edge k of a plane runs from corner k to corner k + 1: the point on it and its weight. */
static double edge_uv(size_t k, double u, double v, double e[2])
{
    switch (k) {
    case 0:
        e[0] = u, e[1] = 0.0;
        return 1.0 - v;
    case 1:
        e[0] = 1.0, e[1] = v;
        return u;
    case 2:
        e[0] = u, e[1] = 1.0;
        return v;
    default:
        e[0] = 0.0, e[1] = v;
        return 1.0 - u;
    }
}

void pc_algo_persp_map_apply(const pc_algo_persp_map *m, double x, double y, double *ox_out, double *oy_out)
{
    double u, v, ox, oy, cu, cv;
    size_t i = locate(m, x, y, &u, &v), k;
    const plane_map *pm = &m->planes[i];
    pc_algo_homography_apply(&pm->dst, u, v, &ox, &oy);
    if (!isfinite(ox) || !isfinite(oy)) {
        *ox_out = x;
        *oy_out = y;
        return;
    }
    cu = pc_clampd(u, 0.0, 1.0);
    cv = pc_clampd(v, 0.0, 1.0);
    for (k = 0; k < 4; k++) {
        double e[2], w, mx, my, qx, qy, ou, ov, tx, ty;
        const plane_map *other;
        if (pm->shared[k] == NO_PLANE) {
            continue;
        }
        w = edge_uv(k, cu, cv, e);
        if (w <= 0.0) {
            continue;
        }
        pc_algo_homography_apply(&pm->dst, e[0], e[1], &mx, &my);
        pc_algo_homography_apply(&pm->src, e[0], e[1], &qx, &qy);
        other = &m->planes[pm->shared[k]];
        pc_algo_homography_apply(&other->src_inv, qx, qy, &ou, &ov);
        pc_algo_homography_apply(&other->dst, ou, ov, &tx, &ty);
        ox += w * ((mx + tx) / 2.0 - mx);
        oy += w * ((my + ty) / 2.0 - my);
    }
    *ox_out = ox;
    *oy_out = oy;
}

bool pc_algo_persp_is_identity(const pc_algo_persp_plane *planes, size_t n)
{
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        for (k = 0; k < 4; k++) {
            if (!(fabs(planes[i].src[k][0] - planes[i].dst[k][0]) < 1e-9 &&
                  fabs(planes[i].src[k][1] - planes[i].dst[k][1]) < 1e-9)) {
                return false;
            }
        }
    }
    return true;
}

void pc_algo_persp_unit_corners(double out[4][2])
{
    memcpy(out, UNIT, sizeof UNIT);
}

void pc_algo_persp_plane_point(const pc_algo_persp_plane *p, double u, double v, bool dst, double out[2])
{
    const double (*q)[2] = (const double (*)[2])(dst ? p->dst : p->src);
    pc_algo_homography h;
    if (unit_to_quad(q, &h)) {
        pc_algo_homography_apply(&h, u, v, &out[0], &out[1]);
    } else {
        out[0] = q[0][0];
        out[1] = q[0][1];
    }
}
