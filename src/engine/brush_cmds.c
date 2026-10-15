/* Brush engine commands (port of crates/engine/src/brush_cmds.rs, E10): the Brush, Pencil, Mixer
 * Brush and Color Replacement tools, brush presets and the session brush (tools.setBrush).
 *
 * Brushes are pc_paint_brush serialised as camelCase JSON; any command taking a "brush" object
 * deep-merges it onto the session brush, so partial objects work. Strokes are deterministic: the
 * jitter seed is `seed` or a hash of the points, so a journaled command replays to identical
 * pixels.
 *
 * The interactive live-preview helpers (LiveStroke / LiveCommit / LiveDab and the Session
 * live_commit seam) are a UI optimisation and are not ported here: the commit path always renders
 * the stroke fresh, which is byte-for-byte what the reference does when no live stroke was offered.
 * (Owed to an E10 follow-up.) */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "base/json.h"
#include "base/mem.h"
#include "color/blend.h"
#include "color/convert.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "geom/geom.h"
#include "paint/brush.h"
#include "paint/dynamics.h"
#include "paint/json.h"
#include "paint/mixer.h"
#include "paint/presets.h"
#include "paint/render.h"
#include "paint/replace.h"
#include "paint/rng.h"
#include "paint/tile.h"
#include "raster/surface.h"

#include "brush_cmds.h"
#include "channel_cmds.h"
#include "engine_internal.h"
#include "symmetry_cmds.h"

/* ========================================================================================== */
/* ToolState helpers (ToolState in crates/engine/src/lib.rs)                                   */
/* ========================================================================================== */

const float PC_ENGINE_MASK_COLORS[2][4] = {{1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};

bool pc_engine_tool_state_painting_default(pc_engine_tool_state *t)
{
    pc_paint_brush_default(&t->brush);
    /* Photoshop's Brush tool starts at 10 % Smoothing. */
    t->brush.smoothing.amount = 0.1f;
    pc_paint_mixer_state_default(&t->mixer);
    t->has_brush_gesture = false;
    t->brush_gesture_key = NULL;
    pc_paint_brush_default(&t->brush_gesture_start);
    t->current_preset = NULL;
    t->mask_targeted = false;
    memcpy(t->other_colors, PC_ENGINE_MASK_COLORS, sizeof t->other_colors);
    t->ncmyk_picks = 0;
    pc_paint_preset_list list = {0};
    if (pc_paint_presets_builtin(&list, NULL) != PC_OK) {
        t->presets = NULL;
        t->npresets = 0;
        return false;
    }
    t->presets = list.v;
    t->npresets = list.len;
    t->presets_rev = 0;
    return true;
}

void pc_engine_tool_state_fini(pc_engine_tool_state *t)
{
    if (t == NULL) {
        return;
    }
    pc_paint_brush_fini(&t->brush);
    pc_paint_brush_fini(&t->brush_gesture_start);
    for (size_t i = 0; i < t->npresets; i++) {
        pc_paint_preset_fini(&t->presets[i]);
    }
    free(t->presets);
    t->presets = NULL;
    t->npresets = 0;
    free(t->brush_gesture_key);
    t->brush_gesture_key = NULL;
    t->has_brush_gesture = false;
    free(t->current_preset);
    t->current_preset = NULL;
}

void pc_engine_tool_state_remember_cmyk(pc_engine_tool_state *t, const float rgb[3], const float cmyk[4])
{
    /* Drop an earlier pick of the same sRGB colour. */
    size_t w = 0;
    for (size_t i = 0; i < t->ncmyk_picks; i++) {
        const float *c = t->cmyk_picks[i].rgb;
        if (!(c[0] == rgb[0] && c[1] == rgb[1] && c[2] == rgb[2])) {
            if (w != i) {
                t->cmyk_picks[w] = t->cmyk_picks[i];
            }
            w++;
        }
    }
    t->ncmyk_picks = w;
    if (t->ncmyk_picks == PC_ENGINE_CMYK_PICKS) {
        memmove(&t->cmyk_picks[0], &t->cmyk_picks[1],
                (PC_ENGINE_CMYK_PICKS - 1) * sizeof t->cmyk_picks[0]);
        t->ncmyk_picks = PC_ENGINE_CMYK_PICKS - 1;
    }
    pc_engine_cmyk_pick *p = &t->cmyk_picks[t->ncmyk_picks++];
    p->rgb[0] = rgb[0];
    p->rgb[1] = rgb[1];
    p->rgb[2] = rgb[2];
    memcpy(p->inks, cmyk, sizeof p->inks);
}

bool pc_engine_tool_state_picked_cmyk(const pc_engine_tool_state *t, const float rgb[3], float out[4])
{
    for (size_t i = t->ncmyk_picks; i-- > 0;) {
        const float *c = t->cmyk_picks[i].rgb;
        if (c[0] == rgb[0] && c[1] == rgb[1] && c[2] == rgb[2]) {
            memcpy(out, t->cmyk_picks[i].inks, sizeof t->cmyk_picks[i].inks);
            return true;
        }
    }
    return false;
}

bool pc_engine_tool_state_target_mask(pc_engine_tool_state *t, bool mask)
{
    if (t->mask_targeted == mask) {
        return false;
    }
    t->mask_targeted = mask;
    float fg[4], bg[4];
    memcpy(fg, t->other_colors[0], sizeof fg);
    memcpy(bg, t->other_colors[1], sizeof bg);
    memcpy(t->other_colors[0], t->foreground, sizeof t->foreground);
    memcpy(t->other_colors[1], t->background, sizeof t->background);
    memcpy(t->foreground, fg, sizeof t->foreground);
    memcpy(t->background, bg, sizeof t->background);
    return true;
}

void pc_engine_brush_presets_changed(pc_engine_session *s)
{
    s->tools.presets_rev++;
    pc_engine_hook_sync_preset_store(s); /* no-op without a preset store (E2a/E2b) */
}

/* ========================================================================================== */
/* param helpers                                                                              */
/* ========================================================================================== */

static bool num_opt(yyjson_val *p, const char *k, float *out)
{
    double v;
    yyjson_val *x = pc_json_get(p, k);
    if (x != NULL && pc_json_as_f64(x, &v)) {
        *out = (float)v;
        return true;
    }
    return false;
}

static bool flag(yyjson_val *p, const char *k, bool d)
{
    bool v;
    yyjson_val *x = pc_json_get(p, k);
    return (x != NULL && pc_json_as_bool(x, &v)) ? v : d;
}

/* Parse "color": an [r,g,b,a?] array or a #rrggbb[aa] string; `def` otherwise. */
static void color_param(yyjson_val *v, const float def[4], float out[4])
{
    if (v != NULL && yyjson_is_arr(v) && yyjson_arr_size(v) >= 3) {
        double c[4] = {0, 0, 0, 1.0};
        for (size_t i = 0; i < 4; i++) {
            yyjson_val *e = yyjson_arr_get(v, i);
            double d;
            if (e != NULL && pc_json_as_f64(e, &d)) {
                c[i] = d;
            }
        }
        out[0] = (float)c[0];
        out[1] = (float)c[1];
        out[2] = (float)c[2];
        out[3] = (float)c[3];
        return;
    }
    size_t len = 0;
    const char *h = v != NULL ? pc_json_as_str(v, &len) : NULL;
    if (h != NULL) {
        if (len > 0 && h[0] == '#') { /* trim a single leading '#' as the reference does */
            h++;
            len--;
        }
        /* The reference accepts only #rrggbb or #rrggbbaa (6 or 8 hex digits). */
        if (len == 6 || len == 8) {
            char buf[10];
            buf[0] = '#';
            memcpy(buf + 1, h, len);
            buf[len + 1] = '\0';
            if (pc_engine_parse_hex(buf, len + 1, out)) {
                return;
            }
        }
    }
    memcpy(out, def, 4 * sizeof *out);
}

/* ========================================================================================== */
/* points                                                                                     */
/* ========================================================================================== */

pc_status pc_engine_check_coords(const pc_paint_point *pts, size_t n, const char *cmd, pc_error *err)
{
    for (size_t i = 0; i < n; i++) {
        if (!(fabs(pts[i].x) <= PC_ENGINE_BRUSH_MAX_COORD && fabs(pts[i].y) <= PC_ENGINE_BRUSH_MAX_COORD)) {
            return pc_engine_err_bad_params(err, cmd, "point coordinates must be finite and within +-%g",
                                            PC_ENGINE_BRUSH_MAX_COORD);
        }
    }
    return PC_OK;
}

pc_status pc_engine_parse_points(yyjson_val *p, const char *cmd, pc_paint_point **out, size_t *n,
                                 pc_error *err)
{
    yyjson_val *arr = pc_json_get(p, "points");
    if (arr == NULL || !yyjson_is_arr(arr)) {
        return pc_engine_err_bad_params(err, cmd, "missing `points`");
    }
    size_t cap = yyjson_arr_size(arr);
    pc_paint_point *pts = pc_malloc_array(cap == 0 ? 1 : cap, sizeof *pts);
    if (pts == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "points");
    }
    size_t cnt = 0;
    size_t idx, max;
    yyjson_val *v;
    yyjson_arr_foreach (arr, idx, max, v) {
        if (yyjson_is_arr(v)) {
            size_t m = yyjson_arr_size(v);
            double g0, g1;
            if (m < 2 || !pc_json_as_f64(yyjson_arr_get(v, 0), &g0) ||
                !pc_json_as_f64(yyjson_arr_get(v, 1), &g1)) {
                continue; /* filter_map drops malformed entries */
            }
            pc_paint_point sp = pc_paint_point_default();
            sp.x = g0;
            sp.y = g1;
            double d;
            sp.pressure = (m > 2 && pc_json_as_f64(yyjson_arr_get(v, 2), &d)) ? (float)d : 1.0f;
            sp.tilt_x = (m > 3 && pc_json_as_f64(yyjson_arr_get(v, 3), &d)) ? (float)d : 0.0f;
            sp.tilt_y = (m > 4 && pc_json_as_f64(yyjson_arr_get(v, 4), &d)) ? (float)d : 0.0f;
            sp.rotation = (m > 5 && pc_json_as_f64(yyjson_arr_get(v, 5), &d)) ? (float)d : 0.0f;
            sp.time = (m > 6 && pc_json_as_f64(yyjson_arr_get(v, 6), &d)) ? d : 0.0;
            sp.wheel = (m > 7 && pc_json_as_f64(yyjson_arr_get(v, 7), &d)) ? (float)d : 1.0f;
            pts[cnt++] = sp;
        } else if (yyjson_is_obj(v)) {
            pc_paint_point sp;
            if (pc_paint_json_read(PC_PAINT_JSON_POINT, v, &sp, NULL) == PC_OK) {
                pts[cnt++] = sp;
            }
        }
    }
    if (cnt == 0) {
        free(pts);
        return pc_engine_err_bad_params(err, cmd, "`points` is empty");
    }
    if (pc_engine_check_coords(pts, cnt, cmd, err) != PC_OK) {
        free(pts);
        return PC_ERR_INVALID;
    }
    *out = pts;
    *n = cnt;
    return PC_OK;
}

/* Compute a stroke seed from `points` the way the reference hashes serde_json's text. */
static uint64_t seed_from_points(yyjson_val *p)
{
    yyjson_val *pts = pc_json_get(p, "points");
    if (pts == NULL) {
        return pc_paint_seed_from_bytes((const uint8_t *)"", 0);
    }
    pc_buf buf = PC_BUF_INIT;
    if (pc_json_write_val(pts, PC_JSON_SORT_KEYS, &buf, NULL) != PC_OK) {
        pc_buf_fini(&buf);
        return pc_paint_seed_from_bytes((const uint8_t *)"", 0);
    }
    uint64_t seed = pc_paint_seed_from_bytes(buf.data, buf.len);
    pc_buf_fini(&buf);
    return seed;
}

/* ========================================================================================== */
/* JSON brush merge (merge / merge_brush)                                                      */
/* ========================================================================================== */

/* base (a serialised value) overlaid with the immutable patch, as a new value in d (merge()). */
static yyjson_mut_val *merge_vals(yyjson_mut_doc *d, yyjson_mut_val *base, yyjson_val *patch)
{
    if (!yyjson_mut_is_obj(base) || !yyjson_is_obj(patch)) {
        return pc_json_normalized_copy_val(d, patch);
    }
    size_t bn = yyjson_mut_obj_size(base), pn = yyjson_obj_size(patch);
    if (bn == 1 && pn == 1) {
        yyjson_mut_obj_iter bit;
        yyjson_mut_obj_iter_init(base, &bit);
        yyjson_mut_val *bk = yyjson_mut_obj_iter_next(&bit);
        yyjson_obj_iter pit;
        yyjson_obj_iter_init(patch, &pit);
        yyjson_val *pk = yyjson_obj_iter_next(&pit);
        const char *bks = yyjson_mut_get_str(bk), *pks = yyjson_get_str(pk);
        if (bks != NULL && pks != NULL && strcmp(bks, pks) != 0) {
            return pc_json_normalized_copy_val(d, patch);
        }
    }
    yyjson_mut_val *res = yyjson_mut_obj(d);
    if (res == NULL) {
        return NULL;
    }
    yyjson_mut_obj_iter bit;
    yyjson_mut_obj_iter_init(base, &bit);
    yyjson_mut_val *bk;
    while ((bk = yyjson_mut_obj_iter_next(&bit)) != NULL) {
        yyjson_mut_val *bv = yyjson_mut_obj_iter_get_val(bk);
        const char *k = yyjson_mut_get_str(bk);
        yyjson_val *pv = k != NULL ? pc_json_get(patch, k) : NULL;
        yyjson_mut_val *mv = pv != NULL ? merge_vals(d, bv, pv) : yyjson_mut_val_mut_copy(d, bv);
        yyjson_mut_val *key = yyjson_mut_strcpy(d, k != NULL ? k : "");
        if (mv == NULL || key == NULL || !yyjson_mut_obj_add(res, key, mv)) {
            return NULL;
        }
    }
    yyjson_obj_iter pit;
    yyjson_obj_iter_init(patch, &pit);
    yyjson_val *pk;
    while ((pk = yyjson_obj_iter_next(&pit)) != NULL) {
        const char *k = yyjson_get_str(pk);
        if (k == NULL || yyjson_mut_obj_get(base, k) != NULL) {
            continue;
        }
        yyjson_mut_val *copy = pc_json_normalized_copy_val(d, yyjson_obj_iter_get_val(pk));
        yyjson_mut_val *key = yyjson_mut_strcpy(d, k);
        if (copy == NULL || key == NULL || !yyjson_mut_obj_add(res, key, copy)) {
            return NULL;
        }
    }
    return res;
}

pc_status pc_engine_merge_brush(const pc_paint_brush *base, yyjson_val *patch, const char *cmd,
                               pc_paint_brush *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "brush");
    }
    yyjson_mut_val *basev = pc_paint_json_write(PC_PAINT_JSON_BRUSH, d, base, 0);
    if (basev == NULL) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "brush");
    }
    yyjson_mut_val *merged = merge_vals(d, basev, patch);
    if (merged == NULL) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "brush");
    }
    pc_buf buf = PC_BUF_INIT;
    if (pc_json_write(merged, 0, &buf, err) != PC_OK) {
        yyjson_mut_doc_free(d);
        pc_buf_fini(&buf);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_free(d);
    yyjson_doc *idoc = NULL;
    if (pc_json_parse((const char *)buf.data, buf.len, &idoc, err) != PC_OK) {
        pc_buf_fini(&buf);
        return PC_ERR_NOMEM;
    }
    pc_buf_fini(&buf);
    pc_error inner = {0};
    pc_status rc = pc_paint_json_read(PC_PAINT_JSON_BRUSH, yyjson_doc_get_root(idoc), out, &inner);
    yyjson_doc_free(idoc);
    if (rc != PC_OK) {
        return pc_engine_err_bad_params(err, cmd, "invalid brush: %s", inner.msg);
    }
    return PC_OK;
}

pc_status pc_engine_validate_brush(const pc_paint_brush *b, const char *cmd, pc_error *err)
{
    float max = PC_PAINT_MAX_BRUSH_SIZE;
    if (!(isfinite(b->size)) || b->size > max) {
        return pc_engine_err_bad_params(err, cmd, "brush size must be finite and at most %g px", max);
    }
    if (b->dual_brush.enabled && (!(isfinite(b->dual_brush.size)) || b->dual_brush.size > max)) {
        return pc_engine_err_bad_params(err, cmd, "dual brush size must be finite and at most %g px", max);
    }
    float scatter_max = PC_PAINT_MAX_SCATTER;
    float sc = b->scattering.scatter.jitter;
    if (b->scattering.enabled && !(sc >= 0.0f && sc <= scatter_max)) {
        return pc_engine_err_bad_params(
            err, cmd, "scatter amount %g is out of range: it must be between 0 and %g (1000 %%)", sc,
            scatter_max);
    }
    float ds = b->dual_brush.scatter;
    if (b->dual_brush.enabled && !(ds >= 0.0f && ds <= scatter_max)) {
        return pc_engine_err_bad_params(
            err, cmd,
            "dual brush scatter amount %g is out of range: it must be between 0 and %g (1000 %%)", ds,
            scatter_max);
    }
    return PC_OK;
}

yyjson_mut_val *pc_engine_brush_json(yyjson_mut_doc *d, const pc_paint_brush *b)
{
    return pc_paint_json_write(PC_PAINT_JSON_BRUSH, d, b, 0);
}

static pc_status brush_json_result(const pc_paint_brush *b, pc_json_value *out, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = pc_engine_brush_json(d, b);
    if (root == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "brush");
    }
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ========================================================================================== */
/* blend / target predicates (crate::commands)                                                */
/* ========================================================================================== */

/* Normalise a blend name as blend_from_str does: lowercase, drop spaces/_/-/()/. */
static void blend_norm(const char *s, size_t len, char *out, size_t cap)
{
    size_t w = 0;
    for (size_t i = 0; i < len && w + 1 < cap; i++) {
        char c = s[i];
        if (c == ' ' || c == '_' || c == '-' || c == '(' || c == ')') {
            continue;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[w++] = c;
    }
    out[w] = '\0';
}

bool pc_engine_blend_from_str(const char *s, size_t len, pc_blend_mode *out)
{
    char want[64];
    blend_norm(s, len, want, sizeof want);
    /* PassThrough first, then every layer mode (Photoshop then Paint.NET). */
    pc_blend_mode all[1 + PC_BLEND_ALL_LAYER_MODE_COUNT];
    size_t n = 0;
    all[n++] = PC_BLEND_PASS_THROUGH;
    for (size_t i = 0; i < PC_BLEND_LAYER_MODE_COUNT; i++) {
        all[n++] = PC_BLEND_LAYER_MODES[i];
    }
    for (size_t i = 0; i < PC_BLEND_PAINT_NET_MODE_COUNT; i++) {
        all[n++] = PC_BLEND_PAINT_NET_MODES[i];
    }
    char buf[64];
    for (size_t i = 0; i < n; i++) {
        const char *label = pc_blend_label(all[i]);
        if (label != NULL) {
            blend_norm(label, strlen(label), buf, sizeof buf);
            if (strcmp(buf, want) == 0) {
                *out = all[i];
                return true;
            }
        }
        const char *name = pc_blend_name(all[i]);
        if (name != NULL) {
            blend_norm(name, strlen(name), buf, sizeof buf);
            if (strcmp(buf, want) == 0) {
                *out = all[i];
                return true;
            }
        }
    }
    return false;
}

bool pc_engine_is_mask_target(yyjson_val *p)
{
    return pc_engine_target_of(p, NULL) == PC_ENGINE_PIXTGT_MASK;
}

pc_status pc_engine_has_paintable(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_edits_channel(s)) {
        return PC_OK;
    }
    pc_error why = {0};
    const pc_layer *l = pc_engine_active_layer_of(s, &why);
    if (l == NULL) {
        if (reason != NULL) {
            *reason = why;
        }
        return PC_ERR_INVALID;
    }
    if (l->content.kind == PC_LAYER_RASTER || l->has_mask) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "active layer is %s %s layer without a mask",
                   pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
}

/* ---- locks and the write surface --------------------------------------------------------- */

static pc_status check_pixels_unlocked(pc_document *doc, pc_layer_id id, pc_error *err)
{
    pc_locks locks = pc_document_effective_locks(doc, id);
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (locks.pixels || locks.all) {
        return pc_engine_err_other(
            err, "Could not complete your request because the layer \"%s\" is locked", pc_str_c(&l->name));
    }
    return PC_OK;
}

static pc_status check_all_unlocked(pc_document *doc, pc_layer_id id, pc_error *err)
{
    pc_locks locks = pc_document_effective_locks(doc, id);
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (locks.all) {
        return pc_engine_err_other(
            err, "Could not complete your request because the layer \"%s\" is locked", pc_str_c(&l->name));
    }
    return PC_OK;
}

pc_status pc_engine_paint_surface(pc_document *doc, pc_layer_id id, yyjson_val *p, pc_surface **out,
                                  pc_error *err)
{
    bool mask = pc_engine_is_mask_target(p);
    pc_status rc = mask ? check_all_unlocked(doc, id, err) : check_pixels_unlocked(doc, id, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer *l = pc_document_layer_mut(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    if (mask) {
        if (!l->has_mask) {
            return pc_engine_err_other(err, "layer has no mask");
        }
        *out = &l->mask.surface;
        return PC_OK;
    }
    pc_surface *surf = pc_layer_surface_mut(l);
    if (surf == NULL) {
        return pc_engine_err_other(err, "not a pixel layer");
    }
    *out = surf;
    return PC_OK;
}

pc_status pc_engine_target_surface(pc_document *doc, bool has_layer, pc_layer_id layer, yyjson_val *p,
                                   pc_surface **out, bool *lock, pc_error *err)
{
    size_t ai = 0;
    pc_engine_pixel_target t = pc_engine_target_of(p, &ai);
    if (t == PC_ENGINE_PIXTGT_ALPHA) {
        if (ai >= doc->nchannels) {
            return pc_engine_err_other(err, "no alpha channel %zu (document has %zu)", ai, doc->nchannels);
        }
        *out = &doc->channels[ai].surface;
        *lock = false;
        return PC_OK;
    }
    if (t == PC_ENGINE_PIXTGT_QUICK_MASK) {
        if (!doc->has_quick_mask) {
            return pc_engine_err_other(err, "not in Quick Mask mode");
        }
        *out = &doc->quick_mask.surface;
        *lock = false;
        return PC_OK;
    }
    if (!has_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    pc_locks locks = pc_document_effective_locks(doc, layer);
    *lock = locks.transparency && t == PC_ENGINE_PIXTGT_PIXELS;
    return pc_engine_paint_surface(doc, layer, p, out, err);
}

/* ========================================================================================== */
/* brush resolution                                                                           */
/* ========================================================================================== */

pc_cmyk_space *pc_engine_doc_cmyk_space(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return NULL;
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (doc->icc_profile == NULL) {
        return NULL;
    }
    return pc_cmyk_space_for_profile(pc_blob_data(doc->icc_profile), pc_blob_len(doc->icc_profile));
}

/* b (a preset) picked over the tool's current brush, honouring the preset's captures. */
static void preset_picked_over(const pc_paint_preset *pr, const pc_paint_brush *current, pc_paint_brush *out)
{
    pc_paint_brush_copy(out, &pr->brush);
    pc_paint_brush_picked_over(out, current);
    if (!pr->captures.size) {
        out->size = current->size;
    }
    if (!pr->captures.tool_settings) {
        out->mode = current->mode;
        out->opacity = current->opacity;
        out->flow = current->flow;
        out->pressure_size = current->pressure_size;
        out->pressure_opacity = current->pressure_opacity;
    }
}

static const pc_paint_preset *find_preset(const pc_engine_session *s, const char *name, size_t len)
{
    return pc_paint_presets_find(s->tools.presets, s->tools.npresets, name, len);
}

/* The tool's brush always paints with full tips. Without a preset store, a Stored tip can't be
 * loaded (built-ins carry their bitmaps inline, so this is a no-op for them). */
static pc_status load_brush_tips(const pc_engine_session *s, pc_paint_brush *b, const char *cmd,
                                 pc_error *err)
{
    (void)s;
    if (b->tip.kind == PC_PAINT_TIP_STORED || b->dual_brush.tip.kind == PC_PAINT_TIP_STORED ||
        b->texture.pattern.kind == PC_PAINT_PATTERN_STORED) {
        return pc_engine_err_bad_params(err, cmd,
                                        "brush tip is not available: no preset store is attached");
    }
    return PC_OK;
}

static void apply_color_from_params(const pc_engine_session *s, yyjson_val *p, pc_paint_brush *b)
{
    color_param(pc_json_get(p, "color"), s->tools.foreground, b->color);
    float inks[4];
    float rgb[3] = {b->color[0], b->color[1], b->color[2]};
    if (pc_engine_tool_state_picked_cmyk(&s->tools, rgb, inks)) {
        b->color_cmyk_set = true;
        b->color_cmyk_rgb[0] = rgb[0];
        b->color_cmyk_rgb[1] = rgb[1];
        b->color_cmyk_rgb[2] = rgb[2];
        memcpy(b->color_cmyk_inks, inks, sizeof inks);
    } else {
        b->color_cmyk_set = false;
    }
    memcpy(b->background, s->tools.background, sizeof b->background);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

pc_status pc_engine_resolve_brush(const pc_engine_session *s, yyjson_val *p, const char *cmd,
                                 pc_paint_brush *out, pc_error *err)
{
    pc_paint_brush b = {0};
    pc_paint_brush_copy(&b, &s->tools.brush);
    size_t len = 0;
    yyjson_val *pv = pc_json_get(p, "preset");
    const char *preset = pv != NULL ? pc_json_as_str(pv, &len) : NULL;
    if (preset != NULL) {
        const pc_paint_preset *pr = find_preset(s, preset, len);
        if (pr == NULL) {
            pc_paint_brush_fini(&b);
            return pc_engine_err_bad_params(err, cmd, "no brush preset named `%.*s`", (int)len, preset);
        }
        pc_paint_brush picked = {0};
        preset_picked_over(pr, &s->tools.brush, &picked);
        pc_paint_brush_fini(&b);
        b = picked;
    }
    yyjson_val *patch = pc_json_get(p, "brush");
    if (patch != NULL && yyjson_is_obj(patch)) {
        pc_paint_brush merged = {0};
        pc_status rc = pc_engine_merge_brush(&b, patch, cmd, &merged, err);
        pc_paint_brush_fini(&b);
        if (rc != PC_OK) {
            return rc;
        }
        b = merged;
    }
    pc_status rc = load_brush_tips(s, &b, cmd, err);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&b);
        return rc;
    }
    float v;
    if (num_opt(p, "size", &v)) {
        b.size = v > 0.5f ? v : 0.5f;
    }
    if (num_opt(p, "hardness", &v)) {
        b.hardness = clampf(v, 0.0f, 1.0f);
    }
    if (num_opt(p, "opacity", &v)) {
        b.opacity = clampf(v, 0.0f, 1.0f);
    }
    if (num_opt(p, "flow", &v)) {
        b.flow = clampf(v, 0.0f, 1.0f);
    }
    if (num_opt(p, "spacing", &v)) {
        b.spacing = clampf(v, 0.01f, 10.0f);
    }
    if (num_opt(p, "smoothing", &v)) {
        b.smoothing.enabled = true;
        b.smoothing.amount = clampf(v, 0.0f, 1.0f);
    }
    apply_color_from_params(s, p, &b);
    b.erase = flag(p, "erase", false);
    uint64_t seed;
    yyjson_val *sv = pc_json_get(p, "seed");
    b.seed = (sv != NULL && pc_json_as_u64(sv, &seed)) ? seed : seed_from_points(p);
    rc = pc_engine_validate_brush(&b, cmd, err);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&b);
        return rc;
    }
    *out = b;
    return PC_OK;
}

/* ========================================================================================== */
/* stroke commit                                                                              */
/* ========================================================================================== */

static bool layer_id_of(const pc_engine_session *s, yyjson_val *p, pc_layer_id *out)
{
    uint64_t id;
    yyjson_val *lv = pc_json_get(p, "layer");
    if (lv != NULL && pc_json_as_u64(lv, &id)) {
        *out = id;
        return true;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && d->has_active_layer) {
        *out = d->active_layer;
        return true;
    }
    return false;
}

static void set_damage(pc_engine_session *s, pc_rect dmg)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->last_damage = dmg;
        st->has_last_damage = true;
    }
}

static pc_status damage_result(pc_engine_session *s, pc_rect dmg, pc_json_value *out, pc_error *err)
{
    set_damage(s, dmg);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, a, dmg.x0);
    yyjson_mut_arr_add_int(d, a, dmg.y0);
    yyjson_mut_arr_add_int(d, a, (int64_t)pc_rect_width(dmg));
    yyjson_mut_arr_add_int(d, a, (int64_t)pc_rect_height(dmg));
    yyjson_mut_obj_add_val(d, root, "damage", a);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static bool clear_mode(yyjson_val *p)
{
    size_t len = 0;
    yyjson_val *m = pc_json_get(p, "mode");
    const char *s = m != NULL ? pc_json_as_str(m, &len) : NULL;
    return s != NULL && len == 5 && (s[0] == 'c' || s[0] == 'C') &&
           (strncasecmp(s, "clear", 5) == 0);
}

/* The options-bar blend mode. "mode" accepts any blend name, or "clear" to erase with the brush. */
static void with_blend_mode(pc_paint_brush *b, yyjson_val *p)
{
    if (clear_mode(p)) {
        b->erase = true;
    }
    if (b->erase) {
        b->mode = PC_BLEND_NORMAL;
        return;
    }
    size_t len = 0;
    yyjson_val *m = pc_json_get(p, "mode");
    const char *s = m != NULL ? pc_json_as_str(m, &len) : NULL;
    pc_blend_mode mode;
    if (s != NULL && pc_engine_blend_from_str(s, len, &mode)) {
        b->mode = mode;
    }
}

/* Erasing a layer with locked transparency paints the background colour instead. */
static void erase_locked(pc_paint_brush *b, bool lock, const float bg[4])
{
    if (b->erase && lock) {
        b->erase = false;
        memcpy(b->color, bg, 4 * sizeof *bg);
    }
}

/* Pencil Auto Erase: starting on a foreground-colour pixel paints the background colour. */
static void apply_auto_erase(pc_paint_brush *b, const pc_surface *surf, const pc_paint_point *start,
                             const float fg[4], const float bg[4])
{
    if (start == NULL) {
        return;
    }
    float c[4];
    pc_surface_rgba(surf, (int32_t)floor(start->x), (int32_t)floor(start->y), c);
    if (c[3] > 0.0f && fabsf(c[0] - fg[0]) < 1.5f / 255.0f && fabsf(c[1] - fg[1]) < 1.5f / 255.0f &&
        fabsf(c[2] - fg[2]) < 1.5f / 255.0f) {
        memcpy(b->color, bg, 4 * sizeof *bg);
    }
}

/* The target layer (or none for a channel), brush and zoom of a stroke. On a mask or channel the
 * eraser paints the background colour (stroke_target). */
static pc_status stroke_target(const pc_engine_session *s, yyjson_val *p, pc_paint_brush *brush,
                               bool *has_layer, pc_layer_id *layer, float *zoom, const char *cmd,
                               pc_error *err)
{
    bool gray = pc_engine_is_mask_target(p) || pc_engine_is_channel_target(p);
    if (pc_engine_is_channel_target(p)) {
        *has_layer = false;
    } else {
        if (!layer_id_of(s, p, layer)) {
            return pc_engine_err_other(err, "no active layer");
        }
        *has_layer = true;
    }
    if (gray && brush->erase) {
        brush->erase = false;
        memcpy(brush->color, s->tools.background, sizeof brush->color);
    }
    float z;
    *zoom = num_opt(p, "zoom", &z) ? z : 1.0f;
    (void)cmd;
    return PC_OK;
}

/* Everything the stroke commit needs, passed through pc_engine_session_edit. */
struct stroke_ctx {
    const pc_paint_brush *brush; /* borrowed */
    const pc_paint_point *pts;
    size_t npts;
    float fg[4]; /* resolved brush colour, before erase_locked */
    const float *bg;
    float zoom;
    bool has_layer;
    pc_layer_id layer;
    yyjson_val *p;
    const struct pc_engine_symmetry *sym;
    bool auto_erase;
    pc_rect damage; /* out */
};

/* Render with symmetry, if any; else a plain stroke. surf/sel already resolved. */
static pc_status render_symmetric(struct stroke_ctx *c, pc_surface *surf, const pc_surface *sel,
                                  bool lock, pc_rect *damage, pc_error *err)
{
    const pc_paint_brush *brush = c->brush;
    pc_pixel_format fmt = pc_surface_format(surf);
    if (c->sym == NULL) {
        return pc_paint_render_stroke(surf, brush, c->pts, c->npts, sel, lock, c->zoom, damage, err);
    }
    pc_paint_point_vec passes[3] = {{0}, {0}, {0}};
    size_t cnt = 0;
    pc_status rc = pc_engine_symmetry_reflected_passes(c->sym, c->pts, c->npts, passes, &cnt, err);
    pc_paint_renderer *copies[3] = {NULL, NULL, NULL};
    size_t ncopies = 0;
    if (rc == PC_OK) {
        for (size_t i = 0; i < cnt; i++) {
            if (!pc_engine_symmetry_has_distinct_mirror(c->pts, passes[i].v, c->npts)) {
                continue;
            }
            pc_paint_renderer *r = NULL;
            rc = pc_paint_renderer_new(brush, &fmt, c->zoom, &r, err);
            if (rc != PC_OK) {
                break;
            }
            rc = pc_paint_renderer_push(r, passes[i].v, passes[i].len, err);
            if (rc == PC_OK) {
                rc = pc_paint_renderer_finish(r, err);
            }
            if (rc != PC_OK) {
                pc_paint_renderer_free(r);
                break;
            }
            copies[ncopies++] = r;
        }
    }
    if (rc == PC_OK) {
        if (ncopies == 0) {
            rc = pc_paint_render_stroke(surf, brush, c->pts, c->npts, sel, lock, c->zoom, damage, err);
        } else {
            pc_surface pre;
            rc = pc_surface_clone(surf, &pre, err);
            if (rc == PC_OK) {
                pc_paint_renderer *orig = NULL;
                rc = pc_paint_renderer_new(brush, &fmt, c->zoom, &orig, err);
                if (rc == PC_OK) {
                    rc = pc_paint_renderer_push(orig, c->pts, c->npts, err);
                    if (rc == PC_OK) {
                        rc = pc_paint_renderer_finish(orig, err);
                    }
                    if (rc == PC_OK) {
                        rc = pc_paint_renderer_composite_union_many(
                            orig, (const pc_paint_renderer *const *)copies, ncopies, &pre, surf, sel,
                            lock, damage, err);
                    }
                    pc_paint_renderer_free(orig);
                }
                pc_surface_fini(&pre);
            }
        }
    }
    for (size_t i = 0; i < ncopies; i++) {
        pc_paint_renderer_free(copies[i]);
    }
    for (size_t i = 0; i < 3; i++) {
        pc_paint_point_vec_fini(&passes[i]);
    }
    return rc;
}

static pc_status stroke_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct stroke_ctx *c = vctx;
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, c->has_layer, c->layer, c->p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_paint_brush brush = {0};
    pc_paint_brush_copy(&brush, c->brush);
    erase_locked(&brush, lock, c->bg);
    if (c->auto_erase) {
        apply_auto_erase(&brush, surf, c->npts > 0 ? &c->pts[0] : NULL, c->fg, c->bg);
    }
    struct stroke_ctx cc = *c;
    cc.brush = &brush;
    pc_rect dmg = PC_RECT_EMPTY;
    rc = render_symmetric(&cc, surf, sel, lock, &dmg, err);
    pc_paint_brush_fini(&brush);
    if (rc != PC_OK) {
        return rc;
    }
    c->damage = dmg;
    return PC_OK;
}

/* Run a resolved brush stroke onto the target (stroke_with). Consumes `brush` (finalises it). */
static pc_status stroke_with(pc_engine_session *s, yyjson_val *p, const char *label, pc_paint_brush brush,
                             pc_paint_point *pts, size_t npts, bool auto_erase, pc_json_value *out,
                             pc_error *err)
{
    float bg[4];
    memcpy(bg, s->tools.background, sizeof bg);
    float fg[4];
    memcpy(fg, brush.color, sizeof fg);
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    const struct pc_engine_symmetry *sym = st != NULL ? st->symmetry : NULL;

    bool has_layer = false;
    pc_layer_id layer = 0;
    float zoom = 1.0f;
    pc_status rc = stroke_target(s, p, &brush, &has_layer, &layer, &zoom, "paint.stroke", err);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&brush);
        free(pts);
        return rc;
    }

    struct stroke_ctx ctx = {0};
    ctx.brush = &brush;
    ctx.pts = pts;
    ctx.npts = npts;
    memcpy(ctx.fg, fg, sizeof fg);
    ctx.bg = bg;
    ctx.zoom = zoom;
    ctx.has_layer = has_layer;
    ctx.layer = layer;
    ctx.p = p;
    ctx.sym = sym;
    ctx.auto_erase = auto_erase;
    ctx.damage = PC_RECT_EMPTY;

    pc_cmyk_space *space = pc_engine_doc_cmyk_space(s);
    pc_cmyk_space *prev = space != NULL ? pc_cmyk_space_enter(space) : NULL;
    rc = pc_engine_session_edit(s, label, stroke_edit, &ctx, err);
    if (space != NULL) {
        pc_cmyk_space_leave(prev);
        pc_cmyk_space_release(space);
    }
    pc_paint_brush_fini(&brush);
    free(pts);
    if (rc != PC_OK) {
        return rc;
    }
    return damage_result(s, ctx.damage, out, err);
}

/* ---- paint.stroke / paint.pencil --------------------------------------------------------- */

static pc_status run_stroke(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_paint_point *pts = NULL;
    size_t npts = 0;
    pc_status rc = pc_engine_parse_points(p, "paint.stroke", &pts, &npts, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_paint_brush brush = {0};
    rc = pc_engine_resolve_brush(s, p, "paint.stroke", &brush, err);
    if (rc != PC_OK) {
        free(pts);
        return rc;
    }
    with_blend_mode(&brush, p);
    const char *label = (brush.erase && !clear_mode(p)) ? "Eraser" : "Brush Tool";
    return stroke_with(s, p, label, brush, pts, npts, false, out, err);
}

/* The Pencil's brush: aliased, hard, full flow, options-bar mode; or the Eraser's Block. */
static pc_status pencil_brush(pc_engine_session *s, yyjson_val *p, pc_paint_brush *out, pc_error *err)
{
    pc_paint_brush b = {0};
    pc_status rc = pc_engine_resolve_brush(s, p, "paint.pencil", &b, err);
    if (rc != PC_OK) {
        return rc;
    }
    with_blend_mode(&b, p);
    if (flag(p, "block", false)) {
        /* The Eraser's Block: a hard, aliased square of 16/zoom document pixels. */
        double zoom = 1.0;
        yyjson_val *zv = pc_json_get(p, "zoom");
        if (zv != NULL) {
            double z;
            if (!pc_json_as_f64(zv, &z) || !(isfinite(z) && z > 0.0)) {
                pc_paint_brush_fini(&b);
                return pc_engine_err_bad_params(err, "paint.pencil", "`zoom` must be a positive number");
            }
            zoom = z;
        }
        double side = 16.0 / zoom;
        side = floor(side + 0.5);
        if (side < 1.0) {
            side = 1.0;
        } else if (side > (double)PC_PAINT_MAX_BRUSH_SIZE) {
            side = (double)PC_PAINT_MAX_BRUSH_SIZE;
        }
        pc_paint_brush blk = {0};
        pc_paint_brush_default(&blk);
        blk.size = (float)side;
        blk.aliased = true;
        blk.square = true;
        blk.pressure_size = false;
        blk.pressure_opacity = false;
        blk.spacing = 0.25f;
        memcpy(blk.color, b.color, sizeof blk.color);
        blk.color_cmyk_set = b.color_cmyk_set;
        memcpy(blk.color_cmyk_rgb, b.color_cmyk_rgb, sizeof blk.color_cmyk_rgb);
        memcpy(blk.color_cmyk_inks, b.color_cmyk_inks, sizeof blk.color_cmyk_inks);
        memcpy(blk.background, b.background, sizeof blk.background);
        blk.erase = b.erase;
        blk.smoothing = b.smoothing;
        blk.seed = b.seed;
        pc_paint_brush_fini(&b);
        *out = blk;
        return PC_OK;
    }
    b.aliased = true;
    float v;
    if (!num_opt(p, "hardness", &v)) {
        b.hardness = 1.0f;
    }
    if (!num_opt(p, "flow", &v)) {
        b.flow = 1.0f;
    }
    *out = b;
    return PC_OK;
}

static pc_status run_pencil(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    pc_paint_point *pts = NULL;
    size_t npts = 0;
    pc_status rc = pc_engine_parse_points(p, "paint.pencil", &pts, &npts, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_paint_brush brush = {0};
    rc = pencil_brush(s, p, &brush, err);
    if (rc != PC_OK) {
        free(pts);
        return rc;
    }
    const char *label = (brush.erase && !clear_mode(p)) ? "Eraser" : "Pencil";
    return stroke_with(s, p, label, brush, pts, npts, flag(p, "autoErase", false), out, err);
}

/* ---- paint.mixerBrush -------------------------------------------------------------------- */

static float pct(yyjson_val *p, const char *k, float d)
{
    float v;
    return clampf(num_opt(p, k, &v) ? v : d, 0.0f, 100.0f) / 100.0f;
}

struct mixer_ctx {
    const pc_paint_stroke *stroke;
    pc_paint_mixer_settings m;
    bool sample_all;
    bool has_layer;
    pc_layer_id layer;
    yyjson_val *p;
    pc_paint_mixer_state *state;
    pc_rect damage;
};

static pc_status mixer_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct mixer_ctx *c = vctx;
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_surface sample = {0};
    bool have_sample = false;
    if (c->sample_all) {
        pc_paint_dab_vec dabs = {0};
        pc_status rc = pc_paint_dabs(&c->stroke->brush, c->stroke->points, c->stroke->count, &dabs, err);
        if (rc != PC_OK) {
            pc_paint_dab_vec_fini(&dabs);
            return rc;
        }
        pc_paint_context ctx;
        rc = pc_paint_context_init(&ctx, &c->stroke->brush, err);
        if (rc != PC_OK) {
            pc_paint_dab_vec_fini(&dabs);
            return rc;
        }
        pc_rect area = PC_RECT_EMPTY;
        for (size_t i = 0; i < dabs.len; i++) {
            area = pc_rect_union(area, pc_paint_context_dab_rect(&ctx, &dabs.v[i], false));
        }
        pc_paint_context_fini(&ctx);
        pc_paint_dab_vec_fini(&dabs);
        pc_compose_buffer buf = {0};
        rc = pc_compose_render(doc, area, &buf, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_init(&sample, PC_PIXEL_FORMAT_RGBA32F);
        if (!pc_rect_is_empty(area) && buf.len > 0) {
            rc = pc_surface_write_region(&sample, area, (const float *)buf.px, buf.len * 4, err);
            if (rc != PC_OK) {
                pc_compose_buffer_fini(&buf);
                pc_surface_fini(&sample);
                return rc;
            }
        }
        pc_compose_buffer_fini(&buf);
        have_sample = true;
    }
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, c->has_layer, c->layer, c->p, &surf, &lock, err);
    if (rc != PC_OK) {
        if (have_sample) {
            pc_surface_fini(&sample);
        }
        return rc;
    }
    pc_rect dmg = PC_RECT_EMPTY;
    rc = pc_paint_apply_mixer_stroke(surf, have_sample ? &sample : NULL, c->stroke, &c->m, c->state, sel,
                                     lock, &dmg, err);
    if (have_sample) {
        pc_surface_fini(&sample);
    }
    if (rc != PC_OK) {
        return rc;
    }
    c->damage = dmg;
    return PC_OK;
}

static pc_status run_mixer(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "paint.mixerBrush";
    pc_paint_point *pts = NULL;
    size_t npts = 0;
    pc_status rc = pc_engine_parse_points(p, cmd, &pts, &npts, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_paint_brush brush = {0};
    rc = pc_engine_resolve_brush(s, p, cmd, &brush, err);
    if (rc != PC_OK) {
        free(pts);
        return rc;
    }
    const pc_paint_mixer_settings *bm = &brush.mixer;
    pc_paint_mixer_settings m = {0};
    m.wet = pct(p, "wet", bm->wet * 100.0f);
    m.load = pct(p, "load", bm->load * 100.0f);
    m.mix = pct(p, "mix", bm->mix * 100.0f);
    m.flow = pct(p, "flow", bm->flow * 100.0f);
    m.sample_all_layers = flag(p, "sampleAllLayers", bm->sample_all_layers);
    bool clean = flag(p, "cleanAfterStroke", true);
    bool load_after = flag(p, "loadAfterStroke", true);

    pc_paint_mixer_state state = s->tools.mixer;
    if (load_after || !state.has_reservoir) {
        float c[4];
        color_param(pc_json_get(p, "color"), s->tools.foreground, c);
        pc_paint_mixer_state_load(&state, c);
    }

    bool has_layer = false;
    pc_layer_id layer = 0;
    if (!pc_engine_is_channel_target(p)) {
        if (!layer_id_of(s, p, &layer)) {
            pc_paint_brush_fini(&brush);
            free(pts);
            return pc_engine_err_other(err, "no active layer");
        }
        has_layer = true;
    }

    pc_paint_stroke stroke = {0};
    stroke.brush = brush; /* moved: finalised via stroke.brush below */
    stroke.points = pts;
    stroke.count = npts;

    struct mixer_ctx ctx = {0};
    ctx.stroke = &stroke;
    ctx.m = m;
    ctx.sample_all = m.sample_all_layers;
    ctx.has_layer = has_layer;
    ctx.layer = layer;
    ctx.p = p;
    ctx.state = &state;
    ctx.damage = PC_RECT_EMPTY;

    rc = pc_engine_session_edit(s, "Mixer Brush", mixer_edit, &ctx, err);
    pc_paint_stroke_fini(&stroke); /* frees brush + points */
    if (rc != PC_OK) {
        return rc;
    }
    if (clean) {
        pc_paint_mixer_state_clean(&state);
    }
    s->tools.mixer = state;
    return damage_result(s, ctx.damage, out, err);
}

/* ---- paint.colorReplacement -------------------------------------------------------------- */

struct replace_ctx {
    const pc_paint_stroke *stroke;
    pc_paint_replace_settings rs;
    pc_layer_id id;
    pc_rect damage;
};

static pc_status replace_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct replace_ctx *c = vctx;
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    bool lock = pc_document_effective_locks(doc, c->id).transparency;
    yyjson_val *empty = NULL;
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, c->id, empty, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect dmg = PC_RECT_EMPTY;
    rc = pc_paint_apply_color_replacement(surf, c->stroke, &c->rs, sel, lock, &dmg, err);
    if (rc != PC_OK) {
        return rc;
    }
    c->damage = dmg;
    return PC_OK;
}

/* Parse an enum param into *out (serde name); missing keeps the default. */
static pc_status parse_replace_mode(yyjson_val *p, const char *k, bool (*parse)(const char *, size_t, void *),
                                    void *out, const char *cmd, pc_error *err)
{
    yyjson_val *v = pc_json_get(p, k);
    if (v == NULL) {
        return PC_OK;
    }
    size_t len = 0;
    const char *s = pc_json_as_str(v, &len);
    if (s == NULL || !parse(s, len, out)) {
        return pc_engine_err_bad_params(err, cmd, "`%s`: invalid value", k);
    }
    return PC_OK;
}

static bool parse_rmode(const char *s, size_t len, void *out)
{
    return pc_paint_replace_mode_parse(s, len, out);
}
static bool parse_samp(const char *s, size_t len, void *out)
{
    return pc_paint_sampling_parse(s, len, out);
}
static bool parse_lim(const char *s, size_t len, void *out)
{
    return pc_paint_limits_parse(s, len, out);
}

static pc_status run_color_replacement(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                       pc_error *err)
{
    const char *cmd = "paint.colorReplacement";
    yyjson_val *t = pc_json_get(p, "target");
    if (t != NULL) {
        size_t len = 0;
        const char *ts = pc_json_as_str(t, &len);
        if (!(ts != NULL && len == 6 && strncmp(ts, "pixels", 6) == 0)) {
            return pc_engine_err_bad_params(
                err, cmd, "Color Replacement paints layer pixels only (`target` is not \"pixels\")");
        }
    }
    pc_paint_point *pts = NULL;
    size_t npts = 0;
    pc_status rc = pc_engine_parse_points(p, cmd, &pts, &npts, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_paint_brush brush = {0};
    rc = pc_engine_resolve_brush(s, p, cmd, &brush, err);
    if (rc != PC_OK) {
        free(pts);
        return rc;
    }
    pc_paint_replace_settings rs;
    pc_paint_replace_settings_default(&rs);
    rc = parse_replace_mode(p, "mode", parse_rmode, &rs.mode, cmd, err);
    if (rc == PC_OK) {
        rc = parse_replace_mode(p, "sampling", parse_samp, &rs.sampling, cmd, err);
    }
    if (rc == PC_OK) {
        rc = parse_replace_mode(p, "limits", parse_lim, &rs.limits, cmd, err);
    }
    if (rc != PC_OK) {
        pc_paint_brush_fini(&brush);
        free(pts);
        return rc;
    }
    rs.tolerance = pct(p, "tolerance", 30.0f);
    rs.anti_alias = flag(p, "antiAlias", true);
    memcpy(rs.color, brush.color, sizeof rs.color);
    memcpy(rs.background, s->tools.background, sizeof rs.background);

    pc_layer_id id = 0;
    if (!layer_id_of(s, p, &id)) {
        pc_paint_brush_fini(&brush);
        free(pts);
        return pc_engine_err_other(err, "no active layer");
    }

    pc_paint_stroke stroke = {0};
    stroke.brush = brush;
    stroke.points = pts;
    stroke.count = npts;

    struct replace_ctx ctx = {0};
    ctx.stroke = &stroke;
    ctx.rs = rs;
    ctx.id = id;
    ctx.damage = PC_RECT_EMPTY;
    rc = pc_engine_session_edit(s, "Color Replacement", replace_edit, &ctx, err);
    pc_paint_stroke_fini(&stroke);
    if (rc != PC_OK) {
        return rc;
    }
    return damage_result(s, ctx.damage, out, err);
}

/* ========================================================================================== */
/* brush management                                                                           */
/* ========================================================================================== */

static pc_status en_always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

static pc_status run_brush_get(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return brush_json_result(&s->tools.brush, out, err);
}

static const char *tip_kind_name(const pc_paint_tip *t)
{
    return t->kind == PC_PAINT_TIP_ROUND ? "round" : "sampled";
}

static pc_status run_presets_list(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    bool full = flag(p, "full", false);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    for (size_t i = 0; i < s->tools.npresets; i++) {
        const pc_paint_preset *pr = &s->tools.presets[i];
        yyjson_mut_val *o = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, o, "name", pc_str_c(&pr->name));
        yyjson_mut_obj_add_bool(d, o, "builtin", pr->builtin);
        yyjson_mut_obj_add_real(d, o, "size", pr->brush.size);
        yyjson_mut_obj_add_real(d, o, "hardness", pr->brush.hardness);
        yyjson_mut_obj_add_str(d, o, "tip", tip_kind_name(&pr->brush.tip));
        if (full) {
            pc_paint_brush b = {0};
            pc_paint_brush_copy(&b, &pr->brush);
            (void)load_brush_tips(s, &b, "brush.presets.list", NULL);
            yyjson_mut_val *bv = pc_engine_brush_json(d, &b);
            pc_paint_brush_fini(&b);
            if (bv != NULL) {
                yyjson_mut_obj_add_val(d, o, "brush", bv);
            }
        }
        yyjson_mut_arr_add_val(arr, o);
    }
    yyjson_mut_obj_add_val(d, root, "presets", arr);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* The trimmed, non-empty `name`, copied into *out (free()). */
static pc_status name_param(yyjson_val *p, const char *cmd, char **out, pc_error *err)
{
    size_t len = 0;
    yyjson_val *v = pc_json_get(p, "name");
    const char *s = v != NULL ? pc_json_as_str(v, &len) : NULL;
    if (s == NULL) {
        return pc_engine_err_bad_params(err, cmd, "missing `name`");
    }
    size_t a = 0, b = len;
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) {
        a++;
    }
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) {
        b--;
    }
    if (b <= a) {
        return pc_engine_err_bad_params(err, cmd, "missing `name`");
    }
    char *name = pc_malloc_array(b - a + 1, 1);
    if (name == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "name");
    }
    memcpy(name, s + a, b - a);
    name[b - a] = '\0';
    *out = name;
    return PC_OK;
}

static int ascii_casecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb || ca == 0) {
            return (int)ca - (int)cb;
        }
    }
}

/* Replace the preset of that name (ASCII case-insensitive), or append. Takes over *preset. */
static pc_status upsert(pc_engine_session *s, pc_paint_preset *preset, pc_error *err)
{
    for (size_t i = 0; i < s->tools.npresets; i++) {
        if (ascii_casecmp(pc_str_c(&s->tools.presets[i].name), pc_str_c(&preset->name)) == 0) {
            pc_paint_preset_fini(&s->tools.presets[i]);
            s->tools.presets[i] = *preset;
            memset(preset, 0, sizeof *preset);
            pc_engine_brush_presets_changed(s);
            return PC_OK;
        }
    }
    size_t cap = s->tools.npresets;
    pc_paint_preset *grown =
        pc_grow_array(s->tools.presets, &cap, s->tools.npresets + 1, sizeof *s->tools.presets);
    if (grown == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "presets");
    }
    s->tools.presets = grown;
    s->tools.presets[s->tools.npresets++] = *preset;
    memset(preset, 0, sizeof *preset);
    pc_engine_brush_presets_changed(s);
    return PC_OK;
}

static void set_current_preset(pc_engine_session *s, const char *name)
{
    free(s->tools.current_preset);
    s->tools.current_preset = NULL;
    if (name != NULL) {
        size_t n = strlen(name) + 1;
        s->tools.current_preset = pc_malloc_array(n, 1);
        if (s->tools.current_preset != NULL) {
            memcpy(s->tools.current_preset, name, n);
        }
    }
}

static pc_status run_presets_save(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.presets.save";
    char *name = NULL;
    pc_status rc = name_param(p, cmd, &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_paint_brush brush = {0};
    yyjson_val *patch = pc_json_get(p, "brush");
    if (patch != NULL && yyjson_is_obj(patch)) {
        rc = pc_engine_merge_brush(&s->tools.brush, patch, cmd, &brush, err);
        if (rc != PC_OK) {
            free(name);
            return rc;
        }
    } else {
        pc_paint_brush_copy(&brush, &s->tools.brush);
    }
    pc_paint_preset_captures captures;
    captures.size = flag(p, "captureSize", true);
    captures.tool_settings = flag(p, "toolSettings", true);
    captures.color = flag(p, "color", false);
    if (captures.color) {
        memcpy(brush.color, s->tools.foreground, sizeof brush.color);
    }
    pc_paint_preset preset;
    pc_paint_preset_init(&preset);
    rc = pc_str_set_c(&preset.name, name, err);
    if (rc != PC_OK) {
        pc_paint_preset_fini(&preset);
        pc_paint_brush_fini(&brush);
        free(name);
        return rc;
    }
    pc_paint_brush_fini(&preset.brush);
    preset.brush = brush;
    preset.builtin = false;
    preset.captures = captures;
    rc = upsert(s, &preset, err);
    pc_paint_preset_fini(&preset);
    if (rc != PC_OK) {
        free(name);
        return rc;
    }
    set_current_preset(s, name);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(name);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", name);
    yyjson_mut_obj_add_uint(d, root, "count", s->tools.npresets);
    yyjson_mut_doc_set_root(d, root);
    free(name);
    return PC_OK;
}

static pc_status run_presets_delete(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.presets.delete";
    char *name = NULL;
    pc_status rc = name_param(p, cmd, &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t before = s->tools.npresets;
    size_t w = 0;
    for (size_t i = 0; i < s->tools.npresets; i++) {
        if (ascii_casecmp(pc_str_c(&s->tools.presets[i].name), name) == 0) {
            pc_paint_preset_fini(&s->tools.presets[i]);
        } else {
            if (w != i) {
                s->tools.presets[w] = s->tools.presets[i];
            }
            w++;
        }
    }
    s->tools.npresets = w;
    if (s->tools.npresets == before) {
        pc_status rc2 = pc_engine_err_bad_params(err, cmd, "no brush preset named `%s`", name);
        free(name);
        return rc2;
    }
    if (s->tools.current_preset != NULL && ascii_casecmp(s->tools.current_preset, name) == 0) {
        free(s->tools.current_preset);
        s->tools.current_preset = NULL;
    }
    pc_engine_brush_presets_changed(s);
    free(name);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "count", s->tools.npresets);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_presets_update(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "brush.presets.update";
    char *name = NULL;
    yyjson_val *nv = pc_json_get(p, "name");
    if (nv != NULL && !yyjson_is_null(nv)) {
        pc_status rc = name_param(p, cmd, &name, err);
        if (rc != PC_OK) {
            return rc;
        }
    } else {
        if (s->tools.current_preset == NULL) {
            return pc_engine_err_bad_params(err, cmd, "the current brush was not picked from a preset");
        }
        size_t n = strlen(s->tools.current_preset) + 1;
        name = pc_malloc_array(n, 1);
        if (name == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "name");
        }
        memcpy(name, s->tools.current_preset, n);
    }
    pc_paint_brush brush = {0};
    yyjson_val *patch = pc_json_get(p, "brush");
    if (patch != NULL && yyjson_is_obj(patch)) {
        pc_status rc = pc_engine_merge_brush(&s->tools.brush, patch, cmd, &brush, err);
        if (rc != PC_OK) {
            free(name);
            return rc;
        }
    } else {
        pc_paint_brush_copy(&brush, &s->tools.brush);
    }
    pc_paint_preset *pr = NULL;
    for (size_t i = 0; i < s->tools.npresets; i++) {
        if (ascii_casecmp(pc_str_c(&s->tools.presets[i].name), name) == 0) {
            pr = &s->tools.presets[i];
            break;
        }
    }
    if (pr == NULL) {
        pc_paint_brush_fini(&brush);
        pc_status rc = pc_engine_err_bad_params(err, cmd, "no brush preset named `%s`", name);
        free(name);
        return rc;
    }
    pc_paint_brush_fini(&pr->brush);
    pr->brush = brush;
    pr->builtin = false;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(name);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", pc_str_c(&pr->name));
    yyjson_mut_obj_add_strcpy(d, root, "group", pc_str_c(&pr->group));
    yyjson_mut_doc_set_root(d, root);
    pc_engine_brush_presets_changed(s);
    free(name);
    return PC_OK;
}

static pc_status run_presets_set_current(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                         pc_error *err)
{
    const char *cmd = "brush.presets.setCurrent";
    char *name = NULL;
    pc_status rc = name_param(p, cmd, &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    bool found = false;
    for (size_t i = 0; i < s->tools.npresets; i++) {
        if (ascii_casecmp(pc_str_c(&s->tools.presets[i].name), name) == 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        rc = pc_engine_err_bad_params(err, cmd, "no brush preset named `%s`", name);
        free(name);
        return rc;
    }
    set_current_preset(s, name);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        free(name);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", name);
    yyjson_mut_doc_set_root(d, root);
    free(name);
    return PC_OK;
}

/* ---- brush.defineFromSelection ----------------------------------------------------------- */

#define MAX_TIP 2500u

static pc_status en_selection_and_pixels(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!doc->has_selection) {
        return pc_fail(reason, PC_ERR_INVALID, "no selection");
    }
    if (!d->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL || l->content.kind != PC_LAYER_RASTER) {
        return pc_fail(reason, PC_ERR_INVALID, "active layer is not a pixel layer");
    }
    return PC_OK;
}

static pc_status run_define_from_selection(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                           pc_error *err)
{
    const char *cmd = "brush.defineFromSelection";
    char *name = NULL;
    pc_status rc = name_param(p, cmd, &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        free(name);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!doc->has_selection) {
        free(name);
        return pc_engine_err_bad_params(err, cmd, "no selection");
    }
    if (!d->has_active_layer) {
        free(name);
        return pc_engine_err_other(err, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
    if (surf == NULL) {
        free(name);
        return pc_engine_err_bad_params(err, cmd, "not a pixel layer");
    }
    pc_rect area = pc_rect_intersect(pc_surface_content_bounds(&doc->selection), pc_document_bounds(doc));
    if (pc_rect_is_empty(area)) {
        free(name);
        return pc_engine_err_bad_params(err, cmd, "the selection is empty");
    }
    size_t w = pc_rect_width(area), h = pc_rect_height(area);
    float *v = pc_calloc_array(w * h, sizeof *v);
    size_t sc = pc_surface_channels(&doc->selection);
    float *sv = NULL;
    size_t svlen = 0;
    if (v == NULL || pc_surface_read_region(&doc->selection, area, &sv, &svlen, err) != PC_OK) {
        free(v);
        free(name);
        return pc_fail(err, PC_ERR_NOMEM, "brush tip");
    }
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            float c[4];
            pc_surface_rgba(surf, area.x0 + (int32_t)x, area.y0 + (int32_t)y, c);
            float rgb[3] = {c[0], c[1], c[2]};
            float lum = pc_blend_lum(rgb);
            float cov = (1.0f - lum) * c[3] * sv[(y * w + x) * sc];
            v[y * w + x] = clampf(cov, 0.0f, 1.0f);
        }
    }
    free(sv);
    size_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            if (v[y * w + x] > 1e-4f) {
                if (x < x0) {
                    x0 = x;
                }
                if (y < y0) {
                    y0 = y;
                }
                if (x + 1 > x1) {
                    x1 = x + 1;
                }
                if (y + 1 > y1) {
                    y1 = y + 1;
                }
            }
        }
    }
    if (x1 <= x0 || y1 <= y0) {
        free(v);
        free(name);
        return pc_engine_err_bad_params(err, cmd,
                                        "the selected pixels contain no paint (all white or transparent)");
    }
    uint32_t tw = (uint32_t)(x1 - x0), th = (uint32_t)(y1 - y0);
    if ((tw > th ? tw : th) > MAX_TIP) {
        free(v);
        pc_status rc2 = pc_engine_err_bad_params(err, cmd,
                                                 "selection too large for a brush tip (%ux%u; max %u px)",
                                                 tw, th, MAX_TIP);
        free(name);
        return rc2;
    }
    float *data = pc_malloc_array((size_t)tw * th, sizeof *data);
    if (data == NULL) {
        free(v);
        free(name);
        return pc_fail(err, PC_ERR_NOMEM, "brush tip");
    }
    for (size_t y = y0; y < y1; y++) {
        memcpy(&data[(y - y0) * tw], &v[y * w + x0], tw * sizeof *data);
    }
    free(v);
    pc_paint_gray_tile tile;
    rc = pc_paint_gray_tile_from_f32(tw, th, data, (size_t)tw * th, &tile, err);
    free(data);
    if (rc != PC_OK) {
        free(name);
        return rc;
    }
    pc_paint_brush brush = {0};
    pc_paint_brush_default(&brush);
    brush.tip.kind = PC_PAINT_TIP_SAMPLED;
    brush.tip.tile = tile;
    brush.size = (float)(tw > th ? tw : th);
    brush.spacing = 0.25f;
    brush.pressure_size = false;

    pc_paint_preset preset;
    pc_paint_preset_init(&preset);
    rc = pc_str_set_c(&preset.name, name, err);
    if (rc != PC_OK) {
        pc_paint_preset_fini(&preset);
        pc_paint_brush_fini(&brush);
        free(name);
        return rc;
    }
    pc_paint_brush_fini(&preset.brush);
    pc_paint_brush_copy(&preset.brush, &brush);
    preset.builtin = false;
    rc = upsert(s, &preset, err);
    pc_paint_preset_fini(&preset);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&brush);
        free(name);
        return rc;
    }
    pc_paint_brush_fini(&s->tools.brush);
    s->tools.brush = brush;
    set_current_preset(s, name);
    yyjson_mut_doc *d2 = NULL;
    if (pc_engine_result_doc(out, &d2, err) != PC_OK) {
        free(name);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d2);
    yyjson_mut_obj_add_strcpy(d2, root, "name", name);
    yyjson_mut_obj_add_uint(d2, root, "width", tw);
    yyjson_mut_obj_add_uint(d2, root, "height", th);
    yyjson_mut_doc_set_root(d2, root);
    free(name);
    return PC_OK;
}

/* ========================================================================================== */
/* tools.setBrush and the coalesce journal hook                                               */
/* ========================================================================================== */

pc_status pc_engine_set_brush(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "tools.setBrush";
    pc_paint_brush b = {0};
    char *picked = NULL;
    bool has_picked_color = false;
    float picked_color[4] = {0, 0, 0, 1};
    size_t len = 0;
    yyjson_val *pv = pc_json_get(p, "preset");
    const char *preset = pv != NULL ? pc_json_as_str(pv, &len) : NULL;
    if (preset != NULL) {
        const pc_paint_preset *pr = find_preset(s, preset, len);
        if (pr == NULL) {
            return pc_engine_err_bad_params(err, cmd, "no brush preset named `%.*s`", (int)len, preset);
        }
        preset_picked_over(pr, &s->tools.brush, &b);
        if (pr->captures.color) {
            has_picked_color = true;
            memcpy(picked_color, pr->brush.color, sizeof picked_color);
        }
        picked = pc_malloc_array(len + 1, 1);
        if (picked == NULL) {
            pc_paint_brush_fini(&b);
            return pc_fail(err, PC_ERR_NOMEM, "preset");
        }
        memcpy(picked, preset, len);
        picked[len] = '\0';
    } else {
        pc_paint_brush_copy(&b, &s->tools.brush);
    }
    bool reset = flag(p, "reset", false);
    if (reset) {
        pc_paint_brush_fini(&b);
        pc_paint_brush_default(&b);
    }
    /* Build a patch value = params minus preset/reset/coalesce/brush; the inner "brush" merges
     * first (and is validated before the second merge, #977). */
    yyjson_val *inner = pc_json_get(p, "brush");
    if (inner != NULL && yyjson_is_obj(inner)) {
        pc_paint_brush tmp = {0};
        pc_status rc = pc_engine_merge_brush(&b, inner, cmd, &tmp, err);
        if (rc == PC_OK) {
            rc = pc_engine_validate_brush(&tmp, cmd, err);
        }
        if (rc != PC_OK) {
            pc_paint_brush_fini(&b);
            if (rc == PC_OK) {
                pc_paint_brush_fini(&tmp);
            }
            free(picked);
            return rc;
        }
        pc_paint_brush_fini(&b);
        b = tmp;
    }
    /* The outer patch: the params object with the control keys and `brush` removed. */
    yyjson_mut_doc *pd = yyjson_mut_doc_new(NULL);
    if (pd == NULL) {
        pc_paint_brush_fini(&b);
        free(picked);
        return pc_fail(err, PC_ERR_NOMEM, "brush");
    }
    yyjson_mut_val *patch = yyjson_mut_obj(pd);
    if (p != NULL && yyjson_is_obj(p)) {
        size_t idx, max;
        yyjson_val *k, *vv;
        yyjson_obj_foreach (p, idx, max, k, vv) {
            const char *ks = yyjson_get_str(k);
            if (ks == NULL || strcmp(ks, "preset") == 0 || strcmp(ks, "reset") == 0 ||
                strcmp(ks, "coalesce") == 0 || strcmp(ks, "brush") == 0) {
                continue;
            }
            yyjson_mut_val *key = yyjson_mut_strcpy(pd, ks);
            yyjson_mut_val *cv = pc_json_normalized_copy_val(pd, vv);
            if (key == NULL || cv == NULL || !yyjson_mut_obj_add(patch, key, cv)) {
                yyjson_mut_doc_free(pd);
                pc_paint_brush_fini(&b);
                free(picked);
                return pc_fail(err, PC_ERR_NOMEM, "brush");
            }
        }
    }
    yyjson_mut_doc_set_root(pd, patch);
    /* Merge the outer patch through the same code path (serialise patch, read back as a val). */
    pc_buf pbuf = PC_BUF_INIT;
    pc_status rc = pc_json_write(patch, 0, &pbuf, err);
    yyjson_mut_doc_free(pd);
    if (rc != PC_OK) {
        pc_buf_fini(&pbuf);
        pc_paint_brush_fini(&b);
        free(picked);
        return PC_ERR_NOMEM;
    }
    yyjson_doc *pdoc = NULL;
    rc = pc_json_parse((const char *)pbuf.data, pbuf.len, &pdoc, err);
    pc_buf_fini(&pbuf);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&b);
        free(picked);
        return PC_ERR_NOMEM;
    }
    pc_paint_brush merged = {0};
    rc = pc_engine_merge_brush(&b, yyjson_doc_get_root(pdoc), cmd, &merged, err);
    yyjson_doc_free(pdoc);
    pc_paint_brush_fini(&b);
    if (rc != PC_OK) {
        free(picked);
        return rc;
    }
    rc = pc_engine_validate_brush(&merged, cmd, err);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&merged);
        free(picked);
        return rc;
    }
    if (reset) {
        free(s->tools.current_preset);
        s->tools.current_preset = NULL;
    } else if (picked != NULL) {
        set_current_preset(s, picked);
        if (has_picked_color) {
            memcpy(s->tools.foreground, picked_color, sizeof s->tools.foreground);
        }
    }
    rc = load_brush_tips(s, &merged, cmd, err);
    if (rc != PC_OK) {
        pc_paint_brush_fini(&merged);
        free(picked);
        return rc;
    }
    /* Swap in the new brush, keeping the previous one for a coalesced gesture. */
    pc_paint_brush before = {0};
    before = s->tools.brush;
    s->tools.brush = merged;

    /* Coalescing: one slider drag (same `coalesce` key, no preset/reset) journals as one call. */
    size_t ck_len = 0;
    yyjson_val *cv = pc_json_get(p, "coalesce");
    const char *key = (cv != NULL && pc_json_get(p, "preset") == NULL && pc_json_get(p, "reset") == NULL)
                          ? pc_json_as_str(cv, &ck_len)
                          : NULL;
    bool continuing = false;
    if (key != NULL && s->tools.has_brush_gesture && s->tools.brush_gesture_key != NULL &&
        strlen(s->tools.brush_gesture_key) == ck_len &&
        strncmp(s->tools.brush_gesture_key, key, ck_len) == 0 && s->njournal > 0 &&
        strcmp(s->journal[s->njournal - 1].id, cmd) == 0) {
        yyjson_mut_val *lp = pc_json_value_root(&s->journal[s->njournal - 1].params);
        if (lp != NULL) {
            yyjson_mut_val *lc = yyjson_mut_obj_get(lp, "coalesce");
            const char *lcs = lc != NULL ? yyjson_mut_get_str(lc) : NULL;
            if (lcs != NULL && strlen(lcs) == ck_len && strncmp(lcs, key, ck_len) == 0) {
                continuing = true;
            }
        }
    }
    if (key != NULL && continuing) {
        pc_paint_brush_fini(&before);
    } else if (key != NULL) {
        pc_paint_brush_fini(&s->tools.brush_gesture_start);
        s->tools.brush_gesture_start = before; /* move */
        free(s->tools.brush_gesture_key);
        s->tools.brush_gesture_key = pc_malloc_array(ck_len + 1, 1);
        if (s->tools.brush_gesture_key != NULL) {
            memcpy(s->tools.brush_gesture_key, key, ck_len);
            s->tools.brush_gesture_key[ck_len] = '\0';
            s->tools.has_brush_gesture = true;
        } else {
            s->tools.has_brush_gesture = false;
        }
    } else {
        pc_paint_brush_fini(&before);
        pc_paint_brush_fini(&s->tools.brush_gesture_start);
        pc_paint_brush_default(&s->tools.brush_gesture_start);
        free(s->tools.brush_gesture_key);
        s->tools.brush_gesture_key = NULL;
        s->tools.has_brush_gesture = false;
    }
    free(picked);
    return brush_json_result(&s->tools.brush, out, err);
}

static pc_status run_set_brush(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return pc_engine_set_brush(s, p, out, err);
}

/* The minimal tools.setBrush "brush" patch turning `old` into `new` (brush_patch), built as a diff
 * of their JSON. Returns a value owned by d, or {} on OOM. */
static yyjson_mut_val *diff_vals(yyjson_mut_doc *d, yyjson_mut_val *a, yyjson_mut_val *b)
{
    if (yyjson_mut_is_obj(a) && yyjson_mut_is_obj(b)) {
        yyjson_mut_val *out = yyjson_mut_obj(d);
        if (out == NULL) {
            return NULL;
        }
        yyjson_mut_obj_iter it;
        yyjson_mut_obj_iter_init(b, &it);
        yyjson_mut_val *bk;
        while ((bk = yyjson_mut_obj_iter_next(&it)) != NULL) {
            const char *k = yyjson_mut_get_str(bk);
            yyjson_mut_val *bv = yyjson_mut_obj_iter_get_val(bk);
            yyjson_mut_val *av = k != NULL ? yyjson_mut_obj_get(a, k) : NULL;
            yyjson_mut_val *dv;
            if (av != NULL) {
                dv = diff_vals(d, av, bv);
                if (dv == NULL) {
                    continue; /* equal subtree */
                }
            } else {
                dv = yyjson_mut_val_mut_copy(d, bv);
                if (dv == NULL) {
                    return NULL;
                }
            }
            yyjson_mut_val *key = yyjson_mut_strcpy(d, k != NULL ? k : "");
            if (key == NULL || !yyjson_mut_obj_add(out, key, dv)) {
                return NULL;
            }
        }
        return yyjson_mut_obj_size(out) > 0 ? out : NULL;
    }
    return pc_json_mut_equal(a, b) ? NULL : yyjson_mut_val_mut_copy(d, b);
}

/* Journal hook: fold consecutive tools.setBrush calls with the same `coalesce` key into one
 * entry (coalesce_journal). Returns true when folded. */
bool pc_engine_hook_coalesce_journal(pc_engine_session *s, const char *id, yyjson_val *params)
{
    if (strcmp(id, "tools.setBrush") != 0) {
        return false;
    }
    size_t klen = 0;
    yyjson_val *cv = pc_json_get(params, "coalesce");
    const char *key = cv != NULL ? pc_json_as_str(cv, &klen) : NULL;
    if (key == NULL) {
        return false;
    }
    if (!s->tools.has_brush_gesture || s->tools.brush_gesture_key == NULL ||
        strlen(s->tools.brush_gesture_key) != klen || strncmp(s->tools.brush_gesture_key, key, klen) != 0) {
        return false;
    }
    if (s->njournal == 0 || strcmp(s->journal[s->njournal - 1].id, id) != 0) {
        return false;
    }
    yyjson_mut_val *lp = pc_json_value_root(&s->journal[s->njournal - 1].params);
    const char *lcs = NULL;
    if (lp != NULL) {
        yyjson_mut_val *lc = yyjson_mut_obj_get(lp, "coalesce");
        lcs = lc != NULL ? yyjson_mut_get_str(lc) : NULL;
    }
    if (lcs == NULL || strlen(lcs) != klen || strncmp(lcs, key, klen) != 0) {
        return false;
    }
    /* patch = diff(gesture_start, current brush), stored as {"brush": patch, "coalesce": key}. */
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    if (d == NULL) {
        return false;
    }
    yyjson_mut_val *av = pc_paint_json_write(PC_PAINT_JSON_BRUSH, d, &s->tools.brush_gesture_start, 0);
    yyjson_mut_val *bv = pc_paint_json_write(PC_PAINT_JSON_BRUSH, d, &s->tools.brush, 0);
    if (av == NULL || bv == NULL) {
        yyjson_mut_doc_free(d);
        return false;
    }
    yyjson_mut_val *patch = diff_vals(d, av, bv);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    if (root == NULL) {
        yyjson_mut_doc_free(d);
        return false;
    }
    yyjson_mut_val *pcopy = patch != NULL ? patch : yyjson_mut_obj(d);
    yyjson_mut_obj_add_val(d, root, "brush", pcopy);
    yyjson_mut_obj_add_strncpy(d, root, "coalesce", key, klen);
    pc_json_value nv = PC_JSON_VALUE_INIT;
    pc_status rc = pc_json_value_from_mut(root, &nv, NULL);
    yyjson_mut_doc_free(d);
    if (rc != PC_OK) {
        return false;
    }
    pc_json_value_fini(&s->journal[s->njournal - 1].params);
    s->journal[s->njournal - 1].params = nv;
    return true;
}

/* ========================================================================================== */
/* registry                                                                                    */
/* ========================================================================================== */

static const pc_engine_command SPECS[] = {
    {"paint.stroke", "Brush Tool", NULL, 0, NULL,
     "{\"points\":[[x,y,pressure?,tiltX?,tiltY?,rotation?,timeMs?,wheel?],…],\"brush\":{…}?,"
     "\"preset\":name?,\"size\":0.5..5000 px?,\"opacity\":0..1?,\"color\":\"#rrggbb\"?=foreground,"
     "\"mode\":\"normal|multiply|…|clear\"=\"normal\",\"erase\":bool?,\"zoom\":view zoom=1,"
     "\"seed\":u64?,\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}=Channels panel target}",
     pc_engine_has_paintable, run_stroke, true},
    {"paint.pencil", "Pencil", NULL, 0, NULL,
     "{\"points\":[[x,y,pressure?,tiltX?,tiltY?,rotation?,timeMs?,wheel?],…],\"brush\":{…}?,"
     "\"preset\":name?,\"size\":0.5..5000 px?,\"opacity\":0..1?,\"color\":\"#rrggbb\"?=foreground,"
     "\"mode\":\"normal|multiply|screen|…|clear (erases with this brush)\"=\"normal\",\"erase\":bool?,"
     "\"block\":bool=false (a hard 16×16 screen-px square, 16/zoom doc px; size, opacity, flow, "
     "hardness and dynamics ignored: the Eraser's Block mode),\"zoom\":view zoom=1,\"autoErase\":"
     "bool=false,\"seed\":u64?,\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}=Channels "
     "panel target}",
     pc_engine_has_paintable, run_pencil, true},
    {"paint.mixerBrush", "Mixer Brush", NULL, 0, NULL,
     "{\"points\":[…],\"brush\":{…}?,\"preset\":name?,\"size\":0.5..5000 px?,\"wet\":0..100=brush.mixer."
     "wet,\"load\":0..100=brush.mixer.load,\"mix\":0..100=brush.mixer.mix,\"flow\":0..100=brush.mixer."
     "flow,\"color\":\"#rrggbb\"?=foreground,\"sampleAllLayers\":bool=brush.mixer.sampleAllLayers,"
     "\"cleanAfterStroke\":bool=true,\"loadAfterStroke\":bool=true,\"seed\":u64?}",
     pc_engine_has_paintable, run_mixer, true},
    {"paint.colorReplacement", "Color Replacement", NULL, 0, NULL,
     "{\"points\":[…],\"brush\":{…}?,\"size\":px?,\"mode\":\"hue|saturation|color|luminosity\"="
     "\"color\",\"sampling\":\"continuous|once|backgroundSwatch\"=\"continuous\",\"limits\":"
     "\"contiguous|discontiguous|findEdges\"=\"contiguous\",\"tolerance\":0..100=30,\"antiAlias\":"
     "bool=true,\"color\":\"#rrggbb\"?=foreground,\"target\":\"pixels\"=\"pixels\",\"seed\":u64?}",
     pc_engine_has_paintable, run_color_replacement, true},
    {"brush.presets.list", "List Brush Presets", NULL, 0, NULL, "{\"full\":bool=false}", en_always,
     run_presets_list, false},
    {"brush.presets.save", "Save Brush Preset", NULL, 0, NULL,
     "{\"name\":string,\"brush\":{…BrushSettings}?=current brush,\"captureSize\":bool=true (false: "
     "picking it keeps the tool's size),\"toolSettings\":bool=true (false: picking it keeps the tool's "
     "mode, opacity, flow and pressure buttons),\"color\":bool=false (picking it sets the foreground "
     "to the current foreground)}",
     en_always, run_presets_save, true},
    {"brush.presets.update", "Update Brush Preset", NULL, 0, NULL,
     "{\"name\":string?=the preset the current brush was picked from,\"brush\":{…BrushSettings}?="
     "current brush}",
     en_always, run_presets_update, true},
    {"brush.presets.delete", "Delete Brush Preset", NULL, 0, NULL, "{\"name\":string}", en_always,
     run_presets_delete, true},
    {"brush.presets.setCurrent", "Set Current Brush Preset", NULL, 0, NULL, "{\"name\":string}", en_always,
     run_presets_set_current, true},
    {"brush.defineFromSelection", "Define Brush Preset…", NULL, 0, NULL, "{\"name\":string}",
     en_selection_and_pixels, run_define_from_selection, true},
    {"brush.get", "Get Brush", NULL, 0, NULL, "{}", en_always, run_brush_get, false},
    {"tools.setBrush", "Set Brush", NULL, 0, NULL,
     "{\"preset\":name?,\"reset\":bool?,…BrushSettings fields (camelCase, deep-merged)}", en_always,
     run_set_brush, true},
};

const pc_engine_command *pc_engine_brush_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
