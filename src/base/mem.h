/* Allocation helpers with overflow checks and a per-allocation cap.
 *
 * All return NULL on failure (size overflow, cap exceeded, or out of memory) and never abort.
 * Memory from these helpers is released with plain free(). Thread-safe. */
#ifndef PC_BASE_MEM_H
#define PC_BASE_MEM_H

#include <stddef.h>
#include <stdint.h>

/* Largest single allocation PhotoCraft will attempt. Guards against sizes taken from hostile
 * files; modules apply tighter domain limits (pixel counts, chunk lengths) before this. */
#if SIZE_MAX > 0xFFFFFFFFu
#define PC_MAX_ALLOC ((size_t)1 << 34) /* 16 GiB */
#else
#define PC_MAX_ALLOC (SIZE_MAX / 2)
#endif

/* malloc(n); a zero-byte request returns a valid unique pointer. */
void *pc_malloc(size_t n);

/* Zeroed array of count * size bytes, overflow-checked. */
void *pc_calloc_array(size_t count, size_t size);

/* Uninitialised array of count * size bytes, overflow-checked. */
void *pc_malloc_array(size_t count, size_t size);

/* Grow an array so it holds at least `need` elements of `elem` bytes.
 *
 * `ptr` is the current array (may be NULL), `*cap` its capacity in elements. On success returns
 * the (possibly moved) array and updates *cap; the caller must replace its pointer with the
 * result. On failure returns NULL, leaves *cap unchanged, and the old array stays valid and owned
 * by the caller. Capacity grows geometrically, so repeated appends are amortised O(1). */
void *pc_grow_array(void *ptr, size_t *cap, size_t need, size_t elem);

/* Duplicate a byte range into a new NUL-terminated string (len bytes + '\0'). NULL on failure. */
char *pc_strndup(const char *s, size_t len);

#endif
