#ifndef PC_GPU_PREFIX_H
#define PC_GPU_PREFIX_H
#include "gpu/gpu.h"
#define PC_GPU_PREFIX_CELLS 512
#define PC_GPU_PREFIX_COVERAGE 16
typedef struct pc_gpu_prefix_cell {
    pc_rect cell, covered[PC_GPU_PREFIX_COVERAGE];
    size_t count;
    GLuint texture;
} pc_gpu_prefix_cell;
typedef struct pc_gpu_prefix {
    pc_document snapshot;
    bool has_snapshot, active;
    pc_gpu_checkpoint checkpoint;
    pc_gpu_prefix_cell cells[PC_GPU_PREFIX_CELLS];
    size_t count;
    uint64_t hits, budget;
    bool configured;
    float gamma;
} pc_gpu_prefix;
uint64_t pc_gpu_prefix_budget(const pc_gpu_prefix *prefix);
void pc_gpu_prefix_set_budget(const gl33 *gl, pc_gpu_prefix *prefix, uint64_t bytes, bool lost);
void pc_gpu_prefix_invalidate(const gl33 *gl, pc_gpu_prefix *prefix, bool lost);
void pc_gpu_prefix_fini(const gl33 *gl, pc_gpu_prefix *prefix, bool lost);
/* Exact COW snapshot comparison, including every non-layer document field. Cache preparation
 * may fail allocation without preventing a correct uncached render. */
pc_status pc_gpu_prefix_prepare(const gl33 *gl, pc_gpu_prefix *prefix, const pc_document *doc,
                                const pc_gpu_plan *plan, pc_error *err);
pc_rect pc_gpu_prefix_cell_of(pc_rect area);
bool pc_gpu_prefix_covered(const pc_gpu_prefix_cell *cell, pc_rect area);
void pc_gpu_prefix_cover(pc_gpu_prefix_cell *cell, pc_rect area);
bool pc_gpu_prefix_copy(const gl33 *gl, pc_gpu_prefix *prefix, GLuint framebuffer, pc_rect area, GLuint dst);
void pc_gpu_prefix_capture(const gl33 *gl, pc_gpu_prefix *prefix, GLuint framebuffer, pc_rect area,
                           GLuint src);
#endif
