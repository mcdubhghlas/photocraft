/* Tests of the compose resync (R9) for layer effects and the compositor: the upstream tests added
 * after C3 in effects.rs (chisel facets and distances placed by edge coverage, #1926; glow and satin
 * measurements), tests.rs (group effects reaching past children's effects, outer glow at 0 % Fill,
 * Transparency Shapes Layer off maps clipped to the band, #1909; flattening transparent pixels),
 * advanced_tests.rs (shapeless stroke skips, localized Background), native.rs (CMYK flattening in
 * ink values), multichannel.rs, proxy.rs and masks.rs. */
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "fxh.h"

/* ---- maps ---- */

/* A len x wid bar through the middle of a w x h map at deg degrees, anti-aliased by 16 x 16
 * supersampling. */
static pc_fxm aa_bar(size_t w, size_t h, float deg, float len, float wid)
{
    pc_fxm m;
    float sn = sinf(deg * 3.14159265f / 180.0f);
    float cs = cosf(deg * 3.14159265f / 180.0f);
    size_t x;
    size_t y;
    if (pc_fxm_new(w, h, 0.0f, &m, NULL) != PC_OK) {
        abort();
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int hit = 0;
            int sx;
            int sy;
            for (sy = 0; sy < 16; sy++) {
                for (sx = 0; sx < 16; sx++) {
                    float px = (float)x + ((float)sx + 0.5f) / 16.0f - (float)w / 2.0f;
                    float py = (float)y + ((float)sy + 0.5f) / 16.0f - (float)h / 2.0f;
                    float u = px * cs + py * sn;
                    float v = -px * sn + py * cs;
                    hit += fabsf(u) <= len / 2.0f && fabsf(v) <= wid / 2.0f;
                }
            }
            m.v[y * w + x] = (float)hit / 256.0f;
        }
    }
    return m;
}

static pc_fxm square(size_t n, size_t x0, size_t x1)
{
    pc_fxm m;
    size_t x;
    size_t y;
    if (pc_fxm_new(n, n, 0.0f, &m, NULL) != PC_OK) {
        abort();
    }
    for (y = x0; y < x1; y++) {
        for (x = x0; x < x1; x++) {
            m.v[y * n + x] = 1.0f;
        }
    }
    return m;
}

/* Coverage of a unit pixel by the half plane n.p + s >= 0, by 64 x 64 supersampling. */
static float half_plane_coverage(const float n[2], float s)
{
    int hit = 0;
    int x;
    int y;
    for (y = 0; y < 64; y++) {
        for (x = 0; x < 64; x++) {
            float px = ((float)x + 0.5f) / 64.0f - 0.5f;
            float py = ((float)y + 0.5f) / 64.0f - 0.5f;
            hit += n[0] * px + n[1] * py + s >= 0.0f;
        }
    }
    return (float)hit / 4096.0f;
}

static void test_edge_offset_places_a_straight_edge_by_its_coverage(void)
{
    int step;
    int k;
    for (step = 0; step <= 12; step++) {
        float a = (float)step * 7.5f * 3.14159265f / 180.0f;
        const float n[2] = {cosf(a), sinf(a)};
        for (k = -12; k <= 12; k++) {
            float s = (float)k * 0.05f;
            float cov = half_plane_coverage(n, s);
            if (cov < 0.03f || cov > 0.97f) {
                continue;
            }
            CHECK(fabsf(pc_fx_edge_offset(n, cov) - s) < 0.03f);
        }
    }
    CHECK(fabsf(pc_fx_edge_offset((const float[2]){1, 0}, 0.25f) + 0.25f) < 1e-6f);
    CHECK(fabsf(pc_fx_edge_offset((const float[2]){0, 1}, 0.9f) - 0.4f) < 1e-6f);
    CHECK(pc_fx_edge_offset((const float[2]){0.6f, 0.8f}, NAN) == 0.0f);
}

static void fields(const pc_fxm *m, float reach, float **bin, float **bout)
{
    pc_fx_sharp_edges sharp;
    CHECK(pc_fx_sharp_edges_find(m, &sharp, NULL) == PC_OK);
    CHECK(pc_fx_bevel_dist_inside(m, &sharp, reach, bin, NULL) == PC_OK);
    CHECK(pc_fx_bevel_dist_outside(m, &sharp, reach, bout, NULL) == PC_OK);
    pc_fx_sharp_edges_fini(&sharp);
}

static void test_bevel_distances_equal_the_plain_ones_without_anti_aliasing(void)
{
    /* Hard edges (no partly covered pixel) and soft ramps (no sharp one) keep the plain fields. */
    const size_t w = 96;
    const size_t h = 80;
    int kind;
    for (kind = 0; kind < 4; kind++) {
        pc_fxm m;
        uint32_t seed = 12345;
        size_t x;
        size_t y;
        float *bin = NULL;
        float *bout = NULL;
        float *din = NULL;
        float *dout = NULL;
        pc_fx_sharp_edges sharp;
        CHECK(pc_fxm_new(w, h, 0.0f, &m, NULL) == PC_OK);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                float dx = (float)x - 48.0f;
                float dy = (float)y - 40.0f;
                float v;
                if (kind == 0) {
                    float t = (float)x * 0.6f + (float)y;
                    v = t >= 50.0f && t < 90.0f ? 1.0f : 0.0f;
                } else if (kind == 1) {
                    v = dx * dx + dy * dy < 900.0f ? 1.0f : 0.0f;
                } else if (kind == 2) {
                    seed = seed * 1664525u + 1013904223u;
                    v = (seed >> 28) < 5 && x > 10 && x < 80 ? 1.0f : 0.0f;
                } else {
                    v = pc_clampf((20.0f - sqrtf(dx * dx + dy * dy)) / 8.0f, 0.0f, 1.0f);
                }
                m.v[y * w + x] = v;
            }
        }
        CHECK(pc_fx_sharp_edges_find(&m, &sharp, NULL) == PC_OK && sharp.npixels == 0);
        pc_fx_sharp_edges_fini(&sharp);
        fields(&m, 1e9f, &bin, &bout);
        CHECK(pc_fx_dist_inside(&m, PC_FX_EUCLIDEAN, &din, NULL) == PC_OK);
        CHECK(pc_fx_dist_outside(&m, PC_FX_EUCLIDEAN, &dout, NULL) == PC_OK);
        CHECK(memcmp(bin, din, w * h * sizeof *bin) == 0);
        CHECK(memcmp(bout, dout, w * h * sizeof *bout) == 0);
        free(bin);
        free(bout);
        free(din);
        free(dout);
        pc_fxm_free(&m);
    }
}

/* Worst standard deviation (ripple) and mean error (bias) of a field on an aa_bar among pixels at
 * the same true distance from a long edge (0.5 px bins, 0.6..8 px). */
static void bar_ripple(const float *f, size_t w, size_t h, float deg, float wid, bool inside, float *ripple,
                       float *bias)
{
    double sum[16] = {0};
    double sq[16] = {0};
    int cnt[16] = {0};
    float sn = sinf(deg * 3.14159265f / 180.0f);
    float cs = cosf(deg * 3.14159265f / 180.0f);
    size_t x;
    size_t y;
    int b;
    *ripple = 0.0f;
    *bias = 0.0f;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float px = (float)x + 0.5f - (float)w / 2.0f;
            float py = (float)y + 0.5f - (float)h / 2.0f;
            float u = px * cs + py * sn;
            float v = -px * sn + py * cs;
            float truth = inside ? wid / 2.0f - fabsf(v) : fabsf(v) - wid / 2.0f;
            if (fabsf(u) < 40.0f && truth >= 0.6f && truth < 8.0f) {
                double e = (double)(f[y * w + x] - truth);
                b = (int)(truth * 2.0f);
                sum[b] += e;
                sq[b] += e * e;
                cnt[b]++;
            }
        }
    }
    for (b = 0; b < 16; b++) {
        if (cnt[b] > 8) {
            double mean = sum[b] / cnt[b];
            double sd = sqrt(fmax(sq[b] / cnt[b] - mean * mean, 0.0));
            *ripple = fmaxf(*ripple, (float)sd);
            *bias = fmaxf(*bias, (float)fabs(mean));
        }
    }
}

static void test_chisel_distance_has_no_ripple_along_oblique_edges(void)
{
    /* Issue #1926: the plain distance ripples with the staircase of an oblique edge. */
    static const float degs[7] = {0, 30, 45, 60, 75, 120, 150};
    int i;
    float r;
    float bi;
    float *plain = NULL;
    pc_fxm m;
    for (i = 0; i < 7; i++) {
        float *in = NULL;
        float *out = NULL;
        m = aa_bar(160, 160, degs[i], 120, 40);
        fields(&m, 40.0f, &in, &out);
        bar_ripple(in, 160, 160, degs[i], 40, true, &r, &bi);
        CHECK(r < 0.008f && bi < 0.01f);
        bar_ripple(out, 160, 160, degs[i], 40, false, &r, &bi);
        CHECK(r < 0.008f && bi < 0.01f);
        free(in);
        free(out);
        pc_fxm_free(&m);
    }
    m = aa_bar(160, 160, 60, 120, 40);
    CHECK(pc_fx_dist_inside(&m, PC_FX_EUCLIDEAN, &plain, NULL) == PC_OK);
    bar_ripple(plain, 160, 160, 60, 40, true, &r, &bi);
    CHECK(r > 0.1f); /* the plain field still ripples, so the checks above mean something */
    free(plain);
    pc_fxm_free(&m);
}

static void test_bevel_distances_survive_degenerate_maps(void)
{
    static const struct {
        size_t w;
        size_t h;
        float a;
    } cases[7] = {{0, 0, 0}, {1, 1, 0.5f}, {1, 1, 1}, {2, 2, 0.5f}, {3, 1, 0.3f}, {5, 5, NAN}, {4, 4, 1}};
    static const float reaches[5] = {0, 8, NAN, INFINITY, -3};
    int c;
    int k;
    for (c = 0; c < 7; c++) {
        pc_fxm m;
        CHECK(pc_fxm_new(cases[c].w, cases[c].h, cases[c].a, &m, NULL) == PC_OK);
        for (k = 0; k < 5; k++) {
            float *in = NULL;
            float *out = NULL;
            fields(&m, reaches[k], &in, &out);
            free(in);
            free(out);
        }
        pc_fxm_free(&m);
    }
    {
        pc_fxm m;
        float *in = NULL;
        float *out = NULL;
        size_t y;
        CHECK(pc_fxm_new(5, 5, 0.0f, &m, NULL) == PC_OK);
        m.v[12] = 0.4f;
        fields(&m, 4, &in, &out);
        free(in);
        free(out);
        pc_fxm_free(&m);
        CHECK(pc_fxm_new(8, 8, 0.0f, &m, NULL) == PC_OK);
        for (y = 0; y < 8; y++) {
            m.v[y * 8 + 7] = 1.0f;
            m.v[y * 8 + 6] = 0.5f;
        }
        fields(&m, 4, &in, &out);
        free(in);
        free(out);
        pc_fxm_free(&m);
    }
    /* The public field kinds, including the bevel ones, take hostile input. */
    {
        const float a[6] = {NAN, 0.5f, 1, 0, -1, INFINITY};
        float *d = NULL;
        CHECK(pc_compose_fx_distance_field(PC_COMPOSE_FIELD_BEVEL_INSIDE, a, 3, 2, NAN, &d, NULL) == PC_OK);
        free(d);
        d = NULL;
        CHECK(pc_compose_fx_distance_field(PC_COMPOSE_FIELD_BEVEL_OUTSIDE, a, 3, 2, 2.0f, &d, NULL) == PC_OK);
        free(d);
    }
}

static void bevel_maps(const pc_fxm *shape, const pc_bevel *b, pc_fxm maps[2])
{
    pc_global_light l = pc_global_light_default();
    pc_compose_texture_ctx tex;
    pc_compose_bevel_paint paint;
    memset(&tex, 0, sizeof tex);
    tex.rect = pc_rect_make(0, 0, 1, 1);
    memset(maps, 0, 2 * sizeof *maps);
    CHECK(pc_fx_bevel_maps(shape, b, &l, &tex, NULL, maps, &paint, NULL) == PC_OK);
}

static float facet_amount(float g)
{
    float se = sinf(30.0f * 3.14159265f / 180.0f);
    float ce = cosf(30.0f * 3.14159265f / 180.0f);
    return ((g * ce + se) / sqrtf(1.0f + g * g) - se) / (1.0f - se);
}

static void test_chisel_facets_match_photoshop(void)
{
    /* Photoshop 27.11 across a straight edge (size 10, depth 100 %, light from the top at 30
     * degrees): a facet has the slope depth x size / (2 (size + 1)). */
    const size_t n = 80;
    pc_fxm shape = square(n, 20, 60);
    pc_bevel b = fxh_bevel(PC_BEVEL_INNER, PC_BEVEL_CHISEL_HARD);
    pc_fxm hard[2];
    pc_fxm soft[2];
    pc_fxm outer[2];
    float facet = facet_amount(10.0f / 22.0f);
    size_t y;
    b.size = 10.0f;
    bevel_maps(&shape, &b, hard);
    for (y = 20; y < 30; y++) {
        CHECK(fabsf(hard[0].v[y * n + 40] - facet) < 0.01f);
    }
    CHECK(hard[0].v[30 * n + 40] > 0.05f && hard[0].v[30 * n + 40] < facet);
    b.technique = PC_BEVEL_CHISEL_SOFT;
    bevel_maps(&shape, &b, soft);
    CHECK(fabsf(soft[0].v[20 * n + 40] - facet_amount(1.5f * 10.0f / 22.0f)) < 0.01f);
    CHECK(fabsf(soft[0].v[24 * n + 40] - facet) < 0.01f);
    b.style = PC_BEVEL_OUTER;
    b.technique = PC_BEVEL_CHISEL_HARD;
    bevel_maps(&shape, &b, outer);
    CHECK(fabsf(outer[1].v[70 * n + 40] - outer[1].v[61 * n + 40]) < 1e-4f);
    CHECK(outer[1].v[71 * n + 40] > 0.05f && outer[1].v[71 * n + 40] < outer[1].v[70 * n + 40]);
    CHECK(outer[1].v[72 * n + 40] == 0.0f);
    pc_fxm_free(&hard[0]);
    pc_fxm_free(&hard[1]);
    pc_fxm_free(&soft[0]);
    pc_fxm_free(&soft[1]);
    pc_fxm_free(&outer[0]);
    pc_fxm_free(&outer[1]);
    pc_fxm_free(&shape);
}

static void test_chisel_h_is_defined_where_the_reference_panics(void)
{
    /* A NaN or negative size makes the reference's clamp bound invalid (it panics); C answers. */
    static const float sizes[4] = {NAN, -1.5f, INFINITY, -INFINITY};
    int i;
    int p;
    for (i = 0; i < 4; i++) {
        for (p = 0; p < 3; p++) {
            (void)pc_compose_fx_bevel_chisel_h((pc_compose_bevel_paint)p, sizes[i], 1.0f, 2.0f, false);
            (void)pc_compose_fx_bevel_chisel_h((pc_compose_bevel_paint)p, sizes[i], 1.0f, 2.0f, true);
        }
    }
    CHECK(pc_compose_fx_bevel_chisel_h(PC_COMPOSE_BEVEL_PAINT_OUTER, -1.5f, 1.0f, 2.0f, false) == 0.0f);
}

static void test_linear_bevel_contour_leaves_the_height_alone_at_any_range(void)
{
    static const float ranges[3] = {0.5f, 0.7f, 1.0f};
    int r;
    int k;
    for (r = 0; r < 3; r++) {
        pc_bevel_contour c = pc_bevel_contour_default();
        float *l = NULL;
        c.range = ranges[r];
        CHECK(pc_compose_fx_bevel_contour_lut(&c, &l, NULL) == PC_OK && l != NULL);
        for (k = 0; k <= 4; k++) {
            float v = (float)k / 4.0f;
            CHECK(fabsf(pc_compose_fx_lut_at(l, PC_COMPOSE_FX_LUT_SIZE, v) - v) < 1e-3f);
        }
        free(l);
    }
}

static void test_glows_match_photoshop(void)
{
    /* Photoshop 27.11 on a square: a precise glow's coverage across the edge, and a centre-source
     * inner glow as 1 - the edge glow. */
    const size_t n = 60;
    pc_fxm shape = square(n, 20, 40);
    pc_glow g = fxh_glow(PC_GLOW_PRECISE, PC_GLOW_SOURCE_EDGE);
    pc_fxm m;
    static const int ds[5] = {2, 4, 6, 8, 10};
    static const float ps[5] = {47, 93, 140, 186, 232};
    int i;
    int t;
    g.size = 10.0f;
    g.paint.u.color = PC_COLOR_BLACK;
    CHECK(pc_fx_glow_map(&shape, &g, false, 0, 0, &m, NULL) == PC_OK);
    for (i = 0; i < 5; i++) {
        float got = 255.0f * (1.0f - m.v[30 * n + (size_t)(20 - ds[i])]);
        CHECK(fabsf(got - ps[i]) <= 1.5f);
    }
    pc_fxm_free(&m);
    g.spread = 0.4f;
    CHECK(pc_fx_glow_map(&shape, &g, false, 0, 0, &m, NULL) == PC_OK);
    CHECK(fabsf(m.v[30 * n + 16] - 1.0f) < 1e-6f && m.v[30 * n + 14] < 1.0f);
    pc_fxm_free(&m);
    for (t = 0; t < 2; t++) {
        pc_glow e = fxh_glow(t == 0 ? PC_GLOW_PRECISE : PC_GLOW_SOFTER, PC_GLOW_SOURCE_EDGE);
        pc_glow c = fxh_glow(t == 0 ? PC_GLOW_PRECISE : PC_GLOW_SOFTER, PC_GLOW_SOURCE_CENTER);
        pc_fxm me;
        pc_fxm mc;
        size_t x;
        e.size = c.size = 6.0f;
        CHECK(pc_fx_glow_map(&shape, &e, true, 0, 0, &me, NULL) == PC_OK);
        CHECK(pc_fx_glow_map(&shape, &c, true, 0, 0, &mc, NULL) == PC_OK);
        for (x = 20; x < 40; x++) {
            CHECK(fabsf(mc.v[30 * n + x] - (1.0f - me.v[30 * n + x])) < 1e-5f);
        }
        pc_fxm_free(&me);
        pc_fxm_free(&mc);
    }
    pc_fxm_free(&shape);
}

static void test_satin_at_distance_zero_matches_photoshop(void)
{
    /* Satin black 100 %, angle 90, Linear, distance 0, size 5 over a #808080 square: the rows from
     * the top edge read 82, 102, 123, 128. */
    pc_fxm shape = square(60, 5, 55);
    pc_satin s;
    pc_fxm m;
    static const int rows[5] = {5, 7, 9, 11, 30};
    static const float grey[5] = {82, 102, 123, 128, 128};
    int i;
    memset(&s, 0, sizeof s);
    s.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    s.color = PC_COLOR_BLACK;
    s.angle = 90.0f;
    s.size = 5.0f;
    CHECK(pc_fx_satin_map(&shape, &s, &m, NULL) == PC_OK);
    for (i = 0; i < 5; i++) {
        CHECK(fabsf(m.v[(size_t)rows[i] * 60 + 30] - (1.0f - grey[i] / 128.0f)) < 0.02f);
    }
    pc_fxm_free(&m);
    pc_fxm_free(&shape);
}

/* ---- compositor ---- */

static void test_group_effects_reach_past_children_effects(void)
{
    /* #3274: a child's outside stroke shapes the group's drop shadow. */
    pc_document d = ch_doc_white(48, 48);
    pc_layer sq = ch_solid("sq", pc_rect_make(14, 14, 34, 34), 1, 0, 0, 1);
    pc_effect st = fxh_stroke(6.0f, PC_STROKE_OUTSIDE);
    pc_effect sh = fxh_wrap_shadow(PC_FX_DROP_SHADOW, fxh_shadow(2.0f, 90.0f));
    pc_layer g;
    fxh_add(&sq, &st, 1);
    g = ch_group("g", &sq, 1);
    fxh_add(&g, &sh, 1);
    ch_push(&d, g);
    CHECK(ch_px_close(&d, 9, 24, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 24, 38, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 24, 41, 0, 0, 0, 1));
    CHECK(ch_px_close(&d, 24, 43, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_outer_glow_does_not_paint_through_a_zero_fill_layer(void)
{
    const float bg[4] = {0.18f, 0.42f, 0.85f, 1.0f};
    int t;
    for (t = 0; t < 2; t++) {
        pc_document d = ch_doc_white(96, 96);
        pc_layer *l;
        pc_glow g = fxh_glow(t == 0 ? PC_GLOW_SOFTER : PC_GLOW_PRECISE, PC_GLOW_SOURCE_EDGE);
        pc_effect e;
        float p[4];
        ch_fill(&d.layers[0], pc_document_bounds(&d), bg, 4);
        l = ch_push(&d, ch_solid("glow", pc_rect_make(24, 24, 72, 72), 1.0f, 0.92f, 0.25f, 1));
        g.paint.u.color = pc_color_rgb(1.0f, 0.94f, 0.28f);
        g.spread = 0.5f;
        g.size = 12.0f;
        e = fxh_wrap_glow(PC_FX_OUTER_GLOW, g);
        fxh_add(l, &e, 1);
        l->fill_opacity = 0.0f;
        CHECK(ch_px_close(&d, 48, 48, bg[0], bg[1], bg[2], 1));
        CHECK(ch_px_close(&d, 5, 5, bg[0], bg[1], bg[2], 1));
        ch_px(&d, 22, 48, p);
        CHECK(p[0] > bg[0] + 0.1f);
        d.layers[1].fill_opacity = 1.0f;
        ch_px(&d, 48, 48, p);
        CHECK(p[0] > 0.9f && p[1] > 0.8f);
        pc_document_fini(&d);
    }
}

/* A tall page of Transparency Shapes Layer off layers with local effects. */
static pc_document shapeless_doc(pc_sample_type depth)
{
    pc_document d;
    int i;
    CHECK(pc_document_with_background(&d, "t", pc_size_make(48, 640), PC_COLOR_MODE_RGB, depth,
                                      PC_COLOR_WHITE, NULL) == PC_OK);
    for (i = 0; i < 6; i++) {
        int32_t y = 24 + i * 100;
        pc_layer l = ch_raster("fx", pc_document_pixel_format(&d));
        const float c[4] = {0.2f + 0.08f * (float)i, 0.5f, 1.0f - 0.1f * (float)i, 1.0f};
        pc_effect e[2];
        size_t ne = 1;
        ch_fill(&l, pc_rect_make(10, y, 38, y + 18), c, 4);
        e[0] = fxh_stroke(3.0f + (float)(i % 3), (pc_stroke_position)(i % 3));
        if (i == 3) {
            e[1] = fxh_wrap_glow(PC_FX_INNER_GLOW, fxh_glow(PC_GLOW_SOFTER, PC_GLOW_SOURCE_EDGE));
            ne = 2;
        } else if (i == 4) {
            e[1] = fxh_wrap_bevel(fxh_bevel(PC_BEVEL_EMBOSS, PC_BEVEL_SMOOTH));
            ne = 2;
        }
        fxh_add(&l, e, ne);
        l.advanced.transparency_shapes = false;
        l.opacity = 0.8f;
        if (i < 5) {
            pc_surface m;
            const float one = 1.0f;
            const float half = 0.5f;
            pc_surface_init(&m, (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, depth, false});
            CHECK(pc_surface_fill_rect(&m, pc_rect_make(4 + i, y - 12, 44, y + 30), &one, 1, NULL) == PC_OK);
            CHECK(pc_surface_fill_rect(&m, pc_rect_make(4 + i, y + 30, 30, y + 34), &half, 1, NULL) == PC_OK);
            l.has_mask = true;
            l.mask.surface = m;
            l.mask.enabled = true;
            l.mask.linked = true;
            l.mask.density = 1.0f;
        }
        ch_push(&d, l);
    }
    return d;
}

typedef struct collect {
    pc_compose_buffer all;
    size_t at;
} collect;

static pc_status collect_band(void *ctx, pc_compose_buffer *band, pc_error *err)
{
    collect *c = ctx;
    (void)err;
    if (c->at + band->len <= c->all.len) {
        memcpy(c->all.px + c->at, band->px, band->len * sizeof *band->px);
    }
    c->at += band->len;
    return PC_OK;
}

static void test_shapeless_effects_render_the_same_in_bands_and_parts(void)
{
    static const pc_sample_type ds[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    int k;
    for (k = 0; k < 3; k++) {
        pc_document d = shapeless_doc(ds[k]);
        pc_compose_buffer whole;
        pc_compose_buffer tiled;
        pc_compose_buffer part;
        collect c;
        pc_rect pr = pc_rect_make(3, 290, 41, 333);
        int32_t x;
        int32_t y;
        bool same = true;
        (void)pc_compose_purge_effect_cache();
        CHECK(pc_compose_buffer_transparent(pc_document_bounds(&d), &c.all, NULL) == PC_OK);
        c.at = 0;
        CHECK(pc_compose_render_bands(&d, pc_document_bounds(&d), 64, NULL, collect_band, &c, NULL) == PC_OK);
        (void)pc_compose_purge_effect_cache();
        CHECK(pc_compose_render(&d, pr, &part, NULL) == PC_OK);
        CHECK(pc_compose_render(&d, pc_document_bounds(&d), &whole, NULL) == PC_OK);
        CHECK(c.at == whole.len && memcmp(c.all.px, whole.px, whole.len * sizeof *whole.px) == 0);
        for (y = pr.y0; y < pr.y1; y++) {
            for (x = pr.x0; x < pr.x1; x++) {
                same = same && memcmp(pc_compose_buffer_get(&part, x, y), pc_compose_buffer_get(&whole, x, y),
                                      4 * sizeof(float)) == 0;
            }
        }
        CHECK(same);
        tiled = ch_render_tiled(&d, pc_document_bounds(&d), 16);
        CHECK(pc_compose_buffer_eq(&tiled, &whole));
        pc_compose_buffer_fini(&tiled);
        pc_compose_buffer_fini(&whole);
        pc_compose_buffer_fini(&part);
        pc_compose_buffer_fini(&c.all);
        pc_document_fini(&d);
    }
}

static void test_shapeless_effect_maps_cover_only_the_band_being_rendered(void)
{
    /* #1909: a band builds a TSL-off layer's maps over the band plus twice the effect reach. */
    pc_document d = shapeless_doc(PC_SAMPLE_TYPE_U8);
    pc_cmp_ctx_store s;
    const pc_layer *l = &d.layers[3];
    pc_cmp_fx_ref *a = NULL;
    pc_cmp_fx_ref *b = NULL;
    pc_rect band = pc_rect_make(0, 256, 48, 320);
    bool clipped = false;
    pc_rect r;
    CHECK(pc_cmp_ctx_init_doc(&s, &d, NULL, NULL) == PC_OK);
    CHECK(pc_cmp_effect_maps(l, band, &s.cx, &a, NULL) == PC_OK);
    pc_cmp_set_fx_clip(&s.cx, band);
    CHECK(pc_cmp_effect_maps(l, band, &s.cx, &b, NULL) == PC_OK);
    CHECK(pc_compose_fx_maps_bytes(pc_cmp_fx_maps(b)) * 4 < pc_compose_fx_maps_bytes(pc_cmp_fx_maps(a)));
    pc_cmp_fx_ref_release(b);
    b = NULL;
    /* The next band lets the previous band's maps go. */
    pc_cmp_set_fx_clip(&s.cx, pc_rect_make(0, 320, 48, 384));
    CHECK(pc_cmp_effect_maps(l, pc_rect_make(0, 320, 48, 384), &s.cx, &b, NULL) == PC_OK);
    CHECK(s.fx.len == 2);
    r = pc_cmp_effect_region(l, pc_rect_make(0, 320, 48, 384), &s.cx, &clipped);
    CHECK(clipped && pc_rect_height(r) < 100);
    /* Composited over more than the band: the whole region. */
    r = pc_cmp_effect_region(l, pc_rect_make(0, 200, 48, 500), &s.cx, &clipped);
    CHECK(!clipped && pc_rect_height(r) > 600);
    pc_cmp_fx_ref_release(a);
    pc_cmp_fx_ref_release(b);
    pc_cmp_ctx_fini(&s);
    pc_document_fini(&d);
}

static void test_shapeless_stroke_skip_rejects_advanced_blending_and_masks(void)
{
    pc_layer l = ch_solid("stroke", pc_rect_make(2, 2, 8, 8), 0, 0, 1, 1);
    pc_effect e = fxh_stroke(6.0f, PC_STROKE_OUTSIDE);
    pc_rect r;
    fxh_add(&l, &e, 1);
    l.advanced.transparency_shapes = false;
    CHECK(pc_cmp_shapeless_stroke_bounds(&l, &r) && pc_rect_eq(r, pc_rect_make(2, 2, 8, 8)));
    l.advanced.knockout = PC_KNOCKOUT_DEEP;
    CHECK(!pc_cmp_shapeless_stroke_bounds(&l, &r));
    l.advanced.knockout = PC_KNOCKOUT_NONE;
    l.advanced.blend_interior = true;
    CHECK(!pc_cmp_shapeless_stroke_bounds(&l, &r));
    l.advanced.blend_interior = false;
    l.clipped = true;
    CHECK(!pc_cmp_shapeless_stroke_bounds(&l, &r));
    l.clipped = false;
    /* A non-local effect (a precise glow) keeps the full path. */
    e = fxh_wrap_glow(PC_FX_OUTER_GLOW, fxh_glow(PC_GLOW_PRECISE, PC_GLOW_SOURCE_EDGE));
    fxh_add(&l, &e, 1);
    CHECK(!pc_cmp_shapeless_stroke_bounds(&l, &r));
    pc_layer_fini(&l);
}

static void test_shapeless_stroke_tile_skips_match_full_region(void)
{
    static const pc_blend_mode modes[4] = {PC_BLEND_NORMAL, PC_BLEND_MULTIPLY, PC_BLEND_SCREEN,
                                           PC_BLEND_DIFFERENCE};
    static const float fills[3] = {0.0f, 0.5f, 1.0f};
    int p;
    int m;
    int f;
    for (p = 0; p < 3; p++) {
        for (m = 0; m < 4; m++) {
            for (f = 0; f < 3; f++) {
                pc_document d;
                pc_layer l = ch_solid("stroke", pc_rect_make(28, 318, 52, 342), 0, 0, 1, 1);
                pc_effect e = fxh_stroke(6.0f, (pc_stroke_position)p);
                pc_compose_buffer whole;
                pc_compose_buffer tiled;
                collect c;
                CHECK(pc_document_with_background(&d, "t", pc_size_make(80, 768), PC_COLOR_MODE_RGB,
                                                  PC_SAMPLE_TYPE_U8, PC_COLOR_WHITE, NULL) == PC_OK);
                e.u.stroke.paint.u.color = pc_color_rgb(1, 0, 0);
                fxh_add(&l, &e, 1);
                l.blend = modes[m];
                l.fill_opacity = fills[f];
                l.advanced.transparency_shapes = false;
                ch_push(&d, l);
                whole = ch_render_tiled(&d, pc_document_bounds(&d), 1024);
                tiled = ch_render_tiled(&d, pc_document_bounds(&d), 64);
                CHECK(pc_compose_buffer_eq(&whole, &tiled));
                CHECK(pc_compose_buffer_transparent(pc_document_bounds(&d), &c.all, NULL) == PC_OK);
                c.at = 0;
                CHECK(pc_compose_render_bands(&d, pc_document_bounds(&d), 256, NULL, collect_band, &c,
                                              NULL) == PC_OK);
                CHECK(memcmp(c.all.px, whole.px, whole.len * sizeof *whole.px) == 0);
                pc_compose_buffer_fini(&c.all);
                pc_compose_buffer_fini(&whole);
                pc_compose_buffer_fini(&tiled);
                pc_document_fini(&d);
            }
        }
    }
}

static void test_deep_knockout_reveals_a_localized_background(void)
{
    static const char *names[3] = {"Hintergrund", "\xe8\x83\x8c\xe6\x99\xaf", "Arri\xc3\xa8re-plan"};
    int i;
    for (i = 0; i < 4; i++) {
        pc_document d = ch_doc_white(8, 8);
        pc_layer k = ch_solid("knock", pc_rect_make(0, 0, 6, 8), 0, 0, 1, 1);
        if (i < 3) {
            CHECK(pc_str_set(&d.layers[0].name, names[i], strlen(names[i]), NULL) == PC_OK);
        } else {
            memset(&d.layers[0].locks, 0, sizeof d.layers[0].locks); /* "Background" without its locks */
        }
        ch_push(&d, ch_solid("green", pc_document_bounds(&d), 0, 1, 0, 1));
        k.advanced.knockout = PC_KNOCKOUT_DEEP;
        k.fill_opacity = 0.0f;
        ch_push(&d, k);
        CHECK(i < 3 ? ch_px_close(&d, 2, 2, 1, 1, 1, 1) : ch_px_close(&d, 2, 2, 0, 0, 0, 0));
        pc_document_fini(&d);
    }
}

static void test_flatten_to_surface_leaves_transparent_pixels_empty_in_every_mode(void)
{
    static const pc_color_mode modes[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                           PC_COLOR_MODE_LAB};
    static const pc_sample_type ss[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    int m;
    int k;
    for (m = 0; m < 4; m++) {
        for (k = 0; k < (m < 2 ? 3 : 2); k++) {
            pc_document d = ch_doc(300, 200, modes[m], ss[k]);
            pc_pixel_format fmt = {modes[m], ss[k], true};
            pc_pixel_format opaque = {modes[m], ss[k], false};
            pc_layer l = ch_raster("dot", fmt);
            const float rgba[4] = {0.9f, 0.4f, 0.1f, 1.0f};
            const float white[3] = {1, 1, 1};
            float px[PC_RASTER_MAX_CHANNELS];
            size_t n = pc_from_rgba(fmt, rgba, px);
            pc_surface s;
            float c[4];
            ch_fill(&l, pc_rect_make(120, 80, 140, 90), px, n);
            ch_push(&d, l);
            CHECK(pc_compose_flatten_to_surface(&d, fmt, NULL, &s, NULL) == PC_OK);
            CHECK(pc_rect_eq(pc_surface_content_bounds(&s), pc_rect_make(120, 80, 140, 90)));
            pc_surface_fini(&s);
            CHECK(pc_compose_flatten_to_surface(&d, opaque, white, &s, NULL) == PC_OK);
            pc_surface_rgba(&s, 5, 5, c);
            CHECK(c[3] == 1.0f);
            pc_surface_fini(&s);
            pc_document_fini(&d);
        }
    }
}

static void test_cmyk_documents_flatten_in_ink_values(void)
{
    /* #3778: a lone CMYK Background keeps its ink exactly; a half-opaque layer mixes linearly. */
    pc_document d = ch_doc(8, 4, PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8);
    pc_layer l = ch_raster("ink", PC_PIXEL_FORMAT_CMYKA8);
    pc_layer top = ch_raster("top", PC_PIXEL_FORMAT_CMYKA8);
    const float ink[5] = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    const float half[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.5f};
    pc_surface s;
    float v[5];
    bool done = false;
    ch_fill(&l, pc_rect_make(0, 0, 8, 4), ink, 5);
    ch_fill(&top, pc_rect_make(4, 0, 8, 4), half, 5);
    ch_push(&d, l);
    ch_push(&d, top);
    CHECK(pc_compose_flatten_to_surface(&d, PC_PIXEL_FORMAT_CMYKA8, NULL, &s, NULL) == PC_OK);
    pc_surface_read_pixel(&s, 1, 1, v);
    CHECK(fabsf(v[0] - 51 / 255.0f) < 1e-6f && fabsf(v[3] - 204 / 255.0f) < 1e-6f && v[4] == 1.0f);
    pc_surface_read_pixel(&s, 5, 1, v);
    CHECK(fabsf(v[0] - 153 / 255.0f) < 1.5f / 255.0f && fabsf(v[3] - 102 / 255.0f) < 1.5f / 255.0f);
    pc_surface_fini(&s);
    /* Not every stack is plain: a mask sends it through the compositor. */
    d.layers[1].blend = PC_BLEND_MULTIPLY;
    CHECK(pc_compose_flatten_cmyk(&d, pc_document_bounds(&d), PC_PIXEL_FORMAT_CMYKA8, false, &done, &s,
                                  NULL) == PC_OK &&
          !done);
    pc_document_fini(&d);
}

static void test_alpha_channels_print_as_black_ink_in_multichannel(void)
{
    pc_document d = ch_doc(4, 1, PC_COLOR_MODE_MULTICHANNEL, PC_SAMPLE_TYPE_U8);
    pc_alpha_channel *ch = calloc(1, sizeof *ch);
    const float half = 0.5f;
    pc_compose_buffer b;
    if (ch == NULL) {
        abort();
    }
    pc_surface_init(&ch->surface, (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, false});
    CHECK(pc_surface_fill_rect(&ch->surface, pc_rect_make(0, 0, 1, 1), &half, 1, NULL) == PC_OK);
    d.channels = ch;
    d.nchannels = 1;
    CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK);
    CHECK(fabsf(b.px[0][0] - 0.5f) <= 1.0f / 255.0f && fabsf(b.px[0][1] - 0.5f) <= 1.0f / 255.0f);
    CHECK(b.px[1][0] == 1.0f);
    pc_compose_buffer_fini(&b);
    pc_document_fini(&d);
}

static void test_proxy_accepts_empty_vector_masks(void)
{
    pc_document d = ch_doc(32, 16, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32);
    pc_layer *l = ch_push(&d, ch_raster("red", PC_PIXEL_FORMAT_RGBA32F));
    pc_path empty;
    memset(&empty, 0, sizeof empty);
    pc_vector_mask_init(&l->vector_mask, &empty);
    l->has_vector_mask = true;
    CHECK(pc_compose_proxy_faithful(&d));
    pc_vector_mask_fini(&l->vector_mask);
    {
        const double pts[4][2] = {{16, 8}, {48, 8}, {48, 24}, {16, 24}};
        pc_path p = ch_polygon(pts, 4);
        pc_vector_mask_init(&l->vector_mask, &p);
    }
    CHECK(!pc_compose_proxy_faithful(&d));
    l->vector_mask.enabled = false;
    CHECK(pc_compose_proxy_faithful(&d));
    pc_document_fini(&d);
}

static void test_thumbnail_size_and_mask_cache_purge(void)
{
    pc_document d = ch_doc(300, 100, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    uint32_t w;
    uint32_t h;
    pc_layer *l;
    pc_surface s;
    bool found = false;
    const float half = 0.5f;
    pc_compose_thumbnail_size(&d, 30, &w, &h);
    CHECK(w == 30 && h == 10);
    pc_compose_thumbnail_size(&d, 1000, &w, &h);
    CHECK(w == 300 && h == 100);
    /* A feathered mask is cached as U16 and accounted; a purge releases it. */
    l = ch_push(&d, ch_raster("m", PC_PIXEL_FORMAT_RGBA8));
    pc_layer_mask_reveal_all(&l->mask);
    l->has_mask = true;
    CHECK(pc_surface_fill_rect(&l->mask.surface, pc_rect_make(0, 0, 100, 100), &half, 1, NULL) == PC_OK);
    l->mask.feather = 4.0f;
    (void)pc_compose_purge_mask_cache();
    CHECK(pc_compose_combined_mask(l, pc_document_bounds(&d), &found, &s, NULL) == PC_OK && found);
    CHECK(pc_surface_format(&s).sample == PC_SAMPLE_TYPE_U16);
    CHECK(pc_compose_mask_cache_bytes() > 0);
    CHECK(pc_compose_purge_mask_cache() > 0 && pc_compose_mask_cache_bytes() == 0);
    pc_surface_fini(&s);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_edge_offset_places_a_straight_edge_by_its_coverage);
    RUN(test_bevel_distances_equal_the_plain_ones_without_anti_aliasing);
    RUN(test_chisel_distance_has_no_ripple_along_oblique_edges);
    RUN(test_bevel_distances_survive_degenerate_maps);
    RUN(test_chisel_facets_match_photoshop);
    RUN(test_chisel_h_is_defined_where_the_reference_panics);
    RUN(test_linear_bevel_contour_leaves_the_height_alone_at_any_range);
    RUN(test_glows_match_photoshop);
    RUN(test_satin_at_distance_zero_matches_photoshop);
    RUN(test_group_effects_reach_past_children_effects);
    RUN(test_outer_glow_does_not_paint_through_a_zero_fill_layer);
    RUN(test_shapeless_effects_render_the_same_in_bands_and_parts);
    RUN(test_shapeless_effect_maps_cover_only_the_band_being_rendered);
    RUN(test_shapeless_stroke_skip_rejects_advanced_blending_and_masks);
    RUN(test_shapeless_stroke_tile_skips_match_full_region);
    RUN(test_deep_knockout_reveals_a_localized_background);
    RUN(test_flatten_to_surface_leaves_transparent_pixels_empty_in_every_mode);
    RUN(test_cmyk_documents_flatten_in_ink_values);
    RUN(test_alpha_channels_print_as_black_ink_in_multichannel);
    RUN(test_proxy_accepts_empty_vector_masks);
    RUN(test_thumbnail_size_and_mask_cache_purge);
    return pc_t_finish();
}
