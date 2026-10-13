/* The per-line handlers of tests/paint/test_parity.c (included once from there). */

/* ---- rng / tiles / procedural ---- */

static void do_rng(char **f)
{
    uint64_t seed = strtoull(f[1], NULL, 10), h = H0;
    char got[17];
    for (uint64_t k = 0; k < 303; k++) {
        uint64_t i = k < 300 ? k : (k == 300 ? UINT64_MAX : (k == 301 ? UINT64_MAX - 1 : (uint64_t)1 << 63));
        for (uint64_t st = 0; st < 22; st++) {
            f32b(&h, pc_paint_rand01(seed, i, st));
            f32b(&h, pc_paint_rand_signed(seed, i, st));
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[2]);
}

static void do_hash2(char **f)
{
    uint64_t salt = strtoull(f[1], NULL, 10), h = H0;
    int32_t ext[3] = {INT32_MIN, INT32_MAX, -1};
    char got[17];
    for (int a = 0; a < 83; a++) {
        int32_t y = a < 80 ? a - 40 : ext[a - 80];
        for (int b = 0; b < 83; b++) {
            int32_t x = b < 80 ? b - 40 : ext[b - 80];
            f32b(&h, pc_paint_hash2(x, y, salt));
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[2]);
}

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    if (strcmp(s, "-") == 0) {
        return 0;
    }
    while (s[0] != '\0' && s[1] != '\0' && n < cap) {
        char b[3] = {s[0], s[1], 0};
        out[n++] = (uint8_t)strtoul(b, NULL, 16);
        s += 2;
    }
    return n;
}

static void tohex(const uint8_t *b, size_t n, char *out, size_t cap)
{
    size_t k = 0;
    if (n == 0) {
        (void)snprintf(out, cap, "-");
        return;
    }
    for (size_t i = 0; i < n && k + 3 <= cap; i++) {
        k += (size_t)snprintf(out + k, cap - k, "%02x", b[i]);
    }
}

static void do_misc(char **f)
{
    char got[512];
    uint8_t b[256];
    if (strcmp(f[0], "mix") == 0) {
        (void)snprintf(got, sizeof got, "%" PRIu64, pc_paint_mix64(strtoull(f[1], NULL, 10)));
        EXPECT_STR(got, f[2]);
    } else if (strcmp(f[0], "seedb") == 0) {
        size_t n = unhex(f[1], b, sizeof b);
        (void)snprintf(got, sizeof got, "%" PRIu64, pc_paint_seed_from_bytes(b, n));
        EXPECT_STR(got, f[2]);
    } else if (strcmp(f[0], "q16") == 0) {
        (void)snprintf(got, sizeof got, "%u", (unsigned)pc_paint_q16(hf32(f[1])));
        EXPECT_STR(got, f[2]);
    } else if (strcmp(f[0], "b64e") == 0) {
        pc_buf out = PC_BUF_INIT;
        size_t n = unhex(f[1], b, sizeof b);
        CHECK(pc_paint_b64_encode(b, n, &out, NULL) == PC_OK);
        EXPECT_STR(pc_buf_cstr(&out), f[2]);
        pc_buf_fini(&out);
    } else if (strcmp(f[0], "b64d") == 0) {
        pc_buf out = PC_BUF_INIT;
        size_t n = unhex(f[1], b, sizeof b);
        if (pc_paint_b64_decode((const char *)b, n, &out, NULL) == PC_OK) {
            tohex(out.data, out.len, got, sizeof got);
        } else {
            (void)snprintf(got, sizeof got, "ERR");
        }
        EXPECT_STR(got, f[2]);
        pc_buf_fini(&out);
    }
}

static void do_mips(char **f)
{
    uint32_t w = (uint32_t)atoi(f[1]), h = (uint32_t)atoi(f[2]);
    pc_paint_gray_tile t;
    pc_paint_mips *m = NULL;
    uint64_t hh = H0, tf = 0;
    float *fl = NULL;
    size_t n = 0;
    static const float pxs[12] = {0.0f, 0.5f,   1.0f, 2.0f,     3.0f,  7.9f,
                                  8.0f, 100.0f, NAN,  INFINITY, -4.0f, 1e-30f};
    char got[17];
    gtile(w, h, strtoull(f[3], NULL, 10), &t);
    if (pc_paint_mips_new(&t, &m, NULL) != PC_OK) {
        CHECK(0);
        pc_paint_gray_tile_fini(&t);
        return;
    }
    u64le(&hh, m->count);
    for (size_t l = 0; l < m->count; l++) {
        u64le(&hh, m->levels[l].width);
        u64le(&hh, m->levels[l].height);
        for (size_t i = 0; i < m->levels[l].width * m->levels[l].height; i++) {
            f32b(&hh, m->levels[l].data[i]);
        }
    }
    for (int i = 0; i < 12; i++) {
        u64le(&hh, pc_paint_mips_level_for(m, pxs[i]));
    }
    for (size_t l = 0; l < m->count; l++) {
        for (int i = -3; i <= 23; i++) {
            for (int j = -3; j <= 23; j++) {
                f32b(&hh, pc_paint_mips_sample_clamped(m, l, (float)i / 20.0f, (float)j / 20.0f));
            }
        }
        f32b(&hh, pc_paint_mips_sample_clamped(m, l, NAN, 0.5f));
        f32b(&hh, pc_paint_mips_sample_clamped(m, l, 1e30f, -1e30f));
    }
    CHECK(pc_paint_gray_tile_to_f32(&t, &fl, &n, NULL) == PC_OK);
    for (size_t i = 0; i < n; i++) {
        uint32_t u;
        memcpy(&u, &fl[i], 4);
        tf += u;
    }
    free(fl);
    u64le(&hh, tf);
    f32b(&hh, pc_paint_gray_tile_get(&t, w - 1, h - 1));
    hx(hh, got);
    EXPECT_STR(got, f[4]);
    pc_paint_mips_release(m);
    pc_paint_gray_tile_fini(&t);
}

static int style_of(const char *s)
{
    static const char *const n[4] = {"noise", "canvas", "paper", "dots"};
    for (int i = 0; i < 4; i++) {
        if (strcmp(s, n[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static void do_pat(char **f)
{
    pc_paint_pattern_image *p = NULL;
    uint64_t h = H0;
    char got[17];
    if (pc_paint_pattern_generate((pc_paint_pattern_style)style_of(f[1]), (uint32_t)strtoul(f[2], NULL, 10),
                                  (uint32_t)strtoul(f[3], NULL, 10), &p, NULL) != PC_OK) {
        CHECK(0);
        return;
    }
    u64le(&h, p->width);
    for (size_t i = 0; i < p->width * p->height; i++) {
        f32b(&h, p->data[i]);
    }
    for (int i = -20; i < 20; i++) {
        f32b(&h, pc_paint_pattern_sample_wrap(p, (float)i * 3.7f, (float)i * -11.3f));
    }
    f32b(&h, pc_paint_pattern_sample_wrap(p, NAN, INFINITY));
    f32b(&h, pc_paint_pattern_sample_wrap(p, 1e30f, -1e30f));
    hx(h, got);
    EXPECT_STR(got, f[4]);
    pc_paint_pattern_image_release(p);
}

static void do_tip(char **f)
{
    uint32_t n = (uint32_t)atoi(f[2]), seed = (uint32_t)atoi(f[3]), extra = (uint32_t)atoi(f[4]);
    pc_paint_gray_tile t = PC_PAINT_GRAY_TILE_INIT;
    pc_status st;
    uint64_t h = H0;
    const uint16_t *s;
    char got[17];
    if (strcmp(f[1], "chalk") == 0) {
        st = pc_paint_chalk_tip(n, seed, &t, NULL);
    } else if (strcmp(f[1], "bristle") == 0) {
        st = pc_paint_bristle_tip(n, seed, &t, NULL);
    } else if (strcmp(f[1], "charcoal") == 0) {
        st = pc_paint_charcoal_tip(n, seed, &t, NULL);
    } else if (strcmp(f[1], "leaf") == 0) {
        st = pc_paint_leaf_tip(n, &t, NULL);
    } else if (strcmp(f[1], "sponge") == 0) {
        st = pc_paint_sponge_tip(n, seed, &t, NULL);
    } else if (strcmp(f[1], "star") == 0) {
        st = pc_paint_star_tip(n, &t, NULL);
    } else if (strcmp(f[1], "spatter") == 0) {
        st = pc_paint_spatter_tip(n, seed, extra, &t, NULL);
    } else if (strcmp(f[1], "grass") == 0) {
        st = pc_paint_grass_tip(n, seed, extra, &t, NULL);
    } else {
        st = pc_paint_rake_tip(n, seed, extra, &t, NULL);
    }
    CHECK(st == PC_OK);
    u64le(&h, t.width);
    u64le(&h, t.height);
    s = pc_paint_gray_tile_samples(&t);
    for (size_t i = 0; i < pc_paint_gray_tile_len(&t); i++) {
        uint8_t le[2] = {(uint8_t)(s[i] & 0xFF), (uint8_t)(s[i] >> 8)};
        fnv(&h, le, 2);
    }
    hx(h, got);
    EXPECT_STR(got, f[5]);
    pc_paint_gray_tile_fini(&t);
}

/* ---- coverage, masks, grid, rects ---- */

static const float SWEEP[19] = {0.0f,  -0.0f, 0.1f,  0.25f, 0.5f,  0.75f, 0.999f,   1.0f,      1.2f, 3.0f,
                                -0.3f, 9.9f,  10.0f, 10.4f, 10.5f, NAN,   INFINITY, -INFINITY, 1e-4f};

static void do_cov(char **f)
{
    static const float rs[9] = {0.5f, 1.0f, 10.0f, 0.0f, -1.0f, 2500.0f, NAN, INFINITY, 0.0005f};
    static const float hs[7] = {0.0f, 0.5f, 1.0f, 0.9999f, NAN, -1.0f, 2.0f};
    uint64_t h = H0;
    char got[17];
    for (int i = 0; i < 19; i++) {
        for (int j = 0; j < 9; j++) {
            for (int k = 0; k < 7; k++) {
                float scale = pc_paint_maxf_k(fabsf(rs[j]), 1.0f);
                f32b(&h, pc_paint_dab_coverage(SWEEP[i] * scale, rs[j], hs[k]));
            }
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[1]);
}

static int mask_of(const char *s)
{
    static const char *const n[10] = {"multiply",  "subtract",   "darken",  "overlay",      "colorDodge",
                                      "colorBurn", "linearBurn", "hardMix", "linearHeight", "height"};
    for (int i = 0; i < 10; i++) {
        if (strcmp(s, n[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static void do_mask(char **f)
{
    static const float ds[6] = {0.0f, 0.3f, 0.8f, 1.0f, NAN, 1.5f};
    pc_paint_mask_mode m = (pc_paint_mask_mode)mask_of(f[1]);
    uint64_t h = H0;
    char got[17];
    for (int i = 0; i < 19; i++) {
        for (int j = 0; j < 19; j++) {
            for (int k = 0; k < 6; k++) {
                f32b(&h, pc_paint_mask_combine(m, SWEEP[i], SWEEP[j], ds[k]));
            }
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[2]);
}

static void do_grid(char **f)
{
    double x = hf64(f[1]), y = hf64(f[2]), sq[4];
    float d = hf32(f[3]), cx, cy;
    char a[9], b[9], s[4][17], all[80];
    pc_paint_grid_center(x, y, d, &cx, &cy);
    pc_paint_grid_square(x, y, d, sq);
    x32(cx, a);
    x32(cy, b);
    EXPECT_STR(a, f[4]);
    EXPECT_STR(b, f[5]);
    for (int i = 0; i < 4; i++) {
        x64(sq[i], s[i]);
    }
    (void)snprintf(all, sizeof all, "%s,%s,%s,%s", s[0], s[1], s[2], s[3]);
    EXPECT_STR(all, f[6]);
}

static void do_drect(char **f)
{
    pc_paint_dab d = pc_paint_dab_round(pc_point_make(hf64(f[1]), hf64(f[2])), hf32(f[3]), 1.0f);
    char got[64];
    rect_s(pc_paint_rect_around(&d, d.radius), got, sizeof got);
    EXPECT_STR(got, f[4]);
}

/* ---- dynamics ---- */

static size_t ctl_steps(pc_paint_step *out)
{
    static const float vals[12] = {0.0f,  -0.0f,  0.3f,    1.0f, 1.7f,     -0.4f,
                                   89.0f, 400.0f, -720.5f, NAN,  INFINITY, -INFINITY};
    size_t n = 0;
    for (int i = 0; i < 12; i++) {
        for (int j = 0; j < 12; j++) {
            float a = vals[i], b = vals[j];
            pc_paint_step s;
            memset(&s, 0, sizeof s);
            s.point = pc_paint_point_make(0.0, 0.0, 1.0f);
            s.point.pressure = a;
            s.point.tilt_x = b;
            s.point.tilt_y = a * 2.0f;
            s.point.rotation = b * 3.0f;
            s.point.wheel = a - b;
            s.direction = a * 0.1f;
            s.initial_direction = b * 0.05f;
            s.step = (uint64_t)(i * 13 + j * 7);
            out[n++] = s;
        }
    }
    memset(&out[n], 0, sizeof out[n]);
    out[n].point = pc_paint_point_make(0.0, 0.0, 1.0f);
    out[n].step = UINT64_MAX;
    return n + 1;
}

static void do_ctl(char **f)
{
    static const uint32_t fs[4] = {0, 1, 25, 400};
    pc_paint_step st[145];
    size_t n = ctl_steps(st);
    uint64_t h = H0;
    char got[17];
    for (size_t k = 0; k < n; k++) {
        for (int c = 0; c < PC_PAINT_CONTROL_COUNT; c++) {
            for (int i = 0; i < 4; i++) {
                f32b(&h, pc_paint_control_value((pc_paint_control)c, fs[i], &st[k]));
                f32b(&h, pc_paint_control_angle((pc_paint_control)c, fs[i], &st[k]));
            }
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[1]);
}

static void do_color(char **f)
{
    pc_paint_brush b;
    pc_paint_step st[145];
    size_t n = ctl_steps(st);
    uint64_t h = H0;
    char got[17];
    if (!parse_brush(f[1], &b)) {
        return;
    }
    for (size_t k = 0; k < n; k += 5) {
        float c[4];
        pc_paint_dynamic_color(&b, &st[k], 99, k, c);
        for (int i = 0; i < 4; i++) {
            f32b(&h, c[i]);
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[2]);
    pc_paint_brush_fini(&b);
}

static void do_smooth(char **f)
{
    pc_paint_smoothing cfg;
    pc_paint_point_vec out = {0};
    char cb[64], *save = NULL, *t[5] = {0};
    int k = 0;
    size_t n = 0;
    pc_paint_point *pts = parse_points(f[3], &n);
    uint64_t h = H0;
    char got[64];
    (void)snprintf(cb, sizeof cb, "%s", f[1]);
    for (char *p = strtok_r(cb, ",", &save); p != NULL && k < 5; p = strtok_r(NULL, ",", &save)) {
        t[k++] = p;
    }
    if (k != 5) {
        CHECK(0);
        free(pts);
        return;
    }
    cfg.amount = hf32(t[0]);
    cfg.pulled_string = t[1][0] == '1';
    cfg.catch_up = t[2][0] == '1';
    cfg.catch_up_on_end = t[3][0] == '1';
    cfg.adjust_for_zoom = t[4][0] == '1';
    CHECK(pc_paint_smooth_points(&cfg, hf32(f[2]), pts, n, &out, NULL) == PC_OK);
    for (size_t i = 0; i < out.len; i++) {
        const pc_paint_point *p = &out.v[i];
        f64b(&h, p->x);
        f64b(&h, p->y);
        f64b(&h, p->time);
        f32b(&h, p->pressure);
        f32b(&h, p->tilt_x);
        f32b(&h, p->tilt_y);
        f32b(&h, p->rotation);
        f32b(&h, p->wheel);
    }
    (void)snprintf(got, sizeof got, "%zu:%016" PRIx64, out.len, h);
    EXPECT_STR(got, f[4]);
    pc_paint_point_vec_fini(&out);
    free(pts);
}

static void do_lsmooth(char **f)
{
    pc_paint_point_vec out = {0};
    size_t n = 0;
    pc_paint_point *pts = parse_points(f[2], &n);
    uint64_t h = H0;
    char got[17];
    CHECK(pc_paint_smooth(pts, n, hf32(f[1]), &out, NULL) == PC_OK);
    for (size_t i = 0; i < out.len; i++) {
        f64b(&h, out.v[i].x);
        f64b(&h, out.v[i].y);
    }
    hx(h, got);
    EXPECT_STR(got, f[3]);
    pc_paint_point_vec_fini(&out);
    free(pts);
}

static void do_dabs(char **f)
{
    pc_paint_brush b;
    pc_paint_dab_generator g;
    pc_paint_dab_vec d = {0}, u = {0};
    size_t n = 0;
    pc_paint_point *pts = parse_points(f[3], &n);
    uint64_t h = H0;
    char got[17], cnt[32];
    if (!parse_brush(f[1], &b)) {
        free(pts);
        return;
    }
    apply_ovr(&b, f[2]);
    pc_paint_dab_generator_init(&g, &b, 1.0f);
    CHECK(pc_paint_dab_generator_push(&g, pts, n, &d, &u, NULL) == PC_OK);
    CHECK(pc_paint_dab_generator_finish(&g, &d, &u, NULL) == PC_OK);
    for (size_t i = 0; i < d.len; i++) {
        dab_h(&h, &d.v[i]);
    }
    for (size_t i = 0; i < u.len; i++) {
        dab_h(&h, &u.v[i]);
    }
    (void)snprintf(cnt, sizeof cnt, "%zu", d.len);
    EXPECT_STR(cnt, f[4]);
    (void)snprintf(cnt, sizeof cnt, "%zu", u.len);
    EXPECT_STR(cnt, f[5]);
    hx(h, got);
    EXPECT_STR(got, f[6]);
    /* Chunked feeding (one point at a time) gives the same dabs. */
    {
        pc_paint_dab_generator g2;
        pc_paint_dab_vec d2 = {0}, u2 = {0};
        uint64_t h2 = H0;
        pc_paint_dab_generator_init(&g2, &b, 1.0f);
        for (size_t i = 0; i < n; i++) {
            CHECK(pc_paint_dab_generator_push(&g2, &pts[i], 1, &d2, &u2, NULL) == PC_OK);
        }
        CHECK(pc_paint_dab_generator_finish(&g2, &d2, &u2, NULL) == PC_OK);
        for (size_t i = 0; i < d2.len; i++) {
            dab_h(&h2, &d2.v[i]);
        }
        for (size_t i = 0; i < u2.len; i++) {
            dab_h(&h2, &u2.v[i]);
        }
        CHECK(h2 == h);
        pc_paint_dab_vec_fini(&d2);
        pc_paint_dab_vec_fini(&u2);
        pc_paint_dab_generator_fini(&g2);
    }
    pc_paint_dab_vec_fini(&d);
    pc_paint_dab_vec_fini(&u);
    pc_paint_dab_generator_fini(&g);
    pc_paint_brush_fini(&b);
    free(pts);
}

/* ---- rasterisation ---- */

static pc_paint_dab parse_dab(const char *s)
{
    char buf[256], *save = NULL, *t[11] = {0};
    int k = 0;
    pc_paint_dab d;
    (void)snprintf(buf, sizeof buf, "%s", s);
    for (char *p = strtok_r(buf, ",", &save); p != NULL && k < 11; p = strtok_r(NULL, ",", &save)) {
        t[k++] = p;
    }
    d = pc_paint_dab_round(pc_point_make(hf64(t[0]), hf64(t[1])), hf32(t[2]), hf32(t[3]));
    d.angle = hf32(t[4]);
    d.roundness = hf32(t[5]);
    d.flip_x = t[6][0] == '1';
    d.flip_y = t[7][0] == '1';
    d.depth = hf32(t[8]);
    d.proj_angle = hf32(t[9]);
    d.proj_scale = hf32(t[10]);
    return d;
}

static void do_rast(char **f)
{
    pc_paint_brush b;
    pc_paint_context ctx;
    pc_paint_dab d = parse_dab(f[3]);
    bool dual = f[4][0] == '1';
    pc_rect r;
    uint64_t h = H0;
    char got[64];
    if (!parse_brush(f[1], &b)) {
        return;
    }
    apply_ovr(&b, f[2]);
    CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
    r = pc_paint_context_dab_rect(&ctx, &d, dual);
    rect_s(r, got, sizeof got);
    EXPECT_STR(got, f[5]);
    if ((uint64_t)pc_rect_width(r) * pc_rect_height(r) <= (1u << 22)) {
        pc_paint_fbuf v = {0};
        CHECK(pc_paint_context_rasterize(&ctx, &d, dual, r, &v, NULL) == PC_OK);
        for (size_t i = 0; i < v.len; i++) {
            f32b(&h, v.v[i]);
        }
        if (!dual) {
            pc_paint_footprint fp;
            char rs[64];
            CHECK(pc_paint_context_footprint(&ctx, &d, 3, &fp, NULL) == PC_OK);
            rect_s(fp.rect, rs, sizeof rs);
            fnv(&h, rs, strlen(rs));
            u64le(&h, fp.index);
            for (size_t i = 0; i < fp.len; i++) {
                f32b(&h, fp.cov[i]);
            }
            pc_paint_footprint_fini(&fp);
        }
        pc_paint_fbuf_fini(&v);
    } else {
        fnv(&h, "big", 3);
    }
    hx(h, got);
    EXPECT_STR(got, f[6]);
    pc_paint_context_fini(&ctx);
    pc_paint_brush_fini(&b);
}

static void do_tex(char **f)
{
    pc_paint_brush b;
    pc_paint_context ctx;
    uint64_t h = H0;
    char got[17];
    if (!parse_brush(f[1], &b)) {
        return;
    }
    CHECK(pc_paint_context_init(&ctx, &b, NULL) == PC_OK);
    for (int a = 0; a < 142; a++) {
        int32_t y = a < 140 ? a - 70 : (a == 140 ? INT32_MIN : INT32_MAX);
        for (int c = 0; c < 49; c++) {
            int32_t x = c < 47 ? -70 + 3 * c : (c == 47 ? INT32_MIN : INT32_MAX);
            f32b(&h, pc_paint_context_texture_at(&ctx, x, y));
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[2]);
    pc_paint_context_fini(&ctx);
    pc_paint_brush_fini(&b);
}

/* ---- strokes ---- */

static void do_stroke(char **f)
{
    pc_paint_brush b;
    scen sc;
    size_t n = 0;
    pc_paint_point *pts = parse_points(f[4], &n);
    pc_rect dmg = PC_RECT_EMPTY;
    pc_error err;
    char got[64];
    if (!parse_brush(f[1], &b)) {
        free(pts);
        return;
    }
    apply_ovr(&b, f[2]);
    scenario(atoi(f[3]), &sc);
    if (pc_paint_render_stroke(&sc.s, &b, pts, n, sc.has_sel ? &sc.sel : NULL, sc.lock, 1.0f, &dmg, &err) !=
        PC_OK) {
        PC_T_FAIL("line %d: render_stroke: %s", g_line, err.msg);
    }
    rect_s(dmg, got, sizeof got);
    EXPECT_STR(got, f[5]);
    hx(surf_h(&sc.s), got);
    EXPECT_STR(got, f[6]);
    scen_fini(&sc);
    pc_paint_brush_fini(&b);
    free(pts);
}

/* Appends s (and a space unless last). */
static void appendf(char *out, size_t cap, const char *s, bool last)
{
    size_t k = strlen(out);
    (void)snprintf(out + k, cap - k, last ? "%s" : "%s ", s);
}

static void do_inc(char **f)
{
    pc_paint_brush b;
    scen sc;
    pc_surface pre, live, tail_live;
    pc_paint_renderer *r = NULL, *t = NULL;
    size_t n = 0, chunk = (size_t)atoi(f[4]);
    pc_paint_point *pts = parse_points(f[3], &n);
    pc_pixel_format fmt;
    const pc_surface *sel;
    char out[4096] = "", rs[64], hs[17];
    pc_rect d;
    if (!parse_brush(f[1], &b)) {
        free(pts);
        return;
    }
    scenario(atoi(f[2]), &sc);
    sel = sc.has_sel ? &sc.sel : NULL;
    fmt = pc_surface_format(&sc.s);
    CHECK(pc_surface_clone(&sc.s, &pre, NULL) == PC_OK);
    CHECK(pc_surface_clone(&sc.s, &live, NULL) == PC_OK);
    CHECK(pc_paint_renderer_new(&b, &fmt, 1.0f, &r, NULL) == PC_OK);
    pc_paint_renderer_record_dabs(r);
    for (size_t i = 0; i < n; i += chunk) {
        size_t m = n - i < chunk ? n - i : chunk;
        pc_rect dirty;
        CHECK(pc_paint_renderer_push(r, &pts[i], m, NULL) == PC_OK);
        dirty = pc_paint_renderer_take_dirty_rect(r);
        rect_s(dirty, rs, sizeof rs);
        appendf(out, sizeof out, rs, false);
        pc_paint_renderer_mark_dirty(r, dirty);
        CHECK(pc_paint_renderer_composite(r, &pre, &live, sel, sc.lock, false, &d, NULL) == PC_OK);
        rect_s(d, rs, sizeof rs);
        appendf(out, sizeof out, rs, false);
    }
    CHECK(pc_surface_clone(&live, &tail_live, NULL) == PC_OK);
    CHECK(pc_paint_renderer_tail_preview(r, &t, NULL) == PC_OK);
    if (t != NULL) {
        CHECK(pc_paint_renderer_composite(t, &pre, &tail_live, sel, sc.lock, false, &d, NULL) == PC_OK);
        char ts[70];
        rect_s(d, rs, sizeof rs);
        (void)snprintf(ts, sizeof ts, "T%s", rs);
        appendf(out, sizeof out, ts, false);
        pc_paint_renderer_free(t);
    } else {
        appendf(out, sizeof out, "T-", false);
    }
    hx(surf_h(&tail_live), hs);
    appendf(out, sizeof out, hs, false);
    CHECK(pc_paint_renderer_finish(r, NULL) == PC_OK);
    CHECK(pc_paint_renderer_composite(r, &pre, &live, sel, sc.lock, false, &d, NULL) == PC_OK);
    rect_s(d, rs, sizeof rs);
    appendf(out, sizeof out, rs, false);
    hx(surf_h(&live), hs);
    appendf(out, sizeof out, hs, false);
    {
        float *cv = NULL;
        size_t cn = 0;
        pc_rect cr = PC_RECT_EMPTY;
        uint64_t hd = H0, hc = H0, hr = H0;
        size_t nd = 0;
        const pc_paint_dab *dd;
        char tmp[128];
        CHECK(pc_paint_renderer_dense_coverage(r, &cr, &cv, &cn, NULL) == PC_OK);
        for (size_t i = 0; i < cn; i++) {
            f32b(&hd, cv[i]);
        }
        free(cv);
        rect_s(cr, rs, sizeof rs);
        (void)snprintf(tmp, sizeof tmp, "%s:%016" PRIx64, rs, hd);
        appendf(out, sizeof out, tmp, false);
        cv = NULL;
        CHECK(pc_paint_renderer_coverage_in(r, pc_rect_make(-10, -10, 90, 50), &cv, &cn, NULL) == PC_OK);
        for (size_t i = 0; i < cn; i++) {
            f32b(&hc, cv[i]);
        }
        free(cv);
        hx(hc, hs);
        appendf(out, sizeof out, hs, false);
        dd = pc_paint_renderer_dabs(r, &nd);
        for (size_t i = 0; i < nd; i++) {
            dab_h(&hr, &dd[i]);
        }
        (void)snprintf(tmp, sizeof tmp, "%zu:%zu:%016" PRIx64, pc_paint_renderer_dab_count(r), nd, hr);
        appendf(out, sizeof out, tmp, true);
    }
    EXPECT_STR(out, f[5]);
    pc_paint_renderer_free(r);
    pc_surface_fini(&pre);
    pc_surface_fini(&live);
    pc_surface_fini(&tail_live);
    scen_fini(&sc);
    pc_paint_brush_fini(&b);
    free(pts);
}

static void do_union(char **f)
{
    pc_paint_brush b;
    scen sc;
    pc_surface pre;
    pc_paint_renderer *a = NULL, *m = NULL;
    size_t n = 0;
    pc_paint_point *pts = parse_points(f[3], &n);
    pc_paint_point *mir = calloc(n > 0 ? n : 1, sizeof *mir);
    pc_pixel_format fmt;
    pc_rect d = PC_RECT_EMPTY;
    char got[64];
    if (!parse_brush(f[1], &b)) {
        free(pts);
        free(mir);
        return;
    }
    for (size_t i = 0; mir != NULL && i < n; i++) {
        mir[i] = pts[i];
        mir[i].x = 300.0 - pts[i].x;
    }
    scenario(atoi(f[2]), &sc);
    fmt = pc_surface_format(&sc.s);
    CHECK(pc_surface_clone(&sc.s, &pre, NULL) == PC_OK);
    CHECK(pc_paint_renderer_new(&b, &fmt, 1.0f, &a, NULL) == PC_OK);
    CHECK(pc_paint_renderer_new(&b, &fmt, 1.0f, &m, NULL) == PC_OK);
    CHECK(pc_paint_renderer_push(a, pts, n, NULL) == PC_OK);
    CHECK(pc_paint_renderer_finish(a, NULL) == PC_OK);
    CHECK(pc_paint_renderer_push(m, mir, n, NULL) == PC_OK);
    CHECK(pc_paint_renderer_finish(m, NULL) == PC_OK);
    CHECK(pc_paint_renderer_composite_union(a, m, &pre, &sc.s, sc.has_sel ? &sc.sel : NULL, sc.lock, &d,
                                            NULL) == PC_OK);
    rect_s(d, got, sizeof got);
    EXPECT_STR(got, f[4]);
    hx(surf_h(&sc.s), got);
    EXPECT_STR(got, f[5]);
    pc_paint_renderer_free(a);
    pc_paint_renderer_free(m);
    pc_surface_fini(&pre);
    scen_fini(&sc);
    pc_paint_brush_fini(&b);
    free(pts);
    free(mir);
}

/* ---- presets, picking, serde ---- */

static pc_paint_preset_list g_presets;

static void to_json(pc_paint_json_type t, const void *v, unsigned flags, pc_buf *out)
{
    pc_error err;
    out->len = 0;
    if (out->data != NULL) {
        out->data[0] = '\0';
    }
    if (pc_paint_json_to_string(t, v, flags, out, &err) != PC_OK) {
        PC_T_FAIL("line %d: to_string: %s", g_line, err.msg);
    }
}

static void do_preset(char **f)
{
    size_t i = (size_t)atoi(f[1]);
    pc_buf js = PC_BUF_INIT;
    pc_surface s;
    pc_paint_point pts[20];
    pc_rect d = PC_RECT_EMPTY;
    char got[64];
    if (i >= g_presets.len) {
        CHECK(0);
        return;
    }
    to_json(PC_PAINT_JSON_PRESET, &g_presets.v[i], 0, &js);
    EXPECT_STR(pc_buf_cstr(&js), f[2]);
    for (int k = 0; k < 20; k++) {
        pts[k] = pc_paint_point_make(40.0 + k * 8.0, 60.0, 0.8f);
        pts[k].time = k * 16.0;
    }
    pc_surface_init(&s, PC_PIXEL_FORMAT_RGBA8);
    CHECK(pc_paint_render_stroke(&s, &g_presets.v[i].brush, pts, 20, NULL, false, 1.0f, &d, NULL) == PC_OK);
    rect_s(d, got, sizeof got);
    EXPECT_STR(got, f[3]);
    hx(surf_h(&s), got);
    EXPECT_STR(got, f[4]);
    pc_surface_fini(&s);
    pc_buf_fini(&js);
}

static void do_pick(char **f)
{
    pc_paint_brush a, b;
    pc_buf js = PC_BUF_INIT;
    if (strcmp(f[1], "bounded") == 0) {
        pc_paint_brush r;
        uint64_t h = H0;
        char got[17];
        if (!parse_brush(f[2], &a)) {
            return;
        }
        apply_ovr(&a, f[3]);
        pc_paint_brush_bounded(&a, &r);
        f32b(&h, r.size);
        f32b(&h, r.dual_brush.size);
        f32b(&h, r.scattering.scatter.jitter);
        f32b(&h, r.dual_brush.scatter);
        hx(h, got);
        EXPECT_STR(got, f[4]);
        pc_paint_brush_fini(&r);
        pc_paint_brush_fini(&a);
        return;
    }
    if (!parse_brush(f[2], &a)) {
        return;
    }
    if (!parse_brush(f[3], &b)) {
        pc_paint_brush_fini(&a);
        return;
    }
    if (strcmp(f[1], "over") == 0) {
        pc_paint_brush_picked_over(&a, &b);
    } else {
        pc_paint_brush_with_protected_texture(&a, &b);
    }
    to_json(PC_PAINT_JSON_BRUSH, &a, 0, &js);
    EXPECT_STR(pc_buf_cstr(&js), f[4]);
    pc_buf_fini(&js);
    pc_paint_brush_fini(&a);
    pc_paint_brush_fini(&b);
}

static pc_paint_json_type type_of(const char *s)
{
    static const struct {
        const char *n;
        pc_paint_json_type t;
    } m[] = {{"BrushSettings", PC_PAINT_JSON_BRUSH}, {"BrushPreset", PC_PAINT_JSON_PRESET},
             {"Stroke", PC_PAINT_JSON_STROKE},       {"GrayTile", PC_PAINT_JSON_GRAY_TILE},
             {"StrokePoint", PC_PAINT_JSON_POINT},   {"Control", PC_PAINT_JSON_CONTROL},
             {"Dynamic", PC_PAINT_JSON_DYNAMIC},     {"Pattern", PC_PAINT_JSON_PATTERN},
             {"TipShape", PC_PAINT_JSON_TIP}};
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        if (strcmp(s, m[i].n) == 0) {
            return m[i].t;
        }
    }
    return PC_PAINT_JSON_TYPE_COUNT;
}

static void do_serde(char **f, size_t nf)
{
    pc_paint_json_type t = type_of(f[1]);
    void *v = calloc(1, pc_paint_json_size(t) + 1);
    pc_buf js = PC_BUF_INIT;
    pc_error err;
    if (v == NULL || t == PC_PAINT_JSON_TYPE_COUNT) {
        CHECK(0);
        free(v);
        return;
    }
    if (pc_paint_json_from_string(t, f[2], strlen(f[2]), v, &err) == PC_OK) {
        if (strcmp(f[0], "de") == 0) {
            to_json(t, v, 0, &js);
            EXPECT_STR(pc_buf_cstr(&js), f[3]);
        } else {
            to_json(t, v, 0, &js);
            EXPECT_STR(pc_buf_cstr(&js), f[2]);
            if (strcmp(f[0], "serv") == 0 && nf > 3) {
                to_json(t, v, PC_PAINT_JSON_VALUE, &js);
                EXPECT_STR(pc_buf_cstr(&js), f[3]);
            }
        }
        pc_paint_json_fini(t, v);
    } else {
        char msg[300];
        (void)snprintf(msg, sizeof msg, "ERR %s", err.msg);
        EXPECT_STR(msg, strcmp(f[0], "de") == 0 ? f[3] : "(ok)");
    }
    pc_buf_fini(&js);
    free(v);
}

/* ---- driver ---- */

static void test_parity_lines(void)
{
    FILE *fp = fopen(GOLDEN, "rb");
    char *buf, *line, *save = NULL;
    long size;
    int lines = 0;
    if (fp == NULL) {
        PC_T_FAIL("cannot open %s (run the tests from the repository root)", GOLDEN);
        return;
    }
    (void)fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    (void)fseek(fp, 0, SEEK_SET);
    buf = malloc((size_t)size + 1);
    if (buf == NULL || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        PC_T_FAIL("cannot read %s", GOLDEN);
        free(buf);
        (void)fclose(fp);
        return;
    }
    (void)fclose(fp);
    buf[size] = '\0';
    CHECK(pc_paint_presets_builtin(&g_presets, NULL) == PC_OK);
    for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        char *f[12];
        size_t nf = split(line, f, 12);
        lines++;
        g_line = lines;
        if (strcmp(f[0], "rng") == 0) {
            do_rng(f);
        } else if (strcmp(f[0], "hash2") == 0) {
            do_hash2(f);
        } else if (strcmp(f[0], "mix") == 0 || strcmp(f[0], "seedb") == 0 || strcmp(f[0], "q16") == 0 ||
                   strcmp(f[0], "b64e") == 0 || strcmp(f[0], "b64d") == 0) {
            do_misc(f);
        } else if (strcmp(f[0], "mips") == 0) {
            do_mips(f);
        } else if (strcmp(f[0], "pat") == 0) {
            do_pat(f);
        } else if (strcmp(f[0], "tip") == 0) {
            do_tip(f);
        } else if (strcmp(f[0], "cov") == 0) {
            do_cov(f);
        } else if (strcmp(f[0], "mask") == 0) {
            do_mask(f);
        } else if (strcmp(f[0], "grid") == 0) {
            do_grid(f);
        } else if (strcmp(f[0], "drect") == 0) {
            do_drect(f);
        } else if (strcmp(f[0], "ctl") == 0) {
            do_ctl(f);
        } else if (strcmp(f[0], "color") == 0) {
            do_color(f);
        } else if (strcmp(f[0], "smooth") == 0) {
            do_smooth(f);
        } else if (strcmp(f[0], "lsmooth") == 0) {
            do_lsmooth(f);
        } else if (strcmp(f[0], "dabs") == 0) {
            do_dabs(f);
        } else if (strcmp(f[0], "rast") == 0) {
            do_rast(f);
        } else if (strcmp(f[0], "tex") == 0) {
            do_tex(f);
        } else if (strcmp(f[0], "stroke") == 0) {
            do_stroke(f);
        } else if (strcmp(f[0], "inc") == 0) {
            do_inc(f);
        } else if (strcmp(f[0], "union") == 0) {
            do_union(f);
        } else if (strcmp(f[0], "preset") == 0) {
            do_preset(f);
        } else if (strcmp(f[0], "pick") == 0) {
            do_pick(f);
        } else if (strcmp(f[0], "ser") == 0 || strcmp(f[0], "serv") == 0 || strcmp(f[0], "de") == 0) {
            do_serde(f, nf);
        } else if (!r7_line(f)) {
            PC_T_FAIL("line %d: unknown kind %s", lines, f[0]);
        }
    }
    pc_paint_preset_list_fini(&g_presets);
    free(buf);
    CHECK(lines > 1900);
    (void)printf("paint parity: %d lines, %d mismatching checks\n", lines, g_fail_lines);
}
