/* Layer effects (C3), the rest: crates/compose/tests/artboard.rs (the crate's integration test),
 * the effect-map cache of fx_cache.c (budget, bytes, purge, eviction), the two deadlock tests of
 * tests.rs (#276), and never-crash checks: hostile effect values (NaN / inf / huge sizes,
 * distances, spreads, angles, ranges, depths), odd contours, hostile gradients and patterns,
 * deep groups with effects, and misuse of the map API. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fxh.h"

/* ---------- artboard.rs ---------- */

static pc_document artboard_doc(pc_sample_type depth, pc_artboard_background bg, pc_blend_mode blend)
{
    pc_document d = ch_doc(40, 20, PC_COLOR_MODE_RGB, depth);
    pc_pixel_format fmt = {PC_COLOR_MODE_RGB, depth, true};
    pc_layer red = ch_raster("red", fmt);
    const float px[4] = {1, 0, 0, 1};
    pc_layer *g;
    ch_fill(&red, pc_rect_make(0, 0, 40, 10), px, 4);
    g = ch_push(&d, ch_group("Artboard 1", &red, 1));
    g->blend = blend;
    pc_artboard_init(&g->content.u.group.artboard, pc_rect_make(10, 5, 30, 15));
    g->content.u.group.artboard.background = bg;
    g->content.u.group.has_artboard = true;
    return d;
}

static bool at_close(const pc_compose_buffer *b, int32_t x, int32_t y, float r, float g, float bl, float a)
{
    const float want[4] = {r, g, bl, a};
    return ch_close4(pc_compose_buffer_get(b, x, y), want);
}

static void test_contents_are_clipped_and_background_painted(void)
{
    const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const pc_blend_mode blends[2] = {PC_BLEND_PASS_THROUGH, PC_BLEND_NORMAL};
    pc_artboard_background white = {PC_ARTBOARD_WHITE, PC_COLOR_WHITE};
    pc_document original;
    pc_document proxy;
    pc_compose_buffer source;
    pc_compose_buffer reduced;
    int k;
    int b;
    int32_t x;
    int32_t y;
    for (k = 0; k < 3; k++) {
        for (b = 0; b < 2; b++) {
            pc_document d = artboard_doc(depths[k], white, blends[b]);
            pc_compose_buffer buf;
            pc_compose_buffer tiled;
            CHECK(pc_compose_render(&d, pc_document_bounds(&d), &buf, NULL) == PC_OK);
            /* Outside the board: nothing (the red layer is clipped away). */
            CHECK(at_close(&buf, 2, 2, 0, 0, 0, 0));
            CHECK(at_close(&buf, 35, 8, 0, 0, 0, 0));
            /* Inside, over the red: red; inside below it: the white board background. */
            CHECK(at_close(&buf, 15, 7, 1, 0, 0, 1));
            CHECK(at_close(&buf, 15, 12, 1, 1, 1, 1));
            /* Edges are half-open. */
            CHECK(at_close(&buf, 29, 14, 1, 1, 1, 1));
            CHECK(at_close(&buf, 30, 14, 0, 0, 0, 0));
            /* Tiled rendering agrees with one pass. */
            tiled = ch_render_tiled(&d, pc_document_bounds(&d), 7);
            CHECK(pc_compose_buffer_eq(&tiled, &buf));
            pc_compose_buffer_fini(&tiled);
            pc_compose_buffer_fini(&buf);
            pc_document_fini(&d);
        }
    }
    /* Proxy pixel (x, y) samples (4x, 4y); unaligned half-open artboard edges must agree. */
    original = artboard_doc(PC_SAMPLE_TYPE_F32, white, PC_BLEND_PASS_THROUGH);
    CHECK(pc_compose_render(&original, pc_document_bounds(&original), &source, NULL) == PC_OK);
    CHECK(pc_compose_proxy_document(&original, 4, &proxy, NULL) == PC_OK);
    CHECK(pc_compose_render(&proxy, pc_document_bounds(&proxy), &reduced, NULL) == PC_OK);
    for (y = 0; y < (int32_t)proxy.size.height; y++) {
        for (x = 0; x < (int32_t)proxy.size.width; x++) {
            CHECK(ch_close4(pc_compose_buffer_get(&reduced, x, y),
                            pc_compose_buffer_get(&source, 4 * x, 4 * y)));
        }
    }
    pc_compose_buffer_fini(&source);
    pc_compose_buffer_fini(&reduced);
    pc_document_fini(&proxy);
    pc_document_fini(&original);
}

static void board_px(pc_artboard_background bg, float out[4])
{
    pc_document d = artboard_doc(PC_SAMPLE_TYPE_U8, bg, PC_BLEND_PASS_THROUGH);
    pc_compose_buffer b;
    memset(out, 0, 4 * sizeof(float));
    CHECK(pc_compose_render(&d, pc_rect_from_xywh(20, 12, 1, 1), &b, NULL) == PC_OK);
    if (b.len == 1) {
        memcpy(out, b.px[0], 4 * sizeof(float));
    }
    pc_compose_buffer_fini(&b);
    pc_document_fini(&d);
}

static void test_background_kinds(void)
{
    pc_artboard_background black = {PC_ARTBOARD_BLACK, PC_COLOR_BLACK};
    pc_artboard_background clear = {PC_ARTBOARD_TRANSPARENT, PC_COLOR_BLACK};
    pc_artboard_background blue = {PC_ARTBOARD_CUSTOM, PC_COLOR_BLACK};
    const float k[4] = {0, 0, 0, 1};
    const float z[4] = {0, 0, 0, 0};
    const float bl[4] = {0, 0, 1, 1};
    float p[4];
    blue.custom = pc_color_rgb(0, 0, 1);
    board_px(black, p);
    CHECK(ch_close4(p, k));
    board_px(clear, p);
    CHECK(ch_close4(p, z));
    board_px(blue, p);
    CHECK(ch_close4(p, bl));
}

static void test_backdrop_outside_the_board_is_untouched(void)
{
    pc_artboard_background clear = {PC_ARTBOARD_TRANSPARENT, PC_COLOR_BLACK};
    pc_document d = artboard_doc(PC_SAMPLE_TYPE_U8, clear, PC_BLEND_PASS_THROUGH);
    pc_layer bg = ch_raster("bg", pc_document_pixel_format(&d));
    const float green[4] = {0, 1, 0, 1};
    pc_layer *grown;
    ch_fill(&bg, pc_document_bounds(&d), green, 4);
    /* Insert at the bottom. */
    grown = realloc(d.layers, (d.nlayers + 1) * sizeof *grown);
    CHECK(grown != NULL);
    if (grown == NULL) {
        pc_layer_fini(&bg);
        pc_document_fini(&d);
        return;
    }
    d.layers = grown;
    memmove(d.layers + 1, d.layers, d.nlayers * sizeof *d.layers);
    d.layers[0] = bg;
    d.nlayers++;
    CHECK(ch_px_close(&d, 2, 2, 0, 1, 0, 1));
    CHECK(ch_px_close(&d, 15, 7, 1, 0, 0, 1));
    CHECK(ch_px_close(&d, 15, 12, 0, 1, 0, 1));
    pc_document_fini(&d);
}

/* ---------- effect-map cache (fx_cache.c) ---------- */

static pc_document big_fx_doc(int nlayers)
{
    pc_document d = ch_doc_white(512, 512);
    int i;
    for (i = 0; i < nlayers; i++) {
        pc_effect e[2];
        pc_layer *l = ch_push(
            &d, ch_solid("fx", pc_rect_make(20 + i * 40, 20, 400 + i * 20, 480), 0.2f, 0.4f, 0.9f, 1));
        e[0] = fxh_stroke(3.0f + (float)i, PC_STROKE_OUTSIDE);
        pc_effect_default_drop_shadow(&e[1]);
        fxh_add(l, e, 2);
    }
    return d;
}

static void test_effect_cache_budget_bytes_and_purge(void)
{
    size_t old = pc_compose_effect_cache_budget();
    pc_document d = big_fx_doc(4);
    pc_compose_buffer b;
    size_t all;
    size_t small;
    pc_compose_set_effect_cache_budget(0);
    CHECK(pc_compose_effect_cache_budget() == ((size_t)1 << 20)); /* clamped to 1 MiB */
    pc_compose_set_effect_cache_budget(PC_COMPOSE_DEFAULT_FX_CACHE_BUDGET);
    (void)pc_compose_purge_effect_cache();
    CHECK(pc_compose_effect_cache_bytes() == 0);
    CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK);
    pc_compose_buffer_fini(&b);
    all = pc_compose_effect_cache_bytes();
    CHECK(all > ((size_t)4 << 20)); /* four layers' maps kept */
    CHECK(pc_compose_purge_effect_cache() == all);
    CHECK(pc_compose_effect_cache_bytes() == 0);
    /* A small budget evicts the oldest maps (never the one just used). */
    pc_compose_set_effect_cache_budget((size_t)1 << 20);
    CHECK(pc_compose_flatten(&d, &b, NULL) == PC_OK);
    small = pc_compose_effect_cache_bytes();
    CHECK(small > 0 && small < all);
    /* Rendering through an evicting cache gives the same pixels. */
    {
        pc_compose_buffer again;
        (void)pc_compose_purge_effect_cache();
        pc_compose_set_effect_cache_budget(PC_COMPOSE_DEFAULT_FX_CACHE_BUDGET);
        CHECK(pc_compose_flatten(&d, &again, NULL) == PC_OK);
        CHECK(pc_compose_buffer_eq(&again, &b));
        pc_compose_buffer_fini(&again);
    }
    pc_compose_buffer_fini(&b);
    (void)pc_compose_purge_effect_cache();
    pc_compose_set_effect_cache_budget(old);
    pc_document_fini(&d);
}

/* ---------- deadlocks (#276) ---------- */

typedef struct watch {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    bool done;
    void (*fn)(void);
} watch;

static void *watch_run(void *arg)
{
    watch *w = arg;
    w->fn();
    pthread_mutex_lock(&w->lock);
    w->done = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
    return NULL;
}

/* Runs fn on its own thread; a hang past `seconds` fails (and exits: the thread can't be killed). */
static void with_watchdog(void (*fn)(void), int seconds)
{
    watch w;
    pthread_t t;
    struct timespec until;
    int rc = 0;
    pthread_mutex_init(&w.lock, NULL);
    pthread_cond_init(&w.cond, NULL);
    w.done = false;
    w.fn = fn;
    CHECK(pthread_create(&t, NULL, watch_run, &w) == 0);
    (void)clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += seconds;
    pthread_mutex_lock(&w.lock);
    while (!w.done && rc == 0) {
        rc = pthread_cond_timedwait(&w.cond, &w.lock, &until);
    }
    pthread_mutex_unlock(&w.lock);
    if (!w.done) {
        PC_T_FAIL("effect-map rendering deadlocked (#276)");
        (void)fprintf(stderr, "deadlock: giving up\n");
        _Exit(1); /* test only: a hung worker can't be joined */
    }
    (void)pthread_join(t, NULL);
    pthread_cond_destroy(&w.cond);
    pthread_mutex_destroy(&w.lock);
}

static void big_shadow_render(void)
{
    pc_document d = ch_doc_white(900, 700);
    pc_layer *l = ch_push(&d, ch_solid("fx", pc_rect_make(100, 100, 800, 600), 0.2f, 0.4f, 0.9f, 1));
    pc_effect e;
    pc_compose_buffer out;
    pc_effect_default_drop_shadow(&e);
    e.u.shadow.size = 30.0f;
    fxh_add(l, &e, 1);
    CHECK(pc_compose_render(&d, pc_document_bounds(&d), &out, NULL) == PC_OK);
    CHECK(out.len == (size_t)900 * 700);
    pc_compose_buffer_fini(&out);
    pc_document_fini(&d);
}

static void test_effect_maps_built_inside_parallel_tiles_do_not_deadlock(void)
{
    with_watchdog(big_shadow_render, 120);
}

static pc_layer gradient_fill_layer(const char *name, float angle)
{
    pc_color_stop stops[2];
    pc_layer_content c;
    pc_layer l;
    memset(&c, 0, sizeof c);
    stops[0].pos = 0.0f;
    stops[0].color = PC_COLOR_BLACK;
    stops[1].pos = 1.0f;
    stops[1].color = pc_color_rgb(0.2f, 0.5f, 0.9f);
    c.kind = PC_LAYER_FILL;
    CHECK(pc_fill_gradient(stops, 2, angle, 1.0f, PC_GRADIENT_LINEAR, false, &c.u.fill, NULL) == PC_OK);
    if (pc_layer_init(&l, name, &c, NULL) != PC_OK) {
        abort();
    }
    return l;
}

static void nested_rounds(void)
{
    pc_document d = ch_doc_white(512, 512);
    pc_layer *l;
    pc_effect e;
    pc_glow g;
    pc_compose_buffer first;
    int round;
    ch_push(&d, gradient_fill_layer("base", 30.0f));
    l = ch_push(&d, gradient_fill_layer("shadow", 0.0f));
    pc_effect_default_drop_shadow(&e);
    fxh_add(l, &e, 1);
    l = ch_push(&d, gradient_fill_layer("stroke", 45.0f));
    l->opacity = 0.6f;
    e = fxh_stroke(3.0f, PC_STROKE_INSIDE);
    e.u.stroke.paint.u.color = pc_color_rgb(1, 0, 0);
    fxh_add(l, &e, 1);
    l = ch_push(&d, gradient_fill_layer("glow", 90.0f));
    l->opacity = 0.5f;
    g = fxh_glow(PC_GLOW_SOFTER, PC_GLOW_SOURCE_EDGE);
    g.common = pc_fx_common_new(PC_BLEND_SCREEN, 0.75f);
    g.paint.u.color = pc_color_rgb(1.0f, 1.0f, 0.7f);
    g.size = 9.0f;
    e = fxh_wrap_glow(PC_FX_INNER_GLOW, g);
    fxh_add(l, &e, 1);
    l = ch_push(&d, gradient_fill_layer("top", 60.0f));
    l->opacity = 0.3f;
    first = ch_render_tiled(&d, pc_document_bounds(&d), 64);
    for (round = 0; round < 25; round++) {
        pc_compose_buffer out;
        (void)pc_compose_purge_effect_cache();
        out = ch_render_tiled(&d, pc_document_bounds(&d), 64);
        CHECK(pc_compose_buffer_eq(&out, &first)); /* renders differ between rounds */
        pc_compose_buffer_fini(&out);
    }
    pc_compose_buffer_fini(&first);
    pc_document_fini(&d);
}

static void test_effect_maps_with_nested_parallel_work_do_not_deadlock(void)
{
    with_watchdog(nested_rounds, 120);
}

/* ---------- never crash ---------- */

static const float HOSTILE[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, 600.0f, -3.0f, 0.0f, 1e-30f, 513.0f};
#define NHOSTILE (sizeof HOSTILE / sizeof HOSTILE[0])

static void set_points(pc_contour *c, const float (*p)[2], size_t n)
{
    size_t i;
    c->custom = true;
    c->points.len = n;
    c->points.pts = n > 0 ? calloc(n, sizeof *c->points.pts) : NULL;
    for (i = 0; c->points.pts != NULL && i < n; i++) {
        c->points.pts[i].input = p[i][0];
        c->points.pts[i].output = p[i][1];
    }
}

/* Odd contour k of 6 (Linear, NaN points, a single point, 25 unsorted points, overshoot...). */
static pc_contour odd_contour(int k)
{
    pc_contour c;
    memset(&c, 0, sizeof c);
    if (k == 1) {
        const float p[3][2] = {{NAN, NAN}, {INFINITY, 0.5f}, {0.5f, -INFINITY}};
        set_points(&c, p, 3);
    } else if (k == 2) {
        const float p[1][2] = {{0.5f, 2.0f}};
        set_points(&c, p, 1);
    } else if (k == 3) {
        float p[25][2];
        int i;
        for (i = 0; i < 25; i++) {
            p[i][0] = (float)((i * 7) % 25) / 24.0f;
            p[i][1] = (float)(i % 3) * 3.0f - 2.0f;
        }
        set_points(&c, (const float (*)[2])p, 25);
    } else if (k == 4) {
        const float p[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1e-7f, 0.0f}, {1.0f, 1e30f}};
        set_points(&c, p, 4);
    } else if (k == 5) {
        set_points(&c, NULL, 0);
    }
    return c;
}

/* Every kind of effect with hostile value v in every numeric field. */
static void hostile_effects(pc_layer *l, float v, int variant, const pc_pattern *pat)
{
    pc_effect e[10];
    int i;
    memset(e, 0, sizeof e);
    for (i = 0; i < 2; i++) {
        e[i].kind = i == 0 ? PC_FX_DROP_SHADOW : PC_FX_INNER_SHADOW;
        e[i].u.shadow = fxh_shadow(v, v);
        e[i].u.shadow.size = v;
        e[i].u.shadow.spread = v;
        e[i].u.shadow.noise = v;
        e[i].u.shadow.common.opacity = variant % 2 == 0 ? 1.0f : v;
        e[i].u.shadow.contour = odd_contour((variant + i) % 6);
    }
    for (i = 2; i < 4; i++) {
        e[i].kind = i == 2 ? PC_FX_OUTER_GLOW : PC_FX_INNER_GLOW;
        e[i].u.glow = fxh_glow(variant % 2 == 0 ? PC_GLOW_PRECISE : PC_GLOW_SOFTER,
                               i == 2 ? PC_GLOW_SOURCE_EDGE : PC_GLOW_SOURCE_CENTER);
        e[i].u.glow.size = v;
        e[i].u.glow.spread = v;
        e[i].u.glow.range = v;
        e[i].u.glow.noise = v;
        e[i].u.glow.contour = odd_contour((variant + i) % 6);
        if (variant % 3 == 1) {
            /* A hostile gradient: no stops, NaN geometry. */
            e[i].u.glow.paint.kind = PC_FX_PAINT_GRADIENT;
            memset(&e[i].u.glow.paint.u.gradient, 0, sizeof e[i].u.glow.paint.u.gradient);
            e[i].u.glow.paint.u.gradient.angle = v;
            e[i].u.glow.paint.u.gradient.scale = v;
        }
    }
    e[4] = fxh_stroke(v, (pc_stroke_position)(variant % 3));
    if (variant % 3 == 0) {
        memset(&e[4].u.stroke.paint, 0, sizeof e[4].u.stroke.paint);
        e[4].u.stroke.paint.kind = PC_FX_PAINT_GRADIENT;
        e[4].u.stroke.paint.u.gradient = fxh_gradient(PC_COLOR_BLACK, PC_COLOR_WHITE);
        e[4].u.stroke.paint.u.gradient.stops[0].pos = NAN;
        e[4].u.stroke.paint.u.gradient.angle = v;
        e[4].u.stroke.paint.u.gradient.offset[0] = v;
        e[4].u.stroke.paint.u.gradient.style = (pc_gradient_style)(variant % 5);
    } else if (variant % 3 == 1) {
        memset(&e[4].u.stroke.paint, 0, sizeof e[4].u.stroke.paint);
        e[4].u.stroke.paint.kind = PC_FX_PAINT_PATTERN;
        CHECK(pc_str_clone(&pat->id, &e[4].u.stroke.paint.u.pattern.id, NULL) == PC_OK);
        e[4].u.stroke.paint.u.pattern.scale = v;
    }
    e[5] = fxh_color_overlay(v, 0.5f, -v);
    e[6].kind = PC_FX_GRADIENT_OVERLAY;
    e[6].u.gradient_overlay.common = pc_fx_common_new(PC_BLEND_LAYER_MODES[variant % 27], 1.0f);
    e[6].u.gradient_overlay.gradient = fxh_gradient(PC_COLOR_BLACK, PC_COLOR_WHITE);
    e[6].u.gradient_overlay.gradient.scale = v;
    e[6].u.gradient_overlay.gradient.angle = v;
    e[6].u.gradient_overlay.gradient.style = (pc_gradient_style)((variant + 2) % 5);
    e[7].kind = PC_FX_PATTERN_OVERLAY;
    e[7].u.pattern_overlay.common = pc_fx_common_new(PC_BLEND_NORMAL, 1.0f);
    CHECK(pc_str_set_c(&e[7].u.pattern_overlay.name, variant % 2 == 0 ? "p" : "missing", NULL) == PC_OK);
    e[7].u.pattern_overlay.scale = v;
    e[7].u.pattern_overlay.angle = v;
    e[7].u.pattern_overlay.link = variant % 2 == 0;
    e[7].u.pattern_overlay.phase[0] = v;
    e[7].u.pattern_overlay.phase[1] = v;
    e[8].kind = PC_FX_SATIN;
    e[8].u.satin.common = pc_fx_common_new(PC_BLEND_MULTIPLY, 1.0f);
    e[8].u.satin.angle = v;
    e[8].u.satin.distance = v;
    e[8].u.satin.size = v;
    e[8].u.satin.invert = variant % 2 == 1;
    e[8].u.satin.contour = odd_contour(variant % 6);
    e[9] = fxh_wrap_bevel(fxh_bevel((pc_bevel_style)(variant % 5), (pc_bevel_technique)(variant % 3)));
    e[9].u.bevel.size = v;
    e[9].u.bevel.depth = v;
    e[9].u.bevel.soften = v;
    e[9].u.bevel.angle = v;
    e[9].u.bevel.altitude = v;
    e[9].u.bevel.up = variant % 2 == 0;
    e[9].u.bevel.gloss_contour = odd_contour((variant + 3) % 6);
    e[9].u.bevel.has_contour = true;
    e[9].u.bevel.contour.contour = odd_contour((variant + 1) % 6);
    e[9].u.bevel.contour.range = v;
    e[9].u.bevel.has_texture = true;
    CHECK(pc_str_clone(&pat->name, &e[9].u.bevel.texture.name, NULL) == PC_OK);
    e[9].u.bevel.texture.scale = v;
    e[9].u.bevel.texture.depth = v;
    e[9].u.bevel.texture.link = true;
    e[9].u.bevel.texture.phase[0] = v;
    fxh_add(l, e, 10);
}

static void test_hostile_effect_values_render(void)
{
    const pc_sample_type depths[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    size_t i;
    int failures = 0;
    for (i = 0; i < NHOSTILE; i++) {
        int variant;
        for (variant = 0; variant < 6; variant++) {
            pc_document d = ch_doc(24, 20, variant % 2 == 0 ? PC_COLOR_MODE_RGB : PC_COLOR_MODE_CMYK,
                                   depths[variant % 3]);
            pc_surface tile;
            const float px[4] = {0.5f, 0.2f, 0.9f, 1.0f};
            pc_pattern *pat = malloc(2 * sizeof *pat);
            pc_layer *l;
            pc_compose_buffer b;
            pc_error e;
            pc_status st;
            if (pat == NULL) {
                abort();
            }
            pc_surface_init(&tile, PC_PIXEL_FORMAT_RGBA8);
            CHECK(pc_surface_fill_rect(&tile, pc_rect_make(0, 0, 3, 2), px, 4, NULL) == PC_OK);
            CHECK(pc_pattern_new("p", 1, &tile, 3, 2, &pat[0], NULL) == PC_OK);
            /* An empty pattern (0 x 0) as well. */
            pc_surface_init(&tile, PC_PIXEL_FORMAT_RGBA8);
            if (pc_pattern_new("empty", 5, &tile, 0, 0, &pat[1], NULL) != PC_OK) {
                pc_surface_fini(&tile);
                memset(&pat[1], 0, sizeof pat[1]);
            }
            d.patterns = pat;
            d.npatterns = 2;
            l = ch_push(&d,
                        ch_solid("fx", pc_rect_make(4, 3, 18, 15), 1, 0, 0, variant % 2 == 0 ? 1.0f : 0.6f));
            hostile_effects(l, HOSTILE[i], variant, &pat[variant % 2]);
            l->fill_opacity = variant % 3 == 0 ? HOSTILE[i] : 0.5f;
            l->advanced.blend_interior = variant % 2 == 1;
            l->advanced.transparency_shapes = variant != 4;
            l->effects.has_reference = variant == 5;
            l->effects.reference[0] = (double)HOSTILE[i];
            l->effects.reference[1] = -1e300;
            d.global_light.angle = HOSTILE[i];
            d.global_light.altitude = HOSTILE[(i + 1) % NHOSTILE];
            st = pc_compose_render_tiled(&d, pc_rect_make(-3, -3, 30, 26), 9, NULL, &b, &e);
            if (st != PC_OK) {
                failures++;
                (void)fprintf(stderr, "  hostile %g variant %d: %s\n", (double)HOSTILE[i], variant, e.msg);
            } else {
                pc_compose_buffer_fini(&b);
            }
            st = pc_compose_render_layer(l, pc_rect_make(0, 0, 24, 20), &b, &e);
            if (st == PC_OK) {
                pc_compose_buffer_fini(&b);
            } else {
                failures++;
            }
            pc_document_fini(&d);
        }
    }
    CHECK_EQ_INT(failures, 0);
}

static void test_deep_groups_with_effects(void)
{
    const int depths[3] = {3, 60, 140};
    int k;
    for (k = 0; k < 3; k++) {
        pc_document d = ch_doc_white(16, 16);
        pc_layer cur = ch_solid("leaf", pc_rect_make(4, 4, 12, 12), 1, 0, 0, 1);
        pc_compose_buffer b;
        pc_error e;
        pc_status st;
        int i;
        for (i = 0; i < depths[k]; i++) {
            pc_effect fx = fxh_stroke(1.0f, PC_STROKE_OUTSIDE);
            pc_layer g = ch_group("g", &cur, 1);
            fxh_add(&g, &fx, 1);
            g.blend = i % 2 == 0 ? PC_BLEND_NORMAL : PC_BLEND_PASS_THROUGH;
            cur = g;
        }
        ch_push(&d, cur);
        st = pc_compose_flatten(&d, &b, &e);
        /* Legal depths render; past PC_COMPOSE_MAX_DEPTH the result is an error, never a crash. */
        CHECK(k < 2 ? st == PC_OK : (st == PC_OK || st == PC_ERR_LIMIT));
        if (st == PC_OK) {
            pc_compose_buffer_fini(&b);
        }
        pc_document_fini(&d);
    }
}

static void test_map_api_misuse_fails_cleanly(void)
{
    pc_layer l = ch_solid("l", pc_rect_make(0, 0, 4, 4), 1, 0, 0, 1);
    pc_layer other = ch_solid("o", pc_rect_make(0, 0, 4, 4), 1, 0, 0, 1);
    pc_effect fx[3];
    pc_compose_fx_maps *m = NULL;
    pc_compose_buffer content;
    pc_compose_buffer back;
    pc_compose_buffer small;
    float *shape;
    pc_error e;
    pc_rect r = pc_rect_make(0, 0, 4, 4);
    size_t i;
    fx[0] = fxh_stroke(2.0f, PC_STROKE_CENTER);
    fx[1] = fxh_wrap_bevel(fxh_bevel(PC_BEVEL_EMBOSS, PC_BEVEL_CHISEL_SOFT));
    fx[2] = fxh_wrap_shadow(PC_FX_DROP_SHADOW, fxh_shadow(2.0f, 45.0f));
    fxh_add(&l, fx, 3);
    /* Wrong shape length, missing shape, absurd region. */
    shape = calloc(15, sizeof *shape);
    CHECK(pc_compose_fx_build_maps(&l, shape, 15, r, NULL, NULL, NULL, &m, &e) == PC_ERR_INVALID &&
          m == NULL);
    CHECK(pc_compose_fx_build_maps(&l, NULL, 16, r, NULL, NULL, NULL, &m, &e) == PC_ERR_INVALID && m == NULL);
    CHECK(pc_compose_fx_build_maps(&l, NULL, 0, pc_rect_make(0, 0, 1 << 20, 1 << 20), NULL, NULL, NULL, &m,
                                   &e) == PC_ERR_LIMIT &&
          m == NULL);
    /* Empty regions (no light, no texture context) build empty maps that composite as nothing. */
    CHECK(pc_compose_fx_build_maps(&l, NULL, 0, pc_rect_make(3, 3, 3, 9), NULL, NULL, NULL, &m, &e) == PC_OK);
    CHECK(pc_compose_fx_maps_bytes(m) == 0);
    pc_compose_fx_maps_free(m);
    m = NULL;
    /* Maps built for one layer, composited with another (more effects than maps). */
    shape = calloc(16, sizeof *shape);
    for (i = 0; shape != NULL && i < 16; i++) {
        shape[i] = 1.0f;
    }
    CHECK(pc_compose_fx_build_maps(&other, shape, 16, r, NULL, NULL, NULL, &m, &e) == PC_OK);
    CHECK(pc_compose_buffer_filled(r, (const float[4]){1, 0, 0, 1}, &content, NULL) == PC_OK);
    CHECK(pc_compose_buffer_transparent(r, &back, NULL) == PC_OK);
    CHECK(pc_compose_fx_composite(&l, &content, &back, m, r, NULL, NULL, &e) == PC_OK);
    /* A buffer whose length doesn't match its rect; no maps. */
    CHECK(pc_compose_buffer_transparent(pc_rect_make(0, 0, 2, 2), &small, NULL) == PC_OK);
    small.rect = r;
    CHECK(pc_compose_fx_composite(&l, &small, &back, m, r, NULL, NULL, &e) == PC_ERR_INVALID);
    small.rect = pc_rect_make(0, 0, 2, 2);
    CHECK(pc_compose_fx_composite(&l, &content, &back, NULL, r, NULL, NULL, &e) == PC_ERR_INVALID);
    CHECK(!pc_compose_fx_maps_stroke_frame(NULL, &fx[0].u.stroke, &r));
    CHECK(pc_compose_fx_maps_bytes(NULL) == 0);
    pc_compose_fx_maps_free(NULL);
    pc_compose_buffer_fini(&small);
    pc_compose_buffer_fini(&content);
    pc_compose_buffer_fini(&back);
    pc_compose_fx_maps_free(m);
    pc_layer_fini(&l);
    pc_layer_fini(&other);
}

static void test_distance_fields_and_kernels_take_hostile_input(void)
{
    float a[12] = {NAN, INFINITY, -1.0f, 2.0f, 0.5f, 0.0f, 1e-30f, -INFINITY, 1.0f, 0.001f, 0.999f, NAN};
    float *d = NULL;
    int k;
    for (k = 0; k < 8; k++) {
        CHECK(pc_compose_fx_distance_field((pc_compose_fx_field_kind)k, a, 4, 3, 2.0f, &d, NULL) == PC_OK);
        free(d);
        d = NULL;
        CHECK(pc_compose_fx_distance_field((pc_compose_fx_field_kind)k, NULL, 0, 7, NAN, &d, NULL) == PC_OK &&
              d == NULL);
    }
    CHECK(pc_compose_fx_distance_field(PC_COMPOSE_FIELD_OUTSIDE, NULL, 3, 3, 1.0f, &d, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_compose_fx_distance_field(PC_COMPOSE_FIELD_OUTSIDE, a, SIZE_MAX, 2, 1.0f, &d, NULL) ==
          PC_ERR_LIMIT);
    CHECK(pc_compose_fx_distance_field((pc_compose_fx_field_kind)99, a, 4, 3, 1.0f, &d, NULL) ==
          PC_ERR_INVALID);
    CHECK(pc_compose_fx_lut_at(a, 0, 0.5f) == 0.5f);
    (void)pc_compose_fx_outline_share(NAN, NAN);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    RUN(test_contents_are_clipped_and_background_painted);
    RUN(test_background_kinds);
    RUN(test_backdrop_outside_the_board_is_untouched);
    RUN(test_effect_cache_budget_bytes_and_purge);
    RUN(test_effect_maps_built_inside_parallel_tiles_do_not_deadlock);
    RUN(test_effect_maps_with_nested_parallel_work_do_not_deadlock);
    RUN(test_hostile_effect_values_render);
    RUN(test_deep_groups_with_effects);
    RUN(test_map_api_misuse_fails_cleanly);
    RUN(test_distance_fields_and_kernels_take_hostile_input);
    return pc_t_finish();
}
