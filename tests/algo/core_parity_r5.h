/* R5 (upstream resync) part of test_core_parity.c: Photoshop's bicubic family and two-pass plans,
 * the measured Shadows/Highlights, Perspective Crop quads, clipped and horizon-crossing warps and
 * the float range warps keep. Mirrors r5_vectors in tools/rustref/src/algo_core_vectors.rs. */

static const pc_algo_resample RS8[8] = {PC_ALGO_RESAMPLE_NEAREST,         PC_ALGO_RESAMPLE_BILINEAR,
                                        PC_ALGO_RESAMPLE_BICUBIC,         PC_ALGO_RESAMPLE_BICUBIC_SMOOTHER,
                                        PC_ALGO_RESAMPLE_BICUBIC_SHARPER, PC_ALGO_RESAMPLE_BICUBIC_AUTOMATIC,
                                        PC_ALGO_RESAMPLE_LANCZOS,         PC_ALGO_RESAMPLE_PRESERVE_DETAILS};

static void r5_resample_vectors(void)
{
    static const double scales[14][2] = {{0.013, 0.013}, {0.2, 0.2},   {0.24, 0.3}, {0.37, 0.37}, {0.5, 0.5},
                                         {1.0, 1.0},     {1.37, 1.37}, {2.0, 2.0},  {2.3, 0.7},   {0.7, 2.3},
                                         {1.0, 0.01},    {0.6, 1.0},   {1.0, 1.6},  {3.1, 3.1}};
    static const double cks[8][2] = {{2.0, 2.0}, {1.37, 1.37}, {0.5, 0.5}, {0.11, 0.11},
                                     {0.2, 0.2}, {0.6, 1.7},   {1.7, 0.6}, {0.3, 0.9}};
    static const float fillv[5] = {0.2f, 0.6f, 1.0f, 1.0f, 0.5f};
    pc_rect canvas = pc_rect_make(0, 0, 30, 20);
    size_t fi, f, k;
    for (fi = 0; fi < NFMT; fi++) {
        pc_surface s;
        surf(&s, FORMATS[fi].f, pc_rect_make(-5, 3, 61, 37), 3, false);
        for (f = 0; f < 8; f++) {
            uint64_t h = H0;
            for (k = 0; k < 14; k++) {
                pc_surface o;
                hh(&h, hs_st(pc_algo_resize_surface(&s, scales[k][0], scales[k][1], RS8[f], &o, NULL), &o));
            }
            line("r5_resize %s %zu %016llx", FORMATS[fi].name, f, (unsigned long long)h);
        }
        pc_surface_fini(&s);
    }
    for (fi = 0; fi < NFMT; fi++) {
        pc_surface s, inner, edge;
        pc_surface_init(&s, FORMATS[fi].f);
        CHECK(pc_surface_fill_rect(&s, canvas, fillv, pc_pixel_format_channels(FORMATS[fi].f), NULL) ==
              PC_OK);
        surf(&inner, FORMATS[fi].f, pc_rect_make(10, 5, 20, 15), 4, false);
        surf(&edge, FORMATS[fi].f, canvas, 5, true);
        for (f = 0; f < 8; f++) {
            uint64_t h = H0;
            for (k = 0; k < 8; k++) {
                pc_surface o;
                double kx = cks[k][0], ky = cks[k][1];
                hh(&h, hs_st(pc_algo_resize_surface_in_canvas(&s, kx, ky, RS8[f], canvas, &o, NULL), &o));
                hh(&h, hs_st(pc_algo_resize_surface_in_canvas(&inner, kx, ky * 1.1, RS8[f], canvas, &o, NULL),
                             &o));
                hh(&h, hs_st(pc_algo_resize_surface_in_canvas(&edge, kx, ky, RS8[f], canvas, &o, NULL), &o));
            }
            line("r5_resize_canvas %s %zu %016llx", FORMATS[fi].name, f, (unsigned long long)h);
        }
        pc_surface_fini(&s), pc_surface_fini(&inner), pc_surface_fini(&edge);
    }
    {
        pc_surface mask, sparse;
        const float one = 1.0f, zero = 0.0f, quarter = 0.25f;
        const float white[4] = {1, 1, 1, 1}, other[4] = {0.5f, 0.25f, 1.0f, 1.0f};
        static const double mk[3] = {0.013, 0.37, 1.6};
        pc_surface_init_default(&mask, PC_PIXEL_FORMAT_GRAY8, &one, 1);
        CHECK(pc_surface_fill_rect(&mask, pc_rect_make(0, 0, 40, 30), &zero, 1, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&mask, pc_rect_make(500, 900, 530, 940), &quarter, 1, NULL) == PC_OK);
        pc_surface_init(&sparse, PC_PIXEL_FORMAT_RGBA32F);
        CHECK(pc_surface_fill_rect(&sparse, pc_rect_make(0, 0, 256, 256), white, 4, NULL) == PC_OK);
        CHECK(pc_surface_fill_rect(&sparse, pc_rect_make(99840, 99840, 100096, 100096), other, 4, NULL) ==
              PC_OK);
        for (f = 0; f < 8; f++) {
            uint64_t h = H0;
            pc_surface o;
            double kk = 1.0 / 100096.0;
            for (k = 0; k < 3; k++) {
                hh(&h, hs_st(pc_algo_resize_surface(&mask, mk[k], mk[k], RS8[f], &o, NULL), &o));
            }
            hh(&h, hs_st(pc_algo_resize_surface(&sparse, kk, kk, RS8[f], &o, NULL), &o));
            hh(&h, hs_st(pc_algo_resize_surface(&sparse, 1000.0 / 100096.0, kk, RS8[f], &o, NULL), &o));
            line("r5_resize_sparse %zu %016llx", f, (unsigned long long)h);
        }
        pc_surface_fini(&mask), pc_surface_fini(&sparse);
    }
}

static pc_algo_shadows_highlights_params r5_sh(float sa, float st, float sr, float ha, float ht, float hr,
                                               float color, float mid, float bc, float wc)
{
    pc_algo_shadows_highlights_params p;
    p.shadow_amount = sa, p.shadow_tone = st, p.shadow_radius = sr;
    p.highlight_amount = ha, p.highlight_tone = ht, p.highlight_radius = hr;
    p.color = color, p.midtone = mid, p.black_clip = bc, p.white_clip = wc;
    return p;
}

static void r5_tone_vectors(void)
{
    pc_algo_shadows_highlights_params sh[7];
    const size_t w = 61, h = 47;
    float (*big)[4] = rgba_px(w * h, 21);
    size_t i, k;
    /* Defaults: 35/50/30, 0/50/30, colour 20, midtone 0, clips 0.01. */
    sh[0] = r5_sh(35, 50, 30, 0, 50, 30, 0, 0, 0, 0);
    sh[1] = r5_sh(100, 100, 10, 0, 50, 30, 0, 0, 0, 0);
    sh[2] = r5_sh(15, 2.5f, 0.1f, 60, 73, 0.2f, 20, 0, 0.01f, 0.01f);
    sh[3] = r5_sh(50, 50, 12, 50, 50, 12, 100, 100, 0.01f, 0.01f);
    sh[4] = r5_sh(1, 50, 30, 99, 50, 40, -100, -60, 0.5f, 0);
    sh[5] = r5_sh(0, 50, 30, 0, 50, 30, 37, 12.5f, 0, 3);
    sh[6] = r5_sh(80, 95, 2500, 30, 0, 7.5f, -15, 0, 0.01f, 0.01f);
    for (i = 0; i < w * h; i++) {
        if (i % 13 == 0) {
            big[i][3] = 0.0f;
        }
    }
    for (k = 0; k < 7; k++) {
        float (*a)[4] = dup_px((const float (*)[4])big, w * h), (*r)[4] = ramp(40, 9);
        float (*g)[4] = malloc(121 * 121 * sizeof *g);
        for (i = 0; i < 121 * 121; i++) {
            bool c = i == 60 * 121 + 60;
            g[i][0] = c ? 0.25f : 0.15f, g[i][1] = c ? 0.125f : 0.15f, g[i][2] = c ? 0.0f : 0.15f,
            g[i][3] = 1.0f;
        }
        CHECK(pc_algo_shadows_highlights(a, w * h, w, h, &sh[k], NULL) == PC_OK);
        CHECK(pc_algo_shadows_highlights(r, 40 * 9, 40, 9, &sh[k], NULL) == PC_OK);
        CHECK(pc_algo_shadows_highlights(g, 121 * 121, 121, 121, &sh[k], NULL) == PC_OK);
        line("r5_shadows_highlights %zu %016llx %016llx %016llx", k,
             (unsigned long long)hpx((const float (*)[4])a, w * h),
             (unsigned long long)hpx((const float (*)[4])r, 360),
             (unsigned long long)hpx((const float (*)[4])g, 121 * 121));
        free(a), free(r), free(g);
    }
    free(big);
}

static void r5_transform_vectors(void)
{
    static const double quads[10][4][2] = {
        {{30.0, 12.0}, {140.0, 25.0}, {128.0, 110.0}, {14.0, 95.0}},
        {{0.0, 0.0}, {50.0, 0.0}, {100.0, 0.0}, {0.0, 50.0}},
        {{0.0, 0.0}, {50.0, 50.0}, {50.0, 0.0}, {0.0, 50.0}},
        {{0.0, 0.0}, {0.0, 50.0}, {50.0, 50.0}, {50.0, 0.0}},
        {{0.0, 0.0}, {NAN, 0.0}, {50.0, 50.0}, {0.0, 50.0}},
        {{0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
        {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}},
        {{0.0, 0.0}, {1.5, 0.0}, {1.5, 1.5}, {0.0, 1.5}},
        {{10.0, 10.0}, {1e9, 10.0}, {1e9, 2e9}, {10.0, 2e9}},
        {{0.0, 0.0}, {100.0, 0.0}, {100.0, 100.0}, {99.99, 100.0}},
    };
    const pc_algo_homography ids[8] = {
        PC_ALGO_HOMOGRAPHY_IDENTITY,
        {{2.0, 0.0, 0.0, 0.0, 2.0, 0.0, 0.0, 0.0, 2.0}},
        {{1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}},
        {{1.0, 0.1, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}},
        {{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1e-13}},
        {{-1.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, -1.0}},
        {{1.0 + 1e-10, 0.0, 1e-10, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}},
        {{NAN, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}},
    };
    static const fmt_entry fmts[5] = {
        {"rgba8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true}},
        {"rgba16", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}},
        {"rgba32", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true}},
        {"rgb8", {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false}},
        {"gray32", {PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_F32, false}},
    };
    static const char *const mnames[6] = {"steep", "horizon", "behind", "cross", "shrink", "translate"};
    static const double sq[4] = {0.0, 0.0, 100.0, 100.0};
    static const double steep_q[4][2] = {{0.0, 0.0}, {300.0, 0.0}, {155.0, 155.0}, {0.0, 300.0}};
    static const double shrink_q[4][2] = {{3.0, 3.0}, {20.0, 4.0}, {19.0, 18.0}, {2.0, 21.0}};
    pc_algo_homography ms[6];
    pc_rect src_r = pc_rect_make(0, 0, 100, 100), clips[5];
    uint64_t hq = H0;
    size_t q, i, fi, mi;
    for (q = 0; q < 10; q++) {
        double qw, qh, sizes[5][2];
        pc_algo_quad_size(quads[q], &qw, &qh);
        fnv(&hq, pc_algo_convex_quad(quads[q]) ? "c" : "n", 1);
        hd(&hq, (double[2]){qw, qh}, 2);
        sizes[0][0] = 100.0, sizes[0][1] = 80.0, sizes[1][0] = qw, sizes[1][1] = qh;
        sizes[2][0] = 0.0, sizes[2][1] = 10.0, sizes[3][0] = INFINITY, sizes[3][1] = 5.0;
        sizes[4][0] = 1e-3, sizes[4][1] = 1e-3;
        for (i = 0; i < 5; i++) {
            pc_algo_homography m;
            if (pc_algo_quad_to_rect(quads[q], sizes[i][0], sizes[i][1], &m)) {
                int c;
                hd(&hq, m.m, 9);
                for (c = 0; c < 4; c++) {
                    double p[2];
                    pc_algo_homography_apply(&m, quads[q][c][0], quads[q][c][1], &p[0], &p[1]);
                    hd(&hq, p, 2);
                }
            } else {
                fnv(&hq, "none", 4);
            }
        }
    }
    line("r5_quads %016llx", (unsigned long long)hq);
    hq = H0;
    for (i = 0; i < 8; i++) {
        fnv(&hq, pc_algo_homography_is_identity(&ids[i]) ? "i" : "n", 1);
    }
    line("r5_is_identity %016llx", (unsigned long long)hq);
    if (!pc_algo_homography_rect_to_quad(sq, steep_q, &ms[0])) {
        ms[0] = PC_ALGO_HOMOGRAPHY_IDENTITY;
    }
    ms[1] = (pc_algo_homography){{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.012, 0.0, 0.4}};
    ms[2] = (pc_algo_homography){{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, -0.01, -0.01, -0.5}};
    ms[3] = (pc_algo_homography){{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, -0.012, 0.0, 0.6}};
    if (!pc_algo_homography_rect_to_quad(sq, shrink_q, &ms[4])) {
        ms[4] = PC_ALGO_HOMOGRAPHY_IDENTITY;
    }
    ms[5] = (pc_algo_homography){{1.0, 0.0, 7.5, 0.0, 1.0, -3.25, 0.0, 0.0, 1.0}};
    clips[0] = pc_rect_make(0, 0, 300, 300), clips[1] = pc_rect_make(100, 100, 200, 200);
    clips[2] = pc_rect_make(-50, -50, 10, 10), clips[3] = pc_rect_make(240, 0, 256, 512);
    clips[4] = PC_RECT_EMPTY;
    for (fi = 0; fi < 5; fi++) {
        pc_surface s;
        size_t n = pc_pixel_format_channels(fmts[fi].f);
        surf(&s, fmts[fi].f, src_r, 9, false);
        if (fmts[fi].f.sample == PC_SAMPLE_TYPE_F32) {
            /* Signed and HDR samples keep their range through the warp. */
            float px[PC_RASTER_MAX_CHANNELS];
            for (i = 0; i < n; i++) {
                px[i] = 2.0f;
            }
            if (n > 1) {
                px[1] = -0.25f;
            }
            if (fmts[fi].f.alpha) {
                px[n - 1] = 0.5f;
            }
            CHECK(pc_surface_fill_rect(&s, pc_rect_make(40, 40, 60, 60), px, n, NULL) == PC_OK);
        }
        for (mi = 0; mi < 6; mi++) {
            uint64_t x = H0;
            int it;
            for (it = 0; it < 3; it++) {
                pc_surface o;
                size_t c;
                hh(&x, hs_st(pc_algo_warp_surface(&s, src_r, &ms[mi], (pc_algo_interp)it, &o, NULL), &o));
                for (c = 0; c < 5; c++) {
                    hh(&x, hs_st(pc_algo_warp_surface_clipped(&s, src_r, &ms[mi], (pc_algo_interp)it,
                                                              clips[c], &o, NULL),
                                 &o));
                }
            }
            line("r5_warp %s %s %016llx", fmts[fi].name, mnames[mi], (unsigned long long)x);
        }
        pc_surface_fini(&s);
    }
}

static void r5_fuse_vectors(void)
{
    static const size_t sizes[8][2] = {{64, 64}, {48, 100}, {2070, 1378}, {8280, 5520},
                                       {100000, 100000}, {0, 5}, {1, 1}, {63, 200}};
    static const size_t chs[3] = {1, 3, 4};
    const size_t w = 45, h = 37;
    uint64_t hl = H0;
    size_t i, c, k, n;
    for (i = 0; i < 8; i++) {
        h64(&hl, (uint64_t)pc_algo_stack_levels(sizes[i][0], sizes[i][1]));
    }
    line("r5_stack_levels %016llx", (unsigned long long)hl);
    for (c = 0; c < 3; c++) {
        size_t ch = chs[c];
        float *imgs[3], *lumas[3], *alphas[3];
        pc_algo_stack_fusion d = pc_algo_stack_fusion_default(), cases[8];
        for (k = 0; k < 3; k++) {
            imgs[k] = buf(w * h * ch, 30 + (int32_t)k);
            lumas[k] = buf(w * h, 40 + (int32_t)k);
            alphas[k] = malloc(w * h * sizeof(float));
            for (i = 0; i < w * h; i++) {
                alphas[k][i] = (i + k * 7) % 9 == 0 ? 0.25f : 1.0f;
            }
        }
        for (i = 0; i < 8; i++) {
            cases[i] = d;
        }
        cases[1].halo = 0.0f;
        cases[2].guide = 0;
        cases[3].energy_radius = 0;
        cases[4].energy_radius = 3, cases[4].halo = 100.0f;
        cases[5].has_levels = true, cases[5].levels = 1;
        cases[6].has_levels = true, cases[6].levels = 3, cases[6].halo = 0.0f;
        cases[7].has_levels = true, cases[7].levels = 40, cases[7].guide = 9;
        for (i = 0; i < 8; i++) {
            uint64_t hh2 = H0;
            for (n = 1; n <= 3; n++) {
                float *o = NULL;
                if (pc_algo_fuse_stack(w, h, ch, (const float *const *)imgs, (const float *const *)lumas,
                                       (const float *const *)alphas, n, cases[i], &o, NULL) == PC_OK) {
                    hf(&hh2, o, w * h * ch);
                } else {
                    h64(&hh2, 0xbad);
                }
                free(o);
            }
            line("r5_fuse_stack %zu %zu %016llx", ch, i, (unsigned long long)hh2);
        }
        for (k = 0; k < 3; k++) {
            free(imgs[k]), free(lumas[k]), free(alphas[k]);
        }
    }
}

static void r5_vectors(void)
{
    r5_resample_vectors();
    r5_tone_vectors();
    r5_transform_vectors();
    r5_fuse_vectors();
}
