/* realpath() is an XSI interface in POSIX.1-2008 (D21). */
#define _XOPEN_SOURCE 700

/* The hardened file operations of directory bundle saves (store.rs: bundle_root,
 * validate_bundle_file, write_bundle_file, remove_bundle_file). See bundle_dir.h. */
#include "format/bundle_dir.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "base/buf.h"
#include "base/file.h"
#include "base/mem.h"
#include "format/format.h"

static const char *const MANAGED[3] = {"tiles", "blobs", "composite"};

static pc_status io_fail(pc_error *err, int e, const char *what, const char *path)
{
    pc_status s =
        pc_fail(err, e == ENOENT ? PC_ERR_NOT_FOUND : PC_ERR_IO, "%s %s: %s", what, path, strerror(e));
    if (err != NULL) {
        err->sys_errno = e;
    }
    return pc_format_io(s, err);
}

pc_status pc_bundle_unsafe_entry(const char *path, pc_error *err)
{
    pc_status s = pc_fail(err, PC_ERR_IO,
                          "refusing directory bundle save: %s is a symlink or not a regular file", path);
    return pc_format_io(s, err);
}

static pc_status unsafe_directory(const char *path, pc_error *err)
{
    pc_status s =
        pc_fail(err, PC_ERR_IO, "refusing directory bundle save: %s is a symlink or not a directory", path);
    return pc_format_io(s, err);
}

pc_status pc_bundle_lstat(const char *path, bool *exists, bool *is_link, bool *is_file, pc_error *err)
{
    struct stat st;
    *exists = false;
    *is_link = false;
    *is_file = false;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? PC_OK : io_fail(err, errno, "cannot inspect", path);
    }
    *exists = true;
    *is_link = S_ISLNK(st.st_mode);
    *is_file = S_ISREG(st.st_mode);
    return PC_OK;
}

pc_status pc_bundle_plain_file_or_missing(const char *path, pc_error *err)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? PC_OK : io_fail(err, errno, "cannot inspect", path);
    }
    return S_ISREG(st.st_mode) ? PC_OK : pc_bundle_unsafe_entry(path, err);
}

pc_status pc_bundle_plain_directory_or_missing(const char *path, pc_error *err)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? PC_OK : io_fail(err, errno, "cannot inspect", path);
    }
    return S_ISDIR(st.st_mode) ? PC_OK : unsafe_directory(path, err);
}

/* *same = realpath(path) is path itself. */
static pc_status is_canonical(const char *path, bool *same, pc_error *err)
{
    char *c = realpath(path, NULL);
    if (c == NULL) {
        return errno == ENOMEM ? pc_fail(err, PC_ERR_NOMEM, "out of memory")
                               : io_fail(err, errno, "cannot resolve", path);
    }
    *same = strcmp(c, path) == 0;
    free(c);
    return PC_OK;
}

static pc_status join(const char *dir, const char *leaf, pc_buf *out, pc_error *err)
{
    size_t dl = strlen(dir);
    pc_buf_truncate(out, 0);
    return pc_buf_appendf(out, err, "%s%s%s", dir, dl > 0 && dir[dl - 1] == '/' ? "" : "/", leaf);
}

/* dir with empty and "." components dropped (Path::components), "/" kept for a root. */
static pc_status normalise(const char *dir, pc_buf *out, pc_error *err)
{
    const char *p = dir;
    pc_status s = PC_OK;
    pc_buf_truncate(out, 0);
    if (*p == '/') {
        s = pc_buf_append(out, "/", 1, err);
    }
    while (s == PC_OK && *p != '\0') {
        const char *q;
        size_t n;
        while (*p == '/') {
            p++;
        }
        q = p;
        while (*q != '\0' && *q != '/') {
            q++;
        }
        n = (size_t)(q - p);
        if (n > 0 && !(n == 1 && p[0] == '.')) {
            if (out->len > 0 && out->data[out->len - 1] != '/') {
                s = pc_buf_append(out, "/", 1, err);
            }
            if (s == PC_OK) {
                s = pc_buf_append(out, p, n, err);
            }
        }
        p = q;
    }
    if (s == PC_OK && out->len == 0) {
        s = pc_buf_append(out, ".", 1, err);
    }
    return s;
}

pc_status pc_bundle_root(const char *dir, char **root_out, pc_error *err)
{
    pc_buf norm = PC_BUF_INIT;
    pc_buf path = PC_BUF_INIT;
    char *root = NULL;
    pc_status s = normalise(dir, &norm, err);
    if (s == PC_OK) {
        s = pc_mkdir_p(pc_buf_cstr(&norm), err);
        if (s != PC_OK && s != PC_ERR_NOMEM) {
            s = pc_format_io(s, err);
        }
    }
    if (s == PC_OK) {
        root = realpath(pc_buf_cstr(&norm), NULL);
        if (root == NULL) {
            s = errno == ENOMEM ? pc_fail(err, PC_ERR_NOMEM, "out of memory")
                                : io_fail(err, errno, "cannot resolve", pc_buf_cstr(&norm));
        }
    }
    if (s == PC_OK && root != NULL) {
        s = pc_bundle_plain_directory_or_missing(root, err);
    }
    for (size_t k = 0; k < 3 && s == PC_OK && root != NULL; k++) {
        bool same = false;
        s = join(root, MANAGED[k], &path, err);
        if (s == PC_OK) {
            s = pc_bundle_plain_directory_or_missing(pc_buf_cstr(&path), err);
        }
        if (s == PC_OK && mkdir(pc_buf_cstr(&path), 0777) != 0 && errno != EEXIST) {
            s = io_fail(err, errno, "cannot create", pc_buf_cstr(&path));
        }
        if (s == PC_OK) {
            s = pc_bundle_plain_directory_or_missing(pc_buf_cstr(&path), err);
        }
        if (s == PC_OK) {
            s = is_canonical(pc_buf_cstr(&path), &same, err);
        }
        if (s == PC_OK && !same) {
            s = pc_bundle_unsafe_entry(pc_buf_cstr(&path), err);
        }
    }
    pc_buf_fini(&norm);
    pc_buf_fini(&path);
    if (s != PC_OK) {
        free(root);
        return s;
    }
    *root_out = root;
    return PC_OK;
}

/* The parent directory of path into *out (path has a '/' when built by join). */
static pc_status parent_of(const char *path, pc_buf *out, pc_error *err)
{
    const char *slash = strrchr(path, '/');
    pc_buf_truncate(out, 0);
    if (slash == NULL) {
        return pc_bundle_unsafe_entry(path, err);
    }
    if (slash == path) {
        return pc_buf_append(out, "/", 1, err);
    }
    return pc_buf_append(out, path, (size_t)(slash - path), err);
}

static bool is_managed_parent(const char *root, const char *parent)
{
    size_t rl = strlen(root);
    if (strcmp(parent, root) == 0) {
        return true;
    }
    if (strncmp(parent, root, rl) != 0) {
        return false;
    }
    if (!(rl > 0 && root[rl - 1] == '/')) {
        if (parent[rl] != '/') {
            return false;
        }
        rl++;
    }
    for (size_t k = 0; k < 3; k++) {
        if (strcmp(parent + rl, MANAGED[k]) == 0) {
            return true;
        }
    }
    return false;
}

pc_status pc_bundle_validate_file(const char *root, const char *path, pc_error *err)
{
    pc_buf parent = PC_BUF_INIT;
    bool same = false;
    pc_status s = parent_of(path, &parent, err);
    if (s == PC_OK && !is_managed_parent(root, pc_buf_cstr(&parent))) {
        s = pc_bundle_unsafe_entry(path, err);
    }
    if (s == PC_OK) {
        s = pc_bundle_plain_directory_or_missing(pc_buf_cstr(&parent), err);
    }
    if (s == PC_OK) {
        s = is_canonical(pc_buf_cstr(&parent), &same, err);
    }
    if (s == PC_OK && !same) {
        s = pc_bundle_unsafe_entry(pc_buf_cstr(&parent), err);
    }
    pc_buf_fini(&parent);
    return s == PC_OK ? pc_bundle_plain_file_or_missing(path, err) : s;
}

typedef struct rename_ctx {
    const char *from;
    const char *to;
} rename_ctx;

static int do_rename(void *ctx)
{
    const rename_ctx *r = ctx;
    return rename(r->from, r->to) == 0 ? 0 : errno;
}

/* Writes data to the fresh file fd and flushes it; 0 or an errno value. */
static int write_flush(int fd, const void *data, size_t len)
{
    pc_error e;
    if (pc_fd_write(fd, data, len, &e) != PC_OK) {
        return e.sys_errno != 0 ? e.sys_errno : EIO;
    }
    return fsync(fd) == 0 ? 0 : errno;
}

pc_status pc_bundle_write_file(const char *root, const char *path, const void *data, size_t len,
                               pc_error *err)
{
    pc_buf parent = PC_BUF_INIT;
    pc_buf leaf = PC_BUF_INIT;
    pc_buf tmp = PC_BUF_INIT;
    const char *name;
    int fd = -1;
    bool created = false;
    bool same = false;
    struct stat st;
    pc_status s = pc_bundle_validate_file(root, path, err);
    if (s == PC_OK) {
        s = parent_of(path, &parent, err);
    }
    if (s != PC_OK) {
        goto done;
    }
    name = strrchr(path, '/') + 1;
    for (int attempt = 0; attempt < 8 && fd < 0 && s == PC_OK; attempt++) {
        s = pc_atomic_temp_name(name, &leaf, err);
        if (s == PC_OK) {
            s = join(pc_buf_cstr(&parent), pc_buf_cstr(&leaf), &tmp, err);
        }
        if (s != PC_OK) {
            break;
        }
        do {
            fd = open(pc_buf_cstr(&tmp), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0 && errno != EEXIST) {
            s = io_fail(err, errno, "cannot create", pc_buf_cstr(&tmp));
        }
    }
    if (s != PC_OK) {
        goto done;
    }
    if (fd < 0) {
        s = pc_format_io(pc_fail(err, PC_ERR_IO, "could not create a temporary file beside %s", path), err);
        goto done;
    }
    created = true;
    /* The parent may have changed between the check and the create: check where the new file
     * really is before putting document bytes into it. */
    s = is_canonical(pc_buf_cstr(&tmp), &same, err);
    if (s == PC_OK && !same) {
        s = pc_bundle_unsafe_entry(pc_buf_cstr(&tmp), err);
    }
    if (s == PC_OK) {
        s = pc_bundle_validate_file(root, path, err);
    }
    if (s == PC_OK) {
        int e = write_flush(fd, data, len);
        if (e != 0) {
            s = io_fail(err, e, "cannot write", pc_buf_cstr(&tmp));
        }
    }
    if (close(fd) != 0 && s == PC_OK && errno != EINTR) {
        s = io_fail(err, errno, "cannot write", pc_buf_cstr(&tmp));
    }
    /* Keep the existing file's mode where possible, as the generic atomic writer does. */
    if (s == PC_OK && stat(path, &st) == 0) {
        (void)chmod(pc_buf_cstr(&tmp), st.st_mode & 07777);
    }
    /* Recheck just before publishing; rename replaces a symlink itself, never its referent. */
    if (s == PC_OK) {
        s = pc_bundle_validate_file(root, path, err);
    }
    if (s == PC_OK) {
        rename_ctx rc = {pc_buf_cstr(&tmp), path};
        int e = pc_atomic_retry_rename(pc_rename_retry_platform(), do_rename, &rc);
        if (e != 0) {
            s = io_fail(err, e, "cannot rename onto", path);
        }
    }
    if (s == PC_OK) {
        int dfd = open(pc_buf_cstr(&parent), O_RDONLY | O_CLOEXEC);
        if (dfd >= 0) {
            (void)fsync(dfd);
            (void)close(dfd);
        }
    } else if (created && pc_bundle_validate_file(root, pc_buf_cstr(&tmp), NULL) == PC_OK) {
        (void)unlink(pc_buf_cstr(&tmp));
    }
done:
    pc_buf_fini(&parent);
    pc_buf_fini(&leaf);
    pc_buf_fini(&tmp);
    return s;
}

pc_status pc_bundle_remove_file(const char *root, const char *path, bool *missing, pc_error *err)
{
    pc_status s = pc_bundle_validate_file(root, path, err);
    if (missing != NULL) {
        *missing = false;
    }
    if (s != PC_OK) {
        return s;
    }
    if (unlink(path) != 0) {
        if (errno == ENOENT && missing != NULL) {
            *missing = true;
            return PC_OK;
        }
        return io_fail(err, errno, "cannot remove", path);
    }
    return PC_OK;
}
