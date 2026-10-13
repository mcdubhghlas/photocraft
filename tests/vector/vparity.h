/* Shared by tests/vector/test_parity.c (one translation unit; header-only, static): the golden
 * file reader, the FNV hashes of paths, polygons, polylines and coverage, and the inputs
 * vector_vectors.rs builds (the path corpus and the stroke styles). */
#ifndef PC_TESTS_VECTOR_VPARITY_H
#define PC_TESTS_VECTOR_VPARITY_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "raster/pixel.h"
#include "vh.h"

#define GOLDEN "tests/golden/vector-vectors.txt"
#define LINE 4096

static char **gold;
static size_t ngold;
static size_t gi;

static inline void load_golden(void)
{
    FILE *f = fopen(GOLDEN, "r");
    char line[LINE];
    if (f == NULL) {
        PC_T_FAIL("cannot open " GOLDEN);
        return;
    }
    while (fgets(line, sizeof line, f) != NULL) {
        size_t n = strlen(line);
        char **g;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        g = realloc(gold, (ngold + 1) * sizeof *gold);
        if (g == NULL) {
            break;
        }
        gold = g;
        gold[ngold] = malloc(n + 1);
        if (gold[ngold] == NULL) {
            break;
        }
        memcpy(gold[ngold], line, n + 1);
        ngold++;
    }
    (void)fclose(f);
}

static inline void expect(const char *got)
{
    if (gi >= ngold) {
        PC_T_FAIL("golden ran out at: %s", got);
        return;
    }
    pc_t_checks++;
    if (strcmp(gold[gi], got) != 0) {
        PC_T_FAIL("line %zu\n  want %s\n  got  %s", gi + 1, gold[gi], got);
    }
    gi++;
}

#define EXPECTF(...)                                                                                         \
    do {                                                                                                     \
        char l_[LINE];                                                                                       \
        (void)snprintf(l_, sizeof l_, __VA_ARGS__);                                                          \
        expect(l_);                                                                                          \
    } while (0)

/* ---- hashing ---- */

static inline void hash_path(uint64_t *h, const pc_path *p)
{
    size_t i;
    size_t k;
    vh_u64(h, p->nsubpaths);
    vh_flag(h, p->fill_rule == PC_FILL_RULE_EVENODD);
    vh_flag(h, p->inverted);
    for (i = 0; i < p->nsubpaths; i++) {
        const pc_subpath *s = &p->subpaths[i];
        uint8_t op = (uint8_t)s->op;
        vh_flag(h, s->closed);
        vh_b(h, &op, 1);
        vh_u64(h, s->nknots);
        for (k = 0; k < s->nknots; k++) {
            const pc_knot *kn = &s->knots[k];
            vh_f64(h, kn->anchor.x);
            vh_f64(h, kn->anchor.y);
            vh_f64(h, kn->in_ctrl.x);
            vh_f64(h, kn->in_ctrl.y);
            vh_f64(h, kn->out_ctrl.x);
            vh_f64(h, kn->out_ctrl.y);
            vh_flag(h, kn->smooth);
        }
    }
}

static inline uint64_t path_hash(const pc_path *p)
{
    uint64_t h = VH_H0;
    hash_path(&h, p);
    return h;
}

static inline size_t hash_polys(uint64_t *h, const pc_vec_polys *v)
{
    size_t n = 0;
    size_t i;
    size_t k;
    vh_u64(h, v->len);
    for (i = 0; i < v->len; i++) {
        vh_u64(h, v->v[i].len);
        n += v->v[i].len;
        for (k = 0; k < v->v[i].len; k++) {
            vh_f64(h, v->v[i].pts[k][0]);
            vh_f64(h, v->v[i].pts[k][1]);
        }
    }
    return n;
}

static inline size_t hash_lines(uint64_t *h, const pc_vec_polyline *v, size_t nv)
{
    size_t n = 0;
    size_t i;
    size_t k;
    vh_u64(h, nv);
    for (i = 0; i < nv; i++) {
        vh_flag(h, v[i].closed);
        vh_u64(h, v[i].len);
        n += v[i].len;
        for (k = 0; k < v[i].len; k++) {
            vh_f64(h, v[i].pts[k][0]);
            vh_f64(h, v[i].pts[k][1]);
            vh_flag(h, v[i].knot[k]);
        }
    }
    return n;
}

static inline uint64_t cov_hash(const float *v, size_t n)
{
    uint64_t h = VH_H0;
    size_t i;
    for (i = 0; i < n; i++) {
        vh_f32(&h, v[i]);
    }
    return h;
}

static inline uint64_t rgba_hash(const float (*v)[4], size_t n)
{
    uint64_t h = VH_H0;
    size_t i;
    int c;
    for (i = 0; i < n; i++) {
        for (c = 0; c < 4; c++) {
            vh_f32(&h, v[i][c]);
        }
    }
    return h;
}

static inline const char *bounds_str(bool has, pc_rect r, char *buf, size_t n)
{
    if (!has) {
        (void)snprintf(buf, n, "none");
    } else {
        (void)snprintf(buf, n, "%d,%d,%d,%d", r.x0, r.y0, r.x1, r.y1);
    }
    return buf;
}

static inline pc_path_op op_of(uint64_t i)
{
    return (pc_path_op)(i % 5);
}

/* ---- the corpus (vector_vectors.rs corpus()) ---- */

static inline pc_path rand_path(vh_rng *rng, size_t nsub, size_t nk, double scale)
{
    pc_path p;
    size_t s;
    memset(&p, 0, sizeof p);
    for (s = 0; s < nsub; s++) {
        pc_subpath sp;
        size_t k;
        memset(&sp, 0, sizeof sp);
        sp.closed = vh_f(rng) < 0.75;
        sp.op = op_of(vh_below(rng, 5));
        sp.knots = calloc(nk, sizeof *sp.knots);
        sp.nknots = sp.knots != NULL ? nk : 0;
        for (k = 0; k < sp.nknots; k++) {
            double ax = vh_f(rng) * scale;
            double ay = vh_f(rng) * scale;
            uint64_t kind = vh_below(rng, 3);
            pc_point a = pc_point_make(ax, ay);
            if (kind == 0) {
                sp.knots[k] = pc_knot_corner(ax, ay);
            } else if (kind == 1) {
                double dx = (vh_f(rng) - 0.5) * scale * 0.3;
                double dy = (vh_f(rng) - 0.5) * scale * 0.3;
                sp.knots[k] =
                    pc_knot_smooth(a, pc_point_make(ax - dx, ay - dy), pc_point_make(ax + dx, ay + dy));
            } else {
                double ix = ax + (vh_f(rng) - 0.5) * scale * 0.4;
                double iy = ay + (vh_f(rng) - 0.5) * scale * 0.4;
                double ox = ax + (vh_f(rng) - 0.5) * scale * 0.4;
                double oy = ay + (vh_f(rng) - 0.5) * scale * 0.4;
                sp.knots[k].anchor = a;
                sp.knots[k].in_ctrl = pc_point_make(ix, iy);
                sp.knots[k].out_ctrl = pc_point_make(ox, oy);
                sp.knots[k].smooth = false;
            }
        }
        vh_push(&p, sp);
    }
    if (vh_below(rng, 4) == 0) {
        p.fill_rule = PC_FILL_RULE_EVENODD;
    }
    if (vh_below(rng, 6) == 0) {
        p.inverted = true;
    }
    return p;
}

#define NCORPUS 36

static pc_path corpus[NCORPUS];

static inline void build_corpus(void)
{
    size_t n = 0;
    size_t i;
    {
        static const double radii[4] = {10.0, 0.0, 15.0, 3.0};
        corpus[n++] = vh_rect(10.3, 20.7, 100.45, 50.2);
        corpus[n++] = vh_ellipse(10.0, 10.0, 80.0, 80.0);
        (void)pc_vec_shape_rounded_rect(5.0, 5.0, 60.0, 40.0, radii, &corpus[n++], NULL);
        (void)pc_vec_shape_polygon(3.0, 5.0, 90.0, 80.0, 7, 0.45, &corpus[n++], NULL);
        corpus[n++] = vh_pentagram();
        corpus[n] = vh_pentagram();
        corpus[n++].fill_rule = PC_FILL_RULE_EVENODD;
    }
    for (i = 0; i < 5; i++) {
        corpus[n++] = vh_two_squares(op_of(i));
    }
    corpus[n] = vh_two_squares(PC_PATH_INTERSECT);
    corpus[n++].inverted = true;
    {
        pc_path d = vh_ellipse(10.0, 10.0, 80.0, 80.0);
        pc_path inner = vh_ellipse(30.0, 30.0, 40.0, 40.0);
        vh_push(&d, vh_take0(&inner, PC_PATH_SUBTRACT));
        corpus[n++] = d;
    }
    corpus[n++] = vh_curve();
    corpus[n++] = vh_tri();
    memset(&corpus[n++], 0, sizeof(pc_path));
    {
        static const double one[1][2] = {{5.0, 5.0}};
        static const double two[2][2] = {{5.0, 5.0}, {5.0, 5.0}};
        double nanp[4][2] = {{5.0, 5.0}, {NAN, 9.0}, {40.0, 30.0}, {10.0, 40.0}};
        double infp[4][2] = {{5.0, 5.0}, {INFINITY, 9.0}, {40.0, 30.0}, {10.0, -INFINITY}};
        static const double a[2] = {3.0, 4.0};
        static const double b[2] = {90.0, 70.0};
        static const double open[4][2] = {{10.0, 10.0}, {60.0, 10.0}, {60.0, 60.0}, {10.0, 60.0}};
        corpus[n++] = vh_poly_path(one, 1, true);
        corpus[n++] = vh_poly_path(two, 2, true);
        corpus[n++] = vh_poly_path((const double (*)[2])nanp, 4, true);
        corpus[n++] = vh_poly_path((const double (*)[2])infp, 4, true);
        corpus[n++] = vh_ellipse(20.0, 20.0, 0.001, 0.002);
        corpus[n++] = vh_ellipse(-1e7, -1e7, 2e7 + 60.0, 2e7 + 60.0);
        (void)pc_vec_shape_line(a, b, 6.5, &corpus[n++], NULL);
        (void)pc_vec_shape_polygon(0.0, 0.0, 100.0, 100.0, 100, 0.9, &corpus[n++], NULL);
        corpus[n++] = vh_poly_path(open, 4, false);
    }
    {
        static const double cp[2][2] = {{0.0, 0.0}, {80.0, 0.0}};
        static const double lp[2][2] = {{10.0, 50.0}, {90.0, 50.0}};
        pc_subpath s[2];
        s[0] = vh_polygon(cp, 2, false);
        s[0].knots[0].out_ctrl = pc_point_make(30.0, 0.0);
        s[0].knots[1].in_ctrl = pc_point_make(60.0, 0.0);
        s[1] = vh_polygon(lp, 2, true);
        s[1].knots[0].out_ctrl = pc_point_make(120.0, -20.0);
        s[1].knots[1].in_ctrl = pc_point_make(-20.0, -20.0);
        s[1].knots[1].out_ctrl = pc_point_make(50.0, 120.0);
        corpus[n++] = vh_path(s, 2);
    }
    {
        vh_rng rng = {0x5eed0001u};
        for (i = 0; i < 10; i++) {
            corpus[n++] = rand_path(&rng, 1 + i % 4, 3 + i % 6, 100.0);
        }
    }
    CHECK_EQ_UINT(n, NCORPUS);
}

static inline void free_corpus(void)
{
    size_t i;
    for (i = 0; i < NCORPUS; i++) {
        pc_path_fini(&corpus[i]);
    }
}

/* Index of the first golden line of the R8 sections (test_parity_r8.c), ngold when absent. */
static inline size_t r8_first_line(void)
{
    size_t i;
    for (i = 0; i < ngold; i++) {
        if (strncmp(gold[i], "validate ", 9) == 0) {
            return i;
        }
    }
    return ngold;
}

static inline void free_golden(void)
{
    size_t i;
    for (i = 0; i < ngold; i++) {
        free(gold[i]);
    }
    free(gold);
    gold = NULL;
    ngold = 0;
    gi = 0;
}

#endif
