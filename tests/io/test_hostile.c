/* Never crash (AGENTS.md, REWRITE_PLAN.md §4): hostile files of every format through the import
 * API (truncations and byte flips), absurd document sizes through export, cancellation and
 * progress, NULL / empty arguments, hostile Multichannel planes and DisplayInfo, and imports
 * running on several threads at once. Every call must return a status (ASan / UBSan / TSan
 * builds check the memory side). io does no file I/O of its own (bytes in, bytes out), so there
 * are no unwritable-path cases here. */
#include <pthread.h>

#include "harness.h"

#include "docs.h"
#include "psd/tiff.h"

/* Imports and drops the result; returns the status. */
static pc_status try_import(const char *name, const uint8_t *b, size_t n)
{
    pc_io_import_result r;
    pc_error e;
    pc_status st = pc_io_import(name, b, n, &r, &e);
    if (st == PC_OK) {
        pc_io_import_result_fini(&r);
    } else {
        CHECK(e.msg[0] != '\0');
    }
    return st;
}

/* Every prefix (stepped) and a byte flip every `flip` bytes. */
static void sweep(const char *name, const uint8_t *b, size_t n, size_t step, size_t flip)
{
    uint8_t *c = xmalloc(n);
    for (size_t k = 0; k < n; k += step) {
        (void)try_import(name, b, k);
    }
    for (size_t i = 0; i < n; i += flip) {
        memcpy(c, b, n);
        c[i] ^= 0x5a;
        (void)try_import(name, c, n);
        c[i] ^= 0xff;
        (void)try_import(name, c, n);
    }
    free(c);
}

static void test_hostile_files_of_every_format(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_document s = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16, true);
    pc_io_export_opts layers = pc_io_export_opts_default();
    layers.tiff_layers = true;
    for (int f = 0; f < PC_CF_COUNT; f++) {
        const char *ext;
        pc_io_export_result r;
        char name[16];
        if (!pc_cformat_caps((pc_cformat)f).write || !pc_cformat_caps((pc_cformat)f).read) {
            continue;
        }
        ext = pc_cformat_extensions((pc_cformat)f)[0];
        (void)snprintf(name, sizeof name, "x.%s", ext);
        must(pc_io_export(f == PC_CF_ICO ? &s : &d, ext, NULL, &r, NULL));
        sweep(name, r.bytes, r.len, r.len > 4000 ? 13 : 3, r.len > 4000 ? 97 : 7);
        sweep("noext", r.bytes, r.len, r.len > 4000 ? 61 : 11, r.len > 4000 ? 211 : 17);
        pc_io_export_result_fini(&r);
    }
    {
        /* .pcraft bundles, by extension and by content. */
        pc_io_export_result r;
        must(pc_io_export(&s, "x.pcraft", NULL, &r, NULL));
        sweep("x.pcraft", r.bytes, r.len, 29, 53);
        sweep("noext", r.bytes, r.len, 97, 131);
        pc_io_export_result_fini(&r);
    }
    {
        /* Layered-TIFF tags are only written once I3 lands; the flat TIFF with Photoshop tags
         * added by hand covers the layered import path meanwhile. */
        pc_io_export_result r;
        pc_status st = pc_io_export(&d, "x.tif", &layers, &r, NULL);
        if (st == PC_OK) {
            sweep("x.tif", r.bytes, r.len, 11, 29);
            pc_io_export_result_fini(&r);
        }
    }
    pc_document_fini(&d);
    pc_document_fini(&s);
}

static void test_hostile_layered_tiff_tags(void)
{
    /* A flat TIFF carrying a hand-made ImageSourceData tag: broken data opens flattened. */
    pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    pc_cimage img;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_cencode_opts o = pc_cencode_opts_default();
    pc_buf out = PC_BUF_INIT;
    pc_io_import_result r;
    uint8_t isd[60];
    memcpy(isd, PC_PSD_ISD_SIGNATURE, 36);
    memset(isd + 36, 0xa5, sizeof isd - 36);
    must(pc_io_document_to_image(&d, &w, &img, NULL));
    must(pc_meta_blob_set(&img.meta.photoshop_layers, isd, sizeof isd, NULL));
    must(pc_meta_blob_set(&img.meta.photoshop_resources, "8BIM\x03\xed\0\0\0\0\0\x05xyz", 15, NULL));
    must(pc_cencode(&img, PC_CF_TIFF, &o, &out, NULL));
    must(pc_io_import("x.tif", out.data, out.len, &r, NULL));
    CHECK_EQ_UINT(r.document.nlayers, 1);
    CHECK(pc_io_warnings_any(&r.warnings, "could not be read"));
    pc_io_import_result_fini(&r);
    sweep("x.tif", out.data, out.len, 7, 5);
    pc_buf_fini(&out);
    pc_cimage_fini(&img);
    pc_io_warnings_fini(&w);
    pc_document_fini(&d);
}

static void test_absurd_sizes_are_errors(void)
{
    static const char *exts[5] = {"png", "tif", "jpg", "bmp", "pcraft"};
    /* Larger than any buffer the exporters may allocate: an error each, never an abort. */
    pc_document d = t_doc("huge", 200000, 200000, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32);
    pc_document g = t_doc("huge", 200000, 200000, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_layer l;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_cimage img;
    pc_error e;
    must(pc_layer_raster(&l, "a", pc_document_pixel_format(&g), NULL));
    t_push(&g, &l);
    must(pc_layer_raster(&l, "b", pc_document_pixel_format(&g), NULL));
    t_push(&g, &l);
    for (int i = 0; i < 4; i++) {
        pc_io_export_result r;
        if (pc_io_export(&d, exts[i], NULL, &r, &e) == PC_OK) {
            PC_T_FAIL("%s of a 200000 x 200000 F32 document succeeded", exts[i]);
            pc_io_export_result_fini(&r);
        }
        if (pc_io_export(&g, exts[i], NULL, &r, &e) == PC_OK) {
            PC_T_FAIL("%s of a 200000 x 200000 layered document succeeded", exts[i]);
            pc_io_export_result_fini(&r);
        }
    }
    {
        /* .pcraft stores tiles, not a canvas: an empty huge document saves (as in the reference). */
        pc_io_export_result r;
        if (pc_io_export(&d, "pcraft", NULL, &r, &e) == PC_OK) {
            pc_io_export_result_fini(&r);
        }
    }
    CHECK(pc_io_document_to_image(&d, &w, &img, &e) != PC_OK);
    pc_io_warnings_fini(&w);
    pc_document_fini(&d);
    pc_document_fini(&g);
    {
        /* Zero-sized documents. */
        pc_document z = t_doc("z", 0, 0, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        for (int i = 0; i < 5; i++) {
            pc_io_export_result r;
            if (pc_io_export(&z, exts[i], NULL, &r, &e) == PC_OK) {
                pc_io_export_result_fini(&r);
            }
        }
        pc_document_fini(&z);
    }
}

/* ---- cancellation ---- */

typedef struct ctl_state {
    int calls;
    int cancel_at; /* cancel on this call to the cancel callback (0 = never) */
    float last;
    int reports;
    bool backwards;
} ctl_state;

static bool cancel_cb(void *ctx)
{
    ctl_state *s = ctx;
    s->calls++;
    return s->cancel_at != 0 && s->calls >= s->cancel_at;
}

static void progress_cb(void *ctx, float f)
{
    ctl_state *s = ctx;
    s->backwards = s->backwards || f < s->last;
    s->last = f;
    s->reports++;
}

static void test_cancelled_import_fails_with_cancelled(void)
{
    pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    pc_io_export_result png;
    must(pc_io_export(&d, "png", NULL, &png, NULL));
    for (int at = 1; at <= 3; at++) {
        ctl_state s = {0, at, 0, 0, false};
        pc_interrupt ctl = {cancel_cb, progress_cb, &s};
        pc_io_import_result r;
        pc_error e;
        pc_status st = pc_io_import_with("t.png", png.bytes, png.len, &ctl, &r, &e);
        if (at <= 2) {
            CHECK(st == PC_ERR_CANCELLED);
            CHECK_EQ_STR(e.msg, "cancelled");
            CHECK(pc_io_error_kind(st, &e) == PC_IO_ERR_CANCELLED);
        } else if (st == PC_OK) {
            pc_io_import_result_fini(&r);
        }
    }
    {
        /* Not cancelled: progress is monotonic and ends at 1. */
        ctl_state s = {0, 0, 0, 0, false};
        pc_interrupt ctl = {cancel_cb, progress_cb, &s};
        pc_io_import_result r;
        must(pc_io_import_with("t.png", png.bytes, png.len, &ctl, &r, NULL));
        CHECK(!s.backwards && s.last == 1.0f && s.reports >= 1);
        pc_io_import_result_fini(&r);
    }
    pc_io_export_result_fini(&png);
    pc_document_fini(&d);
}

static void test_null_and_empty_arguments(void)
{
    pc_io_import_result r;
    pc_io_export_result x;
    pc_error e;
    pc_document d = t_single(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, false);
    CHECK(pc_io_import(NULL, NULL, 0, &r, &e) != PC_OK);
    CHECK(pc_io_import("", (const uint8_t *)"", 0, &r, NULL) != PC_OK);
    CHECK(pc_io_export(&d, NULL, NULL, &x, &e) == PC_ERR_NOT_FOUND);
    CHECK(pc_io_export(&d, "....", NULL, &x, &e) == PC_ERR_NOT_FOUND);
    CHECK(!pc_io_is_psd(NULL, 0) && !pc_io_is_svg(NULL, 0) && !pc_io_affinity_has_extension(""));
    pc_io_import_result_fini(NULL);
    pc_io_export_result_fini(NULL);
    pc_document_fini(&d);
}

/* ---- hostile Multichannel and DisplayInfo ---- */

static void test_hostile_multichannel_planes(void)
{
    pc_psd_file f;
    pc_document d;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    uint8_t plane[12];
    memset(plane, 0x40, sizeof plane);
    pc_psd_file_init(&f);
    /* Three channels declared, one plane given: a warning, no overrun. */
    f.header = pc_psd_header_make(PC_PSD_V1, 4, 3, 3, 8, PC_PSD_MODE_MULTICHANNEL);
    d = t_doc("m", 4, 3, PC_COLOR_MODE_MULTICHANNEL, PC_SAMPLE_TYPE_U8);
    must(pc_io_multichannel_import(&f, plane, sizeof plane, &d, NULL, 0, &w, NULL));
    CHECK_EQ_UINT(d.nchannels, 1);
    CHECK(w.n == 1 && strcmp(w.v[0], "multichannel: channel 2 is missing") == 0);
    CHECK(pc_str_eq_c(&d.channels[0].name, "Channel 1"));
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    /* A 1-bit header: planes are shorter than one byte per pixel; samples past them read 0. */
    f.header = pc_psd_header_make(PC_PSD_V1, 9, 2, 1, 1, PC_PSD_MODE_MULTICHANNEL);
    d = t_doc("m", 9, 2, PC_COLOR_MODE_MULTICHANNEL, PC_SAMPLE_TYPE_U8);
    must(pc_io_multichannel_import(&f, plane, 4, &d, NULL, 0, &w, NULL));
    CHECK_EQ_UINT(d.nchannels, 1);
    pc_document_fini(&d);
    pc_io_warnings_fini(&w);
    pc_psd_file_fini(&f);
}

static void test_hostile_display_info(void)
{
    pc_alpha_channel ch[3];
    uint8_t junk[64];
    for (int i = 0; i < 3; i++) {
        pc_surface s;
        pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
        must(pc_alpha_channel_init(&ch[i], "c", &s, NULL));
    }
    for (int seed = 0; seed < 200; seed++) {
        for (size_t i = 0; i < sizeof junk; i++) {
            junk[i] = (uint8_t)(i * 37 + (size_t)seed * 101);
        }
        pc_io_apply_display_info(junk, (size_t)seed % sizeof junk, seed % 2 == 0, ch, 3);
        pc_io_apply_display_info(junk, sizeof junk, seed % 2 == 1, ch, 0);
    }
    pc_io_apply_display_info(NULL, 0, true, ch, 3);
    for (int i = 0; i < 3; i++) {
        CHECK(ch[i].opacity >= 0.0f && ch[i].opacity <= 1.0f);
        pc_alpha_channel_fini(&ch[i]);
    }
}

/* ---- threads ---- */

typedef struct job {
    const uint8_t *bytes;
    size_t len;
    const char *name;
    int ok;
} job;

static void *run_job(void *arg)
{
    job *j = arg;
    for (int k = 0; k < 4; k++) {
        pc_io_import_result r;
        if (pc_io_import(j->name, j->bytes, j->len, &r, NULL) == PC_OK) {
            pc_io_export_result x;
            if (pc_io_export(&r.document, "tif", NULL, &x, NULL) == PC_OK) {
                j->ok++;
                pc_io_export_result_fini(&x);
            }
            pc_io_import_result_fini(&r);
        }
    }
    return NULL;
}

static void test_imports_on_several_threads(void)
{
    pc_document d = t_gen_doc(PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U16);
    pc_io_export_result png;
    pthread_t th[4];
    job jobs[4];
    must(pc_io_export(&d, "png", NULL, &png, NULL));
    for (int i = 0; i < 4; i++) {
        jobs[i] = (job){png.bytes, png.len, "t.png", 0};
        CHECK(pthread_create(&th[i], NULL, run_job, &jobs[i]) == 0);
    }
    for (int i = 0; i < 4; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
        CHECK_EQ_INT(jobs[i].ok, 4);
    }
    pc_io_export_result_fini(&png);
    pc_document_fini(&d);
}

int main(int argc, char **argv)
{
    RUN(test_hostile_files_of_every_format);
    RUN(test_hostile_layered_tiff_tags);
    RUN(test_absurd_sizes_are_errors);
    RUN(test_cancelled_import_fails_with_cancelled);
    RUN(test_null_and_empty_arguments);
    RUN(test_hostile_multichannel_planes);
    RUN(test_hostile_display_info);
    RUN(test_imports_on_several_threads);
    return pc_t_finish();
}
