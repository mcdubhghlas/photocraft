/* SVG import (port of crates/io/src/svg.rs): a drawing opens as a document of shape layers, and
 * places as a vector smart object re-rasterised at its placement scale (pc_io_svg_rasterize).
 *
 * Parsing (svg_xml.c .. svg_paint.c) normalises the drawing as usvg does: CSS, <use>, units,
 * nested transforms. Each path becomes a shape layer (cubic knots, fill, stroke) and each group a
 * layer group with its opacity and blend mode. What the shape model cannot hold -- clip paths,
 * masks, filters, pattern paints and embedded images -- is rasterised (svg_render.c, resvg's
 * rendering on tiny-skia's pipeline) into a pixel layer, with a warning; gradient paints keep their stops and
 * direction. */
#include "io/svg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "io/svg_internal.h"
#include "raster/pixel.h"
#include "vector/vector.h"

/* A rasterised fallback layer or a smart-object render never exceeds this many pixels. */
#define MAX_RASTER_PIXELS ((uint64_t)24000000)
/* Past this many drawable elements the whole drawing opens as one pixel layer. */
#define MAX_NODES 2000u
/* Nesting past which a subtree is rasterised instead of walked (bounded recursion). */
#define MAX_RECURSION 200u

typedef struct lvec {
    pc_layer *v;
    size_t n, cap;
} lvec;

static void lvec_fini(lvec *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        pc_layer_fini(&l->v[i]);
    }
    free(l->v);
    memset(l, 0, sizeof *l);
}

static pc_status lvec_push(lvec *l, pc_layer *layer, pc_error *err)
{
    if (l->n == l->cap) {
        size_t cap = l->cap == 0 ? 4 : l->cap * 2;
        pc_layer *v = cap > SIZE_MAX / sizeof *v ? NULL : realloc(l->v, cap * sizeof *v);
        if (v == NULL) {
            pc_layer_fini(layer);
            return pc_io_nomem(err, "importing an SVG");
        }
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = *layer;
    memset(layer, 0, sizeof *layer);
    return PC_OK;
}

typedef struct conv {
    pc_pixel_format fmt;
    pc_rect canvas;
    sv_ts global;
    pc_io_warnings *warnings;
    const char *noted[16];
    size_t nnoted;
    size_t counts[4];
} conv;

static pc_status note(conv *c, const char *what, const char *msg, pc_error *err)
{
    size_t i;
    for (i = 0; i < c->nnoted; i++) {
        if (strcmp(c->noted[i], what) == 0) {
            return PC_OK;
        }
    }
    if (c->nnoted < sizeof c->noted / sizeof c->noted[0]) {
        c->noted[c->nnoted++] = what;
    }
    return pc_io_warn(c->warnings, err, "%s", msg);
}

/* The element's id, or a numbered name by kind (0 shape, 1 group, 2 image, 3 text). */
static void layer_name(conv *c, const char *id, size_t kind, char *out, size_t cap)
{
    static const char *const L[4] = {"Shape", "Group", "Image", "Text"};
    if (id != NULL && id[0] != '\0') {
        (void)snprintf(out, cap, "%s", id);
        return;
    }
    c->counts[kind]++;
    (void)snprintf(out, cap, "%s %zu", L[kind], c->counts[kind]);
}

/* tiny-skia's PremultipliedColorU8::demultiply, as floats. */
static void demultiply(const uint8_t p[4], float out[4])
{
    uint8_t a = p[3];
    int k;
    for (k = 0; k < 3; k++) {
        uint8_t v = p[k];
        if (a != 255) {
            double af = (double)a / 255.0;
            v = pc_f64_to_u8((double)p[k] / af + 0.5);
        }
        out[k] = (float)v / 255.0f;
    }
    out[3] = (float)a / 255.0f;
}

/* A transparent pixmap, or false past MAX_RASTER_PIXELS (or out of memory). */
static bool pixmap(uint32_t w, uint32_t h, sv_pixmap *pm)
{
    if ((uint64_t)w * h > MAX_RASTER_PIXELS) {
        return false;
    }
    return sv_pixmap_new(w, h, pm);
}

/* A pixmap as a surface covering rect in fmt. */
static pc_status pm_surface(const sv_pixmap *pm, pc_rect rect, pc_pixel_format fmt, pc_surface *out,
                            pc_error *err)
{
    size_t nch = pc_pixel_format_channels(fmt);
    size_t n = (size_t)pm->w * pm->h;
    float *data = calloc(n * nch, sizeof(float));
    pc_surface s;
    size_t i;
    pc_status st = PC_OK;
    if (data == NULL) {
        return pc_io_nomem(err, "rasterising an SVG");
    }
    for (i = 0; i < n; i++) {
        float rgba[4];
        float px[PC_RASTER_MAX_CHANNELS];
        demultiply(pm->px + i * 4, rgba);
        (void)pc_from_rgba(fmt, rgba, px);
        memcpy(data + i * nch, px, nch * sizeof(float));
    }
    pc_surface_init(&s, fmt);
    if (!pc_rect_is_empty(rect)) {
        st = pc_surface_write_region(&s, rect, data, n * nch, err);
    }
    free(data);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    pc_surface_prune(&s);
    *out = s;
    return PC_OK;
}

/* Drawable elements in the tree (iterative). */
static size_t count_nodes(const sv_unode *root)
{
    const sv_unode **stack;
    size_t sn = 0;
    size_t cap = 64;
    size_t n = 0;
    stack = malloc(cap * sizeof *stack);
    if (stack == NULL) {
        return MAX_NODES + 1;
    }
    stack[sn++] = root;
    while (sn > 0) {
        const sv_unode *g = stack[--sn];
        size_t i;
        for (i = 0; i < g->nchildren; i++) {
            n++;
            if (g->children[i]->kind == UN_GROUP) {
                if (sn == cap) {
                    const sv_unode **v = realloc(stack, cap * 2 * sizeof *v);
                    if (v == NULL) {
                        free(stack);
                        return MAX_NODES + 1;
                    }
                    stack = v;
                    cap *= 2;
                }
                stack[sn++] = g->children[i];
            }
            if (n > MAX_NODES) {
                free(stack);
                return n;
            }
        }
    }
    free(stack);
    return n;
}

static pc_blend_mode blend(sv_blend b)
{
    switch (b) {
    case BM_MULTIPLY:
        return PC_BLEND_MULTIPLY;
    case BM_SCREEN:
        return PC_BLEND_SCREEN;
    case BM_OVERLAY:
        return PC_BLEND_OVERLAY;
    case BM_DARKEN:
        return PC_BLEND_DARKEN;
    case BM_LIGHTEN:
        return PC_BLEND_LIGHTEN;
    case BM_COLOR_DODGE:
        return PC_BLEND_COLOR_DODGE;
    case BM_COLOR_BURN:
        return PC_BLEND_COLOR_BURN;
    case BM_HARD_LIGHT:
        return PC_BLEND_HARD_LIGHT;
    case BM_SOFT_LIGHT:
        return PC_BLEND_SOFT_LIGHT;
    case BM_DIFFERENCE:
        return PC_BLEND_DIFFERENCE;
    case BM_EXCLUSION:
        return PC_BLEND_EXCLUSION;
    case BM_HUE:
        return PC_BLEND_HUE;
    case BM_SATURATION:
        return PC_BLEND_SATURATION;
    case BM_COLOR:
        return PC_BLEND_COLOR;
    case BM_LUMINOSITY:
        return PC_BLEND_LUMINOSITY;
    case BM_NORMAL:
    default:
        return PC_BLEND_NORMAL;
    }
}

static pc_color color(uint8_t r, uint8_t g, uint8_t b, float alpha)
{
    float a = isnan(alpha) ? alpha : alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
    return pc_color_rgba((float)r / 255.0f, (float)g / 255.0f, (float)b / 255.0f, a);
}

/* ---------------------------------------------------------------- knots */

typedef struct spv {
    pc_subpath *v;
    size_t n, cap;
} spv;

static void spv_fini(spv *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) {
        pc_subpath_fini(&s->v[i]);
    }
    free(s->v);
    memset(s, 0, sizeof *s);
}

static bool push_knot(pc_subpath *sp, size_t *cap, pc_knot k)
{
    if (sp->nknots == *cap) {
        size_t nc = *cap == 0 ? 8 : *cap * 2;
        pc_knot *v = nc > SIZE_MAX / sizeof *v ? NULL : realloc(sp->knots, nc * sizeof *v);
        if (v == NULL) {
            return false;
        }
        sp->knots = v;
        *cap = nc;
    }
    sp->knots[sp->nknots++] = k;
    return true;
}

static bool flush(bool *has, pc_subpath *cur, spv *out, bool closed)
{
    pc_subpath sp;
    if (!*has) {
        return true;
    }
    *has = false;
    sp = *cur;
    memset(cur, 0, sizeof *cur);
    sp.closed = closed;
    if (closed && sp.nknots > 1) {
        pc_point a = sp.knots[0].anchor;
        pc_point b = sp.knots[sp.nknots - 1].anchor;
        if (fabs(a.x - b.x) < 1e-6 && fabs(a.y - b.y) < 1e-6) {
            sp.knots[0].in_ctrl = sp.knots[sp.nknots - 1].in_ctrl;
            sp.nknots--;
        }
    }
    if (sp.nknots == 0) {
        pc_subpath_fini(&sp);
        return true;
    }
    if (out->n > 0) {
        sp.op = PC_PATH_JOIN;
    }
    if (out->n == out->cap) {
        size_t nc = out->cap == 0 ? 4 : out->cap * 2;
        pc_subpath *v = nc > SIZE_MAX / sizeof *v ? NULL : realloc(out->v, nc * sizeof *v);
        if (v == NULL) {
            pc_subpath_fini(&sp);
            return false;
        }
        out->v = v;
        out->cap = nc;
    }
    out->v[out->n++] = sp;
    return true;
}

/* A flattened-transform path as subpaths of knots: lines keep retracted handles, quadratics are
 * raised to cubics, a closing segment that returns to the start merges into it, and every
 * subpath after the first joins its component (SVG fill rules count across the whole path). */
static bool knots(const sv_path *path, spv *out)
{
    sv_piter it;
    sv_pseg seg;
    pc_subpath cur;
    size_t cap = 0;
    bool has = false;
    memset(&cur, 0, sizeof cur);
    memset(out, 0, sizeof *out);
    piter_init(&it, path);
    while (piter_next(&it, &seg)) {
        switch (seg.verb) {
        case V_MOVE:
            if (!flush(&has, &cur, out, false)) {
                goto fail;
            }
            memset(&cur, 0, sizeof cur);
            cap = 0;
            has = true;
            if (!push_knot(&cur, &cap, pc_knot_corner((double)seg.p[0].x, (double)seg.p[0].y))) {
                goto fail;
            }
            break;
        case V_LINE:
            if (has && !push_knot(&cur, &cap, pc_knot_corner((double)seg.p[0].x, (double)seg.p[0].y))) {
                goto fail;
            }
            break;
        case V_QUAD:
        case V_CUBIC:
            if (has) {
                pc_point c1;
                pc_point c2;
                pc_point p;
                pc_knot k;
                if (seg.verb == V_QUAD) {
                    pc_point c = pc_point_make((double)seg.p[0].x, (double)seg.p[0].y);
                    pc_point p0;
                    p = pc_point_make((double)seg.p[1].x, (double)seg.p[1].y);
                    p0 = cur.nknots > 0 ? cur.knots[cur.nknots - 1].anchor : p;
                    c1 = pc_point_make(p0.x + 2.0 / 3.0 * (c.x - p0.x), p0.y + 2.0 / 3.0 * (c.y - p0.y));
                    c2 = pc_point_make(p.x + 2.0 / 3.0 * (c.x - p.x), p.y + 2.0 / 3.0 * (c.y - p.y));
                } else {
                    c1 = pc_point_make((double)seg.p[0].x, (double)seg.p[0].y);
                    c2 = pc_point_make((double)seg.p[1].x, (double)seg.p[1].y);
                    p = pc_point_make((double)seg.p[2].x, (double)seg.p[2].y);
                }
                if (cur.nknots > 0) {
                    cur.knots[cur.nknots - 1].out_ctrl = c1;
                }
                k.anchor = p;
                k.in_ctrl = c2;
                k.out_ctrl = p;
                k.smooth = false;
                if (!push_knot(&cur, &cap, k)) {
                    goto fail;
                }
            }
            break;
        case V_CLOSE:
        default:
            if (!flush(&has, &cur, out, true)) {
                goto fail;
            }
            cap = 0;
            break;
        }
    }
    if (!flush(&has, &cur, out, false)) {
        goto fail;
    }
    return true;
fail:
    pc_subpath_fini(&cur);
    spv_fini(out);
    return false;
}

/* ---------------------------------------------------------------- layers */

static pc_status gradient_fill(const sv_grad *g, float opacity, float angle, pc_gradient_style style,
                               pc_fill *out, pc_error *err)
{
    pc_color_stop *stops = g->nstops > 0 ? malloc(g->nstops * sizeof *stops) : NULL;
    pc_status st;
    size_t i;
    if (g->nstops > 0 && stops == NULL) {
        return pc_io_nomem(err, "importing an SVG gradient");
    }
    for (i = 0; i < g->nstops; i++) {
        stops[i].pos = g->stops[i].offset;
        stops[i].color = color(g->stops[i].r, g->stops[i].g, g->stops[i].b, g->stops[i].opacity * opacity);
    }
    st = pc_fill_gradient(stops, g->nstops, angle, 1.0f, style, false, out, err);
    free(stops);
    return st;
}

/* A paint as a layer fill; ts maps the paint's space onto the canvas. */
static pc_status paint_fill(conv *c, const sv_paint *p, float opacity, sv_ts ts, pc_fill *out, pc_error *err)
{
    pc_status st;
    switch (p->kind) {
    case PAINT_LINEAR: {
        sv_ts gt = ts_post_concat(p->grad->transform, ts);
        float ax = p->grad->x1;
        float ay = p->grad->y1;
        float bx = p->grad->x2;
        float by = p->grad->y2;
        float angle;
        st = note(c, "gradient",
                  "SVG: gradient paints keep their stops and direction, not their exact geometry", err);
        if (st != PC_OK) {
            return st;
        }
        ts_map_point(gt, &ax, &ay);
        ts_map_point(gt, &bx, &by);
        /* Photoshop's angle: 0 deg left -> right, counter-clockwise positive (y points down). */
        angle = atan2f(-(by - ay), bx - ax) * 57.2957795130823208767981548141051703f;
        return gradient_fill(p->grad, opacity, isfinite(angle) ? angle : 0.0f, PC_GRADIENT_LINEAR, out, err);
    }
    case PAINT_RADIAL:
        st = note(c, "gradient",
                  "SVG: gradient paints keep their stops and direction, not their exact geometry", err);
        if (st != PC_OK) {
            return st;
        }
        return gradient_fill(p->grad, opacity, 0.0f, PC_GRADIENT_RADIAL, out, err);
    case PAINT_PATTERN:
        *out = pc_fill_solid(pc_color_rgba(0.5f, 0.5f, 0.5f, opacity));
        return PC_OK;
    case PAINT_COLOR:
    default:
        *out = pc_fill_solid(color(p->r, p->g, p->b, opacity));
        return PC_OK;
    }
}

static pc_status raster_layer(conv *c, const sv_unode *n, const char *name, sv_ts parent, bool *some,
                              pc_layer *out, pc_error *err);

/* A path as a shape layer, or a pixel layer when its paint is a pattern. */
static pc_status shape_layer(conv *c, sv_arena *a, const sv_unode *p, sv_ts parent, bool *some, pc_layer *out,
                             pc_error *err)
{
    sv_ts ts;
    const sv_path *data;
    spv sp;
    pc_shape_layer sh;
    pc_layer_content content;
    char name[256];
    pc_status st = PC_OK;
    *some = false;
    if (!p->visible) {
        return PC_OK;
    }
    if ((p->fill != NULL && p->fill->paint.kind == PAINT_PATTERN) ||
        (p->stroke != NULL && p->stroke->paint.kind == PAINT_PATTERN)) {
        st = note(c, "pattern", "SVG: pattern paints were rasterised, so those shapes are not editable", err);
        if (st != PC_OK) {
            return st;
        }
        layer_name(c, p->id, 0, name, sizeof name);
        return raster_layer(c, p, name, parent, some, out, err);
    }
    ts = ts_post_concat(p->abs_transform, c->global);
    data = path_transform(a, p->data, ts);
    if (data == NULL) {
        return a->nomem ? pc_io_nomem(err, "importing an SVG path") : PC_OK;
    }
    if (!knots(data, &sp)) {
        return pc_io_nomem(err, "importing an SVG path");
    }
    if (sp.n == 0) {
        spv_fini(&sp);
        return PC_OK;
    }
    memset(&sh, 0, sizeof sh);
    sh.path.subpaths = sp.v;
    sh.path.nsubpaths = sp.n;
    sh.path.fill_rule = p->fill != NULL && p->fill->evenodd ? PC_FILL_RULE_EVENODD : PC_FILL_RULE_NONZERO;
    if (p->fill != NULL) {
        st = paint_fill(c, &p->fill->paint, p->fill->opacity, ts, &sh.fill, err);
        sh.has_fill = st == PC_OK;
    }
    if (st == PC_OK && p->stroke != NULL) {
        const sv_stroke *s = p->stroke;
        /* Stroke widths are in drawing units; under a non-uniform scale they take the geometric
         * mean, the one width the shape model has. */
        float kx = sqrtf(ts.sx * ts.sx + ts.ky * ts.ky);
        float ky = sqrtf(ts.kx * ts.kx + ts.sy * ts.sy);
        float k;
        float w;
        size_t i;
        if (fabsf(kx - ky) > 0.01f * (kx > ky ? kx : ky)) {
            st = note(c, "stroke-scale", "SVG: strokes under a non-uniform scale keep one averaged width",
                      err);
        }
        k = sqrtf(fabsf(ts.sx * ts.sy - ts.kx * ts.ky));
        k = isnan(k) || k < 1e-6f ? 1e-6f : k;
        w = s->width * k;
        w = isnan(w) || w < 0.01f ? 0.01f : w;
        if (st == PC_OK) {
            st = paint_fill(c, &s->paint, 1.0f, ts, &sh.stroke.paint, err);
        }
        if (st == PC_OK) {
            sh.has_stroke = true;
            sh.stroke.width = w;
            sh.stroke.opacity = s->opacity;
            sh.stroke.align = PC_STROKE_ALIGN_CENTER;
            sh.stroke.cap = s->cap == CAP_ROUND    ? PC_LINE_CAP_ROUND
                            : s->cap == CAP_SQUARE ? PC_LINE_CAP_SQUARE
                                                   : PC_LINE_CAP_BUTT;
            sh.stroke.join = s->join == JOIN_ROUND   ? PC_LINE_JOIN_ROUND
                             : s->join == JOIN_BEVEL ? PC_LINE_JOIN_BEVEL
                                                     : PC_LINE_JOIN_MITER;
            sh.stroke.miter_limit = s->miterlimit;
            if (s->dash != NULL && s->ndash > 0) {
                sh.stroke.dashes = malloc(s->ndash * sizeof(float));
                if (sh.stroke.dashes == NULL) {
                    st = pc_io_nomem(err, "importing an SVG stroke");
                } else {
                    for (i = 0; i < s->ndash; i++) {
                        sh.stroke.dashes[i] = s->dash[i] * k / w;
                    }
                    sh.stroke.ndashes = s->ndash;
                }
            }
            sh.stroke.dash_offset = s->dashoffset * k / w;
        }
    }
    if (st == PC_OK) {
        st = pc_vec_render_shape(&sh, c->fmt, c->canvas, &sh.cache, err);
        sh.has_cache = st == PC_OK;
    }
    if (st != PC_OK) {
        pc_shape_layer_fini(&sh);
        return st;
    }
    layer_name(c, p->id, 0, name, sizeof name);
    memset(&content, 0, sizeof content);
    content.kind = PC_LAYER_SHAPE;
    content.u.shape = sh;
    st = pc_layer_init(out, name, &content, err);
    if (st != PC_OK) {
        pc_layer_content_fini(&content);
        return st;
    }
    *some = true;
    return PC_OK;
}

/* The node rendered into a pixel layer over its bounding box, clipped to the canvas. parent is
 * the absolute transform of the node's parent group. */
static pc_status raster_layer(conv *c, const sv_unode *n, const char *name, sv_ts parent, bool *some,
                              pc_layer *out, pc_error *err)
{
    sv_rect bb;
    float k = c->global.sx;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    sv_pixmap pm;
    sv_ts canvas;
    sv_ts ts;
    pc_layer_content content;
    pc_status st;
    *some = false;
    if (n->kind == UN_GROUP) {
        bb = n->abs_layer_bbox;
    } else if (!rect_to_nz(n->abs_bbox, &bb)) {
        return PC_OK;
    }
    x0 = pc_f32_to_i32(floorf(bb.l * k));
    y0 = pc_f32_to_i32(floorf(bb.t * k));
    x1 = pc_f32_to_i32(ceilf(bb.r * k));
    y1 = pc_f32_to_i32(ceilf(bb.b * k));
    x0 = x0 > c->canvas.x0 ? x0 : c->canvas.x0;
    y0 = y0 > c->canvas.y0 ? y0 : c->canvas.y0;
    x1 = x1 < c->canvas.x1 ? x1 : c->canvas.x1;
    y1 = y1 < c->canvas.y1 ? y1 : c->canvas.y1;
    if (x1 <= x0 || y1 <= y0) {
        return PC_OK;
    }
    if (!pixmap((uint32_t)(x1 - x0), (uint32_t)(y1 - y0), &pm)) {
        return note(c, "raster-size", "SVG: an element too large to rasterise was dropped", err);
    }
    /* resvg renders the node in its parent's space after shifting its absolute bounding box to
     * the origin: undo that shift, apply the parent's transform, then scale to the canvas and
     * shift so the pixmap starts at (x0, y0). */
    canvas = ts_row(k, 0.0f, 0.0f, k, -(float)x0, -(float)y0);
    ts = ts_pre_translate(ts_pre_concat(canvas, parent), bb.l, bb.t);
    st = sv_render_node(n, ts_pre_translate(ts, -bb.l, -bb.t), &pm, err);
    memset(&content, 0, sizeof content);
    content.kind = PC_LAYER_RASTER;
    if (st == PC_OK) {
        st = pm_surface(&pm, pc_rect_make(x0, y0, x1, y1), c->fmt, &content.u.raster, err);
    }
    sv_pixmap_fini(&pm);
    if (st != PC_OK) {
        return st;
    }
    st = pc_layer_init(out, name, &content, err);
    if (st != PC_OK) {
        pc_layer_content_fini(&content);
        return st;
    }
    *some = true;
    return PC_OK;
}

static pc_status convert_children(conv *c, sv_arena *a, const sv_unode *g, size_t depth, size_t rec,
                                  sv_ts parent, lvec *out, pc_error *err);

static pc_status push_opt(lvec *out, bool some, pc_layer *l, pc_error *err)
{
    return some ? lvec_push(out, l, err) : PC_OK;
}

static pc_status convert_node(conv *c, sv_arena *a, const sv_unode *n, lvec *out, size_t depth, size_t rec,
                              sv_ts parent, pc_error *err)
{
    char name[256];
    pc_layer l;
    bool some = false;
    pc_status st;
    switch (n->kind) {
    case UN_GROUP: {
        static const char *const WHAT[3] = {"filter", "clip-path", "mask"};
        bool dropped[3];
        bool too_deep = depth >= PC_MAX_GROUP_DEPTH || rec >= MAX_RECURSION;
        bool plain;
        bool only_id;
        lvec children = {NULL, 0, 0};
        pc_layer_content content;
        int i;
        dropped[0] = n->nfilters > 0;
        dropped[1] = n->clip != NULL;
        dropped[2] = n->mask != NULL;
        if (dropped[0] || dropped[1] || dropped[2] || too_deep) {
            for (i = 0; i < 3; i++) {
                if (dropped[i]) {
                    char msg[128];
                    (void)snprintf(msg, sizeof msg,
                                   "SVG: a group with a %s was rasterised, so it is not editable as shapes",
                                   WHAT[i]);
                    st = note(c, WHAT[i], msg, err);
                    if (st != PC_OK) {
                        return st;
                    }
                }
            }
            if (too_deep) {
                st = note(c, "depth", "SVG: groups nested too deeply were rasterised", err);
                if (st != PC_OK) {
                    return st;
                }
            }
            layer_name(c, n->id, 1, name, sizeof name);
            st = raster_layer(c, n, name, parent, &some, &l, err);
            return st == PC_OK ? push_opt(out, some, &l, err) : st;
        }
        /* A group that only carries a transform (baked into its paths) or that the parser
         * inserted for its own bookkeeping adds nothing to the layer tree. */
        plain = n->id[0] == '\0' && n->opacity >= 1.0f && n->blend == BM_NORMAL;
        st = convert_children(c, a, n, plain ? depth : depth + 1, rec + 1, n->abs_transform, &children, err);
        if (st != PC_OK) {
            lvec_fini(&children);
            return st;
        }
        if (children.n == 0) {
            lvec_fini(&children);
            return PC_OK;
        }
        if (plain) {
            size_t k;
            for (k = 0; k < children.n && st == PC_OK; k++) {
                st = lvec_push(out, &children.v[k], err);
            }
            lvec_fini(&children);
            return st;
        }
        only_id = n->opacity >= 1.0f && n->blend == BM_NORMAL;
        if (only_id && children.n == 1) {
            pc_layer child = children.v[0];
            children.n = 0;
            lvec_fini(&children);
            st = pc_str_set_c(&child.name, n->id, err);
            if (st != PC_OK) {
                pc_layer_fini(&child);
                return st;
            }
            return lvec_push(out, &child, err);
        }
        layer_name(c, n->id, 1, name, sizeof name);
        memset(&content, 0, sizeof content);
        content.kind = PC_LAYER_GROUP;
        content.u.group.children = children.v;
        content.u.group.nchildren = children.n;
        content.u.group.expanded = false;
        st = pc_layer_init(&l, name, &content, err);
        if (st != PC_OK) {
            pc_layer_content_fini(&content);
            return st;
        }
        l.opacity = n->opacity;
        l.blend = blend(n->blend);
        return lvec_push(out, &l, err);
    }
    case UN_PATH:
        st = shape_layer(c, a, n, parent, &some, &l, err);
        return st == PC_OK ? push_opt(out, some, &l, err) : st;
    case UN_IMAGE:
        st = note(c, "image", "SVG: embedded images are pixel layers", err);
        if (st != PC_OK) {
            return st;
        }
        layer_name(c, n->id, 2, name, sizeof name);
        st = raster_layer(c, n, name, parent, &some, &l, err);
        return st == PC_OK ? push_opt(out, some, &l, err) : st;
    case UN_TEXT: {
        /* The flattened outlines (svg_text*.c) as a group named by the id or the text. */
        lvec children = {NULL, 0, 0};
        pc_layer_content content;
        if (n->flat != NULL) {
            st = convert_children(c, a, n->flat, depth + 1, rec + 1, n->flat->abs_transform, &children, err);
            if (st != PC_OK) {
                lvec_fini(&children);
                return st;
            }
        }
        if (children.n == 0) {
            lvec_fini(&children);
            return note(c, "text",
                        "SVG: text needs a font the drawing can use; none matched, so it was dropped", err);
        }
        if ((n->id == NULL || n->id[0] == '\0') && sv_text_layer_name(n->text, name, sizeof name)) {
            /* the trimmed text, at most 31 characters */
        } else {
            layer_name(c, n->id, 3, name, sizeof name);
        }
        memset(&content, 0, sizeof content);
        content.kind = PC_LAYER_GROUP;
        content.u.group.children = children.v;
        content.u.group.nchildren = children.n;
        content.u.group.expanded = false;
        st = pc_layer_init(&l, name, &content, err);
        if (st != PC_OK) {
            pc_layer_content_fini(&content);
            return st;
        }
        return lvec_push(out, &l, err);
    }
    default:
        return PC_OK;
    }
}

static pc_status convert_children(conv *c, sv_arena *a, const sv_unode *g, size_t depth, size_t rec,
                                  sv_ts parent, lvec *out, pc_error *err)
{
    size_t i;
    for (i = 0; i < g->nchildren; i++) {
        pc_status st = convert_node(c, a, g->children[i], out, depth, rec, parent, err);
        if (st != PC_OK) {
            return st;
        }
    }
    return PC_OK;
}

/* The document canvas: the drawing's size in px, scaled down to PC_IO_SVG_MAX_SIDE when needed. */
static void canvas_of(const sv_tree *t, float *scale, pc_size *size, bool *shrunk)
{
    float w = t->width > 1.0f ? t->width : 1.0f;
    float h = t->height > 1.0f ? t->height : 1.0f;
    float limit = (float)PC_IO_SVG_MAX_SIDE;
    float s = limit / w;
    float sh = limit / h;
    double sw;
    double shh;
    s = sh < s ? sh : s;
    s = s > 1.0f ? 1.0f : s;
    *shrunk = s < 1.0f;
    *scale = s;
    sw = round((double)(w * s));
    shh = round((double)(h * s));
    size->width = pc_f64_to_u32(sw);
    size->height = pc_f64_to_u32(shh);
    size->width = size->width < 1 ? 1 : size->width > PC_IO_SVG_MAX_SIDE ? PC_IO_SVG_MAX_SIDE : size->width;
    size->height = size->height < 1                    ? 1
                   : size->height > PC_IO_SVG_MAX_SIDE ? PC_IO_SVG_MAX_SIDE
                                                       : size->height;
}

static pc_status parse(sv_arena *a, const uint8_t *bytes, size_t len, sv_tree *t, pc_error *err)
{
    char msg[200];
    pc_status st = sv_parse_tree(a, bytes, len, t, msg, sizeof msg);
    if (st == PC_ERR_NOMEM) {
        return pc_io_nomem(err, "parsing an SVG");
    }
    if (st != PC_OK) {
        return pc_io_svg_error(err, "%s", msg);
    }
    return PC_OK;
}

pc_status pc_io_svg_import(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                           pc_error *err)
{
    sv_arena a;
    sv_tree t;
    float scale;
    pc_size size;
    bool shrunk;
    pc_io_import_result r;
    conv c;
    lvec layers = {NULL, 0, 0};
    pc_status st;
    sv_arena_init(&a);
    st = parse(&a, bytes, len, &t, err);
    if (st != PC_OK) {
        sv_arena_fini(&a);
        return st;
    }
    memset(&r, 0, sizeof r);
    pc_io_warnings_init(&r.warnings);
    canvas_of(&t, &scale, &size, &shrunk);
    if (shrunk) {
        st = pc_io_warn(&r.warnings, err,
                        "SVG: the %.0f \xc3\x97 %.0f px drawing was scaled down to fit %u px",
                        (double)(t.width > 1.0f ? t.width : 1.0f),
                        (double)(t.height > 1.0f ? t.height : 1.0f), (unsigned)PC_IO_SVG_MAX_SIDE);
    }
    if (st == PC_OK) {
        st = pc_document_init(&r.document, name, size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, err);
    }
    if (st != PC_OK) {
        pc_io_warnings_fini(&r.warnings);
        sv_arena_fini(&a);
        return st;
    }
    memset(&c, 0, sizeof c);
    c.fmt = pc_document_pixel_format(&r.document);
    c.canvas = pc_document_bounds(&r.document);
    c.global = ts_scale(scale, scale);
    c.warnings = &r.warnings;
    if (count_nodes(t.root) > MAX_NODES) {
        sv_pixmap pm;
        st = pc_io_warn(&r.warnings, err,
                        "SVG: more than %u elements; opened as one pixel layer instead of shapes", MAX_NODES);
        if (st == PC_OK && pixmap(size.width, size.height, &pm)) {
            pc_layer_content content;
            pc_layer l;
            memset(&content, 0, sizeof content);
            content.kind = PC_LAYER_RASTER;
            st = sv_render_tree(&t, c.global, &pm, err);
            if (st == PC_OK) {
                st = pm_surface(&pm, c.canvas, c.fmt, &content.u.raster, err);
                if (st == PC_OK) {
                    st = pc_layer_init(&l, "Background", &content, err);
                    if (st != PC_OK) {
                        pc_layer_content_fini(&content);
                    } else {
                        st = lvec_push(&layers, &l, err);
                    }
                }
            }
            sv_pixmap_fini(&pm);
        }
    } else if (st == PC_OK) {
        st = convert_children(&c, &a, t.root, 0, 0, t.root->abs_transform, &layers, err);
    }
    if (st == PC_OK && layers.n == 0) {
        st = pc_io_warn(&r.warnings, err, "SVG: nothing to draw (an empty drawing)");
    }
    sv_arena_fini(&a);
    if (st != PC_OK) {
        lvec_fini(&layers);
        pc_io_import_result_fini(&r);
        return st;
    }
    r.document.layers = layers.v;
    r.document.nlayers = layers.n;
    r.source_read_only = false;
    r.preview_only = false;
    *out = r;
    return PC_OK;
}

pc_status pc_io_svg_rasterize(const uint8_t *bytes, size_t len, float scale, pc_compose_buffer *out,
                              pc_error *err)
{
    sv_arena a;
    sv_tree t;
    float cs;
    pc_size size;
    bool shrunk;
    float k;
    float sc;
    uint32_t w;
    uint32_t h;
    sv_pixmap pm;
    pc_compose_buffer b;
    pc_status st;
    size_t i;
    sv_arena_init(&a);
    st = parse(&a, bytes, len, &t, err);
    if (st != PC_OK) {
        sv_arena_fini(&a);
        return st;
    }
    canvas_of(&t, &cs, &size, &shrunk);
    sc = isnan(scale) ? scale : scale < 1.0f / 64.0f ? 1.0f / 64.0f : scale > 64.0f ? 64.0f : scale;
    k = cs * sc;
    w = pc_f32_to_u32(ceilf(t.width * k));
    h = pc_f32_to_u32(ceilf(t.height * k));
    w = w < 1 ? 1 : w;
    h = h < 1 ? 1 : h;
    if ((uint64_t)w * h > MAX_RASTER_PIXELS) {
        sv_arena_fini(&a);
        return pc_io_svg_error(err, "%u \xc3\x97 %u px is too large to rasterise", (unsigned)w, (unsigned)h);
    }
    if (!pixmap(w, h, &pm)) {
        sv_arena_fini(&a);
        return pc_io_nomem(err, "rasterising an SVG");
    }
    st = sv_render_tree(&t, ts_scale(k, k), &pm, err);
    sv_arena_fini(&a);
    if (st == PC_OK) {
        st = pc_compose_buffer_transparent(pc_rect_make(0, 0, (int32_t)w, (int32_t)h), &b, err);
    }
    if (st != PC_OK) {
        sv_pixmap_fini(&pm);
        return st;
    }
    for (i = 0; i < b.len; i++) {
        demultiply(pm.px + i * 4, b.px[i]);
    }
    sv_pixmap_fini(&pm);
    *out = b;
    return PC_OK;
}
