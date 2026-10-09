#include "status.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void set_msg(pc_error *err, const char *fmt, va_list ap)
{
    int n = vsnprintf(err->msg, sizeof err->msg, fmt, ap);
    if (n < 0) {
        err->msg[0] = '\0';
    }
}

void pc_fail_set(pc_error *err, pc_status code, const char *fmt, ...)
{
    if (err != NULL) {
        va_list ap;
        err->code = code;
        err->sys_errno = 0;
        va_start(ap, fmt);
        set_msg(err, fmt, ap);
        va_end(ap);
    }
}

void pc_fail_errno_set(pc_error *err, pc_status code, int errnum, const char *fmt, ...)
{
    if (err != NULL) {
        va_list ap;
        size_t len;
        err->code = code;
        err->sys_errno = errnum;
        va_start(ap, fmt);
        set_msg(err, fmt, ap);
        va_end(ap);
        /* strerror may share a static buffer between threads; copying it right away keeps the
         * window small, and the message is diagnostic only. */
        len = strlen(err->msg);
        if (len < sizeof err->msg) {
            (void)snprintf(err->msg + len, sizeof err->msg - len, ": %s", strerror(errnum));
        }
    }
}

void pc_error_clear(pc_error *err)
{
    if (err != NULL) {
        err->code = PC_OK;
        err->sys_errno = 0;
        err->msg[0] = '\0';
    }
}

const char *pc_status_name(pc_status s)
{
    switch (s) {
    case PC_OK: return "ok";
    case PC_ERR_INVALID: return "invalid";
    case PC_ERR_NOMEM: return "nomem";
    case PC_ERR_IO: return "io";
    case PC_ERR_FORMAT: return "format";
    case PC_ERR_UNSUPPORTED: return "unsupported";
    case PC_ERR_LIMIT: return "limit";
    case PC_ERR_CANCELLED: return "cancelled";
    case PC_ERR_NOT_FOUND: return "not_found";
    }
    return "unknown";
}
