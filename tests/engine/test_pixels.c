/* Engine pixel helpers (crates/engine/src/pixels.rs try_fill_surface / try_clear_surface) and the
 * fallible allocation layer (allocation.rs). The command-level tests that drive these through
 * edit.fill / edit.clear belong to E4's fill/edit chunks (they need channel and pattern support);
 * here they are exercised directly, plus the never-crash allocation failpoint. */
#include <stdlib.h>
#include <string.h>

#include "color/color.h"
#include "engine/allocation.h"
#include "engine/engine_internal.h"
#include "harness.h"

static bool close4(const float a[4], const float b[4])
{
    for (int i = 0; i < 4; i++) {
        if (fabsf(a[i] - b[i]) >= 1.5f / 255.0f) {
            return false;
        }
    }
    return true;
}

static void test_allocation_reports_oom_without_crashing(void)
{
    /* The failpoint arms the next reservation to report OOM (what a command test uses to prove its
     * transaction boundary), with the reference's exact wording. */
    pc_error err = {0};
    void *buf = (void *)1;
    pc_engine_alloc_fail_next();
    CHECK(pc_engine_alloc_filled(16, 1, "test image", &buf, &err) != PC_OK);
    CHECK(buf == NULL);
    CHECK_CONTAINS(err.msg, "not enough memory for test image");
    /* It is consumed after one use. */
    memset(&err, 0, sizeof err);
    CHECK(pc_engine_alloc_filled(16, 1, "test image", &buf, &err) == PC_OK);
    CHECK(buf != NULL);
    /* filled zeroes its buffer. */
    for (int i = 0; i < 16; i++) {
        CHECK(((uint8_t *)buf)[i] == 0);
    }
    free(buf);
    /* A size that overflows is refused, not wrapped. */
    memset(&err, 0, sizeof err);
    CHECK(pc_engine_alloc_capacity(SIZE_MAX, 2, "big image", &buf, &err) != PC_OK);
    CHECK_CONTAINS(err.msg, "requested size overflowed");
    /* A zero-length reservation succeeds with no allocation. */
    memset(&err, 0, sizeof err);
    buf = (void *)1;
    CHECK(pc_engine_alloc_capacity(0, 8, "empty", &buf, &err) == PC_OK);
    CHECK(buf == NULL);
}

static void test_fill_opaque_colour(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    pc_error err = {0};
    CHECK(pc_engine_try_fill_surface(&s, pc_rect_make(0, 0, 10, 10), red, NULL, false, &err) == PC_OK);
    float px[4];
    pc_surface_rgba(&s, 5, 5, px);
    CHECK(close4(px, red));
    /* Outside the filled area stays transparent. */
    pc_surface_rgba(&s, 15, 15, px);
    CHECK(px[3] == 0.0f);
    pc_surface_fini(&s);
}

static void test_fill_through_a_selection(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    /* A grey selection: full coverage on the left half, none on the right. */
    pc_surface sel;
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    float half = 0.5f;
    pc_error err = {0};
    CHECK(pc_surface_fill_rect(&sel, pc_rect_make(0, 0, 20, 30), &half, 1, &err) == PC_OK);
    float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(pc_engine_try_fill_surface(&s, pc_rect_make(0, 0, 40, 30), white, &sel, false, &err) == PC_OK);
    /* 50% coverage of an opaque white onto transparency = white at alpha 0.5. */
    float px[4];
    pc_surface_rgba(&s, 5, 5, px);
    float want[4] = {1.0f, 1.0f, 1.0f, 0.5f};
    CHECK(close4(px, want));
    /* The uncovered right half is untouched. */
    pc_surface_rgba(&s, 30, 5, px);
    CHECK(px[3] == 0.0f);
    pc_surface_fini(&sel);
    pc_surface_fini(&s);
}

static void test_fill_preserves_locked_transparency(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    pc_error err = {0};
    CHECK(pc_engine_try_fill_surface(&s, pc_rect_make(0, 0, 20, 30), white, NULL, false, &err) == PC_OK);
    /* Lock transparency: the opaque half becomes black, the empty half stays empty. */
    CHECK(pc_engine_try_fill_surface(&s, pc_rect_make(0, 0, 40, 30), black, NULL, true, &err) == PC_OK);
    float px[4];
    pc_surface_rgba(&s, 5, 5, px);
    CHECK(close4(px, black));
    pc_surface_rgba(&s, 30, 5, px);
    CHECK(px[3] == 0.0f);
    pc_surface_fini(&s);
}

static void test_clear_makes_pixels_transparent(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_error err = {0};
    CHECK(pc_engine_try_fill_surface(&s, pc_rect_make(0, 0, 10, 10), white, NULL, false, &err) == PC_OK);
    CHECK(pc_engine_try_clear_surface(&s, pc_rect_make(0, 0, 5, 10), NULL, &err) == PC_OK);
    float px[4];
    pc_surface_rgba(&s, 2, 2, px);
    CHECK(px[3] == 0.0f); /* cleared */
    pc_surface_rgba(&s, 7, 2, px);
    CHECK(px[3] == 1.0f); /* kept */
    /* Clearing a surface with no alpha channel is a no-op, never an error. */
    pc_surface g;
    pc_surface_init(&g, PC_PIXEL_FORMAT_GRAY8);
    CHECK(pc_engine_try_clear_surface(&g, pc_rect_make(0, 0, 4, 4), NULL, &err) == PC_OK);
    pc_surface_fini(&g);
    pc_surface_fini(&s);
}

/* A fill or clear inside a selection keeps the tiles the selection never reaches shared with the
 * (cloned) history, instead of copying each one (#4517). */
static void test_unselected_tiles_stay_shared(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    pc_error err = {0};
    CHECK(pc_engine_try_fill_surface(&s, pc_rect_make(0, 0, 1024, 1024), white, NULL, false, &err) == PC_OK);
    pc_surface before;
    CHECK(pc_surface_clone(&s, &before, &err) == PC_OK);
    /* Two small selections in opposite corners: their bounds span every tile, but only the two
     * corner tiles have anything selected. */
    pc_surface sel;
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    float one = 1.0f;
    CHECK(pc_surface_fill_rect(&sel, pc_rect_make(20, 20, 120, 120), &one, 1, &err) == PC_OK);
    CHECK(pc_surface_fill_rect(&sel, pc_rect_make(900, 900, 1000, 1000), &one, 1, &err) == PC_OK);
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK(pc_engine_try_fill_surface(&s, pc_rect_make(0, 0, 1024, 1024), red, &sel, false, &err) == PC_OK);
    /* A middle tile (coord 2,2) was never selected: same shared tile pointer as before. */
    pc_tile mid = {2, 2};
    CHECK(pc_surface_tile(&s, mid) == pc_surface_tile(&before, mid));
    /* A corner tile (0,0) was edited: a different tile. */
    pc_tile corner = {0, 0};
    CHECK(pc_surface_tile(&s, corner) != pc_surface_tile(&before, corner));
    /* The middle still reads white, the corner red. */
    float px[4];
    pc_surface_rgba(&s, 600, 600, px);
    CHECK(close4(px, white));
    pc_surface_rgba(&s, 25, 25, px);
    CHECK(close4(px, red));
    pc_surface_fini(&sel);
    pc_surface_fini(&before);
    pc_surface_fini(&s);
}

/* Hostile inputs must not crash. An empty area is a clean no-op; the allocation failpoint stands in
 * for an area too large to process (its tile list cannot be reserved), proving edit_tiles reports
 * the error and leaves the surface unchanged rather than crashing. */
static void test_fill_bad_areas_do_not_crash(void)
{
    pc_surface s;
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    pc_error err = {0};
    CHECK(pc_engine_try_fill_surface(&s, PC_RECT_EMPTY, red, NULL, false, &err) == PC_OK);
    /* Reserving the tile list fails: a graceful error, no partial change, no crash. */
    memset(&err, 0, sizeof err);
    pc_engine_alloc_fail_next();
    CHECK(pc_engine_try_fill_surface(&s, pc_rect_make(0, 0, 64, 64), red, NULL, false, &err) != PC_OK);
    CHECK_CONTAINS(err.msg, "filling pixels");
    float px[4];
    pc_surface_rgba(&s, 5, 5, px);
    CHECK(px[3] == 0.0f); /* unchanged */
    pc_surface_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_allocation_reports_oom_without_crashing);
    RUN(test_fill_opaque_colour);
    RUN(test_fill_through_a_selection);
    RUN(test_fill_preserves_locked_transparency);
    RUN(test_clear_makes_pixels_transparent);
    RUN(test_unselected_tiles_stay_shared);
    RUN(test_fill_bad_areas_do_not_crash);
    return pc_t_finish();
}
