/* See crash_guard.h. */
#include "crash_guard.h"

#include <setjmp.h>
#include <signal.h>
#include <stddef.h>

#include "base/log.h"

/* The innermost active guard on this thread (NULL when none). siglongjmp returns the signal number. */
static _Thread_local sigjmp_buf *tls_jmp = NULL;

static const int FATAL_SIGNALS[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

static void crash_handler(int signum)
{
    if (tls_jmp != NULL) {
        siglongjmp(*tls_jmp, signum);
    }
    /* Not inside a guard: restore the default disposition and re-raise, so a genuine bug still
     * produces the normal crash (core dump), rather than looping in this handler. */
    (void)signal(signum, SIG_DFL);
    (void)raise(signum);
}

void pc_app_crash_install_hook(void)
{
    struct sigaction sa;
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_NODEFER so a fault that recurs while handling still reaches us (siglongjmp then restores
     * the mask via sigsetjmp's savemask). */
    sa.sa_flags = SA_NODEFER;
    for (size_t i = 0; i < sizeof FATAL_SIGNALS / sizeof FATAL_SIGNALS[0]; i++) {
        (void)sigaction(FATAL_SIGNALS[i], &sa, NULL);
    }
}

pc_status pc_app_crash_guard(const char *what, pc_status (*fn)(void *ctx, pc_error *err), void *ctx,
                             pc_error *err)
{
    if (fn == NULL) {
        return pc_fail(err, PC_ERR_INVALID, "crash guard: no function");
    }
    sigjmp_buf env;
    sigjmp_buf *prev = tls_jmp;
    tls_jmp = &env;
    int sig = sigsetjmp(env, 1);
    if (sig == 0) {
        pc_status rc = fn(ctx, err);
        tls_jmp = prev;
        return rc;
    }
    tls_jmp = prev;
    pc_log(PC_LOG_ERROR, "PhotoCraft internal error during %s (signal %d)",
           what ? what : "an operation", sig);
    return pc_fail(err, PC_ERR_INVALID,
                   "%s failed with an internal error (logged); your open documents are unchanged",
                   what ? what : "the operation");
}
