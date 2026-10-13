/* Parity with the Rust reference at document level: tests/golden/format2-vectors.txt was
 * written by `rustref format2-vectors` (tools/rustref/src/format2_vectors.rs; its header lists
 * the lines).
 *
 *   - the C writer's manifest.json is byte-identical to the reference's for every document
 *     (parity_docs.h builds the same documents), with the same objects and save statistics;
 *   - loading gives an equal document, and the document a load gives saves to the reference's
 *     manifest again (a canonical dump of the load);
 *   - refused saves (non-finite values, nesting) fail with the reference's messages;
 *   - Rust-written bundles load in C into documents equal to the C-built ones;
 *   - hand-made hostile manifests and a sweep of single mutations of a rich manifest load (or
 *     fail) exactly as in the reference;
 *   - autosave: key sanitising, the sidecar text and list_recovery over synthetic directories.
 *
 * C writes -> Rust reads: with PC_FORMAT2_XDIR=DIR this test also writes every document's C
 * bundle to DIR/NAME.pcraft and DIR/NAME.dir; `PC_FORMAT2_XREAD=DIR rustref format2-vectors`
 * then loads them with the reference and prints `xread` lines (all must say equal). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/base64.h"
#include "base/hash.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "common.h"
#include "format/bridge.h"
#include "harness.h"
#include "parity_docs.h"

#define GOLDEN "tests/golden/format2-vectors.txt"
#define MAXF 6
#define MAX_DOCS 32

typedef struct line {
    char *f[MAXF];
    size_t n;
} line;

static line *g_lines;
static size_t g_nlines;
static char *g_text;
static size_t g_checked[16];
static size_t g_cut; /* error lines compared as a prefix (C messages stop at 255 bytes) */

enum {
    K_MAN,
    K_OBJS,
    K_STATS,
    K_RT,
    K_RSAVE,
    K_SAVEERR,
    K_BUNDLE,
    K_XH,
    K_XM,
    K_KEY,
    K_INFO,
    K_LIST,
    K_COUNT
};

static int load_golden(void)
{
    pc_buf b = PC_BUF_INIT;
    size_t cap = 0;
    char *p;
    if (t_read_file(GOLDEN, &b) != 0) {
        return -1;
    }
    g_text = (char *)pc_buf_take(&b, NULL);
    p = g_text;
    while (p != NULL && *p != '\0') {
        char *end = strchr(p, '\n');
        line l;
        if (end != NULL) {
            *end = '\0';
        }
        memset(&l, 0, sizeof l);
        while (l.n < MAXF) {
            char *tab = strchr(p, '\t');
            l.f[l.n++] = p;
            if (tab == NULL) {
                break;
            }
            *tab = '\0';
            p = tab + 1;
        }
        if (g_nlines == cap) {
            line *g;
            cap = cap ? cap * 2 : 1024;
            g = realloc(g_lines, cap * sizeof *g_lines);
            if (g == NULL) {
                return -1;
            }
            g_lines = g;
        }
        g_lines[g_nlines++] = l;
        p = end != NULL ? end + 1 : NULL;
    }
    return 0;
}

static const line *find(const char *kind, const char *name)
{
    for (size_t i = 0; i < g_nlines; i++) {
        if (g_lines[i].n >= 3 && strcmp(g_lines[i].f[0], kind) == 0 && strcmp(g_lines[i].f[1], name) == 0) {
            return &g_lines[i];
        }
    }
    return NULL;
}

static void dq(pc_buf *out, const char *s, size_t n)
{
    char q[2048];
    pc_utf8_debug_quote((const uint8_t *)s, n, q, sizeof q);
    (void)pc_buf_append_str(out, q, NULL);
}

static void err_text(pc_buf *out, const pc_error *e)
{
    (void)pc_buf_append_str(out, "E", NULL);
    dq(out, e->msg, strlen(e->msg));
}

/* got == want, or got is a cut-off prefix of want (a message longer than pc_error holds). */
static void check_text(const char *what, const char *got, const char *want)
{
    size_t gl = strlen(got);
    if (strcmp(got, want) == 0) {
        return;
    }
    if (gl > 200 && got[gl - 1] == '"' && strncmp(got, want, gl - 1) == 0) {
        g_cut++;
        return;
    }
    PC_T_FAIL("%s:\n  got  %s\n  want %s", what, got, want);
}

static void hex_full(const uint8_t *d, size_t n, char out[65])
{
    pc_blake3_hex(d, n, out);
}

static void manifest_of(const pc_buf *bundle, pc_buf *out)
{
    pc_zip_reader r;
    pc_buf_truncate(out, 0);
    if (pc_zip_reader_open(&r, bundle->data, bundle->len, NULL) != PC_OK) {
        PC_T_FAIL("unreadable bundle");
        return;
    }
    CHECK(pc_zip_read_by_name(&r, PC_FORMAT_MANIFEST, (size_t)1 << 30, out, NULL) == PC_OK);
    pc_zip_reader_close(&r);
}

/* `ok:` + the manifest the loaded document saves to, or the error (rustref's load_result). */
static void load_result(const uint8_t *b, size_t n, pc_buf *out)
{
    pc_document doc;
    pc_error e;
    pc_buf saved = PC_BUF_INIT;
    pc_status st = pc_format_load_from_bytes(b, n, &doc, &e);
    pc_buf_truncate(out, 0);
    if (st != PC_OK) {
        err_text(out, &e);
        return;
    }
    t_renumber_contents(&doc); /* contents ids minted for old bundles depend on the process */
    st = pc_format_save_to_bytes(&doc, NULL, &saved, &e);
    if (st != PC_OK) {
        (void)pc_buf_append_str(out, "ok", NULL);
        err_text(out, &e);
    } else {
        pc_buf m = PC_BUF_INIT;
        char h[65];
        manifest_of(&saved, &m);
        hex_full(m.data, m.len, h);
        h[16] = '\0';
        (void)pc_buf_appendf(out, NULL, "ok:%s", h);
        pc_buf_fini(&m);
    }
    pc_buf_fini(&saved);
    pc_document_fini(&doc);
}

/* ---- a bundle's entries, for rebuilding it with another manifest ---- */

typedef struct entries {
    char **names;
    pc_buf *data;
    size_t n;
} entries;

static void entries_of(const pc_buf *bundle, entries *out)
{
    pc_zip_reader r;
    memset(out, 0, sizeof *out);
    if (pc_zip_reader_open(&r, bundle->data, bundle->len, NULL) != PC_OK) {
        PC_T_FAIL("unreadable bundle");
        return;
    }
    out->names = calloc(r.count, sizeof *out->names);
    out->data = calloc(r.count, sizeof *out->data);
    for (size_t i = 0; out->names != NULL && out->data != NULL && i < r.count; i++) {
        out->names[i] = pc_strndup(pc_str_c(&r.entries[i].name), r.entries[i].name.len);
        CHECK(pc_zip_read(&r, &r.entries[i], (size_t)1 << 30, &out->data[i], NULL) == PC_OK);
        out->n = i + 1;
    }
    pc_zip_reader_close(&r);
}

static void entries_fini(entries *e)
{
    for (size_t i = 0; i < e->n; i++) {
        free(e->names[i]);
        pc_buf_fini(&e->data[i]);
    }
    free(e->names);
    free(e->data);
}

static void with_manifest(const entries *e, const uint8_t *m, size_t mlen, pc_buf *out)
{
    pc_zip_writer w;
    pc_zip_writer_init(&w);
    for (size_t i = 0; i < e->n; i++) {
        bool is_manifest = strcmp(e->names[i], PC_FORMAT_MANIFEST) == 0;
        CHECK(pc_zip_writer_add(&w, e->names[i], strlen(e->names[i]), is_manifest ? m : e->data[i].data,
                                is_manifest ? mlen : e->data[i].len, NULL) == PC_OK);
    }
    CHECK(pc_zip_writer_finish(&w, out, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
}

/* ---- documents ---- */

static named_doc g_docs[MAX_DOCS];
static size_t g_ndocs;
static pc_buf g_saved[MAX_DOCS];

static const pc_buf *saved_bundle(const char *name)
{
    for (size_t i = 0; i < g_ndocs; i++) {
        if (strcmp(g_docs[i].name, name) == 0) {
            return &g_saved[i];
        }
    }
    return NULL;
}

static const pc_document *doc_named(const char *name)
{
    for (size_t i = 0; i < g_ndocs; i++) {
        if (strcmp(g_docs[i].name, name) == 0) {
            return &g_docs[i].doc;
        }
    }
    return NULL;
}

static void stats_text(const pc_format_save_stats *s, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%zu,%zu,%zu,%zu,%zu,%zu,%zu", s->tiles_total, s->tiles_written, s->tiles_reused,
                   s->blobs_total, s->blobs_written, s->objects_removed, s->manifest_bytes);
}

static void test_documents(void)
{
    const char *xdir = getenv("PC_FORMAT2_XDIR");
    g_ndocs = pd_docs(g_docs);
    CHECK_EQ_UINT(g_ndocs, 25);
    for (size_t i = 0; i < g_ndocs; i++) {
        const char *name = g_docs[i].name;
        pc_pcraft_writer *w = NULL;
        pc_format_save_stats st;
        pc_buf m = PC_BUF_INIT;
        pc_buf names = PC_BUF_INIT;
        pc_buf res = PC_BUF_INIT;
        pc_document back;
        pc_error e;
        pc_zip_reader r;
        size_t nobj = 0;
        char h[65];
        char text[256];
        const line *l;
        CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
        if (pc_pcraft_writer_save_zip(w, &g_docs[i].doc, NULL, &g_saved[i], &st, &e) != PC_OK) {
            PC_T_FAIL("%s: save failed: %s", name, e.msg);
            pc_pcraft_writer_free(w);
            continue;
        }
        pc_pcraft_writer_free(w);
        manifest_of(&g_saved[i], &m);
        hex_full(m.data, m.len, h);
        (void)snprintf(text, sizeof text, "%zu", m.len);
        if ((l = find("man", name)) != NULL && l->n >= 4) {
            if (strcmp(text, l->f[2]) != 0 || strcmp(h, l->f[3]) != 0) {
                PC_T_FAIL("%s: manifest %s bytes %s, reference %s bytes %s", name, text, h, l->f[2], l->f[3]);
            }
            g_checked[K_MAN]++;
        } else {
            PC_T_FAIL("%s: no man line", name);
        }
        if (pc_zip_reader_open(&r, g_saved[i].data, g_saved[i].len, NULL) == PC_OK) {
            for (size_t k = 0; k < r.count; k++) {
                if (pc_str_eq_c(&r.entries[k].name, PC_FORMAT_MANIFEST)) {
                    continue;
                }
                if (nobj++ > 0) {
                    (void)pc_buf_append_str(&names, ",", NULL);
                }
                (void)pc_buf_append(&names, pc_str_c(&r.entries[k].name), r.entries[k].name.len, NULL);
            }
            pc_zip_reader_close(&r);
        }
        hex_full(names.data, names.len, h);
        if ((l = find("objs", name)) != NULL && l->n >= 4) {
            CHECK_EQ_UINT(nobj, strtoull(l->f[2], NULL, 10));
            CHECK_EQ_STR(h, l->f[3]);
            g_checked[K_OBJS]++;
        }
        stats_text(&st, text, sizeof text);
        if ((l = find("stats", name)) != NULL) {
            CHECK_EQ_STR(text, l->f[2]);
            g_checked[K_STATS]++;
        }
        if ((l = find("rt", name)) != NULL) {
            pc_status ls = pc_format_load_from_bytes(g_saved[i].data, g_saved[i].len, &back, &e);
            if (ls == PC_OK) {
                check_text(name, pc_document_eq(&back, &g_docs[i].doc) ? "equal" : "DIFFERENT", l->f[2]);
                pc_document_fini(&back);
            } else {
                pc_buf_truncate(&res, 0);
                err_text(&res, &e);
                check_text(name, pc_buf_cstr(&res), l->f[2]);
            }
            g_checked[K_RT]++;
        }
        if ((l = find("rsave", name)) != NULL) {
            load_result(g_saved[i].data, g_saved[i].len, &res);
            check_text(name, pc_buf_cstr(&res), l->f[2]);
            g_checked[K_RSAVE]++;
        }
        if (xdir != NULL) {
            char path[4096];
            pc_pcraft_writer *wx = NULL;
            if (snprintf(path, sizeof path, "%s/%s.pcraft", xdir, name) >= (int)sizeof path) {
                PC_T_FAIL("path too long");
                continue;
            }
            CHECK(t_write_file(path, g_saved[i].data, g_saved[i].len) == 0);
            if (snprintf(path, sizeof path, "%s/%s.dir", xdir, name) >= (int)sizeof path) {
                PC_T_FAIL("path too long");
                continue;
            }
            CHECK(pc_pcraft_writer_new(&wx, NULL) == PC_OK);
            CHECK(pc_pcraft_writer_save_dir(wx, &g_docs[i].doc, path, NULL, NULL, NULL) == PC_OK);
            pc_pcraft_writer_free(wx);
        }
        pc_buf_fini(&m);
        pc_buf_fini(&names);
        pc_buf_fini(&res);
    }
}

static void test_refused_saves(void)
{
    named_doc bad[8];
    size_t n = pd_bad_docs(bad);
    for (size_t i = 0; i < n; i++) {
        const line *l = find("saveerr", bad[i].name);
        pc_buf out = PC_BUF_INIT;
        pc_buf got = PC_BUF_INIT;
        pc_error e;
        pc_status st = pc_format_save_to_bytes(&bad[i].doc, NULL, &out, &e);
        if (st == PC_OK) {
            (void)pc_buf_append_str(&got, "ok", NULL);
        } else {
            err_text(&got, &e);
        }
        if (l != NULL) {
            check_text(bad[i].name, pc_buf_cstr(&got), l->f[2]);
            g_checked[K_SAVEERR]++;
        }
        if (st != PC_OK && strstr(bad[i].name, "deep") == NULL) {
            CHECK(pc_format_error_kind(st, &e) == PC_FORMAT_ERR_NON_FINITE);
        }
        pc_buf_fini(&out);
        pc_buf_fini(&got);
        pc_document_fini(&bad[i].doc);
    }
}

/* Rust-written bundles load in C. */
static void test_rust_bundles(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const line *l = &g_lines[i];
        const pc_document *want;
        const line *rs;
        pc_buf b = PC_BUF_INIT;
        pc_buf res = PC_BUF_INIT;
        pc_document back;
        pc_error e;
        if (strcmp(l->f[0], "bundle") != 0 || l->n < 4) {
            continue;
        }
        want = doc_named(l->f[1]);
        CHECK(pc_base64_decode(l->f[3], strlen(l->f[3]), &b, NULL) == PC_OK);
        CHECK_EQ_UINT(b.len, strtoull(l->f[2], NULL, 10));
        if (pc_format_load_from_bytes(b.data, b.len, &back, &e) != PC_OK) {
            PC_T_FAIL("%s: Rust bundle does not load: %s", l->f[1], e.msg);
        } else {
            CHECK(want != NULL && pc_document_eq(&back, want));
            pc_document_fini(&back);
        }
        if ((rs = find("rsave", l->f[1])) != NULL) {
            load_result(b.data, b.len, &res);
            check_text(l->f[1], pc_buf_cstr(&res), rs->f[2]);
        }
        g_checked[K_BUNDLE]++;
        pc_buf_fini(&b);
        pc_buf_fini(&res);
    }
}

/* Lines whose C text is known to differ (REWRITE_PLAN.md §7): the C text, which must keep the
 * reference's error kind. */
typedef struct known_diff {
    const char *name;
    const char *c_text;
} known_diff;

static const known_diff KNOWN[] = {
    /* pc_error messages stop at a NUL (D1-D2) */
    {"hash-nul", "E\"corrupt .pcraft bundle: invalid tile hash "
                 "`0000000000000000000000000000000000000000000000000000000000000000`\""},
};
static size_t g_known;

static bool known(const char *name, const char *got, const char *want)
{
    for (size_t i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++) {
        if (strcmp(KNOWN[i].name, name) == 0) {
            const char *colon = strchr(want, ':');
            CHECK_EQ_STR(got, KNOWN[i].c_text);
            CHECK(colon != NULL && strncmp(got, want, (size_t)(colon - want)) == 0);
            g_known++;
            return true;
        }
    }
    return false;
}

static void test_hostile(void)
{
    const pc_buf *small = saved_bundle("small");
    entries ents;
    if (small == NULL) {
        PC_T_FAIL("no small bundle");
        return;
    }
    entries_of(small, &ents);
    for (size_t i = 0; i < g_nlines; i++) {
        const line *l = &g_lines[i];
        pc_buf m = PC_BUF_INIT;
        pc_buf b = PC_BUF_INIT;
        pc_buf res = PC_BUF_INIT;
        if (strcmp(l->f[0], "xh") != 0 || l->n < 4) {
            continue;
        }
        CHECK(pc_base64_decode(l->f[2], strlen(l->f[2]), &m, NULL) == PC_OK);
        with_manifest(&ents, m.data, m.len, &b);
        if (strcmp(l->f[1], "ids-far-ahead") == 0) {
            /* Fresh ids differ per process: only that it loads with the layer remapped. */
            pc_document doc;
            pc_error e;
            if (pc_format_load_from_bytes(b.data, b.len, &doc, &e) == PC_OK) {
                (void)pc_buf_appendf(&res, NULL, "ok:remapped=%s",
                                     doc.nlayers > 0 && doc.layers[0].id != 9000000000000ull ? "true"
                                                                                             : "false");
                pc_document_fini(&doc);
            } else {
                err_text(&res, &e);
            }
        } else {
            load_result(b.data, b.len, &res);
        }
        if (!known(l->f[1], pc_buf_cstr(&res), l->f[3])) {
            check_text(l->f[1], pc_buf_cstr(&res), l->f[3]);
        }
        g_checked[K_XH]++;
        pc_buf_fini(&m);
        pc_buf_fini(&b);
        pc_buf_fini(&res);
    }
    entries_fini(&ents);
}

/* ---- the mutation sweep ---- */

/* The value at an RFC 6901 pointer (keys unescaped from ~1 and ~0), and its parent. */
static yyjson_mut_val *resolve(yyjson_mut_val *root, const char *ptr, yyjson_mut_val **parent, char *last,
                               size_t cap)
{
    yyjson_mut_val *cur = root;
    const char *p = ptr;
    *parent = NULL;
    while (*p == '/') {
        const char *seg = p + 1;
        const char *end = strchr(seg, '/');
        size_t n = end != NULL ? (size_t)(end - seg) : strlen(seg);
        size_t k = 0;
        for (size_t i = 0; i < n && k + 1 < cap; i++) {
            if (seg[i] == '~' && i + 1 < n) {
                last[k++] = seg[i + 1] == '1' ? '/' : '~';
                i++;
            } else {
                last[k++] = seg[i];
            }
        }
        last[k] = '\0';
        *parent = cur;
        if (yyjson_mut_is_obj(cur)) {
            cur = yyjson_mut_obj_getn(cur, last, k);
        } else if (yyjson_mut_is_arr(cur)) {
            cur = yyjson_mut_arr_get(cur, strtoul(last, NULL, 10));
        } else {
            return NULL;
        }
        if (cur == NULL) {
            return NULL;
        }
        p = seg + n;
    }
    return cur;
}

static yyjson_mut_val *op_value(yyjson_mut_doc *d, const char *op, yyjson_mut_val *old)
{
    if (strcmp(op, "null") == 0) {
        return yyjson_mut_null(d);
    }
    if (strcmp(op, "str") == 0) {
        return yyjson_mut_str(d, "x");
    }
    if (strcmp(op, "neg") == 0) {
        return yyjson_mut_sint(d, -1);
    }
    if (strcmp(op, "flt") == 0) {
        return yyjson_mut_real(d, 1.5);
    }
    if (strcmp(op, "big") == 0) {
        return yyjson_mut_real(d, 1e300);
    }
    if (strcmp(op, "int") == 0) {
        return yyjson_mut_is_bool(old) ? yyjson_mut_uint(d, 1) : yyjson_mut_uint(d, 7);
    }
    if (strcmp(op, "estr") == 0) {
        return yyjson_mut_str(d, "");
    }
    if (strcmp(op, "obj") == 0 || strcmp(op, "eobj") == 0) {
        return yyjson_mut_obj(d);
    }
    if (strcmp(op, "arr") == 0 || strcmp(op, "earr") == 0) {
        return yyjson_mut_arr(d);
    }
    if (strcmp(op, "dup") == 0) {
        yyjson_mut_val *copy = yyjson_mut_val_mut_copy(d, old);
        yyjson_mut_val *first = copy != NULL ? yyjson_mut_arr_get_first(copy) : NULL;
        if (first != NULL) {
            (void)yyjson_mut_arr_append(copy, yyjson_mut_val_mut_copy(d, first));
        }
        return copy;
    }
    return NULL;
}

static void test_sweep(void)
{
    const pc_buf *bundle = saved_bundle("rich-gray-u8");
    pc_buf base = PC_BUF_INIT;
    entries ents;
    yyjson_doc *parsed = NULL;
    if (bundle == NULL) {
        PC_T_FAIL("no sweep bundle");
        return;
    }
    manifest_of(bundle, &base);
    entries_of(bundle, &ents);
    CHECK(pc_json_parse((const char *)base.data, base.len, &parsed, NULL) == PC_OK);
    for (size_t i = 0; parsed != NULL && i < g_nlines; i++) {
        const line *l = &g_lines[i];
        yyjson_mut_doc *d;
        yyjson_mut_val *parent;
        yyjson_mut_val *target;
        char last[256];
        pc_buf text = PC_BUF_INIT;
        pc_buf b = PC_BUF_INIT;
        pc_buf res = PC_BUF_INIT;
        bool ok = false;
        if (strcmp(l->f[0], "xm") != 0 || l->n < 4) {
            continue;
        }
        d = yyjson_doc_mut_copy(parsed, NULL);
        target = d != NULL ? resolve(yyjson_mut_doc_get_root(d), l->f[1], &parent, last, sizeof last) : NULL;
        if (target != NULL && parent != NULL) {
            if (strcmp(l->f[2], "del") == 0) {
                ok = yyjson_mut_is_obj(parent)
                         ? yyjson_mut_obj_remove_keyn(parent, last, strlen(last)) != NULL
                         : yyjson_mut_arr_remove(parent, strtoul(last, NULL, 10)) != NULL;
            } else {
                yyjson_mut_val *nv = op_value(d, l->f[2], target);
                if (nv != NULL && yyjson_mut_is_obj(parent)) {
                    ok = yyjson_mut_obj_put(parent, yyjson_mut_strcpy(d, last), nv);
                } else if (nv != NULL) {
                    ok = yyjson_mut_arr_replace(parent, strtoul(last, NULL, 10), nv) != NULL;
                }
            }
        }
        if (!ok || pc_json_write(yyjson_mut_doc_get_root(d), 0, &text, NULL) != PC_OK) {
            PC_T_FAIL("xm %s %s: cannot apply", l->f[1], l->f[2]);
        } else {
            with_manifest(&ents, text.data, text.len, &b);
            load_result(b.data, b.len, &res);
            check_text(l->f[1], pc_buf_cstr(&res), l->f[3]);
        }
        g_checked[K_XM]++;
        yyjson_mut_doc_free(d);
        pc_buf_fini(&text);
        pc_buf_fini(&b);
        pc_buf_fini(&res);
    }
    yyjson_doc_free(parsed);
    entries_fini(&ents);
    pc_buf_fini(&base);
}

/* ---- autosave ---- */

/* Decode a Rust `{:?}` string (the escapes key lines use) into out. */
static size_t undq(const char *s, char *out, size_t cap)
{
    size_t k = 0;
    if (*s == '"') {
        s++;
    }
    while (*s != '\0' && *s != '"' && k + 1 < cap) {
        if (*s == '\\' && s[1] != '\0') {
            s++;
            switch (*s) {
            case 'n':
                out[k++] = '\n';
                break;
            case 't':
                out[k++] = '\t';
                break;
            case 'r':
                out[k++] = '\r';
                break;
            case '0':
                out[k++] = '\0';
                break;
            default:
                out[k++] = *s;
                break;
            }
            s++;
        } else {
            out[k++] = *s++;
        }
    }
    out[k] = '\0';
    return k;
}

static void test_autosave_keys_and_info(const char *tmp)
{
    pc_recovery_info info;
    size_t ninfo = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        const line *l = &g_lines[i];
        if (strcmp(l->f[0], "key") == 0 && l->n >= 3) {
            char key[128];
            size_t n = undq(l->f[1], key, sizeof key);
            pc_autosaver *a = NULL;
            pc_buf got = PC_BUF_INIT;
            CHECK(pc_autosaver_new(tmp, key, n, &a, NULL) == PC_OK);
            if (a != NULL) {
                const char *p = pc_autosaver_bundle_path(a);
                const char *slash = strrchr(p, '/');
                const char *base = slash != NULL ? slash + 1 : p;
                dq(&got, base, strlen(base));
                CHECK_EQ_STR(pc_buf_cstr(&got), l->f[2]);
            }
            pc_autosaver_free(a);
            pc_buf_fini(&got);
            g_checked[K_KEY]++;
        } else if (strcmp(l->f[0], "info") == 0 && l->n >= 2) {
            pc_buf json = PC_BUF_INIT;
            pc_buf b64 = PC_BUF_INIT;
            memset(&info, 0, sizeof info);
            if (ninfo == 0) {
                t_str(&info.key, "doc-x_1");
                t_str(&info.document_name, "\xc3\x9c"
                                           "n\xc3\xaf"
                                           "code \"name\"");
                info.has_original_path = true;
                t_str(&info.original_path, "/work/a.pcraft");
                info.saved_at = 1700000000u;
                info.revision = 42;
            } else {
                t_str(&info.key, "k");
                info.saved_at = 0;
                info.revision = UINT64_MAX;
            }
            CHECK(pc_recovery_info_to_json(&info, &json, NULL) == PC_OK);
            CHECK(pc_base64_encode(json.data, json.len, &b64, NULL) == PC_OK);
            CHECK_EQ_STR(pc_buf_cstr(&b64), l->f[1]);
            pc_recovery_info_fini(&info);
            pc_buf_fini(&json);
            pc_buf_fini(&b64);
            ninfo++;
            g_checked[K_INFO]++;
        }
    }
}

typedef struct list_file {
    const char *name; /* "B:<key>" bundle with a manifest, "D:<path>" directory, else a file */
    const char *content;
} list_file;

#define SIDE(key, at)                                                                                        \
    "{\"key\":\"" key "\",\"document_name\":\"n\",\"original_path\":null,\"saved_at\":" at ",\"revision\":"  \
                                                                                           "1}"

typedef struct list_case {
    const char *name;
    list_file files[7];
} list_case;

static const list_case LIST_CASES[] = {
    {"order",
     {{"a.json", SIDE("a", "5")},
      {"b.json", SIDE("b", "9")},
      {"c.json", SIDE("c", "5")},
      {"B:a", ""},
      {"B:b", ""},
      {"B:c", ""}}},
    {"name-mismatch", {{"copy.json", SIDE("k", "1")}, {"B:k", ""}}},
    {"escape", {{"escape.json", SIDE("../k", "1")}, {"B:k", ""}}},
    {"no-bundle", {{"k.json", SIDE("k", "1")}}},
    {"bundle-no-manifest", {{"k.json", SIDE("k", "1")}, {"D:k.pcraft", ""}}},
    {"manifest-dir", {{"k.json", SIDE("k", "1")}, {"D:k.pcraft/manifest.json", ""}}},
    {"junk", {{"bogus.json", "{}"}, {"other.txt", "x"}, {"x.json", "not json"}}},
    {"dot-json", {{".json", SIDE("", "1")}, {"B:", ""}}},
    {"upper-ext", {{"k.JSON", SIDE("k", "1")}, {"B:k", ""}}},
    {"extra-fields",
     {{"k.json",
       "{\"key\":\"k\",\"document_name\":\"n\",\"original_path\":\"/p\",\"saved_at\":3,\"revision\":2,"
       "\"more\":[1]}"},
      {"B:k", ""}}},
    {"missing-field",
     {{"k.json", "{\"key\":\"k\",\"document_name\":\"n\",\"saved_at\":3,\"revision\":2}"}, {"B:k", ""}}},
    {"bad-types",
     {{"k.json",
       "{\"key\":\"k\",\"document_name\":\"n\",\"original_path\":null,\"saved_at\":-3,\"revision\":2}"},
      {"B:k", ""}}},
    {"trailing", {{"k.json", SIDE("k", "1") " x"}, {"B:k", ""}}},
    {"trailing-ws", {{"k.json", SIDE("k", "1") "\n\t "}, {"B:k", ""}}},
    {"unsanitary-key", {{"a b.json", SIDE("a b", "1")}, {"B:a b", ""}}},
    {"ties",
     {{"z.json", SIDE("z", "4")},
      {"y.json", SIDE("y", "4")},
      {"x.json", SIDE("x", "4")},
      {"B:x", ""},
      {"B:y", ""},
      {"B:z", ""}}},
    {"dir-named-json", {{"D:k.json", ""}, {"B:k", ""}}},
    {"deep-json", {{"k.json", NULL}, {"B:k", ""}}},
};

static void make_files(const char *dir, const list_case *c)
{
    for (size_t k = 0; k < 7 && c->files[k].name != NULL; k++) {
        const list_file *f = &c->files[k];
        char path[1200];
        if (strncmp(f->name, "B:", 2) == 0) {
            (void)snprintf(path, sizeof path, "%s/%s.pcraft", dir, f->name + 2);
            CHECK(pc_mkdir_p(path, NULL) == PC_OK);
            (void)snprintf(path, sizeof path, "%s/%s.pcraft/manifest.json", dir, f->name + 2);
            CHECK(t_write_file(path, "{}", 2) == 0);
        } else if (strncmp(f->name, "D:", 2) == 0) {
            (void)snprintf(path, sizeof path, "%s/%s", dir, f->name + 2);
            CHECK(pc_mkdir_p(path, NULL) == PC_OK);
        } else if (f->content == NULL) {
            char deep[401];
            memset(deep, '[', 200);
            memset(deep + 200, ']', 200);
            deep[400] = '\0';
            (void)snprintf(path, sizeof path, "%s/%s", dir, f->name);
            CHECK(t_write_file(path, deep, 400) == 0);
        } else {
            (void)snprintf(path, sizeof path, "%s/%s", dir, f->name);
            CHECK(t_write_file(path, f->content, strlen(f->content)) == 0);
        }
    }
}

static void test_list_recovery(const char *tmp)
{
    for (size_t i = 0; i < sizeof LIST_CASES / sizeof LIST_CASES[0]; i++) {
        const list_case *c = &LIST_CASES[i];
        const line *l = find("list", c->name);
        char dir[1100];
        pc_recovery_entry *e = NULL;
        size_t n = 0;
        pc_buf got = PC_BUF_INIT;
        (void)snprintf(dir, sizeof dir, "%s/list-%s", tmp, c->name);
        CHECK(pc_mkdir_p(dir, NULL) == PC_OK);
        make_files(dir, c);
        CHECK(pc_list_recovery(dir, &e, &n, NULL) == PC_OK);
        (void)pc_buf_append_str(&got, "[", NULL);
        for (size_t k = 0; k < n; k++) {
            if (k > 0) {
                (void)pc_buf_append_str(&got, ",", NULL);
            }
            dq(&got, pc_str_c(&e[k].info.key), e[k].info.key.len);
            (void)pc_buf_appendf(&got, NULL, "@%llu", (unsigned long long)e[k].info.saved_at);
        }
        (void)pc_buf_append_str(&got, "]", NULL);
        if (l != NULL) {
            CHECK_EQ_STR(pc_buf_cstr(&got), l->f[2]);
            g_checked[K_LIST]++;
        } else {
            PC_T_FAIL("no list line for %s", c->name);
        }
        pc_recovery_entries_free(e, n);
        pc_buf_fini(&got);
    }
    {
        char dir[1100];
        pc_recovery_entry *e = NULL;
        size_t n = 7;
        (void)snprintf(dir, sizeof dir, "%s/nope", tmp);
        CHECK(pc_list_recovery(dir, &e, &n, NULL) == PC_OK);
        CHECK(n == 0 && e == NULL);
        CHECK(find("list", "missing-dir") != NULL && strcmp(find("list", "missing-dir")->f[2], "[0]") == 0);
        g_checked[K_LIST]++;
    }
}

static void test_autosave(void)
{
    char tmp[512];
    CHECK(t_mkdtemp("parity2", tmp, sizeof tmp) == 0);
    test_autosave_keys_and_info(tmp);
    test_list_recovery(tmp);
    t_rm_rf(tmp);
}

static void count(void)
{
    static const char *const NAMES[K_COUNT] = {"man",    "objs", "stats", "rt",  "rsave", "saveerr",
                                               "bundle", "xh",   "xm",    "key", "info",  "list"};
    size_t total = 0;
    for (int k = 0; k < K_COUNT; k++) {
        total += g_checked[k];
    }
    (void)printf("golden lines: %zu, checked %zu (", g_nlines, total);
    for (int k = 0; k < K_COUNT; k++) {
        (void)printf("%s%s %zu", k ? ", " : "", NAMES[k], g_checked[k]);
    }
    (void)printf("); error texts compared up to pc_error's 255 bytes: %zu; known differences (§7): %zu\n",
                 g_cut, g_known);
    CHECK_EQ_UINT(total, g_nlines);
}

int main(int argc, char **argv)
{
    if (load_golden() != 0) {
        (void)fprintf(stderr, "cannot read %s\n", GOLDEN);
        return 1;
    }
    RUN(test_documents);
    RUN(test_refused_saves);
    RUN(test_rust_bundles);
    RUN(test_hostile);
    RUN(test_sweep);
    RUN(test_autosave);
    RUN(count);
    for (size_t i = 0; i < g_ndocs; i++) {
        pc_document_fini(&g_docs[i].doc);
        pc_buf_fini(&g_saved[i]);
    }
    free(g_lines);
    free(g_text);
    return pc_t_finish();
}
