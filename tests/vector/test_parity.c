/* Bit-exact parity with crates/vector: tests/golden/vector-vectors.txt was written by
 * `rustref vector-vectors` (tools/rustref/src/vector_vectors.rs). This file rebuilds the same
 * inputs with the same LCG, recomputes every line and compares it with the golden one:
 * flattening, fill coverage (every op / rule / inversion, clipped and parallel renders),
 * stroke outlines and their coverage, dashes, shape generators, point edits and hit tests,
 * tracing, gradient paint, shape layers rendered into surfaces of 7 formats, coverage
 * surfaces and vector masks. Hashes are FNV-1a over little-endian bit patterns. */
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

static void load_golden(void)
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

static void expect(const char *got)
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

static void hash_path(uint64_t *h, const pc_path *p)
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

static uint64_t path_hash(const pc_path *p)
{
    uint64_t h = VH_H0;
    hash_path(&h, p);
    return h;
}

static size_t hash_polys(uint64_t *h, const pc_vec_polys *v)
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

static size_t hash_lines(uint64_t *h, const pc_vec_polyline *v, size_t nv)
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

static uint64_t cov_hash(const float *v, size_t n)
{
    uint64_t h = VH_H0;
    size_t i;
    for (i = 0; i < n; i++) {
        vh_f32(&h, v[i]);
    }
    return h;
}

static uint64_t rgba_hash(const float (*v)[4], size_t n)
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

static const char *bounds_str(bool has, pc_rect r, char *buf, size_t n)
{
    if (!has) {
        (void)snprintf(buf, n, "none");
    } else {
        (void)snprintf(buf, n, "%d,%d,%d,%d", r.x0, r.y0, r.x1, r.y1);
    }
    return buf;
}

static const pc_line_cap CAPS[3] = {PC_LINE_CAP_BUTT, PC_LINE_CAP_ROUND, PC_LINE_CAP_SQUARE};
static const pc_line_join JOINS[3] = {PC_LINE_JOIN_MITER, PC_LINE_JOIN_ROUND, PC_LINE_JOIN_BEVEL};
static const pc_gradient_style STYLES[5] = {PC_GRADIENT_LINEAR, PC_GRADIENT_RADIAL, PC_GRADIENT_ANGLE,
                                            PC_GRADIENT_REFLECTED, PC_GRADIENT_DIAMOND};

static pc_path_op op_of(uint64_t i)
{
    return (pc_path_op)(i % 5);
}

/* ---- the corpus (vector_vectors.rs corpus()) ---- */

static pc_path rand_path(vh_rng *rng, size_t nsub, size_t nk, double scale)
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

static void build_corpus(void)
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

static void free_corpus(void)
{
    size_t i;
    for (i = 0; i < NCORPUS; i++) {
        pc_path_fini(&corpus[i]);
    }
}

/* ---- stroke styles ---- */

#define NSTYLES 30
static pc_vec_stroke_style styles[NSTYLES];

static void build_styles(void)
{
    static const double widths[5] = {0.5, 1.0, 3.0, 10.0, 40.0};
    static const double limits[3] = {1.0, 4.0, 100.0};
    static const double d1[2] = {8.0, 4.0};
    static const double d2[2] = {0.0, 12.0};
    static const double d3[3] = {3.0, 1.0, 2.0};
    static const double d4[4] = {5.0, -1.0, 0.0, 7.0};
    static const double *const dashes[5] = {NULL, d1, d2, d3, d4};
    static const size_t ndashes[5] = {0, 2, 2, 3, 4};
    static const double offsets[3] = {0.0, 5.5, -3.0};
    vh_rng rng = {0x570e5u};
    size_t i;
    for (i = 0; i < NSTYLES; i++) {
        pc_vec_stroke_style *s = &styles[i];
        uint64_t di;
        pc_vec_stroke_style_default(s);
        s->width = widths[i % 5];
        s->cap = CAPS[(i / 5) % 3];
        s->join = JOINS[vh_below(&rng, 3)];
        s->miter_limit = limits[vh_below(&rng, 3)];
        di = vh_below(&rng, 5);
        if (ndashes[di] > 0) {
            s->dashes = malloc(ndashes[di] * sizeof *s->dashes);
            if (s->dashes != NULL) {
                memcpy(s->dashes, dashes[di], ndashes[di] * sizeof *s->dashes);
                s->ndashes = ndashes[di];
            }
        }
        s->dash_offset = offsets[vh_below(&rng, 3)];
    }
}

/* ---- sections ---- */

static void paths_section(void)
{
    size_t i;
    for (i = 0; i < NCORPUS; i++) {
        EXPECTF("path %zu %016" PRIx64, i, path_hash(&corpus[i]));
    }
}

static void flatten_section(void)
{
    const double tols[6] = {1.0, 0.25, 0.01, 0.001, 0.0, NAN};
    size_t i;
    size_t t;
    for (i = 0; i < NCORPUS; i++) {
        for (t = 0; t < 6; t++) {
            pc_vec_polyline *lines = NULL;
            size_t nl = 0;
            uint64_t h = VH_H0;
            size_t n;
            CHECK(pc_vec_flatten_path(&corpus[i], tols[t], &lines, &nl, NULL) == PC_OK);
            n = hash_lines(&h, lines, nl);
            EXPECTF("flatten %zu %zu %zu %016" PRIx64, i, t, n, h);
            pc_vec_polylines_free(lines, nl);
        }
    }
}

static uint64_t render_hash(const pc_vec_rasterizer *r, pc_rect rect)
{
    float *v = NULL;
    size_t n = 0;
    uint64_t h;
    CHECK(pc_vec_rasterizer_render(r, rect, &v, &n, NULL) == PC_OK);
    h = cov_hash(v, n);
    free(v);
    return h;
}

static void fill_section(void)
{
    size_t i;
    char b[64];
    for (i = 0; i < NCORPUS; i++) {
        pc_vec_rasterizer r;
        pc_rect pb;
        bool has;
        CHECK(pc_vec_fill_rasterizer(&corpus[i], PC_VEC_DEFAULT_TOLERANCE, &r, NULL) == PC_OK);
        has = pc_vec_rasterizer_pixel_bounds(&r, &pb);
        EXPECTF("fill %zu %s %016" PRIx64 " %016" PRIx64 " %016" PRIx64, i, bounds_str(has, pb, b, sizeof b),
                render_hash(&r, pc_rect_make(-5, -5, 125, 125)),
                render_hash(&r, pc_rect_make(13, 29, 50, 52)), render_hash(&r, pc_rect_make(40, 40, 41, 41)));
        pc_vec_rasterizer_fini(&r);
    }
    {
        vh_rng rng = {0xb16u};
        for (i = 0; i < 3; i++) {
            pc_path p = rand_path(&rng, 2 + i, 7, 400.0);
            pc_vec_rasterizer r;
            CHECK(pc_vec_fill_rasterizer(&p, PC_VEC_DEFAULT_TOLERANCE, &r, NULL) == PC_OK);
            EXPECTF("fillbig %zu %016" PRIx64, i, render_hash(&r, pc_rect_make(-3, -2, 397, 405)));
            pc_vec_rasterizer_fini(&r);
            pc_path_fini(&p);
        }
    }
    {
        double max = (double)INT32_MAX;
        double sq[4][2] = {
            {max - 2.0, max - 2.0}, {max + 2.0, max - 2.0}, {max + 2.0, max + 2.0}, {max - 2.0, max + 2.0}};
        double tri3[3][2] = {{-1e12, 3.0}, {1e12, 3.5}, {0.0, 1e12}};
        pc_vec_poly poly;
        pc_vec_rasterizer r;
        pc_rect pb;
        bool has;
        pc_vec_rasterizer_init(&r, false);
        poly.pts = sq;
        poly.len = 4;
        CHECK(pc_vec_rasterizer_add_component(&r, &poly, 1, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, NULL) ==
              PC_OK);
        has = pc_vec_rasterizer_pixel_bounds(&r, &pb);
        EXPECTF("bounds max %s", bounds_str(has, pb, b, sizeof b));
        pc_vec_rasterizer_fini(&r);
        pc_vec_rasterizer_init(&r, false);
        poly.pts = tri3;
        poly.len = 3;
        CHECK(pc_vec_rasterizer_add_component(&r, &poly, 1, PC_PATH_COMBINE, PC_FILL_RULE_NONZERO, NULL) ==
              PC_OK);
        has = pc_vec_rasterizer_pixel_bounds(&r, &pb);
        EXPECTF("bounds huge %s %016" PRIx64, bounds_str(has, pb, b, sizeof b),
                render_hash(&r, pc_rect_make(-10, 0, 30, 20)));
        pc_vec_rasterizer_fini(&r);
    }
    {
        vh_rng rng = {0xc0c0u};
        for (i = 0; i < 12; i++) {
            pc_vec_rasterizer r;
            size_t c;
            pc_vec_rasterizer_init(&r, i % 5 == 4);
            for (c = 0; c < 1 + i % 4; c++) {
                pc_vec_poly polys[3];
                double pts[3][8][2];
                size_t np = 1 + (size_t)vh_below(&rng, 3);
                size_t k;
                pc_path_op op;
                pc_fill_rule rule;
                for (k = 0; k < np; k++) {
                    size_t m = 3 + (size_t)vh_below(&rng, 5);
                    size_t q;
                    for (q = 0; q < m; q++) {
                        pts[k][q][0] = vh_f(&rng) * 60.0 - 5.0;
                        pts[k][q][1] = vh_f(&rng) * 60.0 - 5.0;
                    }
                    polys[k].pts = pts[k];
                    polys[k].len = m;
                }
                op = op_of(vh_below(&rng, 5));
                rule = vh_below(&rng, 2) == 0 ? PC_FILL_RULE_NONZERO : PC_FILL_RULE_EVENODD;
                CHECK(pc_vec_rasterizer_add_component(&r, polys, np, op, rule, NULL) == PC_OK);
            }
            EXPECTF("comp %zu %016" PRIx64, i, render_hash(&r, pc_rect_make(-7, -7, 60, 60)));
            pc_vec_rasterizer_fini(&r);
        }
    }
}

static void stroke_section(void)
{
    size_t pi;
    size_t si;
    char b[64];
    for (pi = 0; pi < NCORPUS; pi++) {
        if (pi % 2 == 1 && pi > 12) {
            continue;
        }
        for (si = 0; si < NSTYLES; si++) {
            pc_vec_polyline *lines = NULL;
            size_t nl = 0;
            pc_vec_polys polys = {NULL, 0, 0};
            pc_vec_rasterizer r;
            uint64_t h = VH_H0;
            size_t n;
            pc_rect pb;
            bool has;
            if ((pi + si) % 3 != 0) {
                continue;
            }
            CHECK(pc_vec_flatten_path(&corpus[pi], 0.05, &lines, &nl, NULL) == PC_OK);
            CHECK(pc_vec_stroke_polygons(lines, nl, &styles[si], 0.01, &polys, NULL) == PC_OK);
            n = hash_polys(&h, &polys);
            CHECK(pc_vec_stroke_rasterizer(&corpus[pi], &styles[si], PC_VEC_DEFAULT_TOLERANCE, &r, NULL) ==
                  PC_OK);
            has = pc_vec_rasterizer_pixel_bounds(&r, &pb);
            EXPECTF("stroke %zu %zu %zu %016" PRIx64 " %s %016" PRIx64, pi, si, n, h,
                    bounds_str(has, pb, b, sizeof b), render_hash(&r, pc_rect_make(-25, -25, 135, 135)));
            pc_vec_rasterizer_fini(&r);
            pc_vec_polys_fini(&polys);
            pc_vec_polylines_free(lines, nl);
        }
    }
    {
        double p0[2][2] = {{0.0, 0.0}, {10.0, 0.0}};
        bool k0[2] = {true, true};
        double p1[4][2] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 7.0}, {3.0, 9.0}};
        bool k1[4] = {true, false, true, false};
        double p2[1][2] = {{1.0, 1.0}};
        bool k2[1] = {true};
        double p3[3][2] = {{0.0, 0.0}, {0.0, 0.0}, {5.0, 5.0}};
        bool k3[3] = {true, true, true};
        pc_vec_polyline pls[4] = {
            {p0, k0, 2, 2, false}, {p1, k1, 4, 4, true}, {p2, k2, 1, 1, false}, {p3, k3, 3, 3, false}};
        static const double a0[2] = {2.0, 1.0};
        static const double a1[1] = {2.0};
        static const double a2[2] = {0.0, 3.0};
        static const double a3[2] = {1.0, 0.0};
        static const double a4[2] = {0.0, 0.0};
        double a5[2] = {NAN, 2.0};
        static const double a6[3] = {0.3, 0.2, 0.1};
        const double *pats[7] = {a0, a1, a2, a3, a4, a5, a6};
        const size_t npats[7] = {2, 1, 2, 2, 2, 2, 3};
        const double offs[6] = {0.0, 1.0, -2.5, 1e9, NAN, INFINITY};
        size_t li;
        size_t ki;
        size_t oi;
        for (li = 0; li < 4; li++) {
            for (ki = 0; ki < 7; ki++) {
                for (oi = 0; oi < 6; oi++) {
                    pc_vec_polyline *d = NULL;
                    size_t nd = 0;
                    uint64_t h = VH_H0;
                    size_t n;
                    CHECK(pc_vec_dash_polyline(&pls[li], pats[ki], npats[ki], offs[oi], &d, &nd, NULL) ==
                          PC_OK);
                    n = hash_lines(&h, d, nd);
                    EXPECTF("dash %zu %zu %zu %zu %016" PRIx64, li, ki, oi, n, h);
                    pc_vec_polylines_free(d, nd);
                }
            }
        }
    }
    {
        float d0[2] = {2.0f, 1.0f};
        float d2[1] = {NAN};
        float d3[3] = {0.0f, 3.0f, 1.5f};
        struct {
            float w, m;
            float *d;
            size_t nd;
            float off;
        } cases[4] = {{4.0f, 4.0f, d0, 2, 0.5f},
                      {-1.0f, 0.2f, NULL, 0, 0.0f},
                      {NAN, NAN, d2, 1, 3.0f},
                      {0.1f, 100.0f, d3, 3, -2.0f}};
        size_t i;
        for (i = 0; i < 4; i++) {
            pc_shape_stroke ss;
            pc_vec_stroke_style s;
            uint64_t h = VH_H0;
            size_t k;
            pc_shape_stroke_default(&ss);
            ss.width = cases[i].w;
            ss.miter_limit = cases[i].m;
            ss.dashes = cases[i].d;
            ss.ndashes = cases[i].nd;
            ss.dash_offset = cases[i].off;
            CHECK(pc_vec_stroke_style_from(&ss, &s, NULL) == PC_OK);
            vh_f64(&h, s.width);
            vh_f64(&h, s.miter_limit);
            vh_f64(&h, s.dash_offset);
            vh_u64(&h, s.ndashes);
            for (k = 0; k < s.ndashes; k++) {
                vh_f64(&h, s.dashes[k]);
            }
            EXPECTF("style %zu %016" PRIx64, i, h);
            pc_vec_stroke_style_fini(&s);
        }
    }
}

static void shapes_section(void)
{
    const double radii[7][4] = {{10.0, 10.0, 10.0, 10.0}, {0.0, 0.0, 0.0, 0.0},  {10.0, 0.0, 10.0, 0.0},
                                {100.0, 5.0, 0.0, 30.0},  {-3.0, NAN, 2.0, 1e9}, {25.0, 25.0, 25.0, 25.0},
                                {0.0, 0.0, 0.0, 1e-13}};
    const double rects[4][4] = {
        {0.0, 0.0, 100.0, 50.0}, {5.5, -3.25, -40.0, 20.0}, {0.0, 0.0, 0.0, 10.0}, {1.0, 2.0, 50.0, 50.0}};
    const uint32_t sides[6] = {0, 3, 5, 6, 100, 1000};
    const double stars[6] = {1.0, 0.5, 0.0, -1.0, 0.999, NAN};
    size_t ri;
    for (ri = 0; ri < 4; ri++) {
        const double *r = rects[ri];
        pc_path p;
        size_t ki;
        size_t s;
        size_t t;
        p = vh_rect(r[0], r[1], r[2], r[3]);
        EXPECTF("rect %zu %016" PRIx64, ri, path_hash(&p));
        pc_path_fini(&p);
        p = vh_ellipse(r[0], r[1], r[2], r[3]);
        EXPECTF("ellipse %zu %016" PRIx64, ri, path_hash(&p));
        pc_path_fini(&p);
        for (ki = 0; ki < 7; ki++) {
            double c[4];
            uint64_t h = VH_H0;
            int q;
            CHECK(pc_vec_shape_rounded_rect(r[0], r[1], r[2], r[3], radii[ki], &p, NULL) == PC_OK);
            EXPECTF("rrect %zu %zu %016" PRIx64, ri, ki, path_hash(&p));
            pc_path_fini(&p);
            pc_vec_clamp_radii(r[2], r[3], radii[ki], c);
            for (q = 0; q < 4; q++) {
                vh_f64(&h, c[q]);
            }
            EXPECTF("clamp %zu %zu %016" PRIx64, ri, ki, h);
        }
        for (s = 0; s < 6; s++) {
            for (t = 0; t < 6; t++) {
                CHECK(pc_vec_shape_polygon(r[0], r[1], r[2], r[3], sides[s], stars[t], &p, NULL) == PC_OK);
                EXPECTF("polygon %zu %" PRIu32 " %zu %016" PRIx64, ri, sides[s], t, path_hash(&p));
                pc_path_fini(&p);
            }
        }
    }
    {
        const double lines[4][5] = {{3.0, 4.0, 90.0, 70.0, 6.5},
                                    {5.0, 5.0, 5.0, 5.0, 2.0},
                                    {0.0, 0.0, 10.0, 0.0, -3.0},
                                    {0.0, 0.0, 0.0, 10.0, NAN}};
        size_t i;
        for (i = 0; i < 4; i++) {
            pc_path p;
            CHECK(pc_vec_shape_line(&lines[i][0], &lines[i][2], lines[i][4], &p, NULL) == PC_OK);
            EXPECTF("line %zu %016" PRIx64, i, path_hash(&p));
            pc_path_fini(&p);
        }
    }
    {
        pc_live_shape lives[4];
        size_t i;
        memset(lives, 0, sizeof lives);
        for (i = 0; i < 4; i++) {
            lives[i].rect[0] = 1.0;
            lives[i].rect[1] = 2.0;
            lives[i].rect[2] = 30.0;
            lives[i].rect[3] = 40.0;
        }
        lives[0].kind = PC_LIVE_RECT;
        lives[0].radii[0] = 3.0;
        lives[0].radii[1] = 4.0;
        lives[0].radii[2] = 5.0;
        lives[0].radii[3] = 6.0;
        lives[1].kind = PC_LIVE_ELLIPSE;
        lives[2].kind = PC_LIVE_POLYGON;
        lives[2].sides = 5;
        lives[2].star_ratio = 0.4;
        lives[3].kind = PC_LIVE_LINE;
        lives[3].from[0] = 1.0;
        lives[3].from[1] = 2.0;
        lives[3].to[0] = 30.0;
        lives[3].to[1] = 40.0;
        lives[3].weight = 3.0;
        for (i = 0; i < 4; i++) {
            pc_path p;
            CHECK(pc_vec_live_path(&lives[i], &p, NULL) == PC_OK);
            EXPECTF("live %zu %016" PRIx64, i, path_hash(&p));
            pc_path_fini(&p);
        }
    }
}

static void hit_str(bool found, const pc_vec_hit *h, char *buf, size_t n)
{
    uint64_t tb;
    if (!found) {
        (void)snprintf(buf, n, "none");
        return;
    }
    switch (h->kind) {
    case PC_VEC_HIT_ANCHOR:
        (void)snprintf(buf, n, "a%zu,%zu", h->knot.sub, h->knot.knot);
        break;
    case PC_VEC_HIT_HANDLE:
        (void)snprintf(buf, n, "h%zu,%zu,%s", h->knot.sub, h->knot.knot, pc_vec_handle_name(h->handle));
        break;
    case PC_VEC_HIT_SEGMENT:
    default:
        memcpy(&tb, &h->t, 8);
        (void)snprintf(buf, n, "s%zu,%zu,%016" PRIx64, h->knot.sub, h->knot.knot, tb);
        break;
    }
}

static void res_str(pc_status st, const pc_error *e, const pc_path *p, char *buf, size_t n)
{
    if (st == PC_OK) {
        (void)snprintf(buf, n, "%016" PRIx64, path_hash(p));
    } else {
        (void)snprintf(buf, n, "ERR %s", e->msg);
    }
}

static pc_vec_knot_ref kr(size_t s, size_t k)
{
    pc_vec_knot_ref r;
    r.sub = s;
    r.knot = k;
    return r;
}

static void edit_section(void)
{
    const double ts[9] = {0.0, 0.05, 0.3, 0.5, 0.95, 1.0, NAN, INFINITY, -2.0};
    pc_path bases[6];
    size_t pi;
    char rs[512];
    bases[0] = vh_curve();
    bases[1] = vh_tri();
    bases[2] = vh_clone(&corpus[2]);
    bases[3] = vh_clone(&corpus[1]);
    bases[4] = vh_clone(&corpus[25]);
    bases[5] = vh_clone(&corpus[30]);
    for (pi = 0; pi < 6; pi++) {
        const pc_path *base = &bases[pi];
        pc_path p;
        pc_error e;
        pc_status st;
        size_t hi;
        size_t nk;
        size_t ks[5];
        size_t q;
        static const double dm[2] = {5.0, -2.0};
        static const double d1[2] = {1.0, 1.0};
        static const double db[2] = {3.0, 12.0};
        {
            pc_vec_knot_ref l[3];
            l[0] = kr(0, 1);
            l[1] = kr(0, 1);
            l[2] = kr(0, 0);
            p = vh_clone(base);
            st = pc_vec_move_anchors(&p, l, 3, dm, &e);
            res_str(st, &e, &p, rs, sizeof rs);
            EXPECTF("move %zu %s", pi, rs);
            pc_path_fini(&p);
            l[0] = kr(0, 0);
            l[1] = kr(0, 99);
            p = vh_clone(base);
            st = pc_vec_move_anchors(&p, l, 2, d1, &e);
            res_str(st, &e, &p, rs, sizeof rs);
            EXPECTF("move %zu bad %s", pi, rs);
            pc_path_fini(&p);
        }
        for (hi = 0; hi < 2; hi++) {
            int ind;
            for (ind = 0; ind < 2; ind++) {
                p = vh_clone(base);
                st = pc_vec_move_handle(&p, kr(0, 1), hi == 0 ? PC_VEC_HANDLE_IN : PC_VEC_HANDLE_OUT,
                                        pc_point_make(50.0, 30.0), ind != 0, &e);
                res_str(st, &e, &p, rs, sizeof rs);
                EXPECTF("handle %zu %zu %d %s", pi, hi, ind, rs);
                pc_path_fini(&p);
            }
        }
        {
            pc_point o = pc_point_make(10.0, 60.0);
            p = vh_clone(base);
            st = pc_vec_convert_point(&p, kr(0, 1), NULL, &e);
            res_str(st, &e, &p, rs, sizeof rs);
            EXPECTF("convert %zu none %s", pi, rs);
            pc_path_fini(&p);
            p = vh_clone(base);
            st = pc_vec_convert_point(&p, kr(0, 0), &o, &e);
            res_str(st, &e, &p, rs, sizeof rs);
            EXPECTF("convert %zu some %s", pi, rs);
            pc_path_fini(&p);
            p = vh_clone(base);
            st = pc_vec_convert_point(&p, kr(SIZE_MAX, SIZE_MAX), NULL, &e);
            res_str(st, &e, &p, rs, sizeof rs);
            EXPECTF("convert %zu bad %s", pi, rs);
            pc_path_fini(&p);
        }
        nk = base->nsubpaths > 0 ? base->subpaths[0].nknots : 0;
        ks[0] = 0;
        ks[1] = 1;
        ks[2] = nk > 0 ? nk - 1 : 0;
        ks[3] = nk;
        ks[4] = SIZE_MAX;
        for (q = 0; q < 5; q++) {
            size_t ti;
            for (ti = 0; ti < 9; ti++) {
                p = vh_clone(base);
                st = pc_vec_bend_segment(&p, kr(0, ks[q]), ts[ti], db, &e);
                res_str(st, &e, &p, rs, sizeof rs);
                EXPECTF("bend %zu %zu %zu %s", pi, ks[q], ti, rs);
                pc_path_fini(&p);
            }
        }
        p = vh_clone(base);
        st = pc_vec_bend_segment(&p, kr(7, 0), 0.5, db, &e);
        res_str(st, &e, &p, rs, sizeof rs);
        EXPECTF("bend %zu badsub %s", pi, rs);
        pc_path_fini(&p);
        {
            const double boxes[4][4] = {{110.0, -5.0, 40.0, 5.0},
                                        {0.0, 0.0, 100.0, 100.0},
                                        {NAN, 0.0, 100.0, 100.0},
                                        {-1e9, -1e9, 1e9, 1e9}};
            size_t bi;
            for (bi = 0; bi < 4; bi++) {
                pc_vec_knot_ref *v = NULL;
                size_t n = 0;
                size_t k;
                size_t used = 0;
                rs[0] = '\0';
                CHECK(pc_vec_anchors_in(base, boxes[bi], &v, &n, NULL) == PC_OK);
                for (k = 0; k < n; k++) {
                    int w = snprintf(rs + used, sizeof rs - used, "%s%zu.%zu", k > 0 ? "," : "", v[k].sub,
                                     v[k].knot);
                    if (w > 0 && (size_t)w < sizeof rs - used) {
                        used += (size_t)w;
                    }
                }
                EXPECTF("box %zu %zu %s", pi, bi, rs);
                free(v);
            }
        }
        {
            const size_t subs[3] = {0, 1, 4};
            size_t s;
            for (s = 0; s < 3; s++) {
                pc_vec_knot_ref *v = NULL;
                size_t n = 0;
                CHECK(pc_vec_subpath_anchors(base, subs[s], &v, &n, NULL) == PC_OK);
                EXPECTF("subanchors %zu %zu %zu", pi, subs[s], n);
                free(v);
            }
        }
        {
            pc_vec_handle_ref shown[3];
            uint64_t h = VH_H0;
            int gx;
            int gy;
            pc_vec_hit hit;
            bool f;
            char hs[128];
            shown[0].knot = kr(0, 1);
            shown[0].handle = PC_VEC_HANDLE_OUT;
            shown[1].knot = kr(0, 1);
            shown[1].handle = PC_VEC_HANDLE_IN;
            shown[2].knot = kr(9, 9);
            shown[2].handle = PC_VEC_HANDLE_IN;
            for (gy = 0; gy < 12; gy++) {
                for (gx = 0; gx < 12; gx++) {
                    pc_point pt = pc_point_make((double)gx * 9.0 - 4.0, (double)gy * 9.0 - 4.0);
                    f = pc_vec_hit_test(base, pt, 3.0, shown, 3, &hit);
                    hit_str(f, &hit, hs, sizeof hs);
                    vh_b(&h, hs, strlen(hs));
                    f = pc_vec_hit_test(base, pt, 8.0, NULL, 0, &hit);
                    hit_str(f, &hit, hs, sizeof hs);
                    vh_b(&h, hs, strlen(hs));
                    f = pc_vec_hit_test(base, pt, 0.0, shown, 3, &hit);
                    hit_str(f, &hit, hs, sizeof hs);
                    vh_b(&h, hs, strlen(hs));
                }
            }
            EXPECTF("hit %zu %016" PRIx64, pi, h);
            f = pc_vec_hit_test(base, pc_point_make(71.0, 1.0), 3.0, shown, 3, &hit);
            hit_str(f, &hit, hs, sizeof hs);
            EXPECTF("hit1 %zu %s", pi, hs);
            f = pc_vec_hit_test(base, pc_point_make(50.0, 1.0), 3.0, NULL, 0, &hit);
            hit_str(f, &hit, hs, sizeof hs);
            EXPECTF("hit2 %zu %s", pi, hs);
            f = pc_vec_hit_test(base, pc_point_make(NAN, 1.0), 3.0, shown, 3, &hit);
            hit_str(f, &hit, hs, sizeof hs);
            EXPECTF("hit3 %zu %s", pi, hs);
        }
    }
    for (pi = 0; pi < 6; pi++) {
        pc_path_fini(&bases[pi]);
    }
    {
        const char *in[5] = {"in", "out", "In", "", "outer"};
        size_t i;
        for (i = 0; i < 5; i++) {
            pc_vec_handle h;
            bool ok = pc_vec_handle_parse(in[i], strlen(in[i]), &h);
            EXPECTF("handle_parse \"%s\" %s", in[i], ok ? pc_vec_handle_name(h) : "none");
        }
    }
}

/* ---- tracing ---- */

typedef struct mask {
    pc_rect rect;
    float *v;
    size_t n;
    float outside;
} mask;

static size_t build_masks(mask *out)
{
    size_t n = 0;
    size_t i;
    pc_rect rect = pc_rect_make(0, 0, 160, 120);
    {
        pc_path src = vh_ellipse(10.0, 10.0, 90.0, 90.0);
        pc_path in = vh_ellipse(35.0, 35.0, 40.0, 40.0);
        pc_path r = vh_rect(110.0, 20.0, 40.0, 80.0);
        float *m = NULL;
        size_t len = 0;
        float *hard;
        vh_push(&src, vh_take0(&in, PC_PATH_SUBTRACT));
        vh_push(&src, vh_take0(&r, PC_PATH_COMBINE));
        CHECK(pc_vec_path_coverage(&src, rect, &m, &len, NULL) == PC_OK);
        pc_path_fini(&src);
        hard = malloc(len * sizeof *hard);
        if (hard != NULL) {
            for (i = 0; i < len; i++) {
                hard[i] = m[i] >= 0.5f ? 1.0f : 0.0f;
            }
        }
        out[n++] = (mask){rect, m, len, 0.0f};
        out[n++] = (mask){rect, hard, hard != NULL ? len : 0, 0.0f};
    }
    {
        pc_rect r2 = pc_rect_make(-7, 3, 33, 33);
        vh_rng rng = {0x7aceu};
        float *noise = malloc(1200 * sizeof *noise);
        float *noise2 = malloc(1200 * sizeof *noise2);
        float *blocky = malloc(1200 * sizeof *blocky);
        if (noise == NULL || noise2 == NULL || blocky == NULL) {
            free(noise);
            free(noise2);
            free(blocky);
            return n;
        }
        for (i = 0; i < 1200; i++) {
            noise[i] = (float)vh_f(&rng);
            blocky[i] = (i % 40) / 5 % 2 == (i / 40) / 4 % 2 ? 1.0f : 0.0f;
        }
        memcpy(noise2, noise, 1200 * sizeof *noise);
        out[n++] = (mask){r2, noise, 1200, 0.0f};
        out[n++] = (mask){r2, noise2, 1200, 1.0f};
        out[n++] = (mask){r2, blocky, 1200, 0.0f};
    }
    {
        float *w = malloc(400 * sizeof *w);
        float *one = malloc(sizeof *one);
        float *three = malloc(3 * sizeof *three);
        float *sad = malloc(9 * sizeof *sad);
        static const float sadv[9] = {1, 0, 0, 1, 0, 1, 1, 0, 1};
        int32_t k;
        if (w == NULL || one == NULL || three == NULL || sad == NULL) {
            free(w);
            free(one);
            free(three);
            free(sad);
            return n;
        }
        for (k = 0; k < 400; k++) {
            int32_t a = k % 20 - 10;
            int32_t b = k / 20 - 10;
            w[k] = (a < 0 ? -a : a) + (b < 0 ? -b : b) < 7 ? 1.0f : 0.0f;
        }
        w[5 * 20 + 10] = NAN;
        w[10 * 20 + 10] = INFINITY;
        one[0] = 1.0f;
        three[0] = 0.6f;
        three[1] = 0.4f;
        three[2] = 0.6f;
        memcpy(sad, sadv, sizeof sadv);
        out[n++] = (mask){pc_rect_make(0, 0, 20, 20), w, 400, NAN};
        out[n++] = (mask){pc_rect_make(0, 0, 1, 1), one, 1, 0.0f};
        out[n++] = (mask){pc_rect_make(0, 0, 3, 1), three, 3, 0.5f};
        out[n++] = (mask){pc_rect_make(0, 0, 3, 3), sad, 9, 0.0f};
    }
    return n;
}

static void trace_section(void)
{
    mask ms[9];
    size_t nm = build_masks(ms);
    size_t mi;
    const double tols[5] = {0.5, 1.0, 2.0, 0.0, NAN};
    CHECK_EQ_UINT(nm, 9);
    for (mi = 0; mi < nm; mi++) {
        pc_vec_polys cs = {NULL, 0, 0};
        uint64_t h = VH_H0;
        size_t n;
        size_t ti;
        CHECK(pc_vec_contours(ms[mi].v, ms[mi].n, ms[mi].rect, ms[mi].outside, &cs, NULL) == PC_OK);
        n = hash_polys(&h, &cs);
        EXPECTF("contours %zu %zu %016" PRIx64, mi, n, h);
        pc_vec_polys_fini(&cs);
        for (ti = 0; ti < 5; ti++) {
            pc_path p;
            size_t knots = 0;
            size_t s;
            CHECK(pc_vec_trace_mask(ms[mi].v, ms[mi].n, ms[mi].rect, ms[mi].outside, tols[ti], &p, NULL) ==
                  PC_OK);
            for (s = 0; s < p.nsubpaths; s++) {
                knots += p.subpaths[s].nknots;
            }
            EXPECTF("trace %zu %zu %zu %016" PRIx64, mi, ti, knots, path_hash(&p));
            pc_path_fini(&p);
        }
        free(ms[mi].v);
    }
}

/* ---- paint and shapes ---- */

static pc_shape_layer shape_layer(pc_path path)
{
    pc_shape_layer s;
    memset(&s, 0, sizeof s);
    s.path = path;
    return s;
}

static void shape_line(const char *prefix, const pc_shape_layer *sh, pc_rect canvas, pc_rect r)
{
    pc_vec_compiled_shape cs;
    float (*px)[4] = NULL;
    size_t n = 0;
    pc_rect b;
    bool has;
    char bs[64];
    CHECK(pc_vec_compiled_shape_new(sh, PC_VEC_DEFAULT_TOLERANCE, canvas, &cs, NULL) == PC_OK);
    has = pc_vec_compiled_shape_bounds(&cs, &b);
    CHECK(pc_vec_compiled_shape_render_rgba(&cs, r, &px, &n, NULL) == PC_OK);
    EXPECTF("%s %s %016" PRIx64, prefix, bounds_str(has, b, bs, sizeof bs),
            rgba_hash((const float (*)[4])px, n));
    free(px);
    pc_vec_compiled_shape_fini(&cs);
}

static void paint_section(void)
{
    vh_rng rng = {0x9a17u};
    pc_color colors[5];
    size_t i;
    colors[0] = PC_COLOR_BLACK;
    colors[1] = PC_COLOR_WHITE;
    colors[2] = pc_color_rgb(1.0f, 0.0f, 0.0f);
    colors[3] = pc_color_rgba(0.2f, 0.5f, 0.9f, 0.5f);
    colors[4] = pc_color_gray(0.3f);
    for (i = 0; i < 40; i++) {
        static const float angles[8] = {0.0f, 30.0f, 90.0f, 135.0f, -45.0f, 400.0f, 180.0f, 1e-3f};
        static const float scales[4] = {0.5f, 1.0f, 1.3f, 0.0f};
        static const float ox[3] = {0.0f, 0.1f, -0.3f};
        static const float oy[3] = {0.0f, -0.1f, 0.25f};
        pc_fill fill;
        size_t k;
        size_t nst = 1 + (size_t)vh_below(&rng, 4);
        size_t nop;
        size_t nmid;
        pc_shape_layer sh;
        pc_path path;
        char prefix[32];
        memset(&fill, 0, sizeof fill);
        fill.kind = PC_FILL_GRADIENT;
        fill.u.gradient.stops = calloc(nst, sizeof *fill.u.gradient.stops);
        if (fill.u.gradient.stops == NULL) {
            return;
        }
        fill.u.gradient.nstops = nst;
        for (k = 0; k < nst; k++) {
            fill.u.gradient.stops[k].pos = (float)vh_f(&rng) * 1.2f - 0.1f;
            fill.u.gradient.stops[k].color = colors[vh_below(&rng, 5)];
        }
        nop = (size_t)vh_below(&rng, 3);
        if (nop > 0) {
            fill.u.gradient.opacity_stops = calloc(nop, sizeof *fill.u.gradient.opacity_stops);
            fill.u.gradient.nopacity_stops = fill.u.gradient.opacity_stops != NULL ? nop : 0;
        }
        for (k = 0; k < nop; k++) {
            float pos = (float)vh_f(&rng);
            float op = (float)vh_f(&rng);
            if (fill.u.gradient.opacity_stops != NULL) {
                fill.u.gradient.opacity_stops[k].pos = pos;
                fill.u.gradient.opacity_stops[k].opacity = op;
            }
        }
        nmid = (size_t)vh_below(&rng, 3);
        if (nmid > 0) {
            fill.u.gradient.midpoints = calloc(nmid, sizeof *fill.u.gradient.midpoints);
            fill.u.gradient.nmidpoints = fill.u.gradient.midpoints != NULL ? nmid : 0;
        }
        for (k = 0; k < nmid; k++) {
            float m = (float)vh_f(&rng);
            if (fill.u.gradient.midpoints != NULL) {
                fill.u.gradient.midpoints[k] = m;
            }
        }
        fill.u.gradient.angle = angles[vh_below(&rng, 8)];
        fill.u.gradient.scale = scales[vh_below(&rng, 4)];
        fill.u.gradient.style = STYLES[i % 5];
        fill.u.gradient.reverse = vh_below(&rng, 2) == 0;
        fill.u.gradient.offset[0] = ox[vh_below(&rng, 3)];
        fill.u.gradient.offset[1] = oy[vh_below(&rng, 3)];
        fill.u.gradient.dither = vh_below(&rng, 3) == 0;
        fill.u.gradient.align = vh_below(&rng, 3) != 0;
        switch (i % 3) {
        case 0:
            path = vh_rect(0.0, 0.0, 100.0, 10.0);
            break;
        case 1:
            path = vh_ellipse(3.5, 2.25, 40.0, 33.0);
            break;
        default:
            CHECK(pc_vec_shape_polygon(1.0, 1.0, 37.0, 29.0, 5, 0.5, &path, NULL) == PC_OK);
            break;
        }
        sh = shape_layer(path);
        if (i % 4 == 3) {
            static const pc_stroke_align al[3] = {PC_STROKE_ALIGN_INSIDE, PC_STROKE_ALIGN_CENTER,
                                                  PC_STROKE_ALIGN_OUTSIDE};
            sh.has_stroke = true;
            pc_shape_stroke_default(&sh.stroke);
            sh.stroke.width = 3.0f;
            sh.stroke.paint = pc_fill_solid(pc_color_rgba(0.0f, 0.0f, 1.0f, 0.7f));
            sh.stroke.opacity = 0.8f;
            sh.stroke.align = al[i % 3];
        } else if (i % 4 == 1) {
            sh.has_stroke = true;
            pc_shape_stroke_default(&sh.stroke);
            sh.stroke.width = 2.0f;
            CHECK(pc_fill_clone(&fill, &sh.stroke.paint, NULL) == PC_OK);
        }
        sh.has_fill = true;
        sh.fill = fill;
        (void)snprintf(prefix, sizeof prefix, "paint %zu", i);
        shape_line(prefix, &sh, pc_rect_make(0, 0, 120, 50), pc_rect_make(-2, -3, 110, 45));
        pc_shape_layer_fini(&sh);
    }
    {
        pc_path bases[4];
        size_t pi;
        bases[0] = vh_rect(20.0, 20.0, 60.0, 40.0);
        bases[1] = vh_rect(20.0, 20.0, 60.0, 40.0);
        bases[1].subpaths[0].closed = false;
        bases[2] = vh_curve();
        bases[3] = vh_pentagram();
        for (pi = 0; pi < 4; pi++) {
            static const pc_stroke_align al[3] = {PC_STROKE_ALIGN_INSIDE, PC_STROKE_ALIGN_CENTER,
                                                  PC_STROKE_ALIGN_OUTSIDE};
            size_t ai;
            size_t fi;
            for (ai = 0; ai < 3; ai++) {
                for (fi = 0; fi < 2; fi++) {
                    pc_shape_layer sh = shape_layer(vh_clone(&bases[pi]));
                    char prefix[48];
                    sh.has_stroke = true;
                    pc_shape_stroke_default(&sh.stroke);
                    sh.stroke.width = 6.0f;
                    sh.stroke.paint = pc_fill_solid(pc_color_rgba(0.0f, 0.5f, 1.0f, 0.9f));
                    sh.stroke.opacity = 0.75f;
                    sh.stroke.align = al[ai];
                    sh.stroke.cap = CAPS[ai];
                    sh.stroke.join = JOINS[fi + ai % 2];
                    if (pi == 3) {
                        sh.stroke.dashes = malloc(2 * sizeof(float));
                        if (sh.stroke.dashes != NULL) {
                            sh.stroke.dashes[0] = 2.0f;
                            sh.stroke.dashes[1] = 1.0f;
                            sh.stroke.ndashes = 2;
                        }
                    }
                    if (fi == 1) {
                        sh.has_fill = true;
                        sh.fill = pc_fill_solid(pc_color_rgb(1.0f, 0.0f, 0.0f));
                    }
                    (void)snprintf(prefix, sizeof prefix, "strokeshape %zu %zu %zu", pi, ai, fi);
                    shape_line(prefix, &sh, pc_rect_make(0, 0, 100, 100), pc_rect_make(0, 0, 100, 100));
                    pc_shape_layer_fini(&sh);
                }
            }
        }
        for (pi = 0; pi < 4; pi++) {
            pc_path_fini(&bases[pi]);
        }
    }
    {
        pc_shape_layer cases[5];
        pc_path inv = vh_ellipse(5.0, 5.0, 20.0, 20.0);
        inv.inverted = true;
        cases[0] = shape_layer(vh_rect(2.0, 2.0, 10.0, 10.0));
        cases[0].has_stroke = true;
        pc_shape_stroke_default(&cases[0].stroke);
        cases[0].stroke.width = 0.0f;
        cases[1] = shape_layer(vh_rect(2.0, 2.0, 10.0, 10.0));
        cases[1].has_stroke = true;
        pc_shape_stroke_default(&cases[1].stroke);
        cases[1].stroke.opacity = 0.0f;
        cases[2] = shape_layer(vh_rect(2.0, 2.0, 10.0, 10.0));
        cases[2].has_fill = true;
        cases[2].fill.kind = PC_FILL_PATTERN;
        cases[2].fill.u.pattern.scale = 1.0f;
        cases[2].fill.u.pattern.link = true;
        cases[3] = shape_layer(inv);
        cases[3].has_fill = true;
        cases[3].fill = pc_fill_solid(pc_color_rgb(0.0f, 1.0f, 0.0f));
        {
            pc_path empty;
            memset(&empty, 0, sizeof empty);
            cases[4] = shape_layer(empty);
        }
        cases[4].has_fill = true;
        cases[4].fill = pc_fill_solid(PC_COLOR_BLACK);
        cases[4].has_stroke = true;
        pc_shape_stroke_default(&cases[4].stroke);
        for (i = 0; i < 5; i++) {
            char prefix[32];
            (void)snprintf(prefix, sizeof prefix, "shapecase %zu", i);
            shape_line(prefix, &cases[i], pc_rect_make(0, 0, 30, 30), pc_rect_make(0, 0, 30, 30));
            pc_shape_layer_fini(&cases[i]);
        }
    }
}

static void surface_line(const char *prefix, const pc_surface *s, pc_rect r)
{
    uint8_t *b = NULL;
    size_t n = 0;
    uint64_t h = VH_H0;
    pc_rect cb = pc_surface_content_bounds(s);
    CHECK(pc_surface_to_interleaved(s, r, &b, &n, NULL) == PC_OK);
    vh_b(&h, b, n);
    free(b);
    EXPECTF("%s %d,%d,%d,%d %zu %016" PRIx64, prefix, cb.x0, cb.y0, cb.x1, cb.y1, pc_surface_tile_count(s),
            h);
}

static void surface_section(void)
{
    size_t i;
    char prefix[48];
    {
        pc_shape_layer sh;
        pc_path p = vh_ellipse(-30.0, 40.0, 600.0, 300.0);
        pc_path r = vh_rect(100.0, 100.0, 50.0, 400.0);
        pc_color_stop stops[2];
        pc_rect clip = pc_rect_make(0, 0, 640, 520);
        const pc_pixel_format fmts[7] = {
            PC_PIXEL_FORMAT_RGBA8,
            PC_PIXEL_FORMAT_RGBA16,
            {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true},
            PC_PIXEL_FORMAT_GRAY8,
            {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, true},
            {PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true},
            {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, false},
        };
        vh_push(&p, vh_take0(&r, PC_PATH_EXCLUDE));
        sh = shape_layer(p);
        stops[0].pos = 0.0f;
        stops[0].color = pc_color_rgb(1.0f, 0.0f, 0.0f);
        stops[1].pos = 1.0f;
        stops[1].color = pc_color_rgba(0.0f, 0.0f, 1.0f, 0.5f);
        sh.has_fill = true;
        CHECK(pc_fill_gradient(stops, 2, 30.0f, 1.0f, PC_GRADIENT_LINEAR, false, &sh.fill, NULL) == PC_OK);
        sh.has_stroke = true;
        pc_shape_stroke_default(&sh.stroke);
        sh.stroke.width = 5.0f;
        sh.stroke.paint = pc_fill_solid(pc_color_rgb(0.1f, 0.9f, 0.2f));
        sh.stroke.dashes = malloc(2 * sizeof(float));
        if (sh.stroke.dashes != NULL) {
            sh.stroke.dashes[0] = 3.0f;
            sh.stroke.dashes[1] = 2.0f;
            sh.stroke.ndashes = 2;
        }
        sh.stroke.join = PC_LINE_JOIN_ROUND;
        for (i = 0; i < 7; i++) {
            pc_surface s;
            CHECK(pc_vec_render_shape(&sh, fmts[i], clip, &s, NULL) == PC_OK);
            (void)snprintf(prefix, sizeof prefix, "surface %zu", i);
            surface_line(prefix, &s, clip);
            pc_surface_fini(&s);
        }
        pc_shape_layer_fini(&sh);
    }
    {
        pc_shape_layer sh = shape_layer(vh_rect(2.0, 2.0, 10.0, 10.0));
        const pc_rect clips[4] = {pc_rect_make(0, 0, 20, 20), pc_rect_make(0, 0, 5, 5),
                                  pc_rect_make(-300, -300, -100, -100), pc_rect_make(5, 5, 5, 9)};
        sh.has_fill = true;
        sh.fill = pc_fill_solid(pc_color_rgb(1.0f, 0.0f, 0.0f));
        sh.has_stroke = true;
        pc_shape_stroke_default(&sh.stroke);
        sh.stroke.width = 2.0f;
        sh.stroke.paint = pc_fill_solid(pc_color_rgb(0.0f, 0.0f, 1.0f));
        for (i = 0; i < 4; i++) {
            pc_surface s;
            CHECK(pc_vec_render_shape(&sh, PC_PIXEL_FORMAT_RGBA8, clips[i], &s, NULL) == PC_OK);
            (void)snprintf(prefix, sizeof prefix, "smallsurface %zu", i);
            surface_line(prefix, &s, pc_rect_make(-1, -1, 21, 21));
            pc_surface_fini(&s);
        }
        pc_shape_layer_fini(&sh);
    }
    {
        pc_path p = vh_rect(10.0, 10.0, 20.0, 20.0);
        pc_vec_rasterizer r;
        pc_path inv = vh_ellipse(100.0, 50.0, 300.0, 200.0);
        pc_surface s;
        CHECK(pc_vec_fill_rasterizer(&p, PC_VEC_DEFAULT_TOLERANCE, &r, NULL) == PC_OK);
        for (i = 0; i < 3; i++) {
            pc_pixel_format f = {PC_COLOR_MODE_RGB, (pc_sample_type)i, true};
            CHECK(pc_vec_coverage_surface(&r, f, pc_rect_make(0, 0, 300, 300), &s, NULL) == PC_OK);
            (void)snprintf(prefix, sizeof prefix, "coverage %zu", i);
            surface_line(prefix, &s, pc_rect_make(0, 0, 300, 300));
            pc_surface_fini(&s);
        }
        pc_vec_rasterizer_fini(&r);
        inv.inverted = true;
        CHECK(pc_vec_fill_rasterizer(&inv, PC_VEC_DEFAULT_TOLERANCE, &r, NULL) == PC_OK);
        CHECK(pc_vec_coverage_surface(&r, PC_PIXEL_FORMAT_GRAY8, pc_rect_make(-20, -10, 530, 290), &s,
                                      NULL) == PC_OK);
        surface_line("coverage inv", &s, pc_rect_make(-20, -10, 530, 290));
        pc_surface_fini(&s);
        pc_vec_rasterizer_fini(&r);
        pc_path_fini(&p);
        pc_path_fini(&inv);
    }
    {
        const struct {
            float density;
            bool enabled, inverted, empty;
        } cases[7] = {{1.0f, true, false, false}, {0.25f, true, false, false}, {0.25f, false, false, false},
                      {1.0f, true, true, true},   {0.5f, true, false, true},   {NAN, true, true, false},
                      {-0.5f, true, false, false}};
        for (i = 0; i < 7; i++) {
            pc_path path;
            pc_vector_mask vm;
            float *v = NULL;
            size_t n = 0;
            if (cases[i].empty) {
                memset(&path, 0, sizeof path);
            } else {
                path = vh_ellipse(3.0, 4.0, 30.0, 20.0);
            }
            path.inverted = cases[i].inverted;
            pc_vector_mask_init(&vm, &path);
            vm.density = cases[i].density;
            vm.enabled = cases[i].enabled;
            CHECK(pc_vec_mask_values(&vm, pc_rect_make(0, 0, 40, 30), &v, &n, NULL) == PC_OK);
            EXPECTF("mask %zu %016" PRIx64, i, cov_hash(v, n));
            free(v);
            pc_vector_mask_fini(&vm);
        }
    }
}

static void test_vector_matches_the_reference(void)
{
    size_t i;
    load_golden();
    build_corpus();
    build_styles();
    paths_section();
    flatten_section();
    fill_section();
    stroke_section();
    shapes_section();
    edit_section();
    trace_section();
    paint_section();
    surface_section();
    if (gi != ngold) {
        PC_T_FAIL("%zu golden lines left unchecked", ngold - gi);
    }
    for (i = 0; i < NSTYLES; i++) {
        pc_vec_stroke_style_fini(&styles[i]);
    }
    free_corpus();
    for (i = 0; i < ngold; i++) {
        free(gold[i]);
    }
    free(gold);
}

int main(int argc, char **argv)
{
    RUN(test_vector_matches_the_reference);
    return pc_t_finish();
}
