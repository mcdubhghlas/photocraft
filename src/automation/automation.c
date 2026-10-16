/* Shared automation error helpers (port of AutomationError Display prefixes). */
#include "automation/automation.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void prefixed(pc_error *err, pc_status code, const char *prefix, const char *fmt, va_list ap)
{
    if (!err) {
        return;
    }
    char body[PC_ERROR_MSG_MAX];
    (void)vsnprintf(body, sizeof body, fmt, ap);
    pc_fail_set(err, code, "%s%s", prefix, body);
}

void pc_auto_io_set(pc_error *err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    prefixed(err, PC_ERR_IO, "I/O: ", fmt, ap);
    va_end(ap);
}

void pc_auto_bridge_set(pc_error *err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    prefixed(err, PC_ERR_INVALID, "bridge: ", fmt, ap);
    va_end(ap);
}

void pc_auto_app_set(pc_error *err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    prefixed(err, PC_ERR_INVALID, "app: ", fmt, ap);
    va_end(ap);
}
