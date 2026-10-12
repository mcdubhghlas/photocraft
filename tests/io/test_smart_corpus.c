/* crates/io/tests/smart_corpus.rs: smart objects and smart filters through PSD export over every
 * corpus file that has them (skipped cleanly without corpus/). For each file: import -> export
 * -> import gives back, layer for layer, a live smart object with the same embedded file,
 * transform, filters, switches and filter mask, and every exported block re-parses strictly.
 * Then two edits must survive the same way: the first filter toggled at 40 % in Multiply, and
 * every source re-embedded from its bytes (new lnk2 items, the old ones dropped). */
#include "harness.h"

#include <dirent.h>
#include <strings.h>

#include "psd_rt.h"

typedef struct paths {
    char **v;
    size_t n;
    size_t cap;
} paths;

static void collect(const char *dir, paths *out)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d == NULL) {
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char p[1024];
        struct stat st;
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.') {
            continue;
        }
        (void)snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            collect(p, out);
        } else if (n > 4 && (strcasecmp(e->d_name + n - 4, ".psd") == 0 ||
                             strcasecmp(e->d_name + n - 4, ".psb") == 0)) {
            if (out->n == out->cap) {
                out->cap = out->cap ? out->cap * 2 : 64;
                out->v = realloc(out->v, out->cap * sizeof *out->v);
                if (out->v == NULL) {
                    abort();
                }
            }
            out->v[out->n++] = pc_strndup(p, strlen(p));
        }
    }
    (void)closedir(d);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The smart-object layers of doc in walk order (borrowed). */
static size_t smarts(const pc_document *doc, const pc_layer **out, size_t cap)
{
    pc_walk w;
    size_t n = 0;
    must(pc_document_walk(doc, &w, NULL));
    for (size_t i = 0; i < w.len; i++) {
        if (w.items[i].layer->content.kind == PC_LAYER_SMART && n < cap) {
            out[n++] = w.items[i].layer;
        }
    }
    pc_walk_fini(&w);
    return n;
}

static bool source_file(const pc_document *doc, const pc_smart_object *sm, pc_buf *out)
{
    pc_buf_truncate(out, 0);
    if (!sm->source.linked) {
        must(pc_buf_append(out, pc_blob_data(sm->source.bytes), pc_blob_len(sm->source.bytes), NULL));
        return true;
    }
    {
        pc_io_linked_file f;
        bool found = false;
        must(pc_io_find_linked_file(&doc->metadata, pc_str_c(&sm->source.path), sm->source.path.len, &found,
                                    &f, NULL));
        if (found) {
            must(pc_buf_append(out, f.bytes, f.len, NULL));
            pc_io_linked_file_fini(&f);
        }
        return found;
    }
}

static float *mask_values(const pc_smart_object *sm, pc_rect area, size_t *n)
{
    float *v;
    *n = (size_t)pc_rect_width(area) * pc_rect_height(area);
    v = xmalloc(*n * sizeof *v);
    if (sm->has_filter_mask) {
        must(pc_layer_mask_values_into(&sm->filter_mask, area, v, *n, NULL));
    } else {
        for (size_t i = 0; i < *n; i++) {
            v[i] = 1.0f;
        }
    }
    return v;
}

#define MAXS 256

/* Compares the smart objects of a and b; returns the number of problems (printed). */
static size_t compare(const char *name, const char *what, const pc_document *a, const pc_document *b)
{
    const pc_layer *sa[MAXS];
    const pc_layer *sb[MAXS];
    size_t na = smarts(a, sa, MAXS);
    size_t nb = smarts(b, sb, MAXS);
    size_t errs = 0;
    pc_buf fa = PC_BUF_INIT;
    pc_buf fb = PC_BUF_INIT;
    if (na != nb) {
        (void)fprintf(stderr, "  FAIL %s (%s): %zu smart objects became %zu\n", name, what, na, nb);
        return 1;
    }
    for (size_t i = 0; i < na; i++) {
        const pc_smart_object *x = &sa[i]->content.u.smart;
        const pc_smart_object *y = &sb[i]->content.u.smart;
        bool ha = source_file(a, x, &fa);
        bool hb = source_file(b, y, &fb);
        bool same_filters = x->nsmart_filters == y->nsmart_filters;
        const char *ln = pc_str_c(&sa[i]->name);
        for (size_t k = 0; same_filters && k < x->nsmart_filters; k++) {
            same_filters = pc_smart_filter_eq(&x->smart_filters[k], &y->smart_filters[k]);
        }
        if (ha && hb && (fa.len != fb.len || memcmp(fa.data, fb.data, fa.len) != 0)) {
            (void)fprintf(stderr, "  FAIL %s (%s): %s: embedded file changed\n", name, what, ln);
            errs++;
        }
        if (ha && !hb) {
            (void)fprintf(stderr, "  FAIL %s (%s): %s: embedded file lost\n", name, what, ln);
            errs++;
        }
        if (!same_filters || x->filters_enabled != y->filters_enabled) {
            (void)fprintf(stderr, "  FAIL %s (%s): %s: filters changed\n", name, what, ln);
            errs++;
        }
        for (int k = 0; k < 6; k++) {
            if (fabs(x->transform.m[k] - y->transform.m[k]) > 1e-6) {
                (void)fprintf(stderr, "  FAIL %s (%s): %s: transform changed\n", name, what, ln);
                errs++;
                break;
            }
        }
        if (x->has_warp != y->has_warp || (x->has_warp && !pc_warp_eq(&x->warp, &y->warp))) {
            (void)fprintf(stderr, "  FAIL %s (%s): %s: warp changed\n", name, what, ln);
            errs++;
        }
        if (x->nsmart_filters > 0) {
            size_t n1;
            size_t n2;
            float *m1 = mask_values(x, pc_document_bounds(a), &n1);
            float *m2 = mask_values(y, pc_document_bounds(a), &n2);
            if (n1 != n2 || memcmp(m1, m2, n1 * sizeof *m1) != 0) {
                (void)fprintf(stderr, "  FAIL %s (%s): %s: filter mask changed\n", name, what, ln);
                errs++;
            }
            free(m1);
            free(m2);
        }
        if (x->has_filter_mask != y->has_filter_mask ||
            (x->has_filter_mask && (x->filter_mask.enabled != y->filter_mask.enabled ||
                                    x->filter_mask.linked != y->filter_mask.linked))) {
            (void)fprintf(stderr, "  FAIL %s (%s): %s: filter mask switches changed\n", name, what, ln);
            errs++;
        }
    }
    pc_buf_fini(&fa);
    pc_buf_fini(&fb);
    return errs;
}

/* export -> strict check -> import; false (printed) on a problem. */
static bool round_trip(const char *name, const char *what, const pc_document *doc, pc_document *back,
                       pc_buf *bytes)
{
    pc_io_export_result r;
    pc_error e;
    size_t errs;
    if (pc_io_export(doc, "psd", NULL, &r, &e) != PC_OK) {
        (void)fprintf(stderr, "  FAIL %s (%s): %s\n", name, what, e.msg);
        return false;
    }
    if (pc_io_warnings_any(&r.warnings, "smart object written as pixels")) {
        (void)fprintf(stderr, "  FAIL %s (%s): written as pixels\n", name, what);
        pc_io_export_result_fini(&r);
        return false;
    }
    errs = rt_strict_block_errors(r.bytes, r.len);
    pc_buf_truncate(bytes, 0);
    must(pc_buf_append(bytes, r.bytes, r.len, NULL));
    pc_io_export_result_fini(&r);
    if (errs > 0) {
        (void)fprintf(stderr, "  FAIL %s (%s): strict structure\n", name, what);
        return false;
    }
    *back = rt_import("x.psd", bytes);
    return true;
}

static bool edit_filters(pc_document *doc)
{
    pc_walk w;
    bool any = false;
    must(pc_document_walk(doc, &w, NULL));
    for (size_t i = 0; i < w.len; i++) {
        pc_layer *l = pc_document_layer_mut(doc, w.items[i].layer->id);
        if (l != NULL && l->content.kind == PC_LAYER_SMART && l->content.u.smart.nsmart_filters > 0) {
            pc_smart_filter *f = &l->content.u.smart.smart_filters[0];
            f->visible = !f->visible;
            f->opacity = 0.4f;
            f->blend = PC_BLEND_MULTIPLY;
            any = true;
        }
    }
    pc_walk_fini(&w);
    return any;
}

static bool re_embed(pc_document *doc)
{
    pc_walk w;
    bool any = false;
    pc_metadata meta;
    must(pc_metadata_clone(&doc->metadata, &meta, NULL));
    must(pc_document_walk(doc, &w, NULL));
    for (size_t i = 0; i < w.len; i++) {
        pc_layer *l = pc_document_layer_mut(doc, w.items[i].layer->id);
        pc_smart_object *sm;
        pc_io_linked_file f;
        bool found = false;
        if (l == NULL || l->content.kind != PC_LAYER_SMART || !l->content.u.smart.source.linked) {
            continue;
        }
        sm = &l->content.u.smart;
        must(
            pc_io_find_linked_file(&meta, pc_str_c(&sm->source.path), sm->source.path.len, &found, &f, NULL));
        if (!found) {
            continue;
        }
        pc_str_fini(&sm->source.path);
        sm->source.linked = false;
        sm->source.file_name = f.file_name;
        f.file_name.ptr = NULL;
        f.file_name.len = 0;
        must(pc_blob_new(f.bytes, f.len, &sm->source.bytes, NULL));
        pc_io_linked_file_fini(&f);
        pc_blob_release(sm->psd_raw);
        sm->psd_raw = NULL;
        {
            size_t k = 0;
            for (size_t j = 0; j < l->npsd_blocks; j++) {
                const uint8_t *key = l->psd_blocks[j].key;
                if (memcmp(key, "SoLd", 4) == 0 || memcmp(key, "PlLd", 4) == 0 ||
                    memcmp(key, "SoLE", 4) == 0) {
                    pc_blob_release(l->psd_blocks[j].data);
                } else {
                    l->psd_blocks[k++] = l->psd_blocks[j];
                }
            }
            l->npsd_blocks = k;
        }
        any = true;
    }
    pc_walk_fini(&w);
    pc_metadata_fini(&meta);
    return any;
}

/* After re-embedding: one lnk2 item per distinct source, the old files gone. */
static size_t check_items(const char *name, const pc_buf *out, const pc_document *back)
{
    pc_psd_file f;
    pc_io_strs items = {NULL, 0, 0};
    const pc_layer *s[MAXS];
    size_t ns = smarts(back, s, MAXS);
    size_t used = 0;
    size_t errs = 0;
    must(pc_psd_file_parse(out->data, out->len, &f, NULL));
    for (size_t i = 0; i < f.nglobal; i++) {
        const pc_psd_block *b = &f.global_blocks[i];
        if (pc_psd_block_is(b, "lnk2") || pc_psd_block_is(b, "lnk3") || pc_psd_block_is(b, "lnkD") ||
            pc_psd_block_is(b, "lnkE")) {
            pc_io_strs u = {NULL, 0, 0};
            must(pc_io_linked_block_uuids(b->data.data, b->data.len, &u, NULL));
            for (size_t k = 0; k < u.n; k++) {
                must(pc_io_strs_push(&items, pc_str_c(&u.v[k]), u.v[k].len, NULL));
            }
            pc_io_strs_fini(&u);
        }
    }
    for (size_t i = 0; i < ns; i++) {
        const pc_smart_source *src = &s[i]->content.u.smart.source;
        bool dup = false;
        bool have = false;
        if (!src->linked) {
            continue;
        }
        for (size_t j = 0; j < i && !dup; j++) {
            dup = s[j]->content.u.smart.source.linked &&
                  pc_str_eq(&s[j]->content.u.smart.source.path, &src->path);
        }
        if (dup) {
            continue;
        }
        used++;
        for (size_t k = 0; k < items.n && !have; k++) {
            have = pc_str_eq(&items.v[k], &src->path);
        }
        if (!have) {
            errs++;
        }
    }
    if (errs > 0 || items.n > used) {
        (void)fprintf(stderr, "  FAIL %s (re-embedded): %zu lnk2 items for %zu sources\n", name, items.n,
                      used);
        errs++;
    }
    pc_io_strs_fini(&items);
    pc_psd_file_fini(&f);
    return errs;
}

static void test_smart_objects_round_trip_through_psd(void)
{
    static const char *const DIRS[3] = {"corpus/psd", "corpus/psd-tools", "corpus/photoshop"};
    paths files = {NULL, 0, 0};
    size_t checked = 0;
    size_t objects = 0;
    size_t edited = 0;
    size_t embedded = 0;
    size_t failures = 0;
    struct stat st;
    if (stat("corpus/photoshop", &st) != 0) {
        (void)printf("corpus/ absent: skipped\n");
        return;
    }
    for (size_t i = 0; i < 3; i++) {
        collect(DIRS[i], &files);
    }
    qsort(files.v, files.n, sizeof *files.v, cmp_str);
    for (size_t i = 0; i < files.n; i++) {
        const char *name = files.v[i];
        pc_buf bytes = PC_BUF_INIT;
        pc_buf out = PC_BUF_INIT;
        pc_io_import_result imp;
        const pc_layer *s[MAXS];
        size_t n;
        pc_document back;
        pc_document ed;
        if (pc_file_read(name, (size_t)1 << 31, &bytes, NULL) != PC_OK ||
            pc_io_import(name, bytes.data, bytes.len, &imp, NULL) != PC_OK) {
            pc_buf_fini(&bytes);
            continue;
        }
        pc_buf_fini(&bytes);
        n = smarts(&imp.document, s, MAXS);
        if (n == 0) {
            pc_io_import_result_fini(&imp);
            continue;
        }
        checked++;
        objects += n;
        if (round_trip(name, "as imported", &imp.document, &back, &out)) {
            failures += compare(name, "as imported", &imp.document, &back);
            pc_document_fini(&back);
        } else {
            failures++;
        }
        must(pc_document_clone(&imp.document, &ed, NULL));
        if (edit_filters(&ed)) {
            edited++;
            if (round_trip(name, "filters edited", &ed, &back, &out)) {
                failures += compare(name, "filters edited", &ed, &back);
                pc_document_fini(&back);
            } else {
                failures++;
            }
        }
        pc_document_fini(&ed);
        must(pc_document_clone(&imp.document, &ed, NULL));
        if (re_embed(&ed)) {
            embedded++;
            if (round_trip(name, "re-embedded", &ed, &back, &out)) {
                failures += compare(name, "re-embedded", &ed, &back);
                failures += check_items(name, &out, &back);
                pc_document_fini(&back);
            } else {
                failures++;
            }
        }
        pc_document_fini(&ed);
        pc_buf_fini(&out);
        pc_io_import_result_fini(&imp);
    }
    (void)printf(
        "smart objects through PSD: %zu files, %zu smart objects; %zu filter edits, %zu re-embeds; %zu "
        "failures\n",
        checked, objects, edited, embedded, failures);
    CHECK(checked >= 30);
    CHECK_EQ_UINT(failures, 0);
    for (size_t i = 0; i < files.n; i++) {
        free(files.v[i]);
    }
    free(files.v);
}

int main(int argc, char **argv)
{
    RUN(test_smart_objects_round_trip_through_psd);
    return pc_t_finish();
}
