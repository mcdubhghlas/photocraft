#include <limits.h>
#include <math.h>
#include <string.h>
#include "harness.h"
#include "ui/outline.h"

static bool has(const pc_ui_outline *o, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    pc_ui_segment s = {{x0, y0}, {x1, y1}};
    for (size_t i = 0; i < o->count; i++) {
        if (memcmp(&s, &o->segments[i], sizeof s) == 0) {
            return true;
        }
    }
    return false;
}
static void test_rectangle_and_hole(void)
{
    pc_surface m;
    pc_surface_init(&m, PC_PIXEL_FORMAT_GRAY8);
    float one = 1, zero = 0;
    CHECK(pc_surface_fill_rect(&m, pc_rect_make(2, 3, 10, 7), &one, 1, NULL) == PC_OK);
    pc_ui_outline o = {0};
    CHECK(pc_ui_outline_trace(&m, pc_surface_content_bounds(&m), 1, &o, NULL) == PC_OK);
    CHECK(o.count == 4);
    CHECK(has(&o, 2, 3, 10, 3));
    CHECK(has(&o, 2, 7, 10, 7));
    CHECK(has(&o, 2, 3, 2, 7));
    CHECK(has(&o, 10, 3, 10, 7));
    CHECK(pc_surface_fill_rect(&m, pc_rect_make(4, 4, 6, 6), &zero, 1, NULL) == PC_OK);
    CHECK(pc_ui_outline_trace(&m, pc_surface_content_bounds(&m), 1, &o, NULL) == PC_OK);
    CHECK(o.count == 8);
    pc_ui_outline_fini(&o);
    pc_surface_fini(&m);
}
static void test_scales_and_clipping(void)
{
    pc_surface m;
    pc_surface_init(&m, PC_PIXEL_FORMAT_GRAY8);
    float one = 1;
    CHECK(pc_surface_fill_rect(&m, pc_rect_make(8, 8, 72, 40), &one, 1, NULL) == PC_OK);
    pc_ui_outline o = {0};
    CHECK(pc_ui_outline_trace(&m, pc_surface_content_bounds(&m), 4, &o, NULL) == PC_OK);
    CHECK(o.count == 4 && has(&o, 8, 8, 72, 8) && has(&o, 72, 8, 72, 40));
    const pc_rect bounds[] = {{3, 5, 30, 21}, {-13, -19, 39, 27}, {200, 200, 240, 240}};
    for (size_t b = 0; b < 3; b++) {
        for (uint32_t step = 1; step <= 16; step *= 2) {
            CHECK(pc_ui_outline_trace(&m, bounds[b], step, &o, NULL) == PC_OK);
            for (size_t i = 0; i < o.count; i++) {
                CHECK(o.segments[i].a[0] >= bounds[b].x0 && o.segments[i].b[0] <= bounds[b].x1);
                CHECK(o.segments[i].a[1] >= bounds[b].y0 && o.segments[i].b[1] <= bounds[b].y1);
            }
        }
    }
    CHECK(o.count == 0);
    pc_ui_outline_fini(&o);
    pc_surface_fini(&m);
}
static void test_threshold_and_limits(void)
{
    pc_surface m;
    pc_surface_init(&m, ((pc_pixel_format){PC_COLOR_MODE_GRAYSCALE,PC_SAMPLE_TYPE_F32,false}));
    float v = .5f;
    CHECK(pc_surface_fill_rect(&m, pc_rect_make(-5, -6, 10, 10), &v, 1, NULL) == PC_OK);
    pc_ui_outline o = {0};
    CHECK(pc_ui_outline_trace(&m, pc_rect_make(-5, -6, 10, 10), 0, &o, NULL) == PC_OK);
    CHECK(o.count == 0);
    v = nextafterf(.5f, 1);
    CHECK(pc_surface_fill_rect(&m, pc_rect_make(-5, -6, 10, 10), &v, 1, NULL) == PC_OK);
    CHECK(pc_ui_outline_trace(&m, pc_rect_make(-5, -6, 10, 10), 1, &o, NULL) == PC_OK);
    CHECK(o.count == 4 && has(&o, -5, -6, 10, -6));
    pc_ui_segment *old = o.segments;
    CHECK(pc_ui_outline_trace(&m, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), 1, &o, NULL) ==
          PC_ERR_LIMIT);
    CHECK(o.segments == old && o.count == 4);
    CHECK(pc_ui_outline_trace(&m, pc_rect_make(INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX), UINT32_MAX, &o,
                              NULL) == PC_OK);
    CHECK(pc_ui_outline_trace(&m, pc_rect_make(0, 0, 0, 0), 1, &o, NULL) == PC_OK && o.count == 0);
    pc_ui_outline_fini(&o);
    pc_surface_fini(&m);
}
int main(int argc, char **argv)
{
    RUN(test_rectangle_and_hole);
    RUN(test_scales_and_clipping);
    RUN(test_threshold_and_limits);
    return pc_t_finish();
}
