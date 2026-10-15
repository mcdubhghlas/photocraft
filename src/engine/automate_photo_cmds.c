/* File > Automate computational-photography commands (port of the command layer of
 * crates/engine/src/photo_cmds.rs, ET3):
 *   - file.automate.cropAndStraightenPhotos  -- find the photos on a scan, deskew each into its
 *     own document (scancrop + the projective warp). Fully ported.
 *   - file.automate.photomerge / mergeToHdrPro -- DEFERRED (graceful "unsupported"). The blending
 *     and HDR PRIMITIVES exist (photo_cmds.h register/warp_placed/seam_order/seam_blend, algo
 *     hdr.h / content_aware.h / exif.h), but the command wiring (load_sources via pc_engine_import,
 *     a POSIX directory lister, content-aware fill of panorama edges, the alignment/blend and HDR
 *     merge/tone-map document building and the result JSON) is a large follow-up tracked in the
 *     dev log. They are registered here so the File > Automate menu surface matches and they fail
 *     with an error, never a crash.
 *
 * Never crash on hostile params (REWRITE_PLAN rule 9). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/params.h"
#include "algo/scancrop.h"
#include "algo/transform.h"
#include "base/blob.h"
#include "base/json.h"
#include "base/mem.h"
#include "color/color.h"
#include "doc/doc.h"
#include "raster/surface.h"

#include "engine_internal.h"

/* ---- Crop and Straighten Photos ----------------------------------------------------------- */

static pc_status run_crop_and_straighten(pc_engine_session *s, yyjson_val *p, pc_json_value *out,
                                         pc_error *err)
{
    (void)p;
    const pc_engine_doc_state *d = pc_engine_session_active(s);
    if (d == NULL) {
        return pc_engine_err_no_document(err);
    }
    const pc_document *doc = pc_doc_arc_get(d->doc);
    pc_pixel_format fmt = pc_document_pixel_format(doc);
    pc_rect area = pc_document_bounds(doc);
    size_t w = pc_rect_width(area), h = pc_rect_height(area);
    if (w == 0 || h == 0) {
        return pc_engine_err_other(err, "the document is empty");
    }
    pc_surface flat;
    pc_status rc = pc_engine_flattened(doc, fmt, &flat, err);
    if (rc != PC_OK) {
        pc_surface_fini(&flat);
        return rc;
    }
    size_t count = w * h;
    float (*rgba)[4] = pc_malloc_array(count, sizeof(float[4]));
    if (rgba == NULL) {
        pc_surface_fini(&flat);
        return pc_fail(err, PC_ERR_NOMEM, "crop and straighten");
    }
    if (pc_surface_read_rgba_into(&flat, area, rgba, count, err) != PC_OK) {
        free(rgba);
        pc_surface_fini(&flat);
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    /* Transparent pixels count as background (composite over white). */
    for (size_t i = 0; i < count; i++) {
        float a = rgba[i][3];
        if (a < 1.0f) {
            for (int c = 0; c < 3; c++) {
                rgba[i][c] = rgba[i][c] * a + (1.0f - a);
            }
        }
    }
    pc_algo_found_photo *found = NULL;
    size_t nfound = 0;
    rc = pc_algo_scancrop_find_photos(w, h, (const float (*)[4])rgba, count, &found, &nfound, err);
    free(rgba);
    if (rc != PC_OK) {
        pc_surface_fini(&flat);
        return rc;
    }
    if (nfound == 0) {
        free(found);
        pc_surface_fini(&flat);
        return pc_engine_err_other(err, "no photos found on a uniform background");
    }
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        free(found);
        pc_surface_fini(&flat);
        return PC_ERR_NOMEM;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_val *photos = yyjson_mut_arr(rd);
    for (size_t k = 0; k < nfound; k++) {
        const pc_algo_found_photo *f = &found[k];
        double wd = f->width < 1.0 ? 1.0 : round(f->width);
        double hd = f->height < 1.0 ? 1.0 : round(f->height);
        if (!isfinite(wd) || !isfinite(hd) || wd > 300000.0 || hd > 300000.0) {
            continue;
        }
        uint32_t pw = (uint32_t)wd, ph = (uint32_t)hd;
        double corners[4][2];
        pc_algo_found_photo_corners(f, corners);
        double r[4] = {0.0, 0.0, (double)pw, (double)ph};
        pc_algo_homography fwd, hm;
        if (!pc_algo_homography_rect_to_quad(r, (const double (*)[2])corners, &fwd) ||
            !pc_algo_homography_inverse(&fwd, &hm)) {
            continue;
        }
        pc_surface warped;
        if (pc_algo_warp_surface(&flat, area, &hm, PC_ALGO_INTERP_BICUBIC, &warped, err) != PC_OK) {
            continue; /* a single bad photo doesn't fail the whole scan */
        }
        pc_rect keep = pc_rect_make(0, 0, (int32_t)pw, (int32_t)ph);
        /* Read the cropped region, force it opaque (a scanned print), write it to a fresh surface. */
        size_t kn = (size_t)pw * (size_t)ph;
        size_t nch = pc_pixel_format_channels(fmt);
        float *data = NULL;
        size_t len = 0;
        if (pc_surface_read_region(&warped, keep, &data, &len, err) != PC_OK || nch == 0) {
            pc_surface_fini(&warped);
            free(data);
            continue;
        }
        for (size_t i = 0; i < kn; i++) {
            data[i * nch + (nch - 1)] = 1.0f;
        }
        pc_surface px;
        pc_surface_init(&px, fmt);
        rc = pc_surface_write_region(&px, keep, data, len, err);
        free(data);
        pc_surface_fini(&warped);
        if (rc != PC_OK) {
            pc_surface_fini(&px);
            continue;
        }
        pc_document nd;
        char name[320];
        (void)snprintf(name, sizeof name, "%.*s copy %zu", (int)doc->name.len,
                       doc->name.ptr != NULL ? doc->name.ptr : "", k + 1);
        if (pc_document_init(&nd, name, pc_size_make(pw, ph), doc->mode, doc->depth, err) != PC_OK) {
            pc_surface_fini(&px);
            continue;
        }
        nd.resolution_dpi = doc->resolution_dpi;
        if (doc->icc_profile != NULL) {
            nd.icc_profile = pc_blob_retain(doc->icc_profile);
        }
        pc_layer bg;
        if (pc_layer_raster(&bg, "Background", fmt, err) != PC_OK) {
            pc_surface_fini(&px);
            pc_document_fini(&nd);
            continue;
        }
        pc_surface *bgs = pc_layer_surface_mut(&bg);
        if (bgs != NULL) {
            pc_surface_fini(bgs);
            *bgs = px; /* move */
        } else {
            pc_surface_fini(&px);
        }
        bg.locks.transparency = true;
        bg.locks.position = true;
        pc_layer_id tmp = 0;
        if (pc_document_insert_above(&nd, NULL, &bg, &tmp, err) != PC_OK) {
            pc_layer_fini(&bg);
            pc_document_fini(&nd);
            continue;
        }
        size_t idx = 0;
        if (pc_engine_session_add_document(s, &nd, NULL, &idx, err) != PC_OK) {
            pc_document_fini(&nd);
            continue;
        }
        yyjson_mut_val *row = yyjson_mut_obj(rd);
        yyjson_mut_obj_add_uint(rd, row, "document", idx);
        yyjson_mut_obj_add_uint(rd, row, "width", pw);
        yyjson_mut_obj_add_uint(rd, row, "height", ph);
        yyjson_mut_obj_add_real(rd, row, "angle", f->angle);
        yyjson_mut_val *ctr = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_real(rd, ctr, f->center[0]);
        yyjson_mut_arr_add_real(rd, ctr, f->center[1]);
        yyjson_mut_obj_add_val(rd, row, "center", ctr);
        yyjson_mut_arr_add_val(photos, row);
    }
    free(found);
    pc_surface_fini(&flat);
    yyjson_mut_obj_add_val(rd, root, "photos", photos);
    yyjson_mut_doc_set_root(rd, root);
    return PC_OK;
}

/* ---- DEFERRED: Photomerge / Merge to HDR Pro ---------------------------------------------- */

static pc_status run_photomerge(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    (void)out;
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "Photomerge is not yet wired in this build (the panorama primitives exist; the "
                   "source-loading and blended-document assembly are a tracked follow-up)");
}

static pc_status run_merge_to_hdr(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    (void)s;
    (void)p;
    (void)out;
    return pc_fail(err, PC_ERR_UNSUPPORTED,
                   "Merge to HDR Pro is not yet wired in this build (the HDR merge/tone-map "
                   "primitives exist; the source-loading and document assembly are a tracked "
                   "follow-up)");
}

/* ---- specs -------------------------------------------------------------------------------- */

static pc_status always(const pc_engine_session *s, pc_error *reason)
{
    (void)s;
    (void)reason;
    return PC_OK;
}

const pc_engine_command *pc_engine_photo_automate_specs(size_t *n)
{
    static const char *const MENU[] = {"File", "Automate"};
    static const pc_engine_command SPECS[] = {
        {"file.automate.photomerge", "Photomerge\xE2\x80\xA6", MENU, 2, NULL,
         "{\"paths\":[str]|folder,\"useOpenDocuments\":bool=false,\"layout\":\"auto|perspective|"
         "cylindrical|spherical|collage|reposition\",\"blend\":bool=true,\"vignetteRemoval\":bool="
         "false,\"geometricCorrection\":bool=false,\"contentAwareFill\":bool=false,\"focalLength\":"
         "mm=0} -> new document, one masked layer per image",
         always, run_photomerge, true},
        {"file.automate.mergeToHdrPro", "Merge to HDR Pro\xE2\x80\xA6", MENU, 2, NULL,
         "{\"paths\":[str]|folder,\"useOpenDocuments\":bool=false,\"exposures\":[ev]?,\"align\":"
         "bool=true,\"removeGhosts\":bool=false,\"ghostBase\":int?,\"mode\":\"32|16|8\",\"method\":"
         "\"localAdaptation|exposureGamma|highlightCompression|equalizeHistogram\",\"radius\":"
         "1..500=7,\"strength\":0.1..4=0.52,\"gamma\":0.1..2=1,\"exposure\":-5..5=0,\"detail\":"
         "-100..300=30,\"shadow\":-100..100=0,\"highlight\":-100..100=0,\"vibrance\":-100..100=0,"
         "\"saturation\":-100..100=20} -> new document",
         always, run_merge_to_hdr, true},
        {"file.automate.cropAndStraightenPhotos", "Crop and Straighten Photos", MENU, 2, NULL,
         "{} -> one new document per photo found on the scan", pc_engine_has_doc,
         run_crop_and_straighten, true},
    };
    *n = sizeof SPECS / sizeof SPECS[0];
    return SPECS;
}
