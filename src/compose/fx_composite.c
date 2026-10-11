/* A layer with its effects onto the backdrop (port of effects.rs composite_with_effects_prepared).
 *
 * Render order, bottom to top (Photoshop's layer-style stack read upwards): drop shadows, outer
 * glows, outside strokes, outer bevel, then the layer itself (content at fill opacity) with
 * pattern / gradient / colour overlays, satin, inner glows, inner shadows, a stroked shape's
 * vector stroke, inside strokes and the inner bevel on top. Exterior effects blend straight into
 * the backdrop with their own modes; the layer and its interior effects blend with the layer's
 * mode. Emboss and pillow emboss then shade the composited result. Layer opacity applies to the
 * whole stack, fill opacity only to the layer's own pixels. Multiple instances of one kind: the
 * first listed is on top, so they paint in reverse. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/num.h"
#include "base/rmath.h"
#include "compose/fx_internal.h"

static pc_status nomem(pc_error *err)
{
    return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
}

float pc_compose_fx_outline_share(float cov, float l)
{
    float outside = pc_rs_maxf_k(1.0f - pc_clampf(cov, 0.0f, 1.0f), 0.0f);
    float rest = 1.0f - pc_clampf(l, 0.0f, 1.0f);
    return rest <= 1e-6f ? 1.0f : pc_rs_minf_k(outside / rest, 1.0f);
}

/* Premultiplied linear interpolation between two straight-alpha pixels. */
static void mix_premul(const float a[4], const float b[4], float k, float out[4])
{
    float alpha = a[3] + (b[3] - a[3]) * k;
    int c;
    if (alpha <= 0.0f) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    for (c = 0; c < 3; c++) {
        out[c] = (a[c] * a[3] + (b[c] * b[3] - a[c] * a[3]) * k) / alpha;
    }
    out[3] = alpha;
}

static bool inside(float a)
{
    return a > PC_FX_INSIDE_EPS;
}

/* Everything one composite call shares. */
typedef struct fxc {
    const pc_layer *layer;
    const pc_compose_buffer *content;
    const pc_compose_fx_maps *maps;
    const pc_compose_patterns *patterns;
    const pc_compose_vector_stroke *vs;
    pc_rect big;
    size_t w;
    size_t h;
    size_t n;
    pc_rect sb;
    double anchor[2];
    pc_fxm shape;
    const pc_effect **items; /* enabled effects */
    size_t nitems;
} fxc;

static float kmask(const fxc *c, size_t i)
{
    const pc_compose_vector_stroke *vs = c->vs;
    if (vs != NULL && vs->mask != NULL && vs->stroke != NULL && i < vs->stroke->len) {
        return vs->mask[i];
    }
    return 1.0f;
}

/* The content's alpha joined with a split-off vector stroke (under the layer's mask). */
static float join(const fxc *c, size_t i, float a)
{
    const pc_compose_vector_stroke *vs = c->vs;
    if (vs != NULL && vs->stroke != NULL && i < vs->stroke->len) {
        return kmask(c, i) * (a + vs->stroke->px[i][3] * (1.0f - a));
    }
    return a;
}

/* Effect k of enabled item i, cropped to the composite's rectangle. */
static pc_status fx_map(const fxc *c, size_t i, size_t k, pc_fxm *out, pc_error *err)
{
    const pc_fx_item *it = i < c->maps->nitems ? &c->maps->per[i] : NULL;
    if (it == NULL || k >= it->n) {
        /* Maps built for another layer state: nothing to paint (the reference would panic). */
        return pc_fxm_new(c->w, c->h, 0.0f, out, err);
    }
    return pc_fx_crop(c->maps, &it->m[k], c->big, 0.0f, out, err);
}

/* rel(m): coverage relative to the shape (m / alpha within it). In place. */
static void rel(const fxc *c, pc_fxm *m)
{
    size_t i;
    for (i = 0; i < c->n; i++) {
        float a = c->shape.v[i];
        m->v[i] = inside(a) ? pc_rs_minf_k(m->v[i] / a, 1.0f) : 0.0f;
    }
}

static pc_status paint_fx_map(const fxc *c, pc_compose_buffer *dst, size_t i, size_t k, bool relative,
                              const pc_color *color, pc_blend_mode blend, float opacity, pc_error *err)
{
    pc_fxm m;
    float rgb[3];
    pc_status st = fx_map(c, i, k, &m, err);
    if (st != PC_OK) {
        return st;
    }
    if (relative) {
        rel(c, &m);
    }
    pc_color_to_rgb(color, rgb);
    pc_fx_paint_color(dst, &m, rgb, blend, opacity);
    pc_fxm_free(&m);
    return PC_OK;
}

/* The exterior effects below the layer: drop shadows, then outer glows. */
static pc_status exterior_below(const fxc *c, pc_compose_buffer *work, pc_error *err)
{
    size_t r;
    pc_status st = PC_OK;
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        pc_fxm m;
        float rgb[3];
        size_t i;
        if (e->kind != PC_FX_DROP_SHADOW) {
            continue;
        }
        st = fx_map(c, r, 0, &m, err);
        if (st != PC_OK) {
            break;
        }
        if (e->u.shadow.knocks_out) {
            /* The layer hides the shadow beneath it only where its fill is see-through: at 100 %
             * fill the layer covers it anyway, at 0 % the shape shows the bare backdrop. */
            float see = 1.0f - pc_clampf(c->layer->fill_opacity, 0.0f, 1.0f);
            for (i = 0; i < c->n; i++) {
                m.v[i] *= 1.0f - c->shape.v[i] * see;
            }
        }
        pc_color_to_rgb(&e->u.shadow.color, rgb);
        pc_fx_paint_color(work, &m, rgb, e->u.shadow.common.blend, e->u.shadow.common.opacity);
        pc_fxm_free(&m);
    }
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        pc_fxm m;
        if (e->kind != PC_FX_OUTER_GLOW) {
            continue;
        }
        st = fx_map(c, r, 0, &m, err);
        if (st == PC_OK) {
            st = pc_fx_paint_glow(work, &m, &e->u.glow, c->sb, c->anchor, c->big, c->patterns, err);
            pc_fxm_free(&m);
        }
    }
    return st;
}

/* Overlays, satin and inner glows on the layer (relative to its shape). */
static pc_status interior(const fxc *c, pc_compose_buffer *lay, pc_error *err)
{
    pc_fxm full;
    size_t r;
    size_t i;
    pc_status st = pc_fxm_new(c->w, c->h, 0.0f, &full, err);
    if (st != PC_OK) {
        return st;
    }
    for (i = 0; i < c->n; i++) {
        full.v[i] = inside(c->shape.v[i]) ? 1.0f : 0.0f;
    }
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        if (e->kind == PC_FX_PATTERN_OVERLAY) {
            pc_compose_placement place = pc_compose_placement_anchored(
                c->anchor, e->u.pattern_overlay.link, e->u.pattern_overlay.phase, e->u.pattern_overlay.scale,
                e->u.pattern_overlay.angle);
            st = pc_fx_paint_pattern(
                lay, &full, c->patterns, &e->u.pattern_overlay.name, &e->u.pattern_overlay.id, &place, c->big,
                e->u.pattern_overlay.common.blend, e->u.pattern_overlay.common.opacity, err);
        }
    }
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        if (e->kind == PC_FX_GRADIENT_OVERLAY) {
            st = pc_fx_paint_gradient(lay, &full, &e->u.gradient_overlay.gradient, c->sb, c->big,
                                      e->u.gradient_overlay.common.blend,
                                      e->u.gradient_overlay.common.opacity, err);
        }
    }
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        if (e->kind == PC_FX_COLOR_OVERLAY) {
            float rgb[3];
            pc_color_to_rgb(&e->u.color_overlay.color, rgb);
            pc_fx_paint_color(lay, &full, rgb, e->u.color_overlay.common.blend,
                              e->u.color_overlay.common.opacity);
        }
    }
    pc_fxm_free(&full);
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        if (e->kind == PC_FX_SATIN) {
            st = paint_fx_map(c, lay, r, 0, true, &e->u.satin.color, e->u.satin.common.blend,
                              e->u.satin.common.opacity, err);
        }
    }
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        pc_fxm m;
        if (e->kind != PC_FX_INNER_GLOW) {
            continue;
        }
        st = fx_map(c, r, 0, &m, err);
        if (st == PC_OK) {
            rel(c, &m);
            st = pc_fx_paint_glow(lay, &m, &e->u.glow, c->sb, c->anchor, c->big, c->patterns, err);
            pc_fxm_free(&m);
        }
    }
    return st;
}

static pc_rect stroke_frame_of(const fxc *c, const pc_stroke_fx *st)
{
    pc_rect r;
    return pc_compose_fx_maps_stroke_frame(c->maps, st, &r) ? r : c->sb;
}

/* Inside strokes over the layer, bottom instance first. din may be NULL (no distances: nothing
 * paints, as the reference's zip with an empty field). */
static pc_status inside_strokes(const fxc *c, pc_compose_buffer *lay, const float *din, pc_error *err)
{
    size_t r;
    pc_status st = PC_OK;
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        float in_w;
        float out_w;
        pc_fxm m;
        size_t i;
        if (e->kind != PC_FX_STROKE) {
            continue;
        }
        pc_compose_fx_stroke_widths(&e->u.stroke, &in_w, &out_w);
        if (in_w <= 0.0f) {
            continue;
        }
        st = pc_fxm_new(c->w, c->h, 0.0f, &m, err);
        if (st != PC_OK) {
            break;
        }
        for (i = 0; din != NULL && i < c->n; i++) {
            m.v[i] = inside(c->shape.v[i]) ? pc_clampf(in_w + 0.5f - din[i], 0.0f, 1.0f) : 0.0f;
        }
        st = pc_fx_paint_fx(lay, &m, &e->u.stroke.paint, stroke_frame_of(c, &e->u.stroke), c->anchor, c->big,
                            e->u.stroke.common.blend, e->u.stroke.common.opacity, c->patterns, err);
        pc_fxm_free(&m);
    }
    return st;
}

/* Outside parts of strokes lie beneath the layer and blend onto the backdrop with their own mode.
 * A higher stroke covers the ones below it: each stroke's share is its coverage x opacity not yet
 * taken by strokes above (for Normal strokes this equals stacking them top over bottom). */
static pc_status outside_strokes(const fxc *c, pc_compose_buffer *work, const pc_compose_buffer *lay,
                                 const float *dout, const float *vdout, pc_error *err)
{
    bool vector_shape = c->layer->content.kind == PC_LAYER_SHAPE;
    const float *d = vdout != NULL ? vdout : dout;
    pc_compose_buffer base = {PC_RECT_EMPTY, NULL, 0};
    pc_compose_buffer blended = {PC_RECT_EMPTY, NULL, 0};
    float (*acc)[4] = NULL;
    float *cover = NULL;
    float *share = NULL;
    pc_fxm ones = {0, 0, NULL};
    size_t r;
    size_t i;
    pc_status st;
    bool any = false;
    for (r = 0; r < c->nitems; r++) {
        float in_w;
        float out_w;
        if (c->items[r]->kind == PC_FX_STROKE) {
            pc_compose_fx_stroke_widths(&c->items[r]->u.stroke, &in_w, &out_w);
            any = any || out_w > 0.0f;
        }
    }
    if (!any) {
        return PC_OK;
    }
    st = pc_compose_buffer_clone(work, &base, err);
    if (st != PC_OK) {
        return st;
    }
    acc = pc_calloc_array(c->n > 0 ? c->n : 1, sizeof *acc);
    cover = pc_calloc_array(c->n > 0 ? c->n : 1, sizeof *cover);
    share = pc_calloc_array(c->n > 0 ? c->n : 1, sizeof *share);
    if (acc == NULL || cover == NULL || share == NULL) {
        st = nomem(err);
    }
    if (st == PC_OK) {
        st = pc_fxm_new(c->w, c->h, 0.0f, &ones, err);
    }
    for (r = 0; r < c->nitems && st == PC_OK; r++) {
        const pc_effect *e = c->items[r];
        float in_w;
        float out_w;
        if (e->kind != PC_FX_STROKE) {
            continue;
        }
        pc_compose_fx_stroke_widths(&e->u.stroke, &in_w, &out_w);
        if (out_w <= 0.0f) {
            continue;
        }
        for (i = 0; i < c->n; i++) {
            /* Shape layers: Photoshop strokes the vector outline; the stroke never shows through
             * the shape's pixels. Along a filled shape's outline it covers the part of each edge
             * pixel outside the path. A missing field (maps of another state) strokes nothing. */
            float a = c->shape.v[i];
            float band = pc_clampf(out_w + 0.5f - (d != NULL ? d[i] : PC_FX_FAR), 0.0f, 1.0f);
            float k;
            if (c->maps->outline) {
                k = band * pc_compose_fx_outline_share(a, lay->px[i][3]);
            } else if (inside(a)) {
                if (vector_shape) {
                    k = 0.0f;
                } else {
                    /* Beneath the layer the stroke shows only through the part of the pixel the
                     * shape doesn't cover (1 - a); the layer on top covers cv = a x fill, so the
                     * share beneath is (1 - a) / (1 - cv). */
                    float cv = pc_clampf(lay->px[i][3], 0.0f, 1.0f);
                    k = cv >= 1.0f ? 1.0f
                                   : pc_clampf((1.0f - pc_clampf(a, 0.0f, 1.0f)) / (1.0f - cv), 0.0f, 1.0f);
                }
            } else {
                k = band;
            }
            share[i] = k * e->u.stroke.common.opacity * (1.0f - cover[i]);
            cover[i] += share[i];
            ones.v[i] = share[i] > 0.0f ? 1.0f : 0.0f;
        }
        /* The stroke blended over the backdrop at full coverage. */
        pc_compose_buffer_fini(&blended);
        st = pc_compose_buffer_clone(&base, &blended, err);
        if (st == PC_OK) {
            st = pc_fx_paint_fx(&blended, &ones, &e->u.stroke.paint, stroke_frame_of(c, &e->u.stroke),
                                c->anchor, c->big, e->u.stroke.common.blend, 1.0f, c->patterns, err);
        }
        for (i = 0; st == PC_OK && i < c->n; i++) {
            float s = share[i];
            const float *b = blended.px[i];
            int ch;
            if (s > 0.0f) {
                for (ch = 0; ch < 3; ch++) {
                    acc[i][ch] += s * b[ch] * b[3];
                }
                acc[i][3] += s * b[3];
            }
        }
    }
    for (i = 0; st == PC_OK && i < c->n; i++) {
        float *p = work->px[i];
        float k;
        float alpha;
        if (cover[i] <= 0.0f) {
            continue;
        }
        k = 1.0f - pc_rs_minf_k(cover[i], 1.0f);
        alpha = p[3] * k + acc[i][3];
        if (alpha > 0.0f) {
            float o0 = (p[0] * p[3] * k + acc[i][0]) / alpha;
            float o1 = (p[1] * p[3] * k + acc[i][1]) / alpha;
            float o2 = (p[2] * p[3] * k + acc[i][2]) / alpha;
            p[0] = o0;
            p[1] = o1;
            p[2] = o2;
            p[3] = pc_rs_minf_k(alpha, 1.0f);
        }
    }
    pc_compose_buffer_fini(&base);
    pc_compose_buffer_fini(&blended);
    pc_fxm_free(&ones);
    free(acc);
    free(cover);
    free(share);
    return st;
}

static pc_status paint_bevels(const fxc *c, pc_compose_buffer *dst, pc_compose_bevel_paint which,
                              bool relative, pc_error *err)
{
    size_t r;
    pc_status st = PC_OK;
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        const pc_bevel *b;
        if (e->kind != PC_FX_BEVEL_EMBOSS || r >= c->maps->nitems || c->maps->per[r].n != 2 ||
            c->maps->per[r].bevel_paint != which) {
            continue;
        }
        b = &e->u.bevel;
        st = paint_fx_map(c, dst, r, 0, relative, &b->highlight_color, b->highlight.blend,
                          b->highlight.opacity, err);
        if (st == PC_OK) {
            st = paint_fx_map(c, dst, r, 1, relative, &b->shadow_color, b->shadow.blend, b->shadow.opacity,
                              err);
        }
    }
    return st;
}

/* The layer plus its interior effects (straight RGBA over big), then its outside strokes and
 * outer bevel on work, the layer composited onto work in its mode, and emboss shading on top. */
static pc_status layer_and_strokes(fxc *c, pc_compose_buffer *work, pc_error *err)
{
    const pc_layer *layer = c->layer;
    float fill = layer->fill_opacity;
    /* Blend Interior Effects as Group: interior effects combine with the content first, and fill
     * opacity applies to the combination. */
    bool interior_group = layer->advanced.blend_interior && fill < 1.0f;
    float content_fill = interior_group ? 1.0f : fill;
    bool shapeless = !layer->advanced.transparency_shapes;
    bool relative = shapeless || c->maps->outline || c->vs != NULL;
    bool vector_shape = layer->content.kind == PC_LAYER_SHAPE;
    bool has_strokes = false;
    pc_compose_buffer lay = {PC_RECT_EMPTY, NULL, 0};
    float *din = NULL;
    float *dout = NULL;
    float *vdout = NULL;
    pc_blend_mode mode;
    float gamma;
    size_t i;
    size_t r;
    pc_status st = pc_compose_buffer_transparent(c->big, &lay, err);
    if (st != PC_OK) {
        return st;
    }
    /* Within an outline (or a split-off vector stroke) the content's own transparency acts like
     * fill opacity: the effects still cover the whole shape. */
    for (i = 0; i < c->n; i++) {
        const float *p = c->content->px[i];
        float a = c->shape.v[i];
        float la;
        if (!inside(a)) {
            la = 0.0f;
        } else if (relative) {
            la = content_fill * pc_rs_minf_k(kmask(c, i) * p[3] / a, 1.0f);
        } else {
            la = content_fill;
        }
        lay.px[i][0] = p[0];
        lay.px[i][1] = p[1];
        lay.px[i][2] = p[2];
        lay.px[i][3] = la;
    }
    st = interior(c, &lay, err);
    if (st == PC_OK && interior_group) {
        for (i = 0; i < c->n; i++) {
            lay.px[i][3] *= pc_rs_maxf_k(fill, 0.0f);
        }
    }
    for (r = c->nitems; r-- > 0 && st == PC_OK;) {
        const pc_effect *e = c->items[r];
        if (e->kind == PC_FX_INNER_SHADOW) {
            st = paint_fx_map(c, &lay, r, 0, true, &e->u.shadow.color, e->u.shadow.common.blend,
                              e->u.shadow.common.opacity, err);
        }
    }
    /* A stroked shape's vector stroke: above the fill's clipped layers and interior effects,
     * below the stroke effect (psd-tools stroke-composite). */
    if (st == PC_OK && c->vs != NULL && c->vs->stroke != NULL) {
        size_t m = c->vs->stroke->len < c->n ? c->vs->stroke->len : c->n;
        for (i = 0; i < m; i++) {
            const float *s = c->vs->stroke->px[i];
            float a = c->shape.v[i];
            if (s[3] > 0.0f && inside(a)) {
                float src[4];
                src[0] = s[0];
                src[1] = s[1];
                src[2] = s[2];
                src[3] = fill * pc_rs_minf_k(kmask(c, i) * s[3] / a, 1.0f);
                pc_psblend_composite(PC_BLEND_NORMAL, lay.px[i], src, 1.0f, lay.px[i]);
            }
        }
    }
    for (r = 0; r < c->nitems; r++) {
        has_strokes = has_strokes || c->items[r]->kind == PC_FX_STROKE;
    }
    if (st == PC_OK && has_strokes && c->maps->din != NULL && c->maps->dout != NULL) {
        st = pc_fx_crop_vec(c->maps, c->maps->din, c->big, 0.0f, &din, err);
        if (st == PC_OK) {
            st = pc_fx_crop_vec(c->maps, c->maps->dout, c->big, PC_FX_FAR, &dout, err);
        }
    }
    if (st == PC_OK) {
        st = inside_strokes(c, &lay, din, err);
    }
    if (st == PC_OK) {
        st = paint_bevels(c, &lay, PC_COMPOSE_BEVEL_PAINT_INNER, true, err);
    }
    if (st == PC_OK) {
        /* The shape's own alpha. */
        for (i = 0; i < c->n; i++) {
            lay.px[i][3] *= pc_rs_minf_k(c->shape.v[i], 1.0f);
        }
        if (c->maps->vdout != NULL && vector_shape && has_strokes) {
            st = pc_fx_crop_vec(c->maps, c->maps->vdout, c->big, PC_FX_FAR, &vdout, err);
        }
    }
    if (st == PC_OK) {
        st = outside_strokes(c, work, &lay, dout, vdout, err);
    }
    if (st == PC_OK) {
        st = paint_bevels(c, work, PC_COMPOSE_BEVEL_PAINT_OUTER, false, err);
    }
    if (st == PC_OK) {
        /* Layer (with interior effects) onto the exterior result, in the layer's mode. */
        mode = layer->blend == PC_BLEND_PASS_THROUGH ? PC_BLEND_NORMAL : layer->blend;
        gamma = pc_compose_text_gamma(layer);
        for (i = 0; i < c->n; i++) {
            if (lay.px[i][3] > 0.0f) {
                pc_psblend_composite_gamma(mode, work->px[i], lay.px[i], 1.0f, gamma, work->px[i]);
            }
        }
        /* Emboss styles shade the composited layer (painting them into the layer before its
         * text-gamma composite left a type layer's lit edges dark). */
        st = paint_bevels(c, work, PC_COMPOSE_BEVEL_PAINT_BOTH, false, err);
    }
    free(din);
    free(dout);
    free(vdout);
    pc_compose_buffer_fini(&lay);
    return st;
}

pc_status pc_compose_fx_composite(const pc_layer *layer, const pc_compose_buffer *content,
                                  pc_compose_buffer *backdrop, const pc_compose_fx_maps *maps,
                                  pc_rect layer_bounds, const pc_compose_patterns *patterns,
                                  const pc_compose_vector_stroke *vstroke, pc_error *err)
{
    fxc c;
    pc_rect rect = backdrop->rect;
    pc_rect ov;
    pc_compose_buffer before = {PC_RECT_EMPTY, NULL, 0};
    pc_compose_buffer work = {PC_RECT_EMPTY, NULL, 0};
    size_t i;
    size_t k;
    int32_t x;
    int32_t y;
    float op;
    pc_status st;
    if (maps == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "compose: layer effects without maps");
    }
    memset(&c, 0, sizeof c);
    c.layer = layer;
    c.content = content;
    c.maps = maps;
    c.patterns = patterns;
    c.vs = vstroke;
    c.big = content->rect;
    c.w = pc_cmp_w(c.big);
    c.h = pc_rect_height(c.big);
    c.n = c.w * c.h;
    if (content->len != c.n || backdrop->len != (size_t)pc_cmp_w(rect) * pc_rect_height(rect)) {
        return pc_fail(err, PC_ERR_INVALID, "compose: effect buffers don't match their rectangles");
    }
    c.sb = layer_bounds;
    /* Linked patterns tile from the effects reference point (else the layer's top-left). */
    if (layer->effects.has_reference) {
        c.anchor[0] = layer->effects.reference[0];
        c.anchor[1] = layer->effects.reference[1];
    } else {
        c.anchor[0] = (double)layer_bounds.x0;
        c.anchor[1] = (double)layer_bounds.y0;
    }
    c.items = pc_malloc_array(layer->effects.nitems > 0 ? layer->effects.nitems : 1, sizeof *c.items);
    if (c.items == NULL) {
        return nomem(err);
    }
    for (i = 0; i < layer->effects.nitems; i++) {
        if (pc_effect_enabled(&layer->effects.items[i])) {
            c.items[c.nitems++] = &layer->effects.items[i];
        }
    }
    /* The effect shape: the content's alpha (with a split-off vector stroke the content is the
     * unmasked fill and the mask applies to fill and stroke), joined with a filled shape's
     * outline. Transparency Shapes Layer off: the shape is the whole layer. */
    if (!layer->advanced.transparency_shapes) {
        st = pc_fx_crop(maps, &maps->shape, c.big, 0.0f, &c.shape, err);
    } else if (maps->outline) {
        st = pc_fx_crop(maps, &maps->shape, c.big, 0.0f, &c.shape, err);
        for (i = 0; st == PC_OK && i < c.n; i++) {
            c.shape.v[i] = pc_rs_maxf(c.shape.v[i], join(&c, i, content->px[i][3]));
        }
    } else {
        st = pc_fxm_new(c.w, c.h, 0.0f, &c.shape, err);
        for (i = 0; st == PC_OK && i < c.n; i++) {
            c.shape.v[i] = join(&c, i, content->px[i][3]);
        }
    }
    if (st == PC_OK) {
        st = pc_compose_buffer_clone(backdrop, &before, err);
    }
    /* Exterior effects are painted on a backdrop copy extended to big. */
    if (st == PC_OK) {
        st = pc_compose_buffer_transparent(c.big, &work, err);
    }
    if (st == PC_OK) {
        ov = pc_rect_intersect(rect, c.big);
        for (y = ov.y0; y < ov.y1; y++) {
            for (x = ov.x0; x < ov.x1; x++) {
                size_t wi = (size_t)((int64_t)y - c.big.y0) * c.w + (size_t)((int64_t)x - c.big.x0);
                const float *p = pc_compose_buffer_get(&before, x, y);
                memcpy(work.px[wi], p, sizeof work.px[wi]);
            }
        }
        st = exterior_below(&c, &work, err);
    }
    if (st == PC_OK) {
        st = layer_and_strokes(&c, &work, err);
    }
    if (st == PC_OK) {
        /* Layer opacity applies to the whole stack. */
        size_t rw = pc_cmp_w(rect);
        op = layer->opacity;
        for (y = rect.y0; y < rect.y1; y++) {
            for (x = rect.x0; x < rect.x1; x++) {
                const float *a;
                const float *b;
                i = (size_t)((int64_t)y - rect.y0) * rw + (size_t)((int64_t)x - rect.x0);
                a = before.px[i];
                b = a;
                if (x >= c.big.x0 && x < c.big.x1 && y >= c.big.y0 && y < c.big.y1) {
                    k = (size_t)((int64_t)y - c.big.y0) * c.w + (size_t)((int64_t)x - c.big.x0);
                    b = work.px[k];
                }
                mix_premul(a, b, op, backdrop->px[i]);
            }
        }
    }
    pc_compose_buffer_fini(&before);
    pc_compose_buffer_fini(&work);
    pc_fxm_free(&c.shape);
    free(c.items);
    return st;
}
