#include "buf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"
#include "num.h"

void pc_buf_init(pc_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void pc_buf_fini(pc_buf *b)
{
    if (b != NULL) {
        free(b->data);
        pc_buf_init(b);
    }
}

pc_status pc_buf_reserve(pc_buf *b, size_t extra, pc_error *err)
{
    size_t need;
    uint8_t *p;
    if (!pc_size_add(b->len, extra, &need) || !pc_size_add(need, 1, &need)) {
        return pc_fail(err, PC_ERR_LIMIT, "buffer size overflow");
    }
    if (need <= b->cap && b->data != NULL) {
        return PC_OK;
    }
    p = pc_grow_array(b->data, &b->cap, need, 1);
    if (p == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory growing a buffer to %zu bytes", need);
    }
    b->data = p;
    b->data[b->len] = '\0';
    return PC_OK;
}

pc_status pc_buf_append(pc_buf *b, const void *data, size_t len, pc_error *err)
{
    pc_status s = pc_buf_reserve(b, len, err);
    if (s != PC_OK) {
        return s;
    }
    if (len > 0) {
        memcpy(b->data + b->len, data, len);
    }
    b->len += len;
    b->data[b->len] = '\0';
    return PC_OK;
}

pc_status pc_buf_append_byte(pc_buf *b, uint8_t byte, pc_error *err)
{
    return pc_buf_append(b, &byte, 1, err);
}

pc_status pc_buf_append_str(pc_buf *b, const char *s, pc_error *err)
{
    return pc_buf_append(b, s, strlen(s), err);
}

pc_status pc_buf_appendf(pc_buf *b, pc_error *err, const char *fmt, ...)
{
    va_list ap;
    int n;
    pc_status s;

    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return pc_fail(err, PC_ERR_INVALID, "bad format string");
    }
    s = pc_buf_reserve(b, (size_t)n, err);
    if (s != PC_OK) {
        return s;
    }
    va_start(ap, fmt);
    n = vsnprintf((char *)b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->data[b->len] = '\0';
        return pc_fail(err, PC_ERR_INVALID, "bad format string");
    }
    b->len += (size_t)n;
    return PC_OK;
}

void pc_buf_truncate(pc_buf *b, size_t len)
{
    if (len < b->len) {
        b->len = len;
        b->data[len] = '\0';
    }
}

const char *pc_buf_cstr(const pc_buf *b)
{
    return b->data != NULL ? (const char *)b->data : "";
}

uint8_t *pc_buf_take(pc_buf *b, size_t *len_out)
{
    uint8_t *p = b->data;
    if (len_out != NULL) {
        *len_out = b->len;
    }
    pc_buf_init(b);
    return p;
}
