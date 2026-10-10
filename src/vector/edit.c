/* Point-level path editing, Photoshop's Direct Selection and Convert Point tools (port of
 * crates/vector/src/edit.rs): move anchors, drag direction handles, reshape a segment by
 * dragging it, convert between smooth and corner points, and hit-test a path's parts. The
 * engine's path edit commands and the canvas previews share it. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "vector/internal.h"

bool pc_vec_handle_parse(const char *s, size_t len, pc_vec_handle *out)
{
    if (len == 2 && memcmp(s, "in", 2) == 0) {
        *out = PC_VEC_HANDLE_IN;
        return true;
    }
    if (len == 3 && memcmp(s, "out", 3) == 0) {
        *out = PC_VEC_HANDLE_OUT;
        return true;
    }
    return false;
}

const char *pc_vec_handle_name(pc_vec_handle h)
{
    return h == PC_VEC_HANDLE_IN ? "in" : "out";
}

static pc_point padd(pc_point p, double dx, double dy)
{
    return pc_point_make(p.x + dx, p.y + dy);
}
static double dist(pc_point a, pc_point b)
{
    return hypot(a.x - b.x, a.y - b.y);
}

static pc_knot *knot_at(pc_path *path, pc_vec_knot_ref r, pc_error *err)
{
    if (r.sub < path->nsubpaths && r.knot < path->subpaths[r.sub].nknots) {
        return &path->subpaths[r.sub].knots[r.knot];
    }
    (void)pc_fail(err, PC_ERR_INVALID, "no knot %zu in subpath %zu", r.knot, r.sub);
    return NULL;
}

/* The knot ending the segment that leaves r (wrapping on closed subpaths). */
static pc_status segment_end(const pc_path *path, pc_vec_knot_ref r, pc_vec_knot_ref *out, pc_error *err)
{
    const pc_subpath *sp;
    size_t n;
    if (r.sub >= path->nsubpaths) {
        return pc_fail(err, PC_ERR_INVALID, "no subpath %zu", r.sub);
    }
    sp = &path->subpaths[r.sub];
    n = sp->nknots;
    if (r.knot != SIZE_MAX) {
        size_t next = r.knot + 1;
        if (r.knot < n && next < n) {
            out->sub = r.sub;
            out->knot = next;
            return PC_OK;
        }
        if (r.knot < n && sp->closed && n > 1) {
            out->sub = r.sub;
            out->knot = 0;
            return PC_OK;
        }
    }
    return pc_fail(err, PC_ERR_INVALID, "no segment leaves knot %zu of subpath %zu", r.knot, r.sub);
}

static pc_point *handle_ptr(pc_knot *k, pc_vec_handle h)
{
    return h == PC_VEC_HANDLE_IN ? &k->in_ctrl : &k->out_ctrl;
}
static pc_point handle_of(const pc_knot *k, pc_vec_handle h)
{
    return h == PC_VEC_HANDLE_IN ? k->in_ctrl : k->out_ctrl;
}
static pc_vec_handle other(pc_vec_handle h)
{
    return h == PC_VEC_HANDLE_IN ? PC_VEC_HANDLE_OUT : PC_VEC_HANDLE_IN;
}

/* A smooth knot keeps its handles collinear: point handle h's opposite away from it, keeping
 * the opposite's length (a retracted handle stays retracted, as in Photoshop). */
static void keep_smooth(pc_knot *k, pc_vec_handle h)
{
    pc_point a;
    pc_point moved;
    pc_point *opp;
    double l;
    double d;
    if (!k->smooth) {
        return;
    }
    a = k->anchor;
    moved = handle_of(k, h);
    opp = handle_ptr(k, other(h));
    l = dist(*opp, a);
    d = dist(moved, a);
    if (l > 1e-9 && d > 1e-9) {
        *opp = pc_point_make(a.x - (moved.x - a.x) / d * l, a.y - (moved.y - a.y) / d * l);
    }
}

static int ref_cmp(const void *pa, const void *pb)
{
    const pc_vec_knot_ref *a = pa;
    const pc_vec_knot_ref *b = pb;
    if (a->sub != b->sub) {
        return a->sub < b->sub ? -1 : 1;
    }
    if (a->knot != b->knot) {
        return a->knot < b->knot ? -1 : 1;
    }
    return 0;
}

pc_status pc_vec_move_anchors(pc_path *path, const pc_vec_knot_ref *anchors, size_t n, const double d[2],
                              pc_error *err)
{
    pc_vec_knot_ref *list = NULL;
    size_t m = 0;
    size_t i;
    if (n > 0) {
        list = pc_malloc_array(n, sizeof *list);
        if (list == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        memcpy(list, anchors, n * sizeof *list);
        qsort(list, n, sizeof *list, ref_cmp); /* equal entries are identical: stability is moot */
        for (i = 0; i < n; i++) {
            if (m == 0 || ref_cmp(&list[m - 1], &list[i]) != 0) {
                list[m++] = list[i];
            }
        }
    }
    /* Validate everything before any knot moves. */
    for (i = 0; i < m; i++) {
        if (knot_at(path, list[i], err) == NULL) {
            free(list);
            return PC_ERR_INVALID;
        }
    }
    for (i = 0; i < m; i++) {
        pc_knot *k = knot_at(path, list[i], NULL);
        if (k != NULL) {
            k->anchor = padd(k->anchor, d[0], d[1]);
            k->in_ctrl = padd(k->in_ctrl, d[0], d[1]);
            k->out_ctrl = padd(k->out_ctrl, d[0], d[1]);
        }
    }
    free(list);
    return PC_OK;
}

pc_status pc_vec_move_handle(pc_path *path, pc_vec_knot_ref knot, pc_vec_handle h, pc_point to,
                             bool independent, pc_error *err)
{
    pc_knot *k = knot_at(path, knot, err);
    if (k == NULL) {
        return PC_ERR_INVALID;
    }
    *handle_ptr(k, h) = to;
    if (independent) {
        k->smooth = false;
    }
    keep_smooth(k, h);
    return PC_OK;
}

pc_status pc_vec_convert_point(pc_path *path, pc_vec_knot_ref knot, const pc_point *out, pc_error *err)
{
    pc_knot *k = knot_at(path, knot, err);
    pc_point a;
    if (k == NULL) {
        return PC_ERR_INVALID;
    }
    a = k->anchor;
    if (out != NULL) {
        *k = pc_knot_smooth(a, pc_point_make(2.0 * a.x - out->x, 2.0 * a.y - out->y), *out);
    } else {
        *k = pc_knot_corner(a.x, a.y);
    }
    return PC_OK;
}

/* Drags the segment leaving knot at curve parameter t by d. A straight segment (both handles
 * retracted) moves with its two anchors; a curve reshapes so the point at t follows the pointer
 * exactly, with the nearer handle moving more. Smooth end knots stay smooth. */
pc_status pc_vec_bend_segment(pc_path *path, pc_vec_knot_ref knot, double t, const double d[2], pc_error *err)
{
    pc_vec_knot_ref end;
    pc_knot *ka;
    pc_knot *kb;
    pc_status st = segment_end(path, knot, &end, err);
    double u;
    double s;
    double w1;
    double w2;
    if (st != PC_OK) {
        return st;
    }
    ka = knot_at(path, knot, err);
    kb = knot_at(path, end, err);
    if (ka == NULL || kb == NULL) {
        return PC_ERR_INVALID;
    }
    if (dist(ka->out_ctrl, ka->anchor) < 1e-9 && dist(kb->in_ctrl, kb->anchor) < 1e-9) {
        pc_vec_knot_ref both[2];
        both[0] = knot;
        both[1] = end;
        return pc_vec_move_anchors(path, both, 2, d, err);
    }
    /* B(t) moves by 3(1-t)^2 t dc1 + 3(1-t) t^2 dc2. With dc1 = (1-t) s d and dc2 = t s d that is
     * 3t(1-t)((1-t)^2 + t^2) s d, so s below makes it exactly d. The ends are clamped: a drag
     * right at an anchor would need an unbounded handle move. */
    t = isfinite(t) ? pc_clampd(t, 0.05, 0.95) : 0.5;
    u = 1.0 - t;
    s = 1.0 / (3.0 * t * u * (u * u + t * t));
    w1 = u * s;
    w2 = t * s;
    ka->out_ctrl = padd(ka->out_ctrl, d[0] * w1, d[1] * w1);
    keep_smooth(ka, PC_VEC_HANDLE_OUT);
    kb->in_ctrl = padd(kb->in_ctrl, d[0] * w2, d[1] * w2);
    keep_smooth(kb, PC_VEC_HANDLE_IN);
    return PC_OK;
}

pc_status pc_vec_subpath_anchors(const pc_path *path, size_t s, pc_vec_knot_ref **out, size_t *n,
                                 pc_error *err)
{
    pc_vec_knot_ref *v = NULL;
    size_t m = 0;
    size_t k;
    if (s < path->nsubpaths && path->subpaths[s].nknots > 0) {
        m = path->subpaths[s].nknots;
        v = pc_malloc_array(m, sizeof *v);
        if (v == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        for (k = 0; k < m; k++) {
            v[k].sub = s;
            v[k].knot = k;
        }
    }
    *out = v;
    *n = m;
    return PC_OK;
}

pc_status pc_vec_anchors_in(const pc_path *path, const double box[4], pc_vec_knot_ref **out, size_t *n,
                            pc_error *err)
{
    double x0 = pc_rs_min(box[0], box[2]);
    double x1 = pc_rs_max(box[0], box[2]);
    double y0 = pc_rs_min(box[1], box[3]);
    double y1 = pc_rs_max(box[1], box[3]);
    pc_vec_knot_ref *v = NULL;
    size_t m = 0;
    size_t cap = 0;
    size_t s;
    for (s = 0; s < path->nsubpaths; s++) {
        const pc_subpath *sp = &path->subpaths[s];
        size_t k;
        for (k = 0; k < sp->nknots; k++) {
            pc_point a = sp->knots[k].anchor;
            if (a.x >= x0 && a.x <= x1 && a.y >= y0 && a.y <= y1) {
                if (m == cap) {
                    void *nv = pc_grow_array(v, &cap, m + 1, sizeof *v);
                    if (nv == NULL) {
                        free(v);
                        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
                    }
                    v = nv;
                }
                v[m].sub = s;
                v[m].knot = k;
                m++;
            }
        }
    }
    *out = v;
    *n = m;
    return PC_OK;
}

static pc_point eval(const pc_point seg[4], double t)
{
    double u = 1.0 - t;
    double a = u * u * u;
    double b = 3.0 * u * u * t;
    double c = 3.0 * u * t * t;
    double d = t * t * t;
    return pc_point_make(a * seg[0].x + b * seg[1].x + c * seg[2].x + d * seg[3].x,
                         a * seg[0].y + b * seg[1].y + c * seg[2].y + d * seg[3].y);
}

bool pc_vec_hit_test(const pc_path *path, pc_point p, double tol, const pc_vec_handle_ref *handles,
                     size_t nhandles, pc_vec_hit *out)
{
    /* "nearest": among candidates with d <= tol, the first minimum by total order wins. */
    bool found = false;
    double best = 0.0;
    pc_vec_hit hit;
    size_t i;
    memset(&hit, 0, sizeof hit);
    for (i = 0; i < nhandles; i++) {
        pc_vec_knot_ref r = handles[i].knot;
        double d;
        if (r.sub >= path->nsubpaths || r.knot >= path->subpaths[r.sub].nknots) {
            continue;
        }
        d = dist(handle_of(&path->subpaths[r.sub].knots[r.knot], handles[i].handle), p);
        if (d <= tol && (!found || pc_vec_total_cmp(d, best) < 0)) {
            found = true;
            best = d;
            hit.kind = PC_VEC_HIT_HANDLE;
            hit.knot = r;
            hit.handle = handles[i].handle;
        }
    }
    if (found) {
        *out = hit;
        return true;
    }
    {
        size_t s;
        for (s = 0; s < path->nsubpaths; s++) {
            const pc_subpath *sp = &path->subpaths[s];
            size_t k;
            for (k = 0; k < sp->nknots; k++) {
                double d = dist(sp->knots[k].anchor, p);
                if (d <= tol && (!found || pc_vec_total_cmp(d, best) < 0)) {
                    found = true;
                    best = d;
                    hit.kind = PC_VEC_HIT_ANCHOR;
                    hit.knot.sub = s;
                    hit.knot.knot = k;
                }
            }
        }
    }
    if (found) {
        *out = hit;
        return true;
    }
    {
        /* Segments: the nearest of 64 samples, refined once around it. */
        const size_t N = 64;
        size_t s;
        for (s = 0; s < path->nsubpaths; s++) {
            const pc_subpath *sp = &path->subpaths[s];
            size_t nseg = pc_subpath_segment_count(sp);
            size_t k;
            for (k = 0; k < nseg; k++) {
                pc_point seg[4];
                double bd = 0.0;
                double bt = 0.0;
                double step = 1.0 / (double)N;
                size_t j;
                pc_subpath_segment(sp, k, seg);
                for (j = 0; j <= N; j++) {
                    double t = (double)j / (double)N;
                    double d = dist(eval(seg, t), p);
                    if (j == 0 || pc_vec_total_cmp(d, bd) < 0) {
                        bd = d;
                        bt = t;
                    }
                }
                {
                    double ft = bt;
                    double fd = bd;
                    for (j = 0; j <= 16; j++) {
                        double t = pc_clampd(bt - step + 2.0 * step * (double)j / 16.0, 0.0, 1.0);
                        double d = dist(eval(seg, t), p);
                        if (d < fd) {
                            fd = d;
                            ft = t;
                        }
                    }
                    if (fd <= tol && (!found || pc_vec_total_cmp(fd, best) < 0)) {
                        found = true;
                        best = fd;
                        hit.kind = PC_VEC_HIT_SEGMENT;
                        hit.knot.sub = s;
                        hit.knot.knot = k;
                        hit.t = ft;
                    }
                }
            }
        }
    }
    if (found) {
        *out = hit;
    }
    return found;
}
