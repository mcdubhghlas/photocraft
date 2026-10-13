/* The line kinds tools/rustref/src/paint_vectors_r7.rs adds (resync chunk R7): tip falloff,
 * texture levels and modes, tile previews, tip outlines, symmetry, point-by-point smoothing,
 * tail dabs and the live union of symmetry passes. Included by test_parity.c after its helpers.
 * r7_line returns false for a kind it doesn't know. */

static const char *const R7_MASKS[10] = {"multiply",  "subtract",   "darken",  "overlay",      "colorDodge",
                                         "colorBurn", "linearBurn", "hardMix", "linearHeight", "height"};

/* paint_vectors_r7.rs `r7_sweep`. */
static size_t r7_sweep(float *v)
{
    static const float extra[12] = {-0.5f, 1.5f,     0.999f,    0.001f, 0.4999f, 0.5001f,
                                    NAN,   INFINITY, -INFINITY, -0.0f,  1e-30f,  3.0e38f};
    size_t n = 0;
    for (int i = 0; i <= 40; i++) {
        v[n++] = (float)i / 40.0f;
    }
    for (int i = 0; i < 12; i++) {
        v[n++] = extra[i];
    }
    return n;
}

static void r7_falloff(char **f)
{
    static const float radii[8] = {10.0f, 0.5f, 1e-4f, 0.0f, -2.0f, 3.0e38f, NAN, INFINITY};
    static const float hradii[6] = {10.5f, 1.0f, 0.0f, -1.0f, NAN, INFINITY};
    float sw[64];
    size_t n = r7_sweep(sw);
    uint64_t h = H0;
    char got[17];
    for (size_t i = 0; i < n; i++) {
        for (int r = 0; r < 8; r++) {
            for (size_t k = 0; k < n; k++) {
                f32b(&h, pc_paint_tip_falloff(sw[i] * 12.0f, radii[r], sw[k]));
            }
        }
    }
    for (int r = 0; r < 6; r++) {
        for (size_t k = 0; k < n; k++) {
            f32b(&h, pc_paint_half_coverage_radius(hradii[r], sw[k]));
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[1]);
}

static void r7_texlevel(char **f)
{
    static const float cts[11] = {-1.0f, -0.6f, -0.3f, 0.0f, 0.3f, 0.5f, 0.999f, 1.0f, 1.5f, NAN, INFINITY};
    float sw[64];
    size_t n = r7_sweep(sw);
    uint64_t h = H0;
    char got[17];
    pc_paint_texture t;
    pc_paint_texture_default(&t);
    for (size_t i = 0; i < n; i++) {
        for (int c = 0; c < 11; c++) {
            for (int inv = 0; inv < 2; inv++) {
                t.brightness = sw[i] * 2.0f - 1.0f;
                t.contrast = cts[c];
                t.invert = inv == 1;
                for (size_t k = 0; k < n; k++) {
                    f32b(&h, pc_paint_texture_level(&t, sw[k]));
                }
            }
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[1]);
}

static void r7_texmode(char **f)
{
    static const float ds[11] = {0.0f, 0.002f, 0.01f, 0.05f, 0.13f, 0.25f, 0.5f, 1.0f, 1.2f, -0.1f, NAN};
    float sw[64];
    size_t n = r7_sweep(sw);
    uint64_t h = H0;
    char got[17];
    int m = -1;
    for (int i = 0; i < 10; i++) {
        if (strcmp(f[1], R7_MASKS[i]) == 0) {
            m = i;
        }
    }
    if (m < 0) {
        PC_T_FAIL("line %d: unknown mask mode %s", g_line, f[1]);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < n; k++) {
            for (int d = 0; d < 11; d++) {
                f32b(&h, pc_paint_texture_combine((pc_paint_mask_mode)m, sw[i], sw[k], ds[d]));
            }
        }
    }
    hx(h, got);
    EXPECT_STR(got, f[2]);
}

static void r7_preview(char **f)
{
    pc_paint_gray_tile t = PC_PAINT_GRAY_TILE_INIT, p = PC_PAINT_GRAY_TILE_INIT;
    char got[128];
    uint32_t max = (uint32_t)strtoul(f[4], NULL, 10);
    bool bad = strcmp(f[1], "bad") == 0;
    if (bad) {
        uint16_t one = 1;
        if (atoi(f[3]) == 0) {
            CHECK(pc_paint_gray_tile_from_u16(4, 4, &one, 1, &t, NULL) == PC_OK);
        }
    } else {
        gtile((uint32_t)strtoul(f[1], NULL, 10), (uint32_t)strtoul(f[2], NULL, 10), strtoull(f[3], NULL, 10),
              &t);
    }
    CHECK(pc_paint_gray_tile_preview(&t, max, &p, NULL) == PC_OK);
    if (bad) {
        const uint16_t *s = pc_paint_gray_tile_samples(&p);
        (void)snprintf(got, sizeof got, "%" PRIu32 ",%" PRIu32 ":[%u]", p.width, p.height,
                       s != NULL ? (unsigned)s[0] : 99u);
    } else {
        uint64_t h = H0;
        const uint16_t *s = pc_paint_gray_tile_samples(&p);
        for (size_t i = 0; i < pc_paint_gray_tile_len(&p); i++) {
            uint8_t le[2] = {(uint8_t)(s[i] & 0xFF), (uint8_t)(s[i] >> 8)};
            fnv(&h, le, 2);
        }
        (void)snprintf(got, sizeof got, "%" PRIu32 ",%" PRIu32 ":%016" PRIx64, p.width, p.height, h);
    }
    EXPECT_STR(got, f[5]);
    pc_paint_gray_tile_fini(&t);
    pc_paint_gray_tile_fini(&p);
}

static void r7_outline(char **f)
{
    pc_paint_brush b;
    pc_paint_outline o;
    char got[4096];
    if (!parse_brush(f[1], &b)) {
        return;
    }
    apply_ovr(&b, f[2]);
    CHECK(pc_paint_tip_outline(&b, f[3][0] == '1', &o, NULL) == PC_OK);
    if (o.count == 0) {
        (void)snprintf(got, sizeof got, "none");
    } else {
        uint64_t h = H0;
        size_t at;
        for (size_t i = 0; i < o.count; i++) {
            u64le(&h, o.rings[i].n);
            for (size_t j = 0; j < o.rings[i].n; j++) {
                f32b(&h, o.rings[i].pts[j][0]);
                f32b(&h, o.rings[i].pts[j][1]);
            }
        }
        at = (size_t)snprintf(got, sizeof got, "%zu:", o.count);
        for (size_t i = 0; i < o.count && at < sizeof got; i++) {
            at += (size_t)snprintf(got + at, sizeof got - at, i == 0 ? "%zu" : ",%zu", o.rings[i].n);
        }
        if (at < sizeof got) {
            (void)snprintf(got + at, sizeof got - at, ":%016" PRIx64, h);
        }
    }
    EXPECT_STR(got, f[4]);
    pc_paint_outline_fini(&o);
    pc_paint_brush_fini(&b);
}

static void r7_pts_h(uint64_t *h, const pc_paint_point *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        f64b(h, p[i].x);
        f64b(h, p[i].y);
        f64b(h, p[i].time);
        f32b(h, p[i].pressure);
        f32b(h, p[i].tilt_x);
        f32b(h, p[i].tilt_y);
        f32b(h, p[i].rotation);
        f32b(h, p[i].wheel);
    }
}

static void r7_sym(char **f)
{
    static const double sizes[4][2] = {{100.0, 100.0}, {101.0, 53.0}, {640.0, 480.0}, {0.0, 0.0}};
    pc_paint_symmetry_mode mode;
    pc_paint_symmetry s;
    const double c[2] = {hf64(f[2]), hf64(f[3])};
    char got[256];
    if (!pc_paint_symmetry_mode_from_id(f[1], strlen(f[1]), &mode)) {
        PC_T_FAIL("line %d: unknown symmetry mode %s", g_line, f[1]);
        return;
    }
    if (!pc_paint_symmetry_new(mode, c, hf64(f[4]), &s)) {
        (void)snprintf(got, sizeof got, "none");
    } else {
        pc_paint_point pts[3] = {pc_paint_point_make(12.0, 25.0, 0.4f), pc_paint_point_make(-3.5, 1e5, 1.0f),
                                 pc_paint_point_make(640.25, 0.0, 0.0f)};
        pc_paint_point_vec out[3] = {{0}, {0}, {0}};
        double dirs[2][2], seg[2][2][2];
        size_t nd = pc_paint_symmetry_directions(&s, dirs), count = 0, ns[4];
        uint64_t h = H0;
        pts[0].tilt_x = 18.0f;
        pts[2].time = 7.0;
        f64b(&h, s.rotation);
        for (size_t i = 0; i < nd; i++) {
            f64b(&h, dirs[i][0]);
            f64b(&h, dirs[i][1]);
        }
        for (int k = 0; k < 4; k++) {
            ns[k] = pc_paint_symmetry_segments(&s, sizes[k], seg);
            for (size_t i = 0; i < ns[k]; i++) {
                f64b(&h, seg[i][0][0]);
                f64b(&h, seg[i][0][1]);
                f64b(&h, seg[i][1][0]);
                f64b(&h, seg[i][1][1]);
            }
        }
        CHECK(pc_paint_symmetry_reflected_passes(&s, pts, 3, out, &count, NULL) == PC_OK);
        for (size_t i = 0; i < count; i++) {
            r7_pts_h(&h, out[i].v, out[i].len);
            pc_paint_point_vec_fini(&out[i]);
        }
        (void)snprintf(got, sizeof got, "%zu:%zu:%zu,%zu,%zu,%zu:%016" PRIx64,
                       pc_paint_symmetry_mirror_count(&s), count, ns[0], ns[1], ns[2], ns[3], h);
    }
    EXPECT_STR(got, f[5]);
}

static bool r7_cfg(const char *s, pc_paint_smoothing *cfg)
{
    char cb[64], *save = NULL, *t[5] = {0};
    int k = 0;
    (void)snprintf(cb, sizeof cb, "%s", s);
    for (char *p = strtok_r(cb, ",", &save); p != NULL && k < 5; p = strtok_r(NULL, ",", &save)) {
        t[k++] = p;
    }
    if (k != 5) {
        return false;
    }
    cfg->amount = hf32(t[0]);
    cfg->pulled_string = t[1][0] == '1';
    cfg->catch_up = t[2][0] == '1';
    cfg->catch_up_on_end = t[3][0] == '1';
    cfg->adjust_for_zoom = t[4][0] == '1';
    return true;
}

static void r7_smoothx(char **f)
{
    pc_paint_smoothing cfg;
    pc_paint_smoother s;
    pc_paint_point_vec out = {0};
    size_t n = 0, total = 0, nf = 0;
    pc_paint_point *pts = parse_points(f[3], &n);
    uint64_t h = H0;
    char flags[256], got[320];
    if (!r7_cfg(f[1], &cfg)) {
        CHECK(0);
        free(pts);
        return;
    }
    pc_paint_smoother_init(&s, &cfg, hf32(f[2]));
    for (size_t i = 0; i < n; i++) {
        double br[2], pt[2];
        out.len = 0;
        CHECK(pc_paint_smoother_push(&s, pts[i], &out, NULL) == PC_OK);
        total += out.len;
        r7_pts_h(&h, out.v, out.len);
        if (nf + 3 < sizeof flags) {
            flags[nf++] = pc_paint_smoother_lagging(&s) ? 'L' : '-';
        }
        if (pc_paint_smoother_string(&s, br, pt)) {
            if (nf + 2 < sizeof flags) {
                flags[nf++] = 'S';
            }
            f64b(&h, br[0]);
            f64b(&h, br[1]);
            f64b(&h, pt[0]);
            f64b(&h, pt[1]);
        } else if (nf + 2 < sizeof flags) {
            flags[nf++] = '-';
        }
    }
    out.len = 0;
    CHECK(pc_paint_smoother_finish(&s, &out, NULL) == PC_OK);
    total += out.len;
    r7_pts_h(&h, out.v, out.len);
    flags[nf] = '\0';
    (void)snprintf(got, sizeof got, "%zu:%s:%016" PRIx64, total, nf > 0 ? flags : "-", h);
    EXPECT_STR(got, f[4]);
    pc_paint_point_vec_fini(&out);
    free(pts);
}

static void r7_tdabs(char **f)
{
    pc_paint_brush b;
    pc_paint_renderer *r = NULL;
    pc_pixel_format fmt = PC_PIXEL_FORMAT_RGBA8;
    size_t n = 0, at = 0;
    pc_paint_point *pts = parse_points(f[2], &n);
    char *got = calloc(1, 8192);
    if (got == NULL || !parse_brush(f[1], &b)) {
        free(got);
        free(pts);
        return;
    }
    CHECK(pc_paint_renderer_new(&b, &fmt, 1.0f, &r, NULL) == PC_OK);
    for (size_t k = 0; r != NULL && k < n && at < 8000; k++) {
        pc_paint_dab_vec t = {0};
        uint64_t h = H0;
        double a[2], c[2];
        char s[80], x[4][17];
        CHECK(pc_paint_renderer_push(r, &pts[k], 1, NULL) == PC_OK);
        CHECK(pc_paint_renderer_tail_dabs(r, &t, NULL) == PC_OK);
        for (size_t i = 0; i < t.len; i++) {
            dab_h(&h, &t.v[i]);
        }
        if (pc_paint_renderer_smoothing_string(r, a, c)) {
            x64(a[0], x[0]);
            x64(a[1], x[1]);
            x64(c[0], x[2]);
            x64(c[1], x[3]);
            (void)snprintf(s, sizeof s, "%s,%s,%s,%s", x[0], x[1], x[2], x[3]);
        } else {
            (void)snprintf(s, sizeof s, "-");
        }
        at += (size_t)snprintf(got + at, 8192 - at, "%s%zu:%d:%016" PRIx64 ":%s", k == 0 ? "" : " ", t.len,
                               pc_paint_renderer_wants_time(r) ? 1 : 0, h, s);
        pc_paint_dab_vec_fini(&t);
    }
    EXPECT_STR(got, f[3]);
    pc_paint_renderer_free(r);
    pc_paint_brush_fini(&b);
    free(got);
    free(pts);
}

static void r7_append(char *out, size_t cap, size_t *at, const char *s)
{
    if (*at < cap) {
        *at += (size_t)snprintf(out + *at, cap - *at, "%s%s", *at == 0 ? "" : " ", s);
    }
}

static void r7_ulive(char **f)
{
    pc_paint_brush b;
    pc_paint_renderer *rs[4] = {NULL, NULL, NULL, NULL}, *fin[4] = {NULL, NULL, NULL, NULL};
    pc_paint_union_live live = {0};
    pc_paint_point *paths[4] = {NULL, NULL, NULL, NULL};
    size_t n = 0, npass = (size_t)atoi(f[3]), chunk = (size_t)atoi(f[4]), at = 0, merged = 0, len = 0, nfin;
    char *got = calloc(1, 16384);
    scen sc;
    pc_surface pre, t2;
    pc_pixel_format fmt;
    pc_rect d;
    paths[0] = parse_points(f[5], &n);
    if (got == NULL || paths[0] == NULL || npass < 1 || npass > 4 || chunk == 0) {
        CHECK(0);
        free(got);
        free(paths[0]);
        return;
    }
    if (!parse_brush(f[1], &b)) {
        free(got);
        free(paths[0]);
        return;
    }
    for (int k = 1; k < 4; k++) {
        paths[k] = malloc(n * sizeof *paths[k]);
        CHECK(paths[k] != NULL);
        for (size_t i = 0; paths[k] != NULL && i < n; i++) {
            paths[k][i] = paths[0][i];
            if (k & 1) {
                paths[k][i].x = 200.0 - paths[0][i].x;
            }
            if (k & 2) {
                paths[k][i].y = 150.0 - paths[0][i].y;
            }
        }
    }
    scenario(atoi(f[2]), &sc);
    fmt = pc_surface_format(&sc.s);
    CHECK(pc_surface_clone(&sc.s, &pre, NULL) == PC_OK);
    for (int k = 0; k < 4; k++) {
        CHECK(pc_paint_renderer_new(&b, &fmt, 1.0f, &rs[k], NULL) == PC_OK);
    }
    while (at < n && paths[1] != NULL && paths[2] != NULL && paths[3] != NULL) {
        size_t end = at + chunk < n ? at + chunk : n, count = at * 2 >= n ? npass : (npass < 2 ? npass : 2);
        bool full = merged != count;
        char r1[64], r2[64], hh[17], item[160];
        for (int k = 0; k < 4; k++) {
            CHECK(pc_paint_renderer_push(rs[k], &paths[k][at], end - at, NULL) == PC_OK);
        }
        merged = count;
        CHECK(pc_paint_renderer_composite_union_live(rs, count, &live, full, &pre, &sc.s,
                                                     sc.has_sel ? &sc.sel : NULL, sc.lock, &d,
                                                     NULL) == PC_OK);
        rect_s(d, r1, sizeof r1);
        rect_s(pc_paint_union_live_bounds(&live), r2, sizeof r2);
        hx(surf_h(&sc.s), hh);
        (void)snprintf(item, sizeof item, "%s|%s|%s", r1, r2, hh);
        r7_append(got, 16384, &len, item);
        at = end;
    }
    nfin = merged > 0 ? merged : 1;
    for (size_t k = 0; k < nfin; k++) {
        CHECK(pc_paint_renderer_clone(rs[k], &fin[k], NULL) == PC_OK);
        CHECK(fin[k] != NULL && pc_paint_renderer_finish(fin[k], NULL) == PC_OK);
    }
    CHECK(pc_surface_clone(&pre, &t2, NULL) == PC_OK);
    if (fin[0] != NULL) {
        char r1[64], hh[17], item[100];
        CHECK(pc_paint_renderer_composite_union_many(fin[0], (const pc_paint_renderer *const *)&fin[1],
                                                     nfin - 1, &pre, &t2, sc.has_sel ? &sc.sel : NULL,
                                                     sc.lock, &d, NULL) == PC_OK);
        rect_s(d, r1, sizeof r1);
        hx(surf_h(&t2), hh);
        (void)snprintf(item, sizeof item, "M%s|%s", r1, hh);
        r7_append(got, 16384, &len, item);
    }
    EXPECT_STR(got, f[6]);
    for (int k = 0; k < 4; k++) {
        pc_paint_renderer_free(rs[k]);
        pc_paint_renderer_free(fin[k]);
        free(paths[k]);
    }
    pc_paint_union_live_fini(&live);
    pc_surface_fini(&t2);
    pc_surface_fini(&pre);
    scen_fini(&sc);
    pc_paint_brush_fini(&b);
    free(got);
}

static bool r7_line(char **f)
{
    if (strcmp(f[0], "falloff") == 0) {
        r7_falloff(f);
    } else if (strcmp(f[0], "texlevel") == 0) {
        r7_texlevel(f);
    } else if (strcmp(f[0], "texmode") == 0) {
        r7_texmode(f);
    } else if (strcmp(f[0], "preview") == 0) {
        r7_preview(f);
    } else if (strcmp(f[0], "outline") == 0) {
        r7_outline(f);
    } else if (strcmp(f[0], "sym") == 0) {
        r7_sym(f);
    } else if (strcmp(f[0], "smoothx") == 0) {
        r7_smoothx(f);
    } else if (strcmp(f[0], "tdabs") == 0) {
        r7_tdabs(f);
    } else if (strcmp(f[0], "ulive") == 0) {
        r7_ulive(f);
    } else {
        return false;
    }
    return true;
}
