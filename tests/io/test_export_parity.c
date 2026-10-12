/* Export parity with the Rust reference (tests/golden/io-core-vectors.txt, see
 * test_core_parity.c): document_to_image of every named document, every writable flat format
 * (the file decoded again, pixels compared exactly except for JPEG and GIF), .pcraft round trips,
 * export options, extension forms and errors, and layered-TIFF export decisions. Encoded bytes
 * differ from the reference's by design (REWRITE_PLAN.md §7 F10-F13); what they decode to may
 * not. Formats this build can't write (EXR without OpenEXR) are skipped. */
#include "harness.h"

#include "docs.h"
#include "golden.h"

static bool lossy_ext(const char *ext)
{
    return strcmp(ext, "jpg") == 0 || strcmp(ext, "gif") == 0;
}

/* "ok W[...] <decoded image>" for an export, as the reference prints it. */
static void exp_result(pc_buf *s, pc_status st, const pc_io_export_result *r, const pc_error *e, bool lossy)
{
    pc_buf_truncate(s, 0);
    if (st != PC_OK) {
        s_err(s, st, e);
        return;
    }
    {
        pc_cimage img;
        pc_error de;
        pc_status ds = pc_cdecode(r->bytes, r->len, &img, &de);
        sb(s, "ok ");
        s_warn(s, r->warnings.v, r->warnings.n);
        if (ds == PC_OK) {
            sb(s, " ");
            s_img(s, &img, lossy);
            pc_cimage_fini(&img);
        } else {
            sb(s, " undecodable ");
            sq(s, de.msg, strlen(de.msg));
        }
    }
}

static bool c_can_write_ext(const char *ext)
{
    pc_cformat f;
    return !pc_cformat_from_extension(ext, &f) || pc_cformat_caps(f).write;
}

static void test_document_to_image(void)
{
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_document d;
        pc_io_warnings w = PC_IO_WARNINGS_INIT;
        pc_cimage img;
        pc_error e;
        pc_status st;
        if (!is_kind(l, "d2i") || l->n != 3) {
            continue;
        }
        if (!t_export_doc(l->f[1], &d)) {
            PC_T_FAIL("unknown document %s", l->f[1]);
            continue;
        }
        st = pc_io_document_to_image(&d, &w, &img, &e);
        pc_buf_truncate(&s, 0);
        if (st == PC_OK) {
            s_img(&s, &img, false);
            sb(&s, " ");
            s_warn(&s, w.v, w.n);
            pc_cimage_fini(&img);
        } else {
            s_err(&s, st, &e);
        }
        CHECK_RESULT(l->f[1], l->f[2], pc_buf_cstr(&s));
        pc_io_warnings_fini(&w);
        pc_document_fini(&d);
    }
    pc_buf_fini(&s);
}

static void test_exports(void)
{
    pc_buf s = PC_BUF_INIT;
    char cur[64] = "";
    pc_document d;
    bool have = false;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_io_export_result r;
        pc_error e;
        pc_status st;
        char what[160];
        if (!is_kind(l, "exp") || l->n != 4) {
            continue;
        }
        if (strcmp(cur, l->f[1]) != 0) {
            if (have) {
                pc_document_fini(&d);
            }
            have = t_export_doc(l->f[1], &d);
            (void)snprintf(cur, sizeof cur, "%s", l->f[1]);
            if (!have) {
                PC_T_FAIL("unknown document %s", l->f[1]);
                continue;
            }
        }
        if (!have) {
            continue;
        }
        if (!c_can_write_ext(l->f[2])) {
            g_skipped++;
            continue;
        }
        (void)snprintf(what, sizeof what, "%s.%s", l->f[1], l->f[2]);
        st = pc_io_export(&d, l->f[2], NULL, &r, &e);
        if (strcmp(l->f[2], "pcraft") == 0) {
            pc_buf_truncate(&s, 0);
            if (st != PC_OK) {
                s_err(&s, st, &e);
            } else {
                pc_io_import_result back;
                pc_status bs = pc_io_import("x.pcraft", r.bytes, r.len, &back, &e);
                if (bs == PC_OK) {
                    sb(&s, "ok ");
                    s_warn(&s, r.warnings.v, r.warnings.n);
                    sb(&s, " ");
                    s_doc(&s, &back.document, false);
                    pc_io_import_result_fini(&back);
                } else {
                    sb(&s, "ok reload ");
                    s_err(&s, bs, &e);
                }
            }
        } else {
            exp_result(&s, st, &r, &e, lossy_ext(l->f[2]));
        }
        if (st == PC_OK) {
            pc_io_export_result_fini(&r);
        }
        CHECK_RESULT(what, l->f[3], pc_buf_cstr(&s));
    }
    if (have) {
        pc_document_fini(&d);
    }
    pc_buf_fini(&s);
}

static void test_export_options(void)
{
    pc_buf s = PC_BUF_INIT;
    pc_document meta;
    pc_document single;
    must(t_export_doc("meta", &meta) ? PC_OK : PC_ERR_INVALID);
    must(t_export_doc("single-Rgb-U8-0", &single) ? PC_OK : PC_ERR_INVALID);
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_io_export_opts o = pc_io_export_opts_default();
        pc_io_export_result r;
        pc_error e;
        pc_status st;
        if (is_kind(l, "expo") && l->n == 4) {
            char name[32];
            if (strcmp(l->f[1], "none") == 0) {
                o.xmp = PC_IO_XMP_NONE;
                (void)snprintf(name, sizeof name, "out.%s", l->f[2]);
            } else {
                o.encode.png_interlaced = true;
                o.encode.tiff_compression = PC_TIFF_LZW;
                o.encode.embed_icc = false;
                o.encode.embed_metadata = false;
                (void)snprintf(name, sizeof name, "%s", l->f[2]);
            }
            st = pc_io_export(&meta, name, &o, &r, &e);
            exp_result(&s, st, &r, &e, lossy_ext(l->f[2]));
            /* exp_result prints "ok W[..] IMG"; the reference prints "W[..] IMG". */
            CHECK_RESULT(l->f[1], l->f[3],
                         strncmp(pc_buf_cstr(&s), "ok ", 3) == 0 ? pc_buf_cstr(&s) + 3 : "?");
            if (st == PC_OK) {
                pc_io_export_result_fini(&r);
            }
        } else if (is_kind(l, "err") && l->n == 3 && strncmp(l->f[1], "export:", 7) == 0) {
            const char *name = l->f[1] + 7;
            st = pc_io_export(&single, name, NULL, &r, &e);
            pc_buf_truncate(&s, 0);
            if (st == PC_OK) {
                pc_cformat f;
                sb(&s, "ok ");
                s_warn(&s, r.warnings.v, r.warnings.n);
                sb(&s, " %s", pc_cformat_detect(r.bytes, r.len, &f) ? pc_cformat_debug(f) : "?");
                pc_io_export_result_fini(&r);
            } else if (is_pending(st, &e)) {
                g_pending++;
                continue;
            } else {
                s_err(&s, st, &e);
            }
            CHECK_RESULT(l->f[1], l->f[2], pc_buf_cstr(&s));
        }
    }
    pc_document_fini(&meta);
    pc_document_fini(&single);
    pc_buf_fini(&s);
}

static void test_tiff_layers_export(void)
{
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_document d;
        if (is_kind(l, "tl-would") && l->n == 3) {
            if (!t_export_doc(l->f[1], &d)) {
                PC_T_FAIL("unknown document %s", l->f[1]);
                continue;
            }
            CHECK_RESULT(l->f[1], l->f[2], pc_io_would_write_layers(&d) ? "1" : "0");
            pc_document_fini(&d);
        } else if (is_kind(l, "tl-exp") && l->n == 5) {
            pc_io_export_opts o = pc_io_export_opts_default();
            pc_io_export_result r;
            pc_error e;
            pc_status st;
            if (!t_tiff_doc(l->f[1], &d)) {
                PC_T_FAIL("unknown document %s", l->f[1]);
                continue;
            }
            o.tiff_layers = strcmp(l->f[2], "flat") != 0;
            o.force_psb = strcmp(l->f[2], "psb") == 0;
            st = pc_io_export(&d, "x.tif", &o, &r, &e);
            if (st != PC_OK && is_pending(st, &e)) {
                g_pending++;
            } else {
                exp_result(&s, st, &r, &e, false);
                if (d.depth == PC_SAMPLE_TYPE_F32) {
                    /* 32-bit layer channels are zip-compressed (zlib here, miniz in the reference):
                     * the 37724 block's bytes differ; its decompressed content is checked by
                     * test_export_psd_parity (I3). Compare everything else. */
                    char want[2048];
                    char *p;
                    char *q;
                    (void)snprintf(want, sizeof want, "%s", l->f[3]);
                    if ((p = strstr(want, " psl=")) != NULL) {
                        *p = '\0';
                    }
                    if ((q = strstr((char *)(uintptr_t)pc_buf_cstr(&s), " psl=")) != NULL) {
                        pc_buf_truncate(&s, (size_t)(q - pc_buf_cstr(&s)));
                    }
                    CHECK_RESULT(l->f[1], want,
                                 strncmp(pc_buf_cstr(&s), "ok ", 3) == 0 ? pc_buf_cstr(&s) + 3 : pc_buf_cstr(&s));
                } else {
                    CHECK_RESULT(l->f[1], l->f[3],
                                 strncmp(pc_buf_cstr(&s), "ok ", 3) == 0 ? pc_buf_cstr(&s) + 3
                                                                         : pc_buf_cstr(&s));
                }
            }
            if (st == PC_OK) {
                pc_io_export_result_fini(&r);
            }
            pc_document_fini(&d);
        }
    }
    pc_buf_fini(&s);
}

int main(int argc, char **argv)
{
    if (load_golden() != 0) {
        (void)fprintf(stderr, "cannot read %s (run from the repository root)\n", GOLDEN);
        return 1;
    }
    RUN(test_document_to_image);
    RUN(test_exports);
    RUN(test_export_options);
    RUN(test_tiff_layers_export);
    golden_summary("io export parity");
    return pc_t_finish();
}
