/* Parity with the Rust reference for chunk I4, part 1 of 2 (part 2 is test_maps_parity2.c):
 * tests/golden/io-maps-vectors.txt was written by `rustref io-maps-vectors`
 * (tools/rustref/src/io_maps_vectors.rs, which documents the line kinds). Every line of the kinds
 * checked here (vector masks, paths, shape blocks, live shapes, comps and artboards, notes and the
 * measurement scale, slices) is recomputed and must match. Results that depend on a later io
 * chunk's stubs count as pending. */
#include "maps_parity.h"

static void test_vector(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_error e = {0};
        pc_status st = PC_OK;
        (void)res_reset();
        if (is_kind(l, "vrec") && l->n == 6) {
            pc_path p;
            pc_psd_path rec;
            pc_psd_bytes bytes;
            pc_path back;
            uint32_t w = u32_of(l->f[2]);
            uint32_t h = u32_of(l->f[3]);
            JR(pc_path, l->f[4], &p);
            st = pc_io_path_to_records(&p, w, h, &rec, &e);
            if (st == PC_OK) {
                must(pc_psd_path_to_bytes(&rec, &bytes, NULL));
                sb64(&g_res, bytes.data, bytes.len);
                pc_psd_bytes_fini(&bytes);
                st = pc_io_path_from_records(&rec, w, h, &back, &e);
                if (st == PC_OK) {
                    sb(&g_res, " ");
                    JW(&g_res, pc_path, &back);
                    pc_path_fini(&back);
                }
                pc_psd_path_fini(&rec);
            }
            pc_path_fini(&p);
            check_line(l, st, &e);
        } else if (is_kind(l, "vmskw") && l->n == 7) {
            pc_path p;
            pc_buf out = PC_BUF_INIT;
            JR(pc_path, l->f[5], &p);
            st = pc_io_vmsk_bytes(&p, u32_of(l->f[4]), u32_of(l->f[2]), u32_of(l->f[3]), &out, &e);
            sb64(&g_res, out.data, out.len);
            pc_buf_fini(&out);
            pc_path_fini(&p);
            check_line(l, st, &e);
        } else if (is_kind(l, "vmaskw") && l->n == 6) {
            pc_vector_mask m;
            pc_buf out = PC_BUF_INIT;
            JR(pc_vector_mask, l->f[4], &m);
            st = pc_io_vector_mask_bytes(&m, u32_of(l->f[2]), u32_of(l->f[3]), &out, &e);
            sb64(&g_res, out.data, out.len);
            pc_buf_fini(&out);
            pc_vector_mask_fini(&m);
            check_line(l, st, &e);
        } else if ((is_kind(l, "vmsk") || is_kind(l, "vres")) && l->n == 6) {
            pc_buf d = b64d(l->f[4]);
            uint32_t w = u32_of(l->f[2]);
            uint32_t h = u32_of(l->f[3]);
            st = is_kind(l, "vmsk") ? vmsk_res(&g_res, bdata(&d), d.len, w, h, &e)
                                    : path_res(&g_res, bdata(&d), d.len, w, h, &e);
            pc_buf_fini(&d);
            check_line(l, st, &e);
        } else if (is_kind(l, "vstkw") && l->n == 6) {
            pc_shape_stroke s;
            bool has = false;
            pc_buf out = PC_BUF_INIT;
            JR_OPT(pc_shape_stroke, l->f[4], &s, &has);
            st = pc_io_vstk_bytes(has ? &s : NULL, strcmp(l->f[3], "true") == 0, bits_f(l->f[2]), &out, &e);
            sb64(&g_res, out.data, out.len);
            pc_buf_fini(&out);
            if (has) {
                pc_shape_stroke_fini(&s);
            }
            check_line(l, st, &e);
        } else if (is_kind(l, "vstk") && l->n == 5) {
            pc_buf d = b64d(l->f[3]);
            st = vstk_res(&g_res, bdata(&d), d.len, bits_f(l->f[2]), &e);
            pc_buf_fini(&d);
            check_line(l, st, &e);
        } else if (is_kind(l, "vscgw") && l->n == 4) {
            pc_fill f;
            pc_buf out = PC_BUF_INIT;
            JR(pc_fill, l->f[2], &f);
            st = pc_io_vscg_bytes(&f, &out, &e);
            sb64(&g_res, out.data, out.len);
            pc_buf_fini(&out);
            pc_fill_fini(&f);
            check_line(l, st, &e);
        } else if ((is_kind(l, "vscg") || is_kind(l, "vogk")) && l->n == 4) {
            pc_buf d = b64d(l->f[2]);
            st = is_kind(l, "vscg") ? vscg_res(&g_res, bdata(&d), d.len, &e)
                                    : vogk_res(&g_res, bdata(&d), d.len, &e);
            pc_buf_fini(&d);
            check_line(l, st, &e);
        } else if (is_kind(l, "vogkw") && l->n == 5) {
            pc_live_shape s;
            bool has = false;
            pc_buf out = PC_BUF_INIT;
            JR(pc_live_shape, l->f[3], &s);
            st = pc_io_vogk_bytes(&s, bits_f(l->f[2]), &has, &out, &e);
            if (has) {
                sb(&g_res, "some ");
                sb64(&g_res, out.data, out.len);
            } else {
                sb(&g_res, "none");
            }
            pc_buf_fini(&out);
            check_line(l, st, &e);
        } else if (is_kind(l, "shape") && l->n == 5) {
            pc_shape_layer sh;
            pc_shape_layer back;
            pc_io_raw_blocks raw = {NULL, 0, 0};
            lookup_ctx ctx = {&raw};
            shape_from_json(l->f[2], &sh);
            parse_raw(l->f[3], &raw);
            st = pc_io_shape_blocks(&sh, &raw, 64, 64, 72.0f, &e);
            if (st == PC_OK) {
                raw_s(&g_res, &raw);
                memset(&back, 0, sizeof back);
                st = pc_io_shape_from_blocks(&back, lookup, &ctx, 64, 64, 72.0f, &e);
                if (st == PC_OK) {
                    sb(&g_res, " | ");
                    JW(&g_res, pc_path, &back.path);
                    sb(&g_res, " ");
                    JW_OPT(&g_res, pc_fill, back.has_fill, &back.fill);
                    sb(&g_res, " ");
                    JW_OPT(&g_res, pc_shape_stroke, back.has_stroke, &back.stroke);
                    sb(&g_res, " ");
                    JW_OPT(&g_res, pc_live_shape, back.has_live, &back.live);
                }
                pc_shape_layer_fini(&back);
            }
            pc_shape_layer_fini(&sh);
            pc_io_raw_blocks_fini(&raw);
            check_line(l, st, &e);
        }
    }
}

static void test_comps(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_error e = {0};
        pc_status st = PC_OK;
        (void)res_reset();
        if (is_kind(l, "art") && l->n == 4) {
            pc_buf d = b64d(l->f[2]);
            st = art_res(&g_res, bdata(&d), d.len, &e);
            pc_buf_fini(&d);
            check_line(l, st, &e);
        } else if (is_kind(l, "artw") && l->n == 5) {
            pc_artboard a;
            yyjson_doc *gd = jparse(l->f[3]);
            yyjson_val *arr = yyjson_doc_get_root(gd);
            int32_t g[8];
            size_t ng = 0;
            pc_buf out = PC_BUF_INIT;
            for (size_t k = 0; k < yyjson_arr_size(arr) && k < 8; k++) {
                g[ng++] = (int32_t)yyjson_get_sint(yyjson_arr_get(arr, k));
            }
            yyjson_doc_free(gd);
            JR(pc_artboard, l->f[2], &a);
            st = pc_io_write_artboard(&a, g, ng, &out, &e);
            sb64(&g_res, out.data, out.len);
            pc_buf_fini(&out);
            pc_artboard_fini(&a);
            check_line(l, st, &e);
        } else if (is_kind(l, "gin") && l->n == 5) {
            pc_guides g;
            pc_rect r;
            int32_t *v = NULL;
            size_t n = 0;
            JR(pc_guides, l->f[2], &g);
            if (sscanf(l->f[3], "%d,%d,%d,%d", &r.x0, &r.y0, &r.x1, &r.y1) != 4) {
                abort();
            }
            st = pc_io_guides_in(&g, r, &v, &n, &e);
            sb(&g_res, "[");
            for (size_t k = 0; st == PC_OK && k < n; k++) {
                sb(&g_res, "%s%d", k > 0 ? "," : "", v[k]);
            }
            sb(&g_res, "]");
            free(v);
            pc_guides_fini(&g);
            check_line(l, st, &e);
        } else if (is_kind(l, "cdec") && l->n == 5) {
            pc_buf res = dec(l->f[2]);
            pc_buf cmls = dec(l->f[3]);
            pc_layer_id lid;
            pc_document d = m_comp_doc(strcmp(l->f[3], "-") == 0 ? NULL : bdata(&cmls), cmls.len, &lid);
            pc_io_comps c;
            st =
                pc_io_decode_comps(strcmp(l->f[2], "-") == 0 ? NULL : bdata(&res), res.len, &d, &c, NULL, &e);
            if (st == PC_OK) {
                comps_s(&g_res, &d, c.comps, c.n, c.has_last_applied, c.last_applied,
                        c.has_last_state ? &c.last_state : NULL);
                pc_io_comps_fini(&c);
            }
            pc_document_fini(&d);
            pc_buf_fini(&res);
            pc_buf_fini(&cmls);
            check_line(l, st, &e);
        }
    }
    /* cw: writing comps for built documents (the sequence of rustref comps()). */
    {
        pc_layer_id lid;
        pc_layer_id lid2;
        pc_document d = m_comp_doc(NULL, 0, &lid);
        pc_document empty = m_comp_doc(NULL, 0, &lid2);
        pc_comp_layer_state s;
        pc_layer_comp last;
        pc_buf resb = PC_BUF_INIT;
        pc_buf c1 = PC_BUF_INIT;
        pc_buf c2 = PC_BUF_INIT;
        pc_buf out = PC_BUF_INIT;
        bool has = false;
        bool unchanged = false;
        const pc_layer *l;
        pc_document d2;
        pc_io_comps c;
        state_set(&s, lid, 0, true, 10, 1);
        push_comp(&d, 7, "One", "c", true, true, false, s);
        state_set(&s, lid, -1, true, INT32_MIN, 0);
        push_comp(&d, UINT32_MAX, "Two \xF0\x9F\x98\x80", "", false, true, true, s);
        d.has_last_applied_comp = true;
        d.last_applied_comp = 7;
        memset(&last, 0, sizeof last);
        must(pc_str_set_c(&last.name, "Last Document State", NULL));
        last.apply_visibility = true;
        last.apply_position = true;
        last.states = xmalloc(sizeof *last.states);
        state_set(&last.states[0], lid, 1, true, 4, 4);
        last.nstates = 1;
        must(pc_io_write_comps_resource(&d, &has, &resb, NULL));
        l = pc_document_layer(&d, lid);
        must(pc_io_write_cmls(d.layer_comps, d.nlayer_comps, NULL, l, 3, &has, &c1, NULL));
        must(pc_io_write_cmls(d.layer_comps, d.nlayer_comps, &last, l, UINT32_MAX, &has, &c2, NULL));
        (void)res_reset();
        sb64(&g_res, resb.data, resb.len);
        cw_check("res", pc_buf_cstr(&g_res));
        (void)res_reset();
        sb64(&g_res, c1.data, c1.len);
        cw_check("cmls", pc_buf_cstr(&g_res));
        (void)res_reset();
        sb64(&g_res, c2.data, c2.len);
        cw_check("cmls-last", pc_buf_cstr(&g_res));
        must(pc_io_write_comps_resource(&empty, &has, &out, NULL));
        cw_check("empty", has ? "some" : "none");
        must(pc_io_write_cmls(NULL, 0, NULL, l, 1, &has, &out, NULL));
        cw_check("empty-cmls", has ? "some" : "none");
        d2 = m_comp_doc(c1.data, c1.len, &lid2);
        for (size_t k = 0; k < d.nlayer_comps; k++) {
            d.layer_comps[k].states[0].layer = lid2;
        }
        d2.layer_comps = d.layer_comps;
        d2.nlayer_comps = d.nlayer_comps;
        d.layer_comps = NULL;
        d.nlayer_comps = 0;
        d2.has_last_applied_comp = true;
        d2.last_applied_comp = 7;
        m_push_resource(&d2, PC_IO_LAYER_COMPS, resb.data, resb.len);
        must(pc_io_decode_comps(resb.data, resb.len, &d2, &c, NULL, NULL));
        (void)res_reset();
        comps_s(&g_res, &d2, c.comps, c.n, c.has_last_applied, c.last_applied,
                c.has_last_state ? &c.last_state : NULL);
        cw_check("decode", pc_buf_cstr(&g_res));
        pc_io_comps_fini(&c);
        must(pc_io_comps_unchanged(&d2, &unchanged, NULL));
        cw_check("unchanged", unchanged ? "true" : "false");
        must(pc_str_set_c(&d2.layer_comps[0].name, "Renamed", NULL));
        must(pc_io_comps_unchanged(&d2, &unchanged, NULL));
        cw_check("unchanged-renamed", unchanged ? "true" : "false");
        pc_layer_comp_fini(&last);
        pc_buf_fini(&resb);
        pc_buf_fini(&c1);
        pc_buf_fini(&c2);
        pc_buf_fini(&out);
        pc_document_fini(&d);
        pc_document_fini(&d2);
        pc_document_fini(&empty);
    }
}

static void test_annotations(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_error e = {0};
        pc_status st = PC_OK;
        (void)res_reset();
        if (is_kind(l, "anno") && l->n == 4) {
            pc_buf d = b64d(l->f[2]);
            st = anno_res(&g_res, bdata(&d), d.len, &e);
            pc_buf_fini(&d);
            check_line(l, st, &e);
        } else if (is_kind(l, "annow") && l->n == 4) {
            pc_note *n;
            size_t nn;
            pc_buf out = PC_BUF_INIT;
            read_notes(l->f[2], &n, &nn);
            st = pc_io_write_anno(n, nn, NULL, 0, &out, &e);
            sb64(&g_res, out.data, out.len);
            pc_buf_fini(&out);
            for (size_t k = 0; k < nn; k++) {
                pc_note_fini(&n[k]);
            }
            free(n);
            check_line(l, st, &e);
        } else if (is_kind(l, "anx") && l->n == 6) {
            pc_document d = ndoc();
            pc_doc_psd_global_block *in;
            pc_doc_psd_global_block *out = NULL;
            size_t nout = 0;
            read_notes(l->f[2], &d.notes, &d.nnotes);
            parse_blocks(l->f[3], &d);
            in = xmalloc((d.metadata.npsd_global_blocks + 1) * sizeof *in);
            for (size_t k = 0; k < d.metadata.npsd_global_blocks; k++) {
                in[k] = d.metadata.psd_global_blocks[k];
                pc_blob_retain(in[k].data);
            }
            sb(&g_res, "%s", pc_io_anno_unreadable(&d) ? "true" : "false");
            CHECK_RESULT(l->f[1], l->f[4], pc_buf_cstr(&g_res));
            (void)res_reset();
            st = pc_io_annotation_export_blocks(&d, in, d.metadata.npsd_global_blocks, &out, &nout, &e);
            if (st == PC_OK) {
                blocks_s(&g_res, out, nout, d.metadata.psd_global_blocks, d.metadata.npsd_global_blocks);
                m_global_blocks_free(out, nout);
            }
            pc_document_fini(&d);
            check_line(l, st, &e);
        } else if (is_kind(l, "scale") && l->n == 4) {
            pc_buf d = b64d(l->f[2]);
            bool found = false;
            pc_measurement_scale s;
            st = pc_io_scale_from_resource(bdata(&d), d.len, &found, &s, &e);
            if (st == PC_OK && found) {
                sb(&g_res, "some ");
                JW(&g_res, pc_measurement_scale, &s);
                pc_measurement_scale_fini(&s);
            } else {
                sb(&g_res, "none");
            }
            pc_buf_fini(&d);
            check_line(l, st, &e);
        } else if (is_kind(l, "scalew") && l->n == 4) {
            pc_measurement_scale s;
            pc_buf out = PC_BUF_INIT;
            JR(pc_measurement_scale, l->f[2], &s);
            st = pc_io_write_scale_resource(&s, &out, &e);
            sb64(&g_res, out.data, out.len);
            pc_buf_fini(&out);
            pc_measurement_scale_fini(&s);
            check_line(l, st, &e);
        } else if (is_kind(l, "scx") && l->n == 6) {
            pc_document d = ndoc();
            pc_measurement_scale raw;
            bool has = false;
            pc_buf out = PC_BUF_INIT;
            if (strcmp(l->f[2], "-") != 0) {
                pc_buf r = b64d(l->f[2]);
                m_push_resource(&d, PC_IO_MEASUREMENT_SCALE, bdata(&r), r.len);
                pc_buf_fini(&r);
            }
            pc_measurement_scale_fini(&d.measurement.scale);
            JR(pc_measurement_scale, l->f[3], &d.measurement.scale);
            if (strcmp(l->f[4], "true") == 0) {
                d.measurement.count_groups = xmalloc(sizeof *d.measurement.count_groups);
                must(pc_count_group_default(&d.measurement.count_groups[0], NULL));
                d.measurement.count_groups[0].points = xmalloc(sizeof *d.measurement.count_groups[0].points);
                d.measurement.count_groups[0].points[0][0] = 1.0;
                d.measurement.count_groups[0].points[0][1] = 1.0;
                d.measurement.count_groups[0].npoints = 1;
                d.measurement.ncount_groups = 1;
            }
            if (pc_io_raw_scale(&d, &raw)) {
                JW(&g_res, pc_measurement_scale, &raw);
                pc_measurement_scale_fini(&raw);
            } else {
                sb(&g_res, "none");
            }
            sb(&g_res, " %s %s ", pc_io_keep_resource(&d, PC_IO_MEASUREMENT_SCALE) ? "true" : "false",
               pc_io_keep_resource(&d, 1080) ? "true" : "false");
            st = pc_io_fresh_scale_resource(&d, &has, &out, &e);
            if (has) {
                sb(&g_res, "some ");
                sb64(&g_res, out.data, out.len);
            } else {
                sb(&g_res, "none");
            }
            pc_buf_fini(&out);
            pc_document_fini(&d);
            check_line(l, st, &e);
        }
    }
}

static void test_slices(void)
{
    pc_document base = slice_doc();
    pc_layer_id lid0 = base.layers[0].id;
    uint32_t psd5 = 5;
    pc_io_layer_id_map ids;
    ids.ids = &lid0;
    ids.psd = &psd5;
    ids.n = 1;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_error e = {0};
        pc_status st = PC_OK;
        (void)res_reset();
        if (is_kind(l, "slc") && l->n == 4) {
            pc_buf d = b64d(l->f[2]);
            pc_slices s;
            st = pc_io_slices_from_psd(bdata(&d), d.len, &base, &s, &e);
            if (st == PC_OK) {
                slices_s(&g_res, &base, &s);
                pc_slices_fini(&s);
            }
            pc_buf_fini(&d);
            check_line(l, st, &e);
        } else if (is_kind(l, "slx") && l->n == 4 && strstr(l->f[1], "/back") == NULL) {
            pc_document d;
            pc_buf written = PC_BUF_INIT;
            char back_name[64];
            const gline *bl;
            must(pc_document_clone(&base, &d, NULL));
            slices_into(&d, l->f[2]);
            slices_s(&g_res, &d, &d.slices);
            CHECK_RESULT(l->f[1], l->f[2], pc_buf_cstr(&g_res));
            (void)res_reset();
            st = slx_res(&g_res, &d, &ids, &written, &e);
            check_line(l, st, &e);
            (void)snprintf(back_name, sizeof back_name, "%s/back", l->f[1]);
            bl = find_line("slx", back_name);
            if (st == PC_OK && bl != NULL && bl->n == 4) {
                pc_document back;
                bool same = false;
                pc_io_slices_action act;
                pc_buf out = PC_BUF_INIT;
                must(pc_document_clone(&d, &back, NULL));
                pc_slices_fini(&back.slices);
                memset(&back.slices, 0, sizeof back.slices);
                m_push_resource(&back, PC_IO_SLICES, bdata(&written), written.len);
                must(pc_io_slices_import(&back, NULL));
                (void)res_reset();
                slices_s(&g_res, &back, &back.slices);
                CHECK_RESULT(back_name, bl->f[2], pc_buf_cstr(&g_res));
                must(pc_io_slices_unchanged(&back, &same, NULL));
                must(pc_io_slices_export_resource(&back, &ids, &act, &out, NULL, NULL));
                (void)res_reset();
                sb(&g_res, "%s %s", same ? "true" : "false", act == PC_IO_SLICES_KEEP ? "keep" : "rewrite");
                CHECK_RESULT(back_name, bl->f[3], pc_buf_cstr(&g_res));
                pc_buf_fini(&out);
                pc_document_fini(&back);
            }
            pc_buf_fini(&written);
            pc_document_fini(&d);
        }
    }
    pc_document_fini(&base);
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
    RUN(test_vector);
    RUN(test_comps);
    RUN(test_annotations);
    RUN(test_slices);
    golden_summary("io maps parity (1/2)");
    pc_buf_fini(&g_res);
    return pc_t_finish();
}
