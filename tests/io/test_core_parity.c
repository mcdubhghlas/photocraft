/* Parity with the Rust reference: tests/golden/io-core-vectors.txt was written by
 * `rustref io-core-vectors` (tools/rustref/src/io_core_vectors.rs). This executable checks the
 * pixel shuffles, DisplayInfo, linked files, the PSD size estimate, Multichannel export and
 * import, flat imports (files made by the reference's encoders, embedded in the golden), TIFF
 * pages, camera raws, layered-TIFF fallbacks, merged composites, the corpus imports and the
 * detection helpers; test_export_parity.c checks the exports. Every result must match; results
 * of paths a later io chunk still stubs are counted as pending; corpus lines are skipped when
 * corpus/ is absent, EXR / HEIF lines when the build lacks them. */
#include "harness.h"

#include "docs.h"
#include "golden.h"
#include "psd/tiff.h"

static void res_imp(pc_buf *b, pc_status st, const pc_io_import_result *r, const pc_error *e, bool lossy)
{
    pc_buf_truncate(b, 0);
    s_imp(b, st, r, e, lossy);
}

/* ---- pixels ---- */

static void test_pixels(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if ((is_kind(l, "px-il") || is_kind(l, "px-dl")) && l->n == 4) {
            size_t n = (size_t)strtoul(l->f[1], NULL, 10);
            pc_sample_type s;
            size_t bps;
            uint8_t *planes[4];
            pc_io_plane refs[4];
            uint8_t fill[4][4];
            static const bool invert[4] = {false, true, false, true};
            uint8_t *il = NULL;
            size_t il_len = 0;
            char h[17];
            must(pc_sample_type_from_name(l->f[2], strlen(l->f[2]), &s) ? PC_OK : PC_ERR_INVALID);
            bps = pc_sample_type_bytes(s);
            for (size_t c = 0; c < 4; c++) {
                planes[c] = xmalloc(n * bps);
                for (size_t k = 0; k < n * bps; k++) {
                    planes[c][k] = (uint8_t)(k * 13 + c * 5);
                }
                refs[c] = (pc_io_plane){true, planes[c], n * bps};
                memset(fill[c], (int)(c * 40 + 1), 4);
            }
            refs[2].present = false;
            refs[3].len = (n * bps) / 2;
            must(pc_io_interleave(refs, 4, (const uint8_t (*)[4])fill, n, s, invert, &il, &il_len, NULL));
            if (is_kind(l, "px-il")) {
                h16(il, il_len, h);
            } else {
                uint8_t *back = NULL;
                size_t plane = 0;
                must(pc_io_deinterleave(il, il_len, 4, s, invert, &back, &plane, NULL));
                h16(back, plane * 4, h);
                free(back);
            }
            CHECK_RESULT(l->f[0], l->f[3], h);
            free(il);
            for (size_t c = 0; c < 4; c++) {
                free(planes[c]);
            }
        } else if (is_kind(l, "px-lab") && l->n == 3) {
            size_t len = (size_t)65536 * 3 * 2;
            uint8_t *all = xmalloc(len);
            char h1[17], h2[17];
            for (uint32_t v = 0; v <= 65535; v++) {
                uint16_t u = (uint16_t)v;
                for (int c = 0; c < 3; c++) {
                    memcpy(all + ((size_t)v * 3 + (size_t)c) * 2, &u, 2);
                }
            }
            pc_io_lab16_chroma(all, len, 3, true);
            h16(all, len, h1);
            pc_io_lab16_chroma(all, len, 3, false);
            h16(all, len, h2);
            CHECK_RESULT("px-lab to doc", l->f[1], h1);
            CHECK_RESULT("px-lab back", l->f[2], h2);
            free(all);
        } else if (is_kind(l, "px-be") && l->n == 4) {
            pc_sample_type s;
            uint32_t bits = (uint32_t)strtoul(l->f[2], NULL, 16);
            float v;
            uint8_t o[4];
            char hex[9] = {0};
            size_t k;
            must(pc_sample_type_from_name(l->f[1], strlen(l->f[1]), &s) ? PC_OK : PC_ERR_INVALID);
            memcpy(&v, &bits, 4);
            k = pc_io_encode_be(v, s, o);
            for (size_t j = 0; j < k; j++) {
                (void)snprintf(hex + 2 * j, 3, "%02x", o[j]);
            }
            CHECK_RESULT("px-be", l->f[3], hex);
        } else if (is_kind(l, "px-matte") && l->n == 2) {
            pc_buf b = PC_BUF_INIT;
            char h[17];
            for (int ci = 0; ci <= 20; ci++) {
                for (int ai = 0; ai <= 20; ai++) {
                    float c = (float)ci / 16.0f - 0.1f;
                    float a = (float)ai / 20.0f;
                    for (int w = 0; w < 2; w++) {
                        uint32_t m = fb(pc_io_matte(c, a, (float)w));
                        uint32_t u = fb(pc_io_unmatte(c, a, (float)w));
                        uint8_t le[8] = {(uint8_t)m,         (uint8_t)(m >> 8), (uint8_t)(m >> 16),
                                         (uint8_t)(m >> 24), (uint8_t)u,        (uint8_t)(u >> 8),
                                         (uint8_t)(u >> 16), (uint8_t)(u >> 24)};
                        must(pc_buf_append(&b, le, 8, NULL));
                    }
                }
            }
            h16(b.data, b.len, h);
            CHECK_RESULT("px-matte", l->f[1], h);
            pc_buf_fini(&b);
        }
    }
}

/* ---- channel_map ---- */

static void cm_channels(pc_alpha_channel ch[5])
{
    static const char *names[5] = {"A", "S", "C", "L", "N"};
    static const int widths[5] = {1, 2, 3, 1, 1};
    for (int i = 0; i < 5; i++) {
        pc_surface s;
        float one = 1.0f;
        pc_surface_init(&s, PC_PIXEL_FORMAT_GRAY8);
        must(pc_surface_fill_rect(&s, pc_rect_make(0, 0, widths[i], 2), &one, 1, NULL));
        ch[i] = t_channel(names[i], s);
    }
    ch[0].color = pc_color_rgb(0, 1, 0);
    ch[0].opacity = 0.3f;
    ch[0].indicates = PC_INDICATES_SELECTED_AREAS;
    ch[1].has_spot = true;
    ch[1].spot_color = pc_color_rgb(1, 0, 1);
    ch[1].spot_solidity = 0.8f;
    ch[2].color = (pc_color){PC_COLOR_MODE_CMYK, {0.2f, 0.4f, 0.6f, 0.1f}, 1.0f};
    ch[2].opacity = 1.7f;
    ch[3].has_spot = true;
    ch[3].spot_color = (pc_color){PC_COLOR_MODE_LAB, {0.5f, 0.7f, 0.2f, 0.0f}, 1.0f};
    ch[3].spot_solidity = -0.5f;
    ch[4].color = pc_color_rgb(NAN, 2.0f, -1.0f);
    ch[4].opacity = NAN;
}

static void hex_of(const uint8_t *b, size_t n, pc_buf *out)
{
    pc_buf_truncate(out, 0);
    for (size_t i = 0; i < n; i++) {
        sb(out, "%02x", b[i]);
    }
}

static void test_channel_map(void)
{
    pc_alpha_channel ch[5];
    const pc_alpha_channel *refs[5];
    pc_buf di = PC_BUF_INIT;
    pc_buf s = PC_BUF_INIT;
    pc_buf hx = PC_BUF_INIT;
    cm_channels(ch);
    for (int i = 0; i < 5; i++) {
        refs[i] = &ch[i];
    }
    must(pc_io_display_info(refs, 5, &di, NULL));
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if (is_kind(l, "cm-di") && l->n == 3) {
            pc_buf none = PC_BUF_INIT;
            bool all = strcmp(l->f[1], "all") == 0;
            if (!all) {
                must(pc_io_display_info(NULL, 0, &none, NULL));
            }
            hex_of(all ? di.data : none.data, all ? di.len : none.len, &hx);
            CHECK_RESULT("cm-di", l->f[2], pc_buf_cstr(&hx));
            pc_buf_fini(&none);
        } else if (is_kind(l, "cm-apply") && l->n == 3) {
            pc_alpha_channel f[5];
            uint8_t data[128];
            size_t len = 0;
            bool versioned = true;
            const char *c = l->f[1];
            if (strcmp(c, "v") == 0) {
                memcpy(data, di.data, len = di.len);
            } else if (strcmp(c, "old") == 0) {
                memcpy(data, di.data + 4, len = di.len - 4);
                data[12 + 14] = 1;
                versioned = false;
            } else if (strcmp(c, "short") == 0) {
                memcpy(data, di.data, len = 10);
            } else if (strcmp(c, "partial") == 0) {
                memcpy(data, di.data, len = 4 + 14 * 2 + 5);
            } else if (strcmp(c, "tiny") == 0) {
                memset(data, 0, len = 2);
            } else {
                static const uint8_t kinds[32] = {0,    0, 0,    1,    0,   8, 0x13, 0x88, 0, 0,    0,
                                                  0,    0, 0,    0,    250, 7, 0,    0,    7, 0x27, 0x10,
                                                  0x80, 0, 0x7f, 0xff, 0,   0, 0,    50,   2, 0};
                memcpy(data, kinds, len = sizeof kinds);
            }
            for (int k = 0; k < 5; k++) {
                pc_surface e;
                char nm[16];
                pc_surface_init(&e, PC_PIXEL_FORMAT_GRAY8);
                (void)snprintf(nm, sizeof nm, "x%d", k);
                f[k] = t_channel(nm, e);
            }
            pc_io_apply_display_info(data, len, versioned, f, 5);
            pc_buf_truncate(&s, 0);
            for (int k = 0; k < 5; k++) {
                if (k > 0) {
                    sb(&s, ";");
                }
                if (f[k].has_spot) {
                    s_color(&s, &f[k].spot_color);
                    sb(&s, "@%08x", fb(f[k].spot_solidity));
                } else {
                    sb(&s, "-");
                }
                sb(&s, "|");
                s_color(&s, &f[k].color);
                sb(&s, "|%08x|%d", fb(f[k].opacity), f[k].indicates == PC_INDICATES_SELECTED_AREAS);
                pc_alpha_channel_fini(&f[k]);
            }
            CHECK_RESULT(c, l->f[2], pc_buf_cstr(&s));
        } else if (is_kind(l, "cm-color") && l->n == 7) {
            uint16_t comps[4];
            pc_color col;
            for (int k = 0; k < 4; k++) {
                comps[k] = (uint16_t)strtoul(l->f[2 + k], NULL, 10);
            }
            col = pc_io_decode_color((uint16_t)strtoul(l->f[1], NULL, 10), comps);
            pc_buf_truncate(&s, 0);
            s_color(&s, &col);
            CHECK_RESULT("cm-color", l->f[6], pc_buf_cstr(&s));
        }
    }
    for (int i = 0; i < 5; i++) {
        pc_alpha_channel_fini(&ch[i]);
    }
    pc_buf_fini(&di);
    pc_buf_fini(&s);
    pc_buf_fini(&hx);
}

/* ---- linked ---- */

static void lf_s(pc_buf *b, const pc_io_linked_file *f)
{
    sq(b, pc_str_c(&f->uuid), f->uuid.len);
    sb(b, "|");
    sq(b, pc_str_c(&f->file_name), f->file_name.len);
    sb(b, "|");
    sopt(b, f->bytes, f->len, true);
}

static void lfs_s(pc_buf *b, const pc_io_linked_files *l)
{
    for (size_t i = 0; i < l->n; i++) {
        if (i > 0) {
            sb(b, ";");
        }
        lf_s(b, &l->v[i]);
    }
}

static void uuids_s(pc_buf *b, const uint8_t *d, size_t n)
{
    pc_io_strs u;
    must(pc_io_linked_block_uuids(d, n, &u, NULL));
    sb(b, "[");
    for (size_t i = 0; i < u.n; i++) {
        if (i > 0) {
            sb(b, ", ");
        }
        sq(b, pc_str_c(&u.v[i]), u.v[i].len);
    }
    sb(b, "]");
    pc_io_strs_fini(&u);
}

static pc_io_linked_file mk_lf(const char *uuid, size_t ulen, const char *name, size_t nlen,
                               const void *bytes, size_t n)
{
    pc_io_linked_file f;
    memset(&f, 0, sizeof f);
    must(pc_str_set(&f.uuid, uuid, ulen, NULL));
    must(pc_str_set(&f.file_name, name, nlen, NULL));
    f.bytes = xmalloc(n);
    memcpy(f.bytes, bytes, n);
    f.len = n;
    return f;
}

static bool keep_all(void *ctx, const char *u, size_t n)
{
    (void)ctx;
    (void)u;
    (void)n;
    return true;
}

static bool drop_def(void *ctx, const char *u, size_t n)
{
    (void)ctx;
    return !(n == 5 && memcmp(u, "def-2", 5) == 0);
}

static void test_linked(void)
{
    char x300[301];
    pc_io_linked_file files[8];
    pc_buf block = PC_BUF_INIT;
    pc_buf s = PC_BUF_INIT;
    pc_buf one = PC_BUF_INIT;
    pc_io_linked_files parsed;
    memset(x300, 'x', 300);
    files[0] = mk_lf("abc-1", 5, "a.png", 5, "\x89PNG\x01\x02", 6);
    files[1] = mk_lf("def-2", 5, "b \xF0\x9F\x98\x80.psb", 10, "8BPS\0\x02rest", 10);
    files[2] = mk_lf(x300, 300, "", 0, "", 0);
    files[3] = mk_lf("j", 1, "c\0", 2, "\xFF\xD8\x09", 3);
    files[4] = mk_lf("t\xC3\xA9", 3, "t.tif", 5, "MM\0\x2a....", 8);
    files[5] = mk_lf("g", 1, "g.gif", 5, "GIF89a", 6);
    files[6] = mk_lf("i", 1, "i", 1, "II\x2a\0", 4);
    files[7] = mk_lf("p", 1, "p", 1, "8BPS\0\x01", 6);
    for (int i = 0; i < 8; i++) {
        must(pc_io_encode_linked_file(&files[i], &block, NULL));
    }
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if (is_kind(l, "lk-enc") && l->n == 3) {
            size_t k = (size_t)strtoul(l->f[1], NULL, 10);
            pc_buf_truncate(&one, 0);
            must(pc_io_encode_linked_file(&files[k], &one, NULL));
            hex_of(one.data, one.len, &s);
            CHECK_RESULT("lk-enc", l->f[2], pc_buf_cstr(&s));
        } else if (is_kind(l, "lk-type") && l->n == 3) {
            size_t k = (size_t)strtoul(l->f[1], NULL, 10);
            uint8_t t[4];
            pc_io_linked_file_type(files[k].bytes, files[k].len, t);
            hex_of(t, 4, &s);
            CHECK_RESULT("lk-type", l->f[2], pc_buf_cstr(&s));
        } else if (is_kind(l, "lk-parse") && l->n == 3) {
            must(pc_io_parse_linked_files(block.data, block.len, &parsed, NULL));
            pc_buf_truncate(&s, 0);
            lfs_s(&s, &parsed);
            pc_io_linked_files_fini(&parsed);
            CHECK_RESULT("lk-parse", l->f[2], pc_buf_cstr(&s));
        } else if (is_kind(l, "lk-uuids") && l->n == 3 && strcmp(l->f[1], "all") == 0) {
            pc_buf_truncate(&s, 0);
            uuids_s(&s, block.data, block.len);
            CHECK_RESULT("lk-uuids", l->f[2], pc_buf_cstr(&s));
        } else if ((is_kind(l, "lk-trunc") || is_kind(l, "lk-flip")) && l->n == 2) {
            bool trunc = is_kind(l, "lk-trunc");
            pc_buf h = PC_BUF_INIT;
            char hh[17];
            size_t count = trunc ? block.len + 1 : block.len;
            for (size_t n = 0; n < count; n++) {
                uint8_t *b = xmalloc(block.len);
                size_t len = trunc ? n : block.len;
                memcpy(b, block.data, block.len);
                if (!trunc) {
                    b[n] ^= 0x5a;
                }
                must(pc_io_parse_linked_files(b, len, &parsed, NULL));
                sb(&h, "%zu:", n);
                lfs_s(&h, &parsed);
                sb(&h, "|");
                uuids_s(&h, b, len);
                sb(&h, "|");
                pc_io_linked_files_fini(&parsed);
                free(b);
            }
            h16(h.data, h.len, hh);
            CHECK_RESULT(l->f[0], l->f[1], hh);
            pc_buf_fini(&h);
        } else if (is_kind(l, "lk-desc") && l->n == 4) {
            pc_buf d = b64d(l->f[1]);
            must(pc_io_parse_linked_files(d.data, d.len, &parsed, NULL));
            pc_buf_truncate(&s, 0);
            lfs_s(&s, &parsed);
            pc_io_linked_files_fini(&parsed);
            CHECK_RESULT("lk-desc", l->f[2], pc_buf_cstr(&s));
            pc_buf_truncate(&s, 0);
            uuids_s(&s, d.data, d.len);
            CHECK_RESULT("lk-desc uuids", l->f[3], pc_buf_cstr(&s));
            pc_buf_fini(&d);
        } else if (is_kind(l, "lk-find") && l->n == 3) {
            const gline *dl = NULL;
            pc_metadata meta;
            pc_doc_psd_global_block gb[3];
            pc_io_linked_file f;
            bool found = false;
            pc_buf d;
            for (size_t k = 0; k < g_nlines; k++) {
                if (is_kind(&g_lines[k], "lk-desc")) {
                    dl = &g_lines[k];
                }
            }
            d = b64d(dl->f[1]);
            memset(&meta, 0, sizeof meta);
            memcpy(gb[0].signature, "8BIM", 4);
            memcpy(gb[0].key, "Patt", 4);
            memcpy(gb[1].signature, "8BIM", 4);
            memcpy(gb[1].key, "lnk3", 4);
            memcpy(gb[2].signature, "8BIM", 4);
            memcpy(gb[2].key, "lnk2", 4);
            must(pc_blob_new(block.data, block.len, &gb[0].data, NULL));
            must(pc_blob_new(d.data, d.len, &gb[1].data, NULL));
            must(pc_blob_new(block.data, block.len, &gb[2].data, NULL));
            meta.psd_global_blocks = gb;
            meta.npsd_global_blocks = 3;
            must(pc_io_find_linked_file(&meta, l->f[1], strlen(l->f[1]), &found, &f, NULL));
            pc_buf_truncate(&s, 0);
            if (found) {
                lf_s(&s, &f);
                pc_io_linked_file_fini(&f);
            } else {
                sb(&s, "-");
            }
            CHECK_RESULT("lk-find", l->f[2], pc_buf_cstr(&s));
            for (int k = 0; k < 3; k++) {
                pc_blob_release(gb[k].data);
            }
            pc_buf_fini(&d);
        } else if (is_kind(l, "lk-rebuild") && l->n == 3) {
            pc_io_linked_file add = mk_lf("new", 3, "n.png", 5, "\x89PNG", 4);
            pc_buf src = PC_BUF_INIT;
            pc_buf out = PC_BUF_INIT;
            bool changed = false;
            const char *c = l->f[1];
            must(pc_buf_append(&src, block.data, block.len, NULL));
            if (strcmp(c, "tail") == 0) {
                must(pc_buf_append(&src, "\0\0\0\0\0\0\x01\0\x09", 9, NULL));
            }
            must(pc_io_rebuild_linked_block(
                src.data, src.len, strcmp(c, "drop") == 0 || strcmp(c, "tail") == 0 ? drop_def : keep_all,
                NULL, &add, strcmp(c, "add") == 0 || strcmp(c, "tail") == 0 ? 1 : 0, &changed, &out, NULL));
            if (changed) {
                hex_of(out.data, out.len, &s);
            } else {
                pc_buf_truncate(&s, 0);
                sb(&s, "-");
            }
            CHECK_RESULT(c, l->f[2], pc_buf_cstr(&s));
            if (strcmp(c, "tail") == 0) {
                const gline *u = NULL;
                for (size_t k = 0; k < g_nlines; k++) {
                    if (is_kind(&g_lines[k], "lk-uuids") && strcmp(g_lines[k].f[1], "tail") == 0) {
                        u = &g_lines[k];
                    }
                }
                pc_buf_truncate(&s, 0);
                uuids_s(&s, src.data, src.len);
                CHECK(u != NULL);
                if (u != NULL) {
                    CHECK_RESULT("lk-uuids tail", u->f[2], pc_buf_cstr(&s));
                }
            }
            pc_io_linked_file_fini(&add);
            pc_buf_fini(&src);
            pc_buf_fini(&out);
        }
    }
    for (int i = 0; i < 8; i++) {
        pc_io_linked_file_fini(&files[i]);
    }
    pc_buf_fini(&block);
    pc_buf_fini(&s);
    pc_buf_fini(&one);
}

/* ---- estimate, Multichannel ---- */

static void test_estimate(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_document d;
        bool ok = false;
        uint64_t size = 0;
        char got[32];
        if (!is_kind(l, "est") || l->n != 3) {
            continue;
        }
        if (!t_export_doc(l->f[1], &d)) {
            PC_T_FAIL("unknown document %s", l->f[1]);
            continue;
        }
        must(pc_io_estimate_psd_size(&d, &ok, &size, NULL));
        if (ok) {
            (void)snprintf(got, sizeof got, "%llu", (unsigned long long)size);
        } else {
            (void)snprintf(got, sizeof got, "-");
        }
        CHECK_RESULT(l->f[1], l->f[2], got);
        pc_document_fini(&d);
    }
}

/* psd_import's unicode_names (resource 1045). */
static size_t unicode_names(const uint8_t *data, size_t len, pc_str *out, size_t cap)
{
    size_t at = 0;
    size_t n = 0;
    while (at + 4 <= len && n < cap) {
        size_t units =
            (size_t)data[at] << 24 | (size_t)data[at + 1] << 16 | (size_t)data[at + 2] << 8 | data[at + 3];
        pc_buf b = PC_BUF_INIT;
        size_t bl;
        at += 4;
        if (units > (len - at) / 2) {
            break;
        }
        must(pc_utf16be_lossy(data + at, units, &b, NULL));
        bl = b.len;
        while (bl > 0 && b.data[bl - 1] == 0) {
            bl--;
        }
        must(pc_str_set(&out[n], bl > 0 ? (const char *)b.data : "", bl, NULL));
        pc_buf_fini(&b);
        n++;
        at += units * 2;
    }
    return n;
}

static void test_multichannel(void)
{
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_document d;
        if (is_kind(l, "mc-exp") && l->n == 7) {
            pc_psd_file f;
            pc_io_warnings w = PC_IO_WARNINGS_INIT;
            pc_psd_bytes bytes = {NULL, 0};
            char h[17];
            char len[32];
            if (!t_export_doc(l->f[1], &d)) {
                PC_T_FAIL("unknown document %s", l->f[1]);
                continue;
            }
            must(pc_io_multichannel_to_psd(&d, strcmp(l->f[2], "1") == 0, &f, &w, NULL));
            must(pc_psd_file_to_bytes(&f, &bytes, NULL));
            h16(bytes.data, bytes.len, h);
            (void)snprintf(len, sizeof len, "%zu", bytes.len);
            CHECK_RESULT("mc-exp hash", l->f[3], h);
            CHECK_RESULT("mc-exp len", l->f[4], len);
            pc_buf_truncate(&s, 0);
            s_warn(&s, w.v, w.n);
            CHECK_RESULT("mc-exp warnings", l->f[5], pc_buf_cstr(&s));
            pc_psd_bytes_fini(&bytes);
            pc_psd_file_fini(&f);
            pc_io_warnings_fini(&w);
            pc_document_fini(&d);
        } else if (is_kind(l, "mc-imp") && l->n == 4) {
            /* The reference's file (mc-exp, force 0) through the Multichannel part of PSD import. */
            const gline *e = NULL;
            pc_buf bytes;
            pc_psd_file f;
            pc_psd_bytes all = {NULL, 0};
            pc_io_warnings w = PC_IO_WARNINGS_INIT;
            pc_str names[64];
            size_t nn = 0;
            const pc_psd_resource *r;
            for (size_t k = 0; k < g_nlines; k++) {
                if (is_kind(&g_lines[k], "mc-exp") && strcmp(g_lines[k].f[1], l->f[1]) == 0 &&
                    strcmp(g_lines[k].f[2], "0") == 0) {
                    e = &g_lines[k];
                }
            }
            if (e == NULL) {
                PC_T_FAIL("no mc-exp for %s", l->f[1]);
                continue;
            }
            bytes = b64d(e->f[6]);
            must(pc_psd_file_parse(bytes.data, bytes.len, &f, NULL));
            must(pc_psd_file_decode_merged(&f, &all, NULL));
            d = t_doc("x", f.header.width, f.header.height, PC_COLOR_MODE_MULTICHANNEL,
                      pc_io_sample_for_depth(f.header.depth));
            memset(names, 0, sizeof names);
            r = pc_psd_file_resource(&f, 1045);
            if (r != NULL) {
                nn = unicode_names(r->data.data, r->data.len, names, 64);
            }
            must(pc_io_multichannel_import(&f, all.data, all.len, &d, names, nn, &w, NULL));
            pc_buf_truncate(&s, 0);
            for (size_t k = 0; k < d.nchannels; k++) {
                if (k > 0) {
                    sb(&s, ";");
                }
                s_chan(&s, &d.channels[k]);
            }
            CHECK_RESULT("mc-imp", l->f[2], pc_buf_cstr(&s));
            for (size_t k = 0; k < nn; k++) {
                pc_str_fini(&names[k]);
            }
            pc_io_warnings_fini(&w);
            pc_psd_bytes_fini(&all);
            pc_psd_file_fini(&f);
            pc_buf_fini(&bytes);
            pc_document_fini(&d);
        }
    }
    pc_buf_fini(&s);
}

/* ---- flat import ---- */

static bool c_can_read(const char *name, const uint8_t *b, size_t n)
{
    pc_cformat f;
    const char *dot = strrchr(name, '.');
    if (pc_cformat_detect(b, n, &f) && !pc_cformat_caps(f).read) {
        return false;
    }
    if (dot != NULL && pc_cformat_from_extension(dot + 1, &f) && !pc_cformat_caps(f).read &&
        (f == PC_CF_EXR || f == PC_CF_HEIF)) {
        return false;
    }
    return true;
}

static void test_imports(void)
{
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf bytes;
        pc_io_import_result r;
        pc_error e;
        pc_status st;
        if (!is_kind(l, "imp") || l->n != 5) {
            continue;
        }
        bytes = b64d(l->f[3]);
        if (!c_can_read(l->f[1], bytes.data, bytes.len)) {
            g_skipped++;
            pc_buf_fini(&bytes);
            continue;
        }
        st = pc_io_import(l->f[1], bytes.data, bytes.len, &r, &e);
        if (st != PC_OK && is_pending(st, &e)) {
            g_pending++;
        } else {
            res_imp(&s, st, &r, &e, strcmp(l->f[2], "1") == 0);
            CHECK_RESULT(l->f[1], l->f[4], pc_buf_cstr(&s));
        }
        if (st == PC_OK) {
            pc_io_import_result_fini(&r);
        }
        pc_buf_fini(&bytes);
    }
    pc_buf_fini(&s);
}

/* tests/tiff_pages.rs's generator. */
static void tiff_pages(bool big, const uint32_t (*pages)[5], size_t n, pc_buf *f)
{
    size_t next_field = big ? 8 : 4;
    size_t width = big ? 8 : 4;
    pc_buf_truncate(f, 0);
    must(pc_buf_append(f, big ? "II\x2b\0\x08\0\0\0\0\0\0\0\0\0\0\0" : "II\x2a\0\0\0\0\0", big ? 16 : 8,
                       NULL));
    for (size_t p = 0; p < n; p++) {
        uint32_t w = pages[p][0], h = pages[p][1], level = pages[p][2], kind = pages[p][3], o = pages[p][4];
        uint64_t pixels_at = f->len;
        uint64_t ifd_at;
        uint64_t entries[11][3] = {{254, 4, kind},
                                   {256, 4, w},
                                   {257, 4, h},
                                   {258, 3, 8},
                                   {259, 3, 1},
                                   {262, 3, 1},
                                   {273, 4, 0},
                                   {274, 3, o},
                                   {277, 3, 1},
                                   {278, 4, h},
                                   {279, 4, (uint64_t)w * h}};
        for (uint32_t k = 0; k < w * h; k++) {
            must(pc_buf_append_byte(f, (uint8_t)level, NULL));
        }
        if (f->len % 2 == 1) {
            must(pc_buf_append_byte(f, 0, NULL));
        }
        ifd_at = f->len;
        entries[6][2] = pixels_at;
        for (size_t k = 0; k < width; k++) {
            f->data[next_field + k] = (uint8_t)(ifd_at >> (8 * k));
        }
        for (size_t k = 0; k < (big ? 8u : 2u); k++) {
            must(pc_buf_append_byte(f, k == 0 ? 11 : 0, NULL));
        }
        for (size_t e = 0; e < 11; e++) {
            uint8_t t[20];
            size_t tl = 0;
            t[tl++] = (uint8_t)entries[e][0];
            t[tl++] = (uint8_t)(entries[e][0] >> 8);
            t[tl++] = (uint8_t)entries[e][1];
            t[tl++] = 0;
            for (size_t k = 0; k < width; k++) {
                t[tl++] = k == 0 ? 1 : 0;
            }
            for (size_t k = 0; k < width; k++) {
                t[tl++] = (uint8_t)(entries[e][2] >> (8 * k));
            }
            must(pc_buf_append(f, t, tl, NULL));
        }
        next_field = f->len;
        for (size_t k = 0; k < width; k++) {
            must(pc_buf_append_byte(f, 0, NULL));
        }
    }
}

static void test_tiff_pages(void)
{
    static const uint32_t pages[3][5] = {{2, 2, 9, 1, 1}, {6, 4, 50, 0, 1}, {3, 5, 90, 2, 1}};
    static const uint32_t turned[1][5] = {{6, 4, 50, 0, 6}};
    pc_buf f = PC_BUF_INIT;
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        bool big;
        pc_io_import_result r;
        pc_error e;
        pc_status st;
        if (!is_kind(l, "tpage") || l->n != 4) {
            continue;
        }
        big = strcmp(l->f[1], "1") == 0;
        if (strcmp(l->f[2], "turned") == 0) {
            tiff_pages(big, turned, 1, &f);
            st = pc_io_import_tiff_page("pages.tif", f.data, f.len, false, 0, &r, &e);
        } else {
            tiff_pages(big, pages, 3, &f);
            st = pc_io_import_tiff_page("pages.tif", f.data, f.len, true, (size_t)strtoul(l->f[2], NULL, 10),
                                        &r, &e);
        }
        res_imp(&s, st, &r, &e, false);
        CHECK_RESULT("tpage", l->f[3], pc_buf_cstr(&s));
        if (st == PC_OK) {
            pc_io_import_result_fini(&r);
        }
    }
    pc_buf_fini(&f);
    pc_buf_fini(&s);
}

/* ---- raw ---- */

static void test_raws(void)
{
    pc_buf s = PC_BUF_INIT;
    size_t cam_i = 0;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        const gline *src = NULL;
        pc_buf bytes;
        pc_io_import_result r;
        pc_error e;
        pc_status st;
        bool lossy;
        if (is_kind(l, "cam") && l->n == 4) {
            /* The reference's list, in order (its names are printed with Rust escapes). */
            static const char *const cams[11][2] = {{"Canon", "Canon EOS 80D"},
                                                    {"NIKON CORPORATION", "NIKON D3200"},
                                                    {"SONY", "ILCE-7M3"},
                                                    {NULL, "X"},
                                                    {"Panasonic", "DC-G9"},
                                                    {"Y", NULL},
                                                    {NULL, NULL},
                                                    {"", "Z"},
                                                    {"  \xE2\x80\x83leica camera", "LEICA M10"},
                                                    {"canon", "Can"},
                                                    {"\xC3\xA9t\xC3\xA9", "\xC3\x89T\xC3\x89 1"}};
            char out[256];
            if (cam_i < 11) {
                (void)pc_io_camera_name(cams[cam_i][0], cams[cam_i][1], out, sizeof out);
                pc_buf_truncate(&s, 0);
                sq(&s, out, strlen(out));
                CHECK_RESULT("cam", l->f[3], pc_buf_cstr(&s));
            }
            cam_i++;
            continue;
        }
        if (!(is_kind(l, "raw") || is_kind(l, "rawi") || is_kind(l, "rawt") || is_kind(l, "rawd"))) {
            continue;
        }
        src = is_kind(l, "raw") ? l : find_line("raw", l->f[1]);
        if (src == NULL) {
            PC_T_FAIL("no raw line for %s", l->f[1]);
            continue;
        }
        bytes = b64d(src->f[2]);
        lossy = strstr(l->f[1], "NEF") != NULL;
        if (is_kind(l, "raw")) {
            st = pc_io_import_raw(l->f[1], bytes.data, bytes.len, &r, &e);
            res_imp(&s, st, &r, &e, lossy);
            CHECK_RESULT(l->f[1], l->f[3], pc_buf_cstr(&s));
        } else if (is_kind(l, "rawi")) {
            st = pc_io_import(l->f[1], bytes.data, bytes.len, &r, &e);
            res_imp(&s, st, &r, &e, lossy);
            CHECK_RESULT(l->f[1], l->f[2], pc_buf_cstr(&s));
        } else if (is_kind(l, "rawd")) {
            pc_raw_develop_options o = pc_raw_develop_options_default();
            must(pc_raw_demosaic_from_id(l->f[2], &o.demosaic) ? PC_OK : PC_ERR_INVALID);
            o.limits = pc_raw_limits_default();
            st = pc_io_import_raw_with(l->f[1], bytes.data, bytes.len, &o, &r, &e);
            res_imp(&s, st, &r, &e, lossy);
            CHECK_RESULT(l->f[1], l->f[3], pc_buf_cstr(&s));
        } else {
            pc_io_raw_tuning t;
            uint32_t bits[3];
            bool applied = false;
            for (int k = 0; k < 3; k++) {
                bits[k] = (uint32_t)strtoul(l->f[2 + k], NULL, 16);
            }
            memcpy(&t.temperature, &bits[0], 4);
            memcpy(&t.tint, &bits[1], 4);
            memcpy(&t.exposure, &bits[2], 4);
            st = pc_io_import_raw_tuned(l->f[1], bytes.data, bytes.len, &t, &r, &applied, &e);
            pc_buf_truncate(&s, 0);
            if (st == PC_OK) {
                sb(&s, "%d ", applied);
            }
            s_imp(&s, st, &r, &e, false);
            CHECK_RESULT(l->f[1], l->f[5], pc_buf_cstr(&s));
        }
        if (st == PC_OK) {
            pc_io_import_result_fini(&r);
        }
        pc_buf_fini(&bytes);
    }
    pc_buf_fini(&s);
}

/* ---- layered TIFF fallbacks, stripping ---- */

static void test_tiff_layers_import(void)
{
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if ((is_kind(l, "tl-bad") && l->n == 4) || (is_kind(l, "tl-imp") && l->n == 4)) {
            const gline *src = l;
            pc_buf bytes;
            pc_io_import_result r;
            pc_error e;
            pc_status st;
            const char *b64 = l->f[2];
            const char *want = l->f[3];
            if (is_kind(l, "tl-imp")) {
                src = NULL;
                for (size_t k = 0; k < g_nlines; k++) {
                    if (is_kind(&g_lines[k], "tl-exp") && strcmp(g_lines[k].f[1], l->f[1]) == 0 &&
                        strcmp(g_lines[k].f[2], l->f[2]) == 0) {
                        src = &g_lines[k];
                    }
                }
                if (src == NULL || src->n != 5 || strcmp(src->f[4], "-") == 0) {
                    continue;
                }
                b64 = src->f[4];
            }
            bytes = b64d(b64);
            st = pc_io_import("x.tif", bytes.data, bytes.len, &r, &e);
            if (st != PC_OK && is_pending(st, &e)) {
                g_pending++;
            } else {
                res_imp(&s, st, &r, &e, false);
                CHECK_RESULT(l->f[0], want, pc_buf_cstr(&s));
            }
            if (st == PC_OK) {
                pc_io_import_result_fini(&r);
            }
            pc_buf_fini(&bytes);
        } else if (is_kind(l, "tl-strip") && l->n == 4) {
            pc_buf bytes = b64d(l->f[1]);
            pc_psd_file f;
            pc_io_warnings w = PC_IO_WARNINGS_INIT;
            pc_psd_bytes out = {NULL, 0};
            char h[17];
            must(pc_psd_file_parse(bytes.data, bytes.len, &f, NULL));
            must(pc_io_strip_foreign_order_blocks(&f, &w, NULL));
            must(pc_psd_file_to_bytes(&f, &out, NULL));
            h16(out.data, out.len, h);
            CHECK_RESULT("tl-strip", l->f[2], h);
            pc_buf_truncate(&s, 0);
            s_warn(&s, w.v, w.n);
            CHECK_RESULT("tl-strip warnings", l->f[3], pc_buf_cstr(&s));
            pc_io_warnings_fini(&w);
            pc_psd_bytes_fini(&out);
            pc_psd_file_fini(&f);
            pc_buf_fini(&bytes);
        }
    }
    pc_buf_fini(&s);
}

/* ---- merged composites ---- */

static void px_hash(pc_buf *s, const float (*px)[4], size_t n)
{
    uint8_t *b = xmalloc(n * 16);
    char h[17];
    for (size_t i = 0; i < n; i++) {
        for (size_t c = 0; c < 4; c++) {
            uint32_t u = isnan(px[i][c]) ? 0x7fc00000u : fb(px[i][c]);
            for (int k = 0; k < 4; k++) {
                b[i * 16 + c * 4 + (size_t)k] = (uint8_t)(u >> (8 * k));
            }
        }
    }
    h16(b, n * 16, h);
    sb(s, "%zu:%s", n, h);
    free(b);
}

static void merged_one(const uint8_t *data, size_t len, const char *want, const char *what, pc_buf *s)
{
    pc_psd_file f;
    pc_error e;
    pc_status st = pc_psd_file_parse(data, len, &f, &e);
    float (*px)[4] = NULL;
    size_t n = 0;
    pc_buf_truncate(s, 0);
    if (st != PC_OK) {
        sb(s, "parse ");
        sq(s, e.msg, strlen(e.msg));
        CHECK_RESULT(what, want, pc_buf_cstr(s));
        return;
    }
    st = pc_io_merged_composite(&f, &px, &n, &e);
    pc_psd_file_fini(&f);
    if (st != PC_OK && is_pending(st, &e)) {
        g_pending++;
        return;
    }
    if (st == PC_OK) {
        px_hash(s, (const float (*)[4])px, n);
        free(px);
    } else {
        s_err(s, st, &e);
    }
    CHECK_RESULT(what, want, pc_buf_cstr(s));
}

static void test_merged(void)
{
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if (is_kind(l, "mrg") && l->n == 4) {
            pc_buf bytes = b64d(l->f[2]);
            merged_one(bytes.data, bytes.len, l->f[3], l->f[1], &s);
            pc_buf_fini(&bytes);
        } else if (is_kind(l, "mrgc") && l->n == 3) {
            pc_buf bytes = PC_BUF_INIT;
            if (pc_file_read(l->f[1], (size_t)1 << 32, &bytes, NULL) != PC_OK) {
                g_skipped++;
                continue;
            }
            merged_one(bytes.data, bytes.len, l->f[2], l->f[1], &s);
            pc_buf_fini(&bytes);
        }
    }
    pc_buf_fini(&s);
}

/* ---- corpus ---- */

static void test_corpus(void)
{
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf bytes = PC_BUF_INIT;
        const char *name;
        pc_io_import_result r;
        pc_error e;
        pc_status st;
        if (!is_kind(l, "corp") || l->n != 3) {
            continue;
        }
        if (pc_file_read(l->f[1], (size_t)1 << 32, &bytes, NULL) != PC_OK ||
            !c_can_read(l->f[1], bytes.data, bytes.len)) {
            g_skipped++;
            pc_buf_fini(&bytes);
            continue;
        }
        name = strrchr(l->f[1], '/') != NULL ? strrchr(l->f[1], '/') + 1 : l->f[1];
        st = pc_io_import(name, bytes.data, bytes.len, &r, &e);
        res_imp(&s, st, &r, &e, false);
        CHECK_RESULT(l->f[1], l->f[2], pc_buf_cstr(&s));
        if (st == PC_OK) {
            const gline *x = find_line("corpx", l->f[1]);
            if (x != NULL && x->n == 4) {
                pc_io_export_result out;
                pc_io_import_result back;
                char nm[16];
                st = pc_io_export(&r.document, x->f[2], NULL, &out, &e);
                if (st == PC_OK) {
                    (void)snprintf(nm, sizeof nm, "x.%s", x->f[2]);
                    st = pc_io_import(nm, out.bytes, out.len, &back, &e);
                    res_imp(&s, st, &back, &e, false);
                    if (st == PC_OK) {
                        pc_io_import_result_fini(&back);
                    }
                    pc_io_export_result_fini(&out);
                } else {
                    pc_buf_truncate(&s, 0);
                    s_err(&s, st, &e);
                }
                CHECK_RESULT(x->f[1], x->f[3], pc_buf_cstr(&s));
            }
            pc_io_import_result_fini(&r);
        }
        pc_buf_fini(&bytes);
    }
    pc_buf_fini(&s);
}

/* ---- detection ---- */

static void test_detection(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        bool got;
        if (!is_kind(l, "det") || l->n != 4) {
            continue;
        }
        if (strncmp(l->f[1], "af:", 3) == 0) {
            got = pc_io_affinity_has_extension(l->f[1] + 3);
        } else {
            pc_buf b = b64d(l->f[2]);
            got = strcmp(l->f[1], "psd") == 0 ? pc_io_is_psd(b.data, b.len) : pc_io_is_svg(b.data, b.len);
            pc_buf_fini(&b);
        }
        CHECK_RESULT(l->f[1], l->f[3], got ? "1" : "0");
    }
}

int main(int argc, char **argv)
{
    if (load_golden() != 0) {
        (void)fprintf(stderr, "cannot read %s (run from the repository root)\n", GOLDEN);
        return 1;
    }
    RUN(test_pixels);
    RUN(test_channel_map);
    RUN(test_linked);
    RUN(test_estimate);
    RUN(test_multichannel);
    RUN(test_imports);
    RUN(test_tiff_pages);
    RUN(test_raws);
    RUN(test_tiff_layers_import);
    RUN(test_merged);
    RUN(test_corpus);
    RUN(test_detection);
    golden_summary("io core parity");
    return pc_t_finish();
}
