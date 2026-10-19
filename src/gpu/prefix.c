#include "gpu/prefix.h"
#include "compose/psblend.h"
#include <stdlib.h>
#include <string.h>

static bool contains(pc_rect a, pc_rect b)
{
    return pc_rect_eq(pc_rect_intersect(a, b), b);
}
pc_rect pc_gpu_prefix_cell_of(pc_rect area)
{
    int64_t x = area.x0, y = area.y0;
    x = (x - ((x % 256 + 256) % 256));
    y = (y - ((y % 256 + 256) % 256));
    return pc_rect_make((int32_t)x, (int32_t)y, (int32_t)(x + 256 > INT32_MAX ? INT32_MAX : x + 256),
                        (int32_t)(y + 256 > INT32_MAX ? INT32_MAX : y + 256));
}
bool pc_gpu_prefix_covered(const pc_gpu_prefix_cell *cell, pc_rect area)
{
    if (!cell || pc_rect_is_empty(area) || !contains(cell->cell, area)) {
        return false;
    }
    for (size_t i = 0; i < cell->count; i++) {
        if (contains(cell->covered[i], area)) {
            return true;
        }
    }
    return false;
}
void pc_gpu_prefix_cover(pc_gpu_prefix_cell *cell, pc_rect area)
{
    if (!cell || pc_rect_is_empty(area) || !contains(cell->cell, area)) {
        return;
    }
    for (size_t i = 0; i < cell->count;) {
        pc_rect old = cell->covered[i];
        if (contains(old, area)) {
            return;
        }
        bool rows = old.x0 == area.x0 && old.x1 == area.x1 && old.y1 >= area.y0 && area.y1 >= old.y0;
        bool cols = old.y0 == area.y0 && old.y1 == area.y1 && old.x1 >= area.x0 && area.x1 >= old.x0;
        if (contains(area, old) || rows || cols) {
            area = pc_rect_union(old, area);
            cell->covered[i] = cell->covered[--cell->count];
            i = 0;
        } else {
            i++;
        }
    }
    if (cell->count == PC_GPU_PREFIX_COVERAGE) {
        cell->count = 0;
    }
    cell->covered[cell->count++] = area;
}
static void clear_cells(const gl33 *gl, pc_gpu_prefix *p, bool lost)
{
    if (!lost && gl) {
        for (size_t i = 0; i < p->count; i++) {
            gl->glDeleteTextures(1, &p->cells[i].texture);
        }
    }
    p->count = 0;
    p->active = false;
}
void pc_gpu_prefix_fini(const gl33 *gl, pc_gpu_prefix *p, bool lost)
{
    if (!p) {
        return;
    }
    clear_cells(gl, p, lost);
    if (p->has_snapshot) {
        pc_document_fini(&p->snapshot);
    }
    *p = (pc_gpu_prefix){0};
}
uint64_t pc_gpu_prefix_budget(const pc_gpu_prefix *p)
{
    return p->configured ? p->budget : UINT64_C(128) * 1024 * 1024;
}
void pc_gpu_prefix_set_budget(const gl33 *gl, pc_gpu_prefix *p, uint64_t bytes, bool lost)
{
    const uint64_t tile_bytes = UINT64_C(256) * 256 * 16;
    uint64_t maximum = PC_GPU_PREFIX_CELLS * tile_bytes;
    p->budget = bytes < maximum ? bytes : maximum;
    p->configured = true;
    size_t limit = (size_t)(p->budget / tile_bytes);
    while (p->count > limit) {
        pc_gpu_prefix_cell *cell = &p->cells[--p->count];
        if (gl && !lost)
            gl->glDeleteTextures(1, &cell->texture);
        *cell = (pc_gpu_prefix_cell){0};
    }
}
void pc_gpu_prefix_invalidate(const gl33 *gl, pc_gpu_prefix *p, bool lost)
{
    uint64_t budget = p->budget, hits = p->hits;
    bool configured = p->configured;
    pc_gpu_prefix_fini(gl, p, lost);
    p->budget = budget;
    p->configured = configured;
    p->hits = hits;
}
pc_status pc_gpu_prefix_prepare(const gl33 *gl, pc_gpu_prefix *p, const pc_document *doc,
                                const pc_gpu_plan *plan, pc_error *err)
{
    pc_document context = *doc, old = p->snapshot;
    context.layers = NULL;
    context.nlayers = 0;
    old.layers = NULL;
    old.nlayers = 0;
    float gamma = pc_psblend_text_gamma();
    bool same = p->has_snapshot && p->gamma == gamma && pc_document_eq(&old, &context);
    size_t changed = same ? p->snapshot.nlayers : 0;
    for (size_t i = 0; same && i < p->snapshot.nlayers; i++) {
        if (i >= doc->nlayers || !pc_layer_eq(&p->snapshot.layers[i], &doc->layers[i])) {
            changed = i;
            break;
        }
    }
    const pc_gpu_checkpoint *checkpoint = NULL;
    if (same && changed == p->snapshot.nlayers && p->active) {
        for (size_t i = 0; i < plan->ncheckpoints; i++) {
            const pc_gpu_checkpoint *c = &plan->checkpoints[i];
            if (c->layer_end == p->checkpoint.layer_end && c->pass_end == p->checkpoint.pass_end &&
                c->slot == p->checkpoint.slot) {
                checkpoint = c;
                break;
            }
        }
        if (checkpoint) {
            return PC_OK;
        }
    }
    if (same) {
        for (size_t i = 0; i < plan->ncheckpoints; i++) {
            const pc_gpu_checkpoint *c = &plan->checkpoints[i];
            if (c->pass_end > 1 && c->layer_end <= changed) {
                checkpoint = c;
            }
        }
    } else if (plan->ncheckpoints) {
        for (size_t i = 0; i < plan->ncheckpoints; i++) {
            if (plan->checkpoints[i].pass_end > 1) {
                checkpoint = &plan->checkpoints[i];
                if (i + 2 == plan->ncheckpoints) {
                    break;
                }
            }
        }
    }
    clear_cells(gl, p, false);
    pc_document snapshot = {0};
    pc_status rc = pc_document_clone(doc, &snapshot, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (checkpoint) {
        for (size_t i = checkpoint->layer_end; i < snapshot.nlayers; i++) {
            pc_layer_fini(&snapshot.layers[i]);
        }
        snapshot.nlayers = checkpoint->layer_end;
    }
    if (p->has_snapshot) {
        pc_document_fini(&p->snapshot);
    }
    p->snapshot = snapshot;
    p->has_snapshot = true;
    p->gamma = gamma;
    p->active = checkpoint != NULL;
    if (checkpoint) {
        p->checkpoint = *checkpoint;
    }
    return PC_OK;
}
static pc_gpu_prefix_cell *find(pc_gpu_prefix *p, pc_rect area)
{
    pc_rect cell = pc_gpu_prefix_cell_of(area);
    for (size_t i = 0; i < p->count; i++) {
        if (pc_rect_eq(cell, p->cells[i].cell)) {
            return &p->cells[i];
        }
    }
    return NULL;
}
bool pc_gpu_prefix_copy(const gl33 *gl, pc_gpu_prefix *p, GLuint framebuffer, pc_rect area, GLuint dst)
{
    pc_gpu_prefix_cell *cell = p->active ? find(p, area) : NULL;
    if (!pc_gpu_prefix_covered(cell, area)) {
        return false;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cell->texture, 0);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, dst);
    gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, area.x0 - cell->cell.x0, area.y0 - cell->cell.y0,
                            (int)pc_rect_width(area), (int)pc_rect_height(area));
    p->hits++;
    return true;
}
void pc_gpu_prefix_capture(const gl33 *gl, pc_gpu_prefix *p, GLuint framebuffer, pc_rect area, GLuint src)
{
    if (!p->active || pc_rect_is_empty(area)) {
        return;
    }
    pc_gpu_prefix_cell *cell = find(p, area);
    if (!cell) {
        if (p->count >= pc_gpu_prefix_budget(p) / (UINT64_C(256) * 256 * 16)) {
            return;
        }
        cell = &p->cells[p->count++];
        *cell = (pc_gpu_prefix_cell){.cell = pc_gpu_prefix_cell_of(area)};
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glGenTextures(1, &cell->texture);
        gl->glBindTexture(GL_TEXTURE_2D, cell->texture);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 256, 256, 0, GL_RGBA, GL_FLOAT, NULL);
    }
    if (!contains(cell->cell, area)) {
        return;
    }
    gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, 0);
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, cell->texture);
    gl->glCopyTexSubImage2D(GL_TEXTURE_2D, 0, area.x0 - cell->cell.x0, area.y0 - cell->cell.y0, 0, 0,
                            (int)pc_rect_width(area), (int)pc_rect_height(area));
    pc_gpu_prefix_cover(cell, area);
}
