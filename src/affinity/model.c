/* The document model PhotoCraft imports (port of crates/affinity/src/model.rs): spreads,
 * artboards, layers, groups, curves, shapes, text and images, with transforms composed into
 * document space. Anything not imported is reported by the document's warnings. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "affinity/internal.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"

/* Deepest layer nesting followed; total layer nodes imported. */
#define MAX_TREE_DEPTH 128u
#define MAX_NODES 500000u

const pc_af_affine AX_IDENTITY = {{1.0, 0.0, 0.0, 1.0, 0.0, 0.0}};

/* ---- affine, rect ---- */

pc_af_affine ax_affine_from_xfrm(const double m[6])
{
    pc_af_affine a = {{m[0], m[3], m[1], m[4], m[2], m[5]}};
    return a;
}

pc_af_affine ax_affine_then(pc_af_affine self, pc_af_affine outer)
{
    double a = self.m[0], b = self.m[1], c = self.m[2], d = self.m[3], e = self.m[4], f = self.m[5];
    double oa = outer.m[0], ob = outer.m[1], oc = outer.m[2], od = outer.m[3], oe = outer.m[4], of = outer.m[5];
    pc_af_affine r = {{oa * a + oc * b, ob * a + od * b, oa * c + oc * d, ob * c + od * d, oa * e + oc * f + oe,
                       ob * e + od * f + of}};
    return r;
}

pc_af_point ax_affine_apply(const pc_af_affine *m, pc_af_point p)
{
    pc_af_point q;
    q.x = m->m[0] * p.x + m->m[2] * p.y + m->m[4];
    q.y = m->m[1] * p.x + m->m[3] * p.y + m->m[5];
    return q;
}

double ax_affine_scale(const pc_af_affine *m)
{
    return sqrt(fabs(m->m[0] * m->m[3] - m->m[1] * m->m[2]));
}

bool ax_affine_is_finite(const pc_af_affine *m)
{
    int i;
    for (i = 0; i < 6; i++) {
        if (!isfinite(m->m[i])) {
            return false;
        }
    }
    return true;
}

pc_af_affine ax_xfrm(const pc_af_stream *s, size_t id)
{
    double m[6];
    return ax_floats(s, id, "Xfrm", 6, m) ? ax_affine_from_xfrm(m) : AX_IDENTITY;
}

pc_af_rect ax_rect(const double v[4])
{
    pc_af_rect r;
    r.x0 = v[0];
    r.y0 = v[1];
    r.x1 = v[2];
    r.y1 = v[3];
    return r;
}

pc_af_rect ax_union(pc_af_rect a, pc_af_rect b)
{
    pc_af_rect r;
    r.x0 = pc_rs_min(a.x0, b.x0);
    r.y0 = pc_rs_min(a.y0, b.y0);
    r.x1 = pc_rs_max(a.x1, b.x1);
    r.y1 = pc_rs_max(a.y1, b.y1);
    return r;
}

static bool is4(pc_af_tag t, const char *s) { return t == PC_AF_TAG(s); }

const char *ax_describe(pc_af_tag c)
{
    if (is4(c, "PCrv")) {
        return "curve";
    }
    if (is4(c, "ShpN") || is4(c, "SNEN") || is4(c, "SNRR") || is4(c, "ShRN")) {
        return "shape";
    }
    if (is4(c, "Comp")) {
        return "compound";
    }
    if (is4(c, "Rstr")) {
        return "pixel";
    }
    if (is4(c, "ImgN")) {
        return "image";
    }
    if (is4(c, "EmbN")) {
        return "embedded document";
    }
    if (is4(c, "MPIN")) {
        return "master page instance";
    }
    if (is4(c, "FRst")) {
        return "fill";
    }
    if (is4(c, "MRst")) {
        return "mask";
    }
    if (is4(c, "Slic") || is4(c, "SlcP")) {
        return "slice";
    }
    if (is4(c, "3DLA")) {
        return "3D";
    }
    if (((c >> 8) & 0xFF) == 'R' && (c & 0xFF) == 'A') {
        return "adjustment";
    }
    return "unknown";
}

/* ---- frees ---- */

void ax_subpath_fini(pc_af_subpath *sp)
{
    free(sp->segments);
    sp->segments = NULL;
    sp->nsegments = 0;
}

void ax_path_fini(pc_af_path *p)
{
    size_t i;
    for (i = 0; i < p->nsubpaths; i++) {
        ax_subpath_fini(&p->subpaths[i]);
    }
    free(p->subpaths);
    p->subpaths = NULL;
    p->nsubpaths = 0;
}

pc_status ax_path_push(pc_af_path *p, pc_af_subpath sp, pc_error *err)
{
    pc_af_subpath *v = realloc(p->subpaths, (p->nsubpaths + 1) * sizeof *v);
    if (v == NULL) {
        ax_subpath_fini(&sp);
        return ax_nomem(err);
    }
    p->subpaths = v;
    p->subpaths[p->nsubpaths++] = sp;
    return PC_OK;
}

pc_status ax_path_append_transformed(pc_af_path *dst, const pc_af_path *src, pc_af_affine m, pc_error *err)
{
    size_t i;
    for (i = 0; i < src->nsubpaths; i++) {
        const pc_af_subpath *s = &src->subpaths[i];
        pc_af_subpath t;
        size_t k;
        pc_status st;
        t.start = ax_affine_apply(&m, s->start);
        t.closed = s->closed;
        t.nsegments = s->nsegments;
        t.segments = pc_malloc_array(s->nsegments > 0 ? s->nsegments : 1, sizeof *t.segments);
        if (t.segments == NULL) {
            return ax_nomem(err);
        }
        for (k = 0; k < s->nsegments; k++) {
            int j;
            for (j = 0; j < 3; j++) {
                t.segments[k][j] = ax_affine_apply(&m, s->segments[k][j]);
            }
        }
        if ((st = ax_path_push(dst, t, err)) != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

void ax_paint_fini(pc_af_paint *p)
{
    free(p->stops);
    p->stops = NULL;
    p->nstops = 0;
}

void ax_stroke_fini(pc_af_stroke *s)
{
    ax_paint_fini(&s->paint);
    free(s->dash);
    s->dash = NULL;
}

void ax_image_fini(pc_af_image *i)
{
    free(i->pixels);
    i->pixels = NULL;
}

void ax_text_fini(pc_af_text *t)
{
    size_t i;
    for (i = 0; i < t->nruns; i++) {
        free(t->runs[i].text);
        free(t->runs[i].postscript);
        free(t->runs[i].family);
        ax_paint_fini(&t->runs[i].fill);
    }
    free(t->runs);
    t->runs = NULL;
    t->nruns = 0;
}

static void paints_fini(pc_af_paint *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        ax_paint_fini(&p[i]);
    }
    free(p);
}

void ax_node_fini(pc_af_node *n)
{
    size_t i;
    free(n->name);
    paints_fini(n->background, n->nbackground);
    ax_path_fini(&n->path);
    paints_fini(n->fills, n->nfills);
    for (i = 0; i < n->nstrokes; i++) {
        ax_stroke_fini(&n->strokes[i]);
    }
    free(n->strokes);
    ax_text_fini(&n->text);
    ax_image_fini(&n->image);
    ax_path_fini(&n->mask);
    ax_image_fini(&n->pixel_mask);
    for (i = 0; i < n->nchildren; i++) {
        ax_node_fini(&n->children[i]);
    }
    free(n->children);
    memset(n, 0, sizeof *n);
}

static void nodes_fini(pc_af_node *v, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        ax_node_fini(&v[i]);
    }
    free(v);
}

void pc_af_document_fini(pc_af_document *d)
{
    size_t i;
    if (d == NULL) {
        return;
    }
    for (i = 0; i < d->nspreads; i++) {
        free(d->spreads[i].pages);
        nodes_fini(d->spreads[i].nodes, d->spreads[i].nnodes);
    }
    free(d->spreads);
    free(d->saved_by);
    for (i = 0; i < d->nwarnings; i++) {
        free(d->warnings[i]);
    }
    free(d->warnings);
    memset(d, 0, sizeof *d);
}

/* ---- warnings (a BTreeMap<String, count>) ---- */

void ax_warn(ax_reader *r, const char *what)
{
    size_t lo = 0;
    size_t hi = r->nwarnings;
    ax_warning *v;
    char *t;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(r->warnings[mid].text, what);
        if (c == 0) {
            r->warnings[mid].count++;
            return;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    v = realloc(r->warnings, (r->nwarnings + 1) * sizeof *v);
    t = pc_strndup(what, strlen(what));
    if (v == NULL || t == NULL) {
        if (v != NULL) {
            r->warnings = v;
        }
        free(t);
        r->nomem = true;
        return;
    }
    r->warnings = v;
    memmove(r->warnings + lo + 1, r->warnings + lo, (r->nwarnings - lo) * sizeof *v);
    r->warnings[lo].text = t;
    r->warnings[lo].count = 1;
    r->nwarnings++;
}

void ax_warnf(ax_reader *r, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ax_warn(r, buf);
}

/* ---- the reader ---- */

/* Pushes node n onto a growable node array. */
static pc_status push_node(pc_af_node **v, size_t *n, size_t *cap, pc_af_node *node, pc_error *err)
{
    if (*n == *cap) {
        size_t c = *cap > 0 ? *cap * 2 : 4;
        pc_af_node *g = realloc(*v, c * sizeof *g);
        if (g == NULL) {
            ax_node_fini(node);
            return ax_nomem(err);
        }
        *v = g;
        *cap = c;
    }
    (*v)[(*n)++] = *node;
    return PC_OK;
}

static pc_status node(ax_reader *r, size_t id, pc_af_affine parent, size_t depth, bool *has, pc_af_node *out,
                      pc_error *err);

/* The children of parent, and the pixel mask a mask layer among them puts on the parent. */
static pc_status children(ax_reader *r, size_t parent, pc_af_affine world, size_t depth, pc_af_node **out,
                          size_t *nout, bool *has_mask, pc_af_image *mask, pc_error *err)
{
    const pc_af_stream *s = r->s;
    const pc_af_value *arr = ax_objs(s, parent, "Chld");
    size_t i = 0;
    size_t c;
    pc_af_node *v = NULL;
    size_t n = 0;
    size_t cap = 0;
    pc_status st;
    *has_mask = false;
    memset(mask, 0, sizeof *mask);
    while (ax_next_obj(arr, &i, &c)) {
        if (ax_is(s, c, "MRst")) {
            /* A mask layer inside a group or layer masks that container. */
            bool vis;
            bool got;
            pc_af_image m;
            if (ax_bool(s, c, "Visi", &vis) && !vis) {
                continue;
            }
            if ((st = ax_mask(r, c, ax_affine_then(ax_xfrm(s, c), world), &got, &m, err)) != PC_OK) {
                goto fail;
            }
            if (got && !*has_mask) {
                *mask = m;
                *has_mask = true;
            } else if (got) {
                ax_image_fini(&m);
                ax_warn(r, "several pixel masks on one layer (only the first is used)");
            }
            continue;
        } else {
            bool got;
            pc_af_node nd;
            if ((st = node(r, c, world, depth + 1, &got, &nd, err)) != PC_OK) {
                goto fail;
            }
            if (got && (st = push_node(&v, &n, &cap, &nd, err)) != PC_OK) {
                goto fail;
            }
        }
    }
    *out = v;
    *nout = n;
    return PC_OK;
fail:
    nodes_fini(v, n);
    if (*has_mask) {
        ax_image_fini(mask);
        *has_mask = false;
    }
    return st;
}

static bool is_container(pc_af_tag c) { return is4(c, "Scop") || is4(c, "Grup"); }

static pc_af_blend blend_from_enum(uint16_t id, uint16_t v, bool *ok)
{
    static const struct {
        uint16_t id;
        int ver; /* -1: any */
        pc_af_blend b;
    } T[] = {{0, -1, PC_AF_NORMAL},       {1, 0, PC_AF_DARKEN},       {2, 1, PC_AF_DARKER_COLOR},
             {2, 0, PC_AF_MULTIPLY},      {3, 0, PC_AF_COLOR_BURN},   {4, 0, PC_AF_LIGHTEN},
             {6, 1, PC_AF_LIGHTER_COLOR}, {5, 0, PC_AF_SCREEN},       {6, 0, PC_AF_COLOR_DODGE},
             {7, 0, PC_AF_ADD},           {8, 0, PC_AF_OVERLAY},      {9, 0, PC_AF_SOFT_LIGHT},
             {10, 0, PC_AF_HARD_LIGHT},   {11, 0, PC_AF_VIVID_LIGHT}, {12, 0, PC_AF_PIN_LIGHT},
             {15, 1, PC_AF_LINEAR_LIGHT}, {13, 0, PC_AF_HARD_MIX},    {14, 0, PC_AF_DIFFERENCE},
             {15, 0, PC_AF_EXCLUSION},    {16, 0, PC_AF_SUBTRACT},    {17, 0, PC_AF_HUE},
             {18, 0, PC_AF_SATURATION},   {19, 0, PC_AF_LUMINOSITY},  {20, 0, PC_AF_COLOR},
             {21, 0, PC_AF_AVERAGE},      {22, 0, PC_AF_NEGATION},    {23, 0, PC_AF_REFLECT},
             {24, 0, PC_AF_GLOW},         {25, 0, PC_AF_ERASE}};
    size_t i;
    for (i = 0; i < sizeof T / sizeof T[0]; i++) {
        if (T[i].id == id && (T[i].ver < 0 || T[i].ver == v)) {
            *ok = true;
            return T[i].b;
        }
    }
    *ok = false;
    return PC_AF_NORMAL;
}

static pc_status artboard(ax_reader *r, size_t id, pc_af_affine world, pc_af_node *n, pc_error *err)
{
    const pc_af_stream *s = r->s;
    double v[4] = {0.0, 0.0, 0.0, 0.0};
    pc_af_rect b;
    pc_af_point corners[4];
    int k;
    double x0 = INFINITY;
    double y0 = INFINITY;
    double x1 = -INFINITY;
    double y1 = -INFINITY;
    if (!ax_floats(s, id, "ShpB", 4, v)) {
        memset(v, 0, sizeof v);
    }
    b = ax_rect(v);
    corners[0].x = b.x0;
    corners[0].y = b.y0;
    corners[1].x = b.x1;
    corners[1].y = b.y0;
    corners[2].x = b.x1;
    corners[2].y = b.y1;
    corners[3].x = b.x0;
    corners[3].y = b.y1;
    for (k = 0; k < 4; k++) {
        corners[k] = ax_affine_apply(&world, corners[k]);
    }
    if (fabs(world.m[1]) > 1e-9 || fabs(world.m[2]) > 1e-9 || world.m[0] <= 0.0 || world.m[3] <= 0.0) {
        ax_warn(r, "a rotated or flipped artboard (imported at its bounding box)");
    }
    for (k = 0; k < 4; k++) {
        x0 = pc_rs_min(x0, corners[k].x);
        y0 = pc_rs_min(y0, corners[k].y);
        x1 = pc_rs_max(x1, corners[k].x);
        y1 = pc_rs_max(y1, corners[k].y);
    }
    n->kind = PC_AF_ARTBOARD;
    n->rect.x0 = x0;
    n->rect.y0 = y0;
    n->rect.x1 = x1;
    n->rect.y1 = y1;
    return ax_fills(r, id, world, "BFFl", &n->background, &n->nbackground, err);
}

static pc_status kind(ax_reader *r, size_t id, pc_af_tag cls, pc_af_affine world, pc_af_affine parent, pc_af_node *n,
                      pc_error *err)
{
    const pc_af_stream *s = r->s;
    pc_status st;
    if (is4(cls, "Scop")) {
        n->kind = PC_AF_LAYER;
        return PC_OK;
    }
    if (is4(cls, "Grup")) {
        n->kind = PC_AF_GROUP;
        return PC_OK;
    }
    if (is4(cls, "PCrv") || is4(cls, "ShpN") || is4(cls, "Comp") || is4(cls, "SNEN") || is4(cls, "SNRR") ||
        is4(cls, "ShRN")) {
        bool ab;
        bool ok;
        bool even_odd;
        pc_af_path local = {NULL, 0};
        size_t props;
        /* Current .af boards carry artboard properties (aprp) in phrp; older documents use ABEn.
         * Converted curve boards keep those properties too. Other property classes also use
         * phrp, so the field alone is not a marker. */
        if ((ax_bool(s, id, "ABEn", &ab) && ab) ||
            (ax_obj(s, id, "phrp", &props) && ax_is(s, props, "aprp"))) {
            if (!is4(cls, "ShpN")) {
                ax_warn(r, "a converted artboard outline (imported at its bounding box)");
            }
            return artboard(r, id, world, n, err);
        }
        if ((st = ax_outline(r, id, cls, world, &ok, &local, &even_odd, err)) != PC_OK) {
            return st;
        }
        if (!ok) {
            ax_warnf(r, "a %s that could not be read", ax_describe(cls));
            n->kind = PC_AF_UNSUPPORTED;
            return PC_OK;
        }
        if ((st = ax_node_paint(r, id, world, &n->fills, &n->nfills, &n->strokes, &n->nstrokes, err)) != PC_OK ||
            (st = ax_path_append_transformed(&n->path, &local, world, err)) != PC_OK) {
            ax_path_fini(&local);
            return st;
        }
        ax_path_fini(&local);
        n->even_odd = even_odd;
        n->kind = PC_AF_SHAPE;
        return PC_OK;
    }
    if (is4(cls, "TxtA") || is4(cls, "TxtF") || is4(cls, "TxtC")) {
        bool ok;
        if ((st = ax_text_read(r, id, world, &ok, &n->text, err)) != PC_OK) {
            return st;
        }
        if (ok) {
            n->kind = PC_AF_TEXT;
        } else {
            ax_warn(r, "text whose story could not be read (master-page text, or an unknown layout)");
            n->kind = PC_AF_UNSUPPORTED;
        }
        return PC_OK;
    }
    if (is4(cls, "ImgN") || is4(cls, "Rstr")) {
        bool has;
        if ((st = ax_node_image(r, id, world, &has, &n->image, err)) != PC_OK) {
            return st;
        }
        n->kind = has ? PC_AF_IMAGE : PC_AF_UNSUPPORTED;
        return PC_OK;
    }
    if (is4(cls, "EmbN")) {
        bool has;
        if ((st = ax_embedded(r, id, world, &has, &n->image, err)) != PC_OK) {
            return st;
        }
        if (has) {
            ax_warn(r, "embedded documents and symbols (imported as pictures of them)");
            n->kind = PC_AF_IMAGE;
        } else {
            ax_warn(r, "embedded documents and symbols without a cached picture");
            n->kind = PC_AF_UNSUPPORTED;
        }
        return PC_OK;
    }
    if (is4(cls, "FRst")) {
        /* A fill layer paints its whole bitmap area; its gradient lives in page space. */
        size_t b;
        int64_t iw;
        int64_t ih;
        bool hb = ax_obj(s, id, "Bitm", &b);
        double w = hb && ax_int(s, b, "BmpW", &iw) ? (double)iw : 0.0;
        double h = hb && ax_int(s, b, "BmpH", &ih) ? (double)ih : 0.0;
        if (w > 0.0 && h > 0.0) {
            pc_af_path local = {NULL, 0};
            pc_af_subpath sp;
            if ((st = ax_rectangle(0.0, 0.0, w, h, &sp, err)) != PC_OK ||
                (st = ax_path_push(&local, sp, err)) != PC_OK) {
                return st;
            }
            st = ax_path_append_transformed(&n->path, &local, world, err);
            ax_path_fini(&local);
            if (st != PC_OK || (st = ax_fills(r, id, parent, "BFFl", &n->fills, &n->nfills, err)) != PC_OK) {
                return st;
            }
            n->kind = PC_AF_SHAPE;
            n->even_odd = false;
            return PC_OK;
        }
        ax_warn(r, "fill layers without an area");
        n->kind = PC_AF_UNSUPPORTED;
        return PC_OK;
    }
    ax_warnf(r, "%s layers", ax_describe(cls));
    n->kind = PC_AF_UNSUPPORTED;
    return PC_OK;
}

/* Vector shapes attached to a node (AdCh) mask it; an attached mask layer is a pixel mask. */
static pc_status attached_masks(ax_reader *r, size_t id, pc_af_affine world, pc_af_node *n, pc_error *err)
{
    const pc_af_stream *s = r->s;
    const pc_af_value *arr = ax_objs(s, id, "AdCh");
    size_t i = 0;
    size_t a;
    pc_status st;
    while (ax_next_obj(arr, &i, &a)) {
        pc_af_tag cls = 0;
        bool vis;
        (void)ax_class(s, a, &cls);
        if (ax_bool(s, a, "Visi", &vis) && !vis) {
            continue;
        }
        if (is4(cls, "PCrv") || is4(cls, "ShpN") || is4(cls, "Comp")) {
            pc_af_affine local = ax_affine_then(ax_xfrm(s, a), world);
            pc_af_path p = {NULL, 0};
            bool ok;
            bool eo;
            if ((st = ax_outline(r, a, cls, local, &ok, &p, &eo, err)) != PC_OK) {
                return st;
            }
            if (ok) {
                n->has_mask = true;
                st = ax_path_append_transformed(&n->mask, &p, local, err);
                ax_path_fini(&p);
                if (st != PC_OK) {
                    return st;
                }
            }
        } else if (is4(cls, "MRst")) {
            pc_af_affine local = ax_affine_then(ax_xfrm(s, a), world);
            bool got;
            pc_af_image m;
            if ((st = ax_mask(r, a, local, &got, &m, err)) != PC_OK) {
                return st;
            }
            if (got && !n->has_pixel_mask) {
                n->pixel_mask = m;
                n->has_pixel_mask = true;
            } else if (got) {
                ax_image_fini(&m);
                ax_warn(r, "several pixel masks on one layer (only the first is used)");
            }
        } else {
            ax_warnf(r, "%s layers attached to other layers (imported without them)", ax_describe(cls));
        }
    }
    return PC_OK;
}

static pc_status node(ax_reader *r, size_t id, pc_af_affine parent, size_t depth, bool *has, pc_af_node *out,
                      pc_error *err)
{
    const pc_af_stream *s = r->s;
    pc_af_node n;
    pc_af_affine world;
    pc_af_tag cls = 0;
    const char *name;
    bool b;
    double o;
    uint16_t eid;
    uint16_t ever;
    bool child_mask = false;
    pc_af_image cm;
    pc_status st;
    *has = false;
    if (depth > MAX_TREE_DEPTH) {
        ax_warn(r, "layers nested deeper than 128 levels");
        return PC_OK;
    }
    r->nodes++;
    if (r->nodes > MAX_NODES) {
        return ax_limit(err, "too many layers");
    }
    /* A node that contains itself (a shared object linked back into its own subtree). */
    if (r->active[id]) {
        ax_warn(r, "a layer that contains itself");
        return PC_OK;
    }
    r->active[id] = 1;
    memset(&n, 0, sizeof n);
    world = ax_affine_then(ax_xfrm(s, id), parent);
    (void)ax_class(s, id, &cls);
    n.cls = cls;
    name = ax_str(s, id, "Desc");
    n.name = pc_strndup(name != NULL ? name : "", name != NULL ? strlen(name) : 0);
    if (n.name == NULL) {
        return ax_nomem(err);
    }
    n.visible = ax_bool(s, id, "Visi", &b) ? b : true;
    n.locked = ax_bool(s, id, "Edtb", &b) && !b;
    n.opacity = ax_f64(s, id, "Opac", &o) ? pc_clampd(o, 0.0, 1.0) : 1.0;
    if (!ax_enum(s, id, "Blnd", &eid, &ever)) {
        bool pt;
        n.blend = !(ax_bool(s, id, "PasT", &pt) && !pt) && is_container(cls) ? PC_AF_PASS_THROUGH : PC_AF_NORMAL;
    } else {
        bool ok;
        n.blend = blend_from_enum(eid, ever, &ok);
        if (!ok) {
            ax_warn(r, "an unknown blend mode (imported as Normal)");
        }
    }
    if (!ax_affine_is_finite(&world)) {
        ax_warn(r, "a layer with an invalid transform");
        n.kind = PC_AF_UNSUPPORTED;
    } else if ((st = kind(r, id, cls, world, parent, &n, err)) != PC_OK) {
        ax_node_fini(&n);
        return st;
    }
    /* A compound's children are its operands, already merged into its outline. */
    if (!is4(cls, "Comp") &&
        (st = children(r, id, world, depth, &n.children, &n.nchildren, &child_mask, &cm, err)) != PC_OK) {
        ax_node_fini(&n);
        return st;
    }
    if ((st = attached_masks(r, id, world, &n, err)) != PC_OK) {
        if (child_mask) {
            ax_image_fini(&cm);
        }
        ax_node_fini(&n);
        return st;
    }
    if (child_mask) {
        if (n.has_pixel_mask) {
            ax_image_fini(&cm);
        } else {
            n.pixel_mask = cm;
            n.has_pixel_mask = true;
        }
    }
    r->active[id] = 0;
    {
        const pc_af_value *fx = ax_objs(s, id, "FiEf");
        size_t i = 0;
        size_t e;
        while (ax_next_obj(fx, &i, &e)) {
            bool en;
            if (!(ax_bool(s, e, "Enab", &en) && !en)) {
                ax_warn(r, "layer effects (shadows, glows, outlines, bevels\xe2\x80\xa6)");
                break;
            }
        }
    }
    *out = n;
    *has = true;
    return PC_OK;
}

/* Page rectangles: SpMd.PagR[].rctp, or SprB split into PagC equal pages. */
static pc_status pages(const pc_af_stream *s, size_t spread, pc_af_rect **out, size_t *nout, pc_error *err)
{
    size_t md;
    double v[4];
    int64_t n;
    *out = NULL;
    *nout = 0;
    if (ax_obj(s, spread, "SpMd", &md)) {
        const pc_af_value *arr = ax_objs(s, md, "PagR");
        size_t cnt = ax_objs_count(arr);
        size_t i = 0;
        size_t p;
        pc_af_rect *rv = pc_malloc_array(cnt > 0 ? cnt : 1, sizeof *rv);
        size_t k = 0;
        if (rv == NULL) {
            return ax_nomem(err);
        }
        while (ax_next_obj(arr, &i, &p)) {
            if (ax_floats(s, p, "rctp", 4, v)) {
                pc_af_rect r = ax_rect(v);
                if (r.x1 > r.x0 && r.y1 > r.y0) {
                    rv[k++] = r;
                }
            }
        }
        if (k > 0) {
            *out = rv;
            *nout = k;
            return PC_OK;
        }
        free(rv);
    }
    if (ax_floats(s, spread, "SprB", 4, v) && ax_int(s, spread, "PagC", &n) && n >= 2 && n <= 16) {
        pc_af_rect b = ax_rect(v);
        double w = (b.x1 - b.x0) / (double)n;
        pc_af_rect *rv = pc_malloc_array((size_t)n, sizeof *rv);
        int64_t i;
        if (rv == NULL) {
            return ax_nomem(err);
        }
        for (i = 0; i < n; i++) {
            rv[i] = b;
            rv[i].x0 = b.x0 + w * (double)i;
            rv[i].x1 = b.x0 + w * (double)(i + 1);
        }
        *out = rv;
        *nout = (size_t)n;
    }
    return PC_OK;
}

static pc_status document(ax_reader *r, pc_af_document *out, pc_error *err)
{
    const pc_af_stream *s = r->s;
    size_t root = s->root;
    size_t u;
    size_t v;
    size_t doc;
    double d;
    pc_af_document dc;
    size_t cap = 0;
    const pc_af_value *arr;
    size_t i = 0;
    size_t spread;
    pc_status st;
    memset(&dc, 0, sizeof dc);
    r->dpi = ax_obj(s, root, "UVCn", &u) && ax_f64(s, u, "UPPI", &d) && d > 0.0 && d < 100000.0 ? d : 72.0;
    if (ax_obj(s, root, "NVer", &v)) {
        const char *product = ax_str(s, v, "Prod");
        int64_t a;
        int64_t b;
        int64_t c;
        char buf[512];
        if (product == NULL) {
            product = "Affinity";
        }
        if (ax_int(s, v, "Majr", &a) && ax_int(s, v, "Minr", &b) && ax_int(s, v, "Bild", &c)) {
            (void)snprintf(buf, sizeof buf, "%s %lld.%lld.%lld", product, (long long)a, (long long)b, (long long)c);
            dc.saved_by = pc_strndup(buf, strlen(buf));
        } else {
            dc.saved_by = pc_strndup(product, strlen(product));
        }
        if (dc.saved_by == NULL) {
            return ax_nomem(err);
        }
    }
    if (!ax_obj(s, root, "DocR", &doc)) {
        st = ax_malformed(err, "document has no root node");
        goto fail;
    }
    arr = ax_objs(s, doc, "Chld");
    while (ax_next_obj(arr, &i, &spread)) {
        pc_af_spread sp;
        double sz[2];
        double sb[4];
        bool tr;
        bool has_mask;
        pc_af_image mask;
        memset(&sp, 0, sizeof sp);
        if (!ax_is(s, spread, "Sprd")) {
            ax_warn(r, "an unknown kind of page");
            continue;
        }
        if ((st = pages(s, spread, &sp.pages, &sp.npages, err)) != PC_OK) {
            goto fail;
        }
        if (ax_floats(s, spread, "SprB", 4, sb)) {
            sp.bounds = ax_rect(sb);
        } else if (sp.npages > 0) {
            size_t k;
            sp.bounds = sp.pages[0];
            for (k = 1; k < sp.npages; k++) {
                sp.bounds = ax_union(sp.bounds, sp.pages[k]);
            }
        } else if (ax_floats(s, doc, "DfSz", 2, sz) && sz[0] > 0.0 && sz[1] > 0.0) {
            sp.bounds.x0 = 0.0;
            sp.bounds.y0 = 0.0;
            sp.bounds.x1 = sz[0];
            sp.bounds.y1 = sz[1];
        }
        sp.transparent = ax_bool(s, spread, "SprT", &tr) && tr;
        if ((st = children(r, spread, AX_IDENTITY, 0, &sp.nodes, &sp.nnodes, &has_mask, &mask, err)) != PC_OK) {
            free(sp.pages);
            goto fail;
        }
        if (has_mask) {
            ax_image_fini(&mask);
            ax_warn(r, "mask layers directly on a page (imported without them)");
        }
        if (dc.nspreads == cap) {
            size_t c = cap > 0 ? cap * 2 : 4;
            pc_af_spread *g = realloc(dc.spreads, c * sizeof *g);
            if (g == NULL) {
                free(sp.pages);
                nodes_fini(sp.nodes, sp.nnodes);
                st = ax_nomem(err);
                goto fail;
            }
            dc.spreads = g;
            cap = c;
        }
        dc.spreads[dc.nspreads++] = sp;
    }
    if (dc.nspreads == 0) {
        st = ax_malformed(err, "document has no pages");
        goto fail;
    }
    if (ax_objs_count(ax_objs(s, doc, "MpCh")) > 0) {
        ax_warn(r, "master pages (their content is not placed on the pages that use them)");
    }
    if (r->nomem) {
        st = ax_nomem(err);
        goto fail;
    }
    dc.dpi = r->dpi;
    dc.warnings = pc_malloc_array(r->nwarnings > 0 ? r->nwarnings : 1, sizeof *dc.warnings);
    if (dc.warnings == NULL) {
        st = ax_nomem(err);
        goto fail;
    }
    for (i = 0; i < r->nwarnings; i++) {
        const ax_warning *w = &r->warnings[i];
        char *t;
        if (w->count > 1) {
            size_t len = strlen(w->text) + 32;
            t = malloc(len);
            if (t != NULL) {
                (void)snprintf(t, len, "%s (%zu\xc3\x97)", w->text, w->count);
            }
        } else {
            t = pc_strndup(w->text, strlen(w->text));
        }
        if (t == NULL) {
            st = ax_nomem(err);
            goto fail;
        }
        dc.warnings[dc.nwarnings++] = t;
    }
    *out = dc;
    return PC_OK;
fail:
    pc_af_document_fini(&dc);
    return st;
}

pc_status pc_af_read(const uint8_t *b, size_t n, pc_af_limits limits, pc_af_document *out, pc_error *err)
{
    pc_af_archive a;
    uint8_t *doc = NULL;
    size_t ndoc;
    pc_af_stream s;
    ax_reader r;
    size_t i;
    pc_status st;
    if ((st = pc_af_archive_open(b, n, limits, &a, err)) != PC_OK) {
        return st;
    }
    if ((st = pc_af_archive_read(&a, "doc.dat", &doc, &ndoc, err)) != PC_OK) {
        pc_af_archive_fini(&a);
        return st;
    }
    st = pc_af_stream_parse(doc, ndoc, &s, err);
    free(doc);
    if (st != PC_OK) {
        pc_af_archive_fini(&a);
        return st;
    }
    memset(&r, 0, sizeof r);
    r.s = &s;
    r.archive = &a;
    r.dpi = 72.0;
    r.active = calloc(s.nobjects > 0 ? s.nobjects : 1, 1);
    if (r.active == NULL) {
        st = ax_nomem(err);
    } else {
        st = document(&r, out, err);
    }
    free(r.active);
    for (i = 0; i < r.nwarnings; i++) {
        free(r.warnings[i].text);
    }
    free(r.warnings);
    pc_af_stream_fini(&s);
    pc_af_archive_fini(&a);
    return st;
}
