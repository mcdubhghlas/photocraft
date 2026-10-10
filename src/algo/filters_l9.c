/* Filter registry of chunk L9 (Relight): every pc_algo_filter_def this chunk ports, NULL-terminated
 * (see filter_tables.h). */
#include "algo/filter_tables.h"
#include "algo/relight.h"

#include <stddef.h>

const pc_algo_filter_def *const pc_algo_filters_l9[] = {&PC_ALGO_FILTER_RELIGHT, NULL};
