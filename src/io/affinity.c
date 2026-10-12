/* Affinity documents (port of crates/io/src/affinity.rs): the native layers, curves, shapes,
 * artboards, text, images and masks src/affinity reads, mapped to PhotoCraft layers (groups,
 * shape layers, type layers, embedded smart objects, pixel layers, layer and vector masks). What
 * could not be mapped is listed in the warnings. A file whose native data can't be read opens as
 * its embedded preview, with a warning saying why. */
#include "io/affinity.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "affinity/affinity.h"
#include "base/blob.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "compose/gradient_fill.h"
#include "io/flat.h"
#include "io/text_import.h"
#include "vector/vector.h"

/* Pixels an image or mask is resampled into, after cropping to the canvas. */
#define MAX_PIXELS ((uint64_t)64 << 20)
/* Canvas side in pixels. */
#define MAX_SIDE 300000.0

typedef struct builder {
    pc_document doc;
    char **warnings;
    size_t nwarnings;
    size_t capwarnings;
    pc_pixel_format format;
    pc_rect canvas;
} builder;

/* A growing array of layers (bottom to top). */
typedef struct lvec {
    pc_layer *v;
    size_t n;
    size_t cap;
} lvec;

static void lvec_fini(lvec *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        pc_layer_fini(&l->v[i]);
    }
    free(l->v);
    l->v = NULL;
    l->n = 0;
    l->cap = 0;
}

/* Appends *layer (moved; finalised on failure). */
static pc_status lvec_push(lvec *l, pc_layer *layer, pc_error *err)
{
    if (l->n == l->cap) {
        size_t cap = l->cap == 0 ? 4 : l->cap * 2;
        pc_layer *v = cap > SIZE_MAX / sizeof *v ? NULL : realloc(l->v, cap * sizeof *v);
        if (v == NULL) {
            pc_layer_fini(layer);
            return pc_io_nomem(err, "importing an Affinity document");
        }
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n++] = *layer;
    memset(layer, 0, sizeof *layer);
    return PC_OK;
}

static pc_status warn(builder *b, const char *w, pc_error *err)
{
    size_t i;
    char *s;
    for (i = 0; i < b->nwarnings; i++) {
        if (strcmp(b->warnings[i], w) == 0) {
            return PC_OK;
        }
    }
    if (b->nwarnings == b->capwarnings) {
        size_t cap = b->capwarnings == 0 ? 4 : b->capwarnings * 2;
        char **v = realloc(b->warnings, cap * sizeof *v);
        if (v == NULL) {
            return pc_io_nomem(err, "importing an Affinity document");
        }
        b->warnings = v;
        b->capwarnings = cap;
    }
    s = malloc(strlen(w) + 1);
    if (s == NULL) {
        return pc_io_nomem(err, "importing an Affinity document");
    }
    memcpy(s, w, strlen(w) + 1);
    b->warnings[b->nwarnings++] = s;
    return PC_OK;
}

static void builder_free_warnings(builder *b)
{
    size_t i;
    for (i = 0; i < b->nwarnings; i++) {
        free(b->warnings[i]);
    }
    free(b->warnings);
    b->warnings = NULL;
    b->nwarnings = 0;
    b->capwarnings = 0;
}

static void builder_fini(builder *b)
{
    builder_free_warnings(b);
    pc_document_fini(&b->doc);
}

static pc_affine affine(const pc_af_affine *a)
{
    pc_affine m;
    memcpy(m.m, a->m, sizeof m.m);
    return m;
}

static pc_point point(pc_af_point p)
{
    return pc_point_make(p.x, p.y);
}

/* f64::clamp(0, 1) as f32 (NaN stays NaN). */
static float unit(double v)
{
    if (isnan(v)) {
        return (float)v;
    }
    return (float)(v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v);
}

static pc_color color(const pc_af_color *c)
{
    pc_color out;
    float rgb[3];
    switch (c->kind) {
    case PC_AF_RGB:
        return pc_color_rgba(unit(c->v[0]), unit(c->v[1]), unit(c->v[2]), unit(c->alpha));
    case PC_AF_CMYK:
        out.mode = PC_COLOR_MODE_CMYK;
        out.c[0] = unit(c->v[0]);
        out.c[1] = unit(c->v[1]);
        out.c[2] = unit(c->v[2]);
        out.c[3] = unit(c->v[3]);
        out.alpha = unit(c->alpha);
        pc_color_to_rgb(&out, rgb);
        return pc_color_rgba(rgb[0], rgb[1], rgb[2], unit(c->alpha));
    case PC_AF_GRAY:
        return pc_color_rgba(unit(c->v[0]), unit(c->v[0]), unit(c->v[0]), unit(c->alpha));
    case PC_AF_LAB:
    default:
        out.mode = PC_COLOR_MODE_LAB;
        out.c[0] = unit(c->v[0] / 100.0);
        out.c[1] = unit((c->v[1] + 128.0) / 255.0);
        out.c[2] = unit((c->v[2] + 128.0) / 255.0);
        out.c[3] = 0.0f;
        out.alpha = unit(c->alpha);
        pc_color_to_rgb(&out, rgb);
        return pc_color_rgba(rgb[0], rgb[1], rgb[2], unit(c->alpha));
    }
}

static const char *default_name(pc_af_nkind k)
{
    switch (k) {
    case PC_AF_LAYER:
        return "Layer";
    case PC_AF_ARTBOARD:
        return "Artboard";
    case PC_AF_SHAPE:
        return "Shape";
    case PC_AF_TEXT:
        return "Text";
    case PC_AF_IMAGE:
        return "Image";
    case PC_AF_GROUP:
    case PC_AF_UNSUPPORTED:
    default:
        return "Group";
    }
}

/* f64 -> i32 after clamping into [lo, hi] (NaN -> 0, as Rust's `as`). */
static int32_t clamp_i32(double v, int32_t lo, int32_t hi)
{
    if (!isnan(v)) {
        v = v<(double)lo ? (double)lo : v>(double) hi ? (double)hi : v;
    }
    return pc_f64_to_i32(v);
}

/* Integer bounding box of r mapped by m, clipped to the canvas. */
static pc_rect bbox(const builder *b, pc_affine m, pc_af_rect r)
{
    pc_point p[4];
    double x0 = INFINITY;
    double y0 = INFINITY;
    double x1 = -INFINITY;
    double y1 = -INFINITY;
    size_t i;
    p[0] = pc_affine_apply(m, pc_point_make(r.x0, r.y0));
    p[1] = pc_affine_apply(m, pc_point_make(r.x1, r.y0));
    p[2] = pc_affine_apply(m, pc_point_make(r.x1, r.y1));
    p[3] = pc_affine_apply(m, pc_point_make(r.x0, r.y1));
    for (i = 0; i < 4; i++) {
        x0 = pc_rs_min(x0, p[i].x);
        y0 = pc_rs_min(y0, p[i].y);
        x1 = pc_rs_max(x1, p[i].x);
        y1 = pc_rs_max(y1, p[i].y);
    }
    return pc_rect_make(
        clamp_i32(floor(x0), b->canvas.x0, b->canvas.x1), clamp_i32(floor(y0), b->canvas.y0, b->canvas.y1),
        clamp_i32(ceil(x1), b->canvas.x0, b->canvas.x1), clamp_i32(ceil(y1), b->canvas.y0, b->canvas.y1));
}

static pc_status blend(builder *b, pc_af_blend m, pc_blend_mode *out, pc_error *err)
{
    switch (m) {
    case PC_AF_NORMAL:
        *out = PC_BLEND_NORMAL;
        break;
    case PC_AF_PASS_THROUGH:
        *out = PC_BLEND_PASS_THROUGH;
        break;
    case PC_AF_DARKEN:
        *out = PC_BLEND_DARKEN;
        break;
    case PC_AF_DARKER_COLOR:
        *out = PC_BLEND_DARKER_COLOR;
        break;
    case PC_AF_MULTIPLY:
        *out = PC_BLEND_MULTIPLY;
        break;
    case PC_AF_COLOR_BURN:
        *out = PC_BLEND_COLOR_BURN;
        break;
    case PC_AF_LIGHTEN:
        *out = PC_BLEND_LIGHTEN;
        break;
    case PC_AF_LIGHTER_COLOR:
        *out = PC_BLEND_LIGHTER_COLOR;
        break;
    case PC_AF_SCREEN:
        *out = PC_BLEND_SCREEN;
        break;
    case PC_AF_COLOR_DODGE:
        *out = PC_BLEND_COLOR_DODGE;
        break;
    case PC_AF_ADD:
        *out = PC_BLEND_LINEAR_DODGE;
        break;
    case PC_AF_OVERLAY:
        *out = PC_BLEND_OVERLAY;
        break;
    case PC_AF_SOFT_LIGHT:
        *out = PC_BLEND_SOFT_LIGHT;
        break;
    case PC_AF_HARD_LIGHT:
        *out = PC_BLEND_HARD_LIGHT;
        break;
    case PC_AF_VIVID_LIGHT:
        *out = PC_BLEND_VIVID_LIGHT;
        break;
    case PC_AF_PIN_LIGHT:
        *out = PC_BLEND_PIN_LIGHT;
        break;
    case PC_AF_LINEAR_LIGHT:
        *out = PC_BLEND_LINEAR_LIGHT;
        break;
    case PC_AF_HARD_MIX:
        *out = PC_BLEND_HARD_MIX;
        break;
    case PC_AF_DIFFERENCE:
        *out = PC_BLEND_DIFFERENCE;
        break;
    case PC_AF_EXCLUSION:
        *out = PC_BLEND_EXCLUSION;
        break;
    case PC_AF_SUBTRACT:
        *out = PC_BLEND_SUBTRACT;
        break;
    case PC_AF_HUE:
        *out = PC_BLEND_HUE;
        break;
    case PC_AF_SATURATION:
        *out = PC_BLEND_SATURATION;
        break;
    case PC_AF_LUMINOSITY:
        *out = PC_BLEND_LUMINOSITY;
        break;
    case PC_AF_COLOR:
        *out = PC_BLEND_COLOR;
        break;
    case PC_AF_AVERAGE:
    case PC_AF_NEGATION:
    case PC_AF_REFLECT:
    case PC_AF_GLOW:
    case PC_AF_ERASE:
    default:
        *out = PC_BLEND_NORMAL;
        return warn(b, "the Average, Negation, Reflect, Glow and Erase blend modes import as Normal", err);
    }
    return PC_OK;
}

/* Layer::new + the node's name, visibility, opacity, blend and lock; takes *content. */
static pc_status base(builder *b, const pc_af_node *n, pc_layer_content *content, pc_layer *out,
                      pc_error *err)
{
    pc_layer l;
    pc_blend_mode m;
    pc_status st;
    st = pc_layer_init(&l, n->name != NULL && n->name[0] != '\0' ? n->name : default_name(n->kind), content,
                       err);
    if (st != PC_OK) {
        pc_layer_content_fini(content);
        return st;
    }
    l.visible = n->visible;
    l.opacity = pc_f64_to_f32(n->opacity);
    st = blend(b, n->blend, &m, err);
    if (st != PC_OK) {
        pc_layer_fini(&l);
        return st;
    }
    l.blend = m;
    if (n->locked) {
        memset(&l.locks, 0, sizeof l.locks);
        l.locks.all = true;
    }
    *out = l;
    return PC_OK;
}

static void group_content(lvec *children, bool expanded, pc_layer_content *c)
{
    memset(c, 0, sizeof *c);
    c->kind = PC_LAYER_GROUP;
    c->u.group.children = children->v;
    c->u.group.nchildren = children->n;
    c->u.group.expanded = expanded;
    memset(children, 0, sizeof *children);
}

/* Pass-through and Normal (isolated) groups map one to one; takes *children. */
static pc_status group(builder *b, const pc_af_node *n, lvec *children, pc_layer *out, pc_error *err)
{
    pc_layer_content c;
    group_content(children, false, &c);
    return base(b, n, &c, out, err);
}

static pc_status path(const pc_af_path *p, pc_affine m, bool even_odd, pc_path *out, pc_error *err)
{
    pc_path r;
    size_t i;
    memset(&r, 0, sizeof r);
    if (p->nsubpaths > 0) {
        r.subpaths = pc_calloc_array(p->nsubpaths, sizeof *r.subpaths);
        if (r.subpaths == NULL) {
            return pc_io_nomem(err, "importing an Affinity path");
        }
    }
    r.nsubpaths = p->nsubpaths;
    for (i = 0; i < p->nsubpaths; i++) {
        const pc_af_subpath *s = &p->subpaths[i];
        pc_subpath *sp = &r.subpaths[i];
        pc_point start = pc_affine_apply(m, point(s->start));
        size_t j;
        size_t n;
        /* Knots: each anchor with the control points of the segments arriving and leaving. */
        sp->knots = pc_malloc_array(s->nsegments + 1, sizeof *sp->knots);
        if (sp->knots == NULL) {
            pc_path_fini(&r);
            return pc_io_nomem(err, "importing an Affinity path");
        }
        sp->knots[0] = pc_knot_corner(start.x, start.y);
        n = 1;
        for (j = 0; j < s->nsegments; j++) {
            pc_point e = pc_affine_apply(m, point(s->segments[j][2]));
            sp->knots[n - 1].out_ctrl = pc_affine_apply(m, point(s->segments[j][0]));
            sp->knots[n] = pc_knot_corner(e.x, e.y);
            sp->knots[n].in_ctrl = pc_affine_apply(m, point(s->segments[j][1]));
            n++;
        }
        /* A closed subpath that ends where it starts: the last knot is the first. */
        if (s->closed && n > 1 && fabs(sp->knots[0].anchor.x - sp->knots[n - 1].anchor.x) < 1e-9 &&
            fabs(sp->knots[0].anchor.y - sp->knots[n - 1].anchor.y) < 1e-9) {
            sp->knots[0].in_ctrl = sp->knots[n - 1].in_ctrl;
            n--;
        }
        sp->nknots = n;
        sp->closed = s->closed;
        sp->op = PC_PATH_COMBINE;
    }
    if (even_odd) {
        /* One component filled even-odd, so inner subpaths cut holes as in Affinity. */
        r.fill_rule = PC_FILL_RULE_EVENODD;
        for (i = 1; i < r.nsubpaths; i++) {
            r.subpaths[i].op = PC_PATH_JOIN;
        }
    }
    *out = r;
    return PC_OK;
}

/* *some false for Paint::None. */
static pc_status fill(builder *b, const pc_af_paint *p, pc_rect frame, pc_affine to_doc, bool *some,
                      pc_fill *out, pc_error *err)
{
    pc_gradient_style style;
    pc_affine m;
    pc_point o;
    pc_point e;
    pc_point v;
    double u;
    double w;
    float from[2];
    float to[2];
    float angle;
    float scale;
    float offset[2];
    pc_fill f;
    size_t i;
    pc_status st;
    *some = false;
    if (p->kind == PC_AF_PAINT_NONE) {
        return PC_OK;
    }
    if (p->kind == PC_AF_SOLID) {
        *out = pc_fill_solid(color(&p->color));
        *some = true;
        return PC_OK;
    }
    style = p->gkind == PC_AF_LINEAR   ? PC_GRADIENT_LINEAR
            : p->gkind == PC_AF_RADIAL ? PC_GRADIENT_RADIAL
                                       : PC_GRADIENT_ANGLE;
    m = pc_affine_mul(to_doc, affine(&p->transform));
    o = pc_affine_apply(m, pc_point_make(0.0, 0.0));
    e = pc_affine_apply(m, pc_point_make(1.0, 0.0));
    v = pc_affine_apply(m, pc_point_make(0.0, 1.0));
    u = hypot(e.x - o.x, e.y - o.y);
    w = hypot(v.x - o.x, v.y - o.y);
    if (style == PC_GRADIENT_RADIAL && u > 0.0 && fabs(w / u - 1.0) > 0.01) {
        st = warn(b, "elliptical gradients import as circular ones", err);
        if (st != PC_OK) {
            return st;
        }
    }
    from[0] = pc_f64_to_f32(o.x);
    from[1] = pc_f64_to_f32(o.y);
    to[0] = pc_f64_to_f32(e.x);
    to[1] = pc_f64_to_f32(e.y);
    pc_compose_gradient_from_handles(style, from, to, frame, 0.0f, &angle, &scale, offset);
    memset(&f, 0, sizeof f);
    f.kind = PC_FILL_GRADIENT;
    f.u.gradient.angle = angle;
    f.u.gradient.scale = scale;
    f.u.gradient.style = style;
    f.u.gradient.offset[0] = offset[0];
    f.u.gradient.offset[1] = offset[1];
    f.u.gradient.align = true;
    if (p->nstops > 0) {
        f.u.gradient.stops = pc_malloc_array(p->nstops, sizeof *f.u.gradient.stops);
        f.u.gradient.opacity_stops = pc_malloc_array(p->nstops, sizeof *f.u.gradient.opacity_stops);
        if (p->nstops > 1) {
            f.u.gradient.midpoints = pc_malloc_array(p->nstops - 1, sizeof *f.u.gradient.midpoints);
        }
        if (f.u.gradient.stops == NULL || f.u.gradient.opacity_stops == NULL ||
            (p->nstops > 1 && f.u.gradient.midpoints == NULL)) {
            pc_fill_fini(&f);
            return pc_io_nomem(err, "importing an Affinity gradient");
        }
    }
    for (i = 0; i < p->nstops; i++) {
        pc_color c = color(&p->stops[i].color);
        c.alpha = 1.0f;
        f.u.gradient.stops[i].pos = pc_f64_to_f32(p->stops[i].offset);
        f.u.gradient.stops[i].color = c;
        f.u.gradient.opacity_stops[i].pos = pc_f64_to_f32(p->stops[i].offset);
        f.u.gradient.opacity_stops[i].opacity = pc_f64_to_f32(p->stops[i].color.alpha);
        if (i + 1 < p->nstops) {
            f.u.gradient.midpoints[i] = pc_f64_to_f32(pc_af_stop_half_point(&p->stops[i]));
        }
    }
    f.u.gradient.nstops = p->nstops;
    f.u.gradient.nopacity_stops = p->nstops;
    f.u.gradient.nmidpoints = p->nstops > 0 ? p->nstops - 1 : 0;
    *out = f;
    *some = true;
    return PC_OK;
}

static pc_status stroke(builder *b, const pc_af_stroke *s, pc_rect frame, pc_affine to_doc, bool *some,
                        pc_shape_stroke *out, pc_error *err)
{
    pc_shape_stroke r;
    double wd = pc_rs_max_k(s->width, 1e-9);
    bool has;
    pc_status st;
    size_t i;
    *some = false;
    memset(&r, 0, sizeof r);
    st = fill(b, &s->paint, frame, to_doc, &has, &r.paint, err);
    if (st != PC_OK || !has) {
        return st;
    }
    if (s->behind) {
        st = warn(b, "strokes drawn behind their fill import in front of it", err);
        if (st != PC_OK) {
            pc_fill_fini(&r.paint);
            return st;
        }
    }
    r.width = pc_f64_to_f32(s->width);
    r.opacity = 1.0f;
    r.align = s->align == PC_AF_INSIDE    ? PC_STROKE_ALIGN_INSIDE
              : s->align == PC_AF_OUTSIDE ? PC_STROKE_ALIGN_OUTSIDE
                                          : PC_STROKE_ALIGN_CENTER;
    r.cap = s->cap == PC_AF_ROUND_CAP ? PC_LINE_CAP_ROUND
            : s->cap == PC_AF_SQUARE  ? PC_LINE_CAP_SQUARE
                                      : PC_LINE_CAP_BUTT;
    r.join = s->join == PC_AF_ROUND_JOIN ? PC_LINE_JOIN_ROUND
             : s->join == PC_AF_BEVEL    ? PC_LINE_JOIN_BEVEL
                                         : PC_LINE_JOIN_MITER;
    r.miter_limit = pc_f64_to_f32(s->miter_limit);
    /* In multiples of the width, as Photoshop stores them. */
    if (s->has_dash) {
        if (s->ndash > 0) {
            r.dashes = pc_malloc_array(s->ndash, sizeof *r.dashes);
            if (r.dashes == NULL) {
                pc_fill_fini(&r.paint);
                return pc_io_nomem(err, "importing an Affinity stroke");
            }
        }
        for (i = 0; i < s->ndash; i++) {
            r.dashes[i] = pc_f64_to_f32(s->dash[i] / wd);
        }
        r.ndashes = s->ndash;
        r.dash_offset = pc_f64_to_f32(s->dash_phase / wd);
    }
    *out = r;
    *some = true;
    return PC_OK;
}

/* A shape layer of *p (moved) with the node's first fill and stroke and its rendered cache. */
static pc_status shape(builder *b, pc_path *p, const pc_af_node *n, pc_affine to_doc, pc_shape_layer *out,
                       pc_error *err)
{
    pc_shape_layer sh;
    double cb[4];
    pc_rect frame = b->canvas;
    pc_status st = PC_OK;
    memset(&sh, 0, sizeof sh);
    sh.path = *p;
    memset(p, 0, sizeof *p);
    if (pc_path_control_bounds(&sh.path, cb)) {
        frame = pc_rect_make(pc_f64_to_i32(floor(cb[0])), pc_f64_to_i32(floor(cb[1])),
                             pc_f64_to_i32(ceil(cb[2])), pc_f64_to_i32(ceil(cb[3])));
    }
    if (n->nfills > 1 || n->nstrokes > 1) {
        st = warn(b, "only one fill and one stroke per shape import", err);
    }
    if (st == PC_OK && n->nfills > 0) {
        st = fill(b, &n->fills[0], frame, to_doc, &sh.has_fill, &sh.fill, err);
    }
    if (st == PC_OK && n->nstrokes > 0) {
        st = stroke(b, &n->strokes[0], frame, to_doc, &sh.has_stroke, &sh.stroke, err);
    }
    if (st == PC_OK) {
        st = pc_vec_render_shape(&sh, b->format, b->canvas, &sh.cache, err);
        sh.has_cache = st == PC_OK;
    }
    if (st != PC_OK) {
        pc_shape_layer_fini(&sh);
        return st;
    }
    *out = sh;
    return PC_OK;
}

/* s with U+2029 and U+2028 replaced by "\r" (malloc'd); NULL on out of memory. */
static char *paragraphs(const char *s, size_t *len)
{
    size_t n = strlen(s);
    char *o = malloc(n + 1);
    size_t i = 0;
    size_t j = 0;
    if (o == NULL) {
        return NULL;
    }
    while (i < n) {
        const unsigned char *u = (const unsigned char *)s + i;
        if (i + 2 < n && u[0] == 0xE2 && u[1] == 0x80 && (u[2] == 0xA8 || u[2] == 0xA9)) {
            o[j++] = '\r';
            i += 3;
        } else {
            o[j++] = s[i++];
        }
    }
    o[j] = '\0';
    *len = j;
    return o;
}

static pc_status text(builder *b, const pc_af_text *t, pc_affine to_doc, pc_text_layer *out, pc_error *err)
{
    pc_point origin = t->has_frame ? pc_point_make(t->frame.x0, t->frame.y0) : point(t->anchor);
    pc_affine m =
        pc_affine_mul(pc_affine_mul(to_doc, affine(&t->transform)), pc_affine_translate(origin.x, origin.y));
    double k = pc_rs_max_k(sqrt(fabs(pc_affine_determinant(m))), 1e-9);
    double dpi = (double)pc_rs_maxf_k(b->doc.resolution_dpi, 1.0f);
    pc_text_layer layer;
    pc_paragraph_style ps = pc_paragraph_style_default();
    pc_status st = PC_OK;
    size_t i;
    pc_text_layer_default(&layer);
    layer.transform = pc_affine_mul(m, pc_affine_scale(1.0 / k));
    if (t->nruns > 0) {
        layer.runs = pc_calloc_array(t->nruns, sizeof *layer.runs);
        if (layer.runs == NULL) {
            return pc_io_nomem(err, "importing Affinity text");
        }
    }
    for (i = 0; st == PC_OK && i < t->nruns; i++) {
        const pc_af_text_run *r = &t->runs[i];
        pc_text_run *run = &layer.runs[i];
        size_t len = 0;
        /* Affinity separates paragraphs with U+2029 and lines with U+2028. */
        char *s = paragraphs(r->text != NULL ? r->text : "", &len);
        pc_char_style_default(&run->style);
        layer.nruns = i + 1;
        if (s == NULL) {
            st = pc_io_nomem(err, "importing Affinity text");
            break;
        }
        st = pc_str_set_c(&run->style.font_family, r->family != NULL ? r->family : "", err);
        if (st == PC_OK && r->postscript != NULL && r->postscript[0] != '\0') {
            run->style.has_postscript_name = true;
            st = pc_str_set_c(&run->style.postscript_name, r->postscript, err);
        }
        run->style.weight = (uint16_t)(r->weight < 100 ? 100 : r->weight > 900 ? 900 : r->weight);
        run->style.italic = r->italic;
        run->style.size_pt = pc_f64_to_f32(r->size * k * 72.0 / dpi);
        run->style.color = r->fill.kind == PC_AF_SOLID ? color(&r->fill.color) : PC_COLOR_BLACK;
        run->style.tracking = pc_f64_to_f32(r->tracking * 1000.0);
        if (r->has_leading) {
            run->style.has_leading = true;
            run->style.leading_pt = pc_f64_to_f32(r->leading * k * 72.0 / dpi);
        }
        if (st == PC_OK) {
            st = pc_str_append(&layer.text, s, len, err);
        }
        run->len = len;
        free(s);
    }
    if (st == PC_OK && layer.nruns > 0) {
        st = pc_str_clone(&layer.runs[0].style.font_family, &layer.font_family, err);
        layer.size_pt = layer.runs[0].style.size_pt;
        layer.color = layer.runs[0].style.color;
    }
    if (st == PC_OK) {
        layer.paragraphs = malloc(sizeof *layer.paragraphs);
        if (layer.paragraphs == NULL) {
            st = pc_io_nomem(err, "importing Affinity text");
        } else {
            ps.align = t->align == PC_AF_ACENTER   ? PC_TEXT_ALIGN_CENTER
                       : t->align == PC_AF_RIGHT   ? PC_TEXT_ALIGN_RIGHT
                       : t->align == PC_AF_JUSTIFY ? PC_TEXT_ALIGN_JUSTIFY_LEFT
                                                   : PC_TEXT_ALIGN_LEFT;
            layer.paragraphs[0].len = layer.text.len;
            layer.paragraphs[0].style = ps;
            layer.nparagraphs = 1;
        }
    }
    if (st != PC_OK) {
        pc_text_layer_fini(&layer);
        return st;
    }
    if (t->has_frame) {
        layer.shape.is_box = true;
        layer.shape.x = 0.0f;
        layer.shape.y = 0.0f;
        layer.shape.width = pc_f64_to_f32((t->frame.x1 - t->frame.x0) * k);
        layer.shape.height = pc_f64_to_f32((t->frame.y1 - t->frame.y0) * k);
    }
    *out = layer;
    return PC_OK;
}

/* Straight RGBA8 pixels of an image (decoding an embedded file) with its size; *some false when
 * the embedded file can't be decoded (warned). *rgba is malloc'd (*owned) or borrowed. */
static pc_status pixels(builder *b, const pc_af_image *img, bool *some, uint32_t *w, uint32_t *h,
                        const uint8_t **rgba, size_t *len, uint8_t **owned, pc_error *err)
{
    pc_cdecode_opts opts;
    pc_cimage d;
    pc_error e;
    pc_status st;
    *some = false;
    *owned = NULL;
    if (img->format == PC_AF_RGBA8) {
        *w = img->width;
        *h = img->height;
        *rgba = img->pixels;
        *len = img->npixels;
        *some = true;
        return PC_OK;
    }
    opts = pc_cdecode_opts_default();
    opts.limits.max_width = 65535;
    opts.limits.max_height = 65535;
    opts.limits.max_pixels = MAX_PIXELS;
    opts.limits.max_alloc = (uint64_t)512 << 20;
    st = pc_cdecode_with(img->pixels, img->npixels, &opts, &d, &e);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
    }
    if (st != PC_OK) {
        return warn(b, "an embedded image that could not be decoded was left out", err);
    }
    st = pc_cimage_to_rgba8(&d, owned, len, err);
    *w = d.width;
    *h = d.height;
    pc_cimage_fini(&d);
    if (st != PC_OK) {
        return st;
    }
    *rgba = *owned;
    *some = true;
    return PC_OK;
}

static double at(const uint8_t *src, size_t len, uint32_t w, uint32_t h, int64_t x, int64_t y,
                 size_t channels, size_t ch)
{
    size_t i;
    if (x < 0 || y < 0 || x >= (int64_t)w || y >= (int64_t)h) {
        return 0.0;
    }
    i = ((size_t)y * (size_t)w + (size_t)x) * channels + ch;
    return i < len ? (double)src[i] : 0.0;
}

static int64_t wrap_inc(int64_t v)
{
    return (int64_t)(uint64_t)((uint64_t)v + 1u);
}

/* Bilinear resampling of src (w x h, channels 4 = straight RGBA8 or 1 = grey) mapped by m
 * (source pixels to canvas pixels) into a surface; exact copy when m is an integer translation.
 * Pixels outside the source are transparent (grey: 0, hidden). *some false when the image is
 * left out (warned) or m isn't invertible. */
static pc_status resample(builder *b, uint32_t w, uint32_t h, const uint8_t *src, size_t len, pc_affine m,
                          size_t channels, bool *some, pc_surface *out, pc_error *err)
{
    static const float ZERO[1] = {0.0f};
    pc_pixel_format format = channels == 1 ? PC_PIXEL_FORMAT_GRAY8 : b->format;
    pc_af_rect src_rect = {0.0, 0.0, (double)w, (double)h};
    pc_rect r = bbox(b, m, src_rect);
    pc_surface s;
    pc_affine inv;
    double a = m.m[0];
    double bb = m.m[1];
    double c = m.m[2];
    double d = m.m[3];
    double e = m.m[4];
    double f = m.m[5];
    bool exact;
    size_t rw;
    size_t rh;
    size_t row;
    uint8_t *buf;
    pc_status st;
    *some = false;
    if (channels == 1) {
        pc_surface_init_default(&s, format, ZERO, 1);
    } else {
        pc_surface_init(&s, format);
    }
    if (pc_rect_width(r) == 0 || pc_rect_height(r) == 0) {
        *out = s;
        *some = true;
        return PC_OK;
    }
    if ((uint64_t)pc_rect_width(r) * pc_rect_height(r) > MAX_PIXELS) {
        pc_surface_fini(&s);
        return warn(b, "images larger than 64 megapixels on the canvas were left out", err);
    }
    exact = a == 1.0 && d == 1.0 && bb == 0.0 && c == 0.0 && e - trunc(e) == 0.0 && f - trunc(f) == 0.0;
    if (!pc_affine_inverse(m, &inv)) {
        pc_surface_fini(&s);
        return PC_OK;
    }
    rw = pc_rect_width(r);
    rh = pc_rect_height(r);
    buf = calloc(rw * rh, channels);
    if (buf == NULL) {
        pc_surface_fini(&s);
        return pc_io_nomem(err, "resampling an Affinity image");
    }
    for (row = 0; row < rh; row++) {
        size_t col;
        for (col = 0; col < rw; col++) {
            uint8_t *px = buf + (row * rw + col) * channels;
            pc_point p = pc_affine_apply(
                inv, pc_point_make((double)r.x0 + (double)col + 0.5, (double)r.y0 + (double)row + 0.5));
            double fx;
            double fy;
            int64_t x0;
            int64_t y0;
            int64_t xs[4];
            int64_t ys[4];
            double tx;
            double ty;
            double wt[4];
            size_t ch;
            size_t q;
            if (exact) {
                int64_t x = pc_f64_to_i64(floor(p.x));
                int64_t y = pc_f64_to_i64(floor(p.y));
                for (ch = 0; ch < channels; ch++) {
                    px[ch] = pc_f64_to_u8(at(src, len, w, h, x, y, channels, ch));
                }
                continue;
            }
            fx = p.x - 0.5;
            fy = p.y - 0.5;
            x0 = pc_f64_to_i64(floor(fx));
            y0 = pc_f64_to_i64(floor(fy));
            tx = fx - (double)x0;
            ty = fy - (double)y0;
            xs[0] = x0;
            ys[0] = y0;
            xs[1] = wrap_inc(x0);
            ys[1] = y0;
            xs[2] = x0;
            ys[2] = wrap_inc(y0);
            xs[3] = wrap_inc(x0);
            ys[3] = wrap_inc(y0);
            wt[0] = (1.0 - tx) * (1.0 - ty);
            wt[1] = tx * (1.0 - ty);
            wt[2] = (1.0 - tx) * ty;
            wt[3] = tx * ty;
            if (channels == 1) {
                double sum = -0.0;
                for (q = 0; q < 4; q++) {
                    sum += at(src, len, w, h, xs[q], ys[q], 1, 0) * wt[q];
                }
                px[0] = pc_f64_to_u8(round(sum));
            } else {
                /* Interpolate premultiplied colour so transparent pixels don't bleed. */
                double alpha = -0.0;
                for (q = 0; q < 4; q++) {
                    alpha += at(src, len, w, h, xs[q], ys[q], 4, 3) * wt[q];
                }
                for (ch = 0; ch < 3; ch++) {
                    double pm = -0.0;
                    for (q = 0; q < 4; q++) {
                        pm += at(src, len, w, h, xs[q], ys[q], 4, ch) *
                              at(src, len, w, h, xs[q], ys[q], 4, 3) / 255.0 * wt[q];
                    }
                    px[ch] = alpha > 0.0 ? pc_f64_to_u8(round(pm * 255.0 / alpha)) : 0;
                }
                px[3] = pc_f64_to_u8(round(alpha));
            }
        }
    }
    st = pc_surface_write_interleaved(&s, r, buf, rw * rh * channels, err);
    free(buf);
    if (st != PC_OK) {
        pc_surface_fini(&s);
        return st;
    }
    pc_surface_prune(&s);
    *out = s;
    *some = true;
    return PC_OK;
}

/* The content of an image node; *some false when it is left out. */
static pc_status image(builder *b, const pc_af_image *img, pc_affine to_doc, bool *some,
                       pc_layer_content *out, pc_error *err)
{
    uint32_t w = 0;
    uint32_t h = 0;
    const uint8_t *rgba = NULL;
    size_t len = 0;
    uint8_t *owned = NULL;
    bool has;
    pc_affine m;
    pc_affine scale;
    pc_surface surface;
    pc_status st;
    *some = false;
    st = pixels(b, img, &has, &w, &h, &rgba, &len, &owned, err);
    if (st != PC_OK || !has) {
        return st;
    }
    /* The pixel grid maps onto the Affinity image's own size, whatever the stored file's. */
    memset(&scale, 0, sizeof scale);
    scale.m[0] = (double)img->width / (double)(w > 0 ? w : 1);
    scale.m[3] = (double)img->height / (double)(h > 0 ? h : 1);
    m = pc_affine_mul(pc_affine_mul(to_doc, affine(&img->transform)), scale);
    st = resample(b, w, h, rgba, len, m, 4, &has, &surface, err);
    free(owned);
    if (st != PC_OK || !has) {
        return st;
    }
    memset(out, 0, sizeof *out);
    if (img->format == PC_AF_ENCODED) {
        /* A placed image keeps its file, as a smart object placed by the same transform. */
        pc_smart_source src;
        memset(&src, 0, sizeof src);
        st = pc_str_set_c(&src.file_name, "Affinity image", err);
        if (st == PC_OK) {
            st = pc_blob_new(img->pixels, img->npixels, &src.bytes, err);
        }
        if (st != PC_OK) {
            pc_str_fini(&src.file_name);
            pc_surface_fini(&surface);
            return st;
        }
        out->kind = PC_LAYER_SMART;
        pc_smart_object_init(&out->u.smart, &src, m, &surface);
    } else {
        out->kind = PC_LAYER_RASTER;
        out->u.raster = surface;
    }
    *some = true;
    return PC_OK;
}

static pc_status mask(builder *b, const pc_af_image *img, pc_affine to_doc, bool *some, pc_surface *out,
                      pc_error *err)
{
    uint32_t w = 0;
    uint32_t h = 0;
    const uint8_t *rgba = NULL;
    size_t len = 0;
    uint8_t *owned = NULL;
    uint8_t *grey;
    size_t n;
    size_t i;
    bool has;
    pc_status st;
    *some = false;
    st = pixels(b, img, &has, &w, &h, &rgba, &len, &owned, err);
    if (st != PC_OK || !has) {
        return st;
    }
    n = len / 4;
    grey = malloc(n > 0 ? n : 1);
    if (grey == NULL) {
        free(owned);
        return pc_io_nomem(err, "importing an Affinity mask");
    }
    for (i = 0; i < n; i++) {
        grey[i] = rgba[i * 4];
    }
    free(owned);
    st = resample(b, w, h, grey, n, pc_affine_mul(to_doc, affine(&img->transform)), 1, some, out, err);
    free(grey);
    return st;
}

static pc_status rect_mask(pc_rect r, pc_vector_mask *out, pc_error *err)
{
    double pts[4][2];
    pc_path p;
    pc_status st;
    memset(&p, 0, sizeof p);
    pts[0][0] = (double)r.x0;
    pts[0][1] = (double)r.y0;
    pts[1][0] = (double)r.x1;
    pts[1][1] = (double)r.y0;
    pts[2][0] = (double)r.x1;
    pts[2][1] = (double)r.y1;
    pts[3][0] = (double)r.x0;
    pts[3][1] = (double)r.y1;
    p.subpaths = malloc(sizeof *p.subpaths);
    if (p.subpaths == NULL) {
        return pc_io_nomem(err, "importing an Affinity artboard");
    }
    st = pc_subpath_polygon((const double (*)[2])pts, 4, &p.subpaths[0], err);
    if (st != PC_OK) {
        free(p.subpaths);
        return st;
    }
    p.nsubpaths = 1;
    pc_vector_mask_init(out, &p);
    return PC_OK;
}

static pc_status node_layer(builder *b, const pc_af_node *n, pc_affine to_doc, bool top, bool *some,
                            pc_layer *out, pc_error *err);

static pc_status node_shape(builder *b, const pc_af_node *n, pc_affine to_doc, lvec *children, pc_layer *out,
                            pc_error *err)
{
    pc_path p;
    pc_path pmask;
    pc_layer_content c;
    pc_layer own;
    lvec kids = {NULL, 0, 0};
    pc_status st;
    size_t i;
    st = path(&n->path, to_doc, n->even_odd, &p, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_path_clone(&p, &pmask, err);
    if (st != PC_OK) {
        pc_path_fini(&p);
        return st;
    }
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SHAPE;
    st = shape(b, &p, n, to_doc, &c.u.shape, err);
    if (st != PC_OK) {
        pc_path_fini(&p);
        pc_path_fini(&pmask);
        return st;
    }
    if (children->n == 0) {
        pc_path_fini(&pmask);
        return base(b, n, &c, out, err);
    }
    /* A shape clips its children: a group masked by the outline, the shape at its bottom. */
    st = pc_layer_init(&own, n->name != NULL && n->name[0] != '\0' ? n->name : "Shape", &c, err);
    if (st != PC_OK) {
        pc_layer_content_fini(&c);
        pc_path_fini(&pmask);
        return st;
    }
    own.opacity = 1.0f;
    st = lvec_push(&kids, &own, err);
    for (i = 0; st == PC_OK && i < children->n; i++) {
        st = lvec_push(&kids, &children->v[i], err);
    }
    if (st == PC_OK) {
        free(children->v);
        memset(children, 0, sizeof *children);
        st = group(b, n, &kids, out, err);
    }
    if (st != PC_OK) {
        lvec_fini(&kids);
        pc_path_fini(&pmask);
        return st;
    }
    out->has_vector_mask = true;
    pc_vector_mask_init(&out->vector_mask, &pmask);
    if (n->nstrokes > 0) {
        st = warn(b, "strokes of shapes that clip other layers are clipped to the shape", err);
        if (st != PC_OK) {
            pc_layer_fini(out);
        }
    }
    return st;
}

static pc_status node_artboard(builder *b, const pc_af_node *n, pc_affine to_doc, bool top, lvec *children,
                               pc_layer *out, pc_error *err)
{
    pc_rect r = bbox(b, to_doc, n->rect);
    pc_artboard_background bg;
    pc_status st = PC_OK;
    memset(&bg, 0, sizeof bg);
    bg.kind = PC_ARTBOARD_TRANSPARENT;
    if (n->nbackground > 0) {
        if (n->background[0].kind == PC_AF_SOLID) {
            pc_color c = color(&n->background[0].color);
            if (!(c.alpha <= 0.0f)) {
                bg.kind = PC_ARTBOARD_CUSTOM;
                bg.custom = c;
            }
        } else {
            st = warn(b, "artboard backgrounds other than a solid colour import as transparent", err);
        }
    }
    if (st == PC_OK) {
        st = group(b, n, children, out, err);
    }
    if (st != PC_OK) {
        return st;
    }
    if (top) {
        pc_artboard_init(&out->content.u.group.artboard, r);
        out->content.u.group.artboard.background = bg;
        out->content.u.group.has_artboard = true;
        return PC_OK;
    }
    /* PhotoCraft artboards are top-level groups: a nested one clips by a vector mask. */
    st = rect_mask(r, &out->vector_mask, err);
    if (st == PC_OK) {
        out->has_vector_mask = true;
        st = warn(b, "artboards inside groups import as masked groups", err);
    }
    if (st != PC_OK) {
        pc_layer_fini(out);
    }
    return st;
}

static pc_status node_text(builder *b, const pc_af_node *n, pc_affine to_doc, lvec *children, pc_layer *out,
                           pc_error *err)
{
    pc_layer_content c;
    pc_layer l;
    lvec kids = {NULL, 0, 0};
    pc_status st;
    size_t i;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_TEXT;
    st = text(b, &n->text, to_doc, &c.u.text, err);
    if (st == PC_OK) {
        st = base(b, n, &c, &l, err);
    }
    if (st != PC_OK) {
        return st;
    }
    if (children->n == 0) {
        *out = l;
        return PC_OK;
    }
    st = lvec_push(&kids, &l, err);
    for (i = 0; st == PC_OK && i < children->n; i++) {
        st = lvec_push(&kids, &children->v[i], err);
    }
    if (st == PC_OK) {
        free(children->v);
        memset(children, 0, sizeof *children);
        st = group(b, n, &kids, out, err);
    }
    if (st != PC_OK) {
        lvec_fini(&kids);
    }
    return st;
}

/* One Affinity node and its subtree; to_doc maps Affinity document pixels to the canvas. *some
 * false when the node imports as nothing. */
static pc_status node_layer(builder *b, const pc_af_node *n, pc_affine to_doc, bool top, bool *some,
                            pc_layer *out, pc_error *err)
{
    lvec children = {NULL, 0, 0};
    pc_layer l;
    pc_status st = PC_OK;
    size_t i;
    *some = false;
    for (i = 0; st == PC_OK && i < n->nchildren; i++) {
        bool has;
        pc_layer c;
        st = node_layer(b, &n->children[i], to_doc, false, &has, &c, err);
        if (st == PC_OK && has) {
            st = lvec_push(&children, &c, err);
        }
    }
    if (st != PC_OK) {
        lvec_fini(&children);
        return st;
    }
    switch (n->kind) {
    case PC_AF_UNSUPPORTED:
        if (children.n == 0) {
            return PC_OK;
        }
        st = group(b, n, &children, &l, err);
        break;
    case PC_AF_SHAPE:
        st = node_shape(b, n, to_doc, &children, &l, err);
        break;
    case PC_AF_ARTBOARD:
        st = node_artboard(b, n, to_doc, top, &children, &l, err);
        break;
    case PC_AF_TEXT:
        st = node_text(b, n, to_doc, &children, &l, err);
        break;
    case PC_AF_IMAGE: {
        pc_layer_content c;
        bool has;
        st = image(b, &n->image, to_doc, &has, &c, err);
        if (st == PC_OK && has) {
            st = base(b, n, &c, &l, err);
        } else if (st == PC_OK) {
            if (children.n == 0) {
                return PC_OK;
            }
            st = group(b, n, &children, &l, err);
        }
        break;
    }
    case PC_AF_LAYER:
    case PC_AF_GROUP:
    default:
        st = group(b, n, &children, &l, err);
        break;
    }
    lvec_fini(&children);
    if (st != PC_OK) {
        return st;
    }
    if (n->has_mask) {
        pc_path p;
        st = path(&n->mask, to_doc, false, &p, err);
        if (st == PC_OK) {
            if (l.has_vector_mask) {
                pc_vector_mask_fini(&l.vector_mask);
            }
            l.has_vector_mask = true;
            pc_vector_mask_init(&l.vector_mask, &p);
        }
    }
    if (st == PC_OK && n->has_pixel_mask) {
        pc_surface s;
        bool has;
        st = mask(b, &n->pixel_mask, to_doc, &has, &s, err);
        if (st == PC_OK && has) {
            if (l.has_mask) {
                pc_layer_mask_fini(&l.mask);
            }
            l.has_mask = true;
            l.mask.surface = s;
            l.mask.enabled = true;
            l.mask.linked = true;
            l.mask.density = 1.0f;
            l.mask.feather = 0.0f;
        }
    }
    if (st != PC_OK) {
        pc_layer_fini(&l);
        return st;
    }
    *out = l;
    *some = true;
    return PC_OK;
}

static bool has_artboards(const lvec *layers)
{
    size_t i;
    for (i = 0; i < layers->n; i++) {
        if (layers->v[i].content.kind == PC_LAYER_GROUP && layers->v[i].content.u.group.has_artboard) {
            return true;
        }
    }
    return false;
}

static pc_status builder_new(const char *name, const pc_af_document *d, builder *out, pc_error *err)
{
    double w = 0.0;
    double h = 0.0;
    size_t i;
    pc_size size;
    pc_status st;
    /* Spreads (Publisher pages) side by side, 64 px apart. */
    for (i = 0; i < d->nspreads; i++) {
        const pc_af_rect *r = &d->spreads[i].bounds;
        w += pc_rs_max_k(r->x1 - r->x0, 1.0) + (i > 0 ? 64.0 : 0.0);
        h = pc_rs_max(h, pc_rs_max_k(r->y1 - r->y0, 1.0));
    }
    if (!(isfinite(w) && isfinite(h)) || w > MAX_SIDE || h > MAX_SIDE) {
        return pc_io_unsupported(err, "the Affinity document is larger than 300 000 pixels on a side");
    }
    size = pc_size_make(pc_f64_to_u32(pc_rs_max_k(ceil(w), 1.0)), pc_f64_to_u32(pc_rs_max_k(ceil(h), 1.0)));
    memset(out, 0, sizeof *out);
    st = pc_document_init(&out->doc, name, size, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, err);
    if (st != PC_OK) {
        return st;
    }
    out->doc.resolution_dpi = pc_f64_to_f32(d->dpi);
    out->canvas = pc_rect_make(0, 0, (int32_t)size.width, (int32_t)size.height);
    out->format = PC_PIXEL_FORMAT_RGBA8;
    return PC_OK;
}

static pc_status page_group(builder *b, const pc_af_spread *spread, pc_affine to_doc, size_t index,
                            lvec *children, pc_layer *out, pc_error *err)
{
    pc_af_rect page = spread->npages > 0 ? spread->pages[0] : spread->bounds;
    pc_rect r = bbox(b, to_doc, page);
    pc_layer_content c;
    char name[48];
    pc_status st;
    (void)snprintf(name, sizeof name, "Page %zu", index + 1);
    group_content(children, true, &c);
    st = pc_layer_init(out, name, &c, err);
    if (st != PC_OK) {
        pc_layer_content_fini(&c);
        return st;
    }
    pc_artboard_init(&out->content.u.group.artboard, r);
    out->content.u.group.has_artboard = true;
    if (spread->npages > 1) {
        st = warn(b, "facing pages import as one artboard per spread", err);
        if (st != PC_OK) {
            pc_layer_fini(out);
        }
    }
    return st;
}

static pc_status build_document(builder *b, const pc_af_document *d, pc_error *err)
{
    bool pages = d->nspreads > 1;
    lvec layers = {NULL, 0, 0};
    double x = 0.0;
    pc_status st = PC_OK;
    size_t i;
    for (i = 0; i < d->nspreads; i++) {
        if (d->spreads[i].npages > 1) {
            pages = true;
        }
    }
    for (i = 0; st == PC_OK && i < d->nspreads; i++) {
        const pc_af_spread *spread = &d->spreads[i];
        pc_affine to_doc = pc_affine_translate(x - spread->bounds.x0, -spread->bounds.y0);
        lvec children = {NULL, 0, 0};
        size_t j;
        for (j = 0; st == PC_OK && j < spread->nnodes; j++) {
            bool has;
            pc_layer l;
            st = node_layer(b, &spread->nodes[j], to_doc, true, &has, &l, err);
            if (st == PC_OK && has) {
                st = lvec_push(&children, &l, err);
            }
        }
        if (st == PC_OK && pages) {
            /* Each page becomes an artboard holding the art drawn on it. */
            pc_layer g;
            st = page_group(b, spread, to_doc, i, &children, &g, err);
            if (st == PC_OK) {
                st = lvec_push(&layers, &g, err);
            }
        } else {
            for (j = 0; st == PC_OK && j < children.n; j++) {
                st = lvec_push(&layers, &children.v[j], err);
            }
            free(children.v);
            memset(&children, 0, sizeof children);
        }
        lvec_fini(&children);
        x += pc_rs_max_k(spread->bounds.x1 - spread->bounds.x0, 1.0) + 64.0;
    }
    if (st == PC_OK && d->nspreads > 0 && !d->spreads[0].transparent && !has_artboards(&layers)) {
        /* An opaque page: a white background below the art. */
        static const float WHITE[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        pc_layer_content c;
        pc_layer bg;
        memset(&c, 0, sizeof c);
        c.kind = PC_LAYER_RASTER;
        pc_surface_init_default(&c.u.raster, b->format, WHITE, 4);
        pc_surface_prune(&c.u.raster);
        st = pc_layer_init(&bg, "Background", &c, err);
        if (st != PC_OK) {
            pc_layer_content_fini(&c);
        } else {
            bg.locks.transparency = true;
            st = lvec_push(&layers, &bg, err);
            if (st == PC_OK) {
                pc_layer top = layers.v[layers.n - 1];
                memmove(&layers.v[1], &layers.v[0], (layers.n - 1) * sizeof *layers.v);
                layers.v[0] = top;
            }
        }
    }
    if (st != PC_OK) {
        lvec_fini(&layers);
        return st;
    }
    b->doc.layers = layers.v;
    b->doc.nlayers = layers.n;
    return PC_OK;
}

/* The embedded preview, for files whose native document can't be read. */
static pc_status preview(const char *name, const uint8_t *bytes, size_t len, const pc_error *native,
                         pc_io_import_result *out, pc_error *err)
{
    pc_af_preview p;
    pc_cdecode_opts opts;
    pc_cimage img;
    pc_io_import_result r;
    pc_error e;
    pc_status st = pc_af_preview_read(bytes, len, &p, &e);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
    }
    if (st != PC_OK) {
        return pc_io_unsupported(err, "%s; its embedded preview can't be read either (%s)", native->msg,
                                 e.msg);
    }
    opts = pc_cdecode_opts_default();
    opts.limits.max_width = 4096;
    opts.limits.max_height = 4096;
    opts.limits.max_pixels = (uint64_t)4096 * 4096;
    opts.limits.max_alloc = (uint64_t)128 << 20;
    st = pc_cdecode_with(p.png, p.len, &opts, &img, err);
    if (st != PC_OK) {
        return pc_io_err_prefix(st, err, "codec: ");
    }
    st = pc_io_image_to_document(name, &img, &r, err);
    pc_cimage_fini(&img);
    if (st != PC_OK) {
        return st;
    }
    if (r.document.nlayers > 0) {
        pc_layer *layer = &r.document.layers[0];
        st = pc_str_set_c(&layer->name, "Affinity preview", err);
        memset(&layer->locks, 0, sizeof layer->locks);
    }
    r.source_read_only = true;
    r.preview_only = true;
    if (st == PC_OK) {
        st = pc_io_warn_at(
            &r.warnings, 0, err,
            "The native Affinity document could not be read (%s). Opened only its embedded "
            "%u\xc3\x97%u PNG preview, which may be smaller than the document: no layers, vectors, "
            "text or pages. Save a new copy; the Affinity source cannot be saved back. Export PSD or "
            "PNG from Affinity for full-resolution artwork.",
            native->msg, (unsigned)p.width, (unsigned)p.height);
    }
    if (st != PC_OK) {
        pc_io_import_result_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}

pc_status pc_io_affinity_import(const char *name, const uint8_t *bytes, size_t len, pc_io_import_result *out,
                                pc_error *err)
{
    pc_af_document d;
    pc_error native;
    builder b;
    pc_io_import_result r;
    pc_status st;
    size_t i;
    st = pc_af_read(bytes, len, pc_af_limits_default(), &d, &native);
    if (st == PC_ERR_NOMEM) {
        return pc_fail(err, PC_ERR_NOMEM, "%s", native.msg);
    }
    if (st != PC_OK) {
        return preview(name, bytes, len, &native, out, err);
    }
    st = builder_new(name, &d, &b, err);
    if (st != PC_OK) {
        pc_af_document_fini(&d);
        return st;
    }
    st = build_document(&b, &d, err);
    memset(&r, 0, sizeof r);
    pc_io_warnings_init(&r.warnings);
    for (i = 0; st == PC_OK && i < d.nwarnings; i++) {
        st = pc_io_warn(&r.warnings, err, "Affinity: not imported or approximated: %s.", d.warnings[i]);
    }
    for (i = 0; st == PC_OK && i < b.nwarnings; i++) {
        st = pc_io_warn(&r.warnings, err, "Affinity: %s.", b.warnings[i]);
    }
    pc_af_document_fini(&d);
    /* Type layers draw from their model, within the import's raster bound: the sizes and frames
     * come from an untrusted file. */
    if (st == PC_OK) {
        st = pc_io_text_prepare(&b.doc, err);
    }
    if (st != PC_OK) {
        pc_io_warnings_fini(&r.warnings);
        builder_fini(&b);
        return st;
    }
    r.document = b.doc;
    builder_free_warnings(&b);
    /* Save never writes back over the Affinity file. */
    r.source_read_only = true;
    r.preview_only = false;
    *out = r;
    return PC_OK;
}
