/* Status codes and error details shared by every PhotoCraft module.
 *
 * Convention (REWRITE_PLAN.md §4): a fallible function returns pc_status and takes an optional
 * last parameter `pc_error *err` (may be NULL). On failure it fills *err and returns the same
 * code; on success it leaves *err untouched. Out-params are written only on success. */
#ifndef PC_BASE_STATUS_H
#define PC_BASE_STATUS_H

#include <float.h>

/* Bit-exact parity with the reference needs float and double arithmetic done at their own
 * precision, as Rust always does. 32-bit x86 compilers default to the x87 FPU (FLT_EVAL_METHOD 2),
 * which keeps extra precision and gives different results. */
#if !defined(FLT_EVAL_METHOD) || FLT_EVAL_METHOD != 0
#error "PhotoCraft needs FLT_EVAL_METHOD == 0; on 32-bit x86 build with -msse2 -mfpmath=sse"
#endif

typedef enum pc_status {
    PC_OK = 0,
    PC_ERR_INVALID,     /* bad argument or parameter (caller error, or hostile params) */
    PC_ERR_NOMEM,       /* allocation failed */
    PC_ERR_IO,          /* operating-system I/O failure; pc_error.sys_errno says which */
    PC_ERR_FORMAT,      /* malformed or truncated input data */
    PC_ERR_UNSUPPORTED, /* valid input we don't handle (yet) */
    PC_ERR_LIMIT,       /* a size, count or arithmetic limit would be exceeded */
    PC_ERR_CANCELLED,   /* stopped by an interrupt or cancel request */
    PC_ERR_NOT_FOUND    /* a named thing (file, layer, command) does not exist */
} pc_status;

enum { PC_ERROR_MSG_MAX = 256 };

typedef struct pc_error {
    pc_status code;
    int sys_errno;               /* errno for PC_ERR_IO, else 0 */
    char msg[PC_ERROR_MSG_MAX];  /* NUL-terminated, truncated if longer */
} pc_error;

/* Fill err (if non-NULL) with code and a printf-style message. Use pc_fail instead. */
void pc_fail_set(pc_error *err, pc_status code, const char *fmt, ...);
/* As pc_fail_set, recording errnum and appending ": <strerror(errnum)>". */
void pc_fail_errno_set(pc_error *err, pc_status code, int errnum, const char *fmt, ...);

/* pc_fail(err, code, fmt, ...): fill err, evaluate to code. pc_fail_errno(err, code, errnum,
 * fmt, ...): the same with errno details. Macros so that callers (and static analysers, which
 * don't follow variadic calls) see the result is exactly `code`; `code` is evaluated twice, so
 * pass a constant or a plain variable. */
#define pc_fail(err, code, ...) (pc_fail_set((err), (code), __VA_ARGS__), (pc_status)(code))
#define pc_fail_errno(err, code, errnum, ...) (pc_fail_errno_set((err), (code), (errnum), __VA_ARGS__), (pc_status)(code))

/* Reset err to PC_OK with an empty message. err may be NULL. */
void pc_error_clear(pc_error *err);

/* Stable lower-case name for a status ("ok", "invalid", "nomem", ...). Never NULL. */
const char *pc_status_name(pc_status s);

#endif
