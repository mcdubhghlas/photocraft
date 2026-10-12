/* svgtree -> render tree (port of usvg 0.45's parser/{converter,shapes,units,use_node,switch}.rs
 * and the bounding boxes of tree/mod.rs). Groups that add nothing are flattened exactly where
 * usvg flattens them, so the layer structure svg.c builds matches the reference's. Paints, clip
 * paths, masks, filters and images are in svg_paint.c. */
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <stdio.h>

#include "base/compress.h"
#include "base/utf8.h"
#include "io/svg_internal.h"

#define DPI 96.0f
#define FONT_SIZE 12.0f

/* ---------------------------------------------------------------- small helpers */

sv_unode *sv_group_empty(sv_arena *a)
{
    sv_unode *g = sv_alloc(a, sizeof *g);
    if (g == NULL) {
        return NULL;
    }
    g->kind = UN_GROUP;
    g->id = "";
    g->transform = ts_identity();
    g->abs_transform = ts_identity();
    g->opacity = 1.0f;
    g->blend = BM_NORMAL;
    g->layer_bbox.l = 0.0f;
    g->layer_bbox.t = 0.0f;
    g->layer_bbox.r = 1.0f;
    g->layer_bbox.b = 1.0f;
    g->abs_layer_bbox = g->layer_bbox;
    return g;
}

bool sv_push_child(sv_arena *a, sv_unode *g, sv_unode *c)
{
    void *v = g->children;
    if (!sv_grow(a, &v, &g->capchildren, g->nchildren + 1, sizeof(sv_unode *))) {
        return false;
    }
    g->children = v;
    g->children[g->nchildren++] = c;
    return true;
}

static void push(sv_conv *c, sv_unode *g, sv_unode *n)
{
    if (!sv_push_child(c->a, g, n)) {
        c->nomem = true;
    }
}

/* ---------------------------------------------------------------- groups */

typedef enum {
    COL_ELEMENT,
    COL_CLIP_ELEMENT,
    COL_SWITCH,
    COL_USE_CHILDREN,
    COL_CHILDREN,
    COL_CLIP_CHILDREN
} col_kind;

typedef struct collect {
    col_kind kind;
    sv_eid tag;
    const st_node *node;
    const sv_cstate *st;
    /* COL_USE_CHILDREN */
    sv_ts transform;
    bool is_context;
} collect;

static void convert_element(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent);
static void convert_element_impl(sv_conv *c, sv_eid tag, const st_node *n, const sv_cstate *st, sv_unode *g);
static void convert_clip_element_impl(sv_conv *c, sv_eid tag, const st_node *n, const sv_cstate *st,
                                      sv_unode *g);
static void use_convert_children(sv_conv *c, const st_node *n, sv_ts transform, const sv_cstate *st,
                                 bool is_context, sv_unode *parent);

static void run_collect(sv_conv *c, const collect *k, sv_unode *g)
{
    switch (k->kind) {
    case COL_ELEMENT:
        convert_element_impl(c, k->tag, k->node, k->st, g);
        break;
    case COL_CLIP_ELEMENT:
        convert_clip_element_impl(c, k->tag, k->node, k->st, g);
        break;
    case COL_SWITCH:
        convert_element(c, k->node, k->st, g);
        break;
    case COL_USE_CHILDREN:
        use_convert_children(c, k->node, k->transform, k->st, k->is_context, g);
        break;
    case COL_CLIP_CHILDREN:
        sv_convert_clip_children(c, k->node, k->st, g);
        break;
    case COL_CHILDREN:
    default:
        sv_convert_children(c, k->node, k->st, g);
        break;
    }
}

/* converter::convert_group: the group, or NULL when it adds nothing (its children then went into
 * parent) or when an invalid clip path, mask or filter drops the element. */
static sv_unode *convert_group(sv_conv *c, const st_node *n, const sv_cstate *st, bool force,
                               sv_unode *parent, const collect *k)
{
    float opacity = st->parent_clip_path == NULL ? sv_attr_opacity(n, A_OPACITY) : 1.0f;
    sv_ts transform = sv_resolve_transform(c, n, A_TRANSFORM, st);
    sv_blend blend = sv_blend_of(n);
    const char *iso = st_attr(n, A_ISOLATION);
    bool isolate = iso != NULL && strcmp(iso, "isolate") == 0;
    bool is_g_or_use = n->eid == E_G || n->eid == E_USE;
    sv_unode *g = sv_group_empty(c->a);
    sv_rect obb;
    bool has_obb;
    sv_clip *clip = NULL;
    sv_mask *mask = NULL;
    sv_filter **filters = NULL;
    size_t nfilters = 0;
    st_node *link;
    bool required;
    size_t i;
    if (g == NULL) {
        c->nomem = true;
        return NULL;
    }
    g->id = is_g_or_use && st->markers == NULL ? st_id(n) : "";
    g->transform = transform;
    g->abs_transform = ts_pre_concat(parent->abs_transform, transform);
    g->opacity = opacity;
    g->blend = blend;
    g->isolate = isolate;
    c->depth++;
    if (c->depth <= SV_MAX_CONV_DEPTH) {
        run_collect(c, k, g);
    }
    c->depth--;
    has_obb = sv_object_bbox(g, &obb);
    link = st_node_attr(c->doc, n, A_CLIP_PATH);
    if (link != NULL) {
        clip = sv_convert_clip(c, link, st, has_obb ? &obb : NULL);
        if (clip == NULL) {
            return NULL;
        }
    }
    if (st->parent_clip_path == NULL) {
        link = st_node_attr(c->doc, n, A_MASK);
        if (link != NULL) {
            mask = sv_convert_mask(c, link, st, has_obb ? &obb : NULL);
            if (mask == NULL) {
                return NULL;
            }
        }
        {
            const char *f = st_attr(n, A_FILTER);
            if (f != NULL && strcmp(f, "none") != 0 &&
                !sv_convert_filters(c, n, st, has_obb ? &obb : NULL, &filters, &nfilters)) {
                return NULL;
            }
        }
    }
    required = !sv_approx_eq_ulps(opacity, 1.0f, 4) || clip != NULL || mask != NULL || nfilters > 0 ||
               !ts_is_identity(transform) || blend != BM_NORMAL || isolate || is_g_or_use || force;
    if (!required) {
        for (i = 0; i < g->nchildren; i++) {
            push(c, parent, g->children[i]);
        }
        return NULL;
    }
    g->clip = clip;
    g->mask = mask;
    g->filters = filters;
    g->nfilters = nfilters;
    (void)sv_group_bboxes(g);
    return g;
}

void sv_convert_children(sv_conv *c, const st_node *node, const sv_cstate *st, sv_unode *parent)
{
    const st_node *ch;
    for (ch = node->first; ch != NULL; ch = ch->next) {
        convert_element(c, ch, st, parent);
    }
}

/* ---------------------------------------------------------------- shapes */

typedef struct pbctx {
    sv_pb *b;
} pbctx;

static bool seg_cb(void *ctx, const sv_seg *s)
{
    sv_pb *b = ctx;
    switch (s->kind) {
    case SEG_MOVE:
        pb_move_to(b, (float)s->x, (float)s->y);
        break;
    case SEG_LINE:
        pb_line_to(b, (float)s->x, (float)s->y);
        break;
    case SEG_QUAD:
        pb_quad_to(b, (float)s->x1, (float)s->y1, (float)s->x, (float)s->y);
        break;
    case SEG_CURVE:
        pb_cubic_to(b, (float)s->x1, (float)s->y1, (float)s->x2, (float)s->y2, (float)s->x, (float)s->y);
        break;
    case SEG_CLOSE:
    default:
        pb_close(b);
        break;
    }
    return !b->failed;
}

static bool cubic_cb(void *ctx, const double v[6])
{
    sv_pb *b = ctx;
    pb_cubic_to(b, (float)v[0], (float)v[1], (float)v[2], (float)v[3], (float)v[4], (float)v[5]);
    return !b->failed;
}

/* PathBuilderExt::arc_to (f32 in, kurbo in f64, cubics back in f32). */
static void pb_arc_to(sv_pb *b, float rx, float ry, float x, float y)
{
    sv_pt prev;
    bool ok;
    if (!pb_last_point(b, &prev)) {
        return;
    }
    if (!sv_arc_to_cubics((double)prev.x, (double)prev.y, (double)rx, (double)ry, 0.0, false, true, (double)x,
                          (double)y, cubic_cb, b, &ok)) {
        pb_line_to(b, x, y);
    }
}

static const sv_path *ellipse_path(sv_conv *c, float cx, float cy, float rx, float ry)
{
    sv_pb b;
    pb_init(&b, c->a);
    pb_move_to(&b, cx + rx, cy);
    pb_arc_to(&b, rx, ry, cx, cy + ry);
    pb_arc_to(&b, rx, ry, cx - rx, cy);
    pb_arc_to(&b, rx, ry, cx, cy - ry);
    pb_arc_to(&b, rx, ry, cx + rx, cy);
    pb_close(&b);
    return pb_finish(&b);
}

static void resolve_rx_ry(sv_conv *c, const st_node *n, const sv_cstate *st, float *rx, float *ry)
{
    sv_length lx;
    sv_length ly;
    const char *vx = st_attr(n, A_RX);
    const char *vy = st_attr(n, A_RY);
    bool hx = vx != NULL && sv_parse_length(vx, &lx) && !signbit(lx.number);
    bool hy = vy != NULL && sv_parse_length(vy, &ly) && !signbit(ly.number);
    if (!hx && !hy) {
        *rx = 0.0f;
        *ry = 0.0f;
    } else if (hx && !hy) {
        *rx = sv_convert_length(c, lx, n, A_RX, UNITS_USER, st);
        *ry = *rx;
    } else if (!hx && hy) {
        *ry = sv_convert_length(c, ly, n, A_RY, UNITS_USER, st);
        *rx = *ry;
    } else {
        *rx = sv_convert_length(c, lx, n, A_RX, UNITS_USER, st);
        *ry = sv_convert_length(c, ly, n, A_RY, UNITS_USER, st);
    }
}

static const sv_path *points_path(sv_conv *c, const st_node *n, bool close)
{
    const char *v = st_attr(n, A_POINTS);
    sv_stream s;
    sv_pb b;
    if (v == NULL) {
        return NULL;
    }
    pb_init(&b, c->a);
    ss_init(&s, v);
    while (!ss_at_end(&s)) {
        double x;
        double y;
        if (!ss_parse_list_number(&s, &x) || !ss_parse_list_number(&s, &y)) {
            break;
        }
        if (b.nverbs == 0) {
            pb_move_to(&b, (float)x, (float)y);
        } else {
            pb_line_to(&b, (float)x, (float)y);
        }
    }
    if (b.nverbs < 2) {
        return NULL;
    }
    if (close) {
        pb_close(&b);
    }
    return pb_finish(&b);
}

static const sv_path *shape_path(sv_conv *c, const st_node *n, const sv_cstate *st)
{
    sv_length zero = sv_len_px(0.0);
    switch (n->eid) {
    case E_PATH: {
        const char *d = st_attr(n, A_D);
        sv_pb b;
        if (d == NULL) {
            return NULL;
        }
        pb_init(&b, c->a);
        (void)sv_simplify_path(d, seg_cb, &b);
        return pb_finish(&b);
    }
    case E_RECT: {
        float w = sv_user_length(c, n, A_WIDTH, st, zero);
        float h = sv_user_length(c, n, A_HEIGHT, st, zero);
        float x;
        float y;
        float rx;
        float ry;
        sv_rect r;
        sv_pb b;
        if (!sv_valid_length(w) || !sv_valid_length(h)) {
            return NULL;
        }
        x = sv_user_length(c, n, A_X, st, zero);
        y = sv_user_length(c, n, A_Y, st, zero);
        resolve_rx_ry(c, n, st, &rx, &ry);
        if (rx > w / 2.0f) {
            rx = w / 2.0f;
        }
        if (ry > h / 2.0f) {
            ry = h / 2.0f;
        }
        if (sv_approx_eq_ulps(rx, 0.0f, 4)) {
            if (!rect_from_xywh(x, y, w, h, &r)) {
                return NULL;
            }
            return path_from_rect(c->a, r);
        }
        pb_init(&b, c->a);
        pb_move_to(&b, x + rx, y);
        pb_line_to(&b, x + w - rx, y);
        pb_arc_to(&b, rx, ry, x + w, y + ry);
        pb_line_to(&b, x + w, y + h - ry);
        pb_arc_to(&b, rx, ry, x + w - rx, y + h);
        pb_line_to(&b, x + rx, y + h);
        pb_arc_to(&b, rx, ry, x, y + h - ry);
        pb_line_to(&b, x, y + ry);
        pb_arc_to(&b, rx, ry, x + rx, y);
        pb_close(&b);
        return pb_finish(&b);
    }
    case E_CIRCLE: {
        float cx = sv_user_length(c, n, A_CX, st, zero);
        float cy = sv_user_length(c, n, A_CY, st, zero);
        float r = sv_user_length(c, n, A_R, st, zero);
        if (!sv_valid_length(r)) {
            return NULL;
        }
        return ellipse_path(c, cx, cy, r, r);
    }
    case E_ELLIPSE: {
        float cx = sv_user_length(c, n, A_CX, st, zero);
        float cy = sv_user_length(c, n, A_CY, st, zero);
        float rx;
        float ry;
        resolve_rx_ry(c, n, st, &rx, &ry);
        if (!sv_valid_length(rx) || !sv_valid_length(ry)) {
            return NULL;
        }
        return ellipse_path(c, cx, cy, rx, ry);
    }
    case E_LINE: {
        sv_pb b;
        float x1 = sv_user_length(c, n, A_X1, st, zero);
        float y1 = sv_user_length(c, n, A_Y1, st, zero);
        float x2 = sv_user_length(c, n, A_X2, st, zero);
        float y2 = sv_user_length(c, n, A_Y2, st, zero);
        pb_init(&b, c->a);
        pb_move_to(&b, x1, y1);
        pb_line_to(&b, x2, y2);
        return pb_finish(&b);
    }
    case E_POLYLINE:
        return points_path(c, n, false);
    case E_POLYGON:
        return points_path(c, n, true);
    default:
        return NULL;
    }
}

/* svgtypes::PaintOrder of the nearest paint-order: 0 fill, 1 stroke, 2 markers (default 0 1 2). */
static void paint_order(const st_node *n, int out[3])
{
    static const char *const K[3] = {"fill", "stroke", "markers"};
    const st_node *p = st_find_attr_node(n, A_PAINT_ORDER);
    const char *v = p != NULL ? st_attr(p, A_PAINT_ORDER) : NULL;
    int order[3];
    int count = 0;
    bool left[3] = {true, true, true};
    sv_stream s;
    out[0] = 0;
    out[1] = 1;
    out[2] = 2;
    if (v == NULL) {
        return;
    }
    ss_init(&s, v);
    while (!ss_at_end(&s) && count < 3) {
        size_t s0;
        size_t len;
        int k;
        int found = -1;
        ss_skip_spaces(&s);
        s0 = s.pos;
        while (!ss_at_end(&s) &&
               ((s.s[s.pos] >= 'a' && s.s[s.pos] <= 'z') || (s.s[s.pos] >= 'A' && s.s[s.pos] <= 'Z') ||
                (s.s[s.pos] >= '0' && s.s[s.pos] <= '9') || s.s[s.pos] == '-' || s.s[s.pos] == '_')) {
            s.pos++;
        }
        len = s.pos - s0;
        ss_skip_spaces(&s);
        if (len == 6 && memcmp(s.s + s0, "normal", 6) == 0) {
            return;
        }
        for (k = 0; k < 3; k++) {
            if (strlen(K[k]) == len && memcmp(s.s + s0, K[k], len) == 0) {
                found = k;
            }
        }
        if (found < 0) {
            return;
        }
        left[found] = false;
        order[count++] = found;
    }
    ss_skip_spaces(&s);
    if (!ss_at_end(&s) || count == 0) {
        return;
    }
    {
        int k;
        for (k = 0; k < 3 && count < 3; k++) {
            if (left[k]) {
                order[count++] = k;
                left[k] = false;
            }
        }
    }
    if (order[0] == order[1] || order[0] == order[2] || order[1] == order[2]) {
        return;
    }
    memcpy(out, order, sizeof order);
}

/* svg_paint_order_to_usvg: stroke before fill (shared with svg_text.c). */
bool sv_stroke_first(const st_node *n)
{
    int o[3];
    paint_order(n, o);
    return o[0] == 1 || (o[0] == 2 && o[1] == 1);
}

/* A copy of path p with only its fill (stroke false) or only its stroke, without an id
 * (append_single_paint_path); NULL when it has no such paint. */
static sv_unode *single_paint(sv_conv *c, const sv_unode *p, bool stroke)
{
    sv_unode *q;
    if (stroke ? p->stroke == NULL : p->fill == NULL) {
        return NULL;
    }
    q = sv_alloc(c->a, sizeof *q);
    if (q == NULL) {
        c->nomem = true;
        return NULL;
    }
    *q = *p;
    q->id = "";
    if (stroke) {
        q->fill = NULL;
    } else {
        q->stroke = NULL;
    }
    return q;
}

/* Resolves a paint that came from a marker's path (context-fill / -stroke there). */
static void path_context(sv_conv *c, sv_paint *paint, sv_ctx *ctx, const sv_cstate *st, sv_ts path_ts,
                         sv_rect bounds)
{
    if (*ctx == CTX_PATH) {
        /* the reference ignores whether this succeeds */
        (void)sv_process_paint(c, paint, true, st->ctx_path_ts,
                               st->ctx_path_has_bbox ? &st->ctx_path_bbox : NULL, path_ts, bounds);
        *ctx = CTX_NONE;
    }
}

static void convert_path(sv_conv *c, const st_node *n, const sv_path *path, const sv_cstate *st,
                         sv_unode *parent)
{
    bool has_bbox;
    sv_fill *fill;
    sv_stroke *stroke;
    const st_node *vn;
    const char *vis;
    bool attr_visible;
    bool visible;
    sv_unode *p;
    sv_unode *markers = NULL;
    int order[3];
    if (path->nverbs < 2) {
        return;
    }
    has_bbox = rect_w(path->bounds) > 0.0f && rect_h(path->bounds) > 0.0f;
    fill = sv_resolve_fill(c, n, has_bbox, st);
    stroke = sv_resolve_stroke(c, n, has_bbox, st);
    vn = st_find_attr_node(n, A_VISIBILITY);
    vis = vn != NULL ? st_attr(vn, A_VISIBILITY) : NULL;
    attr_visible = vis == NULL || strcmp(vis, "visible") == 0 ||
                   (strcmp(vis, "hidden") != 0 && strcmp(vis, "collapse") != 0);
    visible = attr_visible;
    paint_order(n, order);
    if (fill == NULL && stroke == NULL) {
        visible = false;
    }
    if (fill != NULL) {
        path_context(c, &fill->paint, &fill->context, st, parent->abs_transform, path->bounds);
    }
    if (stroke != NULL) {
        path_context(c, &stroke->paint, &stroke->context, st, parent->abs_transform, path->bounds);
    }
    if (attr_visible && sv_marker_is_valid(c, n)) {
        sv_cstate ms = *st;
        sv_rect tb;
        markers = sv_group_empty(c->a);
        if (markers == NULL) {
            c->nomem = true;
            return;
        }
        markers->abs_transform = parent->abs_transform;
        ms.has_context = true;
        ms.ctx_fill = NULL;
        ms.ctx_stroke = NULL;
        if (fill != NULL) {
            ms.ctx_fill = sv_alloc(c->a, sizeof *ms.ctx_fill);
            if (ms.ctx_fill == NULL) {
                c->nomem = true;
                return;
            }
            *ms.ctx_fill = *fill;
            ms.ctx_fill->context = CTX_PATH;
        }
        if (stroke != NULL) {
            ms.ctx_stroke = sv_alloc(c->a, sizeof *ms.ctx_stroke);
            if (ms.ctx_stroke == NULL) {
                c->nomem = true;
                return;
            }
            *ms.ctx_stroke = *stroke;
            ms.ctx_stroke->context = CTX_PATH;
        }
        ms.ctx_path_ts = parent->abs_transform;
        ms.ctx_path_has_bbox = path_tight_bounds(path, &tb) && rect_to_nz(tb, &ms.ctx_path_bbox);
        sv_marker_convert(c, n, path, &ms, markers);
        (void)sv_group_bboxes(markers);
    }
    p = sv_new_path(c, st->markers == NULL ? st_id(n) : "", visible, fill, stroke,
                    order[0] == 1 || (order[0] == 2 && order[1] == 1), path, parent->abs_transform);
    if (p == NULL) {
        return;
    }
    {
        /* shape-rendering: optimizeSpeed and crispEdges turn anti-aliasing off */
        const st_node *rn = st_find_attr_node(n, A_SHAPE_RENDERING);
        const char *rv = rn != NULL ? st_attr(rn, A_SHAPE_RENDERING) : NULL;
        p->crisp = rv != NULL && (strcmp(rv, "optimizeSpeed") == 0 || strcmp(rv, "crispEdges") == 0);
    }
    if (markers == NULL) {
        push(c, parent, p);
    } else if (order[0] == 2) {
        push(c, parent, markers);
        push(c, parent, p);
    } else if (order[1] == 2) {
        sv_unode *a = single_paint(c, p, order[0] == 1);
        sv_unode *b = single_paint(c, p, order[2] == 1);
        if (a != NULL) {
            push(c, parent, a);
        }
        push(c, parent, markers);
        if (b != NULL) {
            push(c, parent, b);
        }
    } else {
        push(c, parent, p);
        push(c, parent, markers);
    }
}

/* ---------------------------------------------------------------- use, svg, switch */

static sv_unode *clip_element(sv_conv *c, const st_node *n, sv_rect clip_rect, sv_ts transform)
{
    sv_unode *g = sv_group_empty(c->a);
    sv_clip *clip = sv_alloc(c->a, sizeof *clip);
    sv_unode *root = sv_group_empty(c->a);
    sv_fill *fill = sv_alloc(c->a, sizeof *fill);
    const sv_path *data = path_from_rect(c->a, clip_rect);
    sv_unode *p;
    if (g == NULL || clip == NULL || root == NULL || fill == NULL || data == NULL) {
        c->nomem = true;
        return NULL;
    }
    fill->paint.kind = PAINT_COLOR;
    fill->opacity = 1.0f;
    p = sv_new_path(c, "", true, fill, NULL, false, data, ts_identity());
    if (p == NULL) {
        c->nomem = true;
        return NULL;
    }
    push(c, root, p);
    clip->transform = ts_identity();
    clip->root = root;
    g->id = st_id(n);
    g->transform = transform;
    g->clip = clip;
    return g;
}

static void use_node_size(sv_conv *c, const st_node *n, const sv_cstate *st, float *w, float *h)
{
    *w = sv_user_length(c, n, A_WIDTH, st, sv_len_pct(100.0));
    *h = sv_user_length(c, n, A_HEIGHT, st, sv_len_pct(100.0));
}

static bool get_clip_rect(sv_conv *c, const st_node *use, const st_node *sym, const sv_cstate *st,
                          sv_rect *out)
{
    const char *ov = st_attr(sym, A_OVERFLOW);
    float x;
    float y;
    float w;
    float h;
    if (ov != NULL && (strcmp(ov, "visible") == 0 || strcmp(ov, "auto") == 0)) {
        return false;
    }
    if (use->eid == E_SVG && !st->use_w_set && !st->use_h_set &&
        !(st_has(use, A_WIDTH) && st_has(use, A_HEIGHT))) {
        return false;
    }
    x = sv_user_length(c, use, A_X, st, sv_len_px(0.0));
    y = sv_user_length(c, use, A_Y, st, sv_len_px(0.0));
    use_node_size(c, use, st, &w, &h);
    if (use->eid == E_SVG) {
        if (st->use_w_set) {
            w = st->use_w;
        }
        if (st->use_h_set) {
            h = st->use_h;
        }
    }
    if (!sv_valid_length(w) || !sv_valid_length(h)) {
        return false;
    }
    return nzrect_from_xywh(x, y, w, h, out);
}

static bool viewbox_transform_of(sv_conv *c, const st_node *n, const st_node *linked, const sv_cstate *st,
                                 sv_ts *out)
{
    float w;
    float h;
    sv_rect vb;
    use_node_size(c, n, st, &w, &h);
    if (n->eid == E_SVG) {
        if (st->use_w_set) {
            w = st->use_w;
        }
        if (st->use_h_set) {
            h = st->use_h;
        }
    }
    if (!sv_valid_length(w) || !sv_valid_length(h) || !sv_viewbox_of(linked, &vb)) {
        return false;
    }
    *out = sv_viewbox_transform(vb, sv_aspect_of(linked), w, h);
    return true;
}

static void use_convert_children(sv_conv *c, const st_node *n, sv_ts transform, const sv_cstate *st,
                                 bool is_context, sv_unode *parent)
{
    sv_ts old = parent->abs_transform;
    collect k;
    sv_unode *g;
    parent->abs_transform = ts_pre_concat(parent->abs_transform, transform);
    memset(&k, 0, sizeof k);
    k.kind = st->parent_clip_path != NULL ? COL_CLIP_CHILDREN : COL_CHILDREN;
    k.node = n;
    k.st = st;
    g = convert_group(c, n, st, !ts_is_identity(transform), parent, &k);
    if (g != NULL) {
        g->is_context = is_context;
        g->transform = transform;
        push(c, parent, g);
    }
    parent->abs_transform = old;
}

static void use_convert(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent)
{
    const st_node *child = n->first;
    sv_cstate us;
    sv_ts orig;
    sv_ts nts;
    bool symbol;
    if (child == NULL) {
        return;
    }
    if (st->parent_clip_path != NULL && child->eid == E_SYMBOL) {
        return;
    }
    us = *st;
    us.has_context = true;
    us.ctx_fill = sv_resolve_fill(c, n, true, st);
    us.ctx_stroke = sv_resolve_stroke(c, n, true, st);
    if (us.ctx_fill != NULL) {
        us.ctx_fill->context = CTX_USE;
    }
    if (us.ctx_stroke != NULL) {
        us.ctx_stroke->context = CTX_USE;
    }
    orig = sv_resolve_transform(c, n, A_TRANSFORM, st);
    nts = ts_pre_translate(ts_identity(), sv_user_length(c, n, A_X, &us, sv_len_px(0.0)),
                           sv_user_length(c, n, A_Y, &us, sv_len_px(0.0)));
    symbol = child->eid == E_SYMBOL;
    if (symbol) {
        sv_rect clip_rect;
        sv_ts vbt;
        float x = us.view_box.l;
        float y = us.view_box.t;
        float w =
            st_has(n, A_WIDTH) ? sv_user_length(c, n, A_WIDTH, &us, sv_len_pct(100.0)) : rect_w(us.view_box);
        float h = st_has(n, A_HEIGHT) ? sv_user_length(c, n, A_HEIGHT, &us, sv_len_pct(100.0))
                                      : rect_h(us.view_box);
        sv_rect vb;
        if (nzrect_from_xywh(x, y, w, h, &vb)) {
            us.view_box = vb;
        }
        if (viewbox_transform_of(c, n, child, &us, &vbt)) {
            nts = ts_pre_concat(nts, vbt);
        }
        if (get_clip_rect(c, n, child, &us, &clip_rect)) {
            sv_unode *g = clip_element(c, n, clip_rect, orig);
            sv_unode *g2;
            collect k;
            if (g == NULL) {
                return;
            }
            g->abs_transform = parent->abs_transform;
            memset(&k, 0, sizeof k);
            k.kind = COL_USE_CHILDREN;
            k.node = child;
            k.st = &us;
            k.transform = nts;
            k.is_context = false;
            g2 = convert_group(c, n, &us, true, g, &k);
            if (g2 != NULL) {
                g->is_context = true;
                g2->id = "";
                g2->transform = ts_identity();
                push(c, g, g2);
            }
            if (g->nchildren == 0) {
                return;
            }
            (void)sv_group_bboxes(g);
            push(c, parent, g);
            return;
        }
    }
    orig = ts_pre_concat(orig, nts);
    if (symbol) {
        collect k;
        sv_unode *g;
        memset(&k, 0, sizeof k);
        k.kind = COL_USE_CHILDREN;
        k.node = child;
        k.st = &us;
        k.transform = orig;
        k.is_context = false;
        g = convert_group(c, n, &us, false, parent, &k);
        if (g != NULL) {
            g->is_context = true;
            g->transform = ts_identity();
            push(c, parent, g);
        }
    } else {
        if (child->eid == E_SVG) {
            us.use_w_set = st_has(n, A_WIDTH);
            us.use_h_set = st_has(n, A_HEIGHT);
            if (us.use_w_set) {
                us.use_w = sv_user_length(c, n, A_WIDTH, &us, sv_len_pct(100.0));
            }
            if (us.use_h_set) {
                us.use_h = sv_user_length(c, n, A_HEIGHT, &us, sv_len_pct(100.0));
            }
        }
        use_convert_children(c, n, orig, &us, true, parent);
    }
}

static void convert_svg(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent)
{
    sv_ts orig = sv_resolve_transform(c, n, A_TRANSFORM, st);
    float x = sv_user_length(c, n, A_X, st, sv_len_px(0.0));
    float y = sv_user_length(c, n, A_Y, st, sv_len_px(0.0));
    sv_ts nts = ts_pre_translate(ts_identity(), x, y);
    sv_ts vbt;
    sv_cstate ns = *st;
    sv_rect clip_rect;
    if (viewbox_transform_of(c, n, n, st, &vbt)) {
        nts = ts_pre_concat(nts, vbt);
    }
    if (!sv_viewbox_of(n, &ns.view_box)) {
        float w;
        float h;
        sv_rect vb;
        use_node_size(c, n, st, &w, &h);
        if (st->use_w_set) {
            w = st->use_w;
        }
        if (st->use_h_set) {
            h = st->use_h;
        }
        ns.view_box = nzrect_from_xywh(x, y, w, h, &vb) ? vb : st->view_box;
    }
    if (get_clip_rect(c, n, n, st, &clip_rect)) {
        sv_unode *g = clip_element(c, n, clip_rect, orig);
        if (g == NULL) {
            return;
        }
        g->abs_transform = parent->abs_transform;
        use_convert_children(c, n, nts, &ns, false, g);
        (void)sv_group_bboxes(g);
        push(c, parent, g);
    } else {
        orig = ts_pre_concat(orig, nts);
        use_convert_children(c, n, orig, &ns, false, parent);
    }
}

static void switch_convert(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent)
{
    const st_node *child;
    collect k;
    sv_unode *g;
    for (child = n->first; child != NULL; child = child->next) {
        if (sv_condition_passed(child)) {
            break;
        }
    }
    if (child == NULL) {
        return;
    }
    memset(&k, 0, sizeof k);
    k.kind = COL_SWITCH;
    k.node = child;
    k.st = st;
    g = convert_group(c, n, st, false, parent, &k);
    if (g != NULL) {
        push(c, parent, g);
    }
}

/* ---------------------------------------------------------------- elements */

const sv_path *sv_shape_path(sv_conv *c, const st_node *n, const sv_cstate *st)
{
    return shape_path(c, n, st);
}

static void convert_element_impl(sv_conv *c, sv_eid tag, const st_node *n, const sv_cstate *st, sv_unode *g)
{
    switch (tag) {
    case E_RECT:
    case E_CIRCLE:
    case E_ELLIPSE:
    case E_LINE:
    case E_POLYLINE:
    case E_POLYGON:
    case E_PATH: {
        const sv_path *p = shape_path(c, n, st);
        if (p != NULL) {
            convert_path(c, n, p, st, g);
        } else if (c->a->nomem) {
            c->nomem = true;
        }
        break;
    }
    case E_IMAGE:
        sv_convert_image(c, n, st, g);
        break;
    case E_TEXT:
        sv_convert_text(c, n, st, g);
        break;
    case E_SVG:
        if (st_parent_element(n) != NULL) {
            convert_svg(c, n, st, g);
        } else {
            sv_convert_children(c, n, st, g);
        }
        break;
    case E_G:
        sv_convert_children(c, n, st, g);
        break;
    default:
        break;
    }
}

static void convert_element(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent)
{
    sv_eid tag = n->eid;
    collect k;
    sv_unode *g;
    if (n->kind != ST_ELEMENT) {
        return;
    }
    if (!sv_eid_is_graphic(tag) && tag != E_G && tag != E_SWITCH && tag != E_SVG) {
        return;
    }
    if (!sv_is_visible_element(n)) {
        return;
    }
    if (c->depth > SV_MAX_CONV_DEPTH) {
        return;
    }
    if (tag == E_USE) {
        c->depth++;
        use_convert(c, n, st, parent);
        c->depth--;
        return;
    }
    if (tag == E_SWITCH) {
        switch_convert(c, n, st, parent);
        return;
    }
    memset(&k, 0, sizeof k);
    k.kind = COL_ELEMENT;
    k.tag = tag;
    k.node = n;
    k.st = st;
    g = convert_group(c, n, st, false, parent, &k);
    if (g != NULL) {
        push(c, parent, g);
    }
}

void sv_convert_element(sv_conv *c, const st_node *n, const sv_cstate *st, sv_unode *parent)
{
    convert_element(c, n, st, parent);
}

static void convert_clip_element_impl(sv_conv *c, sv_eid tag, const st_node *n, const sv_cstate *st,
                                      sv_unode *g)
{
    switch (tag) {
    case E_RECT:
    case E_CIRCLE:
    case E_ELLIPSE:
    case E_POLYLINE:
    case E_POLYGON:
    case E_PATH: {
        const sv_path *p = shape_path(c, n, st);
        if (p != NULL) {
            convert_path(c, n, p, st, g);
        }
        break;
    }
    case E_TEXT:
        sv_convert_text(c, n, st, g);
        break;
    default:
        break;
    }
}

void sv_convert_clip_children(sv_conv *c, const st_node *node, const sv_cstate *st, sv_unode *parent)
{
    const st_node *ch;
    for (ch = node->first; ch != NULL; ch = ch->next) {
        collect k;
        sv_unode *g;
        if (ch->kind != ST_ELEMENT || !sv_eid_is_graphic(ch->eid) || !sv_is_visible_element(ch)) {
            continue;
        }
        if (ch->eid == E_USE) {
            if (c->depth <= SV_MAX_CONV_DEPTH) {
                c->depth++;
                use_convert(c, ch, st, parent);
                c->depth--;
            }
            continue;
        }
        memset(&k, 0, sizeof k);
        k.kind = COL_CLIP_ELEMENT;
        k.tag = ch->eid;
        k.node = ch;
        k.st = st;
        g = convert_group(c, ch, st, false, parent, &k);
        if (g != NULL) {
            push(c, parent, g);
        }
    }
}

/* ---------------------------------------------------------------- document */

static bool resolve_size(sv_conv *c, const st_node *svg, float *w, float *h, bool *restore)
{
    sv_cstate st;
    sv_length def = sv_len_pct(100.0);
    sv_length wl = def;
    sv_length hl = def;
    sv_rect vb;
    bool has_vb;
    const char *v;
    memset(&st, 0, sizeof st);
    (void)nzrect_from_xywh(0.0f, 0.0f, 100.0f, 100.0f, &st.view_box);
    v = st_attr(svg, A_WIDTH);
    if (v != NULL && !sv_parse_length(v, &wl)) {
        wl = def;
    }
    v = st_attr(svg, A_HEIGHT);
    if (v != NULL && !sv_parse_length(v, &hl)) {
        hl = def;
    }
    has_vb = sv_viewbox_of(svg, &vb);
    *restore = (wl.unit == U_PERCENT || hl.unit == U_PERCENT) && !has_vb;
    if (has_vb) {
        st.view_box = vb;
        *w = wl.unit == U_PERCENT ? rect_w(vb) * ((float)wl.number / 100.0f)
                                  : sv_user_length(c, svg, A_WIDTH, &st, def);
        *h = hl.unit == U_PERCENT ? rect_h(vb) * ((float)hl.number / 100.0f)
                                  : sv_user_length(c, svg, A_HEIGHT, &st, def);
    } else {
        *w = sv_user_length(c, svg, A_WIDTH, &st, def);
        *h = sv_user_length(c, svg, A_HEIGHT, &st, def);
    }
    return sv_valid_length(*w) && sv_valid_length(*h);
}

static bool convert_doc(sv_conv *c, const st_doc *d, sv_tree *out, char *msg, size_t cap)
{
    const st_node *svg = st_first_element(d->root);
    float w;
    float h;
    bool restore;
    sv_rect vb;
    sv_aspect aspect;
    sv_cstate st;
    sv_ts root_ts;
    sv_unode *root = sv_group_empty(c->a);
    const char *bgv;
    sv_spaint bg;
    bool has_bg = false;
    if (root == NULL) {
        c->nomem = true;
        return false;
    }
    if (!resolve_size(c, svg, &w, &h, &restore)) {
        (void)snprintf(msg, cap, "SVG has an invalid size");
        return false;
    }
    out->width = w;
    out->height = h;
    out->root = root;
    if (!sv_viewbox_of(svg, &vb)) {
        (void)nzrect_from_xywh(0.0f, 0.0f, w, h, &vb);
    }
    aspect = sv_aspect_of(svg);
    bgv = st_attr(svg, A_BACKGROUND_COLOR);
    has_bg = bgv != NULL && sv_parse_paint(bgv, &bg) && bg.kind == P_COLOR;
    if (!sv_is_visible_element(svg)) {
        return true;
    }
    memset(&st, 0, sizeof st);
    st.view_box = vb;
    root_ts = sv_viewbox_transform(vb, aspect, w, h);
    if (ts_is_identity(root_ts) && !has_bg) {
        sv_convert_children(c, d->root, &st, root);
    } else {
        sv_unode *g = sv_group_empty(c->a);
        if (g == NULL) {
            c->nomem = true;
            return false;
        }
        if (has_bg) {
            const sv_path *p = path_from_rect(c->a, vb);
            sv_fill *f = sv_alloc(c->a, sizeof *f);
            sv_unode *pn;
            if (p == NULL || f == NULL) {
                c->nomem = true;
                return false;
            }
            f->paint.kind = PAINT_COLOR;
            f->paint.r = bg.color.r;
            f->paint.g = bg.color.g;
            f->paint.b = bg.color.b;
            f->opacity = (float)bg.color.a / 255.0f;
            pn = sv_new_path(c, "", true, f, NULL, false, p, ts_identity());
            if (pn != NULL) {
                push(c, g, pn);
            }
        }
        g->transform = root_ts;
        g->abs_transform = root_ts;
        sv_convert_children(c, d->root, &st, g);
        (void)sv_group_bboxes(g);
        push(c, root, g);
    }
    sv_update_paint_servers(c, root, ts_identity(), NULL);
    (void)sv_group_bboxes(root);
    if (restore) {
        sv_rect bb = root->abs_bbox;
        if (sv_valid_length(bb.r) && sv_valid_length(bb.b)) {
            out->width = bb.r;
            out->height = bb.b;
        }
    }
    return !c->nomem;
}

/* Parses and converts a drawing; parent is the conversion of the drawing that embeds this one as
 * an <image> (NULL for the document itself): a nested drawing gets no images of its own, as
 * usvg's load_sub_svg. */
static pc_status parse_impl(sv_arena *a, const uint8_t *bytes, size_t len, sv_tree *out, char *msg,
                            size_t cap, const sv_conv *parent)
{
    pc_buf inflated = PC_BUF_INIT;
    const uint8_t *data = bytes;
    size_t n = len;
    sx_doc xml;
    st_doc doc;
    sv_conv c;
    char xmsg[192];
    pc_status st;
    size_t i;
    msg[0] = '\0';
    if (len >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b) {
        /* .svgz; the inflated size is capped (the reference has no cap: a zip bomb). */
        pc_error e;
        if (pc_inflate(PC_Z_GZIP, bytes, len, (size_t)256 << 20, &inflated, NULL, &e) != PC_OK) {
            (void)snprintf(msg, cap, "provided data has a malformed GZip content");
            return PC_ERR_FORMAT;
        }
        data = inflated.data != NULL ? inflated.data : (const uint8_t *)"";
        n = inflated.len;
    }
    /* UTF-8 only; a NUL is not an XML character (roxmltree refuses it). */
    for (i = 0; i < n;) {
        uint32_t cp;
        if (data[i] == 0) {
            pc_buf_fini(&inflated);
            (void)snprintf(msg, cap, "SVG data parsing failed cause a non-XML character '\\u{0}' found");
            return PC_ERR_FORMAT;
        }
        if (data[i] < 0x80) {
            i++;
            continue;
        }
        if (!pc_utf8_next(data, n, &i, &cp)) {
            pc_buf_fini(&inflated);
            (void)snprintf(msg, cap, "provided data has not an UTF-8 encoding");
            return PC_ERR_FORMAT;
        }
    }
    st = sx_parse(a, (const char *)data, n, &xml, xmsg, sizeof xmsg);
    if (st == PC_OK) {
        st = st_build(a, &xml, &doc, xmsg, sizeof xmsg);
    }
    pc_buf_fini(&inflated);
    if (st == PC_ERR_NOMEM || a->nomem) {
        return PC_ERR_NOMEM;
    }
    if (st != PC_OK) {
        (void)snprintf(msg, cap, "SVG data parsing failed cause %s", xmsg);
        return PC_ERR_FORMAT;
    }
    memset(&c, 0, sizeof c);
    c.a = a;
    c.doc = &doc;
    if (parent != NULL) {
        c.no_images = true;
        c.marker_instances = parent->marker_instances;
        c.text_fonts = parent->text_fonts; /* usvg's load_sub_svg reuses the parent's fontdb */
        c.text_glyphs = parent->text_glyphs;
        c.text_work = parent->text_work;
    } else {
        c.text_fonts = len >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b;
        for (i = 0; i + 4 <= len && !c.text_fonts; i++) {
            c.text_fonts = memcmp(bytes + i, "text", 4) == 0;
        }
    }
    if (!convert_doc(&c, &doc, out, msg, cap)) {
        if (c.nomem || a->nomem) {
            return PC_ERR_NOMEM;
        }
        return PC_ERR_FORMAT;
    }
    return PC_OK;
}

pc_status sv_parse_tree(sv_arena *a, const uint8_t *bytes, size_t len, sv_tree *out, char *msg, size_t cap)
{
    return parse_impl(a, bytes, len, out, msg, cap, NULL);
}

bool sv_parse_subtree(sv_conv *c, const uint8_t *bytes, size_t len, sv_tree *out)
{
    char msg[200];
    pc_status st;
    if (c->no_images) {
        return false;
    }
    st = parse_impl(c->a, bytes, len, out, msg, sizeof msg, c);
    if (st == PC_ERR_NOMEM) {
        c->nomem = true;
    }
    return st == PC_OK;
}
