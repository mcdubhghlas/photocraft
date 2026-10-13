/* A port of Rust 1.95's slice::select_nth_unstable (core/src/slice/sort/select.rs, ipnsort's
 * introselect with a median-of-medians fallback, branchless cyclic Lomuto partition), for size_t
 * elements. The reference sums the elements a selection leaves in front of the pivot in their
 * order (Dehaze's airlight), so the order must be the reference's, not just the selected set. */
#include <stdbool.h>

#include "raw/internal.h"

#define INSERTION_SORT_THRESHOLD 16
#define PSEUDO_MEDIAN_REC_THRESHOLD 64

typedef struct sel {
    rx_less_fn less;
    void *ctx;
    bool flip; /* compare as !less(b, a) (the equal-to-pivot partition) */
} sel;

static bool lt(const sel *s, size_t a, size_t b)
{
    return s->flip ? !s->less(s->ctx, b, a) : s->less(s->ctx, a, b);
}

static void swap(size_t *v, size_t a, size_t b)
{
    size_t t = v[a];
    v[a] = v[b];
    v[b] = t;
}

static size_t min_index(const size_t *v, size_t len, const sel *s)
{
    size_t acc = 0;
    size_t i;
    for (i = 1; i < len; i++) {
        if (lt(s, v[i], v[acc])) {
            acc = i;
        }
    }
    return acc;
}

static size_t max_index(const size_t *v, size_t len, const sel *s)
{
    size_t acc = 0;
    size_t i;
    for (i = 1; i < len; i++) {
        if (lt(s, v[acc], v[i])) {
            acc = i;
        }
    }
    return acc;
}

static void insertion_sort(size_t *v, size_t len, const sel *s)
{
    size_t tail;
    for (tail = 1; tail < len; tail++) {
        size_t sift = tail - 1;
        size_t gap = tail;
        size_t tmp;
        if (!lt(s, v[tail], v[sift])) {
            continue;
        }
        tmp = v[tail];
        for (;;) {
            v[gap] = v[sift];
            gap = sift;
            if (sift == 0) {
                break;
            }
            sift--;
            if (!lt(s, tmp, v[sift])) {
                break;
            }
        }
        v[gap] = tmp;
    }
}

/* Branchless Lomuto partition with a cyclic permutation, of v (len) around pivot. */
static size_t lomuto_cyclic(size_t *v, size_t len, size_t pivot, const sel *s)
{
    size_t gap_value;
    size_t gap_pos = 0;
    size_t num_lt = 0;
    size_t right;
    if (len == 0) {
        return 0;
    }
    gap_value = v[0];
    for (right = 1; right <= len; right++) {
        size_t r = right < len ? v[right] : gap_value;
        bool is_lt = lt(s, r, pivot);
        v[gap_pos] = v[num_lt];
        v[num_lt] = r;
        gap_pos = right;
        num_lt += is_lt ? 1 : 0;
    }
    return num_lt;
}

static size_t partition(size_t *v, size_t len, size_t pivot, const sel *s)
{
    size_t num_lt;
    if (len == 0) {
        return 0;
    }
    swap(v, 0, pivot);
    num_lt = lomuto_cyclic(v + 1, len - 1, v[0], s);
    swap(v, 0, num_lt);
    return num_lt;
}

static size_t median3(const size_t *v, size_t a, size_t b, size_t c, const sel *s)
{
    bool x = lt(s, v[a], v[b]);
    bool y = lt(s, v[a], v[c]);
    if (x == y) {
        bool z = lt(s, v[b], v[c]);
        return (z != x) ? c : b;
    }
    return a;
}

static size_t median3_rec(const size_t *v, size_t a, size_t b, size_t c, size_t n, const sel *s)
{
    if (n * 8 >= PSEUDO_MEDIAN_REC_THRESHOLD) {
        size_t n8 = n / 8;
        a = median3_rec(v, a, a + n8 * 4, a + n8 * 7, n8, s);
        b = median3_rec(v, b, b + n8 * 4, b + n8 * 7, n8, s);
        c = median3_rec(v, c, c + n8 * 4, c + n8 * 7, n8, s);
    }
    return median3(v, a, b, c, s);
}

static size_t choose_pivot(const size_t *v, size_t len, const sel *s)
{
    size_t d8 = len / 8;
    if (len < PSEUDO_MEDIAN_REC_THRESHOLD) {
        return median3(v, 0, d8 * 4, d8 * 7, s);
    }
    return median3_rec(v, 0, d8 * 4, d8 * 7, d8, s);
}

static size_t median_idx(const size_t *v, size_t a, size_t b, size_t c, const sel *s)
{
    if (lt(s, v[c], v[a])) {
        size_t t = a;
        a = c;
        c = t;
    }
    if (lt(s, v[c], v[b])) {
        return c;
    }
    if (lt(s, v[b], v[a])) {
        return a;
    }
    return b;
}

static void ninther(size_t *v, const sel *s, size_t a, size_t b, size_t c, size_t d, size_t e, size_t f,
                    size_t g, size_t h, size_t i)
{
    b = median_idx(v, a, b, c, s);
    h = median_idx(v, g, h, i, s);
    if (lt(s, v[h], v[b])) {
        size_t t = b;
        b = h;
        h = t;
    }
    if (lt(s, v[f], v[d])) {
        size_t t = d;
        d = f;
        f = t;
    }
    if (lt(s, v[e], v[d])) {
        /* nothing */
    } else if (lt(s, v[f], v[e])) {
        d = f;
    } else {
        if (lt(s, v[e], v[b])) {
            swap(v, e, b);
        } else if (lt(s, v[h], v[e])) {
            swap(v, e, h);
        }
        return;
    }
    if (lt(s, v[d], v[b])) {
        d = b;
    } else if (lt(s, v[h], v[d])) {
        d = h;
    }
    swap(v, d, e);
}

static void median_of_medians(size_t *v, size_t len, size_t k, const sel *s);

static size_t median_of_ninthers(size_t *v, size_t len, const sel *s)
{
    size_t frac = len <= 1024 ? len / 12 : len <= (size_t)128 * 1024 ? len / 64 : len / 1024;
    size_t pivot = frac / 2;
    size_t lo = len / 2 - pivot;
    size_t hi = frac + lo;
    size_t gap = (len - 9 * frac) / 4;
    size_t a = lo - 4 * frac - gap;
    size_t b = hi + gap;
    size_t i;
    for (i = lo; i < hi; i++) {
        ninther(v, s, a, i - frac, b, a + 1, i, b + 1, a + 2, i + frac, b + 2);
        a += 3;
        b += 3;
    }
    median_of_medians(v + lo, frac, pivot, s);
    return partition(v, len, lo + pivot, s);
}

static void median_of_medians(size_t *v, size_t len, size_t k, const sel *s)
{
    for (;;) {
        size_t p;
        if (len <= INSERTION_SORT_THRESHOLD) {
            if (len >= 2) {
                insertion_sort(v, len, s);
            }
            return;
        }
        if (k == len - 1) {
            swap(v, max_index(v, len, s), k);
            return;
        }
        if (k == 0) {
            swap(v, min_index(v, len, s), k);
            return;
        }
        p = median_of_ninthers(v, len, s);
        if (p == k) {
            return;
        }
        if (p > k) {
            len = p;
        } else {
            v += p + 1;
            len -= p + 1;
            k -= p + 1;
        }
    }
}

static void select_loop(size_t *v, size_t len, size_t index, sel *s)
{
    int limit = 16;
    bool has_ancestor = false;
    size_t ancestor = 0;
    for (;;) {
        size_t pivot_pos;
        size_t mid;
        if (len <= INSERTION_SORT_THRESHOLD) {
            if (len >= 2) {
                insertion_sort(v, len, s);
            }
            return;
        }
        if (limit == 0) {
            median_of_medians(v, len, index, s);
            return;
        }
        limit--;
        pivot_pos = choose_pivot(v, len, s);
        /* A pivot equal to its predecessor is the smallest element: partition into equal and
         * greater instead (many duplicates). */
        if (has_ancestor && !lt(s, ancestor, v[pivot_pos])) {
            size_t num_lt;
            s->flip = true;
            num_lt = partition(v, len, pivot_pos, s);
            s->flip = false;
            mid = num_lt + 1;
            if (mid > index) {
                return;
            }
            v += mid;
            len -= mid;
            index -= mid;
            has_ancestor = false;
            continue;
        }
        mid = partition(v, len, pivot_pos, s);
        if (mid < index) {
            ancestor = v[mid];
            has_ancestor = true;
            v += mid + 1;
            len -= mid + 1;
            index -= mid + 1;
        } else if (mid > index) {
            len = mid;
        } else {
            return;
        }
    }
}

void rx_select_nth(size_t *v, size_t len, size_t index, rx_less_fn less, void *ctx)
{
    sel s;
    if (index >= len) {
        return;
    }
    s.less = less;
    s.ctx = ctx;
    s.flip = false;
    if (index == len - 1) {
        swap(v, max_index(v, len, &s), index);
    } else if (index == 0) {
        swap(v, min_index(v, len, &s), index);
    } else {
        select_loop(v, len, index, &s);
    }
}
