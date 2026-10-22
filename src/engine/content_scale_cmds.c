/* Edit > Content-Aware Scale: edit.contentAwareScale (port of the content_aware_scale command in
 * crates/engine/src/edit_menu_cmds.rs).
 *
 * The region (the selection's bounds, else the active pixel layer's content) is resized to the
 * requested width/height by seam carving (photocraft_algo::seam), with an alpha channel and/or skin
 * tones steering the seams away from protected pixels, blended with a plain bilinear resize by the
 * Amount, then written back at the requested position in one history step.
 *
 * Runs synchronously, as every PhotoCraft C command does today (the background-job path is a later
 * refinement). Never crashes (REWRITE_PLAN.md rule 9): every param is validated, every size bounded
 * before allocating, and an oversized result is refused (not clipped). */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "algo/seam.h"
#include "base/json.h"
#include "base/str.h"
#include "color/color.h"
#include "doc/doc.h"
#include "raster/surface.h"

#include "allocation.h"
#include "content_scale_cmds.h"
#include "engine_internal.h"

#define CMD "edit.contentAwareScale"

/* Reach of x/y: the largest document side. */
#define CAS_POSITION_LIMIT 300000.0
/* Largest output side (as Image Size / Crop). */
#define CAS_MAX_SIDE 60000.0
/* Largest output, total, to keep a hostile request from exhausting memory. */
#define CAS_MAX_PIXELS ((uint64_t)1 << 28)

static const char *const MENU_EDIT[] = {"Edit"};

static const pc_document *cas_doc(const pc_engine_session *s)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    return d != NULL ? pc_doc_arc_get(d->doc) : NULL;
}

/* The active raster layer id, or an error naming why not. */
static pc_status cas_layer(const pc_engine_session *s, pc_layer_id *out, pc_error *err)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    if (!d->has_active_layer || doc == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no active layer");
    }
    const pc_layer *l = pc_document_layer(doc, d->active_layer);
    if (l == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no active layer");
    }
    if (l->content.kind != PC_LAYER_RASTER) {
        return pc_fail(err, PC_ERR_INVALID,
                       "Content-Aware Scale needs a pixel layer (active layer is %s %s layer)",
                       pc_layer_content_article(&l->content), pc_layer_content_kind_name(&l->content));
    }
    *out = d->active_layer;
    return PC_OK;
}

static pc_status cas_enabled(const pc_engine_session *s, pc_error *reason)
{
    pc_layer_id id = 0;
    return cas_layer(s, &id, reason);
}

/* A new side from `k` (px) or `pct` (percent of `cur`); 1..CAS_MAX_SIDE. */
static pc_status cas_dim(yyjson_val *p, const char *k, const char *pct, uint32_t cur, uint32_t *out,
                         pc_error *err)
{
    double v = (double)cur;
    yyjson_val *jk = p != NULL ? pc_json_get(p, k) : NULL;
    double px = 0.0, pc = 0.0;
    if (jk != NULL && pc_json_as_f64(jk, &px)) {
        v = px;
    } else {
        yyjson_val *jp = p != NULL ? pc_json_get(p, pct) : NULL;
        if (jp != NULL && pc_json_as_f64(jp, &pc)) {
            v = (double)cur * pc / 100.0;
        }
    }
    if (!(isfinite(v) && v >= 1.0 && v <= CAS_MAX_SIDE)) {
        return pc_engine_err_bad_params(err, CMD, "`%s` must be 1..%.0f px", k, CAS_MAX_SIDE);
    }
    *out = (uint32_t)(v + 0.5);
    return PC_OK;
}

/* A position from `k`; default `cur`; within +-CAS_POSITION_LIMIT. */
static pc_status cas_at(yyjson_val *p, const char *k, int32_t cur, int32_t *out, pc_error *err)
{
    yyjson_val *v = p != NULL ? pc_json_get(p, k) : NULL;
    if (v == NULL || yyjson_is_null(v)) {
        *out = cur;
        return PC_OK;
    }
    double x = 0.0;
    if (!pc_json_as_f64(v, &x) || !isfinite(x) || x < -CAS_POSITION_LIMIT || x > CAS_POSITION_LIMIT) {
        return pc_engine_err_bad_params(err, CMD, "`%s` must be a number of px within +-%.0f", k,
                                        CAS_POSITION_LIMIT);
    }
    *out = (int32_t)round(x);
    return PC_OK;
}

/* Resolve the `protect` alpha channel's surface (index or name), or NULL for none. */
static pc_status cas_protect(const pc_document *doc, yyjson_val *p, const pc_surface **out, pc_error *err)
{
    *out = NULL;
    yyjson_val *v = p != NULL ? pc_json_get(p, "protect") : NULL;
    if (v == NULL || yyjson_is_null(v)) {
        return PC_OK;
    }
    size_t slen = 0;
    const char *str = pc_json_as_str(v, &slen);
    if (str != NULL && slen == 4 && memcmp(str, "none", 4) == 0) {
        return PC_OK;
    }
    uint64_t idx = 0;
    if (pc_json_as_u64(v, &idx)) {
        if (idx < doc->nchannels) {
            *out = &doc->channels[idx].surface;
            return PC_OK;
        }
    } else if (str != NULL) {
        for (size_t i = 0; i < doc->nchannels; i++) {
            if (pc_str_eq_mem(&doc->channels[i].name, str, slen)) {
                *out = &doc->channels[i].surface;
                return PC_OK;
            }
        }
    }
    return pc_engine_err_bad_params(err, CMD,
                                    "`protect` must name an alpha channel (index or name) or \"none\"");
}

struct cas_ctx {
    pc_layer_id id;
    pc_rect region;
    pc_rect dst;
    float *pixels; /* nw*nh*n, owned */
    size_t len;
};

static pc_status cas_edit(pc_document *doc, bool *ha, pc_layer_id *active, void *vctx, pc_error *err)
{
    (void)ha;
    (void)active;
    struct cas_ctx *c = vctx;
    pc_layer *l = pc_document_layer_mut(doc, c->id);
    if (l == NULL || l->content.kind != PC_LAYER_RASTER) {
        return pc_engine_err_no_layer(err, c->id);
    }
    pc_surface *surf = &l->content.u.raster;
    pc_status rc = pc_engine_try_clear_surface(surf, c->region, NULL, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = pc_surface_write_region(surf, c->dst, c->pixels, c->len, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface_prune(surf);
    return PC_OK;
}

static pc_status run_cas(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const pc_document *doc = cas_doc(s);
    if (doc == NULL) {
        return pc_engine_err_no_document(err);
    }
    pc_layer_id id = 0;
    pc_status rc = cas_layer(s, &id, err);
    if (rc != PC_OK) {
        return rc;
    }
    const pc_layer *l = pc_document_layer(doc, id);
    const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
    if (surf == NULL) {
        return pc_engine_err_no_layer(err, id);
    }
    pc_rect region;
    if (doc->has_selection) {
        region = pc_rect_intersect(pc_surface_content_bounds(&doc->selection), pc_document_bounds(doc));
    } else {
        region = pc_surface_content_bounds(surf);
    }
    if (pc_rect_is_empty(region)) {
        return pc_engine_err_other(err, "nothing to scale: the layer is empty");
    }
    uint32_t w = pc_rect_width(region), h = pc_rect_height(region);
    uint32_t nw = 0, nh = 0;
    rc = cas_dim(p, "width", "scaleX", w, &nw, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = cas_dim(p, "height", "scaleY", h, &nh, err);
    if (rc != PC_OK) {
        return rc;
    }
    if ((uint64_t)nw * (uint64_t)nh > CAS_MAX_PIXELS) {
        return pc_engine_err_bad_params(err, CMD, "%u x %u px is too large to scale", nw, nh);
    }
    int32_t dx = 0, dy = 0;
    rc = cas_at(p, "x", region.x0, &dx, err);
    if (rc != PC_OK) {
        return rc;
    }
    rc = cas_at(p, "y", region.y0, &dy, err);
    if (rc != PC_OK) {
        return rc;
    }
    pc_rect dst = pc_rect_from_xywh(dx, dy, nw, nh);
    double amount = 1.0;
    yyjson_val *ja = p != NULL ? pc_json_get(p, "amount") : NULL;
    if (ja != NULL && pc_json_as_f64(ja, &amount)) {
        amount = isfinite(amount) ? amount : 100.0;
        amount = (amount < 0.0 ? 0.0 : (amount > 100.0 ? 100.0 : amount)) / 100.0;
    }
    bool skin = false;
    yyjson_val *js = p != NULL ? pc_json_get(p, "protectSkinTones") : NULL;
    if (js != NULL) {
        (void)pc_json_as_bool(js, &skin);
    }
    const pc_surface *protect = NULL;
    rc = cas_protect(doc, p, &protect, err);
    if (rc != PC_OK) {
        return rc;
    }

    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_pixel_format_channels(fmt);
    if (n == 0) {
        return pc_engine_err_other(err, "the layer has no channels");
    }
    float *img = NULL;
    size_t ilen = 0;
    rc = pc_engine_alloc_read_region(surf, region, "content-aware scale", &img, &ilen, err);
    if (rc != PC_OK) {
        return rc;
    }

    /* Protection: an alpha channel and/or skin tones (0..1 per pixel). */
    float *pm = NULL;
    size_t npx = (size_t)w * (size_t)h;
    if (protect != NULL || skin) {
        if (pc_engine_alloc_filled(npx, sizeof(float), "content-aware scale", (void **)&pm, err) != PC_OK) {
            free(img);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        if (protect != NULL) {
            for (size_t i = 0; i < npx; i++) {
                int32_t x = region.x0 + (int32_t)(i % w), y = region.y0 + (int32_t)(i / w);
                pm[i] = pc_surface_sample_channel(protect, x, y, 0);
            }
        }
        if (skin) {
            float *sk = NULL;
            pc_status srk = pc_algo_seam_skin_mask(w, h, n, img, ilen, &sk, err);
            if (srk != PC_OK) {
                free(img);
                free(pm);
                return srk;
            }
            for (size_t i = 0; i < npx; i++) {
                if (sk[i] > pm[i]) {
                    pm[i] = sk[i];
                }
            }
            free(sk);
        }
    }

    /* Seam-carve and/or plain resize, blended by amount. */
    float *carved = NULL, *plain = NULL, *result = NULL;
    size_t clen = 0, plen = 0;
    if (amount > 0.0) {
        rc = pc_algo_seam_carve(w, h, n, img, ilen, pm, nw, nh, NULL, &carved, &clen, err);
        if (rc != PC_OK) {
            free(img);
            free(pm);
            return rc;
        }
    }
    if (amount < 1.0) {
        rc = pc_algo_seam_resize_bilinear(w, h, n, img, ilen, nw, nh, &plain, err);
        plen = (size_t)nw * (size_t)nh * n;
        if (rc != PC_OK) {
            free(img);
            free(pm);
            free(carved);
            return rc;
        }
    }
    free(img);
    free(pm);

    size_t want = (size_t)nw * (size_t)nh * n;
    if (carved != NULL && plain == NULL) {
        result = carved;
        carved = NULL;
    } else if (plain != NULL && carved == NULL) {
        result = plain;
        plain = NULL;
    } else if (carved != NULL && plain != NULL && clen == plen) {
        result = carved;
        carved = NULL;
        float a = (float)amount;
        for (size_t i = 0; i < plen; i++) {
            result[i] = plain[i] + (result[i] - plain[i]) * a;
        }
    }
    free(carved);
    free(plain);
    if (result == NULL) {
        return pc_engine_err_other(err, "Content-Aware Scale produced no pixels");
    }

    struct cas_ctx ctx = {id, region, dst, result, want};
    rc = pc_engine_session_edit(s, "Content-Aware Scale", cas_edit, &ctx, err);
    free(result);
    if (rc != PC_OK) {
        return rc;
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *from = yyjson_mut_arr(rd), *to = yyjson_mut_arr(rd), *bnd = yyjson_mut_arr(rd);
    yyjson_mut_arr_add_uint(rd, from, w);
    yyjson_mut_arr_add_uint(rd, from, h);
    yyjson_mut_arr_add_uint(rd, to, nw);
    yyjson_mut_arr_add_uint(rd, to, nh);
    yyjson_mut_arr_add_int(rd, bnd, dst.x0);
    yyjson_mut_arr_add_int(rd, bnd, dst.y0);
    yyjson_mut_arr_add_uint(rd, bnd, nw);
    yyjson_mut_arr_add_uint(rd, bnd, nh);
    yyjson_mut_obj_add_val(rd, root, "from", from);
    yyjson_mut_obj_add_val(rd, root, "to", to);
    yyjson_mut_obj_add_val(rd, root, "bounds", bnd);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

#define CAS_PARAMS                                                                                   \
    "{\"width\":px?,\"height\":px?,\"scaleX\":%?,\"scaleY\":%? (one of px or % per axis; default = "  \
    "no change),\"x\":px?,\"y\":px? (where the result's top-left goes; default = the region's "        \
    "corner),\"amount\":0-100=100 (blend of the carve with a plain resize),\"protect\":\"index|name|" \
    "none\"? (an alpha channel steering seams away),\"protectSkinTones\":bool=false} -> "              \
    "{from:[w,h],to:[w,h],bounds:[x,y,w,h]} (the region = the selection's bounds, else the active "    \
    "pixel layer's content)"

static const pc_engine_command SPECS[] = {
    {CMD, "Content-Aware Scale", MENU_EDIT, 1, "Cmd+Alt+Shift+C", CAS_PARAMS, cas_enabled, run_cas, true},
};

const pc_engine_command *pc_engine_content_scale_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
