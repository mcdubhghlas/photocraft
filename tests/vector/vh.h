/* Test helpers for tests/vector: path builders mirroring the Rust tests' `Path::new(vec![...])`,
 * `Subpath::polygon`, `.with_op`, plus FNV-1a hashing and the LCG that rustref's
 * vector_vectors.rs uses. Header-only (static functions), one copy per test executable. */
#ifndef PC_TESTS_VECTOR_VH_H
#define PC_TESTS_VECTOR_VH_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "vector/vector.h"

#define VH_H0 0xcbf29ce484222325ull

/* ---- FNV-1a over little-endian bit patterns ---- */

static inline void vh_b(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}

static inline void vh_u64(uint64_t *h, uint64_t v)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    vh_b(h, b, 8);
}

static inline void vh_f64(uint64_t *h, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    vh_u64(h, u);
}

static inline void vh_f32(uint64_t *h, float v)
{
    uint32_t u;
    uint8_t b[4];
    memcpy(&u, &v, 4);
    b[0] = (uint8_t)u;
    b[1] = (uint8_t)(u >> 8);
    b[2] = (uint8_t)(u >> 16);
    b[3] = (uint8_t)(u >> 24);
    vh_b(h, b, 4);
}

static inline void vh_flag(uint64_t *h, bool v)
{
    uint8_t b = v ? 1 : 0;
    vh_b(h, &b, 1);
}

/* ---- LCG ---- */

typedef struct vh_rng {
    uint64_t s;
} vh_rng;

static inline uint64_t vh_next(vh_rng *r)
{
    r->s = r->s * 6364136223846793005ull + 1442695040888963407ull;
    return r->s;
}

static inline double vh_f(vh_rng *r)
{
    return (double)(vh_next(r) >> 11) / 9007199254740992.0;
}

static inline uint64_t vh_below(vh_rng *r, uint64_t n)
{
    return (vh_next(r) >> 33) % n;
}

/* ---- path builders (abort-free: tests check results instead) ---- */

/* A path taking over n subpaths (copied bitwise from subs; the caller drops its copies). */
static inline pc_path vh_path(const pc_subpath *subs, size_t n)
{
    pc_path p;
    pc_subpath *v = NULL;
    memset(&p, 0, sizeof p);
    if (n > 0) {
        v = malloc(n * sizeof *v);
        if (v == NULL) {
            return p;
        }
        memcpy(v, subs, n * sizeof *v);
    }
    p.subpaths = v;
    p.nsubpaths = v != NULL ? n : 0;
    return p;
}

static inline pc_subpath vh_polygon(const double (*pts)[2], size_t n, bool closed)
{
    pc_subpath s;
    memset(&s, 0, sizeof s);
    if (closed) {
        (void)pc_subpath_polygon(pts, n, &s, NULL);
    } else {
        (void)pc_subpath_polyline(pts, n, &s, NULL);
    }
    return s;
}

static inline pc_path vh_poly_path(const double (*pts)[2], size_t n, bool closed)
{
    pc_subpath s = vh_polygon(pts, n, closed);
    return vh_path(&s, 1);
}

/* Removes and returns subpath 0 of p, freeing the rest of p (`p.subpaths.remove(0)`). */
static inline pc_subpath vh_take0(pc_path *p, pc_path_op op)
{
    pc_subpath s;
    memset(&s, 0, sizeof s);
    if (p->nsubpaths > 0) {
        s = p->subpaths[0];
        memset(&p->subpaths[0], 0, sizeof s);
    }
    pc_path_fini(p);
    s.op = op;
    return s;
}

/* Appends s to p (taken over). */
static inline void vh_push(pc_path *p, pc_subpath s)
{
    pc_subpath *v = realloc(p->subpaths, (p->nsubpaths + 1) * sizeof *v);
    if (v == NULL) {
        pc_subpath_fini(&s);
        return;
    }
    v[p->nsubpaths++] = s;
    p->subpaths = v;
}

static inline pc_path vh_rect(double x, double y, double w, double h)
{
    pc_path p;
    memset(&p, 0, sizeof p);
    (void)pc_vec_shape_rect(x, y, w, h, &p, NULL);
    return p;
}

static inline pc_path vh_ellipse(double x, double y, double w, double h)
{
    pc_path p;
    memset(&p, 0, sizeof p);
    (void)pc_vec_shape_ellipse(x, y, w, h, &p, NULL);
    return p;
}

static inline pc_path vh_two_squares(pc_path_op op)
{
    pc_path a = vh_rect(10.0, 10.0, 40.0, 40.0);
    pc_path b = vh_rect(30.0, 30.0, 40.0, 40.0);
    pc_subpath s[2];
    s[0] = vh_take0(&a, PC_PATH_COMBINE);
    s[1] = vh_take0(&b, op);
    return vh_path(s, 2);
}

/* Pentagram drawn as one self-intersecting subpath. */
static inline pc_path vh_pentagram(void)
{
    double pts[5][2];
    int i;
    for (i = 0; i < 5; i++) {
        double a = -1.5707963267948966 + 6.283185307179586 * (double)(i * 2 % 5) / 5.0;
        pts[i][0] = 50.0 + 40.0 * cos(a);
        pts[i][1] = 50.0 + 40.0 * sin(a);
    }
    return vh_poly_path((const double (*)[2])pts, 5, true);
}

/* A smooth knot at (50, 0) between corners, handles 20 px either side (open). */
static inline pc_path vh_curve(void)
{
    static const double pts[3][2] = {{0.0, 50.0}, {50.0, 0.0}, {100.0, 50.0}};
    pc_path p = vh_poly_path(pts, 3, false);
    if (p.nsubpaths == 1) {
        p.subpaths[0].knots[1] =
            pc_knot_smooth(pc_point_make(50.0, 0.0), pc_point_make(30.0, 0.0), pc_point_make(70.0, 0.0));
    }
    return p;
}

static inline pc_path vh_tri(void)
{
    static const double pts[3][2] = {{0.0, 0.0}, {100.0, 0.0}, {50.0, 80.0}};
    return vh_poly_path(pts, 3, true);
}

static inline double vh_area(const float *v, size_t n)
{
    double s = -0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        s += (double)v[i];
    }
    return s;
}

static inline pc_path vh_clone(const pc_path *p)
{
    pc_path c;
    memset(&c, 0, sizeof c);
    (void)pc_path_clone(p, &c, NULL);
    return c;
}

/* A solid fill (no allocation). */
static inline pc_fill vh_solid(pc_color c)
{
    return pc_fill_solid(c);
}

#endif
