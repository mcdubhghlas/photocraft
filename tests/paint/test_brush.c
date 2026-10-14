/* Brush-engine tests, part 1 (crates/paint/src/brush_tests.rs: tips, spacing, dynamics). */
#include <math.h>

#include "support.h"

/* ---------- tips ---------- */

static void test_round_tip_roundness_and_angle(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_dab d = pc_paint_dab_round(pc_point_make(50.0, 50.0), 20.0f, 1.0f);
    tb_raster r;
    d.roundness = 0.25f;
    r = tb_rast(&b, &d, false);
    /* Wide along x, thin along y. */
    CHECK(tb_at(&r, 50 + 15, 50) > 0.99f);
    CHECK(tb_at(&r, 50, 50 + 8) < 0.01f);
    CHECK(tb_at(&r, 50, 50 + 3) > 0.99f);
    pc_paint_fbuf_fini(&r.v);
    /* Rotated 90 degrees counter-clockwise: now tall. */
    d.angle = 1.57079632679489661923f;
    r = tb_rast(&b, &d, false);
    CHECK(tb_at(&r, 50, 50 - 15) > 0.99f);
    CHECK(tb_at(&r, 50 + 8, 50) < 0.01f);
    pc_paint_fbuf_fini(&r.v);
    /* 45 degrees: the long axis runs up-right (y-down image space: x+, y-). */
    d.angle = 0.785398163397448309616f;
    r = tb_rast(&b, &d, false);
    CHECK(tb_at(&r, 60, 40) > 0.99f);
    CHECK(tb_at(&r, 60, 60) < 0.01f);
    pc_paint_fbuf_fini(&r.v);
}

static void test_hardness_profile_is_monotonic(void)
{
    const float hs[3] = {0.0f, 0.5f, 1.0f};
    for (int k = 0; k < 3; k++) {
        pc_paint_brush b = tb_brush();
        pc_paint_dab d = pc_paint_dab_round(pc_point_make(50.0, 50.0), 20.0f, 1.0f);
        tb_raster r;
        float prof[64], mid;
        /* A soft tip paints past its 20 px radius, as Photoshop's does, out to its reach. */
        int n = (int)ceilf(pc_paint_tip_reach(20.0f, hs[k])) + 1;
        b.hardness = hs[k];
        if (n > 64) {
            n = 64;
        }
        r = tb_rast(&b, &d, false);
        for (int i = 0; i < n; i++) {
            prof[i] = tb_at(&r, 50 + i, 50);
        }
        for (int i = 1; i < n; i++) {
            CHECK(prof[i] <= prof[i - 1] + 1e-6f);
        }
        CHECK(prof[0] > 0.99f);
        CHECK(prof[n - 1] < 0.002f);
        mid = tb_at(&r, 60, 50);
        if (hs[k] == 1.0f) {
            CHECK(mid > 0.99f);
        } else if (hs[k] == 0.0f) {
            CHECK(mid > 0.2f && mid < 0.8f);
        }
        pc_paint_fbuf_fini(&r.v);
    }
}

static void test_hard_edge_is_sharp(void)
{
    /* A hardness of 1 is a steep rim: opaque inside the nominal radius, empty just past it. */
    pc_paint_brush b = tb_brush(), soft = tb_brush();
    pc_paint_dab d = pc_paint_dab_round(pc_point_make(50.0, 50.0), 20.0f, 1.0f);
    tb_raster r = tb_rast(&b, &d, false), rs;
    int band = 0, soft_band = 0;
    float prof[22];
    for (int i = 0; i < 22; i++) {
        prof[i] = tb_at(&r, 50 + i, 50);
        band += prof[i] >= 0.01f && prof[i] < 0.99f;
    }
    CHECK(band <= 3);
    CHECK(prof[17] > 0.99f);
    CHECK(prof[21] < 0.01f);
    /* A soft tip spreads the same crossing over many pixels instead. */
    soft.hardness = 0.0f;
    rs = tb_rast(&soft, &d, false);
    for (int i = 0; i < 22; i++) {
        float a = tb_at(&rs, 50 + i, 50);
        soft_band += a >= 0.01f && a < 0.99f;
    }
    CHECK(soft_band >= 10);
    pc_paint_fbuf_fini(&r.v);
    pc_paint_fbuf_fini(&rs.v);
}

static void test_sampled_tip_is_scaled_and_oriented(void)
{
    /* Tip: left half painted. */
    pc_paint_brush b = tb_brush();
    pc_paint_dab d = pc_paint_dab_round(pc_point_make(50.0, 50.0), 20.0f, 1.0f), e;
    tb_raster r;
    float sum = 0.0f;
    b.tip.kind = PC_PAINT_TIP_SAMPLED;
    b.tip.tile = tb_left_tile(16, 16, 8);
    r = tb_rast(&b, &d, false);
    CHECK(tb_at(&r, 40, 50) > 0.99f && tb_at(&r, 60, 50) < 0.01f);
    pc_paint_fbuf_fini(&r.v);
    /* Flip X mirrors it. */
    e = d;
    e.flip_x = true;
    r = tb_rast(&b, &e, false);
    CHECK(tb_at(&r, 40, 50) < 0.01f && tb_at(&r, 60, 50) > 0.99f);
    pc_paint_fbuf_fini(&r.v);
    /* Rotated 90 degrees CCW: the painted (left) half moves to the bottom. */
    e = d;
    e.angle = 1.57079632679489661923f;
    r = tb_rast(&b, &e, false);
    CHECK(tb_at(&r, 50, 60) > 0.99f && tb_at(&r, 50, 40) < 0.01f);
    pc_paint_fbuf_fini(&r.v);
    /* Heavily downscaled uses mips: still sensible coverage. */
    e = pc_paint_dab_round(pc_point_make(50.0, 50.0), 2.0f, 1.0f);
    r = tb_rast(&b, &e, false);
    for (size_t i = 0; i < r.v.len; i++) {
        sum += r.v.v[i];
    }
    CHECK(sum > 1.0f);
    pc_paint_fbuf_fini(&r.v);
    pc_paint_brush_fini(&b);
}

static void test_pencil_is_aliased(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2];
    pc_surface s;
    bool partial = false, binary = true;
    b.aliased = true;
    b.hardness = 0.0f;
    b.size = 9.0f;
    tb_line(10.0, 50.0, 20.5, pts);
    tb_paint(&s, &b, pts, 2);
    for (int y = 10; y < 32; y++) {
        for (int x = 0; x < 64; x++) {
            float a = tb_alpha(&s, x, y);
            binary = binary && (a == 0.0f || a == 1.0f);
        }
    }
    CHECK(binary);
    CHECK(tb_alpha(&s, 30, 20) == 1.0f);
    pc_surface_fini(&s);
    /* Anti-aliased brush has partial pixels. */
    b.aliased = false;
    tb_line(10.0, 50.0, 20.3, pts);
    tb_paint(&s, &b, pts, 2);
    for (int y = 10; y < 32 && !partial; y++) {
        for (int x = 0; x < 64; x++) {
            float a = tb_alpha(&s, x, y);
            partial = partial || (a >= 0.01f && a < 0.99f);
        }
    }
    CHECK(partial);
    pc_surface_fini(&s);
}

static void test_pencil_dabs_sit_on_the_pixel_grid(void)
{
    float cx, cy;
    double sq[4];
    pc_paint_brush b = tb_brush();
    pc_paint_point one = pc_paint_point_make(7.0, 4.0, 1.0f), pts[2];
    pc_surface s;
    bool ok = true;
    /* Odd sizes centre on the pixel holding the point, even ones on the nearest corner. */
    pc_paint_grid_center(10.0, 5.0, 1.0f, &cx, &cy);
    CHECK(cx == 10.5f && cy == 5.5f);
    pc_paint_grid_center(10.9, 5.2, 3.0f, &cx, &cy);
    CHECK(cx == 10.5f && cy == 5.5f);
    pc_paint_grid_center(10.4, 5.6, 2.0f, &cx, &cy);
    CHECK(cx == 10.0f && cy == 6.0f);
    pc_paint_grid_square(10.2, 5.7, 1.0f, sq);
    CHECK(sq[0] == 10.0 && sq[1] == 5.0 && sq[2] == 11.0 && sq[3] == 6.0);
    pc_paint_grid_square(10.2, 5.7, 4.0f, sq);
    CHECK(sq[0] == 8.0 && sq[1] == 4.0 && sq[2] == 12.0 && sq[3] == 8.0);
    pc_paint_grid_square(NAN, 0.0, NAN, sq); /* no crash */
    /* A 1 px pencil paints exactly the pixel under each point, even on a pixel corner. */
    b.aliased = true;
    b.hardness = 1.0f;
    b.size = 1.0f;
    tb_paint(&s, &b, &one, 1);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            ok = ok && tb_alpha(&s, x, y) == ((x == 7 && y == 4) ? 1.0f : 0.0f);
        }
    }
    CHECK(ok);
    pc_surface_fini(&s);
    /* A horizontal 1 px line is one pixel thick and unbroken. */
    tb_line(2.0, 20.0, 9.3, pts);
    tb_paint(&s, &b, pts, 2);
    for (int x = 2; x < 20; x++) {
        CHECK(tb_alpha(&s, x, 9) == 1.0f);
        CHECK(tb_alpha(&s, x, 8) + tb_alpha(&s, x, 10) == 0.0f);
    }
    pc_surface_fini(&s);
    /* A 4 px dab stays inside its cursor square. */
    b.size = 4.0f;
    one = pc_paint_point_make(10.2, 5.7, 1.0f);
    tb_paint(&s, &b, &one, 1);
    pc_paint_grid_square(10.2, 5.7, 4.0f, sq);
    ok = true;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 24; x++) {
            bool inside = x >= sq[0] && x < sq[2] && y >= sq[1] && y < sq[3];
            ok = ok && (inside || tb_alpha(&s, x, y) == 0.0f);
        }
    }
    CHECK(ok);
    CHECK(tb_alpha(&s, 9, 5) == 1.0f);
    pc_surface_fini(&s);
}

/* ---------- spacing ---------- */

static void test_spacing_sets_dab_count(void)
{
    const struct {
        float spacing;
        double len;
        size_t expect;
    } cases[3] = {{0.25f, 100.0, 21}, {1.0f, 100.0, 6}, {0.5f, 200.0, 21}};
    pc_paint_point pts[21];
    pc_paint_dab_vec d;
    pc_paint_brush b;
    for (int i = 0; i < 3; i++) {
        pc_paint_point l[2];
        b = tb_brush();
        b.spacing = cases[i].spacing;
        b.size = 20.0f;
        tb_line(0.0, cases[i].len, 0.0, l);
        d = tb_dabs(&b, l, 2);
        CHECK_EQ_UINT(d.len, cases[i].expect);
        pc_paint_dab_vec_fini(&d);
    }
    /* Spacing carries across segments: a polyline gives the same count as the straight line. */
    b = tb_brush();
    b.spacing = 0.5f;
    b.size = 20.0f;
    for (int i = 0; i <= 20; i++) {
        pts[i] = pc_paint_point_make(i * 10.0, 0.0, 1.0f);
    }
    d = tb_dabs(&b, pts, 21);
    CHECK_EQ_UINT(d.len, 21);
    for (size_t i = 1; i < d.len; i++) {
        CHECK(fabs((d.v[i].center.x - d.v[i - 1].center.x) - 10.0) < 1e-6);
    }
    pc_paint_dab_vec_fini(&d);
}

static void test_stroke_has_no_seams(void)
{
    /* Overlapping dabs fuse into one solid band: the minimum coverage inside stays at the opacity. */
    const float sp[3] = {0.1f, 0.25f, 0.5f};
    for (int k = 0; k < 3; k++) {
        pc_paint_brush b = tb_brush();
        pc_paint_point pts[2];
        pc_surface s;
        bool ok = true;
        b.spacing = sp[k];
        b.size = 20.0f;
        tb_line(20.0, 180.0, 100.5, pts);
        tb_paint(&s, &b, pts, 2);
        for (int y = 98; y <= 102; y++) {
            for (int x = 40; x <= 160; x++) {
                ok = ok && tb_alpha(&s, x, y) > 0.99f;
            }
        }
        CHECK(ok);
        pc_surface_fini(&s);
    }
}

/* ---------- dynamics ---------- */

static void test_pressure_controls_size_with_minimum(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2] = {pc_paint_point_make(0.0, 0.0, 0.0f), pc_paint_point_make(200.0, 0.0, 1.0f)};
    pc_paint_dab_vec d;
    b.size = 40.0f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size.control = PC_PAINT_CONTROL_PEN_PRESSURE;
    b.shape_dynamics.size.minimum = 0.25f;
    d = tb_dabs(&b, pts, 2);
    CHECK(d.len > 2);
    CHECK_NEAR(d.v[0].radius, 5.0, 1e-3); /* min diameter 25 % of 40 -> r 5 */
    CHECK_NEAR(d.v[d.len - 1].radius, 20.0, 0.5);
    for (size_t i = 1; i < d.len; i++) {
        CHECK(d.v[i].radius >= d.v[i - 1].radius - 1e-4f);
    }
    pc_paint_dab_vec_fini(&d);
}

static void test_fade_ramps_over_steps(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2];
    pc_paint_dab_vec d;
    bool ok = true;
    b.size = 20.0f;
    b.spacing = 0.5f;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = (pc_paint_dynamic){0.0f, PC_PAINT_CONTROL_FADE, 10, 0.0f};
    tb_line(0.0, 300.0, 0.0, pts);
    d = tb_dabs(&b, pts, 2);
    CHECK(d.len > 10);
    CHECK_NEAR(d.v[0].radius, 10.0, 1e-3);
    CHECK(d.v[5].radius < d.v[0].radius * 0.6f);
    for (size_t i = 10; i < d.len; i++) {
        ok = ok && d.v[i].radius <= 0.5f + 1e-6f;
    }
    CHECK(ok);
    pc_paint_dab_vec_fini(&d);
    /* Fade on opacity (Transfer). */
    b = tb_brush();
    b.transfer.enabled = true;
    b.transfer.opacity = (pc_paint_dynamic){0.0f, PC_PAINT_CONTROL_FADE, 4, 0.0f};
    tb_line(0.0, 100.0, 0.0, pts);
    d = tb_dabs(&b, pts, 2);
    CHECK(d.len > 4);
    CHECK(d.v[0].opacity == 1.0f);
    CHECK(d.v[4].opacity == 0.0f);
    pc_paint_dab_vec_fini(&d);
}

static pc_paint_brush angle_brush(pc_paint_control c)
{
    pc_paint_brush b = tb_brush();
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.angle = pc_paint_dynamic_controlled(c);
    return b;
}

static void test_tilt_direction_and_rotation_drive_angle(void)
{
    pc_paint_point p = pc_paint_point_make(0.0, 0.0, 1.0f), pts[2];
    pc_paint_brush b = angle_brush(PC_PAINT_CONTROL_PEN_TILT);
    pc_paint_dab_vec d;
    bool ok = true;
    p.tilt_x = 0.0f;
    p.tilt_y = -45.0f; /* pen leaning "up" the screen */
    pts[0] = p;
    pts[1] = p;
    pts[1].x = 50.0;
    d = tb_dabs(&b, pts, 2);
    CHECK_NEAR(tb_deg(d.v[0].angle), 90.0, 1e-3);
    pc_paint_dab_vec_fini(&d);
    /* Direction: travelling down the screen = -90 degrees (counter-clockwise positive). */
    b = angle_brush(PC_PAINT_CONTROL_DIRECTION);
    pts[0] = pc_paint_point_make(0.0, 0.0, 1.0f);
    pts[1] = pc_paint_point_make(0.0, 50.0, 1.0f);
    d = tb_dabs(&b, pts, 2);
    for (size_t i = 0; i < d.len; i++) {
        ok = ok && fabsf(tb_deg(d.v[i].angle) + 90.0f) < 1e-3f;
    }
    CHECK(ok && d.len > 0);
    pc_paint_dab_vec_fini(&d);
    /* Rotation. */
    b = angle_brush(PC_PAINT_CONTROL_ROTATION);
    pts[0] = pc_paint_point_make(0.0, 0.0, 1.0f);
    pts[0].rotation = 30.0f;
    pts[1] = pts[0];
    pts[1].x = 20.0;
    d = tb_dabs(&b, pts, 2);
    CHECK_NEAR(tb_deg(d.v[0].angle), 30.0, 1e-3);
    pc_paint_dab_vec_fini(&d);
    /* Pen tilt on size: upright pen full size, flat pen minimum. */
    b = tb_brush();
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = pc_paint_dynamic_controlled(PC_PAINT_CONTROL_PEN_TILT);
    b.size = 20.0f;
    pts[0] = pc_paint_point_make(0.0, 0.0, 1.0f);
    pts[0].tilt_x = 90.0f;
    d = tb_dabs(&b, pts, 1);
    CHECK(d.len == 1 && d.v[0].radius <= 0.5f + 1e-6f);
    pc_paint_dab_vec_fini(&d);
}

static void test_jitter_is_deterministic_per_seed(void)
{
    pc_paint_brush b = tb_brush(), c;
    pc_paint_point pts[2], many[31];
    pc_paint_dab_vec a, a2, cc, one;
    pc_paint_renderer *r = NULL;
    bool same = true, any_small = false, all_le = true, round_ok = true, fx = false, nfx = false,
         diff = false;
    const pc_paint_dab *rec;
    size_t nrec = 0;
    b.size = 30.0f;
    b.seed = 42;
    b.shape_dynamics.enabled = true;
    b.shape_dynamics.size = pc_paint_dynamic_jitter(1.0f);
    b.shape_dynamics.angle = pc_paint_dynamic_jitter(1.0f);
    b.shape_dynamics.roundness = (pc_paint_dynamic){1.0f, PC_PAINT_CONTROL_OFF, 25, 0.2f};
    b.shape_dynamics.flip_x_jitter = true;
    b.shape_dynamics.flip_y_jitter = true;
    b.scattering =
        (pc_paint_scattering){true, pc_paint_dynamic_jitter(2.0f), true, 3, pc_paint_dynamic_jitter(1.0f)};
    tb_line(0.0, 300.0, 50.0, pts);
    a = tb_dabs(&b, pts, 2);
    a2 = tb_dabs(&b, pts, 2);
    same = a.len == a2.len;
    for (size_t i = 0; same && i < a.len; i++) {
        same = pc_paint_dab_eq(&a.v[i], &a2.v[i]);
    }
    CHECK(same);
    c = b;
    c.seed = 43;
    cc = tb_dabs(&c, pts, 2);
    diff = cc.len != a.len;
    for (size_t i = 0; !diff && i < a.len; i++) {
        diff = !pc_paint_dab_eq(&a.v[i], &cc.v[i]);
    }
    CHECK(diff);
    /* Jitter actually varies things, within their ranges. */
    for (size_t i = 0; i < a.len; i++) {
        any_small = any_small || a.v[i].radius < 10.0f;
        all_le = all_le && a.v[i].radius <= 15.0f + 1e-4f;
        round_ok = round_ok && a.v[i].roundness >= 0.2f - 1e-6f && a.v[i].roundness <= 1.0f;
        fx = fx || a.v[i].flip_x;
        nfx = nfx || !a.v[i].flip_x;
    }
    CHECK(any_small && all_le && round_ok && fx && nfx);
    /* Chunked feeding gives identical dabs. */
    CHECK(pc_paint_renderer_new(&b, NULL, 1.0f, &r, NULL) == PC_OK);
    pc_paint_renderer_record_dabs(r);
    for (int i = 0; i <= 30; i++) {
        many[i] = pc_paint_point_make(i * 10.0, 50.0, 1.0f);
    }
    for (size_t i = 0; i < 31; i += 4) {
        CHECK(pc_paint_renderer_push(r, &many[i], 31 - i < 4 ? 31 - i : 4, NULL) == PC_OK);
    }
    CHECK(pc_paint_renderer_finish(r, NULL) == PC_OK);
    rec = pc_paint_renderer_dabs(r, &nrec);
    one = tb_dabs(&b, many, 31);
    same = nrec == one.len;
    for (size_t i = 0; same && i < nrec; i++) {
        same = pc_paint_dab_eq(&rec[i], &one.v[i]);
    }
    CHECK(same);
    pc_paint_renderer_free(r);
    pc_paint_dab_vec_fini(&a);
    pc_paint_dab_vec_fini(&a2);
    pc_paint_dab_vec_fini(&cc);
    pc_paint_dab_vec_fini(&one);
}

static void test_scattering_stays_in_bounds(void)
{
    for (int both = 0; both < 2; both++) {
        pc_paint_brush b = tb_brush();
        pc_paint_point pts[2];
        pc_paint_dab_vec d;
        const double max_off = 2.0 * 10.0;
        bool in = true, far = false, grid = true, off_grid = false;
        b.size = 20.0f;
        b.scattering = (pc_paint_scattering){true, pc_paint_dynamic_jitter(2.0f), both == 1, 4,
                                             pc_paint_dynamic_default()};
        tb_line(0.0, 400.0, 100.0, pts);
        d = tb_dabs(&b, pts, 2);
        /* count 4 per spacing step (5 px) -> 81 steps. */
        CHECK_EQ_UINT(d.len, 81 * 4);
        for (size_t i = 0; i < d.len; i++) {
            double dy = fabs(d.v[i].center.y - 100.0), gx = d.v[i].center.x / 5.0;
            in = in && dy <= max_off + 1e-6;
            far = far || dy > max_off * 0.5;
            grid = grid && fabs(gx - round(gx)) < 1e-6;
            off_grid = off_grid || fabs(gx - round(gx)) > 0.1;
        }
        CHECK(in && far);
        /* Perpendicular only: x stays on the spacing grid. */
        CHECK(both ? off_grid : grid);
        pc_paint_dab_vec_fini(&d);
    }
}

static void test_zero_scatter_keeps_dabs_on_the_stroke(void)
{
    /* Scatter at 0 % must not move a dab, even with the section switched on. */
    for (int k = 0; k < 2; k++) {
        pc_paint_brush b = tb_brush();
        pc_paint_point pts[2];
        pc_paint_dab_vec d;
        bool ok = true;
        b.scattering.enabled = k == 1;
        tb_line(10.0, 90.0, 40.5, pts);
        d = tb_dabs(&b, pts, 2);
        CHECK(d.len > 4);
        for (size_t i = 0; i < d.len; i++) {
            ok = ok && fabs(d.v[i].center.y - 40.5) < 1e-9;
        }
        CHECK(ok);
        pc_paint_dab_vec_fini(&d);
    }
}

static pc_paint_brush color_base(void)
{
    pc_paint_brush b = tb_brush();
    const float fg[4] = {1, 0, 0, 1}, bg[4] = {0, 0, 1, 1};
    memcpy(b.color, fg, sizeof fg);
    memcpy(b.background, bg, sizeof bg);
    b.seed = 5;
    return b;
}

static void test_color_dynamics_ranges(void)
{
    pc_paint_point pts[2];
    pc_paint_dab_vec d;
    pc_paint_brush b = color_base();
    pc_surface s;
    bool ok = true, blue = false, red = false;
    /* FG/BG jitter mixes between the two colours only. */
    b.color_dynamics.enabled = true;
    b.color_dynamics.fg_bg = pc_paint_dynamic_jitter(1.0f);
    tb_line(0.0, 200.0, 0.0, pts);
    d = tb_dabs(&b, pts, 2);
    for (size_t i = 0; i < d.len; i++) {
        const float *c = d.v[i].color;
        ok =
            ok && ((fabsf(c[1]) < 1e-5f && fabsf(c[0] + c[2] - 1.0f) < 1e-3f) || fmaxf(c[0], c[2]) >= 0.999f);
        blue = blue || c[2] > 0.5f;
        red = red || c[0] > 0.5f;
    }
    CHECK(ok && blue && red);
    pc_paint_dab_vec_fini(&d);
    /* Hue jitter keeps saturation/brightness of a pure colour. */
    b = color_base();
    b.color_dynamics.enabled = true;
    b.color_dynamics.hue_jitter = 1.0f;
    d = tb_dabs(&b, pts, 2);
    ok = true;
    blue = false;
    for (size_t i = 0; i < d.len; i++) {
        const float *c = d.v[i].color;
        float mx = fmaxf(fmaxf(c[0], c[1]), c[2]), mn = fminf(fminf(c[0], c[1]), c[2]);
        ok = ok && fabsf(mx - 1.0f) < 1e-4f && fabsf(mn) < 1e-4f;
        blue = blue || c[1] > 0.5f || c[2] > 0.5f;
    }
    CHECK(ok && blue);
    pc_paint_dab_vec_fini(&d);
    /* Purity -1 desaturates completely. */
    b = color_base();
    b.color_dynamics.enabled = true;
    b.color_dynamics.purity = -1.0f;
    tb_line(0.0, 20.0, 0.0, pts);
    d = tb_dabs(&b, pts, 2);
    ok = true;
    for (size_t i = 0; i < d.len; i++) {
        const float *c = d.v[i].color;
        ok = ok && fabsf(c[0] - c[1]) < 1e-5f && fabsf(c[1] - c[2]) < 1e-5f;
    }
    CHECK(ok);
    pc_paint_dab_vec_fini(&d);
    /* Per-stroke: one colour for all dabs. */
    b = color_base();
    b.color_dynamics.enabled = true;
    b.color_dynamics.per_tip = false;
    b.color_dynamics.hue_jitter = 1.0f;
    tb_line(0.0, 200.0, 0.0, pts);
    d = tb_dabs(&b, pts, 2);
    ok = true;
    for (size_t i = 1; i < d.len; i++) {
        ok = ok && memcmp(d.v[i].color, d.v[i - 1].color, sizeof d.v[i].color) == 0;
    }
    CHECK(ok);
    pc_paint_dab_vec_fini(&d);
    /* Per-tip colours land in the pixels. */
    b = color_base();
    b.size = 10.0f;
    b.spacing = 3.0f;
    b.color_dynamics.enabled = true;
    b.color_dynamics.fg_bg = pc_paint_dynamic_jitter(1.0f);
    tb_line(10.0, 310.0, 20.0, pts);
    tb_paint(&s, &b, pts, 2);
    blue = red = false;
    for (int i = 0; i < 11; i++) {
        float c[4];
        pc_surface_rgba(&s, 10 + i * 30, 20, c);
        blue = blue || c[2] > 0.6f;
        red = red || c[0] > 0.6f;
    }
    CHECK(blue && red);
    pc_surface_fini(&s);
}

static void test_transfer_opacity_caps_and_flow_builds(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_point pts[2] = {pc_paint_point_make(10.0, 10.0, 0.4f), pc_paint_point_make(90.0, 10.0, 0.4f)},
                   one;
    pc_surface s;
    pc_paint_dab_vec d;
    float a1, many;
    bool low = false, le = true;
    /* Pressure on opacity caps coverage. */
    b.transfer.enabled = true;
    b.transfer.opacity = pc_paint_dynamic_controlled(PC_PAINT_CONTROL_PEN_PRESSURE);
    b.size = 10.0f;
    tb_paint(&s, &b, pts, 2);
    CHECK_NEAR(tb_alpha(&s, 50, 10), 0.4, 0.01);
    pc_surface_fini(&s);
    /* Low flow builds up with overlap but never past the opacity. */
    b = tb_brush();
    b.flow = 0.1f;
    b.opacity = 0.8f;
    b.size = 10.0f;
    one = pc_paint_point_make(50.0, 10.0, 1.0f);
    tb_paint(&s, &b, &one, 1);
    a1 = tb_alpha(&s, 50, 10);
    pc_surface_fini(&s);
    tb_line(40.0, 60.0, 10.0, pts);
    tb_paint(&s, &b, pts, 2);
    many = tb_alpha(&s, 50, 10);
    pc_surface_fini(&s);
    CHECK_NEAR(a1, 0.08, 0.01);
    CHECK(many > a1 * 3.0f && many <= 0.8f + 1e-4f);
    /* Flow jitter varies the dab alpha deterministically. */
    b = tb_brush();
    b.transfer.enabled = true;
    b.transfer.flow = pc_paint_dynamic_jitter(1.0f);
    tb_line(0.0, 100.0, 0.0, pts);
    d = tb_dabs(&b, pts, 2);
    for (size_t i = 0; i < d.len; i++) {
        low = low || d.v[i].alpha < 0.5f;
        le = le && d.v[i].alpha <= 1.0f;
    }
    CHECK(low && le);
    pc_paint_dab_vec_fini(&d);
}

static void hold(const pc_paint_brush *b, double ms, size_t *n, float *alpha)
{
    pc_paint_point p[2] = {pc_paint_point_make(50.0, 50.0, 1.0f), pc_paint_point_make(50.0, 50.0, 1.0f)};
    pc_paint_dab_vec d;
    pc_surface s;
    p[0].time = 0.0;
    p[1].time = ms;
    tb_paint(&s, b, p, 2);
    *alpha = tb_alpha(&s, 50, 50);
    pc_surface_fini(&s);
    d = tb_dabs(b, p, 2);
    *n = d.len;
    pc_paint_dab_vec_fini(&d);
}

static void test_build_up_accumulates_over_time(void)
{
    pc_paint_brush b = tb_brush();
    size_t n0, n1, n2;
    float a0, a1, a2;
    b.build_up = true;
    b.build_up_rate = 20.0f;
    b.flow = 0.05f;
    b.size = 20.0f;
    b.hardness = 1.0f;
    hold(&b, 0.0, &n0, &a0);
    hold(&b, 500.0, &n1, &a1);
    hold(&b, 2000.0, &n2, &a2);
    CHECK_EQ_UINT(n0, 1);
    CHECK_EQ_UINT(n1, 11);
    CHECK_EQ_UINT(n2, 41);
    CHECK(a0 < a1 && a1 < a2);
    /* Without build-up, lingering adds nothing. */
    b.build_up = false;
    hold(&b, 2000.0, &n0, &a0);
    CHECK_EQ_UINT(n0, 1);
}

static void test_huge_airbrush_time_gap_emits_bounded_catch_up(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_point p[2] = {pc_paint_point_make(20.0, 20.0, 1.0f), pc_paint_point_make(20.0, 20.0, 1.0f)};
    pc_paint_dab_vec d;
    b.build_up = true;
    b.build_up_rate = 1000.0f;
    p[1].time = 200.0;
    d = tb_dabs(&b, p, 2);
    CHECK_EQ_UINT(d.len, 201); /* a 200 ms pause retains its airbrush build-up */
    pc_paint_dab_vec_fini(&d);
    p[1].time = 1.7976931348623157e308;
    d = tb_dabs(&b, p, 2);
    CHECK(d.len <= 2); /* a huge pause resumes with one dab, not its time backlog */
    CHECK(d.len > 0 && d.v[d.len - 1].center.x == 20.0 && d.v[d.len - 1].center.y == 20.0);
    pc_paint_dab_vec_fini(&d);
    p[1].time = INFINITY;
    d = tb_dabs(&b, p, 2);
    CHECK(d.len <= 1);
    pc_paint_dab_vec_fini(&d);
}

static void test_unbounded_brush_dimensions_are_safe_in_the_public_renderer(void)
{
    pc_paint_brush b = tb_brush();
    pc_paint_context ctx;
    pc_paint_point p = pc_paint_point_make(10.0, 10.0, 1.0f);
    pc_paint_dab_vec d;
    bool ok = true;
    b.size = 3.40282347e38f;
    b.dual_brush.enabled = true;
    b.dual_brush.size = INFINITY;
    CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
    CHECK(ctx.brush.size == PC_PAINT_MAX_BRUSH_SIZE);
    CHECK(ctx.brush.dual_brush.size == 0.5f);
    pc_paint_context_fini(&ctx);
    d = tb_dabs(&b, &p, 1);
    for (size_t i = 0; i < d.len; i++) {
        ok = ok && d.v[i].radius <= PC_PAINT_MAX_BRUSH_SIZE / 2.0f;
    }
    CHECK(ok);
    pc_paint_dab_vec_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_round_tip_roundness_and_angle);
    RUN(test_hardness_profile_is_monotonic);
    RUN(test_hard_edge_is_sharp);
    RUN(test_sampled_tip_is_scaled_and_oriented);
    RUN(test_pencil_is_aliased);
    RUN(test_pencil_dabs_sit_on_the_pixel_grid);
    RUN(test_spacing_sets_dab_count);
    RUN(test_stroke_has_no_seams);
    RUN(test_pressure_controls_size_with_minimum);
    RUN(test_fade_ramps_over_steps);
    RUN(test_tilt_direction_and_rotation_drive_angle);
    RUN(test_jitter_is_deterministic_per_seed);
    RUN(test_scattering_stays_in_bounds);
    RUN(test_zero_scatter_keeps_dabs_on_the_stroke);
    RUN(test_color_dynamics_ranges);
    RUN(test_transfer_opacity_caps_and_flow_builds);
    RUN(test_build_up_accumulates_over_time);
    RUN(test_huge_airbrush_time_gap_emits_bounded_catch_up);
    RUN(test_unbounded_brush_dimensions_are_safe_in_the_public_renderer);
    return pc_t_finish();
}
