/* Segmentation cases of test_select_parity.c (included by select_parity_cases.h). */
#ifndef PC_TESTS_ALGO_SELECT_PARITY_SEGMENT_H
#define PC_TESTS_ALGO_SELECT_PARITY_SEGMENT_H

static void gmm_line(const char *key, pc_status st, const bool *some, pc_algo_gmm *g)
{
    if (st != PC_OK || !*some) {
        line("%s none", key);
        return;
    }
    line("%s %016llx", key, X(ogmm(g)));
    pc_algo_gmm_fini(g);
}

static void segment_core_cases(void)
{
    float (*rgba)[4] = rgba_img(13, 11, 1);
    pc_algo_rgb_image img, scene, tmp;
    pc_algo_image_sampler hold;
    pc_algo_sampler smp;
    pc_algo_segment_rng rng;
    uint64_t h;
    size_t k, i, w, hh;
    char b1[32];
    (void)pc_algo_rgb_image_from_rgba((const float (*)[4])rgba, 13 * 11, 13, 11, &img, NULL);
    free(rgba);
    line("seg.from_rgba %016llx", X(orgb(&img)));
    {
        static const size_t steps[5] = {1, 2, 3, 5, 20};
        for (k = 0; k < 5; k++) {
            (void)pc_algo_rgb_image_downsample(&img, steps[k], &tmp, NULL);
            line("seg.downsample.%zu %016llx", steps[k], X(orgb(&tmp)));
            pc_algo_rgb_image_fini(&tmp);
        }
    }
    pc_algo_rgb_image_fini(&img);
    disc_scene(150, 133, 70.0f, 60.0f, 40.0f, 2, &scene);
    line("seg.disc_scene %016llx", X(orgb(&scene)));
    hold.img = &scene;
    hold.ox = 5;
    hold.oy = 3;
    smp = pc_algo_image_sampler_make(&hold);
    {
        static const pc_rect rs[4] = {
            {5, 3, 155, 136}, {0, 0, 160, 140}, {20, 30, 21, 200}, {10, 10, 60, 50}};
        static const size_t st[4] = {4, 3, 7, 1};
        for (k = 0; k < 4; k++) {
            (void)pc_algo_sampler_rgb_scaled(&smp, rs[k], st[k], &tmp, NULL);
            line("seg.rgb_scaled.%zu %016llx", k, X(orgb(&tmp)));
            pc_algo_rgb_image_fini(&tmp);
        }
    }
    for (k = 0; k < NFORMATS; k++) {
        pc_surface sf;
        pc_algo_sampler ss;
        surf(FORMATS[k].f, pc_rect_make(0, 0, 40, 30), 2, &sf);
        ss = pc_algo_surface_sampler(&sf);
        (void)pc_algo_sampler_rgb_scaled(&ss, pc_rect_make(-3, -2, 41, 33), 2, &tmp, NULL);
        line("seg.surface_sampler.%s %016llx", FORMATS[k].name, X(orgb(&tmp)));
        pc_algo_rgb_image_fini(&tmp);
        pc_surface_fini(&sf);
    }
    line("seg.scale_for.640x480.20000 %zu", pc_algo_segment_scale_for(pc_rect_make(0, 0, 640, 480), 20000));
    line("seg.scale_for.100x100.1000000 %zu",
         pc_algo_segment_scale_for(pc_rect_make(0, 0, 100, 100), 1000000));
    line("seg.scale_for.7x3.0 %zu", pc_algo_segment_scale_for(pc_rect_make(0, 0, 7, 3), 0));
    {
        pc_algo_rgb_image z4, z0;
        float b[3];
        (void)pc_algo_rgb_image_init(&z4, 4, 4, NULL);
        (void)pc_algo_rgb_image_init(&z0, 0, 0, NULL);
        b[0] = pc_algo_segment_contrast_beta(&scene);
        b[1] = pc_algo_segment_contrast_beta(&z4);
        b[2] = pc_algo_segment_contrast_beta(&z0);
        line("seg.beta %016llx", X(of(b, 3)));
        pc_algo_rgb_image_fini(&z4);
    }
    rng = pc_algo_segment_rng_new(42);
    h = H0;
    for (i = 0; i < 50; i++) {
        float a, b;
        h64(&h, pc_algo_segment_rng_next(&rng));
        a = pc_algo_segment_rng_f32(&rng);
        b = pc_algo_segment_rng_normal(&rng);
        hf1(&h, a);
        hf1(&h, b);
    }
    line("seg.rng %016llx", X(h));
    /* Max-flow on random graphs. */
    rng = pc_algo_segment_rng_new(7);
    h = H0;
    for (k = 0; k < 60; k++) {
        size_t n = 3 + (k % 12), j;
        pc_algo_maxflow *g = NULL;
        double flow = 0.0;
        (void)pc_algo_maxflow_new(n, n * 3, &g, NULL);
        for (i = 0; i < n; i++) {
            float s = roundf(pc_algo_segment_rng_f32(&rng) * 10.0f),
                  t = roundf(pc_algo_segment_rng_f32(&rng) * 10.0f);
            s = pc_algo_segment_rng_f32(&rng) < 0.4f ? 0.0f : s;
            t = pc_algo_segment_rng_f32(&rng) < 0.4f ? 0.0f : t;
            pc_algo_maxflow_add_tweights(g, i, s, t);
        }
        for (i = 0; i < n; i++) {
            for (j = i + 1; j < n; j++) {
                if (pc_algo_segment_rng_f32(&rng) < 0.35f) {
                    float a = roundf(pc_algo_segment_rng_f32(&rng) * 8.0f);
                    float b;
                    a = a + pc_algo_segment_rng_f32(&rng) * 0.25f;
                    b = roundf(pc_algo_segment_rng_f32(&rng) * 8.0f);
                    (void)pc_algo_maxflow_add_edge(g, i, j, a, b, NULL);
                }
            }
        }
        (void)pc_algo_maxflow_run(g, &flow, NULL);
        hd1(&h, flow);
        for (i = 0; i < n; i++) {
            uint8_t v = pc_algo_maxflow_in_source(g, i) ? 1 : 0;
            fnv(&h, &v, 1);
        }
        hf1(&h, pc_algo_maxflow_residual_out_of_source_side(g));
        pc_algo_maxflow_free(g);
    }
    line("seg.maxflow %016llx", X(h));
    /* Grid cut, components, cleaning. */
    w = scene.w;
    hh = scene.h;
    {
        pc_algo_gmm fg, bg;
        bool sf = false, sb = false;
        float *cf = malloc(w * hh * sizeof(float)), *cb = malloc(w * hh * sizeof(float));
        float *zeros = calloc(w * hh, sizeof(float));
        uint8_t *fixed = calloc(w * hh, 1), *m8;
        bool *cut = malloc(w * hh * sizeof(bool)), *tmpb = malloc(w * hh * sizeof(bool));
        bool *pm = malloc(w * hh * sizeof(bool)), *seeds = malloc(w * hh * sizeof(bool));
        float beta;
        uint32_t *lab = NULL;
        size_t *sizes = NULL, ns = 0;
        static const float cl[3][2] = {{0.1f, 0.002f}, {0.0f, 0.5f}, {0.5f, 0.0f}};
        static const char *const cln[3] = {"0.1.0.002", "0.0.5", "0.5.0"};
        pc_algo_region reg, r;
        bool *low;
        pc_algo_image_sampler h0;
        pc_algo_sampler s0;
        (void)pc_algo_gmm_fit((const float (*)[3])scene.px, 3000, 3, PC_ALGO_GMM_DEFAULT_REG, &fg, &sf, NULL);
        (void)pc_algo_gmm_fit((const float (*)[3])(scene.px + 15000), w * hh - 15000, 4,
                              PC_ALGO_GMM_DEFAULT_REG, &bg, &sb, NULL);
        (void)pc_algo_segment_data_costs(&scene, &fg, &bg, cf, cb, NULL);
        line("seg.data_costs %016llx %016llx", X(of(cf, w * hh)), X(of(cb, w * hh)));
        fixed[60 * w + 70] = PC_ALGO_SEGMENT_HARD_FG;
        fixed[5 * w + 5] = PC_ALGO_SEGMENT_HARD_BG;
        fixed[120 * w + 140] = PC_ALGO_SEGMENT_HARD_BG;
        beta = pc_algo_segment_contrast_beta(&scene);
        (void)pc_algo_segment_grid_cut(&scene, cf, cb, fixed, 50.0f, beta, cut, NULL);
        line("seg.grid_cut %016llx", X(ob(cut, w * hh)));
        (void)pc_algo_segment_grid_cut(&scene, zeros, zeros, fixed, 50.0f, beta, tmpb, NULL);
        line("seg.grid_cut_zero %016llx", X(ob(tmpb, w * hh)));
        (void)pc_algo_segment_components(cut, w, hh, &lab, &sizes, &ns, NULL);
        h = H0;
        for (i = 0; i < ns; i++) {
            h64(&h, sizes[i]);
        }
        for (i = 0; i < w * hh; i++) {
            hi32(&h, (int32_t)lab[i]);
        }
        line("seg.components %016llx", X(h));
        free(lab);
        free(sizes);
        for (i = 0; i < w * hh; i++) {
            pm[i] = pat((int32_t)(i % w), (int32_t)(i / w), 0, 3) > 0.55f;
            seeds[i] = i % 97 == 0;
        }
        (void)pc_algo_segment_keep_seeded(pm, seeds, w, hh, tmpb, NULL);
        line("seg.keep_seeded %016llx", X(ob(tmpb, w * hh)));
        for (k = 0; k < 3; k++) {
            (void)pc_algo_segment_clean_mask(pm, w, hh, cl[k][0], cl[k][1], tmpb, NULL);
            line("seg.clean.%s %016llx", cln[k], X(ob(tmpb, w * hh)));
            (void)pc_algo_segment_clean_mask(cut, w, hh, cl[k][0], cl[k][1], tmpb, NULL);
            line("seg.clean_cut.%s %016llx", cln[k], X(ob(tmpb, w * hh)));
        }
        m8 = malloc(w * hh);
        for (i = 0; i < w * hh; i++) {
            m8[i] = cut[i] ? 255 : 0;
        }
        (void)pc_algo_segment_antialias_u8(m8, w, hh, NULL);
        line("seg.antialias %016llx", X(o8(m8, w * hh)));
        reg.bbox = pc_rect_make(-4, 7, -4 + (int32_t)w, 7 + (int32_t)hh);
        reg.mask = m8;
        reg.len = w * hh;
        (void)pc_algo_segment_trim_region(&reg, &r, NULL);
        line("seg.trim %s", oreg(&r, b1));
        pc_algo_region_fini(&r);
        reg.bbox = pc_rect_make(0, 0, 3, 2);
        reg.mask = calloc(6, 1);
        reg.len = 6;
        (void)pc_algo_segment_trim_region(&reg, &r, NULL);
        line("seg.trim_empty %s", oreg(&r, b1));
        low = malloc(38 * 34 * sizeof(bool));
        for (i = 0; i < 38 * 34; i++) {
            float x = (float)(i % 38) + 0.5f - 17.5f, y = (float)(i / 38) + 0.5f - 15.0f;
            low[i] = x * x + y * y < 100.0f;
        }
        h0.img = &scene;
        h0.ox = 0;
        h0.oy = 0;
        s0 = pc_algo_image_sampler_make(&h0);
        (void)pc_algo_segment_finish_region(&s0, pc_rect_make(0, 0, 150, 133), 4, low, 38, 34, &fg, &bg, &r,
                                            NULL);
        line("seg.finish.step4 %s", oreg(&r, b1));
        pc_algo_region_fini(&r);
        (void)pc_algo_segment_finish_region(&s0, pc_rect_make(0, 0, 150, 133), 4, low, 38, 34, NULL, NULL, &r,
                                            NULL);
        line("seg.finish.step4_nomodel %s", oreg(&r, b1));
        pc_algo_region_fini(&r);
        (void)pc_algo_segment_finish_region(&s0, pc_rect_make(0, 0, 38, 34), 1, low, 38, 34, NULL, NULL, &r,
                                            NULL);
        line("seg.finish.step1 %s", oreg(&r, b1));
        pc_algo_region_fini(&r);
        free(low);
        pc_algo_gmm_fini(&fg);
        pc_algo_gmm_fini(&bg);
        free(cf);
        free(cb);
        free(zeros);
        free(fixed);
        free(cut);
        free(tmpb);
        free(pm);
        free(seeds);
    }
    {
        static const size_t ms[4] = {1000, 999, 7, 1};
        for (k = 0; k < 4; k++) {
            float (*s)[3] = NULL;
            size_t n = 0;
            (void)pc_algo_segment_subsample((const float (*)[3])scene.px, 1000, ms[k], &s, &n, NULL);
            h = H0;
            h64(&h, n);
            hf(&h, (const float *)s, n * 3);
            line("seg.subsample.%zu %016llx", ms[k], X(h));
            free(s);
        }
    }
    pc_algo_rgb_image_fini(&scene);
    /* GMM fitting. */
    {
        static const float truth[3][5] = {{0.2f, 0.3f, 0.8f, 0.03f, 0.5f},
                                          {0.8f, 0.2f, 0.1f, 0.05f, 0.3f},
                                          {0.5f, 0.9f, 0.4f, 0.02f, 0.2f}};
        float (*samples)[3] = malloc(4000 * sizeof *samples), flat[10][3];
        size_t ns = 0, t, *labels;
        pc_algo_gmm g, r2;
        bool ok = false, ok2 = false;
        static const size_t ks[3] = {1, 3, 5};
        static const size_t kk[4] = {1, 2, 7, 40};
        char key[64];
        rng = pc_algo_segment_rng_new(3);
        for (t = 0; t < 3; t++) {
            size_t cnt = (size_t)(truth[t][4] * 3000.0f), j;
            for (j = 0; j < cnt; j++) {
                int c;
                for (c = 0; c < 3; c++) {
                    samples[ns][c] = truth[t][c] + truth[t][3] * pc_algo_segment_rng_normal(&rng);
                }
                ns++;
            }
        }
        for (k = 0; k < 3; k++) {
            pc_status st = pc_algo_gmm_fit((const float (*)[3])samples, ns, ks[k], PC_ALGO_GMM_DEFAULT_REG,
                                           &g, &ok, NULL);
            pc_status st2 = pc_algo_gmm_refit(&g, (const float (*)[3])samples, ns, &r2, &ok2, NULL);
            (void)sprintf(key, "seg.gmm.fit.%zu", ks[k]);
            gmm_line(key, st, &ok, &g);
            (void)sprintf(key, "seg.gmm.refit.%zu", ks[k]);
            gmm_line(key, st2, &ok2, &r2);
        }
        gmm_line("seg.gmm.fit_em0",
                 pc_algo_gmm_fit_em((const float (*)[3])samples, ns, 4, 1e-4f, 0, &g, &ok, NULL), &ok, &g);
        gmm_line("seg.gmm.fit_em6",
                 pc_algo_gmm_fit_em((const float (*)[3])samples, ns, 2, 1e-3f, 6, &g, &ok, NULL), &ok, &g);
        gmm_line("seg.gmm.few",
                 pc_algo_gmm_fit((const float (*)[3])samples, 2, 5, PC_ALGO_GMM_DEFAULT_REG, &g, &ok, NULL),
                 &ok, &g);
        for (i = 0; i < 10; i++) {
            flat[i][0] = flat[i][1] = flat[i][2] = 0.5f;
        }
        gmm_line("seg.gmm.flat", pc_algo_gmm_fit((const float (*)[3])flat, 10, 3, 0.0f, &g, &ok, NULL), &ok,
                 &g);
        (void)pc_algo_gmm_fit(NULL, 0, 3, 0.0f, &g, &ok, NULL);
        line("seg.gmm.empty %s", ok ? "false" : "true");
        /* R5: seg.gmm.broad dropped (quick::broad_model removed upstream). */
        labels = malloc(ns * sizeof(size_t));
        for (i = 0; i < ns; i++) {
            labels[i] = i % 4;
        }
        gmm_line("seg.gmm.labels",
                 pc_algo_gmm_from_labels((const float (*)[3])samples, ns, labels, 4, PC_ALGO_GMM_DEFAULT_REG,
                                         &g, &ok, NULL),
                 &ok, &g);
        free(labels);
        for (k = 0; k < 4; k++) {
            float (*c)[3] = NULL;
            size_t nc = 0;
            (void)pc_algo_gmm_kmeans((const float (*)[3])samples, ns, kk[k], 10, &c, &nc, NULL);
            h = H0;
            h64(&h, nc);
            hf(&h, (const float *)c, nc * 3);
            line("seg.kmeans.%zu %016llx", kk[k], X(h));
            free(c);
        }
        free(samples);
    }
}

static void region_line(const char *key, pc_status st, pc_algo_region *r)
{
    char b1[32];
    if (st != PC_OK) {
        line("%s ERR", key);
        return;
    }
    line("%s %s", key, oreg(r, b1));
    pc_algo_region_fini(r);
}

static void segment_algo_cases(void)
{
    pc_algo_rgb_image scene, img, big, tr, qs, land, overcast, sub, fimg;
    pc_algo_image_sampler hold;
    pc_algo_sampler smp;
    pc_algo_region r;
    uint64_t h = H0;
    size_t k, i, x, y;
    float lab[3];
    int32_t ii;
    for (ii = 0; ii < 64; ii++) {
        float c[3] = {pat(ii, 0, 0, 1), pat(ii, 0, 1, 1), pat(ii, 0, 2, 1)};
        pc_algo_slic_rgb_to_lab(c, lab);
        hf(&h, lab, 3);
    }
    {
        static const float sp[3][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.01f, 0.02f, 0.03f}};
        for (k = 0; k < 3; k++) {
            pc_algo_slic_rgb_to_lab(sp[k], lab);
            hf(&h, lab, 3);
        }
    }
    line("seg.rgb_to_lab %016llx", X(h));
    disc_scene(120, 90, 60.0f, 45.0f, 22.0f, 8, &scene);
    {
        static const size_t tg[4] = {64, 16, 400, 1}, it[4] = {10, 3, 8, 1};
        static const float mm[4] = {10.0f, 20.0f, 5.0f, 10.0f};
        for (k = 0; k < 4; k++) {
            pc_algo_superpixels sp;
            (void)pc_algo_slic(&scene, tg[k], mm[k], it[k], &sp, NULL);
            h = H0;
            h64(&h, sp.count);
            hf1(&h, sp.step);
            for (i = 0; i < sp.w * sp.h; i++) {
                hi32(&h, (int32_t)sp.labels[i]);
            }
            line("seg.slic.%zu %016llx", k, X(h));
            pc_algo_superpixels_fini(&sp);
        }
    }
    /* GrabCut. */
    disc_scene(120, 100, 60.0f, 50.0f, 30.0f, 1, &img);
    {
        uint8_t *tri = calloc(120 * 100, 1), tri2[64];
        pc_algo_gmm f, b;
        bool some = false;
        pc_algo_rgb_image e8;
        for (y = 12; y < 88; y++) {
            for (x = 22; x < 98; x++) {
                tri[y * 120 + x] = PC_ALGO_GRABCUT_PR_FG;
            }
        }
        (void)pc_algo_grabcut(&img, tri, 8, &f, &b, &some, NULL);
        if (some) {
            line("seg.grabcut %016llx %016llx %016llx", X(o8(tri, 12000)), X(ogmm(&f)), X(ogmm(&b)));
            pc_algo_gmm_fini(&f);
            pc_algo_gmm_fini(&b);
        } else {
            line("seg.grabcut %016llx none", X(o8(tri, 12000)));
        }
        free(tri);
        memset(tri2, PC_ALGO_GRABCUT_PR_FG, 64);
        (void)pc_algo_rgb_image_init(&e8, 8, 8, NULL);
        (void)pc_algo_grabcut(&e8, tri2, 3, &f, &b, &some, NULL);
        line("seg.grabcut_onesided %s", some ? "false" : "true");
        if (some) {
            pc_algo_gmm_fini(&f);
            pc_algo_gmm_fini(&b);
        }
        pc_algo_rgb_image_fini(&e8);
    }
    hold.img = &img;
    hold.ox = 0;
    hold.oy = 0;
    smp = pc_algo_image_sampler_make(&hold);
    region_line("seg.object_select.full",
                pc_algo_grabcut_object_select(&smp, pc_rect_make(0, 0, 120, 100),
                                              pc_rect_make(22, 12, 98, 88), 1000000, &r, NULL),
                &r);
    disc_scene(320, 240, 165.0f, 125.0f, 75.0f, 3, &big);
    {
        pc_algo_image_sampler hb = {&big, 0, 0};
        pc_algo_sampler sb = pc_algo_image_sampler_make(&hb);
        region_line("seg.object_select.band",
                    pc_algo_grabcut_object_select(&sb, pc_rect_make(0, 0, 320, 240),
                                                  pc_rect_make(80, 40, 250, 210), 8000, &r, NULL),
                    &r);
    }
    region_line("seg.object_select.all",
                pc_algo_grabcut_object_select(&smp, pc_rect_make(0, 0, 120, 100),
                                              pc_rect_make(-5, -5, 200, 200), 1000000, &r, NULL),
                &r);
    region_line("seg.object_select.tiny",
                pc_algo_grabcut_object_select(&smp, pc_rect_make(0, 0, 120, 100), pc_rect_make(5, 5, 6, 60),
                                              1000000, &r, NULL),
                &r);
    /* Quick selection. */
    two_regions(140, 100, &tr);
    line("seg.two_regions %016llx", X(orgb(&tr)));
    {
        static const float p1[1][2] = {{20.0f, 50.0f}};
        static const float p3[3][2] = {{3.5f, 4.0f}, {30.25f, 20.0f}, {-5.0f, 60.0f}};
        static const float st2[2][2] = {{20.0f, 50.0f}, {35.0f, 50.0f}};
        static const float off[1][2] = {{3e9f, 0.0f}};
        static const float dn[2][2] = {{130.0f, 125.0f}, {170.0f, 125.0f}};
        bool *seeds = malloc(14000 * sizeof(bool)), sm2[40 * 50];
        pc_algo_image_sampler ht = {&tr, 0, 0}, hq;
        pc_algo_sampler st;
        pc_algo_quick_stroke_mask(p1, 1, 4.0f, 140, 100, seeds);
        line("seg.stroke_mask %016llx", X(ob(seeds, 14000)));
        pc_algo_quick_stroke_mask(p3, 3, 2.5f, 40, 50, sm2);
        line("seg.stroke_mask2 %016llx", X(ob(sm2, 2000)));
        /* R5: seg.geodesic and seg.quick_segment dropped (removed upstream by the min-cut rewrite). */
        st = pc_algo_image_sampler_make(&ht);
        region_line("seg.quick.grow",
                    pc_algo_quick_select(&st, pc_rect_make(0, 0, 140, 100), st2, 2, 8.0f, &r, NULL), &r);
        region_line("seg.quick.off",
                    pc_algo_quick_select(&st, pc_rect_make(0, 0, 140, 100), off, 1, 8.0f, &r, NULL), &r);
        disc_scene(300, 250, 150.0f, 125.0f, 90.0f, 4, &qs);
        hq.img = &qs;
        hq.ox = 0;
        hq.oy = 0;
        st = pc_algo_image_sampler_make(&hq);
        region_line("seg.quick.down",
                    pc_algo_quick_select(&st, pc_rect_make(0, 0, 300, 250), dn, 2, 20.0f, &r, NULL), &r);
        free(seeds);
    }
    /* Sky. */
    h = H0;
    for (i = 0; i < 125; i++) {
        float c[3] = {(float)(i % 5) / 4.0f, (float)((i / 5) % 5) / 4.0f, (float)(i / 25) / 4.0f};
        hf1(&h, pc_algo_sky_colour_prior(c));
    }
    line("seg.sky.prior %016llx", X(h));
    landscape(160, 100, &land);
    line("seg.sky.landscape %016llx", X(orgb(&land)));
    {
        float *lk = NULL;
        static const float ts[3] = {0.0f, 50.0f, 100.0f};
        static const char *const tn[3] = {"0", "50", "100"};
        pc_algo_sky_params ps[3] = {{50.0f, 50.0f}, {20.0f, 0.0f}, {80.0f, 100.0f}};
        pc_algo_image_sampler hl = {&land, 0, 0};
        pc_algo_sampler sl = pc_algo_image_sampler_make(&hl);
        char key[64];
        (void)pc_algo_sky_likelihood(&land, &lk, NULL);
        line("seg.sky.likelihood %016llx", X(of(lk, 16000)));
        free(lk);
        for (k = 0; k < 3; k++) {
            bool *m = NULL, some = false;
            (void)pc_algo_sky_mask_low(&land, ts[k], &m, &some, NULL);
            if (some) {
                line("seg.sky.low.%s %016llx", tn[k], X(ob(m, 16000)));
                free(m);
            } else {
                line("seg.sky.low.%s none", tn[k]);
            }
        }
        for (k = 0; k < 3; k++) {
            (void)sprintf(key, "seg.sky.select.%zu", k);
            region_line(key, pc_algo_sky_select(&sl, pc_rect_make(0, 0, 160, 100), ps[k], &r, NULL), &r);
        }
        (void)pc_algo_rgb_image_init(&overcast, 64, 64, NULL);
        for (i = 0; i < 64 * 64; i++) {
            float *p = overcast.px[i];
            if (i / 64 < 30) {
                p[0] = 0.85f;
                p[1] = 0.86f;
                p[2] = 0.88f;
            } else {
                p[0] = 0.2f;
                p[1] = 0.3f;
                p[2] = 0.1f;
            }
        }
        hl.img = &overcast;
        region_line(
            "seg.sky.overcast",
            pc_algo_sky_select(&sl, pc_rect_make(0, 0, 64, 64), pc_algo_sky_params_default(), &r, NULL), &r);
    }
    /* Subject. */
    {
        float *sal = NULL, v[500], o4[4];
        static const float tens[10] = {0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f};
        float odd[3] = {NAN, 2.0f, -1.0f};
        pc_algo_image_sampler hs2;
        pc_algo_sampler ss;
        (void)pc_algo_subject_saliency(&scene, &sal, NULL);
        line("seg.saliency %016llx", X(of(sal, scene.w * scene.h)));
        free(sal);
        for (ii = 0; ii < 500; ii++) {
            v[ii] = pat(ii, 3, 1, 2);
        }
        o4[0] = pc_algo_subject_otsu(v, 500);
        o4[1] = pc_algo_subject_otsu(NULL, 0);
        o4[2] = pc_algo_subject_otsu(tens, 10);
        o4[3] = pc_algo_subject_otsu(odd, 3);
        line("seg.otsu %016llx", X(of(o4, 4)));
        disc_scene(200, 150, 100.0f, 75.0f, 40.0f, 6, &sub);
        hs2.img = &sub;
        hs2.ox = 0;
        hs2.oy = 0;
        ss = pc_algo_image_sampler_make(&hs2);
        region_line("seg.subject", pc_algo_subject_select(&ss, pc_rect_make(0, 0, 200, 150), &r, NULL), &r);
    }
    /* Focus. */
    {
        pc_algo_segment_rng rng = pc_algo_segment_rng_new(12);
        size_t fw = 160, fh = 100;
        float *noise = malloc(fw * fh * sizeof(float)), *blurred = NULL, *s = NULL;
        static const float rn[3][2] = {{0.5f, 0.0f}, {0.1f, 0.3f}, {0.9f, 0.0f}};
        static const char *const rk[3] = {"0.5.0", "0.1.0.3", "0.9.0"};
        pc_algo_image_sampler hf2;
        pc_algo_sampler sf;
        char key[64];
        for (i = 0; i < fw * fh; i++) {
            noise[i] = pc_algo_segment_rng_f32(&rng);
        }
        (void)pc_algo_matting_box_mean(noise, fw, fh, 4, &blurred, NULL);
        (void)pc_algo_rgb_image_init(&fimg, fw, fh, NULL);
        for (y = 0; y < fh; y++) {
            for (x = 0; x < fw; x++) {
                float val = x < 80 ? noise[y * fw + x] : blurred[y * fw + x];
                fimg.px[y * fw + x][0] = val;
                fimg.px[y * fw + x][1] = val * 0.8f;
                fimg.px[y * fw + x][2] = 0.3f;
            }
        }
        (void)pc_algo_focus_sharpness(&fimg, 0.0f, &s, NULL);
        line("seg.sharpness.0 %016llx", X(of(s, fw * fh)));
        free(s);
        (void)pc_algo_focus_sharpness(&fimg, 0.4f, &s, NULL);
        line("seg.sharpness.0.4 %016llx", X(of(s, fw * fh)));
        free(s);
        hf2.img = &fimg;
        hf2.ox = 0;
        hf2.oy = 0;
        sf = pc_algo_image_sampler_make(&hf2);
        for (k = 0; k < 3; k++) {
            (void)sprintf(key, "seg.focus.%s", rk[k]);
            region_line(
                key, pc_algo_focus_area(&sf, pc_rect_make(0, 0, 160, 100), rn[k][0], rn[k][1], &r, NULL), &r);
        }
        free(noise);
        free(blurred);
    }
    pc_algo_rgb_image_fini(&scene);
    pc_algo_rgb_image_fini(&img);
    pc_algo_rgb_image_fini(&big);
    pc_algo_rgb_image_fini(&tr);
    pc_algo_rgb_image_fini(&qs);
    pc_algo_rgb_image_fini(&land);
    pc_algo_rgb_image_fini(&overcast);
    pc_algo_rgb_image_fini(&sub);
    pc_algo_rgb_image_fini(&fimg);
}

#endif
