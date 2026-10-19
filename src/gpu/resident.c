#include "gpu/resident.h"
#include <string.h>

static bool tile_for(const pc_surface *s, pc_rect rect, const pc_raster_tile **tile)
{
    pc_tile first, last;
    if (!pc_rect_tile_range(rect, &first, &last) || pc_tile_cmp(first, last))
        return false;
    *tile = pc_surface_tile(s, first);
    return true;
}
static uint64_t stamp(pc_gpu_resident *c)
{
    if (c->clock == UINT64_MAX) {
        for (size_t i = 0; i < c->count; i++)
            c->entries[i].stamp = 0;
        c->clock = 0;
    }
    return ++c->clock;
}
static uint64_t bytes(pc_rect r)
{
    return (uint64_t)pc_rect_width(r) * pc_rect_height(r) * 16;
}
void pc_gpu_resident_fini(const gl33 *gl, pc_gpu_resident *c, bool lost)
{
    for (size_t i = 0; i < c->count; i++) {
        pc_gpu_resident_entry *e = &c->entries[i];
        pc_raster_tile_release(e->tile);
        pc_cmyk_space_release(e->space);
        if (gl && !lost)
            gl->glDeleteTextures(1, &e->texture);
    }
    *c = (pc_gpu_resident){0};
}
bool pc_gpu_resident_copy(const gl33 *gl, pc_gpu_resident *c, GLuint framebuffer, GLuint dst,
                          const pc_surface *s, pc_rect rect)
{
    const pc_raster_tile *tile = NULL;
    if (!tile_for(s, rect, &tile))
        return false;
    for (size_t i = 0; i < c->count; i++) {
        pc_gpu_resident_entry *e = &c->entries[i];
        if (e->tile != tile || e->space != pc_cmyk_space_active() || !pc_rect_eq(e->rect, rect) ||
            !pc_pixel_format_eq(e->format, s->format) || e->bpp != s->bpp ||
            memcmp(e->default_pixel, s->default_pixel, s->bpp))
            continue;
        gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e->texture, 0);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, dst);
        gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)pc_rect_width(rect),
                                (GLsizei)pc_rect_height(rect));
        /* The caller may capture this pass as a compositing checkpoint. */
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
        e->stamp = stamp(c);
        c->hits++;
        return true;
    }
    return false;
}
void pc_gpu_resident_store(const gl33 *gl, pc_gpu_resident *c, const pc_surface *s, pc_rect rect,
                           const void *pixels)
{
    const pc_raster_tile *tile = NULL;
    if (!tile_for(s, rect, &tile))
        return;
    size_t at = c->count;
    if (at == PC_GPU_RESIDENT_CAP) {
        at = 0;
        for (size_t i = 1; i < c->count; i++)
            if (c->entries[i].stamp < c->entries[at].stamp)
                at = i;
    }
    pc_gpu_resident_entry *e = &c->entries[at];
    if (at < c->count) {
        c->bytes -= bytes(e->rect);
        pc_raster_tile_release(e->tile);
        pc_cmyk_space_release(e->space);
    } else {
        gl->glGenTextures(1, &e->texture);
        c->count++;
    }
    e->tile = pc_surface_retain_tile(s, pc_tile_containing(rect.x0, rect.y0));
    e->space = pc_cmyk_space_retain(pc_cmyk_space_active());
    e->format = s->format;
    e->bpp = s->bpp;
    memcpy(e->default_pixel, s->default_pixel, s->bpp);
    e->rect = rect;
    e->stamp = stamp(c);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, e->texture);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, (GLsizei)pc_rect_width(rect),
                     (GLsizei)pc_rect_height(rect), 0, GL_RGBA, GL_FLOAT, pixels);
    c->bytes += bytes(rect);
    c->uploads++;
}
