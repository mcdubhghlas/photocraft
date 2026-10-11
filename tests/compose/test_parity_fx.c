/* Bit-exact parity of the layer effects with the Rust reference: tests/golden/compose-fx-vectors.txt
 * was written by `rustref compose-fx-vectors` (tools/rustref/src/compose_fx_vectors.rs). The same
 * seeded shapes, effects (every kind, hostile values), patterns and documents are built here from
 * the same xorshift stream, call for call, and printed in the same form; every line must match.
 * Documents 1001..1080 use C2 features (gradient / pattern fills, stroked shapes, masks that hide
 * effects, knockouts, Blend Clipped Layers as Group off) together with effects. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "compose/effects.h"
#include "compose/effects_util.h"
#include "compose/pattern.h"
#include "vector/vector.h"

#define GOLDEN "tests/golden/compose-fx-vectors.txt"
#define H0 0xcbf29ce484222325ull

/* ---- hashing ---- */

static void fnv(uint64_t *h, const void *p, size_t n)
{
    const uint8_t *b = p;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}

static void hf(uint64_t *h, float v)
{
    uint32_t u;
    uint8_t b[4];
    int i;
    memcpy(&u, &v, 4);
    if (isnan(v)) {
        u = 0x7fc00000u;
    }
    for (i = 0; i < 4; i++) {
        b[i] = (uint8_t)(u >> (8 * i));
    }
    fnv(h, b, 4);
}

static void hi(uint64_t *h, int32_t v)
{
    uint32_t u = (uint32_t)v;
    uint8_t b[4];
    int i;
    for (i = 0; i < 4; i++) {
        b[i] = (uint8_t)(u >> (8 * i));
    }
    fnv(h, b, 4);
}

static void hu64(uint64_t *h, uint64_t v)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    fnv(h, b, 8);
}

static uint64_t hbuf(const pc_compose_buffer *b)
{
    uint64_t h = H0;
    size_t i;
    int c;
    for (i = 0; i < b->len; i++) {
        for (c = 0; c < 4; c++) {
            hf(&h, b->px[i][c]);
        }
    }
    hi(&h, b->rect.x0);
    hi(&h, b->rect.y0);
    hi(&h, b->rect.x1);
    hi(&h, b->rect.y1);
    return h;
}

static void hrect(uint64_t *h, pc_rect r)
{
    hi(h, r.x0);
    hi(h, r.y0);
    hi(h, r.x1);
    hi(h, r.y1);
}

/* ---- output ---- */

typedef struct out {
    char *s;
    size_t len;
    size_t cap;
} out;

static void put(out *o, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (o->len + (size_t)n + 1 > o->cap) {
        size_t cap = (o->len + (size_t)n + 1) * 2;
        char *s = realloc(o->s, cap);
        if (s == NULL) {
            return;
        }
        o->s = s;
        o->cap = cap;
    }
    va_start(ap, fmt);
    (void)vsnprintf(o->s + o->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    o->len += (size_t)n;
}

static void rs(out *o, pc_rect r)
{
    put(o, "%d,%d,%d,%d", r.x0, r.y0, r.x1, r.y1);
}

/* ---- rng ---- */

typedef struct rng {
    uint64_t s;
} rng;

static uint64_t next(rng *r)
{
    uint64_t x = r->s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->s = x;
    return x;
}

static uint64_t below(rng *r, uint64_t n)
{
    return next(r) % n;
}

static float unit(rng *r)
{
    return (float)(next(r) >> 40) / 16777216.0f;
}

static int32_t range(rng *r, int32_t lo, int32_t hi_)
{
    return lo + (int32_t)below(r, (uint64_t)(hi_ - lo));
}

static void *xcalloc(size_t n, size_t s)
{
    void *p = calloc(n > 0 ? n : 1, s);
    if (p == NULL) {
        abort(); /* test setup only */
    }
    return p;
}

static pc_str str_of(const char *s)
{
    pc_str v = {NULL, 0};
    if (pc_str_set_c(&v, s, NULL) != PC_OK) {
        abort();
    }
    return v;
}

static pc_str str_clone(const pc_str *s)
{
    pc_str v = {NULL, 0};
    if (pc_str_clone(s, &v, NULL) != PC_OK) {
        abort();
    }
    return v;
}

static const pc_color_mode MODES[6] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE,    PC_COLOR_MODE_CMYK,
                                       PC_COLOR_MODE_LAB, PC_COLOR_MODE_MULTICHANNEL, PC_COLOR_MODE_DUOTONE};
static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
static const pc_gradient_style STYLES[5] = {PC_GRADIENT_LINEAR, PC_GRADIENT_RADIAL, PC_GRADIENT_ANGLE,
                                            PC_GRADIENT_REFLECTED, PC_GRADIENT_DIAMOND};
static const pc_bevel_style BEVEL_STYLES[5] = {PC_BEVEL_OUTER, PC_BEVEL_INNER, PC_BEVEL_EMBOSS,
                                               PC_BEVEL_PILLOW_EMBOSS, PC_BEVEL_STROKE_EMBOSS};
static const pc_bevel_technique TECHNIQUES[3] = {PC_BEVEL_SMOOTH, PC_BEVEL_CHISEL_HARD, PC_BEVEL_CHISEL_SOFT};
static const pc_stroke_position POSITIONS[3] = {PC_STROKE_OUTSIDE, PC_STROKE_INSIDE, PC_STROKE_CENTER};

/* ---- effect generator (mirrors compose_fx_vectors.rs) ---- */

static pc_color color(rng *r)
{
    uint64_t k = below(r, 5);
    pc_color c;
    memset(&c, 0, sizeof c);
    c.alpha = 1.0f;
    if (k <= 1) {
        float a = unit(r);
        float b = unit(r);
        float cc = unit(r);
        return pc_color_rgb(a, b, cc);
    }
    if (k == 2) {
        return pc_color_gray(unit(r));
    }
    if (k == 3) {
        c.mode = PC_COLOR_MODE_CMYK;
        c.c[0] = unit(r);
        c.c[1] = unit(r);
        c.c[2] = unit(r);
        c.c[3] = unit(r);
        return c;
    }
    c.mode = PC_COLOR_MODE_LAB;
    c.c[0] = unit(r);
    c.c[1] = unit(r);
    c.c[2] = unit(r);
    return c;
}

static pc_blend_mode blend(rng *r)
{
    if (below(r, 3) == 0) {
        return PC_BLEND_NORMAL;
    }
    return PC_BLEND_LAYER_MODES[below(r, 27)];
}

static float fx_opacity(rng *r)
{
    switch (below(r, 7)) {
    case 0:
        return 1.0f;
    case 1:
        return 0.75f;
    case 2:
        return 0.5f;
    case 3:
        return unit(r);
    case 4:
        return 0.0f;
    case 5:
        return 1.5f;
    default:
        return -0.2f;
    }
}

static pc_fx_common common(rng *r)
{
    pc_blend_mode b = blend(r);
    float o = fx_opacity(r);
    pc_fx_common c = pc_fx_common_new(b, o);
    if (below(r, 12) == 0) {
        c.enabled = false;
    }
    return c;
}

static float size(rng *r)
{
    const float hostile[4] = {NAN, INFINITY, 1e30f, 600.0f};
    switch (below(r, 12)) {
    case 0:
        return 0.0f;
    case 1:
        return 1.0f;
    case 2:
        return 2.0f;
    case 3:
        return 3.5f;
    case 4:
        return 5.0f;
    case 5:
        return 8.0f;
    case 6:
        return unit(r) * 12.0f;
    case 7:
        return 16.0f;
    case 8:
        return -3.0f;
    case 9:
        return hostile[below(r, 4)];
    default:
        return 4.0f;
    }
}

static float angle(rng *r)
{
    const float hostile[3] = {NAN, INFINITY, 1e20f};
    switch (below(r, 8)) {
    case 0:
        return 0.0f;
    case 1:
        return 90.0f;
    case 2:
        return 120.0f;
    case 3:
        return -45.0f;
    case 4:
        return unit(r) * 360.0f;
    case 5:
        return 200.0f;
    case 6:
        return hostile[below(r, 3)];
    default:
        return 30.0f;
    }
}

static float frac(rng *r)
{
    const float hostile[3] = {NAN, -0.5f, 2.0f};
    switch (below(r, 6)) {
    case 0:
        return 0.0f;
    case 1:
        return 1.0f;
    case 2:
        return 0.5f;
    case 3:
        return unit(r);
    case 4:
        return hostile[below(r, 3)];
    default:
        return 0.25f;
    }
}

static void set_points(pc_contour *c, const float (*p)[2], size_t n)
{
    size_t i;
    c->custom = true;
    c->name = str_of("c");
    c->points.len = n;
    c->points.pts = n > 0 ? xcalloc(n, sizeof *c->points.pts) : NULL;
    for (i = 0; i < n; i++) {
        c->points.pts[i].input = p[i][0];
        c->points.pts[i].output = p[i][1];
    }
}

static pc_contour contour(rng *r)
{
    pc_contour c;
    uint64_t k = below(r, 7);
    memset(&c, 0, sizeof c);
    if (k <= 2) {
        return c;
    }
    if (k == 3) {
        const float p[3][2] = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
        set_points(&c, p, 3);
    } else if (k == 4) {
        uint64_t n = 1 + below(r, 5);
        float p[5][2];
        uint64_t i;
        for (i = 0; i < n; i++) {
            p[i][0] = unit(r);
            p[i][1] = unit(r);
        }
        set_points(&c, (const float (*)[2])p, (size_t)n);
    } else if (k == 5) {
        const float p[5][2] = {{0.0f, 0.0f}, {0.1f, 1.0f}, {0.2f, 0.0f}, {0.2f, 0.5f}, {1.0f, 1.0f}};
        set_points(&c, p, 5);
    } else {
        uint64_t j = below(r, 4);
        if (j == 0) {
            set_points(&c, NULL, 0);
        } else if (j == 1) {
            const float p[1][2] = {{0.3f, 0.8f}};
            set_points(&c, p, 1);
        } else if (j == 2) {
            const float p[3][2] = {{NAN, 0.5f}, {0.0f, 1.0f}, {1.0f, 0.0f}};
            set_points(&c, p, 3);
        } else {
            const float p[4][2] = {{1.0f, 1.0f}, {0.0f, 0.0f}, {0.5f, 2.0f}, {0.25f, -1.0f}};
            set_points(&c, p, 4);
        }
    }
    return c;
}

static pc_gradient gradient(rng *r)
{
    pc_gradient g;
    uint64_t n = below(r, 5);
    uint64_t m;
    uint64_t i;
    const float nan_half[2] = {NAN, 0.5f};
    const float hostile[2] = {0.0f, NAN};
    memset(&g, 0, sizeof g);
    g.stops = xcalloc((size_t)n, sizeof *g.stops);
    g.nstops = (size_t)n;
    for (i = 0; i < n; i++) {
        uint64_t k = below(r, 5);
        float pos;
        if (k == 0) {
            pos = 0.0f;
        } else if (k == 1) {
            pos = 1.0f;
        } else if (k == 2) {
            pos = nan_half[below(r, 2)];
        } else {
            pos = unit(r);
        }
        g.stops[i].pos = pos;
        g.stops[i].color = color(r);
    }
    m = below(r, 3);
    g.opacity_stops = xcalloc((size_t)m, sizeof *g.opacity_stops);
    g.nopacity_stops = (size_t)m;
    for (i = 0; i < m; i++) {
        g.opacity_stops[i].pos = unit(r);
        g.opacity_stops[i].opacity = frac(r);
    }
    g.style = STYLES[below(r, 5)];
    g.angle = angle(r);
    switch (below(r, 5)) {
    case 0:
        g.scale = 1.0f;
        break;
    case 1:
        g.scale = 0.5f;
        break;
    case 2:
        g.scale = 1.5f;
        break;
    case 3:
        g.scale = unit(r) * 2.0f;
        break;
    default:
        g.scale = hostile[below(r, 2)];
        break;
    }
    g.reverse = below(r, 2) == 0;
    g.align = below(r, 2) == 0;
    if (below(r, 3) == 0) {
        g.offset[0] = unit(r) - 0.5f;
        g.offset[1] = unit(r) - 0.5f;
    }
    return g;
}

static void pattern_ref(rng *r, const pc_pattern *pats, size_t n, pc_str *name, pc_str *id)
{
    uint64_t k = below(r, 4);
    const pc_pattern *p;
    if (n == 0 || k == 3) {
        *name = str_of("missing");
        *id = str_of("nope");
        return;
    }
    p = &pats[below(r, n)];
    if (k == 0) {
        *name = str_clone(&p->name);
        *id = str_clone(&p->id);
    } else if (k == 1) {
        *name = str_of("");
        *id = str_clone(&p->id);
    } else {
        *name = str_clone(&p->name);
        *id = str_of("");
    }
}

static float pat_scale(rng *r)
{
    const float hostile[2] = {0.0f, NAN};
    switch (below(r, 4)) {
    case 0:
        return 1.0f;
    case 1:
        return 0.5f;
    case 2:
        return 2.0f;
    default:
        return hostile[below(r, 2)];
    }
}

static pc_fx_paint fx_paint(rng *r, const pc_pattern *pats, size_t n)
{
    pc_fx_paint p;
    uint64_t k = below(r, 4);
    memset(&p, 0, sizeof p);
    if (k <= 1) {
        p.kind = PC_FX_PAINT_COLOR;
        p.u.color = color(r);
    } else if (k == 2) {
        p.kind = PC_FX_PAINT_GRADIENT;
        p.u.gradient = gradient(r);
    } else {
        p.kind = PC_FX_PAINT_PATTERN;
        pattern_ref(r, pats, n, &p.u.pattern.name, &p.u.pattern.id);
        p.u.pattern.scale = pat_scale(r);
    }
    return p;
}

static float fx_noise(rng *r)
{
    const float hostile[2] = {NAN, 2.0f};
    switch (below(r, 5)) {
    case 0:
        return unit(r);
    case 1:
        return hostile[below(r, 2)];
    default:
        return 0.0f;
    }
}

static void phase(rng *r, float ph[2])
{
    ph[0] = 0.0f;
    ph[1] = 0.0f;
    if (below(r, 3) == 0) {
        ph[0] = unit(r) * 8.0f;
        ph[1] = unit(r) * 8.0f;
    }
}

static pc_shadow shadow(rng *r)
{
    pc_shadow s;
    memset(&s, 0, sizeof s);
    s.common = common(r);
    s.color = color(r);
    s.angle = angle(r);
    s.use_global_light = below(r, 2) == 0;
    s.distance = size(r);
    s.spread = frac(r);
    s.size = size(r);
    s.contour = contour(r);
    s.noise = fx_noise(r);
    s.knocks_out = below(r, 3) != 0;
    return s;
}

static pc_glow glow(rng *r, const pc_pattern *pats, size_t n)
{
    pc_glow g;
    const float hostile[3] = {0.0f, NAN, -1.0f};
    memset(&g, 0, sizeof g);
    g.common = common(r);
    g.paint = fx_paint(r, pats, n);
    g.technique = below(r, 2) == 0 ? PC_GLOW_SOFTER : PC_GLOW_PRECISE;
    g.spread = frac(r);
    g.size = size(r);
    g.contour = contour(r);
    switch (below(r, 5)) {
    case 0:
        g.range = 0.5f;
        break;
    case 1:
        g.range = 1.0f;
        break;
    case 2:
        g.range = unit(r);
        break;
    case 3:
        g.range = hostile[below(r, 3)];
        break;
    default:
        g.range = 0.25f;
        break;
    }
    g.noise = fx_noise(r);
    g.source = below(r, 2) == 0 ? PC_GLOW_SOURCE_EDGE : PC_GLOW_SOURCE_CENTER;
    return g;
}

static pc_bevel bevel(rng *r, const pc_pattern *pats, size_t n)
{
    pc_bevel b;
    const float dh[3] = {0.0f, NAN, -1.0f};
    const float sh[2] = {NAN, 1e30f};
    const float ah[2] = {90.0f, NAN};
    memset(&b, 0, sizeof b);
    b.enabled = below(r, 12) != 0;
    b.style = BEVEL_STYLES[below(r, 5)];
    b.technique = TECHNIQUES[below(r, 3)];
    switch (below(r, 5)) {
    case 0:
        b.depth = 1.0f;
        break;
    case 1:
        b.depth = 2.5f;
        break;
    case 2:
        b.depth = unit(r) * 10.0f;
        break;
    case 3:
        b.depth = dh[below(r, 3)];
        break;
    default:
        b.depth = 0.5f;
        break;
    }
    b.up = below(r, 3) != 0;
    b.size = size(r);
    switch (below(r, 4)) {
    case 0:
        b.soften = 0.0f;
        break;
    case 1:
        b.soften = 2.0f;
        break;
    case 2:
        b.soften = unit(r) * 6.0f;
        break;
    default:
        b.soften = sh[below(r, 2)];
        break;
    }
    b.angle = angle(r);
    switch (below(r, 4)) {
    case 0:
        b.altitude = 30.0f;
        break;
    case 1:
        b.altitude = unit(r) * 90.0f;
        break;
    case 2:
        b.altitude = 0.0f;
        break;
    default:
        b.altitude = ah[below(r, 2)];
        break;
    }
    b.use_global_light = below(r, 2) == 0;
    b.gloss_contour = contour(r);
    b.highlight = common(r);
    b.highlight_color = color(r);
    b.shadow = common(r);
    b.shadow_color = color(r);
    if (below(r, 3) == 0) {
        b.has_contour = true;
        b.contour.contour = contour(r);
        b.contour.range = frac(r);
    }
    if (below(r, 3) == 0) {
        uint64_t k;
        b.has_texture = true;
        pattern_ref(r, pats, n, &b.texture.name, &b.texture.id);
        k = below(r, 3);
        b.texture.scale = k == 0 ? 1.0f : (k == 1 ? 0.5f : 2.0f);
        k = below(r, 3);
        b.texture.depth = k == 0 ? 1.0f : (k == 1 ? -0.5f : unit(r) * 2.0f);
        b.texture.invert = below(r, 2) == 0;
        b.texture.link = below(r, 2) == 0;
        phase(r, b.texture.phase);
    }
    return b;
}

static pc_effect effect(rng *r, const pc_pattern *pats, size_t n)
{
    pc_effect e;
    uint64_t k = below(r, 10);
    memset(&e, 0, sizeof e);
    switch (k) {
    case 0:
    case 1:
        e.kind = k == 0 ? PC_FX_DROP_SHADOW : PC_FX_INNER_SHADOW;
        e.u.shadow = shadow(r);
        break;
    case 2:
    case 3:
        e.kind = k == 2 ? PC_FX_OUTER_GLOW : PC_FX_INNER_GLOW;
        e.u.glow = glow(r, pats, n);
        break;
    case 4:
        e.kind = PC_FX_STROKE;
        e.u.stroke.common = common(r);
        e.u.stroke.size = size(r);
        e.u.stroke.position = POSITIONS[below(r, 3)];
        e.u.stroke.paint = fx_paint(r, pats, n);
        break;
    case 5:
        e.kind = PC_FX_COLOR_OVERLAY;
        e.u.color_overlay.common = common(r);
        e.u.color_overlay.color = color(r);
        break;
    case 6:
        e.kind = PC_FX_GRADIENT_OVERLAY;
        e.u.gradient_overlay.common = common(r);
        e.u.gradient_overlay.gradient = gradient(r);
        break;
    case 7:
        e.kind = PC_FX_PATTERN_OVERLAY;
        e.u.pattern_overlay.common = common(r);
        pattern_ref(r, pats, n, &e.u.pattern_overlay.name, &e.u.pattern_overlay.id);
        e.u.pattern_overlay.scale = pat_scale(r);
        e.u.pattern_overlay.angle = angle(r);
        e.u.pattern_overlay.link = below(r, 4) != 0;
        phase(r, e.u.pattern_overlay.phase);
        break;
    case 8:
        e.kind = PC_FX_SATIN;
        e.u.satin.common = common(r);
        e.u.satin.color = color(r);
        e.u.satin.angle = angle(r);
        e.u.satin.distance = size(r);
        e.u.satin.size = size(r);
        e.u.satin.contour = contour(r);
        e.u.satin.anti_alias = true;
        e.u.satin.invert = below(r, 2) == 0;
        break;
    default:
        e.kind = PC_FX_BEVEL_EMBOSS;
        e.u.bevel = bevel(r, pats, n);
        break;
    }
    return e;
}

static void set_effects(rng *r, pc_layer *l, const pc_pattern *pats, size_t np)
{
    uint64_t n = 1 + below(r, 4);
    uint64_t i;
    l->effects.items = xcalloc((size_t)n, sizeof *l->effects.items);
    l->effects.nitems = (size_t)n;
    for (i = 0; i < n; i++) {
        l->effects.items[i] = effect(r, pats, np);
    }
}

static pc_pattern *patterns(rng *r, size_t *count)
{
    uint64_t n = below(r, 4);
    pc_pattern *v = xcalloc((size_t)n, sizeof *v);
    uint64_t i;
    for (i = 0; i < n; i++) {
        uint32_t w = 1 + (uint32_t)below(r, 5);
        uint32_t h = 1 + (uint32_t)below(r, 5);
        uint64_t k = below(r, 5);
        pc_pixel_format fmt;
        pc_surface s;
        size_t ch;
        int32_t x;
        int32_t y;
        char name[32];
        if (k <= 1) {
            fmt = PC_PIXEL_FORMAT_RGBA8;
        } else if (k == 2) {
            fmt = PC_PIXEL_FORMAT_RGBA16;
        } else {
            fmt.mode = k == 3 ? PC_COLOR_MODE_GRAYSCALE : PC_COLOR_MODE_CMYK;
            fmt.sample = PC_SAMPLE_TYPE_U8;
            fmt.alpha = true;
        }
        pc_surface_init(&s, fmt);
        ch = pc_pixel_format_channels(fmt);
        for (y = 0; y < (int32_t)h; y++) {
            for (x = 0; x < (int32_t)w; x++) {
                float px[PC_RASTER_MAX_CHANNELS];
                size_t c;
                for (c = 0; c < ch; c++) {
                    float u = unit(r);
                    px[c] = c + 1 == ch && u > 0.3f ? 1.0f : u;
                }
                CHECK(pc_surface_write_pixel(&s, x, y, px, ch, NULL) == PC_OK);
            }
        }
        (void)snprintf(name, sizeof name, "p%llu", (unsigned long long)i);
        if (pc_pattern_new(name, strlen(name), &s, w, h, &v[i], NULL) != PC_OK) {
            abort();
        }
    }
    *count = (size_t)n;
    return v;
}

static void free_patterns(pc_pattern *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        pc_pattern_fini(&p[i]);
    }
    free(p);
}

static pc_global_light light(rng *r)
{
    pc_global_light l;
    const float hostile[3] = {0.0f, 90.0f, NAN};
    l.angle = angle(r);
    switch (below(r, 4)) {
    case 0:
        l.altitude = 30.0f;
        break;
    case 1:
        l.altitude = unit(r) * 90.0f;
        break;
    case 2:
        l.altitude = 60.0f;
        break;
    default:
        l.altitude = hostile[below(r, 3)];
        break;
    }
    return l;
}

/* ---- unit sweeps ---- */

static float *shape(rng *r, size_t w, size_t h)
{
    float *v = xcalloc(w * h, sizeof *v);
    uint64_t n;
    uint64_t i;
    uint64_t k;
    if (w == 0 || h == 0) {
        return v;
    }
    n = 1 + below(r, 3);
    for (i = 0; i < n; i++) {
        size_t x0 = (size_t)below(r, w);
        size_t y0 = (size_t)below(r, h);
        size_t x1 = x0 + 1 + (size_t)below(r, w - x0);
        size_t y1 = y0 + 1 + (size_t)below(r, h - y0);
        float a = below(r, 3) == 0 ? unit(r) : 1.0f;
        size_t x;
        size_t y;
        for (y = y0; y < y1; y++) {
            for (x = x0; x < x1; x++) {
                v[y * w + x] = a;
            }
        }
    }
    k = below(r, 6);
    for (i = 0; i < k; i++) {
        size_t j = (size_t)below(r, w * h);
        v[j] = unit(r);
    }
    return v;
}

static void unit_sweeps(out *o)
{
    const float widths[17] = {0.0f,  0.5f,   1.0f,   1.5f,  2.0f,     3.5f,      4.0f, 5.0f, 8.25f,
                              41.0f, 511.7f, 512.0f, 1e30f, INFINITY, -INFINITY, NAN,  -3.0f};
    const float ranges[10] = {1.0f, 0.5f, 0.01f, 0.0f, -1.0f, NAN, INFINITY, 0.999f, 2.0f, 0.3f};
    const int64_t ys[10] = {-3, -1, 0, 1, 2, 77, (int64_t)1 << 31, -((int64_t)1 << 40), INT64_MAX, INT64_MIN};
    const int64_t xs[7] = {-2, 0, 1, 5, 1000, (int64_t)1 << 33, INT64_MIN};
    const float ts[9] = {0.0f, 1.0f, 0.5f, 0.25f, -1.0f, 2.0f, NAN, 0.999f, 1e-7f};
    const float vals[12] = {0.0f, 1.0f, 0.5f, 2.5f, -1.0f, 4.0f, 16.0f, 1e30f, INFINITY, NAN, 0.25f, 0.75f};
    const float douts[5] = {0.0f, 1.0f, 3.0f, -0.5f, NAN};
    uint64_t h = H0;
    rng r;
    int i;
    int j;
    int k;
    for (i = 0; i < 17; i++) {
        int32_t rad;
        float *w = NULL;
        size_t n = 0;
        size_t q;
        CHECK(pc_compose_fx_tent_kernel(widths[i], &rad, &w, &n, NULL) == PC_OK);
        hi(&h, rad);
        hi(&h, (int32_t)n);
        for (q = 0; q < n; q++) {
            hf(&h, w[q]);
        }
        free(w);
    }
    put(o, "unit\ttent_kernel\t%016llx\n", (unsigned long long)h);

    r.s = 0x12345678;
    h = H0;
    for (i = 0; i < 40; i++) {
        pc_contour c = contour(&r);
        for (j = 0; j < 10; j++) {
            float *l = NULL;
            CHECK(pc_compose_fx_ranged_lut(&c, ranges[j], &l, NULL) == PC_OK);
            if (l == NULL) {
                hi(&h, -1);
            } else {
                hi(&h, PC_COMPOSE_FX_LUT_SIZE);
                for (k = 0; k < PC_COMPOSE_FX_LUT_SIZE; k++) {
                    hf(&h, l[k]);
                }
            }
            free(l);
        }
        pc_contour_fini(&c);
    }
    put(o, "unit\tranged_lut\t%016llx\n", (unsigned long long)h);

    h = H0;
    for (i = 0; i < 10; i++) {
        for (j = 0; j < 7; j++) {
            hf(&h, pc_compose_fx_hash_noise(xs[j], ys[i]));
        }
    }
    put(o, "unit\thash_noise\t%016llx\n", (unsigned long long)h);

    r.s = 0x9abcdef1;
    h = H0;
    for (i = 0; i < 60; i++) {
        pc_gradient g = gradient(&r);
        for (j = 0; j < 9; j++) {
            float v[4];
            pc_compose_fx_sample_gradient(&g, ts[j], v);
            for (k = 0; k < 4; k++) {
                hf(&h, v[k]);
            }
        }
        pc_gradient_fini(&g);
    }
    put(o, "unit\tsample_gradient\t%016llx\n", (unsigned long long)h);

    h = H0;
    for (i = 0; i < 12; i++) {
        for (j = 0; j < 12; j++) {
            float x;
            float y;
            pc_compose_fx_spread_split(vals[i], vals[j], &x, &y);
            hf(&h, x);
            hf(&h, y);
            hf(&h, pc_compose_fx_outline_share(vals[i], vals[j]));
        }
        hf(&h, pc_compose_fx_glow_gradient_gain(vals[i]));
    }
    put(o, "unit\tsplit_share_gain\t%016llx\n", (unsigned long long)h);

    h = H0;
    r.s = 0x7777;
    for (i = 0; i < 200; i++) {
        pc_effect e;
        pc_compose_bevel_geom g;
        memset(&e, 0, sizeof e);
        e.kind = PC_FX_BEVEL_EMBOSS;
        e.u.bevel = bevel(&r, NULL, 0);
        g = pc_compose_fx_bevel_geom(&e.u.bevel);
        hi(&h, (int32_t)g.paint);
        hf(&h, g.width);
        hf(&h, g.depth);
        hi(&h, g.pillow ? 1 : 0);
        hf(&h, g.chisel_soft);
        pc_effect_fini(&e);
    }
    for (i = 0; i < 3; i++) {
        int s;
        for (s = 0; s < 12; s++) {
            for (j = 0; j < 12; j++) {
                for (k = 0; k < 5; k++) {
                    hf(&h,
                       pc_compose_fx_bevel_chisel_h((pc_compose_bevel_paint)i, vals[s], vals[j], douts[k]));
                }
            }
        }
    }
    put(o, "unit\tbevel_geom\t%016llx\n", (unsigned long long)h);

    h = H0;
    for (i = 0; i < 12; i++) {
        for (j = 0; j < 3; j++) {
            pc_stroke_fx st;
            float a;
            float b;
            memset(&st, 0, sizeof st);
            st.size = vals[i];
            st.position = POSITIONS[j];
            pc_compose_fx_stroke_widths(&st, &a, &b);
            hf(&h, a);
            hf(&h, b);
        }
    }
    put(o, "unit\tstroke_widths\t%016llx\n", (unsigned long long)h);

    for (k = 0; k < 6; k++) {
        rng rr = {0x5151 + (uint64_t)k};
        h = H0;
        for (i = 0; i < 12; i++) {
            uint64_t q = below(&rr, 6);
            size_t w;
            size_t hh;
            float *a;
            float *d = NULL;
            size_t m;
            if (q == 0) {
                w = 0;
                hh = 5;
            } else if (q == 1) {
                w = 1;
                hh = 1;
            } else {
                w = 1 + (size_t)below(&rr, 30);
                hh = 1 + (size_t)below(&rr, 20);
            }
            a = shape(&rr, w, hh);
            CHECK(pc_compose_fx_distance_field((pc_compose_fx_field_kind)k, a, w, hh, &d, NULL) == PC_OK);
            for (m = 0; d != NULL && m < w * hh; m++) {
                hf(&h, d[m]);
            }
            free(d);
            free(a);
        }
        put(o, "unit\tdistance_field_%d\t%016llx\n", k, (unsigned long long)h);
    }
}

/* ---- maps ---- */

static pc_layer layer_kind(rng *r, int32_t w, int32_t h)
{
    uint64_t k = below(r, 5);
    pc_layer l;
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    if (k <= 1) {
        return ch_raster("l", PC_PIXEL_FORMAT_RGBA8);
    }
    if (k <= 3) {
        uint64_t n = 3 + below(r, 2);
        double pts[4][2];
        uint64_t i;
        for (i = 0; i < n; i++) {
            int32_t x = range(r, -2, w + 2);
            int32_t y = range(r, -2, h + 2);
            pts[i][0] = (double)x + 0.5;
            pts[i][1] = (double)y + 0.25;
        }
        c.kind = PC_LAYER_SHAPE;
        c.u.shape.path = ch_polygon((const double (*)[2])pts, (size_t)n);
        if (below(r, 4) != 0) {
            c.u.shape.has_fill = true;
            c.u.shape.fill = pc_fill_solid(pc_color_rgb(1.0f, 0.0f, 0.0f));
        }
        if (pc_layer_init(&l, "shape", &c, NULL) != PC_OK) {
            abort();
        }
        return l;
    }
    c.kind = PC_LAYER_TEXT;
    pc_text_layer_default(&c.u.text);
    if (pc_layer_init(&l, "type", &c, NULL) != PC_OK) {
        abort();
    }
    return l;
}

static float opacity(rng *r)
{
    uint64_t k = below(r, 10);
    if (k <= 4) {
        return 1.0f;
    }
    if (k == 5) {
        return 0.5f;
    }
    if (k == 6) {
        return 0.25f;
    }
    if (k == 7) {
        return unit(r);
    }
    if (k == 8) {
        return 0.0f;
    }
    return below(r, 2) == 0 ? 1.5f : -0.25f;
}

static uint64_t composite_hash(const pc_layer *layer, const pc_compose_buffer *content, pc_rect brect,
                               const float bg[4], const pc_compose_fx_maps *maps, pc_rect lb,
                               const pc_compose_patterns *pp, uint64_t seed)
{
    pc_compose_buffer back;
    pc_error e;
    uint64_t h;
    CHECK(pc_compose_buffer_filled(brect, bg, &back, NULL) == PC_OK);
    if (pc_compose_fx_composite(layer, content, &back, maps, lb, pp, NULL, &e) != PC_OK) {
        PC_T_FAIL("map %llu: composite failed: %s", (unsigned long long)seed, e.msg);
    }
    h = hbuf(&back);
    pc_compose_buffer_fini(&back);
    return h;
}

static void map_line(out *o, uint64_t seed)
{
    rng r = {(seed * 0x2545f4914f6cdd1dull) | 1};
    int32_t w = 1 + (int32_t)below(&r, 40);
    int32_t h = 1 + (int32_t)below(&r, 30);
    int32_t x0 = range(&r, -20, 20);
    int32_t y0 = range(&r, -20, 20);
    pc_rect rect = pc_rect_make(x0, y0, x0 + w, y0 + h);
    float *shp = shape(&r, (size_t)w, (size_t)h);
    size_t np = 0;
    pc_pattern *pats = patterns(&r, &np);
    pc_layer layer = layer_kind(&r, w, h);
    pc_global_light lt;
    pc_compose_texture_ctx tex;
    pc_compose_fx_maps *maps = NULL;
    pc_compose_patterns *pp = NULL;
    float *copy;
    pc_error e;
    uint64_t hfr = H0;
    size_t i;
    bool faint;
    pc_compose_buffer content;
    pc_compose_buffer csub;
    pc_rect lb;
    float bg[4];
    int32_t sx;
    int32_t sy;
    int32_t sw;
    int32_t sh;
    int32_t ox;
    int32_t oy;
    int32_t x;
    int32_t y;
    pc_rect sub;
    set_effects(&r, &layer, pats, np);
    if (below(&r, 4) == 0) {
        float a = unit(&r) * (float)w;
        float b = unit(&r) * (float)h;
        layer.effects.has_reference = true;
        layer.effects.reference[0] = (double)x0 + (double)a;
        layer.effects.reference[1] = (double)y0 + (double)b;
    }
    layer.opacity = opacity(&r);
    layer.fill_opacity = opacity(&r);
    if (below(&r, 2) == 0) {
        layer.blend = PC_BLEND_LAYER_MODES[below(&r, 27)];
    }
    if (below(&r, 5) == 0) {
        layer.advanced.transparency_shapes = false;
    }
    if (below(&r, 4) == 0) {
        layer.advanced.blend_interior = true;
    }
    lt = light(&r);
    tex.rect = rect;
    tex.patterns = pats;
    tex.npatterns = np;
    tex.anchor[0] = (double)x0;
    tex.anchor[1] = (double)y0;
    if (below(&r, 2) == 0) {
        float a = unit(&r) * 10.0f;
        float b = unit(&r) * 10.0f;
        tex.anchor[0] = (double)x0 + (double)a;
        tex.anchor[1] = (double)y0 + (double)b;
    }
    copy = xcalloc((size_t)w * (size_t)h, sizeof *copy);
    memcpy(copy, shp, (size_t)w * (size_t)h * sizeof *copy);
    if (pc_compose_fx_build_maps(&layer, copy, (size_t)w * (size_t)h, rect, &lt, &tex, NULL, &maps, &e) !=
        PC_OK) {
        PC_T_FAIL("map %llu: build failed: %s", (unsigned long long)seed, e.msg);
        free(shp);
        free_patterns(pats, np);
        pc_layer_fini(&layer);
        return;
    }
    put(o, "map\t%llu\tbytes=%zu", (unsigned long long)seed, pc_compose_fx_maps_bytes(maps));
    for (i = 0; i < layer.effects.nitems; i++) {
        const pc_effect *ef = &layer.effects.items[i];
        pc_rect f;
        if (ef->kind != PC_FX_STROKE) {
            continue;
        }
        if (pc_compose_fx_maps_stroke_frame(maps, &ef->u.stroke, &f)) {
            hrect(&hfr, f);
        } else {
            hi(&hfr, -7);
        }
    }
    put(o, "\tframes=%016llx", (unsigned long long)hfr);
    faint = below(&r, 3) == 0;
    CHECK(pc_compose_buffer_transparent(rect, &content, NULL) == PC_OK);
    for (i = 0; i < content.len; i++) {
        float a = unit(&r);
        float b = unit(&r);
        float cc = unit(&r);
        float k = faint ? unit(&r) : 1.0f;
        content.px[i][0] = a;
        content.px[i][1] = b;
        content.px[i][2] = cc;
        content.px[i][3] = shp[i] * k;
    }
    if (below(&r, 2) == 0) {
        lb = rect;
    } else {
        int32_t lx = range(&r, x0, x0 + w);
        int32_t ly = range(&r, y0, y0 + h);
        int32_t lw = range(&r, 1, w + 1);
        int32_t lh = range(&r, 1, h + 1);
        lb = pc_rect_make(lx, ly, lx + lw, ly + lh);
    }
    bg[0] = unit(&r);
    bg[1] = unit(&r);
    bg[2] = unit(&r);
    bg[3] = below(&r, 3) == 0 ? 0.0f : unit(&r);
    CHECK(pc_compose_patterns_new(pats, np, PC_COMPOSE_PREPARED_PATTERN_BYTES, &pp, NULL) == PC_OK);
    put(o, "\tfull=%016llx",
        (unsigned long long)composite_hash(&layer, &content, rect, bg, maps, lb, pp, seed));
    sx = range(&r, x0 - 3, x0 + w);
    sy = range(&r, y0 - 3, y0 + h);
    sw = range(&r, 1, w + 4);
    sh = range(&r, 1, h + 4);
    sub = pc_rect_make(sx, sy, sx + sw, sy + sh);
    CHECK(pc_compose_buffer_transparent(sub, &csub, NULL) == PC_OK);
    for (y = sub.y0; y < sub.y1; y++) {
        for (x = sub.x0; x < sub.x1; x++) {
            if (x >= rect.x0 && x < rect.x1 && y >= rect.y0 && y < rect.y1) {
                size_t k = (size_t)((y - sub.y0) * sw + (x - sub.x0));
                memcpy(csub.px[k], pc_compose_buffer_get(&content, x, y), 4 * sizeof(float));
            }
        }
    }
    put(o, "\tsub=%016llx", (unsigned long long)composite_hash(&layer, &csub, sub, bg, maps, lb, pp, seed));
    ox = range(&r, -4, 5);
    oy = range(&r, -4, 5);
    put(o, "\toff=%016llx\n",
        (unsigned long long)composite_hash(
            &layer, &csub, pc_rect_make(sub.x0 + ox, sub.y0 + oy, sub.x1 + ox + 1, sub.y1 + oy), bg, maps, lb,
            pp, seed));
    pc_compose_buffer_fini(&csub);
    pc_compose_buffer_fini(&content);
    pc_compose_patterns_free(pp);
    pc_compose_fx_maps_free(maps);
    free(shp);
    free_patterns(pats, np);
    pc_layer_fini(&layer);
}

/* ---- documents ---- */

static pc_pixel_format layer_format(rng *r, pc_pixel_format doc_fmt)
{
    uint64_t k = below(r, 6);
    if (k <= 2) {
        return doc_fmt;
    }
    if (k == 3) {
        return PC_PIXEL_FORMAT_RGBA8;
    }
    if (k == 4) {
        return PC_PIXEL_FORMAT_RGBA16;
    }
    return PC_PIXEL_FORMAT_RGBA32F;
}

static size_t pixel(rng *r, pc_pixel_format fmt, float px[PC_RASTER_MAX_CHANNELS])
{
    size_t n = pc_pixel_format_channels(fmt);
    size_t k;
    for (k = 0; k < n; k++) {
        float u = unit(r);
        px[k] = k + 1 == n && fmt.alpha && u > 0.4f ? 1.0f : u;
    }
    return n;
}

static void paint_surface(rng *r, pc_surface *s, int32_t w, int32_t h)
{
    pc_pixel_format fmt = pc_surface_format(s);
    uint64_t n = 1 + below(r, 3);
    uint64_t i;
    float px[PC_RASTER_MAX_CHANNELS];
    for (i = 0; i < n; i++) {
        int32_t x0 = range(r, -3, w);
        int32_t y0 = range(r, -3, h);
        int32_t x1 = x0 + range(r, 1, w / 2 + 3);
        int32_t y1 = y0 + range(r, 1, h / 2 + 3);
        size_t c = pixel(r, fmt, px);
        CHECK(pc_surface_fill_rect(s, pc_rect_make(x0, y0, x1, y1), px, c, NULL) == PC_OK);
    }
    n = below(r, 4);
    for (i = 0; i < n; i++) {
        int32_t x = range(r, 0, w);
        int32_t y = range(r, 0, h);
        size_t c = pixel(r, fmt, px);
        CHECK(pc_surface_write_pixel(s, x, y, px, c, NULL) == PC_OK);
    }
}

static pc_path polygon(rng *r, int32_t w, int32_t h, uint64_t k)
{
    double (*pts)[2] = xcalloc((size_t)k, sizeof *pts);
    uint64_t i;
    pc_path p;
    for (i = 0; i < k; i++) {
        int32_t x = range(r, -2, w + 2);
        uint64_t q = below(r, 4);
        int32_t y = range(r, -2, h + 2);
        pts[i][0] = (double)x + 0.25 * (double)q;
        pts[i][1] = (double)y + 0.5;
    }
    p = ch_polygon((const double (*)[2])pts, (size_t)k);
    free(pts);
    return p;
}

typedef struct gen {
    pc_pixel_format fmt;
    int32_t w;
    int32_t h;
    const pc_pattern *pats;
    size_t npats;
    bool c2;
} gen;

static pc_layer gen_layer(rng *r, const gen *g, unsigned level);

static pc_layer *gen_layers(rng *r, const gen *g, unsigned level, uint64_t n)
{
    pc_layer *v = xcalloc((size_t)n, sizeof *v);
    uint64_t i;
    for (i = 0; i < n; i++) {
        v[i] = gen_layer(r, g, level);
        if (i > 0 && below(r, 4) == 0) {
            v[i].clipped = true;
        }
    }
    return v;
}

static pc_layer gen_layer(rng *r, const gen *g, unsigned level)
{
    int32_t w = g->w;
    int32_t h = g->h;
    uint64_t kind = below(r, 10);
    pc_layer l;
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    if (kind == 6 && level < 2) {
        uint64_t n = 1 + below(r, 3);
        pc_layer *kids = gen_layers(r, g, level + 1, n);
        if (pc_layer_group(&l, "g", kids, (size_t)n, NULL) != PC_OK) {
            abort();
        }
        if (below(r, 2) == 0) {
            l.blend = PC_BLEND_NORMAL;
        }
        if (below(r, 6) == 0) {
            pc_group *gr = &l.content.u.group;
            int32_t ax0 = range(r, -2, w);
            int32_t ay0 = range(r, -2, h);
            int32_t ax1 = ax0 + range(r, 1, w + 2);
            int32_t ay1 = ay0 + range(r, 1, h + 2);
            pc_artboard_init(&gr->artboard, pc_rect_make(ax0, ay0, ax1, ay1));
            gr->has_artboard = true;
            gr->artboard.background.kind = below(r, 2) == 0 ? PC_ARTBOARD_WHITE : PC_ARTBOARD_TRANSPARENT;
        }
    } else if (kind == 7) {
        c.kind = PC_LAYER_FILL;
        if (g->c2 && below(r, 2) == 0) {
            if (below(r, 2) == 0) {
                pc_gradient gr = gradient(r);
                CHECK(pc_fill_gradient(gr.stops, gr.nstops, gr.angle, isfinite(gr.scale) ? gr.scale : 1.0f,
                                       gr.style, gr.reverse, &c.u.fill, NULL) == PC_OK);
                pc_gradient_fini(&gr);
            } else {
                const float scales[3] = {1.0f, 0.5f, 2.0f};
                const float angles[2] = {0.0f, 30.0f};
                c.u.fill.kind = PC_FILL_PATTERN;
                pattern_ref(r, g->pats, g->npats, &c.u.fill.u.pattern.name, &c.u.fill.u.pattern.id);
                c.u.fill.u.pattern.scale = scales[below(r, 3)];
                c.u.fill.u.pattern.angle = angles[below(r, 2)];
                c.u.fill.u.pattern.link = true;
            }
        } else {
            c.u.fill = pc_fill_solid(color(r));
        }
        if (pc_layer_init(&l, "fill", &c, NULL) != PC_OK) {
            abort();
        }
    } else if (kind == 8) {
        pc_surface s;
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        paint_surface(r, &s, w, h);
        c.kind = PC_LAYER_TEXT;
        pc_text_layer_default(&c.u.text);
        c.u.text.has_cache = true;
        c.u.text.cache = s;
        if (pc_layer_init(&l, "type", &c, NULL) != PC_OK) {
            abort();
        }
    } else if (kind == 9) {
        uint64_t k = 3 + below(r, 2);
        bool filled;
        pc_color cc;
        c.kind = PC_LAYER_SHAPE;
        c.u.shape.path = polygon(r, w, h, k);
        filled = below(r, 4) != 0;
        cc = color(r);
        if (filled) {
            c.u.shape.has_fill = true;
            c.u.shape.fill = pc_fill_solid(cc);
        }
        if (g->c2 && below(r, 2) == 0) {
            const float widths[3] = {1.0f, 3.0f, 5.0f};
            const pc_stroke_align aligns[3] = {PC_STROKE_ALIGN_CENTER, PC_STROKE_ALIGN_INSIDE,
                                               PC_STROKE_ALIGN_OUTSIDE};
            float sw = widths[below(r, 3)];
            pc_stroke_align al = aligns[below(r, 3)];
            pc_color sc = color(r);
            pc_shape_stroke_default(&c.u.shape.stroke);
            c.u.shape.stroke.width = sw;
            c.u.shape.stroke.align = al;
            c.u.shape.stroke.paint = pc_fill_solid(sc);
            c.u.shape.has_stroke = true;
        }
        CHECK(pc_vec_render_shape(&c.u.shape, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, w, h),
                                  &c.u.shape.cache, NULL) == PC_OK);
        c.u.shape.has_cache = true;
        if (pc_layer_init(&l, "shape", &c, NULL) != PC_OK) {
            abort();
        }
    } else {
        pc_pixel_format fmt = layer_format(r, g->fmt);
        l = ch_raster("px", fmt);
        paint_surface(r, pc_layer_surface_mut(&l), w, h);
    }
    if (below(r, 10) == 0) {
        l.visible = false;
    }
    if (below(r, 3) == 0) {
        l.blend = PC_BLEND_LAYER_MODES[below(r, 27)];
    }
    l.opacity = opacity(r);
    l.fill_opacity = opacity(r);
    if (below(r, 5) != 0) {
        set_effects(r, &l, g->pats, g->npats);
        if (below(r, 10) == 0) {
            l.effects.enabled = false;
        }
        if (below(r, 4) == 0) {
            float a = unit(r) * (float)w;
            float b = unit(r) * (float)h;
            l.effects.has_reference = true;
            l.effects.reference[0] = (double)a;
            l.effects.reference[1] = (double)b;
        }
    }
    if (below(r, 5) == 0) {
        pc_layer_mask *m = &l.mask;
        int32_t mx0;
        int32_t my0;
        int32_t mx1;
        int32_t my1;
        float v;
        const float feathers[3] = {0.0f, 0.0f, 2.0f};
        if (below(r, 2) == 0) {
            pc_layer_mask_reveal_all(m);
        } else {
            pc_layer_mask_hide_all(m);
        }
        l.has_mask = true;
        mx0 = range(r, -2, w);
        my0 = range(r, -2, h);
        v = unit(r);
        mx1 = mx0 + range(r, 1, w);
        my1 = my0 + range(r, 1, h);
        CHECK(pc_surface_fill_rect(&m->surface, pc_rect_make(mx0, my0, mx1, my1), &v, 1, NULL) == PC_OK);
        m->feather = feathers[below(r, 3)];
    }
    if (below(r, 7) == 0) {
        uint64_t k = 3 + below(r, 3);
        pc_path p = polygon(r, w, h, k);
        pc_vector_mask_init(&l.vector_mask, &p);
        l.has_vector_mask = true;
    }
    if (below(r, 6) == 0) {
        l.advanced.transparency_shapes = false;
    }
    if (below(r, 5) == 0) {
        l.advanced.blend_interior = true;
    }
    if (g->c2) {
        if (below(r, 4) == 0) {
            l.advanced.layer_mask_hides_effects = true;
        }
        if (below(r, 4) == 0) {
            l.advanced.vector_mask_hides_effects = true;
        }
        if (below(r, 6) == 0) {
            l.advanced.knockout = below(r, 2) == 0 ? PC_KNOCKOUT_SHALLOW : PC_KNOCKOUT_DEEP;
        }
        if (below(r, 5) == 0) {
            l.advanced.blend_clipped = false;
        }
    }
    return l;
}

static pc_document gen_doc(uint64_t seed)
{
    rng r = {(seed * 0x9e3779b97f4a7c15ull) | 1};
    pc_color_mode mode = MODES[below(&r, 6)];
    pc_sample_type depth = DEPTHS[below(&r, 3)];
    uint32_t w = 6 + (uint32_t)below(&r, 36);
    uint32_t h = 5 + (uint32_t)below(&r, 28);
    pc_document doc = ch_doc(w, h, mode, depth);
    gen g;
    uint64_t n;
    doc.patterns = patterns(&r, &doc.npatterns);
    doc.global_light = light(&r);
    g.fmt = pc_document_pixel_format(&doc);
    g.w = (int32_t)w;
    g.h = (int32_t)h;
    g.pats = doc.patterns;
    g.npats = doc.npatterns;
    g.c2 = seed > 1000;
    n = 1 + below(&r, 4);
    doc.layers = gen_layers(&r, &g, 0, n);
    doc.nlayers = (size_t)n;
    return doc;
}

static uint64_t must(pc_status st, const pc_compose_buffer *b, const char *what, uint64_t seed,
                     const pc_error *e)
{
    if (st != PC_OK) {
        PC_T_FAIL("doc %llu: %s failed: %s", (unsigned long long)seed, what, e->msg);
        return 0;
    }
    return hbuf(b);
}

static void doc_line(out *o, uint64_t seed)
{
    pc_document doc = gen_doc(seed);
    rng r = {seed ^ 0x0badcafeu};
    pc_rect b = pc_document_bounds(&doc);
    int32_t w = (int32_t)pc_rect_width(b);
    int32_t h = (int32_t)pc_rect_height(b);
    pc_compose_buffer buf;
    pc_error e;
    pc_status st;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    uint64_t hl = H0;
    size_t i;
    put(o, "doc\t%llu", (unsigned long long)seed);
    st = pc_compose_flatten(&doc, &buf, &e);
    put(o, "\tflat=%016llx", (unsigned long long)must(st, &buf, "flatten", seed, &e));
    if (st == PC_OK) {
        pc_compose_buffer again;
        CHECK(pc_compose_flatten(&doc, &again, NULL) == PC_OK && pc_compose_buffer_eq(&buf, &again));
        pc_compose_buffer_fini(&again);
        pc_compose_buffer_fini(&buf);
    }
    st = pc_compose_render_tiled(&doc, b, 7, NULL, &buf, &e);
    put(o, "\ttiled=%016llx", (unsigned long long)must(st, &buf, "tiled", seed, &e));
    pc_compose_buffer_fini(&buf);
    x0 = range(&r, -6, w);
    y0 = range(&r, -6, h);
    x1 = x0 + range(&r, 1, w + 8);
    y1 = y0 + range(&r, 1, h + 8);
    st = pc_compose_render_tiled(&doc, pc_rect_make(x0, y0, x1, y1), 5, NULL, &buf, &e);
    put(o, "\tsub=%016llx", (unsigned long long)must(st, &buf, "sub", seed, &e));
    pc_compose_buffer_fini(&buf);
    for (i = 0; i < doc.nlayers; i++) {
        st = pc_compose_render_layer(&doc.layers[i], b, &buf, &e);
        hu64(&hl, must(st, &buf, "render_layer", seed, &e));
        pc_compose_buffer_fini(&buf);
    }
    put(o, "\tlayers=%016llx\n", (unsigned long long)hl);
    for (i = 0; i < doc.nlayers; i++) {
        const pc_layer *l = &doc.layers[i];
        pc_rect q;
        float *shp = NULL;
        size_t n = 0;
        size_t k;
        uint64_t hs = H0;
        put(o, "q\t%llu\t%zu\tbounds=", (unsigned long long)seed, i);
        rs(o, pc_compose_layer_bounds(l, b));
        put(o, "\tcomp=");
        if (pc_compose_composite_bounds(l, b, &q)) {
            rs(o, q);
        } else {
            put(o, "none");
        }
        put(o, "\tchange=");
        if (pc_compose_change_bounds(l, b, &q)) {
            rs(o, q);
        } else {
            put(o, "none");
        }
        put(o, "\tmargin=%d", pc_compose_effects_margin(l));
        CHECK(pc_compose_layer_shape(&doc, l, b, &shp, &n, NULL) == PC_OK);
        for (k = 0; k < n; k++) {
            hf(&hs, shp[k]);
        }
        free(shp);
        put(o, "\tshape=%016llx", (unsigned long long)hs);
        hs = H0;
        for (k = 0; k < l->effects.nitems; k++) {
            const pc_effect *ef = &l->effects.items[k];
            bool found = false;
            if (ef->kind != PC_FX_STROKE) {
                continue;
            }
            if (pc_compose_stroke_frame(&doc, l, &ef->u.stroke, &found, &q, &e) != PC_OK) {
                PC_T_FAIL("doc %llu: stroke_frame failed: %s", (unsigned long long)seed, e.msg);
            }
            if (found) {
                hrect(&hs, q);
            } else {
                hi(&hs, -7);
            }
        }
        put(o, "\tsframes=%016llx\n", (unsigned long long)hs);
    }
    pc_document_fini(&doc);
}

/* ---- comparison ---- */

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    size_t cap = 0;
    size_t n = 0;
    if (f == NULL) {
        return NULL;
    }
    for (;;) {
        size_t got;
        if (n + 65536 > cap) {
            char *g = realloc(buf, cap = n + 65536 * 4);
            if (g == NULL) {
                free(buf);
                (void)fclose(f);
                return NULL;
            }
            buf = g;
        }
        got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            break;
        }
    }
    (void)fclose(f);
    *len = n;
    return buf;
}

static void test_parity(void)
{
    out o = {NULL, 0, 0};
    size_t glen = 0;
    char *golden = read_file(GOLDEN, &glen);
    uint64_t seed;
    size_t a = 0;
    size_t b = 0;
    int lines = 0;
    int bad = 0;
    CHECK(golden != NULL);
    if (golden == NULL) {
        return;
    }
    unit_sweeps(&o);
    for (seed = 1; seed <= 500; seed++) {
        map_line(&o, seed);
    }
    for (seed = 1; seed <= 300; seed++) {
        doc_line(&o, seed);
    }
    for (seed = 1001; seed <= 1080; seed++) {
        doc_line(&o, seed);
    }
    while (a < o.len || b < glen) {
        size_t ea = a;
        size_t eb = b;
        while (ea < o.len && o.s[ea] != '\n') {
            ea++;
        }
        while (eb < glen && golden[eb] != '\n') {
            eb++;
        }
        lines++;
        pc_t_checks++;
        if (ea - a != eb - b || memcmp(o.s + a, golden + b, ea - a) != 0) {
            if (bad < 30) {
                (void)fprintf(stderr, "line %d\n  got:  %.*s\n  want: %.*s\n", lines, (int)(ea - a), o.s + a,
                              (int)(eb - b), golden + b);
            }
            bad++;
        }
        a = ea + 1;
        b = eb + 1;
    }
    if (bad > 0) {
        PC_T_FAIL("%d of %d lines differ", bad, lines);
    }
    free(o.s);
    free(golden);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_parity);
    return pc_t_finish();
}
