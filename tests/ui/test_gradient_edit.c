#include "harness.h"
#include "ui/gradient_edit.h"
#include <yyjson.h>

static yyjson_mut_val *make_current(yyjson_mut_doc *d, const char *c0, const char *c1, double a0, double a1)
{
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *stops = yyjson_mut_arr(d);
    yyjson_mut_val *s0 = yyjson_mut_arr(d), *s1 = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, s0, 0.0);
    yyjson_mut_arr_add_strcpy(d, s0, c0);
    yyjson_mut_arr_add_real(d, s1, 1.0);
    yyjson_mut_arr_add_strcpy(d, s1, c1);
    yyjson_mut_arr_add_val(stops, s0);
    yyjson_mut_arr_add_val(stops, s1);
    yyjson_mut_obj_add_val(d, root, "stops", stops);
    yyjson_mut_val *tr = yyjson_mut_arr(d);
    yyjson_mut_val *t0 = yyjson_mut_arr(d), *t1 = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, t0, 0.0);
    yyjson_mut_arr_add_real(d, t0, a0);
    yyjson_mut_arr_add_real(d, t1, 1.0);
    yyjson_mut_arr_add_real(d, t1, a1);
    yyjson_mut_arr_add_val(tr, t0);
    yyjson_mut_arr_add_val(tr, t1);
    yyjson_mut_obj_add_val(d, root, "transparency", tr);
    return root;
}
static void test_init(void)
{
    pc_ui_gradient g;
    pc_ui_gradient_init(&g);
    CHECK_EQ_UINT(g.ncolors, 2);
    CHECK_EQ_UINT(g.nopacity, 2);
    CHECK_NEAR(g.colors[0].rgb[0], 0.0f, 1e-6);
    CHECK_NEAR(g.colors[1].rgb[0], 1.0f, 1e-6);
    CHECK_NEAR(g.opacity[1].opacity, 1.0f, 1e-6);
}
static void test_parse_hex(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *cur = make_current(d, "#ff0000", "#0000ff", 100.0, 0.0);
    pc_ui_gradient g;
    CHECK(pc_ui_gradient_parse(&g, cur, NULL, NULL));
    CHECK_EQ_UINT(g.ncolors, 2);
    CHECK_NEAR(g.colors[0].rgb[0], 1.0f, 1e-2);
    CHECK_NEAR(g.colors[1].rgb[2], 1.0f, 1e-2);
    CHECK_NEAR(g.opacity[0].opacity, 1.0f, 1e-6);
    CHECK_NEAR(g.opacity[1].opacity, 0.0f, 1e-6);
    yyjson_mut_doc_free(d);
}
static void test_parse_named(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *cur = make_current(d, "foreground", "background", 100.0, 100.0);
    float fg[3] = {1, 0, 0}, bg[3] = {0, 1, 0};
    pc_ui_gradient g;
    CHECK(pc_ui_gradient_parse(&g, cur, fg, bg));
    CHECK_NEAR(g.colors[0].rgb[0], 1.0f, 1e-6);
    CHECK_NEAR(g.colors[1].rgb[1], 1.0f, 1e-6);
    yyjson_mut_doc_free(d);
}
static void test_sample(void)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *cur = make_current(d, "#000000", "#ffffff", 100.0, 0.0);
    pc_ui_gradient g;
    CHECK(pc_ui_gradient_parse(&g, cur, NULL, NULL));
    float rgba[4];
    pc_ui_gradient_sample(&g, 0.0f, rgba);
    CHECK_NEAR(rgba[0], 0.0f, 1e-6);
    CHECK_NEAR(rgba[3], 1.0f, 1e-6);
    pc_ui_gradient_sample(&g, 1.0f, rgba);
    CHECK_NEAR(rgba[0], 1.0f, 1e-2);
    CHECK_NEAR(rgba[3], 0.0f, 1e-6);
    pc_ui_gradient_sample(&g, 0.5f, rgba);
    CHECK_NEAR(rgba[0], 0.5f, 5e-2);
    CHECK_NEAR(rgba[3], 0.5f, 5e-2);
}
static void test_nearest(void)
{
    pc_ui_gradient g;
    pc_ui_gradient_init(&g);
    CHECK_EQ_INT(pc_ui_gradient_nearest_color(&g, 0.02f, 0.05f), 0);
    CHECK_EQ_INT(pc_ui_gradient_nearest_color(&g, 0.98f, 0.05f), 1);
    CHECK_EQ_INT(pc_ui_gradient_nearest_color(&g, 0.5f, 0.05f), -1);
    CHECK_EQ_INT(pc_ui_gradient_nearest_opacity(&g, 0.0f, 0.05f), 0);
    /* NULL-safe. */
    CHECK_EQ_INT(pc_ui_gradient_nearest_color(NULL, 0.0f, 0.05f), -1);
}
static void test_parse_null_safe(void)
{
    pc_ui_gradient g;
    CHECK(!pc_ui_gradient_parse(&g, NULL, NULL, NULL));
    CHECK_EQ_UINT(g.ncolors, 2); /* left a sane default */
    CHECK(!pc_ui_gradient_parse(NULL, NULL, NULL, NULL));
}
int main(int argc, char **argv)
{
    RUN(test_init);
    RUN(test_parse_hex);
    RUN(test_parse_named);
    RUN(test_sample);
    RUN(test_nearest);
    RUN(test_parse_null_safe);
    return pc_t_finish();
}
