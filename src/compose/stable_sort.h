/* Private to src/compose (C2): the stable sort Rust's slice::sort_by performs, for the colour
 * stops of gradient fills and the points of Curves. Lists of up to 20 elements are insertion
 * sorted exactly like Rust's small-sort (so even a comparator that is not a total order, such as
 * partial_cmp treating NaN as equal, gives the reference's order); longer ones are merge sorted
 * (identical for any total order). */
#ifndef PC_COMPOSE_STABLE_SORT_H
#define PC_COMPOSE_STABLE_SORT_H

#include <stdbool.h>
#include <stddef.h>

#include "base/status.h"

/* is_less(a, b): a sorts strictly before b. */
typedef bool (*pc_cmp_less_fn)(const void *a, const void *b);

/* Sorts n elements of size bytes (size <= 64) in place; PC_ERR_NOMEM when the merge buffer
 * can't be allocated (the input is then unchanged). */
pc_status pc_cmp_stable_sort(void *base, size_t n, size_t size, pc_cmp_less_fn less, pc_error *err);

#endif
