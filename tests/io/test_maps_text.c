/* Type layers through whole PSD files (ports of crates/io/tests/text_styles.rs, text_corpus.rs,
 * psd_kerning.rs, psd_vertical_type.rs and psd_type_bounds.rs): named styles, the TySh model and
 * its byte-exact round trip, kerning, vertical type, imported geometry and the type-layer
 * rendering of chunk I4's text_import. Bundled fonts only (PHOTOCRAFT_NO_SYSTEM_FONTS) except
 * where a test asks for the system fonts; corpus tests skip without corpus/. */
#include "harness.h"

#include <dirent.h>
#include <float.h>
#include <strings.h>

#include "base/num.h"
#include "compose/compose.h"
#include "io/maps_desc.h"
#include "maps.h"
#include "psd/file.h"
#include "text/engine.h"
#include "text/engine_data.h"

/* ---------------------------------------------------------------- helpers */

static pc_buf export_psd(const pc_document *d, pc_io_warnings *w)
{
    pc_io_export_result r;
    pc_buf b = PC_BUF_INIT;
    must(pc_io_export(d, "t.psd", NULL, &r, NULL));
    must(pc_buf_append(&b, r.bytes, r.len, NULL));
    if (w != NULL) {
        must(pc_io_warnings_append(w, &r.warnings, NULL));
    }
    pc_io_export_result_fini(&r);
    return b;
}

static pc_document import_psd(const pc_buf *b)
{
    pc_io_import_result r;
    pc_document d;
    must(pc_io_import("t.psd", b->data, b->len, &r, NULL));
    d = r.document;
    memset(&r.document, 0, sizeof r.document);
    pc_io_import_result_fini(&r);
    return d;
}

static pc_document psd_roundtrip(const pc_document *d)
{
    pc_buf b = export_psd(d, NULL);
    pc_document out = import_psd(&b);
    pc_buf_fini(&b);
    return out;
}

static const pc_text_layer *text0(const pc_document *d)
{
    return d->layers[0].content.kind == PC_LAYER_TEXT ? &d->layers[0].content.u.text : NULL;
}

static pc_text_engine *engine(void)
{
    pc_text_engine *e = NULL;
    must(pc_text_engine_new(&e, NULL));
    return e;
}

static void render_layer(pc_text_layer *t, float dpi, pc_pixel_format fmt)
{
    pc_text_engine *e = engine();
    pc_rect r;
    must(pc_text_engine_render_layer(e, t, dpi, fmt, &r, NULL));
    pc_text_engine_free(e);
}

static pc_char_style inter(float size)
{
    pc_char_style s;
    pc_char_style_default(&s);
    must(pc_str_set_c(&s.font_family, "Inter", NULL));
    s.size_pt = size;
    return s;
}

/* ---------------------------------------------------------------- text_styles.rs */

static pc_document styles_doc(const pc_text_styles *st, bool link)
{
    pc_document doc = t_doc("t", 200, 80, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_text_layer t;
    pc_char_style base = inter(12.0f);
    static const uint32_t three = 3;
    static const uint32_t one = 1;
    pc_layer l;
    pc_text_layer_default(&t);
    must(pc_str_set_c(&t.text, "calm LOUD", NULL));
    t.runs = xmalloc(2 * sizeof *t.runs);
    t.nruns = 2;
    t.runs[0].len = 5;
    t.runs[1].len = 4;
    must(pc_char_style_clone(&base, &t.runs[0].style, NULL));
    if (link) {
        must(pc_text_styles_resolve_char(st, NULL, &three, "Inter", &t.runs[1].style, NULL));
    } else {
        must(pc_char_style_clone(&base, &t.runs[1].style, NULL));
    }
    t.paragraphs = xmalloc(sizeof *t.paragraphs);
    t.nparagraphs = 1;
    t.paragraphs[0].len = 9;
    if (link) {
        must(pc_text_styles_resolve_para(st, &one, &t.paragraphs[0].style, NULL));
    } else {
        t.paragraphs[0].style = pc_paragraph_style_default();
    }
    t.transform = pc_affine_translate(4.0, 40.0);
    must(pc_text_layer_sync_summary(&t, NULL));
    render_layer(&t, doc.resolution_dpi, pc_document_pixel_format(&doc));
    l = m_text("t", &t);
    t_push(&doc, &l);
    pc_text_styles_fini(&doc.text_styles);
    must(pc_text_styles_clone(st, &doc.text_styles, NULL));
    pc_char_style_fini(&base);
    return doc;
}

/* text_styles.rs's styles(): Loud (id 3) and Middle (id 1) only. */
static pc_text_styles two_styles(void)
{
    pc_text_styles s = m_styles();
    pc_character_style_def_fini(&s.character[1]);
    s.ncharacter = 1;
    return s;
}

static void test_named_styles_round_trip_through_psd(void)
{
    pc_text_styles st = two_styles();
    pc_document doc = styles_doc(&st, true);
    pc_document back = psd_roundtrip(&doc);
    const pc_text_styles *bs = &back.text_styles;
    const pc_text_layer *t = text0(&back);
    pc_text_run *runs = NULL;
    size_t n = 0;
    pc_paragraph_run *pr = NULL;
    size_t np = 0;
    yyjson_mut_val *v;
    double size = 0.0;
    CHECK(bs->ncharacter == 1 && pc_str_eq_c(&bs->character[0].name, "Loud"));
    v = pc_style_attrs_get(&bs->character[0].attrs, "size_pt");
    CHECK(v != NULL && yyjson_mut_is_num(v) && (size = yyjson_mut_get_num(v)) == 30.0);
    v = pc_style_attrs_get(&bs->character[0].attrs, "underline");
    CHECK(v != NULL && yyjson_mut_is_true(v));
    CHECK(bs->nparagraph == 1 && pc_str_eq_c(&bs->paragraph[0].name, "Middle"));
    v = pc_style_attrs_get(&bs->paragraph[0].para_attrs, "align");
    CHECK(v != NULL && yyjson_mut_is_str(v) && strcmp(yyjson_mut_get_str(v), "Center") == 0);
    must(pc_text_layer_char_runs(t, &runs, &n, NULL));
    CHECK(n == 2 && !runs[0].style.has_style_sheet && runs[1].style.has_style_sheet &&
          runs[1].style.style_sheet == bs->character[0].id);
    pc_text_runs_free(runs, n);
    must(pc_text_layer_paragraph_runs(t, &pr, &np, NULL));
    CHECK(np == 1 && pr[0].style.has_style_sheet && pr[0].style.style_sheet == bs->paragraph[0].id);
    free(pr);
    {
        pc_document back2 = psd_roundtrip(&back);
        CHECK(back2.text_styles.ncharacter == 1 && back2.text_styles.nparagraph == 1);
        pc_document_fini(&back2);
    }
    pc_document_fini(&back);
    pc_document_fini(&doc);
    pc_text_styles_fini(&st);
}

static void test_documents_without_styles_keep_their_type_data(void)
{
    pc_text_styles none;
    pc_text_styles st = two_styles();
    pc_document doc;
    pc_document back;
    pc_document back2;
    must(pc_text_styles_default(&none, NULL));
    doc = styles_doc(&none, false);
    back = psd_roundtrip(&doc);
    CHECK(pc_text_styles_is_empty(&back.text_styles));
    /* Re-export of an imported file without styles keeps TySh byte-exact. */
    back2 = psd_roundtrip(&back);
    CHECK(text0(&back2)->psd_raw != NULL && pc_blob_eq(text0(&back2)->psd_raw, text0(&back)->psd_raw));
    pc_document_fini(&back2);
    pc_document_fini(&back);
    pc_document_fini(&doc);
    /* Deleting every style removes the named sheets on the next save. */
    doc = styles_doc(&st, true);
    back = psd_roundtrip(&doc);
    pc_text_styles_fini(&back.text_styles);
    must(pc_text_styles_default(&back.text_styles, NULL));
    back2 = psd_roundtrip(&back);
    CHECK(back2.text_styles.ncharacter == 0 && back2.text_styles.nparagraph == 0);
    pc_document_fini(&back2);
    pc_document_fini(&back);
    pc_document_fini(&doc);
    pc_text_styles_fini(&st);
    pc_text_styles_fini(&none);
}

/* ---------------------------------------------------------------- text_corpus.rs */

static void corpus_files(const char *dir, char ***out, size_t *n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char path[1024];
        struct stat st;
        size_t len = strlen(e->d_name);
        if (e->d_name[0] == '.') {
            continue;
        }
        (void)snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            corpus_files(path, out, n);
        } else if (len > 4 && (strcasecmp(e->d_name + len - 4, ".psd") == 0 ||
                               strcasecmp(e->d_name + len - 4, ".psb") == 0)) {
            *out = realloc(*out, (*n + 1) * sizeof **out);
            (*out)[(*n)++] = pc_strndup(path, strlen(path));
        }
    }
    (void)closedir(d);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool has_bytes(const pc_buf *b, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= b->len; i++) {
        if (memcmp(b->data + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool runs_eq(const pc_text_layer *a, const pc_text_layer *b)
{
    pc_text_run *x = NULL;
    pc_text_run *y = NULL;
    size_t nx = 0;
    size_t ny = 0;
    pc_paragraph_run *p = NULL;
    pc_paragraph_run *q = NULL;
    size_t np = 0;
    size_t nq = 0;
    bool eq;
    must(pc_text_layer_char_runs(a, &x, &nx, NULL));
    must(pc_text_layer_char_runs(b, &y, &ny, NULL));
    must(pc_text_layer_paragraph_runs(a, &p, &np, NULL));
    must(pc_text_layer_paragraph_runs(b, &q, &nq, NULL));
    eq = nx == ny && np == nq;
    for (size_t i = 0; eq && i < nx; i++) {
        eq = x[i].len == y[i].len && pc_char_style_eq(&x[i].style, &y[i].style);
    }
    for (size_t i = 0; eq && i < np; i++) {
        eq = p[i].len == q[i].len && pc_paragraph_style_eq(&p[i].style, &q[i].style);
    }
    pc_text_runs_free(x, nx);
    pc_text_runs_free(y, ny);
    free(p);
    free(q);
    return eq;
}

/* Every corpus type layer parses, and the model survives our own TySh writer (with Txt2). The
 * geometry comparison with Photoshop's pixels of the reference is a report there; here only the
 * model checks are asserted. */
static void test_corpus_text_layers(void)
{
    char **files = NULL;
    size_t nf = 0;
    size_t n = 0;
    corpus_files("corpus/psd", &files, &nf);
    if (nf == 0) {
        (void)printf("  (corpus missing: text corpus skipped)\n");
        return;
    }
    qsort(files, nf, sizeof *files, cmp_str);
    for (size_t i = 0; i < nf; i++) {
        pc_buf bytes = PC_BUF_INIT;
        pc_io_import_result imp;
        pc_walk w;
        pc_ed_value txt2;
        bool has_txt2 = false;
        if (pc_file_read(files[i], (size_t)1 << 32, &bytes, NULL) != PC_OK || !has_bytes(&bytes, "TySh") ||
            pc_io_import(files[i], bytes.data, bytes.len, &imp, NULL) != PC_OK) {
            pc_buf_fini(&bytes);
            continue;
        }
        for (size_t k = 0; k < imp.document.metadata.npsd_global_blocks && !has_txt2; k++) {
            const pc_doc_psd_global_block *g = &imp.document.metadata.psd_global_blocks[k];
            if (memcmp(g->key, "Txt2", 4) == 0) {
                has_txt2 = pc_txt2_parse(pc_blob_data(g->data), pc_blob_len(g->data), &txt2, NULL) == PC_OK;
            }
        }
        must(pc_document_walk(&imp.document, &w, NULL));
        for (size_t k = 0; k < w.len; k++) {
            const pc_layer *l = w.items[k].layer;
            const pc_text_layer *t;
            size_t total = 0;
            pc_psd_bytes rebuilt;
            pc_text_layer back;
            if (l->content.kind != PC_LAYER_TEXT) {
                continue;
            }
            t = &l->content.u.text;
            n++;
            CHECK(t->psd_raw != NULL);
            CHECK(t->nruns > 0);
            for (size_t r = 0; r < t->nruns; r++) {
                total += t->runs[r].len;
            }
            CHECK((total < t->text.len ? total : t->text.len) == t->text.len);
            CHECK(t->nruns > 0 && t->runs[0].style.size_pt > 0.0f && t->runs[0].style.has_postscript_name);
            must(pc_text_build_tysh(t, imp.document.resolution_dpi, NULL, &rebuilt, NULL));
            must(
                pc_text_layer_from_tysh(rebuilt.data, rebuilt.len, imp.document.resolution_dpi, &back, NULL));
            if (has_txt2) {
                must(pc_txt2_apply(&back, rebuilt.data, rebuilt.len, &txt2, NULL));
            }
            CHECK(pc_str_eq(&back.text, &t->text));
            if (!runs_eq(&back, t)) {
                PC_T_FAIL("%s / %s: runs differ after our TySh writer", files[i], pc_str_c(&l->name));
            }
            CHECK(back.shape.is_box == t->shape.is_box && back.shape.x == t->shape.x &&
                  back.shape.width == t->shape.width && back.shape.height == t->shape.height);
            pc_text_layer_fini(&back);
            pc_psd_bytes_fini(&rebuilt);
        }
        pc_walk_fini(&w);
        if (has_txt2) {
            pc_ed_fini(&txt2);
        }
        pc_io_import_result_fini(&imp);
        pc_buf_fini(&bytes);
    }
    (void)printf("  text layers: %zu\n", n);
    for (size_t i = 0; i < nf; i++) {
        free(files[i]);
    }
    free(files);
}

static bool find_tysh(const uint8_t *b, size_t len, const uint8_t **out, size_t *n)
{
    for (size_t i = 0; i + 12 <= len; i++) {
        if (memcmp(b + i, "8BIMTySh", 8) == 0) {
            size_t l = (size_t)b[i + 8] << 24 | (size_t)b[i + 9] << 16 | (size_t)b[i + 10] << 8 | b[i + 11];
            if (i + 12 + l > len) {
                return false;
            }
            *out = b + i + 12;
            *n = l;
            return true;
        }
    }
    return false;
}

static void test_corpus_tysh_lossless(void)
{
    pc_buf bytes = PC_BUF_INIT;
    pc_document doc;
    pc_buf out;
    const uint8_t *a;
    const uint8_t *b;
    size_t na;
    size_t nb;
    if (pc_file_read("corpus/psd/ag-psd/read-write/text/src.psd", (size_t)1 << 30, &bytes, NULL) != PC_OK) {
        (void)printf("  (corpus missing: skipped)\n");
        return;
    }
    doc = import_psd(&bytes);
    out = export_psd(&doc, NULL);
    CHECK(find_tysh(bytes.data, bytes.len, &a, &na) && find_tysh(out.data, out.len, &b, &nb) && na == nb &&
          memcmp(a, b, na) == 0);
    pc_buf_fini(&out);
    pc_document_fini(&doc);
    pc_buf_fini(&bytes);
}

static void test_created_text_layer_roundtrips_through_psd(void)
{
    pc_document doc = t_doc("t", 120, 80, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_text_layer t;
    pc_layer l;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_buf out;
    pc_document back;
    const pc_text_layer *bt;
    pc_text_run *x = NULL;
    pc_text_run *y = NULL;
    size_t nx = 0;
    size_t ny = 0;
    doc.resolution_dpi = 144.0f;
    pc_text_layer_default(&t);
    must(pc_str_set_c(&t.text,
                      "Gr\xC3\xB6\xC3\x9F"
                      "e\nzwei",
                      NULL));
    t.runs = xmalloc(2 * sizeof *t.runs);
    t.nruns = 2;
    t.runs[0].len = 3;
    t.runs[0].style = inter(10.0f);
    t.runs[0].style.color = pc_color_rgb(0.2f, 0.4f, 0.6f);
    t.runs[1].len = 9;
    must(pc_char_style_clone(&t.runs[0].style, &t.runs[1].style, NULL));
    t.runs[1].style.faux_bold = true;
    t.runs[1].style.underline = true;
    t.runs[1].style.tracking = 50.0f;
    t.runs[1].style.has_leading = true;
    t.runs[1].style.leading_pt = 14.0f;
    t.paragraphs = xmalloc(2 * sizeof *t.paragraphs);
    t.nparagraphs = 2;
    t.paragraphs[0].len = 7;
    t.paragraphs[0].style = pc_paragraph_style_default();
    t.paragraphs[0].style.align = PC_TEXT_ALIGN_CENTER;
    t.paragraphs[0].style.space_after_pt = 3.0f;
    t.paragraphs[1].len = 5;
    t.paragraphs[1].style = pc_paragraph_style_default();
    t.paragraphs[1].style.align = PC_TEXT_ALIGN_JUSTIFY_ALL;
    t.shape.is_box = true;
    t.shape.width = 100.0f;
    t.shape.height = 60.0f;
    t.transform = pc_affine_translate(8.0, 6.0);
    must(pc_text_layer_sync_summary(&t, NULL));
    render_layer(&t, doc.resolution_dpi, pc_document_pixel_format(&doc));
    {
        pc_text_layer copy;
        must(pc_text_layer_clone(&t, &copy, NULL));
        l = m_text("t", &copy);
        t_push(&doc, &l);
    }
    out = export_psd(&doc, &w);
    CHECK(!pc_io_warnings_any(&w, "text layer"));
    back = import_psd(&out);
    bt = text0(&back);
    CHECK(bt != NULL && pc_str_eq(&bt->text, &t.text));
    must(pc_text_layer_char_runs(bt, &x, &nx, NULL));
    must(pc_text_layer_char_runs(&t, &y, &ny, NULL));
    CHECK(nx == ny);
    for (size_t i = 0; i < nx && i < ny; i++) {
        x[i].style.has_postscript_name = false;
        pc_str_fini(&x[i].style.postscript_name);
        y[i].style.has_postscript_name = false;
        pc_str_fini(&y[i].style.postscript_name);
        for (size_t c = 0; c < 4; c++) {
            x[i].style.color.c[c] = roundf(x[i].style.color.c[c] * 1000.0f) / 1000.0f;
            y[i].style.color.c[c] = roundf(y[i].style.color.c[c] * 1000.0f) / 1000.0f;
        }
        CHECK(x[i].len == y[i].len && pc_char_style_eq(&x[i].style, &y[i].style));
    }
    pc_text_runs_free(x, nx);
    pc_text_runs_free(y, ny);
    CHECK(bt->shape.is_box && bt->shape.width == 100.0f && bt->shape.height == 60.0f);
    CHECK(memcmp(&bt->transform, &t.transform, sizeof t.transform) == 0);
    CHECK(bt->has_cache && pc_rect_width(pc_surface_content_bounds(&bt->cache)) > 10);
    pc_io_warnings_fini(&w);
    pc_document_fini(&back);
    pc_buf_fini(&out);
    pc_text_layer_fini(&t);
    pc_document_fini(&doc);
}

/* ---------------------------------------------------------------- psd_kerning.rs */

typedef struct kf {
    bool autok;
    int64_t kern;
} kf;

/* (AutoKerning, Kerning) of every UTF-16 unit of the engine text. */
static size_t kerning_fields(const uint8_t *tysh, size_t len, kf *out, size_t cap)
{
    pc_tysh t;
    pc_ed_value e;
    const pc_ed_value *runs;
    const pc_ed_value *lens;
    size_t nr = 0;
    size_t nl = 0;
    size_t k = 0;
    must(pc_tysh_parse(tysh, len, &t, NULL));
    must(pc_tysh_engine_data(&t.text, &e, NULL));
    runs = pc_ed_as_array(pc_ed_path(&e, "EngineDict", "StyleRun", "RunArray", (const char *)NULL), &nr);
    lens =
        pc_ed_as_array(pc_ed_path(&e, "EngineDict", "StyleRun", "RunLengthArray", (const char *)NULL), &nl);
    for (size_t i = 0; runs != NULL && lens != NULL && i < nr && i < nl; i++) {
        const pc_ed_value *d = pc_ed_path(&runs[i], "StyleSheet", "StyleSheetData", (const char *)NULL);
        kf f = {true, 0};
        int64_t n = 0;
        (void)pc_ed_as_bool(pc_ed_get(d, "AutoKerning"), &f.autok);
        (void)pc_ed_as_i64(pc_ed_get(d, "Kerning"), &f.kern);
        (void)pc_ed_as_i64(&lens[i], &n);
        for (int64_t j = 0; j < n && k < cap; j++) {
            out[k++] = f;
        }
    }
    pc_ed_fini(&e);
    pc_tysh_fini(&t);
    return k;
}

static size_t kchars(const pc_text_layer *t, pc_kerning *mode, float *kern, size_t cap)
{
    pc_text_run *runs = NULL;
    size_t n = 0;
    size_t at = 0;
    size_t k = 0;
    must(pc_text_layer_char_runs(t, &runs, &n, NULL));
    for (size_t i = 0; i < n; i++) {
        size_t j = at;
        while (j < at + runs[i].len && j < t->text.len && k < cap) {
            uint32_t cp;
            size_t before = j;
            if (!pc_utf8_next((const uint8_t *)t->text.ptr, t->text.len, &j, &cp)) {
                j = before + 1;
            }
            mode[k] = runs[i].style.kerning;
            kern[k++] = runs[i].style.kern;
        }
        at += runs[i].len;
    }
    pc_text_runs_free(runs, n);
    return k;
}

static pc_text_layer klayer(const char *text, const size_t *lens, const pc_kerning *modes, const float *kerns,
                            size_t n)
{
    pc_text_layer t;
    pc_text_layer_default(&t);
    must(pc_str_set_c(&t.text, text, NULL));
    t.runs = xmalloc(n * sizeof *t.runs);
    t.nruns = n;
    for (size_t i = 0; i < n; i++) {
        t.runs[i].len = lens[i];
        t.runs[i].style = inter(24.0f);
        t.runs[i].style.kerning = modes[i];
        t.runs[i].style.kern = kerns[i];
    }
    must(pc_text_layer_sync_summary(&t, NULL));
    return t;
}

static bool same_chars(const pc_text_layer *a, const pc_text_layer *b)
{
    pc_kerning ma[64];
    pc_kerning mb[64];
    float ka[64];
    float kb[64];
    size_t na = kchars(a, ma, ka, 64);
    size_t nb = kchars(b, mb, kb, 64);
    if (na != nb) {
        return false;
    }
    for (size_t i = 0; i < na; i++) {
        if (ma[i] != mb[i] || ka[i] != kb[i]) {
            return false;
        }
    }
    return true;
}

static void test_manual_and_off_kerning_round_trip_through_psd(void)
{
    static const size_t lens[6] = {1, 2, 1, 2, 3, 2};
    static const pc_kerning modes[6] = {PC_KERNING_OFF,     PC_KERNING_METRICS, PC_KERNING_OFF,
                                        PC_KERNING_METRICS, PC_KERNING_OFF,     PC_KERNING_METRICS};
    static const float kerns[6] = {100.0f, 0.0f, -50.0f, 0.0f, 0.0f, 0.0f};
    pc_text_layer t = klayer("AVATAR Wave", lens, modes, kerns, 6);
    pc_document doc = t_doc("k", 64, 32, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_text_layer t2;
    pc_layer l;
    pc_document back;
    pc_psd_bytes tysh;
    kf f[32];
    size_t n;
    must(pc_text_layer_clone(&t, &t2, NULL));
    render_layer(&t2, doc.resolution_dpi, pc_document_pixel_format(&doc));
    l = m_text("kerned", &t2);
    t_push(&doc, &l);
    back = psd_roundtrip(&doc);
    CHECK(text0(&back) != NULL && same_chars(text0(&back), &t));
    must(pc_text_build_tysh(&t, 72.0f, NULL, &tysh, NULL));
    n = kerning_fields(tysh.data, tysh.len, f, 32);
    CHECK_EQ_UINT(n, 12);
    CHECK(!f[1].autok && f[1].kern == 100);
    CHECK(f[4].autok && f[4].kern == -50);
    for (size_t j = 8; j <= 10; j++) {
        CHECK(!f[j].autok);
    }
    for (size_t j = 0; j < n; j++) {
        if (j != 1 && j != 4 && j != 8 && j != 9 && j != 10) {
            CHECK(f[j].autok && f[j].kern == 0);
        }
    }
    pc_psd_bytes_fini(&tysh);
    pc_document_fini(&back);
    pc_document_fini(&doc);
    pc_text_layer_fini(&t);
}

static void test_hostile_kerning_values(void)
{
    static const size_t lens[2] = {1, 1};
    static const pc_kerning modes[2] = {PC_KERNING_OFF, PC_KERNING_METRICS};
    float kerns[2] = {NAN, INFINITY};
    pc_text_layer t = klayer("AV", lens, modes, kerns, 2);
    pc_psd_bytes tysh;
    kf f[8];
    size_t n;
    pc_text_layer back;
    pc_kerning m[8];
    float k[8];
    must(pc_text_build_tysh(&t, 72.0f, NULL, &tysh, NULL));
    n = kerning_fields(tysh.data, tysh.len, f, 8);
    for (size_t i = 0; i < n; i++) {
        CHECK(f[i].kern == 0);
    }
    must(pc_text_layer_from_tysh(tysh.data, tysh.len, 72.0f, &back, NULL));
    n = kchars(&back, m, k, 8);
    for (size_t i = 0; i < n; i++) {
        CHECK(k[i] == 0.0f);
    }
    pc_text_layer_fini(&back);
    pc_psd_bytes_fini(&tysh);
    pc_text_layer_fini(&t);
    kerns[0] = 1e30f;
    kerns[1] = 0.0f;
    t = klayer("AV", lens, modes, kerns, 2);
    must(pc_text_build_tysh(&t, 72.0f, NULL, &tysh, NULL));
    n = kerning_fields(tysh.data, tysh.len, f, 8);
    CHECK(n > 1 && f[1].kern == 10000);
    pc_psd_bytes_fini(&tysh);
    pc_text_layer_fini(&t);
}

/* The model parts of photoshop_kerning_oracles (corpus/photoshop/text/kerning-*.psd): the
 * automatic mode per character and the EngineData fields after export. The layout comparison
 * with Photoshop's glyph positions needs Arial and is left to the text chunk's own tests. */
static void test_photoshop_kerning_oracles(void)
{
    static const char *const names[5] = {"kerning-metrics.psd", "kerning-off.psd", "kerning-off-partial.psd",
                                         "kerning-optical.psd", "kerning-optical-24pt.psd"};
    static const bool same_fields[5] = {true, false, false, true, true};
    for (size_t c = 0; c < 5; c++) {
        char path[256];
        pc_buf bytes = PC_BUF_INIT;
        pc_document doc;
        const pc_text_layer *t = NULL;
        pc_kerning m[64];
        float k[64];
        size_t n;
        pc_psd_bytes rebuilt;
        pc_psd_file file;
        pc_ed_value txt2;
        (void)snprintf(path, sizeof path, "corpus/photoshop/text/%s", names[c]);
        if (pc_file_read(path, (size_t)1 << 30, &bytes, NULL) != PC_OK) {
            (void)printf("  (corpus missing: %s skipped)\n", path);
            continue;
        }
        doc = import_psd(&bytes);
        for (size_t i = 0; i < doc.nlayers && t == NULL; i++) {
            if (doc.layers[i].content.kind == PC_LAYER_TEXT) {
                t = &doc.layers[i].content.u.text;
            }
        }
        CHECK(t != NULL);
        if (t == NULL) {
            pc_document_fini(&doc);
            pc_buf_fini(&bytes);
            continue;
        }
        n = kchars(t, m, k, 64);
        CHECK_EQ_UINT(n, 18);
        for (size_t i = 0; i < n; i++) {
            pc_kerning want = c == 0   ? PC_KERNING_METRICS
                              : c == 1 ? PC_KERNING_OFF
                              : c == 2 ? ((i >= 1 && i <= 3) ? PC_KERNING_OFF : PC_KERNING_METRICS)
                                       : PC_KERNING_OPTICAL;
            CHECK(m[i] == want && k[i] == 0.0f);
        }
        must(pc_text_build_tysh(t, doc.resolution_dpi, NULL, &rebuilt, NULL));
        if (same_fields[c]) {
            kf a[64];
            kf b[64];
            size_t na = kerning_fields(rebuilt.data, rebuilt.len, a, 64);
            size_t nb = kerning_fields(pc_blob_data(t->psd_raw), pc_blob_len(t->psd_raw), b, 64);
            CHECK(na == nb);
            for (size_t i = 0; i < na && i < nb; i++) {
                CHECK(a[i].autok == b[i].autok && a[i].kern == b[i].kern);
            }
        }
        must(pc_psd_file_parse(bytes.data, bytes.len, &file, NULL));
        for (size_t i = 0; i < file.nglobal; i++) {
            if (memcmp(file.global_blocks[i].key, "Txt2", 4) == 0) {
                pc_text_layer back;
                must(pc_txt2_parse(file.global_blocks[i].data.data, file.global_blocks[i].data.len, &txt2,
                                   NULL));
                must(pc_text_layer_from_tysh(rebuilt.data, rebuilt.len, doc.resolution_dpi, &back, NULL));
                must(pc_txt2_apply(&back, rebuilt.data, rebuilt.len, &txt2, NULL));
                CHECK(same_chars(&back, t));
                pc_text_layer_fini(&back);
                pc_ed_fini(&txt2);
            }
        }
        pc_psd_file_fini(&file);
        pc_psd_bytes_fini(&rebuilt);
        pc_document_fini(&doc);
        pc_buf_fini(&bytes);
    }
}

/* ---------------------------------------------------------------- psd_vertical_type.rs */

static pc_document vertical_doc(pc_text_shape shape)
{
    pc_document doc = t_doc("v", 300, 400, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_text_layer t;
    pc_layer l;
    const char *text = "Vertical type\nColumns";
    pc_text_layer_default(&t);
    must(pc_str_set_c(&t.text, text, NULL));
    t.runs = xmalloc(sizeof *t.runs);
    t.nruns = 1;
    t.runs[0].len = strlen(text);
    t.runs[0].style = inter(24.0f);
    t.orientation = PC_ORIENT_VERTICAL;
    t.shape = shape;
    t.transform = pc_affine_translate(250.0, 20.0);
    must(pc_text_layer_sync_summary(&t, NULL));
    render_layer(&t, doc.resolution_dpi, pc_document_pixel_format(&doc));
    l = m_text("v", &t);
    t_push(&doc, &l);
    return doc;
}

static void test_vertical_orientation_round_trips_through_psd(void)
{
    pc_text_shape shapes[2];
    memset(shapes, 0, sizeof shapes);
    shapes[1].is_box = true;
    shapes[1].x = -200.0f;
    shapes[1].width = 220.0f;
    shapes[1].height = 300.0f;
    for (size_t s = 0; s < 2; s++) {
        pc_document doc = vertical_doc(shapes[s]);
        pc_rect ink = pc_surface_content_bounds(&text0(&doc)->cache);
        pc_document back;
        const pc_text_layer *t;
        pc_text_layer again;
        pc_rect r;
        CHECK(pc_rect_size(ink).height > pc_rect_size(ink).width);
        back = psd_roundtrip(&doc);
        t = text0(&back);
        CHECK(t->orientation == PC_ORIENT_VERTICAL);
        CHECK(t->shape.is_box == shapes[s].is_box && t->shape.x == shapes[s].x &&
              t->shape.width == shapes[s].width && t->shape.height == shapes[s].height);
        CHECK(pc_str_eq(&t->text, &text0(&doc)->text));
        must(pc_text_layer_clone(t, &again, NULL));
        render_layer(&again, back.resolution_dpi, pc_document_pixel_format(&back));
        r = pc_surface_content_bounds(&again.cache);
        CHECK(abs(r.x0 - ink.x0) <= 1 && abs(r.y0 - ink.y0) <= 1 && abs(r.x1 - ink.x1) <= 1 &&
              abs(r.y1 - ink.y1) <= 1);
        pc_text_layer_fini(&again);
        {
            pc_document back2 = psd_roundtrip(&back);
            CHECK(text0(&back2)->orientation == PC_ORIENT_VERTICAL);
            pc_document_fini(&back2);
        }
        pc_document_fini(&back);
        pc_document_fini(&doc);
    }
    {
        pc_document doc = vertical_doc(shapes[0]);
        pc_document back;
        doc.layers[0].content.u.text.orientation = PC_ORIENT_HORIZONTAL;
        pc_blob_release(doc.layers[0].content.u.text.psd_raw);
        doc.layers[0].content.u.text.psd_raw = NULL;
        back = psd_roundtrip(&doc);
        CHECK(text0(&back)->orientation == PC_ORIENT_HORIZONTAL);
        pc_document_fini(&back);
        pc_document_fini(&doc);
    }
}

/* ---------------------------------------------------------------- psd_type_bounds.rs */

typedef struct run {
    size_t len;
    double size;
    bool has_leading;
    double leading;
} run;

static void ed_set(pc_ed_value *d, const char *k, pc_ed_value v)
{
    must(pc_ed_set(d, k, &v, NULL));
}

static void ed_push(pc_ed_value *a, pc_ed_value v)
{
    must(pc_ed_push(a, &v, NULL));
}

static pc_ed_value ed_str(const char *s)
{
    pc_ed_value v;
    must(pc_ed_string(s, strlen(s), &v, NULL));
    return v;
}

static pc_ed_value ed_style(double size, bool has_leading, double leading)
{
    pc_ed_value d = pc_ed_dict();
    pc_ed_value fill = pc_ed_dict();
    pc_ed_value vals = pc_ed_array();
    ed_set(&d, "Font", pc_ed_int(0));
    ed_set(&d, "FontSize", pc_ed_real(size));
    ed_set(&d, "AutoLeading", pc_ed_bool(!has_leading));
    ed_set(&d, "Leading", pc_ed_real(has_leading ? leading : 0.0));
    ed_set(&fill, "Type", pc_ed_int(1));
    ed_push(&vals, pc_ed_real(1.0));
    ed_push(&vals, pc_ed_real(0.0));
    ed_push(&vals, pc_ed_real(0.0));
    ed_push(&vals, pc_ed_real(0.0));
    ed_set(&fill, "Values", vals);
    ed_set(&d, "FillColor", fill);
    return d;
}

static size_t utf16_len(const char *s)
{
    size_t i = 0;
    size_t n = 0;
    size_t len = strlen(s);
    while (i < len) {
        uint32_t cp = 0;
        if (!pc_utf8_next((const uint8_t *)s, len, &i, &cp)) {
            i++;
        }
        n += cp >= 0x10000 ? 2 : 1;
    }
    return n;
}

static pc_ed_value engine_data(const char *text, const run *runs, size_t nr, const double *box,
                               const char *font)
{
    char ed_text[512];
    pc_ed_value root = pc_ed_dict();
    pc_ed_value dict = pc_ed_dict();
    pc_ed_value editor = pc_ed_dict();
    pc_ed_value pr = pc_ed_dict();
    pc_ed_value pra = pc_ed_array();
    pc_ed_value prl = pc_ed_array();
    pc_ed_value psheet = pc_ed_dict();
    pc_ed_value sr = pc_ed_dict();
    pc_ed_value sra = pc_ed_array();
    pc_ed_value srl = pc_ed_array();
    pc_ed_value rendered = pc_ed_dict();
    pc_ed_value shapes = pc_ed_dict();
    pc_ed_value children = pc_ed_array();
    pc_ed_value shape = pc_ed_dict();
    pc_ed_value cookie = pc_ed_dict();
    pc_ed_value ps = pc_ed_dict();
    pc_ed_value res = pc_ed_dict();
    pc_ed_value fontset = pc_ed_array();
    pc_ed_value fnt = pc_ed_dict();
    pc_ed_value sss = pc_ed_array();
    pc_ed_value ss = pc_ed_dict();
    pc_ed_value pss = pc_ed_array();
    pc_ed_value pse = pc_ed_dict();
    (void)snprintf(ed_text, sizeof ed_text, "%s\r", text);
    ed_set(&editor, "Text", ed_str(ed_text));
    ed_set(&dict, "Editor", editor);
    {
        pc_ed_value para = pc_ed_dict();
        pc_ed_value para2;
        ed_set(&para, "Justification", pc_ed_int(0));
        ed_set(&para, "AutoLeading", pc_ed_real(1.2));
        must(pc_ed_clone(&para, &para2, NULL));
        ed_set(&psheet, "Properties", para);
        {
            pc_ed_value wrap = pc_ed_dict();
            ed_set(&wrap, "ParagraphSheet", psheet);
            ed_push(&pra, wrap);
        }
        ed_set(&pse, "Name", ed_str("Normal RGB"));
        ed_set(&pse, "Properties", para2);
    }
    ed_push(&prl, pc_ed_int((int64_t)utf16_len(ed_text)));
    ed_set(&pr, "RunArray", pra);
    ed_set(&pr, "RunLengthArray", prl);
    ed_set(&dict, "ParagraphRun", pr);
    for (size_t i = 0; i < nr; i++) {
        pc_ed_value a = pc_ed_dict();
        pc_ed_value b = pc_ed_dict();
        ed_set(&b, "StyleSheetData", ed_style(runs[i].size, runs[i].has_leading, runs[i].leading));
        ed_set(&a, "StyleSheet", b);
        ed_push(&sra, a);
        ed_push(&srl, pc_ed_int((int64_t)runs[i].len + (i + 1 == nr ? 1 : 0)));
    }
    ed_set(&sr, "RunArray", sra);
    ed_set(&sr, "RunLengthArray", srl);
    ed_set(&dict, "StyleRun", sr);
    ed_set(&ps, "ShapeType", pc_ed_int(box != NULL ? 1 : 0));
    if (box != NULL) {
        pc_ed_value bb = pc_ed_array();
        for (size_t i = 0; i < 4; i++) {
            ed_push(&bb, pc_ed_real(box[i]));
        }
        ed_set(&ps, "BoxBounds", bb);
    } else {
        pc_ed_value pb = pc_ed_array();
        ed_push(&pb, pc_ed_real(0.0));
        ed_push(&pb, pc_ed_real(0.0));
        ed_set(&ps, "PointBase", pb);
    }
    ed_set(&cookie, "Photoshop", ps);
    ed_set(&shape, "ShapeType", pc_ed_int(box != NULL ? 1 : 0));
    ed_set(&shape, "Cookie", cookie);
    ed_push(&children, shape);
    ed_set(&shapes, "Children", children);
    ed_set(&rendered, "Shapes", shapes);
    ed_set(&dict, "Rendered", rendered);
    ed_set(&root, "EngineDict", dict);
    ed_set(&fnt, "Name", ed_str(font));
    ed_set(&fnt, "Script", pc_ed_int(0));
    ed_push(&fontset, fnt);
    ed_set(&res, "FontSet", fontset);
    ed_set(&ss, "Name", ed_str("Normal RGB"));
    ed_set(&ss, "StyleSheetData", ed_style(12.0, false, 0.0));
    ed_push(&sss, ss);
    ed_set(&res, "StyleSheetSet", sss);
    ed_push(&pss, pse);
    ed_set(&res, "ParagraphSheetSet", pss);
    ed_set(&root, "ResourceDict", res);
    return root;
}

static pc_psd_value pnt(double v)
{
    return pc_io4_unit("#Pnt", v);
}

static pc_psd_value bounds_rect(const char *cls, const double ink[4])
{
    pc_psd_desc d;
    pc_psd_value v;
    must(pc_psd_desc_init(&d, cls, NULL));
    pc_psd_value l = pnt(ink[0]);
    must(pc_psd_desc_add(&d, "Left", &l, NULL));
    l = pnt(ink[1]);
    must(pc_psd_desc_add(&d, "Top ", &l, NULL));
    l = pnt(ink[2]);
    must(pc_psd_desc_add(&d, "Rght", &l, NULL));
    l = pnt(ink[3]);
    must(pc_psd_desc_add(&d, "Btom", &l, NULL));
    must(pc_psd_value_object(&d, &v, NULL));
    return v;
}

static void dput(pc_psd_desc *d, const char *k, pc_psd_value v)
{
    must(pc_psd_desc_add(d, k, &v, NULL));
}

static pc_psd_value denum(const char *t, const char *v)
{
    pc_psd_value r;
    must(pc_psd_value_enum(t, v, &r, NULL));
    return r;
}

static pc_psd_bytes tysh_font(const char *text, const run *runs, size_t nr, const double *box, pc_affine tr,
                              const double ink[4], const char *font)
{
    pc_tysh t;
    pc_psd_value txt;
    pc_psd_value raw;
    pc_ed_value ed = engine_data(text, runs, nr, box, font);
    pc_buf eb = PC_BUF_INIT;
    pc_psd_bytes out;
    memset(&t, 0, sizeof t);
    t.transform = tr;
    must(pc_psd_desc_init(&t.text, "TxLr", NULL));
    must(pc_psd_value_text(text, true, &txt, NULL));
    dput(&t.text, "Txt ", txt);
    dput(&t.text, "textGridding", denum("textGridding", "None"));
    dput(&t.text, "Ornt", denum("Ornt", "Hrzn"));
    dput(&t.text, "AntA", denum("Annt", "AnSm"));
    dput(&t.text, "bounds", bounds_rect("bounds", ink));
    dput(&t.text, "boundingBox", bounds_rect("boundingBox", ink));
    dput(&t.text, "TextIndex", pc_psd_value_int(0));
    must(pc_ed_write(&ed, &eb, NULL));
    memset(&raw, 0, sizeof raw);
    raw.kind = PC_PSD_V_RAW_DATA;
    must(pc_psd_bytes_set(&raw.raw, eb.data, eb.len, NULL));
    dput(&t.text, "EngineData", raw);
    t.bounds[0] = pc_f64_to_i32(floor(ink[0]));
    t.bounds[1] = pc_f64_to_i32(floor(ink[1]));
    t.bounds[2] = pc_f64_to_i32(ceil(ink[2]));
    t.bounds[3] = pc_f64_to_i32(ceil(ink[3]));
    must(pc_tysh_write(&t, &out, NULL));
    pc_tysh_fini(&t);
    pc_ed_fini(&ed);
    pc_buf_fini(&eb);
    return out;
}

typedef struct tcase {
    const char *name;
    float dpi;
    const char *text;
    run runs[3];
    size_t nr;
    bool has_box;
    double box[4];
    pc_affine transform;
    float size_pt;
} tcase;

static pc_affine scaled(double s, double x, double y)
{
    pc_affine a = {{s, 0.0, 0.0, s, x, y}};
    return a;
}

static size_t cases(tcase *c)
{
    double rad = -20.0 * 3.14159265358979323846 / 180.0;
    double sn = sin(rad);
    double cs = cos(rad);
    const char *h = "Headline 105: Autumn collection";
    const char *para = "Paragraph text that wraps over several lines in its box";
    memset(c, 0, 8 * sizeof *c);
    c[0] = (tcase){"point 36pt @72",        72.0f, h, {{31, 36.0, false, 0}}, 1, false, {0},
                   scaled(1.0, 40.0, 80.0), 36.0f};
    c[1] = (tcase){"point 36pt @300",        300.0f, h, {{31, 150.0, false, 0}}, 1, false, {0},
                   scaled(1.0, 40.0, 200.0), 36.0f};
    c[2] = (tcase){"point 12x3 scaled",     72.0f, h, {{31, 12.0, false, 0}}, 1, false, {0},
                   scaled(3.0, 40.0, 80.0), 12.0f};
    c[3] = (tcase){"point rotated",
                   72.0f,
                   "Autumn collection",
                   {{17, 18.0, false, 0}},
                   1,
                   false,
                   {0},
                   {{2.0 * cs, 2.0 * sn, -2.0 * sn, 2.0 * cs, 60.0, 300.0}},
                   18.0f};
    c[4] = (tcase){"point multi-line",
                   72.0f,
                   "Headline\rAutumn collection\rgy",
                   {{29, 36.0, true, 50.0}},
                   1,
                   false,
                   {0},
                   scaled(1.0, 40.0, 60.0),
                   36.0f};
    c[5] = (tcase){"point mixed sizes",
                   72.0f,
                   "Big small Big",
                   {{4, 60.0, false, 0}, {6, 14.0, false, 0}, {3, 60.0, false, 0}},
                   3,
                   false,
                   {0},
                   scaled(1.0, 30.0, 120.0),
                   60.0f};
    c[6] = (tcase){
        "paragraph box",         72.0f, para, {{55, 24.0, false, 0}}, 1, true, {0.0, 0.0, 260.0, 200.0},
        scaled(1.0, 20.0, 20.0), 24.0f};
    c[7] = (tcase){
        "paragraph box scaled",  144.0f, para, {{55, 20.0, false, 0}}, 1, true, {0.0, 0.0, 200.0, 150.0},
        scaled(1.5, 20.0, 20.0), 10.0f};
    /* The rotation of the reference is built from f64 sin_cos; Affine::translate for plain cases. */
    return 8;
}

static pc_psd_bytes case_tysh(const tcase *c, const double ink[4], const char *font)
{
    return tysh_font(c->text, c->runs, c->nr, c->has_box ? c->box : NULL, c->transform, ink, font);
}

static float iou(const pc_surface *a, const pc_surface *b)
{
    pc_rect u = pc_rect_union(pc_surface_content_bounds(a), pc_surface_content_bounds(b));
    float *x = NULL;
    float *y = NULL;
    size_t nx = 0;
    size_t ny = 0;
    size_t ca = pc_pixel_format_channels(pc_surface_format(a));
    size_t cb = pc_pixel_format_channels(pc_surface_format(b));
    float inter = 0.0f;
    float uni = 0.0f;
    if (pc_rect_is_empty(u)) {
        return 1.0f;
    }
    must(pc_surface_read_region(a, u, &x, &nx, NULL));
    must(pc_surface_read_region(b, u, &y, &ny, NULL));
    for (size_t i = 0; i < nx / ca && i < ny / cb; i++) {
        float p = x[i * ca + ca - 1];
        float q = y[i * cb + cb - 1];
        inter += p < q ? p : q;
        uni += p > q ? p : q;
    }
    free(x);
    free(y);
    return uni > 0.0f ? inter / uni : 1.0f;
}

static bool close_rect(pc_rect a, pc_rect b, int tol)
{
    return abs(a.x0 - b.x0) <= tol && abs(a.y0 - b.y0) <= tol && abs(a.x1 - b.x1) <= tol &&
           abs(a.y1 - b.y1) <= tol;
}

/* One-layer document carrying the case's TySh and "Photoshop's" pixels (our render of the model),
 * exported and imported back. */
static void import_case(const tcase *c, pc_rect *drawn, pc_document *back, float (**merged)[4], size_t *nm)
{
    pc_document doc = t_doc("t", 900, 700, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    static const double zero[4] = {0, 0, 0, 0};
    pc_psd_bytes probe_b = case_tysh(c, zero, "Inter-Regular");
    pc_text_layer probe;
    pc_text_engine *e = engine();
    pc_text_layout lay;
    float fb4[4] = {0, 0, 0, 0};
    double ink[4];
    pc_psd_bytes data;
    pc_text_layer t;
    pc_rect r;
    pc_layer l;
    pc_buf bytes;
    pc_psd_file file;
    doc.resolution_dpi = c->dpi;
    must(pc_text_layer_from_tysh(probe_b.data, probe_b.len, c->dpi, &probe, NULL));
    must(pc_text_engine_layout(e, &probe, c->dpi, &lay, NULL));
    (void)pc_text_layout_bounds(&lay, fb4);
    for (size_t i = 0; i < 4; i++) {
        ink[i] = (double)fb4[i];
    }
    pc_text_layout_fini(&lay);
    pc_text_layer_fini(&probe);
    pc_psd_bytes_fini(&probe_b);
    data = case_tysh(c, ink, "Inter-Regular");
    must(pc_text_layer_from_tysh(data.data, data.len, c->dpi, &t, NULL));
    must(pc_text_engine_render_layer(e, &t, c->dpi, pc_document_pixel_format(&doc), &r, NULL));
    pc_text_engine_free(e);
    *drawn = pc_surface_content_bounds(&t.cache);
    must(pc_blob_new(data.data, data.len, &t.psd_raw, NULL));
    pc_psd_bytes_fini(&data);
    l = m_text(c->name, &t);
    t_push(&doc, &l);
    bytes = export_psd(&doc, NULL);
    must(pc_psd_file_parse(bytes.data, bytes.len, &file, NULL));
    if (merged != NULL) {
        must(pc_io_merged_composite(&file, merged, nm, NULL));
    }
    pc_psd_file_fini(&file);
    *back = import_psd(&bytes);
    pc_buf_fini(&bytes);
    pc_document_fini(&doc);
}

static void test_imported_type_layers_keep_their_full_bounds(void)
{
    tcase cs[8];
    size_t n = cases(cs);
    for (size_t i = 0; i < n; i++) {
        const tcase *c = &cs[i];
        pc_rect drawn;
        pc_document doc;
        float (*merged)[4] = NULL;
        size_t nm = 0;
        const pc_text_layer *t;
        pc_text_engine *e = engine();
        pc_rendered ours;
        pc_compose_buffer flat;
        float worst = 0.0f;
        import_case(c, &drawn, &doc, &merged, &nm);
        t = text0(&doc);
        CHECK(!pc_rect_is_empty(drawn));
        if (fabsf(t->runs[0].style.size_pt - c->size_pt) >= 0.01f) {
            PC_T_FAIL("%s: size %g pt, want %g", c->name, (double)t->runs[0].style.size_pt,
                      (double)c->size_pt);
        }
        CHECK(memcmp(&t->transform, &c->transform, sizeof t->transform) == 0);
        CHECK(t->shape.is_box == c->has_box);
        if (c->has_box) {
            CHECK((double)t->shape.x == c->box[0] && (double)t->shape.y == c->box[1] &&
                  (double)(t->shape.x + t->shape.width) == c->box[2] &&
                  (double)(t->shape.y + t->shape.height) == c->box[3]);
        }
        CHECK(t->has_cache && pc_rect_eq(pc_surface_content_bounds(&t->cache), drawn));
        CHECK(pc_rect_eq(pc_compose_layer_bounds(&doc.layers[0], pc_document_bounds(&doc)), drawn));
        must(pc_text_engine_render(e, t, doc.resolution_dpi, pc_document_pixel_format(&doc), NULL, &ours,
                                   NULL));
        if (!close_rect(pc_surface_content_bounds(&ours.surface), drawn, 1)) {
            PC_T_FAIL("%s: re-render differs", c->name);
        }
        CHECK(iou(&ours.surface, &t->cache) > 0.97f);
        pc_rendered_fini(&ours);
        pc_text_engine_free(e);
        must(pc_compose_flatten(&doc, &flat, NULL));
        CHECK(flat.len == nm);
        /* common::max_diff: premultiplied, fully transparent pixels skipped. */
        for (size_t k = 0; k < nm && k < flat.len; k++) {
            const float *p = flat.px[k];
            const float *q = merged[k];
            if (p[3] <= 0.0f && q[3] <= 0.0f) {
                continue;
            }
            for (size_t ch = 0; ch < 4; ch++) {
                float x = ch < 3 ? p[ch] * p[3] : p[ch];
                float y = ch < 3 ? q[ch] * q[3] : q[ch];
                float d = fabsf(x - y);
                worst = d > worst ? d : worst;
            }
        }
        if (worst > 2.0f / 255.0f) {
            PC_T_FAIL("%s: composite differs from the merged image by %g", c->name, (double)worst);
        }
        free(flat.px);
        free(merged);
        pc_document_fini(&doc);
    }
}

static void test_point_text_height_follows_the_type_size(void)
{
    tcase cs[8];
    tcase c;
    pc_rect drawn;
    pc_rect drawn300;
    pc_document doc;
    float ratio;
    (void)cases(cs);
    import_case(&cs[0], &drawn, &doc, NULL, NULL);
    CHECK(pc_rect_size(drawn).height >= 24 && pc_rect_size(drawn).height <= 30);
    pc_document_fini(&doc);
    c = cs[0];
    c.text = "Headline: Autumn typography";
    c.runs[0].len = 27;
    import_case(&c, &drawn, &doc, NULL, NULL);
    CHECK(pc_rect_size(drawn).height >= 32 && pc_rect_size(drawn).height <= 38);
    pc_document_fini(&doc);
    c.dpi = 300.0f;
    c.runs[0].size = 150.0;
    import_case(&c, &drawn300, &doc, NULL, NULL);
    ratio = (float)pc_rect_size(drawn300).height / (float)pc_rect_size(drawn).height;
    CHECK(fabsf(ratio - 300.0f / 72.0f) < 0.2f);
    pc_document_fini(&doc);
}

/* A document with a type layer whose TySh is data and that has no pixels, exported and back. */
static pc_document without_pixels(const pc_psd_bytes *data, float dpi, uint32_t w, uint32_t h,
                                  const char *name)
{
    pc_document doc = t_doc("t", w, h, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_text_layer t;
    pc_layer l;
    pc_document back;
    doc.resolution_dpi = dpi;
    must(pc_text_layer_from_tysh(data->data, data->len, dpi, &t, NULL));
    must(pc_blob_new(data->data, data->len, &t.psd_raw, NULL));
    l = m_text(name, &t);
    t_push(&doc, &l);
    back = psd_roundtrip(&doc);
    pc_document_fini(&doc);
    return back;
}

static void test_type_layers_without_pixels_are_rendered_on_import(void)
{
    tcase cs[8];
    static const double ink[4] = {40.0, 50.0, 400.0, 110.0};
    pc_psd_bytes data;
    pc_document back;
    const pc_text_layer *bt;
    pc_rect drawn;
    pc_text_engine *e = engine();
    pc_rendered ours;
    (void)cases(cs);
    data = case_tysh(&cs[0], ink, "Inter-Regular");
    back = without_pixels(&data, cs[0].dpi, 900, 700, cs[0].name);
    bt = text0(&back);
    CHECK(pc_str_eq_c(&bt->text, cs[0].text));
    CHECK(bt->has_cache);
    drawn = pc_surface_content_bounds(&bt->cache);
    CHECK(!pc_rect_is_empty(drawn));
    must(pc_text_engine_render(e, bt, back.resolution_dpi, pc_document_pixel_format(&back), NULL, &ours,
                               NULL));
    CHECK(close_rect(pc_surface_content_bounds(&ours.surface), drawn, 1));
    pc_rendered_fini(&ours);
    pc_text_engine_free(e);
    pc_document_fini(&back);
    pc_psd_bytes_fini(&data);
    {
        pc_document d2;
        import_case(&cs[0], &drawn, &d2, NULL, NULL);
        CHECK(text0(&d2)->has_cache && pc_rect_eq(pc_surface_content_bounds(&text0(&d2)->cache), drawn));
        pc_document_fini(&d2);
    }
}

static void test_postscript_font_names_resolve_to_the_installed_family_on_import(void)
{
    tcase cs[8];
    static const double ink[4] = {40.0, 50.0, 400.0, 110.0};
    pc_psd_bytes data;
    pc_text_layer probe;
    pc_document back;
    const pc_text_layer *bt;
    (void)cases(cs);
    data = case_tysh(&cs[0], ink, "JetBrainsMono-Regular");
    must(pc_text_layer_from_tysh(data.data, data.len, cs[0].dpi, &probe, NULL));
    CHECK(pc_str_eq_c(&probe.runs[0].style.font_family, "Jet Brains Mono"));
    pc_text_layer_fini(&probe);
    back = without_pixels(&data, cs[0].dpi, 900, 700, cs[0].name);
    bt = text0(&back);
    CHECK(pc_str_eq_c(&bt->runs[0].style.font_family, "JetBrains Mono"));
    CHECK(bt->runs[0].style.has_postscript_name &&
          pc_str_eq_c(&bt->runs[0].style.postscript_name, "JetBrainsMono-Regular"));
    CHECK(bt->has_cache && !pc_rect_is_empty(pc_surface_content_bounds(&bt->cache)));
    pc_document_fini(&back);
    pc_psd_bytes_fini(&data);
}

static void test_hostile_type_sizes_without_pixels_import_without_a_huge_render(void)
{
    static const struct {
        double size;
        double scale;
        bool big;
    } hs[4] = {{12.0, 1100.0, true}, {1.0e30, 1.0, false}, {12.0, 1.0e12, false}, {DBL_MAX, DBL_MAX, false}};
    static const double ink[4] = {10.0, 10.0, 20.0, 20.0};
    for (size_t i = 0; i < 4; i++) {
        run r = {1, hs[i].size, false, 0.0};
        pc_psd_bytes data =
            tysh_font("M", &r, 1, NULL, scaled(hs[i].scale, 10.0, 10.0), ink, "Inter-Regular");
        pc_document back;
        const pc_text_layer *bt;
        if (hs[i].big) {
            /* Precondition: the renderer's own guard (256 Mpx) would have drawn this one. */
            pc_text_layer t;
            pc_text_engine *e = engine();
            pc_text_layout lay;
            pc_rect inkr;
            uint64_t px;
            must(pc_text_layer_from_tysh(data.data, data.len, 72.0f, &t, NULL));
            must(pc_text_engine_layout(e, &t, 72.0f, &lay, NULL));
            must(pc_text_ink_rect(&lay, &t.transform, NULL, &inkr, NULL));
            px = (uint64_t)pc_rect_size(inkr).width * pc_rect_size(inkr).height;
            CHECK(px > PC_IO_TEXT_MAX_RENDER_PIXELS && px <= PC_TEXT_MAX_PIXELS);
            pc_text_layout_fini(&lay);
            pc_text_layer_fini(&t);
            pc_text_engine_free(e);
        }
        back = without_pixels(&data, 72.0f, 64, 64, "hostile");
        bt = text0(&back);
        CHECK(bt->has_cache && pc_rect_is_empty(pc_surface_content_bounds(&bt->cache)));
        pc_document_fini(&back);
        pc_psd_bytes_fini(&data);
    }
}

static void test_blank_engine_text_is_not_rendered_on_import(void)
{
    run r0 = {0, 24.0, false, 0.0};
    run r1 = {1, 24.0, false, 0.0};
    static const double ink[4] = {10.0, 10.0, 20.0, 20.0};
    static const double zero[4] = {0, 0, 0, 0};
    pc_psd_bytes a = tysh_font("", &r0, 1, NULL, scaled(1.0, 10.0, 40.0), ink, "Inter-Regular");
    pc_psd_bytes b = tysh_font("M", &r1, 1, NULL, scaled(1.0, 0.0, 0.0), zero, "Inter-Regular");
    bool blank = false;
    must(pc_tysh_engine_text_is_blank(a.data, a.len, &blank, NULL));
    CHECK(blank);
    must(pc_tysh_engine_text_is_blank(b.data, b.len, &blank, NULL));
    CHECK(!blank);
    must(pc_tysh_engine_text_is_blank((const uint8_t *)"not a TySh", 10, &blank, NULL));
    CHECK(!blank);
    pc_psd_bytes_fini(&a);
    pc_psd_bytes_fini(&b);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)setenv("PHOTOCRAFT_NO_SYSTEM_FONTS", "1", 1);
    RUN(test_named_styles_round_trip_through_psd);
    RUN(test_documents_without_styles_keep_their_type_data);
    RUN(test_corpus_text_layers);
    RUN(test_corpus_tysh_lossless);
    RUN(test_created_text_layer_roundtrips_through_psd);
    RUN(test_manual_and_off_kerning_round_trip_through_psd);
    RUN(test_hostile_kerning_values);
    RUN(test_photoshop_kerning_oracles);
    RUN(test_vertical_orientation_round_trips_through_psd);
    RUN(test_imported_type_layers_keep_their_full_bounds);
    RUN(test_point_text_height_follows_the_type_size);
    RUN(test_type_layers_without_pixels_are_rendered_on_import);
    RUN(test_postscript_font_names_resolve_to_the_installed_family_on_import);
    RUN(test_hostile_type_sizes_without_pixels_import_without_a_huge_render);
    RUN(test_blank_engine_text_is_not_rendered_on_import);
    return pc_t_finish();
}
