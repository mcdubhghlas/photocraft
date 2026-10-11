/* Stable sort matching Rust's slice::sort_by for small inputs (see stable_sort.h). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "compose/stable_sort.h"

/* Rust's driftsort sorts slices up to this length with insertion sort. */
#define SMALL_SORT 20

static void insertion(unsigned char *b, size_t n, size_t size, pc_cmp_less_fn less)
{
    unsigned char tmp[64];
    size_t i;
    for (i = 1; i < n; i++) {
        size_t j = i;
        if (!less(b + i * size, b + (i - 1) * size)) {
            continue;
        }
        memcpy(tmp, b + i * size, size);
        while (j > 0 && less(tmp, b + (j - 1) * size)) {
            memcpy(b + j * size, b + (j - 1) * size, size);
            j--;
        }
        memcpy(b + j * size, tmp, size);
    }
}

/* Bottom-up merge sort over runs of SMALL_SORT insertion-sorted elements. */
static void merge_sort(unsigned char *b, unsigned char *t, size_t n, size_t size, pc_cmp_less_fn less)
{
    size_t width;
    size_t i;
    for (i = 0; i < n; i += SMALL_SORT) {
        insertion(b + i * size, n - i < SMALL_SORT ? n - i : SMALL_SORT, size, less);
    }
    for (width = SMALL_SORT; width < n; width *= 2) {
        for (i = 0; i < n; i += 2 * width) {
            size_t mid = i + width < n ? i + width : n;
            size_t end = i + 2 * width < n ? i + 2 * width : n;
            size_t l = i;
            size_t r = mid;
            size_t k = i;
            while (l < mid && r < end) {
                /* Take from the right only when strictly less: keeps equal elements in order. */
                if (less(b + r * size, b + l * size)) {
                    memcpy(t + k++ * size, b + r++ * size, size);
                } else {
                    memcpy(t + k++ * size, b + l++ * size, size);
                }
            }
            if (l < mid) {
                memcpy(t + k * size, b + l * size, (mid - l) * size);
            }
            if (r < end) {
                memcpy(t + (k + mid - l) * size, b + r * size, (end - r) * size);
            }
        }
        memcpy(b, t, n * size);
        if (width > SIZE_MAX / 4) {
            break;
        }
    }
}

pc_status pc_cmp_stable_sort(void *base, size_t n, size_t size, pc_cmp_less_fn less, pc_error *err)
{
    unsigned char *t;
    if (n < 2 || size == 0 || size > 64) {
        return PC_OK;
    }
    if (n <= SMALL_SORT) {
        insertion(base, n, size, less);
        return PC_OK;
    }
    t = pc_malloc_array(n, size);
    if (t == NULL) {
        return pc_fail(err, PC_ERR_NOMEM, "compose: out of memory");
    }
    merge_sort(base, t, n, size, less);
    free(t);
    return PC_OK;
}
