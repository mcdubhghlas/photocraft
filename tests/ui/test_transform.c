#include "harness.h"
#include "ui/state.h"
#include "algo/transform.h"
#include <math.h>
#include <string.h>
static pc_ui_transform_frame frame(void)
{
    pc_ui_transform_frame f = {0};
    const double rect[4] = {0, 0, 100, 100};
    pc_ui_transform_frame_rect(&f, rect);
    return f;
}
static pc_ui_transform_drag drag(pc_ui_transform_frame f, pc_ui_transform_part part, size_t index)
{
    pc_ui_transform_drag d = {.hit = {part, index}, .before = f};
    const double *start = part == PC_TRANSFORM_CORNER ? f.quad[index] : f.pivot;
    memcpy(d.start, start, sizeof d.start);
    if (part == PC_TRANSFORM_EDGE)
        for (size_t a = 0; a < 2; a++)
            d.start[a] = (f.quad[index][a] + f.quad[(index + 1) % 4][a]) / 2;
    return d;
}
static bool apply(pc_ui_transform_frame *f, const pc_ui_transform_drag *d, double x, double y,
                  pc_ui_transform_mode mode, bool type, bool shift, bool alt, bool command)
{
    const double point[2] = {x, y};
    return pc_ui_transform_apply(f, d, point, mode, type, shift, alt, command, false, false);
}
static void test_handles_scaling_modifiers_and_pivot(void)
{
    pc_ui_transform_frame f = frame();
    const double points[5][2] = {{0, 0}, {50, 0}, {50, 50}, {75, 75}, {130, 130}};
    const pc_ui_transform_part parts[] = {PC_TRANSFORM_CORNER, PC_TRANSFORM_EDGE, PC_TRANSFORM_PIVOT,
                                          PC_TRANSFORM_INSIDE, PC_TRANSFORM_OUTSIDE};
    for (size_t i = 0; i < 5; i++)
        CHECK(pc_ui_transform_hit_test(&f, points[i], 12).part == parts[i]);
    pc_ui_transform_drag d = drag(f, PC_TRANSFORM_CORNER, 2);
    CHECK(apply(&f, &d, 200, 110, PC_TRANSFORM_FREE, false, false, false, false));
    CHECK_NEAR(f.quad[2][0], 200, 1e-8);
    CHECK_NEAR(f.quad[2][1], 200, 1e-8);
    CHECK_NEAR(f.pivot[0], 100, 1e-8);
    f = frame();
    CHECK(apply(&f, &d, 200, 110, PC_TRANSFORM_FREE, false, true, false, false));
    CHECK_NEAR(f.quad[2][1], 110, 1e-8);
    f = frame();
    CHECK(apply(&f, &d, 150, 150, PC_TRANSFORM_FREE, false, false, true, false));
    CHECK_NEAR(f.quad[0][0], -50, 1e-8);
    CHECK_NEAR(f.quad[2][0], 150, 1e-8);
    CHECK_NEAR(f.pivot[0], 50, 1e-8);
    f = frame();
    const double point[2] = {200, 110};
    CHECK(pc_ui_transform_apply(&f, &d, point, PC_TRANSFORM_FREE, false, false, false, false, true, false));
    CHECK_NEAR(f.quad[2][1], 110, 1e-8);
    f = frame();
    CHECK(pc_ui_transform_apply(&f, &d, point, PC_TRANSFORM_FREE, false, false, false, false, false, true));
    CHECK_NEAR(f.quad[2][1], 110, 1e-8);
    d = drag(f, PC_TRANSFORM_PIVOT, 0);
    pc_ui_transform_frame before = f;
    CHECK(apply(&f, &d, 23, 42, PC_TRANSFORM_FREE, false, false, false, false));
    CHECK(!memcmp(f.quad, before.quad, sizeof f.quad));
    CHECK(f.pivot[0] == 23 && f.pivot[1] == 42);
}
static void test_rotate_skew_distort_perspective_and_type(void)
{
    pc_ui_transform_frame f = frame();
    pc_ui_transform_drag d = drag(f, PC_TRANSFORM_CORNER, 1);
    CHECK(apply(&f, &d, 100, 100, PC_TRANSFORM_ROTATE, false, false, false, false));
    CHECK_NEAR(f.quad[0][0], 100, 1e-8);
    CHECK_NEAR(f.quad[0][1], 0, 1e-8);
    f = frame();
    d = drag(f, PC_TRANSFORM_CORNER, 0);
    CHECK(apply(&f, &d, 20, 8, PC_TRANSFORM_PERSPECTIVE, false, false, false, false));
    CHECK_NEAR(f.quad[0][0], 20, 1e-8);
    CHECK_NEAR(f.quad[0][1], 0, 1e-8);
    CHECK_NEAR(f.quad[1][0], 80, 1e-8);
    f = frame();
    CHECK(apply(&f, &d, 400, 400, PC_TRANSFORM_DISTORT, false, false, false, false));
    CHECK(f.quad[0][0] < 50 && f.quad[0][1] < 50);
    CHECK(pc_algo_convex_quad((const double (*)[2])f.quad));
    f = frame();
    CHECK(apply(&f, &d, 20, 8, PC_TRANSFORM_SKEW, true, false, true, false));
    for (size_t a = 0; a < 2; a++)
        CHECK_NEAR(f.quad[0][a] + f.quad[2][a], f.quad[1][a] + f.quad[3][a], 1e-8);
    CHECK_NEAR(f.quad[0][0], 20, 1e-8);
    CHECK_NEAR(f.quad[1][0], 120, 1e-8);
    CHECK_NEAR(f.quad[2][0], 80, 1e-8);
    f = frame();
    CHECK(apply(&f, &d, -20, -8, PC_TRANSFORM_DISTORT, true, false, false, true));
    for (size_t a = 0; a < 2; a++)
        CHECK_NEAR(f.quad[0][a] + f.quad[2][a], f.quad[1][a] + f.quad[3][a], 1e-8);
}
static void test_direction_hysteresis_and_bad_input_preserves_frame(void)
{
    pc_ui_transform_frame f = frame();
    pc_ui_transform_drag d = drag(f, PC_TRANSFORM_INSIDE, 0);
    d.start[0] = d.start[1] = 0;
    CHECK(apply(&f, &d, 10, 3, PC_TRANSFORM_FREE, false, true, false, false));
    CHECK(f.quad[0][1] == 0);
    CHECK(apply(&f, &d, 10, 5, PC_TRANSFORM_FREE, false, true, false, false));
    CHECK(f.quad[0][1] == 0);
    CHECK(apply(&f, &d, 10, 8, PC_TRANSFORM_FREE, false, true, false, false));
    CHECK_NEAR(f.quad[0][0], f.quad[0][1], 1e-8);
    pc_ui_transform_frame before = f;
    CHECK(!apply(&f, &d, NAN, 0, PC_TRANSFORM_FREE, false, false, false, false));
    CHECK(!memcmp(&f, &before, sizeof f));
    d.start[0] = INFINITY;
    CHECK(!apply(&f, &d, 10, 10, PC_TRANSFORM_FREE, false, false, false, false));
    CHECK(!memcmp(&f, &before, sizeof f));
}
static void test_transform_state_roundtrip_and_atomic_rejection(void)
{
    pc_ui_state state;
    pc_ui_state_init(&state);
    state.transform = (pc_ui_transform){.active = true,
                                        .session = 7,
                                        .document = 8,
                                        .revision = 9,
                                        .layer = 10,
                                        .mode = PC_TRANSFORM_SKEW,
                                        .target = 3,
                                        .alpha = 2};
    state.transform.frame = frame();
    memcpy(state.transform.interpolation, "bicubic", 8);
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = NULL;
    CHECK(pc_ui_state_to_json(&state, d, &root, NULL) == PC_OK);
    pc_buf text = PC_BUF_INIT;
    CHECK(pc_json_write(root, 0, &text, NULL) == PC_OK);
    yyjson_doc *parsed = NULL;
    CHECK(pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, NULL) == PC_OK);
    pc_ui_state other;
    pc_ui_state_init(&other);
    CHECK(pc_ui_state_from_json(&other, yyjson_doc_get_root(parsed), NULL) == PC_OK);
    CHECK(other.transform.active && other.transform.target == 3 && other.transform.alpha == 2);
    CHECK(other.transform.mode == PC_TRANSFORM_SKEW && other.transform.layer == 10);
    CHECK(!memcmp(other.transform.frame.quad, state.transform.frame.quad, sizeof state.transform.frame.quad));
    yyjson_doc_free(parsed);
    const char *bad = "{\"transform\":{\"rect\":[0,0,0,0]}}";
    CHECK(pc_json_parse(bad, strlen(bad), &parsed, NULL) == PC_OK);
    CHECK(pc_ui_state_from_json(&other, yyjson_doc_get_root(parsed), NULL) != PC_OK);
    CHECK(other.transform.active && other.transform.layer == 10);
    yyjson_doc_free(parsed);
    yyjson_mut_doc_free(d);
    pc_buf_fini(&text);
    pc_ui_state_fini(&state);
    pc_ui_state_fini(&other);
}
static void test_numeric_reference_link_rotation_skew_and_bounds(void)
{
    pc_ui_transform_frame f = frame();
    double values[7];
    CHECK(pc_ui_transform_numeric(&f, 4, 2, 200, true));
    pc_ui_transform_measure(&f, 4, values);
    CHECK_NEAR(values[2], 200, 1e-8);
    CHECK_NEAR(values[3], 200, 1e-8);
    CHECK_NEAR(values[0], 50, 1e-8);
    CHECK(pc_ui_transform_numeric(&f, 0, 0, 20, false));
    CHECK_NEAR(f.quad[0][0], 20, 1e-8);
    CHECK(pc_ui_transform_numeric(&f, 4, 4, 90, false));
    pc_ui_transform_measure(&f, 4, values);
    CHECK_NEAR(values[4], 90, 1e-8);
    CHECK_NEAR(values[0], 120, 1e-8);
    CHECK(pc_ui_transform_numeric(&f, 4, 5, 30, false));
    pc_ui_transform_measure(&f, 4, values);
    CHECK_NEAR(values[5], 30, 1e-8);
    pc_ui_transform_frame before = f;
    CHECK(!pc_ui_transform_numeric(&f, 4, 2, 0, false));
    CHECK(!pc_ui_transform_numeric(&f, 4, 5, 90, false));
    CHECK(!pc_ui_transform_numeric(&f, 4, 0, INFINITY, false));
    CHECK(!memcmp(&before, &f, sizeof f));
    f = frame();
    CHECK(pc_ui_transform_numeric(&f, 9, 2, 200, false));
    pc_ui_transform_measure(&f, 9, values);
    CHECK_NEAR(values[3], 100, 1e-8);
    CHECK(pc_ui_transform_numeric(&f, 9, 6, 20, false));
    for (size_t i = 0; i < 4; i++)
        CHECK(isfinite(f.quad[i][0]) && isfinite(f.quad[i][1]));
}
int main(int argc, char **argv)
{
    RUN(test_numeric_reference_link_rotation_skew_and_bounds);
    RUN(test_handles_scaling_modifiers_and_pivot);
    RUN(test_rotate_skew_distort_perspective_and_type);
    RUN(test_direction_hysteresis_and_bad_input_preserves_frame);
    RUN(test_transform_state_roundtrip_and_atomic_rejection);
    return pc_t_finish();
}
