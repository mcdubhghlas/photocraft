/* Everyday Edit-menu commands (port of crates/engine/src/edit_cmds.rs and the `edit.clear` spec
 * from commands.rs::build(), E4): the session clipboard and Clear.
 *
 * Landed here: edit.cut / edit.copy / edit.copyMerged / edit.paste / edit.pasteSpecial.pasteInPlace,
 * file.newFromClipboard, edit.clear, layer.new.layerViaCopy / layerViaCut, layer.mergeVisible,
 * edit.toggleLastState. Copy and Paste with a single Channels-panel channel targeted (the Rust
 * channel_clip module) and the Cut/Clear channel paths are ported as the static helpers below,
 * reusing the E7 channel-target primitives (pc_engine_targeted_color/channel) and the E4 fill
 * helpers (pc_engine_fill_color) -- E7 never ported channel_clip or clear_channel to C, so there
 * was nothing to un-static there (REWRITE_PLAN.md §8).
 *
 * Deferred (REWRITE_PLAN.md §7/§8): the whole-layer clipboard (copying a smart/type/shape/group
 * layer whole so Paste recreates it) needs photocraft_io::linked::select_items + the layer_copy
 * adapt/translate helpers; image.autoTone/Contrast/Color need pixels::adjust_surface (E8);
 * edit.transform.again / againCopy need the full edit.transform (transform_cmds); and the Edit >
 * guide commands (view.newGuide/…) need prefs::Unit::parse_length, which prefs.c does not export. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/resample.h"
#include "base/json.h"
#include "color/color.h"
#include "color/convert.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "allocation.h"
#include "brush_cmds.h"
#include "channel_cmds.h"
#include "edit_cmds.h"
#include "engine_internal.h"
#include "fill_cmds.h"
#include "layer_cmds.h" /* pc_engine_layer_param */

/* ---- the session clipboard (edit_cmds::Clip, pixels only) --------------------------------- */

struct pc_engine_clip {
    pc_surface surface; /* straight-alpha pixels (or a grayscale mask/channel clip) */
    pc_rect bounds;     /* content_bounds of the pixels */
    bool has_resolution;
    float resolution; /* the source document's ppi (File > New from Clipboard takes it) */
};

void pc_engine_clipboard_free(struct pc_engine_clip *c)
{
    if (c == NULL) {
        return;
    }
    pc_surface_fini(&c->surface);
    free(c);
}

/* Take over *clip (moved; its surface is consumed) as the session clipboard. On OOM *clip is
 * released and the clipboard left unchanged with an error. */
static pc_status set_clipboard(pc_engine_session *s, struct pc_engine_clip *clip, pc_error *err)
{
    struct pc_engine_clip *c = malloc(sizeof *c);
    if (c == NULL) {
        pc_surface_fini(&clip->surface);
        return pc_fail(err, PC_ERR_NOMEM, "clipboard");
    }
    *c = *clip;
    memset(clip, 0, sizeof *clip);
    pc_engine_clipboard_free(s->clipboard);
    s->clipboard = c;
    return PC_OK;
}

/* ---- small helpers ------------------------------------------------------------------------ */

static pc_pixel_format with_alpha(pc_pixel_format f)
{
    f.alpha = true;
    return f;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static pc_status check_pixels_unlocked(const pc_document *doc, pc_layer_id id, pc_error *err)
{
    const pc_layer *l = pc_document_layer(doc, id);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    pc_locks locks = pc_document_effective_locks(doc, id);
    if (locks.pixels || locks.all) {
        return pc_engine_err_other(
            err, "Could not complete your request because the layer \"%s\" is locked", pc_str_c(&l->name));
    }
    return PC_OK;
}

/* A {"layer": id, "offset": [dx, dy]} result (offset omitted when off is NULL). */
static pc_status result_layer(pc_json_value *out, pc_layer_id id, const int32_t *off, pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", id);
    if (off != NULL) {
        yyjson_mut_val *a = yyjson_mut_arr(d);
        yyjson_mut_arr_add_int(d, a, off[0]);
        yyjson_mut_arr_add_int(d, a, off[1]);
        yyjson_mut_obj_add_val(d, root, "offset", a);
    }
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* A {"bounds": [x0, y0, w, h]} result, with an optional extra string field. */
static pc_status result_bounds(pc_json_value *out, pc_rect b, const char *key, const char *val,
                               pc_error *err)
{
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, a, b.x0);
    yyjson_mut_arr_add_int(d, a, b.y0);
    yyjson_mut_arr_add_int(d, a, (int32_t)pc_rect_width(b));
    yyjson_mut_arr_add_int(d, a, (int32_t)pc_rect_height(b));
    yyjson_mut_obj_add_val(d, root, "bounds", a);
    if (key != NULL) {
        yyjson_mut_obj_add_strcpy(d, root, key, val);
    }
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- lifting pixels (edit_cmds::lift / lift_region) --------------------------------------- *
 *
 * The whole area is read at once (the reference reads tile by tile and shares unchanged tiles
 * copy-on-write; the pixel VALUES are identical -- a perf difference tracked in §7). */

static pc_status lift_region(const pc_surface *src, const pc_surface *sel, pc_rect r, size_t n,
                             float **out, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(src);
    size_t k = pc_pixel_format_channels(fmt);
    if (k == 0) {
        k = 1;
    }
    float *px = NULL;
    size_t w = pc_rect_width(r), h = pc_rect_height(r);
    size_t pixels = w * h;
    if (fmt.alpha) {
        size_t len = 0;
        pc_status rc = pc_engine_alloc_read_region(src, r, "copying pixels", &px, &len, err);
        if (rc != PC_OK) {
            return rc;
        }
    } else {
        float *opaque = NULL;
        size_t olen = 0;
        pc_status rc = pc_engine_alloc_read_region(src, r, "copying pixels", &opaque, &olen, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = pc_engine_alloc_filled(pixels * n, sizeof(float), "copying pixels", (void **)&px, err);
        if (rc != PC_OK) {
            free(opaque);
            return rc;
        }
        for (size_t i = 0; i < pixels; i++) {
            float *dst = &px[i * n];
            const float *s = &opaque[i * k];
            for (size_t c = 0; c < k; c++) {
                dst[c] = s[c];
            }
            dst[n - 1] = 1.0f; /* n == k + 1 here */
        }
        free(opaque);
    }
    if (sel != NULL) {
        float *mask = NULL;
        size_t mlen = 0;
        pc_status rc =
            pc_engine_alloc_read_region(sel, r, "applying the selection to copied pixels", &mask, &mlen, err);
        if (rc != PC_OK) {
            free(px);
            return rc;
        }
        size_t mk = pc_pixel_format_channels(pc_surface_format(sel));
        if (mk == 0) {
            mk = 1;
        }
        for (size_t i = 0; i < pixels; i++) {
            px[i * n + (n - 1)] *= mask[i * mk];
        }
        free(mask);
    }
    *out = px;
    return PC_OK;
}

/* src's pixels inside the selection (or all of them), as a straight-alpha surface. *out is
 * initialised on success (its surface + content bounds). */
static pc_status lift(const pc_surface *src, const pc_surface *sel, pc_rect canvas,
                      struct pc_engine_clip *out, pc_error *err)
{
    pc_pixel_format wa = with_alpha(pc_surface_format(src));
    pc_rect content = (sel != NULL) ? pc_surface_content_bounds(sel) : pc_surface_content_bounds(src);
    pc_rect area = pc_rect_intersect(content, canvas);
    pc_surface outs;
    pc_surface_init(&outs, wa);
    if (pc_rect_is_empty(area)) {
        out->surface = outs;
        out->bounds = PC_RECT_EMPTY;
        out->has_resolution = false;
        out->resolution = 0.0f;
        return PC_OK;
    }
    size_t n = pc_pixel_format_channels(wa);
    float *px = NULL;
    pc_status rc = lift_region(src, sel, area, n, &px, err);
    if (rc != PC_OK) {
        pc_surface_fini(&outs);
        return rc;
    }
    size_t len = (size_t)pc_rect_width(area) * pc_rect_height(area) * n;
    rc = pc_surface_write_region(&outs, area, px, len, err);
    free(px);
    if (rc != PC_OK) {
        pc_surface_fini(&outs);
        return rc;
    }
    pc_surface_prune(&outs);
    out->surface = outs;
    out->bounds = pc_surface_content_bounds(&outs);
    out->has_resolution = false;
    out->resolution = 0.0f;
    return PC_OK;
}

/* ---- Copy (edit_cmds::copy) --------------------------------------------------------------- */

/* Merged composite of the visible document, in its format with alpha. *out initialised on OK. */
static pc_status merged_surface(const pc_document *doc, pc_surface *out, pc_error *err)
{
    return pc_compose_flatten_to_surface(doc, with_alpha(pc_document_pixel_format(doc)), NULL, out, err);
}

static pc_status do_copy(pc_engine_session *s, bool merged, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_rect canvas = pc_document_bounds(doc);
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    struct pc_engine_clip clip;
    memset(&clip, 0, sizeof clip);
    pc_status rc;
    if (merged) {
        pc_surface ms;
        rc = merged_surface(doc, &ms, err);
        if (rc != PC_OK) {
            return rc;
        }
        rc = lift(&ms, sel, canvas, &clip, err);
        pc_surface_fini(&ms);
    } else {
        if (!st->has_active_layer) {
            return pc_engine_err_other(err, "no active layer");
        }
        const pc_layer *l = pc_document_layer(doc, st->active_layer);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, st->active_layer);
        }
        const pc_surface *surf = pc_layer_surface(l);
        if (surf == NULL) {
            /* A group / smart / type / shape layer with no pixel cache is copied whole in the
             * reference (whole_layer / whole_layers), which Paste recreates. That path needs
             * photocraft_io::linked::select_items; deferred (REWRITE_PLAN.md §7/§8). */
            return pc_fail(err, PC_ERR_UNSUPPORTED,
                           "copying a whole non-pixel layer to the clipboard is not yet supported");
        }
        rc = lift(surf, sel, canvas, &clip, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_rect_is_empty(clip.bounds)) {
        pc_surface_fini(&clip.surface);
        return pc_engine_err_other(err, "Could not copy: the selected area is empty");
    }
    clip.has_resolution = true;
    clip.resolution = doc->resolution_dpi;
    pc_rect b = clip.bounds;
    rc = set_clipboard(s, &clip, err);
    if (rc != PC_OK) {
        return rc;
    }
    return result_bounds(out, b, NULL, NULL, err);
}

/* Copy with the layer mask targeted (edit_cmds::copy_mask): the mask values as a grayscale clip. */
static pc_status copy_mask(pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_rect canvas = pc_document_bounds(doc);
    if (!st->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, st->active_layer);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, st->active_layer);
    }
    if (!l->has_mask) {
        return pc_engine_err_other(err, "the active layer has no layer mask");
    }
    const pc_surface *mask = &l->mask.surface;
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_rect area = (sel != NULL) ? pc_rect_intersect(pc_surface_content_bounds(sel), canvas) : canvas;
    if (pc_rect_is_empty(area)) {
        return pc_engine_err_other(err, "Could not copy: the selected area is empty");
    }
    size_t n = pc_pixel_format_channels(pc_surface_format(mask));
    if (n == 0) {
        n = 1;
    }
    float *values = NULL;
    size_t vlen = 0;
    pc_status rc = pc_engine_alloc_read_region(mask, area, "copying the mask", &values, &vlen, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t pixels = vlen / n;
    float *cover = NULL;
    size_t clen = 0, mk = 1;
    if (sel != NULL) {
        rc = pc_engine_alloc_read_region(sel, area, "copying the mask", &cover, &clen, err);
        if (rc != PC_OK) {
            free(values);
            return rc;
        }
        mk = pc_pixel_format_channels(pc_surface_format(sel));
        if (mk == 0) {
            mk = 1;
        }
    }
    float *px = NULL;
    rc = pc_engine_alloc_filled(pixels * 2, sizeof(float), "copying the mask", (void **)&px, err);
    if (rc != PC_OK) {
        free(values);
        free(cover);
        return rc;
    }
    for (size_t i = 0; i < pixels; i++) {
        px[i * 2] = values[i * n];
        px[i * 2 + 1] = (cover != NULL) ? cover[i * mk] : 1.0f;
    }
    free(values);
    free(cover);
    pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, pc_surface_format(mask).sample, true};
    pc_surface surface;
    pc_surface_init(&surface, fmt);
    rc = pc_surface_write_region(&surface, area, px, pixels * 2, err);
    free(px);
    if (rc != PC_OK) {
        pc_surface_fini(&surface);
        return rc;
    }
    pc_surface_prune(&surface);
    pc_rect b = pc_surface_content_bounds(&surface);
    if (pc_rect_is_empty(b)) {
        pc_surface_fini(&surface);
        return pc_engine_err_other(err, "Could not copy: the selected area is empty");
    }
    struct pc_engine_clip clip = {surface, b, true, doc->resolution_dpi};
    rc = set_clipboard(s, &clip, err);
    if (rc != PC_OK) {
        return rc;
    }
    return result_bounds(out, b, "target", "mask", err);
}

/* ---- Copy / Paste / Cut with a Channels-panel channel targeted (channel_clip) ------------- */

/* The colour channel a paste writes into, or -1 (channel_clip::paste_color_target). */
static int paste_color_target(const pc_engine_session *s, yyjson_val *p)
{
    if (p != NULL && pc_json_get(p, "target") != NULL) {
        return -1;
    }
    size_t k = pc_engine_targeted_color(s);
    return (k == SIZE_MAX) ? -1 : (int)k;
}

/* Copy reads only: with an alpha channel targeted it needs no pixel layer. */
static bool copies_alpha_channel(const pc_engine_session *s)
{
    bool quick = false;
    size_t idx = 0;
    return pc_engine_targeted_channel(s, &quick, &idx) && !quick;
}

/* Copy the targeted colour / alpha channel as a grayscale clip (channel_clip::copy). *did is set
 * true when a single channel is targeted (and the clip was taken); false means "Copy the layer". */
static pc_status cc_copy(pc_engine_session *s, bool *did, pc_json_value *out, pc_error *err)
{
    *did = false;
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return PC_OK;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_rect canvas = pc_document_bounds(doc);
    const pc_surface *src = NULL;
    size_t k = 0;
    size_t colour = pc_engine_targeted_color(s);
    bool quick = false;
    size_t alpha_idx = 0;
    bool alpha = pc_engine_targeted_channel(s, &quick, &alpha_idx) && !quick;
    if (colour != SIZE_MAX) {
        if (!st->has_active_layer) {
            return pc_engine_err_other(err, "no active layer");
        }
        const pc_layer *l = pc_document_layer(doc, st->active_layer);
        const pc_surface *surf = (l != NULL) ? pc_layer_surface(l) : NULL;
        if (surf == NULL) {
            return pc_engine_err_other(err, "the active layer has no pixels");
        }
        src = surf;
        k = colour;
    } else if (alpha) {
        if (alpha_idx >= doc->nchannels) {
            return pc_engine_err_other(err, "no alpha channel %zu (document has %zu)", alpha_idx,
                                       doc->nchannels);
        }
        src = &doc->channels[alpha_idx].surface;
        k = 0;
    } else {
        return PC_OK; /* composite / layer-mask / vector-mask targeted: Copy copies the layer */
    }
    size_t n = pc_pixel_format_channels(pc_surface_format(src));
    if (n == 0) {
        n = 1;
    }
    if (k >= n) {
        return pc_engine_err_other(err, "no colour channel %zu in this layer", k);
    }
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_rect area = (sel != NULL) ? pc_rect_intersect(pc_surface_content_bounds(sel), canvas) : canvas;
    if (pc_rect_is_empty(area)) {
        return pc_engine_err_other(err, "Could not copy: the selected area is empty");
    }
    float *values = NULL;
    size_t vlen = 0;
    pc_status rc = pc_engine_alloc_read_region(src, area, "copying pixels", &values, &vlen, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t pixels = vlen / n;
    float *cover = NULL;
    size_t clen = 0, mk = 1;
    if (sel != NULL) {
        rc = pc_engine_alloc_read_region(sel, area, "copying pixels", &cover, &clen, err);
        if (rc != PC_OK) {
            free(values);
            return rc;
        }
        mk = pc_pixel_format_channels(pc_surface_format(sel));
        if (mk == 0) {
            mk = 1;
        }
    }
    float *px = NULL;
    rc = pc_engine_alloc_filled(pixels * 2, sizeof(float), "copying pixels", (void **)&px, err);
    if (rc != PC_OK) {
        free(values);
        free(cover);
        return rc;
    }
    for (size_t i = 0; i < pixels; i++) {
        px[i * 2] = values[i * n + k];
        px[i * 2 + 1] = (cover != NULL) ? cover[i * mk] : 1.0f;
    }
    free(values);
    free(cover);
    pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, pc_document_pixel_format(doc).sample, true};
    pc_surface surface;
    pc_surface_init(&surface, fmt);
    rc = pc_surface_write_region(&surface, area, px, pixels * 2, err);
    free(px);
    if (rc != PC_OK) {
        pc_surface_fini(&surface);
        return rc;
    }
    pc_surface_prune(&surface);
    pc_rect b = pc_surface_content_bounds(&surface);
    if (pc_rect_is_empty(b)) {
        pc_surface_fini(&surface);
        return pc_engine_err_other(err, "Could not copy: the selected area is empty");
    }
    struct pc_engine_clip clip = {surface, b, true, doc->resolution_dpi};
    rc = set_clipboard(s, &clip, err);
    if (rc != PC_OK) {
        return rc;
    }
    *did = true;
    /* result {"bounds": [...], "channel": k} */
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, a, b.x0);
    yyjson_mut_arr_add_int(d, a, b.y0);
    yyjson_mut_arr_add_int(d, a, (int32_t)pc_rect_width(b));
    yyjson_mut_arr_add_int(d, a, (int32_t)pc_rect_height(b));
    yyjson_mut_obj_add_val(d, root, "bounds", a);
    yyjson_mut_obj_add_uint(d, root, "channel", k);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- Clear (edit_cmds::clear_area + channel_cmds::clear_*) --------------------------------- */

/* Cut / Clear on a layer: make the selected pixels transparent (the Background takes the
 * background colour instead, as it can hold no transparency). */
static pc_status clear_area(pc_document *doc, pc_layer_id id, pc_rect area, const pc_surface *sel,
                            const float background[4], pc_error *err)
{
    if (pc_document_layer(doc, id) == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    bool bg = pc_document_is_background(doc, id);
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, id, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (bg) {
        rc = pc_engine_try_fill_surface(surf, area, background, sel, true, err);
    } else {
        rc = pc_engine_try_clear_surface(surf, area, sel, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_prune(surf);
    return PC_OK;
}

static pc_status clear_selected(pc_document *doc, pc_layer_id id, const float background[4], pc_error *err)
{
    pc_rect canvas = pc_document_bounds(doc);
    pc_rect area = canvas;
    if (doc->has_selection) {
        area = pc_rect_intersect(pc_surface_content_bounds(&doc->selection), canvas);
    }
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    return clear_area(doc, id, area, sel, background, err);
}

/* Clear one colour channel k of a layer to the background colour (channel_cmds::clear_color_channel). */
static pc_status clear_color_channel(pc_document *doc, pc_layer_id id, size_t k, const float background[4],
                                     pc_error *err)
{
    pc_rect canvas = pc_document_bounds(doc);
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_rect area = (sel != NULL) ? pc_rect_intersect(pc_surface_content_bounds(sel), canvas) : canvas;
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, id, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_pixel_format_channels(fmt);
    float rgba[4] = {background[0], background[1], background[2], 1.0f};
    float enc[PC_RASTER_MAX_CHANNELS];
    size_t m = pc_from_rgba(fmt, rgba, enc);
    if (k >= n || k >= m) {
        return pc_engine_err_other(err, "no colour channel %zu in this layer", k);
    }
    float v = enc[k];
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    float *cover = NULL;
    size_t clen = 0, mk = 1;
    if (sel != NULL) {
        rc = pc_engine_alloc_read_region(sel, area, "erasing pixels", &cover, &clen, err);
        if (rc != PC_OK) {
            return rc;
        }
        mk = pc_pixel_format_channels(pc_surface_format(sel));
        if (mk == 0) {
            mk = 1;
        }
    }
    float *px = NULL;
    size_t plen = 0;
    rc = pc_engine_alloc_read_region(surf, area, "erasing pixels", &px, &plen, err);
    if (rc != PC_OK) {
        free(cover);
        return rc;
    }
    size_t pixels = plen / (n == 0 ? 1 : n);
    for (size_t i = 0; i < pixels; i++) {
        float w = (cover != NULL) ? cover[i * mk] : 1.0f;
        px[i * n + k] += (v - px[i * n + k]) * clampf(w, 0.0f, 1.0f);
    }
    free(cover);
    rc = pc_surface_write_region(surf, area, px, plen, err);
    free(px);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_prune(surf);
    return PC_OK;
}

/* Clear a targeted alpha channel or the Quick Mask to the background colour (channel_cmds::clear_channel).
 * The targeted-channel surface is reached directly (target_surface would return the same). */
static pc_status clear_channel_direct(pc_document *doc, bool is_quick, size_t alpha_idx,
                                      const float background[4], pc_error *err)
{
    pc_rect canvas = pc_document_bounds(doc);
    const pc_surface *sel = doc->has_selection ? &doc->selection : NULL;
    pc_rect area = (sel != NULL) ? pc_rect_intersect(pc_surface_content_bounds(sel), canvas) : canvas;
    if (pc_rect_is_empty(area)) {
        return PC_OK;
    }
    pc_surface *surf;
    if (is_quick) {
        if (!doc->has_quick_mask) {
            return pc_engine_err_other(err, "the document is not in Quick Mask mode");
        }
        surf = &doc->quick_mask.surface;
    } else {
        if (alpha_idx >= doc->nchannels) {
            return pc_engine_err_other(err, "no alpha channel %zu", alpha_idx);
        }
        surf = &doc->channels[alpha_idx].surface;
    }
    float color[4] = {background[0], background[1], background[2], 1.0f};
    return pc_engine_fill_color(surf, area, color, sel, err);
}

/* ---- Paste (edit_cmds::paste / paste_to_target / channel_clip::paste) --------------------- */

static int32_t finite_round_i32(double d)
{
    if (!isfinite(d)) {
        return 0;
    }
    d = round(d);
    if (d < (double)INT32_MIN) {
        return INT32_MIN;
    }
    if (d > (double)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)d;
}

/* How far a paste moves the clipboard pixels (edit_cmds::paste_offset). */
static void paste_offset(const struct pc_engine_clip *clip, pc_rect canvas, yyjson_val *p, bool in_place,
                         int32_t *dx, int32_t *dy)
{
    yyjson_val *center = (p != NULL) ? pc_json_get(p, "center") : NULL;
    pc_rect isect = pc_rect_intersect(clip->bounds, canvas);
    bool fully_on_canvas = isect.x0 == clip->bounds.x0 && isect.y0 == clip->bounds.y0 &&
                           isect.x1 == clip->bounds.x1 && isect.y1 == clip->bounds.y1;
    if (in_place || (fully_on_canvas && center == NULL)) {
        *dx = 0;
        *dy = 0;
        return;
    }
    double cx, cy;
    bool have = false;
    if (center != NULL && yyjson_is_arr(center) && yyjson_arr_size(center) >= 2) {
        double a = 0.0, b = 0.0;
        (void)pc_json_as_f64(yyjson_arr_get(center, 0), &a);
        (void)pc_json_as_f64(yyjson_arr_get(center, 1), &b);
        cx = a;
        cy = b;
        have = true;
    }
    if (!have) {
        cx = ((double)canvas.x0 + (double)canvas.x1) / 2.0;
        cy = ((double)canvas.y0 + (double)canvas.y1) / 2.0;
    }
    pc_rect b = clip->bounds;
    *dx = finite_round_i32(cx - ((double)b.x0 + (double)b.x1) / 2.0);
    *dy = finite_round_i32(cy - ((double)b.y0 + (double)b.y1) / 2.0);
}

/* clip->surface moved by (dx, dy); *out owns it (edit_cmds::shifted). */
static pc_status shifted(const pc_surface *surface, int32_t dx, int32_t dy, pc_surface *out, pc_error *err)
{
    if (dx == 0 && dy == 0) {
        return pc_surface_clone(surface, out, err);
    }
    return pc_algo_translate_surface(surface, dx, dy, out, err);
}

/* A raster layer holding `surf` (moved on OK) inserted above `above`; edits set active = it. */
struct insert_ctx {
    pc_surface surf; /* owned; consumed into the new layer */
    pc_pixel_format fmt;
    bool has_above;
    pc_layer_id above;
    bool clear_selection;
    pc_layer_id new_id;
};

static pc_status insert_raster_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                    pc_error *err)
{
    struct insert_ctx *c = vctx;
    pc_str name;
    pc_status rc = pc_document_next_layer_name(doc, "Layer", &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer layer;
    rc = pc_layer_raster(&layer, pc_str_c(&name), c->fmt, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface *ls = pc_layer_surface_mut(&layer);
    pc_surface_fini(ls);
    *ls = c->surf;
    pc_surface_init(&c->surf, c->fmt); /* the real pixels are now in `layer` */
    const pc_layer_id *above = c->has_above ? &c->above : NULL;
    pc_layer_id nid;
    rc = pc_document_insert_above(doc, above, &layer, &nid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&layer);
        return rc;
    }
    *active = nid;
    *has_active = true;
    c->new_id = nid;
    if (c->clear_selection && doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    return PC_OK;
}

/* A new document the size of the clipboard image, holding it as its one layer. */
static pc_status new_from_clipboard(pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    struct pc_engine_clip *clip = s->clipboard;
    if (clip == NULL) {
        return pc_engine_err_other(err, "the clipboard is empty");
    }
    pc_rect b = clip->bounds;
    uint32_t w = pc_rect_width(b), h = pc_rect_height(b);
    if (w == 0 || h == 0) {
        return pc_engine_err_other(err, "the clipboard image has no size");
    }
    pc_pixel_format fmt = pc_surface_format(&clip->surface);
    pc_pixel_format target = with_alpha(fmt);
    pc_surface moved;
    pc_status rc;
    if (b.x0 == 0 && b.y0 == 0) {
        rc = pc_surface_clone(&clip->surface, &moved, err);
    } else {
        rc = pc_algo_translate_surface(&clip->surface, -b.x0, -b.y0, &moved, err);
    }
    if (rc != PC_OK) {
        return rc;
    }
    if (!pc_pixel_format_eq(pc_surface_format(&moved), target)) {
        pc_surface conv;
        rc = pc_surface_convert(&moved, target, &conv, err);
        pc_surface_fini(&moved);
        if (rc != PC_OK) {
            return rc;
        }
        moved = conv;
    }
    pc_document doc;
    rc = pc_document_init(&doc, "Untitled", pc_size_make(w, h), fmt.mode, fmt.sample, err);
    if (rc != PC_OK) {
        pc_surface_fini(&moved);
        return rc;
    }
    if (clip->has_resolution && isfinite(clip->resolution) && clip->resolution > 0.0f) {
        doc.resolution_dpi = clip->resolution;
    }
    pc_str name;
    rc = pc_document_next_layer_name(&doc, "Layer", &name, err);
    if (rc != PC_OK) {
        pc_surface_fini(&moved);
        pc_document_fini(&doc);
        return rc;
    }
    pc_layer layer;
    rc = pc_layer_raster(&layer, pc_str_c(&name), target, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        pc_surface_fini(&moved);
        pc_document_fini(&doc);
        return rc;
    }
    pc_surface *ls = pc_layer_surface_mut(&layer);
    pc_surface_fini(ls);
    *ls = moved;
    pc_layer_id nid;
    rc = pc_document_insert_above(&doc, NULL, &layer, &nid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&layer);
        pc_document_fini(&doc);
        return rc;
    }
    size_t index = 0;
    rc = pc_engine_session_add_document(s, &doc, NULL, &index, err);
    if (rc != PC_OK) {
        pc_document_fini(&doc);
        return rc;
    }
    /* Clipboard content has never been saved; closing must ask before discarding it. */
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st != NULL) {
        st->saved_revision = 0;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "document", index);
    yyjson_mut_obj_add_uint(d, root, "width", w);
    yyjson_mut_obj_add_uint(d, root, "height", h);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

struct target_ctx {
    pc_surface src; /* owned */
    pc_rect area;
    bool has_layer;
    pc_layer_id layer;
    yyjson_val *p;
};

static pc_status paste_target_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                   pc_error *err)
{
    (void)has_active;
    (void)active;
    struct target_ctx *c = vctx;
    pc_surface *surf = NULL;
    bool lock = false;
    pc_status rc = pc_engine_target_surface(doc, c->has_layer, c->layer, c->p, &surf, &lock, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_engine_composite_over(surf, &c->src, c->area, NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_prune(surf);
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    return PC_OK;
}

/* Paste into the targeted mask / alpha channel / Quick Mask (edit_cmds::paste_to_target). */
static pc_status paste_to_target(pc_engine_session *s, yyjson_val *p, bool in_place, pc_json_value *out,
                                 pc_error *err)
{
    pc_status rc = pc_engine_alloc_checkpoint("pasting pixels", err);
    if (rc != PC_OK) {
        return rc;
    }
    struct pc_engine_clip *clip = s->clipboard;
    if (clip == NULL) {
        return pc_engine_err_other(err, "the clipboard is empty");
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_rect canvas = pc_document_bounds(doc);
    int32_t dx = 0, dy = 0;
    paste_offset(clip, canvas, p, in_place, &dx, &dy);
    struct target_ctx c;
    memset(&c, 0, sizeof c);
    rc = shifted(&clip->surface, dx, dy, &c.src, err);
    if (rc != PC_OK) {
        return rc;
    }
    c.area = pc_rect_intersect(pc_surface_content_bounds(&c.src), canvas);
    c.has_layer = st->has_active_layer;
    c.layer = st->active_layer;
    c.p = p;
    rc = pc_engine_session_edit(s, "Paste", paste_target_edit, &c, err);
    pc_surface_fini(&c.src);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_val *tv = (p != NULL) ? pc_json_get(p, "target") : NULL;
    if (tv != NULL) {
        yyjson_mut_obj_add_val(d, root, "target", yyjson_val_mut_copy(d, tv));
    } else {
        yyjson_mut_obj_add_null(d, root, "target");
    }
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, a, dx);
    yyjson_mut_arr_add_int(d, a, dy);
    yyjson_mut_obj_add_val(d, root, "offset", a);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

struct cc_paste_ctx {
    pc_surface src; /* owned */
    pc_rect area;
    pc_layer_id id;
    size_t k;
};

static pc_status cc_paste_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                               pc_error *err)
{
    (void)has_active;
    (void)active;
    struct cc_paste_ctx *c = vctx;
    pc_surface *surf = NULL;
    pc_status rc = pc_engine_paint_surface(doc, c->id, NULL, &surf, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t n = pc_pixel_format_channels(pc_surface_format(surf));
    if (c->k >= n) {
        return pc_engine_err_other(err, "no colour channel %zu in this layer", c->k);
    }
    if (!pc_rect_is_empty(c->area)) {
        size_t w = pc_rect_width(c->area), h = pc_rect_height(c->area);
        size_t pixels = w * h;
        float(*rgba)[4] = NULL;
        pc_status r2 = pc_engine_alloc_filled(pixels, sizeof(float[4]), "pasting pixels", (void **)&rgba, err);
        if (r2 != PC_OK) {
            return r2;
        }
        r2 = pc_surface_read_rgba_into(&c->src, c->area, rgba, pixels, err);
        if (r2 != PC_OK) {
            free(rgba);
            return r2;
        }
        float *px = NULL;
        size_t plen = 0;
        r2 = pc_engine_alloc_read_region(surf, c->area, "pasting pixels", &px, &plen, err);
        if (r2 != PC_OK) {
            free(rgba);
            return r2;
        }
        for (size_t i = 0; i < pixels; i++) {
            float rgb[3] = {rgba[i][0], rgba[i][1], rgba[i][2]};
            float v = pc_rgb_to_gray(rgb);
            float wgt = rgba[i][3];
            px[i * n + c->k] += (v - px[i * n + c->k]) * clampf(wgt, 0.0f, 1.0f);
        }
        free(rgba);
        r2 = pc_surface_write_region(surf, c->area, px, plen, err);
        free(px);
        if (r2 != PC_OK) {
            return r2;
        }
        pc_surface_prune(surf);
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    return PC_OK;
}

/* Paste the clip's luminosity into colour channel k of the active layer (channel_clip::paste). */
static pc_status cc_paste(pc_engine_session *s, yyjson_val *p, size_t k, bool in_place, pc_json_value *out,
                          pc_error *err)
{
    struct pc_engine_clip *clip = s->clipboard;
    if (clip == NULL) {
        return pc_engine_err_other(err, "the clipboard is empty");
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (!st->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_rect canvas = pc_document_bounds(doc);
    int32_t dx = 0, dy = 0;
    paste_offset(clip, canvas, p, in_place, &dx, &dy);
    struct cc_paste_ctx c;
    memset(&c, 0, sizeof c);
    pc_status rc = shifted(&clip->surface, dx, dy, &c.src, err);
    if (rc != PC_OK) {
        return rc;
    }
    c.area = pc_rect_intersect(pc_surface_content_bounds(&c.src), canvas);
    c.id = st->active_layer;
    c.k = k;
    rc = pc_engine_session_edit(s, "Paste", cc_paste_edit, &c, err);
    pc_surface_fini(&c.src);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", c.id);
    yyjson_mut_obj_add_uint(d, root, "channel", k);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, a, dx);
    yyjson_mut_arr_add_int(d, a, dy);
    yyjson_mut_obj_add_val(d, root, "offset", a);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* Paste as a new layer, or into the targeted mask / channel (edit_cmds::paste). */
static pc_status paste(pc_engine_session *s, yyjson_val *p, bool in_place, pc_json_value *out, pc_error *err)
{
    pc_status rc = pc_engine_alloc_checkpoint("pasting pixels", err);
    if (rc != PC_OK) {
        return rc;
    }
    struct pc_engine_clip *clip = s->clipboard;
    if (clip == NULL) {
        return pc_engine_err_other(err, "the clipboard is empty");
    }
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return new_from_clipboard(s, out, err); /* nothing open: a new document */
    }
    size_t alpha_idx = 0;
    if (pc_engine_target_of(p, &alpha_idx) != PC_ENGINE_PIXTGT_PIXELS) {
        return paste_to_target(s, p, in_place, out, err);
    }
    int k = paste_color_target(s, p);
    if (k >= 0) {
        return cc_paste(s, p, (size_t)k, in_place, out, err);
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_rect canvas = pc_document_bounds(doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    int32_t dx = 0, dy = 0;
    paste_offset(clip, canvas, p, in_place, &dx, &dy);
    pc_surface moved;
    rc = shifted(&clip->surface, dx, dy, &moved, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_pixel_format target = with_alpha(fmt);
    struct insert_ctx c;
    memset(&c, 0, sizeof c);
    if (pc_pixel_format_eq(pc_surface_format(&moved), target)) {
        c.surf = moved;
    } else {
        rc = pc_surface_convert(&moved, target, &c.surf, err);
        pc_surface_fini(&moved);
        if (rc != PC_OK) {
            return rc;
        }
    }
    c.fmt = target;
    c.has_above = st->has_active_layer;
    c.above = st->active_layer;
    c.clear_selection = true;
    rc = pc_engine_session_edit(s, "Paste", insert_raster_edit, &c, err);
    pc_surface_fini(&c.surf);
    if (rc != PC_OK) {
        return rc;
    }
    int32_t off[2] = {dx, dy};
    return result_layer(out, c.new_id, off, err);
}

/* ---- Layer via Copy / Cut (edit_cmds::layer_via) ------------------------------------------ */

struct via_ctx {
    pc_surface piece; /* lifted pixels (owned) */
    pc_pixel_format fmt;
    pc_layer_id src;
    bool cut;
    float bg[4];
    pc_layer_id new_id;
};

static pc_status via_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx, pc_error *err)
{
    struct via_ctx *c = vctx;
    if (c->cut) {
        pc_status rc = clear_selected(doc, c->src, c->bg, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    pc_str name;
    pc_status rc = pc_document_next_layer_name(doc, "Layer", &name, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_layer layer;
    rc = pc_layer_raster(&layer, pc_str_c(&name), c->fmt, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface *ls = pc_layer_surface_mut(&layer);
    pc_surface_fini(ls);
    *ls = c->piece;
    pc_surface_init(&c->piece, c->fmt);
    pc_layer_id nid;
    rc = pc_document_insert_above(doc, &c->src, &layer, &nid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&layer);
        return rc;
    }
    *active = nid;
    *has_active = true;
    c->new_id = nid;
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    return PC_OK;
}

struct dup_ctx {
    pc_layer_id src;
    pc_layer_id new_id;
};

static pc_status via_dup_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                              pc_error *err)
{
    struct dup_ctx *c = vctx;
    const pc_layer *src = pc_document_layer(doc, c->src);
    if (src == NULL) {
        return pc_engine_err_no_layer(err, c->src);
    }
    pc_layer dup;
    pc_status rc = pc_layer_duplicate(src, &dup, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_str cn;
    rc = pc_document_copy_name(doc, pc_str_c(&src->name), strlen(pc_str_c(&src->name)), &cn, err);
    if (rc != PC_OK) {
        pc_layer_fini(&dup);
        return rc;
    }
    pc_str_fini(&dup.name);
    dup.name = cn;
    dup.locks = (pc_locks){0};
    pc_layer_id nid;
    rc = pc_document_insert_above(doc, &c->src, &dup, &nid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&dup);
        return rc;
    }
    *active = nid;
    *has_active = true;
    c->new_id = nid;
    return PC_OK;
}

static pc_status layer_via(pc_engine_session *s, bool cut, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (!st->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    pc_layer_id id = st->active_layer;
    const pc_document *doc = pc_doc_arc_get(st->doc);
    bool has_sel = doc->has_selection;
    if (!has_sel && !cut) {
        /* No selection: Layer via Copy duplicates the whole layer. (Multiple selected layers are
         * duplicated together by layer_multi_cmds::duplicate_selected in the reference; that
         * multi-layer path is deferred to E5-multi, so a single layer is duplicated here.) */
        struct dup_ctx c = {id, id};
        pc_status rc = pc_engine_session_edit(s, "Layer Via Copy", via_dup_edit, &c, err);
        if (rc != PC_OK) {
            return rc;
        }
        return result_layer(out, c.new_id, NULL, err);
    }
    const pc_layer *l = pc_document_layer(doc, id);
    const pc_surface *surf = (l != NULL) ? pc_layer_surface(l) : NULL;
    if (surf == NULL) {
        return pc_engine_err_other(err, "the active layer has no pixels");
    }
    struct pc_engine_clip clip;
    memset(&clip, 0, sizeof clip);
    pc_status rc = lift(surf, has_sel ? &doc->selection : NULL, pc_document_bounds(doc), &clip, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (pc_rect_is_empty(clip.bounds)) {
        pc_surface_fini(&clip.surface);
        return pc_engine_err_other(err, "Could not complete the command: the selected area is empty");
    }
    struct via_ctx c;
    memset(&c, 0, sizeof c);
    c.piece = clip.surface;
    c.fmt = pc_surface_format(&clip.surface);
    c.src = id;
    c.cut = cut;
    memcpy(c.bg, pc_engine_session_tools(s)->background, sizeof c.bg);
    rc = pc_engine_session_edit(s, cut ? "Layer Via Cut" : "Layer Via Copy", via_edit, &c, err);
    pc_surface_fini(&c.piece);
    if (rc != PC_OK) {
        return rc;
    }
    return result_layer(out, c.new_id, NULL, err);
}

/* ---- Merge Visible (edit_cmds::merge_visible) --------------------------------------------- */

static pc_status merge_visible_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                    pc_error *err)
{
    (void)vctx;
    size_t n = doc->nlayers;
    size_t nvisible = 0, lowest = 0;
    bool found_lowest = false, any_group = false;
    for (size_t i = 0; i < n; i++) {
        if (doc->layers[i].visible) {
            if (!found_lowest) {
                lowest = i;
                found_lowest = true;
            }
            nvisible++;
            if (doc->layers[i].content.kind == PC_LAYER_GROUP) {
                any_group = true;
            }
        }
    }
    if (nvisible < 2 && !any_group) {
        return pc_engine_err_other(err, "there are not enough visible layers to merge");
    }
    const pc_layer *base = &doc->layers[lowest];
    pc_layer_id base_id = base->id;
    bool is_background = (lowest == 0) && pc_document_is_background(doc, base_id);
    pc_pixel_format docfmt = pc_document_pixel_format(doc);
    pc_pixel_format fmt = is_background ? docfmt : with_alpha(docfmt);

    /* Composite of the visible top-level layers only. */
    pc_document solo;
    pc_status rc = pc_document_clone(doc, &solo, err);
    if (rc != PC_OK) {
        return rc;
    }
    /* Remove the hidden top-level layers from solo. */
    for (;;) {
        bool removed = false;
        for (size_t i = 0; i < solo.nlayers; i++) {
            if (!solo.layers[i].visible) {
                pc_layer tmp;
                if (pc_document_remove(&solo, solo.layers[i].id, &tmp, err) == PC_OK) {
                    pc_layer_fini(&tmp);
                }
                removed = true;
                break;
            }
        }
        if (!removed) {
            break;
        }
    }
    float white[3] = {1.0f, 1.0f, 1.0f};
    pc_surface merged_surf;
    rc = pc_engine_composite_layers(&solo, fmt, is_background ? white : NULL, &merged_surf, err);
    pc_document_fini(&solo);
    if (rc != PC_OK) {
        return rc;
    }

    pc_layer merged;
    rc = pc_layer_raster(&merged, pc_str_c(&base->name), fmt, err);
    if (rc != PC_OK) {
        pc_surface_fini(&merged_surf);
        return rc;
    }
    merged.locks = base->locks;
    pc_surface *ms = pc_layer_surface_mut(&merged);
    pc_surface_fini(ms);
    *ms = merged_surf;
    pc_layer_id mid = merged.id;

    /* Rebuild the top-level layer array: the merged layer in the lowest-visible slot, the hidden
     * layers kept, the other visible layers dropped. */
    pc_layer *res = malloc((n == 0 ? 1 : n) * sizeof *res);
    if (res == NULL) {
        pc_layer_fini(&merged);
        return pc_fail(err, PC_ERR_NOMEM, "merge visible");
    }
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (i == lowest) {
            pc_layer_fini(&doc->layers[i]);
            res[w++] = merged;
        } else if (!doc->layers[i].visible) {
            res[w++] = doc->layers[i];
        } else {
            pc_layer_fini(&doc->layers[i]);
        }
    }
    free(doc->layers);
    doc->layers = res;
    doc->nlayers = w;
    *active = mid;
    *has_active = true;
    return PC_OK;
}

/* ---- enabled predicates ------------------------------------------------------------------- */

static pc_status has_clip_only(const pc_engine_session *s, pc_error *reason)
{
    if (s->clipboard == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "the clipboard is empty");
    }
    return PC_OK;
}

static pc_status has_clip(const pc_engine_session *s, pc_error *reason)
{
    pc_status rc = pc_engine_has_doc(s, reason);
    if (rc != PC_OK) {
        return rc;
    }
    if (s->clipboard == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "the clipboard is empty");
    }
    return PC_OK;
}

/* Cut / Layer via Cut edit the pixels, so they need a pixel layer (edit_cmds::has_pixels). */
static pc_status has_pixels(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (!st->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind == PC_LAYER_RASTER) {
        return PC_OK;
    }
    if (l->content.kind == PC_LAYER_SMART) {
        return pc_fail(reason, PC_ERR_INVALID, "the smart object is not directly editable");
    }
    return pc_fail(reason, PC_ERR_INVALID, "the active layer has no pixels");
}

/* Copy: a targeted alpha channel, or a layer with pixels (edit_cmds::can_copy; the whole-layer
 * group case is part of the deferred whole-layer clipboard). */
static pc_status can_copy(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    if (copies_alpha_channel(s)) {
        return PC_OK;
    }
    if (!st->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "the active layer has no pixels");
    }
    const pc_layer *l = pc_document_layer(pc_doc_arc_get(st->doc), st->active_layer);
    if (l != NULL && pc_layer_surface(l) != NULL) {
        return PC_OK;
    }
    return pc_fail(reason, PC_ERR_INVALID, "the active layer has no pixels");
}

/* ---- the shared Clear edit (edit.clear and the Cut clears) -------------------------------- */

enum clear_kind { CLEAR_SELECTED = 0, CLEAR_COLOR_CHANNEL = 1, CLEAR_CHANNEL = 2 };

struct clear_cmd_ctx {
    enum clear_kind kind;
    pc_layer_id id;
    size_t k;
    bool is_quick;
    size_t alpha_idx;
    float bg[4];
};

static pc_status clear_cmd_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                pc_error *err)
{
    (void)has_active;
    (void)active;
    struct clear_cmd_ctx *c = vctx;
    switch (c->kind) {
    case CLEAR_SELECTED:
        return clear_selected(doc, c->id, c->bg, err);
    case CLEAR_COLOR_CHANNEL:
        return clear_color_channel(doc, c->id, c->k, c->bg, err);
    default:
        return clear_channel_direct(doc, c->is_quick, c->alpha_idx, c->bg, err);
    }
}

/* ---- run wrappers ------------------------------------------------------------------------- */

static pc_status run_cut(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (!st->has_active_layer) {
        return pc_engine_err_other(err, "no active layer");
    }
    pc_layer_id id = st->active_layer;
    /* Refuse a locked layer before copying, so a refused Cut leaves the clipboard alone. */
    pc_status rc = check_pixels_unlocked(pc_doc_arc_get(st->doc), id, err);
    if (rc != PC_OK) {
        return rc;
    }
    struct clear_cmd_ctx cc;
    memset(&cc, 0, sizeof cc);
    cc.id = id;
    memcpy(cc.bg, pc_engine_session_tools(s)->background, sizeof cc.bg);
    int k = paste_color_target(s, p);
    if (k >= 0) {
        /* A targeted colour channel cuts that channel only (channel_clip::cut). */
        bool did = false;
        rc = cc_copy(s, &did, out, err);
        if (rc != PC_OK) {
            return rc;
        }
        if (!did) {
            return pc_engine_err_other(err, "no colour channel is targeted");
        }
        cc.kind = CLEAR_COLOR_CHANNEL;
        cc.k = (size_t)k;
        return pc_engine_session_edit(s, "Cut Pixels", clear_cmd_edit, &cc, err);
    }
    rc = do_copy(s, false, out, err); /* sets the clipboard and *out */
    if (rc != PC_OK) {
        return rc;
    }
    cc.kind = CLEAR_SELECTED;
    return pc_engine_session_edit(s, "Cut Pixels", clear_cmd_edit, &cc, err);
}

static pc_status run_copy(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    if (pc_engine_is_mask_target(p)) {
        return copy_mask(s, out, err);
    }
    bool did = false;
    pc_status rc = cc_copy(s, &did, out, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (did) {
        return PC_OK;
    }
    return do_copy(s, false, out, err);
}

static pc_status run_copy_merged(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return do_copy(s, true, out, err);
}

static pc_status run_paste(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return paste(s, p, false, out, err);
}

static pc_status run_paste_in_place(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return paste(s, p, true, out, err);
}

static pc_status run_new_from_clipboard(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                        pc_error *err)
{
    (void)p;
    return new_from_clipboard(s, out, err);
}

static pc_status run_clear(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    struct clear_cmd_ctx cc;
    memset(&cc, 0, sizeof cc);
    memcpy(cc.bg, pc_engine_session_tools(s)->background, sizeof cc.bg);
    /* A targeted alpha channel or Quick Mask is filled instead of a layer. */
    bool quick = false;
    size_t alpha_idx = 0;
    if (pc_engine_targeted_channel(s, &quick, &alpha_idx)) {
        cc.kind = CLEAR_CHANNEL;
        cc.is_quick = quick;
        cc.alpha_idx = alpha_idx;
        pc_status rc = pc_engine_session_edit(s, "Clear", clear_cmd_edit, &cc, err);
        if (rc != PC_OK) {
            return rc;
        }
        return pc_engine_result_null(out, err);
    }
    pc_layer_id id;
    pc_status rc = pc_engine_layer_param(s, p, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    cc.id = id;
    /* A targeted colour channel clears that channel only. */
    size_t colour = pc_engine_targeted_color(s);
    if (colour != SIZE_MAX) {
        cc.kind = CLEAR_COLOR_CHANNEL;
        cc.k = colour;
        rc = pc_engine_session_edit(s, "Clear", clear_cmd_edit, &cc, err);
        if (rc != PC_OK) {
            return rc;
        }
        return pc_engine_result_null(out, err);
    }
    cc.kind = CLEAR_SELECTED;
    rc = pc_engine_session_edit(s, "Clear", clear_cmd_edit, &cc, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_layer_via_copy(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return layer_via(s, false, out, err);
}

static pc_status run_layer_via_cut(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return layer_via(s, true, out, err);
}

static pc_status run_merge_visible(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    pc_status rc = pc_engine_session_edit(s, "Merge Visible", merge_visible_edit, NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    return pc_engine_result_null(out, err);
}

static pc_status run_toggle_last_state(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    bool redo = pc_history_can_redo(st->history);
    bool done = redo ? pc_engine_session_redo(s) : pc_engine_session_undo(s);
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_doc_set_root(d, yyjson_mut_bool(d, done));
    return PC_OK;
}

/* ---- specs -------------------------------------------------------------------------------- */

static const char *const MENU_EDIT[] = {"Edit"};
static const char *const MENU_FILE[] = {"File"};
static const char *const MENU_LAYER_NEW[] = {"Layer", "New"};
static const char *const MENU_LAYER[] = {"Layer"};
static const char *const MENU_EDIT_PASTE[] = {"Edit", "Paste Special"};

static const pc_engine_command SPECS[] = {
    {"edit.cut", "Cut", MENU_EDIT, 1, "Cmd+X",
     "{} (a targeted colour channel cuts that channel only)", has_pixels, run_cut, true},
    {"edit.copy", "Copy", MENU_EDIT, 1, "Cmd+C",
     "{\"target\":\"mask\"? = the targeted mask, copied as grayscale (#2928)} (a targeted colour or "
     "alpha channel copies that channel, as grayscale)",
     can_copy, run_copy, true},
    {"edit.copyMerged", "Copy Merged", MENU_EDIT, 1, "Cmd+Shift+C", "{}", pc_engine_has_doc,
     run_copy_merged, true},
    {"edit.paste", "Paste", MENU_EDIT, 1, "Cmd+V",
     "{\"center\":[x,y]? (view centre; default keeps the position when it overlaps the canvas),"
     "\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}=Channels panel target (not pixels: "
     "the luminosity pasted into that mask or channel)} (with no document open: a new document from "
     "the clipboard)",
     has_clip_only, run_paste, true},
    {"file.newFromClipboard", "New from Clipboard", MENU_FILE, 1, NULL,
     "{} (a new document the size of the clipboard image, holding it as one layer)", has_clip_only,
     run_new_from_clipboard, true},
    {"edit.pasteSpecial.pasteInPlace", "Paste in Place", MENU_EDIT_PASTE, 2, "Cmd+Shift+V",
     "{\"target\":\"pixels\"|\"mask\"|\"quickMask\"|{\"channel\":i}=Channels panel target (not pixels: "
     "the luminosity pasted into that mask or channel)}",
     has_clip, run_paste_in_place, true},
    {"edit.clear", "Clear", MENU_EDIT, 1, "Delete",
     "{} (a targeted channel or Quick Mask is cleared alone)", pc_engine_has_pixel_or_channel, run_clear,
     true},
    {"layer.new.layerViaCopy", "Layer via Copy", MENU_LAYER_NEW, 2, "Cmd+J", "{}", pc_engine_has_doc,
     run_layer_via_copy, true},
    {"layer.new.layerViaCut", "Layer via Cut", MENU_LAYER_NEW, 2, "Cmd+Shift+J", "{}", has_pixels,
     run_layer_via_cut, true},
    {"layer.mergeVisible", "Merge Visible", MENU_LAYER, 1, "Cmd+Shift+E", "{}", pc_engine_has_doc,
     run_merge_visible, true},
    {"edit.toggleLastState", "Toggle Last State", MENU_EDIT, 1, "Cmd+Alt+Z", "{}", pc_engine_has_doc,
     run_toggle_last_state, true},
};

const pc_engine_command *pc_engine_edit_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}

