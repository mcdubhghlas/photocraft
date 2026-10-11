/* realpath() is an XSI interface in POSIX.1-2008 (base only since POSIX.1-2024): symlink targets
 * are written through (D21). */
#define _XOPEN_SOURCE 700

#include "file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "mem.h"
#include "num.h"

static const char UNCHANGED[] = "the original file was not changed";

pc_status pc_fd_read(int fd, void *buf, size_t len, size_t *got, pc_error *err)
{
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        ssize_t n;
        if (want > (size_t)SSIZE_MAX) {
            want = (size_t)SSIZE_MAX;
        }
        n = read(fd, (unsigned char *)buf + done, want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return pc_fail_errno(err, PC_ERR_IO, errno, "read failed");
        }
        if (n == 0) {
            break;
        }
        done += (size_t)n;
    }
    *got = done;
    return PC_OK;
}

pc_status pc_fd_write(int fd, const void *buf, size_t len, pc_error *err)
{
    size_t done = 0;
    while (done < len) {
        size_t want = len - done;
        ssize_t n;
        if (want > (size_t)SSIZE_MAX) {
            want = (size_t)SSIZE_MAX;
        }
        n = write(fd, (const unsigned char *)buf + done, want);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return pc_fail_errno(err, PC_ERR_IO, errno, "write failed");
        }
        if (n == 0) {
            /* POSIX allows it for some devices; treat as "no progress possible". */
            return pc_fail_errno(err, PC_ERR_IO, EIO, "write made no progress");
        }
        done += (size_t)n;
    }
    return PC_OK;
}

pc_status pc_fd_read_all(int fd, size_t max, pc_buf *out, pc_error *err)
{
    pc_buf tmp = PC_BUF_INIT;
    pc_status s = PC_OK;
    for (;;) {
        size_t got = 0;
        size_t chunk = 64 * 1024;
        /* Read one byte past max so an oversize stream is detected, never silently cut.
         * (max - len < chunk, so the + 1 can't overflow.) */
        if (max - tmp.len < chunk) {
            chunk = max - tmp.len + 1;
        }
        s = pc_buf_reserve(&tmp, chunk, err);
        if (s != PC_OK) {
            goto cleanup;
        }
        s = pc_fd_read(fd, tmp.data + tmp.len, chunk, &got, err);
        if (s != PC_OK) {
            goto cleanup;
        }
        tmp.len += got;
        tmp.data[tmp.len] = '\0';
        if (tmp.len > max) {
            s = pc_fail(err, PC_ERR_LIMIT, "input is larger than the %zu-byte limit", max);
            goto cleanup;
        }
        if (got < chunk) {
            break;
        }
    }
    pc_buf_fini(out);
    *out = tmp;
    return PC_OK;
cleanup:
    pc_buf_fini(&tmp);
    return s;
}

pc_status pc_file_read(const char *path, size_t max, pc_buf *out, pc_error *err)
{
    struct stat st;
    pc_status s;
    int fd;

    do {
        fd = open(path, O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        int e = errno;
        return pc_fail_errno(err, e == ENOENT ? PC_ERR_NOT_FOUND : PC_ERR_IO, e, "cannot open %s", path);
    }
    if (fstat(fd, &st) != 0) {
        s = pc_fail_errno(err, PC_ERR_IO, errno, "cannot stat %s", path);
        goto cleanup;
    }
    if (S_ISDIR(st.st_mode)) {
        s = pc_fail_errno(err, PC_ERR_IO, EISDIR, "cannot read %s", path);
        goto cleanup;
    }
    if (S_ISREG(st.st_mode) && st.st_size > 0 && (uintmax_t)st.st_size > (uintmax_t)max) {
        s = pc_fail(err, PC_ERR_LIMIT, "%s is larger than the %zu-byte limit", path, max);
        goto cleanup;
    }
    s = pc_fd_read_all(fd, max, out, err);
    if (s != PC_OK && err != NULL && err->code != PC_ERR_LIMIT) {
        /* Prefix the path for context. */
        char inner[PC_ERROR_MSG_MAX];
        memcpy(inner, err->msg, sizeof inner);
        if (snprintf(err->msg, sizeof err->msg, "%s: %s", path, inner) < 0) { /* truncation is fine */
            err->msg[0] = '\0';
        }
    }
cleanup:
    (void)close(fd);
    return s;
}

pc_status pc_mkdir_p(const char *path, pc_error *err)
{
    size_t len = strlen(path);
    char *p = pc_strndup(path, len);
    size_t i;
    pc_status s = PC_OK;

    if (p == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    /* Create each prefix ending before a '/', then the whole path. */
    for (i = 1; i <= len; i++) {
        if (p[i] == '/' || p[i] == '\0') {
            char saved = p[i];
            p[i] = '\0';
            if (mkdir(p, 0777) != 0) {
                /* Some systems report EACCES/EROFS rather than EEXIST for an existing folder. */
                int e = errno;
                struct stat st;
                if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    s = pc_fail_errno(err, PC_ERR_IO, e == EEXIST ? ENOTDIR : e, "cannot create the folder %s", p);
                    goto cleanup;
                }
            }
            p[i] = saved;
        }
    }
cleanup:
    free(p);
    return s;
}

/* Byte length of at most `max_chars` UTF-8 characters at the start of s (never splits one). */
static size_t utf8_prefix(const char *s, size_t max_chars)
{
    size_t i = 0;
    size_t chars = 0;
    while (s[i] != '\0') {
        if (((unsigned char)s[i] & 0xC0u) != 0x80u) {
            if (chars == max_chars) {
                break;
            }
            chars++;
        }
        i++;
    }
    return i;
}

/* ---- the real file system ---- */

static int real_write_new(void *ctx, const char *tmp, const void *data, size_t len)
{
    pc_error e;
    int fd;
    int r = 0;
    (void)ctx;
    do {
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return errno;
    }
    if (pc_fd_write(fd, data, len, &e) != PC_OK) {
        r = e.sys_errno != 0 ? e.sys_errno : EIO;
    } else if (fsync(fd) != 0) {
        r = errno;
    }
    if (close(fd) != 0 && r == 0 && errno != EINTR) {
        r = errno;
    }
    return r;
}

static int real_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    return rename(from, to) == 0 ? 0 : errno;
}

static int real_remove(void *ctx, const char *path)
{
    (void)ctx;
    return unlink(path) == 0 ? 0 : errno;
}

static int real_sync_dir(void *ctx, const char *dir)
{
    int fd;
    int r = 0;
    (void)ctx;
    fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return errno;
    }
    if (fsync(fd) != 0) {
        r = errno;
    }
    (void)close(fd);
    return r;
}

static int real_copy_permissions(void *ctx, const char *from, const char *to)
{
    struct stat st;
    (void)ctx;
    if (stat(from, &st) != 0) {
        return errno;
    }
    return chmod(to, st.st_mode & 07777) == 0 ? 0 : errno;
}

const pc_atomic_fs pc_atomic_real_fs = {NULL,        real_write_new, real_rename,
                                        real_remove, real_sync_dir,  real_copy_permissions};

pc_rename_retry pc_rename_retry_platform(void)
{
    pc_rename_retry r = {0, 0};
    return r;
}

static void sleep_ms(uint64_t ms)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

int pc_atomic_retry_rename(pc_rename_retry retry, int (*rename_fn)(void *ctx), void *ctx)
{
    uint64_t delay = retry.first_delay_ms;
    uint32_t left = retry.attempts;
    for (;;) {
        int r = rename_fn(ctx);
        if ((r == EACCES || r == EPERM) && left > 0) {
            left--;
            sleep_ms(delay);
            delay = delay > UINT64_MAX / 2 ? UINT64_MAX : delay * 2;
            continue;
        }
        return r;
    }
}

pc_status pc_atomic_temp_name(const char *file, pc_buf *out, pc_error *err)
{
    static atomic_ulong counter;
    unsigned long n = atomic_fetch_add(&counter, 1ul);
    pc_buf b = PC_BUF_INIT;
    /* Keep the name short: a long file name plus the suffix must stay under NAME_MAX. */
    pc_status s = pc_buf_appendf(&b, err, ".%.*s.%ld-%lu.pcsave.tmp", (int)utf8_prefix(file, 64), file,
                                 (long)getpid(), n);
    if (s != PC_OK) {
        return s;
    }
    pc_buf_fini(out);
    *out = b;
    return PC_OK;
}

/* ---- the algorithm ---- */

static pc_status save_fail(pc_error *err, int e, const char *path, const char *what)
{
    pc_status s = pc_fail(err, PC_ERR_IO, "cannot save %s: %s (%s); %s", path, what, strerror(e), UNCHANGED);
    if (err != NULL) {
        err->sys_errno = e;
    }
    return s;
}

static pc_status write_error(pc_error *err, int e, const char *path, const char *dir)
{
    if (e == EACCES || e == EPERM || e == EROFS) {
        char what[PC_ERROR_MSG_MAX];
        (void)snprintf(what, sizeof what, "the folder %s is not writable", dir);
        return save_fail(err, e, path, what);
    }
    if (e == ENOSPC
#ifdef EDQUOT
        || e == EDQUOT
#endif
    ) {
        return save_fail(err, e, path, "the disk is full");
    }
    return save_fail(err, e, path, "write failed");
}

static pc_status rename_error(pc_error *err, int e, const char *path)
{
    if (e == EXDEV) {
        return save_fail(err, e, path, "the temporary file is on a different volume than the destination");
    }
    if (e == EACCES || e == EPERM) {
        return save_fail(err, e, path, "the file is locked or read-only");
    }
    return save_fail(err, e, path, "rename failed");
}

typedef struct rename_ctx {
    const pc_atomic_fs *fs;
    const char *from;
    const char *to;
} rename_ctx;

static int do_rename(void *ctx)
{
    const rename_ctx *c = ctx;
    return c->fs->rename(c->fs->ctx, c->from, c->to);
}

pc_status pc_atomic_write_with(const pc_atomic_fs *fs, pc_rename_retry retry, const char *path,
                               const void *data, size_t len, pc_error *err)
{
    char *resolved = NULL;
    char *dir = NULL;
    const char *target = path;
    const char *name;
    const char *slash;
    pc_buf tmp = PC_BUF_INIT;
    pc_buf leaf = PC_BUF_INIT;
    struct stat st;
    pc_status s = PC_OK;
    int attempt;
    int e = EEXIST;

    /* Write through a symlink to the file it points at, instead of replacing the link. */
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        resolved = realpath(path, NULL);
        if (resolved != NULL) {
            target = resolved;
        }
    }
    slash = strrchr(target, '/');
    name = slash != NULL ? slash + 1 : target;
    if (name[0] == '\0') {
        s = pc_fail(err, PC_ERR_INVALID, "%s: not a file path", path);
        goto cleanup;
    }
    if (slash == NULL) {
        dir = pc_strndup(".", 1);
    } else {
        size_t dir_len = (size_t)(slash - target);
        dir = pc_strndup(dir_len == 0 ? "/" : target, dir_len == 0 ? 1 : dir_len);
    }
    if (dir == NULL) {
        s = pc_fail(err, PC_ERR_NOMEM, "out of memory");
        goto cleanup;
    }
    s = pc_mkdir_p(dir, err);
    if (s != PC_OK) {
        goto cleanup;
    }

    /* A fresh temporary file beside the target (same directory: the rename never crosses a
     * device). The name is unique per process and call; retry a few on collisions. */
    for (attempt = 0; attempt < 8 && e == EEXIST; attempt++) {
        pc_buf_truncate(&tmp, 0);
        if ((s = pc_atomic_temp_name(name, &leaf, err)) != PC_OK ||
            (s = pc_buf_appendf(&tmp, err, "%s/%s", dir, pc_buf_cstr(&leaf))) != PC_OK) {
            goto cleanup;
        }
        e = fs->write_new(fs->ctx, pc_buf_cstr(&tmp), data, len);
        if (e != 0 && e != EEXIST) {
            /* A partial temporary file may exist; the destination was never touched. */
            (void)fs->remove(fs->ctx, pc_buf_cstr(&tmp));
            s = write_error(err, e, path, dir);
            goto cleanup;
        }
    }
    if (e != 0) {
        s = pc_fail(err, PC_ERR_IO, "%s: could not create a temporary file", dir);
        if (err != NULL) {
            err->sys_errno = EEXIST;
        }
        goto cleanup;
    }
    /* Keep the existing file's mode (e.g. 0600) instead of the process default. */
    if (stat(target, &st) == 0) {
        (void)fs->copy_permissions(fs->ctx, target, pc_buf_cstr(&tmp));
    }
    {
        rename_ctx rc = {fs, pc_buf_cstr(&tmp), target};
        e = pc_atomic_retry_rename(retry, do_rename, &rc);
    }
    if (e != 0) {
        (void)fs->remove(fs->ctx, pc_buf_cstr(&tmp));
        s = rename_error(err, e, path);
        goto cleanup;
    }
    /* The new bytes are in place; a failed directory fsync can't be undone, so best effort. */
    (void)fs->sync_dir(fs->ctx, dir);
cleanup:
    pc_buf_fini(&tmp);
    pc_buf_fini(&leaf);
    free(dir);
    free(resolved);
    return s;
}

pc_status pc_file_write_atomic(const char *path, const void *data, size_t len, pc_error *err)
{
    return pc_atomic_write_with(&pc_atomic_real_fs, pc_rename_retry_platform(), path, data, len, err);
}

pc_status pc_atomic_write(const char *path, const void *data, size_t len, pc_error *err)
{
    return pc_file_write_atomic(path, data, len, err);
}
