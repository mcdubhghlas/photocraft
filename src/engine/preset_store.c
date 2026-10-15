/* Persistent brush-preset store (port of crates/engine/src/preset_store.rs, layer L5). See
 * preset_store.h for the format. This file owns the pc_engine_preset_store type and the real
 * sync_preset_store hook (relocated here out of hooks.c per the HOOK SEAM contract).
 *
 * Never crash: every group, tip and index file is hostile disk data. Sizes are bounded before
 * allocating, corrupt files are skipped with a warning and left on disk, and file names are
 * validated so a backend never leaves its directory.
 *
 * Actions (actions.json) share the directory in the reference; they need E12 actions, which the C
 * session does not have, so that half is a documented stub: open() never reads actions and sync
 * never writes them. */
#include "preset_store.h"

#include <blake3.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <yyjson.h>

#include "base/compress.h"
#include "base/file.h"
#include "base/hash.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "engine_internal.h"
#include "paint/json.h"

#define GROUP_FORMAT "photocraft-brush-group"
static const uint8_t TIP_MAGIC[6] = {'P', 'C', 'T', 'I', 'P', '1'};
#define TIP_HEADER 15u /* 6 magic + 1 bits + 4 width + 4 height */
/* Deepest nested folder a loaded preset may claim (psd::abr::MAX_FOLDER_DEPTH). */
#define PRESET_MAX_FOLDER_DEPTH 32u

/* ============================================================ owned string vectors */

void pc_preset_strs_fini(pc_preset_strs *s)
{
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->len; i++) {
        free(s->v[i]);
    }
    free(s->v);
    s->v = NULL;
    s->len = 0;
    s->cap = 0;
}

/* Push a copy of `n` bytes of `str` (NUL-terminated copy). Returns false on OOM (s unchanged). */
static bool strs_push_n(pc_preset_strs *s, const char *str, size_t n)
{
    void *grown = pc_grow_array(s->v, &s->cap, s->len + 1, sizeof *s->v);
    if (grown == NULL) {
        return false;
    }
    s->v = grown;
    char *dup = pc_strndup(str, n);
    if (dup == NULL) {
        return false;
    }
    s->v[s->len++] = dup;
    return true;
}

static bool strs_push(pc_preset_strs *s, const char *str)
{
    return strs_push_n(s, str, strlen(str));
}

static bool strs_contains(const pc_preset_strs *s, const char *str)
{
    for (size_t i = 0; i < s->len; i++) {
        if (strcmp(s->v[i], str) == 0) {
            return true;
        }
    }
    return false;
}

/* Index of `str` in `s`, or SIZE_MAX. */
static size_t strs_find(const pc_preset_strs *s, const char *str)
{
    for (size_t i = 0; i < s->len; i++) {
        if (strcmp(s->v[i], str) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool strs_eq(const pc_preset_strs *a, const pc_preset_strs *b)
{
    if (a->len != b->len) {
        return false;
    }
    for (size_t i = 0; i < a->len; i++) {
        if (strcmp(a->v[i], b->v[i]) != 0) {
            return false;
        }
    }
    return true;
}

static bool strs_clone(const pc_preset_strs *src, pc_preset_strs *dst)
{
    memset(dst, 0, sizeof *dst);
    for (size_t i = 0; i < src->len; i++) {
        if (!strs_push(dst, src->v[i])) {
            pc_preset_strs_fini(dst);
            return false;
        }
    }
    return true;
}

/* ============================================================ file-name validation */

bool pc_preset_valid_name(const char *name)
{
    const char *leaf = name;
    if (strncmp(name, PC_PRESET_TIPS_DIR "/", 5) == 0) {
        leaf = name + 5;
    }
    size_t n = strlen(leaf);
    if (n == 0 || n > 128 || leaf[0] == '.') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)leaf[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '_' ||
                  c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* ============================================================ tip codec and hashing */

static const uint16_t *tile_samples(const pc_paint_gray_tile *t, size_t *n)
{
    *n = pc_paint_gray_tile_len(t);
    return pc_paint_gray_tile_samples(t);
}

void pc_preset_tip_hash(const pc_paint_gray_tile *t, char out[33])
{
    blake3_hasher h;
    blake3_hasher_init(&h);
    uint8_t dim[4];
    dim[0] = (uint8_t)(t->width);
    dim[1] = (uint8_t)(t->width >> 8);
    dim[2] = (uint8_t)(t->width >> 16);
    dim[3] = (uint8_t)(t->width >> 24);
    blake3_hasher_update(&h, dim, 4);
    dim[0] = (uint8_t)(t->height);
    dim[1] = (uint8_t)(t->height >> 8);
    dim[2] = (uint8_t)(t->height >> 16);
    dim[3] = (uint8_t)(t->height >> 24);
    blake3_hasher_update(&h, dim, 4);
    size_t n = 0;
    const uint16_t *s = tile_samples(t, &n);
    uint8_t buf[8192];
    for (size_t off = 0; off < n; off += 4096) {
        size_t chunk = n - off < 4096 ? n - off : 4096;
        for (size_t i = 0; i < chunk; i++) {
            buf[i * 2] = (uint8_t)(s[off + i]);
            buf[i * 2 + 1] = (uint8_t)(s[off + i] >> 8);
        }
        blake3_hasher_update(&h, buf, chunk * 2);
    }
    uint8_t full[32];
    blake3_hasher_finalize(&h, full, 32);
    pc_hex_encode(full, 16, out); /* first 16 bytes -> 32 hex digits */
}

pc_status pc_preset_encode_tip(const pc_paint_gray_tile *t, pc_buf *out, pc_error *err)
{
    size_t n = 0;
    const uint16_t *s = tile_samples(t, &n);
    bool eight = true;
    for (size_t i = 0; i < n; i++) {
        if (s[i] % 257 != 0) {
            eight = false;
            break;
        }
    }
    uint8_t header[TIP_HEADER];
    memcpy(header, TIP_MAGIC, 6);
    header[6] = eight ? 8 : 16;
    header[7] = (uint8_t)(t->width);
    header[8] = (uint8_t)(t->width >> 8);
    header[9] = (uint8_t)(t->width >> 16);
    header[10] = (uint8_t)(t->width >> 24);
    header[11] = (uint8_t)(t->height);
    header[12] = (uint8_t)(t->height >> 8);
    header[13] = (uint8_t)(t->height >> 16);
    header[14] = (uint8_t)(t->height >> 24);
    /* Raw sample bytes (8-bit: v/257; 16-bit: LE u16). */
    pc_buf raw = PC_BUF_INIT;
    pc_status rc = pc_buf_reserve(&raw, eight ? n : n * 2, err);
    for (size_t i = 0; rc == PC_OK && i < n; i++) {
        if (eight) {
            rc = pc_buf_append_byte(&raw, (uint8_t)(s[i] / 257), err);
        } else {
            uint8_t le[2] = {(uint8_t)s[i], (uint8_t)(s[i] >> 8)};
            rc = pc_buf_append(&raw, le, 2, err);
        }
    }
    pc_buf comp = PC_BUF_INIT;
    if (rc == PC_OK) {
        rc = pc_deflate(PC_Z_ZLIB, PC_Z_FAST, raw.data, raw.len, &comp, err);
    }
    if (rc == PC_OK) {
        rc = pc_buf_append(out, header, TIP_HEADER, err);
    }
    if (rc == PC_OK) {
        rc = pc_buf_append(out, comp.data, comp.len, err);
    }
    pc_buf_fini(&raw);
    pc_buf_fini(&comp);
    return rc;
}

static uint32_t u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static pc_status decode_tip_max(const uint8_t *data, size_t len, uint32_t max_side, pc_paint_gray_tile *out,
                                pc_error *err)
{
    if (len < TIP_HEADER || memcmp(data, TIP_MAGIC, 6) != 0) {
        return pc_fail(err, PC_ERR_FORMAT, "not a PhotoCraft tip");
    }
    uint8_t bits = data[6];
    uint32_t w = u32_le(data + 7), h = u32_le(data + 11);
    uint32_t cap = max_side < PC_PRESET_MAX_TIP_SIDE ? max_side : PC_PRESET_MAX_TIP_SIDE;
    if (w == 0 || h == 0 || w > cap || h > cap) {
        return pc_fail(err, PC_ERR_FORMAT, "bad tip size %ux%u", w, h);
    }
    uint64_t count = (uint64_t)w * (uint64_t)h;
    if (count > PC_PRESET_MAX_TIP_SAMPLES) {
        return pc_fail(err, PC_ERR_FORMAT, "tip too large (%ux%u)", w, h);
    }
    uint64_t bytes_per = bits == 8 ? 1 : bits == 16 ? 2 : 0;
    if (bytes_per == 0) {
        return pc_fail(err, PC_ERR_FORMAT, "unsupported tip depth %u", bits);
    }
    uint64_t want = count * bytes_per;
    pc_buf raw = PC_BUF_INIT;
    pc_status rc = pc_inflate(PC_Z_ZLIB, data + TIP_HEADER, len - TIP_HEADER, (size_t)want + 1, &raw, NULL, err);
    if (rc != PC_OK) {
        pc_buf_fini(&raw);
        return rc;
    }
    if ((uint64_t)raw.len != want) {
        pc_buf_fini(&raw);
        return pc_fail(err, PC_ERR_FORMAT, "tip data has %zu bytes, expected %llu", raw.len,
                       (unsigned long long)want);
    }
    uint16_t *samples = pc_malloc_array(count, sizeof *samples);
    if (samples == NULL && count != 0) {
        pc_buf_fini(&raw);
        return pc_fail(err, PC_ERR_NOMEM, "tip");
    }
    if (bits == 8) {
        for (uint64_t i = 0; i < count; i++) {
            samples[i] = (uint16_t)((uint16_t)raw.data[i] * 257u);
        }
    } else {
        for (uint64_t i = 0; i < count; i++) {
            samples[i] = (uint16_t)((uint16_t)raw.data[i * 2] | ((uint16_t)raw.data[i * 2 + 1] << 8));
        }
    }
    rc = pc_paint_gray_tile_from_u16(w, h, samples, (size_t)count, out, err);
    free(samples);
    pc_buf_fini(&raw);
    return rc;
}

pc_status pc_preset_decode_tip(const uint8_t *data, size_t len, pc_paint_gray_tile *out, pc_error *err)
{
    return decode_tip_max(data, len, PC_PRESET_MAX_TIP_SIDE, out, err);
}

pc_status pc_preset_tip_file(const char *hash, pc_buf *out, pc_error *err)
{
    return pc_buf_appendf(out, err, "%s/%s.%s", PC_PRESET_TIPS_DIR, hash, PC_PRESET_TIP_EXT);
}

pc_status pc_preset_group_file_name(const char *group, pc_buf *out, pc_error *err)
{
    /* Sanitised, readable prefix of at most 48 code points, trimmed. */
    char stem[256];
    size_t sn = 0;
    size_t glen = strlen(group);
    size_t i = 0;
    uint32_t cp = 0;
    size_t taken = 0;
    while (i < glen && taken < 48) {
        if (!pc_utf8_next((const uint8_t *)group, glen, &i, &cp)) {
            break;
        }
        taken++;
        char c = '_';
        if ((cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == ' ' ||
            cp == '_' || cp == '-') {
            c = (char)cp;
        }
        if (sn < sizeof stem - 1) {
            stem[sn++] = c;
        }
    }
    stem[sn] = '\0';
    /* Trim ASCII spaces at both ends. */
    size_t s0 = 0;
    while (s0 < sn && stem[s0] == ' ') {
        s0++;
    }
    while (sn > s0 && stem[sn - 1] == ' ') {
        stem[--sn] = '\0';
    }
    const char *trimmed = stem + s0;
    if (*trimmed == '\0') {
        trimmed = "Ungrouped";
    }
    char hex[PC_BLAKE3_HEX_LEN + 1];
    pc_blake3_hex(group, glen, hex);
    hex[8] = '\0';
    return pc_buf_appendf(out, err, "%s-%s.%s", trimmed, hex, PC_PRESET_GROUP_EXT);
}

/* ============================================================ in-memory backend */

typedef struct mem_entry {
    char *name; /* owned */
    uint8_t *data;
    size_t len;
} mem_entry;

typedef struct mem_backend {
    pc_preset_backend base;
    pthread_mutex_t lock;
    mem_entry *files;
    size_t n, cap;
} mem_backend;

static mem_entry *mem_find(mem_backend *m, const char *name)
{
    for (size_t i = 0; i < m->n; i++) {
        if (strcmp(m->files[i].name, name) == 0) {
            return &m->files[i];
        }
    }
    return NULL;
}

static pc_status mem_list(pc_preset_backend *b, pc_preset_file **out, size_t *n, pc_error *err)
{
    mem_backend *m = (mem_backend *)b;
    pthread_mutex_lock(&m->lock);
    pc_preset_file *arr = NULL;
    pc_status rc = PC_OK;
    if (m->n > 0) {
        arr = pc_calloc_array(m->n, sizeof *arr);
        if (arr == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "preset list");
        }
    }
    size_t cnt = 0;
    for (size_t i = 0; rc == PC_OK && i < m->n; i++) {
        arr[cnt].name = pc_strndup(m->files[i].name, strlen(m->files[i].name));
        if (arr[cnt].name == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "preset list");
            break;
        }
        arr[cnt].size = m->files[i].len;
        cnt++;
    }
    pthread_mutex_unlock(&m->lock);
    if (rc != PC_OK) {
        for (size_t i = 0; i < cnt; i++) {
            free(arr[i].name);
        }
        free(arr);
        return rc;
    }
    *out = arr;
    *n = cnt;
    return PC_OK;
}

static pc_status mem_read(pc_preset_backend *b, const char *name, uint64_t max, pc_buf *out, pc_error *err)
{
    mem_backend *m = (mem_backend *)b;
    pthread_mutex_lock(&m->lock);
    mem_entry *e = mem_find(m, name);
    pc_status rc;
    if (e == NULL) {
        rc = pc_fail(err, PC_ERR_NOT_FOUND, "%s: not found", name);
    } else if ((uint64_t)e->len > max) {
        rc = pc_fail(err, PC_ERR_LIMIT, "%s: too large (%zu bytes)", name, e->len);
    } else {
        pc_buf_truncate(out, 0);
        rc = pc_buf_append(out, e->data, e->len, err);
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}

static pc_status mem_write(pc_preset_backend *b, const char *name, const uint8_t *data, size_t len, pc_error *err)
{
    if (!pc_preset_valid_name(name)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid store file name `%s`", name);
    }
    mem_backend *m = (mem_backend *)b;
    pthread_mutex_lock(&m->lock);
    pc_status rc = PC_OK;
    uint8_t *copy = NULL;
    if (len > 0) {
        copy = pc_malloc_array(len, 1);
        if (copy == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "preset write");
        } else {
            memcpy(copy, data, len);
        }
    }
    if (rc == PC_OK) {
        mem_entry *e = mem_find(m, name);
        if (e != NULL) {
            free(e->data);
            e->data = copy;
            e->len = len;
        } else {
            void *grown = pc_grow_array(m->files, &m->cap, m->n + 1, sizeof *m->files);
            char *nm = pc_strndup(name, strlen(name));
            if (grown == NULL || nm == NULL) {
                free(copy);
                free(nm);
                rc = pc_fail(err, PC_ERR_NOMEM, "preset write");
            } else {
                m->files = grown;
                m->files[m->n].name = nm;
                m->files[m->n].data = copy;
                m->files[m->n].len = len;
                m->n++;
            }
        }
    }
    pthread_mutex_unlock(&m->lock);
    return rc;
}

static pc_status mem_remove(pc_preset_backend *b, const char *name, pc_error *err)
{
    (void)err;
    mem_backend *m = (mem_backend *)b;
    pthread_mutex_lock(&m->lock);
    for (size_t i = 0; i < m->n; i++) {
        if (strcmp(m->files[i].name, name) == 0) {
            free(m->files[i].name);
            free(m->files[i].data);
            m->files[i] = m->files[--m->n];
            break;
        }
    }
    pthread_mutex_unlock(&m->lock);
    return PC_OK;
}

static void mem_destroy(pc_preset_backend *b)
{
    mem_backend *m = (mem_backend *)b;
    for (size_t i = 0; i < m->n; i++) {
        free(m->files[i].name);
        free(m->files[i].data);
    }
    free(m->files);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

pc_preset_backend *pc_preset_mem_backend_new(void)
{
    mem_backend *m = pc_calloc_array(1, sizeof *m);
    if (m == NULL) {
        return NULL;
    }
    pthread_mutex_init(&m->lock, NULL);
    m->base.list = mem_list;
    m->base.read = mem_read;
    m->base.write = mem_write;
    m->base.remove = mem_remove;
    m->base.destroy = mem_destroy;
    return &m->base;
}

/* ============================================================ directory backend */

typedef struct dir_backend {
    pc_preset_backend base;
    char *root; /* owned */
} dir_backend;

/* Join root + name into *out (already validated). */
static pc_status dir_path(const dir_backend *d, const char *name, pc_buf *out, pc_error *err)
{
    pc_buf_truncate(out, 0);
    return pc_buf_appendf(out, err, "%s/%s", d->root, name);
}

static pc_status dir_list_sub(const dir_backend *d, const char *sub, pc_preset_strs *names, uint64_t *sizes,
                              size_t *cap_hint, pc_preset_file **arr, size_t *n, pc_error *err)
{
    (void)names;
    (void)sizes;
    (void)cap_hint;
    pc_buf dirpath = PC_BUF_INIT;
    pc_status rc = sub[0] == '\0' ? pc_buf_append_str(&dirpath, d->root, err)
                                  : pc_buf_appendf(&dirpath, err, "%s/%s", d->root, sub);
    if (rc != PC_OK) {
        pc_buf_fini(&dirpath);
        return rc;
    }
    DIR *dd = opendir(pc_buf_cstr(&dirpath));
    if (dd == NULL) {
        pc_buf_fini(&dirpath);
        return errno == ENOENT ? PC_OK : pc_fail(err, PC_ERR_IO, "%s: %s", pc_buf_cstr(&dirpath), strerror(errno));
    }
    struct dirent *de;
    while (rc == PC_OK && (de = readdir(dd)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        pc_buf full = PC_BUF_INIT;
        if (pc_buf_appendf(&full, err, "%s/%s", pc_buf_cstr(&dirpath), de->d_name) != PC_OK) {
            pc_buf_fini(&full);
            continue;
        }
        struct stat st;
        /* lstat: never follow a symlink into a file (hostile store). */
        if (lstat(pc_buf_cstr(&full), &st) == 0 && S_ISREG(st.st_mode)) {
            pc_buf name = PC_BUF_INIT;
            pc_status nr = sub[0] == '\0' ? pc_buf_append_str(&name, de->d_name, err)
                                          : pc_buf_appendf(&name, err, "%s/%s", sub, de->d_name);
            if (nr == PC_OK && pc_preset_valid_name(pc_buf_cstr(&name))) {
                void *grown = pc_grow_array(*arr, cap_hint, *n + 1, sizeof **arr);
                char *nm = pc_strndup(pc_buf_cstr(&name), name.len);
                if (grown == NULL || nm == NULL) {
                    free(nm);
                    rc = pc_fail(err, PC_ERR_NOMEM, "preset list");
                } else {
                    *arr = grown;
                    (*arr)[*n].name = nm;
                    (*arr)[*n].size = (uint64_t)st.st_size;
                    (*n)++;
                }
            }
            pc_buf_fini(&name);
        }
        pc_buf_fini(&full);
    }
    closedir(dd);
    pc_buf_fini(&dirpath);
    return rc;
}

static pc_status dir_list(pc_preset_backend *b, pc_preset_file **out, size_t *n, pc_error *err)
{
    dir_backend *d = (dir_backend *)b;
    pc_preset_file *arr = NULL;
    size_t cnt = 0, cap = 0;
    pc_status rc = dir_list_sub(d, "", NULL, NULL, &cap, &arr, &cnt, err);
    if (rc == PC_OK) {
        rc = dir_list_sub(d, PC_PRESET_TIPS_DIR, NULL, NULL, &cap, &arr, &cnt, err);
    }
    if (rc != PC_OK) {
        for (size_t i = 0; i < cnt; i++) {
            free(arr[i].name);
        }
        free(arr);
        return rc;
    }
    *out = arr;
    *n = cnt;
    return PC_OK;
}

static pc_status dir_read(pc_preset_backend *b, const char *name, uint64_t max, pc_buf *out, pc_error *err)
{
    dir_backend *d = (dir_backend *)b;
    if (!pc_preset_valid_name(name)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid store file name `%s`", name);
    }
    pc_buf path = PC_BUF_INIT;
    pc_status rc = dir_path(d, name, &path, err);
    if (rc != PC_OK) {
        pc_buf_fini(&path);
        return rc;
    }
    FILE *f = fopen(pc_buf_cstr(&path), "rb");
    if (f == NULL) {
        rc = errno == ENOENT ? pc_fail(err, PC_ERR_NOT_FOUND, "%s: not found", name)
                             : pc_fail(err, PC_ERR_IO, "%s: %s", name, strerror(errno));
        pc_buf_fini(&path);
        return rc;
    }
    pc_buf_truncate(out, 0);
    uint8_t chunk[65536];
    bool over = false, oom = false, ioerr = false;
    for (;;) {
        size_t got = fread(chunk, 1, sizeof chunk, f);
        if (got > 0) {
            if ((uint64_t)out->len + got > max) {
                over = true;
                break;
            }
            if (pc_buf_append(out, chunk, got, err) != PC_OK) {
                oom = true;
                break;
            }
        }
        if (got < sizeof chunk) { /* short read: end of file or an error */
            ioerr = ferror(f) != 0;
            break;
        }
    }
    fclose(f);
    pc_buf_fini(&path);
    if (oom) {
        return PC_ERR_NOMEM;
    }
    if (over) {
        return pc_fail(err, PC_ERR_LIMIT, "%s: larger than %llu bytes", name, (unsigned long long)max);
    }
    if (ioerr) {
        return pc_fail(err, PC_ERR_IO, "%s: read error", name);
    }
    return PC_OK;
}

static pc_status dir_write(pc_preset_backend *b, const char *name, const uint8_t *data, size_t len, pc_error *err)
{
    dir_backend *d = (dir_backend *)b;
    if (!pc_preset_valid_name(name)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid store file name `%s`", name);
    }
    pc_buf path = PC_BUF_INIT;
    pc_status rc = dir_path(d, name, &path, err);
    if (rc == PC_OK) {
        /* Ensure the parent directory (root, or root/tips) exists. */
        const char *slash = strrchr(pc_buf_cstr(&path), '/');
        if (slash != NULL) {
            pc_buf parent = PC_BUF_INIT;
            if (pc_buf_append(&parent, path.data, (size_t)(slash - (char *)path.data), err) == PC_OK) {
                rc = pc_mkdir_p(pc_buf_cstr(&parent), err);
            } else {
                rc = PC_ERR_NOMEM;
            }
            pc_buf_fini(&parent);
        }
    }
    if (rc == PC_OK) {
        /* pc_file_write_atomic: temp file + rename; refuses to follow a symlink as the target. */
        rc = pc_file_write_atomic(pc_buf_cstr(&path), data, len, err);
    }
    pc_buf_fini(&path);
    return rc;
}

static pc_status dir_remove(pc_preset_backend *b, const char *name, pc_error *err)
{
    dir_backend *d = (dir_backend *)b;
    if (!pc_preset_valid_name(name)) {
        return pc_fail(err, PC_ERR_INVALID, "invalid store file name `%s`", name);
    }
    pc_buf path = PC_BUF_INIT;
    pc_status rc = dir_path(d, name, &path, err);
    if (rc == PC_OK && remove(pc_buf_cstr(&path)) != 0 && errno != ENOENT) {
        rc = pc_fail(err, PC_ERR_IO, "%s: %s", name, strerror(errno));
    }
    pc_buf_fini(&path);
    return rc;
}

static void dir_destroy(pc_preset_backend *b)
{
    dir_backend *d = (dir_backend *)b;
    free(d->root);
    free(d);
}

pc_preset_backend *pc_preset_dir_backend_new(const char *root)
{
    dir_backend *d = pc_calloc_array(1, sizeof *d);
    if (d == NULL) {
        return NULL;
    }
    d->root = pc_strndup(root, strlen(root));
    if (d->root == NULL) {
        free(d);
        return NULL;
    }
    d->base.list = dir_list;
    d->base.read = dir_read;
    d->base.write = dir_write;
    d->base.remove = dir_remove;
    d->base.destroy = dir_destroy;
    return &d->base;
}

/* ============================================================ internal store state */

typedef struct tip_size {
    char *hash; /* owned */
    uint64_t size;
} tip_size;

typedef struct synced_group {
    char *file; /* owned */
    pc_paint_preset_list presets;
    pc_preset_strs tips; /* tip files the group uses (full + previews) */
    uint64_t bytes;
    bool stale;
} synced_group;

typedef struct tip_cache_entry {
    char *key;
    pc_paint_gray_tile tile; /* shares samples */
    uint64_t used;
} tip_cache_entry;

typedef struct tip_cache {
    uint64_t limit, tick, bytes, decodes;
    tip_cache_entry *e;
    size_t n, cap;
} tip_cache;

typedef struct index_file {
    uint32_t version;
    pc_preset_strs groups;
    pc_preset_strs hidden_builtins;
    pc_preset_strs order;
} index_file;

struct pc_engine_preset_store {
    pc_preset_backend *backend; /* owned */
    bool has_synced_rev;
    uint64_t synced_rev;
    synced_group *groups;
    size_t ngroups, cap_groups;
    pc_preset_strs skipped; /* files that failed to load; never rewritten */
    tip_size *tips;
    size_t ntips, cap_tips;
    bool has_index;
    index_file index;
    pc_preset_strs warnings;
    tip_cache cache;
    pthread_mutex_t cache_lock;
};

static uint64_t tile_bytes(const pc_paint_gray_tile *t)
{
    return (uint64_t)pc_paint_gray_tile_len(t) * 2;
}

/* ---- tip-size map ---- */
static tip_size *tips_find(struct pc_engine_preset_store *st, const char *hash)
{
    for (size_t i = 0; i < st->ntips; i++) {
        if (strcmp(st->tips[i].hash, hash) == 0) {
            return &st->tips[i];
        }
    }
    return NULL;
}

static bool tips_insert(struct pc_engine_preset_store *st, const char *hash, uint64_t size)
{
    tip_size *e = tips_find(st, hash);
    if (e != NULL) {
        e->size = size;
        return true;
    }
    void *grown = pc_grow_array(st->tips, &st->cap_tips, st->ntips + 1, sizeof *st->tips);
    char *h = pc_strndup(hash, strlen(hash));
    if (grown == NULL || h == NULL) {
        free(h);
        return false;
    }
    st->tips = grown;
    st->tips[st->ntips].hash = h;
    st->tips[st->ntips].size = size;
    st->ntips++;
    return true;
}


/* ---- group map ---- */
static synced_group *groups_find(struct pc_engine_preset_store *st, const char *file)
{
    for (size_t i = 0; i < st->ngroups; i++) {
        if (strcmp(st->groups[i].file, file) == 0) {
            return &st->groups[i];
        }
    }
    return NULL;
}

static void synced_group_fini(synced_group *g)
{
    free(g->file);
    pc_paint_preset_list_fini(&g->presets);
    pc_preset_strs_fini(&g->tips);
}

/* Insert or replace a group (takes ownership of presets/tips). */
static bool groups_set(struct pc_engine_preset_store *st, const char *file, pc_paint_preset_list presets,
                       pc_preset_strs tips, uint64_t bytes, bool stale)
{
    synced_group *g = groups_find(st, file);
    if (g != NULL) {
        pc_paint_preset_list_fini(&g->presets);
        pc_preset_strs_fini(&g->tips);
        g->presets = presets;
        g->tips = tips;
        g->bytes = bytes;
        g->stale = stale;
        return true;
    }
    void *grown = pc_grow_array(st->groups, &st->cap_groups, st->ngroups + 1, sizeof *st->groups);
    char *f = pc_strndup(file, strlen(file));
    if (grown == NULL || f == NULL) {
        free(f);
        pc_paint_preset_list_fini(&presets);
        pc_preset_strs_fini(&tips);
        return false;
    }
    st->groups = grown;
    st->groups[st->ngroups].file = f;
    st->groups[st->ngroups].presets = presets;
    st->groups[st->ngroups].tips = tips;
    st->groups[st->ngroups].bytes = bytes;
    st->groups[st->ngroups].stale = stale;
    st->ngroups++;
    return true;
}

static void index_fini(index_file *ix)
{
    pc_preset_strs_fini(&ix->groups);
    pc_preset_strs_fini(&ix->hidden_builtins);
    pc_preset_strs_fini(&ix->order);
}

static void warn(struct pc_engine_preset_store *st, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    strs_push(&st->warnings, buf); /* best-effort: a dropped warning is not fatal */
}

/* Append a formatted warning to an owned string vector (vsnprintf bounds the output). */
static void warns(pc_preset_strs *s, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    strs_push(s, buf);
}

void pc_engine_preset_store_free(struct pc_engine_preset_store *st)
{
    if (st == NULL) {
        return;
    }
    if (st->backend != NULL && st->backend->destroy != NULL) {
        st->backend->destroy(st->backend);
    }
    for (size_t i = 0; i < st->ngroups; i++) {
        synced_group_fini(&st->groups[i]);
    }
    free(st->groups);
    pc_preset_strs_fini(&st->skipped);
    for (size_t i = 0; i < st->ntips; i++) {
        free(st->tips[i].hash);
    }
    free(st->tips);
    index_fini(&st->index);
    pc_preset_strs_fini(&st->warnings);
    for (size_t i = 0; i < st->cache.n; i++) {
        free(st->cache.e[i].key);
        pc_paint_gray_tile_fini(&st->cache.e[i].tile);
    }
    free(st->cache.e);
    pthread_mutex_destroy(&st->cache_lock);
    free(st);
}

uint64_t pc_engine_preset_store_bytes(const struct pc_engine_preset_store *st)
{
    uint64_t total = 0;
    for (size_t i = 0; i < st->ngroups; i++) {
        total += st->groups[i].bytes;
    }
    for (size_t i = 0; i < st->ntips; i++) {
        total += st->tips[i].size;
    }
    return total;
}

size_t pc_engine_preset_store_take_warnings(struct pc_engine_preset_store *st, char ***out)
{
    size_t n = st->warnings.len;
    *out = st->warnings.v;
    st->warnings.v = NULL;
    st->warnings.len = 0;
    st->warnings.cap = 0;
    return n;
}

/* ============================================================ built-in preset names */

/* Names of the built-in presets (a fresh list each call; sync isn't hot). */
static pc_preset_strs builtin_names(void)
{
    pc_preset_strs names = {0};
    pc_paint_preset_list b = {0};
    pc_error e = {0};
    if (pc_paint_presets_builtin(&b, &e) == PC_OK) {
        for (size_t i = 0; i < b.len; i++) {
            strs_push(&names, pc_str_c(&b.v[i].name));
        }
    }
    pc_paint_preset_list_fini(&b);
    return names;
}

static bool name_eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        unsigned char ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

/* ============================================================ brush bitmap helpers */

static const pc_paint_gray_tile *tip_full(const pc_paint_tip *t)
{
    return t->kind == PC_PAINT_TIP_SAMPLED ? &t->tile : NULL;
}
static const pc_paint_stored_tile *tip_stored_of(const pc_paint_tip *t)
{
    return t->kind == PC_PAINT_TIP_STORED ? &t->stored : NULL;
}
static const pc_paint_gray_tile *pat_full(const pc_paint_pattern *p)
{
    return p->kind == PC_PAINT_PATTERN_TILE ? &p->tile : NULL;
}
static const pc_paint_stored_tile *pat_stored_of(const pc_paint_pattern *p)
{
    return p->kind == PC_PAINT_PATTERN_STORED ? &p->stored : NULL;
}

static void set_tip_stored(pc_paint_tip *t, const pc_paint_stored_tile *r)
{
    pc_paint_tip_fini(t);
    memset(t, 0, sizeof *t);
    t->kind = PC_PAINT_TIP_STORED;
    pc_paint_stored_tile_copy(&t->stored, r);
}
static void set_tip_round(pc_paint_tip *t)
{
    pc_paint_tip_fini(t);
    memset(t, 0, sizeof *t);
    t->kind = PC_PAINT_TIP_ROUND;
}
static void set_pat_stored(pc_paint_pattern *p, const pc_paint_stored_tile *r)
{
    pc_paint_pattern_fini(p);
    memset(p, 0, sizeof *p);
    p->kind = PC_PAINT_PATTERN_STORED;
    pc_paint_stored_tile_copy(&p->stored, r);
}
static void strip_pat(pc_paint_pattern *p)
{
    pc_paint_pattern_fini(p);
    pc_paint_pattern_default(p);
}

/* The three bitmap slots of a brush, in [tip, dual tip, texture] order (as mutable pointers). */
static pc_paint_tip *slot_tip(pc_paint_brush *b, int i)
{
    return i == 0 ? &b->tip : i == 1 ? &b->dual_brush.tip : NULL;
}

/* ============================================================ group / index JSON */

/* Serialise one group's presets (a stripped copy of each brush) into bytes. Fills tips/infos/hashes
 * and sets *refs (owned stored tiles, present flags) per preset for set_stored after the write. */
typedef struct bm_ref {
    bool present;
    pc_paint_stored_tile tile;
} bm_ref;

static pc_status put_tip(struct pc_engine_preset_store *st, const char *h, const pc_paint_gray_tile *t,
                         uint64_t others, uint64_t *new_bytes, const char *name, pc_error *err)
{
    pc_buf bytes = PC_BUF_INIT;
    pc_status rc = pc_preset_encode_tip(t, &bytes, err);
    if (rc == PC_OK && (bytes.len == 0 || (uint64_t)bytes.len > PC_PRESET_MAX_TIP_BYTES)) {
        rc = pc_fail(err, PC_ERR_LIMIT, "the tip of `%s` is too large to save", name);
    }
    if (rc == PC_OK) {
        *new_bytes += bytes.len;
        if (others + *new_bytes > PC_PRESET_MAX_STORE_BYTES) {
            rc = pc_fail(err, PC_ERR_LIMIT, "the preset store is full (%llu MB limit)",
                         (unsigned long long)(PC_PRESET_MAX_STORE_BYTES >> 20));
        }
    }
    pc_buf tf = PC_BUF_INIT;
    if (rc == PC_OK) {
        rc = pc_preset_tip_file(h, &tf, err);
    }
    if (rc == PC_OK) {
        rc = st->backend->write(st->backend, pc_buf_cstr(&tf), bytes.data, bytes.len, err);
    }
    if (rc == PC_OK && !tips_insert(st, h, bytes.len)) {
        rc = pc_fail(err, PC_ERR_NOMEM, "preset tips");
    }
    pc_buf_fini(&bytes);
    pc_buf_fini(&tf);
    return rc;
}

/* Build one stored-preset JSON object inside `doc`. */
static pc_status write_one_preset(struct pc_engine_preset_store *st, yyjson_mut_doc *doc, yyjson_mut_val *obj,
                                  pc_paint_preset *p, uint64_t others, uint64_t *new_bytes, bm_ref refs[3],
                                  pc_preset_strs *hashes, yyjson_mut_val *infos, pc_error *err)
{
    pc_status rc = PC_OK;
    const char *refkey[3] = {NULL, NULL, NULL};
    for (int i = 0; i < 3 && rc == PC_OK; i++) {
        const pc_paint_gray_tile *full = i == 2 ? pat_full(&p->brush.texture.pattern) : tip_full(slot_tip(&p->brush, i));
        const pc_paint_stored_tile *stored =
            i == 2 ? pat_stored_of(&p->brush.texture.pattern) : tip_stored_of(slot_tip(&p->brush, i));
        if (full == NULL && stored == NULL) {
            continue;
        }
        pc_paint_stored_tile tile = {0};
        char key[33];
        if (full != NULL) {
            if (!pc_paint_gray_tile_is_valid(full)) {
                return pc_fail(err, PC_ERR_INVALID, "preset `%s` has an invalid tip", pc_str_c(&p->name));
            }
            pc_preset_tip_hash(full, key);
            if (tips_find(st, key) == NULL) {
                rc = put_tip(st, key, full, others, new_bytes, pc_str_c(&p->name), err);
                if (rc != PC_OK) {
                    return rc;
                }
            }
            pc_paint_gray_tile preview = PC_PAINT_GRAY_TILE_INIT;
            rc = pc_paint_gray_tile_preview(full, PC_PAINT_PREVIEW_SIDE, &preview, err);
            if (rc == PC_OK) {
                rc = pc_paint_stored_tile_init(key, strlen(key), full->width, full->height, &preview, NULL, &tile, err);
            }
            pc_paint_gray_tile_fini(&preview);
            if (rc != PC_OK) {
                return rc;
            }
        } else {
            size_t klen = 0;
            const char *k = pc_paint_stored_tile_key(stored, &klen);
            char kb[64];
            size_t kn = klen < sizeof kb - 1 ? klen : sizeof kb - 1;
            memcpy(kb, k, kn);
            kb[kn] = '\0';
            if (tips_find(st, kb) == NULL) {
                return pc_fail(err, PC_ERR_INVALID, "the tip of `%s` is missing from the preset store",
                               pc_str_c(&p->name));
            }
            pc_paint_stored_tile_copy(&tile, stored);
            tile.loaded = false;
            pc_paint_gray_tile_fini(&tile.full);
            memset(&tile.full, 0, sizeof tile.full);
            snprintf(key, sizeof key, "%.*s", (int)kn, kb);
        }
        char ph[33];
        pc_preset_tip_hash(&tile.preview, ph);
        if (tips_find(st, ph) == NULL) {
            rc = put_tip(st, ph, &tile.preview, others, new_bytes, pc_str_c(&p->name), err);
        }
        if (rc == PC_OK && (!strs_push(hashes, key) || !strs_push(hashes, ph))) {
            rc = pc_fail(err, PC_ERR_NOMEM, "preset hashes");
        }
        if (rc == PC_OK) {
            yyjson_mut_val *ikey = yyjson_mut_strcpy(doc, key);
            yyjson_mut_val *ival = yyjson_mut_obj(doc);
            yyjson_mut_obj_add_uint(doc, ival, "width", tile.width);
            yyjson_mut_obj_add_uint(doc, ival, "height", tile.height);
            yyjson_mut_obj_add_strcpy(doc, ival, "preview", ph);
            yyjson_mut_obj_add(infos, ikey, ival);
            refs[i].present = true;
            pc_paint_stored_tile_copy(&refs[i].tile, &tile);
            refkey[i] = NULL; /* set below from refs */
        }
        pc_paint_stored_tile_fini(&tile);
        if (rc != PC_OK) {
            return rc;
        }
    }
    /* name + stripped brush + tips + folder + captures. */
    yyjson_mut_obj_add_strcpy(doc, obj, "name", pc_str_c(&p->name));
    pc_paint_brush copy;
    memset(&copy, 0, sizeof copy);
    pc_paint_brush_copy(&copy, &p->brush);
    set_tip_round(&copy.tip);
    set_tip_round(&copy.dual_brush.tip);
    if (copy.texture.pattern.kind != PC_PAINT_PATTERN_PROCEDURAL) {
        strip_pat(&copy.texture.pattern);
    }
    yyjson_mut_val *bval = pc_paint_json_write(PC_PAINT_JSON_BRUSH, doc, &copy, 0);
    pc_paint_brush_fini(&copy);
    if (bval == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "brush json");
    }
    yyjson_mut_obj_add_val(doc, obj, "brush", bval);
    yyjson_mut_val *tips = yyjson_mut_obj(doc);
    static const char *tipnames[3] = {"tip", "dualTip", "texture"};
    for (int i = 0; i < 3; i++) {
        if (refs[i].present) {
            size_t kl = 0;
            const char *k = pc_paint_stored_tile_key(&refs[i].tile, &kl);
            char kb[64];
            size_t kn = kl < sizeof kb - 1 ? kl : sizeof kb - 1;
            memcpy(kb, k, kn);
            kb[kn] = '\0';
            (void)refkey;
            yyjson_mut_obj_add_strcpy(doc, tips, tipnames[i], kb);
        }
    }
    yyjson_mut_obj_add_val(doc, obj, "tips", tips);
    if (p->folder.len > 0) {
        yyjson_mut_val *fa = yyjson_mut_arr(doc);
        for (size_t i = 0; i < p->folder.len; i++) {
            yyjson_mut_arr_add_strcpy(doc, fa, pc_str_c(&p->folder.v[i]));
        }
        yyjson_mut_obj_add_val(doc, obj, "folder", fa);
    }
    if (!pc_paint_preset_captures_is_default(&p->captures)) {
        yyjson_mut_val *cap = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_bool(doc, cap, "size", p->captures.size);
        yyjson_mut_obj_add_bool(doc, cap, "toolSettings", p->captures.tool_settings);
        yyjson_mut_obj_add_bool(doc, cap, "color", p->captures.color);
        yyjson_mut_obj_add_val(doc, obj, "captures", cap);
    }
    return PC_OK;
}

static void set_stored_brush(pc_paint_brush *b, bm_ref refs[3])
{
    if (refs[0].present) {
        set_tip_stored(&b->tip, &refs[0].tile);
    }
    if (refs[1].present) {
        set_tip_stored(&b->dual_brush.tip, &refs[1].tile);
    }
    if (refs[2].present) {
        set_pat_stored(&b->texture.pattern, &refs[2].tile);
    }
}

static uint64_t others_bytes(struct pc_engine_preset_store *st, const char *except_file)
{
    uint64_t total = 0;
    for (size_t i = 0; i < st->ngroups; i++) {
        if (strcmp(st->groups[i].file, except_file) != 0) {
            total += st->groups[i].bytes;
        }
    }
    for (size_t i = 0; i < st->ntips; i++) {
        total += st->tips[i].size;
    }
    return total;
}

/* Write a group file (and the tip/preview files it needs). On success the presets' bitmaps become
 * stored tiles (full dropped); on error they are unchanged. */
static pc_status write_group(struct pc_engine_preset_store *st, const char *file, const char *group,
                             pc_paint_preset **items, size_t nitems, pc_error *err)
{
    uint64_t others = others_bytes(st, file);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (doc == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "group json");
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_strcpy(doc, root, "format", GROUP_FORMAT);
    yyjson_mut_obj_add_uint(doc, root, "version", 1);
    yyjson_mut_obj_add_strcpy(doc, root, "group", group);
    yyjson_mut_val *parr = yyjson_mut_arr(doc);
    yyjson_mut_val *infos = yyjson_mut_obj(doc);
    bm_ref *refs = pc_calloc_array(nitems ? nitems * 3 : 1, sizeof *refs);
    pc_preset_strs hashes = {0};
    uint64_t new_bytes = 0;
    pc_status rc = refs == NULL ? pc_fail(err, PC_ERR_NOMEM, "group") : PC_OK;
    for (size_t k = 0; rc == PC_OK && k < nitems; k++) {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        rc = write_one_preset(st, doc, obj, items[k], others, &new_bytes, &refs[k * 3], &hashes, infos, err);
        if (rc == PC_OK) {
            yyjson_mut_arr_add_val(parr, obj);
        }
    }
    if (rc == PC_OK) {
        yyjson_mut_obj_add_val(doc, root, "presets", parr);
        /* The group-level tips map: include only when non-empty (serde skip_if_empty). */
        if (yyjson_mut_obj_size(infos) > 0) {
            yyjson_mut_obj_add_val(doc, root, "tips", infos);
        }
    }
    pc_buf bytes = PC_BUF_INIT;
    if (rc == PC_OK) {
        rc = pc_json_write(root, 0, &bytes, err);
    }
    if (rc == PC_OK && (uint64_t)bytes.len > PC_PRESET_MAX_GROUP_BYTES) {
        rc = pc_fail(err, PC_ERR_LIMIT, "the group is too large to save (%llu MB limit)",
                     (unsigned long long)(PC_PRESET_MAX_GROUP_BYTES >> 20));
    }
    if (rc == PC_OK && others + new_bytes + (uint64_t)bytes.len > PC_PRESET_MAX_STORE_BYTES) {
        rc = pc_fail(err, PC_ERR_LIMIT, "the preset store is full (%llu MB limit)",
                     (unsigned long long)(PC_PRESET_MAX_STORE_BYTES >> 20));
    }
    if (rc == PC_OK) {
        rc = st->backend->write(st->backend, file, bytes.data, bytes.len, err);
    }
    if (rc == PC_OK) {
        /* Point the live presets at their stored tiles and record the synced group. */
        pc_paint_preset_list clone = {0};
        pc_paint_preset *cv = nitems ? pc_calloc_array(nitems, sizeof *cv) : NULL;
        if (nitems && cv == NULL) {
            rc = pc_fail(err, PC_ERR_NOMEM, "group");
        }
        for (size_t k = 0; rc == PC_OK && k < nitems; k++) {
            set_stored_brush(&items[k]->brush, &refs[k * 3]);
            rc = pc_paint_preset_clone(items[k], &cv[k], err);
            if (rc == PC_OK) {
                clone.len = k + 1;
            }
        }
        clone.v = cv;
        pc_preset_strs tips_copy = {0};
        if (rc == PC_OK && !strs_clone(&hashes, &tips_copy)) {
            rc = pc_fail(err, PC_ERR_NOMEM, "group");
        }
        if (rc == PC_OK && !groups_set(st, file, clone, tips_copy, (uint64_t)bytes.len, false)) {
            rc = pc_fail(err, PC_ERR_NOMEM, "group");
        } else if (rc != PC_OK) {
            pc_paint_preset_list_fini(&clone);
            pc_preset_strs_fini(&tips_copy);
        }
    }
    for (size_t k = 0; k < nitems * 3; k++) {
        pc_paint_stored_tile_fini(&refs[k].tile);
    }
    free(refs);
    pc_preset_strs_fini(&hashes);
    pc_buf_fini(&bytes);
    yyjson_mut_doc_free(doc);
    return rc;
}

/* Bring the files in line with `presets` (built-ins skipped): write changed groups, delete removed
 * ones and unreferenced tips, update the index. */
static void store_sync(struct pc_engine_preset_store *st, pc_paint_preset *presets, size_t n)
{
    pc_error err = {0};
    /* User groups in first-appearance order. */
    pc_preset_strs groups_order = {0};
    pc_preset_strs files = {0};
    for (size_t i = 0; i < n; i++) {
        if (!presets[i].builtin) {
            const char *g = pc_str_c(&presets[i].group);
            if (!strs_contains(&groups_order, g)) {
                strs_push(&groups_order, g);
            }
        }
    }
    for (size_t gi = 0; gi < groups_order.len; gi++) {
        const char *group = groups_order.v[gi];
        pc_buf fb = PC_BUF_INIT;
        if (pc_preset_group_file_name(group, &fb, &err) != PC_OK) {
            pc_buf_fini(&fb);
            continue;
        }
        const char *file = pc_buf_cstr(&fb);
        /* Gather the indices of this group's user presets. */
        pc_paint_preset **items = NULL;
        size_t nitems = 0, cap = 0;
        for (size_t i = 0; i < n; i++) {
            if (!presets[i].builtin && strcmp(pc_str_c(&presets[i].group), group) == 0) {
                void *grown = pc_grow_array(items, &cap, nitems + 1, sizeof *items);
                if (grown != NULL) {
                    items = grown;
                    items[nitems++] = &presets[i];
                }
            }
        }
        strs_push(&files, file);
        synced_group *existing = groups_find(st, file);
        bool unchanged = existing != NULL && !existing->stale && existing->presets.len == nitems;
        for (size_t k = 0; unchanged && k < nitems; k++) {
            if (!pc_paint_preset_eq(&existing->presets.v[k], items[k])) {
                unchanged = false;
            }
        }
        if (unchanged) {
            free(items);
            pc_buf_fini(&fb);
            continue;
        }
        if (strs_contains(&st->skipped, file)) {
            warn(st, "Couldn't save brush presets `%s`: %s could not be read, so it was left unchanged",
                 group[0] ? group : "(ungrouped)", file);
            free(items);
            pc_buf_fini(&fb);
            continue;
        }
        pc_error we = {0};
        if (write_group(st, file, group, items, nitems, &we) != PC_OK) {
            warn(st, "Couldn't save brush presets `%s`: %s", group[0] ? group : "(ungrouped)", we.msg);
        }
        free(items);
        pc_buf_fini(&fb);
    }
    /* Groups that are gone. */
    for (size_t i = 0; i < st->ngroups;) {
        if (!strs_contains(&files, st->groups[i].file)) {
            pc_error re = {0};
            if (st->backend->remove(st->backend, st->groups[i].file, &re) == PC_OK) {
                synced_group_fini(&st->groups[i]);
                st->groups[i] = st->groups[--st->ngroups];
                continue;
            }
            warn(st, "Couldn't delete brush presets file: %s", re.msg);
        }
        i++;
    }
    /* Tips nothing references any more. */
    for (size_t i = 0; i < st->ntips;) {
        bool used = false;
        for (size_t g = 0; g < st->ngroups && !used; g++) {
            used = strs_contains(&st->groups[g].tips, st->tips[i].hash);
        }
        if (!used) {
            pc_buf tf = PC_BUF_INIT;
            pc_error re = {0};
            if (pc_preset_tip_file(st->tips[i].hash, &tf, &re) == PC_OK &&
                st->backend->remove(st->backend, pc_buf_cstr(&tf), &re) == PC_OK) {
                pc_buf_fini(&tf);
                free(st->tips[i].hash);
                st->tips[i] = st->tips[--st->ntips];
                continue;
            }
            pc_buf_fini(&tf);
            warn(st, "Couldn't delete brush tip: %s", re.msg);
        }
        i++;
    }
    /* Index: group order, deleted built-ins, and the library order (only when non-default). */
    pc_preset_strs names = builtin_names();
    pc_preset_strs hidden = {0};
    for (size_t i = 0; i < names.len; i++) {
        bool present = false;
        for (size_t j = 0; j < n && !present; j++) {
            present = name_eq_ci(pc_str_c(&presets[j].name), names.v[i]);
        }
        if (!present) {
            strs_push(&hidden, names.v[i]);
        }
    }
    pc_preset_strs want_groups = {0};
    for (size_t i = 0; i < files.len; i++) {
        if (groups_find(st, files.v[i]) != NULL) {
            strs_push(&want_groups, files.v[i]);
        }
    }
    pc_preset_strs order = {0};
    for (size_t i = 0; i < n; i++) {
        strs_push(&order, pc_str_c(&presets[i].name));
    }
    /* Default order = built-ins in their order, then user presets. Save order only when it differs. */
    bool builtins_in_order = true;
    {
        size_t prev = SIZE_MAX;
        bool first = true;
        for (size_t i = 0; i < n && builtins_in_order; i++) {
            if (presets[i].builtin) {
                size_t pos = strs_find(&names, pc_str_c(&presets[i].name));
                if (pos == SIZE_MAX) {
                    builtins_in_order = false;
                } else if (first) {
                    prev = pos;
                    first = false;
                } else if (pos > prev) {
                    prev = pos;
                } else {
                    builtins_in_order = false;
                }
            }
        }
    }
    pc_preset_strs default_order = {0};
    for (size_t i = 0; i < n; i++) {
        if (presets[i].builtin) {
            strs_push(&default_order, pc_str_c(&presets[i].name));
        }
    }
    for (size_t i = 0; i < n; i++) {
        if (!presets[i].builtin) {
            strs_push(&default_order, pc_str_c(&presets[i].name));
        }
    }
    bool default_ok = builtins_in_order && strs_eq(&order, &default_order);
    if (default_ok) {
        pc_preset_strs_fini(&order);
        memset(&order, 0, sizeof order);
    }
    pc_preset_strs_fini(&default_order);
    bool same = st->has_index && strs_eq(&st->index.groups, &want_groups) &&
                strs_eq(&st->index.hidden_builtins, &hidden) && strs_eq(&st->index.order, &order);
    if (!same) {
        bool empty = want_groups.len == 0 && hidden.len == 0 && order.len == 0;
        pc_status rc = PC_OK;
        if (!(empty && !st->has_index)) {
            yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
            if (doc == NULL) {
                rc = PC_ERR_NOMEM;
            } else {
                yyjson_mut_val *root = yyjson_mut_obj(doc);
                yyjson_mut_doc_set_root(doc, root);
                yyjson_mut_obj_add_uint(doc, root, "version", 1);
                yyjson_mut_val *ga = yyjson_mut_arr(doc);
                for (size_t i = 0; i < want_groups.len; i++) {
                    yyjson_mut_arr_add_strcpy(doc, ga, want_groups.v[i]);
                }
                yyjson_mut_obj_add_val(doc, root, "groups", ga);
                yyjson_mut_val *ha = yyjson_mut_arr(doc);
                for (size_t i = 0; i < hidden.len; i++) {
                    yyjson_mut_arr_add_strcpy(doc, ha, hidden.v[i]);
                }
                yyjson_mut_obj_add_val(doc, root, "hiddenBuiltins", ha);
                yyjson_mut_val *oa = yyjson_mut_arr(doc);
                for (size_t i = 0; i < order.len; i++) {
                    yyjson_mut_arr_add_strcpy(doc, oa, order.v[i]);
                }
                yyjson_mut_obj_add_val(doc, root, "order", oa);
                pc_buf bytes = PC_BUF_INIT;
                pc_error we = {0};
                rc = pc_json_write(root, PC_JSON_PRETTY, &bytes, &we);
                if (rc == PC_OK) {
                    rc = st->backend->write(st->backend, PC_PRESET_INDEX_FILE, bytes.data, bytes.len, &we);
                }
                if (rc != PC_OK) {
                    warn(st, "Couldn't save the brush preset index: %s", we.msg);
                }
                pc_buf_fini(&bytes);
                yyjson_mut_doc_free(doc);
            }
        }
        if (rc == PC_OK) {
            index_fini(&st->index);
            st->index.version = 1;
            strs_clone(&want_groups, &st->index.groups);
            strs_clone(&hidden, &st->index.hidden_builtins);
            strs_clone(&order, &st->index.order);
            st->has_index = true;
        }
    }
    pc_preset_strs_fini(&groups_order);
    pc_preset_strs_fini(&files);
    pc_preset_strs_fini(&names);
    pc_preset_strs_fini(&hidden);
    pc_preset_strs_fini(&want_groups);
    pc_preset_strs_fini(&order);
}

/* ============================================================ loading (read side) */

typedef struct tip_info {
    char *hash; /* owned */
    uint32_t w, h;
    char *preview; /* owned */
} tip_info;

typedef struct info_map {
    tip_info *v;
    size_t n, cap;
} info_map;

static void info_map_fini(info_map *m)
{
    for (size_t i = 0; i < m->n; i++) {
        free(m->v[i].hash);
        free(m->v[i].preview);
    }
    free(m->v);
}
static const tip_info *info_find(const info_map *m, const char *h)
{
    for (size_t i = 0; i < m->n; i++) {
        if (strcmp(m->v[i].hash, h) == 0) {
            return &m->v[i];
        }
    }
    return NULL;
}
static void info_insert(info_map *m, const char *h, uint32_t w, uint32_t ht, const char *preview)
{
    if (info_find(m, h) != NULL) {
        return; /* first wins */
    }
    void *grown = pc_grow_array(m->v, &m->cap, m->n + 1, sizeof *m->v);
    char *hc = pc_strndup(h, strlen(h)), *pc = pc_strndup(preview, strlen(preview));
    if (grown == NULL || hc == NULL || pc == NULL) {
        free(hc);
        free(pc);
        return;
    }
    m->v = grown;
    m->v[m->n].hash = hc;
    m->v[m->n].w = w;
    m->v[m->n].h = ht;
    m->v[m->n].preview = pc;
    m->n++;
}

typedef struct tile_ent {
    char *hash; /* owned */
    pc_paint_stored_tile tile;
} tile_ent;
typedef struct tile_map {
    tile_ent *v;
    size_t n, cap;
} tile_map;

static void tile_map_fini(tile_map *m)
{
    for (size_t i = 0; i < m->n; i++) {
        free(m->v[i].hash);
        pc_paint_stored_tile_fini(&m->v[i].tile);
    }
    free(m->v);
}
static const tile_ent *tile_find(const tile_map *m, const char *h)
{
    for (size_t i = 0; i < m->n; i++) {
        if (strcmp(m->v[i].hash, h) == 0) {
            return &m->v[i];
        }
    }
    return NULL;
}
static void tile_insert(tile_map *m, const char *h, pc_paint_stored_tile tile)
{
    void *grown = pc_grow_array(m->v, &m->cap, m->n + 1, sizeof *m->v);
    char *hc = pc_strndup(h, strlen(h));
    if (grown == NULL || hc == NULL) {
        free(hc);
        pc_paint_stored_tile_fini(&tile);
        return;
    }
    m->v = grown;
    m->v[m->n].hash = hc;
    m->v[m->n].tile = tile;
    m->n++;
}

static uint64_t tip_size_of(const pc_preset_file *files, size_t nf, const char *hash)
{
    pc_buf tf = PC_BUF_INIT;
    pc_error e = {0};
    uint64_t sz = UINT64_MAX; /* sentinel: not found */
    if (pc_preset_tip_file(hash, &tf, &e) == PC_OK) {
        for (size_t i = 0; i < nf; i++) {
            if (strcmp(files[i].name, pc_buf_cstr(&tf)) == 0) {
                sz = files[i].size;
                break;
            }
        }
    }
    pc_buf_fini(&tf);
    return sz;
}

/* Read a tip file and return a loaded StoredTile (preview only) for each wanted hash. Previews come
 * from the group files' TipInfo; a tip without one is decoded in full to make its preview. */
static void load_tiles(pc_preset_backend *be, const pc_preset_strs *hashes, const pc_preset_file *files,
                       size_t nf, const info_map *info, tile_map *tiles, pc_preset_strs *regenerated,
                       pc_preset_strs *warnings)
{
    for (size_t i = 0; i < hashes->len; i++) {
        const char *h = hashes->v[i];
        if (tile_find(tiles, h) != NULL) {
            continue;
        }
        if (tip_size_of(files, nf, h) == UINT64_MAX) {
            warns(warnings, "brush presets: tip %s skipped: file missing", h);
            continue;
        }
        const tip_info *ti = info_find(info, h);
        bool sane = ti != NULL && ti->w > 0 && ti->h > 0 && ti->w <= PC_PRESET_MAX_TIP_SIDE &&
                    ti->h <= PC_PRESET_MAX_TIP_SIDE &&
                    (uint64_t)ti->w * ti->h <= PC_PRESET_MAX_TIP_SAMPLES &&
                    tip_size_of(files, nf, ti->preview) != UINT64_MAX;
        bool made = false;
        if (sane) {
            pc_buf pf = PC_BUF_INIT, bytes = PC_BUF_INIT;
            pc_error e = {0};
            if (pc_preset_tip_file(ti->preview, &pf, &e) == PC_OK &&
                be->read(be, pc_buf_cstr(&pf), 1u << 20, &bytes, &e) == PC_OK) {
                pc_paint_gray_tile prev = PC_PAINT_GRAY_TILE_INIT;
                if (decode_tip_max(bytes.data, bytes.len, PC_PAINT_PREVIEW_SIDE, &prev, &e) == PC_OK) {
                    pc_paint_stored_tile tile = {0};
                    if (pc_paint_stored_tile_init(h, strlen(h), ti->w, ti->h, &prev, NULL, &tile, &e) == PC_OK) {
                        tile_insert(tiles, h, tile);
                        made = true;
                    }
                }
                pc_paint_gray_tile_fini(&prev);
            }
            pc_buf_fini(&pf);
            pc_buf_fini(&bytes);
        }
        if (!made) {
            /* Decode the full tip to make a preview (a store written before previews existed). */
            pc_buf tf = PC_BUF_INIT, bytes = PC_BUF_INIT;
            pc_error e = {0};
            bool ok = false;
            if (pc_preset_tip_file(h, &tf, &e) == PC_OK &&
                be->read(be, pc_buf_cstr(&tf), PC_PRESET_MAX_TIP_BYTES, &bytes, &e) == PC_OK) {
                pc_paint_gray_tile full = PC_PAINT_GRAY_TILE_INIT, prev = PC_PAINT_GRAY_TILE_INIT;
                if (pc_preset_decode_tip(bytes.data, bytes.len, &full, &e) == PC_OK &&
                    pc_paint_gray_tile_preview(&full, PC_PAINT_PREVIEW_SIDE, &prev, &e) == PC_OK) {
                    pc_paint_stored_tile tile = {0};
                    if (pc_paint_stored_tile_init(h, strlen(h), full.width, full.height, &prev, NULL, &tile,
                                                  &e) == PC_OK) {
                        tile_insert(tiles, h, tile);
                        strs_push(regenerated, h);
                        ok = true;
                    }
                }
                pc_paint_gray_tile_fini(&full);
                pc_paint_gray_tile_fini(&prev);
            }
            if (!ok) {
                warns(warnings, "brush presets: tip %s skipped: %s", h, e.msg[0] ? e.msg : "unreadable");
            }
            pc_buf_fini(&tf);
            pc_buf_fini(&bytes);
        }
    }
}

/* Convert a pc_preset_strs into a pc_paint_str_list (sanitised folder: trim, non-empty, max depth). */
static pc_status strs_to_folder(const pc_preset_strs *s, pc_paint_str_list *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    size_t depth = s->len < PRESET_MAX_FOLDER_DEPTH ? s->len : PRESET_MAX_FOLDER_DEPTH;
    if (depth == 0) {
        return PC_OK;
    }
    pc_str *v = pc_calloc_array(depth, sizeof *v);
    if (v == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "folder");
    }
    size_t cnt = 0;
    for (size_t i = 0; i < s->len && cnt < depth; i++) {
        const char *raw = s->v[i];
        size_t start = 0, len = pc_utf8_trim(raw, strlen(raw), &start);
        if (len == 0) {
            continue;
        }
        if (pc_str_set(&v[cnt], raw + start, len, err) != PC_OK) {
            for (size_t j = 0; j < cnt; j++) {
                pc_str_fini(&v[j]);
            }
            free(v);
            return PC_ERR_NOMEM;
        }
        cnt++;
    }
    out->v = v;
    out->len = cnt;
    return PC_OK;
}

/* ---- parsed group file (intermediate, freed after presets are built) ---- */
typedef struct pstore_preset {
    pc_str name;
    pc_paint_brush brush;
    char *tip, *dual_tip, *texture; /* owned hashes or NULL */
    pc_preset_strs folder;
    pc_paint_preset_captures captures;
} pstore_preset;

typedef struct pstore_group {
    char *file;
    uint64_t size;
    char *group;
    pstore_preset *v;
    size_t n;
    info_map tips;
} pstore_group;

static void pstore_preset_fini(pstore_preset *p)
{
    if (p == NULL) {
        return;
    }
    pc_str_fini(&p->name);
    pc_paint_brush_fini(&p->brush);
    free(p->tip);
    free(p->dual_tip);
    free(p->texture);
    pc_preset_strs_fini(&p->folder);
}
static void pstore_group_fini(pstore_group *g)
{
    free(g->file);
    free(g->group);
    for (size_t i = 0; i < g->n; i++) {
        pstore_preset_fini(&g->v[i]);
    }
    free(g->v);
    info_map_fini(&g->tips);
}

static char *dup_str_val(yyjson_val *v)
{
    const char *s = yyjson_get_str(v);
    return s != NULL ? pc_strndup(s, strlen(s)) : NULL;
}

/* Parse a group file's bytes. Returns PC_OK and fills *out, or an error (message in err). */
static pc_status parse_group(const uint8_t *data, size_t len, const char *file, uint64_t size,
                             pstore_group *out, pc_error *err)
{
    memset(out, 0, sizeof *out);
    yyjson_doc *doc = NULL;
    pc_status rc = pc_json_parse((const char *)data, len, &doc, err);
    if (rc != PC_OK) {
        return rc;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return pc_fail(err, PC_ERR_FORMAT, "not an object");
    }
    const char *fmt = yyjson_get_str(yyjson_obj_get(root, "format"));
    if (fmt == NULL || strcmp(fmt, GROUP_FORMAT) != 0) {
        yyjson_doc_free(doc);
        return pc_fail(err, PC_ERR_FORMAT, "unknown format `%s`", fmt ? fmt : "");
    }
    uint64_t ver = yyjson_get_uint(yyjson_obj_get(root, "version"));
    if (ver != 1) {
        yyjson_doc_free(doc);
        return pc_fail(err, PC_ERR_FORMAT, "unsupported version %llu", (unsigned long long)ver);
    }
    yyjson_val *parr = yyjson_obj_get(root, "presets");
    size_t np = yyjson_is_arr(parr) ? yyjson_arr_size(parr) : 0;
    if (np > PC_PRESET_MAX_PRESETS_PER_GROUP) {
        yyjson_doc_free(doc);
        return pc_fail(err, PC_ERR_LIMIT, "%zu presets (limit %u)", np, PC_PRESET_MAX_PRESETS_PER_GROUP);
    }
    out->file = pc_strndup(file, strlen(file));
    out->size = size;
    out->group = dup_str_val(yyjson_obj_get(root, "group"));
    if (out->group == NULL) {
        out->group = pc_strndup("", 0);
    }
    out->v = np ? pc_calloc_array(np, sizeof *out->v) : NULL;
    if (out->file == NULL || out->group == NULL || (np && out->v == NULL)) {
        yyjson_doc_free(doc);
        pstore_group_fini(out);
        return pc_fail(err, PC_ERR_NOMEM, "group");
    }
    yyjson_arr_iter it;
    yyjson_arr_iter_init(parr, &it);
    yyjson_val *pv;
    size_t k = 0;
    while (rc == PC_OK && (pv = yyjson_arr_iter_next(&it)) != NULL) {
        pstore_preset *sp = &out->v[k];
        pc_paint_preset_captures_default(&sp->captures);
        const char *nm = yyjson_get_str(yyjson_obj_get(pv, "name"));
        if (nm == NULL) {
            rc = pc_fail(err, PC_ERR_FORMAT, "a preset is missing `name`");
            break;
        }
        rc = pc_str_set_c(&sp->name, nm, err);
        if (rc == PC_OK) {
            rc = pc_paint_json_read(PC_PAINT_JSON_BRUSH, yyjson_obj_get(pv, "brush"), &sp->brush, err);
        }
        if (rc != PC_OK) {
            break;
        }
        yyjson_val *tips = yyjson_obj_get(pv, "tips");
        if (yyjson_is_obj(tips)) {
            sp->tip = dup_str_val(yyjson_obj_get(tips, "tip"));
            sp->dual_tip = dup_str_val(yyjson_obj_get(tips, "dualTip"));
            sp->texture = dup_str_val(yyjson_obj_get(tips, "texture"));
        }
        yyjson_val *folder = yyjson_obj_get(pv, "folder");
        if (yyjson_is_arr(folder)) {
            yyjson_arr_iter fit;
            yyjson_arr_iter_init(folder, &fit);
            yyjson_val *fv;
            while ((fv = yyjson_arr_iter_next(&fit)) != NULL) {
                const char *fs = yyjson_get_str(fv);
                if (fs != NULL) {
                    strs_push(&sp->folder, fs);
                }
            }
        }
        yyjson_val *cap = yyjson_obj_get(pv, "captures");
        if (yyjson_is_obj(cap)) {
            yyjson_val *x;
            if ((x = yyjson_obj_get(cap, "size")) != NULL) {
                sp->captures.size = yyjson_get_bool(x);
            }
            if ((x = yyjson_obj_get(cap, "toolSettings")) != NULL) {
                sp->captures.tool_settings = yyjson_get_bool(x);
            }
            if ((x = yyjson_obj_get(cap, "color")) != NULL) {
                sp->captures.color = yyjson_get_bool(x);
            }
        }
        out->n = ++k;
    }
    /* Group-level tips map. */
    yyjson_val *tipmap = yyjson_obj_get(root, "tips");
    if (rc == PC_OK && yyjson_is_obj(tipmap)) {
        yyjson_obj_iter mit;
        yyjson_obj_iter_init(tipmap, &mit);
        yyjson_val *key;
        while ((key = yyjson_obj_iter_next(&mit)) != NULL) {
            yyjson_val *info = yyjson_obj_iter_get_val(key);
            const char *h = yyjson_get_str(key);
            const char *prev = yyjson_get_str(yyjson_obj_get(info, "preview"));
            if (h != NULL && prev != NULL) {
                info_insert(&out->tips, h, (uint32_t)yyjson_get_uint(yyjson_obj_get(info, "width")),
                            (uint32_t)yyjson_get_uint(yyjson_obj_get(info, "height")), prev);
            }
        }
    }
    yyjson_doc_free(doc);
    if (rc != PC_OK) {
        pstore_group_fini(out);
    }
    return rc;
}

/* Collect a preset's non-NULL tip hashes into `hashes` (deduplicated there is done by caller). */
static void push_refs(pc_preset_strs *hashes, const pstore_preset *p)
{
    if (p == NULL) {
        return;
    }
    const char *r[3] = {p->tip, p->dual_tip, p->texture};
    for (int i = 0; i < 3; i++) {
        if (r[i] != NULL && !strs_contains(hashes, r[i])) {
            strs_push(hashes, r[i]);
        }
    }
}

void pc_engine_preset_store_open(pc_preset_backend *backend, pc_engine_preset_opened *out)
{
    memset(out, 0, sizeof *out);
    struct pc_engine_preset_store *st = pc_calloc_array(1, sizeof *st);
    if (st == NULL) {
        if (backend != NULL && backend->destroy != NULL) {
            backend->destroy(backend);
        }
        return;
    }
    pthread_mutex_init(&st->cache_lock, NULL);
    st->cache.limit = PC_PRESET_TIP_CACHE_BYTES;
    st->backend = backend;

    pc_preset_file *files = NULL;
    size_t nf = 0;
    pc_error e = {0};
    if (backend->list(backend, &files, &nf, &e) != PC_OK) {
        warns(&out->warnings, "brush presets: %s", e.msg);
    }

    /* index.json, if present. */
    index_file index = {0};
    bool have_index = false;
    bool index_listed = false;
    for (size_t i = 0; i < nf; i++) {
        if (strcmp(files[i].name, PC_PRESET_INDEX_FILE) == 0) {
            index_listed = true;
        }
    }
    if (index_listed) {
        pc_buf b = PC_BUF_INIT;
        yyjson_doc *doc = NULL;
        if (backend->read(backend, PC_PRESET_INDEX_FILE, PC_PRESET_MAX_INDEX_BYTES, &b, &e) == PC_OK &&
            pc_json_parse((const char *)b.data, b.len, &doc, &e) == PC_OK && yyjson_is_obj(yyjson_doc_get_root(doc))) {
            yyjson_val *root = yyjson_doc_get_root(doc);
            index.version = (uint32_t)yyjson_get_uint(yyjson_obj_get(root, "version"));
            const char *keys[3] = {"groups", "hiddenBuiltins", "order"};
            pc_preset_strs *dst[3] = {&index.groups, &index.hidden_builtins, &index.order};
            for (int j = 0; j < 3; j++) {
                yyjson_val *a = yyjson_obj_get(root, keys[j]);
                if (yyjson_is_arr(a)) {
                    yyjson_arr_iter ai;
                    yyjson_arr_iter_init(a, &ai);
                    yyjson_val *v;
                    while ((v = yyjson_arr_iter_next(&ai)) != NULL) {
                        const char *s = yyjson_get_str(v);
                        if (s != NULL) {
                            strs_push(dst[j], s);
                        }
                    }
                }
            }
            have_index = true;
        } else {
        warns(&out->warnings, "brush presets: %s skipped: %s", PC_PRESET_INDEX_FILE, e.msg);
        }
        if (doc != NULL) {
            yyjson_doc_free(doc);
        }
        pc_buf_fini(&b);
    }

    /* Group files, in index order then by name. */
    pc_preset_strs gfiles = {0};
    for (size_t i = 0; i < nf; i++) {
        const char *n = files[i].name;
        size_t l = strlen(n);
        if (strchr(n, '/') == NULL && l > sizeof(PC_PRESET_GROUP_EXT) &&
            strcmp(n + l - strlen("." PC_PRESET_GROUP_EXT), "." PC_PRESET_GROUP_EXT) == 0) {
            strs_push(&gfiles, n);
        }
    }
    /* sort by name */
    for (size_t a = 0; a + 1 < gfiles.len; a++) {
        for (size_t b = a + 1; b < gfiles.len; b++) {
            if (strcmp(gfiles.v[a], gfiles.v[b]) > 0) {
                char *t = gfiles.v[a];
                gfiles.v[a] = gfiles.v[b];
                gfiles.v[b] = t;
            }
        }
    }
    if (have_index) {
        /* stable sort by index position (unlisted -> SIZE_MAX, keeps name order). */
        for (size_t a = 0; a + 1 < gfiles.len; a++) {
            for (size_t b = a + 1; b < gfiles.len; b++) {
                size_t pa = strs_find(&index.groups, gfiles.v[a]);
                size_t pb = strs_find(&index.groups, gfiles.v[b]);
                if (pa > pb) {
                    char *t = gfiles.v[a];
                    gfiles.v[a] = gfiles.v[b];
                    gfiles.v[b] = t;
                }
            }
        }
    }
    if (gfiles.len > PC_PRESET_MAX_GROUPS) {
        warns(&out->warnings, "brush presets: only the first %u of %zu groups were loaded", PC_PRESET_MAX_GROUPS,
                 gfiles.len);
        for (size_t i = PC_PRESET_MAX_GROUPS; i < gfiles.len; i++) {
            free(gfiles.v[i]);
        }
        gfiles.len = PC_PRESET_MAX_GROUPS;
    }

    /* Parse the group files. */
    pstore_group *parsed = gfiles.len ? pc_calloc_array(gfiles.len, sizeof *parsed) : NULL;
    size_t nparsed = 0;
    pc_preset_strs skipped = {0};
    bool failed = false;
    for (size_t i = 0; i < gfiles.len && parsed != NULL; i++) {
        pc_buf b = PC_BUF_INIT;
        pc_error pe = {0};
        uint64_t size = 0;
        for (size_t j = 0; j < nf; j++) {
            if (strcmp(files[j].name, gfiles.v[i]) == 0) {
                size = files[j].size;
            }
        }
        pc_status prc = backend->read(backend, gfiles.v[i], PC_PRESET_MAX_GROUP_BYTES, &b, &pe);
        if (prc == PC_OK) {
            prc = parse_group(b.data, b.len, gfiles.v[i], size, &parsed[nparsed], &pe);
        }
        if (prc == PC_OK) {
            nparsed++;
        } else {
            failed = true;
        warns(&out->warnings, "brush presets: %s skipped: %s", gfiles.v[i], pe.msg);
            strs_push(&skipped, gfiles.v[i]);
        }
        pc_buf_fini(&b);
    }

    /* Wanted hashes + info map. */
    pc_preset_strs wanted = {0};
    info_map info = {0};
    for (size_t g = 0; g < nparsed; g++) {
        for (size_t p = 0; p < parsed[g].n; p++) {
            push_refs(&wanted, &parsed[g].v[p]);
        }
        for (size_t t = 0; t < parsed[g].tips.n; t++) {
            const tip_info *ti = &parsed[g].tips.v[t];
            info_insert(&info, ti->hash, ti->w, ti->h, ti->preview);
        }
    }
    tile_map tiles = {0};
    pc_preset_strs regenerated = {0};
    load_tiles(backend, &wanted, files, nf, &info, &tiles, &regenerated, &out->warnings);

    /* Build presets, group by group. */
    pc_paint_preset *opresets = NULL;
    size_t nop = 0, cap_op = 0;
    for (size_t g = 0; g < nparsed; g++) {
        pstore_group *pg = &parsed[g];
        pc_paint_preset *items = pg->n ? pc_calloc_array(pg->n, sizeof *items) : NULL;
        size_t nitems = 0;
        pc_preset_strs hashes = {0};
        size_t missing = 0;
        bool stale = false;
        for (size_t p = 0; p < pg->n && pg->v != NULL; p++) {
            pstore_preset *sp = &pg->v[p];
            const char *refs[3] = {sp->tip, sp->dual_tip, sp->texture};
            const tile_ent *te[3] = {NULL, NULL, NULL};
            bool ok = true;
            for (int i = 0; i < 3; i++) {
                if (refs[i] != NULL) {
                    te[i] = tile_find(&tiles, refs[i]);
                    if (te[i] == NULL) {
                        ok = false;
                    }
                }
            }
            if (!ok) {
                missing++;
                continue;
            }
            pc_paint_preset *dst = &items[nitems];
            pc_error be2 = {0};
            memset(dst, 0, sizeof *dst);
            pc_paint_brush_copy(&dst->brush, &sp->brush);
            if (te[0] != NULL) {
                set_tip_stored(&dst->brush.tip, &te[0]->tile);
            }
            if (te[1] != NULL) {
                set_tip_stored(&dst->brush.dual_brush.tip, &te[1]->tile);
            }
            if (te[2] != NULL) {
                set_pat_stored(&dst->brush.texture.pattern, &te[2]->tile);
            }
            pc_str_clone(&sp->name, &dst->name, &be2);
            dst->builtin = false;
            pc_str_set_c(&dst->group, pg->group, &be2);
            strs_to_folder(&sp->folder, &dst->folder, &be2);
            dst->captures = sp->captures;
            /* Track tip + preview hashes this group uses. */
            for (int i = 0; i < 3; i++) {
                if (refs[i] != NULL) {
                    bool in_info = info_find(&info, refs[i]) != NULL;
                    stale = stale || strs_contains(&regenerated, refs[i]) || !in_info;
                    strs_push(&hashes, refs[i]);
                    const tip_info *ti = info_find(&info, refs[i]);
                    if (ti != NULL && tip_size_of(files, nf, ti->preview) != UINT64_MAX) {
                        strs_push(&hashes, ti->preview);
                    }
                }
            }
            nitems++;
        }
        if (missing > 0) {
            failed = true;
        warns(&out->warnings, "brush presets: %s skipped: %zu preset(s) have missing or unreadable tips",
                     pg->file, missing);
            strs_push(&skipped, pg->file);
            for (size_t x = 0; x < nitems; x++) {
                pc_paint_preset_fini(&items[x]);
            }
            free(items);
            pc_preset_strs_fini(&hashes);
            continue;
        }
        for (size_t x = 0; x < hashes.len; x++) {
            uint64_t sz = tip_size_of(files, nf, hashes.v[x]);
            if (sz != UINT64_MAX) {
                tips_insert(st, hashes.v[x], sz);
            }
        }
        /* opened.presets gets clones; the group's Synced takes the items. */
        for (size_t x = 0; x < nitems; x++) {
            void *grown = pc_grow_array(opresets, &cap_op, nop + 1, sizeof *opresets);
            pc_error ce = {0};
            if (grown != NULL) {
                opresets = grown;
                memset(&opresets[nop], 0, sizeof opresets[nop]);
                if (pc_paint_preset_clone(&items[x], &opresets[nop], &ce) == PC_OK) {
                    nop++;
                }
            }
        }
        pc_paint_preset_list gl = {items, nitems};
        groups_set(st, pg->file, gl, hashes, pg->size, stale); /* takes hashes */
    }

    /* skipped set. */
    st->skipped = skipped;
    /* Orphan tips become garbage only when no group failed to load. */
    if (!failed) {
        for (size_t i = 0; i < nf; i++) {
            const char *n = files[i].name;
            size_t pre = strlen(PC_PRESET_TIPS_DIR "/");
            size_t suf = strlen("." PC_PRESET_TIP_EXT);
            size_t l = strlen(n);
            if (l > pre + suf && strncmp(n, PC_PRESET_TIPS_DIR "/", pre) == 0 &&
                strcmp(n + l - suf, "." PC_PRESET_TIP_EXT) == 0) {
                char hash[64];
                size_t hl = l - pre - suf;
                if (hl < sizeof hash) {
                    memcpy(hash, n + pre, hl);
                    hash[hl] = '\0';
                    if (tips_find(st, hash) == NULL) {
                        tips_insert(st, hash, files[i].size);
                    }
                }
            }
        }
    }

    /* Opened outputs. */
    out->store = st;
    out->presets.v = opresets;
    out->presets.len = nop;
    if (have_index) {
        strs_clone(&index.hidden_builtins, &out->hidden_builtins);
        strs_clone(&index.order, &out->order);
    }
    st->index = index;
    st->has_index = have_index;

    /* Cleanup intermediates. */
    for (size_t i = 0; i < nparsed; i++) {
        pstore_group_fini(&parsed[i]);
    }
    free(parsed);
    tile_map_fini(&tiles);
    info_map_fini(&info);
    pc_preset_strs_fini(&wanted);
    pc_preset_strs_fini(&regenerated);
    pc_preset_strs_fini(&gfiles);
    for (size_t i = 0; i < nf; i++) {
        free(files[i].name);
    }
    free(files);
}

void pc_engine_preset_store_open_dir(const char *root, pc_engine_preset_opened *out)
{
    pc_preset_backend *be = pc_preset_dir_backend_new(root);
    if (be == NULL) {
        memset(out, 0, sizeof *out);
        return;
    }
    pc_engine_preset_store_open(be, out);
}

void pc_engine_preset_opened_fini(pc_engine_preset_opened *o)
{
    if (o == NULL) {
        return;
    }
    pc_engine_preset_store_free(o->store);
    pc_paint_preset_list_fini(&o->presets);
    pc_preset_strs_fini(&o->hidden_builtins);
    pc_preset_strs_fini(&o->order);
    pc_preset_strs_fini(&o->warnings);
    memset(o, 0, sizeof *o);
}

/* ============================================================ decoded-tip cache */

static void cache_evict(tip_cache *c, const char *keep)
{
    while (c->bytes > c->limit) {
        size_t oldest = SIZE_MAX;
        uint64_t best = UINT64_MAX;
        for (size_t i = 0; i < c->n; i++) {
            if (strcmp(c->e[i].key, keep) != 0 && c->e[i].used < best) {
                best = c->e[i].used;
                oldest = i;
            }
        }
        if (oldest == SIZE_MAX) {
            break;
        }
        c->bytes -= tile_bytes(&c->e[oldest].tile);
        free(c->e[oldest].key);
        pc_paint_gray_tile_fini(&c->e[oldest].tile);
        c->e[oldest] = c->e[--c->n];
    }
}

/* The full bitmap of a stored tile (the tile's own when loaded, else cached, else decoded). */
static pc_status load_tile(struct pc_engine_preset_store *st, const pc_paint_stored_tile *t,
                           pc_paint_gray_tile *out, pc_error *err)
{
    if (t->loaded) {
        pc_paint_gray_tile_copy(out, &t->full);
        return PC_OK;
    }
    size_t klen = 0;
    const char *kb = pc_paint_stored_tile_key(t, &klen);
    char key[64];
    size_t kn = klen < sizeof key - 1 ? klen : sizeof key - 1;
    memcpy(key, kb, kn);
    key[kn] = '\0';
    pthread_mutex_lock(&st->cache_lock);
    tip_cache *c = &st->cache;
    c->tick++;
    for (size_t i = 0; i < c->n; i++) {
        if (strcmp(c->e[i].key, key) == 0) {
            c->e[i].used = c->tick;
            pc_paint_gray_tile_copy(out, &c->e[i].tile);
            pthread_mutex_unlock(&st->cache_lock);
            return PC_OK;
        }
    }
    if (tips_find(st, key) == NULL) {
        pthread_mutex_unlock(&st->cache_lock);
        return pc_fail(err, PC_ERR_NOT_FOUND, "brush tip %s is not in the preset store", key);
    }
    pc_buf tf = PC_BUF_INIT, bytes = PC_BUF_INIT;
    pc_status rc = pc_preset_tip_file(key, &tf, err);
    if (rc == PC_OK) {
        rc = st->backend->read(st->backend, pc_buf_cstr(&tf), PC_PRESET_MAX_TIP_BYTES, &bytes, err);
    }
    pc_paint_gray_tile full = PC_PAINT_GRAY_TILE_INIT;
    if (rc == PC_OK) {
        rc = pc_preset_decode_tip(bytes.data, bytes.len, &full, err);
        if (rc != PC_OK) {
            pc_error e2 = {0};
            pc_fail(&e2, rc, "brush tip %s: %s", key, err ? err->msg : "");
            if (err != NULL) {
                *err = e2;
            }
        }
    }
    if (rc == PC_OK && (full.width != t->width || full.height != t->height)) {
        rc = pc_fail(err, PC_ERR_FORMAT, "brush tip %s is %ux%u px, expected %ux%u", key, full.width, full.height,
                     t->width, t->height);
    }
    if (rc == PC_OK) {
        c->tick++;
        c->decodes++;
        void *grown = pc_grow_array(c->e, &c->cap, c->n + 1, sizeof *c->e);
        char *kc = pc_strndup(key, kn);
        if (grown != NULL && kc != NULL) {
            c->e = grown;
            c->e[c->n].key = kc;
            pc_paint_gray_tile_copy(&c->e[c->n].tile, &full);
            c->e[c->n].used = c->tick;
            c->bytes += tile_bytes(&full);
            c->n++;
            cache_evict(c, key);
        } else {
            free(kc);
        }
        pc_paint_gray_tile_copy(out, &full);
    }
    pc_paint_gray_tile_fini(&full);
    pc_buf_fini(&tf);
    pc_buf_fini(&bytes);
    pthread_mutex_unlock(&st->cache_lock);
    return rc;
}

/* ============================================================ session glue */

pc_status pc_engine_session_load_brush_tips(const struct pc_engine_session *s, pc_paint_brush *b, pc_error *err)
{
    struct pc_engine_preset_store *st = s->preset_store;
    pc_paint_tip *tips[2] = {&b->tip, &b->dual_brush.tip};
    for (int i = 0; i < 2; i++) {
        if (tips[i]->kind == PC_PAINT_TIP_STORED && !tips[i]->stored.loaded) {
            if (st == NULL) {
                return pc_fail(err, PC_ERR_NOT_FOUND, "brush tip is not available: no preset store is attached");
            }
            pc_paint_gray_tile full = PC_PAINT_GRAY_TILE_INIT;
            pc_status rc = load_tile(st, &tips[i]->stored, &full, err);
            if (rc != PC_OK) {
                return rc;
            }
            pc_paint_gray_tile_fini(&tips[i]->stored.full);
            tips[i]->stored.full = full;
            tips[i]->stored.loaded = true;
        }
    }
    if (b->texture.pattern.kind == PC_PAINT_PATTERN_STORED && !b->texture.pattern.stored.loaded) {
        if (st == NULL) {
            return pc_fail(err, PC_ERR_NOT_FOUND, "brush tip is not available: no preset store is attached");
        }
        pc_paint_gray_tile full = PC_PAINT_GRAY_TILE_INIT;
        pc_status rc = load_tile(st, &b->texture.pattern.stored, &full, err);
        if (rc != PC_OK) {
            return rc;
        }
        pc_paint_gray_tile_fini(&b->texture.pattern.stored.full);
        b->texture.pattern.stored.full = full;
        b->texture.pattern.stored.loaded = true;
    }
    return PC_OK;
}

/* Turn a just-saved embedded tile of the tool brush into the stored tile the store now holds. */
static void link_one(struct pc_engine_preset_store *st, pc_paint_tip *t)
{
    if (t->kind != PC_PAINT_TIP_SAMPLED || !pc_paint_gray_tile_is_valid(&t->tile)) {
        return;
    }
    char h[33];
    pc_preset_tip_hash(&t->tile, h);
    if (tips_find(st, h) == NULL) {
        return;
    }
    pc_paint_gray_tile preview = PC_PAINT_GRAY_TILE_INIT;
    pc_error e = {0};
    if (pc_paint_gray_tile_preview(&t->tile, PC_PAINT_PREVIEW_SIDE, &preview, &e) != PC_OK) {
        pc_paint_gray_tile_fini(&preview);
        return;
    }
    pc_paint_stored_tile out = {0};
    if (pc_paint_stored_tile_init(h, strlen(h), t->tile.width, t->tile.height, &preview, &t->tile, &out, &e) == PC_OK) {
        pc_paint_tip_fini(t);
        memset(t, 0, sizeof *t);
        t->kind = PC_PAINT_TIP_STORED;
        t->stored = out;
    } else {
        pc_paint_stored_tile_fini(&out);
    }
    pc_paint_gray_tile_fini(&preview);
}

static void link_pattern(struct pc_engine_preset_store *st, pc_paint_pattern *p)
{
    if (p->kind != PC_PAINT_PATTERN_TILE || !pc_paint_gray_tile_is_valid(&p->tile)) {
        return;
    }
    char h[33];
    pc_preset_tip_hash(&p->tile, h);
    if (tips_find(st, h) == NULL) {
        return;
    }
    pc_paint_gray_tile preview = PC_PAINT_GRAY_TILE_INIT;
    pc_error e = {0};
    if (pc_paint_gray_tile_preview(&p->tile, PC_PAINT_PREVIEW_SIDE, &preview, &e) != PC_OK) {
        pc_paint_gray_tile_fini(&preview);
        return;
    }
    pc_paint_stored_tile out = {0};
    if (pc_paint_stored_tile_init(h, strlen(h), p->tile.width, p->tile.height, &preview, &p->tile, &out, &e) == PC_OK) {
        pc_paint_pattern_fini(p);
        memset(p, 0, sizeof *p);
        p->kind = PC_PAINT_PATTERN_STORED;
        p->stored = out;
    } else {
        pc_paint_stored_tile_fini(&out);
    }
    pc_paint_gray_tile_fini(&preview);
}

static void link_stored_tips(struct pc_engine_preset_store *st, pc_paint_brush *b)
{
    link_one(st, &b->tip);
    link_one(st, &b->dual_brush.tip);
    link_pattern(st, &b->texture.pattern);
}

/* HOOK SEAM (real body, replacing the E1/E2a no-op): sync the persistent store after a command
 * changed the brush presets. Actions (actions.json) need E12; that half is a documented stub. */
void pc_engine_hook_sync_preset_store(pc_engine_session *s)
{
    struct pc_engine_preset_store *st = s->preset_store;
    if (st == NULL) {
        return;
    }
    uint64_t rev = s->tools.presets_rev;
    if (!st->has_synced_rev || st->synced_rev != rev) {
        store_sync(st, s->tools.presets, s->tools.npresets);
        st->synced_rev = rev;
        st->has_synced_rev = true;
        link_stored_tips(st, &s->tools.brush);
    }
    /* E12: sync_actions(actions.json) goes here once the session has an Actions list. */
}

/* ---- grow the session's preset library (no cap field: realloc to size) ---- */
static bool lib_reserve(pc_engine_tool_state *t, size_t extra)
{
    pc_paint_preset *grown = pc_malloc_array(t->npresets + extra, sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    if (t->npresets > 0) {
        memcpy(grown, t->presets, t->npresets * sizeof *grown);
    }
    free(t->presets);
    t->presets = grown;
    return true;
}

pc_status pc_engine_session_attach_preset_store(struct pc_engine_session *s, pc_engine_preset_opened *opened,
                                                pc_preset_strs *warnings_out, pc_error *err)
{
    memset(warnings_out, 0, sizeof *warnings_out);
    pc_engine_tool_state *t = &s->tools;

    /* 1. Drop built-ins the user deleted. */
    size_t w = 0;
    for (size_t i = 0; i < t->npresets; i++) {
        bool hide = false;
        if (t->presets[i].builtin) {
            for (size_t j = 0; j < opened->hidden_builtins.len && !hide; j++) {
                hide = name_eq_ci(pc_str_c(&t->presets[i].name), opened->hidden_builtins.v[j]);
            }
        }
        if (hide) {
            pc_paint_preset_fini(&t->presets[i]);
        } else {
            t->presets[w++] = t->presets[i];
        }
    }
    t->npresets = w;

    /* 2. Merge loaded presets: replace a same-name built-in, keep an existing user preset, else add. */
    for (size_t i = 0; i < opened->presets.len; i++) {
        pc_paint_preset *p = &opened->presets.v[i];
        size_t found = SIZE_MAX;
        for (size_t j = 0; j < t->npresets; j++) {
            if (name_eq_ci(pc_str_c(&t->presets[j].name), pc_str_c(&p->name))) {
                found = j;
                break;
            }
        }
        if (found != SIZE_MAX) {
            if (t->presets[found].builtin) {
                pc_paint_preset_fini(&t->presets[found]);
                memset(&t->presets[found], 0, sizeof t->presets[found]);
                if (pc_paint_preset_clone(p, &t->presets[found], err) != PC_OK) {
                    return PC_ERR_NOMEM;
                }
            }
        } else if (lib_reserve(t, 1)) {
            memset(&t->presets[t->npresets], 0, sizeof t->presets[t->npresets]);
            if (pc_paint_preset_clone(p, &t->presets[t->npresets], err) == PC_OK) {
                t->npresets++;
            }
        }
    }

    /* 3. Apply the saved library order (stable; unlisted presets keep their order, at the end). */
    if (opened->order.len > 0) {
        for (size_t a = 0; a + 1 < t->npresets; a++) {
            for (size_t b = a + 1; b < t->npresets; b++) {
                size_t pa = SIZE_MAX, pb = SIZE_MAX;
                for (size_t k = 0; k < opened->order.len; k++) {
                    if (pa == SIZE_MAX && name_eq_ci(pc_str_c(&t->presets[a].name), opened->order.v[k])) {
                        pa = k;
                    }
                    if (pb == SIZE_MAX && name_eq_ci(pc_str_c(&t->presets[b].name), opened->order.v[k])) {
                        pb = k;
                    }
                }
                if (pa > pb) {
                    pc_paint_preset tmp = t->presets[a];
                    t->presets[a] = t->presets[b];
                    t->presets[b] = tmp;
                }
            }
        }
    }

    /* 4. Attach the store and sync. */
    s->preset_store = opened->store;
    opened->store = NULL;
    t->presets_rev++;
    pc_engine_hook_sync_preset_store(s);

    /* 5. Warnings = load warnings + sync warnings. */
    for (size_t i = 0; i < opened->warnings.len; i++) {
        strs_push(warnings_out, opened->warnings.v[i]);
    }
    char **sw = NULL;
    size_t nsw = pc_engine_preset_store_take_warnings(s->preset_store, &sw);
    for (size_t i = 0; i < nsw; i++) {
        strs_push(warnings_out, sw[i]);
        free(sw[i]);
    }
    free(sw);

    /* Consume the rest of `opened`. */
    pc_paint_preset_list_fini(&opened->presets);
    pc_preset_strs_fini(&opened->hidden_builtins);
    pc_preset_strs_fini(&opened->order);
    pc_preset_strs_fini(&opened->warnings);
    memset(opened, 0, sizeof *opened);
    return PC_OK;
}
