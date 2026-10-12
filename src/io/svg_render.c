/* Rendering of the render tree, as resvg 0.45 does it (render.rs, path.rs, clip.rs, mask.rs,
 * image.rs): paths are filled and stroked through svg_raster.c's coverage and svg_pipe.c's port
 * of tiny-skia's pipeline; groups that need it are isolated on a layer the size of their layer
 * bounding box (filters, clip paths and masks applied there) and drawn back with their opacity
 * and blend mode; pattern paints are rendered into a tile pixmap; raster images are drawn as
 * pattern shaders and nested SVG drawings as sub-renders.
 *
 * Every scratch pixmap counts against a per-render budget (MAX_SCRATCH pixels) and nesting is
 * bounded (MAX_RENDER_DEPTH), so hostile drawings (deep groups, recursive patterns, huge layers)
 * end in less output, never in unbounded work. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "io/svg_internal.h"

#define MAX_RENDER_DEPTH 256
/* Scratch pixmap pixels alive at once (1 GiB of RGBA8) and allocated in all by one render (the
 * work bound for drawings made of thousands of isolated layers). */
#define MAX_LIVE ((size_t)1 << 28)
#define MAX_TOTAL ((size_t)1 << 32)

bool sv_pixmap_new(uint32_t w, uint32_t h, sv_pixmap *out)
{
    uint64_t n = (uint64_t)w * h;
    if (w == 0 || h == 0 || n > ((uint64_t)1 << 28)) {
        return false;
    }
    out->px = calloc((size_t)n, 4);
    if (out->px == NULL) {
        return false;
    }
    out->w = w;
    out->h = h;
    return true;
}

void sv_pixmap_fini(sv_pixmap *pm)
{
    if (pm != NULL) {
        free(pm->px);
        pm->px = NULL;
        pm->w = 0;
        pm->h = 0;
    }
}

bool sv_rctx_pixmap(sv_rctx *r, uint32_t w, uint32_t h, sv_pixmap *out)
{
    uint64_t n = (uint64_t)w * h;
    if (w == 0 || h == 0 || n > MAX_LIVE || r->pixels + (size_t)n > MAX_LIVE ||
        r->total + (size_t)n > MAX_TOTAL) {
        return false;
    }
    if (!sv_pixmap_new(w, h, out)) {
        r->nomem = true;
        return false;
    }
    r->pixels += (size_t)n;
    r->total += (size_t)n;
    return true;
}

void sv_rctx_release(sv_rctx *r, sv_pixmap *pm)
{
    size_t n = (size_t)pm->w * pm->h;
    r->pixels = r->pixels >= n ? r->pixels - n : 0;
    sv_pixmap_fini(pm);
}

static void rctx_init(sv_rctx *r, uint32_t w, uint32_t h)
{
    int64_t iw = w;
    int64_t ih = h;
    memset(r, 0, sizeof *r);
    /* IntRect::from_xywh(-2w, -2h, 5w, 5h) */
    r->max_l = (int32_t)(-2 * iw);
    r->max_t = (int32_t)(-2 * ih);
    r->max_r = (int32_t)(3 * iw);
    r->max_b = (int32_t)(3 * ih);
}

static sv_tsblend ts_blend(sv_blend b)
{
    static const sv_tsblend M[16] = {TSB_SOURCE_OVER, TSB_MULTIPLY,   TSB_SCREEN,      TSB_OVERLAY,
                                     TSB_DARKEN,      TSB_LIGHTEN,    TSB_COLOR_DODGE, TSB_COLOR_BURN,
                                     TSB_HARD_LIGHT,  TSB_SOFT_LIGHT, TSB_DIFFERENCE,  TSB_EXCLUSION,
                                     TSB_HUE,         TSB_SATURATION, TSB_COLOR,       TSB_LUMINOSITY};
    return (unsigned)b < 16u ? M[b] : TSB_SOURCE_OVER;
}

static void render_node(sv_rctx *r, const sv_unode *n, sv_ts ts, sv_pixmap *pm);

/* ---------------------------------------------------------------- paints */

/* resvg's render_pattern_pixmap: the tile rendered at the device scale (owned by *tile) and the
 * pattern's shader transform before the path's. False when there is nothing to draw. */
static bool pattern_pixmap(sv_rctx *r, const sv_pattern *pat, sv_ts ts, sv_pixmap *tile, sv_ts *patt_ts)
{
    float sx;
    float sy;
    float tw;
    float th;
    ts_get_scale(ts_pre_concat(ts, pat->transform), &sx, &sy);
    tw = roundf(rect_w(pat->rect) * sx);
    th = roundf(rect_h(pat->rect) * sy);
    if (!(tw >= 1.0f && th >= 1.0f && tw < 4294967296.0f && th < 4294967296.0f) ||
        r->depth >= MAX_RENDER_DEPTH) {
        return false;
    }
    if (!sv_rctx_pixmap(r, (uint32_t)tw, (uint32_t)th, tile)) {
        return false;
    }
    r->depth++;
    sv_render_children(r, pat->root, ts_scale(sx, sy), tile);
    r->depth--;
    *patt_ts = ts_pre_concat(pat->transform, ts_translate(pat->rect.l, pat->rect.t));
    *patt_ts = ts_pre_concat(*patt_ts, ts_scale(1.0f / sx, 1.0f / sy));
    return true;
}

/* The shader of a paint drawn with path transform ts: 1 ready, 0 nothing to draw. */
static int make_shader(sv_rctx *r, const sv_paint *p, float opacity, sv_ts ts, sv_shader *s, sv_pixmap *tile)
{
    switch (p->kind) {
    case PAINT_LINEAR:
    case PAINT_RADIAL: {
        int k = sv_shader_gradient(s, p->grad, opacity, ts);
        if (k < 0) {
            r->nomem = true;
            return 0;
        }
        return k;
    }
    case PAINT_PATTERN: {
        sv_ts patt;
        if (!pattern_pixmap(r, p->pat, ts, tile, &patt)) {
            return 0;
        }
        if (sv_shader_pattern(s, tile, true, SQ_BICUBIC, opacity, ts_post_concat(patt, ts)) == 0) {
            sv_rctx_release(r, tile);
            return 0;
        }
        return 1;
    }
    case PAINT_COLOR:
    default:
        sv_shader_solid(s, p->r, p->g, p->b, opacity);
        return 1;
    }
}

static bool cov_ok(sv_rctx *r, pc_status st)
{
    if (st == PC_ERR_NOMEM) {
        r->nomem = true;
    }
    return st == PC_OK;
}

/* path::fill_path */
static void fill_path(sv_rctx *r, const sv_unode *p, sv_tsblend mode, sv_ts ts, sv_pixmap *pm)
{
    const sv_fill *f = p->fill;
    sv_shader s;
    sv_pixmap tile = {0, 0, NULL};
    sv_pipe_spans ps;
    sv_spans out;
    if (f == NULL || p->data == NULL) {
        return;
    }
    /* horizontal and vertical lines cannot be filled */
    if (rect_w(p->data->bounds) == 0.0f || rect_h(p->data->bounds) == 0.0f) {
        return;
    }
    if (make_shader(r, &f->paint, f->opacity, ts, &s, &tile) == 0) {
        return;
    }
    if (sv_pipe_spans_init(&ps, pm, &s, mode, &out)) {
        (void)cov_ok(r, sv_fill_path(p->data, ts, f->evenodd, !p->crisp, pm->w, pm->h, &out));
    }
    sv_shader_fini(&s);
    sv_rctx_release(r, &tile);
}

static bool pre_scales(sv_tsblend m)
{
    return m == TSB_DESTINATION || m == TSB_DESTINATION_OVER || m == TSB_PLUS || m == TSB_DESTINATION_OUT ||
           m == TSB_SOURCE_ATOP || m == TSB_SOURCE_OVER || m == TSB_XOR;
}

/* path::stroke_path + Pixmap::stroke_path */
static void stroke_path(sv_rctx *r, const sv_unode *p, sv_tsblend mode, sv_ts ts, sv_pixmap *pm)
{
    const sv_stroke *st = p->stroke;
    sv_shader s;
    sv_pixmap tile = {0, 0, NULL};
    float hair;
    bool ok = true;
    sv_pipe_spans ps;
    sv_spans out;
    if (st == NULL || p->data == NULL || !(st->width >= 0.0f)) {
        return;
    }
    if (make_shader(r, &st->paint, st->opacity, ts, &s, &tile) == 0) {
        return;
    }
    hair = sv_hairline_coverage(st->width, ts);
    if (p->crisp && st->width != 0.0f) {
        hair = -1.0f;
    }
    /* a hairline thinner than a pixel is drawn with its coverage folded into the opacity */
    if (hair >= 0.0f && hair != 1.0f && pre_scales(mode)) {
        int32_t scale = (int32_t)(hair * 256.0f);
        int32_t na = (255 * scale) >> 8;
        ok = sv_shader_apply_opacity(&s, (float)na / 255.0f);
        r->nomem = r->nomem || !ok;
    }
    if (ok && sv_pipe_spans_init(&ps, pm, &s, mode, &out)) {
        (void)cov_ok(r, sv_stroke_path(p->data, ts, st, !p->crisp, pm->w, pm->h, &out));
    }
    sv_shader_fini(&s);
    sv_rctx_release(r, &tile);
}

static void render_path(sv_rctx *r, const sv_unode *p, sv_ts ts, sv_pixmap *pm)
{
    if (!p->visible) {
        return;
    }
    if (!p->stroke_first) {
        fill_path(r, p, TSB_SOURCE_OVER, ts, pm);
        stroke_path(r, p, TSB_SOURCE_OVER, ts, pm);
    } else {
        stroke_path(r, p, TSB_SOURCE_OVER, ts, pm);
        fill_path(r, p, TSB_SOURCE_OVER, ts, pm);
    }
}

/* ---------------------------------------------------------------- images */

static void render_raster(sv_rctx *r, const sv_unode *n, sv_ts ts, sv_pixmap *pm)
{
    sv_pixmap img;
    sv_shader s;
    size_t i;
    if (!sv_rctx_pixmap(r, n->iw, n->ih, &img)) {
        return;
    }
    for (i = 0; i < (size_t)n->iw * n->ih; i++) {
        const uint8_t *p = n->rgba + i * 4;
        uint8_t *d = img.px + i * 4;
        int k;
        for (k = 0; k < 3; k++) {
            uint32_t prod = (uint32_t)p[k] * p[3] + 128u;
            d[k] = (uint8_t)((prod + (prod >> 8)) >> 8);
        }
        d[3] = p[3];
    }
    if (sv_shader_pattern(&s, &img, false, (sv_quality)n->quality, 1.0f, ts) != 0) {
        sv_rect rr;
        sv_pipe_spans ps;
        sv_spans out;
        /* Pixmap::fill_rect (anti-aliased) */
        if (rect_from_xywh(0.0f, 0.0f, (float)n->iw, (float)n->ih, &rr) &&
            sv_pipe_spans_init(&ps, pm, &s, TSB_SOURCE_OVER, &out)) {
            (void)cov_ok(r, sv_fill_rect(rr, ts, true, pm->w, pm->h, &out));
        }
        sv_shader_fini(&s);
    }
    sv_rctx_release(r, &img);
}

static void render_image(sv_rctx *r, const sv_unode *n, sv_ts ts, sv_pixmap *pm)
{
    if (!n->visible) {
        return;
    }
    if (n->svg != NULL) {
        /* render_vector: a sub-render the size of the target, drawn back source-over */
        sv_pixmap sub;
        sv_rctx r2;
        if (r->depth >= MAX_RENDER_DEPTH || !sv_rctx_pixmap(r, pm->w, pm->h, &sub)) {
            return;
        }
        rctx_init(&r2, pm->w, pm->h);
        r2.depth = r->depth + 1;
        r2.pixels = r->pixels;
        r2.total = r->total;
        sv_render_children(&r2, n->svg->root, ts, &sub);
        r->nomem = r->nomem || r2.nomem;
        r->total = r2.total;
        sv_draw_pixmap(pm, 0, 0, &sub, 1.0f, TSB_SOURCE_OVER);
        sv_rctx_release(r, &sub);
        return;
    }
    if (n->rgba != NULL && n->iw > 0 && n->ih > 0) {
        render_raster(r, n, ts, pm);
    }
}

/* ---------------------------------------------------------------- clip paths */

static void clip_apply(sv_rctx *r, const sv_clip *clip, sv_ts ts, sv_pixmap *pm);

static void clip_children(sv_rctx *r, const sv_unode *g, sv_tsblend mode, sv_ts ts, sv_pixmap *pm);

/* clip.rs clip_group */
static void clip_group(sv_rctx *r, const sv_unode *g, const sv_clip *clip, sv_ts ts, sv_pixmap *pm)
{
    sv_pixmap cp;
    if (!sv_rctx_pixmap(r, pm->w, pm->h, &cp)) {
        return;
    }
    clip_children(r, g, TSB_SOURCE_OVER, ts, &cp);
    clip_apply(r, clip, ts, &cp);
    sv_draw_pixmap(pm, 0, 0, &cp, 1.0f, TSB_XOR);
    sv_rctx_release(r, &cp);
}

static void clip_children(sv_rctx *r, const sv_unode *g, sv_tsblend mode, sv_ts ts, sv_pixmap *pm)
{
    size_t i;
    if (r->depth >= MAX_RENDER_DEPTH) {
        return;
    }
    r->depth++;
    for (i = 0; i < g->nchildren && !r->nomem; i++) {
        const sv_unode *ch = g->children[i];
        if (ch->kind == UN_PATH) {
            if (ch->visible) {
                fill_path(r, ch, mode, ts, pm);
            }
        } else if (ch->kind == UN_GROUP) {
            sv_ts gts = ts_pre_concat(ts, ch->transform);
            if (ch->clip != NULL) {
                clip_group(r, ch, ch->clip, gts, pm);
            } else {
                clip_children(r, ch, mode, gts, pm);
            }
        } else if (ch->kind == UN_TEXT && ch->flat != NULL) {
            clip_children(r, ch->flat, mode, ts, pm);
        }
    }
    r->depth--;
}

/* clip.rs apply */
static void clip_apply(sv_rctx *r, const sv_clip *clip, sv_ts ts, sv_pixmap *pm)
{
    sv_pixmap cp;
    uint8_t *mask;
    size_t i;
    size_t n = (size_t)pm->w * pm->h;
    if (r->depth >= MAX_RENDER_DEPTH || !sv_rctx_pixmap(r, pm->w, pm->h, &cp)) {
        return;
    }
    for (i = 0; i < n; i++) {
        cp.px[i * 4 + 3] = 255;
    }
    r->depth++;
    clip_children(r, clip->root, TSB_CLEAR, ts_pre_concat(ts, clip->transform), &cp);
    if (clip->clip != NULL) {
        clip_apply(r, clip->clip, ts, pm);
    }
    r->depth--;
    mask = malloc(n);
    if (mask == NULL) {
        r->nomem = true;
        sv_rctx_release(r, &cp);
        return;
    }
    sv_mask_from_pixmap(&cp, false, mask);
    for (i = 0; i < n; i++) {
        mask[i] = (uint8_t)(255u - mask[i]);
    }
    sv_pixmap_apply_mask(pm, mask);
    free(mask);
    sv_rctx_release(r, &cp);
}

/* ---------------------------------------------------------------- masks */

static void mask_apply(sv_rctx *r, const sv_mask *mask, sv_ts ts, sv_pixmap *pm)
{
    sv_pixmap mp;
    uint8_t *m;
    size_t n = (size_t)pm->w * pm->h;
    if (mask->root == NULL || mask->root->nchildren == 0) {
        memset(pm->px, 0, n * 4);
        return;
    }
    if (r->depth >= MAX_RENDER_DEPTH || !sv_rctx_pixmap(r, pm->w, pm->h, &mp)) {
        return;
    }
    m = calloc(n, 1);
    if (m == NULL) {
        r->nomem = true;
        sv_rctx_release(r, &mp);
        return;
    }
    {
        /* the mask's region as an anti-aliased alpha mask */
        sv_arena a;
        const sv_path *rp;
        sv_arena_init(&a);
        rp = path_from_rect(&a, mask->rect);
        if (rp != NULL) {
            sv_cov c;
            if (cov_ok(r, sv_fill_cov(rp, ts, false, true, pm->w, pm->h, &c)) && c.a != NULL) {
                uint32_t y;
                for (y = 0; y < c.h; y++) {
                    memcpy(m + (size_t)(c.y0 + (int32_t)y) * pm->w + (size_t)c.x0, c.a + (size_t)y * c.w,
                           c.w);
                }
            }
            sv_cov_fini(&c);
        } else {
            r->nomem = true;
        }
        sv_arena_fini(&a);
    }
    r->depth++;
    sv_render_children(r, mask->root, ts, &mp);
    sv_pixmap_apply_mask(&mp, m);
    if (mask->mask != NULL) {
        mask_apply(r, mask->mask, ts, pm);
    }
    r->depth--;
    sv_mask_from_pixmap(&mp, !mask->alpha, m);
    sv_pixmap_apply_mask(pm, m);
    free(m);
    sv_rctx_release(r, &mp);
}

/* ---------------------------------------------------------------- groups */

static bool should_isolate(const sv_unode *g)
{
    return g->isolate || g->opacity != 1.0f || g->clip != NULL || g->mask != NULL || g->nfilters > 0 ||
           g->blend != BM_NORMAL;
}

/* IntRect::from_xywh: false when it doesn't fit i32 or is empty. */
static bool int_rect(int64_t x, int64_t y, int64_t w, int64_t h, int64_t out[4])
{
    if (w <= 0 || h <= 0 || x < INT32_MIN || y < INT32_MIN || x + w > INT32_MAX || y + h > INT32_MAX ||
        w > UINT32_MAX || h > UINT32_MAX) {
        return false;
    }
    out[0] = x;
    out[1] = y;
    out[2] = x + w;
    out[3] = y + h;
    return true;
}

/* geom::fit_to_rect */
static bool fit_to(sv_rctx *r, int64_t b[4])
{
    b[0] = b[0] < r->max_l ? r->max_l : b[0];
    b[1] = b[1] < r->max_t ? r->max_t : b[1];
    b[2] = b[2] > r->max_r ? r->max_r : b[2];
    b[3] = b[3] > r->max_b ? r->max_b : b[3];
    return b[2] > b[0] && b[3] > b[1];
}

/* f32 -> i32 / u32 `as` casts (saturating, NaN 0). */
static int64_t as_i32(float v)
{
    if (!(v == v)) {
        return 0;
    }
    return v <= -2147483648.0f ? INT32_MIN : v >= 2147483647.0f ? INT32_MAX : (int64_t)v;
}

static int64_t as_u32(float v)
{
    if (!(v > 0.0f)) {
        return 0;
    }
    return v >= 4294967295.0f ? (int64_t)UINT32_MAX : (int64_t)v;
}

static void render_group(sv_rctx *r, const sv_unode *g, sv_ts ts, sv_pixmap *pm)
{
    sv_ts t = ts_pre_concat(ts, g->transform);
    sv_rect bb;
    int64_t ib[4];
    sv_pixmap layer;
    sv_ts shift;
    if (r->depth >= MAX_RENDER_DEPTH) {
        return;
    }
    if (!should_isolate(g)) {
        r->depth++;
        sv_render_children(r, g, t, pm);
        r->depth--;
        return;
    }
    if (!nzrect_transform(g->layer_bbox, t, &bb)) {
        return;
    }
    if (g->nfilters == 0) {
        /* expanded by 2 px on each side so anti-aliased edges are not clipped */
        if (!int_rect(as_i32(floorf(bb.l)) - 2, as_i32(floorf(bb.t)) - 2, as_u32(ceilf(rect_w(bb))) + 4,
                      as_u32(ceilf(rect_h(bb))) + 4, ib)) {
            return;
        }
    } else {
        /* the filter region already clips: Rect::to_int_rect */
        int64_t w = as_u32(ceilf(rect_w(bb)));
        int64_t h = as_u32(ceilf(rect_h(bb)));
        if (!int_rect(as_i32(floorf(bb.l)), as_i32(floorf(bb.t)), w > 1 ? w : 1, h > 1 ? h : 1, ib)) {
            return;
        }
    }
    if (!fit_to(r, ib)) {
        return;
    }
    {
        float dx = bb.l;
        float dy = bb.t;
        dx -= bb.l - (float)ib[0];
        dy -= bb.t - (float)ib[1];
        shift = ts_translate(-dx, -dy);
    }
    t = ts_pre_concat(shift, t);
    if (!sv_rctx_pixmap(r, (uint32_t)(ib[2] - ib[0]), (uint32_t)(ib[3] - ib[1]), &layer)) {
        return;
    }
    r->depth++;
    sv_render_children(r, g, t, &layer);
    if (g->nfilters > 0) {
        sv_filters_apply(r, g, t, &layer);
    }
    if (g->clip != NULL) {
        clip_apply(r, g->clip, t, &layer);
    }
    if (g->mask != NULL) {
        mask_apply(r, g->mask, t, &layer);
    }
    r->depth--;
    sv_draw_pixmap(pm, (int32_t)ib[0], (int32_t)ib[1], &layer, g->opacity, ts_blend(g->blend));
    sv_rctx_release(r, &layer);
}

static void render_node(sv_rctx *r, const sv_unode *n, sv_ts ts, sv_pixmap *pm)
{
    if (r->nomem) {
        return;
    }
    switch (n->kind) {
    case UN_GROUP:
        render_group(r, n, ts, pm);
        break;
    case UN_PATH:
        render_path(r, n, ts, pm);
        break;
    case UN_IMAGE:
        render_image(r, n, ts, pm);
        break;
    case UN_TEXT:
        if (n->flat != NULL) {
            render_group(r, n->flat, ts, pm); /* resvg: render_group(text.flattened(), ..) */
        }
        break;
    default:
        break;
    }
}

void sv_render_children(sv_rctx *r, const sv_unode *g, sv_ts ts, sv_pixmap *pm)
{
    size_t i;
    for (i = 0; i < g->nchildren && !r->nomem; i++) {
        render_node(r, g->children[i], ts, pm);
    }
}

pc_status sv_render_tree(const sv_tree *t, sv_ts transform, sv_pixmap *pm, pc_error *err)
{
    sv_rctx r;
    rctx_init(&r, pm->w, pm->h);
    sv_render_children(&r, t->root, transform, pm);
    if (r.nomem) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory rendering an SVG");
    }
    return PC_OK;
}

pc_status sv_render_node(const sv_unode *n, sv_ts transform, sv_pixmap *pm, pc_error *err)
{
    sv_rctx r;
    rctx_init(&r, pm->w, pm->h);
    render_node(&r, n, transform, pm);
    if (r.nomem) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory rendering an SVG");
    }
    return PC_OK;
}
