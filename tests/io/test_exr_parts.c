/* Multi-part OpenEXR import runtime tests (exercises src/io/exr_parts.c; the Rust module
 * crates/io/src/exr_parts.rs has no tests of its own, so these assert the documented behaviour
 * against the committed codecs EXR fixtures). Header listing works in every build; pixel decoding
 * needs OpenEXRCore (skipped with WITH_EXR=0). The all-deep refusal uses the corpus when present. */
#include "harness.h"

#include "common.h"

#include "codecs/format.h"
#include "io/exr_parts.h"

static pc_buf load(const char *dir, const char *name)
{
    char path[256];
    pc_buf b = PC_BUF_INIT;
    (void)snprintf(path, sizeof path, "%s/%s", dir, name);
    if (pc_file_read(path, (size_t)4 << 20, &b, NULL) != PC_OK) {
        PC_T_FAIL("missing fixture %s", path);
    }
    return b;
}

static pc_buf fixture(const char *name) { return load("tests/fixtures/codecs", name); }

static const pc_io_exr_entry *find_entry(const pc_io_exr_entry *v, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (strcmp(v[i].name, name) == 0) {
            return &v[i];
        }
    }
    return NULL;
}

static void test_entries_list_parts_and_cryptomatte(void)
{
    pc_buf aov = fixture("aov.exr");
    pc_buf crypto = fixture("crypto_asset.exr");
    pc_io_exr_entry *v = NULL;
    size_t n = 0;
    const pc_io_exr_entry *e;
    must(pc_io_exr_entries(aov.data, aov.len, &v, &n, NULL));
    CHECK_EQ_UINT(n, 3);
    e = find_entry(v, n, "beauty");
    CHECK(e != NULL && e->kind == PC_IO_EXR_PART && e->channels == 4 && e->width == 4 && e->height == 4);
    e = find_entry(v, n, "diffuse");
    CHECK(e != NULL && e->kind == PC_IO_EXR_PART && e->channels == 3);
    e = find_entry(v, n, "Z");
    CHECK(e != NULL && e->kind == PC_IO_EXR_PART);
    pc_io_exr_entries_free(v, n);
    v = NULL;
    n = 0;
    must(pc_io_exr_entries(crypto.data, crypto.len, &v, &n, NULL));
    e = find_entry(v, n, "CryptoAsset");
    CHECK(e != NULL && e->kind == PC_IO_EXR_CRYPTOMATTE && e->has_objects && e->objects == 2);
    pc_io_exr_entries_free(v, n);
    pc_buf_fini(&aov);
    pc_buf_fini(&crypto);
}

static void test_entries_refuses_only_deep(void)
{
    /* The deep-only EXRs under corpus are refused by entries(). Skip if the corpus is absent. */
    pc_buf b = PC_BUF_INIT;
    pc_io_exr_entry *v = NULL;
    size_t n = 0;
    pc_error e;
    if (pc_file_read("corpus/exr/11.deep.exr", (size_t)16 << 20, &b, NULL) != PC_OK) {
        return;
    }
    CHECK(pc_io_exr_entries(b.data, b.len, &v, &n, &e) == PC_ERR_UNSUPPORTED);
    CHECK_CONTAINS(e.msg, "deep");
    pc_io_exr_entries_free(v, n);
    pc_buf_fini(&b);
}

#ifndef PC_NO_EXR

static void test_import_parts_as_layers(void)
{
    pc_buf aov = fixture("aov.exr");
    const char *parts[2] = {"beauty", "diffuse"};
    pc_io_exr_selection sel = {parts, 2, NULL, 0, "both", false};
    pc_io_import_result r;
    must(pc_io_exr_import("render.exr", aov.data, aov.len, &sel, &r, NULL));
    CHECK_EQ_UINT(r.document.nlayers, 2);
    CHECK_EQ_INT(r.document.mode, PC_COLOR_MODE_RGB);
    CHECK_EQ_INT(r.document.depth, PC_SAMPLE_TYPE_F32);
    CHECK(r.document.icc_profile != NULL); /* linear sRGB */
    CHECK_EQ_UINT(r.document.size.width, 4);
    CHECK_EQ_UINT(r.document.size.height, 4);
    CHECK(!r.source_read_only && !r.preview_only);
    if (r.document.nlayers == 2) {
        CHECK(pc_str_eq_c(&r.document.layers[0].name, "beauty"));
        CHECK(pc_str_eq_c(&r.document.layers[1].name, "diffuse"));
    }
    pc_io_import_result_fini(&r);
    pc_buf_fini(&aov);
}

static void test_import_deduplicates_repeated_parts(void)
{
    pc_buf aov = fixture("aov.exr");
    const char *parts[2] = {"beauty", "beauty"};
    pc_io_exr_selection sel = {parts, 2, NULL, 0, "both", false};
    pc_io_import_result r;
    must(pc_io_exr_import("render.exr", aov.data, aov.len, &sel, &r, NULL));
    CHECK_EQ_UINT(r.document.nlayers, 1);
    pc_io_import_result_fini(&r);
    pc_buf_fini(&aov);
}

static void test_import_errors(void)
{
    pc_buf aov = fixture("aov.exr");
    const char *none[1] = {"nope"};
    {
        /* No parts or groups, no precomp. */
        pc_io_exr_selection sel = {NULL, 0, NULL, 0, "both", false};
        pc_io_import_result r;
        pc_error e;
        CHECK(pc_io_exr_import("x.exr", aov.data, aov.len, &sel, &r, &e) == PC_ERR_UNSUPPORTED);
        CHECK_CONTAINS(e.msg, "no parts or groups selected");
    }
    {
        /* A part that does not exist. */
        pc_io_exr_selection sel = {none, 1, NULL, 0, "both", false};
        pc_io_import_result r;
        pc_error e;
        CHECK(pc_io_exr_import("x.exr", aov.data, aov.len, &sel, &r, &e) == PC_ERR_UNSUPPORTED);
        CHECK_CONTAINS(e.msg, "no flat part named nope");
    }
    {
        /* A beauty precomp needs the AOV light-path set, which these fixtures lack. */
        pc_io_exr_selection sel = {NULL, 0, NULL, 0, "both", true};
        pc_io_import_result r;
        pc_error e;
        CHECK(pc_io_exr_import("x.exr", aov.data, aov.len, &sel, &r, &e) == PC_ERR_UNSUPPORTED);
        CHECK_CONTAINS(e.msg, "incomplete for a beauty precomp");
    }
    pc_buf_fini(&aov);
}

static void test_import_grayscale_part(void)
{
    pc_buf b = fixture("single_y.exr");
    const char *parts[1] = {"main"};
    pc_io_exr_selection sel = {parts, 1, NULL, 0, "both", false};
    pc_io_import_result r;
    must(pc_io_exr_import("y.exr", b.data, b.len, &sel, &r, NULL));
    CHECK_EQ_UINT(r.document.nlayers, 1);
    CHECK_EQ_INT(r.document.mode, PC_COLOR_MODE_GRAYSCALE);
    CHECK(r.document.icc_profile == NULL); /* grayscale layers are not tagged linear sRGB here */
    pc_io_import_result_fini(&r);
    pc_buf_fini(&b);
}

static void test_import_half_warns(void)
{
    pc_buf b = fixture("half_rgb.exr");
    const char *parts[1] = {"half"};
    pc_io_exr_selection sel = {parts, 1, NULL, 0, "both", false};
    pc_io_import_result r;
    must(pc_io_exr_import("h.exr", b.data, b.len, &sel, &r, NULL));
    CHECK_EQ_UINT(r.document.nlayers, 1);
    CHECK(pc_io_warnings_any(&r.warnings, "32-bit float"));
    pc_io_import_result_fini(&r);
    pc_buf_fini(&b);
}

static void test_view_filter(void)
{
    pc_buf b = fixture("stereo.exr");
    pc_io_import_result r;
    pc_error e;
    {
        const char *parts[1] = {"beauty_right"};
        pc_io_exr_selection sel = {parts, 1, NULL, 0, "left", false};
        CHECK(pc_io_exr_import("s.exr", b.data, b.len, &sel, &r, &e) == PC_ERR_UNSUPPORTED);
        CHECK_CONTAINS(e.msg, "view");
    }
    {
        const char *parts[1] = {"beauty_left"};
        pc_io_exr_selection sel = {parts, 1, NULL, 0, "left", false};
        must(pc_io_exr_import("s.exr", b.data, b.len, &sel, &r, NULL));
        CHECK_EQ_UINT(r.document.nlayers, 1);
        pc_io_import_result_fini(&r);
    }
    {
        const char *parts[2] = {"beauty_left", "beauty_right"};
        pc_io_exr_selection sel = {parts, 2, NULL, 0, "both", false};
        must(pc_io_exr_import("s.exr", b.data, b.len, &sel, &r, NULL));
        CHECK_EQ_UINT(r.document.nlayers, 2);
        pc_io_import_result_fini(&r);
    }
    pc_buf_fini(&b);
}

static void test_import_only_deep_refused(void)
{
    pc_buf b = PC_BUF_INIT;
    const char *parts[1] = {"anything"};
    pc_io_exr_selection sel = {parts, 1, NULL, 0, "both", false};
    pc_io_import_result r;
    pc_error e;
    if (pc_file_read("corpus/exr/11.deep.exr", (size_t)16 << 20, &b, NULL) != PC_OK) {
        return;
    }
    /* A deep-only file has no flat part to import. */
    CHECK(pc_io_exr_import("d.exr", b.data, b.len, &sel, &r, &e) != PC_OK);
    pc_buf_fini(&b);
}

#endif /* PC_NO_EXR */

int main(int argc, char **argv)
{
    /* Every test here decodes OpenEXR; skip the whole file when the build has no EXR support
     * (WITH_EXR=0). The -DPC_NO_EXR define reaches only src/codecs and tests/codecs, not
     * tests/io, so detect it at runtime like test_flat does rather than via the macro. */
    if (!pc_cformat_caps(PC_CF_EXR).read) {
        (void)printf("  (skipped: built without OpenEXR)\n");
        return pc_t_finish();
    }
    RUN(test_entries_list_parts_and_cryptomatte);
    RUN(test_entries_refuses_only_deep);
#ifndef PC_NO_EXR
    RUN(test_import_parts_as_layers);
    RUN(test_import_deduplicates_repeated_parts);
    RUN(test_import_errors);
    RUN(test_import_grayscale_part);
    RUN(test_import_half_warns);
    RUN(test_view_filter);
    RUN(test_import_only_deep_refused);
#endif
    return pc_t_finish();
}
