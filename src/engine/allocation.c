/* Fallible buffers used by memory-intensive document operations (port of allocation.rs).
 *
 * The destructive commands (fills, clears, flips, transforms) reserve their working buffers
 * through these helpers instead of the abort-on-OOM allocator path, so a document far too large
 * to process fails with an error -- and leaves the document untouched -- rather than terminating
 * the process (AGENTS.md "never crash"). The error text reproduces the reference's EngineError
 * strings verbatim, so tests and the control protocol see the same message.
 *
 * A test-only failpoint (pc_engine_alloc_fail_next) lets command tests prove their transaction
 * boundary: the next capacity()/checkpoint() reports OOM. It is compiled into the library (so the
 * tests that link it can arm it) but production code never arms it. */
#include "allocation.h"

#include <stdlib.h>
#include <string.h>

#include "base/num.h"
#include "engine_internal.h"

/* Armed by pc_engine_alloc_fail_next(); consumed (cleared) by the next capacity/checkpoint. Per
 * thread, mirroring the reference's thread_local Cell<bool>. */
static _Thread_local bool g_fail_next;

void pc_engine_alloc_fail_next(void)
{
    g_fail_next = true;
}

/* Consume the failpoint: true (and cleared) when the next allocation should report OOM. */
static bool take_fail_next(void)
{
    bool f = g_fail_next;
    g_fail_next = false;
    return f;
}

pc_status pc_engine_alloc_error(pc_error *err, const char *purpose, const char *detail)
{
    /* EngineError::Other, so the message is the contract and the variant maps to PC_ERR_INVALID. */
    return pc_engine_err_other(err, "not enough memory for %s (%s); the document was not changed",
                               purpose, detail);
}

pc_status pc_engine_alloc_capacity(size_t count, size_t elem, const char *purpose, void **out, pc_error *err)
{
    size_t bytes;
    if (!pc_size_mul(count, elem, &bytes)) {
        *out = NULL;
        return pc_engine_alloc_error(err, purpose, "requested size overflowed");
    }
    if (take_fail_next()) {
        *out = NULL;
        return pc_engine_alloc_error(err, purpose, "not enough memory");
    }
    if (bytes == 0) {
        *out = NULL; /* an empty reservation needs no allocation */
        return PC_OK;
    }
    void *p = malloc(bytes);
    if (p == NULL) {
        char detail[64];
        (void)snprintf(detail, sizeof detail, "not enough memory for %zu bytes", bytes);
        *out = NULL;
        return pc_engine_alloc_error(err, purpose, detail);
    }
    *out = p;
    return PC_OK;
}

pc_status pc_engine_alloc_filled(size_t count, size_t elem, const char *purpose, void **out, pc_error *err)
{
    pc_status st = pc_engine_alloc_capacity(count, elem, purpose, out, err);
    if (st != PC_OK) {
        return st;
    }
    if (*out != NULL) {
        memset(*out, 0, count * elem); /* every call site fills with the zero value (0.0f / 0) */
    }
    return PC_OK;
}

pc_status pc_engine_alloc_checkpoint(const char *purpose, pc_error *err)
{
    if (take_fail_next()) {
        return pc_engine_alloc_error(err, purpose, "not enough memory");
    }
    return PC_OK;
}

pc_status pc_engine_alloc_read_region(const pc_surface *s, pc_rect area, const char *purpose, float **out,
                                      size_t *len, pc_error *err)
{
    size_t w = (size_t)pc_rect_width(area);
    size_t h = (size_t)pc_rect_height(area);
    size_t n;
    if (!pc_size_mul3(w, h, pc_surface_channels(s), &n)) {
        *out = NULL;
        return pc_engine_alloc_error(err, purpose, "requested size overflowed");
    }
    void *buf = NULL;
    pc_status st = pc_engine_alloc_filled(n, sizeof(float), purpose, &buf, err);
    if (st != PC_OK) {
        return st;
    }
    if (pc_surface_read_region_into(s, area, (float *)buf, n, err) != PC_OK) {
        free(buf);
        return err != NULL ? err->code : PC_ERR_INVALID;
    }
    *out = (float *)buf;
    *len = n;
    return PC_OK;
}
