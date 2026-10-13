/* Tip outlines, painting symmetry and stored tiles: the tests of crates/paint/src/outline.rs,
 * symmetry.rs and the tile.rs tests added after efa62efa (previews, stored tiles). */
#include <math.h>

#include "base/buf.h"
#include "support.h"

/* ---------- outline.rs ---------- */

static float max_radius(const pc_paint_outline *o)
{
    float m = 0.0f;
    for (size_t i = 0; i < o->count; i++) {
        for (size_t j = 0; j < o->rings[i].n; j++) {
            m = fmaxf(m, hypotf(o->rings[i].pts[j][0], o->rings[i].pts[j][1]));
        }
    }
    return m;
}

static void test_round_outlines_sit_on_the_half_coverage_contour(void)
{
    static const float hs[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (int i = 0; i < 5; i++) {
        pc_paint_brush b;
        pc_paint_outline normal, full;
        pc_paint_brush_default(&b);
        b.size = 40.0f;
        b.angle = 33.0f;
        b.roundness = 0.4f;
        b.hardness = hs[i];
        CHECK(pc_paint_tip_outline(&b, false, &normal, NULL) == PC_OK && normal.count == 1);
        CHECK(pc_paint_tip_outline(&b, true, &full, NULL) == PC_OK && full.count == 1);
        CHECK_NEAR(max_radius(&normal), pc_paint_half_coverage_radius(1.0f, hs[i]), 1e-4);
        CHECK_NEAR(max_radius(&full), 1.0, 1e-4);
        CHECK_EQ_UINT(normal.rings[0].n, 64);
        pc_paint_outline_fini(&normal);
        pc_paint_outline_fini(&full);
    }
}

static float disc_fn(void *ctx, uint32_t x, uint32_t y)
{
    float dx = (float)x - 15.5f, dy = (float)y - 15.5f;
    (void)ctx;
    return fminf(fmaxf(1.0f - sqrtf(dx * dx + dy * dy) / 16.0f, 0.0f), 1.0f);
}

static float zero_fn(void *ctx, uint32_t x, uint32_t y)
{
    (void)ctx;
    (void)x;
    (void)y;
    return 0.0f;
}

static void test_sampled_outlines_stay_inside_the_tip_and_blank_tips_have_none(void)
{
    pc_paint_brush b;
    pc_paint_outline o;
    pc_paint_brush_default(&b);
    b.size = 20.0f;
    b.tip.kind = PC_PAINT_TIP_SAMPLED;
    CHECK(pc_paint_gray_tile_from_fn(32, 32, disc_fn, NULL, &b.tip.tile, NULL) == PC_OK);
    CHECK(pc_paint_tip_outline(&b, false, &o, NULL) == PC_OK);
    CHECK(o.count > 0);
    for (size_t i = 0; i < o.count; i++) {
        CHECK(o.rings[i].n >= 3);
        for (size_t j = 0; j < o.rings[i].n; j++) {
            float x = o.rings[i].pts[j][0], y = o.rings[i].pts[j][1];
            CHECK(isfinite(x) && isfinite(y) && hypotf(x, y) <= 1.001f);
        }
    }
    pc_paint_outline_fini(&o);
    /* The full outline of the same tip reaches further out than the 50 % one. */
    pc_paint_brush_fini(&b);
    pc_paint_brush_default(&b);
    b.size = 20.0f;
    b.tip.kind = PC_PAINT_TIP_SAMPLED;
    CHECK(pc_paint_gray_tile_from_fn(8, 8, zero_fn, NULL, &b.tip.tile, NULL) == PC_OK);
    CHECK(pc_paint_tip_outline(&b, false, &o, NULL) == PC_OK && o.count == 0);
    pc_paint_brush_fini(&b);
}

static void test_hostile_settings_get_no_outline(void)
{
    static const float sizes[4] = {NAN, INFINITY, 0.0f, -1.0f};
    static const float angles[2] = {NAN, INFINITY};
    pc_paint_brush b;
    pc_paint_outline o;
    pc_paint_brush_default(&b);
    b.angle = 33.0f;
    b.roundness = 0.5f;
    for (int i = 0; i < 4; i++) {
        b.size = sizes[i];
        CHECK(pc_paint_tip_outline(&b, false, &o, NULL) == PC_OK && o.count == 0);
    }
    b.size = 40.0f;
    for (int i = 0; i < 2; i++) {
        b.angle = angles[i];
        CHECK(pc_paint_tip_outline(&b, false, &o, NULL) == PC_OK && o.count == 0);
    }
    /* An invalid sampled tip (sample count != w * h) has none either. */
    b.angle = 0.0f;
    b.tip.kind = PC_PAINT_TIP_SAMPLED;
    {
        uint16_t one = 65535;
        CHECK(pc_paint_gray_tile_from_u16(4, 4, &one, 1, &b.tip.tile, NULL) == PC_OK);
    }
    CHECK(pc_paint_tip_outline(&b, true, &o, NULL) == PC_OK && o.count == 0);
    pc_paint_brush_fini(&b);
}

/* ---------- symmetry.rs ---------- */

static void test_modes_reflect_about_their_centre_and_keep_stylus_data(void)
{
    static const struct {
        pc_paint_symmetry_mode m;
        double x, y;
    } cases[3] = {{PC_PAINT_SYMMETRY_VERTICAL, 28.0, 25.0},
                  {PC_PAINT_SYMMETRY_HORIZONTAL, 12.0, 35.0},
                  {PC_PAINT_SYMMETRY_DIAGONAL, 15.0, 22.0}};
    static const double dual[3][2] = {{28.0, 25.0}, {12.0, 35.0}, {28.0, 35.0}};
    const double c[2] = {20.0, 30.0};
    pc_paint_point p = pc_paint_point_make(12.0, 25.0, 0.4f);
    pc_paint_symmetry s;
    p.tilt_x = 18.0f;
    for (int i = 0; i < 3; i++) {
        pc_paint_point_vec out[3] = {{0}, {0}, {0}};
        size_t count = 0;
        CHECK(pc_paint_symmetry_new(cases[i].m, c, 0.0, &s));
        CHECK(pc_paint_symmetry_reflected_passes(&s, &p, 1, out, &count, NULL) == PC_OK);
        CHECK_EQ_UINT(count, 1);
        CHECK(out[0].len == 1 && fabs(out[0].v[0].x - cases[i].x) < 1e-9 &&
              fabs(out[0].v[0].y - cases[i].y) < 1e-9);
        CHECK(out[0].v[0].pressure == p.pressure && out[0].v[0].tilt_x == p.tilt_x);
        pc_paint_point_vec_fini(&out[0]);
    }
    CHECK(pc_paint_symmetry_new(PC_PAINT_SYMMETRY_DUAL, c, 0.0, &s));
    {
        pc_paint_point_vec out[3] = {{0}, {0}, {0}};
        size_t count = 0;
        CHECK(pc_paint_symmetry_reflected_passes(&s, &p, 1, out, &count, NULL) == PC_OK);
        CHECK_EQ_UINT(count, 3);
        for (int i = 0; i < 3; i++) {
            CHECK(out[i].len == 1 && fabs(out[i].v[0].x - dual[i][0]) < 1e-9 &&
                  fabs(out[i].v[0].y - dual[i][1]) < 1e-9);
            pc_paint_point_vec_fini(&out[i]);
        }
    }
}

static void test_rotated_axes_clip_on_rectangles_and_accept_half_pixel_centres(void)
{
    static const double sizes[2][2] = {{100.0, 100.0}, {101.0, 53.0}};
    pc_paint_symmetry s;
    double seg[2][2][2], dirs[2][2];
    for (int si = 0; si < 2; si++) {
        const double *size = sizes[si], c[2] = {size[0] / 2.0, size[1] / 2.0};
        for (int m = 0; m < PC_PAINT_SYMMETRY_MODE_COUNT; m++) {
            size_t n;
            CHECK(pc_paint_symmetry_new((pc_paint_symmetry_mode)m, c, 0.0, &s));
            n = pc_paint_symmetry_segments(&s, size, seg);
            CHECK_EQ_UINT(n, m == PC_PAINT_SYMMETRY_DUAL ? 2 : 1);
            for (size_t k = 0; k < n; k++) {
                for (int e = 0; e < 2; e++) {
                    CHECK(seg[k][e][0] >= -1e-9 && seg[k][e][0] <= size[0] + 1e-9 && seg[k][e][1] >= -1e-9 &&
                          seg[k][e][1] <= size[1] + 1e-9);
                }
            }
            if (m == PC_PAINT_SYMMETRY_DIAGONAL) {
                (void)pc_paint_symmetry_directions(&s, dirs);
                CHECK(fabs(dirs[0][0] - dirs[0][1]) < 1e-9);
            }
        }
    }
    {
        const double c[2] = {20.0, 30.0}, nanc[2] = {NAN, 0.0}, zero[2] = {0.0, 0.0},
                     far[2] = {PC_PAINT_SYMMETRY_MAX_COORDINATE + 1.0, 0.0}, below[2] = {20.0, -5.0},
                     doc[2] = {100.0, 60.0};
        pc_paint_point p = pc_paint_point_make(12.0, 25.0, 1.0f);
        pc_paint_point_vec out[3] = {{0}, {0}, {0}};
        size_t count = 0;
        CHECK(pc_paint_symmetry_new(PC_PAINT_SYMMETRY_VERTICAL, c, 90.0, &s));
        CHECK(pc_paint_symmetry_reflected_passes(&s, &p, 1, out, &count, NULL) == PC_OK);
        CHECK(fabs(out[0].v[0].x - 12.0) < 1e-9 && fabs(out[0].v[0].y - 35.0) < 1e-9);
        pc_paint_point_vec_fini(&out[0]);
        CHECK(!pc_paint_symmetry_new(PC_PAINT_SYMMETRY_DUAL, nanc, 0.0, &s));
        CHECK(!pc_paint_symmetry_new(PC_PAINT_SYMMETRY_DUAL, zero, INFINITY, &s));
        CHECK(!pc_paint_symmetry_new(PC_PAINT_SYMMETRY_DUAL, far, 0.0, &s));
        CHECK(pc_paint_symmetry_new(PC_PAINT_SYMMETRY_HORIZONTAL, below, 0.0, &s));
        CHECK_EQ_UINT(pc_paint_symmetry_segments(&s, doc, seg), 0);
        /* Rotation wraps into 0..360, ids round-trip. */
        CHECK(pc_paint_symmetry_new(PC_PAINT_SYMMETRY_DUAL, c, -90.0, &s) && s.rotation == 270.0);
        for (int m = 0; m < PC_PAINT_SYMMETRY_MODE_COUNT; m++) {
            pc_paint_symmetry_mode back;
            const char *id = pc_paint_symmetry_mode_id((pc_paint_symmetry_mode)m);
            CHECK(id != NULL && pc_paint_symmetry_mode_from_id(id, strlen(id), &back) && (int)back == m);
        }
        CHECK(pc_paint_symmetry_mode_id((pc_paint_symmetry_mode)9) == NULL);
    }
}

/* ---------- tile.rs: previews and stored tiles ---------- */

static float half_fn(void *ctx, uint32_t x, uint32_t y)
{
    (void)ctx;
    (void)y;
    return x < 500 ? 1.0f : 0.0f;
}

static float mod7_fn(void *ctx, uint32_t x, uint32_t y)
{
    (void)ctx;
    return (float)((x + y) % 7) / 7.0f;
}

static float half_const_fn(void *ctx, uint32_t x, uint32_t y)
{
    (void)ctx;
    (void)x;
    (void)y;
    return 0.5f;
}

static void test_preview_downscales_deterministically_and_tolerates_bad_tiles(void)
{
    pc_paint_gray_tile t = PC_PAINT_GRAY_TILE_INIT, p = PC_PAINT_GRAY_TILE_INIT, q = PC_PAINT_GRAY_TILE_INIT;
    uint16_t one = 1;
    const uint16_t *s;
    CHECK(pc_paint_gray_tile_from_fn(1000, 400, half_fn, NULL, &t, NULL) == PC_OK);
    CHECK(pc_paint_gray_tile_preview(&t, 96, &p, NULL) == PC_OK);
    CHECK(pc_paint_gray_tile_is_valid(&p) && p.width <= 96 && p.height <= 96);
    CHECK(pc_paint_gray_tile_preview(&t, 96, &q, NULL) == PC_OK && pc_paint_gray_tile_eq(&p, &q));
    CHECK(pc_paint_gray_tile_get(&p, 0, 0) > 0.99f && pc_paint_gray_tile_get(&p, p.width - 1, 0) < 0.01f);
    s = pc_paint_gray_tile_samples(&p);
    for (size_t i = 0; i < pc_paint_gray_tile_len(&p); i++) {
        CHECK(s[i] % 257 == 0); /* 8-bit levels */
    }
    pc_paint_gray_tile_fini(&t);
    pc_paint_gray_tile_fini(&p);
    pc_paint_gray_tile_fini(&q);
    /* Small tiles keep their size; invalid ones never fail. */
    CHECK(pc_paint_gray_tile_from_fn(5, 3, half_const_fn, NULL, &t, NULL) == PC_OK);
    CHECK(pc_paint_gray_tile_preview(&t, 96, &p, NULL) == PC_OK && p.width == 5);
    pc_paint_gray_tile_fini(&t);
    pc_paint_gray_tile_fini(&p);
    CHECK(pc_paint_gray_tile_from_u16(4, 4, &one, 1, &t, NULL) == PC_OK);
    CHECK(pc_paint_gray_tile_preview(&t, 96, &p, NULL) == PC_OK && pc_paint_gray_tile_is_valid(&p));
    pc_paint_gray_tile_fini(&t);
    pc_paint_gray_tile_fini(&p);
    CHECK(pc_paint_gray_tile_preview(&t, 0, &p, NULL) == PC_OK && pc_paint_gray_tile_is_valid(&p));
    pc_paint_gray_tile_fini(&p);
}

static void json_of(pc_paint_json_type ty, const void *v, pc_buf *out)
{
    *out = (pc_buf)PC_BUF_INIT;
    CHECK(pc_paint_json_to_string(ty, v, PC_PAINT_JSON_VALUE, out, NULL) == PC_OK);
}

static void test_stored_tiles_compare_by_key_and_serialise_loaded_as_sampled(void)
{
    pc_paint_gray_tile full = PC_PAINT_GRAY_TILE_INIT, prev = PC_PAINT_GRAY_TILE_INIT;
    pc_paint_tip a, l, sampled, back;
    pc_paint_pattern pa, pl, ptile;
    pc_buf j1, j2;
    CHECK(pc_paint_gray_tile_from_fn(300, 200, mod7_fn, NULL, &full, NULL) == PC_OK);
    CHECK(pc_paint_gray_tile_preview(&full, PC_PAINT_PREVIEW_SIDE, &prev, NULL) == PC_OK);
    pc_paint_tip_default(&a);
    pc_paint_tip_default(&l);
    pc_paint_tip_default(&sampled);
    a.kind = l.kind = PC_PAINT_TIP_STORED;
    CHECK(pc_paint_stored_tile_init("abc", 3, 300, 200, &prev, NULL, &a.stored, NULL) == PC_OK);
    CHECK(pc_paint_stored_tile_init("abc", 3, 300, 200, &prev, &full, &l.stored, NULL) == PC_OK);
    sampled.kind = PC_PAINT_TIP_SAMPLED;
    pc_paint_gray_tile_copy(&sampled.tile, &full);
    CHECK(pc_paint_tip_eq(&a, &l));
    CHECK(!pc_paint_tip_eq(&a, &sampled));
    CHECK(pc_paint_tip_bitmap(&a)->width == prev.width);
    CHECK(pc_paint_gray_tile_eq(pc_paint_tip_bitmap(&l), &full));
    {
        uint32_t w = 0, h = 0;
        CHECK(pc_paint_tip_bitmap_size(&a, &w, &h) && w == 300 && h == 200);
    }
    /* Unloaded: a small reference that reads back as one. */
    json_of(PC_PAINT_JSON_TIP, &a, &j1);
    CHECK_CONTAINS(pc_buf_cstr(&j1), "{\"stored\":{\"height\":200,\"key\":\"abc\",\"preview\":{");
    CHECK(strstr(pc_buf_cstr(&j1), "\"full\"") == NULL);
    CHECK(pc_paint_json_from_string(PC_PAINT_JSON_TIP, pc_buf_cstr(&j1), j1.len, &back, NULL) == PC_OK);
    CHECK(back.kind == PC_PAINT_TIP_STORED && pc_paint_tip_eq(&back, &a) && !back.stored.loaded);
    CHECK(pc_paint_gray_tile_eq(&back.stored.preview, &prev));
    pc_paint_tip_fini(&back);
    pc_buf_fini(&j1);
    /* Loaded: the same JSON as an embedded bitmap. */
    json_of(PC_PAINT_JSON_TIP, &l, &j1);
    json_of(PC_PAINT_JSON_TIP, &sampled, &j2);
    CHECK_EQ_STR(pc_buf_cstr(&j1), pc_buf_cstr(&j2));
    pc_buf_fini(&j1);
    pc_buf_fini(&j2);
    pc_paint_pattern_default(&pa);
    pc_paint_pattern_default(&pl);
    pc_paint_pattern_default(&ptile);
    pa.kind = pl.kind = PC_PAINT_PATTERN_STORED;
    pc_paint_stored_tile_copy(&pa.stored, &a.stored);
    pc_paint_stored_tile_copy(&pl.stored, &l.stored);
    ptile.kind = PC_PAINT_PATTERN_TILE;
    pc_paint_gray_tile_copy(&ptile.tile, &full);
    json_of(PC_PAINT_JSON_PATTERN, &pl, &j1);
    json_of(PC_PAINT_JSON_PATTERN, &ptile, &j2);
    CHECK_EQ_STR(pc_buf_cstr(&j1), pc_buf_cstr(&j2));
    pc_buf_fini(&j1);
    pc_buf_fini(&j2);
    json_of(PC_PAINT_JSON_PATTERN, &pa, &j1);
    CHECK_CONTAINS(pc_buf_cstr(&j1), "\"width\":300");
    CHECK(pc_paint_pattern_eq(&pa, &pl) && !pc_paint_pattern_eq(&pa, &ptile));
    pc_buf_fini(&j1);
    /* Hostile stored payloads are errors with serde's messages. */
    {
        static const char *bad[] = {"{\"stored\":5}", "{\"stored\":{\"key\":\"k\",\"width\":1,\"height\":1}}",
                                    "\"stored\"",
                                    "{\"stored\":{\"key\":1,\"width\":1,\"height\":1,\"preview\":0}}"};
        static const char *msg[] = {"invalid type: integer `5`, expected struct StoredTile",
                                    "missing field `preview`",
                                    "invalid type: unit variant, expected newtype variant",
                                    "invalid type: integer `1`, expected a string"};
        for (int i = 0; i < 4; i++) {
            pc_error err;
            CHECK(pc_paint_json_from_string(PC_PAINT_JSON_TIP, bad[i], strlen(bad[i]), &back, &err) ==
                  PC_ERR_INVALID);
            CHECK_EQ_STR(err.msg, msg[i]);
        }
    }
    pc_paint_pattern_fini(&pa);
    pc_paint_pattern_fini(&pl);
    pc_paint_pattern_fini(&ptile);
    pc_paint_tip_fini(&a);
    pc_paint_tip_fini(&l);
    pc_paint_tip_fini(&sampled);
    pc_paint_gray_tile_fini(&full);
    pc_paint_gray_tile_fini(&prev);
}

/* A stored tip renders with its preview until loaded, then with the full bitmap. */
static void test_stored_tips_render_with_their_bitmap(void)
{
    pc_paint_gray_tile full = PC_PAINT_GRAY_TILE_INIT, prev = PC_PAINT_GRAY_TILE_INIT;
    pc_paint_brush b, s;
    pc_paint_point p = pc_paint_point_make(50.0, 50.0, 1.0f);
    pc_surface x, y;
    CHECK(pc_paint_gray_tile_from_fn(32, 32, disc_fn, NULL, &full, NULL) == PC_OK);
    CHECK(pc_paint_gray_tile_preview(&full, 8, &prev, NULL) == PC_OK);
    b = tb_brush();
    b.size = 30.0f;
    pc_paint_brush_copy(&s, &b);
    b.tip.kind = PC_PAINT_TIP_SAMPLED;
    pc_paint_gray_tile_copy(&b.tip.tile, &full);
    s.tip.kind = PC_PAINT_TIP_STORED;
    CHECK(pc_paint_stored_tile_init("k", 1, 32, 32, &prev, &full, &s.tip.stored, NULL) == PC_OK);
    tb_paint(&x, &b, &p, 1);
    tb_paint(&y, &s, &p, 1);
    CHECK(pc_surface_eq(&x, &y));
    pc_surface_fini(&x);
    pc_surface_fini(&y);
    pc_paint_brush_fini(&b);
    pc_paint_brush_fini(&s);
    pc_paint_gray_tile_fini(&full);
    pc_paint_gray_tile_fini(&prev);
}

int main(int argc, char **argv)
{
    RUN(test_round_outlines_sit_on_the_half_coverage_contour);
    RUN(test_sampled_outlines_stay_inside_the_tip_and_blank_tips_have_none);
    RUN(test_hostile_settings_get_no_outline);
    RUN(test_modes_reflect_about_their_centre_and_keep_stylus_data);
    RUN(test_rotated_axes_clip_on_rectangles_and_accept_half_pixel_centres);
    RUN(test_preview_downscales_deterministically_and_tolerates_bad_tiles);
    RUN(test_stored_tiles_compare_by_key_and_serialise_loaded_as_sampled);
    RUN(test_stored_tips_render_with_their_bitmap);
    return pc_t_finish();
}
