/* realpath() is an XSI interface in POSIX.1-2008 (D21): the directory cache is keyed by the
 * canonical folder, as the reference's std::fs::canonicalize. */
#define _XOPEN_SOURCE 700

/* The incremental writer (store.rs PcraftWriter): prepare, ZIP saves reusing compressed
 * objects, directory saves with the verification cache and its rolling re-check. */
#include "format/bridge.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/compress.h"
#include "base/file.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/num.h"
#include "base/pool.h"
#include "format/bundle_dir.h"
#include "format/pathmap.h"

#if defined(__APPLE__)
#define PC_ST_MTIM(st) ((st).st_mtimespec)
#define PC_ST_CTIM(st) ((st).st_ctimespec)
#else
#define PC_ST_MTIM(st) ((st).st_mtim)
#define PC_ST_CTIM(st) ((st).st_ctim)
#endif

struct pc_pcraft_writer {
    pc_format_hash_cache *cache;
    pc_pathmap compressed; /* ZIP mode: path -> compressed pc_blob from the previous save */
    char *vdir;            /* canonical folder of the verification cache; NULL = none yet */
    uint64_t generation;   /* directory saves so far in vdir */
    pc_pathmap verified;   /* path -> signature and the generation that last read it fully */
    uint64_t reverify_budget;
};

static void release_compressed(pc_pathmap *m)
{
    for (size_t i = 0; i < m->cap; i++) {
        if (m->slots[i].used) {
            pc_blob_release(m->slots[i].ptr);
        }
    }
    pc_pathmap_fini(m);
}

pc_status pc_pcraft_writer_new(pc_pcraft_writer **out, pc_error *err)
{
    pc_pcraft_writer *w = calloc(1, sizeof *w);
    pc_status s;
    if (w == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s = pc_format_hash_cache_new(&w->cache, err);
    if (s != PC_OK) {
        free(w);
        return s;
    }
    w->reverify_budget = PC_FORMAT_DEFAULT_REVERIFY_BUDGET;
    *out = w;
    return PC_OK;
}

void pc_pcraft_writer_free(pc_pcraft_writer *w)
{
    if (w != NULL) {
        pc_format_hash_cache_free(w->cache);
        release_compressed(&w->compressed);
        pc_pathmap_fini(&w->verified);
        free(w->vdir);
        free(w);
    }
}

void pc_pcraft_writer_set_reverify_budget(pc_pcraft_writer *w, uint64_t bytes)
{
    w->reverify_budget = bytes;
}

/* ---- prepare ---- */

pc_status pc_pcraft_writer_prepare(pc_pcraft_writer *w, const pc_document *doc,
                                   const pc_format_save_opts *opts, pc_format_prepared *out, pc_error *err)
{
    static const pc_format_save_opts none = {NULL, NULL};
    pc_format_sink *sink = NULL;
    yyjson_mut_doc *jdoc = NULL;
    yyjson_mut_val *root = NULL;
    pc_buf previews[2] = {PC_BUF_INIT, PC_BUF_INIT};
    const char *names[2] = {NULL, NULL};
    size_t npreviews = 0;
    pc_buf json = PC_BUF_INIT;
    pc_format_prepared p;
    pc_status s;

    if (opts == NULL) {
        opts = &none;
    }
    memset(&p, 0, sizeof p);
    s = pc_format_check_nesting(doc, err);
    if (s != PC_OK) {
        return s;
    }
    s = pc_format_sink_new(w->cache, &sink, err);
    if (s != PC_OK) {
        return s;
    }
    jdoc = yyjson_mut_doc_new(NULL);
    if (jdoc == NULL) {
        s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    pc_format_hash_cache_begin(w->cache);
    s = pc_format_manifest_build(doc, opts->thumbnail != NULL ? PC_FORMAT_THUMB : NULL,
                                 opts->composite != NULL ? PC_FORMAT_COMPOSITE : NULL, sink, jdoc, &root,
                                 err);
    pc_format_hash_cache_end(w->cache);
    if (s != PC_OK) {
        goto cleanup;
    }
    if (opts->thumbnail != NULL) {
        names[npreviews] = PC_FORMAT_THUMB;
        if ((s = pc_format_png(opts->thumbnail, &previews[npreviews++], err)) != PC_OK) {
            goto cleanup;
        }
    }
    if (opts->composite != NULL) {
        names[npreviews] = PC_FORMAT_COMPOSITE;
        if ((s = pc_format_png(opts->composite, &previews[npreviews++], err)) != PC_OK) {
            goto cleanup;
        }
    }
    /* serde_json writes NaN and the infinities as null, which the manifest's float fields refuse
     * on load (issue #1101): refuse the save instead, naming the value. */
    if ((s = pc_format_check_finite(root, NULL, err)) != PC_OK) {
        goto cleanup;
    }
    if ((s = pc_json_write(root, PC_JSON_PRETTY, &json, err)) != PC_OK) {
        goto cleanup;
    }
    if ((s = pc_format_check_manifest_depth(json.data, json.len, err)) != PC_OK) {
        goto cleanup;
    }
    if ((s = pc_format_prepared_from_sink(sink, json.data, json.len, &p, err)) != PC_OK) {
        goto cleanup;
    }
    for (size_t i = 0; i < npreviews; i++) {
        p.previews[i].name = names[i];
        p.previews[i].png = previews[i];
        pc_buf_init(&previews[i]);
    }
    p.npreviews = npreviews;
    *out = p;
cleanup:
    pc_buf_fini(&previews[0]);
    pc_buf_fini(&previews[1]);
    pc_buf_fini(&json);
    yyjson_mut_doc_free(jdoc);
    pc_format_sink_free(sink);
    return s;
}

/* ---- compression ---- */

static pc_status compress_object(const pc_format_object *o, pc_blob **out, pc_error *err)
{
    pc_buf tmp = PC_BUF_INIT;
    pc_buf z = PC_BUF_INIT;
    const uint8_t *data;
    size_t len;
    pc_status s = pc_format_object_content(o, &tmp, &data, &len, err);
    if (s == PC_OK) {
        /* Level 1 is the reference's CompressionLevel::Fastest. */
        s = pc_zstd_compress(1, data, len, &z, err);
    }
    if (s == PC_OK) {
        s = pc_blob_new(z.data, z.len, out, err);
    }
    pc_buf_fini(&tmp);
    pc_buf_fini(&z);
    return s;
}

typedef struct job {
    const pc_format_object *obj;
    pc_blob *out;
    pc_status status;
    pc_error err;
} job;

static pc_status compress_chunk(void *ctx, size_t chunk, size_t begin, size_t end)
{
    job *jobs = ctx;
    pc_status first = PC_OK;
    (void)chunk;
    for (size_t i = begin; i < end; i++) {
        jobs[i].status = compress_object(jobs[i].obj, &jobs[i].out, &jobs[i].err);
        if (jobs[i].status != PC_OK && first == PC_OK) {
            first = jobs[i].status;
        }
    }
    return first;
}

/* Compresses the objects in jobs on all cores (zstd dominates a full save). On failure the first
 * failing job's error is reported. */
static pc_status par_compress(job *jobs, size_t n, pc_error *err)
{
    (void)pc_pool_for(n >= 2 ? pc_pool_global() : NULL, n, 1, compress_chunk, jobs);
    for (size_t i = 0; i < n; i++) {
        if (jobs[i].status != PC_OK) {
            if (err != NULL) {
                *err = jobs[i].err;
            }
            return jobs[i].status;
        }
    }
    return PC_OK;
}

/* ---- ZIP saves ---- */

pc_status pc_pcraft_writer_save_zip_prepared(pc_pcraft_writer *w, const pc_format_prepared *p, pc_buf *out,
                                             pc_format_save_stats *stats_out, pc_error *err)
{
    pc_format_save_stats stats = p->stats;
    pc_zip_writer z;
    pc_pathmap next = PC_PATHMAP_INIT;
    job *jobs = NULL;
    size_t *job_of = NULL; /* object index -> job index + 1 (0 = reused) */
    size_t njobs = 0;
    pc_buf bytes = PC_BUF_INIT;
    pc_status s;

    pc_zip_writer_init(&z);
    s = pc_zip_writer_add(&z, PC_FORMAT_MANIFEST, strlen(PC_FORMAT_MANIFEST), p->manifest.data,
                          p->manifest.len, err);
    for (size_t i = 0; s == PC_OK && i < p->npreviews; i++) {
        const pc_format_preview *pv = &p->previews[i];
        s = pc_zip_writer_add(&z, pv->name, strlen(pv->name), pv->png.data, pv->png.len, err);
    }
    if (s != PC_OK) {
        goto cleanup;
    }
    if (p->nobjects > 0) {
        jobs = pc_calloc_array(p->nobjects, sizeof *jobs);
        job_of = pc_calloc_array(p->nobjects, sizeof *job_of);
        if (jobs == NULL || job_of == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto cleanup;
        }
    }
    for (size_t i = 0; i < p->nobjects; i++) {
        if (pc_pathmap_get(&w->compressed, p->objects[i].path) == NULL) {
            jobs[njobs].obj = &p->objects[i];
            job_of[i] = ++njobs;
        }
    }
    if ((s = par_compress(jobs, njobs, err)) != PC_OK) {
        goto cleanup;
    }
    for (size_t i = 0; i < p->nobjects; i++) {
        const pc_format_object *o = &p->objects[i];
        pc_pathmap_ent *e;
        pc_blob *data;
        bool inserted;
        if (job_of[i] == 0) {
            data = pc_pathmap_get(&w->compressed, o->path)->ptr;
            if (o->is_tile) {
                stats.tiles_reused++;
            }
        } else {
            data = jobs[job_of[i] - 1].out;
            if (o->is_tile) {
                stats.tiles_written++;
            } else {
                stats.blobs_written++;
            }
        }
        s = pc_zip_writer_add(&z, o->path, strlen(o->path), pc_blob_data(data), pc_blob_len(data), err);
        if (s == PC_OK) {
            s = pc_pathmap_put(&next, o->path, &e, &inserted, err);
        }
        if (s != PC_OK) {
            goto cleanup;
        }
        if (inserted) {
            e->ptr = pc_blob_retain(data);
        }
    }
    if ((s = pc_zip_writer_finish(&z, &bytes, err)) != PC_OK) {
        goto cleanup;
    }
    release_compressed(&w->compressed);
    w->compressed = next;
    next = (pc_pathmap)PC_PATHMAP_INIT;
    pc_buf_fini(out);
    *out = bytes;
    if (stats_out != NULL) {
        *stats_out = stats;
    }
cleanup:
    for (size_t i = 0; i < njobs; i++) {
        pc_blob_release(jobs[i].out);
    }
    free(jobs);
    free(job_of);
    release_compressed(&next);
    pc_zip_writer_fini(&z);
    return s;
}

/* ---- directory saves ---- */

static pc_status join(const char *dir, const char *rel, pc_buf *out, pc_error *err)
{
    size_t dl = strlen(dir);
    pc_buf_truncate(out, 0);
    return pc_buf_appendf(out, err, "%s%s%s", dir, dl > 0 && dir[dl - 1] == '/' ? "" : "/", rel);
}

static pc_status io_errno(pc_error *err, int e, const char *what, const char *path)
{
    pc_status s =
        pc_fail(err, e == ENOENT ? PC_ERR_NOT_FOUND : PC_ERR_IO, "%s %s: %s", what, path, strerror(e));
    if (err != NULL) {
        err->sys_errno = e;
    }
    return pc_format_io(s, err);
}

static void sig_from_stat(const struct stat *st, pc_format_sig *sig)
{
    memset(sig, 0, sizeof *sig);
    sig->len = st->st_size > 0 ? (uint64_t)st->st_size : 0;
    sig->mtime_s = (int64_t)PC_ST_MTIM(*st).tv_sec;
    sig->mtime_ns = PC_ST_MTIM(*st).tv_nsec;
    sig->ino = (uint64_t)st->st_ino;
    sig->dev = (uint64_t)st->st_dev;
    sig->ctime_s = (int64_t)PC_ST_CTIM(*st).tv_sec;
    sig->ctime_ns = PC_ST_CTIM(*st).tv_nsec;
}

/* *present = a regular file is there (its signature in *sig); missing is not an error, anything
 * else there (a symlink, a directory) is refused (store.rs path_signature). */
static pc_status path_signature(const char *path, bool *present, pc_format_sig *sig, pc_error *err)
{
    struct stat st;
    *present = false;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            return PC_OK;
        }
        return io_errno(err, errno, "cannot inspect", path);
    }
    if (!S_ISREG(st.st_mode)) {
        return pc_bundle_unsafe_entry(path, err);
    }
    *present = true;
    sig_from_stat(&st, sig);
    return PC_OK;
}

/* Is the existing object file a valid compressed copy of o? (*valid, and its signature in *sig
 * when *has_sig.) Only I/O failures other than "missing" are errors. */
static pc_status existing_object_is_valid(const char *path, const pc_format_object *o, bool *valid,
                                          bool *has_sig, pc_format_sig *sig, pc_error *err)
{
    pc_buf tmp = PC_BUF_INIT;
    pc_buf compressed = PC_BUF_INIT;
    pc_buf actual = PC_BUF_INIT;
    const uint8_t *expected;
    size_t expected_len;
    size_t max;
    struct stat st;
    pc_status s;
    int fd;

    *valid = false;
    *has_sig = false;
    do {
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return errno == ENOENT ? PC_OK : io_errno(err, errno, "cannot open", path);
    }
    if (fstat(fd, &st) != 0) {
        s = io_errno(err, errno, "cannot inspect", path);
        goto done;
    }
    s = PC_OK;
    if (!S_ISREG(st.st_mode)) {
        goto done;
    }
    sig_from_stat(&st, sig);
    if ((s = pc_format_object_content(o, &tmp, &expected, &expected_len, err)) != PC_OK) {
        goto done;
    }
    /* The tile loader's bound: comfortably includes zstd framing while keeping damaged files
     * bounded when inspected. */
    if (!pc_size_add(expected_len, expected_len / 8, &max) || !pc_size_add(max, 4096, &max)) {
        max = SIZE_MAX;
    }
    if (st.st_size > 0 && (uintmax_t)st.st_size > (uintmax_t)max) {
        goto done;
    }
    s = pc_fd_read_all(fd, max, &compressed, err);
    if (s == PC_ERR_LIMIT) {
        s = PC_OK;
        goto done;
    }
    if (s != PC_OK) {
        s = s == PC_ERR_NOMEM ? s : pc_format_io(s, err);
        goto done;
    }
    *has_sig = true;
    {
        pc_error ignored;
        pc_status d = pc_zstd_decompress(compressed.data, compressed.len,
                                         expected_len == SIZE_MAX ? expected_len : expected_len + 1, 0,
                                         &actual, NULL, &ignored);
        if (d == PC_ERR_NOMEM) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto done;
        }
        *valid = d == PC_OK && actual.len == expected_len &&
                 (expected_len == 0 || memcmp(actual.data, expected, expected_len) == 0);
    }
done:
    /* Oversize, unread or non-regular files are simply "not valid", with no signature. */
    pc_buf_fini(&tmp);
    pc_buf_fini(&compressed);
    pc_buf_fini(&actual);
    (void)close(fd);
    return s;
}

static int str_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

typedef struct names {
    char **v;
    size_t n;
    size_t cap;
} names;

static void names_free(names *ns)
{
    for (size_t i = 0; i < ns->n; i++) {
        free(ns->v[i]);
    }
    free(ns->v);
    memset(ns, 0, sizeof *ns);
}

static bool names_contains(const names *ns, const char *path)
{
    return ns->n > 0 && bsearch(&path, ns->v, ns->n, sizeof *ns->v, str_cmp) != NULL;
}

/* Every "tiles/<name>.zst" and "blobs/<name>.zst" in dir, sorted. */
static pc_status list_objects(const char *dir, names *out, pc_error *err)
{
    static const char *const SUBS[2] = {"tiles", "blobs"};
    names ns = {NULL, 0, 0};
    pc_buf path = PC_BUF_INIT;
    pc_status s = PC_OK;

    for (size_t k = 0; k < 2 && s == PC_OK; k++) {
        DIR *d;
        struct dirent *de;
        char *canon;
        if ((s = join(dir, SUBS[k], &path, err)) != PC_OK ||
            (s = pc_bundle_plain_directory_or_missing(pc_buf_cstr(&path), err)) != PC_OK) {
            break;
        }
        canon = realpath(pc_buf_cstr(&path), NULL);
        if (canon == NULL) {
            s = errno == ENOMEM ? pc_fail(err, PC_ERR_NOMEM, "out of memory")
                                : io_errno(err, errno, "cannot resolve", pc_buf_cstr(&path));
            break;
        }
        if (strcmp(canon, pc_buf_cstr(&path)) != 0) {
            free(canon);
            s = pc_bundle_unsafe_entry(pc_buf_cstr(&path), err);
            break;
        }
        free(canon);
        d = opendir(pc_buf_cstr(&path));
        if (d == NULL) {
            s = io_errno(err, errno, "cannot list", pc_buf_cstr(&path));
            break;
        }
        for (;;) {
            size_t nl;
            char *entry;
            errno = 0;
            de = readdir(d);
            if (de == NULL) {
                if (errno != 0) {
                    s = io_errno(err, errno, "cannot list", pc_buf_cstr(&path));
                }
                break;
            }
            nl = strlen(de->d_name);
            if (nl < 4 || memcmp(de->d_name + nl - 4, ".zst", 4) != 0) {
                continue;
            }
            {
                /* A symlink is refused; stale directories (remove can't take them) and vanished
                 * entries are left alone without failing the save. */
                pc_buf full = PC_BUF_INIT;
                bool exists = false;
                bool is_link = false;
                bool is_file = false;
                s = join(pc_buf_cstr(&path), de->d_name, &full, err);
                if (s == PC_OK) {
                    s = pc_bundle_lstat(pc_buf_cstr(&full), &exists, &is_link, &is_file, err);
                }
                if (s == PC_OK && is_link) {
                    s = pc_bundle_unsafe_entry(pc_buf_cstr(&full), err);
                }
                pc_buf_fini(&full);
                if (s != PC_OK) {
                    break;
                }
                if (!is_file) {
                    continue;
                }
            }
            if (ns.n == ns.cap) {
                char **grown = pc_grow_array(ns.v, &ns.cap, ns.n + 1, sizeof *grown);
                if (grown == NULL) {
                    s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    break;
                }
                ns.v = grown;
            }
            entry = pc_malloc(nl + 7);
            if (entry == NULL) {
                s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                break;
            }
            memcpy(entry, SUBS[k], 5);
            entry[5] = '/';
            memcpy(entry + 6, de->d_name, nl + 1);
            ns.v[ns.n++] = entry;
        }
        (void)closedir(d);
    }
    pc_buf_fini(&path);
    if (s != PC_OK) {
        names_free(&ns);
        return s;
    }
    if (ns.n > 1) {
        qsort(ns.v, ns.n, sizeof *ns.v, str_cmp);
    }
    *out = ns;
    return PC_OK;
}

static int object_path_cmp(const void *key, const void *elem)
{
    return strcmp(key, ((const pc_format_object *)elem)->path);
}

static const pc_format_object *find_object(const pc_format_prepared *p, const char *path)
{
    return p->nobjects > 0 ? bsearch(path, p->objects, p->nobjects, sizeof *p->objects, object_path_cmp)
                           : NULL;
}

typedef struct candidate {
    const char *path;
    uint64_t checked_at;
    uint64_t len;
} candidate;

static int candidate_cmp(const void *a, const void *b)
{
    const candidate *x = a;
    const candidate *y = b;
    if (x->checked_at != y->checked_at) {
        return x->checked_at < y->checked_at ? -1 : 1;
    }
    return strcmp(x->path, y->path);
}

/* The cached objects this save re-verifies although their signature is unchanged: oldest-checked
 * first, until their recorded sizes reach the budget (always at least one, so an object larger
 * than the budget is still reached in turn). recheck[i] is set for p->objects[i]. */
static pc_status rolling_recheck(const pc_pcraft_writer *w, const pc_format_prepared *p, bool *recheck,
                                 pc_error *err)
{
    candidate *c;
    size_t n = 0;
    uint64_t spent = 0;
    bool any = false;
    if (w->vdir == NULL || w->reverify_budget == 0 || w->verified.count == 0) {
        return PC_OK;
    }
    c = pc_malloc_array(w->verified.count, sizeof *c);
    if (c == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < w->verified.cap; i++) {
        const pc_pathmap_ent *e = &w->verified.slots[i];
        if (e->used && find_object(p, e->key) != NULL) {
            c[n].path = e->key;
            c[n].checked_at = e->checked_at;
            c[n].len = e->sig.len;
            n++;
        }
    }
    if (n > 1) {
        qsort(c, n, sizeof *c, candidate_cmp);
    }
    for (size_t i = 0; i < n; i++) {
        uint64_t next = c[i].len > UINT64_MAX - spent ? UINT64_MAX : spent + c[i].len;
        if (any && next > w->reverify_budget) {
            break;
        }
        spent = next;
        any = true;
        recheck[find_object(p, c[i].path) - p->objects] = true;
    }
    free(c);
    return PC_OK;
}

static pc_status remember(pc_pcraft_writer *w, const char *path, const pc_format_sig *sig,
                          uint64_t generation, pc_error *err)
{
    pc_pathmap_ent *e;
    bool inserted;
    pc_status s = pc_pathmap_put(&w->verified, path, &e, &inserted, err);
    if (s == PC_OK) {
        e->sig = *sig;
        e->checked_at = generation;
    }
    return s;
}

/* store.rs save_dir before prepare: the canonical bundle root (created with its folders), its
 * existing objects, and the fixed entries checked, so a hostile bundle is refused before anything
 * is written. */
static pc_status dir_setup(const char *dir, char **root, names *existing, pc_error *err)
{
    static const char *const FIXED[3] = {PC_FORMAT_MANIFEST, PC_FORMAT_THUMB, PC_FORMAT_COMPOSITE};
    pc_buf path = PC_BUF_INIT;
    pc_status s = pc_bundle_root(dir, root, err);
    if (s != PC_OK) {
        return s;
    }
    s = list_objects(*root, existing, err);
    for (size_t k = 0; k < 3 && s == PC_OK; k++) {
        s = join(*root, FIXED[k], &path, err);
        if (s == PC_OK) {
            s = pc_bundle_validate_file(*root, pc_buf_cstr(&path), err);
        }
    }
    pc_buf_fini(&path);
    if (s != PC_OK) {
        names_free(existing);
        free(*root);
        *root = NULL;
    }
    return s;
}

/* The rest of save_dir: objects, previews, the manifest (committing the save), then best-effort
 * garbage collection. Takes over root. */
static pc_status dir_save(pc_pcraft_writer *w, const pc_format_prepared *p, char *root, names *existing,
                          pc_format_save_stats *stats_out, pc_error *err)
{
    pc_format_save_stats stats = p->stats;
    pc_buf path = PC_BUF_INIT;
    bool *recheck = NULL;
    uint64_t generation;
    pc_status s = PC_OK;

    if (w->vdir == NULL || strcmp(w->vdir, root) != 0) {
        free(w->vdir);
        w->vdir = root;
        root = NULL;
        w->generation = 0;
        pc_pathmap_fini(&w->verified);
    }
    generation = w->generation = w->generation == UINT64_MAX ? UINT64_MAX : w->generation + 1;
    if (p->nobjects > 0) {
        recheck = pc_calloc_array(p->nobjects, sizeof *recheck);
        if (recheck == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            goto cleanup;
        }
    }
    if ((s = rolling_recheck(w, p, recheck, err)) != PC_OK) {
        goto cleanup;
    }
    for (size_t i = 0; i < p->nobjects; i++) {
        const pc_format_object *o = &p->objects[i];
        bool exists = names_contains(existing, o->path);
        bool present = false;
        pc_format_sig sig;
        pc_blob *z = NULL;
        if ((s = join(w->vdir, o->path, &path, err)) != PC_OK ||
            (s = pc_bundle_plain_file_or_missing(pc_buf_cstr(&path), err)) != PC_OK ||
            (s = path_signature(pc_buf_cstr(&path), &present, &sig, err)) != PC_OK) {
            goto cleanup;
        }
        if (exists && !recheck[i] && present) {
            const pc_pathmap_ent *v = pc_pathmap_get(&w->verified, o->path);
            if (v != NULL && pc_format_sig_eq(&v->sig, &sig)) {
                if (o->is_tile) {
                    stats.tiles_reused++;
                }
                continue;
            }
        }
        if (exists) {
            bool valid;
            bool has_sig;
            pc_format_sig vsig;
            s = existing_object_is_valid(pc_buf_cstr(&path), o, &valid, &has_sig, &vsig, err);
            if (s != PC_OK) {
                goto cleanup;
            }
            if (valid) {
                if (has_sig && (s = remember(w, o->path, &vsig, generation, err)) != PC_OK) {
                    goto cleanup;
                }
                if (o->is_tile) {
                    stats.tiles_reused++;
                }
                continue;
            }
        }
        if (o->is_tile) {
            stats.tiles_written++;
        } else {
            stats.blobs_written++;
        }
        if ((s = compress_object(o, &z, err)) == PC_OK) {
            s = pc_bundle_write_file(w->vdir, pc_buf_cstr(&path), pc_blob_data(z), pc_blob_len(z), err);
        }
        pc_blob_release(z);
        if (s != PC_OK || (s = path_signature(pc_buf_cstr(&path), &present, &sig, err)) != PC_OK) {
            goto cleanup;
        }
        if (present) {
            if ((s = remember(w, o->path, &sig, generation, err)) != PC_OK) {
                goto cleanup;
            }
        } else {
            (void)pc_pathmap_remove(&w->verified, o->path, NULL);
        }
    }
    for (size_t i = 0; i < p->npreviews; i++) {
        if ((s = join(w->vdir, p->previews[i].name, &path, err)) != PC_OK ||
            (s = pc_bundle_write_file(w->vdir, pc_buf_cstr(&path), p->previews[i].png.data,
                                      p->previews[i].png.len, err)) != PC_OK) {
            goto cleanup;
        }
    }
    {
        static const char *const STALE[2] = {PC_FORMAT_THUMB, PC_FORMAT_COMPOSITE};
        bool gone = false; /* a missing stale preview is fine */
        for (size_t k = 0; k < 2; k++) {
            bool kept = false;
            for (size_t i = 0; i < p->npreviews; i++) {
                kept = kept || strcmp(p->previews[i].name, STALE[k]) == 0;
            }
            if (!kept && ((s = join(w->vdir, STALE[k], &path, err)) != PC_OK ||
                          (s = pc_bundle_remove_file(w->vdir, pc_buf_cstr(&path), &gone, err)) != PC_OK)) {
                goto cleanup;
            }
        }
    }
    if ((s = join(w->vdir, PC_FORMAT_MANIFEST, &path, err)) != PC_OK ||
        (s = pc_bundle_write_file(w->vdir, pc_buf_cstr(&path), p->manifest.data, p->manifest.len, err)) !=
            PC_OK) {
        goto cleanup;
    }
    /* The manifest is committed: the save has succeeded. Removing objects it no longer references
     * is best-effort housekeeping (a vanished or locked file must not turn a complete save into a
     * failure); a leftover is retried on the next save. */
    for (size_t i = 0; i < existing->n; i++) {
        if (find_object(p, existing->v[i]) != NULL) {
            continue;
        }
        if (join(w->vdir, existing->v[i], &path, NULL) == PC_OK) {
            bool missing = false;
            if (pc_bundle_remove_file(w->vdir, pc_buf_cstr(&path), &missing, NULL) == PC_OK && !missing) {
                stats.objects_removed++;
            }
        }
        if (strlen(existing->v[i]) < PC_PATHMAP_KEY_MAX) {
            (void)pc_pathmap_remove(&w->verified, existing->v[i], NULL);
        }
    }
    if (stats_out != NULL) {
        *stats_out = stats;
    }
cleanup:
    free(root);
    free(recheck);
    pc_buf_fini(&path);
    return s;
}

pc_status pc_pcraft_writer_save_dir_prepared(pc_pcraft_writer *w, const pc_format_prepared *p,
                                             const char *dir, pc_format_save_stats *stats_out, pc_error *err)
{
    names existing = {NULL, 0, 0};
    char *root = NULL;
    pc_status s = dir_setup(dir, &root, &existing, err);
    if (s == PC_OK) {
        s = dir_save(w, p, root, &existing, stats_out, err);
        names_free(&existing);
    }
    return s;
}

/* ---- the document-level entry points ---- */

pc_status pc_pcraft_writer_save_zip(pc_pcraft_writer *w, const pc_document *doc,
                                    const pc_format_save_opts *opts, pc_buf *out, pc_format_save_stats *stats,
                                    pc_error *err)
{
    pc_format_prepared p;
    pc_status s = pc_pcraft_writer_prepare(w, doc, opts, &p, err);
    if (s != PC_OK) {
        return s;
    }
    s = pc_pcraft_writer_save_zip_prepared(w, &p, out, stats, err);
    pc_format_prepared_fini(&p);
    return s;
}

pc_status pc_pcraft_writer_save_dir(pc_pcraft_writer *w, const pc_document *doc, const char *dir,
                                    const pc_format_save_opts *opts, pc_format_save_stats *stats,
                                    pc_error *err)
{
    pc_format_prepared p;
    names existing = {NULL, 0, 0};
    char *root = NULL;
    pc_status s = dir_setup(dir, &root, &existing, err);
    if (s != PC_OK) {
        return s;
    }
    s = pc_pcraft_writer_prepare(w, doc, opts, &p, err);
    if (s != PC_OK) {
        free(root);
        names_free(&existing);
        return s;
    }
    s = dir_save(w, &p, root, &existing, stats, err);
    names_free(&existing);
    pc_format_prepared_fini(&p);
    return s;
}

pc_status pc_pcraft_writer_save_path(pc_pcraft_writer *w, const pc_document *doc, const char *path,
                                     const pc_format_save_opts *opts, pc_format_save_stats *stats,
                                     pc_error *err)
{
    struct stat st;
    size_t n = strlen(path);
    pc_buf bytes = PC_BUF_INIT;
    pc_format_save_stats st_zip;
    pc_status s;

    if ((stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ||
        (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\'))) {
        return pc_pcraft_writer_save_dir(w, doc, path, opts, stats, err);
    }
    s = pc_pcraft_writer_save_zip(w, doc, opts, &bytes, &st_zip, err);
    if (s == PC_OK) {
        s = pc_format_write_atomic(path, bytes.data, bytes.len, err);
    }
    pc_buf_fini(&bytes);
    if (s == PC_OK && stats != NULL) {
        *stats = st_zip;
    }
    return s;
}
