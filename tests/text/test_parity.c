/* Parity with crates/text (chunk L13 section of tests/golden/text-vectors.txt, written by
 * `rustref text-vectors`, tools/rustref/src/text_vectors.rs). Every line is recomputed here and
 * its result text compared with the reference's; see that file for the line kinds. Lines that
 * need a font file this machine doesn't have are skipped (counted, not failed); system-font lines
 * run only when the scanned font files are exactly the ones the golden was made with. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/buf.h"
#include "base/file.h"
#include "harness.h"
#include "raster/surface.h"
#include "text/text.h"

#define GOLDEN "tests/golden/text-vectors.txt"
#define H0 0xcbf29ce484222325ull
#define MAXTOK 4096

static pc_buf golden;
static int skipped;
static char saved_gids[8192];
static int known; /* intentional differences (REWRITE_PLAN.md §7) */

/* ---- hashing and formatting like the reference ---- */

static void fnv(uint64_t *h, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    for (i = 0; i < n; i++) {
        *h ^= b[i];
        *h *= 0x100000001b3ull;
    }
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
static uint64_t f64bits(double f)
{
    uint64_t u;
    memcpy(&u, &f, 8);
    return u;
}
static void hf32(uint64_t *h, float v)
{
    hu32(h, f32bits(v));
}
static void hf64(uint64_t *h, double v)
{
    hu64(h, f64bits(v));
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

/* esc() of the reference: bytes <= ' ', '\\', '|' and >= 0x7f as \xNN; empty as \- */
static void esc(pc_buf *o, const char *s, size_t n)
{
    size_t i;
    if (n == 0) {
        pc_buf_append_str(o, "\\-", NULL);
        return;
    }
    for (i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if (b <= ' ' || b == '\\' || b == '|' || b >= 0x7f) {
            pc_buf_appendf(o, NULL, "\\x%02x", b);
        } else {
            pc_buf_append_byte(o, b, NULL);
        }
    }
}
static void escz(pc_buf *o, const char *s)
{
    esc(o, s, strlen(s));
}

/* Inverse of esc (into a NUL-terminated malloc'd string, *len set). */
static char *unesc(const char *t, size_t *len)
{
    size_t n = strlen(t), i, m = 0;
    char *o = malloc(n + 1);
    if (o == NULL) {
        return NULL;
    }
    if (strcmp(t, "\\-") == 0) {
        o[0] = '\0';
        *len = 0;
        return o;
    }
    for (i = 0; i < n; i++) {
        if (t[i] == '\\' && i + 3 < n + 1 && t[i + 1] == 'x') {
            char hx[3] = {t[i + 2], t[i + 3], 0};
            o[m++] = (char)strtoul(hx, NULL, 16);
            i += 3;
        } else {
            o[m++] = t[i];
        }
    }
    o[m] = '\0';
    *len = m;
    return o;
}

/* ---- golden lines ---- */

typedef struct line {
    char *buf;
    char *tok[MAXTOK];
    int n;
    int arrow;        /* index of "=>" or -1 */
    const char *want; /* text after "=> " (or "") */
} line;

static bool next_line(size_t *pos, line *l)
{
    const char *d = (const char *)golden.data;
    size_t s = *pos, e = s, i;
    char *p;
    if (s >= golden.len) {
        return false;
    }
    while (e < golden.len && d[e] != '\n') {
        e++;
    }
    *pos = e + 1;
    l->buf = malloc(2 * (e - s) + 2);
    memcpy(l->buf, d + s, e - s);
    l->buf[e - s] = '\0';
    /* want text */
    p = strstr(l->buf, " => ");
    l->want = "";
    {
        char *copy = l->buf + (e - s) + 1;
        if (p != NULL) {
            strcpy(copy, p + 4);
            l->want = copy;
        } else if (e - s >= 3 && strcmp(l->buf + (e - s) - 3, " =>") == 0) {
            copy[0] = '\0';
            l->want = copy;
        }
    }
    l->n = 0;
    l->arrow = -1;
    for (i = 0, p = strtok(l->buf, " "); p != NULL && l->n < MAXTOK; p = strtok(NULL, " "), i++) {
        if (strcmp(p, "=>") == 0 && l->arrow < 0) {
            l->arrow = l->n;
        }
        l->tok[l->n++] = p;
    }
    return true;
}

static void check_line(const line *l, const char *got, const char *what)
{
    pc_t_checks++;
    if (strcmp(got, l->want) != 0) {
        PC_T_FAIL("%s %s: got \"%.300s\" want \"%.300s\"", what, l->n > 1 ? l->tok[1] : "", got, l->want);
    }
}

/* ---- font files by spec ---- */

static pc_blob *make_ttc(const pc_blob *const *fonts, size_t n)
{
    size_t head = 12 + 4 * n, at = (head + 3) & ~(size_t)3, offs[8], i, total;
    uint8_t *out;
    pc_blob *b = NULL;
    for (i = 0; i < n; i++) {
        offs[i] = at;
        at += (pc_blob_len(fonts[i]) + 3) & ~(size_t)3;
    }
    total = at;
    out = calloc(1, total);
    memcpy(out, "ttcf", 4);
    out[4] = 0;
    out[5] = 1;
    out[11] = (uint8_t)n;
    for (i = 0; i < n; i++) {
        const uint8_t *src = pc_blob_data(fonts[i]);
        size_t len = pc_blob_len(fonts[i]), k, nt;
        uint8_t *dst = out + offs[i];
        out[12 + 4 * i] = (uint8_t)(offs[i] >> 24);
        out[13 + 4 * i] = (uint8_t)(offs[i] >> 16);
        out[14 + 4 * i] = (uint8_t)(offs[i] >> 8);
        out[15 + 4 * i] = (uint8_t)offs[i];
        memcpy(dst, src, len);
        nt = (size_t)((dst[4] << 8) | dst[5]);
        for (k = 0; k < nt; k++) {
            uint8_t *p = dst + 12 + 16 * k + 8;
            uint32_t o = ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) +
                         (uint32_t)offs[i];
            p[0] = (uint8_t)(o >> 24);
            p[1] = (uint8_t)(o >> 16);
            p[2] = (uint8_t)(o >> 8);
            p[3] = (uint8_t)o;
        }
    }
    pc_blob_new(out, total, &b, NULL);
    free(out);
    return b;
}

static pc_blob *bundled_blob(const char *name)
{
    size_t n, i;
    const pc_bundled_font *b = pc_text_bundled_fonts(&n);
    for (i = 0; i < n; i++) {
        if (strcmp(b[i].file, name) == 0) {
            pc_blob *o = NULL;
            pc_blob_new(b[i].data, b[i].len, &o, NULL);
            return o;
        }
    }
    return NULL;
}

/* Bytes for a spec (owned reference), NULL when unavailable. */
static pc_blob *spec_blob(const char *spec)
{
    if (strncmp(spec, "bundled:", 8) == 0) {
        return bundled_blob(spec + 8);
    }
    if (strcmp(spec, "ttc:inter+mono") == 0) {
        pc_blob *a = bundled_blob("Inter-Regular.ttf"), *b = bundled_blob("JetBrainsMono-Regular.ttf"), *t;
        const pc_blob *v[2] = {a, b};
        t = make_ttc(v, 2);
        pc_blob_release(a);
        pc_blob_release(b);
        return t;
    }
    {
        pc_buf data = PC_BUF_INIT;
        pc_blob *o = NULL;
        if (pc_file_read(spec, (size_t)1 << 28, &data, NULL) == PC_OK) {
            pc_blob_new(data.data, data.len, &o, NULL);
        }
        pc_buf_fini(&data);
        return o;
    }
}

typedef struct fcache {
    char spec[512];
    uint32_t index;
    pc_font *font;
} fcache;
static fcache fonts[64];
static size_t nfonts;

static pc_font *spec_font(const char *spec, uint32_t index)
{
    size_t i;
    pc_blob *b;
    pc_font *f = NULL;
    for (i = 0; i < nfonts; i++) {
        if (strcmp(fonts[i].spec, spec) == 0 && fonts[i].index == index) {
            return fonts[i].font;
        }
    }
    b = spec_blob(spec);
    if (b == NULL) {
        return NULL;
    }
    if (pc_font_new(b, index, &f, NULL) != PC_OK) {
        f = NULL;
    }
    pc_blob_release(b);
    if (f != NULL && nfonts < 64) {
        snprintf(fonts[nfonts].spec, sizeof fonts[nfonts].spec, "%s", spec);
        fonts[nfonts].index = index;
        fonts[nfonts].font = f;
        nfonts++;
    }
    return f;
}

/* ---- fontdb lines ---- */

static void fmt_faces(pc_fontdb *db, const char *fam, size_t flen, pc_buf *o)
{
    pc_face_info *fi = NULL;
    size_t n = 0, i, k;
    if (pc_fontdb_faces(db, fam, flen, &fi, &n, NULL) != PC_OK) {
        pc_buf_append_str(o, "ERR", NULL);
        return;
    }
    for (i = 0; i < n; i++) {
        pc_buf_appendf(o, NULL, "%s%08x:%d:%zu", i > 0 ? " " : "", f32bits(fi[i].weight),
                       fi[i].italic ? 1 : 0, fi[i].naxes);
        for (k = 0; k < fi[i].naxes; k++) {
            pc_buf_append_byte(o, ':', NULL);
            escz(o, fi[i].axes[k].tag);
            pc_buf_appendf(o, NULL, ":%08x:%08x:%08x", f32bits(fi[i].axes[k].min), f32bits(fi[i].axes[k].def),
                           f32bits(fi[i].axes[k].max));
        }
    }
    pc_face_infos_free(fi, n);
}

static void fmt_charmap(pc_fontdb *db, const char *fam, size_t flen, uint16_t w, bool it, pc_buf *o)
{
    uint32_t *cm = NULL;
    size_t n = 0, i;
    uint64_t h = H0;
    if (pc_fontdb_charmap(db, fam, flen, w, it, &cm, &n, NULL) != PC_OK) {
        pc_buf_append_str(o, "ERR", NULL);
        return;
    }
    for (i = 0; i < n; i++) {
        hu32(&h, cm[i]);
    }
    pc_buf_appendf(o, NULL, "%zu %x %x %016llx", n, n > 0 ? cm[0] : 0u, n > 0 ? cm[n - 1] : 0u,
                   (unsigned long long)h);
    free(cm);
}

static void fmt_resolved(const pc_resolved_font *r, pc_buf *o)
{
    escz(o, r->family);
    pc_buf_appendf(o, NULL, " %u %d %d", (unsigned)r->weight, r->italic ? 1 : 0, r->exact ? 1 : 0);
}

static void fmt_list(const pc_strlist *l, pc_buf *o)
{
    size_t i;
    for (i = 0; i < l->n; i++) {
        if (i > 0) {
            pc_buf_append_byte(o, ' ', NULL);
        }
        escz(o, l->v[i]);
    }
}

/* Hash of the sorted system font file list, like the reference's collect(). */
static void sys_files(size_t *count, uint64_t *hash)
{
    pc_strlist dirs = {NULL, 0}, files = {NULL, 0};
    size_t i, k;
    uint64_t h = H0;
    pc_text_system_font_dirs(&dirs, NULL);
    for (i = 0; i < dirs.n; i++) {
        pc_text_collect_font_files(dirs.v[i], &files, NULL);
    }
    for (i = 1; i < files.n; i++) { /* insertion sort, byte order */
        char *x = files.v[i];
        for (k = i; k > 0 && strcmp(files.v[k - 1], x) > 0; k--) {
            files.v[k] = files.v[k - 1];
        }
        files.v[k] = x;
    }
    for (i = 0; i < files.n; i++) {
        fnv(&h, files.v[i], strlen(files.v[i]));
        fnv(&h, "\n", 1);
    }
    *count = files.n;
    *hash = h;
    pc_strlist_fini(&dirs);
    pc_strlist_fini(&files);
}

/* ---- render helpers ---- */

typedef struct glayout {
    int id;
    bool ok;
    pc_text_layout l;
} glayout;

static glayout layouts[32];
static size_t nlayouts;

static glayout *find_layout(int id)
{
    size_t i;
    for (i = 0; i < nlayouts; i++) {
        if (layouts[i].id == id) {
            return &layouts[i];
        }
    }
    return NULL;
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

static int16_t *parse_coords(const char *t, size_t *n)
{
    int16_t *c = calloc(16, sizeof *c);
    const char *p = t;
    *n = 0;
    if (strcmp(t, "-") == 0) {
        return c;
    }
    while (*p != '\0' && *n < 16) {
        char *end;
        c[(*n)++] = (int16_t)strtol(p, &end, 10);
        p = *end == ',' ? end + 1 : end;
        if (end == p && *end != ',') {
            break;
        }
    }
    return c;
}

static bool parse_warp(const char *spec, const pc_text_layout *l, pc_style_warp *w)
{
    char name[64];
    const char *c = strchr(spec, ':');
    float b[4];
    double bd[4];
    pc_warp_style st;
    float v, hd, vd;
    bool hz;
    if (strcmp(spec, "-") == 0 || c == NULL || (size_t)(c - spec) >= sizeof name) {
        return false;
    }
    memcpy(name, spec, (size_t)(c - spec));
    name[c - spec] = '\0';
    v = f32of(c + 1);
    hd = f32of(c + 10);
    vd = f32of(c + 19);
    hz = c[28] == '1';
    /* warp.rs Warp::new: a preset whose PSD name is the style, over the layout bounds. */
    if (!pc_warp_style_parse(name, strlen(name), &st) || !pc_warp_style_is_preset(st) ||
        strcmp(pc_warp_style_psd_name(st), name) != 0 || !pc_text_layout_bounds(l, b)) {
        return false;
    }
    bd[0] = (double)b[0];
    bd[1] = (double)b[1];
    bd[2] = (double)b[2];
    bd[3] = (double)b[3];
    return pc_style_warp_init(w, st, (double)v, (double)hd, (double)vd, !hz, bd);
}

static bool fmt_of(const char *n, pc_pixel_format *f)
{
    if (strcmp(n, "rgba8") == 0) {
        *f = (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true};
    } else if (strcmp(n, "rgba16") == 0) {
        *f = (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, false};
    } else if (strcmp(n, "rgba32f") == 0) {
        *f = (pc_pixel_format){PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, true};
    } else if (strcmp(n, "graya8") == 0) {
        *f = (pc_pixel_format){PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, true};
    } else if (strcmp(n, "cmyka8") == 0) {
        *f = (pc_pixel_format){PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8, true};
    } else if (strcmp(n, "laba16") == 0) {
        *f = (pc_pixel_format){PC_COLOR_MODE_LAB, PC_SAMPLE_TYPE_U16, true};
    } else {
        return false;
    }
    return true;
}

/* ---- the replay ---- */

static void test_golden(void)
{
    size_t pos = 0;
    pc_fontdb *db0 = NULL, *cur = NULL, *reg = NULL, *sys = NULL;
    bool sys_ok = false, in_sys = false, reg_missing = false;
    line l;
    int lines = 0;
    CHECK(pc_fontdb_new(&db0, NULL) == PC_OK);
    cur = db0;
    while (next_line(&pos, &l)) {
        pc_buf got = PC_BUF_INIT;
        const char *k = l.n > 0 ? l.tok[0] : "";
        lines++;
        if (l.n > 1 && strcmp(k, "#") == 0 && strcmp(l.tok[1], "l13") != 0) {
            free(l.buf);
            break; /* later chunks' sections: tests/text/test_layout_parity.c (L14) */
        }
        if (l.n == 0 || strcmp(k, "#") == 0) {
            free(l.buf);
            continue;
        }
        if (strcmp(k, "families") == 0) {
            pc_strlist f = {NULL, 0};
            size_t i;
            pc_fontdb_families(db0, &f, NULL);
            fmt_list(&f, &got);
            pc_strlist_fini(&f);
            /* compare with everything after "families " */
            {
                pc_buf w = PC_BUF_INIT;
                for (i = 1; i < (size_t)l.n; i++) {
                    pc_buf_appendf(&w, NULL, "%s%s", i > 1 ? " " : "", l.tok[i]);
                }
                l.want = pc_buf_cstr(&w);
                check_line(&l, pc_buf_cstr(&got), "families");
                pc_buf_fini(&w);
            }
        } else if (strcmp(k, "faces") == 0 || strcmp(k, "charmap") == 0) {
            size_t flen;
            char *fam = unesc(l.tok[1], &flen);
            pc_fontdb *d = in_sys ? sys : cur;
            if ((in_sys && !sys_ok) || (cur == reg && reg_missing)) {
                skipped++;
            } else if (k[0] == 'f') {
                fmt_faces(d, fam, flen, &got);
                check_line(&l, pc_buf_cstr(&got), "faces");
            } else {
                fmt_charmap(d, fam, flen, (uint16_t)atoi(l.tok[2]), atoi(l.tok[3]) != 0, &got);
                check_line(&l, pc_buf_cstr(&got), "charmap");
            }
            free(fam);
        } else if (strcmp(k, "guess") == 0 || strcmp(k, "resolve") == 0 || strcmp(k, "sysresolve") == 0 ||
                   strcmp(k, "resolve+ttc") == 0) {
            size_t plen;
            char *ps = unesc(l.tok[1], &plen);
            pc_resolved_font r = {NULL, 0, false, false};
            pc_status st;
            pc_fontdb *d = k[0] == 's' ? sys : strcmp(k, "resolve+ttc") == 0 ? reg : db0;
            if (k[0] == 's' && !sys_ok) {
                skipped++;
                free(ps);
                free(l.buf);
                pc_buf_fini(&got);
                continue;
            }
            if (strcmp(k, "resolve+ttc") == 0) {
                pc_blob *a = bundled_blob("Inter-Medium.ttf"), *b = bundled_blob("JetBrainsMono-Regular.ttf"),
                        *t;
                const pc_blob *v[2] = {a, b};
                t = make_ttc(v, 2);
                pc_fontdb_free(reg);
                pc_fontdb_new(&reg, NULL);
                pc_fontdb_register_font_data(reg, pc_blob_data(t), pc_blob_len(t), NULL, NULL);
                pc_blob_release(a);
                pc_blob_release(b);
                pc_blob_release(t);
                d = reg;
            }
            st = k[0] == 'g' ? pc_text_guess_from_postscript(ps, plen, &r, NULL)
                             : pc_fontdb_resolve_postscript(d, ps, plen, &r, NULL);
            if (st == PC_OK) {
                fmt_resolved(&r, &got);
            }
            check_line(&l, pc_buf_cstr(&got), k);
            pc_resolved_font_fini(&r);
            free(ps);
        } else if (strcmp(k, "facecount") == 0) {
            const char *spec = l.tok[1];
            if (strncmp(spec, "hex:", 4) == 0) {
                uint8_t buf[64];
                size_t n = 0, i;
                pc_strlist names = {NULL, 0};
                pc_fontdb *d = NULL;
                for (i = 4; spec[i] != '\0' && spec[i + 1] != '\0' && n < sizeof buf; i += 2) {
                    char hx[3] = {spec[i], spec[i + 1], 0};
                    buf[n++] = (uint8_t)strtoul(hx, NULL, 16);
                }
                if (strcmp(spec, "hex:-") == 0) {
                    n = 0;
                }
                pc_fontdb_new(&d, NULL);
                pc_fontdb_register_font_data(d, buf, n, &names, NULL);
                pc_buf_appendf(&got, NULL, "%u %zu", (unsigned)pc_text_face_count(buf, n), names.n);
                pc_strlist_fini(&names);
                pc_fontdb_free(d);
                check_line(&l, pc_buf_cstr(&got), "facecount");
            } else {
                pc_blob *b = spec_blob(spec);
                if (b == NULL) {
                    skipped++;
                } else {
                    pc_buf_appendf(&got, NULL, "%u",
                                   (unsigned)pc_text_face_count(pc_blob_data(b), pc_blob_len(b)));
                    check_line(&l, pc_buf_cstr(&got), "facecount");
                    pc_blob_release(b);
                }
            }
        } else if (strcmp(k, "register") == 0) {
            pc_blob *b = spec_blob(l.tok[1]);
            pc_strlist names = {NULL, 0};
            pc_fontdb_free(reg);
            reg = NULL;
            pc_fontdb_new(&reg, NULL);
            cur = reg;
            reg_missing = b == NULL;
            if (b == NULL) {
                skipped++;
            } else {
                pc_fontdb_register_font_data(reg, pc_blob_data(b), pc_blob_len(b), &names, NULL);
                fmt_list(&names, &got);
                check_line(&l, pc_buf_cstr(&got), "register");
                pc_strlist_fini(&names);
                pc_blob_release(b);
            }
        } else if (strcmp(k, "families+") == 0 && reg_missing) {
            skipped++;
            cur = db0;
        } else if (strcmp(k, "families+") == 0) {
            pc_strlist f = {NULL, 0};
            pc_fontdb_families(reg, &f, NULL);
            fmt_list(&f, &got);
            check_line(&l, pc_buf_cstr(&got), "families+");
            pc_strlist_fini(&f);
            cur = db0;
        } else if (strcmp(k, "hidden") == 0) {
            pc_buf_appendf(&got, NULL, "%d %d", pc_text_is_hidden_family(".Hiragino", 9) ? 1 : 0,
                           pc_text_is_hidden_family("Hiragino", 8) ? 1 : 0);
            check_line(&l, pc_buf_cstr(&got), "hidden");
        } else if (strcmp(k, "sysfiles") == 0) {
            size_t n;
            uint64_t h;
            char want[64];
            sys_files(&n, &h);
            snprintf(want, sizeof want, "%zu %016llx", n, (unsigned long long)h);
            sys_ok = strcmp(l.tok[1], "-") != 0 && (size_t)atoi(l.tok[1]) == n &&
                     strcmp(l.tok[2], want + strcspn(want, " ") + 1) == 0;
            in_sys = true;
            if (sys_ok) {
                CHECK(pc_fontdb_with_system_fonts(&sys, NULL) == PC_OK);
            } else {
                (void)printf("system fonts differ from the golden's machine: system-font lines skipped\n");
            }
        } else if (strcmp(k, "sysfamilies") == 0) {
            if (!sys_ok) {
                skipped++;
            } else {
                pc_strlist f = {NULL, 0};
                pc_buf w = PC_BUF_INIT;
                int i;
                pc_fontdb_families(sys, &f, NULL);
                fmt_list(&f, &got);
                for (i = 1; i < l.n; i++) {
                    pc_buf_appendf(&w, NULL, "%s%s", i > 1 ? " " : "", l.tok[i]);
                }
                l.want = pc_buf_cstr(&w);
                check_line(&l, pc_buf_cstr(&got), "sysfamilies");
                pc_buf_fini(&w);
                pc_strlist_fini(&f);
            }
        } else if (strcmp(k, "order") == 0 || strcmp(k, "fallback") == 0) {
            size_t ll;
            char *loc = unesc(l.tok[1], &ll);
            pc_cjk_script o[4];
            static const char *const NM[4] = {"ja", "sc", "tc", "ko"};
            pc_cjk_script_order(ll == 0 ? NULL : loc, o);
            if (k[0] == 'o') {
                pc_buf_appendf(&got, NULL, "%s %s %s %s", NM[o[0]], NM[o[1]], NM[o[2]], NM[o[3]]);
            } else {
                const char **c = NULL;
                size_t n = 0, i;
                pc_text_fallback_candidates(o, &c, &n, NULL);
                for (i = 0; i < n; i++) {
                    if (i > 0) {
                        pc_buf_append_byte(&got, ' ', NULL);
                    }
                    escz(&got, c[i]);
                }
                free(c);
            }
            check_line(&l, pc_buf_cstr(&got), k);
            free(loc);
        } else if (strcmp(k, "cjkfamilies") == 0 || strcmp(k, "cjkfiles") == 0 ||
                   strcmp(k, "lastresort") == 0) {
            pc_cjk_script s = strcmp(l.tok[1], "ja") == 0   ? PC_CJK_JAPANESE
                              : strcmp(l.tok[1], "sc") == 0 ? PC_CJK_SIMPLIFIED_CHINESE
                              : strcmp(l.tok[1], "tc") == 0 ? PC_CJK_TRADITIONAL_CHINESE
                                                            : PC_CJK_KOREAN;
            if (strcmp(k, "cjkfamilies") == 0) {
                const char *const *f = pc_cjk_families(s);
                size_t i;
                for (i = 0; f[i] != NULL; i++) {
                    if (i > 0) {
                        pc_buf_append_byte(&got, ' ', NULL);
                    }
                    escz(&got, f[i]);
                }
            } else {
                pc_cjk_font_file *f = NULL;
                size_t n = 0, i;
                if (strcmp(k, "lastresort") == 0) {
                    pc_cjk_last_resort_files(&f, &n, NULL);
                } else {
                    pc_cjk_font_files(s, &f, &n, NULL);
                }
                for (i = 0; i < n; i++) {
                    if (i > 0) {
                        pc_buf_append_byte(&got, ' ', NULL);
                    }
                    escz(&got, f[i].path);
                    pc_buf_append_byte(&got, '#', NULL);
                    escz(&got, f[i].family);
                }
                pc_cjk_font_files_free(f, n);
            }
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "classify") == 0) {
            uint64_t h = H0;
            uint32_t u;
            for (u = 0; u < 0x40000; u++) {
                uint8_t v;
                if (u >= 0xD800 && u <= 0xDFFF) {
                    continue;
                }
                v = (uint8_t)pc_cjk_classify(u);
                fnv(&h, &v, 1);
            }
            pc_buf_appendf(&got, NULL, "%016llx", (unsigned long long)h);
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "faceindex") == 0) {
            size_t fl;
            char *fam = unesc(l.tok[1], &fl);
            pc_blob *t = spec_blob("ttc:inter+mono"), *m = bundled_blob("JetBrainsMono-Regular.ttf");
            pc_buf_appendf(&got, NULL, "%u %u",
                           (unsigned)pc_cjk_face_index_for_family(pc_blob_data(t), pc_blob_len(t), fam),
                           (unsigned)pc_cjk_face_index_for_family(pc_blob_data(m), pc_blob_len(m), fam));
            check_line(&l, pc_buf_cstr(&got), k);
            pc_blob_release(t);
            pc_blob_release(m);
            free(fam);
        } else if (strcmp(k, "serif") == 0) {
            size_t fl;
            char *fam = unesc(l.tok[1], &fl);
            pc_buf_appendf(&got, NULL, "%d", pc_craft_is_serif_family(fam, fl) ? 1 : 0);
            check_line(&l, pc_buf_cstr(&got), k);
            free(fam);
        } else if (strcmp(k, "craftjp") == 0) {
            const char **f = NULL;
            size_t n = 0, ncf;
            const pc_craft_font *cf = pc_craft_fonts(&ncf);
            pc_craft_japanese_families(cf, ncf, &f, &n, NULL);
            pc_buf_appendf(&got, NULL, "%zu", n);
            free(f);
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "category") == 0) {
            size_t cl;
            char *cat = unesc(l.tok[1], &cl);
            uint64_t h = H0;
            uint32_t u, n = 0;
            for (u = 0; u < 0x20000; u++) {
                if (u >= 0xD800 && u <= 0xDFFF) {
                    continue;
                }
                if (pc_glyph_in_category(u, cat)) {
                    hu32(&h, u);
                    n++;
                }
            }
            pc_buf_appendf(&got, NULL, "%u %016llx", n, (unsigned long long)h);
            check_line(&l, pc_buf_cstr(&got), k);
            free(cat);
        } else if (strcmp(k, "sizeadj") == 0) {
            pc_buf_appendf(&got, NULL, "%08x", f32bits(pc_optical_size_adjust(f32of(l.tok[1]))));
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "kernff") == 0) {
            float f[2] = {f32of(l.tok[1]), f32of(l.tok[2])}, r[2] = {f32of(l.tok[3]), f32of(l.tok[4])};
            pc_buf_appendf(&got, NULL, "%08x", f32bits(pc_optical_kern_from_features(f, r)));
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "sampley") == 0) {
            pc_buf_appendf(&got, NULL, "%08x", f32bits(pc_optical_sample_y((size_t)atoi(l.tok[1]))));
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "pairf") == 0) {
            pc_optical_profile p[2];
            float f[2];
            int t = 1, s, i;
            memset(p, 0, sizeof p);
            for (s = 0; s < 2; s++) {
                for (i = 0; i < PC_OPTICAL_SAMPLES; i++, t++) {
                    p[s].has_left[i] = strcmp(l.tok[t], "-") != 0;
                    p[s].left[i] = p[s].has_left[i] ? f32of(l.tok[t]) : 0.0f;
                }
                for (i = 0; i < PC_OPTICAL_SAMPLES; i++, t++) {
                    p[s].has_right[i] = strcmp(l.tok[t], "-") != 0;
                    p[s].right[i] = p[s].has_right[i] ? f32of(l.tok[t]) : 0.0f;
                }
            }
            if (pc_optical_pair_features(&p[0], &p[1], f)) {
                pc_buf_appendf(&got, NULL, "%08x %08x", f32bits(f[0]), f32bits(f[1]));
            } else {
                pc_buf_append_str(&got, "none", NULL);
            }
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "cov") == 0) {
            size_t w = (size_t)atoi(l.tok[1]), h = (size_t)atoi(l.tok[2]), i, n = w * h;
            pc_coverage c;
            float *v = malloc(n * sizeof *v);
            uint64_t hh = H0;
            int t;
            pc_coverage_init(&c, w, h, NULL);
            for (t = 3; t + 3 < l.arrow; t += 4) {
                pc_coverage_line(&c, f64of(l.tok[t]), f64of(l.tok[t + 1]), f64of(l.tok[t + 2]),
                                 f64of(l.tok[t + 3]));
            }
            pc_coverage_finish(&c, v);
            for (i = 0; i < n; i++) {
                hf32(&hh, v[i]);
            }
            pc_buf_appendf(&got, NULL, "%016llx", (unsigned long long)hh);
            check_line(&l, pc_buf_cstr(&got), k);
            pc_coverage_fini(&c);
            free(v);
        } else if (strcmp(k, "bounds") == 0) {
            pc_bounds b = {false, {0, 0, 0, 0}};
            pc_line_sink s = pc_bounds_sink(&b);
            pc_xform x = {{0.5, 0.25, -1.0, 2.0, 3.0, -4.0}};
            pc_rect_to(&s, &x, 1.0, 2.0, 5.0, 9.5);
            pc_bounds_add(&b, NAN, 1.0);
            pc_buf_appendf(&got, NULL, "%016llx %016llx %016llx %016llx", (unsigned long long)f64bits(b.r[0]),
                           (unsigned long long)f64bits(b.r[1]), (unsigned long long)f64bits(b.r[2]),
                           (unsigned long long)f64bits(b.r[3]));
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "xmul") == 0) {
            pc_xform a = {{1.5, -0.5, 0.25, 2.0, 10.0, -3.0}}, b = {{0.0, 1.0, -1.0, 0.0, 4.0, 5.0}}, m;
            int i;
            m = pc_xform_mul(&a, &b);
            for (i = 0; i < 6; i++) {
                pc_buf_appendf(&got, NULL, "%s%016llx", i > 0 ? " " : "",
                               (unsigned long long)f64bits(m.m[i]));
            }
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "xscale") == 0) {
            pc_xform a = {{3.0, 4.0, 0.5, -0.5, 1.0, 1.0}};
            pc_buf_appendf(&got, NULL, "%016llx", (unsigned long long)f64bits(pc_xform_max_scale(&a)));
            check_line(&l, pc_buf_cstr(&got), k);
        } else if (strcmp(k, "outg") == 0 || strcmp(k, "outl") == 0) {
            uint32_t index = (uint32_t)atoi(l.tok[2]);
            pc_font *f = spec_font(l.tok[1], index);
            if (k[3] == 'l' && f32of(l.tok[4]) * 64.0f < 32.0f) {
                /* Below half a pixel per em FreeType refuses the size and font.c scales the
                 * font-unit outline itself: composite components and CFF outlines then differ
                 * from skrifa by up to 1/64 px (REWRITE_PLAN.md §7). Checked loosely in
                 * test_fonts.c instead. */
                known++;
            } else if (f == NULL) {
                skipped++;
            } else {
                size_t nc;
                int16_t *coords = parse_coords(l.tok[3], &nc);
                pc_text_layout lay;
                pc_char_style st;
                pc_placed_glyph g = {0, 0, 0.0f, 0.0f, 0, PC_GLYPH_HORIZONTAL};
                uint32_t gid, g0 = 0, g1 = 700, nonempty = 0, n = 0;
                uint64_t h = H0;
                float size = k[3] == 'g' ? 37.5f : f32of(l.tok[4]);
                if (k[3] == 'g') {
                    g0 = (uint32_t)atoi(l.tok[4]);
                    g1 = g0 + 1;
                }
                pc_text_layout_init(&lay);
                pc_char_style_default(&st);
                lay.styles = &st;
                lay.nstyles = 1;
                lay.glyphs = &g;
                lay.nglyphs = 1;
                lay.px_per_pt = 1.0f;
                pc_text_layout_add_face(&lay, f, coords, nc, size, false, 0.0f, NULL, NULL);
                for (gid = g0; gid < g1; gid++) {
                    pc_glyph_outline *o = NULL;
                    size_t no = 0;
                    pc_affine id = PC_AFFINE_IDENTITY;
                    g.id = gid;
                    pc_text_outlines(&lay, &id, NULL, &o, &no, NULL);
                    if (k[3] == 'g') {
                        uint64_t hg = H0;
                        if (no > 0) {
                            hash_els(&hg, o[0].els, o[0].n);
                        }
                        pc_buf_appendf(&got, NULL, "%zu %016llx", no > 0 ? o[0].n : 0,
                                       (unsigned long long)hg);
                    } else {
                        hu32(&h, gid);
                        hu32(&h, no > 0 ? (uint32_t)o[0].n : 0);
                        if (no > 0) {
                            hash_els(&h, o[0].els, o[0].n);
                            nonempty++;
                        }
                        n++;
                    }
                    pc_glyph_outlines_free(o, no);
                }
                if (k[3] == 'l') {
                    pc_buf_appendf(&got, NULL, "%u %u %016llx", n, nonempty, (unsigned long long)h);
                }
                check_line(&l, pc_buf_cstr(&got), k);
                lay.styles = NULL;
                lay.nstyles = 0;
                lay.glyphs = NULL;
                lay.nglyphs = 0;
                pc_text_layout_fini(&lay);
                free(coords);
            }
        } else if (strcmp(k, "opticalgids") == 0) {
            /* The glyph ids the reference's layout gave SAMPLE's characters, for the optical line
             * that follows (variable fonts print ids once per coordinate set, optical once). */
            snprintf(saved_gids, sizeof saved_gids, "%s", l.want);
        } else if (strcmp(k, "optical") == 0) {
            pc_font *f = spec_font(l.tok[1], (uint32_t)atoi(l.tok[2]));
            if (f == NULL) {
                skipped++;
            } else {
                uint32_t gids[256];
                bool have[256];
                size_t ng = 0, i;
                char *p = strtok(saved_gids, " ");
                pc_optical_cache *c = NULL;
                while (p != NULL && ng < 256) {
                    have[ng] = strcmp(p, "-") != 0;
                    gids[ng] = have[ng] ? (uint32_t)atoi(p) : 0;
                    ng++;
                    p = strtok(NULL, " ");
                }
                pc_optical_cache_new(&c, NULL);
                for (i = 0; i + 1 < ng; i++) {
                    float v;
                    if (!have[i] || !have[i + 1]) {
                        pc_buf_append_str(&got, i > 0 ? " nochar" : "nochar", NULL);
                    } else if (pc_optical_cache_pair(c, f, NULL, 0, gids[i], gids[i + 1], &v)) {
                        pc_buf_appendf(&got, NULL, "%s%08x", i > 0 ? " " : "", f32bits(v));
                    } else {
                        pc_buf_append_str(&got, i > 0 ? " none" : "none", NULL);
                    }
                }
                check_line(&l, pc_buf_cstr(&got), "optical");
                pc_optical_cache_free(c);
            }
        } else if (strcmp(k, "opticalgrid") == 0) {
            pc_font *f = spec_font(l.tok[1], (uint32_t)atoi(l.tok[2]));
            if (f == NULL) {
                skipped++;
            } else {
                pc_optical_cache *c = NULL;
                uint64_t h = H0;
                uint32_t a, b;
                pc_optical_cache_new(&c, NULL);
                for (a = 0; a < 48; a++) {
                    for (b = 0; b < 48; b++) {
                        float v;
                        if (pc_optical_cache_pair(c, f, NULL, 0, a, b, &v)) {
                            hf32(&h, v);
                        } else {
                            fnv(&h, "-", 1);
                        }
                    }
                }
                pc_optical_cache_free(c);
                pc_buf_appendf(&got, NULL, "%016llx", (unsigned long long)h);
                check_line(&l, pc_buf_cstr(&got), k);
            }
        } else if (strcmp(k, "L") == 0) {
            glayout *g = &layouts[nlayouts++];
            g->id = atoi(l.tok[1]);
            g->ok = true;
            pc_text_layout_init(&g->l);
            g->l.px_per_pt = f32of(l.tok[2]);
            g->l.vertical = atoi(l.tok[3]) != 0;
        } else if (strcmp(k, "LF") == 0) {
            glayout *g = find_layout(atoi(l.tok[1]));
            pc_font *f = spec_font(l.tok[2], (uint32_t)atoi(l.tok[3]));
            if (f == NULL) {
                g->ok = false;
            } else {
                size_t nc;
                int16_t *c = parse_coords(l.tok[7], &nc);
                pc_text_layout_add_face(&g->l, f, c, nc, f32of(l.tok[4]), atoi(l.tok[5]) != 0,
                                        f32of(l.tok[6]), NULL, NULL);
                free(c);
            }
        } else if (strcmp(k, "LG") == 0) {
            glayout *g = find_layout(atoi(l.tok[1]));
            pc_placed_glyph *v = realloc(g->l.glyphs, (g->l.nglyphs + 1) * sizeof *v);
            g->l.glyphs = v;
            v[g->l.nglyphs++] = (pc_placed_glyph){(uint32_t)atoi(l.tok[2]), (uint32_t)atoi(l.tok[3]),
                                                  f32of(l.tok[4]),          f32of(l.tok[5]),
                                                  (uint32_t)atoi(l.tok[6]), (pc_glyph_orient)atoi(l.tok[7])};
        } else if (strcmp(k, "LD") == 0) {
            glayout *g = find_layout(atoi(l.tok[1]));
            pc_decoration_rect *v = realloc(g->l.decorations, (g->l.ndecorations + 1) * sizeof *v);
            g->l.decorations = v;
            v[g->l.ndecorations++] = (pc_decoration_rect){f32of(l.tok[2]), f32of(l.tok[3]), f32of(l.tok[4]),
                                                          f32of(l.tok[5]), (uint32_t)atoi(l.tok[6])};
        } else if (strcmp(k, "LS") == 0) {
            glayout *g = find_layout(atoi(l.tok[1]));
            pc_char_style *v = realloc(g->l.styles, (g->l.nstyles + 1) * sizeof *v);
            static const pc_color_mode MODES[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE,
                                                   PC_COLOR_MODE_CMYK, PC_COLOR_MODE_LAB};
            pc_char_style *s;
            g->l.styles = v;
            s = &v[g->l.nstyles++];
            pc_char_style_default(s);
            s->color.mode = MODES[atoi(l.tok[2]) & 3];
            s->color.c[0] = f32of(l.tok[3]);
            s->color.c[1] = f32of(l.tok[4]);
            s->color.c[2] = f32of(l.tok[5]);
            s->color.c[3] = f32of(l.tok[6]);
            s->color.alpha = f32of(l.tok[7]);
            s->horizontal_scale = f32of(l.tok[8]);
            s->vertical_scale = f32of(l.tok[9]);
            s->baseline_shift_pt = f32of(l.tok[10]);
            s->faux_bold = atoi(l.tok[11]) != 0;
            s->faux_italic = atoi(l.tok[12]) != 0;
        } else if (strcmp(k, "LL") == 0) {
            glayout *g = find_layout(atoi(l.tok[1]));
            pc_line_info *v = realloc(g->l.lines, (g->l.nlines + 1) * sizeof *v);
            g->l.lines = v;
            v[g->l.nlines++] = (pc_line_info){
                0, 0, f32of(l.tok[4]), f32of(l.tok[2]), f32of(l.tok[3]), f32of(l.tok[5]), f32of(l.tok[6]), 0};
        } else if (strcmp(k, "ink") == 0 || strcmp(k, "rast") == 0) {
            glayout *g = find_layout(atoi(l.tok[1]));
            if (g == NULL || !g->ok) {
                skipped++;
            } else {
                pc_affine t;
                pc_style_warp w;
                bool hw;
                int i;
                for (i = 0; i < 6; i++) {
                    t.m[i] = f64of(l.tok[2 + i]);
                }
                hw = parse_warp(l.tok[8], &g->l, &w);
                if (k[0] == 'i') {
                    pc_rect r = PC_RECT_EMPTY;
                    pc_glyph_outline *o = NULL;
                    size_t no = 0, j;
                    uint64_t h = H0;
                    pc_text_ink_rect(&g->l, &t, hw ? &w : NULL, &r, NULL);
                    pc_text_outlines(&g->l, &t, hw ? &w : NULL, &o, &no, NULL);
                    for (j = 0; j < no; j++) {
                        hu32(&h, (uint32_t)o[j].n);
                        hash_els(&h, o[j].els, o[j].n);
                    }
                    pc_glyph_outlines_free(o, no);
                    pc_buf_appendf(&got, NULL, "%d %d %d %d %zu %016llx", (int)r.x0, (int)r.y0, (int)r.x1,
                                   (int)r.y1, no, (unsigned long long)h);
                } else {
                    pc_pixel_format f;
                    pc_rendered r;
                    pc_antialias aa = atoi(l.tok[10]) != 0 ? PC_AA_NONE : PC_AA_SMOOTH;
                    fmt_of(l.tok[9], &f);
                    if (pc_text_rasterize(&g->l, &t, f, aa, hw ? &w : NULL, &r, NULL) == PC_OK) {
                        uint64_t h = H0;
                        size_t n = 0, j;
                        if (!pc_rect_is_empty(r.rect)) {
                            float *px = NULL;
                            pc_surface_read_region(&r.surface, r.rect, &px, &n, NULL);
                            for (j = 0; j < n; j++) {
                                hf32(&h, px[j]);
                            }
                            free(px);
                        }
                        pc_buf_appendf(&got, NULL, "%d %d %d %d %zu %016llx", (int)r.rect.x0, (int)r.rect.y0,
                                       (int)r.rect.x1, (int)r.rect.y1, n, (unsigned long long)h);
                        pc_rendered_fini(&r);
                    }
                }
                check_line(&l, pc_buf_cstr(&got), k);
            }
        } else if (strcmp(k, "prev") == 0) {
            glayout *g = find_layout(atoi(l.tok[1]));
            if (g == NULL || !g->ok) {
                skipped++;
            } else {
                uint32_t px = 0;
                uint8_t *a = NULL;
                uint64_t h = H0, sum = 0;
                size_t i;
                if (pc_glyph_preview_from_layout(&g->l, (uint32_t)atoi(l.tok[2]), &px, &a, NULL) == PC_OK) {
                    fnv(&h, a, (size_t)px * px);
                    for (i = 0; i < (size_t)px * px; i++) {
                        sum += a[i];
                    }
                    pc_buf_appendf(&got, NULL, "%u %llu %016llx", px, (unsigned long long)sum,
                                   (unsigned long long)h);
                    free(a);
                }
                check_line(&l, pc_buf_cstr(&got), k);
            }
        } else if (strcmp(k, "noface") == 0 || strcmp(k, "nowarp") == 0 || strcmp(k, "noprev") == 0) {
            /* informational */
        } else {
            PC_T_FAIL("unknown golden line kind %s", k);
        }
        pc_buf_fini(&got);
        free(l.buf);
    }
    (void)printf("%d golden lines, %d skipped (fonts not installed), %d known differences\n", lines, skipped,
                 known);
    pc_fontdb_free(db0);
    pc_fontdb_free(reg);
    pc_fontdb_free(sys);
}

int main(int argc, char **argv)
{
    size_t i;
    if (pc_file_read(GOLDEN, (size_t)1 << 24, &golden, NULL) != PC_OK) {
        (void)printf("%s missing\n", GOLDEN);
        return 1;
    }
    RUN(test_golden);
    for (i = 0; i < nlayouts; i++) {
        pc_text_layout_fini(&layouts[i].l);
    }
    for (i = 0; i < nfonts; i++) {
        pc_font_release(fonts[i].font);
    }
    pc_buf_fini(&golden);
    return pc_t_finish();
}
