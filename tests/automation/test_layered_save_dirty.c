/* Regression for #1547: a successful save back to the document's own PSD/PSB file records the
 * current revision as saved, so the session stops reporting dirty (as .pcraft already did). A flat
 * export stays a copy and leaves the dirty state and path alone (port of
 * crates/automation/tests/layered_save_dirty.rs).
 *
 * The C headless uses a format-capability test (.pcraft/.psd/.psb/.ora hold the whole document) in
 * place of the reference's holds/holds_at (REWRITE_PLAN.md §7 A2); that is exactly what this
 * regression exercises, so it ports faithfully. */
#include "harness.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "automation/headless.h"
#include "base/buf.h"
#include "base/file.h"
#include "base/json.h"
#include "engine/engine.h"
#include "io/io.h"

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

static bool dirty(pc_headless *h)
{
    const pc_engine_doc_state *d = pc_engine_session_active(h->session);
    return d && pc_engine_doc_state_is_dirty(d);
}

static const pc_engine_doc_state *act(pc_headless *h)
{
    return pc_engine_session_active(h->session);
}

static void mkdir_tmp(char *out, size_t cap, const char *tag)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(out, cap, "%s/pc-layered-save-%s-%d", tmp ? tmp : "/tmp", tag, (int)getpid());
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", out);
    int rc = system(rm);
    (void)rc;
    (void)mkdir(out, 0755);
}

static void rmrf(const char *dir)
{
    char rm[700];
    snprintf(rm, sizeof rm, "rm -rf '%s'", dir);
    int rc = system(rm);
    (void)rc;
}

/* Create `name`, reopen, edit, then save with no path: the save writes the document's own file and
 * must clear dirty. */
static void pathless_save_clears_dirty(const char *dir, const char *name)
{
    char path[700];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    pc_headless *h;
    CHECK(pc_headless_trusted_local(&h, NULL) == PC_OK);
    CHECK(call(h, "doc.new", "{\"width\":8,\"height\":8,\"background\":\"white\"}", NULL) == PC_OK);
    char save[900];
    snprintf(save, sizeof save, "{\"path\":\"%s\"}", path);
    CHECK(call(h, "doc.save", save, NULL) == PC_OK);
    CHECK(access(path, F_OK) == 0);

    char open[900];
    snprintf(open, sizeof open, "{\"path\":\"%s\"}", path);
    CHECK(call(h, "doc.open", open, NULL) == PC_OK);
    CHECK(call(h, "engine.execute", "{\"command\":\"image.adjustments.invert\",\"params\":{}}", NULL) == PC_OK);
    CHECK(dirty(h)); /* the edit dirties */

    /* The reported symptom: the save succeeds but the session still reports dirty. */
    CHECK(call(h, "doc.save", "{}", NULL) == PC_OK);
    CHECK(!dirty(h)); /* a pathless save is clean */
    const pc_engine_doc_state *d = act(h);
    CHECK(d && d->saved_revision == d->revision);
    CHECK(d && d->path && strcmp(d->path, path) == 0);

    /* A second save, with no further edit, stays clean. */
    CHECK(call(h, "doc.save", "{}", NULL) == PC_OK);
    CHECK(!dirty(h));

    pc_buf bytes = PC_BUF_INIT;
    CHECK(pc_file_read(path, (size_t)64 << 20, &bytes, NULL) == PC_OK);
    size_t nl = strlen(name);
    if (nl >= 3 && strcmp(name + nl - 3, "psd") == 0) {
        CHECK(pc_io_is_psd(bytes.data, bytes.len)); /* PSD magic */
    } else {
        CHECK(bytes.len >= 4 && memcmp(bytes.data, "8BPS", 4) == 0); /* PSB magic */
    }
    pc_buf_fini(&bytes);
    pc_headless_free(h);
}

static void test_pathless_psd_and_psb_saves_clear_dirty(void)
{
    char dir[640];
    mkdir_tmp(dir, sizeof dir, "clean");
    pathless_save_clears_dirty(dir, "a.psd");
    pathless_save_clears_dirty(dir, "a.psb");
    rmrf(dir);
}

/* Exporting a flat copy is not a document save: it must neither clear dirty nor retarget the path. */
static void test_flat_export_is_a_copy(void)
{
    char dir[640];
    mkdir_tmp(dir, sizeof dir, "copy");
    char native[700], png[700];
    snprintf(native, sizeof native, "%s/doc.pcraft", dir);
    snprintf(png, sizeof png, "%s/copy.png", dir);
    pc_headless *h;
    CHECK(pc_headless_trusted_local(&h, NULL) == PC_OK);
    CHECK(call(h, "doc.new", "{\"width\":8,\"height\":8,\"background\":\"white\"}", NULL) == PC_OK);
    char save[900];
    snprintf(save, sizeof save, "{\"path\":\"%s\"}", native);
    CHECK(call(h, "doc.save", save, NULL) == PC_OK);
    CHECK(call(h, "engine.execute", "{\"command\":\"image.adjustments.invert\",\"params\":{}}", NULL) == PC_OK);
    CHECK(dirty(h));

    char savepng[900];
    snprintf(savepng, sizeof savepng, "{\"path\":\"%s\"}", png);
    CHECK(call(h, "doc.save", savepng, NULL) == PC_OK);
    CHECK(access(png, F_OK) == 0);
    CHECK(dirty(h)); /* a flat export leaves the document dirty */
    const pc_engine_doc_state *d = act(h);
    CHECK(d && d->path && strcmp(d->path, native) == 0); /* path is not retargeted to the copy */

    /* ...and the document can still be saved back to its own native file. */
    CHECK(call(h, "doc.save", "{}", NULL) == PC_OK);
    CHECK(!dirty(h));
    pc_headless_free(h);
    rmrf(dir);
}

int main(int argc, char **argv)
{
    RUN(test_pathless_psd_and_psb_saves_clear_dirty);
    RUN(test_flat_export_is_a_copy);
    return pc_t_finish();
}
