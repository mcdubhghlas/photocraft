/* Vanishing Point (port of crates/algo/src/vanishing.rs). See vanishing.h. */
#include "algo/vanishing.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/fxutil.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "raster/pixel.h"

#define PI_D 3.14159265358979323846
#define TAU_D 6.28318530717958647692

typedef double v3[3];

static void sub3(const double *a, const double *b, double *o)
{
    o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2];
}
static void add3(const double *a, const double *b, double *o)
{
    o[0] = a[0] + b[0], o[1] = a[1] + b[1], o[2] = a[2] + b[2];
}
static void mul3(const double *a, double k, double *o)
{
    o[0] = a[0] * k, o[1] = a[1] * k, o[2] = a[2] * k;
}
static double dot3(const double *a, const double *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
static void cross3(const double *a, const double *b, double *o)
{
    double t[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(o, t, sizeof t);
}
static void norm3(const double *a, double *o)
{
    mul3(a, 1.0 / pc_rs_max_k(sqrt(dot3(a, a)), 1e-300), o);
}

/* Intersection of lines (a, b) and (c, d) in homogeneous coordinates. */
static void meet(const double *a, const double *b, const double *c, const double *d, double *o)
{
    double pa[3] = {a[0], a[1], 1.0}, pb[3] = {b[0], b[1], 1.0}, pc[3] = {c[0], c[1], 1.0},
           pd[3] = {d[0], d[1], 1.0}, l1[3], l2[3];
    cross3(pa, pb, l1);
    cross3(pc, pd, l2);
    cross3(l1, l2, o);
}

bool pc_algo_vp_plane_homography(const pc_algo_vp_plane *p, pc_algo_homography *out)
{
    static const double r[4] = {0.0, 0.0, 1.0, 1.0};
    return pc_algo_homography_rect_to_quad(r, (const double (*)[2])p->corners, out);
}

bool pc_algo_vp_plane_focal(const pc_algo_vp_plane *p, const double c[2], double *out)
{
    const double (*q)[2] = (const double (*)[2])p->corners;
    double v1[3], v2[3], a[2], b[2], f2;
    meet(q[0], q[1], q[3], q[2], v1);
    meet(q[0], q[3], q[1], q[2], v2);
    if (fabs(v1[2]) < 1e-9 || fabs(v2[2]) < 1e-9) {
        return false;
    }
    a[0] = v1[0] / v1[2] - c[0];
    a[1] = v1[1] / v1[2] - c[1];
    b[0] = v2[0] / v2[2] - c[0];
    b[1] = v2[1] / v2[2] - c[1];
    f2 = -(a[0] * b[0] + a[1] * b[1]);
    if (!(f2 > 1.0)) {
        return false;
    }
    *out = sqrt(f2);
    return true;
}

/* Turn of each corner (sin of the angle between consecutive edges, positive when the corners run
 * clockwise on screen); false when an edge is shorter than a pixel or a corner is not finite. */
static bool plane_turns(const pc_algo_vp_plane *p, double out[4])
{
    const double (*q)[2] = (const double (*)[2])p->corners;
    size_t i;
    for (i = 0; i < 4; i++) {
        if (!isfinite(q[i][0]) || !isfinite(q[i][1]) || fabs(q[i][0]) > 1e9 || fabs(q[i][1]) > 1e9) {
            return false;
        }
    }
    for (i = 0; i < 4; i++) {
        size_t i1 = (i + 1) % 4, i2 = (i + 2) % 4;
        double a[2] = {q[i1][0] - q[i][0], q[i1][1] - q[i][1]},
               b[2] = {q[i2][0] - q[i1][0], q[i2][1] - q[i1][1]};
        double la = hypot(a[0], a[1]), lb = hypot(b[0], b[1]);
        if (la < 1.0 || lb < 1.0) {
            return false;
        }
        out[i] = (a[0] * b[1] - a[1] * b[0]) / (la * lb);
    }
    return true;
}

static bool vp_finite(const double v[3])
{
    return fabs(v[2]) > 1e-9 * pc_rs_max_k(fabs(v[0]) + fabs(v[1]), 1.0);
}

pc_algo_vp_plane_check pc_algo_vp_plane_check_at(const pc_algo_vp_plane *p, const double center[2])
{
    const double (*q)[2] = (const double (*)[2])p->corners;
    const double deg = 3.141592653589793 / 180.0;
    double t[4], mn = 1.7976931348623157e308, v1[3], v2[3], f;
    bool pos = true, neg = true;
    pc_algo_homography h;
    size_t i;
    if (!plane_turns(p, t)) {
        return PC_ALGO_VP_INVALID;
    }
    for (i = 0; i < 4; i++) {
        pos = pos && t[i] > 0.0;
        neg = neg && t[i] < 0.0;
        mn = pc_rs_min(mn, fabs(t[i]));
    }
    if (!(pos || neg) || mn < sin(1.0 * deg) || !pc_algo_vp_plane_homography(p, &h)) {
        return PC_ALGO_VP_INVALID;
    }
    if (mn < sin(10.0 * deg)) {
        return PC_ALGO_VP_QUESTIONABLE;
    }
    /* Both vanishing points finite but no real focal length fits them. */
    meet(q[0], q[1], q[3], q[2], v1);
    meet(q[0], q[3], q[1], q[2], v2);
    if (vp_finite(v1) && vp_finite(v2) && !pc_algo_vp_plane_focal(p, center, &f)) {
        return PC_ALGO_VP_QUESTIONABLE;
    }
    return PC_ALGO_VP_VALID;
}

const char *pc_algo_vp_plane_check_name(pc_algo_vp_plane_check c)
{
    return c == PC_ALGO_VP_VALID ? "valid" : (c == PC_ALGO_VP_QUESTIONABLE ? "questionable" : "invalid");
}

pc_algo_vp_plane pc_algo_vp_plane_normalized(const pc_algo_vp_plane *p)
{
    pc_algo_vp_plane out, q = *p;
    double t[4];
    size_t first = 0, i;
    if (plane_turns(p, t) && t[0] + t[1] + t[2] + t[3] < 0.0) {
        q.corners[1][0] = p->corners[3][0], q.corners[1][1] = p->corners[3][1];
        q.corners[3][0] = p->corners[1][0], q.corners[3][1] = p->corners[1][1];
    }
    /* min_by keeps the first of equal minima (total order). */
    for (i = 1; i < 4; i++) {
        double a = q.corners[i][0] + q.corners[i][1], b = q.corners[first][0] + q.corners[first][1];
        int64_t ia, ib;
        memcpy(&ia, &a, 8);
        memcpy(&ib, &b, 8);
        ia ^= (int64_t)((uint64_t)(ia >> 63) >> 1);
        ib ^= (int64_t)((uint64_t)(ib >> 63) >> 1);
        if (ia < ib) {
            first = i;
        }
    }
    for (i = 0; i < 4; i++) {
        out.corners[i][0] = q.corners[(first + i) % 4][0];
        out.corners[i][1] = q.corners[(first + i) % 4][1];
    }
    return out;
}

static void ray(const double p[2], double f, const double c[2], double o[3])
{
    o[0] = (p[0] - c[0]) / f;
    o[1] = (p[1] - c[1]) / f;
    o[2] = 1.0;
}

/* Vanishing direction of lines (a, b) and (cc, d), or the edge direction when they are parallel
 * in the image. */
static void vdir(const double *a, const double *b, const double *cc, const double *d, double f,
                 const double c[2], double o[3])
{
    double v[3];
    meet(a, b, cc, d, v);
    if (fabs(v[2]) < 1e-9 * pc_rs_max_k(fabs(v[0]) + fabs(v[1]), 1.0)) {
        double t[3] = {v[0], v[1], 0.0};
        norm3(t, o);
    } else {
        double p[2] = {v[0] / v[2], v[1] / v[2]}, r[3];
        ray(p, f, c, r);
        norm3(r, o);
    }
}

void pc_algo_vp_plane_lift(const pc_algo_vp_plane *p, double f, const double c[2], pc_algo_vp_lifted *out)
{
    const double (*q)[2] = (const double (*)[2])p->corners;
    double d1[3], d2[3], n[3], t[3];
    int k;
    vdir(q[0], q[1], q[3], q[2], f, c, d1);
    vdir(q[0], q[3], q[1], q[2], f, c, d2);
    cross3(d1, d2, t);
    norm3(t, n);
    if (n[2] < 0.0) {
        mul3(n, -1.0, n);
    }
    for (k = 0; k < 4; k++) {
        double r[3], lam;
        ray(q[k], f, c, r);
        lam = 1.0 / pc_rs_max_k(fabs(dot3(n, r)), 1e-9);
        mul3(r, lam, out->pts[k]);
    }
    out->focal = f;
    out->center[0] = c[0];
    out->center[1] = c[1];
}

bool pc_algo_vp_lifted_project(const pc_algo_vp_lifted *l, const double x[3], double out[2])
{
    if (!(x[2] > 1e-9)) {
        return false;
    }
    out[0] = l->center[0] + l->focal * x[0] / x[2];
    out[1] = l->center[1] + l->focal * x[1] / x[2];
    return true;
}

static double dist3(const double *a, const double *b)
{
    double d[3];
    sub3(a, b, d);
    return sqrt(dot3(d, d));
}

void pc_algo_vp_lifted_lengths(const pc_algo_vp_lifted *l, double *lu, double *lv)
{
    *lu = (dist3(l->pts[0], l->pts[1]) + dist3(l->pts[3], l->pts[2])) / 2.0;
    *lv = (dist3(l->pts[0], l->pts[3]) + dist3(l->pts[1], l->pts[2])) / 2.0;
}

static pc_status push_plane(pc_algo_vp_scene *s, const pc_algo_vp_plane *p, pc_error *err)
{
    if (s->n == s->cap) {
        size_t cap = s->cap;
        pc_algo_vp_plane *np = pc_grow_array(s->planes, &cap, s->n + 1, sizeof *np);
        if (np == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        s->planes = np;
        s->cap = cap;
    }
    s->planes[s->n++] = *p;
    return PC_OK;
}

pc_status pc_algo_vp_scene_init(pc_algo_vp_scene *s, const pc_algo_vp_plane *first, const double center[2],
                                bool has_focal, double focal, double fallback, pc_error *err)
{
    pc_algo_vp_scene t;
    double f;
    pc_status st;
    memset(&t, 0, sizeof t);
    if (has_focal && focal > 1.0) {
        f = focal;
    } else if (!pc_algo_vp_plane_focal(first, center, &f)) {
        f = fallback;
    }
    t.focal = f;
    t.center[0] = center[0];
    t.center[1] = center[1];
    if ((st = push_plane(&t, first, err)) != PC_OK) {
        return st;
    }
    *s = t;
    return PC_OK;
}

void pc_algo_vp_scene_fini(pc_algo_vp_scene *s)
{
    if (s != NULL) {
        free(s->planes);
        s->planes = NULL;
        s->n = s->cap = 0;
    }
}

void pc_algo_vp_scene_lifted(const pc_algo_vp_scene *s, size_t i, pc_algo_vp_lifted *out)
{
    pc_algo_vp_plane_lift(&s->planes[i], s->focal, s->center, out);
}

pc_status pc_algo_vp_scene_tear_off(pc_algo_vp_scene *s, size_t from, pc_algo_vp_edge edge, double angle,
                                    double depth, size_t *index, pc_error *err)
{
    pc_algo_vp_lifted lf;
    pc_algo_vp_plane np;
    const double *a, *b;
    double axis[3], inward[3], out[3], t[3], len, th, best_score = 0.0, best[4][2];
    bool have = false;
    int ia, ib, ic, k;
    pc_status st;
    if (from >= s->n) {
        return pc_fail(err, PC_ERR_INVALID, "plane %zu does not exist", from);
    }
    pc_algo_vp_scene_lifted(s, from, &lf);
    /* Edge end points (a -> b, clockwise) and the parent's inward direction. */
    switch (edge) {
    case PC_ALGO_VP_TOP:
        ia = 0, ib = 1, ic = 3;
        break;
    case PC_ALGO_VP_RIGHT:
        ia = 1, ib = 2, ic = 0;
        break;
    case PC_ALGO_VP_BOTTOM:
        ia = 2, ib = 3, ic = 1;
        break;
    default:
        ia = 3, ib = 0, ic = 2;
        break;
    }
    a = lf.pts[ia];
    b = lf.pts[ib];
    sub3(b, a, t);
    norm3(t, axis);
    sub3(lf.pts[ic], lf.pts[ia], inward);
    len = sqrt(dot3(inward, inward)) * pc_rs_max_k(depth, 0.05);
    /* The new plane continues outward (-inward), turned about the edge by 180 - angle; of the two
     * turning directions pick the one that faces the camera with the parent in front of it. */
    norm3(inward, t);
    mul3(t, -1.0, out);
    th = (180.0 - angle) * (PI_D / 180.0);
    for (k = 0; k < 2; k++) {
        double sgn = k == 0 ? 1.0 : -1.0, sn = sin(sgn * th), cs = cos(sgn * th);
        double rot[3], u1[3], u2[3], u3[3], a2[3], b2[3], pa[2], pb[2], pa2[2], pb2[2], n2[3], e1[3], e2[3];
        double score;
        mul3(out, cs, u1);
        cross3(axis, out, t);
        mul3(t, sn, u2);
        mul3(axis, dot3(axis, out) * (1.0 - cs), u3);
        add3(u1, u2, rot);
        add3(rot, u3, rot);
        mul3(rot, len, t);
        add3(a, t, a2);
        add3(b, t, b2);
        if (!pc_algo_vp_lifted_project(&lf, a, pa) || !pc_algo_vp_lifted_project(&lf, b, pb) ||
            !pc_algo_vp_lifted_project(&lf, a2, pa2) || !pc_algo_vp_lifted_project(&lf, b2, pb2)) {
            continue;
        }
        sub3(b, a, e1);
        sub3(a2, a, e2);
        cross3(e1, e2, t);
        norm3(t, n2);
        if (dot3(n2, a) > 0.0) {
            mul3(n2, -1.0, n2);
        }
        score = dot3(n2, inward);
        if (!have || score > best_score) {
            have = true;
            best_score = score;
            memcpy(best[0], pa, sizeof pa);
            memcpy(best[1], pb, sizeof pb);
            memcpy(best[2], pa2, sizeof pa2);
            memcpy(best[3], pb2, sizeof pb2);
        }
    }
    if (!have) {
        return pc_fail(err, PC_ERR_INVALID, "the torn-off plane would be behind the camera");
    }
    /* Keep corners clockwise from the new plane's top-left (shared edge first or last). */
    {
        static const int order[4][4] = {{2, 3, 1, 0}, {0, 2, 3, 1}, {1, 0, 2, 3}, {3, 1, 0, 2}};
        int e =
            edge == PC_ALGO_VP_TOP ? 0 : (edge == PC_ALGO_VP_RIGHT ? 1 : (edge == PC_ALGO_VP_BOTTOM ? 2 : 3));
        for (k = 0; k < 4; k++) {
            memcpy(np.corners[k], best[order[e][k]], sizeof np.corners[k]);
        }
    }
    if ((st = push_plane(s, &np, err)) != PC_OK) {
        return st;
    }
    *index = s->n - 1;
    return PC_OK;
}

bool pc_algo_vp_scene_frame(const pc_algo_vp_scene *s, size_t i, pc_algo_homography *h, double *lu,
                            double *lv)
{
    pc_algo_vp_lifted l;
    if (i >= s->n || !pc_algo_vp_plane_homography(&s->planes[i], h)) {
        return false;
    }
    pc_algo_vp_scene_lifted(s, i, &l);
    pc_algo_vp_lifted_lengths(&l, lu, lv);
    return true;
}

bool pc_algo_vp_scene_plane_at(const pc_algo_vp_scene *s, const double p[2], size_t *out)
{
    size_t i;
    for (i = s->n; i-- > 0;) {
        pc_algo_homography h, inv;
        double u, v;
        if (pc_algo_vp_plane_homography(&s->planes[i], &h) && pc_algo_homography_inverse(&h, &inv)) {
            pc_algo_homography_apply(&inv, p[0], p[1], &u, &v);
            if (u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0) {
                *out = i;
                return true;
            }
        }
    }
    return false;
}

pc_status pc_algo_vp_paste(const pc_algo_vp_scene *s, size_t i, const pc_surface *src, pc_rect src_rect,
                           const double at[2], double width, pc_surface *out, pc_error *err)
{
    pc_algo_homography h, to_plane, full, inv;
    double lu, lv, sw, sh, su, sv;
    pc_surface res;
    pc_rect b;
    float *data = NULL;
    size_t len = 0, n, k;
    bool have_inv;
    pc_status st;
    if (!pc_algo_vp_scene_frame(s, i, &h, &lu, &lv)) {
        return pc_fail(err, PC_ERR_INVALID, "plane %zu is degenerate", i);
    }
    sw = (double)pc_rect_width(src_rect);
    sh = (double)pc_rect_height(src_rect);
    if (sw <= 0.0 || sh <= 0.0) {
        return pc_fail(err, PC_ERR_INVALID, "nothing to paste");
    }
    /* Source px -> plane (u, v): u spans `width`, v keeps the metric aspect. */
    su = width / sw;
    sv = width * lu / lv / sw;
    to_plane = (pc_algo_homography){{su, 0.0, at[0] - (double)src_rect.x0 * su, 0.0, sv,
                                     at[1] - (double)src_rect.y0 * sv, 0.0, 0.0, 1.0}};
    full = pc_algo_homography_mul(&h, &to_plane);
    if ((st = pc_algo_warp_surface(src, src_rect, &full, PC_ALGO_INTERP_BICUBIC, &res, err)) != PC_OK) {
        return st;
    }
    /* Clip to the plane. */
    b = pc_surface_content_bounds(&res);
    if (pc_rect_is_empty(b)) {
        *out = res;
        return PC_OK;
    }
    if ((st = pc_surface_read_region(&res, b, &data, &len, err)) != PC_OK) {
        pc_surface_fini(&res);
        return st;
    }
    n = pc_surface_channels(&res);
    have_inv = pc_algo_homography_inverse(&h, &inv);
    for (k = 0; k * n + n <= len; k++) {
        int32_t x, y;
        double u = 0.0, v = 0.0;
        float m = 0.0f;
        pc_algo_xy(b, k, &x, &y);
        if (have_inv) {
            pc_algo_homography_apply(&inv, (double)x + 0.5, (double)y + 0.5, &u, &v);
            m = (u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0) ? 1.0f : 0.0f;
        }
        data[k * n + n - 1] *= m;
    }
    st = pc_surface_write_region(&res, b, data, len, err);
    free(data);
    if (st != PC_OK) {
        pc_surface_fini(&res);
        return st;
    }
    pc_surface_prune(&res);
    *out = res;
    return PC_OK;
}

/* Bilinear sample of all n channels at (x, y) (pixel corners at integers). */
static pc_status sample(const pc_surface *s, double x, double y, size_t n, float *out, pc_error *err)
{
    int32_t x0 = pc_f64_to_i32(floor(x)), y0 = pc_f64_to_i32(floor(y));
    float tx = pc_f64_to_f32(x - (double)x0), ty = pc_f64_to_f32(y - (double)y0);
    float r[4 * PC_RASTER_MAX_CHANNELS];
    size_t c;
    pc_status st = pc_surface_read_region_into(
        s, pc_rect_make(x0, y0, pc_i32_sat_add(x0, 2), pc_i32_sat_add(y0, 2)), r, 4 * n, err);
    if (st != PC_OK) {
        return st;
    }
    for (c = 0; c < n; c++) {
        out[c] = (r[c] * (1.0f - tx) + r[n + c] * tx) * (1.0f - ty) +
                 (r[2 * n + c] * (1.0f - tx) + r[3 * n + c] * tx) * ty;
    }
    return PC_OK;
}

static void metric(const pc_algo_homography *inv, double lu, double lv, double px, double py, double out[2])
{
    double u, v;
    pc_algo_homography_apply(inv, px, py, &u, &v);
    out[0] = u * lu;
    out[1] = v * lv;
}

pc_status pc_algo_vp_clone_stroke(const pc_algo_vp_scene *s, pc_surface *surf, const double source[2],
                                  const double (*points)[2], size_t n_points, double size, double hardness,
                                  float opacity, size_t *dabs_out, pc_error *err)
{
    size_t ps, pd, n, q, dabs = 0;
    pc_algo_homography hs, hd, hs_inv, hd_inv;
    double lus, lvs, lud, lvd, ms[2], md0[2], offset[2], probe[2], r_m;
    pc_surface copy;
    pc_status st = PC_OK;
    *dabs_out = 0;
    if (n_points == 0 || !pc_algo_vp_scene_plane_at(s, source, &ps) ||
        !pc_algo_vp_scene_plane_at(s, points[0], &pd) || !pc_algo_vp_scene_frame(s, ps, &hs, &lus, &lvs) ||
        !pc_algo_vp_scene_frame(s, pd, &hd, &lud, &lvd) || !pc_algo_homography_inverse(&hs, &hs_inv) ||
        !pc_algo_homography_inverse(&hd, &hd_inv)) {
        return PC_OK;
    }
    metric(&hs_inv, lus, lvs, source[0], source[1], ms);
    metric(&hd_inv, lud, lvd, points[0][0], points[0][1], md0);
    offset[0] = ms[0] - md0[0];
    offset[1] = ms[1] - md0[1];
    /* Brush radius in metric units (from the image size at the first dab). */
    metric(&hd_inv, lud, lvd, points[0][0] + size / 2.0, points[0][1], probe);
    r_m = pc_rs_max_k(hypot(probe[0] - md0[0], probe[1] - md0[1]), 1e-9);
    if ((st = pc_surface_clone(surf, &copy, err)) != PC_OK) {
        return st;
    }
    n = pc_surface_channels(surf);
    for (q = 0; q < n_points && st == PC_OK; q++) {
        double mc[2], bb[4] = {1.7976931348623157e308, 1.7976931348623157e308, -1.7976931348623157e308,
                               -1.7976931348623157e308};
        pc_rect area;
        float *data = NULL;
        size_t len = 0, i, c;
        bool changed = false;
        int k;
        metric(&hd_inv, lud, lvd, points[q][0], points[q][1], mc);
        /* Image-space footprint: map a metric circle's bounding points. */
        for (k = 0; k < 16; k++) {
            double a = (double)k / 16.0 * TAU_D, x, y;
            pc_algo_homography_apply(&hd, (mc[0] + r_m * cos(a)) / lud, (mc[1] + r_m * sin(a)) / lvd, &x, &y);
            bb[0] = pc_rs_min(bb[0], x);
            bb[1] = pc_rs_min(bb[1], y);
            bb[2] = pc_rs_max(bb[2], x);
            bb[3] = pc_rs_max(bb[3], y);
        }
        area = pc_rect_make(pc_f64_to_i32(floor(bb[0])), pc_f64_to_i32(floor(bb[1])),
                            pc_i32_sat_add(pc_f64_to_i32(ceil(bb[2])), 1),
                            pc_i32_sat_add(pc_f64_to_i32(ceil(bb[3])), 1));
        if (pc_rect_is_empty(area) || pc_rect_width(area) > 4096 || pc_rect_height(area) > 4096) {
            continue;
        }
        if ((st = pc_surface_read_region(surf, area, &data, &len, err)) != PC_OK) {
            break;
        }
        for (i = 0; i * n + n <= len; i++) {
            int32_t x, y;
            double m[2], d, hard, fall, sx, sy;
            float kk, sv[PC_RASTER_MAX_CHANNELS];
            pc_algo_xy(area, i, &x, &y);
            metric(&hd_inv, lud, lvd, (double)x + 0.5, (double)y + 0.5, m);
            d = hypot(m[0] - mc[0], m[1] - mc[1]) / r_m;
            if (d >= 1.0) {
                continue;
            }
            hard = pc_clampd(hardness, 0.0, 0.99);
            fall = d <= hard ? 1.0 : 1.0 - (d - hard) / (1.0 - hard);
            kk = pc_f64_to_f32(fall * fall * (3.0 - 2.0 * fall)) * opacity;
            /* Source pixel at the same metric offset. */
            pc_algo_homography_apply(&hs, (m[0] + offset[0]) / lus, (m[1] + offset[1]) / lvs, &sx, &sy);
            if ((st = sample(&copy, sx - 0.5, sy - 0.5, n, sv, err)) != PC_OK) {
                break;
            }
            for (c = 0; c < n; c++) {
                data[i * n + c] += (sv[c] - data[i * n + c]) * kk;
            }
            changed = true;
        }
        if (st == PC_OK && changed) {
            st = pc_surface_write_region(surf, area, data, len, err);
            dabs += st == PC_OK;
        }
        free(data);
    }
    pc_surface_fini(&copy);
    if (st != PC_OK) {
        return st;
    }
    *dabs_out = dabs;
    return PC_OK;
}
