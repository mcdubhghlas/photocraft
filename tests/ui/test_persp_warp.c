#include "harness.h"
#include "ui/persp_warp.h"
#include <math.h>

static const double BOUNDS[4] = {0, 0, 200, 120};

static void test_layout_snaps_then_warp_and_commit(void)
{
    pc_ui_persp_session s = {0};
    CHECK(pc_ui_persp_begin(&s, 7, 42, "edit.perspectiveWarp", BOUNDS, NULL) == PC_OK);
    double tol = 8.0;
    /* Plane 1: a clean rectangle in empty space. */
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_DOWN, 20, 20, tol, NULL) == PC_OK);
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_UP, 100, 100, tol, NULL) == PC_OK);
    CHECK(s.nplanes == 1);
    /* Plane 2: dragged from the far side toward plane 1 so its left corners snap. */
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_DOWN, 180, 100, tol, NULL) == PC_OK);
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_MOVE, 120, 40, tol, NULL) == PC_OK);
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_UP, 102, 22, tol, NULL) == PC_OK);
    CHECK(s.nplanes == 2);
    CHECK(s.planes[1].src[0][0] == 100 && s.planes[1].src[0][1] == 20);
    CHECK(s.planes[1].src[3][0] == 100 && s.planes[1].src[3][1] == 100);

    /* Switch to Warp; drag the shared top corner. */
    CHECK(pc_ui_persp_set_mode(&s, PC_UI_PERSP_WARP, NULL) == PC_OK);
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_DOWN, 100, 20, tol, NULL) == PC_OK);
    CHECK(s.drag == PC_UI_PERSP_DRAG_CORNER && s.ngroup == 2);
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_MOVE, 100, 5, tol, NULL) == PC_OK);
    CHECK(s.planes[0].dst[1][0] == 100 && s.planes[0].dst[1][1] == 5);
    CHECK(s.planes[1].dst[0][0] == 100 && s.planes[1].dst[0][1] == 5);
    CHECK(s.planes[0].src[1][0] == 100 && s.planes[0].src[1][1] == 20); /* layout kept */
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_UP, 100, 5, tol, NULL) == PC_OK);
    CHECK(s.drag == PC_UI_PERSP_DRAG_NONE);

    CHECK(!pc_ui_persp_is_noop(&s));
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = NULL;
    bool run = false;
    CHECK(pc_ui_persp_commit_params(&s, doc, &params, &run, NULL) == PC_OK);
    CHECK(run && params != NULL);
    yyjson_mut_val *planes = yyjson_mut_obj_get(params, "planes");
    CHECK(planes != NULL && yyjson_mut_arr_size(planes) == 2);
    CHECK(yyjson_mut_get_uint(yyjson_mut_obj_get(params, "layer")) == 42);
    yyjson_mut_doc_free(doc);
    pc_ui_persp_fini(&s);
}

static void test_replace_planes_mid_corner_drag(void) /* #952 */
{
    for (int release = 0; release < 2; release++) {
        pc_ui_persp_session s = {0};
        CHECK(pc_ui_persp_begin(&s, 1, 1, "edit.perspectiveWarp", BOUNDS, NULL) == PC_OK);
        CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_DOWN, 20, 20, 8, NULL) == PC_OK);
        CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_UP, 100, 100, 8, NULL) == PC_OK);
        /* grab a corner */
        CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_DOWN, 20, 20, 8, NULL) == PC_OK);
        CHECK(s.drag == PC_UI_PERSP_DRAG_CORNER);
        /* replace the planes out from under the drag */
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *empty = yyjson_mut_arr(d);
        yyjson_mut_doc_set_root(d, empty);
        yyjson_doc *imm = yyjson_mut_doc_imut_copy(d, NULL);
        CHECK(pc_ui_persp_set_planes_json(&s, yyjson_doc_get_root(imm), NULL) == PC_OK);
        CHECK(s.drag == PC_UI_PERSP_DRAG_NONE && s.nplanes == 0);
        /* the next pointer event must not crash and must leave planes empty */
        pc_ui_pointer_kind k = release ? PC_UI_POINTER_UP : PC_UI_POINTER_MOVE;
        CHECK(pc_ui_persp_pointer(&s, k, 30, 30, 8, NULL) == PC_OK);
        CHECK(s.nplanes == 0);
        yyjson_doc_free(imm);
        yyjson_mut_doc_free(d);
        pc_ui_persp_fini(&s);
    }
}

static void test_empty_identity_and_hostile(void)
{
    pc_ui_persp_session s = {0};
    CHECK(pc_ui_persp_begin(&s, 1, 1, "edit.perspectiveWarp", BOUNDS, NULL) == PC_OK);
    CHECK(pc_ui_persp_is_noop(&s)); /* no planes */
    /* an undragged plane maps onto itself: identity, still a no-op */
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_DOWN, 20, 20, 8, NULL) == PC_OK);
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_UP, 100, 100, 8, NULL) == PC_OK);
    CHECK(pc_ui_persp_is_noop(&s));
    bool run = true;
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *params = (void *)1;
    CHECK(pc_ui_persp_commit_params(&s, doc, &params, &run, NULL) == PC_OK);
    CHECK(!run && params == NULL);
    yyjson_mut_doc_free(doc);

    /* hostile pointer coordinates are rejected without crashing */
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_DOWN, NAN, 10, 8, NULL) != PC_OK);
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_DOWN, 10, INFINITY, 8, NULL) != PC_OK);

    /* a degenerate drag (too small) adds no plane */
    size_t before = s.nplanes;
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_DOWN, 50, 50, 8, NULL) == PC_OK);
    CHECK(pc_ui_persp_pointer(&s, PC_UI_POINTER_UP, 51, 51, 8, NULL) == PC_OK);
    CHECK(s.nplanes == before);

    /* non-array planes JSON rejected */
    CHECK(pc_ui_persp_set_planes_json(&s, NULL, NULL) != PC_OK);
    pc_ui_persp_fini(&s);

    /* non-finite bounds rejected */
    pc_ui_persp_session s2 = {0};
    double bad[4] = {0, 0, NAN, 1};
    CHECK(pc_ui_persp_begin(&s2, 1, 1, "x", bad, NULL) != PC_OK);
    pc_ui_persp_fini(&s2);
}

int main(int argc, char **argv)
{
    RUN(test_layout_snaps_then_warp_and_commit);
    RUN(test_replace_planes_mid_corner_drag);
    RUN(test_empty_identity_and_hostile);
    return pc_t_finish();
}
