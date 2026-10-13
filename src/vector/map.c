/* Non-affine maps of paths (port of crates/vector/src/map.rs), so shape layers and vector masks
 * stay vectors through Edit > Transform > Distort, Perspective and Warp.
 *
 * pc_vec_map_points sends every anchor and control point through the map and keeps the knots as
 * they are: what a projective map (Distort, Perspective) needs. pc_vec_map_path follows a curved
 * map (Warp): it splits each segment where the mapped control points stray from the mapped curve
 * by more than a tolerance, so the result adds anchors where the warp bends it. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "vector/internal.h"

/* Pieces a path may be split into, in total: bounds the work and memory on hostile input. */
#define MAX_PIECES ((size_t)1 << 20)
/* Deepest split of one segment (2^6 = 64 pieces). */
#define MAX_DEPTH 6u

typedef struct mapper {
    pc_vec_point_map f;
    void *ctx;
} mapper;

static pc_point apply(const mapper *m, pc_point p)
{
    return m->f(p, m->ctx);
}

static pc_knot map_knot(const mapper *m, pc_knot k)
{
    pc_knot o;
    o.anchor = apply(m, k.anchor);
    o.in_ctrl = apply(m, k.in_ctrl);
    o.out_ctrl = apply(m, k.out_ctrl);
    o.smooth = k.smooth;
    return o;
}

pc_status pc_vec_map_points(const pc_path *path, pc_vec_point_map f, void *ctx, pc_path *out, pc_error *err)
{
    mapper m;
    pc_path o;
    size_t s;
    size_t k;
    pc_status st = pc_path_clone(path, &o, err);
    if (st != PC_OK) {
        return st;
    }
    m.f = f;
    m.ctx = ctx;
    for (s = 0; s < o.nsubpaths; s++) {
        pc_subpath *sp = &o.subpaths[s];
        for (k = 0; k < sp->nknots; k++) {
            sp->knots[k] = map_knot(&m, sp->knots[k]);
        }
    }
    *out = o;
    return PC_OK;
}

static pc_point lerp(pc_point a, pc_point b, double t)
{
    return pc_point_make(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

static double dist(pc_point a, pc_point b)
{
    return hypot(a.x - b.x, a.y - b.y);
}

static bool same(pc_point a, pc_point b)
{
    return a.x == b.x && a.y == b.y;
}

/* De Casteljau evaluation. */
static pc_point eval(const pc_point s[4], double t)
{
    pc_point ab = lerp(s[0], s[1], t);
    pc_point bc = lerp(s[1], s[2], t);
    pc_point cd = lerp(s[2], s[3], t);
    return lerp(lerp(ab, bc, t), lerp(bc, cd, t), t);
}

/* De Casteljau split at t = 1/2. */
static void halves(const pc_point s[4], pc_point a[4], pc_point b[4])
{
    pc_point ab = lerp(s[0], s[1], 0.5);
    pc_point bc = lerp(s[1], s[2], 0.5);
    pc_point cd = lerp(s[2], s[3], 0.5);
    pc_point abc = lerp(ab, bc, 0.5);
    pc_point bcd = lerp(bc, cd, 0.5);
    pc_point mid = lerp(abc, bcd, 0.5);
    a[0] = s[0];
    a[1] = ab;
    a[2] = abc;
    a[3] = mid;
    b[0] = mid;
    b[1] = bcd;
    b[2] = cd;
    b[3] = s[3];
}

/* Distance from p to the line through a and b (to a when they coincide). */
static double off_line(pc_point p, pc_point a, pc_point b)
{
    double l = dist(a, b);
    if (l < 1e-12) {
        return dist(p, a);
    }
    return fabs((b.x - a.x) * (a.y - p.y) - (a.x - p.x) * (b.y - a.y)) / l;
}

static const double PROBES[3] = {0.25, 0.5, 0.75};

/* Mapped pieces of all segments, back to back. */
typedef struct piece_list {
    pc_point (*v)[4];
    size_t len;
    size_t cap;
} piece_list;

static pc_status piece_push(piece_list *l, const pc_point p[4], pc_error *err)
{
    if (l->len == l->cap) {
        size_t cap = l->cap;
        void *nv = pc_grow_array(l->v, &cap, l->len + 1, sizeof *l->v);
        if (nv == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
        l->v = nv;
        l->cap = cap;
    }
    memcpy(l->v[l->len], p, sizeof l->v[0]);
    l->len++;
    return PC_OK;
}

/* Recursion depth <= MAX_DEPTH. */
static pc_status split_mapped(const pc_point seg[4], const mapper *m, double tol, unsigned depth,
                              piece_list *out, pc_error *err)
{
    pc_point mapped[4];
    pc_point a[4];
    pc_point b[4];
    bool fits = true;
    size_t i;
    pc_status st;
    for (i = 0; i < 4; i++) {
        mapped[i] = apply(m, seg[i]);
    }
    for (i = 0; i < 3 && fits; i++) {
        fits = dist(apply(m, eval(seg, PROBES[i])), eval(mapped, PROBES[i])) <= tol;
    }
    if (fits || depth == 0) {
        return piece_push(out, mapped, err);
    }
    halves(seg, a, b);
    st = split_mapped(a, m, tol, depth - 1, out, err);
    if (st != PC_OK) {
        return st;
    }
    return split_mapped(b, m, tol, depth - 1, out, err);
}

static pc_status map_segment(const pc_point seg[4], const mapper *m, double tol, unsigned depth,
                             piece_list *out, pc_error *err)
{
    /* A straight segment (retracted handles) whose image is straight stays one. */
    if (same(seg[1], seg[0]) && same(seg[2], seg[3])) {
        pc_point a = apply(m, seg[0]);
        pc_point b = apply(m, seg[3]);
        pc_point line[4];
        bool straight = true;
        size_t i;
        for (i = 0; i < 3 && straight; i++) {
            straight = off_line(apply(m, lerp(seg[0], seg[3], PROBES[i])), a, b) <= tol;
        }
        if (straight) {
            pc_point p[4];
            p[0] = a;
            p[1] = a;
            p[2] = b;
            p[3] = b;
            return piece_push(out, p, err);
        }
        /* Curved by the map: the same line as an evenly parametrised cubic, then split. */
        line[0] = seg[0];
        line[1] = lerp(seg[0], seg[3], 1.0 / 3.0);
        line[2] = lerp(seg[0], seg[3], 2.0 / 3.0);
        line[3] = seg[3];
        return split_mapped(line, m, tol, depth, out, err);
    }
    return split_mapped(seg, m, tol, depth, out, err);
}

static pc_status map_subpath(const pc_subpath *s, const mapper *m, double tol, unsigned depth,
                             pc_subpath *out, pc_error *err)
{
    size_t n = s->nknots;
    size_t nseg = pc_subpath_segment_count(s);
    piece_list pl = {NULL, 0, 0};
    size_t *start = NULL; /* segment i's pieces are pl.v[start[i] .. start[i + 1]] */
    pc_knot *knots = NULL;
    size_t nk = 0;
    size_t i;
    size_t total;
    pc_status st = PC_OK;
    if (nseg == 0) {
        st = pc_subpath_clone(s, out, err);
        if (st != PC_OK) {
            return st;
        }
        for (i = 0; i < out->nknots; i++) {
            out->knots[i] = map_knot(m, out->knots[i]);
        }
        return PC_OK;
    }
    start = pc_malloc_array(nseg + 1, sizeof *start);
    if (start == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
    }
    for (i = 0; i < nseg && st == PC_OK; i++) {
        pc_point seg[4];
        pc_subpath_segment(s, i, seg);
        start[i] = pl.len;
        st = map_segment(seg, m, tol, depth, &pl, err);
    }
    start[nseg] = pl.len;
    if (st != PC_OK) {
        goto cleanup;
    }
    /* n anchors plus the ones added inside each segment (pieces - 1 per segment). */
    total = n + pl.len;
    knots = pc_malloc_array(total, sizeof *knots);
    if (knots == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        goto cleanup;
    }
    for (i = 0; i < n; i++) {
        const pc_knot *k = &s->knots[i];
        pc_knot o;
        size_t in_seg = SIZE_MAX;
        size_t j;
        if (i > 0) {
            in_seg = i - 1;
        } else if (s->closed) {
            in_seg = n - 1;
        }
        o.anchor = apply(m, k->anchor);
        if (in_seg < nseg && start[in_seg + 1] > start[in_seg]) {
            o.in_ctrl = pl.v[start[in_seg + 1] - 1][2];
        } else {
            o.in_ctrl = apply(m, k->in_ctrl);
        }
        if (i < nseg && start[i + 1] > start[i]) {
            o.out_ctrl = pl.v[start[i]][1];
        } else {
            o.out_ctrl = apply(m, k->out_ctrl);
        }
        o.smooth = k->smooth;
        knots[nk++] = o;
        /* The anchors the split added inside segment i: smooth, as the curve runs through them. */
        if (i < nseg) {
            for (j = start[i] + 1; j < start[i + 1]; j++) {
                knots[nk].anchor = pl.v[j][0];
                knots[nk].in_ctrl = pl.v[j - 1][2];
                knots[nk].out_ctrl = pl.v[j][1];
                knots[nk].smooth = true;
                nk++;
            }
        }
    }
    memset(out, 0, sizeof *out);
    out->closed = s->closed;
    out->knots = knots;
    out->nknots = nk;
    out->op = s->op;
    knots = NULL;
cleanup:
    free(pl.v);
    free(start);
    free(knots);
    return st;
}

pc_status pc_vec_map_path(const pc_path *path, pc_vec_point_map f, void *ctx, double tol, pc_path *out,
                          pc_error *err)
{
    mapper m;
    size_t segments = 0;
    unsigned depth = MAX_DEPTH;
    pc_subpath *subs = NULL;
    size_t i;
    pc_status st = PC_OK;
    m.f = f;
    m.ctx = ctx;
    if (!(isfinite(tol) && tol > 0.0)) {
        tol = 0.1;
    }
    for (i = 0; i < path->nsubpaths; i++) {
        size_t k = path->subpaths[i].nknots;
        segments = segments > SIZE_MAX - k ? SIZE_MAX : segments + k;
    }
    /* Fewer splits for huge paths, so the result stays bounded: segments * 2^depth <= MAX_PIECES. */
    while (depth > 0 && segments > (MAX_PIECES >> depth)) {
        depth--;
    }
    if (path->nsubpaths > 0) {
        subs = pc_calloc_array(path->nsubpaths, sizeof *subs);
        if (subs == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "vector: out of memory");
        }
    }
    for (i = 0; i < path->nsubpaths && st == PC_OK; i++) {
        st = map_subpath(&path->subpaths[i], &m, tol, depth, &subs[i], err);
    }
    if (st != PC_OK) {
        size_t k;
        for (k = 0; k < path->nsubpaths; k++) {
            pc_subpath_fini(&subs[k]);
        }
        free(subs);
        return st;
    }
    memset(out, 0, sizeof *out);
    out->subpaths = subs;
    out->nsubpaths = path->nsubpaths;
    out->fill_rule = path->fill_rule;
    out->inverted = path->inverted;
    return PC_OK;
}
