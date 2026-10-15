/* Edit > Auto-Align Layers and Auto-Blend Layers, on the selected pixel layers (port of
 * crates/engine/src/align_cmds.rs, E9).
 *
 * Both share Photomerge's registration and blending (photocraft_algo::panorama via photo_cmds.c).
 * Auto-Align matches features between every pair of layers (or registers by intensity for layers
 * that nearly coincide) and warps each. Auto-Blend masks each layer to show the sharpest (Stack)
 * or one layer per region with seams routed where the layers agree (Panorama), and with Seamless
 * Tones and Colors adds a merged layer. Pure reads of the open document -- no file I/O -- so the
 * commands are reachable now; numeric parity is through src/algo. Never crash on hostile params. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/exif.h"
#include "algo/panorama.h"
#include "algo/pyramid.h"
#include "algo/transform.h"
#include "base/mem.h"
#include "color/color.h"
#include "engine_internal.h"
#include "photo_cmds.h"
#include "raster/pixel.h"
#include "raster/surface.h"

/* ---- selected pixel layers ---------------------------------------------------------------- */

/* Selected raster layers, bottom to top. *out is owned (free()); *n the count. */
static pc_status selected_pixel_layers(const pc_engine_doc_state *d, const pc_document *doc,
                                       pc_layer_id **out, size_t *n, pc_error *err)
{
    *out = NULL;
    *n = 0;
    pc_layer_id *sel = NULL;
    size_t nsel = 0;
    if (pc_engine_doc_state_selected_layers(d, &sel, &nsel, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer_id *ids = nsel > 0 ? pc_malloc_array(nsel, sizeof *ids) : NULL;
    if (nsel > 0 && ids == NULL) {
        free(sel);
        return pc_fail(err, PC_ERR_NOMEM, "selected layers");
    }
    size_t k = 0;
    for (size_t i = 0; i < nsel; i++) {
        const pc_layer *l = pc_document_layer(doc, sel[i]);
        if (l != NULL && l->content.kind == PC_LAYER_RASTER) {
            ids[k++] = sel[i];
        }
    }
    free(sel);
    *out = ids;
    *n = k;
    return PC_OK;
}

static pc_status two_layers(const pc_engine_session *s, pc_error *reason)
{
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_fail(reason, PC_ERR_INVALID, "no document open");
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_layer_id *ids = NULL;
    size_t n = 0;
    if (selected_pixel_layers(d, doc, &ids, &n, reason) != PC_OK) {
        return reason != NULL ? reason->code : PC_ERR_NOMEM;
    }
    free(ids);
    if (n < 2) {
        return pc_fail(reason, PC_ERR_INVALID, "select two or more pixel layers in the Layers panel");
    }
    return PC_OK;
}

static const char *str_param(yyjson_val *p, const char *k, const char *d)
{
    yyjson_val *v = pc_json_get(p, k);
    if (v != NULL && yyjson_is_str(v)) {
        const char *s = yyjson_get_str(v);
        if (s != NULL) {
            return s;
        }
    }
    return d;
}

static bool bool_param(yyjson_val *p, const char *k, bool d)
{
    yyjson_val *v = pc_json_get(p, k);
    bool got;
    if (v != NULL && pc_json_as_bool(v, &got)) {
        return got;
    }
    return d;
}

static const char *layout_model(pc_algo_layout l)
{
    switch (l) {
    case PC_ALGO_LAYOUT_REPOSITION:
        return "reposition";
    case PC_ALGO_LAYOUT_COLLAGE:
        return "collage";
    case PC_ALGO_LAYOUT_CYLINDRICAL:
        return "cylindrical";
    case PC_ALGO_LAYOUT_SPHERICAL:
        return "spherical";
    default:
        return "perspective"; /* Perspective and Auto */
    }
}

/* ---- Auto-Align --------------------------------------------------------------------------- */

struct warped_layer {
    pc_layer_id id;
    pc_surface px;
    bool has_mask;
    pc_surface mask;
};

struct align_ctx {
    struct warped_layer *w;
    size_t n;
};

static pc_status auto_align_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                 pc_error *err)
{
    (void)has_active;
    (void)active;
    struct align_ctx *c = vctx;
    for (size_t i = 0; i < c->n; i++) {
        pc_layer *l = pc_document_layer_mut(doc, c->w[i].id);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, c->w[i].id);
        }
        pc_surface *s = pc_layer_surface_mut(l);
        if (s != NULL) {
            pc_surface_fini(s);
            if (pc_surface_clone(&c->w[i].px, s, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
        if (c->w[i].has_mask && l->has_mask) {
            pc_surface_fini(&l->mask.surface);
            if (pc_surface_clone(&c->w[i].mask, &l->mask.surface, err) != PC_OK) {
                return err != NULL ? err->code : PC_ERR_NOMEM;
            }
        }
    }
    return PC_OK;
}

static pc_status auto_align(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "edit.autoAlignLayers";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (selected_pixel_layers(d, doc, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;
    pc_algo_alignment al;
    memset(&al, 0, sizeof al);
    bool have_al = false;
    struct warped_layer *warped = NULL;
    size_t nwarped = 0;
    size_t *moves = NULL;
    size_t nmoves = 0;
    size_t *inliers = NULL;
    pc_engine_photo_image *images = NULL;
    if (nids < 2) {
        rc = pc_engine_err_bad_params(err, cmd, "select two or more pixel layers");
        goto cleanup;
    }
    const char *projection = str_param(p, "projection", "auto");
    pc_algo_layout layout;
    if (!pc_algo_layout_parse(projection, &layout)) {
        rc = pc_engine_err_bad_params(
            err, cmd, "unknown projection `%s` (auto|perspective|cylindrical|spherical|collage|reposition)",
            projection);
        goto cleanup;
    }
    size_t ref_idx = 0;
    yyjson_val *rv = pc_json_get(p, "reference");
    if (rv != NULL && yyjson_is_uint(rv)) {
        pc_layer_id r = (pc_layer_id)yyjson_get_uint(rv);
        bool found = false;
        for (size_t i = 0; i < nids; i++) {
            if (ids[i] == r) {
                ref_idx = i;
                found = true;
                break;
            }
        }
        if (!found) {
            rc = pc_engine_err_bad_params(err, cmd, "reference %llu is not a selected pixel layer",
                                          (unsigned long long)r);
            goto cleanup;
        }
    }
    pc_rect area = pc_document_bounds(doc);
    images = pc_malloc_array(nids, sizeof *images);
    if (images == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t i = 0; i < nids; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
        if (surf == NULL) {
            rc = pc_engine_err_no_layer(err, ids[i]);
            goto cleanup;
        }
        images[i].surf = surf;
        images[i].rect = area;
    }
    bool has_focal = false;
    double focal35 = 0.0;
    if (doc->metadata.exif != NULL) {
        pc_algo_camera_info info;
        if (pc_algo_exif_read(pc_blob_data(doc->metadata.exif), pc_blob_len(doc->metadata.exif), &info,
                              NULL) == PC_OK) {
            if (info.focal_length_35mm.set) {
                has_focal = true;
                focal35 = info.focal_length_35mm.v;
            }
            pc_algo_camera_info_fini(&info);
        }
    }
    bool geometric = bool_param(p, "geometricCorrection", false);
    const char *registration = str_param(p, "registration", "auto");
    if (strcmp(registration, "auto") != 0 && strcmp(registration, "features") != 0 &&
        strcmp(registration, "intensity") != 0) {
        rc = pc_engine_err_bad_params(err, cmd, "`registration` must be auto|features|intensity");
        goto cleanup;
    }

    const char *used = "features";
    bool by_features = false;
    if (strcmp(registration, "intensity") != 0) {
        rc = pc_engine_photo_register(images, nids, layout, true, ref_idx, geometric, has_focal, focal35,
                                      &by_features, &al, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
    }
    if (by_features) {
        have_al = true; /* `used` stays "features" */
    } else if (strcmp(registration, "features") == 0) {
        rc = pc_engine_err_other(err, "%s", PC_ENGINE_PHOTO_NO_DETAIL);
        goto cleanup;
    } else {
        pc_error de;
        pc_error_clear(&de);
        pc_status drc = pc_engine_photo_register_direct(images, nids, layout, ref_idx, &al, &de);
        if (drc != PC_OK) {
            bool is_no_detail = strcmp(de.msg, PC_ENGINE_PHOTO_NO_DETAIL) == 0;
            if (strcmp(registration, "intensity") == 0 && !is_no_detail) {
                rc = pc_engine_err_bad_params(err, cmd, "%s", de.msg);
            } else {
                rc = pc_engine_err_other(err, "%s", PC_ENGINE_PHOTO_NO_DETAIL);
            }
            goto cleanup;
        }
        have_al = true;
        used = "intensity";
    }

    pc_algo_interp interp = pc_algo_interp_parse(str_param(p, "interpolation", "bicubic"));
    bool planar = al.layout == PC_ALGO_LAYOUT_PERSPECTIVE || al.layout == PC_ALGO_LAYOUT_COLLAGE ||
                  al.layout == PC_ALGO_LAYOUT_REPOSITION;
    inliers = pc_calloc_array(nids, sizeof *inliers);
    if (inliers == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t i = 0; i < al.npairs; i++) {
        if (al.pairs[i].a < nids) {
            inliers[al.pairs[i].a] += al.pairs[i].inliers;
        }
        if (al.pairs[i].b < nids) {
            inliers[al.pairs[i].b] += al.pairs[i].inliers;
        }
    }
    moves = pc_malloc_array(nids, sizeof *moves);
    if (moves == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t i = 0; i < nids && i < al.n; i++) {
        if (al.placed[i] && (i != ref_idx || !planar)) {
            moves[nmoves++] = i;
        }
    }
    if (nmoves == 0) {
        rc = pc_engine_err_other(err, "%s", PC_ENGINE_PHOTO_NO_DETAIL);
        goto cleanup;
    }
    warped = pc_calloc_array(nmoves, sizeof *warped);
    if (warped == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t m = 0; m < nmoves; m++) {
        size_t i = moves[m];
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
        if (surf == NULL) {
            rc = pc_engine_err_no_layer(err, ids[i]);
            goto cleanup;
        }
        warped[nwarped].id = ids[i];
        pc_rect b = pc_surface_content_bounds(surf);
        if (pc_rect_is_empty(b)) {
            if (pc_surface_clone(surf, &warped[nwarped].px, err) != PC_OK) {
                rc = err != NULL ? err->code : PC_ERR_NOMEM;
                goto cleanup;
            }
        } else {
            rc = pc_engine_photo_warp_placed(surf, b, 0.0, 0.0, &al.placements[i], 0.0, 0.0, interp,
                                             &warped[nwarped].px, err);
            if (rc != PC_OK) {
                goto cleanup;
            }
        }
        if (l->has_mask) {
            warped[nwarped].has_mask = true;
            pc_rect mb = pc_surface_content_bounds(&l->mask.surface);
            if (pc_rect_is_empty(mb)) {
                if (pc_surface_clone(&l->mask.surface, &warped[nwarped].mask, err) != PC_OK) {
                    rc = err != NULL ? err->code : PC_ERR_NOMEM;
                    goto cleanup;
                }
            } else {
                rc = pc_engine_photo_warp_placed(&l->mask.surface, mb, 0.0, 0.0, &al.placements[i], 0.0,
                                                 0.0, PC_ALGO_INTERP_BILINEAR, &warped[nwarped].mask, err);
                if (rc != PC_OK) {
                    goto cleanup;
                }
            }
        }
        nwarped++;
    }

    struct align_ctx ctx = {warped, nwarped};
    rc = pc_engine_session_edit(s, "Auto-Align Layers", auto_align_edit, &ctx, err);
    if (rc != PC_OK) {
        goto cleanup;
    }

    /* Result JSON. */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto cleanup;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "reference", ids[ref_idx]);
    yyjson_mut_obj_add_str(rd, root, "layout", pc_algo_layout_name(al.layout));
    yyjson_mut_obj_add_str(rd, root, "registration", used);
    yyjson_mut_obj_add_real(rd, root, "focal", al.focal);
    yyjson_mut_obj_add_real(rd, root, "rms", al.rms);
    const char *model = layout_model(al.layout);
    yyjson_mut_val *aligned = yyjson_mut_arr(rd);
    for (size_t m = 0; m < nmoves; m++) {
        size_t i = moves[m];
        yyjson_mut_val *e = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, e, "layer", ids[i]);
        yyjson_mut_obj_add_str(rd, e, "model", model);
        yyjson_mut_obj_add_uint(rd, e, "matches", inliers[i]);
        pc_algo_homography h;
        if (pc_engine_photo_plane_matrix(&al.placements[i], 0.0, 0.0, &h)) {
            yyjson_mut_val *mat = yyjson_mut_arr(rd);
            for (size_t j = 0; j < 9; j++) {
                yyjson_mut_arr_add_real(rd, mat, h.m[j]);
            }
            yyjson_mut_obj_add_val(rd, e, "matrix", mat);
        } else {
            yyjson_mut_obj_add_val(rd, e, "matrix", yyjson_mut_null(rd));
        }
        yyjson_mut_arr_add_val(aligned, e);
    }
    yyjson_mut_obj_add_val(rd, root, "aligned", aligned);
    yyjson_mut_val *failed = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nids && i < al.n; i++) {
        if (!al.placed[i]) {
            yyjson_mut_arr_add_uint(rd, failed, ids[i]);
        }
    }
    yyjson_mut_obj_add_val(rd, root, "failed", failed);
    yyjson_mut_doc_set_root(rd, root);

cleanup:
    if (warped != NULL) {
        for (size_t m = 0; m < nwarped; m++) {
            pc_surface_fini(&warped[m].px);
            if (warped[m].has_mask) {
                pc_surface_fini(&warped[m].mask);
            }
        }
    }
    if (have_al) {
        pc_algo_alignment_fini(&al);
    }
    free(warped);
    free(moves);
    free(inliers);
    free(images);
    free(ids);
    return rc;
}

/* ---- Auto-Blend --------------------------------------------------------------------------- */

struct blend_ctx {
    const pc_layer_id *ids;
    size_t nids;
    float **weights; /* per layer, w*h each */
    pc_rect area;
    size_t w, h;
    pc_pixel_format fmt;
    bool has_blended;
    const float *blended; /* w*h*channels */
    pc_layer_id top;
    bool has_new;
    pc_layer_id new_id;
};

static pc_status auto_blend_edit(pc_document *doc, bool *has_active, pc_layer_id *active, void *vctx,
                                 pc_error *err)
{
    (void)has_active;
    struct blend_ctx *c = vctx;
    size_t cells = c->w * c->h;
    for (size_t i = 0; i < c->nids; i++) {
        pc_layer *l = pc_document_layer_mut(doc, c->ids[i]);
        if (l == NULL) {
            return pc_engine_err_no_layer(err, c->ids[i]);
        }
        if (l->has_mask) {
            pc_layer_mask_fini(&l->mask);
        }
        pc_layer_mask_hide_all(&l->mask);
        l->has_mask = true;
        if (pc_surface_write_region(&l->mask.surface, c->area, c->weights[i], cells, err) != PC_OK) {
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        pc_surface_prune(&l->mask.surface);
    }
    if (!c->has_blended) {
        return PC_OK;
    }
    pc_str name;
    if (pc_document_next_layer_name(doc, "Blended", &name, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_layer nl;
    pc_status rc = pc_layer_raster(&nl, pc_str_c(&name), c->fmt, err);
    pc_str_fini(&name);
    if (rc != PC_OK) {
        return rc;
    }
    pc_surface *s = pc_layer_surface_mut(&nl);
    if (s != NULL) {
        (void)pc_surface_write_region(s, c->area, c->blended, cells * pc_pixel_format_channels(c->fmt),
                                      err);
        pc_surface_prune(s);
    }
    pc_layer_id nid;
    rc = pc_document_insert_above(doc, &c->top, &nl, &nid, err);
    if (rc != PC_OK) {
        pc_layer_fini(&nl);
        return rc;
    }
    *active = nid;
    c->has_new = true;
    c->new_id = nid;
    return PC_OK;
}

static pc_status auto_blend(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "edit.autoBlendLayers";
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_layer_id *ids = NULL;
    size_t nids = 0;
    if (selected_pixel_layers(d, doc, &ids, &nids, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    pc_status rc = PC_OK;
    float **weights = NULL;
    float *blended = NULL;
    float **imgs = NULL;
    float **luma = NULL;
    float **alpha = NULL;
    pc_surface *surfs = NULL;
    size_t *order = NULL;
    pc_engine_photo_blended blend;
    memset(&blend, 0, sizeof blend);
    bool have_blend = false;
    if (nids < 2) {
        rc = pc_engine_err_bad_params(err, cmd, "select two or more pixel layers");
        goto cleanup;
    }
    const char *method = str_param(p, "method", "panorama");
    if (strcmp(method, "panorama") != 0 && strcmp(method, "stack") != 0) {
        rc = pc_engine_err_bad_params(err, cmd, "`method` must be panorama|stack");
        goto cleanup;
    }
    bool seamless = bool_param(p, "seamlessTones", true);
    float halo = pc_algo_stack_fusion_default().halo;
    yyjson_val *hv = pc_json_get(p, "haloControl");
    if (hv != NULL && !yyjson_is_null(hv)) {
        double hd;
        if (pc_json_as_f64(hv, &hd) && hd >= 0.0 && hd <= (double)PC_ALGO_HALO_MAX) {
            halo = (float)hd;
        } else {
            rc = pc_engine_err_bad_params(err, cmd, "`haloControl` must be a number from 0 to %g",
                                          (double)PC_ALGO_HALO_MAX);
            goto cleanup;
        }
    }
    pc_rect area = PC_RECT_EMPTY;
    for (size_t i = 0; i < nids; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
        if (surf != NULL) {
            area = pc_rect_union(area, pc_surface_content_bounds(surf));
        }
    }
    area = pc_rect_intersect(area, pc_document_bounds(doc));
    if (pc_rect_is_empty(area)) {
        rc = pc_engine_err_other(err, "the selected layers are empty");
        goto cleanup;
    }
    size_t w = pc_rect_width(area), h = pc_rect_height(area);
    size_t cells = w * h;

    imgs = pc_calloc_array(nids, sizeof *imgs);
    luma = pc_calloc_array(nids, sizeof *luma);
    alpha = pc_calloc_array(nids, sizeof *alpha);
    if (imgs == NULL || luma == NULL || alpha == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    bool have_fmt = false;
    pc_pixel_format fmt = PC_PIXEL_FORMAT_RGBA8;
    for (size_t i = 0; i < nids; i++) {
        const pc_layer *l = pc_document_layer(doc, ids[i]);
        const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
        if (surf == NULL) {
            rc = pc_engine_err_no_layer(err, ids[i]);
            goto cleanup;
        }
        pc_pixel_format f = pc_surface_format(surf);
        if (have_fmt && !pc_pixel_format_eq(f, fmt)) {
            rc = pc_engine_err_other(err, "the layers have different pixel formats");
            goto cleanup;
        }
        fmt = f;
        have_fmt = true;
        size_t nch = pc_surface_channels(surf);
        float *px = NULL;
        size_t plen = 0;
        if (pc_surface_read_region(surf, area, &px, &plen, err) != PC_OK) {
            rc = err != NULL ? err->code : PC_ERR_NOMEM;
            goto cleanup;
        }
        float *lv = pc_malloc_array(cells, sizeof *lv);
        float *av = pc_malloc_array(cells, sizeof *av);
        if (lv == NULL || av == NULL) {
            free(px);
            free(lv);
            free(av);
            rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            goto cleanup;
        }
        for (size_t q = 0; q < cells; q++) {
            float cc[4];
            pc_to_rgba(f, px + q * nch, nch, cc);
            lv[q] = 0.2126f * cc[0] + 0.7152f * cc[1] + 0.0722f * cc[2];
            av[q] = cc[3];
        }
        imgs[i] = px;
        luma[i] = lv;
        alpha[i] = av;
    }

    weights = pc_calloc_array(nids, sizeof *weights);
    if (weights == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    if (strcmp(method, "stack") == 0) {
        rc = pc_algo_stack_weights(w, h, (const float *const *)luma, (const float *const *)alpha, nids,
                                   weights, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
        if (seamless) {
            pc_algo_stack_fusion fusion = pc_algo_stack_fusion_default();
            fusion.halo = halo;
            rc = pc_algo_fuse_stack(w, h, pc_pixel_format_channels(fmt), (const float *const *)imgs,
                                    (const float *const *)luma, (const float *const *)alpha, nids, fusion,
                                    &blended, err);
            if (rc != PC_OK) {
                goto cleanup;
            }
        }
    } else {
        surfs = pc_calloc_array(nids, sizeof *surfs);
        order = pc_malloc_array(nids, sizeof *order);
        if (surfs == NULL || order == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            goto cleanup;
        }
        size_t nsurfs = 0;
        for (size_t i = 0; i < nids; i++) {
            const pc_layer *l = pc_document_layer(doc, ids[i]);
            const pc_surface *surf = l != NULL ? pc_layer_surface(l) : NULL;
            if (surf == NULL) {
                rc = pc_engine_err_no_layer(err, ids[i]);
                goto cleanup;
            }
            if (pc_surface_clone(surf, &surfs[nsurfs], err) != PC_OK) {
                rc = err != NULL ? err->code : PC_ERR_NOMEM;
                goto cleanup;
            }
            nsurfs++;
        }
        pc_engine_photo_seam_order(surfs, nids, 0, order);
        rc = pc_engine_photo_seam_blend(surfs, nids, area, order, seamless, false, false, NULL, NULL, 0.0,
                                        0.0, &blend, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
        have_blend = true;
        for (size_t i = 0; i < nids; i++) {
            float *wt = NULL;
            size_t wlen = 0;
            if (pc_surface_read_region(&blend.layer_mask[i], area, &wt, &wlen, err) != PC_OK) {
                rc = err != NULL ? err->code : PC_ERR_NOMEM;
                goto cleanup;
            }
            weights[i] = wt;
        }
        if (seamless) {
            size_t blen = 0;
            if (pc_surface_read_region(&blend.composite, area, &blended, &blen, err) != PC_OK) {
                rc = err != NULL ? err->code : PC_ERR_NOMEM;
                goto cleanup;
            }
        }
    }

    struct blend_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ids = ids;
    ctx.nids = nids;
    ctx.weights = weights;
    ctx.area = area;
    ctx.w = w;
    ctx.h = h;
    ctx.fmt = fmt;
    ctx.has_blended = blended != NULL;
    ctx.blended = blended;
    ctx.top = ids[nids - 1];
    rc = pc_engine_session_edit(s, "Auto-Blend Layers", auto_blend_edit, &ctx, err);
    if (rc != PC_OK) {
        goto cleanup;
    }

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto cleanup;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_str(rd, root, "method", method);
    yyjson_mut_val *larr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nids; i++) {
        yyjson_mut_arr_add_uint(rd, larr, ids[i]);
    }
    yyjson_mut_obj_add_val(rd, root, "layers", larr);
    if (ctx.has_new) {
        yyjson_mut_obj_add_uint(rd, root, "blended", ctx.new_id);
    } else {
        yyjson_mut_obj_add_val(rd, root, "blended", yyjson_mut_null(rd));
    }
    yyjson_mut_doc_set_root(rd, root);

cleanup:
    if (weights != NULL) {
        for (size_t i = 0; i < nids; i++) {
            free(weights[i]);
        }
        free(weights);
    }
    free(blended);
    if (imgs != NULL) {
        for (size_t i = 0; i < nids; i++) {
            free(imgs[i]);
        }
        free(imgs);
    }
    if (luma != NULL) {
        for (size_t i = 0; i < nids; i++) {
            free(luma[i]);
        }
        free(luma);
    }
    if (alpha != NULL) {
        for (size_t i = 0; i < nids; i++) {
            free(alpha[i]);
        }
        free(alpha);
    }
    if (surfs != NULL) {
        for (size_t i = 0; i < nids; i++) {
            pc_surface_fini(&surfs[i]);
        }
        free(surfs);
    }
    free(order);
    if (have_blend) {
        pc_engine_photo_blended_fini(&blend);
    }
    free(ids);
    return rc;
}

/* ---- specs -------------------------------------------------------------------------------- */

static const char *const MENU_EDIT[] = {"Edit"};

static const pc_engine_command SPECS[] = {
    {"edit.autoAlignLayers", "Auto-Align Layers\xE2\x80\xA6", MENU_EDIT, 1, NULL,
     "{\"projection\":\"auto|perspective|cylindrical|spherical|collage|reposition\",\"reference\":layer "
     "id?=bottom selected layer,\"geometricCorrection\":bool=false,\"interpolation\":\"bicubic|bilinear|"
     "nearest\",\"registration\":\"auto|features|intensity\"}",
     two_layers, auto_align, true},
    {"edit.autoBlendLayers", "Auto-Blend Layers\xE2\x80\xA6", MENU_EDIT, 1, NULL,
     "{\"method\":\"panorama|stack\",\"seamlessTones\":bool=true,\"haloControl\":0..8=2}", two_layers,
     auto_blend, true},
};

const pc_engine_command *pc_engine_align_specs(size_t *n)
{
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
