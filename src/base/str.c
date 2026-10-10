#include "str.h"

#include <stdlib.h>
#include <string.h>

#include "mem.h"

void pc_str_fini(pc_str *s)
{
    if (s == NULL) {
        return;
    }
    free(s->ptr);
    s->ptr = NULL;
    s->len = 0;
}

pc_status pc_str_set(pc_str *s, const char *p, size_t len, pc_error *err)
{
    char *n;
    if (len == 0) {
        pc_str_fini(s);
        return PC_OK;
    }
    if (len >= PC_MAX_ALLOC) {
        return pc_fail(err, PC_ERR_LIMIT, "string too long");
    }
    n = pc_malloc(len + 1);
    if (n == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memcpy(n, p, len);
    n[len] = '\0';
    free(s->ptr);
    s->ptr = n;
    s->len = len;
    return PC_OK;
}

pc_status pc_str_set_c(pc_str *s, const char *cstr, pc_error *err)
{
    return pc_str_set(s, cstr, cstr != NULL ? strlen(cstr) : 0, err);
}

pc_status pc_str_clone(const pc_str *s, pc_str *out, pc_error *err)
{
    pc_str t = PC_STR_INIT;
    pc_status st = pc_str_set(&t, s->ptr, s->len, err);
    if (st == PC_OK) {
        *out = t;
    }
    return st;
}

pc_str pc_str_adopt(char *p, size_t len)
{
    pc_str s;
    if (len == 0) {
        free(p);
        p = NULL;
    }
    s.ptr = p;
    s.len = len;
    return s;
}

pc_status pc_str_append(pc_str *s, const char *p, size_t len, pc_error *err)
{
    char *n;
    if (len == 0) {
        return PC_OK;
    }
    if (len >= PC_MAX_ALLOC - s->len) {
        return pc_fail(err, PC_ERR_LIMIT, "string too long");
    }
    n = realloc(s->ptr, s->len + len + 1);
    if (n == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    memcpy(n + s->len, p, len);
    s->len += len;
    n[s->len] = '\0';
    s->ptr = n;
    return PC_OK;
}

bool pc_str_eq_mem(const pc_str *a, const char *p, size_t len)
{
    return a->len == len && (len == 0 || memcmp(a->ptr, p, len) == 0);
}

bool pc_str_eq(const pc_str *a, const pc_str *b) { return pc_str_eq_mem(a, b->ptr, b->len); }

bool pc_str_eq_c(const pc_str *a, const char *cstr) { return pc_str_eq_mem(a, cstr, strlen(cstr)); }

int pc_str_cmp(const pc_str *a, const pc_str *b)
{
    size_t n = a->len < b->len ? a->len : b->len;
    int c = n == 0 ? 0 : memcmp(a->ptr, b->ptr, n);
    if (c != 0) {
        return c;
    }
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}
