/* Clone Source slots data model and the stroke mapping / resample helpers the Clone Stamp and
 * Healing Brush consume (port of crates/engine/src/presets/clone_source.rs, E2b data + E10-retouch
 * Mapping/transform_matrix/resolve/mapping/resample). Never crashes. */
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

/* std::f64::consts::PI (M_PI is not ISO C). */
#define PC_CLONE_PI 3.14159265358979323846

#include "base/json.h"
#include "paint/retouch.h"

#include "engine_internal.h"
#include "presets.h"

void pc_clone_source_default(pc_clone_source *c)
{
    memset(c, 0, sizeof *c);
    c->scale[0] = 100.0;
    c->scale[1] = 100.0;
    c->rotation = 0.0;
}

void pc_clone_sources_default(pc_clone_sources *c)
{
    memset(c, 0, sizeof *c);
    for (size_t i = 0; i < PC_CLONE_SLOTS; i++) {
        pc_clone_source_default(&c->slots[i]);
    }
    c->active = 0;
    c->overlay.show = true;
    c->overlay.opacity = 100.0f;
    c->overlay.clipped = true;
    c->overlay.auto_hide = true;
    c->overlay.invert = false;
    c->overlay.blend = 0; /* normal */
}

const char *pc_clone_overlay_blend_name(int blend)
{
    switch (blend) {
    case 1:
        return "darken";
    case 2:
        return "lighten";
    case 3:
        return "difference";
    default:
        return "normal";
    }
}

bool pc_clone_overlay_blend_parse(const char *s, int *out)
{
    if (strcmp(s, "normal") == 0) {
        *out = 0;
        return true;
    }
    if (strcmp(s, "darken") == 0) {
        *out = 1;
        return true;
    }
    if (strcmp(s, "lighten") == 0) {
        *out = 2;
        return true;
    }
    if (strcmp(s, "difference") == 0) {
        *out = 3;
        return true;
    }
    return false;
}

/* ---- stroke mapping / resample (Clone Stamp, Healing Brush; port of Mapping etc.) --------- */

void pc_engine_clone_transform_matrix(const double scale[2], double rotation_deg, bool flip_h, bool flip_v,
                                      double out[4])
{
    double sx = fmax(scale[0] / 100.0, 1e-3) * (flip_h ? -1.0 : 1.0);
    double sy = fmax(scale[1] / 100.0, 1e-3) * (flip_v ? -1.0 : 1.0);
    /* Forward (source -> destination): rotate after scaling. Rotation is CCW on screen, which is
     * clockwise in y-down maths, hence the negated angle. */
    double rad = -rotation_deg * (PC_CLONE_PI / 180.0);
    double s = sin(rad), c = cos(rad);
    double f[4] = {c * sx, -s * sy, s * sx, c * sy};
    double det = f[0] * f[3] - f[1] * f[2];
    out[0] = f[3] / det;
    out[1] = -f[1] / det;
    out[2] = -f[2] / det;
    out[3] = f[0] / det;
}

bool pc_engine_clone_is_translation(const pc_clone_mapping *map)
{
    return fabs(map->m[0] - 1.0) < 1e-12 && fabs(map->m[1]) < 1e-12 && fabs(map->m[2]) < 1e-12 &&
           fabs(map->m[3] - 1.0) < 1e-12;
}

/* round() saturating to i32 (Rust `v.round() as i32`). */
static int32_t round_i32(double v)
{
    double r = round(v);
    if (r != r) {
        return 0;
    }
    if (r <= (double)INT32_MIN) {
        return INT32_MIN;
    }
    if (r >= (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)r;
}

void pc_engine_clone_offset(const pc_clone_mapping *map, int32_t *dx, int32_t *dy)
{
    *dx = round_i32(map->source[0] - map->anchor[0]);
    *dy = round_i32(map->source[1] - map->anchor[1]);
}

void pc_engine_clone_map(const pc_clone_mapping *map, double x, double y, double *ox, double *oy)
{
    double dx = x - map->anchor[0], dy = y - map->anchor[1];
    *ox = map->source[0] + map->m[0] * dx + map->m[1] * dy;
    *oy = map->source[1] + map->m[2] * dx + map->m[3] * dy;
}

/* clamp an already floor/ceil'd value to i32, then saturating-add pad (Rust apron, #716). */
static int32_t apron(double v, int32_t pad)
{
    int32_t iv;
    if (v != v) {
        iv = 0; /* NaN -> 0, matching Rust `as i32` */
    } else if (v <= (double)INT32_MIN) {
        iv = INT32_MIN;
    } else if (v >= (double)INT32_MAX) {
        iv = INT32_MAX;
    } else {
        iv = (int32_t)v;
    }
    int64_t s = (int64_t)iv + (int64_t)pad;
    if (s < INT32_MIN) {
        s = INT32_MIN;
    }
    if (s > INT32_MAX) {
        s = INT32_MAX;
    }
    return (int32_t)s;
}

pc_rect pc_engine_clone_source_rect(const pc_clone_mapping *map, pc_rect dst)
{
    double cx[4] = {(double)dst.x0, (double)dst.x1, (double)dst.x0, (double)dst.x1};
    double cy[4] = {(double)dst.y0, (double)dst.y0, (double)dst.y1, (double)dst.y1};
    double x0 = DBL_MAX, y0 = DBL_MAX, x1 = -DBL_MAX, y1 = -DBL_MAX;
    for (int i = 0; i < 4; i++) {
        double mx, my;
        pc_engine_clone_map(map, cx[i], cy[i], &mx, &my);
        if (mx < x0) {
            x0 = mx;
        }
        if (my < y0) {
            y0 = my;
        }
        if (mx > x1) {
            x1 = mx;
        }
        if (my > y1) {
            y1 = my;
        }
    }
    return pc_rect_make(apron(floor(x0), -2), apron(floor(y0), -2), apron(ceil(x1), 2), apron(ceil(y1), 2));
}

/* [x,y] number pair from param `k`; false (nothing written) when absent or malformed. */
static bool pt_param(yyjson_val *p, const char *k, double *x, double *y)
{
    yyjson_val *a = pc_json_get(p, k);
    if (a == NULL || !yyjson_is_arr(a) || yyjson_arr_size(a) < 2) {
        return false;
    }
    return pc_json_as_f64(yyjson_arr_get(a, 0), x) && pc_json_as_f64(yyjson_arr_get(a, 1), y);
}

pc_status pc_engine_clone_resolve(const pc_engine_session *s, yyjson_val *p, double fx, double fy,
                                  const char *cmd, pc_clone_mapping *out, bool *has_anchor, double anchor[2],
                                  pc_error *err)
{
    *has_anchor = false;
    bool aligned = true;
    yyjson_val *av = pc_json_get(p, "aligned");
    if (av != NULL) {
        (void)pc_json_as_bool(av, &aligned);
    }
    const pc_clone_sources *cs = &s->presets.clone;
    size_t idx = cs->active < PC_CLONE_SLOTS ? cs->active : PC_CLONE_SLOTS - 1;
    const pc_clone_source *slot = &cs->slots[idx];

    bool explicit_tf = pc_json_get(p, "scale") != NULL || pc_json_get(p, "rotation") != NULL ||
                       pc_json_get(p, "flipH") != NULL || pc_json_get(p, "flipV") != NULL;
    double m[4];
    if (explicit_tf) {
        double sc[2] = {100.0, 100.0};
        yyjson_val *scv = pc_json_get(p, "scale");
        if (scv != NULL && yyjson_is_arr(scv)) {
            double v;
            if (yyjson_arr_size(scv) > 0 && pc_json_as_f64(yyjson_arr_get(scv, 0), &v)) {
                sc[0] = v;
            }
            if (yyjson_arr_size(scv) > 1 && pc_json_as_f64(yyjson_arr_get(scv, 1), &v)) {
                sc[1] = v;
            }
        }
        double rot = 0.0;
        yyjson_val *rv = pc_json_get(p, "rotation");
        if (rv != NULL) {
            (void)pc_json_as_f64(rv, &rot);
        }
        bool fh = false, fv = false;
        yyjson_val *fhv = pc_json_get(p, "flipH"), *fvv = pc_json_get(p, "flipV");
        if (fhv != NULL) {
            (void)pc_json_as_bool(fhv, &fh);
        }
        if (fvv != NULL) {
            (void)pc_json_as_bool(fvv, &fv);
        }
        pc_engine_clone_transform_matrix(sc, rot, fh, fv, m);
    } else {
        pc_engine_clone_transform_matrix(slot->scale, slot->rotation, slot->flip_h, slot->flip_v, m);
    }

    double ax, ay;
    bool have_anchor_param = pt_param(p, "anchor", &ax, &ay);
    double ox, oy;
    if (pt_param(p, "offset", &ox, &oy)) {
        double a0 = have_anchor_param ? ax : fx, a1 = have_anchor_param ? ay : fy;
        out->source[0] = a0 + ox;
        out->source[1] = a1 + oy;
        out->anchor[0] = a0;
        out->anchor[1] = a1;
        memcpy(out->m, m, sizeof out->m);
        return PC_OK;
    }
    double sx, sy;
    if (pt_param(p, "source", &sx, &sy)) {
        out->source[0] = sx;
        out->source[1] = sy;
        out->anchor[0] = have_anchor_param ? ax : fx;
        out->anchor[1] = have_anchor_param ? ay : fy;
        memcpy(out->m, m, sizeof out->m);
        return PC_OK;
    }
    if (!slot->has_source) {
        return pc_engine_err_bad_params(
            err, cmd, "missing `source` ([x,y]) or `offset` ([dx,dy]); or set one with cloneSource.set");
    }
    out->source[0] = slot->source[0];
    out->source[1] = slot->source[1];
    double a0, a1;
    if (aligned && slot->has_anchor) {
        a0 = slot->anchor[0];
        a1 = slot->anchor[1];
    } else {
        a0 = fx;
        a1 = fy;
    }
    out->anchor[0] = a0;
    out->anchor[1] = a1;
    memcpy(out->m, m, sizeof out->m);
    if (aligned) {
        *has_anchor = true;
        anchor[0] = a0;
        anchor[1] = a1;
    }
    return PC_OK;
}

pc_status pc_engine_clone_mapping(pc_engine_session *s, yyjson_val *p, double fx, double fy,
                                  const char *cmd, pc_clone_mapping *out, pc_error *err)
{
    bool has_anchor = false;
    double anchor[2];
    pc_status rc = pc_engine_clone_resolve(s, p, fx, fy, cmd, out, &has_anchor, anchor, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (has_anchor) {
        size_t idx = s->presets.clone.active < PC_CLONE_SLOTS ? s->presets.clone.active : PC_CLONE_SLOTS - 1;
        s->presets.clone.slots[idx].has_anchor = true;
        s->presets.clone.slots[idx].anchor[0] = anchor[0];
        s->presets.clone.slots[idx].anchor[1] = anchor[1];
    }
    return PC_OK;
}

/* src pixel (x,y) into buf[n], premultiplied by alpha; zero-filled outside src. */
static void clone_fetch(const pc_paint_region *src, int32_t x, int32_t y, bool has_alpha, size_t alpha,
                        size_t n, float *buf)
{
    if (x < src->rect.x0 || y < src->rect.y0 || x >= src->rect.x1 || y >= src->rect.y1) {
        for (size_t c = 0; c < n; c++) {
            buf[c] = 0.0f;
        }
        return;
    }
    const float *px = pc_paint_region_cpx(src, x, y);
    if (px == NULL) {
        for (size_t c = 0; c < n; c++) {
            buf[c] = 0.0f;
        }
        return;
    }
    for (size_t c = 0; c < n; c++) {
        buf[c] = px[c];
    }
    if (has_alpha && alpha < n) {
        float av = buf[alpha];
        for (size_t c = 0; c < n; c++) {
            if (c != alpha) {
                buf[c] *= av;
            }
        }
    }
}

pc_status pc_engine_clone_resample(const pc_paint_region *src, pc_rect dst, const pc_clone_mapping *map,
                                   bool has_alpha, size_t alpha, pc_paint_region *out, pc_error *err)
{
    size_t n = src->ch;
    pc_status rc = pc_paint_region_new(dst, n, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_rect_is_empty(dst) || n == 0 || n > PC_RASTER_MAX_CHANNELS) {
        return PC_OK;
    }
    size_t w = (size_t)pc_rect_width(dst);
    float q[4][PC_RASTER_MAX_CHANNELS];
    size_t count = out->len / n;
    for (size_t i = 0; i < count; i++) {
        int32_t x = dst.x0 + (int32_t)(i % w);
        int32_t y = dst.y0 + (int32_t)(i / w);
        double sx, sy;
        pc_engine_clone_map(map, (double)x + 0.5, (double)y + 0.5, &sx, &sy);
        double fx = sx - 0.5, fy = sy - 0.5;
        double ffx = floor(fx), ffy = floor(fy);
        int32_t x0 = apron(ffx, 0), y0 = apron(ffy, 0);
        float tx = (float)(fx - ffx), ty = (float)(fy - ffy);
        clone_fetch(src, x0, y0, has_alpha, alpha, n, q[0]);
        clone_fetch(src, x0 + 1, y0, has_alpha, alpha, n, q[1]);
        clone_fetch(src, x0, y0 + 1, has_alpha, alpha, n, q[2]);
        clone_fetch(src, x0 + 1, y0 + 1, has_alpha, alpha, n, q[3]);
        float *px = &out->data[i * n];
        for (size_t c = 0; c < n; c++) {
            float top = q[0][c] + (q[1][c] - q[0][c]) * tx;
            float bot = q[2][c] + (q[3][c] - q[2][c]) * tx;
            px[c] = top + (bot - top) * ty;
        }
        if (has_alpha && alpha < n) {
            float av = px[alpha];
            for (size_t c = 0; c < n; c++) {
                if (c != alpha) {
                    px[c] = av > 1e-6f ? px[c] / av : 0.0f;
                }
            }
        }
    }
    return PC_OK;
}
