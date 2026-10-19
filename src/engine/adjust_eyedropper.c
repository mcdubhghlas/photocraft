#include "adjust_eyedropper.h"
#include "adjust_params.h"
#include "sampling.h"
#include "compose/adjust.h"
#include "base/mem.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define MAX_POINTS 19
#define DUPLICATE_DISTANCE (.5f / 255.f)
static float clamp(float v)
{
    return fminf(1, fmaxf(0, v));
}
bool pc_engine_tone_picker_supported(pc_color_mode mode, pc_engine_tone_picker picker)
{
    return (unsigned)picker <= PC_TONE_PICK_WHITE &&
           (mode == PC_COLOR_MODE_RGB || (mode == PC_COLOR_MODE_GRAYSCALE && picker != PC_TONE_PICK_GRAY));
}
static float serialized(float value)
{
    return roundf(value * 255 * 100) / 100 / 255;
}
/* Run candidates through the actual serializer precision and parser duplicate rule. */
static pc_status roundtrip(const pc_curve_point *points, size_t count, float input, float output,
                           bool *survives, pc_error *err)
{
    *survives = false;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = d ? yyjson_mut_obj(d) : NULL, *array = d ? yyjson_mut_arr(d) : NULL;
    bool ok = root && array && yyjson_mut_obj_add_val(d, root, "points", array);
    for (size_t i = 0; ok && i < count; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        ok = pair && yyjson_mut_arr_add_real(d, pair, (double)serialized(points[i].input) * 255) &&
             yyjson_mut_arr_add_real(d, pair, (double)serialized(points[i].output) * 255) &&
             yyjson_mut_arr_add_val(array, pair);
    }
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *parsed = NULL;
    pc_adjustment result = {0};
    pc_status status =
        ok ? pc_json_write(root, 0, &text, err) : pc_fail(err, PC_ERR_NOMEM, "eyedropper curve candidate");
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &parsed, err);
    if (status == PC_OK) {
        pc_error candidate_error = {0};
        status = pc_engine_adjustment_from_params("curves", yyjson_doc_get_root(parsed), NULL,
                                                  PC_COLOR_MODE_RGB, &result, &candidate_error);
        if (status != PC_OK && status != PC_ERR_INVALID && err)
            *err = candidate_error;
        if (status == PC_ERR_INVALID)
            status = PC_OK;
        else if (status == PC_OK && result.u.curves.master.len == count) {
            float x = serialized(input), y = serialized(output);
            for (size_t i = 0; i < count; i++)
                if (fabsf(result.u.curves.master.pts[i].input - x) < 1e-6f &&
                    fabsf(result.u.curves.master.pts[i].output - y) < 1e-6f)
                    *survives = true;
        }
    }
    pc_adjustment_fini(&result);
    yyjson_doc_free(parsed);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
    return status;
}
static pc_status endpoint(pc_curve *curve, float sample, bool black, pc_error *err)
{
    if (curve->len < 2 || curve->len > MAX_POINTS || !curve->pts)
        return pc_fail(err, PC_ERR_INVALID, "invalid eyedropper curve");
    pc_curve_point candidate[MAX_POINTS];
    memcpy(candidate, curve->pts, curve->len * sizeof *candidate);
    for (size_t i = 0; i < curve->len; i++)
        candidate[i].input = black ? sample + candidate[i].input * (1 - sample) : candidate[i].input * sample;
    size_t index = black ? 0 : curve->len - 1;
    candidate[index] = (pc_curve_point){sample, black ? 0 : 1};
    bool survives = false;
    pc_status status = roundtrip(candidate, curve->len, sample, black ? 0 : 1, &survives, err);
    if (status != PC_OK)
        return status;
    if (survives)
        memcpy(curve->pts, candidate, curve->len * sizeof *candidate);
    else {
        size_t start = black ? curve->len - 2 : 0;
        curve->pts[start].output = curve->pts[start + 1].output = black ? 0 : 1;
    }
    return PC_OK;
}
static int compare_point(const void *a, const void *b)
{
    float x = ((const pc_curve_point *)a)->input, y = ((const pc_curve_point *)b)->input;
    return x < y ? -1 : x > y ? 1 : 0;
}
static pc_status anchor(pc_curve *curve, float input, float output, pc_error *err)
{
    if (curve->len < 2 || curve->len > MAX_POINTS || !curve->pts)
        return pc_fail(err, PC_ERR_INVALID, "invalid eyedropper curve");
    pc_curve_point candidate[MAX_POINTS];
    size_t count = curve->len;
    memcpy(candidate, curve->pts, count * sizeof *candidate);
    size_t match = SIZE_MAX;
    for (size_t i = 0; i < count; i++)
        if (fabsf(candidate[i].input - input) < DUPLICATE_DISTANCE) {
            match = i;
            break;
        }
    if (match != SIZE_MAX)
        candidate[match] = (pc_curve_point){input, output};
    else if (count < MAX_POINTS)
        candidate[count++] = (pc_curve_point){input, output};
    else {
        float distance = INFINITY;
        for (size_t i = 1; i + 1 < count; i++)
            if (fabsf(candidate[i].input - input) < distance) {
                distance = fabsf(candidate[i].input - input);
                match = i;
            }
        if (match != SIZE_MAX)
            candidate[match] = (pc_curve_point){input, output};
    }
    qsort(candidate, count, sizeof *candidate, compare_point);
    bool survives = false;
    pc_status status = roundtrip(candidate, count, input, output, &survives, err);
    if (status != PC_OK)
        return status;
    if (survives) {
        pc_curve_point *points = pc_malloc_array(count, sizeof *points);
        if (!points)
            return pc_fail(err, PC_ERR_NOMEM, "eyedropper anchor");
        memcpy(points, candidate, count * sizeof *points);
        free(curve->pts);
        curve->pts = points;
        curve->len = count;
    } else {
        size_t split = 0, last = curve->len - 1;
        while (split < curve->len && curve->pts[split].input < input)
            split++;
        size_t lower = split ? split - 1 : 0, upper = split < curve->len ? split : last;
        if (lower > last)
            lower = last;
        if (lower == upper) {
            if (upper < last)
                upper++;
            else if (lower)
                lower--;
        }
        curve->pts[lower].output = curve->pts[upper].output = output;
    }
    return PC_OK;
}
static void levels_input(pc_levels_channel *channel, float sample, bool black)
{
    const float range = 2.f / 255;
    if (black) {
        channel->in_black = fminf(1 - range, fmaxf(0, sample));
        channel->in_white = fminf(1, fmaxf(channel->in_white, channel->in_black + range));
    } else {
        channel->in_white = fminf(1, fmaxf(range, sample));
        channel->in_black = fmaxf(0, fminf(channel->in_black, channel->in_white - range));
    }
}
static void levels_gamma(pc_levels_channel *channel, float sample, float target)
{
    float u = (sample - channel->in_black) / fmaxf(1e-6f, channel->in_white - channel->in_black),
          span = channel->out_white - channel->out_black;
    if (!(u > 0 && u < 1) || fabsf(span) < 1e-6f)
        return;
    float t = (target - channel->out_black) / span;
    if (!(t > 0 && t < 1))
        return;
    float low = logf(.01f), high = logf(9.99f);
    for (unsigned i = 0; i < 40; i++) {
        float mid = (low + high) / 2;
        pc_levels_channel trial = *channel;
        trial.gamma = expf(mid);
        float reached = (pc_compose_levels(&trial, sample) - channel->out_black) / span;
        if (reached < t)
            low = mid;
        else
            high = mid;
    }
    channel->gamma = fminf(9.99f, fmaxf(.01f, expf((low + high) / 2)));
}
pc_status pc_engine_adjustment_pick(const char *kind, yyjson_val *params, const pc_adjustment *base,
                                    pc_engine_tone_picker picker, const float sample[3], pc_color_mode mode,
                                    pc_adjustment *out, pc_error *err)
{
    if (!kind || !sample || !out || (strcmp(kind, "levels") && strcmp(kind, "curves")))
        return pc_fail(err, PC_ERR_INVALID, "eyedropper applies to Levels and Curves only");
    if (!pc_engine_tone_picker_supported(mode, picker))
        return pc_fail(err, PC_ERR_INVALID, "unsupported document mode or neutral-gray picker");
    float value[3];
    for (size_t c = 0; c < 3; c++) {
        if (!isfinite(sample[c]))
            return pc_fail(err, PC_ERR_INVALID, "sample components must be finite");
        value[c] = clamp(sample[c]);
    }
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = NULL;
    bool curves = !strcmp(kind, "curves");
    pc_status status = d ? PC_OK : pc_fail(err, PC_ERR_NOMEM, "eyedropper parameters");
    if (status == PC_OK && base) {
        pc_adjustment merged = {0};
        status = pc_engine_adjustment_from_params(kind, params, base, mode, &merged, err);
        if (status == PC_OK)
            status = pc_engine_adjustment_to_params(&merged, d, &p, err);
        pc_adjustment_fini(&merged);
    } else if (status == PC_OK) {
        p = yyjson_is_obj(params) ? yyjson_val_mut_copy(d, params) : yyjson_mut_obj(d);
        if (!p)
            status = pc_fail(err, PC_ERR_NOMEM, "eyedropper parameters");
    }
    if (status == PC_OK) {
        (void)yyjson_mut_obj_remove_str(p, "eyedropper");
        if (curves) {
            (void)yyjson_mut_obj_remove_str(p, "smooth");
            (void)yyjson_mut_obj_remove_str(p, "pencil");
            if (!yyjson_mut_obj_add_bool(d, p, "pencil", false))
                status = pc_fail(err, PC_ERR_NOMEM, "eyedropper point mode");
        }
    }
    pc_buf text = PC_BUF_INIT;
    yyjson_doc *read = NULL;
    pc_adjustment next = {0};
    if (status == PC_OK)
        status = pc_json_write(p, 0, &text, err);
    if (status == PC_OK)
        status = pc_json_parse(pc_buf_cstr(&text), text.len, &read, err);
    if (status == PC_OK)
        status = pc_engine_adjustment_from_params(kind, yyjson_doc_get_root(read), NULL, mode, &next, err);
    float target = value[0] * .2126f + value[1] * .7152f + value[2] * .0722f;
    if (status == PC_OK && !curves) {
        if (next.kind != PC_ADJ_LEVELS || next.u.levels.space != PC_TONE_RGB)
            status = pc_fail(err, PC_ERR_INVALID, "eyedropper needs RGB channel Levels");
        else if (mode == PC_COLOR_MODE_GRAYSCALE)
            levels_input(&next.u.levels.master, (value[0] + value[1] + value[2]) / 3,
                         picker == PC_TONE_PICK_BLACK);
        else
            for (size_t c = 0; c < 3; c++) {
                if (picker == PC_TONE_PICK_GRAY)
                    levels_gamma(&next.u.levels.per_channel[c], value[c], target);
                else
                    levels_input(&next.u.levels.per_channel[c], value[c], picker == PC_TONE_PICK_BLACK);
            }
    } else if (status == PC_OK) {
        if (next.kind != PC_ADJ_CURVES || next.u.curves.space != PC_TONE_RGB)
            status = pc_fail(err, PC_ERR_INVALID, "eyedropper needs RGB channel Curves");
        else if (mode == PC_COLOR_MODE_GRAYSCALE)
            status = endpoint(&next.u.curves.master, (value[0] + value[1] + value[2]) / 3,
                              picker == PC_TONE_PICK_BLACK, err);
        else if (picker == PC_TONE_PICK_GRAY) {
            if (!pc_curve_is_identity(next.u.curves.master.pts, next.u.curves.master.len)) {
                float table[PC_COMPOSE_LUT_SIZE];
                status = pc_compose_curve_lut(next.u.curves.master.pts, next.u.curves.master.len, table, err);
                if (status == PC_OK) {
                    float best = INFINITY, input = target;
                    for (size_t i = 0; i < PC_COMPOSE_LUT_SIZE; i++)
                        if (fabsf(table[i] - target) < best) {
                            best = fabsf(table[i] - target);
                            input = (float)i / (PC_COMPOSE_LUT_SIZE - 1);
                        }
                    target = input;
                }
            }
            for (size_t c = 0; status == PC_OK && c < 3; c++)
                status = anchor(&next.u.curves.per_channel[c], value[c], target, err);
        } else {
            bool black = picker == PC_TONE_PICK_BLACK;
            for (size_t c = 0; status == PC_OK && c < 3; c++)
                status = endpoint(&next.u.curves.per_channel[c], value[c], black, err);
            if (status == PC_OK && next.u.curves.master.len)
                next.u.curves.master.pts[black ? 0 : next.u.curves.master.len - 1].output = black ? 0 : 1;
        }
    }
    if (status == PC_OK) {
        *out = next;
        next = (pc_adjustment){0};
    }
    pc_adjustment_fini(&next);
    yyjson_doc_free(read);
    pc_buf_fini(&text);
    yyjson_mut_doc_free(d);
    return status;
}
pc_status pc_engine_adjustment_eyedropper(const pc_engine_session *session, const char *kind,
                                          yyjson_val *params, const pc_adjustment *base, bool hide,
                                          pc_layer_id layer, bool *present, pc_adjustment *out, pc_error *err)
{
    if (!session || !present || !out)
        return pc_fail(err, PC_ERR_INVALID, "missing eyedropper context");
    *present = false;
    yyjson_val *e = pc_json_get(params, "eyedropper");
    if (!e)
        return PC_OK;
    *present = true;
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    if (!state)
        return pc_fail(err, PC_ERR_INVALID, "eyedropper needs an open document");
    yyjson_val *point = pc_json_get(e, "point");
    const char *name = yyjson_get_str(point);
    pc_engine_tone_picker picker;
    if (!name || yyjson_get_len(point) != strlen(name))
        return pc_fail(err, PC_ERR_INVALID, "invalid eyedropper point");
    if (!strcmp(name, "black"))
        picker = PC_TONE_PICK_BLACK;
    else if (!strcmp(name, "gray"))
        picker = PC_TONE_PICK_GRAY;
    else if (!strcmp(name, "white"))
        picker = PC_TONE_PICK_WHITE;
    else
        return pc_fail(err, PC_ERR_INVALID, "invalid eyedropper point");
    const pc_document *source = pc_doc_arc_get(state->doc);
    float sample[3];
    yyjson_val *color = pc_json_get(e, "color");
    if (color) {
        if (!yyjson_is_arr(color) || yyjson_arr_size(color) != 3)
            return pc_fail(err, PC_ERR_INVALID, "eyedropper color must be [r,g,b]");
        for (size_t c = 0; c < 3; c++) {
            double v;
            if (!pc_json_as_f64(yyjson_arr_get(color, c), &v) || !isfinite(v) || fabs(v) > 3.402823466e38)
                return pc_fail(err, PC_ERR_INVALID, "eyedropper color components must be finite");
            sample[c] = (float)v;
        }
    } else {
        yyjson_val *at = pc_json_get(e, "at");
        double x, y;
        if (!yyjson_is_arr(at) || yyjson_arr_size(at) != 2 || !pc_json_as_f64(yyjson_arr_get(at, 0), &x) ||
            !pc_json_as_f64(yyjson_arr_get(at, 1), &y) || !isfinite(x) || !isfinite(y) || x < 0 || y < 0 ||
            x >= source->size.width || y >= source->size.height)
            return pc_fail(err, PC_ERR_INVALID, "eyedropper at must be a point inside the image");
        uint32_t size = 1;
        pc_status status = pc_engine_sample_size(e, &size, err);
        if (status != PC_OK)
            return status;
        pc_document copy = {0};
        const pc_document *doc = source;
        if (hide) {
            status = pc_document_clone(source, &copy, err);
            if (status != PC_OK)
                return status;
            pc_layer *hidden = pc_document_layer_mut(&copy, layer);
            if (!hidden) {
                pc_document_fini(&copy);
                return pc_fail(err, PC_ERR_NOT_FOUND, "eyedropper layer was removed");
            }
            hidden->visible = false;
            doc = &copy;
        }
        float rgba[4];
        status = pc_engine_sample_color(doc, state->has_active_layer, state->active_layer, x, y, size,
                                        PC_SAMPLE_ALL, rgba, err);
        if (doc == &copy)
            pc_document_fini(&copy);
        if (status != PC_OK)
            return status;
        if (rgba[3] <= 0)
            return pc_fail(err, PC_ERR_INVALID, "the sampled pixel is transparent");
        memcpy(sample, rgba, sizeof sample);
    }
    return pc_engine_adjustment_pick(kind, params, base, picker, sample, source->mode, out, err);
}
