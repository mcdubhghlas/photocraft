/* Fallible working buffers for memory-intensive document operations (port of allocation.rs).
 *
 * These reserve the buffers destructive commands need (fills, clears, flips, transforms) without
 * the allocator's abort-on-OOM path, so an impossibly large document fails with an error and is
 * left unchanged rather than crashing. On failure *out is NULL and err carries the reference's
 * exact "not enough memory for <purpose> (<detail>); the document was not changed" message; on
 * success the caller owns the buffer (free()). `purpose` is a short borrowed description. */
#ifndef PC_ENGINE_ALLOCATION_H
#define PC_ENGINE_ALLOCATION_H

#include <stddef.h>

#include "base/status.h"
#include "raster/surface.h"

/* Reserve `count` * `elem` bytes (uninitialised); count==0 yields *out = NULL, PC_OK. */
pc_status pc_engine_alloc_capacity(size_t count, size_t elem, const char *purpose, void **out, pc_error *err);
/* Reserve and zero `count` * `elem` bytes (the zero value for f32/u8/ids all call sites use). */
pc_status pc_engine_alloc_filled(size_t count, size_t elem, const char *purpose, void **out, pc_error *err);
/* A boundary where a downstream encoder/decoder allocates internally: honours the test failpoint. */
pc_status pc_engine_alloc_checkpoint(const char *purpose, pc_error *err);
/* Read `area` of `s` into a freshly reserved float buffer of *len samples (w*h*channels). */
pc_status pc_engine_alloc_read_region(const pc_surface *s, pc_rect area, const char *purpose, float **out,
                                      size_t *len, pc_error *err);
/* Build the "not enough memory for <purpose> (<detail>); the document was not changed" error. */
pc_status pc_engine_alloc_error(pc_error *err, const char *purpose, const char *detail);

/* Test-only failpoint: arm so the next capacity()/checkpoint() reports OOM (consumed once, per
 * thread). Compiled into the library for tests; production code never arms it. */
void pc_engine_alloc_fail_next(void);

#endif /* PC_ENGINE_ALLOCATION_H */
