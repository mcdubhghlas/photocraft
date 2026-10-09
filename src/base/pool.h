/* pc_pool: a fixed set of worker threads running blocking parallel-for loops (replaces rayon).
 *
 * pc_pool_for splits [0, n) into ceil(n / grain) chunks of `grain` items (the last may be
 * shorter) and calls fn(ctx, chunk_index, begin, end) once per chunk, on the calling thread and
 * the workers. It returns when every chunk has run. Chunk boundaries depend only on n and grain,
 * never on the thread count, so code that writes per-chunk results (e.g. partial sums indexed by
 * chunk_index) and combines them in order afterwards is deterministic (REWRITE_PLAN.md D6).
 *
 * Errors: every chunk runs; the result is the status of the lowest-index chunk that failed (so
 * the same error is reported whatever the scheduling), or PC_OK.
 *
 * A NULL pool, a pool with no workers, a single chunk, or a call made from inside a chunk (nested
 * parallelism) all run the chunks serially on the calling thread, in index order.
 *
 * Thread safety: pc_pool_for may be called from several threads at once; calls are serialised.
 * fn must be safe to run concurrently on different chunks. */
#ifndef PC_BASE_POOL_H
#define PC_BASE_POOL_H

#include <stddef.h>

#include "status.h"

typedef struct pc_pool pc_pool;

typedef pc_status (*pc_pool_fn)(void *ctx, size_t chunk_index, size_t begin, size_t end);

/* Create a pool with `workers` extra threads (0 is valid: everything runs on the caller). */
pc_status pc_pool_new(size_t workers, pc_pool **out, pc_error *err);

/* Stop and join the workers, then free. pool may be NULL. Must not be called while a
 * pc_pool_for on this pool is running. */
void pc_pool_free(pc_pool *pool);

/* Number of threads that run chunks (workers + the caller). 1 for NULL. */
size_t pc_pool_threads(const pc_pool *pool);

/* Number of chunks pc_pool_for will use for (n, grain). grain 0 is treated as 1. */
size_t pc_pool_chunks(size_t n, size_t grain);

pc_status pc_pool_for(pc_pool *pool, size_t n, size_t grain, pc_pool_fn fn, void *ctx);

/* The process-wide pool, created on first use with (online CPUs - 1) workers, or
 * (PHOTOCRAFT_THREADS - 1) when that environment variable is a positive integer. Returns NULL
 * (callers then run serially) if it cannot be created. Never freed. */
pc_pool *pc_pool_global(void);

#endif
