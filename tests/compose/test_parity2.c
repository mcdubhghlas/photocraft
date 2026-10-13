/* Bit-exact parity with the Rust reference for chunk C2: tests/golden/compose2-vectors.txt was
 * written by `rustref compose2-vectors` (tools/rustref/src/compose2_vectors.rs). Function sweeps
 * of adjust.rs, gradient_fill.rs, pattern.rs and shape_split.rs, and 400 seeded documents (every
 * colour mode and depth) with all 17 adjustment kinds, gradient and pattern fills, stroked shapes,
 * knockout, Blend Clipped Layers as Group off, Transparency Shapes Layer off, masks, clipping and
 * pass-through groups are built from the same xorshift stream (every draw in the same order) and
 * printed in the same form; every line must match. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "compose/adjust.h"
#include "compose/advanced.h"
#include "compose/gradient_fill.h"
#include "compose/pattern.h"
#include "compose/shape_split.h"
#include "gen2.h"

#define GOLDEN "tests/golden/compose2-vectors.txt"
#define H0 0xcbf29ce484222325ull
#define LUT PC_COMPOSE_LUT_SIZE

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

static void hu32(uint64_t *h, uint32_t u)
{
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

static void hf(uint64_t *h, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    hu32(h, isnan(v) ? 0x7fc00000u : u);
}

static void hd(uint64_t *h, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    hu64(h, isnan(v) ? 0x7ff8000000000000ull : u);
}

static uint64_t hpx(float (*px)[4], size_t n)
{
    uint64_t h = H0;
    size_t i;
    int c;
    for (i = 0; i < n; i++) {
        for (c = 0; c < 4; c++) {
            hf(&h, px[i][c]);
        }
    }
    return h;
}

static uint64_t hbuf(const pc_compose_buffer *b)
{
    uint64_t h = hpx(b->px, b->len);
    hu32(&h, (uint32_t)b->rect.x0);
    hu32(&h, (uint32_t)b->rect.y0);
    hu32(&h, (uint32_t)b->rect.x1);
    hu32(&h, (uint32_t)b->rect.y1);
    return h;
}

static uint64_t hsurf(const pc_surface *s, pc_rect r)
{
    uint64_t h = H0;
    uint8_t *bytes = NULL;
    size_t len = 0;
    CHECK(pc_surface_to_interleaved(s, r, &bytes, &len, NULL) == PC_OK);
    fnv(&h, bytes, len);
    free(bytes);
    return h;
}

/* ---- output ---- */

typedef struct out {
    char *s;
    size_t len;
    size_t cap;
} out;

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
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

static void fn_line(out *o, const char *name, uint64_t h)
{
    put(o, "fn\t%s\t%016llx\n", name, (unsigned long long)h);
}

/* ---- renders ---- */

static uint64_t hb_or_fail(pc_status st, pc_compose_buffer *b, const char *what, uint64_t seed,
                           const pc_error *e)
{
    uint64_t h;
    if (st != PC_OK) {
        PC_T_FAIL("seed %llu: %s failed: %s", (unsigned long long)seed, what, e->msg);
        return 0;
    }
    h = hbuf(b);
    pc_compose_buffer_fini(b);
    return h;
}

static void doc_line(out *o, uint64_t seed)
{
    pc_document doc = gen_doc(seed);
    rng r = {seed ^ 0x5bd1e995u};
    pc_rect b = pc_document_bounds(&doc);
    uint32_t w = pc_rect_width(b);
    uint32_t h = pc_rect_height(b);
    pc_compose_buffer buf;
    pc_error e;
    pc_status st;
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
    uint32_t rw;
    uint32_t rh;
    pc_rgba8_image t;
    uint64_t ht = H0;
    pc_pixel_format fmts[3];
    pc_surface s;
    uint64_t hs = H0;
    pc_document p;
    uint64_t hl = H0;
    size_t i;
    memset(&e, 0, sizeof e);
    put(o, "doc\t%llu", (unsigned long long)seed);
    st = pc_compose_flatten(&doc, &buf, &e);
    put(o, "\tflat=%016llx", (unsigned long long)hb_or_fail(st, &buf, "flatten", seed, &e));
    st = pc_compose_render_tiled(&doc, b, 7, NULL, &buf, &e);
    put(o, "\ttiled=%016llx", (unsigned long long)hb_or_fail(st, &buf, "tiled", seed, &e));
    x0 = range(&r, -6, (int32_t)w);
    y0 = range(&r, -6, (int32_t)h);
    x1 = x0 + range(&r, 1, (int32_t)w + 8);
    y1 = y0 + range(&r, 1, (int32_t)h + 8);
    st = pc_compose_render_tiled(&doc, pc_rect_make(x0, y0, x1, y1), 5, NULL, &buf, &e);
    put(o, "\tsub=%016llx", (unsigned long long)hb_or_fail(st, &buf, "sub", seed, &e));
    rw = 1 + (uint32_t)below(&r, w);
    rh = 1 + (uint32_t)below(&r, h);
    st = pc_compose_render_reduced(&doc, rw, rh, &buf, &e);
    put(o, "\treduced=%016llx", (unsigned long long)hb_or_fail(st, &buf, "reduced", seed, &e));
    x0 = range(&r, -3, (int32_t)w);
    y0 = range(&r, -3, (int32_t)h);
    x1 = x0 + range(&r, 1, 9);
    y1 = y0 + range(&r, 1, 9);
    st = pc_compose_render_reduced_damage(&doc, rw, rh, pc_rect_make(x0, y0, x1, y1), &buf, &e);
    put(o, "\tdamage=%016llx", (unsigned long long)hb_or_fail(st, &buf, "damage", seed, &e));
    st = pc_compose_render_reduced_rect(&doc, pc_rect_make(-5, -4, (int32_t)w + 6, (int32_t)h + 3),
                                        (w + 11) / 3, (h + 7) / 2, &buf, &e);
    put(o, "\tarea=%016llx", (unsigned long long)hb_or_fail(st, &buf, "area", seed, &e));
    st = pc_compose_thumbnail(&doc, 1 + (uint32_t)below(&r, 20), &t, &e);
    if (st == PC_OK) {
        hu32(&ht, t.width);
        hu32(&ht, t.height);
        fnv(&ht, t.pixels, (size_t)t.width * t.height * 4);
        pc_rgba8_image_fini(&t);
    } else {
        PC_T_FAIL("seed %llu: thumbnail failed: %s", (unsigned long long)seed, e.msg);
    }
    put(o, "\tthumb=%016llx", (unsigned long long)ht);
    fmts[0] = pc_document_pixel_format(&doc);
    fmts[1] = PC_PIXEL_FORMAT_RGBA8;
    fmts[2] = PC_PIXEL_FORMAT_RGBA32F;
    st = pc_compose_flatten_to_surface(&doc, fmts[below(&r, 3)], NULL, &s, &e);
    if (st == PC_OK) {
        uint8_t *bytes = NULL;
        size_t len = 0;
        CHECK(pc_surface_to_interleaved(&s, b, &bytes, &len, NULL) == PC_OK);
        fnv(&hs, bytes, len);
        free(bytes);
        hu64(&hs, pc_surface_tile_count(&s));
        pc_surface_fini(&s);
    } else {
        PC_T_FAIL("seed %llu: flatten_to_surface failed: %s", (unsigned long long)seed, e.msg);
    }
    put(o, "\tsurface=%016llx", (unsigned long long)hs);
    CHECK(pc_compose_proxy_document(&doc, 2, &p, NULL) == PC_OK);
    st = pc_compose_flatten(&p, &buf, &e);
    put(o, "\tproxy=%016llx", (unsigned long long)hb_or_fail(st, &buf, "proxy", seed, &e));
    pc_document_fini(&p);
    for (i = 0; i < doc.nlayers; i++) {
        st = pc_compose_render_layer(&doc.layers[i], b, &buf, &e);
        hu64(&hl, hb_or_fail(st, &buf, "render_layer", seed, &e));
    }
    put(o, "\tlayers=%016llx\n", (unsigned long long)hl);
    for (i = 0; i < doc.nlayers; i++) {
        const pc_layer *l = &doc.layers[i];
        pc_rect q;
        size_t nclip = 0;
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
        while (i + 1 + nclip < doc.nlayers && doc.layers[i + 1 + nclip].clipped && !l->clipped) {
            nclip++;
        }
        put(o, "\tadv=%d\tshapeless=%d\tbg=%d", pc_compose_advanced_active(l, l + 1, nclip) ? 1 : 0,
            pc_compose_shapeless(l) ? 1 : 0, pc_compose_is_background(l) ? 1 : 0);
        put(o, "\tframe=");
        rs(o, pc_compose_fill_frame(l, b));
        if (l->content.kind == PC_LAYER_FILL) {
            st = pc_compose_render_fill_content(l, &l->content.u.fill, b, doc.patterns, doc.npatterns, &buf,
                                                &e);
            put(o, "\tfill=%016llx", (unsigned long long)hb_or_fail(st, &buf, "fill content", seed, &e));
        }
        CHECK(pc_compose_layer_shape(&doc, l, b, &shape, &n, NULL) == PC_OK);
        for (k = 0; k < n; k++) {
            hf(&hsh, shape[k]);
        }
        free(shape);
        put(o, "\tshape=%016llx", (unsigned long long)hsh);
        if (l->content.kind == PC_LAYER_SHAPE) {
            bool found = false;
            pc_surface fs;
            pc_surface ss;
            CHECK(pc_compose_shape_split(&l->content.u.shape, b, doc.depth, &found, &fs, &ss, NULL) == PC_OK);
            if (found) {
                put(o, "\tsplit=%016llx,%016llx", (unsigned long long)hsurf(&fs, b),
                    (unsigned long long)hsurf(&ss, b));
                pc_surface_fini(&fs);
                pc_surface_fini(&ss);
            } else {
                put(o, "\tsplit=none");
            }
        }
        put(o, "\n");
    }
    pc_document_fini(&doc);
}

/* ---- function sweeps ---- */

static const float VALS[16] = {0.0f, -0.0f, 1e-5f, 0.1f,         0.2f, 0.25f, 0.3f,  0.5f,
                               0.6f, 0.75f, 0.9f,  1.0f - 1e-5f, 1.0f, 1.5f,  -0.1f, NAN};

static pc_compose_buffer test_buffer(void)
{
    static const float alphas[6] = {1.0f, 0.5f, 0.0f, 1.0f, NAN, 1.0f};
    pc_compose_buffer b;
    size_t i;
    must(pc_compose_buffer_transparent(pc_rect_make(-3, 5, 10, 12), &b, NULL));
    for (i = 0; i < 91; i++) {
        b.px[i][0] = VALS[i % 16];
        b.px[i][1] = VALS[(i / 3) % 16];
        b.px[i][2] = VALS[(i * 7 + 3) % 16];
        b.px[i][3] = alphas[i % 6];
    }
    return b;
}

static pc_curve pts(const float (*v)[2], size_t n)
{
    pc_curve c;
    size_t i;
    c.pts = n > 0 ? xmalloc(n * sizeof *c.pts) : NULL;
    c.len = n;
    for (i = 0; i < n; i++) {
        c.pts[i].input = v[i][0];
        c.pts[i].output = v[i][1];
    }
    return c;
}

static void adjust_sweeps(out *o)
{
    const pc_compose_transfer transfers[4] = {{PC_COMPOSE_TRANSFER_SRGB, 0.0f},
                                              {PC_COMPOSE_TRANSFER_GAMMA, 1.732f},
                                              {PC_COMPOSE_TRANSFER_GAMMA, 1.0f},
                                              {PC_COMPOSE_TRANSFER_GAMMA, 2.2f}};
    const bool has_d[4] = {false, true, true, true};
    const pc_sample_type ds[4] = {PC_SAMPLE_TYPE_F32, PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16,
                                  PC_SAMPLE_TYPE_F32};
    uint64_t ha = H0;
    uint64_t hl = H0;
    float *luts = xmalloc(4 * LUT * sizeof *luts);
    uint64_t seed;
    size_t i;
    size_t j;
    size_t k;
    for (seed = 1; seed <= 600; seed++) {
        rng r = {(seed * 0x2545f4914f6cdd1dull) | 1};
        pc_adjustment a = adjustment(&r);
        uint64_t h = H0;
        int t;
        int d;
        for (t = 0; t < 3; t++) {
            for (d = 0; d < 4; d++) {
                pc_compose_buffer b = test_buffer();
                CHECK(pc_compose_adjust_apply_depth(&a, &b, transfers[t], has_d[d], ds[d], NULL) == PC_OK);
                hu64(&h, hpx(b.px, b.len));
                pc_compose_buffer_fini(&b);
            }
        }
        hu64(&ha, h);
        if (seed % 100 == 0) {
            char name[32];
            (void)snprintf(name, sizeof name, "apply_depth_%llu", (unsigned long long)seed);
            fn_line(o, name, ha);
        }
        if (a.kind == PC_ADJ_LEVELS || a.kind == PC_ADJ_CURVES) {
            for (d = 0; d < 4; d++) {
                CHECK(pc_compose_tone_luts_depth(&a, has_d[d], ds[d], luts, NULL) == PC_OK);
                for (i = 0; i < 4 * LUT; i++) {
                    hf(&hl, luts[i]);
                }
            }
        }
        pc_adjustment_fini(&a);
    }
    fn_line(o, "tone_luts_depth", hl);
    {
        static const float c1[1][2] = {{0.5f, 0.5f}};
        static const float c2[2][2] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        static const float c3[3][2] = {{1.0f, 0.2f}, {0.0f, 0.9f}, {0.5f, 0.1f}};
        static const float c4[4][2] = {{0.2f, 0.3f}, {0.2f, 0.9f}, {0.2000001f, 0.4f}, {0.8f, 0.7f}};
        static const float c5[4][2] = {{0.1f, 1.5f}, {0.4f, -0.5f}, {0.6f, 1.2f}, {0.9f, -0.2f}};
        static const float c6[3][2] = {{NAN, 0.5f}, {0.3f, 0.2f}, {0.7f, 0.8f}};
        static const float c7[2][2] = {{0.3f, NAN}, {0.6f, 0.5f}};
        static const float c8[2][2] = {{-0.5f, 0.1f}, {1.5f, 0.9f}};
        const float (*cs[9])[2] = {NULL, c1, c2, c3, c4, c5, c6, c7, c8};
        const size_t ns[9] = {0, 1, 2, 3, 4, 4, 3, 2, 2};
        uint64_t hc = H0;
        for (i = 0; i < 9; i++) {
            pc_curve cv = pts(cs[i], ns[i]);
            CHECK(pc_compose_curve_lut(cv.pts, cv.len, luts, NULL) == PC_OK);
            for (k = 0; k < LUT; k++) {
                hf(&hc, luts[k]);
            }
            pc_curve_fini(&cv);
        }
        fn_line(o, "curve_lut", hc);
    }
    {
        const pc_levels_channel chans[7] = {
            {0.0f, 1.0f, 1.0f, 0.0f, 1.0f},
            {0.2f, 0.8f, 1.0f, 0.0f, 1.0f},
            {44.0f / 255.0f, 214.0f / 255.0f, 1.78f, 0.1f, 0.9f},
            {0.5f, 0.5f, 0.0f, 0.0f, 1.0f},
            {0.6f, 0.3f, 3.0f, 1.0f, 0.0f},
            {0.0f, 1.0f, NAN, 0.0f, 1.0f},
            {15.0f / 255.0f, 230.0f / 255.0f, 1.3f, 0.04f, 0.96f},
        };
        const float qs[7] = {0.0f, 255.0f, 32768.0f, 0.0f, NAN, -3.0f, 1.0f};
        uint64_t hq = H0;
        uint64_t hfl = H0;
        for (i = 0; i < 7; i++) {
            for (j = 0; j < 16; j++) {
                for (k = 0; k < 7; k++) {
                    hf(&hq, pc_compose_levels_q(&chans[i], VALS[j], k != 0, qs[k]));
                }
                hf(&hfl, pc_compose_levels_float(&chans[i], VALS[j]));
            }
        }
        fn_line(o, "levels_q", hq);
        fn_line(o, "levels_float", hfl);
    }
    {
        const float bs[7] = {-150.0f, -50.0f, 0.0f, 37.0f, 150.0f, 400.0f, NAN};
        const float ks[7] = {-50.0f, -25.0f, 0.0f, 50.0f, 100.0f, 128.0f, NAN};
        uint64_t hb = H0;
        uint64_t hk = H0;
        uint64_t hp = H0;
        const uint32_t extra[5] = {UINT32_MAX, 255, 256, 2, 1};
        for (i = 0; i < 7; i++) {
            for (j = 0; j < 16; j++) {
                hf(&hb, pc_compose_modern_brightness(VALS[j], bs[i]));
            }
        }
        for (i = 0; i < 7; i++) {
            for (j = 0; j < 16; j++) {
                hf(&hk, pc_compose_modern_contrast(VALS[j], ks[i]));
            }
        }
        fn_line(o, "modern_brightness", hb);
        fn_line(o, "modern_contrast", hk);
        for (i = 0; i < 300 / 7 + 1 + 5; i++) {
            uint32_t n = i < 43 ? (uint32_t)(i * 7) : extra[i - 43];
            for (j = 0; j < 16; j++) {
                hf(&hp, pc_compose_posterize(VALS[j], n));
            }
        }
        fn_line(o, "posterize", hp);
    }
    free(luts);
}

static void color_sweeps(out *o)
{
    static const float weights[2][6] = {{40, 60, 40, 60, 20, 80}, {100, -50, 0, 200, 30, 70}};
    static const size_t ks[5] = {0, 3, 7, 12, 15};
    static const float hs4[4][3] = {
        {37.0f, 0.5f, 0.0f}, {-200.0f, -1.0f, 0.7f}, {400.0f, 0.1f, -0.5f}, {0.0f, -0.3f, 0.0f}};
    static const bool cz4[4] = {false, false, true, true};
    static const float vs5[5][2] = {{0.5f, 0.0f}, {-0.7f, 0.0f}, {0.0f, -1.0f}, {1.0f, 0.6f}, {-0.3f, -0.4f}};
    const pc_compose_transfer transfers[4] = {{PC_COMPOSE_TRANSFER_SRGB, 0.0f},
                                              {PC_COMPOSE_TRANSFER_GAMMA, 1.732f},
                                              {PC_COMPOSE_TRANSFER_GAMMA, 2.2f},
                                              {PC_COMPOSE_TRANSFER_GAMMA, 1.0f}};
    const float his[2] = {1.0f, INFINITY};
    float sel[9][4];
    uint64_t hh = H0;
    uint64_t hs = H0;
    uint64_t hbw = H0;
    uint64_t hsel = H0;
    uint64_t hvib = H0;
    size_t i;
    size_t j;
    size_t k;
    int m;
    for (i = 0; i < 9; i++) {
        for (j = 0; j < 4; j++) {
            sel[i][j] = fmodf((float)(i * 4 + j) * 37.0f, 200.0f) - 100.0f;
        }
    }
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 16; j++) {
            for (k = 0; k < 5; k++) {
                const float col[3] = {VALS[i], VALS[j], VALS[ks[k]]};
                float v3[3];
                float v9[9];
                float hh_;
                float ss_;
                float ll_;
                pc_compose_rgb_to_hsl(col, &hh_, &ss_, &ll_);
                hf(&hh, hh_);
                hf(&hh, ss_);
                hf(&hh, ll_);
                pc_compose_hsl_to_rgb(col[0], col[1], col[2], v3);
                for (m = 0; m < 3; m++) {
                    hf(&hh, v3[m]);
                }
                for (m = 0; m < 4; m++) {
                    int c;
                    pc_compose_hue_saturation(col, hs4[m][0], hs4[m][1], hs4[m][2], cz4[m], v3);
                    for (c = 0; c < 3; c++) {
                        hf(&hs, v3[c]);
                    }
                }
                for (m = 0; m < 2; m++) {
                    hf(&hbw, pc_compose_black_white_gray(col, weights[m]));
                }
                pc_compose_selective_color_weights(col, v9);
                for (m = 0; m < 9; m++) {
                    hf(&hsel, v9[m]);
                }
                for (m = 0; m < 2; m++) {
                    int c;
                    pc_compose_selective_color(col, m == 1, (const float (*)[4])sel, v3);
                    for (c = 0; c < 3; c++) {
                        hf(&hsel, v3[c]);
                    }
                }
                for (m = 0; m < 5; m++) {
                    int t;
                    int x;
                    int c;
                    for (t = 0; t < 4; t++) {
                        for (x = 0; x < 2; x++) {
                            pc_compose_vibrance_px(col, vs5[m][0], vs5[m][1], transfers[t], his[x], v3);
                            for (c = 0; c < 3; c++) {
                                hf(&hvib, v3[c]);
                            }
                        }
                    }
                    pc_compose_vibrance_lin(col, vs5[m][0], v3);
                    for (c = 0; c < 3; c++) {
                        hf(&hvib, v3[c]);
                    }
                }
            }
        }
    }
    fn_line(o, "hsl", hh);
    fn_line(o, "hue_saturation", hs);
    fn_line(o, "black_white_gray", hbw);
    fn_line(o, "selective_color", hsel);
    fn_line(o, "vibrance", hvib);
    {
        uint64_t hy = H0;
        int32_t x;
        int32_t y;
        for (y = -5; y < 5; y++) {
            for (x = -5; x < 5; x++) {
                hf(&hy, pc_compose_bayer4(x, y));
            }
        }
        fn_line(o, "bayer4", hy);
    }
    {
        static const size_t ns[3] = {2, 3, 5};
        uint64_t hlut = H0;
        rng r = {0x123456789abcdef1ull};
        for (k = 0; k < 3; k++) {
            size_t n = ns[k];
            size_t len = n * n * n * 3;
            float *t = xmalloc(len * sizeof *t);
            for (i = 0; i < len; i++) {
                t[i] = unit(&r);
            }
            for (i = 0; i < 16; i++) {
                for (j = 0; j < 16; j++) {
                    const float col[3] = {VALS[i], VALS[j], VALS[(i + j) % 16]};
                    for (m = 0; m < 2; m++) {
                        float v3[3];
                        int c;
                        pc_compose_lut3d_sample(t, len, n, col, m == 1, v3);
                        for (c = 0; c < 3; c++) {
                            hf(&hlut, v3[c]);
                        }
                    }
                }
            }
            free(t);
        }
        fn_line(o, "lut3d_sample", hlut);
    }
    {
        static const float cols[3][3] = {{1.0f, 0.5f, 0.0f}, {0.0f, 0.3f, 0.9f}, {2.0f, -1.0f, NAN}};
        static const float dens[6] = {0.0f, 0.25f, 1.0f, 1.5f, NAN, INFINITY};
        uint64_t hpf = H0;
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 6; j++) {
                for (m = 0; m < 2; m++) {
                    float mat[3][3];
                    int a;
                    int b;
                    pc_compose_photo_filter_matrix(cols[i], dens[j], m == 1, mat);
                    for (a = 0; a < 3; a++) {
                        for (b = 0; b < 3; b++) {
                            hf(&hpf, mat[a][b]);
                        }
                    }
                }
            }
        }
        fn_line(o, "photo_filter_matrix", hpf);
    }
    {
        uint64_t hr = H0;
        uint64_t seed;
        float *tables = xmalloc(3 * LUT * sizeof *tables);
        for (seed = 1; seed <= 20; seed++) {
            rng r = {seed * 77 + 1};
            pc_hue_range ranges[6];
            pc_hue_range_defaults(ranges);
            for (i = 0; i < 6; i++) {
                if (below(&r, 2) == 0) {
                    float b0;
                    ranges[i].hue = span(&r, -180.0f, 180.0f);
                    ranges[i].saturation = span(&r, -100.0f, 100.0f);
                    ranges[i].lightness = span(&r, -100.0f, 100.0f);
                    b0 = span(&r, -60.0f, 400.0f);
                    ranges[i].bounds[0] = b0;
                    ranges[i].bounds[1] = b0 + 10.0f;
                    ranges[i].bounds[2] = b0 + 40.0f;
                    ranges[i].bounds[3] = b0 + 70.0f;
                }
            }
            pc_compose_hue_range_tables(ranges, tables);
            for (i = 0; i < 3 * LUT; i++) {
                hf(&hr, tables[i]);
            }
        }
        free(tables);
        fn_line(o, "hue_range_tables", hr);
    }
}

static void gradient_sweeps(out *o)
{
    static const float ts[11] = {-0.5f, 0.0f, 0.1f, 0.25f, 0.33f, 0.5f, 0.75f, 0.999f, 1.0f, 1.2f, NAN};
    const pc_rect frames[4] = {{0, 0, 8, 8}, {-3, 2, 20, 9}, {5, 5, 5, 9}, {1, 1, 2, 2}};
    uint64_t hr = H0;
    uint64_t hdd = H0;
    uint64_t seed;
    size_t i;
    size_t j;
    int c;
    for (seed = 1; seed <= 300; seed++) {
        rng r = {(seed * 0x9e3779b97f4a7c15ull) ^ 0xabcdefu};
        pc_fill f = gradient_fill(&r);
        pc_compose_ramp ramp;
        bool found = false;
        pc_compose_buffer b;
        CHECK(pc_compose_ramp_init(&f, &found, &ramp, NULL) == PC_OK && found);
        for (i = 0; found && i < 11; i++) {
            float v[4];
            pc_compose_ramp_sample(&ramp, ts[i], v);
            for (c = 0; c < 4; c++) {
                hf(&hr, v[c]);
            }
        }
        if (found) {
            pc_compose_ramp_fini(&ramp);
        }
        CHECK(pc_compose_gradient_fill_render(&f, pc_rect_make(-2, 1, 9, 6), frames[seed % 4], &b, NULL) ==
              PC_OK);
        hu64(&hdd, hpx(b.px, b.len));
        pc_compose_buffer_fini(&b);
        if (seed % 100 == 0) {
            char name[32];
            (void)snprintf(name, sizeof name, "gradient_render_%llu", (unsigned long long)seed);
            fn_line(o, name, hdd);
        }
        pc_fill_fini(&f);
    }
    fn_line(o, "ramp_sample", hr);
    {
        static const float ms[9] = {0.0f, 0.05f, 0.25f, 0.5f, 0.5000001f, 0.75f, 0.95f, 1.0f, NAN};
        uint64_t hm = H0;
        int32_t x;
        int32_t y;
        for (i = 0; i < 11; i++) {
            for (j = 0; j < 9; j++) {
                hf(&hm, pc_compose_midpoint_remap(ts[i], ms[j]));
            }
        }
        for (y = -3; y < 3; y++) {
            for (x = -3; x < 3; x++) {
                float c4[4] = {0.5f, 0.0f, 1.0f, 0.7f};
                pc_compose_gradient_dither(c4, x * 1000, y * 77);
                for (c = 0; c < 4; c++) {
                    hf(&hm, c4[c]);
                }
            }
        }
        fn_line(o, "midpoint_dither", hm);
    }
    {
        const pc_rect hframes[3] = {{10, -4, 210, 96}, {0, 0, 1, 1}, {3, 3, 3, 3}};
        static const float angles[5] = {0.0f, 30.0f, 90.0f, -135.0f, 1e9f};
        static const float scales[3] = {1.0f, 0.0f, 2.5f};
        static const float offs[2][2] = {{0.0f, 0.0f}, {0.3f, -0.2f}};
        static const float same[2] = {4.0f, 4.0f};
        uint64_t hh = H0;
        size_t s;
        size_t fr;
        for (s = 0; s < 5; s++) {
            for (fr = 0; fr < 3; fr++) {
                size_t a;
                size_t sc;
                size_t of;
                float a2;
                float s2;
                float o2[2];
                for (a = 0; a < 5; a++) {
                    for (sc = 0; sc < 3; sc++) {
                        for (of = 0; of < 2; of++) {
                            float p[2];
                            float q[2];
                            pc_compose_gradient_handles(STYLES[s], angles[a], scales[sc], offs[of],
                                                        hframes[fr], p, q);
                            hf(&hh, p[0]);
                            hf(&hh, p[1]);
                            hf(&hh, q[0]);
                            hf(&hh, q[1]);
                            pc_compose_gradient_from_handles(STYLES[s], p, q, hframes[fr], 17.0f, &a2, &s2,
                                                             o2);
                            hf(&hh, a2);
                            hf(&hh, s2);
                            hf(&hh, o2[0]);
                            hf(&hh, o2[1]);
                        }
                    }
                }
                pc_compose_gradient_from_handles(STYLES[s], same, same, hframes[fr], 17.0f, &a2, &s2, o2);
                hf(&hh, a2);
                hf(&hh, s2);
                hf(&hh, o2[0]);
                hf(&hh, o2[1]);
            }
        }
        fn_line(o, "handles", hh);
    }
}

static void pattern_sweeps(out *o)
{
    static const double coords[12] = {0.0,  0.5,  1.25,  -0.75, 3.999,    7.5,
                                      -1e9, 1e30, -1e30, NAN,   INFINITY, 2.5e18};
    static const bool links[5] = {true, false, true, false, true};
    static const float phases[5][2] = {{0, 0}, {2.5f, -1.0f}, {0, 3.0f}, {0, 0}, {1.0f, 1.0f}};
    static const float scales[5] = {1.0f, 0.5f, 0.0f, 2.0f, NAN};
    static const float angles[5] = {0.0f, 30.0f, 90.0f, -45.0f, NAN};
    const double anchor[2] = {1.5, -2.25};
    uint64_t ht = H0;
    uint64_t hm = H0;
    uint64_t seed;
    size_t i;
    size_t j;
    for (seed = 1; seed <= 12; seed++) {
        rng r = {seed * 1009 + 7};
        uint32_t pw = 1 + (uint32_t)below(&r, 6);
        uint32_t ph = 1 + (uint32_t)below(&r, 6);
        uint64_t k = below(&r, 4);
        pc_pixel_format fmt = k == 0 ? PC_PIXEL_FORMAT_RGBA8
                                     : (k == 1 ? PC_PIXEL_FORMAT_RGBA32F
                                               : pf(k == 2 ? PC_COLOR_MODE_GRAYSCALE : PC_COLOR_MODE_LAB,
                                                    k == 2 ? PC_SAMPLE_TYPE_U16 : PC_SAMPLE_TYPE_U8, true));
        pc_surface s;
        pc_pattern pat;
        pc_compose_pattern_tile *tile = NULL;
        pc_surface_init(&s, fmt);
        paint(&r, &s, (int32_t)pw, (int32_t)ph);
        must(pc_pattern_new("t", 1, &s, pw, ph, &pat, NULL));
        CHECK(pc_compose_pattern_tile_new(&pat, &tile, NULL) == PC_OK);
        if (tile == NULL) {
            pc_pattern_fini(&pat);
            continue;
        }
        for (i = 0; i < 12; i++) {
            for (j = 0; j < 12; j++) {
                float v[4];
                int c;
                pc_compose_pattern_tile_sample(tile, coords[i], coords[j], v);
                for (c = 0; c < 4; c++) {
                    hf(&ht, v[c]);
                }
            }
        }
        for (i = 0; i < 5; i++) {
            pc_compose_placement place = pc_compose_placement_new(pc_rect_make(2, -3, 11, 9), links[i],
                                                                  phases[i], scales[i], angles[i]);
            pc_compose_placement anch =
                pc_compose_placement_anchored(anchor, links[i], phases[i], scales[i], angles[i]);
            pc_compose_buffer b;
            CHECK(pc_compose_pattern_render(tile, &place, pc_rect_make(-3, -2, 6, 5), &b, NULL) == PC_OK);
            hu64(&ht, hpx(b.px, b.len));
            pc_compose_buffer_fini(&b);
            for (j = 0; j < 6; j++) {
                double u;
                double v;
                pc_compose_placement_map(&anch, coords[j], coords[j] * 0.5 - 1.0, &u, &v);
                hd(&hm, u);
                hd(&hm, v);
            }
        }
        pc_compose_pattern_tile_release(tile);
        pc_pattern_fini(&pat);
    }
    {
        const float ph[2] = {1.0f, 2.0f};
        pc_compose_placement e = pc_compose_placement_new(PC_RECT_EMPTY, true, ph, 1.0f, 10.0f);
        double u;
        double v;
        pc_compose_placement_map(&e, 3.0, 4.0, &u, &v);
        hd(&hm, u);
        hd(&hm, v);
    }
    fn_line(o, "pattern_tiles", ht);
    fn_line(o, "placement", hm);
}

static void split_sweeps(out *o)
{
    uint64_t h = H0;
    uint64_t seed;
    for (seed = 1; seed <= 40; seed++) {
        rng r = {seed * 31 + 5};
        int32_t w = 6 + (int32_t)below(&r, 20);
        int32_t hh = 5 + (int32_t)below(&r, 16);
        uint64_t d = below(&r, 3);
        pc_layer l = stroked_shape(&r, w, hh);
        pc_rect canvas = pc_rect_make(0, 0, w, hh);
        bool found = false;
        pc_surface fs;
        pc_surface ss;
        CHECK(pc_compose_shape_split(&l.content.u.shape, canvas, DEPTHS[d], &found, &fs, &ss, NULL) == PC_OK);
        if (found) {
            hu64(&h, hsurf(&fs, canvas));
            hu64(&h, hsurf(&ss, canvas));
            pc_surface_fini(&fs);
            pc_surface_fini(&ss);
        } else {
            fnv(&h, "none", 4);
        }
        pc_layer_fini(&l);
    }
    fn_line(o, "shape_split", h);
}

/* ---- comparison ---- */

/* R9 (resync) sweeps, appended by rustref compose2_vectors.rs r9_sweeps. */
static void r9_fill_sweeps(out *o)
{
    static const bool labs[3] = {false, true, false};
    static const bool hdrs[3] = {false, false, true};
    static const float balpha[3] = {1.0f, 0.5f, 0.0f};
    static const float talpha[4] = {1.0f, 0.25f, 1.0f, 0.6f};
    static const size_t js[7] = {0, 3, 7, 11, 12, 13, 15};
    static const float fills[5] = {0.0f, 0.3f, 0.5f, 1.0f, NAN};
    static const float ops[3][2] = {{1.0f, 1.0f}, {0.6f, 1.0f}, {0.8f, 1.45f}};
    uint64_t h = H0;
    uint64_t hc = H0;
    int flag;
    for (flag = 0; flag < 3; flag++) {
        bool lab = pc_psblend_set_lab_mix(labs[flag]);
        bool hdr = pc_psblend_set_hdr(hdrs[flag]);
        size_t m;
        for (m = 0; m < PC_BLEND_LAYER_MODE_COUNT + PC_BLEND_PAINT_NET_MODE_COUNT; m++) {
            pc_blend_mode mode = m < PC_BLEND_LAYER_MODE_COUNT
                                     ? PC_BLEND_LAYER_MODES[m]
                                     : PC_BLEND_PAINT_NET_MODES[m - PC_BLEND_LAYER_MODE_COUNT];
            size_t i;
            size_t jj;
            for (i = 0; i < 16; i++) {
                for (jj = 0; jj < 7; jj++) {
                    size_t j = js[jj];
                    const float b[4] = {VALS[i], VALS[j], VALS[(i + j) % 16], balpha[flag]};
                    const float t[4] = {VALS[j], VALS[(i * 3) % 16], VALS[i], talpha[i % 4]};
                    float v[4];
                    int f;
                    int k;
                    for (f = 0; f < 5; f++) {
                        int q;
                        pc_psblend_rgb_fill(mode, b, t, fills[f], v);
                        for (k = 0; k < 3; k++) {
                            hf(&h, v[k]);
                        }
                        for (q = 0; q < 3; q++) {
                            pc_psblend_composite_fill(mode, b, t, ops[q][0], fills[f], ops[q][1], v);
                            for (k = 0; k < 4; k++) {
                                hf(&h, v[k]);
                            }
                        }
                    }
                    pc_psblend_composite(mode, b, t, 0.7f, v);
                    for (k = 0; k < 4; k++) {
                        hf(&hc, v[k]);
                    }
                }
            }
        }
        (void)pc_psblend_set_hdr(hdr);
        (void)pc_psblend_set_lab_mix(lab);
    }
    fn_line(o, "r9_fill_blend", h);
    fn_line(o, "r9_paint_net_composite", hc);
}

static void r9_curve_sweeps(out *o)
{
    float *lut = xmalloc(4 * LUT * sizeof *lut);
    uint64_t hp = H0;
    uint64_t hk = H0;
    uint64_t ha = H0;
    uint64_t hl = H0;
    uint64_t seed;
    size_t k;
    const bool has_d[4] = {false, true, true, true};
    const pc_sample_type ds[4] = {PC_SAMPLE_TYPE_F32, PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16,
                                  PC_SAMPLE_TYPE_F32};
    const pc_compose_transfer srgb = {PC_COMPOSE_TRANSFER_SRGB, 0.0f};
    for (seed = 1; seed <= 300; seed++) {
        rng r = {(seed * 0x9e3779b97f4a7c15ull) | 1};
        pc_curve c = curve(&r);
        size_t nc = c.len + (size_t)below(&r, 2);
        bool *corners = xmalloc((nc > 0 ? nc : 1) * sizeof *corners);
        for (k = 0; k < nc; k++) {
            corners[k] = below(&r, 2) == 0;
        }
        CHECK(pc_compose_curve_map_lut(c.pts, c.len, lut, NULL) == PC_OK);
        for (k = 0; k < LUT; k++) {
            hf(&hp, lut[k]);
        }
        CHECK(pc_compose_contour_curve_lut(c.pts, c.len, corners, nc, lut, NULL) == PC_OK);
        for (k = 0; k < LUT; k++) {
            hf(&hk, lut[k]);
        }
        free(corners);
        pc_curve_fini(&c);
    }
    fn_line(o, "r9_curve_map_lut", hp);
    fn_line(o, "r9_contour_curve_lut", hk);
    for (seed = 1; seed <= 300; seed++) {
        rng r = {(seed * 0x2545f4914f6cdd1dull) | 1};
        pc_adjustment a = adjustment(&r);
        int d;
        if (a.kind == PC_ADJ_CURVES) {
            a.u.curves.pencil = below(&r, 2) == 0;
        }
        for (d = 0; d < 4; d++) {
            pc_compose_buffer b = test_buffer();
            CHECK(pc_compose_adjust_apply_in(&a, &b, srgb, has_d[d], ds[d], PC_COLOR_MODE_LAB, NULL) ==
                  PC_OK);
            hu64(&ha, hpx(b.px, b.len));
            pc_compose_buffer_fini(&b);
        }
        if (a.kind == PC_ADJ_CURVES) {
            CHECK(pc_compose_tone_luts(&a, lut, NULL) == PC_OK);
            for (k = 0; k < 4 * LUT; k++) {
                hf(&hl, lut[k]);
            }
        }
        pc_adjustment_fini(&a);
    }
    fn_line(o, "r9_apply_in_lab", ha);
    fn_line(o, "r9_pencil_tone_luts", hl);
    free(lut);
}

static void r9_native_lines(out *o)
{
    static const float alphas[4] = {1.0f, 0.5f, 0.0f, 0.3f};
    static const float opac[3] = {1.0f, 0.6f, 0.0f};
    static const float fills[2] = {1.0f, 0.5f};
    uint64_t seed;
    for (seed = 1; seed <= 40; seed++) {
        rng r = {(seed * 0x517cc1b727220aull) | 1};
        pc_sample_type depth = below(&r, 2) == 0 ? PC_SAMPLE_TYPE_U8 : PC_SAMPLE_TYPE_U16;
        uint32_t w = 1 + (uint32_t)below(&r, 40);
        uint32_t h = 1 + (uint32_t)below(&r, 30);
        pc_document doc = ch_doc(w, h, PC_COLOR_MODE_CMYK, depth);
        pc_pixel_format fmt = {PC_COLOR_MODE_CMYK, depth, true};
        uint64_t nl = 1 + below(&r, 3);
        uint64_t k;
        bool paper;
        bool done = false;
        pc_rect rect;
        pc_surface s;
        for (k = 0; k < nl; k++) {
            pc_layer l = ch_raster("ink", fmt);
            int32_t x0 = (int32_t)below(&r, 30) - 5;
            int32_t y0 = (int32_t)below(&r, 20) - 5;
            float px[5];
            int32_t x1;
            int32_t y1;
            int c;
            for (c = 0; c < 4; c++) {
                px[c] = unit(&r);
            }
            px[4] = alphas[below(&r, 4)];
            x1 = x0 + 1 + (int32_t)below(&r, 30);
            y1 = y0 + 1 + (int32_t)below(&r, 20);
            ch_fill(&l, pc_rect_make(x0, y0, x1, y1), px, 5);
            l.opacity = opac[below(&r, 3)];
            l.fill_opacity = fills[below(&r, 2)];
            l.visible = below(&r, 6) != 0;
            if (below(&r, 8) == 0) {
                l.blend = PC_BLEND_MULTIPLY;
            }
            ch_push(&doc, l);
        }
        paper = below(&r, 2) == 0;
        rect = pc_rect_make(-2, -1, (int32_t)w + 3, (int32_t)h + 1);
        CHECK(pc_compose_flatten_cmyk(&doc, rect, fmt, paper, &done, &s, NULL) == PC_OK);
        if (done) {
            size_t n = (size_t)pc_rect_width(rect) * (size_t)pc_rect_height(rect) * 5;
            float *v = xmalloc(n * sizeof *v);
            uint64_t hh = H0;
            size_t i;
            CHECK(pc_surface_read_region_into(&s, rect, v, n, NULL) == PC_OK);
            for (i = 0; i < n; i++) {
                hf(&hh, v[i]);
            }
            put(o, "native\t%llu\t%016llx\t", (unsigned long long)seed, (unsigned long long)hh);
            rs(o, pc_surface_content_bounds(&s));
            put(o, "\n");
            free(v);
            pc_surface_fini(&s);
        } else {
            put(o, "native\t%llu\tnone\n", (unsigned long long)seed);
        }
        pc_document_fini(&doc);
    }
}

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
    adjust_sweeps(&o);
    color_sweeps(&o);
    gradient_sweeps(&o);
    pattern_sweeps(&o);
    split_sweeps(&o);
    for (seed = 1; seed <= 400; seed++) {
        doc_line(&o, seed);
    }
    r9_fill_sweeps(&o);
    r9_curve_sweeps(&o);
    r9_native_lines(&o);
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
    (void)fprintf(stderr, "%d golden lines compared\n", lines);
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
