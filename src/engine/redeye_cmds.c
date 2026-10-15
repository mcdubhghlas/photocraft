/* Red Eye tool (port of crates/engine/src/redeye_cmds.rs, E10): click a pupil to neutralise the
 * flash reflection. Toolbar-only (paint.redEye). The kernel in src/algo/redeye runs on a window
 * around each click plus the mask-filter halo. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/redeye.h"
#include "base/json.h"
#include "base/mem.h"
#include "color/color.h"
#include "doc/doc.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "brush_cmds.h"
#include "channel_cmds.h"
#include "engine_internal.h"

#define CMD "paint.redEye"
#define MISS "no red-eye pixels near the click"

static float numf(yyjson_val *p, const char *key, float def, bool *ok)
{
    *ok = true;
    yyjson_val *v = pc_json_get(p, key);
    if (v == NULL) {
        return def;
    }
    double x;
    if (pc_json_as_f64(v, &x) && isfinite(x)) {
        return (float)x;
    }
    *ok = false;
    return def;
}

static bool coord(yyjson_val *v, double *out)
{
    double x;
    if (v != NULL && pc_json_as_f64(v, &x) && isfinite(x)) {
        *out = x;
        return true;
    }
    return false;
}

static int32_t clamp_i32(double v)
{
    if (v < (double)INT32_MIN) {
        v = (double)INT32_MIN;
    } else if (v > (double)INT32_MAX) {
        v = (double)INT32_MAX;
    }
    return (int32_t)floor(v);
}

typedef struct {
    int32_t x, y;
} click;

/* Parse one click {x,y} or a batch {points:[[x,y],…]}. *out owned (free). */
static pc_status parse_clicks(yyjson_val *p, click **out, size_t *n, pc_error *err)
{
    yyjson_val *arr = pc_json_get(p, "points");
    if (arr != NULL && yyjson_is_arr(arr)) {
        size_t cnt = yyjson_arr_size(arr);
        if (cnt == 0) {
            return pc_engine_err_bad_params(err, CMD, "`points` is empty");
        }
        if (cnt > PC_ALGO_REDEYE_MAX_POINTS) {
            return pc_engine_err_bad_params(err, CMD, "at most %u points", PC_ALGO_REDEYE_MAX_POINTS);
        }
        click *pts = pc_malloc_array(cnt, sizeof *pts);
        if (pts == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "points");
        }
        size_t idx, max;
        yyjson_val *q;
        size_t w = 0;
        yyjson_arr_foreach (arr, idx, max, q) {
            double x, y;
            if (!yyjson_is_arr(q) || yyjson_arr_size(q) != 2 || !coord(yyjson_arr_get(q, 0), &x) ||
                !coord(yyjson_arr_get(q, 1), &y)) {
                free(pts);
                return pc_engine_err_bad_params(err, CMD, "bad point (want [x, y])");
            }
            pts[w].x = clamp_i32(x);
            pts[w].y = clamp_i32(y);
            w++;
        }
        *out = pts;
        *n = w;
        return PC_OK;
    }
    yyjson_val *xv = pc_json_get(p, "x"), *yv = pc_json_get(p, "y");
    double x, y;
    if (coord(xv, &x) && coord(yv, &y)) {
        click *pts = pc_malloc_array(1, sizeof *pts);
        if (pts == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "points");
        }
        pts[0].x = clamp_i32(x);
        pts[0].y = clamp_i32(y);
        *out = pts;
        *n = 1;
        return PC_OK;
    }
    if (xv == NULL && yv == NULL) {
        return pc_engine_err_bad_params(err, CMD, "missing `x`,`y` or `points`");
    }
    return pc_engine_err_bad_params(err, CMD, "`x` and `y` must be finite numbers");
}

static pc_rect read_rect(int32_t cx, int32_t cy, float radius, bool *ok)
{
    *ok = true;
    int64_t r = isfinite(radius) ? (int64_t)ceilf(radius) : 64;
    if (r < 0) {
        r = 0;
    } else if (r > 256) {
        r = 256;
    }
    int64_t halo = pc_algo_redeye_halo_radius();
    int64_t ext = r + halo;
    int64_t x0 = (int64_t)cx - ext, y0 = (int64_t)cy - ext;
    int64_t x1 = (int64_t)cx + ext + 1, y1 = (int64_t)cy + ext + 1;
    if (x0 < INT32_MIN) {
        x0 = INT32_MIN;
    }
    if (y0 < INT32_MIN) {
        y0 = INT32_MIN;
    }
    if (x1 > INT32_MAX) {
        x1 = INT32_MAX;
    }
    if (y1 > INT32_MAX) {
        y1 = INT32_MAX;
    }
    pc_rect area = {(int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1};
    uint64_t w = pc_rect_width(area), h = pc_rect_height(area);
    uint64_t cap = PC_ALGO_REDEYE_MAX_WINDOW;
    if (pc_rect_is_empty(area) || w * h > cap * cap) {
        *ok = false;
    }
    return area;
}

static bool rgb_changed(const float a[4], const float b[4])
{
    return fabsf(a[0] - b[0]) > 1e-5f || fabsf(a[1] - b[1]) > 1e-5f || fabsf(a[2] - b[2]) > 1e-5f;
}

/* Write back only pixels whose RGBA changed, keeping each pixel's other channels (K, Lab, alpha). */
static pc_status write_rgba(pc_surface *surf, pc_rect area, const float (*before)[4], const float (*px)[4],
                            pc_error *err)
{
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t ch = pc_surface_channels(surf);
    size_t w = pc_rect_width(area), h = pc_rect_height(area);
    size_t need = w * h;
    if (ch == 0) {
        return PC_OK;
    }
    float *data = NULL;
    size_t len = 0;
    if (pc_surface_read_region(surf, area, &data, &len, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    if (len != need * ch) {
        free(data);
        return PC_OK;
    }
    for (size_t i = 0; i < need; i++) {
        if (!rgb_changed(before[i], px[i])) {
            continue;
        }
        float *dst = &data[i * ch];
        float tmp[PC_RASTER_MAX_CHANNELS];
        size_t wrote = pc_from_rgba(fmt, px[i], tmp);
        float alpha = fmt.alpha && ch > 0 ? dst[ch - 1] : 0.0f;
        for (size_t c = 0; c < ch && c < wrote; c++) {
            dst[c] = tmp[c];
        }
        if (fmt.alpha && ch > 0) {
            dst[ch - 1] = alpha;
        }
    }
    pc_status rc = pc_surface_write_region(surf, area, data, len, err);
    free(data);
    return rc;
}

static void mix_selection(const float (*orig)[4], float (*px)[4], pc_rect area, const pc_surface *sel)
{
    size_t w = pc_rect_width(area);
    if (w == 0) {
        return;
    }
    size_t n = (size_t)w * pc_rect_height(area);
    for (size_t i = 0; i < n; i++) {
        size_t y = i / w, x = i % w;
        float k = pc_surface_sample_channel(sel, area.x0 + (int32_t)x, area.y0 + (int32_t)y, 0);
        k = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
        if (k >= 1.0f) {
            continue;
        }
        const float *o = orig[i];
        float *q = px[i];
        if (k <= 0.0f) {
            memcpy(q, o, sizeof(float[4]));
            continue;
        }
        q[0] = o[0] + (q[0] - o[0]) * k;
        q[1] = o[1] + (q[1] - o[1]) * k;
        q[2] = o[2] + (q[2] - o[2]) * k;
        q[3] = o[3];
    }
}

static void restore_locked(const float (*orig)[4], float (*px)[4], size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (orig[i][3] <= 1.0e-5f) {
            memcpy(px[i], orig[i], sizeof(float[4]));
        }
    }
}

static pc_status apply_click(pc_surface *surf, const pc_surface *sel, bool lock, click c, float pupil,
                             float darken, pc_rect clip, pc_rect *out_area, uint32_t *out_changed,
                             pc_error *err)
{
    bool ok = true;
    pc_rect area = pc_rect_intersect(read_rect(c.x, c.y, pc_algo_redeye_search_radius(pupil), &ok), clip);
    *out_area = PC_RECT_EMPTY;
    *out_changed = 0;
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    size_t w = pc_rect_width(area), h = pc_rect_height(area), n = w * h;
    float (*px)[4] = pc_malloc_array(n, sizeof *px);
    float (*orig)[4] = pc_malloc_array(n, sizeof *orig);
    if (px == NULL || orig == NULL) {
        free(px);
        free(orig);
        return pc_fail(err, PC_ERR_NOMEM, "red eye");
    }
    if (pc_surface_read_rgba_into(surf, area, px, n, err) != PC_OK) {
        free(px);
        free(orig);
        return PC_ERR_NOMEM;
    }
    memcpy(orig, px, n * sizeof *px);
    uint32_t changed = 0;
    pc_status rc = pc_algo_redeye_apply(px, n, w, h, c.x, c.y, area.x0, area.y0, pupil, darken, &changed, err);
    if (rc != PC_OK) {
        free(px);
        free(orig);
        return rc;
    }
    if (changed == 0) {
        *out_area = area;
        free(px);
        free(orig);
        return PC_OK;
    }
    if (sel != NULL) {
        mix_selection((const float (*)[4])orig, px, area, sel);
    }
    if (lock) {
        restore_locked((const float (*)[4])orig, px, n);
    }
    uint32_t real = 0;
    for (size_t i = 0; i < n; i++) {
        if (rgb_changed(orig[i], px[i])) {
            real++;
        }
    }
    if (real == 0) {
        *out_area = area;
        free(px);
        free(orig);
        return PC_OK;
    }
    rc = write_rgba(surf, area, (const float (*)[4])orig, (const float (*)[4])px, err);
    free(px);
    free(orig);
    if (rc != PC_OK) {
        return rc;
    }
    *out_area = area;
    *out_changed = real;
    return PC_OK;
}

struct redeye_ctx {
    const click *pts;
    size_t npts;
    float pupil, darken;
    bool has_layer;
    pc_layer_id layer;
    yyjson_val *p;
    pc_rect damage;
    uint32_t total;
};

static pc_status redeye_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    struct redeye_ctx *c = vctx;
    pc_rect canvas = pc_document_bounds(doc);
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    bool has_layer = c->has_layer;
    pc_layer_id layer = c->layer;
    if (!has_layer && *ha) {
        has_layer = true;
        layer = *active;
    }
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, has_layer, layer, c->p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    uint32_t total = 0;
    pc_rect damage = PC_RECT_EMPTY;
    for (size_t i = 0; i < c->npts; i++) {
        pc_rect area;
        uint32_t nch;
        rc = apply_click(surf, sel, lock, c->pts[i], c->pupil, c->darken, canvas, &area, &nch, err);
        if (rc != PC_OK) {
            return rc;
        }
        total += nch;
        damage = pc_rect_is_empty(damage) ? area : pc_rect_union(damage, area);
    }
    if (total == 0) {
        return pc_engine_err_other(err, "%s", MISS);
    }
    c->damage = damage;
    c->total = total;
    return PC_OK;
}

static pc_status en_redeye(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = pc_engine_has_paintable(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_engine_edits_channel(s)) {
        return PC_OK;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL || !d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_locks locks = pc_document_effective_locks(doc, d->active_layer);
    if (locks.pixels || locks.all) {
        const pc_layer *l = pc_document_layer(doc, d->active_layer);
        return pc_fail(reason, PC_ERR_INVALID, "the layer \"%s\" is locked",
                       l != NULL ? pc_str_c(&l->name) : "layer");
    }
    return PC_OK;
}

static pc_status run_redeye(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    click *pts = NULL;
    size_t npts = 0;
    pc_status rc = parse_clicks(p, &pts, &npts, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool ok1 = true, ok2 = true;
    float pupil_raw = numf(p, "pupilSize", 50.0f, &ok1);
    float darken = numf(p, "darken", 50.0f, &ok2);
    if (!ok1 || !ok2) {
        free(pts);
        return pc_engine_err_bad_params(err, CMD, "`pupilSize` and `darken` must be finite numbers");
    }
    uint32_t side;
    if (!pc_algo_redeye_unclamped_window_side(pupil_raw, &side)) {
        free(pts);
        return pc_engine_err_bad_params(err, CMD, "pupil window is larger than %u² px",
                                        PC_ALGO_REDEYE_MAX_WINDOW);
    }
    float pupil = pupil_raw < 1.0f ? 1.0f : (pupil_raw > 100.0f ? 100.0f : pupil_raw);
    darken = darken < 0.0f ? 0.0f : (darken > 100.0f ? 100.0f : darken);

    struct redeye_ctx ctx = {0};
    ctx.pts = pts;
    ctx.npts = npts;
    ctx.pupil = pupil;
    ctx.darken = darken;
    ctx.p = p;
    uint64_t lid;
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv != NULL && pc_json_as_u64(lv, &lid)) {
        ctx.has_layer = true;
        ctx.layer = lid;
    }
    ctx.damage = PC_RECT_EMPTY;
    rc = pc_engine_session_edit(s, "Red Eye", redeye_edit, &ctx, err);
    free(pts);
    if (rc != PC_OK) {
        return rc;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->last_damage = ctx.damage;
        st->has_last_damage = true;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, a, ctx.damage.x0);
    yyjson_mut_arr_add_int(d, a, ctx.damage.y0);
    yyjson_mut_arr_add_int(d, a, (int64_t)pc_rect_width(ctx.damage));
    yyjson_mut_arr_add_int(d, a, (int64_t)pc_rect_height(ctx.damage));
    yyjson_mut_obj_add_val(d, root, "damage", a);
    yyjson_mut_obj_add_uint(d, root, "pixels", ctx.total);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static const pc_engine_command SPECS[] = {
    {"paint.redEye", "Red Eye", NULL, 0, NULL,
     "{\"x\":px, \"y\":px} | {\"points\":[[x,y],…] (at most 64)},\"pupilSize\":1..100=50,\"darken\":"
     "0..100=50,\"layer\":id?=active,\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i} → "
     "{\"damage\":[x,y,w,h], \"pixels\":n}",
     en_redeye, run_redeye, true},
};

const pc_engine_command *pc_engine_redeye_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
