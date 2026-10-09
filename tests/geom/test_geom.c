/* Ports crates/geom/src/lib.rs tests, plus edge cases. */
#include <math.h>

#include "geom/geom.h"
#include "harness.h"

#define CHECK_RECT(r, a, b, c, d) CHECK(pc_rect_eq((r), pc_rect_make((a), (b), (c), (d))))

static void test_rect_basics(void)
{
    pc_rect r = pc_rect_from_xywh(10, 20, 30, 40);
    CHECK_EQ_UINT(pc_rect_width(r), 30);
    CHECK_EQ_UINT(pc_rect_height(r), 40);
    CHECK(pc_rect_contains(r, 10, 20));
    CHECK(!pc_rect_contains(r, 40, 20));
    CHECK(pc_rect_is_empty(pc_rect_make(5, 5, 5, 10)));
    CHECK_EQ_UINT(pc_size_area(pc_size_make(70000, 70000)), 4900000000u);
    CHECK(pc_size_is_empty(pc_size_make(0, 5)));
    CHECK_RECT(pc_rect_from_size(pc_size_make(3, 4)), 0, 0, 3, 4);
}

static void test_intersect_and_union(void)
{
    pc_rect a = pc_rect_make(0, 0, 10, 10);
    pc_rect b = pc_rect_make(5, 5, 15, 15);
    CHECK_RECT(pc_rect_intersect(a, b), 5, 5, 10, 10);
    CHECK_RECT(pc_rect_union(a, b), 0, 0, 15, 15);
    CHECK_RECT(pc_rect_intersect(a, pc_rect_make(20, 20, 30, 30)), 0, 0, 0, 0);
    CHECK(pc_rect_eq(pc_rect_union(PC_RECT_EMPTY, a), a));
    CHECK(pc_rect_eq(pc_rect_union(a, pc_rect_make(9, 9, 3, 3)), a));
    CHECK(pc_rect_contains_rect(a, pc_rect_make(2, 2, 3, 3)));
    CHECK(pc_rect_contains_rect(a, pc_rect_make(50, 50, 40, 40))); /* empty is inside anything */
    CHECK(!pc_rect_contains_rect(a, b));
}

static void test_tile_coords_handle_negatives(void)
{
    pc_tile t = pc_tile_containing(0, 0);
    CHECK(t.tx == 0 && t.ty == 0);
    t = pc_tile_containing(255, 255);
    CHECK(t.tx == 0 && t.ty == 0);
    t = pc_tile_containing(256, 0);
    CHECK(t.tx == 1 && t.ty == 0);
    t = pc_tile_containing(-1, -1);
    CHECK(t.tx == -1 && t.ty == -1);
    t = pc_tile_containing(-256, -257);
    CHECK(t.tx == -1 && t.ty == -2);
    t = pc_tile_containing(INT32_MIN, INT32_MAX);
    CHECK_EQ_INT(t.tx, INT32_MIN / 256);
    CHECK_EQ_INT(t.ty, INT32_MAX / 256);
    CHECK_RECT(pc_tile_rect((pc_tile){-1, 2}), -256, 512, 0, 768);
    CHECK(pc_tile_cmp((pc_tile){0, 5}, (pc_tile){1, 0}) < 0);
    CHECK(pc_tile_cmp((pc_tile){1, 1}, (pc_tile){1, 0}) > 0);
    CHECK_EQ_INT(pc_tile_cmp((pc_tile){3, 3}, (pc_tile){3, 3}), 0);
}

static int count_tiles(pc_rect r, pc_tile *seen, int max)
{
    pc_tile a;
    pc_tile b;
    int n = 0;
    int32_t tx;
    int32_t ty;
    if (!pc_rect_tile_range(r, &a, &b)) {
        return 0;
    }
    for (ty = a.ty; ty <= b.ty; ty++) {
        for (tx = a.tx; tx <= b.tx; tx++) {
            if (n < max) {
                seen[n].tx = tx;
                seen[n].ty = ty;
            }
            n++;
        }
    }
    return n;
}

static void test_rect_tiles_enumeration(void)
{
    pc_tile t[8];
    CHECK_EQ_INT(count_tiles(pc_rect_make(-10, 0, 300, 10), t, 8), 3);
    CHECK(t[0].tx == -1 && t[0].ty == 0 && t[1].tx == 0 && t[2].tx == 1);
    CHECK_EQ_INT(count_tiles(PC_RECT_EMPTY, t, 8), 0);
    CHECK_EQ_INT(count_tiles(pc_rect_make(0, 0, 256, 256), t, 8), 1);
    CHECK_EQ_INT(count_tiles(pc_rect_make(0, 0, 257, 257), t, 8), 4);
    /* Extreme edges don't overflow. */
    CHECK_EQ_INT(count_tiles(pc_rect_make(INT32_MAX - 1, INT32_MIN, INT32_MAX, INT32_MIN + 1), t, 8), 1);
}

static void test_affine_inverse_roundtrip(void)
{
    pc_affine t = pc_affine_then(pc_affine_then(pc_affine_translate(5.0, -3.0), pc_affine_rotate(0.7)),
                                 pc_affine_scale(2.5));
    pc_affine inv;
    pc_affine untouched = PC_AFFINE_IDENTITY;
    pc_point p = pc_point_make(12.0, -7.5);
    pc_point q;
    CHECK(pc_affine_inverse(t, &inv));
    q = pc_affine_apply(inv, pc_affine_apply(t, p));
    CHECK_NEAR(q.x, p.x, 1e-9);
    CHECK_NEAR(q.y, p.y, 1e-9);
    CHECK(!pc_affine_inverse(pc_affine_scale(0.0), &untouched));
    CHECK_NEAR(untouched.m[0], 1.0, 0.0); /* out-param untouched */
}

static void test_affine_then_order(void)
{
    pc_affine t = pc_affine_then(pc_affine_translate(10.0, 0.0), pc_affine_scale(2.0));
    pc_point p = pc_affine_apply(t, pc_point_make(1.0, 0.0)); /* translate first, then scale */
    CHECK_NEAR(p.x, 22.0, 0.0);
    CHECK_NEAR(p.y, 0.0, 0.0);
    CHECK_NEAR(pc_affine_determinant(pc_affine_scale(3.0)), 9.0, 0.0);
}

static void test_translate_and_inflate_saturate_on_overflow(void)
{
    pc_rect r = pc_rect_make(INT32_MAX - 10, INT32_MIN + 10, INT32_MAX, INT32_MIN + 20);
    pc_rect t = pc_rect_translate(r, 100, -100);
    pc_rect grown;
    CHECK_EQ_INT(t.x0, INT32_MAX);
    CHECK_EQ_INT(t.x1, INT32_MAX);
    CHECK_EQ_INT(t.y0, INT32_MIN);
    CHECK_EQ_INT(t.y1, INT32_MIN);
    grown = pc_rect_inflate(pc_rect_make(INT32_MAX - 5, INT32_MIN + 5, INT32_MAX - 1, INT32_MIN + 10), 10);
    CHECK_EQ_INT(grown.x1, INT32_MAX);
    CHECK_EQ_INT(grown.y0, INT32_MIN);
}

static void test_width_and_height_do_not_overflow_on_extreme_edges(void)
{
    pc_rect r = pc_rect_make(INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN);
    pc_size s;
    CHECK_EQ_UINT(pc_rect_width(r), 0);
    CHECK_EQ_UINT(pc_rect_height(r), 0);
    CHECK(pc_rect_is_empty(r));
    r = pc_rect_make(INT32_MIN, INT32_MIN + 20, INT32_MAX, INT32_MAX - 5);
    CHECK_EQ_UINT(pc_rect_width(r), UINT32_MAX);
    CHECK_EQ_UINT(pc_rect_height(r), UINT32_MAX - 25);
    s = pc_rect_size(r);
    CHECK(s.width == UINT32_MAX && s.height == UINT32_MAX - 25);
}

static void test_from_xywh_wraps_like_rust(void)
{
    /* Rust: x.saturating_add(w as i32) with `as` wrapping 0xFFFFFFFF to -1. */
    CHECK_RECT(pc_rect_from_xywh(10, 0, 0xFFFFFFFFu, 5), 10, 0, 9, 5);
    CHECK_RECT(pc_rect_from_xywh(INT32_MAX - 1, 0, 100, 1), INT32_MAX - 1, 0, INT32_MAX, 1);
    CHECK_RECT(pc_rect_from_xywh(INT32_MIN, 0, 0x80000000u, 1), INT32_MIN, 0, INT32_MIN, 1);
}

int main(int argc, char **argv)
{
    RUN(test_rect_basics);
    RUN(test_intersect_and_union);
    RUN(test_tile_coords_handle_negatives);
    RUN(test_rect_tiles_enumeration);
    RUN(test_affine_inverse_roundtrip);
    RUN(test_affine_then_order);
    RUN(test_translate_and_inflate_saturate_on_overflow);
    RUN(test_width_and_height_do_not_overflow_on_extreme_edges);
    RUN(test_from_xywh_wraps_like_rust);
    return pc_t_finish();
}
