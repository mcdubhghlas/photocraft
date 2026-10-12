/* Paints, paint servers, clip paths, masks, filters and images of the render tree (port of usvg
 * 0.45's parser/{style,paint_server,clippath,mask,image}.rs); filters are in svg_filter_conv.c. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/base64.h"
#include "base/buf.h"
#include "codecs/codecs.h"
#include "io/svg_internal.h"

/* ---------------------------------------------------------------- caches */

static void *cache_get(const sv_centry *v, size_t n, const st_node *node)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (v[i].node == node) {
            return v[i].val;
        }
    }
    return NULL;
}

static void cache_put(sv_conv *c, sv_centry **v, size_t *n, size_t *cap, const st_node *node, void *val)
{
    void *p = *v;
    if (!sv_grow(c->a, &p, cap, *n + 1, sizeof(sv_centry))) {
        c->nomem = true;
        return;
    }
    *v = p;
    (*v)[*n].node = node;
    (*v)[*n].val = val;
    (*n)++;
}

/* ---------------------------------------------------------------- helpers */

static sv_length lpx(double v)
{
    sv_length l = {v, U_NONE};
    return l;
}

static sv_length lpct(double v)
{
    sv_length l = {v, U_PERCENT};
    return l;
}

static bool valid_len(float v)
{
    return v > 0.0f && isfinite(v);
}

static float opacity_mul(float a, float b)
{
    float x = a * b;
    return isnan(x) ? 0.0f : x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

/* find_attribute::<Color> (black when missing or unparsable). */
static sv_color current_color(const st_node *n)
{
    const st_node *p = st_find_attr_node(n, A_COLOR);
    sv_color c = {0, 0, 0, 255};
    if (p != NULL && !sv_parse_color(st_attr(p, A_COLOR), &c)) {
        sv_color black = {0, 0, 0, 255};
        c = black;
    }
    return c;
}

static float find_opacity(const st_node *n, sv_aid aid)
{
    const st_node *p = st_find_attr_node(n, aid);
    float o;
    if (p != NULL && sv_parse_opacity(st_attr(p, aid), &o)) {
        return o;
    }
    return 1.0f;
}

static const char *find_str(const st_node *n, sv_aid aid)
{
    const st_node *p = st_find_attr_node(n, aid);
    return p != NULL ? st_attr(p, aid) : NULL;
}

static bool units_of(const char *v, sv_units *out)
{
    if (v == NULL) {
        return false;
    }
    if (strcmp(v, "userSpaceOnUse") == 0) {
        *out = UNITS_USER;
        return true;
    }
    if (strcmp(v, "objectBoundingBox") == 0) {
        *out = UNITS_OBB;
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- href chains */

/* HrefIter: node, then each href target, stopping at a link back to the current or first. */
typedef struct href_iter {
    const st_doc *doc;
    const st_node *origin;
    const st_node *cur;
    bool first;
    bool done;
    size_t steps;
} href_iter;

static void hi_init(href_iter *it, const st_doc *d, const st_node *n)
{
    it->doc = d;
    it->origin = n;
    it->cur = n;
    it->first = true;
    it->done = false;
    it->steps = 0;
}

static const st_node *hi_next(href_iter *it)
{
    const st_node *link;
    if (it->done) {
        return NULL;
    }
    if (it->first) {
        it->first = false;
        return it->cur;
    }
    /* A chain longer than this loops (a -> b -> c -> b); the reference would never end. */
    if (++it->steps > 256) {
        it->done = true;
        return NULL;
    }
    link = st_node_attr(it->doc, it->cur, A_HREF);
    if (link == NULL) {
        it->done = true;
        return NULL;
    }
    if (link == it->cur || link == it->origin) {
        it->done = true;
        return NULL;
    }
    it->cur = link;
    return link;
}

static bool is_gradient(sv_eid e)
{
    return e == E_LINEAR_GRADIENT || e == E_RADIAL_GRADIENT;
}

static bool lg_attr_allowed(sv_aid name, sv_eid tag)
{
    switch (name) {
    case A_X1:
    case A_Y1:
    case A_X2:
    case A_Y2:
        return tag == E_LINEAR_GRADIENT;
    case A_GRADIENT_UNITS:
    case A_SPREAD_METHOD:
    case A_GRADIENT_TRANSFORM:
        return is_gradient(tag);
    default:
        return false;
    }
}

static bool rg_attr_allowed(sv_aid name, sv_eid tag)
{
    switch (name) {
    case A_CX:
    case A_CY:
    case A_R:
    case A_FX:
    case A_FY:
        return tag == E_RADIAL_GRADIENT;
    case A_GRADIENT_UNITS:
    case A_SPREAD_METHOD:
    case A_GRADIENT_TRANSFORM:
        return is_gradient(tag);
    default:
        return false;
    }
}

/* resolve_attr: the node in the href chain that holds name. */
static const st_node *resolve_attr(const st_doc *d, const st_node *n, sv_aid name)
{
    href_iter it;
    const st_node *l;
    if (st_has(n, name)) {
        return n;
    }
    hi_init(&it, d, n);
    while ((l = hi_next(&it)) != NULL) {
        bool ok;
        switch (n->eid) {
        case E_LINEAR_GRADIENT:
            ok = lg_attr_allowed(name, l->eid);
            break;
        case E_RADIAL_GRADIENT:
            ok = rg_attr_allowed(name, l->eid);
            break;
        case E_PATTERN:
        case E_FILTER:
            ok = l->eid == n->eid;
            break;
        default:
            return n;
        }
        if (!ok) {
            break;
        }
        if (st_has(l, name)) {
            return l;
        }
    }
    return n;
}

static float resolve_number(sv_conv *c, const st_node *n, sv_aid name, sv_units units, const sv_cstate *st,
                            sv_length def)
{
    return sv_node_length(c, resolve_attr(c->doc, n, name), name, units, st, def);
}

static sv_units convert_units(sv_conv *c, const st_node *n, sv_aid name, sv_units def)
{
    sv_units u;
    return units_of(st_attr(resolve_attr(c->doc, n, name), name), &u) ? u : def;
}

/* ---------------------------------------------------------------- stops */

static bool stop_eq_ulps(float a, float b)
{
    int32_t ia;
    int32_t ib;
    int64_t d;
    if (a == b) {
        return true;
    }
    if (signbit(a) != signbit(b) || isnan(a) || isnan(b)) {
        return false;
    }
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    d = (int64_t)ia - (int64_t)ib;
    return (d < 0 ? -d : d) <= 4;
}

static float clamp01f(float v)
{
    return isnan(v) ? 0.0f : v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static bool convert_stops(sv_conv *c, const st_node *grad, sv_stop **out, size_t *nout)
{
    sv_stop *v = NULL;
    size_t n = 0;
    size_t cap = 0;
    double prev = 0.0;
    const st_node *s;
    size_t i;
    for (s = grad->first; s != NULL; s = s->next) {
        const char *ov;
        const char *cv;
        sv_length off;
        double o;
        sv_color col = {0, 0, 0, 255};
        float so;
        void *p = v;
        if (s->eid != E_STOP) {
            continue;
        }
        ov = st_attr(s, A_OFFSET);
        if (ov == NULL || !sv_parse_length(ov, &off)) {
            off = lpx(prev);
        }
        o = off.unit == U_NONE ? off.number : off.unit == U_PERCENT ? off.number / 100.0 : prev;
        prev = o;
        cv = st_attr(s, A_STOP_COLOR);
        if (cv != NULL && strcmp(cv, "currentColor") == 0) {
            col = current_color(s);
        } else if (cv != NULL && !sv_parse_color(cv, &col)) {
            sv_color black = {0, 0, 0, 255};
            col = black;
        }
        if (!sv_parse_opacity(st_attr(s, A_STOP_OPACITY), &so)) {
            so = 1.0f;
        }
        if (!sv_grow(c->a, &p, &cap, n + 1, sizeof(sv_stop))) {
            c->nomem = true;
            return false;
        }
        v = p;
        {
            float of = (float)o;
            of = of > 1.0f ? 1.0f : of < 0.0f ? 0.0f : of;
            v[n].offset = clamp01f(of);
        }
        v[n].r = col.r;
        v[n].g = col.g;
        v[n].b = col.b;
        v[n].opacity = opacity_mul((float)col.a / 255.0f, so);
        n++;
    }
    if (n >= 3) {
        i = 0;
        while (i < n - 2) {
            if (stop_eq_ulps(v[i].offset, v[i + 1].offset) &&
                stop_eq_ulps(v[i + 1].offset, v[i + 2].offset)) {
                memmove(&v[i + 1], &v[i + 2], (n - i - 2) * sizeof *v);
                n--;
            } else {
                i++;
            }
        }
    }
    if (n >= 2) {
        for (i = 0; i + 1 < n; i++) {
            if (stop_eq_ulps(v[i].offset, 0.0f) && stop_eq_ulps(v[i + 1].offset, 0.0f)) {
                v[i + 1].offset = clamp01f(v[i].offset + 1.19209290e-7f);
            }
        }
    }
    for (i = 1; i < n; i++) {
        float o1 = v[i - 1].offset;
        float o2 = v[i].offset;
        if (o1 > o2 || stop_eq_ulps(o1, o2)) {
            v[i - 1].offset = clamp01f(o1 - 1.19209290e-7f);
            v[i].offset = clamp01f(o1);
        }
    }
    *out = v;
    *nout = n;
    return true;
}

static const st_node *gradient_with_stops(sv_conv *c, const st_node *n)
{
    href_iter it;
    const st_node *l;
    hi_init(&it, c->doc, n);
    while ((l = hi_next(&it)) != NULL) {
        const st_node *ch;
        if (!is_gradient(l->eid)) {
            return NULL;
        }
        for (ch = l->first; ch != NULL; ch = ch->next) {
            if (ch->eid == E_STOP) {
                return l;
            }
        }
    }
    return NULL;
}

static sv_spread spread_of(sv_conv *c, const st_node *n)
{
    const char *v = st_attr(resolve_attr(c->doc, n, A_SPREAD_METHOD), A_SPREAD_METHOD);
    if (v != NULL && strcmp(v, "reflect") == 0) {
        return SPREAD_REFLECT;
    }
    if (v != NULL && strcmp(v, "repeat") == 0) {
        return SPREAD_REPEAT;
    }
    return SPREAD_PAD;
}

/* ---------------------------------------------------------------- paint servers */

typedef enum { SRV_NONE, SRV_PAINT, SRV_COLOR } srv_kind;

typedef struct server {
    srv_kind kind;
    sv_paint paint;
    uint8_t r, g, b;
    float opacity;
} server;

static srv_kind stops_to_color(const sv_stop *s, size_t n, server *out)
{
    if (n == 0) {
        return SRV_NONE;
    }
    out->r = s[0].r;
    out->g = s[0].g;
    out->b = s[0].b;
    out->opacity = s[0].opacity;
    return SRV_COLOR;
}

static srv_kind convert_gradient(sv_conv *c, const st_node *n, const sv_cstate *st, server *out)
{
    const st_node *with = gradient_with_stops(c, n);
    sv_stop *stops;
    size_t nstops;
    sv_grad *g;
    if (st_id(n)[0] == '\0' || with == NULL) {
        return SRV_NONE;
    }
    if (!convert_stops(c, with, &stops, &nstops)) {
        return SRV_NONE;
    }
    if (nstops < 2) {
        return stops_to_color(stops, nstops, out);
    }
    g = sv_alloc(c->a, sizeof *g);
    if (g == NULL) {
        c->nomem = true;
        return SRV_NONE;
    }
    g->stops = stops;
    g->nstops = nstops;
    g->units = convert_units(c, n, A_GRADIENT_UNITS, UNITS_OBB);
    if (n->eid == E_LINEAR_GRADIENT) {
        g->transform = sv_resolve_transform(c, n, A_GRADIENT_TRANSFORM, st);
        g->x1 = resolve_number(c, n, A_X1, g->units, st, lpx(0.0));
        g->y1 = resolve_number(c, n, A_Y1, g->units, st, lpx(0.0));
        g->x2 = resolve_number(c, n, A_X2, g->units, st, lpct(100.0));
        g->y2 = resolve_number(c, n, A_Y2, g->units, st, lpx(0.0));
        g->spread = spread_of(c, n);
        out->paint.kind = PAINT_LINEAR;
    } else {
        g->radial = true;
        g->r = resolve_number(c, n, A_R, g->units, st, lpct(50.0));
        if (!valid_len(g->r)) {
            out->r = stops[nstops - 1].r;
            out->g = stops[nstops - 1].g;
            out->b = stops[nstops - 1].b;
            out->opacity = stops[nstops - 1].opacity;
            return SRV_COLOR;
        }
        g->spread = spread_of(c, n);
        g->cx = resolve_number(c, n, A_CX, g->units, st, lpct(50.0));
        g->cy = resolve_number(c, n, A_CY, g->units, st, lpct(50.0));
        g->fx = resolve_number(c, n, A_FX, g->units, st, lpx((double)g->cx));
        g->fy = resolve_number(c, n, A_FY, g->units, st, lpx((double)g->cy));
        g->transform = sv_resolve_transform(c, n, A_GRADIENT_TRANSFORM, st);
        out->paint.kind = PAINT_RADIAL;
    }
    out->paint.grad = g;
    return SRV_PAINT;
}

static const st_node *pattern_with_children(sv_conv *c, const st_node *n)
{
    href_iter it;
    const st_node *l;
    hi_init(&it, c->doc, n);
    while ((l = hi_next(&it)) != NULL) {
        if (l->eid != E_PATTERN) {
            return NULL;
        }
        if (l->first != NULL) {
            return l;
        }
    }
    return NULL;
}

static srv_kind convert_pattern(sv_conv *c, const st_node *n, const sv_cstate *st, server *out)
{
    const st_node *with = pattern_with_children(c, n);
    sv_pattern *p;
    const st_node *n1;
    const st_node *n2;
    if (with == NULL || st_id(n)[0] == '\0') {
        return SRV_NONE;
    }
    p = sv_alloc(c->a, sizeof *p);
    if (p == NULL) {
        c->nomem = true;
        return SRV_NONE;
    }
    n1 = resolve_attr(c->doc, n, A_VIEW_BOX);
    n2 = resolve_attr(c->doc, n, A_PRESERVE_ASPECT_RATIO);
    p->has_vb = sv_viewbox_of(n1, &p->vb);
    p->aspect = sv_aspect_default();
    if (st_attr(n2, A_PRESERVE_ASPECT_RATIO) != NULL &&
        !sv_parse_aspect(st_attr(n2, A_PRESERVE_ASPECT_RATIO), &p->aspect)) {
        p->aspect = sv_aspect_default();
    }
    p->units = convert_units(c, n, A_PATTERN_UNITS, UNITS_OBB);
    p->content_units = convert_units(c, n, A_PATTERN_CONTENT_UNITS, UNITS_USER);
    p->transform = sv_resolve_transform(c, n, A_PATTERN_TRANSFORM, st);
    if (!nzrect_from_xywh(resolve_number(c, n, A_X, p->units, st, lpx(0.0)),
                          resolve_number(c, n, A_Y, p->units, st, lpx(0.0)),
                          resolve_number(c, n, A_WIDTH, p->units, st, lpx(0.0)),
                          resolve_number(c, n, A_HEIGHT, p->units, st, lpx(0.0)), &p->rect)) {
        return SRV_NONE;
    }
    p->root = sv_group_empty(c->a);
    if (p->root == NULL) {
        c->nomem = true;
        return SRV_NONE;
    }
    if (p->has_vb && p->units == UNITS_USER && p->content_units == UNITS_USER) {
        sv_unode *g = sv_group_empty(c->a);
        if (g == NULL) {
            c->nomem = true;
            return SRV_NONE;
        }
        g->transform = sv_viewbox_transform(p->vb, p->aspect, rect_w(p->rect), rect_h(p->rect));
        g->abs_transform = g->transform;
        sv_convert_children(c, with, st, g);
        if (g->nchildren == 0) {
            return SRV_NONE;
        }
        (void)sv_group_bboxes(g);
        if (!sv_push_child(c->a, p->root, g)) {
            c->nomem = true;
            return SRV_NONE;
        }
    } else {
        sv_convert_children(c, with, st, p->root);
        if (p->root->nchildren == 0) {
            return SRV_NONE;
        }
    }
    (void)sv_group_bboxes(p->root);
    out->paint.kind = PAINT_PATTERN;
    out->paint.pat = p;
    return SRV_PAINT;
}

static srv_kind paint_server(sv_conv *c, const st_node *n, const sv_cstate *st, server *out)
{
    server *cached = cache_get(c->paints, c->npaints, n);
    srv_kind k;
    memset(out, 0, sizeof *out);
    if (cached != NULL) {
        *out = *cached;
        return SRV_PAINT;
    }
    if (c->depth > SV_MAX_CONV_DEPTH) {
        return SRV_NONE;
    }
    c->depth++;
    k = n->eid == E_PATTERN ? convert_pattern(c, n, st, out) : convert_gradient(c, n, st, out);
    c->depth--;
    if (k == SRV_PAINT) {
        server *s = sv_alloc(c->a, sizeof *s);
        if (s == NULL) {
            c->nomem = true;
            return SRV_NONE;
        }
        *s = *out;
        cache_put(c, &c->paints, &c->npaints, &c->cappaints, n, s);
    }
    return k;
}

static sv_units paint_units(const sv_paint *p)
{
    switch (p->kind) {
    case PAINT_LINEAR:
    case PAINT_RADIAL:
        return p->grad->units;
    case PAINT_PATTERN:
        return p->pat->units;
    case PAINT_COLOR:
    default:
        return UNITS_USER;
    }
}

/* ---------------------------------------------------------------- fill and stroke */

static void color_paint(sv_paint *p, sv_color col, float *opacity)
{
    memset(p, 0, sizeof *p);
    p->kind = PAINT_COLOR;
    p->r = col.r;
    p->g = col.g;
    p->b = col.b;
    *opacity = (float)col.a / 255.0f;
}

static bool from_fallback(const st_node *n, const sv_spaint *sp, sv_paint *out, float *opacity)
{
    switch (sp->fallback) {
    case PF_CURRENT_COLOR:
        color_paint(out, current_color(n), opacity);
        return true;
    case PF_COLOR:
        color_paint(out, sp->fallback_color, opacity);
        return true;
    case PF_NONE:
    case PF_NONE_SET:
    default:
        return false;
    }
}

/* convert_paint: false for no paint. */
static bool convert_paint(sv_conv *c, const st_node *n, sv_aid aid, bool has_bbox, const sv_cstate *st,
                          float *opacity, sv_paint *out, sv_ctx *ctx)
{
    const char *v = st_attr(n, aid);
    sv_spaint sp;
    *ctx = CTX_NONE;
    if (v == NULL) {
        return false;
    }
    if (!sv_parse_paint(v, &sp)) {
        if (aid == A_FILL) {
            sv_color black = {0, 0, 0, 255};
            color_paint(out, black, opacity);
            return true;
        }
        return false;
    }
    switch (sp.kind) {
    case P_NONE:
    case P_INHERIT:
        return false;
    case P_CONTEXT_FILL:
        if (!st->has_context || st->ctx_fill == NULL) {
            return false;
        }
        *out = st->ctx_fill->paint;
        *ctx = st->ctx_fill->context;
        return true;
    case P_CONTEXT_STROKE:
        if (!st->has_context || st->ctx_stroke == NULL) {
            return false;
        }
        *out = st->ctx_stroke->paint;
        *ctx = st->ctx_stroke->context;
        return true;
    case P_CURRENT_COLOR:
        color_paint(out, current_color(n), opacity);
        return true;
    case P_COLOR:
        color_paint(out, sp.color, opacity);
        return true;
    case P_FUNC_IRI:
    default: {
        st_node *link = st_by_id(c->doc, sp.iri, sp.iri_len);
        server srv;
        if (link == NULL) {
            return from_fallback(n, &sp, out, opacity);
        }
        if (link->eid != E_LINEAR_GRADIENT && link->eid != E_RADIAL_GRADIENT && link->eid != E_PATTERN) {
            return false;
        }
        switch (paint_server(c, link, st, &srv)) {
        case SRV_PAINT:
            if (!has_bbox && paint_units(&srv.paint) == UNITS_OBB) {
                return from_fallback(n, &sp, out, opacity);
            }
            *out = srv.paint;
            return true;
        case SRV_COLOR:
            memset(out, 0, sizeof *out);
            out->kind = PAINT_COLOR;
            out->r = srv.r;
            out->g = srv.g;
            out->b = srv.b;
            *opacity = srv.opacity;
            return true;
        case SRV_NONE:
        default:
            return from_fallback(n, &sp, out, opacity);
        }
    }
    }
}

sv_fill *sv_resolve_fill(sv_conv *c, const st_node *n, bool has_bbox, const sv_cstate *st)
{
    sv_fill *f = sv_alloc(c->a, sizeof *f);
    float sub = 1.0f;
    const st_node *p;
    const char *rule;
    if (f == NULL) {
        c->nomem = true;
        return NULL;
    }
    if (st->parent_clip_path != NULL) {
        f->paint.kind = PAINT_COLOR;
        f->opacity = 1.0f;
        rule = find_str(n, A_CLIP_RULE);
        f->evenodd = rule != NULL && strcmp(rule, "evenodd") == 0;
        return f;
    }
    for (p = n; p != NULL && !st_has(p, A_FILL); p = p->parent) {
    }
    if (p != NULL) {
        if (!convert_paint(c, p, A_FILL, has_bbox, st, &sub, &f->paint, &f->context)) {
            return NULL;
        }
    } else {
        f->paint.kind = PAINT_COLOR;
    }
    f->opacity = opacity_mul(sub, find_opacity(n, A_FILL_OPACITY));
    rule = find_str(n, A_FILL_RULE);
    f->evenodd = rule != NULL && strcmp(rule, "evenodd") == 0;
    return f;
}

/* resolve_length: the nearest ancestor-or-self value of aid, in user units. */
static float resolve_length(sv_conv *c, const st_node *n, sv_aid aid, const sv_cstate *st, float def)
{
    const st_node *p;
    for (p = n; p != NULL; p = p->parent) {
        if (st_has(p, aid)) {
            sv_length l;
            if (sv_parse_length(st_attr(p, aid), &l)) {
                return sv_convert_length(c, l, p, aid, UNITS_USER, st);
            }
            break;
        }
    }
    return def;
}

static bool dasharray(sv_conv *c, const st_node *n, const sv_cstate *st, float **out, size_t *nout)
{
    const st_node *p;
    const char *v;
    sv_stream s;
    float *list = NULL;
    size_t cnt = 0;
    size_t cap = 0;
    float sum = 0.0f;
    size_t i;
    for (p = n; p != NULL && !st_has(p, A_STROKE_DASHARRAY); p = p->parent) {
    }
    if (p == NULL) {
        return false;
    }
    v = st_attr(p, A_STROKE_DASHARRAY);
    ss_init(&s, v);
    while (!ss_at_end(&s)) {
        sv_length l;
        void *q = list;
        if (!ss_parse_list_length(&s, &l)) {
            break;
        }
        if (cnt >= 1024 || !sv_grow(c->a, &q, &cap, cnt + 1, sizeof(float))) {
            if (cnt < 1024) {
                c->nomem = true;
            }
            return false;
        }
        list = q;
        list[cnt++] = sv_convert_length(c, l, p, A_STROKE_DASHARRAY, UNITS_USER, st);
    }
    for (i = 0; i < cnt; i++) {
        if (signbit(list[i])) {
            return false;
        }
        sum += list[i];
    }
    if (stop_eq_ulps(sum, 0.0f)) {
        return false;
    }
    if (cnt % 2 != 0) {
        float *d = sv_alloc_array(c->a, cnt * 2, sizeof(float));
        if (d == NULL) {
            c->nomem = true;
            return false;
        }
        memcpy(d, list, cnt * sizeof(float));
        memcpy(d + cnt, list, cnt * sizeof(float));
        list = d;
        cnt *= 2;
    }
    *out = list;
    *nout = cnt;
    return true;
}

sv_stroke *sv_resolve_stroke(sv_conv *c, const st_node *n, bool has_bbox, const sv_cstate *st)
{
    sv_stroke *s;
    float sub = 1.0f;
    const st_node *p;
    const char *v;
    float ml = 4.0f;
    if (st->parent_clip_path != NULL) {
        return NULL;
    }
    for (p = n; p != NULL && !st_has(p, A_STROKE); p = p->parent) {
    }
    if (p == NULL) {
        return NULL;
    }
    s = sv_alloc(c->a, sizeof *s);
    if (s == NULL) {
        c->nomem = true;
        return NULL;
    }
    if (!convert_paint(c, p, A_STROKE, has_bbox, st, &sub, &s->paint, &s->context)) {
        return NULL;
    }
    s->width = resolve_length(c, n, A_STROKE_WIDTH, st, 1.0f);
    if (!valid_len(s->width)) {
        return NULL;
    }
    v = find_str(n, A_STROKE_MITERLIMIT);
    if (v != NULL) {
        double d;
        if (sv_parse_number(v, &d)) {
            ml = (float)d;
        }
    }
    s->miterlimit = ml < 1.0f ? 1.0f : ml;
    s->opacity = opacity_mul(sub, find_opacity(n, A_STROKE_OPACITY));
    if (!dasharray(c, n, st, &s->dash, &s->ndash)) {
        s->dash = NULL;
        s->ndash = 0;
    }
    s->dashoffset = resolve_length(c, n, A_STROKE_DASHOFFSET, st, 0.0f);
    v = find_str(n, A_STROKE_LINECAP);
    s->cap = v != NULL && strcmp(v, "round") == 0    ? CAP_ROUND
             : v != NULL && strcmp(v, "square") == 0 ? CAP_SQUARE
                                                     : CAP_BUTT;
    v = find_str(n, A_STROKE_LINEJOIN);
    s->join = v != NULL && strcmp(v, "round") == 0        ? JOIN_ROUND
              : v != NULL && strcmp(v, "bevel") == 0      ? JOIN_BEVEL
              : v != NULL && strcmp(v, "miter-clip") == 0 ? JOIN_MITER_CLIP
                                                          : JOIN_MITER;
    return s;
}

/* ---------------------------------------------------------------- clip paths and masks */

sv_clip *sv_convert_clip(sv_conv *c, const st_node *link, const sv_cstate *st, const sv_rect *object_bbox)
{
    sv_ts transform = ts_identity();
    sv_units units = UNITS_USER;
    const char *tv;
    sv_clip *clip;
    sv_cstate cs;
    st_node *sub;
    if (link->eid != E_CLIP_PATH) {
        return NULL;
    }
    tv = st_attr(link, A_TRANSFORM);
    if (tv != NULL) {
        double m[6];
        sv_ts t;
        if (!sv_parse_transform(tv, m)) {
            return NULL;
        }
        t = ts_row((float)m[0], (float)m[1], (float)m[2], (float)m[3], (float)m[4], (float)m[5]);
        if (!ts_is_valid(t)) {
            return NULL;
        }
        transform = sv_resolve_transform(c, link, A_TRANSFORM, st);
    }
    (void)units_of(st_attr(link, A_CLIP_PATH_UNITS), &units);
    if (units == UNITS_USER) {
        sv_clip *hit = cache_get(c->clips, c->nclips, link);
        if (hit != NULL) {
            return hit;
        }
    } else {
        if (object_bbox == NULL) {
            return NULL;
        }
        transform = ts_pre_concat(transform, ts_from_bbox(*object_bbox));
    }
    if (c->depth > SV_MAX_CONV_DEPTH) {
        return NULL;
    }
    clip = sv_alloc(c->a, sizeof *clip);
    if (clip == NULL) {
        c->nomem = true;
        return NULL;
    }
    clip->transform = transform;
    sub = st_node_attr(c->doc, link, A_CLIP_PATH);
    if (sub != NULL) {
        c->depth++;
        clip->clip = sv_convert_clip(c, sub, st, object_bbox);
        c->depth--;
        if (clip->clip == NULL) {
            return NULL;
        }
    }
    if (st_id(link)[0] == '\0') {
        return NULL;
    }
    clip->root = sv_group_empty(c->a);
    if (clip->root == NULL) {
        c->nomem = true;
        return NULL;
    }
    cs = *st;
    cs.parent_clip_path = link;
    c->depth++;
    sv_convert_clip_children(c, link, &cs, clip->root);
    c->depth--;
    if (clip->root->nchildren == 0) {
        return NULL;
    }
    (void)sv_group_bboxes(clip->root);
    if (units == UNITS_USER) {
        cache_put(c, &c->clips, &c->nclips, &c->capclips, link, clip);
    }
    return clip;
}

sv_mask *sv_convert_mask(sv_conv *c, const st_node *link, const sv_cstate *st, const sv_rect *object_bbox)
{
    sv_units units = UNITS_OBB;
    sv_units content = UNITS_USER;
    sv_rect rect;
    bool cacheable;
    bool mask_all = false;
    sv_mask *m;
    st_node *sub;
    sv_unode *real_root;
    const char *mt;
    if (link->eid != E_MASK) {
        return NULL;
    }
    (void)units_of(st_attr(link, A_MASK_UNITS), &units);
    (void)units_of(st_attr(link, A_MASK_CONTENT_UNITS), &content);
    cacheable = units == UNITS_USER && content == UNITS_USER;
    if (cacheable) {
        sv_mask *hit = cache_get(c->masks, c->nmasks, link);
        if (hit != NULL) {
            return hit;
        }
    }
    if (!nzrect_from_xywh(sv_node_length(c, link, A_X, units, st, lpct(-10.0)),
                          sv_node_length(c, link, A_Y, units, st, lpct(-10.0)),
                          sv_node_length(c, link, A_WIDTH, units, st, lpct(120.0)),
                          sv_node_length(c, link, A_HEIGHT, units, st, lpct(120.0)), &rect)) {
        return NULL;
    }
    if (units == UNITS_OBB) {
        if (object_bbox != NULL) {
            rect = rect_bbox_transform(rect, *object_bbox);
        } else {
            mask_all = true;
        }
    }
    if (st_id(link)[0] == '\0') {
        return NULL;
    }
    m = sv_alloc(c->a, sizeof *m);
    if (m == NULL) {
        c->nomem = true;
        return NULL;
    }
    m->rect = rect;
    m->root = sv_group_empty(c->a);
    if (m->root == NULL) {
        c->nomem = true;
        return NULL;
    }
    if (mask_all) {
        cache_put(c, &c->masks, &c->nmasks, &c->capmasks, link, m);
        return m;
    }
    if (c->depth > SV_MAX_CONV_DEPTH) {
        return NULL;
    }
    sub = st_node_attr(c->doc, link, A_MASK);
    if (sub != NULL) {
        c->depth++;
        m->mask = sv_convert_mask(c, sub, st, object_bbox);
        c->depth--;
        if (m->mask == NULL) {
            return NULL;
        }
    }
    mt = st_attr(link, A_MASK_TYPE);
    m->alpha = mt != NULL && strcmp(mt, "alpha") == 0;
    real_root = m->root;
    if (content == UNITS_OBB) {
        if (object_bbox == NULL) {
            return NULL;
        }
        real_root = sv_group_empty(c->a);
        if (real_root == NULL) {
            c->nomem = true;
            return NULL;
        }
        real_root->transform = ts_from_bbox(*object_bbox);
        real_root->abs_transform = real_root->transform;
    }
    c->depth++;
    sv_convert_children(c, link, st, real_root);
    c->depth--;
    if (real_root->nchildren == 0) {
        return NULL;
    }
    if (real_root != m->root) {
        (void)sv_group_bboxes(real_root);
        if (!sv_push_child(c->a, m->root, real_root)) {
            c->nomem = true;
            return NULL;
        }
    }
    (void)sv_group_bboxes(m->root);
    cache_put(c, &c->masks, &c->nmasks, &c->capmasks, link, m);
    return m;
}

/* Filters: svg_filter_conv.c. */

/* ---------------------------------------------------------------- paint servers to user space */

static void push_pattern_transform(sv_conv *c, sv_pattern *p, sv_ts ts)
{
    sv_unode *root = sv_group_empty(c->a);
    if (root == NULL) {
        c->nomem = true;
        return;
    }
    p->root->transform = ts;
    p->root->abs_transform = ts;
    if (!sv_push_child(c->a, root, p->root)) {
        c->nomem = true;
        return;
    }
    (void)sv_group_bboxes(root);
    p->root = root;
}

/* Paint::to_user_coordinates on a private copy of the server. */
static bool to_user(sv_conv *c, sv_paint *paint, sv_rect bbox)
{
    sv_rect nz;
    if (!rect_to_nz(bbox, &nz)) {
        return false;
    }
    if (paint->kind == PAINT_LINEAR || paint->kind == PAINT_RADIAL) {
        sv_grad *g = sv_alloc(c->a, sizeof *g);
        if (g == NULL) {
            c->nomem = true;
            return false;
        }
        *g = *paint->grad;
        g->transform = ts_post_concat(g->transform, ts_from_bbox(nz));
        g->units = UNITS_USER;
        paint->grad = g;
    } else if (paint->kind == PAINT_PATTERN) {
        sv_pattern *p = sv_alloc(c->a, sizeof *p);
        if (p == NULL) {
            c->nomem = true;
            return false;
        }
        *p = *paint->pat;
        if (p->units == UNITS_OBB) {
            p->rect = rect_bbox_transform(p->rect, nz);
        }
        if (p->content_units == UNITS_OBB && !p->has_vb) {
            push_pattern_transform(c, p, ts_scale(rect_w(nz), rect_h(nz)));
        }
        if (p->has_vb) {
            push_pattern_transform(c, p,
                                   sv_viewbox_transform(p->vb, p->aspect, rect_w(p->rect), rect_h(p->rect)));
        }
        p->units = UNITS_USER;
        p->content_units = UNITS_USER;
        paint->pat = p;
    }
    return true;
}

static void context_paint(sv_conv *c, sv_paint *paint, sv_ts ctx_ts, sv_ts path_ts)
{
    sv_ts inv;
    sv_ts rev;
    if (!ts_invert(ctx_ts, &inv) || !ts_invert(ts_pre_concat(inv, path_ts), &rev)) {
        return;
    }
    if (paint->kind == PAINT_LINEAR || paint->kind == PAINT_RADIAL) {
        sv_grad *g = sv_alloc(c->a, sizeof *g);
        if (g == NULL) {
            c->nomem = true;
            return;
        }
        *g = *paint->grad;
        g->transform = ts_post_concat(g->transform, rev);
        paint->grad = g;
    } else if (paint->kind == PAINT_PATTERN) {
        sv_pattern *p = sv_alloc(c->a, sizeof *p);
        if (p == NULL) {
            c->nomem = true;
            return;
        }
        *p = *paint->pat;
        p->transform = ts_post_concat(p->transform, rev);
        paint->pat = p;
    }
}

static bool process_paint(sv_conv *c, sv_paint *paint, bool has_context, sv_ts ctx_ts,
                          const sv_rect *ctx_bbox, sv_ts path_ts, sv_rect bbox)
{
    bool obb = paint_units(paint) == UNITS_OBB ||
               (paint->kind == PAINT_PATTERN && paint->pat->content_units == UNITS_OBB);
    if (obb) {
        sv_rect b = bbox;
        if (has_context) {
            if (ctx_bbox == NULL) {
                return false;
            }
            b = *ctx_bbox;
        }
        if (!to_user(c, paint, b)) {
            return false;
        }
    }
    if (paint->kind == PAINT_PATTERN && c->depth <= SV_MAX_CONV_DEPTH) {
        c->depth++;
        sv_update_paint_servers(c, paint->pat->root, ts_identity(), NULL);
        c->depth--;
    }
    if (has_context) {
        context_paint(c, paint, ctx_ts, path_ts);
    }
    return true;
}

bool sv_process_paint(sv_conv *c, sv_paint *paint, bool has_context, sv_ts ctx_ts, const sv_rect *ctx_bbox,
                      sv_ts path_ts, sv_rect bbox)
{
    return process_paint(c, paint, has_context, ctx_ts, ctx_bbox, path_ts, bbox);
}

static void update_ps(sv_conv *c, sv_unode *g, sv_ts ctx_ts, const sv_rect *ctx_bbox,
                      const sv_rect *text_bbox);

/* node_to_user_coordinates; text_bbox: inside a text's flattened group, paths use the text's bbox. */
static void node_to_user(sv_conv *c, sv_unode *n, sv_ts ctx_ts, const sv_rect *ctx_bbox,
                         const sv_rect *text_bbox)
{
    if (n->kind == UN_GROUP) {
        if (n->mask != NULL) {
            sv_update_paint_servers(c, n->mask->root, ctx_ts, ctx_bbox);
            if (n->mask->mask != NULL) {
                sv_update_paint_servers(c, n->mask->mask->root, ctx_ts, ctx_bbox);
            }
        }
        sv_filters_update_paint_servers(c, n, ctx_ts, ctx_bbox);
        update_ps(c, n, ctx_ts, ctx_bbox, text_bbox);
    } else if (n->kind == UN_TEXT) {
        if (n->flat != NULL) {
            sv_rect tb = n->bbox;
            update_ps(c, n->flat, ctx_ts, ctx_bbox, &tb);
        }
    } else if (n->kind == UN_PATH) {
        if (n->fill != NULL) {
            sv_fill *f = sv_alloc(c->a, sizeof *f);
            if (f == NULL) {
                c->nomem = true;
                return;
            }
            *f = *n->fill;
            n->fill = process_paint(c, &f->paint, f->context == CTX_USE, ctx_ts, ctx_bbox, n->abs_transform,
                                    text_bbox != NULL ? *text_bbox : n->bbox)
                          ? f
                          : NULL;
        }
        if (n->stroke != NULL) {
            sv_stroke *s = sv_alloc(c->a, sizeof *s);
            if (s == NULL) {
                c->nomem = true;
                return;
            }
            *s = *n->stroke;
            n->stroke = process_paint(c, &s->paint, s->context == CTX_USE, ctx_ts, ctx_bbox, n->abs_transform,
                                      text_bbox != NULL ? *text_bbox : n->bbox)
                            ? s
                            : NULL;
        }
    }
}

void sv_update_paint_servers(sv_conv *c, sv_unode *g, sv_ts ctx_ts, const sv_rect *ctx_bbox)
{
    update_ps(c, g, ctx_ts, ctx_bbox, NULL);
}

static void update_ps(sv_conv *c, sv_unode *g, sv_ts ctx_ts, const sv_rect *ctx_bbox,
                      const sv_rect *text_bbox)
{
    size_t i;
    if (c->depth > SV_MAX_CONV_DEPTH * 2) {
        return;
    }
    c->depth++;
    for (i = 0; i < g->nchildren; i++) {
        if (g->is_context) {
            sv_rect b = g->bbox;
            node_to_user(c, g->children[i], g->abs_transform, &b, text_bbox);
        } else {
            node_to_user(c, g->children[i], ctx_ts, ctx_bbox, text_bbox);
        }
    }
    c->depth--;
}
