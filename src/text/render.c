/* Text rendering (port of crates/text/src/render.rs; see render.h). */
#include "text/render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "raster/pixel.h"

void pc_rendered_fini(pc_rendered *r)
{
    if (r != NULL) {
        pc_surface_fini(&r->surface);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Warp sink: bends text-space lines with the warp, then maps them through `post`. */

typedef struct warp_sink {
    const pc_line_sink *inner;
    const pc_style_warp *warp;
    pc_xform post;
} warp_sink;

static void warp_map(const warp_sink *ws, double x0, double y0, double x1, double y1, double t, double *ox,
                     double *oy)
{
    pc_point p = pc_style_warp_apply(ws->warp, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t);
    pc_xform_apply(&ws->post, p.x, p.y, ox, oy);
}

static void warp_line(void *ctx, double x0, double y0, double x1, double y1)
{
    const warp_sink *ws = ctx;
    double dx = x1 - x0, dy = y1 - y0;
    double len = sqrt(dx * dx + dy * dy);
    size_t n = pc_f64_to_size(ceil(len / pc_style_warp_max_segment(ws->warp))), i;
    double ax, ay;
    if (n < 1) {
        n = 1;
    } else if (n > 256) {
        n = 256;
    }
    warp_map(ws, x0, y0, x1, y1, 0.0, &ax, &ay);
    for (i = 1; i <= n; i++) {
        double bx, by;
        warp_map(ws, x0, y0, x1, y1, (double)i / (double)n, &bx, &by);
        ws->inner->line(ws->inner->ctx, ax, ay, bx, by);
        ax = bx;
        ay = by;
    }
}

/* ------------------------------------------------------------------------------------------ */

pc_xform pc_text_glyph_xform(const pc_text_layout *l, const pc_placed_glyph *g, float face_skew_deg)
{
    const pc_char_style *st;
    double hs, vs, skew, shift, x = (double)g->x, y = (double)g->y;
    float skew_deg;
    if (g->style >= l->nstyles) {
        return (pc_xform){{1.0, 0.0, 0.0, -1.0, x, y}};
    }
    st = &l->styles[g->style];
    hs = (double)(st->horizontal_scale > 0.0f ? st->horizontal_scale : 1.0f);
    vs = (double)(st->vertical_scale > 0.0f ? st->vertical_scale : 1.0f);
    skew_deg = (st->faux_italic ? PC_TEXT_FAUX_ITALIC_DEG : 0.0f) + face_skew_deg;
    /* f64::to_radians multiplies by the constant PI / 180. */
    skew = tan((double)skew_deg * (3.14159265358979323846 / 180.0)) * vs;
    shift = (double)(st->baseline_shift_pt * l->px_per_pt);
    switch (g->orient) {
    case PC_GLYPH_UPRIGHT:
        return (pc_xform){{hs, 0.0, skew, -vs, x + shift, y}};
    case PC_GLYPH_ROTATED: {
        pc_xform rot = {{0.0, 1.0, -1.0, 0.0, x, y}};
        pc_xform loc = {{hs, 0.0, skew, -vs, 0.0, -shift}};
        return pc_xform_mul(&rot, &loc);
    }
    default:
        return (pc_xform){{hs, 0.0, skew, -vs, x, y - shift}};
    }
}

static bool color_eq(const pc_color *a, const pc_color *b)
{
    return a->mode == b->mode && a->c[0] == b->c[0] && a->c[1] == b->c[1] && a->c[2] == b->c[2] &&
           a->c[3] == b->c[3] && a->alpha == b->alpha;
}

/* f64 in the reference (0.7 is not 0.7f). */
static const double BOLD_OFFSETS[8][2] = {{-1.0, 0.0}, {1.0, 0.0},   {0.0, -1.0}, {0.0, 1.0},
                                          {0.7, 0.7},  {-0.7, -0.7}, {0.7, -0.7}, {-0.7, 0.7}};

/* Draws every glyph and decoration whose style colour is `only` (all when NULL) into `sink`
 * through `transform`, bending outlines with `warp` first (in text space). Font errors skip
 * the glyph as in the reference; only running out of memory fails. */
static pc_status draw(const pc_text_layout *l, const pc_xform *transform, const pc_line_sink *sink,
                      const pc_color *only, const pc_style_warp *warp, pc_error *err)
{
    size_t i;
    warp_sink ws;
    pc_line_sink wsink;
    if (warp != NULL) {
        ws.inner = sink;
        ws.warp = warp;
        ws.post = *transform;
        wsink = (pc_line_sink){warp_line, &ws};
    }
    for (i = 0; i < l->nglyphs; i++) {
        const pc_placed_glyph *g = &l->glyphs[i];
        const pc_char_style *st;
        const pc_glyph_face *face;
        pc_xform glyph;
        size_t k, noff;
        double r;
        bool bold;
        if (g->style >= l->nstyles || g->face >= l->nfaces) {
            continue;
        }
        st = &l->styles[g->style];
        if (only != NULL && !color_eq(only, &st->color)) {
            continue;
        }
        face = &l->faces[g->face];
        if (face->font == NULL || g->id >= pc_font_num_glyphs(face->font)) {
            continue; /* no outline for this glyph */
        }
        glyph = pc_text_glyph_xform(l, g, face->skew_deg);
        bold = st->faux_bold || face->embolden;
        r = (double)(face->size_px * PC_TEXT_FAUX_BOLD_RADIUS);
        noff = bold ? 8 : 1;
        for (k = 0; k < noff; k++) {
            double ox = bold ? BOLD_OFFSETS[k][0] : 0.0, oy = bold ? BOLD_OFFSETS[k][1] : 0.0;
            pc_xform off = {{1.0, 0.0, 0.0, 1.0, ox * r, oy * r}};
            pc_xform local = pc_xform_mul(&glyph, &off); /* offsets in glyph space (#3705) */
            pc_flatten_pen pen;
            pc_outline_pen op;
            pc_error e;
            pc_status s;
            if (warp != NULL) {
                pc_flatten_pen_init(&pen, &wsink, local);
            } else {
                pc_flatten_pen_init(&pen, sink, pc_xform_mul(transform, &local));
            }
            op = pc_flatten_pen_outline(&pen);
            s = pc_font_draw(face->font, g->id, &face->size_px, face->coords, face->ncoords, &op, &e);
            if (s == PC_ERR_NOMEM) {
                return pc_fail(err, PC_ERR_NOMEM, "%s", e.msg);
            }
            if (s == PC_ERR_NOT_FOUND) {
                break;
            }
            if (s == PC_OK) {
                pc_flatten_pen_close(&pen);
            }
        }
    }
    for (i = 0; i < l->ndecorations; i++) {
        const pc_decoration_rect *d = &l->decorations[i];
        if (d->style >= l->nstyles) {
            continue;
        }
        if (only != NULL && !color_eq(only, &l->styles[d->style].color)) {
            continue;
        }
        if (warp != NULL) {
            pc_xform id = PC_XFORM_IDENTITY;
            pc_rect_to(&wsink, &id, (double)d->x0, (double)d->y0, (double)d->x1, (double)d->y1);
        } else {
            pc_rect_to(sink, transform, (double)d->x0, (double)d->y0, (double)d->x1, (double)d->y1);
        }
    }
    return PC_OK;
}

static double clampd(double v, double lo, double hi)
{
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return v;
}

pc_rect pc_text_rect_from_bounds(const double b[4])
{
    double mn = (double)INT32_MIN, mx = (double)INT32_MAX, x0, y0, x1, y1;
    if (!(isfinite(b[0]) && isfinite(b[1]) && isfinite(b[2]) && isfinite(b[3]))) {
        return PC_RECT_EMPTY;
    }
    x0 = clampd(floor(b[0]) - 1.0, mn, mx);
    y0 = clampd(floor(b[1]) - 1.0, mn, mx);
    x1 = clampd(ceil(b[2]) + 1.0, mn, mx);
    y1 = clampd(ceil(b[3]) + 1.0, mn, mx);
    if (x1 <= x0 || y1 <= y0 || x1 - x0 > (double)PC_TEXT_MAX_PIXELS ||
        y1 - y0 > (double)PC_TEXT_MAX_PIXELS) {
        return PC_RECT_EMPTY;
    }
    return pc_rect_make(pc_f64_to_i32(x0), pc_f64_to_i32(y0), pc_f64_to_i32(x1), pc_f64_to_i32(y1));
}

pc_status pc_text_ink_rect(const pc_text_layout *l, const pc_affine *transform, const pc_style_warp *warp,
                           pc_rect *out, pc_error *err)
{
    pc_bounds b = {false, {0.0, 0.0, 0.0, 0.0}};
    pc_line_sink sink = pc_bounds_sink(&b);
    pc_xform t;
    pc_status st;
    if (l == NULL || transform == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "text: invalid arguments");
    }
    memcpy(t.m, transform->m, sizeof t.m);
    st = draw(l, &t, &sink, NULL, warp, err);
    if (st != PC_OK) {
        return st;
    }
    *out = b.has ? pc_text_rect_from_bounds(b.r) : PC_RECT_EMPTY;
    return PC_OK;
}

/* Colour components in format's model (without alpha); returns the count. */
static size_t color_in(pc_pixel_format f, const pc_color *c, float out[PC_RASTER_MAX_CHANNELS])
{
    size_t n = pc_color_mode_channels(f.mode), i;
    if (n > 4) {
        n = 4;
    }
    if (c->mode == f.mode) {
        for (i = 0; i < n; i++) {
            out[i] = c->c[i];
        }
    } else {
        float rgb[3], rgba[4];
        pc_pixel_format nf = f;
        pc_color_to_rgb(c, rgb);
        rgba[0] = rgb[0];
        rgba[1] = rgb[1];
        rgba[2] = rgb[2];
        rgba[3] = 1.0f;
        nf.alpha = false;
        (void)pc_from_rgba(nf, rgba, out);
    }
    return n;
}

pc_status pc_text_rasterize(const pc_text_layout *l, const pc_affine *transform, pc_pixel_format format,
                            pc_antialias aa, const pc_style_warp *warp, pc_rendered *out, pc_error *err)
{
    pc_rect rect;
    size_t w, h, n, stride, npx, i, k, ncolors = 0, len;
    float *acc = NULL, *cov = NULL;
    const pc_color **colors = NULL;
    pc_xform xf, tm, shift;
    pc_status st;
    pc_surface surface;
    if (l == NULL || transform == NULL || out == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "text: invalid arguments");
    }
    format.alpha = true;
    st = pc_text_ink_rect(l, transform, warp, &rect, err);
    if (st != PC_OK) {
        return st;
    }
    w = pc_rect_width(rect);
    h = pc_rect_height(rect);
    pc_surface_init(&surface, format);
    if (w == 0 || h == 0 || (uint64_t)w * (uint64_t)h > PC_TEXT_MAX_PIXELS) {
        out->surface = surface;
        out->rect = PC_RECT_EMPTY;
        return PC_OK;
    }
    n = pc_color_mode_channels(format.mode);
    stride = n + 1;
    npx = w * h;
    if (!pc_size_mul(npx, stride, &len) || len > PC_MAX_ALLOC / sizeof(float)) {
        pc_surface_fini(&surface);
        return pc_fail(err, PC_ERR_LIMIT, "text raster of %zux%zu pixels is too large", w, h);
    }
    acc = pc_calloc_array(len, sizeof(float));
    cov = pc_malloc_array(npx, sizeof(float));
    colors = pc_malloc_array(l->nstyles > 0 ? l->nstyles : 1, sizeof *colors);
    if (acc == NULL || cov == NULL || colors == NULL) {
        st = pc_fail(err, PC_ERR_NOMEM, "out of memory for a %zux%zu text raster", w, h);
        goto cleanup;
    }
    shift = (pc_xform){{1.0, 0.0, 0.0, 1.0, -(double)rect.x0, -(double)rect.y0}};
    memcpy(tm.m, transform->m, sizeof tm.m);
    xf = pc_xform_mul(&shift, &tm);
    for (i = 0; i < l->nstyles; i++) {
        bool seen = false;
        for (k = 0; k < ncolors && !seen; k++) {
            seen = color_eq(colors[k], &l->styles[i].color);
        }
        if (!seen) {
            colors[ncolors++] = &l->styles[i].color;
        }
    }
    for (k = 0; k < ncolors; k++) {
        const pc_color *c = colors[k];
        pc_coverage cv;
        pc_line_sink sink;
        float comps[PC_RASTER_MAX_CHANNELS] = {0}, a;
        st = pc_coverage_init(&cv, w, h, err);
        if (st != PC_OK) {
            goto cleanup;
        }
        sink = pc_coverage_sink(&cv);
        st = draw(l, &xf, &sink, c, warp, err);
        if (st != PC_OK) {
            pc_coverage_fini(&cv);
            goto cleanup;
        }
        pc_coverage_finish(&cv, cov);
        pc_coverage_fini(&cv);
        (void)color_in(format, c, comps);
        a = c->alpha;
        if (a < 0.0f) { /* f32::clamp(0, 1): NaN stays NaN */
            a = 0.0f;
        }
        if (a > 1.0f) {
            a = 1.0f;
        }
        for (i = 0; i < npx; i++) {
            float cvv = cov[i], s, keep;
            float *px = acc + i * stride;
            size_t j;
            if (aa == PC_AA_NONE) {
                cvv = cvv >= 0.5f ? 1.0f : 0.0f;
            }
            s = cvv * a;
            if (s <= 0.0f) {
                continue;
            }
            keep = 1.0f - s;
            for (j = 0; j < n; j++) {
                px[j] = comps[j] * s + px[j] * keep;
            }
            px[n] = s + px[n] * keep;
        }
    }
    /* Unpremultiply. */
    for (i = 0; i < npx; i++) {
        float *px = acc + i * stride;
        float a = px[n];
        if (a > 0.0f) {
            size_t j;
            for (j = 0; j < n; j++) {
                float v = px[j] / a;
                if (v < 0.0f) {
                    v = 0.0f;
                }
                if (v > 1.0f) {
                    v = 1.0f;
                }
                px[j] = v;
            }
        }
    }
    st = pc_surface_write_region(&surface, rect, acc, len, err);
    if (st != PC_OK) {
        goto cleanup;
    }
    pc_surface_prune(&surface);
    out->surface = surface;
    out->rect = rect;
    free(acc);
    free(cov);
    free(colors);
    return PC_OK;
cleanup:
    pc_surface_fini(&surface);
    free(acc);
    free(cov);
    free(colors);
    return st;
}

/* ------------------------------------------------------------------------------------------ */
/* Outlines */

typedef struct recorder {
    pc_xform glyph, post;
    const pc_style_warp *warp;
    pc_path_el *els;
    size_t n, cap;
    bool nomem;
} recorder;

static void rec_map(const recorder *r, float x, float y, double out[2])
{
    double tx, ty;
    pc_xform_apply(&r->glyph, (double)x, (double)y, &tx, &ty);
    if (r->warp != NULL) {
        pc_point p = pc_style_warp_apply(r->warp, tx, ty);
        tx = p.x;
        ty = p.y;
    }
    pc_xform_apply(&r->post, tx, ty, &out[0], &out[1]);
}

static pc_path_el *rec_push(recorder *r, pc_path_kind k)
{
    pc_path_el *e;
    if (r->nomem) {
        return NULL;
    }
    if (r->n == r->cap) {
        pc_path_el *g = pc_grow_array(r->els, &r->cap, r->n + 1, sizeof *g);
        if (g == NULL) {
            r->nomem = true;
            return NULL;
        }
        r->els = g;
    }
    e = &r->els[r->n++];
    memset(e, 0, sizeof *e);
    e->kind = k;
    return e;
}

static void rec_move(void *c, float x, float y)
{
    pc_path_el *e = rec_push(c, PC_PATH_MOVE);
    if (e != NULL) {
        rec_map(c, x, y, e->p[0]);
    }
}

static void rec_line(void *c, float x, float y)
{
    pc_path_el *e = rec_push(c, PC_PATH_LINE);
    if (e != NULL) {
        rec_map(c, x, y, e->p[0]);
    }
}

static void rec_quad(void *c, float cx, float cy, float x, float y)
{
    pc_path_el *e = rec_push(c, PC_PATH_QUAD);
    if (e != NULL) {
        rec_map(c, cx, cy, e->p[0]);
        rec_map(c, x, y, e->p[1]);
    }
}

static void rec_curve(void *c, float ax, float ay, float bx, float by, float x, float y)
{
    pc_path_el *e = rec_push(c, PC_PATH_CURVE);
    if (e != NULL) {
        rec_map(c, ax, ay, e->p[0]);
        rec_map(c, bx, by, e->p[1]);
        rec_map(c, x, y, e->p[2]);
    }
}

static void rec_close(void *c)
{
    (void)rec_push(c, PC_PATH_CLOSE);
}

void pc_glyph_outlines_free(pc_glyph_outline *o, size_t n)
{
    size_t i;
    if (o == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        free(o[i].els);
    }
    free(o);
}

pc_status pc_text_outlines(const pc_text_layout *l, const pc_affine *transform, const pc_style_warp *warp,
                           pc_glyph_outline **out, size_t *n, pc_error *err)
{
    pc_glyph_outline *res = NULL;
    size_t count = 0, cap = 0, i;
    pc_xform post;
    if (l == NULL || transform == NULL || out == NULL || n == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "text: invalid arguments");
    }
    memcpy(post.m, transform->m, sizeof post.m);
    for (i = 0; i < l->nglyphs; i++) {
        const pc_placed_glyph *g = &l->glyphs[i];
        const pc_glyph_face *face;
        recorder r;
        pc_outline_pen pen;
        pc_status s;
        if (g->face >= l->nfaces) {
            continue;
        }
        face = &l->faces[g->face];
        if (face->font == NULL) {
            continue;
        }
        memset(&r, 0, sizeof r);
        r.glyph = pc_text_glyph_xform(l, g, face->skew_deg);
        r.post = post;
        r.warp = warp;
        pen = (pc_outline_pen){rec_move, rec_line, rec_quad, rec_curve, rec_close, &r};
        s = pc_font_draw(face->font, g->id, &face->size_px, face->coords, face->ncoords, &pen, NULL);
        if (r.nomem || s == PC_ERR_NOMEM) {
            free(r.els);
            pc_glyph_outlines_free(res, count);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory for glyph outlines");
        }
        if (s != PC_OK || r.n == 0) {
            free(r.els);
            continue;
        }
        if (count == cap) {
            pc_glyph_outline *gr = pc_grow_array(res, &cap, count + 1, sizeof *gr);
            if (gr == NULL) {
                free(r.els);
                pc_glyph_outlines_free(res, count);
                return pc_fail(err, PC_ERR_NOMEM, "out of memory for glyph outlines");
            }
            res = gr;
        }
        res[count].els = r.els;
        res[count].n = r.n;
        count++;
    }
    *out = res;
    *n = count;
    return PC_OK;
}
