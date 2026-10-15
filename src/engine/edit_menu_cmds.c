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

#include "base/json.h"
#include "base/mem.h"
#include "color/blend.h"
#include "compose/compose.h"
#include "compose/masks.h"
#include "ops/ops.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "brush_cmds.h" /* pc_engine_blend_from_str */
#include "engine_internal.h"
#include "smart_cmds.h"

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
};

const pc_engine_command *pc_engine_edit_menu_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
