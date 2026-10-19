/* Select menu and selection tools (port of crates/engine/src/selection_cmds.rs, E7): Magic Wand,
 * Modify, Grow, Similar, Lasso.
 *
 * Color Range compares samples through the document composite profile in CIE Lab D50. */
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/selection.h"
#include "algo/selection_blur.h"
#include "compose/compose.h"
#include "doc/doc.h"
#include "raster/surface.h"

#include "channel_cmds.h"
#include "selection_cmds.h"
#include "brush_cmds.h"
#include "cms/gamut.h"
#include "cms/transform.h"
#include "cms/builtin.h"
#include "base/mem.h"

/* ---- param helpers ----------------------------------------------------------------------- */

static float pf(yyjson_val *p, const char *k, float def)
{
    double v;
    yyjson_val *x = pc_json_get(p, k);
    if (x != NULL && pc_json_as_f64(x, &v)) {
        return (float)v;
    }
    return def;
}

static bool pb(yyjson_val *p, const char *k, bool def)
{
    bool v;
    yyjson_val *x = pc_json_get(p, k);
    if (x != NULL && pc_json_as_bool(x, &v)) {
        return v;
    }
    return def;
}

static pc_algo_selection_mode pmode(yyjson_val *p)
{
    size_t len = 0;
    yyjson_val *m = pc_json_get(p, "mode");
    const char *s = m != NULL ? pc_json_as_str(m, &len) : NULL;
    char buf[16] = {0};
    if (s != NULL && len < sizeof buf) {
        memcpy(buf, s, len);
        return pc_algo_selection_mode_parse(buf);
    }
    return PC_ALGO_SELECTION_REPLACE;
}

static pc_status has_doc(const pc_engine_session *s, pc_error *reason)
{
    return pc_engine_session_active(s) != NULL ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no document open");
}

static pc_status has_selection(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    return pc_doc_arc_get(d->doc)->has_selection ? PC_OK : pc_fail(reason, PC_ERR_INVALID, "no selection");
}

/* ---- combine helpers --------------------------------------------------------------------- */

struct combine_mask_ctx {
    const float *m;
    size_t n;
    pc_rect area;
    pc_algo_selection_mode mode;
    bool out_some;
};

static pc_status combine_mask_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct combine_mask_ctx *c = vctx;
    pc_surface out;
    bool some = false;
    pc_status rc = pc_algo_selection_combine(doc->has_selection ? &doc->selection : NULL, c->m, c->n, c->area,
                                             c->mode, &out, &some, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    if (some) {
        doc->selection = out;
        doc->has_selection = true;
    } else {
        pc_surface_fini(&out);
    }
    c->out_some = some;
    return PC_OK;
}

static pc_status set_mask_selection(pc_engine_session *s, const char *label, pc_rect area, const float *mask,
                                    size_t n, pc_algo_selection_mode mode, pc_json_value *out, pc_error *err)
{
    struct combine_mask_ctx c = {mask, n, area, mode, false};
    pc_status rc = pc_engine_session_edit(s, label, combine_mask_edit, &c, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "selected", c.out_some);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

struct combine_region_ctx {
    const pc_algo_region *region;
    pc_algo_selection_mode mode;
    bool out_some;
};

static pc_status combine_region_edit(pc_document *doc, bool *ha, pc_layer_id *a, void *vctx, pc_error *err)
{
    (void)ha;
    (void)a;
    struct combine_region_ctx *c = vctx;
    pc_surface out;
    bool some = false;
    pc_status rc = pc_algo_selection_combine_region(doc->has_selection ? &doc->selection : NULL, c->region,
                                                    c->mode, &out, &some, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (doc->has_selection) {
        pc_surface_fini(&doc->selection);
        doc->has_selection = false;
    }
    if (some) {
        doc->selection = out;
        doc->has_selection = true;
    } else {
        pc_surface_fini(&out);
    }
    c->out_some = some;
    return PC_OK;
}

/* ---- sampling ---------------------------------------------------------------------------- */

/* Straight RGBA8 of the active layer (all_layers false) or the composite over the canvas. *out
 * owned (free()). */
static pc_status sample_rgba8(pc_engine_session *s, bool all_layers, pc_rect *area_out, uint8_t (**out)[4],
                              size_t *len, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect area = pc_document_bounds(doc);
    *area_out = area;
    const pc_layer *l = d->has_active_layer ? pc_document_layer(doc, d->active_layer) : NULL;
    const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
    if (!all_layers && surf != NULL) {
        return pc_algo_selection_rgba8_image(surf, area, out, len, err);
    }
    pc_compose_buffer buf;
    pc_status rc = pc_compose_render(doc, area, &buf, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t npx = (size_t)pc_rect_width(area) * (size_t)pc_rect_height(area);
    uint8_t (*img)[4] = malloc(npx ? npx * sizeof(*img) : sizeof(*img));
    if (img == NULL) {
        pc_compose_buffer_fini(&buf);
        return pc_fail(err, PC_ERR_NOMEM, "sample");
    }
    for (size_t i = 0; i < npx; i++) {
        for (int c = 0; c < 4; c++) {
            float v = (i < buf.len) ? buf.px[i][c] : 0.0f;
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            img[i][c] = (uint8_t)(v * 255.0f + 0.5f);
        }
    }
    pc_compose_buffer_fini(&buf);
    *out = img;
    *len = npx;
    return PC_OK;
}

/* Public wrapper over sample_rgba8 for the Paint Bucket / Magic Eraser (paint_cmds, eraser_cmds).
 * Straight RGBA8 of the active layer (all_layers false) or the composite; *out owned (free()). */
pc_status pc_engine_sample_rgba8(pc_engine_session *s, bool all_layers, pc_rect *area_out, uint8_t (**out)[4],
                                 size_t *len, pc_error *err)
{
    return sample_rgba8(s, all_layers, area_out, out, len, err);
}

/* RGBA float pixels of the active layer (all_layers false) or composite. *out owned (free()). */
static pc_status sample_pixels(pc_engine_session *s, bool all_layers, pc_rect *area_out, float (**out)[4],
                               size_t *len, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect area = pc_document_bounds(doc);
    *area_out = area;
    size_t npx = (size_t)pc_rect_width(area) * (size_t)pc_rect_height(area);
    const pc_layer *l = d->has_active_layer ? pc_document_layer(doc, d->active_layer) : NULL;
    const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
    if (!all_layers && surf != NULL) {
        float (*px)[4] = malloc(npx ? npx * sizeof(*px) : sizeof(*px));
        if (px == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "sample");
        }
        pc_status rc = pc_surface_read_rgba_into(surf, area, px, npx, err);
        if (rc != PC_OK) {
            free(px);
            return rc;
        }
        *out = px;
        *len = npx;
        return PC_OK;
    }
    pc_compose_buffer buf;
    pc_status rc = pc_compose_render(doc, area, &buf, err);
    if (rc != PC_OK) {
        return rc;
    }
    *out = buf.px; /* take ownership */
    *len = buf.len;
    buf.px = NULL;
    buf.len = 0;
    pc_compose_buffer_fini(&buf);
    return PC_OK;
}

/* The grayscale plane the canvas actually shows, as RGBA8, when that isn't the colour composite
 * (a gray mask view, or with all colour channels off: visible alpha / Quick Mask / layer mask).
 * *out owned (free()); returns false (nothing extra shown) with *out NULL. */
static bool shown_gray(pc_engine_session *s, pc_rect *area_out, uint8_t (**out)[4], size_t *len)
{
    *out = NULL;
    const pc_engine_doc_state *st = pc_engine_session_active(s);
    if (st == NULL) {
        return false;
    }
    const pc_document *doc = pc_doc_arc_get(st->doc);
    pc_rect area = pc_document_bounds(doc);
    *area_out = area;
    size_t colors = pc_engine_color_count(doc);
    bool colors_off = pc_engine_channel_visible_colors(st, colors) == 0;

    /* The active layer's shown mask (E5 DocState fields). */
    const pc_surface *mask_surf = NULL;
    int mask_mode = -1;
    if (st->has_layer_mask_view && st->has_active_layer && st->active_layer == st->layer_mask_view_layer) {
        const pc_layer *l = pc_document_layer(doc, st->active_layer);
        if (l != NULL && l->has_mask) {
            mask_surf = &l->mask.surface;
            mask_mode = st->layer_mask_view_mode;
        }
    }

    const pc_surface *surf = NULL;
    if (mask_surf != NULL && mask_mode == PC_ENGINE_MASK_VIEW_GRAY) {
        surf = mask_surf;
    } else if (colors_off) {
        bool found = false;
        for (size_t i = 0; i < doc->nchannels; i++) {
            if (pc_engine_channel_alpha_shown(st, i)) {
                surf = &doc->channels[i].surface;
                found = true;
                break;
            }
        }
        if (!found && doc->has_quick_mask && !pc_engine_channel_quick_mask_hidden(st)) {
            surf = &doc->quick_mask.surface;
            found = true;
        }
        if (!found && mask_surf != NULL) {
            surf = mask_surf;
            found = true;
        }
        if (!found && st->has_active_layer) {
            const pc_layer *l = pc_document_layer(doc, st->active_layer);
            if (l != NULL && l->has_mask) {
                surf = &l->mask.surface;
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    } else {
        return false;
    }
    if (pc_algo_selection_rgba8_image(surf, area, out, len, NULL) != PC_OK) {
        *out = NULL;
        return false;
    }
    return true;
}

/* ---- Magic Wand -------------------------------------------------------------------------- */

static pc_status run_magic_wand(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    int32_t x = (int32_t)floorf(pf(p, "x", 0.0f));
    int32_t y = (int32_t)floorf(pf(p, "y", 0.0f));
    pc_rect area = PC_RECT_EMPTY;
    uint8_t (*img)[4] = NULL;
    size_t len = 0;
    if (!shown_gray(s, &area, &img, &len)) {
        pc_status rc = sample_rgba8(s, pb(p, "sampleAllLayers", false), &area, &img, &len, err);
        if (rc != PC_OK) {
            return rc;
        }
    }
    pc_algo_region region;
    pc_status rc =
        pc_algo_selection_wand_region((const uint8_t (*)[4])img, len, area, x, y, pf(p, "tolerance", 32.0f),
                                      pb(p, "contiguous", true), pb(p, "antiAlias", true), &region, err);
    free(img);
    if (rc != PC_OK) {
        return rc;
    }
    struct combine_region_ctx c = {&region, pmode(p), false};
    rc = pc_engine_session_edit(s, "Magic Wand", combine_region_edit, &c, err);
    pc_algo_region_fini(&region);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_mut_doc *d = NULL;
    if (pc_engine_result_doc(out, &d, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, root, "selected", c.out_some);
    yyjson_mut_doc_set_root(d, root);
    return PC_OK;
}

/* ---- Modify ------------------------------------------------------------------------------ */

#define MAX_MODIFY_PIXELS ((size_t)1 << 30)

static bool touches_edge(const float *m, size_t w, size_t h)
{
    for (size_t x = 0; x < w; x++) {
        if ((h > 0 && m[x] > 0.0f) || (h > 0 && m[(h - 1) * w + x] > 0.0f)) {
            return true;
        }
    }
    for (size_t y = 0; y < h; y++) {
        if ((w > 0 && m[y * w] > 0.0f) || (w > 0 && m[y * w + (w - 1)] > 0.0f)) {
            return true;
        }
    }
    return false;
}

/* m (w*h) with a pad-pixel margin; unselected when outside_empty, else the nearest edge repeated. */
static float *pad_mask(const float *m, size_t w, size_t h, size_t pad, bool outside_empty)
{
    size_t pw = w + 2 * pad, ph = h + 2 * pad;
    float *out = calloc((pw * ph) != 0 ? pw * ph : 1, sizeof(float));
    if (out == NULL || w == 0 || h == 0) {
        return out;
    }
    for (size_t py = 0; py < ph; py++) {
        if (outside_empty && !(py >= pad && py < pad + h)) {
            continue;
        }
        size_t y = py < pad ? 0 : (py - pad >= h ? h - 1 : py - pad);
        const float *src = &m[y * w];
        float *row = &out[py * pw];
        memcpy(&row[pad], src, w * sizeof(float));
        if (!outside_empty) {
            float left = w > 0 ? src[0] : 0.0f;
            float right = w > 0 ? src[w - 1] : 0.0f;
            for (size_t i = 0; i < pad; i++) {
                row[i] = left;
                row[pad + w + i] = right;
            }
        }
    }
    return out;
}

static float *crop_mask(const float *m, size_t pw, size_t pad, size_t w, size_t h)
{
    float *out = malloc((w * h) != 0 ? w * h * sizeof(float) : 1);
    if (out == NULL) {
        return NULL;
    }
    for (size_t y = 0; y < h; y++) {
        memcpy(&out[y * w], &m[(y + pad) * pw + pad], w * sizeof(float));
    }
    return out;
}

static pc_status modify(pc_engine_session *s, yyjson_val *p, const char *op, pc_json_value *out,
                        pc_error *err)
{
    float mn, mx;
    if (strcmp(op, "border") == 0) {
        mn = 1.0f;
        mx = 200.0f;
    } else if (strcmp(op, "feather") == 0) {
        mn = 0.1f;
        mx = 1000.0f;
    } else {
        mn = 1.0f;
        mx = 500.0f;
    }
    float r = pf(p, "radius", 1.0f);
    char cmd[32];
    (void)snprintf(cmd, sizeof cmd, "select.modify.%s", op);
    if (!(r >= mn && r <= mx)) {
        return pc_engine_err_bad_params(err, cmd, "radius must be a number in %g..%g", (double)mn,
                                        (double)mx);
    }
    bool at_bounds = strcmp(op, "border") == 0 || pb(p, "applyAtCanvasBounds", false);
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_rect area = pc_document_bounds(doc);
    size_t w = pc_rect_width(area), h = pc_rect_height(area);
    float *m = NULL;
    size_t mn_len = 0;
    pc_status rc = pc_algo_selection_mask_from_surface(doc->has_selection ? &doc->selection : NULL, area, &m,
                                                       &mn_len, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t npx = w * h;
    if (m == NULL) {
        m = calloc(npx ? npx : 1, sizeof(float));
        if (m == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "modify");
        }
    }
    float reach = strcmp(op, "feather") == 0 ? r * 3.0f : r;
    size_t pad = touches_edge(m, w, h) ? (size_t)ceilf(reach) + 2 : 0;
    /* overflow-checked padded dimensions */
    size_t pw = w, ph = h;
    bool overflow = false;
    if (pad != 0) {
        if (pad > (SIZE_MAX - w) / 2 || pad > (SIZE_MAX - h) / 2) {
            overflow = true;
        } else {
            pw = w + 2 * pad;
            ph = h + 2 * pad;
        }
    }
    if (overflow || (ph != 0 && pw > MAX_MODIFY_PIXELS / ph)) {
        free(m);
        return pc_engine_err_bad_params(err, cmd, "the selection is too large to modify");
    }
    float *padded = pad == 0 ? m : pad_mask(m, w, h, pad, at_bounds);
    if (pad != 0) {
        free(m);
        if (padded == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "modify");
        }
    }
    float *res = NULL;
    if (strcmp(op, "expand") == 0) {
        rc = pc_algo_selection_expand(padded, pw, ph, r, &res, err);
    } else if (strcmp(op, "contract") == 0) {
        rc = pc_algo_selection_contract(padded, pw, ph, r, &res, err);
    } else if (strcmp(op, "border") == 0) {
        rc = pc_algo_selection_border(padded, pw, ph, r, &res, err);
    } else if (strcmp(op, "smooth") == 0) {
        rc = pc_algo_selection_smooth(padded, pw * ph, pw, ph, r, &res, err);
    } else {
        rc = pc_algo_selection_feather(padded, pw * ph, pw, ph, r, &res, err);
    }
    free(padded);
    if (rc != PC_OK) {
        return rc;
    }
    float *final = res;
    if (pad != 0) {
        final = crop_mask(res, pw, pad, w, h);
        free(res);
        if (final == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "modify");
        }
    }
    const char *label = "Feather Selection";
    if (strcmp(op, "expand") == 0) {
        label = "Expand Selection";
    } else if (strcmp(op, "contract") == 0) {
        label = "Contract Selection";
    } else if (strcmp(op, "border") == 0) {
        label = "Border Selection";
    } else if (strcmp(op, "smooth") == 0) {
        label = "Smooth Selection";
    }
    rc = set_mask_selection(s, label, area, final, npx, PC_ALGO_SELECTION_REPLACE, out, err);
    free(final);
    return rc;
}

#define MODIFY_FN(name, opstr)                                                                               \
    static pc_status run_modify_##name(pc_engine_session *s, yyjson_val *p, pc_json_value *out,              \
                                       pc_error *err)                                                        \
    {                                                                                                        \
        return modify(s, p, opstr, out, err);                                                                \
    }
MODIFY_FN(border, "border")
MODIFY_FN(smooth, "smooth")
MODIFY_FN(expand, "expand")
MODIFY_FN(contract, "contract")
MODIFY_FN(feather, "feather")

/* ---- Grow / Similar ---------------------------------------------------------------------- */

static pc_status grow_similar(pc_engine_session *s, yyjson_val *p, bool contiguous, pc_json_value *out,
                              pc_error *err)
{
    float tol = pf(p, "tolerance", 32.0f) / 255.0f;
    pc_rect area = PC_RECT_EMPTY;
    float (*px)[4] = NULL;
    size_t npx = 0;
    pc_status rc = sample_pixels(s, pb(p, "sampleAllLayers", false), &area, &px, &npx, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_document *doc = pc_doc_arc_get(pc_engine_session_active(s)->doc);
    float *m = NULL;
    size_t mn = 0;
    rc = pc_algo_selection_mask_from_surface(doc->has_selection ? &doc->selection : NULL, area, &m, &mn, err);
    if (rc != PC_OK) {
        free(px);
        return rc;
    }
    size_t w = pc_rect_width(area), h = pc_rect_height(area);
    if (m == NULL) {
        m = calloc(npx ? npx : 1, sizeof(float));
        if (m == NULL) {
            free(px);
            return pc_fail(err, PC_ERR_NOMEM, "grow");
        }
    }
    float lo[4] = {FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX};
    float hi[4] = {-FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = 0; i < npx; i++) {
        if (m[i] >= 0.5f) {
            for (int c = 0; c < 4; c++) {
                if (px[i][c] < lo[c]) {
                    lo[c] = px[i][c];
                }
                if (px[i][c] > hi[c]) {
                    hi[c] = px[i][c];
                }
            }
        }
    }
    if (lo[0] > hi[0]) {
        free(px);
        free(m);
        return pc_engine_err_other(err, "selection is empty");
    }
    float *res = malloc(npx ? npx * sizeof(float) : 1);
    if (res == NULL) {
        free(px);
        free(m);
        return pc_fail(err, PC_ERR_NOMEM, "grow");
    }
    for (size_t i = 0; i < npx; i++) {
        res[i] = m[i] >= 0.5f ? 1.0f : 0.0f;
    }
    free(m);
    if (contiguous) {
        size_t *stack = malloc(npx ? npx * sizeof(size_t) : 1);
        if (stack == NULL) {
            free(px);
            free(res);
            return pc_fail(err, PC_ERR_NOMEM, "grow");
        }
        size_t sp = 0;
        for (size_t i = 0; i < npx; i++) {
            if (res[i] > 0.0f) {
                stack[sp++] = i;
            }
        }
        while (sp > 0) {
            size_t i = stack[--sp];
            size_t x = i % (w ? w : 1), y = i / (w ? w : 1);
            size_t nb[4];
            int nn = 0;
            if (x > 0) {
                nb[nn++] = i - 1;
            }
            if (x + 1 < w) {
                nb[nn++] = i + 1;
            }
            if (y > 0) {
                nb[nn++] = i - w;
            }
            if (y + 1 < h) {
                nb[nn++] = i + w;
            }
            for (int k = 0; k < nn; k++) {
                size_t j = nb[k];
                if (res[j] == 0.0f) {
                    bool inside = true;
                    for (int c = 0; c < 4; c++) {
                        if (px[j][c] < lo[c] - tol - 1e-6f || px[j][c] > hi[c] + tol + 1e-6f) {
                            inside = false;
                            break;
                        }
                    }
                    if (inside) {
                        res[j] = 1.0f;
                        stack[sp++] = j;
                    }
                }
            }
        }
        free(stack);
    } else {
        for (size_t i = 0; i < npx; i++) {
            bool inside = true;
            for (int c = 0; c < 4; c++) {
                if (px[i][c] < lo[c] - tol - 1e-6f || px[i][c] > hi[c] + tol + 1e-6f) {
                    inside = false;
                    break;
                }
            }
            if (inside) {
                res[i] = 1.0f;
            }
        }
    }
    free(px);
    pc_status rc2 = set_mask_selection(s, contiguous ? "Grow" : "Similar", area, res, npx,
                                       PC_ALGO_SELECTION_REPLACE, out, err);
    free(res);
    return rc2;
}

static pc_status run_grow(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return grow_similar(s, p, true, out, err);
}
static pc_status run_similar(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    return grow_similar(s, p, false, out, err);
}

/* ---- Lasso / polygon --------------------------------------------------------------------- */

pc_status pc_engine_polygon_selection(pc_engine_session *s, const char *cmd, const char *label,
                                      const float (*pts)[2], size_t npts, yyjson_val *p, pc_json_value *out,
                                      pc_error *err)
{
    float feather = pf(p, "feather", 0.0f);
    if (!(feather >= 0.0f && feather <= 1000.0f)) {
        return pc_engine_err_bad_params(err, cmd, "feather must be a number in 0..1000");
    }
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_rect area = pc_document_bounds(pc_doc_arc_get(d->doc));
    float *mask = NULL;
    size_t mlen = 0;
    pc_status rc = pc_algo_selection_polygon(pts, npts, area, pb(p, "antiAlias", true), &mask, &mlen, err);
    if (rc != PC_OK) {
        return rc;
    }
    size_t w = pc_rect_width(area), h = pc_rect_height(area);
    size_t npx = w * h;
    if (mask == NULL) {
        mask = calloc(npx ? npx : 1, sizeof(float));
        if (mask == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "lasso");
        }
        mlen = npx;
    }
    if (feather > 0.0f) {
        float *fe = NULL;
        rc = pc_algo_selection_feather(mask, mlen, w, h, feather, &fe, err);
        free(mask);
        if (rc != PC_OK) {
            return rc;
        }
        mask = fe;
    }
    rc = set_mask_selection(s, label, area, mask, npx, pmode(p), out, err);
    free(mask);
    return rc;
}

static pc_status run_lasso(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    yyjson_val *arr = pc_json_get(p, "points");
    size_t npts = 0;
    float (*pts)[2] = NULL;
    if (arr != NULL && yyjson_is_arr(arr)) {
        size_t cap = yyjson_arr_size(arr);
        pts = malloc(cap ? cap * sizeof(*pts) : sizeof(*pts));
        if (pts == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "lasso");
        }
        size_t i, max;
        yyjson_val *v;
        yyjson_arr_foreach(arr, i, max, v)
        {
            double x, y;
            yyjson_val *vx = v != NULL && yyjson_is_arr(v) ? yyjson_arr_get(v, 0) : NULL;
            yyjson_val *vy = v != NULL && yyjson_is_arr(v) ? yyjson_arr_get(v, 1) : NULL;
            if (vx != NULL && vy != NULL && pc_json_as_f64(vx, &x) && pc_json_as_f64(vy, &y)) {
                pts[npts][0] = (float)x;
                pts[npts][1] = (float)y;
                npts++;
            }
        }
    }
    if (npts < 3) {
        free(pts);
        return pc_engine_err_bad_params(err, "select.lasso", "needs at least 3 points");
    }
    pc_status rc =
        pc_engine_polygon_selection(s, "select.lasso", "Lasso", (const float (*)[2])pts, npts, p, out, err);
    free(pts);
    return rc;
}

#include "color_range.inc"

/* ---- registry ---------------------------------------------------------------------------- */

static const pc_engine_command SPECS[] = {
    {"select.colorRange", "Color Range…", (const char *const[]){"Select"}, 1, NULL,
     "{\"select\":\"sampledColors|reds|yellows|greens|cyans|blues|magentas|highlights|midtones|shadows|"
     "outOfGamut\",\"fuzziness\":0..200=40,\"localized\":bool=false,\"range\":0..100=100,\"color\":color,"
     "\"colors\":json,\"points\":json,\"subtractPoints\":json,\"order\":json,\"tonalRange\":json,\"invert\":"
     "bool=false,\"sampleAllLayers\":bool=true,\"mode\":\"replace|add|subtract|intersect\"}",
     has_doc, run_color_range, true},
    {"select.magicWand", "Magic Wand", NULL, 0, NULL,
     "{\"x\":px,\"y\":px,\"tolerance\":0..255=32,\"contiguous\":bool=true,\"antiAlias\":bool=true,"
     "\"sampleAllLayers\":bool=false,\"mode\":\"replace|add|subtract|intersect\"=\"replace\"}",
     has_doc, run_magic_wand, true},
    {"select.modify.border", "Border…", (const char *const[]){"Select", "Modify"}, 2, NULL,
     "{\"radius\":1..200=1}", has_selection, run_modify_border, true},
    {"select.modify.smooth", "Smooth…", (const char *const[]){"Select", "Modify"}, 2, NULL,
     "{\"radius\":1..500=1,\"applyAtCanvasBounds\":bool=false}", has_selection, run_modify_smooth, true},
    {"select.modify.expand", "Expand…", (const char *const[]){"Select", "Modify"}, 2, NULL,
     "{\"radius\":1..500=1,\"applyAtCanvasBounds\":bool=false}", has_selection, run_modify_expand, true},
    {"select.modify.contract", "Contract…", (const char *const[]){"Select", "Modify"}, 2, NULL,
     "{\"radius\":1..500=1,\"applyAtCanvasBounds\":bool=false}", has_selection, run_modify_contract, true},
    {"select.modify.feather", "Feather…", (const char *const[]){"Select", "Modify"}, 2, NULL,
     "{\"radius\":0.1..1000=1,\"applyAtCanvasBounds\":bool=false}", has_selection, run_modify_feather, true},
    {"select.grow", "Grow", (const char *const[]){"Select"}, 1, NULL,
     "{\"tolerance\":0..255=32,\"sampleAllLayers\":bool=false}", has_selection, run_grow, true},
    {"select.similar", "Similar", (const char *const[]){"Select"}, 1, NULL,
     "{\"tolerance\":0..255=32,\"sampleAllLayers\":bool=false}", has_selection, run_similar, true},
    {"select.lasso", "Lasso", NULL, 0, NULL,
     "{\"points\":[[x,y],…],\"mode\":\"replace|add|subtract|intersect\"=\"replace\",\"antiAlias\":"
     "bool=true,\"feather\":px=0}",
     has_doc, run_lasso, true},
};

const pc_engine_command *pc_engine_selection_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
