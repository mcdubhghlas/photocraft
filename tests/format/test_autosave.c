/* Background autosave and crash recovery (crates/format/tests/autosave.rs). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "format/bridge.h"
#include "harness.h"
#include "rich_doc.h"

static void clone(const pc_document *d, pc_document *out)
{
    if (pc_document_clone(d, out, NULL) != PC_OK) {
        PC_T_FAIL("clone");
        memset(out, 0, sizeof *out);
    }
}

static bool recover_equal(const pc_recovery_entry *e, const pc_document *want)
{
    pc_document back;
    bool eq;
    if (pc_recover(e, &back, NULL) != PC_OK) {
        return false;
    }
    eq = pc_document_eq(&back, want);
    pc_document_fini(&back);
    return eq;
}

static size_t list_count(const char *dir)
{
    pc_recovery_entry *e = NULL;
    size_t n = 0;
    CHECK(pc_list_recovery(dir, &e, &n, NULL) == PC_OK);
    pc_recovery_entries_free(e, n);
    return n;
}

static void test_autosave_then_recover(void)
{
    char dir[512];
    pc_document doc;
    pc_autosaver *a = NULL;
    pc_format_save_stats st;
    bool ok = false;
    pc_recovery_entry *e = NULL;
    size_t n = 0;
    CHECK(t_mkdtemp("recovery", dir, sizeof dir) == 0);
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, &doc);
    CHECK(pc_autosaver_new(dir, "doc-1", 5, &a, NULL) == PC_OK);
    CHECK(pc_autosaver_request(a, &doc, 7, "/work/a.pcraft", NULL, NULL) == PC_OK);
    CHECK(pc_autosaver_flush(a, &ok, &st, NULL)); /* a save ran */
    CHECK(ok);
    CHECK(st.tiles_written > 0);
    pc_autosaver_free(a);
    CHECK(pc_list_recovery(dir, &e, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    if (n == 1) {
        CHECK(e[0].info.revision == 7);
        CHECK(e[0].info.has_original_path && pc_str_eq_c(&e[0].info.original_path, "/work/a.pcraft"));
        CHECK(pc_str_eq_c(&e[0].info.document_name, "Rich"));
        CHECK(recover_equal(&e[0], &doc));
        CHECK(pc_discard_recovery(dir, &e[0], NULL) == PC_OK);
    }
    pc_recovery_entries_free(e, n);
    CHECK_EQ_UINT(list_count(dir), 0);
    pc_document_fini(&doc);
    t_rm_rf(dir);
}

static bool receive(pc_autosaver *a, pc_autosave_outcome *out)
{
    for (int i = 0; i < 3000; i++) {
        pc_autosave_outcome *o = NULL;
        size_t n = 0;
        struct timespec ts = {0, 10 * 1000 * 1000};
        CHECK(pc_autosaver_take_completed(a, &o, &n, NULL) == PC_OK);
        if (n > 0) {
            *out = o[0];
            free(o);
            return true;
        }
        (void)nanosleep(&ts, NULL);
    }
    PC_T_FAIL("autosave worker did not report its completed write within 30 seconds");
    return false;
}

/* A queued snapshot can fail later; callers must observe and retry that revision. */
static void test_background_save_outcomes_report_failure_then_recovery(void)
{
    char root[512];
    char blocked[700];
    pc_document doc;
    pc_autosaver *a = NULL;
    pc_autosave_outcome o;
    pc_recovery_entry *e = NULL;
    size_t n = 0;
    CHECK(t_mkdtemp("retry", root, sizeof root) == 0);
    (void)snprintf(blocked, sizeof blocked, "%s/recovery", root);
    CHECK(t_write_file(blocked, "not a directory", 15) == 0);
    CHECK(pc_autosaver_new(blocked, "doc", 3, &a, NULL) == PC_OK);
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &doc);
    CHECK(pc_autosaver_request(a, &doc, 7, NULL, NULL, NULL) == PC_OK);
    if (receive(a, &o)) {
        CHECK(o.revision == 7);
        CHECK(!o.ok); /* the recovery location is not a directory */
        CHECK(strncmp(o.msg, "I/O: ", 5) == 0);
    }
    CHECK(unlink(blocked) == 0);
    CHECK(mkdir(blocked, 0755) == 0);
    CHECK(pc_autosaver_request(a, &doc, 7, NULL, NULL, NULL) == PC_OK);
    if (receive(a, &o)) {
        CHECK(o.revision == 7);
        CHECK(o.ok);
    }
    CHECK(pc_list_recovery(blocked, &e, &n, NULL) == PC_OK);
    CHECK_EQ_UINT(n, 1);
    CHECK(n == 1 && recover_equal(&e[0], &doc));
    pc_recovery_entries_free(e, n);
    pc_autosaver_free(a);
    pc_document_fini(&doc);
    t_rm_rf(root);
}

static void test_repeated_autosaves_are_incremental_and_coalesced(void)
{
    char dir[512];
    pc_document doc;
    pc_autosaver *a = NULL;
    bool ok = false;
    pc_recovery_entry *e = NULL;
    size_t n = 0;
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    CHECK(t_mkdtemp("coalesce", dir, sizeof dir) == 0);
    CHECK(pc_autosaver_new(dir, "k", 1, &a, NULL) == PC_OK);
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &doc);
    for (int32_t i = 0; i < 5; i++) {
        CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&doc.layers[1]), i, 0, red, 4, NULL) == PC_OK);
        CHECK(pc_autosaver_request(a, &doc, (uint64_t)i, NULL, NULL, NULL) == PC_OK);
    }
    CHECK(pc_autosaver_flush(a, &ok, NULL, NULL) && ok);
    pc_autosaver_free(a);
    CHECK(pc_list_recovery(dir, &e, &n, NULL) == PC_OK);
    CHECK(n == 1 && e[0].info.revision == 4); /* newest snapshot wins */
    CHECK(n == 1 && recover_equal(&e[0], &doc));
    pc_recovery_entries_free(e, n);
    pc_document_fini(&doc);
    t_rm_rf(dir);
}

static void test_discard_removes_everything(void)
{
    char dir[512];
    char path[1024];
    pc_document doc;
    pc_autosaver *a = NULL;
    struct stat st;
    const char *base;
    CHECK(t_mkdtemp("discard", dir, sizeof dir) == 0);
    CHECK(pc_autosaver_new(dir, "x y/z", 5, &a, NULL) == PC_OK);
    t_rich_doc(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U8, &doc);
    CHECK(pc_autosaver_request(a, &doc, 1, NULL, NULL, NULL) == PC_OK);
    (void)snprintf(path, sizeof path, "%s", pc_autosaver_bundle_path(a));
    base = strrchr(path, '/') != NULL ? strrchr(path, '/') + 1 : path;
    CHECK(strncmp(base, "x_y_z", 5) == 0);
    CHECK(pc_autosaver_discard(a, NULL) == PC_OK);
    pc_autosaver_free(a);
    CHECK_EQ_UINT(list_count(dir), 0);
    CHECK(stat(path, &st) != 0);
    pc_document_fini(&doc);
    t_rm_rf(dir);
}

static void test_list_ignores_junk(void)
{
    char dir[512];
    char path[700];
    CHECK(t_mkdtemp("junk", dir, sizeof dir) == 0);
    (void)snprintf(path, sizeof path, "%s/bogus.json", dir);
    CHECK(t_write_file(path, "{}", 2) == 0);
    (void)snprintf(path, sizeof path, "%s/other.txt", dir);
    CHECK(t_write_file(path, "x", 1) == 0);
    CHECK_EQ_UINT(list_count(dir), 0);
    (void)snprintf(path, sizeof path, "%s/missing", dir);
    CHECK_EQ_UINT(list_count(path), 0);
    t_rm_rf(dir);
}

/* ---- RecoveryStore ---- */

/* doc as document `id` with pixel (x, 0) of its paint layer set, so versions are told apart. */
static void edit(pc_document *d, uint64_t id, int32_t x)
{
    float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    size_t n = pc_surface_channels(pc_layer_surface(&d->layers[1]));
    d->id = id;
    CHECK(pc_surface_write_pixel(pc_layer_surface_mut(&d->layers[1]), x, 0, red, n, NULL) == PC_OK);
}

static void version(uint64_t id, int32_t x, pc_color_mode mode, pc_document *out)
{
    t_rich_doc(mode, PC_SAMPLE_TYPE_U8, out);
    edit(out, id, x);
}

/* Save one snapshot under key and wait for it. */
static void save_once(const char *dir, const char *key, const pc_document *doc)
{
    pc_autosaver *a = NULL;
    bool ok = false;
    CHECK(pc_autosaver_new(dir, key, strlen(key), &a, NULL) == PC_OK);
    CHECK(pc_autosaver_request(a, doc, 1, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_autosaver_flush(a, &ok, NULL, NULL) && ok);
    pc_autosaver_free(a);
}

static int recovered_cmp(const void *a, const void *b)
{
    return pc_str_cmp(&((const pc_recovered *)a)->entry.info.key, &((const pc_recovered *)b)->entry.info.key);
}

/* The recovered documents sorted by entry key, ids cleared (they're reassigned on open). */
static size_t recovered(const pc_recovery_store *s, pc_recovered **out)
{
    size_t n = 0;
    *out = NULL;
    CHECK(pc_recovery_store_recover(s, out, &n, NULL) == PC_OK);
    if (n > 1) {
        qsort(*out, n, sizeof **out, recovered_cmp);
    }
    for (size_t i = 0; i < n; i++) {
        (*out)[i].doc.id = 0;
    }
    return n;
}

/* doc with its id cleared equals r. */
static bool same(const pc_document *doc, const pc_document *r)
{
    pc_document c;
    bool eq;
    clone(doc, &c);
    c.id = 0;
    eq = pc_document_eq(&c, r);
    pc_document_fini(&c);
    return eq;
}

static void test_recovered_entries_survive_a_second_crash_until_saved_or_closed(void)
{
    char dir[512];
    pc_document a;
    pc_document b;
    pc_document edited;
    pc_recovery_store *s = NULL;
    pc_recovered *r = NULL;
    size_t n;
    char keys[2][160];
    pc_recovery_entry *e = NULL;
    size_t ne = 0;
    CHECK(t_mkdtemp("second-crash", dir, sizeof dir) == 0);
    version(1, 3, PC_COLOR_MODE_RGB, &a);
    version(2, 5, PC_COLOR_MODE_GRAYSCALE, &b);
    /* Launch 1 autosaves two unsaved documents, then crashes (freeing waits for the writes). */
    CHECK(pc_recovery_store_new(dir, &s, NULL) == PC_OK);
    CHECK(pc_recovery_store_autosave(s, &a, 4, NULL, NULL) == PC_OK);
    CHECK(pc_recovery_store_autosave(s, &b, 6, "/work/b.pcraft", NULL) == PC_OK);
    pc_recovery_store_free(s);
    CHECK_EQ_UINT(list_count(dir), 2);
    /* Launch 2 recovers both and crashes before its first autosave: nothing may be lost. */
    CHECK(pc_recovery_store_new(dir, &s, NULL) == PC_OK);
    n = recovered(s, &r);
    CHECK_EQ_UINT(n, 2);
    CHECK_EQ_UINT(list_count(dir), 2); /* recovering must not delete the entries */
    for (size_t i = 0; i < n && i < 2; i++) {
        CHECK(pc_recovery_store_adopt(s, 10 + i, pc_str_c(&r[i].entry.info.key), NULL) == PC_OK);
        (void)snprintf(keys[i], sizeof keys[i], "%s", pc_str_c(&r[i].entry.info.key));
    }
    pc_recovered_free(r, n);
    pc_recovery_store_free(s);
    /* Launch 3 still has both, pixels intact; one is edited and autosaved, the other closed. */
    clone(&a, &edited);
    edit(&edited, 20, 9);
    CHECK(pc_recovery_store_new(dir, &s, NULL) == PC_OK);
    n = recovered(s, &r);
    CHECK(n == 2 && same(&a, &r[0].doc) && same(&b, &r[1].doc));
    pc_recovered_free(r, n);
    CHECK(pc_recovery_store_adopt(s, 20, keys[0], NULL) == PC_OK);
    CHECK(pc_recovery_store_adopt(s, 21, keys[1], NULL) == PC_OK);
    CHECK(pc_recovery_store_autosave(s, &edited, 9, NULL, NULL) == PC_OK);
    CHECK(pc_recovery_store_discard(s, 21, NULL) == PC_OK);
    CHECK_EQ_UINT(list_count(dir), 1); /* closing removes the adopted entry */
    pc_recovery_store_free(s);
    /* The autosave replaced the adopted entry in place: no duplicate, newest content. */
    CHECK(pc_list_recovery(dir, &e, &ne, NULL) == PC_OK);
    CHECK(ne == 1 && pc_str_eq_c(&e[0].info.key, keys[0]) && e[0].info.revision == 9);
    pc_recovery_entries_free(e, ne);
    /* Launch 4 recovers it once more and saves it: the entry goes. */
    CHECK(pc_recovery_store_new(dir, &s, NULL) == PC_OK);
    n = recovered(s, &r);
    CHECK(n == 1 && same(&edited, &r[0].doc));
    pc_recovered_free(r, n);
    CHECK(pc_recovery_store_adopt(s, 30, keys[0], NULL) == PC_OK);
    CHECK(pc_recovery_store_discard(s, 30, NULL) == PC_OK);
    CHECK_EQ_UINT(list_count(dir), 0);
    pc_recovery_store_free(s);
    pc_document_fini(&a);
    pc_document_fini(&b);
    pc_document_fini(&edited);
    t_rm_rf(dir);
}

static void test_new_documents_never_overwrite_surviving_entries(void)
{
    char dir[512];
    pc_document old;
    pc_document other;
    pc_document fresh;
    pc_recovery_store *s = NULL;
    pc_recovered *r = NULL;
    size_t n;
    bool saw_other = false;
    bool saw_fresh = false;
    CHECK(t_mkdtemp("collision", dir, sizeof dir) == 0);
    /* An earlier launch (including builds that keyed entries by bare document id) left doc-1. */
    version(1, 3, PC_COLOR_MODE_RGB, &old);
    save_once(dir, "doc-1", &old);
    /* A store from another launch left an entry for its own document 1 too. */
    version(1, 6, PC_COLOR_MODE_GRAYSCALE, &other);
    CHECK(pc_recovery_store_new(dir, &s, NULL) == PC_OK);
    CHECK(pc_recovery_store_autosave(s, &other, 2, NULL, NULL) == PC_OK);
    pc_recovery_store_free(s);
    CHECK_EQ_UINT(list_count(dir), 2);
    /* This launch adopts doc-1 for its document 5, and its own new document 1 autosaves. */
    version(1, 8, PC_COLOR_MODE_RGB, &fresh);
    CHECK(pc_recovery_store_new(dir, &s, NULL) == PC_OK);
    CHECK(pc_recovery_store_adopt(s, 5, "doc-1", NULL) == PC_OK);
    CHECK(pc_recovery_store_autosave(s, &fresh, 3, NULL, NULL) == PC_OK);
    pc_recovery_store_free(s);
    CHECK(pc_recovery_store_new(dir, &s, NULL) == PC_OK);
    n = recovered(s, &r);
    CHECK_EQ_UINT(n, 3); /* the new document got an entry of its own */
    for (size_t i = 0; i < n; i++) {
        if (pc_str_eq_c(&r[i].entry.info.key, "doc-1")) {
            CHECK(same(&old, &r[i].doc)); /* the adopted entry is untouched */
        }
        saw_other |= same(&other, &r[i].doc);
        saw_fresh |= same(&fresh, &r[i].doc);
    }
    CHECK(saw_other && saw_fresh);
    pc_recovered_free(r, n);
    pc_recovery_store_free(s);
    /* Closing the new document removes only its own entry. */
    CHECK(pc_recovery_store_new(dir, &s, NULL) == PC_OK);
    CHECK(pc_recovery_store_adopt(s, 5, "doc-1", NULL) == PC_OK);
    CHECK(pc_recovery_store_autosave(s, &fresh, 3, NULL, NULL) == PC_OK);
    CHECK(pc_recovery_store_discard(s, 1, NULL) == PC_OK);
    CHECK_EQ_UINT(list_count(dir), 3);
    CHECK(pc_recovery_store_discard(s, 5, NULL) == PC_OK);
    CHECK_EQ_UINT(list_count(dir), 2);
    pc_recovery_store_free(s);
    pc_document_fini(&old);
    pc_document_fini(&other);
    pc_document_fini(&fresh);
    t_rm_rf(dir);
}

static void test_sidecars_cannot_point_at_other_bundles(void)
{
    char dir[512];
    char inner[700];
    char path[900];
    pc_document doc;
    pc_buf info = PC_BUF_INIT;
    pc_buf esc = PC_BUF_INIT;
    pc_recovery_entry *e = NULL;
    size_t n = 0;
    const char *at;
    CHECK(t_mkdtemp("sidecar-key", dir, sizeof dir) == 0);
    (void)snprintf(inner, sizeof inner, "%s/Recovery", dir);
    t_rich_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, &doc);
    save_once(dir, "outside", &doc);
    save_once(inner, "k", &doc);
    (void)snprintf(path, sizeof path, "%s/k.json", inner);
    CHECK(t_read_file(path, &info) == 0);
    /* A copied sidecar (its name isn't its key) and one whose key leaves the directory. */
    (void)snprintf(path, sizeof path, "%s/copy.json", inner);
    CHECK(t_write_file(path, info.data, info.len) == 0);
    at = strstr(pc_buf_cstr(&info), "\"k\"");
    if (at != NULL) {
        size_t off = (size_t)(at - pc_buf_cstr(&info));
        (void)pc_buf_append(&esc, info.data, off, NULL);
        (void)pc_buf_append_str(&esc, "\"../outside\"", NULL);
        (void)pc_buf_append(&esc, info.data + off + 3, info.len - off - 3, NULL);
    }
    (void)snprintf(path, sizeof path, "%s/escape.json", inner);
    CHECK(t_write_file(path, esc.data, esc.len) == 0);
    CHECK(pc_list_recovery(inner, &e, &n, NULL) == PC_OK);
    CHECK(n == 1 && pc_str_eq_c(&e[0].info.key, "k"));
    pc_recovery_entries_free(e, n);
    pc_buf_fini(&info);
    pc_buf_fini(&esc);
    pc_document_fini(&doc);
    t_rm_rf(dir);
}

/* Not in the reference's tests: requests after a flush fail, outcomes drain once, and the
 * last result is reported before any save as "none". */
static void test_lifecycle_edges(void)
{
    char dir[512];
    pc_document doc;
    pc_autosaver *a = NULL;
    pc_error e;
    pc_autosave_outcome *o = NULL;
    size_t n = 7;
    bool ok = false;
    pc_size one = {1, 1};
    CHECK(t_mkdtemp("edges", dir, sizeof dir) == 0);
    CHECK(pc_document_init(&doc, "e", one, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    CHECK(pc_autosaver_new(dir, "", 0, &a, NULL) == PC_OK);
    CHECK(!pc_autosaver_last_result(a, &ok, NULL, NULL));
    CHECK(strstr(pc_autosaver_bundle_path(a), "/untitled.pcraft") != NULL);
    CHECK(pc_autosaver_take_completed(a, &o, &n, NULL) == PC_OK);
    CHECK(n == 0 && o == NULL);
    CHECK(pc_autosaver_request(a, &doc, 3, NULL, NULL, NULL) == PC_OK);
    CHECK(pc_autosaver_flush(a, &ok, NULL, NULL) && ok);
    CHECK(pc_autosaver_take_completed(a, &o, &n, NULL) == PC_OK);
    CHECK(n == 1 && o[0].revision == 3 && o[0].ok);
    free(o);
    CHECK(pc_autosaver_take_completed(a, &o, &n, NULL) == PC_OK);
    CHECK(n == 0);
    CHECK(pc_autosaver_request(a, &doc, 4, NULL, NULL, &e) == PC_ERR_INVALID);
    CHECK_EQ_STR(e.msg, "autosave worker is stopped");
    pc_autosaver_free(a);
    pc_document_fini(&doc);
    t_rm_rf(dir);
}

int main(int argc, char **argv)
{
    RUN(test_autosave_then_recover);
    RUN(test_background_save_outcomes_report_failure_then_recovery);
    RUN(test_repeated_autosaves_are_incremental_and_coalesced);
    RUN(test_discard_removes_everything);
    RUN(test_list_ignores_junk);
    RUN(test_recovered_entries_survive_a_second_crash_until_saved_or_closed);
    RUN(test_new_documents_never_overwrite_surviving_entries);
    RUN(test_sidecars_cannot_point_at_other_bundles);
    RUN(test_lifecycle_edges);
    return pc_t_finish();
}
