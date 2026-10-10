#include "blob.h"

#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

struct pc_blob {
    atomic_size_t refs;
    size_t len;
    alignas(max_align_t) uint8_t data[];
};

pc_status pc_blob_alloc(size_t len, pc_blob **out, pc_error *err)
{
    pc_blob *b;
    if (len > PC_MAX_ALLOC - sizeof(pc_blob)) {
        return pc_fail(err, PC_ERR_LIMIT, "blob too large");
    }
    b = calloc(1, sizeof(pc_blob) + len);
    if (b == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "out of memory");
    }
    atomic_init(&b->refs, 1);
    b->len = len;
    *out = b;
    return PC_OK;
}

pc_status pc_blob_new(const void *data, size_t len, pc_blob **out, pc_error *err)
{
    pc_blob *b = NULL;
    pc_status st = pc_blob_alloc(len, &b, err);
    if (st != PC_OK) {
        return st;
    }
    if (len > 0) {
        memcpy(b->data, data, len);
    }
    *out = b;
    return PC_OK;
}

pc_blob *pc_blob_retain(pc_blob *b)
{
    if (b != NULL) {
        atomic_fetch_add_explicit(&b->refs, 1, memory_order_relaxed);
    }
    return b;
}

void pc_blob_release(pc_blob *b)
{
    if (b != NULL && atomic_fetch_sub_explicit(&b->refs, 1, memory_order_acq_rel) == 1) {
        free(b);
    }
}

const uint8_t *pc_blob_data(const pc_blob *b) { return b != NULL ? b->data : NULL; }

uint8_t *pc_blob_data_mut(pc_blob *b) { return b != NULL ? b->data : NULL; }

size_t pc_blob_len(const pc_blob *b) { return b != NULL ? b->len : 0; }

bool pc_blob_eq(const pc_blob *a, const pc_blob *b)
{
    if (a == b) {
        return true;
    }
    if (a == NULL || b == NULL) {
        return false;
    }
    return a->len == b->len && (a->len == 0 || memcmp(a->data, b->data, a->len) == 0);
}
