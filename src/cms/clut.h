/* Multi-dimensional colour lookup tables with tetrahedral interpolation (port of
 * crates/cms/src/clut.rs).
 *
 * ICC grid layout: the first input varies slowest; a node's outputs are stored together.
 * 3 inputs: tetrahedral interpolation; 4 inputs: linear along the first input between two
 * tetrahedral evaluations; other counts: multilinear. A table owns its data (pc_cms_clut_fini). */
#ifndef PC_CMS_CLUT_H
#define PC_CMS_CLUT_H

#include <stddef.h>

#include "base/status.h"

#define PC_CLUT_MAX_CHANNELS 15

typedef struct pc_cms_clut {
    size_t inputs;
    size_t outputs;
    size_t grid[PC_CLUT_MAX_CHANNELS];    /* points per input (>= 2, or 1 for degenerate) */
    size_t strides[PC_CLUT_MAX_CHANNELS]; /* values to step one grid point along each input */
    float *data;                          /* owned, normalised node values */
    size_t len;                           /* = nodes * outputs */
} pc_cms_clut;

/* Build from node data. Takes ownership of data in every case (freed on failure). Fails with
 * PC_ERR_INVALID on bad dimensions or a data length that doesn't match the grid. */
pc_status pc_cms_clut_init(pc_cms_clut *out, const size_t *grid, size_t inputs, size_t outputs, float *data,
                           size_t len, pc_error *err);

/* Sample f at every node (inputs normalised to [0, 1]) into a new table. */
typedef void (*pc_cms_clut_fn)(void *ctx, const float *in, float *out);
pc_status pc_cms_clut_sample(const size_t *grid, size_t inputs, size_t outputs, pc_cms_clut_fn f, void *ctx,
                             pc_cms_clut *out, pc_error *err);

void pc_cms_clut_fini(pc_cms_clut *c);
pc_status pc_cms_clut_clone(const pc_cms_clut *c, pc_cms_clut *out, pc_error *err);

size_t pc_cms_clut_nodes(const pc_cms_clut *c);

/* Interpolate at `inputs` values into `outputs` values. */
void pc_cms_clut_eval(const pc_cms_clut *c, const float *in, float *out);
/* Multilinear interpolation over all 2^n cube corners (also the trilinear reference). */
void pc_cms_clut_eval_multilinear(const pc_cms_clut *c, const float *in, float *out);
/* Tetrahedral interpolation in the cube at data offset base, with strides s and fractions r
 * along three dimensions; writes min(outputs, 15) values (the transform fast paths). */
void pc_cms_clut_tetra(const pc_cms_clut *c, size_t base, const size_t s[3], const float r[3], float *out);

#endif
