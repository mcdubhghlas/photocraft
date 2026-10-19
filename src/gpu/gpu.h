/* OpenGL compositor, independent of SDL/window ownership. All GL calls run on the calling
 * thread with the supplied context current. CPU compositor remains the fallback/oracle. */
#ifndef PC_GPU_GPU_H
#define PC_GPU_GPU_H
#include "compose/compose.h"
#include "gpu/gl33.h"
#include "gpu/adjust.h"
#include "gpu/fx_paint.h"

typedef enum pc_gpu_kernel {
    PC_GPU_CLEAR,
    PC_GPU_CONTENT,
    PC_GPU_BLEND,
    PC_GPU_MASK,
    PC_GPU_LERP,
    PC_GPU_ATOP,
    PC_GPU_ADJUST,
    PC_GPU_ADJMIX,
    PC_GPU_CHANNELS,
    PC_GPU_BLEND_IF,
    PC_GPU_COPY_RECT,
    PC_GPU_ADD_DIFF,
    PC_GPU_FX_INIT,
    PC_GPU_FX_PAINT,
    PC_GPU_FX_MERGE,
    PC_GPU_FX_STROKE_PAINT,
    PC_GPU_FX_STROKE_END
} pc_gpu_kernel;
typedef struct pc_gpu_pass {
    pc_gpu_kernel kernel;
    uint32_t dst, a, b, c;
    bool has_c, has_d;
    uint32_t d, flags;
    int fx_kind, map_index;
    const pc_layer *fx_layer;
    const pc_effect *effect;
    float outside, knockout;
    pc_gpu_paint paint;
    pc_rect clip;
    const pc_layer *layer; /* borrowed until plan is freed */
    pc_blend_mode mode;
    float opacity, fill, gamma;
    pc_gpu_adjust_program adjustment;
    bool has_surface;
    pc_surface surface;
} pc_gpu_pass;
typedef struct pc_gpu_checkpoint {
    size_t layer_end, pass_end;
    uint32_t slot;
} pc_gpu_checkpoint;
typedef struct pc_gpu_plan {
    pc_gpu_pass *passes;
    size_t count, capacity;
    uint32_t slots, root;
    pc_gpu_checkpoint *checkpoints;
    size_t ncheckpoints, checkpoint_capacity;
} pc_gpu_plan;
void pc_gpu_plan_fini(pc_gpu_plan *plan);
/* Transactional. Unsupported document features report UNSUPPORTED before any GPU work. */
pc_status pc_gpu_plan_build(const pc_document *document, pc_gpu_plan *out, pc_error *err);
pc_rect pc_gpu_effect_region(const pc_layer *layer, pc_rect canvas);

typedef struct pc_gpu_compositor pc_gpu_compositor;
pc_status pc_gpu_compositor_new(const gl33 *gl, pc_gpu_compositor **out, pc_error *err);
void pc_gpu_compositor_free(pc_gpu_compositor *gpu);
/* Thread-safe fault reporting; first fault wins, except a device loss upgrades an ordinary
 * error. Once faulted, no further work is submitted. Recreate the compositor to recover. */
void pc_gpu_compositor_fault(pc_gpu_compositor *gpu, bool lost, const char *detail);
bool pc_gpu_compositor_healthy(const pc_gpu_compositor *gpu);
bool pc_gpu_compositor_fault_detail(pc_gpu_compositor *gpu, bool *lost, char detail[256]);
uint64_t pc_gpu_compositor_prefix_hits(const pc_gpu_compositor *gpu);
uint64_t pc_gpu_compositor_prefix_bytes(const pc_gpu_compositor *gpu);
/* Calling thread must own the GL context. Zero disables checkpoints; hard cap 512 MiB.
 * Invalidating a closed document releases its immutable checkpoint snapshot. */
uint64_t pc_gpu_compositor_prefix_budget(const pc_gpu_compositor *gpu);
void pc_gpu_compositor_set_prefix_budget(pc_gpu_compositor *gpu, uint64_t bytes);
void pc_gpu_compositor_forget_document(pc_gpu_compositor *gpu, pc_doc_id document);
/* Immutable checkpoint owner, or zero when no document is retained. GL owner thread only. */
pc_doc_id pc_gpu_compositor_document(const pc_gpu_compositor *gpu);
/* Bounded resident source-tile cache diagnostics, scoped to this GL context. */
uint64_t pc_gpu_compositor_effect_fragments(const pc_gpu_compositor *gpu);
uint64_t pc_gpu_compositor_effect_builds(const pc_gpu_compositor *gpu);
uint64_t pc_gpu_compositor_effect_hits(const pc_gpu_compositor *gpu);
uint64_t pc_gpu_compositor_source_hits(const pc_gpu_compositor *gpu);
uint64_t pc_gpu_compositor_source_uploads(const pc_gpu_compositor *gpu);
uint64_t pc_gpu_compositor_source_bytes(const pc_gpu_compositor *gpu);
uint64_t pc_gpu_compositor_submissions(const pc_gpu_compositor *gpu);
/* Rectangular straight RGBA32F output, identical coordinates to CPU render. Tiled into 256-side
 * chunks. Restores the framebuffer; caller must rebind its GL drawing state. Output is replaced
 * only on complete success. No GPU fallback is hidden inside this call. */
pc_status pc_gpu_render(pc_gpu_compositor *gpu, const pc_document *document, pc_rect area,
                        pc_compose_buffer *out, pc_error *err);
#endif
