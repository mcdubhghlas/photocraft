/* Parity with crates/text layout, vertical type, warp, spell and TextEngine (chunk L14 section of
 * tests/golden/text-vectors.txt, written by `rustref text-vectors`, see the l14 part of
 * tools/rustref/src/text_vectors.rs for the line kinds). Every line is recomputed here and
 * compared with the reference's. `s` cases need the system fonts named by the `sysfonts` line
 * and are skipped (counted) when any is missing. Known differences (REWRITE_PLAN.md §7) are
 * listed in KNOWN and counted, not failed. `PC_L14_DETAIL=name` prints a case's layout. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "base/utf8.h"
#include "doc/json.h"
#include "harness.h"
#include "raster/surface.h"
#include "text/text.h"

#define GOLDEN "tests/golden/text-vectors.txt"
#define H0 0xcbf29ce484222325ull
#define MAXTOK 64

static pc_buf golden;
static int skipped, known, lines;

/* Lines whose difference is intentional (REWRITE_PLAN.md §7): "kind name". */
static const char *const KNOWN[] = {
    /* Thai line breaking: the reference's ICU dictionary finds word breaks inside Thai text,
     * UAX #14 alone (libunibreak) does not (D10). */
    "lay thai",
    "nav thai",
    "kern thai",
    NULL,
};

static bool is_known(const char *kind, const char *name)
{
    size_t i;
    char key[256];
    (void)snprintf(key, sizeof key, "%s %s", kind, name);
    for (i = 0; KNOWN[i] != NULL; i++) {
        if (strcmp(KNOWN[i], key) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- hashing like the reference ---- */

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
}
static void hu8(uint64_t *h, uint8_t v)
{
    fnv(h, &v, 1);
}
static void hu32(uint64_t *h, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    fnv(h, b, 4);
}
static void hu64(uint64_t *h, uint64_t v)
{
    uint8_t b[8];
    int i;
    for (i = 0; i < 8; i++) {
        b[i] = (uint8_t)(v >> (8 * i));
    }
    fnv(h, b, 8);
}
static uint32_t f32bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static void hf32(uint64_t *h, float v)
{
    hu32(h, f32bits(v));
}
static void hf64(uint64_t *h, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    hu64(h, u);
}
static float f32of(const char *hex)
{
    uint32_t u = (uint32_t)strtoul(hex, NULL, 16);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static double f64of(const char *hex)
{
    uint64_t u = strtoull(hex, NULL, 16);
    double f;
    memcpy(&f, &u, 8);
    return f;
}
static char *x32(char *b, float v)
{
    (void)snprintf(b, 16, "%08x", f32bits(v));
    return b;
}
static char *x64(char *b, double v)
{
    uint64_t u;
    memcpy(&u, &v, 8);
    (void)snprintf(b, 24, "%016llx", (unsigned long long)u);
    return b;
}

/* Hex string ("-" for empty) into bytes (NUL-terminated). */
static char *unhex(const char *t, size_t *len)
{
    size_t n = strcmp(t, "-") == 0 ? 0 : strlen(t) / 2, i;
    char *o = malloc(n + 1);
    if (o == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        char b[3] = {t[2 * i], t[2 * i + 1], 0};
        o[i] = (char)strtoul(b, NULL, 16);
    }
    o[n] = '\0';
    *len = n;
    return o;
}

static void hexs(pc_buf *o, const char *s, size_t n)
{
    size_t i;
    if (n == 0) {
        (void)pc_buf_append_str(o, "-", NULL);
    }
    for (i = 0; i < n; i++) {
        (void)pc_buf_appendf(o, NULL, "%02x", (unsigned char)s[i]);
    }
}

/* ---- golden lines ---- */

typedef struct line {
    char *buf;
    const char *tok[MAXTOK];
    int n;
    const char *want; /* text after " => " ("" when none) */
    const char *raw;  /* the whole line */
} line;

static bool next_line(size_t *pos, line *l)
{
    const char *d = (const char *)golden.data;
    size_t s = *pos, e = s;
    char *p, *copy;
    if (s >= golden.len) {
        return false;
    }
    while (e < golden.len && d[e] != '\n') {
        e++;
    }
    *pos = e + 1;
    l->buf = malloc(3 * (e - s) + 3);
    memcpy(l->buf, d + s, e - s);
    l->buf[e - s] = '\0';
    copy = l->buf + (e - s) + 1;
    memcpy(copy, d + s, e - s);
    copy[e - s] = '\0';
    l->raw = copy;
    p = strstr(l->buf, " => ");
    l->want = "";
    if (p != NULL) {
        char *w = copy + (e - s) + 1;
        strcpy(w, p + 4);
        l->want = w;
        *p = '\0';
    } else if (e - s >= 3 && strcmp(l->buf + (e - s) - 3, " =>") == 0) {
        l->buf[e - s - 3] = '\0';
    }
    l->n = 0;
    for (p = strtok(l->buf, " "); p != NULL && l->n < MAXTOK; p = strtok(NULL, " ")) {
        l->tok[l->n++] = p;
    }
    return true;
}

static void check_text(const char *kind, const char *name, const char *got, const char *want)
{
    pc_t_checks++;
    if (strcmp(got, want) != 0) {
        if (is_known(kind, name)) {
            known++;
            return;
        }
        PC_T_FAIL("%s %s: got \"%.400s\" want \"%.400s\"", kind, name, got, want);
    }
}

/* ---- cases ---- */

typedef struct ccase {
    char name[128];
    char eng;
    float dpi;
    pc_text_layer layer;
    bool ok, have_layout;
    pc_text_layout l;
} ccase;

static bool apply_fix(pc_text_layer *t, const char *fix)
{
    char key[32];
    const char *eq = strchr(fix, '='), *dot = strchr(fix, '.');
    float v;
    size_t i;
    if (eq == NULL || dot == NULL || (size_t)(eq - fix) >= sizeof key) {
        return false;
    }
    memcpy(key, fix, (size_t)(eq - fix));
    key[eq - fix] = '\0';
    v = f32of(eq + 1);
    i = (size_t)strtoul(fix + 1, NULL, 10);
    dot = strchr(key, '.') + 1;
    if (fix[0] == 'R' && i < t->nruns) {
        pc_char_style *s = &t->runs[i].style;
        if (strcmp(dot, "s") == 0) {
            s->size_pt = v;
        } else if (strcmp(dot, "t") == 0) {
            s->tracking = v;
        } else if (strcmp(dot, "l") == 0) {
            s->leading_pt = v;
        } else if (strcmp(dot, "b") == 0) {
            s->baseline_shift_pt = v;
        } else if (strcmp(dot, "h") == 0) {
            s->horizontal_scale = v;
        } else if (strcmp(dot, "v") == 0) {
            s->vertical_scale = v;
        } else if (strcmp(dot, "k") == 0) {
            s->kern = v;
        } else {
            return false;
        }
        return true;
    }
    if (fix[0] == 'P' && i < t->nparagraphs) {
        pc_paragraph_style *s = &t->paragraphs[i].style;
        if (strcmp(dot, "fi") == 0) {
            s->first_line_indent_pt = v;
        } else if (strcmp(dot, "si") == 0) {
            s->start_indent_pt = v;
        } else if (strcmp(dot, "ei") == 0) {
            s->end_indent_pt = v;
        } else if (strcmp(dot, "sb") == 0) {
            s->space_before_pt = v;
        } else if (strcmp(dot, "sa") == 0) {
            s->space_after_pt = v;
        } else if (strcmp(dot, "al") == 0) {
            s->auto_leading = v;
        } else {
            return false;
        }
        return true;
    }
    if (fix[0] == 'S') {
        if (strcmp(dot, "x") == 0) {
            t->shape.x = v;
        } else if (strcmp(dot, "y") == 0) {
            t->shape.y = v;
        } else if (strcmp(dot, "w") == 0) {
            t->shape.width = v;
        } else if (strcmp(dot, "h") == 0) {
            t->shape.height = v;
        } else {
            return false;
        }
        return true;
    }
    return false;
}

static yyjson_doc *json_of(const char *hex)
{
    size_t n;
    char *s = unhex(hex, &n);
    yyjson_doc *d = NULL;
    if (s != NULL) {
        (void)pc_json_parse(s, n, &d, NULL);
    }
    free(s);
    return d;
}

static bool parse_case(const line *l, ccase *c)
{
    yyjson_doc *runs = NULL, *paras = NULL, *shape = NULL, *orient = NULL, *warp = NULL;
    size_t n, i;
    char *text;
    bool ok = true;
    pc_error e;
    if (l->n < 12) {
        return false;
    }
    memset(c, 0, sizeof *c);
    pc_text_layer_default(&c->layer);
    (void)snprintf(c->name, sizeof c->name, "%s", l->tok[1]);
    c->eng = l->tok[2][0];
    c->dpi = f32of(l->tok[3]);
    text = unhex(l->tok[4], &n);
    if (text == NULL || pc_str_set(&c->layer.text, text, n, NULL) != PC_OK) {
        free(text);
        return false;
    }
    free(text);
    runs = json_of(l->tok[5]);
    paras = json_of(l->tok[6]);
    shape = json_of(l->tok[7]);
    orient = json_of(l->tok[8]);
    if (strcmp(l->tok[9], "-") != 0) {
        warp = json_of(l->tok[9]);
    }
    if (runs == NULL || paras == NULL || shape == NULL || orient == NULL) {
        ok = false;
        goto done;
    }
    {
        yyjson_val *a = yyjson_doc_get_root(runs);
        size_t cnt = yyjson_arr_size(a);
        c->layer.runs = calloc(cnt > 0 ? cnt : 1, sizeof *c->layer.runs);
        for (i = 0; ok && i < cnt; i++) {
            ok = pc_text_run_from_json(yyjson_arr_get(a, i), &c->layer.runs[i], &e) == PC_OK;
            if (ok) {
                c->layer.nruns++;
            } else {
                (void)fprintf(stderr, "run json: %s\n", e.msg);
            }
        }
        a = yyjson_doc_get_root(paras);
        cnt = yyjson_arr_size(a);
        c->layer.paragraphs = calloc(cnt > 0 ? cnt : 1, sizeof *c->layer.paragraphs);
        for (i = 0; ok && i < cnt; i++) {
            ok = pc_paragraph_run_from_json(yyjson_arr_get(a, i), &c->layer.paragraphs[i], NULL) == PC_OK;
            if (ok) {
                c->layer.nparagraphs++;
            }
        }
    }
    ok = ok && pc_text_shape_from_json(yyjson_doc_get_root(shape), &c->layer.shape, NULL) == PC_OK;
    ok = ok && pc_orientation_from_json(yyjson_doc_get_root(orient), &c->layer.orientation, NULL) == PC_OK;
    if (ok && warp != NULL) {
        ok = pc_text_warp_from_json(yyjson_doc_get_root(warp), &c->layer.warp, NULL) == PC_OK;
        c->layer.has_warp = ok;
    }
    {
        char tm[256], *p = tm, *q;
        (void)snprintf(tm, sizeof tm, "%s", l->tok[10]);
        for (i = 0; i < 6; i++) {
            q = strchr(p, ':');
            if (q != NULL) {
                *q = '\0';
            }
            c->layer.transform.m[i] = f64of(p);
            if (q == NULL) {
                break;
            }
            p = q + 1;
        }
    }
    if (ok && strcmp(l->tok[11], "-") != 0) {
        char fixes[1024], *f, *save = NULL;
        (void)snprintf(fixes, sizeof fixes, "%s", l->tok[11]);
        for (f = strtok_r(fixes, ",", &save); f != NULL && ok; f = strtok_r(NULL, ",", &save)) {
            ok = apply_fix(&c->layer, f);
        }
    }
done:
    yyjson_doc_free(runs);
    yyjson_doc_free(paras);
    yyjson_doc_free(shape);
    yyjson_doc_free(orient);
    yyjson_doc_free(warp);
    return ok;
}

static void case_fini(ccase *c)
{
    pc_text_layer_fini(&c->layer);
    if (c->have_layout) {
        pc_text_layout_fini(&c->l);
    }
    memset(c, 0, sizeof *c);
}

static void hash_layout(const pc_text_layout *l, uint64_t h[6])
{
    size_t i, j;
    for (i = 0; i < 6; i++) {
        h[i] = H0;
    }
    for (i = 0; i < l->nfaces; i++) {
        const pc_glyph_face *f = &l->faces[i];
        size_t len;
        (void)pc_font_data(f->font, &len);
        hu64(&h[0], len);
        hu32(&h[0], pc_font_index(f->font));
        hu32(&h[0], (uint32_t)f->ncoords);
        for (j = 0; j < f->ncoords; j++) {
            uint16_t u = (uint16_t)f->coords[j];
            uint8_t b[2] = {(uint8_t)u, (uint8_t)(u >> 8)};
            fnv(&h[0], b, 2);
        }
        hf32(&h[0], f->size_px);
        hu8(&h[0], f->embolden ? 1 : 0);
        hf32(&h[0], f->skew_deg);
    }
    for (i = 0; i < l->nglyphs; i++) {
        const pc_placed_glyph *g = &l->glyphs[i];
        hu32(&h[1], g->face);
        hu32(&h[1], g->id);
        hf32(&h[1], g->x);
        hf32(&h[1], g->y);
        hu32(&h[1], g->style);
        hu8(&h[1], (uint8_t)g->orient);
    }
    for (i = 0; i < l->ndecorations; i++) {
        const pc_decoration_rect *d = &l->decorations[i];
        hf32(&h[2], d->x0);
        hf32(&h[2], d->y0);
        hf32(&h[2], d->x1);
        hf32(&h[2], d->y1);
        hu32(&h[2], d->style);
    }
    for (i = 0; i < l->nlines; i++) {
        const pc_line_info *li = &l->lines[i];
        hu64(&h[3], li->start);
        hu64(&h[3], li->end);
        hf32(&h[3], li->baseline);
        hf32(&h[3], li->x0);
        hf32(&h[3], li->x1);
        hf32(&h[3], li->ascent);
        hf32(&h[3], li->descent);
        hu64(&h[3], li->paragraph);
    }
    for (i = 0; i < l->nclusters; i++) {
        const pc_cluster_info *c = &l->clusters[i];
        hu64(&h[4], c->start);
        hu64(&h[4], c->end);
        hf32(&h[4], c->x);
        hf32(&h[4], c->advance);
        hu64(&h[4], c->line);
        hu8(&h[4], c->rtl ? 1 : 0);
    }
    for (i = 0; i < l->nstyles; i++) {
        /* A runtime face alias (".PhotoCraft-face-<family id>-<index>") without its family id:
         * the id is process-wide state, so the reference hashes ".PhotoCraft-face-<index>". */
        const char *f = pc_str_c(&l->styles[i].font_family), *dash;
        size_t fl = l->styles[i].font_family.len;
        if (fl > 17 && memcmp(f, ".PhotoCraft-face-", 17) == 0 &&
            (dash = memchr(f + 17, '-', fl - 17)) != NULL) {
            fnv(&h[5], ".PhotoCraft-face-", 17);
            fnv(&h[5], dash + 1, fl - (size_t)(dash + 1 - f));
        } else {
            fnv(&h[5], f, fl);
        }
        hu32(&h[5], l->styles[i].weight);
        hu8(&h[5], l->styles[i].italic ? 1 : 0);
    }
}

static void detail(const ccase *c)
{
    const char *want = getenv("PC_L14_DETAIL");
    const pc_text_layout *l = &c->l;
    size_t i;
    if (want == NULL || strcmp(want, c->name) != 0) {
        return;
    }
    for (i = 0; i < l->nfaces; i++) {
        size_t len;
        (void)pc_font_data(l->faces[i].font, &len);
        printf("DF %zu len=%zu idx=%u size=%.9g emb=%d skew=%.9g ncoords=%zu\n", i, len,
               pc_font_index(l->faces[i].font), (double)l->faces[i].size_px, l->faces[i].embolden,
               (double)l->faces[i].skew_deg, l->faces[i].ncoords);
    }
    for (i = 0; i < l->nglyphs; i++) {
        const pc_placed_glyph *g = &l->glyphs[i];
        printf("DG f=%u id=%u x=%.9g y=%.9g s=%u o=%d\n", g->face, g->id, (double)g->x, (double)g->y,
               g->style, (int)g->orient);
    }
    for (i = 0; i < l->nclusters; i++) {
        const pc_cluster_info *cl = &l->clusters[i];
        printf("DC %zu..%zu x=%.9g a=%.9g line=%zu rtl=%d\n", cl->start, cl->end, (double)cl->x,
               (double)cl->advance, cl->line, cl->rtl);
    }
    for (i = 0; i < l->nlines; i++) {
        const pc_line_info *li = &l->lines[i];
        printf("DL %zu..%zu b=%.9g x0=%.9g x1=%.9g a=%.9g d=%.9g p=%zu\n", li->start, li->end,
               (double)li->baseline, (double)li->x0, (double)li->x1, (double)li->ascent, (double)li->descent,
               li->paragraph);
    }
    for (i = 0; i < l->ndecorations; i++) {
        const pc_decoration_rect *d = &l->decorations[i];
        printf("DD %.9g %.9g %.9g %.9g %u\n", (double)d->x0, (double)d->y0, (double)d->x1, (double)d->y1,
               d->style);
    }
}

static void hash_opt4(uint64_t *h, bool has, const float b[4])
{
    size_t i;
    hu8(h, has ? 1 : 0);
    for (i = 0; has && i < 4; i++) {
        hf32(h, b[i]);
    }
}

static uint64_t nav_hash(const pc_text_layout *l, const char *text, size_t len)
{
    uint64_t h = H0;
    float b[4], seg[4], x, t, bo;
    bool hb;
    size_t i, n = 0, k;
    int ix, iy, dir;
    hb = pc_text_layout_bounds(l, b);
    hash_opt4(&h, hb, b);
    hb = pc_text_layout_line_bounds(l, b);
    hash_opt4(&h, hb, b);
    for (i = 0; i <= len + 1; i++) {
        pc_text_layout_caret(l, i, &x, &t, &bo);
        hf32(&h, x);
        hf32(&h, t);
        hf32(&h, bo);
        pc_text_layout_caret_segment(l, i, seg);
        for (k = 0; k < 4; k++) {
            hf32(&h, seg[k]);
        }
        hu64(&h, pc_text_line_index(l, i));
        hu64(&h, pc_text_char_index(text, len, i));
    }
    if (!pc_text_layout_bounds(l, b)) {
        b[0] = b[1] = b[2] = b[3] = 0.0f;
    }
    for (iy = 0; iy < 9; iy++) {
        for (ix = 0; ix < 9; ix++) {
            float px = b[0] - 10.0f + (b[2] - b[0] + 20.0f) * (float)ix / 8.0f;
            float py = b[1] - 10.0f + (b[3] - b[1] + 20.0f) * (float)iy / 8.0f;
            size_t ci, li;
            hu64(&h, pc_text_layout_hit_test(l, px, py));
            pc_text_hit_char(l, text, len, px, py, &ci, &li);
            hu64(&h, ci);
            hu64(&h, li);
            hu8(&h, pc_text_point_inside(l, px, py, 2.0f) ? 1 : 0);
        }
    }
    {
        size_t at = 0;
        while (at < len) {
            uint32_t cp;
            if (!pc_utf8_next((const uint8_t *)text, len, &at, &cp)) {
                at++;
            }
            n++;
        }
    }
    for (i = 0; i <= n + 1; i++) {
        int fwd;
        hu64(&h, pc_text_byte_index(text, len, i));
        for (fwd = 0; fwd < 2; fwd++) {
            hu64(&h, pc_text_word_boundary(text, len, i, fwd != 0));
            hu64(&h, pc_text_line_edge(l, text, len, i, fwd != 0));
        }
        pc_text_layout_caret(l, pc_text_byte_index(text, len, i < n ? i : n), &x, &t, &bo);
        for (dir = -1; dir <= 1; dir += 2) {
            hu64(&h, pc_text_line_step(l, text, len, i, x, dir));
            hu64(&h, pc_text_line_step(l, text, len, i, x + 7.5f, dir));
        }
    }
    return h;
}

static bool fmt_of(const char *n, pc_pixel_format *f)
{
    if (strcmp(n, "rgba8") == 0) {
        *f = (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};
    } else if (strcmp(n, "cmyka8") == 0) {
        *f = (pc_pixel_format){PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true};
    } else if (strcmp(n, "rgba32f") == 0) {
        *f = (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true};
    } else {
        return false;
    }
    return true;
}

static uint64_t surface_hash(const pc_rendered *r)
{
    uint64_t h = H0;
    if (r->rect.x1 > r->rect.x0 && r->rect.y1 > r->rect.y0) {
        float *px = NULL;
        size_t n = 0, j;
        if (pc_surface_read_region(&r->surface, r->rect, &px, &n, NULL) == PC_OK) {
            for (j = 0; j < n; j++) {
                hf32(&h, px[j]);
            }
            free(px);
        }
    }
    return h;
}

static void hash_els(uint64_t *h, const pc_path_el *e, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint8_t k = (uint8_t)e[i].kind;
        int np = e[i].kind == PC_PATH_QUAD    ? 2
                 : e[i].kind == PC_PATH_CURVE ? 3
                 : e[i].kind == PC_PATH_CLOSE ? 0
                                              : 1,
            j;
        fnv(h, &k, 1);
        for (j = 0; j < np; j++) {
            hf64(h, e[i].p[j][0]);
            hf64(h, e[i].p[j][1]);
        }
    }
}

/* ---- unit lines ---- */

static void unit_line(const line *l, const char *k)
{
    pc_buf got = PC_BUF_INIT;
    size_t n;
    char b1[24], b2[24];
    if (strcmp(k, "vclass") == 0) {
        uint64_t h = H0;
        uint32_t u;
        for (u = 0; u < 0x30000u; u++) {
            if (u < 0xD800 || u > 0xDFFF) {
                hu8(&h, (uint8_t)pc_text_vertical_class(u));
            }
        }
        for (u = 0xE0000; u < 0xE0080; u++) {
            hu8(&h, (uint8_t)pc_text_vertical_class(u));
        }
        (void)pc_buf_appendf(&got, NULL, "%016llx", (unsigned long long)h);
        check_text(k, "", pc_buf_cstr(&got), l->tok[1]);
    } else if (strcmp(k, "split") == 0) {
        char *s = unhex(l->tok[1], &n);
        size_t *r = NULL, nr = 0, i;
        if (s != NULL && pc_text_split_paragraphs(s, n, &r, &nr, NULL) == PC_OK) {
            for (i = 0; i < nr; i++) {
                (void)pc_buf_appendf(&got, NULL, "%s%zu..%zu", i > 0 ? " " : "", r[2 * i], r[2 * i + 1]);
            }
        }
        free(r);
        free(s);
        check_text(k, l->tok[1], pc_buf_cstr(&got), l->want);
    } else if (strcmp(k, "wordb") == 0) {
        char *s = unhex(l->tok[1], &n);
        if (s != NULL) {
            (void)pc_buf_appendf(&got, NULL, "%zu",
                                 pc_text_word_boundary(s, n, (size_t)atol(l->tok[2]), atoi(l->tok[3]) != 0));
        }
        free(s);
        check_text(k, l->tok[1], pc_buf_cstr(&got), l->want);
    } else if (strcmp(k, "warpstyle") == 0) {
        /* esc'd id: only \- (empty) occurs among the cases besides plain ASCII */
        const char *id = strcmp(l->tok[1], "\\-") == 0 ? "" : l->tok[1];
        const char *r = pc_text_warp_psd_style(id, strlen(id));
        check_text(k, l->tok[1], r != NULL ? r : "?", l->want);
    } else if (strcmp(k, "warp") == 0) {
        pc_text_warp w;
        float b[4];
        char tmp[64], *p, *q;
        size_t i;
        pc_style_warp sw;
        memset(&w, 0, sizeof w);
        (void)pc_str_set_c(&w.style, l->tok[1], NULL);
        w.value = f32of(l->tok[2]);
        (void)snprintf(tmp, sizeof tmp, "%s", l->tok[3]);
        p = tmp;
        for (i = 0; i < 4; i++) {
            q = strchr(p, ':');
            if (q != NULL) {
                *q = '\0';
            }
            b[i] = f32of(p);
            p = q != NULL ? q + 1 : p;
        }
        /* The reference's cases: distortions and orientation by style. */
        if (strcmp(l->tok[1], "warpFlag") == 0) {
            w.horizontal_distortion = 10.0f;
            w.horizontal = false;
        } else if (strcmp(l->tok[1], "warpTwist") == 0) {
            w.horizontal_distortion = -50.0f;
            w.vertical_distortion = 30.0f;
            w.horizontal = true;
        } else {
            w.horizontal = true;
        }
        if (!pc_text_warp_prepare(&w, b, &sw)) {
            (void)pc_buf_append_str(&got, "none", NULL);
        } else {
            uint64_t h = H0;
            int ix, iy;
            for (iy = 0; iy < 7; iy++) {
                for (ix = 0; ix < 7; ix++) {
                    pc_point pt = pc_style_warp_apply(&sw, -10.0 + 37.0 * ix, -50.0 + 13.0 * iy);
                    hf64(&h, pt.x);
                    hf64(&h, pt.y);
                }
            }
            (void)pc_buf_appendf(&got, NULL, "%s %016llx", x64(b1, pc_style_warp_max_segment(&sw)),
                                 (unsigned long long)h);
        }
        pc_str_fini(&w.style);
        check_text(k, l->tok[1], pc_buf_cstr(&got), l->want);
    } else {
        PC_T_FAIL("unknown golden line kind %s", k);
    }
    (void)b2;
    pc_buf_fini(&got);
}

static void spell_line(const line *l, const char *k, const pc_spell_dict **small)
{
    const pc_spell_dict *d = pc_spell_english();
    pc_buf got = PC_BUF_INIT;
    size_t n, i;
    char *s;
    static const char *const user_words[] = {"photocraft", "zxqv"};
    pc_spell_user user = {user_words, 2};
    if (strcmp(k, "dict") == 0) {
        (void)pc_buf_appendf(&got, NULL, "%zu", pc_spell_len(d));
        check_text(k, "", pc_buf_cstr(&got), l->tok[1]);
    } else if (strcmp(k, "check") == 0 || strcmp(k, "checku") == 0) {
        s = unhex(l->tok[1], &n);
        (void)pc_buf_appendf(&got, NULL, "%d",
                             pc_spell_check(d, s, n, strcmp(k, "checku") == 0 ? &user : NULL));
        free(s);
        check_text(k, l->tok[1], pc_buf_cstr(&got), l->want);
    } else if (strcmp(k, "suggest") == 0) {
        pc_spell_list sl = {NULL, 0};
        s = unhex(l->tok[1], &n);
        if (pc_spell_suggest(d, s, n, (size_t)atol(l->tok[2]), &sl, NULL) == PC_OK) {
            for (i = 0; i < sl.n; i++) {
                if (i > 0) {
                    (void)pc_buf_append_byte(&got, ' ', NULL);
                }
                hexs(&got, sl.v[i], strlen(sl.v[i]));
            }
        }
        pc_spell_list_fini(&sl);
        free(s);
        check_text(k, l->tok[1], pc_buf_cstr(&got), l->want);
    } else if (strcmp(k, "words") == 0 || strcmp(k, "missp") == 0) {
        pc_spell_span *w = NULL;
        size_t nw = 0;
        pc_status st;
        s = unhex(l->tok[1], &n);
        st = strcmp(k, "words") == 0 ? pc_spell_words(s, n, &w, &nw, NULL)
                                     : pc_spell_misspellings(d, s, n, NULL, &w, &nw, NULL);
        if (st == PC_OK) {
            for (i = 0; i < nw; i++) {
                (void)pc_buf_appendf(&got, NULL, "%s%zu..%zu", i > 0 ? " " : "", w[i].start, w[i].end);
            }
        }
        free(w);
        free(s);
        check_text(k, l->tok[1], pc_buf_cstr(&got), l->want);
    } else if (strcmp(k, "small") == 0) {
        static const char list[] = "#10\nhello\nworld\nParis\n#bad\n  spaced  \n#20\nhello\nNASA\n\n";
        pc_spell_dict *sd = NULL;
        if (*small == NULL && pc_spell_from_list(list, sizeof list - 1, &sd, NULL) == PC_OK) {
            *small = sd;
        }
        (void)pc_buf_appendf(&got, NULL, "%zu", pc_spell_len(*small));
        check_text(k, "", pc_buf_cstr(&got), l->tok[1]);
    } else if (strcmp(k, "smallcheck") == 0) {
        (void)pc_buf_appendf(&got, NULL, "%d", pc_spell_check(*small, l->tok[1], strlen(l->tok[1]), NULL));
        check_text(k, l->tok[1], pc_buf_cstr(&got), l->want);
    } else if (strcmp(k, "smallsuggest") == 0) {
        pc_spell_list sl = {NULL, 0};
        if (pc_spell_suggest(*small, l->tok[1], strlen(l->tok[1]), 5, &sl, NULL) == PC_OK) {
            for (i = 0; i < sl.n; i++) {
                (void)pc_buf_appendf(&got, NULL, "%s%s", i > 0 ? "," : "", sl.v[i]);
            }
        }
        pc_spell_list_fini(&sl);
        check_text(k, l->tok[1], pc_buf_cstr(&got), l->want);
    }
    pc_buf_fini(&got);
}

static bool is_spell_kind(const char *k)
{
    static const char *const kinds[] = {"dict",  "check", "checku",     "suggest",      "words",
                                        "missp", "small", "smallcheck", "smallsuggest", NULL};
    size_t i;
    for (i = 0; kinds[i] != NULL; i++) {
        if (strcmp(kinds[i], k) == 0) {
            return true;
        }
    }
    return false;
}

static void test_golden(void)
{
    size_t pos = 0;
    bool in = false, sys_ok = false;
    pc_text_engine *eb = NULL, *es = NULL;
    const pc_spell_dict *small = NULL;
    ccase c;
    line l;
    memset(&c, 0, sizeof c);
    CHECK(pc_file_read(GOLDEN, (size_t)64 << 20, &golden, NULL) == PC_OK);
    CHECK(pc_text_engine_new(&eb, NULL) == PC_OK);
    CHECK(pc_text_engine_new(&es, NULL) == PC_OK);
    if (eb == NULL || es == NULL) {
        return;
    }
    while (next_line(&pos, &l)) {
        const char *k = l.n > 0 ? l.tok[0] : "";
        if (strcmp(k, "#") == 0) {
            in = l.n > 1 && strcmp(l.tok[1], "l14") == 0;
            free(l.buf);
            continue;
        }
        if (!in || l.n == 0) {
            free(l.buf);
            continue;
        }
        lines++;
        if (strcmp(k, "sysfonts") == 0) {
            int i;
            sys_ok = true;
            for (i = 1; i < l.n; i++) {
                pc_buf b = PC_BUF_INIT;
                if (pc_file_read(l.tok[i], (size_t)256 << 20, &b, NULL) != PC_OK) {
                    sys_ok = false;
                } else {
                    (void)pc_fontdb_register_font_data(pc_text_engine_fonts(es), b.data, b.len, NULL, NULL);
                }
                pc_buf_fini(&b);
            }
        } else if (strcmp(k, "case") == 0) {
            case_fini(&c);
            c.ok = parse_case(&l, &c);
            if (!c.ok) {
                PC_T_FAIL("case %s: can't parse", l.n > 1 ? l.tok[1] : "?");
            }
        } else if (strcmp(k, "lay") == 0 || strcmp(k, "nav") == 0 || strcmp(k, "kern") == 0 ||
                   strcmp(k, "ren") == 0 || strcmp(k, "renw") == 0 || strcmp(k, "outl") == 0 ||
                   strcmp(k, "renl") == 0) {
            pc_text_engine *e = c.eng == 's' ? es : eb;
            pc_buf got = PC_BUF_INIT;
            const char *rest = strchr(strchr(l.raw, ' ') + 1, ' ');
            rest = rest != NULL ? rest + 1 : "";
            if (!c.ok || (c.eng == 's' && !sys_ok)) {
                skipped++;
                free(l.buf);
                continue;
            }
            if (strcmp(k, "lay") == 0) {
                pc_error err;
                uint64_t h[6];
                char b[16];
                if (c.have_layout) {
                    pc_text_layout_fini(&c.l);
                    c.have_layout = false;
                }
                if (pc_text_engine_layout(e, &c.layer, c.dpi, &c.l, &err) != PC_OK) {
                    (void)pc_buf_appendf(&got, NULL, "error %s", err.msg);
                } else {
                    c.have_layout = true;
                    detail(&c);
                    hash_layout(&c.l, h);
                    (void)pc_buf_appendf(
                        &got, NULL,
                        "%d %s %zu %zu %zu %zu %zu %016llx %016llx %016llx %016llx %016llx %016llx",
                        c.l.vertical, x32(b, c.l.px_per_pt), c.l.nfaces, c.l.nglyphs, c.l.ndecorations,
                        c.l.nlines, c.l.nclusters, (unsigned long long)h[0], (unsigned long long)h[1],
                        (unsigned long long)h[2], (unsigned long long)h[3], (unsigned long long)h[4],
                        (unsigned long long)h[5]);
                }
            } else if (strcmp(k, "nav") == 0 && c.have_layout) {
                (void)pc_buf_appendf(
                    &got, NULL, "%016llx",
                    (unsigned long long)nav_hash(&c.l, pc_str_c(&c.layer.text), c.layer.text.len));
            } else if (strcmp(k, "kern") == 0) {
                uint64_t h = H0;
                size_t at, len = c.layer.text.len;
                for (at = 0; at < (len < 48 ? len : 48) + 1; at++) {
                    bool has = false;
                    float kk = 0.0f;
                    (void)pc_text_engine_pair_kerning(e, &c.layer, c.dpi, at, &has, &kk, NULL);
                    hu8(&h, has ? 1 : 0);
                    if (has) {
                        hf32(&h, kk);
                    }
                }
                (void)pc_buf_appendf(&got, NULL, "%016llx", (unsigned long long)h);
            } else {
                pc_text_layer layer = c.layer;
                pc_pixel_format rgba8 = {PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};
                layer.transform = (pc_affine){{1.0, 0.0, 0.0, 1.0, 7.0, 60.0}};
                if (strcmp(k, "ren") == 0) {
                    pc_pixel_format f;
                    pc_rendered r;
                    if (fmt_of(l.tok[2], &f) &&
                        pc_text_engine_render(e, &layer, c.dpi, f, NULL, &r, NULL) == PC_OK) {
                        (void)pc_buf_appendf(&got, NULL, "%s %d %d %d %d %016llx", l.tok[2], (int)r.rect.x0,
                                             (int)r.rect.y0, (int)r.rect.x1, (int)r.rect.y1,
                                             (unsigned long long)surface_hash(&r));
                        pc_rendered_fini(&r);
                    }
                } else {
                    pc_text_warp w;
                    pc_text_layout wl;
                    pc_rendered r;
                    memset(&w, 0, sizeof w);
                    (void)pc_str_set_c(&w.style, "warpArc", NULL);
                    w.value = 40.0f;
                    w.horizontal = true;
                    layer.warp = w;
                    layer.has_warp = true;
                    if (pc_text_engine_render(e, &layer, c.dpi, rgba8, &wl, &r, NULL) == PC_OK) {
                        if (strcmp(k, "renw") == 0) {
                            (void)pc_buf_appendf(&got, NULL, "%d %d %d %d %016llx", (int)r.rect.x0,
                                                 (int)r.rect.y0, (int)r.rect.x1, (int)r.rect.y1,
                                                 (unsigned long long)surface_hash(&r));
                        } else if (strcmp(k, "outl") == 0) {
                            pc_style_warp sw;
                            bool hw = pc_text_layout_warp(&wl, &layer.warp, &sw);
                            pc_glyph_outline *o = NULL;
                            size_t no = 0, j;
                            uint64_t h = H0;
                            if (pc_text_outlines(&wl, &layer.transform, hw ? &sw : NULL, &o, &no, NULL) ==
                                PC_OK) {
                                for (j = 0; j < no; j++) {
                                    hu32(&h, (uint32_t)o[j].n);
                                    hash_els(&h, o[j].els, o[j].n);
                                }
                                pc_glyph_outlines_free(o, no);
                            }
                            (void)pc_buf_appendf(&got, NULL, "%zu %016llx", no, (unsigned long long)h);
                        } else {
                            pc_text_layer rl;
                            pc_rect rect;
                            pc_rendered fresh;
                            bool same = false;
                            rl = c.layer;
                            rl.transform = layer.transform;
                            rl.has_cache = false;
                            memset(&rl.cache, 0, sizeof rl.cache);
                            if (pc_text_engine_render_layer(e, &rl, c.dpi, rgba8, &rect, NULL) == PC_OK &&
                                pc_text_engine_render(e, &rl, c.dpi, rgba8, NULL, &fresh, NULL) == PC_OK) {
                                float *a = NULL, *b = NULL;
                                size_t na = 0, nb = 0;
                                if (pc_surface_read_region(&rl.cache, rect, &a, &na, NULL) == PC_OK &&
                                    pc_surface_read_region(&fresh.surface, rect, &b, &nb, NULL) == PC_OK) {
                                    same = na == nb && (na == 0 || memcmp(a, b, na * sizeof *a) == 0);
                                }
                                free(a);
                                free(b);
                                pc_rendered_fini(&fresh);
                            }
                            if (rl.has_cache) {
                                pc_surface_fini(&rl.cache);
                            }
                            (void)pc_buf_appendf(&got, NULL, "%d %d %d %d %d", (int)rect.x0, (int)rect.y0,
                                                 (int)rect.x1, (int)rect.y1, same);
                        }
                        pc_text_layout_fini(&wl);
                        pc_rendered_fini(&r);
                    }
                    pc_str_fini(&w.style);
                }
            }
            if (strcmp(k, "ren") == 0) {
                /* compare "fmt x0 y0 x1 y1 hash" */
                check_text(k, c.name, pc_buf_cstr(&got), rest);
            } else {
                check_text(k, c.name, pc_buf_cstr(&got), rest);
            }
            pc_buf_fini(&got);
        } else if (is_spell_kind(k)) {
            spell_line(&l, k, &small);
        } else {
            unit_line(&l, k);
        }
        free(l.buf);
    }
    case_fini(&c);
    pc_spell_free((pc_spell_dict *)(uintptr_t)small);
    pc_text_engine_free(eb);
    pc_text_engine_free(es);
    pc_buf_fini(&golden);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)setenv("PHOTOCRAFT_NO_SYSTEM_FONTS", "1", 1);
    RUN(test_golden);
    (void)printf("%d golden lines, %d skipped (fonts not installed), %d known differences\n", lines, skipped,
                 known);
    return pc_t_finish();
}
