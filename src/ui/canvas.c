/* Camera behaviour ported from canvas::ViewXform, fit_view/fill_view and zoom_tool::anchor_view;
 * zoom/rotation/wheel/scroll contracts from their reference modules. */
#include "canvas.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "base/num.h"
#include "compose/effects.h"
#include "compose/masks.h"
#include "doc/tree.h"

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

uint32_t pc_ui_canvas_render_key(void)
{
    float gamma = pc_psblend_text_gamma();
    uint32_t bits = 0;
    memcpy(&bits, &gamma, sizeof bits);
    return bits;
}

void pc_ui_canvas_cache_fini(pc_ui_canvas_cache *c)
{
    if (c) {
        pc_compose_buffer_fini(&c->image);
        *c = (pc_ui_canvas_cache){0};
    }
}

static bool local_effect(const pc_effect *effect)
{
    if (!pc_effect_enabled(effect))
        return true;
    switch (effect->kind) {
    case PC_FX_GRADIENT_OVERLAY:
    case PC_FX_PATTERN_OVERLAY:
        return false; /* A changing shape bound can move the entire paint frame. */
    case PC_FX_STROKE:
        return effect->u.stroke.paint.kind == PC_FX_PAINT_COLOR;
    case PC_FX_INNER_GLOW:
    case PC_FX_OUTER_GLOW:
        return effect->u.glow.paint.kind == PC_FX_PAINT_COLOR;
    case PC_FX_BEVEL_EMBOSS:
        return !effect->u.bevel.has_texture;
    default:
        return true;
    }
}
bool pc_ui_canvas_cache_damage_rect(const pc_ui_canvas_cache *c, const pc_engine_doc_state *state,
                                    pc_rect *out)
{
    if (!c || !state || !state->doc || !c->valid || c->revision == UINT64_MAX ||
        c->revision + 1 != state->revision || !state->has_last_damage ||
        c->render_key != pc_ui_canvas_render_key())
        return false;
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (c->document != doc->id || !pc_rect_eq(c->image.rect, c->source_rect))
        return false; /* Area-averaged previews require a complete resample. */
    pc_doc_iter it;
    pc_doc_iter_init(&it, doc->layers, doc->nlayers);
    const pc_layer *layer = NULL;
    int next;
    bool local = true;
    int64_t halo = 0;
    while ((next = pc_doc_iter_next(&it, &layer)) > 0) {
        if (pc_compose_has_feather(layer)) {
            local = false;
            break;
        }
        if (pc_compose_has_effects(layer)) {
            if (!pc_compose_fx_maps_are_local(layer)) {
                local = false;
                break;
            }
            for (size_t i = 0; i < layer->effects.nitems; i++)
                local = local && local_effect(&layer->effects.items[i]);
            halo += pc_compose_effects_margin(layer);
            if (!local || halo > INT32_MAX) {
                local = false;
                break;
            }
        }
    }
    pc_doc_iter_fini(&it);
    if (!local || next < 0)
        return false;
    pc_rect damage = pc_rect_is_empty(state->last_damage)
                         ? PC_RECT_EMPTY
                         : pc_rect_inflate(state->last_damage, (int32_t)halo);
    if (out)
        *out = pc_rect_intersect(c->source_rect, damage);
    return true;
}
bool pc_ui_canvas_cache_can_damage(const pc_ui_canvas_cache *c, const pc_engine_doc_state *state)
{
    return pc_ui_canvas_cache_damage_rect(c, state, NULL);
}
static void premultiply(pc_compose_buffer *image)
{
    for (size_t i = 0; i < image->len; i++) {
        float *p = image->px[i];
        p[3] = isfinite(p[3]) ? fminf(1, fmaxf(0, p[3])) : 0;
        for (size_t j = 0; j < 3; j++)
            p[j] = isfinite(p[j]) ? p[j] * p[3] : 0;
    }
}
static pc_status update_damage(pc_ui_canvas_cache *c, const pc_engine_doc_state *state,
                               pc_gpu_compositor *gpu, pc_error *err)
{
    pc_rect damage;
    if (!pc_ui_canvas_cache_damage_rect(c, state, &damage))
        return pc_fail(err, PC_ERR_INVALID, "canvas damage is no longer bounded");
    pc_compose_buffer patch = {0};
    pc_status status = PC_OK;
    if (!pc_rect_is_empty(damage))
        status = pc_ui_canvas_compose(pc_doc_arc_get(state->doc), damage, gpu, &patch, err);
    if (status != PC_OK)
        return status;
    if (!pc_rect_is_empty(damage) && (!patch.px || !c->image.px)) {
        pc_compose_buffer_fini(&patch);
        return pc_fail(err, PC_ERR_IO, "missing canvas damage pixels");
    }
    premultiply(&patch);
    size_t width = pc_rect_width(c->source_rect), patch_width = pc_rect_width(damage);
    for (size_t row = 0; patch.px && row < pc_rect_height(damage); row++) {
        size_t offset =
            ((size_t)(damage.y0 - c->source_rect.y0) + row) * width + (size_t)(damage.x0 - c->source_rect.x0);
        memcpy(c->image.px + offset, patch.px + row * patch_width, patch_width * sizeof *patch.px);
    }
    pc_compose_buffer_fini(&patch);
    if (!pc_rect_is_empty(damage))
        c->generation++;
    c->revision = state->revision;
    c->last_render = damage;
    c->partial = true;
    return PC_OK;
}

pc_status pc_ui_canvas_cache_update(pc_ui_canvas_cache *c, const pc_engine_doc_state *state, pc_error *err)
{
    if (!c || !state || !state->doc) {
        return pc_fail(err, PC_ERR_INVALID, "no canvas document");
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if (c->valid && c->document == doc->id && c->revision == state->revision &&
        c->render_key == pc_ui_canvas_render_key() && !c->region) {
        return PC_OK;
    }
    if (!c->region && pc_rect_eq(c->source_rect, pc_document_bounds(doc)) &&
        pc_ui_canvas_cache_can_damage(c, state))
        return update_damage(c, state, NULL, err);
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
    c->render_key = pc_ui_canvas_render_key();
    c->generation++;
    c->source_rect = pc_document_bounds(doc);
    c->last_render = c->source_rect;
    c->partial = false;
    c->region = false;
    c->valid = true;
    return PC_OK;
}

pc_status pc_ui_canvas_cache_update_view(pc_ui_canvas_cache *c, const pc_engine_doc_state *state,
                                         const pc_ui_canvas_xform *xf, pc_error *err)
{
    return pc_ui_canvas_cache_update_view_gpu(c, state, xf, NULL, err);
}
pc_status pc_ui_canvas_compose(const pc_document *doc, pc_rect rect, pc_gpu_compositor *gpu,
                               pc_compose_buffer *out, pc_error *err)
{
    if (gpu && pc_gpu_compositor_healthy(gpu) && pc_gpu_render(gpu, doc, rect, out, NULL) == PC_OK) {
        return PC_OK;
    }
    return pc_compose_render(doc, rect, out, err);
}
pc_status pc_ui_canvas_cache_update_view_gpu(pc_ui_canvas_cache *c, const pc_engine_doc_state *state,
                                             const pc_ui_canvas_xform *xf, pc_gpu_compositor *gpu,
                                             pc_error *err)
{
    if (!c || !state || !state->doc || !valid_xform(xf)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid canvas viewport");
    }
    const pc_document *doc = pc_doc_arc_get(state->doc);
    if ((doc->size.width <= 4096 && doc->size.height <= 4096 && !gpu) || xf->zoom < .5f) {
        return pc_ui_canvas_cache_update(c, state, err);
    }
    double lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
    for (unsigned corner = 0; corner < 4; corner++) {
        float p[2];
        if (pc_ui_canvas_to_doc(xf, xf->x + ((corner & 1) ? xf->width : 0),
                                xf->y + ((corner & 2) ? xf->height : 0), p) != PC_OK) {
            return pc_fail(err, PC_ERR_INVALID, "invalid canvas viewport");
        }
        for (size_t axis = 0; axis < 2; axis++) {
            lo[axis] = fmin(lo[axis], p[axis]);
            hi[axis] = fmax(hi[axis], p[axis]);
        }
    }
    pc_rect bounds = pc_document_bounds(doc);
    /* 256-pixel tile padding: small pans reuse the same texture and never rescan a surface. */
    pc_rect area = pc_rect_make(pc_f64_to_i32(fmax(0, floor(lo[0] / 256) * 256 - 256)),
                                pc_f64_to_i32(fmax(0, floor(lo[1] / 256) * 256 - 256)),
                                pc_f64_to_i32(fmin(bounds.x1, (floor(hi[0] / 256) + 2) * 256)),
                                pc_f64_to_i32(fmin(bounds.y1, (floor(hi[1] / 256) + 2) * 256)));
    area = pc_rect_intersect(area, bounds);
    if (pc_rect_is_empty(area) || pc_rect_width(area) > 4096 || pc_rect_height(area) > 4096) {
        return pc_ui_canvas_cache_update(c, state, err);
    }
    if (c->valid && c->region && c->document == doc->id && c->revision == state->revision &&
        c->render_key == pc_ui_canvas_render_key() && pc_rect_eq(c->source_rect, area)) {
        return PC_OK;
    }
    if (c->region && pc_rect_eq(c->source_rect, area) && pc_ui_canvas_cache_can_damage(c, state))
        return update_damage(c, state, gpu, err);
    pc_compose_buffer image = {0};
    pc_status rc = pc_ui_canvas_compose(doc, area, gpu, &image, err);
    if (rc != PC_OK) {
        return rc;
    }
    for (size_t i = 0; i < image.len; i++) {
        float *p = image.px[i];
        p[3] = isfinite(p[3]) ? fminf(1, fmaxf(0, p[3])) : 0;
        for (size_t j = 0; j < 3; j++) {
            p[j] = isfinite(p[j]) ? p[j] * p[3] : 0;
        }
    }
    pc_compose_buffer_fini(&c->image);
    c->image = image;
    c->document = doc->id;
    c->revision = state->revision;
    c->render_key = pc_ui_canvas_render_key();
    c->generation++;
    c->source_rect = area;
    c->last_render = area;
    c->partial = false;
    c->region = true;
    c->valid = true;
    return PC_OK;
}

pc_status pc_ui_canvas_cache_update_plane(pc_ui_canvas_cache *c, const pc_engine_doc_state *state,
                                          const pc_ui_canvas_xform *xf, pc_gpu_compositor *gpu, pc_error *err)
{
    if (!c || !state || !state->doc || !valid_xform(xf))
        return pc_fail(err, PC_ERR_INVALID, "invalid crop viewport");
    double lo[2] = {INFINITY, INFINITY}, hi[2] = {-INFINITY, -INFINITY};
    for (unsigned corner = 0; corner < 4; corner++) {
        float point[2];
        if (pc_ui_canvas_to_doc(xf, xf->x + ((corner & 1) ? xf->width : 0),
                                xf->y + ((corner & 2) ? xf->height : 0), point) != PC_OK)
            return pc_fail(err, PC_ERR_INVALID, "invalid crop viewport");
        for (size_t a = 0; a < 2; a++) {
            lo[a] = fmin(lo[a], point[a]);
            hi[a] = fmax(hi[a], point[a]);
        }
    }
    if (lo[0] < -1000000 || lo[1] < -1000000 || hi[0] > 1000000 || hi[1] > 1000000)
        return pc_fail(err, PC_ERR_LIMIT, "crop viewport exceeds the coordinate budget");
    pc_rect area =
        pc_rect_make((int32_t)(floor(lo[0] / 256) * 256 - 256), (int32_t)(floor(lo[1] / 256) * 256 - 256),
                     (int32_t)((floor(hi[0] / 256) + 2) * 256), (int32_t)((floor(hi[1] / 256) + 2) * 256));
    const pc_document *doc = pc_doc_arc_get(state->doc);
    uint32_t width = pc_rect_width(area), height = pc_rect_height(area),
             longest = width > height ? width : height;
    uint32_t k = longest / 8192;
    bool proxy = k >= 2 && pc_compose_proxy_faithful(doc);
    if (proxy) {
        area =
            pc_rect_make((int32_t)(floor((double)area.x0 / k) * k), (int32_t)(floor((double)area.y0 / k) * k),
                         (int32_t)(ceil((double)area.x1 / k) * k), (int32_t)(ceil((double)area.y1 / k) * k));
        width = pc_rect_width(area);
        height = pc_rect_height(area);
        longest = width > height ? width : height;
    }
    if (c->valid && c->region && c->document == doc->id && c->revision == state->revision &&
        c->render_key == pc_ui_canvas_render_key() && pc_rect_eq(c->source_rect, area))
        return PC_OK;
    pc_compose_buffer image = {0};
    pc_status status;
    if (longest <= 4096)
        status = pc_ui_canvas_compose(doc, area, gpu, &image, err);
    else {
        double factor = 4096. / longest;
        uint32_t w = (uint32_t)fmax(1, round(width * factor)), h = (uint32_t)fmax(1, round(height * factor));
        if (proxy) {
            pc_document reduced_doc = {0};
            status = pc_compose_proxy_document(doc, k, &reduced_doc, err);
            if (status == PC_OK) {
                pc_rect reduced = pc_rect_make(area.x0 / (int32_t)k, area.y0 / (int32_t)k,
                                               area.x1 / (int32_t)k, area.y1 / (int32_t)k);
                status = pc_compose_render_reduced_rect(&reduced_doc, reduced, w, h, &image, err);
                pc_document_fini(&reduced_doc);
            }
        } else if ((uint64_t)width * height > PC_COMPOSE_MAX_PIXELS)
            return pc_fail(err, PC_ERR_LIMIT, "crop viewport reduction exceeds the pixel budget");
        else
            status = pc_compose_render_reduced_rect(doc, area, w, h, &image, err);
    }
    if (status != PC_OK)
        return status;
    premultiply(&image);
    pc_compose_buffer_fini(&c->image);
    c->image = image;
    c->document = doc->id;
    c->revision = state->revision;
    c->render_key = pc_ui_canvas_render_key();
    c->generation++;
    c->source_rect = area;
    c->last_render = area;
    c->partial = false;
    c->region = true;
    c->valid = true;
    return PC_OK;
}

pc_status pc_ui_canvas_gesture(pc_ui_view *view, const pc_ui_canvas_xform *xf, const float from[2],
                               const float to[2], float zoom_factor, float rotation_delta, float ppp)
{
    if (!view || !from || !to || !valid_xform(xf) || !isfinite(ppp) || ppp <= 0 || !isfinite(zoom_factor) ||
        zoom_factor <= 0 || !isfinite(rotation_delta)) {
        return PC_ERR_INVALID;
    }
    float before[2], after[2];
    if (pc_ui_canvas_to_doc(xf, from[0], from[1], before) != PC_OK) {
        return PC_ERR_INVALID;
    }
    pc_ui_view next = *view;
    next.zoom = pc_ui_zoom_clamp(pc_f64_to_f32((double)view->zoom * zoom_factor), view->doc_size);
    next.rotation = pc_ui_rotation_wrap(view->rotation + (xf->flip ? -rotation_delta : rotation_delta));
    pc_ui_canvas_xform updated = *xf;
    updated.zoom = next.zoom / ppp;
    updated.rotation = next.rotation;
    if (pc_ui_canvas_to_doc(&updated, to[0], to[1], after) != PC_OK) {
        return PC_ERR_INVALID;
    }
    next.center[0] += before[0] - after[0];
    next.center[1] += before[1] - after[1];
    if (!isfinite(next.center[0]) || !isfinite(next.center[1])) {
        return PC_ERR_INVALID;
    }
    next.fit_pending = next.fill_pending = false;
    *view = next;
    return PC_OK;
}

bool pc_ui_canvas_clip_line(const pc_ui_canvas_xform *xf, float a[2], float b[2])
{
    if (!xf || !a || !b || !isfinite(xf->x) || !isfinite(xf->y) || !isfinite(xf->width) ||
        !isfinite(xf->height) || xf->width <= 0 || xf->height <= 0)
        return false;
    double lo[2] = {fmax(-32760, xf->x), fmax(-32760, xf->y)},
           hi[2] = {fmin(32760, (double)xf->x + xf->width), fmin(32760, (double)xf->y + xf->height)};
    if (hi[0] < lo[0] || hi[1] < lo[1])
        return false;
    double t0 = 0, t1 = 1, start[2] = {a[0], a[1]}, delta[2] = {(double)b[0] - a[0], (double)b[1] - a[1]};
    for (size_t axis = 0; axis < 2; axis++) {
        if (!isfinite(start[axis]) || !isfinite(delta[axis]))
            return false;
        if (delta[axis] == 0) {
            if (start[axis] < lo[axis] || start[axis] > hi[axis])
                return false;
        } else {
            double u = (lo[axis] - start[axis]) / delta[axis], v = (hi[axis] - start[axis]) / delta[axis];
            t0 = fmax(t0, fmin(u, v));
            t1 = fmin(t1, fmax(u, v));
        }
    }
    if (t0 > t1)
        return false;
    for (size_t axis = 0; axis < 2; axis++) {
        a[axis] = (float)fmin(hi[axis], fmax(lo[axis], start[axis] + t0 * delta[axis]));
        b[axis] = (float)fmin(hi[axis], fmax(lo[axis], start[axis] + t1 * delta[axis]));
    }
    return true;
}
