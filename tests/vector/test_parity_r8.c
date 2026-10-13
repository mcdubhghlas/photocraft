/* Bit-exact parity with crates/vector for the functionality added in the R8 resync: the lines of
 * tests/golden/vector-vectors.txt from the first "validate" line on (vector_vectors.rs
 * r8_vectors): shape validation, dash seams, miter limits, sequential mask rendering, Add /
 * Delete Anchor, Curvature Pen knots, fit_trace and non-affine maps (Distort, Perspective,
 * Warp). The earlier lines are test_parity.c's. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/rmath.h"
#include "harness.h"
#include "vparity.h"

static pc_vec_knot_ref kr(size_t s, size_t k)
{
    pc_vec_knot_ref r;
    r.sub = s;
    r.knot = k;
    return r;
}

/* ---- R8 (upstream resync): validate_shape, dash seams, miter limits, render_sequential,
 * Add / Delete Anchor, Curvature Pen knots, fit_trace, non-affine maps ---- */

#define NR8STROKES 11

static void r8_strokes(pc_shape_stroke *v)
{
    static float d21[2] = {2.0f, 1.0f};
    static float dtiny[2] = {0.01f, 0.01f};
    static float dzero[2] = {0.01f, 0.0f};
    static float d33[33];
    static float dsmall[2] = {0.001f, 0.002f};
    static float dwide[3] = {3.0f, 0.5f, 1.0f};
    size_t i;
    for (i = 0; i < 33; i++) {
        d33[i] = 1.0f;
    }
    for (i = 0; i < NR8STROKES; i++) {
        pc_shape_stroke_default(&v[i]);
    }
    v[1].dashes = d21;
    v[1].ndashes = 2;
    v[1].dash_offset = 0.5f;
    v[2].width = 1.0f;
    v[2].dashes = dtiny;
    v[2].ndashes = 2;
    v[3].width = 0.0f;
    v[3].dashes = dzero;
    v[3].ndashes = 2;
    v[4].miter_limit = 0.5f;
    v[5].width = NAN;
    v[6].dashes = d33;
    v[6].ndashes = 33;
    v[7].dashes = dsmall;
    v[7].ndashes = 2;
    v[8].opacity = 1.5f;
    v[9].dash_offset = 2e6f;
    v[10].width = 100.0f;
    v[10].dashes = dwide;
    v[10].ndashes = 3;
    v[10].dash_offset = -7.0f;
}

/* "ok" or "ERR msg"; with underscores for spaces when `under`. */
static void r8_res(pc_status st, const pc_error *e, bool under, char *buf, size_t n)
{
    size_t i;
    if (st == PC_OK) {
        (void)snprintf(buf, n, "ok");
        return;
    }
    (void)snprintf(buf, n, "ERR %s", e->msg);
    for (i = 0; under && buf[i] != '\0'; i++) {
        if (buf[i] == ' ') {
            buf[i] = '_';
        }
    }
}

static void r8_validate_line(const char *tag, size_t i, const pc_path *p, const pc_shape_stroke *strokes)
{
    char line[LINE];
    char r[300];
    pc_error e;
    size_t s;
    size_t len;
    r8_res(pc_vec_validate_shape(p, NULL, &e), &e, false, r, sizeof r);
    len = (size_t)snprintf(line, sizeof line, "%s %zu %s", tag, i, r);
    for (s = 0; s < NR8STROKES && len < sizeof line; s++) {
        r8_res(pc_vec_validate_shape(p, &strokes[s], &e), &e, true, r, sizeof r);
        len += (size_t)snprintf(line + len, sizeof line - len, " %s", r);
    }
    expect(line);
}

static void r8_validate(void)
{
    pc_shape_stroke strokes[NR8STROKES];
    pc_path big[6];
    size_t i;
    r8_strokes(strokes);
    for (i = 0; i < NCORPUS; i++) {
        r8_validate_line("validate", i, &corpus[i], strokes);
    }
    memset(big, 0, sizeof big);
    {
        double (*pts)[2] = malloc(4097 * sizeof *pts);
        if (pts != NULL) {
            for (i = 0; i < 4097; i++) {
                pts[i][0] = (double)i;
                pts[i][1] = 0.0;
            }
            big[0] = vh_poly_path((const double (*)[2])pts, 4097, false);
            free(pts);
        }
        for (i = 0; i < 4097; i++) {
            double pt[1][2];
            pt[0][0] = (double)i;
            pt[0][1] = 0.0;
            vh_push(&big[1], vh_polygon((const double (*)[2])pt, 1, false));
        }
    }
    {
        static const double far[3][2] = {{0.0, 0.0}, {1000001.0, 0.0}, {0.0, 5.0}};
        static const double edge[3][2] = {{0.0, 0.0}, {1000000.0, 0.0}, {0.0, -1000000.0}};
        static const double lng[2][2] = {{0.0, 0.0}, {900000.0, 0.0}};
        big[2] = vh_poly_path(far, 3, true);
        big[3] = vh_poly_path(edge, 3, true);
        for (i = 0; i < 40; i++) {
            double c[2][2];
            pc_subpath sp;
            c[0][0] = 0.0;
            c[0][1] = (double)i;
            c[1][0] = 900000.0;
            c[1][1] = (double)i;
            sp = vh_polygon((const double (*)[2])c, 2, false);
            if (sp.nknots == 2) {
                sp.knots[0].out_ctrl = pc_point_make(0.0, 900000.0);
                sp.knots[1].in_ctrl = pc_point_make(900000.0, -900000.0);
            }
            vh_push(&big[4], sp);
        }
        big[5] = vh_poly_path(lng, 2, false);
    }
    for (i = 0; i < 6; i++) {
        r8_validate_line("validatebig", i, &big[i], strokes);
        pc_path_fini(&big[i]);
    }
}

static void r8_dash_line(const char *prefix, const pc_vec_polyline *d, size_t n)
{
    uint64_t h = VH_H0;
    size_t np = hash_lines(&h, d, n);
    EXPECTF("%s %zu %zu %016" PRIx64, prefix, n, np, h);
}

static void r8_dashes_and_miters(void)
{
    double sq[4][2] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}};
    bool ksq[4] = {true, true, true, true};
    double tr[3][2] = {{3.0, 1.0}, {20.0, 4.0}, {9.0, 17.0}};
    bool ktr[3] = {true, false, true};
    double seg[2][2] = {{0.0, 0.0}, {5.0, 0.0}};
    bool kseg[2] = {true, true};
    pc_vec_polyline pls[3] = {{sq, ksq, 4, 4, true}, {tr, ktr, 3, 3, true}, {seg, kseg, 2, 2, true}};
    static const double p0[2] = {8.0, 4.0};
    static const double p1[2] = {80.0, 2.0};
    static const double p2[3] = {3.0, 1.0, 2.0};
    static const double p3[4] = {1.0, 1.0, 0.0, 2.0};
    static const double p4[2] = {40.0, 0.0};
    static const double p5[2] = {0.5, 0.25};
    static const double *const pats[6] = {p0, p1, p2, p3, p4, p5};
    static const size_t npats[6] = {2, 2, 3, 4, 2, 2};
    static const double offs[4] = {0.0, 2.0, -3.5, 7.25};
    const size_t mpaths[3] = {3, 4, 24};
    double limits[7];
    size_t li;
    size_t ki;
    size_t oi;
    for (li = 0; li < 3; li++) {
        for (ki = 0; ki < 6; ki++) {
            for (oi = 0; oi < 4; oi++) {
                pc_vec_polyline *d = NULL;
                size_t n = 0;
                char prefix[64];
                CHECK(pc_vec_dash_polyline(&pls[li], pats[ki], npats[ki], offs[oi], &d, &n, NULL) == PC_OK);
                (void)snprintf(prefix, sizeof prefix, "dashseam %zu %zu %zu", li, ki, oi);
                r8_dash_line(prefix, d, n);
                pc_vec_polylines_free(d, n);
            }
        }
    }
    limits[0] = 0.5;
    limits[1] = NAN;
    limits[2] = INFINITY;
    limits[3] = 1.0;
    limits[4] = 400.0;
    limits[5] = 500.0;
    limits[6] = 5000.0;
    for (li = 0; li < 3; li++) {
        pc_vec_polyline *lines = NULL;
        size_t nl = 0;
        CHECK(pc_vec_flatten_path(&corpus[mpaths[li]], 0.05, &lines, &nl, NULL) == PC_OK);
        for (ki = 0; ki < 7; ki++) {
            pc_vec_stroke_style st;
            pc_vec_polys polys = {NULL, 0, 0};
            uint64_t h = VH_H0;
            size_t np;
            pc_vec_stroke_style_default(&st);
            st.width = 6.0;
            st.miter_limit = limits[ki];
            CHECK(pc_vec_stroke_polygons(lines, nl, &st, 0.01, &polys, NULL) == PC_OK);
            np = hash_polys(&h, &polys);
            EXPECTF("miter %zu %zu %zu %016" PRIx64, li, ki, np, h);
            pc_vec_polys_fini(&polys);
        }
        pc_vec_polylines_free(lines, nl);
    }
    {
        double a[3][2] = {{0.0, 0.0}, {NAN, 3.0}, {10.0, 10.0}};
        double b[3][2] = {{0.0, 0.0}, {30.0, 0.0}, {30.0, 20.0}};
        bool ka[3] = {true, true, true};
        bool kb[3] = {true, true, true};
        pc_vec_polyline lines[2] = {{a, ka, 3, 3, false}, {b, kb, 3, 3, false}};
        static double d1[2] = {0.0, 0.0};
        static double d2[2] = {4.0, 2.0};
        static double d3[2] = {-1.0, 3.0};
        double *const ds[4] = {NULL, d1, d2, d3};
        size_t di;
        for (di = 0; di < 4; di++) {
            pc_vec_stroke_style st;
            pc_vec_polys polys = {NULL, 0, 0};
            uint64_t h = VH_H0;
            size_t np;
            pc_vec_stroke_style_default(&st);
            st.width = 3.0;
            st.cap = PC_LINE_CAP_ROUND;
            st.join = PC_LINE_JOIN_ROUND;
            st.dashes = ds[di];
            st.ndashes = ds[di] != NULL ? 2 : 0;
            st.dash_offset = 1.0;
            CHECK(pc_vec_stroke_polygons(lines, 2, &st, 0.01, &polys, NULL) == PC_OK);
            np = hash_polys(&h, &polys);
            EXPECTF("strokeskip %zu %zu %016" PRIx64, di, np, h);
            pc_vec_polys_fini(&polys);
        }
    }
}

static void r8_masks(void)
{
    const pc_rect rects[4] = {{-3, -7, 140, 90}, {0, 0, 1, 1}, {5, 5, 5, 40}, {-50, 20, 300, 300}};
    const size_t pidx[6] = {0, 1, 12, 13, 16, 24};
    const float dens[3] = {1.0f, 0.25f, 0.0f};
    size_t pi;
    size_t di;
    int en;
    for (pi = 0; pi < 6; pi++) {
        for (di = 0; di < 3; di++) {
            for (en = 1; en >= 0; en--) {
                pc_path path = vh_clone(&corpus[pidx[pi]]);
                pc_vector_mask vm;
                pc_vec_compiled_mask c;
                uint64_t h = VH_H0;
                size_t ri;
                pc_vector_mask_init(&vm, &path);
                vm.density = dens[di];
                vm.enabled = en != 0;
                CHECK(pc_vec_compiled_mask_new(&vm, &c, NULL) == PC_OK);
                for (ri = 0; ri < 4; ri++) {
                    float *v = NULL;
                    size_t n = 0;
                    size_t k;
                    CHECK(pc_vec_compiled_mask_render_sequential(&c, rects[ri], &v, &n, NULL) == PC_OK);
                    vh_u64(&h, n);
                    for (k = 0; k < n; k++) {
                        vh_f32(&h, v[k]);
                    }
                    free(v);
                }
                EXPECTF("seqmask %zu %zu %d %016" PRIx64, pi, di, en, h);
                pc_vec_compiled_mask_fini(&c);
                pc_vector_mask_fini(&vm);
            }
        }
    }
}

static void r8_anchor_str(pc_status st, const pc_error *e, pc_vec_knot_ref r, const pc_path *p, char *buf,
                          size_t n)
{
    if (st == PC_OK) {
        (void)snprintf(buf, n, "%zu.%zu %016" PRIx64, r.sub, r.knot, path_hash(p));
    } else {
        (void)snprintf(buf, n, "ERR %s", e->msg);
    }
}

static void r8_anchors(void)
{
    double ts[8];
    pc_path bases[7];
    size_t pi;
    ts[0] = 0.5;
    ts[1] = 0.13;
    ts[2] = 0.9;
    ts[3] = 1e-9;
    ts[4] = 0.0;
    ts[5] = 1.0;
    ts[6] = NAN;
    ts[7] = -0.5;
    bases[0] = vh_curve();
    bases[1] = vh_tri();
    bases[2] = vh_clone(&corpus[2]);
    bases[3] = vh_clone(&corpus[1]);
    bases[4] = vh_clone(&corpus[25]);
    bases[5] = vh_clone(&corpus[30]);
    bases[6] = vh_clone(&corpus[3]);
    for (pi = 0; pi < 7; pi++) {
        const pc_path *base = &bases[pi];
        size_t nk = base->nsubpaths > 0 ? base->subpaths[0].nknots : 0;
        size_t ks[5];
        const size_t ss[3] = {0, 1, 7};
        size_t kk;
        size_t ti;
        size_t si;
        char buf[300];
        pc_error e;
        pc_vec_knot_ref r = {0, 0};
        pc_path p;
        uint64_t h = VH_H0;
        size_t n;
        bool removed = false;
        ks[0] = 0;
        ks[1] = 1;
        ks[2] = nk > 0 ? nk - 1 : 0;
        ks[3] = nk;
        ks[4] = SIZE_MAX;
        for (kk = 0; kk < 5; kk++) {
            for (ti = 0; ti < 8; ti++) {
                pc_status st;
                p = vh_clone(base);
                st = pc_vec_add_anchor(&p, kr(0, ks[kk]), ts[ti], &r, &e);
                r8_anchor_str(st, &e, r, &p, buf, sizeof buf);
                EXPECTF("addanchor %zu %zu %zu %s", pi, ks[kk], ti, buf);
                pc_path_fini(&p);
            }
        }
        p = vh_clone(base);
        r8_anchor_str(pc_vec_add_anchor(&p, kr(7, 0), 0.5, &r, &e), &e, r, &p, buf, sizeof buf);
        EXPECTF("addanchor %zu badsub %s", pi, buf);
        pc_path_fini(&p);
        p = vh_clone(base);
        for (n = 0; n < 6; n++) {
            pc_status st = pc_vec_add_anchor(&p, kr(0, 0), 0.3 + 0.1 * (double)n, &r, &e);
            r8_anchor_str(st, &e, r, &p, buf, sizeof buf);
            vh_b(&h, buf, strlen(buf));
        }
        EXPECTF("addmany %zu %016" PRIx64, pi, h);
        pc_path_fini(&p);
        for (si = 0; si < 3; si++) {
            for (kk = 0; kk < 4; kk++) {
                pc_status st;
                p = vh_clone(base);
                st = pc_vec_delete_anchor(&p, kr(ss[si], ks[kk]), &removed, &e);
                if (st == PC_OK) {
                    EXPECTF("delanchor %zu %zu %zu %d %016" PRIx64, pi, ss[si], ks[kk], removed ? 1 : 0,
                            path_hash(&p));
                } else {
                    EXPECTF("delanchor %zu %zu %zu ERR %s", pi, ss[si], ks[kk], e.msg);
                }
                pc_path_fini(&p);
            }
        }
        p = vh_clone(base);
        n = 0;
        while (pc_vec_delete_anchor(&p, kr(0, 0), &removed, NULL) == PC_OK && n < 1000) {
            n++;
        }
        EXPECTF("delall %zu %zu %016" PRIx64, pi, n, path_hash(&p));
        pc_path_fini(&p);
    }
    for (pi = 0; pi < 7; pi++) {
        pc_path_fini(&bases[pi]);
    }
    {
        static const double one[1][2] = {{5.0, 5.0}};
        pc_path p = vh_poly_path(one, 1, false);
        bool removed = false;
        pc_error e;
        pc_status st = pc_vec_delete_anchor(&p, kr(0, 0), &removed, &e);
        if (st == PC_OK) {
            EXPECTF("delanchor one %d", removed ? 1 : 0);
        } else {
            EXPECTF("delanchor one ERR %s", e.msg);
        }
        pc_path_fini(&p);
    }
}

static void r8_curv_hash(uint64_t *h, const double (*k)[3][2], size_t n)
{
    size_t i;
    size_t j;
    for (i = 0; i < n; i++) {
        for (j = 0; j < 3; j++) {
            vh_f64(h, k[i][j][0]);
            vh_f64(h, k[i][j][1]);
        }
    }
}

static void r8_curvature(void)
{
    static const size_t ns[8] = {0, 1, 2, 3, 4, 5, 7, 12};
    vh_rng rng = {0xc047e5u};
    size_t i;
    for (i = 0; i < 24; i++) {
        size_t n = ns[i % 8];
        double anchors[12][2];
        bool corners[14];
        size_t nc = n + i % 3 > 0 ? n + i % 3 - 1 : 0;
        double out[12][3][2];
        size_t k;
        int closed;
        for (k = 0; k < n; k++) {
            anchors[k][0] = vh_f(&rng) * 200.0 - 50.0;
            anchors[k][1] = vh_f(&rng) * 150.0;
        }
        if (i % 5 == 3 && n >= 3) {
            anchors[1][0] = anchors[0][0];
            anchors[1][1] = anchors[0][1];
        }
        if (i % 7 == 2 && n >= 3) {
            anchors[2][0] = anchors[0][0];
            anchors[2][1] = anchors[0][1];
        }
        for (k = 0; k < nc; k++) {
            corners[k] = vh_below(&rng, 4) == 0;
        }
        for (closed = 0; closed < 2; closed++) {
            uint64_t h = VH_H0;
            pc_vec_curvature_knots((const double (*)[2])anchors, n, corners, nc, closed != 0, out);
            vh_u64(&h, n);
            r8_curv_hash(&h, (const double (*)[3][2])out, n);
            EXPECTF("curv %zu %d %016" PRIx64, i, closed, h);
        }
    }
    {
        double a[4][2] = {{0.0, 0.0}, {NAN, 1.0}, {INFINITY, 2.0}, {3.0, 4.0}};
        double out[4][3][2];
        uint64_t h = VH_H0;
        size_t i2;
        size_t j;
        pc_vec_curvature_knots((const double (*)[2])a, 4, NULL, 0, false, out);
        /* NaN sign and payload depend on operand order: every NaN hashes as one value. */
        for (i2 = 0; i2 < 4; i2++) {
            for (j = 0; j < 3; j++) {
                vh_f64(&h, isnan(out[i2][j][0]) ? (double)NAN : out[i2][j][0]);
                vh_f64(&h, isnan(out[i2][j][1]) ? (double)NAN : out[i2][j][1]);
            }
        }
        EXPECTF("curv hostile %016" PRIx64, h);
    }
}

#define NR8TRACES 11

/* The traces of vector_vectors.rs r8_traces(); *n receives each length. */
static double (*r8_trace(size_t which, size_t *n))[2]
{
    static const double turns[6][2] = {{0.0, 0.0},    {60.0, 0.0},    {60.0, 60.0},
                                       {120.0, 60.0}, {120.0, 120.0}, {180.0, 120.0}};
    static const size_t lens[NR8TRACES] = {101, 151, 180, 201, 600, 900, 50, 8, 2, 20000, 5};
    double (*v)[2] = malloc(lens[which] * sizeof *v);
    size_t i;
    *n = 0;
    if (v == NULL) {
        return NULL;
    }
    switch (which) {
    case 0:
        for (i = 0; i <= 100; i++) {
            v[i][0] = 10.0 + 3.0 * (double)i;
            v[i][1] = 50.0 + (i % 2 == 0 ? 0.5 : -0.5);
        }
        break;
    case 1: {
        size_t m = 0;
        for (i = 0; i < 5; i++) {
            int k;
            for (k = 0; k < 30; k++) {
                v[m][0] = turns[i][0] + (turns[i + 1][0] - turns[i][0]) * (double)k / 30.0;
                v[m][1] = turns[i][1] + (turns[i + 1][1] - turns[i][1]) * (double)k / 30.0;
                m++;
            }
        }
        v[m][0] = turns[5][0];
        v[m][1] = turns[5][1];
        break;
    }
    case 2:
        for (i = 0; i < 180; i++) {
            double a = (double)i * (3.141592653589793 / 180.0) * 2.0;
            v[i][0] = 100.0 + 80.0 * cos(a);
            v[i][1] = 100.0 + 80.0 * sin(a);
        }
        break;
    case 3:
        for (i = 0; i <= 200; i++) {
            double x = (double)i * 2.0;
            v[i][0] = x;
            v[i][1] = 100.0 + 40.0 * sin(x / 200.0 * 6.283185307179586) + 0.5 - (double)(i % 2);
        }
        break;
    case 4: {
        vh_rng rng = {0xf1eeu};
        double x = 50.0;
        double y = 50.0;
        double a = 0.0;
        for (i = 0; i < 600; i++) {
            a += (vh_f(&rng) - 0.5) * 0.6;
            x += 1.5 * cos(a);
            y += 1.5 * sin(a);
            v[i][0] = x + (vh_f(&rng) - 0.5);
            v[i][1] = y + (vh_f(&rng) - 0.5);
        }
        break;
    }
    case 5:
        for (i = 0; i < 900; i++) {
            double t = (double)i * 0.05;
            v[i][0] = 200.0 + t * 4.0 * cos(t);
            v[i][1] = 200.0 + t * 4.0 * sin(t);
        }
        break;
    case 6:
        for (i = 0; i < 50; i++) {
            v[i][0] = 1.0;
            v[i][1] = 1.0;
        }
        break;
    case 7: {
        const double w[8][2] = {{0.0, 0.0},   {NAN, 1.0},  {5.0, 5.0},  {INFINITY, 2.0},
                                {1e300, 3.0}, {10.0, 0.0}, {10.0, 0.1}, {20.0, 7.0}};
        memcpy(v, w, sizeof w);
        break;
    }
    case 8:
        v[0][0] = 0.0;
        v[0][1] = 0.0;
        v[1][0] = 30.0;
        v[1][1] = 40.0;
        break;
    case 9:
        for (i = 0; i < 20000; i++) {
            v[i][0] = (double)i * 0.01;
            v[i][1] = sin((double)i * 0.001) * 40.0;
        }
        break;
    default: {
        static const double w[5][2] = {{0.0, 0.0}, {10.0, 0.0}, {10.0, 10.0}, {0.0, 10.0}, {0.05, 0.05}};
        memcpy(v, w, sizeof w);
        break;
    }
    }
    *n = lens[which];
    return v;
}

static void r8_fit(void)
{
    double tols[6];
    size_t i;
    tols[0] = 2.0;
    tols[1] = 0.5;
    tols[2] = 10.0;
    tols[3] = NAN;
    tols[4] = -5.0;
    tols[5] = 1000.0;
    for (i = 0; i < NR8TRACES; i++) {
        size_t n = 0;
        double (*pts)[2] = r8_trace(i, &n);
        size_t ti;
        int closed;
        CHECK(pts != NULL);
        for (ti = 0; ti < 6 && pts != NULL; ti++) {
            for (closed = 0; closed < 2; closed++) {
                pc_subpath sp;
                bool ok = false;
                CHECK(pc_vec_fit_trace((const double (*)[2])pts, n, tols[ti], closed != 0, &sp, &ok, NULL) ==
                      PC_OK);
                if (ok) {
                    size_t nk = sp.nknots;
                    pc_path p = vh_path(&sp, 1);
                    EXPECTF("fit %zu %zu %d %zu %016" PRIx64, i, ti, closed, nk, path_hash(&p));
                    pc_path_fini(&p);
                } else {
                    EXPECTF("fit %zu %zu %d none", i, ti, closed);
                }
            }
        }
        free(pts);
    }
}

static pc_point r8_persp(pc_point q, void *ctx)
{
    double w = 1.0 + 0.002 * q.x - 0.001 * q.y;
    (void)ctx;
    return pc_point_make((1.1 * q.x + 0.2 * q.y + 3.0) / w, (-0.1 * q.x + 0.9 * q.y - 2.0) / w);
}

static pc_point r8_bend(pc_point q, void *ctx)
{
    double d = q.x - 50.0;
    (void)ctx;
    return pc_point_make(q.x, q.y - d * d * 0.01);
}

static pc_point r8_wave(pc_point q, void *ctx)
{
    (void)ctx;
    return pc_point_make(q.x + 6.0 * sin(q.y * 0.08), q.y + 4.0 * cos(q.x * 0.05));
}

static pc_point r8_swirl(pc_point q, void *ctx)
{
    double dx = q.x - 50.0;
    double dy = q.y - 50.0;
    double a = pc_rs_min_k(hypot(dx, dy) * 0.02, 3.0);
    double s = sin(a);
    double c = cos(a);
    (void)ctx;
    return pc_point_make(50.0 + dx * c - dy * s, 50.0 + dx * s + dy * c);
}

static pc_point r8_nanmap(pc_point q, void *ctx)
{
    (void)ctx;
    return pc_point_make(q.x > 30.0 ? NAN : q.x, q.y * INFINITY);
}

static size_t r8_knots(const pc_path *p)
{
    size_t n = 0;
    size_t i;
    for (i = 0; i < p->nsubpaths; i++) {
        n += p->subpaths[i].nknots;
    }
    return n;
}

static void r8_maps(void)
{
    const pc_vec_point_map maps[4] = {r8_persp, r8_bend, r8_wave, r8_swirl};
    double tols[5];
    size_t pi;
    tols[0] = 0.1;
    tols[1] = 0.01;
    tols[2] = 1.0;
    tols[3] = NAN;
    tols[4] = 0.0;
    for (pi = 0; pi < NCORPUS; pi++) {
        uint64_t h = VH_H0;
        size_t mi;
        size_t ti;
        for (mi = 0; mi < 4; mi++) {
            pc_path m;
            CHECK(pc_vec_map_points(&corpus[pi], maps[mi], NULL, &m, NULL) == PC_OK);
            hash_path(&h, &m);
            pc_path_fini(&m);
        }
        EXPECTF("mappts %zu %016" PRIx64, pi, h);
        for (mi = 0; mi < 4; mi++) {
            for (ti = 0; ti < 5; ti++) {
                pc_path m;
                if ((pi + mi + ti) % 3 != 0 && ti != 0) {
                    continue;
                }
                CHECK(pc_vec_map_path(&corpus[pi], maps[mi], NULL, tols[ti], &m, NULL) == PC_OK);
                EXPECTF("mappath %zu %zu %zu %zu %016" PRIx64, pi, mi, ti, r8_knots(&m), path_hash(&m));
                pc_path_fini(&m);
            }
        }
    }
    {
        pc_path m;
        size_t n = (size_t)1 << 15;
        double (*pts)[2] = malloc(n * sizeof *pts);
        pc_path big;
        size_t i;
        CHECK(pc_vec_map_path(&corpus[1], r8_nanmap, NULL, 0.1, &m, NULL) == PC_OK);
        EXPECTF("mapnan %zu %016" PRIx64, r8_knots(&m), path_hash(&m));
        pc_path_fini(&m);
        if (pts == NULL) {
            CHECK(pts != NULL);
            return;
        }
        for (i = 0; i < n; i++) {
            double a = (double)i * 6.283185307179586 / 32768.0;
            pts[i][0] = 1000.0 * cos(a);
            pts[i][1] = 1000.0 * sin(a);
        }
        big = vh_poly_path((const double (*)[2])pts, n, true);
        free(pts);
        CHECK(pc_vec_map_path(&big, r8_wave, NULL, 1e-9, &m, NULL) == PC_OK);
        EXPECTF("mapbig %zu %016" PRIx64, r8_knots(&m), path_hash(&m));
        pc_path_fini(&m);
        pc_path_fini(&big);
    }
}

static void r8_section(void)
{
    r8_validate();
    r8_dashes_and_miters();
    r8_masks();
    r8_anchors();
    r8_curvature();
    r8_fit();
    r8_maps();
}

static void test_r8_vectors_match_the_reference(void)
{
    load_golden();
    build_corpus();
    gi = r8_first_line();
    CHECK(gi < ngold);
    r8_section();
    if (gi != ngold) {
        PC_T_FAIL("%zu golden lines left unchecked", ngold - gi);
    }
    free_corpus();
    free_golden();
}

int main(int argc, char **argv)
{
    RUN(test_r8_vectors_match_the_reference);
    return pc_t_finish();
}
