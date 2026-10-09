#include "gamut.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "builtin.h"
#include "clut.h"
#include "pipeline.h"
#include "transform.h"

struct pc_gamut_check {
    size_t inputs;
    bool has_grid;
    pc_cms_clut grid;
    pc_cms_pipeline direct;
    pc_cms_pipeline via;
    float threshold;
};

static void de_fn(void *ctx, const float *p, float *o)
{
    const pc_gamut_check *g = ctx;
    float a[PC_PIPE_MAX];
    float b[PC_PIPE_MAX];
    float d0;
    float d1;
    float d2;
    pc_cms_pipeline_eval(&g->direct, p, a);
    pc_cms_pipeline_eval(&g->via, p, b);
    d0 = (a[0] - b[0]) * 100.0f;
    d1 = (a[1] - b[1]) * 255.0f;
    d2 = (a[2] - b[2]) * 255.0f;
    o[0] = sqrtf(d0 * d0 + d1 * d1 + d2 * d2);
}

void pc_gamut_check_free(pc_gamut_check *g)
{
    if (g != NULL) {
        pc_cms_clut_fini(&g->grid);
        pc_cms_pipeline_fini(&g->direct);
        pc_cms_pipeline_fini(&g->via);
        free(g);
    }
}

pc_status pc_gamut_check_new(const pc_profile *src, const pc_profile *proof, float threshold, pc_gamut_check **out,
                             pc_error *err)
{
    const pc_profile *lab = pc_builtin_profile(PC_BUILTIN_LAB_D50);
    pc_gamut_check *g;
    pc_cms_stages direct = PC_CMS_STAGES_INIT;
    pc_cms_stages via = PC_CMS_STAGES_INIT;
    pc_cms_stages back = PC_CMS_STAGES_INIT;
    pc_cms_stage clamp = pc_cms_stage_simple(PC_STAGE_CLAMP01);
    size_t n = pc_profile_channels(src);
    pc_status s;
    if (lab == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory building the Lab profile");
    }
    g = calloc(1, sizeof *g);
    if (g == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s = pc_cms_link_stages(src, lab, PC_INTENT_RELATIVE, false, &direct, err);
    if (s == PC_OK) {
        s = pc_cms_link_stages(src, proof, PC_INTENT_RELATIVE, false, &via, err);
    }
    if (s == PC_OK) {
        s = pc_cms_stages_push(&via, &clamp, err);
    }
    if (s == PC_OK) {
        s = pc_cms_link_stages(proof, lab, PC_INTENT_RELATIVE, false, &back, err);
    }
    if (s == PC_OK) {
        s = pc_cms_stages_append(&via, &back, err);
    }
    if (s != PC_OK) {
        pc_cms_stages_fini(&direct);
        pc_cms_stages_fini(&via);
        pc_cms_stages_fini(&back);
        free(g);
        return s;
    }
    pc_cms_pipeline_init(&g->direct, n, &direct);
    pc_cms_pipeline_init(&g->via, n, &via);
    g->inputs = n;
    g->threshold = threshold;
    if (n == 1 || n == 3 || n == 4) {
        size_t grid[4];
        size_t gs = n == 1 ? 256 : (n == 3 ? 33 : 17);
        grid[0] = grid[1] = grid[2] = grid[3] = gs;
        s = pc_cms_clut_sample(grid, n, 1, de_fn, g, &g->grid, err);
        if (s != PC_OK) {
            pc_gamut_check_free(g);
            return s;
        }
        g->has_grid = true;
    }
    *out = g;
    return PC_OK;
}

size_t pc_gamut_check_inputs(const pc_gamut_check *g) { return g->inputs; }
float pc_gamut_check_threshold(const pc_gamut_check *g) { return g->threshold; }

float pc_gamut_check_delta_e(const pc_gamut_check *g, const float *px)
{
    float o[1] = {0.0f};
    if (g->has_grid) {
        float c[PC_PIPE_MAX];
        size_t k;
        memset(c, 0, sizeof c);
        for (k = 0; k < g->inputs && k < PC_PIPE_MAX; k++) {
            c[k] = pc_clampf(px[k], 0.0f, 1.0f);
        }
        pc_cms_clut_eval(&g->grid, c, o);
    } else {
        float a[PC_PIPE_MAX];
        float b[PC_PIPE_MAX];
        pc_cms_pipeline_eval(&g->direct, px, a);
        pc_cms_pipeline_eval(&g->via, px, b);
        o[0] = hypotf(hypotf((a[0] - b[0]) * 100.0f, (a[1] - b[1]) * 255.0f), (a[2] - b[2]) * 255.0f);
    }
    return o[0];
}

bool pc_gamut_check_out_of_gamut(const pc_gamut_check *g, const float *px)
{
    return pc_gamut_check_delta_e(g, px) > g->threshold;
}
