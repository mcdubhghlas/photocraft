/* File > Automate computational-photography commands (port of the command layer of
 * crates/engine/src/photo_cmds.rs, ET3 + E-tail):
 *   - file.automate.cropAndStraightenPhotos  -- find the photos on a scan, deskew each into its
 *     own document (scancrop + the projective warp).
 *   - file.automate.photomerge                -- panorama: load the sources (open documents or
 *     files/folder), register by features (Brown-Lowe via the panorama primitives), warp each into
 *     the panorama canvas, and build a new document with one gain-compensated, seam-and-multi-band-
 *     blended masked layer per image (or plain warped layers with "blend": false), optionally with
 *     a content-aware fill of the bow-tie edges.
 *   - file.automate.mergeToHdrPro             -- exposure merge: load the exposures, estimate the
 *     relative exposures (params / EXIF / estimated), align by Ward's MTB, merge to a linear
 *     radiance map (Debevec-Malik response + ghost removal), and either keep 32-bit linear or
 *     tone-map to 16/8-bit by one of the four methods.
 *
 * The heavy lifting is the shared panorama / HDR primitives (photo_cmds.c, algo panorama / pyramid
 * / direct / hdr / content_aware / exif); this module is the source-loading and document assembly.
 * Unlike the reference (which runs Photomerge as a background job), these run synchronously
 * (REWRITE_PLAN.md §7). Never crash on hostile params (rule 9): any params or document state yields
 * an error, never a panic. */
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "algo/content_aware.h"
#include "algo/exif.h"
#include "algo/hdr.h"
#include "algo/panorama.h"
#include "algo/params.h"
#include "algo/scancrop.h"
#include "algo/tone.h"
#include "algo/transform.h"
#include "base/blob.h"
#include "base/buf.h"
#include "base/json.h"
#include "base/mem.h"
#include "cms/builtin.h"
#include "cms/profile.h"
#include "color/color.h"
#include "doc/doc.h"
#include "engine.h"
#include "photo_cmds.h"
#include "raster/pixel.h"
#include "raster/surface.h"

#include "engine_internal.h"

/* ---- small helpers ------------------------------------------------------------------------- */

static double now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0.0;
    }
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
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

static float f_param(yyjson_val *p, const char *k, double d)
{
    yyjson_val *v = pc_json_get(p, k);
    double got;
    if (v != NULL && pc_json_as_f64(v, &got)) {
        return (float)got;
    }
    return (float)d;
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

/* The last path component (after the final '/' or '\\'), newly allocated. */
static char *file_name_dup(const char *path)
{
    const char *base = path;
    for (const char *c = path; *c != '\0'; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    return pc_strndup(base, strlen(base));
}

/* ---- source loading (photo_cmds::load_sources) -------------------------------------------- */

/* Extensions a folder is scanned for (file_cmds::OPENABLE). */
static const char *const OPENABLE[] = {
    "pdn", "ora", "kra", "krz", "psd", "psb", "pcraft", "png", "jpg", "jpeg", "tif",
    "tiff", "webp", "gif", "bmp", "tga", "exr", "hdr", "qoi", "ico", "pnm", "ppm", "pgm",
    "fits", "fit", "fts", "dpx", "heic", "heif", "hif", "dng", "cr2", "nef", "nrw", "arw",
    "pef", "svg", "svgz", "af", "afdesign", "afphoto", "afpub"};

static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool openable_ext(const char *name)
{
    const char *dot = NULL;
    for (const char *c = name; *c != '\0'; c++) {
        if (*c == '.') {
            dot = c;
        }
    }
    if (dot == NULL || dot[1] == '\0') {
        return false;
    }
    const char *ext = dot + 1;
    char low[16];
    size_t i = 0;
    for (; ext[i] != '\0' && i < sizeof low - 1; i++) {
        low[i] = lc(ext[i]);
    }
    if (ext[i] != '\0') {
        return false; /* longer than any OPENABLE entry */
    }
    low[i] = '\0';
    for (size_t k = 0; k < sizeof OPENABLE / sizeof OPENABLE[0]; k++) {
        if (strcmp(low, OPENABLE[k]) == 0) {
            return true;
        }
    }
    return false;
}

static int str_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void free_list(char **list, size_t n)
{
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(list[i]);
    }
    free(list);
}

/* Files of `dir` that look like openable images, owned paths sorted by name (file_cmds::list_images). */
static pc_status list_images(const char *dir, char ***out, size_t *out_n, pc_error *err)
{
    DIR *dd = opendir(dir);
    if (dd == NULL) {
        return pc_fail(err, PC_ERR_IO, "%s: %s", dir, strerror(errno));
    }
    char **list = NULL;
    size_t n = 0, cap = 0;
    pc_status rc = PC_OK;
    struct dirent *de;
    while ((de = readdir(dd)) != NULL) {
        if (!openable_ext(de->d_name)) {
            continue;
        }
        char *full = pc_engine_file_join(dir, de->d_name);
        if (full == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "%s", dir);
            break;
        }
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(full);
            continue;
        }
        char **g = pc_grow_array(list, &cap, n + 1, sizeof *list);
        if (g == NULL) {
            free(full);
            rc = pc_fail(err, PC_ERR_NOMEM, "%s", dir);
            break;
        }
        list = g;
        list[n++] = full;
    }
    closedir(dd);
    if (rc != PC_OK) {
        free_list(list, n);
        return rc;
    }
    if (n > 1) {
        qsort(list, n, sizeof *list, str_cmp);
    }
    *out = list;
    *out_n = n;
    return PC_OK;
}

/* One flattened input image plus the exposure / focal metadata derived from its EXIF. */
typedef struct source {
    char *name;
    pc_surface surf;
    size_t w, h;
    bool has_focal35;
    double focal35;
    bool has_exp;
    double exp_factor;
} source;

static void sources_free(source *srcs, size_t n)
{
    if (srcs == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(srcs[i].name);
        pc_surface_fini(&srcs[i].surf);
    }
    free(srcs);
}

/* One document to flatten: either borrowed (an open document) or owned (imported from a file). */
typedef struct src_doc {
    char *name;
    const pc_document *doc;
    pc_document owned;
    bool is_owned;
} src_doc;

static void src_docs_free(src_doc *d, size_t n)
{
    if (d == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        free(d[i].name);
        if (d[i].is_owned) {
            pc_document_fini(&d[i].owned);
        }
    }
    free(d);
}

/* Load the sources: open documents (with "useOpenDocuments") and/or the files named in "paths"
 * (an array) or scanned from the folder in "paths"/"input". *out_srcs is an owned array of `n`
 * flattened sources in the common pixel format *out_fmt; *out_icc (may be NULL) is a retained copy
 * of the first source's profile when the common format kept its colour model. */
static pc_status load_sources(pc_engine_session *s, yyjson_val *p, const char *cmd, source **out_srcs,
                              size_t *out_n, pc_pixel_format *out_fmt, pc_blob **out_icc, pc_error *err)
{
    *out_srcs = NULL;
    *out_n = 0;
    *out_icc = NULL;

    src_doc *docs = NULL;
    size_t ndocs = 0, cap = 0;
    source *srcs = NULL;
    char **paths = NULL;
    size_t npaths = 0, pcap = 0;
    pc_status rc = PC_OK;

#define GROW_DOCS()                                                                                \
    do {                                                                                           \
        src_doc *g = pc_grow_array(docs, &cap, ndocs + 1, sizeof *docs);                           \
        if (g == NULL) {                                                                           \
            rc = pc_fail(err, PC_ERR_NOMEM, "%s", cmd);                                            \
            goto cleanup;                                                                          \
        }                                                                                          \
        docs = g;                                                                                  \
    } while (0)

    if (bool_param(p, "useOpenDocuments", false)) {
        size_t nopen = 0;
        const pc_engine_doc_state *ds = pc_engine_session_documents(s, &nopen);
        for (size_t i = 0; i < nopen; i++) {
            const pc_document *d = pc_doc_arc_get(ds[i].doc);
            if (d == NULL) {
                continue;
            }
            GROW_DOCS();
            docs[ndocs].name = pc_strndup(pc_str_c(&d->name), d->name.len);
            if (docs[ndocs].name == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "%s", cmd);
                goto cleanup;
            }
            docs[ndocs].doc = d;
            docs[ndocs].is_owned = false;
            ndocs++;
        }
    }

    /* "paths" (or "input"): an array of files or a folder string. */
    yyjson_val *pv = pc_json_get(p, "paths");
    if (pv == NULL) {
        pv = pc_json_get(p, "input");
    }
    if (pv != NULL && yyjson_is_arr(pv)) {
        size_t idx, max;
        yyjson_val *e;
        yyjson_arr_foreach(pv, idx, max, e) {
            size_t slen = 0;
            const char *str = pc_json_as_str(e, &slen);
            if (str == NULL) {
                continue;
            }
            char **g = pc_grow_array(paths, &pcap, npaths + 1, sizeof *paths);
            if (g == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "%s", cmd);
                goto cleanup;
            }
            paths = g;
            paths[npaths] = pc_strndup(str, slen);
            if (paths[npaths] == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, "%s", cmd);
                goto cleanup;
            }
            npaths++;
        }
    } else if (pv != NULL && yyjson_is_str(pv)) {
        const char *dir = yyjson_get_str(pv);
        if (dir != NULL) {
            rc = list_images(dir, &paths, &npaths, err);
            if (rc != PC_OK) {
                goto cleanup;
            }
        }
    }

    for (size_t i = 0; i < npaths; i++) {
        pc_buf bytes = PC_BUF_INIT;
        rc = pc_engine_read_file(paths[i], &bytes, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
        char *name = file_name_dup(paths[i]);
        pc_document imported;
        if (name == NULL) {
            pc_buf_fini(&bytes);
            rc = pc_fail(err, PC_ERR_NOMEM, "%s", cmd);
            goto cleanup;
        }
        rc = pc_engine_import(name, bytes.data, bytes.len, &imported, err);
        pc_buf_fini(&bytes);
        if (rc != PC_OK) {
            free(name);
            goto cleanup;
        }
        GROW_DOCS();
        docs[ndocs].name = name;
        docs[ndocs].owned = imported; /* move */
        docs[ndocs].doc = &docs[ndocs].owned;
        docs[ndocs].is_owned = true;
        ndocs++;
    }
#undef GROW_DOCS

    if (ndocs == 0) {
        rc = pc_engine_err_bad_params(
            err, cmd, "pass \"paths\" (files or a folder) or \"useOpenDocuments\": true");
        goto cleanup;
    }

    /* Common pixel format: the first source's, mapped to RGB for CMYK / Lab / Multichannel. */
    const pc_document *first = docs[0].doc;
    pc_pixel_format fmt = pc_document_pixel_format(first);
    bool remapped = fmt.mode == PC_COLOR_MODE_CMYK || fmt.mode == PC_COLOR_MODE_LAB ||
                    fmt.mode == PC_COLOR_MODE_MULTICHANNEL;
    if (remapped) {
        fmt = (pc_pixel_format){PC_COLOR_MODE_RGB, fmt.sample, true};
    }
    pc_blob *icc = remapped ? NULL : pc_blob_retain(first->icc_profile);

    srcs = pc_calloc_array(ndocs, sizeof *srcs);
    if (srcs == NULL) {
        pc_blob_release(icc);
        rc = pc_fail(err, PC_ERR_NOMEM, "%s", cmd);
        goto cleanup;
    }

    for (size_t i = 0; i < ndocs; i++) {
        const pc_document *d = docs[i].doc;
        srcs[i].name = docs[i].name;
        docs[i].name = NULL; /* moved into the source */
        srcs[i].w = d->size.width;
        srcs[i].h = d->size.height;
        rc = pc_engine_flattened(d, fmt, &srcs[i].surf, err);
        if (rc != PC_OK) {
            /* flattened already initialised a valid (empty) surface on OOM; still fail. */
            pc_blob_release(icc);
            goto cleanup;
        }
        pc_algo_camera_info info;
        if (d->metadata.exif != NULL &&
            pc_algo_exif_read(pc_blob_data(d->metadata.exif), pc_blob_len(d->metadata.exif), &info,
                              NULL) == PC_OK) {
            if (info.focal_length_35mm.set) {
                srcs[i].has_focal35 = true;
                srcs[i].focal35 = info.focal_length_35mm.v;
            }
            double ef;
            if (pc_algo_exif_exposure_factor(&info, &ef)) {
                srcs[i].has_exp = true;
                srcs[i].exp_factor = ef;
            }
            pc_algo_camera_info_fini(&info);
        }
    }

    *out_srcs = srcs;
    *out_n = ndocs;
    *out_fmt = fmt;
    *out_icc = icc;
    srcs = NULL;

cleanup:
    sources_free(srcs, srcs != NULL ? ndocs : 0);
    src_docs_free(docs, ndocs);
    free_list(paths, npaths);
    return rc;
}

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

/* ---- Photomerge --------------------------------------------------------------------------- */

/* Content-aware fill of the transparent areas of a composite (inside its bounding rectangle), at a
 * reduced scale for large panoramas (photo_cmds::content_aware_fill). *out is initialised on
 * success (pc_surface_fini). */
static pc_status content_aware_fill(const pc_surface *comp, pc_rect canvas, pc_surface *out,
                                    pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(comp);
    size_t n = pc_pixel_format_channels(fmt);
    if (n < 1) {
        return pc_fail(err, PC_ERR_INVALID, "content-aware fill: no channels");
    }
    size_t cc = n - 1;
    size_t w = pc_rect_width(canvas), h = pc_rect_height(canvas);
    if (w == 0 || h == 0) {
        return pc_fail(err, PC_ERR_INVALID, "content-aware fill: empty canvas");
    }
    double kk = ceil(sqrt((double)(w * h) / 1.5e6));
    size_t k = kk < 1.0 ? 1 : (size_t)kk;
    size_t sw = (w + k - 1) / k, sh = (h + k - 1) / k;

    float *full = NULL;
    size_t flen = 0;
    pc_status rc = pc_surface_read_region(comp, canvas, &full, &flen, err);
    if (rc != PC_OK) {
        return rc;
    }
    float *small = pc_calloc_array(sw * sh * (cc > 0 ? cc : 1), sizeof *small);
    bool *hole = pc_calloc_array(sw * sh, sizeof *hole);
    bool *src_mask = pc_calloc_array(sw * sh, sizeof *src_mask);
    float *acc_a = pc_calloc_array(sw * sh, sizeof *acc_a);
    float *cnt = pc_calloc_array(sw * sh, sizeof *cnt);
    if (small == NULL || hole == NULL || src_mask == NULL || acc_a == NULL || cnt == NULL) {
        free(full);
        free(small);
        free(hole);
        free(src_mask);
        free(acc_a);
        free(cnt);
        return pc_fail(err, PC_ERR_NOMEM, "content-aware fill");
    }
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            size_t i = (y / k) * sw + x / k;
            const float *px = full + (y * w + x) * n;
            float a = px[cc];
            for (size_t c = 0; c < cc; c++) {
                small[i * cc + c] += px[c] * a;
            }
            acc_a[i] += a;
            cnt[i] += 1.0f;
        }
    }
    for (size_t i = 0; i < sw * sh; i++) {
        float a = acc_a[i];
        for (size_t c = 0; c < cc; c++) {
            small[i * cc + c] = a > 0.0f ? small[i * cc + c] / a : 0.0f;
        }
        hole[i] = a / (cnt[i] > 1.0f ? cnt[i] : 1.0f) < 0.99f;
        src_mask[i] = !hole[i];
    }
    pc_algo_ca_fill_options opts = pc_algo_ca_fill_options_default();
    float *filled = NULL;
    rc = pc_algo_ca_fill(sw, sh, cc, small, sw * sh * cc, hole, src_mask, sw * sh, &opts, NULL,
                         &filled, err);
    free(small);
    free(hole);
    free(src_mask);
    free(acc_a);
    free(cnt);
    if (rc != PC_OK) {
        free(full);
        return rc;
    }
    float *outbuf = pc_calloc_array(w * h * n, sizeof *outbuf);
    if (outbuf == NULL) {
        free(full);
        free(filled);
        return pc_fail(err, PC_ERR_NOMEM, "content-aware fill");
    }
    for (size_t y = 0; y < h; y++) {
        for (size_t x = 0; x < w; x++) {
            size_t o = (y * w + x) * n;
            float a = full[o + cc];
            float fx = clampf(((float)x + 0.5f) / (float)k - 0.5f, 0.0f, (float)(sw - 1));
            float fy = clampf(((float)y + 0.5f) / (float)k - 0.5f, 0.0f, (float)(sh - 1));
            size_t xi = (size_t)fx, yi = (size_t)fy;
            size_t x2 = (xi + 1 < sw) ? xi + 1 : sw - 1;
            size_t y2 = (yi + 1 < sh) ? yi + 1 : sh - 1;
            float tx = fx - (float)xi, ty = fy - (float)yi;
            for (size_t c = 0; c < cc; c++) {
                float a00 = filled[(yi * sw + xi) * cc + c];
                float a10 = filled[(yi * sw + x2) * cc + c];
                float a01 = filled[(y2 * sw + xi) * cc + c];
                float a11 = filled[(y2 * sw + x2) * cc + c];
                float fv = (a00 * (1.0f - tx) + a10 * tx) * (1.0f - ty) +
                           (a01 * (1.0f - tx) + a11 * tx) * ty;
                outbuf[o + c] = full[o + c] * a + fv * (1.0f - a);
            }
            outbuf[o + cc] = 1.0f;
        }
    }
    free(full);
    free(filled);
    pc_surface_init(out, fmt);
    rc = pc_surface_write_region(out, canvas, outbuf, w * h * n, err);
    pc_surface_prune(out);
    free(outbuf);
    if (rc != PC_OK) {
        pc_surface_fini(out);
    }
    return rc;
}

/* The 35 mm focal length agreed by the sources' EXIF (median), if any (photo_cmds::exif_focal). */
static bool exif_focal(const source *srcs, size_t n, double *out)
{
    double *f = pc_malloc_array(n > 0 ? n : 1, sizeof *f);
    if (f == NULL) {
        return false;
    }
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (srcs[i].has_focal35) {
            f[m++] = srcs[i].focal35;
        }
    }
    if (m == 0) {
        free(f);
        return false;
    }
    /* total_cmp order: for finite positive focal lengths this is the numeric order. */
    for (size_t i = 1; i < m; i++) {
        double key = f[i];
        size_t j = i;
        while (j > 0 && f[j - 1] > key) {
            f[j] = f[j - 1];
            j--;
        }
        f[j] = key;
    }
    *out = f[m / 2];
    free(f);
    return true;
}

static pc_status run_photomerge(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "file.automate.photomerge";
    double t0 = now_ms();

    const char *layout_s = str_param(p, "layout", "auto");
    pc_algo_layout layout;
    if (!pc_algo_layout_parse(layout_s, &layout)) {
        return pc_engine_err_bad_params(
            err, cmd, "unknown layout `%s` (auto|perspective|cylindrical|spherical|collage|reposition)",
            layout_s);
    }
    bool blend = bool_param(p, "blend", true);
    bool vignette = bool_param(p, "vignetteRemoval", false);
    bool geometric = bool_param(p, "geometricCorrection", false);
    bool fill = bool_param(p, "contentAwareFill", false);

    source *srcs = NULL;
    size_t nsrcs = 0;
    pc_pixel_format fmt;
    pc_blob *icc = NULL;
    pc_status rc = load_sources(s, p, cmd, &srcs, &nsrcs, &fmt, &icc, err);
    if (rc != PC_OK) {
        return rc;
    }
    if (nsrcs < 2) {
        sources_free(srcs, nsrcs);
        pc_blob_release(icc);
        return pc_engine_err_bad_params(err, cmd, "Photomerge needs two or more images");
    }

    double focal_param = 0.0;
    bool has_focal_param = false;
    {
        double fp;
        yyjson_val *fv = pc_json_get(p, "focalLength");
        if (fv != NULL && pc_json_as_f64(fv, &fp) && fp > 0.0) {
            has_focal_param = true;
            focal_param = fp;
        }
    }

    pc_algo_alignment al;
    memset(&al, 0, sizeof al);
    bool have_al = false;
    pc_engine_photo_image *images = NULL;
    size_t *placed = NULL;
    const pc_algo_placement **placements = NULL;
    size_t nplaced = 0;
    pc_surface *warped = NULL;
    size_t nwarped = 0;
    size_t *order = NULL;
    pc_algo_placement *geo_pl = NULL;
    size_t (*geo_sizes)[2] = NULL;
    pc_engine_photo_blended b;
    memset(&b, 0, sizeof b);
    bool have_b = false;
    pc_document doc;
    memset(&doc, 0, sizeof doc);
    bool have_doc = false;
    double reg_ms = 0.0, warp_ms = 0.0;

    double focal35 = 0.0;
    bool has_focal35 = has_focal_param;
    if (has_focal_param) {
        focal35 = focal_param;
    } else {
        has_focal35 = exif_focal(srcs, nsrcs, &focal35);
    }

    images = pc_malloc_array(nsrcs, sizeof *images);
    if (images == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t i = 0; i < nsrcs; i++) {
        images[i].surf = &srcs[i].surf;
        images[i].rect = pc_rect_make(0, 0, (int32_t)srcs[i].w, (int32_t)srcs[i].h);
    }
    bool found = false;
    rc = pc_engine_photo_register(images, nsrcs, layout, false, 0, geometric, has_focal35, focal35,
                                  &found, &al, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    if (!found) {
        rc = pc_engine_err_other(
            err, "Photomerge couldn't find enough matching detail between the images");
        goto cleanup;
    }
    have_al = true;
    reg_ms = now_ms() - t0;

    placed = pc_malloc_array(nsrcs, sizeof *placed);
    placements = pc_malloc_array(nsrcs, sizeof *placements);
    if (placed == NULL || placements == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t i = 0; i < nsrcs && i < al.n; i++) {
        if (al.placed[i]) {
            placements[nplaced] = &al.placements[i];
            placed[nplaced] = i;
            nplaced++;
        }
    }
    if (nplaced < 1) {
        rc = pc_engine_err_other(
            err, "Photomerge couldn't find enough matching detail between the images");
        goto cleanup;
    }

    /* Canvas: union of the placed images. */
    double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (size_t kk = 0; kk < nplaced; kk++) {
        double bb[4];
        pc_algo_placement_bounds(placements[kk], (double)srcs[placed[kk]].w,
                                 (double)srcs[placed[kk]].h, bb);
        if (bb[0] < x0) {
            x0 = bb[0];
        }
        if (bb[1] < y0) {
            y0 = bb[1];
        }
        if (bb[2] > x1) {
            x1 = bb[2];
        }
        if (bb[3] > y1) {
            y1 = bb[3];
        }
    }
    double cwd = ceil(x1 - x0), chd = ceil(y1 - y0);
    if (!(isfinite(cwd) && isfinite(chd)) || cwd > 30000.0 || chd > 30000.0 || cwd < 1.0 ||
        chd < 1.0 || cwd * chd > 6.0e8) {
        rc = pc_engine_err_other(err, "the panorama would be %g\xC3\x97%g px; try another layout", cwd,
                                 chd);
        goto cleanup;
    }
    uint32_t cw = (uint32_t)cwd, ch = (uint32_t)chd;
    pc_rect canvas = pc_rect_make(0, 0, (int32_t)cw, (int32_t)ch);
    double off_x = -floor(x0), off_y = -floor(y0);

    warped = pc_calloc_array(nplaced, sizeof *warped);
    if (warped == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t kk = 0; kk < nplaced; kk++) {
        size_t i = placed[kk];
        rc = pc_engine_photo_warp_placed(&srcs[i].surf,
                                         pc_rect_make(0, 0, (int32_t)srcs[i].w, (int32_t)srcs[i].h),
                                         0.0, 0.0, placements[kk], off_x, off_y, PC_ALGO_INTERP_BICUBIC,
                                         &warped[nwarped], err);
        if (rc != PC_OK) {
            goto cleanup;
        }
        nwarped++;
    }
    warp_ms = now_ms() - t0 - reg_ms;

    /* The reference's position within the placed list (0 if it wasn't placed). */
    size_t ref_pos = 0;
    for (size_t kk = 0; kk < nplaced; kk++) {
        if (placed[kk] == al.reference) {
            ref_pos = kk;
            break;
        }
    }

    size_t ndocs = 0;
    (void)pc_engine_session_documents(s, &ndocs);
    char doc_name[64];
    (void)snprintf(doc_name, sizeof doc_name, "Untitled_Panorama%zu", ndocs + 1);
    rc = pc_document_init(&doc, doc_name, pc_size_make(cw, ch), fmt.mode, fmt.sample, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    have_doc = true;
    doc.icc_profile = icc; /* move ownership into the document */
    icc = NULL;

    if (blend) {
        order = pc_malloc_array(nplaced, sizeof *order);
        if (order == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            goto cleanup;
        }
        pc_engine_photo_seam_order(warped, nplaced, ref_pos, order);
        if (vignette) {
            geo_pl = pc_malloc_array(nplaced, sizeof *geo_pl);
            geo_sizes = pc_malloc_array(nplaced, sizeof *geo_sizes);
            if (geo_pl == NULL || geo_sizes == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, cmd);
                goto cleanup;
            }
            for (size_t kk = 0; kk < nplaced; kk++) {
                geo_pl[kk] = *placements[kk];
                geo_sizes[kk][0] = srcs[placed[kk]].w;
                geo_sizes[kk][1] = srcs[placed[kk]].h;
            }
        }
        rc = pc_engine_photo_seam_blend(warped, nplaced, canvas, order, true, true, vignette, geo_pl,
                                        (const size_t (*)[2])geo_sizes, off_x, off_y, &b, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
        have_b = true;
        for (size_t kk = 0; kk < nplaced; kk++) {
            pc_layer nl;
            rc = pc_layer_raster(&nl, srcs[placed[kk]].name, fmt, err);
            if (rc != PC_OK) {
                goto cleanup;
            }
            pc_surface *ls = pc_layer_surface_mut(&nl);
            if (ls != NULL) {
                pc_surface_fini(ls);
                rc = pc_surface_clone(&b.layer_px[kk], ls, err);
                if (rc != PC_OK) {
                    pc_layer_fini(&nl);
                    goto cleanup;
                }
            }
            pc_layer_mask_hide_all(&nl.mask);
            pc_surface_fini(&nl.mask.surface);
            rc = pc_surface_clone(&b.layer_mask[kk], &nl.mask.surface, err);
            if (rc != PC_OK) {
                pc_layer_fini(&nl);
                goto cleanup;
            }
            nl.has_mask = true;
            pc_layer_id nid = 0;
            rc = pc_document_insert_above(&doc, NULL, &nl, &nid, err);
            if (rc != PC_OK) {
                pc_layer_fini(&nl);
                goto cleanup;
            }
        }
        if (fill) {
            pc_surface filled;
            rc = content_aware_fill(&b.composite, canvas, &filled, err);
            if (rc != PC_OK) {
                goto cleanup;
            }
            pc_layer nl;
            rc = pc_layer_raster(&nl, "Content-Aware Fill", fmt, err);
            if (rc != PC_OK) {
                pc_surface_fini(&filled);
                goto cleanup;
            }
            pc_surface *ls = pc_layer_surface_mut(&nl);
            if (ls != NULL) {
                pc_surface_fini(ls);
                *ls = filled; /* move */
            } else {
                pc_surface_fini(&filled);
            }
            pc_layer_id nid = 0;
            rc = pc_document_insert_above(&doc, NULL, &nl, &nid, err);
            if (rc != PC_OK) {
                pc_layer_fini(&nl);
                goto cleanup;
            }
        }
    } else {
        for (size_t kk = 0; kk < nplaced; kk++) {
            pc_layer nl;
            rc = pc_layer_raster(&nl, srcs[placed[kk]].name, fmt, err);
            if (rc != PC_OK) {
                goto cleanup;
            }
            pc_surface *ls = pc_layer_surface_mut(&nl);
            if (ls != NULL) {
                pc_surface_fini(ls);
                rc = pc_surface_clone(&warped[kk], ls, err);
                if (rc != PC_OK) {
                    pc_layer_fini(&nl);
                    goto cleanup;
                }
            }
            pc_layer_id nid = 0;
            rc = pc_document_insert_above(&doc, NULL, &nl, &nid, err);
            if (rc != PC_OK) {
                pc_layer_fini(&nl);
                goto cleanup;
            }
        }
    }

    size_t nlayers = doc.nlayers;

    /* Collect result fields referring to the source array before the document is moved. */
    size_t max_side = 1;
    for (size_t i = 0; i < nsrcs; i++) {
        size_t m = srcs[i].w > srcs[i].h ? srcs[i].w : srcs[i].h;
        if (m > max_side) {
            max_side = m;
        }
    }
    double focal35_out = al.focal / (double)max_side * 36.0;
    double al_k1 = al.k1, al_rms = al.rms;
    size_t ref_idx = al.reference;

    size_t idx = 0;
    rc = pc_engine_session_add_document(s, &doc, NULL, &idx, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    have_doc = false; /* moved into the session */

    /* ---- result JSON ---- */
    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto cleanup;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "document", idx);
    yyjson_mut_obj_add_str(rd, root, "layout", pc_algo_layout_name(al.layout));
    if (ref_idx < nsrcs) {
        yyjson_mut_obj_add_str(rd, root, "reference", srcs[ref_idx].name);
    } else {
        yyjson_mut_obj_add_val(rd, root, "reference", yyjson_mut_null(rd));
    }
    yyjson_mut_obj_add_real(rd, root, "focalLength35", focal35_out);
    yyjson_mut_obj_add_real(rd, root, "geometricK1", al_k1);
    yyjson_mut_obj_add_real(rd, root, "rms", al_rms);
    yyjson_mut_val *pairs = yyjson_mut_arr(rd);
    for (size_t i = 0; i < al.npairs; i++) {
        yyjson_mut_val *pr = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_uint(rd, pr, al.pairs[i].a);
        yyjson_mut_arr_add_uint(rd, pr, al.pairs[i].b);
        yyjson_mut_arr_add_uint(rd, pr, al.pairs[i].inliers);
        yyjson_mut_arr_add_val(pairs, pr);
    }
    yyjson_mut_obj_add_val(rd, root, "pairs", pairs);
    yyjson_mut_val *placed_arr = yyjson_mut_arr(rd);
    for (size_t kk = 0; kk < nplaced; kk++) {
        yyjson_mut_arr_add_str(rd, placed_arr, srcs[placed[kk]].name);
    }
    yyjson_mut_obj_add_val(rd, root, "placed", placed_arr);
    yyjson_mut_val *failed_arr = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nsrcs && i < al.n; i++) {
        if (!al.placed[i]) {
            yyjson_mut_arr_add_str(rd, failed_arr, srcs[i].name);
        }
    }
    yyjson_mut_obj_add_val(rd, root, "failed", failed_arr);
    yyjson_mut_obj_add_uint(rd, root, "width", cw);
    yyjson_mut_obj_add_uint(rd, root, "height", ch);
    yyjson_mut_obj_add_uint(rd, root, "layers", nlayers);
    yyjson_mut_val *photo = yyjson_mut_obj(rd);
    if (have_b && b.gains != NULL) {
        yyjson_mut_val *gains = yyjson_mut_arr(rd);
        for (size_t kk = 0; kk < nplaced; kk++) {
            yyjson_mut_val *g = yyjson_mut_arr(rd);
            for (size_t c = 0; c < b.gains_ch; c++) {
                yyjson_mut_arr_add_real(rd, g, (double)b.gains[kk * b.gains_ch + c]);
            }
            yyjson_mut_arr_add_val(gains, g);
        }
        yyjson_mut_obj_add_val(rd, photo, "gains", gains);
        yyjson_mut_val *vig = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_real(rd, vig, b.vignette[0]);
        yyjson_mut_arr_add_real(rd, vig, b.vignette[1]);
        yyjson_mut_obj_add_val(rd, photo, "vignette", vig);
    }
    yyjson_mut_obj_add_val(rd, root, "photometric", photo);
    yyjson_mut_val *ms = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_real(rd, ms, "register", reg_ms);
    yyjson_mut_obj_add_real(rd, ms, "warp", warp_ms);
    yyjson_mut_obj_add_real(rd, ms, "total", now_ms() - t0);
    yyjson_mut_obj_add_val(rd, root, "ms", ms);
    yyjson_mut_doc_set_root(rd, root);

cleanup:
    if (have_doc) {
        pc_document_fini(&doc);
    }
    if (have_b) {
        pc_engine_photo_blended_fini(&b);
    }
    if (warped != NULL) {
        for (size_t kk = 0; kk < nwarped; kk++) {
            pc_surface_fini(&warped[kk]);
        }
        free(warped);
    }
    if (have_al) {
        pc_algo_alignment_fini(&al);
    }
    free(order);
    free(geo_pl);
    free(geo_sizes);
    free(images);
    free(placed);
    free(placements);
    pc_blob_release(icc);
    sources_free(srcs, nsrcs);
    return rc;
}

/* ---- Merge to HDR Pro --------------------------------------------------------------------- */

/* Shifts straight-RGBA pixels by (dx, dy), clamping at the edges (photo_cmds::shift_rgba). */
static void shift_rgba(const float (*px)[4], float (*out)[4], size_t w, size_t h, int32_t dx, int32_t dy)
{
    for (size_t i = 0; i < w * h; i++) {
        int32_t x = (int32_t)(i % w) - dx;
        int32_t y = (int32_t)(i / w) - dy;
        if (x < 0) {
            x = 0;
        } else if (x > (int32_t)w - 1) {
            x = (int32_t)w - 1;
        }
        if (y < 0) {
            y = 0;
        } else if (y > (int32_t)h - 1) {
            y = (int32_t)h - 1;
        }
        memcpy(out[i], px[(size_t)y * w + (size_t)x], sizeof(float[4]));
    }
}

static pc_status run_merge_to_hdr(pc_engine_session *s, yyjson_val *p, pc_json_value *out, pc_error *err)
{
    const char *cmd = "file.automate.mergeToHdrPro";
    double t0 = now_ms();

    source *srcs = NULL;
    size_t nsrcs = 0;
    pc_pixel_format sfmt;
    pc_blob *icc = NULL;
    pc_status rc = load_sources(s, p, cmd, &srcs, &nsrcs, &sfmt, &icc, err);
    if (rc != PC_OK) {
        return rc;
    }

    float (**imgs)[4] = NULL;
    double *exposures = NULL;
    int32_t (*offsets)[2] = NULL;
    size_t *order = NULL;
    pc_algo_hdr_img *hdr_imgs = NULL;
    pc_algo_hdr_merged merged;
    memset(&merged, 0, sizeof merged);
    bool have_merged = false;
    float (*px)[4] = NULL;
    pc_document doc;
    memset(&doc, 0, sizeof doc);
    bool have_doc = false;
    const char *source_kind; /* set below: "params" / "exif" / "estimated" */

    if (nsrcs < 2) {
        rc = pc_engine_err_bad_params(err, cmd, "Merge to HDR Pro needs two or more exposures");
        goto cleanup;
    }
    size_t w = srcs[0].w, h = srcs[0].h;
    for (size_t i = 0; i < nsrcs; i++) {
        if (srcs[i].w != w || srcs[i].h != h) {
            rc = pc_engine_err_other(err, "the exposures must have the same pixel dimensions");
            goto cleanup;
        }
    }
    if (w == 0 || h == 0) {
        rc = pc_engine_err_other(err, "the exposures must have the same pixel dimensions");
        goto cleanup;
    }
    pc_rect area = pc_rect_make(0, 0, (int32_t)w, (int32_t)h);

    imgs = pc_calloc_array(nsrcs, sizeof *imgs);
    exposures = pc_malloc_array(nsrcs, sizeof *exposures);
    offsets = pc_calloc_array(nsrcs, sizeof *offsets);
    if (imgs == NULL || exposures == NULL || offsets == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t i = 0; i < nsrcs; i++) {
        imgs[i] = pc_malloc_array(w * h, sizeof(float[4]));
        if (imgs[i] == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            goto cleanup;
        }
        rc = pc_surface_read_rgba_into(&srcs[i].surf, area, imgs[i], w * h, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
    }

    /* Exposures: explicit EV, EXIF, or estimated. */
    yyjson_val *ev_arr = pc_json_get(p, "exposures");
    if (ev_arr != NULL && yyjson_is_arr(ev_arr)) {
        if (yyjson_arr_size(ev_arr) != nsrcs) {
            rc = pc_engine_err_bad_params(err, cmd, "`exposures` needs one EV value per image");
            goto cleanup;
        }
        size_t idx = 0, max;
        yyjson_val *e;
        yyjson_arr_foreach(ev_arr, idx, max, e) {
            double ev;
            if (!pc_json_as_f64(e, &ev)) {
                ev = 0.0;
            }
            exposures[idx] = pow(2.0, ev);
        }
        source_kind = "params";
    } else {
        bool all_exif = true;
        for (size_t i = 0; i < nsrcs; i++) {
            if (!srcs[i].has_exp) {
                all_exif = false;
                break;
            }
        }
        if (all_exif) {
            for (size_t i = 0; i < nsrcs; i++) {
                exposures[i] = srcs[i].exp_factor;
            }
            source_kind = "exif";
        } else {
            pc_algo_hdr_img *ref = pc_malloc_array(nsrcs, sizeof *ref);
            if (ref == NULL) {
                rc = pc_fail(err, PC_ERR_NOMEM, cmd);
                goto cleanup;
            }
            for (size_t i = 0; i < nsrcs; i++) {
                ref[i].px = (const float (*)[4])imgs[i];
                ref[i].n = w * h;
            }
            rc = pc_algo_hdr_estimate_exposures(ref, nsrcs, exposures, err);
            free(ref);
            if (rc != PC_OK) {
                goto cleanup;
            }
            source_kind = "estimated";
        }
    }
    for (size_t i = 0; i < nsrcs; i++) {
        if (!(isfinite(exposures[i]) && exposures[i] > 0.0)) {
            rc = pc_engine_err_bad_params(err, cmd, "exposures must be positive");
            goto cleanup;
        }
    }

    /* order: ascending by exposure factor (stable). */
    order = pc_malloc_array(nsrcs, sizeof *order);
    if (order == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t i = 0; i < nsrcs; i++) {
        order[i] = i;
    }
    for (size_t i = 1; i < nsrcs; i++) {
        size_t key = order[i];
        size_t j = i;
        while (j > 0 && exposures[order[j - 1]] > exposures[key]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    size_t mid = order[nsrcs / 2];

    /* Ward's MTB alignment to the middle exposure, on a downsampled grey copy. */
    if (bool_param(p, "align", true)) {
        size_t k = (w > h ? w : h);
        k = (k + 1535) / 1536;
        if (k < 1) {
            k = 1;
        }
        size_t sw = (w + k - 1) / k, sh = (h + k - 1) / k;
        float *gref = pc_calloc_array(sw * sh, sizeof *gref);
        float *gi = pc_calloc_array(sw * sh, sizeof *gi);
        float *gc = pc_calloc_array(sw * sh, sizeof *gc);
        if (gref == NULL || gi == NULL || gc == NULL) {
            free(gref);
            free(gi);
            free(gc);
            rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            goto cleanup;
        }
        /* grey(mid) into gref. */
        for (size_t c = 0; c < sw * sh; c++) {
            gc[c] = 0.0f;
        }
        for (size_t i = 0; i < w * h; i++) {
            size_t j = (i / w / k) * sw + (i % w) / k;
            const float *q = imgs[mid][i];
            gref[j] += (54.0f * q[0] + 183.0f * q[1] + 19.0f * q[2]) / 256.0f;
            gc[j] += 1.0f;
        }
        for (size_t c = 0; c < sw * sh; c++) {
            gref[c] /= (gc[c] > 1.0f ? gc[c] : 1.0f);
        }
        for (size_t i = 0; i < nsrcs; i++) {
            if (i == mid) {
                continue;
            }
            for (size_t c = 0; c < sw * sh; c++) {
                gi[c] = 0.0f;
                gc[c] = 0.0f;
            }
            for (size_t q = 0; q < w * h; q++) {
                size_t j = (q / w / k) * sw + (q % w) / k;
                const float *pxq = imgs[i][q];
                gi[j] += (54.0f * pxq[0] + 183.0f * pxq[1] + 19.0f * pxq[2]) / 256.0f;
                gc[j] += 1.0f;
            }
            for (size_t c = 0; c < sw * sh; c++) {
                gi[c] /= (gc[c] > 1.0f ? gc[c] : 1.0f);
            }
            int32_t dx = 0, dy = 0;
            rc = pc_algo_hdr_mtb_offset(sw, sh, gref, gi, 5, &dx, &dy, err);
            if (rc != PC_OK) {
                free(gref);
                free(gi);
                free(gc);
                goto cleanup;
            }
            offsets[i][0] = dx * (int32_t)k;
            offsets[i][1] = dy * (int32_t)k;
            if (offsets[i][0] != 0 || offsets[i][1] != 0) {
                float (*shifted)[4] = pc_malloc_array(w * h, sizeof(float[4]));
                if (shifted == NULL) {
                    free(gref);
                    free(gi);
                    free(gc);
                    rc = pc_fail(err, PC_ERR_NOMEM, cmd);
                    goto cleanup;
                }
                shift_rgba((const float (*)[4])imgs[i], shifted, w, h, offsets[i][0], offsets[i][1]);
                free(imgs[i]);
                imgs[i] = shifted;
            }
        }
        free(gref);
        free(gi);
        free(gc);
    }

    bool has_ghost_base = false;
    size_t ghost_base = 0;
    {
        yyjson_val *gv = pc_json_get(p, "ghostBase");
        uint64_t g;
        if (gv != NULL && pc_json_as_u64(gv, &g)) {
            if (g >= nsrcs) {
                rc = pc_engine_err_bad_params(err, cmd, "`ghostBase` is an image index");
                goto cleanup;
            }
            has_ghost_base = true;
            ghost_base = (size_t)g;
        }
    }

    hdr_imgs = pc_malloc_array(nsrcs, sizeof *hdr_imgs);
    if (hdr_imgs == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    for (size_t i = 0; i < nsrcs; i++) {
        hdr_imgs[i].px = (const float (*)[4])imgs[i];
        hdr_imgs[i].n = w * h;
    }
    pc_algo_hdr_merge_opts mopts;
    memset(&mopts, 0, sizeof mopts);
    mopts.exposures = exposures;
    mopts.n_exposures = nsrcs;
    mopts.remove_ghosts = bool_param(p, "removeGhosts", false);
    mopts.has_ghost_base = has_ghost_base;
    mopts.ghost_base = ghost_base;
    mopts.response = NULL;
    rc = pc_algo_hdr_merge(w, h, hdr_imgs, nsrcs, &mopts, &merged, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    have_merged = true;

    /* Output mode: "32" (linear) or "16" / "8" (tone-mapped). */
    char mode_buf[16];
    const char *mode;
    {
        yyjson_val *mv = pc_json_get(p, "mode");
        size_t slen = 0;
        const char *ms_str = mv != NULL ? pc_json_as_str(mv, &slen) : NULL;
        if (ms_str != NULL) {
            size_t n = slen < sizeof mode_buf - 1 ? slen : sizeof mode_buf - 1;
            memcpy(mode_buf, ms_str, n);
            mode_buf[n] = '\0';
            mode = mode_buf;
        } else {
            uint64_t mn;
            if (mv != NULL && pc_json_as_u64(mv, &mn)) {
                (void)snprintf(mode_buf, sizeof mode_buf, "%llu", (unsigned long long)mn);
                mode = mode_buf;
            } else {
                mode = "32";
            }
        }
    }

    pc_sample_type depth;
    px = pc_malloc_array(w * h, sizeof(float[4]));
    if (px == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, cmd);
        goto cleanup;
    }
    memcpy(px, merged.px, w * h * sizeof(float[4]));
    if (strcmp(mode, "32") == 0) {
        depth = PC_SAMPLE_TYPE_F32;
    } else if (strcmp(mode, "16") == 0 || strcmp(mode, "8") == 0) {
        const char *method = str_param(p, "method", "localAdaptation");
        pc_algo_hdr_tone_method tm;
        memset(&tm, 0, sizeof tm);
        if (strcmp(method, "localAdaptation") == 0) {
            tm.kind = PC_ALGO_HDR_LOCAL_ADAPTATION;
            tm.local.radius = clampf(f_param(p, "radius", 7.0), 1.0f, 500.0f);
            tm.local.strength = clampf(f_param(p, "strength", 0.52), 0.1f, 4.0f);
            tm.local.gamma = clampf(f_param(p, "gamma", 1.0), 0.1f, 2.0f);
            tm.local.exposure = clampf(f_param(p, "exposure", 0.0), -5.0f, 5.0f);
            tm.local.detail = clampf(f_param(p, "detail", 30.0), -100.0f, 300.0f);
            tm.local.shadow = clampf(f_param(p, "shadow", 0.0), -100.0f, 100.0f);
            tm.local.highlight = clampf(f_param(p, "highlight", 0.0), -100.0f, 100.0f);
            tm.local.vibrance = clampf(f_param(p, "vibrance", 0.0), -100.0f, 100.0f);
            tm.local.saturation = clampf(f_param(p, "saturation", 20.0), -100.0f, 100.0f);
            tm.local.curve = NULL;
            tm.local.curve_len = 0;
        } else if (strcmp(method, "exposureGamma") == 0) {
            tm.kind = PC_ALGO_HDR_EXPOSURE_GAMMA;
            tm.exposure = f_param(p, "exposure", 0.0);
            tm.gamma = f_param(p, "gamma", 1.0);
        } else if (strcmp(method, "highlightCompression") == 0) {
            tm.kind = PC_ALGO_HDR_HIGHLIGHT_COMPRESSION;
        } else if (strcmp(method, "equalizeHistogram") == 0) {
            tm.kind = PC_ALGO_HDR_EQUALIZE_HISTOGRAM;
        } else {
            rc = pc_engine_err_bad_params(
                err, cmd,
                "unknown method `%s` (localAdaptation|exposureGamma|highlightCompression|"
                "equalizeHistogram)",
                method);
            goto cleanup;
        }
        rc = pc_algo_hdr_tone_map(px, w * h, w, h, &tm, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
        depth = strcmp(mode, "16") == 0 ? PC_SAMPLE_TYPE_U16 : PC_SAMPLE_TYPE_U8;
    } else {
        rc = pc_engine_err_bad_params(err, cmd, "`mode` must be 32|16|8, not %s", mode);
        goto cleanup;
    }
    for (size_t i = 0; i < w * h; i++) {
        px[i][3] = 1.0f;
    }

    size_t ndocs = 0;
    (void)pc_engine_session_documents(s, &ndocs);
    char doc_name[64];
    (void)snprintf(doc_name, sizeof doc_name, "Untitled_HDR%zu", ndocs + 1);
    rc = pc_document_init(&doc, doc_name, pc_size_make((uint32_t)w, (uint32_t)h), PC_COLOR_MODE_RGB,
                          depth, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    have_doc = true;
    if (depth == PC_SAMPLE_TYPE_F32) {
        const pc_profile *lin = pc_builtin_profile(PC_BUILTIN_LINEAR_SRGB);
        if (lin != NULL) {
            pc_buf pb = PC_BUF_INIT;
            if (pc_profile_to_bytes(lin, &pb, NULL) == PC_OK) {
                pc_blob *blob = NULL;
                if (pc_blob_new(pb.data, pb.len, &blob, NULL) == PC_OK) {
                    doc.icc_profile = blob;
                }
            }
            pc_buf_fini(&pb);
        }
    } else {
        doc.icc_profile = icc;
        icc = NULL;
    }
    {
        pc_pixel_format fmt = pc_document_pixel_format(&doc);
        size_t nch = pc_pixel_format_channels(fmt);
        float *data = pc_calloc_array(w * h * nch, sizeof *data);
        if (data == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, cmd);
            goto cleanup;
        }
        for (size_t i = 0; i < w * h; i++) {
            float tmp[PC_RASTER_MAX_CHANNELS];
            pc_from_rgba(fmt, px[i], tmp);
            for (size_t c = 0; c < nch; c++) {
                data[i * nch + c] = tmp[c];
            }
        }
        pc_surface surf;
        pc_surface_init(&surf, fmt);
        rc = pc_surface_write_region(&surf, area, data, w * h * nch, err);
        free(data);
        if (rc != PC_OK) {
            pc_surface_fini(&surf);
            goto cleanup;
        }
        pc_surface_prune(&surf);
        pc_layer bg;
        rc = pc_layer_raster(&bg, "Background", fmt, err);
        if (rc != PC_OK) {
            pc_surface_fini(&surf);
            goto cleanup;
        }
        pc_surface *bs = pc_layer_surface_mut(&bg);
        if (bs != NULL) {
            pc_surface_fini(bs);
            *bs = surf; /* move */
        } else {
            pc_surface_fini(&surf);
        }
        pc_layer_id nid = 0;
        rc = pc_document_insert_above(&doc, NULL, &bg, &nid, err);
        if (rc != PC_OK) {
            pc_layer_fini(&bg);
            goto cleanup;
        }
    }

    double ghost_fraction = merged.ghost_fraction, stops = merged.stops;
    size_t merged_ghost_base = merged.ghost_base;
    double ev0 = exposures[order[0]];

    size_t idx = 0;
    rc = pc_engine_session_add_document(s, &doc, NULL, &idx, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    have_doc = false;

    yyjson_mut_doc *rd = NULL;
    if (pc_engine_result_doc(out, &rd, err) != PC_OK) {
        rc = PC_ERR_NOMEM;
        goto cleanup;
    }
    yyjson_mut_val *root = yyjson_mut_obj(rd);
    yyjson_mut_obj_add_uint(rd, root, "document", idx);
    yyjson_mut_obj_add_str(rd, root, "mode", mode);
    yyjson_mut_obj_add_str(rd, root, "exposureSource", source_kind);
    yyjson_mut_val *ev = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nsrcs; i++) {
        yyjson_mut_arr_add_real(rd, ev, log2(exposures[i] / ev0));
    }
    yyjson_mut_obj_add_val(rd, root, "ev", ev);
    yyjson_mut_val *offs = yyjson_mut_arr(rd);
    for (size_t i = 0; i < nsrcs; i++) {
        yyjson_mut_val *o = yyjson_mut_arr(rd);
        yyjson_mut_arr_add_int(rd, o, offsets[i][0]);
        yyjson_mut_arr_add_int(rd, o, offsets[i][1]);
        yyjson_mut_arr_add_val(offs, o);
    }
    yyjson_mut_obj_add_val(rd, root, "offsets", offs);
    yyjson_mut_obj_add_uint(rd, root, "ghostBase", merged_ghost_base);
    yyjson_mut_obj_add_real(rd, root, "ghostFraction", ghost_fraction);
    yyjson_mut_obj_add_real(rd, root, "stops", stops);
    yyjson_mut_obj_add_real(rd, root, "ms", now_ms() - t0);
    yyjson_mut_doc_set_root(rd, root);

cleanup:
    if (have_doc) {
        pc_document_fini(&doc);
    }
    if (have_merged) {
        pc_algo_hdr_merged_fini(&merged);
    }
    free(px);
    free(hdr_imgs);
    free(order);
    free(offsets);
    free(exposures);
    if (imgs != NULL) {
        for (size_t i = 0; i < nsrcs; i++) {
            free(imgs[i]);
        }
        free(imgs);
    }
    pc_blob_release(icc);
    sources_free(srcs, nsrcs);
    return rc;
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
