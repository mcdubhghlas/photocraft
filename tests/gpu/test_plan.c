#include <stdlib.h>
#include "harness.h"
#include "base/mem.h"
#include "gpu/gpu.h"

static void test_plan_liveness_and_atomic_rejection(void)
{
    pc_document doc;
    CHECK(pc_document_init(&doc, "plan", pc_size_make(8, 8), PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, NULL) ==
          PC_OK);
    doc.layers = pc_calloc_array(100, sizeof *doc.layers);
    CHECK(doc.layers != NULL);
    doc.nlayers = 100;
    for (size_t i = 0; i < 100; i++) {
        CHECK(pc_layer_raster(&doc.layers[i], "raster", pc_document_pixel_format(&doc), NULL) == PC_OK);
    }
    pc_gpu_plan plan = {0};
    CHECK(pc_gpu_plan_build(&doc, &plan, NULL) == PC_OK);
    CHECK(plan.count == 201 && plan.slots == 3);
    pc_gpu_pass *previous = plan.passes;
    doc.layers[50].advanced.knockout = PC_KNOCKOUT_DEEP;
    CHECK(pc_gpu_plan_build(&doc, &plan, NULL) == PC_ERR_UNSUPPORTED);
    CHECK(plan.passes == previous && plan.count == 201);
    doc.layers[50].visible = false;
    CHECK(pc_gpu_plan_build(&doc, &plan, NULL) == PC_OK);
    CHECK(plan.count == 199 && plan.slots == 3);
    pc_gpu_plan_fini(&plan);
    pc_document_fini(&doc);
}
static void test_headless_invalid_context(void)
{
    gl33 table = {0};
    pc_gpu_compositor *out = NULL;
    CHECK(pc_gpu_compositor_new(&table, &out, NULL) == PC_ERR_INVALID);
    CHECK(out == NULL);
    CHECK(pc_gpu_compositor_new(NULL, &out, NULL) == PC_ERR_INVALID);
    CHECK(!pc_gpu_compositor_healthy(NULL));
    pc_gpu_compositor_free(NULL);
}
int main(int argc, char **argv)
{
    RUN(test_plan_liveness_and_atomic_rejection);
    RUN(test_headless_invalid_context);
    return pc_t_finish();
}
