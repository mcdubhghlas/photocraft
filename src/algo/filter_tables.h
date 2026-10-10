/* Internal: the per-chunk filter registries (see filter.h). Each later algo chunk owns one table
 * file and lists its pc_algo_filter_def pointers there, NULL-terminated:
 *
 *   filters_l5.c  blurs, sharpen, noise, denoise (L5)
 *   filters_l6.c  distort, stylize, pixelate, render, other, gallery, video, Filter Gallery (L6)
 *   filters_l9.c  Relight (L9)
 *
 * A tag may appear in only one table. Not part of the public API. */
#ifndef PC_ALGO_FILTER_TABLES_H
#define PC_ALGO_FILTER_TABLES_H

#include "algo/filter.h"

extern const pc_algo_filter_def *const pc_algo_filters_l5[];
extern const pc_algo_filter_def *const pc_algo_filters_l6[];
extern const pc_algo_filter_def *const pc_algo_filters_l9[];

#endif
