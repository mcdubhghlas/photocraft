/* channel_view.rs: channel-only pictures and alpha/spot/Quick Mask/layer-mask rubyliths.
 * Samples the canvas cache's padded source region; view changes never edit document pixels. */
#include "channel_view.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "base/mem.h"
#include "engine/channel_display.h"
#include "raster/pixel.h"
#include "base/num.h"

void pc_ui_channel_cache_fini(pc_ui_channel_cache *c)
{
    if (c) {
        pc_compose_buffer_fini(&c->image);
        *c = (pc_ui_channel_cache){0};
    }
}
static float coverage(const pc_surface *surface, int32_t x, int32_t y)
{
    float v = pc_surface_sample_channel(surface, x, y, 0);
    return isfinite(v) ? fminf(1, fmaxf(0, v)) : 0;
}
static void gray(float out[4], float v)
{
    out[0] = out[1] = out[2] = v;
    out[3] = 1;
}
static void tint(float out[4], const float color[3], float alpha)
{
    alpha = fminf(1, fmaxf(0, alpha));
    for (size_t ch = 0; ch < 3; ch++) {
        out[ch] = color[ch] * alpha + out[ch] * (1 - alpha);
    }
    out[3] = alpha + out[3] * (1 - alpha);
}
pc_status pc_ui_channel_cache_update_overlay(pc_ui_channel_cache *c, const pc_engine_doc_state *st,
                                             const pc_ui_canvas_cache *source, bool in_color,
                                             const pc_ui_channel_overlay *overlay, pc_error *err)
{
    if (!c || !st || !st->doc || !source || !source->valid) {
        return pc_fail(err, PC_ERR_INVALID, "missing channel-view source");
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    if (source->document != doc->id) {
        return pc_fail(err, PC_ERR_INVALID, "channel-view document mismatch");
    }
    bool extra = overlay && overlay->active;
    if (extra && (!isfinite(overlay->opacity) || overlay->opacity < 0 || overlay->opacity > 1)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid overlay opacity");
    }
    for (size_t i = 0; extra && i < 3; i++) {
        if (!isfinite(overlay->color[i]) || overlay->color[i] < 0 || overlay->color[i] > 1) {
            return pc_fail(err, PC_ERR_INVALID, "invalid overlay color");
        }
    }
    if (extra && overlay->surface && !pc_surface_format(overlay->surface).alpha) {
        return pc_fail(err, PC_ERR_INVALID, "text overlay needs an alpha surface");
    }
    bool same_extra = c->extra_active == extra &&
                      (!extra || (c->extra_key == overlay->key && c->extra_opacity == overlay->opacity &&
                                  !memcmp(c->extra_color, overlay->color, sizeof c->extra_color)));
    uint64_t key = pc_engine_channel_display_key(st);
    if (c->valid && c->document == doc->id && c->key == key && c->source_generation == source->generation &&
        c->in_color == in_color && same_extra) {
        return PC_OK;
    }
    size_t colors = pc_engine_color_count(doc), visible = pc_engine_channel_visible_colors(st, colors);
    const pc_layer *mask_layer =
        st->has_layer_mask_view ? pc_document_layer(doc, st->layer_mask_view_layer) : NULL;
    if (mask_layer && !mask_layer->has_mask) {
        mask_layer = NULL;
    }
    bool quick = doc->has_quick_mask && !pc_engine_channel_quick_mask_hidden(st);
    size_t first = SIZE_MAX, alpha_count = 0;
    for (size_t i = 0; i < doc->nchannels; i++) {
        if (pc_engine_channel_alpha_shown(st, i)) {
            if (first == SIZE_MAX) {
                first = i;
            }
            alpha_count++;
        }
    }
    bool active = visible < colors || alpha_count || quick || mask_layer || extra;
    const pc_surface *solo = NULL;
    if (visible == 0) {
        if (first != SIZE_MAX) {
            solo = &doc->channels[first].surface;
        } else if (quick) {
            solo = &doc->quick_mask.surface;
        } else if (mask_layer) {
            solo = &mask_layer->mask.surface;
        } else if (st->has_active_layer) {
            const pc_layer *layer = pc_document_layer(doc, st->active_layer);
            if (layer && layer->has_mask) {
                solo = &layer->mask.surface;
            }
        }
        if (!solo && !extra) {
            active = false;
        }
    }
    bool gray_mask = mask_layer && st->layer_mask_view_mode == PC_ENGINE_MASK_VIEW_GRAY;
    size_t *indices = alpha_count ? pc_malloc_array(alpha_count, sizeof *indices) : NULL;
    if (alpha_count && !indices) {
        return pc_fail(err, PC_ERR_NOMEM, "channel overlays");
    }
    size_t count = 0;
    for (size_t i = 0; indices && i < doc->nchannels && count < alpha_count; i++) {
        if (pc_engine_channel_alpha_shown(st, i)) {
            indices[count++] = i;
        }
    }
    pc_compose_buffer image = {0};
    pc_status rc = PC_OK;
    if (active) {
        uint32_t w = pc_rect_width(source->image.rect), h = pc_rect_height(source->image.rect);
        rc = pc_compose_buffer_transparent(pc_rect_make(0, 0, (int32_t)w, (int32_t)h), &image, err);
        if (rc != PC_OK) {
            free(indices);
            return rc;
        }
        if (alpha_count && image.len > ((size_t)1 << 30) / alpha_count) {
            free(indices);
            pc_compose_buffer_fini(&image);
            return pc_fail(err, PC_ERR_LIMIT, "too many channel overlay samples");
        }
        pc_pixel_format format = pc_document_pixel_format(doc);
        for (uint32_t y = 0; y < h; y++) {
            int32_t sy =
                pc_f64_to_i32(source->source_rect.y0 + (double)y * pc_rect_height(source->source_rect) / h);
            for (uint32_t x = 0; x < w; x++) {
                int32_t sx = pc_f64_to_i32(source->source_rect.x0 +
                                           (double)x * pc_rect_width(source->source_rect) / w);
                float *p = image.px[(size_t)y * w + x];
                if (gray_mask) {
                    gray(p, coverage(&mask_layer->mask.surface, sx, sy));
                } else {
                    if (solo) {
                        gray(p, coverage(solo, sx, sy));
                    } else if (visible < colors) {
                        const float *base = source->image.px[(size_t)y * w + x];
                        float rgba[4] = {base[0] + 1 - base[3], base[1] + 1 - base[3], base[2] + 1 - base[3],
                                         1};
                        float native[PC_RASTER_MAX_CHANNELS] = {0};
                        (void)pc_from_rgba(format, rgba, native);
                        size_t shown = 0;
                        for (size_t ch = 0; ch < colors; ch++) {
                            if (pc_engine_channel_color_shown(st, ch)) {
                                shown = ch;
                            }
                        }
                        if (visible == 1 && !in_color) {
                            gray(p, doc->mode == PC_COLOR_MODE_CMYK ? 1 - native[shown] : native[shown]);
                        } else {
                            for (size_t ch = 0; ch < colors; ch++) {
                                if (!pc_engine_channel_color_shown(st, ch)) {
                                    native[ch] =
                                        doc->mode == PC_COLOR_MODE_LAB && (ch > 0 || in_color) ? .5f : 0;
                                }
                            }
                            pc_to_rgba(format, native, pc_pixel_format_channels(format), p);
                            p[3] = 1;
                        }
                    }
                    for (size_t j = 0; j < alpha_count; j++) {
                        size_t i = indices[j];
                        if (solo && i == first) {
                            continue;
                        }
                        const pc_alpha_channel *channel = &doc->channels[i];
                        float color[3];
                        pc_color_to_rgb(channel->has_spot ? &channel->spot_color : &channel->color, color);
                        float opacity =
                            channel->has_spot ? .5f + .5f * channel->spot_solidity : channel->opacity;
                        tint(p, color,
                             pc_alpha_channel_overlay_coverage(channel, coverage(&channel->surface, sx, sy)) *
                                 opacity);
                    }
                    if (quick && solo != &doc->quick_mask.surface) {
                        const pc_alpha_channel *q = &doc->quick_mask;
                        float color[3];
                        pc_color_to_rgb(&q->color, color);
                        tint(p, color,
                             pc_alpha_channel_overlay_coverage(q, coverage(&q->surface, sx, sy)) *
                                 q->opacity);
                    }
                    if (mask_layer && solo != &mask_layer->mask.surface) {
                        const float red[3] = {1, 0, 0};
                        tint(p, red, .5f * (1 - coverage(&mask_layer->mask.surface, sx, sy)));
                    }
                }
                if (extra) {
                    float alpha = 0;
                    if (overlay->surface) {
                        size_t channels = pc_pixel_format_channels(pc_surface_format(overlay->surface));
                        if (channels) {
                            alpha = pc_surface_sample_channel(overlay->surface, sx, sy, channels - 1);
                        }
                    }
                    alpha = isfinite(alpha) ? fminf(1, fmaxf(0, alpha)) : 0;
                    tint(p, overlay->color, (1 - alpha) * overlay->opacity);
                }
                for (size_t ch = 0; ch < 4; ch++) {
                    p[ch] = floorf(fminf(1, fmaxf(0, p[ch])) * 255.f + .5f) / 255.f;
                }
            }
        }
    }
    free(indices);
    pc_compose_buffer_fini(&c->image);
    c->image = image;
    c->document = doc->id;
    c->extra_active = extra;
    c->extra_key = extra ? overlay->key : 0;
    if (extra) {
        memcpy(c->extra_color, overlay->color, sizeof c->extra_color);
        c->extra_opacity = overlay->opacity;
    }
    c->key = key;
    c->source_generation = source->generation;
    c->generation++;
    c->valid = true;
    c->active = active;
    c->in_color = in_color;
    return PC_OK;
}

pc_status pc_ui_channel_cache_update(pc_ui_channel_cache *cache, const pc_engine_doc_state *state,
                                     const pc_ui_canvas_cache *source, bool in_color, pc_error *err)
{
    return pc_ui_channel_cache_update_overlay(cache, state, source, in_color, NULL, err);
}
