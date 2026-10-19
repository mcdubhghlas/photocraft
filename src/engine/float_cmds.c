/* Floating selection (port of crates/engine/src/float_cmds.rs, E4): the selected pixels of a layer
 * cut out and moved as a piece, without touching the document until the piece is dropped.
 *
 * - select.float {dx,dy,copy,target} cuts the selected pixels of the active layer into a floating
 *   piece (the first time; copy leaves them and floats a duplicate) and moves it by whole pixels.
 * - select.drop drops the piece into its layer and moves the selection with it, as one history step.
 * - Any other command drops the piece first, except Undo, which puts it back (nothing was changed).
 *
 * This module also owns the float_before_command hook (its E1-safe default was deleted from hooks.c
 * when the module landed). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algo/resample.h"
#include "base/json.h"
#include "color/color.h"
#include "doc/doc.h"
#include "raster/surface.h"

#include "brush_cmds.h"
#include "engine_internal.h"
#include "fill_cmds.h"
#include "floating.h"

/* A layer split by its selection: the layer with the selected pixels cut out, and the cut pixels. */
typedef struct {
    pc_layer_id layer;
    pc_surface rest;  /* the layer with the selected pixels removed (or background-filled) */
    pc_surface piece; /* the cut pixels, cropped to the selection (with alpha) */
    bool mask;        /* the pieces are the layer's mask, not its pixels */
} cut_parts;

struct pc_engine_floating {
    pc_layer_id layer;
    int32_t ox, oy;
    uint64_t revision;
    cut_parts parts;
    pc_doc_arc *display;
};

void pc_engine_floating_free(struct pc_engine_floating *f)
{
    if (f == NULL) {
        return;
    }
    pc_doc_arc_release(f->display);
    pc_surface_fini(&f->parts.rest);
    pc_surface_fini(&f->parts.piece);
    free(f);
}

static pc_pixel_format with_alpha_of(pc_pixel_format f)
{
    f.alpha = true;
    return f;
}

/* Split `layer` of `doc` by its selection (CutParts::new). *out is initialised on success. */
static pc_status cut_parts_new(const pc_document *doc, pc_layer_id layer, bool copy,
                               const float background[4], bool mask, cut_parts *out, pc_error *err)
{
    if (!doc->has_selection) {
        return pc_engine_err_other(err, "no selection");
    }
    const pc_surface *sel = &doc->selection;
    const pc_layer *l = pc_document_layer(doc, layer);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, layer);
    }
    const pc_surface *surf;
    if (mask) {
        if (!l->has_mask) {
            return pc_engine_err_other(err, "the layer has no mask");
        }
        surf = &l->mask.surface;
    } else {
        if (l->content.kind != PC_LAYER_RASTER) {
            return pc_engine_err_other(err, "the layer has no pixels to move");
        }
        surf = &l->content.u.raster;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    pc_pixel_format wa = with_alpha_of(fmt);

    pc_surface full;
    pc_status rc;
    if (fmt.mode == wa.mode && fmt.sample == wa.sample && fmt.alpha == wa.alpha) {
        rc = pc_surface_clone(surf, &full, err);
    } else {
        rc = pc_surface_convert(surf, wa, &full, err);
    }
    if (rc != PC_OK) {
        return rc;
    }

    pc_surface rest;
    rc = mask ? pc_surface_clone(surf, &rest, err) : pc_surface_clone(&full, &rest, err);
    if (rc != PC_OK) {
        pc_surface_fini(&full);
        return rc;
    }
    pc_surface piece;
    pc_surface_init(&piece, wa);

    bool fill = !copy && (mask || pc_document_is_background(doc, layer));
    pc_rect clip = mask ? pc_document_bounds(doc) : pc_surface_content_bounds(surf);
    pc_rect b = pc_rect_intersect(pc_surface_content_bounds(sel), clip);

    if (!pc_rect_is_empty(b)) {
        size_t n = pc_pixel_format_channels(wa);
        size_t a = n - 1;
        size_t w = pc_rect_width(b), h = pc_rect_height(b);
        float *rp = NULL, *pp = NULL;
        size_t rlen = 0;
        rc = pc_surface_read_region(&full, b, &rp, &rlen, err);
        if (rc != PC_OK) {
            pc_surface_fini(&full);
            pc_surface_fini(&rest);
            pc_surface_fini(&piece);
            return rc;
        }
        pp = malloc(rlen * sizeof(float));
        if (pp == NULL && rlen > 0) {
            free(rp);
            pc_surface_fini(&full);
            pc_surface_fini(&rest);
            pc_surface_fini(&piece);
            return pc_fail(err, PC_ERR_NOMEM, "floating selection");
        }
        memcpy(pp, rp, rlen * sizeof(float));
        for (size_t i = 0; i < w * h; i++) {
            int32_t x = b.x0 + (int32_t)(i % w), y = b.y0 + (int32_t)(i / w);
            float k = pc_surface_sample_channel(sel, x, y, 0);
            float *p = &pp[i * n];
            float *r = &rp[i * n];
            if (k <= 0.0f) {
                for (size_t c = 0; c < n; c++) {
                    p[c] = 0.0f;
                }
            } else {
                p[a] *= k;
            }
            if (!copy && !fill) {
                r[a] *= 1.0f - k;
            }
        }
        if (!mask) {
            rc = pc_surface_write_region(&rest, b, rp, rlen, err);
        }
        if (rc == PC_OK && fill) {
            rc = pc_engine_try_fill_surface(&rest, b, background, sel, true, err);
        }
        if (rc == PC_OK) {
            pc_surface_prune(&rest);
            rc = pc_surface_write_region(&piece, b, pp, rlen, err);
        }
        if (rc == PC_OK) {
            pc_surface_prune(&piece);
        }
        free(rp);
        free(pp);
        if (rc != PC_OK) {
            pc_surface_fini(&full);
            pc_surface_fini(&rest);
            pc_surface_fini(&piece);
            return rc;
        }
    }
    pc_surface_fini(&full);
    out->layer = layer;
    out->rest = rest;
    out->piece = piece;
    out->mask = mask;
    return PC_OK;
}

/* Put the piece, moved by (dx, dy), over the cut-out layer (or mask) of `doc` (CutParts::place). */
static pc_status cut_parts_place(const cut_parts *cp, pc_document *doc, int32_t dx, int32_t dy, pc_error *err)
{
    pc_layer *l = pc_document_layer_mut(doc, cp->layer);
    if (l == NULL) {
        return pc_engine_err_no_layer(err, cp->layer);
    }
    pc_surface *surf;
    if (cp->mask) {
        if (!l->has_mask) {
            return pc_engine_err_other(err, "the layer has no mask");
        }
        surf = &l->mask.surface;
    } else {
        surf = pc_layer_surface_mut(l);
        if (surf == NULL) {
            return pc_engine_err_other(err, "the layer has no pixels to move");
        }
    }
    pc_surface out;
    pc_status rc = pc_surface_clone(&cp->rest, &out, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface moved;
    rc = pc_algo_translate_surface(&cp->piece, dx, dy, &moved, err);
    if (rc != PC_OK) {
        pc_surface_fini(&out);
        return rc;
    }
    if (cp->mask) {
        rc = pc_engine_composite_over(&out, &moved, pc_surface_content_bounds(&moved), NULL, err);
    } else {
        pc_engine_transform_composite_over(&out, &moved, err);
    }
    pc_surface_fini(&moved);
    if (rc != PC_OK) {
        pc_surface_fini(&out);
        return rc;
    }
    pc_surface_fini(surf);
    *surf = out;
    return PC_OK;
}

/* The floating piece of `st`, if it is still valid (CutParts::floating). */
static struct pc_engine_floating *current_floating(const pc_engine_doc_state *st)
{
    if (st->floating == NULL) {
        return NULL;
    }
    if (st->floating->revision != st->revision) {
        return NULL;
    }
    if (!pc_doc_arc_get(st->doc)->has_selection) {
        return NULL;
    }
    return st->floating;
}

static int32_t int_param(yyjson_val *p, const char *key)
{
    yyjson_val *v = pc_json_get(p, key);
    double d;
    if (v == NULL || !pc_json_as_f64(v, &d) || !isfinite(d)) {
        return 0;
    }
    d = roundf((float)d);
    if (d < -1e7) {
        d = -1e7;
    } else if (d > 1e7) {
        d = 1e7;
    }
    return (int32_t)d;
}

static int32_t sat_add(int32_t a, int32_t b)
{
    int64_t s = (int64_t)a + (int64_t)b;
    if (s < INT32_MIN) {
        s = INT32_MIN;
    } else if (s > INT32_MAX) {
        s = INT32_MAX;
    }
    return (int32_t)s;
}

/* ---- enabled ------------------------------------------------------------------------------ */

static pc_status can_float(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    if (current_floating(st) != NULL) {
        return PC_OK;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    if (!doc->has_selection) {
        return pc_fail(reason, PC_ERR_INVALID, "there is no selection");
    }
    if (!st->has_active_layer) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, st->active_layer);
    if (l == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind != PC_LAYER_RASTER) {
        return pc_fail(reason, PC_ERR_INVALID, "the active layer has no pixels to move");
    }
    pc_locks locks = pc_document_effective_locks(doc, l->id);
    bool locked = locks.all || (locks.position && !pc_document_is_background(doc, l->id));
    if (locked) {
        return pc_fail(reason, PC_ERR_INVALID, "layer \"%s\" is locked", pc_str_c(&l->name));
    }
    return PC_OK;
}

static pc_status can_drop(const pc_engine_session *s, pc_error *reason)
{
    if (pc_engine_session_active(s) == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document");
    }
    return PC_OK;
}

/* ---- run ---------------------------------------------------------------------------------- */

static pc_status run_float(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    int32_t dx = int_param(p, "dx"), dy = int_param(p, "dy");
    yyjson_val *cv = pc_json_get(p, "copy");
    bool copy = cv != NULL && yyjson_is_bool(cv) && yyjson_get_bool(cv);
    bool mask = pc_engine_is_mask_target(p);
    float background[4];
    memcpy(background, pc_engine_session_tools(s)->background, sizeof background);

    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return pc_engine_err_no_document(err);
    }
    if (current_floating(st) == NULL) {
        pc_engine_floating_free(st->floating);
        st->floating = NULL;
        if (!st->has_active_layer) {
            return pc_engine_err_other(err, "no active layer");
        }
        pc_layer_id layer = st->active_layer;
        cut_parts parts;
        pc_status rc = cut_parts_new(pc_doc_arc_get(st->doc), layer, copy, background, mask, &parts, err);
        if (rc != PC_OK) {
            return rc;
        }
        struct pc_engine_floating *f = malloc(sizeof *f);
        if (f == NULL) {
            pc_surface_fini(&parts.rest);
            pc_surface_fini(&parts.piece);
            return pc_fail(err, PC_ERR_NOMEM, "floating selection");
        }
        f->display = NULL;
        f->layer = layer;
        f->ox = 0;
        f->oy = 0;
        f->revision = st->revision;
        f->parts = parts;
        st->floating = f;
    }
    struct pc_engine_floating *f = st->floating;
    pc_doc_arc_release(f->display);
    f->display = NULL;
    f->ox = sat_add(f->ox, dx);
    f->oy = sat_add(f->oy, dy);

    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_uint(d, root, "layer", f->layer);
    yyjson_mut_val *off = yyjson_mut_arr(d);
    yyjson_mut_arr_add_int(d, off, f->ox);
    yyjson_mut_arr_add_int(d, off, f->oy);
    yyjson_mut_obj_add_val(d, root, "offset", off);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

struct drop_ctx {
    const cut_parts *parts;
    int32_t dx, dy;
};

static pc_status drop_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct drop_ctx *c = vctx;
    pc_status rc = cut_parts_place(c->parts, doc, c->dx, c->dy, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface moved;
        rc = pc_algo_translate_surface(&doc->selection, c->dx, c->dy, &moved, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_surface_fini(&doc->selection);
        doc->selection = moved;
    }
    return PC_OK;
}

bool pc_engine_floating_can_move(const pc_engine_session *session)
{
    const pc_engine_doc_state *state = session ? pc_engine_session_active(session) : NULL;
    if (!state || state->nselected > 1 || !state->has_active_layer ||
        !pc_doc_arc_get(state->doc)->has_selection)
        return false;
    const pc_layer *layer = pc_document_layer(pc_doc_arc_get(state->doc), state->active_layer);
    return layer && layer->visible && pc_engine_session_is_enabled(session, "select.float");
}
bool pc_engine_floating_offset(const pc_engine_session *session, int32_t offset[2])
{
    const pc_engine_doc_state *state = session ? pc_engine_session_active(session) : NULL;
    struct pc_engine_floating *f = state ? current_floating(state) : NULL;
    if (!f)
        return false;
    if (offset) {
        offset[0] = f->ox;
        offset[1] = f->oy;
    }
    return true;
}
pc_status pc_engine_floating_preview(const pc_engine_session *session, int32_t dx, int32_t dy, bool copy,
                                     bool mask, pc_doc_arc **out, pc_error *err)
{
    if (!session || !out)
        return pc_fail(err, PC_ERR_INVALID, "missing floating preview context");
    dx = dx < -10000000 ? -10000000 : dx > 10000000 ? 10000000 : dx;
    dy = dy < -10000000 ? -10000000 : dy > 10000000 ? 10000000 : dy;
    if (session->authorize) {
        char text[180];
        (void)snprintf(text, sizeof text, "{\"dx\":%d,\"dy\":%d,\"copy\":%s,\"target\":\"%s\"}", dx, dy,
                       copy ? "true" : "false", mask ? "mask" : "pixels");
        yyjson_doc *params = NULL;
        pc_status status = pc_json_parse(text, strlen(text), &params, err);
        if (status == PC_OK)
            status = session->authorize("select.float", yyjson_doc_get_root(params), err);
        yyjson_doc_free(params);
        if (status != PC_OK)
            return status;
    }
    pc_status status = can_float(session, err);
    if (status != PC_OK)
        return status;
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    struct pc_engine_floating *f = current_floating(state);
    cut_parts owned = {0};
    pc_cmyk_space *space = pc_engine_doc_cmyk_space(session);
    pc_cmyk_space *prior = space ? pc_cmyk_space_enter(space) : NULL;
    if (!f)
        status = cut_parts_new(pc_doc_arc_get(state->doc), state->active_layer, copy,
                               pc_engine_session_tools(session)->background, mask, &owned, err);
    pc_document doc;
    bool initialized = false;
    if (status == PC_OK) {
        status = pc_document_clone(pc_doc_arc_get(state->doc), &doc, err);
        initialized = status == PC_OK;
    }
    if (status == PC_OK) {
        struct drop_ctx context = {f ? &f->parts : &owned, f ? sat_add(f->ox, dx) : dx,
                                   f ? sat_add(f->oy, dy) : dy};
        bool has_active = state->has_active_layer;
        pc_layer_id active = state->active_layer;
        status = drop_edit(&doc, &has_active, &active, &context, err);
        if (status == PC_OK)
            status = pc_doc_arc_new(&doc, out, err);
        if (status == PC_OK)
            initialized = false;
    }
    if (initialized)
        pc_document_fini(&doc);
    pc_surface_fini(&owned.rest);
    pc_surface_fini(&owned.piece);
    if (space)
        pc_cmyk_space_leave(prior);
    pc_cmyk_space_release(space);
    return status;
}
pc_status pc_engine_floating_document(pc_engine_session *session, pc_doc_arc **out, pc_error *err)
{
    if (!session || !out)
        return pc_fail(err, PC_ERR_INVALID, "missing floating display context");
    *out = NULL;
    const pc_engine_doc_state *state = pc_engine_session_active(session);
    struct pc_engine_floating *f = state ? current_floating(state) : NULL;
    if (!f)
        return PC_OK;
    if (!f->display) {
        pc_status status = pc_engine_floating_preview(session, 0, 0, false, f->parts.mask, &f->display, err);
        if (status != PC_OK)
            return status;
    }
    *out = f->display;
    return PC_OK;
}
/* Drop the floating piece into its layer and move the selection with it (drop_floating). */
static pc_status drop_floating(pc_engine_session *s, pc_json_value *out, pc_error *err)
{
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_bool(d, root, "dropped", false);
        yyjson_mut_doc_set_root(d, root);
        return PC_OK;
    }
    bool dropped = true;
    int32_t dx = 0, dy = 0;
    if (current_floating(st) == NULL) {
        pc_engine_floating_free(st->floating);
        st->floating = NULL;
        dropped = false;
    } else {
        struct pc_engine_floating *owned = st->floating;
        st->floating = NULL;
        dx = owned->ox;
        dy = owned->oy;
        if (dx != 0 || dy != 0) {
            struct drop_ctx ctx = {&owned->parts, dx, dy};
            pc_status rc = pc_engine_session_edit(s, "Move", drop_edit, &ctx, err);
            if (rc != PC_OK) {
                st->floating = owned;
                return rc;
            }
        }
        pc_engine_floating_free(owned);
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "dropped", dropped);
    if (dropped) {
        yyjson_mut_val *off = yyjson_mut_arr(d);
        yyjson_mut_arr_add_int(d, off, dx);
        yyjson_mut_arr_add_int(d, off, dy);
        yyjson_mut_obj_add_val(d, root, "offset", off);
    }
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

static pc_status run_drop(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)p;
    return drop_floating(s, out, err);
}

/* ---- the before-command hook (moved from hooks.c, E4) ------------------------------------- */

static bool keeps_floating(const char *id)
{
    if (strcmp(id, "select.float") == 0 || strcmp(id, "select.drop") == 0) {
        return true;
    }
    static const char *const pre[] = {"view.", "window.", "help.", "prefs."};
    for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++) {
        size_t n = strlen(pre[i]);
        if (strncmp(id, pre[i], n) == 0) {
            return true;
        }
    }
    return false;
}

pc_status pc_engine_hook_float_before_command(pc_engine_session *s, const char *id, bool *handled,
                                              pc_json_value *out, pc_error *err)
{
    *handled = false;
    pc_engine_doc_state *st = pc_engine_session_active_mut(s);
    if (st == NULL) {
        return PC_OK;
    }
    if (current_floating(st) == NULL) {
        pc_engine_floating_free(st->floating);
        st->floating = NULL;
        return PC_OK;
    }
    if (strcmp(id, "edit.undo") == 0 || strcmp(id, "edit.stepBackward") == 0) {
        pc_engine_floating_free(st->floating);
        st->floating = NULL;
        yyjson_mut_doc *d = NULL;
        if (pc_engine_result_doc(out, &d, err) != PC_OK) {
            return PC_ERR_NOMEM;
        }
        yyjson_mut_val *root = yyjson_mut_obj(d);
        yyjson_mut_obj_add_strcpy(d, root, "floating", "returned");
        yyjson_mut_doc_set_root(d, root);
        *handled = true;
        return PC_OK;
    }
    if (keeps_floating(id)) {
        return PC_OK;
    }
    /* Drop the piece first; its own result is discarded (the real command runs next). */
    pc_json_value tmp = PC_JSON_VALUE_INIT;
    pc_status rc = drop_floating(s, &tmp, err);
    pc_json_value_fini(&tmp);
    return rc;
}

/* ---- specs -------------------------------------------------------------------------------- */

static const pc_engine_command SPECS[] = {
    {"select.float", "Float Selection", NULL, 0, NULL,
     "{\"dx\":px=0,\"dy\":px=0,\"copy\":bool=false,\"target\":\"pixels\"|\"mask\"=\"pixels\" (mask: "
     "float the selected part of the layer's mask, the hole takes the background colour)} → {layer, "
     "offset} (cuts the selected pixels of the active layer into a floating piece the first time, or "
     "with copy leaves them and floats a duplicate; on the Background the hole takes the background "
     "colour; then moves it by whole pixels; dropped by select.drop or any other command, put back by "
     "edit.undo)",
     can_float, run_float, true},
    {"select.drop", "Drop Floating Selection", NULL, 0, NULL,
     "{} → {dropped, offset} (drops the floating piece into its layer and moves the selection with "
     "it: one history step)",
     can_drop, run_drop, true},
};

const pc_engine_command *pc_engine_float_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
