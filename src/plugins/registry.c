/* The process-wide set of installed plug-ins (port of crates/plugins/src/registry.rs).
 *
 * Plug-ins are installed per process (as in Photoshop), not per document: smart filters and
 * filter previews, which run on scratch sessions, must find the same plug-ins. The set is kept
 * sorted by id (the reference's BTreeMap) under a reader/writer lock. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/mem.h"
#include "plugins/internal.h"

static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
static pc_plugin **g_plugins; /* sorted by id */
static size_t g_count, g_cap;
static atomic_uint_fast64_t g_revision;

/* Index of id, or where it would go. */
static size_t find(const char *id, bool *found)
{
    size_t lo = 0, hi = g_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(pc_plugin_id(g_plugins[mid]), id);
        if (c == 0) {
            *found = true;
            return mid;
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = false;
    return lo;
}

pc_status pc_plugins_install(pc_plugin *p, pc_error *err)
{
    bool found;
    size_t at;
    pc_plugin *old = NULL;
    pc_status st = PC_OK;
    if (p == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "no plug-in");
    }
    (void)pthread_rwlock_wrlock(&g_lock);
    at = find(pc_plugin_id(p), &found);
    if (found) {
        old = g_plugins[at];
        g_plugins[at] = pc_plugin_retain(p);
    } else {
        pc_plugin **grown = pc_grow_array(g_plugins, &g_cap, g_count + 1, sizeof *grown);
        if (grown == NULL) {
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory installing a plug-in");
        } else {
            g_plugins = grown;
            memmove(&g_plugins[at + 1], &g_plugins[at], (g_count - at) * sizeof *g_plugins);
            g_plugins[at] = pc_plugin_retain(p);
            g_count++;
        }
    }
    if (st == PC_OK) {
        atomic_fetch_add_explicit(&g_revision, 1, memory_order_relaxed);
    }
    (void)pthread_rwlock_unlock(&g_lock);
    pc_plugin_release(old);
    return st;
}

pc_status pc_plugins_install_bytes(const uint8_t *bytes, size_t len, pc_plugin **out, pc_error *err)
{
    pc_plugin *p = NULL;
    pc_status st = pc_plugin_load(bytes, len, NULL, &p, err);
    if (st == PC_OK) {
        st = pc_plugins_install(p, err);
    }
    if (st != PC_OK) {
        pc_plugin_release(p);
        return st;
    }
    if (out != NULL) {
        *out = p;
    } else {
        pc_plugin_release(p);
    }
    return PC_OK;
}

pc_plugin *pc_plugins_get(const char *id)
{
    bool found;
    size_t at;
    pc_plugin *p = NULL;
    if (id == NULL) {
        return NULL;
    }
    (void)pthread_rwlock_rdlock(&g_lock);
    at = find(id, &found);
    if (found) {
        p = pc_plugin_retain(g_plugins[at]);
    }
    (void)pthread_rwlock_unlock(&g_lock);
    return p;
}

pc_status pc_plugins_list(pc_plugin ***out, size_t *n, pc_error *err)
{
    pc_plugin **list;
    size_t count;
    (void)pthread_rwlock_rdlock(&g_lock);
    count = g_count;
    list = pc_calloc_array(count > 0 ? count : 1, sizeof *list);
    if (list != NULL) {
        for (size_t i = 0; i < count; i++) {
            list[i] = pc_plugin_retain(g_plugins[i]);
        }
    }
    (void)pthread_rwlock_unlock(&g_lock);
    if (list == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory listing plug-ins");
    }
    *out = list;
    *n = count;
    return PC_OK;
}

void pc_plugins_list_free(pc_plugin **list, size_t n)
{
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        pc_plugin_release(list[i]);
    }
    free(list);
}

bool pc_plugins_remove(const char *id)
{
    bool found = false;
    size_t at;
    pc_plugin *old = NULL;
    if (id == NULL) {
        return false;
    }
    (void)pthread_rwlock_wrlock(&g_lock);
    at = find(id, &found);
    if (found) {
        old = g_plugins[at];
        memmove(&g_plugins[at], &g_plugins[at + 1], (g_count - at - 1) * sizeof *g_plugins);
        g_count--;
        atomic_fetch_add_explicit(&g_revision, 1, memory_order_relaxed);
    }
    (void)pthread_rwlock_unlock(&g_lock);
    pc_plugin_release(old);
    return found;
}

uint64_t pc_plugins_revision(void)
{
    return (uint64_t)atomic_load_explicit(&g_revision, memory_order_relaxed);
}

/* Rust's io::Error Display for an OS error: "<strerror> (os error N)". */
static pc_status io_fail(pc_error *err, int e, const char *path)
{
    (void)pc_fail(err, PC_ERR_IO, "%s: %s (os error %d)", path, strerror(e), e);
    if (err != NULL) {
        err->sys_errno = e;
    }
    return PC_ERR_IO;
}

pc_status pc_plugins_load_file(const char *path, const pc_plugin_limits *limits, pc_plugin **out,
                               pc_error *err)
{
    pc_plugin_limits lim = limits != NULL ? *limits : pc_plugin_limits_default();
    struct stat sb;
    pc_buf b = PC_BUF_INIT;
    pc_plugin *p = NULL;
    pc_status st;
    int fd;
    if (stat(path, &sb) != 0) {
        return io_fail(err, errno, path);
    }
    if (!S_ISREG(sb.st_mode)) {
        return pc_fail(err, PC_ERR_IO, "%s is not a file", path);
    }
    if ((uint64_t)sb.st_size > (uint64_t)lim.max_module_bytes) {
        return PC_PLUGIN_FAIL_MODULE(err, "%s is %llu bytes (limit %zu)", path,
                                     (unsigned long long)sb.st_size, lim.max_module_bytes);
    }
    do {
        fd = open(path, O_RDONLY);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return io_fail(err, errno, path);
    }
    /* At most limit + 1 bytes: a file that grew since stat() still fails the size check. */
    st = pc_fd_read_all(fd, lim.max_module_bytes < SIZE_MAX ? lim.max_module_bytes + 1 : SIZE_MAX, &b, err);
    (void)close(fd);
    if (st == PC_ERR_LIMIT) {
        pc_buf_fini(&b);
        /* The reference reads limit + 1 bytes and then refuses that many. */
        return PC_PLUGIN_FAIL_MODULE(err, "module is %zu bytes (limit %zu)", lim.max_module_bytes + 1,
                                     lim.max_module_bytes);
    }
    if (st != PC_OK) {
        int e = err != NULL ? err->sys_errno : EIO;
        pc_buf_fini(&b);
        return st == PC_ERR_IO ? io_fail(err, e != 0 ? e : EIO, path) : st;
    }
    st = pc_plugin_load(b.data, b.len, &lim, &p, err);
    pc_buf_fini(&b);
    if (st == PC_OK) {
        st = pc_plugin_set_source(p, path, err);
    }
    if (st != PC_OK) {
        pc_plugin_release(p);
        return st;
    }
    *out = p;
    return PC_OK;
}

static bool has_wasm_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    /* Path::extension: none for "x" or ".wasm" (a dot only at the start). */
    if (dot == NULL || dot == name) {
        return false;
    }
    return strlen(dot + 1) == 4 && (dot[1] | 0x20) == 'w' && (dot[2] | 0x20) == 'a' &&
           (dot[3] | 0x20) == 's' && (dot[4] | 0x20) == 'm';
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void pc_plugins_folder_report_fini(pc_plugins_folder_report *r)
{
    if (r == NULL) {
        return;
    }
    for (size_t i = 0; i < r->n_loaded; i++) {
        pc_str_fini(&r->loaded[i]);
    }
    free(r->loaded);
    for (size_t i = 0; i < r->n_failed; i++) {
        free(r->failed[i].file);
        free(r->failed[i].error);
    }
    free(r->failed);
    memset(r, 0, sizeof *r);
}

pc_status pc_plugins_load_folder(const char *dir, pc_plugins_folder_report *out, pc_error *err)
{
    DIR *dh;
    struct dirent *e;
    char **files = NULL;
    size_t nf = 0, capf = 0, capl = 0, capx = 0;
    pc_plugins_folder_report r;
    pc_status st = PC_OK;
    size_t dl = strlen(dir);
    bool slash = dl > 0 && dir[dl - 1] == '/';
    memset(&r, 0, sizeof r);
    dh = opendir(dir);
    if (dh == NULL) {
        return io_fail(err, errno, dir);
    }
    while ((e = readdir(dh)) != NULL && st == PC_OK) {
        char *path, **grown;
        size_t nl;
        if (!has_wasm_extension(e->d_name)) {
            continue;
        }
        nl = strlen(e->d_name);
        path = malloc(dl + nl + 2);
        grown = path != NULL ? pc_grow_array(files, &capf, nf + 1, sizeof *grown) : NULL;
        if (grown == NULL) {
            free(path);
            st = pc_fail(err, PC_ERR_NOMEM, "out of memory listing %s", dir);
            break;
        }
        files = grown;
        (void)snprintf(path, dl + nl + 2, "%s%s%s", dir, slash || dl == 0 ? "" : "/", e->d_name);
        files[nf++] = path;
    }
    (void)closedir(dh);
    if (st == PC_OK && nf > 1) {
        qsort(files, nf, sizeof *files, cmp_str);
    }
    for (size_t i = 0; i < nf && i < 1000 && st == PC_OK; i++) {
        pc_plugin *p = NULL;
        pc_error le;
        pc_status ls;
        pc_error_clear(&le);
        ls = pc_plugins_load_file(files[i], NULL, &p, &le);
        if (ls == PC_OK) {
            ls = pc_plugins_install(p, &le);
        }
        if (ls == PC_OK) {
            pc_str *grown = pc_grow_array(r.loaded, &capl, r.n_loaded + 1, sizeof *grown);
            if (grown == NULL) {
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory loading plug-ins");
            } else {
                r.loaded = grown;
                r.loaded[r.n_loaded] = (pc_str)PC_STR_INIT;
                st = pc_str_set_c(&r.loaded[r.n_loaded], pc_plugin_id(p), err);
                r.n_loaded += st == PC_OK ? 1 : 0;
            }
        } else if (ls == PC_ERR_NOMEM) {
            st = pc_fail(err, PC_ERR_NOMEM, "%s", le.msg);
        } else {
            pc_plugins_failure *grown = pc_grow_array(r.failed, &capx, r.n_failed + 1, sizeof *grown);
            char *f = pc_strndup(files[i], strlen(files[i])), *m = pc_strndup(le.msg, strlen(le.msg));
            if (grown == NULL || f == NULL || m == NULL) {
                if (grown != NULL) {
                    r.failed = grown;
                }
                free(f);
                free(m);
                st = pc_fail(err, PC_ERR_NOMEM, "out of memory loading plug-ins");
            } else {
                r.failed = grown;
                r.failed[r.n_failed].file = f;
                r.failed[r.n_failed].error = m;
                r.n_failed++;
            }
        }
        pc_plugin_release(p);
    }
    for (size_t i = 0; i < nf; i++) {
        free(files[i]);
    }
    free(files);
    if (st != PC_OK) {
        pc_plugins_folder_report_fini(&r);
        return st;
    }
    *out = r;
    return PC_OK;
}
