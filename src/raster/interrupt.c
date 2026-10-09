#include "interrupt.h"

#include <math.h>

#include "base/num.h"

bool pc_interrupt_cancelled(const pc_interrupt *i)
{
    return i != NULL && i->cancel != NULL && i->cancel(i->ctx);
}

void pc_interrupt_progress(const pc_interrupt *i, float fraction)
{
    if (i != NULL && i->progress != NULL && isfinite(fraction)) {
        i->progress(i->ctx, pc_clampf(fraction, 0.0f, 1.0f));
    }
}

pc_status pc_interrupt_check(const pc_interrupt *i, pc_error *err)
{
    if (pc_interrupt_cancelled(i)) {
        return pc_fail(err, PC_ERR_CANCELLED, "cancelled");
    }
    return PC_OK;
}
