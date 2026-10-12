/* The integration tests chunk I4 left for I5 (crates/io/tests):
 *   vector.rs          corpus_shape_coverage_matches_photoshop (corpus/psd)
 *   comps_artboards.rs the .pcraft identity part of synthetic_pcraft_roundtrip_keeps_everything:
 *                      independent of ZIP entry order and of the file name's extension, PNG bytes
 *                      named .pcraft still open, a broken bundle is a pcraft error
 *   psd_kerning.rs     the layout half of photoshop_kerning_oracles: glyph advances against
 *                      Photoshop's (corpus/photoshop/text; needs Arial installed, skipped without)
 *   cancel.rs          the PSD half: a cancelled PSD import fails with "cancelled"; progress is
 *                      monotonic, ends at 1 and is reported per layer
 * Corpus parts skip cleanly without corpus/. */
#include <dirent.h>
#include <strings.h>

#include "harness.h"
#include "io/common.h"

#include "format/zip.h"
#include "psd/file.h"
#include "text/engine.h"
#include "text/engine_data.h"
#include "text/fonts.h"
#include "text/psd.h"
#include "vector/vector.h"

/* ---------------------------------------------------------------- vector.rs */

typedef struct cov_stats {
    size_t n, good;
} cov_stats;

static void shape_layers(const pc_layer *ls, size_t n, const pc_document *d, const char *file, cov_stats *s)
{
    for (size_t i = 0; i < n; i++) {
        const pc_layer *l = &ls[i];
        const pc_shape_layer *sh;
        pc_vec_rasterizer ras;
        pc_rect pb = PC_RECT_EMPTY;
        pc_rect r;
        float *ours;
        size_t len;
        float (*theirs)[4];
        double inter = 0.0;
        double uni = 0.0;
        float mx = 0.0f;
        if (l->content.kind == PC_LAYER_GROUP) {
            shape_layers(l->content.u.group.children, l->content.u.group.nchildren, d, file, s);
            continue;
        }
        if (l->content.kind != PC_LAYER_SHAPE) {
            continue;
        }
        sh = &l->content.u.shape;
        if (!sh->has_cache || sh->has_stroke || !sh->has_fill || sh->fill.kind != PC_FILL_SOLID ||
            l->effects.nitems > 0 || pc_path_is_empty(&sh->path)) {
            continue;
        }
        if (pc_vec_fill_rasterizer(&sh->path, 0.01, &ras, NULL) == PC_OK) {
            if (!pc_vec_rasterizer_pixel_bounds(&ras, &pb)) {
                pb = PC_RECT_EMPTY;
            }
            pc_vec_rasterizer_fini(&ras);
        }
        r = pc_rect_intersect(pc_rect_union(pc_surface_content_bounds(&sh->cache), pb),
                              pc_document_bounds(d));
        if (pc_rect_is_empty(r)) {
            continue;
        }
        must(pc_vec_path_coverage(&sh->path, r, &ours, &len, NULL));
        theirs = xmalloc(len * sizeof *theirs);
        must(pc_surface_read_rgba_into(&sh->cache, r, theirs, len, NULL));
        for (size_t k = 0; k < len; k++) {
            float a = ours[k];
            float b = theirs[k][3];
            inter += a < b ? a : b;
            uni += a > b ? a : b;
            mx = fabsf(a - b) > mx ? fabsf(a - b) : mx;
        }
        s->n++;
        if (inter / (uni > 1e-9 ? uni : 1e-9) > 0.97) {
            s->good++;
        }
        (void)file;
        free(theirs);
        free(ours);
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool psd_ext(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && (strcasecmp(name + n - 4, ".psd") == 0 || strcasecmp(name + n - 4, ".psb") == 0);
}

static void collect_psds(const char *dir, char ***out, size_t *n)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    if (d == NULL) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char path[1024];
        struct stat st;
        if (de->d_name[0] == '.') {
            continue;
        }
        (void)snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            collect_psds(path, out, n);
        } else if (psd_ext(de->d_name)) {
            *out = realloc(*out, (*n + 1) * sizeof **out);
            if (*out == NULL) {
                abort();
            }
            (*out)[(*n)++] = strdup(path);
        }
    }
    (void)closedir(d);
}

static void test_corpus_shape_coverage_matches_photoshop(void)
{
    char **files = NULL;
    size_t n = 0;
    cov_stats s = {0, 0};
    if (access("corpus/psd", R_OK) != 0) {
        (void)printf("  corpus/psd absent: skipped\n");
        return;
    }
    collect_psds("corpus/psd", &files, &n);
    if (n > 1) {
        qsort(files, n, sizeof *files, cmp_str);
    }
    for (size_t i = 0; i < n; i++) {
        pc_buf bytes = PC_BUF_INIT;
        pc_psd_file f;
        pc_document d;
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        if (pc_file_read(files[i], (size_t)1 << 31, &bytes, NULL) == PC_OK &&
            pc_psd_file_parse(bytes.data, bytes.len, &f, NULL) == PC_OK) {
            if (pc_io_psd_to_document(&f, &d, &w, NULL) == PC_OK) {
                shape_layers(d.layers, d.nlayers, &d, files[i], &s);
                pc_document_fini(&d);
            }
            pc_io_warnings_fini(&w);
            pc_psd_file_fini(&f);
        }
        pc_buf_fini(&bytes);
        free(files[i]);
    }
    free(files);
    (void)printf("  vector corpus: %zu/%zu plain shape layers with IoU > 0.97\n", s.good, s.n);
    CHECK(s.n == 0 || s.good * 10 >= s.n * 9);
}

/* ---------------------------------------------------------------- comps_artboards.rs */

static void test_pcraft_identity_ignores_zip_order_and_extension(void)
{
    pc_document d = t_gen_doc_all(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16);
    pc_io_export_result saved;
    pc_io_import_result base;
    pc_zip_reader z;
    pc_zip_writer w;
    pc_buf reordered = PC_BUF_INIT;
    pc_io_import_result r;
    pc_error e;
    static const char *const NAMES[2] = {"x.PCRAFT", "no-extension"};
    must(pc_io_export(&d, "x.pcraft", NULL, &saved, NULL));
    must(pc_io_import("x.pcraft", saved.bytes, saved.len, &base, NULL));
    must(pc_zip_reader_open(&z, saved.bytes, saved.len, NULL));
    pc_zip_writer_init(&w);
    must(pc_zip_writer_add(&w, "extra.txt", 9, (const uint8_t *)"not the manifest", 16, NULL));
    for (size_t i = 0; i < z.count; i++) {
        pc_buf data = PC_BUF_INIT;
        must(pc_zip_read(&z, &z.entries[i], saved.len, &data, NULL));
        must(pc_zip_writer_add(&w, z.entries[i].name.ptr, z.entries[i].name.len, data.data, data.len, NULL));
        pc_buf_fini(&data);
    }
    must(pc_zip_writer_finish(&w, &reordered, NULL));
    for (int k = 0; k < 2; k++) {
        if (pc_io_import(NAMES[k], reordered.data, reordered.len, &r, &e) != PC_OK) {
            PC_T_FAIL("%s: %s", NAMES[k], e.msg);
            continue;
        }
        CHECK(pc_document_eq(&r.document, &base.document));
        CHECK_EQ_UINT(r.document.nlayer_comps, d.nlayer_comps);
        pc_io_import_result_fini(&r);
    }
    {
        pc_io_export_result png;
        must(pc_io_export(&d, "x.png", NULL, &png, NULL));
        CHECK(pc_io_import("pcraft", png.bytes, png.len, &r, NULL) == PC_OK);
        pc_io_import_result_fini(&r);
        pc_io_export_result_fini(&png);
    }
    {
        pc_status st = pc_io_import("broken.pcraft", (const uint8_t *)"not a native bundle", 19, &r, &e);
        CHECK(st != PC_OK && pc_io_error_kind(st, &e) == PC_IO_ERR_PCRAFT);
    }
    pc_buf_fini(&reordered);
    pc_zip_writer_fini(&w);
    pc_zip_reader_close(&z);
    pc_io_import_result_fini(&base);
    pc_io_export_result_fini(&saved);
    pc_document_fini(&d);
}

/* ---------------------------------------------------------------- psd_kerning.rs */

/* Photoshop's glyph x positions for the TextIndex-th text object of Txt2: the first "21"/"1"
 * array under it. */
static const pc_ed_value *find_positions(const pc_ed_value *v, size_t depth)
{
    if (depth > 100) {
        return NULL;
    }
    if (v->kind == PC_ED_DICT) {
        const pc_ed_value *p = pc_ed_get(v, "21");
        const pc_ed_value *a = p != NULL ? pc_ed_get(p, "1") : NULL;
        if (a != NULL && a->kind == PC_ED_ARRAY) {
            return a;
        }
        for (size_t i = 0; i < v->n; i++) {
            const pc_ed_value *r = find_positions(&v->entries[i].value, depth + 1);
            if (r != NULL) {
                return r;
            }
        }
    } else if (v->kind == PC_ED_ARRAY) {
        for (size_t i = 0; i < v->n; i++) {
            const pc_ed_value *r = find_positions(&v->items[i], depth + 1);
            if (r != NULL) {
                return r;
            }
        }
    }
    return NULL;
}

static void test_kerning_layout_against_photoshop(void)
{
    static const char *const NAMES[5] = {"kerning-metrics.psd", "kerning-off.psd", "kerning-off-partial.psd",
                                         "kerning-optical.psd", "kerning-optical-24pt.psd"};
    static const double WORST[5] = {0.02, 0.02, 0.02, 1.0, 0.75};
    static const double MEAN[5] = {0.02, 0.02, 0.02, 0.4, 0.3};
    pc_text_engine *eng;
    if (access("corpus/photoshop/text", R_OK) != 0) {
        (void)printf("  corpus/photoshop/text absent: skipped\n");
        return;
    }
    must(pc_text_engine_with_system_fonts(&eng, NULL));
    if (!pc_fontdb_has_family(pc_text_engine_fonts(eng), "Arial", 5)) {
        (void)printf("  Arial not installed: layout comparison skipped\n");
        pc_text_engine_free(eng);
        return;
    }
    for (int c = 0; c < 5; c++) {
        char path[256];
        pc_buf bytes = PC_BUF_INIT;
        pc_io_import_result r;
        pc_psd_file f;
        const pc_text_layer *t = NULL;
        pc_ed_value txt2;
        pc_tysh tysh;
        const pc_psd_value *ti;
        const pc_ed_value *objs;
        const pc_ed_value *ps;
        pc_text_layout lay;
        double worst = 0.0;
        double sum = 0.0;
        size_t nobj = 0;
        (void)snprintf(path, sizeof path, "corpus/photoshop/text/%s", NAMES[c]);
        must(pc_file_read(path, (size_t)1 << 30, &bytes, NULL));
        must(pc_io_import(NAMES[c], bytes.data, bytes.len, &r, NULL));
        must(pc_psd_file_parse(bytes.data, bytes.len, &f, NULL));
        for (size_t i = 0; i < r.document.nlayers && t == NULL; i++) {
            if (r.document.layers[i].content.kind == PC_LAYER_TEXT) {
                t = &r.document.layers[i].content.u.text;
            }
        }
        CHECK(t != NULL);
        memset(&txt2, 0, sizeof txt2);
        for (size_t i = 0; i < f.nglobal; i++) {
            if (memcmp(f.global_blocks[i].key, "Txt2", 4) == 0) {
                must(pc_txt2_parse(f.global_blocks[i].data.data, f.global_blocks[i].data.len, &txt2, NULL));
            }
        }
        must(pc_tysh_parse(pc_blob_data(t->psd_raw), pc_blob_len(t->psd_raw), &tysh, NULL));
        ti = pc_psd_desc_get(&tysh.text, "TextIndex");
        objs = pc_ed_as_array(pc_ed_path(&txt2, "1", "1", NULL), &nobj);
        ps = ti != NULL && ti->kind == PC_PSD_V_INTEGER && objs != NULL && (size_t)ti->i < nobj
                 ? find_positions(&objs[ti->i], 0)
                 : NULL;
        CHECK(ps != NULL);
        must(pc_text_engine_layout(eng, t, r.document.resolution_dpi, &lay, NULL));
        if (ps != NULL && lay.nglyphs >= 2 && ps->n >= lay.nglyphs) {
            for (size_t i = 0; i + 1 < lay.nglyphs; i++) {
                double a;
                double b;
                double d;
                (void)pc_ed_as_f64(&ps->items[i], &a);
                (void)pc_ed_as_f64(&ps->items[i + 1], &b);
                d = ((double)lay.glyphs[i + 1].x - (double)lay.glyphs[i].x) - (b - a);
                worst = fabs(d) > worst ? fabs(d) : worst;
                sum += fabs(d);
            }
            sum /= (double)(lay.nglyphs - 1);
            (void)printf("  %s: advance error vs Photoshop: mean %.3f px, worst %.3f px\n", NAMES[c], sum,
                         worst);
            if (worst > WORST[c] || sum > MEAN[c]) {
                PC_T_FAIL("%s: mean %g, worst %g", NAMES[c], sum, worst);
            }
        } else {
            CHECK(false);
        }
        pc_text_layout_fini(&lay);
        pc_tysh_fini(&tysh);
        pc_ed_fini(&txt2);
        pc_psd_file_fini(&f);
        pc_io_import_result_fini(&r);
        pc_buf_fini(&bytes);
    }
    pc_text_engine_free(eng);
}

/* ---------------------------------------------------------------- cancel.rs */

typedef struct lo_ctl {
    bool cancel;
    float last;
    unsigned calls;
    bool backwards;
} lo_ctl;

static bool lo_cancel(void *ctx)
{
    return ((lo_ctl *)ctx)->cancel;
}

static void lo_progress(void *ctx, float f)
{
    lo_ctl *s = ctx;
    s->backwards = s->backwards || f < s->last;
    s->last = f;
    s->calls++;
}

static void layered_psd(pc_io_export_result *out)
{
    pc_document d = t_doc("t", 32, 24, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    for (int i = 0; i < 5; i++) {
        char name[16];
        float px[4] = {0.2f * (float)i, 0.5f, 0.7f, 1.0f};
        pc_surface s;
        pc_layer l;
        snprintf(name, sizeof name, "L%d", i);
        pc_surface_init(&s, pc_document_pixel_format(&d));
        must(pc_surface_fill_rect(&s, pc_rect_make(i, i, 20 + i, 16 + i), px, 4, NULL));
        must(pc_io_raster_layer(name, &s, &l, NULL));
        pc_surface_fini(&s);
        t_push(&d, &l);
    }
    must(pc_io_export(&d, "t.psd", NULL, out, NULL));
    pc_document_fini(&d);
}

static void test_psd_import_cancel_and_progress(void)
{
    pc_io_export_result psd;
    layered_psd(&psd);
    {
        lo_ctl s = {true, 0.0f, 0, false};
        pc_interrupt ctl = {lo_cancel, lo_progress, &s};
        pc_io_import_result r;
        pc_error e;
        pc_status st = pc_io_import_with("t.psd", psd.bytes, psd.len, &ctl, &r, &e);
        CHECK_EQ_INT(st, PC_ERR_CANCELLED);
        if (st == PC_OK) {
            pc_io_import_result_fini(&r);
        } else {
            CHECK_EQ_STR(e.msg, "cancelled");
        }
    }
    {
        lo_ctl s = {false, 0.0f, 0, false};
        pc_interrupt ctl = {lo_cancel, lo_progress, &s};
        pc_io_import_result r;
        pc_io_import_result plain;
        must(pc_io_import_with("t.psd", psd.bytes, psd.len, &ctl, &r, NULL));
        CHECK_EQ_UINT(r.document.nlayers, 5);
        CHECK(!s.backwards);
        CHECK(s.last == 1.0f);
        CHECK(s.calls >= 6); /* one report per layer plus stages */
        must(pc_io_import("t.psd", psd.bytes, psd.len, &plain, NULL));
        CHECK_EQ_UINT(plain.document.nlayers, r.document.nlayers);
        pc_io_import_result_fini(&plain);
        pc_io_import_result_fini(&r);
    }
    pc_io_export_result_fini(&psd);
}

int main(int argc, char **argv)
{
    RUN(test_corpus_shape_coverage_matches_photoshop);
    RUN(test_pcraft_identity_ignores_zip_order_and_extension);
    RUN(test_kerning_layout_against_photoshop);
    RUN(test_psd_import_cancel_and_progress);
    return pc_t_finish();
}
