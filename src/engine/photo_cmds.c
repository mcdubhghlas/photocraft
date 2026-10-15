/* Computational-photography registration and blending primitives (port of the shared helpers in
 * crates/engine/src/photo_cmds.rs). See photo_cmds.h.
 *
 * These are the pure functions Edit > Auto-Align / Auto-Blend Layers (align_cmds.c) and -- once E3
 * lands file I/O -- File > Automate > Photomerge build on. The Photomerge / Merge-to-HDR /
 * Crop-and-Straighten COMMANDS load files and create documents, so they are deferred to E3; this
 * module registers no commands. Numeric parity is via photocraft_algo (bit-exact). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/direct.h"
#include "algo/panorama.h"
#include "algo/transform.h"
#include "algo/warp.h"
#include "base/mem.h"
#include "base/status.h"
#include "color/color.h"
#include "engine_internal.h"
#include "geom/geom.h"
#include "photo_cmds.h"
#include "raster/pixel.h"
#include "raster/surface.h"

const char *const PC_ENGINE_PHOTO_NO_DETAIL =
    "Auto-Align couldn't find enough matching detail between the layers";

/* A layer and its neighbour must share at least this fraction of the plane at the pose found. */
#define DIRECT_MIN_OVERLAP 0.25
/* The reference and warped layer must correlate at least this much. */
#define DIRECT_MIN_CORRELATION 0.5

static size_t div_ceil_max1(size_t a, size_t b)
{
    if (b == 0) {
        return 1;
    }
    size_t q = (a + b - 1) / b;
    return q < 1 ? 1 : q;
}

/* ---- luma_alpha --------------------------------------------------------------------------- */

pc_status pc_engine_photo_luma_alpha(const pc_surface *surf, pc_rect area, size_t k, size_t *out_sw,
                                     size_t *out_sh, float **out_l, float **out_a, pc_error *err)
{
    if (k < 1) {
        k = 1;
    }
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_surface_channels(surf);
    size_t w = pc_rect_width(area);
    size_t h = pc_rect_height(area);
    size_t sw = div_ceil_max1(w, k);
    size_t sh = div_ceil_max1(h, k);
    size_t cells = sw * sh;
    float *l = pc_calloc_array(cells, sizeof *l);
    float *a = pc_calloc_array(cells, sizeof *a);
    float *cnt = pc_calloc_array(cells, sizeof *cnt);
    if (l == NULL || a == NULL || cnt == NULL) {
        free(l);
        free(a);
        free(cnt);
        return pc_fail(err, PC_ERR_NOMEM, "luma_alpha");
    }
    for (size_t y = 0; y < h; y++) {
        pc_rect rr = pc_rect_make(area.x0, area.y0 + (int32_t)y, area.x1, area.y0 + (int32_t)y + 1);
        float *row = NULL;
        size_t rlen = 0;
        if (pc_surface_read_region(surf, rr, &row, &rlen, err) != PC_OK) {
            free(l);
            free(a);
            free(cnt);
            return err != NULL ? err->code : PC_ERR_NOMEM;
        }
        for (size_t x = 0; x < w; x++) {
            float p[4];
            pc_to_rgba(fmt, row + x * n, n, p);
            size_t i = (y / k) * sw + x / k;
            l[i] += (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) * p[3];
            a[i] += p[3];
            cnt[i] += 1.0f;
        }
        free(row);
    }
    for (size_t i = 0; i < cells; i++) {
        float c = cnt[i] > 1.0f ? cnt[i] : 1.0f;
        l[i] = a[i] > 0.0f ? l[i] / a[i] : 0.0f;
        a[i] /= c;
    }
    free(cnt);
    *out_sw = sw;
    *out_sh = sh;
    *out_l = l;
    *out_a = a;
    return PC_OK;
}

/* Builds, where coverage drops below one half anywhere, a validity mask (a[i] > 0.5); else NULL. */
static bool *valid_mask(const float *a, size_t cells)
{
    bool any = false;
    for (size_t i = 0; i < cells; i++) {
        if (a[i] < 0.5f) {
            any = true;
            break;
        }
    }
    if (!any) {
        return NULL;
    }
    bool *v = pc_malloc_array(cells, sizeof *v);
    if (v == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < cells; i++) {
        v[i] = a[i] > 0.5f;
    }
    return v;
}

/* ---- feature registration ----------------------------------------------------------------- */

pc_status pc_engine_photo_register(const pc_engine_photo_image *images, size_t n, pc_algo_layout layout,
                                   bool has_ref, size_t reference, bool geometric, bool has_focal35,
                                   double focal35, bool *found, pc_algo_alignment *out, pc_error *err)
{
    *found = false;
    if (n == 0) {
        return PC_OK;
    }
    size_t side = 1;
    for (size_t i = 0; i < n; i++) {
        size_t w = pc_rect_width(images[i].rect);
        size_t h = pc_rect_height(images[i].rect);
        size_t m = w > h ? w : h;
        if (m > side) {
            side = m;
        }
    }
    size_t k = div_ceil_max1(side, PC_ENGINE_PHOTO_REGISTER_SIDE);

    pc_algo_prepared_image *prepared = pc_calloc_array(n, sizeof *prepared);
    float **lbuf = pc_calloc_array(n, sizeof *lbuf);
    float **abuf = pc_calloc_array(n, sizeof *abuf);
    bool **vbuf = pc_calloc_array(n, sizeof *vbuf);
    pc_algo_image_features *feats = pc_calloc_array(n, sizeof *feats);
    if (prepared == NULL || lbuf == NULL || abuf == NULL || vbuf == NULL || feats == NULL) {
        free(prepared);
        free(lbuf);
        free(abuf);
        free(vbuf);
        free(feats);
        return pc_fail(err, PC_ERR_NOMEM, "register");
    }
    pc_status rc = PC_OK;
    pc_algo_pair_match *matches = NULL;
    size_t nmatches = 0;
    bool feats_built = false;
    for (size_t i = 0; i < n; i++) {
        size_t sw = 0, sh = 0;
        rc = pc_engine_photo_luma_alpha(images[i].surf, images[i].rect, k, &sw, &sh, &lbuf[i], &abuf[i], err);
        if (rc != PC_OK) {
            goto cleanup;
        }
        vbuf[i] = valid_mask(abuf[i], sw * sh);
        prepared[i].w = sw;
        prepared[i].h = sh;
        prepared[i].luma = lbuf[i];
        prepared[i].valid = vbuf[i];
    }
    rc = pc_algo_panorama_detect_many(prepared, n, 800, feats, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    feats_built = true;
    rc = pc_algo_panorama_match_pairs(feats, n, &matches, &nmatches, err);
    if (rc != PC_OK) {
        goto cleanup;
    }

    pc_algo_align_options opts = {0};
    opts.layout = layout;
    if (has_focal35) {
        opts.has_focal = true;
        opts.focal = focal35 / 36.0 * (double)side / (double)k;
    }
    opts.has_reference = has_ref;
    opts.reference = reference;
    opts.geometric = geometric;

    bool got = false;
    pc_algo_alignment al;
    memset(&al, 0, sizeof al);
    rc = pc_algo_panorama_align(feats, n, matches, nmatches, &opts, &got, &al, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    if (!got) {
        goto cleanup; /* None: *found stays false */
    }
    double kf = (double)k;
    for (size_t i = 0; i < al.n; i++) {
        if (al.placed[i]) {
            al.placements[i] = pc_algo_placement_scaled(&al.placements[i], kf);
        }
    }
    al.focal *= kf;
    al.rms *= kf;
    *out = al;
    *found = true;

cleanup:
    if (matches != NULL) {
        pc_algo_pair_matches_free(matches, nmatches);
    }
    if (feats_built) {
        for (size_t i = 0; i < n; i++) {
            pc_algo_image_features_fini(&feats[i]);
        }
    }
    for (size_t i = 0; i < n; i++) {
        free(lbuf[i]);
        free(abuf[i]);
        free(vbuf[i]);
    }
    free(prepared);
    free(lbuf);
    free(abuf);
    free(vbuf);
    free(feats);
    return rc;
}

/* ---- intensity (direct) registration ------------------------------------------------------ */

static pc_algo_placement direct_placement(const pc_algo_homography *hm, size_t fw, size_t fh,
                                           const double center[2])
{
    pc_algo_homography to_center = {{1.0, 0.0, center[0], 0.0, 1.0, center[1], 0.0, 0.0, 1.0}};
    pc_algo_placement pl;
    pl.projection = PC_ALGO_PROJECTION_PLANE;
    pl.focal = (double)(fw > fh ? fw : fh);
    pl.center[0] = center[0];
    pl.center[1] = center[1];
    pl.k1 = 0.0;
    pl.h = pc_algo_homography_mul(hm, &to_center);
    return pl;
}

pc_status pc_engine_photo_register_direct(const pc_engine_photo_image *images, size_t n,
                                          pc_algo_layout layout, size_t reference,
                                          pc_algo_alignment *out, pc_error *err)
{
    pc_algo_motion motion;
    pc_algo_layout resolved;
    switch (layout) {
    case PC_ALGO_LAYOUT_REPOSITION:
        motion = PC_ALGO_MOTION_TRANSLATION;
        resolved = PC_ALGO_LAYOUT_REPOSITION;
        break;
    case PC_ALGO_LAYOUT_AUTO:
    case PC_ALGO_LAYOUT_COLLAGE:
        motion = PC_ALGO_MOTION_SIMILARITY;
        resolved = PC_ALGO_LAYOUT_COLLAGE;
        break;
    case PC_ALGO_LAYOUT_PERSPECTIVE:
        motion = PC_ALGO_MOTION_HOMOGRAPHY;
        resolved = PC_ALGO_LAYOUT_PERSPECTIVE;
        break;
    default:
        return pc_fail(err, PC_ERR_INVALID,
                       "the intensity registration supports the auto, perspective, collage and "
                       "reposition projections");
    }
    if (reference >= n) {
        return pc_fail(err, PC_ERR_INVALID, "no layers to align");
    }
    pc_rect area = images[reference].rect;
    uint32_t aw = pc_rect_width(area), ah = pc_rect_height(area);
    size_t fw = aw < 1 ? 1 : aw;
    size_t fh = ah < 1 ? 1 : ah;
    for (size_t i = 0; i < n; i++) {
        if (pc_rect_width(images[i].rect) != aw || pc_rect_height(images[i].rect) != ah) {
            return pc_fail(err, PC_ERR_INVALID, "the layers to align have different sizes");
        }
    }
    size_t k = div_ceil_max1(fw > fh ? fw : fh, PC_ENGINE_PHOTO_DIRECT_SIDE);
    size_t w = div_ceil_max1(fw, k);
    size_t h = div_ceil_max1(fh, k);
    size_t cells = w * h;
    size_t min_overlap = (size_t)(DIRECT_MIN_OVERLAP * (double)cells);

    float **planes = pc_calloc_array(n, sizeof *planes);
    bool **pvalid = pc_calloc_array(n, sizeof *pvalid);
    pc_algo_pose *poses = pc_calloc_array(n, sizeof *poses);
    bool *has_pose = pc_calloc_array(n, sizeof *has_pose);
    pc_algo_pair_inliers *pairs = pc_malloc_array(n > 0 ? n : 1, sizeof *pairs);
    pc_algo_placement *placements = pc_calloc_array(n, sizeof *placements);
    bool *placed = pc_calloc_array(n, sizeof *placed);
    if (planes == NULL || pvalid == NULL || poses == NULL || has_pose == NULL || pairs == NULL ||
        placements == NULL || placed == NULL) {
        free(planes);
        free(pvalid);
        free(poses);
        free(has_pose);
        free(pairs);
        free(placements);
        free(placed);
        return pc_fail(err, PC_ERR_NOMEM, "register_direct");
    }
    pc_status rc = PC_OK;
    size_t npairs = 0;
    double rms_sum = 0.0;
    size_t nrms = 0;
    bool fail_no_detail = false;

    for (size_t i = 0; i < n; i++) {
        size_t sw = 0, sh = 0;
        float *l = NULL, *a = NULL;
        rc = pc_engine_photo_luma_alpha(images[i].surf, images[i].rect, k, &sw, &sh, &l, &a, err);
        if (rc != PC_OK) {
            goto cleanup;
        }
        planes[i] = l;
        pvalid[i] = valid_mask(a, sw * sh);
        free(a);
    }

    /* Outward from the reference, each layer to the previous one aligned, starting from its pose. */
    for (int dir_i = 0; dir_i < 2; dir_i++) {
        int64_t dir = dir_i == 0 ? 1 : -1;
        float *prev_luma = pc_malloc_array(cells, sizeof *prev_luma);
        bool *prev_valid = NULL;
        if (prev_luma == NULL || planes[reference] == NULL) {
            free(prev_luma);
            rc = pc_fail(err, PC_ERR_NOMEM, "register_direct");
            goto cleanup;
        }
        memcpy(prev_luma, planes[reference], cells * sizeof *prev_luma);
        if (pvalid[reference] != NULL) {
            prev_valid = pc_malloc_array(cells, sizeof *prev_valid);
            if (prev_valid == NULL) {
                free(prev_luma);
                rc = pc_fail(err, PC_ERR_NOMEM, "register_direct");
                goto cleanup;
            }
            memcpy(prev_valid, pvalid[reference], cells * sizeof *prev_valid);
        }
        pc_algo_pose init = PC_ALGO_POSE_IDENTITY;
        int64_t i = (int64_t)reference + dir;
        bool stop = false;
        while (!stop && i >= 0 && (size_t)i < n) {
            size_t idx = (size_t)i;
            pc_algo_direct_plane ref_plane = {w, h, prev_luma, prev_valid};
            pc_algo_direct_plane target = {w, h, planes[idx], pvalid[idx]};
            pc_algo_registration reg;
            memset(&reg, 0, sizeof reg);
            rc = pc_algo_direct_register(&ref_plane, &target, motion, init, 0, &reg, err);
            if (rc != PC_OK) {
                free(prev_luma);
                free(prev_valid);
                goto cleanup;
            }
            if (reg.overlap < min_overlap || reg.correlation < DIRECT_MIN_CORRELATION) {
                free(prev_luma);
                free(prev_valid);
                fail_no_detail = true;
                goto cleanup;
            }
            init = reg.pose;
            float *wl = NULL;
            bool *wv = NULL;
            rc = pc_algo_direct_warp(&target, &reg.pose, &wl, &wv, err);
            if (rc != PC_OK) {
                free(prev_luma);
                free(prev_valid);
                goto cleanup;
            }
            free(prev_luma);
            free(prev_valid);
            prev_luma = wl;
            prev_valid = wv;
            pairs[npairs].a = (size_t)(i - dir);
            pairs[npairs].b = idx;
            pairs[npairs].inliers = reg.overlap * k * k;
            npairs++;
            rms_sum += reg.rms;
            nrms++;
            poses[idx] = reg.pose;
            has_pose[idx] = true;
            i += dir;
        }
        free(prev_luma);
        free(prev_valid);
    }

    double center[2] = {(double)fw / 2.0, (double)fh / 2.0};
    for (size_t idx = 0; idx < n; idx++) {
        if (idx == reference) {
            pc_algo_homography id = PC_ALGO_HOMOGRAPHY_IDENTITY;
            placements[idx] = direct_placement(&id, fw, fh, center);
            placed[idx] = true;
        } else if (has_pose[idx]) {
            pc_algo_homography hm = pc_algo_pose_matrix(&poses[idx], fw, fh);
            placements[idx] = direct_placement(&hm, fw, fh, center);
            placed[idx] = true;
        }
    }
    memset(out, 0, sizeof *out);
    out->layout = resolved;
    out->reference = reference;
    out->n = n;
    out->placements = placements;
    out->placed = placed;
    out->focal = 0.0;
    out->k1 = 0.0;
    out->pairs = pairs;
    out->npairs = npairs;
    out->rms = nrms == 0 ? 0.0 : rms_sum / (double)nrms;
    placements = NULL;
    placed = NULL;
    pairs = NULL;

cleanup:
    for (size_t i = 0; i < n; i++) {
        free(planes[i]);
        free(pvalid[i]);
    }
    free(planes);
    free(pvalid);
    free(poses);
    free(has_pose);
    free(pairs);
    free(placements);
    free(placed);
    if (fail_no_detail) {
        return pc_fail(err, PC_ERR_INVALID, "%s", PC_ENGINE_PHOTO_NO_DETAIL);
    }
    return rc;
}

/* ---- placement warping -------------------------------------------------------------------- */

bool pc_engine_photo_plane_matrix(const pc_algo_placement *pl, double ox, double oy,
                                  pc_algo_homography *out)
{
    if (pl->projection != PC_ALGO_PROJECTION_PLANE || pl->k1 != 0.0) {
        return false;
    }
    pc_algo_homography t = {{1.0, 0.0, -pl->center[0] - ox, 0.0, 1.0, -pl->center[1] - oy, 0.0, 0.0, 1.0}};
    *out = pc_algo_homography_mul(&pl->h, &t);
    return true;
}

struct warp_map_ctx {
    const pc_algo_placement *pl;
    double ox, oy;
};

static void warp_map(void *vctx, double x, double y, double *ox, double *oy)
{
    struct warp_map_ctx *c = vctx;
    pc_algo_placement_forward(c->pl, x - c->ox, y - c->oy, ox, oy);
}

pc_status pc_engine_photo_warp_placed(const pc_surface *surf, pc_rect rect, double ox, double oy,
                                      const pc_algo_placement *pl, double offx, double offy,
                                      pc_algo_interp interp, pc_surface *out, pc_error *err)
{
    pc_algo_placement moved = pc_algo_placement_translated(pl, offx, offy);
    pc_algo_homography h;
    if (pc_engine_photo_plane_matrix(&moved, ox, oy, &h)) {
        return pc_algo_warp_surface(surf, rect, &h, interp, out, err);
    }
    struct warp_map_ctx ctx = {&moved, ox, oy};
    return pc_algo_warp_mesh_surface(surf, rect, warp_map, &ctx, interp, out, err);
}

/* ---- seam order --------------------------------------------------------------------------- */

static void surf_centre(const pc_surface *s, double out[2])
{
    pc_rect b = pc_surface_content_bounds(s);
    out[0] = (double)((int64_t)b.x0 + b.x1) / 2.0;
    out[1] = (double)((int64_t)b.y0 + b.y1) / 2.0;
}

struct order_ctx {
    const pc_surface *warped;
    size_t reference;
    double rc[2];
};

static int order_cmp(const void *pa, const void *pb, void *vctx)
{
    struct order_ctx *c = vctx;
    size_t a = *(const size_t *)pa, b = *(const size_t *)pb;
    bool na = a != c->reference, nb = b != c->reference;
    if (na != nb) {
        return na ? 1 : -1; /* the reference (false) sorts first */
    }
    double ca[2], cb[2];
    surf_centre(&c->warped[a], ca);
    surf_centre(&c->warped[b], cb);
    double da = hypot(ca[0] - c->rc[0], ca[1] - c->rc[1]);
    double db = hypot(cb[0] - c->rc[0], cb[1] - c->rc[1]);
    if (da < db) {
        return -1;
    }
    if (da > db) {
        return 1;
    }
    return 0;
}

/* A stable insertion sort with a context (qsort_r is not portable, and the order must be stable to
 * match Rust's stable sort_by). */
static void stable_sort_ctx(size_t *v, size_t n, int (*cmp)(const void *, const void *, void *),
                            void *ctx)
{
    for (size_t i = 1; i < n; i++) {
        size_t key = v[i];
        size_t j = i;
        while (j > 0 && cmp(&v[j - 1], &key, ctx) > 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = key;
    }
}

void pc_engine_photo_seam_order(const pc_surface *warped, size_t n, size_t reference, size_t *out)
{
    for (size_t i = 0; i < n; i++) {
        out[i] = i;
    }
    if (n == 0) {
        return;
    }
    struct order_ctx c;
    c.warped = warped;
    c.reference = reference;
    surf_centre(&warped[reference < n ? reference : n - 1], c.rc);
    stable_sort_ctx(out, n, order_cmp, &c);
}

/* ---- seam blend --------------------------------------------------------------------------- */

/* Colour channels + coverage of a surface region, owned buffers pointed into by `roi`. */
struct roi_buffers {
    float *px;    /* w*h*ch */
    float *alpha; /* w*h */
};

static pc_status roi_of(const pc_surface *surf, pc_rect area, size_t color_ch, pc_algo_roi_image *roi,
                        struct roi_buffers *buf, pc_error *err)
{
    pc_pixel_format fmt = pc_surface_format(surf);
    size_t n = pc_surface_channels(surf);
    size_t w = pc_rect_width(area), h = pc_rect_height(area);
    float *raw = NULL;
    size_t rlen = 0;
    if (pc_surface_read_region(surf, area, &raw, &rlen, err) != PC_OK) {
        return err != NULL ? err->code : PC_ERR_NOMEM;
    }
    float *px = pc_malloc_array(w * h * color_ch, sizeof *px);
    float *alpha = pc_malloc_array(w * h, sizeof *alpha);
    if (px == NULL || alpha == NULL) {
        free(raw);
        free(px);
        free(alpha);
        return pc_fail(err, PC_ERR_NOMEM, "roi_of");
    }
    for (size_t q = 0; q < w * h; q++) {
        const float *src = raw + q * n;
        for (size_t c = 0; c < color_ch; c++) {
            px[q * color_ch + c] = src[c];
        }
        alpha[q] = fmt.alpha ? src[n - 1] : 1.0f;
    }
    free(raw);
    roi->x0 = area.x0;
    roi->y0 = area.y0;
    roi->w = w;
    roi->h = h;
    roi->ch = color_ch;
    roi->px = px;
    roi->alpha = alpha;
    buf->px = px;
    buf->alpha = alpha;
    return PC_OK;
}

void pc_engine_photo_blended_fini(pc_engine_photo_blended *b)
{
    if (b == NULL) {
        return;
    }
    for (size_t i = 0; i < b->n; i++) {
        pc_surface_fini(&b->layer_px[i]);
        pc_surface_fini(&b->layer_mask[i]);
    }
    free(b->layer_px);
    free(b->layer_mask);
    pc_surface_fini(&b->composite);
    memset(b, 0, sizeof *b);
}

static float clamp01(float v)
{
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

static float data_clamp(float v, pc_pixel_format fmt)
{
    return fmt.sample == PC_SAMPLE_TYPE_F32 ? v : clamp01(v);
}

pc_status pc_engine_photo_seam_blend(const pc_surface *warped, size_t n, pc_rect canvas,
                                     const size_t *order, bool seamless, bool gains_on,
                                     bool has_geometry, const pc_algo_placement *geo_pl,
                                     const size_t (*geo_sizes)[2], double geo_off_x, double geo_off_y,
                                     pc_engine_photo_blended *out, pc_error *err)
{
    pc_pixel_format fmt = n > 0 ? pc_surface_format(&warped[0]) : PC_PIXEL_FORMAT_RGBA8;
    size_t cc = pc_color_mode_channels(fmt.mode);
    size_t nch = pc_pixel_format_channels(fmt);
    size_t cw = pc_rect_width(canvas);
    size_t ch = pc_rect_height(canvas);
    if (cw < 1) {
        cw = 1;
    }
    if (ch < 1) {
        ch = 1;
    }

    pc_status rc = PC_OK;
    pc_algo_roi_image *rois = pc_calloc_array(n > 0 ? n : 1, sizeof *rois);
    struct roi_buffers *bufs = pc_calloc_array(n > 0 ? n : 1, sizeof *bufs);
    pc_algo_placement *shifted = NULL;
    pc_algo_photometric photo;
    memset(&photo, 0, sizeof photo);
    bool have_photo = false;
    float **weights = pc_calloc_array(n > 0 ? n : 1, sizeof *weights);
    pc_algo_roi_image *local = pc_calloc_array(n > 0 ? n : 1, sizeof *local);
    const float **luma = NULL;
    const bool **cover = NULL;
    float *luma_store = NULL;
    bool *cover_store = NULL;
    int32_t *labels = NULL;
    size_t *rank = NULL;
    float *blend = NULL;
    float *cov = NULL;
    pc_surface *layer_px = pc_calloc_array(n > 0 ? n : 1, sizeof *layer_px);
    pc_surface *layer_mask = pc_calloc_array(n > 0 ? n : 1, sizeof *layer_mask);
    if (rois == NULL || bufs == NULL || weights == NULL || local == NULL || layer_px == NULL ||
        layer_mask == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "seam_blend");
        goto cleanup;
    }

    for (size_t i = 0; i < n; i++) {
        pc_rect b = pc_rect_intersect(pc_surface_content_bounds(&warped[i]), canvas);
        if (pc_rect_is_empty(b)) {
            rois[i].ch = cc; /* empty ROI */
        } else {
            rc = roi_of(&warped[i], b, cc, &rois[i], &bufs[i], err);
            if (rc != PC_OK) {
                goto cleanup;
            }
        }
    }

    if (has_geometry && geo_pl != NULL) {
        shifted = pc_malloc_array(n > 0 ? n : 1, sizeof *shifted);
        if (shifted == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "seam_blend");
            goto cleanup;
        }
        for (size_t i = 0; i < n; i++) {
            shifted[i] = pc_algo_placement_translated(&geo_pl[i], geo_off_x, geo_off_y);
        }
    }
    bool geo_on = shifted != NULL && geo_sizes != NULL;
    rc = pc_algo_panorama_photometric(rois, n, geo_on ? shifted : NULL, geo_on ? geo_sizes : NULL,
                                      seamless && gains_on, geo_on, &photo, err);
    if (rc != PC_OK) {
        goto cleanup;
    }
    have_photo = true;
    bool vig = photo.vignette[0] != 0.0 || photo.vignette[1] != 0.0;

    for (size_t i = 0; i < n; i++) {
        pc_algo_roi_image *roi = &rois[i];
        bool all_unit = true;
        for (size_t c = 0; c < roi->ch; c++) {
            if (fabsf(photo.gains[i * photo.ch + c] - 1.0f) >= 1e-6f) {
                all_unit = false;
                break;
            }
        }
        if (all_unit && !vig) {
            continue;
        }
        float *mpx = bufs[i].px;
        for (size_t y = 0; y < roi->h; y++) {
            for (size_t x = 0; x < roi->w; x++) {
                double r2 = 0.0;
                if (vig && geo_on) {
                    double sx, sy;
                    if (pc_algo_placement_inverse(&shifted[i], (double)(roi->x0 + (int32_t)x) + 0.5,
                                                  (double)(roi->y0 + (int32_t)y) + 0.5, &sx, &sy)) {
                        double gw = (double)geo_sizes[i][0], gh = (double)geo_sizes[i][1];
                        double dx = sx - gw / 2.0, dy = sy - gh / 2.0;
                        r2 = (dx * dx + dy * dy) / (gw * gw / 4.0 + gh * gh / 4.0);
                    }
                }
                size_t kk = y * roi->w + x;
                for (size_t c = 0; c < roi->ch; c++) {
                    float f = pc_algo_photometric_factor(&photo, i, c, r2);
                    float *v = &mpx[kk * roi->ch + c];
                    *v = fmt.mode == PC_COLOR_MODE_CMYK ? 1.0f - (1.0f - *v) * f : *v * f;
                    if (fmt.sample != PC_SAMPLE_TYPE_F32) {
                        *v = clamp01(*v);
                    }
                }
            }
        }
    }

    /* Seams on a <= 1000 px grid. */
    size_t smax = cw > ch ? cw : ch;
    size_t sstep = div_ceil_max1(smax, 1000);
    size_t gw = div_ceil_max1(cw, sstep);
    size_t gh = div_ceil_max1(ch, sstep);
    size_t gcells = gw * gh;
    luma = pc_calloc_array(n > 0 ? n : 1, sizeof *luma);
    cover = pc_calloc_array(n > 0 ? n : 1, sizeof *cover);
    luma_store = pc_calloc_array(n * gcells > 0 ? n * gcells : 1, sizeof *luma_store);
    cover_store = pc_calloc_array(n * gcells > 0 ? n * gcells : 1, sizeof *cover_store);
    if (luma == NULL || cover == NULL || luma_store == NULL || cover_store == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "seam_blend");
        goto cleanup;
    }
    for (size_t i = 0; i < n; i++) {
        float *lp = luma_store + i * gcells;
        bool *cp = cover_store + i * gcells;
        luma[i] = lp;
        cover[i] = cp;
        pc_algo_roi_image *roi = &rois[i];
        /* acc: sum luma, sum alpha, count. */
        float *acc_l = pc_calloc_array(gcells, sizeof *acc_l);
        float *acc_a = pc_calloc_array(gcells, sizeof *acc_a);
        uint32_t *acc_c = pc_calloc_array(gcells, sizeof *acc_c);
        if (acc_l == NULL || acc_a == NULL || acc_c == NULL) {
            free(acc_l);
            free(acc_a);
            free(acc_c);
            rc = pc_fail(err, PC_ERR_NOMEM, "seam_blend");
            goto cleanup;
        }
        for (size_t y = 0; y < roi->h; y++) {
            for (size_t x = 0; x < roi->w; x++) {
                size_t u = (size_t)(roi->x0 - canvas.x0) + x;
                size_t v = (size_t)(roi->y0 - canvas.y0) + y;
                size_t gy = (v / sstep) < gh ? (v / sstep) : gh - 1;
                size_t gx = (u / sstep) < gw ? (u / sstep) : gw - 1;
                size_t g = gy * gw + gx;
                size_t kk = y * roi->w + x;
                float l = 0.0f;
                for (size_t c = 0; c < roi->ch; c++) {
                    l += roi->px[kk * roi->ch + c];
                }
                l /= (float)roi->ch;
                acc_l[g] += l;
                acc_a[g] += roi->alpha[kk];
                acc_c[g] += 1;
            }
        }
        for (size_t g = 0; g < gcells; g++) {
            if (acc_c[g] > 0) {
                lp[g] = acc_l[g] / (float)acc_c[g];
                cp[g] = acc_a[g] / (float)(sstep * sstep) > 0.98f;
            }
        }
        free(acc_l);
        free(acc_a);
        free(acc_c);
    }
    rc = pc_algo_panorama_seam_labels(gw, gh, luma, cover, n, order, n, &labels, err);
    if (rc != PC_OK) {
        goto cleanup;
    }

    rank = pc_malloc_array(n > 0 ? n : 1, sizeof *rank);
    if (rank == NULL) {
        rc = pc_fail(err, PC_ERR_NOMEM, "seam_blend");
        goto cleanup;
    }
    for (size_t i = 0; i < n; i++) {
        rank[i] = SIZE_MAX;
    }
    for (size_t kk = 0; kk < n; kk++) {
        if (order[kk] < n) {
            rank[order[kk]] = kk;
        }
    }

    for (size_t i = 0; i < n; i++) {
        pc_algo_roi_image *roi = &rois[i];
        size_t cells = roi->w * roi->h;
        float *wt = pc_calloc_array(cells > 0 ? cells : 1, sizeof *wt);
        if (wt == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "seam_blend");
            goto cleanup;
        }
        for (size_t kk = 0; kk < cells; kk++) {
            int32_t x = roi->x0 + (int32_t)(kk % roi->w);
            int32_t y = roi->y0 + (int32_t)(kk / roi->w);
            if (roi->alpha[kk] < 0.5f) {
                wt[kk] = 0.0f;
                continue;
            }
            size_t u = (size_t)(x - canvas.x0), v = (size_t)(y - canvas.y0);
            size_t gy = (v / sstep) < gh ? (v / sstep) : gh - 1;
            size_t gx = (u / sstep) < gw ? (u / sstep) : gw - 1;
            int32_t lab = labels[gy * gw + gx];
            if (lab == (int32_t)i) {
                wt[kk] = 1.0f;
                continue;
            }
            if (lab >= 0) {
                /* alpha_at(lab, x, y) */
                pc_algo_roi_image *rl = &rois[(size_t)lab];
                int32_t lx = x - rl->x0, ly = y - rl->y0;
                float av = (lx < 0 || ly < 0 || (size_t)lx >= rl->w || (size_t)ly >= rl->h)
                               ? 0.0f
                               : rl->alpha[(size_t)ly * rl->w + (size_t)lx];
                if (av >= 0.5f) {
                    wt[kk] = 0.0f;
                    continue;
                }
            }
            /* Fallback: the earliest covering image in `order` wins. */
            size_t first = SIZE_MAX;
            size_t first_rank = SIZE_MAX;
            for (size_t j = 0; j < n; j++) {
                pc_algo_roi_image *rj = &rois[j];
                int32_t lx = x - rj->x0, ly = y - rj->y0;
                float av = (lx < 0 || ly < 0 || (size_t)lx >= rj->w || (size_t)ly >= rj->h)
                               ? 0.0f
                               : rj->alpha[(size_t)ly * rj->w + (size_t)lx];
                if (av >= 0.5f && rank[j] < first_rank) {
                    first_rank = rank[j];
                    first = j;
                }
            }
            wt[kk] = first == i ? 1.0f : 0.0f;
        }
        weights[i] = wt;
    }

    /* Canvas-relative ROIs for the blend. */
    for (size_t i = 0; i < n; i++) {
        local[i] = rois[i];
        local[i].x0 = rois[i].x0 - canvas.x0;
        local[i].y0 = rois[i].y0 - canvas.y0;
    }
    size_t levels = 1;
    if (seamless) {
        levels = pc_algo_panorama_blend_levels(cw, ch);
        if (levels > 7) {
            levels = 7;
        }
    }
    rc = pc_algo_panorama_multiband(cw, ch, local, n, (const float *const *)weights, levels, &blend, &cov,
                                    err);
    if (rc != PC_OK) {
        goto cleanup;
    }

    /* Composite. */
    pc_surface composite;
    pc_surface_init(&composite, fmt);
    {
        size_t cells = cw * ch;
        float *data = pc_calloc_array(cells * nch, sizeof *data);
        if (data == NULL) {
            pc_surface_fini(&composite);
            rc = pc_fail(err, PC_ERR_NOMEM, "seam_blend");
            goto cleanup;
        }
        for (size_t i = 0; i < cells; i++) {
            for (size_t c = 0; c < cc; c++) {
                data[i * nch + c] = data_clamp(blend[i * cc + c], fmt);
            }
            data[i * nch + nch - 1] = cov[i];
        }
        (void)pc_surface_write_region(&composite, canvas, data, cells * nch, err);
        pc_surface_prune(&composite);
        free(data);
    }

    /* Per-layer kept pixels and mask. */
    for (size_t i = 0; i < n; i++) {
        pc_algo_roi_image *roi = &rois[i];
        pc_rect area = pc_rect_make(roi->x0, roi->y0, roi->x0 + (int32_t)roi->w, roi->y0 + (int32_t)roi->h);
        size_t cells = roi->w * roi->h;
        float *px = pc_calloc_array(cells * nch > 0 ? cells * nch : 1, sizeof *px);
        float *mask = pc_calloc_array(cells > 0 ? cells : 1, sizeof *mask);
        if (px == NULL || mask == NULL) {
            free(px);
            free(mask);
            rc = pc_fail(err, PC_ERR_NOMEM, "seam_blend");
            goto cleanup;
        }
        for (size_t kk = 0; kk < cells; kk++) {
            size_t u = (size_t)(roi->x0 - canvas.x0) + kk % roi->w;
            size_t v = (size_t)(roi->y0 - canvas.y0) + kk / roi->w;
            size_t g = v * cw + u;
            bool inside = weights[i][kk] > 0.5f;
            for (size_t c = 0; c < cc; c++) {
                px[kk * nch + c] =
                    inside && seamless ? data_clamp(blend[g * cc + c], fmt) : roi->px[kk * cc + c];
            }
            px[kk * nch + nch - 1] = roi->alpha[kk];
            mask[kk] = weights[i][kk];
        }
        pc_surface_init(&layer_px[i], fmt);
        if (!pc_rect_is_empty(area)) {
            (void)pc_surface_write_region(&layer_px[i], area, px, cells * nch, err);
        }
        pc_surface_prune(&layer_px[i]);
        float gray0 = 0.0f;
        pc_surface_init_default(&layer_mask[i], PC_PIXEL_FORMAT_GRAY8, &gray0, 1);
        if (!pc_rect_is_empty(area)) {
            (void)pc_surface_write_region(&layer_mask[i], area, mask, cells, err);
        }
        pc_surface_prune(&layer_mask[i]);
        free(px);
        free(mask);
    }

    out->layer_px = layer_px;
    out->layer_mask = layer_mask;
    out->n = n;
    out->composite = composite;
    out->vignette[0] = photo.vignette[0];
    out->vignette[1] = photo.vignette[1];
    layer_px = NULL;
    layer_mask = NULL;

cleanup:
    if (bufs != NULL) {
        for (size_t i = 0; i < n; i++) {
            free(bufs[i].px);
            free(bufs[i].alpha);
        }
    }
    if (weights != NULL) {
        for (size_t i = 0; i < n; i++) {
            free(weights[i]);
        }
    }
    if (layer_px != NULL) {
        for (size_t i = 0; i < n; i++) {
            pc_surface_fini(&layer_px[i]);
        }
    }
    if (layer_mask != NULL) {
        for (size_t i = 0; i < n; i++) {
            pc_surface_fini(&layer_mask[i]);
        }
    }
    if (have_photo) {
        pc_algo_photometric_fini(&photo);
    }
    free(rois);
    free(bufs);
    free(shifted);
    free(weights);
    free(local);
    free(luma);
    free(cover);
    free(luma_store);
    free(cover_store);
    free(labels);
    free(rank);
    free(blend);
    free(cov);
    free(layer_px);
    free(layer_mask);
    return rc;
}
