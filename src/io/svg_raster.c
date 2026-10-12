/* Path drawing for the SVG renderer: tiny-skia 0.11.4's painter.rs (Pixmap::fill_path,
 * stroke_path, fill_rect; Mask::fill_path) on top of the svg_ts_*.c port of its scan converters,
 * stroker and dasher (see svg_ts.h). Pixels leave as sv_spans in tiny-skia's blitter order, so
 * svg_pipe.c composites them as RasterPipelineBlitter does.
 *
 * Like the reference: paths are transformed in f32 before filling; strokes are dashed and built
 * in the path's own space (res_scale from the transform) and then filled with the transform;
 * strokes at most one device pixel wide are drawn as anti-aliased hairlines; empty, flat and
 * too-big paths draw nothing; targets wider or taller than 8191 px are drawn in 8191 px tiles
 * (DrawTiler), the path translated into each tile and back in f32. */
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "io/svg_internal.h"
#include "io/svg_ts.h"

#define TILE 8191u

static pc_status pb_status(const ts_pb *b)
{
    return b->limit ? PC_ERR_LIMIT : b->failed ? PC_ERR_NOMEM : PC_OK;
}

static bool tiled(uint32_t w, uint32_t h)
{
    return w > TILE || h > TILE;
}

/* painter::is_too_big_for_math */
static bool too_big(const ts_path *p)
{
    const float max = TS_SCALAR_MAX * 0.25f;
    sv_rect b = p->bounds;
    return !(b.l >= -max && b.t >= -max && b.r <= max && b.b <= max);
}

typedef enum { DRAW_FILL, DRAW_HAIR } draw_kind;

typedef struct draw_op {
    draw_kind kind;
    bool evenodd, aa;
    sv_cap cap;
} draw_op;

static pc_status draw_clip(const draw_op *op, const ts_path *p, ts_clip clip, const sv_spans *out,
                           uint32_t ox, uint32_t oy)
{
    if (op->kind == DRAW_FILL) {
        return ts_scan_fill(p, op->evenodd, op->aa, clip, out, ox, oy);
    }
    ts_scan_hairline(p, op->cap, op->aa, clip, out, ox, oy);
    return PC_OK;
}

/* Draws a device-space path into the w x h target, through DrawTiler when it is too large. */
static pc_status draw_device(const draw_op *op, const ts_path *p, uint32_t w, uint32_t h, const sv_spans *out)
{
    ts_pb tmp;
    ts_path cur;
    uint32_t xo = 0;
    uint32_t yo = 0;
    pc_status st = PC_OK;
    if (w == 0 || h == 0) {
        return PC_OK;
    }
    if (!tiled(w, h)) {
        ts_clip c = {0, 0, w, h};
        return draw_clip(op, p, c, out, 0, 0);
    }
    ts_pb_init(&tmp);
    if (!ts_path_transform(p, ts_identity(), &tmp, &cur)) {
        st = pb_status(&tmp);
        ts_pb_fini(&tmp);
        return st;
    }
    while (xo < w && yo < h && st == PC_OK) {
        ts_clip c = {0, 0, w - xo < TILE ? w - xo : TILE, h - yo < TILE ? h - yo : TILE};
        uint32_t tx = xo;
        uint32_t ty = yo;
        sv_ts there = ts_translate(-(float)tx, -(float)ty);
        sv_ts back = ts_translate((float)tx, (float)ty);
        xo += TILE;
        if (xo >= w) {
            xo = 0;
            yo += TILE;
        }
        if (!ts_is_identity(there)) {
            ts_map_points(there, tmp.pts, tmp.np);
            if (!ts_rect_from_points(tmp.pts, tmp.np, &cur.bounds)) {
                break;
            }
        }
        st = draw_clip(op, &cur, c, out, tx, ty);
        if (!ts_is_identity(back)) {
            ts_map_points(back, tmp.pts, tmp.np);
            if (!ts_rect_from_points(tmp.pts, tmp.np, &cur.bounds)) {
                break;
            }
        }
    }
    ts_pb_fini(&tmp);
    return st;
}

/* Pixmap::fill_path with an identity transform */
static pc_status fill_device(const ts_path *p, bool evenodd, bool aa, uint32_t w, uint32_t h,
                             const sv_spans *out)
{
    draw_op op;
    if (ts_nearly_zero(p->bounds.r - p->bounds.l) || ts_nearly_zero(p->bounds.b - p->bounds.t) ||
        too_big(p)) {
        return PC_OK;
    }
    op.kind = DRAW_FILL;
    op.evenodd = evenodd;
    op.aa = aa;
    op.cap = CAP_BUTT;
    return draw_device(&op, p, w, h, out);
}

static pc_status fill_ts(const ts_path *src, sv_ts ts, bool evenodd, bool aa, uint32_t w, uint32_t h,
                         const sv_spans *out)
{
    ts_pb tmp;
    ts_path p;
    pc_status st;
    if (ts_is_identity(ts)) {
        return fill_device(src, evenodd, aa, w, h, out);
    }
    ts_pb_init(&tmp);
    if (!ts_path_transform(src, ts, &tmp, &p)) {
        st = pb_status(&tmp);
    } else {
        st = fill_device(&p, evenodd, aa, w, h, out);
    }
    ts_pb_fini(&tmp);
    return st;
}

pc_status sv_fill_path(const sv_path *p, sv_ts ts, bool evenodd, bool aa, uint32_t w, uint32_t h,
                       const sv_spans *out)
{
    ts_path src;
    if (p == NULL || p->nverbs == 0) {
        return PC_OK;
    }
    src = ts_path_of(p);
    return fill_ts(&src, ts, evenodd, aa, w, h, out);
}

pc_status sv_fill_rect(sv_rect r, sv_ts ts, bool aa, uint32_t w, uint32_t h, const sv_spans *out)
{
    static const uint8_t verbs[5] = {V_MOVE, V_LINE, V_LINE, V_LINE, V_CLOSE};
    sv_pt pts[4];
    ts_path p;
    if (w == 0 || h == 0) {
        return PC_OK;
    }
    if (ts_is_identity(ts) && !tiled(w, h)) {
        ts_clip c = {0, 0, w, h};
        ts_scan_fill_rect(r, aa, c, out);
        return PC_OK;
    }
    /* PathBuilder::from_rect */
    pts[0] = ts_pt(r.l, r.t);
    pts[1] = ts_pt(r.r, r.t);
    pts[2] = ts_pt(r.r, r.b);
    pts[3] = ts_pt(r.l, r.b);
    p.verbs = verbs;
    p.pts = pts;
    p.nv = 5;
    p.np = 4;
    p.bounds = r;
    return fill_ts(&p, ts, false, aa, w, h, out);
}

float sv_hairline_coverage(float width, sv_ts ts)
{
    float l0;
    float l1;
    sv_pt pt[2];
    if (width == 0.0f) {
        return 1.0f;
    }
    ts.tx = 0.0f;
    ts.ty = 0.0f;
    pt[0] = ts_pt(width, 0.0f);
    pt[1] = ts_pt(0.0f, width);
    ts_map_points(ts, pt, 2);
    {
        float x = fabsf(pt[0].x);
        float y = fabsf(pt[0].y);
        l0 = x < y ? y + x * 0.5f : x + y * 0.5f;
        x = fabsf(pt[1].x);
        y = fabsf(pt[1].y);
        l1 = x < y ? y + x * 0.5f : x + y * 0.5f;
    }
    if (l0 <= 1.0f && l1 <= 1.0f) {
        return (l0 + l1) * 0.5f;
    }
    return -1.0f;
}

pc_status sv_stroke_path(const sv_path *sp, sv_ts ts, const sv_stroke *s, bool aa, uint32_t w, uint32_t h,
                         const sv_spans *out)
{
    ts_path src;
    ts_path path;
    ts_pb dashed;
    ts_pb outline;
    ts_dash dash;
    float res_scale;
    pc_status st = PC_OK;
    bool ok = true;
    if (sp == NULL || sp->nverbs == 0 || !(s->width >= 0.0f)) {
        return PC_OK;
    }
    src = ts_path_of(sp);
    path = src;
    res_scale = ts_res_scale(ts);
    ts_pb_init(&dashed);
    ts_pb_init(&outline);
    if (s->dash != NULL && ts_dash_new(s->dash, s->ndash, s->dashoffset, &dash)) {
        st = ts_dash_path(&src, &dash, res_scale, &dashed, &ok);
        if (st != PC_OK || !ok || !ts_pb_finish(&dashed, &path)) {
            goto done;
        }
    }
    if (s->width == 0.0f || (aa && sv_hairline_coverage(s->width, ts) >= 0.0f)) {
        draw_op op;
        op.kind = DRAW_HAIR;
        op.evenodd = false;
        op.aa = aa;
        op.cap = s->cap;
        if (!ts_is_identity(ts)) {
            ts_path dev;
            if (!ts_path_transform(&path, ts, &outline, &dev)) {
                st = pb_status(&outline);
                goto done;
            }
            st = draw_device(&op, &dev, w, h, out);
        } else {
            st = draw_device(&op, &path, w, h, out);
        }
    } else {
        ts_stroke ts_s;
        ts_path o;
        ts_s.width = s->width;
        ts_s.miter_limit = s->miterlimit;
        ts_s.cap = s->cap;
        ts_s.join = s->join;
        st = ts_stroke_path(&path, &ts_s, res_scale, &outline, &ok);
        if (st == PC_OK && ok && ts_pb_finish(&outline, &o)) {
            st = fill_ts(&o, ts, false, aa, w, h, out);
        }
    }
done:
    ts_pb_fini(&dashed);
    ts_pb_fini(&outline);
    return st;
}

/* ---------------------------------------------------------------- coverage masks */

void sv_cov_fini(sv_cov *c)
{
    if (c != NULL) {
        free(c->a);
        memset(c, 0, sizeof *c);
    }
}

typedef struct mask_spans {
    sv_cov *c;
} mask_spans;

/* The mask blitter of RasterPipelineBlitter::new_mask: lerp towards 255 by the coverage. */
static void mask_span(void *ctx, uint32_t x, uint32_t y, uint32_t w, uint8_t a)
{
    mask_spans *m = ctx;
    sv_cov *c = m->c;
    uint32_t k;
    if (y >= c->h || x >= c->w) {
        return;
    }
    if (w > c->w - x) {
        w = c->w - x;
    }
    for (k = 0; k < w; k++) {
        uint8_t *d = c->a + (size_t)y * c->w + x + k;
        uint32_t v = (uint32_t)*d * (255u - a) + 255u * a;
        *d = a == 255 ? 255 : (uint8_t)((v + 255u) >> 8);
    }
}

pc_status sv_fill_cov(const sv_path *p, sv_ts ts, bool evenodd, bool aa, uint32_t w, uint32_t h, sv_cov *out)
{
    mask_spans m;
    sv_spans sp;
    pc_status st;
    memset(out, 0, sizeof *out);
    if (w == 0 || h == 0) {
        return PC_OK;
    }
    out->a = pc_calloc_array(w, h);
    if (out->a == NULL) {
        return PC_ERR_NOMEM;
    }
    out->w = w;
    out->h = h;
    m.c = out;
    sp.span = mask_span;
    sp.ctx = &m;
    st = sv_fill_path(p, ts, evenodd, aa, w, h, &sp);
    if (st != PC_OK) {
        sv_cov_fini(out);
    }
    return st;
}
