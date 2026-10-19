#include "image_cache.h"
#include <string.h>
void pc_ui_image_cache_fini(pc_ui_image_cache *cache, const gl33 *gl)
{
    for (size_t i = 0; i < 512; i++) {
        if (cache->entries[i].texture && gl) {
            gl->glDeleteTextures(1, &cache->entries[i].texture);
        }
    }
    memset(cache, 0, sizeof *cache);
}
pc_status pc_ui_image_cache_get(pc_ui_image_cache *cache, const gl33 *gl, const uint8_t key[32],
                                uint16_t width, uint16_t height, const uint8_t *rgba, uint64_t frame,
                                struct nk_image *out, pc_error *err)
{
    if (!cache || !gl || !key || !out || !width || !height || width > 512 || height > 512) {
        return pc_fail(err, PC_ERR_INVALID, "invalid UI image");
    }
    size_t slot = 512;
    uint64_t oldest = UINT64_MAX;
    for (size_t i = 0; i < 512; i++) {
        pc_ui_image_texture *entry = &cache->entries[i];
        if (entry->texture && entry->width == width && entry->height == height &&
            !memcmp(entry->key, key, 32)) {
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
    if (!rgba) {
        return PC_ERR_NOT_FOUND;
    }
    if (slot == 512) {
        return pc_fail(err, PC_ERR_LIMIT, "UI image cache is busy");
    }
    pc_ui_image_texture *entry = &cache->entries[slot];
    size_t bytes = (size_t)width * height * 4, prior = (size_t)entry->width * entry->height * 4;
    while (cache->bytes - prior + bytes > 16u * 1024u * 1024u) {
        size_t victim = 512;
        uint64_t age = UINT64_MAX;
        for (size_t i = 0; i < 512; i++) {
            const pc_ui_image_texture *candidate = &cache->entries[i];
            if (i != slot && candidate->texture && candidate->frame != frame && candidate->frame < age) {
                victim = i;
                age = candidate->frame;
            }
        }
        if (victim == 512) {
            return pc_fail(err, PC_ERR_LIMIT, "UI image texture budget exceeded");
        }
        pc_ui_image_texture *old = &cache->entries[victim];
        cache->bytes -= (size_t)old->width * old->height * 4;
        gl->glDeleteTextures(1, &old->texture);
        memset(old, 0, sizeof *old);
    }
    GLint active = 0, binding = 0, alignment = 0;
    gl->glGetIntegerv(0x84e0, &active);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glGetIntegerv(0x8069, &binding);
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
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    GLenum error = gl->glGetError();
    gl->glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
    gl->glBindTexture(GL_TEXTURE_2D, (GLuint)binding);
    gl->glActiveTexture((GLenum)active);
    if (!texture || error != GL_NO_ERROR) {
        if (texture) {
            gl->glDeleteTextures(1, &texture);
        }
        return pc_fail(err, PC_ERR_IO, "UI image upload failed");
    }
    if (entry->texture) {
        gl->glDeleteTextures(1, &entry->texture);
    }
    cache->bytes = cache->bytes - prior + bytes;
    memcpy(entry->key, key, 32);
    entry->texture = texture;
    entry->width = width;
    entry->height = height;
    entry->frame = frame;
    *out = nk_image_id((int)texture);
    return PC_OK;
}
