#ifndef PC_UI_SHELL_ICON_CACHE_H
#define PC_UI_SHELL_ICON_CACHE_H
#include "gl33.h"
#include "photocraft_nuklear.h"
#include "base/status.h"
/* Textures used by the current Nuklear frame stay alive until its draw commands finish. */
typedef struct pc_ui_icon_texture {
    const char *name;
    GLuint texture;
    uint16_t side;
    uint64_t frame;
} pc_ui_icon_texture;
typedef struct pc_ui_icon_cache {
    pc_ui_icon_texture entries[256];
    size_t bytes;
} pc_ui_icon_cache;
void pc_ui_icon_cache_fini(pc_ui_icon_cache *cache, const gl33 *gl);
pc_status pc_ui_icon_cache_get(pc_ui_icon_cache *cache, const gl33 *gl, const char *name, uint16_t side,
                               uint64_t frame, struct nk_image *out, pc_error *err);
#endif
