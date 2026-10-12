/* Markers (port of usvg 0.45's parser/marker.rs): marker-start / -mid / -end of a shape are
 * converted into groups placed at the path's vertices, oriented (auto, auto-start-reverse or a
 * fixed angle), scaled by the stroke width (markerUnits) or the marker's viewBox, shifted by
 * refX / refY and clipped to the marker's box unless overflow is visible. Their content sees the
 * shape's fill and stroke as context-fill / context-stroke.
 *
 * Geometry is f32 as in usvg (tiny-skia transforms, f32 atan2 / fmod / sin / cos). A marker that
 * is already being converted is skipped (recursion), and one import draws at most
 * MAX_MARKER_INSTANCES markers in all (the reference has no limit: a path of a million points
 * with a heavy marker would otherwise multiply the tree). */
#include <math.h>
#include <string.h>

#include "io/svg_internal.h"

#define MAX_MARKER_INSTANCES 65536u
#define PI_F 3.14159265358979323846f

typedef enum { MS_MOVE, MS_LINE, MS_CUBIC, MS_CLOSE } mkind;

typedef struct mseg {
    mkind kind;
    sv_pt p1, p2, p; /* MS_MOVE / MS_LINE use p; MS_CUBIC all three */
} mseg;

/* The link of a marker-* property (inherited), or NULL. */
static const st_node *marker_link(const sv_conv *c, const st_node *n, sv_aid aid)
{
    const st_node *an = st_find_attr_node(n, aid);
    return an != NULL ? st_node_attr(c->doc, an, aid) : NULL;
}

bool sv_marker_is_valid(const sv_conv *c, const st_node *n)
{
    const st_node *p;
    /* marker-* can't be set on shapes inside a clipPath */
    for (p = n; p != NULL; p = p->parent) {
        if (p->kind == ST_ELEMENT && p->eid == E_CLIP_PATH) {
            return false;
        }
    }
    return marker_link(c, n, A_MARKER_START) != NULL || marker_link(c, n, A_MARKER_MID) != NULL ||
           marker_link(c, n, A_MARKER_END) != NULL;
}

/* ---------------------------------------------------------------- angles */

static float norm_rad(float rad)
{
    float v = fmodf(rad, PI_F * 2.0f);
    return v < 0.0f ? v + PI_F * 2.0f : v;
}

static float vector_angle(float vx, float vy)
{
    float rad = atan2f(vy, vx);
    return isnan(rad) ? 0.0f : norm_rad(rad);
}

static float calc_angle(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4)
{
    float in_a = vector_angle(x2 - x1, y2 - y1);
    float out_a = vector_angle(x4 - x3, y4 - y3);
    float d = (out_a - in_a) * 0.5f;
    float angle = in_a + d;
    if (PI_F / 2.0f < fabsf(d)) {
        angle -= PI_F;
    }
    /* f32::to_degrees */
    return norm_rad(angle) * 57.2957795130823208767981548141051703f;
}

static float line_angle(float x1, float y1, float x2, float y2)
{
    return calc_angle(x1, y1, x2, y2, x1, y1, x2, y2);
}

static bool eq4(float a, float b)
{
    return sv_approx_eq_ulps(a, b, 4);
}

static float curves_angle(float px, float py, float cx1, float cy1, float x, float y, float cx2, float cy2,
                          float nx, float ny)
{
    if (eq4(cx1, x) && eq4(cy1, y)) {
        return calc_angle(px, py, x, y, x, y, cx2, cy2);
    }
    if (eq4(x, cx2) && eq4(y, cy2)) {
        return calc_angle(cx1, cy1, x, y, x, y, nx, ny);
    }
    return calc_angle(cx1, cy1, x, y, x, y, cx2, cy2);
}

static sv_pt subpath_start(const mseg *s, size_t n, size_t idx)
{
    size_t offset = n - idx;
    size_t i = n - offset;
    sv_pt z = {0.0f, 0.0f};
    /* iter().rev().skip(offset): indices idx - 1 down to 0 */
    while (i > 0) {
        i--;
        if (s[i].kind == MS_MOVE) {
            return s[i].p;
        }
    }
    return z;
}

static sv_pt prev_vertex(const mseg *s, size_t n, size_t idx)
{
    const mseg *q = &s[idx - 1];
    if (q->kind == MS_CLOSE) {
        return subpath_start(s, n, idx);
    }
    return q->p;
}

static float vertex_angle(const mseg *s, size_t n, size_t idx)
{
    if (idx == 0) {
        const mseg *a = &s[0];
        const mseg *b;
        if (n < 2) {
            return 0.0f;
        }
        b = &s[1];
        if (a->kind == MS_MOVE && b->kind == MS_LINE) {
            return line_angle(a->p.x, a->p.y, b->p.x, b->p.y);
        }
        if (a->kind == MS_MOVE && b->kind == MS_CUBIC) {
            if (eq4(a->p.x, b->p1.x) && eq4(a->p.y, b->p1.y)) {
                return line_angle(a->p.x, a->p.y, b->p.x, b->p.y);
            }
            return line_angle(a->p.x, a->p.y, b->p1.x, b->p1.y);
        }
        return 0.0f;
    }
    if (idx == n - 1) {
        const mseg *a = &s[idx - 1];
        const mseg *b = &s[idx];
        switch (b->kind) {
        case MS_MOVE:
            return 0.0f;
        case MS_LINE: {
            sv_pt pv = prev_vertex(s, n, idx);
            return line_angle(pv.x, pv.y, b->p.x, b->p.y);
        }
        case MS_CUBIC:
            if (eq4(b->p2.x, b->p.x) && eq4(b->p2.y, b->p.y)) {
                return line_angle(b->p1.x, b->p1.y, b->p.x, b->p.y);
            }
            return line_angle(b->p2.x, b->p2.y, b->p.x, b->p.y);
        case MS_CLOSE:
        default:
            if (a->kind == MS_LINE) {
                sv_pt nx = subpath_start(s, n, idx);
                return line_angle(a->p.x, a->p.y, nx.x, nx.y);
            }
            if (a->kind == MS_CUBIC) {
                sv_pt pv = prev_vertex(s, n, idx);
                sv_pt nx = subpath_start(s, n, idx);
                return curves_angle(pv.x, pv.y, a->p2.x, a->p2.y, a->p.x, a->p.y, nx.x, nx.y, nx.x, nx.y);
            }
            return 0.0f;
        }
    }
    {
        const mseg *a = &s[idx];
        const mseg *b = &s[idx + 1];
        sv_pt pv;
        sv_pt nx;
        switch (a->kind) {
        case MS_MOVE:
            if (b->kind == MS_LINE) {
                return line_angle(a->p.x, a->p.y, b->p.x, b->p.y);
            }
            if (b->kind == MS_CUBIC) {
                return line_angle(a->p.x, a->p.y, b->p1.x, b->p1.y);
            }
            return 0.0f;
        case MS_LINE:
            switch (b->kind) {
            case MS_LINE:
                pv = prev_vertex(s, n, idx);
                return calc_angle(pv.x, pv.y, a->p.x, a->p.y, a->p.x, a->p.y, b->p.x, b->p.y);
            case MS_CUBIC:
                pv = prev_vertex(s, n, idx);
                return curves_angle(pv.x, pv.y, pv.x, pv.y, a->p.x, a->p.y, b->p1.x, b->p1.y, b->p.x, b->p.y);
            case MS_MOVE:
                pv = prev_vertex(s, n, idx);
                return line_angle(pv.x, pv.y, a->p.x, a->p.y);
            case MS_CLOSE:
            default:
                pv = prev_vertex(s, n, idx);
                nx = subpath_start(s, n, idx);
                return calc_angle(pv.x, pv.y, a->p.x, a->p.y, a->p.x, a->p.y, nx.x, nx.y);
            }
        case MS_CUBIC:
            switch (b->kind) {
            case MS_CUBIC:
                pv = prev_vertex(s, n, idx);
                return curves_angle(pv.x, pv.y, a->p2.x, a->p2.y, a->p.x, a->p.y, b->p1.x, b->p1.y, b->p.x,
                                    b->p.y);
            case MS_LINE:
                pv = prev_vertex(s, n, idx);
                return curves_angle(pv.x, pv.y, a->p2.x, a->p2.y, a->p.x, a->p.y, b->p.x, b->p.y, b->p.x,
                                    b->p.y);
            case MS_MOVE:
                if (eq4(a->p.x, a->p2.x) && eq4(a->p.y, a->p2.y)) {
                    pv = prev_vertex(s, n, idx);
                    return line_angle(pv.x, pv.y, a->p.x, a->p.y);
                }
                return line_angle(a->p2.x, a->p2.y, a->p.x, a->p.y);
            case MS_CLOSE:
            default:
                pv = prev_vertex(s, n, idx);
                nx = subpath_start(s, n, idx);
                return line_angle(pv.x, pv.y, nx.x, nx.y);
            }
        case MS_CLOSE:
        default:
            return 0.0f;
        }
    }
}

/* ---------------------------------------------------------------- one marker kind */

typedef enum { ORIENT_AUTO, ORIENT_AUTO_START_REVERSE, ORIENT_ANGLE } orient_kind;

/* svgtypes::Angle::from_str, in degrees. */
static bool parse_angle(const char *v, double *deg)
{
    sv_stream s;
    double num;
    ss_init(&s, v);
    ss_skip_spaces(&s);
    if (!ss_parse_number(&s, &num)) {
        return false;
    }
    if (!ss_at_end(&s)) {
        size_t rest = s.len - s.pos;
        const char *p = s.s + s.pos;
        if (rest >= 3 && memcmp(p, "deg", 3) == 0) {
            s.pos += 3;
        } else if (rest >= 4 && memcmp(p, "grad", 4) == 0) {
            s.pos += 4;
            num = num * 180.0 / 200.0;
        } else if (rest >= 3 && memcmp(p, "rad", 3) == 0) {
            s.pos += 3;
            num = num * 57.2957795130823208767981548141051703;
        } else if (rest >= 4 && memcmp(p, "turn", 4) == 0) {
            s.pos += 4;
            num = num * 360.0;
        }
    }
    if (!ss_at_end(&s)) {
        return false;
    }
    *deg = num;
    return true;
}

static sv_ts from_rotate(float angle)
{
    float v = angle * (PI_F / 180.0f);
    float a = cosf(v);
    float b = sinf(v);
    return ts_row(a, b, -b, a, 0.0f, 0.0f);
}

typedef struct mctx {
    sv_conv *c;
    const st_node *marker;
    const sv_cstate *st;
    sv_unode *parent;
    const mseg *segs;
    size_t nsegs;
    orient_kind orient;
    float angle;
    bool has_vb;
    sv_rect vb;
    sv_aspect aspect;
    sv_rect r;
    float stroke_scale;
    sv_clip *clip;
} mctx;

static void draw_marker(mctx *m, sv_pt p, size_t idx)
{
    sv_conv *c = m->c;
    sv_ts ts = ts_translate(p.x, p.y);
    float angle;
    sv_unode *g;
    sv_cstate ms;
    sv_mlink link;
    if (c->marker_instances >= MAX_MARKER_INSTANCES) {
        return;
    }
    c->marker_instances++;
    if (m->orient == ORIENT_AUTO_START_REVERSE && idx == 0) {
        angle = fmodf(vertex_angle(m->segs, m->nsegs, idx) + 180.0f, 360.0f);
    } else if (m->orient == ORIENT_ANGLE) {
        angle = m->angle;
    } else {
        angle = vertex_angle(m->segs, m->nsegs, idx);
    }
    if (!sv_approx_eq_ulps(angle, 0.0f, 4)) {
        ts = ts_pre_concat(ts, from_rotate(angle));
    }
    if (m->has_vb) {
        float w = rect_w(m->r) * m->stroke_scale;
        float h = rect_h(m->r) * m->stroke_scale;
        float sx;
        float sy;
        if (!(w > 0.0f && h > 0.0f && isfinite(w) && isfinite(h))) {
            return; /* Size::from_wh(..).unwrap() would panic in the reference */
        }
        ts_get_scale(sv_viewbox_transform(m->vb, m->aspect, w, h), &sx, &sy);
        ts = ts_pre_concat(ts, ts_scale(sx, sy));
    } else {
        ts = ts_pre_concat(ts, ts_scale(m->stroke_scale, m->stroke_scale));
    }
    ts = ts_pre_translate(ts, -m->r.l, -m->r.t);
    g = sv_group_empty(c->a);
    if (g == NULL) {
        c->nomem = true;
        return;
    }
    g->transform = ts;
    g->abs_transform = ts_pre_concat(m->parent->abs_transform, ts);
    g->clip = m->clip;
    ms = *m->st;
    link.marker = m->marker;
    link.next = m->st->markers;
    ms.markers = &link;
    c->depth++;
    if (c->depth <= SV_MAX_CONV_DEPTH) {
        sv_convert_children(c, m->marker, &ms, g);
    }
    c->depth--;
    (void)sv_group_bboxes(g);
    if (g->nchildren > 0 && !sv_push_child(c->a, m->parent, g)) {
        c->nomem = true;
    }
}

static void draw_markers(mctx *m, int kind)
{
    const mseg *s = m->segs;
    size_t n = m->nsegs;
    if (n == 0) {
        return;
    }
    if (kind == 0) {
        if (s[0].kind == MS_MOVE) {
            draw_marker(m, s[0].p, 0);
        }
    } else if (kind == 1) {
        size_t i;
        for (i = 1; i + 1 < n && !m->c->nomem; i++) {
            if (s[i].kind != MS_CLOSE) {
                draw_marker(m, s[i].p, i);
            }
        }
    } else {
        size_t idx = n - 1;
        if (s[idx].kind == MS_LINE || s[idx].kind == MS_CUBIC) {
            draw_marker(m, s[idx].p, idx);
        } else if (s[idx].kind == MS_CLOSE) {
            draw_marker(m, subpath_start(s, n, idx), idx);
        }
    }
}

/* marker::resolve for one kind (0 start, 1 mid, 2 end). */
static void resolve(sv_conv *c, const st_node *shape, const mseg *segs, size_t nsegs, const st_node *marker,
                    int kind, const sv_cstate *st, sv_unode *parent)
{
    mctx m;
    const char *units = st_attr(marker, A_MARKER_UNITS);
    const char *ov = st_attr(marker, A_OVERFLOW);
    const char *orient = st_attr(marker, A_ORIENT);
    memset(&m, 0, sizeof m);
    m.c = c;
    m.marker = marker;
    m.st = st;
    m.parent = parent;
    m.segs = segs;
    m.nsegs = nsegs;
    if (units != NULL && strcmp(units, "userSpaceOnUse") == 0) {
        m.stroke_scale = 1.0f;
    } else {
        /* resolve_valid_length(stroke-width, 1) */
        const st_node *wn = st_find_attr_node(shape, A_STROKE_WIDTH);
        sv_length l;
        m.stroke_scale = 1.0f;
        if (wn != NULL && sv_parse_length(st_attr(wn, A_STROKE_WIDTH), &l)) {
            m.stroke_scale = sv_convert_length(c, l, wn, A_STROKE_WIDTH, UNITS_USER, st);
        }
        if (!(m.stroke_scale > 0.0f && isfinite(m.stroke_scale))) {
            return;
        }
    }
    if (!nzrect_from_xywh(sv_user_length(c, marker, A_REF_X, st, sv_len_px(0.0)),
                          sv_user_length(c, marker, A_REF_Y, st, sv_len_px(0.0)),
                          sv_user_length(c, marker, A_MARKER_WIDTH, st, sv_len_px(3.0)),
                          sv_user_length(c, marker, A_MARKER_HEIGHT, st, sv_len_px(3.0)), &m.r)) {
        return;
    }
    m.has_vb = sv_viewbox_of(marker, &m.vb);
    m.aspect = sv_aspect_of(marker);
    if (ov == NULL || strcmp(ov, "hidden") == 0 || strcmp(ov, "scroll") == 0) {
        sv_rect cr;
        sv_clip *clip = sv_alloc(c->a, sizeof *clip);
        sv_unode *root = sv_group_empty(c->a);
        sv_fill *fill = sv_alloc(c->a, sizeof *fill);
        const sv_path *data;
        sv_unode *p;
        if (m.has_vb) {
            cr = m.vb;
        } else if (!rect_from_xywh(0.0f, 0.0f, rect_w(m.r), rect_h(m.r), &cr)) {
            return;
        }
        data = path_from_rect(c->a, cr);
        if (clip == NULL || root == NULL || fill == NULL || data == NULL) {
            c->nomem = true;
            return;
        }
        fill->paint.kind = PAINT_COLOR;
        fill->opacity = 1.0f;
        p = sv_new_path(c, "", true, fill, NULL, false, data, ts_identity());
        if (p == NULL) {
            return;
        }
        if (!sv_push_child(c->a, root, p)) {
            c->nomem = true;
            return;
        }
        (void)sv_group_bboxes(root);
        clip->transform = ts_identity();
        clip->root = root;
        m.clip = clip;
    }
    if (orient != NULL && strcmp(orient, "auto") == 0) {
        m.orient = ORIENT_AUTO;
    } else if (orient != NULL && strcmp(orient, "auto-start-reverse") == 0) {
        m.orient = ORIENT_AUTO_START_REVERSE;
    } else {
        double deg = 0.0;
        m.orient = ORIENT_ANGLE;
        if (orient == NULL || !parse_angle(orient, &deg)) {
            deg = 0.0;
        }
        m.angle = (float)deg;
    }
    draw_markers(&m, kind);
}

void sv_marker_convert(sv_conv *c, const st_node *n, const sv_path *path, const sv_cstate *st,
                       sv_unode *parent)
{
    static const sv_aid AIDS[3] = {A_MARKER_START, A_MARKER_MID, A_MARKER_END};
    mseg *segs;
    size_t nsegs = 0;
    sv_piter it;
    sv_pseg seg;
    sv_pt prev = {0.0f, 0.0f};
    sv_pt prev_move = {0.0f, 0.0f};
    int k;
    segs = sv_alloc_array(c->a, path->nverbs > 0 ? path->nverbs : 1, sizeof *segs);
    if (segs == NULL) {
        c->nomem = true;
        return;
    }
    piter_init(&it, path);
    while (piter_next(&it, &seg) && nsegs < path->nverbs) {
        mseg *m = &segs[nsegs++];
        memset(m, 0, sizeof *m);
        switch (seg.verb) {
        case V_MOVE:
            m->kind = MS_MOVE;
            m->p = seg.p[0];
            prev = prev_move = seg.p[0];
            break;
        case V_LINE:
            m->kind = MS_LINE;
            m->p = seg.p[0];
            prev = seg.p[0];
            break;
        case V_QUAD:
            /* quad_to_curve */
            m->kind = MS_CUBIC;
            m->p1.x = (prev.x + seg.p[0].x * 2.0f) / 3.0f;
            m->p1.y = (prev.y + seg.p[0].y * 2.0f) / 3.0f;
            m->p2.x = (seg.p[1].x + seg.p[0].x * 2.0f) / 3.0f;
            m->p2.y = (seg.p[1].y + seg.p[0].y * 2.0f) / 3.0f;
            m->p = seg.p[1];
            prev = seg.p[1];
            break;
        case V_CUBIC:
            m->kind = MS_CUBIC;
            m->p1 = seg.p[0];
            m->p2 = seg.p[1];
            m->p = seg.p[2];
            prev = seg.p[2];
            break;
        case V_CLOSE:
        default:
            m->kind = MS_CLOSE;
            prev = prev_move;
            break;
        }
    }
    for (k = 0; k < 3 && !c->nomem; k++) {
        const st_node *link = marker_link(c, n, AIDS[k]);
        const sv_mlink *l;
        bool recursive = false;
        if (link == NULL || link->eid != E_MARKER) {
            continue;
        }
        for (l = st->markers; l != NULL; l = l->next) {
            if (l->marker == link) {
                recursive = true;
            }
        }
        if (recursive) {
            continue;
        }
        resolve(c, n, segs, nsegs, link, k, st, parent);
    }
}
