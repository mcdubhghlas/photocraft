/* EngineError -> pc_status + message, reproducing the reference's Display text verbatim (tests
 * and the control protocol depend on it). See the mapping table in engine.h. */
#include <stdarg.h>
#include <stdio.h>

#include "engine_internal.h"

pc_status pc_engine_err_unknown_command(pc_error *err, const char *id)
{
    return pc_fail(err, PC_ERR_NOT_FOUND, "unknown command `%s`", id);
}

pc_status pc_engine_err_disabled(pc_error *err, const char *id, const char *why)
{
    return pc_fail(err, PC_ERR_INVALID, "command `%s` is not available right now: %s", id, why);
}

pc_status pc_engine_err_bad_params(pc_error *err, const char *cmd, const char *fmt, ...)
{
    char msg[PC_ERROR_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return pc_fail(err, PC_ERR_INVALID, "invalid parameters for `%s`: %s", cmd, msg);
}

pc_status pc_engine_err_no_document(pc_error *err)
{
    return pc_fail(err, PC_ERR_INVALID, "no active document");
}

pc_status pc_engine_err_no_layer(pc_error *err, pc_layer_id id)
{
    return pc_fail(err, PC_ERR_NOT_FOUND, "no such layer LayerId(%llu)", (unsigned long long)id);
}

pc_status pc_engine_err_other(pc_error *err, const char *fmt, ...)
{
    char msg[PC_ERROR_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    return pc_fail(err, PC_ERR_INVALID, "%s", msg);
}

pc_status pc_engine_err_cancelled(pc_error *err)
{
    return pc_fail(err, PC_ERR_CANCELLED, "cancelled");
}
