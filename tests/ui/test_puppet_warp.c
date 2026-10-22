#include "harness.h"
#include "ui/puppet_warp.h"
#include <math.h>
#include <string.h>

/* An opaque L-shaped blob, like the algo puppet test, so the mesh is non-empty. */
static void blob(pc_surface *s)
{
    pc_pixel_format f = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};
    pc_surface_init(s, f);
    for (int32_t y = 10; y < 70; y++)
        for (int32_t x = 20; x < 36; x++) {
            float px[4] = {0.5f, 0.5f, 0.5f, 1.0f};
            (void)pc_surface_write_pixel(s, x, y, px, 4, NULL);
        }
    for (int32_t y = 56; y < 70; y++)
        for (int32_t x = 36; x < 70; x++) {
            float px[4] = {0.3f, 0.6f, 0.8f, 1.0f};
            (void)pc_surface_write_pixel(s, x, y, px, 4, NULL);
        }
}

static const double BOUNDS[4] = {0, 0, 80, 80};

static void place(pc_ui_puppet_session *s, double x, double y)
{
    CHECK(pc_ui_puppet_pointer(s, PC_UI_POINTER_DOWN, x, y, 8, false, NULL) == PC_OK);
    CHECK(pc_ui_puppet_pointer(s, PC_UI_POINTER_UP, x, y, 8, false, NULL) == PC_OK);
}

static void test_pin_undo_redo(void)
{
    pc_surface surf;
    blob(&surf);
    pc_ui_puppet_session s = {0};
    CHECK(pc_ui_puppet_begin(&s, 3, 9, "edit.puppetWarp", BOUNDS, &surf, NULL) == PC_OK);
    pc_surface_fini(&surf);
    CHECK(s.warp.npins == 0 && pc_ui_puppet_is_noop(&s));

    place(&s, 28, 20); /* pin 0 */
    CHECK(s.warp.npins == 1 && s.nundo == 1);
    place(&s, 50, 62); /* pin 1 */
    CHECK(s.warp.npins == 2 && s.nundo == 2);

    /* drag pin 1 up: one undo step */
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_DOWN, 50, 62, 8, false, NULL) == PC_OK);
    CHECK(s.has_drag && s.drag == 1);
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_MOVE, 50, 40, 8, false, NULL) == PC_OK);
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_UP, 50, 40, 8, false, NULL) == PC_OK);
    CHECK(s.nundo == 3 && s.warp.pins[1].dst[1] == 40);
    CHECK(!pc_ui_puppet_is_noop(&s));

    /* a no-op click records nothing */
    place(&s, 50, 40);
    CHECK(s.nundo == 3);

    /* alt-click removes pin 0 */
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_DOWN, 28, 20, 8, true, NULL) == PC_OK);
    CHECK(s.warp.npins == 1 && s.nundo == 4);

    /* undo brings the pin back; undo again reverts the drag */
    CHECK(pc_ui_puppet_undo(&s) && s.warp.npins == 2);
    CHECK(pc_ui_puppet_undo(&s) && s.warp.pins[1].dst[1] == 62);
    CHECK(s.nredo == 2);
    /* redo re-applies the drag */
    CHECK(pc_ui_puppet_redo(&s) && s.warp.pins[1].dst[1] == 40);

    /* a new edit after undo clears redo */
    CHECK(pc_ui_puppet_undo(&s)); /* back to pin1 at 62 */
    size_t redo_before = s.nredo;
    CHECK(redo_before > 0);
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_DOWN, 28, 20, 8, true, NULL) == PC_OK); /* remove pin0 */
    CHECK(s.nredo == 0);

    /* undo to empty eventually stops */
    for (int i = 0; i < 20; i++)
        (void)pc_ui_puppet_undo(&s);
    CHECK(!pc_ui_puppet_undo(&s));
    pc_ui_puppet_fini(&s);
}

static void test_commit_and_modes(void)
{
    pc_surface surf;
    blob(&surf);
    pc_ui_puppet_session s = {0};
    CHECK(pc_ui_puppet_begin(&s, 1, 42, "edit.puppetWarp", BOUNDS, &surf, NULL) == PC_OK);
    pc_surface_fini(&surf);

    place(&s, 28, 20);
    place(&s, 50, 62);
    /* clicking off the mesh adds nothing */
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_DOWN, 5, 5, 8, false, NULL) == PC_OK);
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_UP, 5, 5, 8, false, NULL) == PC_OK);
    CHECK(s.warp.npins == 2);

    /* drag so the warp is non-trivial */
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_DOWN, 50, 62, 8, false, NULL) == PC_OK);
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_UP, 50, 40, 8, false, NULL) == PC_OK);

    CHECK(pc_ui_puppet_set_mode(&s, PC_ALGO_PUPPET_RIGID, NULL) == PC_OK);
    CHECK(pc_ui_puppet_set_density(&s, PC_ALGO_PUPPET_MORE, NULL) == PC_OK);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = NULL;
    bool run = false;
    CHECK(pc_ui_puppet_commit_params(&s, doc, &params, &run, NULL) == PC_OK);
    CHECK(run && params != NULL);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(params, "layer")) == 42);
    yyjson_mut_val *pins = yyjson_mut_obj_get(params, "pins");
    CHECK(pins != NULL && yyjson_mut_arr_size(pins) == 2);
    const char *mode = yyjson_mut_get_str(yyjson_mut_obj_get(params, "mode"));
    CHECK_EQ_STR(mode, "rigid");
    yyjson_mut_doc_free(doc);
    pc_ui_puppet_fini(&s);
}

static void test_expansion_empties_mesh_refused(void) /* #951 */
{
    pc_surface surf;
    blob(&surf);
    pc_ui_puppet_session s = {0};
    CHECK(pc_ui_puppet_begin(&s, 1, 1, "edit.puppetWarp", BOUNDS, &surf, NULL) == PC_OK);
    pc_surface_fini(&surf);
    double before = s.warp.expansion;
    CHECK(before == 2.0);
    /* a huge negative expansion erodes the blob to nothing: refused, previous restored */
    pc_error e = {0};
    CHECK(pc_ui_puppet_set_expansion(&s, -200.0, &e) != PC_OK);
    CHECK(s.warp.expansion == before);
    CHECK(s.solver != NULL); /* restored to the working mesh */
    /* a small valid change works */
    CHECK(pc_ui_puppet_set_expansion(&s, 4.0, NULL) == PC_OK);
    CHECK(s.warp.expansion == 4.0);
    pc_ui_puppet_fini(&s);
}

static void test_hostile(void)
{
    pc_surface surf;
    blob(&surf);
    pc_ui_puppet_session s = {0};
    CHECK(pc_ui_puppet_begin(&s, 1, 1, "edit.puppetWarp", BOUNDS, &surf, NULL) == PC_OK);
    pc_surface_fini(&surf);
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_DOWN, NAN, 10, 8, false, NULL) != PC_OK);
    CHECK(pc_ui_puppet_pointer(&s, PC_UI_POINTER_DOWN, 10, INFINITY, 8, false, NULL) != PC_OK);
    /* empty bounds -> no mesh -> begin fails */
    pc_surface e2;
    pc_surface_init(&e2, (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true});
    pc_ui_puppet_session s2 = {0};
    double b0[4] = {0, 0, 0, 0};
    CHECK(pc_ui_puppet_begin(&s2, 1, 1, "x", b0, &e2, NULL) != PC_OK);
    pc_surface_fini(&e2);
    pc_ui_puppet_fini(&s2);
    pc_ui_puppet_fini(&s);
}

int main(int argc, char **argv)
{
    RUN(test_pin_undo_redo);
    RUN(test_commit_and_modes);
    RUN(test_expansion_empties_mesh_refused);
    RUN(test_hostile);
    return pc_t_finish();
}
