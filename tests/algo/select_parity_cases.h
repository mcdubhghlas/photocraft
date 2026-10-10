/* The cases of test_select_parity.c, in the order of tools/rustref/src/algo_select_vectors.rs.
 * Included once by that file (one translation unit). */
#ifndef PC_TESTS_ALGO_SELECT_PARITY_CASES_H
#define PC_TESTS_ALGO_SELECT_PARITY_CASES_H

static void selection_cases(void)
{
    static const char *const names[] = {"replace", "add", "subtract", "intersect", "x", ""};
    static const pc_rect areas[3] = {{0, 0, 40, 30}, {-37, 250, 300, 263}, {250, -3, 262, 300}};
    char b1[32], b2[32];
    size_t i, k;
    for (i = 0; i < 6; i++) {
        line("sel.mode.%s %s", names[i], MODE_NAMES[pc_algo_selection_mode_parse(names[i])]);
    }
    for (i = 0; i < 3; i++) {
        pc_rect area = areas[i];
        int32_t w = (int32_t)pc_rect_width(area), h = (int32_t)pc_rect_height(area), j;
        float *m = mask_field(w, h, (int32_t)i), *nw = malloc((size_t)(w * h) * sizeof(float)), *v = NULL;
        size_t n = (size_t)(w * h), len = 0;
        pc_surface old, rgba;
        int mode;
        (void)pc_algo_selection_mask_to_surface(m, n, area, &old, NULL);
        line("sel.to_surface.%zu %016llx %zu", i, X(hs(&old)), pc_surface_tile_count(&old));
        (void)pc_algo_selection_mask_from_surface(&old, area, &v, &len, NULL);
        line("sel.from_surface.%zu %016llx", i, X(of(v, len)));
        free(v);
        surf((pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}, area, 3, &rgba);
        (void)pc_algo_selection_mask_from_surface(&rgba, area, &v, &len, NULL);
        line("sel.from_surface_rgba.%zu %016llx", i, X(of(v, len)));
        free(v);
        for (j = 0; j < w * h; j++) {
            nw[j] = pat(j % w, j / w, 1, (int32_t)i) * 1.2f - 0.1f;
        }
        for (mode = 0; mode < 4; mode++) {
            int pi;
            for (pi = 0; pi < 2; pi++) {
                pc_surface o;
                bool some = false;
                (void)pc_algo_selection_combine(pi ? &old : NULL, nw, n, area, (pc_algo_selection_mode)mode,
                                                &o, &some, NULL);
                line("sel.combine.%zu.%s.%d %s", i, MODE_NAMES[mode], pi, osurf(some, &o, b1));
                pc_surface_fini(&o);
            }
        }
        pc_surface_fini(&old);
        pc_surface_fini(&rgba);
        free(m);
        free(nw);
    }
    {
        pc_surface o;
        bool some = true;
        (void)pc_algo_selection_combine(NULL, NULL, 0, PC_RECT_EMPTY, PC_ALGO_SELECTION_REPLACE, &o, &some,
                                        NULL);
        line("sel.combine.empty %s", osurf(some, &o, b1));
        pc_surface_fini(&o);
    }
    /* Magic wand (float) and wand_region (8-bit). */
    {
        static const struct {
            int32_t sx, sy;
            float tol;
            bool cont, aa;
        } wc[5] = {{10, 5, 10.0f, true, true},
                   {10, 5, 30.0f, false, false},
                   {20, 20, 0.0f, true, false},
                   {39, 26, 80.0f, true, true},
                   {50, 3, 5.0f, true, true}},
          rc[6] = {{10, 5, 10.0f, true, true}, {10, 5, 10.0f, true, false},  {20, 20, 40.0f, false, true},
                   {4, -2, 0.0f, true, true},  {30, 10, 255.0f, true, true}, {0, 0, 1.0f, true, true}};
        pc_rect area = {3, -2, 40, 27};
        float (*px)[4] = rgba_img(37, 29, 2);
        uint8_t (*img)[4] = blobs(37, 29);
        for (k = 0; k < 5; k++) {
            float *m = NULL;
            size_t len = 0;
            (void)pc_algo_selection_magic_wand((const float (*)[4])px, 37 * 29, area, wc[k].sx, wc[k].sy,
                                               wc[k].tol, wc[k].cont, wc[k].aa, &m, &len, NULL);
            line("sel.wand.%zu %016llx", k, X(of(m, len)));
            free(m);
        }
        for (k = 0; k < 6; k++) {
            pc_algo_region r;
            (void)pc_algo_selection_wand_region((const uint8_t (*)[4])img, 37 * 29, area, rc[k].sx, rc[k].sy,
                                                rc[k].tol, rc[k].cont, rc[k].aa, &r, NULL);
            line("sel.wand_region.%zu %s", k, oreg(&r, b1));
            if (!pc_algo_region_is_none(&r)) {
                pc_surface old;
                float one = 1.0f, f4 = 0.4f;
                int mode;
                pc_surface_init(&old, PC_PIXEL_FORMAT_GRAY8);
                (void)pc_surface_fill_rect(&old, pc_rect_make(5, 0, 25, 20), &one, 1, NULL);
                (void)pc_surface_fill_rect(&old, pc_rect_make(8, 8, 12, 10), &f4, 1, NULL);
                for (mode = 0; mode < 4; mode++) {
                    pc_surface o1, o2;
                    bool s1 = false, s2 = false;
                    (void)pc_algo_selection_combine_region(&old, &r, (pc_algo_selection_mode)mode, &o1, &s1,
                                                           NULL);
                    (void)pc_algo_selection_combine_region(NULL, &r, (pc_algo_selection_mode)mode, &o2, &s2,
                                                           NULL);
                    line("sel.combine_region.%zu.%s %s", k, MODE_NAMES[mode], osurf(s1, &o1, b1));
                    line("sel.combine_region_none.%zu.%s %s", k, MODE_NAMES[mode], osurf(s2, &o2, b2));
                    pc_surface_fini(&o1);
                    pc_surface_fini(&o2);
                }
                pc_surface_fini(&old);
            }
            pc_algo_region_fini(&r);
        }
        free(px);
        free(img);
    }
    for (k = 0; k < NFORMATS; k++) {
        pc_surface s;
        uint8_t (*v)[4] = NULL;
        size_t len = 0;
        surf(FORMATS[k].f, pc_rect_make(-5, -3, 30, 300), 4, &s);
        (void)pc_algo_selection_rgba8_image(&s, pc_rect_make(-7, -4, 28, 290), &v, &len, NULL);
        line("sel.rgba8.%s %016llx", FORMATS[k].name, X(o8((const uint8_t *)v, len * 4)));
        free(v);
        pc_surface_fini(&s);
    }
    /* Color Range variants. */
    {
        float (*px)[4] = rgba_img(16, 9, 5), mask[16 * 9];
        static const float fzs[6] = {0.0f, 10.0f, 40.0f, 200.0f, -5.0f, NAN};
        static const float color[3] = {0.5f, 0.5f, 0.1f};
        pc_algo_range_sample samples[3] = {
            {{0.0f, 1.0f, 0.0f}, true, 2.0f, 3.0f},
            {{1.0f, 0.0f, 0.05f}, false, 0.0f, 0.0f},
            {{0.5f, 0.5f, 0.25f}, true, 12.0f, 1.0f},
        };
        static const bool has_loc[3] = {false, true, true};
        static const float loc[3] = {0.0f, 4.0f, 0.5f};
        float (*lab_px)[4] = malloc(12 * 7 * sizeof *lab_px);
        pc_algo_lab_range range;
        static const float span[2][3] = {{50.0f, 0.0f, -5.0f}, {60.0f, 10.0f, 5.0f}};
        static const float lfz[5] = {0.0f, 1.0f, 10.0f, 40.0f, 150.0f};
        static const char *const lfzn[5] = {"0", "1", "10", "40", "150"};
        static const float pts[2][2] = {{1.0f, 1.0f}, {10.0f, 5.0f}};
        uint64_t h;
        int32_t ii;
        for (k = 0; k < 6; k++) {
            size_t li;
            pc_algo_color_range((const float (*)[4])px, 16 * 9, color, fzs[k], mask);
            line("sel.color_range.%zu %016llx", k, X(of(mask, 16 * 9)));
            for (li = 0; li < 3; li++) {
                pc_algo_selection_color_range_samples((const float (*)[4])px, 16 * 9, 16, samples, 3, fzs[k],
                                                      has_loc[li], loc[li], mask);
                line("sel.color_samples.%zu.%zu %016llx", k, li, X(of(mask, 16 * 9)));
            }
        }
        for (ii = 0; ii < 12 * 7; ii++) {
            lab_px[ii][0] = pat(ii, 1, 0, 3) * 100.0f;
            lab_px[ii][1] = pat(ii, 2, 1, 3) * 80.0f - 40.0f;
            lab_px[ii][2] = pat(ii, 3, 2, 3) * 60.0f - 30.0f;
            lab_px[ii][3] = ii % 13 == 0 ? 0.0f : 1.0f;
        }
        (void)pc_algo_lab_range_spanning(span, 2, &range);
        for (k = 0; k < 5; k++) {
            float lm[12 * 7];
            int g8;
            for (g8 = 0; g8 < 2; g8++) {
                (void)pc_algo_selection_color_range_lab((const float (*)[4])lab_px, 84, 12, &range, lfz[k],
                                                        g8 != 0, false, 0.0f, NULL, 0, lm, NULL);
                line("sel.lab.%zu.%s %016llx", k, g8 ? "true" : "false", X(of(lm, 84)));
                (void)pc_algo_selection_color_range_lab((const float (*)[4])lab_px, 84, 12, &range, lfz[k],
                                                        g8 != 0, true, 5.0f, pts, 2, lm, NULL);
                line("sel.lab_loc.%zu.%s %016llx", k, g8 ? "true" : "false", X(of(lm, 84)));
            }
            h = H0;
            for (ii = 0; ii < 84; ii++) {
                float q[3];
                hf1(&h, pc_algo_lab_range_coverage(lab_px[ii], &range, lfz[k]));
                hf1(&h, pc_algo_lab_range_coverage_8bit(lab_px[ii], &range, lfz[k]));
                pc_algo_quantize_lab8(lab_px[ii], q);
                hf(&h, q, 3);
            }
            line("sel.lab_cov.%zu %016llx", k, X(h));
            (void)lfzn;
        }
        h = H0;
        for (ii = 0; ii < 60; ii++) {
            pc_algo_lab_range r0 = {{40.0f, -10.0f, 0.0f}, {60.0f, 10.0f, 20.0f}}, r;
            float m[3] = {pat(ii, 0, 0, 1) * 100.0f, pat(ii, 0, 1, 1) * 60.0f - 30.0f,
                          pat(ii, 0, 2, 1) * 60.0f - 20.0f};
            static const float fz5[5] = {0.0f, 10.0f, 40.0f, 80.0f, NAN};
            float g[3];
            r = pc_algo_lab_range_subtract(r0, m, fz5[ii % 5]);
            g[0] = m[0] * 0.5f;
            g[1] = m[1];
            g[2] = -m[2];
            r = pc_algo_lab_range_grow(r, g);
            hf(&h, r.lo, 3);
            hf(&h, r.hi, 3);
        }
        line("sel.lab_subtract %016llx", X(h));
        {
            static const char *const cn[6] = {"0", "60", "120", "180", "240", "300"};
            for (k = 0; k < 6; k++) {
                pc_algo_selection_hue_range((const float (*)[4])px, 144, (float)(k * 60), mask);
                line("sel.hue.%s %016llx", cn[k], X(of(mask, 144)));
            }
        }
        {
            static const float tr[4][3] = {
                {0.0f, 65.0f, 20.0f}, {190.0f, 255.0f, 20.0f}, {105.0f, 150.0f, 0.0f}, {50.0f, 40.0f, -1.0f}};
            for (k = 0; k < 4; k++) {
                pc_algo_selection_tone_range((const float (*)[4])px, 144, tr[k][0], tr[k][1], tr[k][2], mask);
                line("sel.tone.%zu %016llx", k, X(of(mask, 144)));
            }
        }
        free(px);
        free(lab_px);
    }
    /* Modify. */
    {
        static const size_t dims[4][2] = {{20, 20}, {37, 23}, {1, 9}, {64, 1}};
        static const float rs[4] = {0.0f, 1.0f, 2.5f, 7.0f};
        static const char *const rn[4] = {"0", "1", "2.5", "7"};
        bool none[12] = {false};
        float *e = NULL;
        for (k = 0; k < 4; k++) {
            size_t w = dims[k][0], h = dims[k][1], j, ri;
            float *m = mask_field((int32_t)w, (int32_t)h, (int32_t)k), *o = NULL;
            bool *inside = malloc(w * h * sizeof(bool));
            for (j = 0; j < w * h; j++) {
                inside[j] = m[j] >= 0.5f;
            }
            (void)pc_algo_selection_edt(inside, w, h, &o, NULL);
            line("sel.edt.%zu %016llx", k, X(of(o, w * h)));
            free(o);
            for (ri = 0; ri < 4; ri++) {
                (void)pc_algo_selection_expand(m, w, h, rs[ri], &o, NULL);
                line("sel.expand.%zu.%s %016llx", k, rn[ri], X(of(o, w * h)));
                free(o);
                (void)pc_algo_selection_contract(m, w, h, rs[ri], &o, NULL);
                line("sel.contract.%zu.%s %016llx", k, rn[ri], X(of(o, w * h)));
                free(o);
                (void)pc_algo_selection_border(m, w, h, rs[ri], &o, NULL);
                line("sel.border.%zu.%s %016llx", k, rn[ri], X(of(o, w * h)));
                free(o);
            }
            free(inside);
            free(m);
        }
        (void)pc_algo_selection_edt(none, 4, 3, &e, NULL);
        line("sel.edt.none %016llx", X(of(e, 12)));
        free(e);
    }
    /* Polygons. */
    {
        static const float p0[4][2] = {{2.0f, 2.0f}, {12.0f, 2.0f}, {12.0f, 12.0f}, {2.0f, 12.0f}};
        static const float p1[3][2] = {{0.0f, 0.0f}, {10.0f, 0.0f}, {0.0f, 10.0f}};
        static const float p2[5][2] = {
            {3.3f, -4.0f}, {25.7f, 6.1f}, {14.2f, 19.9f}, {-3.0f, 11.5f}, {9.0f, 8.0f}};
        static const float p3[4][2] = {{5.0f, 5.0f}, {15.0f, 5.0f}, {5.0f, 15.0f}, {15.0f, 15.0f}};
        static const float p4[2][2] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
        const float (*polys[5])[2] = {p0, p1, p2, p3, p4};
        static const size_t np[5] = {4, 3, 5, 4, 2};
        for (k = 0; k < 5; k++) {
            int aa;
            for (aa = 1; aa >= 0; aa--) {
                float *m = NULL;
                size_t len = 0;
                (void)pc_algo_selection_polygon(polys[k], np[k], pc_rect_make(-2, -1, 20, 18), aa != 0, &m,
                                                &len, NULL);
                line("sel.polygon.%zu.%s %016llx", k, aa ? "true" : "false", X(of(m, len)));
                free(m);
            }
        }
    }
}

/* ---- magnetic ---- */

static bool fetch_disc(void *ctx, pc_rect r, uint8_t (*out)[4], size_t n)
{
    size_t i = 0;
    int32_t x, y;
    (void)ctx;
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1 && i < n; x++, i++) {
            uint8_t v = hypot((double)x + 0.5 - 50.0, (double)y + 0.5 - 50.0) < 30.0 ? 255 : 0;
            out[i][0] = out[i][1] = out[i][2] = v;
            out[i][3] = 255;
        }
    }
    return true;
}

static bool fetch_step(void *ctx, pc_rect r, uint8_t (*out)[4], size_t n)
{
    static const uint8_t lv[3] = {60, 100, 255};
    size_t i = 0;
    int32_t x, y;
    (void)ctx;
    for (y = r.y0; y < r.y1; y++) {
        for (x = r.x0; x < r.x1 && i < n; x++, i++) {
            uint8_t v = lv[(y >= 20) + (y >= 40)];
            out[i][0] = v;
            out[i][1] = (uint8_t)(v / 2 + (uint8_t)(x % 7));
            out[i][2] = v;
            out[i][3] = x % 13 == 0 ? 200 : 255;
        }
    }
    return true;
}

static bool fetch_bad(void *ctx, pc_rect r, uint8_t (*out)[4], size_t n)
{
    (void)ctx;
    (void)r;
    (void)out;
    (void)n;
    return false;
}

static void trace_line(const char *key, pc_algo_magnetic_tracer *t, pc_algo_magnetic_fetch f,
                       const double a[2], const double b[2], const double (*g)[2], size_t ng,
                       pc_algo_magnetic_settings s)
{
    double (*p)[2] = NULL;
    size_t n = 0;
    (void)pc_algo_magnetic_trace(t, f, NULL, a, b, g, ng, s, &p, &n, NULL);
    line("%s %016llx", key, X(opts((const double (*)[2])p, n)));
    free(p);
}

static void magnetic_cases(void)
{
    pc_algo_magnetic_tracer *t = NULL, *t2 = NULL, *t3 = NULL;
    pc_algo_magnetic_settings sets[3], s = pc_algo_magnetic_settings_default();
    uint64_t h = H0;
    int32_t i;
    size_t k;
    double a[2], b[2], q[2];
    bool some;
    sets[0] = s;
    sets[1] = pc_algo_magnetic_settings_new(3.0, 0.5f);
    sets[2] = pc_algo_magnetic_settings_new(30.0, 0.02f);
    (void)pc_algo_magnetic_tracer_new(pc_rect_make(0, 0, 100, 100), &t, NULL);
    for (i = 0; i < 40; i++) {
        double p[2] = {(double)pat(i, 1, 0, 2) * 100.0, (double)pat(i, 2, 1, 2) * 100.0};
        for (k = 0; k < 3; k++) {
            (void)pc_algo_magnetic_snap(t, fetch_disc, NULL, p, sets[k], q, &some, NULL);
            if (some) {
                hd(&h, q, 2);
            } else {
                h64(&h, 7);
            }
        }
    }
    line("mag.snap.disc %016llx", X(h));
    {
        double pa[2] = {50.0, 21.0}, pb[2] = {79.0, 50.0}, c0[2] = {20.0, 50.0}, c1[2] = {50.0, 80.0};
        static const double g[2][2] = {{25.0, 70.0}, {35.0, 78.0}};
        (void)pc_algo_magnetic_snap(t, fetch_disc, NULL, pa, s, a, &some, NULL);
        (void)pc_algo_magnetic_snap(t, fetch_disc, NULL, pb, s, b, &some, NULL);
        trace_line("mag.trace.disc", t, fetch_disc, a, b, NULL, 0, s);
        trace_line("mag.trace.disc_guide", t, fetch_disc, c0, c1, g, 2,
                   pc_algo_magnetic_settings_new(12.0, 0.2f));
        line("mag.cached.disc %zu", pc_algo_magnetic_cached_pixels(t));
    }
    (void)pc_algo_magnetic_tracer_new(pc_rect_make(0, 0, 300, 60), &t2, NULL);
    {
        double a0[2] = {10.0, 22.0}, b0[2] = {290.0, 18.0}, a1[2] = {1.0, 41.0}, b1[2] = {299.0, 39.0};
        double a2[2] = {-1e300, -1e9}, b2[2] = {1e9, 1e300};
        static const double g0[3][2] = {{60.0, 23.0}, {150.0, 25.0}, {200.0, 15.0}};
        double g2[2][2] = {{NAN, 3.0}, {1e12, -5.0}};
        pc_algo_magnetic_settings bad = {NAN, NAN};
        trace_line("mag.trace.step", t2, fetch_step, a0, b0, g0, 3,
                   pc_algo_magnetic_settings_new(10.0, 0.1f));
        trace_line("mag.trace.step_long", t2, fetch_step, a1, b1, NULL, 0,
                   pc_algo_magnetic_settings_new(6.0, 0.05f));
        trace_line("mag.trace.hostile", t2, fetch_step, a2, b2, (const double (*)[2])g2, 2, bad);
    }
    (void)pc_algo_magnetic_tracer_new(pc_rect_make(0, 0, 300, 300), &t3, NULL);
    {
        double a0[2] = {10.0, 10.0}, b0[2] = {290.0, 280.0};
        trace_line("mag.trace.bad", t3, fetch_bad, a0, b0, NULL, 0, s);
    }
    {
        double g[50][2], (*o)[2] = NULL;
        static const double tols[4] = {0.0, 0.3, 1.0, 5.0};
        static const char *const tn[4] = {"0", "0.3", "1", "5"};
        size_t n = 0;
        for (i = 0; i < 50; i++) {
            g[i][0] = (double)i * 3.0;
            g[i][1] = (double)pat(i, 0, 0, 1) * 2.0;
        }
        for (k = 0; k < 4; k++) {
            (void)pc_algo_magnetic_simplify((const double (*)[2])g, 50, tols[k], &o, &n, NULL);
            line("mag.simplify.%s %016llx", tn[k], X(opts((const double (*)[2])o, n)));
            free(o);
        }
        h = H0;
        hd1(&h, pc_algo_magnetic_length((const double (*)[2])g, 50));
        hd1(&h, pc_algo_magnetic_length(NULL, 0));
        hd1(&h, pc_algo_magnetic_length((const double (*)[2])g, 1));
        line("mag.length %016llx", X(h));
    }
    pc_algo_magnetic_tracer_free(t);
    pc_algo_magnetic_tracer_free(t2);
    pc_algo_magnetic_tracer_free(t3);
}

/* ---- matting ---- */

static void matting_cases(void)
{
    size_t w = 23, h = 17, k, i;
    float src[23 * 17], guide[23 * 17], *o = NULL, *m = mask_field(23, 17, 2);
    pc_algo_rgb_image img, edge, big;
    char b1[32];
    for (i = 0; i < w * h; i++) {
        src[i] = pat((int32_t)(i % 23), (int32_t)(i / 23), 0, 4);
    }
    {
        static const size_t rs[5] = {0, 1, 2, 5, 30};
        for (k = 0; k < 5; k++) {
            (void)pc_algo_matting_box_mean(src, w, h, rs[k], &o, NULL);
            line("mat.box_mean.%zu %016llx", rs[k], X(of(o, w * h)));
            free(o);
        }
    }
    (void)pc_algo_rgb_image_init(&img, w, h, NULL);
    for (i = 0; i < w * h; i++) {
        int32_t x = (int32_t)(i % w), y = (int32_t)(i / w);
        img.px[i][0] = pat(x, y, 0, 1);
        img.px[i][1] = pat(x, y, 1, 1);
        img.px[i][2] = pat(x, y, 2, 1);
        guide[i] = 0.3f * img.px[i][0] + 0.6f * img.px[i][1] + 0.1f * img.px[i][2];
    }
    {
        static const size_t rs[2] = {1, 3};
        static const float eps[2] = {1e-4f, 1e-2f};
        static const char *const en[2] = {"0.0001", "0.01"};
        size_t e;
        for (k = 0; k < 2; k++) {
            for (e = 0; e < 2; e++) {
                (void)pc_algo_matting_guided_filter_gray(guide, m, w, h, rs[k], eps[e], &o, NULL);
                line("mat.gf_gray.%zu.%s %016llx", rs[k], en[e], X(of(o, w * h)));
                free(o);
                (void)pc_algo_matting_guided_filter_color(&img, m, rs[k], eps[e], &o, NULL);
                line("mat.gf_color.%zu.%s %016llx", rs[k], en[e], X(of(o, w * h)));
                free(o);
            }
        }
    }
    {
        static const float sg[4] = {0.0f, 0.5f, 1.3f, 4.0f};
        static const char *const sn[4] = {"0", "0.5", "1.3", "4"};
        static const size_t mr[4] = {0, 1, 2, 5};
        static const size_t er[3] = {1, 2, 4};
        int grow;
        for (k = 0; k < 4; k++) {
            (void)pc_algo_matting_gaussian_blur(m, w, h, sg[k], &o, NULL);
            line("mat.gauss.%s %016llx", sn[k], X(of(o, w * h)));
            free(o);
        }
        for (k = 0; k < 4; k++) {
            for (grow = 1; grow >= 0; grow--) {
                (void)pc_algo_matting_morph(m, w, h, mr[k], grow != 0, &o, NULL);
                line("mat.morph.%zu.%s %016llx", mr[k], grow ? "true" : "false", X(of(o, w * h)));
                free(o);
            }
        }
        for (k = 0; k < 3; k++) {
            (void)pc_algo_matting_edge_width(&img, er[k], &o, NULL);
            line("mat.edge_width.%zu %016llx", er[k], X(of(o, w * h)));
            free(o);
        }
    }
    {
        pc_algo_refine_params params[6];
        size_t bw = 60, bh = 30, x, y;
        float *bm = malloc(bw * bh * sizeof(float));
        pc_algo_region reg, r;
        pc_algo_image_sampler hold;
        pc_algo_sampler smp;
        pc_surface surf_mask;
        memset(params, 0, sizeof params);
        params[0].radius = 10.0f;
        params[1].radius = 6.0f;
        params[1].smart_radius = true;
        params[2].feather = 3.0f;
        params[2].contrast = 40.0f;
        params[3].smooth = 50.0f;
        params[3].shift_edge = 60.0f;
        params[4].radius = 3.0f;
        params[4].shift_edge = -50.0f;
        params[4].smooth = 20.0f;
        params[4].feather = 1.0f;
        params[4].contrast = 10.0f;
        (void)pc_algo_rgb_image_init(&edge, bw, bh, NULL);
        for (y = 0; y < bh; y++) {
            for (x = 0; x < bw; x++) {
                float t = pc_clampf(((float)x - 30.0f) / 4.0f, -1.0f, 1.0f) * 0.5f + 0.5f +
                          pat((int32_t)x, (int32_t)y, 0, 6) * 0.02f;
                float *p = edge.px[y * bw + x];
                p[0] = 0.9f * (1.0f - t) + 0.1f * t;
                p[1] = 0.8f * (1.0f - t) + 0.2f * t;
                p[2] = 0.2f * (1.0f - t) + 0.6f * t;
            }
        }
        for (i = 0; i < bw * bh; i++) {
            bm[i] = (i % bw) < 30 + (i / bw) % 3 ? 1.0f : 0.0f;
        }
        for (k = 0; k < 6; k++) {
            line("mat.halo.%zu %d %d", k, pc_algo_refine_halo(&params[k]), pc_algo_refine_spread(&params[k]));
            (void)pc_algo_matting_refine_buffer(&edge, bm, bw, bh, &params[k], &o, NULL);
            line("mat.refine.%zu %016llx", k, X(of(o, bw * bh)));
            free(o);
            (void)pc_algo_matting_refine_buffer(NULL, bm, bw, bh, &params[k], &o, NULL);
            line("mat.refine_noimg.%zu %016llx", k, X(of(o, bw * bh)));
            free(o);
        }
        free(bm);
        /* Tiled refine through a sampler and a region reader. */
        disc_scene(300, 280, 150.0f, 140.0f, 90.0f, 7, &big);
        hold.img = &big;
        hold.ox = 0;
        hold.oy = 0;
        smp = pc_algo_image_sampler_make(&hold);
        reg.bbox = pc_rect_make(0, 0, 300, 280);
        reg.len = 300 * 280;
        reg.mask = malloc(reg.len);
        for (y = 0; y < 280; y++) {
            for (x = 0; x < 300; x++) {
                float dx = (float)x + 0.5f - 150.0f, dy = (float)y + 0.5f - 140.0f;
                reg.mask[y * 300 + x] = dx * dx + dy * dy < 88.0f * 88.0f   ? 255
                                        : dx * dx + dy * dy < 92.0f * 92.0f ? 128
                                                                            : 0;
            }
        }
        for (k = 0; k < 6; k++) {
            (void)pc_algo_matting_refine_mask(&smp, pc_algo_matting_region_reader, &reg,
                                              pc_rect_make(40, 30, 260, 250), reg.bbox, &params[k], &r, NULL);
            line("mat.refine_mask.%zu %s", k, oreg(&r, b1));
            pc_algo_region_fini(&r);
        }
        (void)pc_algo_matting_region_surface(&reg, &surf_mask, NULL);
        line("mat.region_surface %016llx", X(hs(&surf_mask)));
        (void)pc_algo_matting_refine_mask(&smp, pc_algo_matting_surface_reader, &surf_mask,
                                          pc_rect_make(40, 30, 260, 250), pc_rect_make(10, 10, 290, 270),
                                          &params[2], &r, NULL);
        line("mat.refine_mask.surface %s", oreg(&r, b1));
        pc_algo_region_fini(&r);
        pc_surface_fini(&surf_mask);
        pc_algo_region_fini(&reg);
        pc_algo_rgb_image_fini(&big);
        pc_algo_rgb_image_fini(&edge);
    }
    {
        pc_algo_region alpha;
        static const float dp[3][2] = {{3.0f, 100.0f}, {1.0f, 50.0f}, {6.5f, 75.0f}};
        alpha.bbox = pc_rect_make(-5, 3, 35, 27);
        alpha.len = 40 * 24;
        alpha.mask = malloc(alpha.len);
        for (i = 0; i < alpha.len; i++) {
            int32_t v = (int32_t)(i % 40) * 255 / 39;
            v = v < 255 ? v : 255;
            alpha.mask[i] = (uint8_t)(v < 90 ? 0 : (v > 200 ? 255 : v));
        }
        for (k = 0; k < NFORMATS; k++) {
            pc_surface s, d;
            size_t j;
            surf(FORMATS[k].f, pc_rect_make(-20, -10, 50, 40), 8, &s);
            for (j = 0; j < 3; j++) {
                (void)pc_algo_matting_decontaminate(&s, &alpha, dp[j][0], dp[j][1], &d, NULL);
                line("mat.decon.%s.%zu %016llx", FORMATS[k].name, j, X(hs(&d)));
                pc_surface_fini(&d);
            }
            (void)pc_algo_matting_masked_copy(&s, &alpha, &d, NULL);
            line("mat.masked.%s %016llx", FORMATS[k].name, X(hs(&d)));
            pc_surface_fini(&d);
            pc_surface_fini(&s);
        }
        pc_algo_region_fini(&alpha);
    }
    pc_algo_rgb_image_fini(&img);
    free(m);
}

#include "select_parity_segment.h"

#endif
