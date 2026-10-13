/* test_fx_parity.c: replay of the golden sections beyond the filter cases (Filter Gallery API,
 * render2, trap, warp). Must mirror tools/rustref/src/algo_fx_vectors.rs. Header-only; included
 * once. */
#ifndef PC_TESTS_ALGO_FXPARITY2_H
#define PC_TESTS_ALGO_FXPARITY2_H

#include <ctype.h>
#include <math.h>
#include <stdarg.h>

#include "algo/artistic.h"
#include "algo/render2.h"
#include "algo/trap.h"
#include "algo/warp.h"
#include "base/json.h"
#include "fxparity.h"

/* Appends formatted text to got (capacity cap). */
static void cat(char *got, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void cat(char *got, size_t cap, const char *fmt, ...)
{
    size_t n = strlen(got);
    va_list ap;
    if (n >= cap) {
        return;
    }
    va_start(ap, fmt);
    (void)vsnprintf(got + n, cap - n, fmt, ap);
    va_end(ap);
}

/* Rust's `{}` of the small integral f32 values these lines print. */
static void fdisp(float v, char *out, size_t cap)
{
    if (v == truncf(v) && fabsf(v) < 1e9f) {
        (void)snprintf(out, cap, "%.0f", (double)v);
    } else {
        (void)snprintf(out, cap, "%g", (double)v);
    }
}

static uint32_t fbits(float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    return u;
}

static void opt_key(char *got, size_t cap, const char *k)
{
    pc_algo_gallery_filter g;
    if (pc_algo_gallery_filter_from_key(k, strlen(k), &g)) {
        cat(got, cap, "Some(\"%s\")", pc_algo_gallery_filter_key(g));
    } else {
        cat(got, cap, "None");
    }
}

/* Serialises a builder's value to compact JSON in got. */
static void cat_json(char *got, size_t cap, yyjson_mut_doc *d, yyjson_mut_val *v)
{
    pc_buf b;
    pc_buf_init(&b);
    if (v != NULL && pc_json_write(v, 0, &b, NULL) == PC_OK) {
        cat(got, cap, "%s", pc_buf_cstr(&b));
    } else {
        cat(got, cap, "?");
    }
    pc_buf_fini(&b);
    (void)d;
}

static void gal_line(const char *key, char *got, size_t cap)
{
    pc_algo_gallery_filter f;
    pc_algo_gallery_param ps[PC_ALGO_GALLERY_MAX_PARAMS];
    size_t n, i, c;
    char up[64];
    if (!pc_algo_gallery_filter_from_key(key, strlen(key), &f)) {
        return;
    }
    cat(got, cap, "gal %s %s %s [%s] ", pc_algo_gallery_filter_key(f), pc_algo_gallery_filter_command_id(f),
        pc_algo_gallery_filter_category(f), pc_algo_gallery_filter_name(f));
    n = pc_algo_gallery_params(f, ps, PC_ALGO_GALLERY_MAX_PARAMS);
    for (i = 0; i < n; i++) {
        char a[32], b[32], d[32];
        cat(got, cap, "%s%s:", i > 0 ? "," : "", ps[i].key);
        if (ps[i].kind == PC_ALGO_GALLERY_RANGE) {
            fdisp(ps[i].min, a, sizeof a), fdisp(ps[i].max, b, sizeof b), fdisp(ps[i].def, d, sizeof d);
            cat(got, cap, "%s..%s=%s", a, b, d);
        } else if (ps[i].kind == PC_ALGO_GALLERY_CHOICE) {
            for (c = 0; c < ps[i].nchoices; c++) {
                cat(got, cap, "%s%s", c > 0 ? "|" : "", ps[i].choices[c]);
            }
        } else {
            cat(got, cap, "bool=%s", ps[i].flag ? "true" : "false");
        }
    }
    cat(got, cap, " colours=%s from=", pc_algo_gallery_filter_uses_colours(f) ? "true" : "false");
    opt_key(got, cap, pc_algo_gallery_filter_key(f));
    cat(got, cap, "/");
    (void)snprintf(up, sizeof up, "%s", pc_algo_gallery_filter_name(f));
    for (i = 0; up[i] != '\0'; i++) {
        if ((unsigned char)up[i] < 128) {
            up[i] = (char)toupper((unsigned char)up[i]);
        }
    }
    opt_key(got, cap, up);
    cat(got, cap, "/");
    opt_key(got, cap, pc_algo_gallery_filter_command_id(f));
}

static void galset_line(char *got, size_t cap)
{
    static const struct {
        const char *k;
        float v;
    } sets[] = {{"relief", 500.0f}, {"relief", -3.0f}, {"relief", NAN},  {"texture", 2.6f}, {"texture", 9.0f},
                {"texture", -1.0f}, {"invert", 0.5f},  {"invert", 0.0f}, {"light", 7.4f},   {"nope", 1.0f}};
    static const char *const choices[5][2] = {
        {"texture", "BRICK"}, {"texture", "wood"}, {"light", "bottomLeft"}, {"relief", "x"}, {"nope", "y"}};
    pc_algo_gallery_effect e, g;
    char cb[PC_ALGO_GALLERY_NAME_MAX], a[32], b[32], c[32];
    size_t i;
    CHECK(pc_algo_gallery_effect_new(PC_ALGO_GAL_TEXTURIZER, &e, NULL) == PC_OK);
    cat(got, cap, "galset");
    for (i = 0; i < sizeof sets / sizeof sets[0]; i++) {
        CHECK(pc_algo_gallery_effect_set(&e, sets[i].k, sets[i].v, NULL) == PC_OK);
        cat(got, cap, " %s=%u/%s/%s/%zu", sets[i].k,
            (unsigned)fbits(pc_algo_gallery_effect_get(&e, sets[i].k)),
            pc_algo_gallery_effect_flag(&e, sets[i].k) ? "true" : "false",
            pc_algo_gallery_effect_choice(&e, sets[i].k, cb), e.nvalues);
    }
    for (i = 0; i < 5; i++) {
        bool ok = false;
        CHECK(pc_algo_gallery_effect_set_choice(&e, choices[i][0], choices[i][1], &ok, NULL) == PC_OK);
        cat(got, cap, " %s:%s=%s/%s", choices[i][0], choices[i][1], ok ? "true" : "false",
            pc_algo_gallery_effect_choice(&e, choices[i][0], cb));
    }
    pc_algo_gallery_effect_fini(&e);
    CHECK(pc_algo_gallery_effect_new(PC_ALGO_GAL_GRAIN, &g, NULL) == PC_OK);
    for (i = 0; i < g.nvalues; i++) {
        free(g.values[i].key);
    }
    g.nvalues = 0;
    fdisp(pc_algo_gallery_effect_get(&g, "intensity"), a, sizeof a);
    fdisp(pc_algo_gallery_effect_get(&g, "grainType"), b, sizeof b);
    fdisp(pc_algo_gallery_effect_get(&g, "nope"), c, sizeof c);
    cat(got, cap, " unset %s %s %s %s", a, b, pc_algo_gallery_effect_choice(&g, "grainType", cb), c);
    CHECK(pc_algo_gallery_effect_put(&g, "grainType", 9, 99.0f, NULL) == PC_OK);
    cat(got, cap, " oob %s", pc_algo_gallery_effect_choice(&g, "grainType", cb));
    CHECK(pc_algo_gallery_effect_put(&g, "grainType", 9, -5.0f, NULL) == PC_OK);
    cat(got, cap, " neg %s", pc_algo_gallery_effect_choice(&g, "grainType", cb));
    pc_algo_gallery_effect_fini(&g);
}

/* ---- render2 ---- */

static uint64_t hprims(const pc_algo_prims *p)
{
    uint64_t h = H0;
    size_t i;
    for (i = 0; i < p->n; i++) {
        const pc_algo_prim *q = &p->v[i];
        uint8_t tag = q->shape == PC_ALGO_PRIM_CAPSULE ? 0 : 1, bl = (uint8_t)q->blend;
        fnv(&h, &tag, 1);
        if (tag == 0) {
            const float v[6] = {q->a[0], q->a[1], q->b[0], q->b[1], q->ra, q->rb};
            hf(&h, v, 6);
        } else {
            const float v[5] = {q->c[0], q->c[1], q->rx, q->ry, q->angle};
            hf(&h, v, 5);
        }
        hf(&h, q->c0, 4);
        hf(&h, q->c1, 4);
        hf(&h, &q->soft, 1);
        fnv(&h, &bl, 1);
        hrect(&h, pc_algo_prim_bounds(q));
    }
    return h;
}

/* Flame paths both sides build by name (rustref: flame_paths); returns the count. */
static size_t flame_paths(const char *name, pc_rect canvas, float (*buf)[2], pc_algo_polyline *paths)
{
    if (strcmp(name, "arc") == 0) {
        pc_algo_default_flame_path(canvas, (float (*)[2])buf);
        paths[0].v = buf, paths[0].n = 33;
        return 1;
    }
    if (strcmp(name, "loop") == 0) {
        const float p[5][2] = {
            {50.0f, 50.0f}, {250.0f, 60.0f}, {240.0f, 220.0f}, {60.0f, 210.0f}, {50.2f, 50.3f}};
        memcpy(buf, p, sizeof p);
        paths[0].v = buf, paths[0].n = 5;
        return 1;
    }
    if (strcmp(name, "line") == 0) {
        const float p[2][2] = {{20.0f, 200.0f}, {380.0f, 180.0f}};
        memcpy(buf, p, sizeof p);
        paths[0].v = buf, paths[0].n = 2;
        return 1;
    }
    if (strcmp(name, "dot") == 0) {
        buf[0][0] = 200.0f, buf[0][1] = 250.0f;
        paths[0].v = buf, paths[0].n = 1;
        return 1;
    }
    if (strcmp(name, "multi") == 0) {
        const float p[5][2] = {
            {10.0f, 280.0f}, {100.0f, 260.0f}, {300.0f, 100.0f}, {300.0f, 100.0f}, {390.0f, 150.0f}};
        memcpy(buf, p, sizeof p);
        paths[0].v = buf, paths[0].n = 2;
        paths[1].v = NULL, paths[1].n = 0;
        paths[2].v = buf + 2, paths[2].n = 3;
        return 3;
    }
    if (strcmp(name, "short") == 0) {
        const float p[2][2] = {{10.0f, 10.0f}, {10.4f, 10.3f}};
        memcpy(buf, p, sizeof p);
        paths[0].v = buf, paths[0].n = 2;
        return 1;
    }
    return 0;
}

static yyjson_doc *parse_doc(const char *text)
{
    yyjson_doc *doc = NULL;
    if (pc_json_parse(text, strlen(text), &doc, NULL) != PC_OK) {
        return NULL;
    }
    return doc;
}

static void spec_line(const char *want, char *got, size_t cap)
{
    char buf[2048], *kind, *in, *tab;
    yyjson_doc *doc;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    pc_error err;
    pc_status st;
    yyjson_mut_val *v = NULL;
    (void)snprintf(buf, sizeof buf, "%s", want + 5);
    kind = buf;
    in = strchr(buf, ' ');
    tab = strchr(buf, '\t');
    if (in == NULL || tab == NULL || d == NULL) {
        yyjson_mut_doc_free(d);
        return;
    }
    *in++ = '\0';
    *tab = '\0';
    doc = parse_doc(in);
    cat(got, cap, "spec %s %s\t", kind, in);
    if (doc == NULL) {
        yyjson_mut_doc_free(d);
        return;
    }
    if (strcmp(kind, "tree") == 0) {
        pc_algo_tree_spec s;
        st = pc_algo_tree_spec_from_json(yyjson_doc_get_root(doc), &s, &err);
        if (st == PC_OK) {
            v = pc_algo_tree_spec_to_json(d, &s);
        }
    } else if (strcmp(kind, "frame") == 0) {
        pc_algo_frame_spec s;
        st = pc_algo_frame_spec_from_json(yyjson_doc_get_root(doc), &s, &err);
        if (st == PC_OK) {
            v = pc_algo_frame_spec_to_json(d, &s);
        }
    } else {
        pc_algo_flame_spec s;
        st = pc_algo_flame_spec_from_json(yyjson_doc_get_root(doc), &s, &err);
        if (st == PC_OK) {
            v = pc_algo_flame_spec_to_json(d, &s);
        }
    }
    if (st == PC_OK) {
        cat_json(got, cap, d, v);
    } else {
        cat(got, cap, "ERR %s", err.msg);
    }
    yyjson_doc_free(doc);
    yyjson_mut_doc_free(d);
}

/* tree / frame / flame lines: the spec JSON comes from the golden line. */
static void shape_line(const char *want, char *got, size_t cap)
{
    char buf[4096], *tok[4];
    yyjson_doc *doc;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    pc_algo_prims prims = {NULL, 0, 0};
    pc_status st = PC_ERR_INVALID;
    yyjson_mut_val *v = NULL;
    pc_rect canvas = pc_rect_make(0, 0, 400, 300);
    (void)snprintf(buf, sizeof buf, "%s", want);
    if (split(buf, tok, 3) < 3 || d == NULL || (doc = parse_doc(tok[1])) == NULL) {
        yyjson_mut_doc_free(d);
        return;
    }
    cat(got, cap, "%s ", tok[0]);
    if (strcmp(tok[0], "tree") == 0) {
        pc_algo_tree_spec s;
        if (pc_algo_tree_spec_from_json(yyjson_doc_get_root(doc), &s, NULL) == PC_OK) {
            v = pc_algo_tree_spec_to_json(d, &s);
            st = pc_algo_tree(&s, canvas, &prims, NULL);
        }
        cat_json(got, cap, d, v);
    } else if (strcmp(tok[0], "frame") == 0) {
        pc_algo_frame_spec s;
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (sscanf(tok[2], "[%d, %d, %d, %d]", &x0, &y0, &x1, &y1) == 4 &&
            pc_algo_frame_spec_from_json(yyjson_doc_get_root(doc), &s, NULL) == PC_OK) {
            v = pc_algo_frame_spec_to_json(d, &s);
            st = pc_algo_picture_frame(&s, pc_rect_make(x0, y0, x1, y1), &prims, NULL);
        }
        cat_json(got, cap, d, v);
        cat(got, cap, " [%d, %d, %d, %d]", x0, y0, x1, y1);
    } else {
        pc_algo_flame_spec s;
        float pbuf[40][2];
        pc_algo_polyline paths[3];
        char pname[16] = {0};
        size_t np;
        (void)sscanf(tok[2], "%15s", pname);
        np = flame_paths(pname, canvas, pbuf, paths);
        if (pc_algo_flame_spec_from_json(yyjson_doc_get_root(doc), &s, NULL) == PC_OK) {
            v = pc_algo_flame_spec_to_json(d, &s);
            st = pc_algo_flame(&s, paths, np, &prims, NULL);
        }
        cat_json(got, cap, d, v);
        cat(got, cap, " %s", pname);
    }
    if (st == PC_OK) {
        cat(got, cap, " %zu %016llx", prims.n, (unsigned long long)hprims(&prims));
    } else {
        cat(got, cap, " ERR");
    }
    pc_algo_prims_fini(&prims);
    yyjson_doc_free(doc);
    yyjson_mut_doc_free(d);
}

static void comp_line(const char *pname, const char *fname, char *got, size_t cap)
{
    pc_rect small = pc_rect_make(0, 0, 80, 60);
    pc_algo_prims prims = {NULL, 0, 0};
    pc_surface sel;
    uint64_t h = H0;
    int32_t x, y;
    int k;
    pc_surface_init(&sel, PC_PIXEL_FORMAT_GRAY8);
    for (y = 0; y < 60; y++) {
        for (x = 0; x < 80; x++) {
            float v = (float)((x + y) % 11) / 10.0f;
            CHECK(pc_surface_write_pixel(&sel, x, y, &v, 1, NULL) == PC_OK);
        }
    }
    if (strcmp(pname, "tree") == 0) {
        pc_algo_tree_spec s = pc_algo_tree_spec_default();
        s.base_tree_type = 14;
        CHECK(pc_algo_tree(&s, small, &prims, NULL) == PC_OK);
    } else if (strcmp(pname, "frame") == 0) {
        pc_algo_frame_spec s = pc_algo_frame_spec_default();
        s.frame = 4;
        s.fade = 30.0f;
        CHECK(pc_algo_picture_frame(&s, small, &prims, NULL) == PC_OK);
    } else if (strcmp(pname, "flame") == 0) {
        pc_algo_flame_spec s = pc_algo_flame_spec_default();
        float pts[33][2];
        pc_algo_polyline pl;
        pc_algo_default_flame_path(small, pts);
        pl.v = pts, pl.n = 33;
        CHECK(pc_algo_flame(&s, &pl, 1, &prims, NULL) == PC_OK);
    } else {
        const float red[4] = {1, 0, 0, 1}, g6[4] = {0, 1, 0, 0.6f}, blue[4] = {0, 0, 1, 1},
                    yel[4] = {1, 1, 0, 1}, cy[4] = {0, 1, 1, 0.2f}, wh[4] = {1, 1, 1, 1},
                    gr[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        const float c1[2] = {20, 20}, a2[2] = {5, 50}, b2[2] = {70, 10}, c3[2] = {60, 40}, c4[2] = {40, 30},
                    c5[2] = {NAN, 2}, c6[2] = {300, 30};
        CHECK(pc_algo_prims_push(&prims, pc_algo_prim_disc(c1, 8, red), NULL) == PC_OK);
        CHECK(pc_algo_prims_push(&prims, pc_algo_prim_capsule(a2, b2, 3, 0.2f, g6, blue), NULL) == PC_OK);
        CHECK(pc_algo_prims_push(&prims, pc_algo_prim_ellipse(c3, 12, 4, 0.7f, yel, cy), NULL) == PC_OK);
        CHECK(pc_algo_prims_push(&prims, pc_algo_prim_capsule(c4, c4, 0.3f, 0.3f, wh, wh), NULL) == PC_OK);
        CHECK(pc_algo_prims_push(&prims, pc_algo_prim_disc(c5, 3, wh), NULL) == PC_OK);
        CHECK(pc_algo_prims_push(&prims, pc_algo_prim_ellipse(c6, 400, 2, 0, gr, gr), NULL) == PC_OK);
    }
    for (k = 0; k < 3; k++) {
        const pc_rect clips[3] = {small, pc_rect_make(10, 5, 300, 50), pc_rect_make(-50, -50, 0, 0)};
        pc_surface s;
        pc_rect used = PC_RECT_EMPTY;
        surf(&s, fmt_named(fname), pc_rect_make(-3, -2, 70, 50), 4, false);
        CHECK(pc_algo_render_composite(&s, prims.v, prims.n, clips[k], k == 1 ? &sel : NULL, &used, NULL) ==
              PC_OK);
        hrect(&h, used);
        hh(&h, hs(&s));
        pc_surface_fini(&s);
    }
    cat(got, cap, "comp %s %s %016llx", pname, fname, (unsigned long long)h);
    pc_algo_prims_fini(&prims);
    pc_surface_fini(&sel);
}

static void trap_line(size_t w, size_t h, size_t ch, size_t width, int32_t seed, char *got, size_t cap)
{
    size_t n = w * h * ch, i = 0, x, y, c;
    float *v = malloc(n * sizeof(float) + 4);
    uint64_t hh_ = H0;
    if (v == NULL) {
        return;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            for (c = 0; c < ch; c++) {
                float p = pat((int32_t)x, (int32_t)y, c, seed);
                v[i++] = p < 0.4f ? 0.0f : p;
            }
        }
    }
    CHECK(pc_algo_trap(v, n, w, h, ch, width, NULL) == PC_OK);
    hf(&hh_, v, n);
    free(v);
    cat(got, cap, "trap %zu %zu %zu %zu %d %016llx", w, h, ch, width, (int)seed, (unsigned long long)hh_);
}

/* ---- warp ---- */

typedef struct wmap {
    const char *name;
    pc_warp w;
} wmap;

static void warp_map(void *vc, double x, double y, double *ox, double *oy)
{
    const wmap *m = vc;
    const char *n = m->name;
    *ox = x, *oy = y;
    if (strcmp(n, "shift") == 0) {
        *ox = x + 10.0, *oy = y - 2.0;
    } else if (strcmp(n, "frac") == 0) {
        *ox = x + 0.37, *oy = y + 0.81;
    } else if (strcmp(n, "rotate") == 0) {
        *ox = 25.0 + (x - 25.0) * 0.8 - (y - 15.0) * 0.6;
        *oy = 15.0 + (x - 25.0) * 0.6 + (y - 15.0) * 0.8;
    } else if (strcmp(n, "scale") == 0) {
        *ox = x * 1.7 - 9.0, *oy = y * 0.6 + 4.0;
    } else if (strcmp(n, "fold") == 0) {
        *ox = 50.0 - fabs(x - 25.0) * 1.3;
    } else if (strcmp(n, "arc") == 0 || strcmp(n, "twist") == 0 || strcmp(n, "fisheye") == 0) {
        pc_point p = pc_warp_map(&m->w, x, y);
        *ox = p.x, *oy = p.y;
    } else if (strcmp(n, "flat") == 0) {
        *oy = 10.0;
    } else if (strcmp(n, "nan") == 0) {
        if (x > 20.0) {
            *ox = NAN;
        }
    } else if (strcmp(n, "far") == 0) {
        *ox = x + 3.0e6, *oy = y - 1.0e7;
    }
}

static const double WB[4] = {5.0, 3.0, 45.0, 27.0};

static void wmap_init(wmap *m, const char *name)
{
    m->name = name;
    if (strcmp(name, "arc") == 0) {
        pc_warp_preset(&m->w, PC_WARP_ARC, 50.0, WB);
    } else if (strcmp(name, "twist") == 0) {
        pc_warp_preset(&m->w, PC_WARP_TWIST, -40.0, WB);
    } else if (strcmp(name, "fisheye") == 0) {
        pc_warp_preset(&m->w, PC_WARP_FISHEYE, 70.0, WB);
    } else {
        pc_warp_none(&m->w, WB);
    }
}

static pc_algo_interp interp_named(const char *n)
{
    return strcmp(n, "nearest") == 0    ? PC_ALGO_INTERP_NEAREST
           : strcmp(n, "bilinear") == 0 ? PC_ALGO_INTERP_BILINEAR
                                        : PC_ALGO_INTERP_BICUBIC;
}

static pc_point mesh_shear(void *ctx, pc_point p)
{
    pc_point q = {p.x + (p.y - 3.0) * 0.2, p.y};
    (void)ctx;
    return q;
}

/* warps by name for place / placep; false for "none". */
static bool place_warp(const char *name, pc_warp *w)
{
    if (strcmp(name, "identity") == 0) {
        pc_warp_none(w, WB);
    } else if (strcmp(name, "bulge") == 0) {
        pc_warp_preset(w, PC_WARP_BULGE, 30.0, WB);
    } else if (strcmp(name, "mesh") == 0) {
        pc_mesh id, m;
        CHECK(pc_mesh_identity(WB, 2, 1, &id, NULL) == PC_OK);
        CHECK(pc_mesh_map_points(&id, mesh_shear, NULL, &m, NULL) == PC_OK);
        pc_mesh_fini(&id);
        pc_warp_custom(w, &m, WB);
    } else {
        return false;
    }
    return true;
}

static void warp_lines(const char *want, char *got, size_t cap)
{
    char buf[512], *tok[6];
    size_t nt;
    pc_surface src, o;
    (void)snprintf(buf, sizeof buf, "%s", want);
    nt = split(buf, tok, 6);
    if (strcmp(tok[0], "warp") == 0 && nt >= 5) {
        wmap m;
        wmap_init(&m, tok[1]);
        surf(&src, fmt_named(tok[2]), pc_rect_make(0, 0, 50, 30), 3, strcmp(tok[2], "rgb8") == 0);
        cat(got, cap, "warp %s %s %s %016llx", tok[1], tok[2], tok[3],
            (unsigned long long)hs_st(pc_algo_warp_mesh_surface(&src, pc_rect_make(5, 3, 45, 27), warp_map,
                                                                &m, interp_named(tok[3]), &o, NULL),
                                      &o));
        pc_warp_fini(&m.w);
        pc_surface_fini(&src);
    } else if (strcmp(tok[0], "warp_empty") == 0) {
        wmap m;
        wmap_init(&m, "identity");
        surf(&src, PC_PIXEL_FORMAT_RGBA8, pc_rect_make(0, 0, 10, 10), 1, false);
        cat(got, cap, "warp_empty %016llx",
            (unsigned long long)hs_st(pc_algo_warp_mesh_surface(&src, PC_RECT_EMPTY, warp_map, &m,
                                                                PC_ALGO_INTERP_BILINEAR, &o, NULL),
                                      &o));
        pc_warp_fini(&m.w);
        pc_surface_fini(&src);
    } else if (strcmp(tok[0], "tris") == 0 && nt >= 4) {
        static const pc_algo_warp_vertex verts[6] = {{{2, 2}, {0, 0}},     {{30, 4}, {40, 0}},
                                                     {{6, 25}, {0, 30}},   {{28, 28}, {40, 30}},
                                                     {{10, 10}, {10, 10}}, {{10, 10}, {20, 20}}};
        static const size_t tris[5][3] = {{0, 1, 2}, {1, 3, 2}, {0, 3, 4}, {4, 4, 5}, {5, 4, 4}};
        const pc_rect srs[3] = {pc_rect_make(0, 0, 50, 30), pc_rect_make(10, 5, 20, 15),
                                pc_rect_make(60, 60, 70, 70)};
        uint64_t h = H0;
        int k;
        surf(&src, fmt_named(tok[1]), pc_rect_make(0, 0, 50, 30), 6, false);
        for (k = 0; k < 3; k++) {
            hh(&h,
               hs_st(pc_algo_warp_triangles(&src, srs[k], verts, 6, tris, 5, interp_named(tok[2]), &o, NULL),
                     &o));
        }
        cat(got, cap, "tris %s %s %016llx", tok[1], tok[2], (unsigned long long)h);
        pc_surface_fini(&src);
    } else if ((strcmp(tok[0], "wgray") == 0 && nt >= 5) || (strcmp(tok[0], "wgray_empty") == 0 && nt >= 3)) {
        bool empty = tok[0][5] == '_';
        bool g8 = strcmp(tok[1], "gray8") == 0;
        pc_pixel_format fmt = {PC_COLOR_MODE_GRAYSCALE, g8 ? PC_SAMPLE_TYPE_U8 : PC_SAMPLE_TYPE_F32, false};
        float def = g8 ? 1.0f : 0.25f, zero = 0.0f;
        pc_surface m;
        wmap wm;
        int32_t x;
        pc_surface_init_default(&m, fmt, &def, 1);
        wmap_init(&wm, empty ? "identity" : tok[2]);
        if (!empty) {
            CHECK(pc_surface_fill_rect(&m, pc_rect_make(4, 4, 10, 10), &zero, 1, NULL) == PC_OK);
            for (x = 12; x < 20; x++) {
                float v = (float)(x - 12) / 8.0f;
                CHECK(pc_surface_write_pixel(&m, x, 6, &v, 1, NULL) == PC_OK);
            }
            cat(got, cap, "wgray %s %s %s %016llx", tok[1], tok[2], tok[3],
                (unsigned long long)hs_st(
                    pc_algo_warp_mesh_gray(&m, warp_map, &wm, interp_named(tok[3]), &o, NULL), &o));
        } else {
            cat(got, cap, "wgray_empty %s %016llx", tok[1],
                (unsigned long long)hs_st(
                    pc_algo_warp_mesh_gray(&m, warp_map, &wm, PC_ALGO_INTERP_BILINEAR, &o, NULL), &o));
        }
        pc_warp_fini(&wm.w);
        pc_surface_fini(&m);
    } else if ((strcmp(tok[0], "place") == 0 || strcmp(tok[0], "placep") == 0) && nt >= 5) {
        pc_rect r = pc_rect_make(5, 3, 45, 27);
        pc_warp w;
        bool has = place_warp(tok[3], &w);
        uint64_t h;
        surf(&src, fmt_named(tok[1]), r, 8, false);
        if (tok[0][5] == 'p') {
            pc_algo_homography hm = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
            if (strcmp(tok[2], "persp") == 0) {
                const pc_algo_homography p = {{1.1, 0.1, 2.0, 0.05, 0.9, 1.0, 0.002, 0.001, 1.0}};
                hm = p;
            }
            h = hs_st(pc_algo_place_source_projective(&src, r, &hm, has ? &w : NULL, &o, NULL), &o);
        } else {
            pc_affine t = {{1, 0, 0, 1, 0, 0}};
            const char *a = tok[2];
            if (strcmp(a, "intshift") == 0) {
                const pc_affine v = {{1, 0, 0, 1, 12, -7}};
                t = v;
            } else if (strcmp(a, "fracshift") == 0) {
                const pc_affine v = {{1, 0, 0, 1, 12.5, -7}};
                t = v;
            } else if (strcmp(a, "scale") == 0) {
                const pc_affine v = {{0.5, 0, 0, 1.5, 3, 2}};
                t = v;
            } else if (strcmp(a, "rot") == 0) {
                const pc_affine v = {{0.8, 0.6, -0.6, 0.8, 10, 0}};
                t = v;
            }
            h = hs_st(pc_algo_place_source(&src, r, &t, has ? &w : NULL, &o, NULL), &o);
        }
        cat(got, cap, "%s %s %s %s %016llx", tok[0], tok[1], tok[2], tok[3], (unsigned long long)h);
        if (has) {
            pc_warp_fini(&w);
        }
        pc_surface_fini(&src);
    }
}

static bool fx_replay_more(const char *want, char *got, size_t cap)
{
    char buf[512], *tok[8];
    size_t nt, i;
    got[0] = '\0';
    (void)snprintf(buf, sizeof buf, "%s", want);
    nt = split(buf, tok, 8);
    if (nt == 0) {
        return false;
    }
    if (strcmp(tok[0], "gal") == 0 && nt > 1) {
        gal_line(tok[1], got, cap);
    } else if (strcmp(tok[0], "galnew") == 0 && nt > 1) {
        pc_algo_gallery_filter f;
        pc_algo_gallery_effect e;
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        if (d != NULL && pc_algo_gallery_filter_from_key(tok[1], strlen(tok[1]), &f) &&
            pc_algo_gallery_effect_new(f, &e, NULL) == PC_OK) {
            cat(got, cap, "galnew %s ", tok[1]);
            cat_json(got, cap, d, pc_algo_gallery_effect_to_json(d, &e));
            pc_algo_gallery_effect_fini(&e);
        }
        yyjson_mut_doc_free(d);
    } else if (strcmp(tok[0], "galkey") == 0) {
        const char *q = strchr(want, '"'), *e = q != NULL ? strchr(q + 1, '"') : NULL;
        char k[64] = {0};
        if (e == NULL || (size_t)(e - q - 1) >= sizeof k) {
            return false;
        }
        memcpy(k, q + 1, (size_t)(e - q - 1));
        cat(got, cap, "galkey \"%s\" ", k);
        opt_key(got, cap, k);
    } else if (strcmp(tok[0], "galset") == 0) {
        galset_line(got, cap);
    } else if (strcmp(tok[0], "trees") == 0 || strcmp(tok[0], "frames") == 0) {
        bool trees = tok[0][0] == 't';
        cat(got, cap, "%s ", tok[0]);
        for (i = 0; i < (trees ? (size_t)PC_ALGO_TREE_TYPES : (size_t)PC_ALGO_FRAME_STYLES); i++) {
            cat(got, cap, "%s%s", i > 0 ? "|" : "",
                trees ? pc_algo_tree_type_names[i] : pc_algo_frame_styles[i]);
        }
    } else if (strcmp(tok[0], "rng") == 0) {
        pc_algo_rng r = pc_algo_rng_new(7, 0x1234);
        cat(got, cap, "rng");
        for (i = 0; i < 4; i++) {
            uint64_t a = pc_algo_rng_next(&r);
            uint32_t b = fbits(pc_algo_rng_f(&r)), c = fbits(pc_algo_rng_range(&r, -3.0f, 5.0f)),
                     d = fbits(pc_algo_rng_sym(&r));
            cat(got, cap, " %016llx/%08x/%08x/%08x", (unsigned long long)a, (unsigned)b, (unsigned)c,
                (unsigned)d);
        }
    } else if (strcmp(tok[0], "flamepath") == 0) {
        float p[33][2];
        uint64_t h = H0;
        pc_algo_default_flame_path(pc_rect_make(-7, 3, 91, 64), p);
        hf(&h, &p[0][0], 66);
        cat(got, cap, "flamepath %016llx", (unsigned long long)h);
    } else if (strcmp(tok[0], "tree") == 0 || strcmp(tok[0], "frame") == 0 || strcmp(tok[0], "flame") == 0) {
        shape_line(want, got, cap);
    } else if (strcmp(tok[0], "spec") == 0) {
        spec_line(want, got, cap);
    } else if (strcmp(tok[0], "comp") == 0 && nt >= 3) {
        comp_line(tok[1], tok[2], got, cap);
    } else if (strcmp(tok[0], "trap") == 0 && nt >= 6) {
        trap_line(strtoul(tok[1], NULL, 10), strtoul(tok[2], NULL, 10), strtoul(tok[3], NULL, 10),
                  strtoul(tok[4], NULL, 10), (int32_t)strtol(tok[5], NULL, 10), got, cap);
    } else if (strncmp(tok[0], "warp", 4) == 0 || strcmp(tok[0], "tris") == 0 ||
               strncmp(tok[0], "wgray", 5) == 0 || strncmp(tok[0], "place", 5) == 0) {
        warp_lines(want, got, cap);
    } else {
        return false;
    }
    return true;
}

#endif
