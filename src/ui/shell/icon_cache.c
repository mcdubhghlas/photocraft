#include "icon_cache.h"
#include "ui/icons.h"
#include <stdlib.h>
#include <string.h>
void pc_ui_icon_cache_fini(pc_ui_icon_cache *cache, const gl33 *gl)
{
    for (size_t i = 0; i < 256; i++) {
        if (cache->entries[i].texture && gl) {
            gl->glDeleteTextures(1, &cache->entries[i].texture);
        }
    }
    memset(cache, 0, sizeof *cache);
}
pc_status pc_ui_icon_cache_get(pc_ui_icon_cache *cache, const gl33 *gl, const char *name, uint16_t side,
                               uint64_t frame, struct nk_image *out, pc_error *err)
{
    if (!gl || side < 8 || side > 256) {
        return pc_fail(err, PC_ERR_INVALID, "invalid icon texture request");
    }
    if (!pc_ui_icon_exists(name)) {
        name = "square";
    }
    size_t slot = 256;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < 256; i++) {
        pc_ui_icon_texture *entry = &cache->entries[i];
        if (entry->texture && entry->side == side && !strcmp(entry->name, name)) {
            entry->frame = frame;
            *out = nk_image_id((int)entry->texture);
            return PC_OK;
        }
        if (!entry->texture) {
            slot = i;
            oldest = 0;
        } else if (entry->frame != frame && entry->frame < oldest) {
            slot = i;
            oldest = entry->frame;
        }
    }
    if (slot == 256) {
        return pc_fail(err, PC_ERR_LIMIT, "icon texture cache is busy");
    }
    pc_rgba8_image image = {0};
    pc_status status = pc_ui_icon_rasterize(name, side, &image, err);
    if (status != PC_OK) {
        return status;
    }
    /* Restrict the cache to 4 MiB even if a caller requests many unusually large icons. */
    size_t bytes = (size_t)side * side * 4;
    pc_ui_icon_texture *entry = &cache->entries[slot];
    size_t prior = (size_t)entry->side * entry->side * 4;
    if (cache->bytes - prior + bytes > 4u * 1024u * 1024u) {
        free(image.pixels);
        return pc_fail(err, PC_ERR_LIMIT, "icon texture budget exceeded");
    }
    GLint active = 0, binding = 0, alignment = 0;
    gl->glGetIntegerv(0x84E0, &active); /* GL_ACTIVE_TEXTURE */
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glGetIntegerv(0x8069, &binding); /* GL_TEXTURE_BINDING_2D */
    gl->glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
    GLuint texture = 0;
    gl->glGenTextures(1, &texture);
    if (texture) {
        gl->glBindTexture(GL_TEXTURE_2D, texture);
        gl->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.pixels);
    }
    GLenum error = gl->glGetError();
    free(image.pixels);
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    gl->glBindTexture(GL_TEXTURE_2D, (GLuint)binding);
    gl->glActiveTexture((GLenum)active);
    if (!texture || error != GL_NO_ERROR) {
        if (texture) {
            gl->glDeleteTextures(1, &texture);
        }
        return pc_fail(err, PC_ERR_INVALID, "icon texture upload failed");
    }
    if (entry->texture) {
        gl->glDeleteTextures(1, &entry->texture);
    }
    cache->bytes = cache->bytes - prior + bytes;
    /* Retain only the embedded registry's static string, never a borrowed command buffer. */
    const char *canonical = "square";
    for (size_t i = 0; i < pc_ui_icon_count(); i++) {
        if (!strcmp(pc_ui_icon_name(i), name)) {
            canonical = pc_ui_icon_name(i);
            break;
        }
    }
    *entry = (pc_ui_icon_texture){canonical, texture, side, frame};
    *out = nk_image_id((int)texture);
    return PC_OK;
}
