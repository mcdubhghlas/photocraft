/* Regression for #1350: saving an inactive .pcraft/.psd/.psb file must not select it (port of
 * crates/automation/tests/inactive_save.rs).
 *
 * §7 divergence (A2, REWRITE_PLAN.md): the C headless save marks the saved document clean
 * (path + saved_revision) when the written format holds the whole document, but it does NOT
 * rename the in-memory document to the saved file's base name the way the reference does -- a
 * reopen shows the file name, the live document keeps the name it was created with. So where the
 * Rust test asserts `documents()[0].doc.name == "<saved filename>"`, this port asserts the
 * document's `path` ends with that file name (the behaviour the regression is actually about --
 * which document is active and which file each is bound to -- is checked exactly). */
#include "harness.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "automation/headless.h"
#include "base/json.h"
#include "base/str.h"
#include "engine/engine.h"

static pc_status call(pc_headless *h, const char *method, const char *pjson, pc_error *err)
{
    yyjson_doc *pd = NULL;
    if (pjson && pc_json_parse(pjson, strlen(pjson), &pd, err) != PC_OK) {
        return PC_ERR_INVALID;
    }
    pc_json_value out = PC_JSON_VALUE_INIT;
    pc_status rc = pc_headless_handle(h, method, pd ? yyjson_doc_get_root(pd) : NULL, &out, err);
    pc_json_value_fini(&out);
    yyjson_doc_free(pd);
    return rc;
}

static const pc_engine_doc_state *docs(pc_headless *h, size_t *n)
{
    return pc_engine_session_documents(h->session, n);
}

static size_t active(pc_headless *h)
{
    size_t a = (size_t)-1;
    (void)pc_engine_session_active_index(h->session, &a);
    return a;
}

static const char *doc_name(const pc_engine_doc_state *d)
{
    return pc_str_c(&pc_doc_arc_get(d->doc)->name);
}

/* true when d->path ends with `name`. */
static bool path_is(const pc_engine_doc_state *d, const char *name)
{
    if (!d->path) {
        return false;
    }
    size_t pl = strlen(d->path), nl = strlen(name);
    return pl >= nl && strcmp(d->path + pl - nl, name) == 0;
}

static void test_indexed_native_save_preserves_active(void)
{
    const char *tmp = getenv("TMPDIR");
    char dir[512];
    snprintf(dir, sizeof dir, "%s/pc-inactive-save-%d", tmp ? tmp : "/tmp", (int)getpid());
    char rm[640];
    snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
    int rc_rm = system(rm);
    (void)rc_rm;
    (void)mkdir(dir, 0755);
    char path[640];
    snprintf(path, sizeof path, "%s/first.pcraft", dir);

    pc_headless *h;
    CHECK(pc_headless_trusted_local(&h, NULL) == PC_OK);

    CHECK(call(h, "doc.new", "{\"width\":2,\"height\":2,\"background\":\"white\",\"name\":\"first\"}", NULL) == PC_OK);
    CHECK(call(h, "doc.new", "{\"width\":2,\"height\":2,\"background\":\"white\",\"name\":\"second\"}", NULL) == PC_OK);
    CHECK_EQ_UINT(active(h), 1u);
    size_t n = 0;
    const pc_engine_doc_state *d = docs(h, &n);
    CHECK_EQ_UINT(n, 2u);
    uint64_t first_rev = d[0].revision, second_rev = d[1].revision;

    char save0[1200];
    snprintf(save0, sizeof save0, "{\"index\":0,\"path\":\"%s\"}", path);
    CHECK(call(h, "doc.save", save0, NULL) == PC_OK);
    CHECK(access(path, F_OK) == 0); /* inactive document was saved */
    CHECK_EQ_UINT(active(h), 1u);   /* save must not select document 0 */
    d = docs(h, &n);
    CHECK(path_is(&d[0], "first.pcraft"));
    CHECK(d[0].revision == d[0].saved_revision); /* saved document is clean */
    CHECK(!pc_engine_doc_state_is_dirty(&d[0]));
    CHECK_EQ_STR(doc_name(&d[0]), "first"); /* C keeps the created name (see header) */

    /* Edit the active document; document 0 is untouched. */
    CHECK(call(h, "engine.execute", "{\"command\":\"image.adjustments.invert\",\"params\":{}}", NULL) == PC_OK);
    CHECK_EQ_UINT(active(h), 1u);
    d = docs(h, &n);
    CHECK(d[0].revision == first_rev);
    CHECK(d[1].revision > second_rev);

    const char *names[] = {"\xe4\xbf\x9d\xe5\xad\x98 first.psd", "renamed.psb"};
    for (size_t k = 0; k < 2; k++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", dir, names[k]);
        char save[1400];
        snprintf(save, sizeof save, "{\"index\":0,\"path\":\"%s\"}", p);
        CHECK(call(h, "doc.save", save, NULL) == PC_OK);
        CHECK_EQ_UINT(active(h), 1u);
        d = docs(h, &n);
        CHECK(path_is(&d[0], names[k]));
        CHECK_EQ_STR(doc_name(&d[1]), "second");
        CHECK(!pc_engine_doc_state_is_dirty(&d[0]));
    }

    /* A save into a path whose parent is a plain file fails, and changes nothing. */
    d = docs(h, &n);
    char before_path[1024];
    snprintf(before_path, sizeof before_path, "%s", d[0].path ? d[0].path : "");
    char blocked[700];
    snprintf(blocked, sizeof blocked, "%s/not-a-directory", dir);
    FILE *bf = fopen(blocked, "wb");
    if (bf) {
        (void)fwrite("blocks the parent directory", 1, 27, bf);
        fclose(bf);
    }
    char failed[1400];
    snprintf(failed, sizeof failed, "{\"index\":0,\"path\":\"%s/fail.psd\"}", blocked);
    CHECK(call(h, "doc.save", failed, NULL) != PC_OK);
    CHECK_EQ_UINT(active(h), 1u);
    d = docs(h, &n);
    CHECK_EQ_STR(d[0].path ? d[0].path : "", before_path);

    /* A flat copy (.png) leaves the path alone. */
    char copy[700];
    snprintf(copy, sizeof copy, "{\"index\":0,\"path\":\"%s/copy.png\"}", dir);
    CHECK(call(h, "doc.save", copy, NULL) == PC_OK);
    d = docs(h, &n);
    CHECK_EQ_STR(d[0].path ? d[0].path : "", before_path);

    pc_headless_free(h);
    rc_rm = system(rm);
    (void)rc_rm;
}

int main(int argc, char **argv)
{
    RUN(test_indexed_native_save_preserves_active);
    return pc_t_finish();
}
