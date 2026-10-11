/* Parity with the Rust reference: tests/golden/format-vectors.txt was written by
 * `rustref format-vectors` (tools/rustref/src/format_vectors.rs; its header lists the lines).
 *
 *   - the ZIP writer's bytes must be identical (STORE entries);
 *   - the ZIP reader must give the same entries, payloads and error texts for every synthetic,
 *     hostile, truncated and mutated archive;
 *   - Rust-written bundles (rustref writes -> C reads): every tile and blob loads through the
 *     verified fetch path; re-saving their manifest and objects through the C writer yields the
 *     same entries in the same order, byte-identical manifest/previews, payloads that decompress
 *     to the same bytes, the same save statistics (first and incremental saves), and the same
 *     directory layout;
 *   - damaged objects fail with the reference's errors (zstd decoder messages compare by kind:
 *     ruzstd and libzstd word them differently, REWRITE_PLAN.md §7);
 *   - limits, the manifest depth check, buffer_for and temp_name match.
 *   - document level (doc-* lines): the same documents built in C save to the same manifest,
 *     previews and statistics, and load back equal (zip and directory bundles).
 *
 * C writes -> Rust reads: with PC_FORMAT_XDIR=DIR this test also writes its re-saved bundles to
 * DIR/NAME.pcraft and DIR/NAME.dir; then `PC_FORMAT_XREAD=DIR rustref format-vectors` loads them
 * with the reference and prints `xread` lines (all must say equal/identical). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/base64.h"
#include "base/compress.h"
#include "base/hash.h"
#include "base/utf8.h"
#include "codecs/codecs.h"
#include "common.h"
#include "format/bridge.h"
#include "harness.h"
#include "rich_doc.h"

#define GOLDEN "tests/golden/format-vectors.txt"
#define MAXF 8

typedef struct line {
    char *f[MAXF];
    size_t n;
} line;

static line *g_lines;
static size_t g_nlines;
static char *g_text;

static int load_golden(void)
{
    pc_buf b = PC_BUF_INIT;
    size_t cap = 0;
    char *p;
    if (t_read_file(GOLDEN, &b) != 0) {
        return -1;
    }
    g_text = (char *)pc_buf_take(&b, NULL);
    p = g_text;
    while (*p != '\0') {
        char *end = strchr(p, '\n');
        line l;
        if (end != NULL) {
            *end = '\0';
        }
        memset(&l, 0, sizeof l);
        while (l.n < MAXF) {
            char *tab = strchr(p, '\t');
            l.f[l.n++] = p;
            if (tab == NULL) {
                break;
            }
            *tab = '\0';
            p = tab + 1;
        }
        if (g_nlines == cap) {
            cap = cap ? cap * 2 : 1024;
            g_lines = realloc(g_lines, cap * sizeof *g_lines);
            if (g_lines == NULL) {
                return -1;
            }
        }
        g_lines[g_nlines++] = l;
        if (end == NULL) {
            break;
        }
        p = end + 1;
    }
    return 0;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

static void unhex(const char *s, pc_buf *out)
{
    size_t n = strlen(s);
    pc_buf_truncate(out, 0);
    (void)pc_buf_reserve(out, n / 2, NULL);
    for (size_t i = 0; i + 1 < n; i += 2) {
        (void)pc_buf_append_byte(out, (uint8_t)(hexval(s[i]) << 4 | hexval(s[i + 1])), NULL);
    }
    if (out->data == NULL) {
        (void)pc_buf_append(out, "", 0, NULL);
    }
}

static void b3_16(const uint8_t *d, size_t n, char out[17])
{
    char hex[65];
    pc_blake3_hex(d, n, hex);
    memcpy(out, hex, 16);
    out[16] = '\0';
}

/* Rust `{:?}` of a string. */
static void dq(pc_buf *out, const char *s, size_t n)
{
    char q[1100];
    pc_utf8_debug_quote((const uint8_t *)s, n, q, sizeof q);
    (void)pc_buf_append_str(out, q, NULL);
}

static void rd(pc_buf *s, pc_status st, const pc_error *e, const pc_buf *d)
{
    if (st == PC_OK) {
        char h[17];
        b3_16(d->data, d->len, h);
        (void)pc_buf_appendf(s, NULL, "%zu:%s", d->len, h);
    } else {
        (void)pc_buf_append_str(s, "E", NULL);
        dq(s, e->msg, strlen(e->msg));
    }
}

static void zsum(const uint8_t *b, size_t n, pc_buf *s)
{
    pc_zip_reader r;
    pc_error e;
    pc_status st;
    pc_buf_truncate(s, 0);
    (void)pc_buf_appendf(s, NULL, "pc=%d ", pc_format_is_pcraft(b, n) ? 1 : 0);
    st = pc_zip_reader_open(&r, b, n, &e);
    if (st != PC_OK) {
        (void)pc_buf_append_str(s, "E", NULL);
        dq(s, e.msg, strlen(e.msg));
        return;
    }
    (void)pc_buf_appendf(s, NULL, "ok%zu", r.count);
    for (size_t i = 0; i < r.count; i++) {
        pc_buf d = PC_BUF_INIT;
        (void)pc_buf_append_str(s, " ", NULL);
        dq(s, r.entries[i].name.ptr != NULL ? r.entries[i].name.ptr : "", r.entries[i].name.len);
        (void)pc_buf_append_str(s, "=", NULL);
        st = pc_zip_read(&r, &r.entries[i], (size_t)1 << 30, &d, &e);
        rd(s, st, &e, &d);
        (void)pc_buf_append_str(s, "/", NULL);
        st = pc_zip_read(&r, &r.entries[i], 3, &d, &e);
        rd(s, st, &e, &d);
        pc_buf_fini(&d);
    }
    {
        pc_buf d = PC_BUF_INIT;
        (void)pc_buf_append_str(s, " rbn=", NULL);
        st = pc_zip_read_by_name(&r, "manifest.json", (size_t)1 << 30, &d, &e);
        rd(s, st, &e, &d);
        pc_buf_fini(&d);
    }
    pc_zip_reader_close(&r);
}

/* ---- writer cases (the same entries as rustref's) ---- */

typedef struct ent {
    const char *name;
    const uint8_t *data;
    size_t len;
} ent;

static void write_zip(const ent *e, size_t n, pc_buf *out)
{
    pc_zip_writer w;
    pc_zip_writer_init(&w);
    for (size_t i = 0; i < n; i++) {
        CHECK(pc_zip_writer_add(&w, e[i].name, strlen(e[i].name), e[i].data, e[i].len, NULL) == PC_OK);
    }
    CHECK(pc_zip_writer_finish(&w, out, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
}

static uint8_t *pattern(size_t n)
{
    uint8_t *p = malloc(n);
    for (uint32_t i = 0; p != NULL && i < n; i++) {
        p[i] = (uint8_t)((uint32_t)(i * 2654435761u) >> 13);
    }
    return p;
}

static size_t g_checked[32];
enum {
    K_CRC,
    K_ZIPW,
    K_ZIPR,
    K_TRUNC,
    K_MUT,
    K_ISPC,
    K_BUF,
    K_TMP,
    K_BUNDLE,
    K_XLOAD,
    K_XLIMIT,
    K_XDEPTH,
    K_DOC
};

static void test_crc_and_writer(void)
{
    uint8_t all[256];
    uint8_t *pat = pattern(1000003);
    uint8_t *a5000 = malloc(5000);
    char longname[301];
    pc_buf out = PC_BUF_INIT;
    for (int i = 0; i < 256; i++) {
        all[i] = (uint8_t)i;
    }
    memset(a5000, 'A', 5000);
    memset(longname, 'n', 300);
    longname[300] = '\0';
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        if (strcmp(l->f[0], "crc") == 0) {
            const uint8_t *d = (const uint8_t *)"";
            size_t n = 0;
            char got[9];
            if (strcmp(l->f[1], "123456789") == 0) {
                d = (const uint8_t *)"123456789";
                n = 9;
            } else if (strcmp(l->f[1], "bytes0-255") == 0) {
                d = all;
                n = 256;
            } else if (strcmp(l->f[1], "pattern1000003") == 0) {
                d = pat;
                n = 1000003;
            }
            (void)snprintf(got, sizeof got, "%08x", pc_zip_crc32(d, n));
            CHECK_EQ_STR(got, l->f[2]);
            g_checked[K_CRC]++;
        } else if (strcmp(l->f[0], "zipw") == 0) {
            const uint8_t four[4] = {0, 1, 2, 3};
            ent e[3];
            size_t n = 0;
            char hex[65];
            if (strcmp(l->f[1], "one") == 0) {
                e[n++] = (ent){"manifest.json", (const uint8_t *)"{}", 2};
            } else if (strcmp(l->f[1], "three") == 0) {
                e[n++] = (ent){"a.txt", (const uint8_t *)"hello", 5};
                e[n++] = (ent){"dir/b.bin", four, 4};
                e[n++] = (ent){"empty", NULL, 0};
            } else if (strcmp(l->f[1], "utf8") == 0) {
                e[n++] = (ent){"t\xc3\xa9st/\xe2\x98\x83.zst", all, 256};
                e[n++] = (ent){"big", pat, 70000};
            } else if (strcmp(l->f[1], "long") == 0) {
                e[n++] = (ent){longname, a5000, 5000};
            }
            write_zip(e, n, &out);
            pc_blake3_hex(out.data, out.len, hex);
            CHECK_EQ_UINT(out.len, strtoull(l->f[2], NULL, 10));
            CHECK_EQ_STR(hex, l->f[3]);
            if (l->n > 4) {
                pc_buf h = PC_BUF_INIT;
                unhex(l->f[4], &h);
                CHECK(h.len == out.len && memcmp(h.data, out.data, h.len) == 0);
                pc_buf_fini(&h);
            }
            g_checked[K_ZIPW]++;
        } else if (strcmp(l->f[0], "zipw-limit") == 0) {
            pc_zip_writer w;
            pc_error e;
            pc_buf r = PC_BUF_INIT;
            char hex[65];
            pc_zip_writer_init(&w);
            for (uint32_t k = 0; k < 65535; k++) {
                const char *nm = k == 0 ? "first" : "";
                if (pc_zip_writer_add(&w, nm, strlen(nm), NULL, 0, NULL) != PC_OK) {
                    PC_T_FAIL("add %u", k);
                    break;
                }
            }
            if (pc_zip_writer_add(&w, "over", 4, NULL, 0, &e) == PC_OK) {
                (void)pc_buf_append_str(&r, "ok", NULL);
            } else {
                (void)pc_buf_append_str(&r, "E", NULL);
                dq(&r, e.msg, strlen(e.msg));
            }
            CHECK_EQ_STR(pc_buf_cstr(&r), l->f[1]);
            CHECK(pc_zip_writer_finish(&w, &out, NULL) == PC_OK);
            pc_blake3_hex(out.data, out.len, hex);
            CHECK_EQ_UINT(out.len, strtoull(l->f[2], NULL, 10));
            CHECK_EQ_STR(hex, l->f[3]);
            pc_zip_writer_fini(&w);
            pc_buf_fini(&r);
            g_checked[K_ZIPW]++;
        }
    }
    pc_buf_fini(&out);
    free(pat);
    free(a5000);
}

/* The sweep archive's DEFLATE entries are `d` and `z`. When their data is damaged, the
 * reference's miniz_oxide and zlib notice at different points: "corrupt deflate stream",
 * "incomplete deflate stream", "has wrong size" and "failed its CRC check" are one outcome there
 * (REWRITE_PLAN.md §7, as in F17). STORE entries are compared exactly. */
static size_t g_deflate_kind;

static void normalize_deflate(const char *in, pc_buf *out)
{
    static const char *const FORMS[] = {"`: corrupt deflate stream\"", "`: incomplete deflate stream\"",
                                        "` has wrong size\"", "` failed its CRC check\""};
    static const char *const NAMES[] = {"zip entry `d", "zip entry `z"};
    pc_buf_truncate(out, 0);
    (void)pc_buf_append_str(out, in, NULL);
    for (size_t n = 0; n < 2; n++) {
        for (size_t f = 0; f < 4; f++) {
            char pat[96];
            char *at;
            (void)snprintf(pat, sizeof pat, "%s%s", NAMES[n], FORMS[f]);
            while ((at = strstr((char *)out->data, pat)) != NULL) {
                size_t off = (size_t)(at - (char *)out->data);
                size_t pl = strlen(pat);
                pc_buf rest = PC_BUF_INIT;
                (void)pc_buf_append_str(&rest, at + pl, NULL);
                pc_buf_truncate(out, off);
                (void)pc_buf_appendf(out, NULL, "<damaged %c>\"%s", NAMES[n][11], pc_buf_cstr(&rest));
                pc_buf_fini(&rest);
            }
        }
    }
}

static bool same_sweep_outcome(const char *got, const char *want)
{
    pc_buf a = PC_BUF_INIT;
    pc_buf b = PC_BUF_INIT;
    bool same;
    normalize_deflate(got, &a);
    normalize_deflate(want, &b);
    same = strcmp(pc_buf_cstr(&a), pc_buf_cstr(&b)) == 0;
    pc_buf_fini(&a);
    pc_buf_fini(&b);
    if (same) {
        g_deflate_kind++;
    }
    return same;
}

static void test_reader(void)
{
    pc_buf b = PC_BUF_INIT;
    pc_buf base = PC_BUF_INIT;
    pc_buf s = PC_BUF_INIT;
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        if (strcmp(l->f[0], "zipr") == 0) {
            unhex(l->f[2], &b);
            zsum(b.data, b.len, &s);
            if (strcmp(pc_buf_cstr(&s), l->f[3]) != 0) {
                PC_T_FAIL("zipr %s:\n  got  %s\n  want %s", l->f[1], pc_buf_cstr(&s), l->f[3]);
            }
            g_checked[K_ZIPR]++;
        } else if (strcmp(l->f[0], "base") == 0) {
            unhex(l->f[1], &base);
        } else if (strcmp(l->f[0], "trunc") == 0) {
            size_t cut = strtoull(l->f[1], NULL, 10);
            zsum(base.data, cut, &s);
            if (strcmp(pc_buf_cstr(&s), l->f[2]) != 0 && !same_sweep_outcome(pc_buf_cstr(&s), l->f[2])) {
                PC_T_FAIL("trunc %zu:\n  got  %s\n  want %s", cut, pc_buf_cstr(&s), l->f[2]);
            }
            g_checked[K_TRUNC]++;
        } else if (strcmp(l->f[0], "mut") == 0) {
            size_t pos = strtoull(l->f[1], NULL, 10);
            uint8_t x = (uint8_t)strtoul(l->f[2], NULL, 10);
            pc_buf_truncate(&b, 0);
            (void)pc_buf_append(&b, base.data, base.len, NULL);
            b.data[pos] ^= x;
            zsum(b.data, b.len, &s);
            if (strcmp(pc_buf_cstr(&s), l->f[3]) != 0 && !same_sweep_outcome(pc_buf_cstr(&s), l->f[3])) {
                PC_T_FAIL("mut %zu^%u:\n  got  %s\n  want %s", pos, x, pc_buf_cstr(&s), l->f[3]);
            }
            g_checked[K_MUT]++;
        } else if (strcmp(l->f[0], "ispc") == 0 && hexval(l->f[1][0]) >= 0 && strlen(l->f[1]) % 2 == 0 &&
                   strcmp(l->f[1], "empty") != 0) {
            unhex(l->f[1], &b);
            CHECK_EQ_STR(pc_format_is_pcraft(b.data, b.len) ? "true" : "false", l->f[2]);
            g_checked[K_ISPC]++;
        } else if (strcmp(l->f[0], "ispc") == 0 && l->f[1][0] == '\0') {
            CHECK_EQ_STR(pc_format_is_pcraft((const uint8_t *)"", 0) ? "true" : "false", l->f[2]);
            g_checked[K_ISPC]++;
        }
    }
    pc_buf_fini(&b);
    pc_buf_fini(&base);
    pc_buf_fini(&s);
}

static void test_read_and_tmpname(void)
{
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        if (strcmp(l->f[0], "bufferfor") == 0) {
            uint64_t n = strtoull(l->f[1], NULL, 10);
            pc_buf b = PC_BUF_INIT;
            pc_error e;
            pc_buf got = PC_BUF_INIT;
            if (pc_read_buffer_for(n, &b, &e) == PC_OK) {
                (void)pc_buf_appendf(&got, NULL, "ok %zu", b.cap - 1);
            } else {
                CHECK(e.code == PC_ERR_NOMEM);
                (void)pc_buf_append_str(&got, "EOutOfMemory ", NULL);
                dq(&got, e.msg, strlen(e.msg));
            }
            CHECK_EQ_STR(pc_buf_cstr(&got), l->f[2]);
            pc_buf_fini(&b);
            pc_buf_fini(&got);
            g_checked[K_BUF]++;
        } else if (strcmp(l->f[0], "tmpname") == 0) {
            /* The input is quoted with {:?}; the inputs are plain text without escapes. */
            char in[512];
            size_t n = strlen(l->f[1]);
            pc_buf t = PC_BUF_INIT;
            pc_buf got = PC_BUF_INIT;
            char marker[64];
            const char *s;
            const char *m;
            memcpy(in, l->f[1] + 1, n - 2);
            in[n - 2] = '\0';
            CHECK(pc_atomic_temp_name(in, &t, NULL) == PC_OK);
            s = pc_buf_cstr(&t);
            (void)snprintf(marker, sizeof marker, ".%ld-", (long)getpid());
            {
                /* rfind of the marker */
                const char *last = NULL;
                for (const char *q = strstr(s, marker); q != NULL; q = strstr(q + 1, marker)) {
                    last = q;
                }
                m = last;
            }
            if (m == NULL) {
                PC_T_FAIL("no pid in %s", s);
            } else {
                const char *rest = m + strlen(marker);
                const char *dot = strchr(rest, '.');
                pc_buf masked = PC_BUF_INIT;
                (void)pc_buf_append(&masked, s, (size_t)(m - s), NULL);
                (void)pc_buf_appendf(&masked, NULL, ".PID-N%s", dot != NULL ? dot : "");
                dq(&got, pc_buf_cstr(&masked), masked.len);
                CHECK_EQ_STR(pc_buf_cstr(&got), l->f[2]);
                pc_buf_fini(&masked);
            }
            pc_buf_fini(&t);
            pc_buf_fini(&got);
            g_checked[K_TMP]++;
        }
    }
}

/* ---- bundles ---- */

typedef struct bundle {
    const char *name;
    pc_buf bytes;
    const char *stats;
    const char *stats2;
    const char *dir;
    const char *dirstats2;
} bundle;

static bundle g_b[8];
static size_t g_nb;

static bundle *find_bundle(const char *name)
{
    for (size_t i = 0; i < g_nb; i++) {
        if (strcmp(g_b[i].name, name) == 0) {
            return &g_b[i];
        }
    }
    return NULL;
}

/* Tile lengths from `tile NAME PATH LEN`. */
static size_t tile_len(const char *bundle_name, const char *path)
{
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        if (strcmp(l->f[0], "tile") == 0 && strcmp(l->f[1], bundle_name) == 0 && strcmp(l->f[2], path) == 0) {
            return strtoull(l->f[3], NULL, 10);
        }
    }
    return SIZE_MAX;
}

static void stats_text(const pc_format_save_stats *s, char *out, size_t cap)
{
    (void)snprintf(out, cap, "%zu,%zu,%zu,%zu,%zu,%zu,%zu", s->tiles_total, s->tiles_written, s->tiles_reused,
                   s->blobs_total, s->blobs_written, s->objects_removed, s->manifest_bytes);
}

/* Prepared bundle from a Rust bundle: its manifest, previews and every object's content. */
static int prepare_from(const bundle *bd, pc_format_prepared *p)
{
    pc_zip_reader r;
    pc_format_sink *sink = NULL;
    pc_buf manifest = PC_BUF_INIT;
    pc_buf d = PC_BUF_INIT;
    pc_buf raw = PC_BUF_INIT;
    int rc = -1;
    if (pc_zip_reader_open(&r, bd->bytes.data, bd->bytes.len, NULL) != PC_OK) {
        return -1;
    }
    if (pc_format_sink_new(NULL, &sink, NULL) != PC_OK) {
        goto done;
    }
    for (size_t i = 0; i < r.count; i++) {
        const pc_zip_entry *e = &r.entries[i];
        const char *n = e->name.ptr;
        char hash[65];
        if (pc_zip_read(&r, e, (size_t)1 << 30, &raw, NULL) != PC_OK) {
            goto done;
        }
        if (strncmp(n, "tiles/", 6) == 0 || strncmp(n, "blobs/", 6) == 0) {
            if (pc_zstd_decompress(raw.data, raw.len, (size_t)1 << 30, 0, &d, NULL, NULL) != PC_OK ||
                pc_format_sink_bytes(sink, n[0] == 't', d.data, d.len, hash, NULL) != PC_OK) {
                goto done;
            }
            CHECK(strncmp(n + 6, hash, 64) == 0);
        }
    }
    if (pc_zip_read_by_name(&r, PC_FORMAT_MANIFEST, (size_t)1 << 30, &manifest, NULL) != PC_OK ||
        pc_format_prepared_from_sink(sink, manifest.data, manifest.len, p, NULL) != PC_OK) {
        goto done;
    }
    for (size_t k = 0; k < 2; k++) {
        const char *pv = k == 0 ? PC_FORMAT_THUMB : PC_FORMAT_COMPOSITE;
        if (pc_zip_find(&r, pv, strlen(pv)) != NULL) {
            if (pc_zip_read_by_name(&r, pv, (size_t)1 << 30, &raw, NULL) != PC_OK ||
                pc_format_prepared_add_preview(p, pv, raw.data, raw.len, NULL) != PC_OK) {
                pc_format_prepared_fini(p);
                goto done;
            }
        }
    }
    rc = 0;
done:
    pc_buf_fini(&manifest);
    pc_buf_fini(&d);
    pc_buf_fini(&raw);
    pc_format_sink_free(sink);
    pc_zip_reader_close(&r);
    return rc;
}

/* Same entries, same order, identical non-object payloads, objects equal after decompression;
 * and each archive is exactly what the zip writer makes of its entries. */
/* PNG previews decode to the same RGBA8 pixels (their deflate streams may differ: zlib vs
 * miniz_oxide, REWRITE_PLAN.md B2). */
static bool same_png_pixels(const pc_buf *a, const pc_buf *b)
{
    pc_cimage ia;
    pc_cimage ib;
    uint8_t *pa = NULL;
    uint8_t *pb = NULL;
    size_t na = 0;
    size_t nb = 0;
    bool same = false;
    if (pc_cdecode(a->data, a->len, &ia, NULL) != PC_OK) {
        return false;
    }
    if (pc_cdecode(b->data, b->len, &ib, NULL) == PC_OK) {
        same = ia.width == ib.width && ia.height == ib.height &&
               pc_cimage_to_rgba8(&ia, &pa, &na, NULL) == PC_OK &&
               pc_cimage_to_rgba8(&ib, &pb, &nb, NULL) == PC_OK && na == nb && memcmp(pa, pb, na) == 0;
        pc_cimage_fini(&ib);
    }
    pc_cimage_fini(&ia);
    free(pa);
    free(pb);
    return same;
}

static void compare_bundles_ex(const char *name, const uint8_t *a, size_t an, const uint8_t *b, size_t bn,
                               bool previews_by_pixels)
{
    pc_zip_reader ra;
    pc_zip_reader rb;
    if (pc_zip_reader_open(&ra, a, an, NULL) != PC_OK || pc_zip_reader_open(&rb, b, bn, NULL) != PC_OK) {
        PC_T_FAIL("%s: unreadable bundle", name);
        return;
    }
    CHECK_EQ_UINT(ra.count, rb.count);
    for (size_t i = 0; i < ra.count && i < rb.count; i++) {
        pc_buf da = PC_BUF_INIT;
        pc_buf db = PC_BUF_INIT;
        const char *n = ra.entries[i].name.ptr;
        CHECK_EQ_STR(n, rb.entries[i].name.ptr);
        CHECK(pc_zip_read(&ra, &ra.entries[i], (size_t)1 << 30, &da, NULL) == PC_OK);
        CHECK(pc_zip_read(&rb, &rb.entries[i], (size_t)1 << 30, &db, NULL) == PC_OK);
        if (strncmp(n, "tiles/", 6) == 0 || strncmp(n, "blobs/", 6) == 0) {
            pc_buf ua = PC_BUF_INIT;
            pc_buf ub = PC_BUF_INIT;
            CHECK(pc_zstd_decompress(da.data, da.len, (size_t)1 << 30, 0, &ua, NULL, NULL) == PC_OK);
            CHECK(pc_zstd_decompress(db.data, db.len, (size_t)1 << 30, 0, &ub, NULL, NULL) == PC_OK);
            CHECK(ua.len == ub.len && (ua.len == 0 || memcmp(ua.data, ub.data, ua.len) == 0));
            pc_buf_fini(&ua);
            pc_buf_fini(&ub);
        } else if (previews_by_pixels && strcmp(n, PC_FORMAT_MANIFEST) != 0) {
            if (!same_png_pixels(&da, &db)) {
                PC_T_FAIL("%s: %s pixels differ", name, n);
            }
        } else if (!(da.len == db.len && (da.len == 0 || memcmp(da.data, db.data, da.len) == 0))) {
            PC_T_FAIL("%s: %s differs (%zu vs %zu bytes)", name, n, da.len, db.len);
        }
        pc_buf_fini(&da);
        pc_buf_fini(&db);
    }
    for (int k = 0; k < 2; k++) {
        pc_zip_reader *r = k == 0 ? &ra : &rb;
        pc_zip_writer w;
        pc_buf out = PC_BUF_INIT;
        pc_zip_writer_init(&w);
        for (size_t i = 0; i < r->count; i++) {
            pc_buf d = PC_BUF_INIT;
            CHECK(pc_zip_read(r, &r->entries[i], (size_t)1 << 30, &d, NULL) == PC_OK);
            CHECK(pc_zip_writer_add(&w, r->entries[i].name.ptr, r->entries[i].name.len, d.data, d.len,
                                    NULL) == PC_OK);
            pc_buf_fini(&d);
        }
        CHECK(pc_zip_writer_finish(&w, &out, NULL) == PC_OK);
        CHECK(out.len == r->len && memcmp(out.data, r->bytes, out.len) == 0);
        pc_zip_writer_fini(&w);
        pc_buf_fini(&out);
    }
    pc_zip_reader_close(&ra);
    pc_zip_reader_close(&rb);
}

static void compare_bundles(const char *name, const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    compare_bundles_ex(name, a, an, b, bn, false);
}

static void check_fetch_all(const bundle *bd)
{
    pc_format_source *src = NULL;
    pc_format_fetch *f = NULL;
    pc_format_load_opts o = pc_format_load_opts_default();
    pc_zip_reader r;
    CHECK(pc_format_source_zip(bd->bytes.data, bd->bytes.len, &src, NULL) == PC_OK);
    CHECK(pc_format_fetch_new(src, &o, &f, NULL) == PC_OK);
    CHECK(pc_zip_reader_open(&r, bd->bytes.data, bd->bytes.len, NULL) == PC_OK);
    for (size_t i = 0; i < r.count; i++) {
        const char *n = r.entries[i].name.ptr;
        char hash[65];
        pc_error e;
        if (strlen(n) != 6 + 64 + 4) {
            continue;
        }
        memcpy(hash, n + 6, 64);
        hash[64] = '\0';
        if (strncmp(n, "tiles/", 6) == 0) {
            pc_buf d = PC_BUF_INIT;
            size_t len = tile_len(bd->name, n);
            CHECK(len != SIZE_MAX);
            if (pc_format_fetch_tile(f, hash, len, &d, &e) != PC_OK) {
                PC_T_FAIL("%s %s: %s", bd->name, n, e.msg);
            }
            CHECK_EQ_UINT(d.len, len);
            pc_buf_fini(&d);
        } else if (strncmp(n, "blobs/", 6) == 0) {
            pc_blob *b = NULL;
            pc_blob *b2 = NULL;
            if (pc_format_fetch_blob(f, hash, &b, &e) != PC_OK) {
                PC_T_FAIL("%s %s: %s", bd->name, n, e.msg);
            }
            CHECK(pc_format_fetch_blob(f, hash, &b2, NULL) == PC_OK);
            CHECK(b == b2); /* cached */
            pc_blob_release(b);
            pc_blob_release(b2);
        }
    }
    pc_zip_reader_close(&r);
    pc_format_fetch_free(f);
    pc_format_source_free(src);
}

static void test_bundles(void)
{
    const char *xdir = getenv("PC_FORMAT_XDIR");
    char tmp[1024];
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        if (strcmp(l->f[0], "bundle") == 0 && g_nb < 8) {
            bundle *b = &g_b[g_nb++];
            b->name = l->f[1];
            b->stats = l->f[2];
            CHECK(pc_base64_decode(l->f[4], strlen(l->f[4]), &b->bytes, NULL) == PC_OK);
            CHECK_EQ_UINT(b->bytes.len, strtoull(l->f[3], NULL, 10));
        }
    }
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        bundle *b = l->n > 1 ? find_bundle(l->f[1]) : NULL;
        if (b == NULL) {
            continue;
        }
        if (strcmp(l->f[0], "doc-stats2") == 0) {
            b->stats2 = l->f[2];
        } else if (strcmp(l->f[0], "dir") == 0) {
            b->dir = l->f[3];
            b->dirstats2 = l->f[2]; /* first directory save; the second follows */
        } else if (strcmp(l->f[0], "doc-dirstats2") == 0) {
            b->dirstats2 = l->f[2];
        } else if (strcmp(l->f[0], "ispc") == 0) {
            CHECK_EQ_STR(pc_format_is_pcraft(b->bytes.data, b->bytes.len) ? "true" : "false", l->f[2]);
            g_checked[K_ISPC]++;
        }
    }
    CHECK(t_mkdtemp("parity", tmp, sizeof tmp) == 0);
    for (size_t k = 0; k < g_nb; k++) {
        bundle *b = &g_b[k];
        pc_format_prepared p;
        pc_pcraft_writer *w = NULL;
        pc_buf out = PC_BUF_INIT;
        pc_buf out2 = PC_BUF_INIT;
        pc_buf listing = PC_BUF_INIT;
        pc_format_save_stats st;
        char text[256];
        char dir[1100];
        const char *first_dir_stats = NULL;

        check_fetch_all(b);
        if (prepare_from(b, &p) != 0) {
            PC_T_FAIL("%s: cannot prepare", b->name);
            continue;
        }
        CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
        CHECK(pc_pcraft_writer_save_zip_prepared(w, &p, &out, &st, NULL) == PC_OK);
        stats_text(&st, text, sizeof text);
        CHECK_EQ_STR(text, b->stats);
        compare_bundles(b->name, b->bytes.data, b->bytes.len, out.data, out.len);
        CHECK(pc_pcraft_writer_save_zip_prepared(w, &p, &out2, &st, NULL) == PC_OK);
        stats_text(&st, text, sizeof text);
        CHECK_EQ_STR(text, b->stats2);
        CHECK(out.len == out2.len && memcmp(out.data, out2.data, out.len) == 0);
        pc_pcraft_writer_free(w);

        /* Directory bundle: the first save's stats and layout, then the incremental save. */
        for (size_t i = 0; i < g_nlines; i++) {
            if (strcmp(g_lines[i].f[0], "dir") == 0 && strcmp(g_lines[i].f[1], b->name) == 0) {
                first_dir_stats = g_lines[i].f[2];
            }
        }
        (void)snprintf(dir, sizeof dir, "%s/%s.dir", tmp, b->name);
        CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
        CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, dir, &st, NULL) == PC_OK);
        stats_text(&st, text, sizeof text);
        CHECK_EQ_STR(text, first_dir_stats);
        t_listing(dir, &listing);
        CHECK_EQ_STR(pc_buf_cstr(&listing), b->dir);
        CHECK(pc_pcraft_writer_save_dir_prepared(w, &p, dir, &st, NULL) == PC_OK);
        stats_text(&st, text, sizeof text);
        CHECK_EQ_STR(text, b->dirstats2);
        pc_pcraft_writer_free(w);

        if (xdir != NULL) {
            char path[1100];
            pc_pcraft_writer *wx = NULL;
            (void)snprintf(path, sizeof path, "%s/%s.pcraft", xdir, b->name);
            CHECK(t_write_file(path, out.data, out.len) == 0);
            (void)snprintf(path, sizeof path, "%s/%s.dir", xdir, b->name);
            CHECK(pc_pcraft_writer_new(&wx, NULL) == PC_OK);
            CHECK(pc_pcraft_writer_save_dir_prepared(wx, &p, path, NULL, NULL) == PC_OK);
            pc_pcraft_writer_free(wx);
        }
        pc_format_prepared_fini(&p);
        pc_buf_fini(&out);
        pc_buf_fini(&out2);
        pc_buf_fini(&listing);
        g_checked[K_BUNDLE]++;
    }
    t_rm_rf(tmp);
}

/* The rustref `rebuild`: rewrite a bundle, replacing or dropping entries. */
typedef enum { M_SWAP, M_TRUNC_HALF, M_GARBAGE, M_EMPTY, M_REMOVE, M_FLIP, M_FOREIGN } mkind;

static void rebuild(const pc_buf *in, const char *target, mkind k, const pc_buf *other, size_t pos,
                    pc_buf *out)
{
    pc_zip_reader r;
    pc_zip_writer w;
    pc_zip_writer_init(&w);
    CHECK(pc_zip_reader_open(&r, in->data, in->len, NULL) == PC_OK);
    for (size_t i = 0; i < r.count; i++) {
        pc_buf d = PC_BUF_INIT;
        const char *n = r.entries[i].name.ptr;
        CHECK(pc_zip_read(&r, &r.entries[i], (size_t)1 << 30, &d, NULL) == PC_OK);
        if (strcmp(n, target) == 0) {
            switch (k) {
            case M_SWAP:
            case M_FOREIGN:
                pc_buf_truncate(&d, 0);
                (void)pc_buf_append(&d, other->data, other->len, NULL);
                break;
            case M_TRUNC_HALF:
                pc_buf_truncate(&d, d.len / 2);
                break;
            case M_GARBAGE:
                pc_buf_truncate(&d, 0);
                (void)pc_buf_append_str(&d, "garbage", NULL);
                break;
            case M_EMPTY:
                pc_buf_truncate(&d, 0);
                break;
            case M_REMOVE:
                pc_buf_fini(&d);
                continue;
            case M_FLIP:
                d.data[pos] ^= 0x5a;
                break;
            }
        }
        CHECK(pc_zip_writer_add(&w, n, strlen(n), d.data, d.len, NULL) == PC_OK);
        pc_buf_fini(&d);
    }
    CHECK(pc_zip_writer_finish(&w, out, NULL) == PC_OK);
    pc_zip_writer_fini(&w);
    pc_zip_reader_close(&r);
}

static void entry_of(const pc_buf *bundle_bytes, const char *name, pc_buf *out)
{
    pc_zip_reader r;
    CHECK(pc_zip_reader_open(&r, bundle_bytes->data, bundle_bytes->len, NULL) == PC_OK);
    CHECK(pc_zip_read_by_name(&r, name, (size_t)1 << 30, out, NULL) == PC_OK);
    pc_zip_reader_close(&r);
}

/* The reference's decoder (ruzstd) and libzstd notice damage at different points; these three
 * outcomes are one kind (§7). */
static bool payload_error(const char *m)
{
    return strstr(m, ": zstd: ") != NULL || strstr(m, ": expected ") != NULL ||
           strstr(m, ": content does not match its hash") != NULL;
}

static size_t g_exact;
static size_t g_kind_only;

/* Both are manifest JSON syntax errors at the same line and column. */
static bool json_syntax_same_place(const char *got, const char *want)
{
    static const char P[] = "E\"manifest JSON: ";
    const char *ga = strstr(got, " at line ");
    const char *wa = strstr(want, " at line ");
    return strncmp(got, P, sizeof P - 1) == 0 && strncmp(want, P, sizeof P - 1) == 0 && ga != NULL &&
           wa != NULL && strcmp(ga, wa) == 0;
}

static void compare_err(const char *what, pc_status st, const pc_error *e, const char *want_dq)
{
    pc_buf got = PC_BUF_INIT;
    if (st == PC_OK) {
        (void)pc_buf_append_str(&got, "ok", NULL);
    } else {
        (void)pc_buf_append_str(&got, "E", NULL);
        dq(&got, e->msg, strlen(e->msg));
    }
    if (strcmp(pc_buf_cstr(&got), want_dq) == 0) {
        g_exact++;
    } else if (st != PC_OK && json_syntax_same_place(pc_buf_cstr(&got), want_dq)) {
        /* base's JSON parser words some syntax errors differently (REWRITE_PLAN.md B2). */
        g_kind_only++;
    } else if (st != PC_OK && payload_error(e->msg) && payload_error(want_dq)) {
        /* Same object, same kind (corrupt payload). */
        const char *colon = strstr(e->msg, ".zst: ");
        CHECK(colon != NULL && strstr(want_dq, ".zst: ") != NULL &&
              strncmp(want_dq + 1 + 1, e->msg, (size_t)(colon - e->msg)) == 0);
        CHECK(pc_format_error_kind(st, e) == PC_FORMAT_ERR_CORRUPT);
        g_kind_only++;
    } else {
        PC_T_FAIL("%s:\n  got  %s\n  want %s", what, pc_buf_cstr(&got), want_dq);
    }
    pc_buf_fini(&got);
}

static void test_damaged(void)
{
    bundle *rgb = find_bundle("rgb8");
    pc_format_load_opts o = pc_format_load_opts_default();
    if (rgb == NULL) {
        PC_T_FAIL("no rgb8 bundle");
        return;
    }
    for (size_t i = 0; i < g_nlines; i++) {
        line *l = &g_lines[i];
        if (strcmp(l->f[0], "xload") == 0) {
            char kind[16];
            char target[128];
            char a3[128] = "";
            char a4[128] = "";
            mkind k;
            size_t pos = 0;
            pc_buf other = PC_BUF_INIT;
            pc_buf damaged = PC_BUF_INIT;
            pc_format_source *src = NULL;
            pc_format_fetch *f = NULL;
            pc_error e;
            pc_status st;
            char hash[65];
            if (sscanf(l->f[2], "%15s %127s %127s %127s", kind, target, a3, a4) < 2) {
                PC_T_FAIL("bad case %s", l->f[2]);
                continue;
            }
            if (strcmp(kind, "swap") == 0) {
                k = M_SWAP;
                entry_of(&rgb->bytes, a3, &other);
            } else if (strcmp(kind, "trunc-half") == 0) {
                k = M_TRUNC_HALF;
            } else if (strcmp(kind, "garbage") == 0) {
                k = M_GARBAGE;
            } else if (strcmp(kind, "empty") == 0) {
                k = M_EMPTY;
            } else if (strcmp(kind, "remove") == 0) {
                k = M_REMOVE;
            } else if (strcmp(kind, "flip") == 0) {
                k = M_FLIP;
                pos = strtoull(a3, NULL, 10);
            } else {
                bundle *fb = find_bundle(a3);
                k = M_FOREIGN;
                CHECK(fb != NULL);
                if (fb != NULL) {
                    entry_of(&fb->bytes, a4, &other);
                }
            }
            rebuild(&rgb->bytes, target, k, &other, pos, &damaged);
            CHECK(pc_format_source_zip(damaged.data, damaged.len, &src, NULL) == PC_OK);
            CHECK(pc_format_fetch_new(src, &o, &f, NULL) == PC_OK);
            memcpy(hash, target + 6, 64);
            hash[64] = '\0';
            if (strcmp(l->f[3], "tile") == 0) {
                pc_buf d = PC_BUF_INIT;
                st = pc_format_fetch_tile(f, hash, strtoull(l->f[4], NULL, 10), &d, &e);
                pc_buf_fini(&d);
            } else {
                pc_blob *bl = NULL;
                st = pc_format_fetch_blob(f, hash, &bl, &e);
                pc_blob_release(bl);
            }
            compare_err(l->f[2], st, &e, l->f[5]);
            pc_format_fetch_free(f);
            pc_format_source_free(src);
            pc_buf_fini(&other);
            pc_buf_fini(&damaged);
            g_checked[K_XLOAD]++;
        } else if (strcmp(l->f[0], "xlimit") == 0) {
            pc_format_source *src = NULL;
            pc_format_load_opts lo = pc_format_load_opts_default();
            uint64_t v = strtoull(l->f[2], NULL, 10);
            pc_zip_reader r;
            pc_error e;
            pc_status st = PC_OK;
            CHECK(pc_format_source_zip(rgb->bytes.data, rgb->bytes.len, &src, NULL) == PC_OK);
            CHECK(pc_zip_reader_open(&r, rgb->bytes.data, rgb->bytes.len, NULL) == PC_OK);
            if (strcmp(l->f[1], "manifest") == 0) {
                pc_buf m = PC_BUF_INIT;
                st = pc_format_read_manifest_raw(src, (size_t)v, &m, &e);
                pc_buf_fini(&m);
                compare_err("xlimit manifest", st, &e, l->f[3]);
            } else {
                /* The load fails at whichever object crosses the limit first; fetching every
                 * object must produce the reference's error among the failures. */
                pc_format_fetch *f = NULL;
                bool seen = false;
                if (strcmp(l->f[1], "total") == 0) {
                    lo.max_total_bytes = v;
                } else {
                    lo.max_blob_bytes = (size_t)v;
                }
                CHECK(pc_format_fetch_new(src, &lo, &f, NULL) == PC_OK);
                for (size_t j = 0; j < r.count && !seen; j++) {
                    const char *n = r.entries[j].name.ptr;
                    char hash[65];
                    pc_buf got = PC_BUF_INIT;
                    if (strlen(n) != 74) {
                        continue;
                    }
                    memcpy(hash, n + 6, 64);
                    hash[64] = '\0';
                    if (n[0] == 't') {
                        pc_buf d = PC_BUF_INIT;
                        st = pc_format_fetch_tile(f, hash, tile_len("rgb8", n), &d, &e);
                        pc_buf_fini(&d);
                    } else {
                        pc_blob *bl = NULL;
                        st = pc_format_fetch_blob(f, hash, &bl, &e);
                        pc_blob_release(bl);
                    }
                    if (st != PC_OK) {
                        (void)pc_buf_append_str(&got, "E", NULL);
                        dq(&got, e.msg, strlen(e.msg));
                        seen = strcmp(pc_buf_cstr(&got), l->f[3]) == 0;
                        CHECK(pc_format_error_kind(st, &e) == PC_FORMAT_ERR_LIMIT);
                    }
                    pc_buf_fini(&got);
                }
                if (!seen) {
                    PC_T_FAIL("xlimit %s %s: the reference's %s never occurred", l->f[1], l->f[2], l->f[3]);
                }
                pc_format_fetch_free(f);
            }
            pc_zip_reader_close(&r);
            pc_format_source_free(src);
            g_checked[K_XLIMIT]++;
        }
    }
}

static void depth_case(const char *c, pc_buf *m)
{
    size_t n = 0;
    pc_buf_truncate(m, 0);
    if (sscanf(c, "nested-%zu", &n) == 1) {
        for (size_t i = 0; i < n; i++) {
            (void)pc_buf_append_byte(m, '[', NULL);
        }
        for (size_t i = 0; i < n; i++) {
            (void)pc_buf_append_byte(m, ']', NULL);
        }
    } else if (strcmp(c, "open-368") == 0) {
        for (int i = 0; i < 368; i++) {
            (void)pc_buf_append_str(m, "{\"a\":", NULL);
        }
    } else if (strcmp(c, "unbalanced-closers") == 0) {
        for (int i = 0; i < 1000; i++) {
            (void)pc_buf_append_byte(m, ']', NULL);
        }
        for (int i = 0; i < 367; i++) {
            (void)pc_buf_append_byte(m, '[', NULL);
        }
    } else if (strcmp(c, "strings") == 0) {
        (void)pc_buf_append_str(m, "{\"x\":\"", NULL);
        for (int i = 0; i < 100000; i++) {
            (void)pc_buf_append_byte(m, '[', NULL);
        }
        (void)pc_buf_append_str(m, "\\\"", NULL);
        for (int i = 0; i < 10; i++) {
            (void)pc_buf_append_byte(m, '{', NULL);
        }
        (void)pc_buf_append_str(m, "\"}", NULL);
    } else if (strcmp(c, "escaped-backslash") == 0) {
        (void)pc_buf_append_str(m, "{\"x\":\"\\\\\"", NULL);
        for (int i = 0; i < 400; i++) {
            (void)pc_buf_append_byte(m, '[', NULL);
        }
    } else if (strcmp(c, "not-json") == 0) {
        (void)pc_buf_append_str(m, "{not json", NULL);
    }
}

static void test_depth(void)
{
    bundle *rgb = find_bundle("rgb8");
    pc_buf m = PC_BUF_INIT;
    for (size_t i = 0; rgb != NULL && i < g_nlines; i++) {
        line *l = &g_lines[i];
        pc_error e;
        pc_status st;
        if (strcmp(l->f[0], "xdepth") != 0) {
            continue;
        }
        g_checked[K_XDEPTH]++;
        if (strcmp(l->f[1], "missing") == 0) {
            pc_buf rebuilt = PC_BUF_INIT;
            pc_buf raw = PC_BUF_INIT;
            pc_format_source *src = NULL;
            rebuild(&rgb->bytes, PC_FORMAT_MANIFEST, M_REMOVE, NULL, 0, &rebuilt);
            CHECK(pc_format_source_zip(rebuilt.data, rebuilt.len, &src, NULL) == PC_OK);
            st = pc_format_read_manifest_raw(src, (size_t)256 << 20, &raw, &e);
            compare_err("xdepth missing", st, &e, l->f[2]);
            pc_format_source_free(src);
            pc_buf_fini(&raw);
            pc_buf_fini(&rebuilt);
            continue;
        }
        depth_case(l->f[1], &m);
        st = pc_format_check_manifest_depth(m.data, m.len, &e);
        if (strstr(l->f[2], "manifest JSON nested deeper") != NULL) {
            compare_err(l->f[1], st, &e, l->f[2]);
        } else {
            /* The depth check passes; the manifest parse reports the rest. */
            pc_format_manifest *mf = NULL;
            CHECK(st == PC_OK);
            st = pc_format_manifest_parse(m.data, m.len, &mf, &e);
            compare_err(l->f[1], st, &e, l->f[2]);
            pc_format_manifest_free(mf);
        }
    }
    pc_buf_fini(&m);
}

/* ---- document level (the doc-* lines): format_vectors.rs docs() built in C ---- */

/* The ids the reference's process gave the document and its layers, from its manifest. */
static void ids_from(const bundle *bd, pc_document *d)
{
    pc_zip_reader r;
    pc_buf m = PC_BUF_INIT;
    yyjson_doc *j = NULL;
    if (pc_zip_reader_open(&r, bd->bytes.data, bd->bytes.len, NULL) != PC_OK) {
        PC_T_FAIL("%s: unreadable", bd->name);
        return;
    }
    CHECK(pc_zip_read_by_name(&r, PC_FORMAT_MANIFEST, (size_t)1 << 30, &m, NULL) == PC_OK);
    pc_zip_reader_close(&r);
    if (pc_json_parse((const char *)m.data, m.len, &j, NULL) == PC_OK) {
        yyjson_val *doc = yyjson_obj_get(yyjson_doc_get_root(j), "document");
        yyjson_val *layers = yyjson_obj_get(doc, "layers");
        d->id = yyjson_get_uint(yyjson_obj_get(doc, "id"));
        for (size_t i = 0; i < d->nlayers && i < yyjson_arr_size(layers); i++) {
            d->layers[i].id = yyjson_get_uint(yyjson_obj_get(yyjson_arr_get(layers, i), "id"));
        }
    }
    yyjson_doc_free(j);
    pc_buf_fini(&m);
}

static void push(pc_document *d, pc_layer *l)
{
    pc_layer *g = realloc(d->layers, (d->nlayers + 1) * sizeof *g);
    if (g == NULL) {
        pc_layer_fini(l);
        return;
    }
    d->layers = g;
    d->layers[d->nlayers++] = *l;
}

static void raster(pc_document *d, const char *name, uint64_t seed, bool hdr, bool masked, bool block)
{
    pc_layer l;
    CHECK(pc_layer_raster(&l, name, pc_document_pixel_format(d), NULL) == PC_OK);
    t_scribble(pc_layer_surface_mut(&l), seed, hdr);
    if (masked) {
        pc_layer_mask_hide_all(&l.mask);
        l.has_mask = true;
        t_scribble(&l.mask.surface, 2, false);
    }
    if (block && (l.psd_blocks = calloc(1, sizeof *l.psd_blocks)) != NULL) {
        memcpy(l.psd_blocks[0].key, "lnsr", 4);
        l.psd_blocks[0].data = t_blob(5, 4);
        l.npsd_blocks = 1;
    }
    push(d, &l);
}

static void c4_doc(const char *name, pc_document *d)
{
    pc_size s1 = {300, 280};
    pc_size s2 = {260, 10};
    pc_size s3 = {64, 64};
    pc_size s4 = {1, 1};
    memset(d, 0, sizeof *d);
    if (strcmp(name, "rgb8") == 0) {
        CHECK(pc_document_with_background(d, "Rgb8", s1, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, PC_COLOR_WHITE,
                                          NULL) == PC_OK);
        d->icc_profile = t_blob(1, 500);
        raster(d, "Paint", 1, false, true, true);
        raster(d, "Twin", 1, false, false, true);
    } else if (strcmp(name, "gray16") == 0) {
        CHECK(pc_document_with_background(d, "Gray16", s2, PC_COLOR_MODE_GRAYSCALE, PC_SAMPLE_TYPE_U16,
                                          PC_COLOR_BLACK, NULL) == PC_OK);
        raster(d, "G", 7, false, false, false);
    } else if (strcmp(name, "rgbf32") == 0) {
        CHECK(pc_document_init(d, "Float", s3, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_F32, NULL) == PC_OK);
        raster(d, "Hdr", 9, true, false, false);
        d->icc_profile = t_blob(2, 3000);
    } else {
        CHECK(pc_document_init(d, "Empty", s4, PC_COLOR_MODE_RGB, PC_SAMPLE_TYPE_U8, NULL) == PC_OK);
    }
}

static void check_doc_line(const char *kind, const char *name, const char *got)
{
    for (size_t i = 0; i < g_nlines; i++) {
        const line *l = &g_lines[i];
        if (l->n >= 3 && strcmp(l->f[0], kind) == 0 && strcmp(l->f[1], name) == 0) {
            CHECK_EQ_STR(got, l->f[2]);
            g_checked[K_DOC]++;
            return;
        }
    }
    PC_T_FAIL("no %s line for %s", kind, name);
}

/* The reference's documents saved by the C writer: the same manifest bytes and previews as its
 * bundle, the same incremental statistics (zip and directory), and loads that give the
 * document back. */
static void test_documents(void)
{
    pc_rgba8_image thumb;
    pc_rgba8_image comp;
    static const uint8_t COMP[16] = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 9, 9, 9, 9};
    char tmp[1024];
    CHECK(pc_rgba8_image_init(&thumb, 4, 3, NULL) == PC_OK);
    CHECK(pc_rgba8_image_init(&comp, 2, 2, NULL) == PC_OK);
    for (size_t i = 0; thumb.pixels != NULL && i < 48; i++) {
        thumb.pixels[i] = (uint8_t)(i * 5);
    }
    if (comp.pixels != NULL) {
        memcpy(comp.pixels, COMP, sizeof COMP);
    }
    CHECK(t_mkdtemp("parity-doc", tmp, sizeof tmp) == 0);
    for (size_t k = 0; k < g_nb; k++) {
        bundle *b = &g_b[k];
        pc_format_save_opts with = {&thumb, &comp};
        pc_format_save_opts none = {NULL, NULL};
        const pc_format_save_opts *opts = strcmp(b->name, "rgb8") == 0 ? &with : &none;
        pc_document d;
        pc_document back;
        pc_pcraft_writer *w = NULL;
        pc_buf out = PC_BUF_INIT;
        pc_format_save_stats st;
        char text[256];
        char dir[1100];
        c4_doc(b->name, &d);
        ids_from(b, &d);
        CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
        CHECK(pc_pcraft_writer_save_zip(w, &d, opts, &out, &st, NULL) == PC_OK);
        stats_text(&st, text, sizeof text);
        CHECK_EQ_STR(text, b->stats);
        compare_bundles_ex(b->name, b->bytes.data, b->bytes.len, out.data, out.len, true);
        CHECK(pc_pcraft_writer_save_zip(w, &d, opts, &out, &st, NULL) == PC_OK);
        stats_text(&st, text, sizeof text);
        check_doc_line("doc-stats2", b->name, text);
        pc_pcraft_writer_free(w);
        check_doc_line("doc-load", b->name,
                       pc_format_load_from_bytes(b->bytes.data, b->bytes.len, &back, NULL) == PC_OK &&
                               pc_document_eq(&back, &d)
                           ? "equal"
                           : "DIFFERENT");
        pc_document_fini(&back);
        (void)snprintf(dir, sizeof dir, "%s/%s.dir", tmp, b->name);
        CHECK(pc_pcraft_writer_new(&w, NULL) == PC_OK);
        CHECK(pc_pcraft_writer_save_dir(w, &d, dir, opts, &st, NULL) == PC_OK);
        CHECK(pc_pcraft_writer_save_dir(w, &d, dir, opts, &st, NULL) == PC_OK);
        stats_text(&st, text, sizeof text);
        check_doc_line("doc-dirstats2", b->name, text);
        pc_pcraft_writer_free(w);
        check_doc_line("doc-loaddir", b->name,
                       pc_format_load_path(dir, &back, NULL) == PC_OK && pc_document_eq(&back, &d)
                           ? "equal"
                           : "DIFFERENT");
        pc_document_fini(&back);
        pc_document_fini(&d);
        pc_buf_fini(&out);
    }
    pc_rgba8_image_fini(&thumb);
    pc_rgba8_image_fini(&comp);
    t_rm_rf(tmp);
}

static void count_rest(void)
{
    size_t total = 0;
    for (int k = 0; k <= K_DOC; k++) {
        total += g_checked[k];
    }
    (void)printf(
        "golden lines: %zu (crc %zu, zipw %zu, zipr %zu, trunc %zu, mut %zu, ispc %zu, bufferfor %zu, "
        "tmpname %zu, bundles %zu, xload %zu, xlimit %zu, xdepth %zu, doc %zu)\n"
        "errors: %zu exact, %zu same kind (zstd decoder or JSON parser wording); sweep lines equal up to the "
        "deflate decoder: %zu\n",
        g_nlines, g_checked[K_CRC], g_checked[K_ZIPW], g_checked[K_ZIPR], g_checked[K_TRUNC],
        g_checked[K_MUT], g_checked[K_ISPC], g_checked[K_BUF], g_checked[K_TMP], g_checked[K_BUNDLE],
        g_checked[K_XLOAD], g_checked[K_XLIMIT], g_checked[K_XDEPTH], g_checked[K_DOC], g_exact, g_kind_only,
        g_deflate_kind);
    CHECK(total > 1300);
    CHECK_EQ_UINT(g_checked[K_ZIPR], 51);
}

int main(int argc, char **argv)
{
    if (load_golden() != 0) {
        (void)fprintf(stderr, "cannot read %s\n", GOLDEN);
        return 1;
    }
    RUN(test_crc_and_writer);
    RUN(test_reader);
    RUN(test_read_and_tmpname);
    RUN(test_bundles);
    RUN(test_damaged);
    RUN(test_depth);
    RUN(test_documents);
    RUN(count_rest);
    for (size_t i = 0; i < g_nb; i++) {
        pc_buf_fini(&g_b[i].bytes);
    }
    free(g_lines);
    free(g_text);
    return pc_t_finish();
}
