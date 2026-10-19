#include "gpu/fx_resources.h"
#include "base/mem.h"
#include "compose/effects.h"
#include <stdlib.h>
const pc_gpu_fx_output *pc_gpu_fx_resources_find(const pc_gpu_fx_resources *r, const pc_layer *layer,
                                                 const pc_effect *effect)
{
    for (size_t i = 0; i < r->count; i++) {
        if (r->items[i].layer == layer && r->items[i].effect == effect) {
            return &r->items[i].output;
        }
    }
    return NULL;
}
void pc_gpu_fx_resources_fini(pc_gpu_fx_context *ctx, pc_gpu_fx_resources *r)
{
    if (!r) {
        return;
    }
    for (size_t i = 0; i < r->count; i++) {
        pc_gpu_fx_output_fini(ctx, &r->items[i].output);
    }
    free(r->items);
    *r = (pc_gpu_fx_resources){0};
}
pc_status pc_gpu_fx_resources_build(const gl33 *gl, pc_gpu_fx_context *ctx, const pc_document *doc,
                                    const pc_gpu_plan *plan, pc_rect area, pc_gpu_fx_resources *out,
                                    pc_error *err)
{
    pc_gpu_fx_resources next = {0};
    pc_status rc = PC_OK;
    const pc_layer *shape_layer = NULL;
    float *shape = NULL;
    size_t length = 0;
    for (size_t i = 0; i < plan->count && rc == PC_OK; i++) {
        const pc_gpu_pass *pass = &plan->passes[i];
        if (!pass->fx_layer || (pass->kernel == PC_GPU_FX_INIT && pass->fx_kind != 7)) {
            continue;
        }
        if (pass->kernel != PC_GPU_FX_INIT && (pass->map_index < 0 || !pass->effect)) {
            continue;
        }
        const pc_layer *layer = pass->fx_layer;
        const pc_effect *effect = pass->effect;
        if (pc_gpu_fx_resources_find(&next, layer, effect)) {
            continue;
        }
        if (next.count >= 1024) {
            rc = pc_fail(err, PC_ERR_LIMIT, "GPU effect resource count");
            break;
        }
        pc_rect region = pc_gpu_effect_region(layer, pc_document_bounds(doc));
        if (pc_compose_fx_maps_are_local(layer)) {
            pc_rect local =
                pc_rect_intersect(region, pc_rect_inflate(area, 2 * pc_compose_effects_margin(layer)));
            if (!pc_rect_is_empty(local))
                region = local;
        }
        uint64_t pixels = (uint64_t)pc_rect_width(region) * pc_rect_height(region);
        if (pixels > UINT64_C(33554432)) {
            rc = pc_fail(err, PC_ERR_LIMIT, "GPU effect shape budget");
            break;
        }
        if (shape_layer != layer) {
            free(shape);
            shape = NULL;
            rc = pc_compose_layer_shape(doc, layer, region, &shape, &length, err);
            if (rc != PC_OK) {
                break;
            }
            shape_layer = layer;
        }
        pc_gpu_fx_resource entry = {.layer = layer, .effect = effect};
        if (!effect) {
            GLint maximum = 0;
            gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
            if (!pixels || pc_rect_width(region) > (uint32_t)maximum ||
                pc_rect_height(region) > (uint32_t)maximum) {
                rc = pc_fail(err, PC_ERR_LIMIT, "GPU outline texture size");
                break;
            }
            entry.output = (pc_gpu_fx_output){.count = 1, .region = region, .valid = region};
            gl->glActiveTexture(GL_TEXTURE0);
            gl->glGenTextures(1, entry.output.textures);
            gl->glBindTexture(GL_TEXTURE_2D, entry.output.textures[0]);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, (int)pc_rect_width(region),
                             (int)pc_rect_height(region), 0, GL_RED, GL_FLOAT, shape);
            if (gl->glGetError() != GL_NO_ERROR) {
                rc = pc_fail(err, PC_ERR_IO, "GPU outline upload");
            }
        } else {
            pc_rect frame;
            if (!pc_compose_paint_bounds(layer, &frame)) {
                frame = pc_compose_layer_bounds(layer, pc_document_bounds(doc));
            }
            double anchor[2] = {(double)frame.x0, (double)frame.y0};
            if (layer->effects.has_reference) {
                anchor[0] = layer->effects.reference[0];
                anchor[1] = layer->effects.reference[1];
            }
            pc_gpu_fx_program program = {0};
            bool vector = layer->content.kind == PC_LAYER_SHAPE && !pc_compose_effect_outline(layer);
            rc = pc_gpu_fx_program_build(effect, doc->global_light, vector, doc->patterns, doc->npatterns,
                                         anchor, &program, err);
            if (rc == PC_OK) {
                rc = pc_gpu_fx_render_window(ctx, &program, region, area, shape, length, doc->mode,
                                             &entry.output, err);
            }
            pc_gpu_fx_program_fini(&program);
        }
        size_t bytes = (size_t)pixels * 4 * entry.output.count;
        if (rc == PC_OK && bytes > ((size_t)128 << 20) - next.bytes) {
            rc = pc_fail(err, PC_ERR_LIMIT, "GPU effect residency budget");
        }
        pc_gpu_fx_resource *grown =
            rc == PC_OK ? pc_grow_array(next.items, &next.capacity, next.count + 1, sizeof *grown) : NULL;
        if (rc == PC_OK && !grown) {
            rc = pc_fail(err, PC_ERR_NOMEM, "GPU effect resources");
        }
        if (rc != PC_OK) {
            pc_gpu_fx_output_fini(ctx, &entry.output);
            break;
        }
        next.items = grown;
        next.items[next.count++] = entry;
        next.bytes += bytes;
    }
    free(shape);
    if (rc != PC_OK) {
        pc_gpu_fx_resources_fini(ctx, &next);
        return rc;
    }
    pc_gpu_fx_resources_fini(ctx, out);
    *out = next;
    return PC_OK;
}
