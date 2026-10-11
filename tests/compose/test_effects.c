/* Layer effects (C3): the #[test]s of crates/compose/src/effects.rs (distance transforms, the tent
 * blur, gradients, bevels, glows, shadows, noise) and the layer-effect tests of
 * crates/compose/src/tests.rs (strokes, shadows, glows, overlays, satin / bevel / emboss,
 * clipped and group effects, tiles, PSD-fidelity semantics, linked patterns). */
#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "fxh.h"

/* ---------- effects.rs ---------- */

/* edt(inside) through the Outside field: binary shapes give edt - 0.5 outside. */
static float *edt_of(const bool *inside, size_t w, size_t h)
{
    float *a = calloc(w * h, sizeof *a);
    float *d = NULL;
    size_t i;
    CHECK(a != NULL);
    if (a == NULL) {
        return NULL;
    }
    for (i = 0; i < w * h; i++) {
        a[i] = inside[i] ? 1.0f : 0.0f;
    }
    CHECK(pc_compose_fx_distance_field(PC_COMPOSE_FIELD_OUTSIDE, a, w, h, &d, NULL) == PC_OK);
    for (i = 0; d != NULL && i < w * h; i++) {
        d[i] = inside[i] ? 0.0f : d[i] + 0.5f;
    }
    free(a);
    return d;
}

static void test_edt_is_exact(void)
{
    bool inside[35] = {false};
    float *d;
    inside[2 * 7 + 3] = true;
    d = edt_of(inside, 7, 5);
    if (d == NULL) {
        return;
    }
    CHECK(d[2 * 7 + 3] == 0.0f);
    CHECK(fabsf(d[2 * 7 + 6] - 3.0f) < 1e-6f);
    CHECK(fabsf(d[0] - sqrtf(13.0f)) < 1e-5f);
    CHECK(fabsf(d[4 * 7 + 5] - sqrtf(8.0f)) < 1e-5f);
    free(d);
}

static void test_edt_two_seeds(void)
{
    bool inside[10] = {false};
    const float want[10] = {0, 1, 2, 3, 4, 4, 3, 2, 1, 0};
    float *d;
    int i;
    inside[0] = true;
    inside[9] = true;
    d = edt_of(inside, 10, 1);
    for (i = 0; d != NULL && i < 10; i++) {
        CHECK(d[i] == want[i]);
    }
    free(d);
}

static void test_blur_preserves_mass(void)
{
    pc_fxm m;
    float sum = 0.0f;
    size_t i;
    CHECK(pc_fxm_new(21, 21, 0.0f, &m, NULL) == PC_OK);
    m.v[10 * 21 + 10] = 1.0f;
    CHECK(pc_fxm_tent(&m, 6.0f, NULL) == PC_OK);
    for (i = 0; i < 21 * 21; i++) {
        sum += m.v[i];
    }
    CHECK(fabsf(sum - 1.0f) < 1e-4f);
    CHECK(m.v[10 * 21 + 10] < 1.0f);
    pc_fxm_free(&m);
}

static float gt(pc_gradient_style s, float x, float y)
{
    const float off[2] = {0.0f, 0.0f};
    return pc_compose_gradient_t(s, 0.0f, 1.0f, false, off, pc_rect_make(0, 0, 100, 100), x, y);
}

static void test_gradient_geometry(void)
{
    const float off[2] = {0.0f, 0.0f};
    CHECK(gt(PC_GRADIENT_LINEAR, 0.0f, 50.0f) < 0.01f);
    /* Linear / reflected sample pixel corners: the centre is reached half a pixel later. */
    CHECK(fabsf(gt(PC_GRADIENT_LINEAR, 50.5f, 50.0f) - 0.5f) < 1e-6f);
    CHECK(gt(PC_GRADIENT_LINEAR, 100.0f, 50.0f) > 0.99f);
    CHECK(gt(PC_GRADIENT_REFLECTED, 50.5f, 50.0f) < 1e-6f);
    CHECK(fabsf(gt(PC_GRADIENT_REFLECTED, 0.0f, 50.0f) - 1.0f) < 1e-6f);
    CHECK(gt(PC_GRADIENT_RADIAL, 50.0f, 50.0f) < 1e-6f);
    CHECK(fabsf(gt(PC_GRADIENT_RADIAL, 50.0f, 0.0f) - 1.0f) < 1e-6f);
    CHECK(fabsf(gt(PC_GRADIENT_DIAMOND, 75.0f, 75.0f) - 1.0f) < 1e-6f);
    CHECK(fabsf(gt(PC_GRADIENT_ANGLE, 50.0f, 0.0f) - 0.75f) < 1e-3f);
    CHECK(pc_compose_gradient_t(PC_GRADIENT_LINEAR, 0.0f, 1.0f, true, off, pc_rect_make(0, 0, 100, 100), 0.0f,
                                50.0f) > 0.99f);
}

static void test_effect_gradients_snap_end_points_and_sample_corners(void)
{
    /* 87 degrees over 600 x 60 at (99, 422): end points (397, 482) and (400, 422). */
    pc_rect frame = pc_rect_make(99, 422, 699, 482);
    const float zero[2] = {0.0f, 0.0f};
    pc_compose_gradient_geom g = pc_compose_gradient_layout(PC_GRADIENT_LINEAR, 87.0f, 1.0f, zero, frame);
    float t;
    CHECK(fabsf(g.angle - (float)(atan2(60.0, 3.0) * 180.0 / 3.14159265358979323846)) < 1e-3f);
    t = pc_compose_gradient_t(PC_GRADIENT_LINEAR, g.angle, g.scale, false, g.offset, frame, 399.0f, 452.5f);
    CHECK(fabsf(t - 0.5f) < 1e-4f);
    CHECK(pc_compose_gradient_t(PC_GRADIENT_LINEAR, g.angle, g.scale, false, g.offset, frame, 400.5f,
                                422.5f) > 0.99f);
    CHECK(pc_compose_gradient_t(PC_GRADIENT_LINEAR, g.angle, g.scale, false, g.offset, frame, 397.5f,
                                482.5f) < 0.01f);
}

static void test_chamfer_uses_euclidean_step_lengths(void)
{
    float a[81] = {0.0f};
    float *d = NULL;
    a[0] = 1.0f;
    CHECK(pc_compose_fx_distance_field(PC_COMPOSE_FIELD_STROKE_OUTSIDE, a, 9, 9, &d, NULL) == PC_OK);
    if (d == NULL) {
        return;
    }
    /* The field is the chamfer distance - 0.5 (the seed starts at 0). */
#define AT(x, y) (d[(y) * 9 + (x)] + 0.5f)
    CHECK(fabsf(AT(3, 0) - 3.0f) < 1e-5f);
    CHECK(fabsf(AT(2, 2) - 2.0f * sqrtf(2.0f)) < 1e-5f);
    CHECK(fabsf(AT(1, 2) - sqrtf(5.0f)) < 1e-5f);
    CHECK(fabsf(AT(1, 3) - (sqrtf(5.0f) + 1.0f)) < 1e-5f); /* knight + straight, not sqrt 10 */
    CHECK(fabsf(AT(2, 3) - (sqrtf(5.0f) + sqrtf(2.0f))) < 1e-5f);
#undef AT
    free(d);
}

static void test_spread_dilates_with_the_chamfer_metric(void)
{
    pc_fxm m;
    pc_fxm d;
    CHECK(pc_fxm_new(9, 9, 0.0f, &m, NULL) == PC_OK);
    m.v[4 * 9 + 4] = 1.0f;
    CHECK(pc_fx_dilate(&m, 3.0f, &d, NULL) == PC_OK);
    /* (1, 3): sqrt 5 + 1 = 3.236 leaves 3 + 0.5 - (3.236 - 0.5) = 0.764. */
    CHECK(fabsf(d.v[7 * 9 + 5] - 0.764f) < 1e-3f);
    CHECK(fabsf(d.v[7 * 9 + 6] - (4.0f - sqrtf(5.0f) - sqrtf(2.0f))) < 1e-4f);
    CHECK(d.v[4 * 9 + 7] == 1.0f);
    pc_fxm_free(&m);
    pc_fxm_free(&d);
}

/* tent_direct: the convolution with the tent kernel (the GPU's way). */
static void tent_direct(float *v, size_t w, size_t h, float width)
{
    int32_t r = 0;
    float *k = NULL;
    size_t n = 0;
    float *tmp = calloc(w * h, sizeof *tmp);
    int64_t x;
    int64_t y;
    CHECK(tmp != NULL && pc_compose_fx_tent_kernel(width, &r, &k, &n, NULL) == PC_OK);
    if (tmp == NULL || k == NULL) {
        free(tmp);
        free(k);
        return;
    }
    for (y = 0; y < (int64_t)h; y++) {
        for (x = 0; x < (int64_t)w; x++) {
            float acc = 0.0f;
            size_t i;
            for (i = 0; i < n; i++) {
                int64_t xx = x + (int64_t)i - r;
                if (xx >= 0 && xx < (int64_t)w) {
                    acc += v[(size_t)(y * (int64_t)w + xx)] * k[i];
                }
            }
            tmp[(size_t)(y * (int64_t)w + x)] = acc;
        }
    }
    for (y = 0; y < (int64_t)h; y++) {
        for (x = 0; x < (int64_t)w; x++) {
            float acc = 0.0f;
            size_t i;
            for (i = 0; i < n; i++) {
                int64_t yy = y + (int64_t)i - r;
                if (yy >= 0 && yy < (int64_t)h) {
                    acc += tmp[(size_t)(yy * (int64_t)w + x)] * k[i];
                }
            }
            v[(size_t)(y * (int64_t)w + x)] = acc;
        }
    }
    free(tmp);
    free(k);
}

static void test_running_sum_tent_matches_the_kernel(void)
{
    const size_t w = 37;
    const size_t h = 23;
    const float widths[6] = {1.0f, 2.0f, 3.5f, 5.0f, 8.25f, 41.0f};
    int k;
    for (k = 0; k < 6; k++) {
        pc_fxm m;
        float *want = malloc(w * h * sizeof *want);
        size_t i;
        int bad = 0;
        CHECK(want != NULL && pc_fxm_new(w, h, 0.0f, &m, NULL) == PC_OK);
        if (want == NULL) {
            return;
        }
        for (i = 0; i < w * h; i++) {
            m.v[i] = (float)((i * 7919) % 101) / 100.0f;
            want[i] = m.v[i];
        }
        CHECK(pc_fxm_tent(&m, widths[k], NULL) == PC_OK);
        tent_direct(want, w, h, widths[k]);
        for (i = 0; i < w * h; i++) {
            bad += !(fabsf(m.v[i] - want[i]) < 1e-5f);
        }
        CHECK_EQ_INT(bad, 0);
        free(want);
        pc_fxm_free(&m);
    }
}

static void test_tent_kernel_is_two_boxes(void)
{
    int32_t r;
    int32_t r2;
    float *k = NULL;
    float *k2 = NULL;
    size_t n;
    size_t n2;
    float sum = 0.0f;
    size_t i;
    const float huge[3] = {1e30f, INFINITY, FLT_MAX};
    CHECK(pc_compose_fx_tent_kernel(5.0f, &r, &k, &n, NULL) == PC_OK);
    CHECK_EQ_INT(r, 4);
    for (i = 0; i < n; i++) {
        sum += k[i];
    }
    CHECK(fabsf(sum - 1.0f) < 1e-6f);
    CHECK(fabsf(k[4] - 5.0f / 25.0f) < 1e-6f && fabsf(k[0] - 1.0f / 25.0f) < 1e-6f);
    free(k);
    /* Fractional widths taper the end taps; width 1 is the identity. */
    CHECK(pc_compose_fx_tent_kernel(4.0f, &r, &k, &n, NULL) == PC_OK);
    CHECK_EQ_INT(r, 4);
    CHECK(k[0] > 0.0f && k[0] < 1.0f / 25.0f);
    free(k);
    CHECK(pc_compose_fx_tent_kernel(1.0f, &r, &k, &n, NULL) == PC_OK);
    CHECK(r == 0 && n == 1 && k[0] == 1.0f);
    free(k);
    /* #1543: a huge, infinite or NaN width is capped, not turned into 2^62 taps. */
    CHECK(pc_compose_fx_tent_kernel(PC_COMPOSE_FX_MAX_REACH, &r2, &k2, &n2, NULL) == PC_OK);
    for (i = 0; i < 3; i++) {
        CHECK(pc_compose_fx_tent_kernel(huge[i], &r, &k, &n, NULL) == PC_OK);
        CHECK(r == r2 && n == n2 && memcmp(k, k2, n * sizeof *k) == 0);
        free(k);
    }
    free(k2);
    CHECK(pc_compose_fx_tent_kernel(NAN, &r, &k, &n, NULL) == PC_OK);
    CHECK(r == 0 && n == 1 && k[0] == 1.0f);
    free(k);
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

static pc_compose_texture_ctx no_tex(void)
{
    pc_compose_texture_ctx t;
    memset(&t, 0, sizeof t);
    t.rect = pc_rect_make(0, 0, 1, 1);
    return t;
}

static void bevel_maps(const pc_fxm *shape, const pc_bevel *b, const pc_compose_texture_ctx *tex,
                       const pc_compose_patterns *pp, pc_fxm maps[2], pc_compose_bevel_paint *paint)
{
    pc_global_light l = pc_global_light_default();
    memset(maps, 0, 2 * sizeof *maps);
    CHECK(pc_fx_bevel_maps(shape, b, &l, tex, pp, maps, paint, NULL) == PC_OK);
}

static void test_smooth_inner_bevel_lights_the_top_edge_inside_only(void)
{
    pc_fxm shape = square(40, 10, 30);
    pc_bevel b = fxh_bevel(PC_BEVEL_INNER, PC_BEVEL_SMOOTH);
    pc_compose_texture_ctx t = no_tex();
    pc_fxm m[2];
    pc_compose_bevel_paint paint;
    bevel_maps(&shape, &b, &t, NULL, m, &paint);
    CHECK(paint == PC_COMPOSE_BEVEL_PAINT_INNER);
#define AT(k, x, y) (m[k].v[(y) * 40 + (x)])
    /* Light from the top: highlight along the top edge, shadow along the bottom, flat middle. */
    CHECK(AT(0, 20, 10) > 0.5f && AT(1, 20, 10) == 0.0f);
    CHECK(AT(1, 20, 29) > 0.5f);
    CHECK(AT(0, 20, 20) < 1e-3f && AT(1, 20, 20) < 1e-3f);
    CHECK(AT(0, 20, 8) == 0.0f);
    CHECK(AT(0, 20, 15) < 0.05f);
#undef AT
    pc_fxm_free(&m[0]);
    pc_fxm_free(&m[1]);
    pc_fxm_free(&shape);
}

static void test_emboss_paints_both_sides_and_pillow_flips_the_outside(void)
{
    pc_fxm shape = square(40, 10, 30);
    pc_bevel b = fxh_bevel(PC_BEVEL_EMBOSS, PC_BEVEL_SMOOTH);
    pc_compose_texture_ctx t = no_tex();
    pc_fxm e[2];
    pc_fxm p[2];
    pc_compose_bevel_paint paint;
    bevel_maps(&shape, &b, &t, NULL, e, &paint);
    CHECK(paint == PC_COMPOSE_BEVEL_PAINT_BOTH);
    CHECK(e[0].v[10 * 40 + 20] > 0.3f && e[0].v[9 * 40 + 20] > 0.3f);
    b.style = PC_BEVEL_PILLOW_EMBOSS;
    bevel_maps(&shape, &b, &t, NULL, p, &paint);
    CHECK(p[0].v[10 * 40 + 20] > 0.3f && p[1].v[9 * 40 + 20] > 0.3f); /* outside top edge in shadow */
    pc_fxm_free(&e[0]);
    pc_fxm_free(&e[1]);
    pc_fxm_free(&p[0]);
    pc_fxm_free(&p[1]);
    pc_fxm_free(&shape);
}

static void test_emboss_width_rounds_up_and_shades_past_the_blur(void)
{
    pc_bevel b = fxh_bevel(PC_BEVEL_EMBOSS, PC_BEVEL_SMOOTH);
    pc_compose_bevel_geom g;
    pc_fxm shape = square(60, 20, 40);
    pc_compose_texture_ctx t = no_tex();
    pc_fxm m[2];
    pc_compose_bevel_paint paint;
    int32_t reach;
    float *k = NULL;
    size_t n;
    size_t row;
    b.size = 41.0f;
    g = pc_compose_fx_bevel_geom(&b);
    CHECK(g.width == 21.0f && g.depth == 21.0f);
    b.size = 5.0f;
    CHECK(pc_compose_fx_bevel_geom(&b).width == 3.0f);
    /* The first row past the height blur's reach still slopes; the row after it does not. */
    b.size = 9.0f;
    b.angle = 90.0f;
    bevel_maps(&shape, &b, &t, NULL, m, &paint);
    CHECK(pc_compose_fx_tent_kernel(pc_compose_fx_bevel_geom(&b).width, &reach, &k, &n, NULL) == PC_OK);
    free(k);
    row = 39 + (size_t)reach + 1;
    CHECK(m[1].v[row * 60 + 30] > 0.0f);
    CHECK(m[1].v[(row + 1) * 60 + 30] == 0.0f);
    pc_fxm_free(&m[0]);
    pc_fxm_free(&m[1]);
    pc_fxm_free(&shape);
}

static void test_chisel_hard_has_flat_facets(void)
{
    pc_fxm shape = square(48, 10, 38);
    pc_bevel b = fxh_bevel(PC_BEVEL_INNER, PC_BEVEL_CHISEL_HARD);
    pc_compose_texture_ctx t = no_tex();
    pc_fxm m[2];
    pc_compose_bevel_paint paint;
    bevel_maps(&shape, &b, &t, NULL, m, &paint);
    CHECK(m[0].v[12 * 48 + 24] > 0.1f && fabsf(m[0].v[12 * 48 + 24] - m[0].v[13 * 48 + 24]) < 1e-3f);
    pc_fxm_free(&m[0]);
    pc_fxm_free(&m[1]);
    pc_fxm_free(&shape);
}

static void test_bevel_contour_and_texture_shape_the_height(void)
{
    pc_fxm shape = square(40, 6, 34);
    pc_bevel plain = fxh_bevel(PC_BEVEL_INNER, PC_BEVEL_SMOOTH);
    pc_bevel c = plain;
    pc_bevel tx = plain;
    pc_compose_texture_ctx t = no_tex();
    pc_fxm a[2];
    pc_fxm b[2];
    pc_fxm m[2];
    pc_compose_bevel_paint paint;
    pc_curve_point fold[3] = {{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}};
    float diff = 0.0f;
    pc_surface sf;
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_pattern pat;
    pc_compose_patterns *pp = NULL;
    float mid = 0.0f;
    float flat = 0.0f;
    size_t i;
    bevel_maps(&shape, &plain, &t, NULL, a, &paint);
    /* A contour reshapes the profile (here a ramp folded at its middle). */
    c.has_contour = true;
    c.contour.contour.custom = true;
    c.contour.contour.points.pts = fold;
    c.contour.contour.points.len = 3;
    c.contour.range = 1.0f;
    bevel_maps(&shape, &c, &t, NULL, b, &paint);
    for (i = 0; i < 1600; i++) {
        diff += fabsf(a[1].v[i] - b[1].v[i]);
    }
    CHECK(diff > 5.0f);
    /* A striped texture lights the flat middle. */
    pc_surface_init(&sf, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&sf, pc_rect_make(0, 0, 4, 2), black, 4, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&sf, pc_rect_make(0, 2, 4, 4), white, 4, NULL) == PC_OK);
    CHECK(pc_pattern_new("stripes", 7, &sf, 4, 4, &pat, NULL) == PC_OK);
    tx.has_texture = true;
    CHECK(pc_str_set_c(&tx.texture.name, "stripes", NULL) == PC_OK);
    tx.texture.scale = 1.0f;
    tx.texture.depth = 1.0f;
    tx.texture.link = true;
    t.rect = pc_rect_make(0, 0, 40, 40);
    t.patterns = &pat;
    t.npatterns = 1;
    CHECK(pc_compose_patterns_new(&pat, 1, PC_COMPOSE_PREPARED_PATTERN_BYTES, &pp, NULL) == PC_OK);
    bevel_maps(&shape, &tx, &t, pp, m, &paint);
    for (i = 16; i < 24; i++) {
        mid += m[0].v[i * 40 + 20] + m[1].v[i * 40 + 20];
        flat += a[0].v[i * 40 + 20] + a[1].v[i * 40 + 20];
    }
    CHECK(mid > flat + 0.5f);
    pc_compose_patterns_free(pp);
    pc_str_fini(&tx.texture.name);
    pc_pattern_fini(&pat);
    for (i = 0; i < 2; i++) {
        pc_fxm_free(&a[i]);
        pc_fxm_free(&b[i]);
        pc_fxm_free(&m[i]);
    }
    pc_fxm_free(&shape);
}

static void test_glow_range_stretches_the_contour(void)
{
    pc_glow g = fxh_glow(PC_GLOW_SOFTER, PC_GLOW_SOURCE_EDGE);
    float *l = NULL;
    g.range = 0.5f;
    CHECK(pc_compose_fx_glow_lut(&g, &l, NULL) == PC_OK && l != NULL);
    if (l != NULL) {
        CHECK(fabsf(pc_compose_fx_lut_at(l, PC_COMPOSE_FX_LUT_SIZE, 0.25f) - 0.5f) < 1e-3f);
        CHECK(fabsf(pc_compose_fx_lut_at(l, PC_COMPOSE_FX_LUT_SIZE, 0.75f) - 1.0f) < 1e-6f);
        free(l);
    }
    g.range = 1.0f;
    l = NULL;
    CHECK(pc_compose_fx_glow_lut(&g, &l, NULL) == PC_OK && l == NULL);
}

static void test_offsets_follow_light(void)
{
    float dx;
    float dy;
    pc_fx_offset(120.0f, 10.0f, &dx, &dy);
    CHECK(dx == 5.0f && dy == 9.0f);
}

static pc_fxm square_shape(size_t n)
{
    return square(n, 5, n - 5);
}

static pc_fxm noisy_shadow(float noise, int32_t ox, int32_t oy)
{
    pc_fxm shape = square_shape(40);
    pc_shadow s = fxh_shadow(0.0f, 0.0f);
    pc_global_light l = pc_global_light_default();
    pc_fxm m;
    s.common = pc_fx_common_new(PC_BLEND_MULTIPLY, 1.0f);
    s.spread = 0.0f;
    s.size = 6.0f;
    s.noise = noise;
    memset(&m, 0, sizeof m);
    CHECK(pc_fx_shadow_map(&shape, &s, &l, false, ox, oy, &m, NULL) == PC_OK);
    pc_fxm_free(&shape);
    return m;
}

static void test_shadow_noise_speckles_soft_coverage_deterministically(void)
{
    pc_fxm plain = noisy_shadow(0.0f, 0, 0);
    pc_fxm noisy = noisy_shadow(0.5f, 0, 0);
    pc_fxm again = noisy_shadow(0.5f, 0, 0);
    pc_fxm moved = noisy_shadow(0.5f, 100, 100);
    pc_fxm heavy = noisy_shadow(1.0f, 0, 0);
    float diff = 0.0f;
    bool in_range = true;
    bool low = false;
    bool high = false;
    size_t i;
    size_t n = pc_fxm_len(&plain);
    for (i = 0; i < n; i++) {
        diff += fabsf(plain.v[i] - noisy.v[i]);
        in_range = in_range && noisy.v[i] >= 0.0f && noisy.v[i] <= 1.0f;
        low = low || heavy.v[i] < 0.5f;
        high = high || heavy.v[i] > 0.5f;
    }
    CHECK(diff > 5.0f);                                        /* noise changes the soft band */
    CHECK(memcmp(noisy.v, again.v, n * sizeof *noisy.v) == 0); /* same coords, same speckle */
    CHECK(in_range);                                           /* every value stays a coverage */
    CHECK(memcmp(noisy.v, moved.v, n * sizeof *noisy.v) != 0); /* hashed from document coords */
    CHECK(low && high);                                        /* heavy noise spans the range */
    pc_fxm_free(&plain);
    pc_fxm_free(&noisy);
    pc_fxm_free(&again);
    pc_fxm_free(&moved);
    pc_fxm_free(&heavy);
}

static void test_glow_noise_follows_the_contour_and_range(void)
{
    pc_fxm shape = square_shape(40);
    pc_glow g = fxh_glow(PC_GLOW_SOFTER, PC_GLOW_SOURCE_EDGE);
    pc_fxm plain;
    pc_fxm noisy;
    float diff = 0.0f;
    size_t i;
    g.common = pc_fx_common_new(PC_BLEND_SCREEN, 1.0f);
    g.size = 6.0f;
    CHECK(pc_fx_glow_map(&shape, &g, false, 0, 0, &plain, NULL) == PC_OK);
    g.noise = 0.6f;
    CHECK(pc_fx_glow_map(&shape, &g, false, 0, 0, &noisy, NULL) == PC_OK);
    for (i = 0; i < pc_fxm_len(&plain); i++) {
        diff += fabsf(plain.v[i] - noisy.v[i]);
    }
    CHECK(diff > 5.0f);
    pc_fxm_free(&plain);
    pc_fxm_free(&noisy);
    pc_fxm_free(&shape);
}

/* ---------- tests.rs: layer effects ---------- */

static void test_outside_stroke_width(void)
{
    pc_effect e = fxh_stroke(3.0f, PC_STROKE_OUTSIDE);
    pc_document d = fxh_doc(&e, 1);
    CHECK(ch_px_close(&d, 8, 20, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 7, 20, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 5, 20, 1, 1, 1, 1));
    CHECK(ch_px_close(&d, 20, 20, 1, 0, 0, 1)); /* interior untouched */
    pc_document_fini(&d);
}

static void test_outside_stroke_at_zero_fill_leaves_the_interior_clear(void)
{
    const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const pc_pixel_format fmts[3] = {PC_PIXEL_FORMAT_RGBA8, PC_PIXEL_FORMAT_RGBA16, PC_PIXEL_FORMAT_RGBA32F};
    int k;
    for (k = 0; k < 3; k++) {
        pc_document d;
        pc_layer *l;
        pc_effect e = fxh_stroke(3.0f, PC_STROKE_OUTSIDE);
        const float red[4] = {1, 0, 0, 1};
        CHECK(pc_document_with_background(&d, "t", pc_size_make(40, 40), PC_COLOR_MODE_RGB, depths[k],
                                          PC_COLOR_WHITE, NULL) == PC_OK);
        l = ch_push(&d, ch_raster("sq", fmts[k]));
        ch_fill(l, pc_rect_make(10, 10, 30, 30), red, 4);
        fxh_add(l, &e, 1);
        l->fill_opacity = 0.0f;
        CHECK(ch_px_close(&d, 20, 20, 1, 1, 1, 1)); /* interior shows the white backdrop */
        CHECK(ch_px_close(&d, 11, 20, 1, 1, 1, 1)); /* just inside the edge too */
        CHECK(ch_px_close(&d, 8, 20, 0, 0, 1, 1));  /* the stroke itself stays */
        d.layers[1].fill_opacity = 0.5f;
        CHECK(ch_px_close(&d, 20, 20, 1, 0.5f, 0.5f, 1));
        d.layers[1].fill_opacity = 1.0f;
        CHECK(ch_px_close(&d, 20, 20, 1, 0, 0, 1));
        pc_document_fini(&d);
    }
}

static void test_inside_and_center_strokes(void)
{
    pc_effect e = fxh_stroke(2.0f, PC_STROKE_INSIDE);
    pc_document d = fxh_doc(&e, 1);
    CHECK(ch_px_close(&d, 10, 20, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 11, 20, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 13, 20, 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 9, 20, 1, 1, 1, 1));
    pc_document_fini(&d);
    e = fxh_stroke(4.0f, PC_STROKE_CENTER);
    d = fxh_doc(&e, 1);
    CHECK(ch_px_close(&d, 8, 20, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 11, 20, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 13, 20, 1, 0, 0, 1));
    pc_document_fini(&d);
}

static void test_master_switch_and_per_effect_enable(void)
{
    pc_effect e = fxh_stroke(3.0f, PC_STROKE_OUTSIDE);
    pc_document d = fxh_doc(&e, 1);
    d.layers[1].effects.enabled = false;
    CHECK(ch_px_close(&d, 8, 20, 1, 1, 1, 1));
    d.layers[1].effects.enabled = true;
    d.layers[1].effects.items[0].u.stroke.common.enabled = false;
    CHECK(ch_px_close(&d, 8, 20, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_color_overlay_ignores_fill_opacity_but_not_opacity(void)
{
    pc_effect e = fxh_color_overlay(0, 1, 0);
    pc_document d = fxh_doc(&e, 1);
    d.layers[1].fill_opacity = 0.0f;
    CHECK(ch_px_close(&d, 20, 20, 0, 1, 0, 1)); /* overlay shows at fill 0 */
    d.layers[1].opacity = 0.5f;
    CHECK(ch_px_close(&d, 20, 20, 0.5f, 1, 0.5f, 1));
    pc_document_fini(&d);
}

static void test_drop_shadow_falls_away_from_light(void)
{
    pc_effect e = fxh_wrap_shadow(PC_FX_DROP_SHADOW, fxh_shadow(5.0f, 90.0f));
    pc_document d = fxh_doc(&e, 1);
    CHECK(ch_px_close(&d, 20, 32, 0, 0, 0, 1)); /* light from the top: shadow below */
    CHECK(ch_px_close(&d, 20, 8, 1, 1, 1, 1));
    pc_document_fini(&d);
    e = fxh_wrap_shadow(PC_FX_DROP_SHADOW, fxh_shadow(5.0f, 0.0f));
    d = fxh_doc(&e, 1);
    CHECK(ch_px_close(&d, 8, 20, 0, 0, 0, 1)); /* light from the right: shadow to the left */
    CHECK(ch_px_close(&d, 32, 20, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_drop_shadow_uses_global_light_and_knockout(void)
{
    pc_shadow s = fxh_shadow(5.0f, 0.0f);
    pc_effect e;
    pc_document d;
    s.use_global_light = true;
    e = fxh_wrap_shadow(PC_FX_DROP_SHADOW, s);
    d = fxh_doc(&e, 1);
    d.global_light.angle = 90.0f;
    CHECK(ch_px_close(&d, 20, 32, 0, 0, 0, 1));
    /* Knock-out: with fill 0 the shadow does not show through the layer. */
    d.layers[1].fill_opacity = 0.0f;
    CHECK(ch_px_close(&d, 20, 28, 1, 1, 1, 1));
    d.layers[1].effects.items[0].u.shadow.knocks_out = false;
    CHECK(ch_px_close(&d, 20, 28, 0, 0, 0, 1));
    pc_document_fini(&d);
}

static void test_soft_shadow_is_blurred_and_bounded(void)
{
    pc_shadow s = fxh_shadow(0.0f, 90.0f);
    pc_effect e;
    pc_document d;
    float edge[4];
    float far[4];
    s.spread = 0.0f;
    s.size = 6.0f;
    e = fxh_wrap_shadow(PC_FX_DROP_SHADOW, s);
    d = fxh_doc(&e, 1);
    ch_px(&d, 30, 20, edge);
    ch_px(&d, 39, 20, far);
    CHECK(edge[0] < 0.8f && edge[0] > 0.2f);
    CHECK(far[0] > 0.99f);
    pc_document_fini(&d);
}

static void test_inner_shadow_only_inside(void)
{
    pc_shadow s = fxh_shadow(4.0f, 90.0f);
    pc_effect e;
    pc_document d;
    s.knocks_out = false;
    e = fxh_wrap_shadow(PC_FX_INNER_SHADOW, s);
    d = fxh_doc(&e, 1);
    CHECK(ch_px_close(&d, 20, 11, 0, 0, 0, 1));
    CHECK(ch_px_close(&d, 20, 25, 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 20, 5, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_outer_and_inner_glow_regions(void)
{
    pc_effect e = fxh_wrap_glow(PC_FX_OUTER_GLOW, fxh_glow(PC_GLOW_PRECISE, PC_GLOW_SOURCE_EDGE));
    pc_document d = fxh_doc(&e, 1);
    float p[4];
    ch_px(&d, 9, 20, p);
    CHECK(p[1] > 0.7f && p[0] < 0.3f);
    CHECK(ch_px_close(&d, 3, 20, 1, 1, 1, 1));
    CHECK(ch_px_close(&d, 20, 20, 1, 0, 0, 1));
    pc_document_fini(&d);
    e = fxh_wrap_glow(PC_FX_INNER_GLOW, fxh_glow(PC_GLOW_SOFTER, PC_GLOW_SOURCE_EDGE));
    d = fxh_doc(&e, 1);
    ch_px(&d, 10, 20, p);
    CHECK(p[1] > 0.3f);
    CHECK(ch_px_close(&d, 20, 20, 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 5, 20, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static void test_precise_inner_glow_from_center_paints_the_middle(void)
{
    pc_effect e = fxh_wrap_glow(PC_FX_INNER_GLOW, fxh_glow(PC_GLOW_PRECISE, PC_GLOW_SOURCE_CENTER));
    pc_document d = fxh_doc(&e, 1);
    float center[4];
    float edge[4];
    ch_px(&d, 20, 20, center);
    ch_px(&d, 10, 20, edge);
    CHECK(center[1] > 0.9f && center[0] < 0.1f);
    CHECK(edge[0] > center[0] && edge[1] < center[1]);
    CHECK(ch_px_close(&d, 5, 20, 1, 1, 1, 1));
    pc_document_fini(&d);
}

static pc_glow gradient_glow(void)
{
    pc_glow g = fxh_glow(PC_GLOW_SOFTER, PC_GLOW_SOURCE_EDGE);
    g.paint.kind = PC_FX_PAINT_GRADIENT;
    g.paint.u.gradient = fxh_gradient(pc_color_rgb(1, 0, 0), pc_color_rgb(0, 0, 1));
    g.size = 8.0f;
    return g;
}

static bool within(const float *a, const float *b, float tol)
{
    int c;
    for (c = 0; c < 4; c++) {
        if (!(fabsf(a[c] - b[c]) < tol)) {
            return false;
        }
    }
    return true;
}

static void test_glow_gradient_follows_the_shape(void)
{
    pc_effect e = fxh_wrap_glow(PC_FX_OUTER_GLOW, gradient_glow());
    pc_document d = fxh_doc(&e, 1);
    float near[4];
    float p[4];
    float o[4];
    const int32_t pts[3][2] = {{30, 20}, {20, 9}, {20, 30}};
    int i;
    ch_px(&d, 9, 20, near);
    for (i = 0; i < 3; i++) {
        ch_px(&d, pts[i][0], pts[i][1], p);
        CHECK(within(p, near, 1.0f / 255.0f));
    }
    CHECK(near[0] > 0.8f && near[2] < 0.2f && near[1] < 0.1f); /* red at the edge */
    ch_px(&d, 5, 20, o);
    CHECK(o[2] > o[0] && o[1] < 0.9f); /* bluer and fading further out */
    pc_document_fini(&d);
    /* An inner glow's gradient also starts at the edge. */
    e = fxh_wrap_glow(PC_FX_INNER_GLOW, gradient_glow());
    d = fxh_doc(&e, 1);
    ch_px(&d, 10, 20, p);
    ch_px(&d, 20, 10, o);
    CHECK(within(p, o, 1.0f / 255.0f));
    CHECK(p[0] > 0.8f && p[2] < 0.2f);
    pc_document_fini(&d);
}

static void test_gradient_overlay_follows_angle_and_reverse(void)
{
    pc_effect e;
    pc_document d;
    float l[4];
    float r[4];
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_GRADIENT_OVERLAY;
    e.u.gradient_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    e.u.gradient_overlay.gradient = fxh_gradient(PC_COLOR_BLACK, PC_COLOR_WHITE);
    e.u.gradient_overlay.gradient.angle = 0.0f;
    d = fxh_doc(&e, 1);
    ch_px(&d, 10, 20, l);
    ch_px(&d, 29, 20, r);
    CHECK(l[0] < 0.1f && r[0] > 0.9f);
    d.layers[1].effects.items[0].u.gradient_overlay.gradient.reverse = true;
    ch_px(&d, 10, 20, l);
    CHECK(l[0] > 0.9f);
    pc_document_fini(&d);
}

static void test_satin_and_bevel_stay_inside_shape(void)
{
    pc_effect e[2];
    pc_bevel b = fxh_bevel(PC_BEVEL_INNER, PC_BEVEL_SMOOTH);
    pc_document d;
    float top[4];
    float bot[4];
    memset(&e[0], 0, sizeof e[0]);
    e[0].kind = PC_FX_SATIN;
    e[0].u.satin.common = pc_fx_common_new(PC_BLEND_MULTIPLY, 1.0f);
    e[0].u.satin.color = PC_COLOR_BLACK;
    e[0].u.satin.angle = 19.0f;
    e[0].u.satin.distance = 4.0f;
    e[0].u.satin.size = 4.0f;
    e[0].u.satin.anti_alias = true;
    b.size = 4.0f;
    e[1] = fxh_wrap_bevel(b);
    d = fxh_doc(e, 2);
    CHECK(ch_px_close(&d, 5, 5, 1, 1, 1, 1));
    CHECK(ch_px_close(&d, 35, 20, 1, 1, 1, 1));
    CHECK(ch_px_close(&d, 20, 35, 1, 1, 1, 1));
    /* Light from the top: the top bevel edge is brighter than the bottom one. */
    ch_px(&d, 20, 11, top);
    ch_px(&d, 20, 28, bot);
    CHECK(top[1] > bot[1]);
    pc_document_fini(&d);
}

static void test_clipped_layer_effects_are_clipped_to_base(void)
{
    pc_document d = ch_doc_white(40, 40);
    pc_effect e = fxh_stroke(3.0f, PC_STROKE_OUTSIDE);
    pc_layer *c;
    ch_push(&d, ch_solid("base", pc_rect_make(10, 10, 30, 30), 1, 0, 0, 1));
    c = ch_push(&d, ch_solid("clip", pc_rect_make(20, 10, 40, 30), 0, 1, 0, 1));
    c->clipped = true;
    fxh_add(c, &e, 1);
    CHECK(ch_px_close(&d, 18, 20, 0, 0, 1, 1)); /* stroke inside base */
    CHECK(ch_px_close(&d, 20, 8, 1, 1, 1, 1));  /* stroke clipped outside base */
    pc_document_fini(&d);
}

static void test_group_effects_apply_to_group_shape(void)
{
    pc_document d = ch_doc_white(40, 40);
    pc_layer kids[2];
    pc_effect e = fxh_stroke(2.0f, PC_STROKE_OUTSIDE);
    pc_layer *g;
    kids[0] = ch_solid("a", pc_rect_make(10, 10, 20, 30), 1, 0, 0, 1);
    kids[1] = ch_solid("b", pc_rect_make(20, 10, 30, 30), 1, 0, 0, 1);
    g = ch_push(&d, ch_group("g", kids, 2));
    fxh_add(g, &e, 1);
    CHECK(ch_px_close(&d, 9, 20, 0, 0, 1, 1));
    CHECK(ch_px_close(&d, 20, 20, 1, 0, 0, 1)); /* no stroke at the seam between children */
    pc_document_fini(&d);
}

static void test_effects_render_identically_in_tiles(void)
{
    const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const int32_t tiles[3] = {1, 7, 33};
    int k;
    for (k = 0; k < 3; k++) {
        pc_shadow s = fxh_shadow(4.0f, 120.0f);
        pc_effect e[2];
        pc_document d;
        pc_compose_buffer full;
        size_t i;
        int t;
        s.spread = 0.0f;
        s.size = 5.0f;
        e[0] = fxh_wrap_shadow(PC_FX_DROP_SHADOW, s);
        e[1] = fxh_stroke(2.0f, PC_STROKE_OUTSIDE);
        d = fxh_doc(e, 2);
        d.depth = depths[k];
        for (i = 0; i < d.nlayers; i++) {
            pc_surface *sf = pc_layer_surface_mut(&d.layers[i]);
            pc_surface conv;
            pc_pixel_format f;
            if (sf == NULL) {
                continue;
            }
            f = pc_surface_format(sf);
            f.sample = depths[k];
            CHECK(pc_surface_convert(sf, f, &conv, NULL) == PC_OK);
            pc_surface_fini(sf);
            *sf = conv;
        }
        CHECK(pc_compose_flatten(&d, &full, NULL) == PC_OK);
        for (t = 0; t < 3; t++) {
            pc_compose_buffer b = ch_render_tiled(&d, pc_document_bounds(&d), tiles[t]);
            CHECK(pc_compose_buffer_eq(&b, &full));
            pc_compose_buffer_fini(&b);
        }
        pc_compose_buffer_fini(&full);
        pc_document_fini(&d);
    }
}

static void test_effect_maps_are_cached_and_invalidated_by_pixel_changes(void)
{
    pc_document doc = ch_doc_white(64, 64);
    pc_layer *l = ch_push(&doc, ch_solid("fx", pc_rect_make(16, 16, 48, 48), 1, 0, 0, 1));
    pc_effect e;
    pc_cmp_ctx_store cs;
    pc_cmp_fx_ref *a = NULL;
    pc_cmp_fx_ref *b = NULL;
    pc_cmp_fx_ref *c = NULL;
    pc_compose_buffer first;
    pc_compose_buffer again;
    pc_compose_buffer tiled;
    const float blue[4] = {0, 0, 1, 1};
    size_t i;
    bool close_all = true;
    pc_effect_default_drop_shadow(&e);
    fxh_add(l, &e, 1);
    CHECK(pc_cmp_ctx_init_doc(&cs, &doc, NULL, NULL) == PC_OK);
    CHECK(pc_cmp_effect_maps(&doc.layers[1], &cs.cx, &a, NULL) == PC_OK);
    CHECK(pc_cmp_effect_maps(&doc.layers[1], &cs.cx, &b, NULL) == PC_OK);
    CHECK(a != NULL && pc_cmp_fx_maps(a) == pc_cmp_fx_maps(b)); /* second request hits the cache */
    pc_cmp_fx_ref_release(b);
    pc_cmp_ctx_fini(&cs);
    CHECK(pc_compose_flatten(&doc, &first, NULL) == PC_OK);
    /* Editing the layer's pixels changes its tiles, so the maps are rebuilt. */
    ch_fill(&doc.layers[1], pc_rect_make(8, 8, 20, 20), blue, 4);
    CHECK(pc_cmp_ctx_init_doc(&cs, &doc, NULL, NULL) == PC_OK);
    CHECK(pc_cmp_effect_maps(&doc.layers[1], &cs.cx, &c, NULL) == PC_OK);
    CHECK(pc_cmp_fx_maps(a) != pc_cmp_fx_maps(c)); /* pixel edit invalidates */
    pc_cmp_fx_ref_release(c);
    pc_cmp_fx_ref_release(a);
    pc_cmp_ctx_fini(&cs);
    CHECK(pc_compose_flatten(&doc, &again, NULL) == PC_OK);
    tiled = ch_render_tiled(&doc, pc_document_bounds(&doc), 16);
    for (i = 0; i < again.len && i < tiled.len; i++) {
        close_all = close_all && ch_close4(again.px[i], tiled.px[i]);
    }
    CHECK(close_all && again.len == tiled.len);
    CHECK(!pc_compose_buffer_eq(&first, &again));
    pc_compose_buffer_fini(&first);
    pc_compose_buffer_fini(&again);
    pc_compose_buffer_fini(&tiled);
    pc_document_fini(&doc);

    /* Tileless masks differ only in their defaults; reusing their maps leaves a ghost shadow. */
    {
        pc_effect s = fxh_wrap_shadow(PC_FX_DROP_SHADOW, fxh_shadow(5.0f, 90.0f));
        pc_document masked = fxh_doc(&s, 1);
        pc_document cold;
        pc_compose_buffer warm;
        pc_compose_buffer coldb;
        pc_layer tmp;
        pc_layer_mask_reveal_all(&masked.layers[1].mask);
        masked.layers[1].has_mask = true;
        CHECK(ch_px_close(&masked, 20, 32, 0, 0, 0, 1));
        pc_layer_mask_fini(&masked.layers[1].mask);
        pc_layer_mask_hide_all(&masked.layers[1].mask);
        CHECK(pc_compose_flatten(&masked, &warm, NULL) == PC_OK);
        CHECK(pc_document_clone(&masked, &cold, NULL) == PC_OK);
        tmp = ch_raster("uncached", PC_PIXEL_FORMAT_RGBA8);
        cold.layers[1].id = tmp.id;
        pc_layer_fini(&tmp);
        CHECK(pc_compose_flatten(&cold, &coldb, NULL) == PC_OK);
        CHECK(pc_compose_buffer_eq(&warm, &coldb)); /* default-only mask changes must invalidate */
        CHECK(ch_px_close(&masked, 20, 32, 1, 1, 1, 1));
        pc_compose_buffer_fini(&warm);
        pc_compose_buffer_fini(&coldb);
        pc_document_fini(&cold);
        pc_document_fini(&masked);
    }
}

static void test_stroke_corners_follow_the_5x5_chamfer_metric(void)
{
    pc_effect e = fxh_stroke(3.0f, PC_STROKE_OUTSIDE);
    pc_document d = fxh_doc(&e, 1);
    float p[4];
    ch_px(&d, 9, 7, p); /* offset (1, 3) from the corner pixel: sqrt 5 + 1 */
    CHECK(fabsf(1.0f - p[0] - 0.764f) < 0.01f);
    ch_px(&d, 8, 8, p); /* (2, 2) stays Euclidean */
    CHECK(fabsf(1.0f - p[0] - 1.0f) < 0.01f);
    pc_document_fini(&d);
}

static void test_inside_stroke_respects_partial_edge_coverage(void)
{
    pc_document d = ch_doc_white(40, 40);
    pc_layer *l = ch_push(&d, ch_solid("sq", pc_rect_make(10, 10, 30, 30), 1, 0, 0, 1));
    const float edge_px[4] = {1, 0, 0, 0.25f};
    pc_effect e = fxh_stroke(3.0f, PC_STROKE_INSIDE);
    float p[4];
    ch_fill(l, pc_rect_make(10, 10, 11, 30), edge_px, 4);
    fxh_add(l, &e, 1);
    CHECK(ch_px_close(&d, 10, 20, 0.75f, 0.75f, 1, 1)); /* blue stroke at 25 % over white */
    ch_px(&d, 13, 20, p);
    CHECK(fabsf(p[2] - 0.75f) < 0.02f && fabsf(p[0] - 0.25f) < 0.02f);
    pc_document_fini(&d);
}

static void emboss_px(bool text, bool fx, float out[4])
{
    pc_document d = ch_doc_white(40, 40);
    pc_layer l = ch_solid("sq", pc_rect_make(10, 10, 30, 30), 0.2f, 0.1f, 0.6f, 1.0f);
    const float half[4] = {0.2f, 0.1f, 0.6f, 0.5f};
    pc_bevel b = fxh_bevel(PC_BEVEL_EMBOSS, PC_BEVEL_SMOOTH);
    ch_fill(&l, pc_rect_make(10, 10, 30, 11), half, 4);
    if (text) {
        pc_layer_content c;
        pc_layer t;
        memset(&c, 0, sizeof c);
        c.kind = PC_LAYER_TEXT;
        pc_text_layer_default(&c.u.text);
        CHECK(pc_surface_clone(pc_layer_surface(&l), &c.u.text.cache, NULL) == PC_OK);
        c.u.text.has_cache = true;
        CHECK(pc_layer_init(&t, "t", &c, NULL) == PC_OK);
        pc_layer_fini(&l);
        l = t;
    }
    if (fx) {
        pc_effect e;
        b.size = 6.0f;
        b.highlight = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
        b.shadow = pc_fx_common_new(PC_BLEND_MULTIPLY, 0.0f);
        e = fxh_wrap_bevel(b);
        fxh_add(&l, &e, 1);
    }
    ch_push(&d, l);
    ch_px(&d, 20, 10, out);
    pc_document_fini(&d);
}

static void test_emboss_shades_the_composited_layer(void)
{
    float ct[4];
    float cr[4];
    float rt[4];
    float rr[4];
    int c;
    emboss_px(true, false, ct);
    emboss_px(false, false, cr);
    emboss_px(true, true, rt);
    emboss_px(false, true, rr);
    CHECK(rr[1] > cr[1] + 0.05f);        /* the top edge is lit */
    CHECK(fabsf(ct[1] - cr[1]) > 0.02f); /* text gamma changes the edge */
    for (c = 0; c < 3; c++) {
        float want = (1.0f - ct[c]) / (1.0f - cr[c]);
        CHECK(fabsf((1.0f - rt[c]) / (1.0f - rr[c]) - want) < 2e-3f);
    }
}

static void test_interior_effects_keep_the_layer_alpha(void)
{
    pc_document d = ch_doc(40, 40, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer *l = ch_push(&d, ch_solid("sq", pc_rect_make(10, 10, 30, 30), 1, 0, 0, 0.5f));
    pc_effect e = fxh_color_overlay(0, 1, 0);
    fxh_add(l, &e, 1);
    CHECK(ch_px_close(&d, 20, 20, 0, 1, 0, 0.5f));
    pc_document_fini(&d);
}

static void test_upper_stroke_blends_with_the_backdrop_not_the_lower_stroke(void)
{
    pc_effect e[2];
    pc_document d;
    e[0] = fxh_stroke(3.0f, PC_STROKE_OUTSIDE);
    e[0].u.stroke.common = pc_fx_common_new(PC_BLEND_MULTIPLY, 1.0f);
    e[0].u.stroke.paint.u.color = pc_color_rgb(1, 1, 0);
    e[1] = fxh_stroke(8.0f, PC_STROKE_OUTSIDE);
    d = fxh_doc(e, 2);
    CHECK(ch_px_close(&d, 8, 20, 1, 1, 0, 1));
    CHECK(ch_px_close(&d, 4, 20, 0, 0, 1, 1));
    pc_document_fini(&d);
}

static void test_shadow_knockout_needs_see_through_fill(void)
{
    pc_document d = ch_doc(40, 40, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer *l;
    const float half[4] = {1, 0, 0, 0.5f};
    pc_shadow s = fxh_shadow(0.0f, 0.0f);
    pc_effect e;
    float on[4];
    float off[4];
    ch_push(&d, ch_solid("bg", pc_rect_make(0, 0, 40, 40), 1, 1, 1, 1));
    l = ch_push(&d, ch_solid("sq", pc_rect_make(10, 10, 30, 30), 1, 0, 0, 1));
    ch_fill(l, pc_rect_make(10, 10, 11, 30), half, 4);
    s.size = 4.0f;
    e = fxh_wrap_shadow(PC_FX_DROP_SHADOW, s);
    fxh_add(l, &e, 1);
    ch_px(&d, 10, 20, on);
    d.layers[1].effects.items[0].u.shadow.knocks_out = false;
    ch_px(&d, 10, 20, off);
    CHECK(ch_close4(on, off));
    pc_document_fini(&d);
}

static void test_linked_pattern_overlay_anchors_at_the_effects_reference_point(void)
{
    pc_surface tile;
    const float red[4] = {1, 0, 0, 1};
    const float blue[4] = {0, 0, 1, 1};
    pc_pattern pat;
    pc_effect e;
    pc_document d;
    pc_surface_init(&tile, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&tile, pc_rect_make(0, 0, 1, 1), red, 4, NULL) == PC_OK);
    CHECK(pc_surface_fill_rect(&tile, pc_rect_make(1, 0, 2, 1), blue, 4, NULL) == PC_OK);
    CHECK(pc_pattern_new("rb", 2, &tile, 2, 1, &pat, NULL) == PC_OK);
    memset(&e, 0, sizeof e);
    e.kind = PC_FX_PATTERN_OVERLAY;
    e.u.pattern_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    CHECK(pc_str_set_c(&e.u.pattern_overlay.name, "rb", NULL) == PC_OK);
    CHECK(pc_str_clone(&pat.id, &e.u.pattern_overlay.id, NULL) == PC_OK);
    e.u.pattern_overlay.scale = 1.0f;
    e.u.pattern_overlay.link = true;
    d = fxh_doc(&e, 1);
    d.patterns = malloc(sizeof pat);
    CHECK(d.patterns != NULL);
    if (d.patterns == NULL) {
        pc_pattern_fini(&pat);
        pc_document_fini(&d);
        return;
    }
    d.patterns[0] = pat;
    d.npatterns = 1;
    /* Without a reference point the layer's top-left (10) anchors the tiling. */
    CHECK(ch_px_close(&d, 10, 20, 1, 0, 0, 1));
    d.layers[1].effects.has_reference = true;
    d.layers[1].effects.reference[0] = 11.0;
    d.layers[1].effects.reference[1] = 0.0;
    CHECK(ch_px_close(&d, 11, 20, 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 12, 20, 0, 0, 1, 1));
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_edt_is_exact);
    RUN(test_edt_two_seeds);
    RUN(test_blur_preserves_mass);
    RUN(test_gradient_geometry);
    RUN(test_effect_gradients_snap_end_points_and_sample_corners);
    RUN(test_chamfer_uses_euclidean_step_lengths);
    RUN(test_spread_dilates_with_the_chamfer_metric);
    RUN(test_running_sum_tent_matches_the_kernel);
    RUN(test_tent_kernel_is_two_boxes);
    RUN(test_smooth_inner_bevel_lights_the_top_edge_inside_only);
    RUN(test_emboss_paints_both_sides_and_pillow_flips_the_outside);
    RUN(test_emboss_width_rounds_up_and_shades_past_the_blur);
    RUN(test_chisel_hard_has_flat_facets);
    RUN(test_bevel_contour_and_texture_shape_the_height);
    RUN(test_glow_range_stretches_the_contour);
    RUN(test_offsets_follow_light);
    RUN(test_shadow_noise_speckles_soft_coverage_deterministically);
    RUN(test_glow_noise_follows_the_contour_and_range);
    RUN(test_outside_stroke_width);
    RUN(test_outside_stroke_at_zero_fill_leaves_the_interior_clear);
    RUN(test_inside_and_center_strokes);
    RUN(test_master_switch_and_per_effect_enable);
    RUN(test_color_overlay_ignores_fill_opacity_but_not_opacity);
    RUN(test_drop_shadow_falls_away_from_light);
    RUN(test_drop_shadow_uses_global_light_and_knockout);
    RUN(test_soft_shadow_is_blurred_and_bounded);
    RUN(test_inner_shadow_only_inside);
    RUN(test_outer_and_inner_glow_regions);
    RUN(test_precise_inner_glow_from_center_paints_the_middle);
    RUN(test_glow_gradient_follows_the_shape);
    RUN(test_gradient_overlay_follows_angle_and_reverse);
    RUN(test_satin_and_bevel_stay_inside_shape);
    RUN(test_clipped_layer_effects_are_clipped_to_base);
    RUN(test_group_effects_apply_to_group_shape);
    RUN(test_effects_render_identically_in_tiles);
    RUN(test_effect_maps_are_cached_and_invalidated_by_pixel_changes);
    RUN(test_stroke_corners_follow_the_5x5_chamfer_metric);
    RUN(test_inside_stroke_respects_partial_edge_coverage);
    RUN(test_emboss_shades_the_composited_layer);
    RUN(test_interior_effects_keep_the_layer_alpha);
    RUN(test_upper_stroke_blends_with_the_backdrop_not_the_lower_stroke);
    RUN(test_shadow_knockout_needs_see_through_fill);
    RUN(test_linked_pattern_overlay_anchors_at_the_effects_reference_point);
    return pc_t_finish();
}
