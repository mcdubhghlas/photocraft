/* Parity with the Rust reference for chunk I4, part 2 of 2 (part 1 is test_maps_parity.c):
 * patterns, gradient baking, brushes, text styles, type-layer import and the maps over the
 * corpus PSDs (skipped without corpus/), recomputed from tests/golden/io-maps-vectors.txt
 * (`rustref io-maps-vectors`). Results depending on a later io chunk's stubs count as pending. */
#include "maps_parity.h"

static void test_patterns(void)
{
    static const pc_color_mode modes[4] = {PC_COLOR_MODE_RGB, PC_COLOR_MODE_GRAYSCALE, PC_COLOR_MODE_CMYK,
                                           PC_COLOR_MODE_LAB};
    static const pc_sample_type samples[3] = {PC_SAMPLE_TYPE_U8, PC_SAMPLE_TYPE_U16, PC_SAMPLE_TYPE_F32};
    pc_pattern all[24];
    size_t nall = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_error e = {0};
        pc_status st = PC_OK;
        (void)res_reset();
        if (is_kind(l, "pblk") && l->n == 4) {
            pc_buf d = b64d(l->f[2]);
            st = pblk_res(&g_res, bdata(&d), d.len, &e);
            pc_buf_fini(&d);
            check_line(l, st, &e);
        } else if (is_kind(l, "pat") && l->n == 4) {
            pc_buf d = b64d(l->f[2]);
            pc_pattern *v = NULL;
            size_t n = 0;
            st = pc_io_read_pat(bdata(&d), d.len, &v, &n, &e);
            if (st == PC_OK) {
                sb(&g_res, "ok [");
                for (size_t k = 0; k < n; k++) {
                    sb(&g_res, "%s", k > 0 ? ";" : "");
                    m_pat_s(&g_res, &v[k]);
                }
                sb(&g_res, "]");
                pc_io_patterns_free(v, n);
            } else if (st != PC_ERR_NOMEM) {
                sb(&g_res, "err ");
                sq(&g_res, e.msg, strlen(e.msg));
                st = PC_OK;
            }
            pc_buf_fini(&d);
            check_line(l, st, &e);
        }
    }
    for (size_t m = 0; m < 4; m++) {
        for (size_t s = 0; s < 3; s++) {
            for (int a = 0; a < 2; a++) {
                pc_pattern p = m_pattern(modes[m], samples[s], a != 0);
                pc_psd_pattern psd;
                pc_psd_bytes blk;
                bool found = false;
                pc_pattern back;
                char name[64];
                (void)res_reset();
                must(pc_io_pattern_to_psd(&p, &psd, NULL));
                must(pc_psd_write_pattern_block(&psd, 1, &blk, NULL));
                m_pat_s(&g_res, &p);
                sb(&g_res, " ");
                sb64(&g_res, blk.data, blk.len);
                sb(&g_res, " ");
                must(pc_io_pattern_from_psd(&psd, &found, &back, NULL));
                if (found) {
                    m_pat_s(&g_res, &back);
                    pc_pattern_fini(&back);
                } else {
                    sb(&g_res, "none");
                }
                (void)snprintf(name, sizeof name, "to/%s/%s/%s", pc_color_mode_name(modes[m]),
                               pc_sample_type_name(samples[s]), a != 0 ? "true" : "false");
                ptx_check(name, pc_buf_cstr(&g_res));
                pc_psd_bytes_fini(&blk);
                pc_psd_pattern_fini(&psd);
                all[nall++] = p;
            }
        }
    }
    {
        pc_pixel_format fmt = {PC_COLOR_MODE_INDEXED, PC_SAMPLE_TYPE_U8, false};
        pc_surface surf;
        pc_pattern odd;
        pc_psd_pattern psd;
        pc_psd_bytes blk;
        static const float px[3] = {0.2f, 0.4f, 0.6f};
        pc_surface_init(&surf, fmt);
        must(pc_pattern_new("Odd \xC3\xA9", 6, &surf, 2, 2, &odd, NULL));
        must(pc_surface_write_pixel(&odd.surface, 1, 1, px, 3, NULL));
        must(pc_io_pattern_to_psd(&odd, &psd, NULL));
        must(pc_psd_write_pattern_block(&psd, 1, &blk, NULL));
        (void)res_reset();
        m_pat_s(&g_res, &odd);
        sb(&g_res, " ");
        sb64(&g_res, blk.data, blk.len);
        ptx_check("to/indexed", pc_buf_cstr(&g_res));
        pc_psd_bytes_fini(&blk);
        pc_psd_pattern_fini(&psd);
        pc_pattern_fini(&odd);
    }
    {
        pc_pattern p = m_pattern(PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, true);
        pc_psd_pattern psd;
        pc_psd_bytes raw;
        pc_document d = t_doc("x", 4, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
        pc_document d16;
        pc_document fresh = t_doc("x", 4, 4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U16);
        static const uint8_t one = 1;
        static const uint8_t zero4[4] = {0, 0, 0, 0};
        pc_buf pat = PC_BUF_INIT;
        pc_buf small = PC_BUF_INIT;
        must(pc_io_pattern_to_psd(&p, &psd, NULL));
        must(pc_psd_write_pattern_block(&psd, 1, &raw, NULL));
        m_push_global(&d, "Txt2", &one, 1);
        m_push_global(&d, "Patt", raw.data, raw.len);
        m_push_global(&d, "Pat2", zero4, 4);
        must(pc_io_patterns_from_global_blocks(&d, &d.patterns, &d.npatterns, NULL));
        ptx_export("same", &d);
        push_pattern(&d, m_pattern(PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16, true));
        ptx_export("added", &d);
        must(pc_document_clone(&d, &d16, NULL));
        d16.depth = PC_SAMPLE_TYPE_F32;
        ptx_export("added32", &d16);
        pc_io_patterns_free(d.patterns, d.npatterns);
        d.patterns = NULL;
        d.npatterns = 0;
        ptx_export("cleared", &d);
        m_push_global(&fresh, "Txt2", &one, 1);
        for (size_t i = 0; i < 3; i++) {
            pc_pattern c;
            must(pc_pattern_clone(&all[i], &c, NULL));
            push_pattern(&fresh, c);
        }
        ptx_export("fresh", &fresh);
        must(pc_io_write_pat(all, nall, &pat, NULL));
        (void)res_reset();
        sh16(&g_res, pat.data, pat.len);
        ptx_check("write-pat", pc_buf_cstr(&g_res));
        must(pc_io_write_pat(all, 2, &small, NULL));
        (void)res_reset();
        sb64(&g_res, small.data, small.len);
        ptx_check("write-pat2", pc_buf_cstr(&g_res));
        pc_buf_fini(&pat);
        pc_buf_fini(&small);
        pc_psd_bytes_fini(&raw);
        pc_psd_pattern_fini(&psd);
        pc_pattern_fini(&p);
        pc_document_fini(&d);
        pc_document_fini(&d16);
        pc_document_fini(&fresh);
    }
    for (size_t i = 0; i < nall; i++) {
        pc_pattern_fini(&all[i]);
    }
}

static void test_gradients(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        char *copy;
        char *code;
        char *smooth;
        char *mids;
        char *stops;
        float mv[8];
        size_t nm = 0;
        pc_io_color_stop sv[8];
        size_t ns = 0;
        pc_io_color_stop *out = NULL;
        size_t nout = 0;
        pc_error e = {0};
        pc_status st;
        if (!is_kind(l, "gb") || l->n != 4) {
            continue;
        }
        (void)res_reset();
        copy = pc_strndup(l->f[2], strlen(l->f[2]));
        code = copy;
        smooth = strchr(code, '|');
        *smooth++ = '\0';
        mids = strchr(smooth, '|');
        *mids++ = '\0';
        stops = strchr(mids, '|');
        *stops++ = '\0';
        for (char *p = mids; *p != '\0' && nm < 8;) {
            mv[nm++] = bits_f(p);
            p = strchr(p, ',');
            p = p != NULL ? p + 1 : mids + strlen(mids);
        }
        for (char *p = stops; *p != '\0' && ns < 8;) {
            unsigned m;
            unsigned t;
            unsigned c[5];
            if (sscanf(p, "%x@%u:%x:%x:%x:%x:%x", &t, &m, &c[0], &c[1], &c[2], &c[3], &c[4]) != 7) {
                abort();
            }
            memcpy(&sv[ns].at, &t, 4);
            sv[ns].color.mode = (pc_color_mode)m;
            for (size_t k = 0; k < 4; k++) {
                memcpy(&sv[ns].color.c[k], &c[k], 4);
            }
            memcpy(&sv[ns].color.alpha, &c[4], 4);
            ns++;
            p = strchr(p, ';');
            p = p != NULL ? p + 1 : stops + strlen(stops);
        }
        st = pc_io_gradient_bake(sv, ns, mv, nm, bits_f(smooth),
                                 pc_io_bake_method_from_code((const uint8_t *)code, strlen(code)), &out,
                                 &nout, &e);
        for (size_t k = 0; st == PC_OK && k < nout; k++) {
            sb(&g_res, "%s%08x@", k > 0 ? ";" : "", fb(out[k].at));
            color_s(&g_res, &out[k].color);
        }
        free(out);
        free(copy);
        check_line(l, st, &e);
    }
}

static void test_abr(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_error e = {0};
        pc_status st;
        if (is_kind(l, "abr") && l->n == 4) {
            pc_buf d = b64d(l->f[2]);
            (void)res_reset();
            pc_buf want = PC_BUF_INIT;
            st = abr_res(&g_res, bdata(&d), d.len, &e);
            pc_buf_fini(&d);
            if (strstr(l->f[3], "\\0") != NULL) {
                cut_at_nul(l->f[3], strstr(l->f[3], "W[\"section ") != NULL, &want);
            } else {
                must(pc_buf_append_str(&want, l->f[3], NULL));
            }
            if (ok_or_pending(st, &e, l->f[1])) {
                CHECK_RESULT(l->f[1], pc_buf_cstr(&want), pc_buf_cstr(&g_res));
            }
            pc_buf_fini(&want);
        }
    }
    /* Generated inputs: the hash in front pins the input bytes. */
    {
        pc_psd_abr_sample s;
        pc_psd_bytes b;
        const gline *l = find_line("abrg", "big");
        pc_error e = {0};
        pc_status st;
        memset(&s, 0, sizeof s);
        must(pc_psd_str_set(&s.id, "$big", 4, NULL));
        s.width = 3000;
        s.height = 1500;
        s.depth = 8;
        s.data.len = (size_t)3000 * 1500;
        s.data.data = xmalloc(s.data.len);
        memset(s.data.data, 200, s.data.len);
        must(pc_psd_abr_write_v6(2, &s, 1, NULL, 0, NULL, 0, true, &b, NULL));
        (void)res_reset();
        sh16(&g_res, b.data, b.len);
        sb(&g_res, " ");
        st = abr_res(&g_res, b.data, b.len, &e);
        if (l != NULL && l->n == 3) {
            check_line(l, st, &e);
        }
        pc_psd_bytes_fini(&b);
        pc_psd_abr_sample_fini(&s);
    }
}

static void test_text_styles(void)
{
    static const char *const names[3] = {"linked", "plain", "unlinked"};
    for (int c = 0; c < 3; c++) {
        pc_text_styles st;
        pc_text_layer t;
        pc_psd_bytes tysh;
        bool has = false;
        pc_buf out = PC_BUF_INIT;
        const uint8_t *data;
        size_t len;
        char name[64];
        if (c == 1) {
            must(pc_text_styles_default(&st, NULL));
        } else {
            st = m_styles();
        }
        t = m_text_layer(&st, c == 0);
        must(pc_text_build_tysh(&t, 72.0f, NULL, &tysh, NULL));
        must(pc_io_export_tysh(tysh.data, tysh.len, &t, &st, 72.0f, &has, &out, NULL));
        (void)res_reset();
        if (has) {
            sb(&g_res, "some ");
            sb64(&g_res, out.data, out.len);
        } else {
            sb(&g_res, "none");
        }
        (void)snprintf(name, sizeof name, "export/%s", names[c]);
        ts_check(name, pc_buf_cstr(&g_res));
        data = has ? out.data : tysh.data;
        len = has ? out.len : tysh.len;
        for (int pre = 0; pre < 2; pre++) {
            pc_document d = ts_doc(data, len);
            if (pre) {
                d.text_styles.character = xmalloc(sizeof *d.text_styles.character);
                memset(d.text_styles.character, 0, sizeof *d.text_styles.character);
                d.text_styles.character[0].id = 9;
                must(pc_str_set_c(&d.text_styles.character[0].name, "Loud", NULL));
                d.text_styles.ncharacter = 1;
            }
            (void)res_reset();
            ts_import_s(&g_res, &d);
            (void)snprintf(name, sizeof name, "import/%s/%s", names[c], pre ? "true" : "false");
            ts_check(name, pc_buf_cstr(&g_res));
            pc_document_fini(&d);
        }
        pc_buf_fini(&out);
        pc_psd_bytes_fini(&tysh);
        pc_text_layer_fini(&t);
        pc_text_styles_fini(&st);
    }
    {
        pc_text_styles st = m_styles();
        pc_text_layer t = m_text_layer(&st, true);
        pc_psd_bytes tysh;
        bool has = false;
        pc_buf out = PC_BUF_INIT;
        pc_document d;
        must(pc_text_build_tysh(&t, 72.0f, NULL, &tysh, NULL));
        must(pc_io_export_tysh(tysh.data, tysh.len, &t, &st, 72.0f, &has, &out, NULL));
        d = ts_doc(out.data, out.len);
        pc_layer_fini(&d.layers[1]);
        d.nlayers = 1;
        d.text_styles.character = xmalloc(sizeof *d.text_styles.character);
        memset(d.text_styles.character, 0, sizeof *d.text_styles.character);
        d.text_styles.character[0].id = UINT32_MAX;
        must(pc_str_set_c(&d.text_styles.character[0].name, "Max", NULL));
        d.text_styles.ncharacter = 1;
        d.text_styles.paragraph = xmalloc(sizeof *d.text_styles.paragraph);
        memset(d.text_styles.paragraph, 0, sizeof *d.text_styles.paragraph);
        d.text_styles.paragraph[0].id = UINT32_MAX;
        must(pc_str_set_c(&d.text_styles.paragraph[0].name, "MaxP", NULL));
        d.text_styles.nparagraph = 1;
        {
            pc_io_warnings w = PC_IO_WARNINGS_INIT;
            must(pc_io_text_styles_import(&d, &w, NULL));
            (void)res_reset();
            JW(&g_res, pc_text_styles, &d.text_styles);
            sb(&g_res, " ");
            s_warn(&g_res, w.v, w.n);
            pc_io_warnings_fini(&w);
        }
        ts_check("exhausted", pc_buf_cstr(&g_res));
        pc_document_fini(&d);
        pc_buf_fini(&out);
        pc_psd_bytes_fini(&tysh);
        pc_text_layer_fini(&t);
        pc_text_styles_fini(&st);
    }
}

static void test_text_import(void)
{
    pc_document d = t_doc("t", 120, 60, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8);
    pc_text_layer t;
    pc_layer l;
    pc_layer *kids = xmalloc(2 * sizeof *kids);
    static const struct {
        const char *name;
        pc_color_mode mode;
        pc_sample_type depth;
    } cases[3] = {{"rgb8", PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8},
                  {"gray16", PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16},
                  {"cmyk8", PC_COLOR_MODE_CMYK, PC_SAMPLE_TYPE_U8}};
    t = ti_text("Hello", "Inter", NULL, 24.0f);
    l = m_text("plain", &t);
    t_push(&d, &l);
    t = ti_text("Semi", "Inter Semi Bold Guess", "Inter-SemiBold", 18.0f);
    l = m_text("ps", &t);
    t_push(&d, &l);
    t = ti_text("code()", "JetBrains Mono", "JetBrainsMono-Regular", 12.0f);
    l = m_text("mono", &t);
    t_push(&d, &l);
    t = ti_text("Fall back", "No Such Family", "NoSuchFont-Bold", 14.0f);
    l = m_text("unknown", &t);
    t_push(&d, &l);
    t = ti_text("W", "Inter", NULL, 30000.0f);
    l = m_text("huge", &t);
    t_push(&d, &l);
    t = ti_text("kept", "Inter", "Inter-Medium", 10.0f);
    pc_surface_init(&t.cache, PC_PIXEL_FORMAT_RGBA8);
    t.has_cache = true;
    kids[0] = m_text("cached", &t);
    t = ti_text("inner", "Inter", NULL, 9.0f);
    kids[1] = m_text("inner", &t);
    must(pc_layer_group(&l, "grp", kids, 2, NULL));
    t_push(&d, &l);
    t = ti_text("Arc", "Inter", NULL, 20.0f);
    t.has_warp = true;
    must(pc_str_set_c(&t.warp.style, "warpArc", NULL));
    t.warp.value = 50.0f;
    t.warp.horizontal = true;
    l = m_text("warp", &t);
    t_push(&d, &l);
    for (size_t c = 0; c < 3; c++) {
        pc_document dd;
        pc_error e = {0};
        pc_status st;
        const gline *gl = find_line("ti", cases[c].name);
        must(pc_document_clone(&d, &dd, NULL));
        dd.mode = cases[c].mode;
        dd.depth = cases[c].depth;
        dd.resolution_dpi = cases[c].depth == PC_SAMPLE_TYPE_U16 ? 144.0f : 72.0f;
        st = pc_io_text_prepare(&dd, &e);
        (void)res_reset();
        if (st == PC_OK) {
            ti_summary(&g_res, &dd);
        }
        if (gl != NULL && gl->n == 3) {
            check_line(gl, st, &e);
        } else {
            PC_T_FAIL("ti %s missing from the golden", cases[c].name);
        }
        pc_document_fini(&dd);
    }
    pc_document_fini(&d);
}

static void test_corpus(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf bytes = PC_BUF_INIT;
        pc_psd_file f;
        bool blocks = is_kind(l, "corp");
        if ((!blocks && !is_kind(l, "corpi")) || l->n != 3) {
            continue;
        }
        if (pc_file_read(l->f[1], (size_t)1 << 32, &bytes, NULL) != PC_OK) {
            g_skipped++;
            continue;
        }
        if (pc_psd_file_parse(bdata(&bytes), bytes.len, &f, NULL) != PC_OK) {
            PC_T_FAIL("%s: does not parse", l->f[1]);
            pc_buf_fini(&bytes);
            continue;
        }
        (void)res_reset();
        if (blocks) {
            corpus_blocks(&g_res, &f);
            CHECK_RESULT(l->f[1], l->f[2], pc_buf_cstr(&g_res));
        } else {
            pc_document d;
            pc_io_warnings w = PC_IO_WARNINGS_INIT;
            pc_error e = {0};
            pc_status st = pc_io_psd_to_document(&f, &d, &w, &e);
            if (ok_or_pending(st, &e, l->f[1])) {
                corpus_doc(&g_res, &d);
                CHECK_RESULT(l->f[1], l->f[2], pc_buf_cstr(&g_res));
                pc_document_fini(&d);
            }
            pc_io_warnings_fini(&w);
        }
        pc_psd_file_fini(&f);
        pc_buf_fini(&bytes);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* Bundled fonts only, like rustref (text rendering must not depend on what is installed). */
    (void)setenv("PHOTOCRAFT_NO_SYSTEM_FONTS", "1", 1);
    if (load_golden() != 0) {
        (void)fprintf(stderr, "cannot read %s (run from the repository root)\n", GOLDEN);
        return 1;
    }
    RUN(test_patterns);
    RUN(test_gradients);
    RUN(test_abr);
    RUN(test_text_styles);
    RUN(test_text_import);
    RUN(test_corpus);
    golden_summary("io maps parity (2/2)");
    pc_buf_fini(&g_res);
    return pc_t_finish();
}
