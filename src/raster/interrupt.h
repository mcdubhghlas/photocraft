/* Cooperative cancellation and progress for long operations (port of
 * crates/raster/src/interrupt.rs).
 *
 * Heavy algorithms take a `const pc_interrupt *` and call pc_interrupt_cancelled (or
 * pc_interrupt_check) once per tile, row band or iteration, returning early when it is set. They
 * may report progress in [0, 1]. A NULL interrupt, or PC_INTERRUPT_NONE, never cancels and
 * ignores progress, so synchronous callers pass NULL.
 *
 * Thread safety: the callbacks may be called from several pool workers at once; they must be
 * thread-safe (typically an atomic flag load and an atomic store). */
#ifndef PC_RASTER_INTERRUPT_H
#define PC_RASTER_INTERRUPT_H

#include <stdbool.h>

#include "base/status.h"

typedef struct pc_interrupt {
    bool (*cancel)(void *ctx);                /* NULL: never cancelled */
    void (*progress)(void *ctx, float fraction); /* NULL: progress ignored */
    void *ctx;
} pc_interrupt;

#define PC_INTERRUPT_NONE ((pc_interrupt){NULL, NULL, NULL})

/* Has the operation been cancelled? Algorithms stop as soon as they see true. */
bool pc_interrupt_cancelled(const pc_interrupt *i);
/* Reports progress, clamped to [0, 1]; non-finite values are ignored. */
void pc_interrupt_progress(const pc_interrupt *i, float fraction);
/* PC_ERR_CANCELLED ("cancelled") once cancelled, else PC_OK. */
pc_status pc_interrupt_check(const pc_interrupt *i, pc_error *err);

#endif
