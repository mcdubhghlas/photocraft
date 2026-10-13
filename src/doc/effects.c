/* Layer styles (port of crates/doc/src/effects.rs). The pc_effects container is in core.c. */
#include "doc/effects.h"

#include <stdlib.h>
#include <string.h>

#include "base/mem.h"

static bool color_eq(const pc_color *a, const pc_color *b)
{
    return a->mode == b->mode && a->c[0] == b->c[0] && a->c[1] == b->c[1] && a->c[2] == b->c[2] &&
           a->c[3] == b->c[3] && a->alpha == b->alpha;
}

/* Copy n elements of size bytes; *out is NULL when n is 0. */
static pc_status dup_array(const void *src, size_t n, size_t size, void **out, pc_error *err)
{
    void *p;
    if (n == 0) {
        *out = NULL;
        return PC_OK;
    }
    p = pc_malloc_array(n, size);
    if (p == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memcpy(p, src, n * size);
    *out = p;
    return PC_OK;
}

/* ---- gradient ---- */

pc_status pc_gradient_default(pc_gradient *out, pc_error *err)
{
    pc_color_stop *s = pc_malloc_array(2, sizeof *s);
    if (s == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s[0].pos = 0.0f;
    s[0].color = PC_COLOR_BLACK;
    s[1].pos = 1.0f;
    s[1].color = PC_COLOR_WHITE;
    memset(out, 0, sizeof *out);
    out->stops = s;
    out->nstops = 2;
    out->style = PC_GRADIENT_LINEAR;
    out->angle = 90.0f;
    out->scale = 1.0f;
    out->reverse = false;
    out->align = true;
    out->offset[0] = 0.0f;
    out->offset[1] = 0.0f;
    return PC_OK;
}

void pc_gradient_fini(pc_gradient *g)
{
    if (g == NULL) {
        return;
    }
    free(g->stops);
    free(g->opacity_stops);
    memset(g, 0, sizeof *g);
}

pc_status pc_gradient_clone(const pc_gradient *g, pc_gradient *out, pc_error *err)
{
    pc_gradient t = *g;
    void *stops;
    void *ops;
    pc_status st = dup_array(g->stops, g->nstops, sizeof *g->stops, &stops, err);
    if (st != PC_OK) {
        return st;
    }
    st = dup_array(g->opacity_stops, g->nopacity_stops, sizeof *g->opacity_stops, &ops, err);
    if (st != PC_OK) {
        free(stops);
        return st;
    }
    t.stops = stops;
    t.opacity_stops = ops;
    *out = t;
    return PC_OK;
}

bool pc_gradient_eq(const pc_gradient *a, const pc_gradient *b)
{
    size_t i;
    if (a->nstops != b->nstops || a->nopacity_stops != b->nopacity_stops) {
        return false;
    }
    for (i = 0; i < a->nstops; i++) {
        if (a->stops[i].pos != b->stops[i].pos || !color_eq(&a->stops[i].color, &b->stops[i].color)) {
            return false;
        }
    }
    for (i = 0; i < a->nopacity_stops; i++) {
        if (a->opacity_stops[i].pos != b->opacity_stops[i].pos ||
            a->opacity_stops[i].opacity != b->opacity_stops[i].opacity) {
            return false;
        }
    }
    return a->style == b->style && a->angle == b->angle && a->scale == b->scale && a->reverse == b->reverse &&
           a->align == b->align && a->offset[0] == b->offset[0] && a->offset[1] == b->offset[1];
}

/* ---- contour ---- */

void pc_contour_fini(pc_contour *c)
{
    if (c == NULL) {
        return;
    }
    pc_str_fini(&c->name);
    pc_curve_fini(&c->points);
    free(c->corners);
    c->corners = NULL;
    c->ncorners = 0;
    c->custom = false;
}

pc_status pc_contour_clone(const pc_contour *c, pc_contour *out, pc_error *err)
{
    pc_contour t;
    pc_status st;
    memset(&t, 0, sizeof t);
    t.custom = c->custom;
    st = pc_str_clone(&c->name, &t.name, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_curve_clone(&c->points, &t.points, err);
    if (st != PC_OK) {
        pc_str_fini(&t.name);
        return st;
    }
    if (c->ncorners > 0) {
        t.corners = pc_malloc_array(c->ncorners, sizeof *t.corners);
        if (t.corners == NULL) {
            pc_contour_fini(&t);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        memcpy(t.corners, c->corners, c->ncorners * sizeof *t.corners);
        t.ncorners = c->ncorners;
    }
    *out = t;
    return PC_OK;
}

bool pc_contour_eq(const pc_contour *a, const pc_contour *b)
{
    if (a->custom != b->custom) {
        return false;
    }
    /* Linear carries no data (the enum variant has no fields). */
    if (!a->custom) {
        return true;
    }
    if (!pc_str_eq(&a->name, &b->name) || !pc_curve_eq(&a->points, &b->points) ||
        a->ncorners != b->ncorners) {
        return false;
    }
    for (size_t i = 0; i < a->ncorners; i++) {
        if (a->corners[i] != b->corners[i]) {
            return false;
        }
    }
    return true;
}

bool pc_contour_is_corner(const pc_contour *c, size_t i)
{
    return c->custom && i < c->ncorners && c->corners[i];
}

/* ---- fx paint ---- */

void pc_fx_paint_fini(pc_fx_paint *p)
{
    if (p == NULL) {
        return;
    }
    switch (p->kind) {
    case PC_FX_PAINT_GRADIENT:
        pc_gradient_fini(&p->u.gradient);
        break;
    case PC_FX_PAINT_PATTERN:
        pc_str_fini(&p->u.pattern.name);
        pc_str_fini(&p->u.pattern.id);
        break;
    default:
        break;
    }
    memset(p, 0, sizeof *p);
}

pc_status pc_fx_paint_clone(const pc_fx_paint *p, pc_fx_paint *out, pc_error *err)
{
    pc_fx_paint t = *p;
    pc_status st = PC_OK;
    switch (p->kind) {
    case PC_FX_PAINT_GRADIENT:
        st = pc_gradient_clone(&p->u.gradient, &t.u.gradient, err);
        break;
    case PC_FX_PAINT_PATTERN:
        st = pc_str_clone(&p->u.pattern.name, &t.u.pattern.name, err);
        if (st == PC_OK) {
            st = pc_str_clone(&p->u.pattern.id, &t.u.pattern.id, err);
            if (st != PC_OK) {
                pc_str_fini(&t.u.pattern.name);
            }
        }
        break;
    default:
        break;
    }
    if (st == PC_OK) {
        *out = t;
    }
    return st;
}

bool pc_fx_paint_eq(const pc_fx_paint *a, const pc_fx_paint *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_FX_PAINT_COLOR:
        return color_eq(&a->u.color, &b->u.color);
    case PC_FX_PAINT_GRADIENT:
        return pc_gradient_eq(&a->u.gradient, &b->u.gradient);
    case PC_FX_PAINT_PATTERN:
        return pc_str_eq(&a->u.pattern.name, &b->u.pattern.name) && pc_str_eq(&a->u.pattern.id, &b->u.pattern.id) &&
               a->u.pattern.scale == b->u.pattern.scale;
    }
    return false;
}

pc_fx_common pc_fx_common_new(pc_blend_mode blend, float opacity)
{
    pc_fx_common c;
    c.enabled = true;
    c.blend = blend;
    c.opacity = opacity;
    return c;
}

static bool common_eq(const pc_fx_common *a, const pc_fx_common *b)
{
    return a->enabled == b->enabled && a->blend == b->blend && a->opacity == b->opacity;
}

pc_bevel_contour pc_bevel_contour_default(void)
{
    pc_bevel_contour c;
    memset(&c, 0, sizeof c);
    c.contour.custom = false;
    c.range = 0.5f;
    c.anti_alias = false;
    return c;
}

pc_global_light pc_global_light_default(void)
{
    pc_global_light g;
    g.angle = 120.0f;
    g.altitude = 30.0f;
    return g;
}

/* ---- effect ---- */

/* Clones two strings into *n and *i (both or neither). */
static pc_status clone_two(const pc_str *a, const pc_str *b, pc_str *na, pc_str *nb, pc_error *err)
{
    pc_str ta = PC_STR_INIT;
    pc_str tb = PC_STR_INIT;
    pc_status st = pc_str_clone(a, &ta, err);
    if (st != PC_OK) {
        return st;
    }
    st = pc_str_clone(b, &tb, err);
    if (st != PC_OK) {
        pc_str_fini(&ta);
        return st;
    }
    *na = ta;
    *nb = tb;
    return PC_OK;
}

void pc_effect_fini(pc_effect *e)
{
    if (e == NULL) {
        return;
    }
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW:
        pc_contour_fini(&e->u.shadow.contour);
        break;
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW:
        pc_fx_paint_fini(&e->u.glow.paint);
        pc_contour_fini(&e->u.glow.contour);
        break;
    case PC_FX_STROKE:
        pc_fx_paint_fini(&e->u.stroke.paint);
        break;
    case PC_FX_COLOR_OVERLAY:
        break;
    case PC_FX_GRADIENT_OVERLAY:
        pc_gradient_fini(&e->u.gradient_overlay.gradient);
        break;
    case PC_FX_PATTERN_OVERLAY:
        pc_str_fini(&e->u.pattern_overlay.name);
        pc_str_fini(&e->u.pattern_overlay.id);
        break;
    case PC_FX_SATIN:
        pc_contour_fini(&e->u.satin.contour);
        break;
    case PC_FX_BEVEL_EMBOSS:
        pc_contour_fini(&e->u.bevel.gloss_contour);
        pc_contour_fini(&e->u.bevel.contour.contour);
        pc_str_fini(&e->u.bevel.texture.name);
        pc_str_fini(&e->u.bevel.texture.id);
        break;
    }
    memset(e, 0, sizeof *e);
}

static pc_status bevel_clone(const pc_bevel *b, pc_bevel *t, pc_error *err)
{
    pc_status st;
    *t = *b;
    memset(&t->gloss_contour, 0, sizeof t->gloss_contour);
    memset(&t->contour.contour, 0, sizeof t->contour.contour);
    memset(&t->texture.name, 0, sizeof t->texture.name);
    memset(&t->texture.id, 0, sizeof t->texture.id);
    st = pc_contour_clone(&b->gloss_contour, &t->gloss_contour, err);
    if (st == PC_OK && b->has_contour) {
        st = pc_contour_clone(&b->contour.contour, &t->contour.contour, err);
    }
    if (st == PC_OK && b->has_texture) {
        st = clone_two(&b->texture.name, &b->texture.id, &t->texture.name, &t->texture.id, err);
    }
    if (st != PC_OK) {
        pc_contour_fini(&t->gloss_contour);
        pc_contour_fini(&t->contour.contour);
        return st;
    }
    return PC_OK;
}

pc_status pc_effect_clone(const pc_effect *e, pc_effect *out, pc_error *err)
{
    pc_effect t = *e;
    pc_status st = PC_OK;
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW:
        st = pc_contour_clone(&e->u.shadow.contour, &t.u.shadow.contour, err);
        break;
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW:
        st = pc_fx_paint_clone(&e->u.glow.paint, &t.u.glow.paint, err);
        if (st == PC_OK) {
            st = pc_contour_clone(&e->u.glow.contour, &t.u.glow.contour, err);
            if (st != PC_OK) {
                pc_fx_paint_fini(&t.u.glow.paint);
            }
        }
        break;
    case PC_FX_STROKE:
        st = pc_fx_paint_clone(&e->u.stroke.paint, &t.u.stroke.paint, err);
        break;
    case PC_FX_COLOR_OVERLAY:
        break;
    case PC_FX_GRADIENT_OVERLAY:
        st = pc_gradient_clone(&e->u.gradient_overlay.gradient, &t.u.gradient_overlay.gradient, err);
        break;
    case PC_FX_PATTERN_OVERLAY:
        st = clone_two(&e->u.pattern_overlay.name, &e->u.pattern_overlay.id, &t.u.pattern_overlay.name,
                       &t.u.pattern_overlay.id, err);
        break;
    case PC_FX_SATIN:
        st = pc_contour_clone(&e->u.satin.contour, &t.u.satin.contour, err);
        break;
    case PC_FX_BEVEL_EMBOSS:
        st = bevel_clone(&e->u.bevel, &t.u.bevel, err);
        break;
    }
    if (st == PC_OK) {
        *out = t;
    }
    return st;
}

static bool shadow_eq(const pc_shadow *a, const pc_shadow *b)
{
    return common_eq(&a->common, &b->common) && color_eq(&a->color, &b->color) && a->angle == b->angle &&
           a->use_global_light == b->use_global_light && a->distance == b->distance && a->spread == b->spread &&
           a->size == b->size && pc_contour_eq(&a->contour, &b->contour) && a->anti_alias == b->anti_alias &&
           a->noise == b->noise && a->knocks_out == b->knocks_out;
}

static bool glow_eq(const pc_glow *a, const pc_glow *b)
{
    return common_eq(&a->common, &b->common) && pc_fx_paint_eq(&a->paint, &b->paint) &&
           a->technique == b->technique && a->spread == b->spread && a->size == b->size &&
           pc_contour_eq(&a->contour, &b->contour) && a->anti_alias == b->anti_alias && a->range == b->range &&
           a->jitter == b->jitter && a->noise == b->noise && a->source == b->source;
}

static bool bevel_eq(const pc_bevel *a, const pc_bevel *b)
{
    if (!(a->enabled == b->enabled && a->style == b->style && a->technique == b->technique &&
          a->depth == b->depth && a->up == b->up && a->size == b->size && a->soften == b->soften &&
          a->angle == b->angle && a->altitude == b->altitude && a->use_global_light == b->use_global_light &&
          pc_contour_eq(&a->gloss_contour, &b->gloss_contour) && common_eq(&a->highlight, &b->highlight) &&
          color_eq(&a->highlight_color, &b->highlight_color) && common_eq(&a->shadow, &b->shadow) &&
          color_eq(&a->shadow_color, &b->shadow_color))) {
        return false;
    }
    if (a->has_contour != b->has_contour || a->has_texture != b->has_texture) {
        return false;
    }
    if (a->has_contour && !(pc_contour_eq(&a->contour.contour, &b->contour.contour) &&
                            a->contour.range == b->contour.range && a->contour.anti_alias == b->contour.anti_alias)) {
        return false;
    }
    if (a->has_texture) {
        const pc_bevel_texture *x = &a->texture;
        const pc_bevel_texture *y = &b->texture;
        return pc_str_eq(&x->name, &y->name) && pc_str_eq(&x->id, &y->id) && x->scale == y->scale &&
               x->depth == y->depth && x->invert == y->invert && x->link == y->link && x->phase[0] == y->phase[0] &&
               x->phase[1] == y->phase[1];
    }
    return true;
}

bool pc_effect_eq(const pc_effect *a, const pc_effect *b)
{
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW:
        return shadow_eq(&a->u.shadow, &b->u.shadow);
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW:
        return glow_eq(&a->u.glow, &b->u.glow);
    case PC_FX_STROKE:
        return common_eq(&a->u.stroke.common, &b->u.stroke.common) && a->u.stroke.size == b->u.stroke.size &&
               a->u.stroke.position == b->u.stroke.position && pc_fx_paint_eq(&a->u.stroke.paint, &b->u.stroke.paint);
    case PC_FX_COLOR_OVERLAY:
        return common_eq(&a->u.color_overlay.common, &b->u.color_overlay.common) &&
               color_eq(&a->u.color_overlay.color, &b->u.color_overlay.color);
    case PC_FX_GRADIENT_OVERLAY:
        return common_eq(&a->u.gradient_overlay.common, &b->u.gradient_overlay.common) &&
               pc_gradient_eq(&a->u.gradient_overlay.gradient, &b->u.gradient_overlay.gradient) &&
               a->u.gradient_overlay.dither == b->u.gradient_overlay.dither;
    case PC_FX_PATTERN_OVERLAY:
        return common_eq(&a->u.pattern_overlay.common, &b->u.pattern_overlay.common) &&
               pc_str_eq(&a->u.pattern_overlay.name, &b->u.pattern_overlay.name) &&
               pc_str_eq(&a->u.pattern_overlay.id, &b->u.pattern_overlay.id) &&
               a->u.pattern_overlay.scale == b->u.pattern_overlay.scale &&
               a->u.pattern_overlay.angle == b->u.pattern_overlay.angle &&
               a->u.pattern_overlay.link == b->u.pattern_overlay.link &&
               a->u.pattern_overlay.phase[0] == b->u.pattern_overlay.phase[0] &&
               a->u.pattern_overlay.phase[1] == b->u.pattern_overlay.phase[1];
    case PC_FX_SATIN: {
        const pc_satin *x = &a->u.satin;
        const pc_satin *y = &b->u.satin;
        return common_eq(&x->common, &y->common) && color_eq(&x->color, &y->color) && x->angle == y->angle &&
               x->distance == y->distance && x->size == y->size && pc_contour_eq(&x->contour, &y->contour) &&
               x->anti_alias == y->anti_alias && x->invert == y->invert;
    }
    case PC_FX_BEVEL_EMBOSS:
        return bevel_eq(&a->u.bevel, &b->u.bevel);
    }
    return false;
}

bool pc_effect_enabled(const pc_effect *e)
{
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW:
        return e->u.shadow.common.enabled;
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW:
        return e->u.glow.common.enabled;
    case PC_FX_STROKE:
        return e->u.stroke.common.enabled;
    case PC_FX_COLOR_OVERLAY:
        return e->u.color_overlay.common.enabled;
    case PC_FX_GRADIENT_OVERLAY:
        return e->u.gradient_overlay.common.enabled;
    case PC_FX_PATTERN_OVERLAY:
        return e->u.pattern_overlay.common.enabled;
    case PC_FX_SATIN:
        return e->u.satin.common.enabled;
    case PC_FX_BEVEL_EMBOSS:
        return e->u.bevel.enabled;
    }
    return false;
}

void pc_effect_set_enabled(pc_effect *e, bool on)
{
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW:
        e->u.shadow.common.enabled = on;
        break;
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW:
        e->u.glow.common.enabled = on;
        break;
    case PC_FX_STROKE:
        e->u.stroke.common.enabled = on;
        break;
    case PC_FX_COLOR_OVERLAY:
        e->u.color_overlay.common.enabled = on;
        break;
    case PC_FX_GRADIENT_OVERLAY:
        e->u.gradient_overlay.common.enabled = on;
        break;
    case PC_FX_PATTERN_OVERLAY:
        e->u.pattern_overlay.common.enabled = on;
        break;
    case PC_FX_SATIN:
        e->u.satin.common.enabled = on;
        break;
    case PC_FX_BEVEL_EMBOSS:
        e->u.bevel.enabled = on;
        break;
    }
}

static void stops_in_mode(pc_color_stop *stops, size_t n, pc_color_mode mode)
{
    for (size_t i = 0; i < n; i++) {
        stops[i].color = pc_color_in_mode(&stops[i].color, mode);
    }
}

static void paint_in_mode(pc_fx_paint *p, pc_color_mode mode)
{
    if (p->kind == PC_FX_PAINT_COLOR) {
        p->u.color = pc_color_in_mode(&p->u.color, mode);
    } else if (p->kind == PC_FX_PAINT_GRADIENT) {
        stops_in_mode(p->u.gradient.stops, p->u.gradient.nstops, mode);
    }
}

void pc_effect_in_mode(pc_effect *e, pc_color_mode mode)
{
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
    case PC_FX_INNER_SHADOW:
        e->u.shadow.color = pc_color_in_mode(&e->u.shadow.color, mode);
        break;
    case PC_FX_OUTER_GLOW:
    case PC_FX_INNER_GLOW:
        paint_in_mode(&e->u.glow.paint, mode);
        break;
    case PC_FX_STROKE:
        paint_in_mode(&e->u.stroke.paint, mode);
        break;
    case PC_FX_COLOR_OVERLAY:
        e->u.color_overlay.color = pc_color_in_mode(&e->u.color_overlay.color, mode);
        break;
    case PC_FX_GRADIENT_OVERLAY:
        stops_in_mode(e->u.gradient_overlay.gradient.stops, e->u.gradient_overlay.gradient.nstops, mode);
        break;
    case PC_FX_SATIN:
        e->u.satin.color = pc_color_in_mode(&e->u.satin.color, mode);
        break;
    case PC_FX_BEVEL_EMBOSS:
        e->u.bevel.highlight_color = pc_color_in_mode(&e->u.bevel.highlight_color, mode);
        e->u.bevel.shadow_color = pc_color_in_mode(&e->u.bevel.shadow_color, mode);
        break;
    case PC_FX_PATTERN_OVERLAY:
        break;
    }
}

const char *pc_effect_label(const pc_effect *e)
{
    switch (e->kind) {
    case PC_FX_DROP_SHADOW:
        return "Drop Shadow";
    case PC_FX_INNER_SHADOW:
        return "Inner Shadow";
    case PC_FX_OUTER_GLOW:
        return "Outer Glow";
    case PC_FX_INNER_GLOW:
        return "Inner Glow";
    case PC_FX_STROKE:
        return "Stroke";
    case PC_FX_COLOR_OVERLAY:
        return "Color Overlay";
    case PC_FX_GRADIENT_OVERLAY:
        return "Gradient Overlay";
    case PC_FX_PATTERN_OVERLAY:
        return "Pattern Overlay";
    case PC_FX_SATIN:
        return "Satin";
    case PC_FX_BEVEL_EMBOSS:
        return "Bevel & Emboss";
    }
    return "Effect";
}

void pc_effect_default_drop_shadow(pc_effect *out)
{
    pc_shadow *s;
    memset(out, 0, sizeof *out);
    out->kind = PC_FX_DROP_SHADOW;
    s = &out->u.shadow;
    s->common = pc_fx_common_new(PC_BLEND_MULTIPLY, 0.75f);
    s->color = PC_COLOR_BLACK;
    s->angle = 120.0f;
    s->use_global_light = true;
    s->distance = 5.0f;
    s->spread = 0.0f;
    s->size = 5.0f;
    s->contour.custom = false;
    s->anti_alias = false;
    s->noise = 0.0f;
    s->knocks_out = true;
}
