/* tiny-skia-path 0.11.4 path_geometry.rs (roots, chops, evaluation, curvature, cusps, unit arcs)
 * and tiny-skia 0.11.4's own path_geometry.rs (monotonic chops) with its path64 module (the f64
 * cubic / line intersection behind chop_mono_cubic_at_x / _y), for the SVG rasteriser. f32 and
 * f64 exactly where the reference has them; root finders loop a bounded number of times. */
#include <float.h>
#include <string.h>

#include "io/svg_ts.h"

#define FLOAT_PI 3.14159265f
#define EPS_F32 FLT_EPSILON

static sv_pt interp_pt(sv_pt a, sv_pt b, float t)
{
    return ts_pt(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

/* NormalizedF32::new_clamped */
static float norm_clamped(float v)
{
    if (!isfinite(v)) {
        return 0.0f;
    }
    return pc_rs_maxf_k(pc_rs_minf(1.0f, v), 0.0f);
}

/* NormalizedF32Exclusive::new_bounded */
static float excl_bounded(float v)
{
    return pc_rs_maxf_k(pc_rs_minf(1.0f - EPS_F32, v), EPS_F32);
}

bool ts_valid_unit_divide(float numer, float denom, float *t)
{
    float r;
    if (numer < 0.0f) {
        numer = -numer;
        denom = -denom;
    }
    if (denom == 0.0f || numer == 0.0f || numer >= denom) {
        return false;
    }
    r = numer / denom;
    if (!(r > 0.0f && r < 1.0f)) {
        return false;
    }
    *t = r;
    return true;
}

size_t ts_find_unit_quad_roots(float a, float b, float c, float roots[3])
{
    double dr;
    float r;
    float q;
    size_t n = 0;
    float v;
    if (a == 0.0f) {
        if (ts_valid_unit_divide(-c, b, &v)) {
            roots[0] = v;
            return 1;
        }
        return 0;
    }
    dr = (double)b * (double)b - 4.0 * (double)a * (double)c;
    if (dr < 0.0) {
        return 0;
    }
    dr = sqrt(dr);
    r = pc_f64_to_f32(dr);
    if (!isfinite(r)) {
        return 0;
    }
    q = b < 0.0f ? -(b - r) / 2.0f : -(b + r) / 2.0f;
    if (ts_valid_unit_divide(q, a, &v)) {
        roots[n++] = v;
    }
    if (ts_valid_unit_divide(c, q, &v)) {
        roots[n++] = v;
    }
    if (n == 2) {
        if (roots[0] > roots[1]) {
            float tmp = roots[0];
            roots[0] = roots[1];
            roots[1] = tmp;
        } else if (roots[0] == roots[1]) {
            n = 1;
        }
    }
    return n;
}

void ts_chop_quad_at(const sv_pt src[3], float t, sv_pt dst[5])
{
    sv_pt p01 = interp_pt(src[0], src[1], t);
    sv_pt p12 = interp_pt(src[1], src[2], t);
    dst[0] = src[0];
    dst[1] = p01;
    dst[2] = interp_pt(p01, p12, t);
    dst[3] = p12;
    dst[4] = src[2];
}

void ts_chop_cubic_at2(const sv_pt src[4], float t, sv_pt dst[7])
{
    sv_pt ab = interp_pt(src[0], src[1], t);
    sv_pt bc = interp_pt(src[1], src[2], t);
    sv_pt cd = interp_pt(src[2], src[3], t);
    sv_pt abc = interp_pt(ab, bc, t);
    sv_pt bcd = interp_pt(bc, cd, t);
    sv_pt abcd = interp_pt(abc, bcd, t);
    dst[0] = src[0];
    dst[1] = ab;
    dst[2] = abc;
    dst[3] = abcd;
    dst[4] = bcd;
    dst[5] = cd;
    dst[6] = src[3];
}

/* dst holds 3 * nt + 4 points. */
void ts_chop_cubic_at(const sv_pt src[4], const float *t, size_t nt, sv_pt *dst)
{
    sv_pt tmp[4];
    const sv_pt *s = src;
    size_t off = 0;
    size_t i;
    float tv;
    if (nt == 0) {
        memcpy(dst, src, 4 * sizeof *dst);
        return;
    }
    tv = t[0];
    for (i = 0; i < nt; i++) {
        float n;
        ts_chop_cubic_at2(s, tv, dst + off);
        if (i == nt - 1) {
            break;
        }
        off += 3;
        memcpy(tmp, dst + off, 4 * sizeof *tmp);
        s = tmp;
        if (ts_valid_unit_divide(t[i + 1] - t[i], 1.0f - t[i], &n)) {
            tv = n;
        } else {
            dst[off + 4] = s[3];
            dst[off + 5] = s[3];
            dst[off + 6] = s[3];
            break;
        }
    }
}

static bool is_not_monotonic(float a, float b, float c)
{
    float ab = a - b;
    float bc = b - c;
    if (ab < 0.0f) {
        bc = -bc;
    }
    return ab == 0.0f || bc < 0.0f;
}

static size_t chop_quad_extrema(const sv_pt src[3], sv_pt dst[5], bool y)
{
    float a = y ? src[0].y : src[0].x;
    float b = y ? src[1].y : src[1].x;
    float c = y ? src[2].y : src[2].x;
    float t;
    if (is_not_monotonic(a, b, c)) {
        if (ts_valid_unit_divide(a - b, a - b - b + c, &t)) {
            ts_chop_quad_at(src, t, dst);
            if (y) {
                dst[1].y = dst[2].y;
                dst[3].y = dst[2].y;
            } else {
                dst[1].x = dst[2].x;
                dst[3].x = dst[2].x;
            }
            return 1;
        }
        b = fabsf(a - b) < fabsf(b - c) ? a : c;
    }
    if (y) {
        dst[0] = ts_pt(src[0].x, a);
        dst[1] = ts_pt(src[1].x, b);
        dst[2] = ts_pt(src[2].x, c);
    } else {
        dst[0] = ts_pt(a, src[0].y);
        dst[1] = ts_pt(b, src[1].y);
        dst[2] = ts_pt(c, src[2].y);
    }
    return 0;
}

size_t ts_chop_quad_at_x_extrema(const sv_pt src[3], sv_pt dst[5])
{
    return chop_quad_extrema(src, dst, false);
}

size_t ts_chop_quad_at_y_extrema(const sv_pt src[3], sv_pt dst[5])
{
    return chop_quad_extrema(src, dst, true);
}

size_t ts_find_cubic_extrema(float a, float b, float c, float d, float t[3])
{
    float na = d - a + 3.0f * (b - c);
    float nb = 2.0f * (a - b - b + c);
    float nc = b - a;
    return ts_find_unit_quad_roots(na, nb, nc, t);
}

static size_t chop_cubic_extrema(const sv_pt src[4], sv_pt dst[10], bool y)
{
    float t[3];
    size_t n = y ? ts_find_cubic_extrema(src[0].y, src[1].y, src[2].y, src[3].y, t)
                 : ts_find_cubic_extrema(src[0].x, src[1].x, src[2].x, src[3].x, t);
    ts_chop_cubic_at(src, t, n, dst);
    if (n > 0) {
        if (y) {
            dst[2].y = dst[3].y;
            dst[4].y = dst[3].y;
            if (n == 2) {
                dst[5].y = dst[6].y;
                dst[7].y = dst[6].y;
            }
        } else {
            dst[2].x = dst[3].x;
            dst[4].x = dst[3].x;
            if (n == 2) {
                dst[5].x = dst[6].x;
                dst[7].x = dst[6].x;
            }
        }
    }
    return n;
}

size_t ts_chop_cubic_at_x_extrema(const sv_pt src[4], sv_pt dst[10])
{
    return chop_cubic_extrema(src, dst, false);
}

size_t ts_chop_cubic_at_y_extrema(const sv_pt src[4], sv_pt dst[10])
{
    return chop_cubic_extrema(src, dst, true);
}

float ts_find_quad_max_curvature(const sv_pt src[3])
{
    float ax = src[1].x - src[0].x;
    float ay = src[1].y - src[0].y;
    float bx = src[0].x - src[1].x - src[1].x + src[2].x;
    float by = src[0].y - src[1].y - src[1].y + src[2].y;
    float numer = -(ax * bx + ay * by);
    float denom = bx * bx + by * by;
    float t;
    if (denom < 0.0f) {
        numer = -numer;
        denom = -denom;
    }
    if (numer <= 0.0f) {
        return 0.0f;
    }
    if (numer >= denom) {
        return 1.0f;
    }
    t = numer / denom;
    /* NormalizedF32::new(t).unwrap(): a NaN would panic in the reference; treat it as 0 */
    return t >= 0.0f && t <= 1.0f ? t : 0.0f;
}

sv_pt ts_eval_quad_at(const sv_pt src[3], float t)
{
    sv_pt c = src[0];
    sv_pt d1 = ts_sub(src[1], c);
    sv_pt b = ts_add(d1, d1);
    sv_pt a = ts_add(ts_sub(src[2], ts_add(src[1], src[1])), c);
    return ts_pt((a.x * t + b.x) * t + c.x, (a.y * t + b.y) * t + c.y);
}

sv_pt ts_eval_quad_tangent_at(const sv_pt src[3], float t)
{
    sv_pt b;
    sv_pt a;
    sv_pt v;
    if ((t == 0.0f && ts_pt_eq(src[0], src[1])) || (t == 1.0f && ts_pt_eq(src[1], src[2]))) {
        return ts_sub(src[2], src[0]);
    }
    b = ts_sub(src[1], src[0]);
    a = ts_sub(ts_sub(src[2], src[1]), b);
    v = ts_pt(a.x * t + b.x, a.y * t + b.y);
    return ts_add(v, v);
}

static void f1_dot_f2(const float s[4], float out[4])
{
    float a = s[1] - s[0];
    float b = s[2] - 2.0f * s[1] + s[0];
    float c = s[3] + 3.0f * (s[1] - s[2]) - s[0];
    out[0] = c * c;
    out[1] = 3.0f * b * c;
    out[2] = 2.0f * b * b + c * a;
    out[3] = a * b;
}

static void sort3(float v[3])
{
    float t;
    if (v[0] > v[1]) {
        t = v[0];
        v[0] = v[1];
        v[1] = t;
    }
    if (v[1] > v[2]) {
        t = v[1];
        v[1] = v[2];
        v[2] = t;
    }
    if (v[0] > v[1]) {
        t = v[0];
        v[0] = v[1];
        v[1] = t;
    }
}

static size_t solve_cubic_poly(const float coeff[4], float t[3])
{
    float inva;
    float a;
    float b;
    float c;
    float q;
    float r;
    float q3;
    float r2mq3;
    float adiv3;
    if (ts_nearly_zero(coeff[0])) {
        float tmp[3];
        size_t n = ts_find_unit_quad_roots(coeff[1], coeff[2], coeff[3], tmp);
        size_t i;
        for (i = 0; i < n; i++) {
            t[i] = tmp[i];
        }
        return n;
    }
    inva = 1.0f / coeff[0];
    a = coeff[1] * inva;
    b = coeff[2] * inva;
    c = coeff[3] * inva;
    q = (a * a - b * 3.0f) / 9.0f;
    r = (2.0f * a * a * a - 9.0f * a * b + 27.0f * c) / 54.0f;
    q3 = q * q * q;
    r2mq3 = r * r - q3;
    adiv3 = a / 3.0f;
    if (r2mq3 < 0.0f) {
        float theta = acosf(pc_rs_maxf_k(pc_rs_minf(1.0f, r / sqrtf(q3)), -1.0f));
        float n2rq = -2.0f * sqrtf(q);
        size_t len = 3;
        t[0] = norm_clamped(n2rq * cosf(theta / 3.0f) - adiv3);
        t[1] = norm_clamped(n2rq * cosf((theta + 2.0f * FLOAT_PI) / 3.0f) - adiv3);
        t[2] = norm_clamped(n2rq * cosf((theta - 2.0f * FLOAT_PI) / 3.0f) - adiv3);
        sort3(t);
        if (t[1] == t[2]) {
            len = 2;
        }
        if (t[0] == t[1]) {
            len = 1;
        }
        return len;
    } else {
        float aa = fabsf(r) + sqrtf(r2mq3);
        aa = powf(aa, 0.3333333f);
        if (r > 0.0f) {
            aa = -aa;
        }
        if (aa != 0.0f) {
            aa += q / aa;
        }
        t[0] = norm_clamped(aa - adiv3);
        return 1;
    }
}

size_t ts_find_cubic_max_curvature(const sv_pt src[4], float t[3])
{
    float xs[4] = {src[0].x, src[1].x, src[2].x, src[3].x};
    float ys[4] = {src[0].y, src[1].y, src[2].y, src[3].y};
    float cx[4];
    float cy[4];
    int i;
    f1_dot_f2(xs, cx);
    f1_dot_f2(ys, cy);
    for (i = 0; i < 4; i++) {
        cx[i] += cy[i];
    }
    return solve_cubic_poly(cx, t);
}

static void cubic_coeff(const sv_pt s[4], sv_pt c[4])
{
    sv_pt p0 = s[0];
    sv_pt p1 = s[1];
    sv_pt p2 = s[2];
    sv_pt p3 = s[3];
    sv_pt d12 = ts_sub(p1, p2);
    sv_pt t2 = ts_add(ts_sub(p2, ts_add(p1, p1)), p0);
    sv_pt d10 = ts_sub(p1, p0);
    c[0] = ts_sub(ts_add(p3, ts_pt(3.0f * d12.x, 3.0f * d12.y)), p0);
    c[1] = ts_pt(3.0f * t2.x, 3.0f * t2.y);
    c[2] = ts_pt(3.0f * d10.x, 3.0f * d10.y);
    c[3] = p0;
}

sv_pt ts_eval_cubic_pos_at(const sv_pt src[4], float t)
{
    sv_pt c[4];
    cubic_coeff(src, c);
    return ts_pt(((c[0].x * t + c[1].x) * t + c[2].x) * t + c[3].x,
                 ((c[0].y * t + c[1].y) * t + c[2].y) * t + c[3].y);
}

sv_pt ts_eval_cubic_derivative(const sv_pt src[4], float t)
{
    sv_pt p0 = src[0];
    sv_pt p1 = src[1];
    sv_pt p2 = src[2];
    sv_pt p3 = src[3];
    sv_pt d12 = ts_sub(p1, p2);
    sv_pt a = ts_sub(ts_add(p3, ts_pt(3.0f * d12.x, 3.0f * d12.y)), p0);
    sv_pt bb = ts_add(ts_sub(p2, ts_add(p1, p1)), p0);
    sv_pt b = ts_add(bb, bb);
    sv_pt c = ts_sub(p1, p0);
    return ts_pt((a.x * t + b.x) * t + c.x, (a.y * t + b.y) * t + c.y);
}

sv_pt ts_eval_cubic_tangent_at(const sv_pt src[4], float t)
{
    if ((t == 0.0f && ts_pt_eq(src[0], src[1])) || (t == 1.0f && ts_pt_eq(src[2], src[3]))) {
        sv_pt tan = t == 0.0f ? ts_sub(src[2], src[0]) : ts_sub(src[3], src[1]);
        if (tan.x == 0.0f && tan.y == 0.0f) {
            tan = ts_sub(src[3], src[0]);
        }
        return tan;
    }
    return ts_eval_cubic_derivative(src, t);
}

size_t ts_find_cubic_inflections(const sv_pt src[4], float t[3])
{
    float ax = src[1].x - src[0].x;
    float ay = src[1].y - src[0].y;
    float bx = src[2].x - 2.0f * src[1].x + src[0].x;
    float by = src[2].y - 2.0f * src[1].y + src[0].y;
    float cx = src[3].x + 3.0f * (src[1].x - src[2].x) - src[0].x;
    float cy = src[3].y + 3.0f * (src[1].y - src[2].y) - src[0].y;
    return ts_find_unit_quad_roots(bx * cy - by * cx, ax * cy - ay * cx, ax * by - ay * bx, t);
}

static bool on_same_side(const sv_pt src[4], int ti, int li)
{
    sv_pt origin = src[li];
    sv_pt line = ts_sub(src[li + 1], origin);
    float c0 = ts_cross(line, ts_sub(src[ti], origin));
    float c1 = ts_cross(line, ts_sub(src[ti + 1], origin));
    return c0 * c1 >= 0.0f;
}

bool ts_find_cubic_cusp(const sv_pt src[4], float *t)
{
    float tv[3];
    size_t n;
    size_t i;
    if (ts_pt_eq(src[0], src[1]) || ts_pt_eq(src[2], src[3])) {
        return false;
    }
    if (on_same_side(src, 0, 2) || on_same_side(src, 2, 0)) {
        return false;
    }
    n = ts_find_cubic_max_curvature(src, tv);
    for (i = 0; i < n; i++) {
        sv_pt d;
        float prec;
        if (0.0f >= tv[i] || tv[i] >= 1.0f) {
            continue;
        }
        d = ts_eval_cubic_derivative(src, tv[i]);
        prec =
            (ts_dist_sqd(src[1], src[0]) + ts_dist_sqd(src[2], src[1]) + ts_dist_sqd(src[3], src[2])) * 1e-8f;
        if (ts_dot(d, d) < prec) {
            *t = excl_bounded(tv[i]);
            return true;
        }
    }
    return false;
}

size_t ts_chop_cubic_at_max_curvature(const sv_pt src[4], sv_pt dst[13])
{
    float roots[3];
    float tv[3];
    size_t n = ts_find_cubic_max_curvature(src, roots);
    size_t cnt = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (0.0f < roots[i] && roots[i] < 1.0f) {
            tv[cnt++] = excl_bounded(roots[i]);
        }
    }
    if (cnt == 0) {
        memcpy(dst, src, 4 * sizeof *dst);
    } else {
        ts_chop_cubic_at(src, tv, cnt, dst);
    }
    return cnt + 1;
}

/* ---------------------------------------------------------------- unit arcs */

size_t ts_build_unit_arc(sv_pt start, sv_pt stop, bool ccw, sv_ts user, ts_conic dst[5])
{
    static const float QP[8][2] = {{1.0f, 0.0f},  {1.0f, 1.0f},   {0.0f, 1.0f},  {-1.0f, 1.0f},
                                   {-1.0f, 0.0f}, {-1.0f, -1.0f}, {0.0f, -1.0f}, {1.0f, -1.0f}};
    float x = ts_dot(start, stop);
    float y = ts_cross(start, stop);
    float ay = fabsf(y);
    size_t quadrant = 0;
    size_t count;
    size_t i;
    sv_pt last_q;
    sv_pt fin;
    float d;
    sv_ts t;
    if (ay <= TS_NEARLY_ZERO && x > 0.0f && ((y >= 0.0f && !ccw) || (y <= 0.0f && ccw))) {
        return 0;
    }
    if (ccw) {
        y = -y;
    }
    if (y == 0.0f) {
        quadrant = 2;
    } else if (x == 0.0f) {
        quadrant = y > 0.0f ? 1 : 3;
    } else {
        if (y < 0.0f) {
            quadrant += 2;
        }
        if ((x < 0.0f) != (y < 0.0f)) {
            quadrant += 1;
        }
    }
    count = quadrant;
    for (i = 0; i < count; i++) {
        int k;
        for (k = 0; k < 3; k++) {
            dst[i].p[k] = ts_pt(QP[i * 2 + (size_t)k][0], QP[i * 2 + (size_t)k][1]);
        }
        dst[i].w = TS_ROOT_2_OVER_2;
    }
    fin = ts_pt(x, y);
    last_q = ts_pt(QP[quadrant * 2][0], QP[quadrant * 2][1]);
    d = ts_dot(last_q, fin);
    if (d < 1.0f) {
        sv_pt off = ts_pt(last_q.x + x, last_q.y + y);
        float cth = sqrtf((1.0f + d) / 2.0f);
        (void)ts_set_length(&off, 1.0f / cth);
        if (ts_can_normalize(ts_sub(last_q, off))) {
            dst[count].p[0] = last_q;
            dst[count].p[1] = off;
            dst[count].p[2] = fin;
            dst[count].w = cth;
            count++;
        }
    }
    t = ts_row(start.x, start.y, -start.y, start.x, 0.0f, 0.0f);
    if (ccw) {
        t = ts_pre_concat(t, ts_scale(1.0f, -1.0f));
    }
    t = ts_post_concat(t, user);
    for (i = 0; i < count; i++) {
        ts_map_points(t, dst[i].p, 3);
    }
    return count;
}

/* ---------------------------------------------------------------- path64 (f64 cubic / line) */

#define DBL_EPS_ERR (DBL_EPSILON * 4.0)
#define FLT_EPS_HALF ((double)(FLT_EPSILON / 2.0f))
#define FLT_EPS_INV (1.0 / (double)FLT_EPSILON)
#define BSEARCH_MAX 4096 /* binary_search terminates within ~1100 halvings; a hard cap regardless */

static bool approx_zero(double v)
{
    return fabs(v) < DBL_EPSILON;
}

static bool approx_zero_or_more(double v)
{
    return v > -DBL_EPSILON;
}

static bool approx_one_or_less(double v)
{
    return v < 1.0 + DBL_EPSILON;
}

static bool approx_zero_inverse(double v)
{
    return fabs(v) > FLT_EPS_INV;
}

static bool approx_zero_cubed(double v)
{
    return fabs(v) < (double)(FLT_EPSILON * FLT_EPSILON * FLT_EPSILON);
}

static bool approx_zero_compared(double a, double b)
{
    return a == 0.0 || fabs(a) < fabs(b * (double)FLT_EPSILON);
}

static bool approx_equal(double a, double b)
{
    return approx_zero(a - b);
}

static bool approx_equal_half(double a, double b)
{
    return a - b < FLT_EPS_HALF;
}

static bool between64(double v, double a, double b)
{
    return (a - v) * (b - v) <= 0.0;
}

static int32_t as_2s(float x)
{
    uint32_t u;
    int32_t i;
    memcpy(&u, &x, 4);
    i = pc_i32_from_bits(u);
    if (i < 0) {
        i &= 0x7FFFFFFF;
        i = -i;
    }
    return i;
}

static bool almost_dequal_ulps_f(float a, float b)
{
    int32_t ab = as_2s(a);
    int32_t bb = as_2s(b);
    return ab < ts_wadd(bb, 16) && bb < ts_wadd(ab, 16);
}

static bool almost_dequal_ulps(double a, double b)
{
    if (fabs(a) < (double)TS_SCALAR_MAX && fabs(b) < (double)TS_SCALAR_MAX) {
        return almost_dequal_ulps_f(pc_f64_to_f32(a), pc_f64_to_f32(b));
    }
    return fabs(a - b) / pc_rs_max(fabs(a), fabs(b)) < (double)(FLT_EPSILON * 16.0f);
}

static double bound64(double v, double lo, double hi)
{
    return pc_rs_max_k(pc_rs_min(hi, v), lo);
}

static size_t quad_handle_zero(double b, double c, double s[3])
{
    if (approx_zero(b)) {
        s[0] = 0.0;
        return c == 0.0 ? 1 : 0;
    }
    s[0] = -c / b;
    return 1;
}

static size_t quad64_roots_real(double a, double b, double c, double s[3])
{
    double p;
    double q;
    double p2;
    double sd = 0.0;
    if (a == 0.0) {
        return quad_handle_zero(b, c, s);
    }
    p = b / (2.0 * a);
    q = c / a;
    if (approx_zero(a) && (approx_zero_inverse(p) || approx_zero_inverse(q))) {
        return quad_handle_zero(b, c, s);
    }
    p2 = p * p;
    if (!almost_dequal_ulps(p2, q) && p2 < q) {
        return 0;
    }
    if (p2 > q) {
        sd = sqrt(p2 - q);
    }
    s[0] = sd - p;
    s[1] = -sd - p;
    return 1 + (almost_dequal_ulps(s[0], s[1]) ? 0 : 1);
}

/* t holds `cap` values. */
static size_t push_valid_ts(const double *s, size_t nreal, double *t, size_t cap)
{
    size_t found = 0;
    size_t i;
    size_t k;
    for (i = 0; i < nreal; i++) {
        double tv = s[i];
        bool dup = false;
        if (!(approx_zero_or_more(tv) && approx_one_or_less(tv))) {
            continue;
        }
        tv = bound64(tv, 0.0, 1.0);
        for (k = 0; k < found; k++) {
            if (approx_equal(t[k], tv)) {
                dup = true;
                break;
            }
        }
        if (!dup && found < cap) {
            t[found++] = tv;
        }
    }
    return found;
}

static size_t quad64_roots_valid_t(double a, double b, double c, double *t, size_t cap)
{
    double s[3] = {0.0, 0.0, 0.0};
    size_t n = quad64_roots_real(a, b, c, s);
    return push_valid_ts(s, n, t, cap);
}

static double cube_root(double x)
{
    double d;
    double a;
    uint64_t bits;
    uint32_t hi;
    int i;
    if (approx_zero_cubed(x)) {
        return 0.0;
    }
    d = fabs(x);
    memcpy(&bits, &d, 8);
    hi = (uint32_t)(bits >> 32) / 3u + 715094163u;
    bits = (uint64_t)hi << 32;
    memcpy(&a, &bits, 8);
    for (i = 0; i < 3; i++) {
        double a3 = a * a * a;
        a = a * (a3 + d + d) / (a3 + a3 + d);
    }
    return x < 0.0 ? -a : a;
}

static size_t cubic64_roots_real(double a, double b, double c, double d, double s[3])
{
    double inv;
    double a2;
    double q;
    double r;
    double r2;
    double q3;
    double r2mq3;
    double adiv3;
    size_t off = 0;
    size_t i;
    if (approx_zero(a) && approx_zero_compared(a, b) && approx_zero_compared(a, c) &&
        approx_zero_compared(a, d)) {
        return quad64_roots_real(b, c, d, s);
    }
    if (approx_zero_compared(d, a) && approx_zero_compared(d, b) && approx_zero_compared(d, c)) {
        size_t num = quad64_roots_real(a, b, c, s);
        for (i = 0; i < num; i++) {
            if (approx_zero(s[i])) {
                return num;
            }
        }
        s[num++] = 0.0;
        return num;
    }
    if (approx_zero(a + b + c + d)) {
        size_t num = quad64_roots_real(a, a + b, -d, s);
        for (i = 0; i < num; i++) {
            if (almost_dequal_ulps(s[i], 1.0)) {
                return num;
            }
        }
        s[num++] = 1.0;
        return num;
    }
    inv = 1.0 / a;
    a = b * inv;
    b = c * inv;
    c = d * inv;
    a2 = a * a;
    q = (a2 - b * 3.0) / 9.0;
    r = (2.0 * a2 * a - 9.0 * a * b + 27.0 * c) / 54.0;
    r2 = r * r;
    q3 = q * q * q;
    r2mq3 = r2 - q3;
    adiv3 = a / 3.0;
    if (r2mq3 < 0.0) {
        const double pi = 3.141592653589793;
        double theta = acos(bound64(r / sqrt(q3), -1.0, 1.0));
        double n2rq = -2.0 * sqrt(q);
        double rr = n2rq * cos(theta / 3.0) - adiv3;
        s[off++] = rr;
        rr = n2rq * cos((theta + 2.0 * pi) / 3.0) - adiv3;
        if (!almost_dequal_ulps(s[0], rr)) {
            s[off++] = rr;
        }
        rr = n2rq * cos((theta - 2.0 * pi) / 3.0) - adiv3;
        if (!almost_dequal_ulps(s[0], rr) && (off == 1 || !almost_dequal_ulps(s[1], rr))) {
            s[off++] = rr;
        }
    } else {
        double sq = sqrt(r2mq3);
        double aa = fabs(r) + sq;
        double rr2;
        aa = cube_root(aa);
        if (r > 0.0) {
            aa = -aa;
        }
        if (aa != 0.0) {
            aa += q / aa;
        }
        rr2 = aa - adiv3;
        s[off++] = rr2;
        if (almost_dequal_ulps(rr2, q3)) {
            rr2 = -aa / 2.0 - adiv3;
            if (!almost_dequal_ulps(s[0], rr2)) {
                s[off++] = rr2;
            }
        }
    }
    return off;
}

static size_t cubic64_roots_valid_t(double a, double b, double c, double d, double t[3])
{
    double s[3] = {0.0, 0.0, 0.0};
    size_t real = cubic64_roots_real(a, b, c, d, s);
    size_t found = push_valid_ts(s, real, t, 3);
    size_t i;
    size_t k;
    for (i = 0; i < real; i++) {
        double tv = s[i];
        bool dup = false;
        if (!approx_one_or_less(tv) && between64(tv, 1.0, 1.00005)) {
            for (k = 0; k < found; k++) {
                if (approx_equal(t[k], 1.0)) {
                    dup = true;
                    break;
                }
            }
            if (!dup && found < 3) {
                t[found++] = 1.0;
            }
        } else if (!approx_zero_or_more(tv) && between64(tv, -0.00005, 0.0)) {
            for (k = 0; k < found; k++) {
                if (approx_equal(t[k], 0.0)) {
                    dup = true;
                    break;
                }
            }
            if (!dup && found < 3) {
                t[found++] = 0.0;
            }
        }
    }
    return found;
}

typedef struct p64 {
    double x, y;
} p64;

static p64 cubic_at(const p64 c[4], double t)
{
    double u;
    double u2;
    double a;
    double b;
    double t2;
    double cc;
    double d;
    p64 r;
    if (t == 0.0) {
        return c[0];
    }
    if (t == 1.0) {
        return c[3];
    }
    u = 1.0 - t;
    u2 = u * u;
    a = u2 * u;
    b = 3.0 * u2 * t;
    t2 = t * t;
    cc = 3.0 * u * t2;
    d = t2 * t;
    r.x = a * c[0].x + b * c[1].x + cc * c[2].x + d * c[3].x;
    r.y = a * c[0].y + b * c[1].y + cc * c[2].y + d * c[3].y;
    return r;
}

static double axis(p64 p, bool x_axis)
{
    return x_axis ? p.x : p.y;
}

static size_t find_inflections64(const p64 c[4], double *t, size_t cap)
{
    double ax = c[1].x - c[0].x;
    double ay = c[1].y - c[0].y;
    double bx = c[2].x - 2.0 * c[1].x + c[0].x;
    double by = c[2].y - 2.0 * c[1].y + c[0].y;
    double cx = c[3].x + 3.0 * (c[1].x - c[2].x) - c[0].x;
    double cy = c[3].y + 3.0 * (c[1].y - c[2].y) - c[0].y;
    return quad64_roots_valid_t(bx * cy - by * cx, ax * cy - ay * cx, ax * by - ay * bx, t, cap);
}

static double binary_search(const p64 c[4], double min, double max, double ai, bool xa)
{
    double t = (min + max) / 2.0;
    double step = (t - min) / 2.0;
    p64 at = cubic_at(c, t);
    double pos = axis(at, xa);
    double dist = pos - ai;
    int iter;
    for (iter = 0; iter < BSEARCH_MAX; iter++) {
        double prior = pc_rs_max(min, t - step);
        p64 less = cubic_at(c, prior);
        double less_dist;
        double last_step;
        bool ok;
        if (approx_equal_half(less.x, at.x) && approx_equal_half(less.y, at.y)) {
            return -1.0;
        }
        less_dist = axis(less, xa) - ai;
        last_step = step;
        step /= 2.0;
        ok = dist > 0.0 ? dist > less_dist : dist < less_dist;
        if (ok) {
            t = prior;
        } else {
            double next = t + last_step;
            p64 more;
            double more_dist;
            if (next > max) {
                return -1.0;
            }
            more = cubic_at(c, next);
            if (approx_equal_half(more.x, at.x) && approx_equal_half(more.y, at.y)) {
                return -1.0;
            }
            more_dist = axis(more, xa) - ai;
            ok = dist > 0.0 ? dist <= more_dist : dist >= more_dist;
            if (ok) {
                continue;
            }
            t = next;
        }
        at = cubic_at(c, t);
        pos = axis(at, xa);
        dist = pos - ai;
        if (approx_equal(pos, ai)) {
            return t;
        }
    }
    return -1.0;
}

static size_t search_roots(const p64 c[4], size_t extrema, double ai, bool xa, double ets[6], double roots[3])
{
    size_t valid = 0;
    size_t index = 0;
    size_t i;
    if (extrema > 4) {
        return 0;
    }
    extrema += find_inflections64(c, ets + extrema, 6 - extrema);
    if (extrema + 1 >= 6) {
        return 0;
    }
    ets[extrema] = 0.0;
    extrema += 1;
    ets[extrema] = 1.0;
    /* stable insertion sort of ets[0..extrema) */
    for (i = 1; i < extrema; i++) {
        double v = ets[i];
        size_t k = i;
        while (k > 0 && ets[k - 1] > v) {
            ets[k] = ets[k - 1];
            k--;
        }
        ets[k] = v;
    }
    while (index < extrema) {
        double mn = ets[index];
        double mx;
        double nt;
        index++;
        mx = ets[index];
        if (mn == mx) {
            continue;
        }
        nt = binary_search(c, mn, mx, ai, xa);
        if (nt >= 0.0) {
            if (valid >= 3) {
                return 0;
            }
            roots[valid++] = nt;
        }
    }
    return valid;
}

static void coefficients(double s0, double s1, double s2, double s3, double out[4])
{
    double a = s3;
    double b = s2 * 3.0;
    double c = s1 * 3.0;
    double d = s0;
    a -= d - c + b;
    b += 3.0 * d - 2.0 * c;
    c -= 3.0 * d;
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}

static size_t find_extrema64(double a, double b, double c, double d, double t[6])
{
    double a2 = d - a + 3.0 * (b - c);
    double b2 = 2.0 * (a - b - b + c);
    double c2 = b - a;
    return quad64_roots_valid_t(a2, b2, c2, t, 6);
}

static size_t intersect(const p64 c[4], double ai, bool xa, double roots[3])
{
    double k[4];
    double v[4];
    size_t count;
    size_t i;
    for (i = 0; i < 4; i++) {
        v[i] = axis(c[i], xa);
    }
    coefficients(v[0], v[1], v[2], v[3], k);
    k[3] -= ai;
    count = cubic64_roots_valid_t(k[0], k[1], k[2], k[3], roots);
    for (i = 0; i < count; i++) {
        p64 p = cubic_at(c, roots[i]);
        if (!approx_equal(axis(p, xa), ai)) {
            double ets[6] = {0, 0, 0, 0, 0, 0};
            size_t ex = find_extrema64(v[0], v[1], v[2], v[3], ets);
            return search_roots(c, ex, ai, xa, ets, roots);
        }
    }
    return count;
}

static double interp64(double a, double b, double t)
{
    return a + (b - a) * t;
}

static bool dchop_at_intercept(const sv_pt src[4], float v, bool vertical, sv_pt dst[7])
{
    p64 c[4];
    double roots[3] = {0.0, 0.0, 0.0};
    p64 o[7];
    size_t i;
    double t;
    for (i = 0; i < 4; i++) {
        c[i].x = (double)src[i].x;
        c[i].y = (double)src[i].y;
    }
    if (intersect(c, (double)v, vertical, roots) == 0) {
        return false;
    }
    t = roots[0];
    if (t == 0.5) {
        o[0] = c[0];
        o[1].x = (c[0].x + c[1].x) / 2.0;
        o[1].y = (c[0].y + c[1].y) / 2.0;
        o[2].x = (c[0].x + 2.0 * c[1].x + c[2].x) / 4.0;
        o[2].y = (c[0].y + 2.0 * c[1].y + c[2].y) / 4.0;
        o[3].x = (c[0].x + 3.0 * (c[1].x + c[2].x) + c[3].x) / 8.0;
        o[3].y = (c[0].y + 3.0 * (c[1].y + c[2].y) + c[3].y) / 8.0;
        o[4].x = (c[1].x + 2.0 * c[2].x + c[3].x) / 4.0;
        o[4].y = (c[1].y + 2.0 * c[2].y + c[3].y) / 4.0;
        o[5].x = (c[2].x + c[3].x) / 2.0;
        o[5].y = (c[2].y + c[3].y) / 2.0;
        o[6] = c[3];
    } else {
        int k;
        for (k = 0; k < 2; k++) {
            double s0 = k == 0 ? c[0].x : c[0].y;
            double s1 = k == 0 ? c[1].x : c[1].y;
            double s2 = k == 0 ? c[2].x : c[2].y;
            double s3 = k == 0 ? c[3].x : c[3].y;
            double ab = interp64(s0, s1, t);
            double bc = interp64(s1, s2, t);
            double cd = interp64(s2, s3, t);
            double abc = interp64(ab, bc, t);
            double bcd = interp64(bc, cd, t);
            double abcd = interp64(abc, bcd, t);
            double vals[7] = {s0, ab, abc, abcd, bcd, cd, s3};
            size_t j;
            for (j = 0; j < 7; j++) {
                if (k == 0) {
                    o[j].x = vals[j];
                } else {
                    o[j].y = vals[j];
                }
            }
        }
    }
    for (i = 0; i < 7; i++) {
        dst[i] = ts_pt(pc_f64_to_f32(o[i].x), pc_f64_to_f32(o[i].y));
    }
    return true;
}

bool ts_chop_mono_cubic_at_x(const sv_pt src[4], float x, sv_pt dst[7])
{
    return dchop_at_intercept(src, x, true, dst);
}

bool ts_chop_mono_cubic_at_y(const sv_pt src[4], float y, sv_pt dst[7])
{
    return dchop_at_intercept(src, y, false, dst);
}
