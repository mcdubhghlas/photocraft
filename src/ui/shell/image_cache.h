#ifndef PC_UI_SHELL_IMAGE_CACHE_H
#define PC_UI_SHELL_IMAGE_CACHE_H
#include "gl33.h"
#include "photocraft_nuklear.h"
#include "base/status.h"
typedef struct pc_ui_image_texture {
    uint8_t key[32];
    GLuint texture;
    uint16_t width, height;
    uint64_t frame;
} pc_ui_image_texture;
typedef struct pc_ui_image_cache {
    pc_ui_image_texture entries[512];
    size_t bytes;
} pc_ui_image_cache;
void pc_ui_image_cache_fini(pc_ui_image_cache *cache, const gl33 *gl);
/* NULL pixels performs a lookup only. Current-frame resources cannot be evicted. */
pc_status pc_ui_image_cache_get(pc_ui_image_cache *cache, const gl33 *gl, const uint8_t key[32],
                                uint16_t width, uint16_t height, const uint8_t *rgba, uint64_t frame,
                                struct nk_image *out, pc_error *err);
#endif
