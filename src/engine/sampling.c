#include "sampling.h"
#include "engine_internal.h"
#include "compose/compose.h"
#include "raster/pixel.h"
#include "base/mem.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
bool pc_engine_sample_size_valid(uint32_t size)
{
    const uint32_t sizes[] = {1, 3, 5, 11, 31, 51, 101};
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++)
        if (size == sizes[i])
            return true;
    return false;
}
bool pc_engine_sample_layers_parse(const char *name, pc_engine_sample_layers *out)
{
    const char *names[] = {"current", "currentAndBelow", "all", "allNoAdjustments",
                           "currentAndBelowNoAdjustments"};
    for (size_t i = 0; name && i < sizeof names / sizeof *names; i++)
        if (!strcmp(name, names[i])) {
            if (out)
                *out = (pc_engine_sample_layers)i;
            return true;
        }
    return false;
}
pc_status pc_engine_sample_size(yyjson_val *params, uint32_t *out, pc_error *err)
{
    if (!out)
        return pc_fail(err, PC_ERR_INVALID, "missing sample size output");
    yyjson_val *value = pc_json_get(params, "size");
    if (!value || yyjson_is_null(value) ||
        (yyjson_is_str(value) && !strcmp(yyjson_get_str(value), "point"))) {
        *out = 1;
        return PC_OK;
    }
    uint64_t size = 0;
    if (!pc_json_as_u64(value, &size) || size > UINT32_MAX || !pc_engine_sample_size_valid((uint32_t)size))
        return pc_fail(err, PC_ERR_INVALID, "size must be point or 1, 3, 5, 11, 31, 51, 101");
    *out = (uint32_t)size;
    return PC_OK;
}
static float quantize(float value, pc_sample_type depth)
{
    uint8_t bytes[4] = {0};
    pc_write_sample(bytes, sizeof bytes, depth, 0, value);
    return pc_read_sample(bytes, sizeof bytes, depth, 0);
}
void pc_engine_sample_average(const float (*pixels)[4], size_t count, pc_color_mode mode,
                              pc_sample_type depth, float out[4])
{
    memset(out, 0, 4 * sizeof *out);
    if (!pixels || !count)
        return;
    pc_pixel_format format = {mode, depth, false};
    size_t channels = pc_pixel_format_channels(format);
    if (!channels || channels > PC_RASTER_MAX_CHANNELS)
        return;
    double sum[PC_RASTER_MAX_CHANNELS] = {0}, alpha = 0;
    for (size_t i = 0; i < count; i++) {
        const float *p = pixels[i];
        float a = isfinite(p[3]) ? fminf(1, fmaxf(0, p[3])) : 0;
        if (a <= 0 || !isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2]))
            continue;
        float native[PC_RASTER_MAX_CHANNELS] = {0};
        size_t written = pc_from_rgba(format, p, native);
        for (size_t c = 0; c < written && c < channels; c++)
            sum[c] += (double)native[c] * a;
        alpha += a;
    }
    if (alpha <= 0)
        return;
    float native[PC_RASTER_MAX_CHANNELS] = {0};
    for (size_t c = 0; c < channels; c++)
        native[c] = quantize((float)(sum[c] / alpha), depth);
    pc_to_rgba(format, native, channels, out);
    out[3] = quantize((float)(alpha / (double)count), depth);
}
static pc_status filter_layers(pc_layer *layers, size_t count, pc_layer_id active, bool below,
                               bool no_adjustments, bool *found, size_t depth, pc_error *err)
{
    if (depth > 100)
        return pc_fail(err, PC_ERR_LIMIT, "sample layer nesting exceeds 100");
    for (size_t i = 0; i < count; i++) {
        pc_layer *layer = &layers[i];
        bool own = below && layer->id == active;
        if (below && *found)
            layer->visible = false;
        if (no_adjustments && layer->content.kind == PC_LAYER_ADJUSTMENT)
            layer->visible = false;
        if (own)
            *found = true;
        /* A selected group includes all its children, which the walk lists after the group. */
        if (layer->content.kind == PC_LAYER_GROUP) {
            bool child_found = *found;
            pc_status status =
                filter_layers(layer->content.u.group.children, layer->content.u.group.nchildren, active,
                              below && !own, no_adjustments, &child_found, depth + 1, err);
            if (status != PC_OK)
                return status;
            if (!own)
                *found = child_found;
        }
    }
    return PC_OK;
}
pc_status pc_engine_sample_color(const pc_document *source, bool has_active, pc_layer_id active, double x,
                                 double y, uint32_t size, pc_engine_sample_layers which, float out[4],
                                 pc_error *err)
{
    if (!source || !out || (unsigned)which > PC_SAMPLE_CURRENT_AND_BELOW_NO_ADJUSTMENTS ||
        !pc_engine_sample_size_valid(size))
        return pc_fail(err, PC_ERR_INVALID, "invalid color sample context");
    memset(out, 0, 4 * sizeof *out);
    if (!isfinite(x) || !isfinite(y) || x < 0 || y < 0 || x >= source->size.width || y >= source->size.height)
        return PC_OK;
    int64_t px = (int64_t)floor(x), py = (int64_t)floor(y), radius = size / 2;
    int64_t x0 = px - radius, y0 = py - radius, x1 = px + radius + 1, y1 = py + radius + 1;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > source->size.width)
        x1 = source->size.width;
    if (y1 > source->size.height)
        y1 = source->size.height;
    if (x0 > INT32_MAX || y0 > INT32_MAX || x1 > INT32_MAX || y1 > INT32_MAX)
        return pc_fail(err, PC_ERR_LIMIT, "sample coordinates exceed compositor range");
    bool below = which == PC_SAMPLE_CURRENT_AND_BELOW || which == PC_SAMPLE_CURRENT_AND_BELOW_NO_ADJUSTMENTS;
    bool omit = which == PC_SAMPLE_ALL_NO_ADJUSTMENTS || which == PC_SAMPLE_CURRENT_AND_BELOW_NO_ADJUSTMENTS;
    if ((which == PC_SAMPLE_CURRENT || below) && (!has_active || !pc_document_layer(source, active)))
        return PC_OK;
    pc_document copy = {0};
    const pc_document *doc = source;
    pc_status status = PC_OK;
    if (which != PC_SAMPLE_ALL) {
        status = pc_document_clone(source, &copy, err);
        if (status != PC_OK)
            return status;
        doc = &copy;
        if (which == PC_SAMPLE_CURRENT) {
            pc_layer *solo = pc_malloc_array(1, sizeof *solo);
            if (!solo)
                status = pc_fail(err, PC_ERR_NOMEM, "sample current layer");
            if (status == PC_OK)
                status = pc_layer_clone(pc_document_layer(source, active), solo, err);
            if (status != PC_OK)
                free(solo);
            else {
                for (size_t i = 0; i < copy.nlayers; i++)
                    pc_layer_fini(&copy.layers[i]);
                free(copy.layers);
                copy.layers = solo;
                copy.nlayers = 1;
                solo->visible = true;
                solo->clipped = false;
                solo->opacity = solo->fill_opacity = 1;
                solo->blend = PC_BLEND_NORMAL;
            }
        } else {
            bool found = false;
            status = filter_layers(copy.layers, copy.nlayers, active, below, omit, &found, 0, err);
        }
    }
    pc_compose_buffer buffer = {0};
    if (status == PC_OK)
        status = pc_compose_render(doc, pc_rect_make((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1),
                                   &buffer, err);
    if (status == PC_OK && size == 1 && buffer.len)
        memcpy(out, buffer.px[0], 4 * sizeof *out);
    else if (status == PC_OK)
        pc_engine_sample_average((const float (*)[4])buffer.px, buffer.len,
                                 pc_document_pixel_format(doc).mode == PC_COLOR_MODE_CMYK
                                     ? PC_COLOR_MODE_RGB
                                     : pc_document_pixel_format(doc).mode,
                                 doc->depth, out);
    pc_compose_buffer_fini(&buffer);
    if (doc == &copy)
        pc_document_fini(&copy);
    return status;
}
static pc_status run(pc_engine_session *session, yyjson_val *params, pc_json_value *out, pc_error *err)
{
    double x, y;
    if (!pc_json_as_f64(pc_json_get(params, "x"), &x) || !isfinite(x) ||
        !pc_json_as_f64(pc_json_get(params, "y"), &y) || !isfinite(y))
        return pc_fail(err, PC_ERR_INVALID, "x and y must be finite document coordinates");
    uint32_t size = 1;
    pc_status status = pc_engine_sample_size(params, &size, err);
    pc_engine_sample_layers which = PC_SAMPLE_ALL;
    yyjson_val *layers = pc_json_get(params, "sampleLayer");
    if (status == PC_OK && layers && !yyjson_is_null(layers) &&
        (!yyjson_is_str(layers) || !pc_engine_sample_layers_parse(yyjson_get_str(layers), &which)))
        status = pc_fail(err, PC_ERR_INVALID, "invalid sampleLayer choice");
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    float color[4] = {0};
    if (status == PC_OK)
        status = pc_engine_sample_color(pc_doc_arc_get(state->doc), state->has_active_layer,
                                        state->active_layer, x, y, size, which, color, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_doc *d = NULL;
    status = pc_engine_result_doc(out, &d, err);
    if (status != PC_OK)
        return status;
    yyjson_mut_val *a = yyjson_mut_arr(d);
    bool ok = a != NULL;
    for (size_t i = 0; ok && i < 4; i++)
        ok = yyjson_mut_arr_add_real(d, a, isfinite(color[i]) ? color[i] : 0);
    if (!ok) {
        pc_json_value_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "color sample result");
    }
    yyjson_mut_doc_set_root(d, a);
    return PC_OK;
}
const pc_engine_command *pc_engine_sampling_specs(size_t *count)
{
    static const pc_engine_command specs[] = {
        {"document.sampleColor", "Sample Color", NULL, 0, NULL,
         "{\"x\":f64,\"y\":f64,\"size\":1|3|5|11|31|51|101=1,\"sampleLayer\":\"current|currentAndBelow|all|"
         "allNoAdjustments|currentAndBelowNoAdjustments\"=\"all\"}",
         pc_engine_has_doc, run, false}};
    *count = sizeof specs / sizeof *specs;
    return specs;
}
