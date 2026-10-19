#include "ui/layer_thumbnail.h"
#include "compose/compose.h"
#include "color/convert.h"
#include "vector/vector.h"
#include "base/num.h"
#include <math.h>
#include <stdlib.h>

static uint8_t byte(float value)
{
    return pc_f32_to_u8((isfinite(value) ? fminf(1, fmaxf(0, value)) : 0) * 255 + .5f);
}
pc_status pc_ui_layer_thumbnail(const pc_document *doc, const pc_layer *layer,
                                pc_ui_layer_thumbnail_kind kind, uint32_t side, pc_rgba8_image *out,
                                pc_error *err)
{
    if (!doc || !layer || !out || !side || side > 128 || kind > PC_UI_THUMB_VECTOR_MASK ||
        kind < PC_UI_THUMB_LAYER || doc->size.width > INT32_MAX || doc->size.height > INT32_MAX)
        return pc_fail(err, PC_ERR_INVALID, "invalid layer thumbnail");
    if ((kind == PC_UI_THUMB_PIXEL_MASK && !layer->has_mask) ||
        (kind == PC_UI_THUMB_VECTOR_MASK && !layer->has_vector_mask))
        return pc_fail(err, PC_ERR_NOT_FOUND, "layer has no requested mask");
    pc_rgba8_image image = {0};
    pc_status status = pc_rgba8_image_init(&image, side, side, err);
    if (status != PC_OK)
        return status;
    double w = doc->size.width ? doc->size.width : 1, h = doc->size.height ? doc->size.height : 1,
           scale = fmax(w, h) / side, ox = (side - w / scale) / 2, oy = (side - h / scale) / 2;
    float *mask = NULL;
    size_t count = 0;
    if (kind == PC_UI_THUMB_VECTOR_MASK) {
        pc_path path = {0};
        pc_affine transform = {{1 / scale, 0, 0, 1 / scale, ox, oy}};
        status = pc_path_transform(&layer->vector_mask.path, transform, &path, err);
        if (status == PC_OK) {
            pc_vector_mask preview = layer->vector_mask;
            preview.path = path;
            preview.enabled = true;
            status = pc_vec_mask_values(&preview, pc_rect_make(0, 0, (int32_t)side, (int32_t)side), &mask,
                                        &count, err);
        }
        pc_path_fini(&path);
        if (status != PC_OK) {
            pc_rgba8_image_fini(&image);
            return status;
        }
    }
    pc_cmyk_space *space = pc_compose_cmyk_space(doc), *previous = pc_cmyk_space_enter(space);
    const pc_surface *surface =
        kind == PC_UI_THUMB_PIXEL_MASK ? &layer->mask.surface : pc_layer_surface(layer);
    for (uint32_t y = 0; y < side; y++) {
        for (uint32_t x = 0; x < side; x++) {
            double dx = ((double)x - ox + .5) * scale, dy = ((double)y - oy + .5) * scale;
            if (dx < 0 || dy < 0 || dx >= w || dy >= h)
                continue;
            size_t i = (size_t)y * side + x;
            float rgba[4] = {0};
            if (kind == PC_UI_THUMB_VECTOR_MASK) {
                float value = i < count ? mask[i] : 1;
                rgba[0] = rgba[1] = rgba[2] = (.5f + .5f * value);
                rgba[3] = 1;
            } else if (kind == PC_UI_THUMB_PIXEL_MASK) {
                rgba[0] = rgba[1] = rgba[2] =
                    pc_surface_sample_channel(surface, pc_f64_to_i32(dx), pc_f64_to_i32(dy), 0);
                rgba[3] = 1;
            } else if (surface)
                pc_surface_rgba(surface, pc_f64_to_i32(dx), pc_f64_to_i32(dy), rgba);
            for (size_t ch = 0; ch < 4; ch++)
                image.pixels[i * 4 + ch] = byte(rgba[ch]);
        }
    }
    pc_cmyk_space_leave(previous);
    pc_cmyk_space_release(space);
    free(mask);
    pc_rgba8_image_fini(out);
    *out = image;
    return PC_OK;
}
