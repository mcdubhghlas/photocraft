/* Phase 5R (R4) handlers for tests/doc/test_parity.c: the fields and functions upstream added
 * after efa62efa (pencil curve maps, shape stroke limits, pattern paint limits, colours into a
 * document mode, effect on/off, contour corners). Included by test_parity.c after its helpers;
 * not a standalone header. */

/* "x32,x32,..." into a malloc'd array (NULL when empty). */
static float *f32_list(const char *s, size_t *n)
{
    size_t cap = 1;
    float *v;
    for (const char *p = s; *p != '\0'; p++) {
        cap += *p == ',';
    }
    v = calloc(cap, sizeof *v);
    if (v == NULL) {
        *n = 0;
        return NULL;
    }
    *n = *s == '\0' ? 0 : P32S(s, v, cap);
    return v;
}

static void fn_curve_map(void)
{
    size_t n;
    float *v = f32_list(F[2], &n);
    pc_curve c = {NULL, 0};
    pc_error e;
    uint64_t h = H0;
    if (pc_curve_map(v, n, &c, &e) == PC_OK) {
        TU(c.len);
        for (size_t i = 0; i < c.len; i++) {
            fnv_f32(&h, c.pts[i].input);
            fnv_f32(&h, c.pts[i].output);
        }
        THASH(h);
    } else {
        TERR(&e);
    }
    pc_curve_fini(&c);
    free(v);
}

static void fn_curve_map_outputs(void)
{
    size_t n;
    float *v = f32_list(F[2], &n);
    pc_curve_point *pts = calloc(n / 2 + 1, sizeof *pts);
    float o[PC_CURVE_MAP_LEN];
    pc_error e;
    uint64_t h = H0;
    if (pts != NULL) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            pts[i / 2].input = v[i];
            pts[i / 2].output = v[i + 1];
        }
        if (pc_curve_map_outputs(pts, n / 2, o, &e) == PC_OK) {
            TU(PC_CURVE_MAP_LEN);
            for (size_t i = 0; i < PC_CURVE_MAP_LEN; i++) {
                fnv_f32(&h, o[i]);
            }
            THASH(h);
            TX32S(o, 8);
        } else {
            TERR(&e);
        }
    }
    free(pts);
    free(v);
}

static void fn_stroke_validate(void)
{
    pc_shape_stroke s;
    size_t n;
    pc_error e;
    float *d = f32_list(F[6], &n);
    pc_shape_stroke_default(&s);
    s.width = P32(F[2]);
    s.opacity = P32(F[3]);
    s.miter_limit = P32(F[4]);
    s.dash_offset = P32(F[5]);
    free(s.dashes);
    s.dashes = d;
    s.ndashes = n;
    if (pc_shape_stroke_validate(&s, &e) == PC_OK) {
        TS("ok");
    } else {
        TS(e.msg);
    }
    pc_shape_stroke_fini(&s);
}

static void fn_pattern_paintable(void)
{
    pc_pattern p;
    memset(&p, 0, sizeof p);
    p.width = (uint32_t)U(F[2]);
    p.height = (uint32_t)U(F[3]);
    TU(pc_pattern_pixel_count(&p));
    TB(pc_pattern_is_paintable(&p));
}

static void fn_pattern_limits(void)
{
    TU(PC_PATTERN_MAX_EDGE);
    TU(PC_PATTERN_MAX_PAINT_PIXELS);
}

static void fn_effect_in_mode(void)
{
    pc_effect fx;
    pc_color_mode m;
    if (load(&TI_pc_effect, F[2], &fx) && load(&TI_pc_color_mode, F[3], &m)) {
        pc_effect_in_mode(&fx, m);
        TJ(&TI_pc_effect, &fx);
    }
    pc_effect_fini(&fx);
}

static void fn_effect_set_enabled(void)
{
    pc_effect fx;
    if (load(&TI_pc_effect, F[2], &fx)) {
        pc_effect_set_enabled(&fx, B(F[3]));
        TJ(&TI_pc_effect, &fx);
    }
    pc_effect_fini(&fx);
}

static void fn_fill_in_mode(void)
{
    pc_fill f;
    pc_color_mode m;
    if (load(&TI_pc_fill, F[2], &f) && load(&TI_pc_color_mode, F[3], &m)) {
        pc_fill_in_mode(&f, m);
        TJ(&TI_pc_fill, &f);
    }
    pc_fill_fini(&f);
}

static void fn_contour_is_corner(void)
{
    pc_contour c;
    if (load(&TI_pc_contour, F[2], &c)) {
        TB(pc_contour_is_corner(&c, (size_t)U(F[3])));
    }
    pc_contour_fini(&c);
}
