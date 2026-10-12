/* Parity of chunk I2 (PSD import, tagged blocks, adjustment and effect mapping) with the Rust
 * reference: tests/golden/io-psd-vectors.txt (rustref io-psd-vectors). Corpus lines skip cleanly
 * without corpus/. Results that depend on a function of a later io chunk that is still a stub
 * ("... is not ported yet (I3)") count as pending. */
#define GOLDEN "tests/golden/io-psd-vectors.txt"
#include "harness.h"
#include "golden.h"
#include "psd_dump.h"
#include "../psd/testgen.h"

#include "io/adjust_map.h"
#include "io/blocks.h"
#include "io/effects_map.h"

static void check_str(const char *what, const char *name, const char *want, const pc_buf *got)
{
    const char *g = got->data != NULL ? (const char *)got->data : "";
    g_checked++;
    pc_t_checks++;
    if (strcmp(want, g) != 0) {
        size_t i = 0;
        while (want[i] != '\0' && want[i] == g[i]) {
            i++;
        }
        PC_T_FAIL("%s %s differs at %zu:\n  want ...%.300s\n  got  ...%.300s", what, name, i,
                  want + (i > 40 ? i - 40 : 0), g + (i > 40 ? i - 40 : 0));
        if (getenv("PSDV_DUMP") != NULL) {
            (void)fprintf(stderr, "WANT %s\nGOT  %s\n", want, g);
        }
    }
}

static pc_buf b64d_n(const char *s)
{
    if (strcmp(s, "-") == 0) {
        pc_buf b = PC_BUF_INIT;
        return b;
    }
    return b64d(s);
}

static void b64e(pc_buf *out, const uint8_t *p, size_t n)
{
    must(pc_base64_encode(p, n, out, NULL));
}

static void hexkey(const char *hex, uint8_t key[4])
{
    for (size_t i = 0; i < 4; i++) {
        unsigned v = 0;
        (void)sscanf(hex + 2 * i, "%2x", &v);
        key[i] = (uint8_t)v;
    }
}

/* ---- imports ---- */

/* The result of psd_to_document for bytes, full and hashed. *pending when a later chunk's stub
 * was hit. */
static void import_res(const uint8_t *p, size_t n, pc_buf *full, pc_buf *hashed, bool *pending)
{
    pc_psd_file f;
    pc_error e;
    pc_document doc;
    pc_io_warnings w = PC_IO_WARNINGS_INIT;
    pc_status st = pc_psd_file_parse(p, n, &f, &e);
    *pending = false;
    if (st != PC_OK) {
        sb(full, "perr ");
        sjs(full, e.msg, strlen(e.msg));
        if (hashed != NULL) {
            sb(hashed, "perr ");
            sjs(hashed, e.msg, strlen(e.msg));
        }
        return;
    }
    st = pc_io_psd_to_document(&f, &doc, &w, &e);
    pc_psd_file_fini(&f);
    if (st != PC_OK) {
        *pending = is_pending(st, &e);
        sb(full, "error %s %s", pc_status_name(st), e.msg);
        if (hashed != NULL) {
            sb(hashed, "error %s %s", pc_status_name(st), e.msg);
        }
        pc_io_warnings_fini(&w);
        return;
    }
    {
        pc_buf dump = PC_BUF_INIT;
        pc_buf ws = PC_BUF_INIT;
        char h1[17];
        char h2[17];
        dd_doc(&dump, &doc);
        dd_warn(&ws, w.v, w.n);
        sb(full, "ok ");
        must(pc_buf_append(full, dump.data, dump.len, NULL));
        sb(full, " ");
        must(pc_buf_append(full, ws.data, ws.len, NULL));
        if (hashed != NULL) {
            h16(dump.data, dump.len, h1);
            h16(ws.data, ws.len, h2);
            sb(hashed, "ok h=%s n=%zu W=%s", h1, w.n, h2);
        }
        pc_buf_fini(&dump);
        pc_buf_fini(&ws);
    }
    pc_document_fini(&doc);
    pc_io_warnings_fini(&w);
}

static void test_synthetic(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf bytes;
        pc_buf full = PC_BUF_INIT;
        bool pending;
        if (!is_kind(l, "imp") || l->n < 4) {
            continue;
        }
        bytes = b64d(l->f[2]);
        import_res(bytes.data, bytes.len, &full, NULL, &pending);
        if (pending) {
            g_pending++;
        } else {
            check_str("imp", l->f[1], l->f[3], &full);
        }
        pc_buf_fini(&full);
        pc_buf_fini(&bytes);
    }
}

static void test_testgen(void)
{
    size_t n = 0;
    tg_case *cases = tg_all_cases(&n);
    for (size_t i = 0; i < n; i++) {
        const gline *l = find_line("tg", cases[i].name);
        pc_psd_bytes bytes = {NULL, 0};
        pc_buf full = PC_BUF_INIT;
        bool pending;
        if (l == NULL || l->n < 3) {
            PC_T_FAIL("no golden line for testgen case %s", cases[i].name);
            continue;
        }
        /* The reference imports the written file: write and re-parse here too. */
        must(pc_psd_file_to_bytes(&cases[i].file, &bytes, NULL));
        import_res(bytes.data, bytes.len, &full, NULL, &pending);
        if (pending) {
            g_pending++;
        } else {
            check_str("tg", cases[i].name, l->f[2], &full);
        }
        pc_buf_fini(&full);
        pc_psd_bytes_fini(&bytes);
    }
    tg_free_cases(cases, n);
}

static uint32_t xs32(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

static void test_sweeps(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        const gline *src;
        pc_buf bytes;
        pc_buf acc = PC_BUF_INIT;
        size_t count = 0;
        bool any_pending = false;
        char h[17];
        if (!is_kind(l, "sweep") || l->n < 5) {
            continue;
        }
        src = find_line("imp", l->f[1]);
        if (src == NULL) {
            PC_T_FAIL("sweep %s: no file", l->f[1]);
            continue;
        }
        bytes = b64d(src->f[2]);
        if (strcmp(l->f[2], "trunc") == 0) {
            for (size_t len = 0; len < bytes.len; len += len < 512 ? 1 : 61) {
                bool p;
                pc_buf full = PC_BUF_INIT;
                import_res(bytes.data, len, &full, &acc, &p);
                pc_buf_fini(&full);
                any_pending = any_pending || p;
                sb(&acc, "\n");
                count++;
            }
        } else {
            uint32_t s = 0x12345678u ^ (uint32_t)bytes.len;
            uint8_t *m = xmalloc(bytes.len);
            for (size_t k = 0; k < 200; k++) {
                size_t at;
                bool p;
                pc_buf full = PC_BUF_INIT;
                memcpy(m, bytes.data, bytes.len);
                at = (size_t)xs32(&s) % bytes.len;
                m[at] = (uint8_t)(xs32(&s) & 0xff);
                import_res(m, bytes.len, &full, &acc, &p);
                if (getenv("PSDV_SWEEP") != NULL && strcmp(getenv("PSDV_SWEEP"), l->f[1]) == 0) {
                    (void)printf("sweepcase\t%zu\t%s\n", at, (const char *)full.data);
                }
                pc_buf_fini(&full);
                any_pending = any_pending || p;
                sb(&acc, "\n");
                count++;
            }
            free(m);
        }
        h16(acc.data, acc.len, h);
        if (any_pending) {
            g_pending++;
        } else {
            char want[64];
            pc_buf got = PC_BUF_INIT;
            (void)snprintf(want, sizeof want, "%s\t%s", l->f[3], l->f[4]);
            sb(&got, "%zu\t%s", count, h);
            check_str("sweep", l->f[1], want, &got);
            pc_buf_fini(&got);
        }
        pc_buf_fini(&acc);
        pc_buf_fini(&bytes);
    }
}

static void test_corpus(void)
{
    struct stat sbuf;
    if (stat("corpus/psd", &sbuf) != 0) {
        for (size_t i = 0; i < g_nlines; i++) {
            if (is_kind(&g_lines[i], "corp")) {
                g_skipped++;
            }
        }
        (void)printf("corpus/ absent: corpus lines skipped\n");
        return;
    }
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf bytes = PC_BUF_INIT;
        pc_buf full = PC_BUF_INIT;
        pc_buf hashed = PC_BUF_INIT;
        bool pending;
        if (!is_kind(l, "corp") || l->n < 3) {
            continue;
        }
        if (pc_file_read(l->f[1], (size_t)1 << 31, &bytes, NULL) != PC_OK) {
            g_skipped++;
            continue;
        }
        import_res(bytes.data, bytes.len, &full, &hashed, &pending);
        if (pending) {
            g_pending++;
        } else {
            check_str("corp", l->f[1], l->f[2], &hashed);
            if (getenv("PSDV_FULL") != NULL) {
                (void)printf("corpfull\t%s\t%s\n", l->f[1], (const char *)full.data);
            }
        }
        pc_buf_fini(&bytes);
        pc_buf_fini(&full);
        pc_buf_fini(&hashed);
    }
}

/* ---- per-function vectors ---- */

static void fill_res(pc_buf *out, const uint8_t key[4], const uint8_t *p, size_t n)
{
    bool found = false;
    pc_fill f;
    must(pc_io_parse_fill(key, p, n, &found, &f, NULL));
    if (!found) {
        sb(out, "none");
        return;
    }
    DJ(out, pc_fill, &f);
    pc_fill_fini(&f);
}

static yyjson_doc *jparse(const char *s)
{
    yyjson_doc *d = NULL;
    must(pc_json_parse(s, strlen(s), &d, NULL));
    return d;
}

static void test_fills(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if (is_kind(l, "fill") && l->n >= 5) {
            uint8_t key[4];
            pc_buf d = b64d_n(l->f[3]);
            pc_buf got = PC_BUF_INIT;
            hexkey(l->f[2], key);
            fill_res(&got, key, d.data, d.len);
            check_str("fill", l->f[1], l->f[4], &got);
            pc_buf_fini(&d);
            pc_buf_fini(&got);
        } else if (is_kind(l, "wfill") && l->n >= 5) {
            yyjson_doc *jd = jparse(l->f[1]);
            pc_fill f;
            uint8_t key[4];
            pc_buf data = PC_BUF_INIT;
            pc_buf got = PC_BUF_INIT;
            pc_buf back = PC_BUF_INIT;
            must(pc_fill_from_json(yyjson_doc_get_root(jd), &f, NULL));
            yyjson_doc_free(jd);
            must(pc_io_write_fill(&f, key, &data, NULL));
            dd_hex(&got, key, 4);
            sb(&got, "\t");
            b64e(&got, data.data, data.len);
            {
                char want[1 << 14];
                (void)snprintf(want, sizeof want, "%s\t%s", l->f[2], l->f[3]);
                check_str("wfill", l->f[1], want, &got);
            }
            fill_res(&back, key, data.data, data.len);
            check_str("wfill back", l->f[1], l->f[4], &back);
            pc_fill_fini(&f);
            pc_buf_fini(&data);
            pc_buf_fini(&got);
            pc_buf_fini(&back);
        }
    }
}

static void test_colors(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if (is_kind(l, "color") && l->n >= 4) {
            pc_buf d = b64d(l->f[2]);
            pc_psd_desc desc;
            pc_color c;
            pc_buf got = PC_BUF_INIT;
            must(pc_psd_desc_parse(d.data, d.len, &desc, NULL));
            if (pc_io_color_from_desc(&desc, &c)) {
                s_color(&got, &c);
            } else {
                sb(&got, "none");
            }
            check_str("color", l->f[1], l->f[3], &got);
            pc_psd_desc_fini(&desc);
            pc_buf_fini(&d);
            pc_buf_fini(&got);
        } else if (is_kind(l, "wcolor") && l->n >= 3) {
            yyjson_doc *jd = jparse(l->f[1]);
            pc_color c;
            pc_psd_desc desc;
            pc_psd_bytes bytes = {NULL, 0};
            pc_buf got = PC_BUF_INIT;
            must(pc_color_from_json(yyjson_doc_get_root(jd), &c, NULL));
            yyjson_doc_free(jd);
            must(pc_io_color_to_desc(&c, &desc, NULL));
            must(pc_psd_desc_to_bytes(&desc, &bytes, NULL));
            b64e(&got, bytes.data, bytes.len);
            check_str("wcolor", l->f[1], l->f[2], &got);
            pc_psd_desc_fini(&desc);
            pc_psd_bytes_fini(&bytes);
            pc_buf_fini(&got);
        }
    }
}

static void fx_res(pc_buf *out,
                   pc_status (*parse)(const uint8_t *, size_t, bool *, bool *, pc_effect **, size_t *,
                                      pc_error *),
                   const uint8_t *p, size_t n)
{
    bool found = false;
    bool master = false;
    pc_effect *items = NULL;
    size_t ni = 0;
    must(parse(p, n, &found, &master, &items, &ni, NULL));
    if (!found) {
        sb(out, "none");
        return;
    }
    sb(out, "%d ", master);
    DJ_ARR(out, pc_effect, items, ni);
    for (size_t k = 0; k < ni; k++) {
        pc_effect_fini(&items[k]);
    }
    free(items);
}

static void test_effects(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if ((is_kind(l, "lfx2") || is_kind(l, "lrfx")) && l->n >= 4) {
            pc_buf d = b64d_n(l->f[2]);
            pc_buf got = PC_BUF_INIT;
            fx_res(&got, is_kind(l, "lfx2") ? pc_io_parse_lfx2 : pc_io_parse_lrfx, d.data, d.len);
            check_str(l->f[0], l->f[1], l->f[3], &got);
            pc_buf_fini(&d);
            pc_buf_fini(&got);
        } else if (is_kind(l, "wlfx2") && l->n >= 5) {
            yyjson_doc *jd = jparse(l->f[2]);
            yyjson_val *arr = yyjson_doc_get_root(jd);
            size_t n = yyjson_arr_size(arr);
            pc_effect *fx = calloc(n > 0 ? n : 1, sizeof *fx);
            pc_buf data = PC_BUF_INIT;
            pc_buf got = PC_BUF_INIT;
            pc_buf back = PC_BUF_INIT;
            for (size_t k = 0; k < n; k++) {
                must(pc_effect_from_json(yyjson_arr_get(arr, k), &fx[k], NULL));
            }
            yyjson_doc_free(jd);
            must(pc_io_write_lfx2(strcmp(l->f[1], "1") == 0, fx, n, &data, NULL));
            b64e(&got, data.data, data.len);
            check_str("wlfx2", l->f[2], l->f[3], &got);
            fx_res(&back, pc_io_parse_lfx2, data.data, data.len);
            check_str("wlfx2 back", l->f[2], l->f[4], &back);
            for (size_t k = 0; k < n; k++) {
                pc_effect_fini(&fx[k]);
            }
            free(fx);
            pc_buf_fini(&data);
            pc_buf_fini(&got);
            pc_buf_fini(&back);
        }
    }
}

static pc_io_adj_channels channels_of(const char *m)
{
    return strcmp(m, "gray") == 0   ? PC_IO_ADJ_GRAY
           : strcmp(m, "cmyk") == 0 ? PC_IO_ADJ_CMYK
           : strcmp(m, "lab") == 0  ? PC_IO_ADJ_LAB
                                    : PC_IO_ADJ_RGB;
}

static void test_adjustments(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        if (is_kind(l, "adj") && l->n >= 6) {
            uint8_t key[4];
            pc_buf d = b64d_n(l->f[3]);
            bool has_cged = strcmp(l->f[4], "-") != 0;
            pc_buf c = b64d_n(l->f[4]);
            pc_adjustment a;
            pc_buf got = PC_BUF_INIT;
            char name[64];
            hexkey(l->f[1], key);
            must(pc_io_adjust_parse(key, d.data, d.len,
                                    has_cged ? (c.data != NULL ? c.data : (const uint8_t *)"") : NULL, c.len,
                                    channels_of(l->f[2]), &a, NULL));
            DJ(&got, pc_adjustment, &a);
            (void)snprintf(name, sizeof name, "%s %s line %zu", l->f[1], l->f[2], i + 1);
            check_str("adj", name, l->f[5], &got);
            pc_adjustment_fini(&a);
            pc_buf_fini(&d);
            pc_buf_fini(&c);
            pc_buf_fini(&got);
        } else if (is_kind(l, "wadj") && l->n >= 2) {
            yyjson_doc *jd = jparse(l->f[1]);
            pc_adjustment a;
            pc_io_raw_blocks raw = {NULL, 0, 0};
            pc_buf got = PC_BUF_INIT;
            must(pc_adjustment_from_json(yyjson_doc_get_root(jd), &a, NULL));
            yyjson_doc_free(jd);
            must(pc_io_adjust_write(&a, &raw, NULL));
            for (size_t k = 0; k < raw.n; k++) {
                /* The exporter keeps a pad byte inside an odd block's length (psd_export.rs); the
                 * reference observes write() through it, so pad here too. */
                uint8_t *pd = xmalloc(raw.v[k].len + 1);
                size_t pl = raw.v[k].len + (raw.v[k].len % 2);
                if (raw.v[k].len > 0) {
                    memcpy(pd, raw.v[k].data, raw.v[k].len);
                }
                pd[raw.v[k].len] = 0;
                if (k > 0) {
                    sb(&got, ",");
                }
                dd_hex(&got, raw.v[k].key, 4);
                sb(&got, ":");
                b64e(&got, pd, pl);
                free(pd);
            }
            check_str("wadj", l->f[1], l->n >= 3 ? l->f[2] : "", &got);
            pc_io_raw_blocks_fini(&raw);
            pc_adjustment_fini(&a);
            pc_buf_fini(&got);
        }
    }
}

/* adv put start lists (mirrors rustref blocks_misc). */
static void adv_start(size_t si, pc_io_raw_blocks *raw)
{
    if (si == 1) {
        must(pc_io_raw_blocks_push(raw, (const uint8_t *)"luni", (const uint8_t[4]){0}, 4, NULL));
        must(pc_io_raw_blocks_push(raw, (const uint8_t *)"knko", (const uint8_t[4]){1, 0, 0, 0}, 4, NULL));
        must(pc_io_raw_blocks_push(raw, (const uint8_t *)"clbl", (const uint8_t[1]){1}, 1, NULL));
        must(pc_io_raw_blocks_push(raw, (const uint8_t *)"tsly", NULL, 0, NULL));
    } else if (si == 2) {
        must(pc_io_raw_blocks_push(raw, (const uint8_t *)"infx", (const uint8_t[5]){1, 2, 3, 4, 5}, 5, NULL));
    }
}

typedef struct flagset {
    uint8_t keys[8][4];
    uint8_t data[8][8];
    size_t len[8];
    size_t n;
} flagset;

static const uint8_t *flag_lookup(void *ctx, const uint8_t key[4], size_t *len)
{
    flagset *fs = ctx;
    for (size_t i = 0; i < fs->n; i++) {
        if (memcmp(fs->keys[i], key, 4) == 0) {
            *len = fs->len[i];
            return fs->data[i];
        }
    }
    return NULL;
}

static void test_blocks(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const gline *l = &g_lines[i];
        pc_buf got = PC_BUF_INIT;
        char name[96];
        (void)snprintf(name, sizeof name, "line %zu", i + 1);
        if (is_kind(l, "brst") && strcmp(l->f[1], "p") == 0) {
            pc_buf d = b64d_n(l->f[2]);
            sb(&got, "%u", pc_io_parse_brst(d.data, d.len));
            check_str("brst", name, l->f[3], &got);
            pc_buf_fini(&d);
        } else if (is_kind(l, "brst")) {
            uint32_t m = (uint32_t)strtoul(l->f[2], NULL, 10);
            bool has = false;
            pc_buf d = PC_BUF_INIT;
            must(pc_io_brst_data(m, &has, &d, NULL));
            if (has) {
                b64e(&got, d.data, d.len);
            } else {
                sb(&got, "-");
            }
            check_str("brst w", name, l->f[3], &got);
            pc_buf_fini(&d);
        } else if (is_kind(l, "adv") && strcmp(l->f[1], "put") == 0) {
            yyjson_doc *jd = jparse(l->f[2]);
            pc_advanced_blending a;
            pc_io_raw_blocks raw = {NULL, 0, 0};
            must(pc_advanced_blending_from_json(yyjson_doc_get_root(jd), &a, NULL));
            yyjson_doc_free(jd);
            adv_start((size_t)atoi(l->f[3]), &raw);
            must(pc_io_put_advanced(&a, &raw, NULL));
            for (size_t k = 0; k < raw.n; k++) {
                sb(&got, "%s", k > 0 ? "," : "");
                dd_hex(&got, raw.v[k].key, 4);
                sb(&got, ":");
                dd_hex(&got, raw.v[k].data, raw.v[k].len);
            }
            check_str("adv put", name, l->n >= 5 ? l->f[4] : "", &got);
            pc_io_raw_blocks_fini(&raw);
        } else if (is_kind(l, "adv")) {
            flagset fs;
            const char *p = l->f[3];
            pc_advanced_blending a;
            memset(&fs, 0, sizeof fs);
            while (*p != '\0' && fs.n < 8) {
                const char *colon = strchr(p, ':');
                const char *end = strchr(p, ',');
                size_t dl;
                if (colon == NULL) {
                    break;
                }
                if (end == NULL) {
                    end = p + strlen(p);
                }
                hexkey(p, fs.keys[fs.n]);
                dl = (size_t)(end - colon - 1) / 2;
                for (size_t k = 0; k < dl && k < 8; k++) {
                    unsigned v = 0;
                    (void)sscanf(colon + 1 + 2 * k, "%2x", &v);
                    fs.data[fs.n][k] = (uint8_t)v;
                }
                fs.len[fs.n++] = dl;
                p = *end == ',' ? end + 1 : end;
            }
            a = pc_io_advanced_from_blocks(flag_lookup, &fs);
            DJ(&got, pc_advanced_blending, &a);
            check_str("adv get", name, l->f[4], &got);
        } else if (is_kind(l, "bif") && strcmp(l->f[1], "p") == 0) {
            pc_buf d = b64d_n(l->f[2]);
            pc_blend_if b;
            must(pc_io_blend_if_from_ranges(d.data, d.len, &b, NULL));
            DJ(&got, pc_blend_if, &b);
            check_str("bif", name, l->f[3], &got);
            pc_blend_if_fini(&b);
            pc_buf_fini(&d);
        } else if (is_kind(l, "bif")) {
            yyjson_doc *jd = jparse(l->f[2]);
            pc_blend_if b;
            pc_psd_bytes r = {NULL, 0};
            must(pc_blend_if_from_json(yyjson_doc_get_root(jd), &b, NULL));
            yyjson_doc_free(jd);
            must(pc_io_ranges_from_blend_if(&b, (size_t)atoi(l->f[3]), &r, NULL));
            b64e(&got, r.data, r.len);
            check_str("bif w", name, l->f[4], &got);
            pc_psd_bytes_fini(&r);
            pc_blend_if_fini(&b);
        } else if (is_kind(l, "lspf")) {
            uint32_t v = (uint32_t)strtoul(l->f[1], NULL, 10);
            pc_locks k = pc_io_locks_from_lspf(v);
            DJ(&got, pc_locks, &k);
            sb(&got, "\t%u", pc_io_lspf_from_locks(&k));
            {
                char want[256];
                (void)snprintf(want, sizeof want, "%s\t%s", l->f[2], l->f[3]);
                check_str("lspf", name, want, &got);
            }
        } else if (is_kind(l, "lbl")) {
            pc_label_color c = pc_io_label_from_index((uint16_t)atoi(l->f[1]));
            DJ(&got, pc_label_color, &c);
            sb(&got, "\t%u", pc_io_label_index(c));
            {
                char want[256];
                (void)snprintf(want, sizeof want, "%s\t%s", l->f[2], l->f[3]);
                check_str("lbl", name, want, &got);
            }
        } else if (is_kind(l, "smart") && l->n >= 6) {
            uint8_t key[4];
            pc_buf d = b64d_n(l->f[3]);
            pc_str id = PC_STR_INIT;
            pc_affine a;
            const gline *pl = find_line("persp", l->f[1]);
            const gline *wl = find_line("warp", l->f[1]);
            bool found = false;
            double m[9];
            pc_warp w;
            hexkey(l->f[2], key);
            must(pc_io_parse_smart(key, d.data, d.len, &id, &a, NULL));
            sq(&got, pc_str_c(&id), id.len);
            sb(&got, "\t");
            DJ(&got, pc_affine, &a);
            {
                char want[4096];
                (void)snprintf(want, sizeof want, "%s\t%s", l->f[4], l->f[5]);
                check_str("smart", l->f[1], want, &got);
            }
            pc_buf_fini(&got);
            must(pc_io_parse_smart_perspective(key, d.data, d.len, &found, m, NULL));
            if (found) {
                for (size_t k = 0; k < 9; k++) {
                    sb(&got, "%s%016llx", k > 0 ? "," : "", (unsigned long long)dd_db(m[k]));
                }
            } else {
                sb(&got, "-");
            }
            if (pl != NULL) {
                check_str("persp", l->f[1], pl->f[2], &got);
            }
            pc_buf_fini(&got);
            must(pc_io_parse_placed_warp(key, d.data, d.len, &found, &w, NULL));
            dd_warp(&got, found, &w);
            if (found) {
                pc_warp_fini(&w);
            }
            if (wl != NULL) {
                check_str("warp", l->f[1], wl->f[2], &got);
            }
            pc_str_fini(&id);
            pc_buf_fini(&d);
        } else if (is_kind(l, "tysh")) {
            pc_buf d = b64d_n(l->f[2]);
            bool found = false;
            pc_str t = PC_STR_INIT;
            pc_affine a;
            must(pc_io_parse_tysh(d.data, d.len, &found, &t, &a, NULL));
            if (found) {
                sq(&got, pc_str_c(&t), t.len);
                sb(&got, " ");
                DJ(&got, pc_affine, &a);
            } else {
                sb(&got, "none");
            }
            check_str("tysh", l->f[1], l->f[3], &got);
            pc_str_fini(&t);
            pc_buf_fini(&d);
        } else if (is_kind(l, "fxen")) {
            pc_buf d = b64d_n(l->f[2]);
            bool on = false;
            must(pc_io_effects_enabled(d.data, d.len, &on, NULL));
            sb(&got, "%d", on);
            check_str("fxen", l->f[1], l->f[3], &got);
            pc_buf_fini(&d);
        }
        pc_buf_fini(&got);
    }
}

int main(int argc, char **argv)
{
    if (load_golden() != 0) {
        (void)fprintf(stderr, "cannot read %s\n", GOLDEN);
        return 1;
    }
    RUN(test_fills);
    RUN(test_colors);
    RUN(test_effects);
    RUN(test_adjustments);
    RUN(test_blocks);
    RUN(test_testgen);
    RUN(test_synthetic);
    RUN(test_sweeps);
    RUN(test_corpus);
    golden_summary("io-psd-vectors");
    return pc_t_finish();
}
