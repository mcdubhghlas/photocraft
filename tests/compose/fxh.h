/* Layer-effect helpers of the C3 test executables (header-only; each test is one TU): the
 * effects tests.rs builds its documents from. Effects are plain structs; attaching them to a
 * layer moves them (the layer then owns their strings and arrays). */
#ifndef PC_TESTS_COMPOSE_FXH_H
#define PC_TESTS_COMPOSE_FXH_H

#include "ch.h"
#include "compose/effects.h"
#include "compose/effects_util.h"
#include "compose/fx_internal.h"

/* Appends n effects to l (moved). */
static inline void fxh_add(pc_layer *l, const pc_effect *e, size_t n)
{
    pc_effect *v = realloc(l->effects.items, (l->effects.nitems + n) * sizeof *v);
    if (v == NULL) {
        abort(); /* test setup only */
    }
    memcpy(v + l->effects.nitems, e, n * sizeof *v);
    l->effects.items = v;
    l->effects.nitems += n;
}

/* doc_white(40, 40) with an opaque red square 10..30 carrying the effects (moved). */
static inline pc_document fxh_doc(const pc_effect *e, size_t n)
{
    pc_document d = ch_doc_white(40, 40);
    pc_layer *l = ch_push(&d, ch_solid("sq", pc_rect_make(10, 10, 30, 30), 1, 0, 0, 1));
    fxh_add(l, e, n);
    return d;
}

static inline pc_effect fxh_stroke(float size, pc_stroke_position pos)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_STROKE;
    e.u.stroke.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    e.u.stroke.size = size;
    e.u.stroke.position = pos;
    e.u.stroke.paint.kind = PC_FX_PAINT_COLOR;
    e.u.stroke.paint.u.color = pc_color_rgb(0.0f, 0.0f, 1.0f);
    return e;
}

/* tests.rs shadow(): Normal, black, spread 1 (hard), size 0, knocks out. */
static inline pc_shadow fxh_shadow(float distance, float angle)
{
    pc_shadow s;
    memset(&s, 0, sizeof s);
    s.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    s.color = PC_COLOR_BLACK;
    s.angle = angle;
    s.distance = distance;
    s.spread = 1.0f;
    s.knocks_out = true;
    return s;
}

static inline pc_effect fxh_wrap_shadow(pc_effect_kind kind, pc_shadow s)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.u.shadow = s;
    return e;
}

/* tests.rs glow(): Normal, green, size 4, range 0.5. */
static inline pc_glow fxh_glow(pc_glow_technique t, pc_glow_source src)
{
    pc_glow g;
    memset(&g, 0, sizeof g);
    g.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    g.paint.kind = PC_FX_PAINT_COLOR;
    g.paint.u.color = pc_color_rgb(0.0f, 1.0f, 0.0f);
    g.technique = t;
    g.size = 4.0f;
    g.range = 0.5f;
    g.source = src;
    return g;
}

static inline pc_effect fxh_wrap_glow(pc_effect_kind kind, pc_glow g)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = kind;
    e.u.glow = g;
    return e;
}

static inline pc_effect fxh_color_overlay(float r, float g, float b)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_COLOR_OVERLAY;
    e.u.color_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    e.u.color_overlay.color = pc_color_rgb(r, g, b);
    return e;
}

/* effects.rs bevel_of(): depth 1, up, size 5, light from 90 / 30, Screen / Multiply at 75 %. */
static inline pc_bevel fxh_bevel(pc_bevel_style style, pc_bevel_technique t)
{
    pc_bevel b;
    memset(&b, 0, sizeof b);
    b.enabled = true;
    b.style = style;
    b.technique = t;
    b.depth = 1.0f;
    b.up = true;
    b.size = 5.0f;
    b.angle = 90.0f;
    b.altitude = 30.0f;
    b.highlight = pc_fx_common_new(PC_BLEND_SCREEN, 0.75f);
    b.highlight_color = PC_COLOR_WHITE;
    b.shadow = pc_fx_common_new(PC_BLEND_MULTIPLY, 0.75f);
    b.shadow_color = PC_COLOR_BLACK;
    return b;
}

static inline pc_effect fxh_wrap_bevel(pc_bevel b)
{
    pc_effect e;
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_BEVEL_EMBOSS;
    e.u.bevel = b;
    return e;
}

/* A two-stop gradient (copied stops). */
static inline pc_gradient fxh_gradient(pc_color a, pc_color b)
{
    pc_gradient g;
    if (pc_gradient_default(&g, NULL) != PC_OK) {
        abort();
    }
    g.stops[0].color = a;
    g.stops[1].color = b;
    return g;
}

static inline float *fxh_px(const pc_document *d, int32_t x, int32_t y, float out[4])
{
    ch_px(d, x, y, out);
    return out;
}

#endif
