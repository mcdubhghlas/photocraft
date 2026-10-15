/* Snapping parity (crates/engine/src/snap.rs tests), ported assertion for assertion. */
#include <stdlib.h>

#include "color/color.h"
#include "engine/snap.h"
#include "harness.h"

static void targets(pc_snap_targets *t)
{
    pc_snap_targets_init(t);
    t->extent[0] = 0.0;
    t->extent[1] = 0.0;
    t->extent[2] = 400.0;
    t->extent[3] = 300.0;
    double r[4] = {0.0, 0.0, 400.0, 300.0};
    (void)pc_snap_targets_add_rect(t, r, PC_SNAP_DOCUMENT_EDGE, PC_SNAP_DOCUMENT_CENTER, NULL);
    pc_snap_target g = {120.0, PC_SNAP_GUIDE, {0.0, 300.0}};
    pc_snap_target *grown = realloc(t->x, (t->nx + 1) * sizeof *t->x);
    CHECK(grown != NULL);
    t->x = grown;
    t->cap_x = t->nx + 1;
    t->x[t->nx++] = g;
}

static void test_point_snaps_within_threshold_only(void)
{
    pc_snap_targets t;
    targets(&t);
    double p[2], out[2];
    pc_snap_line *lines = NULL;
    size_t nlines = 0;
    p[0] = 123.0;
    p[1] = 151.0;
    (void)pc_snap_targets_snap_point(&t, p, 4.0, out, &lines, &nlines, NULL);
    CHECK(out[0] == 120.0 && out[1] == 150.0);
    CHECK_EQ_UINT(nlines, 2);
    CHECK(lines[0].kind == PC_SNAP_GUIDE);
    CHECK(lines[1].kind == PC_SNAP_DOCUMENT_CENTER);
    free(lines);
    p[0] = 130.0;
    p[1] = 160.0;
    lines = NULL;
    nlines = 0;
    (void)pc_snap_targets_snap_point(&t, p, 4.0, out, &lines, &nlines, NULL);
    CHECK(out[0] == 130.0 && out[1] == 160.0);
    CHECK_EQ_UINT(nlines, 0);
    free(lines);
    pc_snap_targets_fini(&t);
}

static void test_threshold_scales_with_zoom(void)
{
    pc_snap_targets t;
    targets(&t);
    pc_snap_target got;
    CHECK(!pc_snap_targets_nearest(&t, true, 125.0, 8.0 / 4.0, &got));
    CHECK(pc_snap_targets_nearest(&t, true, 145.0, 8.0 / 0.25, &got) && got.pos == 120.0);
    pc_snap_targets_fini(&t);
}

static void test_guides_win_ties(void)
{
    pc_snap_targets t;
    targets(&t);
    pc_snap_target le = {120.0, PC_SNAP_LAYER_EDGE, {0.0, 10.0}};
    pc_snap_target *grown = realloc(t.x, (t.nx + 1) * sizeof *t.x);
    CHECK(grown != NULL);
    t.x = grown;
    t.x[t.nx++] = le;
    pc_snap_target got;
    CHECK(pc_snap_targets_nearest(&t, true, 121.0, 5.0, &got) && got.kind == PC_SNAP_GUIDE);
    pc_snap_targets_fini(&t);
}

static void test_grid_snaps_to_nearest_line(void)
{
    pc_snap_targets t;
    pc_snap_targets_init(&t);
    t.has_grid = true;
    t.grid[0] = 25.0;
    t.grid[1] = 25.0;
    t.extent[2] = 100.0;
    t.extent[3] = 100.0;
    double p[2] = {48.0, 61.0}, out[2];
    pc_snap_line *lines = NULL;
    size_t nlines = 0;
    (void)pc_snap_targets_snap_point(&t, p, 3.0, out, &lines, &nlines, NULL);
    CHECK(out[0] == 50.0 && out[1] == 61.0);
    free(lines);
    pc_snap_target got;
    CHECK(pc_snap_targets_nearest(&t, false, 74.0, 3.0, &got) && got.kind == PC_SNAP_GRID);
    pc_snap_targets_fini(&t);
}

static void test_rectangular_grid(void)
{
    pc_snap_targets t;
    pc_snap_targets_init(&t);
    t.has_grid = true;
    t.grid[0] = 128.0;
    t.grid[1] = 72.0;
    t.extent[2] = 1280.0;
    t.extent[3] = 720.0;
    double p[2] = {127.0, 71.0}, out[2];
    pc_snap_line *lines = NULL;
    size_t nlines = 0;
    (void)pc_snap_targets_snap_point(&t, p, 2.0, out, &lines, &nlines, NULL);
    CHECK(out[0] == 128.0 && out[1] == 72.0);
    free(lines);
    pc_snap_target got;
    CHECK(pc_snap_targets_nearest(&t, true, 255.0, 2.0, &got) && got.pos == 256.0);
    CHECK(pc_snap_targets_nearest(&t, false, 143.0, 2.0, &got) && got.pos == 144.0);
    double r[4] = {126.0, 70.0, 160.0, 100.0}, delta[2];
    lines = NULL;
    nlines = 0;
    (void)pc_snap_targets_snap_rect(&t, r, 3.0, delta, &lines, &nlines, NULL);
    CHECK(delta[0] == 2.0 && delta[1] == 2.0);
    bool vgrid = false, hgrid = false;
    for (size_t i = 0; i < nlines; i++) {
        if (lines[i].vertical && lines[i].pos == 128.0 && lines[i].kind == PC_SNAP_GRID) {
            vgrid = true;
        }
        if (!lines[i].vertical && lines[i].pos == 72.0 && lines[i].kind == PC_SNAP_GRID) {
            hgrid = true;
        }
    }
    CHECK(vgrid && hgrid);
    free(lines);
    pc_snap_targets_fini(&t);
}

static void test_rect_snaps_by_closest_anchor(void)
{
    pc_snap_targets t;
    targets(&t);
    double r[4] = {68.0, 10.0, 118.0, 40.0}, d[2];
    pc_snap_line *lines = NULL;
    size_t nlines = 0;
    (void)pc_snap_targets_snap_rect(&t, r, 4.0, d, &lines, &nlines, NULL);
    CHECK(d[0] == 2.0);
    bool at120 = false;
    for (size_t i = 0; i < nlines; i++) {
        if (lines[i].vertical && lines[i].pos == 120.0) {
            at120 = true;
        }
    }
    CHECK(at120);
    free(lines);
    double r2[4] = {148.0, 102.0, 248.0, 202.0};
    lines = NULL;
    nlines = 0;
    (void)pc_snap_targets_snap_rect(&t, r2, 4.0, d, &lines, &nlines, NULL);
    CHECK(d[0] == 2.0 && d[1] == -2.0);
    bool cv = false, ch = false;
    for (size_t i = 0; i < nlines; i++) {
        if (lines[i].kind == PC_SNAP_DOCUMENT_CENTER && lines[i].vertical) {
            cv = true;
        }
        if (lines[i].kind == PC_SNAP_DOCUMENT_CENTER && !lines[i].vertical) {
            ch = true;
        }
    }
    CHECK(cv && ch);
    free(lines);
    pc_snap_targets_fini(&t);
}

static void test_smart_guide_lines_span_both_objects(void)
{
    pc_snap_targets t;
    pc_snap_targets_init(&t);
    double rect[4] = {300.0, 200.0, 350.0, 260.0};
    (void)pc_snap_targets_add_rect(&t, rect, PC_SNAP_LAYER_EDGE, PC_SNAP_LAYER_CENTER, NULL);
    double r[4] = {302.0, 10.0, 330.0, 40.0}, d[2];
    pc_snap_line *lines = NULL;
    size_t nlines = 0;
    (void)pc_snap_targets_snap_rect(&t, r, 5.0, d, &lines, &nlines, NULL);
    CHECK(d[0] == -2.0);
    bool found = false;
    for (size_t i = 0; i < nlines; i++) {
        if (lines[i].vertical && lines[i].pos == 300.0) {
            CHECK(lines[i].from == 10.0 && lines[i].to == 260.0);
            CHECK(pc_snap_kind_is_smart(lines[i].kind));
            found = true;
        }
    }
    CHECK(found);
    free(lines);
    pc_snap_targets_fini(&t);
}

static void test_targets_from_document(void)
{
    pc_document doc;
    CHECK(pc_document_with_background(&doc, "s", pc_size_make(200, 100), PC_COLOR_MODE_RGB,
                                      PC_SAMPLE_TYPE_U8, pc_color_rgba(1.0f, 1.0f, 1.0f, 1.0f), NULL) == PC_OK);
    double g = 40.0;
    double *gv = realloc(doc.guides.vertical, sizeof(double));
    CHECK(gv != NULL);
    doc.guides.vertical = gv;
    doc.guides.vertical[0] = 40.0;
    doc.guides.nvertical = 1;
    (void)g;
    pc_layer box;
    CHECK(pc_layer_raster(&box, "box", pc_document_pixel_format(&doc), NULL) == PC_OK);
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK(pc_surface_fill_rect(pc_layer_surface_mut(&box), pc_rect_make(10, 20, 30, 60), red, 4, NULL) ==
          PC_OK);
    pc_layer_id id;
    CHECK(pc_document_insert_above(&doc, NULL, &box, &id, NULL) == PC_OK);

    pc_snap_options opts = pc_snap_options_default();
    pc_snap_targets t;
    CHECK(pc_snap_targets_from_document(&doc, &opts, NULL, 0, &t, NULL) == PC_OK);
    bool guide40 = false, le30 = false, lc40 = false, dc100 = false;
    for (size_t i = 0; i < t.nx; i++) {
        if (t.x[i].kind == PC_SNAP_GUIDE && t.x[i].pos == 40.0) guide40 = true;
        if (t.x[i].kind == PC_SNAP_LAYER_EDGE && t.x[i].pos == 30.0) le30 = true;
        if (t.x[i].kind == PC_SNAP_DOCUMENT_CENTER && t.x[i].pos == 100.0) dc100 = true;
    }
    for (size_t i = 0; i < t.ny; i++) {
        if (t.y[i].kind == PC_SNAP_LAYER_CENTER && t.y[i].pos == 40.0) lc40 = true;
    }
    CHECK(guide40 && le30 && lc40 && dc100);
    pc_snap_targets_fini(&t);

    pc_snap_options no_guides = opts;
    no_guides.guides = false;
    CHECK(pc_snap_targets_from_document(&doc, &no_guides, &id, 1, &t, NULL) == PC_OK);
    for (size_t i = 0; i < t.nx; i++) {
        CHECK(!(t.x[i].kind == PC_SNAP_GUIDE));
        CHECK(!(t.x[i].kind == PC_SNAP_LAYER_EDGE && t.x[i].pos == 30.0));
    }
    pc_snap_targets_fini(&t);

    double lr[4];
    CHECK(pc_snap_layer_rect(&doc, id, lr) && lr[0] == 10.0 && lr[1] == 20.0 && lr[2] == 30.0 &&
          lr[3] == 60.0);
    const double rects[2][4] = {{0.0, 0.0, 1.0, 1.0}, {2.0, -1.0, 3.0, 0.5}};
    double u[4];
    CHECK(pc_snap_union(rects, 2, u) && u[0] == 0.0 && u[1] == -1.0 && u[2] == 3.0 && u[3] == 1.0);
    pc_document_fini(&doc);
}

int main(int argc, char **argv)
{
    RUN(test_point_snaps_within_threshold_only);
    RUN(test_threshold_scales_with_zoom);
    RUN(test_guides_win_ties);
    RUN(test_grid_snaps_to_nearest_line);
    RUN(test_rectangular_grid);
    RUN(test_rect_snaps_by_closest_anchor);
    RUN(test_smart_guide_lines_span_both_objects);
    RUN(test_targets_from_document);
    return pc_t_finish();
}
