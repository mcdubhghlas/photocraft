/* Corpus oracle: every tests/golden/psd-info/<path>.json (recorded from the Rust reference with
 * `tools/golden.sh psd-info`) must equal the C parser's view of corpus/<path>, byte for byte,
 * including error messages. Skips cleanly when corpus/ has not been fetched (`make corpus`).
 * Also checks that every parsed file writes back byte-identically. */
#define _XOPEN_SOURCE 700
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "harness.h"
#include "psd/file.h"

#define GOLDEN "tests/golden/psd-info"

static void jstr(pc_buf *o, const uint8_t *s, size_t n)
{
    size_t i;
    (void)pc_buf_append_byte(o, '"', NULL);
    for (i = 0; i < n; i++) {
        uint8_t c = s[i];
        switch (c) {
        case '"':
            (void)pc_buf_append_str(o, "\\\"", NULL);
            break;
        case '\\':
            (void)pc_buf_append_str(o, "\\\\", NULL);
            break;
        case '\b':
            (void)pc_buf_append_str(o, "\\b", NULL);
            break;
        case '\f':
            (void)pc_buf_append_str(o, "\\f", NULL);
            break;
        case '\n':
            (void)pc_buf_append_str(o, "\\n", NULL);
            break;
        case '\r':
            (void)pc_buf_append_str(o, "\\r", NULL);
            break;
        case '\t':
            (void)pc_buf_append_str(o, "\\t", NULL);
            break;
        default:
            if (c < 0x20) {
                (void)pc_buf_appendf(o, NULL, "\\u%04x", (unsigned)c);
            } else {
                (void)pc_buf_append_byte(o, c, NULL);
            }
        }
    }
    (void)pc_buf_append_byte(o, '"', NULL);
}

/* String::from_utf8_lossy then JSON-escaped. */
static void jlossy(pc_buf *o, const uint8_t *s, size_t n)
{
    pc_buf t = PC_BUF_INIT;
    (void)pc_utf8_lossy(s, n, &t, NULL);
    jstr(o, t.data != NULL ? t.data : (const uint8_t *)"", t.len);
    pc_buf_fini(&t);
}

static void jhex(pc_buf *o, const pc_psd_bytes *b)
{
    size_t i;
    (void)pc_buf_append_byte(o, '"', NULL);
    for (i = 0; i < b->len; i++) {
        (void)pc_buf_appendf(o, NULL, "%02x", (unsigned)b->data[i]);
    }
    (void)pc_buf_append_byte(o, '"', NULL);
}

static void jblocks(pc_buf *o, const pc_psd_block *b, size_t n)
{
    size_t i;
    (void)pc_buf_append_byte(o, '[', NULL);
    for (i = 0; i < n; i++) {
        (void)pc_buf_append_str(o, i ? ",{\"key\":" : "{\"key\":", NULL);
        jlossy(o, b[i].key, 4);
        (void)pc_buf_appendf(o, NULL, ",\"len\":%zu,\"sig\":", b[i].data.len);
        jlossy(o, b[i].signature, 4);
        (void)pc_buf_append_byte(o, '}', NULL);
    }
    (void)pc_buf_append_byte(o, ']', NULL);
}

/* rustref psd-info, serde_json style (keys sorted). */
static void info_json(const pc_psd_file *f, pc_buf *o)
{
    const pc_psd_header *h = &f->header;
    size_t i;
    size_t k;
    size_t nl;
    const pc_psd_layer *layers = pc_psd_file_layers(f, &nl);
    (void)pc_buf_appendf(o, NULL, "{\"channels\":%u,\"color_mode\":%u,\"color_mode_data_len\":%zu,\"depth\":%u,",
                         (unsigned)h->channels, (unsigned)h->color_mode, f->color_mode_data.len, (unsigned)h->depth);
    (void)pc_buf_append_str(o, "\"global_blocks\":", NULL);
    jblocks(o, f->global_blocks, f->nglobal);
    (void)pc_buf_appendf(o, NULL, ",\"has_layer_info\":%s,\"height\":%u,\"image_compression\":%u,\"image_len\":%zu,",
                         f->has_layer_info ? "true" : "false", (unsigned)h->height, (unsigned)f->image_compression,
                         f->image_data.len);
    (void)pc_buf_append_str(o, "\"layers\":[", NULL);
    for (i = 0; i < nl; i++) {
        const pc_psd_layer *l = &layers[i];
        (void)pc_buf_append_str(o, i ? ",{\"blend\":" : "{\"blend\":", NULL);
        jlossy(o, l->blend, 4);
        (void)pc_buf_append_str(o, ",\"blocks\":", NULL);
        jblocks(o, l->blocks, l->nblocks);
        (void)pc_buf_append_str(o, ",\"channels\":[", NULL);
        for (k = 0; k < l->nchannels; k++) {
            const pc_psd_channel *c = &l->channels[k];
            if (c->has_compression) {
                (void)pc_buf_appendf(o, NULL, "%s{\"compression\":%u,\"id\":%d,\"len\":%zu}", k ? "," : "",
                                     (unsigned)c->compression, (int)c->id, c->data.len);
            } else {
                (void)pc_buf_appendf(o, NULL, "%s{\"compression\":null,\"id\":%d,\"len\":%zu}", k ? "," : "", (int)c->id,
                                     c->data.len);
            }
        }
        (void)pc_buf_appendf(o, NULL, "],\"clipping\":%u,\"flags\":%u,\"name\":", (unsigned)l->clipping,
                             (unsigned)l->flags);
        jlossy(o, l->name.data, l->name.len);
        (void)pc_buf_append_str(o, ",\"name_hex\":", NULL);
        jhex(o, &l->name);
        (void)pc_buf_appendf(o, NULL, ",\"opacity\":%u,\"rect\":[%d,%d,%d,%d]}", (unsigned)l->opacity, (int)l->rect.top,
                             (int)l->rect.left, (int)l->rect.bottom, (int)l->rect.right);
    }
    (void)pc_buf_append_str(o, "],\"merged_alpha\":", NULL);
    (void)pc_buf_append_str(o, f->has_layer_info ? (f->layer_info.merged_alpha ? "true" : "false") : "null", NULL);
    (void)pc_buf_append_str(o, ",\"resources\":[", NULL);
    for (i = 0; i < f->nresources; i++) {
        const pc_psd_resource *r = &f->resources[i];
        (void)pc_buf_appendf(o, NULL, "%s{\"id\":%u,\"len\":%zu,\"name_hex\":", i ? "," : "", (unsigned)r->id, r->data.len);
        jhex(o, &r->name);
        (void)pc_buf_append_str(o, ",\"sig\":", NULL);
        jlossy(o, r->signature, 4);
        (void)pc_buf_append_byte(o, '}', NULL);
    }
    (void)pc_buf_appendf(o, NULL, "],\"version\":%d,\"width\":%u}", h->version == PC_PSD_V2 ? 2 : 1, (unsigned)h->width);
}

static int files;
static int matched;
static int roundtrips;
static int missing;

static void check_one(const char *golden, const char *rel)
{
    char path[4200];
    pc_buf want = PC_BUF_INIT;
    pc_buf data = PC_BUF_INIT;
    pc_buf got = PC_BUF_INIT;
    pc_psd_file f;
    pc_error e;
    size_t wl;
    (void)snprintf(path, sizeof path, "corpus/%s", rel);
    if (pc_file_read(golden, (size_t)64 << 20, &want, NULL) != PC_OK ||
        pc_file_read(path, (size_t)1 << 32, &data, NULL) != PC_OK) {
        missing++;
        goto done;
    }
    files++;
    wl = want.len;
    while (wl > 0 && (want.data[wl - 1] == '\n' || want.data[wl - 1] == '\r')) {
        wl--;
    }
    if (pc_psd_file_parse(data.data, data.len, &f, &e) == PC_OK) {
        pc_psd_bytes back = {NULL, 0};
        info_json(&f, &got);
        if (pc_psd_file_to_bytes(&f, &back, &e) == PC_OK && back.len == data.len &&
            memcmp(back.data, data.data, data.len) == 0) {
            roundtrips++;
        } else {
            pc_t_name = rel;
            PC_T_FAIL("round trip differs (%zu vs %zu bytes)", back.len, data.len);
        }
        pc_psd_bytes_fini(&back);
        pc_psd_file_fini(&f);
    } else {
        (void)pc_buf_append_str(&got, "{\"error\":", NULL);
        jstr(&got, (const uint8_t *)e.msg, strlen(e.msg));
        (void)pc_buf_append_byte(&got, '}', NULL);
    }
    if (got.len == wl && memcmp(got.data, want.data, wl) == 0) {
        matched++;
    } else {
        size_t d = 0;
        while (d < got.len && d < wl && got.data[d] == want.data[d]) {
            d++;
        }
        pc_t_name = rel;
        PC_T_FAIL("differs at byte %zu:\n  want ...%.120s\n  got  ...%.120s", d, (const char *)want.data + (d > 40 ? d - 40 : 0),
                  pc_buf_cstr(&got) + (d > 40 ? d - 40 : 0));
    }
done:
    pc_buf_fini(&want);
    pc_buf_fini(&data);
    pc_buf_fini(&got);
}

static void walk(const char *dir, const char *rel)
{
    DIR *d = opendir(dir);
    struct dirent *ent;
    if (d == NULL) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        char p[4096];
        char r[4096];
        struct stat sb;
        size_t n = strlen(ent->d_name);
        if (ent->d_name[0] == '.') {
            continue;
        }
        (void)snprintf(p, sizeof p, "%s/%s", dir, ent->d_name);
        (void)snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", ent->d_name);
        if (stat(p, &sb) != 0) {
            continue;
        }
        if (S_ISDIR(sb.st_mode)) {
            walk(p, r);
        } else if (n > 5 && strcmp(ent->d_name + n - 5, ".json") == 0) {
            r[strlen(r) - 5] = '\0';
            check_one(p, r);
        }
    }
    closedir(d);
}

static void test_psd_info_goldens(void)
{
    struct stat sb;
    if (stat("corpus", &sb) != 0) {
        (void)printf("(corpus/ absent: run `make corpus`; skipped) ");
        return;
    }
    walk(GOLDEN, "");
    (void)printf("(%d files: %d match, %d round-trip, %d missing) ", files, matched, roundtrips, missing);
    CHECK(files > 700);
}

int main(int argc, char **argv)
{
    RUN(test_psd_info_goldens);
    return pc_t_finish();
}
