/* The seeded generator of tests/compose/test_parity2.c (header-only; that test is its one TU):
 * documents, adjustments, gradient / pattern fills and stroked shapes built from the xorshift
 * stream of tools/rustref/src/compose2_vectors.rs, every draw in the same order. */
#ifndef PC_TESTS_COMPOSE_GEN2_H
#define PC_TESTS_COMPOSE_GEN2_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"

/* ---- the generator (mirrors compose2_vectors.rs draw for draw) ---- */

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

static int32_t range(rng *r, int32_t lo, int32_t hi)
{
    return lo + (int32_t)below(r, (uint64_t)(hi - lo));
}

static float span(rng *r, float lo, float hi)
{
    float u = unit(r);
    return lo + u * (hi - lo);
}

static const pc_color_mode MODES[6] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE,    PC_COLOR_MODE_CMYK,
                                       PC_COLOR_MODE_LAB, PC_COLOR_MODE_MULTICHANNEL, PC_COLOR_MODE_DUOTONE};
static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
static const pc_gradient_style STYLES[5] = {PC_GRADIENT_LINEAR, PC_GRADIENT_RADIAL, PC_GRADIENT_ANGLE,
                                            PC_GRADIENT_REFLECTED, PC_GRADIENT_DIAMOND};
static const pc_tone_space SPACES[3] = {PC_TONE_RGB, PC_TONE_CMYK, PC_TONE_LAB};

static void *xmalloc(size_t n)
{
    void *p = malloc(n > 0 ? n : 1);
    if (p == NULL) {
        abort(); /* test setup only */
    }
    return p;
}

static void must(pc_status st)
{
    if (st != PC_OK) {
        abort(); /* test setup only */
    }
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

static pc_pixel_format pf(pc_color_mode m, pc_sample_type s, bool alpha)
{
    pc_pixel_format f;
    f.mode = m;
    f.sample = s;
    f.alpha = alpha;
    return f;
}

static pc_pixel_format layer_format(rng *r, pc_pixel_format doc_fmt)
{
    uint64_t k = below(r, 8);
    if (k <= 2) {
        return doc_fmt;
    }
    if (k == 3) {
        return PC_PIXEL_FORMAT_RGBA8;
    }
    if (k == 4) {
        return PC_PIXEL_FORMAT_RGBA16;
    }
    if (k == 5) {
        return PC_PIXEL_FORMAT_RGBA32F;
    }
    if (k == 6) {
        uint64_t d = below(r, 3);
        return pf(PC_COLOR_MODE_GRAYSCALE, DEPTHS[d], true);
    } else {
        uint64_t m = below(r, 2);
        uint64_t d = below(r, 3);
        return pf(m == 0 ? PC_COLOR_MODE_CMYK : PC_COLOR_MODE_LAB, DEPTHS[d], true);
    }
}

static size_t pixel(rng *r, pc_pixel_format fmt, float px[PC_RASTER_MAX_CHANNELS])
{
    size_t n = pc_pixel_format_channels(fmt);
    bool hdr = fmt.sample == PC_SAMPLE_TYPE_F32 && below(r, 4) == 0;
    size_t i;
    for (i = 0; i < n; i++) {
        float u = unit(r);
        px[i] = hdr ? u * 2.0f : u;
    }
    return n;
}

static void paint(rng *r, pc_surface *s, int32_t w, int32_t h)
{
    pc_pixel_format fmt = pc_surface_format(s);
    uint64_t k = 1 + below(r, 3);
    uint64_t i;
    float px[PC_RASTER_MAX_CHANNELS];
    for (i = 0; i < k; i++) {
        int32_t x0 = range(r, -4, w + 4);
        int32_t y0 = range(r, -4, h + 4);
        int32_t x1 = x0 + range(r, 1, w + 2);
        int32_t y1 = y0 + range(r, 1, h + 2);
        size_t n = pixel(r, fmt, px);
        CHECK(pc_surface_fill_rect(s, pc_rect_make(x0, y0, x1, y1), px, n, NULL) == PC_OK);
    }
    k = below(r, 4);
    for (i = 0; i < k; i++) {
        int32_t x = range(r, 0, w);
        int32_t y = range(r, 0, h);
        size_t n = pixel(r, fmt, px);
        CHECK(pc_surface_write_pixel(s, x, y, px, n, NULL) == PC_OK);
    }
}

static pc_color color(rng *r)
{
    uint64_t k = below(r, 6);
    float c0 = unit(r);
    float c1 = unit(r);
    float c2 = unit(r);
    float c3 = unit(r);
    float a = below(r, 2) == 0 ? 1.0f : unit(r);
    pc_color c;
    memset(&c, 0, sizeof c);
    c.alpha = a;
    if (k <= 2 || k == 5) {
        c.mode = k == 5 ? PC_COLOR_MODE_LAB : PC_COLOR_MODE_RGB;
        c.c[0] = c0;
        c.c[1] = c1;
        c.c[2] = c2;
    } else if (k == 3) {
        c.mode = PC_COLOR_MODE_GRAYSCALE;
        c.c[0] = c0;
    } else {
        c.mode = PC_COLOR_MODE_CMYK;
        c.c[0] = c0;
        c.c[1] = c1;
        c.c[2] = c2;
        c.c[3] = c3;
    }
    return c;
}

static pc_levels_channel levels_channel(rng *r)
{
    pc_levels_channel c;
    float u;
    uint64_t k;
    if (below(r, 4) == 0) {
        return pc_levels_channel_default();
    }
    c.in_black = span(r, 0.0f, 0.4f);
    c.in_white = span(r, 0.6f, 1.0f);
    u = unit(r);
    k = below(r, 5);
    c.gamma = k == 0 ? 1.0f : (k == 1 ? 0.5f : (k == 2 ? 1.78f : (k == 3 ? 2.5f : u * 3.0f)));
    c.out_black = span(r, 0.0f, 0.2f);
    c.out_white = span(r, 0.8f, 1.0f);
    return c;
}

static pc_curve curve(rng *r)
{
    uint64_t n = below(r, 6);
    pc_curve c;
    uint64_t i;
    c.pts = n > 0 ? xmalloc((size_t)n * sizeof *c.pts) : NULL;
    c.len = (size_t)n;
    for (i = 0; i < n; i++) {
        bool dup = i > 0 && below(r, 5) == 0;
        c.pts[i].input = dup ? c.pts[i - 1].input : unit(r);
        c.pts[i].output = span(r, -0.1f, 1.1f);
    }
    return c;
}

static pc_adjustment adjustment(rng *r)
{
    uint64_t kind = below(r, 17);
    pc_adjustment a;
    size_t i;
    size_t j;
    memset(&a, 0, sizeof a);
    switch (kind) {
    case 0:
        a.kind = PC_ADJ_BRIGHTNESS_CONTRAST;
        a.u.brightness_contrast.brightness = span(r, -150.0f, 150.0f);
        a.u.brightness_contrast.contrast = span(r, -120.0f, 120.0f);
        a.u.brightness_contrast.legacy = below(r, 2) == 0;
        break;
    case 1:
        a.kind = PC_ADJ_LEVELS;
        a.u.levels.master = levels_channel(r);
        for (i = 0; i < 3; i++) {
            a.u.levels.per_channel[i] = levels_channel(r);
        }
        a.u.levels.space = SPACES[below(r, 3)];
        a.u.levels.black = levels_channel(r);
        break;
    case 2:
        a.kind = PC_ADJ_CURVES;
        a.u.curves.master = curve(r);
        for (i = 0; i < 3; i++) {
            a.u.curves.per_channel[i] = curve(r);
        }
        a.u.curves.space = SPACES[below(r, 3)];
        a.u.curves.black = curve(r);
        break;
    case 3: {
        static const float gammas[4] = {1.0f, 0.5f, 2.2f, 0.0f};
        a.kind = PC_ADJ_EXPOSURE;
        a.u.exposure.exposure = span(r, -2.0f, 2.0f);
        a.u.exposure.offset = span(r, -0.2f, 0.2f);
        a.u.exposure.gamma = gammas[below(r, 4)];
        break;
    }
    case 4:
        a.kind = PC_ADJ_VIBRANCE;
        a.u.vibrance.vibrance = below(r, 3) == 0 ? 0.0f : span(r, -100.0f, 100.0f);
        a.u.vibrance.saturation = below(r, 3) == 0 ? 0.0f : span(r, -100.0f, 100.0f);
        break;
    case 5:
        a.kind = PC_ADJ_HUE_SATURATION;
        a.u.hue_saturation.hue = span(r, -180.0f, 180.0f);
        a.u.hue_saturation.saturation = span(r, -100.0f, 100.0f);
        a.u.hue_saturation.lightness = span(r, -100.0f, 100.0f);
        a.u.hue_saturation.colorize = below(r, 4) == 0;
        pc_hue_range_defaults(a.u.hue_saturation.ranges);
        for (i = 0; i < 6; i++) {
            if (below(r, 3) == 0) {
                a.u.hue_saturation.ranges[i].hue = span(r, -180.0f, 180.0f);
                a.u.hue_saturation.ranges[i].saturation = span(r, -100.0f, 100.0f);
                a.u.hue_saturation.ranges[i].lightness = span(r, -100.0f, 100.0f);
            }
        }
        break;
    case 6: {
        float v[9];
        a.kind = PC_ADJ_COLOR_BALANCE;
        for (i = 0; i < 9; i++) {
            v[i] = span(r, -100.0f, 100.0f);
        }
        memcpy(a.u.color_balance.shadows, v, 3 * sizeof(float));
        memcpy(a.u.color_balance.midtones, v + 3, 3 * sizeof(float));
        memcpy(a.u.color_balance.highlights, v + 6, 3 * sizeof(float));
        a.u.color_balance.preserve_luminosity = below(r, 2) == 0;
        break;
    }
    case 7:
        a.kind = PC_ADJ_BLACK_WHITE;
        for (i = 0; i < 6; i++) {
            a.u.black_white.weights[i] = span(r, -100.0f, 200.0f);
        }
        if (below(r, 2) == 0) {
            a.u.black_white.has_tint = true;
            for (i = 0; i < 3; i++) {
                a.u.black_white.tint[i] = unit(r);
            }
        }
        break;
    case 8:
        a.kind = PC_ADJ_PHOTO_FILTER;
        for (i = 0; i < 3; i++) {
            a.u.photo_filter.color[i] = unit(r);
        }
        a.u.photo_filter.density = span(r, -0.2f, 1.2f);
        a.u.photo_filter.preserve_luminosity = below(r, 2) == 0;
        break;
    case 9:
        a.kind = PC_ADJ_CHANNEL_MIXER;
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 4; j++) {
                a.u.channel_mixer.matrix[i][j] = span(r, -2.0f, 2.0f);
            }
        }
        a.u.channel_mixer.monochrome = below(r, 3) == 0;
        break;
    case 10: {
        uint32_t n = 2 + (uint32_t)below(r, 4);
        size_t len = (size_t)n * n * n * 3;
        float *t = xmalloc(len * sizeof *t);
        uint64_t k;
        for (i = 0; i < len; i++) {
            t[i] = unit(r);
        }
        k = below(r, 6);
        a.kind = PC_ADJ_COLOR_LOOKUP;
        must(pc_str_set(&a.u.color_lookup.name, "lut", 3, NULL));
        if (k != 0) {
            must(pc_blob_new(t, len * sizeof *t, &a.u.color_lookup.lut, NULL));
        }
        a.u.color_lookup.size = k == 1 ? n + 1 : n;
        free(t);
        a.u.color_lookup.tetrahedral = below(r, 2) == 0;
        a.u.color_lookup.dither = below(r, 2) == 0;
        break;
    }
    case 11:
        a.kind = PC_ADJ_INVERT;
        break;
    case 12:
        a.kind = PC_ADJ_POSTERIZE;
        a.u.posterize.levels = (uint32_t)below(r, 260);
        break;
    case 13:
        a.kind = PC_ADJ_THRESHOLD;
        a.u.threshold.level = unit(r);
        break;
    case 14: {
        uint64_t n = below(r, 4);
        a.kind = PC_ADJ_GRADIENT_MAP;
        a.u.gradient_map.stops = n > 0 ? xmalloc((size_t)n * sizeof *a.u.gradient_map.stops) : NULL;
        a.u.gradient_map.nstops = (size_t)n;
        for (i = 0; i < n; i++) {
            a.u.gradient_map.stops[i].pos = unit(r);
            for (j = 0; j < 3; j++) {
                a.u.gradient_map.stops[i].rgb[j] = unit(r);
            }
        }
        a.u.gradient_map.reverse = below(r, 2) == 0;
        a.u.gradient_map.dither = below(r, 2) == 0;
        break;
    }
    case 15:
        a.kind = PC_ADJ_SELECTIVE_COLOR;
        a.u.selective_color.relative = below(r, 2) == 0;
        for (i = 0; i < 9; i++) {
            if (below(r, 2) == 0) {
                for (j = 0; j < 4; j++) {
                    a.u.selective_color.adjustments[i][j] = span(r, -100.0f, 100.0f);
                }
            }
        }
        break;
    default: {
        static const uint8_t raw[3] = {1, 2, 3};
        a.kind = PC_ADJ_UNSUPPORTED;
        must(pc_str_set(&a.u.unsupported.psd_key, "xxxx", 4, NULL));
        must(pc_blob_new(raw, 3, &a.u.unsupported.raw, NULL));
        break;
    }
    }
    return a;
}

static pc_fill gradient_fill(rng *r)
{
    uint64_t n = below(r, 4);
    pc_fill f;
    uint64_t i;
    float u;
    uint64_t k;
    memset(&f, 0, sizeof f);
    f.kind = PC_FILL_GRADIENT;
    f.u.gradient.stops = n > 0 ? xmalloc((size_t)n * sizeof *f.u.gradient.stops) : NULL;
    f.u.gradient.nstops = (size_t)n;
    for (i = 0; i < n; i++) {
        f.u.gradient.stops[i].pos = span(r, -0.1f, 1.1f);
        f.u.gradient.stops[i].color = color(r);
    }
    u = unit(r);
    k = below(r, 6);
    f.u.gradient.angle =
        k == 0 ? 0.0f
               : (k == 1 ? 30.0f
                         : (k == 2 ? 45.0f : (k == 3 ? 90.0f : (k == 4 ? -37.0f : u * 720.0f - 360.0f))));
    u = unit(r);
    k = below(r, 4);
    f.u.gradient.scale = k == 0 ? 1.0f : (k == 1 ? 0.5f : (k == 2 ? 1.7f : u * 3.0f));
    f.u.gradient.style = STYLES[below(r, 5)];
    f.u.gradient.reverse = below(r, 2) == 0;
    if (below(r, 2) == 0) {
        uint64_t m = below(r, 4);
        f.u.gradient.opacity_stops = m > 0 ? xmalloc((size_t)m * sizeof *f.u.gradient.opacity_stops) : NULL;
        f.u.gradient.nopacity_stops = (size_t)m;
        for (i = 0; i < m; i++) {
            f.u.gradient.opacity_stops[i].pos = unit(r);
            f.u.gradient.opacity_stops[i].opacity = unit(r);
        }
    }
    if (below(r, 2) == 0) {
        uint64_t m = below(r, 4);
        f.u.gradient.midpoints = m > 0 ? xmalloc((size_t)m * sizeof *f.u.gradient.midpoints) : NULL;
        f.u.gradient.nmidpoints = (size_t)m;
        for (i = 0; i < m; i++) {
            f.u.gradient.midpoints[i] = unit(r);
        }
    }
    if (below(r, 3) == 0) {
        f.u.gradient.offset[0] = span(r, -0.5f, 0.5f);
        f.u.gradient.offset[1] = span(r, -0.5f, 0.5f);
    }
    f.u.gradient.dither = below(r, 3) == 0;
    f.u.gradient.align = below(r, 2) == 0;
    return f;
}

static pc_fill pattern_fill(rng *r, size_t np)
{
    uint64_t k = below(r, (uint64_t)np + 1);
    pc_fill f;
    float u;
    char name[32];
    memset(&f, 0, sizeof f);
    f.kind = PC_FILL_PATTERN;
    if (k < np) {
        (void)snprintf(name, sizeof name, "p%u", (unsigned)k);
    } else {
        memcpy(name, "nope", 5);
    }
    must(pc_str_set_c(&f.u.pattern.name, name, NULL));
    u = unit(r);
    k = below(r, 4);
    f.u.pattern.scale = k == 0 ? 1.0f : (k == 1 ? 0.5f : (k == 2 ? 2.0f : u * 3.0f));
    u = unit(r);
    k = below(r, 4);
    f.u.pattern.angle = k == 0 ? 0.0f : (k == 1 ? 90.0f : (k == 2 ? 30.0f : u * 360.0f));
    f.u.pattern.link = below(r, 2) == 0;
    if (below(r, 2) == 0) {
        f.u.pattern.phase[0] = span(r, -4.0f, 4.0f);
        f.u.pattern.phase[1] = span(r, -4.0f, 4.0f);
    }
    return f;
}

static pc_path polygon(rng *r, int32_t w, int32_t h)
{
    uint64_t k = 3 + below(r, 2);
    double pts[4][2];
    uint64_t i;
    pc_path p;
    for (i = 0; i < k; i++) {
        int32_t x = range(r, -2, w + 2);
        int32_t y = range(r, -2, h + 2);
        pts[i][0] = (double)x + 0.5;
        pts[i][1] = (double)y + 0.25;
    }
    p = ch_polygon((const double (*)[2])pts, (size_t)k);
    p.inverted = below(r, 5) == 0;
    return p;
}

static pc_layer layer_of(pc_layer_content *c, const char *name)
{
    pc_layer l;
    must(pc_layer_init(&l, name, c, NULL));
    return l;
}

static pc_layer stroked_shape(rng *r, int32_t w, int32_t h)
{
    static const float widths[4] = {1.0f, 3.0f, 0.25f, 6.0f};
    static const pc_stroke_align aligns[3] = {PC_STROKE_ALIGN_INSIDE, PC_STROKE_ALIGN_CENTER,
                                              PC_STROKE_ALIGN_OUTSIDE};
    pc_layer_content c;
    pc_shape_layer *sh = &c.u.shape;
    uint64_t k;
    pc_color col;
    memset(&c, 0, sizeof c);
    c.kind = PC_LAYER_SHAPE;
    sh->path = polygon(r, w, h);
    k = below(r, 4);
    if (k == 1) {
        sh->has_fill = true;
        sh->fill = gradient_fill(r);
    } else if (k != 0) {
        sh->has_fill = true;
        sh->fill = pc_fill_solid(color(r));
    }
    k = below(r, 4);
    col = color(r);
    sh->has_stroke = true;
    pc_shape_stroke_default(&sh->stroke);
    sh->stroke.width = widths[k];
    sh->stroke.paint = pc_fill_solid(col);
    sh->stroke.opacity = below(r, 3) == 0 ? unit(r) : 1.0f;
    sh->stroke.align = aligns[below(r, 3)];
    if (below(r, 2) == 0) {
        pc_surface_init(&sh->cache, PC_PIXEL_FORMAT_RGBA8);
        paint(r, &sh->cache, w, h);
        sh->has_cache = true;
    }
    return layer_of(&c, "stroked");
}

static pc_layer gen_layer(rng *r, pc_pixel_format doc_fmt, int32_t w, int32_t h, unsigned level, size_t np);

static pc_layer *gen_layers(rng *r, pc_pixel_format doc_fmt, int32_t w, int32_t h, unsigned level, uint64_t n,
                            size_t np)
{
    pc_layer *v = xmalloc((size_t)n * sizeof *v);
    uint64_t i;
    for (i = 0; i < n; i++) {
        v[i] = gen_layer(r, doc_fmt, w, h, level, np);
        if (i > 0 && below(r, 4) == 0) {
            v[i].clipped = true;
        }
    }
    return v;
}

static pc_layer gen_layer(rng *r, pc_pixel_format doc_fmt, int32_t w, int32_t h, unsigned level, size_t np)
{
    uint64_t kind = below(r, 12);
    pc_layer l;
    pc_layer_content c;
    memset(&c, 0, sizeof c);
    if (kind == 4 || kind == 5) {
        c.kind = PC_LAYER_ADJUSTMENT;
        c.u.adjustment = adjustment(r);
        l = layer_of(&c, "adj");
    } else if (kind == 6 && level < 3) {
        uint64_t n = 1 + below(r, 3);
        pc_layer *kids = gen_layers(r, doc_fmt, w, h, level + 1, n, np);
        must(pc_layer_group(&l, "g", kids, (size_t)n, NULL));
        if (below(r, 2) == 0) {
            l.blend = PC_BLEND_NORMAL;
        }
    } else if (kind == 6 || kind == 7) {
        c.kind = PC_LAYER_FILL;
        c.u.fill = gradient_fill(r);
        l = layer_of(&c, "gradient");
    } else if (kind == 8) {
        c.kind = PC_LAYER_FILL;
        c.u.fill = pattern_fill(r, np);
        l = layer_of(&c, "pattern");
    } else if (kind == 9) {
        c.kind = PC_LAYER_FILL;
        c.u.fill = pc_fill_solid(color(r));
        l = layer_of(&c, "fill");
    } else if (kind == 10) {
        l = stroked_shape(r, w, h);
    } else if (kind == 11) {
        c.kind = PC_LAYER_TEXT;
        pc_text_layer_default(&c.u.text);
        pc_surface_init(&c.u.text.cache, PC_PIXEL_FORMAT_RGBA8);
        paint(r, &c.u.text.cache, w, h);
        c.u.text.has_cache = true;
        l = layer_of(&c, "type");
    } else {
        pc_pixel_format fmt = layer_format(r, doc_fmt);
        l = ch_raster("px", fmt);
        paint(r, pc_layer_surface_mut(&l), w, h);
    }
    if (below(r, 8) == 0) {
        l.visible = false;
    }
    if (below(r, 2) == 0) {
        l.blend = PC_BLEND_LAYER_MODES[below(r, 27)];
    }
    l.opacity = opacity(r);
    l.fill_opacity = opacity(r);
    if (below(r, 4) == 0) {
        static const float feathers[3] = {0.0f, 0.0f, 2.5f};
        int32_t x0;
        int32_t y0;
        int32_t x1;
        int32_t y1;
        float v;
        if (below(r, 2) == 0) {
            pc_layer_mask_reveal_all(&l.mask);
        } else {
            pc_layer_mask_hide_all(&l.mask);
        }
        l.has_mask = true;
        x0 = range(r, -2, w);
        y0 = range(r, -2, h);
        v = unit(r);
        x1 = x0 + range(r, 1, w);
        y1 = y0 + range(r, 1, h);
        CHECK(pc_surface_fill_rect(&l.mask.surface, pc_rect_make(x0, y0, x1, y1), &v, 1, NULL) == PC_OK);
        l.mask.feather = feathers[below(r, 3)];
        l.mask.enabled = below(r, 6) != 0;
    }
    if (below(r, 6) == 0) {
        pc_path p = polygon(r, w, h);
        pc_vector_mask_init(&l.vector_mask, &p);
        l.has_vector_mask = true;
        l.vector_mask.enabled = below(r, 6) != 0;
    }
    if (below(r, 8) == 0) {
        l.excluded_channels = (uint32_t)below(r, 8);
    }
    if (below(r, 8) == 0) {
        size_t i = (size_t)below(r, 4);
        pc_blend_range a;
        pc_blend_range pair[2];
        bool under;
        a.black[0] = (uint8_t)below(r, 200);
        a.black[1] = (uint8_t)(a.black[0] + (uint8_t)below(r, 40));
        a.white[0] = (uint8_t)(120 + (uint8_t)below(r, 100));
        a.white[1] = (uint8_t)(a.white[0] + (uint8_t)below(r, 36));
        under = below(r, 2) == 0;
        pair[0] = under ? PC_BLEND_RANGE_FULL : a;
        pair[1] = under ? a : PC_BLEND_RANGE_FULL;
        CHECK(pc_blend_if_set(&l.blend_if, i, pair, NULL) == PC_OK);
    }
    if (below(r, 5) == 0) {
        l.advanced.knockout = below(r, 2) == 0 ? PC_KNOCKOUT_SHALLOW : PC_KNOCKOUT_DEEP;
    }
    if (below(r, 5) == 0) {
        l.advanced.blend_clipped = false;
    }
    if (below(r, 6) == 0) {
        l.advanced.transparency_shapes = false;
    }
    if (below(r, 6) == 0) {
        l.advanced.blend_interior = true;
    }
    if (below(r, 8) == 0) {
        l.advanced.layer_mask_hides_effects = true;
    }
    if (below(r, 8) == 0) {
        l.advanced.vector_mask_hides_effects = true;
    }
    return l;
}

static pc_pixel_format pattern_format(uint64_t k)
{
    if (k == 0) {
        return PC_PIXEL_FORMAT_RGBA8;
    }
    if (k == 1) {
        return PC_PIXEL_FORMAT_RGBA16;
    }
    if (k == 2) {
        return PC_PIXEL_FORMAT_RGBA32F;
    }
    return pf(k == 3 ? PC_COLOR_MODE_GRAYSCALE : PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true);
}

static pc_document gen_doc(uint64_t seed)
{
    rng r = {(seed * 0x9e3779b97f4a7c15ull) | 1};
    pc_color_mode mode = MODES[below(&r, 6)];
    pc_sample_type depth = DEPTHS[below(&r, 3)];
    uint32_t w = 9 + (uint32_t)below(&r, 30);
    uint32_t h = 7 + (uint32_t)below(&r, 24);
    pc_document doc = ch_doc(w, h, mode, depth);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    size_t np = (size_t)below(&r, 3);
    size_t i;
    uint64_t n;
    pc_layer *layers;
    size_t nl = 0;
    pc_layer *rest;
    doc.patterns = np > 0 ? xmalloc(np * sizeof *doc.patterns) : NULL;
    for (i = 0; i < np; i++) {
        uint32_t pw = 1 + (uint32_t)below(&r, 5);
        uint32_t ph = 1 + (uint32_t)below(&r, 5);
        pc_surface s;
        char name[32];
        pc_surface_init(&s, pattern_format(below(&r, 5)));
        paint(&r, &s, (int32_t)pw, (int32_t)ph);
        (void)snprintf(name, sizeof name, "p%zu", i);
        must(pc_pattern_new(name, strlen(name), &s, pw, ph, &doc.patterns[i], NULL));
        doc.npatterns = i + 1;
    }
    layers = xmalloc(6 * sizeof *layers);
    if (below(&r, 3) == 0) {
        float px[PC_RASTER_MAX_CHANNELS];
        size_t c;
        layers[nl] = ch_raster("Background", fmt);
        c = pixel(&r, fmt, px);
        CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&layers[nl]),
                                   pc_rect_make(0, 0, (int32_t)w, (int32_t)h), px, c, NULL) == PC_OK);
        layers[nl].locks.transparency = true;
        layers[nl].visible = below(&r, 6) != 0;
        nl++;
    }
    n = 1 + below(&r, 5);
    rest = gen_layers(&r, fmt, (int32_t)w, (int32_t)h, 0, n, np);
    memcpy(layers + nl, rest, (size_t)n * sizeof *layers);
    free(rest);
    doc.layers = layers;
    doc.nlayers = nl + (size_t)n;
    return doc;
}

#endif
