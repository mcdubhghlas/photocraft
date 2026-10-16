/* Edit > Fade and Edit > Purge (port of crates/engine/src/edit_menu_cmds.rs).
 *
 * Fade (edit.fade) blends the just-applied filter / adjustment / paint / fill back towards the
 * state before the step at an opacity and blend mode. Its source is recorded after every
 * successful command by pc_engine_hook_edit_menu_after_command (relocated here from hooks.c, EF):
 * the active layer, whether the step painted the layer's pixels or its mask (decided by a COW-tile
 * pointer-identity fingerprint against the previous history state), and the document revision it
 * left (any later change disables Fade).
 *
 * Purge frees the memory held by the undo of the active document, the clipboard, every document's
 * history, or all of them (plus the compositor's effect / mask / smart-filter caches). Each reports
 * what it freed. Purge never changes a document's pixels, so it is not an undoable edit.
 *
 * Never crashes on any session state or hostile params (REWRITE_PLAN.md rule 9). */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "color/blend.h"
#include "compose/compose.h"
#include "compose/masks.h"
#include "doc/doc.h"
#include "doc/vector.h"
#include "ops/ops.h"
#include "raster/pixel.h"
#include "raster/surface.h"
#include "text/unicode.h"

#include "brush_cmds.h" /* pc_engine_blend_from_str */
#include "engine_internal.h"
#include "layer_cmds.h" /* pc_engine_layer_param */
#include "smart_cmds.h"
#include "type_cmds.h" /* pc_engine_replace_text, pc_engine_type_refresh, check_text_unlocked */

/* ================================ Edit > Fade ============================================== */

/* Commands whose result Edit > Fade can blend back against the previous state (fadeable). */
static bool fadeable(const char *id)
{
    if (id == NULL) {
        return false;
    }
    if (strncmp(id, "filter.", 7) == 0 || strncmp(id, "image.adjustments.", 18) == 0 ||
        strncmp(id, "paint.", 6) == 0) {
        return true;
    }
    return strcmp(id, "edit.fill") == 0 || strcmp(id, "edit.stroke") == 0 ||
           strcmp(id, "edit.contentAwareFill") == 0 || strcmp(id, "image.autoTone") == 0 ||
           strcmp(id, "image.autoContrast") == 0 || strcmp(id, "image.autoColor") == 0;
}

/* Tile identity of a surface: an order-independent fold of its tiles' COW pointer identity and
 * coordinates (mirrors the Rust crate_fingerprint, which folds Arc::as_ptr over the tiles). Two
 * surfaces that share every tile (unchanged by an edit) fold to the same value; a changed surface
 * (an un-shared tile) almost certainly differs. NULL -> 0. */
static uint64_t surf_fingerprint(const pc_surface *s)
{
    if (s == NULL) {
        return 0;
    }
    uint64_t h = (uint64_t)pc_surface_tile_count(s);
    size_t cursor = 0;
    pc_tile coord;
    const pc_raster_tile *tile = NULL;
    while (pc_surface_next_tile(s, &cursor, &coord, &tile)) {
        uint64_t k = (uint64_t)(uintptr_t)tile;
        k ^= ((uint64_t)(uint32_t)coord.tx << 32) | (uint64_t)(uint32_t)coord.ty;
        k *= 0xff51afd7ed558ccdULL; /* fmix64 finaliser */
        k ^= k >> 33;
        h ^= k;
    }
    return h;
}

/* Whether two optional surfaces have the same tile identity (both missing = same). */
static bool surf_eq(const pc_surface *a, const pc_surface *b)
{
    if (a == NULL && b == NULL) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    return surf_fingerprint(a) == surf_fingerprint(b);
}

/* The pixel surface (mask false) or the layer mask surface (mask true) of layer `id`, or NULL. */
static const pc_surface *surface_of(const pc_document *doc, pc_layer_id id, bool mask)
{
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return NULL;
    }
    if (mask) {
        return l->has_mask ? &l->mask.surface : NULL;
    }
    return pc_layer_surface(l);
}

/* Called after every successful command (the relocated edit_menu after-command hook): remember a
 * fadeable result. Mirrors edit_menu_cmds::after_command. */
void pc_engine_hook_edit_menu_after_command(pc_engine_session *s, const char *id)
{
    if (!fadeable(id)) {
        return;
    }
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL || !st->has_active_layer) {
        return;
    }
    pc_layer_id layer = st->active_layer;
    const pc_document *doc = pc_doc_arc_get(st->doc);
    const pc_layer *l = pc_document_layer(doc, layer);
    if (l == NULL) {
        return;
    }
    /* The mask changed and the pixels didn't: the command painted the mask. */
    bool mask = false;
    if (l->has_mask) {
        size_t past = pc_history_past_len(st->history);
        if (past > 0) {
            pc_doc_arc *prevarc = pc_history_state(st->history, past - 1);
            if (prevarc != NULL) {
                const pc_document *prev = pc_doc_arc_get(prevarc);
                const pc_layer *was = pc_document_layer(prev, layer);
                const pc_surface *was_px = (was != NULL) ? pc_layer_surface(was) : NULL;
                const pc_surface *was_mask = (was != NULL && was->has_mask) ? &was->mask.surface : NULL;
                mask = surf_eq(pc_layer_surface(l), was_px) && !surf_eq(&l->mask.surface, was_mask);
            }
        }
    }
    const pc_str *ul = pc_history_undo_label(st->history);
    const char *label = (ul != NULL && ul->ptr != NULL && ul->len > 0) ? ul->ptr : id;
    char *copy = pc_strndup(label, strlen(label));
    if (copy == NULL) {
        return; /* leave the previous fade source untouched on OOM */
    }
    free(s->fade_label);
    s->fade_label = copy;
    s->has_fade = true;
    s->fade_doc = doc->id;
    s->fade_revision = st->revision;
    s->fade_layer = layer;
    s->fade_mask = mask;
}

/* Blend `after` back towards `before` over `area`: before + opacity * (B(before, after) - before)
 * with blend mode `mode` (premultiplied, so alpha changes fade too). Port of fade_surface. */
static void fade_surface(const pc_surface *before, pc_surface *after, pc_rect area, pc_blend_mode mode,
                         float opacity)
{
    pc_pixel_format fmt = pc_surface_format(after);
    size_t n = pc_pixel_format_channels(fmt);
    if (area.x1 <= area.x0 || area.y1 <= area.y0 || n == 0) {
        return;
    }
    size_t w = (size_t)(area.x1 - area.x0);
    size_t h = (size_t)(area.y1 - area.y0);
    size_t len = w * h * n;
    float *a = malloc(len * sizeof *a);
    float *b = malloc(len * sizeof *b);
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return;
    }
    if (pc_surface_read_region_into(after, area, a, len, NULL) != PC_OK ||
        pc_surface_read_region_into(before, area, b, len, NULL) != PC_OK) {
        free(a);
        free(b);
        return;
    }
    float k = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
    bool alpha = fmt.alpha;
    for (size_t px = 0; px < w * h; px++) {
        float *pa = a + px * n;
        const float *pb = b + px * n;
        if (mode == PC_BLEND_NORMAL || n == 1) {
            if (!alpha) {
                for (size_t c = 0; c < n; c++) {
                    pa[c] = pb[c] + (pa[c] - pb[c]) * k;
                }
            } else {
                float ab = pb[n - 1], aa = pa[n - 1];
                float ao = ab + (aa - ab) * k;
                for (size_t c = 0; c + 1 < n; c++) {
                    float pm = pb[c] * ab + (pa[c] * aa - pb[c] * ab) * k;
                    pa[c] = ao > 1e-9f ? pm / ao : 0.0f;
                }
                pa[n - 1] = ao;
            }
        } else {
            float rb[4], ra[4], blended[4], o[4], enc[PC_RASTER_MAX_CHANNELS];
            pc_to_rgba(fmt, pb, n, rb);
            pc_to_rgba(fmt, pa, n, ra);
            pc_blend_composite(mode, rb, ra, 1.0f, blended);
            /* premultiplied lerp of the 4 straight-RGBA channels (alpha included) */
            float ab = rb[3], aa = blended[3];
            float ao = ab + (aa - ab) * k;
            for (size_t c = 0; c < 3; c++) {
                float pm = rb[c] * ab + (blended[c] * aa - rb[c] * ab) * k;
                o[c] = ao > 1e-9f ? pm / ao : 0.0f;
            }
            o[3] = ao;
            size_t m = pc_from_rgba(fmt, o, enc);
            size_t cnt = m < n ? m : n;
            for (size_t c = 0; c < cnt; c++) {
                pa[c] = enc[c];
            }
        }
    }
    (void)pc_surface_write_region(after, area, a, len, NULL);
    free(a);
    free(b);
}

struct fade_ctx {
    pc_layer_id layer;
    bool mask;
    const pc_surface *before;
    pc_rect area;
    pc_blend_mode mode;
    float opacity;
};

static pc_status fade_cb(pc_document *doc, bool *ha, pc_layer_id *active, void *v, pc_error *err)
{
    (void)ha;
    (void)active;
    struct fade_ctx *c = v;
    pc_layer *l = pc_document_layer_mut(doc, c->layer);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, c->layer);
    }
    pc_surface *surf = c->mask ? (l->has_mask ? &l->mask.surface : NULL) : pc_layer_surface_mut(l);
    if (surf == NULL) {
        return pc_engine_err_no_layer(err, c->layer);
    }
    fade_surface(c->before, surf, c->area, c->mode, c->opacity);
    pc_surface_prune(surf);
    return PC_OK;
}

static pc_status can_fade(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!s->has_fade) {
        return pc_fail(reason, PC_ERR_INVALID, "nothing to fade");
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    if (s->fade_doc != doc->id || s->fade_revision != st->revision || !pc_history_can_undo(st->history)) {
        return pc_fail(reason, PC_ERR_INVALID,
                       "Fade is only available right after a filter, adjustment or painting step");
    }
    if (pc_document_layer(doc, s->fade_layer) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "the faded layer no longer exists");
    }
    return PC_OK;
}

static pc_status run_fade(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "edit.fade";
    if (!s->has_fade) {
        return pc_engine_err_bad_params(err, CMD, "nothing to fade");
    }
    double op = 100.0;
    yyjson_val *ov = pc_json_get(p, "opacity");
    if (ov != NULL && !pc_json_as_f64(ov, &op)) {
        return pc_engine_err_bad_params(err, CMD, "`opacity` must be a number 0..100");
    }
    if (!isfinite(op)) {
        op = 100.0;
    }
    float opacity = (float)(op < 0.0 ? 0.0 : (op > 100.0 ? 100.0 : op)) / 100.0f;
    const char *modestr = "normal";
    size_t mlen = 6;
    yyjson_val *mv = pc_json_get(p, "mode");
    if (mv != NULL) {
        size_t l = 0;
        const char *str = pc_json_as_str(mv, &l);
        if (str == NULL) {
            return pc_engine_err_bad_params(err, CMD, "unknown blend mode");
        }
        modestr = str;
        mlen = l;
    }
    pc_blend_mode mode;
    if (!pc_engine_blend_from_str(modestr, mlen, &mode)) {
        return pc_engine_err_bad_params(err, CMD, "unknown blend mode");
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    size_t past = pc_history_past_len(st->history);
    if (past == 0) {
        return pc_engine_err_bad_params(err, CMD, "no previous state");
    }
    pc_doc_arc *prevarc = pc_history_state(st->history, past - 1);
    if (prevarc == NULL) {
        return pc_engine_err_bad_params(err, CMD, "no previous state");
    }
    const pc_document *prev = pc_doc_arc_get(prevarc);
    pc_layer_id layer = s->fade_layer;
    bool mask = s->fade_mask;
    const pc_surface *before_src = surface_of(prev, layer, mask);
    if (before_src == NULL) {
        return pc_engine_err_other(err, "the layer did not exist before this step");
    }
    const pc_surface *now = surface_of(pc_doc_arc_get(st->doc), layer, mask);
    if (now == NULL) {
        return pc_engine_err_no_layer(err, layer);
    }
    if (!pc_pixel_format_eq(pc_surface_format(now), pc_surface_format(before_src))) {
        return pc_engine_err_other(err, "can't fade a step that changed the layer's pixel format");
    }
    pc_rect area = pc_rect_union(pc_surface_content_bounds(before_src), pc_surface_content_bounds(now));
    pc_surface before_clone;
    if (pc_surface_clone(before_src, &before_clone, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    /* Capture the label before the edit: fade_label can be overwritten by the hook, but this edit
     * records no fadeable command of its own (edit.fade is not fadeable). */
    char label[256];
    const char *src_label = s->fade_label != NULL ? s->fade_label : "";
    int ln = snprintf(label, sizeof label, "Fade %s", src_label);
    if (ln < 0) {
        label[0] = '\0';
    }
    struct fade_ctx c = {layer, mask, &before_clone, area, mode, opacity};
    pc_status rc = pc_engine_session_edit(s, label, fade_cb, &c, err);
    pc_surface_fini(&before_clone);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "faded", src_label);
    yyjson_mut_obj_add_real(d, root, "opacity", (double)opacity * 100.0);
    yyjson_mut_obj_add_strcpy(d, root, "mode", pc_blend_label(mode));
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ================================ Edit > Purge ============================================= */

enum purge_what { PURGE_UNDO, PURGE_CLIPBOARD, PURGE_HISTORIES, PURGE_ALL };

/* Unique history bytes of the active document (all=false) or every document (all=true). */
static size_t history_bytes(const pc_engine_session *s, bool all)
{
    size_t total = 0;
    if (all) {
        for (size_t i = 0; i < s->ndocs; i++) {
            size_t b = 0;
            if (pc_history_unique_bytes(s->docs[i].history, pc_doc_arc_get(s->docs[i].doc), &b, NULL) ==
                PC_OK) {
                total += b;
            }
        }
        return total;
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL) {
        size_t b = 0;
        if (pc_history_unique_bytes(d->history, pc_doc_arc_get(d->doc), &b, NULL) == PC_OK) {
            total += b;
        }
    }
    return total;
}

static size_t sat_sub(size_t a, size_t b)
{
    return a > b ? a - b : 0;
}

static pc_status do_purge(pc_engine_session *s, enum purge_what what, pc_json_value *out, pc_error *err)
{
    size_t freed = 0;
    const char *items[8];
    size_t nitems = 0;

    const pc_engine_doc_state *active = pc_engine_session_active(s);
    if ((what == PURGE_UNDO || what == PURGE_ALL) && active != NULL &&
        pc_history_can_undo(active->history)) {
        size_t before = history_bytes(s, false);
        pc_engine_doc_state *am = pc_engine_session_active_mut(s);
        if (am != NULL) {
            (void)pc_history_purge_last(am->history);
        }
        freed += sat_sub(before, history_bytes(s, false));
        items[nitems++] = "undo";
    }

    if ((what == PURGE_CLIPBOARD || what == PURGE_ALL) && s->clipboard != NULL) {
        freed += pc_engine_clipboard_bytes(s->clipboard);
        pc_engine_clipboard_free(s->clipboard);
        s->clipboard = NULL;
        items[nitems++] = "clipboard";
    }

    if (what == PURGE_HISTORIES || what == PURGE_ALL) {
        size_t b = history_bytes(s, true);
        bool any = false;
        for (size_t i = 0; i < s->ndocs; i++) {
            if (pc_history_can_undo(s->docs[i].history) || pc_history_can_redo(s->docs[i].history)) {
                any = true;
                break;
            }
        }
        if (any) {
            for (size_t i = 0; i < s->ndocs; i++) {
                pc_history_clear(s->docs[i].history);
                free(s->docs[i].coalesce);
                s->docs[i].coalesce = NULL;
            }
            freed += b;
            items[nitems++] = "histories";
        }
    }

    if (what == PURGE_ALL) {
        size_t fx = pc_compose_purge_effect_cache();
        if (fx > 0) {
            freed += fx;
            items[nitems++] = "effect cache";
        }
        size_t masks = pc_compose_purge_mask_cache();
        if (masks > 0) {
            freed += masks;
            items[nitems++] = "mask cache";
        }
        size_t stages = pc_engine_smart_purge_stack_cache();
        if (stages > 0) {
            freed += stages;
            items[nitems++] = "smart filter cache";
        }
    }

    /* Purging history/clipboard invalidates the recorded Fade source (as Session purge drops it). */
    s->has_fade = false;

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nitems; i++) {
        yyjson_mut_arr_add_strcpy(rd, arr, items[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "purged", arr);
    yyjson_mut_obj_add_uint(rd, root, "bytes", (uint64_t)freed);
    if (nitems == 0) {
        yyjson_mut_obj_add_str(rd, root, "message", "nothing to purge");
    } else {
        char msg[256];
        size_t pos = 0;
        const char prefix[] = "purged ";
        memcpy(msg, prefix, sizeof prefix - 1);
        pos = sizeof prefix - 1;
        for (size_t i = 0; i < nitems; i++) {
            if (i > 0 && pos + 2 < sizeof msg) {
                msg[pos++] = ',';
                msg[pos++] = ' ';
            }
            size_t len = strlen(items[i]);
            if (pos + len < sizeof msg) {
                memcpy(msg + pos, items[i], len);
                pos += len;
            }
        }
        msg[pos < sizeof msg ? pos : sizeof msg - 1] = '\0';
        yyjson_mut_obj_add_strcpy(rd, root, "message", msg);
    }
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status can_purge_undo(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d != NULL && pc_history_can_undo(d->history)) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "nothing to purge");
}

static pc_status can_purge_clipboard(const pc_engine_session *s, pc_error *reason)
{
    if (s->clipboard != NULL) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "the clipboard is empty");
}

static pc_status can_purge_histories(const pc_engine_session *s, pc_error *reason)
{
    for (size_t i = 0; i < s->ndocs; i++) {
        if (pc_history_can_undo(s->docs[i].history) || pc_history_can_redo(s->docs[i].history)) {
            return PC_OK;
        }
    }
    return pc_fail(reason, PC_ERR_INVALID, "no history to purge");
}

static pc_status can_purge_all(const pc_engine_session *s, pc_error *reason)
{
    if (can_purge_histories(s, NULL) == PC_OK || can_purge_clipboard(s, NULL) == PC_OK) {
        return PC_OK;
    }
    if (pc_compose_effect_cache_bytes() > 0 || pc_compose_mask_cache_bytes() > 0 ||
        pc_engine_smart_stack_cache_bytes() > 0) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "nothing to purge");
}

static pc_status always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

/* ---- run entry points --------------------------------------------------------------------- */

static pc_status run_undo(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return do_purge(s, PURGE_UNDO, out, err);
}
static pc_status run_clipboard(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return do_purge(s, PURGE_CLIPBOARD, out, err);
}
static pc_status run_histories(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return do_purge(s, PURGE_HISTORIES, out, err);
}
static pc_status run_all(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return do_purge(s, PURGE_ALL, out, err);
}

static pc_status run_video_cache(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_val(rd, root, "purged", yyjson_mut_arr(rd));
    yyjson_mut_obj_add_uint(rd, root, "bytes", 0);
    yyjson_mut_obj_add_str(rd, root, "message", "nothing to purge (no video layers)");
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ======================= Define Custom Shape (Edit > Define Custom Shape) ================== *
 * Port of edit_menu_cmds::{current_path, define_custom_shape} and the EditState::custom_shapes
 * library (shared with the Shapes panel, shape.presets.*). Never crashes on hostile params. */

static char *dup_cstr(const char *s)
{
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

/* ok(p): the path has at least one subpath (edit_menu_cmds::current_path's `ok`). */
static bool path_has_subpaths(const pc_path *p)
{
    return p != NULL && p->nsubpaths > 0;
}

bool pc_engine_current_path(const pc_document *doc, bool has_active, pc_layer_id active,
                            const char *spec, pc_path *out)
{
    memset(out, 0, sizeof *out);
    if (spec != NULL) {
        if (strcmp(spec, "work") == 0) {
            if (doc->has_work_path && path_has_subpaths(&doc->work_path)) {
                return pc_path_clone(&doc->work_path, out, NULL) == PC_OK;
            }
            return false;
        }
        for (size_t i = 0; i < doc->npaths; i++) {
            if (strcmp(pc_str_c(&doc->paths[i].name), spec) == 0) {
                if (path_has_subpaths(&doc->paths[i].path)) {
                    return pc_path_clone(&doc->paths[i].path, out, NULL) == PC_OK;
                }
                return false;
            }
        }
        return false;
    }
    if (doc->has_work_path && path_has_subpaths(&doc->work_path)) {
        return pc_path_clone(&doc->work_path, out, NULL) == PC_OK;
    }
    if (has_active) {
        const pc_layer *l = pc_document_layer(doc, active);
        if (l != NULL) {
            if (l->content.kind == PC_LAYER_SHAPE && path_has_subpaths(&l->content.u.shape.path)) {
                return pc_path_clone(&l->content.u.shape.path, out, NULL) == PC_OK;
            }
            if (l->has_vector_mask && path_has_subpaths(&l->vector_mask.path)) {
                return pc_path_clone(&l->vector_mask.path, out, NULL) == PC_OK;
            }
        }
    }
    if (doc->npaths > 0 && path_has_subpaths(&doc->paths[doc->npaths - 1].path)) {
        return pc_path_clone(&doc->paths[doc->npaths - 1].path, out, NULL) == PC_OK;
    }
    return false;
}

const pc_engine_custom_shape *pc_engine_custom_shapes(const pc_engine_session *s, size_t *n)
{
    *n = s->ncustom_shapes;
    return s->custom_shapes;
}

pc_engine_custom_shape *pc_engine_custom_shapes_mut(pc_engine_session *s, size_t *n)
{
    *n = s->ncustom_shapes;
    return s->custom_shapes;
}

void pc_engine_custom_shapes_remove(pc_engine_session *s, const char *name)
{
    for (size_t i = 0; i < s->ncustom_shapes;) {
        if (s->custom_shapes[i].name != NULL && strcmp(s->custom_shapes[i].name, name) == 0) {
            free(s->custom_shapes[i].name);
            pc_path_fini(&s->custom_shapes[i].path);
            size_t tail = s->ncustom_shapes - i - 1;
            if (tail > 0) {
                memmove(&s->custom_shapes[i], &s->custom_shapes[i + 1], tail * sizeof *s->custom_shapes);
            }
            s->ncustom_shapes--;
        } else {
            i++;
        }
    }
}

void pc_engine_edit_state_fini(pc_engine_session *s)
{
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->ncustom_shapes; i++) {
        free(s->custom_shapes[i].name);
        pc_path_fini(&s->custom_shapes[i].path);
    }
    free(s->custom_shapes);
    s->custom_shapes = NULL;
    s->ncustom_shapes = s->cap_custom_shapes = 0;
}

static pc_status can_define_shape(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_path path;
    if (!pc_engine_current_path(doc, d->has_active_layer, d->active_layer, NULL, &path)) {
        return pc_fail(reason, PC_ERR_INVALID, "select a path or shape layer first");
    }
    pc_path_fini(&path);
    return PC_OK;
}

static pc_status run_define_custom_shape(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                         pc_error *err)
{
    const char *CMD = "edit.defineCustomShape";
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    size_t slen = 0;
    const char *spec = pc_json_as_str(pc_json_get(p, "path"), &slen);
    char specbuf[512];
    const char *specarg = NULL;
    if (spec != NULL && slen < sizeof specbuf) {
        memcpy(specbuf, spec, slen);
        specbuf[slen] = '\0';
        specarg = specbuf;
    }
    pc_path path;
    if (!pc_engine_current_path(doc, st->has_active_layer, st->active_layer, specarg, &path)) {
        return pc_engine_err_bad_params(err, CMD, "no such path");
    }
    /* name: trimmed non-empty, else "Shape N". */
    char namebuf[512];
    const char *name = NULL;
    size_t nlen = 0;
    const char *nraw = pc_json_as_str(pc_json_get(p, "name"), &nlen);
    if (nraw != NULL) {
        size_t a = 0, b = nlen;
        while (a < b && (nraw[a] == ' ' || nraw[a] == '\t' || nraw[a] == '\n' || nraw[a] == '\r')) {
            a++;
        }
        while (b > a && (nraw[b - 1] == ' ' || nraw[b - 1] == '\t' || nraw[b - 1] == '\n' || nraw[b - 1] == '\r')) {
            b--;
        }
        if (b > a && (b - a) < sizeof namebuf) {
            memcpy(namebuf, nraw + a, b - a);
            namebuf[b - a] = '\0';
            name = namebuf;
        }
    }
    char autoname[64];
    if (name == NULL) {
        (void)snprintf(autoname, sizeof autoname, "Shape %zu", s->ncustom_shapes + 1);
        name = autoname;
    }
    /* Replace an existing shape of the same name, else append. */
    for (size_t i = 0; i < s->ncustom_shapes; i++) {
        if (s->custom_shapes[i].name != NULL && strcmp(s->custom_shapes[i].name, name) == 0) {
            pc_path_fini(&s->custom_shapes[i].path);
            s->custom_shapes[i].path = path; /* move */
            goto emit;
        }
    }
    if (s->ncustom_shapes == s->cap_custom_shapes) {
        size_t ncap = s->cap_custom_shapes == 0 ? 8 : s->cap_custom_shapes * 2;
        pc_engine_custom_shape *na = realloc(s->custom_shapes, ncap * sizeof *na);
        if (na == NULL) {
            pc_path_fini(&path);
            return pc_fail(err, PC_ERR_NOMEM, "custom shape");
        }
        s->custom_shapes = na;
        s->cap_custom_shapes = ncap;
    }
    {
        char *nm = dup_cstr(name);
        if (nm == NULL) {
            pc_path_fini(&path);
            return pc_fail(err, PC_ERR_NOMEM, "custom shape");
        }
        s->custom_shapes[s->ncustom_shapes].name = nm;
        s->custom_shapes[s->ncustom_shapes].path = path; /* move */
        s->ncustom_shapes++;
    }
emit:;
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, root, "name", name);
    yyjson_mut_obj_add_uint(d, root, "count", s->ncustom_shapes);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ======================= Find and Replace Text (edit_menu_cmds) =========================== */

/* word char: alphanumeric or underscore (char::is_alphanumeric || '_'). */
static bool is_word_cp(uint32_t cp)
{
    return cp == '_' || pc_text_is_alphabetic(cp) || pc_text_is_numeric(cp);
}

/* The codepoint ending just before byte `at` in `text` (false when at the start). */
static bool prev_cp(const char *text, size_t at, uint32_t *out)
{
    if (at == 0) {
        return false;
    }
    /* Step back over up to 3 continuation bytes to a lead byte. */
    size_t start = at - 1;
    size_t steps = 0;
    while (start > 0 && ((unsigned char)text[start] & 0xC0) == 0x80 && steps < 3) {
        start--;
        steps++;
    }
    size_t j = start;
    return pc_utf8_next((const uint8_t *)text, at, &j, out);
}

/* Fold one codepoint into `out`: lowercased per char::to_lowercase when !case, else as-is. */
static pc_status fold_cp(uint32_t cp, bool case_sensitive, pc_buf *out, pc_error *err)
{
    if (case_sensitive) {
        return pc_utf8_put(out, cp, err);
    }
    /* Encode cp, then full-lowercase that single char (char::to_lowercase: U+0130 -> 2 chars). */
    pc_buf enc = {0};
    if (pc_utf8_put(&enc, cp, err) != PC_OK) {
        pc_buf_fini(&enc);
        return PC_ERR_NOMEM;
    }
    pc_status rc = pc_text_lower((const char *)enc.data, enc.len, out, err);
    pc_buf_fini(&enc);
    return rc;
}

/* Fold a whole UTF-8 string (char by char). */
static pc_status fold_str(const char *s, size_t len, bool case_sensitive, pc_buf *out, pc_error *err)
{
    size_t i = 0;
    uint32_t cp;
    while (pc_utf8_next((const uint8_t *)s, len, &i, &cp)) {
        if (fold_cp(cp, case_sensitive, out, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
    }
    return PC_OK;
}

pc_status pc_engine_find_matches(const char *text, size_t tlen, const char *find, size_t flen,
                                 bool case_sensitive, bool whole, pc_text_match **out, size_t *n,
                                 pc_error *err)
{
    *out = NULL;
    *n = 0;
    if (flen == 0 || tlen == 0) {
        return PC_OK;
    }
    pc_buf needle = {0};
    if (fold_str(find, flen, case_sensitive, &needle, err) != PC_OK) {
        pc_buf_fini(&needle);
        return PC_ERR_NOMEM;
    }
    if (needle.len == 0) {
        pc_buf_fini(&needle);
        return PC_OK;
    }
    pc_text_match *res = NULL;
    size_t nres = 0, cap = 0;
    pc_status rc = PC_OK;
    size_t at = 0;
    pc_buf hay = {0};
    while (at < tlen) {
        hay.len = 0;
        size_t end = at;
        size_t i = at;
        uint32_t cp;
        while (i < tlen && hay.len < needle.len) {
            size_t j = i;
            if (!pc_utf8_next((const uint8_t *)text, tlen, &j, &cp)) {
                break;
            }
            if (fold_cp(cp, case_sensitive, &hay, err) != PC_OK) {
                rc = PC_ERR_NOMEM;
                goto done;
            }
            end = j;
            i = j;
        }
        bool matched = hay.len == needle.len && memcmp(hay.data, needle.data, needle.len) == 0;
        if (matched) {
            bool ok = !whole;
            if (!ok) {
                uint32_t bcp, acp;
                bool bw = prev_cp(text, at, &bcp) && is_word_cp(bcp);
                size_t k = end;
                bool aw = pc_utf8_next((const uint8_t *)text, tlen, &k, &acp) && is_word_cp(acp);
                ok = !bw && !aw;
            }
            if (ok) {
                if (nres == cap) {
                    size_t ncap = cap == 0 ? 8 : cap * 2;
                    pc_text_match *na = realloc(res, ncap * sizeof *na);
                    if (na == NULL) {
                        rc = pc_fail(err, PC_ERR_NOMEM, "matches");
                        goto done;
                    }
                    res = na;
                    cap = ncap;
                }
                res[nres].a = at;
                res[nres].b = end;
                nres++;
                at = end;
                continue;
            }
        }
        /* Advance by one char. */
        size_t j = at;
        if (!pc_utf8_next((const uint8_t *)text, tlen, &j, &cp) || j <= at) {
            at++;
        } else {
            at = j;
        }
    }
done:
    pc_buf_fini(&hay);
    pc_buf_fini(&needle);
    if (rc != PC_OK) {
        free(res);
        return rc;
    }
    *out = res;
    *n = nres;
    return PC_OK;
}

/* Type layers in Layers-panel order (top first), skipping Lock All layers; reversed when !forward.
 * *out is an owned array (free()) of *n ids. */
static pc_status type_layers(const pc_document *doc, bool forward, pc_layer_id **out, size_t *n,
                             pc_error *err)
{
    *out = NULL;
    *n = 0;
    pc_walk w = {0};
    if (pc_document_walk(doc, &w, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_layer_id *ids = pc_malloc_array(w.len == 0 ? 1 : w.len, sizeof *ids);
    if (ids == NULL) {
        pc_walk_fini(&w);
        return pc_fail(err, PC_ERR_NOMEM, "type layers");
    }
    size_t cnt = 0;
    /* walk is bottom-to-top; top-first = reverse. */
    for (size_t k = w.len; k > 0; k--) {
        const pc_layer *l = w.items[k - 1].layer;
        if (l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        if (pc_document_effective_locks(doc, l->id).all) {
            continue;
        }
        ids[cnt++] = l->id;
    }
    pc_walk_fini(&w);
    if (!forward) {
        for (size_t a = 0; a < cnt / 2; a++) {
            pc_layer_id t = ids[a];
            ids[a] = ids[cnt - 1 - a];
            ids[cnt - 1 - a] = t;
        }
    }
    *out = ids;
    *n = cnt;
    return PC_OK;
}

static pc_status has_type_layer(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (type_layers(doc, true, &ids, &n, NULL) != PC_OK) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no type layers");
    }
    free(ids);
    if (n == 0) {
        return pc_fail(reason, PC_ERR_INVALID, "the document has no type layers");
    }
    return PC_OK;
}

/* The text of a text layer (NULL when the layer is not a type layer). */
static const char *layer_text(const pc_document *doc, pc_layer_id id, size_t *len)
{
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
        return NULL;
    }
    *len = l->content.u.text.text.len;
    return pc_str_c(&l->content.u.text.text);
}

/* edit.findAndReplaceText action=changeAll: a planned replacement per layer. */
struct ca_plan {
    pc_layer_id id;
    pc_text_match *m;
    size_t nm;
};

struct ca_ctx {
    struct ca_plan *plan;
    size_t nplan;
    const char *replace;
    size_t replace_len;
};

static pc_status change_all_cb(pc_document *doc, bool *ha, pc_layer_id *al, void *v, pc_error *err)
{
    (void)ha;
    (void)al;
    struct ca_ctx *c = v;
    pc_document snapshot;
    if (pc_document_clone(doc, &snapshot, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    for (size_t p = 0; p < c->nplan; p++) {
        pc_layer *l = pc_document_layer_mut(doc, c->plan[p].id);
        if (l == NULL || l->content.kind != PC_LAYER_TEXT) {
            continue;
        }
        pc_text_layer *t = &l->content.u.text;
        /* Apply matches back to front so earlier offsets stay valid. */
        for (size_t k = c->plan[p].nm; k > 0; k--) {
            pc_text_match m = c->plan[p].m[k - 1];
            pc_engine_replace_text(t, m.a, m.b, c->replace, c->replace_len);
        }
        pc_engine_type_refresh(&snapshot, t);
    }
    pc_document_fini(&snapshot);
    return PC_OK;
}

struct one_ctx {
    pc_layer_id id;
    size_t a, b;
    const char *replace;
    size_t replace_len;
};

static pc_status replace_one_cb(pc_document *doc, bool *ha, pc_layer_id *al, void *v, pc_error *err)
{
    (void)ha;
    (void)al;
    struct one_ctx *c = v;
    pc_document snapshot;
    if (pc_document_clone(doc, &snapshot, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l != NULL && l->content.kind == PC_LAYER_TEXT) {
        pc_text_layer *t = &l->content.u.text;
        pc_engine_replace_text(t, c->a, c->b, c->replace, c->replace_len);
        pc_engine_type_refresh(&snapshot, t);
    }
    pc_document_fini(&snapshot);
    return PC_OK;
}

static bool bool_or(yyjson_val *p, const char *k, bool d)
{
    bool v = d;
    (void)pc_json_as_bool(pc_json_get(p, k), &v);
    return v;
}

/* Does the layer's current text contain the match range [a,b) exactly (verifying the cursor)? */
static bool match_present(const pc_document *doc, pc_layer_id id, const char *find, size_t flen,
                          bool cs, bool whole, size_t a, size_t b)
{
    size_t tlen = 0;
    const char *text = layer_text(doc, id, &tlen);
    if (text == NULL) {
        return false;
    }
    pc_text_match *ms = NULL;
    size_t nm = 0;
    if (pc_engine_find_matches(text, tlen, find, flen, cs, whole, &ms, &nm, NULL) != PC_OK) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < nm; i++) {
        if (ms[i].a == a && ms[i].b == b) {
            found = true;
            break;
        }
    }
    free(ms);
    return found;
}

static pc_status run_find_replace(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *CMD = "edit.findAndReplaceText";
    size_t flen = 0;
    const char *find = pc_json_as_str(pc_json_get(p, "find"), &flen);
    if (find == NULL || flen == 0) {
        return pc_engine_err_bad_params(err, CMD, "missing `find`");
    }
    /* Copy find and replace (params doc is freed as the command runs through edits). */
    char *findc = pc_strndup(find, flen);
    size_t rlen = 0;
    const char *replace = pc_json_as_str(pc_json_get(p, "replace"), &rlen);
    char *replacec = replace != NULL ? pc_strndup(replace, rlen) : pc_strndup("", 0);
    if (findc == NULL || replacec == NULL) {
        free(findc);
        free(replacec);
        return pc_fail(err, PC_ERR_NOMEM, "find");
    }
    bool cs = bool_or(p, "caseSensitive", false);
    bool whole = bool_or(p, "wholeWord", false);
    bool forward = bool_or(p, "forward", true);
    bool all_layers = bool_or(p, "allLayers", true);
    size_t alen = 0;
    const char *action = pc_json_as_str(pc_json_get(p, "action"), &alen);
    char actbuf[16];
    const char *act = "changeAll";
    if (action != NULL && alen < sizeof actbuf) {
        memcpy(actbuf, action, alen);
        actbuf[alen] = '\0';
        act = actbuf;
    }

    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        free(findc);
        free(replacec);
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_layer_id *layers = NULL;
    size_t nlayers = 0;
    if (type_layers(doc, forward, &layers, &nlayers, err) != PC_OK) {
        free(findc);
        free(replacec);
        return PC_ERR_NOMEM;
    }
    if (!all_layers) {
        pc_layer_id active;
        bool have = pc_engine_layer_param(s, p, &active, NULL) == PC_OK;
        size_t keep = 0;
        for (size_t i = 0; i < nlayers; i++) {
            if (have && layers[i] == active) {
                layers[keep++] = layers[i];
            }
        }
        nlayers = keep;
    }

    pc_status rc = PC_OK;
    if (strcmp(act, "changeAll") == 0) {
        struct ca_plan *plan = nlayers > 0 ? calloc(nlayers, sizeof *plan) : NULL;
        if (nlayers > 0 && plan == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "plan");
            goto cleanup;
        }
        size_t nplan = 0, total = 0;
        for (size_t i = 0; i < nlayers; i++) {
            size_t tlen = 0;
            const char *text = layer_text(doc, layers[i], &tlen);
            if (text == NULL) {
                continue;
            }
            pc_text_match *ms = NULL;
            size_t nm = 0;
            if (pc_engine_find_matches(text, tlen, findc, flen, cs, whole, &ms, &nm, err) != PC_OK) {
                for (size_t q = 0; q < nplan; q++) {
                    free(plan[q].m);
                }
                free(plan);
                rc = PC_ERR_NOMEM;
                goto cleanup;
            }
            if (nm > 0) {
                plan[nplan].id = layers[i];
                plan[nplan].m = ms;
                plan[nplan].nm = nm;
                nplan++;
                total += nm;
            } else {
                free(ms);
            }
        }
        if (total == 0) {
            free(plan);
            yyjson_mut_doc *d = NULL;
            if (pc_engine_result_doc(out, &d, err) != PC_OK) {
                rc = PC_ERR_NOMEM;
                goto cleanup;
            }
            yyjson_mut_val *root = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, root, "count", 0);
            yyjson_mut_obj_add_uint(d, root, "layers", 0);
            yyjson_mut_doc_set_root(d, root);
            goto cleanup;
        }
        struct ca_ctx ctx = {plan, nplan, replacec, strlen(replacec)};
        rc = pc_engine_session_edit(s, "Replace All Text", change_all_cb, &ctx, err);
        for (size_t q = 0; q < nplan; q++) {
            free(plan[q].m);
        }
        free(plan);
        if (rc != PC_OK) {
            goto cleanup;
        }
        s->has_find_cursor = false;
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            goto cleanup;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_uint(d, root, "count", total);
        yyjson_mut_obj_add_uint(d, root, "layers", nplan);
        yyjson_mut_doc_set_root(d, root);
        goto cleanup;
    }
    if (strcmp(act, "find") == 0 || strcmp(act, "change") == 0 || strcmp(act, "changeFind") == 0) {
        bool have_changed = false;
        pc_layer_id changed_layer = 0;
        size_t changed_a = 0, changed_b = 0;
        /* A pending Change/Find replace at the cursor. */
        if (strcmp(act, "find") != 0 && s->has_find_cursor) {
            pc_layer_id cid = s->find_layer;
            size_t ca = s->find_a, cb = s->find_b;
            if (match_present(doc, cid, findc, flen, cs, whole, ca, cb)) {
                if (pc_engine_check_text_unlocked(doc, cid, err) != PC_OK) {
                    rc = err != NULL ? err->code : PC_ERR_INVALID;
                    goto cleanup;
                }
                struct one_ctx c1 = {cid, ca, cb, replacec, strlen(replacec)};
                rc = pc_engine_session_edit(s, "Replace Text", replace_one_cb, &c1, err);
                if (rc != PC_OK) {
                    goto cleanup;
                }
                size_t end = ca + strlen(replacec);
                size_t cur = forward ? end : ca;
                s->has_find_cursor = true;
                s->find_layer = cid;
                s->find_a = cur;
                s->find_b = cur;
                have_changed = true;
                changed_layer = cid;
                changed_a = ca;
                changed_b = end;
                if (strcmp(act, "change") == 0) {
                    yyjson_mut_doc *d = NULL;
                    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
                        rc = PC_ERR_NOMEM;
                        goto cleanup;
                    }
                    yyjson_mut_val *root = yyjson_mut_obj(d);
                    yyjson_mut_val *ch = yyjson_mut_obj(d);
                    yyjson_mut_obj_add_uint(d, ch, "layer", cid);
                    yyjson_mut_obj_add_uint(d, ch, "start", ca);
                    yyjson_mut_obj_add_uint(d, ch, "end", end);
                    yyjson_mut_obj_add_val(d, root, "changed", ch);
                    yyjson_mut_doc_set_root(d, root);
                    goto cleanup;
                }
            }
        }
        /* The edit may have changed the document; re-fetch it. */
        const pc_engine_doc_state *st2 = pc_engine_session_active(s);
        if (st2 == NULL) {
            rc = pc_engine_err_no_document(err);
            goto cleanup;
        }
        const pc_document *doc2 = pc_doc_arc_get(st2->doc);
        /* Starting layer index = the cursor's layer, else 0. */
        size_t start = 0;
        if (s->has_find_cursor) {
            for (size_t i = 0; i < nlayers; i++) {
                if (layers[i] == s->find_layer) {
                    start = i;
                    break;
                }
            }
        }
        bool hit = false;
        pc_layer_id hit_id = 0;
        size_t hit_a = 0, hit_b = 0;
        for (size_t k = 0; k <= nlayers; k++) {
            if (nlayers == 0) {
                break;
            }
            pc_layer_id id = layers[(start + k) % nlayers];
            size_t tlen = 0;
            const char *text = layer_text(doc2, id, &tlen);
            if (text == NULL) {
                continue;
            }
            pc_text_match *ms = NULL;
            size_t nm = 0;
            if (pc_engine_find_matches(text, tlen, findc, flen, cs, whole, &ms, &nm, NULL) != PC_OK) {
                continue;
            }
            bool cursor_here = s->has_find_cursor && k == 0 && s->find_layer == id;
            if (cursor_here) {
                if (forward) {
                    for (size_t i = 0; i < nm; i++) {
                        if (ms[i].a >= s->find_b) {
                            hit = true;
                            hit_id = id;
                            hit_a = ms[i].a;
                            hit_b = ms[i].b;
                            break;
                        }
                    }
                } else {
                    for (size_t i = nm; i > 0; i--) {
                        if (ms[i - 1].b <= s->find_a) {
                            hit = true;
                            hit_id = id;
                            hit_a = ms[i - 1].a;
                            hit_b = ms[i - 1].b;
                            break;
                        }
                    }
                }
            } else if (nm > 0) {
                size_t idx = forward ? 0 : nm - 1;
                hit = true;
                hit_id = id;
                hit_a = ms[idx].a;
                hit_b = ms[idx].b;
            }
            free(ms);
            if (hit) {
                break;
            }
        }
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            rc = PC_ERR_NOMEM;
            goto cleanup;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        if (hit) {
            s->has_find_cursor = true;
            s->find_layer = hit_id;
            s->find_a = hit_a;
            s->find_b = hit_b;
            (void)pc_engine_session_select_layer(s, hit_id, NULL);
            size_t tlen = 0;
            const char *text = layer_text(doc2, hit_id, &tlen);
            yyjson_mut_val *fo = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, fo, "layer", hit_id);
            yyjson_mut_obj_add_uint(d, fo, "start", hit_a);
            yyjson_mut_obj_add_uint(d, fo, "end", hit_b);
            if (text != NULL && hit_b <= tlen && hit_a <= hit_b) {
                yyjson_mut_obj_add_strncpy(d, fo, "text", text + hit_a, hit_b - hit_a);
            } else {
                yyjson_mut_obj_add_strcpy(d, fo, "text", "");
            }
            yyjson_mut_obj_add_val(d, root, "found", fo);
        } else {
            s->has_find_cursor = false;
            yyjson_mut_obj_add_val(d, root, "found", yyjson_mut_null(d));
        }
        if (have_changed) {
            yyjson_mut_val *ch = yyjson_mut_obj(d);
            yyjson_mut_obj_add_uint(d, ch, "layer", changed_layer);
            yyjson_mut_obj_add_uint(d, ch, "start", changed_a);
            yyjson_mut_obj_add_uint(d, ch, "end", changed_b);
            yyjson_mut_obj_add_val(d, root, "changed", ch);
        } else {
            yyjson_mut_obj_add_val(d, root, "changed", yyjson_mut_null(d));
        }
        yyjson_mut_doc_set_root(d, root);
        goto cleanup;
    }
    rc = pc_engine_err_bad_params(err, CMD, "unknown action `%s` (find|change|changeFind|changeAll)", act);
cleanup:
    free(layers);
    free(findc);
    free(replacec);
    return rc;
}

static const char *const MENU_EDIT_PURGE[] = {"Edit", "Purge"};
static const char *const MENU_EDIT[] = {"Edit"};

static const pc_engine_command SPECS[] = {
    {"edit.fade", "Fade\xE2\x80\xA6", MENU_EDIT, 1, "Cmd+Shift+F",
     "{\"opacity\":0..100=100,\"mode\":\"normal|multiply|screen|overlay|softLight|hardLight|darken|"
     "lighten|difference|color|luminosity\"}",
     can_fade, run_fade, true},
    {"edit.purge.undo", "Undo", MENU_EDIT_PURGE, 2, NULL, "{}", can_purge_undo, run_undo, true},
    {"edit.purge.clipboard", "Clipboard", MENU_EDIT_PURGE, 2, NULL, "{}", can_purge_clipboard,
     run_clipboard, true},
    {"edit.purge.histories", "Histories", MENU_EDIT_PURGE, 2, NULL, "{}", can_purge_histories,
     run_histories, true},
    {"edit.purge.videoCache", "Video Cache", MENU_EDIT_PURGE, 2, NULL, "{}", always, run_video_cache,
     true},
    {"edit.purge.all", "All", MENU_EDIT_PURGE, 2, NULL, "{}", can_purge_all, run_all, true},
    {"edit.defineCustomShape", "Define Custom Shape\xE2\x80\xA6", MENU_EDIT, 1, NULL,
     "{\"name\":text,\"path\":\"work|<saved path name>\"?}", can_define_shape, run_define_custom_shape,
     true},
    {"edit.findAndReplaceText", "Find and Replace Text\xE2\x80\xA6", MENU_EDIT, 1, NULL,
     "{\"find\":text,\"replace\":text,\"action\":\"changeAll|find|change|changeFind\","
     "\"caseSensitive\":bool=false,\"wholeWord\":bool=false,\"forward\":bool=true,"
     "\"allLayers\":bool=true}",
     has_type_layer, run_find_replace, true},
};

const pc_engine_command *pc_engine_edit_menu_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
