#include <stdlib.h>
#include "harness.h"
#include "ui/layer_tree.h"
static void test_order_filter_and_transient_layer(void)
{
    pc_document doc;
    CHECK(pc_document_init(&doc, "tree", pc_size_make(8, 8), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) ==
          PC_OK);
    doc.layers = calloc(3, sizeof *doc.layers);
    CHECK(doc.layers != NULL);
    doc.nlayers = 3;
    CHECK(pc_layer_raster(&doc.layers[0], "bottom", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    CHECK(pc_layer_raster(&doc.layers[2], "top", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    pc_layer *children = calloc(2, sizeof *children);
    CHECK(children != NULL);
    CHECK(pc_layer_raster(&children[0], "child", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    pc_layer *deep = calloc(1, sizeof *deep);
    CHECK(deep != NULL);
    CHECK(pc_layer_raster(deep, "deep", PC_PIXEL_FORMAT_RGBA8, NULL) == PC_OK);
    CHECK(pc_layer_group(&children[1], "inner", deep, 1, NULL) == PC_OK);
    CHECK(pc_layer_group(&doc.layers[1], "outer", children, 2, NULL) == PC_OK);
    doc.layers[1].visible = false;
    pc_ui_layer_rows rows = {0};
    CHECK(pc_ui_layer_rows_build(&doc, NULL, 0, NULL, &rows, NULL) == PC_OK);
    CHECK(rows.count == 6);
    const char *names[] = {"top", "outer", "inner", "deep", "child", "bottom"};
    const unsigned depths[] = {0, 0, 1, 2, 1, 0};
    for (size_t i = 0; i < 6; i++) {
        CHECK(strcmp(rows.items[i].layer->name.ptr, names[i]) == 0);
        CHECK(rows.items[i].depth == depths[i]);
        CHECK(rows.items[i].ancestor_hidden == (i >= 2 && i <= 4));
    }
    children[1].content.u.group.expanded = false;
    CHECK(pc_ui_layer_rows_build(&doc, NULL, 0, NULL, &rows, NULL) == PC_OK);
    CHECK(rows.count == 5);
    const char *filter[] = {"pixel"};
    CHECK(pc_ui_layer_rows_build(&doc, filter, 1, NULL, &rows, NULL) == PC_OK);
    CHECK(rows.count == 4 && rows.items[1].depth == 2);
    pc_layer_id omit = deep->id;
    CHECK(pc_ui_layer_rows_build(&doc, filter, 1, &omit, &rows, NULL) == PC_OK);
    CHECK(rows.count == 3);
    pc_ui_layer_row *previous = rows.items;
    CHECK(pc_ui_layer_rows_build(&doc, NULL, 1, NULL, &rows, NULL) == PC_ERR_INVALID);
    CHECK(rows.items == previous && rows.count == 3);
    pc_ui_layer_rows_fini(&rows);
    pc_document_fini(&doc);
}
int main(int argc, char **argv)
{
    RUN(test_order_filter_and_transient_layer);
    return pc_t_finish();
}
