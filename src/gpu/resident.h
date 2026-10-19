#ifndef PC_GPU_RESIDENT_H
#define PC_GPU_RESIDENT_H
#include "gpu/gpu.h"
#include "color/convert.h"
/* At most 128 MiB of RGBA32F textures, plus retained immutable source tiles.
 * Exact tile identity avoids hashing pixels and forces subsequent edits through COW. */
#define PC_GPU_RESIDENT_CAP 128
typedef struct pc_gpu_resident_entry {
    pc_raster_tile *tile;
    pc_cmyk_space *space;
    pc_pixel_format format;
    uint8_t default_pixel[PC_RASTER_MAX_PIXEL_BYTES];
    size_t bpp;
    pc_rect rect;
    GLuint texture;
    uint64_t stamp;
} pc_gpu_resident_entry;
typedef struct pc_gpu_resident {
    pc_gpu_resident_entry entries[PC_GPU_RESIDENT_CAP];
    size_t count;
    uint64_t clock, hits, uploads, bytes;
} pc_gpu_resident;
void pc_gpu_resident_fini(const gl33 *gl, pc_gpu_resident *cache, bool lost);
bool pc_gpu_resident_copy(const gl33 *gl, pc_gpu_resident *cache, GLuint framebuffer, GLuint destination,
                          const pc_surface *surface, pc_rect rect);
void pc_gpu_resident_store(const gl33 *gl, pc_gpu_resident *cache, const pc_surface *surface, pc_rect rect,
                           const void *pixels);
#endif
