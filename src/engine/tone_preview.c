#include "engine/tone_preview.h"
#include "engine/channel_display.h"
#include "compose/compose.h"
#include "compose/adjust.h"
#include "color/convert.h"
#include <math.h>
#include <string.h>
static pc_status isolate(pc_layer *layers, size_t count, pc_layer_id target, size_t depth, bool *found,
                         pc_error *err)
{
    if (depth > PC_MAX_GROUP_DEPTH)
        return pc_fail(err, PC_ERR_LIMIT, "histogram layer nesting too deep");
    bool any = false;
    for (size_t i = 0; i < count; i++) {
        pc_layer *layer = &layers[i];
        bool here = layer->id == target;
        if (!here && layer->content.kind == PC_LAYER_GROUP) {
            pc_status status = isolate(layer->content.u.group.children, layer->content.u.group.nchildren,
                                       target, depth + 1, &here, err);
            if (status != PC_OK)
                return status;
        }
        layer->visible = here;
        any = any || here;
    }
    *found = any;
    return PC_OK;
}
static size_t bin(float value)
{
    return (size_t)lroundf(isfinite(value) ? fminf(1, fmaxf(0, value)) * 255 : 0);
}
static void add(pc_engine_tone_histogram *h, const uint8_t p[4])
{
    if (!p[3])
        return;
    float rgb[3] = {(float)p[0] / 255, (float)p[1] / 255, (float)p[2] / 255};
    if (h->mode == PC_COLOR_MODE_CMYK) {
        float inks[4];
        pc_rgb_to_cmyk(rgb, inks);
        float sum = 0;
        for (size_t c = 0; c < 4; c++) {
            h->bins[c + 1][bin(1 - inks[c])]++;
            sum += inks[c];
        }
        h->bins[0][bin(1 - sum / 4)]++;
    } else if (h->mode == PC_COLOR_MODE_LAB) {
        float lab[3];
        pc_srgb_to_lab(rgb, lab);
        h->bins[0][bin(lab[0] / 100)]++;
        h->bins[1][bin((lab[1] + 128) / 255)]++;
        h->bins[2][bin((lab[2] + 128) / 255)]++;
    } else {
        for (size_t c = 0; c < 3; c++)
            h->bins[c + 1][p[c]]++;
        h->bins[0][((size_t)p[0] + p[1] + p[2]) / 3]++;
    }
}
pc_status pc_engine_tone_histogram_compute(const pc_engine_session *session, bool below,
                                           pc_engine_tone_histogram *out, pc_error *err)
{
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state)
        return pc_fail(err, PC_ERR_INVALID, "histogram needs a document");
    const pc_document *source = pc_doc_arc_get(state->doc);
    pc_engine_tone_histogram h = {.mode = source->mode};
    if (source->size.width > INT32_MAX || source->size.height > INT32_MAX)
        return pc_fail(err, PC_ERR_LIMIT, "histogram extent too large");
    const pc_layer *layer = state->has_active_layer ? pc_document_layer(source, state->active_layer) : NULL;
    bool quick = false;
    size_t index = 0;
    const pc_surface *surface = NULL;
    if (pc_engine_targeted_channel(session, &quick, &index)) {
        if (quick && source->has_quick_mask)
            surface = &source->quick_mask.surface;
        else if (!quick && index < source->nchannels)
            surface = &source->channels[index].surface;
    } else if (pc_engine_targeted_mask(session) && layer && layer->has_mask)
        surface = &layer->mask.surface;
    if (surface) {
        h.grayscale = true;
        h.mode = PC_COLOR_MODE_GRAYSCALE;
        h.channels = 1;
        uint64_t step = source->size.width > source->size.height ? source->size.width : source->size.height;
        step = step > 384 ? (step + 383) / 384 : 1;
        float value[PC_RASTER_MAX_CHANNELS];
        for (uint64_t y = 0; y < source->size.height; y += step)
            for (uint64_t x = 0; x < source->size.width; x += step) {
                pc_surface_read_pixel(surface, (int32_t)x, (int32_t)y, value);
                h.bins[0][bin(value[0])]++;
            }
        *out = h;
        return PC_OK;
    }
    h.grayscale = source->mode == PC_COLOR_MODE_GRAYSCALE;
    h.channels = source->mode == PC_COLOR_MODE_CMYK  ? 5
                 : source->mode == PC_COLOR_MODE_LAB ? 3
                 : h.grayscale                       ? 1
                                                     : 4;
    pc_document doc = {0};
    pc_status status = pc_document_clone(source, &doc, err);
    if (status != PC_OK)
        return status;
    if (below) {
        pc_layer *hidden = pc_document_layer_mut(&doc, state->active_layer);
        if (hidden)
            hidden->visible = false;
    } else {
        bool found = false;
        status = isolate(doc.layers, doc.nlayers, state->active_layer, 0, &found, err);
        if (status == PC_OK && !found)
            status = pc_fail(err, PC_ERR_NOT_FOUND, "histogram source layer was removed");
    }
    pc_rgba8_image image = {0};
    if (status == PC_OK)
        status = pc_compose_thumbnail(&doc, 384, &image, err);
    if (status == PC_OK)
        for (size_t i = 0; i < (size_t)image.width * image.height; i++)
            add(&h, image.pixels + i * 4);
    pc_rgba8_image_fini(&image);
    pc_document_fini(&doc);
    if (status == PC_OK)
        *out = h;
    return status;
}
pc_status pc_engine_tone_curve_sample(const double (*points)[2], size_t count, float out[256], pc_error *err)
{
    if (!points || count < 2 || count > 19 || !out)
        return pc_fail(err, PC_ERR_INVALID, "invalid point curve");
    pc_curve_point p[19];
    for (size_t i = 0; i < count; i++) {
        if (!isfinite(points[i][0]) || !isfinite(points[i][1]) || points[i][0] < 0 || points[i][0] > 255 ||
            points[i][1] < 0 || points[i][1] > 255 || (i && points[i][0] <= points[i - 1][0]))
            return pc_fail(err, PC_ERR_INVALID, "invalid curve point");
        p[i] = (pc_curve_point){(float)(points[i][0] / 255.), (float)(points[i][1] / 255.)};
    }
    float lut[PC_COMPOSE_LUT_SIZE];
    pc_status status = pc_compose_curve_lut(p, count, lut, err);
    if (status == PC_OK)
        for (size_t i = 0; i < 256; i++) {
            double x = (double)i * (PC_COMPOSE_LUT_SIZE - 1) / 255.;
            size_t a = (size_t)x, b = a + 1 < PC_COMPOSE_LUT_SIZE ? a + 1 : a;
            out[i] = lut[a] + (lut[b] - lut[a]) * (float)(x - (double)a);
        }
    return status;
}
