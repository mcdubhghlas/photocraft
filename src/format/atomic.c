/* atomic::atomic_write_progress (crates/format/src/atomic.rs): the base atomic write (base/file.h)
 * through a file system whose write step reports how far it has got. */
#include "format/atomic.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* How much is written between progress reports (atomic.rs WRITE_CHUNK). */
#define WRITE_CHUNK ((size_t)8 << 20)

typedef struct progress_fs {
    const pc_atomic_fs *inner;
    pc_atomic_progress_fn progress;
    void *ctx;
} progress_fs;

static int progress_write_new(void *ctx, const char *tmp, const void *data, size_t len)
{
    const progress_fs *p = ctx;
    const unsigned char *bytes = data;
    size_t written = 0;
    pc_error e;
    int fd;
    int r = 0;
    do {
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return errno;
    }
    /* One report per chunk, the last one 1.0 (none for empty data, as bytes.chunks). */
    while (written < len) {
        size_t n = len - written < WRITE_CHUNK ? len - written : WRITE_CHUNK;
        if (pc_fd_write(fd, bytes + written, n, &e) != PC_OK) {
            r = e.sys_errno != 0 ? e.sys_errno : EIO;
            break;
        }
        written += n;
        if (p->progress != NULL) {
            p->progress(p->ctx, (float)written / (float)len);
        }
    }
    if (r == 0 && fsync(fd) != 0) {
        r = errno;
    }
    if (close(fd) != 0 && r == 0 && errno != EINTR) {
        r = errno;
    }
    return r;
}

static int progress_rename(void *ctx, const char *from, const char *to)
{
    const progress_fs *p = ctx;
    return p->inner->rename(p->inner->ctx, from, to);
}

static int progress_remove(void *ctx, const char *path)
{
    const progress_fs *p = ctx;
    return p->inner->remove(p->inner->ctx, path);
}

static int progress_sync_dir(void *ctx, const char *dir)
{
    const progress_fs *p = ctx;
    return p->inner->sync_dir(p->inner->ctx, dir);
}

static int progress_copy_permissions(void *ctx, const char *from, const char *to)
{
    const progress_fs *p = ctx;
    return p->inner->copy_permissions(p->inner->ctx, from, to);
}

pc_status pc_atomic_write_progress(const char *path, const void *data, size_t len,
                                   pc_atomic_progress_fn progress, void *ctx, pc_error *err)
{
    progress_fs p = {&pc_atomic_real_fs, progress, ctx};
    pc_atomic_fs fs = {&p,
                       progress_write_new,
                       progress_rename,
                       progress_remove,
                       progress_sync_dir,
                       progress_copy_permissions};
    return pc_atomic_write_with(&fs, pc_rename_retry_platform(), path, data, len, err);
}
