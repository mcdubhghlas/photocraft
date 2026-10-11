/* Bit-exact parity with the Rust reference: tests/golden/compose-core-vectors.txt was written by
 * `rustref compose-core-vectors` (tools/rustref/src/compose_core_vectors.rs). The same seeded
 * synthetic documents are built here from the same xorshift stream (every colour mode and depth,
 * mixed layer formats, all blend modes, opacity / fill incl. out-of-range values, nested and
 * pass-through groups, clipping groups, pixel and vector masks with density / feather / disabled,
 * solid fills, type layers, artboards, channel restrictions, Blend If, spot channels, selections)
 * and every render and query is printed in the same form; every line must match. The sweeps run
 * once more with PHOTOCRAFT_THREADS-independent checks: renders repeated must hash the same. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"

#define GOLDEN "tests/golden/compose-core-vectors.txt"
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

/* NaN sign and payload depend on operand order inside an expression (LLVM may commute an
 * addition), not on the formula, so every NaN hashes as the canonical quiet NaN. */
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

static void hi32(uint64_t *h, int32_t v)
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
    hi32(&h, b->rect.x0);
    hi32(&h, b->rect.y0);
    hi32(&h, b->rect.x1);
    hi32(&h, b->rect.y1);
    return h;
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

/* ---- the generator (mirrors compose_core_vectors.rs call for call) ---- */

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

static const pc_color_mode MODES[6] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE,    PC_COLOR_MODE_CMYK,
                                       PC_COLOR_MODE_LAB, PC_COLOR_MODE_MULTICHANNEL, PC_COLOR_MODE_DUOTONE};
static const pc_sample_type DEPTHS[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};

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

static pc_pixel_format layer_format(rng *r, pc_pixel_format doc_fmt)
{
    uint64_t k = below(r, 8);
    pc_pixel_format f;
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
        f.mode = PC_COLOR_MODE_GRAYSCALE;
        f.sample = DEPTHS[below(r, 3)];
        f.alpha = true;
        return f;
    }
    f.mode = below(r, 2) == 0 ? PC_COLOR_MODE_CMYK : PC_COLOR_MODE_LAB;
    f.sample = DEPTHS[below(r, 3)];
    f.alpha = true;
    return f;
}

static size_t pixel(rng *r, pc_pixel_format fmt, float px[PC_RASTER_MAX_CHANNELS])
{
    size_t n = pc_pixel_format_channels(fmt);
    bool hdr = fmt.sample == PC_SAMPLE_TYPE_F32 && below(r, 4) == 0;
    size_t i;
    for (i = 0; i < n; i++) {
        px[i] = hdr ? unit(r) * 2.0f : unit(r);
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

static pc_layer gen_layer(rng *r, pc_pixel_format doc_fmt, int32_t w, int32_t h, unsigned level);

static pc_layer *gen_layers(rng *r, pc_pixel_format doc_fmt, int32_t w, int32_t h, unsigned level, uint64_t n)
{
    pc_layer *v = calloc((size_t)n, sizeof *v);
    uint64_t i;
    if (v == NULL) {
        abort();
    }
    for (i = 0; i < n; i++) {
        v[i] = gen_layer(r, doc_fmt, w, h, level);
        if (i > 0 && below(r, 4) == 0) {
            v[i].clipped = true;
        }
    }
    return v;
}

static pc_blend_range blend_range(rng *r)
{
    pc_blend_range x;
    uint8_t b0 = (uint8_t)below(r, 200);
    uint8_t b1 = (uint8_t)(b0 + (uint8_t)below(r, 40));
    uint8_t w0 = (uint8_t)(120 + (uint8_t)below(r, 100));
    uint8_t w1 = (uint8_t)(w0 + (uint8_t)below(r, 36));
    x.black[0] = b0;
    x.black[1] = b1;
    x.white[0] = w0;
    x.white[1] = w1;
    return x;
}

static pc_layer gen_layer(rng *r, pc_pixel_format doc_fmt, int32_t w, int32_t h, unsigned level)
{
    uint64_t kind = below(r, 10);
    pc_layer l;
    pc_layer_content c;
    if ((kind == 6 || kind == 7) && level < 3) {
        uint64_t n = 1 + below(r, 3);
        pc_layer *kids = gen_layers(r, doc_fmt, w, h, level + 1, n);
        if (pc_layer_group(&l, "g", kids, (size_t)n, NULL) != PC_OK) {
            abort();
        }
        if (below(r, 2) == 0) {
            l.blend = PC_BLEND_NORMAL;
        }
        if (below(r, 5) == 0) {
            pc_group *g = &l.content.u.group;
            int32_t x0 = range(r, -2, w);
            int32_t y0 = range(r, -2, h);
            int32_t x1 = x0 + range(r, 1, w + 2);
            int32_t y1 = y0 + range(r, 1, h + 2);
            uint64_t bk;
            pc_artboard_init(&g->artboard, pc_rect_make(x0, y0, x1, y1));
            g->has_artboard = true;
            bk = below(r, 4);
            if (bk == 0) {
                g->artboard.background.kind = PC_ARTBOARD_WHITE;
            } else if (bk == 1) {
                g->artboard.background.kind = PC_ARTBOARD_BLACK;
            } else if (bk == 2) {
                g->artboard.background.kind = PC_ARTBOARD_TRANSPARENT;
            } else {
                float cr = unit(r);
                float cg = unit(r);
                float cb = unit(r);
                float ca = unit(r);
                g->artboard.background.kind = PC_ARTBOARD_CUSTOM;
                g->artboard.background.custom = pc_color_rgba(cr, cg, cb, ca);
            }
        }
    } else if (kind == 8) {
        float cr = unit(r);
        float cg = unit(r);
        float cb = unit(r);
        float ca = below(r, 2) == 0 ? 1.0f : unit(r);
        c.kind = PC_LAYER_FILL;
        c.u.fill = pc_fill_solid(pc_color_rgba(cr, cg, cb, ca));
        if (pc_layer_init(&l, "fill", &c, NULL) != PC_OK) {
            abort();
        }
    } else if (kind == 9 && below(r, 2) != 0) {
        /* A filled shape without a stroke: its fill path shapes layer_shape (effect_outline). */
        uint64_t k = 3 + below(r, 2);
        double pts[4][2];
        uint64_t i;
        pc_surface s;
        float cr;
        float cg;
        float cb;
        float ca;
        memset(&c, 0, sizeof c);
        c.kind = PC_LAYER_SHAPE;
        for (i = 0; i < k; i++) {
            int32_t x = range(r, -2, w + 2);
            int32_t y = range(r, -2, h + 2);
            pts[i][0] = (double)x + 0.5;
            pts[i][1] = (double)y + 0.25;
        }
        c.u.shape.path = ch_polygon((const double(*)[2])pts, (size_t)k);
        c.u.shape.path.inverted = below(r, 5) == 0;
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        paint(r, &s, w, h);
        cr = unit(r);
        cg = unit(r);
        cb = unit(r);
        ca = unit(r);
        if (below(r, 4) != 0) {
            c.u.shape.has_fill = true;
            c.u.shape.fill = pc_fill_solid(pc_color_rgba(cr, cg, cb, ca));
        }
        c.u.shape.has_cache = true;
        c.u.shape.cache = s;
        if (pc_layer_init(&l, "shape", &c, NULL) != PC_OK) {
            abort();
        }
    } else if (kind == 9) {
        pc_surface s;
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        paint(r, &s, w, h);
        c.kind = PC_LAYER_TEXT;
        pc_text_layer_default(&c.u.text);
        c.u.text.has_cache = true;
        c.u.text.cache = s;
        if (pc_layer_init(&l, "type", &c, NULL) != PC_OK) {
            abort();
        }
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
        pc_layer_mask *m = &l.mask;
        int32_t x0;
        int32_t y0;
        int32_t x1;
        int32_t y1;
        float v;
        uint64_t k;
        const float dens[3] = {1.0f, 1.0f, 0.5f};
        const float feathers[4] = {0.0f, 0.0f, 1.5f, 4.0f};
        if (below(r, 2) == 0) {
            pc_layer_mask_reveal_all(m);
        } else {
            pc_layer_mask_hide_all(m);
        }
        l.has_mask = true;
        x0 = range(r, -2, w);
        y0 = range(r, -2, h);
        v = unit(r);
        x1 = x0 + range(r, 1, w);
        y1 = y0 + range(r, 1, h);
        CHECK(pc_surface_fill_rect(&m->surface, pc_rect_make(x0, y0, x1, y1), &v, 1, NULL) == PC_OK);
        /* [1.0, 1.0, 0.5, r.unit()][r.below(4)]: the array (and its unit()) is built first. */
        {
            float u = unit(r);
            k = below(r, 4);
            m->density = k < 3 ? dens[k] : u;
        }
        m->feather = feathers[below(r, 4)];
        m->enabled = below(r, 5) != 0;
    }
    if (below(r, 5) == 0) {
        uint64_t k = 3 + below(r, 3);
        double (*pts)[2] = calloc((size_t)k, sizeof *pts);
        uint64_t i;
        pc_path p;
        const float dens[2] = {1.0f, 0.6f};
        const float feathers[3] = {0.0f, 0.0f, 2.0f};
        if (pts == NULL) {
            abort();
        }
        for (i = 0; i < k; i++) {
            int32_t x = range(r, -3, w + 3);
            uint64_t q = below(r, 4);
            int32_t y = range(r, -3, h + 3);
            pts[i][0] = (double)x + 0.25 * (double)q;
            pts[i][1] = (double)y + 0.5;
        }
        p = ch_polygon((const double (*)[2])pts, (size_t)k);
        free(pts);
        p.inverted = below(r, 4) == 0;
        pc_vector_mask_init(&l.vector_mask, &p);
        l.has_vector_mask = true;
        l.vector_mask.density = dens[below(r, 2)];
        l.vector_mask.feather = feathers[below(r, 3)];
        l.vector_mask.enabled = below(r, 5) != 0;
    }
    if (below(r, 6) == 0) {
        l.excluded_channels = (uint32_t)below(r, 8);
    }
    if (below(r, 6) == 0) {
        size_t i = (size_t)below(r, 4);
        pc_blend_range pair[2];
        pair[0] = below(r, 2) == 0 ? blend_range(r) : PC_BLEND_RANGE_FULL;
        pair[1] = below(r, 2) == 0 ? blend_range(r) : PC_BLEND_RANGE_FULL;
        CHECK(pc_blend_if_set(&l.blend_if, i, pair, NULL) == PC_OK);
    }
    if (below(r, 6) == 0) {
        l.advanced.transparency_shapes = false;
        l.advanced.blend_interior = true;
    }
    return l;
}

static pc_document gen_doc(uint64_t seed)
{
    rng r = {(seed * 0x9e3779b97f4a7c15ull) | 1};
    pc_color_mode mode = MODES[below(&r, 6)];
    pc_sample_type depth = DEPTHS[below(&r, 3)];
    uint32_t w = 9 + (uint32_t)below(&r, 40);
    uint32_t h = 7 + (uint32_t)below(&r, 30);
    pc_document doc = ch_doc(w, h, mode, depth);
    pc_pixel_format fmt = pc_document_pixel_format(&doc);
    uint64_t n = 1 + below(&r, 5);
    uint64_t k;
    uint64_t i;
    doc.layers = gen_layers(&r, fmt, (int32_t)w, (int32_t)h, 0, n);
    doc.nlayers = (size_t)n;
    k = below(&r, 3);
    for (i = 0; i < k; i++) {
        pc_surface s;
        pc_pixel_format gf = {PC_COLOR_MODE_GRAYSCALE, depth, false};
        pc_alpha_channel *chs = realloc(doc.channels, (doc.nchannels + 1) * sizeof *chs);
        pc_alpha_channel *ch;
        if (chs == NULL) {
            abort();
        }
        doc.channels = chs;
        ch = &chs[doc.nchannels];
        pc_surface_init(&s, gf);
        paint(&r, &s, (int32_t)w, (int32_t)h);
        if (pc_alpha_channel_init(ch, "ink", &s, NULL) != PC_OK) {
            abort();
        }
        doc.nchannels++;
        if (below(&r, 4) != 0) {
            float cr = unit(&r);
            float cg = unit(&r);
            float cb = unit(&r);
            /* [0.0, 1.0, r.unit()][r.below(3)] */
            float u = unit(&r);
            uint64_t j = below(&r, 3);
            ch->has_spot = true;
            ch->spot_color = pc_color_rgb(cr, cg, cb);
            ch->spot_solidity = j == 0 ? 0.0f : (j == 1 ? 1.0f : u);
        }
    }
    if (below(&r, 4) == 0) {
        pc_surface_init(&doc.selection, PC_PIXEL_FORMAT_GRAY8);
        paint(&r, &doc.selection, (int32_t)w, (int32_t)h);
        doc.has_selection = true;
    }
    return doc;
}

/* ---- renders ---- */

static uint64_t must(pc_status st, const pc_compose_buffer *b, const char *what, uint64_t seed)
{
    if (st != PC_OK) {
        PC_T_FAIL("seed %llu: %s failed", (unsigned long long)seed, what);
        return 0;
    }
    return hbuf(b);
}

static void doc_line(out *o, uint64_t seed)
{
    pc_document doc = gen_doc(seed);
    rng r = {seed ^ 0x5bd1e995u};
    pc_rect b = pc_document_bounds(&doc);
    uint32_t w = pc_rect_width(b);
    uint32_t h = pc_rect_height(b);
    pc_compose_buffer buf;
    pc_status st;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    uint32_t rw;
    uint32_t rh;
    pc_rect dmg;
    pc_rgba8_image t;
    uint64_t ht = H0;
    float bg[3];
    bool has_bg;
    pc_pixel_format fmts[3];
    pc_pixel_format fmt;
    pc_surface s;
    uint64_t hs = H0;
    pc_document p;
    uint64_t hl = H0;
    size_t i;
    put(o, "doc\t%llu", (unsigned long long)seed);
    st = pc_compose_flatten(&doc, &buf, NULL);
    put(o, "\tflat=%016llx", (unsigned long long)must(st, &buf, "flatten", seed));
    if (st == PC_OK) {
        pc_compose_buffer again;
        /* Determinism under any scheduling: a second render is identical. */
        CHECK(pc_compose_flatten(&doc, &again, NULL) == PC_OK && pc_compose_buffer_eq(&buf, &again));
        pc_compose_buffer_fini(&again);
        pc_compose_buffer_fini(&buf);
    }
    st = pc_compose_render_tiled(&doc, b, 7, NULL, &buf, NULL);
    put(o, "\ttiled=%016llx", (unsigned long long)must(st, &buf, "tiled", seed));
    pc_compose_buffer_fini(&buf);
    x0 = range(&r, -6, (int32_t)w);
    y0 = range(&r, -6, (int32_t)h);
    x1 = x0 + range(&r, 1, (int32_t)w + 8);
    y1 = y0 + range(&r, 1, (int32_t)h + 8);
    st = pc_compose_render_tiled(&doc, pc_rect_make(x0, y0, x1, y1), 5, NULL, &buf, NULL);
    put(o, "\tsub=%016llx", (unsigned long long)must(st, &buf, "sub", seed));
    pc_compose_buffer_fini(&buf);
    rw = 1 + (uint32_t)below(&r, w);
    rh = 1 + (uint32_t)below(&r, h);
    st = pc_compose_render_reduced(&doc, rw, rh, &buf, NULL);
    put(o, "\treduced=%016llx", (unsigned long long)must(st, &buf, "reduced", seed));
    pc_compose_buffer_fini(&buf);
    x0 = range(&r, -3, (int32_t)w);
    y0 = range(&r, -3, (int32_t)h);
    x1 = x0 + range(&r, 1, 9);
    y1 = y0 + range(&r, 1, 9);
    dmg = pc_rect_make(x0, y0, x1, y1);
    st = pc_compose_render_reduced_damage(&doc, rw, rh, dmg, &buf, NULL);
    put(o, "\tdamage=%016llx", (unsigned long long)must(st, &buf, "damage", seed));
    pc_compose_buffer_fini(&buf);
    st = pc_compose_render_reduced_rect(&doc, pc_rect_make(-5, -4, (int32_t)w + 6, (int32_t)h + 3),
                                        (w + 11) / 3, (h + 7) / 2, &buf, NULL);
    put(o, "\tarea=%016llx", (unsigned long long)must(st, &buf, "area", seed));
    pc_compose_buffer_fini(&buf);
    st = pc_compose_thumbnail(&doc, 1 + (uint32_t)below(&r, 20), &t, NULL);
    if (st == PC_OK) {
        uint8_t le[4];
        int k;
        for (k = 0; k < 4; k++) {
            le[k] = (uint8_t)(t.width >> (8 * k));
        }
        fnv(&ht, le, 4);
        for (k = 0; k < 4; k++) {
            le[k] = (uint8_t)(t.height >> (8 * k));
        }
        fnv(&ht, le, 4);
        fnv(&ht, t.pixels, (size_t)t.width * t.height * 4);
        pc_rgba8_image_fini(&t);
    } else {
        PC_T_FAIL("seed %llu: thumbnail failed", (unsigned long long)seed);
    }
    put(o, "\tthumb=%016llx", (unsigned long long)ht);
    has_bg = below(&r, 2) == 0;
    if (has_bg) {
        bg[0] = unit(&r);
        bg[1] = unit(&r);
        bg[2] = unit(&r);
    }
    fmts[0] = pc_document_pixel_format(&doc);
    fmts[1] = PC_PIXEL_FORMAT_RGBA8;
    fmts[2] = PC_PIXEL_FORMAT_RGBA32F;
    fmt = fmts[below(&r, 3)];
    st = pc_compose_flatten_to_surface(&doc, fmt, has_bg ? bg : NULL, &s, NULL);
    if (st == PC_OK) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        CHECK(pc_surface_to_interleaved(&s, b, &bytes, &len, NULL) == PC_OK);
        fnv(&hs, bytes, len);
        free(bytes);
        hu64(&hs, pc_surface_tile_count(&s));
        pc_surface_fini(&s);
    } else {
        PC_T_FAIL("seed %llu: flatten_to_surface failed", (unsigned long long)seed);
    }
    put(o, "\tsurface=%016llx", (unsigned long long)hs);
    CHECK(pc_compose_proxy_document(&doc, 2, &p, NULL) == PC_OK);
    st = pc_compose_flatten(&p, &buf, NULL);
    put(o, "\tproxy=%016llx\tfaithful=%d", (unsigned long long)must(st, &buf, "proxy", seed),
        pc_compose_proxy_faithful(&doc) ? 1 : 0);
    pc_compose_buffer_fini(&buf);
    pc_document_fini(&p);
    for (i = 0; i < doc.nlayers; i++) {
        st = pc_compose_render_layer(&doc.layers[i], b, &buf, NULL);
        hu64(&hl, must(st, &buf, "render_layer", seed));
        pc_compose_buffer_fini(&buf);
    }
    put(o, "\tlayers=%016llx", (unsigned long long)hl);
    if (doc.mode == PC_COLOR_MODE_MULTICHANNEL) {
        st = pc_compose_inks(&doc, b, &buf, NULL);
        put(o, "\tinks=%016llx", (unsigned long long)must(st, &buf, "inks", seed));
        pc_compose_buffer_fini(&buf);
    }
    put(o, "\n");
    for (i = 0; i < doc.nlayers; i++) {
        const pc_layer *l = &doc.layers[i];
        pc_rect q;
        float cw[3];
        bool found = false;
        float *shape = NULL;
        size_t n = 0;
        uint64_t hsh = H0;
        size_t k;
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
        put(o, "\tto=%d\tocc=%d", pc_compose_transparent_outside(l) ? 1 : 0,
            pc_compose_occludes_below(l, doc.mode) ? 1 : 0);
        if (pc_compose_channel_weights(l, doc.mode, cw)) {
            put(o, "\tcw=[%s, %s, %s]", cw[0] == 1.0f ? "1.0" : "0.0", cw[1] == 1.0f ? "1.0" : "0.0",
                cw[2] == 1.0f ? "1.0" : "0.0");
        } else {
            put(o, "\tcw=none");
        }
        put(o, "\tbi=%d\tframe=", pc_compose_blend_if_active(l, doc.mode) ? 1 : 0);
        rs(o, pc_compose_fill_frame(l, b));
        {
            float g = pc_compose_text_gamma(l);
            uint32_t u;
            memcpy(&u, &g, 4);
            put(o, "\tgamma=%08x", u);
        }
        put(o, "\tfeather=%d", pc_compose_has_feather(l) ? 1 : 0);
        CHECK(pc_compose_combined_mask(l, b, &found, &s, NULL) == PC_OK);
        if (found) {
            size_t np = (size_t)w * h;
            float *v = malloc((np > 0 ? np : 1) * sizeof *v);
            uint64_t hm = H0;
            CHECK(v != NULL && pc_surface_read_region_into(&s, b, v, np, NULL) == PC_OK);
            for (k = 0; v != NULL && k < np; k++) {
                hf(&hm, v[k]);
            }
            free(v);
            pc_surface_fini(&s);
            put(o, "\tmask=%016llx", (unsigned long long)hm);
        }
        CHECK(pc_compose_layer_shape(&doc, l, b, &shape, &n, NULL) == PC_OK);
        for (k = 0; k < n; k++) {
            hf(&hsh, shape[k]);
        }
        free(shape);
        put(o, "\tshape=%016llx\n", (unsigned long long)hsh);
    }
    pc_document_fini(&doc);
}

/* ---- sweeps ---- */

static void blend_sweeps(out *o)
{
    const float vals[14] = {0.0f,  -0.0f,        1e-5f, 0.2f, 0.25f, 0.5f, 0.6f,
                            0.75f, 1.0f - 1e-5f, 1.0f,  1.5f, -0.1f, NAN,  INFINITY};
    const float alphas[4] = {0.0f, 0.3f, 1.0f, 0.7f};
    const float ops[3] = {1.0f, 0.5f, 0.0f};
    const float gammas[2] = {1.45f, 2.2f};
    const char *names[3] = {"comp", "comp_lab", "comp_hdr"};
    uint64_t hr = H0;
    uint64_t hr_hdr = H0;
    uint64_t he = H0;
    size_t m;
    int a;
    int b;
    int f;
    for (m = 0; m < PC_BLEND_LAYER_MODE_COUNT; m++) {
        for (a = 0; a < 14; a++) {
            for (b = 0; b < 14; b++) {
                const float cb[3] = {vals[a], vals[b], 0.4f};
                const float cs[3] = {vals[b], 0.6f, vals[a]};
                float v[3];
                int k;
                pc_psblend_rgb(PC_BLEND_LAYER_MODES[m], cb, cs, v);
                for (k = 0; k < 3; k++) {
                    hf(&hr, v[k]);
                }
                pc_psblend_set_hdr(true);
                pc_psblend_rgb(PC_BLEND_LAYER_MODES[m], cb, cs, v);
                pc_psblend_set_hdr(false);
                for (k = 0; k < 3; k++) {
                    hf(&hr_hdr, v[k]);
                }
            }
        }
    }
    put(o, "blend\trgb\t%016llx\n", (unsigned long long)hr);
    put(o, "blend\trgb_hdr\t%016llx\n", (unsigned long long)hr_hdr);
    for (f = 0; f < 3; f++) {
        uint64_t hc = H0;
        uint64_t hg = H0;
        pc_psblend_set_lab_mix(f == 1);
        pc_psblend_set_hdr(f == 2);
        for (m = 0; m < PC_BLEND_LAYER_MODE_COUNT; m++) {
            for (a = 0; a < 14; a++) {
                for (b = 0; b < 14; b++) {
                    int aa;
                    int ab;
                    int op;
                    for (aa = 0; aa < 4; aa++) {
                        for (ab = 0; ab < 4; ab++) {
                            for (op = 0; op < 3; op++) {
                                const float bd[4] = {vals[a], 0.3f, vals[b], alphas[ab]};
                                const float sr[4] = {vals[b], vals[a], 0.8f, alphas[aa]};
                                float v[4];
                                int k;
                                int g;
                                pc_psblend_composite(PC_BLEND_LAYER_MODES[m], bd, sr, ops[op], v);
                                for (k = 0; k < 4; k++) {
                                    hf(&hc, v[k]);
                                }
                                for (g = 0; g < 2; g++) {
                                    pc_psblend_composite_gamma(PC_BLEND_LAYER_MODES[m], bd, sr, ops[op],
                                                               gammas[g], v);
                                    for (k = 0; k < 4; k++) {
                                        hf(&hg, v[k]);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        pc_psblend_set_lab_mix(false);
        pc_psblend_set_hdr(false);
        put(o, "blend\t%s\t%016llx\n", names[f], (unsigned long long)hc);
        put(o, "blend\t%s_gamma\t%016llx\n", names[f], (unsigned long long)hg);
    }
    for (a = 0; a < 14; a++) {
        const float gs[3] = {1.0f, 1.45f, 2.2f};
        int g;
        for (g = 0; g < 3; g++) {
            hf(&he, pc_psblend_text_encode(vals[a], gs[g]));
            hf(&he, pc_psblend_text_decode(vals[a], gs[g]));
        }
    }
    put(o, "blend\ttext_codec\t%016llx\n", (unsigned long long)he);
}

static void layout_sweeps(out *o)
{
    const pc_gradient_style styles[5] = {PC_GRADIENT_LINEAR, PC_GRADIENT_RADIAL, PC_GRADIENT_ANGLE,
                                         PC_GRADIENT_REFLECTED, PC_GRADIENT_DIAMOND};
    const float angles[10] = {0.0f, 30.0f, 45.0f, 87.0f, 90.0f, -90.0f, 135.0f, 200.0f, 1e30f, NAN};
    const float scales[5] = {1.0f, 0.5f, 1.5f, 0.0f, INFINITY};
    const pc_rect frames[5] = {{0, 0, 4, 4}, {0, 0, 7, 5}, {-5, 3, 40, 17}, {10, 10, 210, 210}, {3, 3, 3, 9}};
    const float offsets[3][2] = {{0.0f, 0.0f}, {0.25f, -0.1f}, {NAN, 0.0f}};
    const float pts4[4][2] = {{0.5f, 0.5f}, {2.5f, 1.5f}, {13.5f, 8.5f}, {-3.0f, 40.0f}};
    const float pts3[3][2] = {{0.5f, 0.5f}, {3.5f, 2.5f}, {20.5f, 10.5f}};
    const float whs[4][2] = {{1.0f, 1.0f}, {600.0f, 60.0f}, {29.0f, 29.0f}, {0.5f, 3.0f}};
    uint64_t hl = H0;
    uint64_t ht = H0;
    uint64_t hg = H0;
    uint64_t hu = H0;
    int s;
    int a;
    int sc;
    int f;
    int k;
    for (s = 0; s < 5; s++) {
        for (a = 0; a < 10; a++) {
            for (sc = 0; sc < 5; sc++) {
                for (f = 0; f < 5; f++) {
                    int of;
                    for (of = 0; of < 3; of++) {
                        pc_compose_gradient_geom g = pc_compose_gradient_layout(
                            styles[s], angles[a], scales[sc], offsets[of], frames[f]);
                        hf(&hl, g.angle);
                        hf(&hl, g.scale);
                        hf(&hl, g.offset[0]);
                        hf(&hl, g.offset[1]);
                        for (k = 0; k < 4; k++) {
                            int rev;
                            for (rev = 0; rev < 2; rev++) {
                                hf(&hg,
                                   pc_compose_gradient_t(styles[s], angles[a], scales[sc], rev == 1,
                                                         offsets[of], frames[f], pts4[k][0], pts4[k][1]));
                            }
                        }
                    }
                    for (k = 0; k < 3; k++) {
                        hf(&ht, pc_compose_fill_gradient_t(styles[s], angles[a], scales[sc], false, frames[f],
                                                           pts3[k][0], pts3[k][1]));
                    }
                }
            }
        }
    }
    put(o, "layout\tgradient_layout\t%016llx\n", (unsigned long long)hl);
    put(o, "layout\tfill_gradient_t\t%016llx\n", (unsigned long long)ht);
    put(o, "layout\tgradient_t\t%016llx\n", (unsigned long long)hg);
    for (a = 0; a < 10; a++) {
        for (k = 0; k < 4; k++) {
            float ch;
            float len;
            pc_compose_gradient_units(angles[a], whs[k][0], whs[k][1], &ch, &len);
            hf(&hu, ch);
            hf(&hu, len);
        }
    }
    put(o, "layout\tgradient_units\t%016llx\n", (unsigned long long)hu);
}

static void misc(out *o)
{
    const uint32_t ws[8] = {1, 0, 14000, 37, 100000, UINT32_MAX, 5, 5};
    const int32_t reqs[8] = {0, -5, 0, 100, 0, 0, INT32_MAX, 1};
    const float fs[7] = {0.0f, 1.5f, 4.0f, 1e6f, -3.0f, NAN, INFINITY};
    const char *fnames[7] = {"0.0", "1.5", "4.0", "1000000.0", "-3.0", "NaN", "inf"};
    int i;
    for (i = 0; i < 8; i++) {
        put(o, "misc\tband_rows_for(%u,%d)\t%d\n", ws[i], reqs[i], pc_compose_band_rows_for(ws[i], reqs[i]));
    }
    for (i = 0; i < 7; i++) {
        float v = pc_compose_feather_sigma(fs[i]);
        uint32_t u;
        memcpy(&u, &v, 4);
        put(o, "misc\tfeather_sigma(%s)\t%08x\n", fnames[i], u);
    }
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
    blend_sweeps(&o);
    layout_sweeps(&o);
    misc(&o);
    for (seed = 1; seed <= 400; seed++) {
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
            if (bad < 20) {
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
    RUN(test_parity);
    return pc_t_finish();
}
