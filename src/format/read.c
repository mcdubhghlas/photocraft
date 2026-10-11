#include "format/read.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void pc_read_size_text(uint64_t len, char out[48])
{
    static const char *const UNITS[5] = {"bytes", "KB", "MB", "GB", "TB"};
    double v = (double)len;
    size_t unit = 0;
    while (v >= 1000.0 && unit + 1 < 5) {
        v /= 1000.0;
        unit++;
    }
    if (unit > 0) {
        (void)snprintf(out, 48, "%.2f %s", v, UNITS[unit]);
    } else {
        (void)snprintf(out, 48, "%llu bytes", (unsigned long long)len);
    }
}

pc_status pc_read_buffer_for(uint64_t len, pc_buf *out, pc_error *err)
{
    uint8_t *p = NULL;
    /* Room for the terminator pc_buf keeps after its contents. */
    if (len < PC_READ_MAX_RESERVE && len < (uint64_t)PTRDIFF_MAX && len < (uint64_t)SIZE_MAX) {
        p = malloc((size_t)len + 1);
    }
    if (p == NULL) {
        char size[48];
        pc_read_size_text(len, size);
        return pc_fail(err, PC_ERR_NOMEM, "not enough memory to read the whole file (%s)", size);
    }
    p[0] = '\0';
    pc_buf_fini(out);
    out->data = p;
    out->len = 0;
    out->cap = (size_t)len + 1;
    return PC_OK;
}

static pc_status io_fail(pc_error *err, int e)
{
    pc_status s = pc_fail(err, e == ENOENT ? PC_ERR_NOT_FOUND : PC_ERR_IO, "%s", strerror(e));
    if (err != NULL) {
        err->sys_errno = e;
    }
    return s;
}

pc_status pc_read_all_with(int fd, uint64_t size_hint, size_t chunk, pc_buf *out, pc_error *err)
{
    pc_buf buf = PC_BUF_INIT;
    pc_status s = pc_read_buffer_for(size_hint, &buf, err);
    if (s != PC_OK) {
        return s;
    }
    if (chunk == 0) {
        chunk = 1; /* a zero chunk still makes progress */
    }
    if (chunk > (size_t)SSIZE_MAX) {
        chunk = (size_t)SSIZE_MAX;
    }
    for (;;) {
        size_t room = buf.cap - 1 - buf.len;
        ssize_t n;
        if (room == 0) {
            /* The file is longer than its hint (or the hint was 0): grow geometrically. */
            size_t grow = buf.len < 8192 ? 8192 : buf.len;
            s = pc_buf_reserve(&buf, grow < chunk ? grow : chunk, err);
            if (s != PC_OK) {
                goto fail;
            }
            room = buf.cap - 1 - buf.len;
        }
        n = read(fd, buf.data + buf.len, room < chunk ? room : chunk);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            s = io_fail(err, errno);
            goto fail;
        }
        if (n == 0) {
            break;
        }
        buf.len += (size_t)n;
        buf.data[buf.len] = '\0';
    }
    pc_buf_fini(out);
    *out = buf;
    return PC_OK;
fail:
    pc_buf_fini(&buf);
    return s;
}

pc_status pc_read_all(int fd, uint64_t size_hint, pc_buf *out, pc_error *err)
{
    return pc_read_all_with(fd, size_hint, PC_READ_CHUNK, out, err);
}

pc_status pc_read_file_with(const char *path, size_t chunk, pc_buf *out, pc_error *err)
{
    struct stat st;
    uint64_t len = 0;
    pc_status s;
    int fd;

    do {
        fd = open(path, O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return io_fail(err, errno);
    }
    if (fstat(fd, &st) != 0) {
        st.st_mode = 0;
        st.st_size = 0;
    }
    if (st.st_size > 0) {
        len = (uint64_t)st.st_size; /* metadata failing only means no size hint, as in the reference */
    }
    if (S_ISDIR(st.st_mode)) {
        s = io_fail(err, EISDIR); /* reading a directory fails like read(2) does */
    } else {
        s = pc_read_all_with(fd, len, chunk, out, err);
    }
    (void)close(fd);
    return s;
}

pc_status pc_read_file(const char *path, pc_buf *out, pc_error *err)
{
    return pc_read_file_with(path, PC_READ_CHUNK, out, err);
}
