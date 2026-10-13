/* Brush-engine tests, part 2 (crates/paint/src/brush_tests.rs: masks, smoothing, rendering,
 * presets, projection, locks, scatter caps). */
#include <math.h>

#include "base/json.h"
#include "support.h"

static pc_paint_brush tex_brush(const pc_paint_gray_tile *tile, pc_paint_mask_mode mode, float depth,
                                bool each_tip)
{
    pc_paint_brush b = tb_brush();
    b.size = 40.0f;
    b.texture.enabled = true;
    b.texture.pattern.kind = PC_PAINT_PATTERN_TILE;
    pc_paint_gray_tile_copy(&b.texture.pattern.tile, tile);
    b.texture.mode = mode;
    b.texture.depth = depth;
    b.texture.each_tip = each_tip;
    return b;
}

static float paint_alpha(const pc_paint_brush *b, const pc_paint_point *pts, size_t n, int32_t x, int32_t y)
{
    pc_surface s;
    float a;
    tb_paint(&s, b, pts, n);
    a = tb_alpha(&s, x, y);
    pc_surface_fini(&s);
    return a;
}

static void test_texture_modulates_coverage(void)
{
    pc_paint_gray_tile tile = tb_left_tile(8, 8, 4);
    pc_paint_point pts[2];
    pc_paint_brush b;
    pc_paint_dab_vec d;
    bool in = true, low = false;
    tb_line(20.0, 80.0, 30.0, pts);
    for (int each = 0; each < 2; each++) {
        float v;
        b = tex_brush(&tile, PC_PAINT_MASK_MULTIPLY, 1.0f, each == 1);
        CHECK(paint_alpha(&b, pts, 2, 46, 30) < 0.1f); /* dark texel blocks paint */
        CHECK(paint_alpha(&b, pts, 2, 41, 30) > 0.9f); /* light texel passes */
        pc_paint_brush_fini(&b);
        b = tex_brush(&tile, PC_PAINT_MASK_MULTIPLY, 0.5f, each == 1);
        v = paint_alpha(&b, pts, 2, 46, 30);
        if (each) {
            CHECK(v > 0.5f && v < 1.0f); /* per-tip texturing builds up across overlapping dabs */
        } else {
            CHECK_NEAR(v, 0.5, 0.02);
        }
        pc_paint_brush_fini(&b);
    }
    /* Brightness and invert. */
    b = tex_brush(&tile, PC_PAINT_MASK_MULTIPLY, 1.0f, false);
    b.texture.invert = true;
    CHECK(paint_alpha(&b, pts, 2, 46, 30) > 0.9f && paint_alpha(&b, pts, 2, 41, 30) < 0.1f);
    b.texture.invert = false;
    b.texture.brightness = 1.0f;
    /* +150 lifts a black texel by 150 levels of 255, not to white. */
    CHECK_NEAR(paint_alpha(&b, pts, 2, 46, 30), 150.0 / 255.0, 0.02);
    pc_paint_brush_fini(&b);
    /* All modes keep zero coverage at zero and stay in range. */
    for (int m = 0; m < PC_PAINT_MASK_MODE_COUNT; m++) {
        const float ts[3] = {0.0f, 0.3f, 1.0f};
        for (int i = 0; i < 3; i++) {
            float v = pc_paint_mask_combine((pc_paint_mask_mode)m, 0.7f, ts[i], 0.8f);
            CHECK(pc_paint_mask_combine((pc_paint_mask_mode)m, 0.0f, ts[i], 1.0f) == 0.0f);
            CHECK(v >= 0.0f && v <= 1.0f);
        }
        for (int i = 0; i < 3; i++) {
            float v = pc_paint_texture_combine((pc_paint_mask_mode)m, 0.7f, ts[i], 0.8f);
            CHECK(pc_paint_texture_combine((pc_paint_mask_mode)m, 0.0f, ts[i], 1.0f) == 0.0f);
            CHECK(v >= 0.0f && v <= 1.0f);
        }
        {
            float v = pc_paint_texture_combine((pc_paint_mask_mode)m, 0.7f, NAN, INFINITY);
            CHECK(v >= 0.0f && v <= 1.0f);
        }
        /* Full-white texture never reduces multiply-like masks below the brush. */
        CHECK(pc_paint_mask_combine((pc_paint_mask_mode)m, 0.6f, 1.0f, 1.0f) >= 0.6f - 1e-5f ||
              m == PC_PAINT_MASK_OVERLAY);
    }
    /* Depth jitter per tip is deterministic and within [min depth, depth]. */
    b = tex_brush(&tile, PC_PAINT_MASK_MULTIPLY, 0.8f, true);
    b.texture.depth_jitter = (pc_paint_dynamic){1.0f, PC_PAINT_CONTROL_OFF, 25, 0.25f};
    tb_line(0.0, 200.0, 0.0, pts);
    d = tb_dabs(&b, pts, 2);
    for (size_t i = 0; i < d.len; i++) {
        in = in && d.v[i].depth <= 0.8f + 1e-6f && d.v[i].depth >= 0.8f * 0.25f - 1e-6f;
        low = low || d.v[i].depth < 0.6f;
    }
    CHECK(in && low);
    pc_paint_dab_vec_fini(&d);
    pc_paint_brush_fini(&b);
    pc_paint_gray_tile_fini(&tile);
}

static void test_dual_brush_intersects(void)
{
    /* Dual tip: small dabs far apart -> the stroke survives only where they land. */
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2];
    pc_surface s;
    b.size = 30.0f;
    b.dual_brush.enabled = true;
    b.dual_brush.size = 6.0f;
    b.dual_brush.spacing = 5.0f;
    b.dual_brush.hardness = 1.0f;
    tb_line(20.0, 200.0, 30.0, pts);
    tb_paint(&s, &b, pts, 2);
    /* Dual dabs every 30 px starting at x = 20. */
    CHECK(tb_alpha(&s, 20, 30) > 0.9f && tb_alpha(&s, 50, 30) > 0.9f);
    CHECK(tb_alpha(&s, 35, 30) < 0.01f);
    /* Outside the primary (y beyond its radius) nothing, even if dual covers. */
    CHECK(tb_alpha(&s, 20, 50) == 0.0f);
    pc_surface_fini(&s);
    /* Without dual, the stroke is continuous. */
    pc_paint_dual_brush_default(&b.dual_brush);
    tb_paint(&s, &b, pts, 2);
    CHECK(tb_alpha(&s, 35, 30) > 0.9f);
    pc_surface_fini(&s);
}

static int extreme(const pc_surface *s)
{
    int n = 0;
    for (int y = 30; y < 70; y++) {
        for (int x = 30; x < 70; x++) {
            float v = tb_alpha(s, x, y);
            n += !(v >= 0.15f && v <= 0.85f);
        }
    }
    return n;
}

static void test_wet_edges_and_noise(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2], one = pc_paint_point_make(70.0, 50.0, 1.0f);
    pc_surface s, a, n;
    float centre, single;
    b.size = 40.0f;
    b.hardness = 0.0f;
    b.wet_edges = true;
    tb_line(20.0, 120.0, 50.0, pts);
    centre = paint_alpha(&b, pts, 2, 70, 50);
    CHECK(centre < 0.6f); /* wet interior is lighter */
    /* Overlap doesn't build up past a single dab. */
    single = paint_alpha(&b, &one, 1, 70, 50);
    CHECK(fabsf(centre - single) < 0.05f);
    /* Rim is darker than the interior for a hard tip. */
    b.hardness = 1.0f;
    one = pc_paint_point_make(50.0, 50.0, 1.0f);
    tb_paint(&s, &b, &one, 1);
    CHECK(tb_alpha(&s, 50 + 18, 50) > tb_alpha(&s, 50, 50) + 0.2f);
    pc_surface_fini(&s);
    /* Noise: soft tip coverage becomes grainy (more pixels near 0/1). */
    b = tb_brush();
    b.size = 40.0f;
    b.hardness = 0.0f;
    tb_paint(&a, &b, &one, 1);
    b.noise = true;
    tb_paint(&n, &b, &one, 1);
    CHECK(extreme(&n) > extreme(&a) + 100);
    pc_surface_fini(&a);
    pc_surface_fini(&n);
}

static double dev(const pc_paint_point *p, size_t n)
{
    double s = 0.0;
    for (size_t i = 0; i < n; i++) {
        s += fabs(p[i].y - 3.0);
    }
    return s / (double)n;
}

static void test_smoothing_modes(void)
{
    pc_paint_point zig[40], jit[20], far[2], abc[3];
    pc_paint_point_vec sm = {0}, no = {0}, out = {0};
    pc_paint_smoothing cfg, ps;
    pc_paint_smoother s;
    for (int i = 0; i < 40; i++) {
        zig[i] = pc_paint_point_make(i * 5.0, i % 2 == 0 ? 0.0 : 6.0, 1.0f);
    }
    /* Exponential smoothing reduces the zig-zag. */
    pc_paint_smoothing_default(&cfg);
    cfg.amount = 0.8f;
    CHECK(pc_paint_smooth_points(&cfg, 1.0f, zig, 40, &sm, NULL) == PC_OK);
    CHECK(sm.len > 6);
    CHECK(dev(&sm.v[5], sm.len - 6) < dev(zig, 40) * 0.5);
    /* Catch-up on end finishes at the last input point; without it, the stroke ends short. */
    CHECK(sm.v[sm.len - 1].x == zig[39].x && sm.v[sm.len - 1].y == zig[39].y);
    cfg.catch_up_on_end = false;
    CHECK(pc_paint_smooth_points(&cfg, 1.0f, zig, 40, &no, NULL) == PC_OK);
    CHECK(no.v[no.len - 1].x < zig[39].x - 1.0);
    /* Pulled string: jitter smaller than the string moves nothing; the brush trails by the string. */
    pc_paint_smoothing_default(&ps);
    ps.amount = 0.2f;
    ps.pulled_string = true;
    ps.catch_up_on_end = false;
    for (int i = 0; i < 20; i++) {
        jit[i] = pc_paint_point_make(i % 2 == 0 ? 0.0 : 5.0, 0.0, 1.0f);
    }
    CHECK(pc_paint_smooth_points(&ps, 1.0f, jit, 20, &out, NULL) == PC_OK);
    CHECK_EQ_UINT(out.len, 1);
    far[0] = pc_paint_point_make(0.0, 0.0, 1.0f);
    far[1] = pc_paint_point_make(100.0, 0.0, 1.0f);
    out.len = 0;
    CHECK(pc_paint_smooth_points(&ps, 1.0f, far, 2, &out, NULL) == PC_OK);
    CHECK_NEAR(out.v[out.len - 1].x, 80.0, 1e-4);
    /* Adjust for zoom: at 200 % zoom the string is half as long in image pixels. */
    out.len = 0;
    CHECK(pc_paint_smooth_points(&ps, 2.0f, far, 2, &out, NULL) == PC_OK);
    CHECK_NEAR(out.v[out.len - 1].x, 90.0, 1e-4);
    /* Stroke catch-up: a pause (timestamps) lets the brush converge on the pointer. */
    abc[0] = pc_paint_point_make(0.0, 0.0, 1.0f);
    abc[1] = abc[0];
    abc[1].x = 100.0;
    abc[1].time = 16.0;
    abc[2] = abc[1];
    abc[2].time = 500.0;
    pc_paint_smoothing_default(&cfg);
    cfg.amount = 0.8f;
    cfg.catch_up_on_end = false;
    pc_paint_smoother_init(&s, &cfg, 1.0f);
    out.len = 0;
    for (int i = 0; i < 3; i++) {
        CHECK(pc_paint_smoother_push(&s, abc[i], &out, NULL) == PC_OK);
    }
    CHECK(out.v[out.len - 1].x > 99.0);
    cfg.catch_up = false;
    pc_paint_smoother_init(&s, &cfg, 1.0f);
    out.len = 0;
    for (int i = 0; i < 3; i++) {
        CHECK(pc_paint_smoother_push(&s, abc[i], &out, NULL) == PC_OK);
    }
    CHECK(out.v[out.len - 1].x < 70.0);
    pc_paint_point_vec_fini(&sm);
    pc_paint_point_vec_fini(&no);
    pc_paint_point_vec_fini(&out);
}

static void test_pose_overrides_stylus_values(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2];
    pc_paint_dab_vec d;
    bool ok = true;
    b.size = 40.0f;
    b.pose.enabled = true;
    b.pose.pressure = 0.5f;
    b.pose.override_pressure = true;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = pc_paint_dynamic_controlled(PC_PAINT_CONTROL_PEN_PRESSURE);
    tb_line(0.0, 50.0, 0.0, pts);
    d = tb_dabs(&b, pts, 2);
    for (size_t i = 0; i < d.len; i++) {
        ok = ok && fabsf(d.v[i].radius - 10.0f) < 1e-4f;
    }
    CHECK(ok && d.len > 0);
    pc_paint_dab_vec_fini(&d);
}

static void test_chunked_rendering_matches_one_shot(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[60];
    pc_surface one, inc, pre;
    pc_paint_renderer *r = NULL;
    pc_rect d1, d;
    pc_pixel_format fmt = PC_PIXEL_FORMAT_RGBA8;
    bool same = true;
    const float red[4] = {1, 0, 0, 1};
    b.size = 24.0f;
    b.seed = 9;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = pc_paint_dynamic_jitter(0.5f);
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    b.scattering =
        (pc_paint_scattering){true, pc_paint_dynamic_jitter(1.0f), true, 2, pc_paint_dynamic_default()};
    b.texture.enabled = true;
    b.texture.depth = 0.5f;
    b.color_dynamics.enabled = true;
    b.color_dynamics.hue_jitter = 0.5f;
    memcpy(b.color, red, sizeof red);
    for (int i = 0; i < 60; i++) {
        pts[i] = pc_paint_point_make(20.0 + i * 4.0, 60.0 + sin(i * 0.3) * 30.0, 1.0f);
    }
    pc_surface_init(&one, fmt);
    CHECK(pc_paint_render_stroke(&one, &b, pts, 60, NULL, false, 1.0f, &d1, NULL) == PC_OK);
    pc_surface_init(&inc, fmt);
    CHECK(pc_surface_clone(&inc, &pre, NULL) == PC_OK);
    CHECK(pc_paint_renderer_new(&b, &fmt, 1.0f, &r, NULL) == PC_OK);
    for (size_t i = 0; i < 60; i += 7) {
        CHECK(pc_paint_renderer_push(r, &pts[i], 60 - i < 7 ? 60 - i : 7, NULL) == PC_OK);
        CHECK(pc_paint_renderer_composite(r, &pre, &inc, NULL, false, false, &d, NULL) == PC_OK);
    }
    CHECK(pc_paint_renderer_finish(r, NULL) == PC_OK);
    CHECK(pc_paint_renderer_composite(r, &pre, &inc, NULL, false, false, &d, NULL) == PC_OK);
    CHECK(pc_rect_eq(d1, pc_paint_renderer_bounds(r)));
    for (int32_t y = d1.y0; y < d1.y1; y++) {
        for (int32_t x = d1.x0; x < d1.x1; x++) {
            float p[5], q[5];
            pc_surface_read_pixel(&one, x, y, p);
            pc_surface_read_pixel(&inc, x, y, q);
            same = same && memcmp(p, q, 4 * sizeof(float)) == 0;
        }
    }
    CHECK(same);
    pc_paint_renderer_free(r);
    pc_surface_fini(&one);
    pc_surface_fini(&inc);
    pc_surface_fini(&pre);
}

static void test_paints_every_depth_and_model(void)
{
    const pc_color_mode modes[3] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK};
    const pc_sample_type samples[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    const float white[4] = {1, 1, 1, 1}, red[4] = {1, 0, 0, 1};
    for (int m = 0; m < 3; m++) {
        for (int s = 0; s < 3; s++) {
            for (int a = 0; a < 2; a++) {
                pc_pixel_format fmt = {modes[m], samples[s], a == 1};
                float px[5], expect[4], c[4], p1[5], p2[5];
                size_t n = pc_from_rgba(fmt, white, px);
                pc_surface sf;
                pc_paint_brush b = tb_brush(), fancy;
                pc_paint_point pts[2];
                pc_rect d;
                b.size = 8.0f;
                memcpy(b.color, red, sizeof red);
                fancy = b;
                fancy.shape_dynamics.enabled = true;
                fancy.shape_dynamics.size = pc_paint_dynamic_jitter(0.3f);
                fancy.color_dynamics.enabled = true;
                fancy.color_dynamics.brightness_jitter = 0.1f;
                fancy.texture.enabled = true;
                fancy.texture.depth = 0.2f;
                pc_surface_init_default(&sf, fmt, px, n);
                tb_line(5.0, 40.0, 5.0, pts);
                CHECK(pc_paint_render_stroke(&sf, &b, pts, 2, NULL, false, 1.0f, &d, NULL) == PC_OK);
                CHECK(pc_paint_render_stroke(&sf, &fancy, pts, 2, NULL, false, 1.0f, &d, NULL) == PC_OK);
                pc_surface_rgba(&sf, 20, 5, c);
                n = pc_from_rgba(fmt, red, px);
                pc_to_rgba(fmt, px, n, expect);
                CHECK(fabsf(c[0] - expect[0]) < 0.15f && fabsf(c[1] - expect[1]) < 0.12f);
                pc_surface_read_pixel(&sf, 20, 30, p1);
                pc_surface_read_pixel(&sf, 5000, 5000, p2);
                CHECK(memcmp(p1, p2, pc_pixel_format_channels(fmt) * sizeof(float)) == 0); /* untouched */
                pc_surface_fini(&sf);
            }
        }
    }
}

static void test_blend_mode_and_dissolve(void)
{
    pc_surface s;
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2];
    pc_rect d;
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    float c[4];
    bool binary = true, zero = false, one = false;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA32F);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 100, 20), grey, 4, NULL) == PC_OK);
    b.size = 10.0f;
    memcpy(b.color, grey, sizeof grey);
    b.mode = PC_BLEND_MULTIPLY;
    tb_line(10.0, 90.0, 10.0, pts);
    CHECK(pc_paint_render_stroke(&s, &b, pts, 2, NULL, false, 1.0f, &d, NULL) == PC_OK);
    pc_surface_rgba(&s, 50, 10, c);
    CHECK_NEAR(c[0], 0.25, 1e-4);
    pc_surface_fini(&s);
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA32F);
    b = tb_brush();
    b.size = 30.0f;
    b.opacity = 0.5f;
    b.mode = PC_BLEND_DISSOLVE;
    tb_line(20.0, 80.0, 20.0, pts);
    CHECK(pc_paint_render_stroke(&s, &b, pts, 2, NULL, false, 1.0f, &d, NULL) == PC_OK);
    for (int x = 30; x < 70; x++) {
        float v = tb_alpha(&s, x, 20);
        binary = binary && (v == 0.0f || v == 1.0f);
        zero = zero || v == 0.0f;
        one = one || v == 1.0f;
    }
    CHECK(binary && zero && one);
    pc_surface_fini(&s);
}

static void test_presets_render_and_round_trip(void)
{
    pc_paint_preset_list l;
    pc_paint_point pts[20];
    pc_paint_brush b;
    CHECK(pc_paint_presets_builtin(&l, NULL) == PC_OK);
    CHECK(l.len >= 12);
    for (int i = 0; i < 20; i++) {
        pts[i] = pc_paint_point_make(40.0 + i * 8.0, 60.0, 0.8f);
        pts[i].time = i * 16.0;
    }
    for (size_t i = 0; i < l.len; i++) {
        pc_buf js = PC_BUF_INIT;
        pc_paint_preset back;
        pc_surface s;
        pc_rect d;
        CHECK(pc_paint_json_to_string(PC_PAINT_JSON_PRESET, &l.v[i], 0, &js, NULL) == PC_OK);
        CHECK(pc_paint_json_from_string(PC_PAINT_JSON_PRESET, pc_buf_cstr(&js), js.len, &back, NULL) ==
              PC_OK);
        CHECK(pc_paint_preset_eq(&back, &l.v[i]));
        pc_paint_preset_fini(&back);
        pc_buf_fini(&js);
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
        CHECK(pc_paint_render_stroke(&s, &l.v[i].brush, pts, 20, NULL, false, 1.0f, &d, NULL) == PC_OK);
        CHECK(!pc_rect_is_empty(d));
        CHECK(tb_alpha_sum(&s, d) > 10.0f);
        pc_surface_fini(&s);
    }
    pc_paint_preset_list_fini(&l);
    /* Partial JSON fills defaults. */
    {
        const char *j = "{\"size\": 50, \"scattering\": {\"enabled\": true, \"scatter\": {\"jitter\": 2}}, "
                        "\"pressure_size\": false}";
        CHECK(pc_paint_json_from_string(PC_PAINT_JSON_BRUSH, j, strlen(j), &b, NULL) == PC_OK);
        CHECK(b.size == 50.0f);
        CHECK(b.scattering.enabled && b.scattering.count == 1 && !b.pressure_size);
        CHECK(b.hardness == 1.0f);
        pc_paint_brush_fini(&b);
    }
}

static const pc_paint_brush *find(const pc_paint_preset_list *l, const char *name)
{
    const pc_paint_preset *p = pc_paint_presets_find(l->v, l->len, name, strlen(name));
    CHECK(p != NULL);
    return p != NULL ? &p->brush : NULL;
}

static void test_protect_texture_keeps_pattern(void)
{
    pc_paint_preset_list l;
    const pc_paint_brush *chalk, *canvas;
    pc_paint_brush cur, next;
    CHECK(pc_paint_presets_builtin(&l, NULL) == PC_OK);
    chalk = find(&l, "chalk");
    canvas = find(&l, "Canvas Texture");
    if (chalk != NULL && canvas != NULL) {
        pc_paint_brush_copy(&cur, chalk);
        cur.protect_texture = true;
        pc_paint_brush_copy(&next, canvas);
        pc_paint_brush_with_protected_texture(&next, &cur);
        CHECK(pc_paint_pattern_eq(&next.texture.pattern, &chalk->texture.pattern));
        CHECK(next.texture.mode == canvas->texture.mode);
        pc_paint_brush_fini(&next);
        pc_paint_brush_copy(&next, canvas);
        pc_paint_brush_with_protected_texture(&next, chalk);
        CHECK(pc_paint_pattern_eq(&next.texture.pattern, &canvas->texture.pattern));
        pc_paint_brush_fini(&next);
        pc_paint_brush_fini(&cur);
    }
    pc_paint_preset_list_fini(&l);
}

static void test_tail_preview_shows_the_stroke_as_finishing_it_would(void)
{
    /* Compositing the tail preview over the incremental composite gives the finished stroke. */
    pc_pixel_format fmt = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true};
    pc_paint_brush b = tb_brush(), plain = tb_brush();
    pc_paint_point pts[12];
    pc_surface pre, live, done;
    pc_paint_renderer *r = NULL, *tail = NULL;
    pc_rect d;
    bool same = true;
    b.size = 9.0f;
    b.smoothing.amount = 0.7f;
    for (int i = 0; i < 12; i++) {
        pts[i] = pc_paint_point_make(5.0 + i * 9.0, 20.0 + (i % 3) * 7.0, 1.0f);
    }
    pc_surface_init(&pre, fmt);
    CHECK(pc_surface_clone(&pre, &live, NULL) == PC_OK);
    CHECK(pc_surface_clone(&pre, &done, NULL) == PC_OK);
    CHECK(pc_paint_renderer_new(&b, &fmt, 1.0f, &r, NULL) == PC_OK);
    CHECK(pc_paint_renderer_push(r, pts, 12, NULL) == PC_OK);
    CHECK(pc_paint_renderer_composite(r, &pre, &live, NULL, false, false, &d, NULL) == PC_OK);
    CHECK(pc_paint_renderer_tail_preview(r, &tail, NULL) == PC_OK);
    CHECK(tail != NULL); /* the brush lags: there is a tail */
    if (tail != NULL) {
        CHECK(pc_paint_renderer_composite(tail, &pre, &live, NULL, false, false, &d, NULL) == PC_OK);
    }
    CHECK(pc_paint_render_stroke(&done, &b, pts, 12, NULL, false, 1.0f, &d, NULL) == PC_OK);
    for (int y = 0; y < 60; y++) {
        for (int x = 0; x < 120; x++) {
            float p[4], q[4];
            pc_surface_rgba(&live, x, y, p);
            pc_surface_rgba(&done, x, y, q);
            same = same && memcmp(p, q, sizeof p) == 0;
        }
    }
    CHECK(same);
    pc_paint_renderer_free(tail);
    pc_paint_renderer_free(r);
    /* Without smoothing (and past the first dab) finishing adds nothing. */
    CHECK(pc_paint_renderer_new(&plain, &fmt, 1.0f, &r, NULL) == PC_OK);
    CHECK(pc_paint_renderer_push(r, pts, 12, NULL) == PC_OK);
    tail = (pc_paint_renderer *)1;
    CHECK(pc_paint_renderer_tail_preview(r, &tail, NULL) == PC_OK);
    CHECK(tail == NULL);
    pc_paint_renderer_free(r);
    pc_surface_fini(&pre);
    pc_surface_fini(&live);
    pc_surface_fini(&done);
}

/* ---------- Tilt Scale, Brush Projection, speed spacing, locks, mixer transfer ---------- */

/* Coverage extent (pixels above half coverage) of a rasterised dab along x and y through its centre. */
static void extents(const pc_paint_brush *b, const pc_paint_dab *d, int *w, int *h)
{
    tb_raster r = tb_rast(b, d, false);
    int32_t cx = (int32_t)floor(d->center.x), cy = (int32_t)floor(d->center.y);
    *w = 0;
    *h = 0;
    for (int32_t x = r.r.x0; x < r.r.x1; x++) {
        *w += tb_at(&r, x, cy) > 0.5f;
    }
    for (int32_t y = r.r.y0; y < r.r.y1; y++) {
        *h += tb_at(&r, cx, y) > 0.5f;
    }
    pc_paint_fbuf_fini(&r.v);
}

static pc_paint_point pen(double x, double y, float tx, float ty, float rot)
{
    pc_paint_point p = pc_paint_point_make(x, y, 1.0f);
    p.tilt_x = tx;
    p.tilt_y = ty;
    p.rotation = rot;
    return p;
}

static pc_paint_dab first_dab(const pc_paint_brush *b, const pc_paint_point *pts, size_t n)
{
    pc_paint_dab_vec d = tb_dabs(b, pts, n);
    pc_paint_dab out = pc_paint_dab_round(pc_point_make(0, 0), 0.0f, 0.0f);
    CHECK(d.len > 0);
    if (d.len > 0) {
        out = d.v[0];
    }
    pc_paint_dab_vec_fini(&d);
    return out;
}

static pc_paint_brush tilt_brush(float scale)
{
    pc_paint_brush b = tb_brush();
    b.size = 40.0f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = (pc_paint_dynamic){0.0f, PC_PAINT_CONTROL_PEN_TILT, 25, 1.0f};
    b.shape_dynamics.tilt_scale = scale;
    return b;
}

static void test_tilt_scale_squashes_the_tip_height_with_pen_tilt(void)
{
    pc_paint_point upright = pen(50.0, 50.0, 0.0f, 0.0f, 0.0f), tilted = pen(50.0, 50.0, 45.0f, 0.0f, 0.0f);
    pc_paint_brush b0 = tilt_brush(0.0f), b1 = tilt_brush(1.0f), b2 = tilt_brush(2.0f), bp;
    pc_paint_dab d, d2;
    pc_paint_point line[2] = {pen(20.0, 40.0, 60.0f, 0.0f, 0.0f), pen(120.0, 40.0, 60.0f, 0.0f, 0.0f)};
    pc_surface a, c;
    int w, h;
    /* 0 % (the default) and an upright pen leave the tip round. */
    CHECK(first_dab(&b0, &tilted, 1).roundness == 1.0f);
    CHECK(first_dab(&b2, &upright, 1).roundness == 1.0f);
    /* 100 % at 45 degrees tilt: height x (1 - 0.5 x 0.5) = 0.75; 200 %: 0.5. */
    d = first_dab(&b1, &tilted, 1);
    CHECK_NEAR(d.roundness, 0.75, 1e-4);
    d2 = first_dab(&b2, &tilted, 1);
    CHECK_NEAR(d2.roundness, 0.5, 1e-4);
    extents(&b2, &d2, &w, &h);
    CHECK(w >= 38 && abs(h - 20) <= 2);
    /* Only with the size control on Pen Tilt (Photoshop greys the slider otherwise). */
    bp = tilt_brush(2.0f);
    bp.shape_dynamics.size.control = PC_PAINT_CONTROL_PEN_PRESSURE;
    CHECK(first_dab(&bp, &tilted, 1).roundness == 1.0f);
    /* Rendered strokes differ. */
    tb_paint(&a, &b0, line, 2);
    tb_paint(&c, &b2, line, 2);
    CHECK(tb_alpha_sum(&c, pc_rect_make(0, 0, 160, 80)) <
          tb_alpha_sum(&a, pc_rect_make(0, 0, 160, 80)) * 0.75f);
    pc_surface_fini(&a);
    pc_surface_fini(&c);
}

static pc_paint_brush proj_brush(bool on)
{
    pc_paint_brush b = tb_brush();
    b.size = 40.0f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.brush_projection = on;
    return b;
}

static void test_brush_projection_foreshortens_along_tilt_and_turns_with_rotation(void)
{
    pc_paint_brush on = proj_brush(true), off = proj_brush(false), b;
    pc_paint_point p = pen(50.0, 50.0, 60.0f, 0.0f, 0.0f), q = pen(50.0, 50.0, 0.0f, 60.0f, 0.0f);
    pc_paint_point rot = pen(50.0, 50.0, 0.0f, 0.0f, 90.0f);
    pc_paint_dab d, dd, e;
    tb_raster ra, rc;
    int w, h;
    /* Tilted 60 degrees towards +x: the tip is foreshortened to cos 60 = 1/2 along x. */
    d = first_dab(&on, &p, 1);
    CHECK(fabsf(d.proj_scale - 0.5f) < 1e-4f && fabsf(d.proj_angle) < 1e-5f);
    extents(&on, &d, &w, &h);
    CHECK(abs(w - 20) <= 2 && h >= 38);
    /* Tilted along y instead: foreshortened vertically. */
    d = first_dab(&on, &q, 1);
    extents(&on, &d, &w, &h);
    CHECK(w >= 38 && abs(h - 20) <= 2);
    /* Off: the tilt doesn't touch the tip. */
    d = first_dab(&off, &p, 1);
    CHECK(d.proj_scale == 1.0f && d.angle == 0.0f);
    extents(&off, &d, &w, &h);
    CHECK(w == 40 && h == 40);
    /* Barrel rotation turns the tip (an elliptical one shows it). */
    b = proj_brush(true);
    b.roundness = 0.5f;
    d = first_dab(&b, &rot, 1);
    CHECK_NEAR(tb_deg(d.angle), 90.0, 1e-3);
    extents(&b, &d, &w, &h);
    CHECK(w < h);
    /* Dual-brush dabs are never projected. */
    dd = pc_paint_dab_round(pc_point_make(50.0, 50.0), 20.0f, 1.0f);
    dd.proj_scale = 0.3f;
    e = dd;
    e.proj_scale = 1.0f;
    ra = tb_rast(&on, &dd, true);
    rc = tb_rast(&on, &e, true);
    CHECK(ra.v.len == rc.v.len && memcmp(ra.v.v, rc.v.v, ra.v.len * sizeof(float)) == 0);
    pc_paint_fbuf_fini(&ra.v);
    pc_paint_fbuf_fini(&rc.v);
}

static size_t timed_count(const pc_paint_brush *b, double ms)
{
    pc_paint_point p[2] = {pc_paint_point_make(0.0, 10.0, 1.0f), pc_paint_point_make(100.0, 10.0, 1.0f)};
    pc_paint_dab_vec d;
    size_t n;
    p[1].time = ms;
    d = tb_dabs(b, p, 2);
    n = d.len;
    pc_paint_dab_vec_fini(&d);
    return n;
}

static void test_spacing_off_spaces_dabs_by_pointer_speed(void)
{
    pc_paint_brush b = tb_brush(), checked;
    pc_paint_point pts[3] = {pc_paint_point_make(0.0, 10.0, 1.0f), pc_paint_point_make(90.0, 10.0, 1.0f),
                             pc_paint_point_make(100.0, 10.0, 1.0f)};
    pc_paint_point many[20];
    pc_paint_dab_vec a, c, one, out = {0}, dual = {0};
    pc_paint_dab_generator g;
    bool same;
    b.size = 20.0f;
    b.spacing_enabled = false;
    /* 100 px in 400 ms: a dab every 8 ms = every 2 px; in 40 ms: every 20 px. */
    CHECK_EQ_UINT(timed_count(&b, 400.0), 51);
    CHECK_EQ_UINT(timed_count(&b, 40.0), 6);
    /* Never denser than half a pixel, however slow. */
    CHECK(timed_count(&b, 1.0e9) <= 201);
    /* No timestamps: spaced by distance like Spacing on, not a dab per point. */
    checked = b;
    checked.spacing_enabled = true;
    a = tb_dabs(&b, pts, 3);
    c = tb_dabs(&checked, pts, 3);
    CHECK_EQ_UINT(a.len, c.len);
    CHECK(a.len > 3);
    pc_paint_dab_vec_fini(&a);
    pc_paint_dab_vec_fini(&c);
    /* Checked: fixed spacing (25 % of 20 px = 5 px) whatever the speed. */
    CHECK_EQ_UINT(timed_count(&checked, 400.0), timed_count(&checked, 40.0));
    /* Chunked input gives the same dabs. */
    for (int i = 0; i < 20; i++) {
        many[i] = pc_paint_point_make(i * 7.0, 10.0, 1.0f);
        many[i].time = i * 13.0;
    }
    one = tb_dabs(&b, many, 20);
    pc_paint_dab_generator_init(&g, &b, 1.0f);
    for (size_t i = 0; i < 20; i += 3) {
        CHECK(pc_paint_dab_generator_push(&g, &many[i], 20 - i < 3 ? 20 - i : 3, &out, &dual, NULL) == PC_OK);
    }
    CHECK(pc_paint_dab_generator_finish(&g, &out, &dual, NULL) == PC_OK);
    same = one.len == out.len;
    for (size_t i = 0; same && i < one.len; i++) {
        same = pc_paint_dab_eq(&one.v[i], &out.v[i]);
    }
    CHECK(same);
    pc_paint_dab_generator_fini(&g);
    pc_paint_dab_vec_fini(&one);
    pc_paint_dab_vec_fini(&out);
    pc_paint_dab_vec_fini(&dual);
}

static void test_locked_sections_survive_picking_a_preset(void)
{
    pc_paint_preset_list l;
    const pc_paint_brush *chalk;
    pc_paint_brush cur = tb_brush(), next;
    CHECK(pc_paint_presets_builtin(&l, NULL) == PC_OK);
    chalk = find(&l, "chalk");
    if (chalk == NULL) {
        pc_paint_preset_list_fini(&l);
        return;
    }
    cur.scattering.enabled = true;
    cur.scattering.count = 5;
    cur.transfer.enabled = true;
    cur.transfer.wetness = pc_paint_dynamic_jitter(0.4f);
    cur.noise = true;
    /* Unlocked: the preset's sections win. */
    pc_paint_brush_copy(&next, chalk);
    pc_paint_brush_picked_over(&next, &cur);
    CHECK(pc_paint_scattering_eq(&next.scattering, &chalk->scattering));
    CHECK(next.noise == chalk->noise);
    pc_paint_brush_fini(&next);
    /* Locked: the current sections stay, and so do the locks. */
    cur.locks.scattering = true;
    cur.locks.transfer = true;
    cur.locks.noise = true;
    pc_paint_brush_copy(&next, chalk);
    pc_paint_brush_picked_over(&next, &cur);
    CHECK(pc_paint_scattering_eq(&next.scattering, &cur.scattering));
    CHECK(pc_paint_transfer_eq(&next.transfer, &cur.transfer));
    CHECK(next.noise);
    CHECK(pc_paint_section_locks_eq(&next.locks, &cur.locks));
    /* Unlocked sections still come from the preset. */
    CHECK(pc_paint_texture_eq(&next.texture, &chalk->texture));
    CHECK(pc_paint_tip_eq(&next.tip, &chalk->tip));
    pc_paint_brush_fini(&next);
    pc_paint_preset_list_fini(&l);
}

static void test_transfer_wetness_and_mix_vary_per_dab(void)
{
    pc_paint_brush b = tb_brush(), full, back, old, plain = tb_brush();
    pc_paint_point pts[2] = {pc_paint_point_make(0.0, 0.0, 0.3f), pc_paint_point_make(60.0, 0.0, 0.3f)};
    pc_paint_dab_vec d;
    pc_paint_mixer_settings mdef;
    bool wet = true, mix = true, low = false;
    pc_buf js = PC_BUF_INIT;
    yyjson_doc *doc = NULL;
    const char *oldj =
        "{\"size\": 12, \"shapeDynamics\": {\"enabled\": true}, \"transfer\": {\"enabled\": true}}";
    b.transfer.enabled = true;
    b.transfer.wetness = pc_paint_dynamic_controlled(PC_PAINT_CONTROL_PEN_PRESSURE);
    b.transfer.mix = (pc_paint_dynamic){1.0f, PC_PAINT_CONTROL_OFF, 25, 0.25f};
    d = tb_dabs(&b, pts, 2);
    for (size_t i = 0; i < d.len; i++) {
        wet = wet && fabsf(d.v[i].wet - 0.3f) < 1e-5f;
        mix = mix && d.v[i].mix >= 0.25f && d.v[i].mix <= 1.0f;
        low = low || d.v[i].mix < 0.9f;
    }
    CHECK(wet && mix && low);
    pc_paint_dab_vec_fini(&d);
    /* Transfer off: as set. */
    d = tb_dabs(&plain, pts, 2);
    wet = true;
    for (size_t i = 0; i < d.len; i++) {
        wet = wet && d.v[i].wet == 1.0f && d.v[i].mix == 1.0f;
    }
    CHECK(wet);
    pc_paint_dab_vec_fini(&d);
    /* New fields round-trip through to_value and old JSON loads with neutral values. */
    full = b;
    full.spacing_enabled = false;
    full.shape_dynamics.tilt_scale = 1.5f;
    full.shape_dynamics.brush_projection = true;
    full.locks.texture = true;
    full.mixer.wet = 0.9f;
    full.mixer.sample_all_layers = true;
    CHECK(pc_paint_json_to_string(PC_PAINT_JSON_BRUSH, &full, PC_PAINT_JSON_VALUE, &js, NULL) == PC_OK);
    CHECK(pc_json_parse(pc_buf_cstr(&js), js.len, &doc, NULL) == PC_OK);
    CHECK(pc_paint_json_read(PC_PAINT_JSON_BRUSH, yyjson_doc_get_root(doc), &back, NULL) == PC_OK);
    CHECK(pc_paint_brush_eq(&back, &full));
    pc_paint_brush_fini(&back);
    yyjson_doc_free(doc);
    pc_buf_fini(&js);
    CHECK(pc_paint_json_from_string(PC_PAINT_JSON_BRUSH, oldj, strlen(oldj), &old, NULL) == PC_OK);
    CHECK(old.spacing_enabled && !old.shape_dynamics.brush_projection &&
          old.shape_dynamics.tilt_scale == 0.0f);
    pc_paint_mixer_default(&mdef);
    CHECK(pc_paint_mixer_eq(&old.mixer, &mdef));
    {
        pc_paint_section_locks l0;
        pc_paint_section_locks_default(&l0);
        CHECK(pc_paint_section_locks_eq(&old.locks, &l0));
    }
    pc_paint_brush_fini(&old);
}

static pc_paint_brush scatter_brush(float j)
{
    pc_paint_brush b = tb_brush();
    b.size = 20.0f;
    b.scattering =
        (pc_paint_scattering){true, pc_paint_dynamic_jitter(j), true, 4, pc_paint_dynamic_default()};
    b.dual_brush.enabled = true;
    b.dual_brush.size = 6.0f;
    b.dual_brush.scatter = j;
    b.dual_brush.both_axes = true;
    return b;
}

static bool dabs_eq(const pc_paint_dab_vec *a, const pc_paint_dab_vec *b)
{
    if (a->len != b->len) {
        return false;
    }
    for (size_t i = 0; i < a->len; i++) {
        if (!pc_paint_dab_eq(&a->v[i], &b->v[i])) {
            return false;
        }
    }
    return true;
}

static bool near_all(const pc_paint_dab_vec *d, double r)
{
    for (size_t i = 0; i < d->len; i++) {
        if (!(fabs(d->v[i].center.x - 25.0) <= 15.0 + r && fabs(d->v[i].center.y - 10.0) <= r)) {
            return false;
        }
    }
    return true;
}

static void test_huge_scatter_is_capped_and_renders_at_every_depth(void)
{
    const float js[5] = {PC_PAINT_MAX_SCATTER + 0.01f, 1e6f, 1e38f, 3.40282347e38f, INFINITY};
    const pc_sample_type ss[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    pc_paint_point pts[2];
    pc_paint_brush cb = scatter_brush(PC_PAINT_MAX_SCATTER), z = scatter_brush(0.0f), nn = scatter_brush(NAN);
    pc_paint_dab_vec cd, cu, zd, zu, nd, nu;
    bool on = true;
    tb_line(10.0, 40.0, 10.0, pts);
    tb_all_dabs(&cb, pts, 2, &cd, &cu);
    /* Primary reach: MAX_SCATTER radii of 10 px; dual reach: MAX_SCATTER radii of 3 px. */
    CHECK(near_all(&cd, PC_PAINT_MAX_SCATTER * 10.0) && near_all(&cu, PC_PAINT_MAX_SCATTER * 3.0));
    for (int k = 0; k < 5; k++) {
        pc_paint_brush b = scatter_brush(js[k]);
        pc_paint_dab_vec d, u;
        pc_paint_context ctx;
        tb_all_dabs(&b, pts, 2, &d, &u);
        CHECK(dabs_eq(&d, &cd) && dabs_eq(&u, &cu)); /* paints like the maximum */
        pc_paint_dab_vec_fini(&d);
        pc_paint_dab_vec_fini(&u);
        CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
        for (size_t i = 0; i < cd.len; i++) {
            pc_rect r = pc_paint_context_dab_rect(&ctx, &cd.v[i], false);
            CHECK(!pc_rect_is_empty(r) && pc_rect_width(r) <= 24 && pc_rect_height(r) <= 24);
        }
        pc_paint_context_fini(&ctx);
        for (int s = 0; s < 3; s++) {
            pc_surface sf;
            pc_rect dmg;
            pc_surface_init(&sf, (pc_pixel_format){PC_COLOR_MODE_RGB, ss[s], true});
            CHECK(pc_paint_render_stroke(&sf, &b, pts, 2, NULL, false, 1.0f, &dmg, NULL) == PC_OK);
            CHECK(!pc_rect_is_empty(dmg) && dmg.x0 > -200 && dmg.x1 < 250);
            pc_surface_fini(&sf);
        }
    }
    /* A NaN amount scatters nothing, as a zero amount does. */
    tb_all_dabs(&z, pts, 2, &zd, &zu);
    tb_all_dabs(&nn, pts, 2, &nd, &nu);
    CHECK(dabs_eq(&zd, &nd) && dabs_eq(&zu, &nu));
    for (size_t i = 0; i < zd.len; i++) {
        on = on && zd.v[i].center.y == 10.0;
    }
    CHECK(on);
    pc_paint_dab_vec_fini(&cd);
    pc_paint_dab_vec_fini(&cu);
    pc_paint_dab_vec_fini(&zd);
    pc_paint_dab_vec_fini(&zu);
    pc_paint_dab_vec_fini(&nd);
    pc_paint_dab_vec_fini(&nu);
}

static void test_ordinary_scatter_is_unchanged_by_the_cap(void)
{
    pc_paint_brush b = tb_brush(), r;
    pc_paint_point pts[2];
    pc_paint_dab_vec d, u;
    bool far = false, in = true, dfar = false, din = true;
    b.size = 20.0f;
    b.scattering =
        (pc_paint_scattering){true, pc_paint_dynamic_jitter(2.0f), true, 3, pc_paint_dynamic_default()};
    b.dual_brush.enabled = true;
    b.dual_brush.size = 6.0f;
    b.dual_brush.scatter = 1.5f;
    b.dual_brush.both_axes = true;
    pc_paint_brush_bounded(&b, &r);
    CHECK(pc_paint_brush_eq(&r, &b));
    pc_paint_brush_fini(&r);
    tb_line(10.0, 200.0, 50.0, pts);
    tb_all_dabs(&b, pts, 2, &d, &u);
    for (size_t i = 0; i < d.len; i++) {
        double dy = fabs(d.v[i].center.y - 50.0);
        far = far || dy > 10.0;
        in = in && dy <= 20.0;
    }
    for (size_t i = 0; i < u.len; i++) {
        double dy = fabs(u.v[i].center.y - 50.0);
        dfar = dfar || dy > 1.0;
        din = din && dy <= 4.5;
    }
    CHECK(far && in && dfar && din);
    pc_paint_dab_vec_fini(&d);
    pc_paint_dab_vec_fini(&u);
}

static void test_dab_rect_of_a_far_or_non_finite_centre_does_not_overflow(void)
{
    const double cs[7] = {INFINITY, -INFINITY, NAN, 1e300, -1e300, 2147483647.0, -2147483648.0};
    pc_paint_brush b = tb_brush();
    pc_paint_context ctx;
    pc_paint_dab huge = pc_paint_dab_round(pc_point_make(5.0, 5.0), INFINITY, 1.0f);
    pc_rect r;
    CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
    for (int i = 0; i < 7; i++) {
        pc_paint_dab d = pc_paint_dab_round(pc_point_make(cs[i], cs[i]), 10.0f, 1.0f);
        pc_rect rs[3] = {pc_paint_context_dab_rect(&ctx, &d, false),
                         pc_paint_context_dab_rect(&ctx, &d, true), pc_paint_rect_around(&d, d.radius)};
        for (int k = 0; k < 3; k++) {
            CHECK(rs[k].x0 <= rs[k].x1 && rs[k].y0 <= rs[k].y1);
        }
    }
    r = pc_paint_rect_around(&huge, huge.radius);
    CHECK(r.x0 <= r.x1 && r.y0 <= r.y1);
    pc_paint_context_fini(&ctx);
}

int main(int argc, char **argv)
{
    RUN(test_texture_modulates_coverage);
    RUN(test_dual_brush_intersects);
    RUN(test_wet_edges_and_noise);
    RUN(test_smoothing_modes);
    RUN(test_pose_overrides_stylus_values);
    RUN(test_chunked_rendering_matches_one_shot);
    RUN(test_paints_every_depth_and_model);
    RUN(test_blend_mode_and_dissolve);
    RUN(test_presets_render_and_round_trip);
    RUN(test_protect_texture_keeps_pattern);
    RUN(test_tail_preview_shows_the_stroke_as_finishing_it_would);
    RUN(test_tilt_scale_squashes_the_tip_height_with_pen_tilt);
    RUN(test_brush_projection_foreshortens_along_tilt_and_turns_with_rotation);
    RUN(test_spacing_off_spaces_dabs_by_pointer_speed);
    RUN(test_locked_sections_survive_picking_a_preset);
    RUN(test_transfer_wetness_and_mix_vary_per_dab);
    RUN(test_huge_scatter_is_capped_and_renders_at_every_depth);
    RUN(test_ordinary_scatter_is_unchanged_by_the_cap);
    RUN(test_dab_rect_of_a_far_or_non_finite_centre_does_not_overflow);
    return pc_t_finish();
}
