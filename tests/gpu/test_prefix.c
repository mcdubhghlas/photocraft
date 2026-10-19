#include <stdlib.h>
#include "harness.h"
#include "gpu/prefix.h"
#include "compose/psblend.h"
static void test_grid_and_coverage(void)
{
    CHECK(pc_rect_eq(pc_gpu_prefix_cell_of(pc_rect_make(-1, -257, 0, 0)), pc_rect_make(-256, -512, 0, -256)));
    CHECK(pc_rect_eq(pc_gpu_prefix_cell_of(pc_rect_make(INT32_MIN, INT32_MAX, 0, 0)),
                     pc_rect_make(INT32_MIN, INT32_MAX - 255, INT32_MIN + 256, INT32_MAX)));
    pc_gpu_prefix_cell cell = {.cell = pc_rect_make(0, 0, 256, 256)};
    pc_gpu_prefix_cover(&cell, pc_rect_make(0, 0, 100, 100));
    CHECK(pc_gpu_prefix_covered(&cell, pc_rect_make(10, 20, 90, 80)));
    CHECK(!pc_gpu_prefix_covered(&cell, pc_rect_make(90, 90, 110, 110)));
    pc_gpu_prefix_cover(&cell, pc_rect_make(0, 100, 100, 200));
    CHECK(cell.count == 1);
    CHECK(pc_gpu_prefix_covered(&cell, pc_rect_make(10, 20, 90, 180)));
    pc_gpu_prefix_cover(&cell, pc_rect_make(100, 0, 200, 200));
    CHECK(cell.count == 1);
    CHECK(pc_gpu_prefix_covered(&cell, pc_rect_make(0, 0, 200, 200)));
    pc_gpu_prefix_cover(&cell, pc_rect_make(200, 220, 256, 256));
    CHECK(!pc_gpu_prefix_covered(&cell, pc_rect_make(150, 150, 250, 250)));
    pc_gpu_prefix_cover(&cell, cell.cell);
    CHECK(cell.count == 1 && pc_gpu_prefix_covered(&cell, cell.cell));
}
static void test_exact_snapshot_invalidation(void)
{
    pc_document doc;
    CHECK(pc_document_init(&doc, "prefix", pc_size_make(16, 16), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32,
                           NULL) == PC_OK);
    doc.layers = calloc(3, sizeof *doc.layers);
    CHECK(doc.layers != NULL);
    doc.nlayers = 3;
    for (size_t i = 0; i < 3; i++) {
        CHECK(pc_layer_raster(&doc.layers[i], "layer", PC_PIXEL_FORMAT_RGBA32F, NULL) == PC_OK);
    }
    pc_gpu_plan plan = {0};
    CHECK(pc_gpu_plan_build(&doc, &plan, NULL) == PC_OK);
    CHECK(plan.ncheckpoints == 3);
    pc_gpu_prefix *prefix = calloc(1, sizeof *prefix);
    CHECK(prefix != NULL);
    CHECK(pc_gpu_prefix_budget(prefix) == UINT64_C(128) * 1024 * 1024);
    pc_gpu_prefix_set_budget(NULL, prefix, 0, false);
    CHECK(pc_gpu_prefix_budget(prefix) == 0);
    pc_gpu_prefix_set_budget(NULL, prefix, UINT64_MAX, false);
    CHECK(pc_gpu_prefix_budget(prefix) == UINT64_C(512) * 1024 * 1024);
    pc_gpu_prefix_invalidate(NULL, prefix, false);
    CHECK(pc_gpu_prefix_budget(prefix) == UINT64_C(512) * 1024 * 1024);
    CHECK(pc_gpu_prefix_prepare(NULL, prefix, &doc, &plan, NULL) == PC_OK);
    CHECK(prefix->active && prefix->snapshot.nlayers == 2 && prefix->checkpoint.layer_end == 2);
    pc_gpu_checkpoint checkpoint = prefix->checkpoint;
    doc.layers[2].opacity = .4f;
    CHECK(pc_gpu_plan_build(&doc, &plan, NULL) == PC_OK);
    CHECK(pc_gpu_prefix_prepare(NULL, prefix, &doc, &plan, NULL) == PC_OK);
    CHECK(prefix->active && prefix->checkpoint.layer_end == checkpoint.layer_end);
    doc.layers[1].opacity = .4f;
    CHECK(pc_gpu_plan_build(&doc, &plan, NULL) == PC_OK);
    CHECK(pc_gpu_prefix_prepare(NULL, prefix, &doc, &plan, NULL) == PC_OK);
    CHECK(prefix->active && prefix->snapshot.nlayers == 1);
    doc.layers[0].opacity = .4f;
    CHECK(pc_gpu_plan_build(&doc, &plan, NULL) == PC_OK);
    CHECK(pc_gpu_prefix_prepare(NULL, prefix, &doc, &plan, NULL) == PC_OK);
    CHECK(!prefix->active);
    doc.resolution_dpi = 144;
    CHECK(pc_gpu_prefix_prepare(NULL, prefix, &doc, &plan, NULL) == PC_OK);
    CHECK(prefix->active && prefix->snapshot.resolution_dpi == 144);
    float gamma = pc_psblend_text_gamma();
    pc_psblend_set_text_gamma(1.7f);
    CHECK(pc_gpu_prefix_prepare(NULL, prefix, &doc, &plan, NULL) == PC_OK);
    CHECK(prefix->gamma == 1.7f);
    pc_psblend_set_text_gamma(gamma);
    pc_gpu_prefix_fini(NULL, prefix, false);
    free(prefix);
    pc_gpu_plan_fini(&plan);
    pc_document_fini(&doc);
}
int main(int argc, char **argv)
{
    RUN(test_grid_and_coverage);
    RUN(test_exact_snapshot_invalidation);
    return pc_t_finish();
}
