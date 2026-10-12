/* tiny-skia-path 0.11.4 dash.rs (SkDashPath / SkContourMeasure): StrokeDash validation and
 * Path::dash, which measures each contour (curves subdivided until flat to 0.5 / res_scale) and
 * cuts it into the "on" intervals. The reference gives up past 1,000,000 dashes; the port also
 * gives up (draws nothing) past MAX_SEGS measured pieces or MAX_STEPS dash steps, bounds a
 * hostile path (huge coordinates subdivide every curve 2^20 times) would otherwise blow up. */
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "io/svg_ts.h"

#define MAX_T 0x3FFFFFFFu
#define MAX_SEGS ((size_t)1 << 23)
#define MAX_STEPS ((uint64_t)1 << 27)

enum { MS_LINE, MS_QUAD, MS_CUBIC };

typedef struct mseg {
    float distance;
    size_t point_index;
    uint32_t t_value;
    int kind;
} mseg;

typedef struct contour {
    mseg *segs;
    size_t nsegs, capsegs;
    sv_pt *pts;
    size_t npts, cappts;
    float length;
    bool closed;
    bool failed, limit;
} contour;

static float adjust_offset(float off, float len)
{
    if (off < 0.0f) {
        off = -off;
        if (off > len) {
            off = fmodf(off, len);
        }
        off = len - off;
        if (off == len) {
            off = 0.0f;
        }
        return off;
    }
    if (off >= len) {
        return fmodf(off, len);
    }
    return off;
}

bool ts_dash_new(const float *array, size_t n, float offset, ts_dash *out)
{
    float sum = -0.0f;
    size_t i;
    float off;
    if (!isfinite(offset) || array == NULL || n < 2 || n % 2 != 0) {
        return false;
    }
    for (i = 0; i < n; i++) {
        if (array[i] < 0.0f) {
            return false;
        }
        sum += array[i];
    }
    if (!(sum > 0.0f && isfinite(sum))) {
        return false;
    }
    off = adjust_offset(offset, sum);
    out->array = array;
    out->n = n;
    out->offset = off;
    out->interval_len = sum;
    out->first_len = array[0];
    out->first_index = 0;
    for (i = 0; i < n; i++) {
        float gap = array[i];
        if (off > gap || (off == gap && gap != 0.0f)) {
            off -= gap;
        } else {
            out->first_len = gap - off;
            out->first_index = i;
            return true;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- contour measure */

static void push_pt(contour *c, sv_pt p)
{
    if (c->failed) {
        return;
    }
    if (c->npts >= TS_MAX_POINTS) {
        c->failed = true;
        c->limit = true;
        return;
    }
    if (c->npts == c->cappts) {
        size_t cap = c->cappts;
        void *v = pc_grow_array(c->pts, &cap, c->npts + 1, sizeof *c->pts);
        if (v == NULL) {
            c->failed = true;
            return;
        }
        c->pts = v;
        c->cappts = cap;
    }
    c->pts[c->npts++] = p;
}

static void push_seg(contour *c, float d, size_t pi, uint32_t t, int kind)
{
    if (c->failed) {
        return;
    }
    if (c->nsegs >= MAX_SEGS) {
        c->failed = true;
        c->limit = true;
        return;
    }
    if (c->nsegs == c->capsegs) {
        size_t cap = c->capsegs;
        void *v = pc_grow_array(c->segs, &cap, c->nsegs + 1, sizeof *c->segs);
        if (v == NULL) {
            c->failed = true;
            return;
        }
        c->segs = v;
        c->capsegs = cap;
    }
    c->segs[c->nsegs].distance = d;
    c->segs[c->nsegs].point_index = pi;
    c->segs[c->nsegs].t_value = t;
    c->segs[c->nsegs].kind = kind;
    c->nsegs++;
}

static float line_seg(contour *c, sv_pt p0, sv_pt p1, float distance, size_t pi)
{
    float d = ts_length(ts_sub(p0, p1));
    float prev = distance;
    distance += d;
    if (distance > prev) {
        push_seg(c, distance, pi, MAX_T, MS_LINE);
    }
    return distance;
}

static bool quad_too_curvy(sv_pt p0, sv_pt p1, sv_pt p2, float tol)
{
    float dx = p1.x * 0.5f - ((p0.x + p2.x) * 0.5f) * 0.5f;
    float dy = p1.y * 0.5f - ((p0.y + p2.y) * 0.5f) * 0.5f;
    return pc_rs_maxf(fabsf(dx), fabsf(dy)) > tol;
}

static bool cheap_dist_exceeds(sv_pt p, float x, float y, float tol)
{
    return pc_rs_maxf(fabsf(x - p.x), fabsf(y - p.y)) > tol;
}

static float interp_safe(float a, float b, float t)
{
    return a + (b - a) * t;
}

static bool cubic_too_curvy(sv_pt p0, sv_pt p1, sv_pt p2, sv_pt p3, float tol)
{
    bool n0 = cheap_dist_exceeds(p1, interp_safe(p0.x, p3.x, 1.0f / 3.0f),
                                 interp_safe(p0.y, p3.y, 1.0f / 3.0f), tol);
    bool n1 = cheap_dist_exceeds(p2, interp_safe(p0.x, p3.x, 2.0f / 3.0f),
                                 interp_safe(p0.y, p3.y, 2.0f / 3.0f), tol);
    return n0 || n1;
}

/* Recursion depth <= 20: the t span halves from 2^30 until it drops below 2^10. */
static float quad_segs(contour *c, const sv_pt p[3], float distance, uint32_t mint, uint32_t maxt, size_t pi,
                       float tol)
{
    if (c->failed) {
        return distance;
    }
    if (((maxt - mint) >> 10) != 0 && quad_too_curvy(p[0], p[1], p[2], tol)) {
        sv_pt tmp[5];
        uint32_t half = (mint + maxt) >> 1;
        ts_chop_quad_at(p, 0.5f, tmp);
        distance = quad_segs(c, tmp, distance, mint, half, pi, tol);
        distance = quad_segs(c, tmp + 2, distance, half, maxt, pi, tol);
    } else {
        float d = ts_length(ts_sub(p[0], p[2]));
        float prev = distance;
        distance += d;
        if (distance > prev) {
            push_seg(c, distance, pi, maxt, MS_QUAD);
        }
    }
    return distance;
}

static float cubic_segs(contour *c, const sv_pt p[4], float distance, uint32_t mint, uint32_t maxt, size_t pi,
                        float tol)
{
    if (c->failed) {
        return distance;
    }
    if (((maxt - mint) >> 10) != 0 && cubic_too_curvy(p[0], p[1], p[2], p[3], tol)) {
        sv_pt tmp[7];
        uint32_t half = (mint + maxt) >> 1;
        ts_chop_cubic_at2(p, 0.5f, tmp);
        distance = cubic_segs(c, tmp, distance, mint, half, pi, tol);
        distance = cubic_segs(c, tmp + 3, distance, half, maxt, pi, tol);
    } else {
        float d = ts_length(ts_sub(p[0], p[3]));
        float prev = distance;
        distance += d;
        if (distance > prev) {
            push_seg(c, distance, pi, maxt, MS_CUBIC);
        }
    }
    return distance;
}

/* ContourMeasureIter::next: false at the end (or when the reference stops: a non-finite length). */
static bool next_contour(ts_iter *it, float tol, contour *c)
{
    size_t pi = 0;
    float distance = 0.0f;
    bool seen_close = false;
    sv_pt prev = {0.0f, 0.0f};
    ts_seg seg;
    c->nsegs = 0;
    c->npts = 0;
    while (ts_iter_next(it, &seg)) {
        switch (seg.verb) {
        case V_MOVE:
            push_pt(c, seg.p[0]);
            prev = seg.p[0];
            break;
        case V_LINE: {
            float pd = distance;
            distance = line_seg(c, prev, seg.p[0], distance, pi);
            if (distance > pd) {
                push_pt(c, seg.p[0]);
                pi += 1;
            }
            prev = seg.p[0];
            break;
        }
        case V_QUAD: {
            float pd = distance;
            sv_pt q[3];
            q[0] = prev;
            q[1] = seg.p[0];
            q[2] = seg.p[1];
            distance = quad_segs(c, q, distance, 0, MAX_T, pi, tol);
            if (distance > pd) {
                push_pt(c, seg.p[0]);
                push_pt(c, seg.p[1]);
                pi += 2;
            }
            prev = seg.p[1];
            break;
        }
        case V_CUBIC: {
            float pd = distance;
            sv_pt q[4];
            q[0] = prev;
            q[1] = seg.p[0];
            q[2] = seg.p[1];
            q[3] = seg.p[2];
            distance = cubic_segs(c, q, distance, 0, MAX_T, pi, tol);
            if (distance > pd) {
                push_pt(c, seg.p[0]);
                push_pt(c, seg.p[1]);
                push_pt(c, seg.p[2]);
                pi += 3;
            }
            prev = seg.p[2];
            break;
        }
        case V_CLOSE:
        default:
            seen_close = true;
            break;
        }
        if (c->failed || ts_iter_next_verb(it) == V_MOVE) {
            break;
        }
    }
    if (c->failed || !isfinite(distance)) {
        return false;
    }
    if (seen_close && c->npts > 0 && pi < c->npts) {
        float pd = distance;
        sv_pt first = c->pts[0];
        distance = line_seg(c, c->pts[pi], first, distance, pi);
        if (distance > pd) {
            push_pt(c, first);
        }
    }
    c->length = distance;
    c->closed = seen_close;
    return c->npts > 0 && !c->failed;
}

static float interp(float a, float b, float t)
{
    return a + (b - a) * t;
}

static float scalar_t(const mseg *s)
{
    return (float)s->t_value * (1.0f / (float)MAX_T);
}

static bool distance_to_segment(const contour *c, float d, size_t *index, float *t)
{
    uint32_t lo = 0;
    uint32_t hi = (uint32_t)(c->nsegs - 1);
    int32_t idx;
    const mseg *seg;
    float st = 0.0f;
    float sd = 0.0f;
    float v;
    while (lo < hi) {
        uint32_t mid = (hi + lo) >> 1;
        if (c->segs[mid].distance < d) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (c->segs[hi].distance < d) {
        hi += 1;
        hi = ~hi;
    } else if (d < c->segs[hi].distance) {
        hi = ~hi;
    }
    idx = pc_i32_from_bits(hi);
    idx ^= idx >> 31;
    if (idx < 0 || (size_t)idx >= c->nsegs) {
        return false;
    }
    seg = &c->segs[idx];
    if (idx > 0) {
        sd = c->segs[idx - 1].distance;
        if (c->segs[idx - 1].point_index == seg->point_index) {
            st = scalar_t(&c->segs[idx - 1]);
        }
    }
    v = st + (scalar_t(seg) - st) * (d - sd) / (seg->distance - sd);
    if (!(isfinite(v) && v >= 0.0f && v <= 1.0f)) {
        return false;
    }
    *index = (size_t)idx;
    *t = v;
    return true;
}

static float excl_bounded(float v)
{
    return pc_rs_maxf_k(pc_rs_minf(1.0f - FLT_EPSILON, v), FLT_EPSILON);
}

static bool pts_ok(const contour *c, size_t pi, int kind)
{
    size_t need = kind == MS_LINE ? 2 : kind == MS_QUAD ? 3 : 4;
    return pi < c->npts && c->npts - pi >= need;
}

static void segment_to(const contour *c, size_t pi, int kind, float start_t, float stop_t, ts_pb *pb)
{
    const sv_pt *p;
    sv_pt t0[7];
    sv_pt t1[7];
    if (start_t == stop_t) {
        sv_pt last;
        if (ts_pb_last_point(pb, &last)) {
            ts_pb_line_to(pb, last.x, last.y);
        }
        return;
    }
    if (!pts_ok(c, pi, kind)) {
        return;
    }
    p = c->pts + pi;
    if (kind == MS_LINE) {
        if (stop_t == 1.0f) {
            ts_pb_line_to(pb, p[1].x, p[1].y);
        } else {
            ts_pb_line_to(pb, interp(p[0].x, p[1].x, stop_t), interp(p[0].y, p[1].y, stop_t));
        }
    } else if (kind == MS_QUAD) {
        if (start_t == 0.0f) {
            if (stop_t == 1.0f) {
                ts_pb_quad_to(pb, p[1].x, p[1].y, p[2].x, p[2].y);
            } else {
                ts_chop_quad_at(p, excl_bounded(stop_t), t0);
                ts_pb_quad_to(pb, t0[1].x, t0[1].y, t0[2].x, t0[2].y);
            }
        } else {
            ts_chop_quad_at(p, excl_bounded(start_t), t0);
            if (stop_t == 1.0f) {
                ts_pb_quad_to(pb, t0[3].x, t0[3].y, t0[4].x, t0[4].y);
            } else {
                float nt = excl_bounded((stop_t - start_t) / (1.0f - start_t));
                ts_chop_quad_at(t0 + 2, nt, t1);
                ts_pb_quad_to(pb, t1[1].x, t1[1].y, t1[2].x, t1[2].y);
            }
        }
    } else {
        if (start_t == 0.0f) {
            if (stop_t == 1.0f) {
                ts_pb_cubic_to(pb, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
            } else {
                ts_chop_cubic_at2(p, excl_bounded(stop_t), t0);
                ts_pb_cubic_to(pb, t0[1].x, t0[1].y, t0[2].x, t0[2].y, t0[3].x, t0[3].y);
            }
        } else {
            ts_chop_cubic_at2(p, excl_bounded(start_t), t0);
            if (stop_t == 1.0f) {
                ts_pb_cubic_to(pb, t0[4].x, t0[4].y, t0[5].x, t0[5].y, t0[6].x, t0[6].y);
            } else {
                float nt = excl_bounded((stop_t - start_t) / (1.0f - start_t));
                ts_chop_cubic_at2(t0 + 3, nt, t1);
                ts_pb_cubic_to(pb, t1[1].x, t1[1].y, t1[2].x, t1[2].y, t1[3].x, t1[3].y);
            }
        }
    }
}

static void push_segment(const contour *c, float sd, float ed, bool move_to, ts_pb *pb)
{
    size_t si;
    size_t ei;
    float st;
    float et;
    const mseg *seg;
    const mseg *stop;
    if (sd < 0.0f) {
        sd = 0.0f;
    }
    if (ed > c->length) {
        ed = c->length;
    }
    if (!(sd <= ed) || c->nsegs == 0) {
        return;
    }
    if (!distance_to_segment(c, sd, &si, &st) || !distance_to_segment(c, ed, &ei, &et)) {
        return;
    }
    seg = &c->segs[si];
    stop = &c->segs[ei];
    if (move_to) {
        sv_pt p;
        if (!pts_ok(c, seg->point_index, seg->kind)) {
            return;
        }
        if (seg->kind == MS_LINE) {
            const sv_pt *q = c->pts + seg->point_index;
            p = ts_pt(interp(q[0].x, q[1].x, st), interp(q[0].y, q[1].y, st));
        } else if (seg->kind == MS_QUAD) {
            p = ts_eval_quad_at(c->pts + seg->point_index, st);
        } else {
            p = ts_eval_cubic_pos_at(c->pts + seg->point_index, st);
        }
        ts_pb_move_to(pb, p.x, p.y);
    }
    if (seg->point_index == stop->point_index) {
        segment_to(c, seg->point_index, seg->kind, st, et, pb);
    } else {
        size_t ni = si;
        for (;;) {
            size_t old;
            segment_to(c, seg->point_index, seg->kind, st, 1.0f, pb);
            old = seg->point_index;
            do {
                ni++;
            } while (ni < c->nsegs && c->segs[ni].point_index == old);
            if (ni >= c->nsegs) {
                return;
            }
            seg = &c->segs[ni];
            st = 0.0f;
            if (seg->point_index >= stop->point_index) {
                break;
            }
        }
        segment_to(c, seg->point_index, seg->kind, 0.0f, et, pb);
    }
}

pc_status ts_dash_path(const ts_path *path, const ts_dash *d, float res_scale, ts_pb *out, bool *ok)
{
    contour c;
    ts_iter it;
    float tol = 0.5f * (1.0f / res_scale);
    float dash_count = 0.0f;
    uint64_t steps = 0;
    pc_status st = PC_OK;
    *ok = false;
    ts_pb_clear(out);
    memset(&c, 0, sizeof c);
    ts_iter_init(&it, path);
    while (next_contour(&it, tol, &c)) {
        bool skip_first = c.closed;
        bool added = false;
        size_t index = d->first_index;
        float distance = 0.0f;
        float dlen = d->first_len;
        dash_count += c.length * (float)(d->n >> 1) / d->interval_len;
        if (dash_count > 1000000.0f) {
            goto done; /* the reference gives up: no path */
        }
        while (distance < c.length) {
            if (++steps > MAX_STEPS) {
                st = PC_ERR_LIMIT;
                goto done;
            }
            added = false;
            if (index % 2 == 0 && !skip_first) {
                added = true;
                push_segment(&c, distance, distance + dlen, true, out);
            }
            distance += dlen;
            skip_first = false;
            index += 1;
            if (index == d->n) {
                index = 0;
            }
            dlen = d->array[index];
        }
        if (c.closed && d->first_index % 2 == 0 && d->first_len >= 0.0f) {
            push_segment(&c, 0.0f, d->first_len, !added, out);
        }
        if (out->failed) {
            break;
        }
    }
    if (c.limit || out->limit) {
        st = PC_ERR_LIMIT;
    } else if (c.failed || out->failed) {
        st = PC_ERR_NOMEM;
    } else {
        *ok = out->nv > 1;
    }
done:
    free(c.segs);
    free(c.pts);
    if (!*ok) {
        ts_pb_clear(out);
        out->failed = false;
        out->limit = false;
    }
    return st;
}
