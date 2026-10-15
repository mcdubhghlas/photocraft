/* Snapping and smart guides (port of crates/engine/src/snap.rs). Pure geometry, except
 * rect_of(), which asks the transform_bounds hook for a layer's Free-Transform bounds. */
#include <math.h>
#include <stdlib.h>

#include "base/mem.h"
#include "engine_internal.h"
#include "snap.h"

bool pc_snap_kind_is_smart(pc_snap_kind k)
{
    return k == PC_SNAP_LAYER_EDGE || k == PC_SNAP_LAYER_CENTER || k == PC_SNAP_DOCUMENT_CENTER;
}

pc_snap_options pc_snap_options_default(void)
{
    pc_snap_options o;
    o.guides = true;
    o.grid = false;
    o.layers = true;
    o.document = true;
    o.selection = true;
    o.grid_step[0] = 18.0;
    o.grid_step[1] = 18.0;
    return o;
}

void pc_snap_targets_init(pc_snap_targets *t)
{
    t->x = NULL;
    t->nx = 0;
    t->cap_x = 0;
    t->y = NULL;
    t->ny = 0;
    t->cap_y = 0;
    t->has_grid = false;
    t->grid[0] = 0.0;
    t->grid[1] = 0.0;
    t->extent[0] = 0.0;
    t->extent[1] = 0.0;
    t->extent[2] = 0.0;
    t->extent[3] = 0.0;
}

void pc_snap_targets_fini(pc_snap_targets *t)
{
    if (t == NULL) {
        return;
    }
    free(t->x);
    free(t->y);
    pc_snap_targets_init(t);
}

bool pc_snap_targets_is_empty(const pc_snap_targets *t)
{
    return t->nx == 0 && t->ny == 0 && !t->has_grid;
}

static bool push_target(pc_snap_target **arr, size_t *n, size_t *cap, pc_snap_target v)
{
    pc_snap_target *grown = pc_grow_array(*arr, cap, *n + 1, sizeof **arr);
    if (grown == NULL) {
        return false;
    }
    *arr = grown;
    (*arr)[(*n)++] = v;
    return true;
}

pc_status pc_snap_targets_add_rect(pc_snap_targets *t, const double r[4], pc_snap_kind edge,
                                   pc_snap_kind center, pc_error *err)
{
    double xs[2] = {r[1], r[3]};
    double ys[2] = {r[0], r[2]};
    pc_snap_target tx0 = {r[0], edge, {xs[0], xs[1]}};
    pc_snap_target tx1 = {r[2], edge, {xs[0], xs[1]}};
    pc_snap_target txc = {(r[0] + r[2]) / 2.0, center, {xs[0], xs[1]}};
    pc_snap_target ty0 = {r[1], edge, {ys[0], ys[1]}};
    pc_snap_target ty1 = {r[3], edge, {ys[0], ys[1]}};
    pc_snap_target tyc = {(r[1] + r[3]) / 2.0, center, {ys[0], ys[1]}};
    if (!push_target(&t->x, &t->nx, &t->cap_x, tx0) || !push_target(&t->x, &t->nx, &t->cap_x, tx1) ||
        !push_target(&t->x, &t->nx, &t->cap_x, txc) || !push_target(&t->y, &t->ny, &t->cap_y, ty0) ||
        !push_target(&t->y, &t->ny, &t->cap_y, ty1) || !push_target(&t->y, &t->ny, &t->cap_y, tyc)) {
        return pc_fail(err, PC_ERR_NOMEM, "snap targets");
    }
    return PC_OK;
}

/* Bounds of a layer for snapping; false for empty layers, groups and maskless adjustment layers. */
static bool rect_of(const pc_document *doc, const pc_layer *l, double out[4])
{
    if (pc_layer_is_group(l) || (l->content.kind == PC_LAYER_ADJUSTMENT && !l->has_mask)) {
        return false;
    }
    pc_rect b = pc_engine_hook_transform_bounds(doc, l);
    if (pc_rect_is_empty(b)) {
        return false;
    }
    out[0] = (double)b.x0;
    out[1] = (double)b.y0;
    out[2] = (double)b.x1;
    out[3] = (double)b.y1;
    return true;
}

bool pc_snap_layer_rect(const pc_document *doc, pc_layer_id id, double out[4])
{
    const pc_layer *l = pc_document_layer(doc, id);
    return l != NULL && rect_of(doc, l, out);
}

pc_status pc_snap_targets_from_document(const pc_document *doc, const pc_snap_options *opts,
                                        const pc_layer_id *exclude, size_t nexclude, pc_snap_targets *out,
                                        pc_error *err)
{
    pc_snap_targets_init(out);
    double w = (double)doc->size.width, h = (double)doc->size.height;
    out->extent[0] = 0.0;
    out->extent[1] = 0.0;
    out->extent[2] = w;
    out->extent[3] = h;
    if (opts->document) {
        double r[4] = {0.0, 0.0, w, h};
        if (pc_snap_targets_add_rect(out, r, PC_SNAP_DOCUMENT_EDGE, PC_SNAP_DOCUMENT_CENTER, err) != PC_OK) {
            goto fail;
        }
    }
    if (opts->guides) {
        for (size_t i = 0; i < doc->guides.nvertical; i++) {
            pc_snap_target v = {doc->guides.vertical[i], PC_SNAP_GUIDE, {0.0, h}};
            if (!push_target(&out->x, &out->nx, &out->cap_x, v)) {
                goto nomem;
            }
        }
        for (size_t i = 0; i < doc->guides.nhorizontal; i++) {
            pc_snap_target v = {doc->guides.horizontal[i], PC_SNAP_GUIDE, {0.0, w}};
            if (!push_target(&out->y, &out->ny, &out->cap_y, v)) {
                goto nomem;
            }
        }
    }
    if (opts->grid && isfinite(opts->grid_step[0]) && opts->grid_step[0] > 0.0 &&
        isfinite(opts->grid_step[1]) && opts->grid_step[1] > 0.0) {
        out->has_grid = true;
        out->grid[0] = opts->grid_step[0];
        out->grid[1] = opts->grid_step[1];
    }
    if (opts->layers) {
        pc_walk wk = {0};
        if (pc_document_walk(doc, &wk, err) != PC_OK) {
            goto fail;
        }
        for (size_t i = 0; i < wk.len; i++) {
            const pc_layer *l = wk.items[i].layer;
            if (!l->visible) {
                continue;
            }
            bool excluded = false;
            for (size_t e = 0; e < nexclude; e++) {
                if (exclude[e] == l->id) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) {
                continue;
            }
            double r[4];
            if (rect_of(doc, l, r)) {
                if (pc_snap_targets_add_rect(out, r, PC_SNAP_LAYER_EDGE, PC_SNAP_LAYER_CENTER, err) != PC_OK) {
                    pc_walk_fini(&wk);
                    goto fail;
                }
            }
        }
        pc_walk_fini(&wk);
    }
    if (opts->selection && doc->has_selection) {
        pc_rect b = pc_surface_content_bounds(&doc->selection);
        if (!pc_rect_is_empty(b)) {
            double r[4] = {(double)b.x0, (double)b.y0, (double)b.x1, (double)b.y1};
            pc_snap_target a0 = {r[0], PC_SNAP_SELECTION, {r[1], r[3]}};
            pc_snap_target a1 = {r[2], PC_SNAP_SELECTION, {r[1], r[3]}};
            pc_snap_target b0 = {r[1], PC_SNAP_SELECTION, {r[0], r[2]}};
            pc_snap_target b1 = {r[3], PC_SNAP_SELECTION, {r[0], r[2]}};
            if (!push_target(&out->x, &out->nx, &out->cap_x, a0) ||
                !push_target(&out->x, &out->nx, &out->cap_x, a1) ||
                !push_target(&out->y, &out->ny, &out->cap_y, b0) ||
                !push_target(&out->y, &out->ny, &out->cap_y, b1)) {
                goto nomem;
            }
        }
    }
    return PC_OK;
nomem:
    (void)pc_fail(err, PC_ERR_NOMEM, "snap targets");
fail:
    pc_snap_targets_fini(out);
    return PC_ERR_NOMEM;
}

bool pc_snap_targets_nearest(const pc_snap_targets *t, bool vertical, double v, double tol,
                             pc_snap_target *out)
{
    const pc_snap_target *list = vertical ? t->x : t->y;
    size_t n = vertical ? t->nx : t->ny;
    bool have = false;
    double best_d = 0.0;
    pc_snap_target best = {0.0, PC_SNAP_GUIDE, {0.0, 0.0}};
    for (size_t i = 0; i < n; i++) {
        double d = fabs(list[i].pos - v);
        if (d > tol) {
            continue;
        }
        bool better = !have || d < best_d - 1e-9 ||
                      (fabs(d - best_d) <= 1e-9 && list[i].kind == PC_SNAP_GUIDE && best.kind != PC_SNAP_GUIDE);
        if (better) {
            have = true;
            best_d = d;
            best = list[i];
        }
    }
    if (t->has_grid) {
        double step = t->grid[vertical ? 0 : 1];
        double g = round(v / step) * step;
        double d = fabs(g - v);
        if (d <= tol) {
            double span0 = vertical ? t->extent[1] : t->extent[0];
            double span1 = vertical ? t->extent[3] : t->extent[2];
            pc_snap_target gt = {g, PC_SNAP_GRID, {span0, span1}};
            bool better = !have || d < best_d - 1e-9 ||
                          (fabs(d - best_d) <= 1e-9 && gt.kind == PC_SNAP_GUIDE && best.kind != PC_SNAP_GUIDE);
            if (better) {
                have = true;
                best = gt; /* grid is the last consideration: best_d need not be updated */
            }
        }
    }
    if (have && out != NULL) {
        *out = best;
    }
    return have;
}

static pc_snap_line make_line(bool vertical, pc_snap_target t, double a, double b)
{
    pc_snap_line l;
    l.vertical = vertical;
    l.pos = t.pos;
    l.from = fmin(fmin(t.span[0], a), b);
    l.to = fmax(fmax(t.span[1], a), b);
    l.kind = t.kind;
    return l;
}

static bool push_line(pc_snap_line **arr, size_t *n, size_t *cap, pc_snap_line v)
{
    pc_snap_line *grown = pc_grow_array(*arr, cap, *n + 1, sizeof **arr);
    if (grown == NULL) {
        return false;
    }
    *arr = grown;
    (*arr)[(*n)++] = v;
    return true;
}

pc_status pc_snap_targets_snap_point(const pc_snap_targets *t, const double p[2], double tol,
                                     double out_p[2], pc_snap_line **lines, size_t *nlines, pc_error *err)
{
    out_p[0] = p[0];
    out_p[1] = p[1];
    *lines = NULL;
    *nlines = 0;
    size_t cap = 0;
    pc_snap_target tx;
    if (pc_snap_targets_nearest(t, true, p[0], tol, &tx)) {
        out_p[0] = tx.pos;
        if (!push_line(lines, nlines, &cap, make_line(true, tx, p[1], p[1]))) {
            goto nomem;
        }
    }
    pc_snap_target ty;
    if (pc_snap_targets_nearest(t, false, p[1], tol, &ty)) {
        out_p[1] = ty.pos;
        if (!push_line(lines, nlines, &cap, make_line(false, ty, out_p[0], out_p[0]))) {
            goto nomem;
        }
    }
    return PC_OK;
nomem:
    free(*lines);
    *lines = NULL;
    *nlines = 0;
    return pc_fail(err, PC_ERR_NOMEM, "snap lines");
}

pc_status pc_snap_targets_snap_rect(const pc_snap_targets *t, const double r[4], double tol,
                                    double out_delta[2], pc_snap_line **lines, size_t *nlines, pc_error *err)
{
    out_delta[0] = 0.0;
    out_delta[1] = 0.0;
    *lines = NULL;
    *nlines = 0;
    size_t cap = 0;
    for (int ax = 0; ax < 2; ax++) {
        bool vertical = ax == 0;
        double a = r[ax], b = r[ax + 2];
        double anchors[3] = {a, (a + b) / 2.0, b};
        bool have = false;
        double best_abs = 0.0, best_d = 0.0;
        for (int k = 0; k < 3; k++) {
            pc_snap_target tgt;
            if (pc_snap_targets_nearest(t, vertical, anchors[k], tol, &tgt)) {
                double d = tgt.pos - anchors[k];
                if (!have || fabs(d) < best_abs) {
                    have = true;
                    best_abs = fabs(d);
                    best_d = d;
                }
            }
        }
        if (!have) {
            continue;
        }
        out_delta[ax] = best_d;
        double other0 = vertical ? r[1] : r[0];
        double other1 = vertical ? r[3] : r[2];
        const pc_snap_target *list = vertical ? t->x : t->y;
        size_t n = vertical ? t->nx : t->ny;
        for (int k = 0; k < 3; k++) {
            double moved = anchors[k] + best_d;
            bool hit = false;
            pc_snap_target ht = {0.0, PC_SNAP_GUIDE, {0.0, 0.0}};
            /* Prefer a guide among equal hits (min_by_key kind != Guide). */
            for (size_t i = 0; i < n; i++) {
                if (fabs(list[i].pos - moved) < 1e-6) {
                    if (!hit || (list[i].kind == PC_SNAP_GUIDE && ht.kind != PC_SNAP_GUIDE)) {
                        hit = true;
                        ht = list[i];
                    }
                }
            }
            if (!hit && t->has_grid) {
                double step = t->grid[ax];
                if (fabs(round(moved / step) * step - moved) < 1e-6) {
                    hit = true;
                    ht.pos = moved;
                    ht.kind = PC_SNAP_GRID;
                    ht.span[0] = other0;
                    ht.span[1] = other1;
                }
            }
            if (hit) {
                if (!push_line(lines, nlines, &cap, make_line(vertical, ht, other0, other1))) {
                    free(*lines);
                    *lines = NULL;
                    *nlines = 0;
                    return pc_fail(err, PC_ERR_NOMEM, "snap lines");
                }
            }
        }
    }
    return PC_OK;
}

bool pc_snap_union(const double (*rects)[4], size_t n, double out[4])
{
    if (n == 0) {
        return false;
    }
    out[0] = rects[0][0];
    out[1] = rects[0][1];
    out[2] = rects[0][2];
    out[3] = rects[0][3];
    for (size_t i = 1; i < n; i++) {
        out[0] = fmin(out[0], rects[i][0]);
        out[1] = fmin(out[1], rects[i][1]);
        out[2] = fmax(out[2], rects[i][2]);
        out[3] = fmax(out[3], rects[i][3]);
    }
    return true;
}
