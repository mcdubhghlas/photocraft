/* Autosave and crash recovery (crates/format/src/autosave.rs). See autosave.h.
 *
 * The worker: one pthread per autosaver, waiting on a condition variable for a pending job.
 * A new request replaces a pending one (the reference drains its channel to the newest
 * snapshot, so skipped snapshots never report an outcome either). Shutting down lets the worker
 * write the pending job first, as dropping the reference's sender does. */
#include "format/autosave.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "base/file.h"
#include "base/json.h"
#include "base/mem.h"
#include "base/utf8.h"
#include "doc/json_util.h"
#include "format/bridge.h"

/* Sidecars are tiny; a larger file is not one of ours (the reference reads any size). */
#define MAX_SIDECAR ((size_t)1 << 20)

/* ---- RecoveryInfo ---- */

static const jfield info_f[] = {
    JFV("key", pc_recovery_info, key, pc_jt_str),
    JFV("document_name", pc_recovery_info, document_name, pc_jt_str),
    JFO("original_path", pc_recovery_info, original_path, has_original_path, pc_jt_str, JD_REQ),
    JFV("saved_at", pc_recovery_info, saved_at, pc_jt_u64),
    JFV("revision", pc_recovery_info, revision, pc_jt_u64),
};
static const jstruct info_s = {"struct RecoveryInfo", info_f, JCOUNT(info_f), NULL, sizeof(pc_recovery_info)};
static const jtype jt_info = JT_STRUCT(pc_recovery_info, info_s);

void pc_recovery_info_fini(pc_recovery_info *i)
{
    if (i != NULL) {
        pc_jfini(&jt_info, i);
    }
}

pc_status pc_recovery_info_to_json(const pc_recovery_info *i, pc_buf *out, pc_error *err)
{
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *v;
    pc_buf b = PC_BUF_INIT;
    pc_status st;
    if (d == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    v = pc_jstruct_write(&info_s, d, i, 0);
    if (v == NULL) {
        yyjson_mut_doc_free(d);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    st = pc_json_write(v, PC_JSON_PRETTY, &b, err);
    yyjson_mut_doc_free(d);
    if (st != PC_OK) {
        pc_buf_fini(&b);
        return st;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

pc_status pc_recovery_info_from_json(const uint8_t *json, size_t len, pc_recovery_info *out, pc_error *err)
{
    yyjson_doc *d = NULL;
    pc_status st = pc_json_parse((const char *)json, len, &d, err);
    if (st == PC_OK) {
        st = pc_jread_into(&jt_info, yyjson_doc_get_root(d), out, err);
    }
    yyjson_doc_free(d);
    return st;
}

/* ---- keys and paths ---- */

static bool key_char_ok(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

pc_status pc_autosave_sanitize_key(const char *key, size_t len, pc_str *out, pc_error *err)
{
    pc_buf b = PC_BUF_INIT;
    size_t i = 0;
    pc_status st = PC_OK;
    while (i < len && st == PC_OK) {
        uint32_t cp = 0;
        if (!pc_utf8_next((const uint8_t *)key, len, &i, &cp)) {
            cp = 0xFFFD; /* one invalid byte stands for one character */
            i++;
        }
        char c = key_char_ok(cp) ? (char)cp : '_';
        st = pc_buf_append(&b, &c, 1, err);
    }
    if (st == PC_OK && b.len == 0) {
        st = pc_buf_append_str(&b, "untitled", err);
    }
    if (st == PC_OK) {
        st = pc_str_set(out, pc_buf_cstr(&b), b.len, err);
    }
    pc_buf_fini(&b);
    return st;
}

/* dir joined with "<key><suffix>" (like Path::join: no doubled separator). */
static char *join(const char *dir, const char *key, size_t key_len, const char *suffix)
{
    size_t dl = strlen(dir);
    bool slash = dl > 0 && dir[dl - 1] == '/';
    pc_buf b = PC_BUF_INIT;
    if (pc_buf_appendf(&b, NULL, "%s%s%.*s%s", dir, slash || dl == 0 ? "" : "/", (int)key_len, key, suffix) !=
        PC_OK) {
        pc_buf_fini(&b);
        return NULL;
    }
    return (char *)pc_buf_take(&b, NULL);
}

/* remove_dir_all without recursion (a symlink is removed, never followed). */
static pc_status remove_tree(const char *root, pc_error *err)
{
    char **stack = NULL;
    size_t n = 0;
    size_t cap = 0;
    struct stat st;
    pc_status s = PC_OK;
    if (lstat(root, &st) != 0) {
        return pc_format_io(pc_fail_errno(err, PC_ERR_IO, errno, "%s", root), err);
    }
    if (!S_ISDIR(st.st_mode)) {
        return pc_format_io(pc_fail_errno(err, PC_ERR_IO, ENOTDIR, "%s", root), err);
    }
    stack = malloc(sizeof *stack * 8);
    if (stack == NULL || (stack[0] = pc_strndup(root, strlen(root))) == NULL) {
        free(stack);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    n = 1;
    cap = 8;
    while (n > 0 && s == PC_OK) {
        const char *top = stack[n - 1];
        DIR *d = opendir(top);
        struct dirent *e;
        char *sub = NULL;
        if (d == NULL) {
            s = pc_format_io(pc_fail_errno(err, PC_ERR_IO, errno, "%s", top), err);
            break;
        }
        while (s == PC_OK && sub == NULL && (e = readdir(d)) != NULL) {
            char *child;
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
                continue;
            }
            child = join(top, e->d_name, strlen(e->d_name), "");
            if (child == NULL) {
                s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            } else if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                sub = child;
            } else {
                if (unlink(child) != 0 && errno != ENOENT) {
                    s = pc_format_io(pc_fail_errno(err, PC_ERR_IO, errno, "%s", child), err);
                }
                free(child);
            }
        }
        (void)closedir(d);
        if (s != PC_OK) {
            free(sub);
            break;
        }
        if (sub != NULL) {
            if (n == cap) {
                char **g = pc_grow_array(stack, &cap, n + 1, sizeof *stack);
                if (g == NULL) {
                    free(sub);
                    s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                    break;
                }
                stack = g;
            }
            stack[n++] = sub;
        } else {
            if (rmdir(top) != 0) {
                s = pc_format_io(pc_fail_errno(err, PC_ERR_IO, errno, "%s", top), err);
                break;
            }
            free(stack[--n]);
        }
    }
    while (n > 0) {
        free(stack[--n]);
    }
    free(stack);
    return s;
}

/* remove_entry: the bundle (if it exists) then the sidecar (if it exists). */
static pc_status remove_entry(const char *dir, const char *key, size_t key_len, pc_error *err)
{
    char *bundle = join(dir, key, key_len, ".pcraft");
    char *sidecar = join(dir, key, key_len, ".json");
    struct stat st;
    pc_status s = PC_OK;
    if (bundle == NULL || sidecar == NULL) {
        s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (s == PC_OK && stat(bundle, &st) == 0) {
        s = remove_tree(bundle, err);
    }
    if (s == PC_OK && stat(sidecar, &st) == 0 && unlink(sidecar) != 0) {
        s = pc_format_io(pc_fail_errno(err, PC_ERR_IO, errno, "%s", sidecar), err);
    }
    free(bundle);
    free(sidecar);
    return s;
}

/* ---- list_recovery ---- */

void pc_recovery_entries_free(pc_recovery_entry *e, size_t n)
{
    if (e == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        pc_recovery_info_fini(&e[i].info);
        free(e[i].bundle);
    }
    free(e);
}

static int entry_cmp(const void *pa, const void *pb)
{
    const pc_recovery_entry *a = pa;
    const pc_recovery_entry *b = pb;
    if (a->info.saved_at != b->info.saved_at) {
        return a->info.saved_at > b->info.saved_at ? -1 : 1;
    }
    return pc_str_cmp(&a->info.key, &b->info.key);
}

/* Path::extension is "json" (a leading dot does not start an extension); *stem_len set. */
static bool json_sidecar(const char *name, size_t *stem_len)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name || strcmp(name, "..") == 0 || strcmp(dot + 1, "json") != 0) {
        return false;
    }
    *stem_len = (size_t)(dot - name);
    return true;
}

/* One directory entry: the parsed entry when it is a valid sidecar with a bundle. */
static pc_status consider(const char *dir, const char *name, pc_recovery_entry *out, bool *found,
                          pc_error *err)
{
    size_t stem_len = 0;
    char *path;
    char *manifest;
    pc_buf bytes = PC_BUF_INIT;
    pc_recovery_info info;
    pc_str clean = PC_STR_INIT;
    struct stat st;
    pc_status s;
    *found = false;
    if (!json_sidecar(name, &stem_len)) {
        return PC_OK;
    }
    path = join(dir, name, strlen(name), "");
    if (path == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s = pc_file_read(path, MAX_SIDECAR, &bytes, NULL);
    free(path);
    if (s != PC_OK) {
        pc_buf_fini(&bytes);
        return s == PC_ERR_NOMEM ? pc_fail(err, PC_ERR_NOMEM, "out of memory") : PC_OK;
    }
    memset(&info, 0, sizeof info);
    s = pc_recovery_info_from_json(bytes.data, bytes.len, &info, NULL);
    pc_buf_fini(&bytes);
    if (s != PC_OK) {
        return s == PC_ERR_NOMEM ? pc_fail(err, PC_ERR_NOMEM, "out of memory") : PC_OK;
    }
    s = pc_autosave_sanitize_key(pc_str_c(&info.key), info.key.len, &clean, err);
    if (s != PC_OK || !pc_str_eq(&clean, &info.key) || !pc_str_eq_mem(&info.key, name, stem_len)) {
        pc_str_fini(&clean);
        pc_recovery_info_fini(&info);
        return s;
    }
    pc_str_fini(&clean);
    out->bundle = join(dir, pc_str_c(&info.key), info.key.len, ".pcraft");
    manifest =
        out->bundle != NULL ? join(out->bundle, PC_FORMAT_MANIFEST, strlen(PC_FORMAT_MANIFEST), "") : NULL;
    if (manifest == NULL) {
        free(out->bundle);
        pc_recovery_info_fini(&info);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (stat(manifest, &st) == 0 && S_ISREG(st.st_mode)) {
        out->info = info;
        *found = true;
    } else {
        free(out->bundle);
        out->bundle = NULL;
        pc_recovery_info_fini(&info);
    }
    free(manifest);
    return PC_OK;
}

pc_status pc_list_recovery(const char *dir, pc_recovery_entry **out, size_t *n, pc_error *err)
{
    pc_recovery_entry *v = NULL;
    size_t len = 0;
    size_t cap = 0;
    pc_status s = PC_OK;
    DIR *d = opendir(dir);
    struct dirent *e;
    if (d == NULL) {
        *out = NULL;
        *n = 0;
        return PC_OK;
    }
    while (s == PC_OK && (e = readdir(d)) != NULL) {
        pc_recovery_entry ent;
        bool found = false;
        memset(&ent, 0, sizeof ent);
        s = consider(dir, e->d_name, &ent, &found, err);
        if (s != PC_OK || !found) {
            continue;
        }
        if (len == cap) {
            pc_recovery_entry *g = pc_grow_array(v, &cap, len + 1, sizeof *v);
            if (g == NULL) {
                pc_recovery_info_fini(&ent.info);
                free(ent.bundle);
                s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
                break;
            }
            v = g;
        }
        v[len++] = ent;
    }
    (void)closedir(d);
    if (s != PC_OK) {
        pc_recovery_entries_free(v, len);
        return s;
    }
    if (len > 1) {
        qsort(v, len, sizeof *v, entry_cmp);
    }
    *out = len > 0 ? v : NULL;
    if (len == 0) {
        free(v);
    }
    *n = len;
    return PC_OK;
}

pc_status pc_recover(const pc_recovery_entry *e, pc_document *out, pc_error *err)
{
    return pc_format_load_path(e->bundle, out, err);
}

pc_status pc_discard_recovery(const char *dir, const pc_recovery_entry *e, pc_error *err)
{
    return remove_entry(dir, pc_str_c(&e->info.key), e->info.key.len, err);
}

/* ---- the autosaver ---- */

typedef struct job {
    pc_document snapshot;
    pc_recovery_info info;
    pc_rgba8_image thumb;
    pc_rgba8_image composite;
    bool has_thumb;
    bool has_composite;
} job;

struct pc_autosaver {
    char *dir;
    pc_str key;
    char *bundle;
    char *sidecar;
    pthread_t thread;
    bool started;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool closing; /* the reference's sender is gone */
    bool stopped; /* flushed or discarded: requests fail */
    job *pending;
    /* last completed save */
    bool has_last;
    bool last_ok;
    pc_format_save_stats last_stats;
    pc_error last_err;
    /* completed writes not yet taken */
    pc_autosave_outcome *done;
    size_t ndone;
    size_t capdone;
};

static void job_free(job *j)
{
    if (j != NULL) {
        pc_document_fini(&j->snapshot);
        pc_recovery_info_fini(&j->info);
        pc_rgba8_image_fini(&j->thumb);
        pc_rgba8_image_fini(&j->composite);
        free(j);
    }
}

static pc_status run_job(pc_pcraft_writer *w, const pc_autosaver *a, job *j, pc_format_save_stats *stats,
                         pc_error *err)
{
    pc_format_save_opts opts = {j->has_thumb ? &j->thumb : NULL, j->has_composite ? &j->composite : NULL};
    pc_buf side = PC_BUF_INIT;
    pc_status s = pc_mkdir_p(a->bundle, err);
    if (s != PC_OK) {
        return pc_format_io(s, err);
    }
    s = pc_pcraft_writer_save_dir(w, &j->snapshot, a->bundle, &opts, stats, err);
    if (s == PC_OK) {
        s = pc_recovery_info_to_json(&j->info, &side, err);
    }
    if (s == PC_OK) {
        s = pc_format_write_atomic(a->sidecar, side.data, side.len, err);
    }
    pc_buf_fini(&side);
    return s;
}

/* Record a finished write (caller holds the lock). A full outcome list drops the oldest
 * outcome rather than fail the worker. */
static void record(pc_autosaver *a, uint64_t revision, pc_status s, const pc_format_save_stats *stats,
                   const pc_error *e)
{
    pc_autosave_outcome o;
    memset(&o, 0, sizeof o);
    o.revision = revision;
    o.ok = s == PC_OK;
    if (s != PC_OK) {
        (void)snprintf(o.msg, sizeof o.msg, "%s", e->msg);
    }
    if (a->ndone == a->capdone) {
        pc_autosave_outcome *g = pc_grow_array(a->done, &a->capdone, a->ndone + 1, sizeof *g);
        if (g != NULL) {
            a->done = g;
        } else if (a->ndone > 0) {
            memmove(a->done, a->done + 1, (a->ndone - 1) * sizeof *a->done);
            a->ndone--;
        }
    }
    if (a->ndone < a->capdone) {
        a->done[a->ndone++] = o;
    }
    a->has_last = true;
    a->last_ok = s == PC_OK;
    if (s == PC_OK) {
        a->last_stats = *stats;
    } else {
        a->last_err = *e;
    }
}

static void *worker(void *arg)
{
    pc_autosaver *a = arg;
    pc_pcraft_writer *w = NULL;
    pc_error werr;
    pc_status ws = pc_pcraft_writer_new(&w, &werr);
    (void)pthread_mutex_lock(&a->mu);
    for (;;) {
        job *j;
        pc_format_save_stats stats;
        pc_error e;
        pc_status s;
        while (a->pending == NULL && !a->closing) {
            (void)pthread_cond_wait(&a->cv, &a->mu);
        }
        if (a->pending == NULL) {
            break;
        }
        j = a->pending;
        a->pending = NULL;
        (void)pthread_mutex_unlock(&a->mu);
        memset(&stats, 0, sizeof stats);
        memset(&e, 0, sizeof e);
        if (ws != PC_OK) {
            s = ws;
            e = werr;
        } else {
            s = run_job(w, a, j, &stats, &e);
        }
        (void)pthread_mutex_lock(&a->mu);
        record(a, j->info.revision, s, &stats, &e);
        (void)pthread_mutex_unlock(&a->mu);
        job_free(j);
        (void)pthread_mutex_lock(&a->mu);
    }
    (void)pthread_mutex_unlock(&a->mu);
    pc_pcraft_writer_free(w);
    return NULL;
}

pc_status pc_autosaver_new(const char *recovery_dir, const char *key, size_t key_len, pc_autosaver **out,
                           pc_error *err)
{
    pc_autosaver *a = calloc(1, sizeof *a);
    pc_status s;
    int r;
    if (a == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s = pc_autosave_sanitize_key(key, key_len, &a->key, err);
    if (s == PC_OK) {
        a->dir = pc_strndup(recovery_dir, strlen(recovery_dir));
        a->bundle = join(recovery_dir, pc_str_c(&a->key), a->key.len, ".pcraft");
        a->sidecar = join(recovery_dir, pc_str_c(&a->key), a->key.len, ".json");
        if (a->dir == NULL || a->bundle == NULL || a->sidecar == NULL) {
            s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
    }
    if (s == PC_OK && pthread_mutex_init(&a->mu, NULL) != 0) {
        s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
    } else if (s == PC_OK && pthread_cond_init(&a->cv, NULL) != 0) {
        (void)pthread_mutex_destroy(&a->mu);
        s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (s != PC_OK) {
        pc_str_fini(&a->key);
        free(a->dir);
        free(a->bundle);
        free(a->sidecar);
        free(a);
        return s;
    }
    r = pthread_create(&a->thread, NULL, worker, a);
    if (r == 0) {
        a->started = true;
    } else {
        /* A thread that never started must not look like a working autosave: record the
         * failure where last_result / flush report it. */
        a->has_last = true;
        a->last_ok = false;
        pc_format_fail_set(&a->last_err, PC_ERR_IO, "I/O: ", "the autosave thread could not start: %s",
                           strerror(r));
    }
    *out = a;
    return PC_OK;
}

static void shutdown_worker(pc_autosaver *a)
{
    (void)pthread_mutex_lock(&a->mu);
    a->closing = true;
    a->stopped = true;
    (void)pthread_cond_signal(&a->cv);
    (void)pthread_mutex_unlock(&a->mu);
    if (a->started) {
        (void)pthread_join(a->thread, NULL);
        a->started = false;
    }
}

void pc_autosaver_free(pc_autosaver *a)
{
    if (a == NULL) {
        return;
    }
    shutdown_worker(a);
    job_free(a->pending);
    (void)pthread_cond_destroy(&a->cv);
    (void)pthread_mutex_destroy(&a->mu);
    pc_str_fini(&a->key);
    free(a->dir);
    free(a->bundle);
    free(a->sidecar);
    free(a->done);
    free(a);
}

const char *pc_autosaver_bundle_path(const pc_autosaver *a)
{
    return a->bundle;
}

static pc_status copy_image(const pc_rgba8_image *src, pc_rgba8_image *dst, pc_error *err)
{
    size_t n;
    pc_status s = pc_rgba8_image_init(dst, src->width, src->height, err);
    if (s == PC_OK) {
        n = (size_t)src->width * src->height * 4u;
        if (n > 0 && src->pixels != NULL) {
            memcpy(dst->pixels, src->pixels, n);
        }
    }
    return s;
}

pc_status pc_autosaver_request(pc_autosaver *a, const pc_document *doc, uint64_t revision,
                               const char *original_path, const pc_format_save_opts *opts, pc_error *err)
{
    job *j;
    pc_status s;
    struct timespec now;
    (void)pthread_mutex_lock(&a->mu);
    if (a->stopped) {
        (void)pthread_mutex_unlock(&a->mu);
        return pc_fail(err, PC_ERR_INVALID, "autosave worker is stopped");
    }
    if (!a->started) {
        if (a->has_last && !a->last_ok) {
            if (err != NULL) {
                *err = a->last_err;
            }
            s = a->last_err.code != PC_OK ? a->last_err.code : PC_ERR_IO;
        } else {
            s = pc_fail(err, PC_ERR_IO, "autosave worker is unavailable");
        }
        (void)pthread_mutex_unlock(&a->mu);
        return s;
    }
    (void)pthread_mutex_unlock(&a->mu);
    j = calloc(1, sizeof *j);
    if (j == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s = pc_document_clone(doc, &j->snapshot, err);
    if (s != PC_OK) {
        free(j);
        return s;
    }
    s = pc_str_clone(&a->key, &j->info.key, err);
    if (s == PC_OK) {
        s = pc_str_clone(&doc->name, &j->info.document_name, err);
    }
    if (s == PC_OK && original_path != NULL) {
        j->info.has_original_path = true;
        s = pc_str_set_c(&j->info.original_path, original_path, err);
    }
    if (s == PC_OK && opts != NULL && opts->thumbnail != NULL) {
        s = copy_image(opts->thumbnail, &j->thumb, err);
        j->has_thumb = s == PC_OK;
    }
    if (s == PC_OK && opts != NULL && opts->composite != NULL) {
        s = copy_image(opts->composite, &j->composite, err);
        j->has_composite = s == PC_OK;
    }
    if (s != PC_OK) {
        job_free(j);
        return s;
    }
    j->info.saved_at = clock_gettime(CLOCK_REALTIME, &now) == 0 && now.tv_sec > 0 ? (uint64_t)now.tv_sec : 0;
    j->info.revision = revision;
    (void)pthread_mutex_lock(&a->mu);
    job_free(a->pending); /* coalesce: only the newest snapshot is written */
    a->pending = j;
    (void)pthread_cond_signal(&a->cv);
    (void)pthread_mutex_unlock(&a->mu);
    return PC_OK;
}

pc_status pc_autosaver_take_completed(pc_autosaver *a, pc_autosave_outcome **out, size_t *n, pc_error *err)
{
    (void)err;
    (void)pthread_mutex_lock(&a->mu);
    *out = a->ndone > 0 ? a->done : NULL;
    *n = a->ndone;
    if (a->ndone == 0) {
        free(a->done);
    }
    a->done = NULL;
    a->ndone = 0;
    a->capdone = 0;
    (void)pthread_mutex_unlock(&a->mu);
    return PC_OK;
}

bool pc_autosaver_last_result(pc_autosaver *a, bool *ok, pc_format_save_stats *stats, pc_error *msg)
{
    bool has;
    (void)pthread_mutex_lock(&a->mu);
    has = a->has_last;
    if (has) {
        if (ok != NULL) {
            *ok = a->last_ok;
        }
        if (a->last_ok && stats != NULL) {
            *stats = a->last_stats;
        }
        if (!a->last_ok && msg != NULL) {
            *msg = a->last_err;
        }
    }
    (void)pthread_mutex_unlock(&a->mu);
    return has;
}

bool pc_autosaver_flush(pc_autosaver *a, bool *ok, pc_format_save_stats *stats, pc_error *msg)
{
    shutdown_worker(a);
    return pc_autosaver_last_result(a, ok, stats, msg);
}

pc_status pc_autosaver_discard(pc_autosaver *a, pc_error *err)
{
    shutdown_worker(a);
    return remove_entry(a->dir, pc_str_c(&a->key), a->key.len, err);
}

/* ---- RecoveryStore ---- */

typedef struct saver_ent {
    uint64_t id;
    pc_autosaver *saver;
} saver_ent;

typedef struct adopted_ent {
    uint64_t id;
    pc_str key;
} adopted_ent;

struct pc_recovery_store {
    char *dir;
    char session[96];
    saver_ent *savers;
    size_t nsavers;
    adopted_ent *adopted;
    size_t nadopted;
};

pc_status pc_recovery_store_new(const char *recovery_dir, pc_recovery_store **out, pc_error *err)
{
    static atomic_ulong stores;
    pc_recovery_store *s = calloc(1, sizeof *s);
    struct timespec now;
    unsigned long long t = 0;
    if (s == NULL || (s->dir = pc_strndup(recovery_dir, strlen(recovery_dir))) == NULL) {
        free(s);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    if (clock_gettime(CLOCK_REALTIME, &now) == 0 && now.tv_sec >= 0) {
        t = (unsigned long long)now.tv_sec * 1000000000ull + (unsigned long long)now.tv_nsec;
    }
    /* Unique to this store: ids restart every launch, so a new document must never overwrite an
     * entry an earlier launch (or another store) left. */
    (void)snprintf(s->session, sizeof s->session, "%llx_%lx_%lx", t, (unsigned long)getpid(),
                   atomic_fetch_add(&stores, 1ul));
    *out = s;
    return PC_OK;
}

void pc_recovery_store_free(pc_recovery_store *s)
{
    if (s == NULL) {
        return;
    }
    for (size_t i = 0; i < s->nsavers; i++) {
        pc_autosaver_free(s->savers[i].saver);
    }
    for (size_t i = 0; i < s->nadopted; i++) {
        pc_str_fini(&s->adopted[i].key);
    }
    free(s->savers);
    free(s->adopted);
    free(s->dir);
    free(s);
}

void pc_recovered_free(pc_recovered *r, size_t n)
{
    if (r == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        pc_recovery_info_fini(&r[i].entry.info);
        free(r[i].entry.bundle);
        pc_document_fini(&r[i].doc);
    }
    free(r);
}

pc_status pc_recovery_store_recover(const pc_recovery_store *s, pc_recovered **out, size_t *n, pc_error *err)
{
    pc_recovery_entry *e = NULL;
    size_t ne = 0;
    size_t k = 0;
    pc_recovered *r;
    pc_status st = pc_list_recovery(s->dir, &e, &ne, err);
    if (st != PC_OK) {
        return st;
    }
    r = ne > 0 ? calloc(ne, sizeof *r) : NULL;
    if (ne > 0 && r == NULL) {
        pc_recovery_entries_free(e, ne);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    for (size_t i = 0; i < ne; i++) {
        pc_document doc;
        pc_error le;
        pc_status ls = pc_recover(&e[i], &doc, &le);
        if (ls == PC_ERR_NOMEM) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory");
            break;
        }
        if (ls != PC_OK) {
            continue; /* it stays on disk untouched */
        }
        r[k].entry = e[i];
        r[k].doc = doc;
        memset(&e[i], 0, sizeof e[i]);
        k++;
    }
    pc_recovery_entries_free(e, ne);
    if (st != PC_OK) {
        pc_recovered_free(r, k);
        return st;
    }
    if (k == 0) {
        free(r);
        r = NULL;
    }
    *out = r;
    *n = k;
    return PC_OK;
}

pc_status pc_recovery_store_adopt(pc_recovery_store *s, uint64_t doc_id, const char *key, pc_error *err)
{
    pc_str clean = PC_STR_INIT;
    adopted_ent *g;
    pc_status st = pc_autosave_sanitize_key(key, strlen(key), &clean, err);
    if (st != PC_OK) {
        return st;
    }
    for (size_t i = 0; i < s->nadopted; i++) {
        if (s->adopted[i].id == doc_id) {
            pc_str_fini(&s->adopted[i].key);
            s->adopted[i].key = clean;
            return PC_OK;
        }
    }
    {
        size_t cap = s->nadopted;
        g = pc_grow_array(s->adopted, &cap, s->nadopted + 1, sizeof *g);
    }
    if (g == NULL) {
        pc_str_fini(&clean);
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    s->adopted = g;
    s->adopted[s->nadopted].id = doc_id;
    s->adopted[s->nadopted].key = clean;
    s->nadopted++;
    return PC_OK;
}

pc_status pc_recovery_store_autosave(pc_recovery_store *s, const pc_document *doc, uint64_t revision,
                                     const char *original_path, pc_error *err)
{
    pc_autosaver *saver = NULL;
    for (size_t i = 0; i < s->nsavers; i++) {
        if (s->savers[i].id == doc->id) {
            saver = s->savers[i].saver;
        }
    }
    if (saver == NULL) {
        char key[160];
        const char *k = NULL;
        size_t klen = 0;
        saver_ent *g;
        pc_status st;
        for (size_t i = 0; i < s->nadopted; i++) {
            if (s->adopted[i].id == doc->id) {
                k = pc_str_c(&s->adopted[i].key);
                klen = s->adopted[i].key.len;
            }
        }
        if (k == NULL) {
            int w = snprintf(key, sizeof key, "doc-%s-%llu", s->session, (unsigned long long)doc->id);
            k = key;
            klen = w > 0 ? (size_t)w : 0;
        }
        {
            size_t cap = s->nsavers;
            g = pc_grow_array(s->savers, &cap, s->nsavers + 1, sizeof *g);
        }
        if (g == NULL) {
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        s->savers = g;
        st = pc_autosaver_new(s->dir, k, klen, &saver, err);
        if (st != PC_OK) {
            return st;
        }
        s->savers[s->nsavers].id = doc->id;
        s->savers[s->nsavers].saver = saver;
        s->nsavers++;
    }
    return pc_autosaver_request(saver, doc, revision, original_path, NULL, err);
}

pc_status pc_recovery_store_take_completed(pc_recovery_store *s, pc_recovery_outcome **out, size_t *n,
                                           pc_error *err)
{
    pc_recovery_outcome *v = NULL;
    size_t len = 0;
    for (size_t i = 0; i < s->nsavers; i++) {
        pc_autosave_outcome *o = NULL;
        size_t no = 0;
        pc_recovery_outcome *g;
        (void)pc_autosaver_take_completed(s->savers[i].saver, &o, &no, NULL);
        if (no == 0) {
            continue;
        }
        {
            size_t cap = len;
            g = pc_grow_array(v, &cap, len + no, sizeof *g);
        }
        if (g == NULL) {
            free(o);
            free(v);
            return pc_fail(err, PC_ERR_NOMEM, "out of memory");
        }
        v = g;
        for (size_t k = 0; k < no; k++) {
            v[len].doc_id = s->savers[i].id;
            v[len].outcome = o[k];
            len++;
        }
        free(o);
    }
    *out = v;
    *n = len;
    return PC_OK;
}

pc_status pc_recovery_store_discard(pc_recovery_store *s, uint64_t doc_id, pc_error *err)
{
    pc_status own = PC_OK;
    pc_status adopted = PC_OK;
    pc_error own_err;
    memset(&own_err, 0, sizeof own_err);
    for (size_t i = 0; i < s->nsavers; i++) {
        if (s->savers[i].id == doc_id) {
            pc_autosaver *a = s->savers[i].saver;
            s->savers[i] = s->savers[--s->nsavers];
            own = pc_autosaver_discard(a, &own_err);
            pc_autosaver_free(a);
            break;
        }
    }
    for (size_t i = 0; i < s->nadopted; i++) {
        if (s->adopted[i].id == doc_id) {
            pc_str key = s->adopted[i].key;
            s->adopted[i] = s->adopted[--s->nadopted];
            adopted = remove_entry(s->dir, pc_str_c(&key), key.len, err);
            pc_str_fini(&key);
            break;
        }
    }
    if (own != PC_OK) {
        if (err != NULL) {
            *err = own_err;
        }
        return own;
    }
    return adopted;
}
