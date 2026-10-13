/* Vector data (port of crates/doc/src/vector.rs). */
#include "doc/vector.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/rmath.h"

static bool point_eq(pc_point a, pc_point b)
{
    return a.x == b.x && a.y == b.y;
}

/* ---- knots ---- */

pc_knot pc_knot_corner(double x, double y)
{
    pc_knot k;
    pc_point p = pc_point_make(x, y);
    k.anchor = p;
    k.in_ctrl = p;
    k.out_ctrl = p;
    k.smooth = false;
    return k;
}

pc_knot pc_knot_smooth(pc_point anchor, pc_point in_ctrl, pc_point out_ctrl)
{
    pc_knot k;
    k.anchor = anchor;
    k.in_ctrl = in_ctrl;
    k.out_ctrl = out_ctrl;
    k.smooth = true;
    return k;
}

pc_knot pc_knot_transform(pc_knot k, pc_affine a)
{
    pc_knot t;
    t.anchor = pc_affine_apply(a, k.anchor);
    t.in_ctrl = pc_affine_apply(a, k.in_ctrl);
    t.out_ctrl = pc_affine_apply(a, k.out_ctrl);
    t.smooth = k.smooth;
    return t;
}

bool pc_knot_eq(const pc_knot *a, const pc_knot *b)
{
    return point_eq(a->anchor, b->anchor) && point_eq(a->in_ctrl, b->in_ctrl) &&
           point_eq(a->out_ctrl, b->out_ctrl) && a->smooth == b->smooth;
}

/* ---- subpaths ---- */

void pc_subpath_fini(pc_subpath *s)
{
    if (s == NULL) {
        return;
    }
    free(s->knots);
    memset(s, 0, sizeof *s);
}

pc_status pc_subpath_clone(const pc_subpath *s, pc_subpath *out, pc_error *err)
{
    pc_subpath t = *s;
    t.knots = NULL;
    if (s->nknots > 0) {
        t.knots = pc_malloc_array(s->nknots, sizeof *t.knots);
        if (t.knots == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(t.knots, s->knots, s->nknots * sizeof *t.knots);
    }
    *out = t;
    return PC_OK;
}

bool pc_subpath_eq(const pc_subpath *a, const pc_subpath *b)
{
    size_t i;
    if (a->closed != b->closed || a->nknots != b->nknots || a->op != b->op) {
        return false;
    }
    for (i = 0; i < a->nknots; i++) {
        if (!pc_knot_eq(&a->knots[i], &b->knots[i])) {
            return false;
        }
    }
    return true;
}

static pc_status subpath_through(const double (*pts)[2], size_t n, bool closed, pc_subpath *out, pc_error *err)
{
    pc_subpath t;
    size_t i;
    memset(&t, 0, sizeof t);
    t.closed = closed;
    t.op = PC_PATH_COMBINE;
    if (n > 0) {
        t.knots = pc_malloc_array(n, sizeof *t.knots);
        if (t.knots == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < n; i++) {
            t.knots[i] = pc_knot_corner(pts[i][0], pts[i][1]);
        }
        t.nknots = n;
    }
    *out = t;
    return PC_OK;
}

pc_status pc_subpath_polygon(const double (*pts)[2], size_t n, pc_subpath *out, pc_error *err)
{
    return subpath_through(pts, n, true, out, err);
}

pc_status pc_subpath_polyline(const double (*pts)[2], size_t n, pc_subpath *out, pc_error *err)
{
    return subpath_through(pts, n, false, out, err);
}

size_t pc_subpath_segment_count(const pc_subpath *s)
{
    if (s->nknots < 2) {
        return 0;
    }
    return s->closed ? s->nknots : s->nknots - 1;
}

void pc_subpath_segment(const pc_subpath *s, size_t i, pc_point out[4])
{
    const pc_knot *a;
    const pc_knot *b;
    if (i >= pc_subpath_segment_count(s)) {
        /* No such segment: zero points rather than reading past the knots. */
        memset(out, 0, 4 * sizeof *out);
        return;
    }
    a = &s->knots[i];
    b = &s->knots[(i + 1) % s->nknots];
    out[0] = a->anchor;
    out[1] = a->out_ctrl;
    out[2] = b->in_ctrl;
    out[3] = b->anchor;
}

/* ---- paths ---- */

void pc_path_fini(pc_path *p)
{
    size_t i;
    if (p == NULL) {
        return;
    }
    for (i = 0; i < p->nsubpaths; i++) {
        pc_subpath_fini(&p->subpaths[i]);
    }
    free(p->subpaths);
    memset(p, 0, sizeof *p);
}

pc_status pc_path_clone(const pc_path *p, pc_path *out, pc_error *err)
{
    pc_path t = *p;
    size_t i;
    t.subpaths = NULL;
    t.nsubpaths = 0;
    if (p->nsubpaths > 0) {
        t.subpaths = pc_calloc_array(p->nsubpaths, sizeof *t.subpaths);
        if (t.subpaths == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        for (i = 0; i < p->nsubpaths; i++) {
            pc_status st = pc_subpath_clone(&p->subpaths[i], &t.subpaths[i], err);
            if (st != PC_OK) {
                pc_path_fini(&t); /* nsubpaths counts the clones made so far */
                return st;
            }
            t.nsubpaths = i + 1;
        }
    }
    *out = t;
    return PC_OK;
}

bool pc_path_eq(const pc_path *a, const pc_path *b)
{
    size_t i;
    if (a->nsubpaths != b->nsubpaths || a->fill_rule != b->fill_rule || a->inverted != b->inverted) {
        return false;
    }
    for (i = 0; i < a->nsubpaths; i++) {
        if (!pc_subpath_eq(&a->subpaths[i], &b->subpaths[i])) {
            return false;
        }
    }
    return true;
}

bool pc_path_is_empty(const pc_path *p)
{
    size_t i;
    for (i = 0; i < p->nsubpaths; i++) {
        if (p->subpaths[i].nknots > 0) {
            return false;
        }
    }
    return true;
}

pc_path_op pc_path_effective_op(const pc_path *p, size_t i)
{
    /* The reference indexes (and would panic) past the end; Combine is the harmless answer. */
    if (i == 0 || i >= p->nsubpaths) {
        return PC_PATH_COMBINE;
    }
    return p->subpaths[i].op;
}

pc_status pc_path_components(const pc_path *p, pc_index_range **out, size_t *n, pc_error *err)
{
    pc_index_range *r = NULL;
    size_t len = 0;
    size_t i;
    if (p->nsubpaths > 0) {
        r = pc_malloc_array(p->nsubpaths, sizeof *r);
        if (r == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    for (i = 0; i < p->nsubpaths; i++) {
        if (len > 0 && p->subpaths[i].op == PC_PATH_JOIN) {
            r[len - 1].end = i + 1;
        } else {
            r[len].start = i;
            r[len].end = i + 1;
            len++;
        }
    }
    *out = r;
    *n = len;
    return PC_OK;
}

pc_status pc_path_transform(const pc_path *p, pc_affine a, pc_path *out, pc_error *err)
{
    pc_path t;
    size_t i;
    size_t k;
    pc_status st = pc_path_clone(p, &t, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < t.nsubpaths; i++) {
        for (k = 0; k < t.subpaths[i].nknots; k++) {
            t.subpaths[i].knots[k] = pc_knot_transform(t.subpaths[i].knots[k], a);
        }
    }
    *out = t;
    return PC_OK;
}

bool pc_path_control_bounds(const pc_path *p, double out[4])
{
    double b[4] = {0.0, 0.0, 0.0, 0.0};
    bool first = true;
    size_t i;
    size_t k;
    int j;
    for (i = 0; i < p->nsubpaths; i++) {
        for (k = 0; k < p->subpaths[i].nknots; k++) {
            const pc_knot *kn = &p->subpaths[i].knots[k];
            pc_point pts[3];
            pts[0] = kn->anchor;
            pts[1] = kn->in_ctrl;
            pts[2] = kn->out_ctrl;
            for (j = 0; j < 3; j++) {
                if (first) {
                    b[0] = b[2] = pts[j].x;
                    b[1] = b[3] = pts[j].y;
                    first = false;
                } else {
                    b[0] = pc_rs_min(b[0], pts[j].x);
                    b[1] = pc_rs_min(b[1], pts[j].y);
                    b[2] = pc_rs_max(b[2], pts[j].x);
                    b[3] = pc_rs_max(b[3], pts[j].y);
                }
            }
        }
    }
    if (first) {
        return false;
    }
    memcpy(out, b, sizeof b);
    return true;
}

/* ---- named and clipping paths, vector masks ---- */

void pc_named_path_fini(pc_named_path *p)
{
    if (p == NULL) {
        return;
    }
    pc_str_fini(&p->name);
    pc_path_fini(&p->path);
    pc_blob_release(p->psd_raw);
    memset(p, 0, sizeof *p);
}

pc_status pc_named_path_clone(const pc_named_path *p, pc_named_path *out, pc_error *err)
{
    pc_named_path t;
    pc_status st;
    memset(&t, 0, sizeof t);
    st = pc_str_clone(&p->name, &t.name, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_path_clone(&p->path, &t.path, err);
    if (st != PC_OK) {
        pc_str_fini(&t.name);
        return st;
    }
    t.psd_raw = pc_blob_retain(p->psd_raw);
    *out = t;
    return PC_OK;
}

bool pc_named_path_eq(const pc_named_path *a, const pc_named_path *b)
{
    return pc_str_eq(&a->name, &b->name) && pc_path_eq(&a->path, &b->path) && pc_blob_eq(a->psd_raw, b->psd_raw);
}

void pc_clipping_path_fini(pc_clipping_path *c)
{
    if (c == NULL) {
        return;
    }
    pc_str_fini(&c->name);
    c->flatness = 0.0f;
}

pc_status pc_clipping_path_clone(const pc_clipping_path *c, pc_clipping_path *out, pc_error *err)
{
    pc_clipping_path t = *c;
    pc_status st = pc_str_clone(&c->name, &t.name, err);
    if (st != PC_OK) {
        return st;
    }
    *out = t;
    return PC_OK;
}

bool pc_clipping_path_eq(const pc_clipping_path *a, const pc_clipping_path *b)
{
    return pc_str_eq(&a->name, &b->name) && a->flatness == b->flatness;
}

void pc_vector_mask_init(pc_vector_mask *out, pc_path *path)
{
    out->path = *path;
    memset(path, 0, sizeof *path);
    out->enabled = true;
    out->linked = true;
    out->density = 1.0f;
    out->feather = 0.0f;
}

void pc_vector_mask_fini(pc_vector_mask *m)
{
    if (m == NULL) {
        return;
    }
    pc_path_fini(&m->path);
    memset(m, 0, sizeof *m);
}

pc_status pc_vector_mask_clone(const pc_vector_mask *m, pc_vector_mask *out, pc_error *err)
{
    pc_vector_mask t = *m;
    pc_status st = pc_path_clone(&m->path, &t.path, err);
    if (st != PC_OK) {
        return st;
    }
    *out = t;
    return PC_OK;
}

bool pc_vector_mask_eq(const pc_vector_mask *a, const pc_vector_mask *b)
{
    return pc_path_eq(&a->path, &b->path) && a->enabled == b->enabled && a->linked == b->linked &&
           a->density == b->density && a->feather == b->feather;
}

/* ---- shape stroke ---- */

void pc_shape_stroke_default(pc_shape_stroke *out)
{
    memset(out, 0, sizeof *out);
    out->width = 3.0f;
    out->paint = pc_fill_solid(PC_COLOR_BLACK);
    out->opacity = 1.0f;
    out->align = PC_STROKE_ALIGN_CENTER;
    out->cap = PC_LINE_CAP_BUTT;
    out->join = PC_LINE_JOIN_MITER;
    out->miter_limit = 100.0f;
    out->dashes = NULL;
    out->ndashes = 0;
    out->dash_offset = 0.0f;
}

void pc_shape_stroke_fini(pc_shape_stroke *s)
{
    if (s == NULL) {
        return;
    }
    pc_fill_fini(&s->paint);
    free(s->dashes);
    memset(s, 0, sizeof *s);
}

pc_status pc_shape_stroke_clone(const pc_shape_stroke *s, pc_shape_stroke *out, pc_error *err)
{
    pc_shape_stroke t = *s;
    pc_status st;
    t.dashes = NULL;
    if (s->ndashes > 0) {
        t.dashes = pc_malloc_array(s->ndashes, sizeof *t.dashes);
        if (t.dashes == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(t.dashes, s->dashes, s->ndashes * sizeof *t.dashes);
    }
    st = pc_fill_clone(&s->paint, &t.paint, err);
    if (st != PC_OK) {
        free(t.dashes);
        return st;
    }
    *out = t;
    return PC_OK;
}

bool pc_shape_stroke_eq(const pc_shape_stroke *a, const pc_shape_stroke *b)
{
    size_t i;
    if (a->ndashes != b->ndashes) {
        return false;
    }
    for (i = 0; i < a->ndashes; i++) {
        if (a->dashes[i] != b->dashes[i]) {
            return false;
        }
    }
    return a->width == b->width && pc_fill_eq(&a->paint, &b->paint) && a->opacity == b->opacity &&
           a->align == b->align && a->cap == b->cap && a->join == b->join && a->miter_limit == b->miter_limit &&
           a->dash_offset == b->dash_offset;
}

/* ---- live shapes and shape layers ---- */

static bool doubles_eq(const double *a, const double *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

bool pc_live_shape_eq(const pc_live_shape *a, const pc_live_shape *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_LIVE_RECT:
        return doubles_eq(a->rect, b->rect, 4) && doubles_eq(a->radii, b->radii, 4);
    case PC_LIVE_ELLIPSE:
        return doubles_eq(a->rect, b->rect, 4);
    case PC_LIVE_POLYGON:
        return doubles_eq(a->rect, b->rect, 4) && a->sides == b->sides && a->star_ratio == b->star_ratio;
    case PC_LIVE_LINE:
        return doubles_eq(a->from, b->from, 2) && doubles_eq(a->to, b->to, 2) && a->weight == b->weight;
    }
    return false;
}

void pc_shape_layer_fini(pc_shape_layer *s)
{
    if (s == NULL) {
        return;
    }
    pc_path_fini(&s->path);
    pc_fill_fini(&s->fill);
    pc_shape_stroke_fini(&s->stroke);
    pc_surface_fini(&s->cache);
    pc_blob_release(s->psd_raw);
    memset(s, 0, sizeof *s);
}

pc_status pc_shape_layer_clone(const pc_shape_layer *s, pc_shape_layer *out, pc_error *err)
{
    pc_shape_layer t;
    pc_status st;
    memset(&t, 0, sizeof t);
    st = pc_path_clone(&s->path, &t.path, err);
    if (st == PC_OK && s->has_fill) {
        st = pc_fill_clone(&s->fill, &t.fill, err);
        t.has_fill = st == PC_OK;
    }
    if (st == PC_OK && s->has_stroke) {
        st = pc_shape_stroke_clone(&s->stroke, &t.stroke, err);
        t.has_stroke = st == PC_OK;
    }
    if (st == PC_OK && s->has_cache) {
        st = pc_surface_clone(&s->cache, &t.cache, err);
        t.has_cache = st == PC_OK;
    }
    if (st != PC_OK) {
        pc_shape_layer_fini(&t);
        return st;
    }
    t.has_live = s->has_live;
    t.live = s->live;
    t.psd_raw = pc_blob_retain(s->psd_raw);
    *out = t;
    return PC_OK;
}

bool pc_shape_layer_eq(const pc_shape_layer *a, const pc_shape_layer *b)
{
    if (!pc_path_eq(&a->path, &b->path)) {
        return false;
    }
    if (a->has_fill != b->has_fill || (a->has_fill && !pc_fill_eq(&a->fill, &b->fill))) {
        return false;
    }
    if (a->has_stroke != b->has_stroke || (a->has_stroke && !pc_shape_stroke_eq(&a->stroke, &b->stroke))) {
        return false;
    }
    if (a->has_live != b->has_live || (a->has_live && !pc_live_shape_eq(&a->live, &b->live))) {
        return false;
    }
    if (a->has_cache != b->has_cache || (a->has_cache && !pc_surface_eq(&a->cache, &b->cache))) {
        return false;
    }
    return pc_blob_eq(a->psd_raw, b->psd_raw);
}

pc_status pc_shape_stroke_validate(const pc_shape_stroke *s, pc_error *err)
{
    float sum = -0.0f;
    if (!isfinite(s->width) || !(s->width >= 0.0f && s->width <= 1000000.0f)) {
        return pc_fail(err, PC_ERR_INVALID, "stroke width must be finite and between 0 and 1000000 px");
    }
    if (!isfinite(s->opacity) || !(s->opacity >= 0.0f && s->opacity <= 1.0f)) {
        return pc_fail(err, PC_ERR_INVALID, "stroke opacity must be between 0 and 100%%");
    }
    if (!isfinite(s->miter_limit) || !(s->miter_limit >= 1.0f && s->miter_limit <= 500.0f)) {
        return pc_fail(err, PC_ERR_INVALID, "miter limit must be between 1 and 500");
    }
    if (!isfinite(s->dash_offset) || fabsf(s->dash_offset) > 1000000.0f) {
        return pc_fail(err, PC_ERR_INVALID,
                       "dash offset must be finite and within \xc2\xb1"
                       "1000000 widths");
    }
    for (size_t i = 0; i < s->ndashes; i++) {
        float d = s->dashes[i];
        if (s->ndashes > 32 || !isfinite(d) || !(d >= 0.0f && d <= 10000.0f)) {
            return pc_fail(err, PC_ERR_INVALID,
                           "dash pattern must contain at most 32 finite lengths between 0 and 10000 widths");
        }
        sum += d;
    }
    if (s->ndashes > 0 && sum < 0.01f) {
        return pc_fail(err, PC_ERR_INVALID, "dash pattern total must be at least 0.01 widths");
    }
    return PC_OK;
}
