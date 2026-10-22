/* Last-resort crash guard (AGENTS.md "Never crash").
 *
 * C code must return errors, never crash. This is the safety net for a fault that escapes anyway: a
 * synchronous fatal signal (SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT) raised by the current thread
 * inside a guarded call is turned into an error the UI shows, so the open documents stay in place.
 * The desktop app wraps command dispatch and file import/export in pc_app_crash_guard. It is a
 * safety net, not a licence to crash.
 *
 * Behavioural port of apps/photocraft/src/crash_guard.rs (which uses catch_unwind; C has no
 * unwinding of a signal, so we use sigsetjmp/siglongjmp). An UNGUARDED fault restores the default
 * handler and re-raises, so a genuine bug still produces a core dump. */
#ifndef PC_APP_CRASH_GUARD_H
#define PC_APP_CRASH_GUARD_H

#include "base/status.h"

/* Install the signal handlers used by pc_app_crash_guard. Idempotent; call once at startup. */
void pc_app_crash_install_hook(void);

/* Run fn(ctx, err); a synchronous fatal signal inside it (or anything it calls on this thread)
 * becomes an error naming `what`, instead of crashing the process. Returns fn's status on a clean
 * run, or PC_ERR_INVALID with a logged "<what> failed ..." message on a caught signal. Nested calls
 * are supported. Not thread-safe across the SAME guard, but independent threads may each guard. */
pc_status pc_app_crash_guard(const char *what, pc_status (*fn)(void *ctx, pc_error *err), void *ctx,
                             pc_error *err);

#endif /* PC_APP_CRASH_GUARD_H */
