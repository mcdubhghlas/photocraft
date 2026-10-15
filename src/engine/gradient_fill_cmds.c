/* Live gradients: the Gradient tool's non-destructive mode -- a Gradient Fill layer whose start/end
 * points, colour stops, opacity stops and midpoints stay editable (port of
 * crates/engine/src/gradient_fill_cmds.rs, E10). Also gradient.presets.apply (a new Gradient Fill
 * layer from a panel preset; E2b-data left this to E10).
 *
 * A live gradient is laid out on the canvas ("Align with layer" off). Its start/end points are
 * stored as the fill's angle, scale and centre offset (compose/gradient_fill), which is what a PSD
 * Gradient Fill holds, and it renders the same pixels as painting the same drag with
 * paint.gradient. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/json.h"
#include "base/mem.h"
#include "color/blend.h"
#include "color/color.h"
#include "compose/compose.h"
#include "compose/gradient_fill.h"
#include "doc/doc.h"
#include "doc/effects.h"
#include "doc/fill.h"
#include "geom/geom.h"
#include "presets.h"

#include "brush_cmds.h"
#include "engine_internal.h"
#include "layer_cmds.h"

#define CMD_CREATE "gradient.fill.create"
#define CMD_SET "gradient.fill.set"
#define CMD_STOP "gradient.fill.stop"
#define CMD_GET "gradient.fill.get"

#define GF_MAX_COORD 1.0e7f
#define GF_SCALE_MIN 0.1f
#define GF_SCALE_MAX 1.5f

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
static float clamp_scale(float v)
{
    return clampf(v, GF_SCALE_MIN, GF_SCALE_MAX);
}

static bool num_opt(yyjson_val *p, const char *k, float *out)
{
    double v;
    yyjson_val *x = pc_json_get(p, k);
    if (x != NULL && pc_json_as_f64(x, &v) && isfinite(v)) {
        *out = (float)v;
        return true;
    }
    return false;
}

/* `k` = [x, y] with finite coords within +-MAX_COORD. r: 1 present+ok, 0 absent, -1 error. */
static int point_param(const char *cmd, yyjson_val *p, const char *k, float out[2], pc_error *err)
{
    yyjson_val *v = pc_json_get(p, k);
    if (v == NULL) {
        return 0;
    }
    if (!yyjson_is_arr(v) || yyjson_arr_size(v) != 2) {
        (void)pc_engine_err_bad_params(err, cmd, "`%s` is [x, y]", k);
        return -1;
    }
    double x, y;
    if (!pc_json_as_f64(yyjson_arr_get(v, 0), &x) || !pc_json_as_f64(yyjson_arr_get(v, 1), &y) ||
        !isfinite(x) || !isfinite(y) || fabs(x) > GF_MAX_COORD || fabs(y) > GF_MAX_COORD) {
        (void)pc_engine_err_bad_params(err, cmd, "`%s` needs finite coordinates within +-%g", k, GF_MAX_COORD);
        return -1;
    }
    out[0] = (float)x;
    out[1] = (float)y;
    return 1;
}

/* r: 1 present+ok, 0 absent/null, -1 error. */
static int style_param(const char *cmd, yyjson_val *p, pc_gradient_style *out, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, "style");
    if (v == NULL || yyjson_is_null(v)) {
        return 0;
    }
    size_t len = 0;
    const char *s = pc_json_as_str(v, &len);
    if (s == NULL) {
        (void)pc_engine_err_bad_params(err, cmd, "`style` is a string");
        return -1;
    }
    char b[16];
    if (len >= sizeof b) {
        (void)pc_engine_err_bad_params(err, cmd, "unknown style");
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        b[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    b[len] = '\0';
    if (strcmp(b, "linear") == 0) {
        *out = PC_GRADIENT_LINEAR;
    } else if (strcmp(b, "radial") == 0) {
        *out = PC_GRADIENT_RADIAL;
    } else if (strcmp(b, "angle") == 0) {
        *out = PC_GRADIENT_ANGLE;
    } else if (strcmp(b, "reflected") == 0) {
        *out = PC_GRADIENT_REFLECTED;
    } else if (strcmp(b, "diamond") == 0) {
        *out = PC_GRADIENT_DIAMOND;
    } else {
        (void)pc_engine_err_bad_params(err, cmd, "unknown style `%s` (linear|radial|angle|reflected|diamond)", b);
        return -1;
    }
    return 1;
}

static const char *style_name(pc_gradient_style s)
{
    switch (s) {
    case PC_GRADIENT_RADIAL:
        return "radial";
    case PC_GRADIENT_ANGLE:
        return "angle";
    case PC_GRADIENT_REFLECTED:
        return "reflected";
    case PC_GRADIENT_DIAMOND:
        return "diamond";
    default:
        return "linear";
    }
}

/* A stop colour: "#rrggbb[aa]", [r,g,b,a?], "foreground" or "background". */
static bool parse_color(yyjson_val *v, const float fg[4], const float bg[4], pc_color *out)
{
    float c[4];
    size_t len = 0;
    const char *s = v != NULL ? pc_json_as_str(v, &len) : NULL;
    if (s != NULL) {
        if (len == 10 && memcmp(s, "foreground", 10) == 0) {
            memcpy(c, fg, sizeof c);
        } else if (len == 10 && memcmp(s, "background", 10) == 0) {
            memcpy(c, bg, sizeof c);
        } else {
            const char *h = s;
            size_t hl = len;
            if (hl > 0 && h[0] == '#') {
                h++;
                hl--;
            }
            if (hl != 6 && hl != 8) {
                return false;
            }
            char buf[10];
            buf[0] = '#';
            memcpy(buf + 1, h, hl);
            buf[hl + 1] = '\0';
            if (!pc_engine_parse_hex(buf, hl + 1, c)) {
                return false;
            }
        }
    } else if (v != NULL && yyjson_is_arr(v) && yyjson_arr_size(v) >= 3 && yyjson_arr_size(v) <= 4) {
        size_t n = yyjson_arr_size(v);
        double d;
        for (size_t i = 0; i < 4; i++) {
            if (i < n) {
                yyjson_val *e = yyjson_arr_get(v, i);
                if (!pc_json_as_f64(e, &d) || !isfinite(d)) {
                    return false;
                }
                c[i] = (float)d;
            } else {
                c[i] = 1.0f;
            }
        }
    } else {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        c[i] = clampf(c[i], 0.0f, 1.0f);
    }
    *out = pc_color_rgba(c[0], c[1], c[2], c[3]);
    return true;
}

static void hex_of(const pc_color *c, char out[10])
{
    float rgb[3];
    pc_color_to_rgb(c, rgb);
    unsigned r = (unsigned)(clampf(rgb[0], 0, 1) * 255.0f + 0.5f);
    unsigned g = (unsigned)(clampf(rgb[1], 0, 1) * 255.0f + 0.5f);
    unsigned b = (unsigned)(clampf(rgb[2], 0, 1) * 255.0f + 0.5f);
    if (c->alpha >= 1.0f) {
        snprintf(out, 10, "#%02x%02x%02x", r, g, b);
    } else {
        unsigned a = (unsigned)(clampf(c->alpha, 0, 1) * 255.0f + 0.5f);
        snprintf(out, 10, "#%02x%02x%02x%02x", r, g, b, a);
    }
}

/* ========================================================================================== */
/* gradient array helpers                                                                      */
/* ========================================================================================== */

static pc_status set_color_stops(pc_fill *f, pc_color_stop *arr, size_t n)
{
    free(f->u.gradient.stops);
    f->u.gradient.stops = arr;
    f->u.gradient.nstops = n;
    return PC_OK;
}
static void set_opacity_stops(pc_fill *f, pc_opacity_stop *arr, size_t n)
{
    free(f->u.gradient.opacity_stops);
    f->u.gradient.opacity_stops = arr;
    f->u.gradient.nopacity_stops = n;
}
static void set_midpoints(pc_fill *f, float *arr, size_t n)
{
    free(f->u.gradient.midpoints);
    f->u.gradient.midpoints = arr;
    f->u.gradient.nmidpoints = n;
}

static int cmp_color_stop(const void *a, const void *b)
{
    float x = ((const pc_color_stop *)a)->pos, y = ((const pc_color_stop *)b)->pos;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static int cmp_opacity_stop(const void *a, const void *b)
{
    float x = ((const pc_opacity_stop *)a)->pos, y = ((const pc_opacity_stop *)b)->pos;
    return x < y ? -1 : (x > y ? 1 : 0);
}

/* ========================================================================================== */
/* enabled predicates and gradient fill lookup                                                 */
/* ========================================================================================== */

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_has_doc(s, reason);
}

static pc_status has_gradient_fill(const pc_engine_session *s, pc_error *reason)
{
    const pc_layer *l = pc_engine_active_layer_of(s, reason);
    if (l == NULL) {
        return PC_ERR_INVALID;
    }
    if (l->content.kind == PC_LAYER_FILL && l->content.u.fill.kind == PC_FILL_GRADIENT) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "the active layer is not a gradient fill layer");
}

/* Clone the gradient fill of layer id into *out and its frame into *frame. */
static pc_status gradient_of(pc_engine_session *s, pc_layer_id id, const char *cmd, pc_fill *out,
                             pc_rect *frame, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (l->content.kind != PC_LAYER_FILL || l->content.u.fill.kind != PC_FILL_GRADIENT) {
        return pc_engine_err_bad_params(err, cmd, "layer \"%s\" is not a gradient fill layer",
                                        pc_str_c(&l->name));
    }
    if (pc_fill_clone(&l->content.u.fill, out, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    *frame = pc_compose_fill_frame(l, pc_document_bounds(doc));
    return PC_OK;
}

static pc_status check_pixels_unlocked(const pc_document *doc, pc_layer_id id, pc_error *err)
{
    pc_locks locks = pc_document_effective_locks(doc, id);
    if (locks.pixels || locks.all) {
        const pc_layer *l = pc_document_layer(doc, id);
        return pc_engine_err_other(err, "Could not complete your request because the layer \"%s\" is locked",
                                   l != NULL ? pc_str_c(&l->name) : "");
    }
    return PC_OK;
}

/* The frame a gradient fill `f` (with its align) is laid out in for `layer`. */
static pc_rect frame_with_align(const pc_layer *layer, const pc_fill *f, pc_rect canvas, pc_error *err)
{
    pc_layer tmp;
    if (pc_layer_clone(layer, &tmp, err) != PC_OK) {
        return canvas;
    }
    pc_fill fc;
    if (pc_fill_clone(f, &fc, err) != PC_OK) {
        pc_layer_fini(&tmp);
        return canvas;
    }
    if (tmp.content.kind == PC_LAYER_FILL) {
        pc_fill_fini(&tmp.content.u.fill);
    } else {
        pc_layer_content_fini(&tmp.content);
        memset(&tmp.content, 0, sizeof tmp.content);
        tmp.content.kind = PC_LAYER_FILL;
    }
    tmp.content.kind = PC_LAYER_FILL;
    tmp.content.u.fill = fc;
    pc_rect r = pc_compose_fill_frame(&tmp, canvas);
    pc_layer_fini(&tmp);
    return r;
}

/* ========================================================================================== */
/* gradient.fill.set                                                                           */
/* ========================================================================================== */

/* Applies gradient.fill.set params to a gradient fill (layer gives the frame; fg/bg resolve stop
 * colours). On error returns != PC_OK and leaves *out finalised internally. On success *out holds
 * the new fill (finalise with pc_fill_fini). */
static pc_status apply_set(const pc_layer *layer, const pc_fill *f, pc_rect canvas, yyjson_val *p,
                           const float fg[4], const float bg[4], pc_fill *out, pc_error *err)
{
    const char *cmd = CMD_SET;
    if (pc_fill_clone(f, out, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (out->kind != PC_FILL_GRADIENT) {
        pc_fill_fini(out);
        return pc_engine_err_bad_params(err, cmd, "not a gradient fill");
    }
    pc_rect old_frame = frame_with_align(layer, f, canvas, err);
    float old_from[2], old_to[2];
    pc_compose_gradient_handles(out->u.gradient.style, out->u.gradient.angle, out->u.gradient.scale,
                                out->u.gradient.offset, old_frame, old_from, old_to);

    yyjson_val *av = pc_json_get(p, "align");
    if (av != NULL) {
        bool b;
        if (!pc_json_as_bool(av, &b)) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`align` is a boolean");
        }
        out->u.gradient.align = b;
    }
    pc_gradient_style new_style = out->u.gradient.style;
    int sr = style_param(cmd, p, &new_style, err);
    if (sr < 0) {
        pc_fill_fini(out);
        return PC_ERR_INVALID;
    }
    bool has_new_style = sr == 1;
    bool explicit_geometry = pc_json_get(p, "angle") != NULL || pc_json_get(p, "scale") != NULL ||
                             pc_json_get(p, "offset") != NULL;
    yyjson_val *angv = pc_json_get(p, "angle");
    if (angv != NULL) {
        double a;
        if (!pc_json_as_f64(angv, &a) || !isfinite(a)) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`angle` is a number (degrees)");
        }
        float ang = (float)fmod(a, 360.0);
        if (ang < 0.0f) {
            ang += 360.0f;
        }
        if (ang > 180.0f) {
            ang -= 360.0f;
        }
        out->u.gradient.angle = ang;
    }
    if (pc_json_get(p, "scale") != NULL) {
        float v;
        if (!num_opt(p, "scale", &v) || !(v > 0.0f)) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`scale` is a positive percentage");
        }
        out->u.gradient.scale = clamp_scale(v / 100.0f);
    }
    yyjson_val *ov = pc_json_get(p, "offset");
    if (ov != NULL) {
        if (!yyjson_is_arr(ov) || yyjson_arr_size(ov) != 2) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`offset` is [x%%, y%%]");
        }
        double x, y;
        if (!pc_json_as_f64(yyjson_arr_get(ov, 0), &x) || !pc_json_as_f64(yyjson_arr_get(ov, 1), &y) ||
            !isfinite(x) || !isfinite(y) || fabs(x) > 1.0e5 || fabs(y) > 1.0e5) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`offset` needs finite percentages");
        }
        out->u.gradient.offset[0] = (float)(x / 100.0);
        out->u.gradient.offset[1] = (float)(y / 100.0);
    }
    yyjson_val *rv = pc_json_get(p, "reverse");
    if (rv != NULL) {
        bool b;
        if (!pc_json_as_bool(rv, &b)) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`reverse` is a boolean");
        }
        out->u.gradient.reverse = b;
    }
    yyjson_val *dv = pc_json_get(p, "dither");
    if (dv != NULL) {
        bool b;
        if (!pc_json_as_bool(dv, &b)) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`dither` is a boolean");
        }
        out->u.gradient.dither = b;
    }
    float from[2], to[2];
    int fr = point_param(cmd, p, "from", from, err);
    if (fr < 0) {
        pc_fill_fini(out);
        return PC_ERR_INVALID;
    }
    int tr = point_param(cmd, p, "to", to, err);
    if (tr < 0) {
        pc_fill_fini(out);
        return PC_ERR_INVALID;
    }
    /* Frame for the current geometry (stops empty: just layout). */
    pc_fill tmpf;
    memset(&tmpf, 0, sizeof tmpf);
    tmpf.kind = PC_FILL_GRADIENT;
    tmpf.u.gradient.angle = out->u.gradient.angle;
    tmpf.u.gradient.scale = out->u.gradient.scale;
    tmpf.u.gradient.style = out->u.gradient.style;
    tmpf.u.gradient.reverse = out->u.gradient.reverse;
    tmpf.u.gradient.offset[0] = out->u.gradient.offset[0];
    tmpf.u.gradient.offset[1] = out->u.gradient.offset[1];
    tmpf.u.gradient.dither = out->u.gradient.dither;
    tmpf.u.gradient.align = out->u.gradient.align;
    pc_rect frame = frame_with_align(layer, &tmpf, canvas, err);

    bool style_changed = has_new_style && new_style != out->u.gradient.style;
    if (has_new_style) {
        out->u.gradient.style = new_style;
    }
    if (fr == 1 || tr == 1 || (style_changed && !explicit_geometry)) {
        float cur0[2], cur1[2];
        if (fr == 1 || tr == 1) {
            pc_compose_gradient_handles(out->u.gradient.style, out->u.gradient.angle, out->u.gradient.scale,
                                        out->u.gradient.offset, frame, cur0, cur1);
        } else {
            memcpy(cur0, old_from, sizeof cur0);
            memcpy(cur1, old_to, sizeof cur1);
        }
        float f0[2] = {fr == 1 ? from[0] : cur0[0], fr == 1 ? from[1] : cur0[1]};
        float t0[2] = {tr == 1 ? to[0] : cur1[0], tr == 1 ? to[1] : cur1[1]};
        float a, sc, off[2];
        pc_compose_gradient_from_handles(out->u.gradient.style, f0, t0, frame, out->u.gradient.angle, &a,
                                         &sc, off);
        out->u.gradient.angle = a;
        out->u.gradient.scale = clamp_scale(sc);
        out->u.gradient.offset[0] = off[0];
        out->u.gradient.offset[1] = off[1];
        /* Past the scale range the dragged handle stops; the other stays put (#1243). */
        bool anchor = false;
        bool is_start = false;
        float pin[2] = {0, 0};
        if (fr == 1 && tr != 1) {
            anchor = true;
            is_start = false;
            memcpy(pin, t0, sizeof pin);
        } else if (fr != 1 && tr == 1) {
            anchor = true;
            is_start = true;
            memcpy(pin, f0, sizeof pin);
        }
        if (anchor && out->u.gradient.scale != sc) {
            float f1[2], t1[2];
            pc_compose_gradient_handles(out->u.gradient.style, out->u.gradient.angle, out->u.gradient.scale,
                                        out->u.gradient.offset, frame, f1, t1);
            const float *at = is_start ? f1 : t1;
            float dx = pin[0] - at[0], dy = pin[1] - at[1];
            float w = (float)(pc_rect_width(frame) > 0 ? pc_rect_width(frame) : 1);
            float h = (float)(pc_rect_height(frame) > 0 ? pc_rect_height(frame) : 1);
            out->u.gradient.offset[0] += dx / w;
            out->u.gradient.offset[1] += dy / h;
        }
    }
    /* stops */
    yyjson_val *sv = pc_json_get(p, "stops");
    if (sv != NULL) {
        if (!yyjson_is_arr(sv)) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`stops` is [[location, colour], ...]");
        }
        size_t cnt = yyjson_arr_size(sv);
        pc_color_stop *arr = cnt > 0 ? pc_malloc_array(cnt, sizeof *arr) : NULL;
        if (cnt > 0 && arr == NULL) {
            pc_fill_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "gradient");
        }
        size_t w = 0, idx, max;
        yyjson_val *el;
        yyjson_arr_foreach (sv, idx, max, el) {
            if (!yyjson_is_arr(el) || yyjson_arr_size(el) != 2) {
                free(arr);
                pc_fill_fini(out);
                return pc_engine_err_bad_params(err, cmd, "a stop is [location 0..1, colour]");
            }
            double t;
            if (!pc_json_as_f64(yyjson_arr_get(el, 0), &t) || !isfinite(t)) {
                free(arr);
                pc_fill_fini(out);
                return pc_engine_err_bad_params(err, cmd, "stop location is a number");
            }
            pc_color col;
            if (!parse_color(yyjson_arr_get(el, 1), fg, bg, &col)) {
                free(arr);
                pc_fill_fini(out);
                return pc_engine_err_bad_params(
                    err, cmd, "stop colour is \"#rrggbb\", [r,g,b,a], \"foreground\" or \"background\"");
            }
            arr[w].pos = clampf((float)t, 0.0f, 1.0f);
            arr[w].color = col;
            w++;
        }
        if (w < 2) {
            free(arr);
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "a gradient needs at least 2 colour stops");
        }
        if (w > 1024) {
            free(arr);
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "too many colour stops (max 1024)");
        }
        qsort(arr, w, sizeof *arr, cmp_color_stop);
        if (w != out->u.gradient.nstops) {
            set_midpoints(out, NULL, 0);
        }
        set_color_stops(out, arr, w);
    }
    /* transparency */
    {
        pc_gradient_preset gp = {0};
        pc_status rc = pc_gradient_with_transparency(&gp, p, cmd, err);
        if (rc != PC_OK) {
            pc_gradient_preset_fini(&gp);
            pc_fill_fini(out);
            return rc;
        }
        if (gp.nopacity > 0 || pc_json_get(p, "transparency") != NULL) {
            /* transparency present (possibly empty): replace. */
            if (pc_json_get(p, "transparency") != NULL) {
                pc_opacity_stop *arr = gp.nopacity > 0 ? pc_malloc_array(gp.nopacity, sizeof *arr) : NULL;
                if (gp.nopacity > 0 && arr == NULL) {
                    pc_gradient_preset_fini(&gp);
                    pc_fill_fini(out);
                    return pc_fail(err, PC_ERR_NOMEM, "gradient");
                }
                for (size_t i = 0; i < gp.nopacity; i++) {
                    arr[i].pos = gp.opacity[i].loc;
                    arr[i].opacity = gp.opacity[i].opacity;
                }
                set_opacity_stops(out, arr, gp.nopacity);
            }
        }
        pc_gradient_preset_fini(&gp);
    }
    /* midpoints */
    yyjson_val *mv = pc_json_get(p, "midpoints");
    if (mv != NULL) {
        if (!yyjson_is_arr(mv)) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "`midpoints` is [0..1, ...] (one per segment)");
        }
        size_t cnt = yyjson_arr_size(mv);
        size_t segs = out->u.gradient.nstops > 0 ? out->u.gradient.nstops - 1 : 0;
        if (cnt > segs) {
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "%zu midpoints for %zu segments", cnt, segs);
        }
        float *arr = cnt > 0 ? pc_malloc_array(cnt, sizeof *arr) : NULL;
        if (cnt > 0 && arr == NULL) {
            pc_fill_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "gradient");
        }
        size_t idx, max;
        yyjson_val *el;
        size_t w = 0;
        yyjson_arr_foreach (mv, idx, max, el) {
            double m;
            if (!pc_json_as_f64(el, &m) || !isfinite(m)) {
                free(arr);
                pc_fill_fini(out);
                return pc_engine_err_bad_params(err, cmd, "midpoints are numbers");
            }
            arr[w++] = clampf((float)m, 0.05f, 0.95f);
        }
        set_midpoints(out, arr, cnt);
    }
    return PC_OK;
}

/* ========================================================================================== */
/* gradient.fill.stop                                                                          */
/* ========================================================================================== */

static bool param_index(yyjson_val *p, const char *cmd, size_t len, const char *what, size_t *out,
                        pc_error *err)
{
    uint64_t u;
    yyjson_val *v = pc_json_get(p, "index");
    if (v == NULL || !pc_json_as_u64(v, &u)) {
        (void)pc_engine_err_bad_params(err, cmd, "missing `index` (a stop number from 0)");
        return false;
    }
    if (u >= len) {
        (void)pc_engine_err_bad_params(err, cmd, "no %s %llu (there are %zu)", what, (unsigned long long)u, len);
        return false;
    }
    *out = (size_t)u;
    return true;
}

static bool param_location(yyjson_val *p, const char *cmd, float *out, pc_error *err)
{
    float v;
    if (!num_opt(p, "location", &v)) {
        (void)pc_engine_err_bad_params(err, cmd, "missing `location` (0..1)");
        return false;
    }
    *out = clampf(v, 0.0f, 1.0f);
    return true;
}

/* The colour stops' own alpha at t (without opacity stops), via a bare ramp. */
static float ramp_color_alpha(const pc_fill *f, float t)
{
    pc_fill bare;
    memset(&bare, 0, sizeof bare);
    bare.kind = PC_FILL_GRADIENT;
    bare.u.gradient.style = PC_GRADIENT_LINEAR;
    bare.u.gradient.scale = 1.0f;
    bare.u.gradient.align = true;
    bare.u.gradient.stops = f->u.gradient.stops;
    bare.u.gradient.nstops = f->u.gradient.nstops;
    bare.u.gradient.midpoints = f->u.gradient.midpoints;
    bare.u.gradient.nmidpoints = f->u.gradient.nmidpoints;
    pc_compose_ramp ramp;
    bool found = false;
    if (pc_compose_ramp_init(&bare, &found, &ramp, NULL) != PC_OK || !found) {
        return 1.0f;
    }
    float out[5];
    pc_compose_ramp_sample(&ramp, t, out);
    pc_compose_ramp_fini(&ramp);
    float alpha = out[3]; /* RGBA ramp: alpha at index 3 */
    return alpha > 1e-6f ? alpha : 1e-6f;
}

/* paired (loc, color, mid) sorted by loc. */
struct trip {
    float pos;
    pc_color color;
    float mid;
};
static int cmp_trip(const void *a, const void *b)
{
    float x = ((const struct trip *)a)->pos, y = ((const struct trip *)b)->pos;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static pc_status apply_stop(const pc_fill *f, yyjson_val *p, const float fg[4], const float bg[4],
                            pc_fill *out, pc_error *err)
{
    const char *cmd = CMD_STOP;
    if (pc_fill_clone(f, out, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    if (out->kind != PC_FILL_GRADIENT) {
        pc_fill_fini(out);
        return pc_engine_err_bad_params(err, cmd, "not a gradient fill");
    }
    size_t alen = 0;
    const char *action = pc_json_as_str(pc_json_get(p, "action"), &alen);
    if (action == NULL) {
        pc_fill_fini(out);
        return pc_engine_err_bad_params(err, cmd, "missing `action` (add|move|delete|color|opacity|midpoint)");
    }
    size_t klen = 0;
    const char *kind = pc_json_as_str(pc_json_get(p, "kind"), &klen);
    bool opacity_kind;
    if (kind == NULL || (klen == 5 && memcmp(kind, "color", 5) == 0)) {
        opacity_kind = false;
    } else if (klen == 7 && memcmp(kind, "opacity", 7) == 0) {
        opacity_kind = true;
    } else {
        pc_fill_fini(out);
        return pc_engine_err_bad_params(err, cmd, "unknown kind `%.*s` (color|opacity)", (int)klen, kind);
    }
#define ACT(str) (alen == strlen(str) && memcmp(action, str, alen) == 0)
    if (opacity_kind) {
        size_t n = out->u.gradient.nopacity_stops;
        pc_opacity_stop *os = pc_malloc_array(n + 2 > 0 ? n + 2 : 1, sizeof *os);
        if (os == NULL) {
            pc_fill_fini(out);
            return pc_fail(err, PC_ERR_NOMEM, "gradient");
        }
        size_t on = 0;
        if (n == 0) {
            os[0].pos = 0.0f;
            os[0].opacity = 1.0f;
            os[1].pos = 1.0f;
            os[1].opacity = 1.0f;
            on = 2;
        } else {
            for (size_t i = 0; i < n; i++) {
                os[i] = out->u.gradient.opacity_stops[i];
            }
            on = n;
        }
        qsort(os, on, sizeof *os, cmp_opacity_stop);
        pc_status rc = PC_OK;
        if (ACT("add")) {
            float t;
            if (!param_location(p, cmd, &t, err)) {
                rc = PC_ERR_INVALID;
            } else {
                float a;
                float v;
                if (num_opt(p, "opacity", &v)) {
                    a = clampf(v / 100.0f, 0.0f, 1.0f);
                } else {
                    pc_compose_ramp ramp;
                    bool found = false;
                    float samp[5] = {0, 0, 0, 1, 0};
                    if (pc_compose_ramp_init(f, &found, &ramp, NULL) == PC_OK && found) {
                        pc_compose_ramp_sample(&ramp, t, samp);
                        pc_compose_ramp_fini(&ramp);
                    }
                    a = clampf(samp[3] / ramp_color_alpha(f, t), 0.0f, 1.0f);
                }
                os[on].pos = t;
                os[on].opacity = a;
                on++;
            }
        } else if (ACT("move")) {
            size_t i;
            float t;
            if (!param_index(p, cmd, on, "opacity stop", &i, err) || !param_location(p, cmd, &t, err)) {
                rc = PC_ERR_INVALID;
            } else {
                os[i].pos = t;
            }
        } else if (ACT("delete")) {
            size_t i;
            if (!param_index(p, cmd, on, "opacity stop", &i, err)) {
                rc = PC_ERR_INVALID;
            } else if (on <= 2) {
                (void)pc_engine_err_bad_params(err, cmd, "a gradient keeps at least 2 opacity stops");
                rc = PC_ERR_INVALID;
            } else {
                memmove(&os[i], &os[i + 1], (on - i - 1) * sizeof *os);
                on--;
            }
        } else if (ACT("opacity")) {
            size_t i;
            float v;
            if (!param_index(p, cmd, on, "opacity stop", &i, err)) {
                rc = PC_ERR_INVALID;
            } else if (!num_opt(p, "opacity", &v)) {
                (void)pc_engine_err_bad_params(err, cmd, "missing `opacity` (0..100)");
                rc = PC_ERR_INVALID;
            } else {
                os[i].opacity = clampf(v / 100.0f, 0.0f, 1.0f);
            }
        } else {
            (void)pc_engine_err_bad_params(err, cmd,
                                           "unknown action `%.*s` for opacity stops (add|move|delete|opacity)",
                                           (int)alen, action);
            rc = PC_ERR_INVALID;
        }
        if (rc != PC_OK) {
            free(os);
            pc_fill_fini(out);
            return rc;
        }
        qsort(os, on, sizeof *os, cmp_opacity_stop);
        if (on > 1024) {
            free(os);
            pc_fill_fini(out);
            return pc_engine_err_bad_params(err, cmd, "too many opacity stops (max 1024)");
        }
        set_opacity_stops(out, os, on);
        return PC_OK;
    }
    /* colour stops: build paired (loc, color, mid). */
    size_t ns = out->u.gradient.nstops;
    struct trip *v = pc_malloc_array(ns + 2 > 0 ? ns + 2 : 1, sizeof *v);
    if (v == NULL) {
        pc_fill_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    for (size_t i = 0; i < ns; i++) {
        v[i].pos = out->u.gradient.stops[i].pos;
        v[i].color = out->u.gradient.stops[i].color;
        v[i].mid = i < out->u.gradient.nmidpoints ? out->u.gradient.midpoints[i] : 0.5f;
    }
    size_t vn = ns;
    qsort(v, vn, sizeof *v, cmp_trip);
    pc_status rc = PC_OK;
    if (ACT("add")) {
        float t;
        if (!param_location(p, cmd, &t, err)) {
            rc = PC_ERR_INVALID;
        } else {
            pc_color c;
            yyjson_val *cv = pc_json_get(p, "color");
            if (cv != NULL) {
                if (!parse_color(cv, fg, bg, &c)) {
                    (void)pc_engine_err_bad_params(err, cmd, "bad `color`");
                    rc = PC_ERR_INVALID;
                }
            } else {
                pc_compose_ramp ramp;
                bool found = false;
                float samp[5] = {0, 0, 0, 1, 0};
                if (pc_compose_ramp_init(f, &found, &ramp, NULL) == PC_OK && found) {
                    pc_compose_ramp_sample(&ramp, t, samp);
                    pc_compose_ramp_fini(&ramp);
                }
                c = pc_color_rgba(samp[0], samp[1], samp[2], ramp_color_alpha(f, t));
            }
            if (rc == PC_OK) {
                /* splitting a segment resets its midpoint (the last stop with pos <= t). */
                for (size_t i = vn; i-- > 0;) {
                    if (v[i].pos <= t) {
                        v[i].mid = 0.5f;
                        break;
                    }
                }
                v[vn].pos = t;
                v[vn].color = c;
                v[vn].mid = 0.5f;
                vn++;
                if (vn > 1024) {
                    (void)pc_engine_err_bad_params(err, cmd, "too many colour stops (max 1024)");
                    rc = PC_ERR_INVALID;
                }
            }
        }
    } else if (ACT("move")) {
        size_t i;
        float t;
        if (!param_index(p, cmd, vn, "colour stop", &i, err) || !param_location(p, cmd, &t, err)) {
            rc = PC_ERR_INVALID;
        } else {
            v[i].pos = t;
        }
    } else if (ACT("delete")) {
        size_t i;
        if (!param_index(p, cmd, vn, "colour stop", &i, err)) {
            rc = PC_ERR_INVALID;
        } else if (vn <= 2) {
            (void)pc_engine_err_bad_params(err, cmd, "a gradient keeps at least 2 colour stops");
            rc = PC_ERR_INVALID;
        } else {
            memmove(&v[i], &v[i + 1], (vn - i - 1) * sizeof *v);
            vn--;
        }
    } else if (ACT("color")) {
        size_t i;
        pc_color c;
        yyjson_val *cv = pc_json_get(p, "color");
        if (!param_index(p, cmd, vn, "colour stop", &i, err)) {
            rc = PC_ERR_INVALID;
        } else if (cv == NULL || !parse_color(cv, fg, bg, &c)) {
            (void)pc_engine_err_bad_params(err, cmd, "missing or bad `color`");
            rc = PC_ERR_INVALID;
        } else {
            v[i].color = c;
        }
    } else if (ACT("midpoint")) {
        size_t segs = vn > 0 ? vn - 1 : 0;
        size_t i;
        float m;
        if (!param_index(p, cmd, segs, "segment", &i, err)) {
            rc = PC_ERR_INVALID;
        } else if (!num_opt(p, "location", &m)) {
            (void)pc_engine_err_bad_params(err, cmd, "missing `location` (0..1 of the segment)");
            rc = PC_ERR_INVALID;
        } else {
            v[i].mid = clampf(m, 0.05f, 0.95f);
        }
    } else {
        (void)pc_engine_err_bad_params(err, cmd, "unknown action `%.*s` (add|move|delete|color|midpoint)",
                                       (int)alen, action);
        rc = PC_ERR_INVALID;
    }
    if (rc != PC_OK) {
        free(v);
        pc_fill_fini(out);
        return rc;
    }
    qsort(v, vn, sizeof *v, cmp_trip);
    /* unpair: stops + midpoints (dropped if all 0.5). */
    pc_color_stop *cs = pc_malloc_array(vn > 0 ? vn : 1, sizeof *cs);
    size_t nmids = vn > 0 ? vn - 1 : 0;
    float *mids = nmids > 0 ? pc_malloc_array(nmids, sizeof *mids) : NULL;
    if (cs == NULL || (nmids > 0 && mids == NULL)) {
        free(v);
        free(cs);
        free(mids);
        pc_fill_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    bool all_half = true;
    for (size_t i = 0; i < vn; i++) {
        cs[i].pos = v[i].pos;
        cs[i].color = v[i].color;
        if (i < nmids) {
            mids[i] = v[i].mid;
            if (fabsf(v[i].mid - 0.5f) >= 1e-6f) {
                all_half = false;
            }
        }
    }
    free(v);
    set_color_stops(out, cs, vn);
    if (all_half) {
        free(mids);
        set_midpoints(out, NULL, 0);
    } else {
        set_midpoints(out, mids, nmids);
    }
    return PC_OK;
#undef ACT
}

/* ========================================================================================== */
/* create (gradient.fill.create) + gradient.presets.apply                                      */
/* ========================================================================================== */

/* preset_param: preset name / stops / current gradient, with transparency. *out finalised by caller. */
static pc_status preset_param(pc_engine_session *s, yyjson_val *p, const char *cmd, pc_gradient_preset *out,
                              pc_error *err)
{
    size_t glen = 0;
    const char *name = pc_json_as_str(pc_json_get(p, "gradient"), &glen);
    if (name != NULL) {
        size_t gi, ii;
        if (!pc_preset_find(&s->presets.gradients, name, NULL, pc_gradient_preset_ops(), &gi, &ii)) {
            return pc_engine_err_bad_params(err, cmd,
                                            "no gradient preset \"%.*s\" (see gradient.presets.list)",
                                            (int)glen, name);
        }
        const pc_gradient_preset *src =
            &((const pc_gradient_preset *)s->presets.gradients.v[gi].items)[ii];
        if (pc_gradient_preset_clone(src, out, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        return pc_gradient_with_transparency(out, p, cmd, err);
    }
    if (pc_json_get(p, "stops") != NULL) {
        return pc_gradient_from_params("Custom", p, cmd, out, err);
    }
    if (pc_gradient_preset_clone(&s->presets.gradient, out, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    return pc_gradient_with_transparency(out, p, cmd, err);
}

/* FillStops of a preset into owned pc_color_stop + pc_opacity_stop arrays. */
static pc_status fill_stops(const pc_gradient_preset *g, const float fg[4], const float bg[4],
                            pc_color_stop **cs, size_t *ncs, pc_opacity_stop **os, size_t *nos,
                            pc_error *err)
{
    size_t n = g->nstops;
    pc_color_stop *c = pc_malloc_array(n < 2 ? 2 : n, sizeof *c);
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    size_t cn = 0;
    for (size_t i = 0; i < n; i++) {
        c[cn].pos = clampf(g->stops[i].loc, 0.0f, 1.0f);
        switch (g->stops[i].color.kind) {
        case PC_STOP_FOREGROUND:
            c[cn].color = pc_color_rgb(fg[0], fg[1], fg[2]);
            break;
        case PC_STOP_BACKGROUND:
            c[cn].color = pc_color_rgb(bg[0], bg[1], bg[2]);
            break;
        default:
            c[cn].color = pc_color_rgb(g->stops[i].color.rgb[0], g->stops[i].color.rgb[1],
                                       g->stops[i].color.rgb[2]);
            break;
        }
        cn++;
    }
    qsort(c, cn, sizeof *c, cmp_color_stop);
    if (cn == 0) {
        c[0].pos = 0.0f;
        c[0].color = pc_color_rgb(fg[0], fg[1], fg[2]);
        c[1].pos = 1.0f;
        c[1].color = pc_color_rgb(bg[0], bg[1], bg[2]);
        cn = 2;
    }
    pc_opacity_stop *o = g->nopacity > 0 ? pc_malloc_array(g->nopacity, sizeof *o) : NULL;
    if (g->nopacity > 0 && o == NULL) {
        free(c);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    bool all_one = true;
    for (size_t i = 0; i < g->nopacity; i++) {
        o[i].pos = clampf(g->opacity[i].loc, 0.0f, 1.0f);
        o[i].opacity = clampf(g->opacity[i].opacity, 0.0f, 1.0f);
        if (fabsf(o[i].opacity - 1.0f) >= 1e-6f) {
            all_one = false;
        }
    }
    if (o != NULL) {
        qsort(o, g->nopacity, sizeof *o, cmp_opacity_stop);
    }
    if (all_one) {
        free(o);
        o = NULL;
        *nos = 0;
    } else {
        *nos = g->nopacity;
    }
    *cs = c;
    *ncs = cn;
    *os = o;
    return PC_OK;
}

struct create_ctx {
    pc_layer layer; /* moved into the doc */
    bool consumed;
    pc_layer_id id;
};
static pc_status create_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    struct create_ctx *c = vctx;
    pc_layer_id id;
    const pc_layer_id *above = *ha ? active : NULL;
    pc_status rc = pc_document_insert_above(doc, above, &c->layer, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    c->consumed = true;
    c->id = id;
    *active = id;
    *ha = true;
    return PC_OK;
}

/* Build the Gradient Fill layer for create (and the preview). */
static pc_status build_layer(pc_engine_session *s, const pc_document *doc, yyjson_val *p, const char *cmd,
                             pc_layer *out, pc_error *err)
{
    float from[2], to[2];
    if (point_param(cmd, p, "from", from, err) != 1) {
        if (err != NULL && err->code == PC_OK) {
            (void)pc_engine_err_bad_params(err, cmd, "missing `from` [x, y]");
        }
        return pc_engine_err_bad_params(err, cmd, "missing `from` [x, y]");
    }
    if (point_param(cmd, p, "to", to, err) != 1) {
        return pc_engine_err_bad_params(err, cmd, "missing `to` [x, y]");
    }
    pc_gradient_style style = PC_GRADIENT_LINEAR;
    if (style_param(cmd, p, &style, err) < 0) {
        return PC_ERR_INVALID;
    }
    pc_gradient_preset g = {0};
    pc_status rc = preset_param(s, p, cmd, &g, err);
    if (rc != PC_OK) {
        pc_gradient_preset_fini(&g);
        return rc;
    }
    pc_color_stop *cs = NULL;
    pc_opacity_stop *os = NULL;
    size_t ncs = 0, nos = 0;
    rc = fill_stops(&g, s->tools.foreground, s->tools.background, &cs, &ncs, &os, &nos, err);
    pc_gradient_preset_fini(&g);
    if (rc != PC_OK) {
        return rc;
    }
    bool reverse = false, dither = true;
    yyjson_val *rv = pc_json_get(p, "reverse");
    if (rv != NULL) {
        (void)pc_json_as_bool(rv, &reverse);
    }
    yyjson_val *dv = pc_json_get(p, "dither");
    if (dv != NULL) {
        (void)pc_json_as_bool(dv, &dither);
    }
    float opacity = 100.0f;
    (void)num_opt(p, "opacity", &opacity);
    opacity = clampf(opacity, 0.0f, 100.0f) / 100.0f;
    pc_blend_mode blend = PC_BLEND_NORMAL;
    size_t mlen = 0;
    const char *mode = pc_json_as_str(pc_json_get(p, "mode"), &mlen);
    if (mode != NULL && mlen > 0) {
        if (!pc_engine_blend_from_str(mode, mlen, &blend)) {
            free(cs);
            free(os);
            return pc_engine_err_bad_params(err, cmd, "unknown blend mode `%.*s`", (int)mlen, mode);
        }
    }
    pc_rect canvas = pc_document_bounds(doc);
    if (pc_rect_is_empty(canvas)) {
        free(cs);
        free(os);
        return pc_engine_err_bad_params(err, cmd, "the document is empty");
    }
    float angle, scale, offset[2];
    pc_compose_gradient_from_handles(style, from, to, canvas, 0.0f, &angle, &scale, offset);
    pc_fill fill;
    memset(&fill, 0, sizeof fill);
    fill.kind = PC_FILL_GRADIENT;
    fill.u.gradient.stops = cs;
    fill.u.gradient.nstops = ncs;
    fill.u.gradient.angle = angle;
    fill.u.gradient.scale = clamp_scale(scale);
    fill.u.gradient.style = style;
    fill.u.gradient.reverse = reverse;
    fill.u.gradient.opacity_stops = os;
    fill.u.gradient.nopacity_stops = nos;
    fill.u.gradient.offset[0] = offset[0];
    fill.u.gradient.offset[1] = offset[1];
    fill.u.gradient.dither = dither;
    fill.u.gradient.align = false;
    pc_fill_in_mode(&fill, doc->mode);
    pc_str nm = {0};
    if (pc_document_next_layer_name(doc, "Gradient Fill", &nm, err) != PC_OK) {
        pc_fill_fini(&fill);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_content content;
    memset(&content, 0, sizeof content);
    content.kind = PC_LAYER_FILL;
    content.u.fill = fill;
    rc = pc_layer_init(out, pc_str_c(&nm), &content, err);
    pc_str_fini(&nm);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        return rc;
    }
    out->opacity = opacity;
    out->blend = blend;
    /* masked by the selection (Photoshop keeps the selection, as a mask). */
    if (doc->has_selection) {
        pc_layer_mask m;
        pc_layer_mask_reveal_all(&m);
        pc_surface sel;
        if (pc_surface_clone(&doc->selection, &sel, err) != PC_OK) {
            pc_layer_mask_fini(&m);
            pc_layer_fini(out);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_fini(&m.surface);
        m.surface = sel;
        pc_pixel_format cur = pc_surface_format(&m.surface);
        pc_pixel_format want = cur;
        want.sample = doc->depth;
        if (!pc_pixel_format_eq(cur, want)) {
            pc_surface conv;
            if (pc_surface_convert(&m.surface, want, &conv, err) != PC_OK) {
                pc_layer_mask_fini(&m);
                pc_layer_fini(out);
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
            pc_surface_fini(&m.surface);
            m.surface = conv;
        }
        if (out->has_mask) {
            pc_layer_mask_fini(&out->mask);
        }
        out->mask = m;
        out->has_mask = true;
    }
    return PC_OK;
}

static pc_status run_create(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    struct create_ctx ctx = {0};
    pc_status rc = build_layer(s, doc, p, CMD_CREATE, &ctx.layer, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_session_edit(s, "New Gradient Fill Layer", create_edit, &ctx, err);
    if (!ctx.consumed) {
        pc_layer_fini(&ctx.layer);
    }
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_uint(jd, root, "layer", ctx.id);
    yyjson_mut_doc_set_root(jd, root);
    return PC_OK;
}

/* Piecewise-linear RGBA of sorted (pos,rgba) stops at t. */
static void samp_rgba(const float *locs, const float (*cols)[4], size_t n, float t, float out[4])
{
    if (n == 0) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    if (t <= locs[0]) {
        memcpy(out, cols[0], 4 * sizeof(float));
        return;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (t <= locs[i + 1]) {
            float u = locs[i + 1] > locs[i] ? (t - locs[i]) / (locs[i + 1] - locs[i]) : 1.0f;
            for (int k = 0; k < 4; k++) {
                out[k] = cols[i][k] + (cols[i + 1][k] - cols[i][k]) * u;
            }
            return;
        }
    }
    memcpy(out, cols[n - 1], 4 * sizeof(float));
}
static float samp_op(const pc_gradient_ostop *os, size_t n, float t)
{
    if (n == 0) {
        return 1.0f;
    }
    if (t <= os[0].loc) {
        return os[0].opacity;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (t <= os[i + 1].loc) {
            float u = os[i + 1].loc > os[i].loc ? (t - os[i].loc) / (os[i + 1].loc - os[i].loc) : 1.0f;
            return os[i].opacity + (os[i + 1].opacity - os[i].opacity) * u;
        }
    }
    return os[n - 1].opacity;
}

/* GradientPreset::doc_stops: colour stops resolved (fg/bg) with opacity stops merged into alpha.
 * *out owned (free()). */
static pc_status resolve_doc_stops(const pc_gradient_preset *g, const float fg[4], const float bg[4],
                                   pc_color_stop **out, size_t *nout, pc_error *err)
{
    size_t nc = g->nstops;
    size_t cap = nc < 2 ? 2 : nc;
    float *locs = pc_malloc_array(cap, sizeof *locs);
    float (*cols)[4] = pc_malloc_array(cap, sizeof *cols);
    if (locs == NULL || cols == NULL) {
        free(locs);
        free(cols);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    size_t cn = 0;
    for (size_t i = 0; i < nc; i++) {
        locs[cn] = clampf(g->stops[i].loc, 0.0f, 1.0f);
        switch (g->stops[i].color.kind) {
        case PC_STOP_FOREGROUND:
            cols[cn][0] = fg[0];
            cols[cn][1] = fg[1];
            cols[cn][2] = fg[2];
            cols[cn][3] = 1.0f;
            break;
        case PC_STOP_BACKGROUND:
            cols[cn][0] = bg[0];
            cols[cn][1] = bg[1];
            cols[cn][2] = bg[2];
            cols[cn][3] = 1.0f;
            break;
        default:
            cols[cn][0] = g->stops[i].color.rgb[0];
            cols[cn][1] = g->stops[i].color.rgb[1];
            cols[cn][2] = g->stops[i].color.rgb[2];
            cols[cn][3] = 1.0f;
            break;
        }
        cn++;
    }
    for (size_t i = 1; i < cn; i++) { /* stable insertion sort */
        float kl = locs[i];
        float kc[4];
        memcpy(kc, cols[i], sizeof kc);
        size_t j = i;
        while (j > 0 && locs[j - 1] > kl) {
            locs[j] = locs[j - 1];
            memcpy(cols[j], cols[j - 1], sizeof kc);
            j--;
        }
        locs[j] = kl;
        memcpy(cols[j], kc, sizeof kc);
    }
    if (cn == 0) {
        locs[0] = 0.0f;
        cols[0][0] = fg[0];
        cols[0][1] = fg[1];
        cols[0][2] = fg[2];
        cols[0][3] = 1.0f;
        locs[1] = 1.0f;
        cols[1][0] = bg[0];
        cols[1][1] = bg[1];
        cols[1][2] = bg[2];
        cols[1][3] = 1.0f;
        cn = 2;
    }
    /* opacity (sorted, clamped); if empty or all-one, keep color stops as-is. */
    pc_gradient_ostop *os = g->nopacity > 0 ? pc_malloc_array(g->nopacity, sizeof *os) : NULL;
    if (g->nopacity > 0 && os == NULL) {
        free(locs);
        free(cols);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    bool all_one = true;
    for (size_t i = 0; i < g->nopacity; i++) {
        os[i].loc = clampf(g->opacity[i].loc, 0.0f, 1.0f);
        os[i].opacity = clampf(g->opacity[i].opacity, 0.0f, 1.0f);
        if (fabsf(os[i].opacity - 1.0f) >= 1e-6f) {
            all_one = false;
        }
    }
    for (size_t i = 1; i < g->nopacity; i++) {
        pc_gradient_ostop k = os[i];
        size_t j = i;
        while (j > 0 && os[j - 1].loc > k.loc) {
            os[j] = os[j - 1];
            j--;
        }
        os[j] = k;
    }
    if (g->nopacity == 0 || all_one) {
        pc_color_stop *r = pc_malloc_array(cn, sizeof *r);
        if (r == NULL) {
            free(locs);
            free(cols);
            free(os);
            return pc_fail(err, PC_ERR_NOMEM, "gradient");
        }
        for (size_t i = 0; i < cn; i++) {
            r[i].pos = locs[i];
            r[i].color = pc_color_rgba(cols[i][0], cols[i][1], cols[i][2], cols[i][3]);
        }
        free(locs);
        free(cols);
        free(os);
        *out = r;
        *nout = cn;
        return PC_OK;
    }
    /* dense resample (apply_opacity). */
    size_t tcap = cn + g->nopacity + 2;
    float *ts = pc_malloc_array(tcap, sizeof *ts);
    if (ts == NULL) {
        free(locs);
        free(cols);
        free(os);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    size_t tn = 0;
    for (size_t i = 0; i < cn; i++) {
        ts[tn++] = locs[i];
    }
    for (size_t i = 0; i < g->nopacity; i++) {
        ts[tn++] = os[i].loc;
    }
    ts[tn++] = 0.0f;
    ts[tn++] = 1.0f;
    for (size_t i = 1; i < tn; i++) {
        float k = ts[i];
        size_t j = i;
        while (j > 0 && ts[j - 1] > k) {
            ts[j] = ts[j - 1];
            j--;
        }
        ts[j] = k;
    }
    size_t dn = 0;
    for (size_t i = 0; i < tn; i++) {
        if (dn == 0 || fabsf(ts[i] - ts[dn - 1]) >= 1e-6f) {
            ts[dn++] = ts[i];
        }
    }
    size_t ncap = (dn > 0 ? dn - 1 : 0) * 4 + 1;
    pc_color_stop *r = pc_malloc_array(ncap > 0 ? ncap : 1, sizeof *r);
    if (r == NULL) {
        free(locs);
        free(cols);
        free(os);
        free(ts);
        return pc_fail(err, PC_ERR_NOMEM, "gradient");
    }
    size_t rn = 0;
    for (size_t i = 0; i + 1 < dn; i++) {
        for (int k = 0; k < 4; k++) {
            float t = ts[i] + (ts[i + 1] - ts[i]) * (float)k / 4.0f;
            float c[4];
            samp_rgba(locs, (const float (*)[4])cols, cn, t, c);
            c[3] *= samp_op(os, g->nopacity, t);
            r[rn].pos = t;
            r[rn].color = pc_color_rgba(c[0], c[1], c[2], c[3]);
            rn++;
        }
    }
    {
        float c[4];
        samp_rgba(locs, (const float (*)[4])cols, cn, 1.0f, c);
        c[3] *= samp_op(os, g->nopacity, 1.0f);
        r[rn].pos = 1.0f;
        r[rn].color = pc_color_rgba(c[0], c[1], c[2], c[3]);
        rn++;
    }
    free(locs);
    free(cols);
    free(os);
    free(ts);
    *out = r;
    *nout = rn;
    return PC_OK;
}

struct apply_ctx {
    pc_fill fill; /* moved into the layer */
    bool consumed;
    pc_layer_id id;
};
static pc_status apply_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    struct apply_ctx *c = vctx;
    pc_fill_in_mode(&c->fill, doc->mode);
    pc_str nm = {0};
    if (pc_document_next_layer_name(doc, "Gradient Fill", &nm, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_content content;
    memset(&content, 0, sizeof content);
    content.kind = PC_LAYER_FILL;
    content.u.fill = c->fill; /* moved */
    c->consumed = true;
    pc_layer l;
    pc_status rc = pc_layer_init(&l, pc_str_c(&nm), &content, err);
    pc_str_fini(&nm);
    if (rc != PC_OK) {
        pc_layer_content_fini(&content);
        return rc;
    }
    pc_layer_id id;
    const pc_layer_id *above = *ha ? active : NULL;
    rc = pc_document_insert_above(doc, above, &l, &id, err);
    if (rc != PC_OK) {
        pc_layer_fini(&l);
        return rc;
    }
    c->id = id;
    *active = id;
    *ha = true;
    return PC_OK;
}

/* gradient.presets.apply: a new classic Gradient Fill layer from a preset, using angle/scale/style
 * directly (no on-canvas handles or selection mask). */
static pc_status run_apply(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "gradient.presets.apply";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_gradient_preset g = {0};
    pc_status rc = preset_param(s, p, cmd, &g, err);
    if (rc != PC_OK) {
        pc_gradient_preset_fini(&g);
        return rc;
    }
    pc_color_stop *cs = NULL;
    size_t ncs = 0;
    rc = resolve_doc_stops(&g, s->tools.foreground, s->tools.background, &cs, &ncs, err);
    if (rc != PC_OK) {
        pc_gradient_preset_fini(&g);
        return rc;
    }
    float angle = 90.0f, scale = 100.0f;
    (void)num_opt(p, "angle", &angle);
    (void)num_opt(p, "scale", &scale);
    scale = clamp_scale(scale / 100.0f);
    pc_gradient_style style = PC_GRADIENT_LINEAR;
    (void)style_param(cmd, p, &style, err);
    bool reverse = false;
    yyjson_val *rv = pc_json_get(p, "reverse");
    if (rv != NULL) {
        (void)pc_json_as_bool(rv, &reverse);
    }
    struct apply_ctx ctx = {0};
    rc = pc_fill_gradient(cs, ncs, angle, scale, style, reverse, &ctx.fill, err);
    free(cs);
    if (rc != PC_OK) {
        pc_gradient_preset_fini(&g);
        return rc;
    }
    rc = pc_engine_session_edit(s, "New Gradient Fill Layer", apply_edit, &ctx, err);
    if (!ctx.consumed) {
        pc_fill_fini(&ctx.fill);
    }
    if (rc != PC_OK) {
        pc_gradient_preset_fini(&g);
        return rc;
    }
    yyjson_mut_doc *jd = NULL;
    if (pc_engine_result_doc(out, &jd, err) != PC_OK) {
        pc_gradient_preset_fini(&g);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(jd);
    yyjson_mut_obj_add_uint(jd, root, "layer", ctx.id);
    yyjson_mut_obj_add_strcpy(jd, root, "preset", g.name != NULL ? g.name : "");
    yyjson_mut_doc_set_root(jd, root);
    pc_gradient_preset_fini(&g);
    return PC_OK;
}

/* ========================================================================================== */
/* describe + set/stop/get runs                                                                */
/* ========================================================================================== */

static pc_status describe(pc_engine_session *s, pc_layer_id id, pc_json_value *out, pc_error *err)
{
    pc_fill f = {0};
    pc_rect frame = PC_RECT_EMPTY;
    pc_status rc = gradient_of(s, id, CMD_GET, &f, &frame, err);
    if (rc != PC_OK) {
        return rc;
    }
    float from[2], to[2];
    pc_compose_gradient_handles(f.u.gradient.style, f.u.gradient.angle, f.u.gradient.scale,
                                f.u.gradient.offset, frame, from, to);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        pc_fill_fini(&f);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, root, "style", style_name(f.u.gradient.style));
    yyjson_mut_obj_add_real(d, root, "angle", f.u.gradient.angle);
    yyjson_mut_obj_add_real(d, root, "scale", (double)f.u.gradient.scale * 100.0);
    yyjson_mut_val *off = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, off, (double)f.u.gradient.offset[0] * 100.0);
    yyjson_mut_arr_add_real(d, off, (double)f.u.gradient.offset[1] * 100.0);
    yyjson_mut_obj_add_val(d, root, "offset", off);
    yyjson_mut_obj_add_bool(d, root, "reverse", f.u.gradient.reverse);
    yyjson_mut_obj_add_bool(d, root, "dither", f.u.gradient.dither);
    yyjson_mut_obj_add_bool(d, root, "align", f.u.gradient.align);
    yyjson_mut_val *fa = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, fa, from[0]);
    yyjson_mut_arr_add_real(d, fa, from[1]);
    yyjson_mut_obj_add_val(d, root, "from", fa);
    yyjson_mut_val *ta = yyjson_mut_arr(d);
    yyjson_mut_arr_add_real(d, ta, to[0]);
    yyjson_mut_arr_add_real(d, ta, to[1]);
    yyjson_mut_obj_add_val(d, root, "to", ta);
    yyjson_mut_val *st = yyjson_mut_arr(d);
    for (size_t i = 0; i < f.u.gradient.nstops; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, pair, f.u.gradient.stops[i].pos);
        char hx[10];
        hex_of(&f.u.gradient.stops[i].color, hx);
        yyjson_mut_arr_add_strcpy(d, pair, hx);
        yyjson_mut_arr_append(st, pair);
    }
    yyjson_mut_obj_add_val(d, root, "stops", st);
    yyjson_mut_val *tr = yyjson_mut_arr(d);
    for (size_t i = 0; i < f.u.gradient.nopacity_stops; i++) {
        yyjson_mut_val *pair = yyjson_mut_arr(d);
        yyjson_mut_arr_add_real(d, pair, f.u.gradient.opacity_stops[i].pos);
        yyjson_mut_arr_add_real(d, pair, (double)f.u.gradient.opacity_stops[i].opacity * 100.0);
        yyjson_mut_arr_append(tr, pair);
    }
    yyjson_mut_obj_add_val(d, root, "transparency", tr);
    yyjson_mut_val *mids = yyjson_mut_arr(d);
    for (size_t i = 0; i < f.u.gradient.nmidpoints; i++) {
        yyjson_mut_arr_add_real(d, mids, f.u.gradient.midpoints[i]);
    }
    yyjson_mut_obj_add_val(d, root, "midpoints", mids);
    yyjson_mut_obj_add_uint(d, root, "layer", id);
    yyjson_mut_doc_set_root(d, root);
    pc_fill_fini(&f);
    return PC_OK;
}

struct replace_ctx {
    pc_layer_id id;
    const pc_fill *fill; /* borrowed */
};
static pc_status replace_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct replace_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_fill nf;
    if (pc_fill_clone(c->fill, &nf, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_fill_in_mode(&nf, doc->mode);
    if (l->content.kind == PC_LAYER_FILL) {
        pc_fill_fini(&l->content.u.fill);
    } else {
        pc_layer_content_fini(&l->content);
        memset(&l->content, 0, sizeof l->content);
    }
    l->content.kind = PC_LAYER_FILL;
    l->content.u.fill = nf;
    return PC_OK;
}

static pc_status replace_fill(pc_engine_session *s, pc_layer_id id, const char *label, const pc_fill *new,
                              pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_status rc = check_pixels_unlocked(pc_doc_arc_get(d->doc), id, err);
    if (rc != PC_OK) {
        return rc;
    }
    struct replace_ctx ctx = {id, new};
    return pc_engine_session_edit(s, label, replace_edit, &ctx, err);
}

static pc_status run_set(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_fill fill = {0};
    pc_rect frame = PC_RECT_EMPTY;
    rc = gradient_of(s, id, CMD_SET, &fill, &frame, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    pc_layer layer;
    if (pc_layer_clone(pc_document_layer(pc_doc_arc_get(d->doc), id), &layer, err) != PC_OK) {
        pc_fill_fini(&fill);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_rect canvas = pc_document_bounds(pc_doc_arc_get(d->doc));
    pc_fill newf;
    rc = apply_set(&layer, &fill, canvas, p, s->tools.foreground, s->tools.background, &newf, err);
    pc_layer_fini(&layer);
    if (rc != PC_OK) {
        pc_fill_fini(&fill);
        return rc;
    }
    if (!pc_fill_eq(&newf, &fill)) {
        rc = replace_fill(s, id, "Edit Gradient Fill", &newf, err);
    }
    pc_fill_fini(&newf);
    pc_fill_fini(&fill);
    if (rc != PC_OK) {
        return rc;
    }
    return describe(s, id, out, err);
}

static pc_status run_stop(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_fill fill = {0};
    pc_rect frame = PC_RECT_EMPTY;
    rc = gradient_of(s, id, CMD_STOP, &fill, &frame, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_fill newf;
    rc = apply_stop(&fill, p, s->tools.foreground, s->tools.background, &newf, err);
    if (rc != PC_OK) {
        pc_fill_fini(&fill);
        return rc;
    }
    if (!pc_fill_eq(&newf, &fill)) {
        rc = replace_fill(s, id, "Edit Gradient Fill", &newf, err);
    }
    pc_fill_fini(&newf);
    pc_fill_fini(&fill);
    if (rc != PC_OK) {
        return rc;
    }
    return describe(s, id, out, err);
}

static pc_status run_get(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    return describe(s, id, out, err);
}

/* ========================================================================================== */
/* specs                                                                                       */
/* ========================================================================================== */

static const pc_engine_command SPECS[] = {
    {CMD_CREATE, "Gradient (Live)", NULL, 0, NULL,
     "{\"from\":[x,y],\"to\":[x,y],\"style\":\"linear|radial|angle|reflected|diamond\"=\"linear\","
     "\"gradient\":preset name?,\"stops\":[[t,\"#rrggbb\"|\"foreground\"|\"background\"],...]?,"
     "\"transparency\":[[t,0..100],...]? (default: the current gradient),\"reverse\":bool=false,"
     "\"dither\":bool=true,\"opacity\":0..100=100,\"mode\":\"normal|multiply|...\"=\"normal\"} -> "
     "{\"layer\":id}; a Gradient Fill layer above the active layer, masked by the selection",
     has_doc, run_create, true},
    {CMD_SET, "Edit Gradient Fill", NULL, 0, NULL,
     "{\"layer\":id?,\"from\":[x,y]?,\"to\":[x,y]?,\"style\":\"linear|radial|angle|reflected|diamond\"?,"
     "\"angle\":deg?,\"scale\":%?,\"offset\":[x%,y%]?,\"reverse\":bool?,\"dither\":bool?,\"align\":bool?,"
     "\"stops\":[[t,\"#rrggbb\"|\"#rrggbbaa\"|[r,g,b,a]|\"foreground\"|\"background\"],...]?,"
     "\"transparency\":[[t,0..100],...]?,\"midpoints\":[0.05..0.95 per segment]?} -> the gradient (see "
     "gradient.fill.get)",
     has_gradient_fill, run_set, true},
    {CMD_STOP, "Edit Gradient Stop", NULL, 0, NULL,
     "{\"layer\":id?,\"action\":\"add|move|delete|color|opacity|midpoint\",\"kind\":\"color|opacity\"="
     "\"color\",\"index\":stop (midpoint: segment) number,\"location\":0..1 (midpoint: of the segment),"
     "\"color\":\"#rrggbb\"|[r,g,b,a]|\"foreground\"|\"background\" (add: default the colour there),"
     "\"opacity\":0..100} -> the gradient",
     has_gradient_fill, run_stop, true},
    {CMD_GET, "Gradient Fill Settings", NULL, 0, NULL,
     "{\"layer\":id?} -> {\"style\",\"angle\",\"scale\":%,\"offset\":[%,%],\"reverse\",\"dither\",\"align\","
     "\"from\":[x,y],\"to\":[x,y],\"stops\":[[t,\"#hex\"]],\"transparency\":[[t,%]],\"midpoints\":[...]}",
     has_gradient_fill, run_get, false},
    {"gradient.presets.apply", "Apply Gradient", NULL, 0, NULL,
     "{\"gradient\":preset name,\"from\":[x,y],\"to\":[x,y],...gradient.fill.create params} (a new "
     "Gradient Fill layer from the preset)",
     has_doc, run_apply, true},
};

const pc_engine_command *pc_engine_gradient_fill_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
