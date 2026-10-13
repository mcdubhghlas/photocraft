/* R9 (resync) sweeps of test_parity_fx.c, appended by rustref compose_fx_vectors.rs r9_sweeps: Chisel
 * Soft heights, the bevel distance fields, contours with corners, satin and bevel contour tables.
 * Included once, after test_parity_fx.c's hashing and generator helpers (one translation unit). */
#ifndef PC_TESTS_COMPOSE_FX_R9_H
#define PC_TESTS_COMPOSE_FX_R9_H

/* An anti-aliased disc or oblique bar (coverage by the distance to its edge, one pixel wide). */
static float *aa_shape(rng *r, size_t w, size_t h)
{
    bool disc = below(r, 2) == 0;
    float cx = unit(r) * (float)w;
    float cy = unit(r) * (float)h;
    float sz = 2.0f + unit(r) * ((float)(w < h ? w : h) / 2.0f);
    float ang = unit(r) * 6.2831855f;
    float sn = sinf(ang);
    float cs = cosf(ang);
    float *v = xcalloc(w * h > 0 ? w * h : 1, sizeof *v);
    size_t x;
    size_t y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float px = (float)x + 0.5f - cx;
            float py = (float)y + 0.5f - cy;
            float d = disc ? sqrtf(px * px + py * py) - sz : fabsf(-px * sn + py * cs) - sz / 2.0f;
            v[y * w + x] = pc_clampf(0.5f - d, 0.0f, 1.0f);
        }
    }
    return v;
}

static void r9_sweeps(out *o)
{
    static const float vals[13] = {0.0f,  1.0f,     0.5f, 2.5f,  -1.0f, 4.0f, 16.0f,
                                   1e30f, INFINITY, NAN,  0.25f, 0.75f, -1.5f};
    static const float douts[5] = {0.0f, 1.0f, 3.0f, -0.5f, NAN};
    static const float reaches[4] = {0.0f, 3.0f, 12.0f, INFINITY};
    uint64_t h = H0;
    uint64_t hs = H0;
    uint64_t hb = H0;
    rng rr;
    int p;
    int s;
    int j;
    int k;
    int it;
    for (p = 0; p < 3; p++) {
        for (s = 0; s < 13; s++) {
            for (j = 0; j < 13; j++) {
                for (k = 0; k < 5; k++) {
                    /* Inner Chisel Soft clamps to (size + 2) / (size + 1): the reference panics
                     * when that bound is NaN or negative (§7); C returns a defined value. */
                    float top = (vals[s] + 2.0f) / (vals[s] + 1.0f);
                    float v = pc_compose_fx_bevel_chisel_h((pc_compose_bevel_paint)p, vals[s], vals[j],
                                                           douts[k], true);
                    if (p == PC_COMPOSE_BEVEL_PAINT_INNER && vals[j] > 0.0f && !(top >= 0.0f)) {
                        fnv(&h, "panic", 5);
                    } else {
                        hf(&h, v);
                    }
                }
            }
        }
    }
    put(o, "unit\tr9_chisel_soft\t%016llx\n", (unsigned long long)h);
    for (k = 0; k < 2; k++) {
        int q;
        for (q = 0; q < 4; q++) {
            rr.s = 0x7a7a + (uint64_t)k;
            h = H0;
            for (it = 0; it < 10; it++) {
                size_t w = 4 + (size_t)below(&rr, 60);
                size_t hh = 4 + (size_t)below(&rr, 50);
                float *a = aa_shape(&rr, w, hh);
                float *d = NULL;
                size_t m;
                CHECK(pc_compose_fx_distance_field(k == 0 ? PC_COMPOSE_FIELD_BEVEL_INSIDE
                                                          : PC_COMPOSE_FIELD_BEVEL_OUTSIDE,
                                                   a, w, hh, reaches[q], &d, NULL) == PC_OK);
                for (m = 0; d != NULL && m < w * hh; m++) {
                    hf(&h, d[m]);
                }
                free(d);
                free(a);
            }
            put(o, "unit\tr9_field_%d_%g\t%016llx\n", k, (double)reaches[q], (unsigned long long)h);
        }
    }
    rr.s = 0x5a71;
    for (it = 0; it < 200; it++) {
        pc_bevel_contour bc;
        float *l = NULL;
        size_t m;
        bc.contour = contour(&rr);
        if (bc.contour.custom) {
            size_t nc = bc.contour.points.len + (size_t)below(&rr, 2);
            bc.contour.corners = xcalloc(nc > 0 ? nc : 1, sizeof *bc.contour.corners);
            bc.contour.ncorners = nc;
            for (m = 0; m < nc; m++) {
                bc.contour.corners[m] = below(&rr, 2) == 0;
            }
        }
        CHECK(pc_compose_fx_satin_lut(&bc.contour, &l, NULL) == PC_OK);
        if (l != NULL) {
            for (m = 0; m < PC_COMPOSE_FX_LUT_SIZE; m++) {
                hf(&hs, l[m]);
            }
        } else {
            fnv(&hs, "none", 4);
        }
        free(l);
        l = NULL;
        bc.range = unit(&rr);
        bc.anti_alias = false;
        CHECK(pc_compose_fx_bevel_contour_lut(&bc, &l, NULL) == PC_OK);
        for (m = 0; l != NULL && m < PC_COMPOSE_FX_LUT_SIZE; m++) {
            hf(&hb, l[m]);
        }
        free(l);
        pc_contour_fini(&bc.contour);
    }
    put(o, "unit\tr9_satin_lut\t%016llx\n", (unsigned long long)hs);
    put(o, "unit\tr9_bevel_contour_lut\t%016llx\n", (unsigned long long)hb);
}

#endif
