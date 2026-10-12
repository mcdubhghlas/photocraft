/* tiny-skia-path 0.11.4 stroker.rs (Skia's SkStroke): the outline of a stroked path built in the
 * path's own space: offset curves approximated by quads (split until the quad is within
 * 1 / (4 * res_scale) of the true offset), joins (miter, miter-clip, round, bevel) and caps
 * (butt, round, square), cusps covered by circles. The reference's recursion limits apply to the
 * recursion depth; the total number of curve-stroking steps is capped too (WORK_MAX, far above
 * what real curves need), past which the stroke draws nothing. */
#include <stdlib.h>
#include <string.h>

#include "io/svg_ts.h"

#define WORK_MAX ((size_t)1 << 24)

static const int32_t RECURSIVE_LIMITS[4] = {5 * 3, 26 * 3, 11 * 3, 11 * 3};

enum { R_POINT, R_LINE, R_QUAD, R_DEGENERATE, R_DEGENERATE2, R_DEGENERATE3 };
enum { RT_SPLIT, RT_DEGENERATE, RT_QUAD };
enum { A_NEARLY180, A_SHARP, A_SHALLOW, A_NEARLY_LINE };

typedef struct quadc {
    sv_pt quad[3];
    sv_pt tangent_start, tangent_end;
    float start_t, mid_t, end_t;
    bool start_set, end_set, opposite_tangents;
} quadc;

typedef struct stroker {
    float radius, inv_miter_limit, res_scale, inv_res_scale, inv_res_scale_sq;
    sv_pt first_normal, prev_normal, first_unit_normal, prev_unit_normal;
    sv_pt first_pt, prev_pt, first_outer_pt;
    size_t first_outer_pt_index;
    int32_t segment_count;
    bool prev_is_line;
    sv_cap cap;
    sv_join join;
    ts_pb inner, outer, cusper;
    float stroke_type; /* 1 outer, -1 inner */
    int32_t recursion_depth;
    bool found_tangents, join_completed;
    size_t work;
    bool over;
} stroker;

static sv_pt rot_cw(sv_pt p)
{
    return ts_pt(-p.y, p.x);
}

static sv_pt rot_ccw(sv_pt p)
{
    return ts_pt(p.y, -p.x);
}

float ts_res_scale(sv_ts t)
{
    float sx = ts_length(ts_pt(t.sx, t.kx));
    float sy = ts_length(ts_pt(t.ky, t.sy));
    if (isfinite(sx) && isfinite(sy)) {
        float s = pc_rs_maxf(sx, sy);
        if (s > 0.0f) {
            return s;
        }
    }
    return 1.0f;
}

/* ---------------------------------------------------------------- helpers */

static bool set_normal_unit_normal(sv_pt before, sv_pt after, float scale, float radius, sv_pt *normal,
                                   sv_pt *unit)
{
    if (!ts_set_length_from(unit, (after.x - before.x) * scale, (after.y - before.y) * scale, 1.0f)) {
        return false;
    }
    *unit = rot_ccw(*unit);
    *normal = ts_scaled(*unit, radius);
    return true;
}

static bool set_normal_unit_normal2(sv_pt v, float radius, sv_pt *normal, sv_pt *unit)
{
    if (!ts_set_length_from(unit, v.x, v.y, 1.0f)) {
        return false;
    }
    *unit = rot_ccw(*unit);
    *normal = ts_scaled(*unit, radius);
    return true;
}

static bool degenerate_vector(sv_pt v)
{
    return !ts_can_normalize(v);
}

static float pt_to_line(sv_pt pt, sv_pt start, sv_pt end)
{
    sv_pt dxy = ts_sub(end, start);
    sv_pt ab0 = ts_sub(pt, start);
    float numer = ts_dot(dxy, ab0);
    float denom = ts_dot(dxy, dxy);
    float t = numer / denom;
    if (t >= 0.0f && t <= 1.0f) {
        sv_pt hit = ts_pt(start.x * (1.0f - t) + end.x * t, start.y * (1.0f - t) + end.y * t);
        return ts_dist_sqd(hit, pt);
    }
    return ts_dist_sqd(pt, start);
}

static bool points_within_dist(sv_pt a, sv_pt b, float limit)
{
    return ts_dist_sqd(a, b) <= limit * limit;
}

static bool sharp_angle(const sv_pt q[3])
{
    sv_pt smaller = ts_sub(q[1], q[0]);
    sv_pt larger = ts_sub(q[1], q[2]);
    float sl = ts_dot(smaller, smaller);
    float ll = ts_dot(larger, larger);
    if (sl > ll) {
        sv_pt t = smaller;
        smaller = larger;
        larger = t;
        ll = sl;
    }
    if (!ts_set_length(&smaller, ll)) {
        return false;
    }
    return ts_dot(smaller, larger) > 0.0f;
}

static bool pt_in_quad_bounds(const sv_pt q[3], sv_pt pt, float irs)
{
    float xmin = pc_rs_minf(pc_rs_minf(q[0].x, q[1].x), q[2].x);
    float xmax;
    float ymin;
    float ymax;
    if (pt.x + irs < xmin) {
        return false;
    }
    xmax = pc_rs_maxf(pc_rs_maxf(q[0].x, q[1].x), q[2].x);
    if (pt.x - irs > xmax) {
        return false;
    }
    ymin = pc_rs_minf(pc_rs_minf(q[0].y, q[1].y), q[2].y);
    if (pt.y + irs < ymin) {
        return false;
    }
    ymax = pc_rs_maxf(pc_rs_maxf(q[0].y, q[1].y), q[2].y);
    if (pt.y - irs > ymax) {
        return false;
    }
    return true;
}

static size_t intersect_quad_ray(const sv_pt line[2], const sv_pt q[3], float roots[3])
{
    sv_pt v = ts_sub(line[1], line[0]);
    float r[3];
    float a;
    float b;
    float c;
    int n;
    for (n = 0; n < 3; n++) {
        r[n] = (q[n].y - line[0].y) * v.x - (q[n].x - line[0].x) * v.y;
    }
    a = r[2];
    b = r[1];
    c = r[0];
    a += c - 2.0f * b;
    b -= c;
    return ts_find_unit_quad_roots(a, 2.0f * b, c, roots);
}

static bool quad_in_line(const sv_pt q[3])
{
    float pt_max = -1.0f;
    int o1 = 0;
    int o2 = 0;
    int i;
    int k;
    int mid;
    for (i = 0; i < 2; i++) {
        for (k = i + 1; k < 3; k++) {
            sv_pt d = ts_sub(q[k], q[i]);
            float tm = pc_rs_maxf(fabsf(d.x), fabsf(d.y));
            if (pt_max < tm) {
                o1 = i;
                o2 = k;
                pt_max = tm;
            }
        }
    }
    mid = o1 ^ o2 ^ 3;
    return pt_to_line(q[mid], q[o1], q[o2]) <= pt_max * pt_max * 0.000005f;
}

static int check_quad_linear(const sv_pt q[3], sv_pt *reduction)
{
    bool dab = degenerate_vector(ts_sub(q[1], q[0]));
    bool dbc = degenerate_vector(ts_sub(q[2], q[1]));
    float t;
    if (dab && dbc) {
        return R_POINT;
    }
    if (dab || dbc) {
        return R_LINE;
    }
    if (!quad_in_line(q)) {
        return R_QUAD;
    }
    t = ts_find_quad_max_curvature(q);
    if (t == 0.0f || t == 1.0f) {
        return R_LINE;
    }
    *reduction = ts_eval_quad_at(q, t);
    return R_DEGENERATE;
}

static bool cubic_in_line(const sv_pt c[4])
{
    float pt_max = -1.0f;
    int o1 = 0;
    int o2 = 0;
    int i;
    int k;
    int m1;
    int m2;
    float slop;
    for (i = 0; i < 3; i++) {
        for (k = i + 1; k < 4; k++) {
            sv_pt d = ts_sub(c[k], c[i]);
            float tm = pc_rs_maxf(fabsf(d.x), fabsf(d.y));
            if (pt_max < tm) {
                o1 = i;
                o2 = k;
                pt_max = tm;
            }
        }
    }
    m1 = (1 + (2 >> o2)) >> o1;
    m2 = o1 ^ o2 ^ m1;
    slop = pt_max * pt_max * 0.00001f;
    return pt_to_line(c[m1], c[o1], c[o2]) <= slop && pt_to_line(c[m2], c[o1], c[o2]) <= slop;
}

static int check_cubic_linear(const sv_pt c[4], sv_pt red[3], sv_pt *tangent_pt)
{
    bool dab = degenerate_vector(ts_sub(c[1], c[0]));
    bool dbc = degenerate_vector(ts_sub(c[2], c[1]));
    bool dcd = degenerate_vector(ts_sub(c[3], c[2]));
    float tv[3];
    size_t n;
    size_t i;
    int rc = 0;
    if (dab && dbc && dcd) {
        return R_POINT;
    }
    if ((int)dab + (int)dbc + (int)dcd == 2) {
        return R_LINE;
    }
    if (!cubic_in_line(c)) {
        *tangent_pt = dab ? c[2] : c[1];
        return R_QUAD;
    }
    n = ts_find_cubic_max_curvature(c, tv);
    for (i = 0; i < n; i++) {
        if (0.0f >= tv[i] || tv[i] >= 1.0f) {
            continue;
        }
        red[rc] = ts_eval_cubic_pos_at(c, tv[i]);
        if (!ts_pt_eq(red[rc], c[0]) && !ts_pt_eq(red[rc], c[3])) {
            rc++;
        }
    }
    return rc == 0 ? R_LINE : rc == 1 ? R_DEGENERATE : rc == 2 ? R_DEGENERATE2 : R_DEGENERATE3;
}

/* ---------------------------------------------------------------- joins and caps */

static int angle_type(float dot)
{
    if (dot >= 0.0f) {
        return ts_nearly_zero(1.0f - dot) ? A_NEARLY_LINE : A_SHALLOW;
    }
    return ts_nearly_zero(1.0f + dot) ? A_NEARLY180 : A_SHARP;
}

static bool is_clockwise(sv_pt before, sv_pt after)
{
    return before.x * after.y > before.y * after.x;
}

static void handle_inner_join(sv_pt pivot, sv_pt after, ts_pb *inner)
{
    ts_pb_line_to(inner, pivot.x, pivot.y);
    ts_pb_line_to(inner, pivot.x - after.x, pivot.y - after.y);
}

static void bevel_joiner(sv_pt before, sv_pt pivot, sv_pt after_unit, float radius, ts_pb *outer,
                         ts_pb *inner)
{
    sv_pt after = ts_scaled(after_unit, radius);
    if (!is_clockwise(before, after_unit)) {
        ts_pb *t = outer;
        outer = inner;
        inner = t;
        after = ts_neg(after);
    }
    ts_pb_line_to(outer, pivot.x + after.x, pivot.y + after.y);
    handle_inner_join(pivot, after, inner);
}

static void round_joiner(sv_pt before, sv_pt pivot, sv_pt after, float radius, ts_pb *outer, ts_pb *inner)
{
    float dot = ts_dot(before, after);
    bool ccw = false;
    ts_conic conics[5];
    size_t n;
    size_t i;
    if (angle_type(dot) == A_NEARLY_LINE) {
        return;
    }
    if (!is_clockwise(before, after)) {
        ts_pb *t = outer;
        outer = inner;
        inner = t;
        before = ts_neg(before);
        after = ts_neg(after);
        ccw = true;
    }
    n = ts_build_unit_arc(before, after, ccw, ts_row(radius, 0.0f, 0.0f, radius, pivot.x, pivot.y), conics);
    if (n > 0) {
        for (i = 0; i < n; i++) {
            ts_pb_conic_to(outer, conics[i].p[1].x, conics[i].p[1].y, conics[i].p[2].x, conics[i].p[2].y,
                           conics[i].w);
        }
        after = ts_scaled(after, radius);
        handle_inner_join(pivot, after, inner);
    }
}

static void do_blunt_or_clipped(ts_pb *outer, ts_pb *inner, sv_pt pivot, float radius, bool prev_is_line,
                                bool curr_is_line, sv_pt before, sv_pt mid, sv_pt after, float inv_ml,
                                bool clip)
{
    after = ts_scaled(after, radius);
    if (clip) {
        float cb;
        float sb;
        float x;
        sv_pt bt;
        sv_pt at;
        sv_pt c1;
        sv_pt c2;
        (void)ts_normalize(&mid);
        cb = ts_dot(before, mid);
        sb = ts_cross(before, mid);
        x = fabsf(sb) <= TS_NEARLY_ZERO ? 1.0f / inv_ml : ((1.0f / inv_ml) - cb) / sb;
        before = ts_scaled(before, radius);
        bt = rot_cw(before);
        at = rot_ccw(after);
        c1 = ts_add(ts_add(pivot, before), ts_scaled(bt, x));
        c2 = ts_add(ts_add(pivot, after), ts_scaled(at, x));
        if (prev_is_line) {
            ts_pb_set_last_point(outer, c1);
        } else {
            ts_pb_line_to(outer, c1.x, c1.y);
        }
        ts_pb_line_to(outer, c2.x, c2.y);
    }
    if (!curr_is_line) {
        ts_pb_line_to(outer, pivot.x + after.x, pivot.y + after.y);
    }
    handle_inner_join(pivot, after, inner);
}

static void do_miter(ts_pb *outer, ts_pb *inner, sv_pt pivot, float radius, bool prev_is_line,
                     bool curr_is_line, sv_pt mid, sv_pt after)
{
    after = ts_scaled(after, radius);
    if (prev_is_line) {
        ts_pb_set_last_point(outer, ts_pt(pivot.x + mid.x, pivot.y + mid.y));
    } else {
        ts_pb_line_to(outer, pivot.x + mid.x, pivot.y + mid.y);
    }
    if (!curr_is_line) {
        ts_pb_line_to(outer, pivot.x + after.x, pivot.y + after.y);
    }
    handle_inner_join(pivot, after, inner);
}

static void miter_joiner(sv_pt before, sv_pt pivot, sv_pt after, float radius, float inv_ml, bool clip,
                         bool prev_is_line, bool curr_is_line, ts_pb *outer, ts_pb *inner)
{
    float dot = ts_dot(before, after);
    int at = angle_type(dot);
    sv_pt mid;
    bool ccw;
    float shalf;
    if (at == A_NEARLY_LINE) {
        return;
    }
    if (at == A_NEARLY180) {
        mid = ts_scaled(ts_sub(after, before), radius / 2.0f);
        do_blunt_or_clipped(outer, inner, pivot, radius, prev_is_line, false, before, mid, after, inv_ml,
                            clip);
        return;
    }
    ccw = !is_clockwise(before, after);
    if (ccw) {
        ts_pb *t = outer;
        outer = inner;
        inner = t;
        before = ts_neg(before);
        after = ts_neg(after);
    }
    if (dot == 0.0f && inv_ml <= TS_ROOT_2_OVER_2) {
        mid = ts_scaled(ts_add(before, after), radius);
        do_miter(outer, inner, pivot, radius, prev_is_line, curr_is_line, mid, after);
        return;
    }
    if (at == A_SHARP) {
        mid = ts_pt(after.y - before.y, before.x - after.x);
        if (ccw) {
            mid = ts_neg(mid);
        }
    } else {
        mid = ts_pt(before.x + after.x, before.y + after.y);
    }
    shalf = sqrtf((1.0f + dot) * 0.5f);
    if (shalf < inv_ml) {
        do_blunt_or_clipped(outer, inner, pivot, radius, prev_is_line, false, before, mid, after, inv_ml,
                            clip);
        return;
    }
    (void)ts_set_length(&mid, radius / shalf);
    do_miter(outer, inner, pivot, radius, prev_is_line, curr_is_line, mid, after);
}

static void join(stroker *s, sv_join j, sv_pt before, sv_pt pivot, sv_pt after, bool prev_is_line,
                 bool curr_is_line)
{
    switch (j) {
    case JOIN_BEVEL:
        bevel_joiner(before, pivot, after, s->radius, &s->outer, &s->inner);
        break;
    case JOIN_ROUND:
        round_joiner(before, pivot, after, s->radius, &s->outer, &s->inner);
        break;
    case JOIN_MITER_CLIP:
        miter_joiner(before, pivot, after, s->radius, s->inv_miter_limit, true, prev_is_line, curr_is_line,
                     &s->outer, &s->inner);
        break;
    case JOIN_MITER:
    default:
        miter_joiner(before, pivot, after, s->radius, s->inv_miter_limit, false, prev_is_line, curr_is_line,
                     &s->outer, &s->inner);
        break;
    }
}

static void capper(sv_cap cap, sv_pt pivot, sv_pt normal, sv_pt stop, bool other, ts_pb *path)
{
    sv_pt par;
    if (cap == CAP_ROUND) {
        sv_pt pc;
        par = rot_cw(normal);
        pc = ts_add(pivot, par);
        ts_pb_conic_to(path, pc.x + normal.x, pc.y + normal.y, pc.x, pc.y, TS_ROOT_2_OVER_2);
        ts_pb_conic_to(path, pc.x - normal.x, pc.y - normal.y, stop.x, stop.y, TS_ROOT_2_OVER_2);
    } else if (cap == CAP_SQUARE) {
        par = rot_cw(normal);
        if (other) {
            ts_pb_set_last_point(path, ts_pt(pivot.x + normal.x + par.x, pivot.y + normal.y + par.y));
            ts_pb_line_to(path, pivot.x - normal.x + par.x, pivot.y - normal.y + par.y);
        } else {
            ts_pb_line_to(path, pivot.x + normal.x + par.x, pivot.y + normal.y + par.y);
            ts_pb_line_to(path, pivot.x - normal.x + par.x, pivot.y - normal.y + par.y);
            ts_pb_line_to(path, stop.x, stop.y);
        }
    } else {
        ts_pb_line_to(path, stop.x, stop.y);
    }
}

/* ---------------------------------------------------------------- contour state */

static void finish_contour(stroker *s, bool close, bool curr_is_line)
{
    if (s->segment_count > 0) {
        sv_pt pt = {0.0f, 0.0f};
        if (close) {
            join(s, s->join, s->prev_unit_normal, s->prev_pt, s->first_unit_normal, s->prev_is_line,
                 curr_is_line);
            ts_pb_close(&s->outer);
            (void)ts_pb_last_point(&s->inner, &pt);
            ts_pb_move_to(&s->outer, pt.x, pt.y);
            ts_pb_reverse_path_to(&s->outer, &s->inner);
            ts_pb_close(&s->outer);
        } else {
            (void)ts_pb_last_point(&s->inner, &pt);
            capper(s->cap, s->prev_pt, s->prev_normal, pt, curr_is_line, &s->outer);
            ts_pb_reverse_path_to(&s->outer, &s->inner);
            capper(s->cap, s->first_pt, ts_neg(s->first_normal), s->first_outer_pt, s->prev_is_line,
                   &s->outer);
            ts_pb_close(&s->outer);
        }
        if (s->cusper.nv > 0) {
            ts_pb_push_builder(&s->outer, &s->cusper);
            ts_pb_clear(&s->cusper);
        }
    }
    ts_pb_clear(&s->inner);
    s->segment_count = -1;
    s->first_outer_pt_index = s->outer.np;
}

static bool pre_join_to(stroker *s, sv_pt p, bool curr_is_line, sv_pt *normal, sv_pt *unit)
{
    float px = s->prev_pt.x;
    float py = s->prev_pt.y;
    if (!set_normal_unit_normal(s->prev_pt, p, s->res_scale, s->radius, normal, unit)) {
        if (s->cap == CAP_BUTT) {
            return false;
        }
        *normal = ts_pt(s->radius, 0.0f);
        *unit = ts_pt(1.0f, 0.0f);
    }
    if (s->segment_count == 0) {
        s->first_normal = *normal;
        s->first_unit_normal = *unit;
        s->first_outer_pt = ts_pt(px + normal->x, py + normal->y);
        ts_pb_move_to(&s->outer, s->first_outer_pt.x, s->first_outer_pt.y);
        ts_pb_move_to(&s->inner, px - normal->x, py - normal->y);
    } else {
        join(s, s->join, s->prev_unit_normal, s->prev_pt, *unit, s->prev_is_line, curr_is_line);
    }
    s->prev_is_line = curr_is_line;
    return true;
}

static void post_join_to(stroker *s, sv_pt p, sv_pt normal, sv_pt unit)
{
    s->join_completed = true;
    s->prev_pt = p;
    s->prev_unit_normal = unit;
    s->prev_normal = normal;
    s->segment_count += 1;
}

/* hv: has_valid_tangent of the segment iterator (-1 when there is no iterator). */
static void line_to(stroker *s, sv_pt p, int hv)
{
    float tol = TS_NEARLY_ZERO * s->inv_res_scale;
    bool teeny = fabsf(s->prev_pt.x - p.x) <= tol && fabsf(s->prev_pt.y - p.y) <= tol;
    sv_pt normal = {0.0f, 0.0f};
    sv_pt unit = {0.0f, 0.0f};
    if (s->cap == CAP_BUTT && teeny) {
        return;
    }
    if (teeny && (s->join_completed || hv == 1)) {
        return;
    }
    if (!pre_join_to(s, p, true, &normal, &unit)) {
        return;
    }
    ts_pb_line_to(&s->outer, p.x + normal.x, p.y + normal.y);
    ts_pb_line_to(&s->inner, p.x - normal.x, p.y - normal.y);
    post_join_to(s, p, normal, unit);
}

/* ---------------------------------------------------------------- quad construction */

static float norm_clamped(float v)
{
    if (!isfinite(v)) {
        return 0.0f;
    }
    return pc_rs_maxf_k(pc_rs_minf(1.0f, v), 0.0f);
}

static bool qc_init(quadc *q, float start, float end)
{
    q->start_t = start;
    q->mid_t = norm_clamped((start + end) * 0.5f);
    q->end_t = end;
    q->start_set = false;
    q->end_set = false;
    return q->start_t < q->mid_t && q->mid_t < q->end_t;
}

static bool qc_init_with_start(quadc *q, const quadc *parent)
{
    if (!qc_init(q, parent->start_t, parent->mid_t)) {
        return false;
    }
    q->quad[0] = parent->quad[0];
    q->tangent_start = parent->tangent_start;
    q->start_set = true;
    return true;
}

static bool qc_init_with_end(quadc *q, const quadc *parent)
{
    if (!qc_init(q, parent->mid_t, parent->end_t)) {
        return false;
    }
    q->quad[2] = parent->quad[2];
    q->tangent_end = parent->tangent_end;
    q->end_set = true;
    return true;
}

static void init_quad(stroker *s, float type, float start, float end, quadc *q)
{
    s->stroke_type = type;
    s->found_tangents = false;
    (void)qc_init(q, start, end);
}

static ts_pb *cur_path(stroker *s)
{
    return s->stroke_type > 0.0f ? &s->outer : &s->inner;
}

static void add_degenerate_line(stroker *s, const quadc *q)
{
    ts_pb_line_to(cur_path(s), q->quad[2].x, q->quad[2].y);
}

static void set_ray_points(const stroker *s, sv_pt tp, sv_pt *dxy, sv_pt *on, sv_pt *tangent)
{
    if (!ts_set_length(dxy, s->radius)) {
        *dxy = ts_pt(s->radius, 0.0f);
    }
    on->x = tp.x + s->stroke_type * dxy->y;
    on->y = tp.y - s->stroke_type * dxy->x;
    if (tangent != NULL) {
        tangent->x = on->x + dxy->x;
        tangent->y = on->y + dxy->y;
    }
}

static void quad_perp_ray(const stroker *s, const sv_pt q[3], float t, sv_pt *tp, sv_pt *on, sv_pt *tangent)
{
    sv_pt dxy;
    *tp = ts_eval_quad_at(q, t);
    dxy = ts_eval_quad_tangent_at(q, t);
    if (dxy.x == 0.0f && dxy.y == 0.0f) {
        dxy = ts_sub(q[2], q[0]);
    }
    set_ray_points(s, *tp, &dxy, on, tangent);
}

static void cubic_perp_ray(const stroker *s, const sv_pt c[4], float t, sv_pt *tp, sv_pt *on, sv_pt *tangent)
{
    sv_pt dxy;
    sv_pt chopped[7];
    *tp = ts_eval_cubic_pos_at(c, t);
    dxy = ts_eval_cubic_tangent_at(c, t);
    if (dxy.x == 0.0f && dxy.y == 0.0f) {
        const sv_pt *cp = c;
        if (ts_nearly_zero(t)) {
            dxy = ts_sub(c[2], c[0]);
        } else if (ts_nearly_zero(1.0f - t)) {
            dxy = ts_sub(c[3], c[1]);
        } else {
            ts_chop_cubic_at2(c, t, chopped);
            dxy = ts_sub(chopped[3], chopped[2]);
            if (dxy.x == 0.0f && dxy.y == 0.0f) {
                dxy = ts_sub(chopped[3], chopped[1]);
                cp = chopped;
            }
        }
        if (dxy.x == 0.0f && dxy.y == 0.0f) {
            dxy = ts_sub(cp[3], cp[0]);
        }
    }
    set_ray_points(s, *tp, &dxy, on, tangent);
}

static int intersect_ray(const stroker *s, bool ctrl_pt, quadc *q)
{
    sv_pt start = q->quad[0];
    sv_pt end = q->quad[2];
    sv_pt alen = ts_sub(q->tangent_start, start);
    sv_pt blen = ts_sub(q->tangent_end, end);
    float denom = ts_cross(alen, blen);
    sv_pt ab0;
    float na;
    float nb;
    if (denom == 0.0f || !isfinite(denom)) {
        q->opposite_tangents = ts_dot(alen, blen) < 0.0f;
        return RT_DEGENERATE;
    }
    q->opposite_tangents = false;
    ab0 = ts_sub(start, end);
    na = ts_cross(blen, ab0);
    nb = ts_cross(alen, ab0);
    if ((na >= 0.0f) == (nb >= 0.0f)) {
        float d1 = pt_to_line(start, end, q->tangent_end);
        float d2 = pt_to_line(end, start, q->tangent_start);
        if (pc_rs_maxf(d1, d2) <= s->inv_res_scale_sq) {
            return RT_DEGENERATE;
        }
        return RT_SPLIT;
    }
    na /= denom;
    if (na > na - 1.0f) {
        if (ctrl_pt) {
            q->quad[1].x = start.x * (1.0f - na) + q->tangent_start.x * na;
            q->quad[1].y = start.y * (1.0f - na) + q->tangent_start.y * na;
        }
        return RT_QUAD;
    }
    q->opposite_tangents = ts_dot(alen, blen) < 0.0f;
    return RT_DEGENERATE;
}

static int stroke_close_enough(const stroker *s, const sv_pt stroke[3], const sv_pt ray[2], const quadc *q)
{
    sv_pt mid = ts_eval_quad_at(stroke, 0.5f);
    float roots[3];
    size_t n;
    sv_pt qp;
    float err;
    if (points_within_dist(ray[0], mid, s->inv_res_scale)) {
        return sharp_angle(q->quad) ? RT_SPLIT : RT_QUAD;
    }
    if (!pt_in_quad_bounds(stroke, ray[0], s->inv_res_scale)) {
        return RT_SPLIT;
    }
    n = intersect_quad_ray(ray, stroke, roots);
    if (n != 1) {
        return RT_SPLIT;
    }
    qp = ts_eval_quad_at(stroke, roots[0]);
    err = s->inv_res_scale * (1.0f - fabsf(roots[0] - 0.5f) * 2.0f);
    if (points_within_dist(ray[0], qp, err)) {
        return sharp_angle(q->quad) ? RT_SPLIT : RT_QUAD;
    }
    return RT_SPLIT;
}

static int compare_quad_quad(const stroker *s, const sv_pt quad[3], quadc *q)
{
    sv_pt tmp;
    sv_pt ray[2];
    sv_pt copy[3];
    int rt;
    if (!q->start_set) {
        quad_perp_ray(s, quad, q->start_t, &tmp, &q->quad[0], &q->tangent_start);
        q->start_set = true;
    }
    if (!q->end_set) {
        quad_perp_ray(s, quad, q->end_t, &tmp, &q->quad[2], &q->tangent_end);
        q->end_set = true;
    }
    rt = intersect_ray(s, true, q);
    if (rt != RT_QUAD) {
        return rt;
    }
    quad_perp_ray(s, quad, q->mid_t, &ray[1], &ray[0], NULL);
    memcpy(copy, q->quad, sizeof copy);
    return stroke_close_enough(s, copy, ray, q);
}

static bool quad_stroke(stroker *s, const sv_pt quad[3], quadc *q)
{
    int rt;
    quadc half;
    if (++s->work > WORK_MAX) {
        s->over = true;
        return false;
    }
    rt = compare_quad_quad(s, quad, q);
    if (rt == RT_QUAD) {
        ts_pb_quad_to(cur_path(s), q->quad[1].x, q->quad[1].y, q->quad[2].x, q->quad[2].y);
        return true;
    }
    if (rt == RT_DEGENERATE) {
        add_degenerate_line(s, q);
        return true;
    }
    s->recursion_depth += 1;
    if (s->recursion_depth > RECURSIVE_LIMITS[3]) {
        return false;
    }
    memset(&half, 0, sizeof half);
    (void)qc_init_with_start(&half, q);
    if (!quad_stroke(s, quad, &half)) {
        return false;
    }
    (void)qc_init_with_end(&half, q);
    if (!quad_stroke(s, quad, &half)) {
        return false;
    }
    s->recursion_depth -= 1;
    return true;
}

static void cubic_quad_ends(const stroker *s, const sv_pt c[4], quadc *q)
{
    sv_pt tmp;
    if (!q->start_set) {
        cubic_perp_ray(s, c, q->start_t, &tmp, &q->quad[0], &q->tangent_start);
        q->start_set = true;
    }
    if (!q->end_set) {
        cubic_perp_ray(s, c, q->end_t, &tmp, &q->quad[2], &q->tangent_end);
        q->end_set = true;
    }
}

static int compare_quad_cubic(const stroker *s, const sv_pt c[4], quadc *q)
{
    sv_pt ray[2];
    sv_pt copy[3];
    int rt;
    cubic_quad_ends(s, c, q);
    rt = intersect_ray(s, true, q);
    if (rt != RT_QUAD) {
        return rt;
    }
    cubic_perp_ray(s, c, q->mid_t, &ray[1], &ray[0], NULL);
    memcpy(copy, q->quad, sizeof copy);
    return stroke_close_enough(s, copy, ray, q);
}

static bool cubic_mid_on_line(const stroker *s, const sv_pt c[4], const quadc *q)
{
    sv_pt mid;
    sv_pt tmp;
    cubic_perp_ray(s, c, q->mid_t, &tmp, &mid, NULL);
    return pt_to_line(mid, q->quad[0], q->quad[2]) < s->inv_res_scale_sq;
}

static bool cubic_stroke(stroker *s, const sv_pt c[4], quadc *q)
{
    quadc half;
    if (++s->work > WORK_MAX) {
        s->over = true;
        return false;
    }
    if (!s->found_tangents) {
        int rt;
        cubic_quad_ends(s, c, q);
        rt = intersect_ray(s, false, q);
        if (rt != RT_QUAD) {
            bool ok = points_within_dist(q->quad[0], q->quad[2], s->inv_res_scale);
            if ((rt == RT_DEGENERATE || ok) && cubic_mid_on_line(s, c, q)) {
                add_degenerate_line(s, q);
                return true;
            }
        } else {
            s->found_tangents = true;
        }
    }
    if (s->found_tangents) {
        int rt = compare_quad_cubic(s, c, q);
        if (rt == RT_QUAD) {
            ts_pb_quad_to(cur_path(s), q->quad[1].x, q->quad[1].y, q->quad[2].x, q->quad[2].y);
            return true;
        }
        if (rt == RT_DEGENERATE && !q->opposite_tangents) {
            add_degenerate_line(s, q);
            return true;
        }
    }
    if (!isfinite(q->quad[2].x)) {
        return false;
    }
    s->recursion_depth += 1;
    if (s->recursion_depth > RECURSIVE_LIMITS[s->found_tangents ? 1 : 0]) {
        return false;
    }
    memset(&half, 0, sizeof half);
    if (!qc_init_with_start(&half, q)) {
        add_degenerate_line(s, q);
        s->recursion_depth -= 1;
        return true;
    }
    if (!cubic_stroke(s, c, &half)) {
        return false;
    }
    if (!qc_init_with_end(&half, q)) {
        add_degenerate_line(s, q);
        s->recursion_depth -= 1;
        return true;
    }
    if (!cubic_stroke(s, c, &half)) {
        return false;
    }
    s->recursion_depth -= 1;
    return true;
}

/* ---------------------------------------------------------------- segments */

static void quad_to(stroker *s, sv_pt p1, sv_pt p2)
{
    sv_pt quad[3];
    sv_pt red = {0.0f, 0.0f};
    int rt;
    sv_pt nab = {0.0f, 0.0f};
    sv_pt uab = {0.0f, 0.0f};
    sv_pt nbc = {0.0f, 0.0f};
    sv_pt ubc = {0.0f, 0.0f};
    quadc q;
    quad[0] = s->prev_pt;
    quad[1] = p1;
    quad[2] = p2;
    rt = check_quad_linear(quad, &red);
    if (rt == R_POINT || rt == R_LINE) {
        line_to(s, p2, -1);
        return;
    }
    if (rt == R_DEGENERATE) {
        sv_join save = s->join;
        line_to(s, red, -1);
        s->join = JOIN_ROUND;
        line_to(s, p2, -1);
        s->join = save;
        return;
    }
    if (!pre_join_to(s, p1, false, &nab, &uab)) {
        line_to(s, p2, -1);
        return;
    }
    memset(&q, 0, sizeof q);
    init_quad(s, 1.0f, 0.0f, 1.0f, &q);
    (void)quad_stroke(s, quad, &q);
    init_quad(s, -1.0f, 0.0f, 1.0f, &q);
    (void)quad_stroke(s, quad, &q);
    if (!set_normal_unit_normal(quad[1], quad[2], s->res_scale, s->radius, &nbc, &ubc)) {
        nbc = nab;
        ubc = uab;
    }
    post_join_to(s, p2, nbc, ubc);
}

static void set_cubic_end_normal(stroker *s, const sv_pt c[4], sv_pt nab, sv_pt uab, sv_pt *ncd, sv_pt *ucd)
{
    sv_pt ab = ts_sub(c[1], c[0]);
    sv_pt cd = ts_sub(c[3], c[2]);
    bool dab = degenerate_vector(ab);
    bool dcb = degenerate_vector(cd);
    if (dab && dcb) {
        *ncd = nab;
        *ucd = uab;
        return;
    }
    if (dab) {
        ab = ts_sub(c[2], c[0]);
        dab = degenerate_vector(ab);
    }
    if (dcb) {
        cd = ts_sub(c[3], c[1]);
        dcb = degenerate_vector(cd);
    }
    if (dab || dcb) {
        *ncd = nab;
        *ucd = uab;
        return;
    }
    (void)set_normal_unit_normal2(cd, s->radius, ncd, ucd);
}

static void cubic_to(stroker *s, sv_pt p1, sv_pt p2, sv_pt p3)
{
    sv_pt c[4];
    sv_pt red[3];
    sv_pt tangent = {0.0f, 0.0f};
    int rt;
    sv_pt nab = {0.0f, 0.0f};
    sv_pt uab = {0.0f, 0.0f};
    sv_pt ncd = {0.0f, 0.0f};
    sv_pt ucd = {0.0f, 0.0f};
    float tv[3];
    size_t n;
    size_t i;
    float last_t = 0.0f;
    float cusp;
    c[0] = s->prev_pt;
    c[1] = p1;
    c[2] = p2;
    c[3] = p3;
    memset(red, 0, sizeof red);
    rt = check_cubic_linear(c, red, &tangent);
    if (rt == R_POINT || rt == R_LINE) {
        line_to(s, p3, -1);
        return;
    }
    if (rt >= R_DEGENERATE) {
        sv_join save = s->join;
        line_to(s, red[0], -1);
        s->join = JOIN_ROUND;
        if (rt >= R_DEGENERATE2) {
            line_to(s, red[1], -1);
        }
        if (rt == R_DEGENERATE3) {
            line_to(s, red[2], -1);
        }
        line_to(s, p3, -1);
        s->join = save;
        return;
    }
    if (!pre_join_to(s, tangent, false, &nab, &uab)) {
        line_to(s, p3, -1);
        return;
    }
    n = ts_find_cubic_inflections(c, tv);
    for (i = 0; i <= n; i++) {
        float next_t = i < n ? tv[i] : 1.0f;
        quadc q;
        memset(&q, 0, sizeof q);
        init_quad(s, 1.0f, last_t, next_t, &q);
        (void)cubic_stroke(s, c, &q);
        init_quad(s, -1.0f, last_t, next_t, &q);
        (void)cubic_stroke(s, c, &q);
        last_t = next_t;
    }
    if (ts_find_cubic_cusp(c, &cusp)) {
        sv_pt loc = ts_eval_cubic_pos_at(c, cusp);
        ts_pb_push_circle(&s->cusper, loc.x, loc.y, s->radius);
    }
    set_cubic_end_normal(s, c, nab, uab, &ncd, &ucd);
    post_join_to(s, p3, ncd, ucd);
}

/* PathSegmentsIter::has_valid_tangent for every verb position (O(n) instead of a scan per line). */
static uint8_t *valid_tangents(const ts_path *p)
{
    uint8_t *tab = malloc(p->nv + 1);
    size_t *pidx = NULL;
    size_t i;
    size_t pi = 0;
    if (tab == NULL) {
        return NULL;
    }
    pidx = malloc((p->nv + 1) * sizeof *pidx);
    if (pidx == NULL) {
        free(tab);
        return NULL;
    }
    for (i = 0; i < p->nv; i++) {
        uint8_t v = p->verbs[i];
        pidx[i] = pi;
        pi += v == V_MOVE || v == V_LINE ? 1 : v == V_QUAD ? 2 : v == V_CUBIC ? 3 : 0;
    }
    tab[p->nv] = 0;
    for (i = p->nv; i-- > 0;) {
        uint8_t v = p->verbs[i];
        size_t k = pidx[i];
        if (v == V_LINE) {
            tab[i] = tab[i + 1];
        } else if (v == V_QUAD && k + 2 <= p->np) {
            tab[i] =
                ts_pt_eq(p->pts[k], p->pts[k + 1]) && ts_pt_eq(p->pts[k + 1], p->pts[k + 1]) ? tab[i + 1] : 1;
        } else if (v == V_CUBIC && k + 3 <= p->np) {
            sv_pt p3 = p->pts[k + 2];
            tab[i] =
                ts_pt_eq(p3, p->pts[k]) && ts_pt_eq(p3, p->pts[k + 1]) && ts_pt_eq(p3, p3) ? tab[i + 1] : 1;
        } else {
            tab[i] = 0;
        }
    }
    free(pidx);
    return tab;
}

pc_status ts_stroke_path(const ts_path *path, const ts_stroke *st, float res_scale, ts_pb *out, bool *ok)
{
    stroker *s;
    ts_iter it;
    ts_seg seg;
    bool last_is_line = false;
    uint8_t *hv;
    pc_status ret = PC_OK;
    *ok = false;
    ts_pb_clear(out);
    if (!(st->width > 0.0f && isfinite(st->width))) {
        return PC_OK;
    }
    s = calloc(1, sizeof *s);
    hv = valid_tangents(path);
    if (s == NULL || hv == NULL) {
        free(s);
        free(hv);
        return PC_ERR_NOMEM;
    }
    s->join = st->join;
    if (s->join == JOIN_MITER) {
        if (st->miter_limit <= 1.0f) {
            s->join = JOIN_BEVEL;
        } else {
            s->inv_miter_limit = 1.0f / st->miter_limit;
        }
    }
    if (s->join == JOIN_MITER_CLIP) {
        s->inv_miter_limit = 1.0f / st->miter_limit;
    }
    s->res_scale = res_scale;
    s->inv_res_scale = 1.0f / (res_scale * 4.0f);
    s->inv_res_scale_sq = s->inv_res_scale * s->inv_res_scale;
    s->radius = st->width * 0.5f;
    s->segment_count = -1;
    s->cap = st->cap;
    s->stroke_type = 1.0f;
    ts_pb_init(&s->inner);
    ts_pb_init(&s->cusper);
    s->outer = *out; /* the result is built in out's storage */
    ts_pb_clear(&s->outer);
    ts_iter_init(&it, path);
    it.auto_close = true;
    while (ts_iter_next(&it, &seg) && !s->over) {
        switch (seg.verb) {
        case V_MOVE:
            if (s->segment_count > 0) {
                finish_contour(s, false, false);
            }
            s->segment_count = 0;
            s->first_pt = seg.p[0];
            s->prev_pt = seg.p[0];
            s->join_completed = false;
            break;
        case V_LINE:
            line_to(s, seg.p[0], it.vi <= path->nv && hv[it.vi] ? 1 : 0);
            last_is_line = true;
            break;
        case V_QUAD:
            quad_to(s, seg.p[0], seg.p[1]);
            last_is_line = false;
            break;
        case V_CUBIC:
            cubic_to(s, seg.p[0], seg.p[1], seg.p[2]);
            last_is_line = false;
            break;
        case V_CLOSE:
        default:
            if (s->cap != CAP_BUTT) {
                if (s->segment_count == 0) {
                    line_to(s, s->first_pt, -1);
                    last_is_line = true;
                    break;
                }
                if (ts_pb_zero_length_since(&s->inner, 0) &&
                    ts_pb_zero_length_since(&s->outer, s->first_outer_pt_index)) {
                    last_is_line = true;
                    break;
                }
            }
            finish_contour(s, true, last_is_line);
            break;
        }
    }
    finish_contour(s, false, last_is_line);
    if (s->over || s->outer.limit || s->inner.limit || s->cusper.limit) {
        ret = PC_ERR_LIMIT;
    } else if (s->outer.failed || s->inner.failed || s->cusper.failed) {
        ret = PC_ERR_NOMEM;
    }
    *out = s->outer;
    ts_pb_fini(&s->inner);
    ts_pb_fini(&s->cusper);
    free(s);
    free(hv);
    if (ret != PC_OK) {
        ts_pb_clear(out);
        out->failed = false;
        out->limit = false;
        return ret;
    }
    *ok = out->nv > 1;
    return PC_OK;
}
