/* Headless tests for the Camera Raw scope + preview-navigation model (src/ui/camera_raw_scope.c).
 * Rule 6 render verification lives here too: the drawn geometry (scope point, clip overlay colour,
 * warning masks) is computed by these pure functions, so asserting their output verifies what the
 * shell rasterises. */
#include "harness.h"

#include <math.h>
#include <string.h>

#include "ui/camera_raw_scope.h"
#include "ui/theme.h"

/* Warnings mode paints a highlight clip red (R only) and a shadow clip blue (B only). */
static bool col_eq_red(pc_ui_color c)
{
    return c.r > 0 && c.g == 0 && c.b == 0;
}
static bool col_eq_blue(pc_ui_color c)
{
    return c.b > 0 && c.r == 0 && c.g == 0;
}

static void test_tone_zones(void)
{
    CHECK_EQ_INT(pc_ui_cr_tone_zone_at(0.0f), PC_UI_CR_ZONE_BLACKS);
    CHECK_EQ_INT(pc_ui_cr_tone_zone_at(0.05f), PC_UI_CR_ZONE_BLACKS);
    CHECK_EQ_INT(pc_ui_cr_tone_zone_at(0.1f), PC_UI_CR_ZONE_SHADOWS);
    CHECK_EQ_INT(pc_ui_cr_tone_zone_at(0.5f), PC_UI_CR_ZONE_EXPOSURE);
    CHECK_EQ_INT(pc_ui_cr_tone_zone_at(0.8f), PC_UI_CR_ZONE_HIGHLIGHTS);
    CHECK_EQ_INT(pc_ui_cr_tone_zone_at(0.95f), PC_UI_CR_ZONE_WHITES);
    CHECK_EQ_INT(pc_ui_cr_tone_zone_at(2.0f), PC_UI_CR_ZONE_WHITES);
    /* NaN and negatives clamp to Blacks, never crash. */
    CHECK_EQ_INT(pc_ui_cr_tone_zone_at(NAN), PC_UI_CR_ZONE_BLACKS);
    CHECK_EQ_INT(pc_ui_cr_tone_zone_at(-5.0f), PC_UI_CR_ZONE_BLACKS);

    CHECK(strcmp(pc_ui_cr_tone_zone_name(PC_UI_CR_ZONE_EXPOSURE), "Exposure") == 0);
    CHECK(strcmp(pc_ui_cr_tone_zone_key(PC_UI_CR_ZONE_BLACKS), "blacks") == 0);
    CHECK(strcmp(pc_ui_cr_tone_zone_key(PC_UI_CR_ZONE_WHITES), "whites") == 0);

    float r[2];
    pc_ui_cr_tone_zone_range(PC_UI_CR_ZONE_EXPOSURE, r);
    CHECK_NEAR(r[0], 0.25, 1e-6);
    CHECK_NEAR(r[1], 0.75, 1e-6);

    CHECK_NEAR(pc_ui_cr_tone_zone_limit(PC_UI_CR_ZONE_EXPOSURE), 5.0, 1e-6);
    CHECK_NEAR(pc_ui_cr_tone_zone_limit(PC_UI_CR_ZONE_WHITES), 100.0, 1e-6);
    CHECK_NEAR(pc_ui_cr_tone_zone_clamp(PC_UI_CR_ZONE_EXPOSURE, 99.0f), 5.0, 1e-6);
    CHECK_NEAR(pc_ui_cr_tone_zone_clamp(PC_UI_CR_ZONE_WHITES, -250.0f), -100.0, 1e-6);
    CHECK_NEAR(pc_ui_cr_tone_zone_clamp(PC_UI_CR_ZONE_WHITES, NAN), 0.0, 1e-6);
}

static void test_clip_masks_and_color(void)
{
    pc_algo_rgb_histogram h;
    memset(&h, 0, sizeof h);
    h.shadows[0] = 3;  /* R clipped at the bottom */
    h.shadows[2] = 1;  /* B clipped at the bottom */
    h.highlights[1] = 7; /* G clipped at the top */
    CHECK_EQ_UINT(pc_ui_cr_shadow_mask(&h), 1u | 4u);
    CHECK_EQ_UINT(pc_ui_cr_highlight_mask(&h), 2u);
    CHECK_EQ_UINT(pc_ui_cr_shadow_mask(NULL), 0u);

    pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_ALL[0]);
    pc_ui_color col;
    /* Transparent / non-finite pixels contribute nothing. */
    float transparent[4] = {0.5f, 0.5f, 0.5f, 0.0f};
    CHECK(!pc_ui_cr_clip_color(transparent, PC_UI_CR_CLIP_WARNINGS, 3, &t, &col));
    float nan_px[4] = {NAN, 0.0f, 0.0f, 1.0f};
    CHECK(!pc_ui_cr_clip_color(nan_px, PC_UI_CR_CLIP_WARNINGS, 3, &t, &col));
    /* None mode never paints. */
    float mid[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    CHECK(!pc_ui_cr_clip_color(mid, PC_UI_CR_CLIP_NONE, 3, &t, &col));
    /* A white (top-clipped) pixel in Warnings with highlights on -> red overlay. */
    float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(pc_ui_cr_clip_color(white, PC_UI_CR_CLIP_WARNINGS, 2, &t, &col));
    CHECK(col_eq_red(col));
    /* The same pixel with only shadows armed does not paint (no shadow clip at white). */
    CHECK(!pc_ui_cr_clip_color(white, PC_UI_CR_CLIP_WARNINGS, 1, &t, &col));
    /* A black (bottom-clipped) pixel with shadows armed -> blue overlay. */
    float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    CHECK(pc_ui_cr_clip_color(black, PC_UI_CR_CLIP_WARNINGS, 1, &t, &col));
    CHECK(col_eq_blue(col));
    /* Alt diagnostics always paint something for a visible pixel. */
    CHECK(pc_ui_cr_clip_color(mid, PC_UI_CR_CLIP_SHADOW_CHANNELS, 0, &t, &col));
    CHECK(pc_ui_cr_clip_color(mid, PC_UI_CR_CLIP_HIGHLIGHT_CHANNELS, 0, &t, &col));
}

static void test_scope_geometry(void)
{
    pc_ui_tokens t = pc_ui_tokens_for_kind(PC_UI_THEME_ALL[0]);
    /* Grey centre: saturation 0 -> equal channels at the theme level. */
    pc_ui_color g = pc_ui_cr_scope_hue(&t, 0.0f, 0.0f);
    CHECK_EQ_UINT(g.r, g.g);
    CHECK_EQ_UINT(g.g, g.b);
    /* Pure red hue at full saturation keeps red bright and drops green/blue. */
    pc_ui_color red = pc_ui_cr_scope_hue(&t, 0.0f, 1.0f);
    CHECK(red.r > red.g);
    CHECK(red.r > red.b);

    float rect[4] = {0.0f, 0.0f, 200.0f, 200.0f};
    float centre[2];
    pc_ui_cr_scope_point(rect, true, 0.3f, 0.0f, centre);
    CHECK_NEAR(centre[0], 100.0, 1e-3); /* saturation 0 sits at the centre */
    CHECK_NEAR(centre[1], 100.0, 1e-3);
    /* red_right: hue 0 at full saturation lands at 3 o'clock (centre.x + radius, same y). */
    float rr[2];
    pc_ui_cr_scope_point(rect, true, 0.0f, 1.0f, rr);
    float radius = pc_ui_cr_scope_radius(rect);
    CHECK_NEAR(rr[0], 100.0 + (double)radius, 1e-2);
    CHECK_NEAR(rr[1], 100.0, 1e-2);
    /* Non-finite inputs fall back to the centre, never NaN out. */
    float bad[2];
    pc_ui_cr_scope_point(rect, false, NAN, INFINITY, bad);
    CHECK(isfinite(bad[0]) && isfinite(bad[1]));
}

static void test_readouts(void)
{
    float rgb[3] = {0.0f, 0.5f, 1.0f};
    double out[3];
    pc_ui_cr_readout_rgb(rgb, out);
    CHECK_NEAR(out[0], 0.0, 1e-3);
    CHECK_NEAR(out[1], 127.5, 1e-3);
    CHECK_NEAR(out[2], 255.0, 1e-3);

    float lab[3] = {1.0f, 0.5f, 0.0f};
    pc_ui_cr_readout_lab(lab, out);
    CHECK_NEAR(out[0], 100.0, 1e-3);
    CHECK_NEAR(out[1], -0.5, 1e-3); /* 0.5*255 - 128 = -0.5 */
    CHECK_NEAR(out[2], -128.0, 1e-3);

    char text[64];
    double vals[3] = {10.0, 200.0, 30.0};
    pc_ui_cr_readout_text(vals, false, text, sizeof text);
    CHECK(strcmp(text, "R 10  G 200  B 30") == 0);
    pc_ui_cr_readout_text(vals, true, text, sizeof text);
    CHECK(strncmp(text, "L 10.0", 6) == 0);
    /* Zero-length buffer must not crash. */
    pc_ui_cr_readout_text(vals, false, text, 0);
}

static void test_samplers(void)
{
    pc_ui_camera_raw_scope v;
    memset(&v, 0, sizeof v);
    float p[2] = {0.25f, 0.75f};
    CHECK(pc_ui_cr_scope_add_sampler(&v, p));
    CHECK_EQ_UINT(v.n_samplers, 1u);
    /* Invalid points rejected. */
    float bad[2] = {1.5f, 0.5f};
    CHECK(!pc_ui_cr_scope_add_sampler(&v, bad));
    float nanp[2] = {NAN, 0.5f};
    CHECK(!pc_ui_cr_scope_add_sampler(&v, nanp));
    CHECK_EQ_UINT(v.n_samplers, 1u);
    /* Cap at PC_UI_CR_MAX_SAMPLERS. */
    for (int i = 0; i < 20; i++) {
        float q[2] = {(float)(i % 10) / 10.0f, 0.5f};
        pc_ui_cr_scope_add_sampler(&v, q);
    }
    CHECK_EQ_UINT(v.n_samplers, (unsigned)PC_UI_CR_MAX_SAMPLERS);
    /* Move + remove. */
    float m[2] = {0.1f, 0.1f};
    CHECK(pc_ui_cr_scope_move_sampler(&v, 0, m));
    CHECK_NEAR(v.samplers[0][0], 0.1, 1e-6);
    CHECK(!pc_ui_cr_scope_move_sampler(&v, 99, m));
    CHECK(pc_ui_cr_scope_remove_sampler(&v, 0));
    CHECK_EQ_UINT(v.n_samplers, (unsigned)(PC_UI_CR_MAX_SAMPLERS - 1));
    CHECK(!pc_ui_cr_scope_remove_sampler(&v, 99));
    pc_ui_cr_scope_clear_samplers(&v);
    CHECK_EQ_UINT(v.n_samplers, 0u);

    /* Sanitize drops invalid points and over-cap counts even if planted directly. */
    pc_ui_cr_scope_add_sampler(&v, p);
    pc_ui_cr_scope_add_sampler(&v, m);
    v.samplers[1][0] = 5.0f; /* corrupt */
    pc_ui_cr_scope_sanitize(&v);
    CHECK_EQ_UINT(v.n_samplers, 1u);
    CHECK_NEAR(v.samplers[0][0], 0.25, 1e-6);

    /* Floating rect validity. */
    float ok[4] = {40.0f, 60.0f, 400.0f, 440.0f};
    CHECK(pc_ui_cr_floating_rect_valid(ok));
    float small[4] = {0.0f, 0.0f, 100.0f, 100.0f};
    CHECK(!pc_ui_cr_floating_rect_valid(small));
    float inf_rect[4] = {0.0f, 0.0f, INFINITY, 300.0f};
    CHECK(!pc_ui_cr_floating_rect_valid(inf_rect));
    v.has_floating_rect = true;
    memcpy(v.floating_rect, inf_rect, sizeof inf_rect);
    pc_ui_cr_scope_sanitize(&v);
    CHECK(!v.has_floating_rect);

    free(v.samplers);
}

static bool rect_contains(const float outer[4], const float inner[4])
{
    return outer[0] <= inner[0] + 1e-3f && outer[1] <= inner[1] + 1e-3f &&
           outer[0] + outer[2] >= inner[0] + inner[2] - 1e-3f &&
           outer[1] + outer[3] >= inner[1] + inner[3] - 1e-3f;
}

static void test_preview_nav(void)
{
    float viewport[4] = {10.0f, 20.0f, 800.0f, 600.0f};
    float size[2] = {1600.0f, 1200.0f};
    pc_ui_camera_raw_preview p;
    memset(&p, 0, sizeof p);
    pc_ui_cr_preview_fit(&p);

    float initial[4];
    pc_ui_cr_preview_image_rect(&p, viewport, size, initial);
    /* Fit maps the whole image into the viewport exactly. */
    CHECK_NEAR(initial[0], 10.0, 1e-3);
    CHECK_NEAR(initial[1], 20.0, 1e-3);
    CHECK_NEAR(initial[2], 800.0, 1e-3);
    CHECK_NEAR(initial[3], 600.0, 1e-3);

    float at[2] = {310.0f, 270.0f};
    float point[2] = {(at[0] - initial[0]) / initial[2], (at[1] - initial[1]) / initial[3]};
    pc_ui_cr_preview_zoom_at(&p, 1.0f, at, viewport, size);
    float enlarged[4];
    pc_ui_cr_preview_image_rect(&p, viewport, size, enlarged);
    float got[2] = {(at[0] - enlarged[0]) / enlarged[2], (at[1] - enlarged[1]) / enlarged[3]};
    CHECK_NEAR(got[0], (double)point[0], 1e-4);
    CHECK_NEAR(got[1], (double)point[1], 1e-4);

    /* A huge pan clamps so the enlarged image still covers the viewport. */
    float big[2] = {1e9f, -1e9f};
    pc_ui_cr_preview_pan(&p, big, viewport, size);
    float panned[4];
    pc_ui_cr_preview_image_rect(&p, viewport, size, panned);
    CHECK(rect_contains(panned, viewport));

    pc_ui_cr_preview_fit(&p);
    float refit[4];
    pc_ui_cr_preview_image_rect(&p, viewport, size, refit);
    CHECK_NEAR(refit[0], (double)initial[0], 1e-3);
    CHECK_NEAR(refit[2], (double)initial[2], 1e-3);
}

static void test_preview_robustness(void)
{
    float viewport[4] = {0.0f, 0.0f, 600.0f, 400.0f};
    float size[2] = {40.0f, 20.0f};
    pc_ui_camera_raw_preview p;
    memset(&p, 0, sizeof p);
    p.has_zoom = true;
    p.zoom = NAN;
    p.center[0] = INFINITY;
    p.center[1] = NAN;
    p.hand = true;

    float rect[4];
    pc_ui_cr_preview_image_rect(&p, viewport, size, rect);
    CHECK(isfinite(rect[0]) && isfinite(rect[1]) && isfinite(rect[2]) && isfinite(rect[3]));

    float at[2] = {100.0f, 100.0f};
    pc_ui_cr_preview_zoom_at(&p, 1.0f, at, viewport, size);
    pc_ui_cr_preview_image_rect(&p, viewport, size, rect);
    /* Tiny image stays centred in the viewport. */
    CHECK_NEAR(rect[0] + rect[2] * 0.5f, 300.0, 1e-2);
    CHECK_NEAR(rect[1] + rect[3] * 0.5f, 200.0, 1e-2);

    /* Non-finite zoom/pan are no-ops. */
    float c0 = p.center[0], c1 = p.center[1];
    bool had = p.has_zoom;
    float z0 = p.zoom;
    pc_ui_cr_preview_zoom_at(&p, INFINITY, at, viewport, size);
    float nanpan[2] = {NAN, 1.0f};
    pc_ui_cr_preview_pan(&p, nanpan, viewport, size);
    CHECK_NEAR(p.center[0], (double)c0, 1e-6);
    CHECK_NEAR(p.center[1], (double)c1, 1e-6);
    CHECK(p.has_zoom == had);
    CHECK_NEAR(p.zoom, (double)z0, 1e-6);
}

int main(int argc, char **argv)
{
    RUN(test_tone_zones);
    RUN(test_clip_masks_and_color);
    RUN(test_scope_geometry);
    RUN(test_readouts);
    RUN(test_samplers);
    RUN(test_preview_nav);
    RUN(test_preview_robustness);
    return pc_t_finish();
}
