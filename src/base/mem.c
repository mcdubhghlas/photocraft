#include "mem.h"

#include <stdlib.h>
#include <string.h>

#include "num.h"

void *pc_malloc(size_t n)
{
    if (n > PC_MAX_ALLOC) {
        return NULL;
    }
    return malloc(n == 0 ? 1 : n);
}

void *pc_malloc_array(size_t count, size_t size)
{
    size_t n;
    if (!pc_size_mul(count, size, &n)) {
        return NULL;
    }
    return pc_malloc(n);
}

void *pc_calloc_array(size_t count, size_t size)
{
    size_t n;
    if (!pc_size_mul(count, size, &n) || n > PC_MAX_ALLOC) {
        return NULL;
    }
    if (n == 0) {
        return calloc(1, 1);
    }
    return calloc(count, size);
}

void *pc_grow_array(void *ptr, size_t *cap, size_t need, size_t elem)
{
    size_t new_cap;
    size_t bytes;
    void *p;

    if (elem == 0) {
        return NULL;
    }
    if (need <= *cap && ptr != NULL) {
        return ptr;
    }
    /* Double, but at least `need` and at least 8; never past what PC_MAX_ALLOC allows. */
    new_cap = *cap < 4 ? 8 : *cap;
    if (*cap >= 4 && !pc_size_mul(*cap, 2, &new_cap)) {
        new_cap = need;
    }
    if (new_cap < need) {
        new_cap = need;
    }
    if (!pc_size_mul(new_cap, elem, &bytes) || bytes > PC_MAX_ALLOC) {
        /* The doubled size is too big: retry with exactly what was asked for. */
        new_cap = need;
        if (!pc_size_mul(new_cap, elem, &bytes) || bytes > PC_MAX_ALLOC) {
            return NULL;
        }
    }
    p = realloc(ptr, bytes == 0 ? 1 : bytes);
    if (p == NULL) {
        return NULL;
    }
    *cap = new_cap;
    return p;
}

char *pc_strndup(const char *s, size_t len)
{
    size_t n;
    char *d;
    if (!pc_size_add(len, 1, &n)) {
        return NULL;
    }
    d = pc_malloc(n);
    if (d == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(d, s, len);
    }
    d[len] = '\0';
    return d;
}
