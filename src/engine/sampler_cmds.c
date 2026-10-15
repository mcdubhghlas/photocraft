/* Color Sampler points (port of crates/engine/src/sampler_cmds.rs, #1046): numbered colour
 * readings pinned to canvas pixels, listed in the Info panel, so a fixed position can be compared
 * instead of chasing the live pointer.
 *
 * Points are per-document view state (pc_engine_doc_state::color_samplers): they belong to their
 * document, are discarded when it closes, and are not saved to .pcraft or PSD. Placing, moving and
 * removing them changes no pixels and adds no History step, so they are not journaled. Each point
 * reads one composite pixel (no averaging); its values match the Info panel's pointer readout
 * (RGB 0-255 and the naive device CMYK percentages, without an ICC profile). A canvas-size edit
 * moves them with their pixels (session.c edit()).
 *
 * Never crashes on hostile params (bad coords, off-canvas points, huge indices): REWRITE_PLAN.md
 * rule 9. */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "compose/compose.h"
#include "doc/doc.h"

#include "engine_internal.h"

/* Photoshop's Color Sampler limit: at most 10 points per document (MAX_SAMPLERS). */
#define PC_ENGINE_MAX_SAMPLERS 10

/* Floor of v to an i32, saturating (NaN -> INT32_MIN). */
static int32_t floor_i32_sat(double v)
{
    double f = floor(v);
    if (!(f >= (double)INT32_MIN)) { /* also catches NaN */
        return INT32_MIN;
    }
    if (f > (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)f;
}

/* Naive device CMYK (no ICC profile), in percent, as the Info panel shows it. 32-bit samples can
 * lie outside 0..1, so each channel is clamped first (NaN reads as 0), like the RGB readout. Port
 * of sampler_cmds::device_cmyk. */
static void device_cmyk(float r, float g, float b, int32_t out[4])
{
    float u[3];
    float in[3] = {r, g, b};
    for (int i = 0; i < 3; i++) {
        float v = in[i];
        u[i] = isnan(v) ? 0.0f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
    }
    float mx = u[0] > u[1] ? u[0] : u[1];
    if (u[2] > mx) {
        mx = u[2];
    }
    float k = 1.0f - mx;
    for (int i = 0; i < 3; i++) {
        float f = (k >= 1.0f) ? 0.0f : (1.0f - u[i] - k) / (1.0f - k);
        out[i] = (int32_t)lroundf(f * 100.0f);
    }
    out[3] = (int32_t)lroundf(k * 100.0f);
}

/* The Info-panel values for a sample point at document pixel (px, py): composite RGB 0-255 and
 * naive device CMYK percentages. One composite pixel, no averaging (sampler_cmds::readout with
 * size 1, SampleLayers::All -> compose::render of the 1x1 rect). */
static void readout(const pc_document *doc, double px, double py, int32_t rgb[3], int32_t cmyk[4])
{
    float rgba[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int32_t ix = floor_i32_sat(px);
    int32_t iy = floor_i32_sat(py);
    pc_rect r = {ix, iy, ix < INT32_MAX ? ix + 1 : ix, iy < INT32_MAX ? iy + 1 : iy};
    if (r.x1 > r.x0 && r.y1 > r.y0) {
        pc_compose_buffer buf = {0};
        if (pc_compose_render(doc, r, &buf, NULL) == PC_OK) {
            const float *p = pc_compose_buffer_get(&buf, ix, iy);
            if (p != NULL) {
                rgba[0] = p[0];
                rgba[1] = p[1];
                rgba[2] = p[2];
                rgba[3] = p[3];
            }
        }
        pc_compose_buffer_fini(&buf);
    }
    for (int i = 0; i < 3; i++) {
        float v = rgba[i];
        v = isnan(v) ? 0.0f : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
        rgb[i] = (int32_t)lroundf(v * 255.0f);
    }
    device_cmyk(rgba[0], rgba[1], rgba[2], cmyk);
}

/* One point as the list command reports it (sampler_cmds::point_json). */
static yyjson_mut_val *point_json(yyjson_mut_doc *d, const pc_document *doc, size_t index,
                                  const double pos[2])
{
    int32_t rgb[3], cmyk[4];
    readout(doc, pos[0], pos[1], rgb, cmyk);
    yyjson_mut_val *o = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, o, "index", (uint64_t)index);
    yyjson_mut_obj_add_uint(d, o, "number", (uint64_t)index + 1);
    yyjson_mut_val *p = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, p, pos[0]);
    yyjson_mut_arr_add_real(d, p, pos[1]);
    yyjson_mut_obj_add_val(d, o, "position", p);
    yyjson_mut_val *jrgb = yyjson_mut_arr(d);
    for (int i = 0; i < 3; i++) {
        yyjson_mut_arr_add_int(d, jrgb, rgb[i]);
    }
    yyjson_mut_obj_add_val(d, o, "rgb", jrgb);
    yyjson_mut_val *jcmyk = yyjson_mut_arr(d);
    for (int i = 0; i < 4; i++) {
        yyjson_mut_arr_add_int(d, jcmyk, cmyk[i]);
    }
    yyjson_mut_obj_add_val(d, o, "cmyk", jcmyk);
    return o;
}

/* ---- params ------------------------------------------------------------------------------- */

static pc_status coord_param(const char *cmd, yyjson_val *p, const char *key, double *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, key);
    double d = 0.0;
    if (v == NULL || !pc_json_as_f64(v, &d) || !isfinite(d)) {
        return pc_engine_err_bad_params(err, cmd, "`%s` must be a finite number (document pixels)", key);
    }
    *out = d;
    return PC_OK;
}

static pc_status index_param(const char *cmd, yyjson_val *p, size_t *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "index");
    double d = 0.0;
    if (v == NULL || !pc_json_as_f64(v, &d) || !isfinite(d) || d < 0.0 || floor(d) != d) {
        return pc_engine_err_bad_params(err, cmd, "pass `index` (0-based)");
    }
    *out = (size_t)d;
    return PC_OK;
}

/* The pixel a point at document coordinate (x, y) sits on, if it is inside the canvas. Whole pixels
 * only: the click's fractional position floors to the pixel it lands on. Returns false when off the
 * canvas or out of range (sampler_cmds::pixel). */
static bool pixel_of(const pc_document *doc, double x, double y, double out[2])
{
    double px = floor(x);
    double py = floor(y);
    if (px < (double)INT32_MIN || px > (double)INT32_MAX || py < (double)INT32_MIN || py > (double)INT32_MAX) {
        return false;
    }
    if (!pc_rect_contains(pc_document_bounds(doc), (int32_t)px, (int32_t)py)) {
        return false;
    }
    out[0] = px;
    out[1] = py;
    return true;
}

/* Apply a Color Sampler change: view state, so no history step and no pixels. Bumps the revision
 * (keeping a clean document clean) so UIs refresh (sampler_cmds::change). */
static void change_commit(pc_engine_doc_state *st)
{
    bool clean = st->saved_revision == st->revision;
    st->revision++;
    if (clean) {
        st->saved_revision = st->revision;
    }
    st->has_last_damage = true;
    st->last_damage = PC_RECT_EMPTY;
}

/* ---- enabled ------------------------------------------------------------------------------ */

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

/* ---- commands ----------------------------------------------------------------------------- */

static pc_status run_add(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "view.colorSamplers.add";
    double x = 0.0, y = 0.0;
    pc_status rc = coord_param(CMD, p, "x", &x, err);
    if (rc != PC_OK) {
        return rc;
    }
    if ((rc = coord_param(CMD, p, "y", &y, err)) != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    double pos[2];
    if (!pixel_of(doc, x, y, pos)) {
        return pc_engine_err_other(err, "the sample point is outside the canvas");
    }
    if (st->ncolor_samplers >= PC_ENGINE_MAX_SAMPLERS) {
        return pc_engine_err_other(
            err, "Color Sampler limit reached: at most %d points per document", PC_ENGINE_MAX_SAMPLERS);
    }
    double(*grown)[2] = pc_grow_array(st->color_samplers, &st->cap_color_samplers, st->ncolor_samplers + 1,
                                      sizeof *st->color_samplers);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "color samplers");
    }
    st->color_samplers = grown;
    size_t i = st->ncolor_samplers;
    st->color_samplers[i][0] = pos[0];
    st->color_samplers[i][1] = pos[1];
    st->ncolor_samplers++;
    change_commit(st);

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "index", (uint64_t)i);
    yyjson_mut_obj_add_uint(d, root, "count", (uint64_t)st->ncolor_samplers);
    yyjson_mut_obj_add_val(d, root, "point", point_json(d, doc, i, st->color_samplers[i]));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_move(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "view.colorSamplers.move";
    size_t i = 0;
    pc_status rc = index_param(CMD, p, &i, err);
    if (rc != PC_OK) {
        return rc;
    }
    double x = 0.0, y = 0.0;
    if ((rc = coord_param(CMD, p, "x", &x, err)) != PC_OK) {
        return rc;
    }
    if ((rc = coord_param(CMD, p, "y", &y, err)) != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (i >= st->ncolor_samplers) {
        return pc_engine_err_bad_params(err, CMD, "no sample point %zu (the document has %zu)", i,
                                        st->ncolor_samplers);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    double pos[2];
    if (!pixel_of(doc, x, y, pos)) {
        return pc_engine_err_other(err, "the sample point is outside the canvas");
    }
    st->color_samplers[i][0] = pos[0];
    st->color_samplers[i][1] = pos[1];
    change_commit(st);

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "index", (uint64_t)i);
    yyjson_mut_obj_add_val(d, root, "point", point_json(d, doc, i, st->color_samplers[i]));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "view.colorSamplers.delete";
    size_t i = 0;
    pc_status rc = index_param(CMD, p, &i, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (i >= st->ncolor_samplers) {
        return pc_engine_err_bad_params(err, CMD, "no sample point %zu (the document has %zu)", i,
                                        st->ncolor_samplers);
    }
    memmove(&st->color_samplers[i], &st->color_samplers[i + 1],
            (st->ncolor_samplers - i - 1) * sizeof *st->color_samplers);
    st->ncolor_samplers--;
    change_commit(st);

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "deleted", 1);
    yyjson_mut_obj_add_uint(d, root, "remaining", (uint64_t)st->ncolor_samplers);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_clear(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    size_t n = st->ncolor_samplers;
    st->ncolor_samplers = 0;
    change_commit(st);

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "deleted", (uint64_t)n);
    yyjson_mut_obj_add_uint(d, root, "remaining", 0);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "max", PC_ENGINE_MAX_SAMPLERS);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < st->ncolor_samplers; i++) {
        yyjson_mut_arr_add_val(arr, point_json(d, doc, i, st->color_samplers[i]));
    }
    yyjson_mut_obj_add_val(d, root, "samplers", arr);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

const pc_engine_command *pc_engine_sampler_specs(size_t *n)
{
    static const pc_engine_command specs[] = {
        {"view.colorSamplers.add", "Add Color Sampler", NULL, 0, NULL,
         "{\"x\":f64,\"y\":f64} -> the new point {index, number, position, rgb, cmyk}; errors at the "
         "10-point limit or off the canvas",
         has_doc, run_add, false},
        {"view.colorSamplers.move", "Move Color Sampler", NULL, 0, NULL, "{\"index\":n,\"x\":f64,\"y\":f64}",
         has_doc, run_move, false},
        {"view.colorSamplers.delete", "Delete Color Sampler", NULL, 0, NULL, "{\"index\":n}", has_doc,
         run_delete, false},
        {"view.colorSamplers.clear", "Clear Color Samplers", NULL, 0, NULL, "{}", has_doc, run_clear, false},
        {"view.colorSamplers.list", "Color Samplers", NULL, 0, NULL,
         "{} -> {max, samplers:[{index, number, position, rgb, cmyk}]}", has_doc, run_list, false},
    };
    *n = sizeof specs / sizeof specs[0];
    return specs;
}
