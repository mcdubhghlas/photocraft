/* Parity with crates/text's PSD type-layer code (tests/golden/text-psd-vectors.txt, written by
 * `rustref text-psd-vectors`, tools/rustref/src/text_psd_vectors.rs; that file documents the
 * line kinds). Every line's result is recomputed here and compared as text. Corpus lines are
 * skipped (counted) when corpus/ has not been fetched. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "doc/text_styles.h"
#include "doc/json.h"
#include "harness.h"
#include "psd/file.h"
#include "text/text.h"

#define GOLDEN "tests/golden/text-psd-vectors.txt"

static int skipped;
static int shown;

/* ---- formatting like the reference ---- */

static uint64_t fnv64(const void *data, size_t n)
{
    const uint8_t *b = data;
    uint64_t h = 0xcbf29ce484222325ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void esc(pc_buf *o, const void *data, size_t n)
{
    const uint8_t *b = data;
    size_t i;
    if (n == 0) {
        pc_buf_append_str(o, "\\-", NULL);
        return;
    }
    for (i = 0; i < n; i++) {
        if (b[i] <= ' ' || b[i] >= 0x7f || (b[i] != 0 && strchr("\\|,:[]{}\"", b[i]) != NULL)) {
            pc_buf_appendf(o, NULL, "\\x%02x", b[i]);
        } else {
            pc_buf_append_byte(o, b[i], NULL);
        }
    }
}

/* Replaces o with "#hash/len" when longer than 300 bytes. */
static void shorten(pc_buf *o, size_t from)
{
    size_t n = o->len - from;
    if (n > 300) {
        uint64_t h = fnv64(o->data + from, n);
        pc_buf_truncate(o, from);
        pc_buf_appendf(o, NULL, "#%016" PRIx64 "/%zu", h, n);
    }
}

static uint8_t *unhex(const char *s, size_t n, size_t *len)
{
    uint8_t *b = malloc(n / 2 + 1);
    size_t i;
    *len = 0;
    if (b == NULL) {
        return NULL;
    }
    if (n == 1 && s[0] == '-') {
        return b;
    }
    for (i = 0; i + 1 < n; i += 2) {
        unsigned v;
        char t[3] = {s[i], s[i + 1], 0};
        v = (unsigned)strtoul(t, NULL, 16);
        b[(*len)++] = (uint8_t)v;
    }
    return b;
}

static float f32of(const char *hex)
{
    uint32_t u = (uint32_t)strtoul(hex, NULL, 16);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void dump(pc_buf *o, const pc_ed_value *v)
{
    size_t k;
    switch (v->kind) {
    case PC_ED_DICT:
        pc_buf_append_byte(o, '{', NULL);
        for (k = 0; k < v->n; k++) {
            if (k > 0) {
                pc_buf_append_byte(o, ',', NULL);
            }
            esc(o, pc_str_c(&v->entries[k].key), v->entries[k].key.len);
            pc_buf_append_byte(o, ':', NULL);
            dump(o, &v->entries[k].value);
        }
        pc_buf_append_byte(o, '}', NULL);
        break;
    case PC_ED_ARRAY:
        pc_buf_append_byte(o, '[', NULL);
        for (k = 0; k < v->n; k++) {
            if (k > 0) {
                pc_buf_append_byte(o, ',', NULL);
            }
            dump(o, &v->items[k]);
        }
        pc_buf_append_byte(o, ']', NULL);
        break;
    case PC_ED_STRING:
        pc_buf_append_byte(o, 's', NULL);
        esc(o, pc_str_c(&v->s), v->s.len);
        break;
    case PC_ED_INT:
        pc_buf_appendf(o, NULL, "i%" PRId64, v->i);
        break;
    case PC_ED_REAL: {
        uint64_t u;
        memcpy(&u, &v->r, 8);
        pc_buf_appendf(o, NULL, "r%016" PRIx64, u);
        break;
    }
    case PC_ED_BOOL:
        pc_buf_append_byte(o, v->b ? 'T' : 'F', NULL);
        break;
    case PC_ED_NAME:
    default:
        pc_buf_append_byte(o, 'n', NULL);
        esc(o, pc_str_c(&v->s), v->s.len);
        break;
    }
}

static void ed_result(pc_buf *o, const uint8_t *in, size_t n)
{
    pc_ed_value v, back;
    pc_error err;
    pc_buf w = PC_BUF_INIT, w2 = PC_BUF_INIT;
    size_t from = o->len;
    bool rt = false, stable = false;
    if (pc_ed_parse(in, n, &v, &err) != PC_OK) {
        pc_buf_append_str(o, "E ", NULL);
        esc(o, err.msg, strlen(err.msg));
        return;
    }
    dump(o, &v);
    shorten(o, from);
    CHECK(pc_ed_write(&v, &w, NULL) == PC_OK);
    if (pc_ed_parse(w.data, w.len, &back, NULL) == PC_OK) {
        rt = pc_ed_eq(&back, &v);
        stable =
            pc_ed_write(&back, &w2, NULL) == PC_OK && w2.len == w.len && memcmp(w2.data, w.data, w.len) == 0;
        pc_ed_fini(&back);
    }
    pc_buf_appendf(o, NULL, " | w%zu %016" PRIx64 " %d%d", w.len, fnv64(w.data, w.len), rt ? 1 : 0,
                   stable ? 1 : 0);
    pc_buf_fini(&w);
    pc_buf_fini(&w2);
    pc_ed_fini(&v);
}

/* ---- JSON (serde forms through doc/json.h) ---- */

static void js_write(pc_buf *o, yyjson_mut_doc *d, yyjson_mut_val *v)
{
    if (v == NULL) {
        v = yyjson_mut_null(d);
    }
    CHECK(pc_json_write(v, 0, o, NULL) == PC_OK);
}

#define JS(o, T, ptr)                                                                                        \
    do {                                                                                                     \
        yyjson_mut_doc *d_ = yyjson_mut_doc_new(NULL);                                                       \
        js_write((o), d_, T##_to_json(d_, (ptr), 0));                                                        \
        yyjson_mut_doc_free(d_);                                                                             \
    } while (0)

static void js_str(pc_buf *o, const pc_str *s)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    js_write(o, d, yyjson_mut_strncpy(d, pc_str_c(s), s->len));
    yyjson_mut_doc_free(d);
}

static void js_runs(pc_buf *o, const pc_text_run *runs, size_t n)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    size_t k;
    for (k = 0; k < n; k++) {
        yyjson_mut_arr_append(a, pc_text_run_to_json(d, &runs[k], 0));
    }
    js_write(o, d, a);
    yyjson_mut_doc_free(d);
}

static void js_paras(pc_buf *o, const pc_paragraph_run *runs, size_t n)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *a = yyjson_mut_arr(d);
    size_t k;
    for (k = 0; k < n; k++) {
        yyjson_mut_arr_append(a, pc_paragraph_run_to_json(d, &runs[k], 0));
    }
    js_write(o, d, a);
    yyjson_mut_doc_free(d);
}

static void ldump(pc_buf *o, const pc_text_layer *t)
{
    int k;
    uint32_t sb;
    pc_buf_append_str(o, "t=", NULL);
    js_str(o, &t->text);
    pc_buf_append_str(o, " f=", NULL);
    js_str(o, &t->font_family);
    memcpy(&sb, &t->size_pt, 4);
    pc_buf_appendf(o, NULL, " s=%08" PRIx32 " c=", sb);
    JS(o, pc_color, &t->color);
    pc_buf_append_str(o, " m=", NULL);
    for (k = 0; k < 6; k++) {
        uint64_t u;
        memcpy(&u, &t->transform.m[k], 8);
        pc_buf_appendf(o, NULL, "%s%016" PRIx64, k > 0 ? "," : "", u);
    }
    pc_buf_append_str(o, " r=", NULL);
    js_runs(o, t->runs, t->nruns);
    pc_buf_append_str(o, " p=", NULL);
    js_paras(o, t->paragraphs, t->nparagraphs);
    pc_buf_append_str(o, " sh=", NULL);
    JS(o, pc_text_shape, &t->shape);
    pc_buf_append_str(o, " o=", NULL);
    JS(o, pc_orientation, &t->orientation);
    pc_buf_append_str(o, " a=", NULL);
    JS(o, pc_antialias, &t->antialias);
    pc_buf_append_str(o, " w=", NULL);
    if (t->has_warp) {
        JS(o, pc_text_warp, &t->warp);
    } else {
        pc_buf_append_str(o, "null", NULL);
    }
}

static void opt_list(pc_buf *o, const pc_opt_index *v, size_t n)
{
    size_t k;
    pc_buf_append_byte(o, '[', NULL);
    for (k = 0; k < n; k++) {
        if (k > 0) {
            pc_buf_append_byte(o, ',', NULL);
        }
        if (v[k].has) {
            pc_buf_appendf(o, NULL, "%zu", v[k].v);
        } else {
            pc_buf_append_str(o, "null", NULL);
        }
    }
    pc_buf_append_byte(o, ']', NULL);
}

static void sdump(pc_buf *o, const pc_psd_style_sheets *s)
{
    size_t k;
    pc_buf_append_str(o, "nc=", NULL);
    JS(o, pc_char_style, &s->normal_char);
    pc_buf_append_str(o, " np=", NULL);
    JS(o, pc_paragraph_style, &s->normal_para);
    pc_buf_append_str(o, " cs=[", NULL);
    for (k = 0; k < s->nchar_sheets; k++) {
        pc_buf_appendf(o, NULL, "%s%zu:", k > 0 ? ";" : "", s->char_sheets[k].index);
        js_str(o, &s->char_sheets[k].name);
        pc_buf_append_byte(o, ':', NULL);
        JS(o, pc_char_style, &s->char_sheets[k].style);
    }
    pc_buf_append_str(o, "] ps=[", NULL);
    for (k = 0; k < s->npara_sheets; k++) {
        pc_buf_appendf(o, NULL, "%s%zu:", k > 0 ? ";" : "", s->para_sheets[k].index);
        js_str(o, &s->para_sheets[k].name);
        pc_buf_append_byte(o, ':', NULL);
        JS(o, pc_paragraph_style, &s->para_sheets[k].style);
        pc_buf_append_byte(o, ':', NULL);
        if (s->para_sheets[k].has_char) {
            JS(o, pc_char_style, &s->para_sheets[k].chr);
        } else {
            pc_buf_append_str(o, "null", NULL);
        }
    }
    pc_buf_append_str(o, "] rc=", NULL);
    opt_list(o, s->run_char, s->nrun_char);
    pc_buf_append_str(o, " rl=[", NULL);
    for (k = 0; k < s->nrun_char_lens; k++) {
        pc_buf_appendf(o, NULL, "%s%zu", k > 0 ? "," : "", s->run_char_lens[k]);
    }
    pc_buf_append_str(o, "] rp=", NULL);
    opt_list(o, s->run_para, s->nrun_para);
}

/* ---- named byte strings (builds and imports) ---- */

typedef struct named {
    char *name;
    uint8_t *data;
    size_t len;
} named;

static named names[512];
static size_t nnames;

static void name_set(const char *name, const uint8_t *data, size_t len)
{
    size_t k;
    for (k = 0; k < nnames; k++) {
        if (strcmp(names[k].name, name) == 0) {
            free(names[k].data);
            break;
        }
    }
    if (k == nnames) {
        if (nnames == sizeof names / sizeof names[0]) {
            return;
        }
        names[nnames].name = strdup(name);
        nnames++;
    }
    names[k].data = malloc(len + 1);
    if (names[k].data != NULL && len > 0) {
        memcpy(names[k].data, data, len);
    }
    names[k].len = len;
}

static const named *name_get(const char *name)
{
    size_t k;
    for (k = 0; k < nnames; k++) {
        if (strcmp(names[k].name, name) == 0) {
            return &names[k];
        }
    }
    return NULL;
}

/* Layer JSON texts by name. */
static named ljson[256];
static size_t nljson;

/* ---- layers from JSON ---- */

static bool layer_from(const char *json, size_t len, pc_text_layer *out)
{
    yyjson_doc *doc;
    yyjson_val *root, *v;
    pc_text_layer t;
    const char *s;
    size_t n, k;
    double f;
    if (pc_json_parse(json, len, &doc, NULL) != PC_OK) {
        return false;
    }
    root = yyjson_doc_get_root(doc);
    pc_text_layer_default(&t);
    if ((s = pc_json_as_str(pc_json_get(root, "text"), &n)) != NULL) {
        pc_str_set(&t.text, s, n, NULL);
    }
    if ((s = pc_json_as_str(pc_json_get(root, "font_family"), &n)) != NULL) {
        pc_str_set(&t.font_family, s, n, NULL);
    }
    if (pc_json_as_f64(pc_json_get(root, "size_pt"), &f)) {
        t.size_pt = (float)f;
    }
    if ((v = pc_json_get(root, "color")) != NULL) {
        CHECK(pc_color_from_json(v, &t.color, NULL) == PC_OK);
    }
    if ((v = pc_json_get(root, "transform")) != NULL) {
        CHECK(pc_affine_from_json(v, &t.transform, NULL) == PC_OK);
    }
    if ((v = pc_json_get(root, "runs")) != NULL) {
        n = yyjson_arr_size(v);
        t.runs = calloc(n + 1, sizeof *t.runs);
        for (k = 0; k < n; k++) {
            CHECK(pc_text_run_from_json(yyjson_arr_get(v, k), &t.runs[k], NULL) == PC_OK);
        }
        t.nruns = n;
    }
    if ((v = pc_json_get(root, "paragraphs")) != NULL) {
        n = yyjson_arr_size(v);
        t.paragraphs = calloc(n + 1, sizeof *t.paragraphs);
        for (k = 0; k < n; k++) {
            CHECK(pc_paragraph_run_from_json(yyjson_arr_get(v, k), &t.paragraphs[k], NULL) == PC_OK);
        }
        t.nparagraphs = n;
    }
    if ((v = pc_json_get(root, "shape")) != NULL) {
        CHECK(pc_text_shape_from_json(v, &t.shape, NULL) == PC_OK);
    }
    if ((v = pc_json_get(root, "orientation")) != NULL) {
        CHECK(pc_orientation_from_json(v, &t.orientation, NULL) == PC_OK);
    }
    if ((v = pc_json_get(root, "antialias")) != NULL) {
        CHECK(pc_antialias_from_json(v, &t.antialias, NULL) == PC_OK);
    }
    if ((v = pc_json_get(root, "warp")) != NULL && !yyjson_is_null(v)) {
        CHECK(pc_text_warp_from_json(v, &t.warp, NULL) == PC_OK);
        t.has_warp = true;
    }
    if ((s = pc_json_as_str(pc_json_get(root, "psd_raw"), &n)) != NULL) {
        const named *r = name_get(s + 1);
        CHECK(r != NULL);
        if (r != NULL) {
            CHECK(pc_blob_new(r->data, r->len, &t.psd_raw, NULL) == PC_OK);
        }
    }
    yyjson_doc_free(doc);
    *out = t;
    return true;
}

static bool layer_named(const char *name, pc_text_layer *out)
{
    size_t k;
    for (k = 0; k < nljson; k++) {
        if (strcmp(ljson[k].name, name) == 0) {
            return layer_from((const char *)ljson[k].data, ljson[k].len, out);
        }
    }
    return false;
}

/* ---- results ---- */

static void imp_result(pc_buf *o, const uint8_t *d, size_t n, float dpi)
{
    pc_text_layer l;
    pc_psd_style_sheets s;
    bool blank = false, named_ = false;
    if (pc_text_layer_from_tysh(d, n, dpi, &l, NULL) == PC_OK) {
        ldump(o, &l);
        pc_text_layer_fini(&l);
    } else {
        pc_buf_append_str(o, "NONE", NULL);
    }
    CHECK(pc_tysh_engine_text_is_blank(d, n, &blank, NULL) == PC_OK);
    CHECK(pc_text_has_named_sheets(d, n, &named_, NULL) == PC_OK);
    pc_buf_appendf(o, NULL, " | b%d n%d | ", blank ? 1 : 0, named_ ? 1 : 0);
    if (pc_text_read_style_sheets(d, n, dpi, &s, NULL) == PC_OK) {
        sdump(o, &s);
        pc_psd_style_sheets_fini(&s);
    } else {
        pc_buf_append_str(o, "NONE", NULL);
    }
}

static void short_imp(pc_buf *o, const uint8_t *d, size_t n)
{
    pc_buf r = PC_BUF_INIT;
    imp_result(&r, d, n, 72.0f);
    pc_buf_appendf(o, NULL, "#%016" PRIx64 "/%zu", fnv64(r.data, r.len), r.len);
    pc_buf_fini(&r);
}

static void build_line(pc_buf *o, const char *name, const char *dpi_hex, const char *ink_s)
{
    pc_text_layer t, back;
    pc_psd_bytes b = {NULL, 0};
    float dpi = f32of(dpi_hex), ink[4];
    bool has_ink = strcmp(ink_s, "-") != 0;
    if (!layer_named(name, &t)) {
        pc_buf_append_str(o, "NOLAYER", NULL);
        return;
    }
    if (has_ink) {
        const char *p = ink_s;
        int k;
        for (k = 0; k < 4; k++) {
            ink[k] = f32of(p);
            p = strchr(p, ',');
            p = p != NULL ? p + 1 : "0";
        }
    }
    CHECK(pc_text_build_tysh(&t, dpi, has_ink ? ink : NULL, &b, NULL) == PC_OK);
    pc_buf_appendf(o, NULL, "%zu %016" PRIx64 " | ", b.len, fnv64(b.data, b.len));
    if (pc_text_layer_from_tysh(b.data, b.len, dpi, &back, NULL) == PC_OK) {
        ldump(o, &back);
        pc_text_layer_fini(&back);
    } else {
        pc_buf_append_str(o, "NONE", NULL);
    }
    if (dpi == 72.0f && !has_ink && strncmp(name, "edit_", 5) != 0) {
        name_set(name, b.data, b.len);
    }
    pc_psd_bytes_fini(&b);
    pc_text_layer_fini(&t);
}

static void bed_line(pc_buf *o, const char *lname, const char *thex)
{
    pc_text_layer t;
    pc_ed_value tmpl, e;
    bool has_tmpl = strcmp(thex, "-") != 0;
    pc_buf w = PC_BUF_INIT;
    size_t from;
    if (!layer_named(lname, &t)) {
        pc_buf_append_str(o, "NOLAYER", NULL);
        return;
    }
    if (has_tmpl) {
        size_t n;
        uint8_t *b = unhex(thex, strlen(thex), &n);
        CHECK(b != NULL && pc_ed_parse(b, n, &tmpl, NULL) == PC_OK);
        free(b);
    }
    CHECK(pc_text_build_engine_data(&t, has_tmpl ? &tmpl : NULL, 96.0f, &e, NULL) == PC_OK);
    from = o->len;
    dump(o, &e);
    shorten(o, from);
    CHECK(pc_ed_write(&e, &w, NULL) == PC_OK);
    pc_buf_appendf(o, NULL, " | %zu %016" PRIx64, w.len, fnv64(w.data, w.len));
    pc_buf_fini(&w);
    pc_ed_fini(&e);
    if (has_tmpl) {
        pc_ed_fini(&tmpl);
    }
    pc_text_layer_fini(&t);
}

static void txt2_line(pc_buf *o, const char *ref, const char *hex)
{
    size_t n;
    uint8_t *b = unhex(hex, strlen(hex), &n);
    const named *r = name_get(ref);
    pc_ed_value txt2;
    pc_text_layer l;
    if (b == NULL || r == NULL) {
        pc_buf_append_str(o, "NOREF", NULL);
        free(b);
        return;
    }
    if (pc_txt2_parse(b, n, &txt2, NULL) != PC_OK) {
        pc_buf_append_str(o, "NOTXT2", NULL);
    } else {
        if (pc_text_layer_from_tysh(r->data, r->len, 72.0f, &l, NULL) != PC_OK) {
            pc_buf_append_str(o, "NONE", NULL);
        } else {
            uint32_t sb;
            CHECK(pc_txt2_apply(&l, r->data, r->len, &txt2, NULL) == PC_OK);
            js_runs(o, l.runs, l.nruns);
            pc_buf_append_byte(o, ' ', NULL);
            js_str(o, &l.font_family);
            memcpy(&sb, &l.size_pt, 4);
            pc_buf_appendf(o, NULL, " %08" PRIx32 " ", sb);
            JS(o, pc_color, &l.color);
            pc_text_layer_fini(&l);
        }
        pc_ed_fini(&txt2);
    }
    free(b);
}

/* wss <ref> <dpi> <layer json> <styles json> */
static void wss_line(pc_buf *o, const char *args)
{
    char ref[64];
    char dpi_hex[16];
    const char *p = args;
    int consumed = 0;
    yyjson_doc *ld;
    yyjson_read_err rerr;
    size_t llen;
    pc_text_layer t;
    yyjson_doc *sd;
    pc_text_styles styles;
    const named *r;
    pc_psd_bytes out = {NULL, 0};
    if (sscanf(p, "%63s %15s %n", ref, dpi_hex, &consumed) != 2) {
        pc_buf_append_str(o, "BADLINE", NULL);
        return;
    }
    p += consumed;
    ld = yyjson_read_opts((char *)(uintptr_t)p, strlen(p), YYJSON_READ_STOP_WHEN_DONE, NULL, &rerr);
    if (ld == NULL) {
        pc_buf_append_str(o, "BADJSON", NULL);
        return;
    }
    llen = yyjson_doc_get_read_size(ld);
    yyjson_doc_free(ld);
    CHECK(layer_from(p, llen, &t));
    p += llen + 1;
    sd = yyjson_read(p, strlen(p), 0);
    CHECK(sd != NULL && pc_text_styles_from_json(yyjson_doc_get_root(sd), &styles, NULL) == PC_OK);
    yyjson_doc_free(sd);
    r = name_get(ref);
    CHECK(r != NULL);
    if (r != NULL &&
        pc_text_write_style_sheets(r->data, r->len, &t, &styles, f32of(dpi_hex), &out, NULL) == PC_OK) {
        pc_psd_style_sheets s;
        pc_text_layer back;
        bool nm = false;
        float dpi = f32of(dpi_hex);
        CHECK(pc_text_has_named_sheets(out.data, out.len, &nm, NULL) == PC_OK);
        pc_buf_appendf(o, NULL, "%zu %016" PRIx64 " n%d | ", out.len, fnv64(out.data, out.len), nm ? 1 : 0);
        if (pc_text_read_style_sheets(out.data, out.len, dpi, &s, NULL) == PC_OK) {
            sdump(o, &s);
            pc_psd_style_sheets_fini(&s);
        } else {
            pc_buf_append_str(o, "NONE", NULL);
        }
        pc_buf_append_str(o, " | ", NULL);
        if (pc_text_layer_from_tysh(out.data, out.len, dpi, &back, NULL) == PC_OK) {
            ldump(o, &back);
            pc_text_layer_fini(&back);
        } else {
            pc_buf_append_str(o, "NONE", NULL);
        }
        pc_psd_bytes_fini(&out);
    } else {
        pc_buf_append_str(o, "NONE", NULL);
    }
    pc_text_styles_fini(&styles);
    pc_text_layer_fini(&t);
}

/* ---- corpus ---- */

static pc_psd_file cfile;
static bool cfile_ok;
static char cpath[1024];

static void hash_of(pc_buf *o, const pc_buf *s)
{
    pc_buf_appendf(o, NULL, "%016" PRIx64, fnv64(s->data, s->len));
}

static void corpus_result(pc_buf *o, const uint8_t *d, size_t n, const pc_ed_value *txt2)
{
    pc_tysh t;
    pc_ed_value e;
    pc_text_layer l, back;
    pc_buf ed = PC_BUF_INIT, rt = PC_BUF_INIT, s = PC_BUF_INIT;
    pc_psd_style_sheets sh;
    pc_psd_bytes bt = {NULL, 0}, ws = {NULL, 0};
    pc_text_styles empty;
    bool blank = false, nm = false;
    if (pc_tysh_parse(d, n, &t, NULL) == PC_OK) {
        pc_psd_bytes w = {NULL, 0};
        if (pc_tysh_engine_data(&t.text, &e, NULL) == PC_OK) {
            pc_buf wb = PC_BUF_INIT;
            CHECK(pc_ed_write(&e, &wb, NULL) == PC_OK);
            pc_buf_appendf(&ed, NULL, "%zu:%016" PRIx64, wb.len, fnv64(wb.data, wb.len));
            pc_buf_fini(&wb);
            pc_ed_fini(&e);
        } else {
            pc_buf_append_str(&ed, "NONE", NULL);
        }
        CHECK(pc_tysh_write(&t, &w, NULL) == PC_OK);
        pc_buf_appendf(&rt, NULL, "%zu:%016" PRIx64 ":%d", w.len, fnv64(w.data, w.len),
                       w.len == n && memcmp(w.data, d, n) == 0 ? 1 : 0);
        pc_psd_bytes_fini(&w);
        pc_tysh_fini(&t);
    } else {
        pc_buf_append_str(&ed, "NONE", NULL);
        pc_buf_append_str(&rt, "NONE", NULL);
    }
    if (pc_text_layer_from_tysh(d, n, 72.0f, &l, NULL) != PC_OK) {
        pc_buf_appendf(o, NULL, "NONE ed%s rt%s", pc_buf_cstr(&ed), pc_buf_cstr(&rt));
        goto done;
    }
    ldump(&s, &l);
    hash_of(o, &s);
    pc_buf_append_byte(o, ' ', NULL);
    if (txt2 != NULL) {
        CHECK(pc_txt2_apply(&l, d, n, txt2, NULL) == PC_OK);
    }
    pc_buf_truncate(&s, 0);
    ldump(&s, &l);
    hash_of(o, &s);
    CHECK(pc_tysh_engine_text_is_blank(d, n, &blank, NULL) == PC_OK);
    CHECK(pc_text_has_named_sheets(d, n, &nm, NULL) == PC_OK);
    pc_buf_appendf(o, NULL, " b%d n%d s", blank ? 1 : 0, nm ? 1 : 0);
    if (pc_text_read_style_sheets(d, n, 72.0f, &sh, NULL) == PC_OK) {
        pc_buf_truncate(&s, 0);
        sdump(&s, &sh);
        hash_of(o, &s);
        pc_psd_style_sheets_fini(&sh);
    } else {
        pc_buf_append_str(o, "NONE", NULL);
    }
    pc_buf_appendf(o, NULL, " ed%s rt%s", pc_buf_cstr(&ed), pc_buf_cstr(&rt));
    CHECK(pc_blob_new(d, n, &l.psd_raw, NULL) == PC_OK);
    CHECK(pc_text_build_tysh(&l, 72.0f, NULL, &bt, NULL) == PC_OK);
    pc_buf_appendf(o, NULL, " bt%zu:%016" PRIx64 " bb", bt.len, fnv64(bt.data, bt.len));
    if (pc_text_layer_from_tysh(bt.data, bt.len, 72.0f, &back, NULL) == PC_OK) {
        pc_buf_truncate(&s, 0);
        ldump(&s, &back);
        hash_of(o, &s);
        pc_text_layer_fini(&back);
    } else {
        pc_buf_append_str(o, "NONE", NULL);
    }
    CHECK(pc_text_styles_default(&empty, NULL) == PC_OK);
    pc_buf_append_str(o, " ws", NULL);
    if (pc_text_write_style_sheets(d, n, &l, &empty, 72.0f, &ws, NULL) == PC_OK) {
        pc_buf_appendf(o, NULL, "%zu:%016" PRIx64, ws.len, fnv64(ws.data, ws.len));
    } else {
        pc_buf_append_str(o, "NONE", NULL);
    }
    pc_text_styles_fini(&empty);
    pc_psd_bytes_fini(&bt);
    pc_psd_bytes_fini(&ws);
    pc_text_layer_fini(&l);
done:
    pc_buf_fini(&ed);
    pc_buf_fini(&rt);
    pc_buf_fini(&s);
}

/* Type layers of the current corpus file. */
static const pc_psd_block *type_block(size_t layer)
{
    size_t n = 0;
    const pc_psd_layer *ls = pc_psd_file_layers(&cfile, &n);
    return layer < n ? pc_psd_layer_block(&ls[layer], "TySh") : NULL;
}

static bool corpus_file_line(pc_buf *o, const char *path)
{
    char full[1100];
    size_t n = 0, k, count = 0;
    const pc_psd_layer *ls;
    const pc_psd_block *t2;
    pc_ed_value txt2;
    bool has_txt2 = false;
    if (cfile_ok) {
        pc_psd_file_fini(&cfile);
        cfile_ok = false;
    }
    (void)snprintf(full, sizeof full, "corpus/%s", path);
    (void)snprintf(cpath, sizeof cpath, "%s", path);
    if (pc_psd_file_open(full, &cfile, NULL) != PC_OK) {
        return false;
    }
    cfile_ok = true;
    ls = pc_psd_file_layers(&cfile, &n);
    for (k = 0; k < n; k++) {
        if (pc_psd_layer_block(&ls[k], "TySh") != NULL) {
            count++;
        }
    }
    t2 = pc_psd_file_global_block(&cfile, "Txt2");
    if (t2 != NULL && pc_txt2_parse(t2->data.data, t2->data.len, &txt2, NULL) == PC_OK) {
        has_txt2 = true;
        pc_ed_fini(&txt2);
    }
    pc_buf_appendf(o, NULL, "%zu %d", count, has_txt2 ? 1 : 0);
    return true;
}

static void corpus_line(pc_buf *o, size_t layer)
{
    const pc_psd_block *b = type_block(layer);
    const pc_psd_block *t2 = pc_psd_file_global_block(&cfile, "Txt2");
    pc_ed_value txt2;
    bool has = false;
    if (b == NULL) {
        pc_buf_append_str(o, "NOLAYER", NULL);
        return;
    }
    if (t2 != NULL && pc_txt2_parse(t2->data.data, t2->data.len, &txt2, NULL) == PC_OK) {
        has = true;
    }
    corpus_result(o, b->data.data, b->data.len, has ? &txt2 : NULL);
    if (has) {
        pc_ed_fini(&txt2);
    }
}

/* ---- driver ---- */

typedef struct base {
    char name[32];
    uint8_t *data;
    size_t len;
} base;

static base bases[4];
static size_t nbases;

static const base *base_get(const char *name)
{
    size_t k;
    for (k = 0; k < nbases; k++) {
        if (strcmp(bases[k].name, name) == 0) {
            return &bases[k];
        }
    }
    return NULL;
}

static int counts[32];
static const char *const kinds[] = {"ed",     "edcut", "edmut", "edreal",     "edint",  "edstr",
                                    "edname", "build", "bed",   "imp",        "impdpi", "impcut",
                                    "impmut", "txt2",  "wss",   "corpusfile", "corpus"};

static void count(const char *kind)
{
    size_t k;
    for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        if (strcmp(kinds[k], kind) == 0) {
            counts[k]++;
        }
    }
}

static void compare(const char *line, const pc_buf *got, const char *want)
{
    pc_t_checks++;
    if (strcmp(pc_buf_cstr(got), want) != 0) {
        pc_t_failed_checks++;
        if (shown++ < (getenv("PSD_PARITY_SHOW") != NULL ? 100000 : 8)) {
            (void)fprintf(stderr, "MISMATCH %.200s\n  want %.1500s\n  got  %.1500s\n", line, want,
                          pc_buf_cstr(got));
        }
    }
}

/* ---- R8 kinds (text_psd_vectors.rs r8_section) ---- */

/* r8b2 <layer>:<index>,... <prev hex|-> => needs len hash dump(parse back) */
static void r8b2_line(pc_buf *o, char *spec, const char *prev)
{
    pc_text_layer layers[8];
    pc_txt2_object objs[8];
    size_t n = 0, k, plen = 0;
    uint8_t *pb = NULL;
    bool needs = false;
    pc_buf out = PC_BUF_INIT;
    pc_ed_value back;
    char *save = NULL, *tok;
    if (strcmp(spec, "-") != 0) {
        for (tok = strtok_r(spec, ",", &save); tok != NULL && n < 8; tok = strtok_r(NULL, ",", &save)) {
            char *colon = strrchr(tok, ':');
            if (colon == NULL) {
                continue;
            }
            *colon = '\0';
            if (!layer_named(tok, &layers[n])) {
                pc_buf_append_str(o, "NOLAYER", NULL);
                for (k = 0; k < n; k++) {
                    pc_text_layer_fini(&layers[k]);
                }
                return;
            }
            objs[n].index = (int32_t)strtol(colon + 1, NULL, 10);
            objs[n].layer = &layers[n];
            n++;
        }
    }
    if (strcmp(prev, "-") != 0) {
        pb = unhex(prev, strlen(prev), &plen);
    }
    CHECK(pc_txt2_needed(objs, n, &needs, NULL) == PC_OK);
    CHECK(pc_txt2_build(objs, n, pb, plen, pb != NULL, &out, NULL) == PC_OK);
    pc_buf_appendf(o, NULL, "%d %zu %016" PRIx64 " ", needs ? 1 : 0, out.len, fnv64(out.data, out.len));
    if (pc_txt2_parse(out.data, out.len, &back, NULL) == PC_OK) {
        size_t from = o->len;
        dump(o, &back);
        shorten(o, from);
        pc_ed_fini(&back);
    } else {
        pc_buf_append_str(o, "NOTXT2", NULL);
    }
    pc_buf_fini(&out);
    free(pb);
    for (k = 0; k < n; k++) {
        pc_text_layer_fini(&layers[k]);
    }
}

static void r8bare_line(pc_buf *o, const char *hex)
{
    size_t n;
    uint8_t *b = unhex(hex, strlen(hex), &n);
    pc_ed_value v;
    if (b != NULL && pc_txt2_parse(b, n, &v, NULL) == PC_OK) {
        pc_buf out = PC_BUF_INIT;
        CHECK(pc_ed_write_bare(&v, &out, NULL) == PC_OK);
        pc_buf_appendf(o, NULL, "%zu %016" PRIx64, out.len, fnv64(out.data, out.len));
        pc_buf_fini(&out);
        pc_ed_fini(&v);
    } else {
        pc_buf_append_str(o, "NOTXT2", NULL);
    }
    free(b);
}

static void r8tix_line(pc_buf *o, const char *ref, const char *idx)
{
    const named *r = name_get(ref);
    pc_psd_bytes b = {NULL, 0};
    bool has = false;
    int32_t i = 0;
    if (r == NULL) {
        pc_buf_append_str(o, "NOREF", NULL);
    } else if (pc_tysh_set_text_index(r->data, r->len, (int32_t)strtol(idx, NULL, 10), &b, NULL) != PC_OK) {
        pc_buf_append_str(o, "NONE", NULL);
    } else {
        CHECK(pc_tysh_text_index(b.data, b.len, &has, &i, NULL) == PC_OK);
        pc_buf_appendf(o, NULL, "%zu %016" PRIx64 " ", b.len, fnv64(b.data, b.len));
        if (has) {
            pc_buf_appendf(o, NULL, "%d", (int)i);
        } else {
            pc_buf_append_byte(o, '-', NULL);
        }
        pc_psd_bytes_fini(&b);
    }
}

static double f64of(const char *hex)
{
    uint64_t u = strtoull(hex, NULL, 16);
    double d;
    memcpy(&d, &u, 8);
    return d;
}

static void r8move_bytes(pc_buf *o, const uint8_t *d, size_t n, double dx, double dy, bool len_only)
{
    pc_psd_bytes b = {NULL, 0};
    if (pc_tysh_translate(d, n, dx, dy, &b, NULL) != PC_OK) {
        pc_buf_append_str(o, "NONE", NULL);
        return;
    }
    if (len_only) {
        pc_buf_appendf(o, NULL, "%zu", b.len);
    } else {
        pc_buf_appendf(o, NULL, "%zu %016" PRIx64, b.len, fnv64(b.data, b.len));
    }
    pc_psd_bytes_fini(&b);
}

static void r8frame_line(pc_buf *o, const char *ref, const char *hex)
{
    size_t n;
    uint8_t *b = unhex(hex, strlen(hex), &n);
    const named *r = name_get(ref);
    pc_ed_value txt2;
    pc_text_layer l;
    if (b == NULL || r == NULL) {
        pc_buf_append_str(o, "NOREF", NULL);
    } else if (pc_txt2_parse(b, n, &txt2, NULL) != PC_OK) {
        pc_buf_append_str(o, "NOTXT2", NULL);
    } else {
        if (pc_text_layer_from_tysh(r->data, r->len, 72.0f, &l, NULL) != PC_OK) {
            pc_buf_append_str(o, "NONE", NULL);
        } else {
            CHECK(pc_txt2_apply_frame(&l, r->data, r->len, &txt2, NULL) == PC_OK);
            ldump(o, &l);
            pc_text_layer_fini(&l);
        }
        pc_ed_fini(&txt2);
    }
    free(b);
}

static void replay_line(char *line, bool have_corpus)
{
    char *arrow = strstr(line, " => ");
    char *want = NULL;
    char kind[32];
    char *args;
    pc_buf got = PC_BUF_INIT;
    if (line[0] == '#' || line[0] == '\0') {
        return;
    }
    if (sscanf(line, "%31s", kind) != 1) {
        return;
    }
    args = line + strlen(kind) + (line[strlen(kind)] == ' ' ? 1 : 0);
    if (strcmp(kind, "layer") == 0) {
        char *sp = strchr(args, ' ');
        if (sp != NULL && nljson < sizeof ljson / sizeof ljson[0]) {
            ljson[nljson].name = strndup(args, (size_t)(sp - args));
            ljson[nljson].data = (uint8_t *)strdup(sp + 1);
            ljson[nljson].len = strlen(sp + 1);
            nljson++;
        }
        return;
    }
    if (strcmp(kind, "edbase") == 0 && nbases < 4) {
        char *sp = strchr(args, ' ');
        if (sp != NULL) {
            (void)snprintf(bases[nbases].name, sizeof bases[nbases].name, "%.*s", (int)(sp - args), args);
            bases[nbases].data = unhex(sp + 1, strlen(sp + 1), &bases[nbases].len);
            nbases++;
        }
        return;
    }
    if (strcmp(kind, "tyshhex") == 0) {
        char *sp = strchr(args, ' ');
        if (sp != NULL) {
            size_t n;
            uint8_t *b = unhex(sp + 1, strlen(sp + 1), &n);
            *sp = '\0';
            name_set(args, b, n);
            free(b);
        }
        return;
    }
    if (arrow == NULL) {
        return;
    }
    *arrow = '\0';
    want = arrow + 4;
    count(kind);
    if (strcmp(kind, "ed") == 0) {
        size_t n;
        uint8_t *b = unhex(args, strlen(args), &n);
        ed_result(&got, b, n);
        free(b);
    } else if (strcmp(kind, "edcut") == 0 || strcmp(kind, "edmut") == 0) {
        char name[32];
        size_t a = 0;
        unsigned byte = 0;
        const base *bs;
        int nf = sscanf(args, "%31s %zu %u", name, &a, &byte);
        bs = base_get(name);
        if (bs != NULL && nf >= 2) {
            uint8_t *m = malloc(bs->len + 1);
            if (m != NULL) {
                memcpy(m, bs->data, bs->len);
                if (kind[2] == 'm' && a < bs->len) {
                    m[a] = (uint8_t)byte;
                }
                ed_result(&got, m, kind[2] == 'c' ? (a < bs->len ? a : bs->len) : bs->len);
                shorten(&got, 0);
                free(m);
            }
        }
    } else if (strcmp(kind, "edreal") == 0 || strcmp(kind, "edint") == 0 || strcmp(kind, "edstr") == 0 ||
               strcmp(kind, "edname") == 0) {
        pc_ed_value v;
        pc_buf w = PC_BUF_INIT;
        if (kind[2] == 'r') {
            uint64_t u = strtoull(args, NULL, 16);
            double r;
            memcpy(&r, &u, 8);
            v = pc_ed_real(r);
        } else if (kind[2] == 'i') {
            v = pc_ed_int(strtoll(args, NULL, 10));
        } else {
            size_t n;
            uint8_t *b = unhex(args, strlen(args), &n);
            if (kind[2] == 's') {
                CHECK(pc_ed_string((const char *)b, n, &v, NULL) == PC_OK);
            } else {
                CHECK(pc_ed_name((const char *)b, n, &v, NULL) == PC_OK);
            }
            free(b);
        }
        CHECK(pc_ed_write(&v, &w, NULL) == PC_OK);
        esc(&got, w.data, w.len);
        pc_buf_fini(&w);
        pc_ed_fini(&v);
    } else if (strcmp(kind, "build") == 0) {
        char name[64], dpi[16], ink[64];
        if (sscanf(args, "%63s %15s %63s", name, dpi, ink) == 3) {
            build_line(&got, name, dpi, ink);
        }
    } else if (strcmp(kind, "bed") == 0) {
        char *sp = strchr(args, ' ');
        if (sp != NULL) {
            *sp = '\0';
            bed_line(&got, args, sp + 1);
        }
    } else if (strcmp(kind, "imp") == 0 || strcmp(kind, "impdpi") == 0) {
        char *sp = strchr(args, ' ');
        if (sp != NULL) {
            *sp = '\0';
            if (kind[3] == '\0') {
                size_t n;
                uint8_t *b = unhex(sp + 1, strlen(sp + 1), &n);
                name_set(args, b, n);
                imp_result(&got, b, n, 72.0f);
                free(b);
            } else {
                const named *r = name_get(args);
                if (r != NULL) {
                    imp_result(&got, r->data, r->len, f32of(sp + 1));
                }
            }
        }
    } else if (strcmp(kind, "impcut") == 0 || strcmp(kind, "impmut") == 0) {
        char name[32];
        size_t a = 0;
        unsigned byte = 0;
        int nf = sscanf(args, "%31s %zu %u", name, &a, &byte);
        const named *r = name_get(name);
        if (r != NULL && nf >= 2) {
            uint8_t *m = malloc(r->len + 1);
            if (m != NULL) {
                memcpy(m, r->data, r->len);
                if (kind[3] == 'm' && a < r->len) {
                    m[a] = (uint8_t)byte;
                }
                short_imp(&got, m, kind[3] == 'c' ? (a < r->len ? a : r->len) : r->len);
                free(m);
            }
        }
    } else if (strcmp(kind, "txt2") == 0) {
        char *sp = strchr(args, ' ');
        if (sp != NULL) {
            *sp = '\0';
            txt2_line(&got, args, sp + 1);
        }
    } else if (strcmp(kind, "wss") == 0) {
        wss_line(&got, args);
    } else if (strcmp(kind, "r8b2") == 0 || strcmp(kind, "r8frame") == 0 || strcmp(kind, "r8tix") == 0 ||
               strcmp(kind, "r8move") == 0) {
        char a[4096], b[16];
        char *sp = strchr(args, ' ');
        if (sp != NULL && (size_t)(sp - args) < sizeof a) {
            char *rest = sp + 1, *end = strstr(rest, " => ");
            memcpy(a, args, (size_t)(sp - args));
            a[sp - args] = '\0';
            if (end != NULL) {
                *end = '\0';
            }
            if (kind[2] == 'b') {
                r8b2_line(&got, a, rest);
            } else if (kind[2] == 'f') {
                r8frame_line(&got, a, rest);
            } else if (kind[2] == 't') {
                r8tix_line(&got, a, rest);
            } else {
                const named *r = name_get(a);
                char dx[32] = "", dy[32] = "";
                (void)b;
                if (r != NULL && sscanf(rest, "%31s %31s", dx, dy) == 2) {
                    r8move_bytes(&got, r->data, r->len, f64of(dx), f64of(dy), false);
                } else {
                    pc_buf_append_str(&got, "NOREF", NULL);
                }
            }
            if (end != NULL) {
                *end = ' ';
            }
        }
    } else if (strcmp(kind, "r8tixr") == 0 || strcmp(kind, "r8bare") == 0 || strcmp(kind, "r8movehex") == 0) {
        char *end = strstr(args, " => ");
        if (end != NULL) {
            *end = '\0';
        }
        if (kind[2] == 'b') {
            r8bare_line(&got, args);
        } else if (kind[2] == 'm') {
            size_t n;
            uint8_t *bb = unhex(args, strlen(args), &n);
            r8move_bytes(&got, bb, n, 1.0, 1.0, true);
            free(bb);
        } else {
            const named *r = name_get(args);
            bool has = false;
            int32_t i = 0;
            if (r != NULL) {
                CHECK(pc_tysh_text_index(r->data, r->len, &has, &i, NULL) == PC_OK);
            }
            if (has) {
                pc_buf_appendf(&got, NULL, "%d", (int)i);
            } else {
                pc_buf_append_byte(&got, '-', NULL);
            }
        }
        if (end != NULL) {
            *end = ' ';
        }
    } else if (strcmp(kind, "corpusfile") == 0) {
        if (!have_corpus) {
            skipped++;
            pc_buf_fini(&got);
            return;
        }
        if (!corpus_file_line(&got, args)) {
            pc_buf_append_str(&got, "UNREADABLE", NULL);
        }
    } else if (strcmp(kind, "corpus") == 0) {
        char *sp = strrchr(args, ' ');
        if (!have_corpus) {
            skipped++;
            pc_buf_fini(&got);
            return;
        }
        if (sp != NULL) {
            *sp = '\0';
            if (cfile_ok && strcmp(cpath, args) == 0) {
                corpus_line(&got, (size_t)strtoul(sp + 1, NULL, 10));
            } else {
                pc_buf_append_str(&got, "NOFILE", NULL);
            }
            *sp = ' ';
        }
    } else {
        pc_buf_fini(&got);
        return;
    }
    compare(line, &got, want);
    pc_buf_fini(&got);
}

static void test_golden(void)
{
    pc_buf g = PC_BUF_INIT;
    struct stat sb;
    bool have_corpus = stat("corpus/psd", &sb) == 0;
    char *p, *nl;
    size_t k;
    if (pc_file_read(GOLDEN, (size_t)1 << 26, &g, NULL) != PC_OK) {
        PC_T_FAIL("cannot read %s", GOLDEN);
        return;
    }
    for (p = (char *)g.data; p != NULL && *p != '\0'; p = nl != NULL ? nl + 1 : NULL) {
        nl = strchr(p, '\n');
        if (nl != NULL) {
            *nl = '\0';
        }
        replay_line(p, have_corpus);
    }
    if (cfile_ok) {
        pc_psd_file_fini(&cfile);
        cfile_ok = false;
    }
    for (k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        (void)printf("%s %d  ", kinds[k], counts[k]);
    }
    (void)printf("\n(corpus lines skipped: %d) ", skipped);
    pc_buf_fini(&g);
    for (k = 0; k < nnames; k++) {
        free(names[k].name);
        free(names[k].data);
    }
    for (k = 0; k < nljson; k++) {
        free(ljson[k].name);
        free(ljson[k].data);
    }
    for (k = 0; k < nbases; k++) {
        free(bases[k].data);
    }
}

int main(int argc, char **argv)
{
    RUN(test_golden);
    return pc_t_finish();
}
