/* Shared security primitives for the loopback JSON control transports (port of security.rs).
 *
 * A poll()-based line reader (REWRITE_PLAN.md D14, no async runtime) bounds every request to
 * PC_AUTO_MAX_REQUEST_BYTES; tokens are 256-bit hex bearer credentials compared without an early
 * exit; a connection limiter caps simultaneous TCP workers. */
#ifndef PC_AUTOMATION_SECURITY_H
#define PC_AUTOMATION_SECURITY_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <yyjson.h>

#include "base/buf.h"
#include "base/status.h"

/* The first request on every TCP connection must use this method. */
#define PC_AUTO_AUTH_METHOD "auth"
/* Maximum encoded JSON request line, including its newline. */
#define PC_AUTO_MAX_REQUEST_BYTES ((size_t)(1u << 20))
/* Maximum simultaneously serviced TCP connections per listener. */
#define PC_AUTO_MAX_CONNECTIONS 16
/* Maximum commands or method calls in one batch. */
#define PC_AUTO_MAX_BATCH_STEPS 256
/* Idle/read and write timeout for loopback TCP connections (seconds). */
#define PC_AUTO_IO_TIMEOUT_SECS 30

/* ---- bounded line reader (poll-based) ---- */

/* Result of reading one bounded JSON-lines frame. */
typedef enum pc_auto_line {
    PC_AUTO_LINE_EOF,          /* no bytes before end of stream */
    PC_AUTO_LINE_OK,           /* a complete UTF-8 line (keeps its trailing newline, if any) */
    PC_AUTO_LINE_TOO_LONG,     /* exceeded the byte limit; lossy bytes left in the buffer */
    PC_AUTO_LINE_INVALID_UTF8, /* within the limit but not UTF-8; the whole line is consumed */
    PC_AUTO_LINE_IOERR         /* read error or timeout (errno set); see *err */
} pc_auto_line;

typedef struct pc_auto_reader {
    int fd;
    bool use_timeout;      /* poll with PC_AUTO_IO_TIMEOUT_SECS (TCP); false = block (stdio) */
    uint8_t buf[65536];
    size_t pos, end;       /* valid bytes buf[pos..end) */
    bool saw_eof;
} pc_auto_reader;

void pc_auto_reader_init(pc_auto_reader *r, int fd, bool use_timeout);

/* Read one line into *line without buffering more than PC_AUTO_MAX_REQUEST_BYTES + 1 bytes.
 * On PC_AUTO_LINE_IOERR fills *err. */
pc_auto_line pc_auto_read_bounded_line(pc_auto_reader *r, pc_buf *line, pc_error *err);
/* Skip the rest of the current line (through its newline, or to EOF). */
pc_status pc_auto_discard_rest_of_line(pc_auto_reader *r, pc_error *err);

/* ---- full-write helper (handles EINTR, partial writes, and poll timeouts on TCP) ---- */
pc_status pc_auto_write_all(int fd, bool use_timeout, const void *data, size_t len, pc_error *err);

/* ---- tokens ---- */

/* Generate a 256-bit bearer token with the OS CSPRNG, as 64 lowercase hex chars into out[65]. */
pc_status pc_auto_generate_token(char out[65], pc_error *err);
/* Accept only the fixed-width hexadecimal representation emitted by generate_token. */
pc_status pc_auto_validate_token(const char *token, pc_error *err);
/* Compare fixed-width tokens without an early exit on a mismatching byte. */
bool pc_auto_token_matches(const char *expected, const char *supplied);
/* Resolve a server token into out[65]. With no supplied token or file, a fresh token is returned;
 * a missing token file is created atomically; an existing one is read and validated. */
pc_status pc_auto_server_token(const char *supplied, const char *token_file, char out[65], pc_error *err);
/* Resolve the token used by a client into out[65]. Clients never silently generate credentials. */
pc_status pc_auto_client_token(const char *supplied, const char *token_file, char out[65], pc_error *err);

/* ---- auth replies (build an owned reply doc; free with yyjson_mut_doc_free) ---- */

/* Validate the first TCP frame. *out_doc is the reply; *authed whether authentication succeeded. */
pc_status pc_auto_authentication_reply(const char *line, size_t len, const char *token,
                                       yyjson_mut_doc **out_doc, bool *authed, pc_error *err);
/* Answer a repeat `auth` frame on an already authenticated connection: *out_doc set (and returns
 * PC_OK) only when the frame is an `auth` method; otherwise *out_doc is NULL (dispatch as usual). */
pc_status pc_auto_repeat_auth_reply(const char *line, size_t len, yyjson_mut_doc **out_doc, pc_error *err);

/* ---- connection limiter ---- */

typedef struct pc_auto_limiter {
    atomic_size_t active;
    size_t max;
} pc_auto_limiter;

void pc_auto_limiter_init(pc_auto_limiter *l, size_t max);
/* Returns true and reserves a slot while below the maximum; release with pc_auto_limiter_release. */
bool pc_auto_limiter_acquire(pc_auto_limiter *l);
void pc_auto_limiter_release(pc_auto_limiter *l);

#endif /* PC_AUTOMATION_SECURITY_H */
