/* Exact nearest-colour search (port of crates/algo/src/quantize/nearest.rs). */
#include "algo/quantize_nearest.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/mem.h"
#include "base/rmath.h"

static float distance(const float a[3], const float b[3])
{
    float d0 = a[0] - b[0], d1 = a[1] - b[1], d2 = a[2] - b[2];
    return d0 * d0 + d1 * d1 + d2 * d2;
}

/* f32::total_cmp on the split axis, then the original index (a total order: the median and the
 * two halves are the same whatever selection algorithm finds them). */

static int total_key_cmp(float a, float b)
{
    int32_t ia, ib;
    memcpy(&ia, &a, 4);
    memcpy(&ib, &b, 4);
    ia ^= (int32_t)((uint32_t)(ia >> 31) >> 1);
    ib ^= (int32_t)((uint32_t)(ib >> 31) >> 1);
    return ia < ib ? -1 : ia > ib;
}

static void sort_entries(pc_algo_nearest_entry *e, size_t n, int axis)
{
    /* Insertion sort: partitions hold at most 256 palette entries in practice, and qsort's
     * comparator could not take the axis without global state. */
    size_t i, j;
    for (i = 1; i < n; i++) {
        pc_algo_nearest_entry x = e[i];
        for (j = i; j > 0; j--) {
            int c = total_key_cmp(e[j - 1].point[axis], x.point[axis]);
            if (c < 0 || (c == 0 && e[j - 1].index < x.index)) {
                break;
            }
            e[j] = e[j - 1];
        }
        e[j] = x;
    }
}

static void partition(pc_algo_nearest_entry *e, size_t n, int depth)
{
    float lo[3] = {3.40282347e+38f, 3.40282347e+38f, 3.40282347e+38f};
    float hi[3] = {-3.40282347e+38f, -3.40282347e+38f, -3.40282347e+38f};
    size_t i, mid;
    int a, axis = 0;
    /* Balanced splits: depth stays below log2(n) + 1; the guard only bounds a broken input. */
    if (n <= 8 || depth > 64) {
        return;
    }
    for (i = 0; i < n; i++) {
        for (a = 0; a < 3; a++) {
            lo[a] = pc_rs_minf(lo[a], e[i].point[a]);
            hi[a] = pc_rs_maxf(hi[a], e[i].point[a]);
        }
    }
    /* Iterator::max_by keeps the last of equal maxima. */
    for (a = 1; a < 3; a++) {
        if (total_key_cmp(hi[a] - lo[a], hi[axis] - lo[axis]) >= 0) {
            axis = a;
        }
    }
    sort_entries(e, n, axis);
    mid = n / 2;
    e[mid].axis = axis;
    partition(e, mid, depth + 1);
    partition(e + mid + 1, n - mid - 1, depth + 1);
}

pc_status pc_algo_nearest_init(pc_algo_nearest *lk, const float (*points)[3], size_t n)
{
    size_t i;
    bool finite = true;
    lk->entries = NULL;
    lk->n = 0;
    lk->tree = false;
    if (n == 0) {
        return PC_OK;
    }
    lk->entries = pc_malloc_array(n, sizeof *lk->entries);
    if (lk->entries == NULL) {
        return PC_ERR_NOMEM;
    }
    for (i = 0; i < n; i++) {
        memcpy(lk->entries[i].point, points[i], sizeof lk->entries[i].point);
        lk->entries[i].index = i;
        lk->entries[i].axis = 0;
        finite = finite && isfinite(points[i][0]) && isfinite(points[i][1]) && isfinite(points[i][2]);
    }
    lk->n = n;
    lk->tree = n > 64 && finite;
    if (lk->tree) {
        partition(lk->entries, n, 0);
    }
    return PC_OK;
}

void pc_algo_nearest_fini(pc_algo_nearest *lk)
{
    if (lk != NULL) {
        free(lk->entries);
        lk->entries = NULL;
        lk->n = 0;
    }
}

typedef struct best {
    float d;
    size_t i;
} best;

static void consider(const pc_algo_nearest_entry *e, const float p[3], best *b)
{
    float d = distance(e->point, p);
    /* Original palette order breaks ties, independent of the traversal order. */
    if (d < b->d || (d == b->d && e->index < b->i)) {
        b->d = d;
        b->i = e->index;
    }
}

static void search(const pc_algo_nearest_entry *e, size_t n, const float p[3], best *b, int depth)
{
    const pc_algo_nearest_entry *pivot, *near, *far;
    size_t mid, nnear, nfar;
    float delta;
    if (n <= 8 || depth > 64) {
        size_t i;
        for (i = 0; i < n; i++) {
            consider(&e[i], p, b);
        }
        return;
    }
    mid = n / 2;
    pivot = &e[mid];
    consider(pivot, p, b);
    delta = p[pivot->axis] - pivot->point[pivot->axis];
    if (delta <= 0.0f) {
        near = e, nnear = mid, far = e + mid + 1, nfar = n - mid - 1;
    } else {
        near = e + mid + 1, nnear = n - mid - 1, far = e, nfar = mid;
    }
    if (nnear > 0) {
        search(near, nnear, p, b, depth + 1);
    }
    /* The splitting plane is a lower bound; equality is searched too (a lower index may tie). */
    if (nfar > 0 && delta * delta <= b->d) {
        search(far, nfar, p, b, depth + 1);
    }
}

size_t pc_algo_nearest_find(const pc_algo_nearest *lk, const float point[3])
{
    best b = {3.40282347e+38f, 0};
    size_t i;
    if (!lk->tree) {
        /* Still in original order: strict comparison already breaks ties. */
        for (i = 0; i < lk->n; i++) {
            float d = distance(lk->entries[i].point, point);
            if (d < b.d) {
                b.d = d;
                b.i = lk->entries[i].index;
            }
        }
    } else if (isfinite(point[0]) && isfinite(point[1]) && isfinite(point[2])) {
        search(lk->entries, lk->n, point, &b, 0);
    }
    /* Non-finite queries against finite entries only see NaN / infinite distances: as in the
     * linear scan none beats f32::MAX and the answer stays index 0. */
    return b.i;
}
