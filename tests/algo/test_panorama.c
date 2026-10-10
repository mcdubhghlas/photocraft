/* panorama.rs: registration layouts, rotating-camera focal, placements, photometric, seams and
 * multi-band blending. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "algo/panorama.h"
#include "analysis_gen.h"
#include "harness.h"

static pc_algo_align_options opts(pc_algo_layout layout, size_t reference)
{
    pc_algo_align_options o;
    memset(&o, 0, sizeof o);
    o.layout = layout;
    o.has_reference = true;
    o.reference = reference;
    return o;
}

static void test_recovers_translations_and_homographies(void)
{
    size_t sw = 640, sh = 300, tw = 260, th = 220, i, nm = 0, nm2 = 0;
    float *sc = gen_scene(sw, sh, 3), *im[3], *im2;
    const double offs[3][2] = {{20.0, 30.0}, {190.0, 40.0}, {360.0, 25.0}};
    const pc_algo_layout layouts[3] = {PC_ALGO_LAYOUT_REPOSITION, PC_ALGO_LAYOUT_COLLAGE,
                                       PC_ALGO_LAYOUT_PERSPECTIVE};
    pc_algo_image_features feats[3], feats2[2];
    pc_algo_pair_match *m = NULL, *m2 = NULL;
    pc_algo_homography hq = {{1.05, 0.03, 180.0, -0.02, 1.0, 35.0, 0.0001, -0.00005, 1.0}};
    const double probe[3][2] = {{20.0, 30.0}, {70.0, 180.0}, {40.0, 100.0}};
    int l;
    for (i = 0; i < 3; i++) {
        im[i] = gen_view(sc, sw, sh, tw, th, gen_map_offset, offs[i]);
        CHECK(pc_algo_panorama_detect(tw, th, im[i], NULL, 700, &feats[i], NULL) == PC_OK);
    }
    CHECK(pc_algo_panorama_match_pairs(feats, 3, &m, &nm, NULL) == PC_OK);
    CHECK(nm >= 2);
    for (l = 0; l < 3; l++) {
        pc_algo_align_options o = opts(layouts[l], 1);
        pc_algo_alignment a;
        bool found = false;
        CHECK(pc_algo_panorama_align(feats, 3, m, nm, &o, &found, &a, NULL) == PC_OK);
        CHECK(found);
        if (!found) {
            continue;
        }
        CHECK_EQ_UINT(a.reference, 1);
        for (i = 0; i < 3; i++) {
            double u, v, eu = 50.0 + offs[i][0] - offs[1][0], ev = 60.0 + offs[i][1] - offs[1][1];
            CHECK(a.placed[i]);
            pc_algo_placement_forward(&a.placements[i], 50.0, 60.0, &u, &v);
            CHECK(fabs(u - eu) < 1.0 && fabs(v - ev) < 1.0);
        }
        CHECK(a.rms < 1.0);
        pc_algo_alignment_fini(&a);
    }
    /* A projective view is recovered by the perspective layout. */
    im2 = gen_view(sc, sw, sh, tw, th, gen_map_homography, &hq);
    CHECK(pc_algo_image_features_copy(&feats[0], &feats2[0], NULL) == PC_OK);
    CHECK(pc_algo_panorama_detect(tw, th, im2, NULL, 700, &feats2[1], NULL) == PC_OK);
    CHECK(pc_algo_panorama_match_pairs(feats2, 2, &m2, &nm2, NULL) == PC_OK);
    {
        pc_algo_align_options o = opts(PC_ALGO_LAYOUT_PERSPECTIVE, 0);
        pc_algo_alignment a;
        bool found = false;
        CHECK(pc_algo_panorama_align(feats2, 2, m2, nm2, &o, &found, &a, NULL) == PC_OK);
        CHECK(found);
        for (i = 0; found && i < 3; i++) {
            double sx, sy, u, v;
            pc_algo_homography_apply(&hq, probe[i][0], probe[i][1], &sx, &sy);
            pc_algo_placement_forward(&a.placements[1], probe[i][0], probe[i][1], &u, &v);
            CHECK(fabs(u - (sx - 20.0)) < 1.5 && fabs(v - (sy - 30.0)) < 1.5);
        }
        if (found) {
            pc_algo_alignment_fini(&a);
        }
    }
    pc_algo_pair_matches_free(m, nm);
    pc_algo_pair_matches_free(m2, nm2);
    for (i = 0; i < 3; i++) {
        pc_algo_image_features_fini(&feats[i]);
        free(im[i]);
    }
    pc_algo_image_features_fini(&feats2[0]);
    pc_algo_image_features_fini(&feats2[1]);
    free(im2), free(sc);
}

static void test_rotating_camera_focal_and_cylindrical_layout(void)
{
    const double f = 300.0, pans[3] = {-25.0, 0.0, 25.0};
    size_t sw = 1400, sh = 500, tw = 320, th = 240, i, nm = 0, adjacent = 0;
    float *sc = gen_scene(sw, sh, 9), *im[3];
    pc_algo_image_features feats[3];
    pc_algo_pair_match *m = NULL;
    const double c160[2] = {160.0, 120.0};
    bool any_close = false, found = false;
    pc_algo_align_options o = opts(PC_ALGO_LAYOUT_CYLINDRICAL, 1);
    pc_algo_alignment a;
    for (i = 0; i < 3; i++) {
        double ctx[6] = {f, (double)tw, (double)th, (double)sw, (double)sh, pans[i]};
        im[i] = gen_view(sc, sw, sh, tw, th, gen_map_pan, ctx);
        CHECK(pc_algo_panorama_detect(tw, th, im[i], NULL, 700, &feats[i], NULL) == PC_OK);
    }
    CHECK(pc_algo_panorama_match_pairs(feats, 3, &m, &nm, NULL) == PC_OK);
    for (i = 0; i < nm; i++) {
        double e;
        adjacent += m[i].b == m[i].a + 1 ? 1u : 0u;
        if (pc_algo_panorama_focal_from_homography(&m[i].homography, c160, c160, &e)) {
            any_close = any_close || fabs(e - f) / f < 0.15;
        }
    }
    CHECK_EQ_UINT(adjacent, 2);
    CHECK(any_close);
    CHECK(pc_algo_panorama_align(feats, 3, m, nm, &o, &found, &a, NULL) == PC_OK);
    CHECK(found);
    if (found) {
        double u, v, x, y, expect;
        CHECK(fabs(a.focal - f) / f < 0.1);
        CHECK(a.placed[2]);
        /* The centre of the right image sits 25 degrees along the cylinder. */
        pc_algo_placement_forward(&a.placements[2], 160.0, 120.0, &u, &v);
        expect = 160.0 + a.focal * (25.0 * (3.141592653589793 / 180.0));
        CHECK(fabs(u - expect) < 4.0);
        CHECK(a.rms < 1.5);
        /* Round trip through the placement. */
        pc_algo_placement_forward(&a.placements[2], 40.0, 200.0, &u, &v);
        CHECK(pc_algo_placement_inverse(&a.placements[2], u, v, &x, &y));
        CHECK(fabs(x - 40.0) < 1e-6 && fabs(y - 200.0) < 1e-6);
        pc_algo_alignment_fini(&a);
    }
    pc_algo_pair_matches_free(m, nm);
    for (i = 0; i < 3; i++) {
        pc_algo_image_features_fini(&feats[i]);
        free(im[i]);
    }
    free(sc);
}

static void test_placement_round_trips_all_projections(void)
{
    const pc_algo_projection ps[3] = {PC_ALGO_PROJECTION_PLANE, PC_ALGO_PROJECTION_CYLINDER,
                                      PC_ALGO_PROJECTION_SPHERE};
    const double pts[3][2] = {{0.0, 0.0}, {390.0, 20.0}, {123.0, 280.0}};
    int k, i;
    for (k = 0; k < 3; k++) {
        pc_algo_placement p = {ps[k],
                               400.0,
                               {200.0, 150.0},
                               0.05,
                               {{0.98, -0.1, 30.0, 0.1, 0.98, -12.0, 0.0, 0.0, 1.0}}},
                          s;
        double u, v, u2, v2, bx, by;
        for (i = 0; i < 3; i++) {
            pc_algo_placement_forward(&p, pts[i][0], pts[i][1], &u, &v);
            CHECK(pc_algo_placement_inverse(&p, u, v, &bx, &by));
            CHECK(fabs(bx - pts[i][0]) < 1e-6 && fabs(by - pts[i][1]) < 1e-6);
        }
        s = pc_algo_placement_scaled(&p, 2.0);
        pc_algo_placement_forward(&p, 100.0, 50.0, &u, &v);
        pc_algo_placement_forward(&s, 200.0, 100.0, &u2, &v2);
        CHECK(fabs(u2 - 2.0 * u) < 1e-6 && fabs(v2 - 2.0 * v) < 1e-6);
    }
}

static float base_fn(int x, int y)
{
    return 0.3f + 0.2f * fabsf(sinf((float)x * 0.05f) * cosf((float)y * 0.07f));
}

static void test_photometric_recovers_gain_and_vignette(void)
{
    /* Two half-overlapping views of a flat-ish scene: the second one 30 % brighter. */
    const size_t w = 200, h = 100;
    const int x0s[2] = {0, 100};
    const float gs[2] = {1.0f, 1.3f};
    float *px[2], *alpha = malloc(w * h * sizeof *alpha);
    pc_algo_roi_image imgs[2];
    pc_algo_placement pls[2];
    const size_t sizes[2][2] = {{200, 100}, {200, 100}};
    pc_algo_photometric ph, pv;
    size_t i;
    int k, x, y;
    for (i = 0; i < w * h; i++) {
        alpha[i] = 1.0f;
    }
    for (k = 0; k < 2; k++) {
        px[k] = malloc(w * h * sizeof *px[k]);
        for (y = 0; y < (int)h; y++) {
            for (x = 0; x < (int)w; x++) {
                px[k][(size_t)y * w + (size_t)x] = base_fn(x + x0s[k], y) * gs[k];
            }
        }
        imgs[k].x0 = x0s[k], imgs[k].y0 = 0, imgs[k].w = w, imgs[k].h = h, imgs[k].ch = 1;
        imgs[k].px = px[k], imgs[k].alpha = alpha;
        pls[k].projection = PC_ALGO_PROJECTION_PLANE, pls[k].focal = 200.0;
        pls[k].center[0] = 100.0, pls[k].center[1] = 50.0, pls[k].k1 = 0.0;
        pls[k].h = (pc_algo_homography){{1.0, 0.0, 100.0 + x0s[k], 0.0, 1.0, 50.0, 0.0, 0.0, 1.0}};
    }
    CHECK(pc_algo_panorama_photometric(imgs, 2, pls, sizes, true, false, &ph, NULL) == PC_OK);
    CHECK(fabsf(ph.gains[1] / ph.gains[0] - 1.3f) < 0.03f);
    /* With vignetting on, the flat scene (no falloff) gives a near-zero model. */
    CHECK(pc_algo_panorama_photometric(imgs, 2, pls, sizes, true, true, &pv, NULL) == PC_OK);
    CHECK(fabs(pv.vignette[0]) < 0.1);
    pc_algo_photometric_fini(&ph);
    pc_algo_photometric_fini(&pv);
    free(px[0]), free(px[1]), free(alpha);
}

static void test_seams_avoid_disagreement_and_multiband_is_seamless(void)
{
    /* Two images over a 120x40 strip overlapping in 40..80; the second has a bright "moving
     * object" at x 50..58 that the first doesn't. */
    const size_t w = 120, h = 40;
    float *luma[2], *pxs[2], *alpha, *wts[2], *out = NULL, *cov = NULL, maxjump = 0.0f;
    bool *cover[2];
    const size_t order[2] = {0, 1};
    int32_t *labels = NULL;
    pc_algo_roi_image imgs[2];
    size_t i, x, y;
    int k;
    for (k = 0; k < 2; k++) {
        luma[k] = malloc(w * h * sizeof *luma[k]);
        cover[k] = malloc(w * h * sizeof *cover[k]);
        for (i = 0; i < w * h; i++) {
            luma[k][i] = 0.5f;
            cover[k][i] = k == 0 ? i % w < 80 : i % w >= 40;
        }
    }
    for (y = 0; y < h; y++) {
        for (x = 50; x < 58; x++) {
            luma[1][y * w + x] = 1.0f;
        }
    }
    CHECK(pc_algo_panorama_seam_labels(w, h, (const float *const *)luma, (const bool *const *)cover, 2, order,
                                       2, &labels, NULL) == PC_OK);
    /* The object column belongs to image 0 (the seam passes to its right), or entirely to 1. */
    for (y = 0; y < h; y++) {
        bool same = true;
        for (x = 50; x < 58; x++) {
            same = same && labels[y * w + x] == labels[y * w + 50];
        }
        CHECK(same);
    }
    CHECK_EQ_INT(labels[5], 0);
    CHECK_EQ_INT(labels[w - 1], 1);
    /* Multi-band blend of two flat images at different levels joins smoothly. */
    alpha = malloc(80 * h * sizeof *alpha);
    for (i = 0; i < 80 * h; i++) {
        alpha[i] = 1.0f;
    }
    for (k = 0; k < 2; k++) {
        pxs[k] = malloc(80 * h * sizeof *pxs[k]);
        wts[k] = malloc(80 * h * sizeof *wts[k]);
        for (i = 0; i < 80 * h; i++) {
            int gx = (int)(i % 80) + (k == 0 ? 0 : 40);
            pxs[k][i] = k == 0 ? 0.3f : 0.7f;
            wts[k][i] = ((gx < 60) == (k == 0)) ? 1.0f : 0.0f;
        }
        imgs[k].x0 = k == 0 ? 0 : 40, imgs[k].y0 = 0, imgs[k].w = 80, imgs[k].h = h, imgs[k].ch = 1;
        imgs[k].px = pxs[k], imgs[k].alpha = alpha;
    }
    CHECK(pc_algo_panorama_multiband(w, h, imgs, 2, (const float *const *)wts, 4, &out, &cov, NULL) == PC_OK);
    CHECK(fabsf(out[20 * w + 2] - 0.3f) < 0.02f && fabsf(out[20 * w + w - 3] - 0.7f) < 0.02f);
    for (x = 1; x < w; x++) {
        maxjump = fmaxf(maxjump, fabsf(out[20 * w + x] - out[20 * w + x - 1]));
    }
    CHECK(maxjump < 0.1f);
    for (i = 0; i < w * h; i++) {
        CHECK(cov[i] == 1.0f);
    }
    free(out), free(cov), free(labels), free(alpha);
    for (k = 0; k < 2; k++) {
        free(luma[k]), free(cover[k]), free(pxs[k]), free(wts[k]);
    }
}

static void test_never_crash(void)
{
    /* Single-image panoramas, empty and flat images, mismatched inputs, hostile params. */
    float *flat = calloc(64 * 64, sizeof *flat), *o = NULL, *c = NULL, one = 0.5f, a1 = 1.0f;
    pc_algo_image_features f[2];
    pc_algo_pair_match *m = (pc_algo_pair_match *)1, pm;
    size_t nm = 9, order = 3;
    pc_algo_align_options op = opts(PC_ALGO_LAYOUT_AUTO, 99);
    pc_algo_alignment al;
    bool found = true;
    pc_algo_roi_image ri = {-5, -5, 1, 1, 1, &one, &a1}, rbad = {0, 0, 1, 1, 2, &one, &a1};
    const float *wts[2] = {&one, &one};
    pc_algo_photometric ph;
    int32_t *lab = NULL;
    const float *lp[1] = {flat};
    bool cv = true;
    const bool *cp[1] = {&cv};
    pc_algo_placement pl = {PC_ALGO_PROJECTION_SPHERE, 0.0, {0, 0}, 1e300, {{0, 0, 0, 0, 0, 0, 0, 0, 0}}};
    double x, y, b[4], fo;
    pc_algo_layout lay;
    pc_algo_prepared_image pi = {64, 64, flat, NULL};
    CHECK(pc_algo_panorama_detect(64, 64, flat, NULL, 100, &f[0], NULL) == PC_OK && f[0].n == 0);
    CHECK(pc_algo_panorama_detect(0, 0, NULL, NULL, 100, &f[1], NULL) == PC_OK && f[1].n == 0);
    CHECK(pc_algo_panorama_match_pairs(f, 1, &m, &nm, NULL) == PC_OK && nm == 0 && m == NULL);
    CHECK(pc_algo_panorama_match_pairs(f, 2, &m, &nm, NULL) == PC_OK && nm == 0);
    CHECK(pc_algo_panorama_align(f, 1, NULL, 0, &op, &found, &al, NULL) == PC_OK && !found);
    memset(&pm, 0, sizeof pm);
    pm.a = 0, pm.b = 5;
    CHECK(pc_algo_panorama_align(f, 2, &pm, 1, &op, &found, &al, NULL) == PC_ERR_INVALID);
    pm.b = 1;
    found = true;
    CHECK(pc_algo_panorama_align(f, 2, &pm, 1, &op, &found, &al, NULL) == PC_OK && !found);
    pc_algo_image_features_fini(&f[0]);
    pc_algo_image_features_fini(&f[1]);
    CHECK(pc_algo_panorama_detect_many(&pi, 1, 10, f, NULL) == PC_OK);
    pc_algo_image_features_fini(&f[0]);
    /* Degenerate placements. */
    pc_algo_placement_forward(&pl, 1.0, 2.0, &x, &y);
    CHECK(!pc_algo_placement_inverse(&pl, 1.0, 2.0, &x, &y));
    pc_algo_placement_bounds(&pl, 10.0, 10.0, b);
    pl.projection = PC_ALGO_PROJECTION_CYLINDER, pl.focal = 1.0, pl.h = PC_ALGO_HOMOGRAPHY_IDENTITY;
    CHECK(!pc_algo_placement_unproject(&pl, 100.0, 0.0, &x, &y));
    CHECK(!pc_algo_panorama_focal_from_homography(&PC_ALGO_HOMOGRAPHY_IDENTITY, b, b, &fo));
    /* Photometric with nothing to compare, mismatched channels. */
    CHECK(pc_algo_panorama_photometric(&ri, 1, NULL, NULL, true, true, &ph, NULL) == PC_OK);
    CHECK(ph.gains[0] == 1.0f);
    pc_algo_photometric_fini(&ph);
    CHECK(pc_algo_panorama_photometric(NULL, 0, NULL, NULL, true, true, &ph, NULL) == PC_OK);
    pc_algo_photometric_fini(&ph);
    {
        pc_algo_roi_image two[2] = {ri, rbad};
        CHECK(pc_algo_panorama_photometric(two, 2, NULL, NULL, true, false, &ph, NULL) == PC_ERR_INVALID);
        CHECK(pc_algo_panorama_multiband(4, 4, two, 2, wts, 3, &o, &c, NULL) == PC_ERR_INVALID);
    }
    /* Seams with a bad order, multiband of an off-canvas image, absurd levels, empty canvas. */
    CHECK(pc_algo_panorama_seam_labels(1, 1, lp, cp, 1, &order, 1, &lab, NULL) == PC_ERR_INVALID);
    order = 0;
    CHECK(pc_algo_panorama_seam_labels(1, 1, lp, cp, 1, &order, 1, &lab, NULL) == PC_OK && lab[0] == 0);
    free(lab);
    CHECK(pc_algo_panorama_multiband(4, 4, &ri, 1, wts, 3, &o, &c, NULL) == PC_OK);
    free(o), free(c);
    CHECK(pc_algo_panorama_multiband(4, 4, &ri, 1, wts, 1000, &o, &c, NULL) == PC_ERR_LIMIT);
    CHECK(pc_algo_panorama_multiband(0, 0, NULL, 0, NULL, 0, &o, &c, NULL) == PC_OK);
    free(o), free(c);
    CHECK_EQ_UINT(pc_algo_panorama_blend_levels(0, 0), 1);
    CHECK_EQ_UINT(pc_algo_panorama_blend_levels(10000, 10000), 7);
    CHECK(pc_algo_layout_parse("spherical", &lay) && lay == PC_ALGO_LAYOUT_SPHERICAL);
    CHECK(!pc_algo_layout_parse("x", &lay));
    CHECK_EQ_STR(pc_algo_layout_name(PC_ALGO_LAYOUT_COLLAGE), "collage");
    free(flat);
}

int main(int argc, char **argv)
{
    RUN(test_recovers_translations_and_homographies);
    RUN(test_rotating_camera_focal_and_cylindrical_layout);
    RUN(test_placement_round_trips_all_projections);
    RUN(test_photometric_recovers_gain_and_vignette);
    RUN(test_seams_avoid_disagreement_and_multiband_is_seamless);
    RUN(test_never_crash);
    return pc_t_finish();
}
