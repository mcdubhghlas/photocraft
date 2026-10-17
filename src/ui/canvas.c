/* Camera behaviour ported from canvas::ViewXform, fit_view/fill_view and zoom_tool::anchor_view;
 * zoom/rotation/wheel/scroll contracts from their reference modules. */
#include "canvas.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const float ladder[] = {
    .001f, .002f,  .003f,      .004f, .005f,     .007f, .01f,      .015f, .02f,      .03f, .04f,
    .05f,  .0625f, 1.f / 12.f, .125f, 1.f / 6.f, .25f,  1.f / 3.f, .5f,   2.f / 3.f, 1.f,  2.f,
    3.f,   4.f,    5.f,        6.f,   7.f,       8.f,   12.f,      16.f,  32.f,      64.f, 128.f};

float pc_ui_zoom_min(const uint32_t size[2])
{
    if (!size) {
        return .001f;
    }
    uint32_t shorter = size[0] < size[1] ? size[0] : size[1];
    return shorter ? fminf(128.f, 2.f / (float)shorter) : .001f;
}

float pc_ui_zoom_clamp(float zoom, const uint32_t size[2])
{
    return isfinite(zoom) ? fmaxf(pc_ui_zoom_min(size), fminf(128.f, zoom)) : 1.f;
}

float pc_ui_zoom_step(float zoom, int direction, const uint32_t size[2])
{
    float z = pc_ui_zoom_clamp(zoom, size), next = direction > 0 ? 128.f : z;
    size_t n = sizeof ladder / sizeof ladder[0];
    for (size_t i = 0; i < n; i++) {
        float rung = ladder[direction > 0 ? i : n - i - 1];
        if (direction > 0 ? rung > z * 1.001f : rung < z * .999f) {
            next = rung;
            break;
        }
    }
    return pc_ui_zoom_clamp(next, size);
}

float pc_ui_rotation_wrap(float degrees)
{
    if (!isfinite(degrees)) {
        return 0;
    }
    double r = fmod((double)degrees, 360.);
    if (r > 180.) {
        r -= 360.;
    }
    if (r <= -180.) {
        r += 360.;
    }
    return (float)r;
}

float pc_ui_rotation_snap(float degrees, float step)
{
    if (!(isfinite(step) && step > 0.f)) {
        return pc_ui_rotation_wrap(degrees);
    }
    return pc_ui_rotation_wrap(roundf(degrees / step) * step);
}

pc_status pc_ui_rotation_params(float current, yyjson_val *params, float *out, pc_error *err)
{
    if (!out) {
        return pc_fail(err, PC_ERR_INVALID, "missing rotation output");
    }
    if (!yyjson_is_obj(params)) {
        return pc_fail(err, PC_ERR_INVALID, "params must be an object");
    }
    bool reset = false, have_angle = false, have_delta = false;
    float angle = current, delta = 0;
    size_t i, n;
    yyjson_val *key, *val;
    yyjson_obj_foreach(params, i, n, key, val)
    {
        const char *k = yyjson_get_str(key);
        if (strcmp(k, "reset") == 0) {
            if (!pc_json_as_bool(val, &reset)) {
                return pc_fail(err, PC_ERR_INVALID, "reset must be a boolean");
            }
        } else if (strcmp(k, "angle") == 0 || strcmp(k, "delta") == 0) {
            double d;
            if (!pc_json_as_f64(val, &d) || !isfinite(d) || fabs(d) > FLT_MAX) {
                return pc_fail(err, PC_ERR_INVALID, "%s must be a finite number representable as f32", k);
            }
            if (strcmp(k, "angle") == 0) {
                angle = (float)d;
                have_angle = true;
            } else {
                delta = (float)d;
                have_delta = true;
            }
        } else {
            return pc_fail(err, PC_ERR_INVALID, "unknown field `%s`", k);
        }
    }
    if (!reset && !have_angle && !have_delta) {
        return pc_fail(err, PC_ERR_INVALID, "expected angle, delta, or reset");
    }
    if (!reset && !isfinite(angle + delta)) {
        return pc_fail(err, PC_ERR_INVALID, "delta overflowed the angle");
    }
    *out = reset ? 0.f : pc_ui_rotation_wrap(angle + delta);
    return PC_OK;
}

static bool valid_xform(const pc_ui_canvas_xform *xf)
{
    return xf && isfinite(xf->x) && isfinite(xf->y) && isfinite(xf->width) && xf->width > 0.f &&
           isfinite(xf->height) && xf->height > 0.f && isfinite(xf->zoom) && xf->zoom > 0.f &&
           isfinite(xf->rotation) && isfinite(xf->center[0]) && isfinite(xf->center[1]);
}

static float aspect_of(const pc_ui_canvas_xform *xf)
{
    return isfinite(xf->aspect) && xf->aspect > 0.f ? xf->aspect : 1.f;
}

static void sincos_of(const pc_ui_canvas_xform *xf, float *c, float *s)
{
    float rad = pc_ui_rotation_wrap(xf->rotation) * .017453292519943295f;
    *c = cosf(rad);
    *s = sinf(rad);
}

pc_status pc_ui_canvas_to_screen(const pc_ui_canvas_xform *xf, float x, float y, float out[2])
{
    if (!out || !valid_xform(xf) || !isfinite(x) || !isfinite(y)) {
        return PC_ERR_INVALID;
    }
    float c, s;
    sincos_of(xf, &c, &s);
    float dx = (x - xf->center[0]) * aspect_of(xf), dy = y - xf->center[1];
    float px = xf->x + xf->width / 2.f + (dx * c - dy * s) * xf->zoom * (xf->flip ? -1.f : 1.f);
    float py = xf->y + xf->height / 2.f + (dx * s + dy * c) * xf->zoom;
    if (!isfinite(px) || !isfinite(py)) {
        return PC_ERR_LIMIT;
    }
    out[0] = px;
    out[1] = py;
    return PC_OK;
}

pc_status pc_ui_canvas_to_doc(const pc_ui_canvas_xform *xf, float x, float y, float out[2])
{
    if (!out || !valid_xform(xf) || !isfinite(x) || !isfinite(y)) {
        return PC_ERR_INVALID;
    }
    float c, s;
    sincos_of(xf, &c, &s);
    float dx = (x - (xf->x + xf->width / 2.f)) / xf->zoom * (xf->flip ? -1.f : 1.f);
    float dy = (y - (xf->y + xf->height / 2.f)) / xf->zoom;
    float px = xf->center[0] + (dx * c + dy * s) / aspect_of(xf);
    float py = xf->center[1] - dx * s + dy * c;
    if (!isfinite(px) || !isfinite(py)) {
        return PC_ERR_LIMIT;
    }
    out[0] = px;
    out[1] = py;
    return PC_OK;
}

void pc_ui_canvas_fit(pc_ui_view *v, float width, float height, float ppp, float aspect, bool fill)
{
    if (!v || !v->doc_size[0] || !v->doc_size[1] || !isfinite(width) || !isfinite(height) || width <= 0.f ||
        height <= 0.f) {
        return;
    }
    if (!(isfinite(ppp) && ppp > 0.f)) {
        ppp = 1.f;
    }
    if (!(isfinite(aspect) && aspect > 0.f)) {
        aspect = 1.f;
    }
    float w = (float)v->doc_size[0], h = (float)v->doc_size[1];
    float z = fill ? fmaxf(width / (w * aspect), height / h)
                   : fminf(1.f, fminf((width - 40.f) / (w * aspect), (height - 40.f) / h));
    v->zoom = pc_ui_zoom_clamp(z * ppp, v->doc_size);
    v->center[0] = w / 2.f;
    v->center[1] = h / 2.f;
    v->fit_pending = false;
    v->fill_pending = false;
}

pc_status pc_ui_canvas_anchor(pc_ui_view *v, const pc_ui_canvas_xform *xf, float x, float y, float zoom,
                              float ppp)
{
    float doc[2], after[2];
    if (!v || !valid_xform(xf) || pc_ui_canvas_to_doc(xf, x, y, doc) != PC_OK) {
        return PC_ERR_INVALID;
    }
    if (!(isfinite(ppp) && ppp > 0.f)) {
        ppp = 1.f;
    }
    pc_ui_canvas_xform next = *xf;
    zoom = pc_ui_zoom_clamp(zoom, v->doc_size);
    next.zoom = zoom / ppp;
    if (pc_ui_canvas_to_doc(&next, x, y, after) != PC_OK) {
        return PC_ERR_INVALID;
    }
    float cx = v->center[0] + doc[0] - after[0], cy = v->center[1] + doc[1] - after[1];
    if (!isfinite(cx) || !isfinite(cy)) {
        return PC_ERR_LIMIT;
    }
    v->zoom = zoom;
    v->center[0] = cx;
    v->center[1] = cy;
    v->fit_pending = false;
    v->fill_pending = false;
    return PC_OK;
}

pc_status pc_ui_canvas_pan(pc_ui_view *v, const pc_ui_canvas_xform *xf, float dx, float dy)
{
    float p[2], q[2];
    if (!v || !valid_xform(xf) || pc_ui_canvas_to_doc(xf, xf->x, xf->y, p) != PC_OK ||
        pc_ui_canvas_to_doc(xf, xf->x + dx, xf->y + dy, q) != PC_OK) {
        return PC_ERR_INVALID;
    }
    float cx = v->center[0] - (q[0] - p[0]), cy = v->center[1] - (q[1] - p[1]);
    if (!isfinite(cx) || !isfinite(cy)) {
        return PC_ERR_LIMIT;
    }
    v->center[0] = cx;
    v->center[1] = cy;
    v->fit_pending = false;
    v->fill_pending = false;
    return PC_OK;
}

pc_ui_wheel_kind pc_ui_wheel_classify(pc_ui_wheel_input i, float v[2])
{
    if (!v) {
        return PC_UI_WHEEL_NONE;
    }
    v[0] = 0;
    v[1] = 0;
    if (i.pinch_zooms && isfinite(i.zoom_delta) && i.zoom_delta > 0.f && i.zoom_delta != 1.f) {
        v[0] = i.zoom_delta;
        return PC_UI_WHEEL_ZOOM;
    }
    if ((i.alt && !i.zoom_with_wheel) ||
        (!i.alt && i.zoom_with_wheel && (i.raw[1] != 0.f || i.scroll[1] != 0.f))) {
        float notch = isfinite(i.notch) && i.notch > 0.f ? i.notch : 40.f;
        float dy = i.raw[1] + (i.alt ? i.raw[0] : 0.f), z = powf(1.1f, dy / notch);
        if (isfinite(z) && z > 0.f && z != 1.f) {
            v[0] = z;
            return PC_UI_WHEEL_ZOOM;
        }
    } else if (isfinite(i.scroll[0]) && isfinite(i.scroll[1]) && (i.scroll[0] != 0.f || i.scroll[1] != 0.f)) {
        v[0] = i.scroll[0];
        v[1] = i.scroll[1];
        return PC_UI_WHEEL_PAN;
    }
    return PC_UI_WHEEL_NONE;
}

pc_status pc_ui_canvas_ruler_xform(const pc_ui_canvas_xform *xf, pc_ui_canvas_xform *out)
{
    if (!out || !valid_xform(xf)) {
        return PC_ERR_INVALID;
    }
    pc_ui_canvas_xform level = *xf;
    level.rotation = 0;
    float actual[2], zero[2], center[2];
    if (pc_ui_canvas_to_screen(xf, 0, 0, actual) != PC_OK ||
        pc_ui_canvas_to_screen(&level, 0, 0, zero) != PC_OK ||
        pc_ui_canvas_to_doc(&level, xf->x + xf->width / 2.f - (actual[0] - zero[0]),
                            xf->y + xf->height / 2.f - (actual[1] - zero[1]), center) != PC_OK) {
        return PC_ERR_INVALID;
    }
    level.center[0] = center[0];
    level.center[1] = center[1];
    *out = level;
    return PC_OK;
}

double pc_ui_ruler_nice_step(double minimum, bool whole)
{
    static const double multipliers[] = {1., 2., 2.5, 5.};
    if (!isfinite(minimum) || minimum <= 0.) {
        return whole ? 1. : .001;
    }
    double step = whole ? 1. : .001;
    for (unsigned i = 0; i < 320; i++, step *= 10.) {
        for (size_t j = 0; j < sizeof multipliers / sizeof multipliers[0]; j++) {
            if (whole && multipliers[j] == 2.5) {
                continue;
            }
            double candidate = step * multipliers[j];
            if (candidate >= minimum) {
                return isfinite(candidate) ? candidate : minimum;
            }
        }
    }
    return minimum;
}

void pc_ui_canvas_cache_fini(pc_ui_canvas_cache *c)
{
    if (c) {
        pc_compose_buffer_fini(&c->image);
        *c = (pc_ui_canvas_cache){0};
    }
}

pc_status pc_ui_canvas_cache_update(pc_ui_canvas_cache *c, const pc_engine_doc_state *state, pc_error *err)
{
    if (!c || !state || !state->doc) {
        return pc_fail(err, PC_ERR_INVALID, "no canvas document");
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (c->valid && c->document == doc->id && c->revision == state->revision) {
        return PC_OK;
    }
    pc_compose_buffer image = {0};
    pc_status rc = pc_compose_thumbnail_buffer(doc, 4096, &image, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < image.len; i++) {
        float *p = image.px[i];
        p[3] = isfinite(p[3]) ? fminf(1.f, fmaxf(0.f, p[3])) : 0.f;
        for (size_t ch = 0; ch < 3; ch++) {
            p[ch] = isfinite(p[ch]) ? p[ch] * p[3] : 0.f;
        }
    }
    pc_compose_buffer_fini(&c->image);
    c->image = image;
    c->document = doc->id;
    c->revision = state->revision;
    c->generation++;
    c->valid = true;
    return PC_OK;
}
