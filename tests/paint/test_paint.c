/* Unit tests of crates/paint lib.rs, rng.rs, tile.rs, presets.rs and procedural.rs, plus the C
 * port's never-crash checks (hostile numbers, empty strokes, huge sizes, bad JSON). */
#include <math.h>

#include "base/json.h"
#include "base/pool.h"
#include "support.h"

/* ---------- lib.rs ---------- */

static pc_paint_brush lib_brush(float size)
{
    pc_paint_brush b;
    pc_paint_brush_default(&b);
    b.size = size;
    b.pressure_size = false;
    return b;
}

static void test_dab_spacing(void)
{
    pc_paint_brush b = lib_brush(10.0f);
    pc_paint_point pts[2] = {pc_paint_point_make(0, 0, 1), pc_paint_point_make(100, 0, 1)};
    pc_paint_dab_vec d = tb_dabs(&b, pts, 2);
    /* spacing 0.1 * 10px = 1px -> ~101 dabs */
    CHECK(d.len >= 95 && d.len <= 105);
    pc_paint_dab_vec_fini(&d);
    b.spacing = 1.0f;
    d = tb_dabs(&b, pts, 2);
    CHECK_EQ_UINT(d.len, 11);
    pc_paint_dab_vec_fini(&d);
}

static void test_coverage_profile(void)
{
    float soft_mid = pc_paint_dab_coverage(5.0f, 10.0f, 0.0f);
    CHECK(pc_paint_dab_coverage(0.0f, 10.0f, 1.0f) == 1.0f);
    CHECK(pc_paint_dab_coverage(20.0f, 10.0f, 1.0f) == 0.0f);
    CHECK(soft_mid > 0.2f && soft_mid < 0.8f);
    CHECK(pc_paint_dab_coverage(9.9f, 10.0f, 1.0f) > 0.5f);
}

static void apply(pc_surface *s, const pc_paint_brush *b, const pc_paint_point *pts, size_t n,
                  const pc_surface *sel, bool lock, pc_rect *dmg)
{
    pc_paint_stroke st;
    st.brush = *b;
    st.points = (pc_paint_point *)(uintptr_t)pts;
    st.count = n;
    CHECK(pc_paint_apply_stroke(s, &st, sel, lock, dmg, NULL) == PC_OK);
}

static void test_hard_brush_paints_solid_line(void)
{
    pc_surface s;
    pc_paint_brush b = lib_brush(8.0f);
    pc_paint_point pts[2] = {pc_paint_point_make(10, 10, 1), pc_paint_point_make(90, 10, 1)};
    pc_rect dmg;
    float px[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    apply(&s, &b, pts, 2, NULL, false, &dmg);
    CHECK(pc_rect_contains(dmg, 50, 10));
    pc_surface_read_pixel(&s, 50, 10, px);
    CHECK(px[0] == 0.0f && px[1] == 0.0f && px[2] == 0.0f && px[3] == 1.0f);
    pc_surface_read_pixel(&s, 50, 30, px);
    CHECK(px[3] == 0.0f);
    pc_surface_fini(&s);
}

static void test_opacity_caps_overlap(void)
{
    pc_surface s;
    pc_paint_brush b = lib_brush(10.0f);
    pc_paint_point pts[3] = {pc_paint_point_make(10, 10, 1), pc_paint_point_make(60, 10, 1),
                             pc_paint_point_make(10, 10, 1)};
    pc_rect dmg;
    b.opacity = 0.5f;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    apply(&s, &b, pts, 3, NULL, false, &dmg);
    CHECK_NEAR(pc_surface_sample_channel(&s, 30, 10, 3), 0.5, 0.02);
    pc_surface_fini(&s);
}

static void test_selection_limits_paint(void)
{
    pc_surface s, sel;
    pc_paint_brush b = lib_brush(8.0f);
    pc_paint_point pts[2] = {pc_paint_point_make(10, 10, 1), pc_paint_point_make(90, 10, 1)};
    pc_rect dmg;
    float one = 1.0f;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_surface_fill_rect(&sel, pc_rect_make(0, 0, 50, 100), &one, 1, NULL) == PC_OK);
    apply(&s, &b, pts, 2, &sel, false, &dmg);
    CHECK(pc_surface_sample_channel(&s, 20, 10, 3) == 1.0f);
    CHECK(pc_surface_sample_channel(&s, 70, 10, 3) == 0.0f);
    pc_surface_fini(&s);
    pc_surface_fini(&sel);
}

static void test_eraser_removes_alpha(void)
{
    pc_surface s;
    pc_paint_brush b = lib_brush(8.0f);
    pc_paint_point pts[2] = {pc_paint_point_make(10, 10, 1), pc_paint_point_make(90, 10, 1)};
    pc_rect dmg;
    const float red[4] = {1, 0, 0, 1};
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 100, 20), red, 4, NULL) == PC_OK);
    b.erase = true;
    apply(&s, &b, pts, 2, NULL, false, &dmg);
    CHECK(pc_surface_sample_channel(&s, 50, 10, 3) == 0.0f);
    CHECK(pc_surface_sample_channel(&s, 50, 1, 3) == 1.0f);
    pc_surface_fini(&s);
}

static void test_lock_transparency_preserves_alpha(void)
{
    pc_surface s;
    pc_paint_brush b = lib_brush(8.0f);
    pc_paint_point pts[2] = {pc_paint_point_make(10, 10, 1), pc_paint_point_make(90, 10, 1)};
    pc_rect dmg;
    const float white[4] = {1, 1, 1, 1};
    float px[4];
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_surface_fill_rect(&s, pc_rect_make(0, 0, 50, 20), white, 4, NULL) == PC_OK);
    apply(&s, &b, pts, 2, NULL, true, &dmg);
    pc_surface_read_pixel(&s, 20, 10, px);
    CHECK(px[0] == 0.0f && px[1] == 0.0f && px[2] == 0.0f && px[3] == 1.0f);
    CHECK(pc_surface_sample_channel(&s, 70, 10, 3) == 0.0f);
    pc_surface_fini(&s);
}

static void test_pressure_changes_size(void)
{
    pc_paint_brush b = lib_brush(20.0f);
    pc_paint_point pts[2] = {pc_paint_point_make(0, 0, 0.1f), pc_paint_point_make(100, 0, 1.0f)};
    pc_paint_dab_vec d;
    b.pressure_size = true;
    d = tb_dabs(&b, pts, 2);
    CHECK(d.len > 1 && d.v[0].radius < d.v[d.len - 1].radius);
    pc_paint_dab_vec_fini(&d);
}

static void test_smoothing_keeps_endpoints(void)
{
    pc_paint_point pts[20];
    pc_paint_point_vec s = {0};
    double jitter = 0.0, orig = 0.0;
    for (int i = 0; i < 20; i++) {
        pts[i] = pc_paint_point_make(i, i % 2 == 0 ? 0.0 : 10.0, 1.0f);
    }
    CHECK(pc_paint_smooth(pts, 20, 0.8f, &s, NULL) == PC_OK);
    CHECK_EQ_UINT(s.len, 20);
    CHECK(pc_paint_point_eq(&s.v[0], &pts[0]));
    CHECK(pc_paint_point_eq(&s.v[19], &pts[19]));
    for (int i = 1; i < 19; i++) {
        jitter += fabs(s.v[i].y - 5.0);
        orig += fabs(pts[i].y - 5.0);
    }
    CHECK(jitter < orig);
    pc_paint_point_vec_fini(&s);
}

static void test_works_at_all_depths(void)
{
    const pc_pixel_format fs[5] = {PC_PIXEL_FORMAT_RGBA8, PC_PIXEL_FORMAT_RGBA16, PC_PIXEL_FORMAT_RGBA32F,
                                   PC_PIXEL_FORMAT_GRAYA8, PC_PIXEL_FORMAT_CMYKA8};
    for (int i = 0; i < 5; i++) {
        pc_surface s;
        pc_paint_brush b = lib_brush(6.0f);
        pc_paint_point pts[2] = {pc_paint_point_make(5, 5, 1), pc_paint_point_make(40, 5, 1)};
        pc_rect dmg;
        float c[4];
        const float white[4] = {1, 1, 1, 1};
        memcpy(b.color, white, sizeof white);
        pc_surface_init(&s, fs[i]);
        apply(&s, &b, pts, 2, NULL, false, &dmg);
        pc_surface_rgba(&s, 20, 5, c);
        CHECK(c[3] > 0.99f && c[0] > 0.98f);
        pc_surface_fini(&s);
    }
}

/* ---------- rng.rs ---------- */

static void test_rng_deterministic_and_uniformish(void)
{
    float mean = -0.0f;
    bool in = true;
    CHECK(pc_paint_rand01(7, 3, 1) == pc_paint_rand01(7, 3, 1));
    CHECK(pc_paint_rand01(7, 3, 1) != pc_paint_rand01(8, 3, 1));
    CHECK(pc_paint_rand01(7, 3, 1) != pc_paint_rand01(7, 3, 2));
    for (uint64_t i = 0; i < 10000; i++) {
        float v = pc_paint_rand01(2, i, 5);
        mean += pc_paint_rand01(1, i, 0);
        in = in && v >= 0.0f && v < 1.0f;
    }
    mean /= 10000.0f;
    CHECK(fabsf(mean - 0.5f) < 0.02f);
    CHECK(in);
}

/* ---------- tile.rs ---------- */

static void test_base64_round_trip(void)
{
    for (int n = 0; n < 10; n++) {
        uint8_t b[10];
        pc_buf enc = PC_BUF_INIT, dec = PC_BUF_INIT;
        for (int i = 0; i < n; i++) {
            b[i] = (uint8_t)(i * 37 + 5);
        }
        CHECK(pc_paint_b64_encode(b, (size_t)n, &enc, NULL) == PC_OK);
        CHECK(pc_paint_b64_decode(pc_buf_cstr(&enc), enc.len, &dec, NULL) == PC_OK);
        CHECK(dec.len == (size_t)n && (n == 0 || memcmp(dec.data, b, (size_t)n) == 0));
        pc_buf_fini(&enc);
        pc_buf_fini(&dec);
    }
}

static float diag_fn(void *ctx, uint32_t x, uint32_t y)
{
    (void)ctx;
    return (float)(x + y) / 6.0f;
}

static void test_tile_serde_round_trip_and_float_form(void)
{
    pc_paint_gray_tile t, back, f;
    pc_buf js = PC_BUF_INIT;
    const char *fl = "{\"width\":2,\"height\":1,\"data\":[0.0,1.0]}";
    const char *bad = "{\"width\":2,\"height\":2,\"data\":[0.0,1.0]}";
    CHECK(pc_paint_gray_tile_from_fn(5, 3, diag_fn, NULL, &t, NULL) == PC_OK);
    CHECK(pc_paint_json_to_string(PC_PAINT_JSON_GRAY_TILE, &t, 0, &js, NULL) == PC_OK);
    CHECK(pc_paint_json_from_string(PC_PAINT_JSON_GRAY_TILE, pc_buf_cstr(&js), js.len, &back, NULL) == PC_OK);
    CHECK(pc_paint_gray_tile_eq(&back, &t));
    CHECK(pc_paint_json_from_string(PC_PAINT_JSON_GRAY_TILE, fl, strlen(fl), &f, NULL) == PC_OK);
    CHECK(pc_paint_gray_tile_len(&f) == 2 && pc_paint_gray_tile_samples(&f)[0] == 0 &&
          pc_paint_gray_tile_samples(&f)[1] == 65535);
    CHECK(pc_paint_json_from_string(PC_PAINT_JSON_GRAY_TILE, bad, strlen(bad), &back, NULL) != PC_OK);
    pc_paint_gray_tile_fini(&f);
    pc_paint_gray_tile_fini(&back);
    pc_paint_gray_tile_fini(&t);
    pc_buf_fini(&js);
}

static void test_mips_average(void)
{
    pc_paint_gray_tile t = tb_left_tile(4, 4, 2);
    pc_paint_mips *m = NULL;
    CHECK(pc_paint_mips_new(&t, &m, NULL) == PC_OK);
    if (m != NULL) {
        CHECK_EQ_UINT(m->count, 3);
        CHECK_NEAR(m->levels[2].data[0], 0.5, 1e-3);
        CHECK_EQ_UINT(pc_paint_mips_level_for(m, 4.0f), 0);
        CHECK_EQ_UINT(pc_paint_mips_level_for(m, 1.0f), 2);
    }
    pc_paint_mips_release(m);
    pc_paint_gray_tile_fini(&t);
}

/* ---------- presets.rs ---------- */

static void test_builtins_are_grouped_and_unique(void)
{
    pc_paint_preset_list l;
    const pc_paint_preset *ps;
    int last = 0;
    CHECK(pc_paint_presets_builtin(&l, NULL) == PC_OK);
    CHECK(l.len >= 30 && l.len <= 40);
    for (size_t i = 0; i < l.len; i++) {
        int g = -1;
        for (size_t j = i + 1; j < l.len; j++) {
            CHECK(!pc_str_eq(&l.v[i].name, &l.v[j].name)); /* no duplicate names */
        }
        for (int k = 0; k < PC_PAINT_GROUP_COUNT; k++) {
            g = pc_str_eq_c(&l.v[i].group, PC_PAINT_GROUPS[k]) ? k : g;
        }
        CHECK(l.v[i].builtin && g >= 0);
        CHECK(g >= last); /* groups are contiguous, in panel order */
        last = g;
    }
    for (int k = 0; k < PC_PAINT_GROUP_COUNT; k++) {
        int n = 0;
        for (size_t i = 0; i < l.len; i++) {
            n += pc_str_eq_c(&l.v[i].group, PC_PAINT_GROUPS[k]);
        }
        CHECK(n >= 6);
    }
    /* Pressure variants respond to pressure. */
    ps = pc_paint_presets_find(l.v, l.len, "Hard Round Pressure Size", 24);
    CHECK(ps != NULL && ps->brush.shape_dynamics.size.control == PC_PAINT_CONTROL_PEN_PRESSURE);
    CHECK(pc_paint_presets_find(l.v, l.len, "HARD ROUND", 10) != NULL);
    CHECK(pc_paint_presets_find(l.v, l.len, "nope", 4) == NULL);
    pc_paint_preset_list_fini(&l);
}

/* ---------- procedural.rs ---------- */

static void test_patterns_are_seamless_and_in_range(void)
{
    pc_paint_pattern_image *p = NULL, *q = NULL;
    float seam = -0.0f;
    for (int st = 0; st < PC_PAINT_PATTERN_STYLE_COUNT; st++) {
        float var = -0.0f;
        bool in = true;
        CHECK(pc_paint_pattern_generate((pc_paint_pattern_style)st, 64, 3, &p, NULL) == PC_OK);
        CHECK(pc_paint_pattern_generate((pc_paint_pattern_style)st, 64, 3, &q, NULL) == PC_OK);
        for (size_t i = 0; i < 64 * 64; i++) {
            in = in && p->data[i] >= 0.0f && p->data[i] <= 1.0f;
            var += fabsf(p->data[i] - 0.5f);
        }
        CHECK(in);
        CHECK(var / (64.0f * 64.0f) > 0.02f);                          /* not flat */
        CHECK(memcmp(p->data, q->data, 64 * 64 * sizeof(float)) == 0); /* deterministic */
        pc_paint_pattern_image_release(p);
        pc_paint_pattern_image_release(q);
    }
    /* Noise tiles: left and right columns are neighbours. */
    CHECK(pc_paint_pattern_generate(PC_PAINT_STYLE_NOISE, 64, 1, &p, NULL) == PC_OK);
    for (size_t y = 0; y < 64; y++) {
        seam += fabsf(p->data[y * 64] - p->data[y * 64 + 63]);
    }
    CHECK(seam / 64.0f < 0.08f);
    pc_paint_pattern_image_release(p);
}

static void test_tips_have_paint(void)
{
    pc_paint_gray_tile t[9];
    CHECK(pc_paint_chalk_tip(48, 1, &t[0], NULL) == PC_OK);
    CHECK(pc_paint_spatter_tip(48, 1, 12, &t[1], NULL) == PC_OK);
    CHECK(pc_paint_bristle_tip(48, 1, &t[2], NULL) == PC_OK);
    CHECK(pc_paint_charcoal_tip(48, 1, &t[3], NULL) == PC_OK);
    CHECK(pc_paint_leaf_tip(48, &t[4], NULL) == PC_OK);
    CHECK(pc_paint_grass_tip(48, 1, 9, &t[5], NULL) == PC_OK);
    CHECK(pc_paint_sponge_tip(48, 1, &t[6], NULL) == PC_OK);
    CHECK(pc_paint_star_tip(48, &t[7], NULL) == PC_OK);
    CHECK(pc_paint_rake_tip(48, 1, 7, &t[8], NULL) == PC_OK);
    for (int i = 0; i < 9; i++) {
        float sum = -0.0f;
        CHECK(pc_paint_gray_tile_is_valid(&t[i]));
        for (uint32_t y = 0; y < 48; y++) {
            for (uint32_t x = 0; x < 48; x++) {
                sum += pc_paint_gray_tile_get(&t[i], x, y);
            }
        }
        CHECK(sum > 48.0f);
        CHECK(pc_paint_gray_tile_get(&t[i], 0, 0) == 0.0f);
        pc_paint_gray_tile_fini(&t[i]);
    }
}

/* ---------- never crash ---------- */

static void test_hostile_brushes_render_at_every_depth(void)
{
    const float bad[6] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, 0.0f};
    const pc_pixel_format fs[4] = {PC_PIXEL_FORMAT_RGBA8, PC_PIXEL_FORMAT_RGBA32F, PC_PIXEL_FORMAT_GRAY8,
                                   PC_PIXEL_FORMAT_CMYKA8};
    pc_paint_point pts[3] = {pc_paint_point_make(5, 5, 1), pc_paint_point_make(30, 12, NAN),
                             pc_paint_point_make(-8, 40, INFINITY)};
    pts[1].tilt_x = NAN;
    pts[2].time = -INFINITY;
    for (int k = 0; k < 6; k++) {
        float v = bad[k];
        pc_paint_brush b = tb_brush();
        b.size = 12.0f;
        b.hardness = v;
        b.roundness = v;
        b.angle = v;
        b.opacity = v;
        b.flow = v;
        b.color[1] = v;
        b.build_up = true;
        b.build_up_rate = v;
        b.shape_dynamics = (pc_paint_shape_dynamics){true,
                                                     {v, PC_PAINT_CONTROL_FADE, 0, v},
                                                     v,
                                                     {v, PC_PAINT_CONTROL_PEN_TILT, 3, v},
                                                     {v, PC_PAINT_CONTROL_ROTATION, 1, v},
                                                     true,
                                                     true,
                                                     true};
        b.scattering = (pc_paint_scattering){
            true, {v, PC_PAINT_CONTROL_STYLUS_WHEEL, 25, v}, true, 99, {v, PC_PAINT_CONTROL_FADE, 0, v}};
        b.texture.enabled = true;
        b.texture.each_tip = k % 2 == 0;
        b.texture.scale = v;
        b.texture.depth = v;
        b.texture.brightness = v;
        b.texture.contrast = v;
        b.texture.pattern.size = 0xFFFFFFFFu;
        b.dual_brush.enabled = true;
        b.dual_brush.size = v;
        b.dual_brush.spacing = v;
        b.dual_brush.scatter = v;
        b.dual_brush.count = 0;
        b.color_dynamics =
            (pc_paint_color_dynamics){true, k % 2 == 1, {v, PC_PAINT_CONTROL_PEN_PRESSURE, 0, v}, v, v, v, v};
        b.transfer = (pc_paint_transfer){true,
                                         {v, PC_PAINT_CONTROL_FADE, 0, v},
                                         {v, PC_PAINT_CONTROL_OFF, 0, v},
                                         {v, PC_PAINT_CONTROL_OFF, 0, v},
                                         {v, PC_PAINT_CONTROL_OFF, 0, v}};
        b.smoothing.amount = v;
        b.wet_edges = true;
        b.noise = true;
        b.aliased = k % 3 == 0;
        b.mode = k % 2 == 0 ? PC_BLEND_DISSOLVE : PC_BLEND_HUE;
        for (int f = 0; f < 4; f++) {
            pc_surface s;
            pc_rect dmg;
            pc_status st;
            pc_surface_init(&s, fs[f]);
            st = pc_paint_render_stroke(&s, &b, pts, 3, NULL, k == 3, v, &dmg, NULL);
            CHECK(st == PC_OK);
            pc_surface_fini(&s);
        }
    }
}

static void test_empty_strokes(void)
{
    pc_surface s, pre;
    pc_paint_brush b = tb_brush();
    pc_paint_renderer *r = NULL, *t = (pc_paint_renderer *)1;
    pc_rect dmg = pc_rect_make(1, 2, 3, 4);
    pc_paint_dab_vec d = {0};
    float *cv = (float *)1;
    size_t n = 7;
    pc_pixel_format f = PC_PIXEL_FORMAT_RGBA8;
    pc_surface_init(&s, f);
    CHECK(pc_paint_render_stroke(&s, &b, NULL, 0, NULL, false, 1.0f, &dmg, NULL) == PC_OK);
    CHECK(pc_rect_is_empty(dmg));
    CHECK(pc_paint_dabs(&b, NULL, 0, &d, NULL) == PC_OK && d.len == 0);
    CHECK(pc_surface_clone(&s, &pre, NULL) == PC_OK);
    CHECK(pc_paint_renderer_new(&b, &f, 1.0f, &r, NULL) == PC_OK);
    CHECK(pc_paint_renderer_push(r, NULL, 0, NULL) == PC_OK);
    CHECK(pc_paint_renderer_tail_preview(r, &t, NULL) == PC_OK && t == NULL);
    CHECK(pc_paint_renderer_finish(r, NULL) == PC_OK);
    CHECK(pc_paint_renderer_dab_count(r) == 0);
    CHECK(pc_paint_renderer_composite(r, &pre, &s, NULL, false, true, &dmg, NULL) == PC_OK &&
          pc_rect_is_empty(dmg));
    CHECK(pc_paint_renderer_dense_coverage(r, &dmg, &cv, &n, NULL) == PC_OK && cv == NULL && n == 0);
    CHECK(pc_rect_is_empty(pc_paint_renderer_take_dirty_rect(r)));
    pc_paint_renderer_mark_dirty(r, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX));
    CHECK(pc_paint_renderer_coverage_at(r, 0, 0) == 0.0f);
    pc_paint_renderer_free(r);
    pc_paint_renderer_free(NULL);
    pc_surface_fini(&s);
    pc_surface_fini(&pre);
}

static void test_huge_sizes_are_limits_not_crashes(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_context ctx;
    pc_paint_dab d = pc_paint_dab_round(pc_point_make(0, 0), 1e9f, 1.0f);
    pc_paint_fbuf v = {0};
    pc_paint_gray_tile t = PC_PAINT_GRAY_TILE_INIT;
    pc_paint_renderer *r = NULL;
    pc_paint_point pts[2] = {pc_paint_point_make(0, 0, 1), pc_paint_point_make(10, 0, 1)};
    pc_error err;
    float *cv = NULL;
    size_t n = 0;
    CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
    CHECK(pc_paint_context_rasterize(&ctx, &d, false, pc_paint_context_dab_rect(&ctx, &d, false), &v, &err) ==
          PC_ERR_LIMIT);
    d.radius = INFINITY;
    CHECK(pc_paint_context_rasterize(&ctx, &d, false, pc_paint_context_dab_rect(&ctx, &d, false), &v, NULL) ==
          PC_ERR_LIMIT);
    pc_paint_context_fini(&ctx);
    pc_paint_fbuf_fini(&v);
    CHECK(pc_paint_gray_tile_from_fn(UINT32_MAX, UINT32_MAX, diag_fn, NULL, &t, NULL) == PC_ERR_LIMIT);
    CHECK(t.data == NULL); /* untouched */
    CHECK(pc_paint_gray_tile_from_f32(2, 2, NULL, 3, &t, NULL) == PC_ERR_INVALID);
    CHECK(pc_paint_spatter_tip(UINT32_MAX, 1, 3, &t, NULL) == PC_ERR_LIMIT);
    CHECK(pc_paint_mips_new(&t, &(pc_paint_mips *){NULL}, NULL) == PC_ERR_INVALID);
    CHECK(pc_paint_renderer_new(&b, NULL, 1.0f, &r, NULL) == PC_OK);
    CHECK(pc_paint_renderer_push(r, pts, 2, NULL) == PC_OK);
    CHECK(pc_paint_renderer_coverage_in(r, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), &cv, &n,
                                        NULL) == PC_ERR_LIMIT);
    pc_paint_renderer_free(r);
}

static void test_runaway_segment_stops_at_the_step_limit(void)
{
    /* A segment of 1e9 px at 0.5 px spacing would never finish in the reference. */
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2] = {pc_paint_point_make(0, 0, 1), pc_paint_point_make(1e9, 0, 1)};
    pc_paint_dab_vec d = {0};
    pc_error err;
    b.size = 1.0f;
    b.spacing = 0.01f;
    CHECK(pc_paint_dabs(&b, pts, 2, &d, &err) == PC_ERR_LIMIT);
    CHECK_CONTAINS(err.msg, "dab steps");
    CHECK(d.len <= PC_PAINT_MAX_STEPS);
    pc_paint_dab_vec_fini(&d);
    /* Infinite segments too. */
    pts[1].x = INFINITY;
    CHECK(pc_paint_dabs(&b, pts, 2, &d, NULL) == PC_ERR_LIMIT);
    pc_paint_dab_vec_fini(&d);
}

static void test_bad_json_is_an_error(void)
{
    static const char *const inputs[] = {
        "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[["
        "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]"
        "]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]",
        "{\"tip\": {\"sampled\": {\"width\": 65536, \"height\": 65536, \"data\": []}}}",
        "{\"texture\": {\"pattern\": {\"tile\": {\"width\": 1, \"height\": 1, \"data\": \"\\u0000===\"}}}}",
        "{\"color\": [1e400, 0, 0, 1]}",
        "{\"seed\": 1e30}",
        "{\"name\": \"\\ud800\", \"brush\": {}}",
        "",
        "{",
    };
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        pc_paint_brush b;
        unsigned char sentinel[sizeof b];
        memset(&b, 0xAB, sizeof b);
        memset(sentinel, 0xAB, sizeof sentinel);
        CHECK(pc_paint_json_from_string(PC_PAINT_JSON_BRUSH, inputs[i], strlen(inputs[i]), &b, NULL) !=
              PC_OK);
        CHECK(memcmp(&b, sentinel, sizeof b) == 0); /* out-param untouched on failure */
    }
    {
        pc_paint_preset p;
        const char *j = "{\"name\": \"\\ud800\", \"brush\": {}}";
        CHECK(pc_paint_json_from_string(PC_PAINT_JSON_PRESET, j, strlen(j), &p, NULL) != PC_OK);
    }
    CHECK(pc_paint_json_size((pc_paint_json_type)99) == 0);
    CHECK(pc_paint_json_read((pc_paint_json_type)99, NULL, &(int){0}, NULL) == PC_ERR_INVALID);
}

static void test_mismatched_pre_surface_is_invalid(void)
{
    pc_paint_brush b = tb_brush();
    pc_pixel_format f = PC_PIXEL_FORMAT_RGBA8;
    pc_surface target, pre;
    pc_paint_renderer *r = NULL;
    pc_paint_point p = pc_paint_point_make(5, 5, 1);
    pc_rect d;
    pc_surface_init(&target, f);
    pc_surface_init(&pre, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_paint_renderer_new(&b, &f, 1.0f, &r, NULL) == PC_OK);
    CHECK(pc_paint_renderer_push(r, &p, 1, NULL) == PC_OK);
    CHECK(pc_paint_renderer_finish(r, NULL) == PC_OK);
    CHECK(pc_paint_renderer_composite(r, &pre, &target, NULL, false, true, &d, NULL) == PC_ERR_INVALID);
    pc_paint_renderer_free(r);
    pc_surface_fini(&target);
    pc_surface_fini(&pre);
}

static void test_copies_share_tiles(void)
{
    pc_paint_brush a = tb_brush(), b, c;
    a.tip.kind = PC_PAINT_TIP_SAMPLED;
    a.tip.tile = tb_left_tile(4, 4, 2);
    pc_paint_brush_copy(&b, &a);
    CHECK(b.tip.tile.data == a.tip.tile.data);
    CHECK(pc_paint_brush_eq(&a, &b));
    pc_paint_brush_fini(&a);
    CHECK(pc_paint_gray_tile_len(&b.tip.tile) == 16); /* still alive through b */
    c = tb_brush();
    pc_paint_brush_assign(&c, &b);
    pc_paint_brush_assign(&c, &c); /* self-assignment keeps it */
    CHECK(pc_paint_brush_eq(&c, &b));
    pc_paint_brush_fini(&b);
    pc_paint_brush_fini(&c);
    pc_paint_brush_fini(NULL);
}

/* ---------- threads ---------- */

typedef struct mt_ctx {
    const pc_paint_brush *brush;
    const pc_paint_context *ctx;
    uint64_t hash[16];
} mt_ctx;

/* Each chunk copies the shared context (retaining its mips and texture) and the brush (retaining
 * its tiles), renders a stroke on its own surface and hashes it. */
static pc_status mt_chunk(void *vp, size_t chunk, size_t begin, size_t end)
{
    mt_ctx *m = vp;
    (void)begin;
    (void)end;
    for (int rep = 0; rep < 3; rep++) {
        pc_paint_brush b;
        pc_paint_context c;
        pc_surface s;
        pc_rect d;
        pc_paint_point pts[6];
        uint64_t h = 0xcbf29ce484222325ull;
        pc_paint_brush_copy(&b, m->brush);
        pc_paint_context_copy(&c, m->ctx);
        for (int i = 0; i < 6; i++) {
            pts[i] = pc_paint_point_make(10.0 + i * 15.0 + (double)chunk, 30.0 + (i % 2) * 9.0, 0.8f);
        }
        pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA16);
        if (pc_paint_render_stroke(&s, &b, pts, 6, NULL, false, 1.0f, &d, NULL) != PC_OK) {
            return PC_ERR_INVALID;
        }
        for (int32_t y = 0; y < 60; y++) {
            for (int32_t x = 0; x < 120; x++) {
                float px[4];
                pc_surface_read_pixel(&s, x, y, px);
                for (int k = 0; k < 4; k++) {
                    uint32_t u;
                    memcpy(&u, &px[k], 4);
                    h = (h ^ u) * 0x100000001b3ull;
                }
            }
        }
        m->hash[chunk] = h;
        pc_surface_fini(&s);
        pc_paint_context_fini(&c);
        pc_paint_brush_fini(&b);
    }
    return PC_OK;
}

static void test_shared_brushes_across_threads(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_context ctx;
    pc_pool *pool = NULL;
    mt_ctx one, many;
    b.size = 18.0f;
    b.tip.kind = PC_PAINT_TIP_SAMPLED;
    b.tip.tile = tb_left_tile(12, 12, 5);
    b.texture.enabled = true;
    b.texture.pattern.kind = PC_PAINT_PATTERN_TILE;
    pc_paint_gray_tile_copy(&b.texture.pattern.tile, &b.tip.tile);
    b.dual_brush.enabled = true;
    pc_paint_tip_copy(&b.dual_brush.tip, &b.tip);
    CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
    memset(&one, 0, sizeof one);
    memset(&many, 0, sizeof many);
    one.brush = many.brush = &b;
    one.ctx = many.ctx = &ctx;
    CHECK(pc_pool_for(NULL, 16, 1, mt_chunk, &one) == PC_OK);
    CHECK(pc_pool_new(4, &pool, NULL) == PC_OK);
    CHECK(pc_pool_for(pool, 16, 1, mt_chunk, &many) == PC_OK);
    pc_pool_free(pool);
    CHECK(memcmp(one.hash, many.hash, sizeof one.hash) == 0); /* identical with 1 or N threads */
    pc_paint_context_fini(&ctx);
    pc_paint_brush_fini(&b);
}

int main(int argc, char **argv)
{
    RUN(test_dab_spacing);
    RUN(test_coverage_profile);
    RUN(test_hard_brush_paints_solid_line);
    RUN(test_opacity_caps_overlap);
    RUN(test_selection_limits_paint);
    RUN(test_eraser_removes_alpha);
    RUN(test_lock_transparency_preserves_alpha);
    RUN(test_pressure_changes_size);
    RUN(test_smoothing_keeps_endpoints);
    RUN(test_works_at_all_depths);
    RUN(test_rng_deterministic_and_uniformish);
    RUN(test_base64_round_trip);
    RUN(test_tile_serde_round_trip_and_float_form);
    RUN(test_mips_average);
    RUN(test_builtins_are_grouped_and_unique);
    RUN(test_patterns_are_seamless_and_in_range);
    RUN(test_tips_have_paint);
    RUN(test_hostile_brushes_render_at_every_depth);
    RUN(test_empty_strokes);
    RUN(test_huge_sizes_are_limits_not_crashes);
    RUN(test_runaway_segment_stops_at_the_step_limit);
    RUN(test_bad_json_is_an_error);
    RUN(test_mismatched_pre_surface_is_invalid);
    RUN(test_copies_share_tiles);
    RUN(test_shared_brushes_across_threads);
    return pc_t_finish();
}
