/* R5 part of test_select_parity.c (included there): upstream's selection rewrite — hybrid
 * max-flow, sparse grid cut, min-cut Quick Selection, distance transform, Border / Smooth /
 * Feather, marquee tiles, octagonal morphology, fractional Shift Edge, Refine Edge Brush and the
 * transparent magic wand. Mirrors r5_* in tools/rustref/src/algo_select_vectors.rs. */

static const char *hst(const pc_surface *s, char *buf)
{
    (void)sprintf(buf, "%016llx %zu", X(hs(s)), pc_surface_tile_count(s));
    return buf;
}

/* Content-Aware Fill's cut weight. */
static float ca_weight(void *ctx, float dd)
{
    (void)ctx;
    return 8.0f * expf(-dd / (2.0f * 0.05f * 0.05f)) + 0.05f;
}

static bool always(void *ctx)
{
    (void)ctx;
    return true;
}

static void r5_maxflow_cases(void)
{
    static const size_t dims[4][2] = {{70, 60}, {64, 64}, {100, 41}, {20, 15}};
    pc_interrupt stop = {always, NULL, NULL};
    pc_algo_rgb_image scene;
    float *cost, *cost2;
    uint8_t *fixed;
    bool *a, *b;
    size_t k, i, w, hh, n;
    int kind;
    uint64_t variant;
    for (k = 0; k < 4; k++) {
        for (variant = 0; variant < 4; variant++) {
            pc_algo_segment_rng rng = pc_algo_segment_rng_new(100 + k * 10 + variant);
            pc_algo_maxflow *g = NULL, *c = NULL;
            uint64_t h = H0;
            double f = 0.0, fc = 0.0;
            char cbuf[32];
            pc_status st;
            w = dims[k][0];
            n = w * dims[k][1];
            (void)pc_algo_maxflow_new(n, n * 2, &g, NULL);
            for (i = 0; i < n; i++) {
                float s = pc_algo_segment_rng_f32(&rng) * 2.0f, t = pc_algo_segment_rng_f32(&rng) * 2.0f;
                if (variant == 1) {
                    pc_algo_maxflow_add_tweights(g, i, s, 0.0f);
                } else if (variant == 2) {
                    pc_algo_maxflow_add_tweights(g, i, i == 7 ? INFINITY : s, t);
                } else {
                    pc_algo_maxflow_add_tweights(g, i, s, t);
                }
                if (i % w + 1 < w) {
                    float ea = pc_algo_segment_rng_f32(&rng), eb = pc_algo_segment_rng_f32(&rng);
                    (void)pc_algo_maxflow_add_edge(g, i, i + 1, ea, variant == 3 && i == 5 ? -0.5f : eb,
                                                   NULL);
                }
                if (i + w < n) {
                    float ea = pc_algo_segment_rng_f32(&rng), eb = pc_algo_segment_rng_f32(&rng);
                    (void)pc_algo_maxflow_add_edge(g, i, i + w, ea, eb, NULL);
                }
            }
            (void)pc_algo_maxflow_clone(g, &c, NULL);
            (void)pc_algo_maxflow_run(g, &f, NULL);
            hd1(&h, f);
            hd1(&h, pc_algo_maxflow_flow(g));
            for (i = 0; i < n; i++) {
                uint8_t v = pc_algo_maxflow_in_source(g, i) ? 1 : 0;
                fnv(&h, &v, 1);
            }
            hf1(&h, pc_algo_maxflow_residual_out_of_source_side(g));
            st = pc_algo_maxflow_run_with(c, &stop, &fc, NULL);
            if (st == PC_OK) {
                uint64_t u;
                memcpy(&u, &fc, 8);
                (void)sprintf(cbuf, "%016llx", X(u));
            } else {
                (void)sprintf(cbuf, "none");
            }
            line("r5.maxflow.%zu.%llu %016llx %s", k, X(variant), X(h), cbuf);
            pc_algo_maxflow_free(g);
            pc_algo_maxflow_free(c);
        }
    }
    disc_scene(150, 133, 70.0f, 60.0f, 40.0f, 2, &scene);
    w = scene.w;
    hh = scene.h;
    n = w * hh;
    cost = malloc(n * sizeof(float));
    cost2 = malloc(n * sizeof(float));
    fixed = malloc(n);
    a = malloc(n * sizeof(bool));
    b = malloc(n * sizeof(bool));
    for (i = 0; i < n; i++) {
        cost[i] = (float)(i % 11) * 0.01f;
        cost2[i] = (float)(i % 7) * 0.013f;
    }
    for (kind = 0; kind < 4; kind++) {
        for (i = 0; i < n; i++) {
            size_t x = i % w, y = i / w;
            bool fr = kind == 0   ? (x >= 68 && x < 73)
                      : kind == 1 ? i % 503 == 0
                      : kind == 2 ? (y == 0 || x == w - 1)
                                  : (x * 3 + y) % 5 != 0;
            fixed[i] = fr ? PC_ALGO_SEGMENT_FREE : x < 70 ? PC_ALGO_SEGMENT_HARD_FG : PC_ALGO_SEGMENT_HARD_BG;
        }
        (void)pc_algo_segment_grid_cut_with(&scene, cost, cost2, fixed, ca_weight, NULL, NULL, a, NULL);
        (void)pc_algo_segment_grid_cut(&scene, cost, cost2, fixed, 50.0f,
                                       pc_algo_segment_contrast_beta(&scene), b, NULL);
        line("r5.grid_cut.%d %016llx %016llx", kind, X(ob(a, n)), X(ob(b, n)));
    }
    free(cost);
    free(cost2);
    free(fixed);
    free(a);
    free(b);
    pc_algo_rgb_image_fini(&scene);
}

static void qs_scene(size_t w, size_t h, const float bg[3], const float fg[3], size_t x0, size_t y0,
                     size_t x1, size_t y1, pc_algo_rgb_image *img)
{
    size_t x, y;
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const float *c = x >= x0 && x < x1 && y >= y0 && y < y1 ? fg : bg;
            memcpy(img->px[y * w + x], c, 3 * sizeof(float));
        }
    }
}
static void qs_square(size_t w, size_t side, const float fg[3], const float bg[3], pc_algo_rgb_image *img)
{
    size_t x0 = (w - side) / 2;
    qs_scene(w, w, bg, fg, x0, x0, x0 + side, x0 + side, img);
}
/* Runs Quick Selection on img (consumed) and prints the region. */
static void qs_line(const char *name, pc_algo_rgb_image *img, const float (*pts)[2], size_t n, float size)
{
    pc_algo_image_sampler hold = {img, 0, 0};
    pc_algo_sampler s = pc_algo_image_sampler_make(&hold);
    pc_algo_region r;
    char key[64];
    (void)sprintf(key, "r5.quick.%s", name);
    region_line(key,
                pc_algo_quick_select(&s, pc_rect_make(0, 0, (int32_t)img->w, (int32_t)img->h), pts, n, size,
                                     &r, NULL),
                &r);
    pc_algo_rgb_image_fini(img);
}

static void levels_image(int kind, size_t w, size_t h, pc_algo_rgb_image *img)
{
    size_t x, y;
    (void)pc_algo_rgb_image_init(img, w, h, NULL);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float *p = img->px[y * w + x], t = (float)((x * 23 + y * 19) % 101) / 100.0f;
            bool inside = kind == 2   ? hypotf((float)x - 180.0f, (float)y - 140.0f) < 72.0f
                          : kind == 3 ? (y > 130 && y < 145)
                                      : (x > 60 && x < 300 && y > 60 && y < 220);
            switch (kind) {
            case 0:
                p[0] = inside ? 0.6f + t * 0.1f : 0.4f;
                p[1] = inside ? 0.3f : 0.3f + t * 0.1f;
                p[2] = 0.2f;
                break;
            case 1:
                p[0] = 0.4f + (float)x / 1800.0f;
                p[1] = 0.3f + (float)y / 1400.0f;
                p[2] = t * 0.02f;
                break;
            case 4:
                p[0] = t;
                p[1] = (float)((x * 7 + y * 31) % 79) / 78.0f;
                p[2] = 0.4f;
                break;
            case 5:
                p[0] = p[1] = p[2] = 0.4f;
                break;
            default:
                p[0] = inside ? 0.8f + t * 0.1f : 0.2f;
                p[1] = inside ? 0.3f : 0.3f + t * 0.1f;
                p[2] = inside ? 0.2f : 0.8f;
                break;
            }
        }
    }
}

static void r5_quick_cases(void)
{
    static const size_t steps[7][2] = {{512, 512},   {513, 512}, {520, 300},   {900, 300},
                                       {5000, 3500}, {0, 9},     {SIZE_MAX, 3}};
    static const float red[3] = {200.0f / 255.0f, 60.0f / 255.0f, 60.0f / 255.0f};
    static const float blue[3] = {60.0f / 255.0f, 60.0f / 255.0f, 200.0f / 255.0f};
    static const float lo[3] = {125.0f / 255.0f, 110.0f / 255.0f, 110.0f / 255.0f};
    static const float hi[3] = {110.0f / 255.0f, 110.0f / 255.0f, 125.0f / 255.0f};
    static const float c320[1][2] = {{320.0f, 320.0f}}, c200[1][2] = {{200.0f, 200.0f}},
                       c250[1][2] = {{250.0f, 250.0f}}, bmid[1][2] = {{140.0f, 250.0f}},
                       bend[1][2] = {{140.0f, 420.0f}}, drag[2][2] = {{220.0f, 250.0f}, {280.0f, 250.0f}},
                       large[1][2] = {{500.5f, 300.5f}}, ddrag[2][2] = {{260.0f, 250.0f}, {340.0f, 250.0f}},
                       c32[1][2] = {{32.0f, 32.0f}};
    float hostile[3][2] = {{NAN, 3.0f}, {20.0f, INFINITY}, {30.0f, 30.0f}};
    static const float lp1[1][2] = {{180.0f, 140.0f}}, lp2[2][2] = {{90.0f, 140.0f}, {270.0f, 140.0f}},
                       lp3[2][2] = {{1.0f, 1.0f}, {160.0f, 130.0f}};
    static const float wp[2][2] = {{20.0f, 50.0f}, {30.0f, 52.0f}};
    static const size_t ups[5][3] = {{1, 13, 9}, {2, 26, 18}, {2, 25, 17}, {3, 39, 27}, {3, 37, 25}};
    pc_algo_rgb_image img, tr;
    pc_algo_quick_tuning t = pc_algo_quick_tuning_default(), t2;
    uint64_t h = H0;
    bool *seeds, *low, lowm[13 * 9];
    size_t i;
    int kind;
    for (i = 0; i < 7; i++) {
        h64(&h, pc_algo_quick_working_step(steps[i][0], steps[i][1]));
    }
    line("r5.quick.step %016llx", X(h));
    qs_square(640, 200, red, blue, &img);
    qs_line("half", &img, c320, 1, 30.0f);
    qs_scene(400, 400, red, red, 0, 0, 0, 0, &img);
    qs_line("flat", &img, c200, 1, 30.0f);
    qs_square(500, 220, red, blue, &img);
    qs_line("sq220", &img, c250, 1, 30.0f);
    qs_square(500, 240, red, blue, &img);
    qs_line("sq240", &img, c250, 1, 30.0f);
    qs_square(500, 94, lo, hi, &img);
    qs_line("weak94", &img, c250, 1, 30.0f);
    qs_square(500, 120, lo, hi, &img);
    qs_line("weak120", &img, c250, 1, 30.0f);
    qs_scene(500, 500, blue, red, 100, 50, 180, 450, &img);
    qs_line("bar_mid", &img, bmid, 1, 30.0f);
    qs_scene(500, 500, blue, red, 100, 50, 180, 450, &img);
    qs_line("bar_end", &img, bend, 1, 30.0f);
    qs_square(500, 300, red, blue, &img);
    qs_line("drag300", &img, drag, 2, 30.0f);
    qs_scene(1000, 600, blue, red, 20, 260, 980, 340, &img);
    qs_line("large", &img, large, 1, 30.0f);
    disc_scene(600, 500, 300.0f, 250.0f, 180.0f, 4, &img);
    qs_line("disc_drag", &img, ddrag, 2, 30.0f);
    qs_square(64, 20, red, blue, &img);
    qs_line("hostile", &img, (const float (*)[2])hostile, 3, NAN);
    qs_square(64, 20, red, blue, &img);
    qs_line("huge", &img, c32, 1, 1e30f);
    for (kind = 0; kind < 6; kind++) {
        pc_algo_image_sampler hold = {&img, 0, 0};
        pc_algo_sampler s;
        const float (*pts[3])[2] = {lp1, lp2, lp3};
        static const size_t np[3] = {1, 2, 2};
        static const float sizes[3] = {12.0f, 40.0f, 64.0f};
        static const size_t direct[2] = {10000, 300000};
        char buf[512], rb[32];
        size_t p, d, len = 0;
        levels_image(kind, 360, 280, &img);
        s = pc_algo_image_sampler_make(&hold);
        buf[0] = '\0';
        for (p = 0; p < 3; p++) {
            for (d = 0; d < 2; d++) {
                pc_algo_quick_tuning tt = pc_algo_quick_tuning_default();
                pc_algo_region r;
                tt.direct_px = direct[d];
                (void)pc_algo_quick_select_with(&s, pc_rect_make(0, 0, 360, 280), pts[p], np[p], sizes[p],
                                                &tt, &r, NULL);
                len += (size_t)sprintf(buf + len, "%s%s", len > 0 ? " " : "", oreg(&r, rb));
                pc_algo_region_fini(&r);
            }
        }
        line("r5.quick.levels.%d %s", kind, buf);
        pc_algo_rgb_image_fini(&img);
    }
    two_regions(140, 100, &tr);
    seeds = malloc(14000 * sizeof(bool));
    low = malloc(14000 * sizeof(bool));
    pc_algo_quick_stroke_mask(wp, 2, 4.0f, 140, 100, seeds);
    (void)pc_algo_quick_select_working(&tr, seeds, 4.0f, &t, low, NULL);
    line("r5.quick.working %016llx", X(ob(low, 14000)));
    t2 = t;
    t2.area = 0.02f;
    t2.pow = 1.5f;
    t2.grow = -3.0f;
    t2.sigma = 0.0f;
    t2.direct_px = 0;
    (void)pc_algo_quick_select_working(&tr, seeds, 2.0f, &t2, low, NULL);
    line("r5.quick.working_tuned %016llx", X(ob(low, 14000)));
    free(seeds);
    free(low);
    pc_algo_rgb_image_fini(&tr);
    for (i = 0; i < 13 * 9; i++) {
        lowm[i] = (i * 7 + i / 13) % 5 < 2;
    }
    for (i = 0; i < 5; i++) {
        size_t n = ups[i][1] * ups[i][2];
        uint8_t *o = malloc(n);
        (void)pc_algo_quick_upsample(lowm, 13, 9, ups[i][0], ups[i][1], ups[i][2], o, NULL);
        line("r5.quick.upsample.%zu.%zux%zu %016llx", ups[i][0], ups[i][1], ups[i][2], X(o8(o, n)));
        free(o);
    }
}

static float *square_mask(size_t w, size_t h)
{
    float *m = malloc(w * h * sizeof(float));
    size_t i;
    for (i = 0; i < w * h; i++) {
        m[i] = (i % w >= 16 && i % w < 48 && i / w >= 16 && i / w < 48) ? 1.0f : 0.0f;
    }
    return m;
}

static float op_max(void *ctx, float a, float b)
{
    (void)ctx;
    return pc_rs_maxf(a, b);
}
static float op_sub(void *ctx, float a, float b)
{
    (void)ctx;
    return a * (1.0f - b);
}
static float op_min(void *ctx, float a, float b)
{
    (void)ctx;
    return pc_rs_minf(a, b);
}

static void edt_line(const char *key, const bool *inside, size_t w, size_t h)
{
    float *d = NULL;
    (void)pc_algo_selection_edt(inside, w, h, &d, NULL);
    line("%s %016llx", key, X(of(d, w * h)));
    free(d);
}

/* Prints a selection modifier on the 64 x 64 square and a 37 x 23 mask field. */
static void modify_line(const char *key, int which, const float *sq, const float *mf, float r)
{
    float *a = NULL, *b = NULL;
    if (which == 0) {
        (void)pc_algo_selection_border(sq, 64, 64, r, &a, NULL);
        (void)pc_algo_selection_border(mf, 37, 23, r, &b, NULL);
    } else if (which == 1) {
        (void)pc_algo_selection_smooth(sq, 64 * 64, 64, 64, r, &a, NULL);
        (void)pc_algo_selection_smooth(mf, 37 * 23, 37, 23, r, &b, NULL);
    } else {
        (void)pc_algo_selection_feather(sq, 64 * 64, 64, 64, r, &a, NULL);
        (void)pc_algo_selection_feather(mf, 37 * 23, 37, 23, r, &b, NULL);
    }
    line("%s %016llx %016llx", key, X(of(a, 64 * 64)), X(of(b, 37 * 23)));
    free(a);
    free(b);
}

static void r5_selection_cases(void)
{
    static const size_t edims[5][2] = {{513, 137}, {1, 101}, {101, 1}, {73, 65}, {9, 255}};
    static const float br[7] = {4.0f, 1.0f, 2.5f, 0.0f, -1.0f, NAN, 100.0f};
    static const char *bkeys[7] = {"4", "1", "2.5", "0", "-1", "NaN", "100"};
    static const float sr[4] = {8.0f, 1.0f, 2.5f, 0.4f}, fr[4] = {4.0f, 1.0f, 2.5f, 12.0f};
    static const char *skeys[4] = {"8", "1", "2.5", "0.4"}, *fkeys[4] = {"4", "1", "2.5", "12"};
    static const uint8_t pixels[6][4] = {{255, 0, 0, 0},   {0, 255, 0, 0}, {255, 0, 0, 128},
                                         {253, 0, 0, 128}, {0, 0, 255, 0}, {255, 0, 0, 255}};
    static const struct {
        int32_t sx;
        float tol;
        bool cont;
    } wands[6] = {{0, 0.0f, true},  {0, 0.0f, false}, {4, 32.0f, false},
                  {2, 0.0f, false}, {2, 2.0f, true},  {5, 32.0f, false}};
    static const int32_t shapes[9][4] = {
        {40, 30, 1000, 620},      {3, 5, 1, 1},        {10, 10, 2, 7},
        {100, 200, 37, 301},      {255, 255, 258, 2},  {-300, -200, 900, 600},
        {-500, -500, 2100, 1700}, {1090, 690, 40, 40}, {2000, 0, 50, 50}};
    pc_algo_combine_fn ops[3] = {op_max, op_sub, op_min};
    pc_rect area, canvas = pc_rect_make(0, 0, 1100, 700);
    float floats[6][4], *sq, *mf, *full, *old_m;
    pc_surface old, rect, ell, rgba, s;
    char buf[64], key[64];
    size_t k, i, w, h, n;
    bool *inside;
    int aa, mode;
    for (k = 0; k < 5; k++) {
        w = edims[k][0];
        h = edims[k][1];
        inside = malloc(w * h * sizeof(bool));
        for (i = 0; i < w * h; i++) {
            inside[i] = (i * 101 + i / w * 37) % 29 == 0;
        }
        (void)sprintf(key, "r5.edt.%zu", k);
        edt_line(key, inside, w, h);
        free(inside);
    }
    for (k = 0; k < 2; k++) {
        w = k == 0 ? 65537 : 2;
        h = k == 0 ? 2 : 65537;
        inside = calloc(w * h, sizeof(bool));
        if (w > h) {
            inside[w - 3] = inside[w - 2] = inside[2 * w - 1] = true;
        } else {
            inside[(h - 3) * w] = inside[(h - 2) * w] = inside[h * w - 1] = true;
        }
        (void)sprintf(key, "r5.edt.far.%zux%zu", w, h);
        edt_line(key, inside, w, h);
        free(inside);
    }
    inside = malloc(6000 * sizeof(bool));
    for (i = 0; i < 6000; i++) {
        inside[i] = true;
    }
    edt_line("r5.edt.all", inside, 6000, 1);
    free(inside);
    sq = square_mask(64, 64);
    mf = mask_field(37, 23, 3);
    for (k = 0; k < 7; k++) {
        (void)sprintf(key, "r5.border.%s", bkeys[k]);
        modify_line(key, 0, sq, mf, br[k]);
    }
    for (k = 0; k < 4; k++) {
        (void)sprintf(key, "r5.smooth.%s", skeys[k]);
        modify_line(key, 1, sq, mf, sr[k]);
    }
    for (k = 0; k < 4; k++) {
        (void)sprintf(key, "r5.feather.%s", fkeys[k]);
        modify_line(key, 2, sq, mf, fr[k]);
    }
    free(sq);
    free(mf);
    /* Magic wand over hidden RGB. */
    for (k = 0; k < 6; k++) {
        for (i = 0; i < 4; i++) {
            floats[k][i] = (float)pixels[k][i] / 255.0f;
        }
    }
    area = pc_rect_make(0, 0, 6, 1);
    for (k = 0; k < 6; k++) {
        for (aa = 0; aa < 2; aa++) {
            float *m = NULL;
            size_t len = 0;
            pc_algo_region r;
            char rb[32];
            (void)pc_algo_selection_magic_wand((const float (*)[4])floats, 6, area, wands[k].sx, 0,
                                               wands[k].tol, wands[k].cont, aa != 0, &m, &len, NULL);
            (void)pc_algo_selection_wand_region(pixels, 6, area, wands[k].sx, 0, wands[k].tol, wands[k].cont,
                                                aa != 0, &r, NULL);
            line("r5.wand.%zu.%s %016llx %s", k, aa ? "true" : "false", X(of(m, len)), oreg(&r, rb));
            free(m);
            pc_algo_region_fini(&r);
        }
    }
    /* Combining with fully selected tiles. */
    area = pc_rect_make(-37, -300, 530, 263);
    w = pc_rect_width(area);
    h = pc_rect_height(area);
    n = w * h;
    full = malloc(n * sizeof(float));
    for (i = 0; i < n; i++) {
        full[i] = i >= 1000 ? 1.0f : 0.0f;
    }
    old_m = mask_field((int32_t)w, (int32_t)h, 1);
    (void)pc_algo_selection_mask_to_surface(old_m, n, area, &old, NULL);
    (void)pc_algo_selection_mask_to_surface(full, n, area, &s, NULL);
    line("r5.to_surface.full %s", hst(&s, buf));
    pc_surface_fini(&s);
    for (mode = 0; mode < 4; mode++) {
        bool some = false;
        (void)pc_algo_selection_combine(&old, full, n, area, (pc_algo_selection_mode)mode, &s, &some, NULL);
        line("r5.combine.full.%s %s", MODE_NAMES[mode], some ? hst(&s, buf) : "none");
        pc_surface_fini(&s);
    }
    pc_surface_fini(&old);
    free(full);
    free(old_m);
    /* Marquee shapes and tile-wise combination. */
    for (k = 0; k < 9; k++) {
        for (aa = 1; aa >= 0; aa--) {
            pc_rect r =
                pc_rect_from_xywh(shapes[k][0], shapes[k][1], (uint32_t)shapes[k][2], (uint32_t)shapes[k][3]);
            (void)pc_algo_selection_ellipse_surface(r, canvas, aa != 0, &s, NULL);
            line("r5.ellipse.%zu.%s %s", k, aa ? "true" : "false", hst(&s, buf));
            pc_surface_fini(&s);
        }
    }
    {
        float one = 1.0f;
        const pc_surface *pa[4], *pb[4];
        pc_surface_init(&rect, PC_PIXEL_FORMAT_GRAY8);
        (void)pc_surface_fill_rect(&rect, pc_rect_from_xywh(200, 100, 600, 520), &one, 1, NULL);
        (void)pc_algo_selection_ellipse_surface(pc_rect_from_xywh(-60, 250, 700, 500), canvas, true, &ell,
                                                NULL);
        surf((pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true}, pc_rect_make(0, 0, 300, 200), 3,
             &rgba);
        pa[0] = &rect, pb[0] = &ell;
        pa[1] = &ell, pb[1] = &rect;
        pa[2] = &ell, pb[2] = &ell;
        pa[3] = &rgba, pb[3] = &ell;
        for (k = 0; k < 4; k++) {
            for (i = 0; i < 3; i++) {
                (void)pc_algo_selection_combine_surfaces(pa[k], pb[k], canvas, ops[i], NULL, &s, NULL);
                line("r5.combine_surfaces.%zu.%zu %s", k, i, hst(&s, buf));
                pc_surface_fini(&s);
            }
        }
        (void)pc_algo_selection_combine_surfaces(&rect, &ell, pc_rect_make(37, -20, 611, 333), op_max, NULL,
                                                 &s, NULL);
        line("r5.combine_surfaces.part %s", hst(&s, buf));
        pc_surface_fini(&s);
        pc_surface_fini(&rect);
        pc_surface_fini(&ell);
        pc_surface_fini(&rgba);
    }
}

static uint64_t hmorph(const float *src, size_t w, size_t h, size_t r, bool grow)
{
    float *o = NULL;
    uint64_t v;
    (void)pc_algo_matting_morph(src, w, h, r, grow, &o, NULL);
    v = of(o, w * h);
    free(o);
    return v;
}

static uint64_t hrefine(const pc_algo_rgb_image *img, const float *m, size_t w, size_t h,
                        const pc_algo_refine_params *p)
{
    float *o = NULL;
    uint64_t v;
    (void)pc_algo_matting_refine_buffer(img, m, w, h, p, &o, NULL);
    v = of(o, w * h);
    free(o);
    return v;
}

static void r5_matting_cases(void)
{
    static const size_t rs[7] = {3, 4, 7, 10, 33, 64, SIZE_MAX};
    static const size_t sr[5] = {1, 3, 4, 17, 64};
    static const struct {
        float radius, shift;
    } shifts[8] = {{0.0f, 12.5f},  {0.0f, -12.5f}, {0.0f, 37.0f}, {6.0f, 21.0f},
                   {6.0f, -70.0f}, {0.0f, 0.001f}, {3.0f, NAN},   {1e30f, 50.0f}};
    size_t w = 23, h = 17, i, k, x, y, bw = 60, bh = 30;
    float *m = mask_field(23, 17, 2), *gray = malloc(w * h * sizeof(float)), *big, *bm;
    float specials[3];
    pc_algo_rgb_image edge;
    int grow;
    uint32_t sb[3] = {0x7fc00001u, 0xff800000u, 0x80000000u};
    for (i = 0; i < w * h; i++) {
        gray[i] = (float)((i * 101 + i / w * 37) % 251) / 250.0f * 4.0f - 1.0f;
    }
    for (k = 0; k < 7; k++) {
        for (grow = 1; grow >= 0; grow--) {
            line("r5.morph.%zu.%s %016llx %016llx", rs[k], grow ? "true" : "false",
                 X(hmorph(m, w, h, rs[k], grow)), X(hmorph(gray, w, h, rs[k], grow)));
        }
    }
    for (k = 0; k < 3; k++) {
        float src[13 * 9];
        uint64_t hh = H0;
        size_t r;
        memcpy(&specials[k], &sb[k], 4);
        for (i = 0; i < 13 * 9; i++) {
            src[i] = i % 7 == 0 ? specials[k] : (float)(i % 11) / 10.0f;
        }
        for (r = 0; r < 5; r++) {
            for (grow = 0; grow < 2; grow++) {
                float *o = NULL;
                (void)pc_algo_matting_morph(src, 13, 9, sr[r], grow != 0, &o, NULL);
                hf(&hh, o, 13 * 9);
                free(o);
            }
        }
        line("r5.morph.special.%08x %016llx", sb[k], X(hh));
    }
    big = malloc(700 * 400 * sizeof(float));
    for (i = 0; i < 700 * 400; i++) {
        big[i] = fmodf((float)(i % 700) / 1000.0f + (float)(i / 700) / 3000.0f, 0.7f);
    }
    line("r5.morph.big %016llx %016llx", X(hmorph(big, 700, 400, 33, false)),
         X(hmorph(big, 700, 400, 9, true)));
    free(big);
    free(m);
    free(gray);
    /* Fractional Shift Edge. */
    bm = malloc(bw * bh * sizeof(float));
    for (i = 0; i < bw * bh; i++) {
        bm[i] = (i % bw) < 30 + (i / bw) % 3 ? 1.0f : 0.0f;
    }
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
    for (k = 0; k < 8; k++) {
        pc_algo_refine_params p = pc_algo_refine_params_default();
        p.radius = shifts[k].radius;
        p.shift_edge = shifts[k].shift;
        line("r5.shift.%zu %d %d %016llx %016llx", k, pc_algo_refine_halo(&p), pc_algo_refine_spread(&p),
             X(hrefine(&edge, bm, bw, bh, &p)), X(hrefine(NULL, bm, bw, bh, &p)));
    }
    free(bm);
    pc_algo_rgb_image_fini(&edge);
    /* Refine Edge Brush. */
    {
        static const float padd[2][2] = {{10.0f, 25.0f}, {90.0f, 25.0f}}, psub[1][2] = {{50.0f, 25.0f}},
                           plong[5][2] = {{3.0f, 3.0f},
                                          {3.2f, 3.1f},
                                          {40.0f, 47.5f},
                                          {97.0f, 1.0f},
                                          {97.0f, 1.0f}},
                           poff[1][2] = {{-500.0f, -500.0f}}, ptiny[1][2] = {{20.5f, 20.5f}},
                           phuge[2][2] = {{0.0f, 0.0f}, {1e6f, 0.0f}};
        float pnan[1][2] = {{NAN, 3.0f}};
        pc_algo_edge_stroke add = {padd, 2, 10.0f, true}, sub = {psub, 1, 20.0f, false},
                            lng = {plong, 5, 7.5f, true}, off = {poff, 1, 10.0f, true},
                            nan = {(const float (*)[2])pnan, 1, 10.0f, true}, empty = {NULL, 0, 10.0f, true},
                            tiny = {ptiny, 1, 0.1f, false}, huge = {phuge, 2, 5000.0f, true};
        pc_algo_edge_stroke sets[7][3] = {{add},  {add, sub}, {sub, add}, {lng, tiny}, {off, nan, empty},
                                          {huge}, {tiny}};
        static const size_t ns[7] = {1, 2, 2, 2, 3, 1, 1};
        pc_rect canvas = pc_rect_make(0, 0, 100, 50), rr = pc_rect_make(-3, -2, 104, 53);
        float *rd = malloc(107 * 55 * sizeof(float));
        for (k = 0; k < 7; k++) {
            double swept = pc_algo_edge_brush_swept_area(sets[k], ns[k]);
            pc_algo_edge_brush b;
            bool some = false;
            uint64_t u;
            char desc[32];
            memcpy(&u, &swept, 8);
            (void)pc_algo_edge_brush_new(sets[k], ns[k], canvas, &b, &some, NULL);
            if (some) {
                uint64_t hh = H0;
                hrect(&hh, b.bbox);
                hrect(&hh, b.added);
                h64(&hh, b.window);
                pc_algo_edge_brush_read(&b, rr, rd, 107 * 55);
                hf(&hh, rd, 107 * 55);
                (void)sprintf(desc, "%016llx", X(hh));
                pc_algo_edge_brush_fini(&b);
            } else {
                (void)sprintf(desc, "none");
            }
            line("r5.brush.%zu %016llx %s", k, X(u), desc);
        }
        free(rd);
    }
    /* Brushing over strands beyond the band, and restoring a blurred edge. */
    {
        static const float fg[3] = {0.9f, 0.7f, 0.2f}, bg[3] = {0.1f, 0.3f, 0.8f};
        static const float p70[2][2] = {{70.0f, 10.0f}, {70.0f, 50.0f}},
                           p40[2][2] = {{40.0f, 0.0f}, {40.0f, 60.0f}}, p110[1][2] = {{110.0f, 5.0f}};
        pc_algo_edge_stroke over = {p70, 2, 30.0f, true}, rest = {p40, 2, 20.0f, false},
                            dab = {p110, 1, 9.0f, true};
        pc_algo_edge_stroke sets[4][3] = {{over}, {over}, {rest}, {over, rest, dab}};
        static const size_t ns[4] = {0, 1, 1, 3};
        size_t sw = 120, shh = 60;
        pc_algo_rgb_image img, blurred;
        pc_algo_image_sampler h1 = {&img, 0, 0}, h2 = {&blurred, 0, 0};
        pc_algo_sampler s1, s2;
        pc_rect canvas = pc_rect_make(0, 0, 120, 60);
        pc_algo_region reg;
        int pi;
        (void)pc_algo_rgb_image_init(&img, sw, shh, NULL);
        (void)pc_algo_rgb_image_init(&blurred, sw, shh, NULL);
        for (y = 0; y < shh; y++) {
            for (x = 0; x < sw; x++) {
                bool strand = x < 40 || (x >= 60 && x < 84 && x % 4 == 0);
                float t = pc_clampf(((float)x - 36.0f) / 8.0f, 0.0f, 1.0f);
                int c;
                memcpy(img.px[y * sw + x], strand ? fg : bg, 3 * sizeof(float));
                for (c = 0; c < 3; c++) {
                    blurred.px[y * sw + x][c] = fg[c] * (1.0f - t) + bg[c] * t;
                }
            }
        }
        s1 = pc_algo_image_sampler_make(&h1);
        s2 = pc_algo_image_sampler_make(&h2);
        reg.bbox = canvas;
        reg.len = sw * shh;
        reg.mask = malloc(reg.len);
        for (i = 0; i < reg.len; i++) {
            reg.mask[i] = i % sw < 40 ? 255 : 0;
        }
        for (k = 0; k < 4; k++) {
            pc_algo_edge_brush b;
            bool some = false;
            (void)pc_algo_edge_brush_new(sets[k], ns[k], canvas, &b, &some, NULL);
            for (pi = 0; pi < 2; pi++) {
                pc_algo_refine_params p = pc_algo_refine_params_default();
                pc_algo_region r1, r2;
                char b1[32], b2[32];
                if (pi == 0) {
                    p.radius = 4.0f;
                } else {
                    p.feather = 2.0f;
                    p.shift_edge = 10.0f;
                }
                (void)pc_algo_matting_refine_mask_brushed(&s1, pc_algo_matting_region_reader, &reg, reg.bbox,
                                                          canvas, &p, some ? &b : NULL, &r1, NULL);
                (void)pc_algo_matting_refine_mask_brushed(&s2, pc_algo_matting_region_reader, &reg, reg.bbox,
                                                          canvas, &p, some ? &b : NULL, &r2, NULL);
                line("r5.brushed.%zu.%d %s %s", k, pi, oreg(&r1, b1), oreg(&r2, b2));
                pc_algo_region_fini(&r1);
                pc_algo_region_fini(&r2);
            }
            if (some) {
                pc_algo_edge_brush_fini(&b);
            }
        }
        pc_algo_region_fini(&reg);
        pc_algo_rgb_image_fini(&img);
        pc_algo_rgb_image_fini(&blurred);
    }
}
